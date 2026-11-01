// Pebble Maps - friendly message screens (setup, errors)
#include "windows.h"
#include "comm.h"

typedef struct {
  Window *window;
  ScrollLayer *scroll;
  Layer *icon_layer;
  TextLayer *title;
  TextLayer *body;
  IconId icon;
  bool pop_on_configured;
  bool info;              // low-memory place info page (text only)
  char title_text[48];
  char *body_text;
} MsgWin;

static MsgWin *s_setup;  // only one setup screen at a time

static void icon_update(Layer *layer, GContext *ctx) {
  MsgWin *mw = *(MsgWin **)layer_get_data(layer);
  GRect b = layer_get_bounds(layer);
  GColor c = mw->icon == ICON_WARNING ? C_RED : C_BLUE;
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorCeleste, GColorWhite));
  if (mw->icon == ICON_WARNING) ui_fill_circle(ctx, PBL_IF_COLOR_ELSE(GColorMelon, GColorWhite), GPoint(b.size.w / 2, b.size.h / 2), b.size.h / 2);
  icon_draw(ctx, mw->icon, GPoint(b.size.w / 2, b.size.h / 2), b.size.h * 6 / 10, c, PBL_IF_COLOR_ELSE(GColorCeleste, GColorWhite));
}

static void layout(MsgWin *mw);

static void handle(int cmd, DictionaryIterator *it, void *ctx) {
  MsgWin *mw = ctx;
  if (mw->info && cmd == CMD_INFO_DATA) {
    strncpy(mw->title_text, tuple_str(it, MESSAGE_KEY_text), sizeof(mw->title_text) - 1);
    const char *body = tuple_str(it, MESSAGE_KEY_list);
    char *nb = malloc(strlen(body) + 1);
    if (nb) {
      strcpy(nb, body);
      free(mw->body_text);
      mw->body_text = nb;
    }
    text_layer_set_text(mw->title, mw->title_text);
    text_layer_set_text(mw->body, mw->body_text);
    layout(mw);
    return;
  }
  if (mw->info && cmd == CMD_ERROR) {
    window_stack_remove(mw->window, false);
    show_error(it);
    return;
  }

  if (cmd == CMD_STATUS && mw->pop_on_configured && g_app.configured) {
    window_stack_remove(mw->window, true);
  }
}

// Size the title and body to their text
static void layout(MsgWin *mw) {
  GRect b = layer_get_bounds(window_get_root_layer(mw->window));
  int pad = PBL_IF_ROUND_ELSE(22, 8);
  int y = PBL_IF_ROUND_ELSE(14, 8) + 48;
  layer_set_frame(text_layer_get_layer(mw->title), GRect(pad, y, b.size.w - pad * 2, 200));
  GSize ts = text_layer_get_content_size(mw->title);
  layer_set_frame(text_layer_get_layer(mw->title), GRect(pad, y, b.size.w - pad * 2, ts.h + 6));
  y += ts.h + 6;
  layer_set_frame(text_layer_get_layer(mw->body), GRect(pad, y, b.size.w - pad * 2, 3000));
  GSize bs = text_layer_get_content_size(mw->body);
  layer_set_frame(text_layer_get_layer(mw->body), GRect(pad, y, b.size.w - pad * 2, bs.h + 8));
  y += bs.h + PBL_IF_ROUND_ELSE(40, 16);
  scroll_layer_set_content_size(mw->scroll, GSize(b.size.w, y));
}

static void window_load(Window *window) {
  MsgWin *mw = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  mw->scroll = scroll_layer_create(b);
  scroll_layer_set_click_config_onto_window(mw->scroll, window);
#if defined(PBL_ROUND) && !TOUCH_HW
  scroll_layer_set_paging(mw->scroll, true);   // Time Round: a page at a time (Round 2's bigger screen scrolls in steps)
#endif
  mw->icon_layer = layer_create_with_data(GRect(b.size.w / 2 - 22, PBL_IF_ROUND_ELSE(14, 8), 44, 44), sizeof(MsgWin *));
  *(MsgWin **)layer_get_data(mw->icon_layer) = mw;
  layer_set_update_proc(mw->icon_layer, icon_update);
  scroll_layer_add_child(mw->scroll, mw->icon_layer);
  mw->title = text_layer_create(GRect(0, 0, b.size.w, 60));
  text_layer_set_font(mw->title, g_fonts.title);
  text_layer_set_text_alignment(mw->title, GTextAlignmentCenter);
  text_layer_set_text(mw->title, mw->title_text);
  scroll_layer_add_child(mw->scroll, text_layer_get_layer(mw->title));
  mw->body = text_layer_create(GRect(0, 0, b.size.w, 2000));
  text_layer_set_font(mw->body, mw->info ? g_fonts.small : g_fonts.body);
  text_layer_set_text_alignment(mw->body, mw->info ? PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft) : GTextAlignmentCenter);
  text_layer_set_text(mw->body, mw->body_text);
  scroll_layer_add_child(mw->scroll, text_layer_get_layer(mw->body));
  layout(mw);
  layer_add_child(root, scroll_layer_get_layer(mw->scroll));
}

