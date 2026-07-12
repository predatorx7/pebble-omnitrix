#include <pebble.h>

// ---------------------------------------------------------------------------
// Omnitrix Game — declarative node router
//
// The game is described as DATA wherever possible:
//
//   * NODES[]  — one row per screen. A node declares how it renders, an
//                optional timed auto-advance (auto_frames -> auto_to), an
//                optional in-node action (on_click), and flags (locked /
//                transient). Timed "journeys" are therefore data, not code.
//   * NAV[]    — declarative navigation edges: "from a node, this button leads
//                to that node", optionally played through a colour flash.
//
// The router composes cross-cutting behaviour on top of the tables: global
// long-press toggles, a red-splash "locked" node, transient nodes that ignore
// input, a per-node frame counter, and a vibration pulse on every transition.
//
// Rendering is built by COMPOSITION: small primitives (badge_ring, hourglass,
// frame, shapes) combine into a badge() combinator that each node's renderer
// composes with overlays.
// ---------------------------------------------------------------------------

typedef enum {
  NODE_IDLE = 0,
  NODE_DNA_SCAN,
  NODE_SELECT_ENTER,
  NODE_TRANSFORM_SELECT,
  NODE_ACTIVATING,
  NODE_TRANSFORMED,
  NODE_DEACTIVATING,
  NODE_RECALIBRATION,
  NODE_MASTER_CONTROL,
  NODE_HIJACKED,
  NODE_ALIEN_SCAN,
  NODE_PURPLE,
  NODE_FLASH,
  NODE_COUNT,
} NodeId;

#define NODE_STAY (-1)

#define NUM_ALIENS 10
#define ANIM_INTERVAL_MS 120
#define SECS_TO_FRAMES(s) ((s) * 1000 / ANIM_INTERVAL_MS)

// Timed journeys, declared in frames (derived from seconds).
#define TRANSFORM_FRAMES       SECS_TO_FRAMES(60)   // transformation lasts 1 min
#define DISCHARGE_RED_FRAMES   SECS_TO_FRAMES(30)   // hold red after discharge
#define RECAL_FLASH_FRAMES     SECS_TO_FRAMES(1)    // flash green
#define RECHARGE_GREEN_FRAMES  SECS_TO_FRAMES(2)    // hold green
#define RECAL_TOTAL_FRAMES     (DISCHARGE_RED_FRAMES + RECAL_FLASH_FRAMES + RECHARGE_GREEN_FRAMES)
#define TIMEOUT_WARN_FRAMES    SECS_TO_FRAMES(4)  // green/red flash before timeout
#define ACT_TOTAL_FRAMES       8    // lock-in flash (~1s)
#define DEACT_RED_FRAMES       5    // red-spark phase of power-down
#define DEACT_TOTAL_FRAMES     10   // red -> green -> idle
#define FLASH_FRAMES           6    // default strobe flash length
#define GREEN_FLASH_FRAMES     5    // idle -> select solid flash
#define SPLASH_FRAMES          3    // red "locked" splash
#define SELECT_ENTER_SPARK_FRAMES 2               // idle->select: emblem sparks green in place
#define SELECT_ENTER_MOVE_FRAMES  SECS_TO_FRAMES(1)  // then triangles slide fully across center
#define SELECT_ENTER_FRAMES    (SELECT_ENTER_SPARK_FRAMES + SELECT_ENTER_MOVE_FRAMES)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static Window *s_main_window;
static Layer *s_omnitrix_layer;
static AppTimer *s_anim_timer;

static int s_node = NODE_IDLE;
static uint32_t s_node_frame = 0;      // frames since entering the current node
static uint32_t s_anim_tick = 0;       // free-running (for continuous effects)
static bool s_omniboost = false;
static int s_selected_alien = 0;
static int s_splash_frame = 0;

// Flash transition parameters.
static bool s_flash_solid = false;
static int s_flash_len = 0;
static int s_flash_target = NODE_IDLE;
static GColor s_flash_color;

static void refresh();

// ---------------------------------------------------------------------------
// Colour / charge model
// ---------------------------------------------------------------------------
static int charge_pct() {
  if (s_node == NODE_TRANSFORMED) {
    int rem = TRANSFORM_FRAMES - (int)s_node_frame;
    if (rem < 0) rem = 0;
    return rem * 100 / TRANSFORM_FRAMES;
  }
  return 100;
}

