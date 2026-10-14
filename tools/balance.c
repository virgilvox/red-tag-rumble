/* tools/balance.c: the native balance tool (spec 15.7, 6.6, 19). Links sim.c,
 * bot.c, arena.c and ai.c with -DRTR_HEADLESS and nothing from three.c, hud.h
 * or wasmcart. Owned by the sim agent (items 4, 19).
 *
 *   build/balance --parts
 *   build/balance --robin N [arenas=all|bay|laser|pile|dock|shop|roof] [hazards=on|off|both]
 *                           [driver=builtin|ai] [mode=0..5] [quiet=1]
 *   build/balance --check-arenas
 *   build/balance --replay tools/scripts/2p-fight.json --setup "<the [sim] setup line the cart logged>" [frames=N]
 *   build/balance --trace arena presetA presetB [frames] [hazards=off]
 *   build/balance --hashcheck
 *   build/balance --make-drive bay|laser|pile|dock|shop|roof tools/scripts/drive-<arena>.json
 *   build/balance --make-fight tools/scripts/2p-fight.json --setup "<the cart's [sim] setup line>" [driver=ai|builtin]
 *   build/balance --modes [N]          every mode, arena and seat count must end with no [stuck]
 *
 * The driver: --robin plays each match with two MEDIUM AIs through ai_think
 * (driver=ai, the default: it is the opponent a player meets); driver=builtin
 * is this file's own steer-and-fire stand-in, which --make-fight records as the
 * human seats of the rig's fight scripts and --hashcheck still uses. The replay
 * never drives: it feeds the script's pads. Both drivers are deterministic. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "sim.h"
#include "bot.h"
#include "arena.h"
#include "ai.h"

static int usage(void) {
  fprintf(stderr, "usage: balance --parts | --robin N [arenas=...] [hazards=on|off|both] [driver=ai|builtin] [mode=M] | --check-arenas |\n"
                  "               --replay script.json --setup \"line\" [frames=N] [verbose=1|2] | --make-fight script.json --setup \"line\" |\n"
                  "               --trace arena a b [frames] [driver=ai|builtin] | --hashcheck | --make-drive arena out.json | --modes [N]\n");
  return 2;
}

static const char *ARENA_KEY[6] = { "bay", "laser", "pile", "dock", "shop", "roof" };
static const char *KO_NAME[9] = { "HP ZERO", "KO BY FLIP", "PITTED", "OFF THE ROOF", "COUNTED OUT", "DECK COUNT", "JUDGES", "HILL", "NONE" };

/* -- the builtin driver ------------------------------------------- */
/* camper: a human stand-in who gives up at low HP and parks in a far corner (or on THE DOCK's deck),
 * which is what exercises the COUNTED OUT and DECK COUNT rules in the robin; aim: a per match approach
 * offset so seeds matter */
typedef struct { uint32_t rng; float px, pz, dealt, aim; int stuck_t, reverse_t, wiggle_t, boost_t, saw_t, side, camper, init, no_camp; } drv_t;

static float wrap_pi(float a) { return rtr_wrap_pi(a); }

static int nearest_enemy(const match_t *m, int slot) {
  const bot_t *me = &m->bots[slot];
  int best = -1; float bd = 1e9f;
  for (int i = 0; i < m->nbots; i++) {
    const bot_t *o = &m->bots[i];
    if (i == slot || !o->alive || o->is_ops || o->hp_max == 0) continue;
    if (m->mode == MODE_2V2 && o->team == me->team) continue;
    float d = sqrtf((o->b.x - me->b.x) * (o->b.x - me->b.x) + (o->b.z - me->b.z) * (o->b.z - me->b.z));
    if (d < bd) { bd = d; best = i; }
  }
  return best;
}

static void drive_builtin(const match_t *m, int slot, drv_t *d, pad_state_t *out) {
  const bot_t *me = &m->bots[slot];
  memset(out, 0, sizeof *out);
  if (!me->alive) return;
  if (!d->init) { d->init = 1; d->camper = !d->no_camp && rtr_rand_n(&d->rng, 6) == 0; d->side = 1; }
  if (m->frame % 90 == 0) d->aim = (rtr_rand_f(&d->rng) - 0.5f) * 0.6f;
  int e = nearest_enemy(m, slot);
  const arena_def_t *ar = sim_arena(m);
  float tx = 0, tz = 0;
  if (e >= 0) { tx = m->bots[e].b.x; tz = m->bots[e].b.z; }
  if (d->camper && me->hp < 40 && e >= 0 && m->mode != MODE_HILL) {
    /* camp: the deck if there is one, else the corner furthest from the enemy, then sit still */
    float cx = tx < 0 ? ar->w * 0.5f - 2.0f : -ar->w * 0.5f + 2.0f, cz = tz < 0 ? ar->d * 0.5f - 2.0f : -ar->d * 0.5f + 2.0f;
    for (int k = 0; k < ar->nhaz; k++) if (ar->haz[k].type == HZ_DECK) { cx = ar->haz[k].x + (tx < 0 ? 4.0f : -4.0f); cz = ar->haz[k].z; }
    if (fabsf(cx - ar->ops_zone[0]) < 3.0f && fabsf(cz - ar->ops_zone[1]) < 3.0f) cz = -cz;
    float ddx = cx - me->b.x, ddz = cz - me->b.z;
    if (ddx * ddx + ddz * ddz < 1.0f) return;          /* parked: no wiggle */
    tx = cx; tz = cz;
  }
  /* in KING OF THE HILL the hill is the target */
  if (m->mode == MODE_HILL) { tx = ar->hill[m->hill][0]; tz = ar->hill[m->hill][1]; }
  float dx = tx - me->b.x, dz = tz - me->b.z, dist = sqrtf(dx * dx + dz * dz);
  int w = me->build.weapon, melty = me->build.drive == DR_MELTY;
  float fx = rtr_sin(me->b.h), fz = rtr_cos(me->b.h);
  float ahead = dx * fx + dz * fz, side = dx * rtr_cos(me->b.h) - dz * rtr_sin(me->b.h);
  /* a stuck detector: under 0.3 u of progress and no damage dealt in 120 frames, back off for 60 and
   * come in from the other side; this is what keeps a push stalemate in a corner from wedging a bot
   * against a wall (the sim would log [stuck]) */
  float moved = sqrtf((me->b.x - d->px) * (me->b.x - d->px) + (me->b.z - d->pz) * (me->b.z - d->pz));
  if (++d->stuck_t >= 45) {
    if (moved < 0.15f && (me->dealt <= d->dealt + 0.5f || (me->pinned & 1)) && d->reverse_t == 0 && bot_can_drive(me)) { d->reverse_t = 60; d->side = -d->side; if (!d->side) d->side = 1; }
    d->stuck_t = 0; d->px = me->b.x; d->pz = me->b.z; d->dealt = me->dealt;
  }
  float nx, nz, wdist;
  int near_wall = arena_nearest_wall(ar, me->b.x, me->b.z, &nx, &nz, &wdist) && wdist < 1.2f;
  /* inverted: self right when the build can, else wiggle */
  if (me->inverted) {
    const weapon_def_t *wd = &WEAPONS[w];
    if (wd->selfright == SR_B) { if ((m->frame % 40) < 2) out->buttons |= IN_B; return; }
    if (wd->selfright == SR_DRUM) { out->buttons |= IN_A; if (me->spin > 0.6f && (m->frame % 30) < 2) out->buttons |= IN_A; else if (me->spin > 0.6f) out->buttons &= ~IN_A; return; }
  }
  /* evade a spun up spinner unless I am a wedge, on treads, or a spinner myself */
  int evade = 0;
  if (e >= 0) {
    const bot_t *o = &m->bots[e];
    int ospin = (o->build.weapon == WP_HORIZ || o->build.weapon == WP_DRUM) && o->spin > 0.7f;
    int tough = me->build.chassis == CH_WEDGE || me->build.drive == DR_TREAD || w == WP_HORIZ || w == WP_DRUM || melty;
    if (ospin && dist < 3.0f && !tough && me->hp < 70) evade = 1;
  }
  float want = rtr_atan2(dx, dz) + (dist > 2.5f ? d->aim : 0);
  if (evade) want = rtr_atan2(-dx, -dz);
  float err = wrap_pi(want - me->b.h);
  if (me->build.drive == DR_HOVER || melty) {
    /* a world direction, the rig's camera yaw is 0 */
    float k = evade ? -1.0f : 1.0f;
    float ux = dist > 0.05f ? dx / dist : 0, uz = dist > 0.05f ? dz / dist : 1;
    if (d->reverse_t > 0) {
      /* boxed in: strafe across the line to the enemy, off any wall */
      d->reverse_t--; k = 1.0f;
      float px = -uz * (float)d->side, pz = ux * (float)d->side;
      ux = px + (near_wall ? nx : 0); uz = pz + (near_wall ? nz : 0);
    }
    else if (near_wall) { ux += nx * 0.6f; uz += nz * 0.6f; }     /* never press into a wall */
    out->lx = (int16_t)(-32767.0f * rtr_clamp(k * ux, -1, 1)); out->ly = (int16_t)(-32767.0f * rtr_clamp(k * uz, -1, 1));   /* stick right is -x against cam_yaw 0 */
    out->cam_yaw = 0;
  } else {
    float turn = err * 2.2f; if (turn > 1) turn = 1; if (turn < -1) turn = -1;
    float thr = fabsf(err) < 1.05f ? 1.0f : 0.3f;
    if (d->reverse_t > 0) {
      d->reverse_t--; thr = -0.8f; turn = (float)d->side;
      /* never back into the lexan: with a wall behind, drive out forward on full lock instead */
      if (near_wall && (fx * nx + fz * nz) > 0.3f) thr = 0.6f;
    }
    /* pressing into a wall with the enemy not ahead is how a bot gets wedged: turn off it */
    if (near_wall && (fx * -nx + fz * -nz) > 0.7f && ahead < 0.8f) { turn = (fx * nz - fz * nx) > 0 ? 1.0f : -1.0f; thr = 0.5f; }
    /* a wedge or a lifter aims at the side; a flipper comes straight; everyone slows into contact a little */
    if (dist < 1.2f && (w == WP_HAMMER || w == WP_SPIKE || w == WP_CRUSHER)) thr = 0.4f;
    out->lx = (int16_t)(-32767.0f * turn); out->ly = (int16_t)(-32767.0f * thr);   /* a positive heading error is a left turn: stick left */
  }
  /* wiggle so a parked bot is never counted out */
  if (me->countout > 300) { d->wiggle_t++; out->ly = (int16_t)(((d->wiggle_t / 20) & 1) ? 32767 : -32767); }
  /* weapons (8.3) */
  switch (melty ? WP_HORIZ : w) {
    case WP_HORIZ: case WP_DRUM: out->buttons |= IN_A; break;
    case WP_SAW: if (!me->saw_on && dist < 1.5f) out->buttons |= IN_A; if (me->saw_on && dist > 4.0f) out->buttons |= IN_A; break;
    case WP_FLIPPER: if (ahead > 0.3f && ahead < 1.1f && fabsf(side) < 0.45f && me->cooldown == 0) out->buttons |= IN_A; break;
    case WP_HAMMER: if (ahead > 0.2f && ahead < 1.3f && fabsf(side) < 0.5f && me->cooldown == 0 && me->windup == 0) out->buttons |= IN_A; break;
    case WP_SPIKE: if (ahead > 0.3f && ahead < 1.1f && fabsf(side) < 0.4f && me->cooldown == 0) out->buttons |= IN_A; break;
    case WP_FLAME: if (ahead > 0 && dist < 2.0f && fabsf(side) < 1.0f) out->buttons |= IN_A; break;
    case WP_LIFTER: if (dist < 1.3f && ahead > 0) out->buttons |= IN_A; break;
    case WP_CRUSHER: if (ahead > 0.3f && ahead < 1.1f && fabsf(side) < 0.4f && me->cooldown == 0) out->buttons |= IN_A; break;
    default: break;
  }
  if (me->build.weapon2 == WP_SAW && !me->saw_on && dist < 1.5f) out->buttons |= IN_A;
  /* boost on a straight line over 4 u */
  if (DRIVES[me->build.drive].boost && dist > 4.0f && fabsf(err) < 0.3f && me->boost_cd == 0 && !evade) { if (++d->boost_t % 7 == 0) out->buttons |= IN_L; }
  /* the odd taunt when the enemy is inverted */
  if (e >= 0 && m->bots[e].inverted && (m->frame % 180) == 0 && dist > 2.0f) out->buttons |= IN_X;
  /* in 2V2 the hazard seats are not used; in FFA a 20 percent roll presses nothing else */
  (void)ar;
}

