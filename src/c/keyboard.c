// Pebble Maps - on-screen touch keyboard
//
// Key engine ported from Pebblegram 3.6's keyboard: keys laid out on a 20-unit
// grid, taps go to the nearest key in the row (no dead zones), keys commit on
// lift-off so you can slide to the right one, a big preview bubble follows your
// finger, delete repeats while held, double-tap shift for caps lock, and
// sentences start with a capital. Keys dock at the bottom; the search bar is
// at the top with suggestions dropping down under it.
#include "keyboard.h"
#include "ui.h"

#if KEYBOARD_AVAILABLE

#define KB_MAX_TEXT 64
#define KB_UNITS 20
#define KB_ROWS 4
#define KB_ROW_H PBL_IF_ROUND_ELSE(24, 22)
#define KB_PAD 3
#define KB_ROUND_BOTTOM_GAP 22
#define KB_REPEAT_DELAY_MS 450
#define KB_REPEAT_MS 90
#define KB_DOUBLE_TAP_MS 400
#define KB_PAUSE_MS 350

typedef enum { KeyNone, KeyChar, KeyShift, KeyDelete, KeySymbols, KeySpace, KeySend } KeyKind;

typedef struct {
  KeyKind kind;
  char ch;
  int8_t row;
  int8_t index;
} KeyHit;

static const char *const KB_ALPHA[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
static const char *const KB_SYMBOLS[3] = { "1234567890", "-/:;()$&@", ".,?!'\"+" };
static const char *const KB_SYMBOLS_2[3] = { "[]{}#%^*+=", "_\\|~<>`;:", ".,?!'\"@" };

static Window *s_window;
static Layer *s_layer;
static char s_text[KB_MAX_TEXT];
static const char *s_placeholder;
static const char *s_action;
static KeyboardHooks s_hooks;
static void *s_ctx;
static bool s_shift, s_symbols, s_caps_lock;
static uint32_t s_last_shift_ms;
static bool s_press_active;       // finger is on a key
static KeyHit s_pressed;
static AppTimer *s_repeat_timer;
static int s_sugg_pressed = -1;   // finger is on a suggestion
static AppTimer *s_pause_timer;
static ListItem *s_sugg;
static int s_nsugg;

static void text_changed(void);

static uint32_t now_ms(void) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  return (uint32_t)s * 1000 + ms;
}

static bool hit_equal(KeyHit a, KeyHit b) {
  return a.kind == b.kind && a.row == b.row && a.index == b.index;
}

static void dirty(void) {
  if (s_layer) layer_mark_dirty(s_layer);
}

// --- Layout -------------------------------------------------------------------------
// Search bar at the top (where the home screen's bar is)
static GRect bar_rect(GRect b) {
  int h = g_fonts.small_h + 12;
#ifdef PBL_ROUND
  return GRect(b.size.w / 2 - 80, 22, 160, h);
#else
  return GRect(6, 6, b.size.w - 12, h);
#endif
}

// Dark panel with the keys, docked at the bottom
static GRect panel_rect(GRect b) {
  int bottom = b.size.h - PBL_IF_ROUND_ELSE(KB_ROUND_BOTTOM_GAP, 0);
  int top = bottom - KB_ROWS * KB_ROW_H - 2 * KB_PAD;
  return GRect(0, top, b.size.w, b.size.h - top);
}

static int row_top(GRect b, int row) {
  return panel_rect(b).origin.y + KB_PAD + row * KB_ROW_H;
}

static int isqrt(int v) {
  if (v <= 0) return 0;
  int r = v, n = (r + 1) / 2;
  while (n < r) { r = n; n = (r + v / r) / 2; }
  return r;
}

// Usable width between two heights (on round screens: inside the circle)
static void span(GRect b, int y_top, int y_bottom, int *x, int *w) {
  *x = 2;
  *w = b.size.w - 4;
#ifdef PBL_ROUND
  int radius = b.size.w / 2, cy = b.size.h / 2;
  int t = y_top - cy, bt = y_bottom - cy;
  int dy = (t < 0 ? -t : t) > (bt < 0 ? -bt : bt) ? (t < 0 ? -t : t) : (bt < 0 ? -bt : bt);
  int half = isqrt(radius * radius - dy * dy) - 4;
  if (half < 24) half = 24;
  if (half > b.size.w / 2 - 2) half = b.size.w / 2 - 2;
  *x = b.size.w / 2 - half;
  *w = half * 2;
#endif
}

