// Pebble Maps - shared map controls: zoom / move bar and touch gestures
#include "mapbar.h"
#include "mapdata.h"
#include <stdlib.h>

#ifndef IDLE_MS
#define IDLE_MS 5000
#endif

static void timeout_cb(void *ctx) {
  MapBar *mb = ctx;
  mb->timer = NULL;
  mapbar_close(mb);
}

void mapbar_poke(MapBar *mb) {
  if (!mb->open) return;
  if (mb->timer) app_timer_reschedule(mb->timer, IDLE_MS);
  else mb->timer = app_timer_register(IDLE_MS, timeout_cb, mb);
}

void mapbar_init(MapBar *mb, Layer *layer, GSize frame, IconId extra_up, IconId extra_down,
                 MapBarCallback cb, void *ctx) {
  memset(mb, 0, sizeof(*mb));
  mb->layer = layer;
  mb->frame = frame;
  mb->extra_up = extra_up;
  mb->extra_down = extra_down;
  mb->cb = cb;
  mb->ctx = ctx;
}

void mapbar_open(MapBar *mb) {
  mb->open = true;
  mb->page = MB_PAGE_ZOOM;
  mapbar_poke(mb);
  if (mb->layer) layer_mark_dirty(mb->layer);
}

void mapbar_close(MapBar *mb) {
  if (mb->timer) {
    app_timer_cancel(mb->timer);
    mb->timer = NULL;
  }
  if (!mb->open) return;
  mb->open = false;
  if (mb->layer) layer_mark_dirty(mb->layer);
  if (mb->cb) mb->cb(MB_EV_CLOSED, mb->ctx);
}

void mapbar_toggle(MapBar *mb) {
  if (mb->open) mapbar_close(mb);
  else mapbar_open(mb);
}

ButtonId mapbar_strip_button(int y, int h) {
  return y < h / 3 ? BUTTON_ID_UP : (y > h * 2 / 3 ? BUTTON_ID_DOWN : BUTTON_ID_SELECT);
}

void mapbar_hint(GContext *ctx, int x, int y) {
  draw_card(ctx, GRect(x, y, 20, 19), 5);
  icon_draw(ctx, ICON_MOVE, GPoint(x + 10, y + 9), 13, C_TEXT, C_BG);
}

#if TOUCH_HW
void mapbar_touch_on(TouchServiceHandler cb) {
  if (g_app.touch) touch_service_subscribe(cb, NULL);
}
#endif

void mapbar_leave_(MapBar *mb, MapTouch *mt) {
  mapbar_deinit(mb);
  maptouch_deinit(mt);
  if (g_app.touch_hw) touch_service_unsubscribe();
}

void mapbar_deinit(MapBar *mb) {
  if (mb->timer) app_timer_cancel(mb->timer);
  mb->timer = NULL;
  mb->open = false;
}

static int pages(MapBar *mb) {
  return (mb->extra_up || mb->extra_down) ? 4 : 3;
}

static void moved(MapBar *mb) {
  if (mb->cb) mb->cb(MB_EV_MOVED, mb->ctx);
}

bool mapbar_button(MapBar *mb, ButtonId b) {
  if (!mb->open) return false;
  mapbar_poke(mb);
  int sx = mb->frame.w / 3, sy = mb->frame.h / 3;
  switch (b) {
    case BUTTON_ID_BACK:
      mapbar_close(mb);
      return true;
    case BUTTON_ID_SELECT:
      mb->page = (mb->page + 1) % pages(mb);
      break;
    case BUTTON_ID_UP:
      switch (mb->page) {
        case MB_PAGE_ZOOM: map_adjust(ADJ_ZOOM_IN, 0, 0); moved(mb); break;
        case MB_PAGE_DOLLY: map_adjust(ADJ_PAN, 0, -sy); moved(mb); break;
        case MB_PAGE_PAN: map_adjust(ADJ_PAN, -sx, 0); moved(mb); break;
        default: if (mb->extra_up && mb->cb) mb->cb(MB_EV_EXTRA_UP, mb->ctx); break;
      }
      break;
    case BUTTON_ID_DOWN:
      switch (mb->page) {
        case MB_PAGE_ZOOM: map_adjust(ADJ_ZOOM_OUT, 0, 0); moved(mb); break;
        case MB_PAGE_DOLLY: map_adjust(ADJ_PAN, 0, sy); moved(mb); break;
        case MB_PAGE_PAN: map_adjust(ADJ_PAN, sx, 0); moved(mb); break;
        default: if (mb->extra_down && mb->cb) mb->cb(MB_EV_EXTRA_DOWN, mb->ctx); break;
      }
      break;
    default:
      return false;
  }
  if (mb->layer) layer_mark_dirty(mb->layer);
  return true;
}