/* -- one match ----------------------------------------------------- */
typedef struct { int winner, how, frames, stuck; int ko_cause[9]; int events[EV_TYPE_COUNT]; } result_t;

static int g_use_ai;
static int g_verbose;
static uint32_t g_trace_seed;

static void run_match(match_setup_t *s, result_t *r, int trace) {
  static match_t m; static ai_t ais[MAX_BOTS]; drv_t drv[MAX_BOTS];
  sim_init(&m, s);
  memset(drv, 0, sizeof drv); memset(r, 0, sizeof *r);
  for (int i = 0; i < m.nbots; i++) { ai_init(&ais[i], AI_MEDIUM, i); ai_bind(&ais[i], i); drv[i].rng = s->seed ^ (uint32_t)(i * 977); drv[i].px = m.bots[i].b.x; drv[i].pz = m.bots[i].b.z; }
  int steps = 0, max_steps = (int)(m.clock_s * 60) + 1200 + 60 * 60;   /* the clock plus slack; KOTH respawns run longer */
  while (!sim_over(&m) && steps < max_steps) {
    for (int i = 0; i < m.nbots; i++) {
      bot_t *b = &m.bots[i];
      if (b->is_ops || b->hp_max == 0) continue;
      if (g_use_ai) ai_think(&ais[i], &m, &b->in); else drive_builtin(&m, i, &drv[i], &b->in);
    }
    sim_step(&m);
    for (int k = 0; k < m.nev; k++) {
      const event_t *e = &m.ev[k];
      r->events[e->type]++;
      if (e->type == EV_KO || e->type == EV_MINI_KO) r->ko_cause[e->zone < 9 ? e->zone : 8]++;
      if (trace && (e->type == EV_HIT || e->type == EV_RAM || e->type == EV_KO || e->type == EV_MINI_KO || e->type == EV_LAUNCH || e->type == EV_INVERT || e->type == EV_HAZ_HIT || e->type == EV_OPS_WAKE || e->type == EV_CLASH || e->type == EV_SPIN_WALL || e->type == EV_RED_TAG || e->type == EV_PICKUP || e->type == EV_STUCK || e->type == EV_SELFRIGHT || e->type == EV_COUNT_TICK))
        printf("  f=%d ev=%d a=%d b=%d zone=%d dmg=%.1f at (%.2f, %.2f)\n", m.frame, e->type, e->a, e->b, e->zone, e->dmg, e->x, e->z);
    }
    m.nev = 0;
    steps++;
    if (trace && m.frame % (trace > 1 ? 10 : 60) == 0) {
      printf("f=%4d t=%5.1f h=%08x", m.frame, m.clock_s, sim_hash(&m));
      for (int i = 0; i < m.nbots; i++) { const bot_t *b = &m.bots[i]; if (b->hp_max == 0) continue; printf(" | %d:(%.1f,%.1f,%.2f) hp %.0f sp %.2f%s%s%s", i, b->b.x, b->b.z, b->b.y, b->hp, b->spin, b->inverted ? " INV" : "", b->airborne ? " AIR" : "", b->alive ? "" : " DEAD"); if (b->stuck_t) printf(" thr %.2f h %.2f pin %d st %d", b->drive_t, b->b.h, b->pinned, b->stuck_t); }
      printf("\n");
    }
  }
  r->winner = m.winner == 255 ? -1 : m.winner; r->how = m.over ? m.win_how : WIN_NONE; r->frames = m.frame; r->stuck = m.stuck_frames;
  if (!m.over) r->how = WIN_NONE;
}

/* -- --robin ------------------------------------------------------- */
/* fight length tallies in frames: per preset (every match it played) and per pair (both sides) */
typedef struct { long sum; int n, lo, hi; } len_t;
static void len_add(len_t *l, int frames) { l->sum += frames; l->n++; if (!l->lo || frames < l->lo) l->lo = frames; if (frames > l->hi) l->hi = frames; }

static int robin(int n, int arena_mask, int haz_mode, int mode) {
  int wins[6][6][6]; int wins2[6][6][6]; int plays[6][6][6]; int ko_cause[9] = { 0 }; int stuck = 0; long frames = 0; int matches = 0, stalls = 0;
  int wins_all[6] = { 0 }, plays_all[6] = { 0 };
  len_t len_pre[6], len_pair[6][6], len_arena[6], len_all; memset(len_pre, 0, sizeof len_pre); memset(len_pair, 0, sizeof len_pair); memset(len_arena, 0, sizeof len_arena); memset(&len_all, 0, sizeof len_all);
  int judges = 0;
  int wedge_v_horiz_w = 0, wedge_v_horiz_n = 0, hover_v_tread_bay_w = 0, hover_v_tread_bay_n = 0, hover_v_tread_laser_w = 0, hover_v_tread_laser_n = 0;
  memset(wins, 0, sizeof wins); memset(wins2, 0, sizeof wins2); memset(plays, 0, sizeof plays);
  clock_t t0 = clock();
  for (int ar = 0; ar < 6; ar++) {
    if (!(arena_mask & (1 << ar))) continue;
    for (int hz = 0; hz < 2; hz++) {
      if (haz_mode == 1 && hz == 1) continue;   /* on only */
      if (haz_mode == 2 && hz == 0) continue;   /* off only */
      for (int a = 0; a < 6; a++) for (int b = 0; b < 6; b++) for (int k = 0; k < n; k++) {
        match_setup_t s; memset(&s, 0, sizeof s);
        s.mode = (uint8_t)mode; s.arena = (uint8_t)ar; s.players = 2; s.fight_len_s = 75; s.comeback = 0; s.hazards_off = (uint8_t)hz;
        build_preset(&s.builds[0], a); build_preset(&s.builds[1], b);
        s.is_ai[0] = s.is_ai[1] = 1; s.ai_level[0] = s.ai_level[1] = AI_MEDIUM;
        s.seed = 1000003u * (uint32_t)(ar * 6 * 6 * 2 + hz * 36 + a * 6 + b) + 7u * (uint32_t)k + 1u;
        result_t r; run_match(&s, &r, 0);
        matches++; frames += r.frames; stuck += r.stuck;
        for (int c = 0; c < 9; c++) ko_cause[c] += r.ko_cause[c];
        if (r.how == WIN_NONE) stalls++;
        if (r.how == WIN_JUDGES) judges++;
        len_add(&len_pre[a], r.frames); if (a != b) len_add(&len_pre[b], r.frames);
        len_add(&len_pair[a < b ? a : b][a < b ? b : a], r.frames); len_add(&len_arena[ar], r.frames); len_add(&len_all, r.frames);
        plays[ar][a][b]++; plays_all[a]++; plays_all[b]++;
        if (r.winner == 0) { wins[ar][a][b]++; wins_all[a]++; }
        else if (r.winner == 1) { wins2[ar][a][b]++; wins_all[b]++; }
        else { /* a draw counts half to each */ }
        if (a == PRE_ROLL_OUT && b == PRE_CHOP) { wedge_v_horiz_n++; if (r.winner == 0) wedge_v_horiz_w++; }
        if (a == PRE_CHOP && b == PRE_ROLL_OUT) { wedge_v_horiz_n++; if (r.winner == 1) wedge_v_horiz_w++; }
        if (ar == AR_BAY) { if (a == PRE_TOAST && b == PRE_ROLL_OUT) { hover_v_tread_bay_n++; if (r.winner == 0) hover_v_tread_bay_w++; } if (a == PRE_ROLL_OUT && b == PRE_TOAST) { hover_v_tread_bay_n++; if (r.winner == 1) hover_v_tread_bay_w++; } }
        if (ar == AR_LASER) { if (a == PRE_TOAST && b == PRE_ROLL_OUT) { hover_v_tread_laser_n++; if (r.winner == 0) hover_v_tread_laser_w++; } if (a == PRE_ROLL_OUT && b == PRE_TOAST) { hover_v_tread_laser_n++; if (r.winner == 1) hover_v_tread_laser_w++; } }
        if (g_verbose) printf("%-5s haz=%d %-8s vs %-8s seed %u: winner %d by %s in %d frames%s\n", ARENA_KEY[ar], !hz, PRESETS[a].name, PRESETS[b].name, s.seed, r.winner, KO_NAME[r.how], r.frames, r.stuck ? " [stuck]" : "");
      }
    }
  }
  double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
  /* the matrices: row a at spawn side 1, column b at side 2, cell is the win percent of the row preset */
  for (int ar = 0; ar < 6; ar++) {
    if (!(arena_mask & (1 << ar))) continue;
    printf("\n%s: win percent of the row preset (side 1) against the column preset (side 2)\n          ", ARENAS[ar].name);
    for (int b = 0; b < 6; b++) printf(" %8s", PRESETS[b].name);
    printf("\n");
    for (int a = 0; a < 6; a++) { printf("%-10s", PRESETS[a].name); for (int b = 0; b < 6; b++) printf(" %7.0f%%", plays[ar][a][b] ? 100.0 * wins[ar][a][b] / plays[ar][a][b] : 0); printf("\n"); }
  }
  /* the fight length table (4.7 retune: a two bot fight should average 35 to 60 s) */
  printf("\nfight length, seconds (mean, min, max):\n");
  for (int a = 0; a < 6; a++) if (len_pre[a].n) printf("  %-10s %5.1f  %5.1f  %5.1f   over %d\n", PRESETS[a].name, len_pre[a].sum / 60.0 / len_pre[a].n, len_pre[a].lo / 60.0, len_pre[a].hi / 60.0, len_pre[a].n);
  for (int ar = 0; ar < 6; ar++) if (len_arena[ar].n) printf("  %-15s %5.1f  %5.1f  %5.1f\n", ARENAS[ar].name, len_arena[ar].sum / 60.0 / len_arena[ar].n, len_arena[ar].lo / 60.0, len_arena[ar].hi / 60.0);
  printf("  %-15s %5.1f  %5.1f  %5.1f   %d of %d went to the judges\n", "ALL", len_all.n ? len_all.sum / 60.0 / len_all.n : 0, len_all.lo / 60.0, len_all.hi / 60.0, judges, matches);
  printf("  mean seconds per pair:\n          "); for (int b = 0; b < 6; b++) printf(" %8s", PRESETS[b].name); printf("\n");
  for (int a = 0; a < 6; a++) { printf("  %-8s", PRESETS[a].name); for (int b = 0; b < 6; b++) { const len_t *l = &len_pair[a < b ? a : b][a < b ? b : a]; if (b < a) printf("         "); else printf(" %8.1f", l->n ? l->sum / 60.0 / l->n : 0); } printf("\n"); }
  /* the pair matrix over both sides and every arena: row preset's win percent against the column preset */
  printf("\nwin percent of the row preset against the column preset (both sides, every arena; a draw is nobody's):\n          ");
  for (int b = 0; b < 6; b++) printf(" %8s", PRESETS[b].name); printf("\n");
  for (int a = 0; a < 6; a++) {
    printf("%-10s", PRESETS[a].name);
    for (int b = 0; b < 6; b++) {
      int w = 0, p = 0;
      for (int ar = 0; ar < 6; ar++) { w += wins[ar][a][b] + wins2[ar][b][a]; p += plays[ar][a][b] + plays[ar][b][a]; }
      printf(" %7.0f%%", p ? 100.0 * w / p : 0);
    }
    printf("\n");
  }
  printf("\noverall (both sides, every arena):\n");
  /* the 8.6 guardrails measure the AI as much as the parts, so they fail the run only with driver=ai;
   * with the builtin stand-in driver they are printed as advice (GUARD) */
  int fail = 0, guard = 0;
  const char *G = g_use_ai ? "FAIL" : "GUARD";
  for (int a = 0; a < 6; a++) {
    double pct = plays_all[a] ? 100.0 * wins_all[a] / plays_all[a] : 0;
    int bad = pct > 65.0 || pct < 35.0;
    printf("  %-10s %5.1f%% of %d%s%s\n", PRESETS[a].name, pct, plays_all[a], bad ? "   " : "", bad ? G : "");
    if (bad) guard = 1;
  }
  double mean_s = matches ? (double)frames / matches / 60.0 : 0;
  if (mean_s < 35.0 || mean_s > 60.0) { printf("  mean fight %.1f s is outside 35 to 60   %s\n", mean_s, G); guard = 1; }
  double wvh = wedge_v_horiz_n ? 100.0 * wedge_v_horiz_w / wedge_v_horiz_n : 0;
  printf("  WEDGE (ROLL OUT) vs HORIZONTAL (CHOP): %.0f%% of %d%s%s\n", wvh, wedge_v_horiz_n, wedge_v_horiz_n && wvh < 40 ? "   over 40 " : "", wedge_v_horiz_n && wvh < 40 ? G : "");
  if (wedge_v_horiz_n && wvh < 40) guard = 1;
  if (hover_v_tread_bay_n) { double p = 100.0 * hover_v_tread_bay_w / hover_v_tread_bay_n; printf("  HOVER (TOAST) vs TREADS (ROLL OUT) in THE BAY: %.0f%% of %d%s%s\n", p, hover_v_tread_bay_n, p >= 45 ? "   under 45 " : "", p >= 45 ? G : ""); if (p >= 45) guard = 1; }
  if (hover_v_tread_laser_n) { double p = 100.0 * hover_v_tread_laser_w / hover_v_tread_laser_n; printf("  HOVER (TOAST) vs TREADS (ROLL OUT) in THE LASER ROOM: %.0f%% of %d%s%s\n", p, hover_v_tread_laser_n, p <= 55 ? "   over 55 " : "", p <= 55 ? G : ""); if (p <= 55) guard = 1; }
  if (guard && g_use_ai) fail = 1;
  printf("\nKO causes:");
  for (int c = 0; c < 9; c++) if (ko_cause[c]) printf(" %s %d;", KO_NAME[c], ko_cause[c]);
  /* every KO cause must happen somewhere in a full robin with hazards on. The AI never parks, so
   * COUNTED OUT and DECK COUNT are the builtin driver's (its camper) to show; the AI run needs the
   * four a fighting bot can meet: HP ZERO, KO BY FLIP, PITTED, OFF THE ROOF */
  if (arena_mask == 63 && haz_mode != 2 && mode == MODE_DEATHMATCH) {
    for (int c = KO_HP; c <= KO_DECK; c++) {
      if (g_use_ai && (c == KO_COUNT || c == KO_DECK)) continue;
      if (!ko_cause[c]) { printf("\nFAIL: no %s in the robin", KO_NAME[c]); fail = 1; }
    }
  }
  printf("\nmatches %d, mean %.1f s, stalls (no result) %d, stuck frames %d, %.1f s wall, %.0fk steps/s\n", matches, matches ? (double)frames / matches / 60.0 : 0, stalls, stuck, secs, secs > 0 ? frames / secs / 1000.0 : 0);
  if (stalls) { printf("FAIL: %d matches did not end\n", stalls); fail = 1; }
  if (stuck) { printf("FAIL: [stuck] nudges in the robin\n"); fail = 1; }
  printf(fail ? "robin: FAIL\n" : "robin: PASS\n");
  return fail;
}

