/* world.c: the three.c side (spec 14.2). Owned by the world agent (items 5, 8,
 * 13, 14, 15, 21). One scene with the fixed l1201 light vector; six arena roots
 * and the garage root built at boot and attached one at a time; bots assembled
 * from primitives per part; hazards with their moving parts and emissive tells;
 * props grouped by cell for the vis table; the crowd; debris as one instanced
 * mesh; blob shadows; the painted floors; render interpolation in world_sync.
 * No shadow maps, no fog, no render targets. */
#include "world.h"
#include "render.h"
#include "camera.h"
#include "fx.h"
#include "hud.h"
#include "bot.h"
#include "game.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define PI 3.14159265f
#define TAU 6.2831853f

static t3_scene *S;
static t3_light *sun, *hemi, *point_a, *point_b;
static const arena_def_t *attached;
static int attached_idx = -1;
static float wall_dt = 1.f / 60;
static int ticked;
static float world_t;
static uint32_t wrng = 0x2545f491u;

static uint32_t wr(void) { return rtr_rand(&wrng); }
static float wrf(void) { return rtr_rand_f(&wrng); }

/* -- shared geometries and materials ------------------------------ */
/* meshes that share a geometry and a material are drawn as one instanced draw
 * by three.c's auto instancing, so both are cached and never released */
static t3_geometry *g_box, *g_cyl12, *g_cyl8, *g_disc, *g_circle, *g_plane, *g_ring, *g_sphere, *g_cone, *g_torus;

/* the cache compares the whole parameter tuple (a hashed key could hand a paint a steel of a related
 * colour) and grows as needed, so a late entry is never a fresh material per call */
typedef struct { uint32_t c, emis; float rough, metal, ei; int kind; t3_material *m; } mat_slot_t;
static mat_slot_t *mats; static int nmats, cmats;

static t3_material *mat_ex(uint32_t c, float rough, float metal, uint32_t emis, float ei, int kind) {
  for (int i = 0; i < nmats; i++) {
    const mat_slot_t *s = &mats[i];
    if (s->c == c && s->kind == kind && s->rough == rough && s->metal == metal && s->emis == emis && s->ei == ei) return s->m;
  }
  t3_material *m = kind == 1 ? t3_mesh_basic_material_new(c) : t3_mesh_standard_material_new(c);
  if (kind != 1) {
    m->roughness = rough; m->metalness = metal;
    if (ei > 0) { m->emissive = t3_color_hex(emis); m->emissive_intensity = ei; }
  }
  if (nmats == cmats) { cmats = cmats ? cmats * 2 : 256; mats = realloc(mats, sizeof *mats * (size_t)cmats); }
  mats[nmats++] = (mat_slot_t){ c, emis, rough, metal, ei, kind, m };
  return m;
}
static t3_material *mat(uint32_t c, float rough, float metal) { return mat_ex(c, rough, metal, 0, 0, 0); }
static t3_material *emis(uint32_t c, float k) { return mat_ex(c, 0.5f, 0, c, k, 0); }
static t3_material *flat(uint32_t c) { return mat_ex(c, 0, 0, 0, 0, 1); }
/* a transparent material is unique to its user (opacity animates) */
static t3_material *glass(uint32_t c, float opacity) {
  t3_material *m = t3_mesh_basic_material_new(c);
  m->transparent = true; m->opacity = opacity; m->depth_write = false;
  return m;
}
static t3_material *lexan_mat(void) {
  static t3_material *m;
  if (!m) { m = t3_mesh_standard_material_new(0x9fe0c8); m->transparent = true; m->opacity = 0.35f; m->roughness = 0.15f; m->metalness = 0.1f; m->depth_write = false; }
  return m;
}

/* painted, steel, plywood (14.2) */
#define M_PAINT(c) mat(c, 0.6f, 0.2f)
#define M_STEEL(c) mat(c, 0.3f, 0.8f)
#define M_PLY(c)   mat(c, 0.9f, 0.0f)
#define M_RUBBER(c) mat(c, 0.95f, 0.0f)

static t3_mesh *part(void *parent, t3_geometry *g, t3_material *m, float sx, float sy, float sz, float x, float y, float z) {
  t3_mesh *me = t3_mesh_new(g, m);
  t3_object_set_scale(me, sx, sy, sz);
  t3_object_set_position(me, x, y, z);
  t3_object_add(parent, me);
  t3_release(me);
  return me;
}
#define BOX(p, m, w, h, d, x, y, z) part(p, g_box, m, w, h, d, x, y, z)
#define CYL(p, m, r, h, x, y, z) part(p, g_cyl12, m, r, h, r, x, y, z)
static t3_mesh *rot(t3_mesh *m, float rx, float ry, float rz) { t3_object_set_rotation(m, rx, ry, rz); return m; }
static t3_object *group_at(void *parent, float x, float y, float z) {
  t3_object *g = t3_group_new();
  t3_object_set_position(g, x, y, z);
  t3_object_add(parent, g);
  t3_release(g);
  return g;
}
static void set_vis(void *o, int on) { ((t3_object *)o)->visible = on != 0; }

/* -- custom geometries -------------------------------------------- */
/* a triangle soup builder with flat normals */
typedef struct { float *p, *n, *uv; int cap, n_; } soup_t;
static void soup_init(soup_t *s, int tris) { s->cap = tris * 3; s->n_ = 0; s->p = calloc((size_t)s->cap * 3, 4); s->n = calloc((size_t)s->cap * 3, 4); s->uv = calloc((size_t)s->cap * 2, 4); }
static void soup_tri(soup_t *s, const float *a, const float *b, const float *c) {
  if (s->n_ + 3 > s->cap) return;
  float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
  float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx, l = sqrtf(nx * nx + ny * ny + nz * nz);
  if (l > 0) { nx /= l; ny /= l; nz /= l; }
  const float *v[3] = { a, b, c };
  for (int i = 0; i < 3; i++) {
    int k = s->n_++;
    s->p[k * 3] = v[i][0]; s->p[k * 3 + 1] = v[i][1]; s->p[k * 3 + 2] = v[i][2];
    s->n[k * 3] = nx; s->n[k * 3 + 1] = ny; s->n[k * 3 + 2] = nz;
    s->uv[k * 2] = v[i][0] + 0.5f; s->uv[k * 2 + 1] = v[i][2] + 0.5f;
  }
}
static void soup_quad(soup_t *s, const float *a, const float *b, const float *c, const float *d) { soup_tri(s, a, b, c); soup_tri(s, a, c, d); }
static t3_geometry *soup_finish(soup_t *s) {
  t3_geometry *g = t3_geometry_new();
  /* the geometry retains each attribute, so the creation ref goes: a wedge is rebuilt every garage
   * and every match, and with the ref kept its three arrays outlived the geometry */
  t3_attribute *p = t3_attribute_new(T3_FLOAT32, s->p, s->n_, 3), *n = t3_attribute_new(T3_FLOAT32, s->n, s->n_, 3), *uv = t3_attribute_new(T3_FLOAT32, s->uv, s->n_, 2);
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, p); t3_release(p);
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, n); t3_release(n);
  t3_geometry_set_attribute(g, T3_ATTR_UV, uv); t3_release(uv);
  t3_geometry_compute_bounding_sphere(g);
  t3_geometry_compute_bounding_box(g);
  free(s->p); free(s->n); free(s->uv);
  return g;
}

/* a wedge: footprint w (x) by d (z), the back face (z = -d/2) is h tall, sloping to the floor at the front */
static t3_geometry *wedge_geometry(float w, float d, float h) {
  soup_t s; soup_init(&s, 8);
  float x0 = -w / 2, x1 = w / 2, z0 = -d / 2, z1 = d / 2;
  float a[3] = { x0, 0, z0 }, b[3] = { x1, 0, z0 }, c[3] = { x1, 0, z1 }, dd[3] = { x0, 0, z1 };
  float e[3] = { x0, h, z0 }, f[3] = { x1, h, z0 };
  soup_quad(&s, a, dd, c, b);         /* bottom */
  soup_quad(&s, e, f, c, dd);         /* slope */
  soup_quad(&s, a, b, f, e);          /* back */
  soup_tri(&s, a, e, dd);             /* left */
  soup_tri(&s, b, c, f);              /* right */
  return soup_finish(&s);
}

/* a panel: a w x h plane facing +z, 4 x 4, with the dent state's clusters pushed in (14.2) */
static t3_geometry *panel_geometry(float w, float h, int state, uint32_t seed) {
  t3_geometry *g = t3_plane_geometry_new(w, h, 4, 4);
  if (state > 0) {
    float depth = state == 1 ? 0.02f : state == 2 ? 0.05f : 0.09f;
    uint32_t r = seed * 2654435761u + (uint32_t)state * 97u;
    int n = 2 + rtr_rand_n(&r, 3);
    t3_attribute *pa = g->attributes[T3_ATTR_POSITION];
    float *p = t3_attribute_f32(pa);
    for (int c = 0; c < n; c++) {
      float cx = (rtr_rand_f(&r) - 0.5f) * w * 0.7f, cy = (rtr_rand_f(&r) - 0.5f) * h * 0.7f;
      float rad = 0.25f * (w < h ? w : h) + rtr_rand_f(&r) * 0.2f * w;
      for (int i = 0; i < pa->count; i++) {
        float x = p[i * 3], y = p[i * 3 + 1];
        /* border vertices stay put so the panel keeps meeting the body */
        float bx = 1 - fabsf(x) / (w * 0.5f), by = 1 - fabsf(y) / (h * 0.5f);
        if (bx < 0.01f || by < 0.01f) continue;
        float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy));
        if (d < rad) { float k = 1 - d / rad; p[i * 3 + 2] -= depth * k * k * (0.7f + 0.3f * bx * by); }
      }
    }
    pa->version++;
    t3_geometry_compute_vertex_normals(g);
    t3_geometry_compute_bounding_sphere(g);
    t3_geometry_compute_bounding_box(g);
  }
  return g;
}

/* a crowd figure: body, head and arms as boxes; frame 0 idle, 1 leaning in, 2 arms up */
static t3_geometry *crowd_geometry(int frame) {
  soup_t s; soup_init(&s, 60);
  float lean = frame == 1 ? 0.12f : 0;
  #define BX(x0, y0, z0, x1, y1, z1) do { \
    float q0[3] = { x0, y0, z0 }, q1[3] = { x1, y0, z0 }, q2[3] = { x1, y1, z0 }, q3[3] = { x0, y1, z0 }; \
    float q4[3] = { x0, y0, z1 }, q5[3] = { x1, y0, z1 }, q6[3] = { x1, y1, z1 }, q7[3] = { x0, y1, z1 }; \
    soup_quad(&s, q0, q3, q2, q1); soup_quad(&s, q4, q5, q6, q7); soup_quad(&s, q0, q1, q5, q4); soup_quad(&s, q3, q7, q6, q2); \
    soup_quad(&s, q0, q4, q7, q3); soup_quad(&s, q1, q2, q6, q5); } while (0)
  BX(-0.22f, 0.0f, -0.1f, 0.22f, 0.95f, 0.1f);                        /* torso */
  BX(-0.14f, 1.0f, -0.12f + lean, 0.14f, 1.3f, 0.12f + lean);           /* head */
  if (frame == 2) { BX(-0.42f, 0.9f, -0.06f, -0.26f, 1.55f, 0.06f); BX(0.26f, 0.9f, -0.06f, 0.42f, 1.55f, 0.06f); }
  else { BX(-0.36f, 0.3f, -0.07f, -0.24f, 0.95f, 0.07f); BX(0.24f, 0.3f, -0.07f, 0.36f, 0.95f, 0.07f); }
  #undef BX
  return soup_finish(&s);
}

/* -- small textures ----------------------------------------------- */
typedef struct { uint8_t *px; int w, h; } canvas_t;
static void cv_px(canvas_t *c, int x, int y, uint32_t rgb, int a) {
  if (x < 0 || y < 0 || x >= c->w || y >= c->h) return;
  uint8_t *p = c->px + ((size_t)y * c->w + x) * 4;
  int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
  if (a >= 255) { p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b; p[3] = 255; return; }
  p[0] = (uint8_t)(p[0] + (r - p[0]) * a / 255); p[1] = (uint8_t)(p[1] + (g - p[1]) * a / 255); p[2] = (uint8_t)(p[2] + (b - p[2]) * a / 255); p[3] = 255;
}
static void cv_rect(canvas_t *c, int x, int y, int w, int h, uint32_t rgb, int a) {
  if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
  if (x + w > c->w) w = c->w - x; if (y + h > c->h) h = c->h - y;
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) cv_px(c, x + i, y + j, rgb, a);
}
static void cv_frame(canvas_t *c, int x, int y, int w, int h, int t, uint32_t rgb, int a) {
  cv_rect(c, x, y, w, t, rgb, a); cv_rect(c, x, y + h - t, w, t, rgb, a); cv_rect(c, x, y, t, h, rgb, a); cv_rect(c, x + w - t, y, t, h, rgb, a);
}
/* 45 degree caution tape, 12 px stripes, tape dark frame */
static void cv_tape(canvas_t *c, int x, int y, int w, int h, int pitch) {
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
    int s = ((i + j) / pitch) & 1;
    cv_px(c, x + i, y + j, s ? C_TAPE : C_HAZARD, 255);
  }
  cv_frame(c, x, y, w, h, 1, C_TAPE, 255);
}
static void cv_circle(canvas_t *c, int cx, int cy, int r, uint32_t rgb, int a) {
  for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) if (i * i + j * j <= r * r) cv_px(c, cx + i, cy + j, rgb, a);
}
static canvas_t cv_new(int w, int h) { canvas_t c = { calloc((size_t)w * h, 4), w, h }; return c; }
static t3_texture *cv_texture(canvas_t *c, int srgb, int repeat) {
  t3_texture *t = t3_texture_new(c->w, c->h, c->px);
  if (srgb) t->color_space = T3_SRGB_COLOR_SPACE;
  if (repeat) { t->wrap_s = T3_REPEAT; t->wrap_t = T3_REPEAT; }
  free(c->px); c->px = 0;
  return t;
}

static t3_texture *tex_tape, *tex_pegboard, *tex_honey, *tex_digit[5], *tex_sign;
static t3_material *mat_tape, *mat_digit[5];

/* capture a rect of the HUD buffer (1280 x 720 coordinates) into a canvas at w x h */
static canvas_t hud_capture(int x, int y, int w, int h) {
  canvas_t c = cv_new(w, h);
  for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) {
    int sx = (x + i * (w / HUD_DIV) / w) / HUD_DIV, sy = (y + j * (h / HUD_DIV) / h) / HUD_DIV;
    sx = (x + i) / HUD_DIV; sy = (y + j) / HUD_DIV;
    if (sx < 0 || sy < 0 || sx >= HUD_W || sy >= HUD_H) continue;
    const uint8_t *p = hud_px + ((size_t)(HUD_H - 1 - sy) * HUD_W + sx) * 4;
    memcpy(c.px + ((size_t)j * w + i) * 4, p, 4);
  }
  return c;
}

static void make_textures(void) {
  canvas_t c = cv_new(96, 24);
  cv_tape(&c, 0, 0, 96, 24, 12);
  tex_tape = cv_texture(&c, 1, 1);
  mat_tape = t3_mesh_basic_material_new(0xffffff);
  t3_material_set_map(mat_tape, tex_tape);
  /* pegboard: bone board with a 1 u hole grid */
  c = cv_new(512, 128);
  cv_rect(&c, 0, 0, 512, 128, 0xd8cfbd, 255);
  for (int y = 8; y < 128; y += 16) for (int x = 8; x < 512; x += 16) cv_circle(&c, x, y, 2, 0x6b6357, 255);
  /* tool silhouettes hung on it */
  for (int k = 0; k < 10; k++) {
    int x = 24 + k * 48, h = 40 + (k * 37) % 50;
    cv_rect(&c, x, 20, 6, h, 0x3a342b, 255);
    if (k & 1) cv_rect(&c, x - 8, 20 + h - 14, 22, 14, 0x3a342b, 255); else cv_circle(&c, x + 3, 20 + h, 9, 0x4a4338, 255);
    if (k % 3 == 0) cv_rect(&c, x + 1, 22, 4, 10, C_RUST, 255);
  }
  tex_pegboard = cv_texture(&c, 1, 1);
  /* the honeycomb bed */
  c = cv_new(256, 256);
  cv_rect(&c, 0, 0, 256, 256, 0x8a857c, 255);
  for (int y = 0; y < 256; y += 20) for (int x = (y / 20 & 1) * 10; x < 256; x += 20) cv_circle(&c, x + 10, y + 10, 7, 0x3a352e, 255);
  tex_honey = cv_texture(&c, 1, 1);
  /* seat digits and the OPS label through the HUD (hud_init ran before world_init) */
  hud_clear();
  const char *lab[5] = { "1", "2", "3", "4", S_OPS };
  for (int i = 0; i < 5; i++) {
    hud_rect(i * 128, 0, 128, 128, C_TAPE, 255);
    hud_text(i < 4 ? F_TITLE : F_H1, i * 128 + 64, i < 4 ? -6 : 30, lab[i], i < 4 ? C_BONE : C_HAZARD, 255, AL_CENTER);
    canvas_t d = hud_capture(i * 128, 0, 128, 128);
    tex_digit[i] = cv_texture(&d, 1, 0);
    mat_digit[i] = t3_mesh_basic_material_new(0xffffff);
    t3_material_set_map(mat_digit[i], tex_digit[i]);
  }
  hud_clear();
  /* the sign board: amber, a tape header, three char bars and a tag shape, no text */
  c = cv_new(256, 128);
  cv_rect(&c, 0, 0, 256, 128, C_HAZARD, 255);
  cv_tape(&c, 0, 0, 256, 20, 10);
  for (int k = 0; k < 3; k++) cv_rect(&c, 24, 36 + k * 24, 120 - k * 30, 10, C_TAPE, 255);
  cv_rect(&c, 170, 40, 60, 44, C_RUST, 255); cv_rect(&c, 170, 40, 14, 14, C_HAZARD, 255); cv_circle(&c, 214, 56, 5, C_BONE, 255);
  tex_sign = cv_texture(&c, 1, 0);
}

