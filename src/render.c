/* render.c: the per-frame viewport loop (spec 14.2) and the 30 fps render
 * fallback (13, 15). Owned by the world agent. Under RTR_FAKE_SIM it also
 * carries a small deterministic fake match driver so the world, camera and fx
 * can be seen and tuned before the sim lands; it is compiled out by default. */
#include "render.h"
#include "world.h"
#include "camera.h"
#include "fx.h"
#include "game.h"
#include "bot.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#ifdef RTR_FAKE_SIM
#include "wasmcart.h"
#endif

/* -- the render half fallback ------------------------------------- */
/* frame_ms is the host's frame delta: on a vsynced 60 Hz host it is 16.7 ms when the cart keeps up and
 * 33 ms when it drops a frame, never under 12, so the spec's "back when under 12 ms" (13) could never
 * release it. On: 30 frames over 20 ms (the cart misses vsync). Off: a run of frames at 60 Hz (17.5 ms
 * or less) that starts at 3 s and doubles every time the fallback re-trips in the same phase, so a
 * fight on the edge settles instead of flapping. Every phase change resets it: a slow 4 pad fight
 * never carries its half rate into the garage and the menus. */
static int half_phase;
static int release_need = 180, release_trips, half_scene = -1;

void render_measure(float frame_ms) {
  if (G.phase != half_scene) { half_scene = G.phase; G.render_half = 0; G.slow_frames = 0; G.fast_frames = 0; release_need = 180; release_trips = 0; }
  G.frame_ms_avg += (frame_ms - G.frame_ms_avg) * 0.1f;
  if (frame_ms > 20) { G.slow_frames++; G.fast_frames = 0; } else if (frame_ms <= 17.5f) { G.fast_frames++; G.slow_frames = 0; } else { G.slow_frames = 0; G.fast_frames = 0; }
  if (!G.render_half && G.slow_frames >= 30) {
    G.render_half = 1; G.slow_frames = 0; G.fast_frames = 0;
    if (release_trips++ > 0 && release_need < 1800) release_need *= 2;
  }
  if (G.render_half && G.fast_frames >= release_need) { G.render_half = 0; G.fast_frames = 0; }
}

/* the arena cell under a camera, for the vis table */
static int camera_cell(t3_camera *c, const arena_def_t *a) {
  if (!a) return -1;
  t3_vec3 p = t3_object_get_world_position(c);
  return arena_cell(a, p.x, p.z);
}

int render_frame(const match_view_t *v) {
  if (G.render_half) { half_phase ^= 1; if (half_phase) return 0; }
  const arena_def_t *a = world_arena();
  /* a layout that leaves screen uncovered (the 3 player map quadrant, the garage
   * panels) clears the whole frame once so the HUD never composites over a stale view */
  int covered = 0;
  for (int i = 0; i < v->n; i++) covered += v->vp[i].w * v->vp[i].h;
  if (covered < 1280 * 720) {
    t3_renderer_set_scissor_test(RTR_R, false);
    t3_renderer_set_viewport(RTR_R, 0, 0, RTR_W, RTR_H);
    t3_renderer_clear(RTR_R, true, true, false);
  }
  t3_renderer_set_scissor_test(RTR_R, true);
  t3_renderer_set_auto_clear(RTR_R, true);           /* clears colour and depth inside the scissor only */
  for (int i = 0; i < v->n; i++) {
    t3_camera *cam = cam_get(i);
    if (!cam) continue;
    rect_t r = rect_scale(&v->vp[i], RTR_W, RTR_H);
    if (r.w <= 0 || r.h <= 0) continue;
    world_set_cell_visibility(v->cell[i] >= 0 ? v->cell[i] : camera_cell(cam, a), a);
    fx_face(i);
    cam->aspect = (float)r.w / (float)r.h;
    t3_camera_update_projection_matrix(cam);
    t3_renderer_set_viewport(RTR_R, r.x, r.y, r.w, r.h);   /* pixels, y from the top */
    t3_renderer_set_scissor(RTR_R, r.x, r.y, r.w, r.h);
    t3_renderer_render(RTR_R, world_scene(), cam);
  }
  t3_renderer_set_scissor_test(RTR_R, false);
  t3_renderer_set_viewport(RTR_R, 0, 0, RTR_W, RTR_H);
  t3_renderer_set_auto_clear(RTR_R, false);          /* the HUD quad composites over every view */
  return 1;
}

