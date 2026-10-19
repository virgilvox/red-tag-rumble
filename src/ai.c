/* ai.c: perception ring, utility scorer, steering, difficulty, habits (spec
 * section 8), the OPS goals (7, 8.6), the practice bot panel and the attract
 * driver. Owned by the AI agent (items 12, 13). Headless: no three.c, no HUD,
 * no host, no libm trig (rtr_sin, rtr_cos, rtr_atan2 from sim.c; sqrtf only).
 *
 * Shape: ai_think runs every frame. It pushes a summary of every bot into a
 * 25 frame ring, reads the frame `reaction` steps back as what the AI knows
 * about the others (it knows itself live, like a driver feels the pedals),
 * scores the goals every sixth frame and steers every frame. Everything the
 * AI "sees" of the arena is the arena table: wall distances, hazard squares
 * and the hazard timeline, which it knows exactly (8.1). */
#include "ai.h"
#include "bot.h"
#include <string.h>
#include <math.h>

#define PI_F      3.14159265f
#define DEG(d)    ((d) * (PI_F / 180.0f))
#define STICK     32767.0f

/* -- small vector helpers ------------------------------------------ */
static float absf(float v) { return v < 0 ? -v : v; }
static float len2(float x, float z) { return sqrtf(x * x + z * z); }
static float dotp(float ax, float az, float bx, float bz) { return ax * bx + az * bz; }
static void fwd_of(float h, float *fx, float *fz) { *fx = rtr_sin(h); *fz = rtr_cos(h); }
static void right_of(float h, float *rx, float *rz) { *rx = rtr_cos(h); *rz = -rtr_sin(h); }
static void norm2(float *x, float *z) { float l = len2(*x, *z); if (l > 1e-6f) { *x /= l; *z /= l; } else { *x = 0; *z = 1; } }
static void rot2(float x, float z, float ang, float *ox, float *oz) {
  float c = rtr_cos(ang), s = rtr_sin(ang);
  *ox = x * c + z * s; *oz = -x * s + z * c;       /* a positive angle is a heading increase: a turn toward the body's local +x, the left on screen */
}
/* a world point in a bot's frame: lx right, lz forward */
static void to_local(float h, float bx, float bz, float px, float pz, float *lx, float *lz) {
  float fx, fz, rx, rz; fwd_of(h, &fx, &fz); right_of(h, &rx, &rz);
  float dx = px - bx, dz = pz - bz;
  *lx = dotp(dx, dz, rx, rz); *lz = dotp(dx, dz, fx, fz);
}

static const arena_def_t *ai_arena(const ai_t *a, const match_t *m) {
  if (a->arena) return a->arena;
  return &ARENAS[m->arena < ARENA_COUNT ? m->arena : 0];
}

/* -- weapon families ----------------------------------------------- */
static int weapon_spinner(int w) { return w == WP_HORIZ || w == WP_DRUM; }
static int bot_is_spinner(const ai_view_t *v) { return weapon_spinner(v->weapon) || v->drive == DR_MELTY; }
static int bot_is_wedge(const bot_t *b) { return b->build.chassis == CH_WEDGE || b->build.weapon == WP_WEDGE; }
static int view_is_wedge(const ai_view_t *v) { return v->chassis == CH_WEDGE || v->weapon == WP_WEDGE; }
static int front_weapon(int w) { return w == WP_FLIPPER || w == WP_HAMMER || w == WP_SPIKE || w == WP_CRUSHER || w == WP_LIFTER || w == WP_FLAME; }
static float push_of(const bot_t *b) { return b->is_ops ? 2.0f * 6.0f : build_push(&b->build); }
static int hover_ignores(int type) { return type == HZ_PIT || type == HZ_KILLSAW || type == HZ_FLAME || type == HZ_FLOORFLIP || type == HZ_SCREW; }

/* -- skill (8.4, and the house brief: EASY gentle, MEDIUM fair, HARD mean) ----
 * throttle caps the drive; lead is how far ahead (seconds of enemy velocity) the attack point sits;
 * hesitate is the chance per think that a tap weapon's window is missed; boost allows the L boost;
 * backoff is how long EASY and MEDIUM drive away after they land a hit, so a new player gets a breath.
 * Two tables: SKILL drives AI against AI (the robin, the attract, the FILL WITH BOT seats among
 * themselves) and SKILL_VS_HUMAN applies whenever a human seat is in the match, where MEDIUM is slower,
 * aims wider and takes the breath; HARD is the same in both */
typedef struct { float throttle, lead, hesitate, aim; int boost, backoff; } skill_t;
static const skill_t SKILL[3] = {
  /* throttle lead  hesitate aim   boost backoff */
  { 0.75f, 0.0f, 0.50f, 0.7f, 0, 90 },     /* EASY: slow, no lead, misses half its windows, aims 0.7 u wide, backs off after a hit, never evades */
  { 1.00f, 0.30f, 0.25f, 0.3f, 1, 0 },     /* MEDIUM: fair; misses a quarter of its tap windows */
  { 1.00f, 0.50f, 0.00f, 0.1f, 1, 0 },     /* HARD: mean; BAIT and the habit memory are HARD only */
};
static const skill_t SKILL_VS_HUMAN[3] = {
  { 0.75f, 0.0f, 0.50f, 0.7f, 0, 90 },
  { 0.85f, 0.30f, 0.25f, 0.5f, 1, 180 },   /* MEDIUM against a person: 0.85 throttle, aims 0.5 u wide, a 3 s breath after a landed hit */
  { 1.00f, 0.50f, 0.00f, 0.1f, 1, 0 },
};
#define AI_OPENING_BREATH 180                  /* frames EASY and MEDIUM hold off at the horn when a human is in the match */
static const skill_t *skill_of(const ai_t *a) {
  int d = a->difficulty > AI_HARD ? AI_MEDIUM : a->difficulty;
  return a->vs_human ? &SKILL_VS_HUMAN[d] : &SKILL[d];
}
static int human_in_match(const match_t *m) {
  for (int i = 0; i < m->nbots && i < MAX_BOTS; i++) if (!m->bots[i].is_ai && !m->bots[i].is_ops && m->bots[i].hp_max > 0) return 1;
  return 0;
}

/* can I self-right right now (3.5, 4.8) */
static int can_selfright(const bot_t *b) {
  int w = b->build.weapon;
  if (WEAPONS[w].selfright == SR_B) return 1;
  if (WEAPONS[w].selfright == SR_DRUM) return 1;     /* spin up inverted, then A */
  return 0;
}

/* is my primary ready to hurt someone this instant (the ATTACK bonus) */
static int weapon_ready(const bot_t *b) {
  int w = b->build.weapon;
  if (b->build.drive == DR_MELTY) return b->spin > 0.5f;
  switch (w) {
    case WP_NONE: case WP_WEDGE: return 1;
    case WP_HORIZ: case WP_DRUM: return b->spin > 0.7f;
    case WP_SAW: return b->saw_on && b->spin > 0.5f;
    case WP_FLAME: return b->fuel > 60 && !b->inverted;
    case WP_LIFTER: return !b->inverted;
    case WP_FLIPPER: case WP_HAMMER: case WP_SPIKE: case WP_CRUSHER:
      return b->cooldown == 0 && b->windup == 0 && b->clamp == 0 && (WEAPONS[w].inv_operate || !b->inverted);
    default: return 0;
  }
}

/* -- hazards ------------------------------------------------------- */
/* distance from a point to the hazard's square (axis aligned; ang is a render tilt) */
static float haz_dist(const hazard_def_t *h, float x, float z) {
  float hw = h->w > 0 ? h->w * 0.5f : 0.6f, hd = h->d > 0 ? h->d * 0.5f : 0.6f;
  if (h->type == HZ_BUMPER) hw = hd = 0.4f;
  float dx = absf(x - h->x) - hw, dz = absf(z - h->z) - hd;
  if (dx < 0) dx = 0; if (dz < 0) dz = 0;
  return len2(dx, dz);
}
/* a hazard that hurts a bot standing on it: 1 active now, 2 activating within `within` seconds, 0 safe.
 * Always-on hazards count as active. Terrain, the laser, wind, the hill and the deck are never danger
 * squares. The pit runs on the sim's own timeline (closed until its first opening at T minus 60, held in
 * the hazard state's aux), not the generic phase, so the AI knows it exactly, as 8.1 promises. With
 * HAZARDS OFF only the bumper posts still hurt. */
static int haz_danger(const ai_t *a, const match_t *m, int hi, float within, float *t_to) {
  const arena_def_t *ar = ai_arena(a, m);
  const hazard_def_t *h = &ar->haz[hi];
  if (t_to) *t_to = 1e9f;
  switch (h->type) {
    case HZ_BUMPER: if (t_to) *t_to = 0; return 1;
    case HZ_PULVERIZER: case HZ_SCREW: if (!m->hazards_on) return 0; if (t_to) *t_to = 0; return 1;
    case HZ_PIT: case HZ_KILLSAW: case HZ_FLAME: case HZ_FLOORFLIP: if (!m->hazards_on) return 0; break;
    default: return 0;
  }
  if (h->type == HZ_PIT) {
    float t = m->haz_clock - m->haz[hi].aux;
    if (t < 0) { if (t_to) *t_to = -t; return -t <= within ? 2 : 0; }
    float p = fmodf(t, h->period > 0 ? h->period : 1.0f);
    if (p < h->active_s) { if (t_to) *t_to = 0; return 1; }
    if (t_to) *t_to = h->period - p;
    return h->period - p <= within ? 2 : 0;
  }
  float pt = 0;
  int ph = hazard_phase(h, m->haz_clock, &pt);
  if (ph == 1) { if (t_to) *t_to = 0; return 1; }
  if (h->period > 0) {
    float t = h->period - pt;
    if (t_to) *t_to = t;
    if (t <= within) return 2;
  }
  return 0;
}

