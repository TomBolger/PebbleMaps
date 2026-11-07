// Pebble Maps - place info: photos, rating, open hours, reviews (like Google Maps' place sheet)
#include "windows.h"
#include "comm.h"
#include "mapdata.h"

#if !PM_LOWMEM

#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
#define INFO_TOUCH 1
#else
#define INFO_TOUCH 0
#endif

#define MAX_INFO_ITEMS 12
enum { IT_SUMMARY = 1, IT_PHONE, IT_WEB, IT_HOURS, IT_REVIEW };

static Window *s_window;
static ScrollLayer *s_scroll;
static Layer *s_content;
static Layer *s_dots;
static int s_src, s_idx;
static bool s_loaded;
// The texts live on the heap while the window does (the watch's RAM is tight)
static struct { char name[64], meta[64], open[48], addr[96]; } *s_tx;
#define s_name (s_tx->name)
#define s_meta (s_tx->meta)
#define s_open (s_tx->open)
#define s_addr (s_tx->addr)
static int s_rating, s_rcount, s_photos, s_photo, s_cat, s_flags;
static ListItem *s_items;
static int s_nitems;
static int s_photo_seq = -1;
static GRect s_dir_btn;       // the Directions button (content coordinates)
static int s_photo_h;
#if INFO_TOUCH
static int16_t s_pan_base;
#endif

#define AL PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft)

static int header_h(GRect b) {
  return s_photos ? b.size.h * PBL_IF_ROUND_ELSE(50, 44) / 100 : PBL_IF_ROUND_ELSE(70, 46);
}


// Text block: measured always, drawn when ctx is set. Returns its height.
static int put(GContext *ctx, const char *t, GFont f, GColor c, int x, int y, int w) {
  if (!t || !t[0]) return 0;
  int h = graphics_text_layout_get_content_size(t, f, GRect(0, 0, w, 2000), GTextOverflowModeWordWrap, AL).h;
  if (ctx) {
    graphics_context_set_text_color(ctx, c);
    graphics_draw_text(ctx, t, f, GRect(x, y - 3, w, h + 6), GTextOverflowModeWordWrap, AL, NULL);
  }
  return h;
}

static void stars(GContext *ctx, int x, int y, int sz, int rating10) {
  int full = (rating10 + 5) / 10;
  for (int i = 0; i < 5; i++) {
    bool on = i < full;
    icon_draw(ctx, on ? ICON_STAR : ICON_STAR_OUTLINE, GPoint(x + i * (sz + 1) + sz / 2, y + sz / 2), sz,
              on ? PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack) : PBL_IF_COLOR_ELSE(C_DIVIDER, GColorBlack), C_BG);
  }
}

static int divider(GContext *ctx, int y, int w) {
  if (ctx) {
    graphics_context_set_stroke_color(ctx, C_DIVIDER);
    graphics_draw_line(ctx, GPoint(PBL_IF_ROUND_ELSE(30, 0), y + 4), GPoint(w - PBL_IF_ROUND_ELSE(30, 0), y + 4));
  }
  return 9;
}

// A row with a small icon on the left (centered text, no icon, on round screens)
static int icon_row(GContext *ctx, IconId icon, const char *t, GFont f, int pad, int y, int w) {
#ifdef PBL_ROUND
  return put(ctx, t, f, C_TEXT, pad, y, w - 2 * pad) + 4;
#else
  int h = put(ctx, t, f, C_TEXT, pad + 20, y, w - pad - 22);
  if (ctx) icon_draw(ctx, icon, GPoint(pad + 7, y + g_fonts.small_h / 2 + 1), 13, C_ACCENT, C_BG);
  return h + 4;
#endif
}

