// Pebble Maps - turn-by-turn navigation on a live Google map
#include "windows.h"
#include "comm.h"
#include "mapdata.h"
#include "mapbar.h"
#include "voice.h"
#ifndef IDLE_MS
#define IDLE_MS 5000
#endif

static Window *s_window;
static Layer *s_canvas;
static Layer *s_dots;
static int s_mode;
static bool s_have;
static bool s_arrived;
// alerts: vibrate only, vibrate + voice (watches with a speaker), or off
enum { ALERT_VIBE = 0, ALERT_VOICE, ALERT_OFF };
static int s_alert_mode;
#define s_muted (s_alert_mode == ALERT_OFF)
#define PERSIST_VOICE 3
static GPoint s_compass;      // where the compass is drawn (tap it to switch north-up / follow)
static bool s_cards_hidden;   // this navigation session only
static int s_flags;
static int s_man = MAN_DEPART;
static int s_next_man = MAN_NONE;
static char s_instr[128] = "Starting navigation";
static char s_dist[24];
static char s_remain[48];
static char s_detail[64];
static time_t s_arrive;
static GPoint s_puck, s_focus;
static bool s_have_puck;
static int s_heading;
static int s_map_heading;     // which way is up on the current picture
static bool s_heading_up = true;
#define PERSIST_HEADING_UP 2
static AppTimer *s_back_timer;
static bool s_back_armed;

// right-side options bar
static bool s_menu_open;
static int s_menu_page;
static AppTimer *s_menu_timer;
static MapBar s_bar;          // map controls (zoom / move)
static MapTouch s_touch;
static bool s_moved;          // user moved the map: re-center when controls close

enum { PAGE_VIEW = 0, PAGE_CARDS, PAGE_VOLUME, PAGE_END };
static int menu_pages(void) { return s_alert_mode == ALERT_VOICE ? 4 : 3; }
static int page_kind(int p) {
  if (p < 2) return p;
  if (s_alert_mode == ALERT_VOICE && p == 2) return PAGE_VOLUME;
  return PAGE_END;
}

static bool cards_visible(void) {
  return !s_cards_hidden || s_arrived || (s_flags & (NAV_NEAR | NAV_ALERT_NOW | NAV_ALERT_SOON | NAV_NO_GPS | NAV_REROUTING));
}

// --- Layout helpers -------------------------------------------------------------
static int icon_size(void) { return g_fonts.level >= 1 ? 38 : 30; }

#ifndef PBL_ROUND
// The instruction text area: measured and drawn with the same box so the banner fits it exactly
static GRect banner_text_rect(GRect b, int h) {
  int col = icon_size() + 16;
  return GRect(col, -3, b.size.w - col - 3, h);
}
#endif

