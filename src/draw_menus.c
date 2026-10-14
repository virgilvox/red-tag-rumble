/* draw_menus.c: boot, title and attract, lobby, garage panels, arena vote,
 * countdown, settings, hall of fame, rules flash, controls card, pause (spec
 * 10.7 to 10.13, 10.22 to 10.26). Owned by the menus agent (item 11).
 *
 * Everything here reads game state (G, SAVE, M, PADS) and paints the HUD
 * buffer; nothing here changes game state. The only state this file keeps is
 * draw side: cursor tweens, card lift tweens, the over budget shake timers and
 * the lobby count timers. Every string comes from content.h or parts.h.
 *
 * Holes: render_frame clears only inside each viewport's scissor, so every
 * menu that has a 3D turntable paints an opaque background and cuts a hole
 * (alpha 0) where the turntable viewport is. garage_hole and lobby_hole give
 * the integration layer the same rects.
 *
 * seat_t.join_slide is read as the distance a panel still has to travel from
 * its screen side (1 = fully out, 0 = in place), so a seat the game never
 * animates draws in place. */
#include "draw_menus.h"
#include "content.h"
#include "parts.h"
#include "juice.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* -- draw side state  -- */
static float title_cursor_y = -1;
static float lobby_lift[MODE_COUNT];
static float lobby_empty_t, lobby_auto_t;
static float garage_shake_t[4];
static int   garage_last_cost[4] = { -1, -1, -1, -1 };
static uint32_t garage_last_deny[4];    /* seat_t.deny_frame last seen: a refused lock shakes the tag bar again */
static float vote_fx[4], vote_fy[4];
static int   vote_init;
static float settings_cursor_y = -1, pause_cursor_y = -1, match_cursor_y = -1;
static const arena_def_t *VOTE_ARENAS = ARENAS;   /* the vote screen's arena table (the fake state swaps it) */
static float flash_wipe;                /* 0..1 wipe between rules cards */
static int   flash_last_step = -1;

/* -- small helpers  -- */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/* the y that centres a font's line box in a box */
static int text_cy(int font, int by, int bh) { int fh = hud_font_h(font); return by + (bh - fh) / 2 - fh / 8; }
/* a 1 Hz pulse 0..1 */
static float pulse(float t, float hz) { return 0.5f + 0.5f * sinf(t * hz * 6.2831853f); }

/* cut a hole (alpha 0) in the HUD buffer so the 3D viewport shows through */
static void hole(int x, int y, int w, int h) {
  int x0 = clampi(x / HUD_DIV, 0, HUD_W), x1 = clampi((x + w) / HUD_DIV, 0, HUD_W);
  int y0 = clampi(y / HUD_DIV, 0, HUD_H), y1 = clampi((y + h) / HUD_DIV, 0, HUD_H);
  for (int yy = y0; yy < y1; yy++) {
    uint8_t *row = hud_px + ((HUD_H - 1 - yy) * HUD_W + x0) * 4;
    memset(row, 0, (size_t)(x1 - x0) * 4);
  }
}

/* a keycap followed by its label in F_MONO_S; returns the width used */
static int key_hint(int x, int y, const char *key, const char *label, uint32_t ink) {
  int w = hud_keycap(x, y, key, C_BONE, C_TAPE);
  if (label && *label) { hud_text(F_MONO_S, x + w + 8, y + 4, label, ink, 255, AL_LEFT); w += 8 + hud_text_w(F_MONO_S, label); }
  return w;
}

/* a GANTRY chip: char fill with a 6 px shadow */
static void chip(int x, int y, int w, int h) {
  hud_rect(x + 6, y + 6, w, h, C_SHADOW, 255);
  hud_rect(x, y, w, h, C_CHAR, 255);
}

/* a stepper value with chevrons: "< VALUE >" centred on cx in a font */
static void stepper(int cx, int y, int font, const char *value, uint32_t ink, int can_left, int can_right, int focused) {
  int vw = hud_text_w(font, value), fh = hud_font_h(font);
  hud_text(font, cx, y, value, ink, 255, AL_CENTER);
  uint32_t cl = can_left ? (focused ? C_TAPE : C_HAZARD) : C_STEEL, cr = can_right ? (focused ? C_TAPE : C_HAZARD) : C_STEEL;
  hud_chevron(cx - vw / 2 - 18, y + fh / 2 + 2, -1, 0, 12, 3, cl, 255);
  hud_chevron(cx + vw / 2 + 18, y + fh / 2 + 2, 1, 0, 12, 3, cr, 255);
}

/* the focus block of a menu_row stack at a tweened y (the caller keeps cur_y), then the
 * focused row's text in tape ink once the block has reached it; the other rows use menu_row */
static float focus_block(float *cur_y, int target_y, int x, int w, float dt) {
  if (*cur_y < 0) *cur_y = (float)target_y;
  hud_tween(cur_y, (float)target_y, 18, dt);
  int y = (int)(*cur_y + 0.5f);
  hud_rect(x + 6, y + 6, w, 40, C_SHADOW, 255);
  hud_rect(x, y, w, 40, C_HAZARD, 255);
  return fabsf(*cur_y - (float)target_y);
}
static void focus_row_text(int x, int y, int w, const char *label, const char *value, int arrived, int is_choice, int can_left, int can_right) {
  uint32_t ink = arrived ? C_TAPE : C_BONE;
  hud_text(F_H3, x + 16, y + 7, label, ink, 255, AL_LEFT);
  if (value && *value) {
    int vw = hud_text_w(F_MONO_L, value), vx = x + w - 16;
    if (is_choice) {
      hud_chevron(vx - 7, y + 20, 1, 0, 14, 3, can_right ? (arrived ? C_TAPE : C_HAZARD) : C_STEEL, 255);
      vx -= 24;
      hud_text(F_MONO_L, vx, y + 5, value, ink, 255, AL_RIGHT);
      hud_chevron(vx - vw - 17, y + 20, -1, 0, 14, 3, can_left ? (arrived ? C_TAPE : C_HAZARD) : C_STEEL, 255);
    } else hud_text(F_MONO_L, vx, y + 5, value, ink, 255, AL_RIGHT);
  }
}

/* caution tape at any size: hazard fill with tape dark stripes, 1 px tape frame */
static void mini_tape(int x, int y, int w, int h) {
  hud_rect(x, y, w, h, C_HAZARD, 255);
  hud_clip(x, y, w, h);
  for (int k = -h; k < w + h; k += 8) hud_line(x + k, y, x + k + h, y + h, 4, C_TAPE, 255);
  hud_clip_clear();
  hud_frame(x, y, w, h, 1, C_TAPE, 255);
}

/* the shared timer tape: shrinks from both ends toward x 640, the seconds in a char chip */
static void timer_tape(int frames_left, int total, float t) {
  if (total <= 0) total = 1;
  float frac = clampf((float)frames_left / (float)total, 0, 1);
  int w = (int)(1280 * frac);
  hud_rect(0, 696, 1280, 24, C_CHAR, 255);
  if (w > 4) hud_tape(640 - w / 2, 696, w, 24, G.tape_phase);
  int s = frames_left > 0 ? (frames_left + 59) / 60 : 0;   /* never a negative clock, whatever the totals do */
  char buf[12];
  snprintf(buf, sizeof buf, "%d:%02d", s / 60, s % 60);
  uint32_t ink = C_BONE;
  if (s <= 10 && ((int)(t * 4) & 1)) ink = C_RUST;
  hud_rect(596, 696, 88, 24, C_CHAR, 255);
  hud_frame(596, 696, 88, 24, 2, s <= 10 ? C_RUST : C_STEEL_HI, 255);
  hud_text(F_MONO, 640, text_cy(F_MONO, 696, 24), buf, ink, 255, AL_CENTER);
}

static int seated_count(void) { int n = 0; for (int i = 0; i < 4; i++) if (G.seats[i].kind != SEAT_EMPTY) n++; return n; }
static int players_now(void) { int n = G.players > 0 ? G.players : seated_count(); return n < 2 ? 2 : n; }
static const char *preset_label(const build_t *b) { int p = build_preset_of(b); return p == PRE_CUSTOM ? S_CUSTOM : PRESETS[p].name; }

/* the HeatSync marquee (10.7): the mark, the eyebrow, RED TAG and RUMBLE. The
 * one hud_logo call in the cart, flame bone, the sun untouched. */
static void marquee(void) {
  hud_logo(64, 56, 160, C_BONE);
  hud_text(F_MONO_S, 248, 70, S_EYEBROW_TITLE, C_ASH, 255, AL_LEFT);
  hud_text_sh(F_H1, 248, 90, S_TITLE_1, C_BONE, 255, AL_LEFT);
  hud_text_sh(F_H1, 248, 156, S_TITLE_2, C_HAZARD, 255, AL_LEFT);
}

/* -- boot (10.1)  -- */
void boot_screen(float frac) {
  hud_rect(0, 0, 1280, 720, C_CHAR, 255);
  hud_tape(0, 0, 1280, 24, G.tape_phase);
  hud_tape(0, 696, 1280, 24, G.tape_phase);
  marquee();
  /* the 12 segment tape bar fills with the real compile progress (one scene per frame, game_flow.c boot_update) */
  int n = (int)(clampf(frac, 0, 1) * 12 + 0.5f);
  int x0 = 640 - (12 * 64 - 4) / 2;
  for (int k = 0; k < 12; k++) {
    if (k < n) mini_tape(x0 + k * 64, 480, 60, 24);
    else hud_frame(x0 + k * 64, 480, 60, 24, 2, C_STEEL, 255);
  }
  hud_text(F_MONO_S, 640, 520, S_BOOT, C_ASH, 255, AL_CENTER);
  if (frac < 1) hud_text(F_MONO_S, 640, 544, game_boot_step_name(), C_SMOKE, 255, AL_CENTER);
  hud_dirty(0, 0, 1280, 720);
}

/* -- title and attract (10.7)  -- */
void attract_overlay(void) {
  if (!G.attract || M.nbots < 2) return;
  const int bx[2] = { 496, 1064 };
  for (int i = 0; i < 2 && i < M.nbots; i++) {
    const bot_t *b = &M.bots[i];
    float f = b->hp_max > 0 ? clampf(b->hp / b->hp_max, 0, 1) : 0;
    hud_rect(bx[i] - 8, 28, 216, 36, C_CHAR, 210);           /* a char backing so the bars read over a light floor */
    hud_rect(bx[i], 32, 200, 10, C_STEEL, 200);
    hud_rect(bx[i], 32, (int)(200 * f), 10, C_ASH, 255);
    hud_text(F_MONO_S, i == 0 ? bx[i] : bx[i] + 200, 46, b->name, C_ASH, 255, i == 0 ? AL_LEFT : AL_RIGHT);
  }
  int tip = (int)(G.phase_t / 8) % LEN(ATTRACT_TIPS);
  int tw = hud_text_w(F_MONO, ATTRACT_TIPS[tip]) + 32;
  hud_rect(880 - tw / 2, 596, tw, 36, C_CHAR, 220);     /* clear of the BEST EVENING line at 668 */
  hud_text(F_MONO, 880, 602, ATTRACT_TIPS[tip], C_BONE, 255, AL_CENTER);
}

void title_screen(float dt) {
  float t = G.phase_t;
  /* tape, the left column scrim, the marquee */
  hud_tape(0, 0, 1280, 24, G.tape_phase);
  hud_tape(0, 696, 1280, 24, G.tape_phase);
  hud_rect(0, 24, 480, 672, C_CHAR, 205);
  marquee();
  /* the tagline rotates every 6 s */
  hud_text_wrap(F_H3, 80, 250, 400, TAGLINES[(int)(t / 6) % LEN(TAGLINES)], C_BONE, 255, 30);
  /* menu rows: the hazard cursor tweens to the focused row */
  int row = clampi(G.title_row, 0, TR_COUNT - 1);
  float target = (float)(360 + 44 * row);
  if (title_cursor_y < 0) title_cursor_y = target;
  hud_tween(&title_cursor_y, target, 18.0f, dt);
  int cy = (int)(title_cursor_y + 0.5f);
  hud_rect(86, cy + 6, 380, 40, C_SHADOW, 255);
  hud_rect(80, cy, 380, 40, C_HAZARD, 255);
  for (int i = 0; i < TR_COUNT; i++) {
    int y = 360 + 44 * i;
    int on = i == row;
    hud_text(F_H2, 96, y + 2, TITLE_ROWS[i], on ? C_TAPE : C_BONE, 255, AL_LEFT);
    if (TITLE_HINTS[i][0]) hud_text(F_MONO_S, 452, y + 13, TITLE_HINTS[i], on ? C_TAPE : C_ASH, 255, AL_RIGHT);
  }
  /* A or Start selects a row (title_update reads nothing else) */
  int kx = 80;
  kx += hud_keycap(kx, 660, "A", C_BONE, C_TAPE) + 6;
  key_hint(kx, 660, "START", S_KEY_SELECT, C_ASH);
  if (SAVE.best_evening_wins > 0) {
    char buf[48];
    snprintf(buf, sizeof buf, S_BEST_EVENING, (int)SAVE.best_evening_wins, SAVE.best_seat + 1);
    hud_text_sh(F_MONO_S, 1248, 668, buf, C_ASH, 255, AL_RIGHT);
  }
  attract_overlay();
  hud_dirty(0, 0, 1280, 720);
}

/* -- lobby (10.8)  -- */
static const int MODE_GLYPH[MODE_COUNT] = { GL_SPINNER, GL_FLIPPER, GL_SKULL, GL_CROWN, GL_TWO_BOTS, GL_BRACKET };
static const int MODE_MIN[MODE_COUNT] = { 1, 2, 3, 2, 4, 3 };
static const int MODE_MAX[MODE_COUNT] = { 4, 4, 4, 4, 4, 4 };

void lobby_hole(int seat, rect_t *out) { *out = (rect_t){ 20 + 310 * seat + 16, 252, 268, 120 }; }

