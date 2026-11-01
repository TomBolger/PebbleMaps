// Pebble Maps - drawing toolkit
#include "ui.h"

Fonts g_fonts;

#define PERSIST_TEXT_LEVEL 1

static GFont F(const char *key) { return fonts_get_system_font(key); }

void ui_set_text_level(int level) {
  if (level < 0 || level > 2) {
    // automatic: one step larger on the big Pebble Time 2 / Round 2 screens
#if defined(PBL_PLATFORM_EMERY) || defined(PBL_PLATFORM_GABBRO)
    level = 1;
#else
    level = 0;
#endif
  }
  g_fonts.level = level;
  g_fonts.chip = F(FONT_KEY_GOTHIC_14_BOLD);
  switch (level) {
    case 0:
      g_fonts.small = F(FONT_KEY_GOTHIC_18);
      g_fonts.small_b = F(FONT_KEY_GOTHIC_18_BOLD);
      g_fonts.body = F(FONT_KEY_GOTHIC_18);
      g_fonts.body_b = F(FONT_KEY_GOTHIC_18_BOLD);
      g_fonts.title = F(FONT_KEY_GOTHIC_24_BOLD);
      g_fonts.big = F(FONT_KEY_GOTHIC_24_BOLD);
      g_fonts.small_h = 18; g_fonts.body_h = 18; g_fonts.title_h = 24; g_fonts.big_h = 24;
      break;
    case 1:
      g_fonts.small = F(FONT_KEY_GOTHIC_18);
      g_fonts.small_b = F(FONT_KEY_GOTHIC_18_BOLD);
      g_fonts.body = F(FONT_KEY_GOTHIC_24);
      g_fonts.body_b = F(FONT_KEY_GOTHIC_24_BOLD);
      g_fonts.title = F(FONT_KEY_GOTHIC_28_BOLD);
      g_fonts.big = F(FONT_KEY_GOTHIC_28_BOLD);
      g_fonts.small_h = 18; g_fonts.body_h = 24; g_fonts.title_h = 28; g_fonts.big_h = 28;
      break;
    default:
      g_fonts.small = F(FONT_KEY_GOTHIC_24);
      g_fonts.small_b = F(FONT_KEY_GOTHIC_24_BOLD);
      g_fonts.body = F(FONT_KEY_GOTHIC_28);
      g_fonts.body_b = F(FONT_KEY_GOTHIC_28_BOLD);
      g_fonts.title = F(FONT_KEY_GOTHIC_28_BOLD);
      g_fonts.big = F(FONT_KEY_BITHAM_30_BLACK);
      g_fonts.small_h = 24; g_fonts.body_h = 28; g_fonts.title_h = 28; g_fonts.big_h = 30;
      break;
  }
}

void ui_init(void) {
  int level = persist_exists(PERSIST_TEXT_LEVEL) ? persist_read_int(PERSIST_TEXT_LEVEL) : 3;
  ui_set_text_level(level);
}

void ui_save_text_level(int level) {
  persist_write_int(PERSIST_TEXT_LEVEL, level);
  ui_set_text_level(level);
}

// ---------------------------------------------------------------------------
// Geometry helpers. Icons are designed on a 40x40 grid.
// ---------------------------------------------------------------------------
static GPoint s_c;
static int s_sz;

static inline GPoint P(int x, int y) {
  return GPoint(s_c.x + (x - 20) * s_sz / 40, s_c.y + (y - 20) * s_sz / 40);
}
static inline int S(int v) {
  int r = v * s_sz / 40;
  return r < 1 ? 1 : r;
}

static void line(GContext *ctx, int x1, int y1, int x2, int y2) {
  graphics_draw_line(ctx, P(x1, y1), P(x2, y2));
}

// Exactly 2 px wide, crisp: two 1 px lines side by side
static void line2(GContext *ctx, int x1, int y1, int x2, int y2) {
  GPoint a = P(x1, y1), b = P(x2, y2);
  int dx = b.x - a.x, dy = b.y - a.y;
  bool flat = (dx < 0 ? -dx : dx) >= (dy < 0 ? -dy : dy);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_line(ctx, a, b);
  if (flat) graphics_draw_line(ctx, GPoint(a.x, a.y + 1), GPoint(b.x, b.y + 1));
  else graphics_draw_line(ctx, GPoint(a.x + 1, a.y), GPoint(b.x + 1, b.y));
}

static void poly(GContext *ctx, const uint8_t *xy, int n, bool fill) {
  GPoint pts[12];
  if (n > 12) n = 12;
  for (int i = 0; i < n; i++) pts[i] = P(xy[2 * i], xy[2 * i + 1]);
  GPath path = { .num_points = n, .points = pts, .rotation = 0, .offset = GPointZero };
  if (fill) gpath_draw_filled(ctx, &path);
  else gpath_draw_outline(ctx, &path);
}

static void circle(GContext *ctx, int x, int y, int r, bool fill) {
  if (fill) graphics_fill_circle(ctx, P(x, y), S(r));
  else graphics_draw_circle(ctx, P(x, y), S(r));
}

static void rect(GContext *ctx, int x1, int y1, int x2, int y2, int radius) {
  GPoint a = P(x1, y1), b = P(x2, y2);
  graphics_fill_rect(ctx, GRect(a.x, a.y, b.x - a.x, b.y - a.y), radius ? S(radius) : 0,
                     GCornersAll);
}

static void text_glyph(GContext *ctx, const char *t, GColor fg) {
  GFont f = F(s_sz >= 28 ? FONT_KEY_GOTHIC_28_BOLD : (s_sz >= 20 ? FONT_KEY_GOTHIC_24_BOLD : FONT_KEY_GOTHIC_18_BOLD));
  int h = s_sz >= 28 ? 28 : (s_sz >= 20 ? 24 : 18);
  graphics_context_set_text_color(ctx, fg);
  ui_text(ctx, t, f, GRect(s_c.x - s_sz / 2 - 4, s_c.y - h / 2 - h / 5, s_sz + 8, h + 4), GTextAlignmentCenter);
}

void ui_text(GContext *ctx, const char *text, GFont font, GRect box, int align) {
  graphics_draw_text(ctx, text, font, box,
                     (align & TXT_ELLIPSIS) ? GTextOverflowModeTrailingEllipsis : GTextOverflowModeFill,
                     (GTextAlignment)(align & 3), NULL);
}

