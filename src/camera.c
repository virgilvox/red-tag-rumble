/* camera.c: chase cameras, ONE CAMERA with hysteresis and the tape cut, the KO
 * orbit, the TIME pull, punch and shake, viewport rects per layout, wall
 * push-out, projection, the garage and lobby turntable cameras (spec section
 * 9). Owned by the camera agent (items 5, 6, 10, 15). */
#include "camera.h"
#include "world.h"
#include "juice.h"
#include "game.h"
#include <math.h>
#include <string.h>

#define PI 3.14159265f

enum { CAMM_MATCH, CAMM_GARAGE, CAMM_LOBBY, CAMM_ATTRACT, CAMM_KO, CAMM_TIME };

typedef struct {
  float px, py, pz;            /* smoothed position */
  float lx, ly, lz;            /* smoothed look point */
  float nudge, nudge_v;        /* right stick yaw nudge, radians */
  float shake_amp, shake_t;    /* amplitude in units, time since the last shake refresh */
  float n0x, n0y, n1x, n1y;    /* value noise samples */
  float punch_x, punch_z, punch_t;
  float yaw;                   /* the chase yaw written into pad_state_t.cam_yaw */
  int init;
} chase_t;

static t3_camera *cams[4], *shared;
static int mode = CAMM_MATCH;
static int cur_players = 2, one_cam_setting = ONECAM_OFF, one_cam_allowed = 0, one_cam_active = 0, cur_split = 0;
static int vp_seat[4] = { 0, 1, 2, 3 }, nvp = 2;
static const arena_def_t *arena;
static chase_t ch[4];
static float shake_scale = 1.0f;
static int cut_frames, cut_target;             /* the tape cut: frames left and the ONE CAMERA state to switch to at the midpoint */
static int engage_frames, release_frames;
static rect_t fixed_rects[4]; static int nfixed;
static float ko_angle; static int ko_victim = -1, ko_victor = -1;
static float time_t_;
static float shared_px, shared_py, shared_pz, shared_lx, shared_ly, shared_lz; static int shared_init;
static float attract_t;

static float frand(uint32_t *s) { return rtr_rand_f(s); }
static uint32_t cam_rng = 0x9e3779b9u;

void cam_init(void) {
  for (int i = 0; i < 4; i++) {
    cams[i] = t3_perspective_camera_new(60, 16.f / 9, 0.1f, 120);
    t3_object_set_position(cams[i], 0, 2.2f, -3.5f);
    t3_object_look_at(cams[i], 0, 0.3f, 0.6f);
  }
  shared = t3_perspective_camera_new(50, 16.f / 9, 0.1f, 160);
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
  for (int i = 0; i < *n; i++) cams[i]->fov = cam_fov(kind);
  shared->fov = cam_fov(LAY_SHARED);
}

static int shared_now(void) { return mode == CAMM_KO || mode == CAMM_TIME || mode == CAMM_ATTRACT || (mode == CAMM_MATCH && one_cam_active); }

t3_camera *cam_get(int viewport) {
  if (shared_now() && viewport == 0) return shared;
  return viewport >= 0 && viewport < 4 ? cams[viewport] : 0;
}
t3_camera *cam_shared(void) { return shared; }
int cam_seat_of(int viewport) {
  if (shared_now()) return -1;
  return viewport >= 0 && viewport < nvp ? vp_seat[viewport] : -1;
}
int cam_viewport_of(int seat) {
  if (shared_now()) return 0;
  for (int i = 0; i < nvp; i++) if (vp_seat[i] == seat) return i;
  return -1;
}

static int effective_players(void) {
  int players = cur_players;
  /* SPLIT HALVES cannot fit 3 or 4 players: quadrants stay */
  if (cur_split == SPLIT_QUADS && players < 3 && players > 1) players = 4; /* QUADRANTS for 2: the empty quadrants are the HUD's */
  return players;
}

