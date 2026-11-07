// Pebble Maps - receives Google Static Maps images (converted by the phone)
#pragma once
#include "common.h"

#define MAX_MARKERS 24

// Tile mode (Pebble Time 2 / Round 2, north-up home map): the picture buffer holds compressed
// TILE_SIZE x TILE_SIZE tiles instead of one picture. The first TILE_SLOTS entries of the buffer are the
// tile table; the phone decides what goes where (see src/pkjs/tilestore.js).
#define TILE_SIZE 64
#define TILE_SLOTS 80
#define TILE_START 1280   // TILE_SLOTS * 16 bytes: where the tile data area begins

typedef struct {
  int16_t x, y;
  uint8_t kind;   // MK_*
  uint8_t index;  // result index for pins
  int16_t name;   // places: offset of the name in g_map.names (-1 = none)
} Marker;

typedef struct {
  GBitmap *bmp;
  int seq;          // current screen's request id
  int w, h;
  bool complete;    // some picture has arrived for this screen
  Marker markers[MAX_MARKERS];
  int n_markers;
  char *names;                // place names for the markers (one block)
  Layer *observer;
  int16_t shift_x, shift_y;   // moves the user made that the phone hasn't drawn yet
  int16_t fw, fh;             // frame size the markers refer to
  // where the picture in the buffer sits in the world
  bool ref_valid;
  int img_id;                 // id of the picture in the buffer
  int chunk_img;              // picture id whose pieces we accept (-1 = none)
  int32_t rcx, rcy;           // world pixel at the picture center
  int rzoom, rheading;
} MapData;

extern MapData g_map;

// How the picture in the buffer was just moved: zoom by 2^k, turn by rot degrees, then slide by (vx, vy)
typedef struct {
  int k, rot, vx, vy;
} MapXform;
typedef void (*MapXformFn)(const MapXform *x);

// Request a new map. Clears the current image. Returns the request id.
int map_request(int cmd, int w, int h, int idx, int mode);
void map_set_observer(Layer *layer);
void map_release(void);
// Reserve the shared picture buffer (call once at start-up)
void map_reserve(void);
// Size of the reserved picture buffer (screen plus margin)
GSize map_buffer_size(void);
// End of the tile data area in the buffer (0 when the watch has no tile mode)
int map_arena_end(void);
void map_handle(int cmd, DictionaryIterator *it);
// Pan/zoom the current map: the picture moves/zooms right away, sharp detail follows
void map_adjust(int action, int dx, int dy);
// Live drag offset (touch); does not request anything
void map_set_drag(int dx, int dy);
// The phone has drawn the user's moves up to this one
void map_apply_ack(int ack);
// Called whenever the buffer's picture is moved in place (navigation keeps its arrow in step)
void map_set_xform_listener(MapXformFn fn);
// Where a buffer point ends up after a move
GPoint map_xform_point(const MapXform *x, GPoint p);
// Draw the image (or placeholder) and markers into frame. selected = result index or -1.
void map_draw(GContext *ctx, GRect frame, int selected);
// Find a marker for a result index; returns NULL if missing
const Marker *map_marker_for(int index);
// Nearest pin to a point (touch); returns result index or -1
int map_pin_near(GPoint p, GRect frame, int max_dist);
// Nearest place dot (home map) to a point; returns its index or -1
int map_poi_near(GPoint p, GRect frame, int max_dist);
