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
 *   build/balance --scenario <name>|all the physics acceptance rows of DEEP-plan 6.8, PASS or FAIL each
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
#ifndef TIP_SWING
#define TIP_SWING 4.0f      /* sim.c's swing tip speed: the hammer's event speed is TIP_SWING (0.5 + p) */
#endif

static int usage(void) {
  fprintf(stderr, "usage: balance --parts | --robin N [arenas=...] [hazards=on|off|both] [driver=ai|builtin] [mode=M] | --check-arenas |\n"
                  "               --replay script.json --setup \"line\" [frames=N] [verbose=1|2] | --make-fight script.json --setup \"line\" |\n"
                  "               --trace arena a b [frames] [driver=ai|builtin] | --hashcheck | --make-drive arena out.json | --modes [N] |\n"
                  "               --scenario <name>|all\n");
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
typedef struct { int winner, how, frames, stuck; int ko_cause[9]; int events[EV_TYPE_COUNT]; int und[2], tips, inv_wedge, inv_flip, glances, wheels, bites[2], hits[2]; float dealt[2]; } result_t;

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
      if (e->type == EV_UNDERRIDE && e->a < 2) r->und[e->a]++;
      if (e->type == EV_RIDE_END && e->zone == RIDE_TIPPED) r->tips++;
      if (e->type == EV_INVERT && e->zone == INV_WEDGE) r->inv_wedge++;
      if (e->type == EV_INVERT && e->zone == INV_FLIPPER) r->inv_flip++;
      if (e->type == EV_HIT && (e->flags & EVF_DEFLECT)) r->glances++;
      if (e->type == EV_WHEEL_HIT) r->wheels++;
      if ((e->type == EV_HIT || e->type == EV_RAM) && e->a < 2 && e->dmg > 0) r->hits[e->a]++;
      if (trace && (e->type == EV_HIT || e->type == EV_RAM || e->type == EV_KO || e->type == EV_MINI_KO || e->type == EV_LAUNCH || e->type == EV_INVERT || e->type == EV_HAZ_HIT || e->type == EV_OPS_WAKE || e->type == EV_CLASH || e->type == EV_SPIN_WALL || e->type == EV_RED_TAG || e->type == EV_PICKUP || e->type == EV_STUCK || e->type == EV_SELFRIGHT || e->type == EV_COUNT_TICK || e->type == EV_UNDERRIDE || e->type == EV_RIDE_END || e->type == EV_WEDGE_CLASH || e->type == EV_LAND || e->type == EV_WHEEL_HIT))
        printf("  f=%d ev=%d a=%d b=%d zone=%d dmg=%.1f at (%.2f, %.2f)\n", m.frame, e->type, e->a, e->b, e->zone, e->dmg, e->x, e->z);
    }
    m.nev = 0;
    steps++;
    if (trace && m.frame % (trace > 1 ? 10 : 60) == 0) {
      printf("f=%4d t=%5.1f h=%08x", m.frame, m.clock_s, sim_hash(&m));
      for (int i = 0; i < m.nbots; i++) { const bot_t *b = &m.bots[i]; if (b->hp_max == 0) continue; printf(" | %d:(%.1f,%.1f,%.2f) v %.1f,%.1f in %d,%d h%.2f w%.2f L%.2f r%d%d hp %.0f sp %.2f g%d%s%s%s", i, b->b.x, b->b.z, b->b.y, b->b.vx, b->b.vz, b->in.lx / 3277, b->in.ly / 3277, b->b.h, b->b.w, b->load, b->ride_by, b->riding, b->hp, b->spin, g_use_ai ? ais[i].goal : -1, b->inverted ? " INV" : "", b->airborne ? " AIR" : "", b->alive ? "" : " DEAD"); if (b->stuck_t) printf(" thr %.2f h %.2f pin %d st %d", b->drive_t, b->b.h, b->pinned, b->stuck_t); }
      printf("\n");
    }
  }
  r->winner = m.winner == 255 ? -1 : m.winner; r->how = m.over ? m.win_how : WIN_NONE; r->frames = m.frame; r->stuck = m.stuck_frames;
  r->dealt[0] = m.bots[0].dealt; r->dealt[1] = m.bots[1].dealt;
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
  long und_ro = 0, ro_matches = 0, tips_ro = 0, inv_wedge = 0, inv_flip = 0, glances = 0, wheels = 0;
  double pre_hits[6] = { 0 }, pre_dealt[6] = { 0 }; int pre_n[6] = { 0 }; int judge_pair[6][6]; memset(judge_pair, 0, sizeof judge_pair);
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
        if (a == PRE_ROLL_OUT) { und_ro += r.und[0]; ro_matches++; tips_ro += r.tips; }
        if (b == PRE_ROLL_OUT && a != PRE_ROLL_OUT) { und_ro += r.und[1]; ro_matches++; tips_ro += r.tips; }
        inv_wedge += r.inv_wedge; inv_flip += r.inv_flip; glances += r.glances; wheels += r.wheels;
        pre_hits[a] += r.hits[0]; pre_dealt[a] += r.dealt[0]; pre_n[a]++; pre_hits[b] += r.hits[1]; pre_dealt[b] += r.dealt[1]; pre_n[b]++;
        for (int c = 0; c < 9; c++) ko_cause[c] += r.ko_cause[c];
        if (r.how == WIN_NONE) stalls++;
        if (r.how == WIN_JUDGES) { judges++; judge_pair[a < b ? a : b][a < b ? b : a]++; }
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
  printf("\nthe new mechanics: %.2f underrides per ROLL OUT match (2 to 6), %.2f tips per ROLL OUT match (0.3 to 1), %ld flips by a wedge, %ld by a flipper, %ld glances off a wedge, %ld wheel hits, over %d matches\n",
         ro_matches ? (double)und_ro / ro_matches : 0, ro_matches ? (double)tips_ro / ro_matches : 0, inv_wedge, inv_flip, glances, wheels, matches);
  printf("matches to the judges per pair:\n          "); for (int b = 0; b < 6; b++) printf(" %8s", PRESETS[b].name); printf("\n");
  for (int a = 0; a < 6; a++) { printf("  %-8s", PRESETS[a].name); for (int b = 0; b < 6; b++) { if (b < a) printf("         "); else printf(" %8d", judge_pair[a][b]); } printf("\n"); }
  printf("hits landed per match and damage per hit (with the burns, landings and holds in the damage):\n");
  for (int a = 0; a < 6; a++) if (pre_n[a]) printf("  %-10s %5.1f hits, %5.1f dealt, %5.1f a hit\n", PRESETS[a].name, pre_hits[a] / pre_n[a], pre_dealt[a] / pre_n[a], pre_hits[a] > 0 ? pre_dealt[a] / pre_hits[a] : 0);
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
  /* 3b. a timed hazard that can reach a spawn (within 1.5 u of it) is idle at haz_clock 0: nobody is hit or
   *     shown a tell before the first tell has had time to play (the laser sweeps the whole floor) */
  for (int h = 0; h < a->nhaz; h++) {
    const hazard_def_t *hz = &a->haz[h];
    if (hz->period <= 0 || hz->type == HZ_RAMP || hz->type == HZ_DECK || hz->type == HZ_MOUND || hz->type == HZ_WIND || hz->type == HZ_HILL) continue;
    float x0, z0, x1, z1; arena_hazard_rect(hz, &x0, &z0, &x1, &z1);
    box_t hb = { x0, z0, x1, z1, 0, "hazard", -1 };
    int reaches = 0;
    for (int k = 0; k < 6; k++) {
      const float *sp = k < 4 ? a->spawn4[k] : a->spawn2[k - 4];
      box_t me = { sp[0], sp[1], sp[0], sp[1], 0, "spawn", -1 };
      if (box_dist(&me, &hb) < 1.5f) reaches = 1;
    }
    if (!reaches) continue;
    float pt; int ph = hazard_phase(hz, 0, &pt);
    if (ph) { printf("  FAIL hazard %d (type %d) reaches a spawn and is %s at haz_clock 0 (offset %.1f)\n", h, hz->type, ph == 1 ? "active" : "telling", hz->offset); fail = 1; }
    /* and its first active phase comes after a full tell: the idle band before it is at least tell_s long */
    float first = hz->period - hz->offset; while (first < 0) first += hz->period;
    if (first < hz->tell_s + 1.0f) { printf("  FAIL hazard %d (type %d) reaches a spawn and fires %.1f s after the horn\n", h, hz->type, first); fail = 1; }
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

/* -- --scenario: the physics acceptance table (DEEP-plan 6.8) ----------------------------------
 * Each row builds a match itself (hazards off, no hit stop, OPS only where named), places the bots by
 * hand, drives them through their pads and reads the events. A row prints PASS or FAIL with its numbers;
 * --scenario all exits non zero on any FAIL. The probe pattern: place, drive in.ly and in.buttons, read M.ev */
static match_t SM;
static event_t SEV[MAX_EVENTS]; static int SNEV;
typedef struct { const char *name; int (*run)(char *msg, int n); } scenario_t;

static void sc_init(int arena, const build_t *b0, const build_t *b1, int ops) {
  match_setup_t s; memset(&s, 0, sizeof s);
  s.mode = MODE_DEATHMATCH; s.arena = (uint8_t)arena; s.players = 2; s.fight_len_s = 90; s.seed = 7;
  s.builds[0] = *b0; s.builds[1] = *b1; s.is_ai[0] = s.is_ai[1] = 1; s.hazards_off = 1; s.hitstop_off = 1; s.no_ops = (uint8_t)!ops;
  sim_init(&SM, &s);
}
static void sc_init_presets(int arena, int p0, int p1, int ops) { build_t a, b; build_preset(&a, p0); build_preset(&b, p1); sc_init(arena, &a, &b, ops); }
static void sc_place(int slot, float x, float z, float h) {
  bot_t *b = &SM.bots[slot];
  b->b.x = b->b.px = x; b->b.z = b->b.pz = z; b->b.h = b->b.ph = h; b->b.vx = b->b.vz = b->b.w = 0;
  b->floor_y = arena_floor_y(sim_arena(&SM), x, z); b->b.y = b->b.py = b->floor_y; b->still_x = x; b->still_z = z;
}
/* thr forward 0..1 (negative reverse), turn positive right, the weapon and other buttons */
static void sc_pad(int slot, float thr, float turn, uint16_t buttons) {
  /* the sim reads the stick past a 0.18 dead zone; this writes the deflection that gives exactly thr */
  pad_state_t *p = &SM.bots[slot].in; memset(p, 0, sizeof *p);
  thr = rtr_clamp(thr, -1, 1); turn = rtr_clamp(turn, -1, 1);
  float y = thr == 0 ? 0 : (thr > 0 ? 1.0f : -1.0f) * (0.18f + 0.82f * fabsf(thr)), x = turn == 0 ? 0 : (turn > 0 ? 1.0f : -1.0f) * (0.18f + 0.82f * fabsf(turn));
  if (thr != 0 && turn != 0) { x = turn; y = thr; }       /* both: the raw deflection */
  p->ly = (int16_t)(-y * 32767.0f); p->lx = (int16_t)(x * 32767.0f); p->buttons = buttons;
}
static int sc_step(void) { sim_step(&SM); SNEV = SM.nev; memcpy(SEV, SM.ev, sizeof(event_t) * (size_t)SNEV); SM.nev = 0; return SNEV; }
static float sc_dist(int i, int j) { float dx = SM.bots[i].b.x - SM.bots[j].b.x, dz = SM.bots[i].b.z - SM.bots[j].b.z; return sqrtf(dx * dx + dz * dz); }
static float sc_speed(int i) { return sqrtf(SM.bots[i].b.vx * SM.bots[i].b.vx + SM.bots[i].b.vz * SM.bots[i].b.vz); }
#define SC_OK(cond, ...) do { snprintf(msg, (size_t)n, __VA_ARGS__); return (cond); } while (0)

/* SOLO (two seats and OPS): slots 2 and 3 exist as zeroed bots and never appear in an event */
static int sc_ghost_sim(char *msg, int n) {
  match_setup_t s; memset(&s, 0, sizeof s);
  s.mode = MODE_DEATHMATCH; s.arena = AR_BAY; s.players = 2; s.fight_len_s = 75; s.seed = 4242;
  build_preset(&s.builds[0], PRE_ROLL_OUT); build_preset(&s.builds[1], PRE_LAUNCH); s.is_ai[0] = s.is_ai[1] = 1; s.ai_level[0] = s.ai_level[1] = AI_MEDIUM;
  sim_init(&SM, &s);
  static ai_t ais[MAX_BOTS]; for (int i = 0; i < 2; i++) { ai_init(&ais[i], AI_MEDIUM, i); ai_bind(&ais[i], i); }
  int bad = 0, events = 0, frames = 0;
  for (int f = 0; f < 3000; f++) {                     /* on past the KO: the corpse and the parked OPS stay in the step */
    for (int i = 0; i < 2; i++) ai_think(&ais[i], &SM, &SM.bots[i].in);
    sc_step(); frames++;
    for (int k = 0; k < SNEV; k++) {
      const event_t *e = &SEV[k]; events++;
      int b_is_slot = !(e->type == EV_HAZ_TELL || e->type == EV_HAZ_HIT || e->type == EV_BENCH_BREAK || e->type == EV_BUMPER_BREAK || e->type == EV_HILL_MOVE || e->type == EV_HILL_SCORE || e->type == EV_OPS_LINE || e->type == EV_CLOSING);
      if (e->a == 2 || e->a == 3 || (b_is_slot && (e->b == 2 || e->b == 3))) bad++;
    }
  }
  SC_OK(SM.nbots == 5 && SM.bots[2].hp_max == 0 && SM.bots[3].hp_max == 0 && bad == 0, "nbots %d, hp_max of slots 2 and 3: %.0f %.0f, %d of %d events over %d frames name slot 2 or 3",
        SM.nbots, SM.bots[2].hp_max, SM.bots[3].hp_max, bad, events, frames);
}

/* THE FREE PILE: a ROLL OUT reversing off the mound into a parked LAUNCH on the floor at r 4.1 meets it at
 * 1.05 (back to back, so no lip is involved) and never overlaps under 1.0 */
static int sc_mound_contact(char *msg, int n) {
  sc_init_presets(AR_PILE, PRE_ROLL_OUT, PRE_LAUNCH, 0);
  const arena_def_t *ar = sim_arena(&SM); float mx = 0, mz = 0;
  for (int h = 0; h < ar->nhaz; h++) if (ar->haz[h].type == HZ_MOUND) { mx = ar->haz[h].x; mz = ar->haz[h].z; }
  /* out along +x from the mound centre; the ROLL OUT faces the centre so its back leads */
  sc_place(0, mx + 2.0f, mz, -1.5707963f); sc_place(1, mx + 4.1f, mz, -1.5707963f);
  float lx0 = SM.bots[1].b.x, at_contact = -1, dmin = 1e9f, y_at = 0;
  for (int f = 0; f < 240; f++) {
    sc_pad(0, -0.7f, 0, 0); sc_pad(1, 0, 0, 0);
    sc_step();
    float d = sc_dist(0, 1); if (d < dmin) dmin = d;
    if (at_contact < 0 && fabsf(SM.bots[1].b.x - lx0) > 0.002f) { at_contact = d; y_at = SM.bots[0].b.y; }
  }
  SC_OK(at_contact > 1.02f && at_contact < 1.08f && dmin >= 1.0f, "first contact at centre distance %.3f (1.05 +- 0.03), closest %.3f (never under 1.0), the ROLL OUT %.2f up the dome at contact", at_contact, dmin, y_at);
}

/* a full spin CHOP into a KO BY FLIP corpse: EV_HIT with EVF_CORPSE and dmg 0 at most every 30 frames, the
 * spin drops by half a live hit's cost, the corpse moves, nobody's tallies change */
static int sc_corpse_hittable(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_CHOP, PRE_LAUNCH, 0);
  sc_place(0, -2.5f, 0, 1.5707963f); sc_place(1, 0, 0, 0);
  bot_t *c = &SM.bots[0], *v = &SM.bots[1];
  v->alive = 0; v->ko_how = KO_FLIP; v->inverted = 1;
  c->spin = 1.0f;
  float vx0 = v->b.x, vz0 = v->b.z, hp0 = v->hp; int hits = 0, last = -100, min_gap = 1000, bad = 0; float drop = -1;
  for (int f = 0; f < 300; f++) {
    sc_pad(0, 0.6f, 0, IN_A); float before = c->spin;
    /* keep driving at the corpse */
    float err = rtr_wrap_pi(rtr_atan2(v->b.x - c->b.x, v->b.z - c->b.z) - c->b.h); SM.bots[0].in.lx = (int16_t)(-rtr_clamp(err * 2.2f, -1, 1) * 32767.0f);
    sc_step();
    for (int k = 0; k < SNEV; k++) if (SEV[k].type == EV_HIT && SEV[k].b == 1) {
      if (!(SEV[k].flags & EVF_CORPSE) || SEV[k].dmg != 0) bad++;
      if (hits && SM.frame - last < min_gap) min_gap = SM.frame - last;
      if (!hits) drop = before + DT / WEAPONS[WP_HORIZ].spin_up - c->spin;
      hits++; last = SM.frame;
    }
  }
  float moved = sqrtf((v->b.x - vx0) * (v->b.x - vx0) + (v->b.z - vz0) * (v->b.z - vz0));
  int tallies = c->dealt == 0 && c->biggest == 0 && c->spinner_dealt == 0 && v->hp == hp0;
  float usual = 0.7f;   /* a live full spin hit's cost on a HORIZONTAL (the energy model: about the bite) */
  SC_OK(hits >= 2 && bad == 0 && min_gap >= 30 && moved > 0.5f && tallies && drop > 0.25f * usual && drop < 0.75f * usual,
        "%d corpse hits (flags and dmg wrong on %d), closest gap %d frames (>= 30), first spin drop %.2f (about half of %.2f), corpse moved %.2f, tallies untouched %d", hits, bad, min_gap, drop, usual, moved, tallies);
}

