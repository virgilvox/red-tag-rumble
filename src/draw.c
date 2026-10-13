/* draw.c: the fight HUD and the post-fight screens (spec 10.4, 10.5, 10.14 to
 * 10.21, 10.26). Owned by the HUD agent (items 9, 10, 16).
 *
 * Everything here is authored in 1280 x 720 reference space and painted with
 * the hud.c primitives; hud.c divides by HUD_DIV. draw_hud is the one entry
 * point main.c calls between render_frame and the HUD pass; it dispatches on
 * G.phase and hands the menu phases to draw_menus.c.
 *
 * RTR_FAKE_HUD: a test entry that fills M and G with a staged match and walks
 * every fight screen in 200 frame stages so the rig can screenshot each one:
 *   EXTRA_CFLAGS=-DRTR_FAKE_HUD ./build.sh
 *   node tools/shot.mjs build/red-tag-rumble.wasc build/shots/fake 150,350,550,750,950,1150,1350,1550,1750,1950,2150,2350
 * (2p halves, 1p full, 3p, 4p with a tell, hazard seat, KO, results, rematch,
 * judges, pause, bracket, closing time). Nothing of it is compiled into a
 * normal cart. */
#include "draw.h"
#include "draw_menus.h"
#include "content.h"
#include "juice.h"
#include "fx.h"
#include "bot.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ── state ─────────────────────────────────────────────────────── */
static rect_t views[4];
static int nviews, view_layout;
static int view_seat[4];
static float T;                                    /* wall clock of this file */

typedef struct { float x, y, z; char text[12]; uint32_t rgb; int big; float age; int live; } num_t;
static num_t NUMS[4][J_NUMBERS_PER_VIEW];

typedef struct { char s[24]; uint32_t c; float frames; int live; } stinger_t;
static stinger_t STING[4];

/* the ghost HP drain per bot slot (10.4) */
typedef struct { float ghost, last, hold; int seen; } ghost_t;
static ghost_t GHOST[MAX_BOTS];

/* one OPS speech panel on screen at a time (section 7) */
static struct { char line[32]; float x, y, z, t; } SPEECH;

/* results: confetti fired once, the dirty hash of the last frame */
static int results_confetti_done;
static float rematch_focus_y;
static uint32_t rematch_hash_prev;

/* ── helpers ───────────────────────────────────────────────────── */
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/* text top so a line box of font f is centred in a box of height h at y */
static int cy_text(int font, int y, int h) { int fh = hud_font_h(font); return y + h / 2 - fh * 13 / 25; }

/* the arena of the current match (the fake rig substitutes its staged table) */
#ifdef RTR_FAKE_HUD
static arena_def_t FAKE_ARENA;
static const arena_def_t *cur_arena(void) { return &FAKE_ARENA; }
#else
static const arena_def_t *cur_arena(void) { return &ARENAS[M.arena % 6]; }
#endif

/* a slot that was set up (bot_setup gives every bot an hp_max; OPS flags itself) */
static int bot_in_use(const bot_t *b) { return b->is_ops || b->hp_max > 0; }
/* a player bot: set up, not OPS */
static int bot_is_player(const bot_t *b) { return !b->is_ops && b->hp_max > 0; }

/* the bot a seat drives, or NULL */
static const bot_t *seat_bot(int seat) {
  if (seat < 0 || seat > 3) return NULL;
  for (int i = 0; i < M.nbots && i < MAX_BOTS; i++) if (bot_is_player(&M.bots[i]) && M.bots[i].seat == seat) return &M.bots[i];
  return NULL;
}
static int bot_slot(const bot_t *b) { return (int)(b - M.bots); }
static int bot_seat_colour_index(const bot_t *b) { return b->seat & 3; }

/* draw text that must fit a width: the font steps down until it fits */
static int fit_font(int font, const char *s, int w) {
  static const int chain[] = { F_TITLE, F_H1, F_H2, F_H3, F_MONO_S };
  int k = 0;
  while (k < 4 && chain[k] != font) k++;
  if (font == F_MONO_L) { if (hud_text_w(F_MONO_L, s) <= w) return F_MONO_L; return hud_text_w(F_MONO, s) <= w ? F_MONO : F_MONO_S; }
  if (font == F_MONO) return hud_text_w(F_MONO, s) <= w ? F_MONO : F_MONO_S;
  if (font == F_STINGER) return hud_text_w(F_STINGER, s) <= w ? F_STINGER : F_STINGER_Q;
  for (; k < 5; k++) if (hud_text_w(chain[k], s) <= w) return chain[k];
  return F_MONO_S;
}

/* clip a string to a width by dropping characters from the end */
static void fit_text(char *dst, int n, int font, const char *s, int w) {
  snprintf(dst, (size_t)n, "%s", s);
  int len = (int)strlen(dst);
  while (len > 1 && hud_text_w(font, dst) > w) dst[--len] = 0;
}

/* a world point in viewport i's camera, in HUD space; the fake rig projects a top down map */
static int project(int viewport, float x, float y, float z, float *sx, float *sy) {
#ifdef RTR_FAKE_HUD
  if (viewport < 0 || viewport >= nviews) return 0;
  const rect_t *r = &views[viewport];
  const arena_def_t *a = cur_arena();
  *sx = r->x + r->w * (0.5f + x / a->w * 0.8f);
  *sy = r->y + r->h * (0.55f + z / a->d * 0.6f) - y * 40.f;
  return 1;
#else
  return cam_project(viewport, x, y, z, sx, sy);
#endif
}

/* ── entry ─────────────────────────────────────────────────────── */
void draw_set_views(const rect_t *vp, int n, int layout) {
  for (int i = 0; i < n && i < 4; i++) { views[i] = vp[i]; view_seat[i] = cam_seat_of(i); }
  nviews = n > 4 ? 4 : n; view_layout = layout;
}

/* ── damage numbers (10.4) ─────────────────────────────────────── */
static void number_add(int viewport, float x, float y, float z, const char *s, uint32_t rgb, int big) {
  if (viewport < 0 || viewport >= 4) return;
  num_t *slot = NULL;
  float oldest = -1;
  for (int k = 0; k < J_NUMBERS_PER_VIEW; k++) {
    num_t *n = &NUMS[viewport][k];
    if (!n->live) { slot = n; break; }
    if (n->age > oldest) { oldest = n->age; slot = n; }
  }
  slot->x = x; slot->y = y; slot->z = z; slot->rgb = rgb; slot->big = big; slot->age = 0; slot->live = 1;
  snprintf(slot->text, sizeof slot->text, "%s", s);
}

void damage_number(int viewport, float x, float y, float z, int value, uint32_t rgb, int big) {
  char s[12];
  snprintf(s, sizeof s, value > 0 && rgb == C_LIVE ? "+%d" : "%d", value);
  number_add(viewport, x, y, z, s, rgb, big);
}
void damage_text(int viewport, float x, float y, float z, const char *s, uint32_t rgb) { number_add(viewport, x, y, z, s, rgb, 1); }

static void numbers_draw(int viewport, const rect_t *vp, float dt) {
  hud_clip(vp->x, vp->y, vp->w, vp->h);
  for (int k = 0; k < J_NUMBERS_PER_VIEW; k++) {
    num_t *n = &NUMS[viewport][k];
    if (!n->live) continue;
    n->age += dt * 60.f;
    if (n->age >= J_NUMBER_LIFE) { n->live = 0; continue; }
    float sx, sy;
    if (!project(viewport, n->x, n->y, n->z, &sx, &sy)) continue;
    float f = n->age / J_NUMBER_LIFE;
    int a = n->age > J_NUMBER_LIFE - 18 ? (int)(255 * (J_NUMBER_LIFE - n->age) / 18) : 255;
    int y = (int)(sy - J_NUMBER_RISE_PX * f) - 20;
    int font = n->big ? F_H2 : F_MONO_L;
    hud_text(font, (int)sx + 2, y + 2, n->text, C_CHAR, a, AL_CENTER);
    hud_text(font, (int)sx, y, n->text, n->rgb, a, AL_CENTER);
  }
  hud_clip_clear();
}

/* ── stingers (10.5) ───────────────────────────────────────────── */
void stinger(int viewport, const char *s, uint32_t seat_colour) {
  if (viewport < 0 || viewport >= 4 || !s) return;
  stinger_t *st = &STING[viewport];
  snprintf(st->s, sizeof st->s, "%s", s);
  st->c = seat_colour; st->frames = 0; st->live = 1;
}
void stinger_all(const char *s, uint32_t seat_colour) { for (int i = 0; i < 4; i++) stinger(i, s, seat_colour); }

static void stinger_draw(int viewport, const rect_t *vp, int compact, float dt) {
  stinger_t *st = &STING[viewport];
  if (!st->live) return;
  st->frames += dt * 60.f;
  if (st->frames >= J_STINGER_FRAMES) { st->live = 0; return; }
  float in = clampf(st->frames / 6.f, 0, 1), out = clampf((J_STINGER_FRAMES - st->frames) / 6.f, 0, 1);
  int a = (int)(255 * (in < out ? in : out));
  int dx = (int)(-20 * (1 - hud_ease_out(in)));
  int h = compact ? 48 : 72;
  int y = vp->y + (compact ? 150 : 400);
  if (y + h > vp->y + vp->h - (compact ? 56 : 72) - 8) y = vp->y + vp->h / 2 - h / 2;
  int font = compact ? F_STINGER_Q : fit_font(F_STINGER, st->s, vp->w - 48);
  hud_set_alpha(a);
  hud_rect(vp->x + dx + 6, y + 6, vp->w, h, st->c, 255);
  hud_rect(vp->x + dx, y, vp->w, h, C_CHAR, 255);
  char s[24];
  fit_text(s, sizeof s, font, st->s, vp->w - 32);
  hud_text(font, vp->x + dx + vp->w / 2, cy_text(font, y, h), s, C_BONE, 255, AL_CENTER);
  hud_set_alpha(255);
  hud_dirty(vp->x - 20, y, vp->w + 26, h + 6);
}

/* ── the clock chip (10.5) ─────────────────────────────────────── */
void clock_chip(const match_t *m, int compact) {
  int h = compact ? 44 : 56, x = 580, y = 6, w = 120;
  int secs = (int)ceilf(m->clock_s > 0 ? m->clock_s : 0);
  int closing = m->closing && m->clock_s > 0;
  int blink = ((int)(T * 4) & 1);
  uint32_t ink = closing ? C_RUST : C_BONE;
  int a = closing && blink ? 140 : 255;
  hud_rect(x + 6, y + 6, w, h, C_SHADOW, 255);
  hud_rect(x, y, w, h, C_CHAR, 255);
  char s[8];
  snprintf(s, sizeof s, "%d:%02d", secs / 60, secs % 60);
  int font = compact ? F_H2 : F_H1;
  hud_text(font, x + w / 2, cy_text(font, y - 2, h - 6), s, ink, a, AL_CENTER);
  float total = m->fight_len_s > 0 ? (float)m->fight_len_s : 75.f;
  float frac = clampf(m->clock_s / total, 0, 1);
  hud_rect(x + 8, y + h - 8, w - 16, 4, C_STEEL, 255);
  hud_rect(x + 8, y + h - 8, (int)((w - 16) * frac), 4, closing ? C_RUST : C_HAZARD, a);
  hud_dirty(x, y, w + 6, h + 6);
}

/* ── tape seams, hazard tape, white, cut (10.5, 9.3) ───────────── */
void tape_seams(int layout) {
  switch (layout) {
    case LAY_HALVES: hud_tape(638, 0, 4, 720, G.tape_phase); break;
    case LAY_QUADS: case LAY_THREE: hud_tape(638, 0, 4, 720, G.tape_phase); hud_tape(0, 358, 1280, 4, G.tape_phase); break;
    default: break;
  }
}

void hazard_tape_strip(const rect_t *vp, float phase) {
  hud_tape(vp->x, vp->y, vp->w, 24, phase);
  hud_dirty(vp->x, vp->y, vp->w, 24);
}

void viewport_white(const rect_t *vp, int alpha) { if (alpha > 0) hud_rect(vp->x, vp->y, vp->w, vp->h, C_WHITE, alpha); }

