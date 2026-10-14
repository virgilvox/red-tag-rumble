/* sim.c: the deterministic 60 Hz match simulation (spec section 4). Owned by
 * the sim agent (items 3, 8, 13, 16, 20, 24). Bodies, the trig table, the
 * drive model, walls and solids, bot vs bot collision and push, the weapon
 * state machines and hit rules, the height channel, hazards, pits and edges,
 * count-out, damage thresholds and shed panels, pickups, RED TAG, OPS, the
 * hill, the win conditions and the judges tallies.
 *
 * No three.c, no HUD, no host, no libm trig: sqrtf, fmodf and floorf only.
 * Every pairwise loop runs in fixed order so the native balance tool and the
 * wasm cart compute the same floats; -ffp-contract=off on both builds.
 *
 * Step order (4.4) is the body of sim_step at the bottom of this file. */
#include "sim.h"
#include "bot.h"
#include "arena.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define PI_F 3.14159265f
#define GRAVITY 20.0f
/* spin spent per landed hit (not in the spec tables; tuned with balance --robin so one approach
 * is one hit and then a visible spin up, not an 18 frame machine gun). A HORIZONTAL and a MELTY
 * spend everything: the disc stops dead on the victim and needs its full spin up (2 s and 1.5 s),
 * which is the spun down window of 4.7; a HORIZONTAL keeps 0.3 (0.4 s to a half strength hit, 1.4 s
 * to full), a DRUM 0.4 */
#ifndef SPIN_BLEED_HORIZ
#define SPIN_BLEED_HORIZ 0.7f
#endif
#ifndef SPIN_BLEED_DRUM
#define SPIN_BLEED_DRUM  0.6f
#endif
#ifndef SPIN_BLEED_MELTY
#define SPIN_BLEED_MELTY 1.0f
#endif
/* a ram is an event, not a drumbeat: one per victim per RAM_CD frames (the spec's 30 let a wedge
 * bounce a bot ten times in ten seconds); RAM_BASE plus RAM_BASE by closing speed (the spec's 10 + 10) */
#ifndef RAM_CD
#define RAM_CD 90
#endif
#ifndef RAM_BASE
#define RAM_BASE 9.0f
#endif
/* landing damage after a flipper launch (the spec's 6; a notch down with the rest of the table) */
#ifndef FLIP_LAND_DMG
#define FLIP_LAND_DMG 5.0f
#endif
/* a MELTY edge hit re-arms after MELTY_HIT_CD frames on top of its spin up; a clash costs each disc
 * CLASH_FRAC of the hit it would have dealt and the pair cannot clash again for CLASH_CD frames */
#ifndef MELTY_HIT_CD
#define MELTY_HIT_CD 90
#endif
/* the ram's closing speed floor (the spec's 2.0 let a TREADS wedge at 3.8 u/s push a backing bot all day
 * without a single ram) */
#ifndef RAM_MIN_CLOSING
#define RAM_MIN_CLOSING 1.5f
#endif
#ifndef CLASH_FRAC
#define CLASH_FRAC 0.4f
#endif
#ifndef CLASH_CD
#define CLASH_CD 90
#endif
/* weapon tip speeds for the closing term of 4.7 (units per second at full spin or mid swing). A
 * full spin hit on a standing target closes at about 0.9, a charge reaches the CLOSING_MAX clamp,
 * and a tap weapon swings at 1.0 instead of the 0.4 floor it used to get with no tip at all */
#ifndef TIP_HORIZ
#define TIP_HORIZ   3.6f
#endif
#ifndef TIP_DRUM
#define TIP_DRUM    3.0f
#endif
#ifndef MELTY_TIP
#define MELTY_TIP   3.0f
#endif
#define TIP_SWING   4.0f
#define CLOSING_MIN 0.4f
#define CLOSING_MAX 1.3f
#define SPINNER_HIT_CD 30
/* frames a HOVER's skirt stays pinched after a bot pressed on it (drive_step, bots_collide) */
#define SKIRT_FRAMES 24
/* a HOVER has thrust but no brakes: the part of its drive that fights the current velocity (slowing,
 * reversing, turning across it) gets this share of the accel, so it drifts through turns and a slower
 * bot that reads the drift can meet it. MELTY keeps its skids */
#ifndef HOVER_BRAKE
#define HOVER_BRAKE 0.3f
#endif
/* a HORIZONTAL or DRUM hits only at this much spin (the spec's 0.3 let a spent disc keep tapping
 * for 1 to 4 damage every half second; a hit spends SPIN_BLEED, so one approach is one hit and the
 * next needs a visible spin up). The spun down zone of 4.7 still opens under 0.3 */
#ifndef SPINNER_HIT_MIN
#define SPINNER_HIT_MIN 0.5f
#endif

/* -- the trig table (4.2) ----------------------------------------- */
/* 1024 entries over a full turn, built once with a double precision series so
 * the native balance tool and the wasm cart hold identical floats. */
#define TRIG_N 1024
static float trig_tab[TRIG_N + 1];
static int trig_ready;

static double series_sine(double x) {
  /* x in [0, pi/2]; odd series to x^21, error well under float precision */
  double x2 = x * x, term = x, sum = x;
  for (int k = 1; k <= 10; k++) { term *= -x2 / ((2.0 * k) * (2.0 * k + 1.0)); sum += term; }
  return sum;
}

static void trig_init(void) {
  const double TWO_PI = 6.283185307179586;
  for (int i = 0; i <= TRIG_N; i++) {
    int q = (i % TRIG_N) / (TRIG_N / 4), r = i % (TRIG_N / 4);
    double a = TWO_PI * (double)r / (double)TRIG_N;        /* [0, pi/2) */
    double s = series_sine(a), c = series_sine(1.5707963267948966 - a);
    double v = q == 0 ? s : q == 1 ? c : q == 2 ? -s : -c;
    trig_tab[i] = (float)v;
  }
  trig_ready = 1;
}

static float trig_lookup(float turns) {
  /* turns is the angle in units of a full turn, any range */
  if (!trig_ready) trig_init();
  float f = turns - floorf(turns);
  float p = f * (float)TRIG_N;
  int i = (int)p;
  float t = p - (float)i;
  if (i >= TRIG_N) i = TRIG_N - 1;
  return trig_tab[i] + (trig_tab[i + 1] - trig_tab[i]) * t;
}

float rtr_sin(float a) { return trig_lookup(a * (1.0f / 6.2831853f)); }
float rtr_cos(float a) { return trig_lookup(a * (1.0f / 6.2831853f) + 0.25f); }

/* odd polynomial on [0, 1] (max error about 1e-5 rad), octant reduction outside */
static float atan_unit(float x) {
  float x2 = x * x;
  return x * (0.99997726f + x2 * (-0.33262347f + x2 * (0.19354346f + x2 * (-0.11643287f + x2 * (0.05265332f + x2 * -0.01172120f)))));
}

float rtr_atan2(float y, float x) {
  const float HALF = 1.57079633f;
  float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
  if (ax == 0 && ay == 0) return 0;
  float r;
  if (ay <= ax) r = atan_unit(ay / ax);
  else r = HALF - atan_unit(ax / ay);
  if (x < 0) r = PI_F - r;
  return y < 0 ? -r : r;
}

/* -- small helpers ------------------------------------------------ */
static inline float fabs_f(float v) { return v < 0 ? -v : v; }
static inline float fmin_f(float a, float b) { return a < b ? a : b; }
static inline float fmax_f(float a, float b) { return a > b ? a : b; }
static inline float len2(float x, float z) { return sqrtf(x * x + z * z); }
static inline float dotp(float ax, float az, float bx, float bz) { return ax * bx + az * bz; }
/* heading 0 is plus z: forward (sin h, cos h). right_of is the body's local +x axis (cos h, minus
 * sin h), the axis three.c's rotation.y = h maps local x onto. In the y up right handed world that
 * axis lies on the bot's LEFT as seen from above and on screen, so a right turn is a heading
 * DECREASE: drive_step applies the stick with that sign and the AI, OPS and the balance driver
 * write their sticks against it. The collision code only needs an axis and does not care. */
static inline void fwd_of(float h, float *fx, float *fz) { *fx = rtr_sin(h); *fz = rtr_cos(h); }
static inline void right_of(float h, float *rx, float *rz) { *rx = rtr_cos(h); *rz = -rtr_sin(h); }
static inline float rnd01(match_t *m) { return rtr_rand_f(&m->rng); }
static inline int rnd_n(match_t *m, int n) { return rtr_rand_n(&m->rng, n); }

const arena_def_t *sim_arena(const match_t *m) { return &ARENAS[m->arena < ARENA_COUNT ? m->arena : 0]; }

int sim_is_1v1(const match_t *m) {
  int n = 0;
  for (int i = 0; i < m->nbots; i++) if (!m->bots[i].is_ops && m->bots[i].hp_max > 0) n++;
  return n == 2;
}

/* -- events ------------------------------------------------------- */
void sim_event(match_t *m, const event_t *e) {
  if (m->nev >= MAX_EVENTS) return;
  m->ev[m->nev++] = *e;
}

static void ev_simple(match_t *m, int type, int a, int b, int zone, float x, float z, float y) {
  event_t e = { (uint8_t)type, (uint8_t)a, (uint8_t)b, (uint8_t)zone, x, z, y, 0, 0, 0 };
  sim_event(m, &e);
}

static void ev_hit(match_t *m, int type, int a, int b, float zone, float x, float z, float y, float dmg, float nx, float nz) {
  event_t e = { (uint8_t)type, (uint8_t)a, (uint8_t)b, (uint8_t)(zone * 10.0f + 0.5f), x, z, y, dmg, nx, nz };
  sim_event(m, &e);
}

/* -- shapes and collision primitives (4.5) ------------------------ */
typedef struct { float x, z, h, hl, hw; uint8_t circle; } shape_t;
/* a contact: normal from a to b, penetration depth, contact point */
typedef struct { float nx, nz, pen, cx, cz; } contact_t;

static void shape_of(const bot_t *b, shape_t *s) {
  s->x = b->b.x; s->z = b->b.z; s->h = b->b.h; s->hl = b->b.hl; s->hw = b->b.hw; s->circle = b->b.shape == SH_CIRCLE;
  /* a spinning MELTY is a circle of r 0.5 (4.5) */
  if (b->build.drive == DR_MELTY && b->spin > 0.5f && !b->is_ops) { s->circle = 1; s->hl = 0.5f; s->hw = 0.5f; }
}

static void shape_aabb(float x0, float z0, float x1, float z1, shape_t *s) {
  s->x = (x0 + x1) * 0.5f; s->z = (z0 + z1) * 0.5f; s->h = 0; s->hw = (x1 - x0) * 0.5f; s->hl = (z1 - z0) * 0.5f; s->circle = 0;
}

/* a box's half extent along a unit axis */
static float extent_along(const shape_t *s, float ax, float az) {
  if (s->circle) return s->hl;
  float fx, fz, rx, rz; fwd_of(s->h, &fx, &fz); right_of(s->h, &rx, &rz);
  return fabs_f(dotp(fx, fz, ax, az)) * s->hl + fabs_f(dotp(rx, rz, ax, az)) * s->hw;
}

/* the vertex of a box furthest along a direction (the circle's rim point) */
static void support(const shape_t *s, float dx, float dz, float *px, float *pz) {
  if (s->circle) { *px = s->x + dx * s->hl; *pz = s->z + dz * s->hl; return; }
  float fx, fz, rx, rz; fwd_of(s->h, &fx, &fz); right_of(s->h, &rx, &rz);
  float sf = dotp(fx, fz, dx, dz) >= 0 ? 1.f : -1.f, sr = dotp(rx, rz, dx, dz) >= 0 ? 1.f : -1.f;
  *px = s->x + fx * s->hl * sf + rx * s->hw * sr;
  *pz = s->z + fz * s->hl * sf + rz * s->hw * sr;
}

/* closest point of a box to a point */
static void box_closest(const shape_t *s, float px, float pz, float *qx, float *qz) {
  float fx, fz, rx, rz; fwd_of(s->h, &fx, &fz); right_of(s->h, &rx, &rz);
  float dx = px - s->x, dz = pz - s->z;
  float lz = rtr_clamp(dotp(dx, dz, fx, fz), -s->hl, s->hl), lx = rtr_clamp(dotp(dx, dz, rx, rz), -s->hw, s->hw);
  *qx = s->x + fx * lz + rx * lx; *qz = s->z + fz * lz + rz * lx;
}

static int collide_circle_circle(const shape_t *a, const shape_t *b, contact_t *c) {
  float dx = b->x - a->x, dz = b->z - a->z, d = len2(dx, dz), r = a->hl + b->hl;
  if (d >= r) return 0;
  if (d < 1e-5f) { c->nx = 0; c->nz = 1; } else { c->nx = dx / d; c->nz = dz / d; }
  c->pen = r - d;
  c->cx = a->x + c->nx * (a->hl - c->pen * 0.5f); c->cz = a->z + c->nz * (a->hl - c->pen * 0.5f);
  return 1;
}

/* box a vs circle b: normal from a to b */
static int collide_box_circle(const shape_t *a, const shape_t *b, contact_t *c) {
  float qx, qz; box_closest(a, b->x, b->z, &qx, &qz);
  float dx = b->x - qx, dz = b->z - qz, d = len2(dx, dz);
  if (d >= b->hl) return 0;
  if (d > 1e-5f) { c->nx = dx / d; c->nz = dz / d; c->pen = b->hl - d; c->cx = qx; c->cz = qz; return 1; }
  /* centre inside the box: the face of least penetration */
  float fx, fz, rx, rz; fwd_of(a->h, &fx, &fz); right_of(a->h, &rx, &rz);
  float ddx = b->x - a->x, ddz = b->z - a->z;
  float lz = dotp(ddx, ddz, fx, fz), lx = dotp(ddx, ddz, rx, rz);
  float pz = a->hl - fabs_f(lz), px = a->hw - fabs_f(lx);
  if (pz < px) { float s = lz >= 0 ? 1.f : -1.f; c->nx = fx * s; c->nz = fz * s; c->pen = pz + b->hl; }
  else { float s = lx >= 0 ? 1.f : -1.f; c->nx = rx * s; c->nz = rz * s; c->pen = px + b->hl; }
  c->cx = b->x; c->cz = b->z;
  return 1;
}

/* SAT on the four face axes; the normal is the least penetration axis from a to b,
 * the contact point is the midpoint of the two support points */
static int collide_box_box(const shape_t *a, const shape_t *b, contact_t *c) {
  float ax[4], az[4];
  fwd_of(a->h, &ax[0], &az[0]); right_of(a->h, &ax[1], &az[1]);
  fwd_of(b->h, &ax[2], &az[2]); right_of(b->h, &ax[3], &az[3]);
  float dx = b->x - a->x, dz = b->z - a->z;
  float best = 1e9f; int bi = -1; float bs = 1;
  for (int i = 0; i < 4; i++) {
    float proj = dotp(dx, dz, ax[i], az[i]);
    float ov = extent_along(a, ax[i], az[i]) + extent_along(b, ax[i], az[i]) - fabs_f(proj);
    if (ov <= 0) return 0;
    if (ov < best) { best = ov; bi = i; bs = proj >= 0 ? 1.f : -1.f; }
  }
  c->nx = ax[bi] * bs; c->nz = az[bi] * bs; c->pen = best;
  float p1x, p1z, p2x, p2z;
  support(a, c->nx, c->nz, &p1x, &p1z);
  support(b, -c->nx, -c->nz, &p2x, &p2z);
  c->cx = (p1x + p2x) * 0.5f; c->cz = (p1z + p2z) * 0.5f;
  return 1;
}

static int collide(const shape_t *a, const shape_t *b, contact_t *c) {
  if (a->circle && b->circle) return collide_circle_circle(a, b, c);
  if (!a->circle && b->circle) return collide_box_circle(a, b, c);
  if (a->circle && !b->circle) {
    if (!collide_box_circle(b, a, c)) return 0;
    c->nx = -c->nx; c->nz = -c->nz;
    return 1;
  }
  return collide_box_box(a, b, c);
}

/* overlap test only, no contact (hitboxes) */
static int overlaps(const shape_t *a, const shape_t *b, contact_t *c) { contact_t t; return collide(a, b, c ? c : &t); }

/* the deepest point of a shape past a wall plane with inward normal n and inner offset off
 * (points satisfy dot(p, n) >= off inside the arena); returns the penetration, 0 if none */
static float wall_pen(const shape_t *s, float nx, float nz, float off, float *cx, float *cz) {
  float px, pz; support(s, -nx, -nz, &px, &pz);
  float pen = off - dotp(px, pz, nx, nz);
  if (pen <= 0) return 0;
  *cx = px; *cz = pz;
  return pen;
}

/* -- solids: the static list the walls step collides with (4.5) --- */
enum { SOL_PROP, SOL_DECK, SOL_GATE, SOL_BUMPER };
typedef struct { float x0, z0, x1, z1, top; uint8_t kind; int16_t idx; } solid_t;
#define MAX_SOLIDS 64