void ui_fill_rect(GContext *ctx, GColor color, GRect box, int radius) {
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_rect(ctx, box, radius, radius ? GCornersAll : GCornerNone);
}

static int isqrt(int v) {
  if (v <= 0) return 0;
  int r = v, last;
  do { last = r; r = (r + v / r) / 2; } while (r < last);
  return last;
}

IconId icon_for_mode(int mode) {
  switch (mode) {
    case MODE_WALK: return ICON_WALK;
    case MODE_BIKE: return ICON_BIKE;
    case MODE_TRANSIT: return ICON_BUS;
    default: return ICON_CAR;
  }
}

// Icon shapes are tiny byte programs (all coordinates on the 40x40 grid):
//   LN x1 y1 x2 y2        line            CO/CF x y r   circle outline / filled
//   RC x1 y1 x2 y2 r      filled rect     PO/PF n x y.. polygon outline / filled
//   AR x y w h a0 a1      arc (angles in 15 degree steps)
//   COL 0|1               draw in background (0) or foreground (1) colour
// Each icon's program ends with END; programs are listed in IconId order.
#define END 0xFF
#define LN(a, b, c, d) 1, a, b, c, d
#define CO(x, y, r) 2, x, y, r
#define CF(x, y, r) 3, x, y, r
#define RC(a, b, c, d, r) 4, a, b, c, d, r
#define PO(n, ...) 5, n, __VA_ARGS__
#define PF(n, ...) 6, n, __VA_ARGS__
#define COL(c) 7, c
#define AR(x, y, w, h, a, b) 8, x, y, w, h, a, b
#define SPK 6, 4, 14, 12, 14, 22, 5, 22, 35, 12, 26, 4, 26
#define SPK2 PF(6, 2, 14, 9, 14, 18, 6, 18, 34, 9, 26, 2, 26)
#define EYE_SHAPE PF(8, 2, 20, 10, 11, 20, 8, 30, 11, 38, 20, 30, 29, 20, 32, 10, 29), \
    COL(0), CF(20, 20, 8), COL(1), CF(20, 20, 4)