static void lobby_seat_card(int i, float t, float dt) {
  (void)dt;
  const seat_t *s = &G.seats[i];
  int x = 20 + 310 * i, y = 220, w = 300, h = 220;
  uint32_t sc = seat_colour(i);
  hud_rect(x + 6, y + 6, w, h, C_SHADOW, 255);
  hud_rect(x, y, w, h, C_SLAB, 255);
  hud_frame(x, y, w, h, 2, s->kind != SEAT_EMPTY && s->ready ? C_LIVE : C_STEEL_HI, 255);
  if (s->kind == SEAT_EMPTY) {
    hud_seat_chip(x + 12, y + 12, 28, 28, i, F_H3, 0);
    int a = 140 + (int)(115 * pulse(t, 1));
    hud_text(F_H2, x + w / 2, y + 62, S_PRESS_A_TO_JOIN_1, C_BONE, a, AL_CENTER);
    hud_text(F_H2, x + w / 2, y + 104, S_PRESS_A_TO_JOIN_2, C_BONE, a, AL_CENTER);
    hud_text(F_MONO_S, x + 16, y + 180, S_FILL_WITH_BOT, C_ASH, 255, AL_LEFT);
    int fill = G.fill_with_bot;
    stepper(x + w - 64, y + 194, F_MONO, VAL_OFF_ON[fill ? 1 : 0], C_BONE, fill, !fill, 0);
    return;
  }
  int ai = s->kind == SEAT_AI;
  hud_seat_chip(x + 12, y + 3, 28, 28, i, F_H3, ai);
  hud_glyph(x + 48, y + 9, ai ? GL_AI : GL_PAD, 1, C_ASH, 255);
  /* the leave ring on this card's B cap, and the idle drop count */
  if (!ai && s->pad >= 0) {
    int bx = x + w - 48, by = y + 4;
    float idle = PADS[s->pad].idle;
    if (s->leave_hold > 0) {
      hud_keycap(bx, by, "B", C_BONE, C_TAPE);
      hud_ring(bx + 12, by + 12, 15, 3, clampf(s->leave_hold, 0, 1), sc, 255);
    } else if (idle > 50) {
      char n[4]; snprintf(n, sizeof n, "%d", clampi((int)(60 - idle) + 1, 0, 10));
      hud_text(F_H3, x + w - 16, y + 2, n, C_RUST, 255, AL_RIGHT);
    }
  }
  /* the turntable hole */
  rect_t hr; lobby_hole(i, &hr);
  hud_frame(hr.x - 1, hr.y - 1, hr.w + 2, hr.h + 2, 1, C_STEEL, 255);
  hole(hr.x, hr.y, hr.w, hr.h);
  /* name, preset and tags, wins, scheme, ready */
  char name[24]; bot_name(name, &s->build);
  hud_text(F_H3, x + 16, y + 154, name, C_BONE, 255, AL_LEFT);
  char line[48];
  snprintf(line, sizeof line, "%s  ", preset_label(&s->build));
  char tags[16]; snprintf(tags, sizeof tags, S_TAGS_N, build_cost(&s->build));
  strncat(line, tags, sizeof line - strlen(line) - 1);
  hud_text(F_MONO_S, x + 16, y + 182, line, C_ASH, 255, AL_LEFT);
  for (int k = 0; k < s->wins && k < 12; k++) hud_rect(x + 16 + k * 14, y + 202, 10, 10, C_BONE, 255);
  if (ai) {
    hud_text(F_MONO_S, x + w - 16, y + 198, VAL_DIFFICULTY[clampi(s->ai_level, 0, 2)], C_ASH, 255, AL_RIGHT);
  } else {
    /* the control scheme is toggled by X, so the X cap carries the value; TANK says it wants two sticks */
    int tank = s->scheme == SCHEME_TANK;
    const char *sv = tank ? S_TANK : S_ARCADE;
    int sw = hud_text_w(F_MONO_S, "X") + 14 + 8 + hud_text_w(F_MONO_S, sv);
    key_hint(x + w - 16 - sw, y + 194, "X", sv, C_BONE);
    if (tank) hud_text(F_MONO_S, x + w - 16, y + 182, S_TANK_NOTE, C_ASH, 255, AL_RIGHT);   /* beside the tags line, inside the card */
  }
  if (s->ready) {
    hud_text(F_MONO_S, x + 16, y + 198, S_READY, C_LIVE, 255, AL_LEFT);
    hud_rect(x, y + h - 4, w, 4, C_LIVE, 255);
  } else if (!ai) {
    key_hint(x + 16, y + 194, "A", S_READY, C_ASH);         /* A marks READY; Start goes */
  }
}

void lobby_screen(float dt) {
  float t = G.phase_t;
  int seated = seated_count(), players = seated;
  hud_rect(0, 0, 1280, 720, C_CHAR, 255);
  hud_eyebrow(32, 14, S_PICK_A_MODE, C_ASH, AL_LEFT);
  char tally[48];
  snprintf(tally, sizeof tally, S_EVENING, G.seats[0].wins, G.seats[1].wins, G.seats[2].wins, G.seats[3].wins);
  hud_text(F_MONO_S, 1248, 14, tally, C_ASH, 255, AL_RIGHT);
  /* mode cards */
  int cur = clampi(G.lobby_mode_cursor, 0, MODE_COUNT - 1);
  for (int i = 0; i < MODE_COUNT; i++) {
    hud_tween(&lobby_lift[i], i == cur ? 4.0f : 0.0f, 16.0f, dt);
    int x = 20 + 208 * i, y = 40, w = 198, h = 100;
    int lift = (int)(lobby_lift[i] + 0.5f);
    int ok = players == 0 || (players >= MODE_MIN[i] && players <= MODE_MAX[i]);
    hud_rect(x + 6 + lift, y + 6 + lift, w, h, C_SHADOW, 255);
    hud_rect(x - lift / 2, y - lift / 2, w, h, C_SLAB, 255);
    hud_frame(x - lift / 2, y - lift / 2, w, h, 2, i == cur ? C_HAZARD : C_STEEL_HI, 255);
    uint32_t ink = ok ? C_BONE : C_SMOKE;
    /* a one word name too wide for the card in F_H3 drops to F_MONO_L so it stays inside the frame */
    int nf = hud_text_w(F_H3, MODE_CARD_1[i]) > w - 20 ? F_MONO_L : F_H3;
    hud_text(nf, x + 8 - lift / 2, y + 10 - lift / 2, MODE_CARD_1[i], ink, 255, AL_LEFT);
    if (MODE_CARD_2[i][0]) hud_text(F_H3, x + 8 - lift / 2, y + 38 - lift / 2, MODE_CARD_2[i], ink, 255, AL_LEFT);
    hud_glyph(x + w - 44 - lift / 2, y + h - 44 - lift / 2, MODE_GLYPH[i], 2, ok ? (i == cur ? C_HAZARD : C_ASH) : C_SMOKE, 255);
  }
  hud_text(F_MONO, 32, 150, MODE_BLURB[cur], C_ASH, 255, AL_LEFT);
  /* seat cards */
  for (int i = 0; i < 4; i++) lobby_seat_card(i, t, dt);
  /* the leave hint and HOW IT WORKS */
  int hx = 32;
  hx += key_hint(hx, 452, "B", S_HOLD_B_LEAVE, C_ASH) + 24;
  key_hint(hx, 452, "Y", S_LOBBY_RULES, C_ASH);
  /* the three steppers */
  {
    const char *lab[3] = { S_ONE_CAMERA, S_SPLIT, S_COMEBACK };
    const char *val[3] = { VAL_ONE_CAMERA[clampi(SAVE.one_camera, 0, 2)], VAL_SPLIT[clampi(SAVE.split, 0, 2)], VAL_AUTO_ON_OFF[clampi(SAVE.comeback, 0, 2)] };
    int idx[3] = { SAVE.one_camera, SAVE.split, SAVE.comeback };
    for (int k = 0; k < 3; k++) {
      int cx = 320 + 320 * k;
      hud_text(F_MONO_S, cx, 508, lab[k], C_ASH, 255, AL_CENTER);
      if (G.lobby_row == k + 1) { hud_rect(cx - 124, 500, 248, 56, C_SLAB, 255); hud_frame(cx - 124, 500, 248, 56, 2, C_HAZARD, 255); hud_text(F_MONO_S, cx, 508, lab[k], C_ASH, 255, AL_CENTER); }
      stepper(cx, 526, F_MONO, val[k], C_BONE, idx[k] > 0, idx[k] < 2, 0);
    }
  }
  /* start: any ready seat; auto start 25 s after the first READY */
  int any_ready = 0;
  for (int i = 0; i < 4; i++) if (G.seats[i].kind != SEAT_EMPTY && G.seats[i].ready) any_ready = 1;
  if (any_ready) lobby_auto_t += dt; else lobby_auto_t = 0;
  int sw = hud_text_w(F_MONO_S, "START") + 14 + 8 + hud_text_w(F_MONO, S_GO_TO_GARAGE);
  int sx = 640 - sw / 2;
  /* Start from any seat readies it and goes, so the cap is live as soon as someone sits down */
  int kw = hud_keycap(sx, 584, "START", seated > 0 ? C_HAZARD : C_BONE, C_TAPE);
  hud_text(F_MONO, sx + kw + 8, 586, S_GO_TO_GARAGE, seated > 0 ? C_BONE : C_SMOKE, 255, AL_LEFT);
  if (any_ready) {
    char buf[24];
    int left = (int)(LOBBY_AUTO_S - lobby_auto_t + 0.99f);
    snprintf(buf, sizeof buf, S_AUTO_IN, clampi(left, 0, (int)LOBBY_AUTO_S));
    hud_text(F_MONO_S, 1248, 590, buf, C_ASH, 255, AL_RIGHT);
  }
  /* the bottom tape: the empty count only when no seat */
  hud_tape(0, 696, 1280, 24, G.tape_phase);
  if (seated == 0) {
    lobby_empty_t += dt;
    char buf[32];
    snprintf(buf, sizeof buf, S_LOBBY_EMPTY, clampi((int)(LOBBY_EMPTY_S - lobby_empty_t + 0.99f), 0, (int)LOBBY_EMPTY_S));
    int w = hud_text_w(F_MONO, buf) + 32;
    hud_rect(640 - w / 2, 696, w, 24, C_CHAR, 255);
    hud_text(F_MONO, 640, text_cy(F_MONO, 696, 24), buf, C_BONE, 255, AL_CENTER);
  } else lobby_empty_t = 0;
  /* the controls card over a seat's card for 3 s after its pad's first join (10.24) */
  if (G.controls_card_t > 0 && G.controls_card_seat >= 0 && G.controls_card_seat < 4 && G.seats[G.controls_card_seat].kind == SEAT_HUMAN) {
    rect_t cr = { 20 + 310 * G.controls_card_seat - 10, 240, 320, 180 };
    if (cr.x < 8) cr.x = 8;
    if (cr.x + cr.w > 1272) cr.x = 1272 - cr.w;
    controls_card(&cr, 1, G.controls_card_seat);
  }
  hud_dirty(0, 0, 1280, 720);
}

/* -- garage (10.9 to 10.11)  -- */
void garage_hole(int compact, const rect_t *panel, rect_t *out) {
  if (compact) *out = (rect_t){ panel->x + 16, panel->y + 32, 300, 230 };
  else *out = (rect_t){ panel->x + 16, panel->y + 100, 608, 230 };
}

/* a small tag for the quadrant header bars: 10 x 8 at pitch 12 */
static void tag_small(int x, int y, int filled, int bonus, int over) {
  uint32_t fill = filled ? (over ? C_RUST : C_HAZARD) : C_CHAR;
  uint32_t frame = bonus ? C_LIVE : C_STEEL;
  hud_rect(x + 2, y, 8, 8, fill, 255);
  hud_rect(x, y + 2, 2, 6, fill, 255);
  if (!filled) { hud_rect(x + 2, y, 8, 1, frame, 255); hud_rect(x, y + 7, 10, 1, frame, 255); hud_rect(x + 9, y, 1, 8, frame, 255); hud_rect(x, y + 2, 1, 6, frame, 255); }
  else if (bonus) { hud_rect(x + 2, y, 8, 1, frame, 255); hud_rect(x, y + 7, 10, 1, frame, 255); }
  hud_rect(x + 7, y + 3, 1, 1, filled ? C_TAPE : C_STEEL, 255);
}

/* the budget bar: budget tags (bonus ones framed live), the spent ones hazard, 19 and 20
 * of 20 rust, over budget segments in C_FAULT beyond the bar, shaking for 0.3 s */
static int tag_bar(int x, int y, const build_t *b, int small, float shake_t) {
  int budget = build_budget(b), spent = build_cost(b);
  int pitch = small ? 12 : 16, tw = small ? 10 : 14, th = small ? 8 : 10;
  int sx = x;
  if (shake_t > 0) sx += ((int)(shake_t * 16) & 1) ? 2 : -2;
  int bonus_from = BUDGET_BASE;
  for (int k = 0; k < budget; k++) {
    int filled = k < spent;
    int over = budget == BUDGET_BASE && filled && k >= 18;
    if (small) tag_small(sx + k * pitch, y, filled, k >= bonus_from, over);
    else hud_tag(sx + k * pitch, y, filled, k >= bonus_from, over);
  }
  int extra = spent - budget;
  for (int k = 0; k < extra && k < 8; k++) {
    int tx = sx + (budget + k) * pitch;
    hud_rect(tx + (small ? 2 : 3), y, tw - (small ? 2 : 3), th, C_FAULT, 255);
    hud_rect(tx, y + (small ? 2 : 3), small ? 2 : 3, th - (small ? 2 : 3), C_FAULT, 255);
  }
  return (budget + (extra > 0 ? extra : 0)) * pitch;
}

/* one of the three bars: the value animates over 18 frames, the delta segment holds 72 */
static void stat_bar(int x, int y, int w, const seat_t *s, int k) {
  int f = (int)G.frame - s->bars_from_t;
  float o = s->bars_old[k], n = s->bars_new[k], v = n;
  if (s->bars_from_t > 0 && f >= 0 && f < J_BARS_ANIM_FRAMES) v = o + (n - o) * hud_ease_out((float)f / J_BARS_ANIM_FRAMES);
  int show_delta = s->bars_from_t > 0 && f >= 0 && f < J_BARS_ANIM_FRAMES + J_BARS_DELTA_HOLD && fabsf(n - o) > 0.005f;
  hud_rect(x, y, w, 10, C_SLAB, 255);
  hud_frame(x, y, w, 10, 1, C_STEEL, 255);
  int iw = w - 2;
  float lo = show_delta ? fminf(o, v) : v;
  hud_rect(x + 1, y + 1, (int)(iw * clampf(lo, 0, 1)), 8, C_BONE, 255);
  if (show_delta) {
    int a = (int)(iw * clampf(fminf(o, v), 0, 1)), bq = (int)(iw * clampf(fmaxf(o, v), 0, 1));
    if (n > o) hud_rect(x + 1 + a, y + 1, bq - a, 8, C_LIVE, 255);
    else hud_rect(x + 1 + a, y + 1, bq - a, 8, C_RUST, 255);
  }
}

static void bars_block(int x, int y, int w, int pitch, const seat_t *s) {
  for (int k = 0; k < BAR_COUNT; k++) {
    hud_text(F_MONO_S, x, y + k * pitch - 3, BAR_NAME[k], C_BONE, 255, AL_LEFT);
    stat_bar(x + 56, y + k * pitch, w, s, k);
  }
}

/* the option shown in a row: the real one, or the slot machine's cycling pick */
static int row_shown_option(const seat_t *s, int row, int *cycling) {
  const build_t *b = &s->build;
  int cur = build_get(b, row);
  *cycling = 0;
  if (s->slot_machine_t > 0) {
    int elapsed = J_SLOT_MACHINE_FRAMES - s->slot_machine_t;
    int settled_at = (row + 1) * 6;
    if (elapsed < settled_at && row != ROW_PAINT) {
      *cycling = 1;
      int n = row_count(b, row);
      return n > 0 ? (elapsed * 7 + row * 3 + (int)G.frame) % n : cur;
    }
  }
  return cur;
}

static int row_visible_count(const build_t *b) { return build_mount_ok(b) ? ROW_COUNT : ROW_COUNT - 1; }

/* the garage header for an AI seat: the practice bot label and the EASY MEDIUM HARD toggle */
static void ai_toggle(int x, int y, int level) {
  int cx = x;
  for (int k = 0; k < 3; k++) {
    int w = hud_text_w(F_MONO_S, VAL_DIFFICULTY[k]) + 12;
    if (k == level) { hud_rect(cx, y, w, 20, C_HAZARD, 255); hud_text(F_MONO_S, cx + 6, y + 2, VAL_DIFFICULTY[k], C_TAPE, 255, AL_LEFT); }
    else hud_text(F_MONO_S, cx + 6, y + 2, VAL_DIFFICULTY[k], C_ASH, 255, AL_LEFT);
    cx += w + 4;
  }
}