/* -- --modes: every mode in every arena with 2, 3 and 4 seats must end, with no [stuck] -- */
static int modes(int n) {
  static const char *MODE_KEY[MODE_COUNT] = { "DEATHMATCH", "KO BY FLIP", "LAST BOT", "HILL", "2V2", "TOURNAMENT" };
  int fail = 0;
  for (int mode = 0; mode < MODE_COUNT; mode++) {
    int matches = 0, stalls = 0, stuck = 0, how[9] = { 0 }; long frames = 0; int ev_hill = 0, ev_red = 0, ev_pick = 0, ev_mini = 0, ev_close = 0;
    for (int ar = 0; ar < 6; ar++) for (int players = 2; players <= 4; players++) for (int k = 0; k < n; k++) {
      if (mode == MODE_2V2 && players != 4) continue;
      if ((mode == MODE_LAST_BOT || mode == MODE_TOURNAMENT) && players < 3) continue;
      match_setup_t s; memset(&s, 0, sizeof s);
      s.mode = (uint8_t)mode; s.arena = (uint8_t)ar; s.players = (uint8_t)players; s.fight_len_s = 75;
      s.comeback = players > 2 || mode == MODE_2V2;          /* AUTO: on in FFA and 2V2, off in 1v1 */
      s.seed = 0xC0FFEEu + (uint32_t)(mode * 1000 + ar * 100 + players * 10 + k);
      for (int i = 0; i < players; i++) { build_preset(&s.builds[i], (int)((k + i * 2 + ar + mode) % PRE_RANDOM)); s.is_ai[i] = 1; s.team[i] = (uint8_t)(i & 1); }
      if (g_trace_seed && s.seed != g_trace_seed) continue;
      result_t r; run_match(&s, &r, g_trace_seed ? 1 + g_verbose : 0);
      matches++; frames += r.frames; stuck += r.stuck; how[r.how]++;
      if (r.how == WIN_NONE) stalls++;
      ev_hill += r.events[EV_HILL_SCORE]; ev_red += r.events[EV_RED_TAG]; ev_pick += r.events[EV_PICKUP] + r.events[EV_NOT_VACUUM]; ev_mini += r.events[EV_MINI_KO]; ev_close += r.events[EV_CLOSING];
      if (g_verbose && (r.how == WIN_NONE || r.stuck)) printf("  %s %s %dp seed %u: %s in %d%s\n", MODE_KEY[mode], ARENAS[ar].name, players, s.seed, KO_NAME[r.how], r.frames, r.stuck ? " [stuck]" : "");
    }
    printf("%-11s %4d matches, mean %5.1f s, stalls %d, stuck %d |", MODE_KEY[mode], matches, matches ? frames / 60.0 / matches : 0, stalls, stuck);
    for (int c = 0; c < 9; c++) if (how[c]) printf(" %s %d", KO_NAME[c], how[c]);
    printf(" | mini-KO %d, red tags %d, pickups %d, closing %d, hill points %d\n", ev_mini, ev_red, ev_pick, ev_close, ev_hill);
    if (stalls || stuck) fail = 1;
    if (mode == MODE_HILL && !ev_hill) { printf("  FAIL: no hill points scored\n"); fail = 1; }
  }
  printf(fail ? "modes: FAIL\n" : "modes: PASS\n");
  return fail;
}

/* -- --check-arenas (6.6) ------------------------------------------ */
typedef struct { float x0, z0, x1, z1, h; const char *what; int cell; } box_t;

static float box_dist(const box_t *a, const box_t *b) {
  float dx = a->x1 < b->x0 ? b->x0 - a->x1 : b->x1 < a->x0 ? a->x0 - b->x1 : 0;
  float dz = a->z1 < b->z0 ? b->z0 - a->z1 : b->z1 < a->z0 ? a->z0 - b->z1 : 0;
  return sqrtf(dx * dx + dz * dz);
}

static int seg_hits_box(float ax, float az, float bx, float bz, const box_t *b) {
  /* slab test */
  float dx = bx - ax, dz = bz - az, t0 = 0, t1 = 1;
  for (int k = 0; k < 2; k++) {
    float o = k ? az : ax, d = k ? dz : dx, lo = k ? b->z0 : b->x0, hi = k ? b->z1 : b->x1;
    if (fabsf(d) < 1e-6f) { if (o < lo || o > hi) return 0; continue; }
    float ta = (lo - o) / d, tb = (hi - o) / d; if (ta > tb) { float t = ta; ta = tb; tb = t; }
    if (ta > t0) t0 = ta; if (tb < t1) t1 = tb;
    if (t0 > t1) return 0;
  }
  return 1;
}

