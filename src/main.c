/* main.c: host glue. wc_info, the negotiated resolution, the pad reader of
 * 10.2, the frame (accumulator, timescale, steps, render, HUD, audio), the
 * event drain into fx, audio, stingers, rumble and camera, the save struct
 * region and the hash log (spec section 14). The phase machine that decides
 * what runs is game_flow.c. */
#include "wasmcart.h"
#include "wc_gl.h"
#include "three.h"
#include "game.h"
#include "sim.h"
#include "bot.h"
#include "arena.h"
#include "ai.h"
#include "world.h"
#include "fx.h"
#include "camera.h"
#include "render.h"
#include "hud.h"
#include "draw.h"
#include "audio.h"
#include "juice.h"
#include "content.h"
#ifdef RTR_FAKE_MENUS
#include "draw_menus.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <malloc.h>
#include <unistd.h>

#define AUDIO_CAP 8192
#define DEAD_ZONE_SQ (5898 * 5898)       /* 0.18 of 32767, squared: the sim's radial dead zone (12) */

/* the sim's hit stop column must match the feel table */
_Static_assert(STOP_TAP == J_STOP_TAP && STOP_HIT == J_STOP_HIT && STOP_HEAVY == J_STOP_HEAVY &&
               STOP_HUGE == J_STOP_HUGE && STOP_CLASH == J_STOP_CLASH && STOP_KO == J_STOP_KO, "sim.h STOP_* must mirror juice.h J_STOP_*");

static uint32_t framebuffer[4];          /* GL cart: the host presents the GL surface */
static float audio_ring[AUDIO_CAP * 2];
static uint32_t audio_cursor;
static wc_pad_t pads[4];
static wc_time_t tm;
static wc_info_t info;
static wc_host_info_t host_info;
save_t SAVE;                             /* static storage: the host persists this region */

t3_renderer *RTR_R;
int RTR_W = 1280, RTR_H = 720;
static t3_scene *hud_scene;
static t3_camera *hud_cam;
/* the HUD textures: a ring, so the one written each frame was last read frames ago and the driver
 * never waits for the GPU on a partial write (hud_upload); a shadow of each one's content and the
 * bands it still has to catch up on; the quad's material points at the one on show */
#ifndef HUD_RING
#define HUD_RING 3
#endif
#define HUD_STAGE_ROWS 192                /* the staging buffer holds this many full width rows; a taller rect goes in chunks */
static t3_texture *hud_tex[HUD_RING];
static t3_material *hud_mat;
static int hud_cur;                      /* the ring entry the quad shows */
static uint8_t *hud_shadow[HUD_RING];    /* HUD_W x HUD_H x 4 each: what that GL texture holds */
static uint8_t *hud_stage;               /* HUD_W x HUD_STAGE_ROWS x 4: the packed rect copies of hud_upload */
static uint8_t hud_pend[HUD_RING][HUD_NBANDS]; static int16_t hud_pend_x0[HUD_RING][HUD_NBANDS], hud_pend_x1[HUD_RING][HUD_NBANDS];
#define GL_SRGB8_ALPHA8_ 0x8C43
static uint32_t seed = 7;
static float acc;                        /* the sim accumulator (4.1) */
static int sim_logged_frame = -1;

__attribute__((export_name("wc_get_info")))
wc_info_t *wc_get_info(void) {
  info.version = WC_ABI_VERSION;
  info.width = RTR_W; info.height = RTR_H;
  info.fb_ptr = (uint32_t)(uintptr_t)framebuffer;
  info.audio_ptr = (uint32_t)(uintptr_t)audio_ring;
  info.audio_cap = AUDIO_CAP;
  info.audio_write_ptr = (uint32_t)(uintptr_t)&audio_cursor;
  info.input_ptr = (uint32_t)(uintptr_t)pads;
  info.time_ptr = (uint32_t)(uintptr_t)&tm;
  info.host_info_ptr = (uint32_t)(uintptr_t)&host_info;
  info.flags = WC_FLAG_AUDIO_F32 | WC_FLAG_DETERMINISTIC;   /* no keyboard, no net (20.2) */
  info.gpu_api = 1;
  info.audio_sample_rate = 0;
  info.save_ptr = (uint32_t)(uintptr_t)&SAVE;
  info.save_size = sizeof SAVE;
  return &info;
}

__attribute__((export_name("wc_set_seed")))
void wc_set_seed(uint32_t s) { seed = s ? s : 7; }

/* -- init ----------------------------------------------------------- */