static int gather_solids(const match_t *m, solid_t *out) {
  const arena_def_t *a = sim_arena(m);
  int n = 0;
  for (int i = 0; i < a->nprops && n < MAX_SOLIDS; i++) {
    const prop_def_t *p = &a->props[i];
    if (!arena_prop_solid(p) || m->prop_broken[i]) continue;
    /* props are axis aligned for collision (ang is a render tilt under 0.8 rad on the mound only) */
    out[n] = (solid_t){ p->x - p->w * 0.5f, p->z - p->d * 0.5f, p->x + p->w * 0.5f, p->z + p->d * 0.5f, p->h, SOL_PROP, (int16_t)i }; n++;
  }
  for (int i = 0; i < a->nhaz && n < MAX_SOLIDS; i++) {
    const hazard_def_t *h = &a->haz[i];
    if (h->type == HZ_DECK) {
      /* the deck face, in pieces that leave the ramps open */
      float x0 = h->x - h->w * 0.5f, x1 = h->x + h->w * 0.5f, z0 = h->z - h->d * 0.5f, z1 = h->z + h->d * 0.5f;
      float cuts[8][2]; int nc = 0;
      for (int k = 0; k < a->nhaz; k++) if (a->haz[k].type == HZ_RAMP && a->haz[k].h > 0.3f && nc < 8) { cuts[nc][0] = a->haz[k].x - a->haz[k].w * 0.5f; cuts[nc][1] = a->haz[k].x + a->haz[k].w * 0.5f; nc++; }
      float sx = x0;
      /* cuts are listed left to right in the data */
      for (int k = 0; k < nc && n < MAX_SOLIDS; k++) { if (cuts[k][0] > sx) { out[n++] = (solid_t){ sx, z0, cuts[k][0], z1, h->h, SOL_DECK, (int16_t)i }; } sx = cuts[k][1]; }
      if (sx < x1 && n < MAX_SOLIDS) out[n++] = (solid_t){ sx, z0, x1, z1, h->h, SOL_DECK, (int16_t)i };
    } else if (h->type == HZ_BUMPER && !m->haz[i].broken) {
      float x0, z0, x1, z1; arena_hazard_rect(h, &x0, &z0, &x1, &z1);
      out[n++] = (solid_t){ x0, z0, x1, z1, (float)h->p0 / 100.0f, SOL_BUMPER, (int16_t)i };
    }
  }
  if (!m->gate_open && m->nbots > OPS_SLOT && m->bots[OPS_SLOT].is_ops) {
    float g[2][4]; arena_ops_gate(a, g);
    for (int k = 0; k < 2 && n < MAX_SOLIDS; k++) out[n++] = (solid_t){ g[k][0], g[k][1], g[k][2], g[k][3], 1.0f, SOL_GATE, (int16_t)k };
  }
  return n;
}

/* -- damage and debris -------------------------------------------- */
static void debris_spawn(match_t *m, float x, float z, float y, float vx, float vz, float vy, int kind, int colour, int pickup, int owner, int life, int panel) {
  debris_t *d;
  if (m->ndebris < MAX_DEBRIS) d = &m->debris[m->ndebris++];
  else {
    /* over the cap the oldest is recycled */
    int best = 0;
    for (int i = 1; i < MAX_DEBRIS; i++) if (m->debris[i].life < m->debris[best].life) best = i;
    d = &m->debris[best];
  }
  memset(d, 0, sizeof *d);
  d->x = x; d->z = z; d->y = y; d->vx = vx; d->vz = vz; d->vy = vy; d->spin = 6.0f;
  d->kind = (uint8_t)kind; d->colour = (uint8_t)colour; d->pickup = (uint8_t)pickup; d->owner = (uint8_t)owner; d->life = (int16_t)life; d->panel = (uint8_t)panel;
}

static void debris_remove(match_t *m, int i) {
  m->ndebris--;
  if (i != m->ndebris) m->debris[i] = m->debris[m->ndebris];
}

static void debris_burst_at(match_t *m, float x, float z, int kind, int colour, int count) {
  for (int k = 0; k < count; k++) {
    float a = rnd01(m) * 6.2831853f, sp = 1.0f + rnd01(m) * 2.0f;
    debris_spawn(m, x, z, 0.3f, rtr_sin(a) * sp, rtr_cos(a) * sp, 2.0f + rnd01(m) * 2.0f, kind, colour, 0, 255, 120 + rnd_n(m, 60), 0);
  }
}

/* a panel leaves the bot as sim debris (4.9) */
static void shed_panel(match_t *m, int slot, int panel) {
  bot_t *b = &m->bots[slot];
  b->panel_state[panel] = 4;
  float a = rnd01(m) * 6.2831853f;
  float vx = b->b.vx + rtr_sin(a) * 2.0f, vz = b->b.vz + rtr_cos(a) * 2.0f;
  int pickup = m->comeback ? 1 : 0;
  debris_spawn(m, b->b.x, b->b.z, b->floor_y + 0.3f, vx, vz, 3.0f, DB_PANEL, b->seat, pickup, slot, 360, panel);
  ev_simple(m, EV_SHED, slot, slot, panel, b->b.x, b->b.z, b->b.y);
}

static void shed_random_panel(match_t *m, int slot) {
  bot_t *b = &m->bots[slot];
  int cands[6], n = 0;
  for (int p = 0; p < PN_COUNT; p++) if (b->panel_state[p] < 4) cands[n++] = p;
  if (!n) return;
  shed_panel(m, slot, cands[rnd_n(m, n)]);
}

/* at 0 HP all remaining panels and the wheels detach, up to 8 debris (4.9) */
static void burst(match_t *m, int slot) {
  bot_t *b = &m->bots[slot];
  int n = 0;
  for (int p = 0; p < PN_COUNT && n < 8; p++) if (b->panel_state[p] < 4) { shed_panel(m, slot, p); n++; }
  int wheels = b->build.drive == DR_TWO ? 2 : b->build.drive == DR_FOUR ? 4 : b->build.drive == DR_SIX ? 6 : b->build.drive == DR_TREAD || b->build.drive == DR_LEGTRACK ? 2 : 0;
  for (int w = 0; w < wheels && n < 8; w++, n++) {
    float a = rnd01(m) * 6.2831853f;
    debris_spawn(m, b->b.x, b->b.z, b->floor_y + 0.2f, b->b.vx + rtr_sin(a) * 2.5f, b->b.vz + rtr_cos(a) * 2.5f, 2.0f, DB_WHEEL, b->seat, 0, slot, 240, 0);
  }
  ev_simple(m, EV_BURST, slot, slot, 0, b->b.x, b->b.z, b->b.y);
}

/* panel geometry thresholds, shed and RED TAG (4.9); called after damage */
static void hp_thresholds(match_t *m, int slot) {
  bot_t *b = &m->bots[slot];
  if (b->is_ops || !b->alive) return;
  int st = b->hp <= 30 ? 3 : b->hp <= 50 ? 2 : b->hp <= 70 ? 1 : 0;
  for (int p = 0; p < PN_COUNT; p++) if (b->panel_state[p] < 4 && b->panel_state[p] < st) b->panel_state[p] = (uint8_t)st;
  while (b->shed_next > 0 && b->hp <= (float)b->shed_next && b->hp > 0) {
    shed_random_panel(m, slot);
    b->shed_next = (uint8_t)(b->build.armour == AR_NONE && b->shed_next > 10 ? b->shed_next - 10 : 0);
  }
  if (b->hp <= 30 && !b->red_tag_used && m->comeback && b->hp > 0) {
    b->red_tag_t = 480; b->red_tag_used = 1;
    ev_simple(m, EV_RED_TAG, slot, slot, 0, b->b.x, b->b.z, b->b.y);
  }
}

static int weapon_is_spinner(int w) { return w == WP_HORIZ || w == WP_DRUM || w == WP_SAW; }

/* the common damage path: armour, red tag, teammates, tallies, stun, dents (4.7).
 * Returns the damage applied. nx, nz is the hit normal from attacker to victim. */
static float apply_damage(match_t *m, int att, int vic, float raw, int weapon, int stun, float cx, float cz, float nx, float nz, int ev_type, float zone) {
  bot_t *a = att >= 0 ? &m->bots[att] : 0, *v = &m->bots[vic];
  if (!v->alive) return 0;
  float dmg = raw;
  dmg *= 1.0f - ARMOURS[v->build.armour].reduction;
  if (a) {
    if (a->red_tag_t > 0) dmg *= 1.4f;
    if (!a->is_ops && !v->is_ops && m->mode == MODE_2V2 && a->team == v->team) dmg *= 0.5f;
  }
  if (dmg <= 0) return 0;
  /* no single weapon hit takes more than HIT_CAP of the victim's bar (4.7): the exposed and spun
   * down zones still make the biggest number on screen, a flip KO is the only bigger moment.
   * OPS's punish shove and the hazards are outside the rule */
  if (a && !a->is_ops && !v->is_ops && v->hp_max > 0 && dmg > v->hp_max * HIT_CAP) dmg = v->hp_max * HIT_CAP;
  v->hp -= dmg;
  if (v->is_ops && v->hp < 1) v->hp = 1;          /* OPS floors at 1 and is never KO'd (7) */
  if (a) {
    a->dealt += dmg;
    if (dmg > a->biggest) a->biggest = dmg;
    if (weapon_is_spinner(weapon) || weapon == -2) a->spinner_dealt += dmg;   /* -2 is MELTY */
    a->countout = 0;                                /* a landed hit resets the count (2.7) */
    v->last_hit_by = (uint8_t)(att + 1); v->last_hit_t = 90;
    if (v->is_ops) { v->ops_dmg += dmg; v->ops_dmg_t = 300; }
  } else if (v->last_hit_by && v->last_hit_t > 0 && v->last_hit_by - 1 < m->nbots) {
    /* a hazard the victim was pushed into within the last 90 frames counts as control (2.5) */
    m->bots[v->last_hit_by - 1].control += 20;
  }
  if (stun > v->stun) v->stun = (int16_t)stun;
  /* a spinner hit on WEAR PLATE stuns the attacker 9 extra frames (3.6) */
  if (a && v->build.armour == AR_WEAR && weapon_is_spinner(weapon) && a->stun < 9) a->stun = 9;
  /* the panel on the hit zone advances one dent state on any hit over 20 */
  if (dmg > 20 && !v->is_ops) {
    float fx, fz, rx, rz; fwd_of(v->b.h, &fx, &fz); right_of(v->b.h, &rx, &rz);
    float dx = cx - v->b.x, dz = cz - v->b.z, lz = dotp(dx, dz, fx, fz), lx = dotp(dx, dz, rx, rz);
    int p = (weapon == WP_HAMMER || weapon == -3) ? PN_TOP : fabs_f(lz) * v->b.hw > fabs_f(lx) * v->b.hl ? (lz > 0 ? PN_FRONT : PN_BACK) : (lx > 0 ? PN_RIGHT : PN_LEFT);
    if (v->panel_state[p] < 3) v->panel_state[p]++;
  }
  m->no_hit_frames = 0;
  if (ev_type >= 0) ev_hit(m, ev_type, att < 0 ? 255 : att, vic, zone, cx, cz, v->b.y + 0.2f, dmg, nx, nz);
  /* hit stop (5.1) from the band; the sim applies it so a replay stays aligned */
  if (!m->hitstop_off && (ev_type == EV_HIT || ev_type == EV_RAM)) {
    int stop = dmg < BAND_HIT_MIN ? STOP_TAP : dmg < BAND_HEAVY_MIN ? STOP_HIT : dmg < BAND_HUGE_MIN ? STOP_HEAVY : STOP_HUGE;
    if (stop > m->hitstop) m->hitstop = (int16_t)stop;
  }
  hp_thresholds(m, vic);
  return dmg;
}

/* knockback impulse along a normal (4.7); a HOVER has no grip to take it on, so it flies 1.5x */
static void knock(bot_t *v, float jx, float jz) {
  if (!v->alive || v->is_ops || v->lifted_by || v->clamped_by) return;
  float mult = DRIVES[v->build.drive].knock_mult * (ARMOURS[v->build.armour].knock_half ? 0.5f : 1.0f);
  if (v->build.drive == DR_HOVER) mult *= 1.5f;
  v->b.vx += jx * v->b.inv_mass * mult;
  v->b.vz += jz * v->b.inv_mass * mult;
}

/* the hit zone of a contact on a victim (4.7) */
static float hit_zone(const bot_t *v, int weapon, float cx, float cz) {
  float fx, fz, rx, rz; fwd_of(v->b.h, &fx, &fz); right_of(v->b.h, &rx, &rz);
  float dx = cx - v->b.x, dz = cz - v->b.z, lz = dotp(dx, dz, fx, fz), lx = dotp(dx, dz, rx, rz);
  float zone = 1.0f;
  if (v->b.shape == SH_BOX && lz > 0.6f * v->b.hl && fabs_f(lx) < v->b.hw) zone = v->build.chassis == CH_WEDGE ? 0.5f : 0.6f;
  int exposed = v->lifted_by || (v->b.y - v->floor_y) > 0.15f || v->selfright > 0 ||
                (v->inverted && (weapon == WP_HAMMER || weapon == WP_DRUM || weapon == WP_SPIKE));
  if (exposed) zone = 1.5f;
  /* a spun down spinner's weapon circle takes 2.0x */
  if ((v->build.weapon == WP_HORIZ || v->build.weapon == WP_DRUM) && v->spin < 0.3f) {
    const weapon_def_t *w = &WEAPONS[v->build.weapon];
    float wx = v->b.x + fx * w->hb_z0, wz = v->b.z + fz * w->hb_z0;
    if (len2(cx - wx, cz - wz) < w->hb_r + 0.1f) zone = 2.0f;
  }
  if (v->build.chassis == CH_SHELL) zone *= 0.8f;
  if (weapon == WP_HAMMER && v->build.chassis == CH_LUNCHBOX) zone *= 0.7f;
  return zone;
}

/* the weapon's hitbox this frame, or 0 when it has none; which 0 is the primary, 1 the secondary */
static int weapon_shape(const bot_t *b, int which, shape_t *s) {
  int wid = which ? b->build.weapon2 : b->build.weapon;
  if (b->build.drive == DR_MELTY && !which && !b->is_ops) {
    if (b->spin <= 0.5f) return 0;
    s->x = b->b.x; s->z = b->b.z; s->h = b->b.h; s->hl = s->hw = 0.5f; s->circle = 1;
    return 1;
  }
  if (wid == WP_NONE || wid == WP_WEDGE || wid == WP_FLAME) return 0;
  const weapon_def_t *w = &WEAPONS[wid];
  float spin = which ? b->spin2 : b->spin;
  int active = which ? b->active2 : b->active;
  float lift = b->lift;
  int on;
  switch (wid) {
    case WP_HORIZ: case WP_DRUM: on = spin > SPINNER_HIT_MIN; break;
    case WP_SAW: on = spin > 0.5f; break;
    case WP_FLIPPER: case WP_HAMMER: case WP_SPIKE: on = active > 0; break;
    case WP_LIFTER: on = lift > 0.3f; break;
    case WP_CRUSHER: on = b->clamp == 1; break;     /* the closing frame */
    default: on = 0;
  }
  if (!on) return 0;
  float fx, fz; fwd_of(b->b.h, &fx, &fz);
  float scale = b->is_ops ? 1.4f : 1.0f;
  if (w->hb_r > 0) {
    s->x = b->b.x + fx * w->hb_z0 * scale; s->z = b->b.z + fz * w->hb_z0 * scale; s->h = b->b.h; s->hl = s->hw = w->hb_r; s->circle = 1;
  } else {
    float zm = (w->hb_z0 + w->hb_z1) * 0.5f * scale;
    s->x = b->b.x + fx * zm; s->z = b->b.z + fz * zm; s->h = b->b.h; s->hl = (w->hb_z1 - w->hb_z0) * 0.5f * scale; s->hw = w->hb_w * 0.5f; s->circle = 0;
  }
  return 1;
}

/* -- the drive model (4.5) ----------------------------------------- */
static void stick_vec(const pad_state_t *in, float *sx, float *sy) {
  float x = (float)in->lx / 32767.0f, y = -(float)in->ly / 32767.0f;   /* y positive is forward */
  float mag = len2(x, y);
  if (mag < 0.18f) { *sx = 0; *sy = 0; return; }
  float k = (fmin_f(mag, 1.0f) - 0.18f) / (1.0f - 0.18f) / mag;
  *sx = x * k; *sy = y * k;
}

static int weapon_button(const pad_state_t *in) { return (in->buttons & IN_A) || in->rt > TRIGGER_ON; }
static int weapon_edge(const pad_state_t *in, const pad_state_t *prev) { return weapon_button(in) && !weapon_button(prev); }
static int boost_edge(const pad_state_t *in, const pad_state_t *prev) {
  int now = (in->buttons & IN_L) || in->lt > TRIGGER_ON, was = (prev->buttons & IN_L) || prev->lt > TRIGGER_ON;
  return now && !was;
}
static int btn_edge(const pad_state_t *in, const pad_state_t *prev, int bit) { return (in->buttons & bit) && !(prev->buttons & bit); }