// Suggestions drop down under the search bar
static int sugg_row_h(void) { return 36; }

static GRect sugg_rect(GRect b) {
  GRect br = bar_rect(b), pr = panel_rect(b);
  int top = br.origin.y + br.size.h + 3;
  int rows = (pr.origin.y - 4 - top) / sugg_row_h();
  if (rows < 0) rows = 0;
#ifdef PBL_ROUND
  return GRect(br.origin.x - 14, top, br.size.w + 28, rows * sugg_row_h());
#else
  return GRect(br.origin.x, top, br.size.w, rows * sugg_row_h());
#endif
}

static int sugg_rows(GRect b) {
  int n = sugg_rect(b).size.h / sugg_row_h();
  return n < s_nsugg ? n : s_nsugg;
}

static GRect sugg_row(GRect b, int i) {
  GRect sr = sugg_rect(b);
  return GRect(sr.origin.x, sr.origin.y + i * sugg_row_h(), sr.size.w, sugg_row_h());
}

// --- Keys -------------------------------------------------------------------------------
//   row 0: 10 letters x 2 units
//   row 1:  9 letters x 2 units, centred
//   row 2: shift 3 + 7 letters x 2 + delete 3
//   row 3: 123 5 + space 10 + action 5
static const char *const *kb_rows(void) {
  if (!s_symbols) return KB_ALPHA;
  return s_shift ? KB_SYMBOLS_2 : KB_SYMBOLS;
}

static void key_units(int row, int index, int *start, int *count) {
  if (row == 0) { *start = index * 2; *count = 2; }
  else if (row == 1) { *start = 1 + index * 2; *count = 2; }
  else if (row == 2) {
    if (index == 0) { *start = 0; *count = 3; }
    else if (index == 8) { *start = 17; *count = 3; }
    else { *start = 3 + (index - 1) * 2; *count = 2; }
  } else {
    static const int starts[3] = { 0, 5, 15 }, counts[3] = { 5, 10, 5 };
    *start = starts[index];
    *count = counts[index];
  }
}

static int row_key_count(int row) { return row == 0 ? 10 : row == 1 ? 9 : row == 2 ? 9 : 3; }

static KeyHit key_at(int row, int index) {
  KeyHit hit = { KeyNone, 0, (int8_t)row, (int8_t)index };
  const char *const *rows = kb_rows();
  if (row == 0 || row == 1) { hit.kind = KeyChar; hit.ch = rows[row][index]; }
  else if (row == 2) {
    if (index == 0) hit.kind = KeyShift;
    else if (index == 8) hit.kind = KeyDelete;
    else { hit.kind = KeyChar; hit.ch = rows[2][index - 1]; }
  } else if (row == 3) {
    hit.kind = index == 0 ? KeySymbols : index == 1 ? KeySpace : KeySend;
  }
  return hit;
}

static GRect key_rect(GRect b, int row, int index) {
  int top = row_top(b, row), x, w, start, count;
  span(b, top, top + KB_ROW_H, &x, &w);
  key_units(row, index, &start, &count);
  int left = x + (w * start) / KB_UNITS, right = x + (w * (start + count)) / KB_UNITS;
  return GRect(left + 1, top + 1, right - left - 2 > 1 ? right - left - 2 : 1, KB_ROW_H - 2);
}

// Nearest key in the touched row
static KeyHit hit_test(GRect b, GPoint p) {
  KeyHit none = { KeyNone, 0, -1, -1 };
  int top0 = row_top(b, 0), bottom = row_top(b, KB_ROWS);
  if (p.y < top0 - 6 || p.y > bottom + 10) return none;
  int row = (p.y - top0) / KB_ROW_H;
  if (row < 0) row = 0;
  if (row > KB_ROWS - 1) row = KB_ROWS - 1;
  int top = row_top(b, row), x, w;
  span(b, top, top + KB_ROW_H, &x, &w);
  int unit = w > 0 ? ((p.x - x) * KB_UNITS) / w : 0;
  if (unit < 0) unit = 0;
  if (unit > KB_UNITS - 1) unit = KB_UNITS - 1;
  int count = row_key_count(row);
  for (int i = 0; i < count; i++) {
    int start, units;
    key_units(row, i, &start, &units);
    if (unit < start + units || i == count - 1) return key_at(row, i);
  }
  return none;
}

