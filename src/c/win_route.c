// Pebble Maps - route overview: Google route map, time, distance, start
#include "windows.h"
#include "comm.h"
#include "mapdata.h"
#include "mapbar.h"

static MapBar s_bar;
static MapTouch s_touch;

static Window *s_window;
static Layer *s_canvas;
static Layer *s_dots;
static int s_mode;
static bool s_loaded;
static bool s_failed;
static char s_dur[24];
static char s_dist[24];
static char s_via[64];
static char s_extra[64];
static time_t s_arrive;
static int s_traffic;

static int card_h(GRect b) { return g_fonts.big_h + g_fonts.small_h * 2 + PBL_IF_ROUND_ELSE(16, 6); }

static void request(void) {
  GRect b = layer_get_bounds(window_get_root_layer(s_window));
  s_loaded = false;
  s_failed = false;
  s_dur[0] = s_dist[0] = s_via[0] = s_extra[0] = 0;
  map_request(CMD_ROUTE, b.size.w - STRIP_W, b.size.h - card_h(b), -1, s_mode);
  dots_layer_set_running(s_dots, true);
  layer_mark_dirty(s_canvas);
}

static void handle(int cmd, DictionaryIterator *it, void *ctx) {
  switch (cmd) {
    case CMD_ROUTE_INFO:
      strncpy(s_dur, tuple_str(it, MESSAGE_KEY_text), sizeof(s_dur) - 1);
      strncpy(s_dist, tuple_str(it, MESSAGE_KEY_text2), sizeof(s_dist) - 1);
      strncpy(s_via, tuple_str(it, MESSAGE_KEY_text3), sizeof(s_via) - 1);
      strncpy(s_extra, tuple_str(it, MESSAGE_KEY_text4), sizeof(s_extra) - 1);
      s_arrive = (time_t)tuple_int(it, MESSAGE_KEY_num2, 0);
      s_traffic = tuple_int(it, MESSAGE_KEY_flags, 0) & 3;
      s_loaded = true;
      dots_layer_set_running(s_dots, false);
      layer_mark_dirty(s_canvas);
      break;
    case CMD_TOAST:
      ui_toast(s_window, tuple_str(it, MESSAGE_KEY_text));
      break;
    case CMD_ERROR:
      dots_layer_set_running(s_dots, false);
      s_failed = true;
      strncpy(s_via, tuple_str(it, MESSAGE_KEY_text), sizeof(s_via) - 1);
      layer_mark_dirty(s_canvas);
      break;
  }
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int w = b.size.w - STRIP_W;
  int ch = card_h(b);
  map_draw(ctx, GRect(0, 0, w, b.size.h - ch), -1);
  GRect card = GRect(0, b.size.h - ch, w, ch);
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, card, 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, C_DIVIDER);
  graphics_draw_line(ctx, card.origin, GPoint(w, card.origin.y));

  int pad = PBL_IF_ROUND_ELSE(22, 5);
  GTextAlignment al = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);
  int x = pad, tw = w - pad - 2, y = card.origin.y;
  if (s_failed) {
    graphics_context_set_text_color(ctx, C_RED);
    ui_text(ctx, s_via[0] ? s_via : "No route found", g_fonts.small_b, GRect(x, y + 2, tw, ch - 4), al | TXT_ELLIPSIS);
  } else if (s_loaded) {
    GColor dc = s_traffic == 2 ? C_RED : (s_traffic == 1 ? PBL_IF_COLOR_ELSE(GColorOrange, GColorBlack) : C_GREEN);
    if (s_mode != MODE_DRIVE) dc = C_BLUE;
    graphics_context_set_text_color(ctx, dc);
#ifdef PBL_ROUND
    char top[48];
    snprintf(top, sizeof(top), "%s (%s)", s_dur, s_dist);
    ui_text(ctx, top, g_fonts.body_b, GRect(x, y - 2, tw, g_fonts.body_h + 6), al);
    y += g_fonts.body_h;
#else
    GSize ds = graphics_text_layout_get_content_size(s_dur, g_fonts.big, GRect(0, 0, tw, 40),
                                                     GTextOverflowModeFill, GTextAlignmentLeft);
    ui_text(ctx, s_dur, g_fonts.big, GRect(x, y - 5, tw, g_fonts.big_h + 8), al);
    graphics_context_set_text_color(ctx, C_SUBTEXT);
    char d[32];
    snprintf(d, sizeof(d), "(%s)", s_dist);
    ui_text(ctx, d, g_fonts.small, GRect(x + ds.w + 4, y + g_fonts.big_h - g_fonts.small_h - 4, tw - ds.w - 4, g_fonts.small_h + 4), GTextAlignmentLeft | TXT_ELLIPSIS);
    y += g_fonts.big_h;
#endif
    graphics_context_set_text_color(ctx, C_SUBTEXT);
    const char *line2 = s_extra[0] ? s_extra : s_via;
    ui_text(ctx, line2, g_fonts.small, GRect(x, y - 3, tw, g_fonts.small_h + 4), al | TXT_ELLIPSIS);
    y += g_fonts.small_h;
    if (s_arrive) {
      char clk[16], arr[32];
      format_clock(s_arrive, clk, sizeof(clk));
      snprintf(arr, sizeof(arr), "Arrive %s", clk);
      graphics_context_set_text_color(ctx, C_TEXT);
      ui_text(ctx, arr, g_fonts.small_b, GRect(x, y - 3, tw, g_fonts.small_h + 4), al);
    }
  }
  if (s_bar.open) {
    mapbar_draw(&s_bar, ctx, b);
  } else {
    GRect bd = GRect(PBL_IF_ROUND_ELSE(w / 2 - 10, 3), b.size.h - ch - 22, 20, 19);
    draw_card(ctx, bd, 5);
    icon_draw(ctx, ICON_MOVE, GPoint(bd.origin.x + 10, bd.origin.y + 9), 13, C_TEXT, C_BG);
  StripIcons ic = { .up = ICON_LIST, .select = ICON_NAV, .down = icon_for_mode((s_mode + 1) % MODE_COUNT),
                    .select_color = C_BLUE };
  draw_action_strip(ctx, b, &ic);
  }
  // current mode chip on the map
  GRect chip = GRect(PBL_IF_ROUND_ELSE(w / 2 - 34, 4), PBL_IF_ROUND_ELSE(12, 4), 68, 20);
  draw_card(ctx, chip, 10);
  icon_draw(ctx, icon_for_mode(s_mode), GPoint(chip.origin.x + 12, chip.origin.y + 10), 13, C_BLUE, C_BG);
  graphics_context_set_text_color(ctx, C_TEXT);
  ui_text(ctx, mode_name(s_mode), g_fonts.chip, GRect(chip.origin.x + 21, chip.origin.y + 1, 46, 16), GTextAlignmentLeft);
}

