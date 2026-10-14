// Pebble Maps - the watch's map picture
//
// The phone sends pictures cut from Google Static Maps. Each picture says
// where it sits in the world. When a new one arrives, the watch first moves
// the picture it already has (slide, zoom or turn) to line up with the new
// one, so the screen never goes blank; the phone then only needs to send the
// parts that are actually new (or the full sharp picture, center first).
#include "mapdata.h"
#include "comm.h"
#include "ui.h"

MapData g_map;
static int s_next_seq = 1;

// One picture buffer, reserved once and reused for every map.
static GBitmap *s_buf;
static GSize s_buf_size;
static int s_bpr;
static uint8_t *s_tmp;        // one row or column of pixels
#ifdef PBL_COLOR
static GColor s_palette[16];
#define BG_PX 0               // palette slot 0 is the map background
#define BG_BYTE 0x00
#else
#define BG_PX 1               // white
#define BG_BYTE 0xFF
#endif

static MapXformFn s_listener;

static uint32_t buf_bytes(int w, int h) {
#ifdef PBL_COLOR
  return (uint32_t)((w + 1) / 2) * h;
#else
  return (uint32_t)((w + 31) / 32) * 4 * h;
#endif
}

GSize map_buffer_size(void) { return s_buf_size; }

void map_reserve(void) {
  if (s_buf) return;
  // Screen size plus a margin all round when memory allows, so panning
  // shows real map straight away (Pebble Time 2, Round 2, Pebble 2).
  int sw = PBL_DISPLAY_WIDTH, sh = PBL_DISPLAY_HEIGHT;
  int free_now = (int)heap_bytes_free();
  int base = (int)buf_bytes(sw, sh);
  int keep = free_now > 60000 ? 30000 : 10000;   // memory left for screens and lists
  int avail = free_now - base - keep;
  int m = (sw > sh ? sw : sh) / 3;
  while (m > 0 && (int)buf_bytes(sw + 2 * m, sh + 2 * m) - base > avail) m -= 4;
  if (m < 0) m = 0;
  s_buf_size = GSize(sw + 2 * m, sh + 2 * m);
  APP_LOG(APP_LOG_LEVEL_INFO, "map buffer %dx%d (free before %d)", s_buf_size.w, s_buf_size.h, free_now);
#ifdef PBL_COLOR
  memset(s_palette, 0xFF, sizeof(s_palette));
  s_buf = gbitmap_create_blank_with_palette(s_buf_size, GBitmapFormat4BitPalette, s_palette, false);
#else
  s_buf = gbitmap_create_blank(s_buf_size, GBitmapFormat1Bit);
#endif
  if (s_buf) s_bpr = gbitmap_get_bytes_per_row(s_buf);
  s_tmp = malloc(s_buf_size.w > s_buf_size.h ? s_buf_size.w : s_buf_size.h);
}

void map_release(void) {
  g_map.bmp = NULL;
  g_map.complete = false;
  g_map.ref_valid = false;
  g_map.chunk_img = -1;
}

void map_set_observer(Layer *layer) { g_map.observer = layer; }
void map_set_xform_listener(MapXformFn fn) { s_listener = fn; }

static void notify(void) {
  if (g_map.observer) layer_mark_dirty(g_map.observer);
}

// --- Pixel helpers -----------------------------------------------------------------
static inline uint8_t *row_ptr(int y) { return gbitmap_get_data(s_buf) + y * s_bpr; }

#ifdef PBL_COLOR
static inline int pget(const uint8_t *r, int x) {
  uint8_t b = r[x >> 1];
  return (x & 1) ? (b & 0x0F) : (b >> 4);   // leftmost pixel in the high nibble
}
static inline void pset(uint8_t *r, int x, int v) {
  uint8_t *b = &r[x >> 1];
  if (x & 1) *b = (*b & 0xF0) | v;
  else *b = (*b & 0x0F) | (v << 4);
}
#else
static inline int pget(const uint8_t *r, int x) { return (r[x >> 3] >> (x & 7)) & 1; }
static inline void pset(uint8_t *r, int x, int v) {
  if (v) r[x >> 3] |= 1 << (x & 7);
  else r[x >> 3] &= ~(1 << (x & 7));
}
#endif

static void unpack_row(int y) {
  const uint8_t *r = row_ptr(y);
  for (int x = 0; x < g_map.w; x++) s_tmp[x] = pget(r, x);
}