// --- Typing ---------------------------------------------------------------------------
static bool sentence_start(void) {
  size_t len = strlen(s_text);
  if (len == 0) return true;
  if (s_text[len - 1] != ' ') return false;
  while (len > 0 && s_text[len - 1] == ' ') len--;
  if (len == 0) return true;
  char last = s_text[len - 1];
  return last == '.' || last == '!' || last == '?';
}

static void update_auto_shift(void) {
  if (s_symbols || s_caps_lock) return;
  s_shift = sentence_start();
}

static void draft_changed(void) {
  update_auto_shift();
  text_changed();
  dirty();
}

static void append(char ch) {
  size_t n = strlen(s_text);
  if (n + 2 > sizeof(s_text)) { vibes_short_pulse(); return; }
  s_text[n] = ch;
  s_text[n + 1] = 0;
}

static void type_char(char ch) {
  if (!s_symbols && s_shift && ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
  append(ch);
  if (!s_caps_lock && !s_symbols) s_shift = false;
  draft_changed();
}

static void kb_delete(void) {
  size_t n = strlen(s_text);
  if (!n) return;
  s_text[n - 1] = 0;
  draft_changed();
}

static void cancel_repeat(void) {
  if (s_repeat_timer) { app_timer_cancel(s_repeat_timer); s_repeat_timer = NULL; }
}

static void repeat_cb(void *data) {
  s_repeat_timer = NULL;
  if (!s_press_active || s_pressed.kind != KeyDelete || !s_text[0]) return;
  kb_delete();
  s_repeat_timer = app_timer_register(KB_REPEAT_MS, repeat_cb, NULL);
}

static void reset_press(void) {
  cancel_repeat();
  s_press_active = false;
  s_pressed = (KeyHit){ KeyNone, 0, -1, -1 };
  s_sugg_pressed = -1;
}

static void clear_suggestions(void) {
  free(s_sugg);
  s_sugg = NULL;
  s_nsugg = 0;
}

void keyboard_set_suggestions(ListItem *items, int count) {
  clear_suggestions();
  if (!s_window) { free(items); return; }
  s_sugg = items;
  s_nsugg = items ? count : 0;
  dirty();
}

const char *keyboard_text(void) { return s_text; }

static void pause_cb(void *data) {
  s_pause_timer = NULL;
  if (s_hooks.changed) s_hooks.changed(s_text, s_ctx);
}

static void text_changed(void) {
  if (strlen(s_text) < 2) clear_suggestions();
  if (s_pause_timer) app_timer_reschedule(s_pause_timer, KB_PAUSE_MS);
  else s_pause_timer = app_timer_register(KB_PAUSE_MS, pause_cb, NULL);
}

static void finish(void) {
  // trim trailing spaces
  size_t n = strlen(s_text);
  while (n && s_text[n - 1] == ' ') s_text[--n] = 0;
  if (!s_text[0]) return;
  char text[KB_MAX_TEXT];
  strncpy(text, s_text, sizeof(text));
  void (*done)(const char *, void *) = s_hooks.done;
  void *ctx = s_ctx;
  window_stack_remove(s_window, false);
  if (done) done(text, ctx);
}

static void pick(int i) {
  if (i < 0 || i >= s_nsugg) return;
  char title[64];
  strncpy(title, s_sugg[i].title, sizeof(title) - 1);
  title[sizeof(title) - 1] = 0;
  void (*fn)(int, const char *, void *) = s_hooks.pick;
  void *ctx = s_ctx;
  window_stack_remove(s_window, false);
  if (fn) fn(i, title, ctx);
}

static void activate(KeyHit hit) {
  switch (hit.kind) {
    case KeyChar: type_char(hit.ch); return;
    case KeySpace: {
      size_t n = strlen(s_text);
      if (n && s_text[n - 1] != ' ') { append(' '); draft_changed(); }
      return;
    }
    case KeyShift: {
      uint32_t now = now_ms();
      if (s_symbols) s_shift = !s_shift;                 // second symbols page
      else if (s_caps_lock) { s_caps_lock = false; s_shift = false; }
      else if (s_shift && now - s_last_shift_ms < KB_DOUBLE_TAP_MS) s_caps_lock = true;  // double-tap = caps lock
      else s_shift = !s_shift;
      s_last_shift_ms = now;
      dirty();
      return;
    }
    case KeySymbols:
      s_symbols = !s_symbols;
      s_shift = false;
      s_caps_lock = false;
      update_auto_shift();
      dirty();
      return;
    case KeySend: finish(); return;
    default: return;
  }
}

// --- Drawing --------------------------------------------------------------------------
static void draw_key(GContext *ctx, GRect r, const char *label, bool pressed, bool special, bool accent) {
  GColor fill = pressed ? C_BLUE : accent ? C_BLUE : special ? C_KEY_SPECIAL : C_KEY;
  GColor text = (pressed || accent) ? GColorWhite : C_TEXT;
  if (pressed && accent) fill = GColorPictonBlue;
  ui_fill_rect(ctx, fill, r, 3);
  graphics_context_set_text_color(ctx, text);
  bool word = strlen(label) > 1;
  GFont f = fonts_get_system_font(word ? FONT_KEY_GOTHIC_14_BOLD : FONT_KEY_GOTHIC_18_BOLD);
  ui_text(ctx, label, f, GRect(r.origin.x, r.origin.y + (word ? 2 : -2), r.size.w, r.size.h), GTextAlignmentCenter);
}

// Shift: drawn up arrow, outline when off, filled when on, bar underneath for caps lock
static void draw_shift_arrow(GContext *ctx, GRect r, GColor color, bool filled, bool caps) {
  int cx = r.origin.x + r.size.w / 2;
  int size = r.size.w - 4 < r.size.h - 6 ? r.size.w - 4 : r.size.h - 6;
  int half = size / 2 > 3 ? size / 2 : 3;
  int top = r.origin.y + (r.size.h - size) / 2 - (caps ? 1 : 0);
  int mid = top + half;
  int stem = half / 2 > 2 ? half / 2 : 2;
  int bottom = top + size - (caps ? 2 : 0);
  GPoint pts[7] = { { cx, top }, { cx + half, mid }, { cx + stem, mid }, { cx + stem, bottom },
                    { cx - stem, bottom }, { cx - stem, mid }, { cx - half, mid } };
  GPath path = { .num_points = 7, .points = pts };
  graphics_context_set_fill_color(ctx, color);
  graphics_context_set_stroke_color(ctx, color);
  if (filled) gpath_draw_filled(ctx, &path);
  else gpath_draw_outline(ctx, &path);
  if (caps) graphics_fill_rect(ctx, GRect(cx - half, bottom + 2, half * 2 + 1, 2), 0, GCornerNone);
}

static void key_label(KeyHit key, char *buf, size_t len) {
  switch (key.kind) {
    case KeyChar:
      buf[0] = (!s_symbols && s_shift && key.ch >= 'a' && key.ch <= 'z') ? key.ch - 'a' + 'A' : key.ch;
      buf[1] = 0;
      return;
    case KeyShift: snprintf(buf, len, "%s", s_shift ? "123" : "#+="); return;
    case KeyDelete: snprintf(buf, len, "del"); return;
    case KeySymbols: snprintf(buf, len, "%s", s_symbols ? "ABC" : "123"); return;
    case KeySpace: snprintf(buf, len, "space"); return;
    case KeySend: snprintf(buf, len, "%s", s_action); return;
    default: buf[0] = 0; return;
  }
}

// Big preview of the key under your finger, drawn above it so the finger
// doesn't hide it; it follows the finger while sliding.
#define KB_PREVIEW_W 36
#define KB_PREVIEW_H 40
#define KB_PREVIEW_GAP 48
#define KB_PREVIEW_TAIL 12

static void draw_key_preview(GContext *ctx, GRect b, GRect kr, KeyHit key) {
  char label[2] = { key.ch, 0 };
  if (!s_symbols && s_shift && key.ch >= 'a' && key.ch <= 'z') label[0] = key.ch - 'a' + 'A';
  int key_cx = kr.origin.x + kr.size.w / 2;
  int head_top = kr.origin.y - KB_PREVIEW_GAP - KB_PREVIEW_H;
  if (head_top < 2) head_top = 2;
  int head_bottom = head_top + KB_PREVIEW_H;
  int sx, sw;
  span(b, head_top, head_bottom, &sx, &sw);
  int head_x = key_cx - KB_PREVIEW_W / 2;
  if (head_x > sx + sw - KB_PREVIEW_W) head_x = sx + sw - KB_PREVIEW_W;
  if (head_x < sx) head_x = sx;
  GRect head = GRect(head_x, head_top, KB_PREVIEW_W, KB_PREVIEW_H);
  int tail_cx = key_cx;
  if (tail_cx < head.origin.x + 8) tail_cx = head.origin.x + 8;
  if (tail_cx > head.origin.x + KB_PREVIEW_W - 8) tail_cx = head.origin.x + KB_PREVIEW_W - 8;
  GPoint tp[3] = { { tail_cx - 9, head_bottom - 2 }, { tail_cx + 9, head_bottom - 2 }, { tail_cx, head_bottom + KB_PREVIEW_TAIL } };
  GPath tail = { .num_points = 3, .points = tp };

  ui_fill_rect(ctx, GColorDarkGray, grect_inset(head, GEdgeInsets(-2)), 8);
  graphics_context_set_stroke_color(ctx, GColorDarkGray);
  graphics_context_set_stroke_width(ctx, 3);
  gpath_draw_outline(ctx, &tail);
  graphics_context_set_stroke_width(ctx, 1);
  ui_fill_rect(ctx, GColorWhite, head, 6);
  gpath_draw_filled(ctx, &tail);
  ui_fill_rect(ctx, C_BLUE, GRect(head.origin.x + 6, head.origin.y + head.size.h - 5, head.size.w - 12, 2), 0);
  graphics_context_set_text_color(ctx, GColorBlack);
  ui_text(ctx, label, fonts_get_system_font(FONT_KEY_GOTHIC_28_BOLD),
                     GRect(head.origin.x, head.origin.y + 1, head.size.w, head.size.h - 6), GTextAlignmentCenter);
}

static void draw_suggestions(GContext *ctx, GRect b) {
  int n = sugg_rows(b);
  if (n == 0) return;
  GRect sr = sugg_rect(b);
  draw_card(ctx, GRect(sr.origin.x, sr.origin.y, sr.size.w, n * sugg_row_h()), 6);
  GFont tf = fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD), sf = fonts_get_system_font(FONT_KEY_GOTHIC_14);
  for (int i = 0; i < n; i++) {
    GRect r = sugg_row(b, i);
    bool hl = s_sugg_pressed == i;
    if (hl) {
      ui_fill_rect(ctx, C_BLUE, grect_inset(r, GEdgeInsets(1)), 5);
    }
    int pad = 6, tx = r.origin.x + pad + 16;
    icon_draw(ctx, ICON_PIN, GPoint(r.origin.x + pad + 6, r.origin.y + r.size.h / 2), 14, hl ? GColorWhite : C_PIN, hl ? C_BLUE : C_BG);
    graphics_context_set_text_color(ctx, hl ? GColorWhite : C_TEXT);
    ui_text(ctx, s_sugg[i].title, tf, GRect(tx, r.origin.y - 1, r.origin.x + r.size.w - tx - pad, 22), GTextAlignmentLeft | TXT_ELLIPSIS);
    graphics_context_set_text_color(ctx, hl ? GColorWhite : C_HINT);
    ui_text(ctx, s_sugg[i].sub, sf, GRect(tx, r.origin.y + 17, r.origin.x + r.size.w - tx - pad, 18), GTextAlignmentLeft | TXT_ELLIPSIS);
    if (i < n - 1) {
      graphics_context_set_stroke_color(ctx, C_DIVIDER);
      graphics_draw_line(ctx, GPoint(tx, r.origin.y + r.size.h - 1), GPoint(r.origin.x + r.size.w - pad, r.origin.y + r.size.h - 1));
    }
  }
}