/* two char curtains with 24 px tape leading edges meet at the centre and part again */
void tape_cut(int frames_left) {
  if (frames_left <= 0) return;
  float p;
  if (frames_left > J_TAPE_CUT_FRAMES / 2) p = (J_TAPE_CUT_FRAMES - frames_left) / 12.f;
  else p = frames_left / 12.f;
  p = hud_ease_inout(clampf(p, 0, 1));
  int w = (int)(640 * p);
  if (w <= 0) return;
  hud_rect(0, 0, w, 720, C_CHAR, 255);
  hud_rect(1280 - w, 0, w, 720, C_CHAR, 255);
  hud_tape(w - 24, 0, 24, 720, G.tape_phase);
  hud_tape(1280 - w, 0, 24, 720, G.tape_phase);
  hud_dirty(0, 0, 1280, 720);
}

/* ── the off-screen enemy chevron (9.2) ────────────────────────── */
static const bot_t *nearest_enemy(const bot_t *b) {
  const bot_t *best = NULL; float bd = 1e9f;
  for (int i = 0; i < M.nbots; i++) {
    const bot_t *o = &M.bots[i];
    if (o == b || !o->alive || !bot_is_player(o)) continue;
    if (M.mode == MODE_2V2 && o->team == b->team) continue;
    float dx = o->b.x - b->b.x, dz = o->b.z - b->b.z, d = dx * dx + dz * dz;
    if (d < bd) { bd = d; best = o; }
  }
  return best;
}

/* a 24 px filled triangle in the enemy's seat colour on a 2 px char shadow at the viewport edge */
static void chevron_at(const rect_t *vp, float ex, float ey, uint32_t c) {
  float cx = vp->x + vp->w * 0.5f, cy = vp->y + vp->h * 0.5f;
  float dx = ex - cx, dy = ey - cy, l = sqrtf(dx * dx + dy * dy);
  if (l < 1) return;
  dx /= l; dy /= l;
  /* the point on the viewport edge along (dx, dy), 20 px in */
  float hx = vp->w * 0.5f - 24, hy = vp->h * 0.5f - 24;
  float tx = fabsf(dx) > 1e-4f ? hx / fabsf(dx) : 1e9f, ty = fabsf(dy) > 1e-4f ? hy / fabsf(dy) : 1e9f;
  float t = tx < ty ? tx : ty;
  float px = cx + dx * t, py = cy + dy * t;
  float nx = -dy, ny = dx;
  float tipx = px + dx * 12, tipy = py + dy * 12, bx = px - dx * 12, by = py - dy * 12;
  hud_tri(tipx + 2, tipy + 2, bx + nx * 12 + 2, by + ny * 12 + 2, bx - nx * 12 + 2, by - ny * 12 + 2, C_CHAR, 255);
  hud_tri(tipx, tipy, bx + nx * 12, by + ny * 12, bx - nx * 12, by - ny * 12, c, 255);
  hud_dirty((int)px - 30, (int)py - 30, 60, 60);
}

void chevron(int viewport, const rect_t *vp, int seat) {
  const bot_t *b = seat_bot(seat);
  if (!b) return;
  const bot_t *e = nearest_enemy(b);
  if (!e) return;
  uint32_t c = SEAT_RGB[bot_seat_colour_index(e)];
  float ex, ey;
  if (cam_enemy_offscreen(seat, &ex, &ey)) { chevron_at(vp, ex, ey, c); return; }
  if (!project(viewport, e->b.x, e->b.y + 0.3f, e->b.z, &ex, &ey)) {
    /* behind the camera: point along the plane direction, mirrored */
    float dx = e->b.x - b->b.x, dz = e->b.z - b->b.z;
    float h = b->b.h, fx = sinf(h), fz = cosf(h);
    float side = dx * fz - dz * fx;
    chevron_at(vp, vp->x + vp->w * 0.5f + (side > 0 ? 400 : -400), vp->y + vp->h * 0.5f + 300, c);
    return;
  }
  if (ex < vp->x || ex > vp->x + vp->w || ey < vp->y || ey > vp->y + vp->h - 72) chevron_at(vp, ex, ey, c);
}

/* ── count-out (2.7, 10.4) ─────────────────────────────────────── */
static int count_number(const bot_t *b) { int n = 10 - b->countout / 60; return n < 0 ? 0 : n; }
static int can_self_right(const bot_t *b) { return WEAPONS[b->build.weapon].selfright != SR_NONE || b->build.drive == DR_MELTY; }

void countout(const rect_t *vp, const bot_t *b) {
  if (!b || b->countout <= 0 || !b->alive) return;
  char s[4];
  snprintf(s, sizeof s, "%d", count_number(b));
  int compact = vp->h <= 360;
  int cx = vp->x + vp->w / 2, cy = vp->y + vp->h / 2;
  /* F_TITLE at the viewport centre; in a quadrant F_H1 under the stinger slot so the two never collide */
  int font = compact ? F_H1 : F_TITLE;
  int fh = hud_font_h(font);
  int y = compact ? vp->y + 196 : cy - fh / 2 - 30;
  hud_text(font, cx + 4, y + 4, s, C_SHADOW, 200, AL_CENTER);
  hud_text(font, cx, y, s, C_RUST, 255, AL_CENTER);
  const char *prompt = b->inverted ? (can_self_right(b) ? S_RIGHT_YOURSELF : S_GET_MOVING) : S_GET_MOVING;
  hud_text_sh(F_MONO, cx, y + fh - (compact ? 4 : 10), prompt, C_BONE, 255, AL_CENTER);
  hud_dirty(cx - 200, y, 400, fh + 40);
}

/* ── the per viewport strip (10.4) ─────────────────────────────── */
static void ghost_update(const bot_t *b, float dt) {
  ghost_t *g = &GHOST[bot_slot(b)];
  if (!g->seen) { g->seen = 1; g->ghost = g->last = b->hp; g->hold = 0; }
  if (b->hp < g->last - 0.01f) { g->hold = 0.6f; if (g->ghost < g->last) g->ghost = g->last; }
  if (b->hp > g->ghost) g->ghost = b->hp;
  g->last = b->hp;
  if (g->hold > 0) {
    g->hold -= dt;
    if (g->hold < 0.35f) {
      float rate = (g->ghost - b->hp) / (g->hold > 0.02f ? g->hold : 0.02f);
      g->ghost -= rate * dt;
      if (g->ghost < b->hp) g->ghost = b->hp;
    }
  } else g->ghost = b->hp;
}

static uint32_t hp_colour(const bot_t *b, int seat_idx) {
  float f = b->hp_max > 0 ? b->hp / b->hp_max : 0;
  if (f > 0.5f) return SEAT_RGB[seat_idx];
  if (f > 0.25f) return C_HAZARD;
  return C_RUST;
}

/* the HP bar with the ghost segment and the RED TAG strip */
static void hp_bar(int x, int y, int w, int h, const bot_t *b, int seat_idx) {
  ghost_t *g = &GHOST[bot_slot(b)];
  float max = b->hp_max > 0 ? b->hp_max : 100;
  float f = clampf(b->hp / max, 0, 1), gf = clampf(g->ghost / max, 0, 1);
  uint32_t fill = hp_colour(b, seat_idx);
  int a = 255;
  if (f <= 0.25f && f > 0) a = 170 + (int)(85 * (0.5f + 0.5f * sinf(T * 6.2831853f * 2)));
  hud_rect(x, y, w, h, C_SLAB, 255);
  hud_rect(x + 2, y + 2, w - 4, h - 4, C_STEEL, 255);                 /* lost segments stay as steel */
  int fw = (int)((w - 4) * f), gw = (int)((w - 4) * gf);
  if (gw > fw) hud_rect(x + 2 + fw, y + 2, gw - fw, h - 4, C_BONE, 255);   /* the ghost drain */
  if (fw > 0) hud_rect(x + 2, y + 2, fw, h - 4, fill, a);
  /* segment ticks every 25 HP */
  for (int k = 1; k < 4; k++) hud_rect(x + 2 + (w - 4) * k / 4 - 1, y + 2, 1, h - 4, C_CHAR, 120);
  if (b->red_tag_t > 0) {
    int sw = h >= 18 ? 84 : 70;
    hud_rect(x + w - sw, y, sw, h, C_RUST, 255);
    hud_text(F_MONO_S, x + w - sw / 2, cy_text(F_MONO_S, y, h), S_RED_TAG, C_TAPE, 255, AL_CENTER);
  }
}

/* seat chip with the stun flicker, the red tag frame and the count badge */
static void strip_chip(int x, int y, int w, int h, const bot_t *b, int font) {
  int seat_idx = bot_seat_colour_index(b);
  int flicker = b->stun > 0 && (((int)(T * 8)) & 1);
  if (flicker) {
    hud_rect(x, y, w, h, C_STEEL, 255);
    char n[4]; snprintf(n, sizeof n, "%d", seat_idx + 1);
    hud_text(font, x + w / 2, cy_text(font, y, h), n, C_BONE, 255, AL_CENTER);
  } else hud_seat_chip(x, y, w, h, seat_idx, font, b->is_ai);
  if (b->red_tag_t > 0) hud_frame(x - 2, y - 2, w + 4, h + 4, 2, C_RUST, 255);
  if (b->countout > 0 && b->alive) {
    char n[4]; snprintf(n, sizeof n, "%d", count_number(b));
    int bw = 18, bh = 16;
    hud_rect(x + w - bw + 4, y - 4, bw, bh, C_RUST, 255);
    hud_text(F_MONO_S, x + w - bw / 2 + 4, y - 5, n, C_TAPE, 255, AL_CENTER);
  }
}

static int is_spinner(const bot_t *b) { return b->build.drive == DR_MELTY || b->build.weapon == WP_HORIZ || b->build.weapon == WP_DRUM || b->build.weapon == WP_SAW; }
static int weapon_glyph(int weapon, int melty) {
  if (melty) return GL_MELTY;
  switch (weapon) {
    case WP_WEDGE: return GL_WEDGE; case WP_SPIKE: return GL_SPIKE; case WP_LIFTER: return GL_LIFTER; case WP_SAW: return GL_SAW;
    case WP_FLAME: return GL_FLAME; case WP_HAMMER: return GL_HAMMER; case WP_FLIPPER: return GL_FLIPPER; case WP_DRUM: return GL_DRUM;
    case WP_HORIZ: return GL_SPINNER; case WP_CRUSHER: return GL_CRUSHER; default: return -1;
  }
}

/* the status word, with the spin words derived here when the sim has none to say */
static int status_of(const bot_t *b) {
  int sw = sim_status_word(b);
  if (sw == SW_NONE && (b->build.drive == DR_MELTY || b->build.weapon == WP_HORIZ || b->build.weapon == WP_DRUM || b->build.weapon == WP_SAW)) {
    if (b->spin >= 0.97f) sw = SW_FULL_SPIN; else if (b->spin > 0.05f) sw = SW_SPINNING_UP;
  }
  if (sw == SW_NONE && b->build.weapon == WP_FLAME && b->fuel <= 0 && b->hp_max > 0) sw = SW_EMPTY;
  return sw;
}

/* the ring colour carries the status in quadrants */
static uint32_t status_colour(int sw, uint32_t seat) {
  switch (sw) {
    case SW_FULL_SPIN: case SW_SPINNING_UP: return C_LIVE;
    case SW_STUNNED: case SW_INVERTED: case SW_BURNING: case SW_RED_TAG: case SW_LIFTED: case SW_CLAMPED: case SW_EMPTY: return C_RUST;
    default: return seat;
  }
}