__attribute__((export_name("wc_init")))
void wc_init(void) {
  extern const struct t3_gen_table rtr_gl;
  t3_register_program_table(&rtr_gl, NULL);

  /* honour the station's Internal Resolution, capped at 720p (13) */
  uint32_t w = 1280, h = 720;
  wc_negotiate_resolution(&host_info, &w, &h, 1280, 720);
  if (w < 320 || h < 180) { w = 1280; h = 720; }
  RTR_W = (int)w; RTR_H = (int)h;
  info.width = RTR_W; info.height = RTR_H;

  RTR_R = t3_renderer_new(RTR_W, RTR_H);
  t3_renderer_set_shadow_map(RTR_R, false, T3_PCF_SHADOW_MAP);   /* no shadow maps anywhere (20.3) */
  t3_renderer_set_exact_output(RTR_R, true);                     /* the HUD pass composites over the views */
  t3_renderer_set_clear_color(RTR_R, C_CHAR, 1);

  if (!save_valid(&SAVE)) save_defaults(&SAVE);

  hud_init();
  game_init();
  world_init();
  cam_init();
  fx_init();
  draw_init();
  uint32_t rate = host_info.audio_sample_rate ? host_info.audio_sample_rate : 48000;
  audio_init(rate);
  audio_set_levels(SAVE.sfx, SAVE.music);

  hud_scene = t3_scene_new();
  hud_cam = t3_orthographic_camera_new(-1, 1, 1, -1, 0, 10);
  t3_object_set_position(hud_cam, 0, 0, 5);
  hud_clear();
  /* the ring's GL textures are made here, cleared, in the sRGB format three.c gives an sRGB data
   * texture (GL decodes on sampling, the program does not); three.c samples them and never uploads */
  glActiveTexture(GL_TEXTURE0 + 31);
  for (int k = 0; k < HUD_RING; k++) {
    GLuint name = 0;
    glGenTextures(1, &name);
    glBindTexture(GL_TEXTURE_2D, name);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8_, HUD_W, HUD_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, hud_px);
    hud_tex[k] = t3_texture_from_gl(name, HUD_W, HUD_H, 1, false, true);
    hud_tex[k]->mag_filter = T3_NEAREST; hud_tex[k]->min_filter = T3_NEAREST;     /* the data texture's fields, for the program key */
    hud_tex[k]->wrap_s = T3_CLAMP_TO_EDGE; hud_tex[k]->wrap_t = T3_CLAMP_TO_EDGE;
    hud_tex[k]->flip_y = false; hud_tex[k]->generate_mipmaps = false;
    hud_shadow[k] = calloc((size_t)HUD_W * HUD_H, 4);
  }
  t3_renderer_reset_bindings(RTR_R);
  hud_stage = malloc((size_t)HUD_W * HUD_STAGE_ROWS * 4);
  hud_mat = t3_mesh_basic_material_new(0xffffff);
  t3_material_set_map(hud_mat, hud_tex[0]); hud_cur = 0;
  hud_mat->transparent = true; hud_mat->depth_test = false; hud_mat->depth_write = false;
  t3_mesh *quad = t3_mesh_new(t3_plane_geometry_new(2, 2, 1, 1), hud_mat);
  t3_object_add(&hud_scene->base, quad);
  t3_release(quad);

  SETUP.seed = seed; G.seed = seed;
  char msg[96];
  snprintf(msg, sizeof msg, "red-tag-rumble: %dx%d hud %dx%d seed %u", RTR_W, RTR_H, HUD_W, HUD_H, seed);
  wc_log(msg, (unsigned)strlen(msg));
}

/* -- pads ----------------------------------------------------------- */

