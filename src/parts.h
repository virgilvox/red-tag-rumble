/* parts.h: the catalogue. Chassis, drive, weapon, armour and paint tables with
 * stats, costs, flavour, adjectives and nouns (spec section 3); build_t; the
 * budget rules; the presets; RANDOM BOT; the three bars; bot_name.
 *
 * Owned by the parts agent (item 2). Everything here is static inline so the
 * header is the whole contract: sim.c, bot.c, ai.c, game.c, draw*.c and the
 * native balance tool all read the same tables. Headless safe: nothing from
 * three.c, hud.h or wasmcart. Only sqrtf would be allowed from libm; none is
 * used. The one RNG of the game (xorshift32) lives here too so the garage,
 * the sim and the AI roll the same way. */
#ifndef RTR_PARTS_H
#define RTR_PARTS_H
#include <stdint.h>
#include <stdio.h>
#include "content.h"

/* -- ids ----------------------------------------------------------- */
enum { CH_BOX, CH_WEDGE, CH_LOWBOY, CH_SHELL, CH_FRAME, CH_LUNCHBOX, CH_COUNT };
enum { DR_TWO, DR_FOUR, DR_SIX, DR_TREAD, DR_HOVER, DR_WALKER, DR_LEGTRACK, DR_MELTY, DR_COUNT };
enum { WP_NONE, WP_WEDGE, WP_SPIKE, WP_LIFTER, WP_SAW, WP_FLAME, WP_HAMMER, WP_FLIPPER, WP_DRUM, WP_HORIZ, WP_CRUSHER, WP_COUNT };
enum { AR_NONE, AR_PLY, AR_ALU, AR_UHMW, AR_STEEL, AR_TI, AR_WEAR, AR_COUNT };
enum { PAINT_COUNT = 4, DECAL_COUNT = 4 };
/* garage rows (3.1); ROW_WEAPON2 exists only while a secondary mount is allowed */
enum { ROW_CHASSIS, ROW_DRIVE, ROW_WEAPON, ROW_ARMOUR, ROW_PAINT, ROW_WEAPON2, ROW_COUNT };
/* presets (3.9); 7 is CUSTOM (bot_t.preset) */
enum { PRE_ROLL_OUT, PRE_LAUNCH, PRE_CHOP, PRE_STOMP, PRE_TOAST, PRE_SPIN, PRE_RANDOM, PRE_CUSTOM, PRE_COUNT = 7 };
/* body shapes (sim 4.5) */
enum { SH_BOX, SH_CIRCLE };
/* weapon input kinds (3.5) */
enum { WI_PASSIVE, WI_TAP, WI_HOLD, WI_TOGGLE };
/* self-right kinds (3.5): none, B for 90 frames, A while spinning (DRUM) */
enum { SR_NONE, SR_B, SR_DRUM };
/* spark colour sets per armour (3.6) */
enum { SP_AMBER_DUST, SP_SPLINTER_BONE, SP_WHITE, SP_SCRAPE, SP_AMBER_RUST, SP_CYCLE, SP_RUST_FEW };
/* paint decal sets (3.7) */
enum { DC_NONE, DC_HAZARD_STRIPES, DC_TAPE_BAND, DC_SEAT_NUMBER };

#define BUDGET_BASE 20
#define BAR_COUNT 3
enum { BAR_SPEED, BAR_HIT, BAR_TOUGH };

/* -- build --------------------------------------------------------- */
typedef struct { uint8_t chassis, drive, weapon, weapon2, armour, paint, decal; } build_t;

/* -- tables -------------------------------------------------------- */
typedef struct {
  const char *name, *short_name, *flavour;   /* short_name is at most 9 chars for quadrant rows */
  uint8_t tags, hp; float mass;
  float len, wid, hgt;                        /* L x W x H in units; len is the diameter for SH_CIRCLE */
  uint8_t flip_odds, shape;
} chassis_def_t;

typedef struct {
  const char *name, *short_name, *flavour;
  uint8_t tags, bonus;                        /* bonus budget (WALKER 4, LEG TRACKS 2) */
  float top, accel, turn_deg, traction, mass, knock_mult;
  uint8_t boost, strafe;                      /* strafe: HOVER and MELTY steer by world direction */
} drive_def_t;

typedef struct {
  const char *name, *short_name, *flavour;
  uint8_t tags;
  float base, bar_base;                       /* base damage; bar_base is the value the HIT bar formula uses */
  float spin_up;                              /* seconds to full spin (spinners, saw, melty); 0 = none */
  int16_t windup, active, cooldown, stun;     /* frames */
  float knock, launch_vy, self_dmg, mass;
  float hb_w, hb_d, hb_z0, hb_z1, hb_r;       /* box: width, (unused), z0..z1 in the attacker frame; circle: r at hb_z0 */
  uint8_t input, inv_drive, inv_operate, selfright, secondary_ok;
} weapon_def_t;