void cam_set_match(const match_t *m, int one_camera_setting, int split_setting, const arena_def_t *a) {
  mode = CAMM_MATCH;
  arena = a;
  cur_split = split_setting;
  one_cam_setting = one_camera_setting;
  /* the viewports show the non-OPS bots in slot order */
  nvp = 0;
  /* only the player slots: with 3 players and OPS, slot 3 is empty and never gets a view */
  int lim = m ? (m->players > 0 && m->players < m->nbots ? m->players : m->nbots) : 0;
  /* a viewport per human seat (solo is the 1 player full view of 10.17, the practice bot gets the
   * short strip); a match with no human (the attract, a test) shows every bot */
  int humans = 0;
  if (m) for (int i = 0; i < lim; i++) if (!m->bots[i].is_ops && !m->bots[i].is_ai) humans++;
  if (m) for (int i = 0; i < lim && nvp < 4; i++) if (!m->bots[i].is_ops && (!humans || !m->bots[i].is_ai)) vp_seat[nvp++] = m->bots[i].seat;
  if (nvp == 0) { nvp = 2; vp_seat[0] = 0; vp_seat[1] = 1; }
  cur_players = nvp;
  one_cam_allowed = one_camera_setting == ONECAM_ALWAYS ? 1 : one_camera_setting == ONECAM_OFF ? 0 : (a && a->one_camera_default && cur_players == 2);
  one_cam_active = one_camera_setting == ONECAM_ALWAYS;
  cut_frames = 0; engage_frames = release_frames = 0;
  for (int i = 0; i < 4; i++) { ch[i].init = 0; ch[i].shake_amp = 0; ch[i].punch_t = 1; ch[i].nudge = 0; }
  shared_init = 0;
  ko_victim = ko_victor = -1;
}

int cam_current(rect_t out[4], int *n) {
  if (mode == CAMM_GARAGE || mode == CAMM_LOBBY) {
    for (int i = 0; i < nfixed; i++) out[i] = fixed_rects[i];
    *n = nfixed;
    return LAY_QUADS;
  }
  if (shared_now()) { cam_layout(1, 1, out, n); return LAY_SHARED; }
  int players = effective_players();
  cam_layout(players, 0, out, n);
  if (*n > nvp && players < 4) *n = nvp;
  return cam_layout_kind(players, 0);
}

/* -- shake and punch (5.1) ---------------------------------------- */
void cam_set_shake_scale(int setting) { float s[4] = J_SHAKE_SCALE; shake_scale = s[setting & 3]; }
void cam_shake(int seat, float amp) {
  if (seat < 0 || seat > 3) return;
  ch[seat].shake_amp += amp * shake_scale;
  if (ch[seat].shake_amp > J_SHAKE_CLAMP) ch[seat].shake_amp = J_SHAKE_CLAMP;
}
void cam_shake_all(float amp) { for (int s = 0; s < 4; s++) cam_shake(s, amp); }
void cam_punch(int seat, float nx, float nz, float amount) {
  if (seat < 0 || seat > 3) return;
  float l = sqrtf(nx * nx + nz * nz); if (l < 0.001f) { nx = 0; nz = 1; l = 1; }
  ch[seat].punch_x = nx / l * amount; ch[seat].punch_z = nz / l * amount; ch[seat].punch_t = 0;
}
void cam_nudge(int seat, float rx, float ry) { (void)ry; if (seat >= 0 && seat < 4) ch[seat].nudge_v = rx; }
float cam_yaw(int seat) { return seat >= 0 && seat < 4 ? ch[seat].yaw : 0; }

/* 28 Hz value noise in -1..1, two channels */
static void shake_offset(chase_t *c, float dt, float *ox, float *oy) {
  c->shake_t += dt;
  float step = 1.0f / J_SHAKE_HZ;
  while (c->shake_t >= step) {
    c->shake_t -= step;
    c->n0x = c->n1x; c->n0y = c->n1y;
    c->n1x = frand(&cam_rng) * 2 - 1; c->n1y = frand(&cam_rng) * 2 - 1;
  }
  float t = c->shake_t / step;
  *ox = (c->n0x + (c->n1x - c->n0x) * t) * c->shake_amp;
  *oy = (c->n0y + (c->n1y - c->n0y) * t) * c->shake_amp;
  c->shake_amp *= expf(-dt / J_SHAKE_TAU);
  if (c->shake_amp < 0.001f) c->shake_amp = 0;
}