static void clear_all(void) {
  for (int y = 0; y < g_map.h; y++) memset(row_ptr(y), BG_BYTE, s_bpr);
}

// rounded (a * n) / 65536 for signed values
static inline int fmul(int32_t a, int n) {
  int32_t p = a * n;
  return p >= 0 ? (p + 32768) >> 16 : -((-p + 32768) >> 16);
}

// --- Moving the picture in place ---------------------------------------------------
// Slide by (vx, vy) pixels
static void op_scroll(int vx, int vy) {
  int w = g_map.w, h = g_map.h;
  if (!vx && !vy) return;
  if (vx >= w || -vx >= w || vy >= h || -vy >= h) { clear_all(); return; }
  int y0 = vy > 0 ? h - 1 : 0, y1 = vy > 0 ? -1 : h, step = vy > 0 ? -1 : 1;
  for (int y = y0; y != y1; y += step) {
    uint8_t *dr = row_ptr(y);
    int sy = y - vy;
    if (sy < 0 || sy >= h) { memset(dr, BG_BYTE, s_bpr); continue; }
    unpack_row(sy);
    for (int x = 0; x < w; x++) {
      int sx = x - vx;
      pset(dr, x, (sx >= 0 && sx < w) ? s_tmp[sx] : BG_PX);
    }
  }
}

static void scale_row(int y, int sy, int k) {
  int w = g_map.w, cx = w / 2;
  uint8_t *dr = row_ptr(y);
  if (sy < 0 || sy >= g_map.h) { memset(dr, BG_BYTE, s_bpr); return; }
  unpack_row(sy);
  for (int x = 0; x < w; x++) {
    int sx = k > 0 ? cx + ((x - cx) >> k) : cx + ((x - cx) << -k);
    pset(dr, x, (sx >= 0 && sx < w) ? s_tmp[sx] : BG_PX);
  }
}

// Zoom by 2^k about the center (k > 0 blows up, k < 0 shrinks)
static void op_scale(int k) {
  int h = g_map.h, cy = h / 2;
  if (!k) return;
  if (k > 0) {
    // source rows are nearer the middle: work from the edges inward
    for (int y = 0; y < cy; y++) scale_row(y, cy + ((y - cy) >> k), k);
    for (int y = h - 1; y >= cy; y--) scale_row(y, cy + ((y - cy) >> k), k);
  } else {
    // source rows are further out: work from the middle outward
    for (int y = cy - 1; y >= 0; y--) scale_row(y, cy + ((y - cy) << -k), k);
    for (int y = cy; y < h; y++) scale_row(y, cy + ((y - cy) << -k), k);
  }
}

// Each row slides sideways by a * (y - cy)
static void op_hshear(int32_t a) {
  int w = g_map.w, h = g_map.h, cy = h / 2;
  for (int y = 0; y < h; y++) {
    int s = fmul(a, y - cy);
    if (!s) continue;
    uint8_t *dr = row_ptr(y);
    unpack_row(y);
    for (int x = 0; x < w; x++) {
      int sx = x - s;
      pset(dr, x, (sx >= 0 && sx < w) ? s_tmp[sx] : BG_PX);
    }
  }
}

// Each column slides up/down by b * (x - cx)
static void op_vshear(int32_t b) {
  int w = g_map.w, h = g_map.h, cx = w / 2;
  for (int x = 0; x < w; x++) {
    int s = fmul(b, x - cx);
    if (!s) continue;
    for (int y = 0; y < h; y++) s_tmp[y] = pget(row_ptr(y), x);
    for (int y = 0; y < h; y++) {
      int sy = y - s;
      pset(row_ptr(y), x, (sy >= 0 && sy < h) ? s_tmp[sy] : BG_PX);
    }
  }
}

// Half turn
static void op_flip(void) {
  int w = g_map.w, h = g_map.h;
  for (int ya = 0, yb = h - 1; ya <= yb; ya++, yb--) {
    uint8_t *ra = row_ptr(ya), *rb = row_ptr(yb);
    unpack_row(ya);
    if (ya == yb) {
      for (int x = 0; x < w; x++) pset(ra, x, s_tmp[w - 1 - x]);
      break;
    }
    for (int x = 0; x < w; x++) pset(ra, x, pget(rb, w - 1 - x));
    for (int x = 0; x < w; x++) pset(rb, x, s_tmp[w - 1 - x]);
  }
}

