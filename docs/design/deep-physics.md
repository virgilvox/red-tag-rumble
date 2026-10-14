# RED TAG RUMBLE: physics, collisions, wedges, weapons and damage

Investigator's spec for the architect and the implementers. Worktree used: `/Users/obsidian/Projects/heatsync/games/_wt/rtr-deep-physics` (detached at 22c05b6, nothing committed). Probe source and output: a scratch `probe.c` and `probe.out` (not kept in the repo; `build/balance --scenario` of DEEP-plan 6.8 replaces them). Screenshots: `/Users/obsidian/Projects/heatsync/games/_wt/rtr-deep-physics/build/shots/`.

Every line pointer below is into the worktree at that commit (identical to main).

---

## 0. Summary

1. The owner's ghost bots are real and reproducible. The main cause is a render bug, not a sim bug: `world_sync` (`src/world.c:1888`) forces every slot `i < m->nbots` visible, `sim_init` sets `nbots = 5` whenever OPS exists (`src/sim.c:1949`), and the boot warm compile builds meshes in slots 0 to 3 (`src/world.c:2024`). So in every 2 or 3 player match, including SOLO and the title attract, the unused slots are drawn at the sim origin as a wheel-less chassis. In THE BAY the origin is the pit. Proof: `build/shots/solo-900.png` (a grey chassis with no wheels on the pit while two bots and OPS are in the match) and `build/shots/title-700.png`, `title-1300.png`. Second cause: a counted out or flipped out corpse keeps its whole model, collides as a 0.5 mass prop, but no weapon can hit it (`src/sim.c:858`): probe S5 pushed one 11.5 u with zero weapon hits. Third, minor: the bot vs bot pair test skips any pair whose centres differ by more than 0.35 in y (`src/sim.c:1138`), which on the mound rim of THE FREE PILE lets a bot on the floor and a bot 1 u up the dome pass into each other before the test re-engages.
2. The wedge does not exist as physics. A "wedge" today is a ram damage number plus a 10 frame hop of 0.044 u (probe S2). Nothing rides up, nothing loses traction, nothing tips. ROLL OUT, the wedge preset, wins 35 percent of its robin matches (the lowest) and LAUNCH 64 percent (the highest).
3. Damage never shows where a hit lands. The per location dent rule needs a hit over 20 after armour (`src/sim.c:455`) but `HIT_CAP` limits any hit to 20 percent of max HP (17 to 22), so the rule fires only on a SHELL. All dents come from global HP thresholds that advance every panel at once (`src/sim.c:405`). Probe S6: two hammer blows into one side of a CHOP left all six panels at state 1.
4. Spinners have no energy: a hit costs a constant (`SPIN_BLEED_*`), the attacker gets the same impulse whatever it hit, and a wedge front is a 0.5 zone with no deflection.
5. Inversion is a dice roll (`flip_odds`), so a flip reads as luck. A flipped ROLL OUT stays inverted for the rest of the fight (probe trace: inverted at frame 618 of 2954).
6. Section 3 designs oriented box bodies with yaw and pitch, impulse contacts with friction, drive as traction limited wheel forces with a lateral grip cap so shoved bots slide, a real underride model with lips, clearances, approach angle and a deterministic tip rule, spinner energy with recoil, hammer and spike arcs, deterministic flipper tumbles, per panel HP pools, and the chassis and drive quirks. Section 5 lists every event the sim emits. Section 6 is the acceptance test plan.

---

## 1. The current model, exactly

### 1.1 Bodies (`src/sim.h:54`, `src/bot.c:10`)

