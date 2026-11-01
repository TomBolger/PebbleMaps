// Pebble Maps - home screen: your location on a Google map, search bar, actions
#include "windows.h"
#include "keyboard.h"
#include "comm.h"
#include "mapdata.h"
#include "mapbar.h"

static Window *s_window;
static Layer *s_canvas;
static Layer *s_dots;
static DictationSession *s_dictation;
static MapBar s_bar;
static MapTouch s_touch;
static bool s_map_requested;
static bool s_hello_sent;
static AppTimer *s_hello_timer;
static int s_hello_tries;
static char s_status[64] = "Connecting to phone";
static bool s_gear;           // options strip showing
static AppTimer *s_gear_timer;
// How the map follows you: heading up (turns with you), north up, or a plain map
enum { FOLLOW_DRIVE = 0, FOLLOW_NORTH, FOLLOW_OFF, FOLLOW_COUNT };
static int s_follow = FOLLOW_DRIVE;
#define PERSIST_FOLLOW 8
static IconId follow_icon(void);
static void touch_cb(const TouchEvent *e, void *ctx);

Window *home_window_get(void) { return s_window; }

void pop_to_home(void) {
  while (window_stack_get_top_window() && window_stack_get_top_window() != s_window) {
    window_stack_pop(false);
  }
}

// --- Side buttons that hide when not in use ---------------------------------------
// The map is always requested and drawn full screen; the strip slides over it.
// s_off: how far the strip has slid out to the right (0 = shown, STRIP_W = gone)
#define TAB_ZONE 24
static int s_off, s_target;
static AppTimer *s_hide_timer, *s_slide_timer;

static void slide_cb(void *ctx) {
  s_slide_timer = NULL;
  s_off += clampi(s_target - s_off, -(STRIP_W / 5), STRIP_W / 5);
  layer_mark_dirty(s_canvas);
  if (s_off != s_target) s_slide_timer = app_timer_register(40, slide_cb, NULL);
}

static void slide_to(int target) {
  s_target = target;
  if (s_off != target && !s_slide_timer) s_slide_timer = app_timer_register(40, slide_cb, NULL);
}

static void hide_cb(void *ctx);

// Something happened: start the countdown again
static void activity(void) {
  if (s_hide_timer) app_timer_cancel(s_hide_timer);
  s_hide_timer = g_app.hide_s ? app_timer_register(g_app.hide_s * 1000, hide_cb, NULL) : NULL;
}

static void hide_cb(void *ctx) {
  s_hide_timer = NULL;
  if (s_bar.open || s_gear || ui_toast_active()) activity();   // still in use
  else slide_to(STRIP_W);
}

// Bring the strip back; true if it was hidden (that press does nothing else)
static bool unhide(void) {
  bool was = s_target != 0;
  slide_to(0);
  activity();
  return was;
}

static void send_hello(void) {
  Layer *root = window_get_root_layer(s_window);
  GRect b = layer_get_bounds(root);
  OutMsg m;
  comm_msg_init(&m, CMD_HELLO);
  m.width = b.size.w;
  m.height = b.size.h;
  m.num = PBL_IF_COLOR_ELSE(FMT_4BIT, FMT_1BIT);
  m.mode = (g_app.has_mic ? 1 : 0) | (PBL_IF_ROUND_ELSE(1, 0) << 1) | (g_app.touch ? 4 : 0) | (PM_LOWMEM ? 8 : 0);
  m.idx = g_app.inbox_size;
  GSize bs = map_buffer_size();
  snprintf(m.text, sizeof(m.text), "%d,%d", bs.w, bs.h);
  comm_send(&m);
  s_hello_sent = true;
}

// "Finding you" that gets no answer: ask again after a few seconds, then give it longer
static AppTimer *s_find_timer;
static int s_find_tries;
static void request_map(bool restore);

