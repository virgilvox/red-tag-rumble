/* sim.h: the deterministic 60 Hz match simulation (spec section 4). Owned by
 * the sim agent (items 3, 8, 13, 16, 20, 24): bodies in sim.c. Headless safe:
 * nothing from three.c, hud.h or wasmcart; no libm trig (rtr_sin, rtr_cos,
 * rtr_atan2 come from the 1024 entry table in sim.c; sqrtf is allowed).
 * The sim never sees dt, wall time, the camera or the renderer. Inputs are
 * written into bots[i].in before each step by main.c (pads) or ai.c (AI). */
#ifndef RTR_SIM_H
#define RTR_SIM_H
#include <stdint.h>
#include "parts.h"

#define DT         (1.0f / 60.0f)
#define MAX_BOTS   5           /* 4 seats plus OPS */
#define MAX_DEBRIS 24
#define MAX_EVENTS 128
#define MAX_HAZ    12
#define OPS_SLOT   4           /* bots[4] is OPS when is_ops is set */

/* pad_state_t.buttons bits: the W3C layout, the same values as WC_BTN_* so
 * main.c copies wc_pad_t.buttons straight across (headless code never sees wasmcart.h) */
#define IN_A      (1 << 0)
#define IN_B      (1 << 1)
#define IN_X      (1 << 2)
#define IN_Y      (1 << 3)
#define IN_L      (1 << 4)
#define IN_R      (1 << 5)
#define IN_START  (1 << 6)
#define IN_SELECT (1 << 7)
#define IN_UP     (1 << 8)
#define IN_DOWN   (1 << 9)
#define IN_LEFT   (1 << 10)
#define IN_RIGHT  (1 << 11)
#define IN_L3     (1 << 12)
#define IN_R3     (1 << 13)
#define TRIGGER_ON 128         /* lt/rt are 0..255 here; digital at 0.5 (spec 12) */

/* modes (2.2) */
enum { MODE_DEATHMATCH, MODE_KO_FLIP, MODE_LAST_BOT, MODE_HILL, MODE_2V2, MODE_TOURNAMENT, MODE_COUNT };
/* win_how (2.4); the first six index KO_CAUSE and PLACE_WORD + 1 */
enum { KO_HP, KO_FLIP, KO_PIT, KO_ROOF, KO_COUNT, KO_DECK, WIN_JUDGES, WIN_HILL, WIN_NONE };
/* OPS states (7) */
enum { OPS_PARKED, OPS_GO, OPS_LIFT, OPS_CARRY, OPS_RETURN, OPS_PUNISH, OPS_LEAVING };
/* panel ids (4.9) */
enum { PN_FRONT, PN_BACK, PN_LEFT, PN_RIGHT, PN_TOP, PN_MOUNT, PN_COUNT };
/* debris kinds */
enum { DB_PANEL, DB_WHEEL, DB_CHIP, DB_CARDBOARD, DB_BENCH, DB_BUMPER };
/* difficulty (8.4) */
enum { AI_EASY, AI_MEDIUM, AI_HARD };
/* control schemes (12) */
enum { SCHEME_ARCADE, SCHEME_TANK };

typedef struct { int16_t lx, ly, rx, ry; uint16_t buttons; uint8_t lt, rt; float cam_yaw; } pad_state_t;

typedef struct {
  float x, z, h;            /* plane position, heading radians (0 is +z) */
  float vx, vz, w;          /* velocity, angular velocity */
  float y, vy;              /* height channel; y > 0 is airborne */
  float px, pz, ph, py;     /* previous step, for render interpolation */
  float mass, inv_mass, inertia, inv_inertia;
  float hl, hw;             /* half length (local z), half width (local x); hl is the radius for SH_CIRCLE */
  uint8_t shape;            /* SH_BOX or SH_CIRCLE */
} body_t;