/* the flavour slot: over budget, an illegal rule, the slot machine blurb, or the part's flavour */
static const char *flavour_line(const seat_t *s, char *buf, int n, uint32_t *ink) {
  const build_t *b = &s->build;
  int players = players_now();
  int over = build_cost(b) - build_budget(b);
  const char *reason = 0;
  *ink = C_BONE;
  if (over > 0) { snprintf(buf, (size_t)n, S_OVER_BUDGET, over); *ink = C_RUST; return buf; }
  if (!build_rules(b, players, &reason) && reason) { *ink = C_SMOKE; return reason; }
  if (s->slot_machine_t > 0) return PRESETS[PRE_RANDOM].blurb;
  int row = clampi(s->row, 0, ROW_COUNT - 1);
  if (row == ROW_PAINT) { int p = build_preset_of(b); return p == PRE_CUSTOM ? "" : PRESETS[p].blurb; }
  if (row == ROW_WEAPON && b->drive == DR_MELTY) return DRIVES[DR_MELTY].flavour;
  return option_flavour(row, build_get(b, row));
}

/* the value shown in a row: the PAINT row names its scheme and decal set (3.7), the others their part */
static const char *row_value(const seat_t *s, int row, int opt, int short_form, char *buf, int n) {
  if (row != ROW_PAINT) return option_name(&s->build, row, opt, short_form);
  snprintf(buf, (size_t)n, S_PAINT_ROW, s->build.paint + 1, DECAL_NAME[clampi(s->build.decal, 0, DECAL_COUNT - 1)]);
  return buf;
}

void garage_panel(const rect_t *r, int seat, int compact, float dt) {
  const seat_t *s = &G.seats[seat];
  if (s->kind == SEAT_EMPTY) return;
  int players = players_now();
  uint32_t sc = seat_colour(seat);
  int ai = s->kind == SEAT_AI;
  /* slide in from the panel's screen side */
  int x = r->x, y = r->y, w = r->w, h = r->h;
  if (s->join_slide > 0.001f) x += (int)((r->x + r->w / 2 < 640 ? -1 : 1) * s->join_slide * r->w);
  /* the over budget shake timer */
  int cost = build_cost(&s->build), budget = build_budget(&s->build);
  if (cost != garage_last_cost[seat]) { if (cost > budget) garage_shake_t[seat] = 0.3f; garage_last_cost[seat] = cost; }
  if (s->deny_frame != garage_last_deny[seat]) { if (s->deny_frame) garage_shake_t[seat] = 0.3f; garage_last_deny[seat] = s->deny_frame; }
  if (garage_shake_t[seat] > 0) garage_shake_t[seat] -= dt;
  /* background */
  hud_rect(x, y, w, h, C_CHAR, 255);
  rect_t hr; rect_t pr = { x, y, w, h }; garage_hole(compact, &pr, &hr);
  hud_frame(hr.x - 2, hr.y - 2, hr.w + 4, hr.h + 4, 2, C_STEEL, 255);
  hole(hr.x, hr.y, hr.w, hr.h);
  char name[24]; bot_name(name, &s->build);
  char buf[64], flbuf[64];
  int rows = row_visible_count(&s->build);
  int row = clampi(s->row, 0, rows - 1);
  uint32_t fl_ink; const char *fl = flavour_line(s, flbuf, sizeof flbuf, &fl_ink);
  if (!compact) {
    /* header: chip, name, preset label; the hint line; the tag bar */
    hud_seat_chip(x + 16, y + 6, 28, 28, seat, F_H3, ai);
    if (ai) {
      hud_text(F_H3, x + 52, y + 6, S_PRACTICE_BOT, C_BONE, 255, AL_LEFT);
      ai_toggle(x + 624 - 3 * 66, y + 10, clampi(s->ai_level, 0, 2));
      snprintf(buf, sizeof buf, "%s  %s", name, preset_label(&s->build));
      hud_text(F_MONO_S, x + 52, y + 40, buf, C_ASH, 255, AL_LEFT);
    } else {
      hud_text(F_H3, x + 52, y + 6, name, C_BONE, 255, AL_LEFT);
      hud_text(F_MONO_S, x + 624, y + 12, preset_label(&s->build), C_ASH, 255, AL_RIGHT);
      if (s->hint[0]) hud_text(F_MONO_S, x + 52, y + 40, s->hint, C_BONE, 255, AL_LEFT);
    }
    int bw = tag_bar(x + 16, y + 72, &s->build, 0, garage_shake_t[seat]);
    snprintf(buf, sizeof buf, S_TAGS_SPENT, cost, clampi(budget - cost, 0, 99));
    hud_text(F_MONO_S, x + 16 + bw + 12, y + 70, buf, cost > budget ? C_RUST : C_BONE, 255, AL_LEFT);
    /* rows */
    if (s->locked) hud_set_alpha(180);
    for (int k = 0; k < rows; k++) {
      int ry = y + 350 + 34 * k;
      int on = k == row && !s->locked;
      int cyc; int opt = row_shown_option(s, k, &cyc);
      const char *reason = 0;
      int legal = cyc ? 1 : option_legal(&s->build, k, opt, players, &reason);
      if (k == ROW_WEAPON && s->build.drive == DR_MELTY) legal = 1;
      uint32_t ink = on ? C_BONE : C_ASH;
      uint32_t vink = legal ? ink : C_SMOKE;
      if (on) hud_rect(x + 16, ry + 2, 4, 30, sc, 255);
      hud_text(F_H3, x + 32, ry + 4, ROW_NAME[k], ink, 255, AL_LEFT);
      char pv[24];
      const char *val = row_value(s, k, opt, 0, pv, sizeof pv);
      int n = row_count(&s->build, k);
      hud_chevron(x + 250, ry + 17, -1, 0, 14, 3, on && n > 1 ? C_HAZARD : C_STEEL, 255);
      hud_text(F_MONO_L, x + 390, ry + 1, val, vink, 255, AL_CENTER);
      hud_chevron(x + 530, ry + 17, 1, 0, 14, 3, on && n > 1 ? C_HAZARD : C_STEEL, 255);
      int tags = option_tags(k, opt);
      if (k != ROW_PAINT && (k != ROW_WEAPON || s->build.drive != DR_MELTY)) {
        snprintf(buf, sizeof buf, "%d", tags);
        hud_text(F_MONO_L, x + 608, ry + 1, buf, cost > budget && on ? C_RUST : C_HAZARD, 255, AL_RIGHT);
      }
    }
    if (s->locked) hud_set_alpha(255);
    /* flavour, bars, hints */
    int fy = rows > 5 ? y + 556 : y + 540;
    hud_text(F_MONO, x + 32, fy, fl, fl_ink, 255, AL_LEFT);
    bars_block(x + 32, rows > 5 ? y + 588 : y + 580, 140, 26, s);
    int hy = y + 672;
    if (ai) hud_text(F_MONO_S, x + 16, hy + 4, S_AI_TOGGLE_HINT, C_ASH, 255, AL_LEFT);   /* how the table sets EASY MEDIUM HARD */
    if (s->locked) {
      if (!ai) key_hint(x + 16, hy, "B", S_RELEASE, C_ASH);
      hud_text(F_MONO_S, x + 624, hy + 4, S_LOCKED, C_LIVE, 255, AL_RIGHT);
    } else if (!ai) {
      int kx = x + 16;
      kx += key_hint(kx, hy, "A", S_LOCK_IN, C_ASH) + 16;
      kx += key_hint(kx, hy, "START", S_START_FIGHT, C_ASH) + 16;
      kx += key_hint(kx, hy, "X", S_RANDOM_BOT, C_ASH) + 16;
      int yx = kx;
      key_hint(kx, hy, "Y", S_PRESET_HOLD_CONTROLS, C_ASH);
      if (s->y_hold > 0) hud_ring(yx + 12, hy + 12, 18, 3, clampf(s->y_hold / 0.4f, 0, 1), sc, 255);
    }
  } else {
    /* quadrant: header 24 px with the chip and name, the tag bar from x 300 */
    hud_seat_chip(x + 8, y + 4, 22, 22, seat, F_MONO_S, ai);
    if (ai) {
      hud_text(F_MONO_L, x + 36, y - 2, S_PRACTICE_BOT, C_BONE, 255, AL_LEFT);
      ai_toggle(x + 300, y + 4, clampi(s->ai_level, 0, 2));
    } else {
      hud_text(F_MONO_L, x + 36, y - 2, name, C_BONE, 255, AL_LEFT);
      tag_bar(x + 300, y + 8, &s->build, 1, garage_shake_t[seat]);
    }
    if (s->locked) hud_set_alpha(180);
    for (int k = 0; k < rows; k++) {
      int ry = y + 36 + 30 * k;
      int on = k == row && !s->locked;
      int cyc; int opt = row_shown_option(s, k, &cyc);
      const char *reason = 0;
      int legal = cyc ? 1 : option_legal(&s->build, k, opt, players, &reason);
      if (k == ROW_WEAPON && s->build.drive == DR_MELTY) legal = 1;
      uint32_t ink = on ? C_BONE : C_ASH;
      uint32_t vink = legal ? ink : C_SMOKE;
      if (on) hud_rect(x + 322, ry + 4, 3, 22, sc, 255);
      hud_text(F_MONO_S, x + 330, ry + 8, k == ROW_WEAPON2 ? S_ROW_2ND_SHORT : ROW_NAME[k], ink, 255, AL_LEFT);
      char pv[24];
      const char *val = row_value(s, k, opt, 1, pv, sizeof pv);
      int n = row_count(&s->build, k);
      hud_chevron(x + 408, ry + 15, -1, 0, 12, 3, on && n > 1 ? C_HAZARD : C_STEEL, 255);
      hud_text(F_MONO_L, x + 508, ry - 1, val, vink, 255, AL_CENTER);
      hud_chevron(x + 606, ry + 15, 1, 0, 12, 3, on && n > 1 ? C_HAZARD : C_STEEL, 255);
      int tags = option_tags(k, opt);
      if (k != ROW_PAINT && (k != ROW_WEAPON || s->build.drive != DR_MELTY)) {
        snprintf(buf, sizeof buf, "%d", tags);
        hud_text(F_MONO_S, x + 634, ry + 8, buf, cost > budget && on ? C_RUST : C_HAZARD, 255, AL_RIGHT);
      }
    }
    if (s->locked) hud_set_alpha(255);
    int by = rows > 5 ? y + 222 : y + 196;
    for (int k = 0; k < BAR_COUNT; k++) {
      hud_text(F_MONO_S, x + 330, by + k * 20 - 3, BAR_NAME[k], C_BONE, 255, AL_LEFT);
      stat_bar(x + 384, by + k * 20, 120, s, k);
    }
    int fy = rows > 5 ? y + 282 : y + 262;
    /* the hint line replaces the flavour line for the first 5 s of a rebuild */
    if (s->hint[0] && G.phase_t < 5 && !ai) hud_text_wrap(F_MONO_S, x + 330, fy, 300, s->hint, C_BONE, 255, 17);
    else hud_text_wrap(F_MONO_S, x + 330, fy, 300, fl, fl_ink, 255, 17);
    int hy = y + 306;
    if (ai) hud_text(F_MONO_S, x + 16, hy + 4, S_AI_TOGGLE_HINT, C_ASH, 255, AL_LEFT);
    if (s->locked) {
      if (!ai) key_hint(x + 16, hy, "B", S_RELEASE, C_ASH);
      hud_text(F_MONO_S, x + 624, hy + 4, S_LOCKED, C_LIVE, 255, AL_RIGHT);
    } else if (!ai) {
      int kx = x + 16;
      kx += key_hint(kx, hy, "A", S_LOCK, C_ASH) + 12;
      kx += key_hint(kx, hy, "START", S_START_FIGHT, C_ASH) + 12;
      kx += key_hint(kx, hy, "X", S_RANDOM, C_ASH) + 12;
      int yx = kx;
      key_hint(kx, hy, "Y", S_PRESET, C_ASH);
      if (s->y_hold > 0) hud_ring(yx + 12, hy + 12, 18, 3, clampf(s->y_hold / 0.4f, 0, 1), sc, 255);
    }
  }
  if (s->locked) hud_frame(x, y, w, h, 3, C_LIVE, 255);
  hud_dirty(x, y, w, h);
}

/* the 3 player match card in the fourth quadrant (10.11) */
void garage_match_card(const rect_t *r) { garage_match_card_dt(r, 1.0f / 60); }

void garage_match_card_dt(const rect_t *r, float dt) {
  int x = r->x, y = r->y;
  hud_rect(x, y, r->w, r->h, C_CHAR, 255);
  hud_eyebrow(x + 40, y + 12, S_NEXT_FIGHT, C_ASH, AL_LEFT);
  char buf[48];
  int players = players_now();
  hud_text(F_H3, x + 40, y + 50, MODE_NAME[clampi(G.mode, 0, MODE_COUNT - 1)], C_BONE, 255, AL_LEFT);
  snprintf(buf, sizeof buf, S_N_BOTS, players);
  hud_text(F_MONO_L, x + 600, y + 48, buf, C_ASH, 255, AL_RIGHT);
  if (G.mode == MODE_TOURNAMENT) {
    rect_t br = { x + 40, y + 96, 560, 160 };
    bracket_card(&br);
  } else {
    /* three steppers, plus PRACTICE BOT when an AI seat sits at the table; the focused row is game_flow.c's
     * (any unseated pad's stick, or any seat's while holding Y) */
    const char *lab[4] = { S_FIGHT_LENGTH, S_ONE_CAMERA, S_COMEBACK, S_PRACTICE_ROW };
    int ai = -1; for (int i = 0; i < 4; i++) if (G.seats[i].kind == SEAT_AI) { ai = i; break; }
    const char *val[4] = { VAL_FIGHT_LEN[clampi(SAVE.fight_len, 0, 3)], VAL_ONE_CAMERA[clampi(SAVE.one_camera, 0, 2)], VAL_AUTO_ON_OFF[clampi(SAVE.comeback, 0, 2)],
                           VAL_DIFFICULTY[ai >= 0 ? clampi(G.seats[ai].ai_level, 0, 2) : 1] };
    int idx[4] = { SAVE.fight_len, SAVE.one_camera, SAVE.comeback, ai >= 0 ? G.seats[ai].ai_level : 0 }, maxi[4] = { 3, 2, 2, 2 };
    int rows = ai >= 0 ? 4 : 3, pitch = ai >= 0 ? 30 : 36;
    int row = clampi(game_match_card_row(), 0, rows - 1);
    float d = focus_block(&match_cursor_y, y + 96 + pitch * row, x + 40, 560, dt);
    for (int k = 0; k < rows; k++) {
      if (k == row) focus_row_text(x + 40, y + 96 + pitch * k, 560, lab[k], val[k], d < 8, 1, idx[k] > 0, idx[k] < maxi[k]);
      else menu_row(x + 40, y + 96 + pitch * k, 560, lab[k], val[k], 0, 1, 1, idx[k] > 0, idx[k] < maxi[k]);
    }
    /* the evening tally with pips */
    int tx = x + 40, ty = y + 222;
    hud_text(F_MONO_S, tx, ty, S_EVENING_LABEL, C_ASH, 255, AL_LEFT);
    tx += 80;
    for (int i = 0; i < 4; i++) {
      if (G.seats[i].kind == SEAT_EMPTY) continue;
      hud_seat_chip(tx, ty - 2, 18, 18, i, F_MONO_S, G.seats[i].kind == SEAT_AI);
      tx += 24;
      for (int k = 0; k < G.seats[i].wins && k < 8; k++) { hud_rect(tx, ty + 2, 10, 10, C_BONE, 255); tx += 14; }
      tx += 18;
    }
  }
  hud_text(F_MONO_S, x + 40, y + 260, S_GARAGE_HINT_1, C_ASH, 255, AL_LEFT);
  hud_text(F_MONO_S, x + 40, y + 280, S_GARAGE_HINT_2, C_ASH, 255, AL_LEFT);
  hud_dirty(x, y, r->w, r->h);
}