void mapbar_draw(MapBar *mb, GContext *ctx, GRect bounds) {
  if (!mb->open) return;
  StripIcons ic = { .select = ICON_MORE, .select_plain = true, .page = mb->page, .pages = pages(mb) };
  switch (mb->page) {
    case MB_PAGE_ZOOM: ic.up = ICON_ZOOM_IN; ic.down = ICON_ZOOM_OUT; break;
    case MB_PAGE_DOLLY: ic.up = ICON_UP; ic.down = ICON_DOWN; break;
    case MB_PAGE_PAN: ic.up = ICON_LEFT; ic.down = ICON_RIGHT; break;
    default:
      ic.up = mb->extra_up;
      ic.down = mb->extra_down;
      ic.up_color = mb->extra_up_color;
      ic.up_page = mb->extra_up_page;
      ic.up_pages = mb->extra_up_pages;
      ic.down_color = mb->extra_down_color;
      break;
  }
  draw_action_strip(ctx, bounds, &ic);
}

#if TOUCH_HW
// ---------------------------------------------------------------------------
// Touch: drag to pan, double-tap to zoom in, press and hold to zoom out
// ---------------------------------------------------------------------------
#define DRAG_START 8
#define DOUBLE_TAP_MS 350
#define HOLD_MS 650

static void hold_cb(void *ctx) {
  MapTouch *mt = ctx;
  mt->hold = NULL;
  if (mt->down && !mt->dragging) {
    mt->ignore = true;   // the lift-off that follows is not a tap
    map_adjust(ADJ_ZOOM_OUT, 0, 0);
    if (mt->bar) mapbar_poke(mt->bar);
    vibes_short_pulse();
  }
}

void maptouch_init(MapTouch *mt, GRect frame, MapTapCallback on_tap, void *ctx, MapBar *bar) {
  memset(mt, 0, sizeof(*mt));
  mt->frame = frame;
  mt->on_tap = on_tap;
  mt->ctx = ctx;
  mt->bar = bar;
}

void maptouch_deinit(MapTouch *mt) {
  if (mt->hold) app_timer_cancel(mt->hold);
  mt->hold = NULL;
  map_set_drag(0, 0);
}

static uint32_t now_ms(void) {
  time_t s;
  uint16_t ms;
  time_ms(&s, &ms);
  return (uint32_t)s * 1000 + ms;
}

bool maptouch_event(MapTouch *mt, const TouchEvent *e) {
  if (e->type == TouchEvent_Touchdown) {
    if (e->non_navigational || !grect_contains_point(&mt->frame, &GPoint(e->x, e->y))) {
      mt->down = false;
      return false;
    }
    mt->down = true;
    mt->dragging = false;
    mt->ignore = false;
    mt->x0 = mt->x = e->x;
    mt->y0 = mt->y = e->y;
    if (mt->hold) app_timer_cancel(mt->hold);
    mt->hold = app_timer_register(HOLD_MS, hold_cb, mt);
    return true;
  }
  if (!mt->down) return false;
  mt->x = e->x;
  mt->y = e->y;
  int dx = mt->x - mt->x0, dy = mt->y - mt->y0;
  if (!mt->dragging && (abs(dx) > DRAG_START || abs(dy) > DRAG_START)) {
    mt->dragging = true;
    if (mt->hold) { app_timer_cancel(mt->hold); mt->hold = NULL; }
  }
  if (e->type == TouchEvent_PositionUpdate) {
    if (mt->dragging) map_set_drag(dx, dy);
    if (mt->bar) mapbar_poke(mt->bar);
    return true;
  }
  // Liftoff
  mt->down = false;
  if (mt->hold) { app_timer_cancel(mt->hold); mt->hold = NULL; }
  if (mt->bar) mapbar_poke(mt->bar);
  if (mt->dragging) {
    map_set_drag(0, 0);
    map_adjust(ADJ_PAN, -dx, -dy);
    if (mt->bar && mt->bar->cb) mt->bar->cb(MB_EV_MOVED, mt->bar->ctx);
    return true;
  }
  if (mt->ignore) return true;
  uint32_t t = now_ms();
  if (t - mt->last_tap_ms < DOUBLE_TAP_MS && abs(mt->last_tap_x - e->x) < 30 && abs(mt->last_tap_y - e->y) < 30) {
    mt->last_tap_ms = 0;
    map_adjust(ADJ_ZOOM_IN, 0, 0);
    if (mt->bar && mt->bar->cb) mt->bar->cb(MB_EV_MOVED, mt->bar->ctx);
    return true;
  }
  mt->last_tap_ms = t;
  mt->last_tap_x = e->x;
  mt->last_tap_y = e->y;
  if (mt->on_tap) mt->on_tap(GPoint(e->x, e->y), mt->ctx);
  return true;
}
#endif  // TOUCH_HW