/* -- wall push-out (9.2) ------------------------------------------ */
static void push_out(float *x, float *y, float *z) {
  if (arena) {
    float hw = arena->w / 2 - 0.4f, hd = arena->d / 2 - 0.4f;
    if (!(arena->open_mask & OPEN_PX) && *x > hw) *x = hw;
    if (!(arena->open_mask & OPEN_NX) && *x < -hw) *x = -hw;
    if (!(arena->open_mask & OPEN_PZ) && *z > hd) *z = hd;
    if (!(arena->open_mask & OPEN_NZ) && *z < -hd) *z = -hd;
    /* solids taller than the camera: HVAC units, servers, shelves */
    for (int i = 0; i < arena->nprops; i++) {
      const prop_def_t *p = &arena->props[i];
      if (p->kind != PR_HVAC && p->kind != PR_SERVER && p->kind != PR_SHELF && p->kind != PR_WALL) continue;
      if (p->h + 0.3f < *y) continue;
      float ex = p->w / 2 + 0.4f, ez = p->d / 2 + 0.4f;
      float dx = *x - p->x, dz = *z - p->z;
      if (fabsf(dx) < ex && fabsf(dz) < ez) {
        if (ex - fabsf(dx) < ez - fabsf(dz)) *x = p->x + (dx < 0 ? -ex : ex); else *z = p->z + (dz < 0 ? -ez : ez);
      }
    }
  }
  if (*y < 0.6f) *y = 0.6f;
}

/* forward for the camera: the heading for drives with a front, the velocity for HOVER, the heading intent for MELTY */
static void bot_forward(const bot_t *b, float *fx, float *fz) {
  if (b->build.drive == DR_HOVER) {
    float sp = sqrtf(b->b.vx * b->b.vx + b->b.vz * b->b.vz);
    if (sp > 0.5f) { *fx = b->b.vx / sp; *fz = b->b.vz / sp; return; }
  }
  *fx = rtr_sin(b->b.h); *fz = rtr_cos(b->b.h);
}

static const bot_t *seat_bot(const match_t *m, int seat, int *slot) {
  if (!m) return 0;
  for (int i = 0; i < m->nbots; i++) if (m->bots[i].seat == seat && !m->bots[i].is_ops) { if (slot) *slot = i; return &m->bots[i]; }
  return 0;
}

static void chase_update(int vp, int seat, const match_t *m, float dt) {
  chase_t *c = &ch[seat];
  int slot = -1;
  const bot_t *b = seat_bot(m, seat, &slot);
  t3_camera *cam = cams[vp];
  if (!b) return;
  float bx, by, bz;
  if (!world_bot_pos(slot, &bx, &by, &bz)) { bx = b->b.x; by = b->b.y; bz = b->b.z; }
  float fx, fz; bot_forward(b, &fx, &fz);
  /* the right stick nudges yaw plus and minus 30 degrees and springs back */
  float target_nudge = -c->nudge_v * (30 * PI / 180);
  c->nudge += (target_nudge - c->nudge) * (1 - expf(-dt / 0.15f));
  float cn = cosf(c->nudge), sn = sinf(c->nudge);
  float rfx = fx * cn - fz * sn, rfz = fx * sn + fz * cn;
  float tx = bx - rfx * 3.5f, ty = 2.2f, tz = bz - rfz * 3.5f;
  float lx = bx + fx * 0.6f, ly = by + 0.3f, lz = bz + fx * 0 + fz * 0.6f;
  if (b->b.y > 0.05f) ly = by + 0.3f; else ly = 0.3f;
  if (!c->init) { c->px = tx; c->py = ty; c->pz = tz; c->lx = lx; c->ly = ly; c->lz = lz; c->init = 1; }
  float tau = b->b.y > 0.05f ? 0.2f : 0.12f;
  float k = 1 - expf(-dt / tau), kl = 1 - expf(-dt / 0.06f);
  c->px += (tx - c->px) * k; c->pz += (tz - c->pz) * k; c->py += (ty - c->py) * k;   /* the camera keeps its own height while the bot is airborne */
  c->lx += (lx - c->lx) * kl; c->ly += (ly - c->ly) * kl; c->lz += (lz - c->lz) * kl;
  float px = c->px, py = c->py, pz = c->pz;
  push_out(&px, &py, &pz);
  c->yaw = atan2f(c->lx - px, c->lz - pz);
  /* punch along the hit normal, recovering with 1 - (1 - t)^3 over 0.30 s */
  float ox = 0, oy = 0, oz = 0;
  if (c->punch_t < J_PUNCH_T) {
    float u = 1 - c->punch_t / J_PUNCH_T; u = u * u * u;
    ox += c->punch_x * u; oz += c->punch_z * u;
    c->punch_t += dt;
  }
  /* shake in camera right and up */
  float sx, sy; shake_offset(c, dt, &sx, &sy);
  float dirx = c->lx - px, dirz = c->lz - pz, dl = sqrtf(dirx * dirx + dirz * dirz); if (dl < 0.001f) dl = 1;
  float rx = dirz / dl, rz = -dirx / dl;      /* camera right */
  ox += rx * sx; oz += rz * sx; oy += sy;
  t3_object_set_position(cam, px + ox, py + oy, pz + oz);
  t3_object_look_at(cam, c->lx + ox, c->ly + oy, c->lz + oz);
}

