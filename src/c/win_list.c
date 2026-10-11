// Pebble Maps - generic list screens: favorites, explore, travel modes, steps
#include "windows.h"
#include "comm.h"

#define MAX_FAVS 20
#define MAX_RECENTS 10
#define MAX_STEPS 40
#define MP_OPEN (PM_LOWMEM ? 1 : 2)        // places-on-map menu: "open now" row
#define MP_FIRST_CAT (MP_OPEN + 1)          // ...first kind-of-place row
#define PL_OPEN 1                          // Places menu: "open now" row (after Favorites)
#define PL_FIRST_CAT 2

typedef struct ListWin {
  ListKind kind;
  int arg;
  Window *window;
  MenuLayer *menu;
  StatusBarLayer *status;
  Layer *header;
  Layer *dots;
  ListItem *items;     // main section
  int count;
  ListItem *items2;    // recents (favorites screen)
  int count2;
  bool loaded;
  char empty_text[64];
  ActionMenu *action_menu;
  struct ListWin *next;
  ActionMenuLevel *am_root, *am_modes;
  int am_index;
  char mode_sub[MODE_COUNT + 1][36];   // travel-time lines for the mode pickers
} ListWin;

static const char *title_for(ListKind k) {
  switch (k) {
    case LW_FAVS: return "Favorites";
    case LW_CATEGORIES: return "Places";
    case LW_MODES: return "Get there by";
    case LW_STEPS: return "Directions";
    case LW_SETMODE: return "Default travel mode";
    case LW_MAPPLACES: return "Places on map";
  }
  return "";
}

static GColor header_color(ListKind k) {
  switch (k) {
    case LW_FAVS: return PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack);
    case LW_STEPS: return C_NAVGREEN;
    case LW_MODES: return C_NAVGREEN;
    default: return C_BLUE;
  }
}

static struct ListWin *s_all;

#define HEADER_H PBL_IF_ROUND_ELSE(0, g_fonts.body_h + 4)

// --- Data ------------------------------------------------------------------
static void fill_modes(ListWin *lw) {
  lw->count = MODE_COUNT;
  for (int i = 0; i < MODE_COUNT; i++) {
    ListItem *it = &lw->items[i];
    memset(it, 0, sizeof(*it));
    it->title = mode_name(i);
    strncpy(lw->mode_sub[i], lw->kind == LW_MODES ? "Checking..." : "", sizeof(lw->mode_sub[i]) - 1);
    it->sub = lw->mode_sub[i];
    it->icon = icon_for_mode(i);
    it->extra = (i == lw->arg) ? 1 : 0;
  }
  if (lw->kind == LW_SETMODE) {
    // extra row 0: "Use app default"
    for (int i = MODE_COUNT; i > 0; i--) lw->items[i] = lw->items[i - 1];
    memset(&lw->items[0], 0, sizeof(ListItem));
    lw->items[0].title = "App default";
    snprintf(lw->mode_sub[MODE_COUNT], sizeof(lw->mode_sub[0]), "Currently %s", mode_name(g_app.default_mode));
    lw->items[0].sub = lw->mode_sub[MODE_COUNT];
    lw->items[0].icon = ICON_NAV;
    lw->count = MODE_COUNT + 1;
  }
}

static void request(ListWin *lw) {
  switch (lw->kind) {
    case LW_FAVS:
      comm_cmd(CMD_FAV_LIST);   // the current list stays on screen until the new one arrives
      break;
    case LW_MODES:
      comm_cmd(CMD_MODE_TIMES);
      break;
    case LW_STEPS:
      comm_cmd(CMD_STEPS);
      break;
    default:
      break;
  }
  dots_layer_set_running(lw->dots, !lw->loaded);
}

// Reload rows, keeping the selected row on screen (rows can change height as data arrives)
static void list_reload(ListWin *lw) {
  menu_layer_reload_data(lw->menu);
  if (lw->kind == LW_MAPPLACES || lw->kind == LW_CATEGORIES) return;   // switches: rows keep their size
  menu_layer_set_selected_index(lw->menu, menu_layer_get_selected_index(lw->menu), MenuRowAlignCenter, false);
}