/* -- the painted floors (6.4) ------------------------------------- */
/* row 0 of the canvas is the back wall (z = -d/2); the data texture is flipped so v = 1 is row 0 */
typedef struct { canvas_t c; const arena_def_t *a; float ppx, ppz; } floorpaint_t;
static int fpx(const floorpaint_t *f, float x) { return (int)((x + f->a->w * 0.5f) * f->ppx); }
static int fpz(const floorpaint_t *f, float z) { return (int)((z + f->a->d * 0.5f) * f->ppz); }
static void fp_rect(floorpaint_t *f, float x0, float z0, float x1, float z1, uint32_t rgb, int a) {
  cv_rect(&f->c, fpx(f, x0), fpz(f, z0), fpx(f, x1) - fpx(f, x0), fpz(f, z1) - fpz(f, z0), rgb, a);
}
static void fp_tape_border(floorpaint_t *f, float x0, float z0, float x1, float z1, float t) {
  int X0 = fpx(f, x0), Z0 = fpz(f, z0), X1 = fpx(f, x1), Z1 = fpz(f, z1), T = (int)(t * f->ppx); if (T < 3) T = 3;
  int pitch = T > 8 ? T / 2 : 4;
  cv_tape(&f->c, X0, Z0, X1 - X0, T, pitch); cv_tape(&f->c, X0, Z1 - T, X1 - X0, T, pitch);
  cv_tape(&f->c, X0, Z0, T, Z1 - Z0, pitch); cv_tape(&f->c, X1 - T, Z0, T, Z1 - Z0, pitch);
}
static void fp_tiles(floorpaint_t *f, uint32_t base, uint32_t grout, uint32_t speck, uint32_t *rng) {
  float pitch = 0.6f * f->ppx, pz = 0.6f * f->ppz;
  int nx = (int)(f->a->w / 0.6f) + 1, nz = (int)(f->a->d / 0.6f) + 1;
  cv_rect(&f->c, 0, 0, f->c.w, f->c.h, base, 255);
  for (int ty = 0; ty < nz; ty++) for (int tx = 0; tx < nx; tx++) {
    int x = (int)(tx * pitch), y = (int)(ty * pz), w = (int)((tx + 1) * pitch) - x, h = (int)((ty + 1) * pz) - y;
    cv_rect(&f->c, x, y, w, 1, grout, 255); cv_rect(&f->c, x, y, 1, h, grout, 255);
    for (int k = 0; k < 6; k++) cv_rect(&f->c, x + 2 + rtr_rand_n(rng, w > 4 ? w - 4 : 1), y + 2 + rtr_rand_n(rng, h > 4 ? h - 4 : 1), 2, 2, speck, 255);
  }
}
static void fp_tile_rects(floorpaint_t *f) {
  float pitch = 0.6f * f->ppx, pz = 0.6f * f->ppz;
  for (int i = 0; i < f->a->ntiles; i++) {
    const tile_rect_t *t = &f->a->tiles[i];
    cv_rect(&f->c, (int)(t->x * pitch) + 1, (int)(t->y * pz) + 1, (int)(t->w * pitch) - 1, (int)(t->h * pz) - 1, t->colour == TILE_RED ? 0xb23a2e : 0xd9672b, 255);
  }
}
/* a one-tile amber ring around a hazard footprint */
static void fp_amber_ring(floorpaint_t *f, float x, float z, float w, float d) {
  int X0 = fpx(f, x - w / 2 - 0.6f), Z0 = fpz(f, z - d / 2 - 0.6f), X1 = fpx(f, x + w / 2 + 0.6f), Z1 = fpz(f, z + d / 2 + 0.6f);
  int IX0 = fpx(f, x - w / 2), IZ0 = fpz(f, z - d / 2), IX1 = fpx(f, x + w / 2), IZ1 = fpz(f, z + d / 2);
  cv_rect(&f->c, X0, Z0, X1 - X0, IZ0 - Z0, C_HAZARD, 230); cv_rect(&f->c, X0, IZ1, X1 - X0, Z1 - IZ1, C_HAZARD, 230);
  cv_rect(&f->c, X0, IZ0, IX0 - X0, IZ1 - IZ0, C_HAZARD, 230); cv_rect(&f->c, IX1, IZ0, X1 - IX1, IZ1 - IZ0, C_HAZARD, 230);
}
static void fp_concrete(floorpaint_t *f, uint32_t base, int grain, uint32_t *rng) {
  int r0 = (base >> 16) & 255, g0 = (base >> 8) & 255, b0 = base & 255;
  for (int y = 0; y < f->c.h; y++) for (int x = 0; x < f->c.w; x++) {
    int n = (int)(rtr_rand(rng) % (uint32_t)(grain * 2 + 1)) - grain;
    float m = sinf(x * 0.011f) * sinf(y * 0.013f + 1.1f) * grain;
    int r = r0 + n + (int)m, g = g0 + n + (int)m, b = b0 + n + (int)m;
    uint8_t *p = f->c.px + ((size_t)y * f->c.w + x) * 4;
    p[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r); p[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g); p[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b); p[3] = 255;
  }
}

void paint_floor(const arena_def_t *a, int w, int h, uint8_t *rgba) {
  floorpaint_t f; f.c.px = rgba; f.c.w = w; f.c.h = h; f.a = a; f.ppx = w / a->w; f.ppz = h / a->d;
  uint32_t rng = 0x51ed27u ^ (uint32_t)(a->w * 7 + a->d);
  float hw = a->w / 2, hd = a->d / 2;
  switch (a->floor_style) {
    case FL_VCT: case FL_SHOP: case FL_GARAGE:
      fp_tiles(&f, 0xd9d3c7, 0x9c968a, 0xcbc4b7, &rng);
      fp_tile_rects(&f);
      if (a->floor_style == FL_SHOP) {
        /* the painted aisle grid and the tape round the electrical panel on the back wall */
        for (float x = -hw + 4; x < hw; x += 4) fp_rect(&f, x - 0.05f, -hd, x + 0.05f, hd, 0xb9b2a4, 255);
        for (float z = -hd + 4; z < hd; z += 4) fp_rect(&f, -hw, z - 0.05f, hw, z + 0.05f, 0xb9b2a4, 255);
        fp_tape_border(&f, -1.6f, -hd, 1.6f, -hd + 1.4f, 0.18f);
      }
      break;
    case FL_DARK:
      fp_concrete(&f, 0x8a857c, 6, &rng);
      fp_tile_rects(&f);
      break;
    case FL_CONCRETE:
      fp_concrete(&f, 0x9c968a, 7, &rng);
      fp_tile_rects(&f);
      /* cardboard scuffs */
      for (int k = 0; k < 60; k++) { int x = rtr_rand_n(&rng, w), y = rtr_rand_n(&rng, h); cv_rect(&f.c, x, y, 6 + rtr_rand_n(&rng, 30), 2 + rtr_rand_n(&rng, 3), 0xa0773f, 70); }
      break;
    case FL_WET:
      fp_concrete(&f, 0x4f4d4b, 5, &rng);
      for (int k = 0; k < 18; k++) {
        int x = rtr_rand_n(&rng, w), y = rtr_rand_n(&rng, h), r = 12 + rtr_rand_n(&rng, 40);
        cv_circle(&f.c, x, y, r, 0x6b7a86, 90); cv_circle(&f.c, x + r / 3, y - r / 4, r / 2, 0x7d8c98, 70);
      }
      fp_tile_rects(&f);
      break;
    case FL_ROOF: default:
      fp_concrete(&f, 0x4a4338, 4, &rng);
      for (float x = -hw + 3; x < hw; x += 3) fp_rect(&f, x - 0.04f, -hd, x + 0.04f, hd, 0xd8d2c6, 200);
      for (float z = -hd + 3; z < hd; z += 3) fp_rect(&f, -hw, z - 0.04f, hw, z + 0.04f, 0xd8d2c6, 200);
      fp_rect(&f, -3, -3, 3, 3, 0xd9672b, 255);
      fp_tile_rects(&f);
      /* hazard tape along the open edges */
      if (a->open_mask & OPEN_PX) cv_tape(&f.c, fpx(&f, hw - 0.45f), 0, w - fpx(&f, hw - 0.45f), h, 8);
      if (a->open_mask & OPEN_NX) cv_tape(&f.c, 0, 0, fpx(&f, -hw + 0.45f), h, 8);
      if (a->open_mask & OPEN_PZ) cv_tape(&f.c, 0, fpz(&f, hd - 0.45f), w, h - fpz(&f, hd - 0.45f), 8);
      if (a->open_mask & OPEN_NZ) cv_tape(&f.c, 0, 0, w, fpz(&f, -hd + 0.45f), 8);
      break;
  }
  /* hazards: amber ring round every killsaw, flame grate and floor flipper; tape round the pit;
   * a dashed square on a drop zone; the hill points faintly */
  for (int i = 0; i < a->nhaz; i++) {
    const hazard_def_t *hz = &a->haz[i];
    switch (hz->type) {
      case HZ_KILLSAW: case HZ_FLAME: case HZ_FLOORFLIP: fp_amber_ring(&f, hz->x, hz->z, hz->w, hz->d); break;
      case HZ_PIT: fp_tape_border(&f, hz->x - hz->w / 2 - 0.35f, hz->z - hz->d / 2 - 0.35f, hz->x + hz->w / 2 + 0.35f, hz->z + hz->d / 2 + 0.35f, 0.3f); break;
      case HZ_DROPZONE: {
        int X0 = fpx(&f, hz->x - hz->w / 2), Z0 = fpz(&f, hz->z - hz->d / 2), X1 = fpx(&f, hz->x + hz->w / 2), Z1 = fpz(&f, hz->z + hz->d / 2);
        for (int k = 0; k < 8; k++) { int t = (k * (X1 - X0)) / 8; if (k & 1) continue; cv_rect(&f.c, X0 + t, Z0, (X1 - X0) / 8, 4, C_HAZARD, 255); cv_rect(&f.c, X0 + t, Z1 - 4, (X1 - X0) / 8, 4, C_HAZARD, 255); cv_rect(&f.c, X0, Z0 + t, 4, (X1 - X0) / 8, C_HAZARD, 255); cv_rect(&f.c, X1 - 4, Z0 + t, 4, (X1 - X0) / 8, C_HAZARD, 255); }
        cv_circle(&f.c, (X0 + X1) / 2, (Z0 + Z1) / 2, (X1 - X0) / 6, C_HAZARD, 120);
        break; }
      case HZ_DECK: fp_rect(&f, hz->x - hz->w / 2, hz->z + hz->d / 2, hz->x + hz->w / 2, hz->z + hz->d / 2 + 0.3f, 0xb23a2e, 255); break;
      default: break;
    }
  }
  /* the OPS zone gets tape; spawn corners a 2 x 2 block in the seat colour at 40 percent */
  if (a->floor_style != FL_GARAGE) {
    fp_tape_border(&f, a->ops_zone[0] - 1, a->ops_zone[1] - 1, a->ops_zone[0] + 1, a->ops_zone[1] + 1, 0.2f);
    for (int s = 0; s < 4; s++) {
      float x = a->spawn4[s][0], z = a->spawn4[s][1];
      if (x == 0 && z == 0) continue;
      fp_rect(&f, x - 1, z - 1, x + 1, z + 1, SEAT_RGB[s], 100);
    }
  }
  /* shadows pooling against the closed walls */
  for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
    float wx = x / f.ppx - hw, wz = y / f.ppz - hd, dmin = 9;
    if (!(a->open_mask & OPEN_NX)) dmin = fminf(dmin, wx + hw);
    if (!(a->open_mask & OPEN_PX)) dmin = fminf(dmin, hw - wx);
    if (!(a->open_mask & OPEN_NZ)) dmin = fminf(dmin, wz + hd);
    if (!(a->open_mask & OPEN_PZ)) dmin = fminf(dmin, hd - wz);
    if (dmin < 0.8f) { uint8_t *p = f.c.px + ((size_t)y * w + x) * 4; int k = (int)((0.8f - dmin) / 0.8f * 60); for (int c = 0; c < 3; c++) p[c] = (uint8_t)(p[c] * (255 - k) / 255); }
  }
}

/* -- arenas ------------------------------------------------------- */
typedef struct {
  int type; float ang, dir_x, dir_z;
  t3_object *g;                 /* the whole hazard, at (x, floor, z) */
  t3_object *mov;               /* the moving part (arm, doors, saws, plate, beam) */
  t3_object *mov2;
  t3_material *tell_m;          /* emissive material of the tell mesh, pulsed at 2 Hz */
  t3_material *glow_m;          /* transparent material (flame cone, laser beam, hill) */
  float lamp;                   /* bumper lamp flash timer */
  float swing;                  /* pulverizer and flipper animation state */
  float spin;                   /* killsaw and screw rotation */
} whaz_t;

typedef struct {
  t3_object *root;
  const arena_def_t *def;
  t3_texture *floor_tex;
  t3_object *cells[16];
  whaz_t haz[MAX_HAZ];
  t3_object *props[32]; t3_mesh *flags[32];
  t3_object *gate; float gate_open;
  t3_material *rail_m;
  t3_instanced_mesh *crowd[3];
  t3_mesh *hill;
  int built, floor_w, floor_h, scorch_frame;
} warena_t;

static warena_t arenas[6];
static warena_t garage;          /* the garage bay uses the same struct for its floor and lights */
static t3_object *garage_root, *turntables[4];
static float tt_angle[4], tt_pause[4]; static int tt_locked[4];

static void arena_floor(warena_t *w, const arena_def_t *a) {
  int W = 1024, H = (int)(1024.0f * a->d / a->w + 0.5f);
  uint8_t *px = malloc((size_t)W * H * 4);
  paint_floor(a, W, H, px);
  /* the rows are swapped here instead of flip_y: three.c's flip is a copy of the whole texture at
   * its first upload, a 4 MB heap step in the first countdown of every arena */
  for (int y = 0; y < H / 2; y++) {
    uint8_t row[4096]; uint8_t *lo = px + (size_t)y * W * 4, *hi = px + (size_t)(H - 1 - y) * W * 4;
    memcpy(row, lo, (size_t)W * 4); memcpy(lo, hi, (size_t)W * 4); memcpy(hi, row, (size_t)W * 4);
  }
  t3_texture *t = t3_data_texture_new(W, H, px);
  free(px);
  t->color_space = T3_SRGB_COLOR_SPACE;
  t->mag_filter = T3_LINEAR; t->min_filter = T3_LINEAR;
  w->floor_tex = t; w->floor_w = W; w->floor_h = H;
  t3_material *m = t3_mesh_standard_material_new(0xffffff);
  m->roughness = a->floor_style == FL_WET ? 0.25f : 0.85f; m->metalness = 0;
  t3_material_set_map(m, t);
  t3_mesh *fl = t3_mesh_new(t3_plane_geometry_new(a->w, a->d, 1, 1), m);
  t3_object_set_rotation(fl, -PI / 2, 0, 0);
  t3_object_add(w->root, fl);
  t3_release(fl); t3_release(m);
}

/* a caution tape rail: a char rail with a tape face on top */
static void tape_rail(void *parent, t3_material *rail, float len, float x, float y, float z, int along_z) {
  part(parent, g_box, rail ? rail : M_PAINT(C_TAPE), along_z ? 0.14f : len, 0.12f, along_z ? len : 0.14f, x, y, z);
  t3_mesh *t = t3_mesh_new(g_plane, mat_tape);
  t3_object_set_scale(t, along_z ? 0.14f : len, along_z ? len : 0.14f, 1);
  t3_object_set_rotation(t, -PI / 2, 0, 0);       /* the scale already orients it */
  t3_object_set_position(t, x, y + 0.061f, z);
  t3_object_add(parent, t); t3_release(t);
}