static void drive_step(match_t *m, int slot) {
  bot_t *b = &m->bots[slot];
  const drive_def_t *d = &DRIVES[b->build.drive];
  const arena_def_t *ar = sim_arena(m);
  b->drive_t = 0; b->drive_s = 0;
  if (!b->alive) return;
  const pad_state_t *in = &b->in, *prev = &b->prev_in;
  /* boost, brake, taunt (4.5) */
  if (d->boost && b->boost == 0 && b->boost_cd == 0 && boost_edge(in, prev) && bot_can_drive(b) && !b->inverted) {
    b->boost = 48; b->boost_cd = (int16_t)(b->red_tag_t > 0 ? 120 : 240);
    ev_simple(m, EV_BOOST, slot, slot, 0, b->b.x, b->b.z, b->b.y);
  }
  if (btn_edge(in, prev, IN_B) && !b->inverted && bot_can_drive(b)) { b->b.vx *= 0.2f; b->b.vz *= 0.2f; b->brake = 18; }
  if (btn_edge(in, prev, IN_X) && b->taunt == 0 && bot_can_drive(b)) {
    b->taunt = 30; b->taunts++;
    if (b->taunts <= 4) b->aggro += 60;             /* capped at 4 taunts (2.5) */
    ev_simple(m, EV_TAUNT, slot, slot, 0, b->b.x, b->b.z, b->b.y);
  }
  if (!bot_can_drive(b)) {
    /* no drive, no steering torque: the yaw rate decays on the floor (an inverted or stunned bot used to keep
     * whatever spin it had, forever); a tumbling airborne bot keeps its spin until it lands */
    if (!b->airborne) b->b.w *= 0.85f;
    return;
  }
  float sx, sy; stick_vec(in, &sx, &sy);
  float top = bot_top_speed(b);
  if (b->lift > 0.3f && b->carry_t > 0) top *= 0.6f;          /* carrying a victim */
  float accel = d->accel * (b->build.drive == DR_TREAD ? 1.0f : ar->traction_mult);
  if (arena_on_slope(ar, b->b.x, b->b.z) && b->build.drive != DR_LEGTRACK && b->build.drive != DR_SIX) accel *= 0.6f, top *= 0.6f;
  if (d->strafe) {
    /* HOVER and MELTY: the stick is a world direction relative to the seat's camera forward. The
     * yaw it is read against is latched when the stick leaves the dead zone and held until it
     * returns: the camera follows the travel direction, so reading the live yaw every frame made
     * stick right mean "90 degrees from wherever the camera got to", a pirouette that never settled */
    float mag_in = len2(sx, sy);
    if (mag_in > 0.1f) { if (!b->strafe_held) { b->strafe_held = 1; b->drive_yaw = in->cam_yaw; } }
    else b->strafe_held = 0;
    float cy = b->strafe_held ? b->drive_yaw : in->cam_yaw, cfx, cfz, crx, crz; fwd_of(cy, &cfx, &cfz); right_of(cy, &crx, &crz);
    float dx = cfx * sy - crx * sx, dz = cfz * sy - crz * sx;   /* screen right is minus the local x axis */
    float mag = len2(dx, dz);
    b->drive_t = mag;
    /* a pinched skirt (a bot pressing on the hover within SKIRT_FRAMES) cuts the thrust to a quarter: a
     * HOVER cannot push off whatever is leaning on it, which is what lets a wedge shove it anywhere */
    if (b->build.drive == DR_HOVER && b->skirt_t > 0) accel *= 0.25f;
    float k = accel / (top > 0 ? top : 1.0f) * DT;
    float dvx = (dx * top - b->b.vx) * k, dvz = (dz * top - b->b.vz) * k;
    if (b->build.drive == DR_HOVER) {
      float sp = len2(b->b.vx, b->b.vz);
      if (sp > 0.2f) {
        float ux = b->b.vx / sp, uz = b->b.vz / sp, along = dotp(dvx, dvz, ux, uz);
        if (along < 0) { dvx += ux * along * (HOVER_BRAKE - 1.0f); dvz += uz * along * (HOVER_BRAKE - 1.0f); }   /* braking thrust is weak */
        float lat_x = dvx - ux * dotp(dvx, dvz, ux, uz), lat_z = dvz - uz * dotp(dvx, dvz, ux, uz);
        dvx -= lat_x * (1.0f - HOVER_BRAKE); dvz -= lat_z * (1.0f - HOVER_BRAKE);                                 /* so is thrust across the slide */
      }
    }
    b->b.vx += dvx; b->b.vz += dvz;
    /* the heading follows the direction of travel at up to 3 rad/s (the camera wants a forward; MELTY spins in render) */
    if (mag > 0.1f) { float want = rtr_atan2(dx, dz); float err = rtr_wrap_pi(want - b->b.h); b->b.w = rtr_clamp(err * 6.0f, -3.0f, 3.0f); }
    else b->b.w *= 0.8f;
    return;
  }
  float t, s;
  if (b->scheme == SCHEME_TANK && in->ry != 0) {
    /* TANK: left Y and right Y are the two sides; the left side faster is a right turn */
    float ly = -(float)in->ly / 32767.0f, ry = -(float)in->ry / 32767.0f;
    if (fabs_f(ly) < 0.18f) ly = 0; if (fabs_f(ry) < 0.18f) ry = 0;
    t = (ly + ry) * 0.5f; s = (ly - ry) * 0.5f;
  } else { t = sy; s = sx; }   /* arcade, and a TANK seat whose right stick rests: a d-pad or one stick still steers */
  if (t < 0) t *= 0.8f;                                        /* reverse at 80 percent */
  if (b->inverted) { if (b->build.weapon == WP_HORIZ) { t = 0; s = 0; } }   /* HORIZONTAL inverted spins but cannot steer */
  b->drive_t = t; b->drive_s = s;
  float fx, fz, rx, rz; fwd_of(b->b.h, &fx, &fz); right_of(b->b.h, &rx, &rz);
  float vf = dotp(b->b.vx, b->b.vz, fx, fz), vl = dotp(b->b.vx, b->b.vz, rx, rz);
  float speed = len2(b->b.vx, b->b.vz);
  /* the forward component is driven toward t * top at accel; the lateral one bleeds by traction */
  float want = t * top;
  float dv = rtr_clamp(want - vf, -accel * DT, accel * DT);
  if (fabs_f(t) < 0.05f) dv = -vf * 0.10f;                      /* coasting: drag 0.90 per frame */
  float nvf = vf + dv, nvl = vl * (1.0f - (4.0f + 8.0f * d->traction) * DT);
  b->b.vx = fx * nvf + rx * nvl; b->b.vz = fz * nvf + rz * nvl;
  float turn = d->turn_deg * (PI_F / 180.0f);
  /* stick right (s > 0) is a right turn, which is a heading decrease in this world (see right_of) */
  float wt = -s * turn * (1.0f - 0.4f * fmin_f(speed / (top > 0 ? top : 1.0f), 1.0f)) * (b->build.weapon == WP_HORIZ && b->spin > 0.6f ? 0.7f : 1.0f);
  if (b->inverted) wt *= 0.7f;
  b->b.w += rtr_clamp(wt - b->b.w, -20.0f * DT, 20.0f * DT);
}

/* -- weapons (4.6) ------------------------------------------------- */
static void spinner_follow(float *spin, int held, float spin_up) {
  *spin += (held ? 1.0f : -0.5f) * DT / spin_up;
  *spin = rtr_clamp(*spin, 0, 1);
}

static void weapon_machine(match_t *m, int slot, int which) {
  bot_t *b = &m->bots[slot];
  int wid = which ? b->build.weapon2 : b->build.weapon;
  if (b->build.drive == DR_MELTY && !which && !b->is_ops) {
    spinner_follow(&b->spin, weapon_button(&b->in) && b->alive, WP_MELTY_SPIN_UP);
    return;
  }
  if (wid == WP_NONE || wid == WP_WEDGE) return;
  const weapon_def_t *w = &WEAPONS[wid];
  float *spin = which ? &b->spin2 : &b->spin, *fuel = which ? &b->fuel2 : &b->fuel;
  int16_t *cooldown = which ? &b->cooldown2 : &b->cooldown, *active = which ? &b->active2 : &b->active;
  int wb = weapon_button(&b->in) && b->alive && !(b->inverted && !w->inv_operate);
  int ew = weapon_edge(&b->in, &b->prev_in) && b->alive;
  if (b->is_ops && !m->ops_driven) { wb = b->in.buttons & (which ? IN_X : IN_A); ew = wb && !(b->prev_in.buttons & (which ? IN_X : IN_A)); }
  if (b->is_ops && m->ops_driven) { int bit = which ? IN_X : IN_A; wb = m->ops_in_gun.buttons & bit; ew = wb && !(b->prev_in.buttons & bit); }
  int operate = !(b->inverted && !w->inv_operate) && !b->lifted_by && !b->clamped_by && b->lock <= 0;
  switch (wid) {
    case WP_HORIZ: case WP_DRUM:
      spinner_follow(spin, wb && operate, w->spin_up);
      break;
    case WP_SAW:
      if (ew && operate) b->saw_on = !b->saw_on;
      if (!b->alive) b->saw_on = 0;
      spinner_follow(spin, b->saw_on && operate, w->spin_up);
      break;
    case WP_FLIPPER:
      if (*active > 0) { (*active)--; if (*active == 0 && b->windup == 0) *cooldown = w->cooldown; }
      else if (ew && *cooldown == 0 && operate && !b->inverted && b->stun <= 0) { *active = w->active; ev_simple(m, EV_WEAPON_TELL, slot, slot, 0, b->b.x, b->b.z, b->b.y); }
      break;
    case WP_HAMMER:
      if (b->windup > 0) { b->windup--; if (b->windup == 0) *active = w->active; }
      else if (*active > 0) {
        (*active)--;
        if (*active == 0) {
          *cooldown = w->cooldown;
          if (!b->saw_tick) {              /* saw_tick doubles as the landed flag for a swing */
            b->hp -= w->self_dmg;          /* a miss costs 4 HP (4.6) */
            if (b->hp < 1 && b->is_ops) b->hp = 1;
            ev_simple(m, EV_WEAPON_TELL, slot, slot, 1, b->b.x + rtr_sin(b->b.h) * 0.8f, b->b.z + rtr_cos(b->b.h) * 0.8f, b->b.y + 0.3f);
            hp_thresholds(m, slot);
          }
        }
      }
      else if (ew && *cooldown == 0 && operate && !b->inverted && b->stun <= 0) { b->windup = w->windup; b->saw_tick = 0; ev_simple(m, EV_WEAPON_TELL, slot, slot, 0, b->b.x, b->b.z, b->b.y); }
      break;
    case WP_SPIKE:
      if (*active > 0) { (*active)--; if (*active == 0) *cooldown = w->cooldown; }
      else if (ew && *cooldown == 0 && operate && b->stun <= 0) *active = w->active;
      break;
    case WP_LIFTER: {
      float want = wb && operate ? 1.0f : 0.0f;
      b->lift += rtr_clamp(want - b->lift, -1.0f / WP_LIFTER_FRAMES, 1.0f / WP_LIFTER_FRAMES);
      break;
    }
    case WP_FLAME:
      if (wb && operate && *fuel > 0) { *fuel -= 1; }
      else if (!(wb && operate)) { *fuel += (float)WP_FLAME_FUEL / (float)WP_FLAME_REFILL; if (*fuel > WP_FLAME_FUEL) *fuel = WP_FLAME_FUEL; }
      break;
    case WP_CRUSHER:
      if (b->clamp > 0) { b->clamp--; if (b->clamp == 0) *cooldown = w->cooldown; }
      else if (ew && *cooldown == 0 && operate && b->stun <= 0 && b->clamped_by == 0) b->clamp = w->windup + 1;   /* counts to 1, the closing frame */
      break;
    default: break;
  }
}

/* -- hits (4.7) ---------------------------------------------------- */
static float closing_of(const bot_t *a, const bot_t *v, float nx, float nz, float tip) {
  float rel = dotp(a->b.vx - v->b.vx, a->b.vz - v->b.vz, nx, nz);
  return rtr_clamp((fabs_f(rel) + tip) / 4.0f, CLOSING_MIN, CLOSING_MAX);
}

/* a weapon hitbox overlapping a victim body: one hit */
static void weapon_hit(match_t *m, int att, int vic, int which, const contact_t *c) {
  bot_t *a = &m->bots[att], *v = &m->bots[vic];
  int wid = which ? a->build.weapon2 : a->build.weapon;
  int melty = a->build.drive == DR_MELTY && !which && !a->is_ops;
  int16_t *cd = which ? &a->hit_cd2[vic] : &a->hit_cd[vic];
  if (*cd > 0) return;
  float spin = which ? a->spin2 : a->spin;
  float nx = v->b.x - a->b.x, nz = v->b.z - a->b.z, nl = len2(nx, nz);
  if (nl < 1e-4f) { nx = 0; nz = 1; } else { nx /= nl; nz /= nl; }
  float base, tip = 0, spin_or_1 = 1; int stun; float knockv = 0, self = 0, fixed_closing = 0;
  if (melty) { base = WP_MELTY_DMG; stun = 18; knockv = 4.0f; tip = spin * MELTY_TIP; *cd = MELTY_HIT_CD; wid = -2; self = WP_MELTY_SELF; }
  else {
    const weapon_def_t *w = &WEAPONS[wid];
    base = w->base; stun = w->stun; knockv = w->knock; self = w->self_dmg;
    switch (wid) {
      case WP_HORIZ: spin_or_1 = spin; tip = spin * TIP_HORIZ; *cd = SPINNER_HIT_CD; break;
      case WP_DRUM: spin_or_1 = spin; tip = spin * TIP_DRUM; *cd = SPINNER_HIT_CD; break;
      case WP_SAW: spin_or_1 = 1; *cd = WP_SAW_TICK_FRAMES; self = 2.0f / 6.0f; stun = 6; fixed_closing = 0.5f; break;   /* a grind, not a swing */
      case WP_FLIPPER: case WP_HAMMER: case WP_SPIKE: *cd = (int16_t)((which ? a->active2 : a->active) + 2); a->saw_tick = 1; tip = TIP_SWING; break;
      case WP_LIFTER: *cd = 600; fixed_closing = 1.0f; break;
      case WP_CRUSHER: *cd = 200; fixed_closing = 1.0f; break;
      default: *cd = 30;
    }
  }
  float closing = fixed_closing > 0 ? fixed_closing : closing_of(a, v, nx, nz, tip);
  float zone = hit_zone(v, wid, c->cx, c->cz);
  float raw = base * spin_or_1 * closing * zone;
  /* TREADS: saws do 2x on the tread zones, the sides (3.4) */
  if (wid == WP_SAW && v->build.drive == DR_TREAD && zone >= 1.0f && zone < 1.5f) raw *= 2.0f;
  float dmg = apply_damage(m, att, vic, raw, wid, stun, c->cx, c->cz, nx, nz, EV_HIT, zone);
  if (dmg <= 0 && wid != WP_LIFTER && wid != WP_CRUSHER) return;
  float J = knockv * (0.5f + dmg / 30.0f);
  if (wid == WP_HORIZ) knock(v, nz * J, -nx * J);                          /* lateral: the normal rotated 90 degrees */
  else if (wid == WP_DRUM) { knock(v, nx * J, nz * J); if (!v->lifted_by && v->build.drive != DR_HOVER) { v->b.vy = 3.0f; v->airborne = 1; } }
  else knock(v, nx * J, nz * J);
  /* the spinning weapons take the reaction: the pair bounces apart, as real spinners do */
  if (wid == WP_HORIZ || wid == WP_DRUM || wid == -2) knock(a, -nx * J, -nz * J);
  /* a landed spinner hit spends stored energy (a balance deviation, see SPIN_BLEED_*): the next
   * full strength hit needs a spin up, which opens the spun down window of 4.7 to the victim */
  if (wid == WP_HORIZ) a->spin = fmax_f(0, a->spin - SPIN_BLEED_HORIZ);
  else if (wid == WP_DRUM) a->spin = fmax_f(0, a->spin - SPIN_BLEED_DRUM);
  else if (wid == -2) a->spin = fmax_f(0, a->spin - SPIN_BLEED_MELTY);
  /* self damage per table; grinding on the referee costs nothing, it is OPS who answers that (7) */
  if (self > 0 && wid != WP_HAMMER && !v->is_ops) { a->hp -= self; if (a->is_ops && a->hp < 1) a->hp = 1; hp_thresholds(m, att); }
  /* TWO WHEEL: a side or rear third hit cuts top speed 40 percent for 240 frames (3.4); from the hit band
   * up (6), so a wedge's ram and a hammer's dent both find the scooter wheel */
  if (v->build.drive == DR_TWO && zone >= 1.0f && dmg >= BAND_HIT_MIN) v->wheel_hit = 240;
  switch (wid) {
    case WP_FLIPPER:
      if (!v->lifted_by && !v->is_ops) {
        float fx, fz; fwd_of(a->b.h, &fx, &fz);
        v->b.vy = 7.0f * (v->build.chassis == CH_LOWBOY ? 0.8f : 1.0f);
        float hv = 3.0f * (v->build.drive == DR_HOVER ? 1.5f : 1.0f);
        v->b.vx += fx * hv; v->b.vz += fz * hv;
        v->airborne = 1; v->flip_pending = 1; v->launched_by = (uint8_t)(att + 1);
        ev_simple(m, EV_LAUNCH, att, vic, 0, v->b.x, v->b.z, v->b.y);
      }
      break;
    case WP_LIFTER:
      if (!v->is_ops && !v->airborne && !v->lifted_by && !v->clamped_by && a->carry_t == 0) {
        /* only a victim facing away or sideways (within 90 degrees) */
        float afx, afz, vfx, vfz; fwd_of(a->b.h, &afx, &afz); fwd_of(v->b.h, &vfx, &vfz);
        /* OPS, the referee, lifts whatever it reaches */
        if (a->is_ops || dotp(afx, afz, vfx, vfz) >= 0) { v->lifted_by = (uint8_t)(att + 1); a->carry_t = 1; }
      }
      break;
    case WP_CRUSHER:
      if (!v->is_ops && !v->lifted_by && !v->clamped_by) { v->clamped_by = (uint8_t)(att + 1); a->lock = 120; v->lock = 120; }
      break;
    default: break;
  }
}

/* two spinners at spin over 0.5 whose circles overlap (4.7) */
static void clash_check(match_t *m) {
  for (int i = 0; i < m->nbots; i++) {
    bot_t *a = &m->bots[i];
    if (!a->alive || !(a->build.weapon == WP_HORIZ || a->build.weapon == WP_DRUM) || a->spin <= 0.5f) continue;
    shape_t sa; if (!weapon_shape(a, 0, &sa)) continue;
    for (int j = i + 1; j < m->nbots; j++) {
      bot_t *b = &m->bots[j];
      if (!b->alive || !(b->build.weapon == WP_HORIZ || b->build.weapon == WP_DRUM) || b->spin <= 0.5f) continue;
      shape_t sb; if (!weapon_shape(b, 0, &sb)) continue;
      contact_t c; if (!collide(&sa, &sb, &c)) continue;
      if (a->hit_cd[j] > 0 || b->hit_cd[i] > 0) continue;       /* one clash per CLASH_CD: an event, not a metronome */
      float da = WEAPONS[a->build.weapon].base * a->spin * CLASH_FRAC, db = WEAPONS[b->build.weapon].base * b->spin * CLASH_FRAC;
      float mx = (sa.x + sb.x) * 0.5f, mz = (sa.z + sb.z) * 0.5f;
      apply_damage(m, i, j, da, a->build.weapon, 30, mx, mz, c.nx, c.nz, -1, 1.0f);
      apply_damage(m, j, i, db, b->build.weapon, 30, mx, mz, -c.nx, -c.nz, -1, 1.0f);
      a->spin = 0; b->spin = 0; a->stun = 30; b->stun = 30; a->clashes++; b->clashes++;
      a->hit_cd[j] = CLASH_CD; b->hit_cd[i] = CLASH_CD;
      knock(a, -c.nx * 6.0f, -c.nz * 6.0f); knock(b, c.nx * 6.0f, c.nz * 6.0f);
      ev_hit(m, EV_CLASH, i, j, 1.0f, mx, mz, 0.3f, da + db, c.nx, c.nz);
      if (!m->hitstop_off && m->hitstop < STOP_CLASH) m->hitstop = STOP_CLASH;
    }
  }
}

