/* src/juice.h: every feel constant in the cart. Tune here, nowhere else.
 * Spec section 5.4. Owned by the fx agent (item 6); read by fx.c, camera.c,
 * draw.c and main.c. Headless safe: nothing from three.c, hud.h or wasmcart. */
#ifndef RTR_JUICE_H
#define RTR_JUICE_H
#include <stdint.h>
#include "audio.h"
#include "sim.h"

typedef struct { uint8_t stop; float punch, shake, flash; uint8_t sparks, chips; float rum_lo, rum_hi; uint16_t rum_ms; uint8_t sfx, crowd; } hit_band_t;
enum { BAND_TAP, BAND_HIT, BAND_HEAVY, BAND_HUGE, BAND_CLASH, BAND_KO, BAND_COUNT };
/* hit stop frames by band. sim.h reads these (STOP_*) because the sim applies the stop
 * itself, so balance --replay stays aligned with the cart. Feel gate (item 7): the hit band
 * holds 2 frames (spec 0) so a 12 damage ram shows its sparks frozen at the contact before
 * they fly; without it the contact frame and the release are the same frame and the ram
 * reads as a brush. */
#define J_STOP_TAP    0
#define J_STOP_HIT    2
#define J_STOP_HEAVY  4
#define J_STOP_HUGE   6
#define J_STOP_CLASH  8
#define J_STOP_KO     6
/* feel gate tuning against the spec table (5.4): punch and shake up a step in the tap, hit
 * and heavy bands, flash quads larger, so the camera visibly kicks on a mid ram */
static const hit_band_t HIT_BANDS[BAND_COUNT] __attribute__((unused)) = {
  /* stop          punch  shake  flash sparks chips  lo    hi    ms   sfx            crowd */
  { J_STOP_TAP,   0.05f, 0.06f, 0.30f,  6, 0, 0.2f, 0.0f,  60, SFX_HIT_LIGHT, 0 },
  { J_STOP_HIT,   0.09f, 0.12f, 0.50f, 12, 2, 0.4f, 0.2f,  80, SFX_HIT_LIGHT, 0 },
  { J_STOP_HEAVY, 0.13f, 0.18f, 0.70f, 20, 4, 0.8f, 0.4f, 120, SFX_HIT_HEAVY, 0 },
  { J_STOP_HUGE,  0.15f, 0.24f, 0.90f, 32, 6, 1.0f, 0.6f, 160, SFX_HIT_HEAVY, 80 },
  { J_STOP_CLASH, 0.14f, 0.28f, 1.00f, 40, 8, 1.0f, 1.0f, 200, SFX_CLASH,     100 },
  { J_STOP_KO,    0.14f, 0.30f, 0.00f, 40, 8, 1.0f, 1.0f, 400, SFX_KO,        100 },
};
/* the band edges live in sim.h (BAND_*_MIN: tap 1 to 5, hit 6 to 10, heavy 11 to 17, huge 18 and up),
 * under the HIT_CAP of a fifth of the bar; the sim applies the stop from the same edges */
#define J_NUMBER_BIG_MIN      BAND_HIT_MIN       /* damage numbers from the hit band up use F_H2 */
static inline int hit_band(float dmg) { return dmg < BAND_HIT_MIN ? BAND_TAP : dmg < BAND_HEAVY_MIN ? BAND_HIT : dmg < BAND_HUGE_MIN ? BAND_HEAVY : BAND_HUGE; }
#define J_BAND_HUGE_WHITE_A   40     /* viewport white alpha, 1 frame */
#define J_BAND_CLASH_WHITE_A  60
#define J_SHAKE_CLAMP         0.34f
#define J_SHAKE_TAU           0.12f
#define J_SHAKE_HZ            28.0f
#define J_SHAKE_SCALE         { 0.0f, 0.5f, 1.0f, 1.5f }   /* settings 0..3 */
#define J_PUNCH_T             0.30f
#define J_STINGER_BIGHIT_MIN  BAND_HUGE_MIN      /* BIG HIT on a huge band hit (the spec's 25 sat above HIT_CAP) */
#define J_SPARK_SPEED_MIN     4.0f
#define J_SPARK_SPEED_MAX     9.0f
#define J_SPARK_CONE_DEG      50.0f
#define J_SPARK_INHERIT       2.0f
#define J_SPARK_GRAVITY       14.0f
#define J_SPARK_BOUNCE        0.3f
#define J_SPARK_LIFE_MIN      0.35f
#define J_SPARK_LIFE_MAX      0.50f
#define J_SPARK_SIZE_MIN      0.08f  /* spec 0.05: read as dust in a half */
#define J_SPARK_SIZE_MAX      0.13f  /* spec 0.09 */
#define J_SPARK_CAP           400
#define J_SMOKE_CAP           160
#define J_SMOKE_RATE_50       4
#define J_SMOKE_RATE_30       6
#define J_SMOKE_LIFE          72
#define J_SMOKE_RISE          0.8f
#define J_FIRE_RATE_30        8
/* smoke and fire by the fraction of max HP (graphics 4.5): grey from 0.40, char and fire from 0.20,
 * out of the most damaged side; the KO frame smokes in a column for 3 s, burns for 180 frames, then embers */
#define J_SMOKE_FRAC_GREY     0.40f
#define J_SMOKE_FRAC_CHAR     0.20f
#define J_SMOKE_RATE_40       4
#define J_SMOKE_RATE_20       7
#define J_FIRE_RATE_20        10
#define J_KO_SMOKE_RATE       12
#define J_KO_SMOKE_S          3.0f
#define J_KO_FIRE_S           3.0f
#define J_KO_EMBER_RATE       1
/* the impact star (graphics 4.4): crossed bone streaks at the contact for 2 frames (3 on a KO, at 1.4 size),
 * the warm disc behind it at half its old size; the heavy band ticks the victim's viewport white */