/* the weapon gauge in a size x size box (10.4) */
static void gauge(int x, int y, int size, const bot_t *b, uint32_t seat_c) {
  int melty = b->build.drive == DR_MELTY;
  int wp = melty ? WP_NONE : b->build.weapon;
  const weapon_def_t *w = &WEAPONS[wp];
  int cx = x + size / 2, cy = y + size / 2, r = size / 2 - 2, t = size >= 44 ? 5 : 4;
  int gl = weapon_glyph(wp, melty);
  int sw = status_of(b);
  uint32_t ring_c = status_colour(sw, seat_c);
  if (melty || wp == WP_HORIZ || wp == WP_DRUM || wp == WP_SAW) {
    float spin = clampf(b->spin, 0, 1);
    hud_ring(cx, cy, r, t, 1, C_STEEL, 255);
    int a = 255;
    if (spin >= 0.97f) a = 170 + (int)(85 * (0.5f + 0.5f * sinf(T * 6.2831853f * 3)));
    if (spin > 0.01f) hud_ring(cx, cy, r, t, spin, spin >= 0.97f ? C_LIVE : ring_c, a);
    if (gl >= 0) hud_glyph(cx - 8, cy - 8, gl, 1, C_BONE, 255);
  } else if (wp == WP_FLIPPER || wp == WP_FLAME) {
    /* a vertical tank: FLIPPER refills with the cooldown, FLAME holds fuel */
    int tw = size >= 44 ? 12 : 10, th = size - 4;
    float f = wp == WP_FLAME ? clampf(b->fuel / WP_FLAME_FUEL, 0, 1) : (w->cooldown > 0 ? 1 - clampf((float)b->cooldown / w->cooldown, 0, 1) : 1);
    hud_rect(x + 2, y + 2, tw, th, C_STEEL, 255);
    int fh = (int)((th - 4) * f);
    uint32_t tc = f <= 0.05f ? C_RUST : f >= 0.99f ? C_LIVE : seat_c;
    hud_rect(x + 4, y + 2 + th - 2 - fh, tw - 4, fh, tc, 255);
    if (gl >= 0) hud_glyph(x + tw + 6 + (size - tw - 6 - 16) / 2, cy - 8, gl, 1, C_BONE, 255);
    if (b->cooldown > 0 && w->cooldown > 0) hud_ring(cx, cy, r, 2, (float)b->cooldown / w->cooldown, C_RUST, 200);
  } else if (wp == WP_HAMMER || wp == WP_SPIKE || wp == WP_CRUSHER) {
    /* the glyph with a short cooldown bar under it */
    float f = w->cooldown > 0 ? 1 - clampf((float)b->cooldown / w->cooldown, 0, 1) : 1;
    if (gl >= 0) hud_glyph(cx - 8, y + (size >= 44 ? 6 : 2), gl, 1, b->windup > 0 ? C_HAZARD : C_BONE, 255);
    int bw = size - 8, by = y + size - (size >= 44 ? 14 : 12);
    hud_rect(x + 4, by, bw, 6, C_STEEL, 255);
    hud_rect(x + 4, by, (int)(bw * f), 6, f >= 0.99f ? C_LIVE : C_RUST, 255);
    if (b->cooldown > 0 && w->cooldown > 0) hud_ring(cx, cy, r, 2, (float)b->cooldown / w->cooldown, C_RUST, 200);
  } else if (wp == WP_LIFTER) {
    float lift = clampf(b->lift, 0, 1);
    float ang = lift * 1.0f;
    float bx = x + 6, by = y + size - 8, len = size - 12;
    hud_tri(bx, by, bx + len, by, bx + len * cosf(ang), by - len * sinf(ang), seat_c, 220);
    hud_line(bx, by, bx + len * cosf(ang), by - len * sinf(ang), 3, C_BONE, 255);
    if (gl >= 0) hud_glyph(cx - 8, y + 2, gl, 1, C_BONE, 255);
  } else {
    /* WEDGE and NONE: nothing; a faint glyph for WEDGE so the slot is not blank */
    if (gl >= 0) hud_glyph(cx - 8, cy - 8, gl, 1, C_SMOKE, 255);
  }
  /* the secondary weapon as a small glyph at the corner */
  if (b->build.weapon2 != WP_NONE) {
    int g2 = weapon_glyph(b->build.weapon2, 0);
    if (g2 >= 0) hud_glyph(x + size - 12, y + size - 12, g2, 1, C_ASH, 255);
  }
}

static void speed_ticks(int x, int y, int w, int h, const bot_t *b, int n) {
  float v = sqrtf(b->b.vx * b->b.vx + b->b.vz * b->b.vz);
  float top = DRIVES[b->build.drive].top * (b->boost > 0 ? 1.5f : 1) * (b->red_tag_t > 0 ? 1.15f : 1);
  float f = top > 0 ? clampf(v / top, 0, 1) : 0;
  int lit = (int)(f * n + 0.5f);
  int pitch = w / n;
  for (int k = 0; k < n; k++) hud_rect(x + k * pitch, y, pitch - 4, h, k < lit ? C_ASH : C_STEEL, 255);
}

/* the mode chip at the strip's right end (44 x 56): the hill score or the team pool */
static void mode_chip(int x, int y, int w, int h, const bot_t *b) {
  if (M.mode != MODE_HILL && M.mode != MODE_2V2) return;
  hud_rect(x, y, w, h, C_SLAB, 255);
  hud_frame(x, y, w, h, 1, C_STEEL_HI, 255);
  if (M.mode == MODE_HILL) {
    hud_glyph(x + w / 2 - 8, y + 4, GL_CROWN, 1, C_LIVE, 255);
    char s[8]; snprintf(s, sizeof s, "%d", M.score[b->seat & 3]);
    hud_text(F_MONO_L, x + w - 4, y + 22, s, C_BONE, 255, AL_RIGHT);
  } else {
    float pool = 0, max = 0;
    for (int i = 0; i < M.nbots; i++) if (bot_is_player(&M.bots[i]) && M.bots[i].team == b->team) { pool += M.bots[i].hp > 0 ? M.bots[i].hp : 0; max += M.bots[i].hp_max; }
    hud_glyph(x + w / 2 - 8, y + 4, GL_TWO_BOTS, 1, SEAT_RGB[b->seat & 3], 255);
    char s[8]; snprintf(s, sizeof s, "%d", (int)pool);
    hud_text(F_MONO, x + w - 4, y + 22, s, C_BONE, 255, AL_RIGHT);
    hud_bar(x + 4, y + h - 12, w - 8, 8, max > 0 ? pool / max : 0, SEAT_RGB[b->seat & 3], C_STEEL);
  }
}

void hud_strip(const rect_t *vp, const bot_t *b, int compact) {
  if (!b) return;
  int h = compact ? 56 : 72, y = vp->y + vp->h - h;
  int seat_idx = bot_seat_colour_index(b);
  uint32_t sc = SEAT_RGB[seat_idx];
  hud_rect(vp->x, y, vp->w, h, C_CHAR, 220);
  hud_rect(vp->x, y, vp->w, 2, sc, 255);
  /* quadrant content stops 110 px short of the inner seam so the PIP never covers it */
  int ox = 0, cw = vp->w;
  if (compact && vp->w <= 640) { cw = vp->w - 110; if (vp->x >= 640) ox = 110; }
  int x = vp->x + ox;
  char name[24];
  fit_text(name, sizeof name, F_MONO_S, b->name, compact ? 280 : 340);
  if (!compact) {
    strip_chip(x + 8, y + 8, 36, 56, b, F_H2);
    hud_text(F_MONO_S, x + 52, y + 6, name, C_BONE, 255, AL_LEFT);
    hp_bar(x + 52, y + 26, 340, 18, b, seat_idx);
    char hp[8]; snprintf(hp, sizeof hp, "%d", (int)ceilf(b->hp > 0 ? b->hp : 0));
    hud_text(F_MONO_L, x + 460, cy_text(F_MONO_L, y + 24, 22), hp, C_BONE, 255, AL_RIGHT);
    gauge(x + 474, y + 8, 44, b, sc);
    speed_ticks(x + 530, y + 26, 60, 18, b, 6);
    if (DRIVES[b->build.drive].boost) {
      if (b->boost_cd > 0 && b->boost <= 0) {
        float f = 1 - clampf(b->boost_cd / (b->red_tag_t > 0 ? 120.f : 240.f), 0, 1);
        hud_rect(x + 530, y + 48, 60, 8, C_STEEL, 255);
        hud_rect(x + 530, y + 48, (int)(60 * f), 8, C_RUST, 255);
      } else hud_rect(x + 530, y + 48, 60, 8, b->boost > 0 ? C_BONE : C_LIVE, 255);
    }
    int sw = status_of(b);
    if (sw != SW_NONE) hud_text(F_MONO_S, x + 474, y + 54, STATUS_WORD[sw], status_colour(sw, C_LIVE), 255, AL_LEFT);
    if (vp->w >= 640) mode_chip(vp->x + vp->w - 50, y + 8, 44, 56, b);
  } else {
    strip_chip(x + 8, y + 8, 28, 40, b, F_H3);
    hud_text(F_MONO_S, x + 40, y + 4, name, C_BONE, 255, AL_LEFT);
    hp_bar(x + 40, y + 20, 280, 14, b, seat_idx);
    char hp[8]; snprintf(hp, sizeof hp, "%d", (int)ceilf(b->hp > 0 ? b->hp : 0));
    hud_text(F_MONO_L, x + 384, cy_text(F_MONO_L, y + 18, 18), hp, C_BONE, 255, AL_RIGHT);
    gauge(x + 400, y + 10, 36, b, sc);
    speed_ticks(x + 446, y + 20, 40, 12, b, 6);
    if (DRIVES[b->build.drive].boost) {
      /* a 3 pip row: ready pips teal, the cooldown fills them back in */
      float f = b->boost_cd > 0 && b->boost <= 0 ? 1 - clampf(b->boost_cd / (b->red_tag_t > 0 ? 120.f : 240.f), 0, 1) : 1;
      for (int k = 0; k < 3; k++) hud_rect(x + 446 + k * 13, y + 38, 9, 6, f >= (k + 1) / 3.f - 0.01f ? C_LIVE : C_STEEL, 255);
    }
    /* the hill score as a small chip at the content's right end */
    if (M.mode == MODE_HILL) {
      hud_glyph(x + cw - 40, y + 8, GL_CROWN, 1, C_LIVE, 255);
      char sc[8]; snprintf(sc, sizeof sc, "%d", M.score[b->seat & 3]);
      hud_text(F_MONO_L, x + cw - 4, y + 22, sc, C_BONE, 255, AL_RIGHT);
    }
  }
  hud_dirty(vp->x, y, vp->w, h);
}

/* the 320 px short strip for the practice bot (10.17): chip, name, HP */
void hud_strip_short(const rect_t *vp, const bot_t *b) {
  if (!b) return;
  int h = 72, y = vp->y + vp->h - h, x = vp->x;
  int seat_idx = bot_seat_colour_index(b);
  hud_rect(x, y, vp->w, h, C_CHAR, 220);
  hud_rect(x, y, vp->w, 2, SEAT_RGB[seat_idx], 255);
  strip_chip(x + 8, y + 8, 36, 56, b, F_H2);
  char name[24];
  fit_text(name, sizeof name, F_MONO_S, b->name, vp->w - 60);
  hud_text(F_MONO_S, x + 52, y + 6, name, C_BONE, 255, AL_LEFT);
  hp_bar(x + 52, y + 26, vp->w - 52 - 76, 18, b, seat_idx);
  char hp[8]; snprintf(hp, sizeof hp, "%d", (int)ceilf(b->hp > 0 ? b->hp : 0));
  hud_text(F_MONO_L, x + vp->w - 12, cy_text(F_MONO_L, y + 24, 22), hp, C_BONE, 255, AL_RIGHT);
  int sw = status_of(b);
  if (sw != SW_NONE) hud_text(F_MONO_S, x + 52, y + 54, STATUS_WORD[sw], status_colour(sw, C_LIVE), 255, AL_LEFT);
  hud_dirty(x, y, vp->w, h);
}

/* ── the minimap (9.5) ─────────────────────────────────────────── */
static int hazard_glyph(int type) {
  switch (type) {
    case HZ_PIT: return GL_PIT; case HZ_PULVERIZER: return GL_HAMMER; case HZ_KILLSAW: return GL_SAW; case HZ_FLAME: return GL_FLAME;
    case HZ_SCREW: return GL_SCREW; case HZ_RAMP: return GL_RAMP; case HZ_BUMPER: return GL_BUMPER; case HZ_DECK: return GL_DECK;
    case HZ_FLOORFLIP: return GL_FLIP_PANEL; case HZ_DROPZONE: return GL_DROP; case HZ_LASER: return GL_BEAM; case HZ_WIND: return GL_WIND;
    default: return -1;
  }
}

typedef struct { float ox, oy, s; } mapxf_t;   /* world (x, z) to HUD: sx = ox + x * s, sy = oy + z * s */
static mapxf_t map_fit(const rect_t *r, const arena_def_t *a, int pad) {
  float sx = (r->w - 2 * pad) / a->w, sz = (r->h - 2 * pad) / a->d;
  mapxf_t m; m.s = sx < sz ? sx : sz;
  m.ox = r->x + r->w * 0.5f; m.oy = r->y + r->h * 0.5f;
  return m;
}

