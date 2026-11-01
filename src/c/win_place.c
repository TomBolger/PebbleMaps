// Pebble Maps - place card: map preview, name, address, save, directions
#include "windows.h"
#include "comm.h"
#include "mapdata.h"
#include "mapbar.h"

static MapBar s_bar;
static MapTouch s_touch;

static Window *s_window;
static Layer *s_canvas;
static Layer *s_dots;
static int s_src, s_idx;
static int s_suggest_request;
static char s_name[64];
static char s_type[48];
static char s_addr[96];
static char s_dist[32];
static bool s_fav;
static bool s_loaded;
static int s_mode = MODE_DRIVE;

int place_default_mode(void) { return s_mode; }

static int map_h(GRect b) { return b.size.h * PBL_IF_ROUND_ELSE(45, 42) / 100; }

static void request_map(void) {
  GRect b = layer_get_bounds(window_get_root_layer(s_window));
  map_request(CMD_PLACE_MAP, b.size.w - STRIP_W, map_h(b), -1, -1);
}

static void handle(int cmd, DictionaryIterator *it, void *ctx) {
  switch (cmd) {
    case CMD_PLACE:
      strncpy(s_name, tuple_str(it, MESSAGE_KEY_text), sizeof(s_name) - 1);
      strncpy(s_type, tuple_str(it, MESSAGE_KEY_text2), sizeof(s_type) - 1);
      strncpy(s_addr, tuple_str(it, MESSAGE_KEY_text3), sizeof(s_addr) - 1);
      strncpy(s_dist, tuple_str(it, MESSAGE_KEY_text4), sizeof(s_dist) - 1);
      s_fav = tuple_int(it, MESSAGE_KEY_flags, 0) & 1;
      s_mode = tuple_int(it, MESSAGE_KEY_mode, g_app.default_mode);
      if (!s_loaded) request_map();
      s_loaded = true;
      dots_layer_set_running(s_dots, false);
      layer_mark_dirty(s_canvas);
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

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int mh = map_h(b);
  int w = b.size.w - STRIP_W;
  map_draw(ctx, GRect(0, 0, w, mh), -1);
  ui_fill_rect(ctx, C_BG, GRect(0, mh, w, b.size.h - mh), 0);
  graphics_context_set_stroke_color(ctx, C_DIVIDER);
  graphics_draw_line(ctx, GPoint(0, mh), GPoint(w, mh));

  int pad = PBL_IF_ROUND_ELSE(18, 5);
  int x = pad, tw = w - pad - 3;
  int y = mh + 1;
  GTextAlignment al = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);
  graphics_context_set_text_color(ctx, C_TEXT);
  GFont nf = g_fonts.body_b;
  int nmax = g_fonts.body_h * 2 + 8;
  GSize ns = graphics_text_layout_get_content_size(s_name, nf, GRect(0, 0, tw, nmax),
                                                   GTextOverflowModeTrailingEllipsis, al);
  ui_text(ctx, s_name, nf, GRect(x, y - 3, tw, nmax), al | TXT_ELLIPSIS);
  y += ns.h;
  if (s_type[0] || s_dist[0]) {
    char line[96];
    if (s_type[0] && s_dist[0]) snprintf(line, sizeof(line), "%s · %s", s_type, s_dist);
    else snprintf(line, sizeof(line), "%s%s", s_type, s_dist);
    graphics_context_set_text_color(ctx, PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack));
    ui_text(ctx, line, g_fonts.small_b, GRect(x, y - 2, tw, g_fonts.small_h + 4), al | TXT_ELLIPSIS);
    y += g_fonts.small_h;
  }
  graphics_context_set_text_color(ctx, C_SUBTEXT);
  ui_text(ctx, s_addr, g_fonts.small, GRect(x, y - 2, tw, b.size.h - y), al | TXT_ELLIPSIS);

  // hint badge: map can be moved (hold Select)
  if (!s_bar.open) {
    GRect bd = GRect(PBL_IF_ROUND_ELSE(w / 2 - 10, 3), mh - 22, 20, 19);
    draw_card(ctx, bd, 5);
    icon_draw(ctx, ICON_MOVE, GPoint(bd.origin.x + 10, bd.origin.y + 9), 13, C_TEXT, C_BG);
  }
  if (s_bar.open) {
    mapbar_draw(&s_bar, ctx, b);
    return;
  }
  // strip: star / directions / info
  StripIcons ic = { .up = s_fav ? ICON_STAR : ICON_STAR_OUTLINE, .select = ICON_NONE,
                    .down = ICON_INFO, .up_color = s_fav ? C_STAR : C_STAR_EDGE, .down_color = C_BLUE };
  draw_action_strip(ctx, b, &ic);
  int cx = PBL_IF_ROUND_ELSE(b.size.w - STRIP_W / 2 - 4, b.size.w - STRIP_W / 2);
  icon_draw(ctx, ICON_DIRECTIONS, GPoint(cx, b.size.h / 2), STRIP_W - 4, C_BLUE, GColorWhite);
}