static void bar_cb(MapBarEvent ev, void *ctx) {
  if (ev == MB_EV_EXTRA_UP) map_adjust(ADJ_RESET, 0, 0);
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void up_click(ClickRecognizerRef r, void *ctx) {
  if (mapbar_button(&s_bar, BUTTON_ID_UP)) return;
  if (s_loaded) list_window_push(LW_STEPS, 0);
}
static void select_click(ClickRecognizerRef r, void *ctx) {
  if (mapbar_button(&s_bar, BUTTON_ID_SELECT)) return;
  if (s_loaded) nav_window_push(s_mode);
  else if (s_failed) request();
}
static void down_click(ClickRecognizerRef r, void *ctx) {
  if (mapbar_button(&s_bar, BUTTON_ID_DOWN)) return;
  s_mode = (s_mode + 1) % MODE_COUNT;
  request();
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

static void touch_cb(const TouchEvent *e, void *ctx) {
  if (maptouch_event(&s_touch, e)) return;
  if (e->type != TouchEvent_Liftoff || e->non_navigational) return;
  GRect b = layer_get_bounds(s_canvas);
  if (e->x < b.size.w - STRIP_W) return;
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
  s_dots = dots_layer_create(GRect(0, b.size.h - card_h(b) + 6, b.size.w - STRIP_W, 30));
  layer_add_child(root, s_dots);
  GSize mf = GSize(b.size.w - STRIP_W, b.size.h - card_h(b));
  mapbar_init(&s_bar, s_canvas, mf, ICON_NAV, ICON_NONE, bar_cb, NULL);
  s_bar.extra_up_color = C_BLUE;
  maptouch_init(&s_touch, GRect(0, 0, mf.w, mf.h), NULL, NULL, &s_bar);
}

static void window_appear(Window *window) {
  comm_set_handler(handle, NULL);
  map_set_observer(s_canvas);
  if (TOUCH_HW && g_app.touch) touch_service_subscribe(touch_cb, NULL);
  if (!s_loaded || !g_map.bmp) request();
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

void route_window_push(int mode) {
  if (s_window) window_stack_remove(s_window, false);
  if (heap_bytes_free() < 4000) results_window_close();   // short on memory (Time, Time Round)
  s_mode = mode;
  s_loaded = false;
  s_window = window_create();
  window_set_background_color(s_window, C_BG);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load, .appear = window_appear,
    .disappear = window_disappear, .unload = window_unload,
  });
  window_stack_push(s_window, true);
}