/* a 6 u/s sideways shove on an idle bot of each drive: the distance it slides before the sideways speed
 * is under 0.3 (TREADS under 0.8, FOUR WHEEL 1.0 to 1.6, TWO WHEEL 1.6 to 2.4, HOVER over 6) */
static float slide_of(int drive) {
  build_t b = { CH_BOX, (uint8_t)drive, WP_NONE, WP_NONE, AR_ALU, 0, 0 }, o = b;
  sc_init(AR_BAY, &b, &o, 0);
  sc_place(0, -9.0f, -4.0f, 0); sc_place(1, 9.0f, 5.0f, 0);
  SM.bots[0].b.vx = 6.0f;                    /* heading 0 faces +z, so +x is straight sideways */
  float x0 = SM.bots[0].b.x;
  for (int f = 0; f < 600; f++) { sc_pad(0, 0, 0, 0); sc_pad(1, 0, 0, 0); sc_step(); if (fabsf(SM.bots[0].b.vx) < 0.3f) break; }
  return SM.bots[0].b.x - x0;
}
static int sc_slide_drives(char *msg, int n) {
  float tr = slide_of(DR_TREAD), fw = slide_of(DR_FOUR), tw = slide_of(DR_TWO), hv = slide_of(DR_HOVER);
  SC_OK(tr < 0.8f && fw >= 1.0f && fw <= 1.6f && tw >= 1.6f && tw <= 2.4f && hv > 6.0f, "slid TREADS %.2f (< 0.8), FOUR WHEEL %.2f (1.0 to 1.6), TWO WHEEL %.2f (1.6 to 2.4), HOVER %.2f (> 6)", tr, fw, tw, hv);
}

