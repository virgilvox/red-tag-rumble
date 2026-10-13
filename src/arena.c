/* arena.c: the six arena tables (spec section 6). Owned by the arena agent
 * (items 4, 14), who adds each ASCII map as a comment on top of its table,
 * the hazards, props, tile lists and visibility tables. This stub carries the
 * sizes, names, hooks, floors, spawns and OPS zones so the camera and world
 * agents have real footprints to build against. Headless: no three.c, no HUD,
 * no host, no libm trig. */
#include "arena.h"
#include <math.h>

#define VIS_ALL { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff }
#define F(x) ((float)(x))

const arena_def_t ARENAS[6] = {
  { /* THE BAY: 24 x 16, teaching arena, one_camera_default 1 */
    .name = "THE BAY", .hook = "PIT. TWO HAMMERS.", .w = 24, .d = 16, .open_mask = 0, .floor_style = FL_VCT,
    .sun = 0xece3d3, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0xf2ab1e, .point_b = 0x5fb892,
    .sun_i = 2.0f, .hemi_i = 1.2f, .point_a_i = 6, .point_b_i = 6, .pa = { 8, 3, -6 }, .pb = { -8, 3, 6 },
    .spawn4 = { { -7.5f, -5.5f, 0 }, { 9.5f, 5.5f, 0 }, { -9.5f, 5.5f, 0 }, { 9.5f, -5.5f, 0 } },
    .spawn2 = { { -8, 0, 1.5707963f }, { 8, 0, -1.5707963f } },
    .ops_zone = { -11, -7 }, .traction_mult = 1, .vis = VIS_ALL, .one_camera_default = 1, .shared_yaw_deg = 0, .tiles_w = 40, .tiles_h = 27 },
  { /* THE LASER ROOM: 20 x 20 */
    .name = "THE LASER ROOM", .hook = "SAWS. FIRE. A LASER.", .w = 20, .d = 20, .open_mask = 0, .floor_style = FL_DARK,
    .sun = 0x9fd8c4, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0x5fb892, .point_b = 0xf2ab1e,
    .sun_i = 0.6f, .hemi_i = 1.2f, .point_a_i = 10, .point_b_i = 4, .pa = { 0, 3, 0 }, .pb = { 0, 2, -8 },
    .spawn4 = { { -5.5f, -7.5f, 0 }, { 7.5f, 7.5f, 0 }, { -7.5f, 7.5f, 0 }, { 7.5f, -7.5f, 0 } },
    .spawn2 = { { -7, 0, 1.5707963f }, { 7, 0, -1.5707963f } },
    .ops_zone = { -9, -9 }, .traction_mult = 1, .vis = VIS_ALL, .one_camera_default = 0, .shared_yaw_deg = 0, .tiles_w = 34, .tiles_h = 34 },
  { /* THE FREE PILE: 24 x 16 */
    .name = "THE FREE PILE", .hook = "A MOUND. SCREWS.", .w = 24, .d = 16, .open_mask = 0, .floor_style = FL_CONCRETE,
    .sun = 0xece3d3, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0xf2ab1e, .point_b = 0xece3d3,
    .sun_i = 2.0f, .hemi_i = 1.2f, .point_a_i = 6, .point_b_i = 4, .pa = { 0, 2, 0 }, .pb = { 8, 3, -5 },
    .spawn4 = { { -9.5f, -5.5f, 0 }, { 9.5f, 5.5f, 0 }, { -7.5f, 5.5f, 0 }, { 9.5f, -5.5f, 0 } },
    .spawn2 = { { -8, 0, 1.5707963f }, { 8, 0, -1.5707963f } },
    .ops_zone = { -11, 7 }, .traction_mult = 1, .vis = VIS_ALL, .one_camera_default = 0, .shared_yaw_deg = 0, .tiles_w = 40, .tiles_h = 27 },
  { /* THE DOCK: 20 x 16, wet */
    .name = "THE DOCK", .hook = "A DECK. RAIN. A GAP.", .w = 20, .d = 16, .open_mask = 0, .floor_style = FL_WET,
    .sun = 0xf2c57a, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0xf2ab1e, .point_b = 0x5fb892,
    .sun_i = 1.2f, .hemi_i = 1.2f, .point_a_i = 10, .point_b_i = 2, .pa = { 0, 4, -4 }, .pb = { 6, 1, 6 },
    .spawn4 = { { -7.5f, -3.5f, 0 }, { 7.5f, 5.5f, 0 }, { -5.5f, 5.5f, 0 }, { 7.5f, -3.5f, 0 } },
    .spawn2 = { { -7, 1, 1.5707963f }, { 7, 1, -1.5707963f } },
    .ops_zone = { -9, 7 }, .traction_mult = 0.85f, .vis = VIS_ALL, .one_camera_default = 0, .shared_yaw_deg = 0, .tiles_w = 34, .tiles_h = 27 },
  { /* THE SHOP: 20 x 20, benches that break */
    .name = "THE SHOP", .hook = "BENCHES THAT BREAK.", .w = 20, .d = 20, .open_mask = 0, .floor_style = FL_SHOP,
    .sun = 0xece3d3, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0xf2ab1e, .point_b = 0x5fb892,
    .sun_i = 2.0f, .hemi_i = 1.2f, .point_a_i = 6, .point_b_i = 6, .pa = { 0, 3, 0 }, .pb = { -7, 2, 7 },
    .spawn4 = { { -5.5f, -7.5f, 0 }, { 7.5f, 7.5f, 0 }, { -7.5f, 7.5f, 0 }, { 7.5f, -7.5f, 0 } },
    .spawn2 = { { -7.5f, -7.5f, 0.7853982f }, { 7.5f, 7.5f, -2.3561945f } },
    .ops_zone = { -9, -9 }, .traction_mult = 1, .vis = VIS_ALL, .one_camera_default = 0, .shared_yaw_deg = 0, .tiles_w = 34, .tiles_h = 34 },
  { /* THE ROOF: 18 x 18, open on +x and +z */
    .name = "THE ROOF", .hook = "NO RAILS. WIND.", .w = 18, .d = 18, .open_mask = OPEN_PX | OPEN_PZ, .floor_style = FL_ROOF,
    .sun = 0xf2ab1e, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0xf2ab1e, .point_b = 0x5fb892,
    .sun_i = 1.6f, .hemi_i = 1.2f, .point_a_i = 5, .point_b_i = 5, .pa = { -4, 3, -4 }, .pb = { 5, 3, 5 },
    .spawn4 = { { -6.5f, -4.5f, 0 }, { 6.5f, 6.5f, 0 }, { -6.5f, 6.5f, 0 }, { 6.5f, -6.5f, 0 } },
    .spawn2 = { { -6, -6, 0.7853982f }, { 6, 6, -2.3561945f } },
    .ops_zone = { -8, -8 }, .traction_mult = 1, .vis = VIS_ALL, .one_camera_default = 0, .shared_yaw_deg = 0, .tiles_w = 30, .tiles_h = 30 },
};