/* -- the perception ring (8.1) ------------------------------------- */
static void ring_push(ai_t *a, const match_t *m) {
  ai_frame_t *f = &a->ring[a->ring_head];
  memset(f, 0, sizeof *f);
  f->nbots = m->nbots;
  for (int i = 0; i < m->nbots && i < MAX_BOTS; i++) {
    const bot_t *b = &m->bots[i];
    ai_view_t *v = &f->bots[i];
    v->x = b->b.x; v->z = b->b.z; v->h = b->b.h; v->vx = b->b.vx; v->vz = b->b.vz;
    v->hp = b->hp; v->spin = b->build.drive == DR_MELTY || weapon_spinner(b->build.weapon) || b->build.weapon == WP_SAW ? b->spin : 0;
    v->y = b->b.y; v->vy = b->b.vy;
    v->windup = b->windup; v->cooldown = b->cooldown; v->countout = b->countout; v->boost = b->boost;
    v->weapon = b->build.weapon; v->drive = b->build.drive; v->chassis = b->build.chassis;
    v->alive = b->alive; v->inverted = b->inverted; v->airborne = b->b.y > 0.001f; v->lifted_by = b->lifted_by;
    v->is_ops = b->is_ops; v->team = b->team;
  }
  if (!a->ring_filled) { for (int i = 0; i < AI_RING; i++) a->ring[i] = *f; a->ring_filled = 1; }
  a->ring_head = (a->ring_head + 1) % AI_RING;
}
static const ai_frame_t *ring_read(const ai_t *a, int back) {
  if (back < 0) back = 0; if (back > AI_RING - 1) back = AI_RING - 1;
  int i = (a->ring_head - 1 - back) % AI_RING;
  if (i < 0) i += AI_RING;
  return &a->ring[i];
}

/* -- the pin detector (2.11), the AI's own view of it --------------- */
static int walls_within(const arena_def_t *ar, float x, float z, float r) {
  float hw = ar->w * 0.5f, hd = ar->d * 0.5f; int n = 0;
  if (!(ar->open_mask & OPEN_PX) && hw - x < r) n++;
  if (!(ar->open_mask & OPEN_NX) && x + hw < r) n++;
  if (!(ar->open_mask & OPEN_PZ) && hd - z < r) n++;
  if (!(ar->open_mask & OPEN_NZ) && z + hd < r) n++;
  return n;
}
static void pins_update(ai_t *a, const match_t *m) {
  const arena_def_t *ar = ai_arena(a, m);
  for (int i = 0; i < m->nbots && i < MAX_BOTS; i++) {
    const bot_t *p = &m->bots[i];
    int pinned_now = 0; uint8_t by = 0;
    if (p->alive && !p->is_ops && walls_within(ar, p->b.x, p->b.z, 1.0f) >= 2 && len2(p->b.vx, p->b.vz) < 0.3f) {
      for (int j = 0; j < m->nbots && j < MAX_BOTS; j++) {
        const bot_t *q = &m->bots[j];
        if (j == i || !q->alive || q->is_ops) continue;
        if (m->mode == MODE_2V2 && q->team == p->team) continue;
        if (len2(q->b.x - p->b.x, q->b.z - p->b.z) < p->b.hl + q->b.hl + 0.2f) { pinned_now = 1; by = (uint8_t)(j + 1); }
      }
    }
    if (pinned_now) { a->pin_t[i]++; a->pinner[i] = by; } else { a->pin_t[i] = 0; a->pinner[i] = 0; }
  }
}
static int pinned_bot(const ai_t *a, const match_t *m, int *pinner) {
  for (int i = 0; i < m->nbots && i < MAX_BOTS; i++) if (a->pin_t[i] >= 240 && a->pinner[i]) { if (pinner) *pinner = a->pinner[i] - 1; return i; }
  return -1;
}
static int hp_leader(const match_t *m) {
  int best = -1; float hp = -1;
  for (int i = 0; i < m->nbots && i < MAX_BOTS; i++) {
    const bot_t *b = &m->bots[i];
    if (!b->alive || b->is_ops) continue;
    if (b->hp > hp) { hp = b->hp; best = i; }
  }
  return best;
}

/* -- habit observation (8.4): what the human does to AI bots, tallied for ai_learn -- */
static struct { int pins, hazard_pushes, boosts; int pin_seen[MAX_BOTS]; } OBS;

static void observe(ai_t *a, const match_t *m) {
  int human = -1;
  for (int i = 0; i < m->nbots && i < MAX_BOTS; i++) if (!m->bots[i].is_ai && !m->bots[i].is_ops) { human = i; break; }
  if (human < 0) return;
  /* one observer per match: the lowest AI slot, so two AIs never count one boost twice */
  for (int i = 0; i < m->nbots && i < MAX_BOTS; i++) if (m->bots[i].is_ai && !m->bots[i].is_ops) { if (i != a->slot) return; break; }
  const bot_t *h = &m->bots[human];
  /* boosts: a rising boost timer on the human */
  if (h->boost > 0 && a->last_boost[human] == 0) OBS.boosts++;
  a->last_boost[human] = h->boost;
  /* pins the human lands on an AI bot: count once per pin */
  for (int i = 0; i < m->nbots && i < MAX_BOTS; i++) {
    if (!m->bots[i].is_ai || m->bots[i].is_ops) { OBS.pin_seen[i] = 0; continue; }
    int by = a->pinner[i] ? a->pinner[i] - 1 : -1;
    if (a->pin_t[i] >= 240 && by == human) { if (!OBS.pin_seen[i]) { OBS.pins++; OBS.pin_seen[i] = 1; } }
    else if (a->pin_t[i] == 0) OBS.pin_seen[i] = 0;
    /* hazard pushes: an AI bot takes hazard damage within 30 frames of touching the human */
    const bot_t *b = &m->bots[i];
    if (len2(h->b.x - b->b.x, h->b.z - b->b.z) < h->b.hl + b->b.hl + 0.2f) a->contact_t[i] = 30;
    else if (a->contact_t[i] > 0) a->contact_t[i]--;
    if (b->hazard_taken > a->last_haz_taken[i] + 0.5f && a->contact_t[i] > 0) OBS.hazard_pushes++;
    a->last_haz_taken[i] = b->hazard_taken;
  }
}

/* -- init and bind ------------------------------------------------- */
void ai_init(ai_t *a, int difficulty, int seat) {
  memset(a, 0, sizeof *a);
  a->difficulty = (uint8_t)difficulty;
  a->seat = (uint8_t)seat;
  a->reaction = difficulty == AI_EASY ? 24 : difficulty == AI_MEDIUM ? 12 : 3;
  a->stagger = (uint8_t)(seat % AI_THINK_EVERY);
  a->think_t = a->stagger;
  a->rng = 0x51ed270bu ^ (uint32_t)(seat * 7919 + difficulty * 104729);
  a->target = -1; a->haz_idx = -1; a->ops_target = -1;
  a->goal = GOAL_NONE;
}

void ai_bind(ai_t *a, int slot) { a->slot = (uint8_t)slot; }
void ai_set_arena(ai_t *a, const arena_def_t *arena) { a->arena = arena; }