static int norm_deg(int d) {
  d %= 360;
  if (d > 180) d -= 360;
  if (d <= -180) d += 360;
  return d;
}

// Turn by rot degrees clockwise (three shears, done in place)
static void op_rotate(int rot) {
  rot = norm_deg(rot);
  if (rot > 90) { op_flip(); rot -= 180; }
  else if (rot < -90) { op_flip(); rot += 180; }
  if (!rot) return;
  int32_t ang = DEG_TO_TRIGANGLE(rot < 0 ? rot + 360 : rot);
  int32_t half = ang / 2;
  int32_t c2 = cos_lookup(half);
  if (c2 == 0) return;
  // 16.16 fixed point, 32-bit math only (|rot/2| <= 45 degrees, so nothing overflows)
  int32_t alpha = -((int32_t)sin_lookup(half) * 32768 / (c2 / 2));   // -tan(rot/2)
  int32_t beta = (int32_t)sin_lookup(ang);   // sin(rot)
  op_hshear(alpha);
  op_vshear(beta);
  op_hshear(alpha);
}

static void apply_xform(const MapXform *x) {
  if (x->k) op_scale(x->k);
  if (x->rot) op_rotate(x->rot);
  op_scroll(x->vx, x->vy);
}

GPoint map_xform_point(const MapXform *x, GPoint p) {
  int cx = g_map.w / 2, cy = g_map.h / 2;
  int32_t dx = p.x - cx, dy = p.y - cy;
  if (x->k > 0) { dx <<= x->k; dy <<= x->k; }
  else if (x->k < 0) { dx >>= -x->k; dy >>= -x->k; }
  if (x->rot) {
    int r = norm_deg(x->rot);
    int32_t a = DEG_TO_TRIGANGLE(r < 0 ? r + 360 : r);
    int32_t c = cos_lookup(a), s = sin_lookup(a);
    int32_t nx = (dx * c - dy * s) / TRIG_MAX_RATIO;
    int32_t ny = (dx * s + dy * c) / TRIG_MAX_RATIO;
    dx = nx;
    dy = ny;
  }
  return GPoint(cx + dx + x->vx, cy + dy + x->vy);
}

// --- The user's moves the phone hasn't drawn yet -------------------------------------
#define MAX_PANS 8
typedef struct { int16_t id, dx, dy; } Pan;
static Pan s_pans[MAX_PANS];
static int s_npans;
static int s_pan_id;
static int s_drag_x, s_drag_y;

static void recompute_shift(void) {
  int sx = 0, sy = 0;
  for (int i = 0; i < s_npans; i++) { sx -= s_pans[i].dx; sy -= s_pans[i].dy; }
  g_map.shift_x = sx;
  g_map.shift_y = sy;
}

void map_apply_ack(int ack) {
  int n = 0;
  for (int i = 0; i < s_npans; i++) {
    int behind = (ack - s_pans[i].id + 30000) % 30000;   // 0 .. ack is at or after this move
    if (behind < 15000) continue;                         // drawn already
    s_pans[n++] = s_pans[i];
  }
  s_npans = n;
  recompute_shift();
  notify();
}

void map_set_drag(int dx, int dy) {
  s_drag_x = dx;
  s_drag_y = dy;
  notify();
}

static void notify_xform(const MapXform *x) {
  if (s_listener) s_listener(x);
}

// Zoom the picture we have right away (blocky until the sharp one arrives)
static void prescale(int k) {
  if (!g_map.bmp || !g_map.ref_valid) return;
  MapXform x = { .k = k };
  apply_xform(&x);
  g_map.chunk_img = -1;   // pieces still on their way belong to the old zoom
  g_map.rzoom += k;
  if (k > 0) { g_map.rcx <<= k; g_map.rcy <<= k; }
  else { g_map.rcx >>= -k; g_map.rcy >>= -k; }
  for (int i = 0; i < s_npans; i++) {
    s_pans[i].dx = k > 0 ? s_pans[i].dx << k : s_pans[i].dx / (1 << -k);
    s_pans[i].dy = k > 0 ? s_pans[i].dy << k : s_pans[i].dy / (1 << -k);
  }
  recompute_shift();
  int fcx = g_map.fw / 2, fcy = g_map.fh / 2;
  for (int i = 0; i < g_map.n_markers; i++) {
    Marker *mk = &g_map.markers[i];
    int dx = mk->x - fcx, dy = mk->y - fcy;
    mk->x = fcx + (k > 0 ? dx << k : dx / (1 << -k));
    mk->y = fcy + (k > 0 ? dy << k : dy / (1 << -k));
  }
  notify_xform(&x);
}