void garage_timer_tape(int frames_left, int total) { timer_tape(frames_left, total, G.phase_t); }

void garage_screen(float dt, int frames_left, int total_frames) {
  int seated = seated_count();
  int order[4], n = 0;
  for (int i = 0; i < 4; i++) if (G.seats[i].kind != SEAT_EMPTY) order[n++] = i;
  if (n == 0) { hud_rect(0, 0, 1280, 720, C_CHAR, 255); garage_timer_tape(frames_left, total_frames); return; }
  if (seated <= 2) {
    rect_t halves[2] = { { 0, 0, 640, 720 }, { 640, 0, 640, 720 } };
    for (int k = 0; k < n; k++) garage_panel(&halves[k], order[k], 0, dt);
    if (n == 1) hud_rect(640, 0, 640, 720, C_CHAR, 255);
    hud_tape(638, 0, 4, 720, G.tape_phase);
  } else {
    rect_t quads[4] = { { 0, 0, 640, 360 }, { 640, 0, 640, 360 }, { 0, 360, 640, 360 }, { 640, 360, 640, 360 } };
    int used[4] = { 0, 0, 0, 0 };
    for (int k = 0; k < n; k++) { garage_panel(&quads[order[k]], order[k], 1, dt); used[order[k]] = 1; }
    for (int q = 0; q < 4; q++) if (!used[q]) { garage_match_card_dt(&quads[q], dt); break; }
    hud_tape(638, 0, 4, 720, G.tape_phase);
    hud_tape(0, 358, 1280, 4, G.tape_phase);
  }
  garage_timer_tape(frames_left, total_frames);
  /* the controls card over a seat's own panel while Y is held, or for 3 s after a first join */
  if (G.controls_card_seat >= 0 && G.controls_card_seat < 4 && G.seats[G.controls_card_seat].kind != SEAT_EMPTY) {
    int s = G.controls_card_seat;
    if (seated <= 2) {
      int k = 0; for (int i = 0; i < n; i++) if (order[i] == s) k = i;
      rect_t cr = { k * 640, 180, 640, 360 };
      controls_card(&cr, 0, s);
    } else {
      rect_t cr = { (s & 1) * 640 + 160, (s >> 1) * 360 + 90, 320, 180 };
      controls_card(&cr, 1, s);
    }
  }
  hud_dirty(0, 0, 1280, 720);
}


/* -- arena vote (10.12)  -- */
/* paint one arena's floor thumbnail from its arena_def_t with hud primitives:
 * the floor by style, the tile list, hazards, props, the OPS zone, open edges and spawns */
static void thumb_paint(const arena_def_t *a, int tx, int ty, int tw, int th) {
  hud_rect(tx, ty, tw, th, C_GRIME, 255);
  float s = fminf((float)(tw - 8) / a->w, (float)(th - 8) / a->d);
  int fw = (int)(a->w * s), fh = (int)(a->d * s);
  int fx = tx + (tw - fw) / 2, fy = ty + (th - fh) / 2;
  #define PX(u) (fx + (int)(((u) + a->w * 0.5f) * s + 0.5f))
  #define PZ(u) (fy + (int)(((u) + a->d * 0.5f) * s + 0.5f))
  #define PW(u) ((int)((u) * s + 0.5f) < 1 ? 1 : (int)((u) * s + 0.5f))
  uint32_t base = 0xd9d3c7, grout = 0x9c968a;
  int grid = 1;
  switch (a->floor_style) {
    case FL_DARK: base = 0x2b2622; grout = 0x201c19; break;
    case FL_CONCRETE: base = 0x8a847a; grout = 0x7a746a; grid = 0; break;
    case FL_WET: base = 0x4f5a60; grout = 0x44505a; grid = 0; break;
    case FL_SHOP: base = 0xcfc7b8; grout = 0x968f83; break;
    case FL_ROOF: base = 0x6e6862; grout = 0x5a554f; grid = 0; break;
    case FL_GARAGE: base = 0x3a342b; grout = 0x2f2920; break;
    default: break;
  }
  hud_rect(fx, fy, fw, fh, base, 255);
  hud_clip(fx, fy, fw, fh);
  if (grid) {
    for (float u = 0.6f; u < a->w; u += 0.6f) hud_rect(fx + (int)(u * s), fy, 1, fh, grout, 160);
    for (float u = 0.6f; u < a->d; u += 0.6f) hud_rect(fx, fy + (int)(u * s), fw, 1, grout, 160);
  }
  if (a->floor_style == FL_ROOF) {
    for (float u = 3; u < a->w; u += 3) hud_rect(fx + (int)(u * s), fy, 1, fh, grout, 255);
    for (float u = 3; u < a->d; u += 3) hud_rect(fx, fy + (int)(u * s), fw, 1, grout, 255);
    hud_rect(PX(-3), PZ(-3), PW(6), PW(6), 0xd9672b, 255);
  }
  if (a->floor_style == FL_WET) {
    for (int k = 0; k < 14; k++) hud_rect(fx + (k * 37) % fw, fy + (k * 53) % fh, 3, 1, 0x7a8a92, 255);
  }
  /* tiles */
  if (a->tiles && a->tiles_w > 0 && a->tiles_h > 0) {
    float tsx = (float)fw / a->tiles_w, tsz = (float)fh / a->tiles_h;
    for (int i = 0; i < a->ntiles; i++) {
      const tile_rect_t *t = &a->tiles[i];
      int x0 = fx + (int)(t->x * tsx), y0 = fy + (int)(t->y * tsz);
      int x1 = fx + (int)((t->x + t->w) * tsx), y1 = fy + (int)((t->y + t->h) * tsz);
      hud_rect(x0, y0, x1 - x0 < 1 ? 1 : x1 - x0, y1 - y0 < 1 ? 1 : y1 - y0, t->colour ? 0xb23a2e : 0xd9672b, 255);
    }
  }
  /* props */
  for (int i = 0; i < a->nprops; i++) {
    const prop_def_t *p = &a->props[i];
    uint32_t c = C_STEEL;
    switch (p->kind) {
      case PR_BENCH: c = 0x8b6b45; break; case PR_HVAC: c = 0x9a9388; break; case PR_BOX: c = 0xa8824e; break;
      case PR_SHELF: c = C_STEEL_HI; break; case PR_SIGN: c = C_HAZARD; break; case PR_SERVER: c = 0x4a4338; break;
      default: break;
    }
    hud_rect(PX(p->x - p->w * 0.5f), PZ(p->z - p->d * 0.5f), PW(p->w), PW(p->d), c, 255);
  }
  /* hazards */
  for (int i = 0; i < a->nhaz; i++) {
    const hazard_def_t *h = &a->haz[i];
    int hx = PX(h->x - h->w * 0.5f), hz = PZ(h->z - h->d * 0.5f), hw = PW(h->w), hd = PW(h->d);
    switch (h->type) {
      case HZ_PIT: hud_rect(hx, hz, hw, hd, C_CHAR, 255); hud_frame(hx, hz, hw, hd, 2, C_HAZARD, 255); break;
      case HZ_PULVERIZER: hud_rect(hx, hz, hw, hd, C_RUST, 255); hud_rect(hx + hw / 2 - 1, hz + hd / 2 - 1, 3, 3, C_BONE, 255); break;
      case HZ_KILLSAW: hud_rect(hx - 2, hz - 2, hw + 4, hd + 4, C_HAZARD, 255); hud_rect(hx, hz, hw, hd, C_STEEL_HI, 255); break;
      case HZ_FLAME: hud_rect(hx - 2, hz - 2, hw + 4, hd + 4, C_HAZARD, 255); hud_rect(hx, hz, hw, hd, 0x6a4a2a, 255); break;
      case HZ_SCREW: hud_rect(hx, hz, hw, hd, C_STEEL_HI, 255); for (int k = 2; k < hw; k += 4) hud_rect(hx + k, hz, 1, hd, C_STEEL, 255); break;
      case HZ_RAMP: hud_rect(hx, hz, hw, hd, 0xb8b0a2, 255); hud_frame(hx, hz, hw, hd, 1, 0x9c968a, 255); break;
      case HZ_BUMPER: hud_circle(PX(h->x), PZ(h->z), PW(0.4f) < 2 ? 2 : PW(0.4f), C_RUST, 255); break;
      case HZ_DECK: hud_rect(hx, hz, hw, hd, 0x5a4a3a, 255); hud_rect(hx, hz + hd - 2, hw, 2, 0x3a2e22, 255); break;
      case HZ_FLOORFLIP: hud_rect(hx - 2, hz - 2, hw + 4, hd + 4, C_HAZARD, 255); hud_rect(hx, hz, hw, hd, C_STEEL_HI, 255); hud_rect(hx + hw / 2, hz, 1, hd, C_LIVE, 255); break;
      case HZ_DROPZONE: hud_frame(hx, hz, hw, hd, 1, C_STEEL_HI, 255); hud_line(hx, hz, hx + hw, hz + hd, 1, C_STEEL_HI, 255); hud_line(hx + hw, hz, hx, hz + hd, 1, C_STEEL_HI, 255); break;
      case HZ_LASER: hud_rect(fx, PZ(h->z), fw, 1, C_LIVE, 200); break;
      case HZ_MOUND: hud_rect(hx, hz, hw, hd, 0x6b4a2e, 255); hud_rect(hx + hw / 4, hz + hd / 4, hw / 2, hd / 2, 0x7d5a3a, 255); break;
      default: break;
    }
  }
  /* the OPS zone: 2 x 2 of tape */
  mini_tape(PX(a->ops_zone[0] - 1), PZ(a->ops_zone[1] - 1), PW(2), PW(2));
  /* spawn corners at 40 percent */
  int players = players_now();
  if (players <= 2) { for (int k = 0; k < 2; k++) hud_rect(PX(a->spawn2[k][0] - 1), PZ(a->spawn2[k][1] - 1), PW(2), PW(2), seat_colour(k), 100); }
  else { for (int k = 0; k < 4 && k < players; k++) hud_rect(PX(a->spawn4[k][0] - 1), PZ(a->spawn4[k][1] - 1), PW(2), PW(2), seat_colour(k), 100); }
  hud_clip_clear();
  /* walls, and tape on the open edges */
  if (a->open_mask & OPEN_NZ) mini_tape(fx, fy - 3, fw, 4); else hud_rect(fx, fy - 2, fw, 2, C_STEEL_HI, 255);
  if (a->open_mask & OPEN_PZ) mini_tape(fx, fy + fh - 1, fw, 4); else hud_rect(fx, fy + fh, fw, 2, C_STEEL_HI, 255);
  if (a->open_mask & OPEN_NX) mini_tape(fx - 3, fy, 4, fh); else hud_rect(fx - 2, fy, 2, fh, C_STEEL_HI, 255);
  if (a->open_mask & OPEN_PX) mini_tape(fx + fw - 1, fy, 4, fh); else hud_rect(fx + fw, fy, 2, fh, C_STEEL_HI, 255);
  #undef PX
  #undef PZ
  #undef PW
}

/* the hazard glyph row of a vote card: one glyph per hazard type present, no words */
static void hazard_glyphs(const arena_def_t *a, int x, int y) {
  static const int glyph_of[HZ_TYPE_COUNT] = { -1, GL_PIT, GL_HAMMER, GL_SAW, GL_FLAME, GL_SCREW, GL_RAMP, GL_BUMPER, GL_DECK, GL_FLIP_PANEL, GL_DROP, GL_BEAM, GL_WIND, GL_CROWN, GL_RAMP };
  int seen[HZ_TYPE_COUNT] = { 0 };
  int n = 0;
  for (int i = 0; i < a->nhaz && n < 8; i++) {
    int t = a->haz[i].type;
    if (t <= 0 || t >= HZ_TYPE_COUNT || seen[t] || glyph_of[t] < 0 || t == HZ_HILL) continue;
    seen[t] = 1;
    hud_glyph(x + n * 22, y, glyph_of[t], 1, C_ASH, 255);
    n++;
  }
  if (a->open_mask && !seen[HZ_WIND]) hud_glyph(x + n * 22, y, GL_WIND, 1, C_ASH, 255);
}