/* the FLAME cone: length hb_z1 (1.5 u; the spec's 2.0 out-reached every other weapon by a bot length,
 * so a HOVER could toast a wedge that never got to touch it), half angle 30 degrees from the nozzle (4.6) */
static void flame_step(match_t *m, int att, int which) {
  bot_t *a = &m->bots[att];
  int wid = which ? a->build.weapon2 : a->build.weapon;
  if (wid != WP_FLAME || !a->alive) return;
  float *fuel = which ? &a->fuel2 : &a->fuel;
  int wb = weapon_button(&a->in) && !(a->inverted) && !a->lifted_by && !a->clamped_by && a->lock <= 0;
  if (!wb || *fuel <= 0) return;
  float fx, fz; fwd_of(a->b.h, &fx, &fz);
  float nx0 = a->b.x + fx * a->b.hl, nz0 = a->b.z + fz * a->b.hl;
  for (int j = 0; j < m->nbots; j++) {
    if (j == att) continue;
    bot_t *v = &m->bots[j];
    if (!v->alive) continue;
    int16_t *cd = which ? &a->hit_cd2[j] : &a->hit_cd[j];
    float dx = v->b.x - nx0, dz = v->b.z - nz0, d = len2(dx, dz), rad = fmax_f(v->b.hl, v->b.hw);
    if (d - rad > WEAPONS[WP_FLAME].hb_z1) continue;
    if (d > 0.05f && dotp(dx, dz, fx, fz) / d < 0.866f && d > rad) continue;
    if (*cd > 0) continue;
    *cd = WP_FLAME_TICK_FRAMES;
    float cx = v->b.x - (d > 0.05f ? dx / d : 0) * rad * 0.8f, cz = v->b.z - (d > 0.05f ? dz / d : 0) * rad * 0.8f;
    float dmg = apply_damage(m, att, j, WEAPONS[WP_FLAME].base, WP_FLAME, 0, cx, cz, fx, fz, EV_HIT, 1.0f);
    /* a burning bot is not lit again until the burn ends: a held cone is 3 per tick plus one burn per approach */
    if (dmg > 0 && !ARMOURS[v->build.armour].burn_immune && !v->is_ops && v->burn_t <= 0) { v->burn_t = WP_FLAME_BURN_T; v->burn_dps = WP_FLAME_BURN_DPS; }
  }
}

/* weapon hitboxes vs bodies, lifter and crusher holds, prop damage (4.6, 4.7) */
static void hit_test(match_t *m) {
  const arena_def_t *ar = sim_arena(m);
  for (int i = 0; i < m->nbots; i++) {
    bot_t *a = &m->bots[i];
    if (!a->alive) continue;
    if (a->lifted_by) continue;          /* a carried bot's weapon is inert while it hangs off the forks (4.6) */
    for (int which = 0; which < 2; which++) {
      if (which && a->build.weapon2 == WP_NONE) continue;
      if (which == 0 && a->build.drive != DR_MELTY && (a->build.weapon == WP_NONE || a->build.weapon == WP_WEDGE)) continue;
      shape_t ws; if (!weapon_shape(a, which, &ws)) continue;
      for (int j = 0; j < m->nbots; j++) {
        if (j == i) continue;
        bot_t *v = &m->bots[j];
        if (!v->alive || v->falling || v->pit_state == 2) continue;
        if (v->lifted_by == i + 1 && which == 0 && a->build.weapon == WP_LIFTER) continue;
        /* a bot well above or below the weapon is not in reach */
        if (fabs_f((v->b.y) - (a->b.y)) > 0.6f) continue;
        shape_t vs; shape_of(v, &vs);
        contact_t c;
        if (!collide(&ws, &vs, &c)) continue;
        weapon_hit(m, i, j, which, &c);
      }
      /* breakable props: a spinner or a hammer in contact wears a bench or a box down */
      int wid = which ? a->build.weapon2 : a->build.weapon;
      int melty = a->build.drive == DR_MELTY && !which && !a->is_ops;
      if (a->prop_cd == 0 && (melty || wid == WP_HORIZ || wid == WP_DRUM || wid == WP_HAMMER || wid == WP_SAW)) {
        for (int p = 0; p < ar->nprops; p++) {
          const prop_def_t *pd = &ar->props[p];
          if (m->prop_broken[p] || pd->hp <= 0 || m->prop_hp[p] <= 0) continue;
          shape_t ps; shape_aabb(pd->x - pd->w * 0.5f, pd->z - pd->d * 0.5f, pd->x + pd->w * 0.5f, pd->z + pd->d * 0.5f, &ps);
          contact_t c; if (!collide(&ws, &ps, &c)) continue;
          float dmg = melty ? 10.0f : wid == WP_HAMMER ? 28.0f : WEAPONS[wid].base * fmax_f(which ? a->spin2 : a->spin, 0.4f) * 0.5f;
          m->prop_hp[p] -= (int16_t)dmg; a->prop_cd = 18;
          if (m->prop_hp[p] <= 0) {
            m->prop_broken[p] = 1;
            debris_burst_at(m, pd->x, pd->z, pd->kind == PR_BOX ? DB_CARDBOARD : DB_BENCH, pd->kind == PR_BOX ? 1 : 2, 6);
            ev_simple(m, EV_BENCH_BREAK, i, p, pd->kind, pd->x, pd->z, pd->h);
          }
          break;
        }
      }
    }
    flame_step(m, i, 0);
    if (a->build.weapon2 == WP_FLAME) flame_step(m, i, 1);
  }
}

/* a held bot is placed at the holder's front point; keep it inside the closed walls so a lift in a
 * corner never parks the victim through the lexan */
static void clamp_inside(const match_t *m, bot_t *v) {
  const arena_def_t *ar = sim_arena(m);
  float r = fmax_f(v->b.hl, v->b.hw), hw = ar->w * 0.5f - r, hd = ar->d * 0.5f - r;
  if (!(ar->open_mask & OPEN_PX) && v->b.x > hw) v->b.x = hw;
  if (!(ar->open_mask & OPEN_NX) && v->b.x < -hw) v->b.x = -hw;
  if (!(ar->open_mask & OPEN_PZ) && v->b.z > hd) v->b.z = hd;
  if (!(ar->open_mask & OPEN_NZ) && v->b.z < -hd) v->b.z = -hd;
}

/* lifter and crusher holds, every frame (4.6) */
static void holds_step(match_t *m) {
  for (int i = 0; i < m->nbots; i++) {
    bot_t *a = &m->bots[i];
    /* lifting */
    if (a->carry_t > 0) {
      int vic = -1;
      for (int j = 0; j < m->nbots; j++) if (m->bots[j].lifted_by == i + 1) vic = j;
      int release = vic < 0 || !a->alive || a->lift < 0.3f || a->stun > 0 || a->carry_t > WP_LIFTER_MAX_HOLD || a->inverted;
      if (vic >= 0) {
        bot_t *v = &m->bots[vic];
        /* the victim escapes when its push exceeds 1.3x mine */
        if (!a->is_ops && fabs_f(v->drive_t) > 0.5f && build_push(&v->build) > 1.3f * build_push(&a->build)) release = 1;
        if (!release) {
          float fx, fz; fwd_of(a->b.h, &fx, &fz);
          float reach = (a->b.hl + v->b.hl * 0.6f);
          v->b.x = a->b.x + fx * reach; v->b.z = a->b.z + fz * reach;
          clamp_inside(m, v);
          v->b.y = a->floor_y + 0.3f; v->b.vy = 0; v->b.vx = a->b.vx; v->b.vz = a->b.vz; v->b.w = 0;
          v->airborne = 0;
          a->control += 1; v->countout = 0;
          a->carry_t++;
          /* OPS: the saw secondary ticks on the carried bot */
          if (a->is_ops && a->spin2 > 0.5f && a->hit_cd2[vic] == 0) { a->hit_cd2[vic] = WP_SAW_TICK_FRAMES; apply_damage(m, i, vic, WEAPONS[WP_SAW].base, WP_SAW, 6, v->b.x, v->b.z, fx, fz, EV_HIT, 1.0f); }
        } else {
          v->lifted_by = 0; v->b.vy = 0.5f; v->airborne = 1;
          a->carry_t = 0;
        }
      } else a->carry_t = 0;
    }
    /* clamping */
    if (a->build.weapon == WP_CRUSHER && a->lock > 0) {
      for (int j = 0; j < m->nbots; j++) if (m->bots[j].clamped_by == i + 1) {
        bot_t *v = &m->bots[j];
        v->hp -= WP_CRUSHER_HOLD_DPS * DT; a->dealt += WP_CRUSHER_HOLD_DPS * DT; a->control += 1;
        float fx, fz; fwd_of(a->b.h, &fx, &fz);
        v->b.x = a->b.x + fx * (a->b.hl + v->b.hl * 0.9f); v->b.z = a->b.z + fz * (a->b.hl + v->b.hl * 0.9f);
        clamp_inside(m, v);
        v->b.vx = a->b.vx; v->b.vz = a->b.vz; v->b.w = 0;
        if (a->lock == 1) { v->clamped_by = 0; }
        hp_thresholds(m, j);
      }
    }
  }
}

/* ram (4.7): a bot vs bot contact with the attacker's forward within 30 degrees of the normal */
static void ram_check(match_t *m, int i, int j, const contact_t *c) {
  bot_t *a = &m->bots[i], *v = &m->bots[j];
  if (!a->alive || !v->alive || a->is_ops || v->is_ops) return;
  if (!(a->build.weapon == WP_WEDGE || a->build.weapon == WP_NONE || a->build.chassis == CH_WEDGE)) return;
  if (a->build.drive == DR_MELTY) return;
  if (a->hit_cd[j] > 0) return;
  float fx, fz; fwd_of(a->b.h, &fx, &fz);
  if (dotp(fx, fz, c->nx, c->nz) < 0.866f) return;
  float closing = dotp(a->b.vx - v->b.vx, a->b.vz - v->b.vz, c->nx, c->nz);
  if (closing <= RAM_MIN_CLOSING) return;
  float raw = RAM_BASE + RAM_BASE * rtr_clamp((closing - 2.0f) / 4.0f, 0, 1);
  float zone = hit_zone(v, WP_WEDGE, c->cx, c->cz);
  float dmg = apply_damage(m, i, j, raw, WP_WEDGE, 15, c->cx, c->cz, c->nx, c->nz, EV_RAM, zone);
  a->hit_cd[j] = RAM_CD;
  if (dmg <= 0) return;
  if (v->build.drive == DR_TWO && zone >= 1.0f && dmg >= BAND_HIT_MIN) v->wheel_hit = 240;   /* the ram finds the scooter wheel too (3.4) */
  float J = 4.0f * (0.5f + dmg / 30.0f);
  knock(v, fx * J, fz * J);
  /* the wedge hop: a hit on the victim's front or side lifts it 1.5 and disables its drive briefly */
  if ((a->build.chassis == CH_WEDGE || a->build.weapon == WP_WEDGE) && !v->airborne && v->build.drive != DR_HOVER) {
    float vfx, vfz; fwd_of(v->b.h, &vfx, &vfz);
    float dx = c->cx - v->b.x, dz = c->cz - v->b.z;
    if (dotp(dx, dz, vfx, vfz) > -0.3f * v->b.hl) { v->b.vy = 1.5f; v->airborne = 1; }
  }
}

/* -- integration, walls and solids (4.4 step 7 and 8) -------------- */
static void integrate(match_t *m) {
  const arena_def_t *ar = sim_arena(m);
  for (int i = 0; i < m->nbots; i++) {
    bot_t *b = &m->bots[i];
    if (!b->alive && b->hp_max == 0) continue;
    body_t *bd = &b->b;
    if (b->lifted_by || b->clamped_by) { bd->px = bd->x; continue; }
    if (b->falling) {                               /* off the roof or down the pit: just fall */
      bd->vy -= GRAVITY * DT; bd->y += bd->vy * DT; if (bd->y < -6) { bd->y = -6; bd->vy = 0; }
      continue;
    }
    if (b->pit_state == 2) { bd->y -= 2.0f / 30.0f; if (bd->y < -2.5f) bd->y = -2.5f; bd->vx *= 0.8f; bd->vz *= 0.8f; bd->x += bd->vx * DT; bd->z += bd->vz * DT; continue; }
    /* wind */
    if (m->wind_x != 0 || m->wind_z != 0) { float k = b->build.drive == DR_HOVER ? 1.5f : 1.0f; bd->vx += m->wind_x * k * DT; bd->vz += m->wind_z * k * DT; }
    /* drag: a bot that is not driving this step (coasting, stunned, braking, dead) loses 10 percent of its
     * plane velocity per frame, HOVER 1.5 percent (4.4 step 7); a driving bot's forward is controlled in
     * drive_step and its lateral bleeds by traction there */
    if (fabs_f(b->drive_t) < 0.05f || !b->alive) { float k = b->build.drive == DR_HOVER && b->alive && b->skirt_t <= 0 ? 0.985f : 0.90f; bd->vx *= k; bd->vz *= k; }
    if (!b->alive) bd->w *= 0.9f;
    /* the taunt wiggle: yaw plus and minus 15 degrees at 6 Hz */
    bd->x += bd->vx * DT; bd->z += bd->vz * DT;
    bd->h += bd->w * DT;
    bd->h = rtr_wrap_pi(bd->h);
    /* the height channel */
    float fy = arena_floor_y(ar, bd->x, bd->z);
    if (b->airborne) {
      bd->vy -= GRAVITY * DT; bd->y += bd->vy * DT;
      if (bd->y > b->peak_y) b->peak_y = bd->y;
      b->airtime += DT;
      if (bd->y <= fy && bd->vy <= 0) { bd->y = fy; b->floor_y = fy; /* landing() runs in the hazards and checks step */ }
    } else {
      if (fy > bd->y) {
        /* a slope or a lip is climbed; a step over 0.25 (the deck face) is a solid and blocks */
        if (fy - bd->y <= 0.25f) bd->y = fy;
      } else if (fy < bd->y - 0.12f) {
        /* driven off an edge */
        b->airborne = 1; bd->vy = 0; b->peak_y = bd->y;
      } else bd->y = fy;
      b->floor_y = fy;
    }
    /* THE DOCK deck and the mound: on_deck for the count */
    b->on_deck = 0;
    for (int k = 0; k < ar->nhaz; k++) if (ar->haz[k].type == HZ_DECK) {
      const hazard_def_t *h = &ar->haz[k];
      if (!b->airborne && bd->y >= h->h - 0.01f && fabs_f(bd->x - h->x) <= h->w * 0.5f && fabs_f(bd->z - h->z) <= h->d * 0.5f) b->on_deck = 1;
    }
  }
}

static void resolve_static(match_t *m, int slot, const contact_t *c, float e, int is_wall, int sol_kind, int sol_idx) {
  bot_t *b = &m->bots[slot];
  body_t *bd = &b->b;
  float im = bd->inv_mass > 0 ? bd->inv_mass : (b->is_ops ? 1.0f / 6.0f : 2.0f);
  /* the normal points from the body into the wall; move the body out */
  bd->x -= c->nx * c->pen * 0.8f; bd->z -= c->nz * c->pen * 0.8f;
  float vn = dotp(bd->vx, bd->vz, c->nx, c->nz);
  if (vn <= 0) return;
  float mass = 1.0f / im;
  float j = (1.0f + e) * vn * mass;
  float tx = -c->nz, tz = c->nx;
  float vt = dotp(bd->vx, bd->vz, tx, tz);
  float jt = rtr_clamp(vt * mass, -0.4f * j, 0.4f * j);
  float Jx = -(c->nx * j + tx * jt), Jz = -(c->nz * j + tz * jt);
  bd->vx += Jx * im; bd->vz += Jz * im;
  float ox = c->cx - bd->x, oz = c->cz - bd->z;
  bd->w += (ox * Jz - oz * Jx) * bd->inv_inertia * 0.5f;
  /* wall hit events (4.5) */
  if (vn > 2.0f && b->alive && !b->is_ops) {
    int ev = EV_WALL;
    if (b->build.weapon == WP_HORIZ && b->spin > 0.5f) {
      b->spin = 0; b->hp -= WP_HORIZ_WALL_DMG; ev = EV_SPIN_WALL; hp_thresholds(m, slot);
    } else if (b->build.weapon == WP_DRUM) { b->spin *= 0.5f; }
    else if (b->build.drive == DR_MELTY && b->spin > 0.5f) { b->hp -= WP_MELTY_WALL_DMG; hp_thresholds(m, slot); }
    if (vn > 4.0f && ev == EV_WALL) { b->hp -= 2.0f; hp_thresholds(m, slot); }   /* 2 HP over 4 u/s (5.2) */
    b->wall_hits++;
    ev_hit(m, ev, slot, slot, 0, c->cx, c->cz, bd->y + 0.2f, ev == EV_SPIN_WALL ? WP_HORIZ_WALL_DMG : vn > 4.0f ? 2.0f : 0, -c->nx, -c->nz);
    /* a bench or a roof bumper takes the impulse as damage */
    if (!is_wall) {
      const arena_def_t *ar = sim_arena(m);
      if (sol_kind == SOL_PROP && ar->props[sol_idx].hp > 0) {
        m->prop_hp[sol_idx] -= (int16_t)j;
        if (m->prop_hp[sol_idx] <= 0) { m->prop_broken[sol_idx] = 1; debris_burst_at(m, ar->props[sol_idx].x, ar->props[sol_idx].z, DB_BENCH, 2, 6); ev_simple(m, EV_BENCH_BREAK, slot, sol_idx, ar->props[sol_idx].kind, ar->props[sol_idx].x, ar->props[sol_idx].z, ar->props[sol_idx].h); }
      }
      if (sol_kind == SOL_BUMPER && ar->haz[sol_idx].p1 > 0) {
        m->haz[sol_idx].hp -= (int16_t)j;
        if (m->haz[sol_idx].hp <= 0) { m->haz[sol_idx].broken = 1; debris_burst_at(m, ar->haz[sol_idx].x, ar->haz[sol_idx].z, DB_BUMPER, 3, 6); ev_simple(m, EV_BUMPER_BREAK, slot, sol_idx, 0, ar->haz[sol_idx].x, ar->haz[sol_idx].z, 0.4f); }
      }
    }
  }
}

