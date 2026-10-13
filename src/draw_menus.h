/* draw_menus.h: the extra entry points of the menu screens beyond what draw.h
 * declares (spec 10.7 to 10.13, 10.22 to 10.26). Owned by the menus agent
 * (item 11); bodies in draw_menus.c. Not headless.
 *
 * draw.h declares the per-screen functions (title_screen, lobby_screen,
 * garage_panel, vote_screen, ...). This header adds the whole-screen helpers
 * that lay those out, so draw_hud (draw.c) can hand every menu phase to one
 * call, plus the pause screen (10.26) and the fake menu state the headless
 * HUD test drives. */
#ifndef RTR_DRAW_MENUS_H
#define RTR_DRAW_MENUS_H
#include "draw.h"

/* every menu phase in one call: PH_BOOT, PH_TITLE, PH_SETTINGS, PH_HALL, PH_RULES,
 * PH_LOBBY, PH_GARAGE, PH_VOTE, PH_COUNTDOWN; draws the pause screen over any phase
 * while G.paused is set, and the controls card over a seat's panel while
 * G.controls_card_seat >= 0. Returns 1 when it drew a screen (a menu phase), 0
 * when the phase is the fight's and draw.c owns the frame. */
int  menus_draw(float dt);

/* the whole garage screen: every seated panel in the fight layout (halves for
 * 1 and 2 seats, quadrants for 3 and 4), the 3 player match card, the seams and
 * the shared timer tape (10.9 to 10.11). frames_left is the garage clock. */
void garage_screen(float dt, int frames_left, int total_frames);
/* the 3D hole of a garage panel or a lobby card: the rect render_frame must draw
 * the turntable into (1280 x 720 space). Lobby cards: garage_hole(0, seat, &r)
 * for the halves layout, 1 for quadrants; lobby_hole(seat, &r). */
void garage_hole(int compact, const rect_t *panel, rect_t *out);
void lobby_hole(int seat, rect_t *out);
/* the pause screen (10.26): the frame in the pauser's colour, PAUSED, the four rows;
 * the _dt form tweens the cursor (pause_screen uses a 60 Hz step) */
void pause_screen(void);
void pause_screen_dt(float dt);
/* the 3 player match card with the cursor tween (garage_match_card uses a 60 Hz step) */
void garage_match_card_dt(const rect_t *r, float dt);
/* the countdown and FIGHT over every viewport (10.13) */
void countdown_screen(float dt);

#ifdef RTR_FAKE_MENUS
/* the fake menu state the HUD test drives: fills G, SAVE, M and PADS for one
 * screenshot scene and returns its name; count is how many scenes exist. */
int         fake_menus_count(void);
const char *fake_menus_setup(int scene);
/* one frame of the chosen scene at wall time t (seconds) */
void        fake_menus_draw(float t, float dt);
#endif

#endif
