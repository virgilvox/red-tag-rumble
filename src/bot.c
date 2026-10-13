/* bot.c: build_t to body_t and stats, the hint picker (spec 14.1). Owned by
 * the sim agent (item 3) and the garage agent (bot_hint, item 11). Stub bodies;
 * the body setup is real because the demo and the balance tool link it.
 * Headless: no three.c, no HUD, no host, no libm trig. */
#include "bot.h"
#include <string.h>
#include <stdio.h>

void bot_body_from_build(body_t *b, const build_t *build) {
  const chassis_def_t *c = &CHASSIS[build->chassis];
  b->shape = c->shape;
  b->hl = c->len * 0.5f;
  b->hw = c->wid * 0.5f;
  b->mass = build_mass(build);
  b->inv_mass = b->mass > 0 ? 1.0f / b->mass : 0;
  b->inertia = c->shape == SH_CIRCLE ? b->mass * b->hl * b->hl / 2 : b->mass * (b->hl * b->hl + b->hw * b->hw) / 3;
  b->inv_inertia = b->inertia > 0 ? 1.0f / b->inertia : 0;
}

float bot_hp_max(const build_t *build) { return (float)CHASSIS[build->chassis].hp; }

int bot_hint(const bot_t *b, const match_t *m, char out[48]) {
  (void)b; (void)m;
  out[0] = 0;
  return 0;
}

void bot_setup(bot_t *b, const build_t *build, int seat, int is_ai, int team, float x, float z, float heading) {
  memset(b, 0, sizeof *b);
  b->build = *build;
  b->seat = (uint8_t)seat; b->is_ai = (uint8_t)is_ai; b->team = (uint8_t)team; b->alive = 1;
  b->preset = (uint8_t)build_preset_of(build);
  bot_name(b->name, build);
  bot_body_from_build(&b->b, build);
  b->b.x = b->b.px = x; b->b.z = b->b.pz = z; b->b.h = b->b.ph = heading;
  b->hp = b->hp_max = bot_hp_max(build);
  b->fuel = WP_FLAME_FUEL;
}

void bot_setup_ops(bot_t *b, float x, float z, float heading, int driven) {
  build_t ops = { CH_LUNCHBOX, DR_TREAD, WP_LIFTER, WP_SAW, AR_STEEL, 1, DC_TAPE_BAND };
  bot_setup(b, &ops, OPS_SLOT, 1, 255, x, z, heading);
  snprintf(b->name, sizeof b->name, "%s", S_OPS);
  b->is_ops = 1;
  b->b.hl *= 1.4f;
  b->b.inv_mass = 0; b->b.inv_inertia = 0;
  b->hp = b->hp_max = driven ? 400 : 300;
}

float bot_top_speed(const bot_t *b) {
  float top = DRIVES[b->build.drive].top;
  if (b->build.chassis == CH_FRAME) top *= 1.1f;
  if (b->boost > 0) top *= 1.5f;
  if (b->red_tag_t > 0) top *= 1.15f;
  if (b->wheel_hit > 0) top *= 0.6f;
  if (b->inverted) top *= 0.7f;
  return top;
}

int bot_can_drive(const bot_t *b) {
  return b->alive && b->b.y <= 0.001f && !b->lifted_by && !b->clamped_by && b->stun <= 0 && b->brake <= 0 && b->lock <= 0 && b->selfright <= 0;
}

int bot_dent_pattern(int panel, int state, dent_t out[4]) {
  (void)panel; (void)state; (void)out;
  return 0;
}
