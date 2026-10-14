/* game.c: evening state (spec 2.1, 10.22, 14.4): the globals, the save struct
 * with its crc, seat joining and leaving, and the settings rows. Free of the
 * renderer, the camera and audio so tools/hudtest.c links it natively; the
 * phase machine that drives all of those is game_flow.c. */
#include "game.h"
#include <string.h>
#include <stdio.h>

game_t G;
match_t M;
ai_t AIS[MAX_BOTS];
match_setup_t SETUP;
pad_input_t PADS[4];

/* -- save --------------------------------------------------------- */
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
  s->ai_level = AI_EASY; s->rules_seen = 0;      /* the practice bot starts gentle on a fresh cabinet (8.4); the garage toggle raises it */
  for (int i = 0; i < 4; i++) { s->scheme[i] = SCHEME_ARCADE; build_preset(&s->fav_build[i], i); s->fav_build[i].paint = (uint8_t)i; }
  s->crc = save_crc(s);
}

int save_valid(const save_t *s) { return s->magic == SAVE_MAGIC && s->version == SAVE_VERSION && s->crc == save_crc(s); }

void save_commit(void) { SAVE.magic = SAVE_MAGIC; SAVE.version = SAVE_VERSION; SAVE.crc = save_crc(&SAVE); }

/* -- seats -------------------------------------------------------- */
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

/* -- the evening -------------------------------------------------- */
void game_init(void) {
  memset(&G, 0, sizeof G);
  G.phase = PH_BOOT; G.timescale = 1.0f;
  for (int i = 0; i < 4; i++) { G.seats[i].pad = -1; G.seats[i].vote = -1; G.seats[i].rematch = -1; G.seats[i].slot = -1; PADS[i].seat = -1; }
  G.ko_victim = G.ko_victor = -1; G.controls_card_seat = -1; G.judge_winner = -1; G.bye_seat = -1;
  for (int i = 0; i < 4; i++) G.bracket[i] = -1;
  G.arena = AR_BAY; G.mode = MODE_DEATHMATCH;
}

/* -- settings rows (10.22): 0 SPLIT .. 10 RUMBLE; 11 STICKS and 12 RESET SAVE are game_flow.c's -- */
static int stepi(int v, int dir, int lo, int hi) { v += dir; return v < lo ? lo : v > hi ? hi : v; }

void game_setting_step(int row, int dir) {
  switch (row) {
    case 0: SAVE.split = (uint8_t)stepi(SAVE.split, dir, 0, 2); break;
    case 1: SAVE.one_camera = (uint8_t)stepi(SAVE.one_camera, dir, 0, 2); break;
    case 2: SAVE.fight_len = (uint8_t)stepi(SAVE.fight_len, dir, 0, 3); break;
    case 3: SAVE.comeback = (uint8_t)stepi(SAVE.comeback, dir, 0, 2); break;
    case 4: SAVE.shake = (uint8_t)stepi(SAVE.shake, dir, 0, 3); break;
    case 5: SAVE.hitstop = (uint8_t)(SAVE.hitstop ? 0 : 1); break;        /* ON, OFF: either direction toggles */
    case 6: SAVE.stinger_big = (uint8_t)(SAVE.stinger_big ? 0 : 1); break;
    case 7: SAVE.sfx = (uint8_t)stepi(SAVE.sfx, dir, 0, 10); break;
    case 8: SAVE.music = (uint8_t)stepi(SAVE.music, dir, 0, 10); break;
    case 9: SAVE.colourblind = (uint8_t)(SAVE.colourblind ? 0 : 1); break;
    case 10: SAVE.rumble = (uint8_t)(SAVE.rumble ? 0 : 1); break;
    default: return;
  }
  save_commit();
}

const char *game_setting_value(int row, char *buf, int n) {
  if (n <= 0) return buf;
  int v = 0;
  switch (row) {
    case 0: v = SAVE.split; break; case 1: v = SAVE.one_camera; break; case 2: v = FIGHT_LEN_S[SAVE.fight_len & 3]; break;
    case 3: v = SAVE.comeback; break; case 4: v = SAVE.shake; break; case 5: v = SAVE.hitstop; break;
    case 6: v = SAVE.stinger_big; break; case 7: v = SAVE.sfx; break; case 8: v = SAVE.music; break;
    case 9: v = SAVE.colourblind; break; case 10: v = SAVE.rumble; break; default: buf[0] = 0; return buf;
  }
  snprintf(buf, (size_t)n, "%d", v);
  return buf;
}

/* COMEBACK AUTO: ON in free-for-all and 2V2, OFF in any 1v1 including solo (2.8) */
int  game_comeback_effective(void) { return SAVE.comeback == SET_ON ? 1 : SAVE.comeback == SET_OFF ? 0 : G.players > 2; }
int  game_one_camera_effective(void) { return SAVE.one_camera == ONECAM_ALWAYS ? 1 : SAVE.one_camera == ONECAM_OFF ? 0 : (G.players == 2 && ARENAS[G.arena].one_camera_default); }
