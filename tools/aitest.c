/* tools/aitest.c: the AI scorer and steering on hand-built match_t snapshots.
 * Links ai.c with the headless sim.c (trig table), bot.c (bodies) and arena.c
 * (hazard_phase, nearest wall). Each situation builds a match by hand, runs
 * ai_think and checks the goal section 8.2 (and 7 for OPS) says it must pick.
 *
 *   cc -std=gnu11 -O2 -Wall -DRTR_HEADLESS -I src src/sim.c src/bot.c src/arena.c src/ai.c tools/aitest.c -lm -o build/aitest
 *   build/aitest            (exit status is the number of failures) */
#include <stdio.h>
#include <string.h>
#include "ai.h"
#include "bot.h"

static const char *GN[GOAL_COUNT] = { "NONE", "ATTACK", "EVADE", "HAZARD_PUSH", "RECOVER", "BAIT", "PICKUP", "TAUNT", "WIGGLE",
                                      "OPS_GO", "OPS_LIFT", "OPS_CARRY", "OPS_RETURN", "OPS_PUNISH" };

static match_t M;
static arena_def_t AR;
static ai_t A;
static int fails, total;

/* a 24 x 16 test floor with one pit at (0, 3), 2 x 2: 10 s period, active 3 s from phase 0, tell 1 s */
static void arena_reset(int pit_state) {
  memset(&AR, 0, sizeof AR);
  AR.name = "TEST BAY"; AR.hook = "TEST"; AR.w = 24; AR.d = 16; AR.traction_mult = 1;
  AR.ops_zone[0] = -10.5f; AR.ops_zone[1] = 6.5f;
  if (pit_state >= 0) {
    hazard_def_t *h = &AR.haz[0];
    h->type = HZ_PIT; h->x = 0; h->z = 3; h->w = 2; h->d = 2; h->period = 10; h->active_s = 3; h->tell_s = 1; h->dmg = 0;
    AR.nhaz = 1;
  }
  /* haz_clock: 1 active, 2 opens in 0.8 s (tell), 3 opens in 1.5 s, 0 idle (opens in 4 s) */
  M.haz_clock = pit_state == 1 ? 1.0f : pit_state == 2 ? 9.2f : pit_state == 3 ? 8.5f : 6.0f;
}

static void preset(build_t *b, int p) { build_preset(b, p); }

/* a fresh 1v1: me in slot 0 at (x, z, h), the enemy in slot 1 */
static void match_reset(int my_preset, float x, float z, float h, int en_preset, float ex, float ez, float eh) {
  memset(&M, 0, sizeof M);
  M.mode = MODE_DEATHMATCH; M.players = 2; M.nbots = 2; M.clock_s = 60; M.ops_target = 255; M.hazards_on = 1;
  build_t b;
  preset(&b, my_preset); bot_setup(&M.bots[0], &b, 0, 1, 0, x, z, h);
  preset(&b, en_preset); bot_setup(&M.bots[1], &b, 1, 0, 1, ex, ez, eh);
  arena_reset(-1);
}

static void add_ops(int state, int target) {
  bot_setup_ops(&M.bots[OPS_SLOT], AR.ops_zone[0], AR.ops_zone[1], 0, 0);
  M.nbots = 5;
  for (int i = 2; i < 4; i++) { M.bots[i].alive = 0; M.bots[i].is_ai = 1; }
  M.ops_state = (uint8_t)state; M.ops_target = (uint8_t)(target < 0 ? 255 : target);
}

static void ai_reset(int diff, int slot) {
  ai_init(&A, diff, slot); ai_bind(&A, slot); ai_set_arena(&A, &AR);
}

static void btns(uint16_t b, char *out) {
  out[0] = 0;
  if (b & IN_A) strcat(out, "A"); if (b & IN_B) strcat(out, "B"); if (b & IN_X) strcat(out, "X"); if (b & IN_L) strcat(out, "L");
  if (!out[0]) strcat(out, "-");
}

/* run `frames` thinks on the frozen snapshot and report the last one */
static pad_state_t run(int frames) {
  pad_state_t out; memset(&out, 0, sizeof out);
  for (int i = 0; i < frames; i++) { ai_think(&A, &M, &out); M.frame++; }
  return out;
}

static void report(const char *name, pad_state_t o, int want, int extra_ok, const char *note) {
  char b[16]; btns(o.buttons, b);
  int ok = A.goal == want && extra_ok;
  total++; if (!ok) fails++;
  printf("%2d %-34s goal %-11s want %-11s turn %+5.2f thr %+5.2f btn %-3s tgt %2d  %s  %s\n", total, name, GN[A.goal], GN[want],
         o.lx / 32767.0f, -o.ly / 32767.0f, b, A.goal >= GOAL_OPS_GO ? A.ops_target : A.target, ok ? "PASS" : "FAIL", note);
}