static int check_arena(int ai) {
  const arena_def_t *a = &ARENAS[ai];
  float hw = a->w * 0.5f, hd = a->d * 0.5f;
  int fail = 0;
  box_t solids[64]; int ns = 0;
  char names[64][40];
  /* the solids: props that collide, the deck, bumper posts, the OPS gate */
  for (int i = 0; i < a->nprops; i++) {
    const prop_def_t *p = &a->props[i];
    if (!arena_prop_solid(p)) continue;
    snprintf(names[ns], 40, "prop %d (%s)", i, p->kind == PR_BENCH ? "bench" : p->kind == PR_HVAC ? "hvac" : "solid");
    solids[ns] = (box_t){ p->x - p->w * 0.5f, p->z - p->d * 0.5f, p->x + p->w * 0.5f, p->z + p->d * 0.5f, p->h, names[ns], arena_cell(a, p->x, p->z) }; ns++;
  }
  for (int i = 0; i < a->nhaz; i++) {
    const hazard_def_t *h = &a->haz[i];
    if (h->type == HZ_DECK) {
      float x0 = h->x - h->w * 0.5f, x1 = h->x + h->w * 0.5f, z0 = h->z - h->d * 0.5f, z1 = h->z + h->d * 0.5f, sx = x0;
      for (int k = 0; k < a->nhaz; k++) if (a->haz[k].type == HZ_RAMP && a->haz[k].h > 0.3f) {
        float rx0 = a->haz[k].x - a->haz[k].w * 0.5f, rx1 = a->haz[k].x + a->haz[k].w * 0.5f;
        if (rx0 > sx) { snprintf(names[ns], 40, "deck"); solids[ns] = (box_t){ sx, z0, rx0, z1, h->h, names[ns], -1 }; ns++; }
        sx = rx1;
      }
      if (sx < x1) { snprintf(names[ns], 40, "deck"); solids[ns] = (box_t){ sx, z0, x1, z1, h->h, names[ns], -1 }; ns++; }
    } else if (h->type == HZ_BUMPER) {
      float x0, z0, x1, z1; arena_hazard_rect(h, &x0, &z0, &x1, &z1);
      snprintf(names[ns], 40, "bumper %d", i); solids[ns] = (box_t){ x0, z0, x1, z1, (float)h->p0 / 100.0f, names[ns], -1 }; ns++;
    }
  }
  float g[2][4]; arena_ops_gate(a, g);
  for (int k = 0; k < 2; k++) { snprintf(names[ns], 40, "ops gate %d", k); solids[ns] = (box_t){ g[k][0], g[k][1], g[k][2], g[k][3], 1.0f, names[ns], -1 }; ns++; }
  /* 1. gaps: between every pair of solids, and between a solid and a wall, a gap is 0 or at least 1.6 */
  for (int i = 0; i < ns; i++) {
    for (int j = i + 1; j < ns; j++) {
      float d = box_dist(&solids[i], &solids[j]);
      if (d > 1e-3f && d < 1.6f) { printf("  FAIL gap %.2f between %s and %s\n", d, solids[i].what, solids[j].what); fail = 1; }
    }
    struct { float d; const char *w; int open; } walls[4] = {
      { hw - solids[i].x1, "+x wall", a->open_mask & OPEN_PX }, { solids[i].x0 + hw, "-x wall", a->open_mask & OPEN_NX },
      { hd - solids[i].z1, "+z wall", a->open_mask & OPEN_PZ }, { solids[i].z0 + hd, "-z wall", a->open_mask & OPEN_NZ } };
    for (int k = 0; k < 4; k++) if (!walls[k].open && walls[k].d > 1e-3f && walls[k].d < 1.6f) { printf("  FAIL gap %.2f between %s and the %s\n", walls[k].d, solids[i].what, walls[k].w); fail = 1; }
  }
  /* 2. the visibility table: a prop in a cell whose bit is clear from cell i must be hidden from most of
   *    64 sample positions in cell i by a solid at least 0.4 high between them */
  float cw = a->w / 4, cd = a->d / 4;
  for (int i = 0; i < 16; i++) {
    for (int p = 0; p < a->nprops; p++) {
      const prop_def_t *pd = &a->props[p];
      int pc = arena_cell(a, pd->x, pd->z);
      if (a->vis[i] & (1 << pc)) continue;
      int seen = 0;
      for (int sy = 0; sy < 8; sy++) for (int sx = 0; sx < 8; sx++) {
        float x = -hw + (i % 4) * cw + (sx + 0.5f) * cw / 8, z = -hd + (i / 4) * cd + (sy + 0.5f) * cd / 8;
        int blocked = 0;
        for (int s = 0; s < ns && !blocked; s++) {
          if (solids[s].h < 0.4f) continue;
          if (solids[s].x0 <= pd->x && pd->x <= solids[s].x1 && solids[s].z0 <= pd->z && pd->z <= solids[s].z1) continue;   /* the prop itself */
          if (seg_hits_box(x, z, pd->x, pd->z, &solids[s])) blocked = 1;
        }
        /* the mound is an occluder too: a line crossing within r 3 of its centre */
        for (int k = 0; k < a->nhaz && !blocked; k++) if (a->haz[k].type == HZ_MOUND) {
          float dx = pd->x - x, dz = pd->z - z, L = dx * dx + dz * dz; if (L < 1e-6f) continue;
          float t = -((x - a->haz[k].x) * dx + (z - a->haz[k].z) * dz) / L; if (t < 0) t = 0; if (t > 1) t = 1;
          float px = x + t * dx - a->haz[k].x, pz = z + t * dz - a->haz[k].z;
          if (px * px + pz * pz < 9.0f) blocked = 1;
        }
        if (!blocked) seen++;
      }
      if (seen > 32) { printf("  FAIL vis: cell %d clears cell %d but prop %d at (%.1f, %.1f) is in view from %d of 64 samples\n", i, pc, p, pd->x, pd->z, seen); fail = 1; }
    }
  }
  /* 3. spawn clearance: 1.5 u from solids, dangerous hazards and the OPS zone */
  for (int k = 0; k < 6; k++) {
    const float *sp = k < 4 ? a->spawn4[k] : a->spawn2[k - 4];
    box_t me = { sp[0], sp[1], sp[0], sp[1], 0, "spawn", -1 };
    for (int s = 0; s < ns; s++) { float d = box_dist(&me, &solids[s]); if (d < 1.5f - 1e-3f) { printf("  FAIL spawn %d at (%.1f, %.1f) is %.2f from %s\n", k, sp[0], sp[1], d, solids[s].what); fail = 1; } }
    for (int h = 0; h < a->nhaz; h++) {
      int t = a->haz[h].type;
      if (t == HZ_RAMP || t == HZ_DECK || t == HZ_MOUND || t == HZ_WIND || t == HZ_LASER || t == HZ_HILL) continue;
      float x0, z0, x1, z1; arena_hazard_rect(&a->haz[h], &x0, &z0, &x1, &z1);
      box_t hb = { x0, z0, x1, z1, 0, "hazard", -1 };
      float d = box_dist(&me, &hb);
      if (d < 1.5f - 1e-3f) { printf("  FAIL spawn %d at (%.1f, %.1f) is %.2f from hazard %d\n", k, sp[0], sp[1], d, h); fail = 1; }
    }
    box_t zone = { a->ops_zone[0] - 1, a->ops_zone[1] - 1, a->ops_zone[0] + 1, a->ops_zone[1] + 1, 0, "ops zone", -1 };
    float d = box_dist(&me, &zone);
    if (d < 1.5f - 1e-3f) { printf("  FAIL spawn %d at (%.1f, %.1f) is %.2f from the OPS zone\n", k, sp[0], sp[1], d); fail = 1; }
    if (fabsf(sp[0]) > hw - 1.0f || fabsf(sp[1]) > hd - 1.0f) { printf("  FAIL spawn %d at (%.1f, %.1f) is within 1 u of a wall\n", k, sp[0], sp[1]); fail = 1; }
  }
  /* 4. hazard reach inside the floor; the OPS zone outside pits, hazards and spawns */
  for (int h = 0; h < a->nhaz; h++) {
    float x0, z0, x1, z1; arena_hazard_rect(&a->haz[h], &x0, &z0, &x1, &z1);
    if (x0 < -hw - 1e-3f || x1 > hw + 1e-3f || z0 < -hd - 1e-3f || z1 > hd + 1e-3f) { printf("  FAIL hazard %d (type %d) reaches outside the floor: %.1f..%.1f x %.1f..%.1f\n", h, a->haz[h].type, x0, x1, z0, z1); fail = 1; }
    int t = a->haz[h].type;
    if (t == HZ_RAMP || t == HZ_DECK || t == HZ_MOUND || t == HZ_WIND || t == HZ_LASER || t == HZ_HILL) continue;
    box_t hb = { x0, z0, x1, z1, 0, "hazard", -1 };
    box_t zone = { a->ops_zone[0] - 1, a->ops_zone[1] - 1, a->ops_zone[0] + 1, a->ops_zone[1] + 1, 0, "ops zone", -1 };
    if (box_dist(&hb, &zone) < 1e-3f) { printf("  FAIL hazard %d overlaps the OPS zone\n", h); fail = 1; }
  }
  if (fabsf(a->ops_zone[0]) > hw - 1 + 1e-3f || fabsf(a->ops_zone[1]) > hd - 1 + 1e-3f) { printf("  FAIL the OPS zone pokes through a wall\n"); fail = 1; }
  /* 5. a 1.1 box must fit between the gate and the nearest solid (the box drive of 6.6, as the gap rule above) */
  for (int h = 0; h < 4; h++) if (fabsf(a->hill[h][0]) > hw - 1.5f || fabsf(a->hill[h][1]) > hd - 1.5f) { printf("  FAIL hill point %d outside the floor margin\n", h); fail = 1; }
  printf("%-15s %2.0f x %2.0f  hazards %d  props %d  solids %d  %s\n", a->name, a->w, a->d, a->nhaz, a->nprops, ns, fail ? "FAIL" : "PASS");
  return fail;
}

static int check_arenas(void) {
  int fail = 0;
  for (int i = 0; i < ARENA_COUNT; i++) fail |= check_arena(i);
  printf(fail ? "check-arenas: FAIL\n" : "check-arenas: PASS\n");
  return fail;
}

/* -- --replay: the setup line and the script (15.7) ---------------- */
typedef struct { int from, to, pad; uint16_t buttons; int lx, ly, rx, ry, lt, rt; uint8_t has_lx, has_ly, has_rx, has_ry, has_lt, has_rt; } seg_t;
static seg_t SEGS[16384]; static int NSEGS;
static uint8_t g_pad_tank[4];          /* pads_at: the scheme of the seat each pad drives (TANK gets both sides from the d-pad) */

static const struct { const char *n; int bit; } BTN[] = { { "A", IN_A }, { "B", IN_B }, { "X", IN_X }, { "Y", IN_Y }, { "L", IN_L }, { "R", IN_R }, { "START", IN_START }, { "SELECT", IN_SELECT },
  { "UP", IN_UP }, { "DOWN", IN_DOWN }, { "LEFT", IN_LEFT }, { "RIGHT", IN_RIGHT }, { "L3", IN_L3 }, { "R3", IN_R3 } };

/* a 60 line reader for the rig's script: an array of flat objects with numbers and a buttons array */
static int read_script(const char *path) {
  FILE *f = fopen(path, "rb"); if (!f) { fprintf(stderr, "replay: cannot open %s\n", path); return 0; }
  static char buf[1 << 20]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
  const char *p = buf; NSEGS = 0;
  while ((p = strchr(p, 0x7b)) && NSEGS < 16384) {
    const char *end = strchr(p, '}'); if (!end) break;
    seg_t s; memset(&s, 0, sizeof s);
    const char *q = p;
    while ((q = strchr(q, '"')) && q < end) {
      const char *k0 = q + 1, *k1 = strchr(k0, '"'); if (!k1 || k1 > end) break;
      char key[16] = { 0 }; size_t kl = (size_t)(k1 - k0); if (kl > 15) kl = 15; memcpy(key, k0, kl);
      q = k1 + 1;
      const char *colon = strchr(q, ':'); if (!colon || colon > end) break;
      q = colon + 1;
      if (!strcmp(key, "buttons")) {
        const char *lb = strchr(q, '['), *rb = lb ? strchr(lb, ']') : 0; if (!lb || !rb || rb > end) break;
        const char *r = lb;
        while ((r = strchr(r, '"')) && r < rb) {
          const char *b0 = r + 1, *b1 = strchr(b0, '"'); if (!b1) break;
          for (size_t i = 0; i < sizeof BTN / sizeof BTN[0]; i++) if ((size_t)(b1 - b0) == strlen(BTN[i].n) && !strncmp(b0, BTN[i].n, (size_t)(b1 - b0))) s.buttons |= (uint16_t)BTN[i].bit;
          r = b1 + 1;
        }
        q = rb + 1;
      } else {
        int v = (int)strtol(q, (char **)&q, 10);
        if (!strcmp(key, "from")) s.from = v; else if (!strcmp(key, "to")) s.to = v; else if (!strcmp(key, "pad")) s.pad = v;
        else if (!strcmp(key, "lx")) { s.lx = v; s.has_lx = 1; } else if (!strcmp(key, "ly")) { s.ly = v; s.has_ly = 1; }
        else if (!strcmp(key, "rx")) { s.rx = v; s.has_rx = 1; } else if (!strcmp(key, "ry")) { s.ry = v; s.has_ry = 1; }
        else if (!strcmp(key, "lt")) { s.lt = v; s.has_lt = 1; } else if (!strcmp(key, "rt")) { s.rt = v; s.has_rt = 1; }
      }
    }
    SEGS[NSEGS++] = s;
    p = end + 1;
  }
  return NSEGS;
}

