/* world.c: the three.c side (spec 14.2). Owned by the world agent (items 5, 8,
 * 13, 14, 15, 21). This stub owns the one scene with the fixed l1201 light
 * vector and the per-frame viewport loop (render_frame, the split-screen
 * recipe); every other entry point is empty. */
#include "world.h"
#include "render.h"
#include "camera.h"
#include "fx.h"
#include <string.h>

static t3_scene *S;
static t3_light *sun, *hemi, *point_a, *point_b;
static const arena_def_t *attached;

t3_scene *world_scene(void) { return S; }

void world_init(void) {
  S = t3_scene_new();
  /* lights, fixed for every scene (14.2): one hemisphere, one sun, two points; l1201, no shadows */
  hemi = t3_hemisphere_light_new(0xece3d3, 0x15120f, 1.2f);
  t3_object_add(&S->base, hemi);
  sun = t3_directional_light_new(0xece3d3, 2.0f);
  t3_object_set_position(sun, 6, 12, 4);
  t3_object_add(&S->base, sun);
  point_a = t3_point_light_new(0xf2ab1e, 6, 0, 2);
  t3_object_set_position(point_a, 8, 3, -6);
  t3_object_add(&S->base, point_a);
  point_b = t3_point_light_new(0x5fb892, 6, 0, 2);
  t3_object_set_position(point_b, -8, 3, 6);
  t3_object_add(&S->base, point_b);
}

static void set_lights(const arena_def_t *a) {
  if (!a) return;
  sun->color = t3_color_hex(a->sun); sun->intensity = a->sun_i;
  hemi->color = t3_color_hex(a->hemi_sky); hemi->ground_color = t3_color_hex(a->hemi_ground); hemi->intensity = a->hemi_i;
  point_a->color = t3_color_hex(a->point_a); point_a->intensity = a->point_a_i;   /* 0 keeps the light vector, not a removal */
  point_b->color = t3_color_hex(a->point_b); point_b->intensity = a->point_b_i;
  t3_object_set_position(point_a, a->pa[0], a->pa[1], a->pa[2]);
  t3_object_set_position(point_b, a->pb[0], a->pb[1], a->pb[2]);
}

void world_attach_arena(int arena) { attached = &ARENAS[arena % ARENA_COUNT]; set_lights(attached); }
void world_attach_garage(void) { attached = &GARAGE_BAY; set_lights(attached); }
void world_detach_all(void) { attached = 0; }
void world_sync(const match_t *m, float alpha) { (void)m; (void)alpha; }
void world_bot_build(int slot, const build_t *b, uint32_t seat_colour) { (void)slot; (void)b; (void)seat_colour; }
void world_bot_visible(int slot, int on) { (void)slot; (void)on; }
void world_bot_pose(int slot, float x, float y, float z, float heading, int inverted) { (void)slot; (void)x; (void)y; (void)z; (void)heading; (void)inverted; }
void world_turntable(int table, float angle) { (void)table; (void)angle; }
void world_turntable_pos(int table, float *x, float *y, float *z) { if (x) *x = 12.0f * table; if (y) *y = 0; if (z) *z = 0; }
void world_weapon_test_fire(int slot) { (void)slot; }
void world_set_cell_visibility(int cell, const arena_def_t *a) { (void)cell; (void)a; }
void paint_floor(const arena_def_t *a, int w, int h, uint8_t *rgba) { (void)a; memset(rgba, 0, (size_t)w * h * 4); }
void world_scorch(float x, float z, float r) { (void)x; (void)z; (void)r; }
void world_ops_gate(float open) { (void)open; }
void world_closing(int on) { (void)on; }
void world_compile_all(void) { if (RTR_R && S) t3_renderer_compile(RTR_R, S, cam_get(0)); }
void world_crowd(int frame, float level) { (void)frame; (void)level; }

/* ── the viewport loop (render.h) ───────────────────────────────── */
void render_frame(const match_view_t *v) {
  t3_renderer_set_scissor_test(RTR_R, true);
  t3_renderer_set_auto_clear(RTR_R, true);           /* clears colour and depth inside the scissor only */
  for (int i = 0; i < v->n; i++) {
    t3_camera *cam = cam_get(i);
    if (!cam) continue;
    rect_t r = rect_scale(&v->vp[i], RTR_W, RTR_H);
    world_set_cell_visibility(v->cell[i], attached);
    fx_face(i);
    cam->aspect = (float)r.w / (float)r.h;
    t3_camera_update_projection_matrix(cam);
    t3_renderer_set_viewport(RTR_R, r.x, r.y, r.w, r.h);   /* pixels, y from the top */
    t3_renderer_set_scissor(RTR_R, r.x, r.y, r.w, r.h);
    t3_renderer_render(RTR_R, S, cam);
  }
  t3_renderer_set_scissor_test(RTR_R, false);
  t3_renderer_set_viewport(RTR_R, 0, 0, RTR_W, RTR_H);
  t3_renderer_set_auto_clear(RTR_R, false);          /* the HUD quad composites over every view */
}