static const uint8_t ICON_PROGS[] = {
  /* MIC */ RC(14, 3, 26, 25, 6), AR(9, 8, 22, 22, 6, 18), LN(20, 30, 20, 36), LN(13, 37, 27, 37), END,
  /* STAR */ END,
  /* STAR_OUTLINE */ END,
  /* EXPLORE */ CF(20, 14, 12), PF(3, 9, 20, 31, 20, 20, 39), COL(0), CF(20, 14, 5), END,
  /* DIRECTIONS */ PF(4, 20, 1, 39, 20, 20, 39, 1, 20), COL(0), LN(14, 28, 14, 20), LN(14, 20, 24, 20),
      PF(3, 23, 14, 30, 20, 23, 26), END,
  /* LIST */ CF(7, 10, 2), LN(14, 10, 35, 10), CF(7, 20, 2), LN(14, 20, 35, 20), CF(7, 30, 2),
      LN(14, 30, 35, 30), END,
  /* NAV */ PF(4, 20, 3, 34, 36, 20, 28, 6, 36), END,
  /* PIN */ CF(20, 14, 12), PF(3, 9, 19, 31, 19, 20, 38), COL(0), CF(20, 14, 4), END,
  /* CLOCK */ CO(20, 20, 16), LN(20, 20, 20, 10), LN(20, 20, 27, 24), END,
  /* MYLOC */ CO(20, 20, 12), CF(20, 20, 5), LN(20, 2, 20, 8), LN(20, 32, 20, 38), LN(2, 20, 8, 20),
      LN(32, 20, 38, 20), END,
  /* FLAG */ LN(10, 4, 10, 38), PF(5, 10, 5, 33, 5, 27, 12, 33, 20, 10, 20), END,
  /* SEARCH */ CO(17, 17, 11), LN(25, 25, 35, 35), END,
  /* PLUS */ LN(20, 6, 20, 34), LN(6, 20, 34, 20), END,
  /* CAR */ PF(4, 11, 8, 29, 8, 33, 19, 7, 19), RC(4, 18, 36, 31, 3), RC(7, 30, 13, 36, 1),
      RC(27, 30, 33, 36, 1), COL(0), PF(4, 13, 11, 27, 11, 29, 18, 11, 18), CF(10, 24, 2), CF(30, 24, 2), END,
  /* WALK */ CF(22, 6, 4), LN(21, 12, 17, 24), LN(17, 24, 11, 36), LN(17, 24, 23, 29), LN(23, 29, 25, 37),
      LN(20, 14, 13, 20), LN(20, 14, 27, 21), END,
  /* BIKE */ CO(9, 27, 7), CO(31, 27, 7), LN(9, 27, 17, 16), LN(17, 16, 28, 16), LN(28, 16, 31, 27),
      LN(17, 16, 21, 27), LN(21, 27, 9, 27), LN(15, 11, 21, 11), LN(28, 16, 26, 9), LN(24, 9, 30, 9), END,
  /* BUS */ RC(7, 3, 33, 32, 5), RC(9, 31, 15, 37, 1), RC(25, 31, 31, 37, 1), COL(0), RC(10, 8, 30, 19, 1),
      CF(13, 26, 2), CF(27, 26, 2), END,
  /* TRAIN */ RC(9, 3, 31, 30, 7), LN(14, 30, 9, 38), LN(26, 30, 31, 38), COL(0), RC(12, 8, 28, 17, 1),
      CF(14, 24, 2), CF(26, 24, 2), END,
  /* FERRY */ PF(4, 3, 24, 37, 24, 31, 33, 9, 33), RC(11, 12, 29, 24, 2), RC(17, 6, 23, 12, 0), COL(0),
      RC(14, 15, 18, 19, 0), RC(22, 15, 26, 19, 0), END,
  /* WARNING */ PF(3, 20, 3, 38, 36, 2, 36), COL(0), LN(20, 14, 20, 25), CF(20, 31, 2), END,
  /* PHONE */ RC(11, 2, 29, 38, 4), COL(0), RC(14, 7, 26, 30, 0), CF(20, 34, 2), END,
  /* COFFEE */ RC(7, 14, 27, 35, 4), CO(29, 22, 5), LN(12, 4, 12, 10), LN(17, 3, 17, 10), LN(22, 4, 22, 10), END,
  /* FOOD */ LN(13, 4, 13, 37), LN(8, 4, 8, 14), LN(18, 4, 18, 14), LN(8, 14, 18, 14),
      PF(4, 26, 3, 32, 7, 32, 22, 26, 22), LN(27, 22, 27, 37), END,
  /* GAS */ RC(7, 5, 24, 37, 2), LN(24, 13, 31, 18), LN(31, 18, 31, 31), LN(31, 31, 35, 31), COL(0),
      RC(10, 9, 21, 17, 0), END,
  /* CART */ LN(3, 7, 9, 7), LN(9, 7, 13, 27), LN(13, 27, 33, 27), PF(4, 10, 11, 37, 11, 33, 23, 12, 23),
      CF(15, 33, 3), CF(30, 33, 3), END,
  /* TREE */ CF(20, 15, 13), RC(17, 24, 23, 38, 0), END,
  /* BED */ RC(3, 19, 37, 29, 1), RC(3, 7, 8, 35, 1), RC(32, 28, 37, 35, 0), RC(10, 12, 18, 18, 2), END,
  /* ATM */ END,
  /* PHARMACY */ RC(15, 4, 25, 36, 1), RC(4, 15, 36, 25, 1), END,
  /* PLUG */ PF(7, 23, 2, 9, 22, 19, 22, 15, 38, 31, 16, 21, 16, 25, 2), END,
  /* PARKING */ END,
  /* BOOK */ PF(4, 3, 7, 19, 10, 19, 35, 3, 32), PF(4, 21, 10, 37, 7, 37, 32, 21, 35), END,
  /* MUTE */ SPK, LN(27, 14, 37, 26), LN(37, 14, 27, 26), END,
  /* SOUND */ SPK, AR(14, 10, 20, 20, 3, 9), AR(8, 4, 32, 32, 3, 9), END,
  /* CLOSE */ LN(8, 8, 32, 32), LN(32, 8, 8, 32), END,
  /* ZOOM_IN */ CO(16, 16, 12), LN(25, 25, 36, 36), LN(10, 16, 22, 16), LN(16, 10, 16, 22), END,
  /* ZOOM_OUT */ CO(16, 16, 12), LN(25, 25, 36, 36), LN(10, 16, 22, 16), END,
  /* MAP */ END,
  /* KEY */ CO(12, 20, 8), LN(20, 20, 37, 20), LN(31, 20, 31, 27), LN(36, 20, 36, 26), END,
  /* MOVE */ LN(20, 6, 20, 34), LN(6, 20, 34, 20), PF(3, 20, 1, 27, 9, 13, 9), PF(3, 20, 39, 27, 31, 13, 31),
      PF(3, 1, 20, 9, 13, 9, 27), PF(3, 39, 20, 31, 13, 31, 27), END,
  /* MORE */ CF(8, 20, 4), CF(20, 20, 4), CF(32, 20, 4), END,
  /* UP */ PF(7, 20, 4, 36, 22, 26, 22, 26, 36, 14, 36, 14, 22, 4, 22), END,
  /* DOWN */ PF(7, 20, 36, 36, 18, 26, 18, 26, 4, 14, 4, 14, 18, 4, 18), END,
  /* LEFT */ PF(7, 4, 20, 22, 36, 22, 26, 36, 26, 36, 14, 22, 14, 22, 4), END,
  /* RIGHT */ PF(7, 36, 20, 18, 36, 18, 26, 4, 26, 4, 14, 18, 14, 18, 4), END,
  /* EYE */ EYE_SHAPE, END,
  /* EYE_OFF */ EYE_SHAPE, END,
  /* STOP */ CF(20, 20, 18), COL(0), LN(13, 13, 27, 27), LN(27, 13, 13, 27), END,
  /* NORTH */ CO(20, 20, 18), PF(3, 20, 4, 26, 20, 14, 20), PO(3, 14, 20, 26, 20, 20, 36), END,
  /* HEADING */ CO(20, 20, 18), PF(4, 20, 7, 30, 31, 20, 25, 10, 31), END,
  /* TRIANGLE */ PF(3, 20, 6, 35, 33, 5, 33), END,
  /* VIBRATE */ LN(6, 12, 6, 28), LN(34, 12, 34, 28), LN(1, 16, 1, 24), LN(39, 16, 39, 24), END,
  /* VOL_UP */ SPK2, LN(24, 20, 38, 20), LN(31, 13, 31, 27), END,
  /* VOL_DOWN */ SPK2, LN(24, 20, 38, 20), END,
  /* INFO */ CO(20, 20, 18), CF(20, 11, 3), RC(17, 17, 23, 31, 1), RC(14, 17, 23, 20, 0), END,
  /* GLOBE */ CO(20, 20, 17), LN(4, 20, 36, 20), AR(12, 3, 16, 34, 0, 24), END,
  /* POI_NAMES */ END,
  /* BAR */ PF(3, 6, 6, 34, 6, 20, 22), RC(18, 20, 22, 33, 0), RC(11, 32, 29, 36, 1), END,
  /* NORTH_UP */ PF(4, 20, 20, 29, 38, 20, 33, 11, 38), LN(13, 15, 13, 2), LN(13, 2, 27, 15), LN(27, 15, 27, 2), END,
  /* GEAR */ LN(20, 3, 20, 37), LN(3, 20, 37, 20), LN(8, 8, 32, 32), LN(32, 8, 8, 32), CF(20, 20, 13),
      COL(0), CF(20, 20, 5), END,
};

// Play one icon's byte program
static void icon_run(GContext *ctx, IconId id, GColor fg, GColor bg) {
  const uint8_t *p = ICON_PROGS;
  for (int i = 1; i < id; i++) {
    while (*p != END) p++;
    p++;
  }
  while (*p != END) {
    uint8_t op = *p++;
    if (op == 1) {
      line(ctx, p[0], p[1], p[2], p[3]);
      p += 4;
    } else if (op <= 3) {
      circle(ctx, p[0], p[1], p[2], op == 3);
      p += 3;
    } else if (op == 4) {
      rect(ctx, p[0], p[1], p[2], p[3], p[4]);
      p += 5;
    } else if (op <= 6) {
      poly(ctx, p + 1, p[0], op == 6);
      p += 1 + 2 * p[0];
    } else if (op == 7) {
      GColor c = *p++ ? fg : bg;
      graphics_context_set_fill_color(ctx, c);
      graphics_context_set_stroke_color(ctx, c);
    } else {
      GPoint o = P(p[0], p[1]);
      graphics_draw_arc(ctx, GRect(o.x, o.y, S(p[2]), S(p[3])), GOvalScaleModeFitCircle,
                        p[4] * TRIG_MAX_ANGLE / 24, p[5] * TRIG_MAX_ANGLE / 24);
      p += 6;
    }
  }
}