static void find_timer_cb(void *ctx) {
  s_find_timer = NULL;
  if (g_map.complete || g_map.chunk_img >= 0 || window_stack_get_top_window() != s_window) return;   // it's arriving
  s_find_tries++;
  request_map(true);
  strncpy(s_status, "Still finding you", sizeof(s_status));
  layer_mark_dirty(s_canvas);
}

static void find_watch_stop(void) {
  if (s_find_timer) app_timer_cancel(s_find_timer);
  s_find_timer = NULL;
}

// restore = keep the last view the user moved to (when coming back to this screen)
static void request_map(bool restore) {
  poi_prefs_send();   // which places to draw
  GRect f = layer_get_bounds(window_get_root_layer(s_window));
  map_request(CMD_HOME_MAP, f.size.w, f.size.h, 15, (restore ? 1 : 0) | (s_follow << 1));
  s_map_requested = true;
  strncpy(s_status, "Finding you", sizeof(s_status));
  dots_layer_set_running(s_dots, true);
  // 6 s, then 15 s, then 30 s between tries
  find_watch_stop();
  if (s_find_tries < 4) s_find_timer = app_timer_register(s_find_tries == 0 ? 6000 : (s_find_tries == 1 ? 15000 : 30000),
                                                          find_timer_cb, NULL);
}

static void hello_timer_cb(void *ctx) {
  s_hello_timer = NULL;
  if (g_app.status_known) return;
  if (++s_hello_tries < 10) {
    send_hello();
    s_hello_timer = app_timer_register(2500, hello_timer_cb, NULL);
  } else {
    strncpy(s_status, "Can't reach your phone", sizeof(s_status));
    dots_layer_set_running(s_dots, false);
    layer_mark_dirty(s_canvas);
  }
}

static void show_setup(void) {
  msg_window_push(ICON_KEY, "Almost ready!",
                  "Pebble Maps needs a free Google Maps key.\n\n"
                  "On your phone, open the Pebble app, find Pebble Maps and tap Settings. "
                  "The settings page walks you through it step by step.",
                  true);
}