typedef struct {
  const char *name, *short_name, *flavour;
  uint8_t tags; float reduction, mass;
  uint8_t knock_half, burn_immune, spark;
} armour_def_t;

typedef struct { const char *name; uint8_t chassis, drive, weapon, armour; const char *blurb; } preset_def_t;

static const chassis_def_t CHASSIS[CH_COUNT] __attribute__((unused)) = {
  /* name        short       flavour                           tags hp  mass  L     W     H     flip shape */
  { "BOX",      "BOX",      "A PELICAN CASE WITH OPINIONS",   2, 100, 1.0f, 1.0f, 0.8f, 0.40f, 50, SH_BOX },
  { "WEDGE",    "WEDGE",    "DOORSTOP, BUT ANGRY",            3,  90, 1.0f, 1.1f, 0.8f, 0.30f, 45, SH_BOX },
  { "LOWBOY",   "LOWBOY",   "IT WAS A SERVER CHASSIS ONCE",   3,  85, 0.9f, 1.0f, 0.9f, 0.25f, 30, SH_BOX },
  { "SHELL",    "SHELL",    "WOK. IT WAS A WOK",              4, 110, 1.1f, 1.0f, 1.0f, 0.45f, 50, SH_CIRCLE },
  { "FRAME",    "FRAME",    "OPEN TUBE. VERY HONEST",         1,  70, 0.8f, 1.0f, 0.7f, 0.35f, 50, SH_BOX },
  { "LUNCHBOX", "LUNCHBOX", "TALL, TIPPY, HOLDS SNACKS",      2, 100, 1.0f, 0.9f, 0.7f, 0.70f, 70, SH_BOX },
};

static const drive_def_t DRIVES[DR_COUNT] __attribute__((unused)) = {
  /* name          short        flavour                                   tags bonus top   accel turn  trac  mass  knock boost strafe */
  { "TWO WHEEL",   "TWO WHEEL", "SCOOTER PARTS. BOTH OF THEM",            2, 0, 6.0f, 12.f, 300.f, 0.60f, 0.20f, 1.0f, 1, 0 },
  { "FOUR WHEEL",  "FOUR WH",   "FOUR WHEELS, ZERO DRAMA",                3, 0, 5.0f,  8.f, 220.f, 0.80f, 0.30f, 1.0f, 1, 0 },
  { "SIX WHEEL",   "SIX WHEEL", "SIX WHEELS, ONE AGENDA",                 4, 0, 4.2f,  8.f, 180.f, 0.95f, 0.40f, 0.8f, 1, 0 },
  { "TREADS",      "TREADS",    "LOUD. SLOW. UNMOVABLE. LIKE A MEETING.", 5, 0, 3.8f,  5.f, 160.f, 1.00f, 0.60f, 0.5f, 0, 0 },
  { "HOVER",       "HOVER",     "LEAF BLOWER. DO NOT ASK",                5, 0, 5.5f, 30.f, 360.f, 0.30f, 0.10f, 1.0f, 1, 1 },
  { "WALKER",      "WALKER",    "LEGS WERE A CHOICE",                     1, 4, 3.0f,  5.f, 120.f, 0.70f, 0.35f, 0.9f, 1, 0 },
  { "LEG TRACKS",  "LEG TRACK", "TANK WITH KNEES",                        3, 2, 3.4f,  5.f, 140.f, 0.90f, 0.50f, 0.7f, 0, 0 },
  { "MELTY BRAIN", "MELTY",     "THE WHOLE BOT IS THE WEAPON",            6, 0, 4.5f,  8.f,   0.f, 0.40f, 0.20f, 1.0f, 1, 1 },
};