/* the pad states at a script frame, as main.c would read them (triggers 0..32767 to 0..255) */
static void pads_at(int frame, pad_state_t out[4]) {
  memset(out, 0, 4 * sizeof out[0]);
  for (int i = 0; i < NSEGS; i++) {
    const seg_t *s = &SEGS[i];
    if (frame < s->from || frame > s->to || s->pad < 0 || s->pad > 3) continue;
    pad_state_t *p = &out[s->pad];
    p->buttons |= s->buttons;
    if (s->has_lx) p->lx = (int16_t)s->lx; if (s->has_ly) p->ly = (int16_t)s->ly;
    if (s->has_rx) p->rx = (int16_t)s->rx; if (s->has_ry) p->ry = (int16_t)s->ry;
    if (s->has_lt) p->lt = (uint8_t)(s->lt > 0 ? s->lt >> 7 : 0); if (s->has_rt) p->rt = (uint8_t)(s->rt > 0 ? s->rt >> 7 : 0);
  }
  /* the d-pad drives the same axes as the left stick (12), as main.c read_pads does: full deflections when
   * the stick rests in the 0.18 dead zone; a TANK seat gets both sides so the hash lines keep agreeing */
  for (int i = 0; i < 4; i++) {
    pad_state_t *p = &out[i];
    int lx = p->lx, ly = p->ly, dz = (int)(0.18f * 32767.0f);
    int dpad = p->buttons & (IN_UP | IN_DOWN | IN_LEFT | IN_RIGHT);
    if (!dpad || lx * lx + ly * ly >= dz * dz) continue;
    int fwd = (p->buttons & IN_UP ? 1 : 0) - (p->buttons & IN_DOWN ? 1 : 0);
    int steer = (p->buttons & IN_RIGHT ? 1 : 0) - (p->buttons & IN_LEFT ? 1 : 0);
    if (g_pad_tank[i] && p->rx * p->rx + p->ry * p->ry < dz * dz) {
      int l = fwd + steer, r = fwd - steer;
      l = l < -1 ? -1 : l > 1 ? 1 : l; r = r < -1 ? -1 : r > 1 ? 1 : r;
      p->ly = (int16_t)(-32767 * l); p->ry = (int16_t)(-32767 * r);
    } else { p->lx = (int16_t)(32767 * steer); p->ly = (int16_t)(-32767 * fwd); }
  }
}

/* main.c's frame clock: acc += dt * timescale, one sim step per DT (at most four), a stall drops time; the
 * mini-KO runs the sim at 0.4 for 36 render frames (J_MINIKO_*), so a 4 player script's frames and the
 * sim's frames drift apart there exactly as they do in the cart */
typedef struct { float acc, timescale; int miniko; } rig_clock_t;
static int rig_frame_begin(rig_clock_t *c) {
  if (c->miniko > 0) { c->miniko--; c->timescale = c->miniko > 0 ? 0.4f : 1.0f; }
  c->acc += DT * c->timescale;
  int steps = 0;
  while (c->acc >= DT - 1e-5f && steps < 4) { c->acc -= DT; if (c->acc < 0) c->acc = 0; steps++; }
  if (steps == 4) c->acc = 0;
  return steps;
}
static void rig_clock_events(rig_clock_t *c, const match_t *m) {
  for (int e = 0; e < m->nev; e++) if (m->ev[e].type == EV_MINI_KO) { c->miniko = 36; c->timescale = 0.4f; }
}

static int kv_ints(const char *line, const char *key, int *out, int n) {
  char k[32]; snprintf(k, sizeof k, " %s=", key);
  const char *p = strstr(line, k); if (!p) return 0;
  p += strlen(k);
  for (int i = 0; i < n; i++) { out[i] = (int)strtol(p, (char **)&p, 10); if (*p == ',') p++; else if (i + 1 < n) return i + 1; }
  return n;
}

static int parse_setup(const char *line, match_setup_t *s, int *start) {
  memset(s, 0, sizeof *s);
  int v[4]; char padded[512]; snprintf(padded, sizeof padded, " %s", line);
  if (!kv_ints(padded, "mode", v, 1)) return 0; s->mode = (uint8_t)v[0];
  if (kv_ints(padded, "arena", v, 1)) s->arena = (uint8_t)v[0];
  if (kv_ints(padded, "seed", v, 1)) s->seed = (uint32_t)v[0];
  if (kv_ints(padded, "start", v, 1)) *start = v[0];
  for (int i = 0; i < 4; i++) { char k[4] = { 'b', (char)('0' + i), 0 }; if (kv_ints(padded, k, v, 4) == 4) { s->builds[i].chassis = (uint8_t)v[0]; s->builds[i].drive = (uint8_t)v[1]; s->builds[i].weapon = (uint8_t)v[2]; s->builds[i].armour = (uint8_t)v[3]; } }
  if (kv_ints(padded, "ai", v, 4)) for (int i = 0; i < 4; i++) s->is_ai[i] = (uint8_t)v[i];
  if (kv_ints(padded, "players", v, 1)) s->players = (uint8_t)v[0];
  if (kv_ints(padded, "comeback", v, 1)) s->comeback = (uint8_t)v[0];
  if (kv_ints(padded, "len", v, 1)) s->fight_len_s = (uint8_t)v[0];
  if (kv_ints(padded, "w2", v, 4)) for (int i = 0; i < 4; i++) s->builds[i].weapon2 = (uint8_t)v[i];
  if (kv_ints(padded, "team", v, 4)) for (int i = 0; i < 4; i++) s->team[i] = (uint8_t)v[i];
  if (kv_ints(padded, "lvl", v, 4)) for (int i = 0; i < 4; i++) s->ai_level[i] = (uint8_t)v[i];
  if (kv_ints(padded, "scheme", v, 4)) for (int i = 0; i < 4; i++) s->scheme[i] = (uint8_t)v[i];
  if (kv_ints(padded, "haz", v, 1)) s->hazards_off = (uint8_t)v[0];
  if (kv_ints(padded, "stop", v, 1)) s->hitstop_off = (uint8_t)v[0];
  if (kv_ints(padded, "ops", v, 1)) s->no_ops = (uint8_t)v[0];
  if (kv_ints(padded, "park", v, 1)) s->park = (uint8_t)v[0];
  if (kv_ints(padded, "pads", v, 4)) for (int i = 0; i < 4; i++) s->pad[i] = (uint8_t)v[i]; else for (int i = 0; i < 4; i++) s->pad[i] = (uint8_t)(s->is_ai[i] ? 0 : i + 1);
  return 1;
}

static int replay(const char *script, const char *setup, int frames) {
  match_setup_t s; int start = 0;
  if (!parse_setup(setup, &s, &start)) { fprintf(stderr, "replay: bad setup line\n"); return 2; }
  if (!read_script(script)) return 2;
  /* by default the replay runs to the script's last frame */
  if (frames <= 0) { int last = start; for (int i = 0; i < NSEGS; i++) if (SEGS[i].to > last) last = SEGS[i].to; frames = last + 1 - start; }
  static match_t m; static ai_t ais[MAX_BOTS];
  sim_init(&m, &s);
  for (int i = 0; i < m.nbots; i++) { ai_init(&ais[i], s.ai_level[i], i); ai_bind(&ais[i], i); }
  memset(g_pad_tank, 0, sizeof g_pad_tank);
  for (int i = 0; i < 4; i++) if (s.pad[i] && s.pad[i] <= 4) g_pad_tank[s.pad[i] - 1] = s.scheme[i];
  char line[512]; sim_setup_line(&s, start, line, sizeof line);
  printf("%s\n", line);
  int last_logged = -1, stuck = 0;
  rig_clock_t clk = { 0, 1.0f, 0 };
  /* verbose=2 prints the heading every 30 frames: the steering proof (stick right must lower h, see sim.c right_of) */
  for (int f = start; f < start + frames && !sim_over(&m); f++) {
    pad_state_t pads[4]; pads_at(f, pads);
    int steps = rig_frame_begin(&clk);
    for (int st = 0; st < steps && !sim_over(&m); st++) {
      for (int i = 0; i < m.nbots && i < 4; i++) {
        bot_t *b = &m.bots[i];
        if (b->hp_max == 0) continue;
        if (s.pad[i]) b->in = pads[s.pad[i] - 1];
        else if (s.is_ai[i] && s.park) memset(&b->in, 0, sizeof b->in);   /* PRACTICE=off: the AI sits still, as in the cart */
        else if (s.is_ai[i]) ai_think(&ais[i], &m, &b->in);
      }
      sim_step(&m);
      rig_clock_events(&clk, &m);
      for (int e = 0; e < m.nev; e++) if (m.ev[e].type == EV_STUCK) { printf("[stuck] f=%d bot %d at (%.2f, %.2f)\n", m.frame, m.ev[e].a, m.ev[e].x, m.ev[e].z); stuck++; }
      m.nev = 0;
      if (m.frame % 60 == 0 && m.frame != last_logged) {
        printf("[sim] f=%d h=%08x\n", m.frame, sim_hash(&m)); last_logged = m.frame;
        if (g_verbose) { printf("   "); for (int i = 0; i < m.nbots; i++) if (m.bots[i].hp_max > 0) printf(" %d:(%.1f,%.1f,%.2f) h %.2f hp %.0f co %d%s", i, m.bots[i].b.x, m.bots[i].b.z, m.bots[i].b.y, m.bots[i].b.h, m.bots[i].hp, m.bots[i].countout, m.bots[i].alive ? "" : " OUT"); printf(" ops %d\n", m.ops_state); }
      }
      if (g_verbose >= 2 && m.frame % 30 == 0 && m.frame % 60 != 0) { printf("[pos] f=%d render=%d", m.frame, f); for (int i = 0; i < m.nbots; i++) if (m.bots[i].hp_max > 0 && !m.bots[i].is_ops) printf(" %d:(%.2f,%.2f) h %.2f w %.2f in lx %d ly %d", i, m.bots[i].b.x, m.bots[i].b.z, m.bots[i].b.h, m.bots[i].b.w, m.bots[i].in.lx, m.bots[i].in.ly); printf("\n"); }
    }
  }
  printf("[sim] end f=%d over=%d winner=%d how=%s stuck=%d", m.frame, m.over, m.winner == 255 ? -1 : m.winner, KO_NAME[m.over ? m.win_how : WIN_NONE], stuck);
  for (int i = 0; i < m.nbots && i < 4; i++) if (m.bots[i].hp_max > 0) printf(" b%d=%s,%.0f", i, m.bots[i].alive ? "alive" : "out", m.bots[i].hp);
  printf("\n");
  return stuck ? 1 : 0;
}

