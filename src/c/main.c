// Pebble Maps - entry point
#include "common.h"
#include "comm.h"
#include "mapdata.h"
#include "ui.h"
#include "windows.h"

AppState g_app = { .default_mode = MODE_DRIVE, .vibrate = true };

const char *const CATEGORY_NAMES[CATEGORY_COUNT] = {
  "Restaurants", "Coffee", "Gas", "Groceries", "Parks", "Hotels",
  "ATMs", "Pharmacies", "Transit", "EV charging", "Parking", "Libraries",
};
const uint8_t CATEGORY_ICONS[CATEGORY_COUNT] = {
  ICON_FOOD, ICON_COFFEE, ICON_GAS, ICON_CART, ICON_TREE, ICON_BED,
  ICON_ATM, ICON_PHARMACY, ICON_BUS, ICON_PLUG, ICON_PARKING, ICON_BOOK,
};
#ifdef PBL_COLOR
const uint8_t CATEGORY_COLORS[CATEGORY_COUNT] = {
  GColorOrangeARGB8, GColorWindsorTanARGB8, GColorBlueMoonARGB8, GColorBlueMoonARGB8,
  GColorIslamicGreenARGB8, GColorPurpleARGB8, GColorDarkGreenARGB8, GColorRedARGB8,
  GColorBlueMoonARGB8, GColorJaegerGreenARGB8, GColorCobaltBlueARGB8, GColorImperialPurpleARGB8,
};
#else
const uint8_t CATEGORY_COLORS[CATEGORY_COUNT] = { 0 };
#endif

const char *map_cat_name(int cat) {
  if (cat < CATEGORY_COUNT) return CATEGORY_NAMES[cat];
  return cat == 12 ? "Bars" : "Other places";
}
IconId map_cat_icon(int cat) {
  if (cat < CATEGORY_COUNT) return CATEGORY_ICONS[cat];
  return cat == 12 ? ICON_BAR : ICON_PIN;
}
GColor map_cat_color(int cat) {
#ifdef PBL_COLOR
  if (cat < CATEGORY_COUNT) return (GColor){ .argb = CATEGORY_COLORS[cat] };
  return cat == 12 ? GColorFashionMagenta : GColorDarkGray;
#else
  return GColorBlack;
#endif
}

// --- Touch: Settings can switch touch off, or ask for the button layout -------------
// prefs: bit 0 = touch off, bit 1 = button layout
void touch_prefs_apply(int prefs) {
  bool off = prefs & 1;
  g_app.touch = g_app.touch_hw && !off;
  g_app.buttons_ui = !g_app.touch || (prefs & 2);
#if TOUCH_HW
  if (g_app.touch_hw) app_touch_navigation_enable(g_app.touch);
#endif
  if (!persist_exists(PERSIST_TOUCH_PREFS) || persist_read_int(PERSIST_TOUCH_PREFS) != prefs) {
    persist_write_int(PERSIST_TOUCH_PREFS, prefs);
  }
}

void hide_prefs_apply(int secs) {
  g_app.hide_s = secs;
  if (!persist_exists(PERSIST_HIDE_S) || persist_read_int(PERSIST_HIDE_S) != secs) persist_write_int(PERSIST_HIDE_S, secs);
}

// --- Places on the main map: on/off, names, which kinds (kept on the watch) ------
#define PERSIST_POI_PREFS 7
void poi_prefs_load(void) {
  g_app.pois_on = true;
  g_app.poi_names = !PM_LOWMEM;
  g_app.poi_mask = (1 << 0) | (1 << 1) | (1 << 12) | (1 << MAP_CAT_OTHER);   // restaurants, coffee, bars, other
  g_app.open_now = true;
  if (persist_exists(PERSIST_POI_PREFS)) {
    int v = persist_read_int(PERSIST_POI_PREFS);
    g_app.pois_on = v & 1;
    g_app.poi_names = (v & 2) && !PM_LOWMEM;
    g_app.poi_mask = (v >> 2) & 0x3FFF;
    // "open now" switches came later: stored inverted so older saves read as on
    g_app.open_now = !(v & (1 << 16));
  }
}
void poi_prefs_send(void) {
  OutMsg m;
  comm_msg_init(&m, CMD_POI_MODE);
  m.idx = (g_app.pois_on ? 1 : 0) | (g_app.poi_names ? 2 : 0) | (g_app.open_now ? 4 : 0);
  m.num = g_app.poi_mask;
  m.seq = g_map.seq;
  comm_send(&m);
}
void poi_prefs_save(void) {
  persist_write_int(PERSIST_POI_PREFS, (g_app.pois_on ? 1 : 0) | (g_app.poi_names ? 2 : 0) | (g_app.poi_mask << 2) |
                    (g_app.open_now ? 0 : 1 << 16));
  poi_prefs_send();
}

const char *mode_name(int mode) {
  switch (mode) {
    case MODE_WALK: return "Walk";
    case MODE_BIKE: return "Bike";
    case MODE_TRANSIT: return "Transit";
    default: return "Drive";
  }
}

int count_list(const char *packed) {
  if (!packed || !*packed) return 0;
  int n = 1;
  for (const char *p = packed; *p; p++) if (*p == 0x1E) n++;
  return n;
}

// One memory block: the items, followed by their text
ListItem *alloc_list(const char *packed, int max, int *count) {
  int n = count_list(packed);
  if (n > max) n = max;
  *count = 0;
  if (n == 0) return NULL;
  size_t text = strlen(packed) + 2 * n + 2;
  ListItem *items = calloc(1, n * sizeof(ListItem) + text);
  if (!items) return NULL;
  char *blob = (char *)(items + n);
  const char *p = packed;
  int k = 0;
  while (*p && k < n) {
    ListItem *it = &items[k];
    int field = 0;
    char num[6] = {0};
    int ni = 0;
    it->title = blob;
    it->sub = "";
    while (*p && *p != 0x1E) {
      if (*p == 0x1F) {
        if (field == 0) { *blob++ = 0; it->sub = blob; }
        else if (field == 1) *blob++ = 0;
        else if (field == 2) it->icon = atoi(num);
        field++;
        memset(num, 0, sizeof(num));
        ni = 0;
      } else if (field <= 1) {
        *blob++ = *p;
      } else if (ni < 5) {
        num[ni++] = *p;
      }
      p++;
    }
    if (field <= 1) *blob++ = 0;
    if (field == 2) it->icon = atoi(num);
    else if (field == 3) it->extra = atoi(num);
    k++;
    if (*p == 0x1E) p++;
  }
  *count = k;
  return items;
}

static void init(void) {
  ui_init();
#if defined(PBL_PLATFORM_APLITE)
  g_app.has_mic = false;
#else
  g_app.has_mic = true;
#endif
  g_app.touch_hw = TOUCH_HW && touch_service_is_enabled();
  touch_prefs_apply(persist_exists(PERSIST_TOUCH_PREFS) ? persist_read_int(PERSIST_TOUCH_PREFS) : 0);
  comm_init();
  g_app.hide_s = persist_exists(PERSIST_HIDE_S) ? persist_read_int(PERSIST_HIDE_S) : 10;
  poi_prefs_load();
  map_reserve();
  home_window_push();
}

static void deinit(void) {
  map_release();
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
