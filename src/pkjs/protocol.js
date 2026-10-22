// Pebble Maps - message protocol shared with the watch (see src/c/common.h)
module.exports = {
  CMD: {
    HELLO: 1, HOME_MAP: 2, SEARCH: 3, NEARBY: 4, SELECT: 5, MODE_TIMES: 6,
    ROUTE: 7, NAV_START: 8, NAV_STOP: 9, FAV_LIST: 10, FAV_TOGGLE: 11,
    FAV_DELETE: 12, FAV_SETMODE: 13, SAVE_HERE: 14, RESULTS_MAP: 15, STEPS: 16,
    PLACE_MAP: 17, ROUTE_MAP: 18, MUTE: 19, CANCEL: 20, MAP_ADJUST: 21, NAV_VIEW: 22, AUTOCOMPLETE: 23, NAV_VOICE: 24,
    INFO: 25, PHOTO: 26, POI_MODE: 27, FOLLOW_MODE: 28, TRANSIT_INFO: 29, VOICE_DONE: 30,
    STATUS: 100, LIST: 101, MAP_BEGIN: 102, MAP_CHUNK: 103, MARKERS: 104,
    PLACE: 105, ROUTE_INFO: 106, NAV: 107, ERROR: 108, BUSY: 109, TOAST: 110, VOICE: 111, INFO_DATA: 112,
    API_SEARCH: 120, API_NAVIGATE: 121
  },
  MODE: { DRIVE: 0, WALK: 1, BIKE: 2, TRANSIT: 3 },
  MODE_USE_DEFAULT: 9,
  MODE_NAMES: ['Drive', 'Walk', 'Bike', 'Transit'],
  // Google Routes API travel modes
  ROUTES_MODE: ['DRIVE', 'WALK', 'BICYCLE', 'TRANSIT'],
  LIST: { RESULTS: 0, FAVS: 1, STEPS: 2, MODES: 3, RECENTS: 4, SUGGEST: 5 },
  SRC: { RESULTS: 0, FAVS: 1, RECENTS: 2, SUGGEST: 3, POI: 4, DEST: 5, API: 6 },
  ERR: { NO_KEY: 1, NO_LOCATION: 2, API: 3, NETWORK: 4, NO_RESULTS: 5 },
  FMT: { BIT1: 0, BIT4: 1 },
  MK: { ME: 0, PIN: 1, START: 2, DEST: 3, ARROW: 4, POI: 5 },   // POI kinds: 5 + category (0-5)
  MAN: {
    STRAIGHT: 0, SLIGHT_LEFT: 1, LEFT: 2, SHARP_LEFT: 3, UTURN_LEFT: 4,
    SLIGHT_RIGHT: 5, RIGHT: 6, SHARP_RIGHT: 7, UTURN_RIGHT: 8,
    RAMP_LEFT: 9, RAMP_RIGHT: 10, MERGE: 11, FORK_LEFT: 12, FORK_RIGHT: 13,
    FERRY: 14, ROUNDABOUT_LEFT: 15, ROUNDABOUT_RIGHT: 16, DEPART: 17, ARRIVE: 18,
    BUS: 19, TRAIN: 20, SUBWAY: 21, TRAM: 22, WALK: 23, NONE: 255
  },
  NAV_FLAG: {
    ALERT_SOON: 1, ALERT_NOW: 2, ARRIVED: 4, REROUTING: 8, TRANSIT: 16,
    NO_GPS: 32, DEST_VISIBLE: 64, NEAR: 128
  },
  ADJ: { ZOOM_IN: 0, ZOOM_OUT: 1, PAN: 2, RESET: 3, FIT_ROUTE: 4 },
  // Explore categories, same order as CATEGORY_NAMES in src/c/main.c
  CATEGORIES: [
    { name: 'Restaurants', types: ['restaurant'], rank: 'POPULARITY', radius: 3000 },
    { name: 'Coffee', types: ['cafe', 'coffee_shop'], rank: 'POPULARITY', radius: 3000 },
    { name: 'Gas', types: ['gas_station'], rank: 'DISTANCE', radius: 8000 },
    { name: 'Groceries', types: ['grocery_store', 'supermarket'], rank: 'DISTANCE', radius: 6000 },
    { name: 'Parks', types: ['park'], rank: 'POPULARITY', radius: 5000 },
    { name: 'Hotels', types: ['lodging'], rank: 'POPULARITY', radius: 8000 },
    { name: 'ATMs', types: ['atm'], rank: 'DISTANCE', radius: 4000 },
    { name: 'Pharmacies', types: ['pharmacy', 'drugstore'], rank: 'DISTANCE', radius: 6000 },
    { name: 'Transit', types: ['transit_station', 'bus_station', 'train_station', 'subway_station', 'light_rail_station'], rank: 'DISTANCE', radius: 2000 },
    { name: 'EV charging', types: ['electric_vehicle_charging_station'], rank: 'DISTANCE', radius: 10000 },
    { name: 'Parking', types: ['parking'], rank: 'DISTANCE', radius: 3000 },
    { name: 'Libraries', types: ['library'], rank: 'DISTANCE', radius: 10000 }
  ]
};
