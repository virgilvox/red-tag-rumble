/* game.h: evening state. Seats, mode, builds, scores, the bracket, the phase
 * machine, menu state (title, lobby, vote, rematch, settings, hall, rules
 * flash, pause), hazard seats, hall of fame updates, the save struct (spec
 * 10.1, 14.4). Owned by the game agent (items 10, 11, 16): bodies in game.c.
 * main.c reads G.timescale, G.phase and the pad edges; draw*.c reads the rest.
 * Not headless (it includes hud.h for the colour tokens). */
#ifndef RTR_GAME_H
#define RTR_GAME_H
#include <stdint.h>
#include "sim.h"
#include "arena.h"
#include "ai.h"
#include "hud.h"

/* phases (10.1) */
enum { PH_BOOT, PH_TITLE, PH_SETTINGS, PH_HALL, PH_RULES, PH_LOBBY, PH_GARAGE, PH_VOTE, PH_COUNTDOWN,
       PH_FIGHT, PH_KO, PH_JUDGES, PH_RESULTS, PH_REMATCH, PH_COUNT };
/* settings values */
enum { SET_AUTO, SET_ON, SET_OFF };                 /* COMEBACK */
enum { ONECAM_AUTO, ONECAM_OFF, ONECAM_ALWAYS };
enum { SPLIT_AUTO, SPLIT_HALVES, SPLIT_QUADS };
enum { FL_60, FL_75, FL_90, FL_120 };               /* FIGHT LENGTH index; seconds in FIGHT_LEN_S */
static const uint8_t FIGHT_LEN_S[4] __attribute__((unused)) = { 60, 75, 90, 120 };
/* rematch rows (10.21) */
enum { RM_REBUILD, RM_SAME_BOTS, RM_NEW_ARENA, RM_LOBBY };
/* title rows (10.7) */
enum { TR_FIGHT, TR_SOLO, TR_RULES, TR_HALL, TR_SETTINGS, TR_COUNT };
/* seat kinds */
enum { SEAT_EMPTY, SEAT_HUMAN, SEAT_AI };

/* frame timings (2.3) */
#define GARAGE_FRAMES     3600
#define REBUILD_FRAMES    1800
#define VOTE_FRAMES       480
#define COUNTDOWN_FRAMES  180
#define RESULTS_MIN       120
#define REMATCH_AUTO      1200
#define LOBBY_AUTO_S      25.0f
#define LOBBY_EMPTY_S     10.0f
#define RESULTS_IDLE_S    40.0f
#define TITLE_IDLE_S      60.0f
#define ATTRACT_DELAY_S   1.5f

/* -- save (14.4) ------------------------------------------------- */
#define SAVE_MAGIC   0x31525452u   /* 'RTR1' */
#define SAVE_VERSION 1
typedef struct {
  uint32_t magic;            /* 0x31525452 'RTR1' */
  uint32_t version;          /* 1 */
  uint32_t crc;              /* crc32 of everything after this field */
  uint8_t  split, one_camera, fight_len, comeback, shake, hitstop, stinger_big, sfx, music, colourblind, rumble, ai_level, rules_seen;
  uint8_t  scheme[4];                                   /* 0 arcade, 1 tank per seat */
  build_t  fav_build[4];                                /* the last locked build per seat */
  uint16_t best_evening_wins; uint8_t best_seat;
  uint32_t fastest_ko_ms;  char fastest_ko_name[24]; uint8_t fastest_ko_seat;
  uint32_t longest_streak; char streak_name[24];     uint8_t streak_seat;
  uint32_t wins_by_preset[8];                           /* 6 presets, CUSTOM, spare */
  uint32_t ko_by_cause[6];                              /* HP ZERO, FLIP, PIT, ROOF, COUNT, DECK */
  uint32_t matches;
  uint8_t  habits[8];                                   /* HARD AI memory */
} save_t;                    /* under 200 bytes */
extern save_t SAVE;          /* static in main.c; info.save_ptr = &SAVE; info.save_size = sizeof SAVE */
void save_defaults(save_t *s);
uint32_t save_crc(const save_t *s);
void save_commit(void);      /* recompute the crc after a change; the host picks the region up after the frame */
int  save_valid(const save_t *s);

/* -- pads and seats ---------------------------------------------- */
/* one per physical pad, written by main.c every frame before game_update */
typedef struct {
  pad_state_t cur, prev;
  uint16_t edge;             /* buttons pressed this frame (IN_*) */
  uint16_t release;          /* buttons released this frame */
  int8_t nav_x, nav_y;       /* menu navigation events this frame: -1, 0, 1 (edge with repeat after 18 then every 8) */
  int repeat_x, repeat_y;    /* frames the direction has been held */
  int seat;                  /* the seat this pad holds, or -1 */
  float idle;                /* seconds since the last input */
  int any;                   /* any input this frame */
} pad_input_t;
extern pad_input_t PADS[4];

