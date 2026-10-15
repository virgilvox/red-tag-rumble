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
/* a ram is an event, not a drumbeat: one per victim per RAM_CD frames (the spec's 30 let a wedge
 * bounce a bot ten times in ten seconds); RAM_BASE plus RAM_BASE by closing speed (the spec's 10 + 10).
 * RAM_CD and RAM_MIN_CLOSING (the closing speed floor; the spec's 2.0 let a TREADS wedge push a backing
 * bot all day without a single ram) are in sim.h (DEEP 2.4) */
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
/* spinner energy (deep-physics 3.6): the share of the stored energy a hit takes, 0.35 plus 0.08 per u/s of
 * tooth speed plus closing speed, between 0.35 and 0.80; the knock a full bite gives (HORIZ 6, DRUM and
 * MELTY 4); a disc glancing off a wedge hops up the slope (1.5 u/s is 9 frames in the air) */
#ifndef BITE_BASE
#define BITE_BASE 0.35f
#endif
#ifndef BITE_SLOPE
#define BITE_SLOPE 0.08f
#endif
#define BITE_MIN 0.35f
#ifndef BITE_MAX
#define BITE_MAX 0.80f
#endif
#ifndef KNOCK_HORIZ
#define KNOCK_HORIZ 6.0f
#endif
#define KNOCK_SPIN(w, melty) ((melty) ? 4.0f : (w) == WP_HORIZ ? KNOCK_HORIZ : 4.0f)
/* the energy a hit must take to land the table's base damage: dmg_raw = base x E_hit / SPIN_E_REF. The plan's
 * 0.5 made a medium bite the base, but the tooth speed alone puts a full spin bite at 0.64 and most hits at
 * the 0.80 ceiling, 1.6x the table; at 0.8 a full bite is the table's base and hit sizes stay as R4 tuned them */
#ifndef SPIN_E_REF
#define SPIN_E_REF 0.8f
#endif
#define DEFLECT_HOP_VY 1.5f
/* no count-out for stillness in the first 5 s of a fight (an inverted bot still counts) */
#define COUNTOUT_GRACE_FRAMES 300
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
 * next needs a visible spin up). The spun down zone of 4.7 still opens under 0.3. SPINNER_HIT_MIN is
 * in sim.h (DEEP 2.4) so the HUD reads the same number */

/* the drive and contact model (DEEP-plan 6 steps 3 to 5): a wheel's grip is mu g load, the floor's lateral
 * hold is that times LAT_GRIP; contacts are impulses with restitution and friction at the contact point */
#define E_BOTS      0.2f
#define E_WALLS     0.3f
#define E_RIDE      0.1f
#define MU_CONTACT  0.4f
#define MU_RIDE     0.15f
#define SLOP        0.01f
#define TIP_G       9.0f        /* rad/s2: gravity's pull on a tipping body about its edge */
#define RELAX_RATE  10.0f       /* rad/s: a supported body settles onto the floor's slope */
#define SLIDE_SPEED 2.0f        /* u/s of lateral slide that is an EV_SLIDE */
#define SCRAPE_SPEED 1.5f       /* u/s along a wall or on a lid that is an EV_SCRAPE after 6 frames */

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
  event_t e = { (uint8_t)type, (uint8_t)a, (uint8_t)b, (uint8_t)zone, x, z, y, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
  sim_event(m, &e);
}

static void ev_hit(match_t *m, int type, int a, int b, float zone, float x, float z, float y, float dmg, float nx, float nz) {
  event_t e = { (uint8_t)type, (uint8_t)a, (uint8_t)b, (uint8_t)(zone * 10.0f + 0.5f), x, z, y, dmg, nx, nz, 0, 0, 0, 0, 0, 0 };
  sim_event(m, &e);
}

/* every field of DEEP-plan 2.5: zone is the raw byte (a hit's zone times ten is the caller's) */
static void ev_full(match_t *m, int type, int a, int b, int zone, float x, float z, float y, float dmg, float nx, float nz,
                    int panel, int weapon, int flags, int reason, float speed, float j) {
  event_t e = { (uint8_t)type, (uint8_t)a, (uint8_t)b, (uint8_t)zone, x, z, y, dmg, nx, nz,
                (uint8_t)panel, (uint8_t)weapon, (uint8_t)flags, (uint8_t)reason, speed, j };
  sim_event(m, &e);
}

/* a counted out or flipped out bot left on the floor: a prop that collides and can be hit (dmg 0) */
static inline int is_corpse(const bot_t *v) { return !v->alive && v->hp_max > 0 && !v->is_ops && !v->falling && v->pit_state != 2 && v->ko_how != KO_PIT && v->ko_how != KO_ROOF; }
/* the inverse mass and yaw inertia a contact sees: a dead chassis is a 0.5 mass prop, OPS is immovable */
static inline float body_im(const bot_t *b) { return b->is_ops ? 0 : b->alive ? b->b.inv_mass : 2.0f; }
static inline float body_ii(const bot_t *b) {
  if (b->is_ops) return 0;
  if (b->alive) return b->b.inv_inertia;
  float r2 = b->b.shape == SH_CIRCLE ? b->b.hl * b->b.hl * 1.5f : b->b.hl * b->b.hl + b->b.hw * b->b.hw;
  return r2 > 0 ? 2.0f * 3.0f / r2 : 0;
}
static inline int on_floor(const bot_t *b) { return !b->airborne && !b->lifted_by && !b->clamped_by; }
/* do two bodies share a height band (DEEP-plan 3, ghost cause 3): two bots on the floor always do unless
 * exactly one is up on the deck; otherwise (airborne, lifted, clamped, one on the deck) the vertical
 * intervals [y + clr, y + clr + hgt] must overlap by 0.05 */
static int same_band(const bot_t *a, const bot_t *b) {
  if (on_floor(a) && on_floor(b) && a->on_deck == b->on_deck) return 1;
  float a0 = a->b.y + a->b.clr, a1 = a0 + a->b.hgt, b0 = b->b.y + b->b.clr, b1 = b0 + b->b.hgt;
  float lo = a0 > b0 ? a0 : b0, hi = a1 < b1 ? a1 : b1;
  return hi - lo >= 0.05f;
}

static void ride_end(match_t *m, int v, int reason);    /* the wedge, below */
static void ride_clear(match_t *m, int v);
static int bot_kind(const bot_t *b) { return b->is_ops ? LIP_NONE : bot_wedge_kind(&b->build, b->lift); }

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
  /* the contact point: midway between the two faces along the normal, and at the middle of the boxes'
   * shared span along the face (a face to face contact is at its middle, not at a corner, so the impulse's
   * torque is right) */
  float tx = -c->nz, tz = c->nx;
  float ca = dotp(a->x, a->z, tx, tz), cb = dotp(b->x, b->z, tx, tz), ea = extent_along(a, tx, tz), eb = extent_along(b, tx, tz);
  float lo = fmax_f(ca - ea, cb - eb), hi = fmin_f(ca + ea, cb + eb), mt = (lo + hi) * 0.5f;
  float sa = dotp(a->x, a->z, c->nx, c->nz) + extent_along(a, c->nx, c->nz), sb = dotp(b->x, b->z, c->nx, c->nz) - extent_along(b, c->nx, c->nz);
  float mn = (sa + sb) * 0.5f;
  c->cx = c->nx * mn + tx * mt; c->cz = c->nz * mn + tz * mt;
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

/* the edge of a body a world point sits on (PN_FRONT, PN_BACK, PN_LEFT or PN_RIGHT); local +x is PN_RIGHT */
static int edge_of(const bot_t *v, float cx, float cz) {
  float fx, fz, rx, rz; fwd_of(v->b.h, &fx, &fz); right_of(v->b.h, &rx, &rz);
  float dx = cx - v->b.x, dz = cz - v->b.z, lz = dotp(dx, dz, fx, fz), lx = dotp(dx, dz, rx, rz);
  float hl = v->b.hl > 0 ? v->b.hl : 0.5f, hw = v->b.shape == SH_CIRCLE ? hl : (v->b.hw > 0 ? v->b.hw : 0.5f);
  if (fabs_f(lz) / hl >= fabs_f(lx) / hw) return lz >= 0 ? PN_FRONT : PN_BACK;
  return lx >= 0 ? PN_RIGHT : PN_LEFT;
}
/* the edge of a body whose outward normal is nearest a world direction (a contact normal is a face normal,
 * so this names the face the contact is on even when the contact point is a corner) */
static int edge_toward(const bot_t *v, float dx, float dz) {
  float fx, fz, rx, rz; fwd_of(v->b.h, &fx, &fz); right_of(v->b.h, &rx, &rz);
  float lz = dotp(dx, dz, fx, fz), lx = dotp(dx, dz, rx, rz);
  if (v->b.shape == SH_BOX) { lz *= v->b.hw; lx *= v->b.hl; }     /* a box face's share of the direction */
  if (fabs_f(lz) >= fabs_f(lx)) return lz >= 0 ? PN_FRONT : PN_BACK;
  return lx >= 0 ? PN_RIGHT : PN_LEFT;
}
/* the outward unit normal of an edge in the world */
static void edge_normal(const bot_t *v, int edge, float *nx, float *nz) {
  float fx, fz, rx, rz; fwd_of(v->b.h, &fx, &fz); right_of(v->b.h, &rx, &rz);
  switch (edge) {
    case PN_FRONT: *nx = fx; *nz = fz; break;
    case PN_BACK: *nx = -fx; *nz = -fz; break;
    case PN_RIGHT: *nx = rx; *nz = rz; break;
    default: *nx = -rx; *nz = -rz; break;
  }
}
/* a body's half extent along a unit axis (the circle's radius) */
static float body_extent(const bot_t *b, float ax, float az) { shape_t s; shape_of(b, &s); return extent_along(&s, ax, az); }
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

/* a panel leaves the bot as sim debris (4.9): it flies off its own side (the scatter is the debris draw) */
static void shed_panel(match_t *m, int slot, int panel) {
  bot_t *b = &m->bots[slot];
  int old = b->panel_state[panel];
  b->panel_state[panel] = 4;
  if (panel < PN_COUNT) b->panel_hp[panel] = 0;
  float a = rnd01(m) * 6.2831853f, dx = rtr_sin(a), dz = rtr_cos(a);
  if (panel <= PN_RIGHT) { float ex, ez; edge_normal(b, panel, &ex, &ez); dx = ex * 0.8f + dx * 0.4f; dz = ez * 0.8f + dz * 0.4f; float l = len2(dx, dz); if (l > 1e-4f) { dx /= l; dz /= l; } }
  float vx = b->b.vx + dx * 2.0f, vz = b->b.vz + dz * 2.0f;
  int pickup = m->comeback ? 1 : 0;
  debris_spawn(m, b->b.x + dx * b->b.hl * 0.6f, b->b.z + dz * b->b.hl * 0.6f, b->floor_y + 0.3f, vx, vz, 3.0f, DB_PANEL, 0, pickup, slot, 360, panel);   /* colour 0: the body paint */
  ev_full(m, EV_SHED, slot, slot, panel, b->b.x, b->b.z, b->b.y + b->b.clr + b->b.hgt * 0.5f, (float)old, dx, dz, panel, 0, 0, 0, 0, 0);
}

/* the shed at 20 HP (and from 50 on FRAME and bare builds) takes the most worn panel; a tie is the debris draw */
static void shed_worst_panel(match_t *m, int slot) {
  bot_t *b = &m->bots[slot];
  int cands[6], n = 0; float worst = 1e9f; int wstate = -1;
  for (int p = 0; p < PN_COUNT; p++) {
    if (b->panel_state[p] >= 4) continue;
    float f = b->panel_hp[p];
    if (b->panel_state[p] > wstate || (b->panel_state[p] == wstate && f < worst - 1e-4f)) { wstate = b->panel_state[p]; worst = f; n = 0; }
    if (b->panel_state[p] == wstate && fabs_f(f - worst) <= 1e-4f) cands[n++] = p;
  }
  if (!n) return;
  shed_panel(m, slot, cands[n == 1 ? 0 : rnd_n(m, n)]);
}

/* at 0 HP all remaining panels and the wheels detach, up to 8 debris (4.9) */
static void burst(match_t *m, int slot) {
  bot_t *b = &m->bots[slot];
  int n = 0;
  for (int p = 0; p < PN_COUNT && n < 8; p++) if (b->panel_state[p] < 4) { shed_panel(m, slot, p); n++; }
  int wheels = b->build.drive == DR_TWO ? 2 : b->build.drive == DR_FOUR ? 4 : b->build.drive == DR_SIX ? 6 : b->build.drive == DR_TREAD || b->build.drive == DR_LEGTRACK ? 2 : 0;
  for (int w = 0; w < wheels && n < 8; w++, n++) {
    float a = rnd01(m) * 6.2831853f;
    debris_spawn(m, b->b.x, b->b.z, b->floor_y + 0.2f, b->b.vx + rtr_sin(a) * 2.5f, b->b.vz + rtr_cos(a) * 2.5f, 2.0f, DB_WHEEL, 2, 0, slot, 240, 0);   /* colour 2: rubber */
  }
  ev_simple(m, EV_BURST, slot, slot, 0, b->b.x, b->b.z, b->b.y);
}

/* the dent state of a pool (3.10): 0 over 70 percent, 1 at 70, 2 at 45, 3 at 20, 4 (gone) at 0 */
static int pool_state(float hp, float max) {
  if (max <= 0) return 0;
  float f = hp / max;
  return f <= 0 ? 4 : f <= 0.20f ? 3 : f <= 0.45f ? 2 : f <= 0.70f ? 1 : 0;
}
/* the panel's centre in the world, for the dent sparks */
static void panel_centre(const bot_t *b, int panel, float *x, float *z, float *y) {
  float ex = 0, ez = 0, ext = 0;
  if (panel <= PN_RIGHT) { edge_normal(b, panel, &ex, &ez); ext = panel <= PN_BACK ? b->b.hl : (b->b.shape == SH_CIRCLE ? b->b.hl : b->b.hw); }
  *x = b->b.x + ex * ext; *z = b->b.z + ez * ext;
  *y = b->b.y + b->b.clr + (panel == PN_TOP || panel == PN_MOUNT ? b->b.hgt : b->b.hgt * 0.5f);
}
/* wear a panel's pool; a state advance is an EV_PANEL_WEAR, the pool reaching 0 sheds it */
static void wear_panel(match_t *m, int att, int slot, int panel, float amount) {
  bot_t *b = &m->bots[slot];
  float max = ARMOUR_PANEL_HP[b->build.armour];
  if (b->is_ops || max <= 0 || panel < 0 || panel >= PN_COUNT || b->panel_state[panel] >= 4 || amount <= 0) return;
  int before = b->panel_state[panel];
  b->panel_hp[panel] -= amount;
  if (b->panel_hp[panel] < 0) b->panel_hp[panel] = 0;
  int st = pool_state(b->panel_hp[panel], max);
  if (st >= 4) { shed_panel(m, slot, panel); return; }
  if (st > before) {
    b->panel_state[panel] = (uint8_t)st;
    float x, z, y; panel_centre(b, panel, &x, &z, &y);
    ev_full(m, EV_PANEL_WEAR, att < 0 ? 255 : att, slot, st, x, z, y, b->panel_hp[panel], 0, 0, panel, 0, 0, 0, 0, 0);
  }
}

/* panel floors, the shed, the lost wheel and track, RED TAG (4.9, 3.10); called after damage */
static void hp_thresholds(match_t *m, int slot) {
  bot_t *b = &m->bots[slot];
  if (b->is_ops || !b->alive) return;
  /* the global floor: at 50 HP every panel is dented at least once, at 30 twice (smoke and fire follow HP) */
  int st = b->hp <= 30 ? 2 : b->hp <= 50 ? 1 : 0;
  for (int p = 0; p < PN_COUNT; p++) if (b->panel_state[p] < 4 && b->panel_state[p] < st) b->panel_state[p] = (uint8_t)st;
  while (b->shed_next > 0 && b->hp <= (float)b->shed_next && b->hp > 0) {
    shed_worst_panel(m, slot);
    b->shed_next = (uint8_t)(b->build.armour == AR_NONE && b->shed_next > 10 ? b->shed_next - 10 : 0);
  }
  /* FOUR WHEEL throws its rear wheel at 30 HP (0.8x top from bot_top_speed); TREADS throw a track at 20 HP
   * once a side is stripped */
  if (b->build.drive == DR_FOUR && b->hp <= 30 && b->hp > 0 && !b->wheel_lost) {
    b->wheel_lost = 1;
    float fx, fz, rx, rz; fwd_of(b->b.h, &fx, &fz); right_of(b->b.h, &rx, &rz);
    float wx = b->b.x + rx * b->b.hw - fx * b->b.hl * 0.6f, wz = b->b.z + rz * b->b.hw - fz * b->b.hl * 0.6f;
    debris_spawn(m, wx, wz, b->floor_y + 0.2f, b->b.vx + rx * 2.0f, b->b.vz + rz * 2.0f, 2.5f, DB_WHEEL, 2, 0, slot, 240, 0);
    ev_full(m, EV_SHED, slot, slot, 8 + 3, wx, wz, b->floor_y + 0.17f, 0, rx, rz, PN_RIGHT, 0, 0, 0, 0, 0);
  }
  if (b->build.drive == DR_TREAD && b->hp <= 20 && b->hp > 0 && !b->track_lost && (b->panel_state[PN_LEFT] >= 4 || b->panel_state[PN_RIGHT] >= 4)) {
    b->track_lost = 1;
    int side = b->panel_state[PN_LEFT] >= 4 ? 0 : 1;
    float ex, ez; edge_normal(b, side ? PN_RIGHT : PN_LEFT, &ex, &ez);
    float wx = b->b.x + ex * b->b.hw, wz = b->b.z + ez * b->b.hw;
    debris_spawn(m, wx, wz, b->floor_y + 0.15f, b->b.vx + ex * 1.5f, b->b.vz + ez * 1.5f, 2.0f, DB_TREAD, 2, 0, slot, 300, 0);
    ev_full(m, EV_SHED, slot, slot, 8 + side, wx, wz, b->floor_y + 0.15f, 0, ex, ez, side ? PN_RIGHT : PN_LEFT, 0, 0, 0, 0, 0);
  }
  if (b->hp <= 30 && !b->red_tag_used && m->comeback && b->hp > 0) {
    b->red_tag_t = 480; b->red_tag_used = 1;
    ev_simple(m, EV_RED_TAG, slot, slot, 0, b->b.x, b->b.z, b->b.y);
  }
}