void vote_screen(float dt) {
  float t = G.phase_t;
  hud_rect(0, 0, 1280, 720, C_CHAR, 255);
  hud_eyebrow(32, 28, S_PICK_AN_ARENA, C_ASH, AL_LEFT);
  char buf[64];
  snprintf(buf, sizeof buf, "%s  %s", MODE_NAME[clampi(G.mode, 0, MODE_COUNT - 1)], VAL_FIGHT_LEN[clampi(SAVE.fight_len, 0, 3)]);
  hud_text(F_MONO, 420, 24, buf, C_ASH, 255, AL_LEFT);
  int oc = clampi(G.vote_one_camera, 0, 2);
  hud_text(F_MONO_S, 880, 30, S_ONE_CAMERA, C_ASH, 255, AL_LEFT);
  stepper(1060, 22, F_MONO, VAL_ONE_CAMERA[oc], C_BONE, oc > 0, oc < 2, 0);
  int frames_left = G.vote_left > 0 || G.vote_decided ? G.vote_left : VOTE_FRAMES - G.phase_frames;
  if (frames_left < 0) frames_left = 0;
  int secs = (frames_left + 59) / 60;
  snprintf(buf, sizeof buf, "0:%02d", secs);
  hud_text(F_H2, 1248, 16, buf, C_HAZARD, 255, AL_RIGHT);
  /* the cards */
  int decided = G.vote_decided, leader = clampi(G.vote_leader, 0, ARENA_COUNT - 1);
  int tie_flicker = decided && G.vote_decided_t < 0.5f && G.vote_decided_t >= 0;
  for (int i = 0; i < ARENA_COUNT; i++) {
    int x = 32 + 416 * (i % 3), y = 72 + 304 * (i / 3), w = 384, h = 232;
    int lead = i == leader;
    int shadow = lead ? 10 : 6;
    int dim = decided && !lead;
    if (tie_flicker) { lead = ((int)(t * 20) + i) % 3 == 0; dim = !lead; shadow = lead ? 10 : 6; }
    hud_rect(x + shadow, y + shadow, w, h, C_SHADOW, 255);
    hud_rect(x, y, w, h, C_SLAB, 255);
    hud_frame(x, y, w, h, 2, lead ? C_HAZARD : C_STEEL_HI, 255);
    hud_text(F_H3, x + 16, y + 8, ARENA_NAME[i], C_BONE, 255, AL_LEFT);
    thumb_paint(&VOTE_ARENAS[i], x + 16, y + 40, 352, 120);
    hud_text(F_MONO_S, x + 16, y + 172, ARENA_BLURB[i], C_ASH, 255, AL_LEFT);
    hazard_glyphs(&VOTE_ARENAS[i], x + 16, y + 200);
    /* voter chips at the bottom right; a human who has pressed A gets a live frame round its chip */
    int cx = x + w - 16 - 24;
    for (int s = 0; s < 4; s++) if (G.seats[s].kind != SEAT_EMPTY && G.seats[s].vote == i) {
      hud_seat_chip(cx, y + h - 16 - 24, 24, 24, s, F_MONO_S, G.seats[s].kind == SEAT_AI);
      if (G.seats[s].kind == SEAT_HUMAN && G.seats[s].voted) hud_frame(cx - 3, y + h - 16 - 27, 30, 30, 2, C_LIVE, 255);
      cx -= 28;
    }
    if (dim) hud_rect(x, y, w, h, C_CHAR, 140);
  }
  /* each seat's 3 px frame tweens between cards; a fresh vote starts its frames on the cards, not where the
   * last vote left them */
  if (!vote_init || G.phase_frames <= 1) { for (int s = 0; s < 4; s++) { vote_fx[s] = -1; } vote_init = 1; }
  for (int s = 0; s < 4; s++) {
    const seat_t *st = &G.seats[s];
    if (st->kind != SEAT_HUMAN || st->vote < 0 || st->vote >= ARENA_COUNT) { vote_fx[s] = -1; continue; }
    float tx = (float)(32 + 416 * (st->vote % 3)) - 4 - s * 3, ty = (float)(72 + 304 * (st->vote / 3)) - 4 - s * 3;
    if (vote_fx[s] < 0) { vote_fx[s] = tx; vote_fy[s] = ty; }
    hud_tween(&vote_fx[s], tx, 20, dt); hud_tween(&vote_fy[s], ty, 20, dt);
    hud_frame((int)vote_fx[s], (int)vote_fy[s], 384 + 8 + s * 6, 232 + 8 + s * 6, 3, seat_colour(s), 255);
  }
  hud_text(F_MONO, 32, 650, S_VOTE_RULE, C_ASH, 255, AL_LEFT);
  /* the bottom tape with the seconds */
  hud_rect(0, 696, 1280, 24, C_CHAR, 255);
  int tw = (int)(1280 * clampf((float)frames_left / VOTE_FRAMES, 0, 1));
  if (tw > 4) hud_tape(640 - tw / 2, 696, tw, 24, G.tape_phase);
  snprintf(buf, sizeof buf, "%d S", secs);
  hud_rect(604, 696, 72, 24, C_CHAR, 255);
  hud_frame(604, 696, 72, 24, 2, C_STEEL_HI, 255);
  hud_text(F_MONO, 640, text_cy(F_MONO, 696, 24), buf, C_BONE, 255, AL_CENTER);
  hud_dirty(0, 0, 1280, 720);
}

/* -- countdown (10.13)  -- */
void countdown_overlay(const rect_t *vp, int n, float t) {
  int cx = vp->x + vp->w / 2, cy = vp->y + vp->h / 2;
  if (n > 0) {
    char d[2] = { (char)('0' + clampi(n, 0, 9)), 0 };
    int fh = hud_font_h(F_TITLE);
    hud_text_sh(F_TITLE, cx, cy - fh / 2 - fh / 8, d, C_BONE, 255, AL_CENTER);
    /* the 0.15 s pop: a ring that opens and fades */
    if (t < 0.15f) {
      float f = t / 0.15f;
      hud_ring(cx, cy, 70 + (int)(50 * f), 4, 1.0f, C_BONE, (int)(200 * (1 - f)));
    }
  } else {
    /* FIGHT in F_STINGER on a hazard bar across the viewport at 35 percent of its height */
    int bh = vp->h >= 500 ? 72 : 48;
    int by = vp->y + (int)(vp->h * 0.35f) - bh / 2;
    hud_rect(vp->x + 6, by + 6, vp->w, bh, C_SHADOW, 255);
    hud_rect(vp->x, by, vp->w, bh, C_HAZARD, 255);
    int font = vp->h >= 500 ? F_STINGER : F_STINGER_Q;
    hud_text(font, cx, text_cy(font, by, bh), ST_FIGHT[0], C_TAPE, 255, AL_CENTER);
  }
  hud_dirty(vp->x, vp->y, vp->w, vp->h);
}

/* the 9.1 viewport rects for a player count (the same table as cam_layout) */
static int menu_layout(int players, rect_t out[4]) {
  if (players <= 1) { out[0] = (rect_t){ 0, 0, 1280, 720 }; return 1; }
  if (players == 2) { out[0] = (rect_t){ 0, 0, 640, 720 }; out[1] = (rect_t){ 640, 0, 640, 720 }; return 2; }
  out[0] = (rect_t){ 0, 0, 640, 360 }; out[1] = (rect_t){ 640, 0, 640, 360 }; out[2] = (rect_t){ 0, 360, 640, 360 }; out[3] = (rect_t){ 640, 360, 640, 360 };
  return players == 3 ? 3 : 4;
}

void countdown_screen(float dt) {
  (void)dt;
  rect_t vp[4];
  int n = menu_layout(players_now(), vp);
  if (G.one_camera_eff) { vp[0] = (rect_t){ 0, 0, 1280, 720 }; n = 1; }
  float t = G.phase_t - floorf(G.phase_t);
  for (int i = 0; i < n; i++) countdown_overlay(&vp[i], G.countdown_n, t);
}

/* -- settings (10.22)  -- */
/* the row's value string from the save; STICKS lists one stepper per seated pad */
static const char *setting_value(int row, char *buf, int n, int *idx, int *max) {
  *idx = 0; *max = 0;
  switch (row) {
    case 0: *idx = SAVE.split; *max = 2; return VAL_SPLIT[clampi(SAVE.split, 0, 2)];
    case 1: *idx = SAVE.one_camera; *max = 2; return VAL_ONE_CAMERA[clampi(SAVE.one_camera, 0, 2)];
    case 2: *idx = SAVE.fight_len; *max = 3; return VAL_FIGHT_LEN[clampi(SAVE.fight_len, 0, 3)];
    case 3: *idx = SAVE.comeback; *max = 2; return VAL_AUTO_ON_OFF[clampi(SAVE.comeback, 0, 2)];
    case 4: *idx = SAVE.shake; *max = 3; snprintf(buf, (size_t)n, "%d", clampi(SAVE.shake, 0, 3)); return buf;
    case 5: *idx = SAVE.hitstop ? 0 : 1; *max = 1; return VAL_ON_OFF[SAVE.hitstop ? 0 : 1];
    case 6: *idx = SAVE.stinger_big ? 0 : 1; *max = 1; return VAL_STINGER[SAVE.stinger_big ? 0 : 1];
    case 7: *idx = SAVE.sfx; *max = 10; snprintf(buf, (size_t)n, "%d", clampi(SAVE.sfx, 0, 10)); return buf;
    case 8: *idx = SAVE.music; *max = 10; snprintf(buf, (size_t)n, "%d", clampi(SAVE.music, 0, 10)); return buf;
    case 9: *idx = SAVE.colourblind ? 1 : 0; *max = 1; return VAL_OFF_ON[SAVE.colourblind ? 1 : 0];
    case 10: *idx = SAVE.rumble ? 0 : 1; *max = 1; return VAL_ON_OFF[SAVE.rumble ? 0 : 1];
    case 11: {
      buf[0] = 0;
      int any = 0;
      for (int i = 0; i < 4; i++) {
        int seated = G.seats[i].kind == SEAT_HUMAN || PADS[i].seat >= 0;
        if (!seated) continue;
        char one[16]; snprintf(one, sizeof one, "%s%d %s", any ? "  " : "", i + 1, SAVE.scheme[i] == SCHEME_TANK ? S_TANK : S_ARCADE);
        strncat(buf, one, (size_t)n - strlen(buf) - 1);
        any = 1;
      }
      if (!any) snprintf(buf, (size_t)n, "1 %s", SAVE.scheme[0] == SCHEME_TANK ? S_TANK : S_ARCADE);
      return buf;
    }
    default: return S_HOLD_A_2S;
  }
}

void settings_screen(float dt) {
  hud_tape(0, 0, 1280, 24, G.tape_phase);
  hud_tape(0, 696, 1280, 24, G.tape_phase);
  hud_rect(0, 24, 1280, 672, C_CHAR, 150);
  hud_panel(280, 72, 720, 540, C_SLAB, C_STEEL_HI);
  hud_text(F_H2, 312, 88, S_SETTINGS, C_BONE, 255, AL_LEFT);
  int row = clampi(G.settings_row, 0, LEN(SETTINGS_ROWS) - 1);
  float d = focus_block(&settings_cursor_y, 136 + 32 * row, 312, 656, dt);
  int on = d < 8;
  for (int k = 0; k < LEN(SETTINGS_ROWS); k++) {
    char vb[64]; int idx, max;
    const char *v = setting_value(k, vb, sizeof vb, &idx, &max);
    int y = 136 + 32 * k;
    int is_choice = k < 11, f = k == row;
    if (k == 11) {
      /* STICKS: the label in the row, one stepper per seated pad as text */
      if (f) focus_row_text(312, y, 656, SETTINGS_ROWS[k], "", on, 0, 0, 0); else menu_row(312, y, 656, SETTINGS_ROWS[k], "", 0, 1, 0, 0, 0);
      hud_text(F_MONO, 952, y + 10, v, f && on ? C_TAPE : C_BONE, 255, AL_RIGHT);
    } else if (k == 12) {
      if (f) focus_row_text(312, y, 656, SETTINGS_ROWS[k], "", on, 0, 0, 0); else menu_row(312, y, 656, SETTINGS_ROWS[k], "", 0, 1, 0, 0, 0);
      int kw = hud_text_w(F_MONO_S, "A") + 14;
      int kx = 952 - hud_text_w(F_MONO_S, S_HOLD_A_2S) - 8 - kw;
      hud_text(F_MONO_S, 952, y + 12, S_HOLD_A_2S, f && on ? C_TAPE : C_ASH, 255, AL_RIGHT);
      hud_keycap(kx, y + 8, "A", f && on ? C_TAPE : C_BONE, f && on ? C_HAZARD : C_TAPE);
      if (G.reset_hold > 0) hud_ring(kx + kw / 2, y + 20, 18, 3, clampf(G.reset_hold / 2, 0, 1), C_RUST, 255);
    } else {
      if (f) focus_row_text(312, y, 656, SETTINGS_ROWS[k], v, on, is_choice, idx > 0, idx < max);
      else menu_row(312, y, 656, SETTINGS_ROWS[k], v, 0, 1, is_choice, idx > 0, idx < max);
    }
  }
  if (G.settings_saved_t > 0 && G.settings_saved_t < 1) hud_text(F_MONO_S, 968, 580, S_SAVED, C_ASH, 255, AL_RIGHT);
  key_hint(312, 576, "B", S_KEY_BACK, C_ASH);
  hud_dirty(0, 0, 1280, 720);
}

/* -- hall of fame (10.23)  -- */
void hall_screen(float dt) {
  (void)dt;
  hud_tape(0, 0, 1280, 24, G.tape_phase);
  hud_tape(0, 696, 1280, 24, G.tape_phase);
  hud_rect(0, 24, 1280, 672, C_CHAR, 150);
  hud_panel(200, 96, 880, 480, C_SLAB, C_STEEL_HI);
  int x = 232, y = 112;
  hud_text(F_H2, x, y, S_HALL, C_BONE, 255, AL_LEFT);
  char buf[64];
  /* the two records */
  y += 56;
  hud_text(F_MONO_L, x, y, S_FASTEST_KO, C_ASH, 255, AL_LEFT);
  if (SAVE.fastest_ko_ms > 0) {
    unsigned s = SAVE.fastest_ko_ms / 1000;
    snprintf(buf, sizeof buf, "%u:%02u", s / 60, s % 60);
    hud_text(F_MONO_L, x + 300, y, buf, C_BONE, 255, AL_LEFT);
    hud_seat_chip(x + 420, y + 4, 24, 24, SAVE.fastest_ko_seat, F_MONO_S, 0);
    hud_text(F_MONO_L, x + 452, y, SAVE.fastest_ko_name, C_BONE, 255, AL_LEFT);
  } else hud_text(F_MONO_L, x + 300, y, S_NONE_YET, C_SMOKE, 255, AL_LEFT);
  y += 36;
  hud_text(F_MONO_L, x, y, S_LONGEST_STREAK, C_ASH, 255, AL_LEFT);
  if (SAVE.longest_streak > 0) {
    snprintf(buf, sizeof buf, "%u", (unsigned)SAVE.longest_streak);
    hud_text(F_MONO_L, x + 300, y, buf, C_BONE, 255, AL_LEFT);
    hud_seat_chip(x + 420, y + 4, 24, 24, SAVE.streak_seat, F_MONO_S, 0);
    hud_text(F_MONO_L, x + 452, y, SAVE.streak_name, C_BONE, 255, AL_LEFT);
  } else hud_text(F_MONO_L, x + 300, y, S_NONE_YET, C_SMOKE, 255, AL_LEFT);
  /* wins by build: 7 short bars scaled to the largest */
  y += 52;
  hud_text(F_MONO_S, x, y, S_WINS_BY_BUILD, C_ASH, 255, AL_LEFT);
  y += 22;
  uint32_t mx = 1;
  for (int i = 0; i < 7; i++) if (SAVE.wins_by_preset[i] > mx) mx = SAVE.wins_by_preset[i];
  for (int i = 0; i < 7; i++) {
    int col = i / 4, rowi = i % 4;
    int bx = x + col * 420, by = y + rowi * 24;
    const char *nm = i < 6 ? PRESETS[i].name : S_CUSTOM;
    hud_text(F_MONO, bx, by, nm, C_BONE, 255, AL_LEFT);
    int bw = (int)(200.0f * (float)SAVE.wins_by_preset[i] / (float)mx);
    hud_rect(bx + 110, by + 5, 200, 12, C_SLAB_HI, 255);
    if (bw > 0) hud_rect(bx + 110, by + 5, bw, 12, C_HAZARD, 255);
    snprintf(buf, sizeof buf, "%u", (unsigned)SAVE.wins_by_preset[i]);
    hud_text(F_MONO, bx + 320, by, buf, SAVE.wins_by_preset[i] ? C_BONE : C_SMOKE, 255, AL_LEFT);
  }
  /* KOs by cause */
  y += 4 * 24 + 16;
  hud_text(F_MONO_S, x, y, S_KOS_BY_CAUSE, C_ASH, 255, AL_LEFT);
  y += 22;
  {
    int cx = x;
    for (int i = 0; i < 6; i++) {
      snprintf(buf, sizeof buf, "%s %u", KO_SHORT[i], (unsigned)SAVE.ko_by_cause[i]);
      hud_text(F_MONO, cx, y, buf, SAVE.ko_by_cause[i] ? C_BONE : C_SMOKE, 255, AL_LEFT);
      cx += hud_text_w(F_MONO, buf) + 28;
    }
  }
  y += 36;
  snprintf(buf, sizeof buf, S_MATCHES, (int)SAVE.matches);
  hud_text(F_MONO_L, x, y, buf, C_BONE, 255, AL_LEFT);
  /* wipe and back */
  int hy = 96 + 480 - 44;
  key_hint(x, hy, "A", S_KEY_BACK, C_ASH);
  int bw2 = hud_text_w(F_MONO_S, "B") + 14;
  int bx = 1080 - 32 - hud_text_w(F_MONO_S, S_HOLD_B_WIPE) - 8 - bw2;
  hud_keycap(bx, hy, "B", C_BONE, C_TAPE);
  hud_text(F_MONO_S, bx + bw2 + 8, hy + 4, S_HOLD_B_WIPE, C_ASH, 255, AL_LEFT);
  if (G.hall_wipe_hold > 0) hud_ring(bx + bw2 / 2, hy + 12, 18, 3, clampf(G.hall_wipe_hold / 3, 0, 1), C_RUST, 255);
  hud_dirty(0, 0, 1280, 720);
}