static const weapon_def_t WEAPONS[WP_COUNT] __attribute__((unused)) = {
  /* name         short      flavour                                       tags base bar  spinup windup act cd   stun knock lvy  self  mass  hbw   hbd  z0    z1    r     input      invdrv invop sr       sec */
  { "NONE",       "NONE",    "LEAVE THE SLOT EMPTY. SAVE TAGS",            0,  0,  0,   0.0f,  0,  0,   0,   0,  0.0f, 0,  0.0f, 0.00f, 0,    0,   0,    0,    0,    WI_PASSIVE, 1, 1, SR_NONE, 0 },
  { "WEDGE",      "WEDGE",   "NO WEAPON. THAT IS THE WEAPON",              1, 10, 10,   0.0f,  0,  0,   0,  15,  4.0f, 0,  0.0f, 0.05f, 0.8f, 0,   0.5f, 0.6f, 0,    WI_PASSIVE, 1, 1, SR_NONE, 1 },
  { "SPIKE",      "SPIKE",   "A PUNCH IN A STRAIGHT LINE",                 3, 15, 15,   0.0f,  0,  9,  60,  18,  2.5f, 0,  0.0f, 0.10f, 0.25f, 0,  0.55f, 1.05f, 0,   WI_TAP,     1, 1, SR_NONE, 0 },
  { "LIFTER",     "LIFTER",  "GETS UNDER, LIFTS, CARRIES",                 3,  4,  4,   0.0f,  0,  0,   0,  18,  1.2f, 0,  0.0f, 0.10f, 0.6f, 0,   0.5f, 0.8f, 0,    WI_HOLD,    0, 0, SR_NONE, 1 },
  { "SAW",        "SAW",     "NEEDS A MOMENT WITH YOU",                    3,  4, 12,   0.8f,  0,  0,   0,   6,  0.4f, 0,  2.0f, 0.10f, 0,    0,   0.7f, 0,    0.3f, WI_TOGGLE,  1, 1, SR_NONE, 0 },
  { "FLAME",      "FLAME",   "SCARY. MILD. GREAT ON CAMERA",               4,  2, 10,   0.0f,  0,  0,   0,   0,  0.0f, 0,  0.0f, 0.10f, 0,    0,   0,    1.5f, 0,    WI_HOLD,    0, 0, SR_NONE, 0 },
  { "HAMMER",     "HAMMER",  "TELEGRAPHS. THEN DENTS",                     5, 20, 20,   0.0f, 15,  4,  72,  30,  1.2f, 0,  4.0f, 0.20f, 0.4f, 0,   0.5f, 1.0f, 0,    WI_TAP,     0, 0, SR_B,    0 },
  { "FLIPPER",    "FLIPPER", "GRAVITY, BUT FOR THEM",                      6,  6,  6,   0.0f,  0,  4, 108,  36,  3.0f, 7,  0.0f, 0.20f, 0.6f, 0,   0.4f, 0.9f, 0,    WI_TAP,     0, 0, SR_B,    0 },
  { "DRUM",       "DRUM",    "POPS THEM UP. HITS THE FLOOR TO SELF RIGHT", 6, 17, 17,   1.5f,  0,  0,   0,  24,  4.0f, 3,  3.0f, 0.30f, 0,    0,   0.55f, 0,   0.3f, WI_HOLD,    1, 1, SR_DRUM, 0 },
  { "HORIZONTAL", "HORIZ",   "CHAINSAW YOU CANNOT STEER",                  7, 22, 22,   2.0f,  0,  0,   0,  21,  6.0f, 0,  4.0f, 0.30f, 0,    0,   0.15f, 0,   0.55f, WI_HOLD,   0, 1, SR_NONE, 0 },
  { "CRUSHER",    "CRUSHER", "HOLDS ON. TWO SECONDS. AWKWARD",             7, 15, 15,   0.0f, 24,  0, 120, 120,  0.0f, 0,  0.0f, 0.30f, 0.5f, 0,   0.5f, 0.9f, 0,    WI_TAP,     0, 0, SR_NONE, 0 },
};
/* weapon extras that do not fit the table: SAW ticks every 10 frames of contact;
 * FLAME ticks every 30 frames, fuel 240 frames, refill 180, burn 1/s for 3 s and
 * a burning bot is not lit again until the burn ends; HORIZONTAL loses 15 HP and
 * all spin on a wall over 0.5 spin; MELTY edges hit for 12 at spin over 0.5 with
 * spin_up 1.5, 3 self damage per hit and no weapon row. The base damages sit one notch under the spec
 * tables (the sim-feel retune of 4.7): no hit over HIT_CAP of the bar, one approach
 * is one hit and then a spin up, and a two bot fight runs 35 to 60 s. */
#define WP_SAW_TICK_FRAMES    10
#define WP_FLAME_TICK_FRAMES  30
#define WP_FLAME_FUEL         240
#define WP_FLAME_REFILL       180
#define WP_FLAME_BURN_T       180
#define WP_FLAME_BURN_DPS     1.0f
#define WP_HORIZ_WALL_DMG     15.0f
#ifndef WP_MELTY_DMG
#define WP_MELTY_DMG          12.0f
#endif
#ifndef WP_MELTY_SPIN_UP
#define WP_MELTY_SPIN_UP      1.5f
#endif
#ifndef WP_MELTY_WALL_DMG
#define WP_MELTY_WALL_DMG     10.0f
#endif
#ifndef WP_MELTY_SELF
#define WP_MELTY_SELF         3.0f    /* the frame takes the knock of every edge hit it lands */
#endif
#define WP_CRUSHER_HOLD_DPS   6.0f
#define WP_LIFTER_FRAMES      36
#define WP_LIFTER_MAX_HOLD    180

/* -- the body and contact tables (DEEP-plan 6, deep-physics 3) ------
 * Ground clearance by drive (the ride height the renderer draws); a HOVER skirt is drawn at 0.22 but is
 * soft, so a lip gets under it as if it were 0.06 */