/* the arena footprint: walls as ash lines, open edges as tape */
static void map_outline(const mapxf_t *m, const arena_def_t *a, uint32_t wall_c, int thick) {
  float hw = a->w * 0.5f * m->s, hd = a->d * 0.5f * m->s;
  float x0 = m->ox - hw, x1 = m->ox + hw, y0 = m->oy - hd, y1 = m->oy + hd;
  struct { int open; float ax, ay, bx, by; } e[4] = {
    { a->open_mask & OPEN_PX, x1, y0, x1, y1 }, { a->open_mask & OPEN_NX, x0, y0, x0, y1 },
    { a->open_mask & OPEN_PZ, x0, y1, x1, y1 }, { a->open_mask & OPEN_NZ, x0, y0, x1, y0 } };
  for (int i = 0; i < 4; i++) {
    if (e[i].open) {
      int vert = e[i].ax == e[i].bx;
      if (vert) hud_tape((int)e[i].ax - 3, (int)e[i].ay, 6, (int)(e[i].by - e[i].ay), 0);
      else hud_tape((int)e[i].ax, (int)e[i].ay - 3, (int)(e[i].bx - e[i].ax), 6, 0);
    } else hud_line(e[i].ax, e[i].ay, e[i].bx, e[i].by, (float)thick, wall_c, 255);
  }
}

/* hazards, props with hp, the hill, bots, OPS and the count digits */
static void map_contents(const mapxf_t *m, const arena_def_t *a, const match_t *mt, int big, int seat_view, const rect_t *clip) {
  int gs = big ? 2 : 1;
  hud_clip(clip->x, clip->y, clip->w, clip->h);
  /* the pit as a char square with a tape frame, every other hazard as a glyph */
  for (int k = 0; k < a->nhaz && k < MAX_HAZ; k++) {
    const hazard_def_t *h = &a->haz[k];
    const hazard_state_t *hs = &mt->haz[k];
    int sx = (int)(m->ox + h->x * m->s), sy = (int)(m->oy + h->z * m->s);
    if (h->type == HZ_PIT) {
      int w = (int)(h->w * m->s), d = (int)(h->d * m->s);
      if (w < 8) w = 8; if (d < 8) d = 8;
      hud_rect(sx - w / 2, sy - d / 2, w, d, C_CHAR, 255);
      if (hs->active) hud_rect(sx - w / 2 + 2, sy - d / 2 + 2, w - 4, d - 4, C_HAZARD, 60 + (int)(60 * (0.5f + 0.5f * sinf(T * 12.56f))));
      hud_tape(sx - w / 2 - 2, sy - d / 2 - 2, w + 4, 3, 0); hud_tape(sx - w / 2 - 2, sy + d / 2 - 1, w + 4, 3, 0);
      hud_tape(sx - w / 2 - 2, sy - d / 2, 3, d, 0); hud_tape(sx + w / 2 - 1, sy - d / 2, 3, d, 0);
      continue;
    }
    if (h->type == HZ_HILL) continue;
    int gl = hazard_glyph(h->type);
    if (gl < 0) continue;
    uint32_t c = hs->active ? C_HAZARD : C_STEEL_HI;
    int al = 255;
    if (hs->tell) { c = C_HAZARD; al = 120 + (int)(135 * (0.5f + 0.5f * sinf(T * 6.2831853f * J_HAZARD_TELL_HZ))); }
    if (hs->broken) c = C_SMOKE;
    if (seat_view >= 0 && mt->hazard_seat[seat_view] == k) c = C_LIVE;
    hud_glyph(sx - 8 * gs, sy - 8 * gs, gl, gs, c, al);
    if (seat_view >= 0 && mt->hazard_seat[seat_view] == k) {
      hud_tri(sx, sy - 10 * gs - 2, sx - 6 * gs, sy - 18 * gs, sx + 6 * gs, sy - 18 * gs, SEAT_RGB[seat_view & 3], 255);
      if (hs->cooldown > 0) {
        float cd = h->type == HZ_PULVERIZER ? 72.f : h->type == HZ_FLAME ? 360.f : 180.f;
        hud_ring(sx, sy, 12 * gs, 2 * gs, clampf(hs->cooldown / cd, 0, 1), C_RUST, 255);
      }
    }
    /* a hazard seat's pick in another seat's colour (arrow) */
    for (int s = 0; s < 4; s++) if (s != seat_view && mt->hazard_seat[s] == k) {
      const bot_t *sb = seat_bot(s);
      if (sb && !sb->alive) hud_tri(sx, sy - 10 * gs - 2, sx - 5 * gs, sy - 16 * gs, sx + 5 * gs, sy - 16 * gs, SEAT_RGB[s], 255);
    }
  }
  /* the hill */
  if (mt->mode == MODE_HILL && mt->hill >= 0 && mt->hill < 4) {
    int sx = (int)(m->ox + a->hill[mt->hill][0] * m->s), sy = (int)(m->oy + a->hill[mt->hill][1] * m->s);
    int w = (int)(3 * m->s);
    hud_rect(sx - w / 2, sy - w / 2, w, w, C_LIVE, 120);
    hud_frame(sx - w / 2, sy - w / 2, w, w, 1, C_LIVE, 255);
  }
  /* bots */
  for (int i = 0; i < mt->nbots && i < MAX_BOTS; i++) {
    const bot_t *b = &mt->bots[i];
    if (!bot_in_use(b)) continue;
    int sx = (int)(m->ox + b->b.x * m->s), sy = (int)(m->oy + b->b.z * m->s);
    if (b->is_ops) {
      int w = 5 * gs;
      hud_rect(sx - w, sy - w, 2 * w, 2 * w, C_CHAR, 255);
      hud_frame(sx - w, sy - w, 2 * w, 2 * w, 1, C_STEEL_HI, 255);
      hud_rect(sx - 1 * gs, sy - 1 * gs, 2 * gs, 2 * gs, C_HAZARD, 255);
      continue;
    }
    if (!b->alive) continue;
    uint32_t c = SEAT_RGB[b->seat & 3];
    int w = 4 * gs;
    hud_rect(sx - w, sy - w, 2 * w, 2 * w, c, 255);
    float hx = sinf(b->b.h), hz = cosf(b->b.h);
    hud_line(sx, sy, sx + hx * (w + 3 * gs), sy + hz * (w + 3 * gs), (float)(gs + 1), c, 255);
    if (big) {
      char nm[24]; fit_text(nm, sizeof nm, F_MONO_S, b->name, 150);
      hud_text(F_MONO_S, sx + w + 6, sy - 8, nm, C_BONE, 255, AL_LEFT);
    } else if (seat_view >= 0) {
      char n[4]; snprintf(n, sizeof n, "%d", (b->seat & 3) + 1);
      hud_text(F_MONO_S, sx + w + 4, sy - 8, n, c, 255, AL_LEFT);
    }
    if (b->countout > 0) {
      char n[4]; snprintf(n, sizeof n, "%d", count_number(b));
      hud_text(F_MONO_S, sx - w - 4, sy - 8, n, C_RUST, 255, AL_RIGHT);
    }
  }
  hud_clip_clear();
}

void minimap(const rect_t *r, const match_t *mt, const arena_def_t *a, int big) {
  hud_rect(r->x, r->y, r->w, r->h, C_CHAR, 220);
  hud_frame(r->x, r->y, r->w, r->h, 2, C_BONE, 255);
  mapxf_t m = map_fit(r, a, big ? 14 : 10);
  map_outline(&m, a, C_ASH, 1);
  map_contents(&m, a, mt, big, -1, r);
  hud_dirty(r->x, r->y, r->w, r->h);
}

/* ── the hazard seat viewport (10.19) ──────────────────────────── */
static int seat_hazard_kind(const arena_def_t *a, int idx) {
  if (idx < 0 || idx >= a->nhaz) return HZ_PULVERIZER;
  return a->haz[idx].type;
}

void hazard_seat_view(const rect_t *vp, int seat) {
  const arena_def_t *a = cur_arena();
  /* a half is 640 wide too, so the view keeps its size and sits vertically centred; the big map glyphs are 2x */
  int big = vp->h > 360;
  int k = 1;
  int my = big ? vp->y + (vp->h - 236) / 2 - 40 : vp->y + 56;
  int hy = big ? my + 236 + 40 : vp->y + 316;
  hud_rect(vp->x, vp->y, vp->w, vp->h, C_CHAR, 255);
  int chosen = seat < 4 && M.hazard_seat[seat] != 255 ? M.hazard_seat[seat] : -1;
  int kind = seat_hazard_kind(a, chosen >= 0 ? chosen : arena_seat_hazard(a, 0));
  /* the badge: seat chip and OUT */
  hud_seat_chip(vp->x + 8 * k, vp->y + 8 * k, 24 * k, 24 * k, seat & 3, F_MONO_S, 0);
  hud_text(F_MONO, vp->x + 38 * k, cy_text(F_MONO, vp->y + 8 * k, 24 * k), S_OUT, C_BONE, 255, AL_LEFT);
  const char *have = kind == HZ_FLAME ? S_YOU_HAVE_FLAME : kind == HZ_FLOORFLIP ? S_YOU_HAVE_FLIPPER : S_YOU_HAVE_HAMMERS;
  hud_text(big ? F_MONO : F_MONO_S, vp->x + 92 * k, cy_text(big ? F_MONO : F_MONO_S, vp->y + 8 * k, 24 * k), have, C_ASH, 255, AL_LEFT);
  /* the top down map */
  rect_t mr = { vp->x + 110 * k, my, 420 * k, 236 * k };
  hud_frame(mr.x, mr.y, mr.w, mr.h, 1, C_STEEL, 255);
  mapxf_t m = map_fit(&mr, a, 12 * k);
  map_outline(&m, a, C_STEEL_HI, 2);
  map_contents(&m, a, &M, big, seat, &mr);
  /* the hints */
  const char *pick = kind == HZ_FLAME ? S_SEAT_PICK_FLAME : kind == HZ_FLOORFLIP ? S_SEAT_PICK_FLIPPER : S_SEAT_PICK_HAMMER;
  const char *fire = kind == HZ_PULVERIZER ? S_SEAT_SWING : S_SEAT_FIRE;
  int hx = vp->x + 16 * k;
  int hf = big ? F_MONO : F_MONO_S;
  hud_chevron(hx + 6, hy + 8 * k, -1, 0, 10, 2, C_HAZARD, 255);
  int tw = hud_text(hf, hx + 16, hy, pick, C_ASH, 255, AL_LEFT);
  int kx = hx + 16 + tw + 16;
  kx += hud_keycap(kx, hy - 2, "A", C_BONE, C_TAPE) + 8;
  hud_text(hf, kx, hy, fire, C_ASH, 255, AL_LEFT);
  if (chosen >= 0 && M.haz[chosen].cooldown > 0) {
    char s[24]; int cs = M.haz[chosen].cooldown * 10 / 60;
    snprintf(s, sizeof s, S_SEAT_COOLDOWN, cs / 10, cs % 10);
    hud_text(hf, hx, hy + 18 * k, s, C_RUST, 255, AL_LEFT);
  }
  hud_dirty(vp->x, vp->y, vp->w, vp->h);
}

/* ── the OPS speech panel (section 7) ──────────────────────────── */
void speech_panel(int viewport, float x, float y, float z, const char *line) {
  (void)viewport;
  snprintf(SPEECH.line, sizeof SPEECH.line, "%s", line ? line : "");
  SPEECH.x = x; SPEECH.y = y; SPEECH.z = z; SPEECH.t = 2.0f;
}

static void speech_draw(int viewport, const rect_t *vp) {
  if (SPEECH.t <= 0 || !SPEECH.line[0]) return;
  float sx, sy;
  if (!project(viewport, SPEECH.x, SPEECH.y + 1.2f, SPEECH.z, &sx, &sy)) return;
  int w = hud_text_w(F_MONO, SPEECH.line) + 24, h = 34;
  int x = (int)sx - w / 2, y = (int)sy - h - 10;
  if (x < vp->x + 8) x = vp->x + 8;
  if (x + w > vp->x + vp->w - 8) x = vp->x + vp->w - 8 - w;
  if (y < vp->y + 30) y = vp->y + 30;
  hud_clip(vp->x, vp->y, vp->w, vp->h);
  hud_panel(x, y, w, h, C_BONE, C_STEEL);
  hud_text(F_MONO, x + w / 2, cy_text(F_MONO, y, h), SPEECH.line, C_TAPE, 255, AL_CENTER);
  hud_tri((float)sx - 8, (float)y + h, (float)sx + 8, (float)y + h, (float)sx, (float)y + h + 8, C_BONE, 255);
  hud_clip_clear();
  hud_dirty(x, y, w + 6, h + 14);
}

