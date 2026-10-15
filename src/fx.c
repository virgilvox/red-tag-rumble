/* fx.c: sparks, smoke, fire, flashes, debris chips, confetti and dust on wall
 * time; the hit stop freeze; the Hit Contract driver (spec section 5). Owned
 * by the fx agent (items 6, 21, 24). One instanced cube mesh carries every
 * cube particle (sparks, chips, fire, confetti, dust, pickup cubes); one
 * instanced flat quad mesh carries the smoke; four camera facing quads are the
 * impact flashes, one per viewport. */
#include "fx.h"
#include "world.h"
#include "camera.h"
#include "hud.h"
#include "game.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f
enum { PK_SPARK, PK_CHIP, PK_FIRE, PK_CONFETTI, PK_DUST, PK_RISE, PK_TRAIL };

typedef struct {
  float x, y, z, vx, vy, vz, life, life0, size, spin;
  uint32_t colour; uint8_t kind, held, bounced, live;
  float hx, hy, hz;             /* held sparks remember the release velocity */
  int16_t free_t;               /* trail cubes fly straight for a few frames before gravity takes them */
} cube_t;

typedef struct { float x, y, z, life, life0, size0, size1, rise, size, roll; uint32_t colour; uint8_t live; int16_t slot; } puff_t;

static cube_t cubes[J_SPARK_CAP];
static puff_t puffs[J_SMOKE_CAP];
static int cube_next, puff_next;
static t3_instanced_mesh *cube_mesh, *puff_mesh;
static t3_mesh *flash[4], *star[4], *streak;
static float flash_x, flash_y, flash_z, flash_size; static int flash_frames;
static float star_size; static int star_frames;
static float streak_x, streak_y, streak_z, streak_dx, streak_dy, streak_dz; static int streak_frames;
static int white_a[4], white_frames[4], white_new[4];
static uint32_t rng = 0x5eed1234u;
static struct { float frac, x, y, z, acc_s, acc_f, dead_t; int seen, burning, side; } emit[MAX_BOTS];
#define FLASH_FRAMES 4            /* one frame at full size, then it shrinks 0.6x a frame */

static float rf(void) { return rtr_rand_f(&rng); }
static float rr(float a, float b) { return a + (b - a) * rf(); }

static void set_drawn(t3_instanced_mesh *im, int n) { im->count = n; im->mesh.base.visible = n > 0; }

void fx_init(void) {
  t3_scene *S = world_scene();
  t3_geometry *g = t3_box_geometry_new(1, 1, 1, 1, 1, 1);
  t3_material *m = t3_mesh_basic_material_new(0xffffff);
  cube_mesh = t3_instanced_mesh_new(g, m, J_SPARK_CAP);
  cube_mesh->mesh.base.frustum_culled = false;
  t3_object_add(&S->base, cube_mesh); t3_release(cube_mesh); t3_release(m); t3_release(g);
  /* smoke puffs are soft translucent discs (14.2: flat quads) faced to each viewport's camera in fx_face */
  t3_geometry *q = t3_circle_geometry_new(0.5f, 10);
  t3_material *pm = t3_mesh_basic_material_new(0xffffff);
  pm->transparent = true; pm->opacity = 0.35f; pm->depth_write = false;
  puff_mesh = t3_instanced_mesh_new(q, pm, J_SMOKE_CAP);
  puff_mesh->mesh.base.frustum_culled = false;
  t3_object_add(&S->base, puff_mesh); t3_release(puff_mesh); t3_release(pm); t3_release(q);
  t3_geometry *fq = t3_circle_geometry_new(0.5f, 14);
  for (int i = 0; i < 4; i++) {
    t3_material *fm = t3_mesh_basic_material_new(0xfff1d6);
    /* additive and depth tested: a brief warm bloom in the contact gap; the spark burst carries the impact */
    fm->transparent = true; fm->opacity = 0.55f; fm->depth_write = false; fm->depth_test = true; fm->blending = 2;
    flash[i] = t3_mesh_new(fq, fm);
    t3_object_add(&S->base, flash[i]); t3_release(flash[i]); t3_release(fm);
    flash[i]->base.visible = false;
  }
  t3_release(fq);
  /* the impact star: four crossed streaks on a canvas, one camera facing quad per viewport */
  uint8_t *px = calloc(64 * 64, 4);
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
    float u = (x + 0.5f) / 32 - 1, v = (y + 0.5f) / 32 - 1, a = 0;
    for (int k = 0; k < 4; k++) {
      float ang = k * PI / 4 + 0.2f, cs = cosf(ang), sn = sinf(ang), along = u * cs + v * sn, across = -u * sn + v * cs;
      float w = (k & 1) ? 0.05f : 0.08f, len = (k & 1) ? 0.7f : 1.0f;
      float t = (1 - fabsf(across) / w) * (1 - fabsf(along) / len); if (t > a) a = t;
    }
    float core = 1 - sqrtf(u * u + v * v) / 0.25f; if (core > a) a = core;
    if (a < 0) a = 0; if (a > 1) a = 1;
    uint8_t *p = px + ((size_t)y * 64 + x) * 4; p[0] = 0xff; p[1] = 0xf6; p[2] = 0xe6; p[3] = (uint8_t)(255 * a);
  }
  t3_texture *st = t3_texture_new(64, 64, px); free(px);
  st->color_space = T3_SRGB_COLOR_SPACE;
  t3_geometry *sq = t3_plane_geometry_new(1, 1, 1, 1);
  for (int i = 0; i < 4; i++) {
    t3_material *sm = t3_mesh_basic_material_new(0xffffff);
    t3_material_set_map(sm, st); sm->transparent = true; sm->opacity = 1; sm->depth_write = false; sm->depth_test = false;
    star[i] = t3_mesh_new(sq, sm); t3_object_add(&S->base, star[i]); t3_release(star[i]); t3_release(sm);
    star[i]->base.visible = false;
  }
  /* the launch streak: a bone quad along the flier's velocity at the plate tip */
  t3_material *lm = t3_mesh_basic_material_new(C_BONE); lm->transparent = true; lm->opacity = 0.6f; lm->depth_write = false; lm->side = T3_DOUBLE_SIDE;
  streak = t3_mesh_new(sq, lm); t3_object_add(&S->base, streak); t3_release(streak); t3_release(lm);
  streak->base.visible = false;
  t3_release(sq); t3_release(st);
  fx_reset();
}

