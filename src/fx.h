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
/* smoke and fire emitters follow HP: 50 grey, 30 black plus fire; a BURNING bot shows the fire
 * whatever its HP (world_sync calls it per alive bot) */
void fx_bot_damage_state(int slot, float hp, int burning, float x, float y, float z);
/* a launch (graphics 4.9): seat colour trail cubes along the flier's velocity and a streak at the plate tip */
void fx_launch(int victim_slot, float x, float y, float z, float vx, float vy, float vz, uint32_t seat_colour);
/* smoke and fire by the fraction of max HP (graphics 4.5): grey from 0.40, char and fire from 0.20, from the
 * world point of the most damaged side; side is the PN_* the smoke pours out of, -1 for the lid */
void fx_bot_damage_frac(int slot, float hp_frac, int burning, float x, float y, float z, int side);
/* a lit flamethrower: fire cubes from the cone drifting forward along (dx, dz), 12 a second (graphics 3.4) */
void fx_flame(float x, float y, float z, float dx, float dz, float dt);
/* the boot compile: show one of every particle so every fx program links before play; 0 hides them */
void fx_warm(int on);
/* the spark colour for an armour and red tag state (3.6, 5.3) */
uint32_t fx_spark_colour(int armour, int red_tag, uint32_t *rng);
/* the viewport white flash alpha this frame (huge and clash bands), 0 when none */
int  fx_white_alpha(int viewport);
/* counts for the perf log */
int  fx_live_sparks(void);
/* world space bursts the menus and the garage fire (5.2): confetti squares in a colour
 * (lock in 12, taunt 4), dust puffs (part cycle 4, launch 6, landing 10) */
void fx_confetti(float x, float y, float z, int n, uint32_t colour);
void fx_dust(float x, float y, float z, int n);
/* plain sparks in a colour from a point with a direction (nx, nz) to fly away from */
void fx_sparks(float x, float y, float z, float nx, float nz, int n, uint32_t colour);

#endif