/* -- controls card (10.24)  -- */
static void callout(int x, int y, int k, int compact) {
  int w = compact ? 112 : 120, h = compact ? 40 : 44;
  hud_rect(x + 4, y + 4, w, h, C_SHADOW, 255);
  hud_rect(x, y, w, h, C_HAZARD, 255);
  hud_text(F_MONO_S, x + w / 2, y + 4, CARD_CALLOUT[k], C_TAPE, 255, AL_CENTER);
  hud_text(F_MONO_S, x + w / 2, y + h - 20, CARD_CALLOUT_SUB[k], C_TAPE, 255, AL_CENTER);
}

/* the bone pad outline: body, two sticks, the face cluster, start, the bumpers */
static void pad_outline(int cx, int cy, float sc) {
  int bw = (int)(260 * sc), bh = (int)(120 * sc);
  hud_frame(cx - bw / 2, cy - bh / 2, bw, bh, 2, C_BONE, 255);
  /* grips */
  hud_frame(cx - bw / 2, cy + bh / 2 - 2, (int)(70 * sc), (int)(40 * sc), 2, C_BONE, 255);
  hud_frame(cx + bw / 2 - (int)(70 * sc), cy + bh / 2 - 2, (int)(70 * sc), (int)(40 * sc), 2, C_BONE, 255);
  /* bumpers */
  hud_rect(cx - bw / 2 + (int)(20 * sc), cy - bh / 2 - (int)(12 * sc), (int)(60 * sc), (int)(10 * sc), C_BONE, 255);
  hud_rect(cx + bw / 2 - (int)(80 * sc), cy - bh / 2 - (int)(12 * sc), (int)(60 * sc), (int)(10 * sc), C_BONE, 255);
  /* left stick */
  int lx = cx - (int)(80 * sc), ly = cy - (int)(10 * sc), r = (int)(22 * sc);
  hud_circle(lx, ly, r, C_BONE, 255);
  hud_circle(lx, ly, r - 2, C_CHAR, 255);
  hud_circle(lx, ly, r / 2, C_BONE, 255);
  /* d-pad */
  int dx = cx - (int)(44 * sc), dy = cy + (int)(28 * sc), ds = (int)(8 * sc);
  hud_rect(dx - ds / 2, dy - ds * 3 / 2, ds, ds * 3, C_BONE, 255);
  hud_rect(dx - ds * 3 / 2, dy - ds / 2, ds * 3, ds, C_BONE, 255);
  /* face buttons: Y top, X left, B right, A bottom */
  int fx = cx + (int)(80 * sc), fy = cy - (int)(10 * sc), fs = (int)(24 * sc), fr = (int)(9 * sc);
  const char *lab[4] = { "Y", "X", "B", "A" };
  int ox[4] = { 0, -fs, fs, 0 }, oy[4] = { -fs, 0, 0, fs };
  for (int k = 0; k < 4; k++) {
    hud_circle(fx + ox[k], fy + oy[k], fr, C_BONE, 255);
    if (sc >= 0.9f) hud_text(F_MONO_S, fx + ox[k], fy + oy[k] - 8, lab[k], C_CHAR, 255, AL_CENTER);
  }
  /* right stick */
  int rx = cx + (int)(30 * sc), ry = cy + (int)(28 * sc);
  hud_circle(rx, ry, (int)(14 * sc), C_BONE, 255);
  hud_circle(rx, ry, (int)(12 * sc), C_CHAR, 255);
  /* start */
  hud_rect(cx - (int)(16 * sc), cy - (int)(4 * sc), (int)(32 * sc), (int)(8 * sc), C_BONE, 255);
}

void controls_card(const rect_t *r, int compact, int seat) {
  int x = r->x, y = r->y, w = r->w, h = r->h;
  hud_rect(x + 6, y + 6, w, h, C_SHADOW, 255);
  hud_rect(x, y, w, h, C_CHAR, 255);
  hud_frame(x, y, w, h, 2, seat >= 0 ? seat_colour(seat) : C_STEEL_HI, 255);
  if (!compact) {
    int cx = x + 320, cy = y + 150;
    pad_outline(cx, cy, 1.0f);
    /* six callouts joined by rust lines: DRIVE, WEAPON, BRAKE, BOOST, TAUNT, PAUSE */
    int px[6] = { x + 40, x + 300, x + 480, x + 40, x + 250, x + 150 };
    int py[6] = { y + 110, y + 236, y + 60, y + 24, y + 24, y + 236 };
    int tx[6] = { cx - 80, cx + 80, cx + 104, cx - 110, cx + 56, cx };
    int ty[6] = { cy - 10, cy + 14, cy - 10, cy - 72, cy - 10, cy };
    int ax[6] = { px[0] + 120, px[1] + 60, px[2], px[3] + 120, px[4] + 60, px[5] + 120 };
    int ay[6] = { py[0] + 22, py[1], py[2] + 22, py[3] + 22, py[4] + 44, py[5] };
    for (int k = 0; k < 6; k++) {
      hud_line(ax[k], ay[k], tx[k], ty[k], 2, C_RUST, 255);
      hud_circle(tx[k], ty[k], 4, C_RUST, 255);
    }
    for (int k = 0; k < 6; k++) callout(px[k], py[k], k, 0);
    hud_text(F_MONO_S, x + w / 2, y + 300, CARD_LINE, C_BONE, 255, AL_CENTER);
    if (seat == 0) hud_text(F_MONO_S, x + w / 2, y + 326, CARD_KEYS, C_ASH, 255, AL_CENTER);
  } else {
    /* four callouts in a 2 x 2 grid round a small pad, no lines */
    pad_outline(x + w / 2, y + h / 2 - 6, 0.5f);
    callout(x + 12, y + 12, 0, 1);
    callout(x + w - 124, y + 12, 1, 1);
    callout(x + w - 124, y + h - 54, 2, 1);
    callout(x + 12, y + h - 54, 3, 1);
  }
  hud_dirty(x, y, w + 6, h + 6);
}

/* -- rules flash (10.25)  -- */
static void keycap_press(int x, int y, const char *k, int pressed) {
  if (pressed) hud_keycap(x, y + 2, k, C_HAZARD, C_TAPE);
  else hud_keycap(x, y, k, C_BONE, C_TAPE);
}

/* a plain bot square seen from above: body, a seat band, wheels */
static void bot_square(int x, int y, int w, int h, uint32_t body, int inverted) {
  hud_rect(x, y, w, h, body, 255);
  hud_frame(x, y, w, h, 2, C_CHAR, 255);
  if (!inverted) { hud_rect(x - 6, y + 6, 6, h / 3, C_CHAR, 255); hud_rect(x + w, y + 6, 6, h / 3, C_CHAR, 255); hud_rect(x - 6, y + h - 6 - h / 3, 6, h / 3, C_CHAR, 255); hud_rect(x + w, y + h - 6 - h / 3, 6, h / 3, C_CHAR, 255); }
  else { hud_rect(x + 8, y + 8, w - 16, h - 16, C_CHAR, 255); hud_rect(x + 12, y + h / 2 - 3, w - 24, 6, C_STEEL_HI, 255); }
}

static void flash_picture(int step, float lt, int cx, int cy) {
  switch (step) {
    case 0: { /* BUILD A BOT: three part glyphs slide in and add up, the bar fills to 11, then shakes at 21 */
      const int gl[4] = { GL_WEDGE, GL_TREAD, GL_SPINNER, GL_HAMMER };
      const int cost[4] = { 2, 2, 7, 10 };
      int sum = 0;
      for (int k = 0; k < 4; k++) {
        float t0 = k * 0.5f;
        if (lt < t0) break;
        float f = hud_ease_out(clampf((lt - t0) / 0.3f, 0, 1));
        int gx = cx - 230 + k * 120 + (int)((1 - f) * -300), gy = cy - 90;
        hud_rect(gx, gy, 72, 72, C_SLAB_HI, 255);
        hud_frame(gx, gy, 72, 72, 2, k == 3 ? C_RUST : C_STEEL_HI, 255);
        hud_glyph(gx + 12, gy + 12, gl[k], 3, k == 3 ? C_RUST : C_BONE, 255);
        char b[4]; snprintf(b, sizeof b, "%d", cost[k]);
        hud_text(F_MONO_L, gx + 36, gy + 78, b, C_HAZARD, 255, AL_CENTER);
        if (k > 0) hud_glyph(gx - 40, gy + 28, GL_PLUS, 1, C_ASH, 255);
        sum += cost[k];
      }
      /* the tag bar */
      int shake = sum > 20 ? (((int)(lt * 16) & 1) ? 2 : -2) : 0;
      int bx = cx - 160 + shake, by = cy + 50;
      for (int k = 0; k < 20; k++) hud_tag(bx + k * 16, by, k < sum, 0, sum >= 19 && k >= 18 && k < sum);
      for (int k = 20; k < sum && k < 24; k++) { hud_rect(bx + k * 16 + 3, by, 11, 10, C_FAULT, 255); hud_rect(bx + k * 16, by + 3, 3, 7, C_FAULT, 255); }
      char s[48];
      if (sum > 20) { snprintf(s, sizeof s, S_OVER_BUDGET, sum - 20); hud_text(F_MONO, cx, by + 24, s, C_RUST, 255, AL_CENTER); }
      else { snprintf(s, sizeof s, S_TAGS_SPENT, sum, 20 - sum); hud_text(F_MONO, cx, by + 24, s, C_BONE, 255, AL_CENTER); }
      break;
    }
    case 1: { /* HOLD TO SPIN: an A keycap presses, the ring fills over 2 s, then sparks on a second square */
      int pressed = lt > 0.2f && lt < 2.1f;
      keycap_press(cx - 230, cy - 12, "A", pressed);
      float spin = clampf((lt - 0.2f) / 2.0f, 0, 1);
      hud_rect(cx - 110, cy - 60, 120, 120, C_SLAB_HI, 255);
      hud_frame(cx - 110, cy - 60, 120, 120, 2, C_STEEL_HI, 255);
      hud_glyph(cx - 74, cy - 24, GL_SPINNER, 3, C_BONE, 255);
      hud_ring(cx - 50, cy, 70, 6, spin, spin >= 1 ? C_LIVE : C_HAZARD, 255);
      bot_square(cx + 120, cy - 40, 80, 80, C_LIVE, 0);
      if (lt > 2.0f) {
        float f = clampf((lt - 2.0f) / 0.5f, 0, 1);
        for (int k = 0; k < 12; k++) {
          float ang = k * 0.5236f, d = 20 + f * 90;
          hud_rect(cx + 120 + (int)(cosf(ang) * d), cy + (int)(sinf(ang) * d * 0.8f), 4, 4, k & 1 ? C_HAZARD : C_BONE, (int)(255 * (1 - f)));
        }
        hud_text(F_H3, cx + 160, cy - 90 - (int)(f * 20), "24", C_BONE, (int)(255 * (1 - f * 0.6f)), AL_CENTER);
      }
      break;
    }
    case 2: { /* HIT THE SIDE: a top-down bot, the front plate 0.6, the sides 1.0; hits flash +24 and +14 */
      int bx = cx - 100, by = cy - 70, bw = 200, bh = 140;
      hud_rect(bx, by, bw, bh, C_SLAB_HI, 255);
      hud_frame(bx, by, bw, bh, 3, C_BONE, 255);
      hud_rect(bx, by, bw, 14, C_SMOKE, 255);              /* the front plate, dim */
      hud_glyph(cx - 16, cy - 16, GL_BOT, 2, C_ASH, 255);
      hud_text(F_MONO, cx, by - 30, "0.6", C_ASH, 255, AL_CENTER);
      hud_text(F_MONO, bx - 120, cy - 12, "1.0", C_BONE, 255, AL_CENTER);
      hud_text(F_MONO, bx + bw + 120, cy - 12, "1.0", C_BONE, 255, AL_CENTER);
      hud_text(F_MONO, cx, by + bh + 8, "1.0", C_BONE, 255, AL_CENTER);
      /* the side hit: the arrow starts inside the label's clear lane and lands on the plate */
      float f1 = clampf((lt - 0.6f) / 0.4f, 0, 1);
      int ax = bx - 80 + (int)(f1 * 56);
      hud_tri(ax, cy - 14, ax, cy + 14, ax + 24, cy, C_HAZARD, 255);
      if (lt > 1.0f) {
        float f = clampf((lt - 1.0f) / 0.8f, 0, 1);
        hud_text_sh(F_H2, bx - 50, cy - 60 - (int)(f * 40), "+24", C_HAZARD, (int)(255 * (1 - f * 0.7f)), AL_CENTER);
        if (f < 0.3f) hud_rect(bx, by, 10, bh, C_WHITE, (int)(160 * (1 - f / 0.3f)));
      }
      /* the front hit */
      float f2 = clampf((lt - 1.4f) / 0.4f, 0, 1);
      int ay = by - 110 + (int)(f2 * 80);
      if (lt > 1.2f) hud_tri(cx + 60 - 14, ay, cx + 60 + 14, ay, cx + 60, ay + 24, C_HAZARD, 255);
      if (lt > 1.8f) {
        float f = clampf((lt - 1.8f) / 0.7f, 0, 1);
        hud_text_sh(F_H2, cx + 130, by - 50 - (int)(f * 40), "+14", C_SMOKE, (int)(255 * (1 - f * 0.7f)), AL_CENTER);
      }
      break;
    }
    case 3: { /* FLIPPED? B: a bot tumbles, the count shrinks in rust, B presses, it rights itself */
      int inverted = lt > 0.3f && lt < 2.0f;
      int bob = inverted ? (int)(sinf(lt * 12) * 3) : 0;
      bot_square(cx - 200, cy - 40 + bob, 90, 70, C_HAZARD, inverted);
      if (lt > 0.3f && lt < 2.0f) {
        int n = 10 - (int)((lt - 0.3f) / 1.7f * 7);
        char s[4]; snprintf(s, sizeof s, "%d", clampi(n, 3, 10));
        int font = n > 7 ? F_TITLE : n > 5 ? F_H1 : F_H2;
        int fh = hud_font_h(font);
        hud_text_sh(font, cx, cy - fh / 2 - fh / 8, s, C_RUST, 255, AL_CENTER);
        hud_text(F_MONO, cx, cy + 70, S_RIGHT_YOURSELF, C_BONE, 255, AL_CENTER);
      }
      keycap_press(cx + 150, cy - 12, "B", lt > 1.7f && lt < 2.1f);
      if (lt >= 2.0f) hud_text(F_H3, cx, cy - 12, ST_INVERTED[0], C_SMOKE, 0, AL_CENTER);
      break;
    }
    case 4: { /* HAZARDS HURT: a pit tells then opens, a hammer swings, a bot dot drives round, a hover floats over */
      int px = cx - 60, py = cy - 50, pw = 120, ph = 100;
      int open = lt > 1.5f;
      int blink = ((int)(lt * 3) & 1);
      hud_rect(px, py, pw, ph, open ? C_CHAR : C_SLAB_HI, 255);
      if (!open && blink) hud_frame(px + 6, py + 6, pw - 12, ph - 12, 3, C_HAZARD, 255);
      mini_tape(px - 6, py - 6, pw + 12, 6); mini_tape(px - 6, py + ph, pw + 12, 6);
      mini_tape(px - 6, py, 6, ph); mini_tape(px + pw, py, 6, ph);
      /* the hammer on the right wall */
      float sw = sinf(lt * 4) * 0.5f + 0.5f;
      int hx = cx + 190, hy = cy - 90;
      hud_rect(hx - 6, hy, 12, 40, C_STEEL_HI, 255);
      hud_line(hx, hy + 40, hx - (int)(60 * sw), hy + 40 + (int)(70 * (1 - sw * 0.3f)), 6, C_ASH, 255);
      hud_glyph(hx - (int)(60 * sw) - 16, hy + 40 + (int)(70 * (1 - sw * 0.3f)) - 16, GL_HAMMER, 2, C_BONE, 255);
      /* the bot dot driving round the pit */
      float ang = lt * 2.2f;
      int dx = cx + (int)(cosf(ang) * 110), dy = cy + (int)(sinf(ang) * 70);
      hud_circle(dx, dy, 10, C_LIVE, 255);
      /* the hover floating over the pit */
      int hvy = cy - 16 + (int)(sinf(lt * 5) * 5);
      hud_glyph(cx - 16, hvy, GL_HOVER, 2, C_BONE, 255);
      /* the caption sits under the dot's orbit (cy + 80), never under the dot */
      hud_text(F_MONO_S, cx, cy + 112, ARENA_BLURB[AR_BAY], C_ASH, 255, AL_CENTER);
      break;
    }
    case 5: { /* LAST BOT STANDING: four seat squares, three go grey one by one, the last gets the KO stinger */
      for (int k = 0; k < 4; k++) {
        int sx = cx - 230 + k * 130, sy = cy - 60;
        int out = k < 3 && lt > 0.5f + k * 0.5f;
        if (out) { hud_rect(sx, sy, 90, 90, C_STEEL, 255); hud_glyph(sx + 29, sy + 29, GL_SKULL, 2, C_SMOKE, 255); hud_text(F_MONO_S, sx + 45, sy + 94, S_OUT, C_SMOKE, 255, AL_CENTER); }
        else hud_seat_chip(sx, sy, 90, 90, k, F_H1, 0);
      }
      if (lt > 2.0f) {
        int bx = cx - 260, by = cy + 50;
        hud_rect(bx + 6, by + 6, 520, 48, seat_colour(3), 255);
        hud_rect(bx, by, 520, 48, C_CHAR, 255);
        hud_text(F_STINGER_Q, cx, text_cy(F_STINGER_Q, by, 48), ST_KO[0], C_BONE, 255, AL_CENTER);
      }
      hud_text(F_MONO_S, cx, cy + 120, FLASH_SMALL_6, C_ASH, 255, AL_CENTER);
      break;
    }
    default: {
      rect_t cr = { cx - 320, cy - 180, 640, 360 };
      controls_card(&cr, 0, 0);
      break;
    }
  }
}