void fx_reset(void) {
  memset(cubes, 0, sizeof cubes); memset(puffs, 0, sizeof puffs); memset(emit, 0, sizeof emit);
  /* only the live particles are drawn: they are packed at the front of the instance buffers each frame and
   * the meshes' counts cut to them (a full buffer of hidden cubes was 4,800 triangles a view) */
  cube_mesh->count = 0; puff_mesh->count = 0; set_drawn(cube_mesh, 0); set_drawn(puff_mesh, 0);
  flash_frames = 0; star_frames = 0; streak_frames = 0; for (int i = 0; i < 4; i++) { white_a[i] = 0; white_frames[i] = 0; }
}

/* the oldest cube is recycled over the cap (20.7) */
static cube_t *cube_alloc(void) {
  for (int k = 0; k < J_SPARK_CAP; k++) { cube_t *c = &cubes[(cube_next + k) % J_SPARK_CAP]; if (!c->live) { cube_next = (cube_next + k + 1) % J_SPARK_CAP; memset(c, 0, sizeof *c); c->live = 1; return c; } }
  cube_t *c = &cubes[cube_next]; cube_next = (cube_next + 1) % J_SPARK_CAP; memset(c, 0, sizeof *c); c->live = 1; return c;
}
static puff_t *puff_alloc(void) {
  for (int k = 0; k < J_SMOKE_CAP; k++) { puff_t *p = &puffs[(puff_next + k) % J_SMOKE_CAP]; if (!p->live) { puff_next = (puff_next + k + 1) % J_SMOKE_CAP; memset(p, 0, sizeof *p); p->live = 1; return p; } }
  puff_t *p = &puffs[puff_next]; puff_next = (puff_next + 1) % J_SMOKE_CAP; memset(p, 0, sizeof *p); p->live = 1; return p;
}

/* a cone of speed 4 to 9 u/s (times mul) round the axis (dx, dy, dz), half angle deg */
static void cone_axis(float dx, float dy, float dz, float deg, float mul, float *vx, float *vy, float *vz) {
  float l = sqrtf(dx * dx + dy * dy + dz * dz); if (l < 1e-4f) { dx = 0; dy = 1; dz = 0; l = 1; } dx /= l; dy /= l; dz /= l;
  float ang = deg * PI / 180 * rf(), rot = rf() * 2 * PI, sp = rr(J_SPARK_SPEED_MIN, J_SPARK_SPEED_MAX) * mul;
  /* an orthonormal frame round d */
  float ax = fabsf(dy) < 0.9f ? 0 : 1, ay = fabsf(dy) < 0.9f ? 1 : 0;
  float ux = ay * dz - 0 * dy, uy = 0 * dx - ax * dz, uz = ax * dy - ay * dx, ul = sqrtf(ux * ux + uy * uy + uz * uz); ux /= ul; uy /= ul; uz /= ul;
  float wx = dy * uz - dz * uy, wy = dz * ux - dx * uz, wz = dx * uy - dy * ux;
  float ca = cosf(ang), sa = sinf(ang), cr = cosf(rot), sr = sinf(rot);
  *vx = (dx * ca + (ux * cr + wx * sr) * sa) * sp;
  *vy = (dy * ca + (uy * cr + wy * sr) * sa) * sp;
  *vz = (dz * ca + (uz * cr + wz * sr) * sa) * sp;
}
/* the old cone: round the reflected normal (-n, up), half angle 50 degrees */
static void cone_velocity(float nx, float nz, float *vx, float *vy, float *vz) { cone_axis(-nx, 0.7f, -nz, J_SPARK_CONE_DEG, 1, vx, vy, vz); }

/* the cone a spawn uses: the reflected normal by default, or a set axis (fx_hit's spray) */
static int cone_set; static float cone_ax, cone_ay, cone_az, cone_deg, cone_mul;
static void spawn_sparks(float x, float y, float z, float nx, float nz, int n, uint32_t colour, int held, float ivx, float ivz, int kind, float life_mul) {
  for (int i = 0; i < n; i++) {
    cube_t *c = cube_alloc();
    c->x = x + rr(-0.05f, 0.05f); c->y = y + rr(-0.05f, 0.05f); c->z = z + rr(-0.05f, 0.05f);
    if (cone_set) cone_axis(cone_ax, cone_ay, cone_az, cone_deg, cone_mul, &c->hx, &c->hy, &c->hz);
    else cone_velocity(nx, nz, &c->hx, &c->hy, &c->hz);
    c->hx += ivx * J_SPARK_INHERIT / 4; c->hz += ivz * J_SPARK_INHERIT / 4;
    c->life = c->life0 = rr(J_SPARK_LIFE_MIN, J_SPARK_LIFE_MAX) * life_mul;
    c->size = kind == PK_CHIP ? rr(0.1f, 0.16f) : rr(J_SPARK_SIZE_MIN, J_SPARK_SIZE_MAX);
    c->colour = colour; c->kind = (uint8_t)kind; c->held = (uint8_t)held; c->spin = rr(-9, 9);
    if (!held) { c->vx = c->hx; c->vy = c->hy; c->vz = c->hz; }
  }
}