static void handle(int cmd, DictionaryIterator *it, void *ctx) {
  switch (cmd) {
    case CMD_STATUS:
#if TOUCH_HW
      // touch switched on/off in Settings
      if (g_app.touch_hw) {
        touch_service_unsubscribe();
        if (TOUCH_HW && g_app.touch) touch_service_subscribe(touch_cb, NULL);
      }
#endif
      if (!g_app.hide_s) slide_to(0);
      activity();
      if (!s_hello_sent || !s_map_requested) send_hello();
      if (!g_app.configured) {
        dots_layer_set_running(s_dots, false);
        strncpy(s_status, "Setup needed", sizeof(s_status));
        show_setup();
      } else if (!s_map_requested || !g_map.bmp) {
        request_map(s_map_requested);
      }
      layer_mark_dirty(s_canvas);
      break;
    case CMD_BUSY:
      strncpy(s_status, tuple_str(it, MESSAGE_KEY_text), sizeof(s_status) - 1);
      dots_layer_set_running(s_dots, true);
      layer_mark_dirty(s_canvas);
      break;
    case CMD_TOAST:
      ui_toast(s_window, tuple_str(it, MESSAGE_KEY_text));
      dots_layer_set_running(s_dots, false);
      break;
    case CMD_ERROR:
      find_watch_stop();
      dots_layer_set_running(s_dots, false);
      s_status[0] = 0;
      layer_mark_dirty(s_canvas);
      if (tuple_int(it, MESSAGE_KEY_num, 0) == ERR_NO_KEY) show_setup();
      else show_error(it);
      break;
  }
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  int vw = b.size.w - STRIP_W + s_off;   // width not covered by the strip right now
  map_draw(ctx, b, -1);
  if (g_map.complete) dots_layer_set_running(s_dots, false);

#if TOUCH_HW
  // Search pill (Google style), touch layout only - hidden while moving the map
  if (!s_bar.open && !g_app.buttons_ui) {
    int ph = g_fonts.small_h + 10;
#ifdef PBL_ROUND
    int rad = b.size.w / 2;
    int py = b.size.h / 9;
    int dy = rad - py;
    int half = 0;
    while ((half + 1) * (half + 1) + dy * dy <= rad * rad) half++;
    half -= 10;
    GRect pill = GRect(rad - half, py, half * 2, ph);
#else
    GRect pill = GRect(6, 6, vw - 12, ph);
#endif
#ifdef PBL_COLOR
    ui_fill_rect(ctx, GColorLightGray, GRect(pill.origin.x + 1, pill.origin.y + 2, pill.size.w, pill.size.h), ph / 2);
#endif
    draw_card(ctx, pill, ph / 2);
    icon_draw(ctx, ICON_SEARCH, GPoint(pill.origin.x + 14, pill.origin.y + ph / 2), 14, C_TEXT, C_BG);
    graphics_context_set_text_color(ctx, C_TEXT);
    ui_text(ctx, g_app.has_mic ? PBL_IF_ROUND_ELSE("Search", "Search here") : "Explore", g_fonts.small,
                       GRect(pill.origin.x + 25, pill.origin.y + (ph - g_fonts.small_h) / 2 - 3, pill.size.w - 30, ph), GTextAlignmentLeft | TXT_ELLIPSIS);
  }
#endif

  // Heading up (map turned to your direction): a compass shows where north is
  if (g_map.bmp && g_map.rheading) {
    int cy = g_app.buttons_ui ? PBL_IF_ROUND_ELSE(b.size.h / 9 + 6, 16) : PBL_IF_ROUND_ELSE(b.size.h / 9 + g_fonts.small_h + 26, g_fonts.small_h + 34);
    draw_compass(ctx, GPoint(PBL_IF_ROUND_ELSE(vw / 2 + 10, 18), cy), g_map.rheading);
  }

  // Status bubble
  if (s_status[0] && !g_map.complete) {
    int sh = g_fonts.small_h + 8;
    GRect sbx = GRect(6, b.size.h - sh - PBL_IF_ROUND_ELSE(34, 8), vw - 12, sh);
#ifdef PBL_ROUND
    sbx.origin.x += 14;
    sbx.size.w -= 28;
#endif
    draw_card(ctx, sbx, 6);
    graphics_context_set_text_color(ctx, C_TEXT);
    ui_text(ctx, s_status, g_fonts.small_b, GRect(sbx.origin.x + 2, sbx.origin.y + 1, sbx.size.w - 4, sh), GTextAlignmentCenter | TXT_ELLIPSIS);
  }

  GRect sb = GRect(0, 0, b.size.w + s_off, b.size.h);   // where the strip is drawn: slid right by s_off
  if (s_bar.open) {
    mapbar_draw(&s_bar, ctx, b);
  } else if (s_off >= STRIP_W) {
    draw_side_tab(ctx, b, g_app.dark_map);   // hidden: just the tab
  } else if (s_gear) {
    // options: following you (top, with dots), Places or map controls (middle), places on the map (bottom)
    StripIcons ic = { .up = follow_icon(), .up_color = C_BLUE, .up_page = s_follow, .up_pages = FOLLOW_COUNT,
                      .select = g_app.buttons_ui ? ICON_EXPLORE : ICON_MOVE, .select_plain = true,
                      .down = ICON_POI_NAMES };
    draw_action_strip(ctx, sb, &ic);
  } else if (g_app.buttons_ui) {
    StripIcons ic = { .up = ICON_MOVE, .select = g_app.has_mic ? ICON_MIC : ICON_SEARCH,
                      .down = ICON_GEAR, .select_color = C_BLUE };
    draw_action_strip(ctx, sb, &ic);
  } else {
    StripIcons ic = { .up = ICON_GEAR, .select = g_app.has_mic ? ICON_MIC : ICON_SEARCH,
                      .down = ICON_EXPLORE, .select_color = C_BLUE, .down_color = C_PIN };
    draw_action_strip(ctx, sb, &ic);
  }
}