/* the shared ONE CAMERA pose: pitch 55, yaw per arena, distance by the spread (9.3) */
static void shared_pose(const match_t *m, float *px, float *py, float *pz, float *lx, float *ly, float *lz) {
  float cx = 0, cz = 0, maxd = 0; int n = 0;
  if (m) {
    for (int i = 0; i < m->nbots; i++) if (m->bots[i].alive && !m->bots[i].is_ops) { float x, y, z; if (!world_bot_pos(i, &x, &y, &z)) { x = m->bots[i].b.x; z = m->bots[i].b.z; } cx += x; cz += z; n++; }
    if (n) { cx /= n; cz /= n; }
    for (int i = 0; i < m->nbots; i++) for (int j = i + 1; j < m->nbots; j++) {
      if (!m->bots[i].alive || !m->bots[j].alive || m->bots[i].is_ops || m->bots[j].is_ops) continue;
      float dx = m->bots[i].b.x - m->bots[j].b.x, dz = m->bots[i].b.z - m->bots[j].b.z, d = sqrtf(dx * dx + dz * dz);
      if (d > maxd) maxd = d;
    }
  }
  float dist = maxd * 1.6f + 6; if (dist < 12) dist = 12; if (dist > 22) dist = 22;
  float yaw = arena ? arena->shared_yaw_deg * PI / 180 : 0, pitch = 55 * PI / 180;
  *px = cx + sinf(yaw) * cosf(pitch) * dist; *py = sinf(pitch) * dist; *pz = cz + cosf(yaw) * cosf(pitch) * dist;
  *lx = cx; *ly = 0; *lz = cz;
}

static void one_camera_hysteresis(const match_t *m) {
  if (!one_cam_allowed || one_cam_setting == ONECAM_ALWAYS || cut_frames > 0 || !m) return;
  float maxd = 0; int all_in = 1, n = 0;
  for (int i = 0; i < m->nbots; i++) {
    if (!m->bots[i].alive || m->bots[i].is_ops) continue;
    n++;
    for (int j = i + 1; j < m->nbots; j++) {
      if (!m->bots[j].alive || m->bots[j].is_ops) continue;
      float dx = m->bots[i].b.x - m->bots[j].b.x, dz = m->bots[i].b.z - m->bots[j].b.z, d = sqrtf(dx * dx + dz * dz);
      if (d > maxd) maxd = d;
    }
    if (one_cam_active) { float sx, sy; if (!cam_project(0, m->bots[i].b.x, 0.3f, m->bots[i].b.z, &sx, &sy) || sx < 0 || sx > 1280 || sy < 0 || sy > 720) all_in = 0; }
  }
  if (n < 2) return;
  if (!one_cam_active) {
    if (maxd <= 10) engage_frames++; else engage_frames = 0;
    if (engage_frames >= 60) { engage_frames = 0; cut_frames = J_TAPE_CUT_FRAMES; cut_target = 1; }
  } else {
    if (maxd > 12 || !all_in) release_frames++; else release_frames = 0;
    if (release_frames >= 60) { release_frames = 0; cut_frames = J_TAPE_CUT_FRAMES; cut_target = 0; }
  }
}

