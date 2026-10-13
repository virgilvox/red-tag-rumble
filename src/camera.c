/* camera.c: chase cameras, ONE CAMERA, the KO orbit, punch and shake, viewport
 * rects per layout, wall push-out (spec section 9). Owned by the camera agent
 * (items 5, 6, 10, 15). The layout table and the camera objects are real in
 * this stub; the chase, orbit, shake and projection bodies are empty. */
#include "camera.h"
#include "world.h"
#include <math.h>

static t3_camera *cams[4], *shared;
static int cur_players = 2, cur_one_camera = 0, cur_split = 0;
static float nudge[4];

void cam_init(void) {
  for (int i = 0; i < 4; i++) {
    cams[i] = t3_perspective_camera_new(60, 16.f / 9, 0.1f, 100);
    t3_object_set_position(cams[i], 0, 2.2f, -3.5f);
    t3_object_look_at(cams[i], 0, 0.3f, 0.6f);
  }
  shared = t3_perspective_camera_new(50, 16.f / 9, 0.1f, 100);
  t3_object_set_position(shared, 0, 14, 10);
  t3_object_look_at(shared, 0, 0, 0);
}

int cam_layout_kind(int players, int one_camera) {
  if (one_camera) return LAY_SHARED;
  if (players <= 1) return LAY_FULL;
  if (players == 2) return LAY_HALVES;
  if (players == 3) return LAY_THREE;
  return LAY_QUADS;
}

float cam_fov(int layout) {
  switch (layout) {
    case LAY_QUADS: case LAY_THREE: return 70;
    case LAY_SHARED: return 50;
    default: return 60;
  }
}

/* 9.1: rects in 1280 x 720 reference space, y from the top */
void cam_layout(int players, int one_camera, rect_t out[4], int *n) {
  int kind = cam_layout_kind(players, one_camera);
  switch (kind) {
    case LAY_FULL: case LAY_SHARED:
      out[0] = (rect_t){ 0, 0, 1280, 720 }; *n = 1; break;
    case LAY_HALVES:
      out[0] = (rect_t){ 0, 0, 640, 720 }; out[1] = (rect_t){ 640, 0, 640, 720 }; *n = 2; break;
    case LAY_THREE:
      out[0] = (rect_t){ 0, 0, 640, 360 }; out[1] = (rect_t){ 640, 0, 640, 360 }; out[2] = (rect_t){ 0, 360, 640, 360 }; *n = 3; break;
    default:
      out[0] = (rect_t){ 0, 0, 640, 360 }; out[1] = (rect_t){ 640, 0, 640, 360 };
      out[2] = (rect_t){ 0, 360, 640, 360 }; out[3] = (rect_t){ 640, 360, 640, 360 }; *n = 4; break;
  }
  for (int i = 0; i < *n; i++) { cams[i]->fov = cam_fov(kind); }
  shared->fov = cam_fov(LAY_SHARED);
}

t3_camera *cam_get(int viewport) {
  if (cur_one_camera && viewport == 0) return shared;
  return viewport >= 0 && viewport < 4 ? cams[viewport] : 0;
}
t3_camera *cam_shared(void) { return shared; }
int cam_seat_of(int viewport) { return cur_one_camera ? -1 : viewport; }

void cam_set_match(const match_t *m, int one_camera_setting, int split_setting, const arena_def_t *a) {
  (void)a;
  cur_players = m ? m->players : 2;
  cur_one_camera = one_camera_setting == 2;      /* ALWAYS; AUTO hysteresis is the camera agent's */
  cur_split = split_setting;
}

int cam_current(rect_t out[4], int *n) {
  int players = cur_players;
  if (cur_split == 1 && players > 2) players = 2;        /* SPLIT HALVES caps at halves for 2; more players need quadrants */
  if (cur_split == 2 && players < 3) players = 4;        /* SPLIT QUADRANTS */
  cam_layout(players, cur_one_camera, out, n);
  return cam_layout_kind(players, cur_one_camera);
}

void cam_update(const match_t *m, float dt) { (void)m; (void)dt; }
void cam_punch(int seat, float nx, float nz, float amount) { (void)seat; (void)nx; (void)nz; (void)amount; }
void cam_shake(int seat, float amp) { (void)seat; (void)amp; }
void cam_shake_all(float amp) { (void)amp; }
void cam_set_shake_scale(int setting) { (void)setting; }
void cam_ko(int victim, int victor) { (void)victim; (void)victor; }
void cam_time(void) {}
int  cam_enemy_offscreen(int seat, float *edge_x, float *edge_y) { (void)seat; (void)edge_x; (void)edge_y; return 0; }
int  cam_project(int viewport, float x, float y, float z, float *sx, float *sy) { (void)viewport; (void)x; (void)y; (void)z; (void)sx; (void)sy; return 0; }
float cam_yaw(int seat) { (void)seat; return 0; }
void cam_nudge(int seat, float rx, float ry) { (void)ry; if (seat >= 0 && seat < 4) nudge[seat] = rx; }
int  cam_one_camera_active(void) { return cur_one_camera; }
int  cam_cut_frames(void) { return 0; }

void cam_turntable(int viewport, int table, float dist, float height) {
  t3_camera *c = cam_get(viewport);
  if (!c) return;
  float x, y, z;
  world_turntable_pos(table, &x, &y, &z);
  t3_object_set_position(c, x, y + height, z + dist);
  t3_object_look_at(c, x, y + 0.4f, z);
}

void cam_attract(float t) {
  t3_object_set_position(shared, sinf(t * 0.1f) * 4, 14, 12);
  t3_object_look_at(shared, 0, 0, 0);
}