static int in_ops_zone(const arena_def_t *a, float x, float z, float grow) {
  return fabs_f(x - a->ops_zone[0]) <= 1.0f + grow && fabs_f(z - a->ops_zone[1]) <= 1.0f + grow;
}

static void walls_step(match_t *m) {
  const arena_def_t *ar = sim_arena(m);
  solid_t sol[MAX_SOLIDS]; int ns = gather_solids(m, sol);
  float hw = ar->w * 0.5f, hd = ar->d * 0.5f;
  for (int it = 0; it < 2; it++) {
    for (int i = 0; i < m->nbots; i++) {
      bot_t *b = &m->bots[i];
      if ((!b->alive && b->hp_max == 0) || b->lifted_by || b->clamped_by || b->falling || b->pit_state == 2) continue;
      shape_t s; shape_of(b, &s);
      int touched = 0;
      /* the four walls, inward normals; open edges have none */
      struct { float nx, nz, off; int open; } W[4] = {
        { -1, 0, -hw, ar->open_mask & OPEN_PX }, { 1, 0, -hw, ar->open_mask & OPEN_NX },
        { 0, -1, -hd, ar->open_mask & OPEN_PZ }, { 0, 1, -hd, ar->open_mask & OPEN_NZ } };
      for (int k = 0; k < 4; k++) {
        if (W[k].open) continue;
        float cx, cz, pen = wall_pen(&s, W[k].nx, W[k].nz, W[k].off, &cx, &cz);
        if (pen <= 0) continue;
        contact_t c = { -W[k].nx, -W[k].nz, pen, cx, cz };   /* from the body into the wall */
        resolve_static(m, i, &c, 0.3f, 1, 0, 0);
        shape_of(b, &s); touched = 1;
      }
      /* solids: only when the solid's top is above the body's y plus 0.1 */
      for (int k = 0; k < ns; k++) {
        if (sol[k].top <= b->b.y + 0.1f) continue;
        /* a bot caught inside the OPS zone when the gate closes is let out, never trapped */
        if (sol[k].kind == SOL_GATE && !b->is_ops && in_ops_zone(ar, b->b.x, b->b.z, 0.2f)) continue;
        if (sol[k].kind == SOL_BUMPER) {
          /* a post r 0.4: contact impulse 5.0 outward and 4 dmg, 18 frame per bot cooldown (6.2) */
          shape_t ps = { (sol[k].x0 + sol[k].x1) * 0.5f, (sol[k].z0 + sol[k].z1) * 0.5f, 0, 0.4f, 0.4f, 1 };
          contact_t c; if (!collide(&s, &ps, &c)) continue;
          resolve_static(m, i, &c, 0.3f, 0, SOL_BUMPER, sol[k].idx);
          int hi = sol[k].idx;
          if (b->alive && !b->is_ops && b->haz_t[hi] == 0) {
            b->haz_t[hi] = 18;
            knock(b, -c.nx * 5.0f, -c.nz * 5.0f);
            float dmg = apply_damage(m, -1, i, 4.0f, -1, 0, c.cx, c.cz, -c.nx, -c.nz, EV_HAZ_HIT, 1.0f);
            b->hazard_taken += dmg; b->hazard_hits++;
            if (ar->haz[hi].p1 > 0 && !m->haz[hi].broken) { m->haz[hi].hp -= 10; if (m->haz[hi].hp <= 0) { m->haz[hi].broken = 1; debris_burst_at(m, ps.x, ps.z, DB_BUMPER, 3, 6); ev_simple(m, EV_BUMPER_BREAK, i, hi, 0, ps.x, ps.z, 0.4f); } }
          }
          shape_of(b, &s); touched = 1;
          continue;
        }
        shape_t ss; shape_aabb(sol[k].x0, sol[k].z0, sol[k].x1, sol[k].z1, &ss);
        contact_t c; if (!collide(&s, &ss, &c)) continue;
        /* SIX WHEEL climbs 0.4 high benches: it rides up instead of stopping */
        if (b->build.drive == DR_SIX && sol[k].top <= 0.45f && b->alive) continue;   /* it rides over the bench */
        resolve_static(m, i, &c, 0.3f, 0, sol[k].kind, sol[k].idx);
        shape_of(b, &s); touched = 1;
      }
      if (it == 0) b->pinned = (uint8_t)(touched ? (b->pinned | 1) : (b->pinned & ~1));
    }
  }
}

/* -- bot vs bot collision and push (4.5) --------------------------- */
static void bots_collide(match_t *m) {
  for (int it = 0; it < 2; it++) {
    for (int i = 0; i < m->nbots; i++) {
      bot_t *a = &m->bots[i];
      if (a->hp_max == 0 || a->lifted_by || a->clamped_by || a->falling || a->pit_state == 2) continue;
      if (!a->alive && (a->ko_how == KO_PIT || a->ko_how == KO_ROOF)) continue;
      for (int j = i + 1; j < m->nbots; j++) {
        bot_t *b = &m->bots[j];
        if (b->hp_max == 0 || b->lifted_by || b->clamped_by || b->falling || b->pit_state == 2) continue;
        if (!b->alive && (b->ko_how == KO_PIT || b->ko_how == KO_ROOF)) continue;
        if (fabs_f(a->b.y - b->b.y) > 0.35f) continue;        /* one is above the other */
        shape_t sa, sb; shape_of(a, &sa); shape_of(b, &sb);
        contact_t c; if (!collide(&sa, &sb, &c)) continue;
        float ima = a->alive ? a->b.inv_mass : 2.0f, imb = b->alive ? b->b.inv_mass : 2.0f;   /* a dead chassis is a 0.5 mass prop */
        if (a->is_ops) ima = 0; if (b->is_ops) imb = 0;
        float ims = ima + imb;
        if (ims <= 0) continue;
        /* the ram test reads the closing speed as the bots met, before the hover rule below bleeds it
         * off (a wedge that drives into a HOVER rams it like anything else) */
        if (it == 0) ram_check(m, i, j, &c), ram_check(m, j, i, &(contact_t){ -c.nx, -c.nz, c.pen, c.cx, c.cz });
        /* HOVER has push 0 (3.4): its cushion cannot press into a bot. Its velocity into the contact is
         * dropped before the impulse and it takes the whole separation, so a driven bot moves it freely */
        int both = a->alive && b->alive && !a->is_ops && !b->is_ops;   /* a dead chassis or OPS: plain mass split */
        int ha = both && a->build.drive == DR_HOVER, hb = both && b->build.drive == DR_HOVER;
        if (ha) { float v = dotp(a->b.vx, a->b.vz, c.nx, c.nz); if (v > 0) { a->b.vx -= c.nx * v; a->b.vz -= c.nz * v; } a->skirt_t = SKIRT_FRAMES; }
        if (hb) { float v = dotp(b->b.vx, b->b.vz, -c.nx, -c.nz); if (v > 0) { b->b.vx += c.nx * v; b->b.vz += c.nz * v; } b->skirt_t = SKIRT_FRAMES; }
        if (ha != hb && ima > 0 && imb > 0) {
          /* the hover is carried along by the driven bot's normal velocity; the driven bot keeps its own */
          float van = dotp(a->b.vx, a->b.vz, c.nx, c.nz), vbn = dotp(b->b.vx, b->b.vz, c.nx, c.nz);
          if (ha && vbn < van) { a->b.vx += c.nx * (vbn - van); a->b.vz += c.nz * (vbn - van); }
          if (hb && van > vbn) { b->b.vx += c.nx * (van - vbn); b->b.vz += c.nz * (van - vbn); }
        }
        float ka = ima / ims, kb = imb / ims;
        if (ha != hb && ima > 0 && imb > 0) { ka = ha ? 1.0f : 0.0f; kb = 1.0f - ka; }
        a->b.x -= c.nx * c.pen * 0.8f * ka; a->b.z -= c.nz * c.pen * 0.8f * ka;
        b->b.x += c.nx * c.pen * 0.8f * kb; b->b.z += c.nz * c.pen * 0.8f * kb;
        /* OPS never pushes a bot through the lexan: what the wall will not take, OPS gives back */
        if (a->is_ops != b->is_ops) {
          bot_t *o = a->is_ops ? a : b, *v = a->is_ops ? b : a;
          float vx0 = v->b.x, vz0 = v->b.z; clamp_inside(m, v);
          o->b.x += v->b.x - vx0; o->b.z += v->b.z - vz0;
        }
        float rvx = b->b.vx - a->b.vx, rvz = b->b.vz - a->b.vz;
        float vn = dotp(rvx, rvz, c.nx, c.nz);
        if (vn < 0) {
          float e = 0.2f, jn = -(1.0f + e) * vn / ims;
          float tx = -c.nz, tz = c.nx, vt = dotp(rvx, rvz, tx, tz);
          float jt = rtr_clamp(-vt / ims, -0.4f * jn, 0.4f * jn);
          float Jx = c.nx * jn + tx * jt, Jz = c.nz * jn + tz * jt;
          a->b.vx -= Jx * ima; a->b.vz -= Jz * ima;
          b->b.vx += Jx * imb; b->b.vz += Jz * imb;
          float oax = c.cx - a->b.x, oaz = c.cz - a->b.z, obx = c.cx - b->b.x, obz = c.cz - b->b.z;
          if (a->alive) a->b.w -= (oax * Jz - oaz * Jx) * a->b.inv_inertia * 0.5f;
          if (b->alive) b->b.w += (obx * Jz - obz * Jx) * b->b.inv_inertia * 0.5f;
        }
        /* push: both driving into the contact */
        if (it == 0 && a->alive && b->alive) {
          float afx, afz, bfx, bfz; fwd_of(a->b.h, &afx, &afz); fwd_of(b->b.h, &bfx, &bfz);
          float da = a->drive_t * dotp(afx, afz, c.nx, c.nz), db = b->drive_t * dotp(bfx, bfz, -c.nx, -c.nz);
          /* MELTY leans on its skids at half weight; a HOVER cannot hold ground at all (push 0, 3.4) */
          if (a->build.drive == DR_MELTY) da = a->drive_t * 0.5f;
          if (b->build.drive == DR_MELTY) db = b->drive_t * 0.5f;
          if (a->build.drive == DR_HOVER) da = 0;
          if (b->build.drive == DR_HOVER) db = 0;
          if (da > 0.2f && db > 0.2f) {
            float pa = a->is_ops ? 2.0f * 6.0f : build_push(&a->build), pb = b->is_ops ? 2.0f * 6.0f : build_push(&b->build);
            float net = (pa - pb) * 0.5f;             /* an extra force along the normal to both */
            a->b.vx += c.nx * net * ima * DT; a->b.vz += c.nz * net * ima * DT;
            b->b.vx += c.nx * net * imb * DT; b->b.vz += c.nz * net * imb * DT;
            a->push_frames++; b->push_frames++;
            /* a pin: within 1.0 of two walls, in contact with an enemy, speed under 0.3 for 240 frames (2.11) */
            const arena_def_t *ar = sim_arena(m);
            bot_t *pair[2] = { a, b };
            for (int k = 0; k < 2; k++) {
              bot_t *p = pair[k], *q = pair[1 - k];
              float hw = ar->w * 0.5f, hd = ar->d * 0.5f;
              int nw = (!(ar->open_mask & OPEN_PX) && hw - p->b.x < 1.0f) + (!(ar->open_mask & OPEN_NX) && p->b.x + hw < 1.0f) +
                       (!(ar->open_mask & OPEN_PZ) && hd - p->b.z < 1.0f) + (!(ar->open_mask & OPEN_NZ) && p->b.z + hd < 1.0f);
              if (nw >= 2 && len2(p->b.vx, p->b.vz) < 0.3f && !q->is_ops) { p->pin_t++; if (p->pin_t >= 240) { p->pinned |= 2; p->last_hit_by = (uint8_t)((q - m->bots) + 1); p->last_hit_t = 90; } }
            }
          }
        }
      }
    }
  }
}

/* -- landing (4.8) ------------------------------------------------- */
static void landing(match_t *m, int slot) {
  bot_t *b = &m->bots[slot];
  body_t *bd = &b->b;
  float vy = bd->vy;
  int from_flip = b->flip_pending;
  float dmg = 0;
  if (from_flip) dmg = FLIP_LAND_DMG;
  else if (b->peak_y - b->floor_y > 1.2f || b->peak_y >= 0.99f) dmg = 8;
  /* landing on another bot deals 8 to it and pops the lander back 2 u/s */
  for (int j = 0; j < m->nbots; j++) {
    if (j == slot) continue;
    bot_t *o = &m->bots[j];
    if (!o->alive || o->airborne) continue;
    shape_t sa, sb; shape_of(b, &sa); shape_of(o, &sb);
    contact_t c;
    if (collide(&sa, &sb, &c)) {
      apply_damage(m, slot, j, 8.0f, -3, 6, c.cx, c.cz, c.nx, c.nz, EV_HIT, 1.0f);
      float fx, fz; fwd_of(bd->h, &fx, &fz);
      bd->vx -= fx * 2.0f; bd->vz -= fz * 2.0f;
      bd->x -= c.nx * 0.5f; bd->z -= c.nz * 0.5f;
      break;
    }
  }
  int was_inverted = b->inverted;
  if ((from_flip || fabs_f(vy) > 5.0f) && b->alive && !b->is_ops && b->build.drive != DR_HOVER && b->build.drive != DR_MELTY) {
    if (rnd_n(m, 100) < CHASSIS[b->build.chassis].flip_odds) {
      b->inverted = 1;
      if (b->build.chassis == CH_SHELL && rnd_n(m, 100) < 50) { b->inverted = 0; ev_simple(m, EV_SELFRIGHT, slot, slot, 1, bd->x, bd->z, bd->y); }
    }
  }
  if (b->inverted && !was_inverted) {
    b->flips++;
    b->inverted_by = b->launched_by;
    if (b->launched_by) { m->bots[b->launched_by - 1].flips_landed++; }
    ev_simple(m, EV_INVERT, slot, b->launched_by ? b->launched_by - 1 : slot, 0, bd->x, bd->z, bd->y);
    if (b->build.weapon == WP_SAW) b->saw_on = 0;
  }
  if (dmg > 0 && b->alive) {
    float adj = dmg * (1.0f - ARMOURS[b->build.armour].reduction);
    b->hp -= adj; if (b->is_ops && b->hp < 1) b->hp = 1;
    if (b->launched_by && from_flip) { bot_t *a = &m->bots[b->launched_by - 1]; a->dealt += adj; if (adj > a->biggest) a->biggest = adj; }
    ev_hit(m, EV_LAND, b->launched_by ? b->launched_by - 1 : slot, slot, 1.0f, bd->x, bd->z, bd->y, adj, 0, 1);
    hp_thresholds(m, slot);
  } else ev_hit(m, EV_LAND, slot, slot, 1.0f, bd->x, bd->z, bd->y, 0, 0, 1);
  bd->vy = 0; bd->vx *= 0.6f; bd->vz *= 0.6f;
  b->airborne = 0; b->flip_pending = 0; b->launched_by = 0; b->peak_y = bd->y;
}

/* -- hazards (6.2) ------------------------------------------------- */
static int haz_contains(const hazard_def_t *h, float x, float z, float grow) {
  return fabs_f(x - h->x) <= h->w * 0.5f + grow && fabs_f(z - h->z) <= h->d * 0.5f + grow;
}

static void haz_fire(match_t *m, int hi, int seat) {
  const arena_def_t *ar = sim_arena(m);
  const hazard_def_t *h = &ar->haz[hi];
  hazard_state_t *hs = &m->haz[hi];
  if (hs->cooldown > 0 || hs->fire_t > 0) return;
  switch (h->type) {
    case HZ_PULVERIZER: hs->fire_t = 24; hs->cooldown = (int16_t)(seat >= 0 ? h->p1 : h->p0); break;
    case HZ_FLAME: hs->fire_t = 120; hs->cooldown = 360; break;        /* 2 s of fire, then 4 s refill (2.10) */
    case HZ_FLOORFLIP: hs->fire_t = 2; hs->cooldown = (int16_t)h->p1; break;
    default: return;
  }
  ev_simple(m, EV_HAZ_TELL, seat >= 0 ? seat : 255, hi, 1, h->x, h->z, h->h);
}

