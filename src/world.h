/* world.h: the three.c side. Scenes, arenas, the garage, bots from primitives,
 * debris, blobs, lights, textures, paint_floor, world_sync and the per-frame
 * viewport loop (spec 14.2, render.h). Owned by the world agent (items 5, 8,
 * 13, 14, 15, 21): bodies in world.c. Not headless. */
#ifndef RTR_WORLD_H
#define RTR_WORLD_H
#include "three.h"
#include "sim.h"
#include "arena.h"

void world_init(void);
/* the one scene: arena_root[6] and garage_root are attached and detached under it */
t3_scene *world_scene(void);
void world_attach_arena(int arena);
void world_attach_garage(void);
void world_detach_all(void);
/* wall time animation: turntables, tread scroll, LEDs, crowd, hazard idle motion.
 * main.c calls it once per render frame before world_sync; when it is not called,
 * world_sync assumes a 60 Hz frame. */
void world_tick(float dt);
/* interpolate bot roots by alpha, swap panel geometry, pose weapons, hazards, debris, blobs */
void world_sync(const match_t *m, float alpha);
/* build (or rebuild) the meshes of bot slot (0..4) from a build; the seat colour is the band */
void world_bot_build(int slot, const build_t *b, uint32_t seat_colour);
/* the OPS house robot look (section 7) in slot (normally OPS_SLOT) */
void world_bot_build_ops(int slot);
void world_bot_visible(int slot, int on);
/* pose a bot outside a match: garage turntables, lobby cards, the results turntable */
void world_bot_pose(int slot, float x, float y, float z, float heading, int inverted);
/* the turntables of the garage bay sit at x = 0, 12, 24, 36 (3.1); angle in radians.
 * Setting an angle (the right stick scrub) pauses the 20 deg/s auto rotation for half a second */
void world_turntable(int table, float angle);
float world_turntable_angle(int table);
void world_turntable_pos(int table, float *x, float *y, float *z);
/* the turntable stops and the bot faces the camera (lock in, 5.2); 0 resumes */
void world_turntable_lock(int table, int locked);
/* RT in the garage: the weapon animates on the turntable, purely cosmetic */
void world_weapon_test_fire(int slot);
/* per viewport prop culling from vis[] (6.5); cell -1 shows everything */
void world_set_cell_visibility(int cell, const arena_def_t *a);
/* THE SHOP: a bench broke, every cell sees every prop from now on */
void world_vis_all(void);
/* the painted floor (6.4): w x h RGBA into rgba, row 0 the back wall (z = -d/2);
 * also used for the vote thumbnails at 352 x 120 */
void paint_floor(const arena_def_t *a, int w, int h, uint8_t *rgba);
/* scorch decals on the attached arena floor: repainted at most every 30 frames in a fight */
void world_scorch(float x, float z, float r);
/* the lexan L in front of the OPS zone, 0 closed .. 1 open */
void world_ops_gate(float open);
/* CLOSING TIME: wall rail emissive goes rust */
void world_closing(int on);
/* hazard tells and poses are read from the match in world_sync; a broken prop is hidden */
/* compile every program for every scene with a camera during PH_BOOT */
void world_compile_all(void);
/* the same one scene at a time (the six arenas, then the garage) so the boot screen can take one
 * per frame: compiles and renders scene i dressed with bots and particles, and returns the scene count */
int  world_compile_one(int i);
/* the crowd quads' frame (0..2) and tint level 0..1 */
void world_crowd(int frame, float level);
/* the world position of a bot root this frame (after world_sync), for the HUD and the camera */
int  world_bot_pos(int slot, float *x, float *y, float *z);
/* build one arena root from a definition (world_init does this for ARENAS[0..5]) */
void world_build_arena(int idx, const arena_def_t *def);
/* a heavy or huge hit landed on slot at the world point (x, y, z) from the direction (nx, nz) (attacker to
 * victim): a scratch decal on the panel that took it, the weapon's strike hold and the hanging plate kick
 * (graphics 3.4, 4.1, 4.3); weapon is the event's WP_* or EVW_*, panel its PN_* */
void world_bot_hit(int slot, float x, float y, float z, float nx, float nz, int weapon, int panel, int band);
/* a decal on bot slot in its local frame: kind DECAL_*, the point on the panel plane and its normal, size in u */
void world_bot_decal(int slot, int kind, float lx, float ly, float lz, float nx, float ny, float nz, float size);
enum { DECAL_SCORCH, DECAL_SCRATCH, DECAL_BOLT_HOLE, DECAL_CRACK, DECAL_KINDS };
/* the garage's amber fill light in front of turntable table (render_frame calls it per viewport) */
void world_garage_light(int table);
/* the draw counters of the last viewport render_frame drew and the scene's object count, for the [draw] log */
int  world_draw_stats(unsigned *calls, unsigned *tris);
int  world_object_count(void);
/* the attached arena definition, or NULL in the garage */
const arena_def_t *world_arena(void);

#endif
