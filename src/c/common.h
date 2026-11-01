// Pebble Maps - shared definitions
#pragma once
#include <pebble.h>

// ---------------------------------------------------------------------------
// Commands (must match src/pkjs/protocol.js)
// ---------------------------------------------------------------------------
enum {
  // watch -> phone
  CMD_HELLO = 1,
  CMD_HOME_MAP = 2,
  CMD_SEARCH = 3,
  CMD_NEARBY = 4,
  CMD_SELECT = 5,
  CMD_MODE_TIMES = 6,
  CMD_ROUTE = 7,
  CMD_NAV_START = 8,
  CMD_NAV_STOP = 9,
  CMD_FAV_LIST = 10,
  CMD_FAV_TOGGLE = 11,
  CMD_FAV_DELETE = 12,
  CMD_FAV_SETMODE = 13,
  CMD_SAVE_HERE = 14,
  CMD_RESULTS_MAP = 15,
  CMD_STEPS = 16,
  CMD_PLACE_MAP = 17,
  CMD_ROUTE_MAP = 18,
  CMD_MUTE = 19,
  CMD_CANCEL = 20,
  CMD_MAP_ADJUST = 21,
  CMD_NAV_VIEW = 22,
  CMD_AUTOCOMPLETE = 23,
  CMD_NAV_VOICE = 24,
  CMD_INFO = 25,
  CMD_PHOTO = 26,
  CMD_POI_MODE = 27,
  CMD_FOLLOW_MODE = 28,
  CMD_TRANSIT_INFO = 29,
  // phone -> watch
  CMD_STATUS = 100,
  CMD_LIST = 101,
  CMD_MAP_BEGIN = 102,
  CMD_MAP_CHUNK = 103,
  CMD_MARKERS = 104,
  CMD_PLACE = 105,
  CMD_ROUTE_INFO = 106,
  CMD_NAV = 107,
  CMD_ERROR = 108,
  CMD_BUSY = 109,
  CMD_TOAST = 110,
  CMD_VOICE = 111,
  CMD_INFO_DATA = 112,
  CMD_VOICE_DONE = 30,
};

// Pebble Time and Time Round: 64 KB for code and a full-color map picture, so a few
// extras (the photo info page) use a lighter version there
#if defined(PBL_PLATFORM_BASALT) || defined(PBL_PLATFORM_CHALK)
#define PM_LOWMEM 1
#else
#define PM_LOWMEM 0
#endif

// Watches with a touch screen (Pebble Time 2, Round 2)
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
#define TOUCH_HW 1
#else
#define TOUCH_HW 0
#endif

// Travel modes
enum { MODE_DRIVE = 0, MODE_WALK = 1, MODE_BIKE = 2, MODE_TRANSIT = 3, MODE_COUNT = 4 };
#define MODE_USE_DEFAULT 9

// List kinds
enum { LIST_RESULTS = 0, LIST_FAVS = 1, LIST_STEPS = 2, LIST_MODES = 3, LIST_RECENTS = 4, LIST_SUGGEST = 5 };

// Destination sources for CMD_SELECT
enum { SRC_RESULTS = 0, SRC_FAVS = 1, SRC_RECENTS = 2, SRC_SUGGEST = 3, SRC_POI = 4, SRC_DEST = 5 };

// Error codes
enum { ERR_NO_KEY = 1, ERR_NO_LOCATION = 2, ERR_API = 3, ERR_NETWORK = 4, ERR_NO_RESULTS = 5 };

// Map formats sent in CMD_HELLO
enum { FMT_1BIT = 0, FMT_4BIT = 1 };

// Maneuver codes (must match protocol.js)
enum {
  MAN_STRAIGHT = 0, MAN_SLIGHT_LEFT, MAN_LEFT, MAN_SHARP_LEFT, MAN_UTURN_LEFT,
  MAN_SLIGHT_RIGHT, MAN_RIGHT, MAN_SHARP_RIGHT, MAN_UTURN_RIGHT,
  MAN_RAMP_LEFT, MAN_RAMP_RIGHT, MAN_MERGE, MAN_FORK_LEFT, MAN_FORK_RIGHT,
  MAN_FERRY, MAN_ROUNDABOUT_LEFT, MAN_ROUNDABOUT_RIGHT, MAN_DEPART, MAN_ARRIVE,
  MAN_BUS, MAN_TRAIN, MAN_SUBWAY, MAN_TRAM, MAN_WALK, MAN_NONE = 255
};

// Map adjust actions
enum { ADJ_ZOOM_IN = 0, ADJ_ZOOM_OUT = 1, ADJ_PAN = 2, ADJ_RESET = 3, ADJ_FIT_ROUTE = 4 };

// NAV flags
#define NAV_ALERT_SOON  (1 << 0)
#define NAV_ALERT_NOW   (1 << 1)
#define NAV_ARRIVED     (1 << 2)
#define NAV_REROUTING   (1 << 3)
#define NAV_TRANSIT     (1 << 4)
#define NAV_NO_GPS      (1 << 5)
#define NAV_NEAR        (1 << 7)   // close to a maneuver: show the cards

// Marker kinds
enum { MK_ME = 0, MK_PIN = 1, MK_START = 2, MK_DEST = 3, MK_ARROW = 4, MK_POI = 5 };
// Places on the map: kind MK_POI + category
// Place kinds on the map: the Places menu's 12 categories, then Bars and Other
#define MAP_CAT_COUNT 14
#define MAP_CAT_OTHER 13

// List item (shared by all list windows). The text lives in the same memory
// block as the list (see alloc_list), so items are as long as they need to be.
typedef struct {
  const char *title;
  const char *sub;
  uint8_t icon;   // icon or maneuver/mode code
  uint8_t extra;  // e.g. mode for favorites
} ListItem;

// App-wide state
typedef struct {
  bool configured;     // phone reports an API key
  bool status_known;   // got CMD_STATUS at least once
  uint8_t default_mode;
  bool imperial;
  bool vibrate;
  bool has_mic;
  bool touch;          // touch input on (watch has it, and it isn't switched off in Settings)
  bool touch_hw;       // the watch has a touch screen
  bool buttons_ui;     // button layout (no touch, or chosen in Settings)
  bool dark_map;       // dark map colors (color watches)
  bool pois_on;        // places drawn on the main map
  bool poi_names;      // ...with their names
  uint16_t poi_mask;   // which kinds of places (bit per MAP_CAT)
  bool open_now;       // open places only: on the map, in Places lists and in searches
  uint8_t hide_s;      // seconds before the home screen's side buttons hide (0 = never)
  uint16_t inbox_size;
} AppState;

extern AppState g_app;

// Helpers
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
const char *mode_name(int mode);