/* the first frame (from the scenario start) an event of a type and zone arrives with victim b (-1 any) */
typedef struct { int first[EV_TYPE_COUNT][16]; int count[EV_TYPE_COUNT]; int f0; } sc_log_t;
static void sc_log_init(sc_log_t *L) { memset(L, 0xff, sizeof L->first); memset(L->count, 0, sizeof L->count); L->f0 = SM.frame; }
static void sc_log(sc_log_t *L) {
  for (int k = 0; k < SNEV; k++) { const event_t *e = &SEV[k]; L->count[e->type]++; int z = e->zone < 16 ? e->zone : 15; if (L->first[e->type][z] < 0) L->first[e->type][z] = SM.frame - L->f0; }
}
static int sc_first(const sc_log_t *L, int type, int zone) {
  if (zone >= 0) return L->first[type][zone];
  int best = -1; for (int z = 0; z < 16; z++) if (L->first[type][z] >= 0 && (best < 0 || L->first[type][z] < best)) best = L->first[type][z];
  return best;
}
static const float HALF_PI = 1.5707963f;

/* ROLL OUT flat out into a standing STOMP's front: under it, up the slope, over the top and onto its lid */
static int sc_wedge_front_lunchbox(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_ROLL_OUT, PRE_STOMP, 0);
  sc_place(0, -3.5f, 0, HALF_PI); sc_place(1, 0, 0, -HALF_PI);
  sc_log_t L; sc_log_init(&L); float up = 0, peak = 0;
  for (int f = 0; f < 240; f++) {
    sc_pad(0, 1, 0, 0); sc_pad(1, 0, 0, 0); sc_step(); sc_log(&L);
    if (g_verbose && f > 55 && f < 130) { bot_t *v = &SM.bots[1]; printf("  f %d v x %.2f y %.2f vy %.2f p %.2f wp %.2f air %d inv %d tip %d bnc %d ride %d up %.2f |", f, v->b.x, v->b.y, v->b.vy, v->b.pitch, v->b.wp, v->airborne, v->inverted, v->tipping, v->bounced, v->ride_by, v->ride_up); for (int k = 0; k < SNEV; k++) printf(" ev%d/%d", SEV[k].type, SEV[k].zone); printf("\n"); }
    if (SM.bots[1].ride_up > up) up = SM.bots[1].ride_up;
    if (SM.bots[1].ride_cl_peak > peak) peak = SM.bots[1].ride_cl_peak;
  }
  int und = sc_first(&L, EV_UNDERRIDE, PN_FRONT), tip = sc_first(&L, EV_RIDE_END, RIDE_TIPPED), la = sc_first(&L, EV_LAUNCH, LAUNCH_WEDGE), inv = sc_first(&L, EV_INVERT, INV_WEDGE);
  if (tip >= 0) up = 1.0f;     /* a tip happens only at the top of the slope */
  SC_OK(und >= 0 && und <= 90 && up >= 1.0f && peak >= 2.2f && tip >= 0 && la >= 0 && inv >= 0 && inv <= 240 && SM.bots[1].inverted,
        "UNDERRIDE (front) at %d (<= 90), ride_up %.2f, ride_cl_peak %.2f (>= 2.2), TIPPED at %d, LAUNCH_WEDGE at %d, INVERT (wedge) at %d (<= 240), inverted at the end %d",
        und, up, peak, tip, la, inv, SM.bots[1].inverted);
}

/* the STOMP side on: its legs are there, so no underride; one ram, it stays on the floor and is shoved */
static int sc_wedge_side_lunchbox(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_ROLL_OUT, PRE_STOMP, 0);
  sc_place(0, -3.5f, 0, HALF_PI); sc_place(1, 0, 0, 0);
  sc_log_t L; sc_log_init(&L); float x0 = SM.bots[1].b.x, ymax = 0; int t_contact = -1; float pushed = 0;
  for (int f = 0; f < 400; f++) {
    sc_pad(0, 1, 0, 0); sc_pad(1, 0, 0, 0); sc_step(); sc_log(&L);
    if (SM.bots[1].b.y - SM.bots[1].floor_y > ymax) ymax = SM.bots[1].b.y - SM.bots[1].floor_y;
    if (t_contact < 0 && SM.bots[1].b.x - x0 > 0.01f) t_contact = f;
    if (t_contact >= 0 && f == t_contact + 120) pushed = SM.bots[1].b.x - x0;
  }
  SC_OK(L.count[EV_UNDERRIDE] == 0 && L.count[EV_RAM] == 1 && ymax < 0.01f && pushed > 1.5f, "%d underrides (0), %d rams (1), highest off the floor %.3f, pushed %.2f in 120 frames from contact (> 1.5)",
        L.count[EV_UNDERRIDE], L.count[EV_RAM], ymax, pushed);
}

/* ROLL OUT at 0.6 throttle into a LAUNCH's front: under and up, too slow to tip, carried; the LAUNCH's own
 * full reverse does not get it off the plate; the ride ends when the wedge stops */
static int sc_wedge_front_box_slow(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_ROLL_OUT, PRE_LAUNCH, 0);
  sc_place(0, -6.5f, 0, HALF_PI); sc_place(1, -2.5f, 0, -HALF_PI);
  sc_log_t L; sc_log_init(&L); float up = 0, x_full = 0, carried = 0, load_max = 0, gap0 = -1, gap_open = 0; int full_at = -1, stop_at = -1, end_zone = -1;
  for (int f = 0; f < 400; f++) {
    int stop = full_at >= 0 && f >= full_at + 80;
    sc_pad(0, stop ? 0 : 0.6f, 0, 0);
    sc_pad(1, full_at >= 0 && !stop ? -1.0f : 0, 0, 0);          /* the victim reverses hard once it is on the plate */
    sc_step(); sc_log(&L);
    bot_t *v = &SM.bots[1];
    if (v->ride_up > up) up = v->ride_up;
    if (full_at < 0 && v->ride_full) { full_at = f; x_full = v->b.x; gap0 = sc_dist(0, 1); }
    if (full_at >= 0 && v->ride_by) { carried = v->b.x - x_full; if (v->load > load_max) load_max = v->load; if (f <= full_at + 60) gap_open = sc_dist(0, 1) - gap0; }
    if (stop && stop_at < 0) stop_at = f;
    for (int k = 0; k < SNEV; k++) if (SEV[k].type == EV_RIDE_END && end_zone < 0) end_zone = SEV[k].zone;
  }
  int und = sc_first(&L, EV_UNDERRIDE, PN_FRONT);
  SC_OK(und >= 0 && up >= 1.0f && L.first[EV_RIDE_END][RIDE_TIPPED] < 0 && carried > 2.0f && load_max < 0.3f && gap_open < 0.3f && (end_zone == RIDE_SLIP || end_zone == RIDE_BACKED_OFF),
        "UNDERRIDE at %d, ride_up %.2f, no tip %d, carried %.2f (> 2), load while carried %.2f (< 0.3), reversing opened the gap %.2f (< 0.3), the ride ended %d (SLIP 0 or BACKED OFF 1)",
        und, up, L.first[EV_RIDE_END][RIDE_TIPPED] < 0, carried, load_max, gap_open, end_zone);
}