/* every pad is polled every frame; connected is never read (20.1) */
static void read_pads(float dt) {
  for (int i = 0; i < 4; i++) {
    pad_input_t *p = &PADS[i];
    const wc_pad_t *w = &pads[i];
    p->prev = p->cur;
    p->cur.buttons = (uint16_t)(w->buttons & 0x3fff);
    p->cur.lx = w->left_x; p->cur.ly = w->left_y; p->cur.rx = w->right_x; p->cur.ry = w->right_y;
    p->cur.lt = (uint8_t)(w->left_trigger > 0 ? w->left_trigger >> 7 : 0);
    p->cur.rt = (uint8_t)(w->right_trigger > 0 ? w->right_trigger >> 7 : 0);
    /* the d-pad drives the same axes as the left stick (12): a keyboard pad has no sticks at all. When
     * the left stick rests inside the 0.18 dead zone the d-pad bits become full deflections (ly negative
     * is forward, as the sim reads it); a TANK seat gets both sides from them so the right stick's
     * absence never matters. The sim stays headless: it only ever sees axes. */
    {
      int lx = p->cur.lx, ly = p->cur.ly;
      int dpad = p->cur.buttons & (IN_UP | IN_DOWN | IN_LEFT | IN_RIGHT);
      if (dpad && lx * lx + ly * ly < DEAD_ZONE_SQ) {
        int fwd = (p->cur.buttons & IN_UP ? 1 : 0) - (p->cur.buttons & IN_DOWN ? 1 : 0);
        int steer = (p->cur.buttons & IN_RIGHT ? 1 : 0) - (p->cur.buttons & IN_LEFT ? 1 : 0);
        int tank = p->seat >= 0 && p->seat < 4 && G.seats[p->seat].scheme == SCHEME_TANK;
        if (tank && p->cur.rx * p->cur.rx + p->cur.ry * p->cur.ry < DEAD_ZONE_SQ) {
          int l = fwd + steer, r = fwd - steer;
          l = l < -1 ? -1 : l > 1 ? 1 : l; r = r < -1 ? -1 : r > 1 ? 1 : r;
          p->cur.ly = (int16_t)(-32767 * l); p->cur.ry = (int16_t)(-32767 * r);
        } else {
          p->cur.lx = (int16_t)(32767 * steer); p->cur.ly = (int16_t)(-32767 * fwd);
        }
      }
    }
    p->cur.cam_yaw = p->seat >= 0 ? cam_yaw(p->seat) : 0;
    p->edge = (uint16_t)(p->cur.buttons & ~p->prev.buttons);
    p->release = (uint16_t)(p->prev.buttons & ~p->cur.buttons);
    /* menu navigation: d-pad or left stick at 0.5, edge with repeat after 18 frames then every 8 (10.2) */
    int dx = 0, dy = 0;
    if ((p->cur.buttons & IN_LEFT) || p->cur.lx < -16384) dx = -1;
    if ((p->cur.buttons & IN_RIGHT) || p->cur.lx > 16384) dx = 1;
    if ((p->cur.buttons & IN_UP) || p->cur.ly < -16384) dy = -1;
    if ((p->cur.buttons & IN_DOWN) || p->cur.ly > 16384) dy = 1;
    p->nav_x = 0; p->nav_y = 0;
    if (dx) { p->repeat_x++; if (p->repeat_x == 1 || (p->repeat_x > 18 && (p->repeat_x - 18) % 8 == 0)) p->nav_x = (int8_t)dx; } else p->repeat_x = 0;
    if (dy) { p->repeat_y++; if (p->repeat_y == 1 || (p->repeat_y > 18 && (p->repeat_y - 18) % 8 == 0)) p->nav_y = (int8_t)dy; } else p->repeat_y = 0;
    p->any = p->cur.buttons || dx || dy || p->cur.lt > TRIGGER_ON || p->cur.rt > TRIGGER_ON ||
             p->cur.rx < -16384 || p->cur.rx > 16384 || p->cur.ry < -16384 || p->cur.ry > 16384;
    p->idle = p->any ? 0 : p->idle + dt;
  }
}

/* write this frame's pads into the match: a seated human's pad, or its AI. OPS
 * drives itself inside the sim (ops_step), so it gets no input here. Under the
 * rig's PRACTICE=off flag the AI seats sit still (M.park). */
static void feed_inputs(void) {
  for (int i = 0; i < M.nbots && i < 4; i++) {
    bot_t *b = &M.bots[i];
    if (b->is_ops) continue;
    int pad = game_slot_pad(i);
    if (pad >= 0) b->in = PADS[pad].cur;
    else if (M.park && !G.attract) memset(&b->in, 0, sizeof b->in);
    else ai_think(&AIS[i], &M, &b->in);
  }
}

/* -- the event drain (5.1, 14.2) ------------------------------------ */

static int seat_pad(int slot) {
  if (slot < 0 || slot >= M.nbots) return -1;
  int seat = M.bots[slot].seat;
  if (seat > 3 || G.seats[seat].kind != SEAT_HUMAN) return -1;
  return G.seats[seat].pad;
}

/* the per event [hit] and [rumble] lines are an RTR_TRACE build (EXTRA_CFLAGS=-DRTR_TRACE): a 4 pad
 * fight writes hundreds a minute through the host's log callback and buries [phase] and [sim]. The
 * station build keeps one [hits] summary per sim second (14.2) */
static int hits_n, rumble_n; static float hits_dmg;

static void rumble(int pad, float lo, float hi, int ms) {
  if (pad < 0 || !SAVE.rumble || ms <= 0 || G.attract) return;
  wc_pad_rumble((unsigned)pad, lo, hi, (unsigned)ms);
  rumble_n++;
#ifdef RTR_TRACE
  char m[64];
  snprintf(m, sizeof m, "[rumble] pad %d %.2f %.2f %d", pad + 1, lo, hi, ms);
  wc_log(m, (unsigned)strlen(m));
#endif
}

/* the viewport that shows a match slot, or -1 (an AI seat without a view, OPS) */
static int slot_view(int slot) {
  if (slot < 0 || slot >= M.nbots || M.bots[slot].seat > 3) return -1;
  return cam_viewport_of(M.bots[slot].seat);
}
static int slot_seat(int slot) { return slot >= 0 && slot < M.nbots && M.bots[slot].seat <= 3 ? M.bots[slot].seat : -1; }
static uint32_t slot_colour(int slot, uint32_t fallback) { int s = slot_seat(slot); return s >= 0 ? seat_colour(s) : fallback; }