/* -- --make-drive: the six arena laps of 15.5, written as rig scripts ----------
 * One seat (pad 0) on ROLL OUT against the practice bot parked at its 1v1 spawn
 * ("practice": 0 in the first entry is the PRACTICE=off flag main.c honours under
 * the rig), a 90 s fight. A waypoint follower drives the sim and its stick is
 * recorded, held for 3 frames at a time and quantised, into from/to segments, so
 * the script replays the same lap natively (--replay) and in the cart. `cross` is
 * driven once (the pit first, while it is still closed), then `lap` repeats along
 * the walls until 5400 frames. */
#define DRIVE_START 668   /* the cart's first fight step: the garage ends at 308, the vote 2 s later, 1 s, then the 3 s countdown */
typedef struct { const float (*cross)[2]; int ncross; const float (*lap)[2]; int nlap; } drive_plan_t;
static const float BAY_X[][2] = { { -3, 0 }, { 3, 0 }, { 4, -4 }, { 0, -7.0f }, { -5, -6.2f }, { -8.5f, -3 }, { -9, 3 }, { -3, 6.2f }, { 0, 7.0f }, { 4, 5 } };
static const float BAY_L[][2] = { { -10, 0 }, { -10, 5.8f }, { -4, 6.0f }, { 5, 6.0f }, { 10.2f, 5.8f }, { 10.2f, -5.8f }, { 5, -6.0f }, { -5, -6.0f }, { -8.5f, -3 } };
static const float LASER_X[][2] = { { -3, 3 }, { 0, 0 }, { -3, -3 }, { 3, -3 }, { 3, 3 }, { 0, 6 }, { -6, 4 }, { -6, -6 }, { -4, -8.6f }, { 0, -8.6f }, { 4, -8.6f } };
static const float LASER_L[][2] = { { 8.5f, -6 }, { 8.6f, 0 }, { 8.2f, 8.2f }, { -8.2f, 8.2f }, { -8.2f, -6 }, { -5, -8.4f }, { 4, -8.4f } };
static const float PILE_X[][2] = { { -4.5f, 0 }, { 4.5f, 0 }, { 4, -5 }, { -6, -6 }, { -9.5f, -4 }, { -11.2f, -3 }, { -9, -1.5f }, { -11.2f, 0 }, { -9, 1.5f }, { -11.2f, 3 }, { -6, 5 }, { 6, 6 }, { 11.0f, 4.5f }, { 11.1f, -0.5f }, { 9.5f, -6 } };
static const float PILE_L[][2] = { { -9.5f, -6.2f }, { -8.5f, 4.5f }, { 0, 6.2f }, { 10.0f, 6.0f }, { 10.0f, -6.0f }, { 0, -6.2f } };
static const float DOCK_X[][2] = { { -3.5f, 6.8f }, { 3.5f, 6.8f }, { 0, 3.5f }, { 0, 0.5f }, { -4, -1.5f }, { -6, -2.5f }, { -6, -6.5f }, { 5.8f, -6.5f }, { 6, -2.5f }, { 4.5f, -1.5f } };
static const float DOCK_L[][2] = { { -8.2f, -3.5f }, { -8.2f, 4.0f }, { -2, 4.5f }, { 8.4f, 4.2f }, { 8.4f, -3.5f }, { 0, -3.5f } };
static const float SHOP_X[][2] = { { -3.5f, -3.5f }, { -1.5f, -1.6f }, { -1.5f, 1.6f }, { 0, 1.2f }, { 1.5f, 1.6f }, { 1.5f, -1.6f }, { 4, -4 }, { 5, -8.8f }, { 0, -9.0f }, { -4, -8.8f }, { -4, -4 }, { -9.0f, -2.5f }, { -9.0f, 0 }, { -9.0f, 2.5f }, { -5, 5 }, { -2.5f, 9.0f }, { 0, 9.0f }, { -4, 6 }, { -1, 1 }, { 4, -4 }, { 9.0f, -2.5f }, { 9.0f, 0 }, { 9.0f, -2.5f }, { 4, -4 } };
static const float SHOP_L[][2] = { { -6, -6 }, { 0, 0 }, { 6, -6 }, { 0, 0 }, { -6, 6 }, { 0, 0 } };
static const float ROOF_X[][2] = { { 0, -6.6f }, { 1, -2 }, { -3.5f, -1.5f }, { 0, 0 }, { 6.8f, 0.5f }, { 4, -3 } };
static const float ROOF_L[][2] = { { 7.5f, -5.5f }, { -2, -7.2f }, { -6, -7.4f }, { -7.5f, -5 }, { -7.5f, 7.0f }, { 1.5f, 7.6f }, { 6.2f, 7.6f }, { 7.2f, 7.2f }, { 7.2f, -2 } };
#define PLAN(a, b) { a, (int)(sizeof a / sizeof a[0]), b, (int)(sizeof b / sizeof b[0]) }
static const drive_plan_t DRIVE_PLANS[6] = { PLAN(BAY_X, BAY_L), PLAN(LASER_X, LASER_L), PLAN(PILE_X, PILE_L), PLAN(DOCK_X, DOCK_L), PLAN(SHOP_X, SHOP_L), PLAN(ROOF_X, ROOF_L) };

static void drive_setup(match_setup_t *s, int ar) {
  memset(s, 0, sizeof *s);
  s->mode = MODE_DEATHMATCH; s->arena = (uint8_t)ar; s->players = 2; s->fight_len_s = 90; s->seed = 7;
  build_preset(&s->builds[0], PRE_ROLL_OUT); build_preset(&s->builds[1], PRE_LAUNCH);
  s->is_ai[1] = 1; s->ai_level[1] = AI_EASY; s->pad[0] = 1; s->pad[1] = 0; s->park = 1;
}

static int make_drive(int ar, const char *path) {
  const drive_plan_t *pl = &DRIVE_PLANS[ar];
  match_setup_t s; drive_setup(&s, ar);
  static match_t m; sim_init(&m, &s);
  static int16_t LX[5400], LY[5400];
  int wp = 0, lapping = 0; int16_t lx = 0, ly = 0; int frames = 0, nudges = 0, back = 0; float px = 0, pz = 0; uint32_t visited = 0;
  for (int k = 0; k < 5400 && !sim_over(&m); k++) {
    bot_t *b = &m.bots[0];
    /* blocked (OPS parked in the way, a shove into a corner): back off for 45 frames on full lock */
    if (k % 45 == 0) { float mv = sqrtf((b->b.x - px) * (b->b.x - px) + (b->b.z - pz) * (b->b.z - pz)); if (k && mv < 0.2f && back == 0) back = 45; px = b->b.x; pz = b->b.z; }
    const float *t = lapping ? pl->lap[wp] : pl->cross[wp];
    float dx = t[0] - b->b.x, dz = t[1] - b->b.z, d = sqrtf(dx * dx + dz * dz);
    if (d < 0.8f) {
      wp++;
      if (!lapping && wp >= pl->ncross) { lapping = 1; wp = 0; }
      else if (lapping && wp >= pl->nlap) wp = 0;
      t = lapping ? pl->lap[wp] : pl->cross[wp]; dx = t[0] - b->b.x; dz = t[1] - b->b.z;
    }
    if (k % 3 == 0) {
      float err = rtr_wrap_pi(rtr_atan2(dx, dz) - b->b.h);
      float turn = rtr_clamp(err * 2.2f, -1, 1), thr = fabsf(err) < 0.6f ? 1.0f : fabsf(err) < 1.4f ? 0.45f : 0.0f;
      if (back > 0) { turn = 1.0f; thr = -0.75f; }
      lx = (int16_t)(-rtr_clamp(floorf(turn * 8.0f + 0.5f) / 8.0f, -1, 1) * 32767.0f);   /* a positive heading error is a left turn: stick left */
      ly = (int16_t)(-rtr_clamp(floorf(thr * 8.0f + 0.5f) / 8.0f, -1, 1) * 32767.0f);
    }
    if (back > 0) back--;
    LX[k] = lx; LY[k] = ly;
    memset(&b->in, 0, sizeof b->in); b->in.lx = lx; b->in.ly = ly;
    sim_step(&m);
    for (int e = 0; e < m.nev; e++) if (m.ev[e].type == EV_STUCK) nudges++;
    m.nev = 0; frames = k + 1;
    const arena_def_t *A = sim_arena(&m);
    for (int h = 0; h < A->nhaz; h++) {
      float x0, z0, x1, z1; arena_hazard_rect(&A->haz[h], &x0, &z0, &x1, &z1);
      if (b->b.x > x0 - 0.7f && b->b.x < x1 + 0.7f && b->b.z > z0 - 0.7f && b->b.z < z1 + 0.7f) visited |= 1u << h;
    }
  }
  FILE *f = fopen(path, "wb"); if (!f) { fprintf(stderr, "make-drive: cannot write %s\n", path); return 2; }
  fprintf(f, "[\n");
  fprintf(f, "  { \"from\": 0,   \"to\": 0,   \"pad\": 0, \"practice\": 0 },\n");
  fprintf(f, "  { \"from\": 30,  \"to\": 32,  \"pad\": 0, \"buttons\": [\"A\"] },\n");
  fprintf(f, "  { \"from\": 60,  \"to\": 62,  \"pad\": 0, \"buttons\": [\"A\"] },\n");
  fprintf(f, "  { \"from\": 90,  \"to\": 92,  \"pad\": 0, \"buttons\": [\"A\"] },\n");
  fprintf(f, "  { \"from\": 110, \"to\": 112, \"pad\": 0, \"buttons\": [\"START\"] },\n");
  fprintf(f, "  { \"from\": 230, \"to\": 300, \"pad\": 0, \"buttons\": [\"A\"] },\n");
  fprintf(f, "  { \"from\": 308, \"to\": 310, \"pad\": 0, \"buttons\": [\"A\"] },\n");
  for (int i = 0; i < ar; i++) fprintf(f, "  { \"from\": %d, \"to\": %d, \"pad\": 0, \"buttons\": [\"RIGHT\"] },\n", 330 + i * 12, 332 + i * 12);
  int nseg = 0;
  for (int k = 0; k < frames; ) {
    int j = k; while (j + 1 < frames && LX[j + 1] == LX[k] && LY[j + 1] == LY[k]) j++;
    if (LX[k] || LY[k]) { fprintf(f, "%s  { \"from\": %d, \"to\": %d, \"pad\": 0, \"lx\": %d, \"ly\": %d }", nseg ? ",\n" : "", DRIVE_START + k, DRIVE_START + j, LX[k], LY[k]); nseg++; }
    k = j + 1;
  }
  fprintf(f, "\n]\n");
  fclose(f);
  char line[512]; sim_setup_line(&s, DRIVE_START, line, sizeof line);
  printf("%-15s %s: %d segments, %d frames, alive %d hp %.0f at (%.1f, %.1f) %s, [stuck] nudges %d\n  %s\n", ARENAS[ar].name, path, nseg, frames, m.bots[0].alive, m.bots[0].hp, m.bots[0].b.x, m.bots[0].b.z, m.bots[0].alive ? "" : KO_NAME[m.bots[0].ko_how], nudges, line);
  int missed = 0;
  for (int h = 0; h < ARENAS[ar].nhaz; h++) if (!(visited & (1u << h))) { printf("  hazard %d (type %d) at (%.1f, %.1f) never crossed\n", h, ARENAS[ar].haz[h].type, ARENAS[ar].haz[h].x, ARENAS[ar].haz[h].z); missed++; }
  return nudges || !m.bots[0].alive || frames < 5400 || missed;
}