/* -- target choice: the nearest live enemy as seen through the ring -- */
static int pick_target(const ai_t *a, const match_t *m, const ai_frame_t *f) {
  const bot_t *me = &m->bots[a->slot];
  int best = -1; float bd = 1e9f;
  for (int i = 0; i < f->nbots && i < MAX_BOTS; i++) {
    const ai_view_t *v = &f->bots[i];
    if (i == a->slot || !v->alive || v->is_ops) continue;
    if (m->mode == MODE_2V2 && v->team == me->team) continue;
    float d = len2(v->x - me->b.x, v->z - me->b.z);
    if (a->target == i) d -= 0.5f;                 /* a little loyalty so the target does not flicker */
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

/* the enemy's time to touch down, or a big number when it is grounded */
static float land_time(const ai_view_t *v) {
  if (!v->airborne) return 1e9f;
  /* y + vy t - 10 t^2 = 0 (gravity 20) */
  float g = 20.0f, disc = v->vy * v->vy + 2.0f * g * v->y;
  if (disc < 0) disc = 0;
  return (v->vy + sqrtf(disc)) / g;
}

/* a spun up spinner worth backing off from (8.2 EVADE, read against the energy model of sim.c 3.6): its
 * disc holds over 0.7 of its energy (spin 0.84; a hit takes 35 to 80 percent of it, so a disc that just
 * landed one is a window, not a threat), it is the right way up, its disc faces me (a HORIZONTAL or DRUM
 * only cuts forward; a MELTY is all disc), and my build does not shed it (a SHELL dome) */
static int spinner_threat(const bot_t *me, const ai_view_t *e, float mx, float mz) {
  if (!bot_is_spinner(e) || e->inverted || e->spin * e->spin <= 0.7f) return 0;
  if (me->build.chassis == CH_SHELL) return 0;
  if (e->drive == DR_MELTY && me->build.weapon == WP_FLIPPER) return 0;   /* a flipper throws a melty frame and all: it goes in */
  if (e->drive != DR_MELTY) {
    float lx, lz; to_local(e->h, e->x, e->z, mx, mz, &lx, &lz);
    if (lz < 0) return 0;                                  /* behind the disc */
  }
  return 1;
}

/* an enemy's half length, for the wedge's staging point */
static float ehl_of(const match_t *m, int slot) { return slot >= 0 && slot < m->nbots ? m->bots[slot].b.hl : 0.5f; }

/* -- the scorer (8.2) ---------------------------------------------- */
typedef struct {
  float score[GOAL_COUNT];
  float tx[GOAL_COUNT], tz[GOAL_COUNT];
  int haz[GOAL_COUNT];
} scores_t;

static void score_goals(ai_t *a, const match_t *m, const ai_frame_t *f, scores_t *s) {
  const bot_t *me = &m->bots[a->slot];
  const arena_def_t *ar = ai_arena(a, m);
  memset(s, 0, sizeof *s);
  for (int g = 0; g < GOAL_COUNT; g++) s->haz[g] = -1;
  const ai_view_t *e = a->target >= 0 ? &f->bots[a->target] : 0;
  float mx = me->b.x, mz = me->b.z;
  float dist = e ? len2(e->x - mx, e->z - mz) : 1e9f;
  int my_spinner = weapon_spinner(me->build.weapon) || me->build.drive == DR_MELTY;
  float my_spin = me->spin;
  int ready_bonus = 0, edge_hold = 0;

  /* ATTACK */
  if (e) {
    float sc = 0.5f;
    int exposed = 0;
    if (weapon_spinner(e->weapon) && e->spin < 0.3f) exposed = 1;                       /* spun down spinner */
    if (e->weapon == WP_FLIPPER) {                                                        /* the flank of a flipper */
      float lx, lz; to_local(e->h, e->x, e->z, mx, mz, &lx, &lz);
      if (lz < absf(lx) * 1.7320508f) exposed = 1;                                        /* outside its 60 degree front cone */
    }
    if (e->inverted) exposed = 1;                                                         /* underside */
    if (land_time(e) < 0.3f) exposed = 1;                                                 /* landing within 0.3 s */
    if (exposed) sc += 0.3f;
    if (weapon_ready(me)) { sc += 0.2f; ready_bonus = 1; }
    if (spinner_threat(me, e, mx, mz) && !my_spinner) sc -= 0.4f;   /* an inverted spinner is a target, not a threat */
    s->score[GOAL_ATTACK] = sc;
    /* the weakest zone: behind a spinner, the flank of a front weapon, straight at anything else; the
     * point leads the enemy by its velocity so a turning bot closes instead of chasing where it was */
    float lead = skill_of(a)->lead;
    float tx = e->x + e->vx * lead + a->aim_x, tz = e->z + e->vz * lead + a->aim_z;
    float efx, efz, erx, erz; fwd_of(e->h, &efx, &efz); right_of(e->h, &erx, &erz);
    if (!e->inverted && !e->airborne && bot_is_wedge(me) && (e->drive == DR_HOVER || e->drive == DR_MELTY || e->chassis == CH_SHELL)) {
      /* a skirt, a dome or a spinning frame: any edge (or none) is the edge, so the plate goes straight in,
       * nose on, and a disc that comes round glances off the plate instead of a side */
    }
    else if (!e->inverted && !e->airborne && bot_is_wedge(me)) {
      /* a wedge gets under an edge, never a wheeled side (sim.c clr_at): it lines up on the enemy's front or
       * back (a WALKER's front), staged 1.3 u out on that edge's line, then drives straight through it. It
       * takes the back of a front weapon, the front of a spinner (the disc glances off the plate and the
       * lower lip gets under its nose), else whichever end is nearer me */
      int back = front_weapon(e->weapon) || view_is_wedge(e);
      if (e->drive == DR_WALKER || weapon_spinner(e->weapon)) back = 0;
      else if (!back) back = dotp(mx - e->x, mz - e->z, efx, efz) < 0;
      float ex = back ? -efx : efx, ez = back ? -efz : efz;                 /* the edge's outward normal */
      float along = dotp(mx - e->x, mz - e->z, ex, ez), across = dotp(mx - e->x, mz - e->z, -ez, ex);
      float stage = ehl_of(m, a->target) + me->b.hl + 0.6f;
      if (along < stage * 0.8f || absf(across) > along * 0.7f + 0.2f) { tx = e->x + ex * stage; tz = e->z + ez * stage; }
    }
    else if (!e->inverted && !e->airborne) {
      if (weapon_spinner(e->weapon) && e->spin > 0.3f && dist > 1.6f) { tx -= efx * 0.9f; tz -= efz * 0.9f; }
      else if ((front_weapon(e->weapon) || view_is_wedge(e) || bot_is_wedge(me) || me->build.weapon == WP_LIFTER) && dist > 1.4f) {
        /* swing to the flank only from inside its front cone (a wedge's plate is the hardest zone there,
         * 4.7); behind or beside it is already the weak side */
        float lx, lz; to_local(e->h, e->x, e->z, mx, mz, &lx, &lz);
        float side = lx >= 0 ? 1.0f : -1.0f;
        if (lz > absf(lx) * 0.577f) { tx += erx * side * 0.9f; tz += erz * side * 0.9f; }
      }
    }
    if (a->approach_add != 0 && dist > 1.5f) {       /* the stuck detector swings the approach */
      float dx = tx - e->x, dz = tz - e->z;
      if (len2(dx, dz) < 0.05f) { dx = mx - e->x; dz = mz - e->z; norm2(&dx, &dz); dx *= 0.9f; dz *= 0.9f; }
      float rx, rz; rot2(dx, dz, a->approach_add, &rx, &rz);
      tx = e->x + rx; tz = e->z + rz;
    }
    s->tx[GOAL_ATTACK] = tx; s->tz[GOAL_ATTACK] = tz;
  }

  /* EVADE (never for EASY, which only backs off after landing a hit). The budget: after 120 frames
   * of evading one target the row is blocked for 90, so a flipper that cannot outrun a spinner goes
   * for its rear instead of orbiting it for the whole fight; an inverted enemy is never evaded */
  if (e) {
    float sc = 0;
    int wedge_or_treads = bot_is_wedge(me) || me->build.drive == DR_TREAD;
    if (a->difficulty != AI_EASY && a->evade_block_t == 0 && !e->inverted) {
      if (spinner_threat(me, e, mx, mz) && dist < 3.0f && !wedge_or_treads) sc = 0.9f;
      if (e->weapon == WP_HAMMER && e->windup > 0 && dist < 1.5f && sc < 0.6f) sc = 0.6f;
      if (me->hp < 25 && e->hp > 60 && sc < 0.5f) sc = 0.5f;
    }
    if (a->backoff_t > 0) sc = 1.2f;                                       /* EASY and MEDIUM: a breath after a landed hit */
    s->score[GOAL_EVADE] = sc;
    /* back off to 3.5 u and circle at 60 degrees; during a breath with the enemy already far, ease away a
     * little instead of closing to the circle (the breath is a hold, not an approach) */
    float dx = mx - e->x, dz = mz - e->z; norm2(&dx, &dz);
    float rx, rz; rot2(dx, dz, DEG(60), &rx, &rz);
    s->tx[GOAL_EVADE] = e->x + rx * 3.5f; s->tz[GOAL_EVADE] = e->z + rz * 3.5f;
    if (a->backoff_t > 0 && dist > 4.0f) { s->tx[GOAL_EVADE] = mx + dx * 1.0f; s->tz[GOAL_EVADE] = mz + dz * 1.0f; }
  }

  /* a practice bot (EASY, and MEDIUM against a person) never shoves a person toward an open edge: with the
   * person within 4 u of a drop and me on the far side of them, my charge runs straight at the drop, so I
   * hold off and circle until they have come back in (THE ROOF used to end a first fight OFF THE ROOF in
   * 14 s with no exchange) */
  if (e && ar->open_mask && skill_of(a)->backoff > 0 && !m->bots[a->target].is_ai && !m->bots[a->target].is_ops) {
    float hw = ar->w * 0.5f, hd = ar->d * 0.5f;
    float ux = e->x - mx, uz = e->z - mz; norm2(&ux, &uz);
    int at_drop = 0;
    if ((ar->open_mask & OPEN_PX) && hw - e->x < 4.0f && ux > 0.3f) at_drop = 1;
    if ((ar->open_mask & OPEN_NX) && e->x + hw < 4.0f && ux < -0.3f) at_drop = 1;
    if ((ar->open_mask & OPEN_PZ) && hd - e->z < 4.0f && uz > 0.3f) at_drop = 1;
    if ((ar->open_mask & OPEN_NZ) && e->z + hd < 4.0f && uz < -0.3f) at_drop = 1;
    if (at_drop) {
      s->score[GOAL_ATTACK] = 0; edge_hold = 1;
      if (s->score[GOAL_EVADE] < 0.95f) s->score[GOAL_EVADE] = 0.95f;
    }
  }

  /* HAZARD PUSH */
  if (e && !e->is_ops) {
    const bot_t *eb = &m->bots[a->target];
    if (push_of(me) > push_of(eb) && !edge_hold) {
      int best = -1; float bd = 1e9f;
      for (int i = 0; i < ar->nhaz && i < MAX_HAZ; i++) {
        const hazard_def_t *h = &ar->haz[i];
        if (e->drive == DR_HOVER && hover_ignores(h->type)) continue;
        if (!haz_danger(a, m, i, 1.5f, 0)) continue;
        float d = haz_dist(h, e->x, e->z);
        /* a HOVER only fears the hammers: a wedge carries it to the nearer pulverizer from across the floor
         * (HANDOFF risk 1, DEEP-plan 6 step 13) */
        float reach = e->drive == DR_HOVER && h->type == HZ_PULVERIZER && bot_is_wedge(me) ? 8.0f : 3.0f;
        if (d < reach && d < bd) { bd = d; best = i; }
      }
      if (best >= 0) {
        const hazard_def_t *h = &ar->haz[best];
        s->score[GOAL_HAZARD_PUSH] = 0.8f;
        s->haz[GOAL_HAZARD_PUSH] = best;
        /* the line from me through them to the hazard: get behind them first, then drive at the square. The
         * drive stops at the near rim of a square that would hurt me too (a pit, a saw), a bot length short of
         * it, so the shove sends them over and not me after them; a HOVER crosses what it ignores */
        float lx = h->x - e->x, lz = h->z - e->z; norm2(&lx, &lz);
        float bx = e->x - lx * (me->b.hl + eb->b.hl + 0.1f), bz = e->z - lz * (me->b.hl + eb->b.hl + 0.1f);
        float behind = dotp(mx - e->x, mz - e->z, -lx, -lz);
        float rim = (me->build.drive == DR_HOVER && hover_ignores(h->type)) || h->type == HZ_PULVERIZER ? 0 : (h->w > 0 ? h->w * 0.5f : 0.6f) + me->b.hl + 0.3f;
        if (behind > 0.3f && len2(mx - bx, mz - bz) < 1.2f) { s->tx[GOAL_HAZARD_PUSH] = h->x - lx * rim; s->tz[GOAL_HAZARD_PUSH] = h->z - lz * rim; }
        else { s->tx[GOAL_HAZARD_PUSH] = bx; s->tz[GOAL_HAZARD_PUSH] = bz; }
      }
    }
  }

  /* RECOVER */
  if (me->inverted && can_selfright(me)) { s->score[GOAL_RECOVER] = 1.0f; s->tx[GOAL_RECOVER] = mx; s->tz[GOAL_RECOVER] = mz; }
  else if (e && my_spinner && my_spin < 0.6f && dist < 2.0f) {
    /* a spent disc (a hit costs SPIN_BLEED) backs off to spin up: one approach is one hit */
    s->score[GOAL_RECOVER] = 0.7f;
    float dx = mx - e->x, dz = mz - e->z; norm2(&dx, &dz);
    s->tx[GOAL_RECOVER] = e->x + dx * 4.0f; s->tz[GOAL_RECOVER] = e->z + dz * 4.0f;
  }

  /* BAIT (HARD only): a hazard about to go off and the enemy chasing me */
  if (e && a->difficulty == AI_HARD) {
    float cx = e->vx, cz = e->vz; float toward_x = mx - e->x, toward_z = mz - e->z; norm2(&toward_x, &toward_z);
    int chasing = dist < 5.0f && dotp(cx, cz, toward_x, toward_z) > 0.5f;
    if (chasing) {
      int best = -1; float bd = 1e9f;
      for (int i = 0; i < ar->nhaz && i < MAX_HAZ; i++) {
        const hazard_def_t *h = &ar->haz[i];
        if (me->build.drive != DR_HOVER || !hover_ignores(h->type)) { if (h->type == HZ_PULVERIZER || h->type == HZ_SCREW || h->type == HZ_BUMPER) continue; }
        float t_to = 0;
        if (haz_danger(a, m, i, 2.0f, &t_to) != 2) continue;                /* activating, not already live */
        float d = haz_dist(h, mx, mz);
        if (d < 4.0f && d < bd) { bd = d; best = i; }
      }
      if (best >= 0) {
        const hazard_def_t *h = &ar->haz[best];
        s->score[GOAL_BAIT] = 0.6f; s->haz[GOAL_BAIT] = best;
        float dx = h->x - mx, dz = h->z - mz;
        if (len2(dx, dz) < 0.3f) { dx = -toward_x; dz = -toward_z; }
        norm2(&dx, &dz);
        s->tx[GOAL_BAIT] = h->x + dx * 1.5f; s->tz[GOAL_BAIT] = h->z + dz * 1.5f;     /* across the square and out */
      }
    }
  }

  /* PICKUP (never for MELTY, never past the 3 per match cap) */
  if (me->build.drive != DR_MELTY && me->hp < 70 && me->pickups < 3) {
    int best = -1; float bd = 1e9f;
    for (int i = 0; i < m->ndebris && i < MAX_DEBRIS; i++) {
      const debris_t *d = &m->debris[i];
      if (!d->pickup || d->life <= 0) continue;
      float dd = len2(d->x - mx, d->z - mz);
      if (dd < 4.0f && dd < bd) { bd = dd; best = i; }
    }
    /* 0.45 as the table says, plus 0.3 when the panel is nearer than the enemy: ATTACK never
     * scores under 0.5, so without it the literal 0.45 could only win with no enemy at all */
    if (best >= 0) { s->score[GOAL_PICKUP] = 0.45f + (bd < dist ? 0.3f : 0.0f); s->tx[GOAL_PICKUP] = m->debris[best].x; s->tz[GOAL_PICKUP] = m->debris[best].z; }
  }

  /* TAUNT: the enemy is inverted and the 20 percent roll passes */
  if (e && e->inverted && me->taunt == 0 && !me->inverted) {
    s->score[GOAL_TAUNT] = 0.65f;
    if (rtr_rand_n(&a->rng, 100) < 20) s->score[GOAL_TAUNT] = 1.3f;   /* a passing roll wins this think: ATTACK on an inverted enemy is 0.8 or more, so a bare 0.65 never would */
    s->tx[GOAL_TAUNT] = mx; s->tz[GOAL_TAUNT] = mz;
  }

  /* The table's ATTACK floor (0.5, 0.7 with a ready weapon) would shadow EVADE's 0.6 and 0.5
   * and BAIT's 0.6 forever. While either has a live reason, ATTACK loses the ready bonus and
   * 0.15 more, so those rows pick what the table says and an exposed enemy still wins. */
  if (s->score[GOAL_ATTACK] > 0 && (s->score[GOAL_EVADE] > 0 || s->score[GOAL_BAIT] > 0) && !(e && e->inverted))
    s->score[GOAL_ATTACK] -= (ready_bonus ? 0.2f : 0.0f) + 0.15f;

  /* WIGGLE: never be counted out while parked */
  if (me->countout > 300 && !me->inverted) { s->score[GOAL_WIGGLE] = 0.95f; s->tx[GOAL_WIGGLE] = mx; s->tz[GOAL_WIGGLE] = mz; }
}

/* -- OPS (7, 8.6): the referee's own goals ------------------------- */
static void ops_goals(ai_t *a, const match_t *m, scores_t *s) {
  const bot_t *me = &m->bots[a->slot];
  const arena_def_t *ar = ai_arena(a, m);
  memset(s, 0, sizeof *s);
  for (int g = 0; g < GOAL_COUNT; g++) s->haz[g] = -1;
  float cx = 0, cz = 0;                                 /* the arena centre */
  float zx = ar->ops_zone[0], zz = ar->ops_zone[1];
  int state = m->ops_state;
  int target = m->ops_target < m->nbots ? m->ops_target : -1;

  /* OPS leaves at T minus 10 so the end is always between players (7) */
  int closing = m->clock_s > 0 && m->clock_s <= 10.0f;
  if (state == OPS_PARKED && closing) { a->ops_target = -1; a->carry_set = 0; return; }
  if (state != OPS_PARKED && closing) {
    a->carry_set = 0;
    s->score[GOAL_OPS_RETURN] = 0.7f; s->tx[GOAL_OPS_RETURN] = zx; s->tz[GOAL_OPS_RETURN] = zz;
    return;
  }
  if (state == OPS_PARKED) {
    /* the wake triggers, read the same way the sim reads them */
    int pinner = -1, pinned = pinned_bot(a, m, &pinner);
    int rescue = sim_ops_rescue(m);
    int wake = m->no_hit_frames >= 720 || pinned >= 0 || rescue >= 0;
    if (!wake) { a->ops_target = -1; a->carry_set = 0; return; }
    target = rescue >= 0 ? rescue : pinned >= 0 ? pinner : hp_leader(m);     /* the rescue, else LEADER TAX */
    if (target < 0) return;
    a->ops_target = target;
    s->score[GOAL_OPS_GO] = 0.8f; s->tx[GOAL_OPS_GO] = m->bots[target].b.x; s->tz[GOAL_OPS_GO] = m->bots[target].b.z;
    return;
  }
  if (target < 0) target = a->ops_target;
  if (target < 0 || target >= m->nbots) { int p; int pinned = pinned_bot(a, m, &p); target = pinned >= 0 ? p : hp_leader(m); }
  a->ops_target = target;
  const bot_t *t = target >= 0 ? &m->bots[target] : 0;
  int carrying = t && t->lifted_by == a->slot + 1;

  switch (state) {
    case OPS_GO: case OPS_LIFT:
      if (!t || !t->alive) { s->score[GOAL_OPS_RETURN] = 0.7f; s->tx[GOAL_OPS_RETURN] = zx; s->tz[GOAL_OPS_RETURN] = zz; break; }
      if (carrying) {
        if (!a->carry_set) { float dx = cx - t->b.x, dz = cz - t->b.z; norm2(&dx, &dz); a->carry_x = t->b.x + dx * 3.0f; a->carry_z = t->b.z + dz * 3.0f; a->carry_set = 1; }
        s->score[GOAL_OPS_CARRY] = 1.0f; s->tx[GOAL_OPS_CARRY] = a->carry_x; s->tz[GOAL_OPS_CARRY] = a->carry_z;
      } else {
        float d = len2(t->b.x - me->b.x, t->b.z - me->b.z);
        a->carry_set = 0;
        if (d < me->b.hl + t->b.hl + 0.6f) { s->score[GOAL_OPS_LIFT] = 0.9f; s->tx[GOAL_OPS_LIFT] = t->b.x; s->tz[GOAL_OPS_LIFT] = t->b.z; }
        else { s->score[GOAL_OPS_GO] = 0.8f; s->tx[GOAL_OPS_GO] = t->b.x; s->tz[GOAL_OPS_GO] = t->b.z; }
      }
      break;
    case OPS_CARRY:
      if (t && !a->carry_set) { float dx = cx - t->b.x, dz = cz - t->b.z; norm2(&dx, &dz); a->carry_x = t->b.x + dx * 3.0f; a->carry_z = t->b.z + dz * 3.0f; a->carry_set = 1; }
      if (len2(me->b.x - a->carry_x, me->b.z - a->carry_z) < 0.8f || !carrying) {
        s->score[GOAL_OPS_RETURN] = 0.7f; s->tx[GOAL_OPS_RETURN] = zx; s->tz[GOAL_OPS_RETURN] = zz;
      } else { s->score[GOAL_OPS_CARRY] = 1.0f; s->tx[GOAL_OPS_CARRY] = a->carry_x; s->tz[GOAL_OPS_CARRY] = a->carry_z; }
      break;
    case OPS_PUNISH:
      if (!t || !t->alive) { s->score[GOAL_OPS_RETURN] = 0.7f; s->tx[GOAL_OPS_RETURN] = zx; s->tz[GOAL_OPS_RETURN] = zz; break; }
      s->score[GOAL_OPS_PUNISH] = 1.0f;
      if (carrying) {
        /* shove: the nearest wall through them */
        float nx = 0, nz = 0, d = 0;
        if (arena_nearest_wall(ar, me->b.x, me->b.z, &nx, &nz, &d)) { s->tx[GOAL_OPS_PUNISH] = me->b.x - nx * 6.0f; s->tz[GOAL_OPS_PUNISH] = me->b.z - nz * 6.0f; }
        else { s->tx[GOAL_OPS_PUNISH] = cx; s->tz[GOAL_OPS_PUNISH] = cz; }
      } else { s->tx[GOAL_OPS_PUNISH] = t->b.x; s->tz[GOAL_OPS_PUNISH] = t->b.z; }
      break;
    case OPS_RETURN: case OPS_LEAVING: default:
      a->carry_set = 0;
      s->score[GOAL_OPS_RETURN] = 0.7f; s->tx[GOAL_OPS_RETURN] = zx; s->tz[GOAL_OPS_RETURN] = zz;
      break;
  }
}

/* -- choosing: highest score, 0.1 hysteresis for the current goal -- */
static void choose(ai_t *a, const scores_t *s) {
  int best = GOAL_NONE; float bs = 0;
  for (int g = 1; g < GOAL_COUNT; g++) {
    float v = s->score[g];
    if (v <= 0) continue;
    if (g == a->goal) v += 0.1f;
    if (v > bs) { bs = v; best = g; }
  }
  if (best != a->goal) {
    a->goal = best; a->last_err = 0;
    if (best == GOAL_TAUNT) a->taunt_t = 30;
    if (best == GOAL_WIGGLE) a->wiggle_t = 0;
    if (best != GOAL_ATTACK) a->approach_add = 0;
  }
  a->goal_score = bs;
  a->haz_idx = best != GOAL_NONE ? s->haz[best] : -1;
  if (best != GOAL_NONE) { a->tx = s->tx[best]; a->tz = s->tz[best]; }
}

/* -- steering (8.5) ------------------------------------------------ */
/* keep a target inside the floor, 1.2 u off every wall and edge_margin off an open edge: 1.2 for a bot that
 * means to shove someone over it, 3 u for the practice bot (a bot easing away from the horn's breath on
 * THE ROOF used to stop 0.6 u from the drop, with the wind blowing it the rest of the way) */
static void clamp_inside(const arena_def_t *ar, float *x, float *z, float edge_margin) {
  float hw = ar->w * 0.5f, hd = ar->d * 0.5f;
  float px = (ar->open_mask & OPEN_PX) ? edge_margin : 1.2f, nx = (ar->open_mask & OPEN_NX) ? edge_margin : 1.2f;
  float pz = (ar->open_mask & OPEN_PZ) ? edge_margin : 1.2f, nz = (ar->open_mask & OPEN_NZ) ? edge_margin : 1.2f;
  *x = rtr_clamp(*x, -hw + nx, hw - px); *z = rtr_clamp(*z, -hd + nz, hd - pz);
}

/* steer around a danger square that sits between me and the target: the target
 * slides to the side of the square nearer to my current heading */
static void avoid_hazards(const ai_t *a, const match_t *m, const bot_t *me, float *tx, float *tz) {
  const arena_def_t *ar = ai_arena(a, m);
  float mx = me->b.x, mz = me->b.z;
  float dx = *tx - mx, dz = *tz - mz, L = len2(dx, dz);
  if (L < 0.05f) return;
  float ux = dx / L, uz = dz / L;
  for (int i = 0; i < ar->nhaz && i < MAX_HAZ; i++) {
    const hazard_def_t *h = &ar->haz[i];
    if (i == a->haz_idx) continue;                                 /* the square I mean to cross or push into */
    if (me->build.drive == DR_HOVER && hover_ignores(h->type)) continue;
    float t_to = 0; int dg = haz_danger(a, m, i, 1.0f, &t_to);
    if (!dg) continue;
    if (h->type != HZ_PIT && dg == 2) continue;                     /* only a pit is worth a detour before it opens */
    float hw = (h->w > 0 ? h->w * 0.5f : 0.6f) + me->b.hl + 0.3f, hd = (h->d > 0 ? h->d * 0.5f : 0.6f) + me->b.hl + 0.3f;
    float r = hw > hd ? hw : hd;
    float cx = h->x - mx, cz = h->z - mz;
    /* on or beside the square, whatever the ray says: step straight away from its centre (a hard turn with
     * the open pit at my side used to sweep me in while the ray test looked the other way) */
    if (len2(cx, cz) < r + 0.1f) {
      float ax = -cx, az = -cz; if (len2(ax, az) < 0.05f) { ax = -uz; az = ux; } norm2(&ax, &az);
      *tx = mx + ax * 2.0f; *tz = mz + az * 2.0f;
      return;
    }
    /* closest approach of the ray me -> target to the square centre */
    float along = dotp(cx, cz, ux, uz);
    if (along < -0.2f || along > L + r) continue;
    float side = dotp(cx, cz, -uz, ux);                              /* lateral offset, positive to the left */
    if (absf(side) > r) continue;
    /* step the target sideways past the square, away from the centre */
    float s = side >= 0 ? -1.0f : 1.0f;
    float px = -uz * s, pz = ux * s;
    *tx = h->x + px * (r + 0.2f); *tz = h->z + pz * (r + 0.2f);
    return;
  }
}

/* turn is the wanted heading change: positive grows h, which is a turn to the left on screen (sim.c
 * right_of), so it goes out as stick LEFT; the sim reads stick right as a heading decrease */
static void write_stick(pad_state_t *out, float turn, float throttle) {
  turn = rtr_clamp(turn, -1, 1); throttle = rtr_clamp(throttle, -1, 1);
  out->lx = (int16_t)(-turn * STICK);
  out->ly = (int16_t)(-throttle * STICK);
}

/* drive toward (tx, tz): PD heading, throttle by error, reverse when it is close behind,
 * the stuck detector, the corner escape and the HOVER and MELTY direction vectors */
static void steer_to(ai_t *a, const match_t *m, const bot_t *me, float tx, float tz, pad_state_t *out, int allow_reverse) {
  const arena_def_t *ar = ai_arena(a, m);
  const drive_def_t *d = &DRIVES[me->build.drive];
  /* an enemy within reach against the wall is driven at, not stopped short of (the clamp parked a wedge
   * 1.2 u off the lexan while a hover pressed into it from the other side) */
  int close_enemy = 0;
  if (a->goal == GOAL_ATTACK && a->target >= 0 && a->target < m->nbots) { const bot_t *e = &m->bots[a->target]; close_enemy = len2(e->b.x - tx, e->b.z - tz) < 0.3f && len2(e->b.x - me->b.x, e->b.z - me->b.z) < me->b.hl + e->b.hl + 0.6f; }
  if (!close_enemy) clamp_inside(ar, &tx, &tz, !me->is_ops && skill_of(a)->backoff > 0 ? 3.0f : 1.2f);
  avoid_hazards(a, m, me, &tx, &tz);
  float mx = me->b.x, mz = me->b.z;
  float dx = tx - mx, dz = tz - mz, dist = len2(dx, dz);

  /* the corner escape: within 1.0 of two walls, drive along the longer wall for 60 frames */
  if (a->corner_t > 0) { a->corner_t--; dx = a->corner_dx; dz = a->corner_dz; dist = 3.0f; }
  else if (walls_within(ar, mx, mz, 1.0f) >= 2) {
    float hw = ar->w * 0.5f, hd = ar->d * 0.5f;
    if (ar->w >= ar->d) { a->corner_dx = mx > 0 ? -1.0f : 1.0f; a->corner_dz = 0; (void)hw; }
    else { a->corner_dx = 0; a->corner_dz = mz > 0 ? -1.0f : 1.0f; (void)hd; }
    a->corner_t = 60; dx = a->corner_dx; dz = a->corner_dz; dist = 3.0f;
  }

  /* the stuck detector: under 0.3 u of progress in 120 frames while trying to move. The escape backs
   * off for 60 frames and swings the approach 50 degrees; with a wall within 1.2 u it drives off the
   * wall instead, diagonally toward the arena centre, never straight back into the lexan (that was
   * the [stuck] nudge in a wedge against wedge shove at a wall) */
  a->stuck_t++;
  /* the sim counts a bot pressing into the lexan or a solid without moving (bot_t.stuck_t, the wedge nudge
   * at 120 frames): 45 of those and the escape starts now, not at the next 120 frame progress check */
  int wedged = me->stuck_t >= 45 && a->reverse_t == 0;
  if (a->stuck_t >= 120 || wedged) {
    float moved = len2(mx - a->progress_x, mz - a->progress_z);
    if ((moved < 0.3f || wedged) && dist > 0.6f && bot_can_drive(me) && !(me->inverted && me->build.weapon == WP_HORIZ)) {   /* an inverted bot that drives (WEDGE, NONE, DRUM...) backs off too */
      a->reverse_t = 60;
      a->approach_add = rtr_wrap_pi(a->approach_add + DEG(50));
      float nx, nz, wd; a->esc_wall = 0;
      if (arena_nearest_wall(ar, mx, mz, &nx, &nz, &wd) && wd < 1.2f) {
        float tx_ = nz, tz_ = -nx;
        if (dotp(tx_, tz_, -mx, -mz) < 0) { tx_ = -tx_; tz_ = -tz_; }
        a->esc_dx = nx + tx_; a->esc_dz = nz + tz_; norm2(&a->esc_dx, &a->esc_dz); a->esc_wall = 1;
      }
    } else if (moved > 2.0f) a->approach_add = 0;
    a->progress_x = mx; a->progress_z = mz; a->stuck_t = 0;
  }
  if (a->reverse_t > 0 && a->esc_wall) { dx = a->esc_dx; dz = a->esc_dz; dist = 3.0f; a->corner_t = 0; }

  if (d->strafe) {
    /* HOVER and MELTY: the stick is the world direction against cam_yaw 0: stick up is +z and
     * stick right is -x (screen right of a camera looking along +z, see sim.c drive_step) */
    out->cam_yaw = 0;
    if (dist < 0.15f) { write_stick(out, 0, 0); return; }
    float ux = dx / dist, uz = dz / dist;
    if (a->reverse_t > 0) { a->reverse_t--; if (!a->esc_wall) { ux = -ux; uz = -uz; } }
    float mag = (dist < 1.0f ? 0.5f + 0.5f * dist : 1.0f) * skill_of(a)->throttle;
    out->lx = (int16_t)(-ux * mag * STICK); out->ly = (int16_t)(-uz * mag * STICK);
    return;
  }

  if (dist < 0.12f) { write_stick(out, 0, 0); a->last_err = 0; return; }
  float want = rtr_atan2(dx, dz);
  float err = rtr_wrap_pi(want - me->b.h);
  int reversing = 0;
  if (a->retreat) { err = rtr_wrap_pi(err + PI_F); reversing = 1; }                    /* back away, enemy ahead */
  else if (allow_reverse && dist < 1.5f && absf(err) > DEG(90)) { err = rtr_wrap_pi(err + PI_F); reversing = 1; }
  float derr = err - a->last_err; a->last_err = err;
  float turn = rtr_clamp(2.2f * err + 0.3f * derr, -1, 1);
  /* full ahead inside 60 degrees, else at least half throttle (the spec's 0.3 made a turning spinner
   * orbit an evader at 1.5 u/s for the whole fight); the skill cap sits on top */
  float throttle = (absf(err) < DEG(60) ? 1.0f : rtr_clamp(rtr_cos(err), 0.5f, 1.0f)) * skill_of(a)->throttle;
  if (reversing) { throttle = -throttle; turn = -turn; }
  if (a->reverse_t > 0) { a->reverse_t--; if (!a->esc_wall) { throttle = -1.0f; turn = -turn; } }
  if (me->inverted && me->build.weapon == WP_HORIZ) { throttle = 0; turn = 0; }       /* cannot steer inverted */
  write_stick(out, turn, throttle);
}

/* -- the LIFTER's carry ------------------------------------------- */
/* a chassis the tip turns onto its lid (deep-physics 3.8 tumble rates over the tip's air time): BOX, WEDGE,
 * FRAME, LUNCHBOX on any drive that can be inverted; a LOWBOY or SHELL lands back upright too often, a HOVER
 * or MELTY never inverts */
static int lifter_tips(const bot_t *v) {
  if (v->build.drive == DR_HOVER || v->build.drive == DR_MELTY) return 0;
  return v->build.chassis == CH_BOX || v->build.chassis == CH_WEDGE || v->build.chassis == CH_FRAME || v->build.chassis == CH_LUNCHBOX;
}
enum { LK_NONE, LK_PIT, LK_EDGE, LK_PULVERIZER };
/* the nearest place to put a carried bot: an open (or soon open) pit, an open edge, a pulverizer, inside 8 u
 * (what 3 s of carry at 0.6x reaches); a practice bot never carries a person to an edge */
static int lifter_kill_target(const ai_t *a, const match_t *m, const bot_t *me, const bot_t *v, float *tx, float *tz) {
  const arena_def_t *ar = ai_arena(a, m);
  int kind = LK_NONE; float bd = 8.0f;
  for (int i = 0; i < ar->nhaz && i < MAX_HAZ; i++) {
    const hazard_def_t *h = &ar->haz[i];
    if (h->type != HZ_PIT && h->type != HZ_PULVERIZER) continue;
    if (h->type == HZ_PIT && v->build.drive == DR_HOVER) continue;
    if (!haz_danger(a, m, i, 2.0f, 0)) continue;
    float d = len2(h->x - me->b.x, h->z - me->b.z);
    if (d < bd) { bd = d; kind = h->type == HZ_PIT ? LK_PIT : LK_PULVERIZER; *tx = h->x; *tz = h->z; }
  }
  if (ar->open_mask && !(skill_of(a)->backoff > 0 && !v->is_ai)) {
    float hw = ar->w * 0.5f, hd = ar->d * 0.5f, x = me->b.x, z = me->b.z;
    /* a point 1 u past the edge, straight out from me */
    if ((ar->open_mask & OPEN_PX) && hw - x < bd) { bd = hw - x; kind = LK_EDGE; *tx = hw + 1.0f; *tz = z; }
    if ((ar->open_mask & OPEN_NX) && x + hw < bd) { bd = x + hw; kind = LK_EDGE; *tx = -hw - 1.0f; *tz = z; }
    if ((ar->open_mask & OPEN_PZ) && hd - z < bd) { bd = hd - z; kind = LK_EDGE; *tx = x; *tz = hd + 1.0f; }
    if ((ar->open_mask & OPEN_NZ) && z + hd < bd) { bd = z + hd; kind = LK_EDGE; *tx = x; *tz = -hd - 1.0f; }
  }
  return kind;
}
/* the carried bot is where letting go kills: over the pit (it drops in), past the edge (it falls), or a throw
 * short of the pulverizer (the tip lands it under the head) */
static int lifter_at_kill(const ai_t *a, const match_t *m, const bot_t *v, int kind, float tx, float tz) {
  const arena_def_t *ar = ai_arena(a, m);
  if (kind == LK_PIT) {
    for (int i = 0; i < ar->nhaz && i < MAX_HAZ; i++) {
      const hazard_def_t *h = &ar->haz[i];
      if (h->type == HZ_PIT && absf(v->b.x - h->x) < h->w * 0.5f - 0.35f && absf(v->b.z - h->z) < h->d * 0.5f - 0.35f) return 1;
    }
    return 0;
  }
  if (kind == LK_EDGE) {
    float hw = ar->w * 0.5f, hd = ar->d * 0.5f;
    return ((ar->open_mask & OPEN_PX) && v->b.x > hw + 0.35f) || ((ar->open_mask & OPEN_NX) && v->b.x < -hw - 0.35f) ||
           ((ar->open_mask & OPEN_PZ) && v->b.z > hd + 0.35f) || ((ar->open_mask & OPEN_NZ) && v->b.z < -hd - 0.35f);
  }
  if (kind == LK_PULVERIZER) return len2(v->b.x - tx, v->b.z - tz) < 1.4f;
  return 1;
}

/* -- weapons (8.3) ------------------------------------------------- */
static int enemy_in_box(const bot_t *me, const ai_view_t *e, float ehl, float w, float z0, float z1, float lead_frames) {
  float ex = e->x + e->vx * lead_frames * DT, ez = e->z + e->vz * lead_frames * DT;
  float lx, lz; to_local(me->b.h, me->b.x, me->b.z, ex, ez, &lx, &lz);
  return absf(lx) < w * 0.5f + ehl * 0.5f && lz > z0 - ehl * 0.5f && lz < z1 + ehl * 0.5f;
}

static void weapons(ai_t *a, const match_t *m, const ai_frame_t *f, pad_state_t *out) {
  const bot_t *me = &m->bots[a->slot];
  const ai_view_t *e = a->target >= 0 ? &f->bots[a->target] : 0;
  float ehl = a->target >= 0 ? m->bots[a->target].b.hl : 0.5f;
  float dist = e ? len2(e->x - me->b.x, e->z - me->b.z) : 1e9f;
  int w = me->build.weapon, melty = me->build.drive == DR_MELTY;
  uint16_t btn = 0;
  int prev_a = a->prev_buttons & IN_A;
  int in_front = 0;
  if (e) { float lx, lz; to_local(me->b.h, me->b.x, me->b.z, e->x, e->z, &lx, &lz); in_front = lz > 0 && absf(lx) < lz * 0.577f + 0.3f; }

  /* self-right first (4.8) */
  if (me->inverted) {
    if (WEAPONS[w].selfright == SR_B) { if (me->selfright == 0 && !(a->prev_buttons & IN_B) && (m->frame % 20) == 0) btn |= IN_B; }
    else if (WEAPONS[w].selfright == SR_DRUM) {
      if (me->spin > 0.5f && me->selfright == 0) { if (!prev_a) btn |= IN_A; }        /* an A edge at spin */
      else btn |= IN_A;                                                               /* spin up inverted */
    }
    /* spinners keep spinning inverted; everything else waits */
    if (w == WP_HORIZ) btn |= IN_A;
    out->buttons |= btn;
    return;
  }

  /* a hesitating think misses this window for the tap weapons: EASY misses half of them */
  int tap_ok = !a->hesitate;
  if (melty) { btn |= IN_A; }                                                          /* the body is the weapon */
  else switch (w) {
    case WP_HORIZ: case WP_DRUM: btn |= IN_A; break;                                   /* hold unless RECOVER, which also holds */
    case WP_SAW: {
      int want = me->saw_on ? dist < 4.0f : dist < 1.5f;
      if (want != (me->saw_on != 0) && !prev_a) btn |= IN_A;                          /* one tap toggles */
      break;
    }
    case WP_FLIPPER:
      if (e && tap_ok && me->cooldown == 0 && enemy_in_box(me, e, ehl, 0.6f, 0.4f, 0.9f, 0) && !prev_a) btn |= IN_A;
      break;
    case WP_HAMMER:
      if (e && tap_ok && me->cooldown == 0 && me->windup == 0 && dist < 1.2f + ehl && enemy_in_box(me, e, ehl, 0.4f, 0.5f, 1.0f, 15) && !prev_a) btn |= IN_A;
      break;
    case WP_SPIKE:
      if (e && tap_ok && me->cooldown == 0 && in_front && dist < 1.0f + ehl + me->b.hl && !prev_a) btn |= IN_A;
      break;
    case WP_CRUSHER:
      if (e && tap_ok && me->cooldown == 0 && me->clamp == 0 && dist < me->b.hl + ehl + 0.25f && in_front && !prev_a) btn |= IN_A;
      break;
    case WP_FLAME:
      if (a->burst_t > 0) { a->burst_t--; btn |= IN_A; if (a->burst_t == 0) a->burst_t = -30; }
      else if (a->burst_t < 0) a->burst_t++;
      else if (e && me->fuel > 30 && dist < 2.0f + ehl && in_front) { a->burst_t = 60; btn |= IN_A; }
      break;
    case WP_LIFTER: {
      /* held while closing (the forks stay down and grab what they touch); on the forks at full lift: a tall
       * or boxy bot is let go at once (it tips over and lands on its lid), anything else is carried to a pit,
       * an open edge or a pulverizer and let go there; with none in reach it is tipped anyway */
      int holding = -1;
      for (int i = 0; i < m->nbots; i++) if (m->bots[i].lifted_by == a->slot + 1) holding = i;
      if (holding >= 0) {
        int full = me->lift > WP_LIFTER_TIP_LIFT && me->carry_t >= WP_LIFTER_TIP_HOLD, let = 0;
        if (full) {
          float tx, tz; int kind = lifter_kill_target(a, m, me, &m->bots[holding], &tx, &tz);
          if (lifter_tips(&m->bots[holding]) || kind == LK_NONE || me->stuck_t > 20) let = 1;   /* blocked on the way: tip it here */
          else let = lifter_at_kill(a, m, &m->bots[holding], kind, tx, tz);
        }
        if (!let) btn |= IN_A;
      } else if (e && dist < me->b.hl + ehl + 2.5f) {
        /* armed only from its side or rear (outside its 90 degree front cone): the front is where its weapon
         * or its wedge is; a dome, a skirt or a spinning frame has no front */
        float lx, lz; to_local(e->h, e->x, e->z, me->b.x, me->b.z, &lx, &lz);
        int round = e->chassis == CH_SHELL || e->drive == DR_HOVER || e->drive == DR_MELTY;
        if ((round || lz < absf(lx)) && (tap_ok || (prev_a && dist > me->b.hl + ehl + 0.3f))) btn |= IN_A;   /* a hesitating think misses the moment */
      }
      break;
    }
    case WP_NONE: case WP_WEDGE:
      /* DIG IN on the approach inside 2 u, nose on: the plate scrapes in and gets under from slow (sim.c) */
      if (e && !melty && me->dig_cd == 0 && dist < 2.0f + ehl && in_front && !prev_a) btn |= IN_A;
      break;
    default: break;
  }
  /* the secondary weapon runs from the same button, so a tap weapon with a SAW on top just saws when touching */
  if (me->build.weapon2 == WP_SAW || me->build.weapon2 == WP_FLAME) { if (e && dist < 1.4f + ehl && in_front) btn |= IN_A; }
  out->buttons |= btn;
}

/* a lifter carrying someone, or a wedge with a bot up on its plate, steers to the nearest hazard square,
 * else the nearest wall (an open edge is a wall with no lexan: the carry goes over it) */
static int lifter_carry_target(const ai_t *a, const match_t *m, const bot_t *me, float *tx, float *tz) {
  const arena_def_t *ar = ai_arena(a, m);
  int holding = -1;
  for (int i = 0; i < m->nbots; i++) if (m->bots[i].lifted_by == a->slot + 1) holding = i;
  if (holding >= 0 && me->build.weapon != WP_LIFTER) holding = -1;
  if (holding >= 0) return 0;                       /* a lifter's carry is steered in ai_think (lifter_kill_target) */
  if (holding < 0 && me->riding && me->riding - 1 < m->nbots && m->bots[me->riding - 1].ride_up > 0.5f) holding = me->riding - 1;
  if (holding < 0) return 0;
  int best = -1; float bd = 1e9f;
  for (int i = 0; i < ar->nhaz && i < MAX_HAZ; i++) {
    const hazard_def_t *h = &ar->haz[i];
    if (!haz_danger(a, m, i, 3.0f, 0)) continue;
    float d = haz_dist(h, me->b.x, me->b.z);
    if (d < bd) { bd = d; best = i; }
  }
  if (best >= 0 && bd < 6.0f) { *tx = ar->haz[best].x; *tz = ar->haz[best].z; return 1; }
  float nx, nz, d;
  if (arena_nearest_wall(ar, me->b.x, me->b.z, &nx, &nz, &d)) { *tx = me->b.x - nx * (d + 1.0f); *tz = me->b.z - nz * (d + 1.0f); return 1; }
  return 0;
}

/* boost (8.3): a straight line over 4 u on ATTACK, or EVADE needing it; never on TREADS or LEG TRACKS */
static void maybe_boost(ai_t *a, const match_t *m, const ai_frame_t *f, pad_state_t *out) {
  const bot_t *me = &m->bots[a->slot];
  if (!skill_of(a)->boost || a->backoff_t > 0) return;                                 /* EASY never boosts; nobody boosts during a breath */
  if (!DRIVES[me->build.drive].boost || me->boost > 0 || me->boost_cd > 0 || me->inverted || !bot_can_drive(me)) return;
  if (a->prev_buttons & IN_L) return;
  const ai_view_t *e = a->target >= 0 ? &f->bots[a->target] : 0;
  if (!e) return;
  float dist = len2(e->x - me->b.x, e->z - me->b.z);
  int want = 0;
  if (a->goal == GOAL_ATTACK || a->goal == GOAL_HAZARD_PUSH) {
    float err = rtr_wrap_pi(rtr_atan2(a->tx - me->b.x, a->tz - me->b.z) - me->b.h);
    float line = len2(a->tx - me->b.x, a->tz - me->b.z);
    if (line > 4.0f && absf(err) < DEG(12)) want = 1;
    if (bot_is_wedge(me) && line > 2.5f && absf(err) < DEG(12)) want = 1;              /* a wedge rams with boost when the line is clear */
    if (weapon_spinner(me->build.weapon) && me->spin > 0.9f && line > 2.5f && absf(err) < DEG(12)) want = 1;   /* a spun up disc closes the gap an evader opened */
  }
  if (a->goal == GOAL_EVADE && bot_is_spinner(e) && e->spin > 0.7f && dist < 2.0f) want = 1;
  /* only on a straight line the steering already took: full throttle, small turn (not while stepping off a square) */
  if (!DRIVES[me->build.drive].strafe && (out->ly > -32000 || out->lx > 8000 || out->lx < -8000)) want = 0;
  if (want) out->buttons |= IN_L;
}

/* -- the frame ----------------------------------------------------- */
void ai_think(ai_t *a, const match_t *m, pad_state_t *out) {
  uint16_t prev = a->prev_buttons;
  memset(out, 0, sizeof *out);
  if (a->slot >= m->nbots || a->slot >= MAX_BOTS) { a->prev_buttons = 0; return; }
  const bot_t *me = &m->bots[a->slot];
  /* the match seed and the slot go into the rng once, so two matches on different seeds do not replay the
   * same hesitations and aim errors (the balance robin samples real variety) */
  if (!a->seeded) { a->rng ^= m->seed * 0x9e3779b9u + (uint32_t)a->slot * 0x7f4a7c15u; if (!a->rng) a->rng = 0x51ed270bu; a->seeded = 1; }
  ring_push(a, m);
  pins_update(a, m);
  if (!me->is_ops) observe(a, m);
  if (!me->alive) { a->prev_buttons = 0; return; }
  const ai_frame_t *f = ring_read(a, a->reaction);

  /* skill bookkeeping: EASY and MEDIUM back off after they land a hit on a person, and hold off for the
   * opening breath at the horn so a player still reading the screen is not hit before they have moved
   * (HARD charges); the EVADE budget */
  if (!me->is_ops) {
    a->vs_human = (uint8_t)human_in_match(m);
    const skill_t *sk = skill_of(a);
    if (m->frame <= 1 && sk->backoff > 0 && a->vs_human) a->backoff_t = AI_OPENING_BREATH;
    if (me->dealt > a->last_dealt + 0.5f && sk->backoff > 0) a->backoff_t = sk->backoff;
    a->last_dealt = me->dealt;
    if (a->backoff_t > 0) a->backoff_t--;
    if (a->evade_block_t > 0) a->evade_block_t--;
    if (a->goal == GOAL_EVADE && a->backoff_t == 0) { if (++a->evade_t >= 120) { a->evade_t = 0; a->evade_block_t = 90; } }
    else a->evade_t = 0;
  }
  /* think every sixth frame, staggered per seat */
  if (++a->think_t >= AI_THINK_EVERY || a->goal == GOAL_NONE) {
    a->think_t = 0;
    scores_t s;
    if (me->is_ops) ops_goals(a, m, &s);
    else { a->target = pick_target(a, m, f); score_goals(a, m, f, &s); }
    choose(a, &s);
    a->hesitate = (uint8_t)(rtr_rand_f(&a->rng) < skill_of(a)->hesitate);
    /* the aim error: a fresh offset on the attack point every think, EASY's wide enough to miss a hammer swing */
    float aim = skill_of(a)->aim;
    a->aim_x = (rtr_rand_f(&a->rng) * 2 - 1) * aim; a->aim_z = (rtr_rand_f(&a->rng) * 2 - 1) * aim;
  }

  a->retreat = 0;
  if (me->is_ops) {
    switch (a->goal) {
      case GOAL_NONE: break;                                                             /* parked behind the gate */
      case GOAL_OPS_GO: steer_to(a, m, me, a->tx, a->tz, out, 0); break;
      case GOAL_OPS_LIFT: steer_to(a, m, me, a->tx, a->tz, out, 0); out->buttons |= IN_A; break;
      case GOAL_OPS_CARRY: steer_to(a, m, me, a->tx, a->tz, out, 0); out->buttons |= IN_A; break;
      case GOAL_OPS_PUNISH: {
        steer_to(a, m, me, a->tx, a->tz, out, 0); out->buttons |= IN_A;
        if (!me->saw_on && !(prev & IN_X)) out->buttons |= IN_X;                        /* the saw comes on for the shove */
        break;
      }
      case GOAL_OPS_RETURN: default: {
        float d = len2(a->tx - me->b.x, a->tz - me->b.z);
        if (d > 0.5f) steer_to(a, m, me, a->tx, a->tz, out, 0);
        if (me->saw_on && !(prev & IN_X)) out->buttons |= IN_X;
        break;
      }
    }
    a->prev_buttons = out->buttons;
    return;
  }

  /* ridden by a wedge (DEEP-plan 6 step 13): reverse hard and turn off the plate, the weapon still live (a
   * flipper or a hammer can answer the bot under its nose) */
  if (me->ride_by && !me->inverted) {
    int rider = me->ride_by - 1;
    float side = 1.0f;
    if (rider < m->nbots) { float lx, lz; to_local(me->b.h, me->b.x, me->b.z, m->bots[rider].b.x, m->bots[rider].b.z, &lx, &lz); side = lx >= 0 ? -1.0f : 1.0f; (void)lz; }
    write_stick(out, side, -1.0f);
    weapons(a, m, f, out);
    a->prev_buttons = out->buttons;
    return;
  }

  /* a LIFTER with a bot on its forks drives it to the kill: where the lifter stands when the carried bot is
   * over the pit, just past the edge, or a throw short of the pulverizer; straight there and slowing in,
   * past the hazard and edge margins steer_to keeps (they are the point) */
  if (me->build.weapon == WP_LIFTER && me->carry_t > 0) {
    int held = -1;
    for (int i = 0; i < m->nbots; i++) if (m->bots[i].lifted_by == a->slot + 1) held = i;
    float px = 0, pz = 0;
    int kind = held >= 0 ? lifter_kill_target(a, m, me, &m->bots[held], &px, &pz) : LK_NONE;
    if (kind != LK_NONE) {
      const bot_t *v = &m->bots[held];
      float ux = px - me->b.x, uz = pz - me->b.z; norm2(&ux, &uz);
      float reach = len2(v->b.x - me->b.x, v->b.z - me->b.z);
      float back = kind == LK_PIT ? reach : kind == LK_PULVERIZER ? reach + 1.0f : 0;
      float tx = px - ux * back, tz = pz - uz * back;
      if (kind == LK_EDGE) { tx = px - ux * (0.6f + reach); tz = pz - uz * (0.6f + reach); }   /* px is 1 u past the edge: the carried bot 0.4 past it */
      float dx = tx - me->b.x, dz = tz - me->b.z, dist = len2(dx, dz);
      if (DRIVES[me->build.drive].strafe) {
        float k = dist < 0.15f ? 0 : rtr_clamp(dist, 0.3f, 1.0f);
        out->cam_yaw = 0; out->lx = (int16_t)(-dx / (dist + 1e-4f) * k * STICK); out->ly = (int16_t)(-dz / (dist + 1e-4f) * k * STICK);
      } else if (dist > 0.15f) {
        float err = rtr_wrap_pi(rtr_atan2(dx, dz) - me->b.h);
        float thr = (absf(err) < DEG(60) ? 1.0f : 0.5f) * rtr_clamp(dist, 0.3f, 1.0f);
        write_stick(out, 2.2f * err, thr);
      }
      weapons(a, m, f, out);
      a->prev_buttons = out->buttons;
      return;
    }
  }

  switch (a->goal) {
    case GOAL_ATTACK: {
      float tx = a->tx, tz = a->tz;
      /* the weak zone point from the last think; close in, drive straight at the enemy as the ring shows it;
       * a lifter that holds someone steers to a hazard or a wall instead */
      if (a->target >= 0) {
        const ai_view_t *e = &f->bots[a->target];
        if (lifter_carry_target(a, m, me, &tx, &tz)) { }
        else if (len2(e->x - me->b.x, e->z - me->b.z) < 1.3f && !(bot_is_wedge(me) && len2(a->tx - e->x, a->tz - e->z) > 1.0f)) { tx = e->x; tz = e->z; }
      }
      steer_to(a, m, me, tx, tz, out, 1);
      weapons(a, m, f, out);
      maybe_boost(a, m, f, out);
      break;
    }
    case GOAL_EVADE:
      steer_to(a, m, me, a->tx, a->tz, out, 0);
      weapons(a, m, f, out);
      maybe_boost(a, m, f, out);
      break;
    case GOAL_HAZARD_PUSH:
      steer_to(a, m, me, a->tx, a->tz, out, 0);
      weapons(a, m, f, out);
      maybe_boost(a, m, f, out);
      break;
    case GOAL_RECOVER:
      if (me->inverted) { weapons(a, m, f, out); }                                       /* B or the DRUM's A; no drive */
      else {
        a->retreat = weapon_spinner(me->build.weapon);                                   /* back off facing them, spinning up */
        steer_to(a, m, me, a->tx, a->tz, out, 0);
        out->buttons |= IN_A;                                                            /* hold the weapon */
      }
      break;
    case GOAL_BAIT:
      steer_to(a, m, me, a->tx, a->tz, out, 0);
      weapons(a, m, f, out);
      break;
    case GOAL_PICKUP:
      steer_to(a, m, me, a->tx, a->tz, out, 1);
      weapons(a, m, f, out);
      break;
    case GOAL_TAUNT:
      if (a->taunt_t > 0) { a->taunt_t--; out->buttons |= IN_X; }
      if (weapon_spinner(me->build.weapon) || me->build.drive == DR_MELTY) out->buttons |= IN_A;
      if (a->taunt_t == 0) a->goal = GOAL_NONE;                                          /* rescore next frame */
      break;
    case GOAL_WIGGLE: {
      a->wiggle_t++;
      float thr = ((a->wiggle_t / 15) & 1) ? -1.0f : 1.0f;
      write_stick(out, 0, thr);
      weapons(a, m, f, out);
      break;
    }
    case GOAL_NONE: default:
      weapons(a, m, f, out);
      break;
  }
  a->prev_buttons = out->buttons;
}

/* -- build choice (8.4) -------------------------------------------- */
static int human_has_wedge(const build_t *h) { return h->chassis == CH_WEDGE || h->weapon == WP_WEDGE; }
static int human_has_spinner(const build_t *h) { return weapon_spinner(h->weapon) || h->drive == DR_MELTY; }

/* MEDIUM's counter-pick table; weapon_hint overrides the human's weapon when HARD remembers a favourite */
static void counter_pick(build_t *b, const build_t *human, int weapon_hint) {
  build_t h = human ? *human : (build_t){ CH_BOX, DR_TWO, WP_FLIPPER, WP_NONE, AR_ALU, 0, 0 };
  if (weapon_hint > 0) h.weapon = (uint8_t)(weapon_hint - 1);
  if (human_has_wedge(&h)) build_preset(b, PRE_CHOP);                                    /* spinner against a wedge */
  else if (human_has_spinner(&h)) build_preset(b, PRE_ROLL_OUT);                         /* wedge against a spinner */
  else if (h.weapon == WP_FLIPPER) { b->chassis = CH_BOX; b->drive = DR_SIX; b->weapon = WP_HAMMER; b->weapon2 = WP_NONE; b->armour = AR_STEEL; b->decal = DC_NONE; }   /* SIX WHEEL plus HAMMER */
  else if (h.drive == DR_TWO) build_preset(b, PRE_LAUNCH);                               /* flipper against TWO WHEEL */
  else if (h.weapon == WP_HAMMER) build_preset(b, PRE_CHOP);                             /* LOWBOY against a hammer */
  else build_preset(b, PRE_LAUNCH);
}

/* fit the budget: armour down, then the weapon down, never below NONE */
static void fit_budget(build_t *b) {
  for (int guard = 0; guard < 16 && build_cost(b) > build_budget(b); guard++) {
    if (b->armour > AR_NONE) { b->armour--; continue; }
    if (b->weapon > WP_NONE) { b->weapon--; continue; }
    break;
  }
  if (b->weapon == WP_HORIZ && b->chassis == CH_WEDGE) b->weapon = WP_DRUM;
  if (b->drive == DR_MELTY) { b->weapon = WP_NONE; b->weapon2 = WP_NONE; }
}

void ai_pick_build(build_t *b, int difficulty, const build_t *human_last, const uint8_t habits[8], uint32_t *rng) {
  static const int easy[3] = { PRE_ROLL_OUT, PRE_LAUNCH, PRE_STOMP };
  uint32_t local = 0x2545f491u; if (!rng) rng = &local;
  if (difficulty == AI_EASY) build_preset(b, easy[rtr_rand_n(rng, 3)]);
  else if (difficulty == AI_MEDIUM) counter_pick(b, human_last, 0);
  else {
    int hint = habits && habits[HB_FAV_STREAK] >= 2 ? habits[HB_FAV_WEAPON] : 0;
    counter_pick(b, human_last, hint);
    if (habits) {
      if (habits[HB_FLIPS] >= 2) b->chassis = CH_LOWBOY;                                 /* flipped twice: get low */
      if (habits[HB_HAZARD_PUSH] >= 1 || habits[HB_BOOSTS] >= 3) b->drive = DR_TREAD;    /* pushed into a hazard or a frequent booster: treads */
    }
    fit_budget(b);
  }
  b->paint = 1; b->decal = DC_NONE;
  if (!build_legal(b, 2)) build_preset(b, PRE_LAUNCH);             /* 2: legal in a 1v1 too, so never CRUSHER */
}

static int same_name(const build_t *a, const build_t *b) {
  int ma = a->drive == DR_MELTY, mb = b->drive == DR_MELTY;
  return a->chassis == b->chassis && (ma || mb ? ma == mb : a->weapon == b->weapon);
}

void ai_pick_build_nth(build_t *b, int difficulty, const build_t *human_last, const uint8_t habits[8], uint32_t *rng, const build_t *others, int nothers) {
  ai_pick_build(b, difficulty, human_last, habits, rng);
  int clash = 0;
  for (int i = 0; i < nothers; i++) if (same_name(b, &others[i])) clash = 1;
  if (!clash) return;
  /* the next seats take the other presets in turn, starting from a random one, skipping any pick that
   * would share a name; EASY stays on its three gentle presets */
  int start = rtr_rand_n(rng, PRE_RANDOM);
  for (int k = 0; k < PRE_RANDOM; k++) {
    int p = (start + k) % PRE_RANDOM;
    if (difficulty == AI_EASY && p != PRE_ROLL_OUT && p != PRE_LAUNCH && p != PRE_STOMP) continue;
    build_t t; build_preset(&t, p); t.paint = 1; t.decal = DC_NONE;
    int ok = build_legal(&t, 2);
    for (int i = 0; i < nothers && ok; i++) if (same_name(&t, &others[i])) ok = 0;
    if (ok) { *b = t; return; }
  }
  /* seven seats would be needed to run out of presets; a RANDOM BOT then fills the slot */
  build_random(b, rng, 2); b->paint = 1;
  if (!build_legal(b, 2)) build_preset(b, PRE_LAUNCH);
}

/* -- habits (8.4): fold a finished match into save.habits ---------- */
static uint8_t sat_add(uint8_t v, int d) { int r = (int)v + d; return (uint8_t)(r > 255 ? 255 : r < 0 ? 0 : r); }

void ai_learn(uint8_t habits[8], const match_t *m, int human_slot) {
  if (!habits || !m) return;
  if (human_slot < 0 || human_slot >= m->nbots) {
    for (int i = 0; i < m->nbots; i++) if (!m->bots[i].is_ai && !m->bots[i].is_ops) { human_slot = i; break; }
  }
  if (human_slot < 0 || human_slot >= m->nbots) { memset(&OBS, 0, sizeof OBS); return; }
  const bot_t *h = &m->bots[human_slot];
  habits[HB_FLIPS] = sat_add(habits[HB_FLIPS], h->flips_landed);
  habits[HB_PINNED] = sat_add(habits[HB_PINNED], OBS.pins);
  habits[HB_HAZARD_PUSH] = sat_add(habits[HB_HAZARD_PUSH], OBS.hazard_pushes);
  uint8_t wid = (uint8_t)(h->build.drive == DR_MELTY ? WP_HORIZ + 1 : h->build.weapon + 1);
  if (habits[HB_FAV_WEAPON] == wid) habits[HB_FAV_STREAK] = sat_add(habits[HB_FAV_STREAK], 1);
  else { habits[HB_FAV_WEAPON] = wid; habits[HB_FAV_STREAK] = 1; }
  habits[HB_BOOSTS] = habits[HB_MATCHES] == 0 ? (uint8_t)(OBS.boosts > 255 ? 255 : OBS.boosts) : (uint8_t)(((int)habits[HB_BOOSTS] * 3 + OBS.boosts) / 4);
  habits[HB_MATCHES] = sat_add(habits[HB_MATCHES], 1);
  if (m->over && m->winner == (uint8_t)human_slot) habits[HB_HUMAN_WINS] = sat_add(habits[HB_HUMAN_WINS], 1);
  memset(&OBS, 0, sizeof OBS);
}

/* -- the practice bot panel (3.12) --------------------------------- */
int ai_garage_tick(build_t *cur, const build_t *pick, int frame, int players, uint32_t *rng) {
  if (frame < 0) return 0;
  if (frame == 300) { *cur = *pick; return 2; }
  if (frame > 300 || frame % 18 != 0) return 0;
  int row = (frame / 18) % 4;                                                             /* CHASSIS, DRIVE, WEAPON, ARMOUR in turn */
  int n = row_count(cur, row);
  for (int tries = 0; tries < 8; tries++) {
    int o = rtr_rand_n(rng, n);
    if (o == build_get(cur, row)) continue;
    if (row == ROW_DRIVE && o == DR_MELTY) continue;
    if (!option_legal(cur, row, o, players, 0)) continue;
    build_set(cur, row, o);
    if (!build_mount_ok(cur)) cur->weapon2 = WP_NONE;
    return 1;
  }
  return 0;
}

/* -- the attract driver (8.6) -------------------------------------- */
void ai_attract_setup(match_setup_t *s, ai_t ais[2], uint32_t frame_seed) {
  memset(s, 0, sizeof *s);
  uint32_t rng = frame_seed * 2654435761u + 0x9e3779b9u;
  s->mode = MODE_DEATHMATCH; s->arena = AR_BAY; s->players = 2; s->comeback = 0; s->fight_len_s = 75;
  s->seed = rng ? rng : 7;
  int p0 = rtr_rand_n(&rng, PRE_RANDOM), p1 = rtr_rand_n(&rng, PRE_RANDOM - 1);
  if (p1 >= p0) p1++;                                                                     /* distinct presets */
  build_preset(&s->builds[0], p0); s->builds[0].paint = 0;
  build_preset(&s->builds[1], p1); s->builds[1].paint = 0;
  for (int i = 0; i < 2; i++) {
    s->is_ai[i] = 1; s->ai_level[i] = AI_MEDIUM; s->team[i] = (uint8_t)i;
    ai_init(&ais[i], AI_MEDIUM, i);
    ais[i].rng ^= rng + (uint32_t)i * 0x7f4a7c15u;
    ai_bind(&ais[i], i);
  }
}