typedef struct {
  body_t b;
  uint8_t seat, alive, is_ai, is_ops, team;
  build_t build; char name[24]; uint8_t preset;        /* preset 0..6, 7 = custom */
  float hp, hp_max, burn_t, burn_dps;
  uint8_t inverted, lifted_by, clamped_by, on_deck, in_pit_edge, flip_pending;
  int16_t stun, brake, boost, boost_cd, taunt, selfright, countout, wheel_hit, lock, deck_count;
  float spin, charge, fuel, lift; int16_t cooldown, active, windup, clamp;
  float spin2, charge2, fuel2; int16_t cooldown2, active2;   /* secondary weapon */
  int16_t hit_cd[MAX_BOTS];                 /* per victim pair cooldown */
  uint8_t panel_state[6];                   /* 0 clean 1 2 3 dented, 4 gone */
  int16_t red_tag_t; uint8_t red_tag_used, pickups, not_vacuum_said;
  float dealt, biggest, airtime, aggro, control; int16_t frames_inverted; uint8_t flips, taunts, hazard_hits, wall_hits, clashes;
  pad_state_t in;
  /* extra tallies the awards and judges read (16, 2.5) */
  float spinner_dealt, burn_dealt, hazard_taken; int16_t push_frames; uint8_t flips_landed, saw_on, scheme, ko_how, place;
  /* sim internals (sim.c only writes these; the renderer may read them) */
  pad_state_t prev_in;                      /* last step's pad, for edges */
  uint8_t airborne, on_ramp, pit_state, pinned, still, launched_by, inverted_by, last_hit_by, shed_next, falling;
  float floor_y, peak_y, still_x, still_z, drive_t, drive_s;   /* drive_t, drive_s: this step's throttle and turn after the dead zone */
  int16_t still_t, fast_t, pin_t, stuck_t, pit_t, last_hit_t, respawn_t, prop_cd, ops_dmg_t, carry_t, saw_tick;
  float ops_dmg;                            /* damage dealt to OPS in the last 300 frames */
  int16_t hit_cd2[MAX_BOTS];                /* the secondary weapon's per victim cooldown */
  int16_t haz_t[MAX_HAZ];                   /* per hazard contact or cooldown counter */
  float drive_yaw; uint8_t strafe_held;     /* HOVER and MELTY: the camera yaw latched when the stick left the dead zone */
  int16_t skirt_t;                          /* HOVER: frames since a bot pressed on the skirt; the cushion is pinched and the drive is weak */
} bot_t;

typedef struct { float x, z, y, vx, vz, vy, spin; int16_t life; uint8_t kind, colour, pickup, owner, bounced, panel; } debris_t;   /* panel: PN_* for DB_PANEL */

typedef enum { EV_HIT, EV_CLASH, EV_WALL, EV_RAM, EV_LAUNCH, EV_LAND, EV_INVERT, EV_SELFRIGHT, EV_SHED, EV_BURST,
               EV_KO, EV_MINI_KO, EV_COUNT_TICK, EV_HAZ_TELL, EV_HAZ_HIT, EV_OPS_WAKE, EV_OPS_LEAVE, EV_OPS_LINE,
               EV_WEAPON_TELL, EV_TAUNT, EV_BOOST, EV_HILL_MOVE, EV_RED_TAG, EV_PICKUP, EV_NOT_VACUUM,
               EV_BENCH_BREAK, EV_BUMPER_BREAK, EV_SPIN_WALL, EV_CLOSING, EV_STUCK, EV_HILL_SCORE, EV_RESPAWN, EV_TYPE_COUNT } ev_type_t;
/* a is the attacker or subject bot, b the victim or the hazard index; zone is the
 * hit zone times 10 for hits, the panel for EV_SHED, the KO cause for EV_KO */
typedef struct { uint8_t type, a, b, zone; float x, z, y, dmg, nx, nz; } event_t;

/* active and tell are this step's phase (also set by a seat or auto fire); fire_t counts a fired
 * burst down; cooldown is the seat or auto cooldown; hp is a roof bumper's hp (p1); seat is the
 * hazard seat that has this hazard picked, 255 for none; aux is the laser's beam x or the
 * drop zone's next prop */
typedef struct { uint8_t type; float phase_t; uint8_t active, tell, broken; int16_t hp, cooldown, fire_t; uint8_t seat; float aux; } hazard_state_t;

typedef struct {
  uint32_t rng; int frame; int16_t hitstop; uint8_t mode, arena, players, comeback, closing;
  float clock_s, haz_clock;                  /* clock counts down; haz_clock counts up at 1x or 2x */
  bot_t bots[MAX_BOTS]; int nbots;
  debris_t debris[MAX_DEBRIS]; int ndebris;
  event_t ev[MAX_EVENTS]; int nev;           /* cleared by the caller after each step */
  hazard_state_t haz[MAX_HAZ];
  uint8_t winner, win_how, over; int hill; int score[4];
  uint8_t ops_state; int16_t ops_t; uint8_t ops_target; int16_t no_hit_frames;
  uint32_t seed; int ko_frame;
  /* props with hp (benches, roof bumpers, mound boxes), by prop index in the arena */
  int16_t prop_hp[32]; uint8_t prop_broken[32];
  float wind_x, wind_z; int16_t hill_t; uint8_t hazard_seat[4];   /* chosen hazard per seat, 255 = none */
  uint8_t fight_len_s;
  /* sim internals */
  uint8_t hazards_on, hitstop_off, gate_open, ops_driven, ops_driver, ops_gunner, ops_awake_once;
  uint8_t ko_count;                          /* eliminations so far, for the mini-KO vs KO decision */
  int16_t ops_punish_t; uint8_t ops_punish_target;
  float ops_tx, ops_tz, ops_carry_x, ops_carry_z;
  int stuck_frames;                          /* frames a stuck nudge was applied (the cart logs [stuck]) */
  int16_t hill_alone_t;
  pad_state_t ops_in_drive, ops_in_gun;      /* a TOURNAMENT final: the eliminated players' pads */
  uint8_t park;                              /* match_setup_t.park */
} match_t;