/* stinger lines rotate through their pool; a local counter, never the sim's rng */
static uint32_t sting_n;
#define PICK(arr) (arr)[(sting_n++) % LEN(arr)]

static void sting_slot(int slot, const char *s, uint32_t c) { int v = slot_view(slot); if (v >= 0) stinger(v, s, c); }

/* a world anchored number in every live viewport (each clips its own) */
static void number_all(float x, float y, float z, int value, uint32_t rgb, int big) {
  rect_t vp[4]; int n = 0;
  cam_current(vp, &n);
  for (int i = 0; i < n && i < 4; i++) damage_number(i, x, y, z, value, rgb, big);
}
static void text_all(float x, float y, float z, const char *s, uint32_t rgb) {
  rect_t vp[4]; int n = 0;
  cam_current(vp, &n);
  for (int i = 0; i < n && i < 4; i++) damage_text(i, x, y, z, s, rgb);
}
static int dmg_int(float d) { int v = (int)(d + 0.5f); return v < 1 ? 1 : v; }

static void hit_log(const event_t *e, int band) {
  hits_n++; hits_dmg += e->dmg;
#ifdef RTR_TRACE
  char m[80];
  snprintf(m, sizeof m, "[hit] f=%u sim=%d type=%d a=%d b=%d dmg=%.1f band=%d", G.frame, M.frame, e->type, e->a, e->b, e->dmg, band);
  wc_log(m, (unsigned)strlen(m));
#else
  (void)e; (void)band;
#endif
}

/* once per sim second while anything happened, right after the [sim] hash line */
static void hits_summary(void) {
  if (!hits_n && !rumble_n) return;
  char m[64];
  snprintf(m, sizeof m, "[hits] f=%d n=%d dmg=%.0f rumble=%d", M.frame, hits_n, hits_dmg, rumble_n);
  wc_log(m, (unsigned)strlen(m));
  hits_n = 0; rumble_n = 0; hits_dmg = 0;
}