/* -- the fake driver ---------------------------------------------- */
#ifdef RTR_FAKE_SIM
#ifndef RTR_FAKE_ARENA
#define RTR_FAKE_ARENA 0
#endif
#ifndef RTR_FAKE_GARAGE
#define RTR_FAKE_GARAGE 0
#endif
#ifndef RTR_FAKE_ONECAM
#define RTR_FAKE_ONECAM 0
#endif
#ifndef RTR_FAKE_KO
#define RTR_FAKE_KO 0
#endif
#if RTR_FAKE_SIM < 1 || RTR_FAKE_SIM > 4
#define FAKE_PLAYERS 2
#else
#define FAKE_PLAYERS RTR_FAKE_SIM
#endif

/* the six arenas with the hazards and props of spec 6.3, so the render side of
 * item 14 can be seen before arena.c carries them; a cross check for the arena agent */
static arena_def_t fake_arenas[6];
#define HZ(T, X, Z, W, D, ANG, H, PER, OFF, ACT, TELL, DMG, P0, P1, SEAT) (hazard_def_t){ T, X, Z, W, D, ANG, H, PER, OFF, ACT, TELL, DMG, P0, P1, SEAT }
#define PR(K, X, Z, W, D, H, ANG, C, HP) (prop_def_t){ K, X, Z, W, D, H, ANG, C, HP }
static const tile_rect_t bay_tiles[] = { { 6, 4, 4, 2, 0 }, { 20, 3, 1, 8, 1 }, { 30, 18, 3, 3, 1 }, { 12, 22, 6, 1, 0 }, { 34, 5, 2, 2, 0 }, { 18, 11, 4, 1, 1 }, { 18, 15, 4, 1, 1 }, { 18, 11, 1, 5, 1 }, { 21, 11, 1, 5, 1 } };
static const tile_rect_t laser_tiles[] = { { 13, 13, 8, 8, 1 }, { 10, 11, 2, 1, 0 }, { 22, 11, 2, 1, 0 }, { 10, 22, 2, 1, 0 }, { 22, 22, 2, 1, 0 } };
static const tile_rect_t pile_tiles[] = { { 3, 3, 3, 3, 0 }, { 33, 2, 3, 3, 0 }, { 5, 20, 3, 3, 0 }, { 31, 21, 3, 3, 0 }, { 14, 2, 3, 3, 0 }, { 26, 23, 3, 3, 0 }, { 12, 8, 16, 1, 1 }, { 12, 19, 16, 1, 1 }, { 12, 8, 1, 12, 1 }, { 27, 8, 1, 12, 1 } };
static const tile_rect_t dock_tiles[] = { { 0, 5, 34, 2, 1 }, { 5, 10, 2, 2, 0 }, { 27, 10, 2, 2, 0 } };
static const tile_rect_t shop_tiles[] = { { 3, 16, 11, 1, 0 }, { 20, 16, 11, 1, 0 }, { 16, 3, 1, 11, 0 }, { 16, 20, 1, 11, 0 }, { 16, 0, 2, 2, 1 }, { 16, 32, 2, 2, 1 }, { 0, 16, 2, 2, 1 }, { 32, 16, 2, 2, 1 } };
static const tile_rect_t garage_tiles[] = { { 4, 2, 3, 2, 0 }, { 14, 6, 1, 4, 1 }, { 24, 1, 6, 1, 0 }, { 36, 3, 2, 2, 1 }, { 42, 7, 4, 1, 0 } };