/* two ROLL OUTs head on: the lips bind; with one at 0.4 throttle the faster gets under */
static int sc_wedge_vs_wedge(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_ROLL_OUT, PRE_ROLL_OUT, 0);
  sc_place(0, -3.0f, 0, HALF_PI); sc_place(1, 3.0f, 0, -HALF_PI);
  sc_log_t L; sc_log_init(&L); int clash = -1, both_slow = -1;
  for (int f = 0; f < 200; f++) {
    sc_pad(0, 1, 0, 0); sc_pad(1, 1, 0, 0); sc_step(); sc_log(&L);
    if (clash < 0 && L.count[EV_WEDGE_CLASH]) clash = f;
    if (clash >= 0 && both_slow < 0 && f <= clash + 10 && sc_speed(0) < 0.5f && sc_speed(1) < 0.5f) both_slow = f - clash;
  }
  int und1 = L.count[EV_UNDERRIDE];
  sc_init_presets(AR_BAY, PRE_ROLL_OUT, PRE_ROLL_OUT, 0);
  sc_place(0, -3.0f, 0, HALF_PI); sc_place(1, 3.0f, 0, -HALF_PI);
  int und_a = -1;
  for (int f = 0; f < 200; f++) {
    sc_pad(0, 1, 0, 0); sc_pad(1, 0.4f, 0, 0); sc_step();
    for (int k = 0; k < SNEV; k++) if (SEV[k].type == EV_UNDERRIDE && und_a < 0) und_a = SEV[k].a;
  }
  SC_OK(clash >= 0 && und1 == 0 && both_slow >= 0 && und_a == 0, "WEDGE_CLASH at %d, %d underrides (0), both under 0.5 u/s %d frames after (<= 10); at 0.4 throttle the underride is by slot %d (0, the faster)",
        clash, und1, both_slow, und_a);
}

/* ROLL OUT into a TOAST's side: the skirt is open at every edge; the hover is carried, never flipped */
static int sc_wedge_under_hover(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_ROLL_OUT, PRE_TOAST, 0);
  sc_place(0, -6.0f, 0, HALF_PI); sc_place(1, -2.5f, 0, 0);
  sc_log_t L; sc_log_init(&L); float x0 = -1e9f, carried = 0, load_max = 0;
  for (int f = 0; f < 300; f++) {
    sc_pad(0, 1, 0, 0); sc_pad(1, 0, 0, 0); sc_step(); sc_log(&L);
    bot_t *v = &SM.bots[1];
    if (v->ride_by) { if (x0 < -1e8f) x0 = v->b.x; carried = v->b.x - x0; if (v->load > load_max) load_max = v->load; }
  }
  SC_OK(L.count[EV_UNDERRIDE] >= 1 && load_max == 0 && carried > 3.0f && L.count[EV_INVERT] == 0, "%d underrides, load while ridden %.2f (0), carried %.2f (> 3), %d inversions (0)",
        L.count[EV_UNDERRIDE], load_max, carried, L.count[EV_INVERT]);
}

/* ROLL OUT into a CHOP's front at 3.8: under the LOWBOY but never over it; a boosted TWO WHEEL wedge does tip it */
static int sc_wedge_lowboy(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_ROLL_OUT, PRE_CHOP, 0);
  sc_place(0, -6.0f, 0, HALF_PI); sc_place(1, -2.0f, 0, -HALF_PI);
  sc_log_t L; sc_log_init(&L); float x_ride = -1e9f, carried = 0;
  for (int f = 0; f < 300; f++) {
    sc_pad(0, 1, 0, 0); sc_pad(1, 0, 0, 0); sc_step(); sc_log(&L);
    if (SM.bots[1].ride_by) { if (x_ride < -1e8f) x_ride = SM.bots[1].b.x; carried = SM.bots[1].b.x - x_ride; }
  }
  int und = L.count[EV_UNDERRIDE], tipped = L.first[EV_RIDE_END][RIDE_TIPPED] >= 0, slip = L.first[EV_RIDE_END][RIDE_SLIP] >= 0;
  build_t tw = { CH_BOX, DR_TWO, WP_WEDGE, WP_NONE, AR_ALU, 0, 0 }, ch; build_preset(&ch, PRE_CHOP);
  sc_init(AR_BAY, &tw, &ch, 0);
  sc_place(0, -9.5f, 0, HALF_PI); sc_place(1, 1.0f, 0, -HALF_PI);
  sc_log_t L2; sc_log_init(&L2); float vmax = 0;
  for (int f = 0; f < 240; f++) {
    sc_pad(0, 1, 0, f == 20 ? IN_L : 0); sc_pad(1, 0, 0, 0); sc_step(); sc_log(&L2);
    if (!SM.bots[1].ride_by && sc_speed(0) > vmax) vmax = sc_speed(0);
  }
  int tipped2 = L2.first[EV_RIDE_END][RIDE_TIPPED] >= 0;
  SC_OK(und >= 1 && !tipped && slip && carried > 1.0f && tipped2, "ROLL OUT: %d underrides, tipped %d (0), carried %.2f then slid off %d; the boosted TWO WHEEL wedge at %.1f u/s: tipped %d (1)",
        und, tipped, carried, slip, vmax, tipped2);
}

/* a ROLL OUT pressing A against a LAUNCH's front from a standstill: DIG IN, then under it within 40 frames */
static int sc_dig_in(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_ROLL_OUT, PRE_LAUNCH, 0);
  sc_place(0, -1.06f, 0, HALF_PI); sc_place(1, 0, 0, -HALF_PI);
  sc_log_t L; sc_log_init(&L);
  for (int f = 0; f < 60; f++) { sc_pad(0, 1, 0, f < 3 ? IN_A : 0); sc_pad(1, 0, 0, 0); sc_step(); sc_log(&L); }
  int tell = sc_first(&L, EV_WEAPON_TELL, TELL_DIG_IN), und = sc_first(&L, EV_UNDERRIDE, -1);
  SC_OK(tell >= 0 && und >= 0 && und - tell <= 40, "TELL_DIG_IN at %d, UNDERRIDE at %d (within 40)", tell, und);
}

/* ROLL OUT and LAUNCH head on, both flat out: the treads win the shove; with the LAUNCH ridden, further */
static int sc_push_treads(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_ROLL_OUT, PRE_LAUNCH, 0);
  sc_place(0, -4.0f, 0, HALF_PI); sc_place(1, -2.6f, 0, -HALF_PI);
  SM.bots[0].ride_cd[1] = 400;      /* a plain shove first: no underride */
  float x0 = (SM.bots[0].b.x + SM.bots[1].b.x) * 0.5f;
  for (int f = 0; f < 240; f++) { sc_pad(0, 1, 0, 0); sc_pad(1, 1, 0, 0); sc_step(); }
  float plain = (SM.bots[0].b.x + SM.bots[1].b.x) * 0.5f - x0;
  /* from a standstill nose to nose, the ROLL OUT digs in under the LAUNCH and both drive flat out */
  sc_init_presets(AR_BAY, PRE_ROLL_OUT, PRE_LAUNCH, 0);
  sc_place(0, -4.0f, 0, HALF_PI); sc_place(1, -4.0f + 1.06f, 0, -HALF_PI);
  x0 = (SM.bots[0].b.x + SM.bots[1].b.x) * 0.5f; int rode = 0;
  for (int f = 0; f < 240; f++) {
    sc_pad(0, 1, 0, f < 2 ? IN_A : 0); sc_pad(1, 1, 0, 0); sc_step(); if (SM.bots[1].ride_by) rode++;
    if (g_verbose) { printf("  f %d a (%.2f %.2f v %.2f) b (%.2f y %.2f v %.2f p %.2f) ride %d up %.2f full %d load %.2f/%.2f", f, SM.bots[0].b.x, SM.bots[0].b.z, SM.bots[0].b.vx, SM.bots[1].b.x, SM.bots[1].b.y, SM.bots[1].b.vx, SM.bots[1].b.pitch, SM.bots[1].ride_by, SM.bots[1].ride_up, SM.bots[1].ride_full, SM.bots[0].load, SM.bots[1].load); for (int k = 0; k < SNEV; k++) printf(" ev%d/%d", SEV[k].type, SEV[k].zone); printf("\n"); }
  }
  float ridden = (SM.bots[0].b.x + SM.bots[1].b.x) * 0.5f - x0;
  SC_OK(plain > 6.0f && ridden > 8.0f && rode > 0, "the pair moved %.2f toward the LAUNCH (> 6); with it ridden (%d frames of 240) %.2f (> 8)", plain, rode, ridden);
}