static const float DRIVE_CLEAR[DR_COUNT] __attribute__((unused)) = { 0.12f, 0.10f, 0.10f, 0.12f, 0.22f, 0.30f, 0.10f, 0.10f };
#define HOVER_CLEAR_SOFT 0.06f
/* lateral grip by drive, as a share of mu g: how hard the tyres, tracks or feet stop a sideways slide
 * (TREADS barely slide, TWO WHEEL skids, HOVER drifts across the arena) */
#ifndef LAT_GRIP_TWO
#define LAT_GRIP_TWO 0.8f
#endif
#ifndef LAT_GRIP_FOUR
#define LAT_GRIP_FOUR 1.0f
#endif
#ifndef LAT_GRIP_TREAD
#define LAT_GRIP_TREAD 1.6f
#endif
static const float LAT_GRIP[DR_COUNT] __attribute__((unused)) = { LAT_GRIP_TWO, LAT_GRIP_FOUR, 1.2f, LAT_GRIP_TREAD, 0.08f, 1.0f, 1.4f, 0.5f };
/* the share of a drive's grip a ridden bot loses at full ride (load = 1 - this x ride_up): a TWO WHEEL's
 * axle is at its middle and a WALKER's feet leave the floor with its belly, a HOVER is carried outright */
static const float DRIVE_RIDE_LOSS[DR_COUNT] __attribute__((unused)) = { 1.0f, 0.75f, 0.75f, 0.75f, 1.0f, 0.9f, 0.75f, 0.75f };
/* the pitch past which an unsupported bot goes over, by chassis, plus the drive's adjustment (wide and
 * heavy tracks hold on, tall legs and a short wheelbase let go) */
static const float CHASSIS_TIP_DEG[CH_COUNT] __attribute__((unused)) = { 55, 58, 66, 50, 55, 35 };
static const float DRIVE_TIP_ADJ_DEG[DR_COUNT] __attribute__((unused)) = { -5, 0, 0, 6, 0, -5, 6, 0 };
/* the flipper tumble rate per chassis, rad/s at k 0.5 (the old flip_odds column, as a rate: a LUNCHBOX goes
 * over from almost any launch, a LOWBOY needs a deep hit on a bot driving in) */
static const float CHASSIS_TUMBLE[CH_COUNT] __attribute__((unused)) = { 4.5f, 4.2f, 2.7f, 4.5f, 5.0f, 6.3f };
/* the closing speed at which a wedge that rode all the way under a chassis tips it over (u/s); a SHELL never
 * tips (it rolls off), a HOVER never tips (it is carried) */
#ifndef V_TIP_BOX
#define V_TIP_BOX 3.4f
#endif
#ifndef V_TIP_LUNCHBOX
#define V_TIP_LUNCHBOX 2.2f
#endif
#define V_TIP_NEVER 99.0f
static const float CHASSIS_VTIP[CH_COUNT] __attribute__((unused)) = { V_TIP_BOX, 3.8f, 4.6f, V_TIP_NEVER, 3.0f, V_TIP_LUNCHBOX };
static const float DRIVE_VTIP_ADJ[DR_COUNT] __attribute__((unused)) = { -0.4f, 0, 0, 0.6f, 0, -0.3f, 0.6f, 0 };
/* per panel armour pools (3.10): what each of the five plates takes before it falls off */
static const float ARMOUR_PANEL_HP[AR_COUNT] __attribute__((unused)) = { 0, 20, 28, 24, 36, 32, 44 };
/* the wedge kinds (3.5): the lowest lip on a bot's front decides. Lip height over the floor, the slope's
 * length S and the lift at the top of it (S tan slope: 26, 20, 15 and 12 degrees) */
enum { WK_NONE = -1, WK_CHASSIS, WK_PLATE, WK_FORKS, WK_NOSE, WK_COUNT };
static const struct { float lip, len, lift; } WEDGE_KIND[WK_COUNT] __attribute__((unused)) = {
  { 0.02f, 0.60f, 0.2926f },     /* the WEDGE chassis: 26 degrees over 0.60 */
  { 0.04f, 0.45f, 0.1638f },     /* the WEDGE plate on any chassis: 20 degrees over 0.45 */
  { 0.05f, 0.35f, 0.0938f },     /* LIFTER forks while lowered: 15 degrees over 0.35 */
  { 0.08f, 0.30f, 0.0638f },     /* the LOWBOY pancake nose: 12 degrees over 0.30 */
};
/* the hammer arc (3.7): the arm is pinned HAMMER_PIVOT_UP over the chassis top and HAMMER_PIVOT_Z behind
 * the centre, raised HAMMER_A0 degrees (from the forward horizontal) at the end of the windup, and strikes
 * the floor HAMMER_REACH in front of the pivot at the end of the swing; the head is a circle of HAMMER_HEAD_R */