void flash_card(int step, float t) {
  step = clampi(step, 0, 6);
  if (step != flash_last_step) { flash_wipe = step > flash_last_step && flash_last_step >= 0 ? 1.0f : 0.0f; flash_last_step = step; }
  hud_rect(0, 0, 1280, 720, C_CHAR, 160);
  card(190, 60, 900, 500, step, 7);
  char buf[32];
  snprintf(buf, sizeof buf, S_N_OF_M, step + 1, 7);
  hud_eyebrow(230, 104, S_HOW_IT_WORKS, C_ASH, AL_LEFT);
  hud_text(F_MONO_S, 1050, 104, buf, C_ASH, 255, AL_RIGHT);
  hud_text_sh(F_H2, 640, 96, FLASH_CAPTION[step], C_BONE, 255, AL_CENTER);
  hud_clip(192, 146, 896, 360);
  flash_picture(step, t, 640, 326);
  hud_clip_clear();
  /* the 0.3 s wipe: a char curtain slides off to the right */
  if (flash_wipe > 0) {
    int wx = 190 + (int)((1 - flash_wipe) * 900);
    hud_rect(wx, 62, 1090 - wx, 496, C_CHAR, 255);
    flash_wipe -= 1.0f / 18.0f;
  }
  int kx = 230;
  if (game_rules_auto()) {
    /* the first evening's automatic flash: the cards run by themselves and any button skips */
    hud_text(F_MONO_S, kx, 516, S_KEY_ANY, C_BONE, 255, AL_LEFT);
    hud_text(F_MONO_S, kx + hud_text_w(F_MONO_S, S_KEY_ANY) + 8, 516, S_KEY_SKIP, C_ASH, 255, AL_LEFT);
  } else {
    kx += key_hint(kx, 512, "A", S_KEY_NEXT, C_ASH) + 24;
    kx += hud_keycap(kx, 512, "B", C_BONE, C_TAPE) + 6;
    key_hint(kx, 512, "START", S_KEY_SKIP, C_ASH);
  }
  hud_dirty(0, 0, 1280, 720);
}

/* -- pause (10.26)  -- */
void pause_screen(void) { pause_screen_dt(1.0f / 60); }

void pause_screen_dt(float dt) {
  int pauser = clampi(G.pauser, 0, 3);
  hud_frame(0, 0, 1280, 720, 12, seat_colour(pauser), 255);
  hud_text_sh(F_H1, 640, 240, S_PAUSED, C_BONE, 255, AL_CENTER);
  hud_panel(440, 320, 400, 180, C_SLAB, C_STEEL_HI);
  int row = clampi(G.pause_row, 0, LEN(PAUSE_ROWS) - 1);
  float d = focus_block(&pause_cursor_y, 330 + 40 * row, 450, 380, dt);
  for (int k = 0; k < LEN(PAUSE_ROWS); k++) {
    int y = 330 + 40 * k;
    if (k == row) focus_row_text(450, y, 380, PAUSE_ROWS[k], "", d < 8, 0, 0, 0);
    else menu_row(450, y, 380, PAUSE_ROWS[k], "", 0, 1, 0, 0, 0);
    if (k == 3 && G.reset_hold > 0 && k == row) hud_ring(810, y + 20, 14, 3, clampf(G.reset_hold, 0, 1), C_TAPE, 255);
  }
  hud_dirty(0, 0, 1280, 720);
}

/* one line at the bottom centre over any menu screen: a refused lock, the timer's fallback (game_flow.c toast) */
static void toast_line(void) {
  if (G.toast_t <= 0 || !G.toast[0]) return;
  /* the band under the vote cards and above the vote rule line; in the garage it sits over the bars */
  int tw = clampi(hud_text_w(F_MONO, G.toast) + 32, 0, 1240), x = 640 - tw / 2, y = 612;
  hud_rect(x + 4, y + 4, tw, 30, C_SHADOW, 255);
  hud_rect(x, y, tw, 30, C_CHAR, 255);
  hud_frame(x, y, tw, 30, 2, C_RUST, 255);
  hud_text(F_MONO, 640, text_cy(F_MONO, y, 30), G.toast, C_BONE, 255, AL_CENTER);
  hud_dirty(x, y, tw + 4, 34);
}

/* -- the dispatcher  -- */
int menus_draw(float dt) {
  int drew = 1;
  switch (G.phase) {
    case PH_BOOT: boot_screen(game_boot_progress()); break;
    case PH_TITLE: title_screen(dt); break;
    case PH_SETTINGS: settings_screen(dt); break;
    case PH_HALL: hall_screen(dt); break;
    case PH_RULES: flash_card(G.flash_step, G.flash_t); break;
    case PH_LOBBY: lobby_screen(dt); break;
    case PH_GARAGE: garage_screen(dt, game_garage_total() - G.phase_frames, game_garage_total()); break;   /* the live total: 60 s, 30 s on a rebuild */
    case PH_VOTE: vote_screen(dt); break;
    case PH_COUNTDOWN: countdown_screen(dt); break;
    default: drew = 0; break;
  }
  if (G.paused) pause_screen_dt(dt);
  if (drew) toast_line();
  /* screen bursts fired on a menu (the lock confetti) play out in the garage rather than waiting, frozen, for
   * the results screen; on the other menus they age unseen under an empty clip */
  if (drew && G.phase != PH_BOOT) {
    if (G.phase != PH_GARAGE && G.phase != PH_LOBBY) hud_clip(0, 0, 0, 0);
    fx2_draw(dt);
    hud_clip_clear();
  }
  return drew;
}

/* -- the fake menu state (RTR_FAKE_MENUS)  -- */
#ifdef RTR_FAKE_MENUS
/* the six arenas with the 6.4 tile lists and the hazards of the 6.3 maps, so the
 * thumbnail painter can be looked at before the arena agent fills arena.c */
static const tile_rect_t FAKE_T_BAY[] = { { 6, 4, 4, 2, 0 }, { 20, 3, 1, 8, 1 }, { 30, 18, 3, 3, 1 }, { 12, 22, 6, 1, 0 }, { 34, 5, 2, 2, 0 }, { 18, 11, 4, 1, 1 }, { 18, 15, 4, 1, 1 }, { 18, 11, 1, 5, 1 }, { 21, 11, 1, 5, 1 } };
static const tile_rect_t FAKE_T_LASER[] = { { 13, 13, 8, 8, 1 }, { 10, 11, 2, 1, 0 }, { 22, 11, 2, 1, 0 }, { 10, 22, 2, 1, 0 }, { 22, 22, 2, 1, 0 } };
static const tile_rect_t FAKE_T_PILE[] = { { 3, 3, 3, 3, 0 }, { 33, 2, 3, 3, 0 }, { 5, 20, 3, 3, 0 }, { 31, 21, 3, 3, 0 }, { 14, 2, 3, 3, 0 }, { 26, 23, 3, 3, 0 }, { 12, 8, 16, 1, 1 }, { 12, 19, 16, 1, 1 }, { 12, 8, 1, 12, 1 }, { 27, 8, 1, 12, 1 } };
static const tile_rect_t FAKE_T_DOCK[] = { { 0, 5, 34, 2, 1 }, { 5, 10, 2, 2, 0 }, { 27, 10, 2, 2, 0 } };
static const tile_rect_t FAKE_T_SHOP[] = { { 3, 16, 11, 1, 0 }, { 20, 16, 11, 1, 0 }, { 16, 3, 1, 11, 0 }, { 16, 20, 1, 11, 0 }, { 16, 0, 2, 2, 1 }, { 16, 32, 2, 2, 1 }, { 0, 16, 2, 2, 1 }, { 32, 16, 2, 2, 1 } };
static arena_def_t FAKE_ARENAS[6];
static match_t fake_match;

#define HZ(T, X, Z, W, D) { .type = T, .x = X, .z = Z, .w = W, .d = D }
#define PR(K, X, Z, W, D) { .kind = K, .x = X, .z = Z, .w = W, .d = D }
static void fake_arenas(void) {
  memcpy(FAKE_ARENAS, ARENAS, sizeof FAKE_ARENAS);
  arena_def_t *a;
  a = &FAKE_ARENAS[AR_BAY]; a->tiles = FAKE_T_BAY; a->ntiles = LEN(FAKE_T_BAY);
  { hazard_def_t h[] = { HZ(HZ_PIT, 0, 0, 2, 2), HZ(HZ_PULVERIZER, 0, -7.4f, 1.2f, 1.2f), HZ(HZ_PULVERIZER, 0, 7.4f, 1.2f, 1.2f) }; memcpy(a->haz, h, sizeof h); a->nhaz = 3; }
  a = &FAKE_ARENAS[AR_LASER]; a->tiles = FAKE_T_LASER; a->ntiles = LEN(FAKE_T_LASER);
  { hazard_def_t h[] = { HZ(HZ_FLAME, -3, -9, 1, 1), HZ(HZ_FLAME, 1, -9, 1, 1), HZ(HZ_FLAME, 5, -9, 1, 1), HZ(HZ_KILLSAW, -3, -3, 1.6f, 0.4f), HZ(HZ_KILLSAW, 3, -3, 1.6f, 0.4f), HZ(HZ_KILLSAW, -3, 4, 1.6f, 0.4f), HZ(HZ_KILLSAW, 3, 4, 1.6f, 0.4f), HZ(HZ_RAMP, 0, 0, 4, 4), HZ(HZ_LASER, 0, 0, 20, 0.1f) }; memcpy(a->haz, h, sizeof h); a->nhaz = 9; }
  a = &FAKE_ARENAS[AR_PILE]; a->tiles = FAKE_T_PILE; a->ntiles = LEN(FAKE_T_PILE);
  { hazard_def_t h[] = { HZ(HZ_MOUND, 0, 0, 8, 7), HZ(HZ_DROPZONE, -6, -5, 2, 2), HZ(HZ_DROPZONE, 6, 6, 2, 2), HZ(HZ_SCREW, 11.6f, 2, 0.8f, 4), HZ(HZ_BUMPER, -11.5f, -1, 0.8f, 0.8f), HZ(HZ_BUMPER, -11.5f, 1, 0.8f, 0.8f), HZ(HZ_BUMPER, -11.5f, 4, 0.8f, 0.8f) }; memcpy(a->haz, h, sizeof h); a->nhaz = 7; }
  a = &FAKE_ARENAS[AR_DOCK]; a->tiles = FAKE_T_DOCK; a->ntiles = LEN(FAKE_T_DOCK);
  { hazard_def_t h[] = { HZ(HZ_DECK, 0, -6.5f, 20, 3), HZ(HZ_RAMP, -5, -3, 2, 3), HZ(HZ_RAMP, 5, -3, 2, 3), HZ(HZ_FLOORFLIP, 0, 2.5f, 2, 2), HZ(HZ_PIT, 0, 6.5f, 3, 2) }; memcpy(a->haz, h, sizeof h); a->nhaz = 5; }
  a = &FAKE_ARENAS[AR_SHOP]; a->tiles = FAKE_T_SHOP; a->ntiles = LEN(FAKE_T_SHOP);
  { hazard_def_t h[] = { HZ(HZ_PULVERIZER, 0, -9.4f, 1.2f, 1.2f), HZ(HZ_PULVERIZER, 0, 9.4f, 1.2f, 1.2f), HZ(HZ_PULVERIZER, -9.4f, 0, 1.2f, 1.2f), HZ(HZ_PULVERIZER, 9.4f, 0, 1.2f, 1.2f), HZ(HZ_KILLSAW, -1.5f, 0, 1.6f, 0.4f), HZ(HZ_KILLSAW, 1.5f, 0, 1.6f, 0.4f) }; memcpy(a->haz, h, sizeof h); a->nhaz = 6; }
  { prop_def_t p[] = { PR(PR_BENCH, -5, 0, 6, 1), PR(PR_BENCH, 5, 0, 6, 1), PR(PR_BENCH, 0, -5, 1, 6), PR(PR_BENCH, 0, 5, 1, 6) }; memcpy(a->props, p, sizeof p); a->nprops = 4; }
  a = &FAKE_ARENAS[AR_ROOF];
  { hazard_def_t h[] = { HZ(HZ_BUMPER, 8.5f, -4.5f, 0.8f, 0.8f), HZ(HZ_BUMPER, 8.5f, 1.5f, 0.8f, 0.8f), HZ(HZ_BUMPER, 8.5f, 7.5f, 0.8f, 0.8f), HZ(HZ_BUMPER, -5.5f, 8.5f, 0.8f, 0.8f), HZ(HZ_BUMPER, 0.5f, 8.5f, 0.8f, 0.8f), HZ(HZ_BUMPER, 6.5f, 8.5f, 0.8f, 0.8f), HZ(HZ_WIND, 0, 0, 0, 0) }; memcpy(a->haz, h, sizeof h); a->nhaz = 7; }
  { prop_def_t p[] = { PR(PR_HVAC, -1.5f, -3.5f, 2, 2), PR(PR_HVAC, -4.5f, 1.5f, 2, 2) }; memcpy(a->props, p, sizeof p); a->nprops = 2; }
}
#undef HZ
#undef PR