static void draw_header(GContext *ctx, int w, int h) {
  if (!s_photos) {
    ui_fill_rect(ctx, s_loaded ? poi_color(s_cat) : C_DIVIDER, GRect(0, 0, w, h), 0);
    if (s_loaded) icon_draw(ctx, map_cat_icon(s_cat), GPoint(w / 2, h / 2 + PBL_IF_ROUND_ELSE(6, 0)), 26, GColorWhite, poi_color(s_cat));
    return;
  }
  if (g_map.bmp && g_map.seq == s_photo_seq) {
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
    graphics_draw_bitmap_in_rect(ctx, g_map.bmp, GRect((w - g_map.w) / 2, (h - g_map.h) / 2, g_map.w, g_map.h));
  } else {
    ui_fill_rect(ctx, PBL_IF_COLOR_ELSE(C_DIVIDER, GColorWhite), GRect(0, 0, w, h), 0);
  }
  if (s_photos > 1) {
    char t[12];
    snprintf(t, sizeof(t), "%d/%d", s_photo + 1, s_photos);
    GRect r = GRect(PBL_IF_ROUND_ELSE(w / 2 - 18, w - 40), h - 21, 36, 17);
    ui_fill_rect(ctx, GColorBlack, r, 8);
    graphics_context_set_text_color(ctx, GColorWhite);
    ui_text(ctx, t, g_fonts.chip, GRect(r.origin.x, r.origin.y - 2, r.size.w, 16), GTextAlignmentCenter);
  }
}

// Select opens Directions once the photos are scrolled away (or there's nothing to flip through)
static bool dir_focused(void) {
  if (!s_loaded) return false;
  if (s_photos < 2) return true;
  return s_scroll && -scroll_layer_get_content_offset(s_scroll).y >= s_photo_h / 2;
}