#define HAMMER_PIVOT_UP 0.1f
#define HAMMER_PIVOT_Z  (-0.1f)
#define HAMMER_A0       110.0f
#define HAMMER_REACH    1.0f
#define HAMMER_HEAD_R   0.25f
#define HAMMER_ACTIVE   10

static const armour_def_t ARMOURS[AR_COUNT] __attribute__((unused)) = {
  /* name         short       flavour                                tags red    mass  khalf burn spark */
  { "NONE",      "NONE",     "ALL FRAME, ALL FAITH",                 0, 0.00f, 0.00f, 0, 0, SP_AMBER_DUST },
  { "PLYWOOD",   "PLYWOOD",  "OFFCUT BIN BY THE LASER",              1, 0.10f, 0.10f, 0, 0, SP_SPLINTER_BONE },
  { "ALUMINIUM", "ALUMIN",   "BRIGHT DENTS. VERY VISIBLE",           2, 0.20f, 0.20f, 0, 0, SP_WHITE },
  { "UHMW",      "UHMW",     "CUTTING BOARD. DO NOT TELL KITCHEN",   3, 0.15f, 0.20f, 1, 1, SP_SCRAPE },
  { "STEEL",     "STEEL",    "HEAVY. YOU WILL FEEL IT TURNING",      4, 0.35f, 0.50f, 0, 0, SP_AMBER_RUST },
  { "TITANIUM",  "TITANIUM", "SOMEONE DONATED THIS. REALLY",         5, 0.30f, 0.30f, 0, 0, SP_CYCLE },
  { "WEAR PLATE","WEAR PLT", "THE SHELF SAGGED UNDER IT",            6, 0.45f, 0.60f, 0, 0, SP_RUST_FEW },
};

static const preset_def_t PRESETS[PRE_COUNT] __attribute__((unused)) = {
  { "ROLL OUT", CH_WEDGE,    DR_TREAD,  WP_WEDGE,   AR_STEEL, "CANNOT BE PUSHED. BORING. WINS." },
  { "LAUNCH",   CH_BOX,      DR_TWO,    WP_FLIPPER, AR_ALU,   "THE ARCADE CLASSIC" },
  { "CHOP",     CH_LOWBOY,   DR_FOUR,   WP_HORIZ,   AR_ALU,   "GLASS CANNON" },
  { "STOMP",    CH_LUNCHBOX, DR_WALKER, WP_HAMMER,  AR_STEEL, "LEGS AND A GRUDGE" },
  { "TOAST",    CH_SHELL,    DR_HOVER,  WP_FLAME,   AR_UHMW,  "THE TROLL BUILD" },
  { "SPIN",     CH_LOWBOY,   DR_MELTY,  WP_NONE,    AR_TI,    "I HAVE DONE THIS BEFORE" },
  { "RANDOM",   0, 0, 0, 0,                                   "SHELF ROULETTE" },
};
/* paint (3.7): body and stripe per scheme; SEAT means the seat colour, resolved in world.c */
enum { PC_SEAT, PC_CHAR, PC_BONE, PC_STEEL, PC_HAZARD };
static const struct { uint8_t body, stripe; } PAINT_SCHEMES[PAINT_COUNT] __attribute__((unused)) = {
  { PC_SEAT, PC_CHAR }, { PC_CHAR, PC_SEAT }, { PC_BONE, PC_SEAT }, { PC_STEEL, PC_HAZARD } };
/* illegal option reasons, one word each (3.1) */
#define REASON_WEDGE   "NOT ON WEDGE"
#define REASON_1V1     "NOT IN 1V1"
#define REASON_MELTY   "BODY IS THE WEAPON"
#define REASON_MOUNT   "NO MOUNT"

/* -- the one RNG (xorshift32); everything random in the game goes through it -- */
static inline uint32_t rtr_rand(uint32_t *s) {
  uint32_t x = *s ? *s : 0x9e3779b9u;
  x ^= x << 13; x ^= x >> 17; x ^= x << 5;
  *s = x;
  return x;
}
static inline int rtr_rand_n(uint32_t *s, int n) { return n <= 0 ? 0 : (int)(rtr_rand(s) % (uint32_t)n); }
static inline float rtr_rand_f(uint32_t *s) { return (float)(rtr_rand(s) >> 8) * (1.0f / 16777216.0f); }

/* -- budget -------------------------------------------------------- */
static inline int build_budget(const build_t *b) { return BUDGET_BASE + DRIVES[b->drive].bonus; }

static inline int build_cost(const build_t *b) {
  int c = CHASSIS[b->chassis].tags + DRIVES[b->drive].tags + WEAPONS[b->weapon].tags + ARMOURS[b->armour].tags;
  if (b->weapon2 != WP_NONE) c += WEAPONS[b->weapon2].tags - 1;
  return c;
}