/* a LAUNCH flips a STOMP from its plate, 20 placements: every one lands on its lid */
static int sc_flip_inverts_tall(char *msg, int n) {
  int launches = 0, invs = 0, air_lo = 999, air_hi = 0; float dmg_lo = 99, dmg_hi = 0;
  for (int k = 0; k < 20; k++) {
    sc_init_presets(AR_BAY, PRE_LAUNCH, PRE_STOMP, 0);
    SM.seed = 100u + (uint32_t)k; SM.rng = SM.seed;
    float d = 1.0f + 0.0075f * (float)k;                  /* the STOMP's front from touching (k 0.8) to 0.14 out (k 0.5) */
    sc_place(0, -d, 0, HALF_PI); sc_place(1, 0, 0, -HALF_PI);
    sc_log_t L; sc_log_init(&L); int t_launch = -1, t_land = -1;
    for (int f = 0; f < 150; f++) {
      sc_pad(0, 0, 0, f < 2 ? IN_A : 0); sc_pad(1, 0, 0, 0); sc_step(); sc_log(&L);
      for (int e = 0; e < SNEV; e++) {
        if (SEV[e].type == EV_LAUNCH && t_launch < 0) t_launch = f;
        if (SEV[e].type == EV_LAND && SEV[e].b == 1 && SEV[e].zone != LAND_BOUNCE && t_land < 0) { t_land = f; if (SEV[e].dmg < dmg_lo) dmg_lo = SEV[e].dmg; if (SEV[e].dmg > dmg_hi) dmg_hi = SEV[e].dmg; }
      }
    }
    if (t_launch >= 0) launches++;
    if (L.first[EV_LAND][LAND_INVERTED] >= 0 && SM.bots[1].inverted) invs++;
    if (t_launch >= 0 && t_land >= 0) { int at = t_land - t_launch; if (at < air_lo) air_lo = at; if (at > air_hi) air_hi = at; }
  }
  SC_OK(launches == 20 && invs == 20 && air_lo >= 38 && air_hi <= 44 && dmg_lo >= 2 && dmg_hi <= 5, "%d launches, %d inverted (20 of 20), airtime %d to %d frames (38 to 44), landing dmg %.1f to %.1f (2 to 5)",
        launches, invs, air_lo, air_hi, dmg_lo, dmg_hi);
}

/* a standing CHOP lands back on its wheels; the same CHOP driving into the plate at 5 u/s goes over */
static int flip_chop(float drive_in, int *bounce, int *landed) {
  sc_init_presets(AR_BAY, PRE_LAUNCH, PRE_CHOP, 0);
  /* standing: touching the flipper's front; driving in: from 0.9 away at 5 u/s, the flipper fired as its
   * front crosses the plate's middle */
  sc_place(0, drive_in > 0 ? -1.9f : -1.0f, 0, HALF_PI); sc_place(1, 0, 0, -HALF_PI);
  SM.bots[1].b.vx = -drive_in;
  sc_log_t L; sc_log_init(&L); int fired = 0, fire_f = -1;
  for (int f = 0; f < 150; f++) {
    float near = (SM.bots[1].b.x - SM.bots[0].b.x) - SM.bots[1].b.hl;
    int press = drive_in > 0 ? (!fired && near < 0.72f) : f < 2;
    if (press && !fired) { fired = 1; fire_f = f; }
    sc_pad(0, 0, 0, press || (fire_f >= 0 && f < fire_f + 2) ? IN_A : 0); sc_pad(1, 0, 0, 0);
    if (drive_in > 0 && !fired) SM.bots[1].b.vx = -drive_in;
    sc_step(); sc_log(&L);
  }
  *bounce = L.first[EV_LAND][LAND_BOUNCE] >= 0; *landed = L.first[EV_LAND][LAND_WHEELS] >= 0 || L.first[EV_LAND][LAND_INVERTED] >= 0;
  return SM.bots[1].inverted;
}
static int sc_flip_lowboy(char *msg, int n) {
  int b1, l1, b2, l2;
  int stand = flip_chop(0, &b1, &l1), drive = flip_chop(5.0f, &b2, &l2);
  SC_OK(!stand && l1 && drive, "standing CHOP: inverted %d (0), bounced %d, landed %d; driving in at 5 u/s: inverted %d (1), bounced %d", stand, b1, l1, drive, b2);
}

/* a BOX at k 0.5 (its front 0.15 out from the flipper, half way along the plate) lands on an edge, bounces
 * once, and the second landing decides inside 90 frames */
static int sc_flip_bounce(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_LAUNCH, PRE_LAUNCH, 0);
  sc_place(0, -1.15f, 0, HALF_PI); sc_place(1, 0, 0, -HALF_PI);
  sc_log_t L; sc_log_init(&L); int t_launch = -1, t_end = -1; float pitch_at = 0;
  for (int f = 0; f < 150; f++) {
    float p = SM.bots[1].b.pitch;
    sc_pad(0, 0, 0, f < 2 ? IN_A : 0); sc_pad(1, 0, 0, 0); sc_step(); sc_log(&L);
    for (int e = 0; e < SNEV; e++) {
      if (SEV[e].type == EV_LAUNCH && t_launch < 0) t_launch = f;
      if (SEV[e].type == EV_LAND && SEV[e].b == 1 && SEV[e].zone == LAND_BOUNCE && pitch_at == 0) pitch_at = fabsf(p) * 57.29578f;
      if (SEV[e].type == EV_LAND && SEV[e].b == 1 && SEV[e].zone != LAND_BOUNCE && t_end < 0) t_end = f;
    }
  }
  SC_OK(L.first[EV_LAND][LAND_BOUNCE] >= 0 && pitch_at >= 60 && pitch_at <= 120 && t_end >= 0 && t_end - t_launch < 90,
        "first landing at %.0f degrees (60 to 120) is a BOUNCE %d, decided at %d frames after the launch (< 90), inverted %d", pitch_at, L.first[EV_LAND][LAND_BOUNCE] >= 0, t_end - t_launch, SM.bots[1].inverted);
}

/* the first EV_HIT on slot b during a run, or a zeroed event */
static int sc_hit_on(int b, event_t *out) { for (int k = 0; k < SNEV; k++) if (SEV[k].type == EV_HIT && SEV[k].b == b) { *out = SEV[k]; return 1; } return 0; }
static float reduction_of(int slot) { return ARMOURS[SM.bots[slot].build.armour].reduction; }

/* a full spin CHOP into a ROLL OUT's front: it glances (EVF_DEFLECT, under 9), hops up the slope 8 to 12
 * frames, and keeps most of its spin */
static int sc_spinner_wedge_glance(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_CHOP, PRE_ROLL_OUT, 0);
  sc_place(0, -2.4f, 0, HALF_PI); sc_place(1, 0, 0, -HALF_PI);
  SM.bots[0].spin = 1.0f;
  event_t e; int got = 0, air = 0, counting = 0; float spin_after = -1;
  for (int f = 0; f < 120; f++) {
    sc_pad(0, got ? 0 : 0.5f, 0, IN_A); sc_pad(1, 0, 0, 0); sc_step();
    if (!got && sc_hit_on(1, &e)) { got = 1; spin_after = SM.bots[0].spin; counting = 1; }
    if (counting) { if (SM.bots[0].airborne) air++; else if (air > 0) counting = 0; }
  }
  SC_OK(got && (e.flags & EVF_DEFLECT) && e.dmg < 9 && air >= 8 && air <= 12 && spin_after > 0.8f, "first hit flags %d (DEFLECT 2), dmg %.1f (< 9), the CHOP airborne %d frames (8 to 12), spin after %.2f (> 0.8), reason %d",
        e.flags, e.dmg, air, spin_after, e.reason);
}

/* a full spin CHOP charging a ROLL OUT's side: the capped hit, the disc spent, the victim sent; a LAUNCH skids */
static int sc_spinner_side_full(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_CHOP, PRE_ROLL_OUT, 0);
  sc_place(0, -2.6f, 0, HALF_PI); sc_place(1, 0, 0, 0);
  SM.bots[0].spin = 1.0f; SM.bots[0].b.vx = 4.0f;
  event_t e; int got = 0, after = -1; float spin_after = -1, vmax = 0;
  for (int f = 0; f < 60; f++) {
    sc_pad(0, 1, 0, IN_A); sc_pad(1, 0, 0, 0); sc_step();
    if (!got && sc_hit_on(1, &e)) { got = 1; spin_after = SM.bots[0].spin; after = 0; }
    if (after >= 0 && after < 5) { if (sc_speed(1) > vmax) vmax = sc_speed(1); after++; }
  }
  sc_init_presets(AR_BAY, PRE_CHOP, PRE_LAUNCH, 0);
  sc_place(0, -2.6f, 0.1f, HALF_PI); sc_place(1, 0, 0, 0);
  SM.bots[0].spin = 1.0f; SM.bots[0].b.vx = 4.0f;
  int slide = 0;
  for (int f = 0; f < 60; f++) { sc_pad(0, 1, 0, IN_A); sc_pad(1, 0, 0, 0); sc_step(); for (int k = 0; k < SNEV; k++) if (SEV[k].type == EV_SLIDE && SEV[k].a == 1) slide++; }
  SC_OK(got && e.dmg >= 16 && e.dmg <= 18 && spin_after < 0.6f && vmax > 2.0f && slide > 0, "ROLL OUT side: dmg %.1f (16 to 18), spin after %.2f (< 0.6), the ROLL OUT sent at %.2f u/s (> 2), reason %d; LAUNCH side: %d EV_SLIDE",
        e.dmg, spin_after, vmax, e.reason, slide);
}