static void arena_walls(warena_t *w, const arena_def_t *a) {
  float hw = a->w / 2, hd = a->d / 2, H = 1.1f;
  int roof = a->floor_style == FL_ROOF;
  struct { int open; float x, z, len; int along_z; float nx, nz; } side[4] = {
    { a->open_mask & OPEN_PX, hw, 0, a->d, 1, -1, 0 }, { a->open_mask & OPEN_NX, -hw, 0, a->d, 1, 1, 0 },
    { a->open_mask & OPEN_PZ, 0, hd, a->w, 0, 0, -1 }, { a->open_mask & OPEN_NZ, 0, -hd, a->w, 0, 0, 1 } };
  w->rail_m = mat_ex(C_TAPE, 0.6f, 0.2f, C_RUST, 0.0f, 0);
  for (int s = 0; s < 4; s++) {
    if (side[s].open) continue;
    float x = side[s].x, z = side[s].z, len = side[s].len; int az = side[s].along_z;
    if (roof) {
      /* a brick parapet instead of lexan */
      BOX(w->root, M_PLY(0x5a4a40), az ? 0.5f : len + 0.5f, 0.9f, az ? len + 0.5f : 0.5f, x, 0.45f, z);
      BOX(w->root, M_PLY(0x8a857c), az ? 0.6f : len + 0.6f, 0.08f, az ? len + 0.6f : 0.6f, x, 0.94f, z);
      continue;
    }
    /* lexan panel */
    t3_mesh *lx = t3_mesh_new(g_plane, lexan_mat());
    t3_object_set_scale(lx, len, H, 1);
    t3_object_set_rotation(lx, 0, az ? (side[s].nx > 0 ? PI / 2 : -PI / 2) : (side[s].nz > 0 ? 0 : PI), 0);
    t3_object_set_position(lx, x, H / 2, z);
    lx->materials[0]->side = T3_DOUBLE_SIDE;
    t3_object_add(w->root, lx); t3_release(lx);
    /* base rail and posts */
    BOX(w->root, M_PAINT(C_TAPE), az ? 0.16f : len + 0.16f, 0.1f, az ? len + 0.16f : 0.16f, x, 0.05f, z);
    tape_rail(w->root, w->rail_m, len + 0.16f, x, H + 0.06f, z, az);
    for (float p = -len / 2; p <= len / 2 + 0.01f; p += 4) {
      float px = az ? x : p, pz = az ? p : z;
      BOX(w->root, M_STEEL(C_STEEL_HI), 0.12f, H + 0.12f, 0.12f, px, (H + 0.12f) / 2, pz);
    }
  }
  /* the surround: an outer concrete floor and dark shop walls with strip lights, or a skyline on the roof */
  t3_mesh *outer = part(w->root, g_plane, M_RUBBER(0x24201b), a->w * 3, a->d * 3, 1, 0, -0.02f, 0);
  rot(outer, -PI / 2, 0, 0);
  if (roof) {
    for (int k = 0; k < 14; k++) {
      float ang = k * 0.45f, r = 22 + (k % 3) * 6, h = 4 + (k * 7) % 9;
      t3_mesh *b = BOX(w->root, M_PAINT(0x1d1915), 4 + (k % 4), h, 4 + (k % 3), cosf(ang) * r, h / 2 - 1.5f, sinf(ang) * r);
      (void)b;
      for (int wy = 0; wy < 3; wy++) part(w->root, g_box, emis(C_HAZARD, 1.5f), 0.3f, 0.3f, 0.3f, cosf(ang) * r + (k % 3) - 1, wy * 1.6f + 0.5f, sinf(ang) * r - 2 - (k % 3) * 0.5f);
    }
  } else {
    for (int s = 0; s < 4; s++) {
      if (side[s].open) continue;
      float dist = 4.2f;
      float x = side[s].x + side[s].nx * -dist, z = side[s].z + side[s].nz * -dist, len = side[s].len + 2 * dist; int az = side[s].along_z;
      BOX(w->root, M_PLY(0x1d1915), az ? 0.4f : len, 5, az ? len : 0.4f, x, 2.5f, z);
      for (float p = -len / 2 + 3; p < len / 2 - 1; p += 6)
        part(w->root, g_box, emis(a->floor_style == FL_DARK ? C_LIVE : 0xfff1d6, 1.8f), az ? 0.1f : 2.4f, 0.08f, az ? 2.4f : 0.1f, az ? x + side[s].nx * 0.25f : p, 3.6f, az ? p : z + side[s].nz * 0.25f);
    }
  }
}

static void prop_box_group(t3_object *parent, const prop_def_t *p, t3_material *m) { BOX(parent, m, p->w, p->h, p->d, 0, p->h / 2, 0); }

static t3_object *build_prop(warena_t *w, const prop_def_t *p, int idx) {
  t3_object *g = t3_group_new();
  t3_object_set_position(g, p->x, 0, p->z);
  t3_object_set_rotation(g, 0, p->ang, 0);
  float h = p->h > 0 ? p->h : 0.4f;
  switch (p->kind) {
    case PR_BENCH: {
      BOX(g, M_PLY(0xa57c4f), p->w, 0.08f, p->d, 0, h - 0.04f, 0);
      BOX(g, M_STEEL(C_STEEL), 0.08f, h - 0.08f, p->d * 0.8f, -p->w / 2 + 0.1f, (h - 0.08f) / 2, 0);
      BOX(g, M_STEEL(C_STEEL), 0.08f, h - 0.08f, p->d * 0.8f, p->w / 2 - 0.1f, (h - 0.08f) / 2, 0);
      /* a vice and a mug */
      BOX(g, M_STEEL(C_STEEL_HI), 0.22f, 0.16f, 0.14f, p->w * 0.25f, h + 0.08f, 0);
      CYL(g, M_PAINT(C_BONE), 0.06f, 0.1f, -p->w * 0.3f, h + 0.05f, 0.1f);
      break; }
    case PR_HVAC: {
      BOX(g, M_STEEL(0x8a857c), p->w, h, p->d, 0, h / 2, 0);
      part(g, g_disc, M_PAINT(C_TAPE), p->w * 0.35f, 0.04f, p->w * 0.35f, 0, h + 0.02f, 0);
      for (int k = 0; k < 4; k++) BOX(g, M_PAINT(C_STEEL), p->w * 0.6f, 0.03f, 0.03f, 0, h * 0.3f + k * 0.12f, p->d / 2 + 0.01f);
      BOX(g, M_STEEL(C_STEEL_HI), 0.05f, 1.6f, 0.05f, p->w / 2 - 0.2f, h + 0.8f, p->d / 2 - 0.2f);
      t3_mesh *flag = part(g, g_plane, M_PAINT(0xd9672b), 0.7f, 0.35f, 1, p->w / 2 - 0.2f + 0.35f, h + 1.4f, p->d / 2 - 0.2f);
      flag->materials[0]->side = T3_DOUBLE_SIDE;
      w->flags[idx] = flag;
      break; }
    case PR_BOX: {
      uint32_t c = p->colour ? p->colour : 0xa0773f;
      BOX(g, M_PLY(c), p->w, h, p->d, 0, h / 2, 0);
      BOX(g, M_PLY(0xc9a271), p->w * 0.3f, h + 0.01f, p->d + 0.01f, 0, h / 2, 0);
      break; }
    case PR_SHELF: {
      for (int k = 0; k < 4; k++) BOX(g, M_STEEL(C_STEEL), 0.06f, h, 0.06f, (k & 1 ? 1 : -1) * (p->w / 2 - 0.03f), h / 2, (k & 2 ? 1 : -1) * (p->d / 2 - 0.03f));
      for (int s = 0; s < 3; s++) {
        float y = 0.3f + s * (h - 0.4f) / 2;
        BOX(g, M_PLY(0xc8a165), p->w, 0.04f, p->d, 0, y, 0);
        for (int k = 0; k < 3; k++) {
          uint32_t cs[5] = { 0xa0773f, C_RUST, C_INFO, 0x2f8562, C_BONE };
          float bw = p->w / 3 - 0.1f;
          BOX(g, M_PLY(cs[(s + k + idx) % 5]), bw, 0.25f + 0.1f * ((s + k) % 2), p->d * 0.7f, -p->w / 2 + 0.05f + bw / 2 + k * p->w / 3, y + 0.02f + (0.25f + 0.1f * ((s + k) % 2)) / 2, 0);
        }
      }
      break; }
    case PR_SIGN: {
      BOX(g, M_STEEL(C_STEEL), 0.08f, h, 0.08f, -p->w / 2 + 0.1f, h / 2, 0);
      BOX(g, M_STEEL(C_STEEL), 0.08f, h, 0.08f, p->w / 2 - 0.1f, h / 2, 0);
      t3_material *sm = t3_mesh_basic_material_new(0xffffff); t3_material_set_map(sm, tex_sign); sm->side = T3_DOUBLE_SIDE;
      t3_mesh *b = t3_mesh_new(g_plane, sm); t3_object_set_scale(b, p->w, p->w * 0.5f, 1); t3_object_set_position(b, 0, h - p->w * 0.25f, 0);
      t3_object_add(g, b); t3_release(b); t3_release(sm);
      break; }
    case PR_SERVER: {
      BOX(g, M_PAINT(0x1d1915), p->w, h, p->d, 0, h / 2, 0);
      for (int k = 0; k < 6; k++) {
        BOX(g, M_PAINT(C_STEEL), p->w * 0.8f, 0.06f, 0.02f, 0, 0.2f + k * (h - 0.4f) / 5, p->d / 2 + 0.01f);
        part(g, g_box, emis(k % 2 ? C_LIVE : C_HAZARD, 2.0f), 0.04f, 0.04f, 0.02f, p->w * 0.3f, 0.2f + k * (h - 0.4f) / 5 + 0.08f, p->d / 2 + 0.02f);
      }
      break; }
    case PR_GATE: break;
    case PR_WALL: default:
      prop_box_group(g, p, M_PAINT(p->colour ? p->colour : C_STEEL));
      break;
  }
  return g;
}

/* the hazard's reach direction: into the arena from the nearest wall */
static void inward(const arena_def_t *a, float x, float z, float *dx, float *dz) {
  float nx = 0, nz = 1;
  arena_nearest_wall(a, x, z, &nx, &nz, 0);
  *dx = nx; *dz = nz;
}

static void build_hazard(warena_t *w, const arena_def_t *a, int i) {
  const hazard_def_t *h = &a->haz[i];
  whaz_t *hz = &w->haz[i];
  memset(hz, 0, sizeof *hz);
  hz->type = h->type; hz->ang = h->ang;
  t3_object *g = t3_group_new();
  t3_object_set_position(g, h->x, 0, h->z);
  t3_object_add(w->root, g); t3_release(g);
  hz->g = g;
  switch (h->type) {
    case HZ_PIT: {
      part(g, g_plane, flat(0x050403), h->w, h->d, 1, 0, 0.006f, 0);
      rot((t3_mesh *)g->children[0], -PI / 2, 0, 0);
      hz->mov = group_at(g, 0, 0, 0);
      BOX(hz->mov, M_STEEL(0x6f6c66), h->w / 2 - 0.02f, 0.06f, h->d, -h->w / 4, 0.03f, 0);
      hz->mov2 = group_at(g, 0, 0, 0);
      BOX(hz->mov2, M_STEEL(0x6f6c66), h->w / 2 - 0.02f, 0.06f, h->d, h->w / 4, 0.03f, 0);
      hz->tell_m = mat_ex(C_HAZARD, 0.5f, 0, C_HAZARD, 0.3f, 0);
      for (int k = 0; k < 4; k++) part(g, g_box, hz->tell_m, 0.16f, 0.06f, 0.16f, (k & 1 ? 1 : -1) * (h->w / 2 + 0.25f), 0.03f, (k & 2 ? 1 : -1) * (h->d / 2 + 0.25f));
      break; }
    case HZ_PULVERIZER: {
      inward(a, h->x, h->z, &hz->dir_x, &hz->dir_z);
      float yaw = atan2f(hz->dir_x, hz->dir_z);
      t3_object_set_rotation(g, 0, yaw, 0);
      /* the post stands at the wall, 1.4 behind the strike square; the arm pivots at its top */
      BOX(g, M_STEEL(C_STEEL_HI), 0.3f, 2.2f, 0.3f, 0, 1.1f, -1.5f);
      BOX(g, M_PAINT(C_TAPE), 0.5f, 0.2f, 0.5f, 0, 2.25f, -1.5f);
      hz->tell_m = mat_ex(C_LIVE, 0.5f, 0, C_LIVE, 0.0f, 0);
      part(g, g_box, hz->tell_m, 0.2f, 0.2f, 0.2f, 0, 2.45f, -1.5f);
      hz->mov = group_at(g, 0, 2.0f, -1.5f);
      BOX(hz->mov, M_STEEL(0x6f6c66), 0.18f, 0.18f, 1.7f, 0, 0, 0.85f);
      BOX(hz->mov, M_PAINT(C_RUST), 0.7f, 0.55f, 0.7f, 0, -0.1f, 1.7f);
      BOX(hz->mov, M_PAINT(C_HAZARD), 0.72f, 0.1f, 0.72f, 0, -0.1f, 1.7f);
      hz->swing = -1.1f;
      t3_object_set_rotation(hz->mov, hz->swing, 0, 0);
      break; }
    case HZ_KILLSAW: {
      t3_object_set_rotation(g, 0, h->ang, 0);
      float L = h->w > h->d ? h->w : h->d, Wd = h->w > h->d ? h->d : h->w;
      BOX(g, M_STEEL(C_STEEL), L + 0.1f, 0.04f, 0.06f, 0, 0.02f, Wd / 2);
      BOX(g, M_STEEL(C_STEEL), L + 0.1f, 0.04f, 0.06f, 0, 0.02f, -Wd / 2);
      hz->tell_m = mat_ex(0x2a241e, 0.6f, 0, C_HAZARD, 0.0f, 0);
      part(g, g_box, hz->tell_m, L, 0.02f, Wd - 0.08f, 0, 0.005f, 0);
      hz->mov = group_at(g, 0, -0.42f, 0);
      for (int k = 0; k < 2; k++) {
        t3_object *saw = group_at(hz->mov, (k ? 1 : -1) * L / 4, 0, 0);
        t3_mesh *d = part(saw, g_cyl12, M_STEEL(0xcfd3d6), 0.36f, 0.05f, 0.36f, 0, 0, 0);
        rot(d, 0, 0, PI / 2);
        for (int t = 0; t < 8; t++) { t3_mesh *th = part(saw, g_box, M_STEEL(0xe6e2d8), 0.04f, 0.12f, 0.1f, 0, cosf(t * PI / 4) * 0.4f, sinf(t * PI / 4) * 0.4f); rot(th, -t * PI / 4, 0, 0); }
      }
      break; }
    case HZ_FLAME: {
      BOX(g, M_STEEL(C_STEEL), h->w, 0.08f, h->d, 0, 0.04f, 0);
      hz->tell_m = mat_ex(0x1d1915, 0.6f, 0, C_HAZARD, 0.0f, 0);
      for (int k = 0; k < 4; k++) part(g, g_box, hz->tell_m, h->w * 0.8f, 0.02f, h->d * 0.12f, 0, 0.085f, -h->d * 0.3f + k * h->d * 0.2f);
      hz->glow_m = glass(0xffb347, 0.0f);
      hz->mov = group_at(g, 0, 0.1f, 0);
      t3_mesh *cone = t3_mesh_new(g_cone, hz->glow_m);
      t3_object_set_scale(cone, h->w * 0.45f, 1.4f, h->d * 0.45f); t3_object_set_position(cone, 0, 0.7f, 0); t3_object_set_rotation(cone, PI, 0, 0);
      t3_object_add(hz->mov, cone); t3_release(cone);
      t3_mesh *core = t3_mesh_new(g_cone, glass(0xfff1d6, 0.0f));
      t3_object_set_scale(core, h->w * 0.22f, 0.9f, h->d * 0.22f); t3_object_set_position(core, 0, 0.45f, 0); t3_object_set_rotation(core, PI, 0, 0);
      t3_object_add(hz->mov, core); t3_release(core);
      hz->mov2 = (t3_object *)core;
      break; }
    case HZ_SCREW: {
      t3_object_set_rotation(g, 0, h->ang, 0);
      float L = h->w > h->d ? h->w : h->d;
      hz->mov = group_at(g, 0, 0.36f, 0);
      t3_mesh *c = part(hz->mov, g_cyl12, M_STEEL(0x6f6c66), 0.3f, L, 0.3f, 0, 0, 0);
      rot(c, 0, 0, PI / 2);
      for (int k = 0; k < 10; k++) { t3_mesh *fin = part(hz->mov, g_box, M_STEEL(C_RUST), 0.08f, 0.86f, 0.1f, -L / 2 + 0.25f + k * (L - 0.5f) / 9, 0, 0); rot(fin, k * 0.63f, 0, 0); }
      BOX(g, M_STEEL(C_STEEL), L + 0.2f, 0.1f, 0.1f, 0, 0.05f, -0.42f);
      BOX(g, M_STEEL(C_STEEL), L + 0.2f, 0.1f, 0.1f, 0, 0.05f, 0.42f);
      break; }
    case HZ_RAMP: {
      t3_object_set_rotation(g, 0, h->ang, 0);
      if (h->h <= 0.25f) {
        /* the honeycomb bed: a lip */
        t3_material *m = t3_mesh_standard_material_new(0xffffff); m->roughness = 0.6f; m->metalness = 0.5f; t3_material_set_map(m, tex_honey);
        t3_mesh *b = t3_mesh_new(g_box, m); t3_object_set_scale(b, h->w, h->h, h->d); t3_object_set_position(b, 0, h->h / 2, 0);
        t3_object_add(g, b); t3_release(b); t3_release(m);
        BOX(g, M_STEEL(C_STEEL_HI), h->w + 0.08f, 0.04f, h->d + 0.08f, 0, h->h - 0.03f, 0);   /* top just under the bed: no z fight */
      } else {
        t3_geometry *wg = wedge_geometry(h->w, h->d, h->h);
        t3_mesh *m = t3_mesh_new(wg, M_STEEL(0x6f6c66)); t3_object_set_rotation(m, 0, PI, 0);
        t3_object_add(g, m); t3_release(m); t3_release(wg);
        for (int k = 0; k < 5; k++) BOX(g, M_PAINT(C_HAZARD), h->w, 0.012f, 0.08f, 0, h->h * (0.9f - k * 0.2f) + 0.01f, h->d / 2 - h->d * (0.1f + k * 0.2f));
      }
      break; }
    case HZ_BUMPER: {
      float H = h->p0 >= 5 ? h->p0 / 10.0f : h->p0 > 0 ? (float)h->p0 : 1.0f;   /* p0: tenths of a unit at 5 and up, whole units below */
      CYL(g, M_PAINT(C_TAPE), 0.4f, H, 0, H / 2, 0);
      CYL(g, M_PAINT(C_HAZARD), 0.42f, 0.22f, 0, H * 0.55f, 0);
      hz->tell_m = mat_ex(C_HAZARD, 0.4f, 0, C_HAZARD, 0.2f, 0);
      part(g, g_sphere, hz->tell_m, 0.16f, 0.16f, 0.16f, 0, H + 0.12f, 0);
      break; }
    case HZ_DECK: {
      BOX(g, M_STEEL(0x5a5651), h->w, 0.12f, h->d, 0, h->h - 0.06f, 0);
      BOX(g, M_PAINT(C_BONE), h->w, 0.03f, 0.12f, 0, h->h + 0.015f, h->d / 2 - 0.06f);
      for (float px = -h->w / 2 + 1; px < h->w / 2; px += 2) BOX(g, M_STEEL(C_STEEL), 0.12f, h->h - 0.12f, 0.12f, px, (h->h - 0.12f) / 2, h->d / 2 - 0.2f);
      BOX(g, M_PAINT(C_TAPE), h->w, h->h - 0.12f, 0.04f, 0, (h->h - 0.12f) / 2, -h->d / 2 + 0.02f);
      break; }
    case HZ_FLOORFLIP: {
      hz->tell_m = mat_ex(C_LIVE, 0.5f, 0, C_LIVE, 0.0f, 0);
      BOX(g, M_STEEL(C_STEEL), h->w + 0.12f, 0.03f, h->d + 0.12f, 0, 0.015f, 0);
      hz->mov = group_at(g, 0, 0.03f, -h->d / 2);
      BOX(hz->mov, M_PAINT(C_BONE), h->w, 0.06f, h->d, 0, 0, h->d / 2);
      part(hz->mov, g_box, hz->tell_m, h->w, 0.02f, 0.08f, 0, 0.035f, h->d - 0.04f);
      part(hz->mov, g_box, hz->tell_m, 0.08f, 0.02f, h->d, -h->w / 2 + 0.04f, 0.035f, h->d / 2);
      part(hz->mov, g_box, hz->tell_m, 0.08f, 0.02f, h->d, h->w / 2 - 0.04f, 0.035f, h->d / 2);
      break; }
    case HZ_DROPZONE: {
      hz->glow_m = glass(0x000000, 0.0f);
      t3_mesh *sh = t3_mesh_new(g_circle, hz->glow_m); rot(sh, -PI / 2, 0, 0); t3_object_set_position(sh, 0, 0.012f, 0); t3_object_add(g, sh); t3_release(sh);
      hz->mov2 = (t3_object *)sh;
      hz->mov = group_at(g, 0, 6, 0);
      BOX(hz->mov, M_PAINT(0x8b8578), 0.9f, 0.7f, 0.8f, 0, 0.35f, 0);         /* a dead CRT */
      BOX(hz->mov, M_PAINT(0x1a2a2a), 0.7f, 0.5f, 0.05f, 0, 0.38f, 0.41f);
      set_vis(hz->mov, 0);
      break; }
    case HZ_LASER: {
      float hw = a->w / 2;
      hz->glow_m = glass(C_LIVE, 0.0f);
      hz->mov = group_at(g, -hw, 0.3f, 0);
      t3_mesh *beam = t3_mesh_new(g_box, hz->glow_m); t3_object_set_scale(beam, 0.05f, 0.05f, a->d); t3_object_add(hz->mov, beam); t3_release(beam);
      hz->tell_m = mat_ex(C_LIVE, 0.4f, 0, C_LIVE, 0.6f, 0);
      for (int s = -1; s <= 1; s += 2) {
        BOX(g, M_PAINT(C_TAPE), 0.4f, 0.5f, 0.5f, 0, 0.3f, s * (a->d / 2 - 0.3f));
        part(g, g_box, hz->tell_m, 0.2f, 0.1f, 0.1f, 0, 0.3f, s * (a->d / 2 - 0.55f));
        BOX(g, M_STEEL(C_STEEL), a->w, 0.06f, 0.06f, 0, 0.05f, s * (a->d / 2 - 0.3f));
      }
      break; }
    case HZ_HILL: break;
    case HZ_WIND: {
      /* a wind sock on a mast outside the closed corner */
      BOX(g, M_STEEL(C_STEEL_HI), 0.06f, 3, 0.06f, 0, 1.5f, 0);
      hz->mov = group_at(g, 0, 2.9f, 0);
      t3_mesh *sock = part(hz->mov, g_cone, M_PAINT(0xd9672b), 0.18f, 1.0f, 0.18f, 0.5f, 0, 0);
      rot(sock, 0, 0, -PI / 2);
      break; }
    default: break;
  }
}

