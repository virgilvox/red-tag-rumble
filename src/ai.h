/* ai.h: perception ring, utility scorer, steering, difficulty, habits (spec
 * section 8). Drives the practice bot, FILL WITH BOT seats, OPS and the title
 * attract. Owned by the AI agent (items 12, 13): bodies in ai.c. Headless
 * safe: nothing from three.c, hud.h or wasmcart; no libm trig. Only ai.c reads
 * the fields of ai_t; everyone else treats it as opaque storage.
 *
 * Conventions ai.c shares with sim.c: heading 0 is +z, forward is (sin h, cos h),
 * the body's local +x is (cos h, -sin h) and lies on the bot's LEFT as seen on
 * screen, so a right turn (stick X positive) is a heading DECREASE; a stick
 * pushed up is a negative ly and means forward. HOVER and MELTY get a world
 * direction against cam_yaw 0 (stick up +z, stick right -x). lifted_by is the
 * lifter's slot plus one. */
#ifndef RTR_AI_H
#define RTR_AI_H
#include "sim.h"
#include "arena.h"

#define AI_RING 25        /* now plus the 24 frames before it, so EASY reads frame now - 24 */
#define AI_THINK_EVERY 6  /* 10 Hz think, 60 Hz steer */

/* goals (8.2) and the OPS goals (8.6) */
enum { GOAL_NONE, GOAL_ATTACK, GOAL_EVADE, GOAL_HAZARD_PUSH, GOAL_RECOVER, GOAL_BAIT, GOAL_PICKUP, GOAL_TAUNT, GOAL_WIGGLE,
       GOAL_OPS_GO, GOAL_OPS_LIFT, GOAL_OPS_CARRY, GOAL_OPS_RETURN, GOAL_OPS_PUNISH, GOAL_COUNT };

/* save.habits[8], the HARD AI's memory of the human across an evening (8.4) */
enum { HB_FLIPS,        /* flips the human landed on AI bots, saturating */
       HB_PINNED,       /* times the human pinned an AI bot */
       HB_FAV_WEAPON,   /* the human's last weapon id plus one, 0 unknown */
       HB_FAV_STREAK,   /* matches in a row with that weapon */
       HB_BOOSTS,       /* running average of the human's boosts per match */
       HB_HAZARD_PUSH,  /* times an AI bot was pushed into a hazard by the human */
       HB_MATCHES,      /* matches folded in */
       HB_HUMAN_WINS };

/* one bot as the AI sees it; a frame of these goes into the ring every step */
typedef struct {
  float x, z, h, vx, vz, hp, spin, y, vy;
  int16_t windup, cooldown, countout, boost;
  uint8_t weapon, drive, chassis, alive, inverted, airborne, lifted_by, is_ops, team;
} ai_view_t;
typedef struct { ai_view_t bots[MAX_BOTS]; int nbots; } ai_frame_t;

typedef struct {
  uint8_t slot, seat, difficulty, reaction;   /* reaction 24 EASY, 12 MEDIUM, 3 HARD */
  uint8_t stagger;                            /* think phase so four AIs never think on one tick */
  int goal; float goal_score;
  int target;                                 /* enemy slot, -1 none */
  float tx, tz;                               /* steering target */
  float last_err;                             /* PD on heading error */
  int stuck_t, reverse_t, corner_t, taunt_t, burst_t, think_t, wiggle_t;
  float progress_x, progress_z, approach_add;
  float corner_dx, corner_dz;                 /* the corner escape direction */
  ai_frame_t ring[AI_RING]; int ring_head, ring_filled;
  uint32_t rng;
  uint16_t prev_buttons;                      /* last frame's output, for taps */
  uint8_t hold_weapon, saw_on, boost;
  uint8_t retreat;                            /* steer_to: back away keeping the enemy ahead */
  int haz_idx;                                /* the hazard of HAZARD PUSH or BAIT, -1 none */
  float pick_x, pick_z;                       /* the pickup of PICKUP */
  int pin_t[MAX_BOTS]; uint8_t pinner[MAX_BOTS];   /* the AI side pin detector (2.11) */
  float carry_x, carry_z; uint8_t carry_set;  /* OPS carry point, 3 u toward the centre */
  int ops_target;                             /* OPS: the chosen target slot */
  const arena_def_t *arena;                   /* override for tests; NULL means ARENAS[m->arena] */
  float last_haz_taken[MAX_BOTS]; int contact_t[MAX_BOTS];   /* habit observation */
  int last_boost[MAX_BOTS];
  /* skill (8.4): EASY backs off for backoff_t frames after it lands a hit; a think may hesitate (skip a
   * tap weapon's window); EVADE has a budget: evade_t counts frames spent evading one target and
   * evade_block_t suppresses it after 120 so a spinner is attacked from the rear, not orbited forever */
  int backoff_t, evade_t, evade_block_t; float last_dealt; uint8_t hesitate;
  uint8_t vs_human;                           /* a human seat is in this match: the gentler skill table applies */
  float esc_dx, esc_dz; uint8_t esc_wall;    /* the stuck detector's escape: off the nearest wall, never backing into it */
  uint8_t seeded;                             /* the match seed is folded into rng on the first think */
  float aim_x, aim_z;                         /* this think's aim error on the attack point */
} ai_t;

void ai_init(ai_t *a, int difficulty, int seat);
/* every frame before sim_step; thinks every 6th frame, steers every frame; writes bots[slot].in */
void ai_think(ai_t *a, const match_t *m, pad_state_t *out);
/* EASY picks ROLL OUT, LAUNCH or STOMP; MEDIUM counter-picks the human's last build; HARD reads habits (8.4) */
void ai_pick_build(build_t *b, int difficulty, const build_t *human_last, const uint8_t habits[8], uint32_t *rng);
/* the same for the n-th AI seat of a garage: `others` are the builds already picked for the other AI
 * seats (FILL WITH BOT). The pick never shares a weapon and chassis pair (so never a name) with one of
 * them: the first seat picks as ai_pick_build, the next ones take the other presets in turn */
void ai_pick_build_nth(build_t *b, int difficulty, const build_t *human_last, const uint8_t habits[8], uint32_t *rng, const build_t *others, int nothers);
/* bind an ai_t to a match slot (OPS uses slot 4) */
void ai_bind(ai_t *a, int slot);
/* HARD AI memory: fold a finished match into save.habits (8.4); human_slot -1 means no human */
void ai_learn(uint8_t habits[8], const match_t *m, int human_slot);
/* tests and the balance tool: read this arena instead of ARENAS[m->arena] (NULL restores the default) */
void ai_set_arena(ai_t *a, const arena_def_t *arena);
/* the practice bot garage panel (3.12): cycles one row visibly every 18 frames for 300 frames,
 * then settles on pick. Returns 0 nothing, 1 a row changed this frame (SFX_MENU_MOVE), 2 locked this frame. */
int  ai_garage_tick(build_t *cur, const build_t *pick, int frame, int players, uint32_t *rng);
/* the title attract (8.6): two MEDIUM AIs with distinct random presets in THE BAY, DEATHMATCH,
 * seeded from the frame counter at title entry; fills the setup and inits and binds ais[0..1] */
void ai_attract_setup(match_setup_t *s, ai_t ais[2], uint32_t frame_seed);

#endif
