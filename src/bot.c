/* bot.c: build_t to body_t and stats, the drive quirks the sim reads every
 * step, the OPS body, and the headless side of dent geometry (spec 14.1).
 * Owned by the sim agent (item 3); bot_hint belongs to the garage agent (item
 * 11) and is left as its stub. Headless: no three.c, no HUD, no host, no libm
 * trig. */
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
  /* inertia is mass * (hl*hl + hw*hw) / 3 for a box and mass * r*r / 2 for a circle (4.5) */
  b->inertia = c->shape == SH_CIRCLE ? b->mass * b->hl * b->hl / 2 : b->mass * (b->hl * b->hl + b->hw * b->hw) / 3;
  b->inv_inertia = b->inertia > 0 ? 1.0f / b->inertia : 0;
  /* the height of the body (DEEP-plan 2.1): it occupies [y + clr, y + clr + hgt]; pitch inertia about local x */
  b->clr = DRIVE_CLEAR[build->drive < DR_COUNT ? build->drive : 0];
  b->hgt = c->hgt;
  b->inertia_p = b->mass * (b->hl * b->hl + b->hgt * b->hgt * 0.25f) / 3;
  b->inv_inertia_p = b->inertia_p > 0 ? 1.0f / b->inertia_p : 0;
  /* the static lips; LIFTER forks are a lip only while lowered, which the sim updates every step */
  int wk = bot_wedge_kind(build, 0);
  b->lip_f = wk >= 0 ? WEDGE_KIND[wk].lip : 0;
  b->lip_b = 0;
}

/* the wedge on a build's front, the lowest lip wins: the WEDGE chassis, a WEDGE plate, lowered LIFTER forks,
 * the LOWBOY nose; WK_NONE when the front is a plain face (MELTY has skids, not a lip) */
int bot_wedge_kind(const build_t *build, float lift) {
  if (build->drive == DR_MELTY) return WK_NONE;
  if (build->chassis == CH_WEDGE) return WK_CHASSIS;
  if (build->weapon == WP_WEDGE) return WK_PLATE;
  if (build->weapon == WP_LIFTER && lift < 0.3f) return WK_FORKS;
  if (build->chassis == CH_LOWBOY) return WK_NOSE;
  return WK_NONE;
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
  b->fuel = WP_FLAME_FUEL; b->fuel2 = WP_FLAME_FUEL;
  /* parts fly from 50 percent HP on FRAME or AR_NONE, else from 20 (4.9) */
  b->shed_next = (uint8_t)(build->chassis == CH_FRAME ? 50 : build->armour == AR_NONE ? 50 : 20);
  b->still_x = x; b->still_z = z;
  b->load = 1.0f;                            /* all wheels on the floor (DEEP: the ride takes it away) */
  for (int p = 0; p < PN_COUNT; p++) b->panel_hp[p] = ARMOUR_PANEL_HP[build->armour < AR_COUNT ? build->armour : 0];   /* the plates (3.10) */
}

void bot_setup_ops(bot_t *b, float x, float z, float heading, int driven) {
  build_t ops = { CH_LUNCHBOX, DR_TREAD, WP_LIFTER, WP_SAW, AR_STEEL, 1, DC_TAPE_BAND };
  bot_setup(b, &ops, OPS_SLOT, 1, 255, x, z, heading);
  snprintf(b->name, sizeof b->name, "%s", S_OPS);
  b->is_ops = 1;
  b->b.hl *= 1.4f;
  b->b.mass = 6.0f;                          /* what the collision code uses against walls */
  b->b.inv_mass = 0; b->b.inv_inertia = 0;   /* immune to knockback and push (7) */
  b->b.inv_inertia_p = 0;
  b->hp = b->hp_max = driven ? 400 : 300;
  b->shed_next = 0;                          /* OPS never sheds and is never KO'd */
}

float bot_top_speed(const bot_t *b) {
  if (b->is_ops) return 3.2f;
  float top = DRIVES[b->build.drive].top;
  if (b->build.chassis == CH_FRAME) top *= 1.1f;
  if (b->build.drive == DR_FOUR && b->hp <= 30) top *= 0.8f;    /* lost a wheel */
  if (b->boost > 0) top *= 1.5f;
  if (b->red_tag_t > 0) top *= 1.15f;
  if (b->wheel_hit > 0) top *= 0.6f;
  if (b->inverted) top *= 0.7f;
  return top;
}

int bot_can_drive(const bot_t *b) {
  if (!b->alive || b->airborne || b->falling || b->pit_state == 2) return 0;
  if (b->lifted_by || b->clamped_by || b->stun > 0 || b->brake > 0 || b->lock > 0 || b->selfright > 0) return 0;
  if (b->inverted && !b->is_ops) {
    /* WEDGE, NONE, DRUM, SAW, SPIKE drive inverted at 70 percent; the rest cannot (3.5) */
    if (b->build.drive == DR_HOVER || b->build.drive == DR_MELTY) return 1;
    if (!WEAPONS[b->build.weapon].inv_drive) return 0;
  }
  return 1;
}

/* 2 to 4 vertex clusters per panel pushed in by 0.02, 0.05 and 0.09, seeded per panel (14.2) */
int bot_dent_pattern(int panel, int state, dent_t out[4]) {
  if (state < 1 || state > 3) return 0;
  static const float DEPTH[4] = { 0, 0.02f, 0.05f, 0.09f };
  uint32_t s = 0x9e3779b9u ^ (uint32_t)(panel * 7919 + 17);
  int n = 2 + (int)(rtr_rand(&s) % 3u);
  for (int i = 0; i < n; i++) {
    out[i].cx = rtr_rand_f(&s) * 1.6f - 0.8f;
    out[i].cy = rtr_rand_f(&s) * 1.6f - 0.8f;
    out[i].cz = 0;
    out[i].r = 0.18f + rtr_rand_f(&s) * 0.22f;
    out[i].depth = DEPTH[state];
  }
  return n;
}