void cam_update(const match_t *m, float dt) {
  if (dt <= 0) dt = 1.f / 60;
  if (cut_frames > 0) {
    cut_frames--;
    if (cut_frames == J_TAPE_CUT_FRAMES / 2) {       /* the layout changes at the midpoint, under the tape */
      one_cam_active = cut_target;
      if (!one_cam_active) for (int i = 0; i < 4; i++) ch[i].init = 0;
      shared_init = 0;
    }
  }
  switch (mode) {
    case CAMM_MATCH: {
      one_camera_hysteresis(m);
      if (one_cam_active) {
        float px, py, pz, lx, ly, lz; shared_pose(m, &px, &py, &pz, &lx, &ly, &lz);
        if (!shared_init) { shared_px = px; shared_py = py; shared_pz = pz; shared_lx = lx; shared_ly = ly; shared_lz = lz; shared_init = 1; }
        float k = 1 - expf(-dt / 0.25f);
        shared_px += (px - shared_px) * k; shared_py += (py - shared_py) * k; shared_pz += (pz - shared_pz) * k;
        shared_lx += (lx - shared_lx) * k; shared_ly += (ly - shared_ly) * k; shared_lz += (lz - shared_lz) * k;
        /* shake takes the larger band across seats; punch is skipped */
        float amp = 0; for (int s = 0; s < 4; s++) if (ch[s].shake_amp > amp) amp = ch[s].shake_amp;
        chase_t *c = &ch[0]; float keep = c->shake_amp; c->shake_amp = amp; float sx, sy; shake_offset(c, dt, &sx, &sy);
        for (int s = 1; s < 4; s++) ch[s].shake_amp *= expf(-dt / J_SHAKE_TAU);
        (void)keep;
        t3_object_set_position(shared, shared_px + sx, shared_py + sy, shared_pz);
        t3_object_look_at(shared, shared_lx, shared_ly, shared_lz);
        shared->fov = cam_fov(LAY_SHARED);
      } else {
        for (int i = 0; i < nvp; i++) chase_update(i, vp_seat[i], m, dt);
      }
      break; }
    case CAMM_KO: {
      ko_angle += J_KO_ORBIT_DEG_S * PI / 180 * dt;
      float vx = 0, vy = 0, vz = 0, ax = 0, az = 0;
      if (m && ko_victim >= 0 && ko_victim < m->nbots) { if (!world_bot_pos(ko_victim, &vx, &vy, &vz)) { vx = m->bots[ko_victim].b.x; vz = m->bots[ko_victim].b.z; } }
      if (m && ko_victor >= 0 && ko_victor < m->nbots) { float y; if (!world_bot_pos(ko_victor, &ax, &y, &az)) { ax = m->bots[ko_victor].b.x; az = m->bots[ko_victor].b.z; } } else { ax = vx; az = vz; }
      /* victim centred, victor in frame (9.4): the orbit widens with the distance between
       * them and the aim leans a quarter of the way toward the victor */
      float sep = sqrtf((ax - vx) * (ax - vx) + (az - vz) * (az - vz));
      float dist = J_KO_ORBIT_DIST, k = sep * 0.8f + 2.0f;
      if (k > dist) dist = k > 10.0f ? 10.0f : k;
      float px = vx + sinf(ko_angle) * dist, pz = vz + cosf(ko_angle) * dist, py = J_KO_ORBIT_HEIGHT * dist / J_KO_ORBIT_DIST;
      push_out(&px, &py, &pz);
      t3_object_set_position(shared, px, py, pz);
      t3_object_look_at(shared, vx + (ax - vx) * 0.25f, 0.4f, vz + (az - vz) * 0.25f);
      shared->fov = 50;
      break; }
    case CAMM_TIME: {
      time_t_ += dt;
      float px, py, pz, lx, ly, lz; shared_pose(m, &px, &py, &pz, &lx, &ly, &lz);
      float u = time_t_ / 0.6f; if (u > 1) u = 1; u = 1 - (1 - u) * (1 - u) * (1 - u);
      t3_object_set_position(shared, shared_px + (px - shared_px) * u, shared_py + (py - shared_py) * u, shared_pz + (pz - shared_pz) * u);
      t3_object_look_at(shared, shared_lx + (lx - shared_lx) * u, shared_ly + (ly - shared_ly) * u, shared_lz + (lz - shared_lz) * u);
      break; }
    case CAMM_ATTRACT: attract_t += dt; cam_attract(attract_t); break;
    default: break;      /* garage and lobby cameras are fixed by cam_set_garage and cam_set_lobby */
  }
}