uint32_t fx_spark_colour(int armour, int red_tag, uint32_t *r) {
  if (red_tag) return C_RUST;
  switch (armour) {
    case AR_PLY: return C_BONE;
    case AR_ALU: case AR_UHMW: return C_WHITE;
    case AR_STEEL: return (rtr_rand(r) & 1) ? C_HAZARD : C_RUST;
    case AR_TI: { uint32_t c[3] = { C_BONE, C_LIVE, C_HAZARD }; return c[rtr_rand_n(r, 3)]; }
    case AR_WEAR: return C_RUST;
    default: return C_HAZARD;
  }
}

static void white_flash(int slot, int a) {
  if (slot < 0 || slot >= M.nbots) return;
  int vp = cam_viewport_of(M.bots[slot].seat);
  if (vp < 0 || vp > 3) return;
  if (a > white_a[vp]) white_a[vp] = a;
  if (white_frames[vp] < 1) white_frames[vp] = 1;
  white_new[vp] = 1;           /* set during the drain: this frame's fx_update must not spend it before the HUD draws it */
}

/* the weapon that made a hit: the event's, or (until the sim carries it) the attacker's build */
static int hit_weapon(const event_t *e, int attacker) {
  int w = e->weapon;
  if (!w && attacker >= 0) {
    const bot_t *a = &M.bots[attacker];
    if (a->build.drive == DR_MELTY) w = EVW_MELTY;
    else if (e->type == EV_RAM) w = a->build.weapon == WP_WEDGE ? WP_WEDGE : WP_NONE;
    else w = a->build.weapon;
  }
  return w;
}

void fx_hit(const event_t *e, int band) {
  if (band < 0 || band >= BAND_COUNT) return;
  const hit_band_t *hb = &HIT_BANDS[band];
  int victim = e->b < M.nbots ? e->b : -1, attacker = e->a < M.nbots ? e->a : -1;
  int armour = victim >= 0 ? M.bots[victim].build.armour : AR_NONE;
  int red = attacker >= 0 && M.bots[attacker].red_tag_t > 0;
  int held = M.hitstop > 0 || hb->stop > 0;
  float ivx = victim >= 0 ? M.bots[victim].b.vx : 0, ivz = victim >= 0 ? M.bots[victim].b.vz : 0;
  int n = hb->sparks;
  /* the spray by weapon (graphics 4.4): along the attacker's forward, along the weapon's tangent, or back
   * off the normal, plus up; a deflect off a wedge front goes up the slope, a KO goes straight up */
  int w = hit_weapon(e, attacker);
  const spark_spray_t *sp = &SPRAY[w >= 0 && w < WP_COUNT + 2 ? w : WP_NONE];
  float y = e->y;
  if (!e->weapon || y <= 0.02f) y = (victim >= 0 ? M.bots[victim].b.y : 0) + sp->y;
  float fx_ = 0, fz_ = 1;
  if (attacker >= 0) { fx_ = sinf(M.bots[attacker].b.h); fz_ = cosf(M.bots[attacker].b.h); }
  float other = 1 - sp->along - sp->tangent; if (other < 0) other = 0;
  cone_ax = sp->along * fx_ + sp->tangent * e->nz + other * -e->nx;
  cone_az = sp->along * fz_ + sp->tangent * -e->nx + other * -e->nz;
  cone_ay = sp->up; cone_deg = sp->cone; cone_mul = sp->speed; cone_set = 1;
  if (e->flags & EVF_DEFLECT) { cone_ax = fx_ * 0.6f; cone_az = fz_ * 0.6f; cone_ay = 0.9f; cone_deg = 30; }
  if (band == BAND_KO) { cone_ax = 0; cone_az = 0; cone_ay = 1; cone_deg = 70; cone_mul = 1; }
  if (band == BAND_CLASH) cone_set = 0;
  uint32_t sc = (e->flags & EVF_STRIPPED) ? C_BONE : 0;
  if (band == BAND_CLASH) { for (int i = 0; i < n; i++) spawn_sparks(e->x, y, e->z, e->nx, e->nz, 1, C_WHITE, held, 0, 0, PK_SPARK, 1); }
  else for (int i = 0; i < n; i++) spawn_sparks(e->x, y, e->z, e->nx, e->nz, 1, sc ? sc : fx_spark_colour(armour, red, &rng), held, ivx, ivz, PK_SPARK, 1);
  /* chips in the victim's panel colour; rubber off a wheel */
  if (hb->chips && victim >= 0) {
    uint32_t tints[AR_COUNT] = { C_HAZARD, 0xb08a55, 0xd7dadc, 0xe9e5dc, 0x6f6c66, 0x8d97a3, 0x4f3a2d };
    spawn_sparks(e->x, y, e->z, e->nx, e->nz, hb->chips, tints[armour % AR_COUNT], held, ivx, ivz, PK_CHIP, (float)J_DEBRIS_CHIP_LIFE / 60 / 0.42f);
  }
  if ((e->flags & EVF_WHEEL) && victim >= 0) spawn_sparks(e->x, y, e->z, e->nx, e->nz, 4, 0x1d1915, held, ivx, ivz, PK_CHIP, 2.0f);
  cone_set = 0;
  /* the flash: the warm disc at half its old size behind the star, which is the visible flash */
  if (hb->flash > 0 || band == BAND_KO) {
    flash_x = e->x; flash_y = y > 0.05f ? y : 0.35f; flash_z = e->z; flash_size = (hb->flash > 0 ? hb->flash : 1) * 0.28f; flash_frames = FLASH_FRAMES;
    if (band >= BAND_HIT) { star_size = band == BAND_KO ? J_STAR_KO_SIZE : hb->flash * J_STAR_SIZE; star_frames = band == BAND_KO ? J_STAR_KO_FRAMES : J_STAR_FRAMES; }
  }
  if (band == BAND_HEAVY) white_flash(victim, J_BAND_HEAVY_WHITE_A);
  if (band == BAND_HUGE) white_flash(victim, J_BAND_HUGE_WHITE_A);
  if (band == BAND_CLASH) { white_flash(victim, J_BAND_CLASH_WHITE_A); white_flash(attacker, J_BAND_CLASH_WHITE_A); }
  if (band == BAND_KO) {
    white_flash(victim, J_BAND_CLASH_WHITE_A);
    { int vp = victim >= 0 ? cam_viewport_of(M.bots[victim].seat) : -1; if (vp >= 0 && vp < 4) white_frames[vp] = J_BAND_KO_WHITE_FRAMES; }
    /* the burst: smoke to black from the frame */
    for (int i = 0; i < 8; i++) { puff_t *p = puff_alloc(); p->x = e->x + rr(-0.3f, 0.3f); p->y = 0.3f; p->z = e->z + rr(-0.3f, 0.3f); p->life = p->life0 = J_SMOKE_LIFE / 60.0f * 1.5f; p->size0 = 0.3f; p->size1 = 0.7f; p->rise = J_SMOKE_RISE; p->colour = 0x1d1915; p->roll = rf() * 6.28f; }
  }
}