/* one landed contact, everything DEEP-plan 2.5 puts on the event (see apply_hit) */
typedef struct {
  int att, vic, weapon;        /* att -1 for a hazard; weapon WP_* or EVW_* */
  float raw;                   /* base x energy x closing x zone x loc, before the armour */
  int stun;
  float cx, cz, cy, nx, nz;    /* the contact, its height, the normal from the attacker into the victim */
  int ev_type;                 /* EV_HIT, EV_RAM, EV_HAZ_HIT, or -1 for no event */
  float zone;                  /* the hit zone (the event carries it times ten) */
  int panel, flags, reason;    /* PN_*, EVF_*, HIT_WHY_* (apply_hit adds STRIPPED and ARMOUR) */
  float speed, j;              /* closing speed, knock impulse */
  int wear;                    /* 1: the damage after armour wears the panel's pool too */
} hit_t;

/* the one damage path (4.7, 3.10, 3.12): the panel's armour (none where it is gone, and 1.25x the exposed
 * internals), RED TAG, teammates, HIT_CAP, tallies, stun, the pool, the event, the hit stop. Returns the
 * damage applied */
static float apply_hit(match_t *m, hit_t *h) {
  bot_t *a = h->att >= 0 ? &m->bots[h->att] : 0, *v = &m->bots[h->vic];
  if (!v->alive) return 0;
  int panel = h->panel >= 0 && h->panel < PN_COUNT ? h->panel : PN_TOP;
  float red = ARMOURS[v->build.armour].reduction, raw = h->raw;
  if (v->panel_state[panel] >= 4 && !v->is_ops) { red = 0; raw *= 1.25f; h->flags |= EVF_STRIPPED; }
  float dmg = raw * (1.0f - red);
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
  int weapon = h->weapon;
  if (a) {
    a->dealt += dmg;
    if (dmg > a->biggest) a->biggest = dmg;
    if (weapon == WP_HORIZ || weapon == WP_DRUM || weapon == WP_SAW || weapon == EVW_MELTY) a->spinner_dealt += dmg;
    /* a landed hit resets the count (2.7), unless the attacker is inverted with no way to drive: a spinner
     * that still bites while it is shoved around is not moving, and its count used to restart on every
     * graze so a flipped TREADS and HORIZONTAL build lay there for 17 s instead of 10 */
    if (!sim_inv_stuck(a)) a->countout = 0;
    v->last_hit_by = (uint8_t)(h->att + 1); v->last_hit_t = 90;
    if (v->is_ops) { v->ops_dmg += dmg; v->ops_dmg_t = 300; }
    if (h->ev_type == EV_HIT || h->ev_type == EV_RAM) a->hit_flash_t = 20;
  } else if (v->last_hit_by && v->last_hit_t > 0 && v->last_hit_by - 1 < m->nbots) {
    /* a hazard the victim was pushed into within the last 90 frames counts as control (2.5) */
    m->bots[v->last_hit_by - 1].control += 20;
  }
  if (h->stun > v->stun) v->stun = (int16_t)h->stun;
  /* a spinner hit on WEAR PLATE stuns the attacker 9 extra frames (3.6) */
  if (a && v->build.armour == AR_WEAR && (weapon == WP_HORIZ || weapon == WP_DRUM || weapon == WP_SAW) && a->stun < 9) a->stun = 9;
  m->no_hit_frames = 0;
  int reason = h->reason;
  if (reason != HIT_WHY_EXPOSED && (h->flags & EVF_STRIPPED)) reason = HIT_WHY_STRIPPED;
  if (!reason && red >= 0.35f && dmg < raw * 0.5f) reason = HIT_WHY_ARMOUR;
  h->reason = reason;
  if (h->ev_type >= 0) {
    ev_full(m, h->ev_type, h->att < 0 ? 255 : h->att, h->vic, (int)(h->zone * 10.0f + 0.5f), h->cx, h->cz, h->cy > 0 ? h->cy : v->b.y + v->b.clr + v->b.hgt * 0.5f,
            dmg, h->nx, h->nz, panel, weapon < 0 ? EVW_NONE : weapon, h->flags, reason, h->speed, h->j);
    if (a && (h->ev_type == EV_HIT || h->ev_type == EV_RAM)) a->last_hit_reason = (uint8_t)reason;   /* why my last hit landed as it did */
  }
  /* hit stop (5.1) from the band; the sim applies it so a replay stays aligned */
  if (!m->hitstop_off && (h->ev_type == EV_HIT || h->ev_type == EV_RAM)) {
    int stop = dmg < BAND_HIT_MIN ? STOP_TAP : dmg < BAND_HEAVY_MIN ? STOP_HIT : dmg < BAND_HUGE_MIN ? STOP_HEAVY : STOP_HUGE;
    if (stop > m->hitstop) m->hitstop = (int16_t)stop;
  }
  if (h->wear) wear_panel(m, h->att, h->vic, panel, dmg);
  hp_thresholds(m, h->vic);
  return dmg;
}

/* the panel a contact lands on: its edge, or the TOP for a blow from above */
static int panel_at(const bot_t *v, float cx, float cz) { return edge_of(v, cx, cz); }

/* the old entry point, kept for the paths that are not weapon contacts (holds, OPS, hazards). weapon -2 is
 * MELTY, -3 a blow from above (the TOP panel), -1 none */
static float apply_damage(match_t *m, int att, int vic, float raw, int weapon, int stun, float cx, float cz, float nx, float nz, int ev_type, float zone) {
  bot_t *v = &m->bots[vic];
  hit_t h = { 0 };
  h.att = att; h.vic = vic; h.raw = raw; h.stun = stun; h.cx = cx; h.cz = cz; h.nx = nx; h.nz = nz; h.ev_type = ev_type; h.zone = zone; h.wear = 1;
  h.weapon = att < 0 ? EVW_HAZARD : weapon == -2 ? EVW_MELTY : weapon == -3 ? EVW_LAND : weapon < 0 ? EVW_NONE : weapon;
  if (att >= 0 && m->bots[att].is_ops) h.weapon = EVW_OPS;
  h.panel = weapon == -3 ? PN_TOP : panel_at(v, cx, cz);
  if (h.panel == PN_TOP) h.flags |= EVF_TOP;
  return apply_hit(m, &h);
}


/* knockback (4.7): an impulse through a point, so a corner hit spins a body as well as shoving it. A HOVER
 * has no grip to take it on (1.5x), the drive's knock_mult and UHMW's half apply, a corpse is a plain prop */
static float knock_mult(const bot_t *v) {
  if (!v->alive) return 1.0f;
  float mult = DRIVES[v->build.drive].knock_mult * (ARMOURS[v->build.armour].knock_half ? 0.5f : 1.0f);
  if (v->build.drive == DR_HOVER) mult *= 1.5f;
  return mult;
}
static void knock(bot_t *v, float jx, float jz) {
  if (!(v->alive || is_corpse(v)) || v->is_ops || v->lifted_by || v->clamped_by) return;
  float k = body_im(v) * knock_mult(v);
  v->b.vx += jx * k; v->b.vz += jz * k;
}
static void knock_at(bot_t *v, float jx, float jz, float cx, float cz) {
  if (!(v->alive || is_corpse(v)) || v->is_ops || v->lifted_by || v->clamped_by) return;
  float mult = knock_mult(v), rx = cx - v->b.x, rz = cz - v->b.z;
  v->b.vx += jx * body_im(v) * mult; v->b.vz += jz * body_im(v) * mult;
  v->b.w += (rz * jx - rx * jz) * body_ii(v) * mult;
}

/* a contact on a wheel, track or leg station at a side (deep-physics 3.9; stations where the renderer puts
 * them: TWO WHEEL a fifth back, FOUR WHEEL at 0.3 L fore and aft, SIX WHEEL and WALKER at the middle and
 * the ends, TREADS and LEG TRACKS the whole side) */
static int wheel_station(const bot_t *v, float cx, float cz, int edge) {
  if (edge != PN_LEFT && edge != PN_RIGHT) return 0;
  int d = v->build.drive;
  if (d == DR_TREAD || d == DR_LEGTRACK) return 1;
  float fx, fz; fwd_of(v->b.h, &fx, &fz);
  float lz = dotp(cx - v->b.x, cz - v->b.z, fx, fz), L = CHASSIS[v->build.chassis].len;
  switch (d) {
    case DR_TWO: return fabs_f(lz + 0.2f * L) < 0.18f;
    case DR_FOUR: return fabs_f(lz - 0.3f * L) < 0.18f || fabs_f(lz + 0.3f * L) < 0.18f;
    case DR_SIX: return fabs_f(lz) < 0.18f || fabs_f(lz - 0.34f * L) < 0.18f || fabs_f(lz + 0.34f * L) < 0.18f;
    case DR_WALKER: return fabs_f(lz) < 0.18f || fabs_f(lz - 0.36f * L) < 0.18f || fabs_f(lz + 0.36f * L) < 0.18f;
    default: return 0;
  }
}

/* the hit zone of a contact (deep-physics 3.12) and the facts the event carries: 1.0; a box's front plate
 * 0.6, and a wedge front (the WEDGE chassis or plate, upright) 0.4 with a deflection against a spinner, saw
 * or spike; 1.5 exposed (lifted, up off the floor, righting, ridden over 0.3, upside down under a hammer,
 * drum or spike); 2.0 a spun down disc (and the 2ND weapon of the wedge riding it, in weapon_hit); a SHELL 0.8 */
static float contact_zone(const bot_t *v, int weapon, float cx, float cz, int *flags, int *reason) {
  float fx, fz, rx, rz; fwd_of(v->b.h, &fx, &fz); right_of(v->b.h, &rx, &rz);
  float dx = cx - v->b.x, dz = cz - v->b.z, lz = dotp(dx, dz, fx, fz), lx = dotp(dx, dz, rx, rz);
  float zone = 1.0f;
  int cutting = weapon == WP_HORIZ || weapon == WP_DRUM || weapon == WP_SAW || weapon == WP_SPIKE || weapon == EVW_MELTY;
  int wedge = !v->inverted && (v->build.chassis == CH_WEDGE || v->build.weapon == WP_WEDGE);
  if (v->b.shape == SH_BOX && lz > 0.6f * v->b.hl && fabs_f(lx) < v->b.hw) {
    if (wedge && cutting) { zone = 0.4f; *flags |= EVF_DEFLECT; *reason = HIT_WHY_WEDGE; }
    else { zone = 0.6f; *reason = HIT_WHY_FRONT; }
  }
  int exposed = v->lifted_by || (v->airborne && v->b.y - v->floor_y > 0.15f) || v->selfright > 0 || (v->ride_by && v->ride_up > 0.3f) ||
                (v->inverted && (weapon == WP_HAMMER || weapon == WP_DRUM || weapon == WP_SPIKE));
  if (exposed) { zone = 1.5f; *flags &= ~EVF_DEFLECT; *reason = HIT_WHY_EXPOSED; }
  /* a spun down spinner's weapon circle takes 2.0x */
  if ((v->build.weapon == WP_HORIZ || v->build.weapon == WP_DRUM) && v->spin < 0.3f) {
    const weapon_def_t *w = &WEAPONS[v->build.weapon];
    float wx = v->b.x + fx * w->hb_z0, wz = v->b.z + fz * w->hb_z0;
    if (len2(cx - wx, cz - wz) < w->hb_r + 0.1f) { zone = 2.0f; *flags &= ~EVF_DEFLECT; *reason = HIT_WHY_EXPOSED; }
  }
  if (v->build.chassis == CH_SHELL) { zone *= 0.8f; if (!*reason) *reason = HIT_WHY_SHELL; }
  return zone;
}

/* the hammer arc (deep-physics 3.7): the arm is pinned HAMMER_PIVOT_UP over the attacker's top and
 * HAMMER_PIVOT_Z behind its centre; it is raised HAMMER_A0 degrees over the forward horizontal at the end of
 * the windup and comes down to the floor HAMMER_REACH in front of the pivot at the end of the swing, so a
 * tall target is met early in the swing and a low one late. p is the share of the swing done (0..1); the
 * head centre comes back in the attacker's frame as (z forward, y up) */
static void hammer_head(const bot_t *a, float p, float *hz, float *hy, float *py, float *pz) {
  float s = a->is_ops ? 1.4f : 1.0f;
  float y0 = a->b.y + a->b.clr + a->b.hgt + HAMMER_PIVOT_UP, z0 = HAMMER_PIVOT_Z * s;
  float ze = z0 + HAMMER_REACH * s, ye = a->floor_y + HAMMER_HEAD_R * 0.6f;
  float R = len2(ze - z0, ye - y0);
  float a0 = HAMMER_A0 * (PI_F / 180.0f), a1 = rtr_atan2(ye - y0, ze - z0);
  float th = a0 + (a1 - a0) * p;
  *hz = z0 + R * rtr_cos(th); *hy = y0 + R * rtr_sin(th);
  if (py) *py = y0; if (pz) *pz = z0;
}
/* the spike's tip (3.7): out over frames 1 to 5 of active (z 0.55 to 1.05), back over 6 to 9 */
static float spike_tip(int active) {
  int f = WEAPONS[WP_SPIKE].active + 1 - active;           /* 1 at the fire frame */
  if (f <= 5) return 0.55f + 0.5f * (float)f / 5.0f;
  return 1.05f - 0.5f * (float)(f - 5) / 4.0f;
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
    case WP_LIFTER: on = b->is_ops ? lift > 0.3f : b->fork_armed && b->carry_t == 0 && lift < 0.3f; break;   /* the forks are live while they are down and armed */
    case WP_CRUSHER: on = b->clamp == 1; break;     /* the closing frame */
    default: on = 0;
  }
  if (!on) return 0;
  float fx, fz; fwd_of(b->b.h, &fx, &fz);
  float scale = b->is_ops ? 1.4f : 1.0f;
  if (wid == WP_HAMMER) {
    /* the head's place on the floor plan this frame; hit_test checks its height against the victim */
    float p = (float)(HAMMER_ACTIVE + 1 - active) / (float)HAMMER_ACTIVE, hz, hy;
    hammer_head(b, rtr_clamp(p, 0, 1), &hz, &hy, 0, 0);
    s->x = b->b.x + fx * hz; s->z = b->b.z + fz * hz; s->h = b->b.h; s->hl = s->hw = HAMMER_HEAD_R; s->circle = 1;
    return 1;
  }
  if (wid == WP_SPIKE) {
    float z0 = w->hb_z0 * scale, z1 = spike_tip(active) * scale;
    float zm = (z0 + z1) * 0.5f;
    s->x = b->b.x + fx * zm; s->z = b->b.z + fz * zm; s->h = b->b.h; s->hl = (z1 - z0) * 0.5f; s->hw = w->hb_w * 0.5f; s->circle = 0;
    return 1;
  }
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

/* the wheel, track or foot model (DEEP-plan 6 step 4, deep-physics 3.3). Forward: the motor drives toward
 * t x top at accel, a coasting or idle bot's motors brake it, and neither can ask the floor for more than its
 * grip mu g load. Lateral: the tyres take out the sideways speed in proportion to it (the old bleed rate) up
 * to the floor's lateral hold mu g LAT_GRIP load, so a gentle turn rolls and a knock slides: TREADS stop a
 * 6 u/s shove in about 0.6 u, a TWO WHEEL skids twice that, and a slide over 2 u/s is an EV_SLIDE */
static void wheel_step(match_t *m, int slot, int driving, float t, float top, float accel) {
  bot_t *b = &m->bots[slot];
  const drive_def_t *d = &DRIVES[b->build.drive];
  const arena_def_t *ar = sim_arena(m);
  float load = b->load < 0 ? 0 : b->load;
  float mu = d->traction * (b->build.drive == DR_TREAD ? 1.0f : ar->traction_mult);
  float grip = mu * GRAVITY * load * DT;                            /* the most the floor gives this frame, u/s */
  float fx, fz, rx, rz; fwd_of(b->b.h, &fx, &fz); right_of(b->b.h, &rx, &rz);
  float vf = dotp(b->b.vx, b->b.vz, fx, fz), vl = dotp(b->b.vx, b->b.vz, rx, rz);
  float dv;
  if (!driving) dv = -vf * 0.10f;                                   /* motors braked: an idle bot rolls to a stop */
  else if (fabs_f(t) < 0.05f) dv = -vf * 0.19f;                     /* coasting: the old 0.90 twice a frame */
  else dv = rtr_clamp(t * top - vf, -accel * DT, accel * DT);
  dv = rtr_clamp(dv, -grip, grip);
  float hold = grip * LAT_GRIP[b->build.drive] * (b->tread_hit > 0 ? 0.7f : 1.0f);
  float dl = rtr_clamp(vl * (4.0f + 8.0f * d->traction) * DT, -hold, hold);
  float nvf = vf + dv, nvl = vl - dl;
  b->b.vx = fx * nvf + rx * nvl; b->b.vz = fz * nvf + rz * nvl;
  b->wheel_fric = 1;
  float sl = fabs_f(nvl);
  if (sl > 1.0f) { if (b->slide_t < 32000) b->slide_t++; } else b->slide_t = 0;
  if (sl > SLIDE_SPEED && b->slide_cd == 0 && b->alive) {
    float s = nvl > 0 ? 1.0f : -1.0f;
    ev_full(m, EV_SLIDE, slot, slot, 0, b->b.x, b->b.z, b->b.y, sl, rx * s, rz * s, 0, b->build.weapon, 0, 0, sl, 0);
    b->slide_cd = 30;
  }
}

