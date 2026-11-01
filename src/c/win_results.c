// Pebble Maps - search / explore results: map with pins + card, or list
#include "windows.h"
#include "comm.h"
#include "mapdata.h"
#include "mapbar.h"

#define MAX_RESULTS 10

static Window *s_window;
static Layer *s_canvas;
static Layer *s_dots;
static MenuLayer *s_menu;
static MapBar s_bar;
static MapTouch s_touch;
static ListItem *s_items;
static int s_count;
static int s_sel;
static bool s_loaded;
static char s_title[48];
static char s_query[160];
static int s_category = -1;
static char s_status[96];


static int card_h(void) { return g_fonts.body_h + g_fonts.small_h * 2 + PBL_IF_ROUND_ELSE(14, 8); }

static void request_results(void) {
  OutMsg m;
  if (s_category >= 0) {
    comm_msg_init(&m, CMD_NEARBY);
    m.idx = s_category;
    m.mode = g_app.open_now ? 1 : 0;   // open now only
  } else {
    comm_msg_init(&m, CMD_SEARCH);
    m.mode = g_app.open_now ? 1 : 0;
    strncpy(m.text, s_query, sizeof(m.text) - 1);
  }
  comm_send(&m);
  s_loaded = false;
  snprintf(s_status, sizeof(s_status), "Searching");
  dots_layer_set_running(s_dots, true);
}

static void request_map(bool restore) {
  GRect b = layer_get_bounds(window_get_root_layer(s_window));
  // mode = bottom area covered by the card (so pins fit above it); num via width/height
  map_request(CMD_RESULTS_MAP, b.size.w, b.size.h, s_sel, card_h() + 6 + (restore ? 10000 : 0));
}

static void handle(int cmd, DictionaryIterator *it, void *ctx) {
  switch (cmd) {
    case CMD_LIST:
      if (tuple_int(it, MESSAGE_KEY_num, -1) != LIST_RESULTS) return;
      free(s_items);
      s_items = alloc_list(tuple_str(it, MESSAGE_KEY_list), MAX_RESULTS, &s_count);
      s_sel = 0;
      s_loaded = true;
      s_status[0] = 0;
      if (s_menu) menu_layer_reload_data(s_menu);
      if (s_count) request_map(false);
      dots_layer_set_running(s_dots, false);
      layer_mark_dirty(s_canvas);
      break;
    case CMD_BUSY:
      strncpy(s_status, tuple_str(it, MESSAGE_KEY_text), sizeof(s_status) - 1);
      layer_mark_dirty(s_canvas);
      break;
    case CMD_TOAST:
      ui_toast(s_window, tuple_str(it, MESSAGE_KEY_text));
      break;
    case CMD_ERROR:
      dots_layer_set_running(s_dots, false);
      s_loaded = true;
      s_count = 0;
      if (tuple_int(it, MESSAGE_KEY_num, 0) == ERR_NO_RESULTS) {
        strncpy(s_status, tuple_str(it, MESSAGE_KEY_text), sizeof(s_status) - 1);
        layer_mark_dirty(s_canvas);
      } else {
        s_status[0] = 0;
        window_stack_remove(s_window, false);
        show_error(it);
      }
      break;
  }
}

static GRect card_rect(GRect b) {
  int ch = card_h();
#ifdef PBL_ROUND
  return GRect(0, b.size.h - ch, b.size.w, ch + 10);
#else
  int right = s_bar.open ? STRIP_W : 0;
  return GRect(3, b.size.h - ch - 3, b.size.w - 6 - right, ch);
#endif
}

static void draw_card_content(GContext *ctx, GRect card) {
  if (!s_count) return;
  ListItem *item = &s_items[s_sel];
  int pad = PBL_IF_ROUND_ELSE(26, 6);
  GTextAlignment al = PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft);
  GRect tr = GRect(card.origin.x + pad, card.origin.y - 2, card.size.w - pad * 2, g_fonts.body_h + 6);
  graphics_context_set_text_color(ctx, C_TEXT);
  ui_text(ctx, item->title, g_fonts.body_b, tr, al | TXT_ELLIPSIS);
  GRect sr = GRect(tr.origin.x, card.origin.y + g_fonts.body_h, tr.size.w, g_fonts.small_h * 2 + 4);
  graphics_context_set_text_color(ctx, C_SUBTEXT);
  ui_text(ctx, item->sub, g_fonts.small, sr, al | TXT_ELLIPSIS);