void fx_sparks(float x, float y, float z, float nx, float nz, int n, uint32_t colour) { spawn_sparks(x, y, z, nx, nz, n, colour, 0, 0, 0, PK_SPARK, 1); }

void fx_dust(float x, float y, float z, int n) {
  for (int i = 0; i < n; i++) {
    cube_t *c = cube_alloc();
    c->x = x + rr(-0.3f, 0.3f); c->y = y + 0.05f; c->z = z + rr(-0.3f, 0.3f);
    float a = rf() * 2 * PI, sp = rr(0.6f, 1.8f);
    c->vx = cosf(a) * sp; c->vz = sinf(a) * sp; c->vy = rr(0.6f, 1.6f);
    c->life = c->life0 = rr(0.4f, 0.7f); c->size = rr(0.08f, 0.16f); c->colour = (i & 1) ? C_ASH : 0x8a857c; c->kind = PK_DUST; c->spin = rr(-3, 3);
  }
}

void fx_confetti(float x, float y, float z, int n, uint32_t colour) {
  for (int i = 0; i < n; i++) {
    cube_t *c = cube_alloc();
    c->x = x + rr(-0.2f, 0.2f); c->y = y; c->z = z + rr(-0.2f, 0.2f);
    float a = rf() * 2 * PI, sp = rr(0.8f, 2.2f);
    c->vx = cosf(a) * sp; c->vz = sinf(a) * sp; c->vy = rr(2.5f, 4.5f);
    c->life = c->life0 = rr(1.2f, 1.8f); c->size = 0.09f; c->colour = colour; c->kind = PK_CONFETTI; c->spin = rr(-12, 12);
  }
}

static void rise_cubes(float x, float y, float z, int n, uint32_t colour) {
  for (int i = 0; i < n; i++) {
    cube_t *c = cube_alloc();
    c->x = x + rr(-0.25f, 0.25f); c->y = y + rr(0, 0.2f); c->z = z + rr(-0.25f, 0.25f);
    c->vy = rr(1.2f, 2.2f); c->vx = rr(-0.3f, 0.3f); c->vz = rr(-0.3f, 0.3f);
    c->life = c->life0 = rr(0.5f, 0.8f); c->size = 0.1f; c->colour = colour; c->kind = PK_RISE; c->spin = rr(-4, 4);
  }
}

static int haz_type_of(const match_t *m, int idx) {
  if (!m || m->arena >= ARENA_COUNT || idx < 0 || idx >= ARENAS[m->arena].nhaz) return HZ_NONE;
  return ARENAS[m->arena].haz[idx].type;
}