/* a secondary weapon is allowed only with the WEDGE or LIFTER weapon (front mount)
 * or on the LUNCHBOX chassis (top mount), and must be SAW, FLAME or SPIKE */
static inline int build_mount_ok(const build_t *b) {
  if (b->drive == DR_MELTY) return 0;
  return WEAPONS[b->weapon].secondary_ok || b->chassis == CH_LUNCHBOX;
}
static inline int weapon2_allowed(int w) { return w == WP_NONE || w == WP_SAW || w == WP_FLAME || w == WP_SPIKE; }

/* the rules apart from the budget; returns 0 and the one-word reason when a rule fails */
static inline int build_rules(const build_t *b, int players, const char **reason) {
  if (b->weapon == WP_HORIZ && b->chassis == CH_WEDGE) { if (reason) *reason = REASON_WEDGE; return 0; }
  if (b->weapon == WP_CRUSHER && players <= 2) { if (reason) *reason = REASON_1V1; return 0; }
  if (b->drive == DR_MELTY && (b->weapon != WP_NONE || b->weapon2 != WP_NONE)) { if (reason) *reason = REASON_MELTY; return 0; }
  if (b->weapon2 != WP_NONE && (!build_mount_ok(b) || !weapon2_allowed(b->weapon2))) { if (reason) *reason = REASON_MOUNT; return 0; }
  if (reason) *reason = 0;
  return 1;
}

/* playable: a chassis and a drive, the rules, and the budget; weapon NONE and armour NONE are legal */
static inline int build_legal(const build_t *b, int players) {
  if (b->chassis >= CH_COUNT || b->drive >= DR_COUNT || b->weapon >= WP_COUNT || b->armour >= AR_COUNT || b->weapon2 >= WP_COUNT) return 0;
  if (!build_rules(b, players, 0)) return 0;
  return build_cost(b) <= build_budget(b);
}

static inline int row_count(const build_t *b, int row) {
  switch (row) {
    case ROW_CHASSIS: return CH_COUNT;
    case ROW_DRIVE: return DR_COUNT;
    case ROW_WEAPON: return WP_COUNT;
    case ROW_ARMOUR: return AR_COUNT;
    case ROW_PAINT: return PAINT_COUNT;
    case ROW_WEAPON2: return WP_COUNT;
    default: (void)b; return 0;
  }
}
static inline int build_get(const build_t *b, int row) {
  switch (row) {
    case ROW_CHASSIS: return b->chassis; case ROW_DRIVE: return b->drive; case ROW_WEAPON: return b->weapon;
    case ROW_ARMOUR: return b->armour; case ROW_PAINT: return b->paint; case ROW_WEAPON2: return b->weapon2; default: return 0;
  }
}
static inline void build_set(build_t *b, int row, int option) {
  switch (row) {
    case ROW_CHASSIS: b->chassis = (uint8_t)option; break; case ROW_DRIVE: b->drive = (uint8_t)option; break;
    case ROW_WEAPON: b->weapon = (uint8_t)option; break; case ROW_ARMOUR: b->armour = (uint8_t)option; break;
    case ROW_PAINT: b->paint = (uint8_t)option; break; case ROW_WEAPON2: b->weapon2 = (uint8_t)option; break; default: break;
  }
}

/* is `option` in `row` legal against the rest of the build (the budget is not a
 * legality question: an over-budget build shakes, an illegal option is skipped).
 * Returns 1, or 0 with the one-word reason for the flavour slot. */
static inline int option_legal(const build_t *b, int row, int option, int players, const char **reason) {
  build_t t = *b;
  if (reason) *reason = 0;
  if (row == ROW_WEAPON2 && !build_mount_ok(b)) { if (reason) *reason = REASON_MOUNT; return option == WP_NONE; }
  if (row == ROW_WEAPON2 && !weapon2_allowed(option)) { if (reason) *reason = REASON_MOUNT; return 0; }
  build_set(&t, row, option);
  if (row == ROW_DRIVE && option == DR_MELTY) { t.weapon = WP_NONE; t.weapon2 = WP_NONE; }   /* MELTY clears the weapon row */
  if (row == ROW_CHASSIS || row == ROW_WEAPON) { if (!build_mount_ok(&t)) t.weapon2 = WP_NONE; }
  return build_rules(&t, players, reason);
}

/* the seven presets; i 6 is RANDOM and needs build_random instead */
static inline void build_preset(build_t *b, int i) {
  if (i < 0 || i >= PRE_RANDOM) i = PRE_ROLL_OUT;
  const preset_def_t *p = &PRESETS[i];
  b->chassis = p->chassis; b->drive = p->drive; b->weapon = p->weapon; b->weapon2 = WP_NONE; b->armour = p->armour;
  b->decal = DC_NONE;
}

/* RANDOM BOT (3.10): uniform from the legal sets for this player count, then
 * while over budget downgrade the most expensive row one step toward cheaper
 * until legal (never below NONE for weapon and armour); if under 17 tags,
 * upgrade armour then drive one step while it fits. Paint is randomised too. */
