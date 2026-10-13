/* camera.h: chase cameras, ONE CAMERA, the KO orbit, punch and shake, the
 * viewport rects per layout, wall push-out (spec section 9). Owned by the
 * camera agent (items 5, 6, 10, 15): bodies in camera.c. Not headless: it
 * owns t3_camera objects. */
#ifndef RTR_CAMERA_H
#define RTR_CAMERA_H
#include "three.h"
#include "sim.h"
#include "arena.h"

/* a rect in 1280 x 720 reference space, y from the top */
typedef struct { int x, y, w, h; } rect_t;

/* layouts (9.1) */
enum { LAY_FULL, LAY_HALVES, LAY_THREE, LAY_QUADS, LAY_SHARED };
/* the map quadrant of the 3 player layout */
#define RECT_MAP_QUADRANT ((rect_t){ 640, 360, 640, 360 })

/* scale a reference rect to the negotiated resolution */
static inline rect_t rect_scale(const rect_t *r, int W, int H) {
  rect_t o = { r->x * W / 1280, r->y * H / 720, r->w * W / 1280, r->h * H / 720 };
  return o;
}

void cam_init(void);
/* the viewport rects for a player count and the ONE CAMERA state; n is 1, 2, 3 or 4.
 * players 1 -> full; 2 -> vertical halves; 3 -> two top, bottom left (the fourth
 * quadrant is the map); 4 -> quadrants; one_camera -> one full rect. The
 * SPLIT setting (HALVES, QUADRANTS) is applied by cam_set_match. */
void cam_layout(int players, int one_camera, rect_t out[4], int *n);
int  cam_layout_kind(int players, int one_camera);
/* the FOV per layout (9.1): 60 full and halves, 70 quadrants, 50 shared */
float cam_fov(int layout);
/* the camera that renders viewport i (0..3); the shared camera under LAY_SHARED is cam_get(0) */
t3_camera *cam_get(int viewport);
t3_camera *cam_shared(void);
/* which seat viewport i shows (the seat index into M.bots), or -1 for the shared view */
int  cam_seat_of(int viewport);
/* bind the cameras to a match: players, the settings, the arena (shared yaw, walls for push-out) */
void cam_set_match(const match_t *m, int one_camera_setting, int split_setting, const arena_def_t *a);
/* the current layout and rects after cam_set_match and the ONE CAMERA hysteresis */
int  cam_current(rect_t out[4], int *n);
void cam_update(const match_t *m, float dt);
void cam_punch(int seat, float nx, float nz, float amount);
void cam_shake(int seat, float amp);
void cam_shake_all(float amp);
void cam_set_shake_scale(int setting);     /* SHAKE 0..3 */
/* the final KO: every viewport collapses to the orbit around the victim (9.4) */
void cam_ko(int victim, int victor);
/* TIME: pull up to the ONE CAMERA pose over 0.6 s */
void cam_time(void);
int  cam_enemy_offscreen(int seat, float *edge_x, float *edge_y);
/* project a world point through viewport i's camera into 1280 x 720 HUD space; 0 when behind */
int  cam_project(int viewport, float x, float y, float z, float *sx, float *sy);
/* the chase camera's yaw for pad_state_t.cam_yaw (HOVER and MELTY steer relative to it) */
float cam_yaw(int seat);
/* the right stick nudge, plus and minus 30 degrees, springs back */
void cam_nudge(int seat, float rx, float ry);
/* ONE CAMERA state and the tape cut (9.3): frames left in the cut, 0 when none */
int  cam_one_camera_active(void);
int  cam_cut_frames(void);
/* garage, lobby and results: viewport i looks at turntable `table` (0..3) at distance dist */
void cam_turntable(int viewport, int table, float dist, float height);
/* the shared orbit for the title attract */
void cam_attract(float t);

#endif