typedef struct {
  uint8_t mode, arena, players, comeback, fight_len_s; build_t builds[4]; uint8_t is_ai[4], ai_level[4], team[4]; uint32_t seed;
  uint8_t hazards_off;                       /* balance --robin hazards=off; the cart never sets it */
  uint8_t hitstop_off;                       /* the HIT STOP setting: the sim applies the stop itself so a replay stays aligned */
  uint8_t scheme[4];                         /* SCHEME_ARCADE or SCHEME_TANK per seat */
  uint8_t no_ops;                            /* 1 leaves OPS out (the attract, a test); the zeroed default parks it in its zone */
  uint8_t pad[4];                            /* the physical pad of each seat plus one, 0 for an AI seat; balance --replay feeds the script's pads by it */
  uint8_t park;                              /* PRACTICE=off under the rig (15.5): AI seats sit still and are never counted out */
} match_setup_t;

/* hit stop frames by band (5.1). These mirror J_STOP_* in juice.h, which the headless
 * build cannot include (the balance include check refuses audio.h); main.c static
 * asserts that the two agree, so tune juice.h and copy the value here. The sim applies
 * the stop itself on every hit so balance --replay stays aligned with the cart. */
#define STOP_TAP    0
#define STOP_HIT    2
#define STOP_HEAVY  4
#define STOP_HUGE   6
#define STOP_CLASH  8
#define STOP_KO     6
/* the band edges in damage after armour (5.1), mirrored by hit_band in juice.h: no single weapon
 * hit may take more than HIT_CAP of the victim's max HP (a flip KO is the only bigger moment), so
 * the bands sit under that cap: tap 1 to 5, hit 6 to 10, heavy 11 to 17, huge 18 and up */
#define BAND_HIT_MIN    6.0f
#define BAND_HEAVY_MIN 11.0f
#define BAND_HUGE_MIN  18.0f
#define HIT_CAP         0.20f
/* count-out and deck count frames (2.7, 6.2) */
#define COUNTOUT_FRAMES 600
#define DECK_FRAMES     600

/* -- API, the whole surface ---------------------------------------- */
void     sim_init(match_t *m, const match_setup_t *s);
void     sim_step(match_t *m);
void     sim_snapshot(const match_t *src, match_t *dst);     /* a memcpy */
int      sim_over(const match_t *m);
uint32_t sim_hash(const match_t *m);                        /* FNV-1a over x, z, h, y, hp of every bot and x, z of live debris */
/* the one trig table: linear interpolation between 1024 entries, octant reduction for atan2 */
float    rtr_sin(float a);
float    rtr_cos(float a);
float    rtr_atan2(float y, float x);
/* push an event; drops silently at MAX_EVENTS */
void     sim_event(match_t *m, const event_t *e);
/* the setup line the cart logs on sim_init and balance --replay reads back */
void     sim_setup_line(const match_setup_t *s, int start_frame, char *out, int n);
/* judges (2.5): 15 point bars per bot; returns the winner slot or -1 for a draw, and which rule applied (0 points, 1 cheaper, 2 fewer flips, 3 draw) */
int      sim_judges(const match_t *m, float damage[MAX_BOTS], float aggro[MAX_BOTS], float control[MAX_BOTS], int *rule);
/* the hazard seat (2.10): seat picks hazard k of the arena's seat_ok list, fires it */
void     sim_seat_pick(match_t *m, int seat, int dir);
void     sim_seat_fire(match_t *m, int seat);
/* the eliminated player's pad drives OPS in a TOURNAMENT final */
void     sim_ops_driven(match_t *m, int driver_seat, int gunner_seat);
/* a bot's status word (content.h SW_*), read by the strip */
int      sim_status_word(const bot_t *b);
/* the arena of a match */
const struct arena_def_s *sim_arena(const match_t *m);
/* the count-out number a bot shows (10 down to 0), or -1 when it is not counting */
int      sim_countout_number(const bot_t *b);
/* a 1v1 is any match with two bots that are not OPS; the comeback rule reads it */
int      sim_is_1v1(const match_t *m);
/* helpers shared by sim.c, ai.c and the renderer */
static inline float rtr_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float rtr_wrap_pi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

#endif
