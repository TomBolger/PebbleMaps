// Pebble Maps - screens
#pragma once
#include "common.h"
#include "ui.h"

// Home: current location map with search bar
void home_window_push(void);
Window *home_window_get(void);
void pop_to_home(void);

// Results (search / nearby): map with pins + bottom card, toggle to list
void results_window_push_search(const char *query);
void results_window_push_nearby(int category);

// Generic lists
typedef enum { LW_FAVS, LW_CATEGORIES, LW_MODES, LW_STEPS, LW_SETMODE, LW_MAPPLACES } ListKind;
// Places on the main map: preferences (see main.c)
void poi_prefs_load(void);
void poi_prefs_save(void);   // also tells the phone
void poi_prefs_send(void);
// Touch settings from the phone (bit 0 touch off, bit 1 button layout)
#define PERSIST_TOUCH_PREFS 9
void touch_prefs_apply(int prefs);
// Seconds of non-use before the home screen's side buttons slide away (0 = never; kept on the watch)
#define PERSIST_HIDE_S 10
void hide_prefs_apply(int secs);
// A text page filled in by the phone (sends cmd with idx; shows CMD_INFO_DATA)
void text_page_push(IconId icon, const char *title, int cmd, int idx);
void list_window_push(ListKind kind, int arg);

// Place card
void place_window_push(int src, int idx, const char *title);
void place_window_push_suggestion(int idx, const char *title, int request_id);
int place_default_mode(void);

// Place info: photos, rating, hours, reviews (src = SRC_*, title may be NULL)
void info_window_push(int src, int idx, const char *title);

// Route overview
void route_window_push(int mode);

// Turn-by-turn navigation
void nav_window_push(int mode);

// Info / error message screen
void msg_window_push(IconId icon, const char *title, const char *body, bool pop_on_configured);
void show_error(DictionaryIterator *it);

// Explore categories (shared with JS by index)
#define CATEGORY_COUNT 12
extern const char *const CATEGORY_NAMES[CATEGORY_COUNT];
extern const uint8_t CATEGORY_ICONS[CATEGORY_COUNT];
extern const uint8_t CATEGORY_COLORS[CATEGORY_COUNT];

// List message parsing: fields separated by 0x1F, items by 0x1E.
// Allocate exactly as many items as the message holds (NULL if none)
ListItem *alloc_list(const char *packed, int max, int *count);
// Close screens between home and the route overview (frees memory for navigation)
void results_window_close(void);
void place_window_close(void);
void list_windows_close_all(void);
void list_windows_trim(void);