#ifndef PBL_ROUND
  char cnt[16];
  snprintf(cnt, sizeof(cnt), "%d/%d", s_sel + 1, s_count);
  GRect cr = GRect(card.origin.x + card.size.w - 34, card.origin.y - 20, 30, 16);
  graphics_context_set_fill_color(ctx, C_BLUE);
  graphics_fill_rect(ctx, cr, 8, GCornersAll);
  graphics_context_set_text_color(ctx, GColorWhite);
  ui_text(ctx, cnt, g_fonts.chip, GRect(cr.origin.x, cr.origin.y - 2, cr.size.w, 16), GTextAlignmentCenter);
#endif
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  map_draw(ctx, b, s_sel);
  // title chip
  GRect top = GRect(PBL_IF_ROUND_ELSE(40, 4), PBL_IF_ROUND_ELSE(10, 4),
                    b.size.w - PBL_IF_ROUND_ELSE(80, 8) - (s_bar.open ? PBL_IF_ROUND_ELSE(0, STRIP_W) : 0), 20);
  draw_card(ctx, top, 10);
  graphics_context_set_text_color(ctx, C_TEXT);
  ui_text(ctx, s_title, g_fonts.chip, GRect(top.origin.x + 6, top.origin.y + 1, top.size.w - 12, 18), GTextAlignmentCenter | TXT_ELLIPSIS);

  GRect card = card_rect(b);
  if (s_loaded && s_count) {
    draw_card(ctx, card, PBL_IF_ROUND_ELSE(0, 8));
    draw_card_content(ctx, card);
  } else if (s_status[0]) {
    draw_card(ctx, card, PBL_IF_ROUND_ELSE(0, 8));
    graphics_context_set_text_color(ctx, C_TEXT);
    int pad = PBL_IF_ROUND_ELSE(26, 6);
    ui_text(ctx, s_status, g_fonts.body_b,
                       GRect(card.origin.x + pad, card.origin.y + 2, card.size.w - pad * 2, card.size.h - 4), GTextAlignmentCenter | TXT_ELLIPSIS);
  }
  if (s_bar.open) mapbar_draw(&s_bar, ctx, b);
  else if (s_count) draw_side_tab(ctx, b);   // always beside the middle button
}

// --- List mode -------------------------------------------------------------
static uint16_t menu_rows(MenuLayer *m, uint16_t s, void *ctx) { return s_count; }
static int16_t menu_cell_h(MenuLayer *m, MenuIndex *i, void *ctx) {
#ifdef PBL_ROUND
  return menu_layer_is_index_selected(m, i) ? g_fonts.body_h + g_fonts.small_h * 2 + 12 : g_fonts.body_h + 14;
#else
  return g_fonts.body_h + g_fonts.small_h + 10;
#endif
}
static void menu_draw_row(GContext *ctx, const Layer *cell, MenuIndex *i, void *cb) {
  ListItem *item = &s_items[i->row];
  GRect b = layer_get_bounds(cell);
  bool hl = menu_cell_layer_is_highlighted(cell);
  graphics_context_set_text_color(ctx, hl ? GColorWhite : C_TEXT);
#ifdef PBL_ROUND
  ui_text(ctx, item->title, g_fonts.body_b, GRect(18, 2, b.size.w - 36, g_fonts.body_h + 6), GTextAlignmentCenter | TXT_ELLIPSIS);
  if (hl) ui_text(ctx, item->sub, g_fonts.small, GRect(18, g_fonts.body_h + 2, b.size.w - 36, g_fonts.small_h * 2 + 4), GTextAlignmentCenter | TXT_ELLIPSIS);
#else
  icon_draw(ctx, ICON_PIN, GPoint(14, b.size.h / 2), 18, hl ? GColorWhite : C_PIN, hl ? C_BLUE : C_BG);
  ui_text(ctx, item->title, g_fonts.body_b, GRect(28, -2, b.size.w - 32, g_fonts.body_h + 6), GTextAlignmentLeft | TXT_ELLIPSIS);
  ui_text(ctx, item->sub, g_fonts.small, GRect(28, g_fonts.body_h, b.size.w - 32, g_fonts.small_h + 4), GTextAlignmentLeft | TXT_ELLIPSIS);
#endif
}
static void menu_select(MenuLayer *m, MenuIndex *i, void *ctx) {
  s_sel = i->row;
  place_window_push(SRC_RESULTS, s_sel, s_items[s_sel].title);
}

