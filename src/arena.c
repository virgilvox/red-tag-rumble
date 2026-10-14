/* arena.c: the six arena tables (spec section 6), data only, plus the small
 * terrain and timing helpers the sim and the renderer share. Owned by the arena
 * side of the sim agent (items 4, 14). Each arena_def_t carries its ASCII map as
 * a comment on top; the legend is in 6.3. Headless: no three.c, no HUD, no host,
 * no libm trig (sqrtf and fmodf only).
 *
 * Map legend: # wall, : open edge, P pulverizer, K killsaw, F flame grate,
 * S screw, R ramp or lip, B bumper post, D drop zone, X floor flipper, O OPS
 * zone, ^ pit, = upper deck, ~ bench, * mound boxes, H HVAC, . floor.
 * Row 0 is the back wall (z = minus d/2), column 0 the left wall (x = minus w/2).
 *
 * Hazard timing runs on M.haz_clock (4.4): phase_t = fmod(haz_clock + offset,
 * period); active while phase_t < active_s; tell while phase_t > period - tell_s.
 * The pits open first at T minus 60 of a 75 s fight (haz_clock 15), so their
 * offset is period 20 minus 15. */
#include "arena.h"
#include <math.h>

#define VIS_ALL { 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff }
#define PI_F 3.14159265f
#define HALF_PI_F 1.5707963f
/* hazard_def_t: type x z w d ang h period offset active tell dmg p0 p1 seat_ok */
#define HZ(type, x, z, w, d, ang, h, period, offset, active, tell, dmg, p0, p1, seat) { type, x, z, w, d, ang, h, period, offset, active, tell, dmg, p0, p1, seat }
/* prop_def_t: kind x z w d h ang colour hp */
#define PR(kind, x, z, w, d, h, ang, colour, hp) { kind, x, z, w, d, h, ang, colour, hp }

/* -- painted floor tile lists (6.4), tile indices from the back-left tile -- */
static const tile_rect_t TILES_BAY[] = {
  { 6, 4, 4, 2, TILE_ORANGE }, { 20, 3, 1, 8, TILE_RED }, { 30, 18, 3, 3, TILE_RED }, { 12, 22, 6, 1, TILE_ORANGE }, { 34, 5, 2, 2, TILE_ORANGE },
  /* the red ring round the pit tape */
  { 18, 11, 4, 1, TILE_RED }, { 18, 15, 4, 1, TILE_RED }, { 18, 11, 1, 5, TILE_RED }, { 21, 11, 1, 5, TILE_RED } };
static const tile_rect_t TILES_LASER[] = {
  { 13, 13, 8, 8, TILE_RED },                                                             /* under the bed */
  { 10, 11, 2, 1, TILE_ORANGE }, { 22, 11, 2, 1, TILE_ORANGE }, { 10, 22, 2, 1, TILE_ORANGE }, { 22, 22, 2, 1, TILE_ORANGE } };
static const tile_rect_t TILES_PILE[] = {
  { 3, 3, 3, 3, TILE_ORANGE }, { 33, 2, 3, 3, TILE_ORANGE }, { 5, 20, 3, 3, TILE_ORANGE }, { 31, 21, 3, 3, TILE_ORANGE }, { 14, 2, 3, 3, TILE_ORANGE }, { 26, 23, 3, 3, TILE_ORANGE },
  /* a red one tile band round the mound footprint */
  { 12, 8, 16, 1, TILE_RED }, { 12, 19, 16, 1, TILE_RED }, { 12, 8, 1, 12, TILE_RED }, { 27, 8, 1, 12, TILE_RED } };
static const tile_rect_t TILES_DOCK[] = {
  { 0, 5, 34, 2, TILE_RED },                                                              /* the deck edge band */
  { 5, 10, 2, 2, TILE_ORANGE }, { 27, 10, 2, 2, TILE_ORANGE } };                          /* the ramp feet */
static const tile_rect_t TILES_SHOP[] = {
  { 3, 16, 11, 1, TILE_ORANGE }, { 20, 16, 11, 1, TILE_ORANGE }, { 16, 3, 1, 11, TILE_ORANGE }, { 16, 20, 1, 11, TILE_ORANGE },   /* under the benches */
  { 16, 0, 2, 2, TILE_RED }, { 16, 32, 2, 2, TILE_RED }, { 0, 16, 2, 2, TILE_RED }, { 32, 16, 2, 2, TILE_RED } };                  /* under each pulverizer */
