# DEEP plan: the combat overhaul (physics, graphics, controls, audio)

The brief for four implementers and one integrator. It reconciles the four lens designs in this folder (`deep-physics.md`, `deep-graphics.md`, `deep-controls.md`, `deep-audio.md`) into one event model, one ownership map and one set of gates. Where this plan and a lens design disagree, this plan wins; where this plan is silent, the lens design is the spec. Everything else in the repo is as HANDOFF.md says. House rules apply to every string, comment and doc: ASCII caps on screen, no dashes, no emoji, no real names, GANTRY colours only.

The owner's words, and where each is answered: "not clear how to attack" (section 8, plus DIG IN in 6 and the weapon poses in 7); "battle sound effects aren't very good" (9); "I want damage to show" (6.6 panel pools, 7.4); "models better" (7.2, 7.3); "weapons to damage how they would" (6.4, 6.5); "wedge physics to work" (6.3); "robots that don't register as rigid bodies" (3); "overall better" (10, the feel gate).

## 1. Decisions the designs did not agree on

| Question | Decision |
|---|---|
| The weapon byte on an event: `wid` (graphics) or `weapon` (audio, physics) | `event_t.weapon`, `WP_*` plus the `EVW_*` sentinels of 2.3. Zero on an EV_HIT means the sim branch is not merged; consumers fall back to `bots[a].build.weapon` |
| Closing speed on an event: `speed` (audio) and `j` (physics) | both, as `float speed, j` (2.3) |
| Why a hit was weak: `reason` (controls) and `flags` (physics) | both: `flags` are facts about the contact (EVF_*), `reason` is the one word the HUD shows (HIT_WHY_*, priority in 2.5) |
| EV_SPIN_READY and EV_SAW_TOGGLE (controls), EV_SPIN_KILL (audio), EV_SPIN_CHANGE (physics) | one `EV_SPIN_CHANGE` with zones (armed, lost to a hit, clash, wall, stun); the saw toggle is `EV_WEAPON_TELL` zone TELL_SAW_ON or TELL_SAW_OFF |
| EV_WEAPON_TELL zone numbering (physics and audio both extended it, differently) | the `TELL_*` enum of 2.3; zone 0 stays the hammer windup and the flipper fire (the weapon byte separates them) |
| EV_LIFT and EV_LIFT_DROP (physics) against TELL codes for the lifter (audio) | events, since OPS lifts too |
| The box front plate zone 0.6 (today) against the physics 3.12 zone list that omits it | kept: a box front plate is 0.6 (the rules card and coach beat 3 teach it); the WEDGE chassis front and the WEDGE plate become `loc 0.4` with EVF_DEFLECT against spinners, saws and spikes, 0.6 against everything else |
| Local dent on `dmg >= BAND_HEAVY_MIN` (graphics) against panel pools (physics) | pools. A heavy hit wears 11 or more from a pool of 20 to 44, so one heavy hit reaches state 1 on every armour, which is what graphics asked for |
| Smoke and fire thresholds | fractions 0.40 and 0.20 of max HP everywhere (fx, audio fizz and rattle, the strip colour stays as it is) |
| `WEAPON_HOWTO` in parts.h (controls) | moves to `content.h` (strings live there; parts.h belongs to sim-physics) |
| `juice.h` duck column (audio) | the duck table lives in the band (`audio_duck_band(band)`); juice.h belongs to graphics |
| `fight_audio` in game_flow.c driving the loops (audio) | the band gets `audio_fight_update(const match_t *m)` in audio_pcm.c; main.c calls it once per render frame in a fight; the integrator deletes the whine loop in `fight_audio` at the merge |
| `scrape_t` (audio) | replaced by the ride state (`riding`, `ride_up`) and `dig_t`; the scrape loop reads those |
| `SPINNER_HIT_MIN_PUBLIC` (controls) | `SPINNER_HIT_MIN`, `RAM_MIN_CLOSING` and `RAM_CD` move to sim.h under their `#ifndef` guards |
| `sim_hit_panel` as public API (physics) | sim internal; the event carries `panel`. `sim_spin_energy` and `sim_ridden` are `static inline` in sim.h so every branch links |
| Seat 1's first evening preset becomes LAUNCH (controls) | yes, and ROLL OUT keeps its RAM widget and gets DIG IN, so a player who picks it still has a button |
| First join card 2 s (today) or 3 s (spec 10.24, controls) | 3 s |

## 2. The event model: `src/sim.h` changes, verbatim

Every implementer puts these exact lines in their branch (the integrator lands them on main first as step 0 of section 5; a branch forked before that pastes them). Appended, never reordered; `sim_hash` is unchanged (still x, z, h, y, hp and the live debris), so every replay and `--hashcheck` keeps its meaning. `ev_simple` and `ev_hit` use positional initialisers, so the new trailing fields are zero wherever the sim does not set them.

### 2.1 `body_t`: append after `uint8_t shape;`

```c
  /* DEEP (docs/design/DEEP-plan.md 2.1): orientation and the underride geometry */
  float pitch, wp, roll, wr;          /* pitch about local x, nose up positive; roll about local z, left up positive; rates rad/s */
  float pp, pr;                        /* previous step pitch and roll, interpolated like h */
  float clr, hgt, lip_f, lip_b;        /* ground clearance, chassis height, front and back lip heights over the floor (0 = no wedge on that edge) */
  float inertia_p, inv_inertia_p;      /* pitch inertia m (hl^2 + (hgt/2)^2) / 3; OPS has inverse 0 */
```

### 2.2 `bot_t`: append after `int16_t skirt_t;`

```c
  /* DEEP 2.2: the underride (ride_by and riding are slot + 1, 0 for none) */
  uint8_t ride_by, riding, ride_edge, wheel_flag; int16_t ride_t; float ride_up, ride_s, ride_cl_peak;
  /* DEEP 2.2: per panel pools, PN_FRONT..PN_MOUNT; 0 means no pool (AR_NONE) */
  float panel_hp[6]; int16_t panel_wear_cd[6];
  /* DEEP 2.2: contact and status timers the renderer and the band read */
  int16_t grind_t, slide_t, tread_hit, dig_t, dig_cd; float grind_x, grind_z; int8_t wheel_hit_side;
  /* DEEP 2.2: HUD readiness, written every step, never read by the sim */
  float ram_closing; int16_t hit_flash_t, spent_t; uint8_t weapon_ready, can_self_right, self_right_key, last_hit_reason;
```

### 2.3 Events: replace the `ev_type_t` and `event_t` declarations with these

```c
typedef enum { EV_HIT, EV_CLASH, EV_WALL, EV_RAM, EV_LAUNCH, EV_LAND, EV_INVERT, EV_SELFRIGHT, EV_SHED, EV_BURST,
               EV_KO, EV_MINI_KO, EV_COUNT_TICK, EV_HAZ_TELL, EV_HAZ_HIT, EV_OPS_WAKE, EV_OPS_LEAVE, EV_OPS_LINE,
               EV_WEAPON_TELL, EV_TAUNT, EV_BOOST, EV_HILL_MOVE, EV_RED_TAG, EV_PICKUP, EV_NOT_VACUUM,
               EV_BENCH_BREAK, EV_BUMPER_BREAK, EV_SPIN_WALL, EV_CLOSING, EV_STUCK, EV_HILL_SCORE, EV_RESPAWN,
               /* DEEP 2.3, appended */
               EV_UNDERRIDE, EV_RIDE_END, EV_WEDGE_CLASH, EV_WHEEL_HIT, EV_PANEL_WEAR, EV_SCRAPE, EV_GRIND,
               EV_BURN_START, EV_BURN_END, EV_SPIN_CHANGE, EV_SLIDE, EV_LIFT, EV_LIFT_DROP, EV_FALL, EV_TYPE_COUNT } ev_type_t;
/* a is the attacker or subject bot, b the victim or the hazard index; zone is the hit zone times 10 for hits,
 * the panel for EV_SHED, the KO cause for EV_KO, and the per event code of the enums below otherwise.
 * DEEP 2.3: panel is the PN_* the contact landed on; weapon is WP_* or an EVW_* sentinel; flags are EVF_*;
 * reason is HIT_WHY_*; speed is the closing speed (u/s) or the spin before a change; j is the impulse, the
 * launch vy or the pitch rate, per the table in the plan. Unset fields are 0. */
typedef struct { uint8_t type, a, b, zone; float x, z, y, dmg, nx, nz;
                 uint8_t panel, weapon, flags, reason; float speed, j; } event_t;
/* event_t.weapon beyond the WP_* table */
enum { EVW_MELTY = WP_COUNT, EVW_HAZARD, EVW_OPS, EVW_LAND, EVW_WALL, EVW_NONE = 255 };
/* event_t.flags */
#define EVF_CORPSE   1    /* the victim is a counted out or flipped out corpse: dmg 0, no tallies */
#define EVF_DEFLECT  2    /* a spinner, saw or spike glanced off a wedge front (loc 0.4) */
#define EVF_WHEEL    4    /* the contact was on a wheel, track or leg station */
#define EVF_STRIPPED 8    /* the panel there is gone: reduction 0, loc 1.25 */
#define EVF_TOP      16   /* the TOP panel: hammer, pulverizer, drop, landing on a bot, crusher hold */
/* event_t.reason: the one word the HUD shows under a landed number (priority order in the plan) */
enum { HIT_WHY_NONE, HIT_WHY_FRONT, HIT_WHY_WEDGE, HIT_WHY_SPINNING_DOWN, HIT_WHY_SHELL, HIT_WHY_ARMOUR,
       HIT_WHY_WALL, HIT_WHY_EXPOSED, HIT_WHY_STRIPPED, HIT_WHY_WHEEL, HIT_WHY_COUNT };
/* EV_WEAPON_TELL zone */
enum { TELL_WINDUP, TELL_HAMMER_MISS, TELL_DIG_IN, TELL_SPIKE_OUT, TELL_SPIKE_BACK, TELL_CLAMP_START, TELL_CLAMP_BITE,
       TELL_CLAMP_RELEASE, TELL_FLAME_ON, TELL_FLAME_OUT, TELL_FLIPPER_RESET, TELL_SAW_ON, TELL_SAW_OFF, TELL_COUNT };
/* zones of the other events */
enum { RIDE_SLIP, RIDE_BACKED_OFF, RIDE_TIPPED, RIDE_CARRIED_OFF, RIDE_OPS };      /* EV_RIDE_END */
enum { LAUNCH_FLIPPER, LAUNCH_RAMP, LAUNCH_WEDGE, LAUNCH_DRUM };                    /* EV_LAUNCH */
enum { LAND_WHEELS, LAND_INVERTED, LAND_BOUNCE, LAND_ON_BOT };                      /* EV_LAND */
enum { INV_FLIPPER, INV_WEDGE, INV_HAZARD, INV_ROLL };                              /* EV_INVERT */
enum { SPIN_LOST_HIT, SPIN_LOST_CLASH, SPIN_LOST_WALL, SPIN_ARMED, SPIN_LOST_STUN };/* EV_SPIN_CHANGE */
enum { FALL_ROOF, FALL_PIT };                                                       /* EV_FALL */
enum { SCRAPE_FLOOR, SCRAPE_WALL };                                                 /* EV_SCRAPE */
enum { GRIND_START, GRIND_END, GRIND_TICK };                                        /* EV_GRIND */
```

### 2.4 Constants and helpers: add before the API block

```c
/* DEEP 2.4: balance knobs the HUD reads (moved from sim.c; the -D overrides stay) */
#ifndef SPINNER_HIT_MIN
#define SPINNER_HIT_MIN 0.5f
#endif
#ifndef RAM_MIN_CLOSING
#define RAM_MIN_CLOSING 1.5f
#endif
#ifndef RAM_CD
#define RAM_CD 90
#endif
/* DEEP 2.4: derived state every branch can link against */
static inline float sim_spin_energy(const bot_t *b) { return b->spin * b->spin; }
static inline int   sim_ridden(const bot_t *b) { return b->ride_by != 0; }
/* DEEP 2.4: the thrown track (physics 3.10), after the existing DB_* kinds */
enum { DB_TREAD = DB_BUMPER + 1 };
```