/* a standing full spin CHOP: a LAUNCH backing into the disc at 6 u/s takes 1.4x what one leaving at 3 u/s does */
static float moving_hit(float vx) {
  sc_init_presets(AR_BAY, PRE_CHOP, PRE_LAUNCH, 0);
  sc_place(0, -1.65f, 0, HALF_PI); sc_place(1, vx < 0 ? 0.05f : -0.6f, 0, HALF_PI);   /* the LAUNCH's back to the disc */
  SM.bots[0].spin = 1.0f; SM.bots[1].b.vx = vx;
  event_t e; e.dmg = 0;
  for (int f = 0; f < 20; f++) {
    sc_pad(0, 0, 0, IN_A);
    if (vx > 0) { sc_pad(1, vx / DRIVES[DR_TWO].top, 0, 0); if (f == 0) SM.bots[1].b.vx = vx; } else { sc_pad(1, 0, 0, 0); SM.bots[1].b.vx = vx; }   /* away under its own drive, or shoved back into the disc */
    sc_step(); if (sc_hit_on(1, &e)) break;
  }
  return e.dmg;
}
static int sc_spinner_moving_target(char *msg, int n) {
  float in = moving_hit(-6.0f), out = moving_hit(3.0f);
  SC_OK(in > 0 && out > 0 && in >= 1.4f * out, "into the disc at 6 u/s: %.1f, away at 3 u/s: %.1f, ratio %.2f (>= 1.4)", in, out, out > 0 ? in / out : 0);
}

/* a CHOP on a LAUNCH's wheel: EVF_WHEEL, EV_WHEEL_HIT, wheel_hit 240 and the scooter at 0.6x top */
static int sc_spinner_wheel(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_CHOP, PRE_LAUNCH, 0);
  float wz = -0.2f * CHASSIS[CH_BOX].len;
  sc_place(0, -2.4f, wz, HALF_PI); sc_place(1, 0, 0, 0);
  SM.bots[0].spin = 1.0f;
  event_t e; int got = 0, wheel_ev = 0, wh = 0; float vmax = 0; int after = -1;
  for (int f = 0; f < 200; f++) {
    sc_pad(0, got ? -0.5f : 0.5f, 0, got ? 0 : IN_A); sc_pad(1, after >= 0 ? 1.0f : 0, 0, 0); sc_step();
    if (!got && sc_hit_on(1, &e)) { got = 1; after = 0; wh = SM.bots[1].wheel_hit; }
    for (int k = 0; k < SNEV; k++) if (SEV[k].type == EV_WHEEL_HIT && SEV[k].b == 1) wheel_ev++;
    if (after >= 0) { after++; if (after > 30 && after <= 90 && sc_speed(1) > vmax) vmax = sc_speed(1); }
  }
  float top = DRIVES[DR_TWO].top;
  SC_OK(got && (e.flags & EVF_WHEEL) && wheel_ev == 1 && wh >= 238 && vmax <= 0.6f * top + 0.05f && vmax > 0.5f * top, "flags %d (WHEEL 4), %d EV_WHEEL_HIT, wheel_hit %d (240), top over 60 frames %.2f (0.6 x %.1f = %.2f)",
        e.flags, wheel_ev, wh, vmax, top, 0.6f * top);
}

/* a STOMP's hammer at 1.2 u: the tall STOMP is met early in the swing, the low CHOP late; both on the TOP */
static void hammer_once(int victim, float dist, event_t *e, int *wear) {
  sc_init_presets(AR_BAY, PRE_STOMP, victim, 0);
  sc_place(0, -dist, 0, HALF_PI); sc_place(1, 0, 0, -HALF_PI);
  memset(e, 0, sizeof *e); *wear = 0; int got = 0;
  for (int f = 0; f < 200; f++) {
    sc_pad(0, 0, 0, (f % 100) < 2 ? IN_A : 0); sc_pad(1, 0, 0, 0); sc_step();
    for (int k = 0; k < SNEV; k++) { if (SEV[k].type == EV_HIT && SEV[k].b == 1 && !got) { *e = SEV[k]; got = 1; } if (SEV[k].type == EV_PANEL_WEAR && SEV[k].b == 1 && SEV[k].panel == PN_TOP) (*wear)++; }
    SM.bots[0].cooldown = SM.bots[0].cooldown > 2 && f > 30 ? 2 : SM.bots[0].cooldown;    /* the second swing comes at frame 100 */
    sc_place(1, 0, 0, -HALF_PI);                                                         /* the victim held still for the swing */
    sc_place(0, -dist, 0, HALF_PI);
  }
}
static int sc_hammer_arc(char *msg, int n) {
  event_t a, b, c; int wa, wb, wc;
  hammer_once(PRE_STOMP, 1.2f, &a, &wa); hammer_once(PRE_CHOP, 1.2f, &b, &wb);
  build_t ham = { CH_BOX, DR_FOUR, WP_HAMMER, WP_NONE, AR_ALU, 0, 0 }, tall; build_preset(&tall, PRE_STOMP);
  sc_init(AR_BAY, &ham, &tall, 0);                 /* a low hammer bot hugging a tall one: the arm meets it first */
  sc_place(0, -1.0f, 0, HALF_PI); sc_place(1, 0, 0, -HALF_PI);
  memset(&c, 0, sizeof c);
  for (int f = 0; f < 60; f++) { sc_pad(0, 0, 0, f < 2 ? IN_A : 0); sc_pad(1, 0, 0, 0); sc_step(); event_t t; if (!c.type && sc_hit_on(1, &t)) c = t; }
  float pa = a.speed / TIP_SWING - 0.5f, pb = b.speed / TIP_SWING - 0.5f;
  float ra = 20.0f * pa, rb = 20.0f * pb, rc = c.dmg / (1.0f - reduction_of(1));
  wc = 0; (void)wc;
  /* the arc with a 1.0 reach meets the STOMP's lid at p 0.6 to 0.75 and the CHOP's at 0.85 to 1.0 (the plan's
   * 0.2 to 0.4 and 0.8 to 1.0 came from an arc that never reached the floor); the damage is the swing's
   * energy, base x p, which lands in its 11 to 14 and 18 to 20 */
  SC_OK(a.dmg > 0 && b.dmg > 0 && pa >= 0.6f && pa <= 0.75f && pb >= 0.85f && pb <= 1.0f && a.panel == PN_TOP && b.panel == PN_TOP && (a.flags & EVF_TOP) && (b.flags & EVF_TOP) && wb >= 1 && rc > 0 && rc < 8.0f,
        "STOMP met at p %.2f (raw %.1f, dealt %.1f), CHOP met at p %.2f (raw %.1f, dealt %.1f), both TOP %d %d, CHOP TOP wear events %d; the handle on a hugging STOMP %.1f raw (< 8)",
        pa, ra, a.dmg, pb, rb, b.dmg, a.panel == PN_TOP, b.panel == PN_TOP, wb, rc);
}

/* a SPIKE: a target in the rod's way on the way out takes the full 15, one sliding in on the way back 0.3 of it */
static int sc_spike_extend(char *msg, int n) {
  build_t sp = { CH_BOX, DR_FOUR, WP_SPIKE, WP_NONE, AR_ALU, 0, 0 }, la; build_preset(&la, PRE_LAUNCH);
  sc_init(AR_BAY, &sp, &la, 0);
  sc_place(0, -1.3f, 0, HALF_PI); sc_place(1, 0, 0, HALF_PI);   /* the LAUNCH's back face 0.8 in front of the spike's centre */
  event_t e1; memset(&e1, 0, sizeof e1); int f1 = -1;
  for (int f = 0; f < 30; f++) { sc_pad(0, 0, 0, f < 2 ? IN_A : 0); sc_pad(1, 0, 0, 0); sc_step(); event_t t; if (!e1.type && sc_hit_on(1, &t)) { e1 = t; f1 = WEAPONS[WP_SPIKE].active + 1 - SM.bots[0].active; } }
  sc_init(AR_BAY, &sp, &la, 0);
  sc_place(0, -1.3f, 0, HALF_PI); sc_place(1, 0, 0.125f + 0.4f + 0.05f * 5.5f, HALF_PI);   /* beside the rod, sliding across it into frame 6 */
  event_t e2; memset(&e2, 0, sizeof e2); int f2 = -1;
  for (int f = 0; f < 30; f++) {
    sc_pad(0, 0, 0, f < 1 ? IN_A : 0); sc_pad(1, 0, 0, 0); SM.bots[1].b.vz = -3.0f; sc_step();
    event_t t; if (!e2.type && sc_hit_on(1, &t)) { e2 = t; f2 = WEAPONS[WP_SPIKE].active + 1 - SM.bots[0].active; }
  }
  float r1 = e1.dmg / (1.0f - reduction_of(1)), r2 = e2.dmg / (1.0f - reduction_of(1));
  SC_OK(f1 >= 1 && f1 <= 5 && r1 > 14.9f && f2 >= 6 && r2 > 0 && r2 < 0.31f * 15.0f, "out: hit in frame %d (1 to 5) for %.1f raw (15); back: hit in frame %d (6 to 9) for %.1f raw (0.3 x 15)", f1, r1, f2, r2);
}