/* the garage bay: four turntables in a row at x = 0, 12, 24, 36, a pegboard back wall */
const arena_def_t GARAGE_BAY = {
  .name = "GARAGE", .hook = "", .w = 48, .d = 10, .floor_style = FL_GARAGE,
  .sun = 0xece3d3, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0xf2ab1e, .point_b = 0x5fb892,
  .sun_i = 2.0f, .hemi_i = 1.2f, .point_a_i = 6, .point_b_i = 6, .pa = { 6, 3, 2 }, .pb = { 30, 3, 2 },
  .traction_mult = 1, .vis = VIS_ALL, .tiles_w = 48, .tiles_h = 10 };

int arena_cell(const arena_def_t *a, float x, float z) {
  int cx = (int)((x + a->w * 0.5f) / a->w * 4), cz = (int)((z + a->d * 0.5f) / a->d * 4);
  if (cx < 0) cx = 0; if (cx > 3) cx = 3;
  if (cz < 0) cz = 0; if (cz > 3) cz = 3;
  return cz * 4 + cx;
}

float arena_floor_y(const arena_def_t *a, float x, float z) { (void)a; (void)x; (void)z; return 0; }

int hazard_phase(const hazard_def_t *h, float haz_clock, float *phase_t) {
  if (h->period <= 0) { if (phase_t) *phase_t = 0; return h->active_s > 0 ? 1 : 0; }
  float p = fmodf(haz_clock + h->offset, h->period);
  if (p < 0) p += h->period;
  if (phase_t) *phase_t = p;
  if (p < h->active_s) return 1;
  if (p > h->period - h->tell_s) return 2;
  return 0;
}

int arena_nearest_wall(const arena_def_t *a, float x, float z, float *nx, float *nz, float *dist) {
  float hw = a->w * 0.5f, hd = a->d * 0.5f;
  float best = 1e9f; int found = 0;
  struct { float d, nx, nz; int open; } c[4] = {
    { hw - x, -1, 0, a->open_mask & OPEN_PX }, { x + hw, 1, 0, a->open_mask & OPEN_NX },
    { hd - z, 0, -1, a->open_mask & OPEN_PZ }, { z + hd, 0, 1, a->open_mask & OPEN_NZ } };
  for (int i = 0; i < 4; i++) if (!c[i].open && c[i].d < best) { best = c[i].d; if (nx) *nx = c[i].nx; if (nz) *nz = c[i].nz; found = 1; }
  if (dist) *dist = best;
  return found;
}

int arena_seat_hazard_count(const arena_def_t *a) {
  int n = 0;
  for (int i = 0; i < a->nhaz; i++) if (a->haz[i].seat_ok) n++;
  return n;
}

int arena_seat_hazard(const arena_def_t *a, int k) {
  for (int i = 0; i < a->nhaz; i++) if (a->haz[i].seat_ok && k-- == 0) return i;
  return -1;
}