#ifndef PBL_ROUND
// Big text if the instruction fits on two lines, otherwise the smaller font
static GFont banner_font(GRect b) {
  GRect r = banner_text_rect(b, 200);
  int two = graphics_text_layout_get_content_size("A\nA", g_fonts.body_b, r, GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
  int h = graphics_text_layout_get_content_size(s_instr, g_fonts.body_b, r, GTextOverflowModeWordWrap, GTextAlignmentLeft).h;
  return h <= two ? g_fonts.body_b : g_fonts.small_b;
}
#endif

static int banner_h(GRect b) {
#ifdef PBL_ROUND
  GSize ts = graphics_text_layout_get_content_size(s_instr, g_fonts.small_b, GRect(0, 0, b.size.w - 50, 100),
                                                   GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter);
  int h = 40 + ts.h + 8;
  return clampi(h, 70, b.size.h * 45 / 100);
#else
  GSize ts = graphics_text_layout_get_content_size(s_instr, banner_font(b), banner_text_rect(b, 200),
                                                   GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
  int left = icon_size() + g_fonts.small_h + 8;
  int h = ts.h + 6 > left ? ts.h + 6 : left;
  return clampi(h, left, b.size.h * 42 / 100);
#endif
}
static int bottom_h(void) { return PBL_IF_ROUND_ELSE(g_fonts.small_h * 2 + 18, g_fonts.title_h + 8); }
static int then_h(void) { return s_next_man != MAN_NONE && !s_arrived ? 18 : 0; }

// --- Phone messages ------------------------------------------------------------
static void handle(int cmd, DictionaryIterator *it, void *ctx) {
  switch (cmd) {
    case CMD_NAV: {
      s_have = true;
      dots_layer_set_running(s_dots, false);
      strncpy(s_instr, tuple_str(it, MESSAGE_KEY_text), sizeof(s_instr) - 1);
      strncpy(s_dist, tuple_str(it, MESSAGE_KEY_text2), sizeof(s_dist) - 1);
      strncpy(s_remain, tuple_str(it, MESSAGE_KEY_text3), sizeof(s_remain) - 1);
      strncpy(s_detail, tuple_str(it, MESSAGE_KEY_text4), sizeof(s_detail) - 1);
      s_man = tuple_int(it, MESSAGE_KEY_num, MAN_STRAIGHT);
      s_next_man = tuple_int(it, MESSAGE_KEY_idx, MAN_NONE);
      s_arrive = (time_t)tuple_int(it, MESSAGE_KEY_num2, 0);
      s_flags = tuple_int(it, MESSAGE_KEY_flags, 0);
      // arrow position is in the coordinates of a particular picture
      int img = tuple_int(it, MESSAGE_KEY_img, -1);
      if (dict_find(it, MESSAGE_KEY_width) && (img < 0 || img == g_map.img_id)) {
        s_have_puck = true;
        s_puck = GPoint(tuple_int(it, MESSAGE_KEY_width, 0), tuple_int(it, MESSAGE_KEY_height, 0));
        s_focus = GPoint(tuple_int(it, MESSAGE_KEY_stride, s_puck.x), tuple_int(it, MESSAGE_KEY_total, s_puck.y));
        s_heading = tuple_int(it, MESSAGE_KEY_offset, 0);
        s_map_heading = tuple_int(it, MESSAGE_KEY_mode, 0);
        if (dict_find(it, MESSAGE_KEY_ack)) map_apply_ack(tuple_int(it, MESSAGE_KEY_ack, 0));
      }
      bool was_arrived = s_arrived;
      s_arrived = (s_flags & NAV_ARRIVED) != 0;
      if (!s_muted) {
        if (s_arrived && !was_arrived) { vibe_now(); light_enable_interaction(); }
        else if (s_flags & NAV_ALERT_NOW) { vibe_now(); light_enable_interaction(); }
        else if (s_flags & NAV_ALERT_SOON) { vibe_soon(); light_enable_interaction(); }
      }
      layer_mark_dirty(s_canvas);
      break;
    }
    case CMD_TOAST:
      ui_toast(s_window, tuple_str(it, MESSAGE_KEY_text));
      break;
    case CMD_ERROR:
      dots_layer_set_running(s_dots, false);
      strncpy(s_instr, tuple_str(it, MESSAGE_KEY_text), sizeof(s_instr) - 1);
      s_flags = NAV_NO_GPS;
      layer_mark_dirty(s_canvas);
      break;
  }
}

// The picture was moved in place (slid, zoomed or turned): keep the arrow on the same spot
static void on_xform(const MapXform *x) {
  s_focus = map_xform_point(x, s_focus);
  s_puck = map_xform_point(x, s_puck);
  s_heading = (s_heading + x->rot + 360) % 360;
  s_map_heading = g_map.rheading;
  if (s_canvas) layer_mark_dirty(s_canvas);
}

// --- Drawing -------------------------------------------------------------------
static void draw_map(GContext *ctx, GRect b, GRect visible) {
  draw_map_placeholder(ctx, b);
  GPoint anchor = GPoint(visible.origin.x + visible.size.w / 2, visible.origin.y + visible.size.h / 2);
  GPoint origin = GPoint(anchor.x - s_focus.x + g_map.shift_x, anchor.y - s_focus.y + g_map.shift_y);
  if (g_map.bmp) {
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
    graphics_draw_bitmap_in_rect(ctx, g_map.bmp, GRect(origin.x, origin.y, g_map.w, g_map.h));
  }
  if (s_have_puck) draw_puck(ctx, GPoint(origin.x + s_puck.x, origin.y + s_puck.y), g_fonts.level >= 1 ? 9 : 7, s_heading);
  // small compass: the red tip points north
  if (s_have_puck) {
    s_compass = GPoint(visible.origin.x + PBL_IF_ROUND_ELSE(visible.size.w / 2 - 50, 14), visible.origin.y + 14);
    draw_compass(ctx, s_compass, s_map_heading);
  }
}

static void draw_banner(GContext *ctx, GRect b, int bh) {
  GColor bg = (s_flags & (NAV_NO_GPS | NAV_REROUTING)) ? PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack) : C_NAVGREEN;
  if (s_flags & NAV_TRANSIT) bg = PBL_IF_COLOR_ELSE(GColorCobaltBlue, GColorBlack);
  graphics_context_set_fill_color(ctx, bg);
  graphics_fill_rect(ctx, GRect(0, 0, b.size.w, bh), 0, GCornerNone);
  graphics_context_set_text_color(ctx, GColorWhite);
  GColor ghost = PBL_IF_COLOR_ELSE(GColorMintGreen, GColorLightGray);
  const char *text = (s_flags & NAV_REROUTING) ? "Finding a new route..." : s_instr;
#ifdef PBL_ROUND
  maneuver_draw(ctx, s_man, GPoint(b.size.w / 2 - 24, 24), 28, GColorWhite, ghost);
  ui_text(ctx, s_dist, g_fonts.title, GRect(b.size.w / 2 - 6, 6, 80, 32), GTextAlignmentLeft);
  ui_text(ctx, text, g_fonts.small_b, GRect(25, 38, b.size.w - 50, bh - 40), GTextAlignmentCenter | TXT_ELLIPSIS);
#else
  int isz = icon_size();
  int col = isz + 16;
  maneuver_draw(ctx, s_man, GPoint(col / 2, 4 + isz / 2), isz, GColorWhite, ghost);
  ui_text(ctx, s_dist, g_fonts.small_b, GRect(0, isz + 2, col, g_fonts.small_h + 4), GTextAlignmentCenter);
  GFont f = banner_font(b);
  GRect tr = banner_text_rect(b, bh + 3);
  ui_text(ctx, text, f, tr, GTextAlignmentLeft | TXT_ELLIPSIS);
#endif
  int th = then_h();
  if (th) {
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(C_NAVGREEN_DK, GColorBlack));
    GRect r = GRect(PBL_IF_ROUND_ELSE(b.size.w / 2 - 34, 0), bh, 68, th);
    graphics_fill_rect(ctx, r, 4, GCornersBottom);
    graphics_context_set_text_color(ctx, GColorWhite);
    ui_text(ctx, "Then", g_fonts.chip, GRect(r.origin.x + 6, bh - 1, 34, 16), GTextAlignmentLeft);
    maneuver_draw(ctx, s_next_man, GPoint(r.origin.x + 52, bh + th / 2), 15, GColorWhite, GColorWhite);
  }
}