void icon_draw(GContext *ctx, IconId id, GPoint c, int size, GColor fg, GColor bg) {
  s_c = c;
  s_sz = size;
  // every line icon uses the same 2 px stroke (1 px only on tiny icons)
  int sw = size >= 14 ? 2 : 1;
#ifdef PBL_COLOR
  graphics_context_set_antialiased(ctx, true);
#endif
  graphics_context_set_fill_color(ctx, fg);
  graphics_context_set_stroke_color(ctx, fg);
  graphics_context_set_stroke_width(ctx, sw);

  // shapes that need more than the byte programs can say
  if (id == ICON_STAR || id == ICON_STAR_OUTLINE) {
    // classic 5-point star, built symmetrically around the vertical axis
    int R = size / 2, r = size * 20 / 100;
    GPoint pts[10];
    for (int i = 0; i <= 5; i++) {
      int rad = (i % 2 == 0) ? R : r;
      int32_t ang = TRIG_MAX_ANGLE * i / 10;
      int x = (int)(sin_lookup(ang) * rad / TRIG_MAX_RATIO);
      int y = (int)(-cos_lookup(ang) * rad / TRIG_MAX_RATIO) + size / 12;
      pts[i] = GPoint(c.x + x, c.y + y);
      if (i > 0 && i < 5) pts[10 - i] = GPoint(c.x - x, c.y + y);
    }
    GPath path = { .num_points = 10, .points = pts };
    if (id == ICON_STAR) {
      gpath_draw_filled(ctx, &path);
      graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(gcolor_equal(fg, C_STAR) ? C_STAR_EDGE : fg, fg));
      graphics_context_set_stroke_width(ctx, 1);
    }
    gpath_draw_outline(ctx, &path);
  } else if (id == ICON_ATM || id == ICON_PARKING) {
    text_glyph(ctx, id == ICON_ATM ? "$" : "P", fg);
  } else if (id == ICON_POI_NAMES) {
    // a tiny name pill: dot + "A"
    GPoint a = P(1, 9), b = P(39, 31);
    GRect r = GRect(a.x, a.y, b.x - a.x, b.y - a.y);
    ui_fill_rect(ctx, GColorWhite, r, r.size.h / 2);
    graphics_context_set_stroke_color(ctx, fg);
    graphics_context_set_stroke_width(ctx, 1);
    graphics_draw_round_rect(ctx, r, r.size.h / 2);
    graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorOrange, GColorBlack));
    circle(ctx, 11, 20, 5, true);
    graphics_context_set_text_color(ctx, fg);
    GPoint t = P(19, 9);
    ui_text(ctx, "A", F(FONT_KEY_GOTHIC_14_BOLD), GRect(t.x, t.y - 4, S(18), 18), GTextAlignmentCenter);
  } else if (id == ICON_MAP) {
    // folded paper map: crisp 2 px outline and folds
#ifdef PBL_COLOR
    graphics_context_set_antialiased(ctx, false);
#endif
    static const int8_t m[] = {4, 11, 14, 5, 26, 11, 36, 5, 36, 30, 26, 36, 14, 30, 4, 36};
    for (int i = 0; i < 8; i++) {
      int j = (i + 1) % 8;
      line2(ctx, m[2 * i], m[2 * i + 1], m[2 * j], m[2 * j + 1]);
    }
    line2(ctx, 14, 5, 14, 30);
    line2(ctx, 26, 11, 26, 36);
#ifdef PBL_COLOR
    graphics_context_set_antialiased(ctx, true);
#endif
  } else {
    if (id == ICON_VIBRATE) {
      // a watch shaking: outline body with buzz marks either side
      graphics_draw_round_rect(ctx, GRect(P(13, 7).x, P(13, 7).y, S(14), S(26)), S(3));
    } else if (id == ICON_GEAR) {
      // a cog: four thick bars make eight teeth, a ring on top, a hole in the middle
      int tw = S(7);
      graphics_context_set_stroke_width(ctx, tw < 2 ? 2 : tw);
    }
    icon_run(ctx, id, fg, bg);
    if (id == ICON_EYE_OFF) {
      graphics_context_set_stroke_color(ctx, bg);
      graphics_context_set_stroke_width(ctx, sw + 2);
      line(ctx, 6, 35, 34, 5);
      graphics_context_set_stroke_color(ctx, fg);
      graphics_context_set_stroke_width(ctx, sw);
      line(ctx, 6, 35, 34, 5);
    }
  }
  graphics_context_set_stroke_width(ctx, 1);
}

// ---------------------------------------------------------------------------
// Maneuver arrows: thick polyline + arrow head. Designed for right turns,
// mirrored for left.
// ---------------------------------------------------------------------------
typedef struct {
  int8_t n;
  int8_t pts[12];
  int8_t ghost[4];  // optional faint branch (x1,y1,x2,y2), all zero = none
} ManShape;

static const ManShape MAN_SHAPES[] = {
  [MAN_STRAIGHT]     = {2, {20, 38, 20, 8}, {0}},
  [MAN_SLIGHT_RIGHT] = {3, {15, 38, 15, 24, 27, 11}, {0}},
  [MAN_RIGHT]        = {3, {12, 38, 12, 19, 30, 19}, {0}},
  [MAN_SHARP_RIGHT]  = {3, {13, 38, 13, 9, 28, 25}, {0}},
  [MAN_UTURN_RIGHT]  = {6, {11, 38, 11, 15, 15, 8, 25, 8, 29, 15, 29, 26}, {0}},
  [MAN_RAMP_RIGHT]   = {3, {15, 38, 15, 24, 27, 11}, {15, 24, 15, 4}},
  [MAN_FORK_RIGHT]   = {3, {20, 38, 20, 25, 29, 12}, {20, 25, 11, 12}},
  [MAN_MERGE]        = {3, {28, 38, 20, 24, 20, 8}, {12, 38, 20, 24}},
};

