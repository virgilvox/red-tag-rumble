/* ai.h: perception ring, utility scorer, steering, difficulty, habits (spec
 * section 8). Drives the practice bot, FILL WITH BOT seats, OPS and the title
 * attract. Owned by the AI agent (items 12, 13): bodies in ai.c. Headless
 * safe: nothing from three.c, hud.h or wasmcart; no libm trig. Only ai.c reads
 * the fields of ai_t; everyone else treats it as opaque storage. */
#ifndef RTR_AI_H
#define RTR_AI_H
#include "sim.h"
#include "arena.h"

#define AI_RING 24

/* goals (8.2) and the OPS goals (8.6) */
enum { GOAL_NONE, GOAL_ATTACK, GOAL_EVADE, GOAL_HAZARD_PUSH, GOAL_RECOVER, GOAL_BAIT, GOAL_PICKUP, GOAL_TAUNT, GOAL_WIGGLE,
       GOAL_OPS_GO, GOAL_OPS_LIFT, GOAL_OPS_CARRY, GOAL_OPS_RETURN, GOAL_OPS_PUNISH, GOAL_COUNT };

typedef struct { float x, z, h, vx, vz, hp, spin; uint8_t weapon, alive, inverted, airborne; } ai_view_t;
typedef struct { ai_view_t bots[MAX_BOTS]; } ai_frame_t;

typedef struct {
  uint8_t slot, seat, difficulty, reaction;   /* reaction 24 EASY, 12 MEDIUM, 3 HARD */
  uint8_t stagger;                            /* think phase so four AIs never think on one tick */
  int goal; float goal_score;
  int target;                                 /* enemy slot */
  float tx, tz;                               /* steering target */
  float last_err;                             /* PD on heading error */
  int stuck_t, reverse_t, corner_t, taunt_t, burst_t, think_t;
  float progress_x, progress_z, approach_add;
  ai_frame_t ring[AI_RING]; int ring_head;
  uint32_t rng;
  uint8_t hold_weapon, saw_on, boost;
} ai_t;

void ai_init(ai_t *a, int difficulty, int seat);
/* every frame before sim_step; thinks every 6th frame, steers every frame; writes bots[slot].in */
void ai_think(ai_t *a, const match_t *m, pad_state_t *out);
/* EASY picks ROLL OUT, LAUNCH or STOMP; MEDIUM counter-picks the human's last build; HARD reads habits (8.4) */
void ai_pick_build(build_t *b, int difficulty, const build_t *human_last, const uint8_t habits[8], uint32_t *rng);
/* bind an ai_t to a match slot (OPS uses slot 4) */
void ai_bind(ai_t *a, int slot);
/* HARD AI memory: fold a finished match into save.habits (8.4) */
void ai_learn(uint8_t habits[8], const match_t *m, int human_slot);

#endif