static void draw_bottom(GContext *ctx, GRect b) {
  int h = bottom_h();
  GRect r = GRect(0, b.size.h - h, b.size.w, h);
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_rect(ctx, r, 0, GCornerNone);
  graphics_context_set_stroke_color(ctx, C_DIVIDER);
  graphics_draw_line(ctx, r.origin, GPoint(b.size.w, r.origin.y));
  char clk[16] = "";
  if (s_arrive) format_clock(s_arrive, clk, sizeof(clk));
#ifdef PBL_ROUND
  graphics_context_set_text_color(ctx, C_GREEN);
  ui_text(ctx, s_remain, g_fonts.small_b, GRect(24, r.origin.y + 1, b.size.w - 48, g_fonts.small_h + 4), GTextAlignmentCenter | TXT_ELLIPSIS);
  if (clk[0]) {
    char arr[32];
    snprintf(arr, sizeof(arr), "Arrive %s", clk);
    graphics_context_set_text_color(ctx, C_SUBTEXT);
    ui_text(ctx, arr, g_fonts.small, GRect(34, r.origin.y + g_fonts.small_h, b.size.w - 68, g_fonts.small_h + 4), GTextAlignmentCenter | TXT_ELLIPSIS);
  }
#else
  int rw = b.size.w - ((s_bar.open || s_menu_open) ? STRIP_W : 0);
  char t[32];
  strncpy(t, s_remain, sizeof(t) - 1);
  t[sizeof(t) - 1] = 0;
  // split "12 min · 2.3 mi" at the dot (no strstr: it costs memory)
  char *sep = NULL;
  for (char *q = t; *q && !sep; q++) if (q[0] == ' ' && !strncmp(q + 1, "·", strlen("·")) && q[1 + strlen("·")] == ' ') sep = q;
  const char *dist = "";
  if (sep) { *sep = 0; dist = s_remain + (sep - t) + strlen(" · "); }
  graphics_context_set_text_color(ctx, C_GREEN);
  GSize ts = graphics_text_layout_get_content_size(t, g_fonts.title, GRect(0, 0, b.size.w, h), GTextOverflowModeFill, GTextAlignmentLeft);
  ui_text(ctx, t, g_fonts.title, GRect(5, r.origin.y + (h - g_fonts.title_h) / 2 - 5, ts.w + 2, h), GTextAlignmentLeft);
  char right[48];
  snprintf(right, sizeof(right), "%s%s%s", dist, (dist[0] && clk[0]) ? " · " : "", clk);
  graphics_context_set_text_color(ctx, C_SUBTEXT);
  ui_text(ctx, right, g_fonts.small_b,
                     GRect(ts.w + 9, r.origin.y + (h - g_fonts.small_h) / 2 - 3, rw - ts.w - 12, g_fonts.small_h + 4), GTextAlignmentRight | TXT_ELLIPSIS);
#endif
}