The sim-physics branch deletes the three `#ifndef` copies in sim.c. Other branches leave sim.c alone (its copies are guarded, so there is no redefinition).

### 2.5 Payload per event (what physics writes, what the others read)

`a` attacker or subject, `b` victim. Unlisted fields are 0.

| Event | zone | dmg | speed | j | panel, weapon, flags, reason, x z y, nx nz |
|---|---|---|---|---|---|
| EV_HIT | hit zone x10 | after armour (0 with EVF_CORPSE) | closing speed before the clamp (a standing spinner reports its tip 3.6) | knock impulse J | panel the contact landed on; weapon WP_* or EVW_MELTY, EVW_OPS; flags; reason; x z the contact, y its height (hammer: the victim's top; else `v.y + clr + hgt/2`); nx nz from a to b |
| EV_RAM | as EV_HIT | 9 to 18 | closing speed | J | weapon WP_WEDGE for a wedge lip, WP_NONE for a bare front; panel the victim's edge |
| EV_CLASH, EV_SPIN_WALL, EV_WALL | as today | as today | vn (closing into the wall) for the walls | J | EV_SPIN_WALL reason HIT_WHY_WALL, weapon of the spinner; EV_WALL weapon EVW_WALL |
| EV_UNDERRIDE | victim edge PN_FRONT..PN_RIGHT | closing speed | closing speed | | x z the lip |
| EV_RIDE_END | RIDE_* | | | lift at exit | |
| EV_WEDGE_CLASH | | closing speed | closing speed | | x z the lips |
| EV_WHEEL_HIT | | dmg of the hit | | | panel PN_LEFT or PN_RIGHT, weapon |
| EV_LAUNCH | LAUNCH_* | vy | vy | wp (rad/s) | b the flier |
| EV_LAND | LAND_* | landing damage | impact vy (abs) | | panel PN_TOP when inverted; weapon EVW_LAND |
| EV_INVERT | INV_* | | | | a the victim, b the launcher |
| EV_SELFRIGHT | 0 done, 1 SHELL roll, 2 OPS righted | | | | |
| EV_PANEL_WEAR | new state 1..3 | pool left | | | panel; x z y the panel centre |
| EV_SHED | panel (as today) | the state it had | | | nx nz the debris direction (unit); zone 8 + wheel index for a lost wheel, DB_TREAD debris for a thrown track |
| EV_SCRAPE | SCRAPE_* | speed along | speed along | | |
| EV_GRIND | GRIND_* | tick damage on GRIND_TICK | | | weapon WP_SAW or WP_CRUSHER, panel, x z the contact |
| EV_BURN_START, EV_BURN_END | | | | | a lighter, b the burning bot |
| EV_SPIN_CHANGE | SPIN_* | spin lost (spin for SPIN_ARMED) | spin before | | weapon |
| EV_SLIDE | | lateral speed | lateral speed | | nx nz the slide direction |
| EV_LIFT, EV_LIFT_DROP | 0 lifter, 1 OPS | | | | |
| EV_FALL | FALL_* | | | | a the faller |
| EV_WEAPON_TELL | TELL_* | | | | weapon set on every one (a 2ND weapon tells too) |
| EV_HAZ_HIT | as today | as today | | | panel PN_TOP for pulverizers and drops, weapon EVW_HAZARD |
| everything else | as today | | | | |

`reason` priority in `weapon_hit`, first that applies: EXPOSED (zone 1.5 or 2.0), STRIPPED (EVF_STRIPPED), WHEEL (EVF_WHEEL), WEDGE (EVF_DEFLECT), FRONT (zone 0.6), SPINNING_DOWN (spinner energy term under 0.75), SHELL (0.8), ARMOUR (reduction 0.35 or over and dmg under half the raw), NONE. `ram_check` writes WEDGE on an EV_RAM that was deflected; `walls_step` writes WALL on EV_SPIN_WALL.

### 2.6 Per frame render and audio state (not events)

Read straight from `bot_t` and `body_t`: `pitch, roll` (interpolated with `pp, pr`), `ride_by, riding, ride_up`, `grind_t, grind_x, grind_z`, `slide_t`, `dig_t`, `burn_t`, `wheel_hit, wheel_hit_side, tread_hit`, `panel_state[], panel_hp[]`, `spin, sim_spin_energy`, `lift, clamp, lock, windup, active, cooldown, fuel, saw_on`, `ram_closing, hit_flash_t, spent_t, weapon_ready, can_self_right, self_right_key, last_hit_reason`, `b.vx vz w`, `airborne, lifted_by, clamped_by, inverted, alive, hp, hp_max`, `M.haz[i].active, aux`.

## 3. Ghost bots: three causes, three owners

1. The renderer draws the empty sim slots (the owner's report). `sim_init` sets `nbots = 5` whenever OPS exists; `world_sync` (`src/world.c:1888`) forces every slot under `nbots` visible and `sync_bot` parks a zeroed bot at the origin as a wheel-less chassis. Fix, graphics: `if (!(bt->hp_max > 0 || bt->is_ops)) { if (b->visible) world_bot_visible(i, 0); continue; }` before the sync, and `world_sync` never forces visibility on; `game_start_match` and `game_attract_start` stay the owners of visibility. Log `[world] bots=0x<mask>` on every visibility change in a fight. Acceptance: `solo.json` 900 and `title.json` 700, 1300 show nothing on THE BAY pit; the log reads `bots=0x13` through the fight and the attract, `0x1f` in `4p-fight.json`, `0x13` in every `mode-tourn.json` bracket fight.
2. Corpses that collide but cannot be hit (a counted out or flipped out bot is a 0.5 mass prop that `hit_test`, `ram_check` and `flame_step` skip). Fix, sim-physics: a corpse (`!alive && hp_max > 0 && !falling && pit_state != 2`) is a valid victim with dmg 0: `weapon_hit` runs the contact (knock, recoil, half the spin cost, `EV_HIT` with EVF_CORPSE), no tallies, no count reset, hit cooldown 30. Acceptance: scenario `corpse-hittable` in 6.8.
3. The height gap pair test (`|ya - yb| > 0.35` skips the pair; `hit_test` 0.6) lets bots on the mound rim, the ramps, the deck and in OPS's forks pass through others. Fix, sim-physics: two grounded bots are always a pair unless exactly one is `on_deck`; otherwise (airborne, lifted, clamped) the vertical intervals `[y + clr, y + clr + hgt]` must overlap by 0.05. Acceptance: scenario `mound-contact` and `ghost-sim`.

## 4. Ownership and the build-alone rule

| Implementer | Files owned | Nobody else touches |
|---|---|---|
| sim-physics | `src/sim.c`, `src/sim.h`, `src/bot.c`, `src/bot.h`, `src/parts.h`, `src/arena.c`, `src/arena.h`, `src/ai.c` (steering and attack edge only), `tools/balance.c`, `tools/scripts/*fight*.json`, `feel.json`, the new fixtures | |
| graphics | `src/world.c`, `src/world.h`, `src/fx.c`, `src/fx.h`, `src/juice.h`, `src/render.c`, `src/camera.c`, `tools/states.txt`, `src/gen/rtr_gl.c` (regenerated) | |
| controls-hud | `src/game_flow.c`, `src/draw.c`, `src/draw.h`, `src/draw_menus.c`, `src/content.h`, `src/hud.c`, `src/hud.h`, `src/glyphs.h`, `src/keys.h`, `src/keys.c` (new; build.sh already compiles every `src/*.c`), `src/game.h` (the save field only), `web/index.html`, `README.md` controls section, `tools/scripts/probe-chop.json` and `probe-rollout.json` (new) | |
| audio | `src/audio_band.cpp`, `src/audio_pcm.c`, `src/audio.h`, `tools/bake_music.cpp`, `assets/music/*`, `tools/wavstat.mjs` (new, 9) | |
| integrator | `src/main.c`, `src/game.c`, `build.sh`, `tools/shot.mjs`, `tools/play.mjs`, `HANDOFF.md`, `web/red-tag-rumble.wasc` | applies every patch the implementers list in their reports |

Rules:

- `src/sim.h` carries the identical block of section 2 in every branch (step 0 puts it on main, so no branch edits sim.h for it). Every name another branch reads is in that block. sim-physics may add sim internal state that no other branch reads (load and carry timers, tip state and the like) as one trailing `/* DEEP sim internals */` line at the end of `bot_t` or `match_t`, never inside or before the shared block; new sim constants go in `parts.h` or sim.c.
- A branch builds alone with `./build.sh` and `./build.sh balance` (balance is sim-physics only; the others must still not break it: audio.h and sim.h stay headless, `audio_pcm.c` may include `sim.h`).
- New calls into a sibling's new API are made only from main.c, and main.c already carries them (section 5) under `#ifdef RTR_DEEP_WORLD`, `RTR_DEEP_HUD`, `RTR_DEEP_AUDIO`. An implementer builds their branch with `EXTRA_CFLAGS=-DRTR_DEEP_<LENS>` to reach their own new entry points through the drain; the integrator removes the three ifdefs at the merge. Nothing else in main.c or game.c is edited on a branch: every other change is a patch (file, function, before and after) in the implementer's report.
- Stubs when the sim branch is not merged: `e->weapon == 0` on an EV_HIT means derive from `bots[a].build.weapon`; `e->speed == 0` means derive a velocity from dmg (`0.3 + 0.05 dmg`); `e->y == 0` means `v.y + 0.3`; `pitch, roll, ride_up, grind_t, dig_t, panel_hp` read 0 and every consumer must render and sound exactly as today when they do; EV_PANEL_WEAR never arrives, so the dent swap keeps following `panel_state`.
- Every implementer regenerates nothing shared: the fight fixtures are regenerated once by sim-physics on its branch (its sim changes move every hit) and once by the integrator after the merge (the default preset change moves them again). Acceptance frame numbers quoted below are today's; a report states the new frames it looked at.
- Commits: as virgilvox, one plain line, no build output, no `web/*.wasc` (the integrator rebuilds the cart).

## 5. Integration

### 5.1 Step 0, before the branches fork

The integrator commits on main: the section 2 block in `src/sim.h`; `src/sim.c` with the three `#ifndef` copies removed (the header now has them); the `ev_simple` and `ev_hit` initialisers padded with `0, 0, 0, 0, 0, 0`; the drain of 5.2 in `src/main.c`; `#define RTR_DEEP_NONE` nowhere (the ifdef groups are off). Build, `--hashcheck`, `--replay` of `2p-fight.json`: identical to 22c05b6. Then the four worktrees fork.

The 22c05b6 reference: `--hashcheck` PASS (all six arenas identical across two runs); the cart on `2p-fight.json` logs the setup line `mode=0 arena=1 seed=1401181144 start=668 b0=1,3,1,4 b1=2,1,9,2 ... pads=1,2,0,0 park=0`, 42 `[sim] f= h=` lines from `f=60 h=e8c4cf73` to `f=2520 h=a15394e6`, and `--replay` with that line prints the same 42 and `end f=2560 over=1 winner=1 how=HP ZERO`.

With the three groups off the drain must sound and shake exactly as at 22c05b6 (only the number colours and heights change, which is the point): every audio call the old drain made outside the banded path is kept under `NOAUDIO(...)`, which expands only when `RTR_DEEP_AUDIO` is off.

### 5.2 `src/main.c` drain, pre-written

Replace `drain_events` and its helpers with this. The integrator fills nothing except the merge (deleting the `#ifdef` lines and the `#else` branches).

```c
/* -- the event drain (DEEP-plan 5.2): one place fans every sim event out to fx, HUD, audio, camera, pads -- */
static int16_t hit_db[MAX_BOTS];           /* one impact sound per victim slot per 2 frames */
static int armour_of(int slot) { return slot >= 0 && slot < M.nbots ? M.bots[slot].build.armour : -1; }
static int weapon_of(const event_t *e) {   /* 0 on an EV_HIT means the sim branch is not merged yet */
  if (e->weapon || e->type != EV_HIT) return e->weapon;
  return e->a < M.nbots ? M.bots[e->a].build.weapon : EVW_NONE;
}
static float hit_y(const event_t *e) { return e->y > 0 ? e->y : (e->b < M.nbots ? M.bots[e->b].b.y + 0.3f : 0.3f); }
/* a world anchored number in every live viewport: the attacker's view in its seat colour, the victim's in rust, others bone */
static void number_views(float x, float y, float z, int value, int attacker, int victim, int big) {
  rect_t vp[4]; int n = 0; cam_current(vp, &n);
  for (int i = 0; i < n && i < 4; i++) {
    int s = cam_seat_of(i), as = slot_seat(attacker), vs = slot_seat(victim);
    uint32_t c = (s >= 0 && s == as) ? seat_colour(s) : (s >= 0 && s == vs) ? C_RUST : C_BONE;
    damage_number(i, x, y, z, value, c, big);
  }
}
#ifdef RTR_DEEP_AUDIO
static int hk_of(const event_t *e) {
  if (e->type == EV_RAM || e->type == EV_WEDGE_CLASH) return HK_RAM;
  if (e->type == EV_WALL || e->type == EV_SPIN_WALL) return HK_WALL;
  if (e->type == EV_LAND) return HK_LAND;
  if (e->type == EV_HAZ_HIT) {
    int ht = e->b < ARENAS[M.arena].nhaz ? ARENAS[M.arena].haz[e->b].type : -1;
    switch (ht) { case HZ_PULVERIZER: return HK_HAZ_PULVERIZER; case HZ_KILLSAW: return HK_HAZ_KILLSAW; case HZ_FLAME: return HK_HAZ_FLAME;
                  case HZ_SCREW: return HK_HAZ_SCREW; case HZ_LASER: return HK_HAZ_LASER; case HZ_DROPZONE: return HK_HAZ_DROP; case HZ_PIT: return HK_HAZ_PIT; default: return HK_GENERIC; }
  }
  switch (weapon_of(e)) {
    case WP_HORIZ: case WP_DRUM: case EVW_MELTY: return HK_SPINNER;  case WP_SAW: return HK_SAW;  case WP_HAMMER: return HK_HAMMER;
    case WP_SPIKE: return HK_SPIKE;  case WP_FLIPPER: return HK_FLIPPER;  case WP_FLAME: return HK_FLAME;  case WP_CRUSHER: return HK_CRUSHER;
    case WP_LIFTER: return HK_LIFTER;  case EVW_OPS: return HK_OPS;  default: return HK_GENERIC;
  }
}
static float vel_of(const event_t *e) {
  float d = (e->type == EV_WALL || e->type == EV_SPIN_WALL) ? 6.0f : e->type == EV_LAND ? 7.0f : 8.0f;
  return rtr_clamp(e->speed > 0 ? e->speed / d : 0.3f + 0.05f * e->dmg, 0, 1);
}
#define AUDIO(x) x
#define NOAUDIO(x)
#else
#define AUDIO(x)
#define NOAUDIO(x) x                     /* the 22c05b6 sound, until the band lands */
#endif
#ifdef RTR_DEEP_HUD
#define HUD(x) x
#else
#define HUD(x)
#endif
#ifdef RTR_DEEP_WORLD
#define WORLD(x) x
#else
#define WORLD(x)
#endif

static void drain_events(void) {
  for (int i = 0; i < MAX_BOTS; i++) if (hit_db[i] > 0) hit_db[i]--;
  if (G.attract) {                       /* the title attract: sparks and smoke, no sound, no stingers */
    for (int i = 0; i < M.nev; i++) { const event_t *e = &M.ev[i]; if (e->type == EV_HIT || e->type == EV_RAM || e->type == EV_CLASH) fx_hit(e, e->type == EV_CLASH ? BAND_CLASH : hit_band(e->dmg)); else fx_event(e, &M); }
    M.nev = 0; return;
  }
#ifndef RTR_DEEP_AUDIO
  int sfx_band_done = 0;                 /* one SFX per band per frame (the old rule) */
#endif
  for (int i = 0; i < M.nev; i++) {
    const event_t *e = &M.ev[i];
    int band = -1;
    switch (e->type) {
      case EV_HIT: case EV_RAM: band = e->dmg > 0 ? hit_band(e->dmg) : (e->flags & EVF_CORPSE) ? BAND_TAP : -1; break;
      case EV_CLASH: band = BAND_CLASH; break;
      case EV_KO: band = BAND_KO; break;
      default: break;
    }
    int a = e->a < M.nbots ? e->a : -1, b = e->b < M.nbots ? e->b : -1;
    if (band >= 0) {
      const hit_band_t *hb = &HIT_BANDS[band];
      int vs = slot_seat(b), as = slot_seat(a);
      hit_log(e, band);
      fx_hit(e, band);                                           /* reads weapon, panel, flags, y (graphics 7.4) */
      if (band == BAND_KO || band == BAND_CLASH) cam_shake_all(hb->shake);
      else { if (vs >= 0) cam_shake(vs, hb->shake); if (as >= 0) cam_shake(as, hb->shake * 0.5f); }
      if (vs >= 0) cam_punch(vs, e->nx, e->nz, hb->punch);
      if (as >= 0 && band >= BAND_HEAVY) cam_punch(as, -e->nx, -e->nz, hb->punch * 0.5f);
#ifdef RTR_DEEP_AUDIO
      if (b < 0 || hit_db[b] == 0) { audio_hit(hk_of(e), band, vel_of(e), e->x, e->z, armour_of(b), a); audio_duck_band(band); if (b >= 0) hit_db[b] = 2; }
      if (band == BAND_KO) audio_crowd_react(CROWD_KO); else if (band >= BAND_HUGE) audio_crowd_react(CROWD_ROAR); else if (band == BAND_HEAVY) audio_crowd_react(CROWD_OOOH);
#else
      if (!(sfx_band_done & (1 << band))) { audio_play(hb->sfx); audio_duck_hit(); sfx_band_done |= 1 << band; }
      if (hb->crowd) audio_crowd(hb->crowd / 100.0f);
#endif
      if (band == BAND_KO || band == BAND_CLASH) { rumble(seat_pad(b), hb->rum_lo, hb->rum_hi, hb->rum_ms); rumble(seat_pad(a), hb->rum_lo, hb->rum_hi, hb->rum_ms); }
      else { rumble(seat_pad(b), hb->rum_lo, hb->rum_hi, hb->rum_ms); rumble(seat_pad(a), 0, hb->rum_hi, hb->rum_ms / 2); }
      if (e->type == EV_CLASH) { text_all(e->x, e->y + 0.6f, e->z, ST_CLASH[0], C_BONE); sting_slot(b, PICK(ST_CLASH), C_BONE); sting_slot(a, ST_CLASH[0], C_BONE); }
      else if (band != BAND_KO && e->dmg > 0) {
        number_views(e->x, hit_y(e) + 0.4f, e->z, dmg_int(e->dmg), a, b, e->dmg >= J_NUMBER_BIG_MIN);
        HUD(hit_marker_all(e->x, hit_y(e), e->z, band, slot_colour(a, C_BONE)));
        HUD(if (vs >= 0) strip_taken(vs, band));                        /* the HP bar flash and drain, the rust viewport frame */
        HUD(if (as >= 0) strip_dealt(as));                              /* the live tick on the attacker's bar and the widget word HIT */
        HUD(if (as >= 0 && (e->reason || e->zone < 10)) hit_mark_add(cam_viewport_of(as), e->reason ? e->reason : HIT_WHY_FRONT, e->x, hit_y(e), e->z));
      }
      WORLD(if (b >= 0 && band >= BAND_HEAVY) world_bot_hit(b, e->x, hit_y(e), e->z, e->nx, e->nz, weapon_of(e), e->panel, band));
      WORLD(if (band == BAND_KO) world_scorch(e->x, e->z, 0.9f));
      if (band != BAND_KO && band != BAND_CLASH && e->dmg >= J_STINGER_BIGHIT_MIN) sting_slot(b, PICK(ST_BIGHIT), slot_colour(a, C_BONE));
      game_on_event(e);
      continue;
    }
    fx_event(e, &M);
    switch (e->type) {
      case EV_LAUNCH:
        AUDIO(audio_play_at(e->zone == LAUNCH_WEDGE ? SFX_LAUNCH_WEDGE : SFX_LAUNCH, 0, e->x, e->z)); AUDIO(audio_play_at(SFX_LAUNCH_WHOOSH, 0, e->x, e->z));
        NOAUDIO(audio_play(SFX_LAUNCH));
        rumble(seat_pad(b), 0.3f, 0.7f, 80);
        WORLD(if (b >= 0) fx_launch(b, e->x, e->y, e->z, M.bots[b].b.vx, M.bots[b].b.vy, M.bots[b].b.vz, slot_colour(b, C_BONE)));
        { const char *s = PICK(ST_AIRBORNE); sting_slot(b, s, slot_colour(a, C_BONE)); if (a != b) sting_slot(a, s, slot_colour(a, C_BONE)); }
        break;
      case EV_LAND:
        AUDIO(audio_hit(HK_LAND, e->zone == LAND_INVERTED ? BAND_HEAVY : hit_band(e->dmg), vel_of(e), e->x, e->z, armour_of(b), a));
        NOAUDIO(audio_play_var(SFX_HIT_LIGHT, SFXV_LOW));
        if (e->dmg > 0.5f) { cam_shake(slot_seat(b), J_LAND_SHAKE); number_views(e->x, e->y + 0.6f, e->z, dmg_int(e->dmg), a, b, 0); }
        break;
      case EV_WALL:
        AUDIO(audio_hit(HK_WALL, e->dmg > 0.5f ? BAND_HIT : BAND_TAP, vel_of(e), e->x, e->z, -1, a));
        NOAUDIO(audio_play_var(SFX_HIT_LIGHT, SFXV_LOW));
        cam_shake(slot_seat(a), J_WALL_SHAKE);
        if (e->dmg > 0.5f) number_views(e->x, e->y + 0.4f, e->z, dmg_int(e->dmg), -1, a, 0);
        break;
      case EV_SPIN_WALL:
        AUDIO(audio_hit(HK_WALL, BAND_HEAVY, 1.0f, e->x, e->z, -1, a)); NOAUDIO(audio_play_var(SFX_CLASH, SFXV_LOW)); cam_shake(slot_seat(a), J_SPINWALL_SHAKE);
        number_views(e->x, e->y + 0.4f, e->z, dmg_int(e->dmg), -1, a, 1);
        HUD(if (slot_seat(a) >= 0) hit_mark_add(cam_viewport_of(slot_seat(a)), HIT_WHY_WALL, e->x, e->y + 0.4f, e->z));
        break;
      case EV_UNDERRIDE: AUDIO(audio_play_at(SFX_UNDERRIDE, 0, e->x, e->z)); rumble(seat_pad(b), 0.2f, 0.4f, 120); HUD(sting_slot(b, PICK(ST_WEDGED), slot_colour(a, C_BONE))); break;
      case EV_RIDE_END: AUDIO(if (e->zone == RIDE_SLIP || e->zone == RIDE_BACKED_OFF || e->zone == RIDE_CARRIED_OFF) audio_play_at(SFX_RIDE_DROP, e->zone, e->x, e->z)); break;
      case EV_WEDGE_CLASH:
        AUDIO(audio_hit(HK_RAM, BAND_HIT, vel_of(e), e->x, e->z, -1, a));
        cam_shake(slot_seat(a), J_WALL_SHAKE); cam_shake(slot_seat(b), J_WALL_SHAKE); rumble(seat_pad(a), 0.3f, 0.3f, 60); rumble(seat_pad(b), 0.3f, 0.3f, 60);
        break;
      case EV_WHEEL_HIT: AUDIO(audio_play_at(SFX_WHEEL_HIT, 0, e->x, e->z)); HUD(sting_slot(b, PICK(ST_LIMPING), C_RUST)); break;
      case EV_PANEL_WEAR: AUDIO(audio_play_at(SFX_DENT, e->zone, e->x, e->z)); break;
      case EV_SHED: AUDIO(audio_play_at(SFX_SHED, 0, e->x, e->z)); HUD(if (slot_seat(b) >= 0) strip_panel_gone(slot_seat(b), e->zone)); break;
      case EV_BURST: AUDIO(audio_play_at(SFX_BURST, 0, e->x, e->z)); WORLD(world_scorch(e->x, e->z, 0.9f)); break;
      case EV_SCRAPE: AUDIO(audio_play_at(SFX_SCRAPE, e->zone, e->x, e->z)); if (e->zone == SCRAPE_WALL) cam_shake(slot_seat(a), J_WALL_SHAKE * 0.5f); break;
      case EV_GRIND: AUDIO(if (e->zone == GRIND_START) audio_play_at(SFX_GRIND_BITE, 0, e->x, e->z)); break;   /* the loop itself follows grind_t in audio_fight_update */
      case EV_BURN_START: AUDIO(audio_play_at(SFX_BURN_START, 0, e->x, e->z)); break;
      case EV_BURN_END: AUDIO(audio_play_at(SFX_BURN_END, 0, e->x, e->z)); break;
      case EV_SPIN_CHANGE:
        if (e->zone == SPIN_ARMED) { AUDIO(audio_play_at(SFX_SPIN_READY, 0, e->x, e->z)); }
        else { AUDIO(audio_spin_change(a, e->zone, e->speed, e->dmg)); if (e->zone != SPIN_LOST_HIT) rumble(seat_pad(a), 0.4f, 0.2f, 120); }
        break;
      case EV_SLIDE: AUDIO(audio_play_at(SFX_SKID, a >= 0 ? M.bots[a].build.drive : 0, e->x, e->z)); break;
      case EV_LIFT: AUDIO(audio_play_at(SFX_LIFT, e->zone, e->x, e->z)); break;
      case EV_LIFT_DROP: AUDIO(audio_play_at(SFX_LIFT_DROP, e->zone, e->x, e->z)); break;
      case EV_FALL: AUDIO(audio_play_at(SFX_FALL, e->zone, e->x, e->z)); break;
      case EV_WEAPON_TELL:
        AUDIO(audio_tell(a, weapon_of(e), e->zone, e->x, e->z));
        if (e->zone == TELL_DIG_IN && a >= 0) cam_punch(slot_seat(a), -rtr_sin(M.bots[a].b.h), -rtr_cos(M.bots[a].b.h), 0.04f);
        break;
      case EV_BOOST: AUDIO(audio_play_at(SFX_BOOST, 0, e->x, e->z)); break;
      case EV_SELFRIGHT: AUDIO(audio_play_at(SFX_SELFRIGHT, e->zone, e->x, e->z)); break;
      case EV_STUCK: AUDIO(audio_play_at(SFX_STUCK, 0, e->x, e->z)); break;
      case EV_BENCH_BREAK: AUDIO(audio_play_at(SFX_BENCH, 0, e->x, e->z)); break;
      case EV_BUMPER_BREAK: AUDIO(audio_play_at(SFX_BUMPER, 0, e->x, e->z)); break;
      case EV_INVERT: audio_play_var(SFX_KLAXON, SFXV_SHORT); AUDIO(audio_crowd_react(CROWD_ROAR)); sting_slot(a, PICK(ST_INVERTED), slot_colour(b, C_BONE)); break;
      case EV_COUNT_TICK:
        if (a >= 0 && M.bots[a].deck_count == 60) { audio_play_var(SFX_KLAXON, SFXV_TELL); sting_slot(a, PICK(ST_DECK), C_RUST); }
        else if (a >= 0 && M.bots[a].inverted) audio_play_var(SFX_KLAXON, SFXV_LOW);
        else audio_play_var(SFX_COUNT, SFXV_SHORT);
        break;
      case EV_HAZ_TELL: audio_play_var(SFX_KLAXON, SFXV_TELL); AUDIO(audio_haz_tell(e->b, e->x, e->z)); break;
      case EV_HAZ_HIT: {
        int ht = e->b < ARENAS[M.arena].nhaz ? ARENAS[M.arena].haz[e->b].type : -1;
        int heavy = e->dmg >= 15;
#ifdef RTR_DEEP_AUDIO
        audio_hit(hk_of(e), heavy ? BAND_HEAVY : hit_band(e->dmg), 1.0f, e->x, e->z, armour_of(a), -1);
#else
        if (heavy) audio_play(SFX_HIT_HEAVY); else audio_play_var(SFX_HIT_LIGHT, SFXV_LOW);
#endif
        rumble(seat_pad(a), heavy ? 0.6f : 0.2f, heavy ? 0.6f : 0.3f, heavy ? 100 : 50);
        cam_shake(slot_seat(a), heavy ? J_HAZARD_SHAKE : J_HAZARD_SHAKE * 0.4f);
        if (e->dmg > 0.5f) number_views(e->x, e->y + 0.6f, e->z, dmg_int(e->dmg), -1, a, heavy);
        HUD(if (slot_seat(a) >= 0) strip_taken(slot_seat(a), heavy ? BAND_HEAVY : BAND_HIT));
        WORLD(if (ht == HZ_FLAME || ht == HZ_LASER) world_scorch(e->x, e->z, 0.5f));
        if (e->zone == KO_PIT) sting_slot(a, PICK(ST_PIT), C_RUST); else if (ht == HZ_LASER) sting_slot(a, PICK(ST_DUCK), C_RUST);
        break; }
      /* EV_OPS_WAKE, EV_OPS_LINE, EV_OPS_LEAVE, EV_RED_TAG, EV_PICKUP, EV_NOT_VACUUM, EV_TAUNT, EV_HILL_MOVE, EV_RESPAWN,
       * EV_MINI_KO, EV_CLOSING: the 22c05b6 cases pasted verbatim (with `a` and `b` now the loop's slots), EV_TAUNT
       * adding AUDIO(audio_crowd_react(CROWD_JEER)) */
      default: break;
    }
    game_on_event(e);
  }
  M.nev = 0;
}
```

### 5.3 Other integration patches (the integrator applies these from the reports)

| File, place | Change | From |
|---|---|---|
| `main.c wc_render`, after `read_pads` | `keys_poll()` once a second (controls 4.1); `read_pads` substitutes the d-pad whenever any d-pad bit is down, not only inside the dead zone | controls |
| `main.c`, the fight frame after `drain_events` | `audio_fight_update(&M)` while `G.sim_on && (G.phase == PH_FIGHT || G.phase == PH_KO)`, `audio_fight_update(NULL)` once on leaving them | audio |
| `game_flow.c fight_audio` | delete the `audio_whine` loop (the band owns the loops now); keep the tension and the T minus 10 ticks; add `audio_crowd_react(CROWD_HUSH)` at the countdown's 3 and `CROWD_ROAR` at the horn | audio |
| `game.c save_defaults`, `save_valid` | `coach_done` zeroed; `SAVE_VERSION 2`, a version 1 save is accepted with the field zeroed | controls |
| `game.c game_seat_join`, `save_defaults` | seat 1 of a fresh save starts on PRE_LAUNCH, seat 2 on PRE_ROLL_OUT, 3 CHOP, 4 STOMP | controls |
| `game.c game_on_event` | KO cause counts already cover KO_FLIP from a wedge; EV_WHEEL_HIT and EV_PANEL_WEAR add nothing to the hall of fame | sim-physics |
| `tools/shot.mjs` | a script's pad entry `"name": "Keyboard"` is passed through to the host pad name | controls |
| `web/index.html` | `res[0].name = 'Keyboard'` when `keyBits` is set and no gamepad holds slot 0 (controls owns the file; listed here because the rig depends on it) | controls |
| `HANDOFF.md` | the new fixtures, scenarios, `[world] bots=` and `[draw]` lines, the deviations table | integrator |

## 6. Implementer A: sim-physics

Spec: `deep-physics.md` sections 3, 5, 6. Numbers below are the ones to ship; `-D` overrides on every knob marked (k). Keep `float`, `-ffp-contract=off`, the trig table, fixed pair order, no allocation, no new `rnd01` or `rnd_n` call anywhere in the contact, wedge, spinner, swing, flip or landing paths.

Order of work (each step: `./build.sh balance`, `--hashcheck`, `--scenario all` green so far, then commit):

1. Ghost causes 2 and 3 (section 3). Debris colour: `shed_panel` passes 0 (body paint) for DB_PANEL and 2 (rubber) for DB_WHEEL instead of `b->seat`. Delete the dead `dmg > 20` dent rule.
2. The scenario harness: `build/balance --scenario <name>|all` with the table of 6.8 (`scenario_t { name, setup, drive, assert }`, the probe pattern: place bots, drive `in.ly` and `in.buttons`, read `M.ev`), PASS or FAIL per row with its numbers, non zero exit on any FAIL. Write the rows as they are implemented; the first commit has `ghost-sim`, `mound-contact`, `corpse-hittable`.
3. Bodies (3.1): `clr` per drive TWO 0.12, FOUR 0.10, SIX 0.10, TREADS 0.12, HOVER 0.22 drawn and 0.06 effective, WALKER 0.30, LEG TRACKS 0.10, MELTY 0.10; `hgt` = chassis H; pitch inertia; lips WEDGE chassis 0.02, WEDGE plate 0.04, LIFTER forks down 0.05, LOWBOY nose 0.08, OPS forks 0.05, SHELL none; `theta_tip` BOX 55, WEDGE 58, LOWBOY 66, SHELL 50, FRAME 55, LUNCHBOX 35 degrees (TREADS and LEG TRACKS +6, WALKER and TWO WHEEL -5). Tables in parts.h: `DRIVE_CLEAR`, `LAT_GRIP`, `CHASSIS_TIP_DEG`, `CHASSIS_TUMBLE`, `CHASSIS_VTIP`, `ARMOUR_PANEL_HP`.
4. Drive as grip limited force and the slide (3.3): `F_grip = mu m g load`, g 20, drag 0.6 coasting and 0.15 driving, `LAT_GRIP` TWO 0.8, FOUR 1.0, SIX 1.2, TREADS 1.6, HOVER 0.08, WALKER 1.0, LEG TRACKS 1.4, MELTY 0.5 (k); `slide_t` and EV_SLIDE over 2.0 u/s once per 30 frames; `build_push` times `load`; yaw as today times `load`. HOVER keeps its thrust controller and skirt.
5. Contacts (3.4): the grounded pair rule, slop 0.01, correction 0.8, full angular term (no 0.5), e 0.2 bots, 0.3 walls, 0.1 underride, 0 HOVER, friction 0.4 (0.15 underride); EV_SCRAPE along a wall over 1.5 u/s for 6 frames once per 30. Pitch and roll integrate; supported bodies relax to the terrain slope at 10 rad/s; free bodies take the TIP_G 9 rad/s^2 torque toward or past `theta_tip`.
6. The wedge (3.5): classification in step 8 (PLAIN, UNDERRIDE, WEDGE_CLASH, RAM), approach within 35 degrees, closing over 0.5, sides BLOCKED for wheeled and tracked drives, clr 0.5x on a SHELL rim, HOVER 0.06 at every edge; slopes WEDGE chassis 26 degrees over 0.60, plate 20 over 0.45, forks 15 over 0.35, nose 12 over 0.30; `ride_up = ride_s / S`, lift `ride_up S tan(slope)`, victim pitch servoed at 12 rad/s, `load = 1 - 0.75 ride_up`, attacker load `1 + 0.2 ride_up`; zone 1.5 on the victim while `ride_up > 0.3`, 2.0 for the attacker's 2ND weapon; the SLIP rule at 50 degrees off the normal; carry 90 frames then slide off at 1.5 u/s lateral; `v_tip` (k) LUNCHBOX 2.2, FRAME 3.0, BOX 3.4, WEDGE 3.8, LOWBOY 4.6, SHELL never (rolls off at ride_up 0.5), HOVER never (carried, load 0), +0.6 TREADS or LEG TRACKS, -0.4 TWO WHEEL, -0.3 WALKER; TIP: `vy = 1.2 + 0.3 (cl_peak - v_tip)`, `wp 3.5` past the slope, EV_RIDE_END(TIPPED), EV_LAUNCH(LAUNCH_WEDGE). Ram damage once on entry as today (9 + 9 clamp((cl - 2) / 4), stun 10, RAM_CD), the hop deleted. WEDGE_CLASH: both stop (e 0.3), EV_WEDGE_CLASH, load 0.6 for 20 frames. DIG IN: A or RT on a WEDGE or NONE build: 20 frames at accel 1.6x, lip minus 0.01, cooldown 90, `dig_t`, `dig_cd`, EV_WEAPON_TELL(TELL_DIG_IN). An inverted bot's lips swap sides (it cannot underride, it cannot be underridden).
7. Flipper, tumble, landing (3.8): `k` from the contact point along the plate (0 tip, 1 hinge), `vy = 7 (LOWBOY 0.8) (0.85 + 0.15 k)`, `wp = TUMBLE[ch] (0.7 + 0.6 k) + 0.3 max(dot(vv, -fa), 0)` with TUMBLE BOX 4.5, WEDGE 4.2, LOWBOY 2.7, SHELL 4.5, FRAME 5.0, LUNCHBOX 6.3 rad/s (`flip_odds` keeps the column, the dice roll goes); in the air `pitch += wp DT`, no gravity torque; landing by pitch: under 60 degrees wheels (`dmg 2 + 3 impact/7`, 8 off the deck or mound or a peak over 1.2), over 120 inverted (EV_INVERT by cause), 60 to 120 BOUNCE (`vy 1.6`, wp 0.6x, EV_LAND(LAND_BOUNCE)); SHELL band 150 and the roll upright (EV_SELFRIGHT zone 1); HOVER and MELTY never invert; landing on a bot 8 to it, the lander pitch minus 30 and never inverts. `flips_landed`, `launched_by`, `inverted_by` credits as today. EV_FALL where `falling` is set.
8. Spinner energy (3.6): `e = spin^2`, `bite = clamp(0.35 + 0.08 (tip + rel), 0.35, 0.80)` with the signed relative speed, `E_hit = e bite`, `dmg_raw = base (E_hit / 0.5) zone loc`, `spin = sqrt(e - E_hit)`, EV_SPIN_CHANGE(SPIN_LOST_HIT) when the drop is over 0.15 and SPIN_ARMED on crossing 0.5 upward; knock `KNOCK[w] (0.4 + 0.6 dmg / base)` HORIZ 6, DRUM 4, MELTY 4, SAW 0.4 with the victim's yaw impulse and the attacker's `-0.7 J` and yaw kick; wedge front loc 0.4, bite 0.25, the attacker hops `vy 1.2`, EVF_DEFLECT; SHELL loc 0.8 bite 0.25; WEAR PLATE stun as today plus 0.1 energy; clash each loses `min(e_a, e_b)`, damage `0.4 base_other e_other`; walls as today with EV_SPIN_CHANGE(SPIN_LOST_WALL or CLASH). `SPIN_BLEED_*` go.
9. Swings (3.7): HAMMER `active 10`, head arc 110 to minus 10 degrees, pivot `(0, clr + hgt + 0.1, -0.1)`, reach 1.0, hitbox r 0.25, damage `20 (0.5 + 0.5 p) zone loc`, first overlap ends the swing, handle inside 0.35 is `0.3 base`, TOP panel 1.25x unless WEAR PLATE, victim `wp -= 2.0`, the LUNCHBOX 0.7 rule deleted (emergent at p 0.3). SPIKE `active 9`, extend frames 1 to 5 (z 0.55 to 1.05), retract 6 to 9 at 0.3x, EV_WEAPON_TELL(TELL_SPIKE_OUT, TELL_SPIKE_BACK), a wedge front skids (loc 0.4).
10. The rest (3.9): CRUSHER pitch minus 15 held, TOP wear every 30 frames, EV_GRIND(GRIND_TICK), TELL_CLAMP_START, BITE, RELEASE; SAW `grind_t, grind_x, grind_z`, 1 pool point per 6 frames, EV_GRIND START and END, 0.02 energy per tick (0.04 on STEEL and WEAR); FLAME TELL_FLAME_ON and OUT, EV_BURN_START and END, TOP wear 1 per 30 frames; LIFTER forks as a lip, a ride converts to the hold, pitch minus 20 held, EV_LIFT and EV_LIFT_DROP (OPS zone 1); wheel stations and `wheel_hit_side`, EV_WHEEL_HIT with FOUR and SIX 0.8x 180, TREADS `tread_hit` 0.85x and grip 0.7x 180, WALKER load 0.7 for 120; MELTY as the disc (base 12, TIP 3.0), BLOCKED while spinning; FLIPPER TELL_FLIPPER_RESET when the cooldown ends; SAW TELL_SAW_ON and OFF.
11. Panel pools (3.10): `ARMOUR_PANEL_HP` NONE 0, PLY 20, ALU 28, UHMW 24, STEEL 36, TI 32, WEAR 44; every hit names its panel (edge, TOP for hammer, pulverizer, drop, landing on a bot, crusher hold; FRONT for a wedge glance); the same post armour damage wears the pool; states 0 over 70 percent, 1 at 70, 2 at 45, 3 at 20, 4 at 0 (shed with EV_SHED carrying nx nz and the old state); a gone panel reads reduction 0 and loc 1.25 with EVF_STRIPPED; the global floor 50 HP at least 1, 30 at least 2; `shed_next` becomes the most worn panel; FOUR WHEEL loses a wheel at 30 HP as EV_SHED zone 8 + index with a DB_WHEEL debris; TREADS at 20 HP with a side at state 4 throws a track: new `DB_TREAD` debris kind, EV_SHED zone 8 + side. EV_PANEL_WEAR on every state advance.
12. HUD fields (controls 6.1): `ram_closing` every step for WEDGE and NONE builds (0 when the nose is over 30 degrees off the nearest enemy), `hit_flash_t 20`, `spent_t 20`, `weapon_ready`, `can_self_right` (SR_B builds; DRUM only over 0.5 spin), `self_right_key` 0 none 1 B 2 A, `last_hit_reason`; `reason` on every EV_HIT, EV_RAM and EV_SPIN_WALL by the priority of 2.5; `weapon`, `speed`, `j`, `panel`, `flags`, `y` on every event per 2.5.
13. AI (steering only, `ai.c`): a wedge attacks the front or back of a wheeled bot, any side of a HOVER or a WALKER front, never a wheeled side; DIG IN on the approach inside 2 u; a ridden self is RECOVER (reverse hard, turn); a HOVER enemy in THE BAY is shoved toward the nearer pulverizer (HANDOFF risk 1). No new difficulty rules.
14. Regenerate the fight fixtures with `--make-fight` (`2p-fight.json`, `4p-fight.json`, `feel.json` frames re-read), record the three new fixtures of 6.9, run the full gate of section 11 and write the report: the robin table, every scenario's numbers, the fixture frames, the main.c and game.c patches.

Expected shape (3.12): hit sizes unchanged, fewer spinner hits per approach (bite 35 to 80 percent instead of a flat 70), rams that mean something, flips by wedges (KO BY FLIP appears in the robin), fights 35 to 60 s. If the mean drifts under 35 s, raise `RAM_CD` to 120 before touching base damage.

### 6.8 Acceptance: `build/balance --scenario all` (every row from `deep-physics.md` 6.1, these numbers)

| Scenario | Must hold |
|---|---|
| ghost-sim | SOLO setup: `nbots 5`, slots 2 and 3 `hp_max 0`, never in any event `a` or `b` over 3000 frames |
| mound-contact | THE FREE PILE: contact at centre distance 1.05 +- 0.03, never an overlap under 1.0 |
| corpse-hittable | CHOP at spin 1 into a KO BY FLIP corpse: EV_HIT EVF_CORPSE dmg 0 every 30 frames, spin cost half the usual, the corpse moves, no tally changes |
| wedge-front-lunchbox | ROLL OUT at full throttle into a standing STOMP front: EV_UNDERRIDE (PN_FRONT) within 90 frames, `ride_up` 1.0, `ride_cl_peak >= 2.2`, EV_RIDE_END(TIPPED), EV_LAUNCH(LAUNCH_WEDGE), EV_INVERT(INV_WEDGE) within 240 frames |
| wedge-side-lunchbox | STOMP side on: no EV_UNDERRIDE, one EV_RAM, the STOMP never leaves the floor, pushed over 1.5 u in 120 frames |
| wedge-front-box-slow | ROLL OUT at 0.6 throttle into a LAUNCH front: UNDERRIDE, `ride_up` 1.0, no TIPPED, carried over 2 u, the victim's own full reverse moves it under 0.3 u in 60 frames, RIDE_END(SLIP or BACKED_OFF) when the wedge stops |
| wedge-vs-wedge | two ROLL OUTs head on: EV_WEDGE_CLASH, no UNDERRIDE, both under 0.5 u/s within 10 frames; one at 0.4 throttle: the faster underrides |
| wedge-under-hover | ROLL OUT into a TOAST side: UNDERRIDE, load 0 while ridden, carried over 3 u, never EV_INVERT |
| wedge-lowboy | ROLL OUT into a CHOP front at 3.8: UNDERRIDE, never TIPPED, carried and slides off; a TWO WHEEL wedge build boosted to 9 u/s: TIPPED |
| spinner-wedge-glance | CHOP at spin 1 into a ROLL OUT front: first EV_HIT EVF_DEFLECT, dmg under 9, the CHOP airborne 8 to 12 frames, spin after over 0.8 |
| spinner-side-full | CHOP into a ROLL OUT side: dmg 16 to 18, spin after under 0.6, the victim's lateral speed over 2.0; with a LAUNCH victim: EV_SLIDE |
| spinner-moving-target | inbound target at 6 u/s takes at least 1.4x the outbound one at 3 u/s; both hits fire |
| spinner-wheel | CHOP into a LAUNCH wheel station (z 0): EVF_WHEEL, EV_WHEEL_HIT, `wheel_hit 240`, top 0.6x over 60 frames |
| hammer-arc | STOMP at 1.2 u: on a LUNCHBOX p 0.2 to 0.4 (11 to 14 raw), on a LOWBOY p 0.8 to 1.0 (18 to 20 raw), panel TOP and EVF_TOP, EV_PANEL_WEAR on the CHOP's TOP after two hits; from 0.3 u under 8 raw |
| spike-extend | the hit in active frames 1 to 5 at full base; a retraction hit 0.3x |
| flip-inverts-tall | LAUNCH on a STOMP, 20 seeds: 20 launches, 20 inversions, airtime 38 to 44 frames, landing dmg 2 to 5, EV_LAND(LAND_INVERTED) |
| flip-lowboy | a standing CHOP lands upright (LAND_WHEELS or BOUNCE); a CHOP driving in at 5 u/s inverts |
| flip-bounce | a BOX at k 0.5: first landing 60 to 120 degrees, EV_LAND(LAND_BOUNCE), the second landing decides, under 90 frames in all |
| crusher-hold | 15 on the close, 6 per second for 120 frames, EV_GRIND(GRIND_TICK), TOP pool wears 4, the victim's drive is 0 |
| saw-grind | 120 frames on a LAUNCH side: 12 ticks of 3.2 after ALU, the side pool loses 20 (state 2), GRIND_START and END, `grind_t` continuous |
| flame-burn | 4 ticks of 2, one EV_BURN_START, 3 over 180 frames, EV_BURN_END, no relight while burning; TOAST on TOAST: no burn |
| panel-location | two hammers left, two HORIZONTALs right on a CHOP: LEFT state 3, RIGHT state 2, FRONT and BACK 0 until HP 50; the third left hit sheds LEFT; the fourth has EVF_STRIPPED and 1.25x the third |
| slide-drives | a 6 u/s lateral impulse: TREADS slide under 0.8 u, FOUR WHEEL 1.0 to 1.6, TWO WHEEL 1.6 to 2.4, HOVER over 6 |
| push-treads | ROLL OUT vs LAUNCH head on: the pair moves over 6 u toward the LAUNCH in 240 frames; with the LAUNCH ridden over 8 u |
| dig-in | a ROLL OUT presses A touching a LAUNCH front at rest: TELL_DIG_IN, then EV_UNDERRIDE within 40 frames |

### 6.9 New rig fixtures (recorded with `--make-fight`, frames in the report)

`wedge-flip.json` (ROLL OUT vs STOMP AI in THE BAY: the underride frame, the tip frame plus 20, the landing), `spinner-glance.json` (CHOP vs ROLL OUT: the deflect hop frame), `panels.json` (a fight that strips a side). Each is shot and looked at by the graphics implementer after the merge (7.7).

## 7. Implementer B: graphics

Spec: `deep-graphics.md` sections 2 to 6, 8. Budget: under 15k triangles and 60 calls per view in a 2 player fight, 1,200 per bot (1,500 for the heaviest legal build), under 40 objects per bot, under 300 in the scene. Zero new shader programs beyond the states listed in 7.6.

1. The four bug fixes of graphics 1.6: ghost cause 1 (section 3) with the `[world] bots=` log; `world_scorch` finally called (the drain of 5.2 does it); `debris_colour` keeps its table (the sim passes 0 and 2 now); the garage sun to `(10, 14, 16)`, hemisphere 1.3, the amber point 2.5 u in front of the turntables at intensity 5; arena hemisphere ground 0x2a241e. Half a day, ship first.
2. The crowd as flat silhouettes: 10 triangles per figure (240 x 10 = 2,400 per view instead of 11,520), the 1 Hz back row bob, and the permanent `[draw] f= v= calls= tris=` line under `RTR_TRACE` through `world_draw_stats`. Prove the budget before the bots grow.
3. The geometry kit (3.1): `chamfer_box`, `plate`, `tyre`, `tooth_ring`, `hex_bolts`, `tread_loop`, `skirt`, `leg`, `lathe_rim`, cached in a tuple keyed `geo_slot_t` table so a rebuild never allocates twice; `soup_quad` winding as the wedge comment says.
4. Chassis, armour and paint (3.2, 3.5, 3.6): char steel bodies with the paint on bevelled plates, lid line, bevelled seat band with pin stripes on bone and rust seats, 8 bolts per side, a belly pan (two motor cans, a rust battery with an amber stripe) seen when inverted; BOX latches and bumpers, WEDGE lip bar and skids, LOWBOY vents and rack ears, SHELL rolled rim and rivets, FRAME round tubes and braces, LUNCHBOX lid plus base; the seven armour looks (ALU brushed and brightest, WEAR 12 bolts and amber corners, UHMW no bolts, TI heat stripe, PLY bone edge, STEEL weld bead, NONE raw); the four two tone schemes with the seat number plate. The garage sweep is the acceptance.
5. Drives (3.3): tyres with hubs and spokes and motor cans, axles, the SIX WHEEL rock, TREADS as a `tread_loop` with sprockets and road wheels (scroll the belt canvas, turn the sprockets), HOVER pleated skirt with a bone rim and two spinning fan intakes plus the lean and the pinch, WALKER knees, LEG TRACKS as knees over pods, MELTY bright edge bars with chevrons.
6. Weapons with READY, TELL, STRIKE, RECOVER (3.4): the ready glint the frame `cooldown` reaches 0, the WEDGE plate at 28 degrees no taller than 0.5 u with two skids and a lip bar (hitbox reach unchanged), SPIKE r 0.08 with the collar, LIFTER ram and bevelled forks, SAW tooth ring and idle at 2 rad/s, FLAME tank and pilot and the fire cube emitter, HAMMER face plate, pivot pin, counterweight, stretch frame and the 2 frame contact hold, FLIPPER gap line, knuckles, piston, the render hold and bounce, DRUM end plates and bearing blocks, HORIZONTAL four fat teeth and the amber ring, CRUSHER jaws with cones and a ram; brackets with bolts under every mount. Pose the hammer head from `active` with the 110 to minus 10 arc so the head is where the hit is; pose the spike rod from `active` as the sim extends it.
7. Damage that shows (4.1 to 4.7, 4.10): `plate(w, h, t, state, seed)` with the four states (crease, bent corner at 5 degrees, hanging at 14 degrees with the 3 Hz flap while moving, gone), depth by armour (ALU 1.4, STEEL 1.0, TI 0.8, UHMW 0.5 scrapes, WEAR 0.3 and never state 3, PLY 1.0 with a crack); the hidden internals shown at state 3 and 4 (loom, motor cans, battery, ESC, the dark lid hole); the 128 x 128 decal atlas (scorch, scratches, bolt hole, crack) and `decals[8]` per bot through `world_bot_hit` (the drain calls it on every heavy and huge hit with the contact and the panel); `fx_hit` reads `e->weapon`, `e->panel`, `e->flags` and `e->y` and sprays by the `SPRAY` table of graphics 4.4 (WEDGE low fan forward, SPIKE tight jet, SAW rooster tail, HAMMER flat radial fan, DRUM up and over, HORIZONTAL long tangent streak, MELTY tangent) with the 2 frame impact star replacing the disc as the visible flash; EVF_DEFLECT sprays along the slope; EVF_WHEEL adds rubber chips; EVF_STRIPPED bone sparks; `fx_bot_damage_frac` by fraction (0.40 grey, 0.20 char and fire) from the most damaged panel; the wheel wobble from `wheel_hit_side`, the FOUR WHEEL stub, the TREADS thrown track (`DB_TREAD`), the WALKER limp, the HOVER sag; the KO burst with three debris meshes (plates, wheels, chips), the smoke column, the ember, the floor scorch; the heavy band's 1 frame white at alpha 16 on the victim's viewport.
8. The new motion (physics 2.6): `place_root` applies `pitch` and `roll` about the body's local axes before the inverted flip, interpolated with `pp, pr`; a ridden bot shows its nose up on the plate; a flying bot tumbles; `sync_panels` reads `panel_hp` only for the hanging flap rate. `camera.c`: the chase camera adds `0.6 pitch` to its lift while `ride_by` or airborne so the wedge never clips the lens, and never reads `pitch` otherwise.
9. Spinner blur discs replacing the halo (4.8): real teeth under 0.3, the blur ring 0.3 to 0.7 (alpha 0.3 spin), the ghost teeth canvas over 0.7; the two rings become 0.05 wide floor danger rings at `spin x 90/255` and `30/255`; the blur ring thins with `sim_spin_energy`; DRUM blur cylinder; MELTY bar ring. Launch trail (4.9): `fx_launch` with PK_TRAIL cubes and the streak quad, the blob drifting with the velocity while airborne.
10. Arena (5.1 to 5.5): oriented blob shadow quads with a light direction plus the AO ring, debris blobs, prop shadow pools in `paint_floor`, the lexan reflection plane, bright top edges and smudges, hazard floor tells (killsaw amber quad, flame pilots, pulverizer growing blob, pit tape pulse, floor flipper lift, laser beacons, drop zone ring, bumper tape), tyre marks, the skirting board, THE ROOF rim light.
11. States: append `basic+map+trans+instbig` and `standard+map+trans+l1201` (and `standard+vcol+l1201` only if used) to `tools/states.txt`, `REGEN_STATES=1 ./build.sh`, commit the regenerated `src/gen/rtr_gl.c`. Boot frame 1 logs no missing state.

Stubs: with the sim branch absent `pitch`, `roll`, `ride_up`, `grind_t`, `dig_t`, `panel_hp` are 0 and `e->weapon` is 0 on hits; every path above must then draw exactly as the current fixtures show (dents by `panel_state`, sparks by the weapon derived from the attacker's build).

Acceptance (graphics section 8, every shot looked at): `garage-parts.json` 172 to 2162 (bevels and bolts, the WEDGE lip and skids, LOWBOY vents, SHELL rim, FRAME tubes, LUNCHBOX hinge; char paint front faces over 25 percent of the lid's luminance; lugs, sprockets, pleated skirt, knees; the WEDGE plate under 0.5 u; six different armour plates; four schemes; the test fire TELL and STRIKE poses at +15 and +19); `2p-fight.json` 680 (an empty bed, under 60 calls and 15k per view in the `[draw]` line); `feel.json` 864, 866, 870 (impact star, the fan low and forward along the plate, floor bounce, the 28 px marker, a scratch decal); a heavy HORIZONTAL hit from the trace log (tangent streak over 1 u, a creased plate, the blur ring); `2p-fight.json` 2600 (two plates gone, wires and battery, hanging plates, smoke from the hole, decals, a grate scorch); 3243 to 3290 (plates and wheels in their colours, the column, the scorch, crowd frame 2, the star at 1.4); `keyboard-solo.json` 1800 (the belly pan); `4p-fight.json` 700 (four views under budget, oriented blobs, the reflection plane) and 1800 (smoke from the damaged side); `drive-laser.json` in the laser tell (beacons); `wedge-flip.json` after the merge (nose up on the wedge, the tumble visible in three frames); `panels.json` (the stripped side reads as missing). Every PR logs the `[draw]` line of the first fight frame of 2p-fight, 4p-fight and keyboard-solo and the scene object count.

## 8. Implementer C: controls-hud

Spec: `deep-controls.md` sections 4 to 7. Strings in `content.h` (including `WEAPON_HOWTO[WP_COUNT]` and `MELTY_HOWTO` from controls 4.9, under 44 characters each), nothing inline; `./build.sh lint` clean.

1. `src/keys.h`, `src/keys.c` (controls 4.1): `keys_poll` (the integrator calls it), `keys_kind` (KEYBOARD when `wc_pad_name` starts with KEYBOARD, PAD on any other name, UNKNOWN on none), `key_name`, `key_cap`, `key_cap_seat`; names KIND_PAD `A B X Y L R START SELECT D PAD STICK RT LT`, KIND_KEYBOARD `Z X A S Q W ENTER SHIFT ARROWS ARROWS Z Q`, KIND_UNKNOWN the two caps keyboard first (`Z OR A`). Every keycap and key hint in draw.c and draw_menus.c goes through it (the list in controls 4.1: title, lobby card, garage footer with `W TEST FIRE` on the weapon rows, rules flash, pause, judges, results, the hold chips); the content strings that embed a letter become `%s` formats or get a drawn cap. `web/index.html`: `name: 'Keyboard'` on pad 0 when `keyBits` is set and no gamepad holds slot 0.
2. The weapon readiness widget (4.2) replacing `gauge`: 96 x 56 at (470, 8) in halves and full, 72 x 40 at (400, 8) in quadrants, speed ticks moved right by 52 and 36; a 40 px ring (36) with the glyph, a 24 px keycap, a state word; per weapon rows exactly as the table in controls 4.2 (spinners HOLD, SPIN UP, READY, SPENT with the rust bite tick at `SPINNER_HIT_MIN`; SAW TAP: ON, ON, SPIN UP; FLIPPER HAMMER SPIKE CRUSHER TAP, SWING, CLAMP, HOLD n S, `n.n S`; LIFTER HOLD, LIFT, CARRY n.n S; FLAME HOLD, EMPTY, REFILL on the ring; WEDGE and NONE the closing meter from `ram_closing` over `RAM_MIN_CLOSING`, RAM, DRIVE AT THEM inside 3 u and 30 degrees, HIT for `hit_flash_t`, `n.n S` through `RAM_CD`); colours steel base, seat fill, live at ready, rust cooling or spent, hazard transient; INVERTED in smoke when `inv_operate` is 0. `weapon_ready` and `spent_t` come from the sim; until the merge derive them locally from `spin`, `cooldown`, `lift`, `fuel`, `saw_on` and keep the RAM row at RAM.
3. The attack coach (4.3): `SAVE.coach_done` (game.h, version 2 of the save; the game.c patches in 5.3), `coach_update` in game_flow.c, `coach_draw` in draw.c as a 360 x 64 chip above the strip; only a human seat, PH_FIGHT, first fight of a fresh save (`SAVE.matches == 0` at the horn), first human seat, never KO BY FLIP or 3 and 4 players; beat 1 `DRIVE WITH {ARROWS or STICK}. TURN TOWARD THE ARROW` (the chevron live and 32 px) done on `drive_t > 0.5` for 60 frames and the enemy on screen 30 frames, max 12 s; beat 2 by weapon (`HOLD {Z} TO SPIN UP. KEEP OFF THE WALL`, `TAP {Z}: SAW ON`, `TAP {Z} WHEN THEY ARE CLOSE`, `HOLD {Z} UNDER THEIR SIDE`, `HOLD {Z}. IT IS SHORT`, `DRIVE INTO THEIR SIDE. SPEED IS THE WEAPON`) done on the first use, max 15 s; beat 3 `HIT THEIR SIDE, NOT THE FRONT` with the weak side indicator forced on, done on a landed hit at zone 1.0 or over, max 20 s, a front hit shows `FRONT. TRY THE SIDE` 2 s; each beat ends with a 0.3 s live tick and a low `SFX_MENU_OK`; `coach_done` committed after beat 3. The attract tips: `HOLD {Z} TO SPIN UP. A HIT SPENDS THE SPIN.` and `{X} RIGHTS A FLIPPED HAMMER OR FLIPPER.`
4. Hit markers (4.4 and graphics 7.2), the draw.h additions the drain of 5.2 calls: `hit_marker_all(x, y, z, band, rgb)` (a ring 16 to 28 px over 8 frames, 3 px bone tap and hit, 4 px hazard heavy, 5 px rust huge, in every viewport); `strip_taken(seat, band)` (the HP bar flashes bone 2 frames then drains over `J_BARS_ANIM_FRAMES`, the number pops 1.15x, a 12 px rust frame on that viewport fading over 18 frames); `strip_dealt(seat)` (a 10 px live tick at the bar's end for 20 frames; the widget word HIT reads `hit_flash_t`); `strip_panel_gone(seat, panel)` (the strip's bot outline marks that side in rust); `hit_mark_add(viewport, reason, x, y, z)` (a 2 s `F_MONO_S` word under the number from `HIT_WHY[]`: `FRONT ARMOUR`, `WEDGE`, `SPINNING DOWN`, `SHELL`, `ARMOUR`, `WALL`, and in live `EXPOSED`, `STRIPPED`, `WHEEL`). `damage_number` is unchanged; main.c picks the colour per viewport (5.2).
5. The weak side indicator (4.5): a 20 px live chevron 0.5 u off the enemy's weakest side (lowest `panel_state`, never the front or a wedge front), hazard and `EXPOSED` for 1 s when `lifted_by`, `ride_by`, airborne over 0.15, inverted against a hammer, drum or spike, or a spun down spinner; shown for the whole fight while `SAVE.matches < 6`, 2 s after every reason word, during beat 3, and under Y; humans only, own viewport only, not in ONE CAMERA except under Y.
6. The self right prompt (4.6) replacing the two `countout` lines, the DRUM's three lines, `DRIVES UPSIDE DOWN. KEEP MOVING` for `inv_drive` builds, `UPSIDE DOWN. SPINS, CANNOT STEER` for HORIZONTAL, OPS lines in a 1v1, and a new `WEDGED` status word (SW_WEDGED, after SW_CLAMPED) for `ride_by`, `LIMPING` for `wheel_hit` or `tread_hit`. `sim_status_word` stays as it is (sim.c is not yours): the strip derives the two new words itself before it asks the sim (`b->ride_by ? SW_WEDGED : (b->wheel_hit || b->tread_hit) ? SW_LIMPING : sim_status_word(b)`, with `STATUS_WORD` and the rust colour case extended). Add the `ST_WEDGED` and `ST_LIMPING` stinger pools to content.h (the drain calls them under `HUD()`).
7. Boost and brake (4.7): the `BOOST` word and `{Q}` cap beside the bar, `NO BOOST` in smoke for 3 s on TREADS and LEG TRACKS, the boost prompt once per 20 s at over 6 u, `BRAKE` chip on `brake > 0`, the `{X} BRAKES` prompt near an edge or a pit tell.
8. The controls card (4.8): the keyboard outline variant, `CARD_SUB_PAD`, `CARD_SUB_KEYS`, `CARD_SUB_BOTH`, `CARD_LINE` with `%s`, the `WEAPON:` howto line, `FIRST_CARD_S 3.0f`, the keyboard variant as the last rules card when pad 0 is KIND_KEYBOARD. The garage flavour slot alternates the joke and the howto every 2 s; the lobby seat card shows the howto's first word after the preset.
9. A clearer 16 x 16 hammer glyph in glyphs.h (rows 16 wide, lint). README.md controls section: the keyboard map and the widget words.

Acceptance (controls section 7, the frames of today's fixtures; re-read after the merge): key names (`keyboard-solo.json` 111 with `"name": "Keyboard"` reads `Z LOCK IN`, `ENTER FIGHT`, `A RANDOM BOT`, `S PRESET . HOLD: CONTROLS`, `W TEST FIRE`; without the name `Z OR A LOCK IN`; `2p-fight.json` with pad 1 named a controller reads `A LOCK IN`); widget states on `probe-chop.json` 700, 745, 790, 850, 1100 (HOLD, HOLD, SPIN UP, READY, SPENT then SPIN UP with `WALL` under the rust 15) and `probe-rollout.json` 700 (`ARROWS OR STICK`, RAM) then DRIVE AT THEM, HIT, `n.n S`; `keyboard-solo.json` 1000 to 1112 (TAP, 1.8 S, TAP); `4p-fight.json` seat 4 in a windup (SWING); `feel.json` 863 to 870 (the attacker's half in its seat colour with the tick and HIT, the victim's in rust with the fading frame, a reason word or none); `2p-fight.json` 668 to 3243 at least one `SPINNING DOWN` and one `EXPOSED`; weak side at 900 on a fresh save and none with a `SAVE_IN` of `matches 6` unless Y is held; the coach beats on `keyboard-solo.json` and none on a second run with `SAVE_IN`, none in `4p-fight.json` or `mode-flip.json`; the self right prompt from a `--trace` inverted frame for LAUNCH, a DRUM build and a LIFTER build; boost and brake words; the two controls card variants at `pause.json` 1660 and `menu.json` 525; `menus.json` 100 seat 1 `YEETING BOX LAUNCH`, seat 2 `POLITE DOORSTOP ROLL OUT`; `dpad-turn.json` with pad 0 `lx 7000` still turns on RIGHT; `TIMING=1` on `2p-fight.json` under 2 extra sub image calls a frame and under 3,000 KB/s over the R4 numbers. The two probe scripts are reproduced in controls section 7; add them to `tools/scripts/` as `probe-chop.json` and `probe-rollout.json`.

## 9. Implementer D: audio

Spec: `deep-audio.md` sections 3 to 6. Principles: noise first (transient, body, tail), every weapon has a voice and pressing the weapon button sounds within one frame, velocity from the sim, 15 percent draws from the band's own rngs and no palette notes in the fight, a priority mixer, pan by world x (`clamp(x / (arena.w / 2), -1, 1) x 0.6`), measured in the bake. The sim never calls audio; `audio_fight_update` reads `match_t` in `audio_pcm.c` (C, headless safe includes).

`audio.h` additions (the names the drain of 5.2 uses; `audio_whine` stays as a wrapper for one release; `SFX_HIT_LIGHT`, `SFX_HIT_HEAVY`, `SFX_CLASH` stay and map to `audio_hit(HK_GENERIC, band, 0.6, 0, 0, -1, -1)`):

```c
enum { HK_GENERIC, HK_SPINNER, HK_SAW, HK_HAMMER, HK_SPIKE, HK_RAM, HK_FLIPPER, HK_FLAME, HK_CRUSHER, HK_LIFTER, HK_OPS,
       HK_HAZ_PULVERIZER, HK_HAZ_KILLSAW, HK_HAZ_FLAME, HK_HAZ_SCREW, HK_HAZ_LASER, HK_HAZ_DROP, HK_HAZ_PIT, HK_WALL, HK_LAND, HK_COUNT_ };
enum { WK_NONE, WK_SPIN_HORIZ, WK_SPIN_DRUM, WK_MELTY, WK_SAW, WK_FLAME, WK_HAMMER_ARM, WK_CRUSHER_SERVO, WK_LIFTER_SERVO, WK_SCRAPE, WK_COUNT_ };
enum { DK_NONE, DK_WHEEL_TWO, DK_WHEEL_FOUR, DK_WHEEL_SIX, DK_TREAD, DK_HOVER, DK_WALKER, DK_LEGTRACK, DK_MELTY, DK_COUNT_ };
enum { SFX_TELL_HAMMER = SFX_COUNT_, SFX_TELL_FLIPPER, SFX_TELL_SPIKE, SFX_TELL_CRUSHER, SFX_BITE_CRUSHER, SFX_RELEASE_CRUSHER, SFX_LIFT, SFX_LIFT_DROP,
       SFX_FLAME_IGNITE, SFX_FLAME_OUT, SFX_SPIN_KILL, SFX_SPIN_READY, SFX_SHED, SFX_BURST, SFX_SELFRIGHT, SFX_BOOST, SFX_STUCK, SFX_FALL, SFX_LAUNCH_WHOOSH,
       SFX_LAUNCH_WEDGE, SFX_BENCH, SFX_BUMPER, SFX_UNDERRIDE, SFX_RIDE_DROP, SFX_WHEEL_HIT, SFX_DENT, SFX_SCRAPE, SFX_SKID, SFX_GRIND_BITE, SFX_BURN_START,
       SFX_BURN_END, SFX_DIG_IN, SFX_SAW_TOGGLE, SFX_COUNT2_ };
enum { CROWD_OOOH, CROWD_ROAR, CROWD_KO, CROWD_HUSH, CROWD_JEER };
void audio_hit(int kind, int band, float vel, float x, float z, int armour, int attacker_slot);   /* an impact; the attacker's loop takes the load dip */
void audio_play_at(int sfx, int var, float x, float z);                                           /* audio_play_var with a pan; var is the event's zone where the plan says so */
void audio_tell(int slot, int weapon, int tell_zone, float x, float z);                          /* EV_WEAPON_TELL: TELL_* by weapon */
void audio_spin_change(int slot, int zone, float spin_before, float spin_lost);                   /* EV_SPIN_CHANGE except SPIN_ARMED */
void audio_haz_tell(int haz_index, float x, float z);                                             /* the per type tell add beside the klaxon */
void audio_crowd_react(int kind);
void audio_duck_band(int band);                                                                   /* TAP 1 dB, HIT 2.5, HEAVY 5, HUGE and CLASH 8, KO 12; attack 15 ms, hold 60, release 180 (KO 400, 900) */
void audio_weapon(int slot, int kind, float level, float pitch, float load);                      /* the per slot loops; 0..3 seats, 4 OPS */
void audio_drive(int slot, int kind, float speed, float turn, int grounded, int inverted);
void audio_damage(int slot, float hp_frac, int burning, int alive);
void audio_hazard(int slot, int type, float level, float x, float aux);
void audio_fight_update(const void *match);   /* once per render frame in a fight: a match_t (audio.h stays free of sim.h; audio_pcm.c includes sim.h and casts); NULL silences every loop */
```

`audio_fight_update` drives `audio_weapon`, `audio_drive`, `audio_damage` and `audio_hazard` per slot from `build`, `spin`, `saw_on`, `fuel` (falling means lit), `clamp`, `lift`, `carry_t`, `riding`, `ride_up`, `dig_t`, `grind_t`, `burn_t`, `hp / hp_max`, `b.vx vz w`, `airborne`, `lifted_by`, `inverted` and `M.haz[]` (the two active hazards nearest any seat bot inside 6 u, level `1 - d / 6`).

1. Voices and pools (3.2): `Burst`, `Crackle`, `Loop`, `Thud`, `Ring` as plain arrays with the steal rule (KO 5, CLASH and WRECK 4, CRUNCH and stingers 3, CLANK, tells and hazards 2, debris and fizz 1; loops never stolen); the impact bus through `punch_`; the slap delay only on stingers and crowd; pan in `Mix`.
2. The three impact recipes with the velocity law (3.3): CLANK (tap and hit), CRUNCH (heavy), WRECK (huge and clash); level 0.55 + 0.45 vel, transient 4 + 8 vel ms at 6 kHz minus 3 kHz vel, body 40 + 100 vel ms, ring vel 0.3 + 0.6 vel; HK colouring (spinner second transient at 8 ms and the load dip, saw bite, hammer always a thud, spike stab, ram body dominant, flipper quiet, flame fizz only, crusher bite and creak, lifter whir, OPS bell at 82 Hz); armour colouring (PLY wood thock, ALU +1 octave, UHMW no ring, STEEL 0.45, TI 0.6 and brighter, WEAR two rings 7 cents apart, NONE rattle).
3. Weapon loops and tells (3.4): spinner whine 90 to 820 Hz HORIZ, 120 to 680 DRUM, 70 to 520 MELTY with the 6.5 Hz wobble and the 120 ms load dip, level 0.15 + 0.55 spin at -18 dBFS RMS full, the detent clicks under 0.35, the spin kill fall (EV_SPIN_CHANGE); saw spin and grind loop (41 Hz teeth) while `grind_t > 0`; flame roar while `fuel` falls, ignite whump and fuel out pops, the BURNING crackle on `burn_t`; hammer wind up hiss and arm sweep, miss thud; flipper fire burst, whoosh on EV_LAUNCH, landing CRUNCH by `speed / 7` with the inverted settle; spike, crusher, lifter tells; the wedge scrape loop while `riding` (sweep rate by `ride_s`) or `dig_t`, HK_RAM CRUNCH on the ram and `SFX_UNDERRIDE` on entry; melty at the body pitch.
4. Drive loops (3.5): TREAD, WHEEL (TWO a fifth up, SIX a fifth down), HOVER blower with the skirt pinch, WALKER steps, MELTY wheel whine at 0.05; boost hiss and pitch up, stuck squeal, airborne squeal (`airborne || lifted_by || ride_up > 0.5`), inverted grind, wall CLANK by `speed / 6`.
5. Damage states, shed, burst, KO, fall, count tick, self right (3.6): fizz under 0.40, rattle under 0.20, the shed's three bounces, the burst rain, the KO blast (thud 40 Hz, brown boom, bell 82 Hz, debris, the gong 250 ms later, the crowd roar, the 12 dB duck), EV_FALL whoosh and the lowpassed far KO, the count tick at an audible level, the self right whir.
6. Hazards (3.7) and crowd (3.8): per type tells, loops for the two nearest active hazards, per type hits; the crowd bed at -34 dBFS RMS, three formant voices for OOOH, ROAR, KO with the stomp on the beat grid, HUSH, JEER.
7. Stingers (3.9), the garage parts table with noise bursts, the test fire playing the tell and the strike; mix, priority and duck rules (3.10); slow motion pitches every loop down 40 percent and never the stems.
8. The bake (4.5, 6.1, 6.2): `Preview()` renders every id, every HK kind at every band at vel 0.3, 0.7, 1.0, every loop, the fight preview script, prints peak, RMS 300 ms, attack, T20, centroid, band shares and flatness per slot, and FAILs on the targets of audio 6.1 (CLANK tap -14 to -10 dBFS flatness over 0.01 centroid 1000 to 3500 Hz over 20 percent above 2 kHz; CRUNCH -6 to -3 flatness over 0.03; WRECK -3 to -1 under 120 Hz 25 to 50 percent; KO -1.5 to -0.8 T40 over 600 ms; whine full under -10 peak, -20 to -16 RMS, under 10 percent below 120 Hz; grind flatness over 0.15 centroid 2 to 7 kHz; roar flatness over 0.1; tread -24 to -20 RMS; fizz under -24; crowd bed -34; every one shot alone never over -0.8; band cost under 0.7 ms per 800 frames). `build/preview-fight.wav`: mix RMS -18 to -13, true peak under -0.8, music alone -26 to -22 at MUSIC 6, every impact 6 to 10 dB over the bed's median second, the duck 7 to 9 dB after the WRECK, the grind second over 15 percent above 2 kHz, the whine's tracked fundamental rising under 200 to over 600 Hz.
9. The music (5.2): the kit voicing, metal that is metal, the bass fuzz harmonics, open on BREAK or cut STOMP to 4 bars, the Eq3 tilt on every stem (low 100 Hz -2 dB, mid 2.5 kHz +3, high 6 kHz +3), the garage sub down, overtime sirens saturated, `MODE_TRIM` arena 0.56 and overtime 0.6; stems stay 22050 Hz mono under 4.4 MB.

Stubs: with no sim branch the new event fields are 0, so `vel_of` falls back to damage, `grind_t`, `ride_up`, `dig_t` are 0 (no grind or scrape loops), and `EV_SPIN_CHANGE` never arrives: `audio_fight_update` also detects a spin drop over 0.3 in one frame by polling and plays the kill itself, so the spinner sounds right either way. The branch is tested with the bake (no cart needed) and in the cart with `EXTRA_CFLAGS=-DRTR_DEEP_AUDIO`.

Acceptance in the cart (audio 6.3): `WAV=build/out-2p.wav` on `2p-fight.json` then the band statistics from `node tools/wavstat.mjs build/out-2p.wav 1` (new, yours: per second peak, RMS, the energy shares under 120 Hz and over 2 kHz, flatness; no dependencies): no second over 85 percent under 120 Hz after frame 668, at least one second per 5 s over 12 percent above 2 kHz, peak never over -0.8 dBFS; `feel.json` 863: the scrape before the CRUNCH, flatness over 0.03; `ks-medium.json` with `STATION=1`: the practice bot's drive and spinner audible, the KO blast and crowd at 2782; the `[sim]` hashes unchanged against `--replay`; `[perf]` without new `half=1` stretches; `./build.sh lint` clean.

## 10. The feel gate (the integrator, in a real visible Chrome with the keyboard)

`node tools/serve.mjs 8794 &` then `node tools/play.mjs 'http://127.0.0.1:8794/?v=1' build/play` on a fresh save for the scripted path, then a hand played second session in the same Chrome (play.mjs leaves it open; never the extension tab, never a background tab). Look at every PNG and read `log.txt`. The gate passes when all of these are seen:

1. First fight on EASY: seat 1 is YEETING BOX (LAUNCH); the garage footer reads `Z LOCK IN`, `ENTER FIGHT`, `A RANDOM BOT`, `S PRESET . HOLD: CONTROLS`, `W TEST FIRE`; the first join card lasts 3 s and names the keys; the fight runs about a minute with the player over 80 HP at ten seconds; the `[hits]` log shows the player landing at least three hits; pressing Z always makes a sound within a frame (the flipper fire) and the widget reads `TAP` then `1.8 S`.
2. The attack coach: beat 1 at the horn with the live 32 px chevron, ticked once the enemy is in frame; beat 2 `TAP Z WHEN THEY ARE CLOSE` ticked on the first Z; beat 3 `HIT THEIR SIDE, NOT THE FRONT` with the weak side chevron on the practice bot, ticked on a side hit or `FRONT. TRY THE SIDE` after a front one; no chip on the second fight of the session.
3. A wedge underride: pick ROLL OUT (S taps), drive at the practice bot's front; the widget reads `DRIVE AT THEM` inside 3 u, Z scrapes (DIG IN: amber sparks at the lip, the scrape loop, a camera dip), the bot goes under, the victim's nose rises on the plate with `WEDGED` on its strip and the scrape loop playing, and against a LUNCHBOX or BOX at speed it goes over (air time, the tumble, `INVERTED`); the log has `EV_UNDERRIDE` (`[hit]` lines under `RTR_TRACE` or the `[hits]` summary with a ram).
4. A spinner hit: pick CHOP, hold Z (the whine climbs for 2 s, the ring passes the rust tick, `READY` with the pulse), hit the practice bot's side: a tangent spark streak over 1 u, the impact star, a dent (a creased plate on that side, visible from the chase camera), the number in the player's seat colour with the live tick, the whine bogs (`SPENT` then `SPIN UP`), the victim slides. Then a wall: the rust 15 with `WALL`, the kill sound, `SPINNING UP`.
5. A flip: LAUNCH, Z on a close bot: the fire burst, the launch streak and trail, the blob drifting under the flier, the tumble through three frames, a landing thud by height, `AIRBORNE` then `INVERTED` on a tall victim, the belly pan and bolts visible on the inverted bot, the count and the right prompt with the right key name.
6. A KO: plates and wheels scatter in their colours, the smoke column and fire on the stripped frame, the floor scorch, the blast and the crowd roar with the stomp, slow motion with the loops pitched down, the KO card.

Also the pad path once (any controller): the footer reads `A LOCK IN`; everything above with the pad letters.

## 11. Determinism, balance and performance gates

Run after every branch's final commit (the implementer) and after the merge (the integrator):

- `./build.sh`, `./build.sh balance`, `./build.sh music`, `./build.sh lint`, `HUD_DIV=2 ./build.sh`: no warnings, no missing render state in any rig run (boot frame 1 of every fixture).
- `build/balance --hashcheck` PASS; the cart's `[sim] f= h=` lines identical to `build/balance --replay` for `drive-bay`, `drive-dock`, `drive-laser`, `drive-pile`, `drive-roof`, `drive-shop`, `dpad-turn`, and the regenerated `2p-fight`, `4p-fight`, `wedge-flip`, `spinner-glance`, `panels`. The graphics, controls and audio branches must match the hashes of their fork point exactly (they touch no integrated float).
- `grep` the sim diff: no new `rnd01` or `rnd_n` call outside debris scatter, the shed fallback, the DRUM self right roll, OPS lines and the pit cover; `-ffp-contract=off` on both builds; no libm trig.
- `build/balance --scenario all` PASS (every row of 6.8).
- `build/balance --robin 20` (driver ai, every arena, hazards both): mean fight 35 to 60 s; every preset 35 to 65 percent; WEDGE over HORIZONTAL at least 40 percent; HOVER under 45 percent against TREADS in THE BAY (the wedge carries it and the AI shoves it toward a pulverizer; if it still fails, the AI is the next lever, not a table number); KO BY FLIP occurs; no stalls; `[stuck]` nudges 0; the new summary line prints underrides per ROLL OUT match 2 to 6, tips 0.3 to 1, flips by wedge and by flipper, glances, wheel hits. `--modes 4` ends every mode with no stuck frames. `--check-arenas` PASS.
- `[world] bots=` masks per section 3; `solo.json` 900 and `title.json` 700, 1300 looked at.
- `TIMING=1` on `4p-fight.json`: host ms per frame within 0.2 ms of 22c05b6 (the sim adds under 0.1 ms); `[draw]` under 15k triangles and 60 calls per view in `2p-fight.json`; the HUD under 2 extra sub image calls a frame; `band cost` under 0.7 ms per 800 frames.
- The twelve rematch soak: `[mem] used` flat from the second match (the geometry cache and decals must not leak).
- Save: `SAVE_OUT` after `flow.json`, `SAVE_IN` into `menu.json`: the hall of fame shows; a 22c05b6 save (version 1) is accepted with `coach_done 0`.

## 12. Merge order and risks

Order: step 0 (5.1) on main; the four branches fork; sim-physics merges first (its fixtures and the scenario harness), then graphics (re-shoot the acceptance frames on the new fixtures, append states), then controls-hud (regenerate the fixtures once more for the default preset, re-read the frame numbers), then audio (the drain's ifdefs come off, `fight_audio` loses its whine loop, `audio_fight_update` goes in). After each merge: build, `--hashcheck`, `--replay` of the fixtures, the gate of section 11; after the last: section 10, HANDOFF.md, the cart rebuilt and committed by the integrator alone.

Risks and their knobs: the wedge makes ROLL OUT strong (`V_TIP_*`, the 90 frame carry, `RAM_CD`); the flipper on a LOWBOY now needs a moving target (watch the LAUNCH vs CHOP cell, 82 percent LAUNCH in robin 3); the AI must attack the right edge or wedges never underride (6.13 lands before the robin is judged); pitch is a new camera input (7.8 is the only camera change); the corpse path must not credit tallies or reset counts (`corpse-hittable` asserts it); the saw grind and scrape loops are the first audio voices to cut if the Pi `[perf]` line shows `half=1` (then the formants, then the hazard loops); the three ifdef groups in main.c are a merge aid only and must not survive the last merge (`grep RTR_DEEP src/` is empty on the shipped main); the default preset change moves every fixture frame, so every frame number in sections 7 to 9 is re-read once after the controls merge and written into HANDOFF.md.