/* a CRUSHER closes on a LAUNCH: 15 on the bite, then 3 every 30 frames for 120 (6 a second) through the TOP,
 * the victim's own drive does nothing */
static int sc_crusher_hold(char *msg, int n) {
  build_t cr = { CH_BOX, DR_FOUR, WP_CRUSHER, WP_NONE, AR_ALU, 0, 0 }, la; build_preset(&la, PRE_LAUNCH);
  sc_init(AR_BAY, &cr, &la, 0);
  sc_place(0, -1.15f, 0, HALF_PI); sc_place(1, 0, 0, HALF_PI);
  event_t e; memset(&e, 0, sizeof e); int ticks = 0; float tick_dmg = 0, top0 = SM.bots[1].panel_hp[PN_TOP], d0 = -1, dmax = 0;
  for (int f = 0; f < 200; f++) {
    sc_pad(0, 0, 0, f < 2 ? IN_A : 0); sc_pad(1, SM.bots[1].clamped_by ? 1.0f : 0, 0, 0); sc_step();
    event_t t; if (!e.type && sc_hit_on(1, &t)) e = t;
    for (int k = 0; k < SNEV; k++) if (SEV[k].type == EV_GRIND && SEV[k].zone == GRIND_TICK) { ticks++; tick_dmg += SEV[k].dmg; }
    if (SM.bots[1].clamped_by) { float d = sc_dist(0, 1); if (d0 < 0) d0 = d; if (fabsf(d - d0) > dmax) dmax = fabsf(d - d0); }
  }
  float raw = e.dmg / (1.0f - reduction_of(1)), worn = top0 - SM.bots[1].panel_hp[PN_TOP];
  SC_OK(raw > 14.9f && raw < 15.1f && ticks == 4 && tick_dmg > 11.9f && tick_dmg < 12.1f && worn > 3.9f && worn < 4.1f && dmax < 0.01f,
        "the bite %.1f raw (15), %d GRIND_TICKs for %.1f over the hold (6 a second for 2 s), TOP pool wore %.1f (4), the victim's own drive moved it %.3f (0)", raw, ticks, tick_dmg, worn, dmax);
}

/* a WEDGE plus SAW build holding its blade on a LAUNCH's side: a tick every 10 frames, the side plate wears
 * a point every 6, the grind starts and ends */
static int sc_saw_grind(char *msg, int n) {
  build_t ws = { CH_BOX, DR_TWO, WP_WEDGE, WP_SAW, AR_ALU, 0, 0 }, la; build_preset(&la, PRE_LAUNCH);
  sc_init(AR_BAY, &ws, &la, 0);
  sc_place(0, -1.25f, 0.3f, HALF_PI); sc_place(1, 0, 0, 0);       /* the blade on the LAUNCH's side, ahead of its wheels */
  SM.bots[0].spin2 = 1.0f; SM.bots[0].saw_on = 1;
  int ticks = 0, start = 0, end = -1, gt_max = 0, gaps = 0, prev_gt = 0; float tick_raw = 0, side0 = 0; int side = -1;
  for (int f = 0; f < 200; f++) {
    int hold = f < 120;
    sc_pad(0, hold ? 0 : -0.6f, 0, 0); sc_pad(1, 0, 0, 0);
    sc_place(1, 0, 0, 0); if (hold) sc_place(0, -1.25f, 0.3f, HALF_PI);
    sc_step();
    for (int k = 0; k < SNEV; k++) {
      const event_t *e = &SEV[k];
      if (e->type == EV_HIT && e->b == 1 && e->weapon == WP_SAW) { ticks++; tick_raw = e->dmg / (1.0f - reduction_of(1)); if (side < 0) { side = e->panel; side0 = ARMOUR_PANEL_HP[AR_ALU]; } }
      if (e->type == EV_GRIND && e->zone == GRIND_START) start++;
      if (e->type == EV_GRIND && e->zone == GRIND_END && end < 0) end = f;
    }
    int gt = SM.bots[0].grind_t; if (gt > gt_max) gt_max = gt; if (hold && prev_gt > 0 && gt != prev_gt + 1) gaps++; prev_gt = gt;
  }
  float worn = side >= 0 ? side0 - SM.bots[1].panel_hp[side] : 0;
  int st = side >= 0 ? SM.bots[1].panel_state[side] : -1;
  SC_OK(ticks >= 11 && ticks <= 13 && tick_raw > 3.99f && tick_raw < 4.01f && worn >= 19 && worn <= 21 && st == 2 && start == 1 && end >= 0 && gaps == 0,
        "%d ticks (12) of %.1f raw (4), side %d wore %.0f (20) to state %d (2), GRIND_START %d, END at %d, grind_t continuous %d (max %d)", ticks, tick_raw, side, worn, st, start, end, gaps == 0, gt_max);
}

/* a TOAST holding its flame on a LAUNCH: 4 ticks of 2, one burn of 3 over 180 frames, no relight while it
 * burns; a TOAST on a TOAST (UHMW) never burns */
static int sc_flame_burn(char *msg, int n) {
  sc_init_presets(AR_BAY, PRE_TOAST, PRE_LAUNCH, 0);
  sc_place(0, -1.55f, 0, HALF_PI); sc_place(1, 0, 0, HALF_PI);
  int ticks = 0, starts = 0, end_f = -1, start_f = -1; float tick_raw = 0, hp_burn = 0;
  for (int f = 0; f < 320; f++) {
    sc_pad(0, 0, 0, f < 120 ? IN_A : 0); sc_pad(1, 0, 0, 0); sc_place(1, 0, 0, HALF_PI); sc_place(0, -1.55f, 0, HALF_PI);
    float hp0 = SM.bots[1].hp;
    sc_step();
    int ticked = 0;
    for (int k = 0; k < SNEV; k++) {
      const event_t *e = &SEV[k];
      if (e->type == EV_HIT && e->b == 1) { ticks++; ticked = 1; tick_raw = e->dmg / (1.0f - reduction_of(1)); }
      if (e->type == EV_BURN_START && e->b == 1) { starts++; if (start_f < 0) start_f = f; }
      if (e->type == EV_BURN_END && e->b == 1 && end_f < 0) end_f = f;
    }
    hp_burn += hp0 - SM.bots[1].hp;
    for (int k = 0; k < SNEV; k++) if (SEV[k].type == EV_HIT && SEV[k].b == 1) hp_burn -= SEV[k].dmg;
    (void)ticked;
  }
  float burn_raw = hp_burn / (1.0f - reduction_of(1));
  sc_init_presets(AR_BAY, PRE_TOAST, PRE_TOAST, 0);
  sc_place(0, -1.65f, 0, HALF_PI); sc_place(1, 0, 0, HALF_PI);
  int toast_burn = 0, toast_ticks = 0;
  for (int f = 0; f < 150; f++) { sc_pad(0, 0, 0, IN_A); sc_pad(1, 0, 0, 0); sc_place(1, 0, 0, HALF_PI); sc_place(0, -1.65f, 0, HALF_PI); sc_step(); for (int k = 0; k < SNEV; k++) { if (SEV[k].type == EV_BURN_START) toast_burn++; if (SEV[k].type == EV_HIT && SEV[k].b == 1) toast_ticks++; } }
  SC_OK(ticks == 4 && tick_raw > 1.99f && tick_raw < 2.01f && starts == 1 && burn_raw > 2.95f && burn_raw < 3.05f && end_f - start_f >= 178 && end_f - start_f <= 182 && toast_burn == 0 && toast_ticks > 0,
        "%d ticks (4) of %.1f raw (2), %d BURN_START (1), the burn %.2f raw (3) ending %d frames later (180), TOAST on TOAST: %d burns (0) under %d ticks",
        ticks, tick_raw, starts, burn_raw, end_f - start_f, toast_burn, toast_ticks);
}

/* where a hit lands is where the plate wears (3.10): two spike hits on a CHOP's left side and two disc hits
 * on its right; FRONT and BACK stay clean until HP 50; the third left hit sheds LEFT, the fourth lands on
 * the bare frame (EVF_STRIPPED, no armour, 1.25x) */