#if KEYBOARD_AVAILABLE
// --- Typing a search (touch keyboard with suggestions from Google) ---------------
static int s_ac_id;
static int s_ac_shown_id;
static bool s_ac_new_session;

static void kb_background(GContext *ctx, GRect b, void *c) { map_draw(ctx, b, -1); }

static void kb_handler(int cmd, DictionaryIterator *it, void *ctx) {
  if (cmd != CMD_LIST || tuple_int(it, MESSAGE_KEY_num, -1) != LIST_SUGGEST) return;
  if (tuple_int(it, MESSAGE_KEY_idx, 0) != s_ac_id) return;   // answer to older typing
  int n = 0;
  ListItem *items = alloc_list(tuple_str(it, MESSAGE_KEY_list), 5, &n);
  keyboard_set_suggestions(items, n);
  s_ac_shown_id = s_ac_id;
}

static void kb_appear(void *c) { comm_set_handler(kb_handler, NULL); }

static void kb_changed(const char *text, void *c) {
  s_ac_id = s_ac_id % 30000 + 1;
  if (strlen(text) < 2) return;
  OutMsg m;
  comm_msg_init(&m, CMD_AUTOCOMPLETE);
  m.idx = s_ac_id;
  if (s_ac_new_session) { m.mode = 1; s_ac_new_session = false; }
  strncpy(m.text, text, sizeof(m.text) - 1);
  comm_send(&m);
}

static void kb_pick(int i, const char *title, void *c) { place_window_push_suggestion(i, title, s_ac_shown_id); }
static void kb_done(const char *text, void *c) { results_window_push_search(text); }

static void open_keyboard(void) {
  static const KeyboardHooks hooks = {
    .background = kb_background, .appear = kb_appear, .changed = kb_changed, .pick = kb_pick, .done = kb_done,
  };
  s_ac_new_session = true;
  keyboard_window_push("Search here", "Search", &hooks, NULL);
}
#endif

// --- Dictation -------------------------------------------------------------
static void dictation_cb(DictationSession *session, DictationSessionStatus status,
                         char *transcription, void *ctx) {
  if (status == DictationSessionStatusSuccess && transcription && transcription[0]) {
    results_window_push_search(transcription);
  } else if (status == DictationSessionStatusFailureNoSpeechDetected ||
             status == DictationSessionStatusFailureTranscriptionRejected ||
             status == DictationSessionStatusFailureTranscriptionRejectedWithError) {
    // user cancelled or said nothing; stay quiet
  } else {
    msg_window_push(ICON_MIC, "Voice unavailable",
                    "Dictation didn't work. Make sure your phone is connected, "
                    "or browse places with the red pin button.", false);
  }
}

static void start_search(void) {
  if (!g_app.configured) { show_setup(); return; }
#ifdef SHOT_TEST
#if KEYBOARD_AVAILABLE
  open_keyboard(); return;  // TEST ONLY
#else
  results_window_push_search("Starbucks"); return;  // TEST ONLY
#endif
#endif
#if defined(PBL_MICROPHONE)
  if (!s_dictation) s_dictation = dictation_session_create(160, dictation_cb, NULL);
  if (s_dictation) {
    dictation_session_enable_confirmation(s_dictation, true);
    dictation_session_start(s_dictation);
    return;
  }
#endif
  list_window_push(LW_CATEGORIES, 0);
}

// --- Map controls ------------------------------------------------------------
static IconId follow_icon(void) {
  return s_follow == FOLLOW_DRIVE ? ICON_NAV : (s_follow == FOLLOW_NORTH ? ICON_NORTH_UP : ICON_MAP);
}

