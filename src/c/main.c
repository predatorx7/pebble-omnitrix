#include <pebble.h>

// ---------------------------------------------------------------------------
// Omnitrix Game — full-screen badge state machine (game, not a watchface)
//
// The Omnitrix face fills the whole screen. A black "hourglass" emblem sits on
// a colored energy disc. Everything is driven by GAME LOGIC and the buttons —
// there is no dependency on real battery / charging / wall clock.
//
//   * When inactive the badge is WHITE. Pressing SELECT opens alien-select
//     mode (browse with UP/DOWN); pressing SELECT again transforms and starts
//     a silent 1-minute timer. It holds steady green while charged, then flashes
//     between green and red as it nears timeout. On discharge it holds red for
//     30 s, then flashes green, turns green, and after 2 s returns to the white
//     idle badge.
//     Pressing SELECT while transformed powers down through a brief red-spark
//     -> green surge -> white idle sequence instead of cutting straight back.
//
//   * Buttons:
//       - SELECT(short) : advance flow  Idle -> Alien Select -> Transformed -> Idle
//       - SELECT(long)  : enter / exit DNA Scan mode
//       - UP    (short) : previous alien (in select); else toggle Omniboost
//       - DOWN  (short) : next alien (in select)
//       - UP    (long)  : toggle Hijacked badge state
//       - DOWN  (long)  : toggle Master Control badge state
// ---------------------------------------------------------------------------

// Manual states the user drives with the buttons.
typedef enum {
  MSTATE_IDLE = 0,          // Normal / Omniboost badge (colour from charge)
  MSTATE_DNA_SCAN,          // Scanning for a DNA sample
  MSTATE_TRANSFORM_SELECT,  // Selecting an alien
  MSTATE_ACTIVATING,        // Lock-in flash: diamond -> green transformed
  MSTATE_TRANSFORMED,       // Transformation locked in, countdown running
  MSTATE_DEACTIVATING,      // Manual power-down: red spark -> green -> idle
  MSTATE_RECALIBRATION,     // Discharge recovery: flash -> red -> flash -> green -> idle
  MSTATE_MASTER_CONTROL,    // Master Control unlocked
  MSTATE_HIJACKED,          // Badge hijacked
  MSTATE_ALIEN_SCAN,        // Yellow field with a fast glitchy scan line
  MSTATE_PURPLE,            // Purple-only field
  MSTATE_FLASH,             // Full-screen colour flash transitioning to a target
} GameState;

#define NUM_ALIENS 10
#define ANIM_INTERVAL_MS 120
#define TRANSFORM_SECS 60      // transformation lasts 1 minute
#define ACT_TOTAL_FRAMES 8     // lock-in flash length (~1s)
// Discharge recovery timeline (seconds since discharge):
//   hold red -> flash green -> hold green (2s) -> idle.
#define DISCHARGE_RED_SECS 30
#define GREEN_FLASH_SECS 1
#define RECHARGE_GREEN_SECS 2
#define RECAL_FLASH_END (DISCHARGE_RED_SECS + GREEN_FLASH_SECS)
#define RECAL_TOTAL_SECS (RECAL_FLASH_END + RECHARGE_GREEN_SECS)
#define DEACT_RED_FRAMES 5     // red-spark phase of manual power-down (~0.6s)
#define DEACT_TOTAL_FRAMES 10  // red then green, then back to white idle
#define FLASH_FRAMES 6         // full-screen intro flash length (~0.7s)
#define SPLASH_FRAMES 3        // brief red "locked" splash (~0.4s)

static Window *s_main_window;
static Layer *s_omnitrix_layer;
static TextLayer *s_time_layer;
static TextLayer *s_date_layer;
static AppTimer *s_anim_timer;