static void draw_bar(GContext *ctx, GRect b) {
  GRect br = bar_rect(b);
  draw_card(ctx, br, br.size.h / 2);
  graphics_context_set_stroke_color(ctx, C_BLUE);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_round_rect(ctx, grect_inset(br, GEdgeInsets(1)), br.size.h / 2 - 1);
  graphics_context_set_stroke_width(ctx, 1);
  icon_draw(ctx, ICON_SEARCH, GPoint(br.origin.x + 13, br.origin.y + br.size.h / 2), 13, C_TEXT, C_BG);
  GFont tf = g_fonts.small_b;
  GRect tr = GRect(br.origin.x + 24, br.origin.y + (br.size.h - g_fonts.small_h) / 2 - 3, br.size.w - 34, g_fonts.small_h + 4);
  if (s_text[0]) {
    const char *shown = s_text;   // keep the end (where you type) visible
    while (*shown && graphics_text_layout_get_content_size(shown, tf, GRect(0, 0, 1000, 40),
           GTextOverflowModeFill, GTextAlignmentLeft).w > tr.size.w - 4) shown++;
    graphics_context_set_text_color(ctx, C_TEXT);
    ui_text(ctx, shown, tf, tr, GTextAlignmentLeft);
    GSize ts = graphics_text_layout_get_content_size(shown, tf, tr, GTextOverflowModeFill, GTextAlignmentLeft);
    ui_fill_rect(ctx, C_BLUE, GRect(tr.origin.x + ts.w + 1, br.origin.y + 5, 2, br.size.h - 10), 0);
  } else {
    graphics_context_set_text_color(ctx, C_HINT);
    ui_text(ctx, s_placeholder, g_fonts.small, tr, GTextAlignmentLeft | TXT_ELLIPSIS);
  }
}