static void window_appear(Window *window) {
  comm_set_handler(handle, window_get_user_data(window));
}

static void window_disappear(Window *window) {
  comm_clear_handler(handle);
}

static void window_unload(Window *window) {
  MsgWin *mw = window_get_user_data(window);
  text_layer_destroy(mw->title);
  text_layer_destroy(mw->body);
  layer_destroy(mw->icon_layer);
  scroll_layer_destroy(mw->scroll);
  if (s_setup == mw) s_setup = NULL;
  free(mw->body_text);
  free(mw);
  window_destroy(window);
}

static MsgWin *msg_create(IconId icon, const char *title, const char *body) {
  MsgWin *mw = calloc(1, sizeof(MsgWin));
  if (!mw) return NULL;
  mw->body_text = malloc(strlen(body) + 1);
  if (!mw->body_text) { free(mw); return NULL; }
  strcpy(mw->body_text, body);
  mw->icon = icon;
  strncpy(mw->title_text, title, sizeof(mw->title_text) - 1);
  return mw;
}

static bool msg_show(MsgWin *mw) {
  mw->window = window_create();
  if (!mw->window) {   // out of memory: give up quietly
    free(mw->body_text);
    free(mw);
    return false;
  }
  window_set_user_data(mw->window, mw);
  window_set_background_color(mw->window, C_BG);
  window_set_window_handlers(mw->window, (WindowHandlers){
    .load = window_load, .appear = window_appear,
    .disappear = window_disappear, .unload = window_unload,
  });
  window_stack_push(mw->window, true);
  return true;
}

void msg_window_push(IconId icon, const char *title, const char *body, bool pop_on_configured) {
  if (pop_on_configured && s_setup) return;
  MsgWin *mw = msg_create(icon, title, body);
  if (!mw) return;
  mw->pop_on_configured = pop_on_configured;
  if (!msg_show(mw)) return;
  if (pop_on_configured) s_setup = mw;
}

// A page whose text comes from the phone (transit schedules; place info on Pebble Time / Time Round)
static void remote_page(IconId icon, const char *title, int cmd, int idx, int mode) {
  if (heap_bytes_free() < 6000) list_windows_trim();   // Pebble Time / Time Round
  if (heap_bytes_free() < 4000) list_windows_close_all();   // the list this page opened from too
  MsgWin *mw = msg_create(icon, title ? title : "", "Loading...");
  if (!mw) return;
  mw->info = true;
  if (!msg_show(mw)) return;
  OutMsg m;
  comm_msg_init(&m, cmd);
  m.idx = idx;
  if (mode >= 0) m.mode = mode;
  comm_send(&m);
}

void text_page_push(IconId icon, const char *title, int cmd, int idx) {
  remote_page(icon, title, cmd, idx, -1);
}

#if PM_LOWMEM
// Pebble Time / Time Round: the place info page as text (rating, hours, reviews)
void info_window_push(int src, int idx, const char *title) {
  if (heap_bytes_free() < 4000) list_windows_close_all();
  remote_page(ICON_INFO, title, CMD_INFO, idx, src);
}
#endif

void show_error(DictionaryIterator *it) {
  int code = tuple_int(it, MESSAGE_KEY_num, ERR_API);
  const char *text = tuple_str(it, MESSAGE_KEY_text);
  const char *title = tuple_str(it, MESSAGE_KEY_text2);
  IconId icon = ICON_WARNING;
  if (!title[0]) {
    switch (code) {
      case ERR_NO_KEY: title = "Setup needed"; icon = ICON_KEY; break;
      case ERR_NO_LOCATION: title = "Where are you?"; icon = ICON_MYLOC; break;
      case ERR_NETWORK: title = "No connection"; icon = ICON_PHONE; break;
      case ERR_NO_RESULTS: title = "Nothing found"; icon = ICON_SEARCH; break;
      default: title = "Something went wrong"; break;
    }
  }
  if (code == ERR_NO_LOCATION) icon = ICON_MYLOC;
  msg_window_push(icon, title, text[0] ? text : "Please try again.", code == ERR_NO_KEY);
}
