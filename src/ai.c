/* ai.c: perception ring, utility scorer, steering, difficulty, habits (spec
 * section 8). Owned by the AI agent (items 12, 13). Stub bodies. Headless: no
 * three.c, no HUD, no host, no libm trig. */
#include "ai.h"
#include <string.h>

void ai_init(ai_t *a, int difficulty, int seat) {
  memset(a, 0, sizeof *a);
  a->difficulty = (uint8_t)difficulty;
  a->seat = (uint8_t)seat;
  a->reaction = difficulty == AI_EASY ? 24 : difficulty == AI_MEDIUM ? 12 : 3;
  a->stagger = (uint8_t)(seat % 6);
  a->rng = 0x51ed270bu ^ (uint32_t)(seat * 7919);
  a->target = -1;
}

void ai_bind(ai_t *a, int slot) { a->slot = (uint8_t)slot; }

void ai_think(ai_t *a, const match_t *m, pad_state_t *out) {
  (void)a; (void)m;
  memset(out, 0, sizeof *out);
}

void ai_pick_build(build_t *b, int difficulty, const build_t *human_last, const uint8_t habits[8], uint32_t *rng) {
  (void)human_last; (void)habits;
  static const int easy[3] = { PRE_ROLL_OUT, PRE_LAUNCH, PRE_STOMP };
  if (difficulty == AI_EASY) build_preset(b, easy[rtr_rand_n(rng, 3)]);
  else build_preset(b, PRE_LAUNCH);
  b->paint = 1; b->decal = DC_NONE;
}

void ai_learn(uint8_t habits[8], const match_t *m, int human_slot) { (void)habits; (void)m; (void)human_slot; }