static void draw_arrived(GContext *ctx, GRect b) {
  graphics_context_set_fill_color(ctx, C_NAVGREEN);
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  icon_draw(ctx, ICON_FLAG, GPoint(b.size.w / 2, b.size.h / 2 - 34), 40, GColorWhite, C_NAVGREEN);
  graphics_context_set_text_color(ctx, GColorWhite);
  ui_text(ctx, "You've arrived", g_fonts.title, GRect(4, b.size.h / 2 - 8, b.size.w - 8, 34), GTextAlignmentCenter);
  ui_text(ctx, s_instr, g_fonts.body, GRect(PBL_IF_ROUND_ELSE(20, 6), b.size.h / 2 + 24,
                     b.size.w - PBL_IF_ROUND_ELSE(40, 12), g_fonts.body_h * 2 + 8), GTextAlignmentCenter | TXT_ELLIPSIS);
}

// What Up and Down do on the first two option pages.
// Buttons: map controls + heading / hide cards + alerts. Touch: alerts + heading / map controls + hide cards.
enum { ACT_MOVE, ACT_HEADING, ACT_CARDS, ACT_ALERT };
static void page_acts(int kind, int a[2]) {
  bool touch_layout = !g_app.buttons_ui;
  if (kind == PAGE_VIEW) { a[0] = touch_layout ? ACT_ALERT : ACT_MOVE; a[1] = ACT_HEADING; }
  else if (touch_layout) { a[0] = ACT_MOVE; a[1] = ACT_CARDS; }
  else { a[0] = ACT_CARDS; a[1] = ACT_ALERT; }
}
static void act_icon(int act, IconId *icon, GColor *color) {
  switch (act) {
    case ACT_MOVE: *icon = ICON_MOVE; break;
    case ACT_HEADING: *icon = s_heading_up ? ICON_HEADING : ICON_NORTH; *color = C_BLUE; break;
    case ACT_CARDS: *icon = s_cards_hidden ? ICON_EYE : ICON_EYE_OFF; break;
    default:
      *icon = s_alert_mode == ALERT_OFF ? ICON_MUTE : (s_alert_mode == ALERT_VOICE ? ICON_SOUND : ICON_VIBRATE);
      *color = s_alert_mode == ALERT_OFF ? C_RED : (s_alert_mode == ALERT_VOICE ? C_BLUE : C_ICON);
      break;
  }
}

static void draw_menu(GContext *ctx, GRect b) {
  StripIcons ic = { .select = ICON_MORE, .select_plain = true, .page = s_menu_page, .pages = menu_pages() };
  switch (page_kind(s_menu_page)) {
    case PAGE_VIEW:
    case PAGE_CARDS: {
      int a[2];
      page_acts(page_kind(s_menu_page), a);
      act_icon(a[0], &ic.up, &ic.up_color);
      act_icon(a[1], &ic.down, &ic.down_color);
      break;
    }
    case PAGE_VOLUME:
      ic.up = ICON_VOL_UP;
      ic.down = ICON_VOL_DOWN;
      break;
    default:
      ic.up = ICON_LIST;
      ic.down = ICON_STOP;
      ic.down_color = C_RED;
      break;
  }
  draw_action_strip(ctx, b, &ic);
}