void fx_event(const event_t *e, const match_t *m) {
  int a = e->a < MAX_BOTS ? e->a : 0;
  uint32_t seatc = (m && a < m->nbots && m->bots[a].seat < 4) ? SEAT_RGB[m->bots[a].seat] : C_BONE;
  switch (e->type) {
    case EV_LAUNCH: fx_dust(e->x, 0, e->z, J_LAUNCH_DUST); break;
    case EV_LAND: fx_dust(e->x, 0, e->z, J_LANDING_DUST); break;
    case EV_WALL: fx_sparks(e->x, e->y > 0 ? e->y : 0.25f, e->z, -e->nx, -e->nz, J_WALL_SPARKS, C_HAZARD); break;
    case EV_SPIN_WALL: {
      /* along the wall: the tangent of the normal */
      for (int i = 0; i < J_SPINWALL_SPARKS; i++) { float t = rr(-0.8f, 0.8f); fx_sparks(e->x + e->nz * t, 0.2f + rr(0, 0.2f), e->z - e->nx * t, -e->nx, -e->nz, 1, (i & 1) ? C_WHITE : C_HAZARD); }
      break; }
    case EV_SHED: fx_sparks(e->x, e->y > 0 ? e->y : 0.3f, e->z, e->nx, e->nz, 8, C_HAZARD); break;
    case EV_BURST: {
      fx_sparks(e->x, 0.3f, e->z, 0, 1, 20, C_HAZARD); fx_sparks(e->x, 0.3f, e->z, 0, -1, 20, C_RUST);
      for (int i = 0; i < 8; i++) { puff_t *p = puff_alloc(); p->x = e->x + rr(-0.3f, 0.3f); p->y = 0.3f; p->z = e->z + rr(-0.3f, 0.3f); p->life = p->life0 = J_SMOKE_LIFE / 60.0f * 1.5f; p->size0 = 0.3f; p->size1 = 0.7f; p->rise = J_SMOKE_RISE; p->colour = 0x1d1915; p->roll = rf() * 6.28f; }
      break; }
    case EV_RED_TAG: fx_sparks(e->x, 0.4f, e->z, 0, 1, J_REDTAG_SPARKS / 2, C_RUST); fx_sparks(e->x, 0.4f, e->z, 0, -1, J_REDTAG_SPARKS / 2, C_RUST); break;
    case EV_PICKUP: rise_cubes(e->x, 0.1f, e->z, J_PICKUP_CUBES, C_LIVE); break;
    case EV_NOT_VACUUM: fx_sparks(e->x, 0.2f, e->z, 0, 1, 6, C_HAZARD); break;
    case EV_TAUNT: fx_confetti(e->x, 0.6f, e->z, J_TAUNT_CONFETTI, seatc); break;
    case EV_WEAPON_TELL: break;
    case EV_BENCH_BREAK: for (int i = 0; i < 6; i++) { cube_t *c = cube_alloc(); c->x = e->x + rr(-0.5f, 0.5f); c->y = 0.4f; c->z = e->z + rr(-0.3f, 0.3f); c->vx = rr(-2, 2); c->vz = rr(-2, 2); c->vy = rr(2, 4); c->life = c->life0 = 1.5f; c->size = rr(0.12f, 0.2f); c->colour = 0xa57c4f; c->kind = PK_CHIP; c->spin = rr(-8, 8); } break;
    case EV_BUMPER_BREAK: for (int i = 0; i < 6; i++) { cube_t *c = cube_alloc(); c->x = e->x; c->y = 0.6f; c->z = e->z; c->vx = rr(-2, 2); c->vz = rr(-2, 2); c->vy = rr(2, 4); c->life = c->life0 = 1.5f; c->size = rr(0.1f, 0.18f); c->colour = (i & 1) ? C_HAZARD : C_TAPE; c->kind = PK_CHIP; c->spin = rr(-8, 8); } break;
    case EV_HAZ_HIT: {
      int ht = haz_type_of(m, e->b);
      if (ht == HZ_FLAME) for (int i = 0; i < 12; i++) { cube_t *c = cube_alloc(); c->x = e->x + rr(-0.3f, 0.3f); c->y = 0.2f; c->z = e->z + rr(-0.3f, 0.3f); c->vy = rr(1.5f, 3); c->life = c->life0 = 0.5f; c->size = 0.14f; c->colour = (i & 1) ? C_HAZARD : C_RUST; c->kind = PK_FIRE; }
      else if (ht == HZ_PULVERIZER || ht == HZ_DROPZONE || ht == HZ_FLOORFLIP) fx_dust(e->x, 0, e->z, 16);
      else fx_sparks(e->x, 0.15f, e->z, e->nx, e->nz, 20, C_HAZARD);
      break; }
    case EV_HAZ_TELL: break;
    default: break;
  }
}

void fx_bot_damage_state(int slot, float hp, int burning, float x, float y, float z) {
  fx_bot_damage_frac(slot, hp / 100.0f, burning, x, y, z, -1);
}

void fx_bot_damage_frac(int slot, float hp_frac, int burning, float x, float y, float z, int side) {
  if (slot < 0 || slot >= MAX_BOTS) return;
  emit[slot].frac = hp_frac; emit[slot].x = x; emit[slot].y = y; emit[slot].z = z; emit[slot].seen = 1; emit[slot].burning = burning; emit[slot].side = side;
}

static void emit_puff(int i, float spread, float size0, float size1, float rise, float life, uint32_t colour) {
  puff_t *p = puff_alloc();
  p->x = emit[i].x + rr(-spread, spread); p->y = emit[i].y; p->z = emit[i].z + rr(-spread, spread);
  p->life = p->life0 = life; p->size0 = size0; p->size1 = size1; p->rise = rise;
  p->colour = colour; p->roll = rf() * 6.28f;
}
static void emit_fire(int i, float spread, float size) {
  cube_t *c = cube_alloc();
  c->x = emit[i].x + rr(-spread, spread); c->y = emit[i].y - 0.05f; c->z = emit[i].z + rr(-spread, spread);
  c->vy = J_FIRE_RISE; c->vx = rr(-0.2f, 0.2f); c->vz = rr(-0.2f, 0.2f);
  c->life = c->life0 = J_FIRE_LIFE / 60.0f; c->size = size * rr(0.7f, 1.3f);
  c->colour = rtr_rand_n(&rng, 3) == 0 ? 0x1d1915 : C_HAZARD;   /* one in three an ember */
  c->kind = PK_FIRE; c->spin = rr(-6, 6);
}

/* smoke and fire by the fraction of max HP (graphics 4.5): grey puffs from 0.40, char puffs and fire from
 * 0.20, out of the most damaged side; a burning bot keeps its fire; a KO frame (fraction 0) smokes in a
 * column for 3 s, burns, then settles to an ember */