`body_t`: plane position `x, z`, heading `h` (0 is plus z, forward is `(sin h, cos h)`, local plus x is `(cos h, -sin h)` and lies on the bot's left on screen, so a right turn is a heading decrease), plane velocity `vx, vz`, yaw rate `w`, height `y` and `vy`, previous transforms for interpolation, `mass, inv_mass, inertia, inv_inertia`, half extents `hl, hw` (hl is the radius for `SH_CIRCLE`), `shape`.

`bot_body_from_build`: `mass = build_mass` (chassis plus drive plus weapon plus armour, 1.0 to 2.3), inertia `m (hl^2 + hw^2) / 3` for a box, `m r^2 / 2` for a circle. No pitch, no roll, no height extents: the body has no top and no clearance. OPS: `hl *= 1.4`, mass 6, `inv_mass = inv_inertia = 0`.

### 1.2 Drive (`drive_step`, `src/sim.c:552`)

Arcade: stick after a 0.18 radial dead zone gives throttle `t` (reverse 0.8) and turn `s`. Forward speed is driven straight toward `t * top` at `accel` per second (a velocity controller, not a force): `dv = clamp(want - vf, -accel DT, accel DT)` (line 628); coasting `dv = -0.10 vf`. Lateral velocity bleeds by a fraction `(4 + 8 traction) DT` per frame (line 630): TREADS lose 20 percent of lateral speed per frame, TWO WHEEL 14.7 percent. There is no friction cap, so a shoved bot never slides: a 6 u/s lateral knock on TREADS is gone in 10 frames. Yaw: `w` moves toward `-s turn (1 - 0.4 speed/top)` at 20 rad/s^2 (line 636). HOVER and MELTY (strafe): the stick is a world direction against the latched camera yaw; a velocity controller with weak braking (`HOVER_BRAKE 0.3`) and a pinched skirt at 0.25 accel for 24 frames after contact. Slopes (ramps over 0.3, the mound): accel and top 0.6x unless LEG TRACKS or SIX WHEEL. `bot_top_speed` (`src/bot.c:57`): FRAME 1.1x, FOUR WHEEL 0.8x under 30 HP, boost 1.5x, red tag 1.15x, wheel hit 0.6x, inverted 0.7x. `bot_can_drive`: not airborne, lifted, clamped, stunned, braking, locked, self righting; inverted only HOVER, MELTY and weapons with `inv_drive` (NONE, WEDGE, SPIKE, LIFTER... see parts.h: `inv_drive` is 1 for NONE, WEDGE, SPIKE, SAW, DRUM, HORIZ).

### 1.3 Integration (`integrate`, `src/sim.c:977`)

Wind, drag 0.90 per frame on a non driving bot (HOVER 0.985), `x += v DT`, `h += w DT`. Height: `arena_floor_y` (0, the deck 1.0, ramp slopes, the mound dome `0.9 t (2 - t)` over r 4). Airborne: `vy -= 20 DT`, landing when `y <= floor` (the event runs in `checks_step`). Grounded: a rise up to 0.25 is climbed instantly, a drop over 0.12 makes the bot airborne with `vy 0`. The lifted and clamped are skipped; the falling drop to y minus 6; a pitted bot sinks to minus 2.5.

### 1.4 Contact detection and resolution (`src/sim.c:194` to `1213`)

Shapes: OBB from `(x, z, h, hl, hw)`; SHELL and a spinning MELTY (spin over 0.5) are circles. `collide_box_box` is SAT on the four face axes, normal is the least penetration axis from a to b, contact point is the midpoint of the two support points. Circle vs box by closest point. Walls are four planes with inward normals (`wall_pen`); solids (benches, HVAC, the deck faces, the OPS gate, bumper posts) are AABBs gathered per step (`gather_solids`), used only when the solid's top is above `y + 0.1`.

`walls_step` (line 1072): two iterations; `resolve_static` moves the body out by `0.8 pen`, applies a normal impulse `j = (1 + 0.3) vn m` plus a tangential impulse clamped to `0.4 j`, and a yaw impulse at half the cross term. Wall events over `vn 2`: HORIZONTAL at spin over 0.5 loses its spin and 15 HP (`EV_SPIN_WALL`), DRUM halves its spin, MELTY loses 10 HP, everybody loses 2 HP over `vn 4`.

`bots_collide` (line 1128): two iterations over pairs `i < j`; skipped when either has `hp_max 0`, is lifted or clamped, falling, pitted, a KO_PIT or KO_ROOF corpse, or when `|ya - yb| > 0.35`. Dead bots are 0.5 mass props (`ima = 2.0`), OPS is immovable. On iteration 0 `ram_check` runs both ways with the pre resolution closing speed. A HOVER drops its velocity into the contact and takes the whole separation. Positional separation `0.8 pen` split by inverse mass; impulse `jn = -(1 + 0.2) vn / ims` with tangential friction clamped to `0.4 jn`; yaw impulses at half the cross term (only on live bots). Push (iteration 0, both alive, both driving into the contact over 0.2 throttle): `net = (push_a - push_b) / 2` applied as a force along the normal to both, where `build_push = traction^2 mass 8` (`src/parts.h:321`; TREADS ROLL OUT 17.2, TWO WHEEL LAUNCH 4.6; HOVER 0, MELTY half throttle), plus the pin detector (two walls within 1.0, speed under 0.3 for 240 frames).

### 1.5 The wedge today (`ram_check`, `src/sim.c:950`)

Any bot with the WEDGE weapon, the WEDGE chassis or NONE: on a contact where its forward is within 30 degrees of the normal and the closing speed exceeds `RAM_MIN_CLOSING 1.5`, once per victim per `RAM_CD 90` frames: `raw = 9 + 9 clamp((closing - 2) / 4)`, stun 15, `EV_RAM`, knockback `J = 4 (0.5 + dmg / 30)` along the attacker's forward, TWO WHEEL wheel hit from the hit band up, and the hop: if the attacker is a WEDGE chassis or weapon and the contact is on the victim's front or side (not its back third) and the victim is grounded and not a HOVER, `vy = 1.5` (a 0.044 u, 10 frame hop; probe S2). Nothing else. The hop does not disable the victim's drive for more than the 10 frames of `airborne`, the victim's underside is `zone 1.5` for other weapons only while `y - floor > 0.15`, which a 0.044 hop never reaches. A wedge vs a wedge is two rams. There is no lip, no clearance, no ride, no tip.

### 1.6 Lifting, flips, inversion (`weapon_hit`, `holds_step`, `landing`)

LIFTER (`src/sim.c:776`, `904`): with `lift > 0.3` and the front box (0.6 wide, z 0.5 to 0.8) over a grounded, unheld victim facing away or sideways, `lifted_by` is set; the victim is slaved to the holder's front point at `floor + 0.3`, drive 0, `zone 1.5`, holder top 0.6x; released on lift under 0.3, holder stun, 180 frames, inverted holder, or victim push over 1.3x the holder's. FLIPPER (`src/sim.c:766`): `vy 7` (LOWBOY 0.8x), horizontal 3 forward (HOVER 1.5x), `flip_pending`, `EV_LAUNCH`. Landing (`src/sim.c:1216`): damage 5 after a flipper (`FLIP_LAND_DMG`), 8 over a peak of 1.2, landing on another bot deals it 8; inversion is `rng % 100 < flip_odds` when `flip_pending` or `|vy| > 5` (never HOVER, MELTY; SHELL re rolls 50 percent upright). Probe S4: 20 of 20 launches, 14 of 20 inversions on a LUNCHBOX, peak 1.17 u, 41 airborne frames, landing damage 3.2 through STEEL. Self right: B for HAMMER and FLIPPER (90 frames), A for DRUM at spin over 0.5 (45 frames, 80 percent). Everything else stays inverted; OPS rescues in a 1v1 after the count passes 5.

### 1.7 Weapons: hit decision and damage (`weapon_shape`, `weapon_hit`, `apply_damage`)

A weapon has a hitbox this frame when (line 515): HORIZONTAL and DRUM spin over `SPINNER_HIT_MIN 0.5`; SAW spin over 0.5; FLIPPER, HAMMER, SPIKE while `active > 0`; LIFTER `lift > 0.3`; CRUSHER on the closing frame (`clamp == 1`). The hitbox is a circle at `hb_z0` of radius `hb_r` (spinners, drum, saw) or a box `hb_w` wide from `hb_z0` to `hb_z1` in the attacker frame. `hit_test` (line 845): every attacker hitbox against every other alive body within 0.6 in y, one call per overlapping pair per frame, gated by `hit_cd[victim]`.

`weapon_hit` (line 718): `closing = clamp((|rel normal speed| + tip) / 4, 0.4, 1.3)` with tips HORIZ `3.6 spin`, DRUM `3.0 spin`, swings 4.0, MELTY `3.0 spin`; SAW fixed 0.5, LIFTER and CRUSHER fixed 1.0. `zone = hit_zone` (line 482): front plate 0.6 (WEDGE chassis 0.5), exposed 1.5 (lifted, over 0.15 up, self righting, or inverted against HAMMER, DRUM, SPIKE), spun down weapon circle 2.0, SHELL 0.8x, HAMMER on LUNCHBOX 0.7x. `raw = base spin_or_1 closing zone` (SAW 2x on TREADS sides). `apply_damage` (line 421): armour reduction, red tag 1.4, teammate 0.5, `HIT_CAP` 0.20 of max HP for bot on bot, tallies, stun, WEAR PLATE stuns a spinner 9, the dead dent rule, hit stop by band, `hp_thresholds`. Then knockback `J = knock (0.5 + dmg / 30)` (lateral for HORIZ, up 3.0 and forward for DRUM, forward otherwise), the spinner's reaction `-J` on itself, spin bleed (HORIZ 0.7, DRUM 0.6, MELTY all), self damage, wheel hit, and the FLIPPER, LIFTER, CRUSHER effects.

Cooldowns per victim: spinners 30 frames (`SPINNER_HIT_CD`), MELTY 90, SAW 10 (a tick), swings `active + 2` (one hit per swing), LIFTER 600, CRUSHER 200, FLAME 30, RAM 90, CLASH 90. The clash (line 792): two spinners over 0.5 with overlapping circles: each takes 0.4 of the other's base times spin, spin 0, stun 30, knock 6 apart, `STOP_CLASH`.

Base damage after the retune (`src/parts.h:101`): WEDGE ram 9 to 18, SPIKE 15, LIFTER 4, SAW 4 per 10 frames, FLAME 2 per 30 frames plus a 1 dps burn for 180 frames (not re lit, skips UHMW), HAMMER 20 (4 self on a miss), FLIPPER 6 plus 5 landing, DRUM 17 (3 self), HORIZONTAL 22 (4 self, 15 on a wall), CRUSHER 15 then 6 per second for 2 s, MELTY 12 (3 self, 10 on a wall). Bands after armour: tap 1 to 5, hit 6 to 10, heavy 11 to 17, huge 18 and up (`BAND_*_MIN`), stinger BIG HIT at 18.

### 1.8 Armour, panels, debris, hazards

Armour (`src/parts.h:145`): a flat reduction on every hit from any side (PLY 10, ALU 20, UHMW 15 and half knock and burn immune, STEEL 35, TI 30, WEAR 45 plus the attacker stun). Panels (`panel_state[6]`, `hp_thresholds` line 402): every panel goes to state 1 at 70 HP, 2 at 50, 3 at 30, regardless of where hits land; `shed_next` sheds one random panel at 20 HP (50 on FRAME or AR_NONE, then every 10 on AR_NONE) as a `DB_PANEL` debris (pickup with COMEBACK); `burst` at 0 HP sheds everything plus the wheels, up to 8 debris. Debris (`checks_step` line 1577): gravity 20, one bounce at 0.3, slide drag 0.9, clamped inside the walls, life 360 panels, 240 wheels. Hazards (`hazards_on_bots` line 1329): pit (sink and KO), pulverizer (20, stun 30, vy 2), killsaw (6 per 15 frames, 2x TREADS), flame grate (3 plus burn), screw (1.5 u/s conveyor, 3 per 30), ramp launches (`vy = speed 0.8` over 4 u/s off the top edge, the lip hop 1.5 over 3 u/s), deck count, floor flipper (vy 6), drop zone (25), laser (10 unless LOWBOY or HOVER or airborne), wind. Bumper posts: impulse 5 and 4 damage.

### 1.9 Step order (`sim_step`, `src/sim.c:1962`)

hitstop; prev transforms and clocks; timers; hazard timeline; drive (OPS after `ops_step`); weapon machines; integrate; walls and solids; bot vs bot and push (with the ram test inside); clash, weapon hitboxes, holds; hazards on bots; checks (landing, ring out, count-out, stuck nudge, self right, burn, judges tallies, pickups, debris); hill; win conditions; `prev_in`.

---

## 2. Problems, with evidence

### P1. Ghost bots

**Cause 1, the renderer draws the empty slots (the owner's report).**

- `src/sim.c:1944` to `1951`: OPS parks in `bots[4]` and `nbots = OPS_SLOT + 1 = 5` for any player count; slots `players..3` stay zeroed by the `memset` at line 1921 (`hp_max 0`, body at the origin).
- `src/world.c:2017` (`warm_show`): the boot compile builds four bots and OPS in slots 0 to 3 and 4 so every program links; it hides them but leaves `built = 1`.
- `src/world.c:1884` to `1891`: `in_match = m && m->nbots > 0 && attached != GARAGE_BAY`; for every slot `i < m->nbots` with `b->built`: `if (!b->visible) world_bot_visible(i, 1); sync_bot(b, bt, ...)`. No `hp_max` guard. `game_start_match` (`src/game_flow.c:458`) and `game_attract_start` (`:232`) hide the slots over `players`, and `world_sync` turns them back on at the next frame.
- `sync_bot` (`src/world.c:1557`) then places the chassis at `(0, 0, 0)` heading 0; `burst = !alive && hp <= 0` is true for a zeroed bot so wheels, treads and weapons are hidden and the bare chassis with clean panels is drawn. After a 4 player evening the ghost wears whoever last sat in that slot.
- The ghost is in the sim as nothing: `bots_collide` skips `hp_max 0` (line 1132), `hit_test` skips `!alive` (line 858), the camera skips `hp_max 0` (`src/camera.c:274`). So it is a bot on the field that registers as nothing, which is the report.

Reproduction (rig, deterministic): `node tools/shot.mjs build/red-tag-rumble.wasc build/shots/solo 700,900,1500 tools/scripts/solo.json` then look at `build/shots/solo-900.png`: THE BAY, one player view, POLITE DOORSTOP in front, YEETING BOX at the far wall, and a grey wheel-less chassis sitting on the pit square at the arena centre. `node tools/shot.mjs build/red-tag-rumble.wasc build/shots/title 700,1300 tools/scripts/title.json`: `title-700.png` and `title-1300.png` show the same chassis on the pit through the attract (the attract has only two bots and OPS). Every mode with under 4 seats shows it: SOLO, FIGHT with two or three pads, 3 player FILL WITH BOT (one ghost), TOURNAMENT (two ghosts, since `SETUP.players = 2`), the attract, and both rematch paths. It is strongest in THE BAY and THE DOCK where the origin is in the open; in THE FREE PILE the mound hides it inside the boxes.

Fix (world.c, one line, plus a log for the test): in `world_sync` the in match branch becomes `if (in_match && i < m->nbots && (bt->hp_max > 0 || bt->is_ops))`, else fall through to hide when visible (`else if (b->visible) { if (in_match) world_bot_visible(i, 0); ... }`). Also give `game_start_match` and `game_attract_start` nothing to do: they already hide. Add a `[world] bots=<visible mask>` log line on every phase change and on every `world_bot_visible` change during a fight, so the rig scripts can assert the mask (section 6.4).

**Cause 2, corpses that collide but cannot be hit.**

`eliminate` (`src/sim.c:1776`) leaves a KO_HP bot as a burst frame and a KO_COUNT, KO_FLIP or KO_DECK bot as a whole model with `alive 0`. `bots_collide` keeps them as 0.5 mass props (`ima = 2.0`, line 1141) so bodies bump them; `hit_test` skips `!v->alive` (line 858) and so does `flame_step` (line 830) and `clash_check`. A spinner driven through a counted out bot throws no sparks and loses no spin. Probe S5 (`probe.out` line 167): a full spin CHOP into a KO BY FLIP corpse: 0 weapon hits, the corpse shoved 11.49 u. In LAST BOT STANDING and KING OF THE HILL (5 s respawn) these corpses are on the field for the rest of the fight, and in KO BY FLIP mode (no HP) a flipped out bot is a whole, un hittable model at the spot it died.

Fix: a corpse is a hittable prop. In `hit_test` and `ram_check` treat `!alive && hp_max > 0 && !falling && pit_state != 2` as a valid victim with `dmg 0`: `weapon_hit` runs the contact (knock, recoil, spin cost at half, `EV_HIT` with `zone 0` and the new `corpse` flag) so sparks fly and the spinner bogs; no tallies. OPS should also clear corpses: `OPS_RETURN` picks up a corpse within 3 u of its path and drops it in the zone (a later item for the AI agent; list it as optional).

**Cause 3, the height gap test on the mound rim and the ramps.**

`bots_collide` line 1138: `if (fabs_f(a->b.y - b->b.y) > 0.35f) continue;` and `hit_test` line 861 at 0.6. On the mound, `0.9 t (2 - t)` has a rim slope of 0.45 u per u; a bot on the floor at r 4 and one at r 2.95 (y 0.41) are 1.05 apart (touching) with a y gap over 0.35, so the pair is skipped until they overlap, then pops apart. THE DOCK ramps rise 0.333 per u, so a pair on the ramp 1.05 apart sits at the margin. Probe S1 (`probe.out` lines 2 to 42): the two bots met further up the dome (gap 0.285) and collided, which shows the gap rule only fails near the rim; S7 on the ramp sat exactly at 0.35. Fix in section 3.4: grounded bots always collide unless one is on the deck and the other is not; the interval test is for airborne and lifted bodies only.

### P2. The wedge is a number, not a mechanism

Probe S2 (`probe.out` lines 69 to 105): a ROLL OUT at 3.8 u/s into a standing LAUNCH facing it: one `EV_RAM` for 10.4, the LAUNCH hops to 0.044 u for 10 frames, is pushed 2.7 u and keeps driving. From behind: same push, no hop. The wedge never goes under; the victim's wheels never leave the floor; no tip. Robin 3 (`build/balance --robin 3`, 1296 matches): ROLL OUT 35.4 percent overall, LAUNCH 64.4, and "no KO BY FLIP in the robin": nobody is ever flipped out, since every inverted bot except HAMMER, FLIPPER and DRUM builds stays inverted and drives on at 70 percent or waits for OPS.

### P3. Damage does not show where you hit

`apply_damage` line 455: `if (dmg > 20 && !v->is_ops)` advances the panel under the contact. `HIT_CAP` (line 434) caps bot on bot damage at `0.20 hp_max`: 20 on BOX and LUNCHBOX, 18 on WEDGE, 17 on LOWBOY, 14 on FRAME, 22 on SHELL. Only a SHELL can ever take a 21. Probe S6 (`probe.out` 174 to 177): two hammer blows of 17 and 16 on a CHOP's left side: `panel_state` 1 1 1 1 1 1, from the 70 HP threshold, nothing on the hit panel. The `panel` field of `EV_SHED` exists but `EV_HIT` carries no panel, so fx cannot mark the side either.

### P4. Spinner hits are not energy

`SPIN_BLEED_HORIZ 0.7` per hit whatever the bite; the attacker's recoil equals the victim's knock (`src/sim.c:754`) with no dependence on what was hit; a wedge front is `zone 0.5` with no deflection (probe S3: 9.3 on the wedge front, 18 on the back, the disc keeps grinding at the plate every 30 frames: four hits in 5 s). The whine (`audio_whine`) reads `spin`, so a hit sounds like a volume step, not an energy dump. The victim's motion into the disc is folded into `closing` as an absolute value, so a target driving away from the disc hits as hard as one driving into it.

### P5. Inversion is luck, and a flipped bot is dead weight

`landing` line 1241: `rnd_n(m, 100) < flip_odds`. A perfect flip on a LOWBOY fails 70 percent of the time and the player cannot tell why. Once inverted, WEDGE, NONE, SPIKE, SAW, DRUM and HORIZONTAL builds drive on inverted at 70 percent (the `inv_drive` column) for the rest of the fight; LIFTER, CRUSHER and FLAME builds sit until OPS. There is no tumble in the air and no roll: the model snaps upright or upside down on `inverted`.

### P6. No angular response, no slide

Weapon hits apply no yaw impulse to the victim (`knock` is linear only) and the contact yaw term is halved (`* 0.5f`, lines 1042 and 1180), so a hammer on a corner does not spin the victim and a spinner clip does not twist it. The lateral bleed (P1.2) stops any slide inside 10 frames. A shoved bot looks glued.

### P7. Attack clarity (the physics half of it)

A WEDGE or NONE build has no weapon input: A does nothing, RT does nothing. The ram needs a closing speed over 1.5 u/s, which a TREADS wedge only reaches after 0.4 s of straight driving, so a new player pushing into a bot sees nothing happen. A spinner fires at `spin > 0.5`, with no on screen tell of the threshold beyond the ring alpha. The hammer's 15 frame windup is the only readable tell.

---

## 3. The design

Constraints kept: fixed 60 Hz, `float` with `-ffp-contract=off`, no libm trig (the table), fixed order pair loops, no allocation, native compile for `tools/balance`, every random draw through `m->rng` (the design below adds no new random draws). Sim budget stays under 0.5 ms: 5 bodies, at most 10 pairs, 4 walls, under 20 solids.

### 3.1 Bodies

`body_t` gains (append, never reorder; `sim_hash` keeps hashing `x, z, h, y, hp`):

```c
float pitch, wp;        /* pitch about local x, radians, positive is nose up; pitch rate */
float roll, wr;         /* roll about local z, positive is left side up; roll rate */
float clr, hgt;         /* ground clearance and chassis height: the body occupies [y + clr, y + clr + hgt] */
float lip_f, lip_b;     /* front and back lip heights over the floor (0 when that edge has no wedge) */
float inertia_p, inv_inertia_p;   /* pitch inertia m (hl^2 + hc^2) / 3 with hc = hgt / 2 */
```

`clr` per drive (matches `ride_y` the renderer already draws, `src/world.c:1016` to `1073`): TWO 0.12, FOUR 0.10, SIX 0.10, TREADS 0.12, HOVER 0.22 (a soft skirt: 0.06 for underride purposes, see 3.5), WALKER 0.30, LEG TRACKS 0.10, MELTY 0.10. `hgt` is the chassis H. Centre of mass height `hc = clr + hgt / 2`.

Masses stay the table's (`build_mass`). Inertias: yaw `I = m (hl^2 + hw^2) / 3` (unchanged), pitch `I_p = m (hl^2 + (hgt/2)^2) / 3`. OPS: both inverses 0.

Lips: WEDGE chassis front `lip_f 0.02`; WEDGE weapon (the plate, any chassis) `lip_f 0.04`; LIFTER with `lift < 0.3` `lip_f 0.05`; LOWBOY front `lip_f 0.08` (a pancake nose, not a wedge: it can underride only what has clearance over 0.08 at over 1.5 u/s, which is everything but another LOWBOY and a SIX WHEEL or FOUR WHEEL from the side); everything else 0, meaning no wedge. A SHELL has no lips. OPS forks: 0.05.

Tip angle per chassis `theta_tip[CH_COUNT]` (the pitch past which an unsupported bot goes over, from `atan((L/2) / hc)` rounded to feel): BOX 55 degrees, WEDGE 58, LOWBOY 66, SHELL 50 (rolls, see 3.11), FRAME 55, LUNCHBOX 35. TREADS and LEG TRACKS add 6 degrees (wide, heavy), WALKER subtracts 5 (tall legs), TWO WHEEL subtracts 5 (short wheelbase).

### 3.2 Step order (4.4 adapted)

1. `hitstop`.
2. Prev transforms (`x z h y pitch roll`), clocks, closing.
3. Timers (adds `ride_t`, `grind_t`, `slide_t`, `tread_hit`, the panel wear cooldowns).
4. Hazard timeline.
5. Drive: wheel forces (3.3) into `vx vz w`, using last step's support (`ride` and `wheels_up`).
6. Weapons: machines, spin energy, swing arcs (3.7), flipper plate (3.8).
7. Integrate (3.4): plane, yaw, pitch and roll (free when unsupported, servoed to the support when ridden or on terrain), height channel.
8. Contacts: gather the fixed order contact list (walls, solids, pairs); classify each pair contact as PLAIN, UNDERRIDE, WEDGE_CLASH or RAM (3.5); resolve walls and solids (2 iterations), then pairs (2 iterations) with restitution, friction, positional correction (3.4).
9. Support and push: ride geometry, traction factors, the push force with lifted wheels, pins.
10. Clash; weapon hitboxes (arcs); holds; flame. Hits read the ride state for zones.
11. Hazards on bots.
12. Checks: landings (3.8 decides inversion from `pitch`), tip releases (3.5), pits, edges, count-out, panel pools and thresholds (3.10), pickups, debris, burn.
13. Hill; OPS.
14. Win conditions; `prev_in`.

The one ordering change from today: the ram test moves out of `bots_collide` into step 8's classification so that UNDERRIDE, WEDGE_CLASH and RAM are decided once per pair per step from the pre resolution closing speed, and the push (step 9) runs after every contact has been resolved.

### 3.3 Drive as wheel forces with grip

Replace the velocity controller with a force limited by grip. Per bot, per step, with `mu = DRIVES[].traction`, `g 20`, `load = 1 - 0.75 ride_up` (3.5: the share of the wheels still on the floor) and `boost`, `red tag`, `wheel_hit`, slope as today:

```
F_drive  = t * accel * mass                          (the motor, as the spec's 4.5)
F_grip   = mu * mass * g * load                      (what the floor will give)
F_fwd    = clamp(F_drive, -F_grip, F_grip)
F_fwd   -= drag: 0.6 * mass * vf (coasting, |t| < 0.05) else 0.15 * mass * vf
vf      += F_fwd / mass * DT, then clamp |vf| <= top (boost, red tag, wheel hit applied to top)
lateral: dv_lat_max = mu * g * load * DT * LAT_GRIP[drive]
         vl -= clamp(vl, -dv_lat_max, dv_lat_max)
         if |vl| > 1.0 after that: slide_t++ (EV_SLIDE on the first frame over 2.0 u/s) else slide_t = 0
yaw:     w_target = -s * turn * (1 - 0.4 speed/top) * horiz_spin_factor * load
         w += clamp(w_target - w, -20 DT, 20 DT); w *= 0.85 when !bot_can_drive and grounded
```

`LAT_GRIP`: TWO 0.8, FOUR 1.0, SIX 1.2, TREADS 1.6, HOVER 0.08, WALKER 1.0, LEG TRACKS 1.4, MELTY 0.5. Numbers: TREADS remove up to `1.0 * 20 * 1.6 / 60 = 0.53` u/s of lateral speed per frame, so a 6 u/s HORIZONTAL knock slides them 11 frames and about 0.6 u; TWO WHEEL removes 0.16 u/s per frame, so the same knock slides 37 frames and about 1.9 u; a HOVER removes 0.03 and slides across the arena. Push: `build_push` stays `traction^2 mass 8` but is multiplied by `load`, so a bot with its wheels up pushes at a quarter, which is what lets a wedge win the shove. HOVER keeps its strafe controller (a thrust vector) with `HOVER_BRAKE`, its skirt pinch, and `load` as the pinch factor.

### 3.4 Contacts and integration

Shapes: OBB (box) or circle in the plane as today. Height: a body occupies `[y + clr, y + clr + hgt]` (the lifted and the airborne carry their `y`). Pair test: both grounded (`!airborne && !lifted_by && !clamped_by`): always a candidate unless exactly one is `on_deck` (the deck face is a solid); else the vertical intervals must overlap by 0.05. This removes the 0.35 gap rule (P1 cause 3).

Resolution (per contact, normal from a to b, the contact point `c`):

```
positional: slop 0.01, correction = max(pen - slop, 0) * 0.8 split by inv_mass (walls, solids, OPS: 0)
relative velocity at c: vr = (vb + wb x rb) - (va + wa x ra)        (plane cross products)
vn = dot(vr, n); if vn < 0:
  jn = -(1 + e) vn / (ima + imb + ta + tb)   with ta = (ra x n)^2 iia, tb likewise   (full angular term, no 0.5)
  e = 0.2 bots, 0.3 walls, 0.1 UNDERRIDE contacts, 0.0 for a HOVER
  friction: jt = -vt / (ima + imb + ...), clamped to mu_c * jn with mu_c = 0.4 bots, 0.4 walls, 0.15 UNDERRIDE
  apply J = jn n + jt t to velocities and yaw rates: w += (r x J) inv_inertia (no halving)
```

Two iterations over walls and solids, then two over pairs, in the fixed order `i < j`. Yaw impulses are now full strength on live bots; a dead chassis keeps `inv_inertia` at `2 / (hl^2 + hw^2) * 3` (a 0.5 mass prop with its box's inertia). Wall events as today (`vn 2`), plus `EV_SCRAPE` when a bot slides along a wall with `|vt| > 1.5` for 6 consecutive frames (once per 30).

Integrate: plane and yaw as today; `pitch += wp DT`, `roll += wr DT`. Supported (grounded, not ridden): `pitch` and `roll` relax to the terrain slope (the floor height difference between the nose and the tail, and between the two sides, from `arena_floor_y`) at 10 rad/s, `wp = wr = 0`. Ridden: `pitch` is set from the ride geometry (3.5). Airborne or tipping: free, with gravity torque `wp -= sign(pitch) * TIP_G * DT` where `TIP_G = 9` rad/s^2 while `|pitch| < theta_tip` (it falls back) and `wp += sign(pitch) * TIP_G * DT` past it (it goes over), until the landing.

### 3.5 The wedge model

Classification runs in step 8 for every pair contact between live bots, both ways (a as the attacker). Inputs: the contact normal `n` (from a into b), the attacker's forward `fa`, the contact edge of the victim (which of its four edges the contact point sits on, `edge_of(v, c)`: front, back, left, right, from the local coordinates), the closing speed `cl = dot(va - vb, n)` before resolution, the attacker's lip at its contact edge `lip_a` (its `lip_f` if the contact is on its front edge, `lip_b` on its back edge, else none), the victim's lip at its edge `lip_v` likewise, and the victim's clearance at that edge:

```
clr_v(edge) = v.clr                                    front and back
            = BLOCKED for wheels, treads, legs, leg tracks at the sides (a wheel or a track is there)
            = v.clr for HOVER (0.06 effective: the skirt is soft) and MELTY, any edge
            = v.clr * 0.5 for SHELL any edge (the dome's rim sits low)
approach ok = dot(fa, n) >= cos 35 deg                 the lip meets the target squarely
```

Decision, in this order:

1. `lip_a` none, or `cl < 0.5`, or approach not ok: PLAIN (then the RAM test as today with `RAM_MIN_CLOSING 1.5`, the hop removed).
2. `lip_v` present and the victim's edge is its lip edge (wedge on wedge): compare. `lip_a + 0.01 < lip_v`: the attacker underrides. `lip_v + 0.01 < lip_a`: the victim underrides the attacker (swap roles). Equal within 0.01: the faster one by over 0.8 u/s of closing underrides; otherwise WEDGE_CLASH: both stop (restitution 0.3 on the contact, `e` 0.3), no damage, `EV_WEDGE_CLASH` with `dmg = cl`, 10 amber sparks at the lips, each attacker's `drive load` 0.6 for 20 frames (the plates bind), and the shove proceeds by `build_push`.
3. `clr_v(edge)` BLOCKED (a side with wheels): PLAIN plus a WHEEL hit (3.9) when `cl > 1.5`.
4. `lip_a < clr_v(edge)`: UNDERRIDE.
5. Else PLAIN.

UNDERRIDE state on the victim: `ride_by = a + 1`, `ride_t = 0`, `ride_edge`, `ride_up = 0` (0 to 1: how far up the slope), `ride_s = 0` (the wedge's advance under the victim in u). The attacker: `riding = b + 1`. Each step while both remain in contact and the attacker's forward stays within 50 degrees of the normal (else the victim slides off: RIDE_END, reason SLIP):

```
slope angle  : WEDGE chassis 26 deg (H 0.30 over 0.605), WEDGE plate 20 deg, LIFTER forks 15 deg, LOWBOY nose 12 deg
slope length : S = 0.60 chassis, 0.45 plate, 0.35 forks, 0.30 nose
ride_s      += max(cl_now, 0) * DT  (the wedge advances under the victim by the closing speed; a victim driving away reduces cl_now)
ride_up      = clamp(ride_s / S, 0, 1)
lift height  = ride_up * S * tan(slope)                      (WEDGE chassis: up to 0.29 u)
victim.pitch = asin(lift / (2 hl_v)) toward the attacker (nose up if ridden at the front, tail up at the back), servoed at 12 rad/s
victim.y     = floor_y + lift * 0.5 (the centre rises half the edge lift)
victim load  = 1 - 0.75 ride_up      (3.3: drive, push, lateral grip all scale)
attacker load= 1 + 0.2 ride_up       (the victim's weight presses its wheels down), capped at mu limits
zone (3.9)   = 1.5 on the victim for any weapon while ride_up > 0.3 (the exposed underside), 2.0 for a saw or spike or flame mounted on the attacker (the 2ND weapon slot)
```

Events: `EV_UNDERRIDE` on entry (a, b, edge, `dmg = cl`); `EV_RIDE_END` on exit with the reason in `zone` (0 SLIP sideways, 1 BACKED OFF, 2 TIPPED, 3 CARRIED OFF a drop, 4 OPS). Count-out: a ridden bot's count does not reset by its own wheels (it is not moving on purpose), but the attacker's does.

Tip decision (deterministic, no RNG), evaluated when `ride_up` reaches 1.0 or on RIDE_END:

```
v_tip[chassis] = LUNCHBOX 2.2, FRAME 3.0, BOX 3.4, WEDGE 3.8, SHELL never (rolls off, 3.11), LOWBOY 4.6 (only a boosted wedge)
               + 0.6 for TREADS or LEG TRACKS victims, - 0.4 for a TWO WHEEL victim, - 0.3 for a WALKER victim; HOVER never (carried)
cl_peak = the largest closing speed seen during this ride
if cl_peak >= v_tip: TIP: the victim leaves the support: airborne, vy = 1.2 + 0.3 (cl_peak - v_tip), wp = +3.5 rad/s past the slope (it goes over),
     flip_pending = 1, launched_by = attacker, EV_RIDE_END(TIPPED), EV_LAUNCH (zone 2 = wedge tip)
else if ride_up == 1 and the attacker keeps pushing: the victim is carried on the plate: it stays at full lift, pitch at the slope,
     load 0.25, pushed by the attacker's push minus nothing; after 90 frames of carry or when the attacker's speed drops under 0.5
     it slides off the nearer side (RIDE_END SLIP) with a lateral 1.5 u/s and lands with pitch relaxing (3.4)
else (backed off): drops back, pitch relaxes at 10 rad/s, EV_RIDE_END(BACKED OFF), EV_LAND with dmg 0 when the lift was over 0.15
```

Flip by a wedge inverts on the landing exactly as a flipper launch does (3.8): the pitch keeps integrating in the air, so the chassis is seen going over. A ROLL OUT at 3.8 u/s flips a LUNCHBOX (2.2), a FRAME (3.0) and a BOX (3.4), not another WEDGE (3.8 plus TREADS 0.6 = 4.4) and not a LOWBOY; a TWO WHEEL wedge with boost (9 u/s) flips everything but a SHELL and a HOVER. The AI's attack point for a wedge becomes the victim's front or back, never a wheeled side (an AI item; today it flanks).

Damage: an UNDERRIDE deals the ram damage once on entry (`RAM_BASE 9 + 9 clamp((cl - 2) / 4)`, stun 10, the same cooldown 90) with `EV_RAM`, and nothing per frame; the point of the wedge is control, the 2ND weapon and the tip. Landing after a TIP: landing damage per 3.8 (5 to 8) credited to the wedge.

HOVER victim: `clr 0.06` effective at every edge, so any lip gets under it from any side; a ridden HOVER has `load 0` (the skirt is off the floor: no thrust, it is carried) and never tips; it slides off at the SLIP rule. This is the lever for the HOVER vs TREADS guardrail in THE BAY (HANDOFF risk 1): the wedge now carries the hover into a pulverizer or the pit. Keep the `knock 1.5x` and the push 0.

Ramp and wall interactions: a ridden victim shoved into a wall is pinned at full lift (the attacker cannot advance, `ride_up` holds); a ridden victim carried over an open edge or a pit lip is dropped (RIDE_END CARRIED OFF) and falls as today.

### 3.6 Spinner energy

State: `spin` 0 to 1 stays (the whine, the ring and the HUD read it). Stored energy fraction `e = spin^2`. Spin up as today (`1 / spin_up` per second held, minus half when released). A hit while `spin > SPINNER_HIT_MIN 0.5`:

```
rel     = dot(va - vv, n)                              signed: the victim moving away is negative
tip     = TIP[w] * spin                               HORIZ 3.6, DRUM 3.0, MELTY 3.0, SAW 2.0
bite    = clamp(0.35 + 0.08 (tip + rel), 0.35, 0.80) the energy fraction the hit takes; a target driving into the disc bites more
E_hit   = e * bite
dmg_raw = base * (E_hit / 0.5) * zone * loc           so a full spin, medium bite hit is base; the energy term replaces spin_or_1 and closing
e      -= E_hit;  spin = sqrt(e)                      EV_SPIN_CHANGE(dmg = spin lost) when the drop is over 0.15
J       = KNOCK[w] * (0.4 + 0.6 dmg / base)            HORIZ 6, DRUM 4, MELTY 4, SAW 0.4 (the cap on dmg keeps J bounded)
victim  : knock(J along d) where d is lateral (the normal rotated 90 deg in the spin direction) for HORIZ and MELTY, up 3.0 and forward for DRUM, forward for SAW
          plus a yaw impulse w += (r x J d) inv_inertia  (a corner hit spins the victim)
attacker: knock(-0.7 J along d) and a yaw kick w -= 0.5 J r_tip inv_inertia (the disc drags the frame round)
self    : self_dmg * (E_hit / 0.5) (a glancing hit costs less than a full bite)
cooldown: SPINNER_HIT_CD 30 per victim stays; the next hit's E_hit is smaller anyway
```

Wedge front (the victim's lip edge faces the disc and the contact is on it): `loc 0.4`, the disc is deflected: the attacker gets `vy 1.2` (a hop, airborne 10 frames, `zone 1.5` for the wedge's 2ND weapon), `bite 0.25` (a glance takes little energy), 6 sparks along the slope, `EV_HIT` with `panel PN_FRONT` and the new `deflect` flag. Side or back: `loc 1.0`; a wheel (3.9): `loc 1.2`. SHELL: `loc 0.8` everywhere and a 0.25 bite (the dome sheds the disc). WEAR PLATE: `bite 0.5 e` taken from the attacker as today's 9 frame stun plus 0.1 extra energy lost.

A clash: both over 0.5: each loses `min(e_a, e_b)` of energy (the lesser disc stops, the greater keeps the rest), damage `0.4 base_other * e_other` each, knock 6 apart, `EV_CLASH`.

Walls: HORIZONTAL over 0.5 at `vn 2`: `e = 0`, 15 HP, `EV_SPIN_WALL` (unchanged); DRUM `e *= 0.5`; MELTY 10 HP.

The HUD gauge reads `spin`; the whine pitch reads `spin` and its level `e`, so a hit is heard as the disc bogging.

### 3.7 Hammer and spike as swing arcs

HAMMER: `windup 15` (the tell, arm raised to 110 degrees), then `active 10` (today 4): the head swings from 110 to minus 10 degrees over the 10 frames with `p = (10 - active) / 10`. Head position in the attacker frame: pivot at `(0, clr + hgt + 0.1, -0.1)`, reach 1.0, so the head is at `z = -0.1 + 1.0 sin(angle)`, `y = pivot_y + 1.0 cos(angle)` (angle measured from straight up, 110 down to minus 10 over the swing). The hitbox is a circle of r 0.25 at the head's plane position, valid when the head's `y` is under the victim's top `v.y + clr + hgt + 0.1` and over its clearance. First overlap ends the swing (`active = 0`, `cooldown 72`). Damage `base 20 * (0.5 + 0.5 p) * zone * loc`: a LUNCHBOX top (1.0 high) is met at about p 0.3: 13 raw (the 0.7 LUNCHBOX rule becomes emergent and is removed); a LOWBOY (0.35 high) at p 0.9: 19 raw. Inside reach 0.35 (the victim hugging the hammer) the handle lands: `0.3 base`, `loc` 1. The head hits the TOP panel (3.10). Knock 1.2 forward plus a pitch kick on the victim `wp -= 2.0` (the nose dips; render shows the squash), stun 30, a miss costs 4 as today and dents the floor (`EV_WEAPON_TELL zone 1` stays as the miss event, renamed in fx to a floor strike). Prop damage unchanged.

SPIKE: `active 9`: extend over frames 1 to 5 (`z` from 0.55 to 1.05), retract over 6 to 9. The hitbox is a 0.25 wide box from `0.55` to the current tip. Damage `base 15 * loc` on extension, `0.3 base` on retraction; knock 2.5 forward, stun 18; hits the panel under the tip. A spike into a wedge front: `loc 0.4` and the spike skids up (no hop).

### 3.8 Flipper, air time, tumbling, landing, inversion

FLIPPER: the plate rotates 0 to 70 degrees over `active 4`. Launch when the victim's body overlaps the plate box (as today) and the victim is grounded or ridden:

```
k   = how far back on the plate the victim sits: 0 at the plate tip (z 0.9), 1 at the hinge (z 0.4), from the contact point
vy  = 7.0 * (LOWBOY 0.8) * (0.85 + 0.15 k)       (deep on the plate throws higher)
vfw = 3.0 forward (HOVER 1.5x)
wp  = TUMBLE[chassis] * (0.7 + 0.6 k) + 0.3 * max(dot(vv, -fa), 0)   (a victim driving into the plate tumbles more)
      TUMBLE: BOX 4.5, WEDGE 4.2, LOWBOY 2.7, SHELL 4.5, FRAME 5.0, LUNCHBOX 6.3 rad/s (flip_odds / 50 * 4.5, the table column is reused)
      sign: nose up (the plate lifts the edge nearest the flipper)
airborne, flip_pending, launched_by, zone 1.5, EV_LAUNCH (zone 0)
```

In the air: `pitch += wp DT` (no gravity torque while airborne, the tumble is free); `vy -= 20 DT`. Air time at vy 7 is 0.70 s: a LUNCHBOX with k 0.5 turns `6.3 * 1.0 * 0.7 = 4.4 rad` (252 degrees), a BOX 3.15 rad (180), a LOWBOY at 0.56 s turns `2.7 * 0.56 = 1.5 rad` (86 degrees).

Landing (`landing`): reduce `pitch` to `(-pi, pi]`:

```
|pitch| < 60 deg          : lands on its wheels; pitch relaxes at 10 rad/s; dmg = 2 + 3 (impact_vy / 7) (flip) or 8 (deck, mound, peak over 1.2)
|pitch| > 120 deg         : lands inverted: inverted = 1, pitch = pitch - sign * pi (so the render's 180 flip plus the residual), EV_INVERT; the same dmg
60 to 120 deg (on an edge): a second bounce: vy = 1.6, wp keeps its sign at 0.6x, the next landing decides (deterministic); EV_LAND with zone 2 (BOUNCE)
SHELL                     : the 120 degree band is 150 (a dome rolls back) and a landing with |pitch| in 120 to 150 rolls upright over 30 frames (EV_SELFRIGHT zone 1, the wobble)
HOVER and MELTY           : never inverted: pitch is zeroed on landing (the skirt or the frame rights it), dmg as a flip
```

`flip_odds` and the landing RNG go. The TUMBLE column keeps the chassis character: a LOWBOY needs a deep plate hit and a victim driving in to go over; a LUNCHBOX goes over from almost any launch. Landing damage credit to `launched_by`, `flips_landed`, `inverted_by` as today. Landing on another bot: 8 to it, the lander bounces 2 u/s back (today), plus the lander's `pitch` is set to minus 30 degrees (it slides off nose down) and it never inverts from that landing.

Self right stays as today (B for HAMMER and FLIPPER, A for DRUM), plus the wedge rule: an inverted bot with a lip is a ramp for everyone (its `lip` flips to the other side: `clr 0.02` means it cannot be underridden, but it also cannot underride). Inverted driving stays at 70 percent for the `inv_drive` builds.

### 3.9 The other weapons

CRUSHER: unchanged grab (clamp 24 frames, close hit 15, hold 120 frames at 6 per second, escape at 1.3x push), plus every 30 frames of hold the TOP panel wears one point of its pool (3.10) and `EV_GRIND` ticks with `weapon WP_CRUSHER`; the victim's `pitch` is minus 15 degrees while clamped (hung on the jaw).

SAW: sustained grinding: a tick every 10 frames of contact (4 base, 2x on TREADS sides, `loc` by panel); the panel under the saw wears 1 pool point per 6 frames of contact; `grind_t` counts contact frames and `EV_GRIND` fires on entry (zone 0) and exit (zone 1) so audio can run a loop; sparks per frame are a render state (the fx agent reads `grind_t > 0` and the contact point `grind_x, grind_z` on the attacker). The saw's own spin loses `0.02 e` per tick (it bogs on STEEL and WEAR: 0.04).

FLAME: cone 1.5 u, half angle 30 degrees, a tick every 30 frames of 2 plus the burn (1 dps, 180 frames, re lit only after it ends, UHMW immune): unchanged numbers. Burn becomes a status with events: `EV_BURN_START` (a, b) and `EV_BURN_END` (b); a burning bot's TOP panel wears 1 pool point per 30 frames; `burn_t` is the render state.

LIFTER: unchanged hold, plus the lowered forks are a lip (3.1) so a LIFTER underrides like a plate (slope 15 degrees over 0.35) and the lift itself raises the ridden victim: `lift > 0.3` on a ridden victim converts the ride into the hold with no facing test (today's "facing away or sideways" test stays for a grab without a ride). A held victim: `pitch` minus 20 degrees (render), `EV_LIFT` on grab, `EV_LIFT_DROP` on release.

WEDGE and NONE builds get a weapon input: A or RT is DIG IN: 20 frames at `accel 1.6x` and `lip - 0.01` (the plate scrapes), cooldown 90, `EV_WEAPON_TELL zone 2` (a scrape, 4 amber sparks at the lip, SFX), so A always does something and the ram's 1.5 u/s is reached in 0.25 s instead of 0.4 s. The AI uses it on the approach.

Wheel hits: a contact on a victim's side within 0.18 of a wheel station (wheel stations: TWO `z = 0`, FOUR and SIX `z = +-0.35 hl`, TREADS the whole side as a track, WALKER legs `z = -0.36 L, 0, +0.36 L`, LEG TRACKS the whole side) by any weapon or ram over the hit band: `loc 1.2`, `wheel_hit 240` (top 0.6x) for TWO WHEEL as today, `wheel_hit 180` at 0.8x for FOUR and SIX, `tread_hit 180` at 0.85x and lateral grip 0.7x for TREADS and LEG TRACKS, a WALKER loses `load 0.7` for 120 frames (a limp). `EV_HIT` carries `panel` PN_LEFT or PN_RIGHT and the `wheel` flag.

MELTY: the body is the disc (3.6 with `base 12`, `TIP 3.0`, mass the body mass); a spinning MELTY is never underridden (its skids are at 0.10 but the contact edge changes every frame: treat `clr` as BLOCKED while spin over 0.5) and a stopped one is a LOWBOY (clr 0.10, no lips).

### 3.10 Per location damage: panel pools

Each panel `p` (FRONT, BACK, LEFT, RIGHT, TOP; MOUNT is the weapon's own plate) has a pool `panel_hp[p]` (new `bot_t` field, `uint8_t` tenths or `float`) set by armour: NONE 0 (no panels; FRAME and AR_NONE draw the frame), PLYWOOD 20, ALUMINIUM 28, UHMW 24, STEEL 36, TITANIUM 32, WEAR PLATE 44. Every hit names its panel: the contact point's edge for body hits (`edge_of`), TOP for the hammer, the pulverizer, a drop, a landing on a bot and the crusher hold, FRONT for a wedge glance. Damage after armour goes to the chassis HP as today (the KO pool, `HIT_CAP` unchanged) and the same amount wears the panel's pool (saw and flame wear by the rules above). Panel state from its pool: 0 over 70 percent, 1 at 70, 2 at 45, 3 at 20, 4 (gone) at 0: `shed_panel` fires then, with the debris, `EV_SHED(panel)` as today (pickup with COMEBACK). A gone panel: hits there take `reduction 0` and `loc 1.25` (the exposed internals), so a stripped side is where you aim; the HUD can draw the bot outline with the stripped side in rust (a draw agent item).

The global thresholds stay for the state only as a floor: at 50 HP every panel is at least state 1, at 30 at least state 2 (smoke and fire follow HP as today). `shed_next` (the random shed at 20 HP) becomes "the most worn panel sheds at 20 HP", and FRAME or AR_NONE from 50 as today. `EV_PANEL_WEAR` fires when a panel's state advances (a, b, panel, zone = new state) so fx can spark that panel and audio can play the dent. The dead `dmg > 20` rule is deleted.

Armour also gets a side bias for the TOP: hammers and pulverizers hit the TOP pool at 1.25x (lids are thin) unless WEAR PLATE.

### 3.11 Chassis and drive differences, in one table

| Part | Sim rule |
|---|---|
| BOX | the reference: clr by drive, hgt 0.40, tip 55 degrees, tumble 4.5, v_tip 3.4 |
| WEDGE | lip_f 0.02 (slope 26 over 0.60), hgt 0.30, tip 58, v_tip 3.8; its front is a 0.4 glance zone for spinners with the deflect hop; cannot mount HORIZONTAL (unchanged) |
| LOWBOY | hgt 0.25, lip_f 0.08 nose (slope 12 over 0.30: it underrides only clearances over 0.08 at over 1.5 u/s), tip 66, tumble 2.7, v_tip 4.6, flipper 0.8x, ducks the laser |
| SHELL | circle; loc 0.8 everywhere and bite 0.25 (deflects spinners); any lip gets under its rim (clr 0.5x) but it rolls off sideways at ride_up 0.5 (RIDE_END SLIP, lateral 2.0), never tips from a wedge; landing band 150 and the roll upright; 110 HP |
| FRAME | mass 0.8 chassis, knock 1.25x, tumble 5.0, v_tip 3.0, panels are bolt on plates only with armour, parts fly from 50 HP; top 1.1x |
| LUNCHBOX | hgt 0.70, tip 35, tumble 6.3, v_tip 2.2 (a ROLL OUT at speed flips it); the hammer meets it at p 0.3; the only top mount for a 2ND weapon |
| TWO WHEEL | clr 0.12, LAT_GRIP 0.8 (slides), accel 12 (quick), tip minus 5, v_tip minus 0.4, wheel hit 0.6x top 240 frames; roll: a lateral impulse over 4.0 J rolls it (roll to 90 degrees then a BOUNCE landing) |
| FOUR WHEEL | clr 0.10, LAT_GRIP 1.0, loses a wheel at 30 HP (0.8x top), wheel hit 0.8x 180 frames |
| SIX WHEEL | clr 0.10, LAT_GRIP 1.2, climbs 0.4 benches and the lip (unchanged), wide turn |
| TREADS | clr 0.12, mu 1.0, LAT_GRIP 1.6 (does not slide), push 17.2 on ROLL OUT, no boost, sides BLOCKED to underride, tread hit 0.85x and grip 0.7x 180 frames, saws 2x on the sides, tip plus 6 |
| HOVER | clr 0.22 drawn, 0.06 effective: underridden from every side and carried with load 0; never inverted; push 0; knock 1.5x; LAT_GRIP 0.08 (slides); floats over floor hazards |
| WALKER | clr 0.30 (every wedge gets under it from the front or back), sides BLOCKED, mass 0.35, slow, push strong (mu 0.7, heavy build), tip minus 5, a limp on a leg hit; steps over pit lips |
| LEG TRACKS | clr 0.10, LAT_GRIP 1.4, climbs ramps and the deck at full speed, immune to screws, sides BLOCKED, tip plus 6 |
| MELTY | the body is the spinner (3.6, base 12, 3 self); BLOCKED to underride while spinning, a LOWBOY when stopped; LAT_GRIP 0.5; never inverted |

### 3.12 Damage formula, caps and cooldowns (the one place)

```
dmg = base * energy * closing * zone * loc * (1 - reduction_panel) * red_tag(1.4) * team(0.5)
  energy  = E_hit / 0.5 for spinners, drum, melty (3.6); 1 for the rest
  closing = 1 for spinners (inside energy); clamp((|rel| + tip) / 4, 0.4, 1.3) for swings and the ram as today; fixed 0.5 saw, 1.0 lifter and crusher
  zone    = 1.0; 1.5 exposed (lifted, ridden over 0.3, airborne, self righting, inverted against HAMMER DRUM SPIKE); 2.0 a spun down disc; SHELL 0.8
  loc     = 1.0; 0.4 a wedge front or a LOWBOY nose against a spinner or spike; 1.2 a wheel; 1.25 a stripped panel; 1.25 TOP against a hammer or pulverizer
  reduction_panel = the armour's reduction, or 0 when that panel is gone
  HIT_CAP 0.20 of max HP on bot on bot damage stays; the bands stay (tap 1 to 5, hit 6 to 10, heavy 11 to 17, huge 18 and up)
```

Cooldowns per victim: spinners 30, MELTY 90, SAW 10, swings one per swing, LIFTER 600, CRUSHER 200, FLAME 30, RAM 90, CLASH 90, WEDGE_CLASH 60, the corpse 30 (dmg 0). Hit stop by band from the sim as today.

Expected fight shape: unchanged hit sizes (the retune of R4 stands), fewer spinner hits per approach (the energy bite is 35 to 80 percent instead of a 70 percent flat bleed, so a glance leaves the disc dangerous), more rams that mean something, and flips by wedges, so fights stay in the 35 to 60 s band with a new KO cause (KO BY FLIP from wedges) appearing in the robin. If the mean drifts under 35 s, raise `RAM_CD` to 120 before touching base damage (the spec's order: tags, self damage, base last).

### 3.13 Ghost fixes, restated as work items

1. `src/world.c` `world_sync`: the `hp_max > 0 || is_ops` guard; hide a visible mesh whose slot is empty. One line plus the `[world] bots=` log.
2. `src/sim.c` `hit_test`, `ram_check`, `flame_step`: corpses are hittable props (dmg 0, sparks, recoil, half spin cost, `EV_HIT corpse`).
3. `src/sim.c` `bots_collide` and `hit_test`: the grounded rule replaces the 0.35 and 0.6 gaps.
4. Optional (AI and OPS): OPS sweeps a corpse to its zone on the way back.

---

## 4. Interfaces

### 4.1 `sim.h` additions (owned by the sim agent; additive only)

```c
/* body_t, appended */ float pitch, wp, roll, wr, clr, hgt, lip_f, lip_b, inertia_p, inv_inertia_p;
/* bot_t, appended */
uint8_t ride_by, riding, ride_edge; int16_t ride_t; float ride_up, ride_s, ride_cl_peak;   /* the underride (3.5) */
float panel_hp[6]; int16_t panel_wear_cd[6];                                                 /* the pools (3.10) */
int16_t grind_t, slide_t, tread_hit, dig_t, dig_cd; float grind_x, grind_z;                  /* saw contact, sliding, tread hit, DIG IN */
uint8_t wheel_flag;                                                                          /* the last hit was a wheel */
/* event_t, appended */ uint8_t panel, weapon, flags; float j;
/* flags bits */ #define EVF_CORPSE 1  #define EVF_DEFLECT 2  #define EVF_WHEEL 4  #define EVF_STRIPPED 8  #define EVF_TOP 16
/* new API */
int   sim_hit_panel(const bot_t *v, float cx, float cz, int weapon);   /* the panel a contact lands on (edge_of plus the TOP rules) */
float sim_spin_energy(const bot_t *b);                                 /* spin^2, for the whine level and the gauge */
int   sim_ridden(const bot_t *b);                                      /* ride_by != 0 */
```

Tables in `parts.h` (the parts agent, additive): `DRIVE_CLEAR[DR_COUNT]`, `LAT_GRIP[DR_COUNT]`, `CHASSIS_TIP_DEG[CH_COUNT]`, `CHASSIS_TUMBLE[CH_COUNT]`, `CHASSIS_VTIP[CH_COUNT]`, `ARMOUR_PANEL_HP[AR_COUNT]`, the lip and slope constants per wedge kind, `WEAPON_REACH` for the hammer, all `static const` with `-D` overrides on the balance knobs (`V_TIP_*`, `BITE_*`, `LAT_GRIP_*`).

The renderer reads `pitch`, `roll` (interpolated like `h`), `ride_up`, `grind_t`, `burn_t`, `panel_state`, `spin` and `sim_spin_energy`. `place_root` adds the pitch and roll rotations about the body's local axes before the inverted flip; `sync_bot` interpolates `pitch` and `roll` with the previous values (`pp, pr` appended to `body_t` for the prev transforms, stored in step 2).

`sim_hash` is unchanged (x, z, h, y, hp per bot plus debris), so `--hashcheck` and `--replay` keep their meaning; the new state is derived every step from the hashed state and the inputs.

### 4.2 `ai.h`

No signature change. The AI agent reads `ride_by`, `riding`, the lips and clearances to pick the attack edge (a wedge attacks the front or back of a wheeled bot, any side of a HOVER or WALKER front), uses DIG IN, and treats a ridden self as RECOVER (reverse hard, turn).

---

## 5. Events the sim must emit

Existing events keep their meaning; the fields in brackets are the additions in `event_t` (`panel, weapon, flags, j`). `a` is the attacker or subject, `b` the victim.

| Event | When | Fields | What graphics and audio do with it |
|---|---|---|---|
| EV_HIT | every weapon contact that dealt damage, or a corpse contact (dmg 0, EVF_CORPSE) | a, b, zone x10, x z y, dmg, nx nz, panel, weapon, j, flags (DEFLECT, WHEEL, STRIPPED, TOP) | band by dmg; sparks in the armour colour at the panel; the deflect hop sparks along the slope; a wheel hit shows rubber chips and the wheel wobble; a stripped panel hit shows bone sparks and a louder clang; the number at the contact; rumble by band; the clang pitch by weapon |
| EV_RAM | a wedge or NONE ram on entry to a contact | as EV_HIT, weapon WP_WEDGE | the ram band, dust at the lip |
| EV_UNDERRIDE | a got under b at edge | a, b, zone = edge, dmg = closing speed, x z | a scrape and a metal slide SFX, dust from the victim's lifted wheels, the victim's HUD status LIFTED (new word WEDGED), the camera lag rises on the victim |
| EV_RIDE_END | the ride ends | a, b, zone = reason (0 SLIP, 1 BACKED OFF, 2 TIPPED, 3 CARRIED OFF, 4 OPS) | a thud (SLIP, BACKED OFF), nothing extra for TIPPED (EV_LAUNCH follows), the drop sting for CARRIED OFF |
| EV_WEDGE_CLASH | two lips meet | a, b, dmg = closing, x z | 10 amber sparks at the lips, a bright clank, both pads a short buzz |
| EV_WHEEL_HIT | a wheel, tread or leg hit that applied a mobility penalty | a, b, panel (LEFT or RIGHT), weapon, dmg | rubber chips, a wheel wobble in sync_drive for `wheel_hit` frames, a flat tyre SFX, the status word LIMPING |
| EV_LAUNCH | a flipper launch (zone 0), a ramp or floor flipper (1), a wedge tip (2), a drum pop (3) | a, b, zone, x z, dmg = vy, j = wp | SFX_LAUNCH by zone; the AIRBORNE stinger for zone 0 and 2; dust; the victim camera lag |
| EV_LAND | every landing | a (launcher or self), b, zone (0 wheels, 1 inverted, 2 BOUNCE on an edge, 3 on a bot), dmg, j = impact vy, panel (TOP when inverted) | dust by impact; a thud by impact vy; the number if dmg; BOUNCE plays a clatter and keeps the tumble visible |
| EV_INVERT | a bot comes to rest inverted | a victim, b launcher, zone (0 flipper, 1 wedge, 2 hazard, 3 roll) | the INVERTED stinger and klaxon as today; the count starts |
| EV_SELFRIGHT | a self right succeeded (zone 0) or a SHELL rolled upright (1) or OPS righted it (2) | a | the pop and the dust |
| EV_PANEL_WEAR | a panel's state advanced | a, b, panel, zone = new state (1 to 3), dmg = pool left | the dent swap sparks at that panel (today's geometry swap plus 6 sparks), a dull dent sound; at state 3 a creak |
| EV_SHED | a panel left the bot (state 4) | a, b, panel, x z y | the debris and 8 sparks as today, the clatter; the HUD outline marks the side in rust |
| EV_BURST | 0 HP | as today | as today |
| EV_SCRAPE | a lip, an inverted bot or a stripped panel scraping the floor or a wall for 6 frames (once per 30) | a, zone (0 floor, 1 wall), x z, dmg = speed | a short scrape loop, 4 amber sparks along the velocity; the wall scrape adds the WALL camera shake at half |
| EV_GRIND | saw or crusher contact start (zone 0) and end (1); CRUSHER hold ticks (2) | a, b, weapon, panel, x z | a grinding loop between start and end at the contact point (the attacker's `grind_x, grind_z` is the per frame position), continuous sparks from fx reading `grind_t`, the crowd murmur rises |
| EV_BURN_START, EV_BURN_END | the burn status | a, b | fire on the bot (today's `burn_t` emitter), a hiss loop between them, the BURNING word |
| EV_SPIN_CHANGE | a spinner lost over 0.15 of spin to a hit, a clash or a wall (zone 0 hit, 1 clash, 2 wall), or crossed 0.5 going up (3, the armed threshold) | a, dmg = spin lost (or spin for zone 3), weapon | the whine drops its pitch and level (the bog), a dull wump; zone 3 plays a short click and lights the ring at full alpha so the player knows the disc is armed |
| EV_CLASH, EV_SPIN_WALL, EV_WALL | as today | plus j | as today; EV_WALL with j drives the thud volume |
| EV_SLIDE | lateral speed over 2.0 u/s on a grounded bot (once per 30 frames) | a, dmg = lateral speed, nx nz = slide direction | a skid SFX by drive (rubber for wheels, a clatter for treads, nothing for HOVER), skid dust behind the wheels for `slide_t` frames |
| EV_LIFT, EV_LIFT_DROP | the lifter or OPS grabs and releases | a, b | the fork sound, the status LIFTED, the drop thud |
| EV_WEAPON_TELL | zone 0 the hammer windup (as today), 1 a hammer miss on the floor, 2 DIG IN (a wedge or NONE build pressed A), 3 the flipper reset, 4 the crusher closing | a, zone, x z y | the arm raise SFX, the floor strike sparks, the DIG IN scrape and the camera dip, the plate click |
| EV_HAZ_HIT, EV_HAZ_TELL, EV_BENCH_BREAK, EV_BUMPER_BREAK | as today | EV_HAZ_HIT adds panel TOP for pulverizers and drops | as today, plus the panel wear sparks |
| EV_KO, EV_MINI_KO, EV_COUNT_TICK, EV_OPS_*, EV_RED_TAG, EV_PICKUP, EV_NOT_VACUUM, EV_TAUNT, EV_BOOST, EV_HILL_*, EV_CLOSING, EV_STUCK, EV_RESPAWN | as today | | as today |

Per frame render state (not events): `spin`, `sim_spin_energy`, `ride_up`, `ride_by`, `pitch`, `roll`, `grind_t` and `grind_x, grind_z`, `slide_t`, `burn_t`, `wheel_hit`, `tread_hit`, `panel_state[]`, `panel_hp[]`, `dig_t`, `lift`, `clamp`, `windup`, `active` (the hammer arc is posed from `active` with the same 110 to minus 10 curve so the head is where the hit is).

Audio shape this asks for (for the audio agent, not designed here): a clang family keyed by weapon and panel material (the `weapon` and `panel` fields plus the victim's armour), a bog on EV_SPIN_CHANGE, a scrape loop (EV_SCRAPE, EV_UNDERRIDE), a grind loop (EV_GRIND), a skid (EV_SLIDE), a dent (EV_PANEL_WEAR), a clatter (EV_LAND zone 2 and EV_SHED), a hiss loop (EV_BURN_*). `audio.h` grows by `audio_loop(slot, kind, level, pitch)` for the three loops, pooled per slot like the whines.

---

## 6. Acceptance tests

### 6.1 `build/balance --scenario <name>` (new, native, deterministic)

A table of named scenarios in `tools/balance.c` (`scenario_t { name, setup(), drive(frame, pads), assert() }`), each printing PASS or FAIL with its numbers; `--scenario all` runs the table and exits non zero on any FAIL. The probe harness in `probe.c` is the pattern (place bots, drive with `in.ly` and `in.buttons`, read events). Hazards off, no OPS unless named.

| Scenario | Setup | Must hold |
|---|---|---|
| wedge-front-lunchbox | THE BAY; ROLL OUT at (-3.5, 0) facing plus x, full throttle; STOMP standing at (0, 0) facing minus x | EV_UNDERRIDE within 90 frames with edge FRONT; STOMP `ride_up` reaches 1.0; `ride_cl_peak >= 2.2`; EV_RIDE_END(TIPPED), EV_LAUNCH zone 2, EV_INVERT zone 1 within 240 frames; STOMP ends inverted |
| wedge-side-lunchbox | same, STOMP facing plus z (its side to the wedge) | no EV_UNDERRIDE; EV_RAM once; STOMP never leaves the floor; a push of over 1.5 u in 120 frames (treads win the shove) |
| wedge-front-box-slow | ROLL OUT at 0.6 throttle (about 2.3 u/s) into a BOX (LAUNCH) front | EV_UNDERRIDE; `ride_up` reaches 1.0; no TIPPED (v_tip 3.4); the LAUNCH is carried over 2 u; load under 0.3 while ridden (its own full throttle reverse moves it under 0.3 u in 60 frames); RIDE_END(SLIP) or (BACKED OFF) when the wedge stops |
| wedge-vs-wedge | two ROLL OUTs head on at full throttle | EV_WEDGE_CLASH, no EV_UNDERRIDE, both speeds under 0.5 within 10 frames; with one at 0.4 throttle the faster underrides (EV_UNDERRIDE a = the faster) |
| wedge-under-hover | ROLL OUT into a TOAST from its side | EV_UNDERRIDE (HOVER is open every side); the hover's `load` is 0 while ridden; it is carried over 3 u; never EV_INVERT |
| wedge-lowboy | ROLL OUT into a CHOP front at 3.8 | EV_UNDERRIDE (lip 0.02 under clr 0.10) but never TIPPED (v_tip 4.6): the CHOP is carried and slides off; the same with a TWO WHEEL wedge build (BOX, TWO, WEDGE weapon) boosted to 9 u/s: TIPPED |
| spinner-wedge-glance | CHOP at spin 1 into a ROLL OUT front | first EV_HIT has EVF_DEFLECT, `loc 0.4`: dmg under 9; the CHOP gets `vy 1.2` (airborne 8 to 12 frames); its spin after the hit is over 0.8 (bite 0.25) |
| spinner-side-full | CHOP into a ROLL OUT side | first EV_HIT dmg 16 to 18 (the cap), spin after the hit under 0.6 (bite over 0.5), the ROLL OUT's lateral speed over 2.0 after the hit, EV_SLIDE on a TWO WHEEL victim (run the pair again with LAUNCH) |
| spinner-moving-target | CHOP at spin 1 standing; a LAUNCH driven into its disc at 6 u/s versus one driving away at 3 u/s past it | the hit on the inbound target deals at least 1.4x the hit on the outbound one; the outbound hit still fires |
| spinner-wheel | CHOP into the side of a LAUNCH at its wheel station (z 0) | EV_HIT flags WHEEL, EV_WHEEL_HIT, `wheel_hit 240`, top speed 0.6x (measure 60 frames of full throttle) |
| hammer-arc | STOMP taps A at 1.2 u from a standing STOMP, then from a standing CHOP | hits land in the active window with `p` 0.2 to 0.4 on the LUNCHBOX (dmg 11 to 14 raw before STEEL) and 0.8 to 1.0 on the LOWBOY (18 to 20 raw); both EV_HIT carry panel TOP and EVF_TOP; EV_PANEL_WEAR on the TOP after two hits on the CHOP; from 0.3 u the handle hit is under 8 raw |
| spike-extend | a SPIKE build taps A at 0.8 u | the hit lands in frames 1 to 5 of active at full base; a hit during retraction (start the tap with the victim at 1.0 and driving in) is 0.3x |
| flip-inverts-tall | LAUNCH taps A with a STOMP on its plate, 20 seeds | 20 of 20 launches, 20 of 20 inverted (deterministic: the LUNCHBOX tumbles 252 degrees); EV_LAND zone 1; airtime 38 to 44 frames; landing dmg 2 to 5 |
| flip-lowboy | LAUNCH on a standing CHOP, then on a CHOP driving into the plate at 5 u/s | standing: EV_LAND zone 0 or 2 then upright (no inversion); driving in: inverted (the 0.3 per u/s term adds 1.5 rad/s) |
| flip-bounce | LAUNCH on a BOX with k 0.5 | pitch at the first landing in 60 to 120 degrees: EV_LAND zone 2, a second EV_LAND decides; the whole sequence under 90 frames |
| crusher-hold | a CRUSHER build (4 players) closes on a LAUNCH | 15 on the close, 6 per second for 120 frames, EV_GRIND zone 2 ticks, the TOP pool wears 4 points, the victim's drive is 0 (position slaved) |
| saw-grind | a WEDGE plus SAW build holds against a LAUNCH side for 120 frames | 12 ticks of 4 base (3.2 after ALU), the LEFT or RIGHT pool wears 20 points (state 2), EV_GRIND start and end, `grind_t` continuous |
| flame-burn | TOAST holds A at 1.0 u from a LAUNCH for 120 frames, then releases | 4 ticks of 2, one EV_BURN_START, the burn deals 3 over 180 frames, EV_BURN_END at the end, no re light while burning; a TOAST on a TOAST (UHMW): no burn |
| panel-location | STOMP hammers a CHOP's left side twice, then a HORIZONTAL hits its right side twice | LEFT pool 28 to under 10 (state 3), RIGHT state 2, FRONT and BACK state 0 until HP 50; the third left hit sheds LEFT (EV_SHED panel LEFT); the fourth hit on the stripped LEFT has EVF_STRIPPED and `reduction 0` (dmg 1.25x the third) |
| corpse-hittable | CHOP at spin 1 into a KO BY FLIP corpse | EV_HIT with EVF_CORPSE and dmg 0 every 30 frames while in contact; the CHOP's spin drops by half the usual bite; the corpse moves |
| ghost-sim | SOLO setup (players 2, OPS on) | `nbots 5`, slots 2 and 3 have `hp_max 0`, never appear in any event `a` or `b` over 3000 frames (the sim half of the ghost test; the render half is 6.4) |
| mound-contact | THE FREE PILE, a LAUNCH parked at r 4.1 on the floor, a ROLL OUT driving out from r 2.0 | contact when the centre distance is 1.05 plus or minus 0.03 (no overlap under 1.0 ever) |
| slide-drives | a 6 u/s lateral impulse on each drive | the distance slid before lateral speed drops under 0.3: TREADS under 0.8 u, FOUR WHEEL 1.0 to 1.6, TWO WHEEL 1.6 to 2.4, HOVER over 6 |
| push-treads | ROLL OUT vs LAUNCH head on, both full throttle, as probe S8 | the pair moves toward the LAUNCH side over 6 u in 240 frames; the same with the LAUNCH ridden: over 8 u (load 0.25) |
| dig-in | a ROLL OUT presses A while touching a LAUNCH's front at 0 speed | EV_WEAPON_TELL zone 2, then EV_UNDERRIDE within 40 frames (the ram threshold is reached in under 0.3 s) |

### 6.2 Determinism and replay

- `build/balance --hashcheck` PASS (two runs identical, the snapshot resume identical), with the hash still over `x, z, h, y, hp` and debris.
- The cart's `[sim] f= h=` lines against `build/balance --replay` for `drive-*.json`, `dpad-turn.json`, `2p-fight.json`, `4p-fight.json`: identical after regenerating the fight fixtures with `build/balance --make-fight` (HANDOFF: regenerate after any sim or AI change).
- No new `rnd01` or `rnd_n` call in the contact, wedge, spinner, swing, flip or landing paths (grep the diff); the only RNG users stay debris scatter, shed choice fallback, the DRUM self right roll, OPS lines, the pit cover.
- `-ffp-contract=off` on both builds; no libm trig (the build's grep).

### 6.3 Balance

- `build/balance --robin 20` (driver ai, every arena, hazards both): mean fight 35 to 60 s; every preset 35 to 65 percent; WEDGE over HORIZONTAL at least 40 percent; HOVER under 45 percent against TREADS in THE BAY (the wedge carries it now; if it still fails, the AI shove is the next lever, not the physics); KO BY FLIP occurs (new: wedges flip LUNCHBOX and BOX builds); no stalls; `[stuck]` nudges 0.
- `build/balance --modes 4`: every mode ends with no stuck frames.
- `build/balance --check-arenas` PASS.
- A new line in the robin summary: rides per match, tips per match, flips by wedge and by flipper, glances, wheel hits, so the architect can read the new mechanics' rates (target: 2 to 6 underrides per ROLL OUT match, 0.3 to 1 tips).

### 6.4 Rig and screenshots

- `[world] bots=<mask>` logged by world.c on every change: `solo.json` must log `bots=0x13` (slots 0 and 1 plus OPS at bit 4, logged as hex) during the fight and never a bit for slots 2 or 3; `title.json` the same during the attract; `4p-fight.json` `0x1f`; `mode-tourn.json` `0x13` in every bracket fight. Looked at: `build/shots/solo-900.png` after the fix shows nothing on the pit.
- New fixtures recorded with `--make-fight`: `wedge-flip.json` (a ROLL OUT vs STOMP AI fight in THE BAY that contains an underride and a tip: shots at the underride frame, the tip frame plus 20, the landing), `spinner-glance.json` (CHOP vs ROLL OUT: the deflect hop frame), `panels.json` (a fight that strips a side: a shot of the bot with one side gone). Each shot is looked at: the ridden bot must show its nose up on the wedge, the tumble must be visible in three frames, the stripped side must read as missing.
- `feel.json` still shows its ram at 863 (regenerate if the frame moves).
- Keyboard play in Chrome (`node tools/serve.mjs 8794; node tools/play.mjs`): a first SOLO fight on EASY still runs about a minute with the player over 80 HP at ten seconds; pressing A on a ROLL OUT scrapes (DIG IN) and the practice bot is underridden at least once in the log.

### 6.5 Performance

`TIMING=1 node tools/shot.mjs ... 4p-fight.json`: the host ms per frame unchanged within 0.2 ms (the sim adds under 0.1 ms: the contact list is at most 10 pairs, 4 walls and the solids, 2 iterations each; the arcs are one circle test per frame).

---

## 7. Order of work and risks

1. Ghost fixes (3.13): a day, no balance impact, ship first.
2. Bodies, contacts, drive grip, slides (3.1, 3.3, 3.4) with the scenario harness: the robin must still pass before any weapon changes.
3. The wedge (3.5) and the flip tumble (3.8): the two mechanics the owner asked for by name; `flip_odds` becomes the tumble column.
4. Spinner energy (3.6), swings (3.7), the other weapons (3.9), wheel hits.
5. Panel pools and location damage (3.10), the events (5), then the render and audio agents consume them.

Risks: the wedge makes ROLL OUT strong (it was 35 percent; the tip rule's `v_tip` and the SLIP after 90 frames of carry are the knobs, exposed as `-D` overrides); the flipper on a LOWBOY becomes reliable only against a moving target, which is a skill test (watch the LAUNCH vs CHOP cell, it was 82 percent LAUNCH in robin 3); the AI needs the attack edge rule or wedges will keep flanking into wheels and never underride (an AI agent item, before the robin is judged); pitch and roll are new render inputs and the chase camera reads `y` only, so a ridden bot's camera may clip the wedge until the camera agent adds the pitch; the corpse hit path must not credit tallies or reset counts (assert in corpse-hittable).