static void send_view_mode(void) {
  OutMsg m;
  comm_msg_init(&m, CMD_NAV_VIEW);
  m.idx = s_heading_up ? 1 : 0;
  m.seq = g_map.seq;
  comm_send(&m);
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  if (s_arrived) {
    draw_arrived(ctx, b);
    return;
  }
  bool cards = cards_visible();
  int top = cards ? banner_h(b) + then_h() : 0;
  int bot = cards ? bottom_h() : 0;
  GRect visible = GRect(0, top, b.size.w, b.size.h - top - bot);
  draw_map(ctx, b, visible);
  if (cards) {
    draw_banner(ctx, b, banner_h(b));
    draw_bottom(ctx, b);
  }
  if (s_bar.open) mapbar_draw(&s_bar, ctx, b);
  else if (s_menu_open) draw_menu(ctx, b);
  else draw_side_tab(ctx, b);   // always beside the middle button
  if (s_muted && !s_menu_open && !s_bar.open && cards) {
    icon_draw(ctx, ICON_MUTE, GPoint(b.size.w - PBL_IF_ROUND_ELSE(40, 12), b.size.h - bot - 12), 14, C_RED, C_BG);
  }
}

// --- Options bar -----------------------------------------------------------------
static void end_nav(void) {
  voice_set_enabled(false);
  comm_cmd(CMD_NAV_STOP);
  pop_to_home();   // straight back to the main map (route and place screens close too)
}

static void set_overview(bool on);

static void recenter_if_moved(void) {
  if (s_moved) {
    s_moved = false;
    set_overview(false);
    map_adjust(ADJ_RESET, 0, 0);
  }
}

static void menu_close(void) {
  s_menu_open = false;
  if (s_menu_timer) { app_timer_cancel(s_menu_timer); s_menu_timer = NULL; }
  recenter_if_moved();
  layer_mark_dirty(s_canvas);
}

static void menu_timeout(void *ctx) {
  s_menu_timer = NULL;
  menu_close();
}

static void menu_poke(void) {
  if (s_menu_timer) app_timer_reschedule(s_menu_timer, IDLE_MS);
  else s_menu_timer = app_timer_register(IDLE_MS, menu_timeout, NULL);
}

static void menu_open(void) {
  s_menu_open = true;
  s_menu_page = 0;
  menu_poke();
  layer_mark_dirty(s_canvas);
}

// While showing the whole route, the "my location" button becomes "back to navigation"
static void set_overview(bool on) {
  s_bar.extra_up = on ? ICON_TRIANGLE : ICON_MYLOC;
}