/* ── the NEXT MATCH chip (10.2) ────────────────────────────────── */
void next_match_chip(int pad, float t) {
  if (t <= 0) return;
  int seat = 0;
  for (int i = 0; i < 4; i++) if (G.seats[i].kind == SEAT_EMPTY) { seat = i; break; }
  (void)pad;
  int a = t < 0.3f ? (int)(255 * t / 0.3f) : 255;
  int w = hud_text_w(F_MONO_S, S_NEXT_MATCH) + 52, h = 30, x = 640 - w / 2, y = 600;
  hud_set_alpha(a);
  hud_rect(x + 4, y + 4, w, h, C_SHADOW, 255);
  hud_rect(x, y, w, h, C_CHAR, 255);
  hud_seat_chip(x + 6, y + 5, 20, 20, seat, F_MONO_S, 0);
  hud_text(F_MONO_S, x + 36, cy_text(F_MONO_S, y, h), S_NEXT_MATCH, C_BONE, 255, AL_LEFT);
  hud_set_alpha(255);
  hud_dirty(x, y, w + 4, h + 4);
}

/* ── pause (10.26) ─────────────────────────────────────────────── */
void pause_overlay(void) {
  uint32_t c = SEAT_RGB[G.pauser & 3];
  hud_frame(0, 0, 1280, 720, 12, c, 255);
  hud_rect(12, 12, 1256, 696, C_CHAR, 120);
  hud_text_sh(F_H1, 640, 240, S_PAUSED, C_BONE, 255, AL_CENTER);
  hud_panel(440, 320, 400, 180, C_SLAB, C_STEEL_HI);
  for (int i = 0; i < 4; i++) {
    int y = 320 + 8 + i * 41;
    menu_row(448, y, 384, PAUSE_ROWS[i], NULL, G.pause_row == i, 1, 0, 0, 0);
  }
  /* QUIT TO LOBBY needs a 1 s hold on A: the ring at the row's right fills with G.reset_hold (game.c) */
  if (G.pause_row == 3) {
    float held = clampf(G.reset_hold, 0, 1);
    hud_ring(448 + 384 - 28, 320 + 8 + 3 * 41 + 20, 12, 3, 1, C_TAPE, 120);
    if (held > 0) hud_ring(448 + 384 - 28, 320 + 8 + 3 * 41 + 20, 12, 3, held, C_TAPE, 255);
    hud_text(F_MONO_S, 448 + 384 - 48, 320 + 8 + 3 * 41 + 12, S_HOLD_A_QUIT, C_TAPE, 255, AL_RIGHT);
  }
  hud_dirty(0, 0, 1280, 720);
}

/* ── the KO card (10.18) ───────────────────────────────────────── */
static void vignette_tint(uint32_t c, int a) {
  /* 36 thin edge bands with a quadratic falloff: a vignette without a per pixel pass */
  const int n = 36, t = 4;
  for (int k = 0; k < n; k++) {
    float f = 1.f - (float)k / n;
    int al = (int)(a * f * f);
    if (al <= 0) break;
    int d = 24 + k * t;
    hud_rect(0, d, 1280, t, c, al); hud_rect(0, 720 - d - t, 1280, t, c, al);
    hud_rect(k * t, d + t, t, 720 - 2 * d - 2 * t, c, al); hud_rect(1280 - k * t - t, d + t, t, 720 - 2 * d - 2 * t, c, al);
  }
}

static void ko_strips(int alpha) {
  /* every bot's strip along the bottom in seat order: 640 wide for two, 320 short strips for more */
  int n = 0; const bot_t *bs[4];
  for (int i = 0; i < M.nbots && n < 4; i++) if (bot_is_player(&M.bots[i])) bs[n++] = &M.bots[i];
  if (!n) return;
  /* above the bottom tape band */
  hud_set_alpha(alpha);
  if (n <= 2) for (int i = 0; i < n; i++) { rect_t r = { 640 * i, 0, 640, 696 }; hud_strip(&r, bs[i], 0); }
  else for (int i = 0; i < n; i++) { rect_t r = { 320 * i, 0, 320, 696 }; hud_strip_short(&r, bs[i]); }
  hud_set_alpha(255);
}

void ko_card(float t) {
  const bot_t *victim = G.ko_victim >= 0 && G.ko_victim < M.nbots ? &M.bots[G.ko_victim] : NULL;
  const bot_t *victor = G.ko_victor >= 0 && G.ko_victor < M.nbots ? &M.bots[G.ko_victor] : NULL;
  uint32_t vc = victor ? SEAT_RGB[victor->seat & 3] : C_BONE;
  /* the tape bands slide in over 0.4 s and scroll at 60 px/s */
  float slide = hud_ease_out(clampf(t / 0.4f, 0, 1));
  int bh = (int)(24 * slide);
  if (bh > 0) { hud_tape(0, 0, 1280, bh, T * 60); hud_tape(0, 720 - bh, 1280, bh, T * 60); }
  vignette_tint(vc, J_KO_VIGNETTE_A);
  ko_strips(120);
  if (t >= 0.3f) {
    float u = t - 0.3f;
    int outline = u < 0.2f;
    int a = u < 0.1f ? (int)(255 * u / 0.1f) : 255;
    int fh = hud_font_h(F_TITLE);
    int y = 260 - fh / 2;
    hud_text(F_TITLE, 646, y + 6, S_KO_BIG, vc, a, AL_CENTER);
    if (outline) for (int dx = -4; dx <= 4; dx += 4) for (int dy = -4; dy <= 4; dy += 4) if (dx || dy) hud_text(F_TITLE, 640 + dx, y + dy, S_KO_BIG, C_BONE, a / 2, AL_CENTER);
    hud_text(F_TITLE, 640, y, S_KO_BIG, C_BONE, a, AL_CENTER);
    if (u > 0.15f) {
      const char *cause = KO_CAUSE[M.win_how < 6 ? M.win_how : KO_HP];
      if (victim) hud_text_sh(F_H3, 640, 400, victim->name, C_BONE, 255, AL_CENTER);
      hud_text_sh(F_H3, 640, 432, cause, C_RUST, 255, AL_CENTER);
    }
  }
  hud_dirty(0, 0, 1280, 720);
}

/* ── the judges card (10.20) ───────────────────────────────────── */
static const char *judge_line(int rule) {
  switch (rule) { case 1: return JUDGE_TIE_COST; case 2: return JUDGE_TIE_FLIP; case 3: return JUDGE_DRAW; default: return NULL; }
}

void judges_card(float t) {
  hud_text_sh(F_TITLE, 640, 48, S_TIME_BIG, C_BONE, 255, AL_CENTER);
  int n = 0; const bot_t *bs[4];
  for (int i = 0; i < M.nbots && n < 4; i++) if (bot_is_player(&M.bots[i])) bs[n++] = &M.bots[i];
  if (!n) return;
  float slide = hud_ease_out(clampf(t / 0.3f, 0, 1));
  int x = 816 + (int)(500 * (1 - slide)), y = 144, w = 400, h = 480;
  hud_panel(x, y, w, h, C_SLAB, C_STEEL_HI);
  hud_eyebrow(x + 20, y + 16, S_JUDGES, C_ASH, AL_LEFT);
  static const int weights[3] = { 7, 4, 4 };
  int pitch = n >= 4 ? 22 : n == 3 ? 24 : 26, bh = pitch - 8;
  int cy = y + 44;
  int totals_y;
  for (int c = 0; c < 3; c++) {
    char ws[4]; snprintf(ws, sizeof ws, "%d", weights[c]);
    hud_text(F_H3, x + 20, cy, JUDGE_ROW[c], C_BONE, 255, AL_LEFT);
    hud_text(F_H3, x + w - 20, cy, ws, C_BONE, 255, AL_RIGHT);
    /* bars fill in sequence over 0.6 s each: damage, aggression, control */
    float f = clampf((t - 0.3f - c * 0.6f) / 0.6f, 0, 1);
    f = hud_ease_out(f);
    for (int i = 0; i < n; i++) {
      int by = cy + 30 + i * pitch;
      const bot_t *b = bs[i];
      float v = G.judge_bars[c][bot_slot(b)];
      float frac = clampf(v / weights[c], 0, 1) * f;
      hud_seat_chip(x + 20, by, bh, bh, b->seat & 3, F_MONO_S, b->is_ai);
      hud_bar(x + 20 + bh + 8, by, w - 40 - bh - 8 - 52, bh, frac, SEAT_RGB[b->seat & 3], C_STEEL);
      char vs[8]; snprintf(vs, sizeof vs, "%.1f", v * f);
      hud_text(F_MONO, x + w - 20, cy_text(F_MONO, by, bh), vs, C_BONE, 255, AL_RIGHT);
    }
    cy += 30 + n * pitch + 10;
  }
  totals_y = cy;
  int show_totals = t > 0.3f + 1.8f;
  if (show_totals) {
    /* totals in F_H2 with a chip each, spread across the width */
    int cw = (w - 40) / n;
    for (int i = 0; i < n; i++) {
      const bot_t *b = bs[i];
      float tot = 0; for (int c = 0; c < 3; c++) tot += G.judge_bars[c][bot_slot(b)];
      int tx = x + 20 + i * cw;
      hud_seat_chip(tx, totals_y + 6, 22, 22, b->seat & 3, F_MONO_S, b->is_ai);
      char ts[8]; snprintf(ts, sizeof ts, "%.1f", tot);
      int tf = n >= 4 ? F_H3 : F_H2;
      hud_text(tf, tx + 28, cy_text(tf, totals_y, 34), ts, C_BONE, 255, AL_LEFT);
    }
  }
  int win_y = y + h - 76;
  if (t > 0.3f + 2.1f) {
    const char *line = judge_line(G.judges_rule);
    int winner = G.judge_winner;
    if (G.judges_rule >= 1 && G.judges_rule <= 2) {
      int cw = hud_text_w(F_MONO_S, JUDGE_UNDERDOG) + 20;
      hud_rect(x + 20, win_y, cw, 24, C_LIVE, 255);
      hud_text(F_MONO_S, x + 20 + cw / 2, cy_text(F_MONO_S, win_y, 24), JUDGE_UNDERDOG, C_TAPE, 255, AL_CENTER);
      if (winner >= 0 && winner < M.nbots) {
        char ws[48]; fit_text(ws, sizeof ws, F_MONO_S, M.bots[winner].name, w - 60 - cw);
        hud_text(F_MONO_S, x + w - 20, cy_text(F_MONO_S, win_y, 24), ws, C_BONE, 255, AL_RIGHT);
      }
      if (line) hud_text_wrap(F_MONO_S, x + 20, win_y + 32, w - 40, line, C_ASH, 255, 18);
    } else if (G.judges_rule == 3 || winner < 0) {
      if (line) hud_text_wrap(F_MONO_S, x + 20, win_y + 8, w - 40, line, C_HAZARD, 255, 18);
    } else if (winner < M.nbots) {
      char ws[48], nm[24], pre[24];
      snprintf(pre, sizeof pre, S_WINNER_IS, "");
      fit_text(nm, sizeof nm, F_H3, M.bots[winner].name, w - 40 - hud_text_w(F_H3, pre));
      snprintf(ws, sizeof ws, S_WINNER_IS, nm);
      int tw = hud_text_w(F_H3, ws) + 24;
      if (tw > w - 40) tw = w - 40;
      hud_rect(x + 20, win_y, tw, 36, C_LIVE, 255);
      hud_text(F_H3, x + 20 + 12, cy_text(F_H3, win_y, 36), ws, C_TAPE, 255, AL_LEFT);
    }
  }
  hud_dirty(x, y, w + 6, h + 6);
  hud_dirty(400, 40, 480, 140);
}

/* ── results and the rematch menu (10.21) ──────────────────────── */
static const char *place_word(const bot_t *b) {
  if (b->place <= 1) return PLACE_WORD[0];          /* the sim ranks from 1 */
  switch (b->ko_how) {
    case KO_HP: return PLACE_WORD[1]; case KO_FLIP: return PLACE_WORD[2]; case KO_PIT: return PLACE_WORD[3];
    case KO_ROOF: return PLACE_WORD[4]; case KO_COUNT: return PLACE_WORD[5]; case KO_DECK: return PLACE_WORD[6];
    default: return PLACE_WORD[7];
  }
}