static inline void build_random(build_t *b, uint32_t *rng, int players) {
  for (int tries = 0; tries < 64; tries++) {
    b->chassis = (uint8_t)rtr_rand_n(rng, CH_COUNT);
    b->drive = (uint8_t)rtr_rand_n(rng, DR_COUNT);
    b->weapon = b->drive == DR_MELTY ? WP_NONE : (uint8_t)rtr_rand_n(rng, WP_COUNT);
    b->armour = (uint8_t)rtr_rand_n(rng, AR_COUNT);
    b->weapon2 = WP_NONE;
    if (build_rules(b, players, 0)) break;
  }
  b->paint = (uint8_t)rtr_rand_n(rng, PAINT_COUNT);
  b->decal = (uint8_t)rtr_rand_n(rng, DECAL_COUNT);
  /* downgrade the most expensive row one step at a time */
  for (int guard = 0; guard < 32 && build_cost(b) > build_budget(b); guard++) {
    int rc = CHASSIS[b->chassis].tags, rd = DRIVES[b->drive].tags, rw = WEAPONS[b->weapon].tags, ra = ARMOURS[b->armour].tags;
    int best = -1, cost = -1;
    if (rw > cost && b->weapon > WP_NONE) { best = ROW_WEAPON; cost = rw; }
    if (ra > cost && b->armour > AR_NONE) { best = ROW_ARMOUR; cost = ra; }
    if (rd > cost && b->drive > DR_TWO) { best = ROW_DRIVE; cost = rd; }
    if (rc > cost && b->chassis > CH_BOX) { best = ROW_CHASSIS; cost = rc; }
    if (best < 0) break;
    /* one step toward a cheaper option in that row that keeps the rules */
    int cur = build_get(b, best), n = row_count(b, best), pick = -1;
    for (int o = cur - 1; o >= 0; o--) {
      build_t t = *b; build_set(&t, best, o);
      int tc = best == ROW_CHASSIS ? CHASSIS[o].tags : best == ROW_DRIVE ? DRIVES[o].tags : best == ROW_WEAPON ? WEAPONS[o].tags : ARMOURS[o].tags;
      if (tc < cost && build_rules(&t, players, 0)) { pick = o; break; }
    }
    if (pick < 0) { for (int o = 0; o < n; o++) { build_t t = *b; build_set(&t, best, o); int tc = best == ROW_CHASSIS ? CHASSIS[o].tags : best == ROW_DRIVE ? DRIVES[o].tags : best == ROW_WEAPON ? WEAPONS[o].tags : ARMOURS[o].tags; if (tc < cost && build_rules(&t, players, 0)) { pick = o; break; } } }
    if (pick < 0) break;
    build_set(b, best, pick);
  }
  /* spend the shelf: armour up, then drive up, one step each while it fits */
  if (build_cost(b) < 17) {
    for (int o = b->armour + 1; o < AR_COUNT; o++) { build_t t = *b; t.armour = (uint8_t)o; if (build_legal(&t, players)) { *b = t; break; } }
  }
  if (build_cost(b) < 17) {
    for (int o = b->drive + 1; o < DR_COUNT; o++) { build_t t = *b; t.drive = (uint8_t)o; if (t.drive == DR_MELTY) continue; if (build_legal(&t, players)) { *b = t; break; } }
  }
}

/* -- the three bars (3.3 to 3.6, 3.8), each clamped 0..1 ----------- */
static inline float build_mass(const build_t *b) {
  float m = CHASSIS[b->chassis].mass + DRIVES[b->drive].mass + WEAPONS[b->weapon].mass + ARMOURS[b->armour].mass;
  if (b->weapon2 != WP_NONE) m += WEAPONS[b->weapon2].mass;
  return m;
}
/* push = traction squared times the total mass times a stall torque of 8 u/s2; HOVER push is 0.
 * The spec's traction * mass * accel let a TWO WHEEL box (0.6 x 12) out-push TREADS (1.0 x 5), the
 * drive the table calls UNMOVABLE: grip and weight decide a shoving match, the motor only up to the
 * grip limit, so the torque term is one constant for every drive */
