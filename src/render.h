/* render.h: the per-frame viewport loop (spec 14.2 "Frame, in wc_render").
 * main.c builds a match_view_t from cam_current and calls render_frame once per
 * frame after world_sync and cam_update and before the HUD pass. The body is
 * owned by the world agent (world.c): it runs the split-screen recipe of the
 * platform map, section 3:
 *   scissor test on, auto clear on;
 *   per viewport: world_set_cell_visibility(cell[i]), fx_face(i), aspect,
 *                 viewport and scissor equal (pixels, y from the top), render;
 *   scissor off, full viewport, auto clear off (main.c then renders the HUD quad).
 * The renderer and the negotiated size are defined in main.c. */
#ifndef RTR_RENDER_H
#define RTR_RENDER_H
#include "three.h"
#include "camera.h"

typedef struct {
  rect_t vp[4];        /* reference rects (1280 x 720); render_frame scales by RTR_W / 1280 */
  int n;               /* viewports to render, 1..4 */
  int layout;          /* LAY_* */
  int seat_of[4];      /* seat shown in each viewport, -1 for the shared view */
  int cell[4];         /* arena cell of each viewport's camera, for prop culling; -1 = all visible */
} match_view_t;

extern t3_renderer *RTR_R;      /* the one renderer, created in wc_init at the negotiated size */
extern int RTR_W, RTR_H;        /* negotiated resolution, capped at 1280 x 720 */

void render_frame(const match_view_t *v);
/* the HUD pass: scissor off, full viewport, auto clear off, then the HUD quad; main.c owns it */

#endif