static void fake_arenas_init(void) {
  for (int i = 0; i < 6; i++) fake_arenas[i] = ARENAS[i];
  arena_def_t *a;
  a = &fake_arenas[AR_BAY];
  a->tiles = bay_tiles; a->ntiles = 9;
  a->haz[a->nhaz++] = HZ(HZ_PULVERIZER, 0, -7.4f, 1.2f, 1.2f, 0, 0, 0, 0, 0, 1.5f, 20, 0, 0, 1);
  a->haz[a->nhaz++] = HZ(HZ_PULVERIZER, 0, 7.4f, 1.2f, 1.2f, 0, 0, 0, 0, 0, 1.5f, 20, 0, 0, 1);
  a->haz[a->nhaz++] = HZ(HZ_PIT, 0, 0, 2, 2, 0, 0, 20, 5, 6, 1.5f, 0, 0, 0, 0);
  a->props[a->nprops++] = PR(PR_SHELF, -4.5f, -9.8f, 2.4f, 0.8f, 2.2f, 0, 0, 0);
  a->props[a->nprops++] = PR(PR_SHELF, 5.5f, -9.8f, 2.4f, 0.8f, 2.2f, 0, 0, 0);
  a->props[a->nprops++] = PR(PR_SIGN, 0.5f, -9.8f, 2.6f, 0.2f, 2.8f, 0, 0, 0);
  a = &fake_arenas[AR_LASER];
  a->tiles = laser_tiles; a->ntiles = 5;
  float offs[4] = { 0, 1.75f, 3.5f, 5.25f }, sx[4] = { -3, 3, -3, 3 }, sz[4] = { -3, -3, 3, 3 };
  for (int k = 0; k < 4; k++) a->haz[a->nhaz++] = HZ(HZ_KILLSAW, sx[k], sz[k], 1.6f, 0.4f, 0, 0, 7, offs[k], 1.5f, 1.5f, 6, 0, 0, 0);
  a->haz[a->nhaz++] = HZ(HZ_RAMP, 0, 0, 4, 4, 0, 0.2f, 0, 0, 0, 0, 0, 0, 0, 0);
  for (int k = 0; k < 3; k++) a->haz[a->nhaz++] = HZ(HZ_FLAME, -4 + k * 4.0f, -9.3f, 1, 1, 0, 0, 8, k * 2.5f, 2, 1.5f, 3, 0, 0, 1);
  a->haz[a->nhaz++] = HZ(HZ_LASER, 0, 0, 0, 0, 0, 0.3f, 30, 20, 2, 1.5f, 10, 0, 0, 0);
  a = &fake_arenas[AR_PILE];
  a->tiles = pile_tiles; a->ntiles = 10;
  a->haz[a->nhaz++] = HZ(HZ_DROPZONE, -6, -6, 2, 2, 0, 0, 12, 0, 1.5f, 1.5f, 25, 0, 0, 0);
  a->haz[a->nhaz++] = HZ(HZ_DROPZONE, 6, 6, 2, 2, 0, 0, 12, 6, 1.5f, 1.5f, 25, 0, 0, 0);
  a->haz[a->nhaz++] = HZ(HZ_SCREW, 11.5f, 2, 0.8f, 4, 1.5707963f, 0, 0, 0, 1, 0, 3, 0, 0, 0);
  for (int k = 0; k < 3; k++) a->haz[a->nhaz++] = HZ(HZ_BUMPER, -11.5f, -3 + k * 3.0f, 0.8f, 0.8f, 0, 0, 0, 0, 0, 0, 4, 10, 0, 0);
  { float ring[12][3] = { { 0, 0, 0.9f }, { 1.6f, 0, 0.7f }, { -1.6f, 0, 0.7f }, { 0, 1.6f, 0.7f }, { 0, -1.6f, 0.7f }, { 1.3f, 1.3f, 0.6f }, { -1.3f, 1.3f, 0.6f }, { 1.3f, -1.3f, 0.6f }, { -1.3f, -1.3f, 0.6f }, { 3.0f, 0.5f, 0.4f }, { -3.0f, -0.5f, 0.4f }, { 0.5f, -3.0f, 0.45f } };
    for (int k = 0; k < 12; k++) a->props[a->nprops++] = PR(PR_BOX, ring[k][0], ring[k][1], 1.3f + (k % 3) * 0.2f, 1.2f + (k % 2) * 0.25f, ring[k][2], k * 0.35f, 0, 60); }
  a = &fake_arenas[AR_DOCK];
  a->tiles = dock_tiles; a->ntiles = 3;
  a->haz[a->nhaz++] = HZ(HZ_DECK, 0, -6.5f, 20, 3, 0, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0);
  a->haz[a->nhaz++] = HZ(HZ_RAMP, -6, -3.5f, 2, 3, 0, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0);
  a->haz[a->nhaz++] = HZ(HZ_RAMP, 6, -3.5f, 2, 3, 0, 1.0f, 0, 0, 0, 0, 0, 0, 0, 0);
  a->haz[a->nhaz++] = HZ(HZ_FLOORFLIP, 0, 2, 2, 2, 0, 0, 9, 0, 0.6f, 1.5f, 0, 0, 0, 1);
  a->haz[a->nhaz++] = HZ(HZ_PIT, 0, 7, 3, 2, 0, 0, 20, 5, 6, 1.5f, 0, 0, 0, 0);
  a = &fake_arenas[AR_SHOP];
  a->tiles = shop_tiles; a->ntiles = 8;
  for (int k = 0; k < 3; k++) {
    float p = 3 + k * 2.0f;
    a->props[a->nprops++] = PR(PR_BENCH, p, 0, 2, 0.6f, 0.4f, 0, 0, 60); a->props[a->nprops++] = PR(PR_BENCH, -p, 0, 2, 0.6f, 0.4f, 0, 0, 60);
    a->props[a->nprops++] = PR(PR_BENCH, 0, p, 2, 0.6f, 0.4f, 1.5707963f, 0, 60); a->props[a->nprops++] = PR(PR_BENCH, 0, -p, 2, 0.6f, 0.4f, 1.5707963f, 0, 60);
  }
  a->props[a->nprops++] = PR(PR_WALL, 0, -9.75f, 1.6f, 0.3f, 1.4f, 0, 0x6f6c66, 0);
  a->haz[a->nhaz++] = HZ(HZ_PULVERIZER, 0, -9.4f, 1.2f, 1.2f, 0, 0, 0, 0, 0, 1.5f, 20, 0, 0, 1);
  a->haz[a->nhaz++] = HZ(HZ_PULVERIZER, 0, 9.4f, 1.2f, 1.2f, 0, 0, 0, 0, 0, 1.5f, 20, 0, 0, 1);
  a->haz[a->nhaz++] = HZ(HZ_PULVERIZER, -9.4f, 0, 1.2f, 1.2f, 0, 0, 0, 0, 0, 1.5f, 20, 0, 0, 1);
  a->haz[a->nhaz++] = HZ(HZ_PULVERIZER, 9.4f, 0, 1.2f, 1.2f, 0, 0, 0, 0, 0, 1.5f, 20, 0, 0, 1);
  a->haz[a->nhaz++] = HZ(HZ_KILLSAW, -1.5f, 0, 0.4f, 1.6f, 1.5707963f, 0, 7, 0, 1.5f, 1.5f, 6, 0, 0, 0);
  a->haz[a->nhaz++] = HZ(HZ_KILLSAW, 1.5f, 0, 0.4f, 1.6f, 1.5707963f, 0, 7, 3.5f, 1.5f, 1.5f, 6, 0, 0, 0);
  a = &fake_arenas[AR_ROOF];
  { float bx[6] = { 7.5f, 7.5f, 7.5f, -5.5f, 0.5f, 6.5f }, bz[6] = { -5.5f, 0.5f, 6.5f, 8.5f, 8.5f, 8.5f };
    for (int k = 0; k < 6; k++) a->haz[a->nhaz++] = HZ(HZ_BUMPER, bx[k], bz[k], 0.8f, 0.8f, 0, 0, 0, 0, 0, 0, 4, 10, 60, 0); }
  a->haz[a->nhaz++] = HZ(HZ_WIND, -8.4f, -8.4f, 0, 0, 0, 0, 15, 0, 3, 1.5f, 0, 0, 0, 0);
  a->props[a->nprops++] = PR(PR_HVAC, -2, -4, 2, 2, 1.3f, 0, 0, 0);
  a->props[a->nprops++] = PR(PR_HVAC, -5, 1, 2, 2, 1.3f, 0.3f, 0, 0);
  (void)garage_tiles;
}

