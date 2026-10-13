/* game.c: evening state and the phase machine (spec 10.1, 14.4). Owned by the
 * game agent (items 10, 11, 16). Stub: the globals, seat joining and the save
 * helpers are real; the phase machine is empty (G.phase stays PH_BOOT and
 * main.c runs its cube demo until this file drives phases). */
#include "game.h"
#include <string.h>
#include <stdio.h>

game_t G;
match_t M;
ai_t AIS[MAX_BOTS];
match_setup_t SETUP;
pad_input_t PADS[4];

/* ── save ───────────────────────────────────────────────────────── */
uint32_t save_crc(const save_t *s) {
  const uint8_t *p = (const uint8_t *)&s->split;
  size_t n = sizeof *s - ((size_t)p - (size_t)s);
  uint32_t c = 0xffffffffu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
  }
  return ~c;
}

void save_defaults(save_t *s) {
  memset(s, 0, sizeof *s);
  s->magic = SAVE_MAGIC; s->version = SAVE_VERSION;
  s->split = SPLIT_AUTO; s->one_camera = ONECAM_AUTO; s->fight_len = FL_75; s->comeback = SET_AUTO;
  s->shake = 2; s->hitstop = 1; s->stinger_big = 1; s->sfx = 8; s->music = 6; s->colourblind = 0; s->rumble = 1;
  s->ai_level = AI_MEDIUM; s->rules_seen = 0;
  for (int i = 0; i < 4; i++) { s->scheme[i] = SCHEME_ARCADE; build_preset(&s->fav_build[i], i); s->fav_build[i].paint = (uint8_t)i; }
  s->crc = save_crc(s);
}

int save_valid(const save_t *s) { return s->magic == SAVE_MAGIC && s->version == SAVE_VERSION && s->crc == save_crc(s); }

void save_commit(void) { SAVE.magic = SAVE_MAGIC; SAVE.version = SAVE_VERSION; SAVE.crc = save_crc(&SAVE); }

/* ── seats ──────────────────────────────────────────────────────── */
int game_seated(void) { int n = 0; for (int i = 0; i < 4; i++) if (G.seats[i].kind != SEAT_EMPTY) n++; return n; }
int game_humans(void) { int n = 0; for (int i = 0; i < 4; i++) if (G.seats[i].kind == SEAT_HUMAN) n++; return n; }

int game_seat_join(int pad) {
  if (pad < 0 || pad > 3 || PADS[pad].seat >= 0) return -1;
  for (int i = 0; i < 4; i++) if (G.seats[i].kind == SEAT_EMPTY) {
    seat_t *s = &G.seats[i];
    memset(s, 0, sizeof *s);
    s->kind = SEAT_HUMAN; s->pad = (int8_t)pad; s->vote = -1; s->rematch = -1; s->slot = -1; s->hazard_pick = -1;
    s->scheme = SAVE.scheme[i];
    build_preset(&s->build, i); s->build.paint = (uint8_t)i; s->preset = (uint8_t)i;
    bot_name(s->name, &s->build);
    PADS[pad].seat = i;
    return i;
  }
  return -1;
}

void game_seat_leave(int seat) {
  if (seat < 0 || seat > 3) return;
  if (G.seats[seat].pad >= 0) PADS[G.seats[seat].pad].seat = -1;
  memset(&G.seats[seat], 0, sizeof G.seats[seat]);
  G.seats[seat].pad = -1;
}

/* ── the evening ────────────────────────────────────────────────── */
void game_init(void) {
  memset(&G, 0, sizeof G);
  G.phase = PH_BOOT; G.timescale = 1.0f;
  for (int i = 0; i < 4; i++) { G.seats[i].pad = -1; G.seats[i].vote = -1; G.seats[i].rematch = -1; PADS[i].seat = -1; }
  G.ko_victim = G.ko_victor = -1;
  G.arena = AR_BAY; G.mode = MODE_DEATHMATCH;
}

void game_set_phase(int phase) { G.phase = phase; G.phase_t = 0; G.phase_frames = 0; }
void game_update(float dt) { G.phase_t += dt; G.phase_frames++; G.frame++; G.tape_phase += 30 * dt; }
void game_start_match(void) {}
void game_on_event(const event_t *e) { (void)e; }
void game_setting_step(int row, int dir) { (void)row; (void)dir; }
const char *game_setting_value(int row, char *buf, int n) { (void)row; if (n > 0) buf[0] = 0; return buf; }
int  game_comeback_effective(void) { return SAVE.comeback == SET_ON ? 1 : SAVE.comeback == SET_OFF ? 0 : G.players > 2; }
int  game_one_camera_effective(void) { return SAVE.one_camera == ONECAM_ALWAYS ? 1 : SAVE.one_camera == ONECAM_OFF ? 0 : (G.players == 2 && ARENAS[G.arena].one_camera_default); }
int  game_hazard_seat_of(int seat) { (void)seat; return -1; }
void game_record_results(void) {}
void game_attract_start(void) {}
void game_attract_stop(void) {}
void game_record_inputs(const pad_state_t in[4]) { (void)in; }