static void bar_cb(MapBarEvent ev, void *ctx) {
  switch (ev) {
    case MB_EV_MOVED: s_moved = true; break;
    case MB_EV_EXTRA_UP:
      s_moved = false;
      set_overview(false);
      map_adjust(ADJ_RESET, 0, 0);
      break;
    case MB_EV_EXTRA_DOWN:
      s_moved = true;
      set_overview(true);
      map_adjust(ADJ_FIT_ROUTE, 0, 0);
      break;
    case MB_EV_CLOSED: menu_close(); break;
    default: break;
  }
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void send_voice(void) {
  OutMsg m;
  comm_msg_init(&m, CMD_NAV_VOICE);
  m.idx = s_alert_mode == ALERT_VOICE ? 1 : 0;
  comm_send(&m);
}

// Vibrate -> Vibrate + voice (if the watch has a speaker) -> Off -> Vibrate ...
static void toggle_mute(void) {
  if (s_alert_mode == ALERT_VIBE) s_alert_mode = VOICE_AVAILABLE ? ALERT_VOICE : ALERT_OFF;
  else if (s_alert_mode == ALERT_VOICE) s_alert_mode = ALERT_OFF;
  else s_alert_mode = ALERT_VIBE;
  voice_set_enabled(s_alert_mode == ALERT_VOICE);
  if (VOICE_AVAILABLE) persist_write_bool(PERSIST_VOICE, s_alert_mode == ALERT_VOICE);
  send_voice();
  if (s_menu_page >= menu_pages()) s_menu_page = 0;
  ui_toast(s_window, s_alert_mode == ALERT_VOICE ? "Voice directions on" :
                     (s_alert_mode == ALERT_OFF ? "Alerts off" : "Alerts: vibrate only"));
}

static void toggle_heading(void) {
  s_heading_up = !s_heading_up;
  persist_write_bool(PERSIST_HEADING_UP, s_heading_up);
  send_view_mode();
  ui_toast(s_window, s_heading_up ? "Map turns with you" : "North up");
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void change_volume(int delta) {
  voice_set_volume(voice_volume() + delta);
  static char text[24];
  snprintf(text, sizeof(text), "Volume %d%%", voice_volume());
  ui_toast(s_window, text);
}

static void menu_action(ButtonId b) {
  menu_poke();
  if (b == BUTTON_ID_SELECT) {
    s_menu_page = (s_menu_page + 1) % menu_pages();
  } else if (page_kind(s_menu_page) == PAGE_VIEW || page_kind(s_menu_page) == PAGE_CARDS) {
    int a[2];
    page_acts(page_kind(s_menu_page), a);
    switch (a[b == BUTTON_ID_UP ? 0 : 1]) {
      case ACT_MOVE:
        // map controls replace the options bar
        if (s_menu_timer) { app_timer_cancel(s_menu_timer); s_menu_timer = NULL; }
        mapbar_open(&s_bar);
        break;
      case ACT_HEADING: toggle_heading(); break;
      case ACT_CARDS:
        s_cards_hidden = !s_cards_hidden;
        ui_toast(s_window, s_cards_hidden ? "Cards hidden until the next turn" : "Cards shown");
        break;
      default: toggle_mute(); break;
    }
  } else if (page_kind(s_menu_page) == PAGE_VOLUME) {
    change_volume(b == BUTTON_ID_UP ? 10 : -10);
  } else {
    if (b == BUTTON_ID_UP) {
      menu_close();
      list_window_push(LW_STEPS, 0);
      return;
    } else {
      end_nav();
      return;
    }
  }
  layer_mark_dirty(s_canvas);
}

// --- Buttons ---------------------------------------------------------------------
static void back_reset(void *ctx) {
  s_back_timer = NULL;
  s_back_armed = false;
}

static void press(ButtonId b) {
  if (s_arrived) {
    if (b == BUTTON_ID_SELECT || b == BUTTON_ID_BACK) end_nav();
    return;
  }
  if (mapbar_button(&s_bar, b)) return;
  if (s_menu_open) {
    if (b == BUTTON_ID_BACK) menu_close();
    else menu_action(b);
    return;
  }
  switch (b) {
    case BUTTON_ID_SELECT: menu_open(); break;
    case BUTTON_ID_UP: list_window_push(LW_STEPS, 0); break;
    case BUTTON_ID_DOWN:
      toggle_mute();
      layer_mark_dirty(s_canvas);
      break;
    case BUTTON_ID_BACK:
      if (s_back_armed) { end_nav(); return; }
      s_back_armed = true;
      ui_toast(s_window, "Press Back again to end");
      if (s_back_timer) app_timer_cancel(s_back_timer);
      s_back_timer = app_timer_register(2500, back_reset, NULL);
      break;
    default: break;
  }
}

static void up_click(ClickRecognizerRef r, void *ctx) { press(BUTTON_ID_UP); }
static void select_click(ClickRecognizerRef r, void *ctx) { press(BUTTON_ID_SELECT); }
static void down_click(ClickRecognizerRef r, void *ctx) { press(BUTTON_ID_DOWN); }
static void back_click(ClickRecognizerRef r, void *ctx) { press(BUTTON_ID_BACK); }

static void click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
}

// --- Touch -----------------------------------------------------------------------
static void map_tap(GPoint p, void *ctx) {
  // the compass: switch between north-up and the map turning with you
  int dx = p.x - s_compass.x, dy = p.y - s_compass.y;
  if (s_have_puck && dx * dx + dy * dy <= 20 * 20) {
    toggle_heading();
    return;
  }
  if (!s_menu_open && !s_bar.open) menu_open();
}

static void touch_cb(const TouchEvent *e, void *ctx) {
  GRect b = layer_get_bounds(s_canvas);
  if (e->x >= b.size.w - STRIP_W && e->type == TouchEvent_Liftoff && !e->non_navigational) {
    ButtonId btn = e->y < b.size.h / 3 ? BUTTON_ID_UP : (e->y > b.size.h * 2 / 3 ? BUTTON_ID_DOWN : BUTTON_ID_SELECT);
    if (s_bar.open || s_menu_open) { press(btn); return; }
    if (btn == BUTTON_ID_SELECT) { menu_open(); return; }
  }
  if (maptouch_event(&s_touch, e) && s_touch.dragging && !s_bar.open) {
    mapbar_open(&s_bar);   // moving the map shows the controls
  }
}

// --- Lifecycle ---------------------------------------------------------------------
static void window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  s_canvas = layer_create(b);
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
  s_dots = dots_layer_create(GRect(0, b.size.h / 2 - 12, b.size.w, 24));
  layer_add_child(root, s_dots);
  dots_layer_set_running(s_dots, true);
  mapbar_init(&s_bar, s_canvas, b.size, ICON_MYLOC, ICON_MAP, bar_cb, NULL);
  s_bar.extra_up_color = C_BLUE;
  s_bar.extra_down_color = C_ICON;
  maptouch_init(&s_touch, GRect(0, 0, b.size.w - STRIP_W, b.size.h), map_tap, NULL, &s_bar);
}