static void drive_step(match_t *m, int slot) {
  bot_t *b = &m->bots[slot];
  const drive_def_t *d = &DRIVES[b->build.drive];
  const arena_def_t *ar = sim_arena(m);
  b->drive_t = 0; b->drive_s = 0; b->wheel_fric = 0;
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
  /* wheels, tracks or feet on the floor: the tyre model below runs whether or not the bot can drive, so a
   * stunned or braked bot that is shoved still slides on its grip (DEEP-plan 6 step 4) */
  int wheels = !d->strafe && !b->airborne && !b->lifted_by && !b->clamped_by && !b->falling && b->pit_state != 2 && !b->is_ops && !(b->inverted && !bot_can_drive(b));
  if (!bot_can_drive(b)) {
    /* no drive, no steering torque: the yaw rate decays on the floor (an inverted or stunned bot used to keep
     * whatever spin it had, forever); a tumbling airborne bot keeps its spin until it lands */
    if (!b->airborne) b->b.w *= 0.85f;
    if (wheels) wheel_step(m, slot, 0, 0, 0, 0);
    return;
  }
  float sx, sy; stick_vec(in, &sx, &sy);
  float top = bot_top_speed(b);
  if (b->carry_t > 0 && (b->lift > 0.3f || !b->is_ops)) top *= 0.6f;   /* carrying a victim */
  float accel = d->accel * (b->build.drive == DR_TREAD ? 1.0f : ar->traction_mult);
  if (b->dig_t > 0) accel *= 1.6f;                            /* DIG IN: the plate scrapes and the motors bite */
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
     * HOVER cannot push off whatever is leaning on it, which is what lets a wedge shove it anywhere; a
     * ridden hover is off the floor and has no thrust at all (load 0) */
    if (b->build.drive == DR_HOVER && b->skirt_t > 0) accel *= 0.25f;
    accel *= b->load;
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
    /* a HOVER with the stick at rest has no thrust: it drifts on its cushion (LAT_GRIP 0.08, the 0.985 drag);
     * MELTY keeps its skids and stops */
    if (b->build.drive != DR_HOVER || mag > 0.1f) { b->b.vx += dvx; b->b.vz += dvz; }
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
  float speed = len2(b->b.vx, b->b.vz);
  if (wheels) wheel_step(m, slot, 1, t, top, accel);
  float turn = d->turn_deg * (PI_F / 180.0f);
  /* stick right (s > 0) is a right turn, which is a heading decrease in this world (see right_of); a bot with
   * its wheels up turns no better than it drives (load) */
  float wt = -s * turn * (1.0f - 0.4f * fmin_f(speed / (top > 0 ? top : 1.0f), 1.0f)) * (b->build.weapon == WP_HORIZ && b->spin > 0.6f ? 0.7f : 1.0f) * fmin_f(b->load, 1.0f);
  if (b->inverted) wt *= 0.7f;
  b->b.w += rtr_clamp(wt - b->b.w, -20.0f * DT, 20.0f * DT);
}

/* -- weapons (4.6) ------------------------------------------------- */
static void spinner_follow(float *spin, int held, float spin_up) {
  *spin += (held ? 1.0f : -0.5f) * DT / spin_up;
  *spin = rtr_clamp(*spin, 0, 1);
}

/* the spinner's energy events (deep-physics 3.6): a drop of more than 0.15 spin is a bog (a hit, a clash,
 * a wall), and crossing SPINNER_HIT_MIN going up is the disc armed */
static void spin_lost(match_t *m, int slot, int weapon, float before, float after, int zone) {
  if (before - after <= 0.15f) return;
  bot_t *b = &m->bots[slot];
  ev_full(m, EV_SPIN_CHANGE, slot, slot, zone, b->b.x, b->b.z, b->b.y + b->b.clr + b->b.hgt, before - after, 0, 0, 0, weapon, 0, 0, before, 0);
}
static void spin_armed_check(match_t *m, int slot, int weapon) {
  bot_t *b = &m->bots[slot];
  if (b->spin > SPINNER_HIT_MIN && !b->spin_armed) {
    b->spin_armed = 1;
    ev_full(m, EV_SPIN_CHANGE, slot, slot, SPIN_ARMED, b->b.x, b->b.z, b->b.y + b->b.clr + b->b.hgt, b->spin, 0, 0, 0, weapon, 0, 0, b->spin, 0);
  } else if (b->spin < SPINNER_HIT_MIN - 0.05f) b->spin_armed = 0;
}
static void tell(match_t *m, int slot, int zone, int weapon) {
  bot_t *b = &m->bots[slot];
  float fx, fz; fwd_of(b->b.h, &fx, &fz);
  ev_full(m, EV_WEAPON_TELL, slot, slot, zone, b->b.x + fx * b->b.hl, b->b.z + fz * b->b.hl, b->b.y + b->b.clr + b->b.hgt * 0.5f, 0, fx, fz, PN_FRONT, weapon, 0, 0, 0, 0);
}

static void weapon_machine(match_t *m, int slot, int which) {
  bot_t *b = &m->bots[slot];
  int wid = which ? b->build.weapon2 : b->build.weapon;
  if (b->build.drive == DR_MELTY && !which && !b->is_ops) {
    spinner_follow(&b->spin, weapon_button(&b->in) && b->alive, WP_MELTY_SPIN_UP);
    if (b->alive) spin_armed_check(m, slot, EVW_MELTY);
    return;
  }
  if (wid == WP_NONE || wid == WP_WEDGE) {
    /* DIG IN (deep-physics 3.9): A or RT on a WEDGE or a bare build: 20 frames of 1.6x accel with the lip
     * scraping a hundredth lower, then 90 frames to rearm; the weapon button always does something */
    if (!which && !b->is_ops && b->alive && b->dig_cd == 0 && weapon_edge(&b->in, &b->prev_in) && bot_can_drive(b) && !b->inverted) {
      b->dig_t = 20; b->dig_cd = 90;
      float fx, fz; fwd_of(b->b.h, &fx, &fz);
      ev_full(m, EV_WEAPON_TELL, slot, slot, TELL_DIG_IN, b->b.x + fx * b->b.hl, b->b.z + fz * b->b.hl, b->b.y + 0.02f, 0, fx, fz, PN_FRONT, wid, 0, 0, 0, 0);
    }
    return;
  }
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
      if (!which && b->alive) spin_armed_check(m, slot, wid);
      break;
    case WP_SAW: {
      int was = b->saw_on;
      if (ew && operate) b->saw_on = !b->saw_on;
      if (!b->alive) b->saw_on = 0;
      if (b->saw_on != was && b->alive && !b->is_ops) tell(m, slot, b->saw_on ? TELL_SAW_ON : TELL_SAW_OFF, WP_SAW);
      spinner_follow(spin, b->saw_on && operate, w->spin_up);
      break;
    }
    case WP_FLIPPER:
      if (*active > 0) { (*active)--; if (*active == 0 && b->windup == 0) *cooldown = w->cooldown; }
      else if (ew && *cooldown == 0 && operate && !b->inverted && b->stun <= 0) { *active = w->active; tell(m, slot, TELL_WINDUP, WP_FLIPPER); }
      break;
    case WP_HAMMER:
      if (b->windup > 0) { b->windup--; if (b->windup == 0) *active = HAMMER_ACTIVE; }
      else if (*active > 0) {
        (*active)--;
        if (*active == 0) {
          *cooldown = w->cooldown;
          if (!b->saw_tick) {              /* saw_tick doubles as the landed flag for a swing */
            b->hp -= w->self_dmg;          /* a miss costs 4 HP (4.6) and strikes the floor */
            if (b->hp < 1 && b->is_ops) b->hp = 1;
            float fx, fz; fwd_of(b->b.h, &fx, &fz);
            float reach = HAMMER_PIVOT_Z + HAMMER_REACH;
            ev_full(m, EV_WEAPON_TELL, slot, slot, TELL_HAMMER_MISS, b->b.x + fx * reach, b->b.z + fz * reach, b->floor_y, 0, fx, fz, PN_FRONT, WP_HAMMER, 0, 0, 0, 0);
            hp_thresholds(m, slot);
          }
        }
      }
      else if (ew && *cooldown == 0 && operate && !b->inverted && b->stun <= 0) { b->windup = w->windup; b->saw_tick = 0; tell(m, slot, TELL_WINDUP, WP_HAMMER); }
      break;
    case WP_SPIKE:
      if (*active > 0) {
        (*active)--;
        if (*active == w->active - 5 && !b->is_ops) tell(m, slot, TELL_SPIKE_BACK, WP_SPIKE);   /* frame 6: the rod comes back */
        if (*active == 0) *cooldown = w->cooldown;
      }
      else if (ew && *cooldown == 0 && operate && b->stun <= 0) { *active = w->active; tell(m, slot, TELL_SPIKE_OUT, WP_SPIKE); }
      break;
    case WP_LIFTER: {
      if (b->is_ops) {
        float want = wb && operate ? 1.0f : 0.0f;
        b->lift += rtr_clamp(want - b->lift, -1.0f / WP_LIFTER_FRAMES, 1.0f / WP_LIFTER_FRAMES);
        break;
      }
      /* held, the forks stay down (the lip) and live; they rise only with a bot on them (holds_step) */
      b->fork_armed = (uint8_t)(wb && operate && !b->ride_by);
      float want = b->carry_t > 0 ? 1.0f : 0.0f;
      b->lift += rtr_clamp(want - b->lift, -1.0f / WP_LIFTER_RISE, 1.0f / WP_LIFTER_RISE);
      break;
    }
    case WP_FLAME: {
      int lit = wb && operate && *fuel > 0, bit = which ? 2 : 1;
      if (lit) { *fuel -= 1; }
      else if (!(wb && operate)) { *fuel += (float)WP_FLAME_FUEL / (float)WP_FLAME_REFILL; if (*fuel > WP_FLAME_FUEL) *fuel = WP_FLAME_FUEL; }
      if (lit && !(b->flame_on & bit)) { b->flame_on |= (uint8_t)bit; tell(m, slot, TELL_FLAME_ON, WP_FLAME); }
      else if (!lit && (b->flame_on & bit)) { b->flame_on &= (uint8_t)~bit; tell(m, slot, TELL_FLAME_OUT, WP_FLAME); }
      break;
    }
    case WP_CRUSHER:
      if (b->clamp > 0) { b->clamp--; if (b->clamp == 0) *cooldown = w->cooldown; }
      else if (ew && *cooldown == 0 && operate && b->stun <= 0 && b->clamped_by == 0) { b->clamp = w->windup + 1; tell(m, slot, TELL_CLAMP_START, WP_CRUSHER); }   /* counts to 1, the closing frame */
      break;
    default: break;
  }
}

/* -- hits (4.7, deep-physics 3.6 to 3.9) ---------------------------- */
static float closing_of(const bot_t *a, const bot_t *v, float nx, float nz, float tip) {
  float rel = dotp(a->b.vx - v->b.vx, a->b.vz - v->b.vz, nx, nz);
  return rtr_clamp((fabs_f(rel) + tip) / 4.0f, CLOSING_MIN, CLOSING_MAX);
}

/* a wheel, track or leg station hit hard enough (the hit band) costs the victim mobility (3.9) */
static void wheel_hit_effect(match_t *m, int att, int vic, int edge, float dmg, int weapon, float cx, float cz) {
  bot_t *v = &m->bots[vic];
  switch (v->build.drive) {
    case DR_TWO: v->wheel_hit = 240; break;                       /* top 0.6x */
    case DR_FOUR: case DR_SIX: v->wheel_hit = 180; break;         /* top 0.8x */
    case DR_TREAD: case DR_LEGTRACK: v->tread_hit = 180; break;   /* top 0.85x, lateral hold 0.7x */
    case DR_WALKER: v->limp_t = 120; break;                       /* load 0.7: a limp */
    default: return;
  }
  v->wheel_hit_side = (int8_t)(edge == PN_RIGHT ? 1 : -1);
  ev_full(m, EV_WHEEL_HIT, att, vic, 0, cx, cz, v->floor_y + v->b.clr, dmg, 0, 0, edge, weapon, EVF_WHEEL, HIT_WHY_WHEEL, 0, 0);
}

/* the direction a disc's blow throws a body: a face can only push back along its normal plus the friction
 * the tooth drags along it, so the impulse leaves inside the friction cone round the normal, leaning the way
 * the tooth was going (perpendicular to the disc radius, in the spin sense: the discs turn heading-positive,
 * so the tooth at r moves along (rz, -rx)). TOOTH_MU 0.8 leans it about 40 degrees: the victim is thrown
 * away and spun, the disc kicked back off it */
#ifndef TOOTH_MU
#define TOOTH_MU 0.8f
#endif
static void tooth_dir(float ox, float oz, float cx, float cz, float nx, float nz, float *dx, float *dz) {
  float rx = cx - ox, rz = cz - oz, l = len2(rx, rz);
  float tx = l < 1e-4f ? -nz : rz / l, tz = l < 1e-4f ? nx : -rx / l;
  float ux = nx + TOOTH_MU * tx, uz = nz + TOOTH_MU * tz, ul = len2(ux, uz);
  if (ul < 1e-4f) { *dx = nx; *dz = nz; return; }
  *dx = ux / ul; *dz = uz / ul;
}

/* the LIFTER's forks touch a bot (armed, down, nothing on them): it is grabbed from any side, head on too, for
 * WP_LIFTER_GRAB_DMG through the armour, unless its front lip is lower than the forks' and faces them (a WEDGE
 * chassis or plate): then the forks ride up the wedge, nothing is grabbed, and the plates clash. OPS is never
 * grabbed, nor a bot in the air (unless the forks are under it), held or clamped */
static void lifter_grab(match_t *m, int att, int vic, const contact_t *c, float nx, float nz) {
  bot_t *a = &m->bots[att], *v = &m->bots[vic];
  int16_t *cd = &a->hit_cd[vic];
  if (v->is_ops || v->lifted_by || v->clamped_by || v->falling || a->carry_t > 0 || !a->fork_armed || a->ride_by) return;
  if (v->airborne && v->ride_by != att + 1) return;
  int edge = edge_of(v, c->cx, c->cz);
  float afx, afz, vfx, vfz; fwd_of(a->b.h, &afx, &afz); fwd_of(v->b.h, &vfx, &vfz);
  int facing = dotp(afx, afz, vfx, vfz) < -0.5f;
  float vlip = v->inverted ? 0 : v->b.lip_f;
  float cl = dotp(a->b.vx - v->b.vx, a->b.vz - v->b.vz, nx, nz);
  if (v->riding == att + 1 || (vlip > 0 && vlip + 0.005f < WEDGE_KIND[LIP_FORKS].lip && (edge == PN_FRONT || facing))) {
    *cd = 30;
    ev_full(m, EV_WEDGE_CLASH, att, vic, 0, c->cx, c->cz, a->b.y + 0.05f, cl, nx, nz, PN_FRONT, WP_LIFTER, 0, HIT_WHY_WEDGE, cl, 0);
    return;
  }
  if (v->ride_by) ride_clear(m, vic);
  hit_t h = { 0 };
  h.att = att; h.vic = vic; h.weapon = WP_LIFTER; h.raw = WP_LIFTER_GRAB_DMG; h.stun = WEAPONS[WP_LIFTER].stun;
  h.cx = c->cx; h.cz = c->cz; h.cy = v->b.y + v->b.clr; h.nx = nx; h.nz = nz; h.ev_type = EV_HIT; h.zone = 1.0f; h.panel = edge;
  h.speed = fmax_f(cl, 0); h.j = 0; h.wear = 1;
  apply_hit(m, &h);
  *cd = WP_LIFTER_MAX_HOLD + WP_LIFTER_LIFT_CD;          /* the release sets the 120 that counts */
  v->lifted_by = (uint8_t)(att + 1); v->lift_edge = (uint8_t)edge; v->airborne = 0; v->lift_from = 0; v->soft_land = 0;
  a->carry_t = 1; a->lift_esc_t = 0;
  ev_full(m, EV_LIFT, att, vic, 0, v->b.x, v->b.z, v->b.y, 0, 0, 0, edge, WP_LIFTER, 0, 0, 0, 0);
}

/* a weapon hitbox overlapping a victim body: one hit. p is the hammer's share of its swing at the contact
 * and handle 1 when the arm met the victim before the head (3.7); both unused by the rest */
