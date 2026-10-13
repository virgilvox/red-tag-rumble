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
/* interpolate bot roots by alpha, swap panel geometry, pose weapons, hazards, debris, blobs */
void world_sync(const match_t *m, float alpha);
/* build (or rebuild) the meshes of bot slot (0..4) from a build; the seat colour is the band */
void world_bot_build(int slot, const build_t *b, uint32_t seat_colour);
void world_bot_visible(int slot, int on);
/* pose a bot outside a match: garage turntables, lobby cards, the results turntable */
void world_bot_pose(int slot, float x, float y, float z, float heading, int inverted);
/* the turntables of the garage bay sit at x = 0, 12, 24, 36 (3.1); angle in radians */
void world_turntable(int table, float angle);
void world_turntable_pos(int table, float *x, float *y, float *z);
/* RT in the garage: the weapon animates on the turntable, purely cosmetic */
void world_weapon_test_fire(int slot);
/* per viewport prop culling from vis[] (6.5) */
void world_set_cell_visibility(int cell, const arena_def_t *a);
/* the painted floor (6.4): w x h RGBA into rgba, also used for the vote thumbnails */
void paint_floor(const arena_def_t *a, int w, int h, uint8_t *rgba);
/* scorch decals: repaint at most every 30 frames in a fight */
void world_scorch(float x, float z, float r);
/* the lexan L in front of the OPS zone, 0 closed .. 1 open */
void world_ops_gate(float open);
/* CLOSING TIME: wall rail emissive goes rust */
void world_closing(int on);
/* hazard tells and poses are read from the match in world_sync; a broken prop is hidden */
/* compile every program for a scene with a camera during PH_BOOT */
void world_compile_all(void);
/* the crowd quads' frame (0..2) and tint */
void world_crowd(int frame, float level);

#endif