static void handle(int cmd, DictionaryIterator *it, void *ctx) {
  ListWin *lw = ctx;
  switch (cmd) {
    case CMD_LIST: {
      int kind = tuple_int(it, MESSAGE_KEY_num, -1);
      const char *packed = tuple_str(it, MESSAGE_KEY_list);
      if (lw->kind == LW_FAVS && kind == LIST_FAVS) {
        free(lw->items);
        lw->items = alloc_list(packed, MAX_FAVS, &lw->count);
        lw->loaded = true;
      } else if (lw->kind == LW_FAVS && kind == LIST_RECENTS) {
        free(lw->items2);
        lw->items2 = alloc_list(packed, MAX_RECENTS, &lw->count2);
        lw->loaded = true;
      } else if (lw->kind == LW_MODES && kind == LIST_MODES) {
        int n = 0;
        ListItem *tmp = alloc_list(packed, MODE_COUNT, &n);
        for (int i = 0; i < n && i < MODE_COUNT; i++) {
          strncpy(lw->mode_sub[i], tmp[i].sub, sizeof(lw->mode_sub[i]) - 1);
        }
        free(tmp);
        lw->loaded = true;
      } else if (lw->kind == LW_STEPS && kind == LIST_STEPS) {
        free(lw->items);
        lw->items = alloc_list(packed, MAX_STEPS, &lw->count);
        lw->loaded = true;
      } else {
        return;
      }
      dots_layer_set_running(lw->dots, false);
      list_reload(lw);
      break;
    }
    case CMD_TOAST:
      ui_toast(lw->window, tuple_str(it, MESSAGE_KEY_text));
      if (lw->kind == LW_FAVS) request(lw);
      break;
    case CMD_ERROR:
      dots_layer_set_running(lw->dots, false);
      lw->loaded = true;
      if (lw->kind == LW_MODES) {
        // keep the picker usable even if times failed
        for (int i = 0; i < MODE_COUNT; i++) lw->mode_sub[i][0] = 0;
        list_reload(lw);
      } else {
        show_error(it);
      }
      break;
  }
}

// --- Menu ------------------------------------------------------------------
static uint16_t num_sections(MenuLayer *m, void *ctx) {
  ListWin *lw = ctx;
  return lw->kind == LW_FAVS ? 2 : 1;
}

static uint16_t num_rows(MenuLayer *m, uint16_t section, void *ctx) {
  ListWin *lw = ctx;
  switch (lw->kind) {
    case LW_FAVS:
      if (!lw->loaded) return 0;
      if (section == 0) return lw->count + (lw->count ? 1 : 2);  // + hint + "Save my location"
      return lw->count2 ? lw->count2 : 0;
    case LW_CATEGORIES: return CATEGORY_COUNT + PL_FIRST_CAT;  // + Favorites, Open now
    case LW_MAPPLACES: return MP_FIRST_CAT + MAP_CAT_COUNT;
    case LW_STEPS: return lw->loaded ? (lw->count ? lw->count : 1) : 0;
    default: return lw->count;
  }
}

// --- Places on the map: switches -------------------------------------------------
// rows: show places, (show names), then one per kind of place

static bool mp_on(int row) {
  if (row == 0) return g_app.pois_on;
  if (row == MP_OPEN) return g_app.open_now;
  if (row < MP_FIRST_CAT) return g_app.poi_names;
  return (g_app.poi_mask >> (row - MP_FIRST_CAT)) & 1;
}

static void mp_toggle(int row) {
  if (row == 0) g_app.pois_on = !g_app.pois_on;
  else if (row == MP_OPEN) g_app.open_now = !g_app.open_now;
  else if (row < MP_FIRST_CAT) g_app.poi_names = !g_app.poi_names;
  else g_app.poi_mask ^= 1 << (row - MP_FIRST_CAT);
  poi_prefs_save();
}

// A small on/off switch
static bool mp_disabled(int row) { return row > 0 && !g_app.pois_on; }

static void draw_switch(GContext *ctx, GPoint c, bool on, bool hl, bool dim) {
  GRect r = GRect(c.x - 12, c.y - 7, 24, 14);
  GColor blue = dim ? PBL_IF_COLOR_ELSE(GColorDarkGray, C_BLUE) : C_BLUE;
  GColor track = on ? (hl ? GColorWhite : (dim ? PBL_IF_COLOR_ELSE(GColorLightGray, C_BLUE) : C_BLUE))
                    : PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite);
  graphics_context_set_fill_color(ctx, track);
  graphics_fill_rect(ctx, r, 7, GCornersAll);
  graphics_context_set_stroke_color(ctx, hl ? GColorWhite : C_DIVIDER);
  graphics_draw_round_rect(ctx, r, 7);
  graphics_context_set_fill_color(ctx, on ? (hl ? blue : GColorWhite) : (hl ? blue : PBL_IF_COLOR_ELSE(dim ? GColorDarkGray : GColorBlack, GColorBlack)));
  graphics_fill_circle(ctx, GPoint(on ? r.origin.x + 17 : r.origin.x + 7, c.y), 4);
}