static GameState s_state = MSTATE_IDLE;
static bool s_omniboost = false;
static int s_selected_alien = 0;
static uint32_t s_anim_tick = 0;
static int s_charge_pct = 100;         // game energy when idle / ready
static time_t s_transform_deadline = 0;
static time_t s_discharge_time = 0;    // when the transformation discharged
static int s_deact_frame = 0;          // manual power-down animation frame
static int s_act_frame = 0;            // lock-in (activation) animation frame
static GameState s_prev_state = MSTATE_IDLE; // for detecting transitions (vibe)
static int s_splash_frame = 0;         // brief red "locked" splash counter
static int s_flash_frame = 0;          // full-screen intro flash frame
static int s_flash_len = 0;            // flash duration in frames
static bool s_flash_solid = false;     // true = one solid flash, false = strobe
static GColor s_flash_color;           // colour of the intro flash
static GameState s_flash_target;       // state to enter when the flash ends

// Begin a full-screen colour flash that settles into `target`. When `solid` is
// true it holds one steady flash for `frames`; otherwise it strobes.
static void start_flash(GColor color, GameState target, bool solid, int frames) {
  s_flash_color = color;
  s_flash_target = target;
  s_flash_solid = solid;
  s_flash_len = frames;
  s_flash_frame = 0;
  s_state = MSTATE_FLASH;
}

// ---------------------------------------------------------------------------
// Charge / colour model
// ---------------------------------------------------------------------------

// Effective charge: during a transformation this is the countdown remaining as
// a percentage; otherwise it's the stored game energy.
static int charge_pct() {
  if (s_state == MSTATE_TRANSFORMED) {
    int remaining = (int)(s_transform_deadline - time(NULL));
    if (remaining < 0) remaining = 0;
    return (remaining * 100) / TRANSFORM_SECS;
  }
  return s_charge_pct;
}

// Battery-style colour band, now driven by the game charge.
//   >60% green, >20% orange, else red  (reference 100-60 / 60-20 / 20-0 rows).
static GColor band_color() {
  int pct = charge_pct();
  if (pct > 60) {
    return GColorGreen;
  } else if (pct > 20) {
    return GColorOrange;
  }
  return GColorRed;
}

// Resolve auto-transitions (countdown expiry, recovery end) then return the
// state we actually draw.
static GameState resolve_state() {
  time_t now = time(NULL);

  if (s_state == MSTATE_TRANSFORMED) {
    if (now >= s_transform_deadline) {
      // Discharged: begin the red -> green flash -> green -> idle recovery.
      s_state = MSTATE_RECALIBRATION;
      s_discharge_time = now;
    }
  } else if (s_state == MSTATE_RECALIBRATION) {
    if (now - s_discharge_time >= RECAL_TOTAL_SECS) {
      s_state = MSTATE_IDLE;
      s_charge_pct = 100;
    }
  }
  return s_state;
}

// The 4 sub-variants shown in the reference cycle over time.
static int variant_index(uint32_t period) {
  return (int)((s_anim_tick / period) % 4);
}

// ---------------------------------------------------------------------------
// Emblem drawing helpers
// ---------------------------------------------------------------------------

// Colour passthrough (no-op). The colour-flip experiment was reverted; kept as
// a single hook in case a global colour transform is wanted later.
static GColor flip(GColor c) {
  return c;
}

// Full-screen energy field. A thin border ring is drawn on the left/right
// edges only (the layout is rotated 90 degrees), so the field runs edge-to-
// edge vertically.
static void draw_badge_ring(GContext *ctx, GRect bounds, GColor fill, GColor ring) {
  graphics_context_set_fill_color(ctx, flip(ring));
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);
  graphics_context_set_fill_color(ctx, flip(fill));
  graphics_fill_rect(ctx, GRect(4, 0, bounds.size.w - 8, bounds.size.h), 0, GCornerNone);
}