static const tile_rect_t TILES_GARAGE[] = {
  { 4, 2, 3, 2, TILE_ORANGE }, { 14, 6, 1, 4, TILE_RED }, { 24, 1, 6, 1, TILE_ORANGE }, { 36, 3, 2, 2, TILE_RED }, { 42, 7, 4, 1, TILE_ORANGE } };

const arena_def_t ARENAS[6] = {
  /* -- 6.3.1 THE BAY, 24 x 16, the teaching arena, one_camera_default 1 --
   * ##########################
   * #OO.........PP...........#
   * #OO......................#
   * #........................#
   * #........................#
   * #........................#
   * #........................#
   * #........................#
   * #...........^^...........#
   * #...........^^...........#
   * #........................#
   * #........................#
   * #........................#
   * #........................#
   * #........................#
   * #........................#
   * #...........PP...........#
   * ##########################
   */
  { .name = "THE BAY", .hook = "PIT. TWO HAMMERS.", .w = 24, .d = 16, .open_mask = 0, .floor_style = FL_VCT,
    .tiles = TILES_BAY, .ntiles = (int)(sizeof TILES_BAY / sizeof TILES_BAY[0]),
    .sun = 0xece3d3, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0xf2ab1e, .point_b = 0x5fb892,
    .sun_i = 2.0f, .hemi_i = 1.2f, .point_a_i = 6, .point_b_i = 6, .pa = { 8, 3, -6 }, .pb = { -8, 3, 6 },
    .haz = {
      HZ(HZ_PULVERIZER, 0, -7.4f, 1.2f, 1.2f, 0, 1.6f,  0, 0, 0, 1.5f, 20, 150, 72, 1),
      HZ(HZ_PULVERIZER, 0,  7.4f, 1.2f, 1.2f, PI_F, 1.6f, 0, 0, 0, 1.5f, 20, 150, 72, 1),
      HZ(HZ_PIT,        0,  0,    2.0f, 2.0f, 0, -2.0f, 20, 5, 6, 1.5f, 0, 0, 0, 0) },
    .nhaz = 3,
    /* props stand outside the lexan (two shelving units and a sign board behind the back wall) */
    .props = {
      PR(PR_SHELF, -6, -9.4f, 4.0f, 1.0f, 2.0f, 0, 0x4a4338, 0),
      PR(PR_SHELF,  6, -9.4f, 4.0f, 1.0f, 2.0f, 0, 0x4a4338, 0),
      PR(PR_SIGN,   0, -9.8f, 6.0f, 0.2f, 1.5f, 0, 0xf2ab1e, 0) },
    .nprops = 3,
    .spawn4 = { { -7.5f, -5.5f, 0.7853982f }, { 9.5f, 5.5f, -2.3561945f }, { -9.5f, 5.5f, -0.7853982f }, { 9.5f, -5.5f, 2.3561945f } },
    .spawn2 = { { -8, 0, HALF_PI_F }, { 8, 0, -HALF_PI_F } },
    .ops_zone = { -11, -7 }, .traction_mult = 1, .vis = VIS_ALL, .one_camera_default = 1, .shared_yaw_deg = 0, .tiles_w = 40, .tiles_h = 27,
    .hill = { { -6, -3 }, { 6, -3 }, { 6, 3 }, { -6, 3 } } },

  /* -- 6.3.2 THE LASER ROOM, 20 x 20 --
   * ######################
   * #OO....F...F...F.....#
   * #OO..................#
   * #....................#
   * #....................#
   * #....................#
   * #....................#
   * #......KK....KK......#
   * #....................#
   * #........RRRR........#
   * #........RRRR........#
   * #........RRRR........#
   * #........RRRR........#
   * #....................#
   * #......KK....KK......#
   * #....................#
   * #....................#
   * #....................#
   * #....................#
   * #....................#
   * #....................#
   * ######################
   */
  { .name = "THE LASER ROOM", .hook = "SAWS. FIRE. A LASER.", .w = 20, .d = 20, .open_mask = 0, .floor_style = FL_DARK,
    .tiles = TILES_LASER, .ntiles = (int)(sizeof TILES_LASER / sizeof TILES_LASER[0]),
    .sun = 0x9fd8c4, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0x5fb892, .point_b = 0xf2ab1e,
    .sun_i = 0.6f, .hemi_i = 1.2f, .point_a_i = 10, .point_b_i = 4, .pa = { 0, 3, 0 }, .pb = { 0, 2, -8 },
    .haz = {
      HZ(HZ_KILLSAW, -3, -3, 1.6f, 0.4f, 0, 0.3f, 7, 0.00f, 1.5f, 1.5f, 6, 0, 0, 0),
      HZ(HZ_KILLSAW,  3, -3, 1.6f, 0.4f, 0, 0.3f, 7, 1.75f, 1.5f, 1.5f, 6, 0, 0, 0),
      HZ(HZ_KILLSAW, -3,  3, 1.6f, 0.4f, 0, 0.3f, 7, 3.50f, 1.5f, 1.5f, 6, 0, 0, 0),
      HZ(HZ_KILLSAW,  3,  3, 1.6f, 0.4f, 0, 0.3f, 7, 5.25f, 1.5f, 1.5f, 6, 0, 0, 0),
      HZ(HZ_RAMP,     0,  0, 4.0f, 4.0f, 0, 0.2f, 0, 0, 0, 0, 0, 0, 0, 0),                   /* the honeycomb bed, a lip */
      HZ(HZ_FLAME,   -4, -9.5f, 1.0f, 1.0f, 0, 0.1f, 8, 0,    2, 1.5f, 3, 0, 0, 1),
      HZ(HZ_FLAME,    0, -9.5f, 1.0f, 1.0f, 0, 0.1f, 8, 2.67f, 2, 1.5f, 3, 0, 0, 1),
      HZ(HZ_FLAME,    4, -9.5f, 1.0f, 1.0f, 0, 0.1f, 8, 5.33f, 2, 1.5f, 3, 0, 0, 1),
      HZ(HZ_LASER,    0,  0, 20.0f, 20.0f, 0, 0.3f, 30, 10, 2, 1.5f, 10, 0, 0, 0) },   /* offset 10: idle at the horn, the first tell at 18.5 s */
    .nhaz = 9,
    .nprops = 0,
    .spawn4 = { { -5.5f, -7.5f, 0.7853982f }, { 7.5f, 7.5f, -2.3561945f }, { -7.5f, 7.5f, -0.7853982f }, { 7.5f, -7.5f, 2.3561945f } },
    .spawn2 = { { -7, 0, HALF_PI_F }, { 7, 0, -HALF_PI_F } },
    .ops_zone = { -9, -9 }, .traction_mult = 1, .vis = VIS_ALL, .one_camera_default = 0, .shared_yaw_deg = 0, .tiles_w = 34, .tiles_h = 34,
    .hill = { { -6, -6 }, { 6, -6 }, { 6, 6 }, { -6, 6 } } },

  /* -- 6.3.3 THE FREE PILE, 24 x 16 --
   * ##########################
   * #........................#
   * #.....DD.................#
   * #.....DD.................#
   * #........................#
   * #..........****..........#
   * #B........******.........#
   * #........********........#
   * #........********........#
   * #B.......********.......S#
   * #........********.......S#
   * #.........******........S#
   * #B.........****.........S#
   * #........................#
   * #.................DD.....#
   * #OO...............DD.....#
   * #OO......................#
   * ##########################
   */
  { .name = "THE FREE PILE", .hook = "A MOUND. SCREWS.", .w = 24, .d = 16, .open_mask = 0, .floor_style = FL_CONCRETE,
    .tiles = TILES_PILE, .ntiles = (int)(sizeof TILES_PILE / sizeof TILES_PILE[0]),
    .sun = 0xece3d3, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0xf2ab1e, .point_b = 0xece3d3,
    .sun_i = 2.0f, .hemi_i = 1.2f, .point_a_i = 6, .point_b_i = 4, .pa = { 0, 2, 0 }, .pb = { 8, 3, -5 },
    .haz = {
      HZ(HZ_MOUND,    0,  0, 8.0f, 8.0f, 0, 0.9f, 0, 0, 0, 0, 8, 0, 0, 0),
      HZ(HZ_DROPZONE, -6, -6, 2.0f, 2.0f, 0, 0, 0, 0, 0, 1.5f, 25, 180, 0, 0),
      HZ(HZ_DROPZONE,  6,  6, 2.0f, 2.0f, 0, 0, 0, 0, 0, 1.5f, 25, 180, 0, 0),
      HZ(HZ_SCREW,  11.6f, 2, 0.8f, 4.0f, 0, 0.3f, 0, 0, 1, 0, 3, 0, 0, 0),                  /* carries toward plus z */
      HZ(HZ_BUMPER, -11.6f, -3, 0.8f, 0.8f, 0, 1.0f, 0, 0, 0, 0, 4, 100, 0, 0),             /* old servers, unbreakable */
      HZ(HZ_BUMPER, -11.6f,  0, 0.8f, 0.8f, 0, 1.0f, 0, 0, 0, 0, 4, 100, 0, 0),
      HZ(HZ_BUMPER, -11.6f,  3, 0.8f, 0.8f, 0, 1.0f, 0, 0, 0, 0, 4, 100, 0, 0) },
    .nhaz = 7,
    /* the mound: 12 boxes, 60 HP each, tops near the height field; they are terrain skin, not solids */
    .props = {
      PR(PR_BOX,  0,     0,    1.4f, 1.4f, 0.9f, 0.3f, 0xb08a5a, 60),
      PR(PR_BOX,  1.5f,  0.4f, 1.3f, 1.3f, 0.6f, 0.0f, 0xc29a66, 60),
      PR(PR_BOX, -0.4f,  1.5f, 1.3f, 1.3f, 0.6f, 0.5f, 0xa8835a, 60),
      PR(PR_BOX, -1.5f, -0.4f, 1.3f, 1.3f, 0.6f, 0.2f, 0xb8926a, 60),
      PR(PR_BOX,  0.4f, -1.5f, 1.3f, 1.3f, 0.6f, 0.7f, 0xc29a66, 60),
      PR(PR_BOX,  3.0f,  0.0f, 1.2f, 1.2f, 0.4f, 0.1f, 0xb08a5a, 60),
      PR(PR_BOX,  2.1f,  2.1f, 1.2f, 1.2f, 0.4f, 0.4f, 0xa8835a, 60),
      PR(PR_BOX,  0.0f,  3.0f, 1.2f, 1.2f, 0.4f, 0.0f, 0xc29a66, 60),
      PR(PR_BOX, -2.1f,  2.1f, 1.2f, 1.2f, 0.4f, 0.6f, 0xb8926a, 60),
      PR(PR_BOX, -3.0f,  0.0f, 1.2f, 1.2f, 0.4f, 0.2f, 0xb08a5a, 60),
      PR(PR_BOX, -2.1f, -2.1f, 1.2f, 1.2f, 0.4f, 0.5f, 0xa8835a, 60),
      PR(PR_BOX,  2.1f, -2.1f, 1.2f, 1.2f, 0.4f, 0.3f, 0xc29a66, 60) },
    .nprops = 12,
    .spawn4 = { { -9.5f, -5.5f, 0.7853982f }, { 9.5f, 5.5f, -2.3561945f }, { -7.5f, 5.5f, -0.7853982f }, { 9.5f, -5.5f, 2.3561945f } },
    .spawn2 = { { -8, 0, HALF_PI_F }, { 8, 0, -HALF_PI_F } },
    .ops_zone = { -11, 7 }, .traction_mult = 1,
    /* the mound hides the opposite row from the edge cells (6.5) */
    .vis = { 0x3fff, 0x1fff, 0x8fff, 0xcfff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xfff3, 0xfff1, 0xfff8, 0xfffc },
    .one_camera_default = 0, .shared_yaw_deg = 0, .tiles_w = 40, .tiles_h = 27,
    .hill = { { -7, -4 }, { 7, -4 }, { 7, 4 }, { -7, 4 } } },

  /* -- 6.3.4 THE DOCK, 20 x 16, wet, outdoors at night --
   * ######################
   * #====================#
   * #====================#
   * #====================#
   * #...RR..........RR...#
   * #...RR..........RR...#
   * #...RR..........RR...#
   * #....................#
   * #....................#
   * #....................#
   * #.........XX.........#
   * #.........XX.........#
   * #....................#
   * #....................#
   * #....................#
   * #OO.......^^^........#
   * #OO.......^^^........#
   * ######################
   */
  { .name = "THE DOCK", .hook = "A DECK. RAIN. A GAP.", .w = 20, .d = 16, .open_mask = 0, .floor_style = FL_WET,
    .tiles = TILES_DOCK, .ntiles = (int)(sizeof TILES_DOCK / sizeof TILES_DOCK[0]),
    .sun = 0xf2c57a, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0xf2ab1e, .point_b = 0x5fb892,
    .sun_i = 1.2f, .hemi_i = 1.2f, .point_a_i = 10, .point_b_i = 2, .pa = { 0, 4, -4 }, .pb = { 6, 1, 6 },
    .haz = {
      HZ(HZ_DECK,      0, -6.5f, 20.0f, 3.0f, 0, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0),
      HZ(HZ_RAMP,     -6, -3.5f, 2.0f, 3.0f, PI_F, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0),           /* rises toward minus z onto the deck */
      HZ(HZ_RAMP,      6, -3.5f, 2.0f, 3.0f, PI_F, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0),
      HZ(HZ_FLOORFLIP, 0,  2,    2.0f, 2.0f, 0, 0.05f, 0, 0, 0, 1.0f, 0, 60, 180, 1),
      HZ(HZ_PIT,       0,  7,    3.0f, 2.0f, 0, -2.0f, 20, 5, 6, 1.5f, 0, 0, 0, 0) },        /* the roll up door gap */
    .nhaz = 5,
    .nprops = 0,
    .spawn4 = { { -7.5f, -3.5f, 0.7853982f }, { 7.5f, 5.5f, -2.3561945f }, { -5.5f, 5.5f, -0.7853982f }, { 7.5f, -3.5f, 2.3561945f } },
    .spawn2 = { { -7, 1, HALF_PI_F }, { 7, 1, -HALF_PI_F } },
    .ops_zone = { -9, 7 }, .traction_mult = 0.85f, .vis = VIS_ALL, .one_camera_default = 0, .shared_yaw_deg = 0, .tiles_w = 34, .tiles_h = 27,
    .hill = { { -6, -1 }, { 6, -1 }, { 6, 5 }, { -6, 5 } } },

  /* -- 6.3.5 THE SHOP, 20 x 20, benches that break --
   * ######################
   * #OO.......PP.........#
   * #OO..................#
   * #..........~.........#
   * #..........~.........#
   * #..........~.........#
   * #..........~.........#
   * #..........~.........#
   * #..........~.........#
   * #....................#
   * #P.~~~~~~K..K~~~~~~.P#
   * #....................#
   * #....................#
   * #..........~.........#
   * #..........~.........#
   * #..........~.........#
   * #..........~.........#
   * #..........~.........#
   * #..........~.........#
   * #....................#
   * #.........PP.........#
   * ######################
   */
  { .name = "THE SHOP", .hook = "BENCHES THAT BREAK.", .w = 20, .d = 20, .open_mask = 0, .floor_style = FL_SHOP,
    .tiles = TILES_SHOP, .ntiles = (int)(sizeof TILES_SHOP / sizeof TILES_SHOP[0]),
    .sun = 0xece3d3, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0xf2ab1e, .point_b = 0x5fb892,
    .sun_i = 2.0f, .hemi_i = 1.2f, .point_a_i = 6, .point_b_i = 6, .pa = { 0, 3, 0 }, .pb = { -7, 2, 7 },
    .haz = {
      HZ(HZ_PULVERIZER,  0, -9.4f, 1.2f, 1.2f, 0, 1.6f, 0, 0, 0, 1.5f, 20, 150, 72, 1),
      HZ(HZ_PULVERIZER,  0,  9.4f, 1.2f, 1.2f, PI_F, 1.6f, 0, 0, 0, 1.5f, 20, 150, 72, 1),
      HZ(HZ_PULVERIZER, -9.4f, 0, 1.2f, 1.2f, HALF_PI_F, 1.6f, 0, 0, 0, 1.5f, 20, 150, 72, 1),
      HZ(HZ_PULVERIZER,  9.4f, 0, 1.2f, 1.2f, -HALF_PI_F, 1.6f, 0, 0, 0, 1.5f, 20, 150, 72, 1),
      HZ(HZ_KILLSAW, -1.5f, 0, 0.4f, 1.6f, 0, 0.3f, 7, 0,    1.5f, 1.5f, 6, 0, 0, 0),         /* slots along z */
      HZ(HZ_KILLSAW,  1.5f, 0, 0.4f, 1.6f, 0, 0.3f, 7, 3.5f, 1.5f, 1.5f, 6, 0, 0, 0) },
    .nhaz = 6,
    /* four bench runs 6 x 0.6 at 0.4 high from |x| or |z| 2 to 8, three 2 u segments of 60 HP each */
    .props = {
      PR(PR_BENCH,  3, 0, 2.0f, 0.6f, 0.4f, 0, 0x8a6a48, 60), PR(PR_BENCH,  5, 0, 2.0f, 0.6f, 0.4f, 0, 0x8a6a48, 60), PR(PR_BENCH,  7, 0, 2.0f, 0.6f, 0.4f, 0, 0x8a6a48, 60),
      PR(PR_BENCH, -3, 0, 2.0f, 0.6f, 0.4f, 0, 0x8a6a48, 60), PR(PR_BENCH, -5, 0, 2.0f, 0.6f, 0.4f, 0, 0x8a6a48, 60), PR(PR_BENCH, -7, 0, 2.0f, 0.6f, 0.4f, 0, 0x8a6a48, 60),
      PR(PR_BENCH, 0,  3, 0.6f, 2.0f, 0.4f, 0, 0x8a6a48, 60), PR(PR_BENCH, 0,  5, 0.6f, 2.0f, 0.4f, 0, 0x8a6a48, 60), PR(PR_BENCH, 0,  7, 0.6f, 2.0f, 0.4f, 0, 0x8a6a48, 60),
      PR(PR_BENCH, 0, -3, 0.6f, 2.0f, 0.4f, 0, 0x8a6a48, 60), PR(PR_BENCH, 0, -5, 0.6f, 2.0f, 0.4f, 0, 0x8a6a48, 60), PR(PR_BENCH, 0, -7, 0.6f, 2.0f, 0.4f, 0, 0x8a6a48, 60) },
    .nprops = 12,
    .spawn4 = { { -5.5f, -7.5f, 0.7853982f }, { 7.5f, 7.5f, -2.3561945f }, { -7.5f, 7.5f, -0.7853982f }, { 7.5f, -7.5f, 2.3561945f } },
    /* opposite quadrants; 6.5 u in so the spawn clears the OPS zone by 1.5 (the spec's 7.5 sits 0.7 from it) */
    .spawn2 = { { -6.5f, -6.5f, 0.7853982f }, { 6.5f, 6.5f, -2.3561945f } },
    .ops_zone = { -9, -9 }, .traction_mult = 1,
    /* a cell sees itself, its row and column, and of the far diagonal quadrant only the cell on the
     * centre cross, where the bench segments nearest the middle sit (6.5) */
    .vis = { 0x37ff, 0x37ff, 0xceff, 0xceff, 0x37ff, 0x37ff, 0xceff, 0xceff, 0xff73, 0xff73, 0xffec, 0xffec, 0xff73, 0xff73, 0xffec, 0xffec },
    .one_camera_default = 0, .shared_yaw_deg = 0, .tiles_w = 34, .tiles_h = 34,
    .hill = { { -5, -5 }, { 5, -5 }, { 5, 5 }, { -5, 5 } } },

  /* -- 6.3.6 THE ROOF, 18 x 18, open on plus x and plus z --
   * ###################:
   * #OO................:
   * #OO................:
   * #..................:
   * #.................B:
   * #......HH..........:
   * #......HH..........:
   * #..................:
   * #..................:
   * #..................:
   * #...HH............B:
   * #...HH.............:
   * #..................:
   * #..................:
   * #..................:
   * #..................:
   * #.................B:
   * #..................:
   * #...B.....B.....B..:
   * ::::::::::::::::::::
   */
  { .name = "THE ROOF", .hook = "NO RAILS. WIND.", .w = 18, .d = 18, .open_mask = OPEN_PX | OPEN_PZ, .floor_style = FL_ROOF,
    .tiles = 0, .ntiles = 0,
    .sun = 0xf2ab1e, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0xf2ab1e, .point_b = 0x5fb892,
    .sun_i = 1.6f, .hemi_i = 1.2f, .point_a_i = 5, .point_b_i = 5, .pa = { -4, 3, -4 }, .pb = { 5, 3, 5 },
    .haz = {
      HZ(HZ_BUMPER,  8.5f, -5.5f, 0.8f, 0.8f, 0, 0.8f, 0, 0, 0, 0, 4, 80, 60, 0),
      HZ(HZ_BUMPER,  8.5f,  0.5f, 0.8f, 0.8f, 0, 0.8f, 0, 0, 0, 0, 4, 80, 60, 0),
      HZ(HZ_BUMPER,  8.5f,  6.5f, 0.8f, 0.8f, 0, 0.8f, 0, 0, 0, 0, 4, 80, 60, 0),
      HZ(HZ_BUMPER, -4.5f,  8.5f, 0.8f, 0.8f, 0, 0.8f, 0, 0, 0, 0, 4, 80, 60, 0),
      HZ(HZ_BUMPER,  1.5f,  8.5f, 0.8f, 0.8f, 0, 0.8f, 0, 0, 0, 0, 4, 80, 60, 0),
      HZ(HZ_BUMPER,  6.5f,  8.5f, 0.8f, 0.8f, 0, 0.8f, 0, 0, 0, 0, 4, 80, 60, 0),      /* the map's (7.5, 8.5) sits 1.2 from the corner post */
      HZ(HZ_WIND,    0, 0, 18.0f, 18.0f, 0, 0, 15, 0, 3, 1.5f, 0, 0, 0, 0) },               /* toward plus x then plus z, alternating */
    .nhaz = 7,
    .props = {
      PR(PR_HVAC, -2, -4, 2.0f, 2.0f, 1.2f, 0, 0x8a857c, 0),
      PR(PR_HVAC, -5,  1, 2.0f, 2.0f, 1.2f, 0, 0x8a857c, 0) },
    .nprops = 2,
    .spawn4 = { { -6.5f, -4.5f, 0.7853982f }, { 6.5f, 6.5f, -2.3561945f }, { -6.5f, 6.5f, -0.7853982f }, { 6.5f, -6.5f, 2.3561945f } },
    /* 5.5 u in so the spawn clears the OPS zone by 1.5 (the spec's 6 sits 1.4 from it) */
    .spawn2 = { { -5.5f, -5.5f, 0.7853982f }, { 5.5f, 5.5f, -2.3561945f } },
    .ops_zone = { -8, -8 }, .traction_mult = 1, .vis = VIS_ALL, .one_camera_default = 0, .shared_yaw_deg = 0, .tiles_w = 30, .tiles_h = 30,
    .hill = { { -5, -5 }, { 4, -5 }, { 4, 4 }, { -5, 4 } } },
};

