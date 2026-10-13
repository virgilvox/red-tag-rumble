/* sim.c: the deterministic match simulation (spec section 4). Owned by the sim
 * agent (items 3, 8, 13, 16, 20, 24). This file is a stub that compiles and
 * links: the trig table is real, everything else returns at once. No three.c,
 * no HUD, no host, no libm trig (sqrtf and fmodf only). */
#include "sim.h"
#include <string.h>
#include <math.h>

/* ── the trig table (4.2) ───────────────────────────────────────── */
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
  const float PI = 3.14159265f, HALF = 1.57079633f;
  float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
  if (ax == 0 && ay == 0) return 0;
  float r;
  if (ay <= ax) r = atan_unit(ay / ax);
  else r = HALF - atan_unit(ax / ay);
  if (x < 0) r = PI - r;
  return y < 0 ? -r : r;
}

/* ── events ─────────────────────────────────────────────────────── */
void sim_event(match_t *m, const event_t *e) {
  if (m->nev >= MAX_EVENTS) return;
  m->ev[m->nev++] = *e;
}

/* ── the API (stubs) ────────────────────────────────────────────── */
void sim_init(match_t *m, const match_setup_t *s) {
  memset(m, 0, sizeof *m);
  m->mode = s->mode; m->arena = s->arena; m->players = s->players; m->comeback = s->comeback;
  m->fight_len_s = s->fight_len_s ? s->fight_len_s : 75;
  m->clock_s = (float)m->fight_len_s;
  m->seed = s->seed; m->rng = s->seed ? s->seed : 0x9e3779b9u;
  for (int i = 0; i < 4; i++) m->hazard_seat[i] = 255;
  m->ops_target = 255;
}

void sim_step(match_t *m) {
  if (m->hitstop > 0) { m->hitstop--; return; }
  (void)m;
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
  snprintf(out, (size_t)n, "[sim] setup mode=%d arena=%d seed=%u start=%d b0=%d,%d,%d,%d b1=%d,%d,%d,%d b2=%d,%d,%d,%d b3=%d,%d,%d,%d ai=%d,%d,%d,%d players=%d comeback=%d len=%d",
    s->mode, s->arena, s->seed, start_frame,
    s->builds[0].chassis, s->builds[0].drive, s->builds[0].weapon, s->builds[0].armour,
    s->builds[1].chassis, s->builds[1].drive, s->builds[1].weapon, s->builds[1].armour,
    s->builds[2].chassis, s->builds[2].drive, s->builds[2].weapon, s->builds[2].armour,
    s->builds[3].chassis, s->builds[3].drive, s->builds[3].weapon, s->builds[3].armour,
    s->is_ai[0], s->is_ai[1], s->is_ai[2], s->is_ai[3], s->players, s->comeback, s->fight_len_s);
}

int sim_judges(const match_t *m, float damage[MAX_BOTS], float aggro[MAX_BOTS], float control[MAX_BOTS], int *rule) {
  for (int i = 0; i < MAX_BOTS; i++) { damage[i] = 0; aggro[i] = 0; control[i] = 0; }
  if (rule) *rule = 3;
  (void)m;
  return -1;
}

void sim_seat_pick(match_t *m, int seat, int dir) { (void)m; (void)seat; (void)dir; }
void sim_seat_fire(match_t *m, int seat) { (void)m; (void)seat; }
void sim_ops_driven(match_t *m, int driver_seat, int gunner_seat) { (void)m; (void)driver_seat; (void)gunner_seat; }

int sim_status_word(const bot_t *b) {
  if (b->clamped_by) return SW_CLAMPED;
  if (b->lifted_by) return SW_LIFTED;
  if (b->red_tag_t > 0) return SW_RED_TAG;
  if (b->burn_t > 0) return SW_BURNING;
  if (b->inverted) return SW_INVERTED;
  if (b->stun > 0) return SW_STUNNED;
  return SW_NONE;
}