/* the hazard timeline: phases, tells, fired bursts, the laser and the wind (4.4 step 4) */
static void hazard_step(match_t *m) {
  const arena_def_t *ar = sim_arena(m);
  m->wind_x = 0; m->wind_z = 0;
  for (int i = 0; i < ar->nhaz; i++) {
    const hazard_def_t *h = &ar->haz[i];
    hazard_state_t *hs = &m->haz[i];
    hs->type = h->type;
    if (hs->cooldown > 0) hs->cooldown--;
    int was_active = hs->active, was_tell = hs->tell;
    int ph = m->hazards_on ? hazard_phase(h, m->haz_clock, &hs->phase_t) : 0;
    if (h->period <= 0) ph = 0;
    hs->active = ph == 1; hs->tell = ph == 2;
    if (hs->fire_t > 0) { hs->fire_t--; if (h->type == HZ_PULVERIZER) hs->tell = hs->fire_t > 0; else hs->active = 1; }
    if (!m->hazards_on && h->type != HZ_DECK && h->type != HZ_RAMP && h->type != HZ_MOUND && h->type != HZ_HILL) { hs->active = 0; hs->tell = 0; if (h->type != HZ_PULVERIZER && h->type != HZ_FLAME && h->type != HZ_FLOORFLIP) continue; }
    if (hs->tell && !was_tell && h->type != HZ_PIT) ev_simple(m, EV_HAZ_TELL, 255, i, 0, h->x, h->z, h->h);
    switch (h->type) {
      case HZ_LASER:
        /* the beam sweeps along x over the active 2 s */
        hs->aux = hs->active ? -ar->w * 0.5f + (hs->phase_t / (h->active_s > 0 ? h->active_s : 1)) * ar->w : -100;
        break;
      case HZ_WIND:
        if (hs->active) {
          int gust = (int)((m->haz_clock + h->offset) / h->period);
          int toward_x = (ar->open_mask & OPEN_PX) && (!(ar->open_mask & OPEN_PZ) || (gust & 1) == 0);
          if (toward_x) m->wind_x = 0.5f; else m->wind_z = 0.5f;
          hs->aux = toward_x ? 0 : 1;
        }
        break;
      case HZ_PIT: {
        /* closed until its first opening (T minus 60), then active_s of every period */
        float t = m->haz_clock - hs->aux;
        hs->active = 0; hs->tell = 0;
        if (m->hazards_on) {
          if (t < 0) hs->tell = t > -h->tell_s;
          else { float p = fmodf(t, h->period); hs->phase_t = p; hs->active = p < h->active_s; hs->tell = p > h->period - h->tell_s; }
        }
        if (hs->tell && !was_tell) ev_simple(m, EV_HAZ_TELL, 255, i, 0, h->x, h->z, h->h);
      } break;
      default: break;
    }
    (void)was_active;
  }
}

/* hazards on bots (4.4 step 11) */
static void hazards_on_bots(match_t *m) {
  const arena_def_t *ar = sim_arena(m);
  int one_v_one = sim_is_1v1(m);
  for (int i = 0; i < ar->nhaz; i++) {
    const hazard_def_t *h = &ar->haz[i];
    hazard_state_t *hs = &m->haz[i];
    for (int j = 0; j < m->nbots; j++) {
      bot_t *b = &m->bots[j];
      if (!b->alive || b->is_ops || b->falling || b->pit_state == 2) continue;
      if (b->haz_t[i] > 0 && h->type != HZ_FLOORFLIP && h->type != HZ_DROPZONE && h->type != HZ_PIT) b->haz_t[i]--;
      int hover = b->build.drive == DR_HOVER;
      int grounded = !b->airborne && !b->lifted_by;
      switch (h->type) {
        case HZ_PIT:
          if (hover || !m->hazards_on) break;
          if (!hs->active) { if (b->pit_state == 1) { b->pit_state = 0; b->pit_t = 0; } break; }
          if (!grounded) break;
          if (haz_contains(h, b->b.x, b->b.z, -0.3f)) {
            b->pit_state = 2; b->pit_t = 0; b->b.vy = 0;
            ev_simple(m, EV_HAZ_HIT, j, i, KO_PIT, b->b.x, b->b.z, b->b.y);
          } else if (haz_contains(h, b->b.x, b->b.z, 0.3f + b->b.hw * 0.5f) && DRIVES[b->build.drive].traction <= 0.7f && b->build.drive != DR_WALKER) {
            b->pit_state = 1; b->pit_t++;
            float dx = h->x - b->b.x, dz = h->z - b->b.z, d = len2(dx, dz);
            if (d > 0.01f) { float k = 1.2f * DT * (float)b->pit_t / 90.0f * 4.0f; b->b.vx += dx / d * k; b->b.vz += dz / d * k; }
            if (b->pit_t >= 90) { b->pit_state = 2; b->pit_t = 0; ev_simple(m, EV_HAZ_HIT, j, i, KO_PIT, b->b.x, b->b.z, b->b.y); }
          } else if (b->pit_state == 1) { b->pit_state = 0; b->pit_t = 0; }
          break;
        case HZ_PULVERIZER:
          if (!grounded) break;
          if (haz_contains(h, b->b.x, b->b.z, 0.2f)) {
            /* auto swing in 1v1 when a bot sits under it */
            if (one_v_one && hs->fire_t == 0 && hs->cooldown == 0 && m->hazards_on) haz_fire(m, i, -1);
            if (hs->fire_t == 1) {
              b->b.vy = 2.0f; b->airborne = 1;
              float dmg = apply_damage(m, -1, j, h->dmg, -3, 30, b->b.x, b->b.z, 0, -1, EV_HAZ_HIT, 1.0f);
              b->hazard_taken += dmg; b->hazard_hits++;
              if (hs->seat != 255 && hs->seat < 4) { b->last_hit_t = 90; }
            }
          }
          break;
        case HZ_KILLSAW:
          if (hover || !grounded || !hs->active) break;
          if (haz_contains(h, b->b.x, b->b.z, 0.2f) && b->haz_t[i] == 0) {
            b->haz_t[i] = 15;
            float dmg = apply_damage(m, -1, j, h->dmg * (b->build.drive == DR_TREAD ? 2.0f : 1.0f), WP_SAW, 4, b->b.x, b->b.z, 0, 1, EV_HAZ_HIT, 1.0f);
            b->hazard_taken += dmg; b->hazard_hits++;
          }
          break;
        case HZ_FLAME:
          if (hover || !grounded) break;
          if (haz_contains(h, b->b.x, b->b.z, 0.3f)) {
            if (!hs->active && hs->fire_t == 0 && hs->cooldown == 0 && len2(b->b.vx, b->b.vz) > 3.0f && m->hazards_on) haz_fire(m, i, -1);
            if (hs->active && b->haz_t[i] == 0) {
              b->haz_t[i] = WP_FLAME_TICK_FRAMES;
              float dmg = apply_damage(m, -1, j, h->dmg, WP_FLAME, 0, b->b.x, b->b.z, 0, 1, EV_HAZ_HIT, 1.0f);
              if (!ARMOURS[b->build.armour].burn_immune && b->burn_t <= 0) { b->burn_t = WP_FLAME_BURN_T; b->burn_dps = WP_FLAME_BURN_DPS; }
              b->hazard_taken += dmg; b->hazard_hits++;
            }
          }
          break;
        case HZ_SCREW:
          if (hover || !grounded || b->build.drive == DR_LEGTRACK || !m->hazards_on) break;
          if (haz_contains(h, b->b.x, b->b.z, 0.2f)) {
            /* a conveyor: the strip moves the bot 1.5 u/s along ang on top of its own drive */
            float sx = rtr_sin(h->ang), sz = rtr_cos(h->ang);
            b->b.x += sx * 1.5f * DT; b->b.z += sz * 1.5f * DT;
            if (b->haz_t[i] == 0) { b->haz_t[i] = 30; float dmg = apply_damage(m, -1, j, h->dmg, -1, 0, b->b.x, b->b.z, sx, sz, EV_HAZ_HIT, 1.0f); b->hazard_taken += dmg; b->hazard_hits++; }
          }
          break;
        case HZ_RAMP: {
          /* the top edge launch and the lip hop */
          int inside = haz_contains(h, b->b.x, b->b.z, 0);
          if (h->h <= 0.3f) {
            if (inside && !b->on_ramp && grounded && len2(b->b.vx, b->b.vz) > 3.0f) { b->b.vy = 1.5f; b->airborne = 1; }
            b->on_ramp = (uint8_t)inside;
          } else {
            if (!inside && b->on_ramp && grounded) {
              float sx = rtr_sin(h->ang), sz = rtr_cos(h->ang), sp = len2(b->b.vx, b->b.vz);
              if (sp > 4.0f && dotp(b->b.vx, b->b.vz, sx, sz) > 0 && b->b.y >= h->h - 0.05f) { b->b.vy = sp * 0.8f; b->airborne = 1; b->flip_pending = 1; b->launched_by = 0; ev_simple(m, EV_LAUNCH, j, j, 1, b->b.x, b->b.z, b->b.y); }
            }
            b->on_ramp = (uint8_t)inside;
          }
        } break;
        case HZ_DECK:
          if (b->on_deck) {
            b->deck_count++;
            if (b->deck_count % 60 == 0) ev_simple(m, EV_COUNT_TICK, j, j, 1, b->b.x, b->b.z, b->b.y);
          } else b->deck_count = 0;
          break;
        case HZ_FLOORFLIP:
          if (hover || !grounded) break;
          if (haz_contains(h, b->b.x, b->b.z, 0)) {
            b->haz_t[i]++;
            if ((b->haz_t[i] >= h->p0 && m->hazards_on && hs->cooldown == 0) || hs->fire_t == 1) {
              b->b.vy = 6.0f; b->airborne = 1; b->flip_pending = 1; b->launched_by = 0; b->haz_t[i] = 0;
              if (hs->fire_t != 1) hs->cooldown = (int16_t)h->p1;
              b->hazard_hits++;
              ev_simple(m, EV_HAZ_HIT, j, i, 0, b->b.x, b->b.z, b->b.y);
              ev_simple(m, EV_LAUNCH, j, j, 1, b->b.x, b->b.z, b->b.y);
            }
          } else b->haz_t[i] = 0;
          break;
        case HZ_DROPZONE:
          if (!grounded || !m->hazards_on) break;
          if (haz_contains(h, b->b.x, b->b.z, 0) && len2(b->b.vx, b->b.vz) < 0.3f) {
            b->haz_t[i]++;
            if (b->haz_t[i] == h->p0 - 90) ev_simple(m, EV_HAZ_TELL, j, i, 0, h->x, h->z, 0);
            if (b->haz_t[i] >= h->p0) {
              b->haz_t[i] = 0;
              float dmg = apply_damage(m, -1, j, h->dmg, -3, 30, b->b.x, b->b.z, 0, -1, EV_HAZ_HIT, 1.0f);
              b->hazard_taken += dmg; b->hazard_hits++;
              hs->aux = (float)(((int)hs->aux + 1) % 3);      /* the dropped prop cycles: a CRT, a mini fridge, a Free Pile box */
            }
          } else b->haz_t[i] = 0;
          break;
        case HZ_LASER:
          if (!hs->active || hover || b->build.chassis == CH_LOWBOY || b->airborne) break;
          if (fabs_f(b->b.x - hs->aux) < b->b.hw + 0.2f && b->haz_t[i] == 0) {
            b->haz_t[i] = 120;
            float dmg = apply_damage(m, -1, j, h->dmg, -1, 0, b->b.x, b->b.z, 1, 0, EV_HAZ_HIT, 1.0f);
            b->hazard_taken += dmg; b->hazard_hits++;
          }
          break;
        default: break;
      }
    }
  }
}

/* pits, ring outs, the deck, count-out, thresholds, pickups, debris (4.4 step 12) */
static void checks_step(match_t *m) {
  const arena_def_t *ar = sim_arena(m);
  float hw = ar->w * 0.5f, hd = ar->d * 0.5f;
  for (int i = 0; i < m->nbots; i++) {
    bot_t *b = &m->bots[i];
    /* a dead chassis shoved into an open pit drops out of the arena, for a laugh (4.9) */
    if (!b->alive && b->hp_max > 0 && !b->is_ops && b->ko_how == KO_HP && b->pit_state != 2) {
      for (int h = 0; h < ar->nhaz; h++) if (ar->haz[h].type == HZ_PIT && m->haz[h].active && haz_contains(&ar->haz[h], b->b.x, b->b.z, -0.3f)) { b->pit_state = 2; b->pit_t = 0; }
    }
    if (!b->alive) continue;
    body_t *bd = &b->b;
    /* landing */
    if (b->airborne && bd->y <= b->floor_y + 1e-4f && bd->vy <= 0 && !b->lifted_by) landing(m, i);
    /* ring out: the centre 0.3 beyond an open edge */
    if (!b->is_ops && !b->falling) {
      if (((ar->open_mask & OPEN_PX) && bd->x > hw + 0.3f) || ((ar->open_mask & OPEN_NX) && bd->x < -hw - 0.3f) ||
          ((ar->open_mask & OPEN_PZ) && bd->z > hd + 0.3f) || ((ar->open_mask & OPEN_NZ) && bd->z < -hd - 0.3f)) {
        b->falling = 1; b->airborne = 1; if (bd->vy > 0) bd->vy = 0;
      }
    }
    if (b->pit_state == 2) b->pit_t++;
    /* count-out (2.7) */
    if (!b->is_ops) {
      float moved = len2(bd->x - b->still_x, bd->z - b->still_z);
      b->still_t++;
      if (b->still_t >= 120) { b->still = (uint8_t)(moved < 0.2f); b->still_x = bd->x; b->still_z = bd->z; b->still_t = 0; }
      float speed = len2(bd->vx, bd->vz);
      if (speed > 0.5f) { b->fast_t++; if (b->fast_t >= 10) { b->countout = 0; } } else b->fast_t = 0;
      int inv_stuck = b->inverted && !b->airborne && b->build.drive != DR_HOVER && b->build.drive != DR_MELTY && !WEAPONS[b->build.weapon].inv_drive;
      int counting = inv_stuck || (b->still && !b->airborne && !b->lifted_by && !b->clamped_by && speed < 0.5f);
      if (m->park && b->is_ai) counting = 0;          /* a parked practice bot under the rig */
      if (counting) {
        b->countout++;
        if (b->countout % 60 == 0 && b->countout < COUNTOUT_FRAMES) ev_simple(m, EV_COUNT_TICK, i, i, 10 - b->countout / 60, bd->x, bd->z, bd->y);
      } else if (!b->inverted && b->fast_t >= 10) b->countout = 0;
      if (b->inverted) b->frames_inverted++;
      /* the wall wedge nudge: |thr| > 0.5 against a wall with no displacement for 120 frames */
      if ((b->pinned & 1) && fabs_f(b->drive_t) > 0.5f && moved < 0.05f && !b->airborne) {
        b->stuck_t++;
        if (b->stuck_t >= 120) {
          float nx, nz, dist;
          if (arena_nearest_wall(ar, bd->x, bd->z, &nx, &nz, &dist)) {
            float tx = nz, tz = -nx;
            if (dotp(tx, tz, -bd->x, -bd->z) < 0) { tx = -tx; tz = -tz; }
            bd->x += tx * 0.05f; bd->z += tz * 0.05f;
            m->stuck_frames++;
            if (b->stuck_t % 60 == 0) ev_simple(m, EV_STUCK, i, i, 0, bd->x, bd->z, bd->y);
          }
        }
      } else b->stuck_t = 0;
      /* self right (4.8) */
      if (b->selfright > 0) {
        b->selfright--;
        if (b->selfright == 0) {
          int ok = 1;
          if (b->build.weapon == WP_DRUM) ok = rnd_n(m, 100) < 80;
          if (ok) { b->inverted = 0; bd->vy = 1.0f; b->airborne = 1; b->countout = 0; ev_simple(m, EV_SELFRIGHT, i, i, 0, bd->x, bd->z, bd->y); }
        }
      } else if (b->inverted && !b->airborne && !b->lifted_by) {
        const weapon_def_t *w = &WEAPONS[b->build.weapon];
        if (w->selfright == SR_B && btn_edge(&b->in, &b->prev_in, IN_B)) b->selfright = 90;
        if (w->selfright == SR_DRUM && b->spin > 0.5f && btn_edge(&b->in, &b->prev_in, IN_A)) { b->selfright = 45; b->spin = 0; }
      }
      /* burn: the armour takes its share like any other damage (a bare 2 dps on a STEEL bot was the
       * flame's whole case against it) */
      if (b->burn_t > 0) {
        b->burn_t -= 1.0f; float d = b->burn_dps * DT * (1.0f - ARMOURS[b->build.armour].reduction); b->hp -= d;
        if (b->last_hit_by && b->last_hit_by - 1 < m->nbots) { bot_t *a = &m->bots[b->last_hit_by - 1]; a->burn_dealt += d; a->dealt += d; }
        hp_thresholds(m, i);
      }
    }
    /* judges tallies (2.5): aggression and control, per frame */
    if (!b->is_ops) {
      for (int j = 0; j < m->nbots; j++) {
        if (j == i) continue;
        bot_t *o = &m->bots[j];
        if (!o->alive || o->is_ops || (m->mode == MODE_2V2 && o->team == b->team)) continue;
        float dx = o->b.x - bd->x, dz = o->b.z - bd->z, d = len2(dx, dz);
        if (d < 2.0f && d > 0.01f && dotp(bd->vx - o->b.vx, bd->vz - o->b.vz, dx / d, dz / d) > 0.5f) b->aggro += 1;
        if (o->inverted && o->inverted_by == i + 1) b->control += 1;
      }
    }
  }
  /* pickups (2.9) */
  if (m->comeback) {
    for (int d = 0; d < m->ndebris; d++) {
      debris_t *db = &m->debris[d];
      if (!db->pickup || db->life <= 0) continue;
      int taken = 0;
      for (int i = 0; i < m->nbots && !taken; i++) {
        bot_t *b = &m->bots[i];
        if (!b->alive || b->is_ops || b->airborne) continue;
        shape_t s; shape_of(b, &s);
        float qx, qz;
        if (s.circle) { float ddx = db->x - s.x, ddz = db->z - s.z, l = len2(ddx, ddz); float k = l > s.hl ? s.hl / l : 1; qx = s.x + ddx * k; qz = s.z + ddz * k; }
        else box_closest(&s, db->x, db->z, &qx, &qz);
        if (len2(db->x - qx, db->z - qz) > 0.4f) continue;
        if (b->build.drive == DR_MELTY) {
          ev_simple(m, EV_NOT_VACUUM, i, i, b->not_vacuum_said ? 0 : 1, db->x, db->z, db->y);
          b->not_vacuum_said = 1; taken = 1;
        } else if (b->pickups < 3) {
          b->hp = fmin_f(b->hp + 6.0f, b->hp_max); b->pickups++;
          ev_simple(m, EV_PICKUP, i, i, b->pickups == 1 ? 1 : 0, db->x, db->z, db->y);
          taken = 1;
        }
      }
      if (taken) { debris_remove(m, d); d--; }
    }
  }
  /* debris integration: gravity 20, one bounce at e 0.3, slide with drag 0.9, recycled at 0 life */
  for (int d = 0; d < m->ndebris; d++) {
    debris_t *db = &m->debris[d];
    float fy = arena_floor_y(ar, db->x, db->z);
    db->vy -= GRAVITY * DT; db->y += db->vy * DT; db->x += db->vx * DT; db->z += db->vz * DT;
    if (db->y <= fy) {
      db->y = fy;
      if (!db->bounced && db->vy < -1.0f) { db->vy = -db->vy * 0.3f; db->bounced = 1; }
      else { db->vy = 0; db->vx *= 0.9f; db->vz *= 0.9f; db->spin *= 0.9f; }
    }
    if (db->x < -hw) db->x = -hw; if (db->x > hw) db->x = hw; if (db->z < -hd) db->z = -hd; if (db->z > hd) db->z = hd;
    db->life--;
    if (db->life <= 0) { debris_remove(m, d); d--; }
  }
}