/* -- --make-fight: the rig's fight fixtures (2p-fight.json, 4p-fight.json) -----------------
 * The cart's setup line names the match (seed, builds, arena, pads); the existing script keeps every
 * segment before the first fight step (the lobby, garage and vote presses); the human seats are then
 * driven by the builtin stand-in, sampled every 3 frames and quantised to 1/8 like the drive laps, and
 * recorded as segments, so the cart replays a real fight: hits, a KO, no [stuck]. The AI seats think
 * live in the cart and here alike, and the frame clock is main.c's (mini-KO slow-mo included). */
static int make_fight(const char *path, const char *setup, int use_ai) {
  match_setup_t s; int start = 0;
  if (!parse_setup(setup, &s, &start)) { fprintf(stderr, "make-fight: bad setup line\n"); return 2; }
  if (!read_script(path)) return 2;
  seg_t head[256]; int nhead = 0;
  for (int i = 0; i < NSEGS && nhead < 256; i++) if (SEGS[i].to < start) head[nhead++] = SEGS[i];
  static match_t m; static ai_t ais[MAX_BOTS]; drv_t drv[MAX_BOTS];
  sim_init(&m, &s);
  memset(drv, 0, sizeof drv);
  for (int i = 0; i < m.nbots; i++) { ai_init(&ais[i], s.ai_level[i] ? s.ai_level[i] : AI_MEDIUM, i); ai_bind(&ais[i], i); drv[i].rng = s.seed ^ (uint32_t)(i * 977); drv[i].no_camp = 1; drv[i].px = m.bots[i].b.x; drv[i].pz = m.bots[i].b.z; }
  static int16_t LX[4][7200], LY[4][7200]; static uint16_t BT[4][7200];
  pad_state_t pads[4]; memset(pads, 0, sizeof pads);
  rig_clock_t clk = { 0, 1.0f, 0 };
  int frames = 0, hits = 0, stuck = 0;
  for (int k = 0; k < 7200 && !sim_over(&m); k++) {
    for (int i = 0; i < m.nbots && i < 4; i++) {
      if (!s.pad[i]) continue;
      pad_state_t o;
      if (use_ai) ai_think(&ais[i], &m, &o); else drive_builtin(&m, i, &drv[i], &o);   /* the human stand-in: the MEDIUM AI by default */
      pad_state_t *p = &pads[s.pad[i] - 1];
      if (k % 3 == 0) {
        p->lx = (int16_t)(rtr_clamp(floorf(o.lx / 32767.0f * 8.0f + 0.5f) / 8.0f, -1, 1) * 32767.0f);
        p->ly = (int16_t)(rtr_clamp(floorf(o.ly / 32767.0f * 8.0f + 0.5f) / 8.0f, -1, 1) * 32767.0f);
      }
      p->buttons = o.buttons;
      LX[s.pad[i] - 1][k] = p->lx; LY[s.pad[i] - 1][k] = p->ly; BT[s.pad[i] - 1][k] = p->buttons;
    }
    int steps = rig_frame_begin(&clk);
    for (int st = 0; st < steps && !sim_over(&m); st++) {
      for (int i = 0; i < m.nbots && i < 4; i++) {
        bot_t *b = &m.bots[i];
        if (b->hp_max == 0) continue;
        if (s.pad[i]) b->in = pads[s.pad[i] - 1];
        else if (s.is_ai[i]) ai_think(&ais[i], &m, &b->in);
      }
      sim_step(&m);
      rig_clock_events(&clk, &m);
      for (int e = 0; e < m.nev; e++) { if (m.ev[e].type == EV_STUCK) stuck++; if (m.ev[e].type == EV_HIT || m.ev[e].type == EV_RAM) hits++; }
      m.nev = 0;
    }
    frames = k + 1;
  }
  FILE *f = fopen(path, "wb"); if (!f) { fprintf(stderr, "make-fight: cannot write %s\n", path); return 2; }
  fprintf(f, "[\n");
  for (int i = 0; i < nhead; i++) {
    const seg_t *h = &head[i];
    fprintf(f, "  { \"from\": %d, \"to\": %d, \"pad\": %d", h->from, h->to, h->pad);
    if (h->buttons) { fprintf(f, ", \"buttons\": ["); int first = 1; for (size_t b = 0; b < sizeof BTN / sizeof BTN[0]; b++) if (h->buttons & BTN[b].bit) { fprintf(f, "%s\"%s\"", first ? "" : ", ", BTN[b].n); first = 0; } fprintf(f, "]"); }
    if (h->has_lx) fprintf(f, ", \"lx\": %d", h->lx); if (h->has_ly) fprintf(f, ", \"ly\": %d", h->ly);
    if (h->has_rx) fprintf(f, ", \"rx\": %d", h->rx); if (h->has_ry) fprintf(f, ", \"ry\": %d", h->ry);
    if (h->has_lt) fprintf(f, ", \"lt\": %d", h->lt); if (h->has_rt) fprintf(f, ", \"rt\": %d", h->rt);
    fprintf(f, " },\n");
  }
  int nseg = 0;
  for (int pad = 0; pad < 4; pad++) {
    int used = 0; for (int i = 0; i < 4; i++) if (s.pad[i] == pad + 1) used = 1;
    if (!used) continue;
    for (int k = 0; k < frames; ) {
      int j = k; while (j + 1 < frames && LX[pad][j + 1] == LX[pad][k] && LY[pad][j + 1] == LY[pad][k] && BT[pad][j + 1] == BT[pad][k]) j++;
      if (LX[pad][k] || LY[pad][k] || BT[pad][k]) {
        fprintf(f, "%s  { \"from\": %d, \"to\": %d, \"pad\": %d, \"lx\": %d, \"ly\": %d", nseg ? ",\n" : "", start + k, start + j, pad, LX[pad][k], LY[pad][k]);
        if (BT[pad][k]) { fprintf(f, ", \"buttons\": ["); int first = 1; for (size_t b = 0; b < sizeof BTN / sizeof BTN[0]; b++) if (BT[pad][k] & BTN[b].bit) { fprintf(f, "%s\"%s\"", first ? "" : ", ", BTN[b].n); first = 0; } fprintf(f, "]"); }
        fprintf(f, " }"); nseg++;
      }
      k = j + 1;
    }
  }
  fprintf(f, "\n]\n");
  fclose(f);
  printf("%s: %d header segments kept, %d fight segments, %d render frames, winner %d by %s at sim frame %d, %d hits, [stuck] %d\n", path, nhead, nseg, frames, m.winner == 255 ? -1 : m.winner, KO_NAME[m.over ? m.win_how : WIN_NONE], m.frame, hits, stuck);
  for (int i = 0; i < m.nbots && i < 4; i++) if (m.bots[i].hp_max > 0) printf("  b%d %s hp %.0f dealt %.0f\n", i, m.bots[i].name, m.bots[i].hp, m.bots[i].dealt);
  return stuck ? 1 : 0;
}

/* -- --map: a top down picture of an arena's data and its drive lap (a PPM), to check the
 * tables against the ASCII maps of 6.3 by eye -- */
static void px_rect(uint8_t *img, int W, int H, int x0, int y0, int x1, int y1, uint32_t c) {
  for (int y = y0 < 0 ? 0 : y0; y < y1 && y < H; y++) for (int x = x0 < 0 ? 0 : x0; x < x1 && x < W; x++) { uint8_t *q = img + (y * W + x) * 3; q[0] = (uint8_t)(c >> 16); q[1] = (uint8_t)(c >> 8); q[2] = (uint8_t)c; }
}
static int draw_map(int ar, const char *script, const char *out) {
  const arena_def_t *a = &ARENAS[ar];
  const int S = 24, M = 24; int W = (int)(a->w * S) + 2 * M, H = (int)(a->d * S) + 2 * M;
  static uint8_t img[1200 * 1200 * 3]; memset(img, 0, sizeof img);
  #define WX(x) ((int)(((x) + a->w * 0.5f) * S) + M)
  #define WZ(z) ((int)(((z) + a->d * 0.5f) * S) + M)
  px_rect(img, W, H, 0, 0, W, H, 0x15120f);
  px_rect(img, W, H, M, M, W - M, H - M, 0x8a857c);
  for (int i = 0; i <= (int)a->w; i++) px_rect(img, W, H, WX(-a->w * 0.5f + i), M, WX(-a->w * 0.5f + i) + 1, H - M, 0x7a756c);
  for (int i = 0; i <= (int)a->d; i++) px_rect(img, W, H, M, WZ(-a->d * 0.5f + i), W - M, WZ(-a->d * 0.5f + i) + 1, 0x7a756c);
  /* walls: char rails, amber where open */
  uint32_t wall = 0x2a2520, open = 0xf2ab1e;
  px_rect(img, W, H, M - 6, M - 6, W - M + 6, M, (a->open_mask & OPEN_NZ) ? open : wall);
  px_rect(img, W, H, M - 6, H - M, W - M + 6, H - M + 6, (a->open_mask & OPEN_PZ) ? open : wall);
  px_rect(img, W, H, M - 6, M - 6, M, H - M + 6, (a->open_mask & OPEN_NX) ? open : wall);
  px_rect(img, W, H, W - M, M - 6, W - M + 6, H - M + 6, (a->open_mask & OPEN_PX) ? open : wall);
  static const uint32_t HZC[HZ_TYPE_COUNT] = { 0, 0x111111, 0xc2542c, 0xf2ab1e, 0xd9672b, 0x9c7a4a, 0xb7b0a2, 0xece3d3, 0x5a5048, 0x5fb892, 0x6a4f8a, 0x9fd8c4, 0, 0, 0xb08a5a };
  for (int h = 0; h < a->nhaz; h++) {
    const hazard_def_t *d = &a->haz[h];
    if (d->type == HZ_WIND || d->type == HZ_LASER) continue;
    float x0, z0, x1, z1; arena_hazard_rect(d, &x0, &z0, &x1, &z1);
    if (d->type == HZ_MOUND) { for (int y = WZ(-4); y < WZ(4); y++) for (int x = WX(-4); x < WX(4); x++) { float fx = (float)(x - M) / S - a->w * 0.5f - d->x, fz = (float)(y - M) / S - a->d * 0.5f - d->z; float hh = arena_mound_h(sqrtf(fx * fx + fz * fz)); if (hh > 0) px_rect(img, W, H, x, y, x + 1, y + 1, (uint32_t)(0x8a857c + ((int)(hh * 60) << 16) + ((int)(hh * 30) << 8))); } continue; }
    px_rect(img, W, H, WX(x0), WZ(z0), WX(x1), WZ(z1), HZC[d->type]);
  }
  for (int p = 0; p < a->nprops; p++) {
    const prop_def_t *d = &a->props[p];
    uint32_t c = arena_prop_solid(d) ? 0x4a4338 : 0xb08a5a;
    if (d->kind == PR_SHELF || d->kind == PR_SIGN) c = 0x2a2520;
    px_rect(img, W, H, WX(d->x - d->w * 0.5f), WZ(d->z - d->d * 0.5f), WX(d->x + d->w * 0.5f), WZ(d->z + d->d * 0.5f), c);
  }
  /* OPS zone and gate */
  float g[2][4]; arena_ops_gate(a, g);
  px_rect(img, W, H, WX(a->ops_zone[0] - 1), WZ(a->ops_zone[1] - 1), WX(a->ops_zone[0] + 1), WZ(a->ops_zone[1] + 1), 0x3a3530);
  for (int k = 0; k < 2; k++) px_rect(img, W, H, WX(g[k][0]), WZ(g[k][1]), WX(g[k][2]) + 1, WZ(g[k][3]) + 1, 0x5fb892);
  /* spawns: seat colours for 4, small bone squares for the 1v1 */
  static const uint32_t SEATC[4] = { 0xf2ab1e, 0x5fb892, 0xece3d3, 0xc2542c };
  for (int k = 0; k < 4; k++) px_rect(img, W, H, WX(a->spawn4[k][0]) - 6, WZ(a->spawn4[k][1]) - 6, WX(a->spawn4[k][0]) + 6, WZ(a->spawn4[k][1]) + 6, SEATC[k]);
  for (int k = 0; k < 2; k++) px_rect(img, W, H, WX(a->spawn2[k][0]) - 3, WZ(a->spawn2[k][1]) - 3, WX(a->spawn2[k][0]) + 3, WZ(a->spawn2[k][1]) + 3, 0x15120f);
  /* the drive lap, replayed */
  if (script && read_script(script)) {
    match_setup_t s; drive_setup(&s, ar);
    static match_t m; sim_init(&m, &s);
    int last = DRIVE_START; for (int i = 0; i < NSEGS; i++) if (SEGS[i].to > last) last = SEGS[i].to;
    for (int f = DRIVE_START; f <= last && !sim_over(&m); f++) {
      pad_state_t pads[4]; pads_at(f, pads); m.bots[0].in = pads[0];
      sim_step(&m); m.nev = 0;
      int x = WX(m.bots[0].b.x), y = WZ(m.bots[0].b.z);
      px_rect(img, W, H, x - 1, y - 1, x + 1, y + 1, m.bots[0].b.y > 0.3f ? 0xffffff : 0xf2ab1e);
    }
  }
  FILE *f = fopen(out, "wb"); if (!f) return 2;
  fprintf(f, "P6\n%d %d\n255\n", W, H); fwrite(img, 1, (size_t)(W * H * 3), f); fclose(f);
  printf("%s: %s (%d x %d)\n", a->name, out, W, H);
  #undef WX
  #undef WZ
  return 0;
}