static void draw_shape(GContext *ctx, const ManShape *sh, bool mirror, GColor fg, GColor ghost) {
  int sw = s_sz / 7;
  if (sw < 2) sw = 2;
  GPoint pts[6];
  for (int i = 0; i < sh->n; i++) {
    int x = sh->pts[2 * i];
    if (mirror) x = 40 - x;
    pts[i] = P(x, sh->pts[2 * i + 1]);
  }
  if (sh->ghost[0] || sh->ghost[1]) {
    graphics_context_set_stroke_color(ctx, ghost);
    graphics_context_set_stroke_width(ctx, sw > 3 ? sw - 2 : 2);
    int x1 = mirror ? 40 - sh->ghost[0] : sh->ghost[0];
    int x2 = mirror ? 40 - sh->ghost[2] : sh->ghost[2];
    graphics_draw_line(ctx, P(x1, sh->ghost[1]), P(x2, sh->ghost[3]));
  }
  graphics_context_set_stroke_color(ctx, fg);
  graphics_context_set_fill_color(ctx, fg);
  graphics_context_set_stroke_width(ctx, sw);
  // shorten the final segment so the head sits at the end
  GPoint a = pts[sh->n - 2], b = pts[sh->n - 1];
  int dx = b.x - a.x, dy = b.y - a.y;
  int len = isqrt(dx * dx + dy * dy);
  if (len == 0) len = 1;
  int hl = sw * 2;       // head length
  int hw = sw * 3 / 2 + 1;  // head half-width
  GPoint base = GPoint(b.x - dx * hl / len, b.y - dy * hl / len);
  for (int i = 0; i < sh->n - 2; i++) graphics_draw_line(ctx, pts[i], pts[i + 1]);
  graphics_draw_line(ctx, a, base);
  for (int i = 1; i < sh->n - 1; i++) graphics_fill_circle(ctx, pts[i], sw / 2);
  GPoint head[3] = {
    b,
    GPoint(base.x + (-dy) * hw / len, base.y + dx * hw / len),
    GPoint(base.x - (-dy) * hw / len, base.y - dx * hw / len),
  };
  GPath path = { .num_points = 3, .points = head };
  gpath_draw_filled(ctx, &path);
  graphics_context_set_stroke_width(ctx, 1);
}

void maneuver_draw(GContext *ctx, int code, GPoint c, int size, GColor fg, GColor ghost) {
  s_c = c;
  s_sz = size;
#ifdef PBL_COLOR
  graphics_context_set_antialiased(ctx, true);
#endif
  switch (code) {
    case MAN_STRAIGHT:
    case MAN_NONE:
      draw_shape(ctx, &MAN_SHAPES[MAN_STRAIGHT], false, fg, ghost);
      break;
    case MAN_SLIGHT_RIGHT: case MAN_RIGHT: case MAN_SHARP_RIGHT: case MAN_UTURN_RIGHT:
    case MAN_RAMP_RIGHT: case MAN_FORK_RIGHT: case MAN_MERGE:
      draw_shape(ctx, &MAN_SHAPES[code], false, fg, ghost);
      break;
    case MAN_SLIGHT_LEFT: draw_shape(ctx, &MAN_SHAPES[MAN_SLIGHT_RIGHT], true, fg, ghost); break;
    case MAN_LEFT: draw_shape(ctx, &MAN_SHAPES[MAN_RIGHT], true, fg, ghost); break;
    case MAN_SHARP_LEFT: draw_shape(ctx, &MAN_SHAPES[MAN_SHARP_RIGHT], true, fg, ghost); break;
    case MAN_UTURN_LEFT: draw_shape(ctx, &MAN_SHAPES[MAN_UTURN_RIGHT], true, fg, ghost); break;
    case MAN_RAMP_LEFT: draw_shape(ctx, &MAN_SHAPES[MAN_RAMP_RIGHT], true, fg, ghost); break;
    case MAN_FORK_LEFT: draw_shape(ctx, &MAN_SHAPES[MAN_FORK_RIGHT], true, fg, ghost); break;
    case MAN_ROUNDABOUT_LEFT:
    case MAN_ROUNDABOUT_RIGHT: {
      bool left = code == MAN_ROUNDABOUT_LEFT;
      graphics_context_set_stroke_color(ctx, ghost);
      graphics_context_set_stroke_width(ctx, s_sz / 10 > 1 ? s_sz / 10 : 2);
      graphics_draw_circle(ctx, P(20, 17), S(9));
      ManShape sh = {3, {20, 38, 20, 26, 31, 9}, {0}};
      draw_shape(ctx, &sh, left, fg, ghost);
      break;
    }
    case MAN_DEPART: icon_draw(ctx, ICON_NAV, c, size, fg, GColorWhite); break;
    case MAN_ARRIVE: icon_draw(ctx, ICON_FLAG, c, size, fg, GColorWhite); break;
    case MAN_FERRY: icon_draw(ctx, ICON_FERRY, c, size, fg, ghost); break;
    case MAN_BUS: icon_draw(ctx, ICON_BUS, c, size, fg, ghost); break;
    case MAN_TRAIN: case MAN_SUBWAY: case MAN_TRAM:
      icon_draw(ctx, ICON_TRAIN, c, size, fg, ghost);
      break;
    case MAN_WALK: icon_draw(ctx, ICON_WALK, c, size, fg, ghost); break;
    default:
      draw_shape(ctx, &MAN_SHAPES[MAN_STRAIGHT], false, fg, ghost);
      break;
  }
}

// ---------------------------------------------------------------------------
// Map glyphs
// ---------------------------------------------------------------------------
void draw_pin(GContext *ctx, GPoint tip, int r, bool selected) {
  if (selected) r = r * 3 / 2;
  GPoint c = GPoint(tip.x, tip.y - r * 2);
#ifdef PBL_COLOR
  graphics_context_set_antialiased(ctx, true);
#endif
  // body
  GPoint tri[3] = { GPoint(c.x - r + 1, c.y + r / 2), GPoint(c.x + r - 1, c.y + r / 2), tip };
  GPath path = { .num_points = 3, .points = tri };
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(C_RED_DARK, GColorWhite));
  graphics_fill_circle(ctx, c, r + 1);
  graphics_context_set_fill_color(ctx, selected ? C_RED : PBL_IF_COLOR_ELSE(GColorSunsetOrange, GColorBlack));
  graphics_fill_circle(ctx, c, r);
  gpath_draw_filled(ctx, &path);