static const char *FAKE_NAMES[] = {
  "boot", "title", "title-attract", "lobby-2p", "lobby-4p", "lobby-empty", "garage-2p", "garage-solo", "garage-4p", "garage-3p",
  "vote", "vote-decided", "countdown-3", "countdown-fight", "countdown-quads", "settings", "hall", "hall-empty",
  "flash-1", "flash-2", "flash-3", "flash-4", "flash-5", "flash-6", "flash-7", "controls", "controls-compact", "pause", "garage-controls" };
static int fake_scene;

int fake_menus_count(void) { return LEN(FAKE_NAMES); }

static void fake_seat(int i, int kind, int preset, int row, int locked, int ready) {
  seat_t *s = &G.seats[i];
  memset(s, 0, sizeof *s);
  s->kind = (uint8_t)kind; s->pad = kind == SEAT_HUMAN ? (int8_t)i : -1; s->vote = -1; s->rematch = -1; s->slot = -1; s->hazard_pick = -1;
  build_preset(&s->build, preset); s->build.paint = (uint8_t)i; s->preset = (uint8_t)preset;
  bot_name(s->name, &s->build);
  s->row = row; s->locked = (uint8_t)locked; s->ready = (uint8_t)ready; s->ai_level = AI_MEDIUM; s->scheme = SCHEME_ARCADE;
  build_bars(&s->build, &s->bars_new[0], &s->bars_new[1], &s->bars_new[2]);
  memcpy(s->bars_old, s->bars_new, sizeof s->bars_old);
  if (kind == SEAT_HUMAN) PADS[i].seat = i;
}

const char *fake_menus_setup(int scene) {
  fake_scene = clampi(scene, 0, LEN(FAKE_NAMES) - 1);
  VOTE_ARENAS = ARENAS;
  game_init();
  save_defaults(&SAVE);
  memset(&fake_match, 0, sizeof fake_match);
  for (int i = 0; i < 4; i++) { memset(&PADS[i], 0, sizeof PADS[i]); PADS[i].seat = -1; }
  title_cursor_y = -1; settings_cursor_y = -1; pause_cursor_y = -1; match_cursor_y = -1; vote_init = 0; flash_last_step = -1; flash_wipe = 0;
  lobby_empty_t = 0; lobby_auto_t = 0;
  for (int i = 0; i < 4; i++) { garage_shake_t[i] = 0; garage_last_cost[i] = -1; }
  for (int i = 0; i < MODE_COUNT; i++) lobby_lift[i] = 0;
  G.controls_card_seat = -1;
  G.tape_phase = 0;
  const char *name = FAKE_NAMES[fake_scene];
  if (!strcmp(name, "boot")) { G.phase = PH_BOOT; G.phase_t = 0.6f; }
  else if (!strncmp(name, "title", 5)) {
    G.phase = PH_TITLE; G.title_row = 0; G.phase_t = 3.0f;
    SAVE.best_evening_wins = 4; SAVE.best_seat = 1;
    if (!strcmp(name, "title-attract")) {
      G.attract = 1;
      M.nbots = 2;
      for (int i = 0; i < 2; i++) { build_t b; build_preset(&b, i + 1); bot_name(M.bots[i].name, &b); M.bots[i].hp_max = 100; M.bots[i].hp = i ? 62 : 88; }
    }
  }
  else if (!strcmp(name, "lobby-2p")) {
    G.phase = PH_LOBBY; G.phase_t = 4; G.lobby_mode_cursor = MODE_DEATHMATCH;
    fake_seat(0, SEAT_HUMAN, PRE_ROLL_OUT, 0, 0, 1); fake_seat(1, SEAT_HUMAN, PRE_LAUNCH, 0, 0, 0);
    G.seats[0].wins = 2; G.seats[1].wins = 1; G.seats[1].scheme = SCHEME_TANK;
  }
  else if (!strcmp(name, "lobby-4p")) {
    G.phase = PH_LOBBY; G.phase_t = 4; G.lobby_mode_cursor = MODE_LAST_BOT; G.fill_with_bot = 1;
    fake_seat(0, SEAT_HUMAN, PRE_ROLL_OUT, 0, 0, 1); fake_seat(1, SEAT_HUMAN, PRE_LAUNCH, 0, 0, 1);
    fake_seat(2, SEAT_HUMAN, PRE_CHOP, 0, 0, 0); fake_seat(3, SEAT_AI, PRE_STOMP, 0, 0, 1);
    G.seats[0].wins = 3; G.seats[1].wins = 1; G.seats[2].leave_hold = 0.6f; PADS[1].idle = 55;
  }
  else if (!strcmp(name, "lobby-empty")) { G.phase = PH_LOBBY; G.phase_t = 6; G.lobby_mode_cursor = MODE_HILL; lobby_empty_t = 4; }
  else if (!strcmp(name, "garage-2p") || !strcmp(name, "garage-controls")) {
    G.phase = PH_GARAGE; G.phase_t = 18; G.phase_frames = 18 * 60; G.players = 2; G.frame = 2000;
    fake_seat(0, SEAT_HUMAN, PRE_CHOP, ROW_DRIVE, 1, 0);
    snprintf(G.seats[0].hint, sizeof G.seats[0].hint, HINT_FLIPPED, 3);
    fake_seat(1, SEAT_HUMAN, PRE_LAUNCH, ROW_ARMOUR, 0, 0);
    G.seats[1].build.armour = AR_WEAR;                 /* 12 - 2 + 6 = 16, then the bars animate */
    G.seats[1].build.weapon = WP_HORIZ;                /* 2 + 2 + 7 + 6 = 17 */
    G.seats[1].build.drive = DR_TREAD;                 /* 2 + 5 + 7 + 6 = 20 */
    G.seats[1].build.chassis = CH_SHELL;               /* 4 + 5 + 7 + 6 = 22: OVER BUDGET BY 2 */
    build_bars(&G.seats[1].build, &G.seats[1].bars_new[0], &G.seats[1].bars_new[1], &G.seats[1].bars_new[2]);
    G.seats[1].bars_from_t = (int)G.frame - 30;
    G.seats[1].y_hold = 0.18f; G.seats[1].deny_frame = G.frame - 5;
    if (!strcmp(name, "garage-controls")) G.controls_card_seat = 0;
  }
  else if (!strcmp(name, "garage-solo")) {
    G.phase = PH_GARAGE; G.phase_t = 6; G.phase_frames = 360; G.players = 2; G.frame = 400;
    fake_seat(0, SEAT_HUMAN, PRE_LAUNCH, ROW_WEAPON, 0, 0);
    G.seats[0].build.weapon = WP_CRUSHER;              /* illegal in 1v1: smoke with NOT IN 1V1 */
    fake_seat(1, SEAT_AI, PRE_ROLL_OUT, ROW_PAINT, 1, 0);
  }
  else if (!strcmp(name, "garage-4p")) {
    G.phase = PH_GARAGE; G.phase_t = 43; G.phase_frames = 43 * 60; G.players = 4; G.frame = 3000;
    fake_seat(0, SEAT_HUMAN, PRE_CHOP, ROW_DRIVE, 0, 0);
    fake_seat(1, SEAT_HUMAN, PRE_LAUNCH, ROW_WEAPON, 0, 0);
    G.seats[1].build.chassis = CH_LUNCHBOX; G.seats[1].build.weapon2 = WP_SAW;   /* the sixth row */
    fake_seat(2, SEAT_HUMAN, PRE_TOAST, ROW_CHASSIS, 0, 0);
    G.seats[2].slot_machine_t = 20;
    fake_seat(3, SEAT_AI, PRE_ROLL_OUT, ROW_PAINT, 1, 0);
    G.seats[0].bars_from_t = (int)G.frame - 40; G.seats[0].bars_old[0] = 0.35f; G.seats[0].bars_old[2] = 0.7f;
    G.seats[0].y_hold = 0.28f;
  }
  else if (!strcmp(name, "garage-3p")) {
    G.phase = PH_GARAGE; G.phase_t = 43; G.phase_frames = 43 * 60; G.players = 3; G.frame = 3000;
    fake_seat(0, SEAT_HUMAN, PRE_CHOP, ROW_DRIVE, 0, 0);
    fake_seat(1, SEAT_HUMAN, PRE_LAUNCH, ROW_WEAPON, 1, 0);
    fake_seat(2, SEAT_HUMAN, PRE_TOAST, ROW_ARMOUR, 0, 0);
    G.seats[0].wins = 2; G.seats[1].wins = 1;
  }
  else if (!strncmp(name, "vote", 4)) {
    G.phase = PH_VOTE; G.phase_t = 2.0f; G.phase_frames = 120; G.players = 4; G.mode = MODE_DEATHMATCH;
    fake_seat(0, SEAT_HUMAN, PRE_ROLL_OUT, 0, 1, 1); fake_seat(1, SEAT_HUMAN, PRE_LAUNCH, 0, 1, 1);
    fake_seat(2, SEAT_HUMAN, PRE_CHOP, 0, 1, 1); fake_seat(3, SEAT_AI, PRE_STOMP, 0, 1, 1);
    G.seats[0].vote = AR_LASER; G.seats[1].vote = AR_LASER; G.seats[2].vote = AR_ROOF; G.seats[3].vote = AR_DOCK;
    G.vote_leader = AR_LASER; G.vote_one_camera = ONECAM_AUTO;
    if (!strcmp(name, "vote-decided")) { G.vote_decided = 1; G.vote_decided_t = 0.8f; G.phase_frames = 470; }
    fake_arenas();
    VOTE_ARENAS = FAKE_ARENAS;
  }
  else if (!strncmp(name, "countdown", 9)) {
    G.phase = PH_COUNTDOWN; G.players = 2; G.countdown_n = 3; G.phase_t = 0.08f;
    fake_seat(0, SEAT_HUMAN, PRE_CHOP, 0, 1, 1); fake_seat(1, SEAT_HUMAN, PRE_LAUNCH, 0, 1, 1);
    if (!strcmp(name, "countdown-fight")) { G.countdown_n = 0; G.phase_t = 3.3f; }
    if (!strcmp(name, "countdown-quads")) { G.players = 4; G.countdown_n = 2; G.phase_t = 1.4f; fake_seat(2, SEAT_HUMAN, PRE_TOAST, 0, 1, 1); fake_seat(3, SEAT_HUMAN, PRE_STOMP, 0, 1, 1); }
  }
  else if (!strcmp(name, "settings")) {
    G.phase = PH_SETTINGS; G.settings_row = 4; G.settings_saved_t = 0.4f; G.phase_t = 3;
    fake_seat(0, SEAT_HUMAN, PRE_ROLL_OUT, 0, 0, 0); fake_seat(1, SEAT_HUMAN, PRE_LAUNCH, 0, 0, 0);
    SAVE.scheme[1] = SCHEME_TANK; G.reset_hold = 0;
  }
  else if (!strcmp(name, "hall")) {
    G.phase = PH_HALL; G.phase_t = 3;
    SAVE.fastest_ko_ms = 18400; snprintf(SAVE.fastest_ko_name, sizeof SAVE.fastest_ko_name, "SPINNY PANCAKE"); SAVE.fastest_ko_seat = 1;
    SAVE.longest_streak = 5; snprintf(SAVE.streak_name, sizeof SAVE.streak_name, "POLITE DOORSTOP"); SAVE.streak_seat = 0;
    uint32_t w[7] = { 12, 9, 4, 2, 1, 3, 7 }; memcpy(SAVE.wins_by_preset, w, sizeof w);
    uint32_t c[6] = { 20, 6, 4, 2, 3, 1 }; memcpy(SAVE.ko_by_cause, c, sizeof c);
    SAVE.matches = 41; G.hall_wipe_hold = 1.2f;
  }
  else if (!strcmp(name, "hall-empty")) { G.phase = PH_HALL; G.phase_t = 3; }
  else if (!strncmp(name, "flash-", 6)) { G.phase = PH_RULES; G.flash_step = name[6] - '1'; G.flash_t = 2.2f; G.phase_t = 3; }
  else if (!strcmp(name, "controls")) { G.phase = PH_FIGHT; }
  else if (!strcmp(name, "controls-compact")) { G.phase = PH_FIGHT; }
  else if (!strcmp(name, "pause")) {
    G.phase = PH_FIGHT; G.paused = 1; G.pauser = 2; G.pause_row = 3; G.reset_hold = 0.5f;
    fake_seat(0, SEAT_HUMAN, PRE_CHOP, 0, 1, 1); fake_seat(1, SEAT_HUMAN, PRE_LAUNCH, 0, 1, 1); fake_seat(2, SEAT_HUMAN, PRE_TOAST, 0, 1, 1);
  }
  return name;
}

void fake_menus_draw(float t, float dt) {
  const char *name = FAKE_NAMES[fake_scene];
  G.tape_phase = t * 30;
  G.frame++;
  if (!strcmp(name, "controls")) { rect_t r = { 320, 180, 640, 360 }; hud_rect(0, 0, 1280, 720, C_GRIME, 255); controls_card(&r, 0, 0); return; }
  if (!strcmp(name, "controls-compact")) {
    hud_rect(0, 0, 1280, 720, C_GRIME, 255);
    rect_t r = { 160, 90, 320, 180 }; controls_card(&r, 1, 1);
    rect_t r2 = { 800, 450, 320, 180 }; controls_card(&r2, 1, 3);
    return;
  }
  if (!strcmp(name, "pause")) { pause_screen_dt(dt); return; }
  if (!strncmp(name, "flash-", 6)) { G.flash_t = 0.4f + fmodf(t, 2.1f); }
  if (!strncmp(name, "title", 5)) { G.phase_t = 3.0f + t; }
  if (!strncmp(name, "lobby", 5)) { G.phase_t = 4.0f + t; }
  menus_draw(dt);
}
#endif