static void bar_cb(MapBarEvent ev, void *ctx) {
  if (ev == MB_EV_EXTRA_UP) map_adjust(ADJ_RESET, 0, 0);
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void up_click(ClickRecognizerRef r, void *ctx) {
  if (mapbar_button(&s_bar, BUTTON_ID_UP)) return;
  if (!s_loaded) return;
  comm_cmd(CMD_FAV_TOGGLE);
  s_fav = !s_fav;
  layer_mark_dirty(s_canvas);
}
static void select_click(ClickRecognizerRef r, void *ctx) {
  if (mapbar_button(&s_bar, BUTTON_ID_SELECT)) return;
  if (!s_loaded) return;
  list_window_push(LW_MODES, s_mode);
}
static void down_click(ClickRecognizerRef r, void *ctx) {
  if (mapbar_button(&s_bar, BUTTON_ID_DOWN)) return;
  if (!s_loaded) return;
  info_window_push(SRC_DEST, 0, s_name);
}

static void back_click(ClickRecognizerRef r, void *ctx) {
  if (mapbar_button(&s_bar, BUTTON_ID_BACK)) return;
  window_stack_remove(s_window, true);
}
static void select_long(ClickRecognizerRef r, void *ctx) {
  if (!s_loaded) return;
  if (s_bar.open) mapbar_close(&s_bar);
  else mapbar_open(&s_bar);
}

static void click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long, NULL);
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
}

static void map_tap(GPoint p, void *ctx) {
  GRect b = layer_get_bounds(s_canvas);
  if (!s_bar.open && p.y > map_h(b) - 24 && p.x < 30) mapbar_open(&s_bar);
}


static void touch_cb(const TouchEvent *e, void *ctx) {
  if (maptouch_event(&s_touch, e)) return;
  if (e->type != TouchEvent_Liftoff || e->non_navigational) return;
  GRect b = layer_get_bounds(s_canvas);
  if (e->x < b.size.w - STRIP_W) {
    // the name card: more info
    if (e->y > map_h(b) && s_loaded && !s_bar.open) info_window_push(SRC_DEST, 0, s_name);
    return;
  }
  if (e->y < b.size.h / 3) up_click(NULL, NULL);
  else if (e->y > b.size.h * 2 / 3) down_click(NULL, NULL);
  else select_click(NULL, NULL);
}

static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  s_canvas = layer_create(b);
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
  s_dots = dots_layer_create(GRect(0, map_h(b) + 10, b.size.w - STRIP_W, 30));
  layer_add_child(root, s_dots);
  dots_layer_set_running(s_dots, true);
  mapbar_init(&s_bar, s_canvas, GSize(b.size.w - STRIP_W, map_h(b)), ICON_NONE, ICON_NONE, bar_cb, NULL);
  maptouch_init(&s_touch, GRect(0, 0, b.size.w - STRIP_W, map_h(b)), map_tap, NULL, &s_bar);
  OutMsg m;
  comm_msg_init(&m, CMD_SELECT);
  m.idx = s_idx;
  m.mode = s_src;
  if (s_src == SRC_SUGGEST) m.num = s_suggest_request;
  comm_send(&m);
}

static void window_appear(Window *window) {
  comm_set_handler(handle, NULL);
  map_set_observer(s_canvas);
  if (TOUCH_HW && g_app.touch) touch_service_subscribe(touch_cb, NULL);
  if (s_loaded) request_map();  // coming back from route/modes: redraw map (cached on phone)
}

static void window_disappear(Window *window) {
  comm_clear_handler(handle);
  comm_cmd(CMD_CANCEL);
  map_set_observer(NULL);
  ui_toast_cancel();
  mapbar_deinit(&s_bar);
  maptouch_deinit(&s_touch);
  if (g_app.touch_hw) touch_service_unsubscribe();
}

static void window_unload(Window *window) {
  dots_layer_destroy(s_dots);
  layer_destroy(s_canvas);
  window_destroy(s_window);
  s_window = NULL;
}

void place_window_close(void) {
  if (s_window) window_stack_remove(s_window, false);
}

static void place_push(int src, int idx, const char *title, int request_id) {
  if (s_window) window_stack_remove(s_window, false);
  s_src = src;
  s_idx = idx;
  s_suggest_request = request_id;
  s_loaded = false;
  s_fav = false;
  strncpy(s_name, title ? title : "", sizeof(s_name) - 1);
  s_type[0] = s_addr[0] = s_dist[0] = 0;
  // short on memory (Pebble Time, Time Round): drop the screens underneath first
  if (heap_bytes_free() < 3000) {
    list_windows_close_all();
    results_window_close();
  }
  s_window = window_create();
  window_set_background_color(s_window, C_BG);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load, .appear = window_appear,
    .disappear = window_disappear, .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

void place_window_push(int src, int idx, const char *title) { place_push(src, idx, title, 0); }
void place_window_push_suggestion(int idx, const char *title, int request_id) { place_push(SRC_SUGGEST, idx, title, request_id); }