static void weapon_hit(match_t *m, int att, int vic, int which, const contact_t *c, float p, int handle) {
  bot_t *a = &m->bots[att], *v = &m->bots[vic];
  int wid = which ? a->build.weapon2 : a->build.weapon;
  int melty = a->build.drive == DR_MELTY && !which && !a->is_ops;
  int w = a->is_ops ? EVW_OPS : melty ? EVW_MELTY : wid;
  int16_t *cd = which ? &a->hit_cd2[vic] : &a->hit_cd[vic];
  if (*cd > 0) return;
  float *spin = which ? &a->spin2 : &a->spin;
  float nx = v->b.x - a->b.x, nz = v->b.z - a->b.z, nl = len2(nx, nz);
  if (nl < 1e-4f) { nx = 0; nz = 1; } else { nx /= nl; nz /= nl; }
  float afx, afz; fwd_of(a->b.h, &afx, &afz);
  int spinner = melty || wid == WP_HORIZ || wid == WP_DRUM;
  float tipk = melty ? MELTY_TIP : wid == WP_HORIZ ? TIP_HORIZ : wid == WP_DRUM ? TIP_DRUM : 0;
  float rel = dotp(a->b.vx - v->b.vx, a->b.vz - v->b.vz, nx, nz);
  /* the disc centre (the knock's tangent is taken about it) */
  float ox = a->b.x, oz = a->b.z;
  if (!melty && spinner) { ox += afx * WEAPONS[wid].hb_z0; oz += afz * WEAPONS[wid].hb_z0; }
  float base = melty ? WP_MELTY_DMG : WEAPONS[wid].base, kv = melty ? 4.0f : WEAPONS[wid].knock;
  if (wid == WP_FLIPPER || wid == WP_HAMMER || wid == WP_SPIKE) a->saw_tick = 1;
  if (wid == WP_HAMMER) { if (which) a->active2 = 0; else a->active = 0; *(which ? &a->cooldown2 : &a->cooldown) = WEAPONS[WP_HAMMER].cooldown; }   /* the first contact ends the swing */
  if (!v->alive) {
    /* a corpse (DEEP-plan 3, ghost cause 2): the weapon meets it like a prop. Sparks and the knock, the
     * spinner bogs at half the usual cost, no damage, no tallies, no count reset, one contact per 30 frames */
    *cd = 30;
    float J = kv * 0.5f, dx = nx, dz = nz;
    if (spinner) {
      float s0 = *spin, e = s0 * s0, bite = rtr_clamp(BITE_BASE + BITE_SLOPE * (tipk * s0 + rel), BITE_MIN, BITE_MAX);
      e -= e * bite * 0.5f; *spin = sqrtf(e > 0 ? e : 0);
      if (wid != WP_DRUM) tooth_dir(ox, oz, c->cx, c->cz, nx, nz, &dx, &dz);
      knock_at(a, -dx * J * 0.7f, -dz * J * 0.7f, c->cx, c->cz);
    }
    knock_at(v, dx * J, dz * J, c->cx, c->cz);
    ev_full(m, EV_HIT, att, vic, 10, c->cx, c->cz, v->b.y + v->b.clr + v->b.hgt * 0.5f, 0, nx, nz, edge_of(v, c->cx, c->cz), w, EVF_CORPSE, HIT_WHY_NONE, fmax_f(rel, 0) + tipk * *spin, J);
    return;
  }
  if (wid == WP_LIFTER && !a->is_ops) { lifter_grab(m, att, vic, c, nx, nz); return; }
  /* the contact: zone, panel and the location factor (3.12) */
  int flags = 0, reason = 0;
  int edge = edge_of(v, c->cx, c->cz), panel = edge;
  float zone = contact_zone(v, w, c->cx, c->cz, &flags, &reason);
  if (which == 1 && a->riding == vic + 1) { zone = 2.0f; flags &= ~EVF_DEFLECT; reason = HIT_WHY_EXPOSED; }   /* the 2ND weapon under the bot the wedge rides */
  float loc = 1.0f;
  int wheel = wheel_station(v, c->cx, c->cz, edge);
  if (wheel) { loc *= 1.2f; flags |= EVF_WHEEL; if (reason != HIT_WHY_EXPOSED) reason = HIT_WHY_WHEEL; }
  float raw, energy = 1, self = 0, speed = 0, cy = v->b.y + v->b.clr + v->b.hgt * 0.5f;
  int stun = melty ? 18 : WEAPONS[wid].stun, wear = 1;
  if (spinner) {
    /* spinner energy (3.6): the disc stores spin^2; the bite is the share a hit takes, more on a target
     * driving into the disc, a quarter on a wedge front or a SHELL that sheds it; the hit is the energy */
    float s0 = *spin, e = s0 * s0, tip = tipk * s0;
    float bite = rtr_clamp(BITE_BASE + BITE_SLOPE * (tip + rel), BITE_MIN, BITE_MAX);
    if ((flags & EVF_DEFLECT) || v->build.chassis == CH_SHELL) bite = 0.25f;
    float eh = e * bite;
    energy = eh / SPIN_E_REF;
    raw = base * energy * zone * loc;
    e -= eh;
    if (v->build.armour == AR_WEAR) e -= 0.1f;                   /* a WEAR PLATE throws the disc back */
    *spin = sqrtf(e > 0 ? e : 0);
    if (!which) { spin_lost(m, att, w, s0, *spin, SPIN_LOST_HIT); a->spent_t = 20; }
    if (energy < 0.75f && (reason == HIT_WHY_NONE || reason == HIT_WHY_SHELL)) reason = HIT_WHY_SPINNING_DOWN;
    *cd = melty ? MELTY_HIT_CD : SPINNER_HIT_CD;
    self = (melty ? WP_MELTY_SELF : WEAPONS[wid].self_dmg) * energy;
    speed = fmax_f(rel, 0) + tip;
  } else switch (wid) {
    case WP_SAW:
      /* a grind, not a swing (3.9): a tick every 10 frames of contact, 2x on a TREADS side; the plate wears by
       * the grind (hit_test), and the blade bogs 0.02 of its energy a tick, 0.04 on STEEL and WEAR PLATE */
      raw = base * zone * loc * (v->build.drive == DR_TREAD && (edge == PN_LEFT || edge == PN_RIGHT) ? 2.0f : 1.0f);
      *cd = WP_SAW_TICK_FRAMES; self = 2.0f / 6.0f; stun = 6; wear = 0;
      { float e = *spin * *spin - (v->build.armour == AR_STEEL || v->build.armour == AR_WEAR ? 0.04f : 0.02f); *spin = sqrtf(e > 0 ? e : 0); }
      speed = 2.0f * *spin;
      break;
    case WP_FLIPPER:
      raw = base * closing_of(a, v, nx, nz, TIP_SWING) * zone * loc;
      *cd = (int16_t)((which ? a->active2 : a->active) + 2); speed = fabs_f(rel) + TIP_SWING;
      break;
    case WP_HAMMER:
      /* the head lands on the TOP: its energy grows with the share of the swing it fell through (p; the arc
       * meets a LUNCHBOX at p 0.7 and a LOWBOY at 0.9, 14 and 18 raw), the lid takes 1.25x unless it is WEAR
       * PLATE; a victim hugging the hammer meets the arm first and gets the handle, 0.3 of the base */
      panel = PN_TOP; flags |= EVF_TOP;
      if (handle) { raw = 0.3f * base; loc = 1.0f; }
      else raw = base * p * zone * loc * (v->build.armour == AR_WEAR ? 1.0f : 1.25f);
      *cd = 30; speed = TIP_SWING * (0.5f + p);
      cy = v->b.y + v->b.clr + v->b.hgt;
      v->b.pitch -= 0.2f;                                          /* the nose dips under the blow */
      break;
    case WP_SPIKE: {
      /* the rod (3.7): full base on the way out (frames 1 to 5), 0.3 on the way back */
      int act = which ? a->active2 : a->active, f = WEAPONS[WP_SPIKE].active + 1 - act;
      raw = base * zone * loc * (f <= 5 ? 1.0f : 0.3f);
      *cd = (int16_t)(act + 2); speed = 6.0f;
      break;
    }
    case WP_LIFTER: raw = base * zone * loc; *cd = 600; break;
    case WP_CRUSHER: raw = base * zone * loc; *cd = 200; tell(m, att, TELL_CLAMP_BITE, WP_CRUSHER); break;   /* the jaws close on that edge; the hold works the TOP */
    default: raw = base * zone * loc; *cd = 30; break;
  }
  hit_t h = { 0 };
  h.att = att; h.vic = vic; h.weapon = w; h.raw = raw; h.stun = stun; h.cx = c->cx; h.cz = c->cz; h.cy = cy; h.nx = nx; h.nz = nz;
  h.ev_type = EV_HIT; h.zone = zone; h.panel = panel; h.flags = flags; h.reason = reason; h.speed = speed; h.wear = wear;
  /* the knock (3.6, 3.7): a spinner's by the energy it gave up, the rest by the damage as today */
  float J = spinner ? KNOCK_SPIN(wid, melty) * (0.4f + 0.6f * energy) : wid == WP_SAW ? 0.4f : kv * (0.5f + raw * (1.0f - ARMOURS[v->build.armour].reduction) / 30.0f);
  h.j = J;
  float dmg = apply_hit(m, &h);
  if (dmg <= 0 && wid != WP_LIFTER && wid != WP_CRUSHER) return;
  if (spinner && wid != WP_DRUM) {
    float dx, dz; tooth_dir(ox, oz, c->cx, c->cz, nx, nz, &dx, &dz);
    knock_at(v, dx * J, dz * J, c->cx, c->cz);
    knock_at(a, -dx * J * 0.7f, -dz * J * 0.7f, c->cx, c->cz);   /* the blow kicks the disc back and drags its frame round */
  } else if (wid == WP_DRUM) {
    knock_at(v, nx * J, nz * J, c->cx, c->cz); knock(a, -nx * J * 0.7f, -nz * J * 0.7f);
    if (!v->lifted_by && v->build.drive != DR_HOVER && !v->airborne) {
      /* the drum pops the near edge up and over (a small turn, not a flip) */
      if (v->ride_by) ride_end(m, vic, RIDE_BACKED_OFF);
      v->b.vy = 3.0f; v->airborne = 1; v->peak_y = v->b.y; v->bounced = 0;
      float wp = CHASSIS_TUMBLE[v->build.chassis] * 0.4f * energy;
      if (edge == PN_FRONT) v->b.wp = wp; else if (edge == PN_BACK) v->b.wp = -wp; else if (edge == PN_RIGHT) v->b.wr = wp; else v->b.wr = -wp;
      ev_full(m, EV_LAUNCH, att, vic, LAUNCH_DRUM, v->b.x, v->b.z, v->b.y, 3.0f, nx, nz, edge, WP_DRUM, 0, 0, 3.0f, wp);
    }
  } else knock_at(v, nx * J, nz * J, c->cx, c->cz);
  /* a spinner glancing off a wedge front is thrown up the slope and back off it (3.6): the hop, and the
   * plate's push along the normal (the disc cannot bite, so the reaction comes straight back at it) */
  if ((flags & EVF_DEFLECT) && spinner && !a->lifted_by && !a->clamped_by && a->build.drive != DR_HOVER && !a->airborne) {
    a->b.vy = DEFLECT_HOP_VY; a->airborne = 1; a->peak_y = a->b.y; a->bounced = 0;
    knock(a, -nx * J, -nz * J);
  }
  /* self damage per table, scaled by the energy a spinner gave up; grinding on the referee costs nothing */
  if (self > 0 && wid != WP_HAMMER && !v->is_ops) { a->hp -= self; if (a->is_ops && a->hp < 1) a->hp = 1; hp_thresholds(m, att); }
  if (wheel && dmg >= BAND_HIT_MIN) wheel_hit_effect(m, att, vic, edge, dmg, w, c->cx, c->cz);
  switch (wid) {
    case WP_FLIPPER:
      if (!v->lifted_by && !v->is_ops && (!v->airborne || v->ride_by)) {
        /* deep-physics 3.8: k is how far back on the plate the victim sits (0 at the tip z 0.9, 1 at the hinge
         * z 0.4); deep on the plate throws higher and turns faster, a victim driving into the plate turns
         * more; the edge nearest the flipper goes up. No dice: the landing decides from the turn */
        float fx = afx, fz = afz;
        /* the victim's near face along the plate: a bot touching the flipper's front sits at z 0.5 (k 0.8) */
        float lz = (dotp(v->b.x - a->b.x, v->b.z - a->b.z, fx, fz) - body_extent(v, fx, fz)) / (a->is_ops ? 1.4f : 1.0f);
        float k = rtr_clamp((0.9f - lz) / 0.5f, 0, 1);
        float vy = 7.0f * (v->build.chassis == CH_LOWBOY ? 0.8f : 1.0f) * (0.85f + 0.15f * k);
        float in = fmax_f(dotp(v->b.vx, v->b.vz, -fx, -fz), 0);
        float wp = CHASSIS_TUMBLE[v->build.chassis] * (0.7f + 0.6f * k) + 0.3f * in;
        if (v->ride_by) ride_end(m, vic, RIDE_BACKED_OFF);
        float hv = 3.0f * (v->build.drive == DR_HOVER ? 1.5f : 1.0f);
        v->b.vy = vy; v->b.vx += fx * hv; v->b.vz += fz * hv;
        int ed = edge_of(v, c->cx, c->cz);
        if (ed == PN_FRONT) v->b.wp = wp; else if (ed == PN_BACK) v->b.wp = -wp; else if (ed == PN_RIGHT) v->b.wr = wp; else v->b.wr = -wp;
        v->airborne = 1; v->flip_pending = 1; v->launched_by = (uint8_t)(att + 1); v->peak_y = v->b.y; v->bounced = 0; v->tipping = 0;
        ev_full(m, EV_LAUNCH, att, vic, LAUNCH_FLIPPER, v->b.x, v->b.z, v->b.y, vy, fx, fz, ed, WP_FLIPPER, 0, 0, vy, wp);
      }
      break;
    case WP_LIFTER:
      if (!v->is_ops && (!v->airborne || v->ride_by) && !v->lifted_by && !v->clamped_by && a->carry_t == 0) {
        /* only a victim facing away or sideways (within 90 degrees), unless the forks are already under it
         * (a ride converts to the hold, 3.9); OPS, the referee, lifts whatever it reaches */
        float vfx, vfz; fwd_of(v->b.h, &vfx, &vfz);
        int under = v->ride_by == att + 1;
        if (a->is_ops || under || dotp(afx, afz, vfx, vfz) >= 0) {
          if (v->ride_by) ride_clear(m, vic);
          v->lifted_by = (uint8_t)(att + 1); a->carry_t = 1;
          ev_full(m, EV_LIFT, att, vic, a->is_ops ? 1 : 0, v->b.x, v->b.z, v->b.y, 0, 0, 0, panel, a->is_ops ? EVW_OPS : WP_LIFTER, 0, 0, 0, 0);
        }
      }
      break;
    case WP_CRUSHER:
      if (!v->is_ops && !v->lifted_by && !v->clamped_by) {
        if (v->ride_by) ride_end(m, vic, RIDE_BACKED_OFF);
        v->clamped_by = (uint8_t)(att + 1); a->lock = 120; v->lock = 120; a->crush_t = 0;
      }
      break;
    default: break;
  }
}

/* two spinners at spin over 0.5 whose circles overlap (4.7, 3.6): each loses the lesser disc's energy (the
 * lesser stops dead, the greater keeps the rest), each takes 0.4 of the other's base times its energy */
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
      float ea = a->spin * a->spin, eb = b->spin * b->spin, loss = fmin_f(ea, eb);
      float da = WEAPONS[a->build.weapon].base * ea * CLASH_FRAC, db = WEAPONS[b->build.weapon].base * eb * CLASH_FRAC;
      float mx = (sa.x + sb.x) * 0.5f, mz = (sa.z + sb.z) * 0.5f;
      apply_damage(m, i, j, da, a->build.weapon, 30, mx, mz, c.nx, c.nz, -1, 1.0f);
      apply_damage(m, j, i, db, b->build.weapon, 30, mx, mz, -c.nx, -c.nz, -1, 1.0f);
      float sa0 = a->spin, sb0 = b->spin;
      a->spin = sqrtf(fmax_f(ea - loss, 0)); b->spin = sqrtf(fmax_f(eb - loss, 0));
      spin_lost(m, i, a->build.weapon, sa0, a->spin, SPIN_LOST_CLASH); spin_lost(m, j, b->build.weapon, sb0, b->spin, SPIN_LOST_CLASH);
      a->stun = 30; b->stun = 30; a->clashes++; b->clashes++;
      a->hit_cd[j] = CLASH_CD; b->hit_cd[i] = CLASH_CD;
      knock(a, -c.nx * 6.0f, -c.nz * 6.0f); knock(b, c.nx * 6.0f, c.nz * 6.0f);
      ev_full(m, EV_CLASH, i, j, 10, mx, mz, a->b.y + a->b.clr + a->b.hgt * 0.5f, da + db, c.nx, c.nz, PN_FRONT, a->build.weapon, 0, 0, dotp(a->b.vx - b->b.vx, a->b.vz - b->b.vz, c.nx, c.nz), 6.0f);
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
    if (!(v->alive || is_corpse(v)) || !same_band(a, v)) continue;
    int16_t *cd = which ? &a->hit_cd2[j] : &a->hit_cd[j];
    float dx = v->b.x - nx0, dz = v->b.z - nz0, d = len2(dx, dz), rad = fmax_f(v->b.hl, v->b.hw);
    if (d - rad > WEAPONS[WP_FLAME].hb_z1) continue;
    if (d > 0.05f && dotp(dx, dz, fx, fz) / d < 0.866f && d > rad) continue;
    if (*cd > 0) continue;
    *cd = WP_FLAME_TICK_FRAMES;
    float cx = v->b.x - (d > 0.05f ? dx / d : 0) * rad * 0.8f, cz = v->b.z - (d > 0.05f ? dz / d : 0) * rad * 0.8f;
    if (!v->alive) { ev_full(m, EV_HIT, att, j, 10, cx, cz, v->b.y + v->b.clr + v->b.hgt * 0.5f, 0, fx, fz, edge_of(v, cx, cz), WP_FLAME, EVF_CORPSE, HIT_WHY_NONE, 0, 0); continue; }
    hit_t h = { 0 };
    int flags = 0, reason = 0, edge = edge_of(v, cx, cz);
    float zone = contact_zone(v, WP_FLAME, cx, cz, &flags, &reason);
    h.att = att; h.vic = j; h.weapon = WP_FLAME; h.raw = WEAPONS[WP_FLAME].base; h.stun = 0; h.cx = cx; h.cz = cz; h.nx = fx; h.nz = fz;
    h.ev_type = EV_HIT; h.zone = 1.0f; h.panel = edge; h.flags = flags & ~EVF_DEFLECT; h.reason = reason == HIT_WHY_EXPOSED ? reason : 0; h.wear = 0;
    (void)zone;
    float dmg = apply_hit(m, &h);
    /* a burning bot is not lit again until the burn ends: a held cone is 3 per tick plus one burn per approach */
    if (dmg > 0 && !ARMOURS[v->build.armour].burn_immune && !v->is_ops && v->burn_t <= 0) {
      v->burn_t = WP_FLAME_BURN_T; v->burn_dps = WP_FLAME_BURN_DPS;
      ev_full(m, EV_BURN_START, att, j, 0, v->b.x, v->b.z, v->b.y + v->b.clr + v->b.hgt, 0, 0, 0, PN_TOP, WP_FLAME, 0, 0, 0, 0);
    }
  }
}

/* the weapons that grab: a lifter's forks and a crusher's jaws never take hold of a corpse */
static int wid0_lifts(const bot_t *a, int which) { int w = which ? a->build.weapon2 : a->build.weapon; return w == WP_LIFTER || w == WP_CRUSHER; }

/* the hammer head against a victim's body in the attacker's vertical plane (3.7): the head circle at the
 * swing share p must meet the victim's box (its length along my forward, its height band) and the victim
 * must be under the head sideways. Returns 1 for the head, 2 for the arm (the handle), 0 for a miss */
static int hammer_meets(const bot_t *a, const bot_t *v, float p) {
  float fx, fz, rx, rz; fwd_of(a->b.h, &fx, &fz); right_of(a->b.h, &rx, &rz);
  float dx = v->b.x - a->b.x, dz = v->b.z - a->b.z;
  float vz = dotp(dx, dz, fx, fz), vx = dotp(dx, dz, rx, rz);
  float ext = body_extent(v, fx, fz), side = body_extent(v, rx, rz);
  if (fabs_f(vx) > side + HAMMER_HEAD_R) return 0;
  float z0 = vz - ext, z1 = vz + ext, y0 = v->b.y + v->b.clr, y1 = y0 + v->b.hgt;
  float hz, hy, py, pz; hammer_head(a, p, &hz, &hy, &py, &pz);
  float qz = rtr_clamp(hz, z0, z1), qy = rtr_clamp(hy, y0, y1);
  if (len2(hz - qz, hy - qy) < HAMMER_HEAD_R) return 1;
  /* the arm from the pivot to the head (a segment against the box: the slab clip) */
  float t0 = 0, t1 = 1, sz = hz - pz, sy = hy - py;
  for (int k = 0; k < 2; k++) {
    float o = k ? py : pz, d = k ? sy : sz, lo = k ? y0 : z0, hi = k ? y1 : z1;
    if (fabs_f(d) < 1e-6f) { if (o < lo || o > hi) return 0; continue; }
    float ta = (lo - o) / d, tb = (hi - o) / d; if (ta > tb) { float t = ta; ta = tb; tb = t; }
    if (ta > t0) t0 = ta; if (tb < t1) t1 = tb;
    if (t0 > t1) return 0;
  }
  return 2;
}