/* the bots ranked by placement (bot_t.place, 0 is the winner); ties keep slot order */
static int ranked(const bot_t *out[4]) {
  int n = 0;
  for (int i = 0; i < M.nbots && n < 4; i++) if (bot_is_player(&M.bots[i])) out[n++] = &M.bots[i];
  for (int i = 1; i < n; i++) { const bot_t *k = out[i]; int j = i - 1; while (j >= 0 && out[j]->place > k->place) { out[j + 1] = out[j]; j--; } out[j + 1] = k; }
  return n;
}

static int names_collide(const bot_t *const *bs, int n) {
  for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (!strcmp(bs[i]->name, bs[j]->name)) return 1;
  return 0;
}

static int evening_wins(int seat) { return G.seats[seat & 3].wins; }

/* a char backdrop everywhere but the turntable viewport: the 3D pass clears only that
 * viewport, so nothing stale shows through and every line of the screen sits on char */
static void results_backdrop(const rect_t *r) {
  if (M.mode == MODE_TOURNAMENT) { hud_rect(0, 24, 1280, 672, C_CHAR, 255); return; }
  hud_rect(0, 24, 1280, r->y - 24, C_CHAR, 255);
  hud_rect(0, r->y + r->h, 1280, 696 - r->y - r->h, C_CHAR, 255);
  hud_rect(0, r->y, r->x, r->h, C_CHAR, 255);
  hud_rect(r->x + r->w, r->y, 1280 - r->x - r->w, r->h, C_CHAR, 255);
}

static void results_bracket_or_turntable(const rect_t *r) {
  if (M.mode == MODE_TOURNAMENT) bracket_card(r);
  else {
    /* the turntable viewport shows through; a steel frame marks it */
    hud_frame(r->x, r->y, r->w, r->h, 2, C_STEEL_HI, 255);
    hud_rect(r->x, r->y + r->h - 4, r->w, 4, C_STEEL_HI, 255);
  }
}

void results_screen(float t) {
  const bot_t *bs[4];
  int n = ranked(bs);
  hud_tape(0, 0, 1280, 24, 0);          /* static here, so the results hold uploads nothing */
  hud_tape(0, 696, 1280, 24, 0);
  rect_t tt = { 64, 180, 420, 300 };    /* the turntable viewport (or the bracket card) */
  results_backdrop(&tt);
  if (!n) return;
  const bot_t *winner = bs[0];
  int judged = M.win_how == WIN_JUDGES;
  /* eyebrow: arena, mode, time left; the evening tally at the right */
  char left[16], eb[96], tally[48];
  int secs = (int)(M.clock_s > 0 ? M.clock_s : 0);
  snprintf(left, sizeof left, S_LEFT, secs / 60, secs % 60);
  snprintf(eb, sizeof eb, S_RESULTS_EYEBROW, ARENA_NAME[M.arena % 6], MODE_NAME[M.mode % MODE_COUNT], left);
  hud_text_sh(F_MONO_S, 64, 40, eb, C_ASH, 255, AL_LEFT);
  snprintf(tally, sizeof tally, S_EVENING, evening_wins(0), evening_wins(1), evening_wins(2), evening_wins(3));
  hud_text_sh(F_MONO_S, 1216, 40, tally, C_ASH, 255, AL_RIGHT);
  /* the winner line in the seat colour */
  char wl[64];
  if (M.win_how == WIN_JUDGES && G.judges_rule == 3) snprintf(wl, sizeof wl, "%s", JUDGE_DRAW);
  else if (names_collide(bs, n)) snprintf(wl, sizeof wl, S_SEAT_WINS, (winner->seat & 3) + 1, winner->name);
  else snprintf(wl, sizeof wl, S_X_WINS, winner->name);
  int wf = fit_font(F_H1, wl, 1152);
  hud_text_sh(wf, 64, 64, wl, SEAT_RGB[winner->seat & 3], 255, AL_LEFT);
  /* the cause line from KO_CAUSE_LINES */
  int ci = judged ? (G.judges_rule >= 1 ? 6 : 7) : (M.win_how < 6 ? M.win_how : 0);
  hud_text_sh(fit_font(F_H3, KO_CAUSE_LINES[ci], 1152), 64, 136, KO_CAUSE_LINES[ci], C_HAZARD, 255, AL_LEFT);
  /* the turntable or the bracket */
  results_bracket_or_turntable(&tt);
  if (!results_confetti_done && t > 0.3f) { fx2_burst(FX2_CONFETTI, 274, 200, SEAT_RGB[winner->seat & 3]); results_confetti_done = 1; }
  /* the rows panel */
  int px = 520, py = 180, pw = 696, ph = 330;
  hud_panel(px, py, pw, ph, C_SLAB, C_STEEL_HI);
  hud_clip(px, py, pw, ph);
  for (int i = 0; i < n; i++) {
    const bot_t *b = bs[i];
    float f = hud_ease_out(clampf((t - 0.2f * i) / 0.3f, 0, 1));
    int rx = px + 12 + (int)(80 * (1 - f)), ry = py + 13 + i * 76, rw = pw - 24, rh = 72;
    int a = (int)(255 * f);
    hud_set_alpha(a);
    hud_rect(rx, ry, rw, rh, i == 0 ? C_SLAB_HI : C_GRIME, 255);
    hud_rect(rx, ry, 4, rh, i == 0 ? SEAT_RGB[b->seat & 3] : C_STEEL, 255);
    char rank[4]; snprintf(rank, sizeof rank, "%d", i + 1);
    hud_text(F_H2, rx + 16, ry + 2, rank, C_SMOKE, 255, AL_LEFT);
    hud_seat_chip(rx + 52, ry + 8, 28, 28, b->seat & 3, F_H3, b->is_ai);
    char nm[24]; fit_text(nm, sizeof nm, F_H3, b->name, 400);
    hud_text(F_H3, rx + 92, ry + 6, nm, C_BONE, 255, AL_LEFT);
    hud_text(F_MONO_S, rx + rw - 16, ry + 12, place_word(b), C_ASH, 255, AL_RIGHT);
    /* the four stats, two spaces apart, from their content.h formats */
    char st[96], s1[16], s2[16], s3[16], s4[16]; int air = (int)(b->airtime * 10 + 0.5f);
    snprintf(s1, sizeof s1, S_DEALT, (int)b->dealt); snprintf(s2, sizeof s2, S_BIGGEST, (int)b->biggest);
    snprintf(s3, sizeof s3, S_AIR, air / 10, air % 10); snprintf(s4, sizeof s4, S_FLIPS, b->flips);
    snprintf(st, sizeof st, "%s  %s  %s  %s", s1, s2, s3, s4);
    hud_text(F_MONO_S, rx + 92, ry + 38, st, C_BONE, 255, AL_LEFT);
    int aw = G.award[bot_slot(b)];
    if (aw >= 0 && aw < AW_COUNT) hud_text(F_MONO_S, rx + 92, ry + 54, AWARDS[aw].name, C_HAZARD, 255, AL_LEFT);
    hud_set_alpha(255);
  }
  hud_clip_clear();
  /* judges bars on a timeout: the winner's three values with the chip and the tie line */
  if (judged) {
    int jx = 520, jy = 540;
    static const int weights[3] = { 7, 4, 4 };
    int slot = bot_slot(winner);
    for (int c = 0; c < 3; c++) {
      int cx = jx + c * 232;
      hud_text(F_MONO_S, cx, jy, JUDGE_ROW[c], C_ASH, 255, AL_LEFT);
      hud_bar(cx, jy + 20, 150, 10, clampf(G.judge_bars[c][slot] / weights[c], 0, 1), SEAT_RGB[winner->seat & 3], C_STEEL);
      char v[8]; snprintf(v, sizeof v, "%.1f", G.judge_bars[c][slot]);
      hud_text(F_MONO_S, cx + 160, jy + 16, v, C_BONE, 255, AL_LEFT);
    }
    const char *line = judge_line(G.judges_rule);
    if (G.judges_rule >= 1 && G.judges_rule <= 2) {
      int cw = hud_text_w(F_MONO_S, JUDGE_UNDERDOG) + 20;
      hud_rect(jx, jy + 44, cw, 22, C_LIVE, 255);
      hud_text(F_MONO_S, jx + cw / 2, cy_text(F_MONO_S, jy + 44, 22), JUDGE_UNDERDOG, C_TAPE, 255, AL_CENTER);
      if (line) hud_text(F_MONO_S, jx + cw + 12, cy_text(F_MONO_S, jy + 44, 22), line, C_ASH, 255, AL_LEFT);
    } else if (line) hud_text(F_MONO_S, jx, jy + 48, line, C_HAZARD, 255, AL_LEFT);
  }
  /* the key hints and the wins tally */
  int kx = 520;
  kx += hud_keycap(kx, 660, "A", C_BONE, C_TAPE) + 6;
  kx += hud_text_sh(F_MONO_S, kx, 664, S_KEY_VOTE, C_BONE, 255, AL_LEFT) + 18;
  kx += hud_keycap(kx, 660, "B", C_BONE, C_TAPE) + 6;
  hud_text_sh(F_MONO_S, kx, 664, S_KEY_BACK, C_BONE, 255, AL_LEFT);
  char wins[48];
  snprintf(wins, sizeof wins, S_WINS_TALLY, evening_wins(0), evening_wins(1), evening_wins(2), evening_wins(3));
  hud_text_sh(F_MONO_S, 1216, 664, wins, C_BONE, 255, AL_RIGHT);
  if (t < 1.5f) hud_dirty(0, 0, 1280, 720);
}

void rematch_menu(float t) {
  int x = 64, y = 520, w = 400, h = 176;
  hud_panel(x, y, w, h, C_SLAB, C_STEEL_HI);
  int focus = G.rematch_choice >= 0 && G.rematch_choice < 4 ? G.rematch_choice : RM_REBUILD;
  /* the focus block tweens between rows */
  float target = (float)(y + 8 + focus * 40);
  if (rematch_focus_y <= 0) rematch_focus_y = target;
  hud_tween(&rematch_focus_y, target, 18, 1.f / 60);
  int auto_left = REMATCH_AUTO / 60 - (int)t;
  if (auto_left < 0) auto_left = 0;
  /* the panel is uploaded only when something on it changed */
  uint32_t hsh = 2166136261u;
  hsh = (hsh ^ (uint32_t)auto_left) * 16777619u; hsh = (hsh ^ (uint32_t)focus) * 16777619u; hsh = (hsh ^ (uint32_t)(int)rematch_focus_y) * 16777619u;
  for (int s = 0; s < 4; s++) { hsh = (hsh ^ (uint32_t)(G.seats[s].rematch + 2)) * 16777619u; hsh = (hsh ^ (uint32_t)(G.rematch_votes[s] + 1)) * 16777619u; hsh = (hsh ^ (uint32_t)G.seats[s].kind) * 16777619u; }
  if (hsh != rematch_hash_prev) { rematch_hash_prev = hsh; hud_dirty(x, y, w + 6, h + 6); }
  for (int i = 0; i < 4; i++) {
    int ry = y + 8 + i * 40;
    char val[16] = "";
    if (i == RM_REBUILD) snprintf(val, sizeof val, S_AUTO_N, auto_left);
    int focused = i == focus;
    if (focused) {
      /* draw the hazard block at the tweened y, then the row text over it */
      hud_rect(x + 8 + 6, (int)rematch_focus_y + 6, w - 16, 40, C_SHADOW, 255);
      hud_rect(x + 8, (int)rematch_focus_y, w - 16, 40, C_HAZARD, 255);
    }
    uint32_t ink = focused ? C_TAPE : C_BONE;
    hud_text(F_H3, x + 24, ry + 7, REMATCH_ROWS[i], ink, 255, AL_LEFT);
    /* each seat's cursor is a 20 px chip at the row's right */
    int cx = x + w - 24;
    if (val[0]) { hud_text(F_MONO, cx, ry + 10, val, focused ? C_TAPE : C_ASH, 255, AL_RIGHT); cx -= hud_text_w(F_MONO, val) + 10; }
    for (int s = 3; s >= 0; s--) {
      if (G.seats[s].kind == SEAT_EMPTY || G.seats[s].rematch != i) continue;
      cx -= 24;
      hud_seat_chip(cx, ry + 10, 20, 20, s, F_MONO_S, G.seats[s].kind == SEAT_AI);
      if (G.rematch_votes[s]) hud_frame(cx - 2, ry + 8, 24, 24, 2, C_LIVE, 255);
    }
  }
}

