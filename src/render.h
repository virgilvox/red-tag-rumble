/* render.h: the per-frame viewport loop (spec 14.2 "Frame, in wc_render").
 * main.c builds a match_view_t from cam_current and calls render_frame once per
 * frame after world_sync and cam_update and before the HUD pass. The body is
 * owned by the world agent (render.c): it runs the split-screen recipe of the
 * platform map, section 3:
 *   scissor test on, auto clear on;
 *   per viewport: world_set_cell_visibility(cell[i]), fx_face(i), aspect,
 *                 viewport and scissor equal (pixels, y from the top), render;
 *   scissor off, full viewport, auto clear off (main.c then renders the HUD quad).
 * The renderer and the negotiated size are defined in main.c.
 *
 * The 30 fps render fallback (13, 15): render_measure(frame_ms) once per frame
 * flips G.render_half on after 30 frames over 20 ms and off after 120 under
 * 12 ms; while it is on, render_frame renders every other frame and returns 0
 * on the frames it skips, and main.c should skip the HUD pass on those frames
 * too so the host presents the previous frame unchanged. */
#ifndef RTR_RENDER_H
#define RTR_RENDER_H
#include "three.h"
#include "camera.h"

typedef struct {
  rect_t vp[4];        /* reference rects (1280 x 720); render_frame scales by RTR_W / 1280 */
  int n;               /* viewports to render, 1..4 */
  int layout;          /* LAY_* */
  int seat_of[4];      /* seat shown in each viewport, -1 for the shared view */
  int cell[4];         /* arena cell of each viewport's camera, for prop culling; -1 = computed from the camera */
} match_view_t;

extern t3_renderer *RTR_R;      /* the one renderer, created in wc_init at the negotiated size */
extern int RTR_W, RTR_H;        /* negotiated resolution, capped at 1280 x 720 */

/* returns 1 when the viewports were rendered, 0 on a skipped half-rate frame */
int  render_frame(const match_view_t *v);
void render_measure(float frame_ms);
/* the HUD pass: scissor off, full viewport, auto clear off, then the HUD quad; main.c owns it */

#ifdef RTR_FAKE_SIM
/* the deterministic fake driver (render.c, compiled out by default): two or four
 * bots circling and ramming with scripted hit events every 90 frames */
void render_fake_tick(match_t *m);
#endif

#endif