static void update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  ui_fill_rect(ctx, C_BG, b, 0);
  if (s_hooks.background) s_hooks.background(ctx, b, s_ctx);
  draw_bar(ctx, b);
  draw_suggestions(ctx, b);

  ui_fill_rect(ctx, C_KEY_PANEL, GRect(0, panel_rect(b).origin.y, b.size.w, b.size.h - panel_rect(b).origin.y), 0);
  char label[12];
  for (int row = 0; row < KB_ROWS; row++) {
    int count = row_key_count(row);
    for (int i = 0; i < count; i++) {
      KeyHit key = key_at(row, i);
      bool pressed = s_press_active && hit_equal(key, s_pressed);
      bool special = key.kind != KeyChar && key.kind != KeySpace;
      bool accent = (key.kind == KeySend && s_text[0]) ||
                    (key.kind == KeyShift && !s_symbols && (s_shift || s_caps_lock));
      GRect kr = key_rect(b, row, i);
      if (key.kind == KeySend && graphics_text_layout_get_content_size(s_action, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
              GRect(0, 0, 200, 20), GTextOverflowModeFill, GTextAlignmentLeft).w > kr.size.w - 4) {
        // label doesn't fit (round screens): show a magnifier instead
        draw_key(ctx, kr, "", pressed, special, accent);
        icon_draw(ctx, ICON_SEARCH, grect_center_point(&kr), 14, (pressed || accent) ? GColorWhite : C_TEXT, C_BLUE);
        continue;
      }
      if (key.kind == KeyShift && !s_symbols) {
        draw_key(ctx, kr, "", pressed, special, accent);
        draw_shift_arrow(ctx, kr, (pressed || accent) ? GColorWhite : C_TEXT, s_shift || s_caps_lock, s_caps_lock);
        continue;
      }
      key_label(key, label, sizeof(label));
      draw_key(ctx, kr, label, pressed, special, accent);
    }
  }
  if (s_press_active && s_pressed.kind == KeyChar) {
    draw_key_preview(ctx, b, key_rect(b, s_pressed.row, s_pressed.index), s_pressed);
  }
}

