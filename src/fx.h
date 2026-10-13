/* fx.h: sparks, smoke, fire, flashes and debris visuals on wall time; the hit
 * stop freeze; the Hit Contract driver (spec section 5). Owned by the fx agent
 * (items 6, 21, 24): bodies in fx.c. Not headless: it owns instanced meshes in
 * the world scene. Everything here is fired by main.c from the event queue;
 * the sim never calls it. */
#ifndef RTR_FX_H
#define RTR_FX_H
#include "three.h"
#include "sim.h"
#include "juice.h"

/* creates the one spark mesh, the one smoke mesh and the four flash quads and adds them to world_scene() */
void fx_init(void);
void fx_reset(void);
/* an EV_HIT, EV_CLASH, EV_RAM or EV_KO with its band: sparks frozen at the contact until the hit stop ends, flash, chips */
void fx_hit(const event_t *e, int band);
/* every other event: launch dust, landing dust, wall sparks, spinner wall sparks, shed debris,
 * burst, red tag sparks, pickup cubes, hammer miss, taunt confetti, bench and bumper break */
void fx_event(const event_t *e, const match_t *m);
/* wall time; frozen is M.hitstop > 0: sparks hold, smoke and fire keep running */
void fx_update(float dt, int frozen);
/* face flash quad i toward viewport i's camera before that viewport renders */
void fx_face(int viewport);
/* smoke and fire emitters follow HP: 50 grey, 30 black plus fire */
void fx_bot_damage_state(int slot, float hp, float x, float y, float z);
/* the spark colour for an armour and red tag state (3.6, 5.3) */
uint32_t fx_spark_colour(int armour, int red_tag, uint32_t *rng);
/* the viewport white flash alpha this frame (huge and clash bands), 0 when none */
int  fx_white_alpha(int viewport);
/* counts for the perf log */
int  fx_live_sparks(void);

#endif