// Heading up -> north up -> plain map -> ...; every change also brings the map back to you
static void cycle_follow(void) {
  s_follow = (s_follow + 1) % FOLLOW_COUNT;
  persist_write_int(PERSIST_FOLLOW, s_follow);
  OutMsg m;
  comm_msg_init(&m, CMD_FOLLOW_MODE);
  m.idx = s_follow;
  m.seq = g_map.seq;
  comm_send(&m);
  ui_toast(s_window, s_follow == FOLLOW_DRIVE ? "Heading up" : (s_follow == FOLLOW_NORTH ? "Follow me, north up" : "Map only"));
}

static void bar_cb(MapBarEvent ev, void *ctx) {
  activity();
  layer_mark_dirty(s_canvas);
}

// --- Options strip (gear) ------------------------------------------------------
static void gear_close(void) {
  s_gear = false;
  if (s_gear_timer) app_timer_cancel(s_gear_timer);
  s_gear_timer = NULL;
  if (s_canvas) layer_mark_dirty(s_canvas);
  activity();
}
static void gear_timeout(void *ctx) {
  s_gear_timer = NULL;
  gear_close();
}
static void gear_poke(void) {
  if (s_gear_timer) app_timer_reschedule(s_gear_timer, 5000);
  else s_gear_timer = app_timer_register(5000, gear_timeout, NULL);
}
static void gear_open(void) {
  s_gear = true;
  gear_poke();
  layer_mark_dirty(s_canvas);
}

static void gear_press(ButtonId b) {
  switch (b) {
    case BUTTON_ID_UP:
      gear_poke();
      cycle_follow();
      layer_mark_dirty(s_canvas);
      break;
    case BUTTON_ID_SELECT:
      gear_close();
      if (g_app.buttons_ui) list_window_push(LW_CATEGORIES, 0);   // Places
      else mapbar_open(&s_bar);                                    // map controls
      break;
    case BUTTON_ID_DOWN:
      gear_close();
      list_window_push(LW_MAPPLACES, 0);
      break;
    default:
      gear_close();
      break;
  }
}

// --- Buttons ---------------------------------------------------------------
static void press(ButtonId b) {
  if (unhide()) return;   // hidden: any button only brings the side buttons back (Back too)
  if (mapbar_button(&s_bar, b)) return;
  if (s_gear) { gear_press(b); return; }
  if (!g_app.configured && b != BUTTON_ID_BACK) { show_setup(); return; }
  switch (b) {
    case BUTTON_ID_UP:
      if (g_app.buttons_ui) mapbar_open(&s_bar);
      else gear_open();
      break;
    case BUTTON_ID_SELECT: start_search(); break;
    case BUTTON_ID_DOWN:
      if (g_app.buttons_ui) gear_open();
      else list_window_push(LW_CATEGORIES, 0);
      break;
    case BUTTON_ID_BACK: window_stack_pop(true); break;
    default: break;
  }
}
static void up_click(ClickRecognizerRef r, void *ctx) { press(BUTTON_ID_UP); }
static void select_click(ClickRecognizerRef r, void *ctx) { press(BUTTON_ID_SELECT); }
static void down_click(ClickRecognizerRef r, void *ctx) { press(BUTTON_ID_DOWN); }
static void back_click(ClickRecognizerRef r, void *ctx) { press(BUTTON_ID_BACK); }
static void zoom(int action) {
  activity();
  if (!g_app.configured) return;
  map_adjust(action, 0, 0);
  mapbar_poke(&s_bar);
}
static void up_long(ClickRecognizerRef r, void *ctx) { zoom(ADJ_ZOOM_IN); }
static void down_long(ClickRecognizerRef r, void *ctx) { zoom(ADJ_ZOOM_OUT); }
static void select_long(ClickRecognizerRef r, void *ctx) {
  activity();
  if (!g_app.configured) { show_setup(); return; }
  map_adjust(ADJ_RESET, 0, 0);
  ui_toast(s_window, "Back to you");
}

static void click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
  window_long_click_subscribe(BUTTON_ID_UP, 500, up_long, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 500, down_long, NULL);
  window_long_click_subscribe(BUTTON_ID_SELECT, 600, select_long, NULL);
}

