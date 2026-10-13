/* arena.h: the six arena tables (spec section 6), data only. Owned by the
 * arena agent (items 4, 14): bodies in arena.c, each arena_def_t with its
 * ASCII map as a comment on top. Headless safe: nothing from three.c, hud.h
 * or wasmcart; no libm trig. */
#ifndef RTR_ARENA_H
#define RTR_ARENA_H
#include <stdint.h>
#include "sim.h"

#define ARENA_COUNT 6
enum { AR_BAY, AR_LASER, AR_PILE, AR_DOCK, AR_SHOP, AR_ROOF };

/* hazard types (6.2) */
enum { HZ_NONE, HZ_PIT, HZ_PULVERIZER, HZ_KILLSAW, HZ_FLAME, HZ_SCREW, HZ_RAMP, HZ_BUMPER, HZ_DECK, HZ_FLOORFLIP,
       HZ_DROPZONE, HZ_LASER, HZ_WIND, HZ_HILL, HZ_MOUND, HZ_TYPE_COUNT };
/* prop kinds (6.1) */
enum { PR_WALL, PR_BENCH, PR_HVAC, PR_BOX, PR_SHELF, PR_SIGN, PR_SERVER, PR_GATE, PR_KIND_COUNT };
/* floor styles (6.4) */
enum { FL_VCT, FL_DARK, FL_CONCRETE, FL_WET, FL_SHOP, FL_ROOF, FL_GARAGE };
/* open_mask bits */
enum { OPEN_PX = 1, OPEN_NX = 2, OPEN_PZ = 4, OPEN_NZ = 8 };
/* tile colours */
enum { TILE_ORANGE, TILE_RED };

typedef struct { uint8_t type; float x, z, w, d, ang, h; float period, offset, active_s, tell_s; float dmg; int16_t p0, p1; uint8_t seat_ok; } hazard_def_t;
typedef struct { uint8_t kind; float x, z, w, d, h, ang; uint32_t colour; int16_t hp; } prop_def_t;   /* kind: PR_WALL, PR_BENCH, PR_HVAC, PR_BOX, PR_SHELF, PR_SIGN, PR_SERVER */
typedef struct { uint8_t x, y, w, h, colour; } tile_rect_t;                                        /* colour: 0 orange, 1 red */
typedef struct {
  const char *name, *hook;                 /* hook is ARENA_BLURB[i], under 24 chars */
  float w, d;                              /* full size; walls at +-w/2, +-d/2 */
  uint8_t open_mask;                       /* bit 0 +x, 1 -x, 2 +z, 3 -z: open edges (THE ROOF) */
  uint8_t floor_style;                     /* FL_VCT, FL_DARK, FL_CONCRETE, FL_WET, FL_SHOP, FL_ROOF */
  const tile_rect_t *tiles; int ntiles;
  uint32_t sun, hemi_sky, hemi_ground, point_a, point_b; float sun_i, hemi_i, point_a_i, point_b_i;
  float pa[3], pb[3];                      /* point light positions */
  hazard_def_t haz[MAX_HAZ]; int nhaz;
  prop_def_t props[32]; int nprops;
  float spawn4[4][3], spawn2[2][3];        /* x z heading per seat, 3 or 4 players; 1v1 */
  float ops_zone[2];                       /* centre of the 2 x 2 Corner Patrol Zone */
  float traction_mult;                     /* 0.85 in THE DOCK, else 1 */
  uint16_t vis[16];                        /* 4 x 4 cells, bit j set if cell j is visible from cell i */
  uint8_t one_camera_default, shared_yaw_deg;
  uint8_t tiles_w, tiles_h;                /* the tile grid the tile_rect_t list indexes (6.4) */
  float hill[4][2];                        /* KING OF THE HILL points, in order */
} arena_def_t;

extern const arena_def_t ARENAS[6];
/* the garage bay floor (6.4): 48 x 10 tiles, FL_GARAGE */
extern const arena_def_t GARAGE_BAY;

int   arena_cell(const arena_def_t *a, float x, float z);                 /* 0..15 on the 4 x 4 grid */
float arena_floor_y(const arena_def_t *a, float x, float z);              /* 0, 1.0 on the deck, the mound height */
/* phase_t = fmod(haz_clock + offset, period); returns 1 active, 2 tell, 0 idle */
int   hazard_phase(const hazard_def_t *h, float haz_clock, float *phase_t);
/* the inward normal and distance of the nearest wall to (x, z); open edges return 0 */
int   arena_nearest_wall(const arena_def_t *a, float x, float z, float *nx, float *nz, float *dist);
/* the k-th seat-drivable hazard index, or -1 */
int   arena_seat_hazard(const arena_def_t *a, int k);
int   arena_seat_hazard_count(const arena_def_t *a);

#endif
