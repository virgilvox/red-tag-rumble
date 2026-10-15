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
#include "wasmcart.h"
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
static t3_geometry *g_box, *g_cyl12, *g_disc, *g_circle, *g_plane, *g_ring, *g_sphere, *g_cone;

/* the cache compares the whole parameter tuple, never a hash, so two related
 * paints can never share a slot; it grows as needed, so a late entry is never a
 * fresh material per call */
typedef struct { uint32_t c, emis; float rough, metal, ei; int kind; t3_material *m; } mat_slot_t;
static mat_slot_t *mats; static int nmats, cmats;

static t3_material *mat_ex(uint32_t c, float rough, float metal, uint32_t emis, float ei, int kind) {
  if (kind == 1) { rough = 0; metal = 0; emis = 0; ei = 0; }
  for (int i = 0; i < nmats; i++) {
    const mat_slot_t *s = &mats[i];
    if (s->c == c && s->kind == kind && s->rough == rough && s->metal == metal && s->emis == emis && s->ei == ei) return s->m;
  }
  t3_material *m = kind == 1 ? t3_mesh_basic_material_new(c) : t3_mesh_standard_material_new(c);
  if (kind == 2) m->vertex_colors = true;   /* the vertex coloured parts: one draw carries every colour of a part */
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
/* a vertex coloured standard material: the colour is the tint the vertex colours multiply */
static t3_material *vmat_c(uint32_t c, float rough, float metal) { return mat_ex(c, rough, metal, 0, 0, 2); }
static t3_material *vmat(float rough, float metal) { return vmat_c(0xffffff, rough, metal); }
#define VM_BODY   vmat(0.55f, 0.30f)       /* the chassis, its trim and the static drive parts */
#define VM_METAL  vmat(0.35f, 0.55f)       /* weapon parts: discs, heads, forks, jaws */
#define VM_RUBBER vmat(0.90f, 0.05f)       /* tyres with their hubs, skirts, legs */
/* a transparent material is unique to its user (opacity animates) */
static t3_material *glass(uint32_t c, float opacity) {
  t3_material *m = t3_mesh_basic_material_new(c);
  m->transparent = true; m->opacity = opacity; m->depth_write = false;
  return m;
}
/* a mesh that takes over the creation reference of its own material */
static t3_mesh *mesh_take(t3_geometry *g, t3_material *m) { t3_mesh *me = t3_mesh_new(g, m); t3_release(m); return me; }
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
/* a 3 x 4 affine transform, row major: the rotation in m[0..2], m[4..6], m[8..10], the translation in m[3], m[7], m[11] */
typedef struct { float m[12]; } xf_t;
static xf_t xf_ident(void) { xf_t x = { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 } }; return x; }
static xf_t xf_mul(const xf_t *a, const xf_t *b) {
  xf_t r;
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) r.m[i * 4 + j] = a->m[i * 4] * b->m[j] + a->m[i * 4 + 1] * b->m[4 + j] + a->m[i * 4 + 2] * b->m[8 + j];
    r.m[i * 4 + 3] = a->m[i * 4] * b->m[3] + a->m[i * 4 + 1] * b->m[7] + a->m[i * 4 + 2] * b->m[11] + a->m[i * 4 + 3];
  }
  return r;
}
/* translate then rotate by the three.js XYZ Euler (R = Rx Ry Rz), the order t3_object_set_rotation uses */
static xf_t xf_make(float x, float y, float z, float rx, float ry, float rz) {
  float cx = cosf(rx), sx = sinf(rx), cy = cosf(ry), sy = sinf(ry), cz = cosf(rz), sz = sinf(rz);
  xf_t r = { { cy * cz, -cy * sz, sy, x,
               cx * sz + sx * sy * cz, cx * cz - sx * sy * sz, -sx * cy, y,
               sx * sz - cx * sy * cz, sx * cz + cx * sy * sz, cx * cy, z } };
  return r;
}
static void xf_pt(const xf_t *x, const float *p, float *o) {
  float a = p[0], b = p[1], c = p[2];
  o[0] = x->m[0] * a + x->m[1] * b + x->m[2] * c + x->m[3];
  o[1] = x->m[4] * a + x->m[5] * b + x->m[6] * c + x->m[7];
  o[2] = x->m[8] * a + x->m[9] * b + x->m[10] * c + x->m[11];
}

/* a triangle soup builder with flat normals, per vertex colours and a transform stack. Points go in
 * through the current transform; colours are the current colour (soup_rgb) or per vertex (soup_tri_c),
 * in linear light as t3_color_hex gives them, so a vertex coloured part matches a material coloured one.
 * A soup made with soup_init_buf writes into caller owned arrays (the dynamic plate and decal soups) and
 * is never freed by soup_finish */
typedef struct { float *p, *n, *uv, *c; int cap, n_, owned, vcol; float rgb[3]; xf_t xf, stack[8]; int sp; } soup_t;
static void soup_reset(soup_t *s) { s->n_ = 0; s->rgb[0] = s->rgb[1] = s->rgb[2] = 1; s->xf = xf_ident(); s->sp = 0; }
static void soup_init(soup_t *s, int tris) {
  s->cap = tris * 3; s->owned = 1; s->vcol = 0;
  s->p = calloc((size_t)s->cap * 3, 4); s->n = calloc((size_t)s->cap * 3, 4); s->uv = calloc((size_t)s->cap * 2, 4); s->c = calloc((size_t)s->cap * 3, 4);
  soup_reset(s);
}
static void soup_init_vcol(soup_t *s, int tris) { soup_init(s, tris); s->vcol = 1; }
static void soup_rgb(soup_t *s, uint32_t hex) { t3_color c = t3_color_hex(hex); s->rgb[0] = c.r; s->rgb[1] = c.g; s->rgb[2] = c.b; }
static void soup_push(soup_t *s, const xf_t *local) { if (s->sp < 8) s->stack[s->sp++] = s->xf; s->xf = xf_mul(&s->xf, local); }
static void soup_push_at(soup_t *s, float x, float y, float z, float rx, float ry, float rz) { xf_t l = xf_make(x, y, z, rx, ry, rz); soup_push(s, &l); }
static void soup_pop(soup_t *s) { if (s->sp > 0) s->xf = s->stack[--s->sp]; }
static void soup_emit(soup_t *s, const float *a, const float *b, const float *c, const float *ca, const float *cb, const float *cc, const float *uva, const float *uvb, const float *uvc) {
  if (s->n_ + 3 > s->cap) return;
  float w[3][3]; xf_pt(&s->xf, a, w[0]); xf_pt(&s->xf, b, w[1]); xf_pt(&s->xf, c, w[2]);
  float ux = w[1][0] - w[0][0], uy = w[1][1] - w[0][1], uz = w[1][2] - w[0][2], vx = w[2][0] - w[0][0], vy = w[2][1] - w[0][1], vz = w[2][2] - w[0][2];
  float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx, l = sqrtf(nx * nx + ny * ny + nz * nz);
  if (l > 0) { nx /= l; ny /= l; nz /= l; }
  const float *col[3] = { ca ? ca : s->rgb, cb ? cb : s->rgb, cc ? cc : s->rgb }, *uv[3] = { uva, uvb, uvc };
  for (int i = 0; i < 3; i++) {
    int k = s->n_++;
    s->p[k * 3] = w[i][0]; s->p[k * 3 + 1] = w[i][1]; s->p[k * 3 + 2] = w[i][2];
    s->n[k * 3] = nx; s->n[k * 3 + 1] = ny; s->n[k * 3 + 2] = nz;
    s->c[k * 3] = col[i][0]; s->c[k * 3 + 1] = col[i][1]; s->c[k * 3 + 2] = col[i][2];
    if (uv[i]) { s->uv[k * 2] = uv[i][0]; s->uv[k * 2 + 1] = uv[i][1]; }
    else { s->uv[k * 2] = w[i][0] + 0.5f; s->uv[k * 2 + 1] = w[i][2] + 0.5f; }
  }
}
static void soup_tri(soup_t *s, const float *a, const float *b, const float *c) { soup_emit(s, a, b, c, 0, 0, 0, 0, 0, 0); }
static void soup_quad(soup_t *s, const float *a, const float *b, const float *c, const float *d) { soup_tri(s, a, b, c); soup_tri(s, a, c, d); }
/* a triangle wound so its normal points away from ctr (local space, before the transform): convex parts
 * never need their winding worked out by hand */
static int faces_out(const float *a, const float *b, const float *c, const float *ctr) {
  float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
  float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
  float mx = (a[0] + b[0] + c[0]) / 3 - ctr[0], my = (a[1] + b[1] + c[1]) / 3 - ctr[1], mz = (a[2] + b[2] + c[2]) / 3 - ctr[2];
  return nx * mx + ny * my + nz * mz >= 0;
}
static void soup_tri_out(soup_t *s, const float *a, const float *b, const float *c, const float *ctr) {
  if (faces_out(a, b, c, ctr)) soup_tri(s, a, b, c); else soup_tri(s, a, c, b);
}
static void soup_quad_out(soup_t *s, const float *a, const float *b, const float *c, const float *d, const float *ctr) {
  soup_tri_out(s, a, b, c, ctr); soup_tri_out(s, a, c, d, ctr);
}
static t3_geometry *soup_finish(soup_t *s) {
  t3_geometry *g = t3_geometry_new();
  /* the geometry retains each attribute, so the creation ref goes: a wedge is rebuilt every garage
   * and every match, and with the ref kept its three arrays outlived the geometry */
  t3_attribute *p = t3_attribute_new(T3_FLOAT32, s->p, s->n_, 3), *n = t3_attribute_new(T3_FLOAT32, s->n, s->n_, 3), *uv = t3_attribute_new(T3_FLOAT32, s->uv, s->n_, 2);
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, p); t3_release(p);
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, n); t3_release(n);
  t3_geometry_set_attribute(g, T3_ATTR_UV, uv); t3_release(uv);
  if (s->vcol) { t3_attribute *c = t3_attribute_new(T3_FLOAT32, s->c, s->n_, 3); t3_geometry_set_attribute(g, T3_ATTR_COLOR, c); t3_release(c); }
  t3_geometry_compute_bounding_sphere(g);
  t3_geometry_compute_bounding_box(g);
  if (s->owned) { free(s->p); free(s->n); free(s->uv); free(s->c); }
  return g;
}

/* a wedge: footprint w (x) by d (z), the back face (z = -d/2) is h tall, sloping to the floor at the front.
 * Every face is wound counter clockwise seen from outside, so soup_tri's (b - a) x (c - a)
 * normal points out of the solid and three.c's front side cull keeps the outside */
static t3_geometry *wedge_geometry(float w, float d, float h) {
  soup_t s; soup_init(&s, 8);
  float x0 = -w / 2, x1 = w / 2, z0 = -d / 2, z1 = d / 2;
  float a[3] = { x0, 0, z0 }, b[3] = { x1, 0, z0 }, c[3] = { x1, 0, z1 }, dd[3] = { x0, 0, z1 };
  float e[3] = { x0, h, z0 }, f[3] = { x1, h, z0 };
  soup_quad(&s, a, b, c, dd);         /* bottom, -y */
  soup_quad(&s, e, dd, c, f);         /* slope, +y +z */
  soup_quad(&s, a, e, f, b);          /* back, -z */
  soup_tri(&s, a, dd, e);             /* left, -x */
  soup_tri(&s, b, f, c);              /* right, +x */
  return soup_finish(&s);
}

/* -- the geometry kit (graphics 3.1) -------------------------------- */
/* every primitive appends to a soup at the soup's current transform, centred on the local origin unless
 * it says otherwise, and winds its faces outward itself */

/* a box with every edge cut at 45 degrees by c (44 triangles; c 0 gives the plain 12) */
static void sp_cbox(soup_t *s, float w, float h, float d, float c) {
  float X = w / 2, Y = h / 2, Z = d / 2, ctr[3] = { 0, 0, 0 };
  float m = w < h ? w : h, big = w > h ? w : h; if (d < m) m = d; if (d > big) big = d;
  if (c > m * 0.45f) c = m * 0.45f;
  if (big < 0.15f || m < 0.025f) c = 0;          /* a bevel on a small part is under a pixel: 12 triangles, not 44 */
  if (c <= 0.0005f) {
    float v[8][3];
    for (int k = 0; k < 8; k++) { v[k][0] = k & 1 ? X : -X; v[k][1] = k & 2 ? Y : -Y; v[k][2] = k & 4 ? Z : -Z; }
    soup_quad_out(s, v[0], v[2], v[6], v[4], ctr); soup_quad_out(s, v[1], v[3], v[7], v[5], ctr);
    soup_quad_out(s, v[0], v[1], v[5], v[4], ctr); soup_quad_out(s, v[2], v[3], v[7], v[6], ctr);
    soup_quad_out(s, v[0], v[1], v[3], v[2], ctr); soup_quad_out(s, v[4], v[5], v[7], v[6], ctr);
    return;
  }
  float P[8][3][3];
  for (int k = 0; k < 8; k++) {
    float sx = k & 1 ? 1 : -1, sy = k & 2 ? 1 : -1, sz = k & 4 ? 1 : -1;
    P[k][0][0] = sx * X; P[k][0][1] = sy * (Y - c); P[k][0][2] = sz * (Z - c);
    P[k][1][0] = sx * (X - c); P[k][1][1] = sy * Y; P[k][1][2] = sz * (Z - c);
    P[k][2][0] = sx * (X - c); P[k][2][1] = sy * (Y - c); P[k][2][2] = sz * Z;
  }
  for (int f = 0; f < 2; f++) {
    int bx = f, by = f << 1, bz = f << 2;
    /* the x, y and z faces: corners cycled round the face */
    soup_quad_out(s, P[bx][0], P[bx | 2][0], P[bx | 6][0], P[bx | 4][0], ctr);
    soup_quad_out(s, P[by][1], P[by | 1][1], P[by | 5][1], P[by | 4][1], ctr);
    soup_quad_out(s, P[bz][2], P[bz | 1][2], P[bz | 3][2], P[bz | 2][2], ctr);
  }
  for (int e = 0; e < 4; e++) {
    int a = e & 1, b = (e >> 1) & 1;
    int kx = (a << 1) | (b << 2), ky = a | (b << 2), kz = a | (b << 1);
    soup_quad_out(s, P[kx][1], P[kx | 1][1], P[kx | 1][2], P[kx][2], ctr);   /* edges along x */
    soup_quad_out(s, P[ky][0], P[ky | 2][0], P[ky | 2][2], P[ky][2], ctr);   /* along y */
    soup_quad_out(s, P[kz][0], P[kz | 4][0], P[kz | 4][1], P[kz][1], ctr);   /* along z */
  }
  for (int k = 0; k < 8; k++) soup_tri_out(s, P[k][0], P[k][1], P[k][2], ctr);
}
static void sp_cbox_at(soup_t *s, float w, float h, float d, float c, float x, float y, float z) { soup_push_at(s, x, y, z, 0, 0, 0); sp_cbox(s, w, h, d, c); soup_pop(s); }
static void sp_box_at(soup_t *s, float w, float h, float d, float x, float y, float z) { sp_cbox_at(s, w, h, d, 0, x, y, z); }

/* a cylinder of radius r and length h along axis 0 x, 1 y or 2 z, capped; r2 differs from r for a cone frustum */
static void sp_cyl2(soup_t *s, float r, float r2, float h, int seg, int axis) {
  if (axis == 0) soup_push_at(s, 0, 0, 0, 0, 0, -PI / 2); else if (axis == 2) soup_push_at(s, 0, 0, 0, PI / 2, 0, 0); else soup_push_at(s, 0, 0, 0, 0, 0, 0);
  float ctr[3] = { 0, 0, 0 }, top[3] = { 0, h / 2, 0 }, bot[3] = { 0, -h / 2, 0 };
  for (int i = 0; i < seg; i++) {
    float a0 = i * TAU / seg, a1 = (i + 1) * TAU / seg;
    float p0[3] = { sinf(a0) * r, -h / 2, cosf(a0) * r }, p1[3] = { sinf(a1) * r, -h / 2, cosf(a1) * r };
    float q0[3] = { sinf(a0) * r2, h / 2, cosf(a0) * r2 }, q1[3] = { sinf(a1) * r2, h / 2, cosf(a1) * r2 };
    soup_quad_out(s, p0, p1, q1, q0, ctr);
    if (r2 > 0.0005f) soup_tri_out(s, top, q0, q1, bot);
    if (r > 0.0005f) soup_tri_out(s, bot, p0, p1, top);
  }
  soup_pop(s);
}
static void sp_cyl(soup_t *s, float r, float h, int seg, int axis) { sp_cyl2(s, r, r, h, seg, axis); }
static void sp_cyl_at(soup_t *s, float r, float h, int seg, int axis, float x, float y, float z) { soup_push_at(s, x, y, z, 0, 0, 0); sp_cyl(s, r, h, seg, axis); soup_pop(s); }

/* a bolt head on a face that looks along +z: a low four sided pyramid, base radius r, height t (4 triangles;
 * a hex head is the same few pixels in a quadrant at half again the cost) */
static void sp_bolt(soup_t *s, float r, float t) {
  float apex[3] = { 0, 0, t }, ctr[3] = { 0, 0, -t };
  for (int i = 0; i < 4; i++) {
    float a0 = i * PI / 2 + PI / 4, a1 = (i + 1) * PI / 2 + PI / 4;
    float p0[3] = { cosf(a0) * r, sinf(a0) * r, 0 }, p1[3] = { cosf(a1) * r, sinf(a1) * r, 0 };
    soup_tri_out(s, p0, p1, apex, ctr);
  }
}
/* a flat hexagon facing +z (a bolt hole), 4 triangles */
static void sp_hex(soup_t *s, float r) {
  float h[6][3], ctr[3] = { 0, 0, -1 };
  for (int i = 0; i < 6; i++) { float a = i * PI / 3 + PI / 6; h[i][0] = cosf(a) * r; h[i][1] = sinf(a) * r; h[i][2] = 0; }
  for (int i = 1; i < 5; i++) soup_tri_out(s, h[0], h[i], h[i + 1], ctr);
}
/* a w x h quad in the xy plane facing +z */
static void sp_quad(soup_t *s, float w, float h) {
  float a[3] = { -w / 2, -h / 2, 0 }, b[3] = { w / 2, -h / 2, 0 }, c[3] = { w / 2, h / 2, 0 }, d[3] = { -w / 2, h / 2, 0 }, ctr[3] = { 0, 0, -1 };
  soup_quad_out(s, a, b, c, d, ctr);
}
/* a wedge on y = 0: footprint w by d, the back face (z = -d / 2) h tall, sloping to the floor at the front */
static void sp_wedge(soup_t *s, float w, float d, float h) {
  float x0 = -w / 2, x1 = w / 2, z0 = -d / 2, z1 = d / 2, ctr[3] = { 0, h / 3, -d / 6 };
  float a[3] = { x0, 0, z0 }, b[3] = { x1, 0, z0 }, c[3] = { x1, 0, z1 }, dd[3] = { x0, 0, z1 }, e[3] = { x0, h, z0 }, f[3] = { x1, h, z0 };
  soup_quad_out(s, a, b, c, dd, ctr); soup_quad_out(s, e, dd, c, f, ctr); soup_quad_out(s, a, e, f, b, ctr);
  soup_tri_out(s, a, dd, e, ctr); soup_tri_out(s, b, f, c, ctr);
}
/* a lathe round y of a profile of (r, y) points walked bottom centre, out, up, back in to the top (the
 * outward side is to the right of the walk), seg segments over phil from phi0 (three.js LatheGeometry angles) */
static void sp_lathe(soup_t *s, const float (*pr)[2], int n, int seg, float phi0, float phil) {
  for (int j = 0; j < seg; j++) {
    float a0 = phi0 + j * phil / seg, a1 = phi0 + (j + 1) * phil / seg, am = (a0 + a1) / 2;
    for (int i = 0; i + 1 < n; i++) {
      float r0 = pr[i][0], y0 = pr[i][1], r1 = pr[i + 1][0], y1 = pr[i + 1][1];
      float p00[3] = { sinf(a0) * r0, y0, cosf(a0) * r0 }, p01[3] = { sinf(a1) * r0, y0, cosf(a1) * r0 };
      float p10[3] = { sinf(a0) * r1, y1, cosf(a0) * r1 }, p11[3] = { sinf(a1) * r1, y1, cosf(a1) * r1 };
      /* the profile edge's outward normal (dy, -dr), and a point just inside it to wind against */
      float nr = y1 - y0, ny = -(r1 - r0), nl = sqrtf(nr * nr + ny * ny); if (nl <= 0) continue; nr /= nl; ny /= nl;
      float rm = (r0 + r1) / 2 - nr * 0.01f, ym = (y0 + y1) / 2 - ny * 0.01f, ctr[3] = { sinf(am) * rm, ym, cosf(am) * rm };
      if (r0 > 0.0005f) soup_tri_out(s, p00, p01, p11, ctr);
      if (r1 > 0.0005f) soup_tri_out(s, p00, p11, p10, ctr);
    }
  }
}
/* n wedge teeth round the y axis at radius r: each tw wide (tangential), th tall, reaching td out, the
 * leading face slanted by tilt radians */
static void sp_teeth(soup_t *s, int n, float r, float tw, float th, float td, float tilt) {
  for (int i = 0; i < n; i++) {
    soup_push_at(s, 0, 0, 0, 0, i * TAU / n, 0);
    soup_push_at(s, 0, 0, r + td / 2, 0, tilt, 0);
    soup_push_at(s, 0, -th / 2, 0, 0, PI / 2, 0);      /* the wedge's slope runs tangentially: the cutting face */
    sp_wedge(s, td, tw, th);
    soup_pop(s); soup_pop(s); soup_pop(s);
  }
}
/* a gear: a 2n point star prism round the y axis, radii r (tips) and ri, thickness t */
static void sp_gear(soup_t *s, int n, float r, float ri, float t) {
  int m = n * 2; float ctr[3] = { 0, 0, 0 }, top[3] = { 0, t / 2, 0 }, bot[3] = { 0, -t / 2, 0 };
  for (int i = 0; i < m; i++) {
    float a0 = i * TAU / m, a1 = (i + 1) * TAU / m, ra = i & 1 ? ri : r, rb = i & 1 ? r : ri;
    float p0[3] = { sinf(a0) * ra, -t / 2, cosf(a0) * ra }, p1[3] = { sinf(a1) * rb, -t / 2, cosf(a1) * rb };
    float q0[3] = { sinf(a0) * ra, t / 2, cosf(a0) * ra }, q1[3] = { sinf(a1) * rb, t / 2, cosf(a1) * rb };
    soup_quad_out(s, p0, p1, q1, q0, ctr);
    soup_tri_out(s, top, q0, q1, bot); soup_tri_out(s, bot, p0, p1, top);
  }
}

/* the cache of shared geometries, keyed by the whole parameter tuple (never a hash): a part that every
 * bot of a size uses (a tyre, a sprocket, a hub) is built once, kept for the session, and auto instanced
 * with every other use of it */
enum { GK_TYRE, GK_SPROCKET, GK_LEG_THIGH, GK_LEG_SHIN, GK_FAN, GK_HANG, GK_DEBRIS_PLATE, GK_DEBRIS_WHEEL, GK_BLUR, GK_RING, GK_SKIRT, GK_BELT, GK_DISC, GK_TEETH, GK_STAR, GK_QUAD };
typedef struct { int kind; float k[8]; t3_geometry *g; } geo_slot_t;
static geo_slot_t *geos; static int ngeos, cgeos;
static t3_geometry *geo_find(int kind, const float *k) {
  for (int i = 0; i < ngeos; i++) if (geos[i].kind == kind && !memcmp(geos[i].k, k, sizeof geos[i].k)) return geos[i].g;
  return 0;
}
static t3_geometry *geo_keep(int kind, const float *k, t3_geometry *g) {
  if (ngeos == cgeos) { cgeos = cgeos ? cgeos * 2 : 64; geos = realloc(geos, sizeof *geos * (size_t)cgeos); }
  geos[ngeos].kind = kind; memcpy(geos[ngeos].k, k, sizeof geos[ngeos].k); geos[ngeos].g = g; ngeos++;
  return g;
}

/* a soup's geometry as a mesh under parent; nothing when the soup is empty */
static t3_mesh *soup_mesh(void *parent, soup_t *s, t3_material *m) {
  if (s->n_ == 0) { if (s->owned) { free(s->p); free(s->n); free(s->uv); free(s->c); } return 0; }
  t3_geometry *g = soup_finish(s);
  t3_mesh *me = t3_mesh_new(g, m);
  t3_object_add(parent, me); t3_release(me); t3_release(g);
  return me;
}
/* a soup that ended up unused */
static void soup_free_empty(soup_t *s) { if (s->owned) { free(s->p); free(s->n); free(s->uv); free(s->c); } s->p = s->n = s->uv = s->c = 0; s->n_ = 0; }
/* a cached geometry as a mesh under parent at a place */
static t3_mesh *geo_mesh(void *parent, t3_geometry *g, t3_material *m, float x, float y, float z) {
  t3_mesh *me = t3_mesh_new(g, m);
  t3_object_set_position(me, x, y, z);
  t3_object_add(parent, me); t3_release(me);
  return me;
}
/* a textured transparent quad material of its own (its opacity animates) */
static t3_material *glass_map(t3_texture *t, float opacity) {
  t3_material *m = t3_mesh_basic_material_new(0xffffff);
  t3_material_set_map(m, t); m->transparent = true; m->opacity = opacity; m->depth_write = false;
  return m;
}

/* a quad geometry with its uv window into an atlas (u0, v0, u1, v1) */
static t3_geometry *quad_geo(float u0, float v0, float u1, float v1) {
  float key[8] = { u0, v0, u1, v1 };
  t3_geometry *g = geo_find(GK_QUAD, key);
  if (g) return g;
  soup_t s; soup_init(&s, 2);
  float a[3] = { -0.5f, -0.5f, 0 }, b[3] = { 0.5f, -0.5f, 0 }, c[3] = { 0.5f, 0.5f, 0 }, d[3] = { -0.5f, 0.5f, 0 };
  float ua[2] = { u0, v0 }, ub[2] = { u1, v0 }, uc[2] = { u1, v1 }, ud[2] = { u0, v1 };
  soup_emit(&s, a, b, c, 0, 0, 0, ua, ub, uc); soup_emit(&s, a, c, d, 0, 0, 0, ua, uc, ud);
  return geo_keep(GK_QUAD, key, soup_finish(&s));
}

/* the textures every bot shares (make_bot_textures) */
static t3_texture *tex_blob, *tex_ring, *tex_blur, *tex_glint, *tex_decal;

/* a crowd figure as a flat silhouette facing +z (the arena): a torso trapezoid (2 triangles), a head
 * hexagon (4) and two arm quads (4), 10 triangles in all (graphics 5.4; four boxes cost 48, and 240
 * of them were 11.5k triangles a view behind the glass). Frame 0 idle, 1 leaning in, 2 arms up */