void map_adjust(int action, int dx, int dy) {
  if (action == ADJ_PAN) {
    s_pan_id = s_pan_id % 30000 + 1;
    if (s_npans == MAX_PANS) {
      // fold the oldest move into the next one
      s_pans[1].dx += s_pans[0].dx;
      s_pans[1].dy += s_pans[0].dy;
      memmove(&s_pans[0], &s_pans[1], sizeof(Pan) * (MAX_PANS - 1));
      s_npans--;
    }
    s_pans[s_npans++] = (Pan){ .id = s_pan_id, .dx = dx, .dy = dy };
    recompute_shift();
  } else if (action == ADJ_ZOOM_IN) {
    prescale(1);
  } else if (action == ADJ_ZOOM_OUT) {
    prescale(-1);
  }
  OutMsg m;
  comm_msg_init(&m, CMD_MAP_ADJUST);
  m.seq = g_map.seq;
  m.idx = action;
  m.width = dx;
  m.height = dy;
  m.num = s_pan_id;
  comm_send(&m);
  notify();
}

// Faster Bluetooth while a picture is coming in; back to normal 1.5 s after the last piece
static AppTimer *s_fast;
static void fast_off(void *c) { s_fast = NULL; app_comm_set_sniff_interval(SNIFF_INTERVAL_NORMAL); }
static void comm_fast(void) {
  app_comm_set_sniff_interval(SNIFF_INTERVAL_REDUCED);
  if (!s_fast || !app_timer_reschedule(s_fast, 1500)) s_fast = app_timer_register(1500, fast_off, NULL);
}

int map_request(int cmd, int w, int h, int idx, int mode) {
  comm_fast();
  map_release();
  g_map.n_markers = 0;
  s_npans = 0;
  recompute_shift();
  g_map.fw = w;
  g_map.fh = h;
  g_map.seq = s_next_seq++;
  if (s_next_seq > 30000) s_next_seq = 1;
  OutMsg m;
  comm_msg_init(&m, cmd);
  m.seq = g_map.seq;
  m.width = w;
  m.height = h;
  m.idx = idx < 0 ? OUT_NONE : idx;
  m.mode = mode < 0 ? OUT_NONE : mode;
  comm_send(&m);
  notify();
  return g_map.seq;
}

// A new picture is starting: line up what we already have with it
static void begin(DictionaryIterator *it) {
  int w = tuple_int(it, MESSAGE_KEY_width, 0);
  int h = tuple_int(it, MESSAGE_KEY_height, 0);
  if (w <= 0 || h <= 0) return;
  map_reserve();
  if (!s_buf || !s_tmp) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "map buffer missing");
    return;
  }
  if (w > s_buf_size.w) w = s_buf_size.w;
  if (h > s_buf_size.h) h = s_buf_size.h;
#ifdef PBL_COLOR
  const Tuple *pt = dict_find(it, MESSAGE_KEY_palette);
  if (pt) {
    int n = pt->length > 16 ? 16 : pt->length;
    for (int i = 0; i < n; i++) s_palette[i].argb = pt->value->data[i];
  }