static GColor band_color() {
  int pct = charge_pct();
  if (pct > 60) return GColorGreen;
  if (pct > 20) return GColorOrange;
  return GColorRed;
}

static int variant_index(uint32_t period) {
  return (int)((s_anim_tick / period) % 4);
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------
static GColor flip(GColor c) { return c; }  // global colour hook (passthrough)

static void draw_badge_ring(GContext *ctx, GRect b, GColor fill, GColor ring) {
  graphics_context_set_fill_color(ctx, flip(ring));
  graphics_fill_rect(ctx, b, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, flip(fill));
  graphics_fill_rect(ctx, GRect(4, 0, b.size.w - 8, b.size.h), 0, GCornerNone);
}

static void draw_hourglass(GContext *ctx, GRect b, GColor color, int inset) {
  GPoint center = GPoint(b.size.w / 2, b.size.h / 2);
  int left = inset, right = b.size.w - inset, top = 0, bottom = b.size.h;
  int waist = (right - left) / 8;
  int ty = center.y + 3, by = center.y - 3;
  graphics_context_set_fill_color(ctx, flip(color));
  GPathInfo t = { .num_points = 4, .points = (GPoint[]) {
    GPoint(left, top), GPoint(right, top),
    GPoint(center.x + waist, ty), GPoint(center.x - waist, ty) } };
  GPath *tp = gpath_create(&t); gpath_draw_filled(ctx, tp); gpath_destroy(tp);
  GPathInfo m = { .num_points = 4, .points = (GPoint[]) {
    GPoint(left, bottom), GPoint(right, bottom),
    GPoint(center.x + waist, by), GPoint(center.x - waist, by) } };
  GPath *mp = gpath_create(&m); gpath_draw_filled(ctx, mp); gpath_destroy(mp);
}

static void draw_frame(GContext *ctx, GRect b, GColor color, int inset, int width) {
  graphics_context_set_stroke_color(ctx, flip(color));
  graphics_context_set_stroke_width(ctx, width);
  graphics_draw_rect(ctx, GRect(inset, inset, b.size.w - 2 * inset, b.size.h - 2 * inset));
}

static int pulse(int max, uint32_t period) {
  uint32_t p = s_anim_tick % period, half = period / 2;
  uint32_t up = (p < half) ? p : (period - p);
  return (int)((up * max) / half);
}

static void draw_diamond(GContext *ctx, GPoint c, int r, GColor color) {
  graphics_context_set_fill_color(ctx, flip(color));
  GPathInfo info = { .num_points = 4, .points = (GPoint[]) {
    GPoint(c.x, c.y - r), GPoint(c.x + r, c.y),
    GPoint(c.x, c.y + r), GPoint(c.x - r, c.y) } };
  GPath *p = gpath_create(&info); gpath_draw_filled(ctx, p); gpath_destroy(p);
}

static void draw_polygon(GContext *ctx, GPoint c, int r, int sides, GColor color) {
  GPoint pts[8];
  for (int i = 0; i < sides; i++) {
    int32_t a = TRIG_MAX_ANGLE * i / sides - (TRIG_MAX_ANGLE / 4);
    pts[i] = GPoint(c.x + (int)(cos_lookup(a) * r / TRIG_MAX_RATIO),
                    c.y + (int)(sin_lookup(a) * r / TRIG_MAX_RATIO));
  }
  GPathInfo info = { .num_points = (uint32_t)sides, .points = pts };
  GPath *p = gpath_create(&info);
  graphics_context_set_fill_color(ctx, flip(color));
  gpath_draw_filled(ctx, p); gpath_destroy(p);
}

static void draw_star(GContext *ctx, GPoint c, int r, GColor color) {
  GPoint pts[10];
  for (int i = 0; i < 10; i++) {
    int rad = (i % 2 == 0) ? r : r / 2;
    int32_t a = TRIG_MAX_ANGLE * i / 10 - (TRIG_MAX_ANGLE / 4);
    pts[i] = GPoint(c.x + (int)(cos_lookup(a) * rad / TRIG_MAX_RATIO),
                    c.y + (int)(sin_lookup(a) * rad / TRIG_MAX_RATIO));
  }
  GPathInfo info = { .num_points = 10, .points = pts };
  GPath *p = gpath_create(&info);
  graphics_context_set_fill_color(ctx, flip(color));
  gpath_draw_filled(ctx, p); gpath_destroy(p);
}

static void draw_alien_shape(GContext *ctx, GPoint c, int r, int index, GColor color) {
  graphics_context_set_fill_color(ctx, flip(color));
  graphics_context_set_stroke_color(ctx, flip(color));
  switch (index) {
    case 0:  graphics_fill_circle(ctx, c, r); break;
    case 1:  draw_polygon(ctx, c, r, 3, color); break;
    case 2:  graphics_fill_rect(ctx, GRect(c.x - r, c.y - r, 2 * r, 2 * r), 0, GCornerNone); break;
    case 3:  draw_star(ctx, c, r, color); break;
    case 4:  draw_diamond(ctx, c, r, color); break;
    case 5:  draw_polygon(ctx, c, r, 5, color); break;
    case 6:  draw_polygon(ctx, c, r, 6, color); break;
    case 7:
      graphics_fill_rect(ctx, GRect(c.x - r / 3, c.y - r, 2 * r / 3, 2 * r), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(c.x - r, c.y - r / 3, 2 * r, 2 * r / 3), 0, GCornerNone);
      break;
    case 8:  graphics_context_set_stroke_width(ctx, 4); graphics_draw_circle(ctx, c, r); break;
    default: graphics_context_set_stroke_width(ctx, 4);
             graphics_draw_line(ctx, GPoint(c.x - r, c.y - r), GPoint(c.x + r, c.y + r));
             graphics_draw_line(ctx, GPoint(c.x - r, c.y + r), GPoint(c.x + r, c.y - r));
             break;
  }
}

// ---------------------------------------------------------------------------
// Widget layer (Flutter-like): a BuildContext carries the canvas + bounds, and
// small composable widgets paint into it. Every node's build() composes these.
// ---------------------------------------------------------------------------
typedef struct { GContext *ctx; GRect rect; } BuildContext;

// Leaf widgets.
static void Fill(BuildContext c, GColor color) {
  graphics_context_set_fill_color(c.ctx, flip(color));
  graphics_fill_rect(c.ctx, c.rect, 0, GCornerNone);
}
static void Emblem(BuildContext c, GColor color) { draw_hourglass(c.ctx, c.rect, color, 6); }
static void EnergyFrame(BuildContext c, GColor color, int inset, int width) {
  draw_frame(c.ctx, c.rect, color, inset, width);
}
static void ScanBarV(BuildContext c, GColor color, int x, int w) {
  graphics_context_set_fill_color(c.ctx, flip(color));
  graphics_fill_rect(c.ctx, GRect(x, 0, w, c.rect.size.h), 0, GCornerNone);
}

// The reusable IDLE-STYLE TEMPLATE, shared by every screen except select:
// a full field with a left/right border ring and the hourglass emblem.
typedef struct {
  GColor fill, ring, emblem;
} Badge;

static void BadgeScreen(BuildContext c, Badge b) {
  draw_badge_ring(c.ctx, c.rect, b.fill, b.ring);
  Emblem(c, b.emblem);
}

// Composite widget: the animated glitch scan overlay (alien scan screen).
static void GlitchScan(BuildContext c) {
  GRect b = c.rect;
  int base_y = (int)((s_anim_tick * 8) % b.size.h);
  for (int x = 0; x < b.size.w; x += 10) {
    uint32_t h = (uint32_t)(x * 2654435761u + s_anim_tick * 40503u);
    if ((h & 7) == 0) continue;
    int jitter = (int)((h >> 3) % 7) - 3;
    int segw = 5 + (int)((h >> 6) % 6);
    graphics_context_set_fill_color(c.ctx, flip(((h >> 9) & 3) == 0 ? GColorCyan : GColorWhite));
    graphics_fill_rect(c.ctx, GRect(x, base_y + jitter, segw, 3), 0, GCornerNone);
  }
  for (int i = 0; i < 2; i++) {
    uint32_t g = (uint32_t)((i + 1) * 2246822519u + s_anim_tick * 3266489917u);
    graphics_context_set_fill_color(c.ctx, flip(GColorWhite));
    graphics_fill_rect(c.ctx, GRect((int)((g >> 8) % b.size.w), (int)(g % b.size.h),
                                    8 + (int)((g >> 4) % 20), 2), 0, GCornerNone);
  }
}

// ---------------------------------------------------------------------------
// Node build() methods — each composes the shared BadgeScreen template plus
// overlays (except build_select, the intentional exception).
// ---------------------------------------------------------------------------
static void build_idle(BuildContext c) {
  BadgeScreen(c, (Badge){ s_omniboost ? GColorGreen : GColorWhite, GColorBlack, GColorBlack });
  if (s_omniboost) EnergyFrame(c, GColorMintGreen, 3 + pulse(3, 16), 4);
}

static void build_dna_scan(BuildContext c) {
  BadgeScreen(c, (Badge){ band_color(), GColorBlack, GColorBlack });
  GRect b = c.rect;
  graphics_context_set_fill_color(c.ctx, GColorCyan);
  graphics_context_set_stroke_color(c.ctx, GColorCyan);
  GPoint center = GPoint(b.size.w / 2, b.size.h / 2);
  switch (variant_index(6)) {
    case 0: ScanBarV(c, GColorCyan, (s_anim_tick * 4) % b.size.w, 4); break;
    case 1: graphics_context_set_stroke_width(c.ctx, 3); graphics_draw_circle(c.ctx, center, pulse(b.size.w / 2, 16)); break;
    case 2: graphics_context_set_stroke_width(c.ctx, 2);
            graphics_draw_line(c.ctx, GPoint(0, center.y), GPoint(b.size.w, center.y));
            graphics_draw_line(c.ctx, GPoint(center.x, 0), GPoint(center.x, b.size.h)); break;
    default: graphics_fill_rect(c.ctx, GRect((int)(s_anim_tick % b.size.w), center.y - 3, 8, 6), 0, GCornerNone); break;
  }
  EnergyFrame(c, GColorCyan, 2, 2);
}

// Idle -> select transition: the same two triangles that make up the idle
// emblem spark green in place, then the left one slides fully to the right
// and the right one fully to the left -- each stops once every pixel of it
// has crossed the centerline, leaving them swapped as <| |> at the edges.
static void build_select_enter(BuildContext c) {
  GRect b = c.rect;
  GPoint ctr = GPoint(b.size.w / 2, b.size.h / 2);

  int f = (int)s_node_frame;
  if (f > SELECT_ENTER_FRAMES) f = SELECT_ENTER_FRAMES;

  Fill(c, GColorBlack);

  bool sparking = f < SELECT_ENTER_SPARK_FRAMES;
  GColor color = sparking ? ((f % 2) ? GColorGreen : GColorWhite) : GColorGreen;

  int mf = f - SELECT_ENTER_SPARK_FRAMES;
  if (mf < 0) mf = 0;
  int dx = (ctr.x * mf) / SELECT_ENTER_MOVE_FRAMES;

  graphics_context_set_fill_color(c.ctx, flip(color));

  // Left triangle: base on the left edge, tip at center -- slides right by dx.
  GPathInfo left_info = { .num_points = 3, .points = (GPoint[]) {
    GPoint(dx, 0), GPoint(dx, b.size.h), GPoint(ctr.x + dx, ctr.y) } };
  GPath *left = gpath_create(&left_info);
  gpath_draw_filled(c.ctx, left);
  gpath_destroy(left);

  // Right triangle: base on the right edge, tip at center -- slides left by dx.
  GPathInfo right_info = { .num_points = 3, .points = (GPoint[]) {
    GPoint(b.size.w - dx, 0), GPoint(b.size.w - dx, b.size.h), GPoint(ctr.x - dx, ctr.y) } };
  GPath *right = gpath_create(&right_info);
  gpath_draw_filled(c.ctx, right);
  gpath_destroy(right);
}

// The one screen that does NOT use the badge template: the green diamond.
static void build_select(BuildContext c) {
  GRect b = c.rect;
  GPoint ctr = GPoint(b.size.w / 2, b.size.h / 2);
  int dr = (b.size.w < b.size.h ? b.size.w : b.size.h) / 2 - 6;
  Fill(c, GColorBlack);
  draw_diamond(c.ctx, ctr, dr, GColorGreen);
  draw_alien_shape(c.ctx, ctr, dr / 3, s_selected_alien, GColorBlack);
}

static void build_activating(BuildContext c) {
  if (s_node_frame % 2 == 0) build_select(c);
  else BadgeScreen(c, (Badge){ GColorGreen, GColorBlack, GColorBlack });
}

static void build_transformed(BuildContext c) {
  int rem = TRANSFORM_FRAMES - (int)s_node_frame;   // steady green, flash near timeout
  GColor field = (rem > TIMEOUT_WARN_FRAMES) ? GColorGreen
               : ((s_anim_tick % 2) ? GColorGreen : GColorRed);
  BadgeScreen(c, (Badge){ field, GColorBlack, GColorBlack });
}

static void build_deactivating(BuildContext c) {
  bool red_phase = (int)s_node_frame < DEACT_RED_FRAMES;
  BadgeScreen(c, (Badge){ red_phase ? GColorRed : GColorGreen, GColorBlack, GColorBlack });
  if (red_phase) EnergyFrame(c, (s_anim_tick % 2) ? GColorWhite : GColorRed, 3, 4);
  else EnergyFrame(c, GColorMintGreen, 3, 3);
}

static void build_recalibration(BuildContext c) {
  int f = (int)s_node_frame;
  GColor field = (f < DISCHARGE_RED_FRAMES)      ? GColorRed
               : (f < DISCHARGE_RED_FRAMES + RECAL_FLASH_FRAMES)
                   ? ((s_anim_tick % 2) ? GColorGreen : GColorBlack)   // flash green
                   : GColorGreen;
  BadgeScreen(c, (Badge){ field, GColorBlack, GColorBlack });
}

static void build_master_control(BuildContext c) {
  BadgeScreen(c, (Badge){ GColorBlack, GColorGreen, GColorGreen });
  int p = pulse(3, 20);
  EnergyFrame(c, GColorMintGreen, 4 + p, 4);
  EnergyFrame(c, GColorGreen, 10 + p, 2);
}

static void build_hijacked(BuildContext c) {
  bool flicker = (s_anim_tick % 6) < 4;
  BadgeScreen(c, (Badge){ GColorBlack, GColorRed, flicker ? GColorRed : GColorDarkCandyAppleRed });
  EnergyFrame(c, GColorRed, 3, 4);
}

static void build_alien_scan(BuildContext c) {
  BadgeScreen(c, (Badge){ GColorYellow, GColorBlack, GColorBlack });
  GlitchScan(c);
}

static void build_purple(BuildContext c) {
  BadgeScreen(c, (Badge){ GColorPurple, GColorBlack, GColorBlack });
}

static void build_flash(BuildContext c) {
  bool on = s_flash_solid || ((s_node_frame % 2) == 0);
  Fill(c, on ? s_flash_color : GColorBlack);
}

// ---------------------------------------------------------------------------
// In-node actions (short press). Return NODE_STAY (no navigation).
// ---------------------------------------------------------------------------
static int act_omniboost(uint8_t button) {
  if (button == BUTTON_ID_UP) s_omniboost = !s_omniboost;
  return NODE_STAY;
}
static int act_pick_alien(uint8_t button) {
  if (button == BUTTON_ID_UP)   s_selected_alien = (s_selected_alien + NUM_ALIENS - 1) % NUM_ALIENS;
  if (button == BUTTON_ID_DOWN) s_selected_alien = (s_selected_alien + 1) % NUM_ALIENS;
  return NODE_STAY;
}

// ---------------------------------------------------------------------------
// Node table (declarative)
// ---------------------------------------------------------------------------
typedef struct {
  const char *name;
  void (*build)(BuildContext);
  uint16_t auto_frames;   // 0 = none; else advance to auto_to after N frames
  int8_t   auto_to;
  int (*on_click)(uint8_t button);
  bool locked;            // input only splashes red
  bool transient;         // ignore all input
} Node;

static const Node NODES[NODE_COUNT] = {
  [NODE_IDLE]             = { "idle",         build_idle,           0,                   NODE_STAY,          act_omniboost,  false, false },
  [NODE_DNA_SCAN]         = { "dna_scan",     build_dna_scan,       0,                   NODE_STAY,          act_omniboost,  false, false },
  [NODE_SELECT_ENTER]     = { "select_enter", build_select_enter,   SELECT_ENTER_FRAMES, NODE_TRANSFORM_SELECT, NULL,        false, true  },
  [NODE_TRANSFORM_SELECT] = { "select",       build_select,         0,                   NODE_STAY,          act_pick_alien, false, false },
  [NODE_ACTIVATING]       = { "activating",   build_activating,     ACT_TOTAL_FRAMES,    NODE_TRANSFORMED,   NULL,           false, true  },
  [NODE_TRANSFORMED]      = { "transformed",  build_transformed,    TRANSFORM_FRAMES,    NODE_RECALIBRATION, act_omniboost,  false, false },
  [NODE_DEACTIVATING]     = { "deactivating", build_deactivating,   DEACT_TOTAL_FRAMES,  NODE_IDLE,          NULL,           false, true  },
  [NODE_RECALIBRATION]    = { "recal",        build_recalibration,  RECAL_TOTAL_FRAMES,  NODE_IDLE,          NULL,           true,  false },
  [NODE_MASTER_CONTROL]   = { "master",       build_master_control, 0,                   NODE_STAY,          act_omniboost,  false, false },
  [NODE_HIJACKED]         = { "hijacked",     build_hijacked,       0,                   NODE_STAY,          act_omniboost,  false, false },
  [NODE_ALIEN_SCAN]       = { "alien_scan",   build_alien_scan,     0,                   NODE_STAY,          act_omniboost,  false, false },
  [NODE_PURPLE]           = { "purple",       build_purple,         0,                   NODE_STAY,          act_omniboost,  false, false },
  [NODE_FLASH]            = { "flash",        build_flash,          0,                   NODE_STAY,          NULL,           false, true  },
};

// Declarative navigation edges (Yarn-style): from + short button -> to,
// optionally played through a colour flash.
typedef struct {
  int8_t from, to;
  uint8_t button;
  bool flash, flash_solid;
  uint8_t flash_len, flash_argb;
} Nav;

static const Nav NAV[] = {
  { NODE_IDLE,             NODE_SELECT_ENTER,     BUTTON_ID_SELECT, false, false, 0,                  0 },
  { NODE_DNA_SCAN,         NODE_TRANSFORM_SELECT, BUTTON_ID_SELECT, false, false, 0,                  0 },
  { NODE_TRANSFORM_SELECT, NODE_ACTIVATING,       BUTTON_ID_SELECT, true,  true,  GREEN_FLASH_FRAMES, GColorGreenARGB8 },
  { NODE_TRANSFORM_SELECT, NODE_IDLE,             BUTTON_ID_BACK,   false, false, 0,                  0 },
  { NODE_TRANSFORMED,      NODE_DEACTIVATING,     BUTTON_ID_SELECT, false, false, 0,                  0 },
  { NODE_MASTER_CONTROL,   NODE_IDLE,             BUTTON_ID_SELECT, false, false, 0,                  0 },
  { NODE_HIJACKED,         NODE_IDLE,             BUTTON_ID_SELECT, false, false, 0,                  0 },
  { NODE_ALIEN_SCAN,       NODE_IDLE,             BUTTON_ID_SELECT, false, false, 0,                  0 },
  { NODE_PURPLE,           NODE_IDLE,             BUTTON_ID_SELECT, false, false, 0,                  0 },
  { NODE_IDLE,             NODE_ALIEN_SCAN,       BUTTON_ID_DOWN,   true,  false, FLASH_FRAMES,       GColorYellowARGB8 },
  { NODE_ALIEN_SCAN,       NODE_PURPLE,           BUTTON_ID_DOWN,   true,  false, FLASH_FRAMES,       GColorPurpleARGB8 },
  { NODE_PURPLE,           NODE_IDLE,             BUTTON_ID_DOWN,   false, false, 0,                  0 },
};
#define NAV_COUNT (sizeof(NAV) / sizeof(NAV[0]))

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------
static void router_goto(int id) {
  s_node = id;
  s_node_frame = 0;
  vibes_short_pulse();               // buzz on every transition/flash
  refresh();
}

static void start_flash(uint8_t argb, int target, bool solid, int len) {
  s_flash_color = (GColor){ .argb = argb };
  s_flash_target = target;
  s_flash_solid = solid;
  s_flash_len = len;
  router_goto(NODE_FLASH);
}

static void router_click(uint8_t button, bool long_press) {
  const Node *n = &NODES[s_node];

  if (n->locked) {                   // discharged: input only splashes red
    s_splash_frame = SPLASH_FRAMES;
    vibes_short_pulse();
    refresh();
    return;
  }
  if (n->transient) return;

  if (long_press) {                  // global long-press toggles
    int to = NODE_STAY;
    if (button == BUTTON_ID_UP)          to = (s_node == NODE_HIJACKED)       ? NODE_IDLE : NODE_HIJACKED;
    else if (button == BUTTON_ID_DOWN)   to = (s_node == NODE_MASTER_CONTROL) ? NODE_IDLE : NODE_MASTER_CONTROL;
    else if (button == BUTTON_ID_SELECT) to = (s_node == NODE_DNA_SCAN)       ? NODE_IDLE : NODE_DNA_SCAN;
    if (to != NODE_STAY) router_goto(to);
    return;
  }

  for (unsigned i = 0; i < NAV_COUNT; i++) {   // declarative navigation
    if (NAV[i].from == s_node && NAV[i].button == button) {
      if (NAV[i].flash) start_flash(NAV[i].flash_argb, NAV[i].to, NAV[i].flash_solid, NAV[i].flash_len);
      else router_goto(NAV[i].to);
      return;
    }
  }
  if (button == BUTTON_ID_BACK) {    // no BACK edge -> default: exit the app
    window_stack_pop(true);
    return;
  }
  if (n->on_click) {                 // in-node action
    int t = n->on_click(button);
    if (t != NODE_STAY) router_goto(t); else refresh();
  }
}

static void router_tick(void) {
  if (s_splash_frame > 0) s_splash_frame--;
  s_node_frame++;
  const Node *n = &NODES[s_node];
  int limit  = (s_node == NODE_FLASH) ? s_flash_len    : n->auto_frames;
  int target = (s_node == NODE_FLASH) ? s_flash_target : n->auto_to;
  if (limit > 0 && (int)s_node_frame >= limit) router_goto(target);
}

// ---------------------------------------------------------------------------
// Rendering entry point
// ---------------------------------------------------------------------------
static void omnitrix_update_proc(Layer *layer, GContext *ctx) {
  BuildContext c = { ctx, layer_get_bounds(layer) };
  Fill(c, GColorBlack);
  if (s_splash_frame > 0) {          // locked red splash overrides everything
    Fill(c, GColorRed);
    return;
  }
  if (NODES[s_node].build) NODES[s_node].build(c);
}

// ---------------------------------------------------------------------------
// Timers / input
// ---------------------------------------------------------------------------
static void refresh() {
  if (s_omnitrix_layer) layer_mark_dirty(s_omnitrix_layer);
}

static void anim_timer_callback(void *data) {
  s_anim_tick++;
  router_tick();
  if (s_omnitrix_layer) layer_mark_dirty(s_omnitrix_layer);
  s_anim_timer = app_timer_register(ANIM_INTERVAL_MS, anim_timer_callback, NULL);
}

static void up_click(ClickRecognizerRef r, void *c)     { router_click(BUTTON_ID_UP, false); }
static void up_long(ClickRecognizerRef r, void *c)      { router_click(BUTTON_ID_UP, true); }
static void select_click(ClickRecognizerRef r, void *c) { router_click(BUTTON_ID_SELECT, false); }
static void select_long(ClickRecognizerRef r, void *c)  { router_click(BUTTON_ID_SELECT, true); }
static void down_click(ClickRecognizerRef r, void *c)   { router_click(BUTTON_ID_DOWN, false); }
static void down_long(ClickRecognizerRef r, void *c)    { router_click(BUTTON_ID_DOWN, true); }
static void back_click(ClickRecognizerRef r, void *c)   { router_click(BUTTON_ID_BACK, false); }

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click);
  window_single_click_subscribe(BUTTON_ID_BACK, back_click);
  window_long_click_subscribe(BUTTON_ID_UP, 0, up_long, NULL);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, select_long, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 0, down_long, NULL);
}

// ---------------------------------------------------------------------------
// Window / app lifecycle
// ---------------------------------------------------------------------------
static void main_window_load(Window *window) {
  Layer *wl = window_get_root_layer(window);
  s_omnitrix_layer = layer_create(layer_get_bounds(wl));
  layer_set_update_proc(s_omnitrix_layer, omnitrix_update_proc);
  layer_add_child(wl, s_omnitrix_layer);
}

static void main_window_unload(Window *window) {
  layer_destroy(s_omnitrix_layer);
}

static void init() {
  s_main_window = window_create();
  window_set_background_color(s_main_window, GColorBlack);
  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load, .unload = main_window_unload });
  window_set_click_config_provider(s_main_window, click_config_provider);
  window_stack_push(s_main_window, true);
  s_anim_timer = app_timer_register(ANIM_INTERVAL_MS, anim_timer_callback, NULL);
}

static void deinit() {
  if (s_anim_timer) app_timer_cancel(s_anim_timer);
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