/* -- KO and TIME (9.4) -------------------------------------------- */
void cam_ko(int victim, int victor) {
  mode = CAMM_KO; ko_victim = victim; ko_victor = victor;
  float vx = 0, vz = 0, ax = 0, az = 0, y;
  if (!world_bot_pos(victim, &vx, &y, &vz)) { vx = M.bots[victim & 3].b.x; vz = M.bots[victim & 3].b.z; }
  if (victor >= 0 && world_bot_pos(victor, &ax, &y, &az)) ko_angle = atan2f(ax - vx, az - vz) + 0.6f;   /* start on the line from victor to victim, a little aside */
  else ko_angle = 0;
  cut_frames = J_TAPE_CUT_FRAMES; cut_target = 1;
}

void cam_time(void) {
  t3_vec3 p = t3_object_get_world_position(shared_now() ? shared : cams[0]);
  shared_px = p.x; shared_py = p.y; shared_pz = p.z;
  shared_lx = ch[0].lx; shared_ly = ch[0].ly; shared_lz = ch[0].lz;
  if (shared_now()) { shared_lx = p.x; shared_ly = 0; shared_lz = p.z - 1; }
  mode = CAMM_TIME; time_t_ = 0;
  cut_frames = J_TAPE_CUT_FRAMES; cut_target = 1;
}

int cam_one_camera_active(void) { return shared_now(); }
int cam_cut_frames(void) { return cut_frames; }

/* -- projection (for the HUD) ------------------------------------- */
int cam_project(int viewport, float x, float y, float z, float *sx, float *sy) {
  t3_camera *c = cam_get(viewport);
  if (!c) return 0;
  rect_t r[4]; int n; cam_current(r, &n);
  if (viewport >= n) return 0;
  t3_object_update_matrix_world(c, true);
  t3_mat4 inv; t3_mat4_invert(&inv, &c->base.matrix_world);
  t3_vec3 v = t3_vec3_apply_mat4((t3_vec3){ x, y, z }, &inv);           /* camera space; -z is ahead */
  if (v.z > -0.05f) return 0;
  const float *p = c->projection_matrix.e;
  float cx = p[0] * v.x + p[4] * v.y + p[8] * v.z + p[12];
  float cy = p[1] * v.x + p[5] * v.y + p[9] * v.z + p[13];
  float cw = p[3] * v.x + p[7] * v.y + p[11] * v.z + p[15];
  if (cw <= 0.0001f) return 0;
  float nx = cx / cw, ny = cy / cw;
  if (sx) *sx = r[viewport].x + (nx * 0.5f + 0.5f) * r[viewport].w;
  if (sy) *sy = r[viewport].y + (1 - (ny * 0.5f + 0.5f)) * r[viewport].h;
  return 1;
}

int cam_enemy_offscreen_ex(int seat, float *edge_x, float *edge_y, int *enemy) {
  int vp = cam_viewport_of(seat);
  if (vp < 0 || shared_now()) return 0;
  int slot = -1;
  const bot_t *me = seat_bot(&M, seat, &slot);
  if (!me) return 0;
  int best = -1; float bd = 1e9f;
  for (int i = 0; i < M.nbots; i++) {
    const bot_t *o = &M.bots[i];
    if (i == slot || !o->alive || o->is_ops) continue;
    if (M.mode == MODE_2V2 && o->team == me->team) continue;
    float dx = o->b.x - me->b.x, dz = o->b.z - me->b.z, d = dx * dx + dz * dz;
    if (d < bd) { bd = d; best = i; }
  }
  if (best < 0) return 0;
  if (enemy) *enemy = best;
  rect_t r[4]; int n; cam_current(r, &n);
  if (vp >= n) return 0;
  const rect_t *R = &r[vp];
  float sx, sy;
  int vis = cam_project(vp, M.bots[best].b.x, 0.3f, M.bots[best].b.z, &sx, &sy);
  if (vis && sx >= R->x && sx <= R->x + R->w && sy >= R->y && sy <= R->y + R->h) return 0;
  float cx = R->x + R->w / 2.0f, cy = R->y + R->h / 2.0f, dx, dy;
  if (vis) { dx = sx - cx; dy = sy - cy; }
  else {
    /* behind the camera: use the direction in camera space */
    t3_camera *c = cams[vp];
    t3_mat4 inv; t3_object_update_matrix_world(c, true); t3_mat4_invert(&inv, &c->base.matrix_world);
    t3_vec3 v = t3_vec3_apply_mat4((t3_vec3){ M.bots[best].b.x, 0.3f, M.bots[best].b.z }, &inv);
    dx = v.x; dy = 1;                 /* point at the bottom edge toward the enemy's side */
    if (fabsf(dx) < 0.01f) dx = 0.01f;
  }
  float hw = R->w / 2.0f - 24, hh = R->h / 2.0f - 24;
  float kx = fabsf(dx) > 0.001f ? hw / fabsf(dx) : 1e9f, ky = fabsf(dy) > 0.001f ? hh / fabsf(dy) : 1e9f, k = kx < ky ? kx : ky;
  if (edge_x) *edge_x = cx + dx * k;
  if (edge_y) *edge_y = cy + dy * k;
  return 1;
}
int cam_enemy_offscreen(int seat, float *edge_x, float *edge_y) { return cam_enemy_offscreen_ex(seat, edge_x, edge_y, 0); }