static t3_geometry *crowd_geometry(int frame) {
  soup_t s; soup_init(&s, 10);
  float lean = frame == 1 ? 0.12f : 0;
  /* the torso: wide at the hips, narrower at the shoulders, the shoulders forward when leaning in */
  float t0[3] = { -0.22f, 0, 0 }, t1[3] = { 0.22f, 0, 0 }, t2[3] = { 0.18f, 0.95f, lean }, t3[3] = { -0.18f, 0.95f, lean };
  soup_quad(&s, t0, t1, t2, t3);
  /* the head: a hexagon of radius 0.15 at y 1.15 */
  float hc[6][3];
  for (int k = 0; k < 6; k++) { float a = k * PI / 3 + PI / 6; hc[k][0] = cosf(a) * 0.15f; hc[k][1] = 1.15f + sinf(a) * 0.16f; hc[k][2] = lean * 1.3f; }
  for (int k = 1; k < 5; k++) soup_tri(&s, hc[0], hc[k], hc[k + 1]);
  /* the arms: down at the sides, or up and out */
  for (int side = -1; side <= 1; side += 2) {
    float x0, x1, x2, x3, y0, y1;
    if (frame == 2) { x0 = 0.18f; x1 = 0.28f; x2 = 0.46f; x3 = 0.34f; y0 = 0.86f; y1 = 1.55f; }
    else { x0 = 0.20f; x1 = 0.31f; x2 = 0.35f; x3 = 0.24f; y0 = 0.3f; y1 = 0.92f; }
    float a0[3] = { side * x0, y0, lean * 0.6f }, a1[3] = { side * x1, y0, lean * 0.6f }, a2[3] = { side * x2, y1, lean }, a3[3] = { side * x3, y1, lean };
    if (side < 0) soup_quad(&s, a1, a0, a3, a2); else soup_quad(&s, a0, a1, a2, a3);
  }
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
  /* the arena has history: tyre marks along the long axis (not on the roof's membrane) */
  if (a->floor_style != FL_ROOF && a->floor_style != FL_GARAGE) {
    uint32_t tr = 0x7e3a11u ^ (uint32_t)(a->w * 13 + a->d * 7);
    int along_x = a->w >= a->d;
    for (int k = 0; k < 24; k++) {
      float cx = (rtr_rand_f(&tr) - 0.5f) * a->w * 0.8f, cz = (rtr_rand_f(&tr) - 0.5f) * a->d * 0.8f, len = 1.0f + rtr_rand_f(&tr) * 1.0f, bend = (rtr_rand_f(&tr) - 0.5f) * 0.4f;
      for (int t = 0; t < 24; t++) {
        float u = (t / 23.0f - 0.5f) * len, x = along_x ? cx + u : cx + bend * u * u, z = along_x ? cz + bend * u * u : cz + u;
        for (int sd = -1; sd <= 1; sd += 2) {
          float ox = along_x ? 0 : sd * 0.18f, oz = along_x ? sd * 0.18f : 0;
          cv_rect(&f.c, fpx(&f, x + ox) - 1, fpz(&f, z + oz) - 1, 3, 3, 0x1d1915, 40);
        }
      }
    }
  }
  /* soft shadow pools under the benches, shelves, HVAC units and servers */
  for (int i = 0; i < a->nprops; i++) {
    const prop_def_t *pr = &a->props[i];
    if (pr->kind != PR_BENCH && pr->kind != PR_SHELF && pr->kind != PR_HVAC && pr->kind != PR_SERVER) continue;
    float hw2 = pr->w / 2 + 0.4f, hd2 = pr->d / 2 + 0.4f, ca = cosf(pr->ang), sa = sinf(pr->ang), ext = fabsf(ca) * hw2 + fabsf(sa) * hd2, extz = fabsf(sa) * hw2 + fabsf(ca) * hd2;
    for (int y = fpz(&f, pr->z - extz); y < fpz(&f, pr->z + extz); y++) for (int x = fpx(&f, pr->x - ext); x < fpx(&f, pr->x + ext); x++) {
      if (x < 0 || y < 0 || x >= w || y >= h) continue;
      float wx = x / f.ppx - hw - pr->x, wz = y / f.ppz - hd - pr->z, lx = wx * ca - wz * sa, lz = wx * sa + wz * ca;
      float dx = fabsf(lx) - pr->w / 2, dz = fabsf(lz) - pr->d / 2, d = sqrtf((dx > 0 ? dx * dx : 0) + (dz > 0 ? dz * dz : 0));
      if (d > 0.4f) continue;
      uint8_t *pp = f.c.px + ((size_t)y * w + x) * 4; int k = (int)((1 - d / 0.4f) * 70); for (int c = 0; c < 3; c++) pp[c] = (uint8_t)(pp[c] * (255 - k) / 255);
    }
  }
  /* THE DOCK's wet floor catches the strip lights: bone streaks along the closed walls */
  if (a->floor_style == FL_WET) {
    for (int sd = 0; sd < 4; sd++) {
      int open = sd == 0 ? a->open_mask & OPEN_PX : sd == 1 ? a->open_mask & OPEN_NX : sd == 2 ? a->open_mask & OPEN_PZ : a->open_mask & OPEN_NZ;
      if (open) continue;
      float len = sd < 2 ? a->d : a->w;
      for (float p = -len / 2 + 3; p < len / 2 - 1; p += 6) {
        float x = sd == 0 ? hw - 1.2f : sd == 1 ? -hw + 1.2f : p, z = sd == 2 ? hd - 1.2f : sd == 3 ? -hd + 1.2f : p;
        float ex = sd < 2 ? 0.06f : 1.0f, ez = sd < 2 ? 1.0f : 0.06f;
        fp_rect(&f, x - ex, z - ez, x + ex, z + ez, 0xd8d2c6, 70);
      }
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
  t3_mesh *floor_tell;          /* the tell's floor component the chase camera can see (graphics 5.3), shown in the tell only */
  t3_material *tell2_m;         /* a second emissive tell: the laser beacons */
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
  struct { float x, y, z; t3_quat q; float sx, sy, ph; } crowd_back[120]; int ncrowd_back;   /* the back row bobs (5.4) */
  short crowd_back_idx[120];
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
  /* the canvas has the back wall on row 0; the plane wants it at v = 1, so the rows are
   * swapped in place here once instead of flip_y, which copies the whole texture on every
   * upload (a 4 MB heap step at the first countdown of every arena and on every scorch repaint) */
  for (int y = 0; y < H / 2; y++) {
    uint8_t row[4096]; uint8_t *lo = px + (size_t)y * W * 4, *hi = px + (size_t)(H - 1 - y) * W * 4;
    memcpy(row, lo, (size_t)W * 4); memcpy(lo, hi, (size_t)W * 4); memcpy(hi, row, (size_t)W * 4);
  }
  t3_texture *t = t3_data_texture_new(W, H, px);
  free(px);
  t->color_space = T3_SRGB_COLOR_SPACE; t->flip_y = false;
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

static t3_texture *tex_refl;
static void arena_walls(warena_t *w, const arena_def_t *a) {
  float hw = a->w / 2, hd = a->d / 2, H = 1.1f;
  /* the walls in as few draws as they can be: every lexan sheet one soup, every frame part (posts, skirting,
   * the lit top edges, the parapets, the shop walls and the skyline) one vertex coloured soup */
  soup_t refl, trim, lex; soup_init(&refl, 8); soup_init_vcol(&trim, 900); soup_init(&lex, 8);
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
      soup_rgb(&trim, 0x5a4a40u); sp_box_at(&trim, az ? 0.5f : len + 0.5f, 0.9f, az ? len + 0.5f : 0.5f, x, 0.45f, z);
      soup_rgb(&trim, 0x8a857cu); sp_box_at(&trim, az ? 0.6f : len + 0.6f, 0.08f, az ? len + 0.6f : 0.6f, x, 0.94f, z);
      continue;
    }
    /* lexan panel */
    float yaw = az ? (side[s].nx > 0 ? PI / 2 : -PI / 2) : (side[s].nz > 0 ? 0 : PI);
    soup_push_at(&lex, x, H / 2, z, 0, yaw, 0); sp_quad(&lex, len, H); soup_pop(&lex);
    /* the reflection: a plane 0.02 inside the sheet with the strip lights and the crowd smeared in it, and
     * the sheet's lit top edge (graphics 5.2); one soup each for the whole arena */
    { float ix = x + side[s].nx * 0.02f, iz = z + side[s].nz * 0.02f;
      soup_push_at(&refl, ix, H / 2 + 0.02f, iz, 0, yaw, 0);
      float a0[3] = { -len / 2, -H / 2, 0 }, a1[3] = { len / 2, -H / 2, 0 }, a2[3] = { len / 2, H / 2, 0 }, a3[3] = { -len / 2, H / 2, 0 };
      float u0[2] = { 0, 0 }, u1[2] = { len / 4, 0 }, u2[2] = { len / 4, 1 }, u3[2] = { 0, 1 };
      soup_emit(&refl, a0, a1, a2, 0, 0, 0, u0, u1, u2); soup_emit(&refl, a0, a2, a3, 0, 0, 0, u0, u2, u3);
      soup_pop(&refl);
      soup_rgb(&trim, 0xd8d2c6u); sp_box_at(&trim, az ? 0.02f : len, 0.02f, az ? len : 0.02f, x, H - 0.01f, z);
      soup_rgb(&trim, 0x1d1915u); sp_cbox_at(&trim, az ? 0.2f : len + 0.2f, 0.14f, az ? len + 0.2f : 0.2f, 0.03f, x, 0.07f, z); }
    tape_rail(w->root, w->rail_m, len + 0.16f, x, H + 0.06f, z, az);
    soup_rgb(&trim, 0x5a5246u);
    for (float p = -len / 2; p <= len / 2 + 0.01f; p += 4) {
      float px = az ? x : p, pz = az ? p : z;
      sp_box_at(&trim, 0.12f, H + 0.12f, 0.12f, px, (H + 0.12f) / 2, pz);
    }
  }
  if (lex.n_) { t3_material *lm = lexan_mat(); lm->side = T3_DOUBLE_SIDE; soup_mesh(w->root, &lex, lm); } else soup_free_empty(&lex);
  if (!tex_refl) {
    /* four soft bone streaks (the strip lights), a faint row of char blocks (the crowd), two smudges */
    canvas_t c = cv_new(256, 32);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 256; x++) { uint8_t *pp = c.px + ((size_t)y * 256 + x) * 4; pp[0] = 0xec; pp[1] = 0xe3; pp[2] = 0xd3; pp[3] = 0; }
    for (int k = 0; k < 4; k++) for (int y = 2; y < 9; y++) for (int x = k * 64 + 6; x < k * 64 + 50; x++) { uint8_t *pp = c.px + ((size_t)y * 256 + x) * 4; int e = (x - k * 64 - 6) < 4 || (k * 64 + 50 - x) < 4 || y < 3 || y > 7 ? 90 : 200; pp[3] = (uint8_t)e; }
    for (int k = 0; k < 32; k++) { int x = k * 8 + (k * 5) % 3, hh = 6 + (k * 7) % 5; for (int y = 32 - hh; y < 32; y++) for (int i = 0; i < 6; i++) { uint8_t *pp = c.px + ((size_t)y * 256 + x + i) * 4; pp[0] = 0x1d; pp[1] = 0x19; pp[2] = 0x15; pp[3] = 120; } }
    for (int k = 0; k < 2; k++) { int cx = 70 + k * 110, cy = 18; for (int y = -6; y <= 6; y++) for (int x = -14; x <= 14; x++) { float d = sqrtf(x * x / 196.0f + y * y / 36.0f); if (d < 1) { uint8_t *pp = c.px + ((size_t)(cy + y) * 256 + cx + x) * 4; int al = (int)(60 * (1 - d)); if (al > pp[3]) { pp[0] = 0xec; pp[1] = 0xe3; pp[2] = 0xd3; pp[3] = (uint8_t)al; } } } }
    tex_refl = cv_texture(&c, 1, 1);
  }
  if (refl.n_) { t3_material *rm = glass_map(tex_refl, 0.14f); rm->side = T3_DOUBLE_SIDE; soup_mesh(w->root, &refl, rm); t3_release(rm); } else soup_free_empty(&refl);
  /* the surround: an outer concrete floor and dark shop walls with strip lights, or a skyline on the roof */
  t3_mesh *outer = part(w->root, g_plane, M_RUBBER(0x24201b), a->w * 3, a->d * 3, 1, 0, -0.02f, 0);
  rot(outer, -PI / 2, 0, 0);
  if (roof) {
    for (int k = 0; k < 14; k++) {
      float ang = k * 0.45f, r = 22 + (k % 3) * 6, h = 4 + (k * 7) % 9;
      soup_rgb(&trim, 0x1d1915u); sp_box_at(&trim, 4 + (k % 4), h, 4 + (k % 3), cosf(ang) * r, h / 2 - 1.5f, sinf(ang) * r);
      part(w->root, g_box, emis(C_HAZARD, 1.2f), 4 + (k % 4) + 0.04f, 0.06f, 4 + (k % 3) + 0.04f, cosf(ang) * r, h - 1.5f + 0.03f, sinf(ang) * r);   /* the amber rim light */
      for (int wy = 0; wy < 3; wy++) part(w->root, g_box, emis(C_HAZARD, 1.5f), 0.3f, 0.3f, 0.3f, cosf(ang) * r + (k % 3) - 1, wy * 1.6f + 0.5f, sinf(ang) * r - 2 - (k % 3) * 0.5f);
    }
  } else {
    for (int s = 0; s < 4; s++) {
      if (side[s].open) continue;
      float dist = 4.2f;
      float x = side[s].x + side[s].nx * -dist, z = side[s].z + side[s].nz * -dist, len = side[s].len + 2 * dist; int az = side[s].along_z;
      soup_rgb(&trim, 0x1d1915u); sp_box_at(&trim, az ? 0.4f : len, 5, az ? len : 0.4f, x, 2.5f, z);
      for (float p = -len / 2 + 3; p < len / 2 - 1; p += 6)
        part(w->root, g_box, emis(a->floor_style == FL_DARK ? C_LIVE : 0xfff1d6, 1.8f), az ? 0.1f : 2.4f, 0.08f, az ? 2.4f : 0.1f, az ? x + side[s].nx * 0.25f : p, 3.6f, az ? p : z + side[s].nz * 0.25f);
    }
  }
  if (trim.n_) soup_mesh(w->root, &trim, VM_BODY); else soup_free_empty(&trim);
}


/* a prop's static body as one vertex coloured soup, cached by its shape: every bench of a size is one
 * geometry, so a room of benches is one draw; what moves or glows (the HVAC flag, the server LEDs) and the
 * sign's printed board stay their own meshes */
enum { GK_PROP = 40 };
static t3_geometry *prop_geo(const prop_def_t *p, int idx) {
  float h = p->h > 0 ? p->h : 0.4f;
  int variant = p->kind == PR_SHELF ? idx % 5 : 0;
  float key[8] = { (float)p->kind, p->w, p->d, h, (float)(p->colour & 0xffff), (float)(p->colour >> 16), (float)variant };
  t3_geometry *g = geo_find(GK_PROP, key);
  if (g) return g;
  soup_t s; soup_init_vcol(&s, 400);
  switch (p->kind) {
    case PR_BENCH:
      soup_rgb(&s, 0xa57c4fu); sp_box_at(&s, p->w, 0.08f, p->d, 0, h - 0.04f, 0);
      soup_rgb(&s, C_STEEL); for (int k = -1; k <= 1; k += 2) sp_box_at(&s, 0.08f, h - 0.08f, p->d * 0.8f, k * (p->w / 2 - 0.1f), (h - 0.08f) / 2, 0);
      soup_rgb(&s, C_STEEL_HI); sp_box_at(&s, 0.22f, 0.16f, 0.14f, p->w * 0.25f, h + 0.08f, 0);   /* a vice */
      soup_rgb(&s, C_BONE); sp_cyl_at(&s, 0.06f, 0.1f, 8, 1, -p->w * 0.3f, h + 0.05f, 0.1f);       /* and a mug */
      break;
    case PR_HVAC:
      soup_rgb(&s, 0x8a857cu); sp_box_at(&s, p->w, h, p->d, 0, h / 2, 0);
      soup_rgb(&s, C_TAPE); sp_cyl_at(&s, p->w * 0.35f, 0.04f, 12, 1, 0, h + 0.02f, 0);
      soup_rgb(&s, C_STEEL); for (int k = 0; k < 4; k++) sp_box_at(&s, p->w * 0.6f, 0.03f, 0.03f, 0, h * 0.3f + k * 0.12f, p->d / 2 + 0.01f);
      soup_rgb(&s, C_STEEL_HI); sp_box_at(&s, 0.05f, 1.6f, 0.05f, p->w / 2 - 0.2f, h + 0.8f, p->d / 2 - 0.2f);
      break;
    case PR_BOX: {
      uint32_t c = p->colour ? p->colour : 0xa0773f;
      soup_rgb(&s, c); sp_box_at(&s, p->w, h, p->d, 0, h / 2, 0);
      soup_rgb(&s, 0xc9a271u); sp_box_at(&s, p->w * 0.3f, h + 0.01f, p->d + 0.01f, 0, h / 2, 0);
      break; }
    case PR_SHELF:
      soup_rgb(&s, C_STEEL); for (int k = 0; k < 4; k++) sp_box_at(&s, 0.06f, h, 0.06f, (k & 1 ? 1 : -1) * (p->w / 2 - 0.03f), h / 2, (k & 2 ? 1 : -1) * (p->d / 2 - 0.03f));
      for (int sh = 0; sh < 3; sh++) {
        float y = 0.3f + sh * (h - 0.4f) / 2;
        soup_rgb(&s, 0xc8a165u); sp_box_at(&s, p->w, 0.04f, p->d, 0, y, 0);
        for (int k = 0; k < 3; k++) {
          static const uint32_t cs[5] = { 0xa0773f, C_RUST, C_INFO, 0x2f8562, C_BONE };
          float bw = p->w / 3 - 0.1f, bh = 0.25f + 0.1f * ((sh + k) % 2);
          soup_rgb(&s, cs[(sh + k + variant) % 5]); sp_box_at(&s, bw, bh, p->d * 0.7f, -p->w / 2 + 0.05f + bw / 2 + k * p->w / 3, y + 0.02f + bh / 2, 0);
        }
      }
      break;
    case PR_SIGN:
      soup_rgb(&s, C_STEEL); for (int k = -1; k <= 1; k += 2) sp_box_at(&s, 0.08f, h, 0.08f, k * (p->w / 2 - 0.1f), h / 2, 0);
      break;
    case PR_SERVER:
      soup_rgb(&s, 0x1d1915u); sp_box_at(&s, p->w, h, p->d, 0, h / 2, 0);
      soup_rgb(&s, C_STEEL); for (int k = 0; k < 6; k++) sp_box_at(&s, p->w * 0.8f, 0.06f, 0.02f, 0, 0.2f + k * (h - 0.4f) / 5, p->d / 2 + 0.01f);
      break;
    default:
      soup_rgb(&s, p->colour ? p->colour : C_STEEL); sp_box_at(&s, p->w, h, p->d, 0, h / 2, 0);
      break;
  }
  return geo_keep(GK_PROP, key, soup_finish(&s));
}

