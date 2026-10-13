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

#define AUDIO_CAP 8192

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
static t3_texture *hud_tex;
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
  hud_tex = t3_data_texture_new(HUD_W, HUD_H, hud_px);
  hud_tex->color_space = T3_SRGB_COLOR_SPACE;
  free(hud_px);                 /* the HUD now draws straight into the texture's texels */
  hud_px = hud_tex->pixels;
  t3_material *hm = t3_mesh_basic_material_new(0xffffff);
  t3_material_set_map(hm, hud_tex);
  hm->transparent = true; hm->depth_test = false; hm->depth_write = false;
  t3_mesh *quad = t3_mesh_new(t3_plane_geometry_new(2, 2, 1, 1), hm);
  t3_object_add(&hud_scene->base, quad);
  t3_release(quad); t3_release(hm);

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

static void rumble(int pad, float lo, float hi, int ms) {
  if (pad < 0 || !SAVE.rumble || ms <= 0 || G.attract) return;
  wc_pad_rumble((unsigned)pad, lo, hi, (unsigned)ms);
  char m[64];
  snprintf(m, sizeof m, "[rumble] pad %d %.2f %.2f %d", pad + 1, lo, hi, ms);
  wc_log(m, (unsigned)strlen(m));
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
  char m[80];
  snprintf(m, sizeof m, "[hit] f=%u sim=%d type=%d a=%d b=%d dmg=%.1f band=%d", G.frame, M.frame, e->type, e->a, e->b, e->dmg, band);
  wc_log(m, (unsigned)strlen(m));
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
        audio_play(SFX_CLASH); cam_shake(slot_seat(a), J_SPINWALL_SHAKE);
        number_all(e->x, e->y + 0.4f, e->z, dmg_int(e->dmg), C_RUST, 1);
        break;
      case EV_INVERT: audio_play_var(SFX_KLAXON, SFXV_SHORT); sting_slot(a, PICK(ST_INVERTED), slot_colour(e->b, C_BONE)); break;
      case EV_COUNT_TICK:
        if (a >= 0 && M.bots[a].deck_count == 60) { audio_play_var(SFX_KLAXON, SFXV_TELL); sting_slot(a, PICK(ST_DECK), C_RUST); }
        else audio_play_var(SFX_COUNT, SFXV_SHORT);
        break;
      case EV_HAZ_TELL: audio_play_var(SFX_KLAXON, SFXV_TELL); break;
      case EV_HAZ_HIT: {
        int v = e->a < M.nbots ? e->a : -1;
        int ht = e->b < ARENAS[M.arena].nhaz ? ARENAS[M.arena].haz[e->b].type : -1;
        audio_play(SFX_HIT_HEAVY); rumble(seat_pad(v), 0.6f, 0.6f, 100);
        cam_shake(slot_seat(v), J_HAZARD_SHAKE);
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

/* -- the frame (4.1, 14.2) ----------------------------------------- */

__attribute__((export_name("wc_render")))
void wc_render(void) {
  float dt = (float)(tm.delta_ms / 1000.0);
  if (dt > 0.05f) dt = 0.05f;
  if (dt <= 0) dt = 1.f / 60;

  read_pads(dt);
  /* seats are claimed inside game_update (lobby and garage only, 2.1) */
  game_update(dt);

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
    if (hud_dirty_get(0, 0, 0, 0)) hud_tex->version++;
    t3_renderer_render(RTR_R, hud_scene, hud_cam);
  }

  uint32_t rate = host_info.audio_sample_rate ? host_info.audio_sample_rate : 48000;
  audio_render(audio_ring, AUDIO_CAP, &audio_cursor, rate, tm.delta_ms > 0 ? tm.delta_ms : dt * 1000.0);

  static int logged;
  const char *miss = t3_renderer_generated_missing(RTR_R);
  if (miss && *miss && logged < 3 && tm.frame % 120 == 0) { wc_log(miss, (unsigned)strlen(miss)); logged++; }
}