#endif
  int id = tuple_int(it, MESSAGE_KEY_img, 0);
  int ref = tuple_int(it, MESSAGE_KEY_ref, 0);
  int32_t ncx = tuple_int(it, MESSAGE_KEY_idx, 0), ncy = tuple_int(it, MESSAGE_KEY_mode, 0);
  int nz = tuple_int(it, MESSAGE_KEY_num, 0), nh = tuple_int(it, MESSAGE_KEY_num2, 0);

  bool keep = false;
  MapXform x = { 0 };
  if (g_map.bmp && g_map.ref_valid && w == g_map.w && h == g_map.h) {
    if (ref && ref == g_map.img_id && nz == g_map.rzoom && nh == g_map.rheading) {
      // the phone says exactly how far to slide
      x.vx = tuple_int(it, MESSAGE_KEY_vx, 0);
      x.vy = tuple_int(it, MESSAGE_KEY_vy, 0);
      keep = true;
    } else {
      int k = nz - g_map.rzoom;
      if (k >= -3 && k <= 3) {
        int32_t ocx = k >= 0 ? g_map.rcx << k : g_map.rcx >> -k;
        int32_t ocy = k >= 0 ? g_map.rcy << k : g_map.rcy >> -k;
        int32_t dx = ocx - ncx, dy = ocy - ncy;
        int lim = 2 * (w + h);
        if (dx > -lim && dx < lim && dy > -lim && dy < lim) {
          int32_t a = DEG_TO_TRIGANGLE(((nh % 360) + 360) % 360);
          int32_t c = cos_lookup(a), s = sin_lookup(a);
          x.k = k;
          x.rot = norm_deg(g_map.rheading - nh);
          x.vx = (int)((dx * c + dy * s) / TRIG_MAX_RATIO);
          x.vy = (int)((-dx * s + dy * c) / TRIG_MAX_RATIO);
          keep = true;
        }
      }
    }
  }
  gbitmap_set_bounds(s_buf, GRect(0, 0, w, h));
  g_map.bmp = s_buf;
  g_map.w = w;
  g_map.h = h;
  if (keep) {
    apply_xform(&x);
    g_map.img_id = id;
    g_map.rcx = ncx;
    g_map.rcy = ncy;
    g_map.rzoom = nz;
    g_map.rheading = nh;
    notify_xform(&x);
  } else {
    clear_all();
  }
  g_map.img_id = id;
  g_map.chunk_img = id;
  g_map.rcx = ncx;
  g_map.rcy = ncy;
  g_map.rzoom = nz;
  g_map.rheading = nh;
  g_map.ref_valid = true;
  if (dict_find(it, MESSAGE_KEY_ack)) map_apply_ack(tuple_int(it, MESSAGE_KEY_ack, 0));
}

void map_handle(int cmd, DictionaryIterator *it) {
  int seq = tuple_int(it, MESSAGE_KEY_seq, -1);
  if (seq != g_map.seq) return;  // another screen's picture
  if (cmd != CMD_MARKERS) comm_fast();
  if (cmd == CMD_MAP_BEGIN) {
    begin(it);
    notify();
  } else if (cmd == CMD_MAP_CHUNK) {
    // a block of rows: starting at row `offset`, byte `num`, `stride` bytes per row
    if (!g_map.bmp || tuple_int(it, MESSAGE_KEY_img, -2) != g_map.chunk_img) return;
    const Tuple *dt = dict_find(it, MESSAGE_KEY_data);
    if (!dt) return;
    int y = tuple_int(it, MESSAGE_KEY_offset, 0);
    int xb = tuple_int(it, MESSAGE_KEY_num, 0);
    int stride = tuple_int(it, MESSAGE_KEY_stride, s_bpr);
    if (stride <= 0 || xb < 0 || xb >= s_bpr || y < 0) return;
    int copy = stride < s_bpr - xb ? stride : s_bpr - xb;
    // rows packed with PackBits: n < 128 -> n+1 bytes as they are, n > 128 -> next byte 257-n times
    const uint8_t *s = dt->value->data, *e = s + dt->length;
    int col = 0;
    while (s < e && y < g_map.h) {
      int n = *s++;
      bool run = n > 128;
      int cnt = run ? 257 - n : n + 1;
      uint8_t v = 0;
      if (run) { if (s >= e) break; v = *s++; }
      while (cnt-- > 0 && y < g_map.h) {
        if (!run) { if (s >= e) break; v = *s++; }
        if (col < copy) row_ptr(y)[xb + col] = v;
        if (++col == stride) { col = 0; y++; }
      }
    }
    // redraw only once the screen's part of the picture is all here (no slow wipe)
    if (tuple_int(it, MESSAGE_KEY_flags, 1)) {
      g_map.complete = true;
      notify();
    }
  } else if (cmd == CMD_MARKERS) {
    const Tuple *dt = dict_find(it, MESSAGE_KEY_data);
    g_map.n_markers = 0;
#if !PM_LOWMEM
    // names for the places, in marker order, separated by 0x1E
    free(g_map.names);
    g_map.names = NULL;
    const char *nl = tuple_str(it, MESSAGE_KEY_list);
    if (nl[0]) {
      size_t len = strlen(nl);
      if (len > (PM_LOWMEM ? 220 : 600)) len = PM_LOWMEM ? 220 : 600;
      g_map.names = malloc(len + 1);
      if (g_map.names) {
        memcpy(g_map.names, nl, len);
        g_map.names[len] = 0;
      }
    }
#endif
    if (dt) {
      int n = dt->length / 6;
      if (n > MAX_MARKERS) n = MAX_MARKERS;
      const uint8_t *d = dt->value->data;
      for (int i = 0; i < n; i++) {
        Marker *mk = &g_map.markers[i];
        mk->x = (int16_t)(d[i * 6] | (d[i * 6 + 1] << 8));
        mk->y = (int16_t)(d[i * 6 + 2] | (d[i * 6 + 3] << 8));
        mk->kind = d[i * 6 + 4];
        mk->index = d[i * 6 + 5];
        mk->name = -1;
      }
#if !PM_LOWMEM
      if (g_map.names) {
        char *p = g_map.names;
        for (int i = 0; i < n && *p; i++) {
          if (g_map.markers[i].kind < MK_POI) continue;
          char *e = p;
          while (*e && *e != 0x1E) e++;
          bool last = !*e;
          *e = 0;
          if (*p) g_map.markers[i].name = p - g_map.names;
          if (last) break;
          p = e + 1;
        }
      }
#endif
      g_map.n_markers = n;
    }
    notify();
  }
}

