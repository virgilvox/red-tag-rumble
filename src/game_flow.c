/* game_flow.c: the phase machine of an evening (spec 10.1, 10.2, 2.1, 2.3, 3.1).
 * Title and attract, lobby, garage, arena vote, countdown, fight, KO, judges,
 * results, rematch, settings, hall of fame, the rules flash and pause. It reads
 * PADS[] (written by main.c every frame), drives parts.h through the garage
 * rows, the world's turntables and scenes, the cameras, the music and the AI
 * seats, and starts every match through sim_init. main.c steps the sim while
 * G.sim_on is set and scales the accumulator by G.timescale.
 *
 * Seats: a pad claims a seat on an A, B, X, Y or Start edge in the lobby or the
 * garage (never on `connected`). On leaving the lobby the seats are packed so
 * the seated ones are 0..n-1; seat i is then match slot i and turntable i. */
#include "game.h"
#include "draw.h"
#include "world.h"
#include "camera.h"
#include "audio.h"
#include "fx.h"
#include "juice.h"
#include "content.h"
#include "wasmcart.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#define FLASH_STEPS     7
#define FLASH_CARD_S    2.6f        /* 7 cards in about 18 s (10.7 HOW IT WORKS 18 S) */
#define SNAP_FRAMES     18          /* the garage timer snaps to zero over 0.3 s when every seat is locked */
#define VOTE_ALL_WAIT   120         /* 2 s after every seat has voted */
#define VOTE_SHOW       60          /* the decided card lifts for 1 s */
#define VOTE_TIE_FLICK  30          /* a tie flickers over the tied cards for 0.5 s */
#define JUDGES_FRAMES   240         /* 1 s freeze plus the 3 s card */
#define RESULTS_FRAMES  360         /* 6 s, then the rematch menu */
#define RIG_COMBO       (IN_SELECT | IN_L3 | IN_R3)

static const int MODE_MIN_PLAYERS[MODE_COUNT] = { 1, 2, 3, 2, 4, 3 };

static float title_idle, attract_over_t;
static float lobby_ready_t, lobby_empty_t;
static int   garage_total, snap_left = -1;
static build_t ai_pick[4]; static int ai_frame[4];
static uint32_t grng = 0x2545f491u;            /* the garage and vote rng, reseeded from the host seed */
static int   vote_all_frame = -1, vote_decide_frame = -1, vote_tie;
static uint8_t voted[4];
static int   rs_prev[4];                        /* the right stick's x direction last frame, for edges */
static int   taunt_frame[MAX_BOTS]; static uint8_t taunted_hit[MAX_BOTS];
static int   streak_seat = -1; static uint32_t streak_n;
static int   last_count_s = -1;
static float b_hold[4];                         /* hall of fame: seconds B has been held per pad */

/* -- TOURNAMENT (2.2) ---------------------------------------------
 * Seats stay seats; a tournament match is a 1v1 between two of them in match slots 0
 * and 1 with bot_t.seat set to the real seat, so the cameras, strips and colours follow
 * the seat. Schedule: semi 1, semi 2, third place (the semi losers), final; with three
 * players a visible coin flip in the garage gives one seat the bye and the schedule is
 * semi, final. In the final the first eliminated player drives OPS and the second runs
 * its lifter and saw (with one eliminated player it does both). */
static int t_pair[2] = { -1, -1 };              /* the two seats in this tournament match */
static int t_match;                             /* index into the schedule */
static int t_done;                              /* the final has been played */
static int t_flip = -1;                         /* garage frames left in the bye coin flip */
static int t_flip_to = -1;                      /* where the coin lands */
static int t_loser[2] = { -1, -1 };             /* the semi losers, in order */
static int t_winner[2] = { -1, -1 };            /* the semi winners (the bye counts as a winner) */
static int ops_driver = -1, ops_gunner = -1;    /* the final's OPS crew, by seat */

static int tourney_on(void) { return G.mode == MODE_TOURNAMENT; }
static int tourney_matches(void) { return G.players >= 4 ? 4 : 2; }

/* lay the bracket out: four seats pair in order; three seats pair the two that are not the bye */
static void tourney_layout(void) {
  for (int i = 0; i < 4; i++) G.bracket[i] = -1;
  if (G.players >= 4) { for (int i = 0; i < 4; i++) G.bracket[i] = i; G.bye_seat = -1; return; }
  int k = 0;
  for (int i = 0; i < 3; i++) if (i != G.bye_seat) G.bracket[k++] = i;
  G.bracket[2] = G.bye_seat; G.bracket[3] = -1;
}

static void tourney_reset(void) {
  t_match = 0; t_done = 0; ops_driver = ops_gunner = -1;
  for (int i = 0; i < 2; i++) { t_loser[i] = t_winner[i] = -1; t_pair[i] = -1; }
  for (int i = 0; i < 4; i++) G.bracket_wins[i] = 0;
  G.tournament_round = 0; G.bye_seat = -1; t_flip = -1;
  if (!tourney_on()) { for (int i = 0; i < 4; i++) G.bracket[i] = -1; return; }
  if (G.players == 3) { t_flip_to = (int)rtr_rand_n(&grng, 3); G.bye_seat = 0; t_flip = 90; }
  tourney_layout();
}

/* the coin flip: the bye chip walks the three seats and lands after 1.5 s (2.2) */
static void tourney_flip_tick(void) {
  if (t_flip < 0) return;
  if (t_flip > 0 && t_flip % 8 == 0) { G.bye_seat = (G.bye_seat + 1) % 3; tourney_layout(); audio_play(SFX_MENU_MOVE); }
  if (t_flip == 0) { G.bye_seat = t_flip_to; tourney_layout(); G.bracket_wins[G.bye_seat] = 1; t_winner[1] = G.bye_seat; audio_play(SFX_MENU_OK); }
  t_flip--;
}

/* the seats of the next match in the schedule */
static void tourney_next_pair(void) {
  if (t_flip >= 0) { t_flip = 0; tourney_flip_tick(); }
  int four = G.players >= 4;
  switch (t_match) {
    case 0: t_pair[0] = G.bracket[0]; t_pair[1] = G.bracket[1]; G.tournament_round = 0; break;
    case 1:
      if (four) { t_pair[0] = G.bracket[2]; t_pair[1] = G.bracket[3]; G.tournament_round = 0; }
      else { t_pair[0] = t_winner[0]; t_pair[1] = t_winner[1]; G.tournament_round = 2; }
      break;
    case 2: t_pair[0] = t_loser[0]; t_pair[1] = t_loser[1]; G.tournament_round = 1; break;
    default: t_pair[0] = t_winner[0]; t_pair[1] = t_winner[1]; G.tournament_round = 2; break;
  }
  ops_driver = ops_gunner = -1;
  if (G.tournament_round == 2) {
    ops_driver = t_loser[0]; ops_gunner = t_loser[1] >= 0 ? t_loser[1] : t_loser[0];
    if (ops_driver >= 0 && G.seats[ops_driver].kind != SEAT_HUMAN) ops_driver = -1;
    if (ops_gunner >= 0 && G.seats[ops_gunner].kind != SEAT_HUMAN) ops_gunner = ops_driver;
    if (ops_driver < 0) ops_driver = ops_gunner;
  }
}

/* a tournament match ended: the bracket advances (the winner is a seat) */
static void tourney_record(int win_seat) {
  int lose_seat = win_seat == t_pair[0] ? t_pair[1] : t_pair[0];
  if (win_seat < 0) { win_seat = t_pair[0]; lose_seat = t_pair[1]; }   /* a DRAW goes to the higher seed */
  int four = G.players >= 4;
  if (t_match == 0 || (four && t_match == 1)) {
    t_winner[t_match] = win_seat; t_loser[t_match] = lose_seat;
    G.bracket_wins[win_seat]++;
  } else if (four && t_match == 2) {
    /* third place: the evening tally counts it, the bracket does not advance */
  } else { G.bracket_wins[win_seat]++; t_done = 1; }
  t_match++;
  if (t_match >= tourney_matches()) t_done = 1;
}

/* -- hazard seats (2.10) ------------------------------------------
 * An eliminated player in LAST BOT STANDING, a free-for-all DEATHMATCH or 2V2 gets a
 * hazard seat 1.5 s after the mini-KO (the stinger plays first); a human sitting out a
 * tournament match drives one from the start. */
static int out_frame[4] = { -1, -1, -1, -1 };    /* render frame the seat went out, -1 when live */
static int seat_slot(int seat) { for (int i = 0; i < M.nbots && i < 4; i++) if (M.bots[i].seat == seat && !M.bots[i].is_ops) return i; return -1; }
static int hazard_mode(void) {
  if (G.mode == MODE_LAST_BOT || G.mode == MODE_2V2) return 1;
  if (G.mode == MODE_DEATHMATCH) return M.players >= 3;
  return G.mode == MODE_TOURNAMENT;
}