/* -- turntables, garage, lobby, attract --------------------------- */
void cam_turntable(int viewport, int table, float dist, float height) {
  t3_camera *c = viewport >= 0 && viewport < 4 ? cams[viewport] : 0;
  if (!c) return;
  float x, y, z;
  world_turntable_pos(table, &x, &y, &z);
  t3_object_set_position(c, x + 0.5f, y + height, z + dist);
  t3_object_look_at(c, x, y + 0.3f, z);
}

void cam_set_garage(int players) {
  mode = CAMM_GARAGE;
  if (players < 1) players = 1;
  if (players <= 2) {
    nfixed = 2;
    fixed_rects[0] = (rect_t){ 16, 100, 608, 230 }; fixed_rects[1] = (rect_t){ 656, 100, 608, 230 };
    for (int i = 0; i < 2; i++) { cams[i]->fov = 24; cam_turntable(i, i, 4.8f, 1.9f); }
  } else {
    nfixed = players;
    rect_t q[4] = { { 16, 32, 300, 230 }, { 656, 32, 300, 230 }, { 16, 392, 300, 230 }, { 656, 392, 300, 230 } };
    for (int i = 0; i < players && i < 4; i++) { fixed_rects[i] = q[i]; cams[i]->fov = 36; cam_turntable(i, i, 3.3f, 1.35f); }
  }
  nvp = nfixed; for (int i = 0; i < nvp; i++) vp_seat[i] = i;
}

void cam_set_lobby(int seated_mask) {
  mode = CAMM_LOBBY;
  nfixed = 0;
  for (int s = 0; s < 4; s++) {
    if (!(seated_mask >> s & 1)) continue;
    fixed_rects[nfixed] = (rect_t){ 20 + 310 * s + 16, 252, 268, 120 };
    cams[nfixed]->fov = 22; cam_turntable(nfixed, s, 3.6f, 1.2f);
    vp_seat[nfixed] = s;
    nfixed++;
  }
  nvp = nfixed;
}

/* results (10.21): one viewport (64, 180, 420, 300) on the winner's turntable; table -1 shows none (TOURNAMENT) */
void cam_set_results(int table) {
  mode = CAMM_GARAGE;
  nfixed = 0;
  if (table >= 0 && table < 4) {
    fixed_rects[0] = (rect_t){ 64, 180, 420, 300 };
    cams[0]->fov = 30; cam_turntable(0, table, 4.4f, 1.7f);
    vp_seat[0] = table; nfixed = 1;
  }
  nvp = nfixed;
}

void cam_set_attract(void) { mode = CAMM_ATTRACT; attract_t = 0; }

void cam_attract(float t) {
  float yaw = t * 0.08f, dist = 15;
  t3_object_set_position(shared, sinf(yaw) * dist * 0.6f, 12, cosf(yaw) * dist * 0.6f + 2);
  t3_object_look_at(shared, 0, 0, 0);
  shared->fov = 50;
}