/* the crowd: silhouettes behind every closed wall, two rows, three frames (14.2) */
static void build_crowd(warena_t *w, const arena_def_t *a) {
  float hw = a->w / 2, hd = a->d / 2;
  static t3_geometry *cg[3];
  if (!cg[0]) for (int f = 0; f < 3; f++) cg[f] = crowd_geometry(f);
  t3_material *cm = t3_mesh_basic_material_new(0xffffff);
  int n = 240;
  for (int f = 0; f < 3; f++) {
    w->crowd[f] = t3_instanced_mesh_new(cg[f], cm, n);
    t3_object_add(w->root, w->crowd[f]); t3_release(w->crowd[f]);
  }
  t3_release(cm);
  int sides = 0;
  int closed[4] = { !(a->open_mask & OPEN_PX), !(a->open_mask & OPEN_NX), !(a->open_mask & OPEN_PZ), !(a->open_mask & OPEN_NZ) };
  for (int s = 0; s < 4; s++) sides += closed[s];
  if (!sides) sides = 1;
  int per_side = n / sides, k = 0;
  uint32_t r = 0xc0ffee;
  for (int s = 0; s < 4 && k < n; s++) {
    if (!closed[s]) continue;
    for (int i = 0; i < per_side && k < n; i++) {
      int row = i & 1;
      float t = (float)(i / 2) / (float)(per_side / 2) - 0.5f + (rtr_rand_f(&r) - 0.5f) * 0.04f;
      float dist = 1.6f + row * 1.1f, y = 0.0f + row * 0.6f, x, z, yaw;
      if (s == 0) { x = hw + dist; z = t * (a->d + 2); yaw = -PI / 2; }
      else if (s == 1) { x = -hw - dist; z = t * (a->d + 2); yaw = PI / 2; }
      else if (s == 2) { x = t * (a->w + 2); z = hd + dist; yaw = PI; }
      else { x = t * (a->w + 2); z = -hd - dist; yaw = 0; }
      t3_mat4 m; t3_vec3 p = { x, y, z }, sc = { 0.9f + rtr_rand_f(&r) * 0.2f, 0.85f + rtr_rand_f(&r) * 0.3f, 1 };
      t3_quat q = t3_quat_from_axis_angle((t3_vec3){ 0, 1, 0 }, yaw + (rtr_rand_f(&r) - 0.5f) * 0.3f);
      t3_mat4_compose(&m, p, q, sc);
      uint32_t shades[5] = { 0x1d1915, 0x26211b, 0x2f2920, 0x3a342b, 0x241f1a };
      t3_color c = t3_color_hex(shades[rtr_rand_n(&r, 5)]);
      for (int f = 0; f < 3; f++) { t3_instanced_mesh_set_matrix_at(w->crowd[f], k, &m); t3_instanced_mesh_set_color_at(w->crowd[f], k, c); }
      k++;
    }
  }
  for (; k < n; k++) { t3_mat4 m; t3_mat4_compose(&m, (t3_vec3){ 0, -10, 0 }, t3_quat_identity(), (t3_vec3){ 0.001f, 0.001f, 0.001f }); for (int f = 0; f < 3; f++) t3_instanced_mesh_set_matrix_at(w->crowd[f], k, &m); }
  for (int f = 0; f < 3; f++) { w->crowd[f]->mesh.base.frustum_culled = false; set_vis(w->crowd[f], f == 0); }
}

static void build_gate(warena_t *w, const arena_def_t *a) {
  float zx = a->ops_zone[0], zz = a->ops_zone[1];
  float sx = zx < 0 ? 1 : -1, sz = zz < 0 ? 1 : -1;      /* toward the centre */
  w->gate = group_at(w->root, zx, 0, zz);
  t3_object *g = w->gate;
  /* two lexan 2 u segments on the inner faces, each with a rail; the face toward the centre in x and in z */
  for (int k = 0; k < 2; k++) {
    t3_object *seg = group_at(g, k == 0 ? sx * 1.0f : 0, 0, k == 0 ? 0 : sz * 1.0f);
    t3_mesh *lx = t3_mesh_new(g_plane, lexan_mat());
    t3_object_set_scale(lx, 2, 1.1f, 1); t3_object_set_position(lx, 0, 0.55f, 0);
    t3_object_set_rotation(lx, 0, k == 0 ? PI / 2 : 0, 0);
    t3_object_add(seg, lx); t3_release(lx);
    tape_rail(seg, w->rail_m, 2.1f, 0, 1.16f, 0, k == 0);
    BOX(seg, M_PAINT(C_TAPE), k == 0 ? 0.14f : 2.1f, 0.1f, k == 0 ? 2.1f : 0.14f, 0, 0.05f, 0);
  }
  /* the zone's taped post */
  BOX(g, M_PAINT(C_TAPE), 0.12f, 1.2f, 0.12f, sx * 1.0f, 0.6f, sz * 1.0f);
}

void world_build_arena(int idx, const arena_def_t *a) {
  if (idx < 0 || idx >= 6) return;
  warena_t *w = &arenas[idx];
  if (w->root) { if (attached_idx == idx) { t3_object_remove(&S->base, w->root); attached_idx = -1; attached = 0; } t3_release(w->root); }
  memset(w, 0, sizeof *w);
  w->root = t3_group_new();
  w->def = a;
  arena_floor(w, a);
  arena_walls(w, a);
  for (int c = 0; c < 16; c++) w->cells[c] = group_at(w->root, 0, 0, 0);
  for (int i = 0; i < a->nprops && i < 32; i++) {
    if (a->props[i].kind == PR_GATE) continue;
    int cell = arena_cell(a, a->props[i].x, a->props[i].z);
    t3_object *g = build_prop(w, &a->props[i], i);
    t3_object_add(w->cells[cell], g); t3_release(g);
    w->props[i] = g;
  }
  for (int i = 0; i < a->nhaz && i < MAX_HAZ; i++) build_hazard(w, a, i);
  build_gate(w, a);
  build_crowd(w, a);
  /* KING OF THE HILL square */
  t3_material *hm = glass(C_LIVE, 0.35f);
  w->hill = t3_mesh_new(g_plane, hm); t3_object_set_scale(w->hill, 3, 3, 1); rot(w->hill, -PI / 2, 0, 0); t3_object_set_position(w->hill, 0, 0.02f, 0);
  t3_object_add(w->root, w->hill); t3_release(w->hill); t3_release(hm);
  set_vis(w->hill, 0);
  w->built = 1;
}

/* -- the garage bay (3.1) ----------------------------------------- */
static void build_garage(void) {
  const arena_def_t *a = &GARAGE_BAY;
  garage_root = t3_group_new();
  warena_t *w = &garage;
  memset(w, 0, sizeof *w);
  w->root = garage_root; w->def = a;
  /* the bay floor is centred on the four turntables: x from -6 to 42 */
  arena_floor(w, a);
  t3_object_set_position(w->root->children[0], 18, 0, 0);
  /* pegboard back wall and a char ceiling line */
  t3_material *pm = t3_mesh_standard_material_new(0xffffff); pm->roughness = 0.9f; pm->metalness = 0; t3_material_set_map(pm, tex_pegboard);
  tex_pegboard->repeat.x = 4; tex_pegboard->repeat.y = 1;
  t3_mesh *peg = t3_mesh_new(g_plane, pm); t3_object_set_scale(peg, 50, 3.2f, 1); t3_object_set_position(peg, 18, 1.6f, -4.9f);
  t3_object_add(garage_root, peg); t3_release(peg); t3_release(pm);
  BOX(garage_root, M_PAINT(C_TAPE), 50, 0.3f, 0.3f, 18, 3.35f, -4.85f);
  BOX(garage_root, M_PAINT(C_TAPE), 50, 0.2f, 0.4f, 18, 0.1f, -4.8f);
  t3_mesh *outer = part(garage_root, g_plane, M_RUBBER(0x24201b), 160, 60, 1, 18, -0.02f, 0); rot(outer, -PI / 2, 0, 0);
  /* side walls */
  BOX(garage_root, M_PLY(0x1d1915), 0.4f, 3.5f, 12, -6.2f, 1.75f, 0);
  BOX(garage_root, M_PLY(0x1d1915), 0.4f, 3.5f, 12, 42.2f, 1.75f, 0);
  /* strip lights */
  for (int k = 0; k < 8; k++) part(garage_root, g_box, emis(0xfff1d6, 1.8f), 3.0f, 0.08f, 0.12f, -3 + k * 6.0f, 3.3f, -2.0f);
  for (int t = 0; t < 4; t++) {
    float x = 12.0f * t;
    turntables[t] = group_at(garage_root, x, 0, 0);
    CYL(turntables[t], M_STEEL(0x4a4338), 1.1f, 0.14f, 0, 0.07f, 0);
    CYL(turntables[t], M_PAINT(C_HAZARD), 1.12f, 0.04f, 0, 0.12f, 0);
    part(turntables[t], g_disc, M_PAINT(C_TAPE), 1.02f, 0.02f, 1.02f, 0, 0.15f, 0);
    for (int k = 0; k < 8; k++) { t3_mesh *m = part(turntables[t], g_box, M_PAINT(C_BONE), 0.36f, 0.012f, 0.05f, cosf(k * PI / 4) * 0.8f, 0.165f, sinf(k * PI / 4) * 0.8f); rot(m, 0, -k * PI / 4, 0); }
    /* a shelf unit and a tag board between tables */
    prop_def_t sh = { PR_SHELF, x + 6, -3.6f, 2.4f, 0.8f, 2.2f, 0, 0, 0 };
    if (t < 3) { t3_object *g = build_prop(w, &sh, t); t3_object_add(garage_root, g); t3_release(g); }
    BOX(garage_root, M_PLY(0xc8a165), 1.6f, 1.0f, 0.06f, x - 2.6f, 1.9f, -4.75f);
    for (int k = 0; k < 6; k++) BOX(garage_root, M_PAINT(k % 3 == 0 ? C_RUST : C_HAZARD), 0.22f, 0.3f, 0.02f, x - 3.2f + (k % 3) * 0.5f, 2.2f - (k / 3) * 0.45f, -4.7f);
    /* a tool chest in front of the wall */
    BOX(garage_root, M_PAINT(C_RUST), 1.2f, 0.9f, 0.6f, x + 3.2f, 0.45f, -4.3f);
    for (int k = 0; k < 3; k++) BOX(garage_root, M_STEEL(C_STEEL_HI), 1.0f, 0.04f, 0.04f, x + 3.2f, 0.2f + k * 0.26f, -3.98f);
    /* a crate of parts */
    BOX(garage_root, M_PLY(0xa0773f), 0.7f, 0.5f, 0.6f, x - 3.6f, 0.25f, -3.8f);
    part(garage_root, g_cyl12, M_RUBBER(C_TAPE), 0.22f, 0.12f, 0.22f, x - 3.6f, 0.56f, -3.8f);
  }
}

/* -- bots --------------------------------------------------------- */
typedef struct {
  t3_object *root, *body, *drive_g, *weapon_g, *weapon2_g;
  t3_object *wpivot, *w2pivot;                 /* the moving part of each weapon */
  t3_mesh *panels[PN_COUNT][4];
  t3_mesh *wheels[8]; int nwheels;
  t3_mesh *legs[6]; int nlegs;
  t3_mesh *treads[2]; t3_texture *tread_tex;
  t3_mesh *ring[2], *led, *blob, *glow;        /* glow: flame cone */
  t3_mesh *seat_ring;                          /* the 3 px seat colour ring under the bot in ONE CAMERA (10.17) */
  t3_mesh *wsub[4];                            /* weapon sub parts (disc, blur copy, teeth) */
  build_t build; uint32_t seat_colour; int built, is_ops, visible, posed, seat;
  float x, y, z, h, inv_roll;                  /* this frame's interpolated root */
  float melty_yaw, gait, scroll, wheel_spin, spin_vis, saw_vis, led_t, radius, top_y, ride_y;
  float fire_t; int fire_on;                   /* garage test fire */
  float ring_r, ring_y;
} wbot_t;
static wbot_t bots[MAX_BOTS];