// Lays out (and draws, with ctx) the whole page. Returns the content height.
static int layout(GContext *ctx, GRect b) {
  int w = b.size.w;
  int pad = PBL_IF_ROUND_ELSE(26, 6);
  int tw = w - 2 * pad;
  int hh = header_h(b);
  s_photo_h = hh;
  if (ctx) draw_header(ctx, w, hh);
  int y = hh + 5;
  y += put(ctx, s_name[0] ? s_name : "Loading...", g_fonts.body_b, C_TEXT, pad, y, tw) + 2;
  if (!s_loaded) return y + 50;

  if (s_rating > 0) {
    char r[8], c[16];
    snprintf(r, sizeof(r), "%d.%d", s_rating / 10, s_rating % 10);
    snprintf(c, sizeof(c), "(%d)", s_rcount);
    int sz = g_fonts.small_h - 4;
    if (ctx) {
      GFont f = g_fonts.small_b;
      int rw = graphics_text_layout_get_content_size(r, f, GRect(0, 0, 60, 30), GTextOverflowModeFill, GTextAlignmentLeft).w;
      int cw = graphics_text_layout_get_content_size(c, g_fonts.small, GRect(0, 0, 90, 30), GTextOverflowModeFill, GTextAlignmentLeft).w;
      int x = PBL_IF_ROUND_ELSE((w - (rw + 4 + 5 * (sz + 1) + 4 + cw)) / 2, pad);
      graphics_context_set_text_color(ctx, C_TEXT);
      ui_text(ctx, r, f, GRect(x, y - 3, rw + 2, 30), GTextAlignmentLeft);
      stars(ctx, x + rw + 4, y + (g_fonts.small_h - sz) / 2 + 1, sz, s_rating);
      graphics_context_set_text_color(ctx, C_SUBTEXT);
      ui_text(ctx, c, g_fonts.small, GRect(x + rw + 8 + 5 * (sz + 1), y - 3, cw + 2, 30), GTextAlignmentLeft);
    }
    y += g_fonts.small_h + 3;
  }
  y += put(ctx, s_meta, g_fonts.small, C_SUBTEXT, pad, y, tw);
  if (s_open[0]) {
    y += 1 + put(ctx, s_open, g_fonts.small_b, (s_flags & 2) ? C_GREEN_T : C_RED_T,
                 pad, y + 1, tw);
  }

  // Directions button
  y += 7;
  int bh = g_fonts.small_h + 12;
  int bw = PBL_IF_ROUND_ELSE(w * 6 / 10, tw);
  s_dir_btn = GRect((w - bw) / 2, y, bw, bh);
  if (ctx) {
    // highlighted (white ring) once you've scrolled past the photos: Select then opens it
    bool focus = dir_focused();
    ui_fill_rect(ctx, focus ? C_BLUE_DARK : C_BLUE, s_dir_btn, bh / 2);
    if (focus) {
      graphics_context_set_stroke_color(ctx, GColorWhite);
      graphics_context_set_stroke_width(ctx, 2);
      graphics_draw_round_rect(ctx, grect_inset(s_dir_btn, GEdgeInsets(2)), bh / 2 - 2);
      graphics_context_set_stroke_width(ctx, 1);
    }
    const char *label = "Directions";
    GFont f = g_fonts.small_b;
    int lw = graphics_text_layout_get_content_size(label, f, GRect(0, 0, bw, bh), GTextOverflowModeFill, GTextAlignmentLeft).w;
    int x0 = s_dir_btn.origin.x + (bw - lw - 18) / 2;
    icon_draw(ctx, ICON_DIRECTIONS, GPoint(x0 + 7, y + bh / 2), 14, GColorWhite, C_BLUE);
    graphics_context_set_text_color(ctx, GColorWhite);
    ui_text(ctx, label, f, GRect(x0 + 18, y + (bh - g_fonts.small_h) / 2 - 4, lw + 4, bh), GTextAlignmentLeft);
  }
  y += bh + 4;
  y += divider(ctx, y, w);

  // About, address, hours, phone, website - then reviews
  for (int i = 0; i < s_nitems; i++) {
    if (s_items[i].icon == IT_SUMMARY) y += put(ctx, s_items[i].sub, g_fonts.small, C_TEXT, pad, y, tw) + 6;
  }
  if (s_addr[0]) y += icon_row(ctx, ICON_PIN, s_addr, g_fonts.small, pad, y, w);
  for (int i = 0; i < s_nitems; i++) {
    if (s_items[i].icon != IT_HOURS) continue;
    y += icon_row(ctx, ICON_CLOCK, s_items[i].title, g_fonts.small_b, pad, y, w) - 3;
    y += put(ctx, s_items[i].sub, g_fonts.small, C_SUBTEXT, PBL_IF_ROUND_ELSE(pad, pad + 20), y,
             PBL_IF_ROUND_ELSE(tw, w - pad - 22)) + 5;
  }
  for (int i = 0; i < s_nitems; i++) {
    if (s_items[i].icon == IT_PHONE) y += icon_row(ctx, ICON_PHONE, s_items[i].title, g_fonts.small, pad, y, w);
    if (s_items[i].icon == IT_WEB) y += icon_row(ctx, ICON_GLOBE, s_items[i].title, g_fonts.small, pad, y, w);
  }
  bool heading = false;
  for (int i = 0; i < s_nitems; i++) {
    ListItem *it = &s_items[i];
    if (it->icon != IT_REVIEW) continue;
    if (!heading) {
      y += divider(ctx, y, w);
      y += put(ctx, "Reviews", g_fonts.body_b, C_TEXT, pad, y, tw) + 3;
      heading = true;
    }
    // title is "Name\nwhen"
    char who[40];
    const char *when = "";
    strncpy(who, it->title, sizeof(who) - 1);
    who[sizeof(who) - 1] = 0;
    char *nl = who;
    while (*nl && *nl != '\n') nl++;
    if (!*nl) nl = NULL;
    if (nl) { *nl = 0; when = it->title + (nl - who) + 1; }
    y += put(ctx, who, g_fonts.small_b, C_TEXT, pad, y, tw);
    int sz = g_fonts.small_h - 6;
    int sx = PBL_IF_ROUND_ELSE(w / 2 - (5 * (sz + 1)) / 2 - (when[0] ? 30 : 0), pad);
    if (ctx) stars(ctx, sx, y + 3, sz, it->extra * 10);
    if (when[0]) {
      if (ctx) {
        graphics_context_set_text_color(ctx, C_SUBTEXT);
        ui_text(ctx, when, g_fonts.small, GRect(sx + 5 * (sz + 1) + 4, y - 3, w - sx - 5 * (sz + 1) - 8, g_fonts.small_h + 4), GTextAlignmentLeft | TXT_ELLIPSIS);
      }
    }
    y += g_fonts.small_h + 1;
    y += put(ctx, it->sub, g_fonts.small, C_TEXT, pad, y, tw) + 8;
  }
  y += divider(ctx, y, w);
  y += put(ctx, "Photos, hours and reviews from Google", g_fonts.chip, C_SUBTEXT, pad, y, tw);
  return y + PBL_IF_ROUND_ELSE(50, 10);
}