#define PUSH_TORQUE 8.0f
static inline float build_push(const build_t *b) {
  const drive_def_t *d = &DRIVES[b->drive];
  if (b->drive == DR_HOVER) return 0;
  return d->traction * d->traction * build_mass(b) * PUSH_TORQUE;
}
static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static inline void build_bars(const build_t *b, float *speed, float *hit, float *tough) {
  const chassis_def_t *c = &CHASSIS[b->chassis];
  const drive_def_t *d = &DRIVES[b->drive];
  const weapon_def_t *w = &WEAPONS[b->weapon];
  const armour_def_t *a = &ARMOURS[b->armour];
  float sp = 0, hi = 0, to = 0;
  /* chassis */
  to += (float)c->hp / 110.f * 0.5f + (1.f - (float)c->flip_odds / 100.f) * 0.5f;
  sp += (1.1f - c->mass) * 0.3f;
  /* drive */
  float turn = b->drive == DR_MELTY ? 360.f : d->turn_deg;
  sp += d->top / 6.0f * 0.6f + d->accel / 30.f * 0.2f + turn / 360.f * 0.2f;
  to += d->traction * 0.3f + (1.f - d->knock_mult) * 0.2f;
  {
    build_t tr = *b; tr.drive = DR_TREAD;
    float pt = build_push(&tr);
    hi += (pt > 0 ? build_push(b) / pt : 0) * 0.2f;
  }
  /* weapon (MELTY: the body is the weapon, counted as a spinner at 18) */
  if (b->drive == DR_MELTY) {
    hi += WP_MELTY_DMG / 30.f * 0.6f + 6.0f / 6.f * 0.2f + (0.2f - WP_MELTY_SPIN_UP / 10.f);
  } else if (b->weapon != WP_NONE) {
    hi += w->bar_base / 30.f * 0.6f + w->knock / 6.f * 0.2f + (w->spin_up == 0 ? 0.2f : 0.2f - w->spin_up / 10.f);
    sp -= w->mass * 0.3f;
    if (!w->inv_operate && w->selfright == SR_NONE) to -= 0.15f;
  }
  if (b->weapon2 != WP_NONE) { const weapon_def_t *w2 = &WEAPONS[b->weapon2]; hi += w2->bar_base / 30.f * 0.3f; sp -= w2->mass * 0.3f; }
  /* armour */
  to += a->reduction / 0.45f * 0.5f;
  sp -= a->mass * 0.4f;
  if (speed) *speed = clamp01(sp);
  if (hit) *hit = clamp01(hi);
  if (tough) *tough = clamp01(to);
}

/* "%s %s" from WEAPON_ADJ and CHASSIS_NOUN; DIZZY when the drive is MELTY BRAIN.
 * Longest is YEETING LUNCHBOX (16); the buffer is 24 and the drawn maximum 20. */
static inline void bot_name(char out[24], const build_t *b) {
  const char *adj = b->drive == DR_MELTY ? MELTY_ADJ : WEAPON_ADJ[b->weapon < WP_COUNT ? b->weapon : WP_NONE];
  snprintf(out, 24, "%s %s", adj, CHASSIS_NOUN[b->chassis < CH_COUNT ? b->chassis : CH_BOX]);
}

/* the preset a build matches, or PRE_CUSTOM */
static inline int build_preset_of(const build_t *b) {
  for (int i = 0; i < PRE_RANDOM; i++) {
    const preset_def_t *p = &PRESETS[i];
    if (b->chassis == p->chassis && b->drive == p->drive && b->weapon == p->weapon && b->armour == p->armour && b->weapon2 == WP_NONE) return i;
  }
  return PRE_CUSTOM;
}

/* option names for a garage row; the WEAPON row reads BODY on MELTY */
static inline const char *option_name(const build_t *b, int row, int option, int short_form) {
  static char scheme[16];
  switch (row) {
    case ROW_CHASSIS: return short_form ? CHASSIS[option].short_name : CHASSIS[option].name;
    case ROW_DRIVE: return short_form ? DRIVES[option].short_name : DRIVES[option].name;
    case ROW_WEAPON: if (b->drive == DR_MELTY) return S_ROW_BODY; /* fallthrough */
    case ROW_WEAPON2: return short_form ? WEAPONS[option].short_name : WEAPONS[option].name;
    case ROW_ARMOUR: return short_form ? ARMOURS[option].short_name : ARMOURS[option].name;
    case ROW_PAINT: snprintf(scheme, sizeof scheme, short_form ? "SCHEM %d" : S_SCHEME, option + 1); return scheme;
    default: return "";
  }
}
static inline const char *option_flavour(int row, int option) {
  switch (row) {
    case ROW_CHASSIS: return CHASSIS[option].flavour;
    case ROW_DRIVE: return DRIVES[option].flavour;
    case ROW_WEAPON: case ROW_WEAPON2: return WEAPONS[option].flavour;
    case ROW_ARMOUR: return ARMOURS[option].flavour;
    default: return "";
  }
}
static inline int option_tags(int row, int option) {
  switch (row) {
    case ROW_CHASSIS: return CHASSIS[option].tags;
    case ROW_DRIVE: return DRIVES[option].tags;
    case ROW_WEAPON: return WEAPONS[option].tags;
    case ROW_WEAPON2: return option == WP_NONE ? 0 : WEAPONS[option].tags - 1;
    case ROW_ARMOUR: return ARMOURS[option].tags;
    default: return 0;
  }
}

#endif