/* the garage bay: four turntables in a row at x = 0, 12, 24, 36, a pegboard back wall */
const arena_def_t GARAGE_BAY = {
  .name = "GARAGE", .hook = "", .w = 48, .d = 10, .floor_style = FL_GARAGE,
  .tiles = TILES_GARAGE, .ntiles = (int)(sizeof TILES_GARAGE / sizeof TILES_GARAGE[0]),
  .sun = 0xece3d3, .hemi_sky = 0xece3d3, .hemi_ground = 0x15120f, .point_a = 0xf2ab1e, .point_b = 0x5fb892,
  .sun_i = 2.0f, .hemi_i = 1.2f, .point_a_i = 6, .point_b_i = 6, .pa = { 6, 3, 2 }, .pb = { 30, 3, 2 },
  .traction_mult = 1, .vis = VIS_ALL, .tiles_w = 48, .tiles_h = 10 };

/* -- helpers ------------------------------------------------------- */

int arena_cell(const arena_def_t *a, float x, float z) {
  int cx = (int)((x + a->w * 0.5f) / a->w * 4), cz = (int)((z + a->d * 0.5f) / a->d * 4);
  if (cx < 0) cx = 0; if (cx > 3) cx = 3;
  if (cz < 0) cz = 0; if (cz > 3) cz = 3;
  return cz * 4 + cx;
}