#define J_STAR_FRAMES         2
#define J_STAR_KO_FRAMES      3
#define J_STAR_SIZE           1.1f   /* times the band's flash */
#define J_STAR_KO_SIZE        1.4f
#define J_BAND_HEAVY_WHITE_A  16
#define J_BAND_KO_WHITE_FRAMES 2
/* the launch trail (graphics 4.9) */
#define J_TRAIL_CUBES         8
#define J_TRAIL_LIFE          0.3f
#define J_TRAIL_FREE_FRAMES   8
#define J_STREAK_FRAMES       8
/* where the sparks of a hit come from and go, per weapon (graphics 4.4): y is the contact height over the
 * victim's floor when the sim does not give one; the cone axis is along the attacker's forward (along),
 * the weapon's tangent (tangent, spinners), or back off the hit normal (the rest), plus up; cone is the
 * half angle in degrees; speed multiplies J_SPARK_SPEED_*. Indexed by WP_*, then EVW_MELTY, EVW_HAZARD */
typedef struct { float y, along, tangent, up, cone, speed; } spark_spray_t;
static const spark_spray_t SPRAY[WP_COUNT + 2] __attribute__((unused)) = {
  /* NONE */     { 0.25f, 0.0f, 0.0f, 0.7f, 50.0f, 1.0f },
  /* WEDGE */    { 0.08f, 1.0f, 0.0f, 0.3f, 35.0f, 1.0f },   /* a low fan forward along the plate, sparks skid on the floor */
  /* SPIKE */    { 0.30f, 1.0f, 0.0f, 0.2f, 20.0f, 1.3f },   /* a tight jet along the rod */
  /* LIFTER */   { 0.10f, 0.0f, 0.0f, 0.6f, 60.0f, 0.5f },
  /* SAW */      { 0.35f, 0.0f, 1.0f, 0.9f, 25.0f, 1.2f },   /* a rooster tail up the disc tangent */
  /* FLAME */    { 0.30f, 1.0f, 0.0f, 0.5f, 40.0f, 0.4f },
  /* HAMMER */   { 0.45f, 0.0f, 0.0f, 0.1f, 80.0f, 1.1f },   /* a flat radial fan from under the head */
  /* FLIPPER */  { 0.15f, 1.0f, 0.0f, 0.8f, 45.0f, 0.8f },
  /* DRUM */     { 0.30f, 0.6f, 0.0f, 1.0f, 40.0f, 1.3f },   /* up and over the drum */
  /* HORIZ */    { 0.40f, 0.0f, 1.0f, 0.2f, 20.0f, 1.5f },   /* a long tangent streak, the signature spinner spray */
  /* CRUSHER */  { 0.30f, 0.0f, 0.0f, 0.5f, 60.0f, 0.4f },
  /* MELTY */    { 0.25f, 0.0f, 1.0f, 0.3f, 25.0f, 1.4f },
  /* hazard */   { 0.15f, 0.0f, 0.0f, 0.8f, 60.0f, 1.0f } };
#define J_FIRE_LIFE           24
#define J_FIRE_RISE           1.5f
#define J_DEBRIS_CAP          24
#define J_DEBRIS_PANEL_LIFE   360
#define J_DEBRIS_WHEEL_LIFE   240
#define J_DEBRIS_FADE         60
#define J_DEBRIS_CHIP_LIFE    120
#define J_NUMBER_RISE_PX      40
#define J_NUMBER_LIFE         48     /* frames; alpha fades over the last 18 */
#define J_NUMBERS_PER_VIEW    6
#define J_STINGER_FRAMES      36     /* 6 in, 24 hold, 6 out */
#define J_KO_TIMESCALE        0.2f
#define J_KO_SLOW_FRAMES      90
#define J_KO_HOLD_FRAMES      60
#define J_KO_SCATTER          2.2f   /* the KO burst's spread over a fight shed, and it flies at full speed through the slow motion */
#define J_KO_ORBIT_DEG_S      30.0f
#define J_KO_ORBIT_DIST       4.5f
#define J_KO_ORBIT_HEIGHT     2.4f
#define J_KO_VIGNETTE_A       60
#define J_MINIKO_TIMESCALE    0.4f
#define J_MINIKO_FRAMES       36
#define J_TAPE_CUT_FRAMES     24     /* 12 in, hold 1, 12 out at 0.4 s */
#define J_SPIN_RING_A         90     /* the floor danger ring under a spinner, times spin; its echo ring is a third of it (graphics 4.8) */
#define J_HAZARD_TELL_HZ      2.0f
#define J_HAZARD_SHAKE        0.08f
#define J_OPS_SHAKE           0.06f
#define J_LAND_SHAKE          0.06f  /* 5.2: landing, own viewport */
#define J_WALL_SHAKE          0.04f  /* 5.2: wall hit over 2 u/s */
#define J_SPINWALL_SHAKE      0.10f  /* 5.2: spinner wall hit */
#define J_REDTAG_SHAKE        0.04f  /* 5.2: RED TAG */
#define J_REDTAG_SPARKS       20
#define J_PICKUP_CUBES        8
#define J_LANDING_DUST        10
#define J_LAUNCH_DUST         6
#define J_WALL_SPARKS         8
#define J_SPINWALL_SPARKS     30
#define J_HAMMER_MISS_SPARKS  10
#define J_BARS_ANIM_FRAMES    18
#define J_BARS_DELTA_HOLD     72
#define J_SLOT_MACHINE_FRAMES 36
#define J_LOCK_CONFETTI       12
#define J_TAUNT_CONFETTI      4
#define J_FX2_CAP             128

#endif