// --- Touch ----------------------------------------------------------------------------
static int sugg_at(GRect b, GPoint p) {
  for (int i = 0; i < sugg_rows(b); i++) {
    GRect r = sugg_row(b, i);
    if (grect_contains_point(&r, &p)) return i;
  }
  return -1;
}

static void touch_cb(const TouchEvent *e, void *ctx) {
  GRect b = layer_get_bounds(s_layer);
  GPoint p = GPoint(e->x, e->y);
  bool in_keys = p.y >= panel_rect(b).origin.y - 6;
  KeyHit hit = in_keys ? hit_test(b, p) : (KeyHit){ KeyNone, 0, -1, -1 };
  switch (e->type) {
    case TouchEvent_Touchdown:
      reset_press();
      if (hit.kind != KeyNone) {
        s_press_active = true;
        s_pressed = hit;
        if (hit.kind == KeyDelete) {
          kb_delete();   // delete fires right away and repeats while held
          s_repeat_timer = app_timer_register(KB_REPEAT_DELAY_MS, repeat_cb, NULL);
        }
      } else {
        s_sugg_pressed = sugg_at(b, p);
      }
      dirty();
      break;
    case TouchEvent_PositionUpdate:
      if (s_press_active && s_pressed.kind != KeyDelete && hit.kind != KeyNone && !hit_equal(hit, s_pressed)) {
        s_pressed = hit;   // slide to correct: the highlight follows your finger
        dirty();
      } else if (!s_press_active && s_sugg_pressed >= 0) {
        int i = sugg_at(b, p);
        if (i != s_sugg_pressed) { s_sugg_pressed = i; dirty(); }
      }
      break;
    case TouchEvent_Liftoff: {
      bool was_key = s_press_active;
      KeyHit pressed = s_pressed;
      int sugg = s_sugg_pressed;
      reset_press();
      if (was_key && pressed.kind != KeyDelete) {
        // released off the keys: use the last key the finger was on
        activate(hit.kind != KeyNone ? hit : pressed);
      } else if (!was_key && sugg >= 0 && sugg_at(b, p) == sugg) {
        pick(sugg);
        return;
      }
      dirty();
      break;
    }
  }
}