static void drain_events(void) {
  if (G.attract) {                       /* the title attract: sparks and smoke, no sound, no stingers */
    for (int i = 0; i < M.nev; i++) { const event_t *e = &M.ev[i]; if (e->type == EV_HIT || e->type == EV_RAM || e->type == EV_CLASH) fx_hit(e, e->type == EV_CLASH ? BAND_CLASH : hit_band(e->dmg)); else fx_event(e, &M); }
    M.nev = 0;
    return;
  }
  int sfx_band_done = 0;                 /* one SFX per band per frame (5.1) */
  for (int i = 0; i < M.nev; i++) {
    const event_t *e = &M.ev[i];
    int band = -1;
    switch (e->type) {
      case EV_HIT: case EV_RAM: band = e->dmg > 0 ? hit_band(e->dmg) : -1; break;
      case EV_CLASH: band = BAND_CLASH; break;
      case EV_KO: band = BAND_KO; break;
      default: break;
    }
    if (band >= 0) {
      const hit_band_t *hb = &HIT_BANDS[band];
      hit_log(e, band);
      fx_hit(e, band);
      int victim = e->b < M.nbots ? e->b : -1, attacker = e->a < M.nbots ? e->a : -1;
      int vs = slot_seat(victim), as = slot_seat(attacker);
      if (band == BAND_KO || band == BAND_CLASH) cam_shake_all(hb->shake);
      else {
        if (vs >= 0) cam_shake(vs, hb->shake);
        if (as >= 0) cam_shake(as, hb->shake * 0.5f);
      }
      /* punch: the victim's camera along the hit normal, the attacker's a recoil half as far */
      if (vs >= 0) cam_punch(vs, e->nx, e->nz, hb->punch);
      if (as >= 0 && band >= BAND_HEAVY) cam_punch(as, -e->nx, -e->nz, hb->punch * 0.5f);
      if (!(sfx_band_done & (1 << band))) {
        audio_play(hb->sfx);
        sfx_band_done |= 1 << band;
      }
      if (hb->crowd) audio_crowd(hb->crowd / 100.0f);
      if (band == BAND_KO || band == BAND_CLASH) { rumble(seat_pad(victim), hb->rum_lo, hb->rum_hi, hb->rum_ms); rumble(seat_pad(attacker), hb->rum_lo, hb->rum_hi, hb->rum_ms); }
      else { rumble(seat_pad(victim), hb->rum_lo, hb->rum_hi, hb->rum_ms); rumble(seat_pad(attacker), 0, hb->rum_hi, hb->rum_ms / 2); }
      /* the number: bone, F_MONO_L for tap and hit, F_H2 from heavy up; CLASH is words (5.1) */
      if (e->type == EV_CLASH) { text_all(e->x, e->y + 0.6f, e->z, ST_CLASH[0], C_BONE); sting_slot(victim, PICK(ST_CLASH), C_BONE); sting_slot(attacker, ST_CLASH[0], C_BONE); }
      else if (band != BAND_KO) number_all(e->x, e->y + 0.6f, e->z, dmg_int(e->dmg), C_BONE, e->dmg >= J_NUMBER_BIG_MIN);
      if (band != BAND_KO && band != BAND_CLASH && e->dmg >= J_STINGER_BIGHIT_MIN) sting_slot(victim, PICK(ST_BIGHIT), slot_colour(attacker, C_BONE));
      game_on_event(e);
      continue;
    }
    fx_event(e, &M);
    int a = e->a < M.nbots ? e->a : -1;
    switch (e->type) {
      case EV_LAUNCH:
        audio_play(SFX_LAUNCH); rumble(seat_pad(e->b), 0.3f, 0.7f, 80);
        { const char *s = PICK(ST_AIRBORNE); sting_slot(e->b, s, slot_colour(a, C_BONE)); if (a != e->b) sting_slot(a, s, slot_colour(a, C_BONE)); }
        break;
      case EV_LAND:
        audio_play_var(SFX_HIT_LIGHT, SFXV_LOW);
        if (e->dmg > 0.5f) { cam_shake(slot_seat(e->b), J_LAND_SHAKE); number_all(e->x, e->y + 0.6f, e->z, dmg_int(e->dmg), C_BONE, 0); }
        break;
      case EV_WALL:
        audio_play_var(SFX_HIT_LIGHT, SFXV_LOW);
        cam_shake(slot_seat(a), J_WALL_SHAKE);
        if (e->dmg > 0.5f) number_all(e->x, e->y + 0.4f, e->z, dmg_int(e->dmg), C_RUST, 0);
        break;
      case EV_SPIN_WALL:
        audio_play_var(SFX_CLASH, SFXV_LOW); cam_shake(slot_seat(a), J_SPINWALL_SHAKE);   /* the 0.6 variant of 11.3 */
        number_all(e->x, e->y + 0.4f, e->z, dmg_int(e->dmg), C_RUST, 1);
        break;
      case EV_INVERT: audio_play_var(SFX_KLAXON, SFXV_SHORT); sting_slot(a, PICK(ST_INVERTED), slot_colour(e->b, C_BONE)); break;
      case EV_COUNT_TICK:
        if (a >= 0 && M.bots[a].deck_count == 60) { audio_play_var(SFX_KLAXON, SFXV_TELL); sting_slot(a, PICK(ST_DECK), C_RUST); }
        else if (a >= 0 && M.bots[a].inverted) audio_play_var(SFX_KLAXON, SFXV_LOW);   /* the inverted tick (11.3) */
        else audio_play_var(SFX_COUNT, SFXV_SHORT);
        break;
      case EV_HAZ_TELL: audio_play_var(SFX_KLAXON, SFXV_TELL); break;
      case EV_HAZ_HIT: {
        int v = e->a < M.nbots ? e->a : -1;
        int ht = e->b < ARENAS[M.arena].nhaz ? ARENAS[M.arena].haz[e->b].type : -1;
        /* by damage: a pulverizer or a saw is the heavy clang, a flame tick or a bumper tap is a light one */
        int heavy = e->dmg >= 15;
        if (heavy) audio_play(SFX_HIT_HEAVY); else audio_play_var(SFX_HIT_LIGHT, SFXV_LOW);
        rumble(seat_pad(v), heavy ? 0.6f : 0.2f, heavy ? 0.6f : 0.3f, heavy ? 100 : 50);
        cam_shake(slot_seat(v), heavy ? J_HAZARD_SHAKE : J_HAZARD_SHAKE * 0.4f);
        if (e->dmg > 0.5f) number_all(e->x, e->y + 0.6f, e->z, dmg_int(e->dmg), C_RUST, e->dmg >= 15);
        if (e->zone == KO_PIT) sting_slot(v, PICK(ST_PIT), C_RUST);
        else if (ht == HZ_LASER) sting_slot(v, PICK(ST_DUCK), C_RUST);
        break; }
      case EV_OPS_WAKE:
        audio_play_var(SFX_HORN, SFXV_LOW); cam_shake_all(J_OPS_SHAKE);
        for (int p = 0; p < 4; p++) rumble(p, 0.5f, 0, 200);
        stinger_all(PICK(ST_OPS), C_HAZARD);
        break;
      case EV_OPS_LINE: speech_panel(-1, e->x, e->y, e->z, OPS_LINES[e->zone % LEN(OPS_LINES)]); break;
      case EV_OPS_LEAVE: audio_play(SFX_MENU_BACK); break;
      case EV_RED_TAG:
        audio_play_var(SFX_KLAXON, SFXV_TELL); rumble(seat_pad(a), 0.3f, 0.3f, 300);
        cam_shake(slot_seat(a), J_REDTAG_SHAKE); sting_slot(a, PICK(ST_REDTAG), C_RUST);
        break;
      case EV_PICKUP:
        audio_play_var(SFX_MENU_OK, SFXV_LOW);
        number_all(e->x, e->y + 0.6f, e->z, 6, C_LIVE, 0);
        if (e->zone == 1) sting_slot(a, PICK(ST_PICKUP), C_LIVE);
        break;
      case EV_NOT_VACUUM: if (e->zone == 1) { audio_play(SFX_MENU_BACK); sting_slot(a, ST_VACUUM, C_HAZARD); } break;
      case EV_TAUNT: audio_play_var(SFX_MENU_OK, SFXV_LOW); break;
      case EV_HILL_MOVE: audio_play(SFX_MENU_MOVE); { const char *s = PICK(ST_HILL); rect_t vp[4]; int n = 0; cam_current(vp, &n); for (int k = 0; k < n; k++) stinger(k, s, C_HAZARD); } break;
      case EV_RESPAWN: fx_dust(e->x, 0, e->z, J_LANDING_DUST); audio_play_var(SFX_HORN, SFXV_LOW); break;
      case EV_MINI_KO: {
        /* the KO band in the victim's and the attacker's viewports only (2.10) */
        int v = e->b < M.nbots ? e->b : -1;
        hit_log(e, BAND_KO);
        fx_hit(e, BAND_KO);
        cam_shake(slot_seat(v), HIT_BANDS[BAND_KO].shake);
        cam_shake(slot_seat(a), HIT_BANDS[BAND_KO].shake);
        audio_play(SFX_KO); audio_crowd(1.0f);
        rumble(seat_pad(v), 1, 1, 300);
        break; }
      case EV_CLOSING: audio_music(MUSIC_OVERTIME); break;
      default: break;
    }
    game_on_event(e);
  }
  M.nev = 0;
}