static void emitters(float dt) {
  for (int i = 0; i < MAX_BOTS; i++) {
    if (!emit[i].seen) continue;
    emit[i].seen = 0;
    float f = emit[i].frac;
    if (f <= 0) {
      emit[i].dead_t += dt;
      float t = emit[i].dead_t;
      if (t < J_KO_SMOKE_S) { emit[i].acc_s += dt * J_KO_SMOKE_RATE; while (emit[i].acc_s >= 1) { emit[i].acc_s -= 1; emit_puff(i, 0.2f, 0.25f, 0.7f, J_SMOKE_RISE * 1.5f, J_SMOKE_LIFE / 60.0f * 1.5f, 0x1d1915); } }
      emit[i].acc_f += dt * (t < J_KO_FIRE_S ? J_FIRE_RATE_20 : J_KO_EMBER_RATE);
      while (emit[i].acc_f >= 1) { emit[i].acc_f -= 1; emit_fire(i, 0.2f, t < J_KO_FIRE_S ? 0.14f : 0.08f); }
      continue;
    }
    emit[i].dead_t = 0;
    int black = f <= J_SMOKE_FRAC_CHAR;
    if (f <= J_SMOKE_FRAC_GREY) {
      emit[i].acc_s += dt * (black ? J_SMOKE_RATE_20 : J_SMOKE_RATE_40);
      while (emit[i].acc_s >= 1) { emit[i].acc_s -= 1; emit_puff(i, 0.15f, 0.2f, 0.45f, J_SMOKE_RISE, J_SMOKE_LIFE / 60.0f, black ? 0x1d1915 : 0x8a857c); }
    }
    if (black || emit[i].burning) {
      emit[i].acc_f += dt * J_FIRE_RATE_20;
      while (emit[i].acc_f >= 1) { emit[i].acc_f -= 1; emit_fire(i, 0.15f, 0.12f); }
    }
  }
}