const Marker *map_marker_for(int index) {
  for (int i = 0; i < g_map.n_markers; i++) {
    if (g_map.markers[i].kind == MK_PIN && g_map.markers[i].index == index) return &g_map.markers[i];
  }
  return NULL;
}

static int near_marker(GPoint p, GRect frame, int max_dist, bool pins) {
  int best = -1, best_d = max_dist * max_dist;
  for (int i = 0; i < g_map.n_markers; i++) {
    const Marker *mk = &g_map.markers[i];
    if (pins ? mk->kind != MK_PIN : mk->kind < MK_POI) continue;
    int dx = frame.origin.x + mk->x + g_map.shift_x - p.x;
    int dy = frame.origin.y + mk->y + g_map.shift_y - (pins ? 10 : 0) - p.y;  // pin body sits above the tip
    int d = dx * dx + dy * dy;
    if (d < best_d) {
      best_d = d;
      best = mk->index;
    }
  }
  return best;
}

int map_pin_near(GPoint p, GRect frame, int max_dist) { return near_marker(p, frame, max_dist, true); }
#if !PM_LOWMEM
// Name pills drawn last time (screen coordinates), so tapping a name works like tapping its dot
static GRect s_pills[17];
static uint8_t s_pill_idx[17];
static int s_npills;
#endif

int map_poi_near(GPoint p, GRect frame, int max_dist) {
#if !PM_LOWMEM
  for (int i = 0; i < s_npills; i++) {
    if (grect_contains_point(&s_pills[i], &p)) return s_pill_idx[i];
  }
#endif
  return near_marker(p, frame, max_dist, false);
}