/* -- OPS (section 7) ----------------------------------------------- */
static void ops_steer(bot_t *o, float tx, float tz, float throttle) {
  float want = rtr_atan2(tx - o->b.x, tz - o->b.z);
  float err = rtr_wrap_pi(want - o->b.h);
  o->in.ly = (int16_t)(-32767.0f * throttle * (fabs_f(err) < 1.2f ? 1.0f : 0.4f));
  o->in.lx = (int16_t)(-32767.0f * rtr_clamp(err * 2.0f, -1.0f, 1.0f));   /* a positive error is a left turn: stick left */
}

static int hp_leader(const match_t *m, int exclude) {
  int best = -1; float hp = -1e9f;        /* a double KO still names the bot that kept more of its bar */
  for (int i = 0; i < m->nbots; i++) {
    const bot_t *b = &m->bots[i];
    if (!b->alive || b->is_ops || i == exclude) continue;
    if (b->hp > hp) { hp = b->hp; best = i; }
  }
  return best;
}

static void ops_step(match_t *m) {
  if (m->nbots <= OPS_SLOT || !m->bots[OPS_SLOT].is_ops) return;
  bot_t *o = &m->bots[OPS_SLOT];
  const arena_def_t *ar = sim_arena(m);
  float zx = ar->ops_zone[0], zz = ar->ops_zone[1];
  int leaving_time = m->clock_s <= 10.0f;
  if (m->ops_t > 0) m->ops_t--;
  /* punish: more than 20 damage from one bot within 300 frames */
  if (o->ops_dmg_t > 0) { o->ops_dmg_t--; if (o->ops_dmg_t == 0) o->ops_dmg = 0; }
  if (o->ops_dmg > 20 && m->ops_state != OPS_PARKED && m->ops_state != OPS_PUNISH && m->ops_state != OPS_LEAVING && o->last_hit_by) {
    m->ops_state = OPS_PUNISH; m->ops_punish_t = 300; m->ops_punish_target = (uint8_t)(o->last_hit_by - 1); o->ops_dmg = 0;
    ev_simple(m, EV_OPS_LINE, OPS_SLOT, m->ops_punish_target, rnd_n(m, 6), o->b.x, o->b.z, o->b.y);
  }
  if (!m->ops_driven) memset(&o->in, 0, sizeof o->in);
  else { o->in = m->ops_in_drive; }
  switch (m->ops_state) {
    case OPS_PARKED: {
      o->b.vx = o->b.vz = 0; o->b.w = 0;
      /* a bot left in the zone when the gate closed is eased out toward the arena centre */
      for (int i = 0; i < m->nbots; i++) {
        bot_t *b = &m->bots[i];
        if (b->is_ops || !b->alive || !in_ops_zone(ar, b->b.x, b->b.z, 0.6f)) continue;
        float dx = -b->b.x, dz = -b->b.z, d = len2(dx, dz);
        if (d > 0.1f) { b->b.vx += dx / d * 0.4f; b->b.vz += dz / d * 0.4f; }
      }
      if (leaving_time || m->over) break;
      int pinner = -1;
      for (int i = 0; i < m->nbots; i++) if (m->bots[i].alive && !m->bots[i].is_ops && (m->bots[i].pinned & 2) && m->bots[i].last_hit_by) pinner = m->bots[i].last_hit_by - 1;
      int wake = m->no_hit_frames >= 720 || pinner >= 0;
      if (m->ops_driven) wake = wake || (m->ops_in_drive.ly < -16000);
      if (!wake) break;
      int target = pinner >= 0 && pinner < m->nbots && m->bots[pinner].alive ? pinner : hp_leader(m, -1);
      if (target < 0) break;
      m->ops_target = (uint8_t)target; m->ops_state = OPS_GO; m->gate_open = 1; m->ops_t = 420; m->ops_awake_once = 1;
      m->no_hit_frames = 0;
      for (int i = 0; i < m->nbots; i++) m->bots[i].pinned &= 1, m->bots[i].pin_t = 0;
      ev_simple(m, EV_OPS_WAKE, OPS_SLOT, target, 0, o->b.x, o->b.z, o->b.y);
      ev_simple(m, EV_OPS_LINE, OPS_SLOT, target, rnd_n(m, 6), o->b.x, o->b.z, o->b.y);
      break;
    }
    case OPS_GO: case OPS_PUNISH: {
      int target = m->ops_state == OPS_PUNISH ? m->ops_punish_target : m->ops_target;
      if (m->ops_state == OPS_PUNISH) { if (--m->ops_punish_t <= 0) { m->ops_state = OPS_RETURN; break; } }
      if (target >= m->nbots || !m->bots[target].alive || leaving_time || m->ops_t == 0) { m->ops_state = OPS_RETURN; break; }
      bot_t *t = &m->bots[target];
      if (!m->ops_driven) { ops_steer(o, t->b.x, t->b.z, 1.0f); o->in.buttons |= IN_A; }
      if (t->lifted_by == OPS_SLOT + 1) {
        m->ops_state = m->ops_state == OPS_PUNISH ? OPS_PUNISH : OPS_LIFT; o->carry_t = 1;
        if (m->ops_state == OPS_PUNISH) {
          /* a 40 damage lift and shove */
          apply_damage(m, OPS_SLOT, target, 40.0f, WP_LIFTER, 30, t->b.x, t->b.z, rtr_sin(o->b.h), rtr_cos(o->b.h), EV_HIT, 1.5f);
          t->lifted_by = 0; o->carry_t = 0; o->lift = 0; t->airborne = 1; t->b.vy = 2.0f;
          knock(t, rtr_sin(o->b.h) * 6.0f, rtr_cos(o->b.h) * 6.0f);
          m->ops_state = OPS_RETURN;
        } else {
          float dx = -t->b.x, dz = -t->b.z, d = len2(dx, dz);
          if (d < 0.1f) { dx = 1; dz = 0; d = 1; }
          m->ops_carry_x = t->b.x + dx / d * 3.0f; m->ops_carry_z = t->b.z + dz / d * 3.0f;
          m->ops_state = OPS_CARRY; o->carry_t = 1;
        }
      }
      break;
    }
    case OPS_LIFT: m->ops_state = OPS_CARRY; break;
    case OPS_CARRY: {
      bot_t *t = &m->bots[m->ops_target];
      if (!m->ops_driven) { ops_steer(o, m->ops_carry_x, m->ops_carry_z, 0.8f); o->in.buttons |= IN_A | IN_X; }
      int there = len2(o->b.x - m->ops_carry_x, o->b.z - m->ops_carry_z) < 1.0f;
      if (t->lifted_by != OPS_SLOT + 1 || there || o->carry_t > 170 || leaving_time) {
        if (t->lifted_by == OPS_SLOT + 1) { t->lifted_by = 0; t->airborne = 1; t->b.vy = 0.5f; }
        o->carry_t = 0; o->lift = 0; o->saw_on = 0; m->ops_state = OPS_RETURN;
      }
      break;
    }
    case OPS_RETURN: case OPS_LEAVING: {
      /* a bot sitting in the zone: wait outside the gate so it is never trapped in the corner */
      int occupied = 0;
      for (int i = 0; i < m->nbots; i++) if (!m->bots[i].is_ops && m->bots[i].alive && in_ops_zone(ar, m->bots[i].b.x, m->bots[i].b.z, 0.5f)) occupied = 1;
      if (occupied) {
        float d = len2(zx, zz), wx = zx - zx / (d > 0.1f ? d : 1) * 6.0f, wz = zz - zz / (d > 0.1f ? d : 1) * 6.0f;
        if (!m->ops_driven) { if (len2(o->b.x - wx, o->b.z - wz) > 0.5f) ops_steer(o, wx, wz, 0.6f); else memset(&o->in, 0, sizeof o->in); }
        break;
      }
      if (!m->ops_driven) ops_steer(o, zx, zz, 1.0f);
      if (len2(o->b.x - zx, o->b.z - zz) < 0.6f) {
        m->ops_state = OPS_PARKED; m->gate_open = 0; m->no_hit_frames = 0; o->b.vx = o->b.vz = 0;
        o->b.x = zx; o->b.z = zz;
        ev_simple(m, EV_OPS_LEAVE, OPS_SLOT, OPS_SLOT, 0, o->b.x, o->b.z, o->b.y);
      }
      break;
    }
    default: m->ops_state = OPS_PARKED;
  }
  if (leaving_time && m->ops_state != OPS_PARKED && m->ops_state != OPS_RETURN) {
    for (int i = 0; i < m->nbots; i++) if (m->bots[i].lifted_by == OPS_SLOT + 1) { m->bots[i].lifted_by = 0; m->bots[i].airborne = 1; }
    o->carry_t = 0; o->lift = 0; m->ops_state = OPS_LEAVING;
  }
}

/* -- KING OF THE HILL (2.2, 6.2) ----------------------------------- */
static void hill_step(match_t *m) {
  if (m->mode != MODE_HILL) return;
  const arena_def_t *ar = sim_arena(m);
  m->hill_t++;
  if (m->hill_t == 1200 - 90) ev_simple(m, EV_HAZ_TELL, 255, 200 + (m->hill + 1) % 4, 0, ar->hill[(m->hill + 1) % 4][0], ar->hill[(m->hill + 1) % 4][1], 0);
  if (m->hill_t >= 1200) { m->hill_t = 0; m->hill = (m->hill + 1) % 4; m->hill_alone_t = 0; ev_simple(m, EV_HILL_MOVE, 255, m->hill, 0, ar->hill[m->hill][0], ar->hill[m->hill][1], 0); }
  float hx = ar->hill[m->hill][0], hz = ar->hill[m->hill][1];
  int on = -1, count = 0;
  for (int i = 0; i < m->nbots; i++) {
    bot_t *b = &m->bots[i];
    if (!b->alive || b->is_ops || b->airborne) continue;
    if (fabs_f(b->b.x - hx) <= 1.5f && fabs_f(b->b.z - hz) <= 1.5f) { on = i; count++; }
  }
  if (count == 1) {
    m->hill_alone_t++;
    if (m->hill_alone_t % 60 == 0) { int seat = m->bots[on].seat; if (seat < 4) m->score[seat]++; ev_simple(m, EV_HILL_SCORE, on, on, m->score[seat & 3], hx, hz, 0); }
  } else m->hill_alone_t = 0;
  /* respawns: in your corner after 5 s with 60 HP */
  for (int i = 0; i < m->nbots; i++) {
    bot_t *b = &m->bots[i];
    if (b->alive || b->is_ops || b->hp_max == 0) continue;
    if (b->respawn_t > 0) { b->respawn_t--; if (b->respawn_t == 0) {
      const float *sp = m->players == 2 && sim_is_1v1(m) ? ar->spawn2[b->seat & 1] : ar->spawn4[b->seat & 3];
      build_t bd = b->build; int seat = b->seat, is_ai = b->is_ai, team = b->team, scheme = b->scheme;
      int sc[4]; memcpy(sc, m->score, sizeof sc);
      float dealt = b->dealt, biggest = b->biggest, aggro = b->aggro, control = b->control, air = b->airtime; int fi = b->frames_inverted; uint8_t fl = b->flips, ta = b->taunts;
      uint8_t rtu = b->red_tag_used, pk = b->pickups, nv = b->not_vacuum_said, fll = b->flips_landed, hh = b->hazard_hits, wh = b->wall_hits, cl = b->clashes;
      float sd = b->spinner_dealt, bd2 = b->burn_dealt, ht = b->hazard_taken;
      bot_setup(b, &bd, seat, is_ai, team, sp[0], sp[1], sp[2]);
      b->scheme = (uint8_t)scheme; b->hp = 60; b->dealt = dealt; b->biggest = biggest; b->aggro = aggro; b->control = control; b->airtime = air; b->frames_inverted = (int16_t)fi; b->flips = fl; b->taunts = ta;
      /* once per match stays once per match across a respawn (2.8, 2.9) */
      b->red_tag_used = rtu; b->pickups = pk; b->not_vacuum_said = nv; b->flips_landed = fll; b->hazard_hits = hh; b->wall_hits = wh; b->clashes = cl;
      b->spinner_dealt = sd; b->burn_dealt = bd2; b->hazard_taken = ht;
      hp_thresholds(m, i);
      ev_simple(m, EV_RESPAWN, i, i, 0, b->b.x, b->b.z, 0);
    } }
  }
}

/* -- win conditions (2.4) ------------------------------------------ */
static void eliminate(match_t *m, int slot, int cause, int by) {
  bot_t *b = &m->bots[slot];
  if (!b->alive) return;
  b->alive = 0; b->ko_how = (uint8_t)cause;
  if (cause == KO_HP) burst(m, slot);
  if (cause == KO_PIT || cause == KO_ROOF) { b->falling = 1; }
  if (b->lifted_by) b->lifted_by = 0;
  if (b->clamped_by) b->clamped_by = 0;
  for (int i = 0; i < m->nbots; i++) { if (m->bots[i].lifted_by == slot + 1) { m->bots[i].lifted_by = 0; m->bots[i].airborne = 1; } if (m->bots[i].clamped_by == slot + 1) m->bots[i].clamped_by = 0; }
  b->carry_t = 0; b->lock = 0; b->saw_on = 0; b->spin = 0;
  m->ko_count++;
  /* who gets the credit: the last bot to hit it; the referee never takes a KO, so a bot that died in
   * OPS's forks credits the player who hit it before that, else nobody */
  int attacker = by >= 0 ? by : (b->last_hit_by ? b->last_hit_by - 1 : 255);
  if (attacker < m->nbots && attacker != 255 && m->bots[attacker].is_ops) {
    attacker = 255;
    for (int i = 0; i < m->nbots; i++) if (i != slot && !m->bots[i].is_ops && m->bots[i].hp_max > 0 && m->bots[i].dealt > 0 && (attacker == 255 || m->bots[i].dealt > m->bots[attacker].dealt)) attacker = i;
  }
  /* alive bots left, not OPS */
  int alive = 0, humans = 0, humans_ever = 0;
  for (int i = 0; i < m->nbots; i++) if (!m->bots[i].is_ops && m->bots[i].hp_max > 0) {
    if (!m->bots[i].is_ai) humans_ever++;
    if (m->bots[i].alive) { alive++; if (!m->bots[i].is_ai) humans++; }
  }
  /* a KO that leaves only AI bots ends the match (the humans lost); an all AI match (the attract,
   * the balance tool) plays on to the last bot */
  int humans_out = humans_ever > 0 && humans == 0;
  int final = 0;
  if (m->mode == MODE_HILL) { b->respawn_t = 300; final = 0; }
  else if (m->mode == MODE_2V2) {
    int team_alive[2] = { 0, 0 };
    for (int i = 0; i < m->nbots; i++) if (m->bots[i].alive && !m->bots[i].is_ops) team_alive[m->bots[i].team & 1]++;
    final = team_alive[0] == 0 || team_alive[1] == 0;
  } else final = alive <= 1 || humans_out;
  if (final) {
    m->over = 1; m->win_how = (uint8_t)cause; m->ko_frame = m->frame;
    if (m->mode == MODE_2V2) { m->winner = 255; for (int i = 0; i < m->nbots; i++) if (m->bots[i].alive && !m->bots[i].is_ops && m->bots[i].team != b->team) { m->winner = (uint8_t)i; break; } }
    else if (m->mode == MODE_HILL) { int best = -1; for (int s = 0; s < 4; s++) if (best < 0 || m->score[s] > m->score[best]) best = s; m->winner = 255; for (int i = 0; i < m->nbots; i++) if (m->bots[i].seat == best && !m->bots[i].is_ops) m->winner = (uint8_t)i; }
    else { int w = hp_leader(m, -1); m->winner = (uint8_t)(w < 0 ? 255 : w); }
    ev_hit(m, EV_KO, attacker, slot, (float)cause / 10.0f, b->b.x, b->b.z, b->b.y, 0, 0, 1);
    if (!m->hitstop_off && m->hitstop < STOP_KO) m->hitstop = STOP_KO;
    /* placements: the KO'd bot is last among those still unplaced */
    b->place = (uint8_t)(alive + 1);
    for (int i = 0; i < m->nbots; i++) if (m->bots[i].alive && !m->bots[i].is_ops) m->bots[i].place = 1;
  } else {
    b->place = (uint8_t)(alive + 1);
    ev_hit(m, EV_MINI_KO, attacker, slot, (float)cause / 10.0f, b->b.x, b->b.z, b->b.y, 0, 0, 1);
  }
}