float arena_mound_h(float r) {
  if (r >= 4.0f) return 0;
  float t = 1.0f - r / 4.0f;        /* 1 at the centre */
  return 0.9f * t * (2.0f - t);     /* a dome: steep at the rim, flat on top */
}

static int in_rect(const hazard_def_t *h, float x, float z) {
  return x >= h->x - h->w * 0.5f && x <= h->x + h->w * 0.5f && z >= h->z - h->d * 0.5f && z <= h->z + h->d * 0.5f;
}

/* a ramp rises from 0 at its low end to h at the end it points to (ang is the direction of rise) */
static float ramp_h(const hazard_def_t *h, float x, float z) {
  float dx = x - h->x, dz = z - h->z;
  float sx = rtr_sin(h->ang), sz = rtr_cos(h->ang);           /* the rise direction */
  float along = dx * sx + dz * sz;                             /* -L/2 .. L/2 */
  float len = (sx * sx > sz * sz) ? h->w : h->d;
  float t = (along + len * 0.5f) / len;
  return h->h * rtr_clamp(t, 0, 1);
}

float arena_floor_y(const arena_def_t *a, float x, float z) {
  float y = 0;
  for (int i = 0; i < a->nhaz; i++) {
    const hazard_def_t *h = &a->haz[i];
    switch (h->type) {
      case HZ_DECK: if (in_rect(h, x, z) && h->h > y) y = h->h; break;
      case HZ_RAMP:
        if (in_rect(h, x, z)) { float r = h->h <= 0.3f ? h->h : ramp_h(h, x, z); if (r > y) y = r; }
        break;
      case HZ_MOUND: {
        float dx = x - h->x, dz = z - h->z, r = sqrtf(dx * dx + dz * dz), mh = arena_mound_h(r);
        if (mh > y) y = mh;
      } break;
      default: break;
    }
  }
  return y;
}