#ifdef PBL_BW
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_draw_circle(ctx, c, r);
#endif
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(C_RED_DARK, GColorWhite));
  graphics_fill_circle(ctx, c, r / 3 + 1);
}

void draw_me_dot(GContext *ctx, GPoint c, int r) {
#ifdef PBL_COLOR
  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_stroke_color(ctx, C_BLUE_LT);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_circle(ctx, c, r * 3);
#endif
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorWhite, GColorBlack));
  graphics_fill_circle(ctx, c, r + 2);
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(C_BLUE, GColorWhite));
  graphics_fill_circle(ctx, c, r);
#ifdef PBL_BW
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, c, r / 2);
#endif
}

void draw_puck(GContext *ctx, GPoint c, int r, int heading_deg) {
#ifdef PBL_COLOR
  graphics_context_set_antialiased(ctx, true);
#endif
  // arrow pointing up, then rotated to the heading (0 = north/up)
  int ox[4] = { 0, r + 3, 0, -r - 3 }, oy[4] = { -r - 4, r + 3, r / 2 + 2, r + 3 };
  int ix[4] = { 0, r, 0, -r }, iy[4] = { -r, r, r / 2, r };
  int32_t a = DEG_TO_TRIGANGLE(heading_deg);
  int32_t sn = sin_lookup(a), cs = cos_lookup(a);
  GPoint outer[4], inner[4];
  for (int i = 0; i < 4; i++) {
    outer[i] = GPoint(c.x + (ox[i] * cs - oy[i] * sn) / TRIG_MAX_RATIO, c.y + (ox[i] * sn + oy[i] * cs) / TRIG_MAX_RATIO);
    inner[i] = GPoint(c.x + (ix[i] * cs - iy[i] * sn) / TRIG_MAX_RATIO, c.y + (ix[i] * sn + iy[i] * cs) / TRIG_MAX_RATIO);
  }
  GPath po = { .num_points = 4, .points = outer };
  GPath pi = { .num_points = 4, .points = inner };
  graphics_context_set_fill_color(ctx, GColorWhite);
  gpath_draw_filled(ctx, &po);
  graphics_context_set_stroke_color(ctx, GColorBlack);
  graphics_context_set_stroke_width(ctx, 1);
  gpath_draw_outline(ctx, &po);
  graphics_context_set_fill_color(ctx, C_BLUE);
  gpath_draw_filled(ctx, &pi);
}

GColor poi_color(int cat) {
  return map_cat_color(cat);
}

void draw_poi(GContext *ctx, GPoint c, int cat) {
  int r = g_fonts.level >= 1 ? 5 : 4;
#ifdef PBL_COLOR
  graphics_context_set_antialiased(ctx, true);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, c, r + 1);
  graphics_context_set_fill_color(ctx, poi_color(cat));
  graphics_fill_circle(ctx, c, r);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, c, 1);
#else
  // hollow, so it reads differently from your (filled) dot
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, c, r + 1);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, c, r - 1);
#endif
}

#if !PM_LOWMEM
GRect poi_label_rect(GPoint c, const char *name, bool left) {
  int r = g_fonts.level >= 1 ? 5 : 4;
  int tw = graphics_text_layout_get_content_size(name, g_fonts.chip, GRect(0, 0, 120, 16),
                                                 GTextOverflowModeTrailingEllipsis, GTextAlignmentLeft).w;
  int w = 2 * r + 6 + tw + 4;
  return GRect(left ? c.x + r + 3 - w : c.x - r - 3, c.y - 7, w, 15);
}

void draw_poi_label(GContext *ctx, GPoint c, int cat, const char *name, GRect pill) {
  bool dark = g_app.dark_map;
  ui_fill_rect(ctx, dark ? GColorBlack : GColorWhite, pill, pill.size.h / 2);
  graphics_context_set_stroke_color(ctx, dark ? PBL_IF_COLOR_ELSE(GColorDarkGray, GColorWhite) : C_DIVIDER);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_round_rect(ctx, pill, pill.size.h / 2);
  int r = g_fonts.level >= 1 ? 5 : 4;
  graphics_context_set_fill_color(ctx, poi_color(cat));
  graphics_fill_circle(ctx, c, r);
  graphics_context_set_text_color(ctx, dark ? GColorWhite : GColorBlack);
  bool left = c.x > pill.origin.x + pill.size.w / 2;   // the name sits on the dot's left
  int tx = left ? pill.origin.x + 4 : c.x + r + 3;
  ui_text(ctx, name, g_fonts.chip, GRect(tx, pill.origin.y - 3, pill.size.w - 2 * r - 6, 16), GTextAlignmentLeft | TXT_ELLIPSIS);
}

#endif

void draw_compass(GContext *ctx, GPoint cc, int map_heading) {
#ifdef PBL_COLOR
  graphics_context_set_antialiased(ctx, true);
#endif
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, cc, 11);
  graphics_context_set_stroke_color(ctx, C_DIVIDER);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_circle(ctx, cc, 11);
  int32_t a = DEG_TO_TRIGANGLE(-map_heading);
  int sx = sin_lookup(a) * 8 / TRIG_MAX_RATIO, sy = -cos_lookup(a) * 8 / TRIG_MAX_RATIO;
  int px = cos_lookup(a) * 3 / TRIG_MAX_RATIO, py = sin_lookup(a) * 3 / TRIG_MAX_RATIO;
  GPoint n[3] = { GPoint(cc.x + sx, cc.y + sy), GPoint(cc.x + px, cc.y + py), GPoint(cc.x - px, cc.y - py) };
  GPoint so[3] = { GPoint(cc.x - sx, cc.y - sy), GPoint(cc.x + px, cc.y + py), GPoint(cc.x - px, cc.y - py) };
  GPath pn = { .num_points = 3, .points = n }, ps = { .num_points = 3, .points = so };
  graphics_context_set_fill_color(ctx, C_RED);
  gpath_draw_filled(ctx, &pn);
  graphics_context_set_fill_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
  gpath_draw_filled(ctx, &ps);
}