static Window *s_list_win;

static void list_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  GRect lb = layer_get_bounds(root);
#ifndef PBL_ROUND
  lb.origin.y += 4;
  lb.size.h -= 4;
#endif
  s_menu = menu_layer_create(lb);
  menu_layer_set_callbacks(s_menu, NULL, (MenuLayerCallbacks){
    .get_num_rows = menu_rows, .get_cell_height = menu_cell_h,
    .draw_row = menu_draw_row,
    .get_separator_height = ui_separator_h, .draw_separator = ui_draw_separator, .select_click = menu_select,
  });
  menu_layer_set_highlight_colors(s_menu, C_BLUE, GColorWhite);
  menu_layer_set_click_config_onto_window(s_menu, w);
  layer_add_child(root, menu_layer_get_layer(s_menu));
  menu_layer_set_selected_index(s_menu, MenuIndex(0, s_sel), MenuRowAlignCenter, false);
}

static void list_unload(Window *w) {
  s_sel = menu_layer_get_selected_index(s_menu).row;
  menu_layer_destroy(s_menu);
  s_menu = NULL;
  window_destroy(w);
  s_list_win = NULL;
}

static void set_list_mode(bool on) {
  if (!on || s_list_win || !s_count) return;
  mapbar_close(&s_bar);
  if (heap_bytes_free() < 4000) list_windows_close_all();   // short on memory (Time Round)
  s_list_win = window_create();
  window_set_window_handlers(s_list_win, (WindowHandlers){ .load = list_load, .unload = list_unload });
  window_stack_push(s_list_win, true);
}

// --- Map controls ------------------------------------------------------------
static void bar_cb(MapBarEvent ev, void *ctx) {
  if (ev == MB_EV_EXTRA_UP) set_list_mode(true);
  else if (ev == MB_EV_EXTRA_DOWN) map_adjust(ADJ_RESET, 0, 0);
  if (s_canvas) layer_mark_dirty(s_canvas);
}

// --- Buttons ---------------------------------------------------------------
static void move(int d) {
  if (!s_count) return;
  s_sel = (s_sel + d + s_count) % s_count;
  layer_mark_dirty(s_canvas);
}
static void press(ButtonId b) {
  if (mapbar_button(&s_bar, b)) return;
  switch (b) {
    case BUTTON_ID_UP: move(-1); break;
    case BUTTON_ID_DOWN: move(1); break;
    case BUTTON_ID_SELECT:
      if (s_count) place_window_push(SRC_RESULTS, s_sel, s_items[s_sel].title);
      else if (s_loaded) request_results();
      break;
    case BUTTON_ID_BACK: window_stack_remove(s_window, true); break;
    default: break;
  }
}
static void up_click(ClickRecognizerRef r, void *ctx) { press(BUTTON_ID_UP); }
static void down_click(ClickRecognizerRef r, void *ctx) { press(BUTTON_ID_DOWN); }
static void select_click(ClickRecognizerRef r, void *ctx) { press(BUTTON_ID_SELECT); }
static void back_click(ClickRecognizerRef r, void *ctx) { press(BUTTON_ID_BACK); }
static void select_long(ClickRecognizerRef r, void *ctx) {
  if (!s_count) return;
  if (s_bar.open) mapbar_close(&s_bar);
  else mapbar_open(&s_bar);
}