// Draw the Omnitrix "hourglass" rotated 90 degrees: two triangles whose apexes
// meet at the centre, bases pinned to the top/bottom screen edges.
static void draw_hourglass(GContext *ctx, GRect bounds, GColor color, int inset) {
  GPoint center = GPoint(bounds.size.w / 2, bounds.size.h / 2);
  int left   = inset;
  int right  = bounds.size.w - inset;
  int top    = 0;                       // bases run to the top/bottom edges
  int bottom = bounds.size.h;
  // The waist gives the join a real width so the two halves meet across a
  // solid band instead of pinching to a single point (which looks like a gap).
  int waist = (right - left) / 8;
  // Overlap the two halves across the centre so there is no seam line.
  int ty = center.y + 3;
  int by = center.y - 3;

  graphics_context_set_fill_color(ctx, flip(color));

  GPathInfo top_info = {
    .num_points = 4,
    .points = (GPoint[]) {
      GPoint(left, top), GPoint(right, top),
      GPoint(center.x + waist, ty), GPoint(center.x - waist, ty)
    }
  };
  GPath *tt = gpath_create(&top_info);
  gpath_draw_filled(ctx, tt);
  gpath_destroy(tt);

  GPathInfo bottom_info = {
    .num_points = 4,
    .points = (GPoint[]) {
      GPoint(left, bottom), GPoint(right, bottom),
      GPoint(center.x + waist, by), GPoint(center.x - waist, by)
    }
  };
  GPath *bt = gpath_create(&bottom_info);
  gpath_draw_filled(ctx, bt);
  gpath_destroy(bt);
}

// Draw an inset energy frame (used for glow / boost / warning borders).
static void draw_frame(GContext *ctx, GRect bounds, GColor color, int inset, int width) {
  graphics_context_set_stroke_color(ctx, flip(color));
  graphics_context_set_stroke_width(ctx, width);
  GRect r = GRect(inset, inset,
                  bounds.size.w - 2 * inset,
                  bounds.size.h - 2 * inset);
  graphics_draw_rect(ctx, r);
}

// Simple triangle-wave pulse in [0,1] scaled to [0,max].
static int pulse(int max, uint32_t period) {
  uint32_t p = s_anim_tick % period;
  uint32_t half = period / 2;
  uint32_t up = (p < half) ? p : (period - p);
  return (int)((up * max) / half);
}