typedef struct {
  uint8_t kind;              /* SEAT_EMPTY, SEAT_HUMAN, SEAT_AI */
  int8_t pad;                /* physical pad, -1 for an AI seat */
  uint8_t ready, locked, scheme, ai_level, out, team;
  build_t build; uint8_t preset;
  int row;                   /* garage cursor row */
  float lock_hold;           /* seconds A has been held toward the first lock */
  float leave_hold;          /* seconds B has been held toward leaving */
  int vote;                  /* arena vote card, or -1 */
  int rematch;               /* rematch row, or -1 */
  uint8_t wins;              /* evening tally */
  char name[24];
  int slot;                  /* M.bots slot once a match is set up */
  int hazard_pick;           /* hazard seat choice */
  int bars_from_t;           /* frame the bars started animating */
  float bars_old[3], bars_new[3];
  int slot_machine_t;        /* frames left in the RANDOM BOT animation */
  int locked_first;          /* has locked once this evening (instant lock afterwards) */
  float join_slide;          /* 0..1 panel slide-in */
  char hint[48];
} seat_t;

/* -- the evening ------------------------------------------------- */
typedef struct {
  int phase; float phase_t; int phase_frames;
  float timescale;           /* 1 play, 0.2 KO slow-mo, 0.4 mini-KO, 0 paused */
  int paused; int pauser; int pause_row;
  seat_t seats[4];
  uint8_t mode, arena, players, comeback_eff;    /* comeback_eff: COMEBACK resolved for this match */
  int one_camera_eff, split_eff;
  uint8_t fill_with_bot;
  int lobby_mode_cursor;
  int title_row; int title_owner; float title_owner_t;
  int settings_row; float settings_saved_t; float reset_hold;
  int flash_step; float flash_t; int flash_from;  /* rules flash: the phase to return to */
  float hall_wipe_hold;
  int vote_leader; int vote_decided; float vote_decided_t; int vote_one_camera;
  int countdown_n;
  int ko_victim, ko_victor, ko_frames; int miniko_frames;
  int rematch_choice; int rematch_votes[4];
  int judges_rule; float judge_bars[3][MAX_BOTS]; int judge_winner;
  int award[MAX_BOTS]; int place[MAX_BOTS];
  int tournament_round; int bracket[4]; int bracket_wins[4]; int bye_seat;
  int attract; uint32_t attract_seed;
  int render_half; float frame_ms_avg; int slow_frames, fast_frames;
  int controls_card_seat; float controls_card_t;
  int next_match_chip_pad; float next_match_chip_t;
  int matches_this_evening;
  uint32_t frame;            /* render frames since boot */
  float tape_phase;          /* the scrolling caution tape phase */
  int evening_started;
  uint32_t match_index;
  /* game_flow.c */
  int lobby_row;             /* lobby: 0 the mode cards, 1..3 the ONE CAMERA, SPLIT and COMEBACK steppers */
  int rig_park;              /* the PRACTICE=off script flag under the rig (15.5): AI seats park, 90 s fights */
  int sim_on;                /* main.c steps the sim this frame (fight, KO, the title attract) */
  int settings_from_pause;   /* SETTINGS opened from the pause menu: B returns to the paused fight */
  uint32_t seed;             /* the host seed (wc_set_seed); every match seed derives from it */
  int solo;                  /* SOLO from the title: one human and the practice bot */
  int vote_left;             /* frames until the vote closes (8 s, or 2 s after every seat has voted); 0 when unset */
} game_t;

extern game_t G;
extern match_t M;
extern ai_t AIS[MAX_BOTS];
extern match_setup_t SETUP;

void game_init(void);
/* once per render frame after main.c wrote PADS[]; drives the phase machine and menus */
void game_update(float dt);
void game_set_phase(int phase);
int  game_seated(void);                /* seats with a human or an AI */
int  game_humans(void);
/* a seat is claimed on an A, B, X, Y or Start edge from any pad in the lobby or garage; never on connected */
int  game_seat_join(int pad);
void game_seat_leave(int seat);
/* seat colour (2.1) by seat index */
static inline uint32_t seat_colour(int seat) { return SEAT_RGB[seat & 3]; }
/* build the match_setup_t from the seats and settings and start the fight */
void game_start_match(void);
/* every event drained from M.ev after a step reaches game.c too (hall of fame, awards, hazard seats, mini-KO) */
void game_on_event(const event_t *e);
/* the setting rows (10.22): step a row by dir and write the save */
void game_setting_step(int row, int dir);
const char *game_setting_value(int row, char *buf, int n);
/* COMEBACK AUTO means ON in free-for-all and 2V2 and OFF in any 1v1 (2.8) */
int  game_comeback_effective(void);
/* ONE CAMERA default on for 2 players in THE BAY, off elsewhere (9.3) */
int  game_one_camera_effective(void);
/* the hazard seat a pad drives after elimination, or -1 */
int  game_hazard_seat_of(int seat);
/* hall of fame and the evening tally at results */
void game_record_results(void);
/* the attract match on the title (8.6): two MEDIUM AIs, random presets, THE BAY */
void game_attract_start(void);
void game_attract_stop(void);
/* the input ring for the KO replay and balance --replay (4.10) */
void game_record_inputs(const pad_state_t in[4]);
/* game_flow.c: 0 when the frame has no 3D views (boot, the vote, a title without the attract), else 1 */
int  game_view_kind(void);
/* the physical pad driving a match slot, or -1 for an AI (or attract) slot */
int  game_slot_pad(int slot);

#endif