static void panel_spike(int left) {
  /* a spike bot in front of the CHOP's side, fired once */
  float x = left ? -1.3f : 1.3f;
  SM.bots[0].b.x = x; SM.bots[0].b.z = 0; SM.bots[0].b.h = left ? HALF_PI : -HALF_PI; SM.bots[0].b.vx = SM.bots[0].b.vz = SM.bots[0].b.w = 0;
  SM.bots[0].cooldown = 0; SM.bots[0].active = 0;
  for (int f = 0; f < 20; f++) {
    sc_pad(0, 0, 0, f < 2 ? IN_A : 0); sc_pad(1, 0, 0, 0); SM.bots[1].spin = 0.4f; sc_place(1, 0, 0, 0); SM.bots[0].b.x = x; SM.bots[0].b.z = 0; SM.bots[0].b.vx = SM.bots[0].b.vz = 0; sc_step();
    if (g_verbose) for (int k = 0; k < SNEV; k++) printf("  spike f %d ev %d b %d zone %d dmg %.1f panel %d flags %d pools %.0f %.0f %.0f %.0f\n", f, SEV[k].type, SEV[k].b, SEV[k].zone, SEV[k].dmg, SEV[k].panel, SEV[k].flags, SM.bots[1].panel_hp[0], SM.bots[1].panel_hp[1], SM.bots[1].panel_hp[2], SM.bots[1].panel_hp[3]);
  }
}
static int sc_panel_location(char *msg, int n) {
  build_t sp = { CH_BOX, DR_FOUR, WP_SPIKE, WP_NONE, AR_ALU, 0, 0 }, ch; build_preset(&ch, PRE_CHOP);
  sc_init(AR_BAY, &sp, &ch, 0);
  sc_place(0, -1.3f, 0, HALF_PI); sc_place(1, 0, 0, 0);
  bot_t *v = &SM.bots[1];
  /* the CHOP faces +z: its local +x (PN_RIGHT) is world +x, PN_LEFT world -x */
  panel_spike(1); panel_spike(1);
  int left_after2 = v->panel_state[PN_LEFT], fb_clean = v->panel_state[PN_FRONT] == 0 && v->panel_state[PN_BACK] == 0; float hp_after_left = v->hp;
  /* two disc hits on the right from a spinner at 0.85 spin closing at 1 u/s */
  build_t hz; build_preset(&hz, PRE_CHOP);
  SM.bots[0].build = hz; bot_body_from_build(&SM.bots[0].b, &hz);
  for (int k = 0; k < 2; k++) {
    SM.bots[0].b.x = 1.5f; SM.bots[0].b.z = 0; SM.bots[0].b.h = -HALF_PI; SM.bots[0].spin = 0.85f; memset(SM.bots[0].hit_cd, 0, sizeof SM.bots[0].hit_cd);
    for (int f = 0; f < 30; f++) {
      sc_pad(0, 0, 0, IN_A); sc_pad(1, 0, 0, 0); SM.bots[1].spin = 0.4f; SM.bots[0].spin = 0.85f; sc_place(1, 0, 0, 0); SM.bots[0].b.vx = -1.0f; SM.bots[0].b.vz = 0; SM.bots[0].b.w = 0;
      sc_step(); event_t t;
      if (g_verbose) for (int q = 0; q < SNEV; q++) printf("  disc k %d f %d ev %d a %d b %d zone %d dmg %.1f panel %d x %.2f spin %.2f w %d speed %.2f j %.2f flags %d reason %d hp %.1f\n", k, f, SEV[q].type, SEV[q].a, SEV[q].b, SEV[q].zone, SEV[q].dmg, SEV[q].panel, SM.bots[0].b.x, SM.bots[0].spin, SEV[q].weapon, SEV[q].speed, SEV[q].j, SEV[q].flags, SEV[q].reason, SM.bots[1].hp);
      if (sc_hit_on(1, &t)) break;
    }
    if (k == 0) fb_clean = fb_clean && (v->hp > 50 ? v->panel_state[PN_FRONT] == 0 && v->panel_state[PN_BACK] == 0 : 1);
  }
  int right_after2 = v->panel_state[PN_RIGHT];
  /* back to the spike for the third and fourth left hits */
  SM.bots[0].build = sp; bot_body_from_build(&SM.bots[0].b, &sp); SM.bots[0].spin = 0;
  int shed_left = 0; event_t third, fourth; memset(&third, 0, sizeof third); memset(&fourth, 0, sizeof fourth);
  for (int k = 0; k < 2; k++) {
    float x = -1.3f;
    SM.bots[0].b.x = x; SM.bots[0].b.h = HALF_PI; SM.bots[0].cooldown = 0; SM.bots[0].active = 0; memset(SM.bots[0].hit_cd, 0, sizeof SM.bots[0].hit_cd);
    for (int f = 0; f < 20; f++) {
      sc_pad(0, 0, 0, f >= 2 && f < 4 ? IN_A : 0); sc_pad(1, 0, 0, 0); SM.bots[1].spin = 0.4f; sc_place(1, 0, 0, 0); SM.bots[0].b.x = x; SM.bots[0].b.z = 0; SM.bots[0].b.h = HALF_PI; SM.bots[0].b.vx = SM.bots[0].b.vz = SM.bots[0].b.w = 0;
      sc_step();
      for (int q = 0; q < SNEV; q++) {
        if (SEV[q].type == EV_SHED && SEV[q].b == 1 && SEV[q].zone == PN_LEFT) shed_left++;
        if (SEV[q].type == EV_HIT && SEV[q].b == 1) { if (k == 0) third = SEV[q]; else fourth = SEV[q]; }
        if (g_verbose) printf("  left k %d f %d ev %d b %d zone %d dmg %.1f panel %d flags %d hp %.1f\n", k, f, SEV[q].type, SEV[q].b, SEV[q].zone, SEV[q].dmg, SEV[q].panel, SEV[q].flags, SM.bots[1].hp);
      }
    }
  }
  SC_OK(left_after2 == 3 && right_after2 == 2 && fb_clean && hp_after_left > 50 && shed_left == 1 && third.panel == PN_LEFT && (fourth.flags & EVF_STRIPPED) && fourth.dmg >= 1.25f * third.dmg - 0.01f,
        "LEFT state %d (3) after two spikes, RIGHT state %d (2) after two discs, FRONT and BACK clean above HP 50 %d, LEFT shed by the third %d, fourth on the bare frame flags %d (STRIPPED 8) for %.1f against %.1f (>= 1.25x)",
        left_after2, right_after2, fb_clean, shed_left, fourth.flags, fourth.dmg, third.dmg);
}

static const scenario_t SCENARIOS[] = {
  { "ghost-sim", sc_ghost_sim },
  { "mound-contact", sc_mound_contact },
  { "corpse-hittable", sc_corpse_hittable },
  { "slide-drives", sc_slide_drives },
  { "wedge-front-lunchbox", sc_wedge_front_lunchbox },
  { "wedge-side-lunchbox", sc_wedge_side_lunchbox },
  { "wedge-front-box-slow", sc_wedge_front_box_slow },
  { "wedge-vs-wedge", sc_wedge_vs_wedge },
  { "wedge-under-hover", sc_wedge_under_hover },
  { "wedge-lowboy", sc_wedge_lowboy },
  { "push-treads", sc_push_treads },
  { "dig-in", sc_dig_in },
  { "flip-inverts-tall", sc_flip_inverts_tall },
  { "flip-lowboy", sc_flip_lowboy },
  { "flip-bounce", sc_flip_bounce },
  { "spinner-wedge-glance", sc_spinner_wedge_glance },
  { "spinner-side-full", sc_spinner_side_full },
  { "spinner-moving-target", sc_spinner_moving_target },
  { "spinner-wheel", sc_spinner_wheel },
  { "hammer-arc", sc_hammer_arc },
  { "spike-extend", sc_spike_extend },
  { "crusher-hold", sc_crusher_hold },
  { "saw-grind", sc_saw_grind },
  { "flame-burn", sc_flame_burn },
  { "panel-location", sc_panel_location },
};

static int scenarios(const char *which) {
  int fail = 0, ran = 0;
  for (size_t i = 0; i < sizeof SCENARIOS / sizeof SCENARIOS[0]; i++) {
    if (strcmp(which, "all") && strcmp(which, SCENARIOS[i].name)) continue;
    char msg[512] = { 0 };
    int ok = SCENARIOS[i].run(msg, sizeof msg);
    printf("%s %-22s %s\n", ok ? "PASS" : "FAIL", SCENARIOS[i].name, msg);
    if (!ok) fail = 1;
    ran++;
  }
  if (!ran) { fprintf(stderr, "scenario: no row named %s\n", which); return 2; }
  printf(fail ? "scenario: FAIL\n" : "scenario: PASS\n");
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
  if (!strcmp(argv[1], "--scenario")) return scenarios(argc > 2 ? argv[2] : "all");
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