static void window_appear(Window *window) {
  comm_set_handler(handle, NULL);
  map_set_observer(s_canvas);
  map_set_xform_listener(on_xform);
  if (TOUCH_HW && g_app.touch) touch_service_subscribe(touch_cb, NULL);
}

static void window_disappear(Window *window) {
  comm_clear_handler(handle);
  map_set_observer(NULL);
  map_set_xform_listener(NULL);
  ui_toast_cancel();
  mapbar_deinit(&s_bar);
  maptouch_deinit(&s_touch);
  if (s_menu_timer) { app_timer_cancel(s_menu_timer); s_menu_timer = NULL; }
  s_menu_open = false;
  if (g_app.touch_hw) touch_service_unsubscribe();
}

static void window_unload(Window *window) {
  if (s_back_timer) app_timer_cancel(s_back_timer);
  s_back_timer = NULL;
  dots_layer_destroy(s_dots);
  layer_destroy(s_canvas);
  s_canvas = NULL;
  window_destroy(s_window);
  s_window = NULL;
}

void nav_window_push(int mode) {
  if (s_window) window_stack_remove(s_window, false);
  // free memory: only home and the route overview stay underneath
  list_windows_close_all();
  place_window_close();
  results_window_close();
  s_mode = mode;
  s_have = s_arrived = s_back_armed = false;
  s_cards_hidden = false;
  s_moved = false;
  s_have_puck = false;
  s_flags = 0;
  s_man = MAN_DEPART;
  s_next_man = MAN_NONE;
  strncpy(s_instr, "Starting navigation", sizeof(s_instr));
  s_dist[0] = s_remain[0] = s_detail[0] = 0;
  s_window = window_create();
  window_set_background_color(s_window, C_BG);
  window_set_click_config_provider(s_window, click_config);
  window_set_window_handlers(s_window, (WindowHandlers){
    .load = window_load, .appear = window_appear,
    .disappear = window_disappear, .unload = window_unload,
  });
  window_stack_push(s_window, true);
  GRect b = layer_get_bounds(window_get_root_layer(s_window));
  s_heading_up = persist_exists(PERSIST_HEADING_UP) ? persist_read_bool(PERSIST_HEADING_UP) : true;
  s_map_heading = 0;
  s_alert_mode = (VOICE_AVAILABLE && persist_exists(PERSIST_VOICE) && persist_read_bool(PERSIST_VOICE)) ? ALERT_VOICE : ALERT_VIBE;
  map_request(CMD_NAV_START, b.size.w, b.size.h, -1, mode);
  if (!s_heading_up) send_view_mode();
  voice_set_enabled(s_alert_mode == ALERT_VOICE);
  if (s_alert_mode == ALERT_VOICE) send_voice();
}