void draw_fab(GContext *ctx, GPoint c, int r, IconId icon, GColor fill) {
#ifdef PBL_COLOR
  graphics_context_set_antialiased(ctx, true);
#endif
  graphics_context_set_fill_color(ctx, fill);
  graphics_fill_circle(ctx, c, r);
  icon_draw(ctx, icon, c, r + r / 4, GColorWhite, fill);
}

void draw_card(GContext *ctx, GRect r, int radius) {
  ui_fill_rect(ctx, C_BG, r, radius);
  graphics_context_set_stroke_color(ctx, C_DIVIDER);
  graphics_context_set_stroke_width(ctx, 1);
  graphics_draw_round_rect(ctx, r, radius);
}

void draw_map_placeholder(GContext *ctx, GRect r) {
  // a faint grid in the map's own background color (dark maps get a dark grid)
  ui_fill_rect(ctx, g_app.dark_map ? PBL_IF_COLOR_ELSE(GColorOxfordBlue, GColorBlack) : GColorWhite, r, 0);
  graphics_context_set_stroke_color(ctx, g_app.dark_map ? PBL_IF_COLOR_ELSE(GColorDukeBlue, GColorWhite)
                                                       : PBL_IF_COLOR_ELSE(GColorLightGray, GColorBlack));
  graphics_context_set_stroke_width(ctx, 1);
  for (int x = r.origin.x + 12; x < r.origin.x + r.size.w; x += 24) {
    for (int y = r.origin.y; y < r.origin.y + r.size.h; y += PBL_IF_COLOR_ELSE(1, 3)) {
      graphics_draw_pixel(ctx, GPoint(x, y));
    }
  }
  for (int y = r.origin.y + 12; y < r.origin.y + r.size.h; y += 24) {
    for (int x = r.origin.x; x < r.origin.x + r.size.w; x += PBL_IF_COLOR_ELSE(1, 3)) {
      graphics_draw_pixel(ctx, GPoint(x, y));
    }
  }
}

// ---------------------------------------------------------------------------
// Loading dots
// ---------------------------------------------------------------------------
typedef struct {
  AppTimer *timer;
  int phase;
  bool running;
} DotsData;

static void dots_tick(void *ctx) {
  Layer *layer = ctx;
  DotsData *d = layer_get_data(layer);
  d->timer = NULL;
  if (!d->running) return;
  d->phase = (d->phase + 2) % 36;   // 9 frames a second: smooth enough, and leaves the watch free for messages
  layer_mark_dirty(layer);
  d->timer = app_timer_register(110, dots_tick, layer);
}

static void dots_update(Layer *layer, GContext *ctx) {
  DotsData *d = layer_get_data(layer);
  GRect b = layer_get_bounds(layer);
  static const uint8_t cols[4] = {
#ifdef PBL_COLOR
    GColorBlueMoonARGB8, GColorRedARGB8, GColorChromeYellowARGB8, GColorIslamicGreenARGB8
#else
    GColorBlackARGB8, GColorBlackARGB8, GColorBlackARGB8, GColorBlackARGB8
#endif
  };
  int r = 4, gap = 14;
  int x0 = b.size.w / 2 - gap * 3 / 2;
#ifdef PBL_COLOR
  graphics_context_set_antialiased(ctx, true);
#endif
  for (int i = 0; i < 4; i++) {
    int32_t ang = TRIG_MAX_ANGLE * ((d->phase + i * 4) % 36) / 36;
    int dy = (int)(sin_lookup(ang) * 6 / TRIG_MAX_RATIO);
    if (dy > 0) dy = dy / 3;
    graphics_context_set_fill_color(ctx, (GColor){ .argb = cols[i] });
    graphics_fill_circle(ctx, GPoint(x0 + i * gap, b.size.h / 2 - dy), r);
  }
}

Layer *dots_layer_create(GRect frame) {
  Layer *layer = layer_create_with_data(frame, sizeof(DotsData));
  DotsData *d = layer_get_data(layer);
  d->timer = NULL;
  d->phase = 0;
  d->running = false;
  layer_set_update_proc(layer, dots_update);
  return layer;
}

void dots_layer_set_running(Layer *layer, bool running) {
  if (!layer) return;
  DotsData *d = layer_get_data(layer);
  layer_set_hidden(layer, !running);
  if (running == d->running) return;
  d->running = running;
  if (running && !d->timer) d->timer = app_timer_register(110, dots_tick, layer);
  if (!running && d->timer) {
    app_timer_cancel(d->timer);
    d->timer = NULL;
  }
}

void dots_layer_destroy(Layer *layer) {
  if (!layer) return;
  dots_layer_set_running(layer, false);
  layer_destroy(layer);
}

// ---------------------------------------------------------------------------
// Action strip
// ---------------------------------------------------------------------------
static GColor or_default(GColor c, GColor def) { return c.argb ? c : def; }