static int fake_ready, fake_frame;
static float fake_hp[4];
static int fake_fire_slot;

static void fake_spawn(match_t *m, const arena_def_t *a) {
  memset(m, 0, sizeof *m);
  m->mode = MODE_DEATHMATCH; m->arena = RTR_FAKE_ARENA; m->players = FAKE_PLAYERS; m->fight_len_s = 75; m->clock_s = 75;
  m->nbots = FAKE_PLAYERS; m->rng = 7; m->ops_target = 255;
  int presets[4] = { PRE_CHOP, PRE_ROLL_OUT, PRE_LAUNCH, PRE_STOMP };
  for (int i = 0; i < m->nbots; i++) {
    build_t b; build_preset(&b, presets[i]); b.paint = (uint8_t)i;
    const float *sp = FAKE_PLAYERS <= 2 ? a->spawn2[i] : a->spawn4[i];
    bot_setup(&m->bots[i], &b, i, 0, (uint8_t)i, sp[0], sp[1], sp[2]);
    m->bots[i].b.px = m->bots[i].b.x; m->bots[i].b.pz = m->bots[i].b.z; m->bots[i].b.ph = m->bots[i].b.h;
    fake_hp[i] = m->bots[i].hp;
    world_bot_build(i, &b, SEAT_RGB[i]);
    world_bot_visible(i, 1);
  }
  if (FAKE_PLAYERS >= 3) {
    bot_setup_ops(&m->bots[OPS_SLOT], a->ops_zone[0], a->ops_zone[1], a->ops_zone[0] < 0 ? 1.5707963f : -1.5707963f, 0);
    m->bots[OPS_SLOT].b.px = m->bots[OPS_SLOT].b.x; m->bots[OPS_SLOT].b.pz = m->bots[OPS_SLOT].b.z; m->bots[OPS_SLOT].b.ph = m->bots[OPS_SLOT].b.h;
    m->nbots = OPS_SLOT + 1;
    world_bot_build_ops(OPS_SLOT);
    world_bot_visible(OPS_SLOT, 1);
  }
}