static int16_t header_h(MenuLayer *m, uint16_t section, void *ctx) {
  ListWin *lw = ctx;
  if (lw->kind != LW_FAVS || !lw->loaded) return 0;
  if (section == 1 && lw->count2 == 0) return 0;
  return MENU_CELL_BASIC_HEADER_HEIGHT;
}

static void draw_header(GContext *ctx, const Layer *cell, uint16_t section, void *cb) {
  GRect b = layer_get_bounds(cell);
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorLightGray, GColorWhite));
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_text_color(ctx, C_TEXT);
  graphics_draw_text(ctx, section == 0 ? "SAVED" : "RECENT", g_fonts.chip,
                     GRect(PBL_IF_ROUND_ELSE(0, 6), -2, b.size.w - PBL_IF_ROUND_ELSE(0, 6), 16),
                     GTextOverflowModeFill, PBL_IF_ROUND_ELSE(GTextAlignmentCenter, GTextAlignmentLeft), NULL);
}

static void row_content(ListWin *lw, MenuIndex *i, const char **title, const char **sub,
                        IconId *icon, GColor *icon_color, int *maneuver, bool *badge);

static int16_t cell_h(MenuLayer *m, MenuIndex *i, void *ctx) {
  ListWin *lw = ctx;
  const char *title, *sub;
  IconId icon;
  GColor icol;
  int man;
  bool badge;
  row_content(lw, i, &title, &sub, &icon, &icol, &man, &badge);
  GRect mb = layer_get_bounds(menu_layer_get_layer(m));
#ifdef PBL_ROUND
  if (!menu_layer_is_index_selected(m, i)) return g_fonts.body_h + 16;
  int tw = mb.size.w - 36;
  GSize ts = graphics_text_layout_get_content_size(title, g_fonts.body_b, GRect(0, 0, tw, g_fonts.body_h * 3 + 8),
                                                   GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter);
  return 26 + ts.h + (sub ? g_fonts.small_h + 4 : 0) + 8;
#else
  int tw = mb.size.w - ((icon || man >= 0) ? 38 : 10);
  if (lw->kind == LW_STEPS) {
    GSize ts = graphics_text_layout_get_content_size(title, g_fonts.body_b, GRect(0, 0, tw, g_fonts.body_h * 4 + 8),
                                                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft);
    int h = ts.h + (sub ? g_fonts.small_h : 0) + 10;
    return h < 44 ? 44 : h;
  }
  int h = g_fonts.body_h + (sub ? g_fonts.small_h : 0) + 12;
  return h < 40 ? 40 : h;
#endif
}