// --- Touch (Pebble Time 2 / Round 2) ----------------------------------------
static void touch_cb(const TouchEvent *e, void *ctx) {
  activity();
  GRect b = layer_get_bounds(window_get_root_layer(s_window));
  // the strip (or, hidden, the edge where its tab is) is not part of the map
  if (!s_touch.down && e->x >= b.size.w - (s_target ? TAB_ZONE : STRIP_W)) {
    if (e->type != TouchEvent_Liftoff || e->non_navigational) return;
    if (e->y < b.size.h / 3) press(BUTTON_ID_UP);
    else if (e->y > b.size.h * 2 / 3) press(BUTTON_ID_DOWN);
    else press(BUTTON_ID_SELECT);
    return;
  }
  maptouch_event(&s_touch, e);
}

static void map_tap(GPoint p, void *ctx) {
  if (!g_app.configured) { show_setup(); return; }
  bool in_pill = !s_bar.open && !g_app.buttons_ui && p.y < g_fonts.small_h + 20 + PBL_IF_ROUND_ELSE(24, 0);
  if (!in_pill) {
    // a place on the map: its photos, hours and reviews
    int poi = map_poi_near(p, layer_get_bounds(s_canvas), 18);
    if (poi >= 0) info_window_push(SRC_POI, poi, NULL);
    return;
  }
  // tapping the search bar opens the keyboard (the mic button is for voice)
#if KEYBOARD_AVAILABLE
  if (!g_app.configured) { show_setup(); return; }
  open_keyboard();
#else
  press(BUTTON_ID_SELECT);
#endif
}

// --- Lifecycle -------------------------------------------------------------
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  s_canvas = layer_create(b);
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
  s_dots = dots_layer_create(GRect(0, b.size.h / 2 - 20, b.size.w, 40));
  layer_add_child(root, s_dots);
  dots_layer_set_running(s_dots, true);
  if (persist_exists(PERSIST_FOLLOW)) s_follow = clampi(persist_read_int(PERSIST_FOLLOW), 0, FOLLOW_COUNT - 1);
  mapbar_init(&s_bar, s_canvas, b.size, ICON_NONE, ICON_NONE, bar_cb, NULL);
  maptouch_init(&s_touch, b, map_tap, NULL, &s_bar);
}

static void window_appear(Window *window) {
  comm_set_handler(handle, NULL);
  s_off = s_target = 0;   // coming back: the side buttons are shown again
  activity();
  map_set_observer(s_canvas);
  if (TOUCH_HW && g_app.touch) touch_service_subscribe(touch_cb, NULL);
  if (g_app.status_known && g_app.configured) {
    s_find_tries = 0;
    request_map(s_map_requested);
  } else if (!s_hello_timer && !g_app.status_known) {
    send_hello();
    s_hello_timer = app_timer_register(2500, hello_timer_cb, NULL);
  }
}

static void window_disappear(Window *window) {
  find_watch_stop();
  comm_clear_handler(handle);
  comm_cmd(CMD_CANCEL);   // map not showing: the phone can rest the GPS
  map_set_observer(NULL);
  ui_toast_cancel();
  gear_close();
  mapbar_deinit(&s_bar);
  if (s_hide_timer) app_timer_cancel(s_hide_timer);
  if (s_slide_timer) app_timer_cancel(s_slide_timer);
  s_hide_timer = s_slide_timer = NULL;
  maptouch_deinit(&s_touch);
  if (g_app.touch_hw) touch_service_unsubscribe();
}

static void window_unload(Window *window) {
  dots_layer_destroy(s_dots);
  layer_destroy(s_canvas);
  if (s_dictation) dictation_session_destroy(s_dictation);
  s_dictation = NULL;
}

void home_window_push(void) {
  s_window = window_create();
  window_set_background_color(s_window, C_MAPBG);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load, .appear = window_appear,
    .disappear = window_disappear, .unload = window_unload,
  });
  window_stack_push(s_window, true);
}
