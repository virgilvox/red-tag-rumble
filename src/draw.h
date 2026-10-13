/* draw.h: the HUD layer (spec section 10). Every screen and overlay the
 * integration layer calls. Owned by two agents:
 *   draw.c        the fight: strips, stingers, damage numbers, minimap, clock,
 *                 chevron, count-out, hazard tape, tape seams, KO card, judges
 *                 card, results and the rematch menu, hazard seat view, speech
 *                 panel, pause (items 9, 10, 16)
 *   draw_menus.c  boot, title and attract scrim, lobby, garage panels (halves,
 *                 quadrants, the 3 player match card), arena vote, countdown,
 *                 settings, hall of fame, rules flash, controls card (item 11)
 * Not headless. draw_hud dispatches on G.phase and is the single entry point
 * main.c calls between render_frame and the HUD pass. */
#ifndef RTR_DRAW_H
#define RTR_DRAW_H
#include <stdint.h>
#include "hud.h"
#include "sim.h"
#include "camera.h"
#include "game.h"

/* ── entry ─────────────────────────────────────────────────────── */
void draw_init(void);
/* dispatches on game phase; per viewport strips project with cam_project(i) */
void draw_hud(float dt);
/* the current viewport rects the HUD is drawing over (set by main.c every frame from cam_current) */
void draw_set_views(const rect_t *vp, int n, int layout);

/* ── fight (draw.c) ────────────────────────────────────────────── */
/* the per viewport strip (10.4): 72 px tall in halves and full, 56 px in quadrants (compact) */
void hud_strip(const rect_t *vp, const bot_t *b, int compact);
/* the 320 px short strip for the practice bot in 1 player (10.17) */
void hud_strip_short(const rect_t *vp, const bot_t *b);
/* one stinger per viewport at a time, J_STINGER_FRAMES; a new one replaces the old */
void stinger(int viewport, const char *s, uint32_t seat_colour);
void stinger_all(const char *s, uint32_t seat_colour);
/* a world anchored damage number in viewport i (at most J_NUMBERS_PER_VIEW live) */
void damage_number(int viewport, float x, float y, float z, int value, uint32_t rgb, int big);
void damage_text(int viewport, float x, float y, float z, const char *s, uint32_t rgb);
void clock_chip(const match_t *m, int compact);
void minimap(const rect_t *r, const match_t *m, const arena_def_t *a, int big);
void chevron(int viewport, const rect_t *vp, int seat);
void countout(const rect_t *vp, const bot_t *b);
void hazard_tape_strip(const rect_t *vp, float phase);
void tape_seams(int layout);
void ko_card(float t);
void judges_card(float t);
void results_screen(float t);
void rematch_menu(float t);
void hazard_seat_view(const rect_t *vp, int seat);
void speech_panel(int viewport, float x, float y, float z, const char *line);
void pause_overlay(void);
void viewport_white(const rect_t *vp, int alpha);
void tape_cut(int frames_left);
void next_match_chip(int pad, float t);

/* ── menus (draw_menus.c) ──────────────────────────────────────── */
void boot_screen(float frac);
void title_screen(float dt);
void attract_overlay(void);
void lobby_screen(float dt);
/* one garage panel in its rect; compact for quadrants; the AI panel shows the difficulty toggle */
void garage_panel(const rect_t *r, int seat, int compact, float dt);
void garage_match_card(const rect_t *r);
void garage_timer_tape(int frames_left, int total);
void vote_screen(float dt);
void countdown_overlay(const rect_t *vp, int n, float t);
void settings_screen(float dt);
void hall_screen(float dt);
void flash_card(int step, float t);
void controls_card(const rect_t *r, int compact, int seat);
void bracket_card(const rect_t *r);

#endif
