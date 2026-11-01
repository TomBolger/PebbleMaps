// Pebble Maps - drawing toolkit: colors, fonts, icons, pins, loading dots
#pragma once
#include "common.h"

// Google-ish palette mapped to the Pebble 64-color palette.
#define C_BLUE      PBL_IF_COLOR_ELSE(GColorBlueMoon, GColorBlack)
#define C_BLUE_DARK PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorBlack)
#define C_BLUE_LT   PBL_IF_COLOR_ELSE(GColorPictonBlue, GColorLightGray)
#define C_RED       PBL_IF_COLOR_ELSE(GColorRed, GColorBlack)
#define C_RED_DARK  PBL_IF_COLOR_ELSE(GColorDarkCandyAppleRed, GColorBlack)
#define C_YELLOW    PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack)
#define C_GREEN     PBL_IF_COLOR_ELSE(GColorIslamicGreen, GColorBlack)
#define C_NAVGREEN  PBL_IF_COLOR_ELSE(GColorJaegerGreen, GColorBlack)
#define C_NAVGREEN_DK PBL_IF_COLOR_ELSE(GColorDarkGreen, GColorBlack)
#define C_TEXT      PBL_IF_COLOR_ELSE(GColorBlack, GColorBlack)
#define C_SUBTEXT   GColorBlack
#define C_ICON      PBL_IF_COLOR_ELSE(GColorBlack, GColorBlack)
#define C_STAR      PBL_IF_COLOR_ELSE(GColorYellow, GColorBlack)
#define C_STAR_EDGE PBL_IF_COLOR_ELSE(GColorWindsorTan, GColorBlack)
#define C_PIN       PBL_IF_COLOR_ELSE(GColorRed, GColorBlack)
#define C_BG        GColorWhite
#define C_DIVIDER   PBL_IF_COLOR_ELSE(GColorLightGray, GColorBlack)
#define C_MAPBG     PBL_IF_COLOR_ELSE(GColorWhite, GColorWhite)

typedef enum {
  ICON_NONE = 0,
  ICON_MIC, ICON_STAR, ICON_STAR_OUTLINE, ICON_EXPLORE, ICON_DIRECTIONS,
  ICON_LIST, ICON_NAV, ICON_PIN, ICON_CLOCK, ICON_MYLOC, ICON_FLAG,
  ICON_SEARCH, ICON_PLUS, ICON_CAR, ICON_WALK, ICON_BIKE, ICON_BUS,
  ICON_TRAIN, ICON_FERRY, ICON_WARNING, ICON_PHONE, ICON_COFFEE,
  ICON_FOOD, ICON_GAS, ICON_CART, ICON_TREE, ICON_BED, ICON_ATM,
  ICON_PHARMACY, ICON_PLUG, ICON_PARKING, ICON_BOOK, ICON_MUTE,
  ICON_SOUND, ICON_CLOSE, ICON_ZOOM_IN, ICON_ZOOM_OUT, ICON_MAP,
  ICON_KEY, ICON_MOVE, ICON_MORE, ICON_UP, ICON_DOWN, ICON_LEFT, ICON_RIGHT,
  ICON_EYE, ICON_EYE_OFF, ICON_STOP, ICON_NORTH, ICON_HEADING, ICON_TRIANGLE, ICON_VIBRATE, ICON_VOL_UP, ICON_VOL_DOWN,
  ICON_INFO, ICON_GLOBE, ICON_POI_NAMES, ICON_BAR, ICON_NORTH_UP, ICON_GEAR, ICON_COUNT
} IconId;

// Fonts, chosen by the text-size setting (level 0 = standard, 1 = large, 2 = extra large)
typedef struct {
  int level;
  GFont small, small_b;   // labels and secondary lines
  GFont body, body_b;     // list titles, main text
  GFont title;            // card titles
  GFont big;              // big numbers (distance, time)
  GFont chip;             // tiny chips (always small)
  int small_h, body_h, title_h, big_h;
} Fonts;
extern Fonts g_fonts;