// --- Buttons and window -------------------------------------------------------------------
static void select_click(ClickRecognizerRef rec, void *ctx) { finish(); }
static void up_click(ClickRecognizerRef rec, void *ctx) { kb_delete(); }
static void down_click(ClickRecognizerRef rec, void *ctx) { activate((KeyHit){ KeySpace, 0, 3, 1 }); }

static void click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 150, up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
}

static void window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_layer = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_layer, update);
  layer_add_child(root, s_layer);
}

static void window_appear(Window *w) {
  app_touch_navigation_enable(false);   // swipes type, they don't press buttons
  touch_service_subscribe(touch_cb, NULL);
  if (s_hooks.appear) s_hooks.appear(s_ctx);
}

static void window_disappear(Window *w) {
  reset_press();
  touch_service_unsubscribe();
  app_touch_navigation_enable(true);
}

static void window_unload(Window *w) {
  if (s_pause_timer) app_timer_cancel(s_pause_timer);
  s_pause_timer = NULL;
  clear_suggestions();
  layer_destroy(s_layer);
  s_layer = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

void keyboard_window_push(const char *placeholder, const char *action_label, const KeyboardHooks *hooks, void *ctx) {
  if (s_window) return;
  s_text[0] = 0;
  s_symbols = false;
  s_caps_lock = false;
  s_shift = true;        // capital first letter
  reset_press();
#ifdef SHOT_TEST
  strcpy(s_text, "sta"); s_shift = false;  // TEST ONLY
  s_pause_timer = app_timer_register(KB_PAUSE_MS, pause_cb, NULL);  // TEST ONLY
#endif
  s_placeholder = placeholder;
  s_action = action_label;
  memset(&s_hooks, 0, sizeof(s_hooks));
  if (hooks) s_hooks = *hooks;
  s_ctx = ctx;
  s_window = window_create();
  window_set_background_color(s_window, C_BG);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load, .appear = window_appear,
    .disappear = window_disappear, .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

#else
void keyboard_window_push(const char *placeholder, const char *action_label, const KeyboardHooks *hooks, void *ctx) {}
void keyboard_set_suggestions(ListItem *items, int count) { free(items); }
const char *keyboard_text(void) { return ""; }
#endif