/* weapon hitboxes vs bodies, lifter and crusher holds, prop damage (4.6, 4.7) */
static void hit_test(match_t *m) {
  const arena_def_t *ar = sim_arena(m);
  for (int i = 0; i < m->nbots; i++) {
    bot_t *a = &m->bots[i];
    if (!a->alive) continue;
    if (a->lifted_by) { if (a->grind_on) { a->grind_on = 0; a->grind_t = 0; } continue; }   /* a carried bot's weapon is inert while it hangs off the forks (4.6) */
    int grinding = 0;
    for (int which = 0; which < 2; which++) {
      if (which && a->build.weapon2 == WP_NONE) continue;
      if (which == 0 && a->build.drive != DR_MELTY && (a->build.weapon == WP_NONE || a->build.weapon == WP_WEDGE)) continue;
      shape_t ws; if (!weapon_shape(a, which, &ws)) continue;
      int wid = which ? a->build.weapon2 : a->build.weapon;
      for (int j = 0; j < m->nbots; j++) {
        if (j == i) continue;
        bot_t *v = &m->bots[j];
        if (!(v->alive || is_corpse(v)) || v->falling || v->pit_state == 2) continue;
        if (!v->alive && (wid0_lifts(a, which) || a->is_ops)) continue;     /* nobody lifts or clamps a corpse */
        if (v->lifted_by == i + 1 && which == 0 && a->build.weapon == WP_LIFTER) continue;
        if (wid == WP_HAMMER) {
          /* the head sweeps down through this frame's share of the arc: two samples so a thin lid is not skipped */
          /* this frame's share of the swing, in eight steps: the first of the head or the arm to meet the
           * victim decides (a tall bot hugging the hammer meets the arm before the head comes down) */
          int act = which ? a->active2 : a->active, hit = 0;
          float p1 = rtr_clamp((float)(HAMMER_ACTIVE + 1 - act) / (float)HAMMER_ACTIVE, 0, 1), p = p1;
          for (int k = 1; k <= 8 && !hit; k++) { p = p1 - (1.0f - (float)k / 8.0f) / (float)HAMMER_ACTIVE; if (p < 0) p = 0; hit = hammer_meets(a, v, p); }
          if (!hit) continue;
          float fx, fz; fwd_of(a->b.h, &fx, &fz);
          float hz, hy; hammer_head(a, p, &hz, &hy, 0, 0);
          contact_t c = { fx, fz, 0, a->b.x + fx * hz, a->b.z + fz * hz };
          weapon_hit(m, i, j, which, &c, p, hit == 2);
          break;                                                      /* the first contact ends the swing */
        }
        /* a bot above or below the weapon's height band is not in reach */
        if (!same_band(a, v)) continue;
        shape_t vs; shape_of(v, &vs);
        contact_t c;
        if (!collide(&ws, &vs, &c)) continue;
        if (wid == WP_SAW && v->alive) {
          /* the grind (3.9): every frame of contact, the plate under the blade wears a point per 6 frames */
          grinding = 1;
          if (!a->grind_on) { a->grind_on = 1; a->grind_t = 0; ev_full(m, EV_GRIND, i, j, GRIND_START, c.cx, c.cz, v->b.y + v->b.clr + v->b.hgt * 0.5f, 0, 0, 0, edge_of(v, c.cx, c.cz), WP_SAW, 0, 0, 0, 0); }
          a->grind_t++; a->grind_x = c.cx; a->grind_z = c.cz;
          if (a->grind_t % 6 == 0) wear_panel(m, i, j, edge_of(v, c.cx, c.cz), 1.0f);
        }
        weapon_hit(m, i, j, which, &c, 0, 0);
      }
      /* breakable props: a spinner or a hammer in contact wears a bench or a box down */
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
    if (!grinding && a->grind_on) {
      a->grind_on = 0;
      ev_full(m, EV_GRIND, i, i, GRIND_END, a->grind_x, a->grind_z, a->b.y + a->b.clr, 0, 0, 0, 0, WP_SAW, 0, 0, 0, 0);
      a->grind_t = 0;
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

/* a lifted bot is let go (the holder lowered, was stunned, timed out, or was beaten) */
static void lift_release(match_t *m, int holder, int vic) {
  bot_t *v = &m->bots[vic];
  v->lifted_by = 0; v->b.vy = 0.5f; v->airborne = 1; v->peak_y = v->b.y; v->bounced = 0;
  ev_full(m, EV_LIFT_DROP, holder, vic, m->bots[holder].is_ops ? 1 : 0, v->b.x, v->b.z, v->b.y, 0, 0, 0, 0, m->bots[holder].is_ops ? EVW_OPS : WP_LIFTER, 0, 0, 0, 0);
}

/* the tip: let go at full lift, the victim goes over the forks, up and forward, the near edge first, at the
 * flipper's tumble rate for its chassis (the holder driving forward flicks it harder); the flipper's landing
 * decides the face it comes down on */
static void lifter_tip(match_t *m, int holder, int vic) {
  bot_t *a = &m->bots[holder], *v = &m->bots[vic];
  float fx, fz; fwd_of(a->b.h, &fx, &fz);
  float s = rtr_clamp(dotp(a->b.vx, a->b.vz, fx, fz), 0, 3.0f);
  float vy = WP_LIFTER_TIP_VY, wp = CHASSIS_TUMBLE[v->build.chassis] * (0.8f + 0.1f * s);
  int ed = v->lift_edge;
  v->lifted_by = 0; v->airborne = 1; v->b.vy = vy;
  v->b.vx = a->b.vx + fx * WP_LIFTER_TIP_FWD; v->b.vz = a->b.vz + fz * WP_LIFTER_TIP_FWD; v->b.w = 0;
  if (ed == PN_FRONT) v->b.wp = wp; else if (ed == PN_BACK) v->b.wp = -wp; else if (ed == PN_RIGHT) v->b.wr = wp; else v->b.wr = -wp;
  v->flip_pending = 1; v->launched_by = (uint8_t)(holder + 1); v->peak_y = v->b.y; v->bounced = 0; v->tipping = 0;
  v->lift_from = (uint8_t)(holder + 1); v->soft_land = 0;
  a->carry_t = 0; a->hit_cd[vic] = WP_LIFTER_LIFT_CD; a->lift_esc_t = 0;
  ev_full(m, EV_LAUNCH, holder, vic, LAUNCH_FLIPPER, v->b.x, v->b.z, v->b.y, vy, fx, fz, ed, WP_LIFTER, 0, 0, vy, wp);
}

/* a held bot over an open pit or past an open edge: let go, it drops straight in (the lifter's real kill) */
static int haz_contains(const hazard_def_t *h, float x, float z, float grow);
static int over_hole(const match_t *m, const bot_t *v) {
  const arena_def_t *ar = sim_arena(m);
  float hw = ar->w * 0.5f, hd = ar->d * 0.5f, x = v->b.x, z = v->b.z;
  if (((ar->open_mask & OPEN_PX) && x > hw + 0.3f) || ((ar->open_mask & OPEN_NX) && x < -hw - 0.3f) ||
      ((ar->open_mask & OPEN_PZ) && z > hd + 0.3f) || ((ar->open_mask & OPEN_NZ) && z < -hd - 0.3f)) return 1;
  if (!m->hazards_on || v->build.drive == DR_HOVER) return 0;
  for (int i = 0; i < ar->nhaz; i++) if (ar->haz[i].type == HZ_PIT && m->haz[i].active && haz_contains(&ar->haz[i], x, z, -0.3f)) return 1;
  return 0;
}

/* the LIFTER's carry, every frame: put down (let go early, the 3 s limit, the escape, the holder beaten), tipped
 * (let go at full lift), or carried on the forks at the holder's front, near edge up */
static void lifter_hold(match_t *m, int holder, int vic) {
  bot_t *a = &m->bots[holder];
  if (vic < 0) { a->carry_t = 0; a->lift_esc_t = 0; return; }
  bot_t *v = &m->bots[vic];
  /* the escape: its stick held and its push over 1.3x mine, WP_LIFTER_ESCAPE frames running */
  float sx, sy; stick_vec(&v->in, &sx, &sy);
  int pushing = len2(sx, sy) > 0.5f || (v->scheme == SCHEME_TANK && (v->in.ry > 16384 || v->in.ry < -16384));
  if (pushing && build_push(&v->build) > 1.3f * build_push(&a->build)) a->lift_esc_t++; else a->lift_esc_t = 0;
  int drop = !a->alive || !v->alive || a->stun > 0 || a->inverted || a->ride_by || a->lifted_by || a->clamped_by ||
             a->carry_t > WP_LIFTER_MAX_HOLD || a->lift_esc_t >= WP_LIFTER_ESCAPE;
  if (!drop && !a->fork_armed && a->lift > WP_LIFTER_TIP_LIFT && a->carry_t >= WP_LIFTER_TIP_HOLD && !over_hole(m, v)) { lifter_tip(m, holder, vic); return; }
  if (drop || !a->fork_armed) {
    lift_release(m, holder, vic);
    v->soft_land = 1; v->lift_from = (uint8_t)(holder + 1);
    a->carry_t = 0; a->hit_cd[vic] = WP_LIFTER_LIFT_CD; a->lift_esc_t = 0;
    return;
  }
  float fx, fz; fwd_of(a->b.h, &fx, &fz);
  float reach = a->b.hl + body_extent(v, fx, fz) * 0.8f;
  v->b.h = rtr_wrap_pi(v->b.h + rtr_wrap_pi(a->b.h - a->b.ph));      /* it turns with the forks */
  v->b.x = a->b.x + fx * reach; v->b.z = a->b.z + fz * reach;
  clamp_inside(m, v);
  float want_y = a->floor_y + WP_LIFTER_HOLD_Y + WP_LIFTER_RAISE_Y * a->lift;
  v->b.y += rtr_clamp(want_y - v->b.y, -0.05f, 0.05f);
  v->b.vy = 0; v->b.vx = a->b.vx; v->b.vz = a->b.vz; v->b.w = 0; v->airborne = 0;
  /* the edge on the forks is up 20 degrees (a SHELL's dome sits level on them) */
  float ang = v->build.chassis == CH_SHELL ? 0 : 0.349f, wp_ = 0, wr_ = 0;
  if (v->lift_edge == PN_FRONT) wp_ = ang; else if (v->lift_edge == PN_BACK) wp_ = -ang; else if (v->lift_edge == PN_RIGHT) wr_ = ang; else wr_ = -ang;
  v->b.pitch += rtr_clamp(wp_ - v->b.pitch, -6.0f * DT, 6.0f * DT);
  v->b.roll += rtr_clamp(wr_ - v->b.roll, -6.0f * DT, 6.0f * DT);
  v->b.wp = v->b.wr = 0;
  a->control += 1; v->countout = 0;
  a->carry_t++;
}

/* lifter and crusher holds, every frame (4.6) */
static void holds_step(match_t *m) {
  for (int i = 0; i < m->nbots; i++) {
    bot_t *a = &m->bots[i];
    /* lifting */
    if (a->carry_t > 0 && !a->is_ops) { int vic = -1; for (int j = 0; j < m->nbots; j++) if (m->bots[j].lifted_by == i + 1) vic = j; lifter_hold(m, i, vic); }
    else if (a->carry_t > 0) {
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
          v->b.pitch += rtr_clamp(-0.349f - v->b.pitch, -6.0f * DT, 6.0f * DT);   /* hung nose down off the forks */
          a->control += 1; v->countout = 0;
          a->carry_t++;
          /* OPS: the saw secondary ticks on the carried bot */
          if (a->is_ops && a->spin2 > 0.5f && a->hit_cd2[vic] == 0) { a->hit_cd2[vic] = WP_SAW_TICK_FRAMES; apply_damage(m, i, vic, WEAPONS[WP_SAW].base, WP_SAW, 6, v->b.x, v->b.z, fx, fz, EV_HIT, 1.0f); }
        } else {
          lift_release(m, i, vic);
          a->carry_t = 0;
        }
      } else a->carry_t = 0;
    }
    /* clamping: the jaws bite through the lid, 3 every 30 frames (6 a second) for the 120 frame hold */
    if (a->build.weapon == WP_CRUSHER && a->lock > 0) {
      for (int j = 0; j < m->nbots; j++) if (m->bots[j].clamped_by == i + 1) {
        bot_t *v = &m->bots[j];
        a->crush_t++; a->control += 1;
        float fx, fz; fwd_of(a->b.h, &fx, &fz);
        v->b.x = a->b.x + fx * (a->b.hl + v->b.hl * 0.9f); v->b.z = a->b.z + fz * (a->b.hl + v->b.hl * 0.9f);
        clamp_inside(m, v);
        v->b.vx = a->b.vx; v->b.vz = a->b.vz; v->b.w = 0;
        v->b.pitch += rtr_clamp(-0.262f - v->b.pitch, -6.0f * DT, 6.0f * DT);   /* hung on the jaw */
        if (a->crush_t % 30 == 0) {
          float d = WP_CRUSHER_HOLD_DPS * 0.5f;
          v->hp -= d; a->dealt += d; m->no_hit_frames = 0;
          float x, z, y; panel_centre(v, PN_TOP, &x, &z, &y);
          ev_full(m, EV_GRIND, i, j, GRIND_TICK, x, z, y, d, fx, fz, PN_TOP, WP_CRUSHER, EVF_TOP, 0, 0, 0);
          wear_panel(m, i, j, PN_TOP, 1.0f);
        }
        if (a->lock == 1) { v->clamped_by = 0; tell(m, i, TELL_CLAMP_RELEASE, WP_CRUSHER); }
        hp_thresholds(m, j);
      }
    }
  }
}

/* ram (4.7): a bot vs bot contact with the attacker's forward within 30 degrees of the normal, for a WEDGE
 * or a bare build; a wheeled or tracked side it cannot get under takes it on the wheel (3.5, 3.9) */
static void ram_check(match_t *m, int i, int j, const contact_t *c) {
  bot_t *a = &m->bots[i], *v = &m->bots[j];
  if (!a->alive || !(v->alive || is_corpse(v)) || a->is_ops || v->is_ops) return;
  if (!(a->build.weapon == WP_WEDGE || a->build.weapon == WP_NONE || a->build.chassis == CH_WEDGE)) return;
  if (a->build.drive == DR_MELTY) return;
  if (a->hit_cd[j] > 0) return;
  float fx, fz; fwd_of(a->b.h, &fx, &fz);
  if (dotp(fx, fz, c->nx, c->nz) < 0.866f) return;
  float closing = dotp(a->b.vx - v->b.vx, a->b.vz - v->b.vz, c->nx, c->nz);
  if (closing <= RAM_MIN_CLOSING) return;
  int w = bot_kind(a) == LIP_CHASSIS || bot_kind(a) == LIP_PLATE ? WP_WEDGE : WP_NONE;
  if (!v->alive) {                                /* a corpse: the shove and the sparks, no damage (DEEP-plan 3) */
    a->hit_cd[j] = 30;
    knock(v, fx * 2.0f, fz * 2.0f);
    ev_full(m, EV_RAM, i, j, 10, c->cx, c->cz, v->b.y + v->b.clr + v->b.hgt * 0.5f, 0, c->nx, c->nz, edge_toward(v, -c->nx, -c->nz), w, EVF_CORPSE, HIT_WHY_NONE, closing, 2.0f);
    return;
  }
  float raw = RAM_BASE + RAM_BASE * rtr_clamp((closing - 2.0f) / 4.0f, 0, 1);
  int flags = 0, reason = 0, edge = edge_toward(v, -c->nx, -c->nz);
  float zone = contact_zone(v, WP_WEDGE, c->cx, c->cz, &flags, &reason), loc = 1.0f;
  int wheel = wheel_station(v, c->cx, c->cz, edge);
  if (wheel) { loc = 1.2f; flags |= EVF_WHEEL; if (reason != HIT_WHY_EXPOSED) reason = HIT_WHY_WHEEL; }
  if (bot_kind(v) == LIP_CHASSIS || bot_kind(v) == LIP_PLATE) { if (edge == PN_FRONT && !v->inverted && reason == HIT_WHY_FRONT) reason = HIT_WHY_WEDGE; }
  float J = 4.0f * (0.5f + raw * zone * loc * (1.0f - ARMOURS[v->build.armour].reduction) / 30.0f);
  hit_t h = { 0 };
  h.att = i; h.vic = j; h.weapon = w; h.raw = raw * zone * loc; h.stun = 15; h.cx = c->cx; h.cz = c->cz; h.cy = v->b.y + v->b.clr + v->b.hgt * 0.5f;
  h.nx = c->nx; h.nz = c->nz; h.ev_type = EV_RAM; h.zone = zone; h.panel = edge; h.flags = flags; h.reason = reason; h.speed = closing; h.j = J; h.wear = 1;
  float dmg = apply_hit(m, &h);
  a->hit_cd[j] = RAM_CD;
  if (dmg <= 0) return;
  if (wheel && dmg >= BAND_HIT_MIN) wheel_hit_effect(m, i, j, edge, dmg, w, c->cx, c->cz);
  knock_at(v, fx * J, fz * J, c->cx, c->cz);
}

/* -- integration, walls and solids (4.4 step 7 and 8) -------------- */
/* the pitch past which an unsupported bot goes over (deep-physics 3.1), radians */
static float tip_angle(const bot_t *b) { return (CHASSIS_TIP_DEG[b->build.chassis] + DRIVE_TIP_ADJ_DEG[b->build.drive]) * (PI_F / 180.0f); }

/* the floor's slope under a body: the pitch (nose up) and roll (left up) a supported body settles to */
static void terrain_slope(const arena_def_t *ar, const body_t *bd, float *pitch, float *roll) {
  float fx, fz, rx, rz; fwd_of(bd->h, &fx, &fz); right_of(bd->h, &rx, &rz);
  float nose = arena_floor_y(ar, bd->x + fx * bd->hl, bd->z + fz * bd->hl), tail = arena_floor_y(ar, bd->x - fx * bd->hl, bd->z - fz * bd->hl);
  float left = arena_floor_y(ar, bd->x + rx * bd->hw, bd->z + rz * bd->hw), right = arena_floor_y(ar, bd->x - rx * bd->hw, bd->z - rz * bd->hw);
  *pitch = rtr_atan2(nose - tail, 2.0f * bd->hl);
  *roll = rtr_atan2(left - right, 2.0f * bd->hw);
}

static void orient_step(match_t *m, int slot) {
  bot_t *b = &m->bots[slot];
  body_t *bd = &b->b;
  if (b->is_ops) { bd->pitch = bd->roll = bd->wp = bd->wr = 0; return; }
  if (b->airborne && !b->ride_by) {
    if (b->tipping) {
      /* going over the wedge's top edge: gravity pulls it back under the tip angle and over past it */
      float th = tip_angle(b), a = fabs_f(bd->pitch), sg = bd->pitch >= 0 ? 1.0f : -1.0f;
      if (a < th) bd->wp -= sg * TIP_G * DT; else if (a < PI_F * 0.5f) bd->wp += sg * TIP_G * DT;
    }
    bd->pitch += bd->wp * DT; bd->roll += bd->wr * DT;
    return;
  }
  if (b->ride_by || b->lifted_by || b->clamped_by) return;     /* posed by the ride or the hold */
  float pt, rt; terrain_slope(sim_arena(m), bd, &pt, &rt);
  bd->pitch += rtr_clamp(pt - bd->pitch, -RELAX_RATE * DT, RELAX_RATE * DT);
  bd->roll += rtr_clamp(rt - bd->roll, -RELAX_RATE * DT, RELAX_RATE * DT);
  bd->wp = bd->wr = 0;
}

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
    if ((fabs_f(b->drive_t) < 0.05f && !b->wheel_fric) || !b->alive) { float k = b->build.drive == DR_HOVER && b->alive && b->skirt_t <= 0 ? 0.985f : 0.90f; bd->vx *= k; bd->vz *= k; }
    if (!b->alive) bd->w *= 0.9f;
    /* the taunt wiggle: yaw plus and minus 15 degrees at 6 Hz */
    bd->x += bd->vx * DT; bd->z += bd->vz * DT;
    bd->h += bd->w * DT;
    bd->h = rtr_wrap_pi(bd->h);
    /* pitch and roll (DEEP-plan 6 step 5): a flier tumbles freely, a wedge tip falls over its edge under
     * gravity's torque, a supported body settles onto the slope under it; a ridden or held body is posed by
     * the ride or the hold below */
    orient_step(m, i);
    /* the height channel; a ridden bot sits on the wedge at the height the ride gives it */
    float fy = arena_floor_y(ar, bd->x, bd->z);
    if (b->ride_by && !b->airborne) { b->floor_y = fy; bd->vy = 0; continue; }
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

static int resolve_static(match_t *m, int slot, const contact_t *c, float e, int is_wall, int sol_kind, int sol_idx) {
  bot_t *b = &m->bots[slot];
  body_t *bd = &b->b;
  float im = bd->inv_mass > 0 ? bd->inv_mass : (b->is_ops ? 1.0f / 6.0f : 2.0f);
  float ii = b->is_ops ? 0 : body_ii(b);
  /* the normal points from the body into the wall; move the body out past the slop */
  float corr = fmax_f(c->pen - SLOP, 0) * 0.8f;
  bd->x -= c->nx * corr; bd->z -= c->nz * corr;
  /* the contact point's velocity: the body's plus the yaw rate's sweep, w x (rz, -rx) in this world */
  float rx = c->cx - bd->x, rz = c->cz - bd->z;
  float vpx = bd->vx + bd->w * rz, vpz = bd->vz - bd->w * rx;
  float vn = dotp(vpx, vpz, c->nx, c->nz);
  if (vn <= 0) return 0;
  float tx = -c->nz, tz = c->nx;
  float rn = rz * c->nx - rx * c->nz, rt = rz * tx - rx * tz;
  float j = (1.0f + e) * vn / (im + rn * rn * ii);
  float vt = dotp(vpx, vpz, tx, tz);
  float jt = rtr_clamp(vt / (im + rt * rt * ii), -MU_CONTACT * j, MU_CONTACT * j);
  float Jx = -(c->nx * j + tx * jt), Jz = -(c->nz * j + tz * jt);
  bd->vx += Jx * im; bd->vz += Jz * im;
  bd->w += (rz * Jx - rx * Jz) * ii;                    /* the torque of the impulse about the centre (full, DEEP-plan 6 step 5) */
  /* a bot sliding along the lexan: walls_step counts the frames and fires EV_SCRAPE */
  int scraped = is_wall && b->alive && fabs_f(vt) > SCRAPE_SPEED;
  /* wall hit events (4.5) */
  if (vn > 2.0f && b->alive && !b->is_ops) {
    int ev = EV_WALL;
    float s0 = b->spin;
    if (b->build.weapon == WP_HORIZ && b->spin > 0.5f) {
      b->spin = 0; b->hp -= WP_HORIZ_WALL_DMG; ev = EV_SPIN_WALL; hp_thresholds(m, slot);
    } else if (b->build.weapon == WP_DRUM) { b->spin *= 0.70710678f; }        /* the energy halves (3.6) */
    else if (b->build.drive == DR_MELTY && b->spin > 0.5f) { b->hp -= WP_MELTY_WALL_DMG; hp_thresholds(m, slot); }
    if (b->build.weapon == WP_HORIZ || b->build.weapon == WP_DRUM) spin_lost(m, slot, b->build.weapon, s0, b->spin, SPIN_LOST_WALL);
    if (vn > 4.0f && ev == EV_WALL) { b->hp -= 2.0f; hp_thresholds(m, slot); }   /* 2 HP over 4 u/s (5.2) */
    b->wall_hits++;
    if (ev == EV_SPIN_WALL) ev_full(m, EV_SPIN_WALL, slot, slot, 0, c->cx, c->cz, bd->y + bd->clr + bd->hgt * 0.5f, WP_HORIZ_WALL_DMG, -c->nx, -c->nz, PN_FRONT, WP_HORIZ, 0, HIT_WHY_WALL, vn, j);
    else ev_full(m, EV_WALL, slot, slot, 0, c->cx, c->cz, bd->y + 0.2f, vn > 4.0f ? 2.0f : 0, -c->nx, -c->nz, 0, EVW_WALL, 0, 0, vn, j);
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
  return scraped ? 1 : 0;
}

static int in_ops_zone(const arena_def_t *a, float x, float z, float grow) {
  return fabs_f(x - a->ops_zone[0]) <= 1.0f + grow && fabs_f(z - a->ops_zone[1]) <= 1.0f + grow;
}

static void walls_step(match_t *m) {
  const arena_def_t *ar = sim_arena(m);
  solid_t sol[MAX_SOLIDS]; int ns = gather_solids(m, sol);
  float hw = ar->w * 0.5f, hd = ar->d * 0.5f;
  uint8_t scraped[MAX_BOTS] = { 0 }; contact_t scrape_c[MAX_BOTS]; memset(scrape_c, 0, sizeof scrape_c);
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
        if (resolve_static(m, i, &c, E_WALLS, 1, 0, 0)) { scraped[i] = 1; scrape_c[i] = c; }
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
          resolve_static(m, i, &c, E_WALLS, 0, SOL_BUMPER, sol[k].idx);
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
        resolve_static(m, i, &c, E_WALLS, 0, sol[k].kind, sol[k].idx);
        shape_of(b, &s); touched = 1;
      }
      if (it == 0) b->pinned = (uint8_t)(touched ? (b->pinned | 1) : (b->pinned & ~1));
    }
  }
  /* six frames sliding along the lexan over 1.5 u/s is a scrape (once per 30) */
  for (int i = 0; i < m->nbots; i++) {
    bot_t *b = &m->bots[i];
    if (scraped[i]) {
      if (++b->scrape_t == 6 && b->scrape_cd == 0) {
        float sp = len2(b->b.vx, b->b.vz);
        ev_full(m, EV_SCRAPE, i, i, SCRAPE_WALL, scrape_c[i].cx, scrape_c[i].cz, b->b.y + 0.1f, sp, -scrape_c[i].nx, -scrape_c[i].nz, 0, EVW_WALL, 0, 0, sp, 0);
        b->scrape_cd = 30;
      }
    } else if (!b->inverted) b->scrape_t = 0;
  }
}