static t3_object *build_prop(warena_t *w, const prop_def_t *p, int idx) {
  t3_object *g = t3_group_new();
  t3_object_set_position(g, p->x, 0, p->z);
  t3_object_set_rotation(g, 0, p->ang, 0);
  float h = p->h > 0 ? p->h : 0.4f;
  if (p->kind == PR_GATE) return g;
  geo_mesh(g, prop_geo(p, idx), VM_BODY, 0, 0, 0);
  switch (p->kind) {
    case PR_HVAC: {
      t3_mesh *flag = part(g, g_plane, M_PAINT(0xd9672b), 0.7f, 0.35f, 1, p->w / 2 - 0.2f + 0.35f, h + 1.4f, p->d / 2 - 0.2f);
      flag->materials[0]->side = T3_DOUBLE_SIDE;
      w->flags[idx] = flag;
      break; }
    case PR_SIGN: {
      static t3_material *sm;
      if (!sm) { sm = t3_mesh_basic_material_new(0xffffff); t3_material_set_map(sm, tex_sign); sm->side = T3_DOUBLE_SIDE; }
      t3_mesh *b = t3_mesh_new(g_plane, sm); t3_object_set_scale(b, p->w, p->w * 0.5f, 1); t3_object_set_position(b, 0, h - p->w * 0.25f, 0);
      t3_object_add(g, b); t3_release(b);
      break; }
    case PR_SERVER:
      for (int k = 0; k < 6; k++) part(g, g_box, emis(k % 2 ? C_LIVE : C_HAZARD, 2.0f), 0.04f, 0.04f, 0.02f, p->w * 0.3f, 0.2f + k * (h - 0.4f) / 5 + 0.08f, p->d / 2 + 0.02f);
      break;
    default: break;
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
      { soup_t fr; soup_init(&fr, 8); float bw = h->w + 0.7f, bd = h->d + 0.7f, t = 0.3f;
        soup_push_at(&fr, 0, 0.011f, 0, -PI / 2, 0, 0);
        soup_push_at(&fr, 0, bd / 2 - t / 2, 0, 0, 0, 0); sp_quad(&fr, bw, t); soup_pop(&fr);
        soup_push_at(&fr, 0, -bd / 2 + t / 2, 0, 0, 0, 0); sp_quad(&fr, bw, t); soup_pop(&fr);
        soup_push_at(&fr, bw / 2 - t / 2, 0, 0, 0, 0, 0); sp_quad(&fr, t, bd - 2 * t); soup_pop(&fr);
        soup_push_at(&fr, -bw / 2 + t / 2, 0, 0, 0, 0, 0); sp_quad(&fr, t, bd - 2 * t); soup_pop(&fr);
        soup_pop(&fr);
        hz->floor_tell = soup_mesh(g, &fr, glass(C_RUST, 0)); t3_release(hz->floor_tell->materials[0]); set_vis(hz->floor_tell, 0); }
      break; }
    case HZ_PULVERIZER: {
      inward(a, h->x, h->z, &hz->dir_x, &hz->dir_z);
      float yaw = atan2f(hz->dir_x, hz->dir_z);
      t3_object_set_rotation(g, 0, yaw, 0);
      /* the post stands at the wall, 1.4 behind the strike square; the arm pivots at its top: the post and the
       * arm one cached soup each (every pulverizer shares them) */
      float kp[8] = { 1 }, ka[8] = { 2 };
      t3_geometry *pg = geo_find(GK_PROP + 1, kp), *ag = geo_find(GK_PROP + 1, ka);
      if (!pg) { soup_t ps; soup_init_vcol(&ps, 40); soup_rgb(&ps, C_STEEL_HI); sp_box_at(&ps, 0.3f, 2.2f, 0.3f, 0, 1.1f, 0); soup_rgb(&ps, C_TAPE); sp_box_at(&ps, 0.5f, 0.2f, 0.5f, 0, 2.25f, 0); pg = geo_keep(GK_PROP + 1, kp, soup_finish(&ps)); }
      if (!ag) { soup_t as; soup_init_vcol(&as, 80); soup_rgb(&as, 0x6f6c66u); sp_box_at(&as, 0.18f, 0.18f, 1.7f, 0, 0, 0.85f); soup_rgb(&as, C_RUST); sp_cbox_at(&as, 0.7f, 0.55f, 0.7f, 0.05f, 0, -0.1f, 1.7f); soup_rgb(&as, C_HAZARD); sp_box_at(&as, 0.72f, 0.1f, 0.72f, 0, -0.1f, 1.7f); ag = geo_keep(GK_PROP + 1, ka, soup_finish(&as)); }
      geo_mesh(g, pg, VM_BODY, 0, 0, -1.5f);
      hz->tell_m = mat_ex(C_LIVE, 0.5f, 0, C_LIVE, 0.0f, 0);
      part(g, g_box, hz->tell_m, 0.2f, 0.2f, 0.2f, 0, 2.45f, -1.5f);
      hz->mov = group_at(g, 0, 2.0f, -1.5f);
      geo_mesh(hz->mov, ag, VM_BODY, 0, 0, 0);
      hz->swing = -1.1f;
      t3_object_set_rotation(hz->mov, hz->swing, 0, 0);
      hz->floor_tell = mesh_take(g_circle, glass(0x000000, 0)); rot(hz->floor_tell, -PI / 2, 0, 0); t3_object_set_position(hz->floor_tell, 0, 0.013f, 0.2f);
      t3_object_add(g, hz->floor_tell); t3_release(hz->floor_tell); set_vis(hz->floor_tell, 0);
      break; }
    case HZ_KILLSAW: {
      t3_object_set_rotation(g, 0, h->ang, 0);
      float L = h->w > h->d ? h->w : h->d, Wd = h->w > h->d ? h->d : h->w;
      BOX(g, M_STEEL(C_STEEL), L + 0.1f, 0.04f, 0.06f, 0, 0.02f, Wd / 2);
      BOX(g, M_STEEL(C_STEEL), L + 0.1f, 0.04f, 0.06f, 0, 0.02f, -Wd / 2);
      hz->tell_m = mat_ex(0x2a241e, 0.6f, 0, C_HAZARD, 0.0f, 0);
      part(g, g_box, hz->tell_m, L, 0.02f, Wd - 0.08f, 0, 0.005f, 0);
      hz->mov = group_at(g, 0, -0.42f, 0);
      /* each saw one cached soup: a disc and eight wedge teeth */
      float key[8] = { 0.36f, 0.05f, 8 };
      t3_geometry *sg = geo_find(GK_DISC, key);
      if (!sg) {
        soup_t ss; soup_init_vcol(&ss, 120);
        soup_rgb(&ss, 0xcfd3d6u); sp_cyl(&ss, 0.36f, 0.05f, 12, 0);
        soup_rgb(&ss, 0xe6e2d8u); soup_push_at(&ss, 0, 0, 0, 0, 0, -PI / 2); sp_teeth(&ss, 8, 0.34f, 0.1f, 0.04f, 0.1f, 0); soup_pop(&ss);
        sg = geo_keep(GK_DISC, key, soup_finish(&ss));
      }
      for (int k = 0; k < 2; k++) { t3_object *saw = group_at(hz->mov, (k ? 1 : -1) * L / 4, 0, 0); geo_mesh(saw, sg, VM_METAL, 0, 0, 0); }
      /* the tell: a hazard amber quad over the slot */
      hz->floor_tell = mesh_take(g_plane, glass(C_HAZARD, 0)); t3_object_set_scale(hz->floor_tell, L + 0.2f, Wd + 0.2f, 1);
      rot(hz->floor_tell, -PI / 2, 0, 0); t3_object_set_position(hz->floor_tell, 0, 0.03f, 0);
      t3_object_add(g, hz->floor_tell); t3_release(hz->floor_tell); set_vis(hz->floor_tell, 0);
      break; }
    case HZ_FLAME: {
      BOX(g, M_STEEL(C_STEEL), h->w, 0.08f, h->d, 0, 0.04f, 0);
      hz->tell_m = mat_ex(0x1d1915, 0.6f, 0, C_HAZARD, 0.0f, 0);
      for (int k = 0; k < 4; k++) part(g, g_box, hz->tell_m, h->w * 0.8f, 0.02f, h->d * 0.12f, 0, 0.085f, -h->d * 0.3f + k * h->d * 0.2f);
      for (int k = 0; k < 4; k++) part(g, g_sphere, hz->tell_m, 0.035f, 0.035f, 0.035f, (k & 1 ? 1 : -1) * h->w * 0.42f, 0.11f, (k & 2 ? 1 : -1) * h->d * 0.42f);   /* the pilots */
      hz->glow_m = glass(0xffb347, 0.0f);
      hz->mov = group_at(g, 0, 0.1f, 0);
      t3_mesh *cone = t3_mesh_new(g_cone, hz->glow_m);
      t3_object_set_scale(cone, h->w * 0.45f, 1.4f, h->d * 0.45f); t3_object_set_position(cone, 0, 0.7f, 0); t3_object_set_rotation(cone, PI, 0, 0);
      t3_object_add(hz->mov, cone); t3_release(cone);
      t3_material *cm = glass(0xfff1d6, 0.0f);
      t3_mesh *core = t3_mesh_new(g_cone, cm); t3_release(cm);
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
      /* p0 is the post height in hundredths of a unit, the unit the sim collides with (sim.c reads p0 / 100) */
      float H = h->p0 > 0 ? h->p0 / 100.0f : 1.0f;
      CYL(g, M_PAINT(0x3a342b), 0.4f, H, 0, H / 2, 0);
      CYL(g, M_PAINT(C_HAZARD), 0.42f, 0.16f, 0, H * 0.55f, 0);
      part(g, g_cyl12, mat_tape, 0.41f, 0.14f, 0.41f, 0, H * 0.3f, 0);   /* a tape band on the post */
      part(g, g_disc, M_PAINT(C_TAPE), 0.4f, 0.03f, 0.4f, 0, H + 0.015f, 0);
      hz->tell_m = mat_ex(C_HAZARD, 0.4f, 0, C_HAZARD, 0.2f, 0);
      part(g, g_sphere, hz->tell_m, 0.14f, 0.14f, 0.14f, 0, H + 0.12f, 0);
      break; }
    case HZ_DECK: {
      /* the deck reads from the floor: a painted steel top, a lit front wall with a tape lip
       * along its edge, and two strip lights under the lip so the climb is visible at night */
      t3_material *top = mat(0x8a857c, 0.5f, 0.2f), *face = mat(0x6f6c66, 0.6f, 0.2f);
      BOX(g, top, h->w, 0.12f, h->d, 0, h->h - 0.06f, 0);
      BOX(g, face, h->w, h->h - 0.12f, 0.06f, 0, (h->h - 0.12f) / 2, h->d / 2 - 0.03f);
      BOX(g, M_PAINT(C_HAZARD), h->w, 0.04f, 0.14f, 0, h->h + 0.02f, h->d / 2 - 0.07f);
      BOX(g, M_PAINT(C_TAPE), h->w, 0.1f, 0.02f, 0, h->h - 0.17f, h->d / 2 + 0.005f);
      for (float px = -h->w / 2 + 1; px < h->w / 2; px += 2) BOX(g, M_STEEL(C_STEEL), 0.14f, h->h - 0.12f, 0.14f, px, (h->h - 0.12f) / 2, h->d / 2 + 0.02f);
      for (int k = -1; k <= 1; k += 2) part(g, g_box, emis(0xfff1d6, 1.8f), 2.4f, 0.05f, 0.05f, k * h->w * 0.25f, h->h - 0.3f, h->d / 2 + 0.06f);
      BOX(g, face, h->w, h->h - 0.12f, 0.04f, 0, (h->h - 0.12f) / 2, -h->d / 2 + 0.02f);
      break; }
    case HZ_MOUND: {
      /* the height field of arena_mound_h as a lathe dome under the cardboard: a tarp over the pile */
      t3_vec2 prof[10];
      for (int k = 0; k < 10; k++) { float rr = h->w * 0.5f * (1 - k / 9.0f); prof[k] = (t3_vec2){ rr, arena_mound_h(rr) - 0.02f }; }
      prof[9].x = 0;
      t3_geometry *dg = t3_lathe_geometry_new(prof, 10, 28, 0, TAU);
      t3_mesh *dm = t3_mesh_new(dg, M_PLY(0x8f6b45));
      t3_object_add(g, dm); t3_release(dm); t3_release(dg);
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
      { t3_material *rm = glass_map(tex_ring, 0); rm->color = t3_color_hex(C_HAZARD);
        hz->floor_tell = mesh_take(quad_geo(0, 0, 1, 1), rm); t3_object_set_scale(hz->floor_tell, h->w * 1.3f, h->d * 1.3f, 1); rot(hz->floor_tell, -PI / 2, 0, 0); t3_object_set_position(hz->floor_tell, 0, 0.014f, 0);
        t3_object_add(g, hz->floor_tell); t3_release(hz->floor_tell); set_vis(hz->floor_tell, 0); }
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
      hz->tell2_m = mat_ex(C_HAZARD, 0.4f, 0, C_HAZARD, 0.1f, 0);
      for (int s = -1; s <= 1; s += 2) {
        BOX(g, M_PAINT(C_TAPE), 0.4f, 0.5f, 0.5f, 0, 0.3f, s * (a->d / 2 - 0.3f));
        part(g, g_sphere, hz->tell2_m, 0.09f, 0.09f, 0.09f, 0, 0.62f, s * (a->d / 2 - 0.3f));   /* the amber beacon */
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
      if (row && w->ncrowd_back < 120) { int b = w->ncrowd_back++; w->crowd_back[b].x = x; w->crowd_back[b].y = y; w->crowd_back[b].z = z; w->crowd_back[b].q = q; w->crowd_back[b].sx = sc.x; w->crowd_back[b].sy = sc.y; w->crowd_back[b].ph = rtr_rand_f(&r) * TAU; w->crowd_back_idx[b] = (short)k; }
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
    /* the mound boxes are terrain skin: each sits on the height field, tilted down the slope */
    for (int k = 0; k < a->nhaz; k++) {
      const hazard_def_t *hz = &a->haz[k];
      if (hz->type != HZ_MOUND || a->props[i].kind != PR_BOX) continue;
      float dx = a->props[i].x - hz->x, dz = a->props[i].z - hz->z, rr = sqrtf(dx * dx + dz * dz);
      if (rr >= hz->w * 0.5f) continue;
      float y = arena_mound_h(rr) - 0.08f, slope = rr > 0.1f ? (arena_mound_h(rr - 0.1f) - arena_mound_h(rr + 0.1f)) / 0.2f : 0;
      float tilt = atanf(slope) * 0.6f;
      t3_object_set_position(g, a->props[i].x, y, a->props[i].z);
      /* tilt about the axis tangent to the slope: lean the box outward */
      if (rr > 0.1f) { float ax = -dz / rr, az = dx / rr; t3_object_set_quaternion(g, t3_quat_multiply(t3_quat_from_axis_angle((t3_vec3){ ax, 0, az }, tilt), t3_quat_from_axis_angle((t3_vec3){ 0, 1, 0 }, a->props[i].ang))); }
    }
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
/* Paint on the plates, raw frame under them (graphics 2): the chassis is char steel, the scheme lives on
 * six bevelled, bolted plates, so a plate that leaves shows raw steel, bolt holes and the wiring. Every
 * part is a vertex coloured soup, one draw per part: the static body (chassis, trim, band, mounts), the
 * plates (rewritten in place when a panel changes state: bolts, holes and internals ride in the same
 * soup), the drive's static parts, and each moving part of a weapon. */
#define BC_CHAR     0x3a342bu    /* the raw frame */
#define BC_CHAR_DK  0x262019u
#define BC_HOLE     0x0e0c0au
#define BC_BRIGHT   0xcfccc4u    /* bright steel: latches, axles, lips, faces */
#define BC_STEEL    0x8a857cu
#define BC_STEEL_DK 0x5a554cu
#define BC_RUBBER   0x1d1915u
#define BC_LUG      0x3a342bu
#define BC_PAINT_CHAR 0x2a2520u

/* the seven armour looks (graphics 3.5): tint and how much of it, the plate material, dent depth factor,
 * thickness, bevel, bolts on a side plate and on an end plate */
typedef struct { uint32_t tint; float mixk, rough, metal, depth, t, c; uint8_t bolts_side, bolts_end; } armour_look_t;
static const armour_look_t ARMOUR_LOOK[AR_COUNT] = {
  /* NONE  */ { 0x000000, 0.00f, 0.60f, 0.20f, 1.0f, 0.012f, 0.006f, 0, 0 },
  /* PLY   */ { 0xb08a55, 0.45f, 0.90f, 0.00f, 1.0f, 0.030f, 0.010f, 4, 4 },
  /* ALU   */ { 0xe2e4e3, 0.40f, 0.30f, 0.45f, 1.4f, 0.030f, 0.012f, 6, 4 },
  /* UHMW  */ { 0xe9e5dc, 0.40f, 0.55f, 0.05f, 0.5f, 0.030f, 0.026f, 0, 0 },
  /* STEEL */ { 0x6f6c66, 0.35f, 0.40f, 0.50f, 1.0f, 0.030f, 0.012f, 6, 4 },
  /* TI    */ { 0x8d97a3, 0.35f, 0.35f, 0.45f, 0.8f, 0.030f, 0.012f, 6, 4 },
  /* WEAR  */ { 0x8a4a2c, 0.40f, 0.50f, 0.40f, 0.3f, 0.050f, 0.014f, 12, 6 },
};

typedef struct { float w, h, t; xf_t xf; float x, y, z, rx, ry, rz; int on, hinge_top; } wplate_t;
typedef struct { uint8_t panel, kind; float u, v, size, rot; } wdecal_t;
#define NDECALS 8

typedef struct {
  t3_object *root, *body, *drive_g, *weapon_g, *weapon2_g;
  t3_object *wpivot, *w2pivot;                 /* the moving part of each weapon */
  soup_t base;                                 /* the static body soup (chassis, trim, drive, mounts): the plate soup's first part */
  t3_mesh *plates; int pl_cap;                 /* the plate soup, rewritten in place on a state change */
  wplate_t pl[PN_COUNT];
  uint8_t pstate[PN_COUNT]; int plates_dirty;
  t3_object *hang[PN_COUNT]; t3_mesh *hang_m[PN_COUNT]; float hang_kick[PN_COUNT];
  t3_mesh *decals; wdecal_t dec[NDECALS]; int ndec, dec_next, dec_dirty; float burn_acc;
  t3_mesh *digits[3], *tape;
  t3_mesh *wheels[8]; float wheel_x[8], wheel_y[8], wheel_z[8]; int nwheels;
  t3_object *legs[6], *knees[6]; int nlegs;
  t3_mesh *belts[4]; int nbelts; t3_mesh *sprockets[4]; int nsprockets; t3_texture *tread_tex;
  t3_mesh *skirt, *fans[2];
  t3_mesh *ring, *blur, *glint, *led, *glow, *beacon;          /* glow: the flame cone */
  t3_mesh *seat_ring;                          /* the 3 px seat colour ring under the bot in ONE CAMERA (10.17) */
  t3_mesh *wsub[4];                            /* moving weapon parts: rod, disc, head, piston, ram, collar, pilot */
  t3_mesh *wsub2[4];
  build_t build; uint32_t seat_colour; int built, is_ops, visible, posed, seat;
  float x, y, z, h; int inverted;              /* this frame's interpolated root */
  float melty_yaw, gait, scroll, wheel_spin, spin_vis, saw_vis, led_t, radius, top_y, ride_y, len, wid;
  float fire_t; int fire_on;                   /* garage test fire */
  float ring_r, ring_y, ring_z;                /* the spinner's radius and centre (the danger ring, the blur) */
  float hammer_max, hammer_last;               /* the slam angle that keeps the head off the floor; the last strike pose */
  int16_t prev_cool[2]; int glint_t; float glint_x, glint_y, glint_z; t3_object *glint_parent;
  int panels_hidden;                           /* a bare FRAME shows its tubes, never its panels */
  float rock, prev_drive_t, lean_x, lean_z;
  int wedge_dip;
  float deck_y, deck_w, deck_l;                /* the surface the lid marks sit on (a bare FRAME's is its electronics box) */
  float hinge_y, hinge_z;                      /* the WEDGE plate's hinge, for a 2ND mounted on it */
  float flip_rest;                             /* a flipper on a slope (WEDGE, SHELL) rests along it */
} wbot_t;
static wbot_t bots[MAX_BOTS];

static uint32_t paint_colour(int pc, uint32_t seat) {
  switch (pc) { case PC_SEAT: return seat; case PC_CHAR: return BC_PAINT_CHAR; case PC_BONE: return C_BONE; case PC_STEEL: return 0x6b655a; default: return C_HAZARD; }
}
static uint32_t mix_rgb(uint32_t a, uint32_t b, float t) {
  int r = (int)(((a >> 16) & 255) + ((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t);
  int g = (int)(((a >> 8) & 255) + ((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t);
  int bl = (int)((a & 255) + ((int)(b & 255) - (int)(a & 255)) * t);
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

/* the four schemes, every one two tone and visible (graphics 3.6): side plates, lid, lid stripe and the
 * front chevron (0 for none); stripe2 is the double width amber stripe of scheme 4 */
typedef struct { uint32_t plate, lid, stripe, chevron; int stripe2; } paint_t;
static paint_t bot_paint(const wbot_t *b) {
  paint_t p = { 0 };
  uint32_t seat = b->seat_colour;
  if (b->is_ops) { p.plate = BC_PAINT_CHAR; p.lid = BC_PAINT_CHAR; p.stripe = C_HAZARD; p.stripe2 = 1; return p; }
  switch (b->build.paint % PAINT_COUNT) {
    case 0: p.plate = seat; p.lid = seat; p.stripe = BC_PAINT_CHAR; break;
    case 1: p.plate = BC_PAINT_CHAR; p.lid = seat; p.stripe = BC_PAINT_CHAR; break;
    case 2: p.plate = C_BONE; p.lid = seat; p.stripe = C_BONE; p.chevron = seat; break;
    default: p.plate = 0x6b655a; p.lid = 0x6b655a; p.stripe = C_HAZARD; p.stripe2 = 1; p.chevron = C_HAZARD; break;
  }
  if (p.chevron == C_BONE && p.plate == C_BONE) p.chevron = BC_PAINT_CHAR;   /* a bone seat on bone plates */
  return p;
}
static int bot_armour(const wbot_t *b) { return b->is_ops ? AR_STEEL : b->build.armour % AR_COUNT; }
static uint32_t plate_colour(const wbot_t *b, uint32_t paint) {
  const armour_look_t *a = &ARMOUR_LOOK[bot_armour(b)];
  return a->mixk > 0 ? mix_rgb(paint, a->tint, a->mixk) : paint;
}

typedef struct { float w, h; float x, y, z; float rx, ry, rz; } panel_place_t;
/* where each of the six panels sits on a chassis of L x W x H standing on ride_y: the plate's back face
 * is on the body face, its front looks along the place's +z */
static panel_place_t panel_place(const chassis_def_t *c, int panel, float ride) {
  float L = c->len, W = c->wid, H = c->hgt, cy = ride + H / 2;
  panel_place_t p = { 0 };
  switch (panel) {
    case PN_FRONT: p = (panel_place_t){ W * 0.9f, H * 0.82f, 0, cy, L / 2 + 0.003f, 0, 0, 0 }; break;
    case PN_BACK: p = (panel_place_t){ W * 0.9f, H * 0.82f, 0, cy, -L / 2 - 0.003f, 0, PI, 0 }; break;
    case PN_LEFT: p = (panel_place_t){ L * 0.9f, H * 0.82f, -W / 2 - 0.003f, cy, 0, 0, -PI / 2, 0 }; break;
    case PN_RIGHT: p = (panel_place_t){ L * 0.9f, H * 0.82f, W / 2 + 0.003f, cy, 0, 0, PI / 2, 0 }; break;
    case PN_TOP: p = (panel_place_t){ W * 0.88f, L * 0.55f, 0, ride + H + 0.003f, -L * 0.2f, -PI / 2, 0, 0 }; break;
    default: p = (panel_place_t){ W * 0.6f, L * 0.28f, 0, ride + H + 0.003f, L * 0.3f, -PI / 2, 0, 0 }; break;   /* weapon mount */
  }
  if (c->shape == SH_CIRCLE) {
    /* the dome (the lathe in build_chassis) is widest at its rolled rim, 0.36 H up at radius 0.52 L, and
     * its flat top is a disc of radius 0.25 L: the four side plates are small plates on the dome's flank
     * leaning with it, the two top plates sit inside the disc, so nothing pokes out of the wok */
    float r = c->len / 2, R = r * 0.94f, by = ride + H * 0.6f;
    switch (panel) {
      case PN_FRONT: p = (panel_place_t){ r * 0.7f, H * 0.32f, 0, by, R, -0.3f, 0, 0 }; break;
      case PN_BACK: p = (panel_place_t){ r * 0.7f, H * 0.32f, 0, by, -R, -0.3f, PI, 0 }; break;
      case PN_LEFT: p = (panel_place_t){ r * 0.7f, H * 0.32f, -R, by, 0, -0.3f, -PI / 2, 0 }; break;
      case PN_RIGHT: p = (panel_place_t){ r * 0.7f, H * 0.32f, R, by, 0, -0.3f, PI / 2, 0 }; break;
      case PN_TOP: p = (panel_place_t){ r * 0.45f, r * 0.45f, 0, ride + H + 0.003f, -r * 0.1f, -PI / 2, 0, 0 }; break;
      default: p = (panel_place_t){ r * 0.35f, r * 0.26f, 0, ride + H + 0.003f, r * 0.3f, -PI / 2, 0, 0 }; break;
    }
  }
  if (panel == PN_FRONT && c->shape == SH_BOX && c->len >= 1.1f && c->hgt <= 0.3f) { /* the WEDGE front slopes */
    float run = L * 0.55f, ang = atanf(H / run), len = sqrtf(run * run + H * H);
    p = (panel_place_t){ W * 0.88f, len * 0.8f, 0, ride + H * 0.5f + 0.003f * cosf(ang), L * 0.225f + 0.003f * sinf(ang), -(PI / 2 - ang), 0, 0 };
  }
  return p;
}

static void clear_group(t3_object *g) { while (g->child_count > 0) t3_object_remove(g, g->children[g->child_count - 1]); }


/* -- plates: four dent states and gone (graphics 4.1, 4.2) ---------- */
/* a plate's local frame: x across, y up the plate, z out of it; the back face sits on z = 0 */
typedef struct { float cx, cy, ang, off; } crease_t;
static uint32_t plate_seed(int chassis, int panel) { return (uint32_t)(chassis * 16 + panel) * 2654435761u + 0x9e37u; }
static crease_t plate_crease(uint32_t seed) {
  uint32_t r = seed; crease_t c;
  c.ang = rtr_rand_f(&r) * PI; c.off = (rtr_rand_f(&r) - 0.5f) * 0.3f;
  c.cx = (rtr_rand(&r) & 1) ? 1.0f : -1.0f; c.cy = (rtr_rand(&r) & 1) ? 1.0f : -1.0f;
  return c;
}
/* how far a point of the front face is pushed in (negative z) or pulled out, and how deep the crease is
 * there (0..1, for the colour) */
static float plate_dz(float x, float y, float w, float h, int state, const crease_t *c, float f, float *k_out) {
  float m = w < h ? w : h, bx = 1 - fabsf(x) / (w * 0.5f), by = 1 - fabsf(y) / (h * 0.5f);
  float inner = 4 * (bx < 0 ? 0 : bx) * (by < 0 ? 0 : by); if (inner > 1) inner = 1;
  float dz = 0, k = 0;
  if (state >= 1) {
    float d = fabsf(x * sinf(c->ang) - y * cosf(c->ang) - c->off * m) / (0.3f * m);
    k = d < 1 ? 1 - d : 0;
    dz -= (state >= 3 ? 0.10f : 0.06f) * f * k * inner;
  }
  if (state >= 2) {
    float ex = x - c->cx * w * 0.5f, ey = y - c->cy * h * 0.5f, d = sqrtf(ex * ex + ey * ey) / (0.6f * m);
    if (d < 1) dz += 0.1f * f * (1 - d) * (1 - d);
  }
  if (k_out) *k_out = k * inner;
  return dz;
}

/* bolt positions in -1..1 plate coordinates */
static int plate_bolts(int n, float (*out)[2]) {
  static const float b12[12][2] = { { -0.86f, 0.8f }, { -0.43f, 0.8f }, { 0, 0.8f }, { 0.43f, 0.8f }, { 0.86f, 0.8f },
                                    { -0.86f, -0.8f }, { -0.43f, -0.8f }, { 0, -0.8f }, { 0.43f, -0.8f }, { 0.86f, -0.8f }, { -0.86f, 0 }, { 0.86f, 0 } };
  static const float b8[8][2] = { { -0.86f, 0.8f }, { -0.3f, 0.8f }, { 0.3f, 0.8f }, { 0.86f, 0.8f }, { -0.86f, -0.8f }, { -0.3f, -0.8f }, { 0.3f, -0.8f }, { 0.86f, -0.8f } };
  static const float b6[6][2] = { { -0.86f, 0.8f }, { 0, 0.8f }, { 0.86f, 0.8f }, { -0.86f, -0.8f }, { 0, -0.8f }, { 0.86f, -0.8f } };
  static const float b4[4][2] = { { -0.86f, 0.8f }, { 0.86f, 0.8f }, { -0.86f, -0.8f }, { 0.86f, -0.8f } };
  const float (*src)[2] = n >= 12 ? b12 : n >= 8 ? b8 : n >= 6 ? b6 : n >= 4 ? b4 : 0;
  int k = n >= 12 ? 12 : n >= 8 ? 8 : n >= 6 ? 6 : n >= 4 ? 4 : 0;
  for (int i = 0; i < k; i++) { out[i][0] = src[i][0]; out[i][1] = src[i][1]; }
  return k;
}

/* one plate in its local frame at a dent state (0..3): the bevelled front grid, its edge strips, the bolts
 * and the armour's marks. colour is the plate's paint after the armour tint; white_bolts paints the bolts
 * bright (the plate soup) or leaves them a darker shade of the plate (a hanging plate, whose material
 * carries the paint) */
static void emit_plate(soup_t *s, const wbot_t *b, int panel, float w, float h, float t, int state, uint32_t colour, int hinge_top) {
  int ar = bot_armour(b); const armour_look_t *al = &ARMOUR_LOOK[ar];
  if (ar == AR_WEAR && state > 2) state = 2;                              /* WEAR PLATE never hangs */
  crease_t cr = plate_crease(plate_seed(b->build.chassis, panel));
  float f = al->depth, c = al->c; if (c > w * 0.2f) c = w * 0.2f; if (c > h * 0.2f) c = h * 0.2f;
  int top = panel == PN_TOP || panel == PN_MOUNT, gx = panel == PN_MOUNT ? 2 : 3, gy = gx;
  /* state 2: the plate pulled 5 degrees off the body about the edge away from the pulled corner */
  if (state == 2) { float px = -cr.cx * w * 0.5f; soup_push_at(s, px, 0, 0, 0, -cr.cx * 0.087f, 0); soup_push_at(s, -px, 0, 0, 0, 0, 0); }
  t3_color pc = t3_color_hex(colour), dk = t3_color_hex(BC_CHAR_DK), bright = t3_color_hex(ar == AR_ALU ? C_BONE : (ar == AR_STEEL ? C_RUST : BC_CHAR_DK));
  float V[5][5][3], C[5][5][3];
  for (int j = 0; j <= gy; j++) for (int i = 0; i <= gx; i++) {
    float x = -w / 2 + c + (w - 2 * c) * i / gx, y = -h / 2 + c + (h - 2 * c) * j / gy, k = 0;
    float dz = plate_dz(x, y, w, h, state, &cr, f, &k);
    V[j][i][0] = x; V[j][i][1] = y; V[j][i][2] = t + dz;
    /* a dent darkens; aluminium dents go bright, steel ones rust: the colour follows the crease */
    float kk = k * (state >= 3 ? 0.8f : 0.55f), *o = C[j][i];
    t3_color tc = ar == AR_ALU || ar == AR_STEEL ? bright : dk;
    o[0] = pc.r + (tc.r - pc.r) * kk; o[1] = pc.g + (tc.g - pc.g) * kk; o[2] = pc.b + (tc.b - pc.b) * kk;
  }
  float ctr[3] = { 0, 0, -1 };
  for (int j = 0; j < gy; j++) for (int i = 0; i < gx; i++) {
    const float *a = V[j][i], *bb = V[j][i + 1], *cc = V[j + 1][i + 1], *d = V[j + 1][i];
    if (faces_out(a, bb, cc, ctr)) { soup_emit(s, a, bb, cc, C[j][i], C[j][i + 1], C[j + 1][i + 1], 0, 0, 0); soup_emit(s, a, cc, d, C[j][i], C[j + 1][i + 1], C[j + 1][i], 0, 0, 0); }
    else { soup_emit(s, a, cc, bb, C[j][i], C[j + 1][i + 1], C[j][i + 1], 0, 0, 0); soup_emit(s, a, d, cc, C[j][i], C[j + 1][i], C[j + 1][i + 1], 0, 0, 0); }
  }
  /* the edge strips, front border down to the full size back: the bevel. PLY shows its layers in bone;
   * a vertical plate's bottom strip faces the floor and is left out */
  uint32_t edge = ar == AR_PLY ? 0xd9c7a2u : mix_rgb(colour, 0xffffff, 0.12f);
  soup_rgb(s, edge);
  float ectr[3] = { 0, 0, t * 0.3f };
  for (int side = 0; side < 4; side++) {
    if (side == 0 && !top && !hinge_top) continue;   /* bottom */
    int n = side & 1 ? gy : gx;
    for (int k = 0; k < n; k++) {
      const float *p0, *p1; float q0[3], q1[3];
      if (side == 0) { p0 = V[0][k]; p1 = V[0][k + 1]; }
      else if (side == 2) { p0 = V[gy][k]; p1 = V[gy][k + 1]; }
      else if (side == 1) { p0 = V[k][gx]; p1 = V[k + 1][gx]; }
      else { p0 = V[k][0]; p1 = V[k + 1][0]; }
      float sx = side == 1 ? 1 : side == 3 ? -1 : 0, sy = side == 2 ? 1 : side == 0 ? -1 : 0;
      q0[0] = sx ? sx * w / 2 : p0[0] * w / (w - 2 * c); q0[1] = sy ? sy * h / 2 : p0[1] * h / (h - 2 * c); q0[2] = 0;
      q1[0] = sx ? sx * w / 2 : p1[0] * w / (w - 2 * c); q1[1] = sy ? sy * h / 2 : p1[1] * h / (h - 2 * c); q1[2] = 0;
      soup_quad_out(s, p0, p1, q1, q0, ectr);
    }
  }
  /* bolts: a side plate carries the armour's side count, the others its end count; a bent plate has lost
   * the bolt at its pulled corner, a hanging one two */
  int nb = (panel == PN_LEFT || panel == PN_RIGHT) ? al->bolts_side : panel == PN_MOUNT ? 0 : al->bolts_end;
  if (panel == PN_TOP && nb > 4) nb = 4;
  float bp[12][2]; nb = plate_bolts(nb, bp);
  soup_rgb(s, BC_BRIGHT);
  for (int i = 0; i < nb; i++) {
    float x = bp[i][0] * (w / 2 - c - 0.02f), y = bp[i][1] * (h / 2 - c - 0.015f);
    if (state >= 2 && x * cr.cx > 0 && y * cr.cy > 0 && fabsf(bp[i][0]) > 0.8f && fabsf(bp[i][1]) > 0.7f) continue;
    if (state >= 3 && x * cr.cx < 0 && y * cr.cy > 0 && fabsf(bp[i][0]) > 0.8f && fabsf(bp[i][1]) > 0.7f) continue;
    soup_push_at(s, x, y, t + plate_dz(x, y, w, h, state, &cr, f, 0), 0, 0, 0);
    sp_bolt(s, 0.022f, 0.012f);
    soup_pop(s);
  }
  /* the armour's marks, a hair proud of the face */
  float zf = t + 0.002f;
  if (ar == AR_WEAR) {                     /* hazard amber corner triangles */
    soup_rgb(s, C_HAZARD);
    for (int k = 0; k < 4; k++) {
      float sx = k & 1 ? 1 : -1, sy = k & 2 ? 1 : -1, l = 0.07f;
      float a0[3] = { sx * (w / 2 - c), sy * (h / 2 - c), zf }, a1[3] = { sx * (w / 2 - c - l), sy * (h / 2 - c), zf }, a2[3] = { sx * (w / 2 - c), sy * (h / 2 - c - l), zf };
      a0[2] += plate_dz(a0[0], a0[1], w, h, state, &cr, f, 0); a1[2] = a0[2]; a2[2] = a0[2];
      soup_tri_out(s, a0, a1, a2, ctr);
    }
  }
  if (ar == AR_TI && (panel == PN_LEFT || panel == PN_RIGHT)) {   /* the titanium heat tint: bone to amber to teal */
    static const uint32_t ramp[4] = { C_BONE, C_HAZARD, 0x9b7fd0, C_LIVE };
    float y0 = h * 0.18f, y1 = y0 + 0.08f, x0 = -w * 0.3f;
    for (int k = 0; k < 3; k++) {
      t3_color c0 = t3_color_hex(ramp[k]), c1 = t3_color_hex(ramp[k + 1]);
      float xa = x0 + w * 0.2f * k, xb = xa + w * 0.2f;
      float p0[3] = { xa, y0, zf }, p1[3] = { xb, y0, zf }, p2[3] = { xb, y1, zf }, p3[3] = { xa, y1, zf };
      float ca[3] = { c0.r, c0.g, c0.b }, cb[3] = { c1.r, c1.g, c1.b };
      p0[2] += plate_dz(xa, y0, w, h, state, &cr, f, 0); p1[2] += plate_dz(xb, y0, w, h, state, &cr, f, 0); p2[2] += plate_dz(xb, y1, w, h, state, &cr, f, 0); p3[2] += plate_dz(xa, y1, w, h, state, &cr, f, 0);
      soup_emit(s, p0, p1, p2, ca, cb, cb, 0, 0, 0); soup_emit(s, p0, p2, p3, ca, cb, ca, 0, 0, 0);
    }
  }
  if (ar == AR_STEEL && !top) {            /* the weld bead along the top edge */
    soup_rgb(s, C_RUST);
    soup_push_at(s, 0, h / 2 - c * 0.5f, t * 0.6f, 0, 0, 0); sp_box_at(s, w - 2 * c, 0.015f, t * 0.9f, 0, 0, 0); soup_pop(s);
  }
  if (ar == AR_PLY && state >= 2) {        /* plywood splits: a bone crack along the crease */
    soup_rgb(s, 0xd9c7a2u);
    for (int k = -1; k <= 1; k++) {
      float cx = (k * 0.22f) * w * cosf(cr.ang), cy = (k * 0.22f) * w * sinf(cr.ang) + cr.off * h * 0.3f;
      soup_push_at(s, cx, cy, t + plate_dz(cx, cy, w, h, state, &cr, f, 0) + 0.003f, 0, 0, cr.ang + (k & 1 ? 0.5f : -0.5f));
      sp_quad(s, w * 0.2f, 0.014f); soup_pop(s);
    }
  }
  if (b->build.chassis == CH_LOWBOY && (panel == PN_LEFT || panel == PN_RIGHT) && !b->is_ops) {   /* the server chassis vents */
    soup_rgb(s, BC_HOLE);
    for (int k = 0; k < 4; k++) {
      float x = -w * 0.3f + k * w * 0.13f, y = -h * 0.05f;
      soup_push_at(s, x, y, zf + plate_dz(x, y, w, h, state, &cr, f, 0), 0, 0, 0); sp_quad(s, 0.1f, 0.028f); soup_pop(s);
    }
  }
  if (state == 2) { soup_pop(s); soup_pop(s); }
}

/* the paint marks that ride on a plate (the lid stripe and decals, the front chevron) */
static void emit_plate_paint(soup_t *s, const wbot_t *b, int panel, float w, float h, float t, int state) {
  if (state >= 2) return;                  /* a bent plate's paint is under the scrapes */
  paint_t pt = bot_paint(b);
  crease_t cr = plate_crease(plate_seed(b->build.chassis, panel));
  float f = ARMOUR_LOOK[bot_armour(b)].depth, z = t + 0.003f;
  if (panel == PN_TOP && CHASSIS[b->build.chassis].shape != SH_CIRCLE) {
    float sw = w * (pt.stripe2 ? 0.3f : 0.16f), sl = h * 0.9f;
    soup_rgb(s, pt.stripe);
    soup_push_at(s, 0, 0, z + plate_dz(0, 0, w, h, state, &cr, f, 0), 0, 0, 0); sp_quad(s, sw, sl); soup_pop(s);
    if (b->build.decal == DC_HAZARD_STRIPES) {
      soup_rgb(s, C_HAZARD);
      for (int k = 0; k < 3; k++) { soup_push_at(s, -w * 0.3f + k * w * 0.3f, h * 0.25f, z + 0.002f, 0, 0, 0.785f); sp_quad(s, 0.07f, w * 0.4f); soup_pop(s); }
    }
  }
  if (panel == PN_FRONT && pt.chevron) {
    soup_rgb(s, pt.chevron);
    float cw = w * 0.42f, ch = h * 0.5f;
    for (int k = -1; k <= 1; k += 2) {
      soup_push_at(s, k * cw * 0.25f, 0, z, 0, 0, -k * atanf(ch / (cw * 0.5f)) + (k > 0 ? PI / 2 : -PI / 2)); sp_quad(s, 0.05f, sqrtf(cw * cw * 0.25f + ch * ch)); soup_pop(s);
    }
  }
}

/* the body face behind a plate that has gone (state 4), or glimpsed behind a hanging one (3): a dark hole,
 * bolt holes, the wiring loom and what sits behind that side (graphics 4.2) */
static void emit_internals(soup_t *s, const wbot_t *b, int panel, float w, float h, int state) {
  int ar = bot_armour(b);
  soup_rgb(s, BC_HOLE);
  soup_push_at(s, 0, 0, 0.002f, 0, 0, 0); sp_quad(s, w * 0.78f, h * 0.7f); soup_pop(s);
  if (state >= 4) {
    int nb = (panel == PN_LEFT || panel == PN_RIGHT) ? ARMOUR_LOOK[ar].bolts_side : panel == PN_MOUNT ? 0 : ARMOUR_LOOK[ar].bolts_end;
    if (nb < 4 && panel != PN_MOUNT) nb = 4;
    float bp[12][2]; nb = plate_bolts(nb, bp);
    for (int i = 0; i < nb; i++) {
      float x = bp[i][0] * (w / 2 - 0.03f), y = bp[i][1] * (h / 2 - 0.025f);
      soup_rgb(s, BC_STEEL); soup_push_at(s, x, y, 0.002f, 0, 0, 0); sp_hex(s, 0.024f); soup_pop(s);
      soup_rgb(s, BC_HOLE); soup_push_at(s, x, y, 0.004f, 0, 0, 0); sp_hex(s, 0.014f); soup_pop(s);
    }
  }
  /* the loom: rust, teal and bone runs across the hole */
  static const uint32_t loom[3] = { C_RUST, C_LIVE, C_BONE };
  for (int k = 0; k < 3; k++) {
    soup_rgb(s, loom[k]);
    float y = (k - 1) * h * 0.16f;
    soup_push_at(s, 0, y, 0.016f, 0, 0, (k - 1) * 0.12f); sp_cbox(s, w * 0.7f, 0.022f, 0.022f, 0.006f); soup_pop(s);
  }
  switch (panel) {
    case PN_FRONT: {                       /* the battery: rust with an amber stripe */
      float bw = w * 0.5f < 0.3f ? w * 0.5f : 0.3f, bh = h * 0.5f < 0.1f ? h * 0.5f : 0.1f;
      soup_rgb(s, C_RUST); sp_cbox_at(s, bw, bh, 0.08f, 0.01f, w * 0.12f, -h * 0.12f, 0.03f);
      soup_rgb(s, C_HAZARD); sp_box_at(s, bw * 1.02f, bh * 0.25f, 0.082f, w * 0.12f, -h * 0.12f, 0.031f);
      break; }
    case PN_BACK:                          /* the speed controller, a bone box, and its fan */
      soup_rgb(s, C_BONE); sp_cbox_at(s, w * 0.3f, h * 0.4f, 0.06f, 0.01f, -w * 0.15f, 0, 0.025f);
      soup_rgb(s, BC_STEEL_DK); soup_push_at(s, w * 0.2f, 0, 0.02f, PI / 2, 0, 0); sp_cyl(s, h * 0.2f, 0.03f, 8, 1); soup_pop(s);
      break;
    case PN_LEFT: case PN_RIGHT:           /* the drive motor cans */
      soup_rgb(s, BC_BRIGHT);
      for (int k = -1; k <= 1; k += 2) { soup_push_at(s, k * w * 0.28f, -h * 0.1f, 0.035f, PI / 2, 0, 0); sp_cyl(s, h * 0.22f < 0.06f ? h * 0.22f : 0.06f, 0.07f, 8, 1); soup_pop(s); }
      break;
    default: break;
  }
}

/* the plate transform: place, yaw then pitch (a SHELL flank plate leans with the dome) */
static xf_t plate_xf(const panel_place_t *p) {
  xf_t a = xf_make(p->x, p->y, p->z, 0, p->ry, 0), r = xf_make(0, 0, 0, p->rx, 0, 0);
  return xf_mul(&a, &r);
}

/* every plate of a bot at its state into the soup, in the body frame */
static void emit_plates(soup_t *s, const wbot_t *b) {
  for (int p = 0; p < PN_COUNT; p++) {
    const wplate_t *pl = &b->pl[p];
    if (!pl->on) continue;
    int st = b->pstate[p];
    soup_push(s, &pl->xf);
    if (st >= 3) emit_internals(s, b, p, pl->w, pl->h, st);
    if (st <= 2) {
      paint_t pt = bot_paint(b);
      uint32_t col = plate_colour(b, p == PN_TOP || p == PN_MOUNT ? pt.lid : pt.plate);
      emit_plate(s, b, p, pl->w, pl->h, pl->t, st, col, pl->hinge_top);
      emit_plate_paint(s, b, p, pl->w, pl->h, pl->t, st);
    }
    soup_pop(s);
  }
}

/* the plate soup's scratch: one static soup every bot's rewrite goes through, so a state change never allocates */
#define PLATE_SCRATCH_TRIS 4600
static soup_t plate_scratch;
static soup_t *plate_soup(void) {
  if (!plate_scratch.p) soup_init_vcol(&plate_scratch, PLATE_SCRATCH_TRIS);
  plate_scratch.owned = 0;
  soup_reset(&plate_scratch);
  return &plate_scratch;
}
/* a geometry of cap vertices, zeroed, with colours: written in place by plates_commit and decals_commit */
static t3_geometry *dyn_geometry(int cap, int vcol) {
  t3_geometry *g = t3_geometry_new();
  float *z = calloc((size_t)cap * 3, 4);
  t3_attribute *p = t3_attribute_new(T3_FLOAT32, z, cap, 3), *n = t3_attribute_new(T3_FLOAT32, z, cap, 3), *uv = t3_attribute_new(T3_FLOAT32, z, cap, 2);
  p->usage = n->usage = uv->usage = T3_DYNAMIC_DRAW;
  t3_geometry_set_attribute(g, T3_ATTR_POSITION, p); t3_release(p);
  t3_geometry_set_attribute(g, T3_ATTR_NORMAL, n); t3_release(n);
  t3_geometry_set_attribute(g, T3_ATTR_UV, uv); t3_release(uv);
  if (vcol) { t3_attribute *c = t3_attribute_new(T3_FLOAT32, z, cap, 3); c->usage = T3_DYNAMIC_DRAW; t3_geometry_set_attribute(g, T3_ATTR_COLOR, c); t3_release(c); }
  free(z);
  g->draw_start = 0; g->draw_count = 0;
  return g;
}
static void dyn_commit(t3_geometry *g, const soup_t *s) {
  t3_attribute *p = g->attributes[T3_ATTR_POSITION], *n = g->attributes[T3_ATTR_NORMAL], *uv = g->attributes[T3_ATTR_UV], *c = g->attributes[T3_ATTR_COLOR];
  int cnt = s->n_ < p->count ? s->n_ : p->count;
  memcpy(p->array, s->p, (size_t)cnt * 12); memcpy(n->array, s->n, (size_t)cnt * 12); memcpy(uv->array, s->uv, (size_t)cnt * 8);
  if (c) memcpy(c->array, s->c, (size_t)cnt * 12);
  p->version++; n->version++; uv->version++; if (c) c->version++;
  g->draw_count = cnt;
}
static void plates_commit(wbot_t *b) {
  if (!b->plates) return;
  soup_t *s = plate_soup();
  /* the static body goes first, the plates after it: one soup, one draw */
  int nb = b->base.n_ < s->cap ? b->base.n_ : s->cap;
  if (nb > 0) { memcpy(s->p, b->base.p, (size_t)nb * 12); memcpy(s->n, b->base.n, (size_t)nb * 12); memcpy(s->uv, b->base.uv, (size_t)nb * 8); memcpy(s->c, b->base.c, (size_t)nb * 12); s->n_ = nb; }
  emit_plates(s, b);
  dyn_commit(b->plates->geometry, s);
  b->plates_dirty = 0;
}

/* -- the chassis (graphics 3.2) -------------------------------------- */
/* the body soup: the char steel chassis with its bevels and lid line, the bevelled seat band, the belly pan
 * an inverted bot shows, and the chassis' own trim */
static void build_chassis(wbot_t *b, soup_t *sh) {
  const chassis_def_t *c = &CHASSIS[b->build.chassis];
  float L = c->len, W = c->wid, H = c->hgt, r = b->ride_y;
  if (b->is_ops) L *= 1.4f;
  b->len = L; b->wid = W;
  int frame = b->build.chassis == CH_FRAME, bare_frame = frame && b->build.armour == AR_NONE && !b->is_ops;
  b->panels_hidden = bare_frame;
  float deck = r + H, deck_w = W, deck_l = L;
  uint32_t seat = b->seat_colour;
  switch (b->build.chassis) {
    case CH_WEDGE: {
      soup_rgb(sh, BC_CHAR);
      sp_cbox_at(sh, W, H, L * 0.5f, 0.03f, 0, r + H / 2, -L * 0.25f);
      soup_push_at(sh, 0, r, L * 0.225f, 0, 0, 0); sp_wedge(sh, W, L * 0.55f, H); soup_pop(sh);
      /* the lip bar across the front edge, two skids under the tip and three bolts along the lip */
      soup_rgb(sh, BC_BRIGHT);
      sp_cyl_at(sh, 0.022f, W, 6, 0, 0, r + 0.02f, L / 2 - 0.01f);
      for (int k = -1; k <= 1; k += 2) sp_cbox_at(sh, 0.06f, 0.03f, 0.3f, 0.008f, k * 0.3f * W, r - 0.012f, L / 2 - 0.16f);
      for (int k = -1; k <= 1; k++) { soup_push_at(sh, k * W * 0.3f, r + 0.05f, L / 2 - 0.07f, -1.1f, 0, 0); sp_bolt(sh, 0.02f, 0.012f); soup_pop(sh); }
      break; }
    case CH_SHELL: {
      /* the wok: a rolled rim, twelve rivets round it and a hinge block at the back */
      const float prof[8][2] = { { 0, 0 }, { L * 0.42f, 0 }, { L * 0.5f, H * 0.3f }, { L * 0.52f, H * 0.36f }, { L * 0.5f, H * 0.42f }, { L * 0.45f, H * 0.8f }, { L * 0.25f, H }, { 0, H } };
      /* the wok is the bot's shell: it wears the lid paint darkened toward the frame */
      soup_rgb(sh, mix_rgb(plate_colour(b, bot_paint(b).lid), BC_CHAR, 0.45f));
      soup_push_at(sh, 0, r, 0, 0, 0, 0); sp_lathe(sh, prof, 8, 14, 0, TAU); soup_pop(sh);
      soup_rgb(sh, BC_BRIGHT);
      for (int k = 0; k < 12; k++) { float a = k * TAU / 12 + 0.26f; soup_push_at(sh, sinf(a) * L * 0.52f, r + H * 0.36f, cosf(a) * L * 0.52f, 0, a, 0); sp_bolt(sh, 0.02f, 0.012f); soup_pop(sh); }
      soup_rgb(sh, BC_CHAR_DK); sp_cbox_at(sh, 0.22f, 0.09f, 0.08f, 0.015f, 0, r + H * 0.55f, -L * 0.47f);
      break; }
    case CH_FRAME: {
      /* an open tube frame that catches the light, cross braces on the sides; the electronics box (bone),
       * a teal LED and a rust battery inside */
      soup_rgb(sh, 0x9a948au);
      float tr = 0.035f, x0 = W / 2 - tr, z0 = L / 2 - tr;
      for (int k = 0; k < 4; k++) sp_cyl_at(sh, tr, H, 6, 1, (k & 1 ? 1 : -1) * x0, r + H / 2, (k & 2 ? 1 : -1) * z0);
      for (int k = 0; k < 2; k++) {
        float y = r + (k ? H - tr : tr);
        sp_cyl_at(sh, tr, W, 6, 0, 0, y, z0); sp_cyl_at(sh, tr, W, 6, 0, 0, y, -z0);
        sp_cyl_at(sh, tr, L, 6, 2, -x0, y, 0); sp_cyl_at(sh, tr, L, 6, 2, x0, y, 0);
      }
      float dl = sqrtf(L * L + H * H), da = atanf(H / L);
      for (int k = -1; k <= 1; k += 2) { soup_push_at(sh, k * x0, r + H / 2, 0, k > 0 ? da : -da, 0, 0); sp_cyl(sh, tr * 0.8f, dl * 0.92f, 6, 2); soup_pop(sh); }
      soup_rgb(sh, C_BONE); sp_cbox_at(sh, W * 0.6f, H * 0.55f, L * 0.5f, 0.02f, -W * 0.05f, r + H * 0.3f + 0.035f, -L * 0.05f);
      soup_rgb(sh, C_RUST); sp_cbox_at(sh, W * 0.22f, H * 0.3f, L * 0.3f, 0.015f, W * 0.3f, r + H * 0.17f + 0.035f, L * 0.18f);
      soup_rgb(sh, C_HAZARD); sp_box_at(sh, W * 0.225f, 0.02f, L * 0.06f, W * 0.3f, r + H * 0.32f + 0.036f, L * 0.18f);
      if (bare_frame) { deck = r + H * 0.58f + 0.035f; deck_w = W * 0.6f; deck_l = L * 0.5f; }
      break; }
    case CH_LUNCHBOX: {
      /* a lid on a base with a hinge line, a bone handle arch and two latches */
      soup_rgb(sh, BC_CHAR); sp_cbox_at(sh, W, H * 0.7f, L, 0.03f, 0, r + H * 0.35f, 0);
      soup_rgb(sh, 0x443d33u); sp_cbox_at(sh, W + 0.01f, H * 0.3f, L + 0.01f, 0.04f, 0, r + H * 0.85f, 0);
      soup_rgb(sh, BC_HOLE); sp_box_at(sh, W + 0.014f, 0.012f, L + 0.014f, 0, r + H * 0.7f, 0);
      soup_rgb(sh, C_BONE);
      for (int k = -1; k <= 1; k += 2) sp_cbox_at(sh, 0.05f, 0.2f, 0.05f, 0.012f, 0, r + H + 0.1f, k * L * 0.22f);
      sp_cbox_at(sh, 0.06f, 0.05f, L * 0.44f + 0.06f, 0.015f, 0, r + H + 0.2f, 0);
      soup_rgb(sh, BC_BRIGHT);
      for (int k = -1; k <= 1; k += 2) sp_cbox_at(sh, 0.08f, 0.07f, 0.03f, 0.01f, k * W * 0.28f, r + H * 0.7f, L / 2 + 0.045f);
      break; }
    case CH_LOWBOY: {
      soup_rgb(sh, BC_CHAR); sp_cbox_at(sh, W, H, L, 0.03f, 0, r + H / 2, 0);
      /* rack ears at the front corners and a 2U handle on the lid */
      soup_rgb(sh, BC_BRIGHT);
      for (int k = -1; k <= 1; k += 2) sp_cbox_at(sh, 0.06f, H * 0.9f, 0.05f, 0.012f, k * (W / 2 + 0.035f), r + H / 2, L / 2 - 0.03f);
      for (int k = -1; k <= 1; k += 2) sp_box_at(sh, 0.025f, 0.05f, 0.025f, k * W * 0.2f, r + H + 0.025f, L * 0.42f);
      sp_cyl_at(sh, 0.016f, W * 0.42f, 6, 0, 0, r + H + 0.05f, L * 0.42f);
      break; }
    default: {   /* BOX */
      soup_rgb(sh, BC_CHAR); sp_cbox_at(sh, W, H, L, 0.035f, 0, r + H / 2, 0);
      /* two latches on the front edge and rubber corner bumpers at the back */
      soup_rgb(sh, BC_BRIGHT);
      for (int k = -1; k <= 1; k += 2) sp_cbox_at(sh, 0.08f, 0.05f, 0.03f, 0.01f, k * W * 0.25f, r + H - 0.035f, L / 2 + 0.04f);
      soup_rgb(sh, BC_RUBBER);
      for (int k = -1; k <= 1; k += 2) sp_cbox_at(sh, 0.07f, H, 0.07f, 0.015f, k * (W / 2 + 0.005f), r + H / 2, -L / 2 - 0.005f);
      break; }
  }
  /* the lid line: a dark recess round the top of a box body */
  if (c->shape == SH_BOX && !frame && b->build.chassis != CH_LUNCHBOX) {
    soup_rgb(sh, BC_HOLE);
    if (b->build.chassis == CH_WEDGE) sp_box_at(sh, W + 0.006f, 0.012f, L * 0.5f + 0.006f, 0, r + H - 0.045f, -L * 0.25f);
    else sp_box_at(sh, W + 0.006f, 0.012f, L + 0.006f, 0, r + H - 0.045f, 0);
  }
  /* the seat band: the one colour that never changes, bevelled so it catches the light; on a bone or rust
   * seat a char pin stripe above and below so it reads against bone and rust paint */
  int pin = seat == C_BONE || seat == C_RUST;
  if (c->shape == SH_CIRCLE) {
    soup_rgb(sh, seat); soup_push_at(sh, 0, r + H * 0.22f, 0, 0, 0, 0); sp_cyl(sh, L * 0.5f + 0.03f, 0.08f, 14, 1); soup_pop(sh);
    if (pin) { soup_rgb(sh, BC_PAINT_CHAR); for (int k = -1; k <= 1; k += 2) { soup_push_at(sh, 0, r + H * 0.22f + k * 0.045f, 0, 0, 0, 0); sp_cyl(sh, L * 0.5f + 0.034f, 0.01f, 14, 1); soup_pop(sh); } }
  } else if (!frame || !bare_frame) {
    /* proud of the plates, so it wraps the bot like a bumper strip */
    float pt_ = b->build.armour == AR_NONE && !b->is_ops ? 0 : ARMOUR_LOOK[bot_armour(b)].t, grow = 2 * pt_ + 0.03f;
    float bl = b->build.chassis == CH_WEDGE ? L * 0.5f : L, bz = b->build.chassis == CH_WEDGE ? -L * 0.25f : 0, bw = W;
    if (frame) { bw = W - 0.02f; bl = L - 0.02f; grow = 0.04f; }
    float by = r + (H < 0.3f ? 0.06f : 0.08f);
    soup_rgb(sh, seat); sp_cbox_at(sh, bw + grow, 0.07f, bl + grow, 0.014f, 0, by, bz);
    if (pin) { soup_rgb(sh, BC_PAINT_CHAR); for (int k = -1; k <= 1; k += 2) sp_box_at(sh, bw + grow + 0.006f, 0.01f, bl + grow + 0.006f, 0, by + k * 0.04f, bz); }
  } else {
    soup_rgb(sh, seat); sp_cbox_at(sh, W * 0.62f, 0.06f, L * 0.52f, 0.012f, -W * 0.05f, r + 0.07f, -L * 0.05f);
  }
  /* the belly pan an inverted bot shows: a char plate, two motor cans, a rust battery with an amber stripe
   * and four bolt heads */
  if (!frame) {
    float pw = c->shape == SH_CIRCLE ? L * 0.6f : W * 0.86f, pl = c->shape == SH_CIRCLE ? L * 0.6f : L * 0.8f;
    soup_rgb(sh, 0x2f2920u); sp_box_at(sh, pw, 0.02f, pl, 0, r - 0.006f, 0);
    soup_rgb(sh, BC_BRIGHT);
    for (int k = -1; k <= 1; k += 2) sp_cyl_at(sh, 0.05f, pw * 0.55f, 6, 0, 0, r - 0.05f, k * pl * 0.3f);
    soup_rgb(sh, C_RUST); sp_box_at(sh, pw * 0.4f, 0.07f, pl * 0.3f, 0, r - 0.045f, 0);
    soup_rgb(sh, C_HAZARD); sp_box_at(sh, pw * 0.41f, 0.072f, pl * 0.06f, 0, r - 0.045f, 0);
    soup_rgb(sh, BC_BRIGHT);
    for (int k = 0; k < 2; k++) { soup_push_at(sh, (k ? 1 : -1) * pw * 0.42f, r - 0.016f, (k ? 1 : -1) * pl * 0.44f, PI / 2, 0, 0); sp_bolt(sh, 0.02f, 0.01f); soup_pop(sh); }
  }
  if (b->is_ops) {
    /* OPS: an amber stripe on the lid; the tape bumper and the beacon mast are built in bot_rebuild */
    soup_rgb(sh, C_STEEL_HI); sp_cbox_at(sh, 0.04f, 0.3f, 0.04f, 0.01f, W * 0.3f, r + H + 0.15f, -L * 0.35f);
  }
  /* the plates: where each sits, its size and thickness; a FRAME with armour wears bolt on plates inside its
   * tube rectangle, a bare FRAME none, and NONE armour only a thin painted lid */
  const armour_look_t *al = &ARMOUR_LOOK[bot_armour(b)];
  for (int p = 0; p < PN_COUNT; p++) {
    panel_place_t pp = panel_place(c, p, r);
    if (b->is_ops && (p == PN_LEFT || p == PN_RIGHT)) pp.w *= 1.4f;
    if (b->build.chassis == CH_WEDGE && (p == PN_LEFT || p == PN_RIGHT)) { pp.w = L * 0.46f; pp.z = -L * 0.25f; }
    if (b->is_ops && (p == PN_FRONT)) pp.z = L / 2 + 0.003f;
    if (b->is_ops && (p == PN_BACK)) pp.z = -L / 2 - 0.003f;
    if (b->is_ops && (p == PN_TOP || p == PN_MOUNT)) { pp.z *= 1.4f; pp.h *= 1.4f; }
    if (frame && !bare_frame) { pp.w *= 0.55f; pp.h *= 0.55f; }
    wplate_t *pl = &b->pl[p];
    pl->w = pp.w; pl->h = pp.h; pl->t = al->t;
    pl->x = pp.x; pl->y = pp.y; pl->z = pp.z; pl->rx = pp.rx; pl->ry = pp.ry; pl->rz = 0;
    pl->xf = plate_xf(&pp);
    pl->hinge_top = p == PN_TOP || p == PN_MOUNT;
    pl->on = !bare_frame && (b->build.armour != AR_NONE || b->is_ops || p == PN_TOP);
    if (b->build.armour == AR_NONE && !b->is_ops) pl->t = 0.012f;
  }
  b->top_y = r + H;
  b->deck_y = deck; b->deck_w = deck_w; b->deck_l = deck_l;
}

/* -- the drives (graphics 3.3) --------------------------------------- */
/* a tyre round the x axis: rounded shoulders, alternate lugs on the tread, a dark hub with a bright cross
 * on its +x face (the outside face; a left wheel is turned round) */
static t3_geometry *tyre_geo(float r, float w) {
  int seg = 8;
  float key[8] = { r, w, (float)seg };
  t3_geometry *g = geo_find(GK_TYRE, key);
  if (g) return g;
  soup_t s; soup_init_vcol(&s, 140);
  soup_push_at(&s, 0, 0, 0, 0, 0, -PI / 2);             /* built round y, turned to x */
  const float pr[6][2] = { { 0.6f * r, -w / 2 }, { 0.9f * r, -w / 2 }, { r, -0.28f * w }, { r, 0.28f * w }, { 0.9f * r, w / 2 }, { 0.6f * r, w / 2 } };
  for (int j = 0; j < seg; j++) {
    float a0 = j * TAU / seg, a1 = (j + 1) * TAU / seg, am = (a0 + a1) / 2;
    for (int i = 1; i < 5; i++) {
      soup_rgb(&s, i == 2 && (j & 1) ? 0x3d362cu : BC_RUBBER);
      float r0 = pr[i][0], y0 = pr[i][1], r1 = pr[i + 1][0], y1 = pr[i + 1][1];
      float p00[3] = { sinf(a0) * r0, y0, cosf(a0) * r0 }, p01[3] = { sinf(a1) * r0, y0, cosf(a1) * r0 }, p10[3] = { sinf(a0) * r1, y1, cosf(a0) * r1 }, p11[3] = { sinf(a1) * r1, y1, cosf(a1) * r1 };
      float nr = y1 - y0, ny = -(r1 - r0), nl = sqrtf(nr * nr + ny * ny); nr /= nl; ny /= nl;
      float rm = (r0 + r1) / 2 - nr * 0.01f, ym = (y0 + y1) / 2 - ny * 0.01f, ctr[3] = { sinf(am) * rm, ym, cosf(am) * rm };
      soup_quad_out(&s, p00, p01, p11, p10, ctr);
    }
  }
  soup_rgb(&s, BC_STEEL_DK); sp_cyl(&s, 0.6f * r, w * 0.7f, 6, 1);
  soup_rgb(&s, BC_BRIGHT);
  for (int k = 0; k < 2; k++) { soup_push_at(&s, 0, w * 0.35f + 0.004f, 0, -PI / 2, 0, k * PI / 2); sp_quad(&s, 0.06f * (r / 0.17f), 1.15f * r); soup_pop(&s); }
  soup_pop(&s);
  return geo_keep(GK_TYRE, key, soup_finish(&s));
}

static void build_wheel(wbot_t *b, float r, float w, float x, float y, float z) {
  if (b->nwheels >= 8) return;
  t3_mesh *m = geo_mesh(b->drive_g, tyre_geo(r, w), VM_RUBBER, x, y, z);
  b->wheel_x[b->nwheels] = x; b->wheel_y[b->nwheels] = y; b->wheel_z[b->nwheels] = z;
  b->wheels[b->nwheels++] = m;
}

/* a closed tread belt round the x axis (a stadium in the yz plane), outer and inner faces, v along the
 * loop so the tread canvas scrolls round it */
static t3_geometry *belt_geo(float len, float hgt, float wid) {
  float key[8] = { len, hgt, wid };
  t3_geometry *g = geo_find(GK_BELT, key);
  if (g) return g;
  float R = hgt / 2, zc = len / 2 - R;
  float pts[20][2]; int n = 0;                         /* (y, z) round the loop */
  for (int k = 0; k <= 5; k++) { float a = PI / 2 - k * PI / 5; pts[n][0] = R * sinf(a); pts[n][1] = zc + R * cosf(a); n++; }
  for (int k = 0; k <= 5; k++) { float a = -PI / 2 - k * PI / 5; pts[n][0] = R * sinf(a); pts[n][1] = -zc + R * cosf(a); n++; }
  soup_t s; soup_init(&s, n * 4);
  float v = 0;
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    float dy = pts[j][0] - pts[i][0], dz = pts[j][1] - pts[i][1], seg = sqrtf(dy * dy + dz * dz), v1 = v + seg * 4.0f;
    for (int face = 0; face < 2; face++) {
      float k = face ? 0.93f : 1.0f, yi = pts[i][0] * k, zi = pts[i][1] * (face ? (zc + R * 0.93f) / (zc + R) : 1), yj = pts[j][0] * k, zj = pts[j][1] * (face ? (zc + R * 0.93f) / (zc + R) : 1);
      float a[3] = { -wid / 2, yi, zi }, bb[3] = { wid / 2, yi, zi }, c[3] = { wid / 2, yj, zj }, d[3] = { -wid / 2, yj, zj };
      float ua[2] = { 0, v }, ub[2] = { 1, v }, uc[2] = { 1, v1 }, ud[2] = { 0, v1 };
      float ctr[3] = { 0, 0, 0 };
      int out = faces_out(a, bb, c, ctr);
      if (face) out = !out;                            /* the inner face looks into the loop */
      if (out) { soup_emit(&s, a, bb, c, 0, 0, 0, ua, ub, uc); soup_emit(&s, a, c, d, 0, 0, 0, ua, uc, ud); }
      else { soup_emit(&s, a, c, bb, 0, 0, 0, ua, uc, ub); soup_emit(&s, a, d, c, 0, 0, 0, ua, ud, uc); }
    }
    v = v1;
  }
  return geo_keep(GK_BELT, key, soup_finish(&s));
}
static t3_geometry *sprocket_geo(float r, float t) {
  float key[8] = { r, t };
  t3_geometry *g = geo_find(GK_SPROCKET, key);
  if (g) return g;
  soup_t s; soup_init_vcol(&s, 80);
  soup_push_at(&s, 0, 0, 0, 0, 0, -PI / 2);
  soup_rgb(&s, BC_STEEL); sp_gear(&s, 5, r, r * 0.74f, t);
  soup_pop(&s);
  return geo_keep(GK_SPROCKET, key, soup_finish(&s));
}
static t3_material *tread_material(wbot_t *b) {
  canvas_t cv = cv_new(16, 32);
  for (int y = 0; y < 32; y++) for (int x = 0; x < 16; x++) {
    int lug = (y % 8) < 3, edge = x < 2 || x > 13;
    cv_px(&cv, x, y, lug && !edge ? 0x5a5246u : (lug ? 0x4a4338u : 0x1d1915u), 255);
    if ((y % 8) == 0 && !edge) cv_px(&cv, x, y, 0x6b655au, 255);
  }
  b->tread_tex = cv_texture(&cv, 1, 1);
  b->tread_tex->repeat.x = 1; b->tread_tex->repeat.y = 1;
  t3_material *tm = t3_mesh_standard_material_new(0xffffff); tm->roughness = 0.9f; tm->metalness = 0.1f; t3_material_set_map(tm, b->tread_tex);
  return tm;
}

/* a leg: the thigh from the hip out and down to the knee, then the shin and its foot (or a tread pod) */
static t3_geometry *thigh_geo(void) {
  float key[8] = { 1 };
  t3_geometry *g = geo_find(GK_LEG_THIGH, key);
  if (g) return g;
  soup_t s; soup_init_vcol(&s, 120);
  soup_rgb(&s, BC_BRIGHT); sp_cbox(&s, 0.1f, 0.1f, 0.1f, 0.02f);                     /* the hip */
  soup_rgb(&s, BC_STEEL); soup_push_at(&s, 0.04f, -0.08f, 0, 0, 0, 0.45f); sp_cbox(&s, 0.07f, 0.2f, 0.07f, 0.015f); soup_pop(&s);
  return geo_keep(GK_LEG_THIGH, key, soup_finish(&s));
}
static t3_geometry *shin_geo(void) {
  float key[8] = { 1 };
  t3_geometry *g = geo_find(GK_LEG_SHIN, key);
  if (g) return g;
  soup_t s; soup_init_vcol(&s, 120);
  soup_rgb(&s, BC_BRIGHT); sp_cyl(&s, 0.04f, 0.1f, 6, 0);                            /* the knee pin */
  soup_rgb(&s, BC_STEEL); sp_cbox_at(&s, 0.06f, 0.13f, 0.06f, 0.012f, 0, -0.065f, 0);
  soup_rgb(&s, C_RUST); sp_cbox_at(&s, 0.15f, 0.035f, 0.14f, 0.01f, 0, -0.135f, 0.01f);
  return geo_keep(GK_LEG_SHIN, key, soup_finish(&s));
}
static t3_geometry *fan_geo(float r) {
  float key[8] = { r };
  t3_geometry *g = geo_find(GK_FAN, key);
  if (g) return g;
  soup_t s; soup_init_vcol(&s, 40);
  soup_rgb(&s, BC_CHAR_DK);
  for (int k = 0; k < 2; k++) { soup_push_at(&s, 0, 0, 0, 0, k * PI / 2 + 0.3f, 0); sp_box_at(&s, r * 1.8f, 0.012f, 0.05f, 0, 0, 0); soup_pop(&s); }
  soup_rgb(&s, BC_BRIGHT); sp_cyl(&s, r * 0.25f, 0.03f, 6, 1);
  return geo_keep(GK_FAN, key, soup_finish(&s));
}
/* the hover skirt: a pleated lathe that bulges out and tucks under */
static t3_geometry *skirt_geo(float R) {
  float key[8] = { R };
  t3_geometry *g = geo_find(GK_SKIRT, key);
  if (g) return g;
  const float pr[5][2] = { { 0.72f, 0.0f }, { 0.97f, 0.03f }, { 1.0f, 0.1f }, { 0.96f, 0.17f }, { 0.78f, 0.2f } };
  int seg = 16;
  soup_t s; soup_init_vcol(&s, seg * 8 + 4);
  for (int j = 0; j < seg; j++) {
    float a0 = j * TAU / seg, a1 = (j + 1) * TAU / seg, am = (a0 + a1) / 2, f0 = j & 1 ? 0.94f : 1.0f, f1 = j & 1 ? 1.0f : 0.94f;
    soup_rgb(&s, j & 1 ? 0x2f2920u : 0x4a4338u);
    for (int i = 0; i < 4; i++) {
      float r0 = pr[i][0] * R, y0 = pr[i][1], r1 = pr[i + 1][0] * R, y1 = pr[i + 1][1];
      float k0 = (i == 0 || i == 3) ? 1 : 0;   /* the pleats fold the bulge, not the lips */
      float g0 = 1 + (f0 - 1) * (1 - k0), g1 = 1 + (f1 - 1) * (1 - k0), h0 = 1 + (f0 - 1) * (i + 1 == 4 ? 0 : 1), h1 = 1 + (f1 - 1) * (i + 1 == 4 ? 0 : 1);
      float p00[3] = { sinf(a0) * r0 * g0, y0, cosf(a0) * r0 * g0 }, p01[3] = { sinf(a1) * r0 * g1, y0, cosf(a1) * r0 * g1 };
      float p10[3] = { sinf(a0) * r1 * h0, y1, cosf(a0) * r1 * h0 }, p11[3] = { sinf(a1) * r1 * h1, y1, cosf(a1) * r1 * h1 };
      float nr = y1 - y0, ny = -(r1 - r0), nl = sqrtf(nr * nr + ny * ny); nr /= nl; ny /= nl;
      float rm = (r0 + r1) / 2 - nr * 0.01f, ym = (y0 + y1) / 2 - ny * 0.01f, ctr[3] = { sinf(am) * rm, ym, cosf(am) * rm };
      soup_quad_out(&s, p00, p01, p11, p10, ctr);
    }
  }
  return geo_keep(GK_SKIRT, key, soup_finish(&s));
}

static void build_drive(wbot_t *b, const chassis_def_t *c, soup_t *ds, soup_t *sh) {
  float L = b->len ? b->len : c->len, W = c->wid;
  const armour_look_t *al = &ARMOUR_LOOK[bot_armour(b)];
  float side = W / 2 + al->t + 0.015f;                /* a wheel never meets a side plate */
  b->nwheels = 0; b->nlegs = 0; b->nbelts = 0; b->nsprockets = 0; b->skirt = 0; b->fans[0] = b->fans[1] = 0;
  switch (b->build.drive) {
    case DR_TWO: {
      float w = 0.16f;
      for (int k = -1; k <= 1; k += 2) build_wheel(b, 0.24f, w, k * (side + w / 2), 0.24f, -L * 0.2f);
      /* motor cans inboard of the wheels, the skid caster at the front */
      soup_rgb(ds, BC_BRIGHT);
      for (int k = -1; k <= 1; k += 2) sp_cyl_at(ds, 0.06f, 0.05f + al->t, 8, 0, k * (W / 2 + (0.05f + al->t) / 2), 0.24f, -L * 0.2f);
      soup_rgb(ds, BC_RUBBER); sp_cyl_at(ds, 0.07f, 0.05f, 8, 0, 0, 0.07f, L * 0.38f);
      soup_rgb(ds, BC_STEEL); sp_cbox_at(ds, 0.12f, 0.06f, 0.1f, 0.01f, 0, 0.13f, L * 0.38f);
      break; }
    case DR_FOUR: case DR_SIX: {
      int six = b->build.drive == DR_SIX;
      float r = six ? 0.15f : 0.17f, w = six ? 0.13f : 0.14f;
      for (int k = 0; k < (six ? 6 : 4); k++) {
        float z = six ? ((k >> 1) - 1) * L * 0.34f : (k & 2 ? 1 : -1) * L * 0.3f, y = r - (six && (k >> 1) == 1 ? 0.01f : 0);
        build_wheel(b, r, w, (k & 1 ? 1 : -1) * (side + w / 2), y, z);
      }
      soup_rgb(ds, BC_BRIGHT);   /* axles between each pair */
      for (int k = 0; k < (six ? 3 : 2); k++) { float z = six ? (k - 1) * L * 0.34f : (k ? 1 : -1) * L * 0.3f; sp_cyl_at(ds, 0.025f, 2 * side + w, 6, 0, 0, r, z); }
      break; }
    case DR_TREAD: case DR_LEGTRACK: {
      t3_material *tm = tread_material(b);
      if (b->build.drive == DR_TREAD) {
        float bl = L + 0.15f, bh = 0.3f, bw = 0.26f, R = bh / 2, zc = bl / 2 - R, x = side + bw / 2 - 0.01f;
        for (int k = -1; k <= 1; k += 2) {
          t3_mesh *bm = geo_mesh(b->drive_g, belt_geo(bl, bh, bw), tm, k * x, R + 0.01f, 0);
          b->belts[b->nbelts++] = bm;
          for (int e = -1; e <= 1; e += 2) b->sprockets[b->nsprockets++] = geo_mesh(b->drive_g, sprocket_geo(R * 0.82f, bw * 0.55f), VM_METAL, k * x, R + 0.01f, e * zc);
          /* three road wheels along the bottom run, and a side frame between the sprockets */
          soup_rgb(ds, BC_STEEL_DK);
          for (int r = -1; r <= 1; r += 2) sp_cyl_at(ds, 0.062f, bw * 0.6f, 6, 0, k * x, 0.085f, r * zc * 0.45f);
          soup_rgb(ds, BC_CHAR_DK); sp_box_at(ds, 0.03f, 0.08f, zc * 1.6f, k * (x - bw * 0.3f), R + 0.01f, 0);
        }
      } else {
        /* LEG TRACKS: per side two knees over tread pods */
        float x = side + 0.12f;
        for (int k = 0; k < 4; k++) {
          int sd = k & 1 ? 1 : -1; float z = (k & 2 ? 1 : -1) * L * 0.3f;
          t3_object *hip = group_at(b->drive_g, sd * x, 0.3f, z);
          if (sd < 0) t3_object_set_rotation(hip, 0, PI, 0);
          geo_mesh(hip, thigh_geo(), VM_BODY, 0, 0, 0);
          t3_object *pod = group_at(hip, 0.06f, -0.19f, 0);
          t3_mesh *pm = geo_mesh(pod, belt_geo(0.5f, 0.2f, 0.22f), tm, 0, 0, 0);
          b->belts[b->nbelts++] = pm;
          b->legs[b->nlegs] = hip; b->knees[b->nlegs] = pod; b->nlegs++;
        }
      }
      t3_release(tm);
      break; }
    case DR_HOVER: {
      float R = (L > W ? L : W) * 0.6f;
      b->skirt = geo_mesh(b->drive_g, skirt_geo(R), VM_RUBBER, 0, 0.0f, 0);
      /* the bone rim round the skirt's shoulder, and the deck the body sits on */
      soup_rgb(ds, C_BONE); soup_push_at(ds, 0, 0.165f, 0, 0, 0, 0); sp_cyl(ds, R * 0.975f, 0.025f, 16, 1); soup_pop(ds);
      soup_rgb(ds, BC_CHAR_DK); soup_push_at(ds, 0, 0.205f, 0, 0, 0, 0); sp_cyl(ds, R * 0.8f, 0.03f, 16, 1); soup_pop(ds);
      /* two fan intakes on the lid with their blades, and a rust fuel can */
      float fy = c->hgt + b->ride_y;
      for (int k = 0; k < 2; k++) {
        float fx = (k ? 1 : -1) * W * 0.26f, fz = -L * 0.28f;
        soup_rgb(sh, BC_STEEL); sp_cyl_at(sh, 0.15f, 0.06f, 10, 1, fx, fy + 0.03f, fz);
        soup_rgb(sh, BC_HOLE); sp_cyl_at(sh, 0.12f, 0.062f, 10, 1, fx, fy + 0.032f, fz);
        b->fans[k] = geo_mesh(b->body, fan_geo(0.12f), VM_BODY, fx, fy + 0.07f, fz);
      }
      soup_rgb(sh, C_RUST); sp_cyl_at(sh, 0.06f, 0.16f, 8, 1, 0, fy + 0.08f, -L * 0.38f);
      b->radius = R;
      break; }
    case DR_WALKER: {
      for (int k = 0; k < 6; k++) {
        int sd = k & 1 ? 1 : -1; float z = ((k >> 1) - 1) * L * 0.36f;
        t3_object *hip = group_at(b->drive_g, sd * (W / 2 + 0.06f), 0.31f, z);
        if (sd < 0) t3_object_set_rotation(hip, 0, PI, 0);
        geo_mesh(hip, thigh_geo(), VM_BODY, 0, 0, 0);
        t3_object *knee = group_at(hip, 0.09f, -0.16f, 0);
        geo_mesh(knee, shin_geo(), VM_BODY, 0, 0, 0);
        b->legs[b->nlegs] = hip; b->knees[b->nlegs] = knee; b->nlegs++;
      }
      soup_rgb(ds, BC_STEEL);
      for (int k = -1; k <= 1; k++) sp_cyl_at(ds, 0.03f, W + 0.1f, 6, 0, 0, 0.31f, k * L * 0.36f);
      break; }
    case DR_MELTY: {
      for (int k = -1; k <= 1; k += 2) build_wheel(b, 0.16f, 0.12f, k * (side + 0.06f), 0.16f, 0);
      /* four bright edge bars with amber chevrons: the weapon is the whole bot */
      for (int k = 0; k < 4; k++) {
        float a = k * PI / 2, R = (k & 1 ? W : L) / 2 + 0.04f;
        soup_push_at(sh, sinf(a) * R, b->ride_y + c->hgt / 2, cosf(a) * R, 0, a, 0);
        soup_rgb(sh, BC_BRIGHT); sp_cbox(sh, 0.3f, c->hgt * 0.9f, 0.12f, 0.02f);
        soup_rgb(sh, C_HAZARD);
        for (int j = -1; j <= 1; j += 2) { soup_push_at(sh, j * 0.05f, 0, 0.062f, 0, 0, j * 0.6f); sp_quad(sh, 0.035f, c->hgt * 0.6f); soup_pop(sh); }
        soup_pop(sh);
      }
      break; }
    default: break;
  }
}

/* -- the weapons (graphics 3.4) -------------------------------------- */
/* every weapon has a READY, TELL, STRIKE and RECOVER look (pose_weapon). What does not move goes in the body
 * soup in body coordinates; each moving part is its own VM_METAL soup under its pivot */
#define DEG(d) ((d) * PI / 180.0f)
#define WEDGE_ANG DEG(28.0f)

/* the height of the chassis front surface z where it is h high: where the WEDGE plate's hinge and a front
 * mount bolt on */
static float front_z(const wbot_t *b, float h) {
  const chassis_def_t *c = &CHASSIS[b->build.chassis];
  float L = b->len, t = ARMOUR_LOOK[bot_armour(b)].t;
  if (b->build.chassis == CH_WEDGE) {
    float run = L * 0.55f, k = (h - b->ride_y) / c->hgt; if (k < 0) k = 0; if (k > 1) k = 1;
    return L / 2 - k * run + t * 0.5f;
  }
  if (c->shape == SH_CIRCLE) return L * 0.5f;
  if (b->build.chassis == CH_FRAME) return L / 2 + 0.03f;
  return L / 2 + t;
}

/* the soup of a moving part, a mesh under parent */
static t3_mesh *part_mesh(void *parent, soup_t *s) { return soup_mesh(parent, s, VM_METAL); }

static void build_weapon(wbot_t *b, int secondary, soup_t *sh) {
  int wp = secondary ? b->build.weapon2 : b->build.weapon;
  const chassis_def_t *c = &CHASSIS[b->build.chassis];
  t3_object *g = secondary ? b->weapon2_g : b->weapon_g;
  int dome = c->shape == SH_CIRCLE;
  float L = b->len, W = c->wid, top = b->ride_y + c->hgt, r = b->ride_y;
  float z0 = front_z(b, r + c->hgt * 0.5f), y0 = r + c->hgt * 0.5f;
  int si = secondary ? 2 : 0;                      /* wsub slots: 0..1 the primary's, 2..3 the 2ND's */
  /* a bracket with two bolts that touches the chassis */
  #define BRACKET(w_, h_, d_, x_, y_, z_) do { soup_rgb(sh, BC_STEEL_DK); sp_cbox_at(sh, w_, h_, d_, 0.012f, x_, y_, z_); soup_rgb(sh, BC_BRIGHT); \
    for (int bk = -1; bk <= 1; bk += 2) { soup_push_at(sh, (x_) + bk * (w_) * 0.28f, (y_) + (h_) / 2, z_, -PI / 2, 0, 0); sp_bolt(sh, 0.018f, 0.01f); soup_pop(sh); } } while (0)
  if (dome) {
    z0 = L * 0.48f; y0 = r + c->hgt * 0.42f;
    if (wp != WP_WEDGE && wp != WP_HAMMER && wp != WP_FLIPPER && wp != WP_HORIZ && wp != WP_CRUSHER && wp != WP_DRUM && wp != WP_NONE)
      BRACKET(0.18f, 0.14f, 0.34f, 0, y0 - 0.07f, z0 - 0.17f);
  }
  if (secondary) {
    if (b->build.chassis == CH_LUNCHBOX && !(b->build.weapon == WP_WEDGE || b->build.weapon == WP_LIFTER)) { z0 = L * 0.1f; y0 = top + 0.05f; BRACKET(0.2f, 0.06f, 0.24f, 0, top + 0.01f, z0); }
    else if (b->build.weapon == WP_WEDGE) {
      /* on the wedge plate: a bracket from the plate up to the mount */
      float zp = b->hinge_z + 0.16f, sy = b->hinge_y - 0.16f * tanf(WEDGE_ANG);
      z0 = zp; y0 = sy + 0.14f;
      BRACKET(0.16f, y0 - sy, 0.16f, 0, (y0 + sy) / 2 - 0.04f, z0);
    } else {
      /* ahead of the chassis front, bolted to it */
      y0 = r + c->hgt * 0.75f; z0 = front_z(b, y0) + 0.12f;
      BRACKET(0.14f, 0.1f, 0.3f, 0, y0 - 0.06f, z0 - 0.12f);
    }
  }
  t3_object *piv = group_at(g, 0, y0, z0);
  if (secondary) b->w2pivot = piv; else b->wpivot = piv;
  soup_t s; soup_init_vcol(&s, 400);
  switch (wp) {
    case WP_WEDGE: {
      /* a steel plate at 28 degrees, no taller than half a unit: the tip on the floor 0.42 ahead of the hinge,
       * two skids under it, a bright lip bar, three bolts and an amber band, hinged on a bar at the front */
      float run = 0.42f, rise = run * tanf(WEDGE_ANG) + 0.01f, plen = run / cosf(WEDGE_ANG);
      float hz = front_z(b, rise) + 0.03f, hy = rise;
      if (b->build.chassis == CH_WEDGE) hz = front_z(b, rise) + 0.01f;
      b->hinge_y = hy; b->hinge_z = hz;
      t3_object_set_position(piv, 0, hy, hz);
      soup_rgb(&s, 0x8a857cu);
      soup_push_at(&s, 0, -rise / 2, run / 2, WEDGE_ANG, 0, 0); sp_cbox(&s, W * 1.05f, 0.03f, plen, 0.008f); soup_pop(&s);
      soup_rgb(&s, BC_BRIGHT);
      sp_cyl_at(&s, 0.03f, W * 1.0f, 8, 0, 0, 0, 0);                                     /* the hinge bar */
      sp_cyl_at(&s, 0.02f, W * 1.06f, 6, 0, 0, -rise + 0.01f, run - 0.01f);              /* the lip bar */
      for (int k = -1; k <= 1; k++) { soup_push_at(&s, k * W * 0.3f, -rise * 0.62f + 0.016f, run * 0.62f, -(PI / 2 - WEDGE_ANG), 0, 0); sp_bolt(&s, 0.02f, 0.012f); soup_pop(&s); }
      soup_rgb(&s, C_HAZARD);
      soup_push_at(&s, 0, -rise * 0.86f + 0.017f, run * 0.86f, -(PI / 2 - WEDGE_ANG), 0, 0); sp_quad(&s, W * 1.04f, 0.05f); soup_pop(&s);
      soup_rgb(&s, BC_STEEL_DK);
      for (int k = -1; k <= 1; k += 2) sp_box_at(&s, 0.06f, 0.03f, 0.26f, k * 0.3f * W, -rise + 0.005f, run - 0.15f);
      /* two cheeks from the chassis to the hinge bar */
      BRACKET(0.06f, 0.1f, 0.08f, -W * 0.4f, hy - 0.03f, hz - 0.05f);
      BRACKET(0.06f, 0.1f, 0.08f, W * 0.4f, hy - 0.03f, hz - 0.05f);
      part_mesh(piv, &s);
      if (!secondary) { b->glint_x = 0; b->glint_y = hy - rise + 0.05f; b->glint_z = hz + run; b->glint_parent = b->body; }
      break; }
    case WP_SPIKE: {
      /* a square guide block, a fat rod with a bright cone tip that rests just out of the guide, a rust
       * spring collar that compresses as it recovers */
      soup_rgb(sh, BC_STEEL); sp_cbox_at(sh, 0.22f, 0.2f, 0.26f, 0.02f, 0, y0, z0 - 0.08f);
      soup_rgb(sh, BC_BRIGHT); for (int k = -1; k <= 1; k += 2) { soup_push_at(sh, k * 0.07f, y0 + 0.1f, z0 - 0.08f, -PI / 2, 0, 0); sp_bolt(sh, 0.018f, 0.01f); soup_pop(sh); }
      t3_object *rod = group_at(piv, 0, 0, 0);
      soup_rgb(&s, BC_BRIGHT); sp_cyl_at(&s, 0.08f, 0.36f, 8, 2, 0, 0, -0.22f);
      soup_push_at(&s, 0, 0, 0.03f, PI / 2, 0, 0); sp_cyl2(&s, 0.1f, 0.0f, 0.14f, 8, 1); soup_pop(&s);
      part_mesh(rod, &s);
      b->wsub[si] = (t3_mesh *)rod;
      soup_t sc; soup_init_vcol(&sc, 40); soup_rgb(&sc, C_RUST); sp_cyl(&sc, 0.11f, 0.06f, 8, 2);
      t3_mesh *col = part_mesh(piv, &sc); if (col) t3_object_set_position(col, 0, 0, 0.0f);
      b->wsub[si + 1] = col;
      if (!secondary) { b->glint_x = 0; b->glint_y = y0; b->glint_z = z0 + 0.1f; b->glint_parent = b->body; }
      break; }
    case WP_LIFTER: {
      /* bevelled forks along the floor so the lifter gets under, a crossbar, two hinge blocks and a
       * hydraulic ram that grows as it lifts */
      float fork_y = (r < 0.2f ? r : 0.2f) + 0.035f;
      if (!secondary) { y0 = fork_y + 0.09f; z0 = front_z(b, y0) + 0.02f; t3_object_set_position(piv, 0, y0, z0); }
      float fy = fork_y - y0;
      soup_rgb(&s, BC_STEEL);
      for (int k = -1; k <= 1; k += 2) {
        sp_cbox_at(&s, 0.1f, 0.06f, 0.5f, 0.012f, k * 0.22f, fy, 0.25f);
        soup_push_at(&s, k * 0.22f, fy - 0.03f, 0.56f, 0, PI, 0); sp_wedge(&s, 0.1f, 0.12f, 0.06f); soup_pop(&s);   /* the toe */
      }
      sp_cbox_at(&s, 0.6f, 0.08f, 0.1f, 0.015f, 0, fy + 0.03f, 0.02f);
      soup_rgb(&s, C_HAZARD); sp_box_at(&s, 0.62f, 0.03f, 0.05f, 0, fy + 0.075f, 0.02f);
      /* the hydraulic ram from the hinge block to the crossbar rides with the forks (one draw) */
      soup_push_at(&s, 0, 0.02f, -0.06f, 0.5f, 0, 0);
      soup_rgb(&s, BC_BRIGHT); sp_cyl_at(&s, 0.035f, 0.2f, 6, 1, 0, 0.1f, 0);
      soup_rgb(&s, BC_STEEL_DK); sp_cyl_at(&s, 0.05f, 0.08f, 6, 1, 0, 0.0f, 0);
      soup_pop(&s);
      part_mesh(piv, &s);
      soup_rgb(sh, BC_STEEL_DK);
      for (int k = -1; k <= 1; k += 2) sp_cbox_at(sh, 0.1f, 0.14f, 0.18f, 0.015f, k * 0.24f, y0, z0 - 0.08f);
      if (!secondary) { b->glint_x = 0.22f; b->glint_y = fork_y + 0.05f; b->glint_z = z0 + 0.5f; b->glint_parent = b->body; }
      break; }
    case WP_SAW: {
      /* the arm reaches the disc, the disc rides on a visible axle, twelve bright teeth */
      soup_rgb(sh, BC_STEEL); sp_cbox_at(sh, 0.12f, 0.1f, 0.72f, 0.02f, 0, y0 + 0.1f, z0 + 0.36f);
      t3_object *disc = group_at(piv, 0, 0.05f, 0.7f);
      soup_rgb(&s, BC_BRIGHT); sp_cyl(&s, 0.05f, 0.2f, 6, 0);
      soup_rgb(&s, 0xb9b4aau); sp_cyl(&s, 0.27f, 0.03f, 12, 0);
      soup_rgb(&s, 0xe6e2d8u); soup_push_at(&s, 0, 0, 0, 0, 0, -PI / 2); sp_teeth(&s, 12, 0.26f, 0.06f, 0.03f, 0.06f, 0.0f); soup_pop(&s);
      part_mesh(disc, &s);
      b->wsub[si] = (t3_mesh *)disc;
      if (!secondary) { b->ring_r = 0.32f; b->ring_y = y0 + 0.05f; b->ring_z = z0 + 0.7f; }
      if (!secondary) { b->glint_x = 0; b->glint_y = y0 + 0.37f; b->glint_z = z0 + 0.7f; b->glint_parent = b->body; }
      break; }
    case WP_FLAME: {
      /* a rust tank lying across the back with a bone valve, a hose, the nozzle, an amber pilot that is
       * always lit while there is fuel, and the cone */
      soup_rgb(sh, C_RUST); sp_cyl_at(sh, 0.12f, 0.32f, 8, 0, 0, y0 + 0.06f, z0 - 0.16f);
      soup_rgb(sh, C_BONE); sp_cbox_at(sh, 0.05f, 0.06f, 0.05f, 0.01f, 0.1f, y0 + 0.19f, z0 - 0.16f);
      soup_rgb(sh, BC_RUBBER); sp_cyl_at(sh, 0.03f, 0.2f, 6, 2, 0, y0 + 0.08f, z0 - 0.0f);
      soup_rgb(sh, BC_CHAR_DK); sp_cyl_at(sh, 0.05f, 0.34f, 8, 2, 0, y0 + 0.08f, z0 + 0.25f);
      soup_rgb(sh, BC_BRIGHT); sp_cyl_at(sh, 0.075f, 0.06f, 8, 2, 0, y0 + 0.08f, z0 + 0.43f);
      t3_mesh *pilot = t3_mesh_new(g_sphere, emis(C_HAZARD, 2.5f));
      t3_object_set_scale(pilot, 0.035f, 0.035f, 0.035f); t3_object_set_position(pilot, 0, 0.13f, 0.47f);
      t3_object_add(piv, pilot); t3_release(pilot);
      b->wsub[si + 1] = pilot;
      t3_material *gm = glass(0xffb347, 0.0f);
      t3_mesh *cone = t3_mesh_new(g_cone, gm); t3_release(gm);      /* the mesh keeps the one reference */
      t3_object_set_scale(cone, 0.35f, 2.0f, 0.35f); t3_object_set_position(cone, 0, 0.08f, 1.45f); t3_object_set_rotation(cone, -PI / 2, 0, 0);
      t3_object_add(piv, cone); t3_release(cone);
      if (secondary) b->wsub[3] = cone; else b->glow = cone;
      soup_free_empty(&s);
      if (!secondary) { b->glint_x = 0; b->glint_y = y0 + 0.13f; b->glint_z = z0 + 0.47f; b->glint_parent = b->body; }
      break; }
    case WP_HAMMER: {
      /* the pivot pin through two gusseted posts on the lid; the arm reaches forward to a rust head with a
       * bright striking face and an amber band; a counterweight behind the pin. The slam stops where the
       * head meets the floor, which depends on how tall the bot stands */
      float py = top + 0.12f, pz = -0.1f;
      float sin_max = (py - 0.17f) / 1.0f; if (sin_max > 0.6f) sin_max = 0.6f; if (sin_max < 0.1f) sin_max = 0.1f;
      b->hammer_max = asinf(sin_max);
      t3_object_set_position(piv, 0, py, pz);
      soup_rgb(sh, BC_STEEL);
      for (int k = -1; k <= 1; k += 2) {
        sp_cbox_at(sh, 0.07f, py - top + 0.06f, 0.16f, 0.012f, k * 0.12f, (py + top) / 2, pz);
        soup_push_at(sh, k * 0.12f, top + 0.03f, pz - 0.09f, 0, PI, 0); sp_wedge(sh, 0.06f, 0.08f, 0.1f); soup_pop(sh);   /* gussets */
      }
      soup_rgb(sh, BC_BRIGHT); sp_cyl_at(sh, 0.045f, 0.32f, 8, 0, 0, py, pz);
      soup_rgb(&s, BC_STEEL); sp_cbox_at(&s, 0.1f, 0.1f, 0.92f, 0.02f, 0, 0, 0.46f);
      soup_rgb(&s, C_RUST); sp_cbox_at(&s, 0.2f, 0.16f, 0.16f, 0.02f, 0, 0, -0.16f);
      part_mesh(piv, &s);
      soup_t sh2; soup_init_vcol(&sh2, 120);
      soup_rgb(&sh2, C_RUST); sp_cbox(&sh2, 0.4f, 0.3f, 0.36f, 0.04f);
      soup_rgb(&sh2, C_HAZARD); sp_cbox_at(&sh2, 0.408f, 0.07f, 0.368f, 0.01f, 0, 0.05f, 0);
      soup_rgb(&sh2, BC_BRIGHT); sp_cbox_at(&sh2, 0.42f, 0.025f, 0.38f, 0.008f, 0, -0.158f, 0);
      t3_object *headg = group_at(piv, 0, 0, 1.0f);
      part_mesh(headg, &sh2);
      b->wsub[si] = (t3_mesh *)headg;
      if (!secondary) { b->glint_x = 0; b->glint_y = -0.17f; b->glint_z = 0; b->glint_parent = headg; }
      break; }
    case WP_FLIPPER: {
      /* the plate over the front half of the lid, hinged at its back on two knuckles, a bright lip, a dark
       * gap line round it so it reads as a separate plate at rest, and a piston under it */
      float pw, pl, hy, hz, rest = 0, pt_ = ARMOUR_LOOK[bot_armour(b)].t;
      if (dome) { pw = L * 0.5f; pl = L * 0.55f; hy = top + pt_ + 0.012f; hz = -L * 0.06f; rest = 0.42f; }
      else if (b->build.chassis == CH_WEDGE) {
        /* on a WEDGE the plate lies down the slope from its top edge */
        float run = L * 0.55f; rest = atanf(c->hgt / run); pw = W * 0.9f; pl = sqrtf(run * run + c->hgt * c->hgt) * 0.88f; hy = top + pt_ + 0.012f; hz = -L * 0.05f;
      } else { pw = W * 0.92f; pl = L * 0.58f; hy = top + pt_ + 0.012f; hz = -L * 0.08f; }
      if (!secondary) b->flip_rest = rest;
      t3_object_set_position(piv, 0, hy, hz);
      t3_object_set_rotation(piv, rest, 0, 0);
      soup_rgb(&s, 0x8a857cu); sp_cbox_at(&s, pw, 0.045f, pl, 0.012f, 0, 0.022f, pl / 2);
      soup_rgb(&s, C_HAZARD); sp_box_at(&s, pw, 0.02f, 0.07f, 0, 0.05f, pl - 0.045f);
      soup_rgb(&s, BC_BRIGHT); sp_cyl_at(&s, 0.02f, pw * 1.02f, 6, 0, 0, 0.02f, pl);
      for (int k = -1; k <= 1; k += 2) sp_cyl_at(&s, 0.04f, 0.12f, 8, 0, k * pw * 0.3f, 0.02f, 0);
      for (int k = -1; k <= 1; k++) { soup_push_at(&s, k * pw * 0.32f, 0.045f, pl * 0.15f, -PI / 2, 0, 0); sp_bolt(&s, 0.018f, 0.01f); soup_pop(&s); }
      part_mesh(piv, &s);
      soup_push_at(sh, 0, hy, hz, rest, 0, 0);
      soup_rgb(sh, BC_HOLE); sp_box_at(sh, pw + 0.04f, 0.006f, pl + 0.04f, 0, -0.004f, pl / 2);   /* the gap line */
      soup_rgb(sh, BC_STEEL_DK); for (int k = -1; k <= 1; k += 2) sp_cbox_at(sh, 0.05f, 0.06f, 0.1f, 0.01f, k * pw * 0.3f, -0.02f, 0);
      soup_pop(sh);
      soup_t sp; soup_init_vcol(&sp, 60);
      soup_rgb(&sp, BC_BRIGHT); sp_cyl_at(&sp, 0.03f, 0.5f, 8, 1, 0, 0.25f, 0);
      soup_rgb(&sp, BC_STEEL_DK); sp_cyl_at(&sp, 0.045f, 0.18f, 8, 1, 0, 0.09f, 0);
      t3_mesh *pis = part_mesh(g, &sp); if (pis) t3_object_set_position(pis, 0, hy - 0.05f, hz + pl * 0.45f);
      b->wsub[si + 1] = pis;
      if (!secondary) { b->glint_x = 0; b->glint_y = hy + 0.06f; b->glint_z = hz + pl; b->glint_parent = b->body; }
      break; }
    case WP_DRUM: {
      /* a drum with bright end plates and six teeth between two bearing blocks, each with a bolt */
      float dy = r + 0.3f, dz = front_z(b, dy) + 0.28f;
      if (dome) dz = L / 2 + 0.25f;
      t3_object_set_position(piv, 0, dy, dz);
      soup_rgb(sh, BC_STEEL_DK);
      for (int k = -1; k <= 1; k += 2) {
        sp_cbox_at(sh, 0.1f, 0.3f, 0.32f, 0.02f, k * (W / 2 - 0.05f), dy, dz - 0.14f);
        soup_rgb(sh, BC_BRIGHT); soup_push_at(sh, k * (W / 2), dy, dz - 0.1f, 0, k * PI / 2, 0); sp_bolt(sh, 0.03f, 0.015f); soup_pop(sh); soup_rgb(sh, BC_STEEL_DK);
      }
      if (dome) sp_cbox_at(sh, W - 0.1f, 0.12f, 0.3f, 0.02f, 0, dy, L / 2 + 0.05f);
      soup_rgb(&s, 0x6f6c66u); sp_cyl(&s, 0.27f, W - 0.22f, 12, 0);
      soup_rgb(&s, BC_BRIGHT); for (int k = -1; k <= 1; k += 2) sp_cyl_at(&s, 0.3f, 0.03f, 12, 0, k * (W - 0.22f) / 2, 0, 0);
      soup_rgb(&s, 0xe6e2d8u);
      for (int t = 0; t < 6; t++) { float a = t * TAU / 6; soup_push_at(&s, 0, cosf(a) * 0.29f, sinf(a) * 0.29f, -a, 0, 0); sp_cbox(&s, W - 0.26f, 0.06f, 0.09f, 0.01f); soup_pop(&s); }
      part_mesh(piv, &s);
      if (!secondary) { b->ring_r = 0.32f; b->ring_y = dy; b->ring_z = dz; }
      if (!secondary) { b->glint_x = 0; b->glint_y = dy + 0.33f; b->glint_z = dz; b->glint_parent = b->body; }
      break; }
    case WP_HORIZ: {
      /* a heavy disc with four fat bright teeth, a hub cap with six bolts and an amber ring on top, on a
       * bearing tower */
      float dy = top + 0.11f, dz = L * 0.15f;
      t3_object_set_position(piv, 0, dy, dz);
      soup_rgb(sh, BC_STEEL_DK); sp_cbox_at(sh, 0.2f, 0.16f, 0.2f, 0.03f, 0, top + 0.03f, dz);
      t3_object *disc = group_at(piv, 0, 0, 0);
      soup_rgb(&s, 0x6f6c66u); sp_cyl(&s, 0.46f, 0.05f, 10, 1);
      soup_rgb(&s, BC_BRIGHT); sp_teeth(&s, 4, 0.42f, 0.16f, 0.07f, 0.13f, 0.35f);
      sp_cyl_at(&s, 0.17f, 0.07f, 6, 1, 0, 0.01f, 0);
      for (int k = 0; k < 4; k++) { float a = k * TAU / 4; soup_push_at(&s, sinf(a) * 0.12f, 0.045f, cosf(a) * 0.12f, -PI / 2, 0, 0); sp_bolt(&s, 0.018f, 0.01f); soup_pop(&s); }
      soup_rgb(&s, C_HAZARD);
      for (int k = 0; k < 10; k++) {
        float a0 = k * TAU / 10, a1 = (k + 1) * TAU / 10, ri = 0.27f, ro = 0.36f, y = 0.027f, ctr[3] = { 0, -1, 0 };
        float p0[3] = { sinf(a0) * ri, y, cosf(a0) * ri }, p1[3] = { sinf(a1) * ri, y, cosf(a1) * ri }, p2[3] = { sinf(a1) * ro, y, cosf(a1) * ro }, p3[3] = { sinf(a0) * ro, y, cosf(a0) * ro };
        soup_quad_out(&s, p0, p1, p2, p3, ctr);
      }
      part_mesh(disc, &s);
      b->wsub[si] = (t3_mesh *)disc;
      if (!secondary) { b->ring_r = 0.56f; b->ring_y = dy; b->ring_z = dz; }
      if (!secondary) { b->glint_x = 0.5f; b->glint_y = dy + 0.03f; b->glint_z = dz; b->glint_parent = b->body; }
      break; }
    case WP_CRUSHER: {
      /* a fixed lower jaw with three bright teeth, an upper jaw of two arms and a bar with three teeth down,
       * a pivot pin and a hydraulic ram from the lid to the upper arm */
      float jz = dome ? L * 0.3f + 0.4f : front_z(b, r + 0.12f) + 0.38f;
      t3_object_set_position(piv, 0, top + 0.02f, L * 0.1f);
      soup_rgb(sh, BC_STEEL); sp_cbox_at(sh, 0.5f, 0.08f, jz - L * 0.25f + 0.1f, 0.015f, 0, r + 0.1f, (jz + L * 0.25f) / 2 - 0.05f);
      soup_rgb(sh, BC_BRIGHT);
      for (int k = -1; k <= 1; k++) { soup_push_at(sh, k * 0.15f, r + 0.19f, jz + 0.02f, 0, 0, 0); sp_cyl2(sh, 0.05f, 0.0f, 0.1f, 6, 1); soup_pop(sh); }
      sp_cyl_at(sh, 0.04f, 0.5f, 8, 0, 0, top + 0.02f, L * 0.1f);
      soup_rgb(&s, BC_STEEL);
      for (int k = -1; k <= 1; k += 2) sp_cbox_at(&s, 0.1f, 0.1f, 0.92f, 0.015f, k * 0.18f, 0.05f, 0.5f);
      soup_rgb(&s, C_RUST); sp_cbox_at(&s, 0.5f, 0.12f, 0.16f, 0.02f, 0, 0, 0.95f);
      soup_rgb(&s, BC_BRIGHT);
      for (int k = -1; k <= 1; k++) { soup_push_at(&s, k * 0.15f, -0.11f, 0.95f, PI, 0, 0); sp_cyl2(&s, 0.05f, 0.0f, 0.1f, 6, 1); soup_pop(&s); }
      part_mesh(piv, &s);
      soup_t sr; soup_init_vcol(&sr, 60); soup_rgb(&sr, BC_BRIGHT); sp_cyl_at(&sr, 0.03f, 0.3f, 8, 1, 0, 0.15f, 0);
      soup_rgb(&sr, BC_STEEL_DK); sp_cyl_at(&sr, 0.045f, 0.12f, 8, 1, 0, 0.06f, 0);
      t3_mesh *ram = part_mesh(g, &sr); if (ram) t3_object_set_position(ram, 0, top + 0.01f, L * 0.1f + 0.27f);
      b->wsub[si + 1] = ram;
      t3_object_set_rotation(piv, -0.5f, 0, 0);
      if (!secondary) { b->glint_x = 0; b->glint_y = top + 0.1f; b->glint_z = L * 0.1f + 0.95f; b->glint_parent = b->body; }
      break; }
    default: soup_free_empty(&s); break;
  }
  #undef BRACKET
}

/* -- bot assembly ---------------------------------------------------- */
/* the chassis underside height each drive stands the body on */
static float drive_ride(int drive) {
  switch (drive) { case DR_TWO: return 0.12f; case DR_TREAD: case DR_LEGTRACK: return 0.12f; case DR_HOVER: return 0.22f; case DR_WALKER: return 0.3f; default: return 0.1f; }
}

/* textures every bot shares: the oriented blob shadow, the floor danger ring, the spinner blur atlas, the
 * ready glint star and the hit decal atlas (graphics 4.3, 4.8, 5.1) */
static t3_material *mat_decal, *mat_glint;
static float smooth01(float e0, float e1, float x) { float t = (x - e0) / (e1 - e0); if (t < 0) t = 0; if (t > 1) t = 1; return t * t * (3 - 2 * t); }
static void make_bot_textures(void) {
  canvas_t c = cv_new(32, 32);
  for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
    /* a soft rounded square with a darker core: the cast shadow and the contact darkening in one */
    float u = fabsf((x + 0.5f) / 16 - 1), v = fabsf((y + 0.5f) / 16 - 1);
    float dx = u - 0.55f, dy = v - 0.55f, d = sqrtf((dx > 0 ? dx * dx : 0) + (dy > 0 ? dy * dy : 0));
    float a = 1 - smooth01(0.05f, 0.42f, d), core = 1 - smooth01(0.0f, 0.5f, fmaxf(u, v));
    uint8_t *p = c.px + ((size_t)y * 32 + x) * 4; p[0] = p[1] = p[2] = 0; p[3] = (uint8_t)(255 * (0.72f * a + 0.28f * core));
  }
  tex_blob = cv_texture(&c, 0, 0);
  c = cv_new(64, 64);
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
    float r = sqrtf((x + 0.5f - 32) * (x + 0.5f - 32) + (y + 0.5f - 32) * (y + 0.5f - 32)) / 32;
    float a = (1 - smooth01(0.0f, 0.05f, fabsf(r - 0.86f))) + (1 - smooth01(0.0f, 0.025f, fabsf(r - 0.97f))) * 0.33f;
    uint8_t *p = c.px + ((size_t)y * 64 + x) * 4; p[0] = 0xec; p[1] = 0xe3; p[2] = 0xd3; p[3] = (uint8_t)(255 * (a > 1 ? 1 : a));
  }
  tex_ring = cv_texture(&c, 1, 0);
  c = cv_new(128, 64);
  for (int y = 0; y < 64; y++) for (int x = 0; x < 128; x++) {
    int half = x >= 64; float lx = (x % 64) + 0.5f - 32, ly = y + 0.5f - 32, r = sqrtf(lx * lx + ly * ly) / 32, ang = atan2f(ly, lx);
    /* a blurred disc: a dark hub, the steel body, the amber ring smeared round, the teeth a lighter band at
     * the rim; the right half carries three ghost teeth in that band */
    float outer = 1 - smooth01(0.93f, 1.0f, r), teeth = smooth01(0.78f, 0.84f, r) * outer;
    float a = 0.8f * (1 - smooth01(0.8f, 0.84f, r)) + teeth * 0.7f;
    uint32_t col = 0x8a857cu;
    if (r < 0.22f) col = 0x4a4338u; else if (r > 0.55f && r < 0.7f) col = mix_rgb(0x8a857cu, C_HAZARD, 0.6f); else if (r >= 0.8f) col = 0xe6e2d8u;
    if (half && r >= 0.78f) { float ghost = 0.5f + 0.5f * cosf(ang * 4); a = teeth * (0.25f + 0.75f * ghost * ghost * ghost); }
    a *= outer;
    uint8_t *p = c.px + ((size_t)y * 128 + x) * 4; p[0] = (uint8_t)(col >> 16); p[1] = (uint8_t)(col >> 8); p[2] = (uint8_t)col; p[3] = (uint8_t)(255 * (a > 1 ? 1 : a));
  }
  tex_blur = cv_texture(&c, 1, 0);
  c = cv_new(32, 32);
  for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
    float u = (x + 0.5f) / 16 - 1, v = (y + 0.5f) / 16 - 1;
    float a = fmaxf(1 - smooth01(0, 0.12f, fabsf(u)) , 0) * (1 - fabsf(v)) + fmaxf(1 - smooth01(0, 0.12f, fabsf(v)), 0) * (1 - fabsf(u));
    a += 1 - smooth01(0.0f, 0.3f, sqrtf(u * u + v * v));
    uint8_t *p = c.px + ((size_t)y * 32 + x) * 4; p[0] = 0xff; p[1] = 0xf8; p[2] = 0xec; p[3] = (uint8_t)(255 * (a > 1 ? 1 : a));
  }
  tex_glint = cv_texture(&c, 1, 0);
  mat_glint = t3_mesh_basic_material_new(0xffffff); t3_material_set_map(mat_glint, tex_glint);
  mat_glint->transparent = true; mat_glint->opacity = 0.9f; mat_glint->depth_write = false; mat_glint->side = T3_DOUBLE_SIDE;
  /* the decal atlas: scorch, three bone scratches, a bolt hole, a crack */
  c = cv_new(128, 128);
  for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) {
    float u = (x + 0.5f) / 32 - 1, v = (y + 0.5f) / 32 - 1, r = sqrtf(u * u + v * v) + 0.08f * sinf(atan2f(v, u) * 5);
    float a = 1 - smooth01(0.2f, 0.95f, r);
    uint8_t *p = c.px + ((size_t)y * 128 + x) * 4; p[0] = 0x1d; p[1] = 0x19; p[2] = 0x15; p[3] = (uint8_t)(235 * a);
  }
  for (int k = 0; k < 3; k++) for (int t = 0; t < 52; t++) {
    int x = 64 + 6 + t, y = 14 + k * 16 + (t * (3 + k)) / 13;
    for (int w = -1; w <= 1; w++) cv_px(&c, x, y + w, w ? C_ASH : C_BONE, 255);
  }
  for (int y = 64; y < 128; y++) for (int x = 0; x < 64; x++) {
    float u = (x + 0.5f) / 32 - 1, v = (y - 64 + 0.5f) / 32 - 1, r = fmaxf(fabsf(u) * 0.87f + fabsf(v) * 0.5f, fabsf(v));
    uint8_t *p = c.px + ((size_t)y * 128 + x) * 4;
    if (r < 0.45f) { p[0] = 0x0e; p[1] = 0x0c; p[2] = 0x0a; p[3] = 255; } else if (r < 0.62f) { p[0] = 0xec; p[1] = 0xe3; p[2] = 0xd3; p[3] = 220; }
  }
  { int x = 70, y = 70; for (int k = 0; k < 9; k++) { int nx = x + 6, ny = y + (k & 1 ? 9 : -2) + 3; if (ny > 124) ny = 124; for (int t = 0; t <= 8; t++) { int px = x + (nx - x) * t / 8, py = y + (ny - y) * t / 8; cv_px(&c, px, py, C_BONE, 255); cv_px(&c, px + 1, py, C_BONE, 200); } x = nx; y = ny; } }
  tex_decal = cv_texture(&c, 1, 0);
  mat_decal = t3_mesh_standard_material_new(0xffffff); t3_material_set_map(mat_decal, tex_decal);
  mat_decal->roughness = 0.8f; mat_decal->metalness = 0; mat_decal->transparent = true; mat_decal->opacity = 0.9f; mat_decal->depth_write = false;
}

static t3_instanced_mesh *bot_blobs;
static void blob_hide(int slot) {
  if (!bot_blobs || slot < 0 || slot >= MAX_BOTS) return;
  t3_mat4 z; t3_mat4_compose(&z, (t3_vec3){ 0, -9, 0 }, t3_quat_identity(), (t3_vec3){ 0.0001f, 0.0001f, 0.0001f });
  t3_instanced_mesh_set_matrix_at(bot_blobs, slot, &z); bot_blobs->instance_matrix->version++;
}
static void bot_alloc(wbot_t *b, int slot) {
  if (b->root) return;
  b->root = t3_group_new();
  b->body = group_at(b->root, 0, 0, 0);
  b->drive_g = group_at(b->body, 0, 0, 0);     /* the drive rides in the body: one soup carries both (bot_rebuild) */
  b->weapon_g = group_at(b->body, 0, 0, 0);
  b->weapon2_g = group_at(b->body, 0, 0, 0);
  /* the floor danger ring under a spinner (graphics 4.8): a textured quad, the two rings in the canvas */
  t3_material *rm = glass_map(tex_ring, 0); b->ring = t3_mesh_new(quad_geo(0, 0, 1, 1), rm); t3_release(rm);
  t3_object_add(&S->base, b->ring); t3_release(b->ring); set_vis(b->ring, 0);
  t3_material *lm = t3_mesh_basic_material_new(C_LIVE);
  b->led = t3_mesh_new(g_box, lm); t3_object_set_scale(b->led, 0.1f, 0.02f, 0.03f); t3_object_add(b->root, b->led); t3_release(b->led); t3_release(lm); set_vis(b->led, 0);
  /* every bot's blob shadow is one instance of one mesh: five shadows, one draw */
  if (!bot_blobs) {
    t3_material *bm = glass_map(tex_blob, 0.5f);
    bot_blobs = t3_instanced_mesh_new(quad_geo(0, 0, 1, 1), bm, MAX_BOTS); t3_release(bm);
    bot_blobs->mesh.base.frustum_culled = false;
    t3_object_add(&S->base, bot_blobs); t3_release(bot_blobs);
    for (int i = 0; i < MAX_BOTS; i++) blob_hide(i);
  }
  t3_material *srm = t3_mesh_basic_material_new(C_BONE);
  b->seat_ring = t3_mesh_new(g_ring, srm); rot(b->seat_ring, -PI / 2, 0, 0); t3_object_add(&S->base, b->seat_ring); t3_release(b->seat_ring); t3_release(srm);
  set_vis(b->seat_ring, 0);
  t3_object_add(&S->base, b->root);
  b->root->visible = false;
  (void)slot;
}

/* the hanging plate of a panel (state 3): its own mesh on a hinge at the plate's bottom edge (the lid's
 * rear edge), so it can swing; built with the bot, hidden until a panel hangs */
static void build_hanging(wbot_t *b, int p) {
  wplate_t *pl = &b->pl[p];
  b->hang[p] = 0; b->hang_m[p] = 0;
  if (!pl->on || bot_armour(b) == AR_WEAR) return;
  float hy = pl->hinge_top ? pl->h / 2 : -pl->h / 2;
  float hp[3] = { 0, hy, 0 }, wpos[3]; xf_pt(&pl->xf, hp, wpos);
  t3_object *hinge = group_at(b->body, wpos[0], wpos[1], wpos[2]);
  t3_object_set_quaternion(hinge, t3_quat_multiply(t3_quat_from_axis_angle((t3_vec3){ 0, 1, 0 }, pl->ry), t3_quat_from_axis_angle((t3_vec3){ 1, 0, 0 }, pl->rx)));
  soup_t s; soup_init_vcol(&s, 260);
  soup_push_at(&s, 0, -hy, 0, 0, 0, 0);
  paint_t pt = bot_paint(b);
  emit_plate(&s, b, p, pl->w, pl->h, pl->t, 3, plate_colour(b, p == PN_TOP || p == PN_MOUNT ? pt.lid : pt.plate), pl->hinge_top);
  soup_pop(&s);
  const armour_look_t *al = &ARMOUR_LOOK[bot_armour(b)];
  t3_mesh *m = soup_mesh(hinge, &s, vmat(al->rough, al->metal));
  set_vis(hinge, 0);
  b->hang[p] = hinge; b->hang_m[p] = m;
}

static int plate_capacity(wbot_t *b) {
  uint8_t keep[PN_COUNT]; memcpy(keep, b->pstate, sizeof keep);
  int most = 0;
  for (int st = 0; st <= 4; st++) {
    for (int p = 0; p < PN_COUNT; p++) b->pstate[p] = (uint8_t)st;
    soup_t *s = plate_soup(); emit_plates(s, b);
    if (s->n_ > most) most = s->n_;
  }
  /* a mix of states can exceed any single one by the internals of the gone panels on top of the most bolted */
  memcpy(b->pstate, keep, sizeof keep);
  return most + 600;
}

#ifdef RTR_TRACE
/* the triangles a bot draws with every part shown (hidden spares and damage meshes left out) and its object count */
static void bot_count(const t3_object *o, int *tris, int *objs) {
  (*objs)++;
  if (!o->visible) return;
  if (o->type == T3_MESH) { const t3_geometry *g = ((const t3_mesh *)o)->geometry; if (g && g->attributes[T3_ATTR_POSITION]) { int t = (g->draw_count >= 0 ? g->draw_count : g->attributes[T3_ATTR_POSITION]->count) / 3; *tris += t;
} }
  for (int i = 0; i < o->child_count; i++) bot_count(o->children[i], tris, objs);
}
#endif

static void bot_rebuild(wbot_t *b) {
  clear_group(b->drive_g); clear_group(b->body);
  if (b->base.p) { free(b->base.p); free(b->base.n); free(b->base.uv); free(b->base.c); memset(&b->base, 0, sizeof b->base); }
  b->drive_g = group_at(b->body, 0, 0, 0);
  /* the tread material holds its own reference to the stripe map; ours goes with the old drive */
  if (b->tread_tex) { t3_release(b->tread_tex); b->tread_tex = 0; }
  b->hammer_max = 0.5f; b->hammer_last = -0.6f; b->panels_hidden = 0;
  b->weapon_g = group_at(b->body, 0, 0, 0);
  b->weapon2_g = group_at(b->body, 0, 0, 0);
  memset(b->wsub, 0, sizeof b->wsub); memset(b->wsub2, 0, sizeof b->wsub2); b->glow = 0; b->wpivot = b->w2pivot = 0; b->ring_r = 0; b->blur = 0; b->glint = 0; b->beacon = 0; b->tape = 0;
  memset(b->hang, 0, sizeof b->hang); memset(b->hang_m, 0, sizeof b->hang_m); memset(b->hang_kick, 0, sizeof b->hang_kick);
  memset(b->pstate, 0, sizeof b->pstate); memset(b->digits, 0, sizeof b->digits);
  b->ndec = 0; b->dec_next = 0; b->dec_dirty = 0; b->burn_acc = 0; b->glint_t = 0; b->glint_parent = b->body; b->glint_x = b->glint_y = b->glint_z = 0;
  b->wedge_dip = 0; b->flip_rest = 0; b->rock = 0; b->lean_x = b->lean_z = 0; b->hinge_y = b->hinge_z = 0;
  const chassis_def_t *c = &CHASSIS[b->build.chassis];
  b->ride_y = drive_ride(b->build.drive);
  b->len = c->len * (b->is_ops ? 1.4f : 1); b->wid = c->wid;
  soup_t sh; soup_init_vcol(&sh, 3200);
  build_chassis(b, &sh);
  build_drive(b, c, &sh, &sh);                 /* the drive's static parts go in the body soup too: one draw fewer a bot */
  if (b->build.drive != DR_MELTY) {
    build_weapon(b, 0, &sh);
    if (b->build.weapon2 != WP_NONE) build_weapon(b, 1, &sh);
  }
  b->base = sh;                                /* kept: every plate rewrite copies it in first */
  /* the body and its plates: one soup, sized for the body and the worst mix of plate states, rewritten in place */
  const armour_look_t *al = &ARMOUR_LOOK[bot_armour(b)];
  b->pl_cap = b->base.n_ + plate_capacity(b);
  t3_geometry *pg = dyn_geometry(b->pl_cap, 1);
  b->plates = t3_mesh_new(pg, vmat(al->rough, al->metal)); t3_release(pg);
  t3_object_add(b->body, b->plates); t3_release(b->plates);
  plates_commit(b);
  pg->bounding_sphere.center = (t3_vec3){ 0, b->ride_y + c->hgt / 2, 0 }; pg->bounding_sphere.radius = b->len * 0.8f + 0.3f; pg->has_bounding_sphere = true;
  for (int p = 0; p < PN_COUNT; p++) build_hanging(b, p);
  /* the decals: one soup of up to eight quads on the plates */
  t3_geometry *dg = dyn_geometry(NDECALS * 6 + 6, 0);
  b->decals = t3_mesh_new(dg, mat_decal); t3_release(dg);
  dg->bounding_sphere = pg->bounding_sphere; dg->has_bounding_sphere = true;
  t3_object_add(b->body, b->decals); t3_release(b->decals); set_vis(b->decals, 0);
  /* seat digits on both sides and the lid (OPS reads OPS), outside the plates */
  int digit = b->is_ops ? 4 : (b->seat & 3);
  float L = b->len, W = c->wid, H = c->hgt, r = b->ride_y, t = al->t;
  float ds_ = H * 0.7f; if (ds_ < 0.18f) ds_ = 0.18f; if (ds_ > 0.3f) ds_ = 0.3f;
  if (c->shape == SH_CIRCLE) {
    /* on the dome: smaller, on the flank, swung 50 degrees back from the side plates */
    float dd = H * 0.45f, R = L * 0.485f, y = r + H * 0.62f;
    for (int k = 0; k < 2; k++) {
      t3_object *az = group_at(b->body, 0, 0, 0); t3_object_set_rotation(az, 0, (k ? 1 : -1) * (PI / 2 + 0.87f), 0);
      t3_mesh *pl = t3_mesh_new(g_plane, mat_digit[digit]); t3_object_set_scale(pl, dd, dd, 1); t3_object_set_position(pl, 0, y, R); t3_object_set_rotation(pl, -0.3f, 0, 0);
      t3_object_add(az, pl); t3_release(pl); b->digits[k] = pl;
    }
  } else if (!b->panels_hidden) {
    for (int k = 0; k < 2; k++) {
      t3_mesh *n = t3_mesh_new(g_plane, mat_digit[digit]); t3_object_set_scale(n, ds_, ds_, 1);
      t3_object_set_position(n, (k ? 1 : -1) * (W / 2 + t + 0.016f), r + H / 2 + 0.01f, -L * 0.22f); rot(n, 0, (k ? 1 : -1) * PI / 2, 0);
      t3_object_add(b->body, n); t3_release(n); b->digits[k] = n;
    }
    float td = ds_; if (td > b->deck_w * 0.36f) td = b->deck_w * 0.36f;
    t3_mesh *n = t3_mesh_new(g_plane, mat_digit[digit]); t3_object_set_scale(n, td, td, 1);
    t3_object_set_position(n, b->deck_w * 0.27f, b->deck_y + t + 0.014f, -b->deck_l * 0.3f); rot(n, -PI / 2, 0, PI);
    t3_object_add(b->body, n); t3_release(n); b->digits[2] = n;
  }
  if (b->build.decal == DC_TAPE_BAND || b->is_ops) {
    t3_mesh *tp = t3_mesh_new(g_plane, mat_tape);
    if (b->is_ops) { t3_object_set_scale(tp, W + 0.1f, 0.2f, 1); t3_object_set_position(tp, 0, r + 0.25f, L / 2 + t + 0.02f); }
    else { t3_object_set_scale(tp, b->deck_w * 0.8f, 0.12f, 1); rot(tp, -PI / 2, 0, 0); t3_object_set_position(tp, 0, b->deck_y + t + 0.012f, -b->deck_l * 0.42f); }
    t3_object_add(b->body, tp); t3_release(tp); b->tape = tp;
  }
  if (b->is_ops) {
    /* the amber beacon on its mast, blinking at 2 Hz while OPS is out */
    b->beacon = t3_mesh_new(g_sphere, mat_ex(C_HAZARD, 0.4f, 0, C_HAZARD, 0.2f, 0));
    t3_object_set_scale(b->beacon, 0.08f, 0.08f, 0.08f); t3_object_set_position(b->beacon, W * 0.3f, r + H + 0.34f, -L * 0.35f);
    t3_object_add(b->body, b->beacon); t3_release(b->beacon);
  }
  /* the READY glint at the weapon's working edge */
  if (b->build.weapon != WP_NONE || b->build.drive == DR_MELTY) {
    t3_object *gp = b->glint_parent ? b->glint_parent : b->body;
    t3_object *gg = group_at(gp, b->glint_x, b->glint_y, b->glint_z);
    for (int k = 0; k < 2; k++) { t3_mesh *q = t3_mesh_new(quad_geo(0, 0, 1, 1), mat_glint); t3_object_set_scale(q, 0.22f, 0.22f, 1); t3_object_set_rotation(q, 0, k * PI / 2, 0); t3_object_add(gg, q); t3_release(q); }
    b->glint = (t3_mesh *)gg; set_vis(gg, 0);
  }
  /* the spinner blur (graphics 4.8): HORIZONTAL a flat disc quad, SAW one in the disc's plane, DRUM a
   * glass cylinder, MELTY a flat ring round the body */
  int wp = b->build.weapon;
  if (b->build.drive == DR_MELTY) { b->ring_r = (L > W ? L : W) * 0.5f + 0.1f; b->ring_y = r + H / 2; b->ring_z = 0; }
  if (b->build.drive == DR_MELTY || wp == WP_HORIZ || wp == WP_SAW) {
    int melty = b->build.drive == DR_MELTY;
    t3_material *bm = glass_map(melty ? tex_ring : tex_blur, 0);
    b->blur = t3_mesh_new(melty ? quad_geo(0, 0, 1, 1) : quad_geo(0, 0, 0.5f, 1), bm); t3_release(bm);
    float d = b->ring_r * 2.05f;
    t3_object_set_scale(b->blur, d, d, 1);
    if (b->build.drive == DR_MELTY) { t3_object_set_position(b->blur, 0, b->ring_y, 0); rot(b->blur, -PI / 2, 0, 0); t3_object_add(b->root, b->blur); }
    else if (wp == WP_HORIZ) { t3_object_set_position(b->blur, 0, 0.0f, 0); rot(b->blur, -PI / 2, 0, 0); t3_object_add(b->wpivot, b->blur); }
    else { t3_object_set_position(b->blur, 0, 0.05f, 0.7f); rot(b->blur, 0, PI / 2, 0); t3_object_add(b->wpivot, b->blur); }
    t3_release(b->blur); set_vis(b->blur, 0);
  } else if (wp == WP_DRUM) {
    t3_material *gm = glass(0xd8d3ca, 0);
    b->blur = t3_mesh_new(g_cyl12, gm); t3_release(gm);
    t3_object_set_scale(b->blur, 0.34f, W - 0.18f, 0.34f); rot(b->blur, 0, 0, PI / 2);
    t3_object_add(b->wpivot, b->blur); t3_release(b->blur); set_vis(b->blur, 0);
  }
  if (b->ring_r > 0) { float d = (b->ring_r + 0.1f) * 2; t3_object_set_scale(b->ring, d, d, 1); }
  set_vis(b->ring, 0);
  float rr = (b->len > c->wid ? b->len : c->wid) * 0.7f;
  if (b->radius < rr) b->radius = rr;
  b->seat_ring->materials[0]->color = t3_color_hex(b->is_ops ? C_HAZARD : b->seat_colour);
  t3_object_set_scale(b->seat_ring, b->radius * 1.15f, b->radius * 1.15f, 1);
  set_vis(b->led, b->build.drive == DR_MELTY);
  t3_object_set_position(b->led, 0, b->top_y + al->t + 0.015f, b->len * 0.3f);
  b->built = 1;
#ifdef RTR_TRACE
  { int tris = 0, objs = 0; bot_count(b->root, &tris, &objs);
    char m[96]; snprintf(m, sizeof m, "[bot] slot=%d ch=%d dr=%d wp=%d ar=%d tris=%d objs=%d", (int)(b - bots), b->build.chassis, b->build.drive, b->build.weapon, b->build.armour, tris, objs); wc_log(m, (unsigned)strlen(m)); }
#endif
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
  if (!on) blob_hide(slot);
  if (!on) { set_vis(bots[slot].seat_ring, 0); set_vis(bots[slot].ring, 0); }
}

/* the root: heading, then the pitch and roll about the bot's own axes, then the inverted flip (DEEP-plan
 * 7.8). On the floor the bot pitches about its trailing bottom edge and rolls about its low side, so a
 * ridden victim's tail stays down and its nose climbs the plate; in the air it turns about its middle */
static void place_root(wbot_t *b, float x, float y, float z, float h, int inverted, float pitch, float roll, int air) {
  b->x = x; b->y = y; b->z = z; b->h = h; b->inverted = inverted;
  float raise = inverted ? b->top_y + 0.02f : 0;
  t3_quat qh = t3_quat_from_axis_angle((t3_vec3){ 0, 1, 0 }, h);
  t3_quat q = inverted ? t3_quat_multiply(qh, t3_quat_from_axis_angle((t3_vec3){ 0, 0, 1 }, PI)) : qh;
  t3_vec3 off = { 0, 0, 0 };
  if (fabsf(pitch) > 0.0005f || fabsf(roll) > 0.0005f) {
    t3_quat qpr = t3_quat_multiply(t3_quat_from_axis_angle((t3_vec3){ 1, 0, 0 }, -pitch), t3_quat_from_axis_angle((t3_vec3){ 0, 0, 1 }, -roll));
    float cy = b->ride_y + CHASSIS[b->build.chassis].hgt / 2;
    t3_vec3 P = air ? (t3_vec3){ 0, (inverted ? -cy : cy), 0 }
                    : (t3_vec3){ roll > 0 ? b->wid / 2 : -b->wid / 2, -raise, pitch > 0 ? -b->len / 2 : b->len / 2 };
    t3_vec3 rp = t3_vec3_apply_quat(P, qpr), d = { P.x - rp.x, P.y - rp.y, P.z - rp.z };
    off = t3_vec3_apply_quat(d, qh);
    q = t3_quat_multiply(t3_quat_multiply(qh, qpr), inverted ? t3_quat_from_axis_angle((t3_vec3){ 0, 0, 1 }, PI) : t3_quat_identity());
  }
  t3_object_set_position(b->root, x + off.x, y + raise + off.y, z + off.z);
  t3_object_set_quaternion(b->root, q);
  float fy = attached ? arena_floor_y(attached, x, z) : 0;
  t3_object_set_position(b->seat_ring, x, 0.016f + fy, z);
}
/* the oriented blob shadow: a soft rounded rectangle turned with the heading and cast a little away from
 * the sun; while airborne it drifts with the flight so the player sees where it lands (graphics 4.9, 5.1) */
static void place_blob(wbot_t *b, float x, float y, float z, float h, float vx, float vz) {
  float fy = attached ? arena_floor_y(attached, x, z) : 0, up = y - fy; if (up < 0) up = 0;
  float k = 1 - up / 3; if (k < 0.2f) k = 0.2f;
  float bx = x + 0.1f * -0.83f + vx * (up / 7), bz = z + 0.1f * -0.55f + vz * (up / 7);
  int slot = (int)(b - bots);
  float bw = (b->build.drive == DR_HOVER ? b->radius * 1.6f : b->wid * 1.3f + 0.2f), bl = (b->build.drive == DR_HOVER ? b->radius * 1.6f : b->len * 1.3f + 0.1f);
  /* the fade with height is in the size: the instances share one opacity */
  float sk = (0.6f + 0.4f * k) * (0.5f + 0.5f * k);
  t3_mat4 m; t3_mat4_compose(&m, (t3_vec3){ bx, 0.012f + fy, bz }, t3_quat_multiply(t3_quat_from_axis_angle((t3_vec3){ 0, 1, 0 }, h), t3_quat_from_axis_angle((t3_vec3){ 1, 0, 0 }, -PI / 2)), (t3_vec3){ bw * sk, bl * sk, 1 });
  t3_instanced_mesh_set_matrix_at(bot_blobs, slot, &m); bot_blobs->instance_matrix->version++;
}

void world_bot_pose(int slot, float x, float y, float z, float heading, int inverted) {
  if (slot < 0 || slot >= MAX_BOTS) return;
  wbot_t *b = &bots[slot];
  bot_alloc(b, slot);
  b->posed = 1;
  place_root(b, x, y, z, heading, inverted, 0, 0, 0);
  place_blob(b, x, y, z, heading, 0, 0);
}

int world_bot_pos(int slot, float *x, float *y, float *z) {
  if (slot < 0 || slot >= MAX_BOTS || !bots[slot].root || !bots[slot].visible) return 0;
  if (x) *x = bots[slot].x; if (y) *y = bots[slot].y; if (z) *z = bots[slot].z;
  return 1;
}

/* -- poses: READY, TELL, STRIKE, RECOVER (graphics 3.4) ---------------- */
static void glint_fire(wbot_t *b) { if (b->glint) { b->glint_t = 3; set_vis(b->glint, 1); } }

/* the hammer's angle about x for a phase, from the arm pointing forward (0) to the slam (hammer_max below
 * horizontal); windup raises it to 10 degrees past straight up (DEEP-plan 7.6, physics 3.7) */
#define HAMMER_REST (-0.6f)
#define HAMMER_UP   (-1.745f)
static float hammer_angle(wbot_t *b, const bot_t *bt, int active, int cooldown, float *stretch) {
  float hm = b->hammer_max > 0 ? b->hammer_max : 0.5f;
  int A = WEAPONS[WP_HAMMER].active > 0 ? WEAPONS[WP_HAMMER].active : 4, W = WEAPONS[WP_HAMMER].windup > 0 ? WEAPONS[WP_HAMMER].windup : 15, C = WEAPONS[WP_HAMMER].cooldown > 0 ? WEAPONS[WP_HAMMER].cooldown : 72;
  *stretch = 1;
  if (bt->windup > 0) { float t = (float)(W - bt->windup) / W; return HAMMER_REST + (HAMMER_UP - HAMMER_REST) * t * (2 - t); }
  if (active > 0) {
    float p = (float)(A - active + 1) / A; if (p > 1) p = 1;
    if (active == A) *stretch = 1.15f;                       /* squash and stretch on the first frame */
    b->hammer_last = HAMMER_UP + p * p * (hm - HAMMER_UP);
    return b->hammer_last;
  }
  if (cooldown > 0) {
    int t = C - cooldown;
    if (t < 2) return b->hammer_last;                       /* the head holds 2 frames where it landed */
    float u = (float)(t - 2) / (C - 2);
    float a = b->hammer_last + (HAMMER_REST - b->hammer_last) * (1 - (1 - u) * (1 - u));
    if (t >= 18 && t < 30) a -= 0.05f * sinf(PI * (t - 18) / 12.0f);   /* the overshoot */
    return a;
  }
  return HAMMER_REST + 0.02f * sinf(world_t * TAU);         /* READY: the arm breathes */
}

static void pose_weapon(wbot_t *b, const bot_t *bt, int secondary, float dt) {
  int wp = secondary ? b->build.weapon2 : b->build.weapon;
  t3_object *piv = secondary ? b->w2pivot : b->wpivot;
  if (!piv) return;
  int si = secondary ? 2 : 0;
  float spin = secondary ? bt->spin2 : bt->spin;
  int active = secondary ? bt->active2 : bt->active, cooldown = secondary ? bt->cooldown2 : bt->cooldown;
  int wb = (bt->in.buttons & IN_A) || bt->in.rt > TRIGGER_ON;
  /* READY: a glint at the working edge the frame the cooldown runs out, or a spinner crosses the hit line */
  if (!secondary) {
    if (b->prev_cool[0] > 0 && cooldown == 0 && bt->alive) glint_fire(b);
    if ((wp == WP_HORIZ || wp == WP_DRUM || wp == WP_SAW) && bt->alive) { static float last[MAX_BOTS]; int i = (int)(b - bots); if (last[i] < SPINNER_HIT_MIN && spin >= SPINNER_HIT_MIN) glint_fire(b); last[i] = spin; }
    b->prev_cool[0] = (int16_t)cooldown;
  }
  switch (wp) {
    case WP_HORIZ: {
      float *vis = secondary ? &b->saw_vis : &b->spin_vis;
      *vis += (spin * 40.0f + (bt->alive ? 0.5f : 0)) * dt;
      t3_object *disc = (t3_object *)b->wsub[si];
      if (disc) { t3_object_set_rotation(disc, 0, *vis, 0); set_vis(disc, spin < 0.65f); }
      if (!secondary && b->blur) {
        /* the blur crossfades in from 0.3, the real teeth go at 0.65, the ghost teeth from 0.7; the blur
         * thins as the spin's energy is spent */
        float e = sim_spin_energy(bt), a = spin < 0.3f ? 0 : spin < 0.7f ? 0.3f * spin + 0.3f * (spin - 0.3f) / 0.4f : 0.62f + 0.3f * e;
        b->blur->materials[0]->opacity = a > 0.95f ? 0.95f : a;
        set_vis(b->blur, a > 0.01f);
        t3_geometry *want = spin > 0.7f ? quad_geo(0.5f, 0, 1, 1) : quad_geo(0, 0, 0.5f, 1);
        if (b->blur->geometry != want) { t3_retain(want); t3_release(b->blur->geometry); b->blur->geometry = want; }
        t3_object_set_rotation(b->blur, -PI / 2, 0, -*vis * 0.1f);
        t3_object_set_position(b->blur, 0, 0.04f, 0);
      }
      break; }
    case WP_SAW: {
      float *vis = secondary ? &b->saw_vis : &b->spin_vis;
      float sp = bt->saw_on ? spin : 0;
      *vis += (sp * 50.0f + (bt->alive ? 2.0f : 0)) * dt;    /* the motor is live: it idles */
      t3_object *disc = (t3_object *)b->wsub[si];
      if (disc) { t3_object_set_rotation(disc, *vis, 0, 0); set_vis(disc, sp < 0.65f); }
      if (!secondary && b->blur) {
        float a = sp < 0.3f ? 0 : sp < 0.7f ? 0.6f * (sp - 0.3f) / 0.4f : 0.85f;
        b->blur->materials[0]->opacity = a; set_vis(b->blur, a > 0.01f);
        t3_geometry *want = sp > 0.7f ? quad_geo(0.5f, 0, 1, 1) : quad_geo(0, 0, 0.5f, 1);
        if (b->blur->geometry != want) { t3_retain(want); t3_release(b->blur->geometry); b->blur->geometry = want; }
        t3_object_set_rotation(b->blur, *vis * 0.1f, PI / 2, 0);
      }
      break; }
    case WP_DRUM: {
      b->spin_vis += (spin * 40.0f + (bt->alive ? 1.0f : 0)) * dt;
      t3_object_set_rotation(piv, b->spin_vis, 0, 0);
      if (!secondary && b->blur) { float a = spin < 0.3f ? 0 : 0.2f + 0.35f * spin; b->blur->materials[0]->opacity = a; set_vis(b->blur, a > 0.01f); }
      break; }
    case WP_SPIKE: {
      /* the rod goes out over active frames 1 to 5 and comes back over 6 to 9, as the sim extends it */
      t3_object *rod = (t3_object *)b->wsub[si];
      int A = WEAPONS[WP_SPIKE].active > 0 ? WEAPONS[WP_SPIKE].active : 9, f = active > 0 ? A - active + 1 : 0;
      float ext = 0;
      if (f > 0) ext = f <= 5 ? 0.42f * f / 5.0f : 0.42f * (1 - (float)(f - 5) / (A - 4));
      if (ext < 0) ext = 0;
      if (rod) t3_object_set_position(rod, 0, 0, ext);
      t3_mesh *col = b->wsub[si + 1];
      int C = WEAPONS[WP_SPIKE].cooldown > 0 ? WEAPONS[WP_SPIKE].cooldown : 60;
      if (col) { float k = active > 0 ? 0.6f : (cooldown > C - 20 ? 0.6f + 0.4f * (C - cooldown) / 20.0f : 1); t3_object_set_scale(col, 1, 1, k); }
      break; }
    case WP_LIFTER: {
      float lift = bt->lift;
      t3_object_set_rotation(piv, -lift * 0.9f, 0, 0);
      break; }
    case WP_HAMMER: {
      float st = 1, ang = hammer_angle(b, bt, active, cooldown, &st);
      t3_object_set_rotation(piv, ang, 0, 0);
      t3_object *head = (t3_object *)b->wsub[si];
      if (head) t3_object_set_scale(head, 1 / sqrtf(st), 1 / sqrtf(st), st);
      if (!secondary && bt->windup == (WEAPONS[WP_HAMMER].windup > 0 ? WEAPONS[WP_HAMMER].windup : 15) - 10) glint_fire(b);   /* the tell */
      break; }
    case WP_FLIPPER: {
      int A = WEAPONS[WP_FLIPPER].active > 0 ? WEAPONS[WP_FLIPPER].active : 4, C = WEAPONS[WP_FLIPPER].cooldown > 0 ? WEAPONS[WP_FLIPPER].cooldown : 108;
      float ang = 0;
      if (active > 0) ang = -(float)(A - active + 1) / A * 1.1f;
      else if (cooldown > 0) {
        int t = C - cooldown, back = (int)(C * 0.3f);
        if (t < 4) ang = -1.1f;                                /* the render hold at the top */
        else if (t < 4 + back) ang = -1.1f * (1 - (float)(t - 4) / back);
        else if (t < 4 + back + 8) ang = -0.03f * sinf(PI * (t - 4 - back) / 8.0f);   /* it lands with a bounce */
      }
      float rest = secondary ? 0 : b->flip_rest;
      t3_object_set_rotation(piv, rest + ang * (1 + rest / 1.1f), 0, 0);
      t3_mesh *pis = b->wsub[si + 1]; if (pis) { float k = 0.12f + 1.15f * sinf(-ang); t3_object_set_scale(pis, 1, k, 1); set_vis(pis, k > 0.13f && rest == 0); }
      break; }
    case WP_CRUSHER: {
      int C = WEAPONS[WP_CRUSHER].cooldown > 0 ? WEAPONS[WP_CRUSHER].cooldown : 120;
      float ang = -0.5f;
      if (bt->clamp > 0) ang = -0.5f + (24 - bt->clamp) / 24.0f * 0.45f;
      else if (bt->lock > 0) ang = -0.05f + 0.01f * sinf(world_t * TAU * 4);
      else if (cooldown > C - 30) ang = -0.05f - 0.45f * (C - cooldown) / 30.0f;
      t3_object_set_rotation(piv, ang, 0, 0);
      /* the ram reaches from the lid to the upper arm 0.3 out from the pin */
      t3_mesh *ram = b->wsub[si + 1]; if (ram) t3_object_set_scale(ram, 1, (0.02f + 0.3f * sinf(-ang)) / 0.3f, 1);
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
      /* the pilot: lit while there is fuel, flickering at 8 Hz under a third of the tank */
      t3_mesh *pilot = b->wsub[si + 1];
      if (pilot) set_vis(pilot, bt->alive && fuel > 0 && (fuel > WP_FLAME_FUEL * 0.3f || fmodf(world_t * 8, 1.0f) < 0.6f));
      if (firing && b->visible) {
        /* the fire cubes from the cone so the flame has grain */
        t3_vec3 p = t3_object_get_world_position(cone ? (void *)cone : (void *)piv);
        fx_flame(p.x, p.y, p.z, sinf(b->h), cosf(b->h), dt);
      }
      break; }
    case WP_WEDGE: {
      /* the plate dips 0.02 for 6 frames after it lands a ram */
      float dip = bt->hit_flash_t > 14 ? 0.02f : 0;
      t3_object_set_position(piv, 0, b->hinge_y - dip, b->hinge_z);
      break; }
    default: break;
  }
}

/* -- the drive, every frame ------------------------------------------- */
static void sync_drive(wbot_t *b, const bot_t *bt, float speed, float dt) {
  const chassis_def_t *c = &CHASSIS[b->build.chassis];
  int alive = !bt || bt->alive;
  float frac = bt && bt->hp_max > 0 ? bt->hp / bt->hp_max : 1;
  b->wheel_spin += speed * dt / 0.17f;
  int hit_side = bt && bt->wheel_hit > 0 ? (bt->wheel_hit_side ? bt->wheel_hit_side : 1) : 0;
  for (int k = 0; k < b->nwheels; k++) {
    int left = b->wheel_x[k] < 0;
    float wob = 0, dy = 0;
    if (hit_side && ((hit_side < 0) == left)) { wob = 0.14f * sinf(3 * b->wheel_spin); dy = 0.02f * sinf(3 * b->wheel_spin); }
    t3_object_set_position(b->wheels[k], b->wheel_x[k], b->wheel_y[k] + dy, b->wheel_z[k]);
    t3_object_set_rotation(b->wheels[k], left ? -b->wheel_spin : b->wheel_spin, left ? PI : 0, wob);
  }
  /* the FOUR WHEEL loses a wheel at HP 30 */
  if (b->build.drive == DR_FOUR && bt && bt->hp <= 30 && b->nwheels == 4 && bt->alive) set_vis(b->wheels[3], 0);
  else if (b->build.drive == DR_FOUR && b->nwheels == 4) set_vis(b->wheels[3], 1);
  if (b->tread_tex) { b->scroll += speed * dt * 4.0f * (bt && bt->tread_hit > 0 ? 0.85f : 1); b->tread_tex->offset.y = -b->scroll; }
  for (int k = 0; k < b->nsprockets; k++) t3_object_set_rotation(b->sprockets[k], b->scroll / 4.0f / 0.12f, 0, 0);
  /* TREADS throw a track at HP 20 on a side whose plate has gone */
  if (b->build.drive == DR_TREAD && bt) for (int k = 0; k < b->nbelts && k < 2; k++) set_vis(b->belts[k], !(bt->hp <= 20 && b->pstate[k ? PN_RIGHT : PN_LEFT] >= 4));
  if (b->nlegs) {
    float limp = frac < 0.3f ? 1 : 0;
    b->gait += speed * dt * 3.0f;
    for (int k = 0; k < b->nlegs; k++) {
      int left = k % 2 == 0;
      float ph = b->gait + (k % 3) * 2.1f + (k & 1) * PI, amp = (speed > 0.05f ? 1 : 0) * (limp && left ? 0.5f : 1);
      float sw = sinf(ph) * 0.35f * amp, bend = fmaxf(0, cosf(ph)) * 0.5f * amp;
      if (limp && k == 3) bend = 0.6f;
      if (b->build.drive == DR_WALKER) {
        t3_object_set_rotation(b->legs[k], (left ? -sw : sw), left ? PI : 0, 0);
        t3_object_set_rotation(b->knees[k], -bend * (left ? -1 : 1), 0, 0);
      } else {
        t3_object_set_rotation(b->legs[k], (left ? -sw : sw) * 0.4f, left ? PI : 0, 0);
        t3_object_set_rotation(b->knees[k], sinf(ph) * 0.2f * amp, 0, 0);
      }
    }
    float bob = (speed > 0.05f ? 0.03f * sinf(b->gait * 2) : 0);
    t3_object_set_position(b->body, 0, bob, 0);
    t3_object_set_position(b->drive_g, 0, -bob, 0);    /* the body bobs on its legs, the feet stay down */
  }
  if (b->skirt) {
    /* the skirt squashes when a bot presses on it, and sags when the hover is nearly done */
    float pinch = bt ? 1 - 0.25f * (bt->skirt_t > 24 ? 1 : bt->skirt_t / 24.0f) : 1;
    if (frac < 0.3f) pinch *= 0.8f;
    t3_object_set_scale(b->skirt, 1, pinch, 1);
    for (int k = 0; k < 2; k++) if (b->fans[k]) { float a = alive ? world_t * 60 + k : 0; t3_object_set_rotation(b->fans[k], 0, a, 0); }
  }
  (void)c;
}

/* -- decals (graphics 4.3) -------------------------------------------- */
static void decals_commit(wbot_t *b) {
  b->dec_dirty = 0;
  if (!b->decals) return;
  soup_t *s = plate_soup(); s->vcol = 0;
  for (int i = 0; i < b->ndec; i++) {
    const wdecal_t *d = &b->dec[i]; const wplate_t *pl = &b->pl[d->panel];
    if (!pl->on || b->pstate[d->panel] >= 3) continue;
    float u0 = (d->kind & 1) * 0.5f, v0 = 0.5f - (d->kind >> 1) * 0.5f;
    float ua[2] = { u0, v0 }, ub[2] = { u0 + 0.5f, v0 }, uc[2] = { u0 + 0.5f, v0 + 0.5f }, ud[2] = { u0, v0 + 0.5f };
    soup_push(s, &pl->xf);
    soup_push_at(s, d->u * pl->w / 2, d->v * pl->h / 2, pl->t + 0.007f, 0, 0, d->rot);
    float hs = d->size / 2, a[3] = { -hs, -hs, 0 }, bb[3] = { hs, -hs, 0 }, c[3] = { hs, hs, 0 }, dd[3] = { -hs, hs, 0 };
    soup_emit(s, a, bb, c, 0, 0, 0, ua, ub, uc); soup_emit(s, a, c, dd, 0, 0, 0, ua, uc, ud);
    soup_pop(s); soup_pop(s);
  }
  dyn_commit(b->decals->geometry, s);
  s->vcol = 1;
  set_vis(b->decals, s->n_ > 0);
}
/* a point in the bot's body frame onto a plate: the plate it falls on (or the one asked for) and where */
static int decal_place(wbot_t *b, int panel, float lx, float ly, float lz, float *u, float *v) {
  if (panel < 0 || panel >= PN_COUNT || !b->pl[panel].on) {
    float ax = fabsf(lx) / (b->wid / 2), az = fabsf(lz) / (b->len / 2);
    panel = ly > b->top_y - 0.02f ? PN_TOP : ax > az ? (lx > 0 ? PN_RIGHT : PN_LEFT) : (lz > 0 ? PN_FRONT : PN_BACK);
  }
  if (!b->pl[panel].on) panel = PN_TOP;
  if (!b->pl[panel].on) return -1;
  const wplate_t *pl = &b->pl[panel];
  float uu, vv;
  switch (panel) {
    case PN_FRONT: uu = lx; vv = ly - pl->y; break;
    case PN_BACK: uu = -lx; vv = ly - pl->y; break;
    case PN_LEFT: uu = lz; vv = ly - pl->y; break;
    case PN_RIGHT: uu = -lz; vv = ly - pl->y; break;
    default: uu = lx; vv = -(lz - pl->z); break;
  }
  uu /= pl->w / 2; vv /= pl->h / 2;
  *u = rtr_clamp(uu, -0.62f, 0.62f); *v = rtr_clamp(vv, -0.55f, 0.55f);
  return panel;
}
static void decal_add(wbot_t *b, int panel, int kind, float u, float v, float size) {
  static uint32_t drng = 0x7a3c19u;
  wdecal_t *d = &b->dec[b->dec_next];
  d->panel = (uint8_t)panel; d->kind = (uint8_t)kind; d->u = u; d->v = v; d->size = size; d->rot = rtr_rand_f(&drng) * TAU;
  b->dec_next = (b->dec_next + 1) % NDECALS; if (b->ndec < NDECALS) b->ndec++;
  b->dec_dirty = 1;
}

void world_bot_decal(int slot, int kind, float lx, float ly, float lz, float nx, float ny, float nz, float size) {
  if (slot < 0 || slot >= MAX_BOTS || !bots[slot].built) return;
  wbot_t *b = &bots[slot];
  int panel = -1;
  if (ny > 0.7f) panel = PN_TOP;
  else if (fabsf(nx) > fabsf(nz)) panel = nx > 0 ? PN_RIGHT : PN_LEFT;
  else if (nz != 0) panel = nz > 0 ? PN_FRONT : PN_BACK;
  float u, v;
  panel = decal_place(b, panel, lx, ly, lz, &u, &v);
  if (panel >= 0) decal_add(b, panel, kind < 0 || kind >= DECAL_KINDS ? DECAL_SCRATCH : kind, u, v, size);
}

/* a heavy or huge hit: a scratch (or a scorch from a flame, a crack in plywood) on the plate that took it,
 * and a hanging plate there swings harder (graphics 4.1, 4.3) */
void world_bot_hit(int slot, float x, float y, float z, float nx, float nz, int weapon, int panel, int band) {
  if (slot < 0 || slot >= MAX_BOTS || !bots[slot].built || !bots[slot].visible) return;
  wbot_t *b = &bots[slot];
  (void)nx; (void)nz;
  float dx = x - b->x, dz = z - b->z, ch = cosf(b->h), sh = sinf(b->h);
  float lx = dx * ch - dz * sh, lz = dx * sh + dz * ch, ly = y - b->y;
  if (b->inverted) { lx = -lx; ly = b->top_y + 0.02f - ly; }   /* inverted: the flip mirrors x and y */
  /* panel 0 is also the not yet merged sim's blank: the front only when the point is at the front */
  int p = panel;
  if (weapon == WP_HAMMER) p = PN_TOP;
  else if (p == PN_FRONT) { float ax = fabsf(lx) / (b->wid / 2), az = lz / (b->len / 2); if (!(az > 0.6f && az > ax)) p = -1; }
  float u, v;
  p = decal_place(b, p, lx, ly, lz, &u, &v);
  if (p < 0) return;
  int kind = weapon == WP_FLAME || weapon == EVW_HAZARD ? DECAL_SCORCH : bot_armour(b) == AR_PLY ? DECAL_CRACK : DECAL_SCRATCH;
  decal_add(b, p, kind, u, v, band >= BAND_HUGE ? 0.3f : 0.22f);
  b->hang_kick[p] = 1;
}

/* -- every frame ------------------------------------------------------- */
static void sync_panels(wbot_t *b, const uint8_t *state) {
  for (int p = 0; p < PN_COUNT; p++) {
    int s = state ? state[p] : 0; if (s > 4) s = 4;
    if (!b->pl[p].on) s = 0;
    if (b->pstate[p] != s) { b->pstate[p] = (uint8_t)s; b->plates_dirty = 1; b->dec_dirty = 1; }
  }
  if (b->plates_dirty) plates_commit(b);
  if (b->dec_dirty) decals_commit(b);
  /* a hanging plate swings 14 degrees off its hinge and flaps at 3 Hz while the bot moves */
  for (int p = 0; p < PN_COUNT; p++) {
    if (!b->hang[p]) continue;
    int on = b->pstate[p] == 3 && !b->panels_hidden;
    set_vis(b->hang[p], on);
    if (!on) continue;
    float flap = 0.0175f * sinf(world_t * TAU * 3) * (b->lean_x != 0 || b->lean_z != 0 ? 1 : 0.4f) + 0.12f * b->hang_kick[p] * sinf(world_t * TAU * 5);
    b->hang_kick[p] *= 0.92f;
    float a = 0.244f + flap;
    if (b->hang_m[p]) t3_object_set_rotation(b->hang_m[p], b->pl[p].hinge_top ? -a : a, 0, 0);
  }
  /* a digit goes with its side plate */
  if (b->digits[0] && CHASSIS[b->build.chassis].shape != SH_CIRCLE) { set_vis(b->digits[0], b->pstate[PN_LEFT] < 4); set_vis(b->digits[1], b->pstate[PN_RIGHT] < 4); }
  if (b->digits[2]) set_vis(b->digits[2], b->pstate[PN_TOP] < 3);
  if (b->tape && !b->is_ops) set_vis(b->tape, b->pstate[PN_TOP] < 2);
}

static void sync_bot(wbot_t *b, const bot_t *bt, float alpha, float dt) {
  const body_t *bo = &bt->b;
  float x = bo->px + (bo->x - bo->px) * alpha, z = bo->pz + (bo->z - bo->pz) * alpha;
  float y = bo->py + (bo->y - bo->py) * alpha;
  float dh = rtr_wrap_pi(bo->h - bo->ph), h = bo->ph + dh * alpha;
  float pitch = bo->pp + rtr_wrap_pi(bo->pitch - bo->pp) * alpha, roll = bo->pr + rtr_wrap_pi(bo->roll - bo->pr) * alpha;
  if (bt->is_ops) h = bo->h;
  if (bt->lifted_by || bt->clamped_by) { x = bo->x; z = bo->z; y = bo->y; h = bo->h; pitch = bo->pitch; roll = bo->roll; }
  int air = bt->airborne || y > 0.15f;
  place_root(b, x, y, z, h, bt->inverted, pitch, roll, air);
  place_blob(b, x, y, z, h, bt->airborne ? bo->vx : 0, bt->airborne ? bo->vz : 0);
  /* MELTY spins the body; a taunt wiggles it; a carried bot hangs nose up when the sim does not pitch it */
  float body_yaw = 0, body_x = 0, body_z = 0;
  if (b->build.drive == DR_MELTY) { b->melty_yaw += bt->spin * 30.0f * dt; body_yaw = b->melty_yaw; }
  if (bt->taunt > 0) body_yaw += sinf(world_t * TAU * 6) * 0.26f;
  if (bt->lifted_by && fabsf(pitch) < 0.01f) body_x = -0.35f;
  else if (bt->stun > 0 && !bt->inverted) body_z = sinf(world_t * 40) * 0.04f;
  float speed = sqrtf(bo->vx * bo->vx + bo->vz * bo->vz);
  /* the SIX WHEEL rocks on its middle pair when the throttle changes; a HOVER leans into its velocity */
  float dthr = bt->drive_t - b->prev_drive_t; b->prev_drive_t = bt->drive_t;
  if (b->build.drive == DR_SIX) { b->rock += (-dthr * 0.6f - b->rock) * 0.15f; body_x += rtr_clamp(b->rock, -0.026f, 0.026f); }
  b->lean_x = b->lean_z = 0;
  if (speed > 0.3f) { b->lean_x = 1; }
  if (b->build.drive == DR_HOVER && bt->alive) {
    float ch = cosf(h), sh = sinf(h), fwd = bo->vx * sh + bo->vz * ch, side = bo->vx * ch - bo->vz * sh;
    body_x += rtr_clamp(fwd * 0.006f, -0.035f, 0.035f); body_z += rtr_clamp(-side * 0.006f, -0.035f, 0.035f);
    float frac = bt->hp_max > 0 ? bt->hp / bt->hp_max : 1;
    t3_object_set_position(b->body, 0, (frac < 0.3f ? 0.02f : 0.01f) * sinf(world_t * TAU), 0);
  }
  /* TWO WHEEL: the body rolls 2 degrees toward a wobbling wheel */
  if (bt->wheel_hit > 0 && b->build.drive == DR_TWO) body_z += (bt->wheel_hit_side < 0 ? -1 : 1) * 0.035f;
  t3_object_set_rotation(b->body, body_x, body_yaw, body_z);
  sync_drive(b, bt, bt->alive ? speed : 0, dt);
  sync_panels(b, bt->panel_state);
  /* at 0 HP the wheels, the drive and the weapons go: the frame stays */
  int burst = !bt->alive && bt->hp <= 0;
  set_vis(b->drive_g, !burst);
  set_vis(b->weapon_g, !burst); set_vis(b->weapon2_g, !burst);
  set_vis(b->seat_ring, bt->alive && cam_one_camera_active() && !bt->is_ops);
  pose_weapon(b, bt, 0, dt);
  if (b->build.weapon2 != WP_NONE) pose_weapon(b, bt, 1, dt);
  if (b->glint) { if (b->glint_t > 0) b->glint_t--; set_vis(b->glint, b->glint_t > 0); }
  /* the floor danger ring under a spinner (graphics 4.8): 0.05 wide at spin x 90/255, its echo at 30/255 */
  if (b->ring_r > 0) {
    float sp = bt->spin;
    if (b->build.weapon == WP_SAW) sp = bt->saw_on ? bt->spin : 0;
    float ch = cosf(h), sh = sinf(h), fy = attached ? arena_floor_y(attached, x, z) : 0;
    t3_object_set_position(b->ring, x + b->ring_z * sh, fy + 0.014f, z + b->ring_z * ch);
    t3_object_set_rotation(b->ring, -PI / 2, 0, b->spin_vis * 0.05f);
    b->ring->materials[0]->opacity = sp * J_SPIN_RING_A / 255.0f;
    set_vis(b->ring, sp > 0.05f && bt->alive && !burst);
  }
  if (b->build.drive == DR_MELTY && b->blur) {
    float a = bt->spin < 0.5f ? 0 : 0.5f * (bt->spin - 0.5f) / 0.5f + 0.2f;
    b->blur->materials[0]->opacity = a; set_vis(b->blur, a > 0.01f && bt->alive);
  }
  /* MELTY LED blinks at 4 Hz on the heading intent; OPS's beacon at 2 Hz while it is out */
  if (b->build.drive == DR_MELTY) set_vis(b->led, fmodf(world_t * 4, 1.0f) < 0.5f);
  if (b->beacon) b->beacon->materials[0]->emissive_intensity = (M.ops_state != OPS_PARKED && fmodf(world_t * 2, 1.0f) < 0.5f) ? 3.0f : 0.2f;
  /* burning: a scorch on a plate every half second */
  if (bt->burn_t > 0 && bt->alive) {
    b->burn_acc += dt;
    if (b->burn_acc > 0.5f) { b->burn_acc = 0; int p = (int)(wrf() * 5); float u = (wrf() - 0.5f) * 1.2f, v = (wrf() - 0.5f) * 1.0f; if (b->pl[p].on) decal_add(b, p, DECAL_SCORCH, u, v, 0.26f); }
  }
  /* smoke and fire by the fraction of max HP, out of the most damaged side (graphics 4.5) */
  if (bt->alive || bt->hp_max > 0) {
    int side = -1, worst = 2;
    for (int p = 0; p < PN_COUNT; p++) if (b->pl[p].on && b->pstate[p] > worst) { worst = b->pstate[p]; side = p; }
    float lx = 0, ly = b->top_y, lz = 0;
    if (side >= 0) { lx = b->pl[side].x; ly = b->pl[side].y; lz = b->pl[side].z; }
    if (bt->inverted) { lx = -lx; ly = b->top_y + 0.02f - ly + 0.2f; }
    float ch = cosf(h), sh = sinf(h);
    float frac = bt->hp_max > 0 ? bt->hp / bt->hp_max : 1;
    fx_bot_damage_frac((int)(b - bots), bt->alive ? frac : 0, bt->burn_t > 0, x + lx * ch + lz * sh, y + ly, z - lx * sh + lz * ch, side);
  }
}

/* the garage test fire runs the same poser on a synthetic bot: the primary's timeline and, in parallel, the
 * 2ND weapon's, so a ROLL OUT with a SAW on the plate still shows something on RT */
static void pose_test_fire(wbot_t *b, float dt) {
  bot_t t; memset(&t, 0, sizeof t);
  t.alive = 1; t.build = b->build; t.hp = 100; t.hp_max = 100;
  if (b->fire_on) {
    b->fire_t += dt;
    float ft = b->fire_t;
    int wp = b->build.weapon, w2 = b->build.weapon2, running = 0;
    if (wp == WP_HORIZ || wp == WP_DRUM || wp == WP_SAW || b->build.drive == DR_MELTY) {
      float up = WEAPONS[wp].spin_up > 0 ? WEAPONS[wp].spin_up : 1.5f;
      t.spin = ft < up ? ft / up : (ft < up + 1.5f ? 1 : fmaxf(0, 1 - (ft - up - 1.5f) / (up * 2)));
      t.saw_on = 1; t.in.buttons = ft < up + 1.5f ? IN_A : 0;
      running |= ft <= up * 3 + 1.5f;
    } else if (wp == WP_HAMMER) {
      int f = (int)(ft * 60), W = WEAPONS[WP_HAMMER].windup, A = WEAPONS[WP_HAMMER].active, C = WEAPONS[WP_HAMMER].cooldown;
      if (f < W) t.windup = (int16_t)(W - f); else if (f < W + A) t.active = (int16_t)(A - (f - W)); else if (f < W + A + C) t.cooldown = (int16_t)(C - (f - W - A));
      running |= f < W + A + C;
    } else if (wp == WP_FLIPPER) {
      int f = (int)(ft * 60), A = WEAPONS[WP_FLIPPER].active, C = WEAPONS[WP_FLIPPER].cooldown;
      if (f < A) t.active = (int16_t)(A - f); else if (f < A + C) t.cooldown = (int16_t)(C - (f - A));
      running |= f < A + C;
    } else if (wp == WP_SPIKE) {
      int f = (int)(ft * 60), A = WEAPONS[WP_SPIKE].active, C = WEAPONS[WP_SPIKE].cooldown;
      if (f < A) t.active = (int16_t)(A - f); else if (f < A + C) t.cooldown = (int16_t)(C - (f - A));
      running |= f < A + C;
    } else if (wp == WP_LIFTER) {
      t.lift = ft < 0.6f ? ft / 0.6f : (ft < 1.4f ? 1 : fmaxf(0, 1 - (ft - 1.4f) / 0.6f));
      running |= ft <= 2.1f;
    } else if (wp == WP_FLAME) {
      t.fuel = WP_FLAME_FUEL; t.in.buttons = ft < 1.6f ? IN_A : 0;
      running |= ft <= 2.2f;
    } else if (wp == WP_CRUSHER) {
      int f = (int)(ft * 60);
      if (f < 24) t.clamp = (int16_t)(24 - f); else if (f < 100) t.lock = 1; else if (f < 130) t.cooldown = (int16_t)(WEAPONS[WP_CRUSHER].cooldown - (f - 100));
      running |= f < 130;
    }
    if (w2 == WP_SAW) {
      t.spin2 = ft < 0.8f ? ft / 0.8f : (ft < 2.3f ? 1 : fmaxf(0, 1 - (ft - 2.3f) / 1.6f));
      t.saw_on = 1; running |= ft <= 3.9f;
    } else if (w2 == WP_FLAME) {
      t.fuel2 = WP_FLAME_FUEL; t.in.buttons |= ft < 1.6f ? IN_A : 0;
      running |= ft <= 2.2f;
    } else if (w2 == WP_SPIKE) {
      int f = (int)(ft * 60);
      if (f < 9) t.active2 = (int16_t)(9 - f);
      running |= f < 30;
    } else if (w2 == WP_LIFTER) {
      t.lift = ft < 0.6f ? ft / 0.6f : (ft < 1.4f ? 1 : fmaxf(0, 1 - (ft - 1.4f) / 0.6f));
      running |= ft <= 2.1f;
    }
    if (!running) b->fire_on = 0;
    if (b->build.drive == DR_MELTY) { b->melty_yaw += t.spin * 30.0f * dt; t3_object_set_rotation(b->body, 0, b->melty_yaw, 0); }
  } else t.fuel = WP_FLAME_FUEL;
  pose_weapon(b, &t, 0, dt);
  if (b->build.weapon2 != WP_NONE) pose_weapon(b, &t, 1, dt);
  if (b->glint) { if (b->glint_t > 0) b->glint_t--; set_vis(b->glint, b->glint_t > 0); }
  if (b->build.drive == DR_MELTY) {
    set_vis(b->led, fmodf(world_t * 4, 1.0f) < 0.5f);
    if (b->blur) { float a = t.spin < 0.5f ? 0 : 0.5f * (t.spin - 0.5f) / 0.5f + 0.2f; b->blur->materials[0]->opacity = a; set_vis(b->blur, a > 0.01f); }
  }
  sync_drive(b, 0, 0, dt);
}

void world_weapon_test_fire(int slot) {
  if (slot < 0 || slot >= MAX_BOTS || !bots[slot].built) return;
  bots[slot].fire_on = 1; bots[slot].fire_t = 0;
}

/* -- debris and pickup rings -------------------------------------- */
/* three instanced meshes (graphics 4.10): plates as bevelled plates, wheels as tyres, everything else (chips,
 * cardboard, bench wood, bumpers, a thrown track) as boxes; and a soft blob under each airborne piece */
enum { DM_PLATE, DM_WHEEL, DM_BOX, DM_COUNT };
static t3_instanced_mesh *debris_im[DM_COUNT], *debris_blob;
static t3_mesh *pickup_rings[MAX_DEBRIS];

static t3_geometry *debris_plate_geo(void) {
  soup_t s; soup_init(&s, 60); sp_cbox(&s, 1, 1, 1, 0.22f); return soup_finish(&s);
}
static t3_geometry *debris_wheel_geo(void) {
  soup_t s; soup_init(&s, 60);
  soup_push_at(&s, 0, 0, 0, 0, 0, -PI / 2);
  const float pr[6][2] = { { 0, -0.5f }, { 0.85f, -0.5f }, { 1, -0.25f }, { 1, 0.25f }, { 0.85f, 0.5f }, { 0, 0.5f } };
  sp_lathe(&s, pr, 6, 8, 0, TAU);
  soup_pop(&s);
  return soup_finish(&s);
}
/* an instanced mesh draws only its first n instances: the live pieces are packed at the front */
static void im_drawn(t3_instanced_mesh *im, int n) { im->count = n; im->mesh.base.visible = n > 0; }
static void debris_hide_all(void) {
  for (int k = 0; k < DM_COUNT; k++) im_drawn(debris_im[k], 0);
  im_drawn(debris_blob, 0);
  for (int i = 0; i < MAX_DEBRIS; i++) set_vis(pickup_rings[i], 0);
}

static void debris_init(void) {
  t3_geometry *gp = debris_plate_geo(), *gw = debris_wheel_geo();
  t3_geometry *gs[DM_COUNT] = { gp, gw, g_box };
  for (int k = 0; k < DM_COUNT; k++) {
    t3_material *m = t3_mesh_standard_material_new(0xffffff); m->roughness = k == DM_WHEEL ? 0.9f : 0.5f; m->metalness = k == DM_PLATE ? 0.35f : 0.1f;
    debris_im[k] = t3_instanced_mesh_new(gs[k], m, MAX_DEBRIS);
    debris_im[k]->mesh.base.frustum_culled = false;
    t3_object_add(&S->base, debris_im[k]); t3_release(debris_im[k]); t3_release(m);
    for (int i = 0; i < MAX_DEBRIS; i++) t3_instanced_mesh_set_color_at(debris_im[k], i, t3_color_hex(C_STEEL));
  }
  t3_release(gp); t3_release(gw);
  t3_material *bm = glass_map(tex_blob, 0.5f);
  debris_blob = t3_instanced_mesh_new(quad_geo(0, 0, 1, 1), bm, MAX_DEBRIS); t3_release(bm);
  debris_blob->mesh.base.frustum_culled = false;
  t3_object_add(&S->base, debris_blob); t3_release(debris_blob);
  for (int i = 0; i < MAX_DEBRIS; i++) {
    t3_material *rm = glass(C_LIVE, 0.8f);
    pickup_rings[i] = t3_mesh_new(g_ring, rm); rot(pickup_rings[i], -PI / 2, 0, 0); t3_object_set_scale(pickup_rings[i], 0.5f, 0.5f, 1);
    t3_object_add(&S->base, pickup_rings[i]); t3_release(pickup_rings[i]); t3_release(rm);
    set_vis(pickup_rings[i], 0);
  }
  debris_hide_all();
}

/* debris_t.colour: 0 the owner's body paint, 1 the owner's armour tint, 2 rubber (wheels),
 * 3 cardboard, 4 bench wood, 5 bone, 6 rust. A bot's plate is its plate paint and a wheel or a track
 * rubber whatever the colour byte says */
static uint32_t debris_colour(const debris_t *d) {
  const wbot_t *o = d->owner < MAX_BOTS && bots[d->owner].built ? &bots[d->owner] : 0;
  if (o && d->kind == DB_PANEL) { paint_t pt = bot_paint(o); return plate_colour(o, d->panel == PN_TOP || d->panel == PN_MOUNT ? pt.lid : pt.plate); }
  if (d->kind == DB_WHEEL || d->kind == DB_TREAD) return 0x262019;
  switch (d->colour) {
    case 0: { if (o) return plate_colour(o, bot_paint(o).plate); return C_STEEL; }
    case 1: { if (o) { uint32_t tints[AR_COUNT] = { C_STEEL, 0xb08a55, 0xd7dadc, 0xe9e5dc, 0x6f6c66, 0x8d97a3, 0x4f3a2d }; return tints[o->build.armour % AR_COUNT]; } return C_STEEL; }
    case 2: return 0x1d1915;
    case 3: return 0xa0773f;
    case 4: return 0xa57c4f;
    case 5: return C_BONE;
    default: return C_RUST;
  }
}

static void sync_debris(const match_t *m, float alpha) {
  int nk[DM_COUNT] = { 0 }, nb = 0;
  for (int i = 0; i < MAX_DEBRIS; i++) {
    const debris_t *d = &m->debris[i];
    if (i >= m->ndebris || d->life <= 0) { set_vis(pickup_rings[i], 0); continue; }
    float fade = d->life < J_DEBRIS_FADE ? (float)d->life / J_DEBRIS_FADE : 1;
    float t = (m->frame + alpha) * DT;
    int k = d->kind == DB_PANEL ? DM_PLATE : d->kind == DB_WHEEL ? DM_WHEEL : DM_BOX;
    t3_vec3 sc; t3_quat q; float fy = attached ? arena_floor_y(attached, d->x, d->z) : 0, lift;
    if (k == DM_PLATE) {
      const wbot_t *o = d->owner < MAX_BOTS && bots[d->owner].built ? &bots[d->owner] : 0;
      float pw = o && d->panel < PN_COUNT && o->pl[d->panel].on ? o->pl[d->panel].w : 0.45f, ph = o && d->panel < PN_COUNT && o->pl[d->panel].on ? o->pl[d->panel].h : 0.35f;
      sc = (t3_vec3){ pw, 0.03f, ph };
      q = d->bounced && d->y < fy + 0.05f ? t3_quat_from_axis_angle((t3_vec3){ 0, 1, 0 }, i * 1.7f) : t3_quat_from_axis_angle((t3_vec3){ 0.3f, 1, 0.2f }, d->spin * t * 2.0f + i);
      lift = 0.02f;
    } else if (k == DM_WHEEL) {
      /* a wheel rolls about its axle along its travel, and lies on its side once it has bounced */
      float r = 0.17f, head = atan2f(d->vx, d->vz);
      sc = (t3_vec3){ 0.14f, r, r };
      if (d->bounced && d->y < fy + 0.2f) { q = t3_quat_multiply(t3_quat_from_axis_angle((t3_vec3){ 0, 1, 0 }, i * 1.3f), t3_quat_from_axis_angle((t3_vec3){ 0, 0, 1 }, PI / 2)); lift = 0.07f; }
      else { q = t3_quat_multiply(t3_quat_from_axis_angle((t3_vec3){ 0, 1, 0 }, head + PI / 2), t3_quat_from_axis_angle((t3_vec3){ 1, 0, 0 }, t * 9 + i)); lift = r; }
    } else {
      sc = d->kind == DB_CHIP ? (t3_vec3){ 0.12f, 0.06f, 0.12f } : d->kind == DB_CARDBOARD ? (t3_vec3){ 0.4f, 0.25f, 0.35f } : d->kind == DB_BENCH ? (t3_vec3){ 0.5f, 0.08f, 0.3f }
         : d->kind == DB_TREAD ? (t3_vec3){ 0.26f, 0.04f, 0.9f } : (t3_vec3){ 0.3f, 0.2f, 0.3f };
      q = t3_quat_from_axis_angle((t3_vec3){ 0.3f, 1, 0.2f }, (d->kind == DB_TREAD ? 0.3f : 1.0f) * d->spin * t * 2.0f + i);
      lift = sc.y / 2 + 0.01f;
    }
    sc.x *= fade; sc.y *= fade; sc.z *= fade;
    t3_mat4 mat4; t3_mat4_compose(&mat4, (t3_vec3){ d->x, d->y + lift, d->z }, q, sc);
    t3_instanced_mesh_set_matrix_at(debris_im[k], nk[k], &mat4);
    t3_instanced_mesh_set_color_at(debris_im[k], nk[k]++, t3_color_hex(debris_colour(d)));
    float up = d->y - fy;
    if (up > 0.06f && (k != DM_BOX || d->kind == DB_TREAD)) {
      float bs = (k == DM_PLATE ? 0.6f : 0.4f) * (1 - up / 4 > 0.3f ? 1 - up / 4 : 0.3f);
      t3_mat4 bmx; t3_mat4_compose(&bmx, (t3_vec3){ d->x, fy + 0.013f, d->z }, t3_quat_from_axis_angle((t3_vec3){ 1, 0, 0 }, -PI / 2), (t3_vec3){ bs, bs, 1 });
      t3_instanced_mesh_set_matrix_at(debris_blob, nb++, &bmx);
    }
    set_vis(pickup_rings[i], d->pickup);
    if (d->pickup) { t3_object_set_position(pickup_rings[i], d->x, 0.02f, d->z); float ps = 0.5f + 0.06f * sinf(world_t * 5 + i); t3_object_set_scale(pickup_rings[i], ps, ps, 1); }
  }
  for (int k = 0; k < DM_COUNT; k++) { im_drawn(debris_im[k], nk[k]); debris_im[k]->instance_matrix->version++; if (debris_im[k]->instance_color) debris_im[k]->instance_color->version++; }
  im_drawn(debris_blob, nb); debris_blob->instance_matrix->version++;
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
        if (hz->floor_tell) { set_vis(hz->floor_tell, tell); hz->floor_tell->materials[0]->opacity = 0.45f * pulse; }
        break; }
      case HZ_PULVERIZER: {
        float target = active ? (phase < 0.2f ? -1.1f + phase / 0.2f * 1.25f : (phase < 0.4f ? 0.15f - (phase - 0.2f) / 0.2f * 1.25f : -1.1f)) : -1.1f;
        if (st && st->cooldown > 0 && !active) target = -1.1f;
        hz->swing += (target - hz->swing) * fminf(1, dt * 30);
        t3_object_set_rotation(hz->mov, hz->swing + (tell ? sinf(world_t * 30) * 0.03f : 0), 0, 0);
        hz->tell_m->emissive_intensity = tell ? 1.0f + 2.0f * pulse : 0;
        /* a char blob grows under the strike square through the tell */
        if (hz->floor_tell) {
          float g = tell && hd->tell_s > 0 && hd->period > 0 ? fminf(1, fmaxf(0, (hd->tell_s - (hd->period - phase)) / hd->tell_s)) : (tell ? 1 : 0);
          set_vis(hz->floor_tell, tell); t3_object_set_scale(hz->floor_tell, 1.2f * g + 0.01f, 1.2f * g + 0.01f, 1); hz->floor_tell->materials[0]->opacity = 0.45f * g;
        }
        break; }
      case HZ_KILLSAW: {
        float up = active ? fminf(1, phase / 0.25f) : 0;
        if (active && phase > hd->active_s - 0.25f) up = fmaxf(0, (hd->active_s - phase) / 0.25f);
        t3_object_set_position(hz->mov, 0, -0.42f + up * 0.6f, 0);
        set_vis(hz->mov, up > 0.001f);                       /* under the floor the saws are not drawn */
        hz->spin += (active ? 45.0f : 8.0f) * dt;
        for (int c = 0; c < hz->mov->child_count; c++) t3_object_set_rotation(hz->mov->children[c], hz->spin, 0, 0);
        hz->tell_m->emissive_intensity = tell ? 2.5f * pulse : (active ? 1.0f : 0);
        if (hz->floor_tell) { set_vis(hz->floor_tell, tell); hz->floor_tell->materials[0]->opacity = 0.35f * pulse; }
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
        t3_object_set_position(hz->mov, 0, 0.03f + (tell ? 0.02f * pulse : 0), -hd->d / 2);   /* the panel twitches in the tell */
        hz->tell_m->emissive_intensity = tell ? 2.5f * pulse : 0.3f;
        break; }
      case HZ_DROPZONE: {
        t3_mesh *sh = (t3_mesh *)hz->mov2;
        float grow = tell ? fminf(1, (hd->tell_s > 0 ? (hd->tell_s - (hd->period - phase)) / hd->tell_s : 1)) : (active ? 1 : 0);
        sh->materials[0]->opacity = 0.5f * grow;
        t3_object_set_scale(sh, 0.9f * grow + 0.01f, 0.9f * grow + 0.01f, 1);
        if (active) { set_vis(hz->mov, 1); float t = fminf(1, phase / 0.35f); t3_object_set_position(hz->mov, 0, 6 * (1 - t) * (1 - t), 0); }
        else set_vis(hz->mov, 0);
        if (hz->floor_tell) { set_vis(hz->floor_tell, tell); hz->floor_tell->materials[0]->opacity = 0.6f * pulse; }
        break; }
      case HZ_LASER: {
        float x = active ? -a->w / 2 + (phase / fmaxf(0.01f, hd->active_s)) * a->w : -a->w / 2;
        t3_object_set_position(hz->mov, x, 0.3f, 0);
        hz->glow_m->opacity = active ? 0.9f : (tell ? 0.25f * pulse : 0);
        hz->tell_m->emissive_intensity = tell ? 1.0f + 2.0f * pulse : (active ? 3.0f : 0.6f);
        if (hz->tell2_m) hz->tell2_m->emissive_intensity = tell ? (fmodf(world_t * 4, 1.0f) < 0.5f ? 3.0f : 0.2f) : (active ? 2.0f : 0.1f);
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
/* the hemisphere's ground colour under every arena: the arena tables carry 0x15120f, which put the
 * undersides and the shaded plates of a char bot in black on the far side of a wall shadow; a warmer
 * 0x2a241e keeps them readable (graphics 3.8) */
#define WORLD_HEMI_GROUND 0x2a241eu
static void set_lights(const arena_def_t *a) {
  if (!a) return;
  sun->color = t3_color_hex(a->sun); sun->intensity = a->sun_i;
  uint32_t ground = a->hemi_ground == 0x15120fu ? WORLD_HEMI_GROUND : a->hemi_ground;
  hemi->color = t3_color_hex(a->hemi_sky); hemi->ground_color = t3_color_hex(ground); hemi->intensity = a->hemi_i;
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
  /* every turntable camera looks from +z toward -z, so the sun comes from the camera side, front right,
   * 31 degrees up: the faces the player sees are lit, not the hemisphere's ground (graphics 3.8) */
  t3_object_set_position(sun, 10, 14, 16);
  hemi->intensity = 1.3f;
  point_a->color = t3_color_hex(0xf2ab1e); point_a->intensity = 5;
  world_garage_light(0);
}

/* the garage's amber point sits 2.5 u in front of the turntable a viewport shows, 2 u up, so the
 * front faces get warmth; render_frame moves it per viewport (one light, four turntables) */
void world_garage_light(int table) {
  if (attached != &GARAGE_BAY || !point_a) return;
  if (table < 0 || table > 3) table = 0;
  t3_object_set_position(point_a, 12.0f * table, 2.0f, 2.5f);
}

/* the crowd's back row bobs at 1 Hz, 0.03 u: its instance matrices are nudged every fourth frame */
static void crowd_bob(void) {
  static int tick;
  if (attached_idx < 0 || (++tick & 3)) return;
  warena_t *w = &arenas[attached_idx];
  if (!w->crowd[0]) return;
  for (int b = 0; b < w->ncrowd_back; b++) {
    t3_mat4 m;
    t3_vec3 p = { w->crowd_back[b].x, w->crowd_back[b].y + 0.03f * sinf(world_t * TAU + w->crowd_back[b].ph), w->crowd_back[b].z };
    t3_mat4_compose(&m, p, w->crowd_back[b].q, (t3_vec3){ w->crowd_back[b].sx, w->crowd_back[b].sy, 1 });
    for (int f = 0; f < 3; f++) t3_instanced_mesh_set_matrix_at(w->crowd[f], w->crowd_back_idx[b], &m);
  }
  for (int f = 0; f < 3; f++) w->crowd[f]->instance_matrix->version++;
}

void world_tick(float dt) {
  if (dt <= 0) dt = 1.f / 60; if (dt > 0.1f) dt = 0.1f;
  wall_dt = dt; ticked = 1;
  world_t += dt;
  crowd_bob();
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
      /* an empty sim slot (sim_init keeps nbots at 5 whenever OPS exists, so a 1 or 2 player match has
       * zeroed bots in slots 2 and 3) is never drawn: it has no body, so a chassis there would be a ghost
       * the player drives through. Visibility is owned by game_start_match and game_attract_start; this
       * loop only ever hides */
      if (!(bt->hp_max > 0 || bt->is_ops)) { if (b->visible) world_bot_visible(i, 0); continue; }
      if (!b->visible) continue;
      sync_bot(b, bt, alpha, dt);
    } else if (b->visible) {
      /* posed outside a match: the turntable carries it */
      if (attached == &GARAGE_BAY) {
        int t = b->seat < 4 ? b->seat : 3;
        for (int k = 0; k < 4; k++) if (fabsf(b->x - 12.0f * k) < 1) t = k;
        place_root(b, 12.0f * t, 0.17f, 0, tt_locked[t] ? 0 : tt_angle[t], 0, 0, 0, 0);
        place_blob(b, 12.0f * t, 0.17f, 0, tt_locked[t] ? 0 : tt_angle[t], 0, 0);
      }
      pose_test_fire(b, dt);
#ifdef RTR_FAKE_DENTS
      { uint8_t st[PN_COUNT]; for (int p = 0; p < PN_COUNT; p++) st[p] = (uint8_t)((i + p + (int)(world_t / 3)) % 5); sync_panels(b, st); }
#else
      sync_panels(b, 0);
#endif
    }
  }
  static int last_mask = -1;
  if (!in_match) last_mask = -1;
  if (in_match) {
    /* the visible bot mask, logged on every change in a fight or the attract (DEEP-plan 3) */
    int mask = 0;
    for (int i = 0; i < MAX_BOTS; i++) if (bots[i].root && bots[i].visible) mask |= 1 << i;
    if (mask != last_mask) { char ml[48]; snprintf(ml, sizeof ml, "[world] bots=0x%02x", mask); wc_log(ml, (unsigned)strlen(ml)); last_mask = mask; }
    sync_debris(m, alpha); sync_hazards(m, dt);
  }
  else if (attached && attached != &GARAGE_BAY) sync_hazards(0, dt);
  /* debris is hidden outside a match */
  static int debris_hidden;
  if (in_match) debris_hidden = 0;
  else if (debris_im[0] && !debris_hidden) { debris_hide_all(); debris_hidden = 1; }
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
  /* the texture rows run from the front wall (row 0) to the back wall: see arena_floor */
  int cx = (int)((x + a->w / 2) / a->w * w->floor_w), cy = w->floor_h - 1 - (int)((z + a->d / 2) / a->d * w->floor_h), cr = (int)(r / a->w * w->floor_w);
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

/* every object under the scene, visible or not (the [draw] log's objs=) */
static int count_objects(const t3_object *o) { int n = 1; for (int i = 0; i < o->child_count; i++) n += count_objects(o->children[i]); return n; }
int world_object_count(void) { return S ? count_objects(&S->base) - 1 : 0; }



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
  g_disc = t3_cylinder_geometry_new(1, 1, 1, 20, 1, false);
  g_circle = t3_circle_geometry_new(1, 16);
  g_plane = t3_plane_geometry_new(1, 1, 1, 1);
  g_ring = t3_ring_geometry_new(0.9f, 1.0f, 24, 1);
  g_sphere = t3_sphere_geometry_new(1, 10, 7);
  g_cone = t3_cone_geometry_new(1, 1, 10, 1, false);
  make_textures();
  make_bot_textures();
  build_garage();
  for (int i = 0; i < ARENA_COUNT; i++) world_build_arena(i, &ARENAS[i]);
  debris_init();
  for (int i = 0; i < MAX_BOTS; i++) bot_alloc(&bots[i], i);
  world_attach_arena(AR_BAY);
}

/* -- the boot compile --------------------------------------------- */
/* Every program this cart draws links here, one scene per call, so nothing links during play
 * (a link is a visible hitch on the station). The scene is dressed first with what a fight adds
 * to an arena: four bots that between them carry every drive, weapon and armour material (treads
 * with their stripe map, the hover skirt, a spinner with its blur disc, a flame cone, a MELTY
 * with its LED), OPS, one of every particle and the flash quads; then it is compiled and rendered
 * once into a four pixel corner, which also forms the auto instanced batches and uploads the
 * painted floor. The dressing is hidden again before the call returns. */
static int warm_built;
static const int warm_slots[5] = { 0, 1, 2, 3, OPS_SLOT };

static void warm_show(int on) {
  if (!warm_built) {
    const build_t set[4] = {
      { CH_WEDGE, DR_TREAD, WP_WEDGE, WP_SAW, AR_STEEL, 0, DC_TAPE_BAND },
      { CH_SHELL, DR_HOVER, WP_FLAME, WP_NONE, AR_UHMW, 1, DC_HAZARD_STRIPES },
      { CH_LOWBOY, DR_FOUR, WP_HORIZ, WP_NONE, AR_ALU, 2, DC_NONE },
      { CH_FRAME, DR_MELTY, WP_NONE, WP_NONE, AR_NONE, 3, DC_SEAT_NUMBER } };
    for (int i = 0; i < 4; i++) world_bot_build(i, &set[i], SEAT_RGB[i]);
    world_bot_build_ops(OPS_SLOT);
    warm_built = 1;
  }
  for (int k = 0; k < 5; k++) {
    int i = warm_slots[k];
    wbot_t *b = &bots[i];
    world_bot_visible(i, on);
    if (on) {
      world_bot_pose(i, -2.0f + k, 0, 0.6f, 0, 0);
      set_vis(b->ring, 1); set_vis(b->led, 1); set_vis(b->seat_ring, 1);
      if (b->glow) b->glow->materials[0]->opacity = 0.5f;
      if (b->blur) { b->blur->materials[0]->opacity = 0.3f; set_vis(b->blur, 1); }
      if (b->glint) set_vis(b->glint, 1);
      if (b->decals) { decal_add(b, PN_TOP, DECAL_SCRATCH, 0, 0, 0.2f); decals_commit(b); }
      for (int p = 0; p < PN_COUNT; p++) if (b->hang[p]) set_vis(b->hang[p], 1);
    } else {
      b->posed = 0;
      if (b->glow) b->glow->materials[0]->opacity = 0;
      if (b->blur) { b->blur->materials[0]->opacity = 0; set_vis(b->blur, 0); }
      if (b->glint) set_vis(b->glint, 0);
      set_vis(b->ring, 0);
      if (b->decals) { b->ndec = 0; b->dec_next = 0; decals_commit(b); }
      for (int p = 0; p < PN_COUNT; p++) if (b->hang[p]) set_vis(b->hang[p], 0);
    }
  }
  if (debris_im[0]) {
    if (on) {
      for (int k = 0; k < DM_COUNT; k++) { t3_mat4 m; t3_mat4_compose(&m, (t3_vec3){ -1.0f + k, 0.3f, 0.6f }, t3_quat_identity(), (t3_vec3){ 0.3f, 0.3f, 0.3f }); t3_instanced_mesh_set_matrix_at(debris_im[k], 0, &m); im_drawn(debris_im[k], 1); debris_im[k]->instance_matrix->version++; }
      t3_mat4 m; t3_mat4_compose(&m, (t3_vec3){ 0, 0.02f, 0.6f }, t3_quat_from_axis_angle((t3_vec3){ 1, 0, 0 }, -PI / 2), (t3_vec3){ 0.5f, 0.5f, 1 }); t3_instanced_mesh_set_matrix_at(debris_blob, 0, &m); im_drawn(debris_blob, 1); debris_blob->instance_matrix->version++;
    } else debris_hide_all();
    set_vis(pickup_rings[0], on);
  }
  if (attached_idx >= 0 && arenas[attached_idx].hill) set_vis(arenas[attached_idx].hill, on);
  fx_warm(on);
}

int world_compile_one(int i) {
  const int count = ARENA_COUNT + 1;
  if (!RTR_R || !S || i < 0 || i >= count) return count;
  t3_camera *c = cam_get(0);
  const arena_def_t *was = attached; int was_idx = attached_idx;
  if (i < ARENA_COUNT) world_attach_arena(i); else world_attach_garage();
  warm_show(1);
  t3_renderer_compile(RTR_R, S, c);
  t3_renderer_set_scissor_test(RTR_R, true);
  t3_renderer_set_viewport(RTR_R, 0, 0, 4, 4);
  t3_renderer_set_scissor(RTR_R, 0, 0, 4, 4);
  t3_renderer_render(RTR_R, S, c);
  t3_renderer_set_scissor_test(RTR_R, false);
  t3_renderer_set_viewport(RTR_R, 0, 0, RTR_W, RTR_H);
  warm_show(0);
  if (was_idx >= 0) world_attach_arena(was_idx); else if (was == &GARAGE_BAY) world_attach_garage(); else world_detach_all();
  return count;
}

void world_compile_all(void) {
  int n = world_compile_one(0);
  for (int i = 1; i < n; i++) world_compile_one(i);
}