static void results_click_config(void *ctx) {
  window_single_repeating_click_subscribe(BUTTON_ID_UP, 300, up_click);
  window_single_repeating_click_subscribe(BUTTON_ID_DOWN, 300, down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
  window_long_click_subscribe(BUTTON_ID_SELECT, 500, select_long, NULL);
}

static void map_tap(GPoint p, void *ctx) {
  GRect b = layer_get_bounds(s_canvas);
  if (p.y > b.size.h - card_h()) {
    // the card with the place's name: photos, hours and reviews
    if (s_count) info_window_push(SRC_RESULTS, s_sel, s_items[s_sel].title);
    else press(BUTTON_ID_SELECT);
    return;
  }
  int idx = map_pin_near(p, b, 26);
  if (idx >= 0) {
    if (idx == s_sel) press(BUTTON_ID_SELECT);
    else {
      s_sel = idx;
      layer_mark_dirty(s_canvas);
    }
  }
}

static void touch_cb(const TouchEvent *e, void *ctx) {
  if (s_menu) return;
  GRect b = layer_get_bounds(s_canvas);
  if (e->x >= b.size.w - STRIP_W && e->type == TouchEvent_Liftoff && !e->non_navigational) {
    if (s_bar.open) {
      mapbar_button(&s_bar, e->y < b.size.h / 3 ? BUTTON_ID_UP : (e->y > b.size.h * 2 / 3 ? BUTTON_ID_DOWN : BUTTON_ID_SELECT));
      return;
    }
    if (abs(e->y - b.size.h / 2) < 30) { mapbar_open(&s_bar); return; }
  }
  maptouch_event(&s_touch, e);
}

// --- Lifecycle -------------------------------------------------------------
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  s_items = NULL;
  s_canvas = layer_create(b);
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
  s_dots = dots_layer_create(GRect(0, b.size.h / 2 - 30, b.size.w, 30));
  layer_add_child(root, s_dots);
  mapbar_init(&s_bar, s_canvas, GSize(b.size.w, b.size.h - card_h()), ICON_LIST, ICON_EXPLORE, bar_cb, NULL);
  s_bar.extra_up_color = C_BLUE;
  s_bar.extra_down_color = C_PIN;
  maptouch_init(&s_touch, GRect(0, 0, b.size.w - STRIP_W, b.size.h), map_tap, NULL, &s_bar);
  request_results();
}

static void window_appear(Window *window) {
  comm_set_handler(handle, NULL);
  map_set_observer(s_canvas);
  if (TOUCH_HW && g_app.touch) touch_service_subscribe(touch_cb, NULL);
  if (s_loaded && s_count && !g_map.bmp) request_map(true);
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
  s_canvas = NULL;
  free(s_items);
  s_items = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

static void push(void) {
  if (s_window) window_stack_remove(s_window, false);
  // short on memory (Pebble Time, Time Round): drop the lists underneath first
  if (heap_bytes_free() < 4000) list_windows_close_all();
  s_count = 0;
  s_sel = 0;
  s_loaded = false;
  s_window = window_create();
  window_set_background_color(s_window, C_MAPBG);
  window_set_click_config_provider(s_window, results_click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load, .appear = window_appear,
    .disappear = window_disappear, .unload = window_unload,
  });
  window_stack_push(s_window, true);
}

void results_window_close(void) {
  if (s_list_win) window_stack_remove(s_list_win, false);
  if (s_window) window_stack_remove(s_window, false);
}

void results_window_push_search(const char *query) {
  s_category = -1;
  strncpy(s_query, query, sizeof(s_query) - 1);
  s_query[sizeof(s_query) - 1] = 0;
  snprintf(s_title, sizeof(s_title), "\"%s\"", query);
  push();
}

void results_window_push_nearby(int category) {
  s_category = category;
  s_query[0] = 0;
  snprintf(s_title, sizeof(s_title), PBL_IF_ROUND_ELSE("%s", "%s nearby"), CATEGORY_NAMES[category]);
  push();
}