static uint32_t paint_colour(int pc, uint32_t seat) {
  switch (pc) { case PC_SEAT: return seat; case PC_CHAR: return 0x2a2520; case PC_BONE: return C_BONE; case PC_STEEL: return 0x6b655a; default: return C_HAZARD; }
}
static uint32_t mix_rgb(uint32_t a, uint32_t b, float t) {
  int r = (int)(((a >> 16) & 255) + ((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t);
  int g = (int)(((a >> 8) & 255) + ((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t);
  int bl = (int)((a & 255) + ((int)(b & 255) - (int)(a & 255)) * t);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}
static t3_material *armour_mat(int armour, uint32_t body) {
  switch (armour) {
    case AR_PLY: return mat(mix_rgb(body, 0xb08a55, 0.6f), 0.9f, 0);
    case AR_ALU: return mat(mix_rgb(body, 0xd7dadc, 0.5f), 0.3f, 0.8f);
    case AR_UHMW: return mat(mix_rgb(body, 0xe9e5dc, 0.55f), 0.55f, 0.05f);
    case AR_STEEL: return mat(mix_rgb(body, 0x6f6c66, 0.5f), 0.3f, 0.8f);
    case AR_TI: return mat(mix_rgb(body, 0x8d97a3, 0.5f), 0.3f, 0.8f);
    case AR_WEAR: return mat(mix_rgb(body, 0x4f3a2d, 0.55f), 0.5f, 0.6f);
    default: return M_PAINT(body);
  }
}

typedef struct { float w, h; float x, y, z; float rx, ry, rz; } panel_place_t;
/* where each of the six panels sits on a chassis of L x W x H standing on ride_y */
static panel_place_t panel_place(const chassis_def_t *c, int panel, float ride) {
  float L = c->len, W = c->wid, H = c->hgt, cy = ride + H / 2;
  panel_place_t p = { 0 };
  switch (panel) {
    case PN_FRONT: p = (panel_place_t){ W * 0.9f, H * 0.9f, 0, cy, L / 2 + 0.012f, 0, 0, 0 }; break;
    case PN_BACK: p = (panel_place_t){ W * 0.9f, H * 0.9f, 0, cy, -L / 2 - 0.012f, 0, PI, 0 }; break;
    case PN_LEFT: p = (panel_place_t){ L * 0.9f, H * 0.9f, -W / 2 - 0.012f, cy, 0, 0, -PI / 2, 0 }; break;
    case PN_RIGHT: p = (panel_place_t){ L * 0.9f, H * 0.9f, W / 2 + 0.012f, cy, 0, 0, PI / 2, 0 }; break;
    case PN_TOP: p = (panel_place_t){ W * 0.9f, L * 0.55f, 0, ride + H + 0.012f, -L * 0.2f, -PI / 2, 0, 0 }; break;
    default: p = (panel_place_t){ W * 0.6f, L * 0.3f, 0, ride + H + 0.012f, L * 0.3f, -PI / 2, 0, 0 }; break;   /* weapon mount */
  }
  if (c->shape == SH_CIRCLE) {
    float r = c->len / 2;
    switch (panel) {
      case PN_FRONT: p = (panel_place_t){ r, H * 0.8f, 0, cy, r * 0.82f, -0.5f, 0, 0 }; break;
      case PN_BACK: p = (panel_place_t){ r, H * 0.8f, 0, cy, -r * 0.82f, 0.5f, PI, 0 }; break;
      case PN_LEFT: p = (panel_place_t){ r, H * 0.8f, -r * 0.82f, cy, 0, 0, -PI / 2, -0.5f }; break;
      case PN_RIGHT: p = (panel_place_t){ r, H * 0.8f, r * 0.82f, cy, 0, 0, PI / 2, 0.5f }; break;
      case PN_TOP: p = (panel_place_t){ r * 0.9f, r * 0.9f, 0, ride + H + 0.01f, 0, -PI / 2, 0, 0 }; break;
      default: p = (panel_place_t){ r * 0.5f, r * 0.4f, 0, ride + H + 0.012f, r * 0.45f, -PI / 2, 0, 0 }; break;
    }
  }
  if (panel == PN_FRONT && c->hgt <= 0.3f && c->len >= 1.1f) { /* the WEDGE front slopes */
    p = (panel_place_t){ W * 0.9f, 0.62f, 0, ride + H * 0.5f, L * 0.22f, -0.50f, 0, 0 };
  }
  return p;
}

static void clear_group(t3_object *g) { while (g->child_count > 0) t3_object_remove(g, g->children[g->child_count - 1]); }

static void build_wheel(wbot_t *b, float r, float x, float y, float z) {
  if (b->nwheels >= 8) return;
  t3_mesh *w = part(b->drive_g, g_cyl12, M_RUBBER(0x1d1915), r, 0.14f, r, x, y, z);
  rot(w, 0, 0, PI / 2);
  b->wheels[b->nwheels++] = w;
  t3_mesh *hub = part(b->drive_g, g_cyl8, M_STEEL(C_STEEL_HI), r * 0.5f, 0.15f, r * 0.5f, x, y, z);
  rot(hub, 0, 0, PI / 2);
}

static void build_drive(wbot_t *b, const chassis_def_t *c) {
  const drive_def_t *d = &DRIVES[b->build.drive];
  float L = c->len, W = c->wid;
  b->nwheels = 0; b->nlegs = 0; b->treads[0] = b->treads[1] = 0;
  switch (b->build.drive) {
    case DR_TWO:
      b->ride_y = 0.12f;
      build_wheel(b, 0.24f, -W / 2 - 0.08f, 0.24f, -L * 0.2f); build_wheel(b, 0.24f, W / 2 + 0.08f, 0.24f, -L * 0.2f);
      part(b->drive_g, g_sphere, M_RUBBER(0x1d1915), 0.08f, 0.08f, 0.08f, 0, 0.08f, L * 0.38f);
      break;
    case DR_FOUR:
      b->ride_y = 0.1f;
      for (int k = 0; k < 4; k++) build_wheel(b, 0.17f, (k & 1 ? 1 : -1) * (W / 2 + 0.08f), 0.17f, (k & 2 ? 1 : -1) * L * 0.3f);
      break;
    case DR_SIX:
      b->ride_y = 0.1f;
      for (int k = 0; k < 6; k++) build_wheel(b, 0.15f, (k & 1 ? 1 : -1) * (W / 2 + 0.08f), 0.15f, ((k >> 1) - 1) * L * 0.34f);
      break;
    case DR_TREAD: case DR_LEGTRACK: {
      b->ride_y = 0.12f;
      canvas_t cv = cv_new(32, 8);
      for (int x = 0; x < 32; x++) cv_rect(&cv, x, 0, 1, 8, (x / 4) & 1 ? 0x1d1915 : 0x4a4338, 255);
      b->tread_tex = cv_texture(&cv, 1, 1);
      t3_material *tm = t3_mesh_standard_material_new(0xffffff); tm->roughness = 0.95f; tm->metalness = 0.1f; t3_material_set_map(tm, b->tread_tex);
      b->tread_tex->repeat.x = 1; b->tread_tex->repeat.y = 4;
      for (int k = 0; k < 2; k++) {
        float x = (k ? 1 : -1) * (W / 2 + 0.12f);
        if (b->build.drive == DR_TREAD) {
          t3_mesh *t = t3_mesh_new(g_box, tm); t3_object_set_scale(t, 0.26f, 0.3f, L + 0.15f); t3_object_set_position(t, x, 0.16f, 0);
          t3_object_add(b->drive_g, t); t3_release(t); b->treads[k] = t;
          for (int r = 0; r < 3; r++) { t3_mesh *rl = part(b->drive_g, g_cyl8, M_STEEL(C_STEEL_HI), 0.1f, 0.3f, 0.1f, x, 0.16f, (r - 1) * (L + 0.1f) * 0.45f); rot(rl, 0, 0, PI / 2); }
        } else {
          for (int p = 0; p < 2; p++) {
            float z = (p ? 1 : -1) * L * 0.3f;
            t3_mesh *knee = BOX(b->drive_g, M_STEEL(C_STEEL_HI), 0.1f, 0.26f, 0.12f, x, 0.3f, z);
            rot(knee, (p ? -1 : 1) * 0.5f, 0, 0);
            t3_mesh *pod = t3_mesh_new(g_box, tm); t3_object_set_scale(pod, 0.24f, 0.22f, 0.5f); t3_object_set_position(pod, x, 0.12f, z + (p ? 0.08f : -0.08f));
            t3_object_add(b->drive_g, pod); t3_release(pod); if (!b->treads[k]) b->treads[k] = pod;
          }
        }
      }
      t3_release(tm);
      break; }
    case DR_HOVER: {
      b->ride_y = 0.22f;
      float r = (L > W ? L : W) * 0.6f;
      t3_mesh *sk = part(b->drive_g, g_torus, M_RUBBER(0x1d1915), r, r, 1, 0, 0.14f, 0);
      rot(sk, PI / 2, 0, 0);
      t3_object_set_scale(sk, r, r, 0.9f);
      for (int k = 0; k < 2; k++) part(b->drive_g, g_cyl8, M_STEEL(C_STEEL_HI), 0.18f, 0.12f, 0.18f, (k ? 1 : -1) * W * 0.3f, c->hgt + b->ride_y + 0.06f, -L * 0.3f);
      b->radius = r;
      break; }
    case DR_WALKER: {
      b->ride_y = 0.3f;
      for (int k = 0; k < 6; k++) {
        float x = (k & 1 ? 1 : -1) * (W / 2 + 0.1f), z = ((k >> 1) - 1) * L * 0.36f;
        t3_mesh *leg = BOX(b->drive_g, M_STEEL(C_STEEL_HI), 0.08f, 0.34f, 0.08f, x, 0.17f, z);
        b->legs[b->nlegs++] = leg;
        BOX(b->drive_g, M_PAINT(C_RUST), 0.14f, 0.05f, 0.14f, x, 0.025f, z);
        BOX(b->drive_g, M_STEEL(C_STEEL), 0.2f, 0.06f, 0.06f, x * 0.65f, 0.32f, z);
      }
      break; }
    case DR_MELTY: {
      b->ride_y = 0.1f;
      for (int k = 0; k < 2; k++) build_wheel(b, 0.16f, (k ? 1 : -1) * (W / 2 + 0.06f), 0.16f, 0);
      for (int k = 0; k < 4; k++) { t3_mesh *edge = BOX(b->body, M_STEEL(0xcfd3d6), 0.12f, c->hgt * 0.9f, 0.3f, cosf(k * PI / 2) * (W / 2 + 0.02f), b->ride_y + c->hgt / 2, sinf(k * PI / 2) * (L / 2 + 0.02f)); rot(edge, 0, -k * PI / 2, 0); }
      break; }
    default: break;
  }
  (void)d;
}

/* weapon sub parts; the pivot is what animates */
static void build_weapon(wbot_t *b, int secondary) {
  int wp = secondary ? b->build.weapon2 : b->build.weapon;
  const chassis_def_t *c = &CHASSIS[b->build.chassis];
  t3_object *g = secondary ? b->weapon2_g : b->weapon_g;
  float L = c->len, W = c->wid, top = b->ride_y + c->hgt;
  float z0 = L / 2, y0 = b->ride_y + c->hgt * 0.5f;
  if (secondary) {
    if (b->build.chassis == CH_LUNCHBOX && !(b->build.weapon == WP_WEDGE || b->build.weapon == WP_LIFTER)) { z0 = L * 0.1f; y0 = top; }
    else { z0 = L / 2 + 0.35f; y0 = b->ride_y + c->hgt * 0.7f; }
  }
  t3_object *piv = group_at(g, 0, y0, z0);
  if (secondary) b->w2pivot = piv; else b->wpivot = piv;
  t3_material *steel = M_STEEL(0x7a766e), *bright = M_STEEL(0xd0ccc4), *rust = M_PAINT(C_RUST);
  switch (wp) {
    case WP_WEDGE: {
      t3_geometry *wg = wedge_geometry(W * 1.05f, 0.55f, b->ride_y + c->hgt * 0.95f);
      t3_mesh *m = t3_mesh_new(wg, steel); t3_object_set_position(m, 0, -y0, 0.25f);
      t3_object_add(piv, m); t3_release(m); t3_release(wg);
      BOX(piv, M_PAINT(C_HAZARD), W * 1.05f, 0.02f, 0.08f, 0, -y0 + 0.012f, 0.5f);
      break; }
    case WP_SPIKE: {
      BOX(piv, steel, 0.24f, 0.2f, 0.3f, 0, 0, -0.1f);
      t3_object *rod = group_at(piv, 0, 0, 0);
      t3_mesh *r = part(rod, g_cyl8, bright, 0.05f, 0.5f, 0.05f, 0, 0, 0.3f); rot(r, PI / 2, 0, 0);
      t3_mesh *tip = part(rod, g_cone, bright, 0.07f, 0.18f, 0.07f, 0, 0, 0.62f); rot(tip, PI / 2, 0, 0);
      b->wsub[secondary ? 2 : 0] = (t3_mesh *)rod;
      break; }
    case WP_LIFTER: {
      for (int k = 0; k < 2; k++) {
        t3_mesh *f = BOX(piv, steel, 0.08f, 0.05f, 0.62f, (k ? 1 : -1) * 0.22f, -c->hgt * 0.4f, 0.3f);
        (void)f;
      }
      BOX(piv, steel, 0.6f, 0.08f, 0.1f, 0, -c->hgt * 0.4f, 0.02f);
      t3_mesh *bar = BOX(piv, M_PAINT(C_HAZARD), 0.62f, 0.03f, 0.05f, 0, -c->hgt * 0.4f + 0.04f, 0.6f); (void)bar;
      break; }
    case WP_SAW: {
      BOX(piv, steel, 0.12f, 0.12f, 0.5f, 0, 0.1f, 0.25f);
      t3_object *disc = group_at(piv, 0, 0.05f, 0.7f);
      t3_mesh *d = part(disc, g_cyl12, bright, 0.3f, 0.03f, 0.3f, 0, 0, 0); rot(d, 0, 0, PI / 2);
      for (int t = 0; t < 10; t++) { t3_mesh *th = part(disc, g_box, M_STEEL(0xe6e2d8), 0.04f, 0.09f, 0.08f, 0, cosf(t * TAU / 10) * 0.32f, sinf(t * TAU / 10) * 0.32f); rot(th, -t * TAU / 10, 0, 0); }
      b->wsub[secondary ? 2 : 0] = (t3_mesh *)disc;
      if (!secondary) { b->ring_r = 0.3f; b->ring_y = y0 + 0.05f; }
      break; }
    case WP_FLAME: {
      BOX(piv, M_PAINT(C_RUST), 0.3f, 0.3f, 0.28f, 0, 0.05f, -0.1f);
      t3_mesh *n = part(piv, g_cyl8, M_PAINT(C_TAPE), 0.06f, 0.4f, 0.06f, 0, 0.08f, 0.25f); rot(n, PI / 2, 0, 0);
      part(piv, g_cyl8, bright, 0.09f, 0.06f, 0.09f, 0, 0.08f, 0.47f);
      t3_material *cm = glass(0xffb347, 0.0f);
      t3_mesh *cone = t3_mesh_new(g_cone, cm); t3_release(cm);     /* the mesh holds the only ref */
      t3_object_set_scale(cone, 0.35f, 2.0f, 0.35f); t3_object_set_position(cone, 0, 0.08f, 1.45f); t3_object_set_rotation(cone, -PI / 2, 0, 0);
      t3_object_add(piv, cone); t3_release(cone);
      if (secondary) b->wsub[3] = cone; else b->glow = cone;
      break; }
    case WP_HAMMER: {
      /* the pivot sits at the back of the mount; the arm reaches forward and the head drops in front of the bot */
      t3_object_set_position(piv, 0, top + 0.15f, -L * 0.2f);
      BOX(g, steel, 0.16f, 0.3f, 0.16f, -0.16f, top + 0.05f, -L * 0.2f);
      BOX(g, steel, 0.16f, 0.3f, 0.16f, 0.16f, top + 0.05f, -L * 0.2f);
      BOX(piv, steel, 0.1f, 0.1f, 1.1f, 0, 0, 0.5f);
      BOX(piv, rust, 0.4f, 0.3f, 0.36f, 0, 0, 1.05f);
      BOX(piv, M_PAINT(C_HAZARD), 0.42f, 0.06f, 0.1f, 0, 0, 1.05f);
      break; }
    case WP_FLIPPER: {
      t3_object_set_position(piv, 0, b->ride_y + 0.08f, L * 0.05f);
      t3_mesh *plate = BOX(piv, M_STEEL(0x8a857c), W * 0.95f, 0.05f, L * 0.5f + 0.2f, 0, 0, L * 0.3f);
      (void)plate;
      BOX(piv, M_PAINT(C_HAZARD), W * 0.95f, 0.02f, 0.08f, 0, 0.03f, L * 0.55f + 0.1f);
      BOX(g, steel, 0.3f, 0.12f, 0.12f, 0, b->ride_y + 0.08f, L * 0.05f);
      break; }
    case WP_DRUM: {
      t3_object_set_position(piv, 0, b->ride_y + 0.3f, L / 2 + 0.25f);
      BOX(g, steel, 0.1f, 0.3f, 0.3f, -W / 2 + 0.05f, b->ride_y + 0.3f, L / 2 + 0.1f);
      BOX(g, steel, 0.1f, 0.3f, 0.3f, W / 2 - 0.05f, b->ride_y + 0.3f, L / 2 + 0.1f);
      t3_mesh *d = part(piv, g_cyl12, M_STEEL(0x6f6c66), 0.3f, W - 0.1f, 0.3f, 0, 0, 0); rot(d, 0, 0, PI / 2);
      for (int t = 0; t < 4; t++) { t3_mesh *th = part(piv, g_box, bright, W - 0.1f, 0.08f, 0.1f, 0, cosf(t * PI / 2) * 0.3f, sinf(t * PI / 2) * 0.3f); rot(th, -t * PI / 2, 0, 0); }
      if (!secondary) { b->ring_r = 0.32f; b->ring_y = b->ride_y + 0.3f; }
      break; }
    case WP_HORIZ: {
      t3_object_set_position(piv, 0, top + 0.07f, L * 0.15f);
      BOX(g, steel, 0.16f, 0.2f, 0.16f, 0, top, L * 0.15f);
      t3_object *disc = group_at(piv, 0, 0, 0);
      part(disc, g_cyl12, M_STEEL(0x6f6c66), 0.55f, 0.06f, 0.55f, 0, 0, 0);
      part(disc, g_cyl8, bright, 0.2f, 0.09f, 0.2f, 0, 0, 0);
      for (int t = 0; t < 3; t++) { t3_mesh *th = part(disc, g_box, bright, 0.14f, 0.08f, 0.22f, cosf(t * TAU / 3) * 0.55f, 0, sinf(t * TAU / 3) * 0.55f); rot(th, 0, -t * TAU / 3, 0); }
      b->wsub[secondary ? 2 : 0] = (t3_mesh *)disc;
      /* the motion blur copy */
      t3_material *bm = glass(0x8d97a3, 0.0f);
      t3_mesh *blur = t3_mesh_new(g_disc, bm); t3_release(bm); t3_object_set_scale(blur, 0.58f, 0.03f, 0.58f);
      t3_object_add(piv, blur); t3_release(blur); b->wsub[secondary ? 3 : 1] = blur;
      if (!secondary) { b->ring_r = 0.6f; b->ring_y = top + 0.07f; }
      break; }
    case WP_CRUSHER: {
      t3_object_set_position(piv, 0, top, L * 0.1f);
      BOX(g, steel, 0.5f, 0.1f, 0.9f, 0, b->ride_y + 0.12f, L / 2 + 0.2f);         /* the fixed lower jaw */
      BOX(g, rust, 0.5f, 0.1f, 0.12f, 0, b->ride_y + 0.17f, L / 2 + 0.6f);
      BOX(piv, steel, 0.12f, 0.1f, 0.9f, -0.18f, 0.05f, 0.5f);
      BOX(piv, steel, 0.12f, 0.1f, 0.9f, 0.18f, 0.05f, 0.5f);
      BOX(piv, rust, 0.5f, 0.12f, 0.16f, 0, 0.0f, 0.95f);
      t3_mesh *tooth = part(piv, g_cone, bright, 0.06f, 0.16f, 0.06f, 0, -0.12f, 0.95f); rot(tooth, PI, 0, 0);
      t3_object_set_rotation(piv, -0.5f, 0, 0);
      break; }
    default: break;
  }
}

static void build_chassis(wbot_t *b) {
  const chassis_def_t *c = &CHASSIS[b->build.chassis];
  uint32_t body = paint_colour(PAINT_SCHEMES[b->build.paint % PAINT_COUNT].body, b->seat_colour);
  uint32_t stripe = paint_colour(PAINT_SCHEMES[b->build.paint % PAINT_COUNT].stripe, b->seat_colour);
  if (b->is_ops) { body = 0x2a2520; stripe = C_HAZARD; }
  t3_material *bm = M_PAINT(body);
  float L = c->len, W = c->wid, H = c->hgt, r = b->ride_y;
  if (b->is_ops) L *= 1.4f;
  switch (b->build.chassis) {
    case CH_WEDGE: {
      BOX(b->body, bm, W, H, L * 0.5f, 0, r + H / 2, -L * 0.25f);
      t3_geometry *wg = wedge_geometry(W, L * 0.55f, H);
      t3_mesh *m = t3_mesh_new(wg, bm); t3_object_set_position(m, 0, r, L * 0.225f);
      t3_object_add(b->body, m); t3_release(m); t3_release(wg);
      break; }
    case CH_SHELL: {
      t3_vec2 prof[6] = { { 0, 0 }, { L * 0.42f, 0 }, { L * 0.5f, H * 0.35f }, { L * 0.45f, H * 0.8f }, { L * 0.25f, H }, { 0, H } };
      t3_geometry *lg = t3_lathe_geometry_new(prof, 6, 14, 0, TAU);
      t3_mesh *m = t3_mesh_new(lg, bm); t3_object_set_position(m, 0, r, 0);
      t3_object_add(b->body, m); t3_release(m); t3_release(lg);
      break; }
    case CH_FRAME: {
      t3_material *tube = M_STEEL(0x8a857c);
      for (int k = 0; k < 4; k++) BOX(b->body, tube, 0.06f, H, 0.06f, (k & 1 ? 1 : -1) * (W / 2 - 0.03f), r + H / 2, (k & 2 ? 1 : -1) * (L / 2 - 0.03f));
      for (int k = 0; k < 2; k++) { BOX(b->body, tube, W, 0.06f, 0.06f, 0, r + (k ? H - 0.03f : 0.03f), L / 2 - 0.03f); BOX(b->body, tube, W, 0.06f, 0.06f, 0, r + (k ? H - 0.03f : 0.03f), -L / 2 + 0.03f);
        BOX(b->body, tube, 0.06f, 0.06f, L, (k ? 1 : -1) * (W / 2 - 0.03f), r + 0.03f, 0); BOX(b->body, tube, 0.06f, 0.06f, L, (k ? 1 : -1) * (W / 2 - 0.03f), r + H - 0.03f, 0); }
      BOX(b->body, bm, W * 0.5f, H * 0.5f, L * 0.5f, 0, r + H * 0.3f, 0);     /* the electronics box inside */
      part(b->body, g_box, emis(C_LIVE, 2.0f), 0.05f, 0.05f, 0.05f, W * 0.1f, r + H * 0.58f, -L * 0.1f);
      break; }
    case CH_LUNCHBOX: {
      BOX(b->body, bm, W, H, L, 0, r + H / 2, 0);
      BOX(b->body, M_STEEL(C_STEEL_HI), 0.06f, 0.2f, 0.06f, 0, r + H + 0.1f, -L * 0.25f);
      BOX(b->body, M_STEEL(C_STEEL_HI), 0.06f, 0.2f, 0.06f, 0, r + H + 0.1f, L * 0.25f);
      BOX(b->body, M_STEEL(C_STEEL_HI), 0.06f, 0.06f, L * 0.5f + 0.06f, 0, r + H + 0.2f, 0);
      BOX(b->body, M_PAINT(C_TAPE), W + 0.02f, 0.04f, L + 0.02f, 0, r + H * 0.5f, 0);   /* the lid seam */
      break; }
    default:
      BOX(b->body, bm, W, H, L, 0, r + H / 2, 0);
      break;
  }
  /* the band: 0.08 u in the seat colour, the one colour that never changes */
  if (c->shape == SH_CIRCLE) part(b->body, g_cyl12, M_PAINT(b->seat_colour), L / 2 + 0.03f, 0.08f, L / 2 + 0.03f, 0, r + H * 0.22f, 0);
  else if (b->build.chassis == CH_WEDGE) BOX(b->body, M_PAINT(b->seat_colour), W + 0.04f, 0.08f, L * 0.5f + 0.04f, 0, r + 0.1f, -L * 0.25f);
  else BOX(b->body, M_PAINT(b->seat_colour), W + 0.04f, 0.08f, L + 0.04f, 0, r + 0.1f, 0);
  /* the stripe on the top and decals */
  if (c->shape != SH_CIRCLE) {
    BOX(b->body, M_PAINT(stripe), W * 0.18f, 0.015f, L * 0.8f, 0, r + H + 0.02f, 0);
    if (b->build.decal == DC_HAZARD_STRIPES) for (int k = 0; k < 3; k++) { t3_mesh *s = BOX(b->body, M_PAINT(C_HAZARD), 0.08f, 0.012f, W * 0.5f, -W * 0.3f + k * W * 0.3f, r + H + 0.022f, -L * 0.3f); rot(s, 0, 0.785f, 0); }
    if (b->build.decal == DC_TAPE_BAND) { t3_mesh *t = t3_mesh_new(g_plane, mat_tape); t3_object_set_scale(t, W * 0.96f, 0.14f, 1); rot(t, -PI / 2, 0, 0); t3_object_set_position(t, 0, r + H + 0.024f, -L * 0.3f); t3_object_add(b->body, t); t3_release(t); }
  }
  /* seat digits: top and both sides (OPS reads OPS) */
  int digit = b->is_ops ? 4 : (b->seat & 3);
  float ds = H * 0.75f; if (ds < 0.18f) ds = 0.18f; if (ds > 0.34f) ds = 0.34f;
  if (b->build.decal == DC_SEAT_NUMBER || b->is_ops || 1) {
    t3_mesh *n;
    n = t3_mesh_new(g_plane, mat_digit[digit]); t3_object_set_scale(n, ds, ds, 1); t3_object_set_position(n, -W / 2 - 0.02f, r + H / 2 + 0.03f, -L * 0.25f); rot(n, 0, -PI / 2, 0); t3_object_add(b->body, n); t3_release(n);
    n = t3_mesh_new(g_plane, mat_digit[digit]); t3_object_set_scale(n, ds, ds, 1); t3_object_set_position(n, W / 2 + 0.02f, r + H / 2 + 0.03f, -L * 0.25f); rot(n, 0, PI / 2, 0); t3_object_add(b->body, n); t3_release(n);
    if (c->shape != SH_CIRCLE) { n = t3_mesh_new(g_plane, mat_digit[digit]); t3_object_set_scale(n, ds, ds, 1); t3_object_set_position(n, W * 0.28f, r + H + 0.03f, -L * 0.28f); rot(n, -PI / 2, 0, PI); t3_object_add(b->body, n); t3_release(n); }
  }
  /* panels: four states each, one visible */
  t3_material *am = armour_mat(b->is_ops ? AR_STEEL : b->build.armour, body);
  for (int p = 0; p < PN_COUNT; p++) {
    panel_place_t pl = panel_place(c, p, r);
    if (b->is_ops && (p == PN_LEFT || p == PN_RIGHT)) pl.w *= 1.4f;
    if (b->is_ops && (p == PN_TOP)) pl.z *= 1.4f;
    for (int s = 0; s < 4; s++) {
      t3_geometry *pg = panel_geometry(pl.w, pl.h, s, (uint32_t)(b->build.chassis * 16 + p));
      t3_mesh *m = t3_mesh_new(pg, am);
      t3_object_set_position(m, pl.x, pl.y, pl.z);
      t3_object_set_rotation(m, pl.rx, pl.ry, pl.rz);
      t3_object_add(b->body, m); t3_release(m); t3_release(pg);
      set_vis(m, s == 0);
      b->panels[p][s] = m;
    }
  }
  if (b->is_ops) {
    /* caution tape bumper and an amber stripe */
    t3_mesh *t = t3_mesh_new(g_plane, mat_tape); t3_object_set_scale(t, W + 0.1f, 0.2f, 1); t3_object_set_position(t, 0, r + 0.25f, L / 2 + 0.03f); t3_object_add(b->body, t); t3_release(t);
    BOX(b->body, M_PAINT(C_HAZARD), 0.1f, 0.02f, L, 0, r + H + 0.03f, 0);
  }
  b->top_y = r + H;
}

static void bot_alloc(wbot_t *b, int slot) {
  if (b->root) return;
  b->root = t3_group_new();
  b->body = group_at(b->root, 0, 0, 0);
  b->drive_g = group_at(b->root, 0, 0, 0);
  b->weapon_g = group_at(b->body, 0, 0, 0);
  b->weapon2_g = group_at(b->body, 0, 0, 0);
  for (int k = 0; k < 2; k++) {
    t3_mesh *r = t3_mesh_new(g_ring, glass(0xcfd3d6, 0));
    rot(r, -PI / 2, 0, k * 0.26f);
    t3_object_add(b->root, r); t3_release(r); b->ring[k] = r; set_vis(r, 0);
  }
  t3_material *lm = t3_mesh_basic_material_new(C_LIVE);
  b->led = t3_mesh_new(g_sphere, lm); t3_object_set_scale(b->led, 0.06f, 0.06f, 0.06f); t3_object_add(b->root, b->led); t3_release(b->led); t3_release(lm); set_vis(b->led, 0);
  t3_material *bm = glass(0x000000, 0.35f);
  b->blob = t3_mesh_new(g_circle, bm); rot(b->blob, -PI / 2, 0, 0); t3_object_add(&S->base, b->blob); t3_release(b->blob); t3_release(bm);
  set_vis(b->blob, 0);
  t3_material *srm = t3_mesh_basic_material_new(C_BONE);
  b->seat_ring = t3_mesh_new(g_ring, srm); rot(b->seat_ring, -PI / 2, 0, 0); t3_object_add(&S->base, b->seat_ring); t3_release(b->seat_ring); t3_release(srm);
  set_vis(b->seat_ring, 0);
  t3_object_add(&S->base, b->root);
  b->root->visible = false;
  (void)slot;
}

static void bot_rebuild(wbot_t *b) {
  if (b->tread_tex) { t3_release(b->tread_tex); b->tread_tex = 0; }   /* the creation ref; the material held its own */
  clear_group(b->body); clear_group(b->drive_g);
  b->weapon_g = group_at(b->body, 0, 0, 0);
  b->weapon2_g = group_at(b->body, 0, 0, 0);
  memset(b->wsub, 0, sizeof b->wsub); b->glow = 0; b->wpivot = b->w2pivot = 0; b->ring_r = 0;
  memset(b->panels, 0, sizeof b->panels);
  const chassis_def_t *c = &CHASSIS[b->build.chassis];
  build_drive(b, c);
  build_chassis(b);
  if (b->build.drive != DR_MELTY) {
    build_weapon(b, 0);
    if (b->build.weapon2 != WP_NONE) build_weapon(b, 1);
  }
  float rr = (c->len > c->wid ? c->len : c->wid) * 0.7f;
  if (b->radius < rr) b->radius = rr;
  b->seat_ring->materials[0]->color = t3_color_hex(b->is_ops ? C_HAZARD : b->seat_colour);
  t3_object_set_scale(b->seat_ring, b->radius * 1.15f, b->radius * 1.15f, 1);
  t3_object_set_scale(b->blob, b->radius, b->radius, 1);
  for (int k = 0; k < 2; k++) {
    set_vis(b->ring[k], b->ring_r > 0);
    if (b->ring_r > 0) { t3_object_set_scale(b->ring[k], b->ring_r + 0.1f, b->ring_r + 0.1f, 1); t3_object_set_position(b->ring[k], 0, b->ring_y + (k ? 0.02f : 0.0f), b->build.weapon == WP_HORIZ ? c->len * 0.15f : (b->build.weapon == WP_SAW ? c->len / 2 + 0.7f : c->len / 2 + 0.25f)); }
  }
  set_vis(b->led, b->build.drive == DR_MELTY);
  t3_object_set_position(b->led, 0, b->top_y + 0.06f, c->len * 0.45f);
  b->built = 1;
}

void world_bot_build(int slot, const build_t *bd, uint32_t seat_colour) {
  if (slot < 0 || slot >= MAX_BOTS) return;
  wbot_t *b = &bots[slot];
  bot_alloc(b, slot);
  b->build = *bd; b->seat_colour = seat_colour; b->is_ops = 0;
  b->seat = slot;
  for (int s = 0; s < 4; s++) if (SEAT_RGB[s] == seat_colour) b->seat = s;
  b->radius = 0;
  bot_rebuild(b);
}

void world_bot_build_ops(int slot) {
  if (slot < 0 || slot >= MAX_BOTS) return;
  wbot_t *b = &bots[slot];
  bot_alloc(b, slot);
  build_t ops = { CH_LUNCHBOX, DR_TREAD, WP_LIFTER, WP_SAW, AR_STEEL, 1, DC_TAPE_BAND };
  b->build = ops; b->seat_colour = C_HAZARD; b->is_ops = 1; b->seat = 4; b->radius = 1.1f;
  bot_rebuild(b);
}

void world_bot_visible(int slot, int on) {
  if (slot < 0 || slot >= MAX_BOTS || !bots[slot].root) return;
  bots[slot].visible = on;
  bots[slot].root->visible = on != 0;
  set_vis(bots[slot].blob, on);
  if (!on) set_vis(bots[slot].seat_ring, 0);
}

static void place_root(wbot_t *b, float x, float y, float z, float h, int inverted) {
  b->x = x; b->y = y; b->z = z; b->h = h;
  float ry = inverted ? b->top_y + 0.02f : 0;
  t3_object_set_position(b->root, x, y + ry, z);
  t3_object_set_rotation(b->root, 0, h, inverted ? PI : 0);
  float fy = attached ? arena_floor_y(attached, x, z) : 0;
  t3_object_set_position(b->blob, x, 0.012f + fy, z);
  t3_object_set_position(b->seat_ring, x, 0.016f + fy, z);
  float k = 1 - y / 3; if (k < 0.2f) k = 0.2f;
  t3_object_set_scale(b->blob, b->radius * k, b->radius * k, 1);
  b->blob->materials[0]->opacity = 0.35f * k;
}

void world_bot_pose(int slot, float x, float y, float z, float heading, int inverted) {
  if (slot < 0 || slot >= MAX_BOTS) return;
  wbot_t *b = &bots[slot];
  bot_alloc(b, slot);
  b->posed = 1;
  place_root(b, x, y, z, heading, inverted);
}

int world_bot_pos(int slot, float *x, float *y, float *z) {
  if (slot < 0 || slot >= MAX_BOTS || !bots[slot].root || !bots[slot].visible) return 0;
  if (x) *x = bots[slot].x; if (y) *y = bots[slot].y; if (z) *z = bots[slot].z;
  return 1;
}

/* the moving parts, from the sim state (or the garage test fire) */
static void pose_weapon(wbot_t *b, const bot_t *bt, int secondary, float dt) {
  int wp = secondary ? b->build.weapon2 : b->build.weapon;
  t3_object *piv = secondary ? b->w2pivot : b->wpivot;
  if (!piv) return;
  float spin = secondary ? bt->spin2 : bt->spin;
  int active = secondary ? bt->active2 : bt->active, cooldown = secondary ? bt->cooldown2 : bt->cooldown;
  int wb = (bt->in.buttons & IN_A) || bt->in.rt > TRIGGER_ON;
  switch (wp) {
    case WP_HORIZ: {
      float *vis = secondary ? &b->saw_vis : &b->spin_vis;
      *vis += spin * 40.0f * dt;
      t3_mesh *disc = b->wsub[secondary ? 2 : 0], *blur = b->wsub[secondary ? 3 : 1];
      if (disc) t3_object_set_rotation(disc, 0, *vis, 0);
      if (blur) { blur->materials[0]->opacity = spin * (J_SPIN_BLUR_A / 255.0f); t3_object_set_rotation(blur, 0, *vis + J_SPIN_BLUR_DEG * PI / 180, 0); }
      break; }
    case WP_SAW: {
      float *vis = secondary ? &b->saw_vis : &b->spin_vis;
      *vis += spin * 50.0f * dt;
      t3_mesh *disc = b->wsub[secondary ? 2 : 0];
      if (disc) t3_object_set_rotation(disc, *vis, 0, 0);
      break; }
    case WP_DRUM: {
      b->spin_vis += spin * 40.0f * dt;
      t3_object_set_rotation(piv, b->spin_vis, 0, 0);
      break; }
    case WP_SPIKE: {
      t3_mesh *rod = b->wsub[secondary ? 2 : 0];
      float ext = active > 0 ? 0.45f * (1 - fabsf((active - 4.5f) / 4.5f)) : 0;
      if (rod) t3_object_set_position(rod, 0, 0, ext);
      break; }
    case WP_LIFTER: {
      float lift = bt->lift;
      t3_object_set_rotation(piv, -lift * 0.9f, 0, 0);
      break; }
    case WP_HAMMER: {
      /* windup raises the arm, active slams it, cooldown returns it */
      float ang;
      if (bt->windup > 0) ang = -0.6f - (15 - bt->windup) / 15.0f * 1.2f;        /* raise to -1.8 */
      else if (active > 0) ang = -1.8f + (4 - active) / 4.0f * 2.3f;              /* slam to +0.5 */
      else if (cooldown > 0) ang = 0.5f - (72 - cooldown) / 72.0f * 1.1f;        /* back to -0.6 */
      else ang = -0.6f;
      t3_object_set_rotation(piv, ang, 0, 0);
      break; }
    case WP_FLIPPER: {
      float ang;
      if (active > 0) ang = -(4 - active) / 4.0f * 1.1f;
      else if (cooldown > 0) { float t = (108 - cooldown) / 108.0f; ang = -1.1f * (1 - (t < 0.3f ? t / 0.3f : 1)); }
      else ang = 0;
      t3_object_set_rotation(piv, ang, 0, 0);
      break; }
    case WP_CRUSHER: {
      float ang = bt->clamp > 0 ? -0.5f + (24 - bt->clamp) / 24.0f * 0.45f : (bt->lock > 0 ? -0.05f : -0.5f);
      t3_object_set_rotation(piv, ang, 0, 0);
      break; }
    case WP_FLAME: {
      t3_mesh *cone = secondary ? b->wsub[3] : b->glow;
      float fuel = secondary ? bt->fuel2 : bt->fuel;
      int firing = wb && fuel > 0 && bt->alive;
      if (cone) {
        float target = firing ? 0.55f : 0;
        t3_material *m = cone->materials[0];
        m->opacity += (target - m->opacity) * (firing ? 0.5f : 0.3f);
        float fl = 1 + 0.15f * sinf(world_t * 37) + 0.1f * sinf(world_t * 53 + 1);
        t3_object_set_scale(cone, 0.35f * fl, 2.0f * (0.8f + 0.2f * fl), 0.35f * fl);
      }
      break; }
    default: break;
  }
}

static void sync_panels(wbot_t *b, const uint8_t *state) {
  for (int p = 0; p < PN_COUNT; p++) {
    int s = state ? state[p] : 0; if (s > 4) s = 4;
    for (int k = 0; k < 4; k++) if (b->panels[p][k]) set_vis(b->panels[p][k], k == s);
  }
}

static void sync_drive(wbot_t *b, float speed, float dt, int inverted) {
  const chassis_def_t *c = &CHASSIS[b->build.chassis];
  (void)inverted;
  b->wheel_spin += speed * dt / 0.17f;
  for (int k = 0; k < b->nwheels; k++) t3_object_set_rotation(b->wheels[k], b->wheel_spin, 0, PI / 2);
  if (b->tread_tex) { b->scroll += speed * dt * 2.0f; b->tread_tex->offset.y = b->scroll; }
  if (b->nlegs) {
    b->gait += speed * dt * 3.0f;
    for (int k = 0; k < b->nlegs; k++) {
      float ph = b->gait + (k % 3) * 2.1f + (k & 1) * PI;
      float sw = sinf(ph) * 0.35f * (speed > 0.05f ? 1 : 0.0f), lift = fmaxf(0, cosf(ph)) * 0.08f * (speed > 0.05f ? 1 : 0);
      float x = (k & 1 ? 1 : -1) * (c->wid / 2 + 0.1f), z = ((k >> 1) - 1) * c->len * 0.36f;
      t3_object_set_position(b->legs[k], x, 0.17f + lift, z + sw * 0.2f);
      t3_object_set_rotation(b->legs[k], sw, 0, 0);
    }
    float bob = (speed > 0.05f ? 0.03f * sinf(b->gait * 2) : 0);
    t3_object_set_position(b->body, 0, bob, 0);
  }
}

static void sync_bot(wbot_t *b, const bot_t *bt, float alpha, float dt) {
  const body_t *bo = &bt->b;
  float x = bo->px + (bo->x - bo->px) * alpha, z = bo->pz + (bo->z - bo->pz) * alpha;
  float y = bo->py + (bo->y - bo->py) * alpha;
  float dh = rtr_wrap_pi(bo->h - bo->ph), h = bo->ph + dh * alpha;
  if (bt->is_ops) h = bo->h;
  if (bt->lifted_by || bt->clamped_by) { x = bo->x; z = bo->z; y = bo->y; h = bo->h; }
  place_root(b, x, y, z, h, bt->inverted);
  /* MELTY spins the body; taunt wiggles it */
  float body_yaw = 0;
  if (b->build.drive == DR_MELTY) { b->melty_yaw += bt->spin * 30.0f * dt; body_yaw = b->melty_yaw; }
  if (bt->taunt > 0) body_yaw += sinf(world_t * TAU * 6) * 0.26f;
  if (bt->lifted_by) t3_object_set_rotation(b->body, -0.35f, body_yaw, 0);
  else if (bt->stun > 0 && !bt->inverted) t3_object_set_rotation(b->body, 0, body_yaw, sinf(world_t * 40) * 0.04f);
  else t3_object_set_rotation(b->body, 0, body_yaw, 0);
  float speed = sqrtf(bo->vx * bo->vx + bo->vz * bo->vz);
  sync_drive(b, bt->alive ? speed : 0, dt, bt->inverted);
  sync_panels(b, bt->panel_state);
  /* wheels and parts fall off at 0 HP: the frame stays */
  int burst = !bt->alive && bt->hp <= 0;
  for (int k = 0; k < b->nwheels; k++) set_vis(b->wheels[k], !burst);
  for (int k = 0; k < 2; k++) if (b->treads[k]) set_vis(b->drive_g, !burst);
  set_vis(b->weapon_g, !burst); set_vis(b->weapon2_g, !burst);
  set_vis(b->seat_ring, bt->alive && cam_one_camera_active() && !bt->is_ops);
  /* FOUR WHEEL loses a wheel at HP 30 (visual) */
  if (b->build.drive == DR_FOUR && bt->hp <= 30 && b->nwheels == 4 && bt->alive) set_vis(b->wheels[3], 0);
  pose_weapon(b, bt, 0, dt);
  if (b->build.weapon2 != WP_NONE) pose_weapon(b, bt, 1, dt);
  /* the spinner ring: alpha spin times 160, the 15 degree copy at 50 */
  if (b->ring_r > 0) {
    float sp = bt->spin;
    if (b->build.weapon == WP_SAW) sp = bt->saw_on ? bt->spin : 0;
    b->ring[0]->materials[0]->opacity = sp * J_SPIN_RING_A / 255.0f;
    b->ring[1]->materials[0]->opacity = sp * J_SPIN_BLUR_A / 255.0f;
    t3_object_set_rotation(b->ring[0], -PI / 2, 0, b->spin_vis);
    t3_object_set_rotation(b->ring[1], -PI / 2, 0, b->spin_vis + J_SPIN_BLUR_DEG * PI / 180);
  }
  /* MELTY LED blinks at 4 Hz on the heading intent */
  if (b->build.drive == DR_MELTY) set_vis(b->led, fmodf(world_t * 4, 1.0f) < 0.5f);
  /* smoke and fire follow HP */
  if (bt->alive) fx_bot_damage_state((int)(b - bots), bt->hp, x, y + b->top_y, z);
}

/* the garage test fire runs the same poser on a synthetic bot */
static void pose_test_fire(wbot_t *b, float dt) {
  bot_t t; memset(&t, 0, sizeof t);
  t.alive = 1; t.build = b->build; t.hp = 100;
  if (b->fire_on) {
    b->fire_t += dt;
    float ft = b->fire_t;
    int wp = b->build.weapon;
    if (wp == WP_HORIZ || wp == WP_DRUM || wp == WP_SAW || b->build.drive == DR_MELTY) {
      float up = WEAPONS[wp].spin_up > 0 ? WEAPONS[wp].spin_up : 1.5f;
      t.spin = ft < up ? ft / up : (ft < up + 1.5f ? 1 : fmaxf(0, 1 - (ft - up - 1.5f) / (up * 2)));
      t.saw_on = 1; t.in.buttons = ft < up + 1.5f ? IN_A : 0;
      if (ft > up * 3 + 1.5f) b->fire_on = 0;
    } else if (wp == WP_HAMMER) {
      int f = (int)(ft * 60);
      if (f < 15) t.windup = (int16_t)(15 - f); else if (f < 19) t.active = (int16_t)(4 - (f - 15)); else if (f < 91) t.cooldown = (int16_t)(72 - (f - 19)); else b->fire_on = 0;
    } else if (wp == WP_FLIPPER) {
      int f = (int)(ft * 60);
      if (f < 4) t.active = (int16_t)(4 - f); else if (f < 112) t.cooldown = (int16_t)(108 - (f - 4)); else b->fire_on = 0;
    } else if (wp == WP_SPIKE) {
      int f = (int)(ft * 60);
      if (f < 9) t.active = (int16_t)(9 - f); else if (f < 30) t.cooldown = 1; else b->fire_on = 0;
    } else if (wp == WP_LIFTER) {
      t.lift = ft < 0.6f ? ft / 0.6f : (ft < 1.4f ? 1 : fmaxf(0, 1 - (ft - 1.4f) / 0.6f));
      if (ft > 2.1f) b->fire_on = 0;
    } else if (wp == WP_FLAME) {
      t.fuel = 240; t.in.buttons = ft < 1.6f ? IN_A : 0;
      if (ft > 2.2f) b->fire_on = 0;
    } else if (wp == WP_CRUSHER) {
      int f = (int)(ft * 60);
      if (f < 24) t.clamp = (int16_t)(24 - f); else if (f < 100) t.lock = 1; else b->fire_on = 0;
    } else b->fire_on = 0;
    if (b->build.drive == DR_MELTY) { b->melty_yaw += t.spin * 30.0f * dt; t3_object_set_rotation(b->body, 0, b->melty_yaw, 0); }
  }
  pose_weapon(b, &t, 0, dt);
  if (b->build.weapon2 != WP_NONE) pose_weapon(b, &t, 1, dt);
  if (b->ring_r > 0) {
    b->ring[0]->materials[0]->opacity = t.spin * J_SPIN_RING_A / 255.0f;
    b->ring[1]->materials[0]->opacity = t.spin * J_SPIN_BLUR_A / 255.0f;
  }
  if (b->build.drive == DR_MELTY) set_vis(b->led, fmodf(world_t * 4, 1.0f) < 0.5f);
  sync_drive(b, 0, dt, 0);
}

void world_weapon_test_fire(int slot) {
  if (slot < 0 || slot >= MAX_BOTS || !bots[slot].built) return;
  bots[slot].fire_on = 1; bots[slot].fire_t = 0;
}

/* -- debris and pickup rings -------------------------------------- */
static t3_instanced_mesh *debris_mesh;
static t3_mesh *pickup_rings[MAX_DEBRIS];

static void debris_init(void) {
  t3_material *m = t3_mesh_basic_material_new(0xffffff);
  debris_mesh = t3_instanced_mesh_new(g_box, m, MAX_DEBRIS);
  debris_mesh->mesh.base.frustum_culled = false;
  t3_object_add(&S->base, debris_mesh); t3_release(debris_mesh); t3_release(m);
  t3_mat4 zero; t3_mat4_compose(&zero, (t3_vec3){ 0, -5, 0 }, t3_quat_identity(), (t3_vec3){ 0.001f, 0.001f, 0.001f });
  for (int i = 0; i < MAX_DEBRIS; i++) {
    t3_instanced_mesh_set_matrix_at(debris_mesh, i, &zero);
    t3_instanced_mesh_set_color_at(debris_mesh, i, t3_color_hex(C_STEEL));
    t3_material *rm = glass(C_LIVE, 0.8f);
    pickup_rings[i] = t3_mesh_new(g_ring, rm); rot(pickup_rings[i], -PI / 2, 0, 0); t3_object_set_scale(pickup_rings[i], 0.5f, 0.5f, 1);
    t3_object_add(&S->base, pickup_rings[i]); t3_release(pickup_rings[i]); t3_release(rm);
    set_vis(pickup_rings[i], 0);
  }
}

/* debris_t.colour: 0 the owner's body paint, 1 the owner's armour tint, 2 steel (wheels),
 * 3 cardboard, 4 bench wood, 5 bone, 6 rust; kind picks the box proportions */
static uint32_t debris_colour(const debris_t *d) {
  switch (d->colour) {
    case 0: { if (d->owner < MAX_BOTS && bots[d->owner].built) return paint_colour(PAINT_SCHEMES[bots[d->owner].build.paint % PAINT_COUNT].body, bots[d->owner].seat_colour); return C_STEEL; }
    case 1: { if (d->owner < MAX_BOTS && bots[d->owner].built) { const armour_def_t *a = &ARMOURS[bots[d->owner].build.armour]; (void)a; uint32_t tints[AR_COUNT] = { C_STEEL, 0xb08a55, 0xd7dadc, 0xe9e5dc, 0x6f6c66, 0x8d97a3, 0x4f3a2d }; return tints[bots[d->owner].build.armour % AR_COUNT]; } return C_STEEL; }
    case 2: return 0x1d1915;
    case 3: return 0xa0773f;
    case 4: return 0xa57c4f;
    case 5: return C_BONE;
    default: return C_RUST;
  }
}

static void sync_debris(const match_t *m, float alpha) {
  (void)alpha;
  for (int i = 0; i < MAX_DEBRIS; i++) {
    const debris_t *d = &m->debris[i];
    t3_mat4 mat4;
    if (i >= m->ndebris || d->life <= 0) {
      t3_mat4_compose(&mat4, (t3_vec3){ 0, -5, 0 }, t3_quat_identity(), (t3_vec3){ 0.001f, 0.001f, 0.001f });
      t3_instanced_mesh_set_matrix_at(debris_mesh, i, &mat4);
      set_vis(pickup_rings[i], 0);
      continue;
    }
    float fade = d->life < J_DEBRIS_FADE ? (float)d->life / J_DEBRIS_FADE : 1;
    t3_vec3 sc = d->kind == DB_WHEEL ? (t3_vec3){ 0.3f, 0.3f, 0.14f } : d->kind == DB_CHIP ? (t3_vec3){ 0.12f, 0.06f, 0.12f } : d->kind == DB_CARDBOARD ? (t3_vec3){ 0.4f, 0.25f, 0.35f } : d->kind == DB_BENCH ? (t3_vec3){ 0.5f, 0.08f, 0.3f } : (t3_vec3){ 0.45f, 0.04f, 0.35f };
    sc.x *= fade; sc.y *= fade; sc.z *= fade;
    t3_quat q = t3_quat_from_axis_angle((t3_vec3){ 0.3f, 1, 0.2f }, d->spin * (m->frame + alpha) * DT * 2.0f + i);
    t3_mat4_compose(&mat4, (t3_vec3){ d->x, d->y + sc.y / 2 + 0.01f, d->z }, q, sc);
    t3_instanced_mesh_set_matrix_at(debris_mesh, i, &mat4);
    t3_instanced_mesh_set_color_at(debris_mesh, i, t3_color_hex(debris_colour(d)));
    set_vis(pickup_rings[i], d->pickup);
    if (d->pickup) { t3_object_set_position(pickup_rings[i], d->x, 0.02f, d->z); float ps = 0.5f + 0.06f * sinf(world_t * 5 + i); t3_object_set_scale(pickup_rings[i], ps, ps, 1); }
  }
  debris_mesh->instance_matrix->version++; if (debris_mesh->instance_color) debris_mesh->instance_color->version++;
}

/* -- hazards, every frame ----------------------------------------- */
static void sync_hazards(const match_t *m, float dt) {
  if (attached_idx < 0) return;
  warena_t *w = &arenas[attached_idx];
  const arena_def_t *a = w->def;
  float pulse = 0.5f + 0.5f * sinf(world_t * TAU * J_HAZARD_TELL_HZ);
  for (int i = 0; i < a->nhaz && i < MAX_HAZ; i++) {
    whaz_t *hz = &w->haz[i];
    const hazard_def_t *hd = &a->haz[i];
    const hazard_state_t *st = m ? &m->haz[i] : 0;
    int active = st ? st->active : 0, tell = st ? st->tell : 0;
    float phase = st ? st->phase_t : 0;
    if (!st && hd->period > 0) { hazard_phase(hd, world_t, &phase); active = hazard_phase(hd, world_t, 0) == 1; tell = hazard_phase(hd, world_t, 0) == 2; }
    switch (hz->type) {
      case HZ_PIT: {
        float open = active ? fminf(1, phase / 0.4f) : (tell ? 0 : fmaxf(0, 1 - (phase - hd->active_s) / 0.5f));
        if (!active && !tell && phase > hd->active_s + 0.5f) open = 0;
        /* the doors drop under the floor and slide apart */
        float drop = -0.12f * fminf(1, open * 4);
        t3_object_set_position(hz->mov, -open * hd->w / 2, drop, 0);
        t3_object_set_position(hz->mov2, open * hd->w / 2, drop, 0);
        hz->tell_m->emissive_intensity = 0.3f + 2.5f * (tell ? pulse : (active ? 1 : 0));
        break; }
      case HZ_PULVERIZER: {
        float target = active ? (phase < 0.2f ? -1.1f + phase / 0.2f * 1.25f : (phase < 0.4f ? 0.15f - (phase - 0.2f) / 0.2f * 1.25f : -1.1f)) : -1.1f;
        if (st && st->cooldown > 0 && !active) target = -1.1f;
        hz->swing += (target - hz->swing) * fminf(1, dt * 30);
        t3_object_set_rotation(hz->mov, hz->swing + (tell ? sinf(world_t * 30) * 0.03f : 0), 0, 0);
        hz->tell_m->emissive_intensity = tell ? 1.0f + 2.0f * pulse : 0;
        break; }
      case HZ_KILLSAW: {
        float up = active ? fminf(1, phase / 0.25f) : 0;
        if (active && phase > hd->active_s - 0.25f) up = fmaxf(0, (hd->active_s - phase) / 0.25f);
        t3_object_set_position(hz->mov, 0, -0.42f + up * 0.6f, 0);
        hz->spin += (active ? 45.0f : 8.0f) * dt;
        for (int c = 0; c < hz->mov->child_count; c++) t3_object_set_rotation(hz->mov->children[c], hz->spin, 0, 0);
        hz->tell_m->emissive_intensity = tell ? 2.5f * pulse : (active ? 1.0f : 0);
        break; }
      case HZ_FLAME: {
        float on = active ? 1 : 0;
        float fl = 1 + 0.2f * sinf(world_t * 41 + i) + 0.1f * sinf(world_t * 67);
        hz->glow_m->opacity += (on * 0.6f - hz->glow_m->opacity) * fminf(1, dt * 12);
        ((t3_mesh *)hz->mov2)->materials[0]->opacity = hz->glow_m->opacity * 0.9f;
        t3_object_set_scale(hz->mov, fl, 0.8f + 0.25f * fl * on + 0.2f, fl);
        hz->tell_m->emissive_intensity = tell ? 2.0f * pulse : (active ? 2.0f : 0);
        break; }
      case HZ_SCREW: {
        hz->spin += 6.0f * dt;
        t3_object_set_rotation(hz->mov, hz->spin, 0, 0);
        break; }
      case HZ_BUMPER: {
        if (hz->lamp > 0) hz->lamp -= dt;
        hz->tell_m->emissive_intensity = hz->lamp > 0 ? 3.0f : 0.2f;
        if (m && i < 32 && m->prop_broken[0] == 0) { /* roof bumpers break: the sim reports through haz[i].broken */ }
        if (st && st->broken) set_vis(hz->g, 0);
        break; }
      case HZ_FLOORFLIP: {
        float ang = active ? -0.9f * fminf(1, phase / 0.1f) * fmaxf(0, 1 - fmaxf(0, phase - 0.25f) / 0.4f) : 0;
        t3_object_set_rotation(hz->mov, ang, 0, 0);
        hz->tell_m->emissive_intensity = tell ? 2.5f * pulse : 0.3f;
        break; }
      case HZ_DROPZONE: {
        t3_mesh *sh = (t3_mesh *)hz->mov2;
        float grow = tell ? fminf(1, (hd->tell_s > 0 ? (hd->tell_s - (hd->period - phase)) / hd->tell_s : 1)) : (active ? 1 : 0);
        sh->materials[0]->opacity = 0.5f * grow;
        t3_object_set_scale(sh, 0.9f * grow + 0.01f, 0.9f * grow + 0.01f, 1);
        if (active) { set_vis(hz->mov, 1); float t = fminf(1, phase / 0.35f); t3_object_set_position(hz->mov, 0, 6 * (1 - t) * (1 - t), 0); }
        else set_vis(hz->mov, 0);
        break; }
      case HZ_LASER: {
        float x = active ? -a->w / 2 + (phase / fmaxf(0.01f, hd->active_s)) * a->w : -a->w / 2;
        t3_object_set_position(hz->mov, x, 0.3f, 0);
        hz->glow_m->opacity = active ? 0.9f : (tell ? 0.25f * pulse : 0);
        hz->tell_m->emissive_intensity = tell ? 1.0f + 2.0f * pulse : (active ? 3.0f : 0.6f);
        break; }
      case HZ_WIND: {
        float gust = active ? 1 : (tell ? 0.4f * pulse : 0.1f);
        t3_object_set_rotation(hz->mov, 0, hd->ang, -0.3f + sinf(world_t * 9) * 0.15f * gust);
        break; }
      default: break;
    }
  }
  /* flags snap during a wind tell or gust */
  float wind = 0.1f;
  if (m) wind += fminf(1, sqrtf(m->wind_x * m->wind_x + m->wind_z * m->wind_z) * 2);
  for (int i = 0; i < 32; i++) if (w->flags[i]) t3_object_set_rotation(w->flags[i], 0, 0.0f + sinf(world_t * 7 + i) * 0.4f * wind, 0);
  /* broken props vanish */
  if (m) for (int i = 0; i < a->nprops && i < 32; i++) if (w->props[i]) set_vis(w->props[i], !m->prop_broken[i]);
  /* the hill */
  if (w->hill) {
    int on = m && m->mode == MODE_HILL;
    set_vis(w->hill, on);
    if (on) { int k = m->hill & 3; t3_object_set_position(w->hill, a->hill[k][0], 0.02f, a->hill[k][1]); }
  }
  /* the OPS gate follows the OPS state */
  if (m) { float target = (m->ops_state == OPS_PARKED) ? 0 : 1; w->gate_open += (target - w->gate_open) * fminf(1, dt * 4); world_ops_gate(w->gate_open); }
}

/* -- attach, sync, tick ------------------------------------------- */
static void set_lights(const arena_def_t *a) {
  if (!a) return;
  sun->color = t3_color_hex(a->sun); sun->intensity = a->sun_i;
  hemi->color = t3_color_hex(a->hemi_sky); hemi->ground_color = t3_color_hex(a->hemi_ground); hemi->intensity = a->hemi_i;
  point_a->color = t3_color_hex(a->point_a); point_a->intensity = a->point_a_i;   /* 0 keeps the light vector, not a removal */
  point_b->color = t3_color_hex(a->point_b); point_b->intensity = a->point_b_i;
  t3_object_set_position(point_a, a->pa[0], a->pa[1], a->pa[2]);
  t3_object_set_position(point_b, a->pb[0], a->pb[1], a->pb[2]);
}

t3_scene *world_scene(void) { return S; }
const arena_def_t *world_arena(void) { return attached; }

void world_detach_all(void) {
  if (attached_idx >= 0 && arenas[attached_idx].root) t3_object_remove(&S->base, arenas[attached_idx].root);
  if (attached == &GARAGE_BAY && garage_root) t3_object_remove(&S->base, garage_root);
  attached = 0; attached_idx = -1;
}

void world_attach_arena(int arena) {
  arena %= ARENA_COUNT; if (arena < 0) arena = 0;
  if (attached_idx == arena) return;
  world_detach_all();
  if (!arenas[arena].built) world_build_arena(arena, &ARENAS[arena]);
  t3_object_add(&S->base, arenas[arena].root);
  attached = arenas[arena].def; attached_idx = arena;
  set_lights(attached);
  t3_object_set_position(sun, 6, 12, 4);
}

void world_attach_garage(void) {
  if (attached == &GARAGE_BAY) return;
  world_detach_all();
  t3_object_add(&S->base, garage_root);
  attached = &GARAGE_BAY;
  set_lights(attached);
  t3_object_set_position(sun, 22, 12, 6);
}

void world_tick(float dt) {
  if (dt <= 0) dt = 1.f / 60; if (dt > 0.1f) dt = 0.1f;
  wall_dt = dt; ticked = 1;
  world_t += dt;
  for (int t = 0; t < 4; t++) {
    if (tt_pause[t] > 0) tt_pause[t] -= dt;
    else if (!tt_locked[t]) tt_angle[t] += 20.0f * PI / 180 * dt;
    if (turntables[t]) t3_object_set_rotation(turntables[t], 0, tt_angle[t], 0);
  }
}

void world_sync(const match_t *m, float alpha) {
#ifdef RTR_FAKE_SIM
  render_fake_tick((match_t *)m);
#endif
  if (!ticked) world_tick(1.f / 60);
  ticked = 0;
  float dt = wall_dt;
  if (alpha < 0) alpha = 0; if (alpha > 1) alpha = 1;
  int in_match = m && m->nbots > 0 && attached && attached != &GARAGE_BAY;
  for (int i = 0; i < MAX_BOTS; i++) {
    wbot_t *b = &bots[i];
    if (!b->root || !b->built) continue;
    if (in_match && i < m->nbots) {
      const bot_t *bt = &m->bots[i];
      if (!b->visible) world_bot_visible(i, 1);
      sync_bot(b, bt, alpha, dt);
    } else if (b->visible) {
      /* posed outside a match: the turntable carries it */
      if (attached == &GARAGE_BAY) {
        int t = b->seat < 4 ? b->seat : 3;
        for (int k = 0; k < 4; k++) if (fabsf(b->x - 12.0f * k) < 1) t = k;
        place_root(b, 12.0f * t, 0.17f, 0, tt_locked[t] ? 0 : tt_angle[t], 0);
      }
      pose_test_fire(b, dt);
#ifdef RTR_FAKE_DENTS
      { uint8_t st[PN_COUNT]; for (int p = 0; p < PN_COUNT; p++) st[p] = (uint8_t)((i + p) % 4); sync_panels(b, st); }
#else
      sync_panels(b, 0);
#endif
    }
  }
  if (in_match) { sync_debris(m, alpha); sync_hazards(m, dt); }
  else if (attached && attached != &GARAGE_BAY) sync_hazards(0, dt);
  /* debris is hidden outside a match */
  if (!in_match && debris_mesh) { t3_mat4 zero; t3_mat4_compose(&zero, (t3_vec3){ 0, -5, 0 }, t3_quat_identity(), (t3_vec3){ 0.001f, 0.001f, 0.001f }); for (int i = 0; i < MAX_DEBRIS; i++) { t3_instanced_mesh_set_matrix_at(debris_mesh, i, &zero); set_vis(pickup_rings[i], 0); } debris_mesh->instance_matrix->version++; }
}

/* -- turntables --------------------------------------------------- */
void world_turntable(int table, float angle) { if (table < 0 || table > 3) return; tt_angle[table] = angle; tt_pause[table] = 0.5f; }
float world_turntable_angle(int table) { return table >= 0 && table < 4 ? tt_angle[table] : 0; }
void world_turntable_pos(int table, float *x, float *y, float *z) { if (x) *x = 12.0f * (table & 3); if (y) *y = 0.17f; if (z) *z = 0; }
void world_turntable_lock(int table, int locked) {
  if (table < 0 || table > 3) return;
  tt_locked[table] = locked;
  if (locked) { tt_angle[table] = 0; if (turntables[table]) t3_object_set_rotation(turntables[table], 0, 0, 0); }
}

/* -- visibility, scorch, gate, closing, crowd, compile ------------ */
static int vis_all_override;
void world_vis_all(void) { vis_all_override = 1; }

void world_set_cell_visibility(int cell, const arena_def_t *a) {
  if (attached_idx < 0 || !a) return;
  warena_t *w = &arenas[attached_idx];
  uint16_t bits = (cell < 0 || cell > 15 || vis_all_override) ? 0xffff : a->vis[cell];
  for (int c = 0; c < 16; c++) if (w->cells[c]) w->cells[c]->visible = (bits >> c) & 1;
}

void world_scorch(float x, float z, float r) {
  if (attached_idx < 0) return;
  warena_t *w = &arenas[attached_idx];
  if (G.frame - (uint32_t)w->scorch_frame < 30 && w->scorch_frame) return;
  w->scorch_frame = (int)G.frame;
  const arena_def_t *a = w->def;
  canvas_t c = { w->floor_tex->pixels, w->floor_w, w->floor_h };
  int cx = (int)((x + a->w / 2) / a->w * w->floor_w), cy = (int)((z + a->d / 2) / a->d * w->floor_h), cr = (int)(r / a->w * w->floor_w);
  cv_circle(&c, cx, cy, cr, 0x1d1915, 110);
  cv_circle(&c, cx, cy, cr * 2 / 3, 0x0e0c0a, 90);
  w->floor_tex->version++;
}

void world_ops_gate(float open) {
  if (attached_idx < 0 || !arenas[attached_idx].gate) return;
  warena_t *w = &arenas[attached_idx];
  const arena_def_t *a = w->def;
  float sx = a->ops_zone[0] < 0 ? 1 : -1, sz = a->ops_zone[1] < 0 ? 1 : -1;
  if (w->gate->child_count >= 2) {
    t3_object_set_position(w->gate->children[0], sx * 1.0f, 0, -sz * open * 2.1f);     /* slides along the wall, away from the centre */
    t3_object_set_position(w->gate->children[1], -sx * open * 2.1f, 0, sz * 1.0f);
  }
}

void world_closing(int on) {
  if (attached_idx < 0 || !arenas[attached_idx].rail_m) return;
  arenas[attached_idx].rail_m->emissive_intensity = on ? 1.2f : 0;
}

void world_crowd(int frame, float level) {
  if (attached_idx < 0) return;
  warena_t *w = &arenas[attached_idx];
  if (frame < 0) frame = 0; if (frame > 2) frame = 2;
  for (int f = 0; f < 3; f++) if (w->crowd[f]) set_vis(w->crowd[f], f == frame);
  (void)level;
}

void world_compile_all(void) {
  if (!RTR_R || !S) return;
  t3_camera *c = cam_get(0);
  const arena_def_t *was = attached; int was_idx = attached_idx;
  for (int i = 0; i < ARENA_COUNT; i++) { world_attach_arena(i); t3_renderer_compile(RTR_R, S, c); }
  world_attach_garage(); t3_renderer_compile(RTR_R, S, c);
  if (was_idx >= 0) world_attach_arena(was_idx); else if (was == &GARAGE_BAY) world_attach_garage(); else world_detach_all();
}

/* -- init --------------------------------------------------------- */
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

  g_box = t3_box_geometry_new(1, 1, 1, 1, 1, 1);
  g_cyl12 = t3_cylinder_geometry_new(1, 1, 1, 12, 1, false);
  g_cyl8 = t3_cylinder_geometry_new(1, 1, 1, 8, 1, false);
  g_disc = t3_cylinder_geometry_new(1, 1, 1, 20, 1, false);
  g_circle = t3_circle_geometry_new(1, 16);
  g_plane = t3_plane_geometry_new(1, 1, 1, 1);
  g_ring = t3_ring_geometry_new(0.9f, 1.0f, 24, 1);
  g_sphere = t3_sphere_geometry_new(1, 10, 7);
  g_cone = t3_cone_geometry_new(1, 1, 10, 1, false);
  g_torus = t3_torus_geometry_new(1, 0.16f, 8, 18, TAU);
  make_textures();
  build_garage();
  for (int i = 0; i < ARENA_COUNT; i++) world_build_arena(i, &ARENAS[i]);
  debris_init();
  for (int i = 0; i < MAX_BOTS; i++) bot_alloc(&bots[i], i);
  world_attach_arena(AR_BAY);
}