// Filled diamond (rotated square) centred at c with half-extent r.
static void draw_diamond(GContext *ctx, GPoint c, int r, GColor color) {
  graphics_context_set_fill_color(ctx, flip(color));
  GPathInfo info = {
    .num_points = 4,
    .points = (GPoint[]) {
      GPoint(c.x, c.y - r), GPoint(c.x + r, c.y),
      GPoint(c.x, c.y + r), GPoint(c.x - r, c.y)
    }
  };
  GPath *p = gpath_create(&info);
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

// Draw a filled regular polygon (n sides) centred at c, radius r, top vertex up.
static void draw_polygon(GContext *ctx, GPoint c, int r, int sides, GColor color) {
  GPoint pts[8];
  for (int i = 0; i < sides; i++) {
    int32_t angle = TRIG_MAX_ANGLE * i / sides - (TRIG_MAX_ANGLE / 4);
    pts[i] = GPoint(c.x + (int)(cos_lookup(angle) * r / TRIG_MAX_RATIO),
                    c.y + (int)(sin_lookup(angle) * r / TRIG_MAX_RATIO));
  }
  GPathInfo info = { .num_points = (uint32_t)sides, .points = pts };
  GPath *p = gpath_create(&info);
  graphics_context_set_fill_color(ctx, flip(color));
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

// Draw a filled 5-point star centred at c, outer radius r.
static void draw_star(GContext *ctx, GPoint c, int r, GColor color) {
  GPoint pts[10];
  for (int i = 0; i < 10; i++) {
    int rad = (i % 2 == 0) ? r : r / 2;
    int32_t angle = TRIG_MAX_ANGLE * i / 10 - (TRIG_MAX_ANGLE / 4);
    pts[i] = GPoint(c.x + (int)(cos_lookup(angle) * rad / TRIG_MAX_RATIO),
                    c.y + (int)(sin_lookup(angle) * rad / TRIG_MAX_RATIO));
  }
  GPathInfo info = { .num_points = 10, .points = pts };
  GPath *p = gpath_create(&info);
  graphics_context_set_fill_color(ctx, flip(color));
  gpath_draw_filled(ctx, p);
  gpath_destroy(p);
}

// Draw the glyph for alien `index` (0..NUM_ALIENS-1) centred at c, size r.
static void draw_alien_shape(GContext *ctx, GPoint c, int r, int index, GColor color) {
  graphics_context_set_fill_color(ctx, flip(color));
  graphics_context_set_stroke_color(ctx, flip(color));
  switch (index) {
    case 0:  graphics_fill_circle(ctx, c, r); break;                 // circle
    case 1:  draw_polygon(ctx, c, r, 3, color); break;               // triangle
    case 2:  graphics_fill_rect(ctx, GRect(c.x - r, c.y - r, 2 * r, 2 * r),
                                0, GCornerNone); break;              // square
    case 3:  draw_star(ctx, c, r, color); break;                     // star
    case 4:  draw_diamond(ctx, c, r, color); break;                  // diamond
    case 5:  draw_polygon(ctx, c, r, 5, color); break;               // pentagon
    case 6:  draw_polygon(ctx, c, r, 6, color); break;               // hexagon
    case 7:  // plus / cross
      graphics_fill_rect(ctx, GRect(c.x - r / 3, c.y - r, 2 * r / 3, 2 * r), 0, GCornerNone);
      graphics_fill_rect(ctx, GRect(c.x - r, c.y - r / 3, 2 * r, 2 * r / 3), 0, GCornerNone);
      break;
    case 8:  // ring (hollow circle)
      graphics_context_set_stroke_width(ctx, 4);
      graphics_draw_circle(ctx, c, r);
      break;
    default: // X
      graphics_context_set_stroke_width(ctx, 4);
      graphics_draw_line(ctx, GPoint(c.x - r, c.y - r), GPoint(c.x + r, c.y + r));
      graphics_draw_line(ctx, GPoint(c.x - r, c.y + r), GPoint(c.x + r, c.y - r));
      break;
  }
}

// ---------------------------------------------------------------------------
// Per-state rendering
// ---------------------------------------------------------------------------
static void omnitrix_update_proc(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);
  GameState state = resolve_state();
  GColor band = band_color();

  // Base fill (full screen).
  graphics_context_set_fill_color(ctx, flip(GColorBlack));
  graphics_fill_rect(ctx, bounds, 0, GCornerNone);

  // A button press during the red timed-out state just splashes red briefly.
  if (s_splash_frame > 0) {
    graphics_context_set_fill_color(ctx, GColorRed);
    graphics_fill_rect(ctx, bounds, 0, GCornerNone);
    return;
  }

  switch (state) {
    case MSTATE_IDLE: {
      // Inactive: white energy field with black hourglass emblem.
      // Omniboost tints it bright green with a pulsing glow.
      GColor idle_fill = s_omniboost ? GColorGreen : GColorWhite;
      draw_badge_ring(ctx, bounds, idle_fill, GColorBlack);
      draw_hourglass(ctx, bounds, GColorBlack, 6);
      if (s_omniboost) {
        int p = pulse(3, 16);
        draw_frame(ctx, bounds, GColorMintGreen, 3 + p, 4);
      }
      break;
    }

    case MSTATE_DNA_SCAN: {
      // Disc + emblem with one of 4 cycling cyan scan effects.
      draw_badge_ring(ctx, bounds, band, GColorBlack);
      draw_hourglass(ctx, bounds, GColorBlack, 6);
      graphics_context_set_fill_color(ctx, GColorCyan);
      graphics_context_set_stroke_color(ctx, GColorCyan);
      GPoint center = GPoint(bounds.size.w / 2, bounds.size.h / 2);

      switch (variant_index(6)) {
        case 0: {   // scan bar sweep (rotated: vertical bar, left -> right)
          int scan_x = (s_anim_tick * 4) % bounds.size.w;
          graphics_fill_rect(ctx, GRect(scan_x, 0, 4, bounds.size.h), 0, GCornerNone);
          break;
        }
        case 1: {   // expanding cyan ring
          int r = pulse(bounds.size.w / 2, 16);
          graphics_context_set_stroke_width(ctx, 3);
          graphics_draw_circle(ctx, center, r);
          break;
        }
        case 2: {   // crosshair / reticle
          graphics_context_set_stroke_width(ctx, 2);
          graphics_draw_line(ctx, GPoint(0, center.y), GPoint(bounds.size.w, center.y));
          graphics_draw_line(ctx, GPoint(center.x, 0), GPoint(center.x, bounds.size.h));
          break;
        }
        default: {  // spinner tick
          int a = (int)(s_anim_tick % bounds.size.w);
          graphics_fill_rect(ctx, GRect(a, center.y - 3, 8, 6), 0, GCornerNone);
          break;
        }
      }
      draw_frame(ctx, bounds, GColorCyan, 2, 2);
      break;
    }

    case MSTATE_TRANSFORM_SELECT: {
      // Green diamond badge; the selected alien is shown as a distinct black
      // glyph in the middle (circle, triangle, square, star, ... up to 10).
      GPoint c = GPoint(bounds.size.w / 2, bounds.size.h / 2);
      int dr = (bounds.size.w < bounds.size.h ? bounds.size.w : bounds.size.h) / 2 - 6;
      graphics_context_set_fill_color(ctx, flip(GColorBlack));
      graphics_fill_rect(ctx, bounds, 0, GCornerNone);
      draw_diamond(ctx, c, dr, GColorGreen);
      draw_alien_shape(ctx, c, dr / 3, s_selected_alien, GColorBlack);
      break;
    }

    case MSTATE_ACTIVATING: {
      // Lock-in flash: alternate the green diamond glyph with the green
      // transformed badge, settling into the transformation.
      if (s_act_frame % 2 == 0) {
        GPoint c = GPoint(bounds.size.w / 2, bounds.size.h / 2);
        int dr = (bounds.size.w < bounds.size.h ? bounds.size.w : bounds.size.h) / 2 - 6;
        graphics_context_set_fill_color(ctx, flip(GColorBlack));
        graphics_fill_rect(ctx, bounds, 0, GCornerNone);
        draw_diamond(ctx, c, dr, GColorGreen);
        draw_alien_shape(ctx, c, dr / 3, s_selected_alien, GColorBlack);
      } else {
        draw_badge_ring(ctx, bounds, GColorGreen, GColorBlack);
        draw_hourglass(ctx, bounds, GColorBlack, 6);
      }
      break;
    }

    case MSTATE_TRANSFORMED: {
      // Steady green while charged; once it drops out of the green band (close
      // to timeout) it flashes between green and red until discharge. At
      // discharge, RECALIBRATION flashes then settles to solid red.
      GColor field = (charge_pct() > 60)
          ? GColorGreen
          : ((s_anim_tick % 2) ? GColorGreen : GColorRed);
      draw_badge_ring(ctx, bounds, field, GColorBlack);
      draw_hourglass(ctx, bounds, GColorBlack, 6);
      break;
    }

    case MSTATE_DEACTIVATING: {
      // Manual power-down: a red spark burst, then a green surge, then idle.
      bool red_phase = s_deact_frame < DEACT_RED_FRAMES;
      GColor field = red_phase ? GColorRed : GColorGreen;
      draw_badge_ring(ctx, bounds, field, GColorBlack);
      draw_hourglass(ctx, bounds, GColorBlack, 6);
      if (red_phase) {
        // Electric red spark flicker.
        bool spark = (s_anim_tick % 2) == 0;
        draw_frame(ctx, bounds, spark ? GColorWhite : GColorRed, 3, 4);
      } else {
        draw_frame(ctx, bounds, GColorMintGreen, 3, 3);
      }
      break;
    }

    case MSTATE_RECALIBRATION: {
      // Discharge recovery timeline:
      //   hold red (30s) -> flash green (1s) -> hold green (2s) ->
      //   resolve_state() returns the badge to white idle.
      int elapsed = (int)(time(NULL) - s_discharge_time);
      GColor field;
      if (elapsed < DISCHARGE_RED_SECS) {
        field = GColorRed;                                     // discharged: red
      } else if (elapsed < RECAL_FLASH_END) {
        field = (s_anim_tick % 2) ? GColorGreen : GColorBlack; // flash green
      } else {
        field = GColorGreen;                                   // recharged: green (2s)
      }
      draw_badge_ring(ctx, bounds, field, GColorBlack);
      draw_hourglass(ctx, bounds, GColorBlack, 6);
      break;
    }

    case MSTATE_HIJACKED: {
      // Black field, flickering red emblem + red warning frame.
      bool flicker = (s_anim_tick % 6) < 4;
      draw_badge_ring(ctx, bounds, GColorBlack, GColorRed);
      draw_hourglass(ctx, bounds, flicker ? GColorRed : GColorDarkCandyAppleRed, 6);
      draw_frame(ctx, bounds, GColorRed, 3, 4);
      break;
    }

    case MSTATE_MASTER_CONTROL: {
      // Black field, green emblem, steady bright green master frames.
      draw_badge_ring(ctx, bounds, GColorBlack, GColorGreen);
      draw_hourglass(ctx, bounds, GColorGreen, 6);
      int p = pulse(3, 20);
      draw_frame(ctx, bounds, GColorMintGreen, 4 + p, 4);
      draw_frame(ctx, bounds, GColorGreen, 10 + p, 2);
      break;
    }

    case MSTATE_ALIEN_SCAN: {
      // Yellow alien-scanning field + black hourglass with a fast, glitchy
      // scan line sweeping top -> bottom.
      draw_badge_ring(ctx, bounds, GColorYellow, GColorBlack);
      draw_hourglass(ctx, bounds, GColorBlack, 6);

      int base_y = (int)((s_anim_tick * 8) % bounds.size.h);
      // Broken/jittered main line: short white segments at varying y offsets.
      for (int x = 0; x < bounds.size.w; x += 10) {
        uint32_t h = (uint32_t)(x * 2654435761u + s_anim_tick * 40503u);
        if ((h & 7) == 0) continue;                 // occasional gap
        int jitter = (int)((h >> 3) % 7) - 3;        // -3..3 px
        int segw = 5 + (int)((h >> 6) % 6);          // 5..10 px
        GColor c = ((h >> 9) & 3) == 0 ? GColorCyan : GColorWhite;
        graphics_context_set_fill_color(ctx, flip(c));
        graphics_fill_rect(ctx, GRect(x, base_y + jitter, segw, 3), 0, GCornerNone);
      }
      // Stray glitch tear lines elsewhere on the field.
      for (int i = 0; i < 2; i++) {
        uint32_t g = (uint32_t)((i + 1) * 2246822519u + s_anim_tick * 3266489917u);
        int gy = (int)(g % bounds.size.h);
        int gx = (int)((g >> 8) % bounds.size.w);
        graphics_context_set_fill_color(ctx, flip(GColorWhite));
        graphics_fill_rect(ctx, GRect(gx, gy, 8 + (int)((g >> 4) % 20), 2), 0, GCornerNone);
      }
      break;
    }

    case MSTATE_PURPLE: {
      // Purple-only field with black hourglass (no scan line).
      draw_badge_ring(ctx, bounds, GColorPurple, GColorBlack);
      draw_hourglass(ctx, bounds, GColorBlack, 6);
      break;
    }

    case MSTATE_FLASH: {
      // Full-screen colour flash before the target: one solid fill, or a
      // strobe alternating with black.
      bool on = s_flash_solid || ((s_flash_frame % 2) == 0);
      graphics_context_set_fill_color(ctx, flip(on ? s_flash_color : GColorBlack));
      graphics_fill_rect(ctx, bounds, 0, GCornerNone);
      break;
    }
  }
}

// ---------------------------------------------------------------------------
// Text overlay — game status (not a clock)
// ---------------------------------------------------------------------------
static void update_status() {
  // Every state is now conveyed purely through the badge graphics — no text.
  layer_set_hidden(text_layer_get_layer(s_time_layer), true);
  layer_set_hidden(text_layer_get_layer(s_date_layer), true);
}

static void tick_handler(struct tm *tick_time, TimeUnits units_changed) {
  resolve_state();
  update_status();
  if (s_omnitrix_layer) {
    layer_mark_dirty(s_omnitrix_layer);
  }
}

// ---------------------------------------------------------------------------
// Animation driver
// ---------------------------------------------------------------------------
// Buzz once whenever the rendered state changes (covers every transition and
// flash, including the timed auto-transitions).
static void buzz_on_transition() {
  if (s_state != s_prev_state) {
    vibes_short_pulse();
    s_prev_state = s_state;
  }
}

static void anim_timer_callback(void *data) {
  s_anim_tick++;
  if (s_splash_frame > 0) {
    s_splash_frame--;
  }
  if (s_state == MSTATE_FLASH) {
    s_flash_frame++;
    if (s_flash_frame >= s_flash_len) {
      s_state = s_flash_target;
    }
  } else if (s_state == MSTATE_ACTIVATING) {
    s_act_frame++;
    if (s_act_frame >= ACT_TOTAL_FRAMES) {
      s_state = MSTATE_TRANSFORMED;
      s_transform_deadline = time(NULL) + TRANSFORM_SECS;
    }
  } else if (s_state == MSTATE_DEACTIVATING) {
    s_deact_frame++;
    if (s_deact_frame >= DEACT_TOTAL_FRAMES) {
      s_state = MSTATE_IDLE;
      s_charge_pct = 100;
      update_status();
    }
  }
  buzz_on_transition();
  if (s_omnitrix_layer) {
    layer_mark_dirty(s_omnitrix_layer);
  }
  s_anim_timer = app_timer_register(ANIM_INTERVAL_MS, anim_timer_callback, NULL);
}

// ---------------------------------------------------------------------------
// Button handlers
// ---------------------------------------------------------------------------
static void refresh() {
  update_status();
  if (s_omnitrix_layer) {
    layer_mark_dirty(s_omnitrix_layer);
  }
}

// While in the red timed-out state the badge is locked: any button just
// splashes red briefly and stays put until it recharges. Returns true if the
// press was consumed.
static bool locked_in_recal() {
  if (s_state == MSTATE_RECALIBRATION) {
    s_splash_frame = SPLASH_FRAMES;
    vibes_short_pulse();
    refresh();
    return true;
  }
  return false;
}

// UP short: previous alien while selecting; otherwise toggle Omniboost.
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (locked_in_recal()) return;
  if (s_state == MSTATE_TRANSFORM_SELECT) {
    s_selected_alien = (s_selected_alien + NUM_ALIENS - 1) % NUM_ALIENS;
  } else {
    s_omniboost = !s_omniboost;
  }
  refresh();
}