// A row of small dots, the current one filled blue
static void strip_dots(GContext *ctx, int cx, int y, int page, int pages) {
  int dx0 = cx - (pages - 1) * 3;
  for (int i = 0; i < pages; i++) {
    GPoint d = GPoint(dx0 + i * 6, y);
    if (i == page) {
      graphics_context_set_fill_color(ctx, C_BLUE);
      graphics_fill_circle(ctx, d, 2);
    } else {
      graphics_context_set_stroke_color(ctx, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
      graphics_context_set_stroke_width(ctx, 1);
      graphics_draw_circle(ctx, d, 2);
    }
  }
}

void draw_action_strip(GContext *ctx, GRect bounds, const StripIcons *ic) {
  int w = STRIP_W;
  int cx;
  int isz = PBL_IF_ROUND_ELSE(20, w >= 36 ? 22 : 19);
#ifdef PBL_ROUND
  int R = bounds.size.h;
  graphics_context_set_fill_color(ctx, C_BG);
  graphics_fill_circle(ctx, GPoint(bounds.size.w - w + R, bounds.size.h / 2), R);
  graphics_context_set_stroke_color(ctx, C_DIVIDER);
  graphics_draw_circle(ctx, GPoint(bounds.size.w - w + R, bounds.size.h / 2), R);
  cx = bounds.size.w - w / 2 - 4;
  int dy = bounds.size.h / 4;
#else
  GRect strip = GRect(bounds.size.w - w, 0, w, bounds.size.h);
  ui_fill_rect(ctx, C_BG, strip, 0);
  graphics_context_set_stroke_color(ctx, C_DIVIDER);
  graphics_draw_line(ctx, GPoint(strip.origin.x, 0), GPoint(strip.origin.x, bounds.size.h));
  cx = strip.origin.x + w / 2;
  int dy = bounds.size.h / 2 - 26;
  if (dy > 70) dy = 70;
#endif
  int cy = bounds.size.h / 2;
  if (ic->up) icon_draw(ctx, ic->up, GPoint(cx, cy - dy), isz, or_default(ic->up_color, C_ICON), C_BG);
  if (ic->select) {
    if (ic->select_plain) {
      icon_draw(ctx, ic->select, GPoint(cx, cy), isz - 2, C_ICON, C_BG);
    } else {
      draw_fab(ctx, GPoint(cx, cy), w >= 36 ? 15 : 12, ic->select, or_default(ic->select_color, C_BLUE));
    }
  }
  if (ic->up_pages > 1) strip_dots(ctx, cx, cy - dy + isz / 2 + 6, ic->up_page, ic->up_pages);
  if (ic->pages > 1) strip_dots(ctx, cx, cy + 13, ic->page, ic->pages);
  if (ic->down) icon_draw(ctx, ic->down, GPoint(cx, cy + dy), isz, or_default(ic->down_color, C_ICON), C_BG);
}

void draw_side_tab(GContext *ctx, GRect bounds) {
  int r = PBL_IF_ROUND_ELSE(16, 14);
  GPoint c = GPoint(bounds.origin.x + bounds.size.w + PBL_IF_ROUND_ELSE(0, 2), bounds.origin.y + bounds.size.h / 2);
#ifdef PBL_COLOR
  graphics_context_set_antialiased(ctx, true);
#endif
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_circle(ctx, c, r);
  graphics_context_set_fill_color(ctx, GColorWhite);
  for (int i = -1; i <= 1; i++) graphics_fill_circle(ctx, GPoint(c.x - r / 2 - 1, c.y + i * 6), 1);
}

// Right-edge half circle with a lowercase "i": "press Select for more info"
void draw_info_tab(GContext *ctx, GRect cell, bool hl) {
  int r = 11;
  GPoint c = GPoint(cell.origin.x + cell.size.w, cell.origin.y + cell.size.h / 2);
#ifdef PBL_COLOR
  graphics_context_set_antialiased(ctx, true);
#endif
  graphics_context_set_fill_color(ctx, hl ? GColorWhite : C_BLUE);
  graphics_fill_circle(ctx, c, r);
  GColor fg = hl ? C_BLUE : GColorWhite;
  graphics_context_set_fill_color(ctx, fg);
  graphics_fill_circle(ctx, GPoint(c.x - 5, c.y - 5), 1);
  graphics_fill_rect(ctx, GRect(c.x - 6, c.y - 2, 2, 8), 0, GCornerNone);
}

// ---------------------------------------------------------------------------
// Toast
// ---------------------------------------------------------------------------
static TextLayer *s_toast;
static AppTimer *s_toast_timer;
static char s_toast_text[64];

static void toast_hide(void *ctx) {
  s_toast_timer = NULL;
  if (s_toast) {
    layer_remove_from_parent(text_layer_get_layer(s_toast));
    text_layer_destroy(s_toast);
    s_toast = NULL;
  }
}

void ui_toast_cancel(void) {
  if (s_toast_timer) app_timer_cancel(s_toast_timer);
  toast_hide(NULL);
}

void ui_toast(Window *window, const char *text) {
  ui_toast_cancel();
  if (!window) return;
  Layer *root = window_get_root_layer(window);
  GRect b = layer_get_bounds(root);
  strncpy(s_toast_text, text, sizeof(s_toast_text) - 1);
  s_toast_text[sizeof(s_toast_text) - 1] = 0;
  int w = b.size.w - PBL_IF_ROUND_ELSE(50, 16);
  GSize sz = graphics_text_layout_get_content_size(s_toast_text, g_fonts.body_b,
                                                   GRect(0, 0, w - 8, 60),
                                                   GTextOverflowModeWordWrap, GTextAlignmentCenter);
  int h = sz.h + 10;
  s_toast = text_layer_create(GRect((b.size.w - w) / 2, b.size.h - h - PBL_IF_ROUND_ELSE(22, 6), w, h));
  text_layer_set_background_color(s_toast, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
  text_layer_set_text_color(s_toast, GColorWhite);
  text_layer_set_font(s_toast, g_fonts.body_b);
  text_layer_set_text_alignment(s_toast, GTextAlignmentCenter);
  text_layer_set_text(s_toast, s_toast_text);
  layer_add_child(root, text_layer_get_layer(s_toast));
  s_toast_timer = app_timer_register(2200, toast_hide, NULL);
}

void format_clock(time_t t, char *buf, size_t len) {
  struct tm *tm = localtime(&t);
  if (clock_is_24h_style()) {
    strftime(buf, len, "%H:%M", tm);
  } else {
    strftime(buf, len, "%I:%M %p", tm);
    if (buf[0] == '0') memmove(buf, buf + 1, strlen(buf));
  }
}

void vibe_soon(void) {
  if (!g_app.vibrate) return;
  static const uint32_t segs[] = {120, 120, 120};
  VibePattern p = { .durations = segs, .num_segments = ARRAY_LENGTH(segs) };
  vibes_enqueue_custom_pattern(p);
}

void vibe_now(void) {
  if (!g_app.vibrate) return;
  static const uint32_t segs[] = {400, 150, 400};
  VibePattern p = { .durations = segs, .num_segments = ARRAY_LENGTH(segs) };
  vibes_enqueue_custom_pattern(p);
}

// --- List dividers -------------------------------------------------------------------
#define SEP_PAD 3
int16_t ui_separator_h(MenuLayer *menu, MenuIndex *index, void *ctx) {
  return SEP_PAD * 2 + 1;
}

void ui_draw_separator(GContext *ctx, const Layer *cell, MenuIndex *index, void *cb) {
  GRect b = layer_get_bounds(cell);
  ui_fill_rect(ctx, C_BG, b, 0);
  graphics_context_set_stroke_color(ctx, C_DIVIDER);
  int inset = PBL_IF_ROUND_ELSE(24, 6);
  graphics_draw_line(ctx, GPoint(inset, SEP_PAD), GPoint(b.size.w - inset, SEP_PAD));
}