int sim_judges(const match_t *m, float damage[MAX_BOTS], float aggro[MAX_BOTS], float control[MAX_BOTS], int *rule) {
  float td = 0, ta = 0, tc = 0;
  for (int i = 0; i < m->nbots; i++) { const bot_t *b = &m->bots[i]; damage[i] = aggro[i] = control[i] = 0; if (b->is_ops || b->hp_max == 0) continue; td += b->dealt; ta += b->aggro; tc += b->control; }
  int best = -1; float bs = -1; int tie = 0;
  for (int i = 0; i < m->nbots; i++) {
    const bot_t *b = &m->bots[i];
    if (b->is_ops || b->hp_max == 0) continue;
    damage[i] = td > 0 ? floorf(b->dealt / td * 70.0f + 0.5f) / 10.0f : 0;
    aggro[i] = ta > 0 ? floorf(b->aggro / ta * 40.0f + 0.5f) / 10.0f : 0;
    control[i] = tc > 0 ? floorf(b->control / tc * 40.0f + 0.5f) / 10.0f : 0;
    float s = damage[i] + aggro[i] + control[i];
    if (!b->alive && m->mode != MODE_HILL) s -= 100;      /* an eliminated bot cannot win on points */
    if (s > bs + 1e-4f) { bs = s; best = i; tie = 0; } else if (fabs_f(s - bs) <= 1e-4f) tie = 1;
  }
  if (rule) *rule = 0;
  if (!tie || best < 0) return best;
  /* the tie chain: cheaper build, fewer frames inverted, draw (2.5) */
  int cands[MAX_BOTS], n = 0;
  for (int i = 0; i < m->nbots; i++) { const bot_t *b = &m->bots[i]; if (b->is_ops || b->hp_max == 0) continue; float s = damage[i] + aggro[i] + control[i] - (!b->alive && m->mode != MODE_HILL ? 100 : 0); if (fabs_f(s - bs) <= 1e-4f) cands[n++] = i; }
  int cheapest = cands[0], cheap_tie = 0;
  for (int k = 1; k < n; k++) { int c = build_cost(&m->bots[cands[k]].build), cc = build_cost(&m->bots[cheapest].build); if (c < cc) { cheapest = cands[k]; cheap_tie = 0; } else if (c == cc) cheap_tie = 1; }
  if (!cheap_tie) { if (rule) *rule = 1; return cheapest; }
  int fewest = cands[0], flip_tie = 0;
  for (int k = 1; k < n; k++) { int f = m->bots[cands[k]].frames_inverted, ff = m->bots[fewest].frames_inverted; if (f < ff) { fewest = cands[k]; flip_tie = 0; } else if (f == ff) flip_tie = 1; }
  if (!flip_tie) { if (rule) *rule = 2; return fewest; }
  if (rule) *rule = 3;
  return -1;
}

static void win_check(match_t *m) {
  if (m->over) return;
  for (int i = 0; i < m->nbots; i++) {
    bot_t *b = &m->bots[i];
    if (!b->alive || b->is_ops) continue;
    if (m->mode != MODE_KO_FLIP && b->hp <= 0) { eliminate(m, i, KO_HP, -1); if (m->over) return; continue; }
    if (b->pit_state == 2 && b->pit_t >= 30) { eliminate(m, i, KO_PIT, -1); if (m->over) return; continue; }
    if (b->falling) { eliminate(m, i, KO_ROOF, -1); if (m->over) return; continue; }
    if (b->deck_count >= DECK_FRAMES) { eliminate(m, i, KO_DECK, -1); if (m->over) return; continue; }
    if (b->countout >= COUNTOUT_FRAMES) { eliminate(m, i, b->inverted ? KO_FLIP : KO_COUNT, -1); if (m->over) return; continue; }
  }
  if (m->mode == MODE_HILL) {
    for (int s = 0; s < 4; s++) if (m->score[s] >= 30) {
      m->over = 1; m->win_how = WIN_HILL; m->winner = 255; m->ko_frame = m->frame;
      for (int i = 0; i < m->nbots; i++) if (m->bots[i].seat == s && !m->bots[i].is_ops) m->winner = (uint8_t)i;
      return;
    }
  }
  if (m->clock_s <= 0) {
    m->over = 1; m->ko_frame = m->frame;
    if (m->mode == MODE_HILL) {
      int best = -1, tie = 0;
      for (int s = 0; s < 4; s++) { if (best < 0 || m->score[s] > m->score[best]) { best = s; tie = 0; } else if (m->score[s] == m->score[best] && m->score[s] > 0) tie = 1; }
      m->win_how = WIN_HILL; m->winner = 255;
      if (!tie && best >= 0 && m->score[best] > 0) { for (int i = 0; i < m->nbots; i++) if (m->bots[i].seat == best && !m->bots[i].is_ops) m->winner = (uint8_t)i; return; }
    }
    float d[MAX_BOTS], a[MAX_BOTS], c[MAX_BOTS]; int rule;
    int w = sim_judges(m, d, a, c, &rule);
    m->win_how = WIN_JUDGES; m->winner = (uint8_t)(w < 0 ? 255 : w);
    if (m->mode == MODE_2V2 && w >= 0) {
      /* the team with the higher pool wins; the slot returned is a member of it */
      float pool[2] = { 0, 0 };
      for (int i = 0; i < m->nbots; i++) if (!m->bots[i].is_ops && m->bots[i].hp_max > 0 && m->bots[i].alive) pool[m->bots[i].team & 1] += m->bots[i].hp;
      int t = pool[0] > pool[1] ? 0 : pool[1] > pool[0] ? 1 : m->bots[w].team & 1;
      for (int i = 0; i < m->nbots; i++) if (!m->bots[i].is_ops && m->bots[i].hp_max > 0 && (m->bots[i].team & 1) == t) { m->winner = (uint8_t)i; break; }
    }
    for (int i = 0; i < m->nbots; i++) if (!m->bots[i].is_ops && m->bots[i].hp_max > 0) m->bots[i].place = (uint8_t)(i == m->winner ? 1 : 2);
  }
}

/* -- timers (4.4 step 3) ------------------------------------------- */
static void timers_step(match_t *m) {
  m->no_hit_frames++;
  for (int i = 0; i < m->nbots; i++) {
    bot_t *b = &m->bots[i];
    if (b->hp_max == 0) continue;
    if (b->stun > 0) b->stun--;
    if (b->cooldown > 0) b->cooldown--;
    if (b->cooldown2 > 0) b->cooldown2--;
    if (b->boost > 0) b->boost--;
    if (b->boost_cd > 0) b->boost_cd--;
    if (b->brake > 0) b->brake--;
    if (b->taunt > 0) b->taunt--;
    if (b->red_tag_t > 0) b->red_tag_t--;
    if (b->wheel_hit > 0) b->wheel_hit--;
    if (b->lock > 0) b->lock--;
    if (b->last_hit_t > 0) b->last_hit_t--;
    if (b->prop_cd > 0) b->prop_cd--;
    if (b->skirt_t > 0) b->skirt_t--;
    if (b->pin_t > 0 && len2(b->b.vx, b->b.vz) >= 0.3f) b->pin_t = 0;
    for (int j = 0; j < MAX_BOTS; j++) { if (b->hit_cd[j] > 0) b->hit_cd[j]--; if (b->hit_cd2[j] > 0) b->hit_cd2[j]--; }
  }
}

/* -- setup --------------------------------------------------------- */
void sim_init(match_t *m, const match_setup_t *s) {
  memset(m, 0, sizeof *m);
  m->mode = s->mode; m->arena = s->arena < ARENA_COUNT ? s->arena : 0; m->players = s->players; m->comeback = s->comeback ? 1 : 0;
  m->fight_len_s = s->fight_len_s ? s->fight_len_s : 75;
  m->clock_s = m->mode == MODE_HILL ? 120.0f : (float)m->fight_len_s;
  m->seed = s->seed; m->rng = s->seed ? s->seed : 0x9e3779b9u;
  m->hazards_on = !s->hazards_off; m->hitstop_off = s->hitstop_off; m->park = s->park;
  m->winner = 255; m->ops_target = 255;
  for (int i = 0; i < 4; i++) m->hazard_seat[i] = 255;
  const arena_def_t *ar = sim_arena(m);
  int players = s->players < 1 ? 1 : s->players > 4 ? 4 : s->players;
  /* bots by seat: slot i is seat i; a 1v1 uses spawn2 */
  int nb = 0;
  int one_v_one = players == 2;
  for (int i = 0; i < players; i++) {
    const float *sp = one_v_one ? ar->spawn2[i & 1] : ar->spawn4[i & 3];
    build_t b = s->builds[i];
    if (b.chassis >= CH_COUNT || b.drive >= DR_COUNT || b.weapon >= WP_COUNT || b.armour >= AR_COUNT || b.weapon2 >= WP_COUNT) build_preset(&b, PRE_ROLL_OUT);
    bot_setup(&m->bots[i], &b, i, s->is_ai[i], s->mode == MODE_2V2 ? s->team[i] & 1 : (uint8_t)i, sp[0], sp[1], sp[2]);
    m->bots[i].scheme = s->scheme[i] ? SCHEME_TANK : SCHEME_ARCADE;
    m->bots[i].floor_y = arena_floor_y(ar, sp[0], sp[1]);
    m->bots[i].b.y = m->bots[i].b.py = m->bots[i].floor_y;
    nb = i + 1;
  }
  /* OPS parks in its taped zone facing the arena centre; bots[4] whatever the player count */
  if (!s->no_ops) {
    float zx = ar->ops_zone[0], zz = ar->ops_zone[1];
    bot_setup_ops(&m->bots[OPS_SLOT], zx, zz, rtr_atan2(-zx, -zz), 0);
    m->bots[OPS_SLOT].floor_y = 0;
    nb = OPS_SLOT + 1;
  }
  m->nbots = nb;
  /* hazard and prop state */
  for (int i = 0; i < ar->nhaz && i < MAX_HAZ; i++) {
    m->haz[i].type = ar->haz[i].type; m->haz[i].seat = 255; m->haz[i].hp = ar->haz[i].type == HZ_BUMPER ? ar->haz[i].p1 : 0; m->haz[i].aux = -100;
    if (ar->haz[i].type == HZ_PIT) m->haz[i].aux = m->clock_s > 60 ? m->clock_s - 60 : 0;   /* first opens at T minus 60 */
  }
  for (int i = 0; i < ar->nprops && i < 32; i++) m->prop_hp[i] = ar->props[i].hp;
  m->ops_state = OPS_PARKED;
}

/* -- the step (4.4) ------------------------------------------------ */
void sim_step(match_t *m) {
  if (m->hitstop > 0) { m->hitstop--; return; }
  /* 2. previous transforms, the clocks */
  for (int i = 0; i < m->nbots; i++) { body_t *b = &m->bots[i].b; b->px = b->x; b->pz = b->z; b->ph = b->h; b->py = b->y; }
  m->frame++;
  if (!m->over) {
    m->clock_s -= DT;
    if (m->clock_s < 0) m->clock_s = 0;
    m->haz_clock += m->closing ? 2 * DT : DT;
    if (!m->closing && m->clock_s <= 15.0f) { m->closing = 1; ev_simple(m, EV_CLOSING, 255, 255, 0, 0, 0, 0); }
  }
  /* 3. timers */
  timers_step(m);
  /* 4. hazard timeline */
  hazard_step(m);
  /* 5. drive */
  for (int i = 0; i < m->nbots; i++) if (!m->bots[i].is_ops || m->ops_driven) drive_step(m, i);
  if (m->nbots > OPS_SLOT && m->bots[OPS_SLOT].is_ops) { ops_step(m); drive_step(m, OPS_SLOT); }
  /* 6. weapons */
  for (int i = 0; i < m->nbots; i++) { if (m->bots[i].hp_max == 0) continue; weapon_machine(m, i, 0); if (m->bots[i].build.weapon2 != WP_NONE) weapon_machine(m, i, 1); }
  /* 7. integrate */
  integrate(m);
  /* 8. walls and solids */
  walls_step(m);
  /* 9. bot vs bot and push */
  bots_collide(m);
  /* 10. clash, hitboxes, holds */
  clash_check(m);
  hit_test(m);
  holds_step(m);
  /* 11. hazards on bots */
  hazards_on_bots(m);
  /* 12. pits, edges, deck, count-out, thresholds, pickups, debris */
  checks_step(m);
  /* 13. the hill and OPS wake and leave ran above; the AI thinks are the caller's */
  hill_step(m);
  /* 14. win conditions */
  win_check(m);
  /* remember the pads for next step's edges; a taunt wiggles the body in render only */
  for (int i = 0; i < m->nbots; i++) m->bots[i].prev_in = m->bots[i].in;
}

void sim_snapshot(const match_t *src, match_t *dst) { memcpy(dst, src, sizeof *dst); }

int sim_over(const match_t *m) { return m->over; }

uint32_t sim_hash(const match_t *m) {
  uint32_t h = 2166136261u;
  #define FNV(v) do { uint32_t bits; memcpy(&bits, &(v), 4); for (int k = 0; k < 4; k++) { h ^= (bits >> (8 * k)) & 255; h *= 16777619u; } } while (0)
  for (int i = 0; i < m->nbots; i++) {
    const bot_t *b = &m->bots[i];
    FNV(b->b.x); FNV(b->b.z); FNV(b->b.h); FNV(b->b.y); FNV(b->hp);
  }
  for (int i = 0; i < m->ndebris; i++) if (m->debris[i].life > 0) { FNV(m->debris[i].x); FNV(m->debris[i].z); }
  #undef FNV
  return h;
}

void sim_setup_line(const match_setup_t *s, int start_frame, char *out, int n) {
  snprintf(out, (size_t)n, "[sim] setup mode=%d arena=%d seed=%u start=%d b0=%d,%d,%d,%d b1=%d,%d,%d,%d b2=%d,%d,%d,%d b3=%d,%d,%d,%d ai=%d,%d,%d,%d players=%d comeback=%d len=%d w2=%d,%d,%d,%d team=%d,%d,%d,%d lvl=%d,%d,%d,%d scheme=%d,%d,%d,%d haz=%d stop=%d ops=%d pads=%d,%d,%d,%d park=%d",
    s->mode, s->arena, s->seed, start_frame,
    s->builds[0].chassis, s->builds[0].drive, s->builds[0].weapon, s->builds[0].armour,
    s->builds[1].chassis, s->builds[1].drive, s->builds[1].weapon, s->builds[1].armour,
    s->builds[2].chassis, s->builds[2].drive, s->builds[2].weapon, s->builds[2].armour,
    s->builds[3].chassis, s->builds[3].drive, s->builds[3].weapon, s->builds[3].armour,
    s->is_ai[0], s->is_ai[1], s->is_ai[2], s->is_ai[3], s->players, s->comeback, s->fight_len_s,
    s->builds[0].weapon2, s->builds[1].weapon2, s->builds[2].weapon2, s->builds[3].weapon2,
    s->team[0], s->team[1], s->team[2], s->team[3], s->ai_level[0], s->ai_level[1], s->ai_level[2], s->ai_level[3],
    s->scheme[0], s->scheme[1], s->scheme[2], s->scheme[3], s->hazards_off, s->hitstop_off, s->no_ops, s->pad[0], s->pad[1], s->pad[2], s->pad[3], s->park);
}

/* the hazard seat (2.10) */
void sim_seat_pick(match_t *m, int seat, int dir) {
  if (seat < 0 || seat > 3) return;
  const arena_def_t *ar = sim_arena(m);
  int n = arena_seat_hazard_count(ar);
  if (n <= 0) return;
  int k = 0;
  if (m->hazard_seat[seat] != 255) { for (int i = 0; i < n; i++) if (arena_seat_hazard(ar, i) == m->hazard_seat[seat]) k = i; k = (k + dir + n) % n; }
  int hi = arena_seat_hazard(ar, k);
  for (int i = 0; i < MAX_HAZ; i++) if (m->haz[i].seat == seat) m->haz[i].seat = 255;
  m->hazard_seat[seat] = (uint8_t)hi;
  if (hi >= 0) m->haz[hi].seat = (uint8_t)seat;
}

void sim_seat_fire(match_t *m, int seat) {
  if (seat < 0 || seat > 3 || m->hazard_seat[seat] == 255) return;
  haz_fire(m, m->hazard_seat[seat], seat);
}

void sim_ops_driven(match_t *m, int driver_seat, int gunner_seat) {
  if (m->nbots <= OPS_SLOT || !m->bots[OPS_SLOT].is_ops) return;
  m->ops_driven = 1; m->ops_driver = (uint8_t)driver_seat; m->ops_gunner = (uint8_t)gunner_seat;
  m->bots[OPS_SLOT].hp = m->bots[OPS_SLOT].hp_max = 400;
}

int sim_countout_number(const bot_t *b) {
  if (b->countout <= 0 || !b->alive) return -1;
  int n = 10 - b->countout / 60;
  return n < 0 ? 0 : n;
}

int sim_status_word(const bot_t *b) {
  if (b->clamped_by) return SW_CLAMPED;
  if (b->lifted_by) return SW_LIFTED;
  if (b->red_tag_t > 0) return SW_RED_TAG;
  if (b->burn_t > 0) return SW_BURNING;
  if (b->inverted) return SW_INVERTED;
  if (b->stun > 0) return SW_STUNNED;
  int w = b->build.weapon;
  if (b->build.drive == DR_MELTY || w == WP_HORIZ || w == WP_DRUM || w == WP_SAW) {
    if (b->spin >= 0.95f) return SW_FULL_SPIN;
    if (b->spin > 0.05f) return SW_SPINNING_UP;
  }
  if (w == WP_FLAME && b->fuel <= 0) return SW_EMPTY;
  return SW_NONE;
}
