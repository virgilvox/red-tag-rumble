/* src/juice.h: every feel constant in the cart. Tune here, nowhere else.
 * Spec section 5.4. Owned by the fx agent (item 6); read by fx.c, camera.c,
 * draw.c and main.c. Headless safe: nothing from three.c, hud.h or wasmcart. */
#ifndef RTR_JUICE_H
#define RTR_JUICE_H
#include <stdint.h>
#include "audio.h"

typedef struct { uint8_t stop; float punch, shake, flash; uint8_t sparks, chips; float rum_lo, rum_hi; uint16_t rum_ms; uint8_t sfx, crowd; } hit_band_t;
enum { BAND_TAP, BAND_HIT, BAND_HEAVY, BAND_HUGE, BAND_CLASH, BAND_KO, BAND_COUNT };
static const hit_band_t HIT_BANDS[BAND_COUNT] __attribute__((unused)) = {
  /* stop punch shake flash sparks chips  lo   hi   ms   sfx            crowd */
  {  0, 0.04f, 0.04f, 0.0f,  6, 0, 0.2f, 0.0f,  60, SFX_HIT_LIGHT, 0 },
  {  0, 0.06f, 0.08f, 0.3f, 12, 0, 0.4f, 0.2f,  80, SFX_HIT_LIGHT, 0 },
  {  3, 0.10f, 0.14f, 0.5f, 20, 0, 0.8f, 0.4f, 120, SFX_HIT_HEAVY, 0 },
  {  6, 0.12f, 0.22f, 0.7f, 32, 6, 1.0f, 0.6f, 160, SFX_HIT_HEAVY, 80 },
  {  8, 0.12f, 0.26f, 0.9f, 40, 8, 1.0f, 1.0f, 200, SFX_CLASH,     100 },
  {  6, 0.12f, 0.30f, 0.0f, 40, 8, 1.0f, 1.0f, 400, SFX_KO,        100 },
};
static inline int hit_band(float dmg) { return dmg < 8 ? BAND_TAP : dmg < 15 ? BAND_HIT : dmg < 30 ? BAND_HEAVY : BAND_HUGE; }
#define J_BAND_HUGE_WHITE_A   40     /* viewport white alpha, 1 frame */
#define J_BAND_CLASH_WHITE_A  60
#define J_SHAKE_CLAMP         0.34f
#define J_SHAKE_TAU           0.12f
#define J_SHAKE_HZ            28.0f
#define J_SHAKE_SCALE         { 0.0f, 0.5f, 1.0f, 1.5f }   /* settings 0..3 */
#define J_PUNCH_T             0.30f
#define J_STINGER_BIGHIT_MIN  25.0f
#define J_SPARK_SPEED_MIN     4.0f
#define J_SPARK_SPEED_MAX     9.0f
#define J_SPARK_CONE_DEG      50.0f
#define J_SPARK_INHERIT       2.0f
#define J_SPARK_GRAVITY       14.0f
#define J_SPARK_BOUNCE        0.3f
#define J_SPARK_LIFE_MIN      0.35f
#define J_SPARK_LIFE_MAX      0.50f
#define J_SPARK_SIZE_MIN      0.05f
#define J_SPARK_SIZE_MAX      0.09f
#define J_SPARK_CAP           400
#define J_SMOKE_CAP           160
#define J_SMOKE_RATE_50       4
#define J_SMOKE_RATE_30       6
#define J_SMOKE_LIFE          72
#define J_SMOKE_RISE          0.8f
#define J_FIRE_RATE_30        8
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
#define J_KO_ORBIT_DEG_S      30.0f
#define J_KO_ORBIT_DIST       4.5f
#define J_KO_ORBIT_HEIGHT     2.4f
#define J_KO_VIGNETTE_A       60
#define J_MINIKO_TIMESCALE    0.4f
#define J_MINIKO_FRAMES       36
#define J_TAPE_CUT_FRAMES     24     /* 12 in, hold 1, 12 out at 0.4 s */
#define J_SPIN_RING_A         160    /* times spin */
#define J_SPIN_BLUR_A         50
#define J_SPIN_BLUR_DEG       15
#define J_HAZARD_TELL_HZ      2.0f
#define J_HAZARD_SHAKE        0.08f
#define J_OPS_SHAKE           0.06f
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