// UP long: toggle Hijacked badge state.
static void up_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (locked_in_recal()) return;
  s_state = (s_state == MSTATE_HIJACKED) ? MSTATE_IDLE : MSTATE_HIJACKED;
  refresh();
}

// SELECT short: advance the activation flow.
//   Idle / Scan -> Alien Select -> Transformed (10 min) -> Idle.
static void select_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (locked_in_recal()) return;
  switch (s_state) {
    case MSTATE_IDLE:             start_flash(GColorGreen, MSTATE_TRANSFORM_SELECT, true, 5); break;
    case MSTATE_DNA_SCAN:         s_state = MSTATE_TRANSFORM_SELECT; break;
    case MSTATE_TRANSFORM_SELECT:
      // Play the lock-in flash first; the timer starts when it settles.
      s_state = MSTATE_ACTIVATING;
      s_act_frame = 0;
      break;
    case MSTATE_TRANSFORMED:
      // Power down through the red-spark -> green -> white sequence.
      s_state = MSTATE_DEACTIVATING;
      s_deact_frame = 0;
      break;
    case MSTATE_DEACTIVATING:     break;  // already powering down
    // Recalibration / Master Control / Hijacked exit to idle.
    default:                      s_state = MSTATE_IDLE; break;
  }
  refresh();
}