/* ── the bracket card (TOURNAMENT, 10.21) ──────────────────────── */
static void bracket_chip(int x, int y, int seat, int w, int hot) {
  if (seat < 0 || seat > 3) {
    hud_rect(x, y, w, 28, C_GRIME, 255);
    hud_frame(x, y, w, 28, 1, C_STEEL, 255);
    hud_text(F_MONO_S, x + w / 2, cy_text(F_MONO_S, y, 28), S_BYE, C_SMOKE, 255, AL_CENTER);
    return;
  }
  const seat_t *s = &G.seats[seat];
  hud_rect(x, y, w, 28, hot ? C_HAZARD : C_GRIME, 255);
  hud_frame(x, y, w, 28, 1, hot ? C_HAZARD_DP : C_STEEL, 255);
  hud_seat_chip(x + 4, y + 4, 20, 20, seat, F_MONO_S, s->kind == SEAT_AI);
  char nm[24]; fit_text(nm, sizeof nm, F_MONO_S, s->name[0] ? s->name : S_BYE, w - 36);
  hud_text(F_MONO_S, x + 30, cy_text(F_MONO_S, y, 28), nm, hot ? C_TAPE : C_BONE, 255, AL_LEFT);
}

void bracket_card(const rect_t *r) {
  hud_panel(r->x, r->y, r->w, r->h, C_SLAB, C_STEEL_HI);
  hud_eyebrow(r->x + 16, r->y + 12, MODE_NAME[MODE_TOURNAMENT], C_ASH, AL_LEFT);
  int round = G.tournament_round;
  const char *rn = round <= 0 ? S_SEMIS : round == 1 ? S_THIRD_PLACE : S_FINAL;
  hud_text(F_MONO_S, r->x + r->w - 16, r->y + 12, rn, C_HAZARD, 255, AL_RIGHT);
  /* two columns of 170 px chips (the longest generated name fits): the semi pairs on the
   * left, the final pair on the right with the champion slot under it */
  int cw = 170, lx = r->x + 16, mx = r->x + 16 + cw + 24;
  int y0 = r->y + 56;
  int sem[2][2] = { { G.bracket[0], G.bracket[1] }, { G.bracket[2], G.bracket[3] } };
  int fin[2] = { -1, -1 }, champ = -1;
  for (int p = 0; p < 2; p++) for (int k = 0; k < 2; k++) {
    int s = sem[p][k];
    if (s < 0 || s > 3) continue;
    if (G.bracket_wins[s] >= 1 || s == G.bye_seat || sem[p][1 - k] < 0) fin[p] = s;
  }
  for (int p = 0; p < 2; p++) if (fin[p] >= 0 && G.bracket_wins[fin[p]] >= 2) champ = fin[p];
  static const int pair_y[2] = { 0, 110 };
  for (int p = 0; p < 2; p++) {
    int py = y0 + pair_y[p];
    int next = round <= 0 && fin[p] < 0;
    bracket_chip(lx, py, sem[p][0], cw, next);
    hud_text(F_MONO_S, lx + cw / 2, py + 29, S_VS, next ? C_HAZARD : C_SMOKE, 255, AL_CENTER);
    bracket_chip(lx, py + 44, sem[p][1], cw, next);
    /* caution tape joins the pair's midline to its slot in the final */
    hud_tape(lx + cw, py + 33, mx - lx - cw, 6, 0);
  }
  int final_next = round == 2 && champ < 0;
  /* the final chips sit on the pair midlines (y0 + 33 and y0 + 143) the tapes arrive at */
  bracket_chip(mx, y0 + 19, fin[0], cw, final_next);
  hud_text(F_MONO_S, mx + cw / 2, y0 + 80, S_VS, final_next ? C_HAZARD : C_SMOKE, 255, AL_CENTER);
  bracket_chip(mx, y0 + 129, fin[1], cw, final_next);
  /* the chain from the final down to the champion slot */
  hud_tape(mx + cw / 2 - 3, y0 + 157, 6, 26, 0);
  hud_text(F_MONO_S, mx + cw / 2, y0 + 185, S_CHAMPION, C_ASH, 255, AL_CENTER);
  if (champ >= 0) bracket_chip(mx, y0 + 206, champ, cw, 1);
  else { hud_rect(mx, y0 + 206, cw, 28, C_GRIME, 255); hud_frame(mx, y0 + 206, cw, 28, 1, C_STEEL, 255); }
  /* the NEXT tag above the pairing that fights next */
  int nx = round <= 0 ? lx : mx, ny = round <= 0 ? y0 + (fin[0] < 0 ? 0 : pair_y[1]) - 22 : y0 + 19 - 22;
  if (round == 1) { nx = lx; ny = y0 + pair_y[1] - 22; }
  hud_rect(nx, ny, hud_text_w(F_MONO_S, S_NEXT) + 16, 18, C_HAZARD, 255);
  hud_text(F_MONO_S, nx + 8, ny + 1, S_NEXT, C_TAPE, 255, AL_LEFT);
  hud_dirty(r->x, r->y, r->w + 6, r->h + 6);
}

/* ── the fight frame ───────────────────────────────────────────── */
static int in_tell_zone(const bot_t *b, const arena_def_t *a) {
  for (int k = 0; k < a->nhaz && k < MAX_HAZ; k++) {
    if (!M.haz[k].tell) continue;
    const hazard_def_t *h = &a->haz[k];
    float hw = h->w * 0.5f + 0.6f, hd = h->d * 0.5f + 0.6f;
    if (fabsf(b->b.x - h->x) <= hw && fabsf(b->b.z - h->z) <= hd) return 1;
  }
  return 0;
}

static int hazard_seats_available(const arena_def_t *a) { return M.players > 2 && arena_seat_hazard_count(a) > 0; }

static void fight_frame(float dt) {
  const arena_def_t *a = cur_arena();
  int compact = view_layout == LAY_QUADS || view_layout == LAY_THREE;
  SPEECH.t -= dt;
  for (int i = 0; i < nviews; i++) {
    const rect_t *vp = &views[i];
    int seat = view_seat[i];
    if (seat < 0) {
      /* the shared view: every strip along the bottom in seat order */
      int n = 0; const bot_t *bs[4];
      for (int k = 0; k < M.nbots && n < 4; k++) if (bot_is_player(&M.bots[k])) bs[n++] = &M.bots[k];
      if (n <= 2) for (int k = 0; k < n; k++) { rect_t r = { 640 * k, 0, 640, 720 }; hud_strip(&r, bs[k], 0); }
      else for (int k = 0; k < n; k++) { rect_t r = { 320 * k, 0, 320, 720 }; hud_strip_short(&r, bs[k]); }
      numbers_draw(i, vp, dt);
      speech_draw(i, vp);
      stinger_draw(i, vp, 0, dt);
      viewport_white(vp, fx_white_alpha(i));
      continue;
    }
    const bot_t *b = seat_bot(seat);
    if (b && !b->alive && hazard_seats_available(a)) {
      hazard_seat_view(vp, seat);
      stinger_draw(i, vp, compact, dt);
      continue;
    }
    if (b) {
      ghost_update(b, dt);
      if (in_tell_zone(b, a)) hazard_tape_strip(vp, G.tape_phase);
    }
    numbers_draw(i, vp, dt);
    speech_draw(i, vp);
    if (b && b->alive) chevron(i, vp, seat);
    if (b) countout(vp, b);
    stinger_draw(i, vp, compact, dt);
    if (view_layout == LAY_FULL && M.players <= 2 && nviews == 1 && b) {
      /* 1 player: a 640 strip bottom left and a 320 short strip for the practice bot bottom right */
      rect_t l = { 0, 0, 640, 720 };
      hud_strip(&l, b, 0);
      for (int k = 0; k < M.nbots; k++) if (bot_is_player(&M.bots[k]) && &M.bots[k] != b) { rect_t r = { 960, 0, 320, 720 }; hud_strip_short(&r, &M.bots[k]); break; }
    } else hud_strip(vp, b, compact);
    viewport_white(vp, fx_white_alpha(i));
  }
  /* shared overlays */
  tape_seams(view_layout);
  if (M.players > 0 && G.phase != PH_JUDGES) clock_chip(&M, compact);
  switch (view_layout) {
    case LAY_FULL: { rect_t r = { 1024, 16, 240, 135 }; minimap(&r, &M, a, 0); break; }
    case LAY_HALVES: { rect_t r = { 540, 66, 200, 112 }; minimap(&r, &M, a, 0); break; }
    case LAY_QUADS: { rect_t r = { 540, 304, 200, 112 }; minimap(&r, &M, a, 0); break; }
    case LAY_THREE: {
      rect_t mq = RECT_MAP_QUADRANT;
      hud_rect(mq.x, mq.y, mq.w, mq.h, C_CHAR, 255);
      char card[96], tally[48];
      snprintf(tally, sizeof tally, S_EVENING, evening_wins(0), evening_wins(1), evening_wins(2), evening_wins(3));
      snprintf(card, sizeof card, S_RESULTS_EYEBROW, a->name, MODE_NAME[M.mode % MODE_COUNT], tally);
      hud_text(F_MONO_S, 660, 372, card, C_ASH, 255, AL_LEFT);
      rect_t r = { 750, 420, 420, 236 };
      minimap(&r, &M, a, 1);
      break;
    }
    default: break;
  }
  if (G.next_match_chip_t > 0) next_match_chip(G.next_match_chip_pad, G.next_match_chip_t);
  tape_cut(cam_cut_frames());
  hud_dirty(0, 0, 1280, 720);
}

/* ── draw_hud ──────────────────────────────────────────────────── */
void draw_init(void) {
  memset(NUMS, 0, sizeof NUMS); memset(STING, 0, sizeof STING); memset(GHOST, 0, sizeof GHOST);
  SPEECH.t = 0; results_confetti_done = 0; rematch_focus_y = 0;
#ifdef RTR_FAKE_HUD
  extern void fake_hud_init(void);
  fake_hud_init();
#endif
}

#ifdef RTR_FAKE_HUD
extern void fake_hud_frame(float dt);
#endif

void draw_hud(float dt) {
  T += dt;
  hud_clear();
#ifdef RTR_FAKE_HUD
  fake_hud_frame(dt);
#endif
  /* the menu phases (draw_menus.c) own the whole frame; the countdown keeps the live strips */
  if (G.phase != PH_COUNTDOWN && G.phase != PH_FIGHT && menus_draw(dt)) {
    results_confetti_done = 0; rematch_focus_y = 0; rematch_hash_prev = 0;
    return;
  }
  switch (G.phase) {
    case PH_COUNTDOWN:
      fight_frame(dt);
      for (int i = 0; i < nviews; i++) countdown_overlay(&views[i], G.countdown_n, G.phase_t);
      break;
    case PH_FIGHT:
      fight_frame(dt);
      if (G.paused) {
        pause_overlay();
        if (G.controls_card_seat >= 0) { rect_t cr = { 320, 180, 640, 360 }; controls_card(&cr, 0, G.controls_card_seat); }
      }
      break;
    case PH_KO:
      if (G.miniko_frames > 0) fight_frame(dt);
      else ko_card(G.phase_t);
      break;
    case PH_JUDGES: fight_frame(dt); judges_card(G.phase_t); break;
    case PH_RESULTS: results_screen(G.phase_t); fx2_draw(dt); break;
    case PH_REMATCH: results_screen(G.phase_t + 6.f); rematch_menu(G.phase_t); fx2_draw(dt); break;
    default: break;
  }
  if (G.phase != PH_RESULTS && G.phase != PH_REMATCH) { results_confetti_done = 0; rematch_focus_y = 0; rematch_hash_prev = 0; }
}

/* ══════════════════════════════════════════════════════════════════
 * RTR_FAKE_HUD: a staged match that walks every fight screen by frame
 * ══════════════════════════════════════════════════════════════════ */
#ifdef RTR_FAKE_HUD
#include "world.h"
#include "render.h"

static t3_mesh *fake_box[6];