/* -- the HUD upload (13) ------------------------------------------- */

/* only the pixels the HUD changed go to the GPU. three.c re-uploads a texture whole on a version
 * bump (no sub-rect path), so the HUD textures are the cart's own GL objects (wc_init) and this
 * path writes them with glTexSubImage2D on a texture unit of its own, after which the renderer
 * forgets its bindings. The bands the writers touched this frame (hud_dirty_bands) are added to
 * every ring entry's pending set; on a dirty frame the next entry in the ring is brought up to date
 * (its pending bands, merged into rects, compared band by band against its shadow, only the bands
 * that differ sent, packed through the staging buffer) and the quad's material is pointed at it.
 * The ring keeps a write off the texture the GPU is still reading, which would make the driver wait
 * for it (a partial write to a busy texture stalls on Mesa V3D and on the rig's host alike). The
 * compare is what makes the gate real for the menus, which repaint a full backdrop every frame:
 * there only the tape and the cursor go up. A clean frame uploads nothing and keeps the texture on
 * show. The counters feed the [perf] line. */
static uint32_t hud_up_calls, hud_up_kb, hud_cmp_kb;

#define HUD_RECTS 16                      /* quads have more regions than halves: strips at two heights, both seams, the chip, the map */

/* GL rows [r0, r1) of columns [x0, x1) differ between the buffer and the shadow */
static int hud_rows_differ(const uint8_t *shadow, int x0, int x1, int r0, int r1) {
  int n = x1 - x0;
  for (int r = r0; r < r1; r++) {
    const uint32_t *a = (const uint32_t *)(hud_px + ((size_t)r * HUD_W + x0) * 4);
    const uint32_t *b = (const uint32_t *)(shadow + ((size_t)r * HUD_W + x0) * 4);
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 1;
  }
  return 0;
}

/* HUD rows [y0, y1) of columns [x0, x1): into the shadow and the staging buffer, then to the GPU, in
 * as many chunks as the staging buffer needs */
static void hud_send(uint8_t *shadow, int x0, int x1, int y0, int y1) {
  int w = x1 - x0, per = (int)(((size_t)HUD_W * HUD_STAGE_ROWS) / (size_t)w);   /* rows a chunk holds */
  for (int ty = HUD_H - y1, end = HUD_H - y0; ty < end; ty += per) {               /* rows are stored bottom up: HUD row y is GL row HUD_H - 1 - y */
    int h = end - ty < per ? end - ty : per;
    for (int r = 0; r < h; r++) {
      size_t src = ((size_t)(ty + r) * HUD_W + x0) * 4;
      memcpy(hud_stage + (size_t)r * w * 4, hud_px + src, (size_t)w * 4);
      memcpy(shadow + src, hud_px + src, (size_t)w * 4);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, x0, ty, w, h, GL_RGBA, GL_UNSIGNED_BYTE, hud_stage);
    hud_up_calls++; hud_up_kb += (uint32_t)((size_t)w * h * 4 / 1024);
  }
}