// SELECT long: enter / exit DNA Scan mode.
static void select_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (locked_in_recal()) return;
  s_state = (s_state == MSTATE_DNA_SCAN) ? MSTATE_IDLE : MSTATE_DNA_SCAN;
  refresh();
}

// DOWN short: next alien while selecting; otherwise cycle the scan modes
// idle -> yellow alien scan -> purple only -> idle.
static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (locked_in_recal()) return;
  if (s_state == MSTATE_TRANSFORM_SELECT) {
    s_selected_alien = (s_selected_alien + 1) % NUM_ALIENS;
    refresh();
  } else if (s_state == MSTATE_IDLE) {
    start_flash(GColorYellow, MSTATE_ALIEN_SCAN, false, FLASH_FRAMES);
    refresh();
  } else if (s_state == MSTATE_ALIEN_SCAN) {
    start_flash(GColorPurple, MSTATE_PURPLE, false, FLASH_FRAMES);
    refresh();
  } else if (s_state == MSTATE_PURPLE) {
    s_state = MSTATE_IDLE;
    refresh();
  }
}

// DOWN long: toggle Master Control badge state.
static void down_long_click_handler(ClickRecognizerRef recognizer, void *context) {
  if (locked_in_recal()) return;
  s_state = (s_state == MSTATE_MASTER_CONTROL) ? MSTATE_IDLE : MSTATE_MASTER_CONTROL;
  refresh();
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_SELECT, select_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
  window_long_click_subscribe(BUTTON_ID_UP, 0, up_long_click_handler, NULL);
  window_long_click_subscribe(BUTTON_ID_SELECT, 0, select_long_click_handler, NULL);
  window_long_click_subscribe(BUTTON_ID_DOWN, 0, down_long_click_handler, NULL);
}

