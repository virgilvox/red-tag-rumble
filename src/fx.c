/* fx.c: sparks, smoke, fire, flashes, debris visuals, the hit stop freeze, the
 * Hit Contract driver (spec section 5). Owned by the fx agent (items 6, 21,
 * 24). Stub bodies. */
#include "fx.h"
#include "world.h"
#include "hud.h"

void fx_init(void) {}
void fx_reset(void) {}
void fx_hit(const event_t *e, int band) { (void)e; (void)band; }
void fx_event(const event_t *e, const match_t *m) { (void)e; (void)m; }
void fx_update(float dt, int frozen) { (void)dt; (void)frozen; }
void fx_face(int viewport) { (void)viewport; }
void fx_bot_damage_state(int slot, float hp, float x, float y, float z) { (void)slot; (void)hp; (void)x; (void)y; (void)z; }
uint32_t fx_spark_colour(int armour, int red_tag, uint32_t *rng) {
  if (red_tag) return C_RUST;
  switch (armour) {
    case AR_PLY: return C_BONE;
    case AR_ALU: case AR_UHMW: return C_WHITE;
    case AR_STEEL: return (rtr_rand(rng) & 1) ? C_HAZARD : C_RUST;
    case AR_TI: { uint32_t c[3] = { C_BONE, C_LIVE, C_HAZARD }; return c[rtr_rand_n(rng, 3)]; }
    case AR_WEAR: return C_RUST;
    default: return C_HAZARD;
  }
}
int fx_white_alpha(int viewport) { (void)viewport; return 0; }
int fx_live_sparks(void) { return 0; }