void render_fake_tick(match_t *m) {
  if (!fake_ready) {
    fake_ready = 1;
    fake_arenas_init();
    for (int i = 0; i < 6; i++) world_build_arena(i, &fake_arenas[i]);
    if (RTR_FAKE_GARAGE) {
      G.phase = PH_GARAGE;
      world_attach_garage();
#ifdef RTR_FAKE_PRESET_SET
      /* the other presets and two custom builds: TOAST, SPIN, a FRAME saw bot and a LUNCHBOX crusher with a SPIKE on top */
      build_t set[4] = { { CH_SHELL, DR_HOVER, WP_FLAME, WP_NONE, AR_UHMW, 1, DC_NONE }, { CH_LOWBOY, DR_MELTY, WP_NONE, WP_NONE, AR_TI, 0, DC_HAZARD_STRIPES },
                         { CH_FRAME, DR_SIX, WP_SAW, WP_NONE, AR_PLY, 2, DC_NONE }, { CH_LUNCHBOX, DR_LEGTRACK, WP_CRUSHER, WP_SPIKE, AR_WEAR, 3, DC_TAPE_BAND } };
      for (int i = 0; i < 4; i++) { world_bot_build(i, &set[i], SEAT_RGB[i]); world_bot_visible(i, 1); world_bot_pose(i, 12.0f * i, 0.17f, 0, 0, 0); }
#else
      int presets[4] = { PRE_CHOP, PRE_ROLL_OUT, PRE_LAUNCH, PRE_STOMP };
      for (int i = 0; i < 4; i++) { build_t b; build_preset(&b, presets[i]); b.paint = (uint8_t)i; b.decal = (uint8_t)(i % DECAL_COUNT); if (i == 2) b.weapon2 = WP_NONE; world_bot_build(i, &b, SEAT_RGB[i]); world_bot_visible(i, 1); world_bot_pose(i, 12.0f * i, 0.17f, 0, 0, 0); }
#endif
      cam_set_garage(FAKE_PLAYERS);
      return;
    }
    G.phase = PH_FIGHT;
    world_attach_arena(RTR_FAKE_ARENA);
    fake_spawn(m, &fake_arenas[RTR_FAKE_ARENA]);
    cam_set_match(m, RTR_FAKE_ONECAM == 2 ? ONECAM_AUTO : RTR_FAKE_ONECAM ? ONECAM_ALWAYS : ONECAM_OFF, SPLIT_AUTO, &fake_arenas[RTR_FAKE_ARENA]);
    return;
  }
  fake_frame++;
  if (RTR_FAKE_GARAGE) {
    if (fake_frame % 150 == 10) { world_weapon_test_fire(fake_fire_slot); fake_fire_slot = (fake_fire_slot + 1) & 3; }
    if (fake_frame % 400 == 200) world_turntable_lock(1, 1);
    if (fake_frame % 400 == 300) world_turntable_lock(1, 0);
    return;
  }
  const arena_def_t *a = &fake_arenas[RTR_FAKE_ARENA];
  float t = fake_frame * DT;
  m->frame = fake_frame; m->clock_s = 75 - t; m->haz_clock = t;
  for (int i = 0; i < a->nhaz; i++) { float ph; int st = hazard_phase(&a->haz[i], m->haz_clock, &ph); m->haz[i].type = a->haz[i].type; m->haz[i].phase_t = ph; m->haz[i].active = st == 1; m->haz[i].tell = st == 2; }
  int nplay = m->nbots > FAKE_PLAYERS ? FAKE_PLAYERS : m->nbots;
  /* the bots circle the centre and lunge at each other every 90 frames: the radius
   * factor eases in over the last 36 frames of a cycle and eases back over the first 24 */
  int cycle = fake_frame % 90;
  float lunge = cycle >= 54 ? (cycle - 54) / 35.0f : (cycle < 24 ? 1 - cycle / 24.0f : 0);
  lunge = lunge * lunge * (3 - 2 * lunge);
  for (int i = 0; i < m->nbots; i++) {
    bot_t *b = &m->bots[i];
    if (b->is_ops) { b->b.px = b->b.x; b->b.pz = b->b.z; b->b.ph = b->b.h; b->lift = 0.5f + 0.5f * sinf(t); b->spin2 = 1; b->saw_on = 1; continue; }
    b->b.px = b->b.x; b->b.pz = b->b.z; b->b.ph = b->b.h; b->b.py = b->b.y;
    float ang = t * 0.5f + i * 6.2831853f / nplay;
    float R = (nplay == 2 ? 3.2f : 4.0f) * (1 - 0.72f * lunge);
    float cx = a->w * 0.05f, cz = 0;
    float nx = cx + cosf(ang) * R, nz = cz + sinf(ang) * R;
    b->b.vx = (nx - b->b.x) / DT; b->b.vz = (nz - b->b.z) / DT;
    b->b.x = nx; b->b.z = nz;
    float fwd = atan2f(b->b.vx, b->b.vz);
    if (lunge > 0) fwd = atan2f(cx - nx, cz - nz);
    b->b.h = fwd;
    b->in.buttons = IN_A;
    const weapon_def_t *w = &WEAPONS[b->build.weapon];
    if (w->spin_up > 0) b->spin = fminf(1, b->spin + DT / w->spin_up);
    if (b->build.weapon == WP_FLAME) b->fuel = 240;
    if (b->build.weapon == WP_HAMMER) { int c = (fake_frame + i * 37) % 180; b->windup = (int16_t)(c < 15 ? 15 - c : 0); b->active = (int16_t)(c >= 15 && c < 19 ? 19 - c : 0); b->cooldown = (int16_t)(c >= 19 && c < 91 ? 91 - c : 0); }
    if (b->build.weapon == WP_FLIPPER) { int c = (fake_frame + i * 53) % 200; b->active = (int16_t)(c < 4 ? 4 - c : 0); b->cooldown = (int16_t)(c >= 4 && c < 112 ? 112 - c : 0); }
    if (b->build.weapon == WP_LIFTER) b->lift = 0.5f + 0.5f * sinf(t * 2);
    if (b->stun > 0) b->stun--;
    if (b->taunt > 0) b->taunt--;
    if (i == 1 && fake_frame % 300 == 150) b->taunt = 30;
    if (fake_frame > 600 && fake_frame % 400 == 20 + i * 50 && b->b.y <= 0) { b->b.vy = 6; }
    if (b->b.y > 0 || b->b.vy > 0) { b->b.vy -= 20 * DT; b->b.y += b->b.vy * DT; if (b->b.y <= 0) { b->b.y = 0; b->b.vy = 0; event_t e = { EV_LAND, (uint8_t)i, (uint8_t)i, 0, b->b.x, b->b.z, 0, 0, 0, 0 }; sim_event(m, &e); } }
  }
  /* the hit: the last lunge frame */
  if (cycle == 89 && nplay >= 2 && fake_frame > 90) {
    int round = fake_frame / 90;
    int att = round % nplay, vic = (round + 1) % nplay;
    bot_t *A = &m->bots[att], *V = &m->bots[vic];
    float dmgs[6] = { 5, 12, 22, 35, 12, 24 };
    float dmg = dmgs[(round / 1) % 6];
    float dx = V->b.x - A->b.x, dz = V->b.z - A->b.z, l = sqrtf(dx * dx + dz * dz); if (l < 0.01f) l = 1;
    event_t e = { EV_HIT, (uint8_t)att, (uint8_t)vic, 10, (A->b.x + V->b.x) / 2, (A->b.z + V->b.z) / 2, 0.3f, dmg, dx / l, dz / l };
    if (round % 7 == 3) { e.type = EV_CLASH; e.dmg = 20; }
    sim_event(m, &e);
    int band = e.type == EV_CLASH ? BAND_CLASH : hit_band(dmg);
    m->hitstop = HIT_BANDS[band].stop;
    V->hp -= dmg; V->stun = 20; m->no_hit_frames = 0;
    if (V->hp < 15) V->hp = 90;      /* keep the show going */
    int st = V->hp <= 30 ? 3 : V->hp <= 50 ? 2 : V->hp <= 70 ? 1 : 0;
    for (int p = 0; p < PN_COUNT; p++) if (V->panel_state[p] < 4) V->panel_state[p] = (uint8_t)st;
    if (dmg >= 30 && m->ndebris < MAX_DEBRIS) {
      int p = round % PN_COUNT; V->panel_state[p] = 4;
      debris_t *d = &m->debris[m->ndebris++];
      *d = (debris_t){ V->b.x, V->b.z, 0.4f, dx / l * 2 + 1, dz / l * 2, 3, 6, 360, DB_PANEL, 1, 1, (uint8_t)vic };
      event_t s = { EV_SHED, (uint8_t)vic, (uint8_t)vic, (uint8_t)p, V->b.x, V->b.z, 0.4f, 0, dx / l, dz / l }; sim_event(m, &s);
    }
    /* a wall hit for the attacker now and then */
    if (round % 5 == 2) { event_t w = { EV_WALL, (uint8_t)att, (uint8_t)att, 0, A->b.x, A->b.z, 0.2f, 2, -dx / l, -dz / l }; sim_event(m, &w); }
    if (round % 11 == 6) { event_t r = { EV_RED_TAG, (uint8_t)vic, (uint8_t)vic, 0, V->b.x, V->b.z, 0.3f, 0, 0, 0 }; sim_event(m, &r); V->red_tag_t = 480; }
  }
  for (int i = 0; i < m->nbots; i++) if (m->bots[i].red_tag_t > 0) m->bots[i].red_tag_t--;
  /* debris integrate: gravity 20, one bounce at 0.3, drag 0.9, life */
  for (int i = 0; i < m->ndebris; i++) {
    debris_t *d = &m->debris[i];
    if (d->life <= 0) continue;
    d->vy -= 20 * DT; d->y += d->vy * DT; d->x += d->vx * DT; d->z += d->vz * DT;
    if (d->y < 0) { d->y = 0; d->vy = -d->vy * 0.3f; d->vx *= 0.6f; d->vz *= 0.6f; d->spin *= 0.5f; }
    if (d->y <= 0.001f) { d->vx *= 0.9f; d->vz *= 0.9f; }
    d->life--;
  }
  if (RTR_FAKE_KO && fake_frame == RTR_FAKE_KO) {
    bot_t *V = &m->bots[1];
    event_t e = { EV_KO, 0, 1, KO_HP, V->b.x, V->b.z, 0.3f, 40, 1, 0 };
    sim_event(m, &e); V->alive = 0; V->hp = 0;
    for (int p = 0; p < PN_COUNT; p++) V->panel_state[p] = 4;
    cam_ko(1, 0);
  }
  if (fake_frame % 60 == 0) world_crowd((fake_frame / 60) % 3, 0.5f);
  if (fake_frame % 30 == 0) { char l[48]; snprintf(l, sizeof l, "[fx] f=%d live=%d", fake_frame, fx_live_sparks()); wc_log(l, (unsigned)strlen(l)); }
}
#endif
