/* draw.c: the fight HUD and the post-fight screens (spec 10.4, 10.5, 10.14 to
 * 10.21, 10.26). Owned by the HUD agent (items 9, 10, 16). Stub bodies;
 * draw_hud dispatches on G.phase once the screens exist. */
#include "draw.h"
#include "content.h"

static rect_t views[4];
static int nviews, view_layout;

void draw_init(void) {}

void draw_set_views(const rect_t *vp, int n, int layout) {
  for (int i = 0; i < n && i < 4; i++) views[i] = vp[i];
  nviews = n; view_layout = layout;
}

void draw_hud(float dt) { (void)dt; (void)nviews; (void)view_layout; }

void hud_strip(const rect_t *vp, const bot_t *b, int compact) { (void)vp; (void)b; (void)compact; }
void hud_strip_short(const rect_t *vp, const bot_t *b) { (void)vp; (void)b; }
void stinger(int viewport, const char *s, uint32_t seat_colour) { (void)viewport; (void)s; (void)seat_colour; }
void stinger_all(const char *s, uint32_t seat_colour) { (void)s; (void)seat_colour; }
void damage_number(int viewport, float x, float y, float z, int value, uint32_t rgb, int big) { (void)viewport; (void)x; (void)y; (void)z; (void)value; (void)rgb; (void)big; }
void damage_text(int viewport, float x, float y, float z, const char *s, uint32_t rgb) { (void)viewport; (void)x; (void)y; (void)z; (void)s; (void)rgb; }
void clock_chip(const match_t *m, int compact) { (void)m; (void)compact; }
void minimap(const rect_t *r, const match_t *m, const arena_def_t *a, int big) { (void)r; (void)m; (void)a; (void)big; }
void chevron(int viewport, const rect_t *vp, int seat) { (void)viewport; (void)vp; (void)seat; }
void countout(const rect_t *vp, const bot_t *b) { (void)vp; (void)b; }
void hazard_tape_strip(const rect_t *vp, float phase) { (void)vp; (void)phase; }
void tape_seams(int layout) { (void)layout; }
void ko_card(float t) { (void)t; }
void judges_card(float t) { (void)t; }
void results_screen(float t) { (void)t; }
void rematch_menu(float t) { (void)t; }
void hazard_seat_view(const rect_t *vp, int seat) { (void)vp; (void)seat; }
void speech_panel(int viewport, float x, float y, float z, const char *line) { (void)viewport; (void)x; (void)y; (void)z; (void)line; }
void pause_overlay(void) {}
void viewport_white(const rect_t *vp, int alpha) { if (alpha > 0) hud_rect(vp->x, vp->y, vp->w, vp->h, C_WHITE, alpha); }
void tape_cut(int frames_left) { (void)frames_left; }
void next_match_chip(int pad, float t) { (void)pad; (void)t; }
