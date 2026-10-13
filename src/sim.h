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
} bot_t;

typedef struct { float x, z, y, vx, vz, vy, spin; int16_t life; uint8_t kind, colour, pickup, owner; } debris_t;

typedef enum { EV_HIT, EV_CLASH, EV_WALL, EV_RAM, EV_LAUNCH, EV_LAND, EV_INVERT, EV_SELFRIGHT, EV_SHED, EV_BURST,
               EV_KO, EV_MINI_KO, EV_COUNT_TICK, EV_HAZ_TELL, EV_HAZ_HIT, EV_OPS_WAKE, EV_OPS_LEAVE, EV_OPS_LINE,
               EV_WEAPON_TELL, EV_TAUNT, EV_BOOST, EV_HILL_MOVE, EV_RED_TAG, EV_PICKUP, EV_NOT_VACUUM,
               EV_BENCH_BREAK, EV_BUMPER_BREAK, EV_SPIN_WALL, EV_CLOSING, EV_TYPE_COUNT } ev_type_t;
/* a is the attacker or subject bot, b the victim or the hazard index; zone is the
 * hit zone times 10 for hits, the panel for EV_SHED, the KO cause for EV_KO */
typedef struct { uint8_t type, a, b, zone; float x, z, y, dmg, nx, nz; } event_t;

typedef struct { uint8_t type; float phase_t; uint8_t active, tell, broken; int16_t hp, cooldown; uint8_t seat; } hazard_state_t;

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
} match_t;

typedef struct { uint8_t mode, arena, players, comeback, fight_len_s; build_t builds[4]; uint8_t is_ai[4], ai_level[4], team[4]; uint32_t seed; } match_setup_t;

/* ── API, the whole surface ──────────────────────────────────────── */
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
/* helpers shared by sim.c, ai.c and the renderer */
static inline float rtr_clamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float rtr_wrap_pi(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

#endif