static void content_update(Layer *layer, GContext *ctx) {
  layout(ctx, layer_get_bounds(window_get_root_layer(s_window)));
}

static void relayout(void) {
  GRect b = layer_get_bounds(window_get_root_layer(s_window));
  int h = layout(NULL, b);
  layer_set_frame(s_content, GRect(0, 0, b.size.w, h));
  scroll_layer_set_content_size(s_scroll, GSize(b.size.w, h));
  layer_mark_dirty(s_content);
}

static void request_photo(void) {
  GRect b = layer_get_bounds(window_get_root_layer(s_window));
  s_photo_seq = map_request(CMD_PHOTO, b.size.w, header_h(b), s_photo, -1);
}

static void handle(int cmd, DictionaryIterator *it, void *ctx) {
  switch (cmd) {
    case CMD_INFO_DATA:
      strncpy(s_name, tuple_str(it, MESSAGE_KEY_text), sizeof(s_name) - 1);
      strncpy(s_meta, tuple_str(it, MESSAGE_KEY_text2), sizeof(s_meta) - 1);
      strncpy(s_open, tuple_str(it, MESSAGE_KEY_text3), sizeof(s_open) - 1);
      strncpy(s_addr, tuple_str(it, MESSAGE_KEY_text4), sizeof(s_addr) - 1);
      s_rating = tuple_int(it, MESSAGE_KEY_num, 0);
      s_rcount = tuple_int(it, MESSAGE_KEY_num2, 0);
      s_flags = tuple_int(it, MESSAGE_KEY_flags, 0);
      s_photos = tuple_int(it, MESSAGE_KEY_idx, 0);
      s_cat = tuple_int(it, MESSAGE_KEY_mode, MAP_CAT_OTHER);
      free(s_items);
      s_items = alloc_list(tuple_str(it, MESSAGE_KEY_list), MAX_INFO_ITEMS, &s_nitems);
      s_loaded = true;
      s_photo = 0;
      dots_layer_set_running(s_dots, false);
      relayout();
      if (s_photos) request_photo();
      break;
    case CMD_TOAST:
      ui_toast(s_window, tuple_str(it, MESSAGE_KEY_text));
      break;
    case CMD_ERROR:
      dots_layer_set_running(s_dots, false);
      window_stack_remove(s_window, false);
      show_error(it);
      break;
  }
}

// --- Actions ---------------------------------------------------------------------
// Directions: the "Get there by" list (Back returns here)
static void go_directions(void) {
  if (!s_loaded) return;
  if (s_src != SRC_DEST) comm_cmd2(CMD_SELECT, s_idx, s_src);   // this place becomes the destination
  list_window_push(LW_MODES, s_src == SRC_DEST ? place_default_mode() : g_app.default_mode);
}

static void next_photo(void) {
  if (s_photos < 2) return;
  s_photo = (s_photo + 1) % s_photos;
  scroll_layer_set_content_offset(s_scroll, GPointZero, true);
  request_photo();
  layer_mark_dirty(s_content);
}

static void select_click(ClickRecognizerRef r, void *ctx) {
  if (dir_focused()) go_directions();
  else next_photo();
}

static void scrolled(ScrollLayer *sl, void *ctx) { layer_mark_dirty(s_content); }
static void select_long(ClickRecognizerRef r, void *ctx) { go_directions(); }

static void click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long, NULL);
}

#if INFO_TOUCH
// Drag to scroll; tap the photo for the next one, or the Directions button
static void pan_cb(const Recognizer *r, RecognizerEvent ev) {
  if (ev == RecognizerEvent_Started) {
    s_pan_base = scroll_layer_get_content_offset(s_scroll).y;
  } else if (ev == RecognizerEvent_Updated) {
    GPoint d = pan_recognizer_get_delta_since_start(r);
    scroll_layer_set_content_offset(s_scroll, GPoint(0, s_pan_base + d.y), false);
  }
}