// ---------------------------------------------------------------------------
// Window lifecycle
// ---------------------------------------------------------------------------
static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  s_omnitrix_layer = layer_create(bounds);
  layer_set_update_proc(s_omnitrix_layer, omnitrix_update_proc);
  layer_add_child(window_layer, s_omnitrix_layer);

  // Status overlay (countdown / alien / cooldown).
  s_time_layer = text_layer_create(GRect(0, bounds.size.h / 2 - 30, bounds.size.w, 44));
  text_layer_set_background_color(s_time_layer, GColorClear);
  text_layer_set_text_color(s_time_layer, GColorWhite);
  text_layer_set_font(s_time_layer, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD));
  text_layer_set_text_alignment(s_time_layer, GTextAlignmentCenter);

  s_date_layer = text_layer_create(GRect(0, bounds.size.h / 2 + 18, bounds.size.w, 28));
  text_layer_set_background_color(s_date_layer, GColorClear);
  text_layer_set_text_color(s_date_layer, GColorWhite);
  text_layer_set_font(s_date_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD));
  text_layer_set_text_alignment(s_date_layer, GTextAlignmentCenter);

  layer_set_hidden(text_layer_get_layer(s_time_layer), true);
  layer_set_hidden(text_layer_get_layer(s_date_layer), true);

  layer_add_child(window_layer, text_layer_get_layer(s_time_layer));
  layer_add_child(window_layer, text_layer_get_layer(s_date_layer));
}

static void main_window_unload(Window *window) {
  layer_destroy(s_omnitrix_layer);
  text_layer_destroy(s_time_layer);
  text_layer_destroy(s_date_layer);
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------
static void init() {
  s_main_window = window_create();
  window_set_background_color(s_main_window, GColorBlack);

  window_set_window_handlers(s_main_window, (WindowHandlers) {
    .load = main_window_load,
    .unload = main_window_unload
  });
  window_set_click_config_provider(s_main_window, click_config_provider);

  window_stack_push(s_main_window, true);
  update_status();

  // One-second tick drives the countdown and auto-transitions.
  tick_timer_service_subscribe(SECOND_UNIT, tick_handler);
  s_anim_timer = app_timer_register(ANIM_INTERVAL_MS, anim_timer_callback, NULL);
}

static void deinit() {
  if (s_anim_timer) {
    app_timer_cancel(s_anim_timer);
  }
  tick_timer_service_unsubscribe();
  window_destroy(s_main_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}