/* -- --hashcheck: the same seed twice, and a snapshot resumed, give the same hashes -- */
static int hashcheck(void) {
  int fail = 0;
  for (int ar = 0; ar < 6; ar++) {
    match_setup_t s; memset(&s, 0, sizeof s);
    s.mode = MODE_DEATHMATCH; s.arena = (uint8_t)ar; s.players = 2; s.fight_len_s = 75; s.seed = 777u + (uint32_t)ar;
    build_preset(&s.builds[0], (ar + 1) % 6); build_preset(&s.builds[1], (ar + 3) % 6);
    s.is_ai[0] = s.is_ai[1] = 1;
    uint32_t h1[8], h2[8]; int n1 = 0, n2 = 0;
    for (int run = 0; run < 2; run++) {
      static match_t m; drv_t drv[MAX_BOTS]; memset(drv, 0, sizeof drv);
      sim_init(&m, &s);
      static match_t snap; int snapped = 0;
      for (int f = 0; f < 1500 && !sim_over(&m); f++) {
        for (int i = 0; i < m.nbots; i++) if (!m.bots[i].is_ops && m.bots[i].hp_max) drive_builtin(&m, i, &drv[i], &m.bots[i].in);
        sim_step(&m); m.nev = 0;
        if (m.frame % 240 == 0 && (run ? n2 : n1) < 8) { if (run) h2[n2++] = sim_hash(&m); else h1[n1++] = sim_hash(&m); }
        if (run == 1 && m.frame == 600 && !snapped) { sim_snapshot(&m, &snap); snapped = 1; }
      }
      if (run == 1 && snapped) {
        /* resume from the snapshot with zero inputs twice: identical */
        static match_t a, b; sim_snapshot(&snap, &a); sim_snapshot(&snap, &b);
        for (int f = 0; f < 120; f++) { sim_step(&a); sim_step(&b); }
        if (sim_hash(&a) != sim_hash(&b)) { printf("  FAIL snapshot resume differs in %s\n", ARENAS[ar].name); fail = 1; }
      }
    }
    int same = n1 == n2;
    for (int i = 0; same && i < n1; i++) same = h1[i] == h2[i];
    printf("%-15s %d hashes %s\n", ARENAS[ar].name, n1, same ? "identical across two runs" : "DIFFER");
    if (!same) fail = 1;
  }
  printf(fail ? "hashcheck: FAIL\n" : "hashcheck: PASS\n");
  return fail;
}

/* -- main --------------------------------------------------------- */
static int arg_int(int argc, char **argv, const char *key, int dflt) {
  size_t kl = strlen(key);
  for (int i = 2; i < argc; i++) if (!strncmp(argv[i], key, kl) && argv[i][kl] == '=') return atoi(argv[i] + kl + 1);
  return dflt;
}
static const char *arg_str(int argc, char **argv, const char *key, const char *dflt) {
  size_t kl = strlen(key);
  for (int i = 2; i < argc; i++) if (!strncmp(argv[i], key, kl) && argv[i][kl] == '=') return argv[i] + kl + 1;
  return dflt;
}

int main(int argc, char **argv) {
  if (argc < 2) return usage();
  g_verbose = arg_int(argc, argv, "verbose", 0);
  if (!strcmp(argv[1], "--check-arenas")) return check_arenas();
  if (!strcmp(argv[1], "--hashcheck")) return hashcheck();
  g_trace_seed = (uint32_t)arg_int(argc, argv, "trace", 0);
  if (!strcmp(argv[1], "--modes")) return modes(argc > 2 && argv[2][0] != '-' && !strchr(argv[2], '=') ? atoi(argv[2]) : 4);
  if (!strcmp(argv[1], "--robin")) {
    int n = argc > 2 && argv[2][0] != '-' && !strchr(argv[2], '=') ? atoi(argv[2]) : 20;
    if (n <= 0) n = 20;
    const char *ar = arg_str(argc, argv, "arenas", "all"), *hz = arg_str(argc, argv, "hazards", "both"), *dr = arg_str(argc, argv, "driver", "ai");
    int mask = 0;
    if (!strcmp(ar, "all")) mask = 63; else for (int i = 0; i < 6; i++) if (strstr(ar, ARENA_KEY[i])) mask |= 1 << i;
    if (!mask) { fprintf(stderr, "robin: unknown arena %s\n", ar); return 2; }
    int haz = !strcmp(hz, "on") ? 1 : !strcmp(hz, "off") ? 2 : 0;
    g_use_ai = !strcmp(dr, "ai");
    int mode = arg_int(argc, argv, "mode", MODE_DEATHMATCH);
    return robin(n, mask, haz, mode);
  }
  if (!strcmp(argv[1], "--map")) {
    if (argc < 4) return usage();
    int ar = -1; for (int i = 0; i < 6; i++) if (!strcmp(argv[2], ARENA_KEY[i])) ar = i;
    if (ar < 0) return usage();
    return draw_map(ar, argc > 4 ? argv[4] : 0, argv[3]);
  }
  if (!strcmp(argv[1], "--make-drive")) {
    if (argc < 4) return usage();
    int ar = -1; for (int i = 0; i < 6; i++) if (!strcmp(argv[2], ARENA_KEY[i])) ar = i;
    if (ar < 0) return usage();
    return make_drive(ar, argv[3]);
  }
  if (!strcmp(argv[1], "--replay")) {
    if (argc < 5 || strcmp(argv[3], "--setup")) return usage();
    return replay(argv[2], argv[4], arg_int(argc, argv, "frames", 0));
  }
  if (!strcmp(argv[1], "--make-fight")) {
    if (argc < 5 || strcmp(argv[3], "--setup")) return usage();
    return make_fight(argv[2], argv[4], !strcmp(arg_str(argc, argv, "driver", "ai"), "ai"));
  }
  if (!strcmp(argv[1], "--trace")) {
    if (argc < 5) return usage();
    int ar = -1; for (int i = 0; i < 6; i++) if (!strcmp(argv[2], ARENA_KEY[i])) ar = i;
    if (ar < 0) ar = atoi(argv[2]);
    int pa = -1, pb = -1;
    for (int i = 0; i < PRE_RANDOM; i++) { if (!strcasecmp(argv[3], PRESETS[i].name)) pa = i; if (!strcasecmp(argv[4], PRESETS[i].name)) pb = i; }
    if (pa < 0) pa = atoi(argv[3]); if (pb < 0) pb = atoi(argv[4]);
    match_setup_t s; memset(&s, 0, sizeof s);
    s.mode = (uint8_t)arg_int(argc, argv, "mode", MODE_DEATHMATCH); s.arena = (uint8_t)ar; s.players = 2; s.fight_len_s = 75; s.seed = (uint32_t)arg_int(argc, argv, "seed", 7);
    s.hazards_off = (uint8_t)(!strcmp(arg_str(argc, argv, "hazards", "on"), "off"));
    s.comeback = (uint8_t)arg_int(argc, argv, "comeback", 0);
    build_preset(&s.builds[0], pa); build_preset(&s.builds[1], pb); s.is_ai[0] = s.is_ai[1] = 1;
    g_use_ai = !strcmp(arg_str(argc, argv, "driver", "ai"), "ai");
    char line[512]; sim_setup_line(&s, 0, line, sizeof line); printf("%s\n", line);
    result_t r; run_match(&s, &r, 1);
    printf("winner %d by %s in %d frames (%.1f s), stuck frames %d\n", r.winner, KO_NAME[r.how], r.frames, r.frames / 60.0, r.stuck);
    return 0;
  }
  if (!strcmp(argv[1], "--parts")) {
    for (int i = 0; i < PRE_RANDOM; i++) {
      build_t b; build_preset(&b, i);
      char name[24]; bot_name(name, &b);
      float sp, hi, to; build_bars(&b, &sp, &hi, &to);
      printf("%-9s %-18s %2d of %2d tags  speed %.2f hit %.2f tough %.2f  legal %d\n", PRESETS[i].name, name, build_cost(&b), build_budget(&b), sp, hi, to, build_legal(&b, 4));
    }
    return 0;
  }
  return usage();
}