static void hud_upload(void) {
  uint8_t on[HUD_NBANDS]; int16_t bx0[HUD_NBANDS], bx1[HUD_NBANDS];
  if (!hud_dirty_bands(on, bx0, bx1)) return;
  for (int k = 0; k < HUD_RING; k++) for (int b = 0; b < HUD_NBANDS; b++) {
    if (!on[b]) continue;
    if (!hud_pend[k][b]) { hud_pend[k][b] = 1; hud_pend_x0[k][b] = bx0[b]; hud_pend_x1[k][b] = bx1[b]; }
    else { if (bx0[b] < hud_pend_x0[k][b]) hud_pend_x0[k][b] = bx0[b]; if (bx1[b] > hud_pend_x1[k][b]) hud_pend_x1[k][b] = bx1[b]; }
  }
  int k = (hud_cur + 1) % HUD_RING;
  uint8_t *shadow = hud_shadow[k];
  int x0[HUD_RECTS], y0[HUD_RECTS], x1[HUD_RECTS], y1[HUD_RECTS];
  int n = hud_band_rects(hud_pend[k], hud_pend_x0[k], hud_pend_x1[k], x0, y0, x1, y1, HUD_RECTS);
  memset(hud_pend[k], 0, sizeof hud_pend[k]);
#ifdef RTR_TRACE
  if (G.frame % 60 == 0) {
    char m[256]; int c = snprintf(m, sizeof m, "[hud] f=%u tex=%d rects=%d", G.frame, k, n);
    for (int i = 0; i < n && c < (int)sizeof m - 24; i++) c += snprintf(m + c, sizeof m - (size_t)c, " %d,%d-%d,%d", x0[i], y0[i], x1[i], y1[i]);
    wc_log(m, (unsigned)strlen(m));
  }
#endif
  unsigned name = t3_texture_gl_name(hud_tex[k]);
  if (name && n) {
    glActiveTexture(GL_TEXTURE0 + 31);                  /* three.c's scratch unit, never a sampler's */
    glBindTexture(GL_TEXTURE_2D, name);
    for (int i = 0; i < n; i++) {
      /* the rect's bands that differ from the shadow: when three quarters or more changed the rect goes
       * up whole, else as runs where up to two unchanged bands between changed ones ride along instead
       * of costing a call of their own */
      uint8_t changed[HUD_NBANDS]; int nb = 0, nc = 0;
      for (int yb = y0[i]; yb < y1[i] && nb < HUD_NBANDS; yb += HUD_BAND, nb++) {
        int ye = yb + HUD_BAND < y1[i] ? yb + HUD_BAND : y1[i];
        hud_cmp_kb += (uint32_t)((size_t)(x1[i] - x0[i]) * (ye - yb) * 4 / 1024);
        changed[nb] = (uint8_t)hud_rows_differ(shadow, x0[i], x1[i], HUD_H - ye, HUD_H - yb);
        nc += changed[nb];
      }
#ifdef RTR_TRACE
      if (G.frame % 60 == 0) { char m[64]; snprintf(m, sizeof m, "[hud] rect %d changed %d of %d bands", i, nc, nb); wc_log(m, (unsigned)strlen(m)); }
#endif
      if (!nc) continue;
      if (nc * 4 >= nb * 3) { hud_send(shadow, x0[i], x1[i], y0[i], y1[i]); continue; }
      int run0 = -1, run1 = -1, gap = 0;
      for (int b = 0; b < nb; b++) {
        int yb = y0[i] + b * HUD_BAND, ye = yb + HUD_BAND < y1[i] ? yb + HUD_BAND : y1[i];
        if (changed[b]) { if (run0 < 0) run0 = yb; run1 = ye; gap = 0; }
        else if (run0 >= 0 && ++gap > 2) { hud_send(shadow, x0[i], x1[i], run0, run1); run0 = -1; }
      }
      if (run0 >= 0) hud_send(shadow, x0[i], x1[i], run0, run1);
    }
    t3_renderer_reset_bindings(RTR_R);
  }
  if (k != hud_cur) { t3_material_set_map(hud_mat, hud_tex[k]); hud_cur = k; }
}

/* every 5 s: the host frame time, the render fallback and the HUD upload counts (item 23's perf log) */
static void perf_log(void) {
#ifdef RTR_TRACE
  if (G.frame % 60 == 0) { struct mallinfo mi = mallinfo(); char t[64]; snprintf(t, sizeof t, "[memt] f=%u ph=%d used=%u", G.frame, G.phase, (unsigned)mi.uordblks); wc_log(t, (unsigned)strlen(t)); }   /* the heap once a second */
#endif
  if (G.frame % 300 != 299) return;
  char m[128];
  snprintf(m, sizeof m, "[perf] f=%u host_ms=%.1f half=%d hud_calls=%u hud_kb=%u cmp_kb=%u", G.frame, G.frame_ms_avg, G.render_half, hud_up_calls, hud_up_kb, hud_cmp_kb);
  wc_log(m, (unsigned)strlen(m));
  hud_up_calls = hud_up_kb = hud_cmp_kb = 0;
}