int arena_on_slope(const arena_def_t *a, float x, float z) {
  for (int i = 0; i < a->nhaz; i++) {
    const hazard_def_t *h = &a->haz[i];
    if (h->type == HZ_RAMP && h->h > 0.3f && in_rect(h, x, z)) return 1;
    if (h->type == HZ_MOUND) { float dx = x - h->x, dz = z - h->z; if (dx * dx + dz * dz < 16.0f) return 1; }
  }
  return 0;
}

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

void arena_ops_gate(const arena_def_t *a, float out[2][4]) {
  float cx = a->ops_zone[0], cz = a->ops_zone[1];
  float ix = cx < 0 ? cx + 1 : cx - 1;    /* the inner face toward the arena centre */
  float iz = cz < 0 ? cz + 1 : cz - 1;
  /* segment along z on the inner x face */
  out[0][0] = ix - 0.05f; out[0][2] = ix + 0.05f; out[0][1] = cz - 1; out[0][3] = cz + 1;
  /* segment along x on the inner z face */
  out[1][0] = cx - 1; out[1][2] = cx + 1; out[1][1] = iz - 0.05f; out[1][3] = iz + 0.05f;
}

void arena_hazard_rect(const hazard_def_t *h, float *x0, float *z0, float *x1, float *z1) {
  float hw = h->w * 0.5f, hd = h->d * 0.5f;
  if (h->type == HZ_BUMPER) hw = hd = 0.4f;
  *x0 = h->x - hw; *x1 = h->x + hw; *z0 = h->z - hd; *z1 = h->z + hd;
}

int arena_prop_solid(const prop_def_t *p) {
  return p->kind == PR_BENCH || p->kind == PR_HVAC || p->kind == PR_SERVER || p->kind == PR_WALL;
}