/* -- the wedge (DEEP-plan 6 step 6, deep-physics 3.5) ---------------
 * Every contact between two live bots is classified once per step from the closing speed before it is
 * resolved: PLAIN (resolved as a contact, the old ram test), UNDERRIDE (a lip got under the other's edge:
 * the pair now overlaps in plan by how far the slope is under the victim, which lifts and pitches it and
 * takes its grip), or WEDGE_CLASH (two lips met evenly and bind). A ride ends by slipping off sideways,
 * backing off, being carried off a drop, OPS, or the tip: a victim that reaches the top of the slope at a
 * closing speed over its chassis' v_tip goes over the top edge. No random draw anywhere in here. */
#define RIDE_BLOCKED 9.0f          /* a clearance no lip gets under: wheels or tracks at that side, a lid */
#define COS35 0.8191520f
#define COS50 0.6427876f
static void pair_impulse(bot_t *a, bot_t *b, const contact_t *c, float e, float mu);

/* a bot's lip at the edge a contact touches, 0 for none: only an upright front edge with a wedge has one */
static float lip_at(const bot_t *b, int edge) {
  if (edge != PN_FRONT || b->inverted || !b->alive || b->is_ops || b->airborne || b->riding || b->ride_by || b->lifted_by || b->clamped_by) return 0;
  return b->b.lip_f;
}
/* how high a lip must be under to get in at a victim's edge; RIDE_BLOCKED where nothing gets under */
static float clr_at(const bot_t *v, int edge) {
  if (!v->alive || v->is_ops || v->inverted || v->airborne || v->ride_by || v->riding || v->lifted_by || v->clamped_by || v->falling) return RIDE_BLOCKED;
  int d = v->build.drive;
  if (d == DR_HOVER) return HOVER_CLEAR_SOFT;                         /* the skirt is soft at every edge */
  if (d == DR_MELTY) return v->spin > 0.5f ? RIDE_BLOCKED : v->b.clr;   /* a spinning edge never sits still */
  if (v->build.chassis == CH_SHELL) return v->b.clr * 0.5f;           /* the dome's rim sits low all round */
  if ((edge == PN_LEFT || edge == PN_RIGHT) && (d == DR_TWO || d == DR_FOUR || d == DR_SIX || d == DR_TREAD || d == DR_WALKER || d == DR_LEGTRACK)) return RIDE_BLOCKED;
  return v->b.clr;
}
/* the closing speed at which a ride over the top tips this victim over (deep-physics 3.5) */
static float v_tip_of(const bot_t *v) {
  if (v->build.drive == DR_HOVER) return V_TIP_NEVER;
  return CHASSIS_VTIP[v->build.chassis] + DRIVE_VTIP_ADJ[v->build.drive];
}

static void ride_start(match_t *m, int a, int v, int edge, int kind, float cl, float cx, float cz) {
  bot_t *A = &m->bots[a], *V = &m->bots[v];
  V->ride_by = (uint8_t)(a + 1); V->ride_edge = (uint8_t)edge; V->ride_t = 0; V->ride_up = 0; V->ride_s = 0; V->ride_cl_peak = cl;
  V->ride_full = 0; V->carry_ride_t = 0; V->ride_kind = (uint8_t)kind; V->ride_lift = 0;
  A->riding = (uint8_t)(v + 1);
  int w = A->build.weapon == WP_LIFTER && kind == LIP_FORKS ? WP_LIFTER : WP_WEDGE;
  ev_full(m, EV_UNDERRIDE, a, v, edge, cx, cz, V->b.y + 0.05f, cl, 0, 0, edge, w, 0, 0, cl, 0);
  /* the ram lands once, on entry, as today (9 + 9 by closing speed, stun 10, RAM_CD), for a ramming build (a
   * WEDGE chassis or plate, or a bare front); nothing per frame. A LOWBOY nose or a lifter's forks get under
   * without the hit */
  int rammer = A->build.weapon == WP_WEDGE || A->build.weapon == WP_NONE || A->build.chassis == CH_WEDGE;
  if (rammer && cl > RAM_MIN_CLOSING && A->hit_cd[v] == 0) {
    float nx, nz; edge_normal(V, edge, &nx, &nz);
    float raw = RAM_BASE + RAM_BASE * rtr_clamp((cl - 2.0f) / 4.0f, 0, 1);
    hit_t h = { 0 };
    h.att = a; h.vic = v; h.weapon = WP_WEDGE; h.raw = raw; h.stun = 10; h.cx = cx; h.cz = cz; h.cy = V->b.y + V->b.clr;
    h.nx = -nx; h.nz = -nz; h.ev_type = EV_RAM; h.zone = 1.0f; h.panel = edge; h.speed = cl; h.j = 0; h.wear = 1;
    apply_hit(m, &h);
    A->hit_cd[v] = RAM_CD;
  }
}

/* the forks under a ridden bot lift it: the ride becomes the hold with no event of its own (EV_LIFT follows) */
static void ride_clear(match_t *m, int v) {
  bot_t *V = &m->bots[v];
  if (!V->ride_by) return;
  bot_t *A = &m->bots[V->ride_by - 1];
  if (A->riding == v + 1) A->riding = 0;
  V->ride_by = 0; V->ride_up = 0; V->ride_s = 0; V->ride_full = 0; V->carry_ride_t = 0; V->ride_lift = 0;
}

static void ride_end(match_t *m, int v, int reason) {
  bot_t *V = &m->bots[v];
  if (!V->ride_by) return;
  int a = V->ride_by - 1; bot_t *A = &m->bots[a];
  float lift = V->ride_lift;
  ev_full(m, EV_RIDE_END, a, v, reason, V->b.x, V->b.z, V->b.y, 0, 0, 0, V->ride_edge, WP_WEDGE, 0, 0, 0, lift);
  V->ride_by = 0; V->ride_up = 0; V->ride_s = 0; V->ride_full = 0; V->carry_ride_t = 0; V->ride_lift = 0;
  if (A->riding == v + 1) A->riding = 0;
  A->ride_cd[v] = 30; V->ride_cd[a] = 30;              /* no instant re-entry, and the pair parts gently (bots_collide) */
  /* off the slope: what was lifted drops onto its wheels (a landing with no damage) */
  if (reason != RIDE_TIPPED && !V->lifted_by && !V->falling && V->b.y > V->floor_y + 0.02f) { V->airborne = 1; V->b.vy = 0; V->peak_y = V->b.y; }
}

/* the tip (deep-physics 3.5): over the top edge, nose first, with the closing speed's surplus as height */
static void ride_tip(match_t *m, int v) {
  bot_t *V = &m->bots[v];
  int a = V->ride_by - 1, edge = V->ride_edge;
  float surplus = V->ride_cl_peak - v_tip_of(V);
  /* it leaves the plate going the wedge's way: at least the wedge's speed along the normal, plus the turn
   * over the top edge carrying its centre on past the edge */
  bot_t *A = &m->bots[a];
  float nx, nz; edge_normal(V, edge, &nx, &nz); nx = -nx; nz = -nz;
  float va = dotp(A->b.vx, A->b.vz, nx, nz), vv = dotp(V->b.vx, V->b.vz, nx, nz), want = va + 1.0f;
  if (vv < want) { V->b.vx += nx * (want - vv); V->b.vz += nz * (want - vv); }
  ride_end(m, v, RIDE_TIPPED);
  float vy = 1.2f + 0.3f * surplus, w = 3.5f;
  V->airborne = 1; V->b.vy = vy; V->peak_y = V->b.y; V->tipping = 1; V->flip_pending = 1; V->launched_by = (uint8_t)(a + 1);
  if (edge == PN_FRONT) V->b.wp = w; else if (edge == PN_BACK) V->b.wp = -w;
  else if (edge == PN_RIGHT) V->b.wr = w; else V->b.wr = -w;
  ev_full(m, EV_LAUNCH, a, v, LAUNCH_WEDGE, V->b.x, V->b.z, V->b.y, vy, 0, 0, edge, WP_WEDGE, 0, 0, vy, w);
}


/* one ridden victim, once per step before the pair contacts: the overlap along the ridden edge's normal is
 * how far the slope is under it (ride_s), which sets the lift, the pose and the load; past the slope's
 * length the excess is a contact (the carry) */