static void logf_(const char *s) { wc_log(s, (unsigned)strlen(s)); }

/* -- small helpers ----------------------------------------------- */
static int clampi_(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int wrap(int v, int n) { return n <= 0 ? 0 : ((v % n) + n) % n; }
static int edge(int p, int bits) { return PADS[p].edge & bits; }
static int held(int p, int bits) { return PADS[p].cur.buttons & bits; }
static int face_edge(int p) { return edge(p, IN_A | IN_B | IN_X | IN_Y | IN_START); }
/* the right stick as a stepper: -1 or 1 on the frame it crosses 0.5 */
static int rs_step(int p) {
  int d = PADS[p].cur.rx < -16384 ? -1 : PADS[p].cur.rx > 16384 ? 1 : 0;
  int e = d != rs_prev[p] ? d : 0;
  return e;
}
static int seat_of_pad(int p) { return PADS[p].seat; }
static int human_count(void) { return game_humans(); }

int game_slot_pad(int slot) {
  if (G.attract || slot < 0 || slot > 3) return -1;
  int seat = slot < M.nbots ? M.bots[slot].seat : slot;
  if (seat < 0 || seat > 3) return -1;
  const seat_t *s = &G.seats[seat];
  return s->kind == SEAT_HUMAN ? s->pad : -1;
}

void game_set_phase(int phase) {
  G.phase = phase; G.phase_t = 0; G.phase_frames = 0;
  char m[40]; snprintf(m, sizeof m, "[phase] %d at frame %u", phase, G.frame);
  logf_(m);
}

int game_view_kind(void) {
  switch (G.phase) {
    case PH_BOOT: case PH_VOTE: return 0;
    case PH_TITLE: case PH_HALL: case PH_RULES: return G.attract;
    case PH_SETTINGS: return G.attract || G.settings_from_pause;
    case PH_LOBBY: return game_seated() > 0;
    default: return 1;
  }
}

/* -- the world: bots on turntables, scenes ----------------------- */
static void hide_bots(int from) { for (int i = from; i < MAX_BOTS; i++) world_bot_visible(i, 0); }

static void bars_now(const build_t *b, float out[3]) { build_bars(b, &out[0], &out[1], &out[2]); }

static void seat_show(int s) {
  seat_t *st = &G.seats[s];
  world_bot_build(s, &st->build, seat_colour(s));
  world_bot_visible(s, 1);
  float x, y, z; world_turntable_pos(s, &x, &y, &z);
  world_bot_pose(s, x, y, z, 0, 0);
}

static void lobby_scene(void) {
  world_attach_garage();
  int mask = 0;
  for (int s = 0; s < 4; s++) {
    if (G.seats[s].kind != SEAT_EMPTY) { seat_show(s); mask |= 1 << s; }
    else world_bot_visible(s, 0);
  }
  hide_bots(4);
  cam_set_lobby(mask);
}

/* -- attract (8.6, 10.7) ----------------------------------------- */
void game_attract_start(void) {
  match_setup_t s;
  ai_attract_setup(&s, AIS, G.frame + G.seed);
  sim_init(&M, &s); M.nev = 0;
  world_attach_arena(AR_BAY);
  for (int i = 0; i < 2; i++) { world_bot_build(i, &s.builds[i], seat_colour(i)); world_bot_visible(i, 1); }
  world_bot_visible(2, 0); world_bot_visible(3, 0);
  if (M.nbots > OPS_SLOT && M.bots[OPS_SLOT].is_ops) { world_bot_build_ops(OPS_SLOT); world_bot_visible(OPS_SLOT, 1); }
  cam_set_attract();
  G.attract = 1; G.sim_on = 1; G.timescale = 1; attract_over_t = 0;
}

void game_attract_stop(void) {
  if (!G.attract) return;
  G.attract = 0; G.sim_on = 0; M.nbots = 0; M.nev = 0;
  for (int i = 0; i < 4; i++) audio_whine(i, 0, 0);
  hide_bots(0);
}

/* -- seats ------------------------------------------------------- */
static void seat_reset_garage(seat_t *s) {
  s->locked = 0; s->ready = 0; s->row = 0; s->lock_hold = 0; s->leave_hold = 0; s->slot_machine_t = 0;
  s->bars_from_t = 0; bars_now(&s->build, s->bars_new); memcpy(s->bars_old, s->bars_new, sizeof s->bars_old);
}

static int join_seat(int pad) {
  int s = game_seat_join(pad);
  if (s < 0) return -1;
  seat_t *st = &G.seats[s];
  /* the favourite build from the cabinet, else the seat's preset (3.9) */
  build_t fav = SAVE.fav_build[s];
  if (SAVE.matches > 0 && build_legal(&fav, 2)) st->build = fav;
  bot_name(st->name, &st->build);
  st->preset = (uint8_t)build_preset_of(&st->build);
  st->ai_level = SAVE.ai_level;
  seat_reset_garage(st);
  PADS[pad].edge = 0;                         /* the joining edge does nothing else this frame */
  audio_play(SFX_MENU_OK);
  return s;
}

static int add_ai_seat(int level) {
  for (int i = 0; i < 4; i++) if (G.seats[i].kind == SEAT_EMPTY) {
    seat_t *s = &G.seats[i];
    memset(s, 0, sizeof *s);
    s->kind = SEAT_AI; s->pad = -1; s->vote = -1; s->rematch = -1; s->slot = -1; s->hazard_pick = -1;
    s->ai_level = (uint8_t)level; s->scheme = SCHEME_ARCADE;
    build_preset(&s->build, i); s->build.paint = (uint8_t)i; s->preset = (uint8_t)i;
    bot_name(s->name, &s->build);
    seat_reset_garage(s);
    return i;
  }
  return -1;
}

/* pack the seats so the seated ones are 0..n-1 (seat i is match slot i and turntable i) */
static void pack_seats(void) {
  seat_t tmp[4]; int n = 0;
  for (int i = 0; i < 4; i++) if (G.seats[i].kind != SEAT_EMPTY) tmp[n++] = G.seats[i];
  for (int i = 0; i < 4; i++) {
    if (i < n) G.seats[i] = tmp[i];
    else { memset(&G.seats[i], 0, sizeof G.seats[i]); G.seats[i].pad = -1; G.seats[i].vote = -1; G.seats[i].rematch = -1; G.seats[i].slot = -1; }
  }
  for (int p = 0; p < 4; p++) PADS[p].seat = -1;
  for (int i = 0; i < n; i++) { G.seats[i].slot = i; if (G.seats[i].kind == SEAT_HUMAN && G.seats[i].pad >= 0) PADS[(int)G.seats[i].pad].seat = i; }
}

static void drop_ai_seats(void) {
  for (int i = 0; i < 4; i++) if (G.seats[i].kind == SEAT_AI) { memset(&G.seats[i], 0, sizeof G.seats[i]); G.seats[i].pad = -1; G.seats[i].vote = -1; G.seats[i].rematch = -1; G.seats[i].slot = -1; }
}

/* -- phase entries ----------------------------------------------- */
static void go_title(void) {
  game_set_phase(PH_TITLE);
  G.title_row = 0; title_idle = 0; G.solo = 0;
  G.timescale = 1; G.paused = 0;
  audio_music(MUSIC_GARAGE);
}

static void go_lobby(void) {
  game_attract_stop();
  G.sim_on = 0; G.timescale = 1; G.paused = 0; G.solo = 0;
  drop_ai_seats();
  for (int i = 0; i < 4; i++) { G.seats[i].ready = 0; G.seats[i].leave_hold = 0; }
  M.nbots = 0;
  lobby_ready_t = 0; lobby_empty_t = 0; G.lobby_row = 0;
  G.lobby_mode_cursor = G.mode;
  t_done = 1;
  game_set_phase(PH_LOBBY);
  lobby_scene();
  audio_music(MUSIC_GARAGE);
}

static void garage_hint(seat_t *s, int seat) {
  s->hint[0] = 0;
  int slot = seat_slot(seat);
  if (slot < 0 || slot >= M.nbots || M.bots[slot].hp_max <= 0) return;
  const bot_t *b = &M.bots[slot];
  int lost = M.winner != slot;
  const bot_t *k = G.ko_victor >= 0 && G.ko_victor < M.nbots ? &M.bots[G.ko_victor] : 0;
  int ate_spinner = lost && k && (k->build.weapon == WP_HORIZ || k->build.weapon == WP_DRUM || k->build.drive == DR_MELTY);
  int cost = build_cost(&s->build), budget = build_budget(&s->build);
  if (b->flips >= 3) snprintf(s->hint, sizeof s->hint, HINT_FLIPPED, b->flips);
  else if (ate_spinner && (s->build.armour == AR_NONE || s->build.armour == AR_PLY)) snprintf(s->hint, sizeof s->hint, "%s", HINT_SPINNER);
  else if (b->hazard_hits >= 2) snprintf(s->hint, sizeof s->hint, "%s", HINT_HAZARD);
  else if (lost && b->ko_how == KO_COUNT && !b->alive) snprintf(s->hint, sizeof s->hint, "%s", HINT_COUNTED);
  else if (cost < 14) snprintf(s->hint, sizeof s->hint, HINT_CHEAP, budget - cost);
  else if (!lost) snprintf(s->hint, sizeof s->hint, "%s", HINT_WON);
  else snprintf(s->hint, sizeof s->hint, "%s", HINT_TIPS[rtr_rand_n(&grng, LEN(HINT_TIPS))]);
}

static const build_t *human_last_build(void) {
  for (int i = 0; i < 4; i++) if (G.seats[i].kind == SEAT_HUMAN) return &G.seats[i].build;
  return &SAVE.fav_build[0];
}

static void go_garage(int rebuild) {
  game_attract_stop();
  if (!rebuild) {
    pack_seats();
    int humans = human_count();
    if (humans == 1 && game_seated() == 1) { add_ai_seat(SAVE.ai_level); G.solo = 1; }
    if (G.fill_with_bot) while (game_seated() < 4 && add_ai_seat(SAVE.ai_level) >= 0) {}
    pack_seats();
    G.players = (uint8_t)game_seated();
    int mode = G.lobby_mode_cursor;
    if (G.players < MODE_MIN_PLAYERS[mode] || (mode == MODE_2V2 && G.players != 4)) mode = MODE_DEATHMATCH;
    G.mode = (uint8_t)mode;
    if (G.rig_park) for (int i = 0; i < 4; i++) if (G.seats[i].kind == SEAT_AI) G.seats[i].ai_level = AI_EASY;
  }
  G.sim_on = 0; G.timescale = 1; G.paused = 0; G.controls_card_seat = -1;
  if (!rebuild || t_done || !tourney_on()) tourney_reset();
  game_set_phase(PH_GARAGE);
  garage_total = rebuild ? REBUILD_FRAMES : GARAGE_FRAMES;
  snap_left = -1;
  world_attach_garage();
  for (int s = 0; s < 4; s++) {
    seat_t *st = &G.seats[s];
    if (st->kind == SEAT_EMPTY) { world_bot_visible(s, 0); continue; }
    if (rebuild && st->kind == SEAT_HUMAN) garage_hint(st, s); else st->hint[0] = 0;
    seat_reset_garage(st);
    st->join_slide = 0;
    if (st->kind == SEAT_AI) {
      ai_pick_build(&ai_pick[s], st->ai_level, human_last_build(), SAVE.habits, &grng);
      if (!build_legal(&ai_pick[s], G.players)) build_preset(&ai_pick[s], PRE_LAUNCH);
      ai_pick[s].paint = (uint8_t)s;
      ai_frame[s] = 0;
      if (G.rig_park) { build_preset(&ai_pick[s], PRE_LAUNCH); ai_pick[s].paint = (uint8_t)s; st->build = ai_pick[s]; st->locked = 1; }   /* the drive scripts' parked practice bot (balance drive_setup) */
      bot_name(st->name, &st->build);
      bars_now(&st->build, st->bars_new); memcpy(st->bars_old, st->bars_new, sizeof st->bars_old);
    }
    seat_show(s);
    world_turntable_lock(s, st->locked);
  }
  hide_bots(4);
  cam_set_garage(G.players);
  audio_music(MUSIC_GARAGE);
}

static void go_vote(void) {
  G.sim_on = 0; G.timescale = 1;
  game_set_phase(PH_VOTE);
  /* every seat starts with a vote on the last arena (THE BAY on the first fight), so the vote ends
   * 2 s after it opens unless someone is still moving; the 15.5 scripts rely on that (4p leaves it alone) */
  for (int s = 0; s < 4; s++) { G.seats[s].vote = G.seats[s].kind != SEAT_EMPTY ? G.arena : -1; voted[s] = G.seats[s].kind != SEAT_EMPTY; }
  G.vote_leader = G.arena; G.vote_decided = 0; G.vote_decided_t = -1; G.vote_one_camera = SAVE.one_camera;
  vote_all_frame = -1; vote_decide_frame = -1; vote_tie = 0;
}

/* build the setup from the seats and settings, init the sim and the scene (the countdown shows the spawn) */
void game_start_match(void) {
  game_attract_stop();
  memset(&SETUP, 0, sizeof SETUP);
  SETUP.mode = G.mode; SETUP.arena = G.arena; SETUP.players = G.players;
  int seat_of_slot[4] = { 0, 1, 2, 3 };
  if (tourney_on()) {
    if (t_done) tourney_reset();                 /* SAME BOTS or NEW ARENA after a final starts a new bracket */
    tourney_next_pair();
    SETUP.players = 2; seat_of_slot[0] = t_pair[0]; seat_of_slot[1] = t_pair[1];
  }
  G.comeback_eff = (uint8_t)game_comeback_effective();
  SETUP.comeback = G.comeback_eff;
  SETUP.fight_len_s = G.rig_park ? 90 : FIGHT_LEN_S[SAVE.fight_len & 3];
  SETUP.seed = G.rig_park ? G.seed : G.seed * 2654435761u + G.match_index * 0x9e3779b9u + 1;   /* under the rig the drive scripts replay with the host seed */
  SETUP.hitstop_off = SAVE.hitstop ? 0 : 1;
  SETUP.park = (uint8_t)G.rig_park;
  for (int i = 0; i < SETUP.players && i < 4; i++) {
    const seat_t *s = &G.seats[seat_of_slot[i]];
    SETUP.builds[i] = s->build;
    SETUP.is_ai[i] = s->kind == SEAT_AI;
    SETUP.ai_level[i] = s->kind == SEAT_AI ? s->ai_level : 0;
    SETUP.team[i] = G.mode == MODE_2V2 ? (uint8_t)(i & 1) : 0;   /* 2V2: seats 1 and 3 against seats 2 and 4 (2.2) */
    SETUP.scheme[i] = s->scheme;
    SETUP.pad[i] = s->kind == SEAT_HUMAN && s->pad >= 0 ? (uint8_t)(s->pad + 1) : 0;
  }
  G.match_index++;
  sim_init(&M, &SETUP); M.nev = 0;
  for (int i = 0; i < 4; i++) { G.seats[i].slot = -1; out_frame[i] = -1; }
  for (int i = 0; i < SETUP.players && i < 4; i++) {
    int seat = seat_of_slot[i];
    M.bots[i].seat = (uint8_t)seat;             /* a tournament pair keeps its seats' colours and views */
    G.seats[seat].slot = i;
    if (G.seats[seat].kind == SEAT_AI) { ai_init(&AIS[i], G.seats[seat].ai_level, i); ai_bind(&AIS[i], i); }
  }
  /* humans sitting out a tournament match drive hazard seats from the start */
  if (tourney_on()) for (int s = 0; s < G.players && s < 4; s++) if (G.seats[s].slot < 0 && G.seats[s].kind == SEAT_HUMAN) out_frame[s] = (int)G.frame - 90;
  if (tourney_on() && G.tournament_round == 2 && ops_driver >= 0) sim_ops_driven(&M, ops_driver, ops_gunner);
  world_attach_arena(G.arena);
  for (int i = 0; i < 4; i++) {
    if (i < SETUP.players) { world_bot_build(i, &SETUP.builds[i], seat_colour(seat_of_slot[i])); world_bot_visible(i, 1); }
    else world_bot_visible(i, 0);
  }
  if (M.nbots > OPS_SLOT && M.bots[OPS_SLOT].is_ops) { world_bot_build_ops(OPS_SLOT); world_bot_visible(OPS_SLOT, 1); }
  else world_bot_visible(OPS_SLOT, 0);
  G.one_camera_eff = game_one_camera_effective();
  cam_set_match(&M, SAVE.one_camera, SAVE.split, &ARENAS[G.arena]);
  cam_set_shake_scale(SAVE.shake);
  G.ko_victim = G.ko_victor = -1; G.miniko_frames = 0; G.judge_winner = -1; G.judges_rule = 0;
  memset(taunt_frame, 0, sizeof taunt_frame); memset(taunted_hit, 0, sizeof taunted_hit);
  last_count_s = -1;
}

static void go_countdown(void) {
  game_start_match();
  game_set_phase(PH_COUNTDOWN);
  G.countdown_n = 3; G.sim_on = 0; G.timescale = 1;
  audio_play(SFX_COUNT);
  audio_music(MUSIC_OFF);
}

static void go_fight(void) {
  game_set_phase(PH_FIGHT);
  G.sim_on = 1; G.timescale = 1; G.paused = 0;
  stinger_all(ST_FIGHT[0], C_HAZARD);
  audio_play(SFX_HORN);
  audio_music(MUSIC_ARENA);
  audio_crowd(0.6f);
  char line[512];
  sim_setup_line(&SETUP, (int)G.frame - 1, line, sizeof line);   /* the shot frame of the first step */
  logf_(line);
}

static void go_results(void);

static void go_ko(void) {
  game_set_phase(PH_KO);
  int v = G.ko_victim, w = M.winner < M.nbots ? M.winner : G.ko_victor;
  if (v < 0 || v >= M.nbots) { for (int i = 0; i < M.nbots; i++) if (!M.bots[i].is_ops && !M.bots[i].alive) v = i; }
  if (v < 0) v = 0;
  G.ko_victim = v; G.ko_victor = w < M.nbots ? w : -1;
  G.ko_frames = 0; G.timescale = J_KO_TIMESCALE; G.miniko_frames = 0;
  cam_ko(G.ko_victim, G.ko_victor);
  audio_slowmo(1);
  audio_crowd(1.0f);
}

static void go_judges(void) {
  game_set_phase(PH_JUDGES);
  G.timescale = 0;
  float d[MAX_BOTS], a[MAX_BOTS], c[MAX_BOTS]; int rule = 0;
  memset(d, 0, sizeof d); memset(a, 0, sizeof a); memset(c, 0, sizeof c);
  int w = sim_judges(&M, d, a, c, &rule);
  for (int i = 0; i < MAX_BOTS; i++) { G.judge_bars[0][i] = d[i]; G.judge_bars[1][i] = a[i]; G.judge_bars[2][i] = c[i]; }
  G.judge_winner = w; G.judges_rule = rule;
  G.ko_victor = w; G.ko_victim = -1;
  cam_time();
  stinger_all(ST_TIME[0], C_RUST);
  audio_play(SFX_HORN);
}

/* awards (16): one per player, the first rule in priority order it meets */
static int arg_max(int (*val)(const bot_t *, float *), int slot) {
  float best = 0, mine = 0; int who = -1;
  for (int i = 0; i < M.players && i < 4; i++) { float v; if (!val(&M.bots[i], &v)) continue; if (v > best) { best = v; who = i; } if (i == slot) mine = v; }
  return who == slot && mine > 0;
}
static int v_flips(const bot_t *b, float *v) { *v = b->flips_landed; return 1; }
static int v_spin(const bot_t *b, float *v) { *v = b->spinner_dealt; return 1; }
static int v_push(const bot_t *b, float *v) { *v = b->push_frames; return 1; }
static int v_pick(const bot_t *b, float *v) { *v = b->pickups; return 1; }
static int v_burn(const bot_t *b, float *v) { *v = b->burn_dealt; return 1; }
static int v_haz(const bot_t *b, float *v) { *v = b->hazard_taken; return 1; }
static int v_wall(const bot_t *b, float *v) { *v = b->wall_hits; return 1; }

static int award_for(int i) {
  const bot_t *b = &M.bots[i];
  int won = M.winner == i, timed = M.win_how == WIN_JUDGES;
  if (won && b->build.weapon == WP_WEDGE) return AW_DOORSTOP;
  if (arg_max(v_flips, i)) return AW_GRAVITY;
  if (arg_max(v_spin, i)) return AW_SPINNER;
  if (won && b->red_tag_used) return AW_REDTAG;
  if (!b->alive && (b->ko_how == KO_PIT || b->ko_how == KO_ROOF)) return AW_LEFT;
  if (taunted_hit[i]) return AW_TAUNTED;
  if (arg_max(v_push, i)) return AW_FLOOR;
  if (arg_max(v_pick, i)) return AW_JANITOR;
  if (arg_max(v_burn, i)) {
    int least = 1; for (int k = 0; k < M.players && k < 4; k++) if (k != i && M.bots[k].dealt < b->dealt) least = 0;
    if (least) return AW_MILD;
  }
  if (arg_max(v_haz, i)) return AW_SIGN;
  if (arg_max(v_wall, i)) return AW_WALL;
  if (timed && b->alive) {
    float sp, h, t, slowest = 9; build_bars(&b->build, &sp, &h, &t);
    for (int k = 0; k < M.players && k < 4; k++) { float s2, h2, t2; build_bars(&M.bots[k].build, &s2, &h2, &t2); if (s2 < slowest) slowest = s2; }
    if (sp <= slowest) return AW_ARRIVED;
  }
  if (b->alive && build_cost(&b->build) < 12) return AW_SHELF;
  return AW_ATTENDED;
}

void game_record_results(void) {
  int wslot = M.winner < M.nbots ? M.winner : -1;
  int w = wslot >= 0 && M.bots[wslot].seat < 4 ? M.bots[wslot].seat : -1;
  for (int i = 0; i < MAX_BOTS; i++) { G.award[i] = AW_ATTENDED; G.place[i] = 0; }
  for (int i = 0; i < M.players && i < 4; i++) { G.award[i] = award_for(i); G.place[i] = M.bots[i].place; }
  SAVE.matches++;
  if (w >= 0 && w < 4) {
    seat_t *ws = &G.seats[w];
    ws->wins++;
    int p = build_preset_of(&M.bots[wslot].build);
    SAVE.wins_by_preset[p < PRE_RANDOM ? p : 6]++;
    if (M.win_how < 6) {
      SAVE.ko_by_cause[M.win_how]++;
      uint32_t ms = (uint32_t)M.frame * 1000u / 60u;
      if (SAVE.fastest_ko_ms == 0 || ms < SAVE.fastest_ko_ms) {
        SAVE.fastest_ko_ms = ms; SAVE.fastest_ko_seat = (uint8_t)w;
        snprintf(SAVE.fastest_ko_name, sizeof SAVE.fastest_ko_name, "%s", M.bots[wslot].name);
      }
    }
    if (streak_seat == w) streak_n++; else { streak_seat = w; streak_n = 1; }
    if (streak_n > SAVE.longest_streak) {
      SAVE.longest_streak = streak_n; SAVE.streak_seat = (uint8_t)w;
      snprintf(SAVE.streak_name, sizeof SAVE.streak_name, "%s", M.bots[wslot].name);
    }
    if (ws->wins > SAVE.best_evening_wins) { SAVE.best_evening_wins = ws->wins; SAVE.best_seat = (uint8_t)w; }
  } else { streak_seat = -1; streak_n = 0; }
  /* the HARD AI learns from the human it fought (8.4) */
  int human = -1, ai = 0;
  for (int i = 0; i < M.players && i < 4; i++) { int st = M.bots[i].seat & 3; if (G.seats[st].kind == SEAT_HUMAN && human < 0) human = i; if (G.seats[st].kind == SEAT_AI) ai = 1; }
  if (ai && human >= 0) ai_learn(SAVE.habits, &M, human);
  if (tourney_on()) tourney_record(w);
  save_commit();
  G.matches_this_evening++;
  G.evening_started = 1;
}

static void go_results(void) {
  game_set_phase(PH_RESULTS);
  G.sim_on = 0; G.timescale = 1; G.paused = 0; G.miniko_frames = 0;
  audio_slowmo(0); audio_duck(1); audio_tension(0);
  for (int i = 0; i < 4; i++) audio_whine(i, 0, 0);
  game_record_results();
  audio_music(MUSIC_GARAGE);
  /* the winner's bot on its turntable in the results viewport (10.21) */
  int w = M.winner < M.nbots && M.bots[M.winner].seat < 4 ? M.bots[M.winner].seat : 0;
  world_attach_garage();
  hide_bots(0);
  if (G.mode != MODE_TOURNAMENT && w < 4 && G.seats[w].kind != SEAT_EMPTY) {
    seat_show(w);
    world_turntable_lock(w, 0);
    cam_set_results(w);
  } else cam_set_results(-1);
}

static void go_rematch(void) {
  game_set_phase(PH_REMATCH);
  for (int s = 0; s < 4; s++) { G.seats[s].rematch = G.seats[s].kind != SEAT_EMPTY ? RM_REBUILD : -1; G.rematch_votes[s] = G.seats[s].kind == SEAT_AI; }
  G.rematch_choice = RM_REBUILD;
}

/* -- the boot screen --------------------------------------------- */
static void boot_update(void) {
  /* the first frame shows the boot screen; the second compiles every program; then the title */
  if (G.phase_frames == 2) world_compile_all();
  if (G.phase_frames >= 8) go_title();
}

/* -- title (10.7) ------------------------------------------------ */
static void title_select(int p) {
  audio_play(SFX_MENU_OK);
  switch (G.title_row) {
    case TR_FIGHT: go_lobby(); break;
    case TR_SOLO: {
      go_lobby();
      join_seat(p);
      G.lobby_mode_cursor = MODE_DEATHMATCH;
      go_garage(0);
      break; }
    case TR_RULES: G.flash_from = PH_TITLE; G.flash_step = 0; G.flash_t = 0; game_set_phase(PH_RULES); break;
    case TR_HALL: G.hall_wipe_hold = 0; game_set_phase(PH_HALL); break;
    case TR_SETTINGS: G.settings_row = 0; G.settings_from_pause = 0; G.settings_saved_t = 0; game_set_phase(PH_SETTINGS); break;
  }
}

static void title_update(float dt) {
  int any = 0;
  if (G.title_owner_t > 0) G.title_owner_t -= dt;
  for (int p = 0; p < 4; p++) {
    if (PADS[p].any) any = 1;
    if (G.title_owner_t > 0 && G.title_owner != p && (PADS[p].nav_y || PADS[p].edge)) continue;
    if (PADS[p].nav_y) { G.title_row = wrap(G.title_row + PADS[p].nav_y, TR_COUNT); G.title_owner = p; G.title_owner_t = 2; audio_play(SFX_MENU_MOVE); }
    if (edge(p, IN_A | IN_START)) { G.title_owner = p; title_select(p); return; }
  }
  title_idle = any ? 0 : title_idle + dt;
  if (!G.attract && title_idle >= ATTRACT_DELAY_S && G.phase_t >= ATTRACT_DELAY_S) game_attract_start();
  if (G.attract && title_idle >= TITLE_IDLE_S) { game_attract_stop(); game_attract_start(); title_idle = 0; }
}

/* the attract keeps running behind the title, settings, hall and rules; a finished bout restarts */
static void attract_update(float dt) {
  if (!G.attract) return;
  if (M.over) { attract_over_t += dt; if (attract_over_t > 3.0f) { game_attract_stop(); game_attract_start(); } }
  cam_attract(G.phase_t);
}

/* -- lobby (10.8) ------------------------------------------------ */
static void lobby_update(float dt) {
  int changed = 0;
  /* joins: an unseated pad on any face button edge */
  for (int p = 0; p < 4; p++) if (PADS[p].seat < 0 && face_edge(p) && game_seated() < 4) { if (join_seat(p) >= 0) changed = 1; }
  int go = 0;
  for (int p = 0; p < 4; p++) {
    int s = seat_of_pad(p);
    if (s < 0) {
      /* an unseated pad moves the FILL WITH BOT stepper with its stick */
      if (PADS[p].nav_x) { G.fill_with_bot = PADS[p].nav_x > 0; audio_play(SFX_MENU_MOVE); }
      continue;
    }
    seat_t *st = &G.seats[s];
    int rs = rs_step(p);
    if (held(p, IN_Y) && rs) { G.fill_with_bot = rs > 0; audio_play(SFX_MENU_MOVE); }
    if (PADS[p].nav_y) { G.lobby_row = clampi_(G.lobby_row + PADS[p].nav_y, 0, 3); audio_play(SFX_MENU_MOVE); }
    if (PADS[p].nav_x) {
      int dx = PADS[p].nav_x;
      if (G.lobby_row == 0) G.lobby_mode_cursor = wrap(G.lobby_mode_cursor + dx, MODE_COUNT);
      else {
        uint8_t *v = G.lobby_row == 1 ? &SAVE.one_camera : G.lobby_row == 2 ? &SAVE.split : &SAVE.comeback;
        *v = (uint8_t)clampi_(*v + dx, 0, 2); save_commit();
      }
      audio_play(SFX_MENU_MOVE);
    }
    if (edge(p, IN_X)) { st->scheme = st->scheme == SCHEME_TANK ? SCHEME_ARCADE : SCHEME_TANK; SAVE.scheme[s] = st->scheme; save_commit(); audio_play(SFX_MENU_MOVE); }
    if (edge(p, IN_A)) { st->ready = !st->ready; audio_play(st->ready ? SFX_MENU_OK : SFX_MENU_BACK); }
    if (edge(p, IN_START) && st->ready) go = 1;
    if (held(p, IN_B)) {
      st->leave_hold += dt;
      if (st->leave_hold >= 1.0f) { game_seat_leave(s); audio_play(SFX_MENU_BACK); changed = 1; continue; }
    } else st->leave_hold = 0;
    if (PADS[p].idle >= 60.0f) { game_seat_leave(s); changed = 1; }
  }
  if (changed) lobby_scene();
  int any_ready = 0;
  for (int i = 0; i < 4; i++) if (G.seats[i].kind != SEAT_EMPTY && G.seats[i].ready) any_ready = 1;
  lobby_ready_t = any_ready ? lobby_ready_t + dt : 0;
  if (any_ready && lobby_ready_t >= LOBBY_AUTO_S) go = 1;
  if (go) { audio_play(SFX_MENU_OK); go_garage(0); return; }
  if (game_seated() == 0) {
    lobby_empty_t += dt;
    if (lobby_empty_t >= LOBBY_EMPTY_S) go_title();
  } else lobby_empty_t = 0;
}

/* -- garage (3.1) ------------------------------------------------ */
static void build_changed(int s) {
  seat_t *st = &G.seats[s];
  memcpy(st->bars_old, st->bars_new, sizeof st->bars_old);
  bars_now(&st->build, st->bars_new);
  st->bars_from_t = (int)G.frame;
  bot_name(st->name, &st->build);
  st->preset = (uint8_t)build_preset_of(&st->build);
  world_bot_build(s, &st->build, seat_colour(s));
}

static void cycle_row(int s, int row, int dir) {
  seat_t *st = &G.seats[s];
  build_t *b = &st->build;
  if (row == ROW_WEAPON && b->drive == DR_MELTY) return;          /* MELTY has no weapon row */
  int n = row_count(b, row), cur = build_get(b, row);
  for (int k = 1; k < n; k++) {
    int o = wrap(cur + dir * k, n);
    if (!option_legal(b, row, o, G.players, 0)) continue;           /* illegal options are skipped */
    build_set(b, row, o);
    if (row == ROW_DRIVE && o == DR_MELTY) { b->weapon = WP_NONE; b->weapon2 = WP_NONE; }
    if (!build_mount_ok(b)) b->weapon2 = WP_NONE;
    build_changed(s);
    audio_part(row, o);
    return;
  }
}

static void lock_seat(int s) {
  seat_t *st = &G.seats[s];
  st->locked = 1; st->lock_hold = 0; st->locked_first = 1;
  world_turntable_lock(s, 1);
  if (st->kind == SEAT_HUMAN) { SAVE.fav_build[s] = st->build; save_commit(); }
  audio_play(SFX_LOCK);
  rect_t vp[4]; int n; cam_current(vp, &n);
  if (s < n) for (int k = 0; k < J_LOCK_CONFETTI / 4; k++) fx2_burst(FX2_CONFETTI, vp[s].x + vp[s].w * 0.5f, vp[s].y + vp[s].h * 0.5f, seat_colour(s));
}

static int ai_seat(void) { for (int i = 0; i < 4; i++) if (G.seats[i].kind == SEAT_AI) return i; return -1; }

static void set_ai_level(int s, int level) {
  seat_t *st = &G.seats[s];
  level = clampi_(level, AI_EASY, AI_HARD);
  if (level == st->ai_level) return;
  st->ai_level = (uint8_t)level; SAVE.ai_level = (uint8_t)level; save_commit();
  ai_pick_build(&ai_pick[s], level, human_last_build(), SAVE.habits, &grng);
  if (!build_legal(&ai_pick[s], G.players)) build_preset(&ai_pick[s], PRE_LAUNCH);
  ai_pick[s].paint = (uint8_t)s;
  if (st->locked) { st->build = ai_pick[s]; build_changed(s); } else ai_frame[s] = 0;
  audio_play(SFX_MENU_MOVE);
}

static void garage_join(int p) {
  if (game_seated() >= 4) return;
  int s = join_seat(p);
  if (s < 0) return;
  /* a seat the practice bot held stays an AI; a new human takes the next free seat */
  G.seats[s].join_slide = 1;
  G.players = (uint8_t)game_seated();
  for (int i = 0; i < 4; i++) G.seats[i].slot = G.seats[i].kind != SEAT_EMPTY ? i : -1;
  seat_show(s);
  cam_set_garage(G.players);
}

static void garage_update(float dt) {
  G.controls_card_seat = -1;
  if (tourney_on()) {
    static int players_seen;
    if (G.phase_frames == 1) players_seen = G.players;
    if (G.players != players_seen && t_match == 0) { players_seen = G.players; tourney_reset(); }   /* a mid garage join reseeds the bracket */
    tourney_flip_tick();
  }
  /* joining mid garage: an unseated pad on a face button gets a panel (3.1); a stick moves the practice bot toggle */
  for (int p = 0; p < 4; p++) {
    if (PADS[p].seat >= 0) continue;
    if (face_edge(p) && !G.solo) garage_join(p);
    else if (PADS[p].nav_x && ai_seat() >= 0) set_ai_level(ai_seat(), G.seats[ai_seat()].ai_level + PADS[p].nav_x);
  }
  for (int s = 0; s < 4; s++) {
    seat_t *st = &G.seats[s];
    if (st->kind == SEAT_EMPTY) continue;
    if (st->join_slide > 0) { st->join_slide -= dt / 0.3f; if (st->join_slide < 0) st->join_slide = 0; }
    if (st->slot_machine_t > 0) {
      st->slot_machine_t--;
      if (st->slot_machine_t % 6 == 0) audio_play(st->slot_machine_t == 0 ? SFX_LOCK : SFX_MENU_MOVE);
    }
    if (st->kind == SEAT_AI) {
      if (!st->locked) {
        int r = ai_garage_tick(&st->build, &ai_pick[s], ai_frame[s]++, G.players, &grng);
        if (r) { build_changed(s); audio_play(SFX_MENU_MOVE); }
        if (r == 2) lock_seat(s);
      }
      continue;
    }
    int p = st->pad;
    if (p < 0) continue;
    if (held(p, IN_Y)) G.controls_card_seat = s;
    /* the right stick scrubs the turntable; releasing resumes (3.1) */
    if (PADS[p].cur.rx < -8000 || PADS[p].cur.rx > 8000) world_turntable(s, world_turntable_angle(s) + PADS[p].cur.rx / 32767.0f * 3.0f * dt);
    if (PADS[p].cur.rt > TRIGGER_ON && PADS[p].prev.rt <= TRIGGER_ON) world_weapon_test_fire(s);
    if (st->locked) {
      if (edge(p, IN_B)) { st->locked = 0; world_turntable_lock(s, 0); audio_play(SFX_MENU_BACK); }
      continue;
    }
    if (st->slot_machine_t > 0) continue;
    int rows = build_mount_ok(&st->build) ? ROW_COUNT : ROW_COUNT - 1;
    if (st->row >= rows) st->row = rows - 1;
    if (PADS[p].nav_y) { st->row = wrap(st->row + PADS[p].nav_y, rows); audio_play(SFX_MENU_MOVE); }
    if (PADS[p].nav_x) cycle_row(s, st->row, PADS[p].nav_x);
    /* the practice bot toggle: this human's right stick while the cursor is on PAINT (3.12) */
    int rs = rs_step(p);
    if (rs && st->row == ROW_PAINT && ai_seat() >= 0) set_ai_level(ai_seat(), G.seats[ai_seat()].ai_level + rs);
    if (edge(p, IN_START)) {
      int next = wrap((st->preset < PRE_RANDOM ? st->preset : -1) + 1, PRE_RANDOM + 1);
      uint8_t paint = st->build.paint;
      if (next == PRE_RANDOM) { build_random(&st->build, &grng, G.players); st->slot_machine_t = J_SLOT_MACHINE_FRAMES; }
      else build_preset(&st->build, next);
      st->build.paint = paint;
      build_changed(s);
      if (next == PRE_RANDOM) st->preset = PRE_RANDOM;
      audio_play(SFX_MENU_MOVE);
    }
    if (edge(p, IN_X)) {
      uint8_t paint = st->build.paint;
      build_random(&st->build, &grng, G.players);
      st->build.paint = (uint8_t)rtr_rand_n(&grng, PAINT_COUNT);
      (void)paint;
      build_changed(s);
      st->slot_machine_t = J_SLOT_MACHINE_FRAMES;
    }
    /* A locks: a 1.0 s hold the first time in an evening, instant afterwards; never over budget or illegal */
    int legal = build_legal(&st->build, G.players);
    if (held(p, IN_A) && legal) {
      if (st->locked_first) { if (edge(p, IN_A)) lock_seat(s); }
      else { st->lock_hold += dt; if (st->lock_hold >= 1.0f - 1e-4f) lock_seat(s); }
    } else {
      if (edge(p, IN_A) && !legal) audio_play(SFX_MENU_BACK);
      st->lock_hold = 0;
    }
  }
  /* the shared timer: at 0 every unlocked bot locks as is (an illegal one falls back to its seat's preset) */
  int all = 1;
  for (int s = 0; s < 4; s++) if (G.seats[s].kind != SEAT_EMPTY && !G.seats[s].locked) all = 0;
  if (snap_left < 0 && G.phase_frames >= garage_total) {
    for (int s = 0; s < 4; s++) {
      seat_t *st = &G.seats[s];
      if (st->kind == SEAT_EMPTY || st->locked) continue;
      if (!build_legal(&st->build, G.players)) { uint8_t paint = st->build.paint; build_preset(&st->build, s % PRE_RANDOM); st->build.paint = paint; build_changed(s); }
      lock_seat(s);
    }
    all = 1;
  }
  if (all && snap_left < 0) snap_left = SNAP_FRAMES;
  if (snap_left >= 0) {
    /* the tape snaps to zero over 0.3 s: the drawn clock reads phase_frames */
    int left = garage_total - G.phase_frames;
    if (snap_left > 0 && left > 0) G.phase_frames += left / snap_left;
    if (snap_left-- <= 0) go_vote();
  }
}

/* -- vote (10.12) ------------------------------------------------ */
static void vote_update(float dt) {
  if (!G.vote_decided) {
    for (int p = 0; p < 4; p++) {
      int s = seat_of_pad(p);
      if (s < 0) continue;
      seat_t *st = &G.seats[s];
      if (held(p, IN_Y)) {
        if (PADS[p].nav_x) { G.vote_one_camera = clampi_(G.vote_one_camera + PADS[p].nav_x, 0, 2); SAVE.one_camera = (uint8_t)G.vote_one_camera; save_commit(); audio_play(SFX_MENU_MOVE); }
        continue;
      }
      /* Left and Right walk the six cards in order (wrapping across the rows); Up and Down jump a row */
      if (PADS[p].nav_x) { st->vote = wrap(st->vote + PADS[p].nav_x, ARENA_COUNT); voted[s] = 1; audio_play(SFX_MENU_MOVE); }
      if (PADS[p].nav_y) { st->vote = wrap(st->vote + 3 * PADS[p].nav_y, ARENA_COUNT); voted[s] = 1; audio_play(SFX_MENU_MOVE); }
      if (edge(p, IN_A)) { voted[s] = 1; audio_play(SFX_MENU_OK); }
    }
    /* the tally; AI seats go where the humans lead */
    int count[ARENA_COUNT] = { 0 }, humans = 0, all = 1;
    for (int s = 0; s < 4; s++) if (G.seats[s].kind == SEAT_HUMAN) { count[G.seats[s].vote]++; humans++; if (!voted[s]) all = 0; }
    int lead = G.vote_leader;
    for (int i = 0; i < ARENA_COUNT; i++) if (count[i] > count[lead]) lead = i;
    for (int s = 0; s < 4; s++) if (G.seats[s].kind == SEAT_AI) { G.seats[s].vote = lead; count[lead]++; }
    G.vote_leader = lead;
    if (humans && all && vote_all_frame < 0) vote_all_frame = G.phase_frames;
    int close_at = VOTE_FRAMES;
    if (vote_all_frame >= 0 && vote_all_frame + VOTE_ALL_WAIT < close_at) close_at = vote_all_frame + VOTE_ALL_WAIT;
    G.vote_left = close_at - G.phase_frames;
    int decide = G.vote_left <= 0;
    if (decide) {
      int best = 0; for (int i = 0; i < ARENA_COUNT; i++) if (count[i] > best) best = count[i];
      int tied[ARENA_COUNT], nt = 0;
      for (int i = 0; i < ARENA_COUNT; i++) if (count[i] == best) tied[nt++] = i;
      int win = nt == 1 ? tied[0] : tied[rtr_rand_n(&grng, nt)];
      vote_tie = nt > 1;
      G.vote_leader = win; G.arena = (uint8_t)win;
      G.vote_decided = 1; G.vote_decided_t = vote_tie ? 0 : 0.5f; G.vote_left = 0;
      vote_decide_frame = G.phase_frames;
      audio_play(SFX_LOCK);
    }
    return;
  }
  G.vote_decided_t += dt;
  if (G.phase_frames - vote_decide_frame >= VOTE_SHOW + (vote_tie ? VOTE_TIE_FLICK : 0)) go_countdown();
}

/* -- fight, KO, judges (2.3, 2.10) ------------------------------- */
static void pause_open(int seat) {
  G.paused = 1; G.pauser = seat; G.pause_row = 0; G.reset_hold = 0; G.timescale = 0;
  audio_duck(0.3f);
  audio_play(SFX_MENU_OK);
}
static void pause_close(void) {
  G.paused = 0; G.controls_card_seat = -1; G.reset_hold = 0;
  G.timescale = G.miniko_frames > 0 ? J_MINIKO_TIMESCALE : 1;
  audio_duck(1);
}

static void pause_update(float dt) {
  if (G.controls_card_seat >= 0) {
    for (int p = 0; p < 4; p++) if (edge(p, IN_A | IN_B | IN_START)) { G.controls_card_seat = -1; return; }
    return;
  }
  int quit_held = 0;
  for (int p = 0; p < 4; p++) {
    if (PADS[p].nav_y) { G.pause_row = wrap(G.pause_row + PADS[p].nav_y, 4); G.reset_hold = 0; audio_play(SFX_MENU_MOVE); }
    if (edge(p, IN_START | IN_B)) { pause_close(); return; }
    if (G.pause_row == 3) { if (held(p, IN_A)) quit_held = 1; continue; }
    if (edge(p, IN_A)) {
      if (G.pause_row == 0) { pause_close(); return; }
      if (G.pause_row == 1) { G.controls_card_seat = G.pauser; return; }
      if (G.pause_row == 2) { G.settings_from_pause = 1; G.paused = 0; G.settings_row = 0; G.settings_saved_t = 0; G.phase = PH_SETTINGS; G.phase_t = 0; return; }
    }
  }
  if (quit_held) {
    G.reset_hold += dt;
    if (G.reset_hold >= 1.0f) { G.paused = 0; G.reset_hold = 0; audio_duck(1); for (int i = 0; i < 4; i++) audio_whine(i, 0, 0); go_lobby(); }
  } else G.reset_hold = 0;
}

static int is_spinner(const bot_t *b) { return b->build.drive == DR_MELTY || b->build.weapon == WP_HORIZ || b->build.weapon == WP_DRUM; }

static void fight_audio(void) {
  int tense = 0;
  for (int i = 0; i < 4; i++) {
    if (i >= M.players || i >= M.nbots) { audio_whine(i, 0, 0); continue; }
    const bot_t *b = &M.bots[i];
    float lv = b->alive && is_spinner(b) ? b->spin : 0;
    audio_whine(i, lv * 0.8f, 0.3f + lv * 0.9f);
    if (b->alive && !b->is_ai && b->hp <= 30 && b->hp > 0) tense = 1;
  }
  audio_tension(tense);
  /* T minus 10: the clock ticks each second (2.3) */
  if (M.fight_len_s && G.mode != MODE_HILL) {
    int s = (int)ceilf(M.clock_s);
    if (s <= 10 && s > 0 && s != last_count_s) { audio_play(SFX_COUNT); last_count_s = s; }
  }
}

static void fight_update(float dt) {
  (void)dt;
  if (G.paused) { pause_update(dt); return; }
  for (int p = 0; p < 4; p++) {
    int s = seat_of_pad(p);
    if (s >= 0 && G.seats[s].kind == SEAT_HUMAN && edge(p, IN_START)) { pause_open(s); return; }
    /* nothing joins mid fight: an unseated pad gets the NEXT MATCH chip (10.2) */
    if (s < 0 && PADS[p].edge) { G.next_match_chip_pad = p; G.next_match_chip_t = 2.0f; }
  }
  if (G.miniko_frames > 0) { G.miniko_frames--; G.timescale = G.miniko_frames > 0 ? J_MINIKO_TIMESCALE : 1; }
  /* hazard seats: the stick picks (a new seat starts on the first hazard), A fires (2.10) */
  for (int s = 0; s < 4; s++) {
    if (game_hazard_seat_of(s) < 0) continue;
    if (G.seats[s].kind != SEAT_HUMAN || G.seats[s].pad < 0) continue;
    if (s == ops_driver || s == ops_gunner) continue;
    int p = G.seats[s].pad;
    if (M.hazard_seat[s] == 255) { sim_seat_pick(&M, s, 0); if (M.hazard_seat[s] != 255) audio_play(SFX_MENU_MOVE); continue; }
    int d = PADS[p].nav_x ? PADS[p].nav_x : PADS[p].nav_y;
    if (d) { sim_seat_pick(&M, s, d); audio_play(SFX_MENU_MOVE); }
    if (edge(p, IN_A)) sim_seat_fire(&M, s);
  }
  /* the final's OPS crew: the driver's left stick, the gunner's A (lifter) and X (saw) */
  if (M.ops_driven) {
    if (ops_driver >= 0 && G.seats[ops_driver].pad >= 0) M.ops_in_drive = PADS[(int)G.seats[ops_driver].pad].cur;
    if (ops_gunner >= 0 && G.seats[ops_gunner].pad >= 0) M.ops_in_gun = PADS[(int)G.seats[ops_gunner].pad].cur;
  }
  fight_audio();
  if (M.over) {
    if (M.win_how == WIN_JUDGES) go_judges(); else go_ko();
  }
}

static void ko_update(void) {
  G.ko_frames++;
  if (G.ko_frames == J_KO_SLOW_FRAMES) { G.timescale = 0; audio_slowmo(0); }
  if (G.ko_frames >= J_KO_SLOW_FRAMES + J_KO_HOLD_FRAMES) go_results();
}

/* -- results and rematch (10.21) --------------------------------- */
static void results_next(void) {
  if (tourney_on() && !t_done) { audio_play(SFX_MENU_OK); go_vote(); }   /* the bracket goes on: the next pairing votes an arena */
  else go_rematch();
}

static void results_update(void) {
  if (G.phase_frames >= RESULTS_MIN) for (int p = 0; p < 4; p++) if (seat_of_pad(p) >= 0 && edge(p, IN_A | IN_START)) { results_next(); return; }
  if (G.phase_frames >= RESULTS_FRAMES) results_next();
}

static void rematch_apply(int row) {
  audio_play(SFX_MENU_OK);
  switch (row) {
    case RM_REBUILD: go_garage(1); break;
    case RM_SAME_BOTS: case RM_NEW_ARENA: go_vote(); break;
    default: go_lobby(); break;
  }
}

static void rematch_update(void) {
  int humans = 0, all = 1;
  for (int p = 0; p < 4; p++) {
    int s = seat_of_pad(p);
    if (s < 0 || G.seats[s].kind != SEAT_HUMAN) continue;
    seat_t *st = &G.seats[s];
    if (PADS[p].nav_y && !G.rematch_votes[s]) { st->rematch = wrap(st->rematch + PADS[p].nav_y, 4); audio_play(SFX_MENU_MOVE); }
    if (edge(p, IN_A)) { G.rematch_votes[s] = 1; audio_play(SFX_MENU_OK); }
    if (edge(p, IN_B)) G.rematch_votes[s] = 0;
  }
  int count[4] = { 0 };
  for (int s = 0; s < 4; s++) {
    if (G.seats[s].kind == SEAT_EMPTY) continue;
    if (G.seats[s].kind == SEAT_HUMAN) { humans++; if (!G.rematch_votes[s]) all = 0; }
    count[clampi_(G.seats[s].rematch, 0, 3)] += G.seats[s].kind == SEAT_HUMAN ? 2 : 1;   /* humans outvote the practice bot */
  }
  int best = RM_REBUILD;                           /* REBUILD wins ties */
  for (int r = 0; r < 4; r++) if (count[r] > count[best]) best = r;
  G.rematch_choice = best;
  if ((humans && all) || G.phase_frames >= REMATCH_AUTO) rematch_apply(best);
}

/* -- settings, hall, rules (10.22, 10.23, 10.25) ----------------- */
static void settings_back(void) {
  audio_play(SFX_MENU_BACK);
  if (G.settings_from_pause) { G.settings_from_pause = 0; G.phase = PH_FIGHT; G.phase_t = 0; G.paused = 1; G.pause_row = 2; return; }
  game_set_phase(PH_TITLE); title_idle = 0;
}

static void settings_apply(void) {
  audio_set_levels(SAVE.sfx, SAVE.music);
  cam_set_shake_scale(SAVE.shake);
  G.settings_saved_t = 0.001f;
}

static void settings_update(float dt) {
  if (G.settings_saved_t > 0) G.settings_saved_t += dt;
  int nrows = LEN(SETTINGS_ROWS), reset_held = 0;
  for (int p = 0; p < 4; p++) {
    if (PADS[p].nav_y) { G.settings_row = wrap(G.settings_row + PADS[p].nav_y, nrows); G.reset_hold = 0; audio_play(SFX_MENU_MOVE); }
    if (PADS[p].nav_x) {
      if (G.settings_row <= 10) { game_setting_step(G.settings_row, PADS[p].nav_x); settings_apply(); audio_play(SFX_MENU_MOVE); }
      else if (G.settings_row == 11) {
        int s = PADS[p].seat >= 0 ? PADS[p].seat : p;
        SAVE.scheme[s] = SAVE.scheme[s] == SCHEME_TANK ? SCHEME_ARCADE : SCHEME_TANK;
        if (G.seats[s].kind == SEAT_HUMAN) G.seats[s].scheme = SAVE.scheme[s];
        save_commit(); settings_apply(); audio_play(SFX_MENU_MOVE);
      }
    }
    if (G.settings_row == 12 && held(p, IN_A)) reset_held = 1;
    if (edge(p, IN_B)) { settings_back(); return; }
  }
  if (reset_held) {
    G.reset_hold += dt;
    if (G.reset_hold >= 2.0f) { save_defaults(&SAVE); for (int s = 0; s < 4; s++) if (G.seats[s].kind == SEAT_HUMAN) G.seats[s].scheme = SAVE.scheme[s]; settings_apply(); G.reset_hold = 0; audio_play(SFX_MENU_BACK); }
  } else if (G.settings_row == 12) G.reset_hold = 0;
}

static void hall_update(float dt) {
  int wipe = 0;
  for (int p = 0; p < 4; p++) {
    if (held(p, IN_B)) { b_hold[p] += dt; wipe = 1; }
    else if (PADS[p].release & IN_B) { if (b_hold[p] < 0.5f) { b_hold[p] = 0; G.hall_wipe_hold = 0; audio_play(SFX_MENU_BACK); game_set_phase(PH_TITLE); title_idle = 0; return; } b_hold[p] = 0; }
    if (edge(p, IN_A)) { audio_play(SFX_MENU_BACK); game_set_phase(PH_TITLE); title_idle = 0; return; }
  }
  if (wipe) {
    G.hall_wipe_hold += dt;
    if (G.hall_wipe_hold >= 3.0f) {
      /* the hall of fame only; settings stay (20.13) */
      SAVE.best_evening_wins = 0; SAVE.best_seat = 0; SAVE.fastest_ko_ms = 0; SAVE.fastest_ko_name[0] = 0; SAVE.fastest_ko_seat = 0;
      SAVE.longest_streak = 0; SAVE.streak_name[0] = 0; SAVE.streak_seat = 0;
      memset(SAVE.wins_by_preset, 0, sizeof SAVE.wins_by_preset); memset(SAVE.ko_by_cause, 0, sizeof SAVE.ko_by_cause); SAVE.matches = 0;
      save_commit();
      audio_play(SFX_MENU_BACK);
      G.hall_wipe_hold = 0; for (int p = 0; p < 4; p++) b_hold[p] = -100;   /* the release after a wipe is not a back */
    }
  } else G.hall_wipe_hold = 0;
}

static void rules_update(float dt) {
  G.flash_t += dt;
  int next = G.flash_t >= FLASH_CARD_S, skip = 0;
  for (int p = 0; p < 4; p++) { if (edge(p, IN_A)) next = 1; if (edge(p, IN_B)) skip = 1; }
  if (next) { G.flash_step++; G.flash_t = 0; audio_play(SFX_MENU_MOVE); }
  if (skip || G.flash_step >= FLASH_STEPS) {
    SAVE.rules_seen = 1; save_commit();
    G.flash_step = 0;
    if (G.flash_from == PH_TITLE) { game_set_phase(PH_TITLE); title_idle = 0; } else game_set_phase(G.flash_from);
  }
}

/* -- events from the sim (every drained event) ------------------- */
void game_on_event(const event_t *e) {
  if (G.attract) return;
  switch (e->type) {
    case EV_KO:
      G.ko_victim = e->b < M.nbots ? e->b : -1;
      G.ko_victor = e->a < M.nbots ? e->a : -1;
      break;
    case EV_MINI_KO: {
      G.miniko_frames = J_MINIKO_FRAMES; G.timescale = J_MINIKO_TIMESCALE;
      if (e->b < M.nbots) {
        const bot_t *v = &M.bots[e->b];
        int vp = v->seat < 4 ? cam_viewport_of(v->seat) : -1;
        if (vp >= 0) stinger(vp, KO_CAUSE[e->zone < 6 ? e->zone : 0], seat_colour(e->a < M.nbots && M.bots[e->a].seat < 4 ? M.bots[e->a].seat : v->seat & 3));
        if (v->seat < 4) out_frame[v->seat] = (int)G.frame;
      }
      break; }
    case EV_TAUNT: if (e->a < MAX_BOTS) taunt_frame[e->a] = M.frame; break;
    case EV_HIT:
      if (e->b < MAX_BOTS && e->dmg >= 30 && taunt_frame[e->b] && M.frame - taunt_frame[e->b] <= 60) taunted_hit[e->b] = 1;
      break;
    case EV_STUCK: {
      char m[64]; snprintf(m, sizeof m, "[stuck] f=%d bot %d at %.2f %.2f", M.frame, e->a, e->x, e->z);
      logf_(m);
      break; }
    default: break;
  }
}

/* the hazard a seat drives, or -1: an eliminated player (after the 1.5 s stinger) or a
 * human sitting out a tournament match, in an arena with seat hazards */
int game_hazard_seat_of(int seat) {
  if (seat < 0 || seat > 3 || G.phase != PH_FIGHT || out_frame[seat] < 0 || !hazard_mode()) return -1;
  if ((int)G.frame - out_frame[seat] < 90 || seat == ops_driver || seat == ops_gunner) return -1;
  if (arena_seat_hazard_count(&ARENAS[M.arena]) <= 0) return -1;
  return M.hazard_seat[seat] != 255 ? M.hazard_seat[seat] : arena_seat_hazard(&ARENAS[M.arena], 0);
}
void game_record_inputs(const pad_state_t in[4]) { (void)in; }

/* -- the frame --------------------------------------------------- */
void game_update(float dt) {
  G.phase_t += dt; G.phase_frames++; G.frame++;
  G.tape_phase += 30 * dt;
  if (G.next_match_chip_t > 0) G.next_match_chip_t -= dt;
  /* the PRACTICE=off script flag (15.5): the rig holds SELECT, L3 and R3 on pad 0 in the first frames */
  if (G.frame <= 4 && (PADS[0].cur.buttons & RIG_COMBO) == RIG_COMBO && !G.rig_park) { G.rig_park = 1; logf_("[rig] practice parked"); }
  if (G.frame == 1) grng ^= G.seed * 2654435761u;
  if (G.frame <= 4) for (int p = 0; p < 4; p++) PADS[p].edge = 0;   /* the rig flag never counts as a press */
  switch (G.phase) {
    case PH_BOOT: boot_update(); break;
    case PH_TITLE: title_update(dt); break;
    case PH_SETTINGS: settings_update(dt); break;
    case PH_HALL: hall_update(dt); break;
    case PH_RULES: rules_update(dt); break;
    case PH_LOBBY: lobby_update(dt); break;
    case PH_GARAGE: garage_update(dt); break;
    case PH_VOTE: vote_update(dt); break;
    case PH_COUNTDOWN: {
      int n = 3 - G.phase_frames / 60;
      if (n != G.countdown_n && n > 0) audio_play(SFX_COUNT);
      G.countdown_n = n > 0 ? n : 0;
      if (G.phase_frames >= COUNTDOWN_FRAMES) go_fight();
      break; }
    case PH_FIGHT: fight_update(dt); break;
    case PH_KO: ko_update(); break;
    case PH_JUDGES: if (G.phase_frames >= JUDGES_FRAMES) go_results(); break;
    case PH_RESULTS: results_update(); break;
    case PH_REMATCH: rematch_update(); break;
    default: break;
  }
  if (G.phase == PH_TITLE || G.phase == PH_SETTINGS || G.phase == PH_HALL || G.phase == PH_RULES) attract_update(dt);
  for (int p = 0; p < 4; p++) rs_prev[p] = PADS[p].cur.rx < -16384 ? -1 : PADS[p].cur.rx > 16384 ? 1 : 0;
}