// Taps come straight from the touch screen (finger down and up without moving)
static GPoint s_down;
static bool s_down_on, s_moved;

static void touch_cb(const TouchEvent *e, void *ctx) {
  if (e->type == TouchEvent_Touchdown) {
    s_down = GPoint(e->x, e->y);
    s_down_on = true;
    s_moved = false;
    return;
  }
  if (!s_down_on) return;
  if (abs(e->x - s_down.x) > 8 || abs(e->y - s_down.y) > 8) s_moved = true;
  if (e->type != TouchEvent_Liftoff) return;
  s_down_on = false;
  if (s_moved) return;
  GPoint c = GPoint(s_down.x, s_down.y - scroll_layer_get_content_offset(s_scroll).y);
  GRect hit = grect_inset(s_dir_btn, GEdgeInsets(-6));   // a little forgiving
  if (grect_contains_point(&hit, &c)) go_directions();
  else if (c.y < s_photo_h) next_photo();
}
#endif

// --- Lifecycle -------------------------------------------------------------------
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  s_scroll = scroll_layer_create(b);
  scroll_layer_set_shadow_hidden(s_scroll, true);
  scroll_layer_set_callbacks(s_scroll, (ScrollLayerCallbacks){ .click_config_provider = click_config,
                                                              .content_offset_changed_handler = scrolled });
  scroll_layer_set_click_config_onto_window(s_scroll, window);
  s_content = layer_create(GRect(0, 0, b.size.w, b.size.h));
  layer_set_update_proc(s_content, content_update);
  scroll_layer_add_child(s_scroll, s_content);
  layer_add_child(root, scroll_layer_get_layer(s_scroll));
  s_dots = dots_layer_create(GRect(0, b.size.h / 2 + 10, b.size.w, 30));
  layer_add_child(root, s_dots);
  dots_layer_set_running(s_dots, true);
  relayout();
#if INFO_TOUCH
  if (g_app.touch) {
  window_set_touch_bridge_disabled(window, true);
  window_attach_recognizer(window, pan_recognizer_create(pan_cb, NULL, PanAxis_Vertical));
  }
#endif
  OutMsg m;
  comm_msg_init(&m, CMD_INFO);
  m.mode = s_src;
  m.idx = s_idx;
  comm_send(&m);
}

static void window_appear(Window *window) {
  comm_set_handler(handle, NULL);
  map_set_observer(s_content);
#if INFO_TOUCH
  if (g_app.touch) touch_service_subscribe(touch_cb, NULL);
#endif
  if (s_loaded && s_photos && g_map.seq != s_photo_seq) request_photo();
}

static void window_disappear(Window *window) {
  comm_clear_handler(handle);
  map_set_observer(NULL);
  ui_toast_cancel();
  map_release();      // the photo isn't a map: screens underneath ask for theirs again
#if INFO_TOUCH
  if (g_app.touch_hw) touch_service_unsubscribe();
#endif
}

static void window_unload(Window *window) {
  dots_layer_destroy(s_dots);
  layer_destroy(s_content);
  scroll_layer_destroy(s_scroll);
  free(s_items);
  s_items = NULL;
  s_nitems = 0;
  free(s_tx);
  s_tx = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

void info_window_push(int src, int idx, const char *title) {
  if (s_window) window_stack_remove(s_window, false);
  // short on memory (Pebble Time, Time Round): drop the lists underneath first
  if (heap_bytes_free() < 4000) list_windows_close_all();
  s_src = src;
  s_idx = idx;
  s_loaded = false;
  s_photos = s_photo = s_rating = s_rcount = s_flags = 0;
  s_cat = MAP_CAT_OTHER;
  s_photo_seq = -1;
  s_tx = calloc(1, sizeof(*s_tx));
  s_window = s_tx ? window_create() : NULL;
  if (!s_window) {
    free(s_tx);
    s_tx = NULL;
    return;
  }
  strncpy(s_name, title ? title : "", sizeof(s_name) - 1);
  window_set_background_color(s_window, C_BG);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load, .appear = window_appear,
    .disappear = window_disappear, .unload = window_unload,
  });
  window_stack_push(s_window, true);
}
#endif  // !PM_LOWMEM