static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t) {
  int r = (int)(((a >> 16) & 255) + ((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t);
  int g = (int)(((a >> 8) & 255) + ((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t);
  int bl = (int)((a & 255) + ((int)(b & 255) - (int)(a & 255)) * t);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

void fx_update(float dt, int frozen) {
  if (dt <= 0) dt = 1.f / 60; if (dt > 0.1f) dt = 0.1f;
  for (int v = 0; v < 4; v++) { if (white_new[v]) white_new[v] = 0; else if (white_frames[v] > 0) white_frames[v]--; if (!white_frames[v]) white_a[v] = 0; }
  /* the flash is one full frame then three shrinking ones, on wall time whatever the hit stop does */
  if (flash_frames > 0) { if (flash_frames < FLASH_FRAMES) flash_size *= 0.6f; flash_frames--; }
  if (star_frames > 0) star_frames--;
  if (streak_frames > 0) streak_frames--;
  emitters(dt);
  int nd = 0;
  for (int i = 0; i < J_SPARK_CAP; i++) {
    cube_t *c = &cubes[i];
    if (!c->live) continue;
    if (c->held) {
      if (frozen) { /* drawn at the contact, not integrated */ }
      else { c->held = 0; c->vx = c->hx; c->vy = c->hy; c->vz = c->hz; }
    }
    if (!c->held) {
      switch (c->kind) {
        case PK_TRAIL: if (c->free_t > 0) { c->free_t--; break; } c->vy -= J_SPARK_GRAVITY * dt; break;
        case PK_SPARK: case PK_CHIP: case PK_DUST:
          c->vy -= (c->kind == PK_DUST ? 3.0f : J_SPARK_GRAVITY) * dt;
          if (c->kind == PK_DUST) { c->vx *= 1 - 2 * dt; c->vz *= 1 - 2 * dt; }
          break;
        case PK_CONFETTI: c->vy -= 2.5f * dt; c->vx *= 1 - 1.5f * dt; c->vz *= 1 - 1.5f * dt; c->vx += sinf(c->life * 9 + i) * 0.4f * dt * 10; break;
        case PK_FIRE: c->vy = J_FIRE_RISE; break;
        case PK_RISE: break;
        default: break;
      }
      c->x += c->vx * dt; c->y += c->vy * dt; c->z += c->vz * dt;
      if (c->y < 0.02f && c->vy < 0) {
        if (c->kind == PK_SPARK && !c->bounced) { c->vy = -c->vy * J_SPARK_BOUNCE; c->vx *= 0.7f; c->vz *= 0.7f; c->bounced = 1; c->y = 0.02f; }
        else if (c->kind == PK_CONFETTI || c->kind == PK_CHIP || c->kind == PK_DUST) { c->y = 0.02f; c->vy = 0; c->vx *= 0.8f; c->vz *= 0.8f; }
        else { c->y = 0.02f; c->vy = 0; }
      }
      c->life -= dt;
    }
    if (c->life <= 0) { c->live = 0; continue; }
    float k = c->life / c->life0, s = c->size;
    uint32_t col = c->colour;
    if (c->kind == PK_SPARK) s *= 0.4f + 0.6f * k;
    if (c->kind == PK_FIRE) { s *= 0.5f + 0.5f * k; col = lerp_rgb(C_RUST, C_HAZARD, k); }
    if (c->kind == PK_RISE || c->kind == PK_DUST) s *= k < 0.3f ? k / 0.3f : 1;
    if (c->kind == PK_CHIP && k < 0.25f) s *= k / 0.25f;
    t3_mat4 m; t3_quat q = t3_quat_from_axis_angle((t3_vec3){ 0.6f, 0.8f, 0 }, c->spin * (c->life0 - c->life) + i * 0.7f);
    t3_vec3 sc = c->kind == PK_CONFETTI ? (t3_vec3){ s * 1.4f, s * 0.2f, s * 1.0f } : (t3_vec3){ s, s, s };
    if (c->kind == PK_TRAIL) s *= 0.5f + 0.5f * k;
    if (c->kind == PK_SPARK || c->kind == PK_TRAIL) {
      /* a spark is a streak along its own velocity (the release velocity while it is held at the contact) */
      float vx = c->held ? c->hx : c->vx, vy = c->held ? c->hy : c->vy, vz = c->held ? c->hz : c->vz;
      float sp = sqrtf(vx * vx + vy * vy + vz * vz);
      if (sp > 0.01f) {
        float dx = vx / sp, dy = vy / sp, dz = vz / sp;
        float ax = -dy, ay = dx, al = sqrtf(ax * ax + ay * ay);       /* z cross d */
        float ang = acosf(dz > 1 ? 1 : dz < -1 ? -1 : dz);
        q = al > 0.0001f ? t3_quat_from_axis_angle((t3_vec3){ ax / al, ay / al, 0 }, ang) : t3_quat_identity();
      }
      float len = 1.6f + sp * 0.25f; if (len > 4) len = 4;
      sc = (t3_vec3){ s * 0.55f, s * 0.55f, s * len };
    }
    t3_mat4_compose(&m, (t3_vec3){ c->x, c->y, c->z }, q, sc);
    t3_instanced_mesh_set_matrix_at(cube_mesh, nd, &m);
    t3_instanced_mesh_set_color_at(cube_mesh, nd, t3_color_hex(col));
    nd++;
  }
  set_drawn(cube_mesh, nd);
  int np = 0;
  /* smoke runs on wall time and never freezes; a puff grows, drifts up, then shrinks away over its
   * last third (the instanced alpha is global, so the fade is in size and in a drift toward the floor tone) */
  for (int i = 0; i < J_SMOKE_CAP; i++) {
    puff_t *p = &puffs[i];
    if (!p->live) continue;
    p->life -= dt; p->y += p->rise * dt;
    if (p->life <= 0) { p->live = 0; continue; }
    float u = 1 - p->life / p->life0, s = p->size0 + (p->size1 - p->size0) * u;
    if (u > 0.66f) s *= 1 - (u - 0.66f) / 0.34f;
    p->size = s;
    p->slot = (int16_t)np;
    t3_instanced_mesh_set_color_at(puff_mesh, np++, t3_color_hex(lerp_rgb(p->colour, C_SMOKE, u * 0.5f)));
  }
  set_drawn(puff_mesh, np);
  /* the instance buffers re-upload only on a version bump (three.js needsUpdate) */
  cube_mesh->instance_matrix->version++; if (cube_mesh->instance_color) cube_mesh->instance_color->version++;
  if (puff_mesh->instance_color) puff_mesh->instance_color->version++;
}

/* the puffs and the flash face viewport i's camera: written per viewport, just before it renders */
void fx_face(int viewport) {
  for (int i = 0; i < 4; i++) if (flash[i]) flash[i]->base.visible = false;
  if (viewport < 0 || viewport > 3) return;
  t3_camera *c = cam_get(viewport);
  if (!c) return;
  t3_quat cq = c->base.quaternion;
  for (int i = 0; i < J_SMOKE_CAP; i++) {
    puff_t *p = &puffs[i];
    if (!p->live) continue;
    float s = p->size;
    t3_mat4 m; t3_quat q = t3_quat_multiply(cq, t3_quat_from_axis_angle((t3_vec3){ 0, 0, 1 }, p->roll + (p->life0 - p->life) * 0.6f));
    t3_mat4_compose(&m, (t3_vec3){ p->x, p->y + s * 0.3f, p->z }, q, (t3_vec3){ s, s * 0.85f, 1 });
    t3_instanced_mesh_set_matrix_at(puff_mesh, p->slot, &m);
  }
  puff_mesh->instance_matrix->version++;
  t3_vec3 cpos = t3_object_get_world_position(c);
  for (int i = 0; i < 4; i++) if (star[i]) star[i]->base.visible = false;
  if (streak) streak->base.visible = false;
  /* the star faces the camera at the contact, spun a little each frame so it never reads as a sprite */
  if (star_frames > 0 && star[viewport]) {
    t3_mesh *st = star[viewport];
    t3_object_set_position(st, flash_x, flash_y, flash_z);
    t3_object_look_at(st, cpos.x, cpos.y, cpos.z);
    t3_object_set_quaternion(st, t3_quat_multiply(st->base.quaternion, t3_quat_from_axis_angle((t3_vec3){ 0, 0, 1 }, star_frames * 0.4f)));
    t3_object_set_scale(st, star_size, star_size, 1);
    st->base.visible = true;
  }
  /* the launch streak: a long quad along the launch, turned about its axis to face the camera */
  if (streak_frames > 0 && streak) {
    float len = 1.2f, cx = streak_x + streak_dx * len * 0.5f, cy = streak_y + streak_dy * len * 0.5f, cz = streak_z + streak_dz * len * 0.5f;
    t3_vec3 d = { streak_dx, streak_dy, streak_dz }, tc = { cpos.x - cx, cpos.y - cy, cpos.z - cz };
    t3_vec3 side = { d.y * tc.z - d.z * tc.y, d.z * tc.x - d.x * tc.z, d.x * tc.y - d.y * tc.x };
    float sl = sqrtf(side.x * side.x + side.y * side.y + side.z * side.z);
    if (sl > 1e-4f) {
      side.x /= sl; side.y /= sl; side.z /= sl;
      t3_vec3 nrm = { side.y * d.z - side.z * d.y, side.z * d.x - side.x * d.z, side.x * d.y - side.y * d.x };
      t3_mat4 m; float e[16] = { side.x * 0.12f, side.y * 0.12f, side.z * 0.12f, 0, d.x * len, d.y * len, d.z * len, 0, nrm.x, nrm.y, nrm.z, 0, cx, cy, cz, 1 };
      memcpy(m.e, e, sizeof e);
      t3_vec3 pos, scl; t3_quat q; t3_mat4_decompose(&m, &pos, &q, &scl);
      t3_object_set_position(streak, pos.x, pos.y, pos.z); t3_object_set_quaternion(streak, q); t3_object_set_scale(streak, scl.x, scl.y, scl.z);
      streak->materials[0]->opacity = 0.6f * streak_frames / J_STREAK_FRAMES;
      streak->base.visible = true;
    }
  }
  if (flash_frames <= 0) return;
  t3_mesh *f = flash[viewport];
  t3_object_set_position(f, flash_x, flash_y, flash_z);
  t3_object_set_scale(f, flash_size, flash_size, 1);
  t3_object_look_at(f, cpos.x, cpos.y, cpos.z);
  f->base.visible = true;
}

/* the boot compile: one spark, one chip, one puff and the flash quads on screen so their programs link
 * before the first fight; off hides them again */
void fx_warm(int on) {
  if (on) {
    t3_mat4 m; t3_mat4_compose(&m, (t3_vec3){ 0, 0.5f, 0 }, t3_quat_identity(), (t3_vec3){ 0.1f, 0.1f, 0.2f });
    t3_instanced_mesh_set_matrix_at(cube_mesh, 0, &m); t3_instanced_mesh_set_color_at(cube_mesh, 0, t3_color_hex(C_HAZARD));
    t3_instanced_mesh_set_matrix_at(puff_mesh, 0, &m); t3_instanced_mesh_set_color_at(puff_mesh, 0, t3_color_hex(0x8a857c));
    set_drawn(cube_mesh, 1); set_drawn(puff_mesh, 1);
    for (int i = 0; i < 4; i++) { t3_object_set_position(flash[i], 0, 0.5f, 0); t3_object_set_scale(flash[i], 0.3f, 0.3f, 1); flash[i]->base.visible = true; t3_object_set_position(star[i], 0, 0.5f, 0); star[i]->base.visible = true; }
    t3_object_set_position(streak, 0, 0.5f, 0); streak->base.visible = true;
  } else {
    set_drawn(cube_mesh, 0); set_drawn(puff_mesh, 0);
    for (int i = 0; i < 4; i++) { flash[i]->base.visible = false; star[i]->base.visible = false; }
    streak->base.visible = false;
  }
  cube_mesh->instance_matrix->version++; if (cube_mesh->instance_color) cube_mesh->instance_color->version++;
  puff_mesh->instance_matrix->version++; if (puff_mesh->instance_color) puff_mesh->instance_color->version++;
}

int fx_white_alpha(int viewport) { return viewport >= 0 && viewport < 4 && white_frames[viewport] > 0 ? white_a[viewport] : 0; }
int fx_live_sparks(void) { int n = 0; for (int i = 0; i < J_SPARK_CAP; i++) n += cubes[i].live; return n; }

void fx_flame(float x, float y, float z, float dx, float dz, float dt) {
  static float acc;
  acc += dt * 12;
  while (acc >= 1) {
    acc -= 1;
    cube_t *c = cube_alloc();
    c->x = x + rr(-0.1f, 0.1f); c->y = y + rr(-0.05f, 0.08f); c->z = z + rr(-0.1f, 0.1f);
    c->vx = dx * 3 + rr(-0.4f, 0.4f); c->vz = dz * 3 + rr(-0.4f, 0.4f); c->vy = rr(0.2f, 0.8f);
    c->life = c->life0 = rr(0.25f, 0.4f); c->size = rr(0.1f, 0.16f); c->colour = C_HAZARD; c->kind = PK_FIRE; c->spin = rr(-6, 6);
  }
}

/* a launch (graphics 4.9): eight seat colour trail cubes along the flier's velocity that fly straight for 8
 * frames before gravity takes them, and a bone streak at the plate tip along the launch */
void fx_launch(int victim_slot, float x, float y, float z, float vx, float vy, float vz, uint32_t seat_colour) {
  (void)victim_slot;
  float sp = sqrtf(vx * vx + vy * vy + vz * vz);
  if (sp < 0.5f) { vx = 0; vy = 7; vz = 0; sp = 7; }
  for (int i = 0; i < J_TRAIL_CUBES; i++) {
    cube_t *c = cube_alloc();
    float k = rr(0.3f, 1.0f);
    c->x = x + rr(-0.15f, 0.15f); c->y = (y > 0.05f ? y : 0.2f) + rr(0, 0.2f); c->z = z + rr(-0.15f, 0.15f);
    c->vx = vx * k + rr(-0.6f, 0.6f); c->vy = vy * k; c->vz = vz * k + rr(-0.6f, 0.6f);
    c->life = c->life0 = J_TRAIL_LIFE * rr(0.8f, 1.2f); c->size = 0.06f * 1.6f; c->colour = seat_colour; c->kind = PK_TRAIL; c->free_t = J_TRAIL_FREE_FRAMES;
  }
  streak_x = x; streak_y = y > 0.05f ? y : 0.2f; streak_z = z; streak_dx = vx / sp; streak_dy = vy / sp; streak_dz = vz / sp; streak_frames = J_STREAK_FRAMES;
}