int main(void) {
  pad_state_t o;
  /* 1 enemy ahead: ROLL OUT vs LAUNCH 3 u straight ahead, facing away */
  match_reset(PRE_ROLL_OUT, 0, -3, 0, PRE_LAUNCH, 0, 0, 0); ai_reset(AI_MEDIUM, 0);
  o = run(1); report("ENEMY AHEAD", o, GOAL_ATTACK, o.ly < 0 && o.lx > -8000 && o.lx < 8000, "full ahead, no turn");

  /* 2 enemy behind, far: turn hard at 0.3 throttle */
  match_reset(PRE_ROLL_OUT, 0, 0, 0, PRE_LAUNCH, 1, -5, 0); ai_reset(AI_MEDIUM, 0);
  o = run(1); report("ENEMY BEHIND FAR", o, GOAL_ATTACK, (o.lx > 30000 || o.lx < -30000) && o.ly > -0.6f * 32767 && o.ly < 0, "turn in place, half throttle");

  /* 3 enemy behind, close: reverse into it */
  match_reset(PRE_ROLL_OUT, 0, 0, 0, PRE_LAUNCH, 0, -1.3f, 0); ai_reset(AI_MEDIUM, 0);
  o = run(1); report("ENEMY BEHIND CLOSE", o, GOAL_ATTACK, o.ly > 0, "reverse");

  /* 4 a spun-up spinner within 3 against a flipper: MEDIUM evades */
  match_reset(PRE_LAUNCH, 0, -2.5f, 0, PRE_CHOP, 0, 0, 3.14159f); M.bots[1].spin = 0.9f; ai_reset(AI_MEDIUM, 0);
  o = run(1); report("SPINNER UP, MEDIUM FLIPPER", o, GOAL_EVADE, 1, "back off, circle 60 deg");

  /* 5 the same for EASY: never evades */
  ai_reset(AI_EASY, 0);
  o = run(1); report("SPINNER UP, EASY FLIPPER", o, GOAL_ATTACK, 1, "EASY never evades");

  /* 6 a wedge on treads does not evade a spinner */
  match_reset(PRE_ROLL_OUT, 0, -2.5f, 0, PRE_CHOP, 0, 0, 3.14159f); M.bots[1].spin = 0.9f; ai_reset(AI_MEDIUM, 0);
  o = run(1); report("SPINNER UP, WEDGE ON TREADS", o, GOAL_ATTACK, 1, "wedges take spinners head on");

  /* 7 a hammer winding up within 1.5 */
  match_reset(PRE_LAUNCH, 0, -1.4f, 0, PRE_STOMP, 0, 0, 3.14159f); M.bots[1].windup = 10; ai_reset(AI_MEDIUM, 0);
  o = run(1); report("HAMMER WINDING UP", o, GOAL_EVADE, 1, "");

  /* 8 low HP against a healthy enemy */
  match_reset(PRE_LAUNCH, 0, -3, 0, PRE_STOMP, 0, 0, 3.14159f); M.bots[0].hp = 20; M.bots[1].hp = 80; ai_reset(AI_MEDIUM, 0);
  o = run(1); report("LOW HP", o, GOAL_EVADE, 1, "20 vs 80");

  /* 9 a spun-down spinner is exposed */
  match_reset(PRE_LAUNCH, 0, -3, 0, PRE_CHOP, 0, 0, 3.14159f); M.bots[1].spin = 0.1f; ai_reset(AI_MEDIUM, 0);
  o = run(1); report("SPINNER SPUN DOWN", o, GOAL_ATTACK, A.goal_score > 0.95f, "exposed: score 1.0");

  /* 10 enemy within 3 of an open pit and my push is bigger: shove it in */
  match_reset(PRE_ROLL_OUT, 0, -1.5f, 0, PRE_STOMP, 0, 0.8f, 3.14159f); arena_reset(1); ai_reset(AI_MEDIUM, 0);   /* TREADS out-push a WALKER */
  o = run(1); report("ENEMY NEAR AN OPEN PIT", o, GOAL_HAZARD_PUSH, o.ly < 0, "shove through them to the pit");

  /* 11 standing on a pit that opens in 0.8 s: still ATTACK, but the stick steps off the square */
  match_reset(PRE_LAUNCH, 0, 3, 0, PRE_STOMP, 0, 8, 3.14159f); arena_reset(2); ai_reset(AI_MEDIUM, 0);
  o = run(1); report("OVER A PIT ABOUT TO OPEN", o, GOAL_ATTACK, o.lx > 16000 || o.lx < -16000, "turns off the square");

  /* 12 inverted with a self-righting flipper */
  match_reset(PRE_LAUNCH, 0, -3, 0, PRE_STOMP, 0, 2, 3.14159f); M.bots[0].inverted = 1; ai_reset(AI_MEDIUM, 0);
  o = run(1); report("INVERTED, CAN SELF-RIGHT", o, GOAL_RECOVER, (o.buttons & IN_B) != 0, "B");

  /* 13 inverted drum: spin up, then A */
  match_reset(PRE_LAUNCH, 0, -3, 0, PRE_STOMP, 0, 2, 3.14159f);
  M.bots[0].build.weapon = WP_DRUM; M.bots[0].inverted = 1; ai_reset(AI_MEDIUM, 0);
  o = run(1); report("INVERTED DRUM", o, GOAL_RECOVER, (o.buttons & IN_A) != 0, "spin up inverted");

  /* 14 my spinner below 0.3 with the enemy within 2: back off and spin up */
  match_reset(PRE_CHOP, 0, -1.6f, 0, PRE_LAUNCH, 0, 0, 3.14159f); M.bots[0].spin = 0.1f; ai_reset(AI_MEDIUM, 0);
  o = run(1); report("MY SPINNER SPUN DOWN", o, GOAL_RECOVER, (o.buttons & IN_A) && o.ly > 0, "reverse holding A");

  /* 15 HARD: a pit opens in 1.5 s next to me and the enemy is chasing */
  match_reset(PRE_ROLL_OUT, 0, 1.2f, 0, PRE_LAUNCH, 0, -2.0f, 0); arena_reset(3);
  M.bots[1].b.vz = 2.0f; ai_reset(AI_HARD, 0);
  o = run(1); report("HARD, PIT OPENING, CHASED", o, GOAL_BAIT, 1, "cross the square");

  /* 16 MEDIUM never baits */
  ai_reset(AI_MEDIUM, 0);
  o = run(1); report("MEDIUM, PIT OPENING, CHASED", o, GOAL_ATTACK, 1, "BAIT is HARD only");

  /* 17 a pickup panel 2 u away at 50 HP, enemy far */
  match_reset(PRE_LAUNCH, 0, 0, 0, PRE_STOMP, 0, 8, 3.14159f); M.bots[0].hp = 50;
  M.ndebris = 1; M.debris[0].x = 2; M.debris[0].z = 0.5f; M.debris[0].life = 200; M.debris[0].pickup = 1; M.debris[0].kind = DB_PANEL;
  ai_reset(AI_MEDIUM, 0);
  o = run(1); report("PICKUP NEARBY", o, GOAL_PICKUP, A.tx == 2.0f, "drive over it");

  /* 18 MELTY never picks up */
  M.bots[0].build.drive = DR_MELTY; M.bots[0].build.weapon = WP_NONE; ai_reset(AI_MEDIUM, 0);
  o = run(1); report("PICKUP NEARBY, MELTY", o, GOAL_ATTACK, 1, "NOT A VACUUM");

  /* 19 my own countout over 300 frames */
  match_reset(PRE_LAUNCH, 0, 0, 0, PRE_STOMP, 0, 8, 3.14159f); M.bots[0].countout = 320; ai_reset(AI_MEDIUM, 0);
  o = run(1); report("PARKED, COUNTOUT 320", o, GOAL_WIGGLE, o.ly != 0, "forward and back");

  /* 20 the enemy is inverted: TAUNT on the 20 percent roll, ATTACK otherwise */
  {
    int taunts = 0, n = 200; pad_state_t first; int got = 0;
    for (int s = 0; s < n; s++) {
      match_reset(PRE_LAUNCH, 0, -3, 0, PRE_STOMP, 0, 0, 0); M.bots[1].inverted = 1; ai_reset(AI_MEDIUM, 0);
      A.rng = 0x1234567u + (uint32_t)s * 2654435761u;
      o = run(1);
      if (A.goal == GOAL_TAUNT) { taunts++; if (!got) { first = o; got = 1; } }
    }
    char note[64]; snprintf(note, sizeof note, "taunted on %d of %d rolls", taunts, n);
    A.goal = got ? GOAL_TAUNT : GOAL_NONE;
    report("ENEMY INVERTED", got ? first : o, GOAL_TAUNT, taunts > n / 10 && taunts < n * 3 / 10 && (first.buttons & IN_X), note);
  }

  /* 21 pinned in a corner by the enemy: the corner escape drives along the longer wall */
  match_reset(PRE_LAUNCH, -11.3f, -7.3f, 3.92699f, PRE_ROLL_OUT, -10.4f, -6.4f, 3.92699f); ai_reset(AI_MEDIUM, 0);
  o = run(1); report("PINNED IN A CORNER", o, GOAL_ATTACK, A.corner_t > 0 && A.corner_dx > 0, "escape along +x for 60 frames");

  /* 22 a flipper with the enemy in its front box fires */
  match_reset(PRE_LAUNCH, 0, -1.0f, 0, PRE_STOMP, 0, 0, 0); ai_reset(AI_HARD, 0);   /* HARD never hesitates on a window */
  o = run(1); report("FLIPPER, ENEMY IN THE BOX", o, GOAL_ATTACK, (o.buttons & IN_A) != 0, "A");

  /* 23 HOVER steers by world direction */
  match_reset(PRE_TOAST, 0, 0, 0, PRE_STOMP, 4, 0, 0); ai_reset(AI_MEDIUM, 0);
  o = run(1); report("HOVER, ENEMY TO THE RIGHT", o, GOAL_ATTACK, o.lx < -20000 && o.ly > -8000 && o.ly < 8000 && o.cam_yaw == 0, "stick right is world -x against cam_yaw 0");

  /* 24 OPS parked, no trigger */
  match_reset(PRE_LAUNCH, 0, -3, 0, PRE_STOMP, 0, 3, 3.14159f); add_ops(OPS_PARKED, -1); M.no_hit_frames = 100; ai_reset(AI_HARD, OPS_SLOT);
  o = run(1); report("OPS PARKED, NO TRIGGER", o, GOAL_NONE, o.lx == 0 && o.ly == 0, "stays behind the gate");

  /* 25 OPS wakes on 720 quiet frames: LEADER TAX on the HP leader */
  match_reset(PRE_LAUNCH, 0, -3, 0, PRE_STOMP, 0, 3, 3.14159f); add_ops(OPS_PARKED, -1); M.no_hit_frames = 720;
  M.bots[0].hp = 60; M.bots[1].hp = 90; ai_reset(AI_HARD, OPS_SLOT);
  o = run(1); report("OPS AWAKE, LEADER TAX", o, GOAL_OPS_GO, A.ops_target == 1, "target the HP leader");

  /* 26 OPS wakes on a pin held 240 frames: the pinner, even when the pinned bot leads */
  match_reset(PRE_LAUNCH, 11.2f, -7.2f, 0.785f, PRE_ROLL_OUT, 10.4f, -6.4f, 0.785f); add_ops(OPS_PARKED, -1);
  M.bots[0].hp = 100; M.bots[1].hp = 40; M.no_hit_frames = 200; ai_reset(AI_HARD, OPS_SLOT);
  o = run(241); report("OPS AWAKE, PIN", o, GOAL_OPS_GO, A.ops_target == 1, "target the pinner");

  /* 27 OPS reaches its target: lift */
  match_reset(PRE_LAUNCH, 0, -3, 0, PRE_STOMP, -9.0f, 6.5f, 0); add_ops(OPS_GO, 1); ai_reset(AI_HARD, OPS_SLOT);
  o = run(1); report("OPS AT THE TARGET", o, GOAL_OPS_LIFT, (o.buttons & IN_A) != 0, "lifter up");

  /* 28 OPS carrying: 3 u toward the centre */
  match_reset(PRE_LAUNCH, 0, -3, 0, PRE_STOMP, -8.0f, 6.0f, 0); add_ops(OPS_CARRY, 1); M.bots[1].lifted_by = OPS_SLOT + 1;
  M.bots[OPS_SLOT].b.x = -9.0f; M.bots[OPS_SLOT].b.z = 6.0f; ai_reset(AI_HARD, OPS_SLOT);
  o = run(1);
  { char note[64]; snprintf(note, sizeof note, "carry point (%.2f, %.2f)", A.carry_x, A.carry_z);
    float d = (A.carry_x + 8) * (A.carry_x + 8) + (A.carry_z - 6) * (A.carry_z - 6);
    report("OPS CARRYING", o, GOAL_OPS_CARRY, (o.buttons & IN_A) && d > 8.9f && d < 9.1f && A.carry_x > -8 && A.carry_z < 6, note); }

  /* 29 OPS turned on its attacker */
  match_reset(PRE_LAUNCH, 0, -3, 0, PRE_STOMP, -6.0f, 4.0f, 0); add_ops(OPS_PUNISH, 0); ai_reset(AI_HARD, OPS_SLOT);
  o = run(1); report("OPS PUNISH", o, GOAL_OPS_PUNISH, A.ops_target == 0 && (o.buttons & IN_A), "lift and shove the attacker");

  /* 30 OPS leaves at T minus 10 whatever it was doing */
  match_reset(PRE_LAUNCH, 0, -3, 0, PRE_STOMP, -6.0f, 4.0f, 0); add_ops(OPS_GO, 1); M.clock_s = 8.0f;
  M.bots[OPS_SLOT].b.x = -3; M.bots[OPS_SLOT].b.z = 0; ai_reset(AI_HARD, OPS_SLOT);
  o = run(1); report("OPS AT T MINUS 8", o, GOAL_OPS_RETURN, 1, "back to the zone");

  /* the counter-pick table (8.4) */
  printf("\nai_pick_build\n");
  static const char *PN[PRE_COUNT] = { "ROLL OUT", "LAUNCH", "CHOP", "STOMP", "TOAST", "SPIN", "RANDOM" };
  static const char *DN[] = { "EASY", "MEDIUM", "HARD" };
  for (int d = AI_EASY; d <= AI_HARD; d++) for (int p = 0; p < PRE_RANDOM; p++) {
    build_t h, b; build_preset(&h, p); uint32_t rng = 99u + (uint32_t)p;
    uint8_t hab[8] = { 0 };
    ai_pick_build(&b, d, &h, hab, &rng);
    printf("  %-6s vs %-8s -> %-8s %s/%s/%s/%s legal %d\n", DN[d], PN[p], build_preset_of(&b) < PRE_RANDOM ? PN[build_preset_of(&b)] : "CUSTOM",
           option_name(&b, ROW_CHASSIS, b.chassis, 1), option_name(&b, ROW_DRIVE, b.drive, 1), option_name(&b, ROW_WEAPON, b.weapon, 1), option_name(&b, ROW_ARMOUR, b.armour, 1), build_legal(&b, 2));
  }
  {
    build_t h, b; build_preset(&h, PRE_LAUNCH); uint32_t rng = 5;
    uint8_t hab[8] = { 0 }; hab[HB_FLIPS] = 2;
    ai_pick_build(&b, AI_HARD, &h, hab, &rng);
    printf("  HARD flipped twice     -> %s/%s/%s/%s legal %d\n", option_name(&b, ROW_CHASSIS, b.chassis, 1), option_name(&b, ROW_DRIVE, b.drive, 1), option_name(&b, ROW_WEAPON, b.weapon, 1), option_name(&b, ROW_ARMOUR, b.armour, 1), build_legal(&b, 2));
    memset(hab, 0, sizeof hab); hab[HB_HAZARD_PUSH] = 1;
    ai_pick_build(&b, AI_HARD, &h, hab, &rng);
    printf("  HARD pushed into pit   -> %s/%s/%s/%s legal %d\n", option_name(&b, ROW_CHASSIS, b.chassis, 1), option_name(&b, ROW_DRIVE, b.drive, 1), option_name(&b, ROW_WEAPON, b.weapon, 1), option_name(&b, ROW_ARMOUR, b.armour, 1), build_legal(&b, 2));
  }

  /* the practice bot panel: cycles every 18 frames, locks at 300 */
  {
    build_t cur, pick; build_preset(&cur, PRE_ROLL_OUT); build_preset(&pick, PRE_CHOP); uint32_t rng = 3; int changes = 0, lock = -1;
    for (int f = 0; f <= 320; f++) { int r = ai_garage_tick(&cur, &pick, f, 2, &rng); if (r == 1) changes++; if (r == 2) lock = f; }
    printf("\npractice panel: %d visible changes, locked at frame %d on %s, equal to pick %d\n", changes, lock, build_preset_of(&cur) == PRE_CHOP ? "CHOP" : "?", memcmp(&cur, &pick, sizeof cur) == 0);
  }
  /* the attract driver: two MEDIUM AIs, distinct presets, THE BAY */
  {
    match_setup_t s; ai_t ais[2];
    for (uint32_t f = 100; f < 103; f++) {
      ai_attract_setup(&s, ais, f);
      printf("attract seed %u: arena %d mode %d %s vs %s, levels %d %d\n", f, s.arena, s.mode, PN[build_preset_of(&s.builds[0])], PN[build_preset_of(&s.builds[1])], ais[0].difficulty, ais[1].difficulty);
    }
  }
  printf("\n%d of %d situations pass\n", total - fails, total);
  return fails;
}