static void row_content(ListWin *lw, MenuIndex *i, const char **title, const char **sub,
                        IconId *icon, GColor *icon_color, int *maneuver, bool *badge) {
  *maneuver = -1;
  *badge = false;
  *icon_color = C_ICON;
  switch (lw->kind) {
    case LW_FAVS:
      if (i->section == 0) {
        if (lw->count == 0 && i->row == 0) {
          *title = "No favorites yet";
          *sub = "Use the star on any place";
          *icon = ICON_STAR_OUTLINE;
          *icon_color = C_STAR_EDGE;
        } else if (i->row >= lw->count) {
          *title = "Save my location";
          *sub = "Add where you are now";
          *icon = ICON_PLUS;
          *icon_color = C_BLUE;
        } else {
          ListItem *it = &lw->items[i->row];
          *title = it->title;
          *sub = it->sub;
          *icon = ICON_STAR;
          *icon_color = C_STAR;
        }
      } else {
        ListItem *it = &lw->items2[i->row];
        *title = it->title;
        *sub = it->sub;
        *icon = ICON_CLOCK;
      }
      break;
    case LW_CATEGORIES:
      if (i->row == 0) {
        *title = "Favorites";
        *sub = NULL;
        *icon = ICON_STAR;
        *icon_color = C_STAR;
        break;
      }
      if (i->row == PL_OPEN) {
        *title = "Open now";
        *sub = PBL_IF_ROUND_ELSE(g_app.open_now ? "On" : "Off", NULL);
        *icon = ICON_CLOCK;
        *icon_color = C_GREEN;
        break;
      }
      *title = CATEGORY_NAMES[i->row - PL_FIRST_CAT];
      *sub = NULL;
      *icon = CATEGORY_ICONS[i->row - PL_FIRST_CAT];
#ifdef PBL_COLOR
      *icon_color = (GColor){ .argb = CATEGORY_COLORS[i->row - PL_FIRST_CAT] };
#endif
      break;
    case LW_MAPPLACES: {
      int r = i->row;
      if (r == 0) {
        *title = "Show places";
        *icon = ICON_EXPLORE;
        *icon_color = C_PIN;
      } else if (r == MP_OPEN) {
        *title = "Open now";
        *icon = ICON_CLOCK;
        *icon_color = C_GREEN;
      } else if (r < MP_FIRST_CAT) {
        *title = "Show names";
        *icon = ICON_POI_NAMES;
      } else {
        *title = map_cat_name(r - MP_FIRST_CAT);
        *icon = map_cat_icon(r - MP_FIRST_CAT);
#ifdef PBL_COLOR
        *icon_color = map_cat_color(r - MP_FIRST_CAT);
#endif
      }
      *sub = PBL_IF_ROUND_ELSE(mp_on(r) ? "On" : "Off", NULL);
      break;
    }
    case LW_STEPS:
      if (!lw->count) {
        *title = "No steps";
        *sub = NULL;
        *icon = ICON_NONE;
        break;
      }
      *title = lw->items[i->row].title;
      *sub = lw->items[i->row].sub;
      *icon = ICON_NONE;
      *maneuver = lw->items[i->row].icon;
      *icon_color = C_NAVGREEN;
      break;
    default: {
      ListItem *it = &lw->items[i->row];
      *title = it->title;
      *sub = it->sub[0] ? it->sub : NULL;
      *icon = it->icon;
      *badge = it->extra != 0;
      *icon_color = C_BLUE;
      break;
    }
  }
}