static void fake_bot(int slot, int seat, int preset, float hp, float x, float z, float h, int ai) {
  bot_t *b = &M.bots[slot];
  memset(b, 0, sizeof *b);
  build_preset(&b->build, preset);
  b->build.paint = (uint8_t)seat;
  bot_name(b->name, &b->build);
  b->seat = (uint8_t)seat; b->alive = 1; b->is_ai = (uint8_t)ai; b->preset = (uint8_t)preset;
  b->hp_max = 100; b->hp = hp;
  b->b.x = x; b->b.z = z; b->b.h = h; b->b.vx = sinf(h) * 2.5f; b->b.vz = cosf(h) * 2.5f;
  b->team = (uint8_t)(seat & 1);
  G.seats[seat].kind = ai ? SEAT_AI : SEAT_HUMAN; G.seats[seat].pad = ai ? -1 : (int8_t)seat; G.seats[seat].slot = slot;
  snprintf(G.seats[seat].name, sizeof G.seats[seat].name, "%s", b->name);
  G.seats[seat].rematch = -1;
}

static void fake_match(int players, int arena, int mode) {
  memset(&M, 0, sizeof M);
  for (int i = 0; i < 4; i++) { memset(&G.seats[i], 0, sizeof G.seats[i]); G.seats[i].pad = -1; G.seats[i].rematch = -1; G.seats[i].slot = -1; }
  M.arena = (uint8_t)arena; M.mode = (uint8_t)mode; M.players = (uint8_t)players; M.clock_s = 48; M.fight_len_s = 75; M.frame = 1620;
  const arena_def_t *a = &FAKE_ARENA;
  fake_bot(0, 0, PRE_SPIN, 58, -4, 2, 0.8f, 0);
  M.bots[0].spin = 1.0f; M.bots[0].boost_cd = 0;
  fake_bot(1, 1, PRE_LAUNCH, 88, 3, -3, -2.2f, 0);
  M.bots[1].red_tag_t = 300; M.bots[1].hp = 28; M.bots[1].cooldown = 50; M.bots[1].boost_cd = 150;
  M.nbots = 2;
  if (players >= 3) { fake_bot(2, 2, PRE_TOAST, 22, -6, -4, 1.5f, 0); M.bots[2].fuel = 130; M.bots[2].inverted = 1; M.bots[2].countout = 180; M.nbots = 3; }
  if (players >= 4) { fake_bot(3, 3, PRE_ROLL_OUT, 100, 7, 4, 3.0f, 1); M.nbots = 4; }
  if (players == 1) { fake_bot(1, 1, PRE_ROLL_OUT, 52, 3, -3, -2.2f, 1); M.bots[1].red_tag_t = 0; M.bots[1].hp = 52; }
  /* OPS parked in its zone */
  bot_t *o = &M.bots[4]; memset(o, 0, sizeof *o);
  o->is_ops = 1; o->seat = 4; o->alive = 1; o->b.x = a->ops_zone[0]; o->b.z = a->ops_zone[1]; o->hp = 300; o->hp_max = 300;
  snprintf(o->name, sizeof o->name, "%s", S_OPS);
  M.nbots = 5;
  G.seats[0].wins = 2; G.seats[1].wins = 1;
  G.players = (uint8_t)players; G.arena = (uint8_t)arena; G.mode = (uint8_t)mode;
  for (int s = 0; s < 4; s++) M.hazard_seat[s] = 255;
  cam_set_match(&M, 0, 0, a);
}

/* the staged hazards: THE BAY has no hazard table yet in the stub, so the fake adds the pit and two hammers */
static void fake_arena(void) {
  FAKE_ARENA = ARENAS[AR_BAY];
  FAKE_ARENA.nhaz = 4;
  FAKE_ARENA.haz[0] = (hazard_def_t){ .type = HZ_PIT, .x = 0, .z = 0, .w = 2, .d = 2, .period = 20, .active_s = 6 };
  FAKE_ARENA.haz[1] = (hazard_def_t){ .type = HZ_PULVERIZER, .x = 0, .z = -7.4f, .w = 1.2f, .d = 1.2f, .seat_ok = 1 };
  FAKE_ARENA.haz[2] = (hazard_def_t){ .type = HZ_PULVERIZER, .x = 0, .z = 7.4f, .w = 1.2f, .d = 1.2f, .seat_ok = 1 };
  FAKE_ARENA.haz[3] = (hazard_def_t){ .type = HZ_KILLSAW, .x = -6, .z = 0, .w = 1.6f, .d = 0.4f, .period = 7, .active_s = 1.5f, .tell_s = 1.5f };
}

void fake_hud_init(void) {
  fake_arena();
  /* a floor and some boxes so the HUD sits over a lit scene */
  t3_scene *S = world_scene();
  t3_material *fm = t3_mesh_standard_material_new(0xd9d3c7);
  fm->roughness = 0.9f;
  t3_mesh *floor_ = t3_mesh_new(t3_plane_geometry_new(60, 60, 1, 1), fm);
  t3_object_set_rotation(floor_, -1.5708f, 0, 0);
  t3_object_add(&S->base, floor_);
  t3_release(floor_); t3_release(fm);
  t3_geometry *g = t3_box_geometry_new(1.0f, 0.5f, 1.2f, 1, 1, 1);
  for (int i = 0; i < 6; i++) {
    t3_material *m = t3_mesh_standard_material_new(i < 4 ? SEAT_RGB[i] : C_STEEL);
    m->roughness = 0.6f; m->metalness = 0.2f;
    fake_box[i] = t3_mesh_new(g, m);
    t3_object_set_position(fake_box[i], -3 + i * 1.6f, 0.25f, (i % 2) * 1.5f);
    t3_object_add(&S->base, fake_box[i]);
    t3_release(m);
  }
  t3_release(g);
  G.phase = PH_FIGHT;
  fake_match(2, AR_BAY, MODE_DEATHMATCH);
}

/* the scenario per 200 frames: 0 2p halves, 1 1p full, 2 3p, 3 4p with a tell, 4 4p with a hazard seat,
 * 5 KO, 6 results, 7 rematch, 8 judges, 9 pause, 10 bracket, 11 closing time with 2V2 and OPS */
void fake_hud_frame(float dt) {
  (void)dt;
  int f = (int)G.frame;
  static int stage = -1;
  int s = f / 200; if (s > 12) s = 12;
  if (s != stage) {
    stage = s;
    G.phase_t = 0; G.paused = 0; G.miniko_frames = 0; G.timescale = 1;
    G.phase = PH_FIGHT;
    switch (s) {
      case 0: fake_match(2, AR_BAY, MODE_DEATHMATCH); break;
      case 1: fake_match(1, AR_BAY, MODE_DEATHMATCH); M.bots[1].b.x = 20; M.bots[1].b.z = 2; break;   /* the practice bot off screen: the chevron */
      case 2: fake_match(3, AR_BAY, MODE_HILL); M.hill = 1; M.score[0] = 12; M.score[1] = 7; M.score[2] = 3; break;
      case 3: fake_match(4, AR_BAY, MODE_DEATHMATCH); M.haz[3].tell = 1; M.bots[3].b.x = -6; M.bots[3].b.z = 0.5f; break;
      case 4: fake_match(4, AR_BAY, MODE_LAST_BOT); M.bots[2].alive = 0; M.bots[2].inverted = 0; M.bots[2].countout = 0; M.hazard_seat[2] = 1; M.haz[1].cooldown = 40; break;
      case 5: fake_match(2, AR_BAY, MODE_DEATHMATCH); G.phase = PH_KO; G.ko_victim = 1; G.ko_victor = 0; M.bots[1].hp = 0; M.bots[1].alive = 0; M.win_how = KO_FLIP; break;
      case 6: case 7: case 10:
        fake_match(4, AR_BAY, s == 10 ? MODE_TOURNAMENT : MODE_DEATHMATCH);
        M.bots[0].place = 1; M.bots[0].ko_how = KO_FLIP; M.bots[0].dealt = 64; M.bots[0].biggest = 30; M.bots[0].airtime = 4.2f; M.bots[0].flips = 3;
        M.bots[1].place = 0; M.bots[1].dealt = 112; M.bots[1].biggest = 31; M.bots[1].flips = 0;
        M.bots[2].place = 2; M.bots[2].ko_how = KO_PIT; M.bots[2].dealt = 22; M.bots[2].biggest = 9; M.bots[2].airtime = 6.0f; M.bots[2].flips = 2;
        M.bots[3].place = 3; M.bots[3].ko_how = KO_COUNT; M.bots[3].dealt = 8; M.bots[3].biggest = 8;
        M.win_how = KO_FLIP; M.winner = 1;
        G.award[0] = AW_SPINNER; G.award[1] = AW_DOORSTOP; G.award[2] = AW_LEFT; G.award[3] = AW_ARRIVED;
        G.seats[0].wins = 3; G.seats[1].wins = 1;
        G.phase = s == 7 ? PH_REMATCH : PH_RESULTS;
        G.rematch_choice = RM_REBUILD;
        if (s == 7) { G.seats[0].rematch = RM_REBUILD; G.seats[1].rematch = RM_NEW_ARENA; G.seats[2].rematch = RM_REBUILD; G.seats[3].rematch = RM_SAME_BOTS; G.rematch_votes[0] = 1; G.rematch_votes[3] = 1; }
        if (s == 10) { G.tournament_round = 2; G.bracket[0] = 0; G.bracket[1] = 2; G.bracket[2] = 1; G.bracket[3] = 3; G.bracket_wins[0] = 1; G.bracket_wins[1] = 1; G.bye_seat = -1; }
        break;
      case 8:
        fake_match(2, AR_BAY, MODE_DEATHMATCH); G.phase = PH_JUDGES; M.clock_s = 0; M.win_how = WIN_JUDGES;
        G.judge_bars[0][0] = 5.2f; G.judge_bars[0][1] = 1.8f; G.judge_bars[1][0] = 1.9f; G.judge_bars[1][1] = 2.1f; G.judge_bars[2][0] = 1.0f; G.judge_bars[2][1] = 3.0f;
        G.judge_winner = 0; G.judges_rule = 0;
        cam_set_match(&M, 2, 0, &ARENAS[AR_BAY]);
        break;
      case 12:   /* the judges tie: the UNDERDOG RULE chip and the cheaper build line */
        fake_match(2, AR_BAY, MODE_DEATHMATCH); G.phase = PH_JUDGES; M.clock_s = 0; M.win_how = WIN_JUDGES;
        G.judge_bars[0][0] = 4.0f; G.judge_bars[0][1] = 4.0f; G.judge_bars[1][0] = 2.0f; G.judge_bars[1][1] = 2.0f; G.judge_bars[2][0] = 1.5f; G.judge_bars[2][1] = 1.5f;
        G.judge_winner = 1; G.judges_rule = 1;
        cam_set_match(&M, 2, 0, &ARENAS[AR_BAY]);
        break;
      case 9: fake_match(2, AR_BAY, MODE_DEATHMATCH); G.paused = 1; G.pauser = 1; G.pause_row = 3; G.reset_hold = 0.6f; break;
      case 11: fake_match(2, AR_BAY, MODE_2V2); M.closing = 1; M.clock_s = 9.4f; M.players = 4; M.bots[0].team = 0; M.bots[1].team = 1; break;
    }
    if (s == 0 || s == 11) { stinger(0, ST_AIRBORNE[0], SEAT_RGB[1]); }
    if (s == 2) stinger(0, ST_BIGHIT[0], SEAT_RGB[1]);
    if (s == 3) { stinger(0, ST_INVERTED[0], SEAT_RGB[2]); M.bots[0].inverted = 1; M.bots[0].countout = 200; }
    if (s == 4) stinger(2, ST_KO[0], SEAT_RGB[0]);
    if (s == 11) speech_panel(-1, M.bots[4].b.x, 0, M.bots[4].b.z, OPS_LINES[0]);
  }
  if (G.phase == PH_FIGHT && (f % 40) == 5) {
    const bot_t *v = &M.bots[1];
    damage_number(0, v->b.x, 0.5f, v->b.z, 24, SEAT_RGB[0], 1);
    damage_number(nviews > 1 ? 1 : 0, M.bots[0].b.x, 0.5f, M.bots[0].b.z, -12, C_RUST, 0);
    bot_t *w = &M.bots[1];
    w->hp = w->hp > 20 ? w->hp - 9 : 88;
  }
  /* cameras: each viewport looks at the boxes from its own side */
  rect_t vp[4]; int n;
  cam_current(vp, &n);
  for (int i = 0; i < n; i++) {
    t3_camera *c = cam_get(i);
    if (!c) continue;
    t3_object_set_position(c, -2 + i * 1.5f, 2.4f, -4.5f + i * 0.3f);
    t3_object_look_at(c, 0.5f, 0.2f, 0.8f);
  }
}
#endif