static void ride_step(match_t *m, int v) {
  bot_t *V = &m->bots[v];
  int a = V->ride_by - 1;
  if (a < 0 || a >= m->nbots) { V->ride_by = 0; return; }
  bot_t *A = &m->bots[a];
  if (V->lifted_by) { ride_end(m, v, V->lifted_by == OPS_SLOT + 1 ? RIDE_OPS : RIDE_BACKED_OFF); return; }
  if (V->falling || V->pit_state == 2) { ride_end(m, v, RIDE_CARRIED_OFF); return; }
  if (!V->alive || !A->alive || A->airborne || A->inverted || A->lifted_by || A->clamped_by || V->clamped_by || V->airborne) { ride_end(m, v, RIDE_SLIP); return; }
  int kind = V->ride_kind < LIP_COUNT ? V->ride_kind : LIP_PLATE;
  float S = WEDGE_KIND[kind].len;
  float en_x, en_z; edge_normal(V, V->ride_edge, &en_x, &en_z);
  float nx = -en_x, nz = -en_z, tx = -nz, tz = nx;                  /* n from the attacker into the victim */
  float dx = V->b.x - A->b.x, dz = V->b.z - A->b.z;
  float d = body_extent(A, nx, nz) + body_extent(V, nx, nz) - dotp(dx, dz, nx, nz);
  float ov_t = body_extent(A, tx, tz) + body_extent(V, tx, tz) - fabs_f(dotp(dx, dz, tx, tz));
  float afx, afz; fwd_of(A->b.h, &afx, &afz);
  if (ov_t <= 0.05f || dotp(afx, afz, nx, nz) < COS50) { ride_end(m, v, RIDE_SLIP); return; }
  if (d <= 0) { ride_end(m, v, RIDE_BACKED_OFF); return; }
  float cl = dotp(A->b.vx - V->b.vx, A->b.vz - V->b.vz, nx, nz);
  if (cl > V->ride_cl_peak) V->ride_cl_peak = cl;
  V->ride_t++;
  float lat_side = dotp(dx, dz, tx, tz) >= 0 ? 1.0f : -1.0f;
  if (!V->ride_full) {
    V->ride_s = d < S ? d : S;
    V->ride_up = V->ride_s / S;
    /* a SHELL's dome rolls off sideways half way up; it never tips */
    if (V->build.chassis == CH_SHELL && V->ride_up >= 0.5f) { V->b.vx += tx * lat_side * 2.0f; V->b.vz += tz * lat_side * 2.0f; ride_end(m, v, RIDE_SLIP); return; }
    if (V->ride_up >= 1.0f) {
      /* the top of the slope: tip it over, or carry it (a pancake nose or forks lift too little to tip) */
      if ((kind == LIP_CHASSIS || kind == LIP_PLATE) && V->ride_cl_peak >= v_tip_of(V)) { V->ride_lift = WEDGE_KIND[kind].lift; ride_tip(m, v); return; }
      V->ride_full = 1; V->carry_ride_t = 0;
    }
  } else { V->ride_s = S; V->ride_up = 1.0f; }
  if (V->ride_full) {
    /* carried on the plate: 90 frames, or the wedge stops, and it slides off the nearer side */
    V->carry_ride_t++;
    if (V->carry_ride_t > 90 || len2(A->b.vx, A->b.vz) < 0.5f) {
      V->b.vx += tx * lat_side * 1.5f; V->b.vz += tz * lat_side * 1.5f;
      ride_end(m, v, RIDE_SLIP); return;
    }
  }
  /* past the slope's length the bodies meet: resolve the excess as a contact (low bounce, the plate's friction) */
  if (d > S) {
    float corr = (d - S) * 0.8f, ima = body_im(A), imv = body_im(V), ims = ima + imv;
    if (ims > 0) { A->b.x -= nx * corr * ima / ims; A->b.z -= nz * corr * ima / ims; V->b.x += nx * corr * imv / ims; V->b.z += nz * corr * imv / ims; }
    float lip_x = A->b.x + nx * body_extent(A, nx, nz), lip_z = A->b.z + nz * body_extent(A, nx, nz);
    contact_t c = { nx, nz, d - S, lip_x, lip_z };
    pair_impulse(A, V, &c, E_RIDE, MU_RIDE);
  }
  /* the pose: the edge is lifted up the slope, the centre half of that, the body pitched (or rolled) toward the attacker */
  float lift = V->ride_up * WEDGE_KIND[kind].lift;
  V->ride_lift = lift;
  float fy = arena_floor_y(sim_arena(m), V->b.x, V->b.z);
  V->floor_y = fy; V->b.y = fy + lift * 0.5f; V->b.vy = 0;
  float half = V->ride_edge == PN_FRONT || V->ride_edge == PN_BACK ? V->b.hl : (V->b.shape == SH_CIRCLE ? V->b.hl : V->b.hw);
  float ang = rtr_atan2(lift, sqrtf(fmax_f(4.0f * half * half - lift * lift, 1e-4f)));
  float want_p = 0, want_r = 0;
  if (V->ride_edge == PN_FRONT) want_p = ang; else if (V->ride_edge == PN_BACK) want_p = -ang;
  else if (V->ride_edge == PN_RIGHT) want_r = ang; else want_r = -ang;
  V->b.pitch += rtr_clamp(want_p - V->b.pitch, -12.0f * DT, 12.0f * DT);
  V->b.roll += rtr_clamp(want_r - V->b.roll, -12.0f * DT, 12.0f * DT);
  V->b.wp = V->b.wr = 0;
}

/* classify a fresh contact between two live bots (i < j, normal from i to j): 0 PLAIN, 1 UNDERRIDE (begun),
 * 2 WEDGE_CLASH. Run once per pair per step, before the contact is resolved */
static int wedge_classify(match_t *m, int i, int j, const contact_t *c) {
  bot_t *P = &m->bots[i], *Q = &m->bots[j];
  if (!P->alive || !Q->alive || P->is_ops || Q->is_ops) return 0;
  if (P->ride_cd[j] > 0 || Q->ride_cd[i] > 0) return 0;
  int ep = edge_toward(P, c->nx, c->nz), eq = edge_toward(Q, -c->nx, -c->nz);
  float lp = lip_at(P, ep), lq = lip_at(Q, eq);
  if (lp <= 0 && lq <= 0) return 0;
  for (int dir = 0; dir < 2; dir++) {
    int a = dir ? j : i, v = dir ? i : j;
    bot_t *A = &m->bots[a], *V = &m->bots[v];
    float nx = dir ? -c->nx : c->nx, nz = dir ? -c->nz : c->nz;
    int ea = dir ? eq : ep, ev = dir ? ep : eq;
    float lip_a = dir ? lq : lp, lip_v = dir ? lp : lq;
    if (lip_a <= 0) continue;
    float cl = dotp(A->b.vx - V->b.vx, A->b.vz - V->b.vz, nx, nz);
    float afx, afz; fwd_of(A->b.h, &afx, &afz);
    int kind = bot_kind(A);
    float min_cl = kind == LIP_NOSE ? RAM_MIN_CLOSING : 0.5f;         /* a pancake nose gets under only at speed */
    int pushing = A->dig_t > 0 && A->drive_t > 0.2f;                  /* DIG IN: the plate scrapes in from rest */
    if (!(cl >= min_cl || pushing) || dotp(afx, afz, nx, nz) < COS35) continue;
    (void)ea;
    if (lip_v > 0) {
      /* wedge on wedge: the lower lip gets under; even lips go to the faster one by 0.8 u/s, else they bind */
      if (lip_a + 0.01f < lip_v) { ride_start(m, a, v, ev, kind, cl, c->cx, c->cz); return 1; }
      if (lip_v + 0.01f < lip_a) { ride_start(m, v, a, ea, bot_kind(V), cl, c->cx, c->cz); return 1; }
      float sa = dotp(A->b.vx, A->b.vz, nx, nz), sv = -dotp(V->b.vx, V->b.vz, nx, nz);
      if (sa > sv + 0.8f) { ride_start(m, a, v, ev, kind, cl, c->cx, c->cz); return 1; }
      if (sv > sa + 0.8f) { ride_start(m, v, a, ea, bot_kind(V), cl, c->cx, c->cz); return 1; }
      ev_full(m, EV_WEDGE_CLASH, a, v, 0, c->cx, c->cz, A->b.y + 0.05f, cl, nx, nz, PN_FRONT, WP_WEDGE, 0, 0, cl, 0);
      A->bind_t = V->bind_t = 20; A->ride_cd[v] = V->ride_cd[a] = 60;
      return 2;
    }
    float clr_v = clr_at(V, ev);
    if (clr_v >= RIDE_BLOCKED) continue;                              /* a wheel or a track there: the ram test */
    if (lip_a < clr_v) { ride_start(m, a, v, ev, kind, cl, c->cx, c->cz); return 1; }
  }
  return 0;
}

/* -- bot vs bot collision and push (4.5) --------------------------- */
/* the impulse between two bodies at a contact (DEEP-plan 6 step 5): restitution e, Coulomb friction mu, the
 * full angular term in both directions (the yaw rate's sweep at the contact point, w x r = w (rz, -rx), and
 * the torque rz Jx - rx Jz), so a corner hit spins a bot the right way */
static void pair_impulse(bot_t *a, bot_t *b, const contact_t *c, float e, float mu) {
  float ima = body_im(a), imb = body_im(b), iia = body_ii(a), iib = body_ii(b);
  if (ima + imb <= 0) return;
  float rax = c->cx - a->b.x, raz = c->cz - a->b.z, rbx = c->cx - b->b.x, rbz = c->cz - b->b.z;
  float vax = a->b.vx + a->b.w * raz, vaz = a->b.vz - a->b.w * rax;
  float vbx = b->b.vx + b->b.w * rbz, vbz = b->b.vz - b->b.w * rbx;
  float rvx = vbx - vax, rvz = vbz - vaz;
  float vn = dotp(rvx, rvz, c->nx, c->nz);
  if (vn >= 0) return;
  float tx = -c->nz, tz = c->nx;
  float ran = raz * c->nx - rax * c->nz, rbn = rbz * c->nx - rbx * c->nz, rat = raz * tx - rax * tz, rbt = rbz * tx - rbx * tz;
  float jn = -(1.0f + e) * vn / (ima + imb + ran * ran * iia + rbn * rbn * iib);
  float vt = dotp(rvx, rvz, tx, tz);
  float jt = rtr_clamp(-vt / (ima + imb + rat * rat * iia + rbt * rbt * iib), -mu * jn, mu * jn);
  float Jx = c->nx * jn + tx * jt, Jz = c->nz * jn + tz * jt;
  a->b.vx -= Jx * ima; a->b.vz -= Jz * ima;
  b->b.vx += Jx * imb; b->b.vz += Jz * imb;
  a->b.w -= (raz * Jx - rax * Jz) * iia;
  b->b.w += (rbz * Jx - rbx * Jz) * iib;
}

