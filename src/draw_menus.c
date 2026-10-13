/* draw_menus.c: boot, title and attract, lobby, garage panels, arena vote,
 * countdown, settings, hall of fame, rules flash, controls card (spec 10.7 to
 * 10.13, 10.22 to 10.25). Owned by the menus agent (item 11). Stub bodies. */
#include "draw.h"
#include "content.h"

void boot_screen(float frac) { (void)frac; }
void title_screen(float dt) { (void)dt; }
void attract_overlay(void) {}
void lobby_screen(float dt) { (void)dt; }
void garage_panel(const rect_t *r, int seat, int compact, float dt) { (void)r; (void)seat; (void)compact; (void)dt; }
void garage_match_card(const rect_t *r) { (void)r; }
void garage_timer_tape(int frames_left, int total) { (void)frames_left; (void)total; }
void vote_screen(float dt) { (void)dt; }
void countdown_overlay(const rect_t *vp, int n, float t) { (void)vp; (void)n; (void)t; }
void settings_screen(float dt) { (void)dt; }
void hall_screen(float dt) { (void)dt; }
void flash_card(int step, float t) { (void)step; (void)t; }
void controls_card(const rect_t *r, int compact, int seat) { (void)r; (void)compact; (void)seat; }