void map_draw(GContext *ctx, GRect frame, int selected) {
  int sx = g_map.shift_x + s_drag_x, sy = g_map.shift_y + s_drag_y;
  if (g_map.bmp && g_map.complete) {   // a new picture shows once its screen part is all here
    GRect r = GRect(frame.origin.x + (frame.size.w - g_map.w) / 2 + sx,
                    frame.origin.y + (frame.size.h - g_map.h) / 2 + sy, g_map.w, g_map.h);
    // only the strips the picture doesn't cover get the "loading" look
    if (r.origin.x > frame.origin.x || r.origin.y > frame.origin.y ||
        r.origin.x + r.size.w < frame.origin.x + frame.size.w || r.origin.y + r.size.h < frame.origin.y + frame.size.h) {
      draw_map_placeholder(ctx, frame);
    }
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
    graphics_draw_bitmap_in_rect(ctx, g_map.bmp, r);
  } else {
    draw_map_placeholder(ctx, frame);
  }
  // places first (names as pills, skipping any that would cover another), then start / you,
  // then pins, the selected pin last (on top)
#if !PM_LOWMEM
  GRect used[17];
  int n_used = 0;
  // keep names off your own dot / arrow
  for (int i = 0; i < g_map.n_markers && !n_used; i++) {
    const Marker *mk = &g_map.markers[i];
    if (mk->kind == MK_ME || mk->kind == MK_ARROW) {
      used[n_used++] = GRect(frame.origin.x + mk->x + sx - 11, frame.origin.y + mk->y + sy - 11, 22, 22);
    }
  }
  int n_me = n_used;
  s_npills = 0;
  for (int i = 0; i < g_map.n_markers; i++) {
    const Marker *mk = &g_map.markers[i];
    if (mk->kind < MK_POI || mk->name < 0 || !g_map.names) continue;
    GPoint p = GPoint(frame.origin.x + mk->x + sx, frame.origin.y + mk->y + sy);
    const char *name = g_map.names + mk->name;
    GRect r = poi_label_rect(p, name, false);
    if (r.origin.x + r.size.w > frame.origin.x + frame.size.w) r = poi_label_rect(p, name, true);   // no room: name on the left
    bool clash = r.origin.x < frame.origin.x || r.origin.x + r.size.w > frame.origin.x + frame.size.w ||
                 r.origin.y < frame.origin.y;
    for (int j = 0; j < n_used && !clash; j++) {
      clash = r.origin.x < used[j].origin.x + used[j].size.w + 2 && used[j].origin.x < r.origin.x + r.size.w + 2 &&
              r.origin.y < used[j].origin.y + used[j].size.h + 1 && used[j].origin.y < r.origin.y + r.size.h + 1;
    }
    if (clash || n_used == 17) continue;
    used[n_used++] = r;
  }
#endif
  for (int pass = 0; pass < 4; pass++) {
    for (int i = 0; i < g_map.n_markers; i++) {
      const Marker *mk = &g_map.markers[i];
      GPoint p = GPoint(frame.origin.x + mk->x + sx, frame.origin.y + mk->y + sy);
      bool is_sel = mk->kind == MK_PIN && mk->index == selected;
      if (pass == 0 && mk->kind >= MK_POI) {
        bool labeled = false;
#if !PM_LOWMEM
        if (mk->name >= 0 && g_map.names) {
          GRect r = GRect(0, 0, 0, 0);
          for (int j = n_me; j < n_used && !labeled; j++) {
            // this place's pill: its dot sits at one end
            labeled = used[j].origin.y == p.y - 7 && (used[j].origin.x == p.x - (g_fonts.level >= 1 ? 5 : 4) - 3 ||
                                                     used[j].origin.x + used[j].size.w == p.x + (g_fonts.level >= 1 ? 5 : 4) + 3);
            if (labeled) r = used[j];
          }
          if (labeled) {
            draw_poi_label(ctx, p, mk->kind - MK_POI, g_map.names + mk->name, r);
            if (s_npills < 17) {
              s_pills[s_npills] = GRect(r.origin.x - 2, r.origin.y - 3, r.size.w + 4, r.size.h + 6);
              s_pill_idx[s_npills++] = mk->index;
            }
          }
        }
#endif
        // with names on, every place shown has its name: no room for the name, no dot
        if (!labeled && !(mk->name >= 0 && g_map.names)) draw_poi(ctx, p, mk->kind - MK_POI);
      } else if (pass == 1 && (mk->kind == MK_START || mk->kind == MK_ME || mk->kind == MK_ARROW)) {
        if (mk->kind == MK_ME) draw_me_dot(ctx, p, 5);
        else if (mk->kind == MK_ARROW) draw_puck(ctx, p, g_fonts.level >= 1 ? 7 : 6, mk->index * 2);
        else {
          graphics_context_set_fill_color(ctx, GColorWhite);
          graphics_fill_circle(ctx, p, 6);
          graphics_context_set_stroke_color(ctx, GColorBlack);
          graphics_context_set_stroke_width(ctx, 2);
          graphics_draw_circle(ctx, p, 5);
          graphics_context_set_stroke_width(ctx, 1);
        }
      } else if (pass == 2 && ((mk->kind == MK_PIN && !is_sel) || mk->kind == MK_DEST)) {
        draw_pin(ctx, p, mk->kind == MK_DEST ? 7 : 5, mk->kind == MK_DEST);
      } else if (pass == 3 && is_sel) {
        draw_pin(ctx, p, 6, true);
      }
    }
  }
}