static void bots_collide(match_t *m) {
  /* the lips this step: the kind's lip, a hundredth lower while DIG IN scrapes; a lifter's forks only while down */
  for (int i = 0; i < m->nbots; i++) { bot_t *b = &m->bots[i]; int k = bot_kind(b); b->b.lip_f = k >= 0 ? WEDGE_KIND[k].lip - (b->dig_t > 0 ? 0.01f : 0) : 0; }
  for (int i = 0; i < m->nbots; i++) m->bots[i].touch_bot = 0;
  /* the rides first: a ridden pair overlaps on purpose and is not a plain contact this step */
  for (int v = 0; v < m->nbots; v++) if (m->bots[v].ride_by) ride_step(m, v);
  for (int it = 0; it < 2; it++) {
    for (int i = 0; i < m->nbots; i++) {
      bot_t *a = &m->bots[i];
      if (a->hp_max == 0 || a->lifted_by || a->clamped_by || a->falling || a->pit_state == 2) continue;
      if (!a->alive && (a->ko_how == KO_PIT || a->ko_how == KO_ROOF)) continue;
      for (int j = i + 1; j < m->nbots; j++) {
        bot_t *b = &m->bots[j];
        if (b->hp_max == 0 || b->lifted_by || b->clamped_by || b->falling || b->pit_state == 2) continue;
        if (!b->alive && (b->ko_how == KO_PIT || b->ko_how == KO_ROOF)) continue;
        if (a->ride_by == j + 1 || b->ride_by == i + 1) continue;   /* a ride: ride_step owns this pair */
        if (!same_band(a, b)) continue;                       /* one is above the other */
        shape_t sa, sb; shape_of(a, &sa); shape_of(b, &sb);
        contact_t c; if (!collide(&sa, &sb, &c)) continue;
        a->touch_bot = b->touch_bot = 1;
        float ima = body_im(a), imb = body_im(b);               /* a dead chassis is a 0.5 mass prop, OPS immovable */
        float ims = ima + imb;
        if (ims <= 0) continue;
        /* the wedge test reads the closing speed as the bots met: an underride begins (and the pair is the
         * ride's from here), two lips bind, or it is a plain contact and the ram test runs, before the hover
         * rule below bleeds the speed off (a wedge that drives into a HOVER rams it like anything else) */
        int cls = 0;
        if (it == 0) {
          cls = wedge_classify(m, i, j, &c);
          if (cls == 1) continue;
          if (cls == 0) ram_check(m, i, j, &c), ram_check(m, j, i, &(contact_t){ -c.nx, -c.nz, c.pen, c.cx, c.cz });
        }
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
        float corr = fmax_f(c.pen - SLOP, 0) * 0.8f;
        /* a pair just off a ride still overlaps where the slope was: it slides apart over a few frames
         * instead of being popped apart in one */
        int parting = a->ride_cd[j] > 0 || b->ride_cd[i] > 0;
        if (parting && corr > 0.04f) corr = 0.04f;
        a->b.x -= c.nx * corr * ka; a->b.z -= c.nz * corr * ka;
        b->b.x += c.nx * corr * kb; b->b.z += c.nz * corr * kb;
        /* OPS never pushes a bot through the lexan: what the wall will not take, OPS gives back */
        if (a->is_ops != b->is_ops) {
          bot_t *o = a->is_ops ? a : b, *v = a->is_ops ? b : a;
          float vx0 = v->b.x, vz0 = v->b.z; clamp_inside(m, v);
          o->b.x += v->b.x - vx0; o->b.z += v->b.z - vz0;
        }
        pair_impulse(a, b, &c, cls == 2 ? 0.3f : ha || hb || parting ? 0.0f : E_BOTS, parting ? MU_RIDE : MU_CONTACT);
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
            /* a bot with its wheels up pushes with what is left on the floor (load): the wedge wins the shove */
            float pa = a->is_ops ? 2.0f * 6.0f : build_push(&a->build) * a->load, pb = b->is_ops ? 2.0f * 6.0f : build_push(&b->build) * b->load;
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

/* -- support (DEEP-plan 6 step 5 and 6): what share of each bot's grip is on the floor for the next step --
 * A ridden bot loses DRIVE_RIDE_LOSS x ride_up of it (a HOVER all of it), the wedge under it gains 0.2 x
 * ride_up (the victim's weight on its plate), two binding plates hold 0.6 for 20 frames, a WALKER limps
 * at 0.7 for 120 frames after a leg hit */
static void support_step(match_t *m) {
  for (int i = 0; i < m->nbots; i++) {
    bot_t *b = &m->bots[i];
    float load = 1.0f;
    if (b->ride_by) load = b->build.drive == DR_HOVER ? 0.0f : 1.0f - DRIVE_RIDE_LOSS[b->build.drive] * (b->ride_full ? 1.0f : b->ride_up);
    if (b->riding && b->riding - 1 < m->nbots) load = 1.0f + 0.2f * m->bots[b->riding - 1].ride_up;
    if (b->bind_t > 0) load *= 0.6f;
    if (b->limp_t > 0) load *= 0.7f;
    b->load = load < 0 ? 0 : load;
  }
}

/* -- landing (4.8, deep-physics 3.8) -------------------------------
 * The orientation decides, not a dice roll: under 60 degrees off upright the bot lands on its wheels, over
 * 120 it lands on its lid, in between it lands on an edge and bounces once more and the next landing goes
 * whichever side of 90 it is on. A SHELL's dome rolls back up from anything under 150. HOVER and MELTY are
 * set right by the skirt or the frame. The axis is whichever the bot is turning about (pitch, or roll for a
 * side flip). Landing on another bot is 8 to it on its TOP, the lander slides off nose down and stays as it was */
static void landing(match_t *m, int slot) {
  bot_t *b = &m->bots[slot];
  body_t *bd = &b->b;
  float impact = fabs_f(bd->vy);
  int from_flip = b->flip_pending;
  float dmg = 0;
  if (b->soft_land) dmg = 0;                             /* put down off the forks */
  else if (from_flip) dmg = b->bounced ? 3.0f * rtr_clamp(impact / 7.0f, 0, 1.5f) : 2.0f + 3.0f * rtr_clamp(impact / 7.0f, 0, 1.5f);
  else if (b->peak_y - b->floor_y > 1.2f || b->peak_y >= 0.99f) dmg = 8;
  int launcher = b->launched_by ? b->launched_by - 1 : -1;
  int on_bot = 0;
  for (int j = 0; j < m->nbots; j++) {
    if (j == slot) continue;
    bot_t *o = &m->bots[j];
    if (!o->alive || o->airborne) continue;
    if (b->tipping && j == launcher) continue;          /* tipped off a wedge: it comes down on the slope, not on the bot */
    if (b->lift_from == j + 1) continue;                 /* off a lifter's forks: it comes down past them, not on the lifter */
    /* only a real fall lands on a bot: a flip, or a drop of half a unit; a spinner's hop up a wedge plate or
     * a ramp lip settles back onto it */
    if (!from_flip && b->peak_y - b->floor_y < 0.5f) continue;
    shape_t sa, sb; shape_of(b, &sa); shape_of(o, &sb);
    contact_t c;
    if (collide(&sa, &sb, &c)) {
      hit_t h = { 0 };
      h.att = slot; h.vic = j; h.weapon = EVW_LAND; h.raw = 8.0f; h.stun = 6; h.cx = c.cx; h.cz = c.cz; h.cy = o->b.y + o->b.clr + o->b.hgt;
      h.nx = c.nx; h.nz = c.nz; h.ev_type = EV_HIT; h.zone = 1.0f; h.panel = PN_TOP; h.flags = EVF_TOP; h.speed = impact; h.wear = 1;
      apply_hit(m, &h);
      float fx, fz; fwd_of(bd->h, &fx, &fz);
      bd->vx -= fx * 2.0f; bd->vz -= fz * 2.0f;
      bd->x -= c.nx * 0.5f; bd->z -= c.nz * 0.5f;
      on_bot = 1;
      break;
    }
  }
  int was_inverted = b->inverted, zone = LAND_WHEELS;
  int use_roll = fabs_f(bd->roll) > fabs_f(bd->pitch);
  float ang = use_roll ? bd->roll : bd->pitch;
  float o = rtr_wrap_pi(ang + (b->inverted ? PI_F : 0)), ao = fabs_f(o);
  const float D60 = PI_F / 3.0f, D90 = PI_F * 0.5f, D120 = PI_F * 2.0f / 3.0f, D150 = PI_F * 5.0f / 6.0f;
  int never = b->is_ops || b->build.drive == DR_HOVER || b->build.drive == DR_MELTY;
  int shell = b->build.chassis == CH_SHELL;
  int inv = b->inverted, rolled_up = 0;
  if (never) { inv = 0; o = 0; bd->pitch = bd->roll = 0; }
  else if (on_bot) { bd->pitch = -0.5236f; bd->roll = 0; bd->wp = bd->wr = 0; zone = LAND_ON_BOT; }
  else if (b->bounced) {
    /* the second landing of an edge bounce decides by which side of 90 degrees it came down */
    inv = shell ? ao >= D150 : ao >= D90;
    if (shell && !inv && ao >= D90) rolled_up = 1;
  } else if (ao < D60) inv = 0;
  else if (ao > (shell ? D150 : D120)) inv = 1;
  else if (shell && ao > D120) { inv = 0; rolled_up = 1; }
  else if (b->alive) {
    /* on an edge: the fall's damage lands now, then one more bounce with the tumble at 0.6x; the landing
     * after it decides the face and adds only that small fall */
    float adj = dmg * (1.0f - ARMOURS[b->build.armour].reduction);
    if (adj > 0) {
      b->hp -= adj;
      if (b->launched_by && from_flip) { bot_t *a = &m->bots[b->launched_by - 1]; a->dealt += adj; if (adj > a->biggest) a->biggest = adj; }
      hp_thresholds(m, slot);
    }
    ev_full(m, EV_LAND, launcher >= 0 ? launcher : slot, slot, LAND_BOUNCE, bd->x, bd->z, bd->y, adj, 0, 1, 0, EVW_LAND, 0, 0, impact, 0);
    b->bounced = 1; bd->vy = 1.6f; bd->wp *= 0.6f; bd->wr *= 0.6f;
    return;
  }
  if (!never && !on_bot) {
    /* the residual off the face it landed on, which then settles to the floor */
    float res = inv ? rtr_wrap_pi(o - (o >= 0 ? PI_F : -PI_F)) : o;
    if (use_roll) { bd->roll = res; } else { bd->pitch = res; }
    bd->wp = bd->wr = 0;
    b->inverted = (uint8_t)inv;
    if (inv) zone = LAND_INVERTED;
  }
  if (rolled_up) ev_simple(m, EV_SELFRIGHT, slot, slot, 1, bd->x, bd->z, bd->y);
  if (b->inverted && !was_inverted) {
    b->flips++;
    b->inverted_by = b->launched_by;
    if (b->launched_by) { m->bots[b->launched_by - 1].flips_landed++; }
    int cause = b->tipping ? INV_WEDGE : b->launched_by ? INV_FLIPPER : INV_HAZARD;
    ev_simple(m, EV_INVERT, slot, b->launched_by ? b->launched_by - 1 : slot, cause, bd->x, bd->z, bd->y);
    if (b->build.weapon == WP_SAW) b->saw_on = 0;
  } else if (!b->inverted && was_inverted) {
    /* thrown back over onto its wheels */
    b->countout = 0;
    ev_simple(m, EV_SELFRIGHT, slot, slot, 0, bd->x, bd->z, bd->y);
  }
  if (dmg > 0 && b->alive) {
    float adj = dmg * (1.0f - ARMOURS[b->build.armour].reduction);
    b->hp -= adj; if (b->is_ops && b->hp < 1) b->hp = 1;
    if (b->launched_by && from_flip) { bot_t *a = &m->bots[b->launched_by - 1]; a->dealt += adj; if (adj > a->biggest) a->biggest = adj; }
    ev_full(m, EV_LAND, launcher >= 0 ? launcher : slot, slot, zone, bd->x, bd->z, bd->y, adj, 0, 1, b->inverted ? PN_TOP : 0, EVW_LAND, b->inverted ? EVF_TOP : 0, 0, impact, 0);
    if (b->inverted) wear_panel(m, launcher, slot, PN_TOP, adj);
    hp_thresholds(m, slot);
  } else ev_full(m, EV_LAND, launcher >= 0 ? launcher : slot, slot, zone, bd->x, bd->z, bd->y, 0, 0, 1, b->inverted ? PN_TOP : 0, EVW_LAND, 0, 0, impact, 0);
  bd->vy = 0; bd->vx *= 0.6f; bd->vz *= 0.6f;
  b->airborne = 0; b->flip_pending = 0; b->launched_by = 0; b->peak_y = bd->y; b->bounced = 0; b->tipping = 0;
  b->lift_from = 0; b->soft_land = 0;
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
            if (b->ride_by) ride_end(m, j, RIDE_CARRIED_OFF);
            ev_full(m, EV_HAZ_HIT, j, i, KO_PIT, b->b.x, b->b.z, b->b.y, 0, 0, 0, PN_TOP, EVW_HAZARD, 0, 0, 0, 0);
            ev_full(m, EV_FALL, j, j, FALL_PIT, b->b.x, b->b.z, b->b.y, 0, 0, 0, 0, 0, 0, 0, 0, 0);
          } else if (haz_contains(h, b->b.x, b->b.z, 0.3f + b->b.hw * 0.5f) && DRIVES[b->build.drive].traction <= 0.7f && b->build.drive != DR_WALKER) {
            b->pit_state = 1; b->pit_t++;
            float dx = h->x - b->b.x, dz = h->z - b->b.z, d = len2(dx, dz);
            if (d > 0.01f) { float k = 1.2f * DT * (float)b->pit_t / 90.0f * 4.0f; b->b.vx += dx / d * k; b->b.vz += dz / d * k; }
            if (b->pit_t >= 90) {
              b->pit_state = 2; b->pit_t = 0;
              ev_full(m, EV_HAZ_HIT, j, i, KO_PIT, b->b.x, b->b.z, b->b.y, 0, 0, 0, PN_TOP, EVW_HAZARD, 0, 0, 0, 0);
              ev_full(m, EV_FALL, j, j, FALL_PIT, b->b.x, b->b.z, b->b.y, 0, 0, 0, 0, 0, 0, 0, 0, 0);
            }
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
              if (sp > 4.0f && dotp(b->b.vx, b->b.vz, sx, sz) > 0 && b->b.y >= h->h - 0.05f) {
                b->b.vy = sp * 0.8f; b->airborne = 1; b->flip_pending = 1; b->launched_by = 0; b->peak_y = b->b.y; b->bounced = 0; b->tipping = 0;
                ev_full(m, EV_LAUNCH, j, j, LAUNCH_RAMP, b->b.x, b->b.z, b->b.y, b->b.vy, sx, sz, 0, EVW_HAZARD, 0, 0, b->b.vy, 0);
              }
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
              b->b.vy = 6.0f; b->airborne = 1; b->flip_pending = 1; b->launched_by = 0; b->haz_t[i] = 0; b->peak_y = b->b.y; b->bounced = 0; b->tipping = 0;
              if (b->ride_by) ride_end(m, j, RIDE_BACKED_OFF);
              /* the plate throws up the end of the bot that is over it: the bot turns away from the plate's centre */
              {
                float fx, fz, rx, rz; fwd_of(b->b.h, &fx, &fz); right_of(b->b.h, &rx, &rz);
                float dx = h->x - b->b.x, dz = h->z - b->b.z, lz = dotp(dx, dz, fx, fz), lx = dotp(dx, dz, rx, rz);
                float w = CHASSIS_TUMBLE[b->build.chassis] * 0.85f;
                if (fabs_f(lz) >= fabs_f(lx)) b->b.wp = lz >= 0 ? w : -w; else b->b.wr = lx >= 0 ? w : -w;
              }
              if (hs->fire_t != 1) hs->cooldown = (int16_t)h->p1;
              b->hazard_hits++;
              ev_full(m, EV_HAZ_HIT, j, i, 0, b->b.x, b->b.z, b->b.y, 0, 0, 0, 0, EVW_HAZARD, 0, 0, 6.0f, 0);
              ev_full(m, EV_LAUNCH, j, j, LAUNCH_RAMP, b->b.x, b->b.z, b->b.y, 6.0f, 0, 0, 0, EVW_HAZARD, 0, 0, 6.0f, fabs_f(b->b.wp) + fabs_f(b->b.wr));
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
    if (!b->is_ops && !b->falling && !b->lifted_by) {            /* held over the edge, it goes when it is let go */
      if (((ar->open_mask & OPEN_PX) && bd->x > hw + 0.3f) || ((ar->open_mask & OPEN_NX) && bd->x < -hw - 0.3f) ||
          ((ar->open_mask & OPEN_PZ) && bd->z > hd + 0.3f) || ((ar->open_mask & OPEN_NZ) && bd->z < -hd - 0.3f)) {
        b->falling = 1; b->airborne = 1; if (bd->vy > 0) bd->vy = 0;
        ev_full(m, EV_FALL, i, i, FALL_ROOF, bd->x, bd->z, bd->y, 0, 0, 0, 0, 0, 0, 0, 0, 0);
      }
    }
    if (b->pit_state == 2) b->pit_t++;
    /* count-out (2.7) */
    if (!b->is_ops) {
      float moved = len2(bd->x - b->still_x, bd->z - b->still_z);
      b->still_t++;
      if (b->still_t >= 120) { b->still = (uint8_t)(moved < 0.2f); b->still_x = bd->x; b->still_z = bd->z; b->still_t = 0; }
      float speed = len2(bd->vx, bd->vz);
      int inv_stuck = sim_inv_stuck(b);
      int inv_cant = b->inverted && b->build.drive != DR_HOVER && b->build.drive != DR_MELTY && !WEAPONS[b->build.weapon].inv_drive;
      /* ten fast frames reset the count, unless the bot is inverted with no way to drive (on the floor or in
       * the air off a flipper): a hit's knockback or a launch is not movement, and it used to restart a
       * helpless bot's count on every hit so the KO took 17 s instead of 10, or never came at all */
      /* a ridden bot is not moving on purpose: being carried does not reset its count (the wedge's does) */
      if (speed > 0.5f && !b->ride_by) { b->fast_t++; if (b->fast_t >= 10 && !inv_cant) { b->countout = 0; } } else b->fast_t = 0;
      int counting = inv_stuck || (b->still && !b->airborne && !b->lifted_by && !b->clamped_by && speed < 0.5f);
      if (m->park && b->is_ai) counting = 0;          /* a parked practice bot under the rig */
      if (m->frame < COUNTOUT_GRACE_FRAMES && !inv_stuck) counting = 0;   /* the first 5 s: a player reading the screen is not counted */
      if (m->ops_rescue && m->ops_target == i && (m->ops_state == OPS_GO || m->ops_state == OPS_LIFT || m->ops_state == OPS_CARRY)) counting = 0;   /* OPS is on its way */
      if (counting) {
        b->countout++;
        if (b->countout % 60 == 0 && b->countout < COUNTOUT_FRAMES) ev_simple(m, EV_COUNT_TICK, i, i, 10 - b->countout / 60, bd->x, bd->z, bd->y);
      } else if (!b->inverted && b->fast_t >= 10) b->countout = 0;
      if (b->inverted) b->frames_inverted++;
      /* a lid on the floor at over 1.5 u/s for 6 frames scrapes (once per 30; the wall's scrape is walls_step's) */
      if (b->inverted && !b->airborne && speed > SCRAPE_SPEED) {
        if (++b->scrape_t == 6 && b->scrape_cd == 0) { ev_full(m, EV_SCRAPE, i, i, SCRAPE_FLOOR, bd->x, bd->z, b->floor_y, speed, bd->vx / speed, bd->vz / speed, PN_TOP, 0, 0, 0, speed, 0); b->scrape_cd = 30; }
      } else if (b->inverted) b->scrape_t = 0;
      /* the wall wedge nudge: |thr| > 0.5 against a wall with no displacement for 120 frames. A bot held
       * there by another bot is not wedged on the geometry: that is a pin (2.11), OPS's to break */
      if ((b->pinned & 1) && fabs_f(b->drive_t) > 0.5f && moved < 0.05f && !b->airborne && !b->touch_bot && !b->ride_by) {
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
        int lighter = b->last_hit_by && b->last_hit_by - 1 < m->nbots ? b->last_hit_by - 1 : -1;
        if (lighter >= 0) { bot_t *a = &m->bots[lighter]; a->burn_dealt += d; a->dealt += d; }
        /* the lid cooks: a pool point a second off the TOP (3.9) */
        if (((int)b->burn_t) % 30 == 0) wear_panel(m, lighter, i, PN_TOP, 1.0f);
        if (b->burn_t <= 0) { b->burn_t = 0; ev_full(m, EV_BURN_END, lighter < 0 ? 255 : lighter, i, 0, bd->x, bd->z, bd->y + bd->clr + bd->hgt, 0, 0, 0, PN_TOP, WP_FLAME, 0, 0, 0, 0); }
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
        /* my own speed toward them (the relative speed is the same for both bots, so it tallied every fight
         * as an even split and the judges card read 2.0 and 2.0, or 0.0 and 0.0) */
        if (d < 2.0f && d > 0.01f && dotp(bd->vx, bd->vz, dx / d, dz / d) > 0.5f) b->aggro += 1;
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
      int rescue = sim_ops_rescue(m);
      int wake = m->no_hit_frames >= 720 || pinner >= 0 || rescue >= 0;
      if (m->ops_driven) wake = wake || (m->ops_in_drive.ly < -16000);
      if (!wake) break;
      int target = rescue >= 0 ? rescue : pinner >= 0 && pinner < m->nbots && m->bots[pinner].alive ? pinner : hp_leader(m, -1);
      if (target < 0) break;
      m->ops_rescue = (uint8_t)(rescue >= 0);
      m->ops_target = (uint8_t)target; m->ops_state = OPS_GO; m->gate_open = 1; m->ops_t = (int16_t)(rescue >= 0 ? 900 : 420); m->ops_awake_once = 1;   /* a rescue chases a bot that is still being shoved about */
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
          lift_release(m, OPS_SLOT, target); t->b.vy = 2.0f; o->carry_t = 0; o->lift = 0;
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
        if (t->lifted_by == OPS_SLOT + 1) {
          lift_release(m, OPS_SLOT, m->ops_target);
          /* a rescue sets the bot down the right way up and its count starts over */
          if (m->ops_rescue && t->inverted) { t->inverted = 0; t->countout = 0; t->b.pitch = t->b.roll = 0; ev_simple(m, EV_SELFRIGHT, m->ops_target, m->ops_target, 2, t->b.x, t->b.z, t->b.y); }
        }
        o->carry_t = 0; o->lift = 0; o->saw_on = 0; m->ops_state = OPS_RETURN; m->ops_rescue = 0;
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
    for (int i = 0; i < m->nbots; i++) if (m->bots[i].lifted_by == OPS_SLOT + 1) lift_release(m, OPS_SLOT, i);
    o->carry_t = 0; o->lift = 0; m->ops_state = OPS_LEAVING;
  }
  if (m->ops_state == OPS_RETURN || m->ops_state == OPS_LEAVING || m->ops_state == OPS_PARKED) m->ops_rescue = 0;
}

/* inverted with no way to drive (4.8, 3.5): HOVER and MELTY drive either way up, WEDGE, NONE, DRUM, SAW and
 * SPIKE drive inverted at 70 percent, the rest sit there */
int sim_inv_stuck(const bot_t *b) {
  return b->inverted && !b->airborne && b->build.drive != DR_HOVER && b->build.drive != DR_MELTY && !WEAPONS[b->build.weapon].inv_drive;
}

/* the OPS rescue: in a 1v1 that is not KO BY FLIP, a bot inverted with no way to drive and no self right has
 * nothing left to do, so once its count passes 5 OPS comes, lifts it, sets it down the right way up near the
 * centre and the count starts over; the count holds while OPS is on its way */
int sim_ops_rescue(const match_t *m) {
  if (!sim_is_1v1(m) || m->mode == MODE_KO_FLIP) return -1;
  for (int i = 0; i < m->nbots; i++) {
    const bot_t *b = &m->bots[i];
    if (!b->alive || b->is_ops || b->hp_max <= 0) continue;
    if (sim_inv_stuck(b) && WEAPONS[b->build.weapon].selfright == SR_NONE && b->countout >= 300) return i;
  }
  return -1;
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
    if (b->cooldown > 0) { b->cooldown--; if (b->cooldown == 0 && b->build.weapon == WP_FLIPPER && b->alive && !b->is_ops) tell(m, i, TELL_FLIPPER_RESET, WP_FLIPPER); }
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
    if (b->slide_cd > 0) b->slide_cd--;
    if (b->scrape_cd > 0) b->scrape_cd--;
    if (b->bind_t > 0) b->bind_t--;
    if (b->limp_t > 0) b->limp_t--;
    if (b->tread_hit > 0) b->tread_hit--;
    if (b->dig_t > 0) b->dig_t--;
    if (b->dig_cd > 0) b->dig_cd--;
    if (b->hit_flash_t > 0) b->hit_flash_t--;
    if (b->spent_t > 0) b->spent_t--;
    for (int j = 0; j < MAX_BOTS; j++) if (b->ride_cd[j] > 0) b->ride_cd[j]--;
    if (b->pin_t > 0 && len2(b->b.vx, b->b.vz) >= 0.3f) b->pin_t = 0;
    for (int j = 0; j < MAX_BOTS; j++) { if (b->hit_cd[j] > 0) b->hit_cd[j]--; if (b->hit_cd2[j] > 0) b->hit_cd2[j]--; }
  }
}

/* -- the HUD's readiness fields (DEEP-plan 6 step 12): written every step, never read by the sim -- */
static void hud_fields(match_t *m) {
  for (int i = 0; i < m->nbots; i++) {
    bot_t *b = &m->bots[i];
    if (b->hp_max == 0 || b->is_ops) continue;
    int w = b->build.weapon, melty = b->build.drive == DR_MELTY;
    const weapon_def_t *wd = &WEAPONS[w];
    int operate = b->alive && !(b->inverted && !wd->inv_operate) && !b->lifted_by && !b->clamped_by && b->lock <= 0;
    int ready = 0;
    if (melty) ready = b->spin > SPINNER_HIT_MIN;
    else switch (w) {
      case WP_NONE: case WP_WEDGE: ready = b->dig_cd == 0 && b->alive && !b->inverted; break;
      case WP_HORIZ: case WP_DRUM: ready = b->spin > SPINNER_HIT_MIN && operate; break;
      case WP_SAW: ready = b->saw_on && b->spin > 0.5f && operate; break;
      case WP_FLAME: ready = b->fuel > 0 && operate; break;
      case WP_LIFTER: ready = operate; break;
      default: ready = operate && b->cooldown == 0 && b->active == 0 && b->windup == 0 && b->clamp == 0 && b->stun <= 0; break;
    }
    b->weapon_ready = (uint8_t)ready;
    b->self_right_key = (uint8_t)(wd->selfright == SR_B ? 1 : wd->selfright == SR_DRUM ? 2 : 0);
    b->can_self_right = (uint8_t)(wd->selfright == SR_B || (wd->selfright == SR_DRUM && b->spin > 0.5f));
    /* a ramming build: its closing speed on the nearest enemy along its nose (0 more than 30 degrees off it) */
    b->ram_closing = 0;
    if (!melty && (w == WP_NONE || w == WP_WEDGE) && b->alive) {
      int best = -1; float bd = 1e9f;
      for (int j = 0; j < m->nbots; j++) {
        const bot_t *o = &m->bots[j];
        if (j == i || !o->alive || o->is_ops || o->hp_max == 0 || (m->mode == MODE_2V2 && o->team == b->team)) continue;
        float d = len2(o->b.x - b->b.x, o->b.z - b->b.z);
        if (d < bd) { bd = d; best = j; }
      }
      if (best >= 0 && bd > 1e-3f) {
        const bot_t *o = &m->bots[best];
        float ux = (o->b.x - b->b.x) / bd, uz = (o->b.z - b->b.z) / bd, fx, fz; fwd_of(b->b.h, &fx, &fz);
        if (dotp(fx, fz, ux, uz) >= 0.866f) b->ram_closing = fmax_f(0, dotp(b->b.vx - o->b.vx, b->b.vz - o->b.vz, ux, uz));
      }
    }
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
  for (int i = 0; i < m->nbots; i++) { body_t *b = &m->bots[i].b; b->px = b->x; b->pz = b->z; b->ph = b->h; b->py = b->y; b->pp = b->pitch; b->pr = b->roll; }
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
  /* 9. bot vs bot and push, then the support each bot drives on next step */
  bots_collide(m);
  support_step(m);
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
  hud_fields(m);
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