void ui_init(void);
// level: 0..2, or 3 = automatic for this watch
void ui_set_text_level(int level);
void ui_save_text_level(int level);

// Icons: size is the icon's bounding box edge in pixels, centered at c.
void icon_draw(GContext *ctx, IconId id, GPoint c, int size, GColor fg, GColor bg);
IconId icon_for_mode(int mode);
// Maneuver arrow, centered at c
void maneuver_draw(GContext *ctx, int code, GPoint c, int size, GColor fg, GColor ghost);

// Map glyphs
void draw_pin(GContext *ctx, GPoint tip, int r, bool selected);
void draw_me_dot(GContext *ctx, GPoint c, int r);
void draw_puck(GContext *ctx, GPoint c, int r, int heading_deg);
// A place on the map (small colored dot by category, POI_*)
void draw_poi(GContext *ctx, GPoint c, int category);
// A place with its name: a pill around the dot with the name beside it (readable over the map).
// Returns the pill's rectangle.
GRect poi_label_rect(GPoint c, const char *name, bool left);
void draw_poi_label(GContext *ctx, GPoint c, int category, const char *name, GRect pill);
GColor poi_color(int category);
// Place kinds on the map (MAP_CAT_*): name, icon, color
const char *map_cat_name(int cat);
IconId map_cat_icon(int cat);
GColor map_cat_color(int cat);
// Small compass: the red tip points north on a map turned so map_heading is up
void draw_compass(GContext *ctx, GPoint c, int map_heading);

// Google "FAB": a filled circle with a white icon
void draw_fab(GContext *ctx, GPoint c, int r, IconId icon, GColor fill);

// Loading dots (blue, red, yellow, green bouncing)
typedef struct DotsLayer DotsLayer;
Layer *dots_layer_create(GRect frame);
void dots_layer_set_running(Layer *layer, bool running);
void dots_layer_destroy(Layer *layer);

// Simple action strip on the right edge (Up / Select / Down)
#if defined(PBL_PLATFORM_EMERY)
#define STRIP_W 36
#else
#define STRIP_W PBL_IF_ROUND_ELSE(40, 30)
#endif
typedef struct {
  IconId up, select, down;
  GColor select_color;   // FAB color for the middle button
  GColor up_color, down_color;  // icon colors (clear = default)
  bool select_plain;     // draw the middle icon without a FAB circle
  int page, pages;       // page dots under the middle icon (pages > 1)
  int up_page, up_pages; // option dots under the top icon (up_pages > 1)
} StripIcons;
// Small black half-circle on the right edge: "press Select for options"
void draw_side_tab(GContext *ctx, GRect bounds);
// Half circle with an "i" on a list row's right edge (Select for more info)
void draw_info_tab(GContext *ctx, GRect cell, bool hl);
void draw_action_strip(GContext *ctx, GRect bounds, const StripIcons *icons);

// Toast banner (one at a time, attached to a window)
void ui_toast(Window *window, const char *text);
void ui_toast_cancel(void);

// Draw text: align is a GTextAlignment, optionally | TXT_ELLIPSIS (else clipped)
#define TXT_ELLIPSIS 4
void ui_text(GContext *ctx, const char *text, GFont font, GRect box, int align);

// Format a unix time as local clock text ("2:43 PM" or "14:43")
void format_clock(time_t t, char *buf, size_t len);

// List dividers: a thin line with a few pixels of white above and below,
// so the highlighted row never touches it
int16_t ui_separator_h(MenuLayer *menu, MenuIndex *index, void *ctx);
void ui_draw_separator(GContext *ctx, const Layer *cell, MenuIndex *index, void *cb);

// Rounded "card" background with subtle border
void draw_card(GContext *ctx, GRect r, int radius);

// Placeholder map tile pattern while loading
void draw_map_placeholder(GContext *ctx, GRect r);

// Vibrations
void vibe_soon(void);
void vibe_now(void);