/* -- the frame (4.1, 14.2) ----------------------------------------- */

/* once per phase change: the heap at every countdown, one line per match, so an evening's log
 * shows whether the rematch loop grows (the soak of HANDOFF.md reads these) */
static void on_phase_change(int from, int to) {
  (void)from;
#ifdef RTR_TRACE
  if (1) {                                 /* a trace build logs the heap at every phase change */
#else
  if (to == PH_COUNTDOWN) {
#endif
    struct mallinfo mi = mallinfo();
    char m[96];
    snprintf(m, sizeof m, "[mem] f=%u match=%u used=%u arena=%u sbrk=%u", G.frame, G.match_index, (unsigned)mi.uordblks, (unsigned)mi.arena, (unsigned)(uintptr_t)sbrk(0));
    wc_log(m, (unsigned)strlen(m));
  }
}

__attribute__((export_name("wc_render")))
void wc_render(void) {
  float dt = (float)(tm.delta_ms / 1000.0);
  if (dt > 0.05f) dt = 0.05f;
  if (dt <= 0) dt = 1.f / 60;

  read_pads(dt);
  /* seats are claimed inside game_update (lobby and garage only, 2.1) */
  game_update(dt);
  {
    static int last_phase = -1;
    if (G.phase != last_phase) { on_phase_change(last_phase, G.phase); last_phase = G.phase; }
  }

  /* the sim: fixed steps, the timescale scales the accumulator, never DT (20.5) */
  if (G.sim_on && M.nbots > 0) acc += dt * G.timescale; else acc = 0;
  int steps = 0;
  /* a 60 Hz host frame is exactly one step: the tolerance absorbs float rounding of delta_ms so
   * a scripted run never splits one frame's input across two steps (the replay lines up, 15.7) */
  while (acc >= DT - 1e-5f && steps < 4) {
    feed_inputs(); sim_step(&M); drain_events();
    acc -= DT; if (acc < 0) acc = 0; steps++;
  }
  if (steps == 4) acc = 0;                   /* a stall drops time, never fast-forwards */
  float alpha = acc / DT;
  if (G.sim_on && !G.attract && M.nbots > 0 && M.frame % 60 == 0 && M.frame != sim_logged_frame) {
    char m[48];
    snprintf(m, sizeof m, "[sim] f=%d h=%08x", M.frame, sim_hash(&M));
    wc_log(m, (unsigned)strlen(m));
    sim_logged_frame = M.frame;
    hits_summary();
  }

  world_tick(dt);
  world_sync(&M, alpha);
  cam_update(&M, dt);
  fx_update(dt, M.hitstop > 0);

  hud_dirty_reset();
  render_measure(tm.delta_ms > 0 ? (float)tm.delta_ms : dt * 1000.0f);
  int drawn = 1;
#ifdef RTR_FAKE_MENUS
  /* the menus probe: every fake menu scene for 120 frames each (tools/hudtest.sh is the native twin) */
  {
    static int scene = -1;
    int want = (int)(tm.frame / 120) % fake_menus_count();
    if (want != scene) { scene = want; fake_menus_setup(scene); }
    match_view_t none = { .n = 0 };
    render_frame(&none);
    hud_clear();
    fake_menus_draw((float)(tm.frame % 120) / 60.0f + 0.66f, dt);
    hud_dirty(0, 0, 1280, 720);
  }
  if (0) {}
  else
#endif
  {
    rect_t vp[4]; int n = 0;
    int layout = cam_current(vp, &n);
    if (!game_view_kind()) n = 0;
    match_view_t view = { .n = n, .layout = layout };
    for (int i = 0; i < n; i++) { view.vp[i] = vp[i]; view.seat_of[i] = cam_seat_of(i); view.cell[i] = -1; }
    drawn = render_frame(&view);
    if (drawn) {
      draw_set_views(vp, n, layout);
      draw_hud(dt);
    }
  }
  /* a skipped half-rate frame presents the previous frame unchanged (13) */
  if (drawn) {
    hud_upload();
    t3_renderer_render(RTR_R, hud_scene, hud_cam);
  }
  perf_log();

  uint32_t rate = host_info.audio_sample_rate ? host_info.audio_sample_rate : 48000;
  audio_render(audio_ring, AUDIO_CAP, &audio_cursor, rate, tm.delta_ms > 0 ? tm.delta_ms : dt * 1000.0);

  static int logged;
  const char *miss = t3_renderer_generated_missing(RTR_R);
  if (miss && *miss && logged < 3 && tm.frame % 120 == 0) { wc_log(miss, (unsigned)strlen(miss)); logged++; }
}