static void draw_row(GContext *ctx, const Layer *cell, MenuIndex *i, void *cb) {
  ListWin *lw = cb;
  const char *title, *sub;
  IconId icon;
  GColor icol;
  int man;
  bool badge;
  row_content(lw, i, &title, &sub, &icon, &icol, &man, &badge);
  GRect b = layer_get_bounds(cell);
  bool hl = menu_cell_layer_is_highlighted(cell);
  GColor fg = hl ? GColorWhite : C_TEXT;
  GColor sub_fg = hl ? GColorWhite : C_SUBTEXT;
#ifdef PBL_COLOR
  bool keep_color = (icon == ICON_STAR);
  // places menu with "Show places" off: the other rows are greyed out
  bool dim = lw->kind == LW_MAPPLACES && mp_disabled(i->row);
  if (dim) {
    if (!hl) fg = sub_fg = GColorDarkGray;
    icol = hl ? GColorWhite : GColorLightGray;
  }
#else
  bool keep_color = false;
  bool dim = false;
#endif
  GColor ic = (hl && !keep_color) ? GColorWhite : icol;
  GColor bg = hl ? (dim ? PBL_IF_COLOR_ELSE(GColorDarkGray, C_BLUE) : C_BLUE) : C_BG;
  int isz = g_fonts.level >= 1 ? 24 : 20;

#ifdef PBL_ROUND
  if (!hl) {
    graphics_context_set_text_color(ctx, fg);
    graphics_draw_text(ctx, title, g_fonts.body_b, GRect(20, 4, b.size.w - 40, g_fonts.body_h + 6),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
    return;
  }
  int y = 4;
  if (man >= 0) maneuver_draw(ctx, man, GPoint(b.size.w / 2, y + 11), 22, ic, ic);
  else if (icon) icon_draw(ctx, icon, GPoint(b.size.w / 2, y + 10), 20, ic, bg);
  y += 22;
  graphics_context_set_text_color(ctx, fg);
  int sub_h = sub ? g_fonts.small_h + 4 : 0;
  graphics_draw_text(ctx, title, g_fonts.body_b, GRect(18, y - 3, b.size.w - 36, b.size.h - y - sub_h),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  if (sub) {
    graphics_context_set_text_color(ctx, sub_fg);
    graphics_draw_text(ctx, sub, g_fonts.small, GRect(18, b.size.h - sub_h - 6, b.size.w - 36, sub_h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
#else
  int tx = isz + 14;
  if (man >= 0) {
    maneuver_draw(ctx, man, GPoint(tx / 2, b.size.h / 2), isz + 4, ic, hl ? GColorLightGray : C_DIVIDER);
  } else if (icon) {
    icon_draw(ctx, icon, GPoint(tx / 2, b.size.h / 2), isz, ic, bg);
  } else {
    tx = 6;
  }
  int tw = b.size.w - tx - 4;
  if (lw->kind == LW_STEPS && lw->count && lw->items[i->row].extra) tw -= 12;
  if (lw->kind == LW_MAPPLACES || (lw->kind == LW_CATEGORIES && i->row == PL_OPEN)) {
    bool on = lw->kind == LW_MAPPLACES ? mp_on(i->row) : g_app.open_now;
    draw_switch(ctx, GPoint(b.size.w - 18, b.size.h / 2), on, hl, dim);
    tw -= 34;
  }
  int sub_h = sub ? g_fonts.small_h : 0;
  graphics_context_set_text_color(ctx, fg);
  graphics_draw_text(ctx, title, g_fonts.body_b, GRect(tx, -2, tw - (badge ? 62 : 0), b.size.h - sub_h - 2),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  if (sub) {
    graphics_context_set_text_color(ctx, sub_fg);
    graphics_draw_text(ctx, sub, g_fonts.small, GRect(tx, b.size.h - sub_h - 6, tw, sub_h + 4),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft, NULL);
  }
  if (badge) {
    // pill a little roomier than the word, so the rounded corners never clip it
    int bw = graphics_text_layout_get_content_size("Default", g_fonts.chip, GRect(0, 0, 80, 20),
                                                   GTextOverflowModeFill, GTextAlignmentLeft).w + 14;
    GRect br = GRect(b.size.w - bw - 4, 4, bw, 18);
    graphics_context_set_fill_color(ctx, hl ? GColorWhite : C_BLUE);
    graphics_fill_rect(ctx, br, 9, GCornersAll);
    graphics_context_set_text_color(ctx, hl ? C_BLUE : GColorWhite);
    graphics_draw_text(ctx, "Default", g_fonts.chip, GRect(br.origin.x, br.origin.y - 2, br.size.w, 18),
                       GTextOverflowModeFill, GTextAlignmentCenter, NULL);
  }
#endif
  // transit steps: Select shows the schedule
  if (lw->kind == LW_STEPS && lw->count && lw->items[i->row].extra) draw_info_tab(ctx, b, hl);
}

// --- Favorite options (ActionMenu) ----------------------------------------
static void am_cb(ActionMenu *menu, const ActionMenuItem *item, void *ctx) {
  ListWin *lw = ctx;
  int action = (int)action_menu_item_get_action_data(item);
  if (action == 100) {
    comm_cmd2(CMD_FAV_DELETE, lw->am_index, -1);
  } else if (action >= 0 && action <= MODE_COUNT) {
    comm_cmd2(CMD_FAV_SETMODE, lw->am_index, action == MODE_COUNT ? MODE_USE_DEFAULT : action);
  }
}

static void am_did_close(ActionMenu *menu, const ActionMenuItem *performed, void *ctx) {
  ListWin *lw = ctx;
  action_menu_hierarchy_destroy(lw->am_root, NULL, NULL);
  lw->am_root = lw->am_modes = NULL;
  lw->action_menu = NULL;
}

static void open_fav_options(ListWin *lw, int index) {
  lw->am_index = index;
  lw->am_root = action_menu_level_create(2);
  lw->am_modes = action_menu_level_create(MODE_COUNT + 1);
  action_menu_level_add_child(lw->am_root, lw->am_modes, "Default travel mode");
  action_menu_level_add_action(lw->am_root, "Remove favorite", am_cb, (void *)100);
  for (int m = 0; m < MODE_COUNT; m++) {
    action_menu_level_add_action(lw->am_modes, mode_name(m), am_cb, (void *)m);
  }
  action_menu_level_add_action(lw->am_modes, "Use app default", am_cb, (void *)MODE_COUNT);
  ActionMenuConfig cfg = {
    .root_level = lw->am_root,
    .colors = { .background = PBL_IF_COLOR_ELSE(GColorChromeYellow, GColorBlack),
                .foreground = PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite) },
    .align = ActionMenuAlignCenter,
    .context = lw,
    .did_close = am_did_close,
  };
  lw->action_menu = action_menu_open(&cfg);
}

// --- Clicks ----------------------------------------------------------------
static void select_cb(MenuLayer *m, MenuIndex *i, void *ctx) {
  ListWin *lw = ctx;
  switch (lw->kind) {
    case LW_FAVS:
      if (i->section == 0 && lw->count == 0 && i->row == 0) {
        ui_toast(lw->window, "Or add them in Settings on your phone");
      } else if (i->section == 0 && i->row >= lw->count) {
        comm_cmd(CMD_SAVE_HERE);
        ui_toast(lw->window, "Saving...");
      } else if (i->section == 0) {
        place_window_push(SRC_FAVS, i->row, lw->items[i->row].title);
      } else {
        place_window_push(SRC_RECENTS, i->row, lw->items2[i->row].title);
      }
      break;
    case LW_MAPPLACES:
      if (mp_disabled(i->row)) break;   // greyed out while places are off
      mp_toggle(i->row);
      list_reload(lw);
      break;
    case LW_CATEGORIES:
      if (i->row == 0) list_window_push(LW_FAVS, 0);
      else if (i->row == PL_OPEN) {
        g_app.open_now = !g_app.open_now;
        poi_prefs_save();
        list_reload(lw);
      } else results_window_push_nearby(i->row - PL_FIRST_CAT);
      break;
    case LW_MODES:
      route_window_push(i->row);
      break;
    case LW_STEPS:
      if (lw->count && lw->items[i->row].extra) {
        uint8_t v = lw->items[i->row].icon;   // the ride's vehicle
        text_page_push(v == MAN_FERRY ? ICON_FERRY : (v == MAN_BUS ? ICON_BUS : ICON_TRAIN),
                       lw->items[i->row].title, CMD_TRANSIT_INFO, i->row);
      }
      break;
    case LW_SETMODE:
      comm_cmd2(CMD_FAV_SETMODE, lw->arg, i->row == 0 ? MODE_USE_DEFAULT : i->row - 1);
      window_stack_remove(lw->window, true);
      break;
    default:
      break;
  }
}

// Greyed-out rows get a grey highlight
static void selection_changed(MenuLayer *m, MenuIndex new_index, MenuIndex old_index, void *ctx) {
  ListWin *lw = ctx;
  if (lw->kind != LW_MAPPLACES) return;
  menu_layer_set_highlight_colors(m, mp_disabled(new_index.row) ? PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack) : C_BLUE, GColorWhite);
}

static void select_long_cb(MenuLayer *m, MenuIndex *i, void *ctx) {
  ListWin *lw = ctx;
  if (lw->kind == LW_FAVS && i->section == 0 && i->row < lw->count) {
    open_fav_options(lw, i->row);
  } else if (lw->kind == LW_FAVS && i->section == 1) {
    ui_toast(lw->window, "Open it, then tap the star to save");
  } else {
    select_cb(m, i, ctx);
  }
}

static void header_update(Layer *layer, GContext *ctx) {
  ListWin *lw = *(ListWin **)layer_get_data(layer);
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, header_color(lw->kind));
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  GColor tc = (lw->kind == LW_FAVS) ? PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite) : GColorWhite;
  graphics_context_set_text_color(ctx, tc);
  graphics_draw_text(ctx, title_for(lw->kind), g_fonts.body_b, GRect(0, -3, b.size.w, g_fonts.body_h + 6),
                     GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  // empty-favorites hint lives below the menu rows
}

// --- Lifecycle -------------------------------------------------------------
static void window_load(Window *window) {
  ListWin *lw = window_get_user_data(window);
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  int top = 0;
#ifndef PBL_ROUND
  lw->status = status_bar_layer_create();
  status_bar_layer_set_colors(lw->status, header_color(lw->kind),
                              lw->kind == LW_FAVS ? PBL_IF_COLOR_ELSE(GColorBlack, GColorWhite) : GColorWhite);
  layer_add_child(root, status_bar_layer_get_layer(lw->status));
  top = STATUS_BAR_LAYER_HEIGHT;
  lw->header = layer_create_with_data(GRect(0, top, b.size.w, HEADER_H), sizeof(ListWin *));
  *(ListWin **)layer_get_data(lw->header) = lw;
  layer_set_update_proc(lw->header, header_update);
  layer_add_child(root, lw->header);
  top += HEADER_H;
#endif
  top += PBL_IF_ROUND_ELSE(0, 4);   // breathing room under the title bar
  lw->menu = menu_layer_create(GRect(0, top, b.size.w, b.size.h - top));
  menu_layer_set_callbacks(lw->menu, lw, (MenuLayerCallbacks){
    .get_num_sections = num_sections,
    .get_num_rows = num_rows,
    .get_header_height = header_h,
    .draw_header = draw_header,
    .get_cell_height = cell_h,
    .draw_row = draw_row,
    .get_separator_height = ui_separator_h,
    .draw_separator = ui_draw_separator,
    .select_click = select_cb,
    .select_long_click = select_long_cb,
    .selection_changed = selection_changed,
  });
  menu_layer_set_highlight_colors(lw->menu, C_BLUE, GColorWhite);
  menu_layer_set_click_config_onto_window(lw->menu, window);
  layer_add_child(root, menu_layer_get_layer(lw->menu));
  lw->dots = dots_layer_create(GRect(0, b.size.h / 2 - 10, b.size.w, 30));
  layer_add_child(root, lw->dots);

  if (lw->kind == LW_MODES || lw->kind == LW_SETMODE) {
    fill_modes(lw);
    lw->loaded = true;
    int sel = lw->kind == LW_MODES ? lw->arg : 0;
    menu_layer_set_selected_index(lw->menu, MenuIndex(0, sel), MenuRowAlignCenter, false);
  }
  if (lw->kind == LW_CATEGORIES || lw->kind == LW_MAPPLACES) lw->loaded = true;
  request(lw);
}

static void window_appear(Window *window) {
  ListWin *lw = window_get_user_data(window);
  comm_set_handler(handle, lw);
  if (lw->kind == LW_FAVS && lw->loaded) request(lw);  // refresh after edits
}

static void window_disappear(Window *window) {
  comm_clear_handler(handle);
  ui_toast_cancel();
}

static void window_unload(Window *window) {
  ListWin *lw = window_get_user_data(window);
  menu_layer_destroy(lw->menu);
  if (lw->status) status_bar_layer_destroy(lw->status);
  if (lw->header) layer_destroy(lw->header);
  dots_layer_destroy(lw->dots);
  free(lw->items);
  free(lw->items2);
  for (ListWin **pp = &s_all; *pp; pp = &(*pp)->next) {
    if (*pp == lw) { *pp = lw->next; break; }
  }
  free(lw);
  window_destroy(window);
}

void list_windows_close_all(void) {
  // removing a window unloads it, which unlinks it from s_all
  while (s_all) {
    ListWin *lw = s_all;
    window_stack_remove(lw->window, false);
    if (s_all == lw) s_all = lw->next;  // safety if unload did not run
  }
}

// Short on memory (Pebble Time, Time Round): drop the lists underneath the top window
void list_windows_trim(void) {
  Window *top = window_stack_get_top_window();
  for (ListWin *o = s_all, *nx; o; o = nx) {
    nx = o->next;
    if (o->window != top) window_stack_remove(o->window, false);
  }
  results_window_close();
}

void list_window_push(ListKind kind, int arg) {
  if (heap_bytes_free() < 4000) list_windows_trim();
  ListWin *lw = calloc(1, sizeof(ListWin));
  if (!lw) return;
  lw->kind = kind;
  lw->arg = arg;
  if (kind == LW_MODES || kind == LW_SETMODE) {
    lw->items = calloc(MODE_COUNT + 1, sizeof(ListItem));
    if (!lw->items) { free(lw); return; }
  }
  lw->next = s_all;
  s_all = lw;
  lw->window = window_create();
  if (!lw->window) {   // out of memory: give up quietly
    s_all = lw->next;
    free(lw->items);
    free(lw);
    return;
  }
  window_set_user_data(lw->window, lw);
  window_set_background_color(lw->window, C_BG);
  window_set_window_handlers(lw->window, (WindowHandlers){
    .load = window_load, .appear = window_appear,
    .disappear = window_disappear, .unload = window_unload,
  });
  window_stack_push(lw->window, true);
}
