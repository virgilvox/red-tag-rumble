# RED TAG RUMBLE: final design specification

A split-screen robot combat cart for wasmcart, built in its own folder beside DO-OCRACY. One to four players on pads (the RetroArch keyboard is pad 0), split-screen through three.c viewports and scissors, no CLASP, no network, deterministic 60 Hz sim, Raspberry Pi 5 class performance. This is the implementation document: a C programmer with the four maps (platform, combat brief, audio, HUD) and this file needs nothing else. Every number is a starting value to type in; the balance tool in section 15 is how numbers change.

This document is the synthesis of three design proposals. The base is RED TAG RUMBLE (the builder-first proposal). Grafted in from BENCH WARS (feel first): the Hit Contract severity bands, the timescale accumulator with render interpolation, the sin table and replay hash, the arena checker, the cube sparks, the wall-safe chase camera, the enemy chevron, the four-row rematch vote, the hazard-seat viewport, the humour placement rule and the milestone-3 feel gate. Grafted in from SPIN CYCLE (party first): RED TAG overdrive, FREE PILE pickups, auto-generated bot names, CLOSING TIME and FIGHT LENGTH, the hall of fame, hazard seats on the floor flipper, OPS LEADER TAX, the left-column title scrim, HP ghost drain and status words, mini-KO for free-for-all eliminations, screen-space bursts, per-arena drive scripts, the ASCII arena maps, the KO cause card and the content tables named in section 16. Where the judges asked for a fix in the base (the vote screen, the SOLO hint, a three-player layout) it is fixed here.

House rules observed in every string, comment and screen: no em or en dashes, no emoji, no bold for emphasis, ASCII caps in game, roles never names, HeatSync material is general hackerspace culture only. Visual language is GANTRY: char, bone, hazard amber as a fill, rust, live teal, square corners, solid offset shadows, caution tape. The HeatSync mark is drawn only by `hud_logo`, only on the title marquee, never on a bot, never recoloured.

## 0. Stance, and what this document resolves

1. The hit is the product, the garage is the strategy, and nobody at the cabinet ever has nothing to press. When two features fight, the one that makes a hit more readable or a wait shorter wins.
2. The sim never sees the renderer, wall time, the camera or the pads. `sim.c` (the brief's physics.c) plus `bot.c`, `arena.c`, `ai.c` and `parts.h` compile natively for `tools/balance.c` and produce the same positions as the wasm cart, verified by a hash.
3. One file of feel constants (`juice.h`), one file of strings (`content.h`), one program table (`src/gen/rtr_gl.c`), one light vector (`l1201`), no shadow maps.
4. Conflicts between the judges' notes are settled as follows. Two notes asked for FREE PILE pickups in full and one asked to defer them: pickups are specified in full (sections 2.9, 4.9, 8.2) and live in work item 24 as the last sim feature, behind the same COMEBACK toggle as RED TAG, so they can slip to a point release without touching anything else. The A minor garage song becomes E minor so `DiatonicBelow` works unchanged. The screen-space burst the judges called `fx2d_burst` is the `fx2_burst` DO-OCRACY's front end is adding to hud.c; this cart uses that name and copies hud.c after that work lands (section 10.3). Rebuild is 30 seconds (the base) with the rematch menu auto-selecting REBUILD at 20 seconds (the graft): the two timers are different timers.
5. The brief's suggested file names map as: physics.c is `sim.c`, audio.cpp is `audio_band.cpp` plus `audio_pcm.c`.

## 1. Name, folder, fiction, pillars

Name: RED TAG RUMBLE. Marquee: RED TAG on one line, RUMBLE under it. Short name for code and files: rtr. Program table symbol `rtr_gl`, file `src/gen/rtr_gl.c`. Cart `build/red-tag-rumble.wasc`, manifest name "RED TAG RUMBLE". Save magic 'RTR1'.

Folder: `/Users/obsidian/Projects/heatsync/games/red-tag-rumble`, its own git repo, author `virgilvox <hackbuildvideo@gmail.com>`, set up exactly as the platform map section 5 lists (two submodules at the pinned commits, bellows, stb_truetype.h, fonts, brand paths, tools, build.sh, compile_flags.txt, package.json, .gitignore, LICENSE, web/vendor, .github). Do not copy qrcodegen or config.json.

Fiction, one paragraph, never more than one line of it on screen at a time: a red tag on a project in a hackerspace means fourteen days and then it is free for the taking or it is trash. Everything on the garage shelf is red tagged. You have twenty tags to claim parts with. Build a bot from other people's abandoned projects, drive it into another bot made of other people's abandoned projects, and whatever survives goes back on the shelf. Operations (OPS) is the house robot and it would like the aisle clear.

The budget is called TAGS in every string ("3 TAGS", "6 LEFT", "OVER BUDGET BY 2"). Budget segments are drawn as little tag shapes (a rectangle with a notched corner, `hud_tag`) in hazard amber, so the fiction and the mechanic are one picture.

Pillars:

1. Build is the strategy, drive is the skill, and both are legible from across the room. The garage is a bot on a turntable and three bars, never a table of numbers.
2. Instant previews. Cycling a part swaps the mesh that frame, animates the bars that frame, and plays the part's own sound.
3. Real tradeoffs. Every part that raises one bar lowers another or costs tags another row wanted. No part is strictly better.
4. Nobody waits. Simultaneous building, a shared timer, presets for a 20 second start, a RANDOM BOT button, and eliminated players always get a hazard to fire.
5. Every loser has a lever. RED TAG overdrive at low HP, dropped panels are HP for anyone, hazard seats, and OPS taxes the leader when the fight stalls.
6. Pi honest. Fixed 60 Hz sim with a smooth timescale, render at whatever, no shadow maps, particle and debris caps, one light budget, one state table, measured on the box.

## 2. Game design

### 2.1 Players, seats and colours

1 to 4 players on W3C gamepads. Pad 0 is also the RetroArch keyboard (arrows, Z is A, X is B, A is X, S is Y, Enter is Start, Q and W are L and R). All four pads are polled every frame; a seat is claimed on an A, B, X, Y or Start edge in the lobby or the garage; `connected` is never read (the RetroArch core reports only pad 0 connected). Solo play is always a 1v1 against the practice bot, which gets its own garage panel with a difficulty toggle. Empty seats in 3 and 4 player modes may be filled with AI from the lobby (FILL WITH BOT stepper, default OFF).

Seat colours in GANTRY order: seat 1 hazard `0xf2ab1e`, seat 2 live `0x5fb892`, seat 3 bone `0xece3d3`, seat 4 rust `0xc2542c`. Because amber is also the UI's fill colour and bone its text colour, a seat is always shown as a colour chip with its number ("1".."4" in `F_MONO_S` tape ink) and never by colour alone. AI seats use the seat colour with a char hatch over the chip and an AI glyph. OPS is char with an amber stripe.

### 2.2 Modes

| Mode | Players | Clock | Win | Notes |
|---|---|---|---|---|
| DEATHMATCH | 1 to 4 | FIGHT LENGTH | last bot with HP above 0 and not counted out; judges at time | the default; FFA with 3 or 4; the attract screen plays it |
| KO BY FLIP | 2 to 4 | FIGHT LENGTH | HP off; only count-out (inverted or immobile 10 s), pit, ring-out, deck count | flippers, lifters, hammers; a wedge still wins by pushing into the pit |
| LAST BOT STANDING | 3 to 4 | FIGHT LENGTH | DEATHMATCH FFA; eliminated players take the hazard seats | pads of eliminated players are rebound at the moment of KO |
| KING OF THE HILL | 2 to 4 | first to 30, cap 120 s | a 3 x 3 hill moves every 20 s; alone on it earns 1 per second; KO respawns in your corner after 5 s with 60 HP | treads and lifters shine |
| 2V2 | 4 | FIGHT LENGTH | team HP pool; both bots down ends it | teams amber plus bone vs live plus rust; friendly fire 50 percent; crusher allowed |
| TOURNAMENT | 3 to 4 | 3 matches | bracket: semis then a final; the losers fight third place first; eliminated players drive OPS in the final | 3 players: one bye, chosen by a visible coin flip |

Crusher is refused in any 1v1 by `build_legal(const build_t*, int players)` in code; the garage row shows it dimmed with NOT IN 1V1 as its flavour line. Modes are picked in the lobby from one row of six cards; nothing in the game scrolls.

### 2.3 Match flow and timings

```
LOBBY -> GARAGE (60 s shared) -> ARENA VOTE (8 s) -> COUNTDOWN (3 s)
      -> FIGHT (FIGHT LENGTH, default 75 s) -> KO (1.5 s at 0.2x, then 1.0 s orbit hold) or TIME + JUDGES (3 s)
      -> RESULTS (6 s) -> REMATCH MENU (majority vote, REBUILD auto at 20 s)
      -> REBUILD (30 s, skip when all locked) -> ARENA VOTE -> COUNTDOWN -> FIGHT ...
```

Frames at 60 Hz: garage 3600, vote 480, countdown 180, KO 90 render frames at timescale 0.2 (18 sim steps) then 60 render frames of orbit with the sim paused, results minimum 120 before any input counts, rematch menu auto at 1200, rebuild 1800. FIGHT LENGTH is a setting: 60, 75, 90 or 120 seconds, default 75, so READY plus fight plus KO plus results is under two minutes.

CLOSING TIME is the last 15 s of any timed fight: every hazard cycles at double rate (the hazard clock advances 2 x DT, so phases stay continuous), the wall rail lights go rust, the music switches to OVERTIME on the next beat, the clock chip turns rust and blinks at 2 Hz. At T minus 10 OPS leaves and the clock ticks SFX_COUNT each second. Nothing about it is random; players learn the tell.

Timeout: at 0 the clock flashes rust, stinger TIME, 1 s freeze, then the judges card (section 2.5).

### 2.4 Win conditions, checked in this order once per sim step after hazards

1. `hp <= 0`: KO, cause HP ZERO.
2. Centre more than 0.3 inside an open pit, beyond an open edge, or deck count expired: KO, cause PITTED, OFF THE ROOF or DECK.
3. `countout >= 600` frames: KO, cause COUNTED OUT.
4. In KO BY FLIP, check 1 is skipped (hp is tracked for the results card but never ends the fight).
5. Clock at 0: JUDGES.
6. KING OF THE HILL: `score >= 30` ends it at once.

A KO that leaves only AI or OPS bots active still ends the match (the humans lost). In a free-for-all a non-final elimination is a mini-KO (section 2.10); the slow-mo plays only on the KO that ends the match.

### 2.5 Judges decision (timeout only)

Three bars, 15 points: DAMAGE 7 (share of total damage dealt), AGGRESSION 4 (share of frames within 2.0 u of an enemy while closing at more than 0.5 u/s, plus 60 frames per taunt, capped at 4 taunts), CONTROL 4 (share of frames the enemy spent lifted, clamped, inverted by you, or in a hazard you pushed it into within the last 90 frames). Shares are rounded to one decimal and drawn as bars in seat colour.

Tie chain, in code and on the card: cheaper build wins (card line TIE. CHEAPER BOT WINS. THAT IS THE RULE. with the chip UNDERDOG RULE); if costs tie, fewer frames spent inverted wins (card line TIE. FEWER FLIPS WINS.); if still tied, DRAW (card line DRAW. NOBODY TELL THE TREASURER.).

### 2.6 Scoring across an evening

The evening tally is matches won, shown as pips on the lobby seat cards and on results. Per player on the results card: DEALT, BIGGEST, AIR (seconds airborne), FLIPS (times inverted), one award from the priority list in section 16. Nothing persists across cart restarts except `save_t` (section 14.4): settings, best evening, favourite build per seat, the hall of fame.

### 2.7 Count-out, flips, stuck

A bot is counting out when it is inverted and cannot drive, or has moved less than 0.2 u in the last 120 frames while not airborne, lifted or clamped. The count shows 10 to 0 in `F_TITLE` rust at that bot's viewport centre, with B TO RIGHT YOURSELF under it in `F_MONO` when the build can self-right, else GET MOVING; other viewports show a small rust number beside that bot's minimap dot. Any 10 consecutive frames over 0.5 u/s, or a successful self-right, or a landed hit, resets it. A bot that sits still on purpose is counted out too, which is the real rule and it makes wedges wiggle. A bot wedged against a wall with `|thr| > 0.5` and no displacement for 120 frames is nudged 0.05 u along the wall tangent per frame and the cart logs `[stuck]`; the drive scripts in section 15.5 must never produce that line.

### 2.8 RED TAG overdrive (comeback lever 1)

When a bot first drops to 30 HP or less and COMEBACK is on for this match: 8 s of overdrive. Weapon damage 1.4x, top speed 1.15x, boost cooldown halved, sparks go rust, a rust RED TAG strip appears on its HP bar, stinger RED TAGGED in its viewport, SFX_KLAXON tell variant, rumble 0.3 low 0.3 high 300 ms. Once per bot per match, automatic. One timer (`red_tag_t`) and one multiplier in the sim; the render reads the timer.

COMEBACK is a settings row: AUTO (default), ON, OFF. AUTO means ON in free-for-all and 2V2 and OFF in any 1v1 including solo. The same toggle governs pickups.

### 2.9 FREE PILE pickups (comeback lever 2)

With COMEBACK on, every armour panel that detaches (section 4.9) lies on the floor for 6 s as sim debris with `pickup = 1` and a teal ring. Any bot whose body overlaps it heals 6 HP (not above max), up to 3 pickups per bot per match; the panel vanishes with a teal plus-sign burst (8 teal cubes rising) and a +6 number in live teal, SFX_MENU_OK low variant, stinger FREE PILE the first time per bot. MELTY cannot pick up (the body shreds the panel: the debris is recycled with 6 sparks and no heal) and gets the stinger NOT A VACUUM once per match. Wheels are debris without the pickup flag and live 4 s. The AI scorer has a PICKUP goal (section 8.2).

### 2.10 Eliminations, hazard seats, mini-KO

In LAST BOT STANDING, DEATHMATCH FFA and 2V2 an eliminated player gets a hazard seat the frame it dies: the left stick picks a seat-drivable hazard in arena order (pulverizers, flame jets, the floor flipper), A fires it, the chosen hazard glows teal and its minimap glyph gets a teal arrow in the seat's colour. Cooldowns: pulverizer 72 frames, flame jet 2 s fire then 4 s refill, floor flipper 3 s. In a TOURNAMENT final the first eliminated player drives OPS (left stick) and the second runs its lifter (A) and saw (X); OPS gets 400 HP while driven, and the wake triggers in section 7 still decide when it may leave the zone. In 1v1 the loser watches the KO camera; the fight is over.

A non-final FFA elimination is a mini-KO: 0.6 s at timescale 0.4, no camera change, the KO band of the Hit Contract (shake 0.30 in the victim's and attacker's viewports only), the KO stinger in the victim's viewport, and that viewport then becomes the hazard seat view (section 10.19). The victim's name and cause line are drawn in the stinger slot for 1.5 s.

### 2.11 OPS LEADER TAX

OPS wakes on 720 frames without a landed hit, or when a bot has been pinned (within 1.0 of two walls, in contact with an enemy, speed under 0.3 for 240 frames). Its target: the pinner when there is a pin; otherwise the current HP leader in free-for-all and the HP leader in 1v1 too. It lifts the target and carries it 3 u toward the arena centre. Section 7 has the rest.

## 3. The garage (bot builder)

### 3.1 Layout and flow

One panel per seat in the same split layout as the fight (section 9.1), so nobody learns two screen regions. The 3D half of each panel is a real three.c viewport onto the shared garage scene: four turntables in a row at x = 0, 12, 24, 36 in a long char bay with a pegboard back wall; camera i looks at turntable i. The turntable rotates at 20 degrees per second; the right stick scrubs it and releasing resumes.

Five rows: CHASSIS, DRIVE, WEAPON, ARMOUR, PAINT. A sixth row, 2ND WEAPON, exists only while the WEAPON row is WEDGE or LIFTER, or the chassis is LUNCHBOX (top mount); the panel grows by one row and nothing scrolls. Up and Down pick a row, Left and Right cycle the option with wrap. Cycling applies that frame: the part mesh swaps, the three bars animate, the part's sound plays, the budget bar updates, the bot name updates. A locks the whole bot (hold 1.0 s the first time in an evening with a fill ring over the A keycap; instant afterwards); B releases the lock. Start cycles the seven presets. X is RANDOM BOT. Y holds the control card over your own panel. Right trigger test-fires the weapon on the turntable (the spinner spins up with its whine, the flipper fires, the hammer swings, the flame lights, the saw spins, the lifter raises, the crusher clamps, the spike pops), purely cosmetic, very moreish.

The shared 60 s timer is a 24 px caution tape strip across the bottom of the whole screen shrinking from both ends toward the centre, with the seconds in a char chip at the centre; at 10 s it flashes; at 0 every bot not yet locked locks as is. When all seats are locked the timer snaps to zero over 0.3 s and the arena vote starts.

Illegal state: if the total exceeds the budget, the budget bar shakes (2 px, 8 Hz, 0.3 s), the segments over budget draw in `C_FAULT`, A does nothing, and the flavour line is replaced by OVER BUDGET BY N in rust. Illegal options (HORIZONTAL on WEDGE chassis, CRUSHER in 1v1, any weapon on MELTY, a secondary where none is allowed) are drawn in smoke with a one-word reason in place of the flavour line (NOT ON WEDGE, NOT IN 1V1, BODY IS THE WEAPON, NO MOUNT) and Left and Right skip them. A build is playable if it has a chassis and a drive; weapon NONE and armour NONE are legal.

Hint line (rebuild only): one line in bone at the top of the panel from the last fight's record, picked by priority: flipped 3 or more times; lost to a spinner while on NONE or PLYWOOD; pushed into a hazard twice; counted out; spent under 14 tags; else a random tip. Strings in section 16.

Joining is allowed in the garage: an unseated pad pressing a face button gets a panel that slides in from its screen side over 0.3 s and the layout reflows with a 0.4 s caution tape cut.

### 3.2 Budget

20 tags. WALKER adds 4 (24), LEG TRACKS adds 2 (22). Overspend blocked, underspend gives nothing. A secondary weapon is allowed only with the WEDGE or LIFTER weapon (front mount) or on the LUNCHBOX chassis (top mount, any primary), must be SAW, FLAME or SPIKE, and costs its price minus 1.

The bar: 20 (or 22, 24) tag glyphs 14 px wide and 10 px tall, 2 px apart, hazard amber when spent, char with a 1 px steel frame when free, bonus tags (from WALKER or LEG TRACKS) framed in live teal. Segments 19 and 20 draw rust when 19 or 20 of 20 are spent. The current total and remainder are one string under the bar: "14 TAGS SPENT. 6 LEFT."

### 3.3 Chassis (choose one)

Length is along the bot's forward axis (+z local), width across (+x local), height to the top plate. Flip odds is the chance of landing inverted after a flipper launch or any landing with |vy| at impact above 5.0.

| id | Name | Tags | HP | Mass | L x W x H | Flip odds | Rule | Noun | Flavour (under 40) |
|---|---|---|---|---|---|---|---|---|---|
| CH_BOX | BOX | 2 | 100 | 1.0 | 1.0 x 0.8 x 0.40 | 50 | none | BOX | A PELICAN CASE WITH OPINIONS |
| CH_WEDGE | WEDGE | 3 | 90 | 1.0 | 1.1 x 0.8 x 0.30 | 45 | cannot mount HORIZONTAL; wedge front hop (section 4.7) | DOORSTOP | DOORSTOP, BUT ANGRY |
| CH_LOWBOY | LOWBOY | 3 | 85 | 0.9 | 1.0 x 0.9 x 0.25 | 30 | 0.8x flipper launch; ducks the laser sweep | PANCAKE | IT WAS A SERVER CHASSIS ONCE |
| CH_SHELL | SHELL | 4 | 110 | 1.1 | circle r 0.50 x 0.45 | 50 | zone 0.8 everywhere; 50 percent auto self-right on landing | BOWL | WOK. IT WAS A WOK |
| CH_FRAME | FRAME | 1 | 70 | 0.8 | 1.0 x 0.7 x 0.35 | 50 | parts fly off from 50 percent HP; top speed 1.1x | SKELETON | OPEN TUBE. VERY HONEST |
| CH_LUNCHBOX | LUNCHBOX | 2 | 100 | 1.0 | 0.9 x 0.7 x 0.70 | 70 | hammers hit it for 0.7x; only chassis with a top mounted secondary | LUNCHBOX | TALL, TIPPY, HOLDS SNACKS |

Chassis bar contributions: TOUGH gets `hp/110 * 0.5 + (1 - flip_odds/100) * 0.5`; SPEED gets `(1.1 - mass) * 0.3`; HIT gets nothing.

### 3.4 Drive (choose one)

| id | Name | Tags | Top u/s | Accel u/s2 | Turn deg/s | Traction | Mass | Knock mult | Boost | Quirk | Flavour (under 40) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| DR_TWO | TWO WHEEL | 2 | 6.0 | 12 | 300 | 0.60 | 0.20 | 1.0 | yes | a side or rear third hit cuts top speed 40 percent for 240 frames | SCOOTER PARTS. BOTH OF THEM |
| DR_FOUR | FOUR WHEEL | 3 | 5.0 | 8 | 220 | 0.80 | 0.30 | 1.0 | yes | loses a wheel at HP 30 (visual), keeps 80 percent speed | FOUR WHEELS, ZERO DRAMA |
| DR_SIX | SIX WHEEL | 4 | 4.2 | 8 | 180 | 0.95 | 0.40 | 0.8 | yes | climbs 0.4 high benches and the honeycomb lip; wide turn | SIX WHEELS, ONE AGENDA |
| DR_TREAD | TREADS | 5 | 3.8 | 5 | 160 | 1.00 | 0.60 | 0.5 | no | highest push; saws do 2x on tread zones | LOUD. SLOW. UNMOVABLE. LIKE A MEETING. |
| DR_HOVER | HOVER | 5 | 5.5 | 30 | 360 | 0.30 | 0.10 | 1.0 (1.5x flip distance) | yes | strafes; ignores every floor hazard; push 0; never inverted | LEAF BLOWER. DO NOT ASK |
| DR_WALKER | WALKER | 1 (+4 budget) | 3.0 | 5 | 120 | 0.70 | 0.35 | 0.9 | yes | steps over pit lips within 0.3; gait animation and stomp SFX | LEGS WERE A CHOICE |
| DR_LEGTRACK | LEG TRACKS | 3 (+2 budget) | 3.4 | 5 | 140 | 0.90 | 0.50 | 0.7 | no | climbs ramps and the deck at full speed; immune to screws | TANK WITH KNEES |
| DR_MELTY | MELTY BRAIN | 6 | 4.5 | 8 | n/a | 0.40 | 0.20 | 1.0 | yes | no weapon slot; every edge hits for 18 at spin over 0.5; 10 self damage on walls over 2 u/s; never inverted | THE WHOLE BOT IS THE WEAPON |

Push force: `push = traction * mass_total * accel`; HOVER push is 0. Knock mult scales incoming knockback impulses.

Steering (all but HOVER and MELTY): arcade car. Left stick Y is throttle (reverse at 80 percent), X is turn; turn rate falls to 60 percent of the table value at top speed. Tank scheme per seat: left Y and right Y are the two sides. HOVER and MELTY: the stick is a world direction of travel relative to the seat's camera forward (main.c writes `cam_yaw` into `pad_state_t`). MELTY heading intent is a blinking teal LED on the body.

Drive bar contributions: SPEED gets `top/6.0 * 0.6 + accel/30 * 0.2 + turn/360 * 0.2` (MELTY uses turn 360); TOUGH gets `traction * 0.3 + (1 - knock_mult) * 0.2`; HIT gets `push / push_treads * 0.2`.

### 3.5 Weapon (choose one, or NONE)

Hitboxes are in the attacker's local frame (+z forward, +x right). Spin up is seconds to full. Cooldown and stun are frames. Knock is the impulse base (section 4.7).

| id | Name | Tags | Base dmg | Spin up or wind up | Cooldown | Stun | Knock | Self dmg | Mass | Hitbox | Input | Adjective | Flavour (under 40) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| WP_NONE | NONE | 0 | 0 | | | | | | 0 | | | HUMBLE | LEAVE THE SLOT EMPTY. SAVE TAGS |
| WP_WEDGE | WEDGE | 1 | 10 to 20 by speed | none | none | 15 | 4.0 fwd | 0 | 0.05 | front plate, ram at closing speed over 2.0 | passive | POLITE | NO WEAPON. THAT IS THE WEAPON |
| WP_SPIKE | SPIKE | 3 | 22 | 9 frames extend | 60 | 18 | 2.5 fwd | 0 | 0.10 | box 0.25 x 0.5 at z 0.55 to 1.05, active 9 | tap | POINTY | A PUNCH IN A STRAIGHT LINE |
| WP_LIFTER | LIFTER | 3 | 4 | 36 frames to full | none | 18 while held | 1.2 | 0 | 0.10 | box 0.6 x 0.3 at z 0.5 to 0.8 | hold | HELPFUL | GETS UNDER, LIFTS, CARRIES |
| WP_SAW | SAW | 3 | 5 per tick every 10 frames | 0.8 s | none | 6 per tick | 0.4 | 2 per s in contact | 0.10 | circle r 0.3 at z 0.7 | tap toggles | GRINDY | NEEDS A MOMENT WITH YOU |
| WP_FLAME | FLAME | 4 | 3 per tick every 20 frames plus burn 2/s for 3 s | none | fuel 240 frames, refill 180 | 0 | 0 | 0 | 0.10 | cone length 2.0, half angle 30 degrees | hold | TOASTY | SCARY. MILD. GREAT ON CAMERA |
| WP_HAMMER | HAMMER | 5 | 28 | 15 frames wind up with a tell | 72 | 30 | 1.2 | 4 on a miss | 0.20 | box 0.4 x 0.5 at z 0.5 to 1.0, active 4 | tap | THUMPY | TELEGRAPHS. THEN DENTS |
| WP_FLIPPER | FLIPPER | 6 | 6 | instant | 108 | 36 airborne | launch vy 7.0, vx 3.0 fwd | 0 | 0.20 | box 0.6 x 0.5 at z 0.4 to 0.9, active 4 | tap | YEETING | GRAVITY, BUT FOR THEM |
| WP_DRUM | DRUM | 6 | 24 at full spin | 1.5 s | none | 24 | 4.0 up and fwd (pop vy 3.0) | 5 per hit | 0.30 | circle r 0.3 at z 0.55 | hold | DRUMMY | POPS THEM UP. HITS THE FLOOR TO SELF RIGHT |
| WP_HORIZ | HORIZONTAL | 7 | 30 at full spin | 2.0 s | none | 21 | 6.0 lateral | 8 per hit, 15 on a wall | 0.30 | circle r 0.55 at z 0.15 | hold | SPINNY | CHAINSAW YOU CANNOT STEER |
| WP_CRUSHER | CRUSHER | 7 | 18 on close then 6/s held | 24 frames to clamp | 120 | lock 120 | 0 | 0 | 0.30 | box 0.5 x 0.4 at z 0.5 to 0.9 | tap | CLINGY | HOLDS ON. TWO SECONDS. AWKWARD |

MELTY BRAIN has no weapon row (the row reads BODY) and its adjective is DIZZY.

Spinner behaviour: spin rises at `1/spin_up` per second while held, falls at half that rate when released, clamped 0..1. Hits need spin over 0.3 and scale by spin. HORIZONTAL at spin over 0.6 reduces turn rate 30 percent. HORIZONTAL at spin over 0.5 hitting a wall drops spin to 0, costs 15 HP and makes 30 sparks. DRUM pops the victim up (vy 3.0) which exposes the underside (zone 1.5) to the next hit within 30 frames. Hitting a HORIZONTAL or DRUM weapon circle while its spin is under 0.3 does 2.0x.

Inverted behaviour: WEDGE, NONE, DRUM, SAW, SPIKE drive inverted at 70 percent. HORIZONTAL inverted spins but cannot steer. HAMMER and FLIPPER cannot operate inverted and self-right on B (90 frames, no drive, zone 1.5 while animating, always succeeds). DRUM self-rights on A while inverted at spin over 0.5 (45 frames, 80 percent, costs all spin). LIFTER, CRUSHER, FLAME cannot operate inverted and cannot self-right; a SHELL still rolls upright 50 percent of the time on landing; HOVER and MELTY are never inverted.

Weapon bar contributions: HIT gets `base/30 * 0.6 + knock/6 * 0.2 + (spin_up == 0 ? 0.2 : 0.2 - spin_up/10)`; SPEED loses `mass * 0.3`; TOUGH loses 0.15 if the weapon cannot operate inverted and cannot self-right. SAW counts base 12 and FLAME base 10 for the formula.

### 3.6 Armour (choose one)

| id | Name | Tags | Reduction | Mass | Extra | Spark colour | Flavour (under 40) |
|---|---|---|---|---|---|---|---|
| AR_NONE | NONE | 0 | 0 percent | 0 | parts fly from 50 percent HP | amber dust | ALL FRAME, ALL FAITH |
| AR_PLY | PLYWOOD | 1 | 10 percent | 0.10 | body darkens 20 percent per burn | splinters, bone | OFFCUT BIN BY THE LASER |
| AR_ALU | ALUMINIUM | 2 | 20 percent | 0.20 | bright dents | white | BRIGHT DENTS. VERY VISIBLE |
| AR_UHMW | UHMW | 3 | 15 percent | 0.20 | 0.5x knockback, burn immune | none, white scrapes | CUTTING BOARD. DO NOT TELL KITCHEN |
| AR_STEEL | STEEL | 4 | 35 percent | 0.50 | deep dents | amber, rust at half life | HEAVY. YOU WILL FEEL IT TURNING |
| AR_TI | TITANIUM | 5 | 30 percent | 0.30 | | cycles bone, live, amber per particle | SOMEONE DONATED THIS. REALLY |
| AR_WEAR | WEAR PLATE | 6 | 45 percent | 0.60 | spinner hits on it stun the attacker 9 extra frames; nearly no dents | rust, few | THE SHELF SAGGED UNDER IT |

Armour bar contributions: TOUGH gets `reduction/45 * 0.5`; SPEED loses `mass * 0.4`.

### 3.7 Paint

Four schemes per seat, each a body colour plus a stripe colour, all GANTRY safe: seat colour body with char stripe; char body with seat stripe; bone body with seat stripe; steel body with hazard stripe. A second press on the PAINT row cycles the decal set: none, hazard stripes, caution tape band, the seat number in Archivo. The seat colour band around the chassis (0.08 u tall) never changes. Paint costs nothing. Never the HeatSync mark on a bot.

### 3.8 The three bars

SPEED, HIT, TOUGH, each 0 to 1, 140 px wide and 10 px tall with a 1 px steel frame, a word in `F_MONO_S` over each. The value is the clamped sum of the contributions in 3.3 to 3.6. When an option is cycled the bar animates from old to new over 18 frames with `hud_ease_out`; the delta segment is drawn in live teal (gain) or rust (loss) and held 72 frames after the change, then settles to bone. The player never sees the tables; the bars are the whole stat system.

### 3.9 Presets (Start cycles these in order; the seventh is RANDOM)

| Preset | Chassis | Drive | Weapon | Armour | Tags | Bot name | Blurb (under 40) |
|---|---|---|---|---|---|---|---|
| ROLL OUT | WEDGE | TREADS | WEDGE | STEEL | 13 of 20 | POLITE DOORSTOP | CANNOT BE PUSHED. BORING. WINS. |
| LAUNCH | BOX | TWO WHEEL | FLIPPER | ALUMINIUM | 12 | YEETING BOX | THE ARCADE CLASSIC |
| CHOP | LOWBOY | FOUR WHEEL | HORIZONTAL | ALUMINIUM | 15 | SPINNY PANCAKE | GLASS CANNON |
| STOMP | LUNCHBOX | WALKER | HAMMER | STEEL | 12 of 24 | THUMPY LUNCHBOX | LEGS AND A GRUDGE |
| TOAST | SHELL | HOVER | FLAME | UHMW | 16 | TOASTY BOWL | THE TROLL BUILD |
| SPIN | LOWBOY | MELTY BRAIN | none | TITANIUM | 14 | DIZZY PANCAKE | I HAVE DONE THIS BEFORE |
| RANDOM | random legal | | | | | | SHELF ROULETTE |

Seat i starts the first garage of an evening on preset i (seat 1 ROLL OUT, seat 2 LAUNCH, seat 3 CHOP, seat 4 STOMP); the practice bot starts on the difficulty's pick (section 8.4). The preset name is shown as a label under the bot name on the panel and nowhere else; the bot name is what the rest of the game uses.

### 3.10 RANDOM BOT (X)

Pick chassis, drive, weapon and armour uniformly from the legal sets for this player count, then while over budget downgrade the most expensive row one step toward cheaper until legal (never below NONE for weapon and armour); if under 17 tags, upgrade armour then drive one step while it fits. The roll uses the match RNG so the test rig is deterministic. Animate as a 36 frame slot machine: each row cycles visibly for 6 frames in sequence top to bottom with SFX_MENU_MOVE per tick and SFX_LOCK at the end. Paint is randomised too.

### 3.11 Bot names

`bot_name(char out[24], const build_t*)` writes `"%s %s"` from `WEAPON_ADJ[weapon]` (HUMBLE, POLITE, POINTY, HELPFUL, GRINDY, TOASTY, THUMPY, YEETING, DRUMMY, SPINNY, CLINGY; DIZZY when the drive is MELTY BRAIN) and `CHASSIS_NOUN[chassis]` (BOX, DOORSTOP, PANCAKE, BOWL, SKELETON, LUNCHBOX). The longest is YEETING LUNCHBOX at 16 characters; the buffer is 24 and the drawn maximum is 20. The name is shown on the garage panel, the lobby card, the fight strip, the KO card, the results rows and the hall of fame.

### 3.12 Garage AI panel

In solo play the second panel is the practice bot: same rows, driven by the AI (it visibly cycles parts for the first 5 s then locks). A three-way toggle at its top, EASY MEDIUM HARD, moved by the human's Right stick while their cursor is on PAINT, or by any unseated pad. MEDIUM counter-picks the human's last build (section 8.4). With FILL WITH BOT on in a 3 or 4 player lobby the AI panels behave the same.
## 4. Simulation (sim.c; no three.c, no HUD, no host calls)

### 4.1 Units, time, timescale and interpolation

One unit is one bot length (about half a metre). Fixed step `DT = 1.0f/60`. The sim never reads wall time. `main.c` keeps `G.timescale` (1.0 in play, 0.2 in the KO slow-mo, 0.4 in a mini-KO, 0 while paused) and an accumulator:

```c
acc += clamp(delta_ms / 1000, 0, 0.05) * G.timescale;
steps = 0;
while (acc >= DT && steps < 4) { feed_inputs(&M); sim_step(&M); acc -= DT; steps++; }
if (steps == 4) acc = 0;                      /* a stall drops time, never fast-forwards */
alpha = acc / DT;                             /* render interpolates prev..cur by alpha */
```

`sim_step` copies every body's `x, z, h, y` into `px, pz, ph, py` at its start, so `world_sync` always has a valid previous state and 0.2x is smooth, not stepped. Hit stop is a sim counter: while `M.hitstop > 0`, `sim_step` decrements it and returns without integrating; main.c still renders, shakes, plays audio and advances particles (section 5.3).

### 4.2 Determinism

All randomness goes through one xorshift32 in `match_t`, seeded from `wc_set_seed` xor the match index. No `sinf`, `cosf`, `atan2f` from libm inside sim.c, bot.c, arena.c or ai.c: they use `rtr_sin`, `rtr_cos`, `rtr_atan2` from a 1024-entry table in sim.c (linear interpolation between entries, octant reduction for atan2), so the native balance build and the wasm cart compute identical floats. `sqrtf` is exact under IEEE and allowed. Every pairwise loop runs in fixed order (i < j). The cart logs `[sim] f=%d h=%08x` every 60 sim frames where `h = sim_hash(&M)` (FNV-1a over the raw bits of `x, z, h, y, hp` of every bot and `x, z` of every live debris); `build/balance --replay` on the same seed and inputs must print the same lines. `wc_info.flags` carries `WC_FLAG_DETERMINISTIC`.

### 4.3 Data

```c
#define MAX_BOTS   5           /* 4 seats plus OPS */
#define MAX_DEBRIS 24
#define MAX_EVENTS 128
#define MAX_HAZ    12

typedef struct { int16_t lx, ly, rx, ry; uint16_t buttons; uint8_t lt, rt; float cam_yaw; } pad_state_t;

typedef struct {
  float x, z, h;            /* plane position, heading radians (0 is +z) */
  float vx, vz, w;          /* velocity, angular velocity */
  float y, vy;              /* height channel; y > 0 is airborne */
  float px, pz, ph, py;     /* previous step, for render interpolation */
  float mass, inv_mass, inertia, inv_inertia;
  float hl, hw;             /* half length (local z), half width (local x); hl is the radius for SH_CIRCLE */
  uint8_t shape;            /* SH_BOX or SH_CIRCLE */
} body_t;

typedef struct {
  body_t b;
  uint8_t seat, alive, is_ai, is_ops, team;
  build_t build; char name[24]; uint8_t preset;        /* preset 0..6, 7 = custom */
  float hp, hp_max, burn_t, burn_dps;
  uint8_t inverted, lifted_by, clamped_by, on_deck, in_pit_edge, flip_pending;
  int16_t stun, brake, boost, boost_cd, taunt, selfright, countout, wheel_hit, lock, deck_count;
  float spin, charge, fuel, lift; int16_t cooldown, active, windup, clamp;
  float spin2, charge2, fuel2; int16_t cooldown2, active2;   /* secondary weapon */
  int16_t hit_cd[MAX_BOTS];                 /* per victim pair cooldown */
  uint8_t panel_state[6];                   /* 0 clean 1 2 3 dented, 4 gone */
  int16_t red_tag_t; uint8_t red_tag_used, pickups, not_vacuum_said;
  float dealt, biggest, airtime, aggro, control; int16_t frames_inverted; uint8_t flips, taunts, hazard_hits, wall_hits, clashes;
  pad_state_t in;
} bot_t;

typedef struct { float x, z, y, vx, vz, vy, spin; int16_t life; uint8_t kind, colour, pickup, owner; } debris_t;

typedef enum { EV_HIT, EV_CLASH, EV_WALL, EV_RAM, EV_LAUNCH, EV_LAND, EV_INVERT, EV_SELFRIGHT, EV_SHED, EV_BURST,
               EV_KO, EV_MINI_KO, EV_COUNT_TICK, EV_HAZ_TELL, EV_HAZ_HIT, EV_OPS_WAKE, EV_OPS_LEAVE, EV_OPS_LINE,
               EV_WEAPON_TELL, EV_TAUNT, EV_BOOST, EV_HILL_MOVE, EV_RED_TAG, EV_PICKUP, EV_NOT_VACUUM,
               EV_BENCH_BREAK, EV_BUMPER_BREAK, EV_SPIN_WALL, EV_CLOSING } ev_type_t;
typedef struct { uint8_t type, a, b, zone; float x, z, y, dmg, nx, nz; } event_t;

typedef struct { uint8_t type; float phase_t; uint8_t active, tell, broken; int16_t hp, cooldown; uint8_t seat; } hazard_state_t;

typedef struct {
  uint32_t rng; int frame; int16_t hitstop; uint8_t mode, arena, players, comeback, closing;
  float clock_s, haz_clock;                  /* clock counts down; haz_clock counts up at 1x or 2x */
  bot_t bots[MAX_BOTS]; int nbots;
  debris_t debris[MAX_DEBRIS]; int ndebris;
  event_t ev[MAX_EVENTS]; int nev;           /* cleared by the caller after each step */
  hazard_state_t haz[MAX_HAZ];
  uint8_t winner, win_how, over; int hill; int score[4];
  uint8_t ops_state; int16_t ops_t; uint8_t ops_target; int16_t no_hit_frames;
  uint32_t seed; int ko_frame;
} match_t;

typedef struct { uint8_t mode, arena, players, comeback, fight_len_s; build_t builds[4]; uint8_t is_ai[4], ai_level[4], team[4]; uint32_t seed; } match_setup_t;
```

API, the whole surface: `void sim_init(match_t*, const match_setup_t*)`, `void sim_step(match_t*)`, `void sim_snapshot(const match_t *src, match_t *dst)` (a memcpy), `int sim_over(const match_t*)`, `uint32_t sim_hash(const match_t*)`. Inputs are written into `bots[i].in` before the step by main.c (pads) or ai.c (AI, also before the step). `sizeof(match_t)` is a few KB.

### 4.4 Step order

1. If `hitstop > 0`: `hitstop--`, return.
2. Store prev transforms; `frame++`; `clock_s -= DT`; `haz_clock += closing ? 2*DT : DT`; set `closing` when `clock_s <= 15`.
3. Timers: stun, cooldowns, boost, brake, taunt, selfright, burn, hit_cd, red_tag_t, wheel_hit, lock, deck_count, debris life, no_hit_frames.
4. Hazard timeline (section 6.2): states, tells, seat-fired hazards.
5. Drive (4.5): skip if airborne, lifted, clamped, stunned, braking, locked or dead; inverted scaling per weapon table.
6. Weapons (4.6): spin, charge, fuel, lift, cooldowns; this frame's hitboxes.
7. Integrate: `v += F/m * DT`; drag `v *= 0.90` per frame on the plane (0.985 for HOVER); clamp speed to top (times boost 1.5, times red tag 1.15); `x += v * DT`; `h += w * DT`; `vy -= 20 * DT; y += vy * DT`; landing when y crosses floor_y downward (4.8).
8. Walls and solids: OBB or circle against the wall list, benches, deck edges, bumpers, HVAC boxes, mound boxes (4.5), two iterations. Wall events.
9. Bot vs bot collision and push (4.5), two iterations.
10. Spinner clash check, then weapon hitboxes vs bodies (4.7), ram hits, lifter and crusher holds.
11. Hazards on bots: damage, launches, carries, pit and edge falls.
12. Pit, ring-out and deck checks; count-out; HP thresholds (dents, shed, burst, RED TAG); pickups; debris integration.
13. OPS wake and leave logic (section 7); AI thinks are run by the caller, not here.
14. Win conditions (2.4); judges tallies.

### 4.5 Drive model and collision

Arcade steering: throttle `t` in [-0.8, 1] and turn `s` in [-1, 1] after a 0.18 radial dead zone. `F = t * accel * mass` along heading; `w_target = s * turn_rad * (1 - 0.4 * speed/top) * (horiz_spin > 0.6 ? 0.7 : 1)`; `w` moves toward `w_target` at 20 rad/s2. Lateral velocity bleeds by `v_lat *= 1 - (4 + 8 * traction) * DT`. HOVER and MELTY: desired world velocity `d = stick rotated by in.cam_yaw`; `F = (d * top - v) * accel * mass / top`. MELTY heading spins visually at `spin * 30` rad/s in render and is not used for collision; its box is a circle of r 0.5 while spin > 0.5. Boost (L or LT edge, not on TREADS or LEG TRACKS): `boost = 48`, `boost_cd = 240` (120 while red tagged). Brake (B edge upright): `v *= 0.2` once, `brake = 18`. Taunt (X edge): `taunt = 30`, body yaws plus and minus 15 degrees at 6 Hz, `taunts++`, `aggro += 60`.

Shapes: boxes are OBBs from (x, z, h, hl, hw); SHELL and spinning MELTY are circles. Walls are axis-aligned segments with an inward normal; benches, deck edges, bumpers, HVAC units and mound boxes are AABBs with a height (a body collides with a solid only when the solid's top is above the body's y plus 0.1, so a bot on the deck does not hit the deck edge and a launched bot can clear a bench). Tests: circle vs circle, OBB vs circle (closest point in the box frame), OBB vs AABB and OBB vs OBB (SAT on the four face axes; normal is the least-penetration axis; contact point is the midpoint of the overlapping corner projections). Resolution: separate by penetration times 0.8 split by inverse mass (walls, solids and OPS have inv_mass 0); relative normal velocity `vn`; if `vn < 0`, impulse `j = -(1 + e) * vn / (inv_mass_a + inv_mass_b)` with e 0.2 for bots and 0.3 for walls, plus a tangential friction impulse clamped to `0.4 * j`; angular effect `w += cross(offset, impulse) * inv_inertia * 0.5`. Inertia is `mass * (hl*hl + hw*hw) / 3` for a box and `mass * r*r / 2` for a circle.

Push: when both bots drive into the contact (each forward force dotted with the normal toward the other is positive), `net = (push_a - push_b)` is applied along the normal as an extra force to both for the frame. A bot pressed into a corner cannot be moved further; after 240 frames of that the pin flag fires and OPS wakes.

Wall hit event when |vn| > 2.0 against a wall or solid: HORIZONTAL at spin over 0.5 loses all spin and 15 HP (EV_SPIN_WALL, 30 sparks); DRUM loses half its spin; MELTY at spin over 0.5 loses 10 HP; everyone else gets EV_WALL scaled by |vn| and `wall_hits++`. A bench or roof bumper takes the impulse magnitude as damage against its hp and breaks at 0 (EV_BENCH_BREAK or EV_BUMPER_BREAK with 6 debris).

### 4.6 Weapon state machine (per bot, per frame)

Inputs: weapon button `wb` is A or RT over 0.5; edge `e_w`.

- HORIZONTAL, DRUM: `spin += (wb ? 1 : -0.5) * DT / spin_up`, clamped 0..1. Hitbox exists when spin > 0.3: a circle at the table offset.
- SAW: `e_w` toggles `saw_on`; spin follows with spin_up 0.8; hitbox when spin > 0.5; a tick every 10 frames of contact.
- FLIPPER: `e_w` and cooldown 0 and not inverted: `active = 4`, `cooldown = 108`. Hitbox while active.
- HAMMER: `e_w` and cooldown 0 and not inverted: `windup = 15` (EV_WEAPON_TELL: arm raise, SFX), then `active = 4`, then `cooldown = 72`. A miss (active ends with no hit) costs 4 HP and makes 10 sparks at the head.
- SPIKE: `e_w` and cooldown 0: `active = 9`, `cooldown = 60`.
- LIFTER: `lift` moves toward `wb ? 1 : 0` at 1/36 per frame. While lift > 0.3 and the front box overlaps a victim that is not airborne and is facing away or sideways (within 90 degrees): `victim.lifted_by = me`; the victim's position is slaved to my front point at y 0.3 each frame; its drive is 0; its zone is 1.5 for everyone; my top speed is 0.6x; `control++`. Release on lift < 0.3, my stun, or after 180 frames. The victim escapes if its push exceeds 1.3x mine.
- FLAME: while `wb` and fuel > 0: `fuel -= 1`; cone hitbox; every 20 frames of overlap: 3 dmg plus `burn_t = 180, burn_dps = 2` (not on UHMW). Not firing: `fuel += 240/180` per frame to 240.
- CRUSHER: `e_w` and cooldown 0: `clamp = 24`; at clamp end, if a victim is in the front box: 18 dmg, `victim.clamped_by = me`, `lock = 120` on both (no drive either side), 6 dmg per second to the victim, `control++`; then release, `cooldown = 120`.
- WEDGE and NONE: passive; ram test in 4.7.
- MELTY: spin follows `wb` like a spinner with spin_up 1.5; the body is the hitbox at spin over 0.5.

A secondary weapon runs the same machine on its own state (`spin2, charge2, fuel2, cooldown2, active2`) from the same button, so WEDGE plus SAW just saws when touching.

### 4.7 Hits, damage, knockback

For each attacker hitbox vs each other alive body with `hit_cd[victim] == 0` (18 frames for spinners and DRUM, 10 for saw ticks, 20 for flame ticks, otherwise the active window ends the pair):

```
closing = clamp(|relative velocity of the weapon tip toward the victim| / 4.0, 0.4, 1.6)
          (spinner tip speed is spin * 12 for HORIZONTAL, spin * 9 for DRUM, plus the closing speed)
zone    = 1.0 side or rear; 0.6 front plate (contact in victim local z > 0.6*hl and |x| < hw); 0.5 on a WEDGE chassis front
          1.5 if victim.lifted_by or victim.y > 0.15 or (victim.inverted and attacker weapon is HAMMER, DRUM or SPIKE)
          2.0 if the victim has HORIZONTAL or DRUM at spin < 0.3 and the contact is within its weapon circle
          times 0.8 for SHELL; times 0.7 for HAMMER on LUNCHBOX
dmg     = base * spin_or_1 * closing * zone * (1 - armour_reduction) * (attacker.red_tag_t > 0 ? 1.4 : 1) * (teammate ? 0.5 : 1)
```

Apply: `victim.hp -= dmg`; `attacker.dealt += dmg`; `biggest`; `victim.stun = weapon stun` (plus 9 on the attacker when the victim wears WEAR PLATE and the weapon is a spinner); knockback impulse `J = knock * (0.5 + dmg/30)` along the hit normal (lateral for HORIZONTAL: the normal rotated 90 degrees in the spin direction; up and forward for DRUM with vy 3.0), `v += J * inv_mass * drive_knock_mult * (UHMW ? 0.5 : 1)`; attacker self damage per table; the panel on the hit zone advances one dent state on any hit over 20; `no_hit_frames = 0`. Event EV_HIT with position, normal, dmg, attacker, victim, zone; main.c looks the band up (section 5). Damage is applied as float and rounded for display.

Ram (WEDGE weapon, WEDGE chassis, or NONE): on a bot vs bot contact where the attacker's forward is within 30 degrees of the contact normal and closing speed > 2.0: `dmg = 10 + 10 * clamp((closing - 2) / 4, 0, 1)`, stun 15, knock 4.0 forward, `hit_cd` 30, EV_RAM. If the attacker has a WEDGE chassis or WEDGE weapon and the contact is on the victim's front or side, the victim gets vy 1.5 (a hop, airborne about 9 frames) which exposes its underside and disables its drive briefly. That hop is why wedges win pushing fights.

Spinner clash: two bots with HORIZONTAL or DRUM at spin > 0.5 whose weapon circles overlap: both take 0.6 of the hit the other would deal, both spin 0, both stun 30, `clashes++`, EV_CLASH at the midpoint. Checked before ordinary hits.

Flipper launch: `victim.vy = 7.0 * (LOWBOY ? 0.8 : 1)`, horizontal 3.0 along the attacker forward (1.5x for HOVER), `flip_pending = 1`, EV_LAUNCH (stinger AIRBORNE in both viewports).

### 4.8 Height channel, landing, inversion

`floor_y` is 0, or 1.0 on the deck, or the mound height in THE FREE PILE (a radial function). On landing: if `flip_pending` or |vy| > 5, roll `rng % 100 < flip_odds` to set inverted (MELTY and HOVER never; SHELL then rolls 50 percent to un-invert with a wobble event). Landing damage: 6 after a flipper, 8 from the deck or mound or any peak y over 1.2, else 0; landing on another bot deals 8 to it and pops the lander back 2 u/s. `vy = 0; v *= 0.6`; EV_LAND. Inverted bots: drive per the weapon table; render rotates the body 180 degrees about local z; zone rules in 4.7; countout runs; `frames_inverted++`. Self-right: B (HAMMER, FLIPPER) sets `selfright = 90`; A (DRUM at spin > 0.5) sets `selfright = 45` and spin 0 with an 80 percent roll at the end; on success `inverted = 0`, vy 1.0 for the visual hop, EV_SELFRIGHT.

### 4.9 Damage visuals, parts falling off, pickups

`panel_state[6]` for FRONT, BACK, LEFT, RIGHT, TOP, WEAPON MOUNT: at HP 70 all panels go to state 1, at 50 state 2, at 30 state 3 (render swaps geometry). At HP 20 (50 for FRAME, and every 10 HP below that for AR_NONE) one random panel with state < 4 becomes state 4 and spawns a debris body with the bot's velocity plus a random 2 u/s, vy 3, spin 6 rad/s, `life 360`, `pickup = comeback && kind == panel`, EV_SHED. At 0 HP all remaining panels and the wheels detach (up to 8 debris, wheels `life 240`, no pickup), the chassis frame stays as a static 0.5 mass prop that collides and can be shoved into a pit for a laugh, EV_BURST then EV_KO. Debris integrate with gravity 20, bounce once at e 0.3, slide with drag 0.9 per frame, fade over the last 60 frames; over the cap the oldest is recycled. A pickup is taken when a live bot's body overlaps the debris within 0.4 u (section 2.9).

RED TAG: when `hp <= 30` and `!red_tag_used` and `comeback`: `red_tag_t = 480`, `red_tag_used = 1`, EV_RED_TAG.

### 4.10 Snapshots and replay

Every 120 frames main.c copies `match_t` into a two-slot ring. Pad inputs per frame are recorded into a ring of 5400 entries of `pad_state_t[4]`. The KO replay re-simulates from the older snapshot with the recorded inputs for up to 240 frames and shows it from the KO camera with REPLAY in rust and a caution tape frame, skippable with A. Ship it only if it costs nothing; the snapshot ring also powers the attract loop restart. The input ring is also what `balance --replay` consumes (section 15.7).

## 5. The Hit Contract (juice.h)

Every entry below is fired by main.c from the event queue, never by the sim, so the headless build has none of it and the cart has all of it. Every constant lives in `src/juice.h` so feel is tuned in one file.

### 5.1 Severity bands

Damage is after armour. Shake is amplitude in units; punch is units along the hit normal; flash is the world size of the impact quad; rumble is low, high and ms on the victim's pad (the attacker's pad gets high only, half the ms).

| Band | Damage | Hit stop | Punch | Shake | Sparks | Flash | Extra | Number font | Rumble (low, high, ms) | SFX | Stinger |
|---|---|---|---|---|---|---|---|---|---|---|---|
| tap | 1 to 7 | 0 | 0.04 | 0.04 | 6 | none | | F_MONO_L | 0.2, 0.0, 60 | SFX_HIT_LIGHT | none |
| hit | 8 to 14 | 0 | 0.06 | 0.08 | 12 | 0.3 u | | F_MONO_L | 0.4, 0.2, 80 | SFX_HIT_LIGHT | none |
| heavy | 15 to 29 | 3 | 0.10 | 0.14 | 20 | 0.5 u | | F_H2 | 0.8, 0.4, 120 | SFX_HIT_HEAVY | BIG HIT at 25 and up |
| huge | 30 and up | 6 | 0.12 | 0.22 | 32 | 0.7 u plus viewport white at alpha 40 for 1 frame | 6 debris chips, life 120 | F_H2 | 1.0, 0.6, 160 | SFX_HIT_HEAVY plus crowd 0.8 | BIG HIT |
| clash | spinner on spinner | 8 | 0.12 both | 0.26 both | 40 at the midpoint, white | 0.9 u plus viewport white alpha 60 | 8 chips | CLASH text instead of a number | 1.0, 1.0, 200 both pads | SFX_CLASH plus crowd 1.0 | CLASH |
| KO | final HP 0 or hazard KO | 6 then slow-mo | 0.12 | 0.30 all viewports | 40 plus the full burst | viewport white alpha 60 | all panels and wheels | | 1.0, 1.0, 400 both pads | SFX_KO plus crowd 1.0 for 2 s | KO card |

Rules: multiple hits in one frame take the maximum hit stop; shake amplitudes add and clamp at 0.34; spark counts add and clamp at the global 400; one SFX per band per frame. HIT STOP OFF in settings zeroes the stop column and nothing else. SHAKE 0 to 3 scales amplitudes by 0, 0.5, 1.0, 1.5. Shake is 28 Hz value noise in camera right and up, decaying by `exp(-t / 0.12)`, added after chase smoothing so it never feeds back into the lag. Punch recovers with `1 - (1 - t)^3` over 0.30 s in the camera's own frame. In the shared ONE CAMERA view punch is skipped and shake takes the larger band.

### 5.2 Everything else that moves

| Moment | Sim | Camera | Particles | HUD | Audio and rumble |
|---|---|---|---|---|---|
| flipper launch | vy 7 | victim lag rises to 0.2 s while airborne | 6 dust puffs at the launch point | stinger AIRBORNE both viewports | SFX_LAUNCH, victim 0.3 low 0.7 high 80 ms |
| landing | landing damage | shake 0.06 for 12 frames on the victim | 10 dust puffs | number if damage; stinger INVERTED if inverted | SFX_HIT_LIGHT low variant |
| inverted | countout starts | none | none | count numeral at 7 | SFX_KLAXON short, a hat tick each second |
| wall hit over 2 u/s | 2 HP over 4 u/s | shake 0.04 for 12 own viewport | 8 sparks | number in rust if damage | SFX_HIT_LIGHT low |
| spinner wall hit | spin 0, 15 dmg | shake 0.10 for 20 own viewport | 30 sparks along the wall | number in rust | SFX_CLASH at 0.6 |
| hazard tell | none | none | the hazard's emissive pulses at 2 Hz for 1.5 s | minimap glyph blinks amber; a 24 px tape strip at the top of any viewport whose bot is in the zone | SFX_KLAXON tell variant once |
| hazard hit | per table | shake 0.08 for 20 | 20 sparks (killsaw), 12 flame quads (flame), 16 dust (pulverizer) | number; stinger PIT, DECK or DUCK where it applies | SFX_HIT_HEAVY, 0.6 low 0.6 high 100 ms |
| OPS inbound | OPS wakes | every viewport shake 0.06 for 20 | none | full screen stinger OPS INBOUND 0.5 s, minimap flash, speech panel 2 s | SFX_HORN low variant, 0.5 low 200 ms all pads |
| HP 50 | smoke emitter starts | | grey smoke quads 4 per second, life 72, rising 0.8 u/s, growing 0.2 to 0.6 u | HP bar goes amber | none |
| HP 30 | smoke black, fire joins | | smoke 6 per second char; fire 8 per second amber to rust cubes 0.12 u rising 1.5 u/s life 24 | HP bar goes rust, the seat chip pulses | none |
| HP 20 | a panel detaches | | debris body plus 8 sparks | | SFX_HIT_LIGHT |
| RED TAG | red_tag_t 480 | shake 0.04 for 18 own viewport | 20 rust sparks | rust RED TAG strip on the HP bar, stinger RED TAGGED | SFX_KLAXON tell variant, 0.3 low 0.3 high 300 ms |
| pickup | hp += 6 | none | 8 teal cubes rising | +6 in live teal, stinger FREE PILE once | SFX_MENU_OK low |
| KO | burst, timescale 0.2 for 90 render frames then hold 60 | KO orbit camera 30 deg/s | all panels and wheels, 40 sparks, smoke 12 per second | KO card, vignette tint, strips fade to alpha 120 | SFX_KO gong, music drops to the drums stem, both pads 1.0 for 400 ms |
| mini-KO | timescale 0.4 for 36 render frames | none | burst | KO stinger plus name and cause in the victim's viewport | SFX_KO, victim pad 1.0 for 300 ms |
| countdown | | cams already split | | 3 2 1 in F_TITLE with a 1.08x pop over 0.15 s, FIGHT in F_STINGER | SFX_COUNT x3 then SFX_HORN |
| closing time | hazards 2x | | wall rail emissive rust | clock rust, 2 Hz | OVERTIME on the next beat |
| taunt | 30 frames wiggle | none | 4 confetti squares in the seat colour | +1 aggression tick on the strip | SFX_MENU_OK low variant |
| lock in | | turntable stops, bot faces camera | 12 confetti over the turntable | panel live frame | SFX_LOCK |
| part cycle | | part swaps that frame | 4 dust puffs at the swapped part | bars animate 18 frames | SFX_MENU_MOVE plus the part's own sound |
| menu move, menu pop | | | `fx2_burst` 6 bone squares at the cursor | | SFX_MENU_MOVE |
| results row lands | | | `fx2_burst` 24 confetti in the seat colour on the winner row | | SFX_MENU_MOVE tick |

Caps: 400 sparks across all viewports (one pool, oldest recycled), 160 smoke quads, 24 debris, 6 live damage numbers per viewport, one stinger per viewport, 128 screen-space bursts.

### 5.3 Hit stop and particles

Sparks are spawned on the first frozen frame with zero velocity and a stored release time; while `M.hitstop > 0` they are drawn at the contact and not integrated; when the stop ends they integrate from release with speed 4 to 9 u/s in a 50 degree cone around the reflected hit normal plus 2 u/s of the victim's velocity, gravity 14, one floor bounce at 0.3, life 0.35 to 0.5 s, size 0.05 to 0.09 u. The impact flash is drawn on the first frozen frame. Smoke and fire run on wall time and never freeze. Spark colour comes from the victim's armour (section 3.6) or rust while the attacker is red tagged.

### 5.4 juice.h

```c
/* src/juice.h: every feel constant in the cart. Tune here, nowhere else. */
typedef struct { uint8_t stop; float punch, shake, flash; uint8_t sparks, chips; float rum_lo, rum_hi; uint16_t rum_ms; uint8_t sfx, crowd; } hit_band_t;
enum { BAND_TAP, BAND_HIT, BAND_HEAVY, BAND_HUGE, BAND_CLASH, BAND_KO, BAND_COUNT };
static const hit_band_t HIT_BANDS[BAND_COUNT] = {
  /* stop punch shake flash sparks chips  lo   hi   ms   sfx            crowd */
  {  0, 0.04f, 0.04f, 0.0f,  6, 0, 0.2f, 0.0f,  60, SFX_HIT_LIGHT, 0 },
  {  0, 0.06f, 0.08f, 0.3f, 12, 0, 0.4f, 0.2f,  80, SFX_HIT_LIGHT, 0 },
  {  3, 0.10f, 0.14f, 0.5f, 20, 0, 0.8f, 0.4f, 120, SFX_HIT_HEAVY, 0 },
  {  6, 0.12f, 0.22f, 0.7f, 32, 6, 1.0f, 0.6f, 160, SFX_HIT_HEAVY, 80 },
  {  8, 0.12f, 0.26f, 0.9f, 40, 8, 1.0f, 1.0f, 200, SFX_CLASH,     100 },
  {  6, 0.12f, 0.30f, 0.0f, 40, 8, 1.0f, 1.0f, 400, SFX_KO,        100 },
};
static inline int hit_band(float dmg) { return dmg < 8 ? BAND_TAP : dmg < 15 ? BAND_HIT : dmg < 30 ? BAND_HEAVY : BAND_HUGE; }
#define J_BAND_HUGE_WHITE_A   40     /* viewport white alpha, 1 frame */
#define J_BAND_CLASH_WHITE_A  60
#define J_SHAKE_CLAMP         0.34f
#define J_SHAKE_TAU           0.12f
#define J_SHAKE_HZ            28.0f
#define J_SHAKE_SCALE         { 0.0f, 0.5f, 1.0f, 1.5f }   /* settings 0..3 */
#define J_PUNCH_T             0.30f
#define J_STINGER_BIGHIT_MIN  25.0f
#define J_SPARK_SPEED_MIN     4.0f
#define J_SPARK_SPEED_MAX     9.0f
#define J_SPARK_CONE_DEG      50.0f
#define J_SPARK_INHERIT       2.0f
#define J_SPARK_GRAVITY       14.0f
#define J_SPARK_BOUNCE        0.3f
#define J_SPARK_LIFE_MIN      0.35f
#define J_SPARK_LIFE_MAX      0.50f
#define J_SPARK_SIZE_MIN      0.05f
#define J_SPARK_SIZE_MAX      0.09f
#define J_SPARK_CAP           400
#define J_SMOKE_CAP           160
#define J_SMOKE_RATE_50       4
#define J_SMOKE_RATE_30       6
#define J_SMOKE_LIFE          72
#define J_SMOKE_RISE          0.8f
#define J_FIRE_RATE_30        8
#define J_FIRE_LIFE           24
#define J_FIRE_RISE           1.5f
#define J_DEBRIS_CAP          24
#define J_DEBRIS_PANEL_LIFE   360
#define J_DEBRIS_WHEEL_LIFE   240
#define J_DEBRIS_FADE         60
#define J_DEBRIS_CHIP_LIFE    120
#define J_NUMBER_RISE_PX      40
#define J_NUMBER_LIFE         48     /* frames; alpha fades over the last 18 */
#define J_NUMBERS_PER_VIEW    6
#define J_STINGER_FRAMES      36     /* 6 in, 24 hold, 6 out */
#define J_KO_TIMESCALE        0.2f
#define J_KO_SLOW_FRAMES      90
#define J_KO_HOLD_FRAMES      60
#define J_KO_ORBIT_DEG_S      30.0f
#define J_KO_ORBIT_DIST       4.5f
#define J_KO_ORBIT_HEIGHT     2.4f
#define J_KO_VIGNETTE_A       60
#define J_MINIKO_TIMESCALE    0.4f
#define J_MINIKO_FRAMES       36
#define J_TAPE_CUT_FRAMES     24     /* 12 in, hold 1, 12 out at 0.4 s */
#define J_SPIN_RING_A         160    /* times spin */
#define J_SPIN_BLUR_A         50
#define J_SPIN_BLUR_DEG       15
#define J_HAZARD_TELL_HZ      2.0f
#define J_HAZARD_SHAKE        0.08f
#define J_OPS_SHAKE           0.06f
#define J_REDTAG_SPARKS       20
#define J_PICKUP_CUBES        8
#define J_LANDING_DUST        10
#define J_LAUNCH_DUST         6
#define J_WALL_SPARKS         8
#define J_SPINWALL_SPARKS     30
#define J_HAMMER_MISS_SPARKS  10
#define J_BARS_ANIM_FRAMES    18
#define J_BARS_DELTA_HOLD     72
#define J_SLOT_MACHINE_FRAMES 36
#define J_LOCK_CONFETTI       12
#define J_TAUNT_CONFETTI      4
#define J_FX2_CAP             128
```
## 6. Arenas (arena.c, data only)

### 6.1 Data format

```c
typedef struct { uint8_t type; float x, z, w, d, ang, h; float period, offset, active_s, tell_s; float dmg; int16_t p0, p1; uint8_t seat_ok; } hazard_def_t;
typedef struct { uint8_t kind; float x, z, w, d, h, ang; uint32_t colour; int16_t hp; } prop_def_t;   /* kind: PR_WALL, PR_BENCH, PR_HVAC, PR_BOX, PR_SHELF, PR_SIGN, PR_SERVER */
typedef struct { uint8_t x, y, w, h, colour; } tile_rect_t;                                        /* colour: 0 orange, 1 red */
typedef struct {
  const char *name, *hook;                 /* hook is ARENA_BLURB[i], under 24 chars */
  float w, d;                              /* full size; walls at +-w/2, +-d/2 */
  uint8_t open_mask;                       /* bit 0 +x, 1 -x, 2 +z, 3 -z: open edges (THE ROOF) */
  uint8_t floor_style;                     /* FL_VCT, FL_DARK, FL_CONCRETE, FL_WET, FL_SHOP, FL_ROOF */
  const tile_rect_t *tiles; int ntiles;
  uint32_t sun, hemi_sky, hemi_ground, point_a, point_b; float sun_i, hemi_i, point_a_i, point_b_i;
  float pa[3], pb[3];                      /* point light positions */
  hazard_def_t haz[MAX_HAZ]; int nhaz;
  prop_def_t props[32]; int nprops;
  float spawn4[4][3], spawn2[2][3];        /* x z heading per seat, 3 or 4 players; 1v1 */
  float ops_zone[2];                       /* centre of the 2 x 2 Corner Patrol Zone */
  float traction_mult;                     /* 0.85 in THE DOCK, else 1 */
  uint16_t vis[16];                        /* 4 x 4 cells, bit j set if cell j is visible from cell i */
  uint8_t one_camera_default, shared_yaw_deg;
} arena_def_t;
```

Hazard timing runs on `M.haz_clock` (section 4.4): `phase_t = fmod(haz_clock + offset, period)`; the tell is on when `phase_t > period - tell_s`; the hazard is active when `phase_t < active_s`. CLOSING TIME doubles the clock rate, so every hazard cycles twice as fast with no phase jump. Hazards hurt everyone equally; HOVER ignores HZ_PIT, HZ_KILLSAW, HZ_FLAME, HZ_FLOORFLIP, HZ_SCREW; WALKER ignores pit lips within 0.3; LEG TRACKS ignore HZ_SCREW. Hazards with `seat_ok` can be fired from a hazard seat (section 2.10).

### 6.2 Hazard types

| type | behaviour | damage | tell (1.5 s unless noted) | seat_ok |
|---|---|---|---|---|
| HZ_PIT | w x d square; closed until `offset`, then open `active_s` 6 of period 20; a bot whose centre is more than 0.3 inside while open drops 2 u over 30 frames and is KO; within 0.3 of the edge it slides in over 90 frames unless traction > 0.7 or WALKER | KO | amber floor lights blink, klaxon; caution tape border always | no |
| HZ_PULVERIZER | an edge hammer with a 1.2 x 1.2 strike square; auto swings when a bot sits under it in 1v1 (0.4 s swing, 150 frame cooldown); fired from a hazard seat otherwise (72 frame cooldown) | 20, stun 30, vy 2 | arm twitch, lamp above it goes teal | yes |
| HZ_KILLSAW | floor slot 1.6 x 0.4; saws up for 1.5 s on a 7 s period; tick 6 every 15 frames; 2x on TREADS | 6 per tick | slot glows amber, pitch-rising SFX | no |
| HZ_FLAME | grate 1.0 x 1.0; fire 2 s on an 8 s period; also fires when a bot crosses at over 3 u/s; seat fire 2 s then 4 s refill | 3 per tick every 20 frames plus burn | gas hiss, grate emissive | yes |
| HZ_SCREW | wall strip 4.0 x 0.8; always on; carries a bot along `ang` at 1.5 u/s and 3 dmg per 30 frames | 3 | constant rumble | no |
| HZ_RAMP | box w x d rising to h along `ang`; a bot crossing the top edge over 4.0 u/s gets vy = speed * 0.8 and flip_pending; the LASER ROOM bed uses h 0.2 as a lip (vy 1.5 over 3 u/s) | fall damage per 4.8 | none, it is terrain | no |
| HZ_BUMPER | post r 0.4 (p0 is height); contact impulse 5.0 outward and 4 dmg, 18 frame per-bot cooldown; p1 is HP on THE ROOF (breaks at 0) | 4 | lamp flash and bell | no |
| HZ_DECK | raised platform at height 1.0; bots on it run a 600 frame count (KO at 0); reached by ramps; LEG TRACKS and SIX WHEEL climb at full speed, others at 0.6x | KO on count | count number in the viewport | no |
| HZ_FLOORFLIP | w x d panel; fires when a bot has sat on it 60 frames: vy 6, flip_pending; seat fire 180 frame cooldown | 0 | panel pulses teal | yes |
| HZ_DROPZONE | 2 x 2 square; a bot immobile on it 180 frames gets a dropped prop (a dead CRT, a mini fridge, a Free Pile box, cycling) | 25, stun 30 | a blob shadow grows for 1.5 s | no |
| HZ_LASER | a beam at height 0.3 sweeping along x over 2 s every 30 s; bots taller than 0.3 (everything but LOWBOY and HOVER) take 10 and a scorch | 10 | teal line pre-drawn and a rising whine for 1.5 s | no |
| HZ_WIND | every 15 s a 3 s gust pushing 0.5 u/s2 toward an open edge (HOVER 1.5x) | 0 | flags on the HVAC units snap for 1.5 s | no |
| HZ_HILL | KING OF THE HILL only: 3 x 3 scoring square moving every 20 s to the next of 4 points | 0 | the next square flashes 1.5 s before the move | no |

Benches (THE SHOP), roof bumpers and the mound boxes are props with hp: PR_BENCH 60 per 2 u segment, HVAC solid, PR_BOX 60 (cardboard debris), PR_SERVER solid bumper. A broken prop spawns 6 debris and disappears from collision and from render.

### 6.3 The six arenas

Legend for the maps, 1 character = 1 unit horizontally and 1 row = 1 unit vertically, row 0 is the back wall (z = minus d/2), column 0 is the left wall (x = minus w/2). Each map is kept as a comment at the top of its `arena_def_t` in arena.c.

```
#  wall (lexan panel on a char rail, caution tape on top)    :  open edge, no wall, ring-out
P  pulverizer strike square     K  killsaw slot      F  flame grate      S  screw strip
R  ramp or lip                  B  bumper post       D  drop zone        X  floor flipper
O  OPS Corner Patrol Zone       ^  pit               =  upper deck       ~  bench (breakable)
*  mound (box props)            H  HVAC unit (solid) .  open floor
```

Spawns: 3 and 4 players spawn in the corners 2.5 u in from both walls facing the centre, except the OPS corner, whose spawn moves 2 u along the wall; seat 1 is the back-left corner, seat 2 front-right, seat 3 front-left, seat 4 back-right. 1v1 spawns are listed per arena. OPS parks in its 2 x 2 taped zone behind a lexan gate (an L of two 2 u wall segments on the zone's inner faces) that opens when it wakes; the zone is outside the pit, hazard and spawn clearances, which `balance --check-arenas` verifies.

#### 6.3.1 THE BAY (default, teaching arena, 24 x 16, one_camera_default 1)

```
##########################
#OO.........PP...........#
#OO......................#
#........................#
#........................#
#........................#
#........................#
#........................#
#...........^^...........#
#...........^^...........#
#........................#
#........................#
#........................#
#........................#
#........................#
#........................#
#...........PP...........#
##########################
```

Floor FL_VCT. Walls are low lexan (transparent 0.35, teal tinted) with a caution tape rail. Hazards: HZ_PULVERIZER at (0, minus 7.4) and (0, 7.4), seat_ok; HZ_PIT 2 x 2 at (0, 0), offset so it first opens at T minus 60 (period 20, active 6). Props outside the lexan only: two shelving units and a sign board behind the back wall. Lights: sun bone 2.0, hemisphere 1.2, point A amber 6 at (8, 3, minus 6), point B teal 6 at (minus 8, 3, 6). Spawns: (minus 7.5, minus 5.5), (9.5, 5.5), (minus 9.5, 5.5), (9.5, minus 5.5); 1v1 at (minus 8, 0) and (8, 0) facing each other. OPS zone centre (minus 11, minus 7). Hook: PIT. TWO HAMMERS.

#### 6.3.2 THE LASER ROOM (20 x 20)

```
######################
#OO....F...F...F.....#
#OO..................#
#....................#
#....................#
#....................#
#....................#
#......KK....KK......#
#....................#
#........RRRR........#
#........RRRR........#
#........RRRR........#
#........RRRR........#
#....................#
#......KK....KK......#
#....................#
#....................#
#....................#
#....................#
#....................#
#....................#
######################
```

Floor FL_DARK (concrete 0x8a857c) with one red tile square under the bed. Teal task lights. HZ_KILLSAW x4 at (minus 3, minus 3), (3, minus 3), (minus 3, 3), (3, 3), period 7, offsets 0, 1.75, 3.5, 5.25 so they go round. HZ_RAMP honeycomb bed 4 x 4 at the centre, h 0.2 (a lip). HZ_FLAME x3 along the back wall at x minus 4, 0, 4 (z minus 9.5), seat_ok. HZ_LASER sweeping along x every 30 s. Lights: sun dim 0.6 teal tinted, point A teal 10 at (0, 3, 0), point B amber 4 at (0, 2, minus 8). Spawns: (minus 5.5, minus 7.5), (7.5, 7.5), (minus 7.5, 7.5), (7.5, minus 7.5); 1v1 at (minus 7, 0) and (7, 0). OPS zone (minus 9, minus 9). Hook: SAWS. FIRE. A LASER.

#### 6.3.3 THE FREE PILE (24 x 16)

```
##########################
#........................#
#.....DD.................#
#.....DD.................#
#........................#
#..........****..........#
#B........******.........#
#........********........#
#........********........#
#B.......********.......S#
#........********.......S#
#.........******........S#
#B.........****.........S#
#........................#
#.................DD.....#
#OO...............DD.....#
#OO......................#
##########################
```

Floor FL_CONCRETE with cardboard scuffs painted. HZ_MOUND is 12 PR_BOX props (0.4 to 0.9 high, 60 HP each, under 60 triangles each, cardboard debris on break) forming a radial height field r 4, peak 0.9; bots on top drop 8 fall damage onto others. HZ_DROPZONE 2 x 2 at (minus 6, minus 6) and (6, 6). HZ_SCREW along the right wall at x 11.6, z 0 to 4, carrying toward plus z. HZ_BUMPER x3 (old servers, PR_SERVER) along the left wall at z minus 3, 0, 3. Most props, fewest hazards. Lights: sun bone 2.0, point A amber 6 at (0, 2, 0), point B bone 4 at (8, 3, minus 5). Spawns: (minus 9.5, minus 5.5), (9.5, 5.5), (minus 7.5, 5.5), (9.5, minus 5.5); 1v1 at (minus 8, 0) and (8, 0). OPS zone (minus 11, 7). Hook: A MOUND. SCREWS.

#### 6.3.4 THE DOCK (20 x 16)

```
######################
#====================#
#====================#
#====================#
#...RR..........RR...#
#...RR..........RR...#
#...RR..........RR...#
#....................#
#....................#
#....................#
#.........XX.........#
#.........XX.........#
#....................#
#....................#
#....................#
#OO.......^^^........#
#OO.......^^^........#
######################
```

Floor FL_WET (traction_mult 0.85 for everyone but TREADS), outdoors at night. HZ_DECK 20 x 3 along the back wall at height 1.0 reached by two HZ_RAMP 2 x 3 at x minus 6 and 6. HZ_FLOORFLIP 2 x 2 at (0, 2), seat_ok. HZ_PIT 3 x 2 at (0, 7), the roll-up door gap, open from T minus 60 on the same cycle as THE BAY. Lights: one sodium amber point A 10 at (0, 4, minus 4), point B dim teal 2 at (6, 1, 6), sun 1.2 amber tinted. Spawns: (minus 7.5, minus 3.5), (7.5, 5.5), (minus 5.5, 5.5), (7.5, minus 3.5); 1v1 at (minus 7, 1) and (7, 1). OPS zone (minus 9, 7). Hook: A DECK. RAIN. A GAP.

#### 6.3.5 THE SHOP (20 x 20)

```
######################
#OO.......PP.........#
#OO..................#
#..........~.........#
#..........~.........#
#..........~.........#
#..........~.........#
#..........~.........#
#..........~.........#
#....................#
#P.~~~~~~K..K~~~~~~.P#
#....................#
#....................#
#..........~.........#
#..........~.........#
#..........~.........#
#..........~.........#
#..........~.........#
#..........~.........#
#....................#
#.........PP.........#
######################
```

Floor FL_SHOP (concrete with a painted aisle grid and caution tape round the electrical panel on the back wall). Four PR_BENCH runs 6 x 0.6 at height 0.4 along the axes from |x| or |z| 2 to 8, each three 2 u segments of 60 HP that break separately, so the room opens up during the match. HZ_PULVERIZER at all four wall midpoints, seat_ok. HZ_KILLSAW x2 at (minus 1.5, 0) and (1.5, 0), slots along z. The arena that changes shape and the 4 player perf worst case. Lights: sun bone 2.0, point A amber 6 at (0, 3, 0), point B teal 6 at (minus 7, 2, 7). Spawns: (minus 5.5, minus 7.5), (7.5, 7.5), (minus 7.5, 7.5), (7.5, minus 7.5); 1v1 at (minus 7.5, minus 7.5) and (7.5, 7.5) (opposite quadrants). OPS zone (minus 9, minus 9). Hook: BENCHES THAT BREAK.

#### 6.3.6 THE ROOF (18 x 18, open_mask plus x and plus z)

```
###################:
#OO................:
#OO................:
#..................:
#.................B:
#......HH..........:
#......HH..........:
#..................:
#..................:
#..................:
#...HH............B:
#...HH.............:
#..................:
#..................:
#..................:
#..................:
#.................B:
#..................:
#...B.....B.....B..:
::::::::::::::::::::
```

Floor FL_ROOF (slab grey 0x4a4338 with white roof seam lines and hazard tape along the open edges). No walls on plus x and plus z: a bot whose centre is 0.3 beyond an open edge falls (KO, cause OFF THE ROOF, 1.2 s fall camera). HZ_BUMPER x6 along the open edges with p1 60 HP each. HZ_WIND toward plus x then plus z alternating every 15 s. Two PR_HVAC units 2 x 2 (solid, with flags for the wind tell) at (minus 2, minus 4) and (minus 5, 1). Best for flippers, terror for HOVER. Lights: low sunset sun amber 1.6, point A amber 5 at (minus 4, 3, minus 4), point B teal 5 at (5, 3, 5). Spawns: (minus 6.5, minus 4.5), (6.5, 6.5), (minus 6.5, 6.5), (6.5, minus 6.5); 1v1 at (minus 6, minus 6) and (6, 6). OPS zone (minus 8, minus 8). Hook: NO RAILS. WIND.

### 6.4 The painted floors

`paint_floor(const arena_def_t*)` in world.c paints a 1024 x N RGBA texture through the HUD buffer (the `hud_texture` trick from DO-OCRACY; N is 683 for 24 x 16, 1024 for 20 x 20, 819 for 20 x 16, 1024 for 18 x 18) and hands it to `t3_data_texture_new` with `color_space = T3_SRGB_COLOR_SPACE` as the floor plane's map (state `standard+map+l1201`). For FL_VCT and FL_SHOP: tile pitch 0.6 u (25.6 px for a 24 u wide arena, 30.7 px for 20 u), base tile 0xd9d3c7 with a 1 px grout 0x9c968a and six random 2 px speckles per tile at 0xcbc4b7; then the arena's `tile_rect_t` list in orange 0xd9672b and red 0xb23a2e as whole tiles; then a one-tile hazard amber ring around every killsaw, flame grate and floor flipper; the pit gets a caution tape border; the OPS zone gets tape; spawn corners get a 2 x 2 block in the seat colour at 40 percent over the tile. Albedo stays under 0.78 because the sun is 2.0 and the hemisphere 1.2 (not DO-OCRACY's 2.8 and 1.7), so white never clips. The texture is painted once per arena at boot and repainted for scorch decals at most every 30 frames in a fight. The vote screen thumbnails are the same painter run at 352 x 120 into the HUD buffer directly (section 10.12).

Tile rectangles (tile indices from the back-left tile; 0 orange, 1 red):

| Arena | Rectangles (x, y, w, h, colour) |
|---|---|
| THE BAY (40 x 27 tiles) | (6, 4, 4, 2, 0), (20, 3, 1, 8, 1), (30, 18, 3, 3, 1), (12, 22, 6, 1, 0), (34, 5, 2, 2, 0), (18, 11, 4, 1, 1), (18, 15, 4, 1, 1), (18, 11, 1, 5, 1), (21, 11, 1, 5, 1) [the last four are the red ring round the pit tape] |
| THE LASER ROOM (34 x 34) | (13, 13, 8, 8, 1) under the bed; one orange tile at each saw slot: (10, 11, 2, 1, 0), (22, 11, 2, 1, 0), (10, 22, 2, 1, 0), (22, 22, 2, 1, 0) |
| THE FREE PILE (40 x 27) | six orange 3 x 3 blocks at (3, 3), (33, 2), (5, 20), (31, 21), (14, 2), (26, 23); a red one-tile band round the mound footprint: (12, 8, 16, 1, 1), (12, 19, 16, 1, 1), (12, 8, 1, 12, 1), (27, 8, 1, 12, 1) |
| THE DOCK (34 x 27) | red 2 x 34 band along the deck edge (0, 5, 34, 2, 1); orange 2 x 2 at the ramp feet (5, 10, 2, 2, 0), (27, 10, 2, 2, 0) |
| THE SHOP (34 x 34) | orange one-tile bands under the benches: (3, 16, 11, 1, 0), (20, 16, 11, 1, 0), (16, 3, 1, 11, 0), (16, 20, 1, 11, 0); red 2 x 2 under each pulverizer: (16, 0, 2, 2, 1), (16, 32, 2, 2, 1), (0, 16, 2, 2, 1), (32, 16, 2, 2, 1) |
| THE ROOF | no tiles; FL_ROOF draws seam lines every 3 u and a 6 x 6 orange square at the centre because the roofer liked it |
| garage bay (48 x 10 tiles) | (4, 2, 3, 2, 0), (14, 6, 1, 4, 1), (24, 1, 6, 1, 0), (36, 3, 2, 2, 1), (42, 7, 4, 1, 0) |

### 6.5 Visibility table

`vis[16]` is a 4 x 4 grid over the arena. For every arena but THE SHOP and THE FREE PILE all bits are set (frustum culling does the work). THE SHOP: a cell sees itself, its row and column, and the diagonal quadrants only through the centre cross (the far diagonal quadrant's bits are cleared until a bench segment on that axis breaks, at which point `world_set_cell_visibility` is given the all-set table). THE FREE PILE: the mound hides the opposite row's props from the edge cells. draw.c sets prop group visible flags per viewport from the camera's cell before each render. Bots, hazards and debris are never culled by the table.

### 6.6 The arena checker

`build/balance --check-arenas` drives a 1.1 x 1.1 box through every gap between solids and fails if any gap is between 0 and 1.6 u (a gap is either at least 1.6 u or fully closed); samples 64 camera positions per cell at the chase height and flags any prop whose bounding box projects into the view while its cell bit is clear; checks every spawn has 1.5 u of clearance from solids, hazards and the OPS zone; checks that every hazard's reach (strike square, slot, cone, strip) lies inside the floor. It runs in CI before packing. Every arena also gets a scripted lap (section 15.5) before it ships.

## 7. OPS, the house robot

A referee with a grudge: the Operations role made into a bot. Build: LUNCHBOX chassis stretched 1.4x, TREADS, LIFTER primary with a SAW secondary, 300 HP (400 while driven by eliminated players in a TOURNAMENT final), inv_mass 0 (immune to knockback and push), push 2.0, speed 3.2, dents show, HP floors at 1 and it is never KO'd. Char body with a hazard amber stripe and a caution tape bumper, label OPS. It is `bots[4]` with `is_ops` set and never counts for spawns, judges or awards.

Triggers (checked every step while parked): `no_hit_frames >= 720`, or a pin (section 2.11). Target: the pinner if there is a pin, else the HP leader (LEADER TAX). Behaviour: EV_OPS_WAKE (gate opens, horn, stinger), drive to the target with the AI steering, lift it (zone 1.5 while carried), carry it 3 u toward the arena centre, drop it, return to the zone, EV_OPS_LEAVE. If any bot damages OPS more than 20 within 300 frames, OPS turns on that bot for 300 frames with a 40 damage lift-and-shove, then returns. OPS leaves at T minus 10 (drives into the zone, the gate closes) so the end is always between players. In a TOURNAMENT final the two eliminated players drive it (one drives, one runs the lifter and saw); the same triggers decide when it may leave the zone.

Arrival is an event: minimap flash, SFX_HORN low variant, a 0.5 s full screen stinger OPS INBOUND, every viewport shakes 0.06 for 20 frames, and one of OPS_LINES in a bone speech panel over its body for 2 s (section 16). Speech panels have their own budget: one on screen at a time.

## 8. AI (ai.c, headless, 10 Hz think, 60 Hz steer)

### 8.1 Perception

Positions, headings, velocities, HP, weapon type and spin of every bot, read from a delayed copy: the AI keeps a 24 frame ring of bot summaries and reads frame `now - reaction`, reaction 24 for EASY, 12 for MEDIUM, 3 for HARD. It knows the hazard timeline exactly (like a player who has watched one match). Steering uses the arena walls and solids directly: steer to target and slide along walls, no pathfinding. Thinks are staggered per bot so four AIs never think on the same tick.

### 8.2 Goals, scored every 6 frames, highest wins with a 0.1 hysteresis bonus for the current goal

| Goal | Score | Action |
|---|---|---|
| ATTACK | 0.5 + 0.3 if the enemy is exposed (spun-down spinner, flank of a flipper, underside of an inverted bot, landing within 0.3 s) + 0.2 if my weapon is ready; minus 0.4 if the enemy spinner is over 0.7 and mine is not a spinner | steer at the enemy's weakest zone, weapon per 8.3 |
| EVADE | 0.9 if the enemy spinner is over 0.7 within 3.0 and I am not a wedge or on treads; 0.6 if an enemy hammer is winding up within 1.5; 0.5 if my HP is under 25 and the enemy's over 60 | back off to 3.5 u and circle at 60 degrees |
| HAZARD PUSH | 0.8 if the enemy is within 3 of a hazard active or activating within 1.5 s and my push exceeds theirs | shove along the line from me through them to the hazard |
| RECOVER | 1.0 if inverted and I can self-right; 0.7 if my spinner is below 0.3 and the enemy within 2 | self-right; or retreat to 4 u and hold the weapon |
| BAIT (HARD only) | 0.6 if a hazard activates within 2 s and the enemy is chasing me | drive across the hazard square so it is active when the enemy arrives |
| PICKUP | 0.45 if a pickup panel is within 4 and my HP is under 70 (never for MELTY) | drive over it |
| TAUNT | 0.65 if the enemy is inverted, 20 percent roll per think | X for 30 frames |
| WIGGLE | 0.95 if my own countout exceeds 300 frames (5 s) | drive forward and back 0.5 u so a parked AI is never counted out |

### 8.3 Weapon rules

Spinners and drums hold the weapon unless RECOVER says otherwise. Flipper fires when the enemy centre is inside the 0.6 x 0.5 front box and the cooldown is 0. Hammer fires when the enemy is within 1.2 and its predicted position in 15 frames is still inside the strike box. Spike fires within 1.0 ahead. Flame fires in 60 frame bursts within 2.0. Saw toggles on at 1.5 and off when nothing is within 4 (it costs HP). Lifter holds while touching the enemy's front or side and steers to the nearest hazard or wall. Crusher closes on any contact. Boost when ATTACK has a straight line over 4 u or EVADE needs it. Wedge drives at the enemy's side with boost when the line is clear.

### 8.4 Difficulty and build choice

EASY: reaction 24, never EVADE or BAIT, build is ROLL OUT, LAUNCH or STOMP at random. MEDIUM: reaction 12, evades, counter-picks the human's last build: spinner against wedge, wedge against spinner, SIX WHEEL plus HAMMER against a flipper, flipper against anything on TWO WHEEL, LOWBOY against a hammer, else LAUNCH. HARD: reaction 3, BAIT on, remembers across the evening in `save.habits[8]` (flips landed, times pinned, favourite weapon, boosts) and goes LOWBOY after being flipped twice, TREADS after being pushed into a hazard or against a frequent booster.

### 8.5 Steering

PD on heading error: `turn = clamp(2.2 * err + 0.3 * derr, -1, 1)`; throttle 1.0 when |err| < 60 degrees, 0.3 otherwise, reverse when the target is behind and within 1.5. Stuck detector: under 0.3 u of progress in 120 frames, then reverse 60 frames and add 50 degrees to the approach angle. Corner escape: within 1.0 of two walls, drive along the longer wall for 60 frames. HOVER and MELTY AIs use direction vectors directly.

### 8.6 Other users

OPS runs the same scorer with OPS_GO (to the target), OPS_LIFT, OPS_CARRY, OPS_RETURN and OPS_PUNISH. The attract screen runs two MEDIUM AIs with random presets in THE BAY, seeded from the frame counter at title entry, so it is never the same twice. The headless balance tool (section 15.7) runs 20 matches per preset pair in every arena with hazards on and off; no preset may win over 65 or under 35 percent overall, WEDGE must beat HORIZONTAL at least 40 percent, HOVER must lose to TREADS over 55 percent in THE BAY and win over 55 percent in THE LASER ROOM. Tune tag costs first, self damage and the wall penalty second, base damage last.

## 9. Cameras (camera.c)

### 9.1 Layouts

| Players | Layout | Viewport rects (x, y from the top, w, h) | FOV | Minimap |
|---|---|---|---|---|
| 1 | full | (0, 0, 1280, 720) | 60 | 240 x 135 at (1024, 16) |
| 2 | vertical halves | (0, 0, 640, 720), (640, 0, 640, 720) | 60 | 200 x 112 at (540, 66) under the clock, over the seam; hidden in ONE CAMERA |
| 3 | two top, one bottom left, the fourth quadrant is the map quadrant | (0, 0, 640, 360), (640, 0, 640, 360), (0, 360, 640, 360); map quadrant (640, 360, 640, 360) | 70 | 420 x 236 at (750, 420) with the match card under it |
| 4 | quadrants | (0, 0, 640, 360), (640, 0, 640, 360), (0, 360, 640, 360), (640, 360, 640, 360) | 70 | 200 x 112 at (540, 304) over the centre seams |
| ONE CAMERA | shared | (0, 0, 1280, 720) | 50, pitched 55 degrees | none; the shared view is the map |

Vertical halves for 2 players on purpose: the gap between you and them is a horizontal problem and each half keeps its HUD strip at the bottom out of the action. SPLIT is a setting (AUTO, HALVES, QUADRANTS). Every rect scales by the negotiated resolution over 1280 x 720. The seams are 4 px caution tape lines drawn by the HUD.

### 9.2 Chase camera (per seat)

Target position `bot - forward * 3.5 + up * 2.2`; look point `bot + forward * 0.6 + up * 0.3`. The position follows with a 0.12 s time constant (`k = 1 - exp(-dt / 0.12)`), the look point with 0.06 s. `forward` is the heading for drives with a front, the velocity (or last stick direction) for HOVER, and the heading intent for MELTY, never the spinning body. When the bot is inverted the camera does not roll. When the bot is airborne the camera keeps its own height and the bot rises in frame (lag 0.2 s while airborne). The right stick nudges yaw plus and minus 30 degrees and springs back. After smoothing, the camera position is pushed out of every wall AABB and solid by a 0.4 u radius and never goes below y 0.6, so halves stay readable in corners. Punch and shake (section 5.1) are added last. The bot sits about 60 percent down the viewport and the strip covers the bottom 10 percent, which is floor.

When this seat's nearest enemy is outside the viewport frustum, a 24 px filled chevron (`hud_tri`) in the enemy's seat colour on a 2 px char shadow sits at the viewport edge on the line from the viewport centre to the enemy's projected position. Never text.

### 9.3 ONE CAMERA (shared)

Pitch 55 degrees, yaw fixed per arena (`shared_yaw_deg`), aimed at the centroid of the alive bots, distance `clamp(max pairwise distance * 1.6 + 6, 12, 22)` so everyone fits with 3 u of margin. Engaged when all alive bots have been within 10 u of each other for 60 frames; released when any pair exceeds 12 u for 60 frames or any bot leaves the frustum for 60 frames. Default on for 2 players in THE BAY, off elsewhere, and a per-match toggle ONE CAMERA on the arena vote screen and the 3 player map quadrant. Every switch is a 0.4 s cut with a caution tape wipe (two 24 px tape bands slide in from the sides over 12 frames, hold 1 frame while the layout changes, slide out over 12), never a zoom. On a Pi it is one render pass instead of two.

### 9.4 KO camera and TIME camera

On the final KO every viewport collapses with the tape wipe to one shared camera orbiting the victim at 30 degrees per second, distance 4.5, height 2.4, looking at the midpoint of victor and victim, FOV 50, yaw starting on the line from victor to victim. Timescale 0.2 for 90 render frames, then the sim pauses and the orbit holds 60 frames at 1.0. The victor's seat colour tints the vignette at alpha 60; the strips fade to alpha 120. Then a hard cut to results. On TIME the camera pulls up to the ONE CAMERA pose over 0.6 s while the judges card slides in.

### 9.5 Minimap

Drawn on the HUD from sim positions, never rendered: a char panel at alpha 220 with a 2 px bone frame scaled to the arena footprint; walls as 1 px ash lines, open edges as hazard tape; hazards as glyph squares (`hud_glyph` pit, hammer, saw, flame, screw, ramp, bumper, deck, flipper, drop, beam, wind) in steel, amber while active, pulsing during a tell; the pit as a char square with a tape frame; bots as 8 px squares in seat colour with a 3 px heading tick; OPS as a 10 px char square with an amber dot; the hill as a live teal square; a hazard seat's pick as a teal arrow in that seat's colour; the ONE CAMERA frustum footprint as a faint bone rectangle; count-out digits beside counted bots. The 3 player big map adds bot names in `F_MONO_S` beside the squares.
## 10. Screen flow and HUD

### 10.1 Phases (game.c)

```
PH_BOOT -> PH_TITLE <-> PH_SETTINGS, PH_HALL, PH_RULES
PH_TITLE -> PH_LOBBY -> PH_GARAGE -> PH_VOTE -> PH_COUNTDOWN -> PH_FIGHT
PH_FIGHT -> PH_KO (final KO) or PH_JUDGES (time) -> PH_RESULTS -> PH_REMATCH
PH_REMATCH: REBUILD -> PH_GARAGE (30 s, hint) ; SAME BOTS -> PH_VOTE ; NEW ARENA -> PH_VOTE (builds kept) ; LOBBY -> PH_LOBBY
PH_RESULTS idle 40 s -> PH_LOBBY ; PH_LOBBY empty 10 s -> PH_TITLE ; PH_TITLE idle 60 s -> attract reseeds
PH_FIGHT + Start -> pause flag (not a phase); PH_GARAGE + Y held -> control card flag
```

PH_BOOT builds every scene, paints every floor texture, calls `t3_renderer_compile` on the garage scene and each arena scene with a camera, logs `t3_renderer_generated_missing` once, and shows a char screen with the marquee and a 12 segment tape bar. About 1 s on a Mac and a few seconds on a Pi, which is why it is a screen.

Title idle 1.5 s: the attract match starts behind the menu (two MEDIUM AIs, random presets, THE BAY, seeded from the frame counter). Any input brings the cursor back. The attract never leaves the title; it is the title's background.

### 10.2 Input in menus

One menu reader in main.c: per pad, d-pad or left stick with a 0.5 threshold, edge detected with repeat after 18 frames then every 8; A, B, X, Y, Start, Select edges. The title, settings, hall of fame and pause take input from every pad (first edge wins the frame, and the pad that moved owns the cursor for 2 s). The lobby, garage, vote and rematch take per seat input. Nothing joins mid fight: an unseated pad that presses anything during a fight gets a 2 s NEXT MATCH chip in its would-be seat colour at the bottom centre of the screen and nothing else.

### 10.3 HUD texture, scale, fonts and hud.c additions

The HUD is one RGBA data texture composited after the viewports exactly as DO-OCRACY does (`basic+map+trans+nearest` on a 2 x 2 plane under an ortho camera, auto clear off). hud.c, hud.h and glyphs.h are copied from DO-OCRACY after its front-end work (FINAL-frontend.md section 5) so this cart gets `hud_text_sh`, `hud_clip`, `hud_clip_clear`, `hud_eyebrow`, `hud_keycap`, `hud_ease_out`, `hud_ease_inout`, `hud_ease_back`, `hud_tween`, `hud_glyph(x, y, id, scale, rgb, a)`, `hud_set_alpha`, `hud_text_runs`, `fx2_burst` and `fx2_draw` for free. If the copy happens first, those are ported from that document. Additions for this cart:

- `hud_tag(x, y, filled, bonus)`: the 14 x 10 tag glyph with a notched corner (hazard fill, char with a steel frame, live frame for bonus tags).
- `hud_tri(x0, y0, x1, y1, x2, y2, rgb, a)`: one filled triangle, for the off-screen chevron and the control card arrows.
- `menu_row(x, y, w, label, value, focused, enabled, is_choice)`: label left in `F_H3`, value right in `F_MONO_L`, 14 px chevrons either side of a choice value (amber when a direction is available), the focus treatment is a hazard block with tape ink text and a 6 px shadow that `hud_tween`s between rows; disabled rows draw in smoke.
- `card(x, y, w, h, step, total)`: a panel with a 24 px tape header and a step dot row at the bottom (10 px bone squares, the current one hazard).
- `hud_dirty(x, y, w, h)`: widens the frame's dirty rect; main.c bumps `hud_tex->version` only on frames with a dirty rect (in a fight that is every frame; on the title idle and the results hold the saving is real).
- Glyph ids added to glyphs.h: GL_PAD, GL_KEYS, GL_BOT, GL_AI, GL_SPINNER, GL_DRUM, GL_FLIPPER, GL_HAMMER, GL_FLAME, GL_SAW, GL_SPIKE, GL_LIFTER, GL_CRUSHER, GL_WEDGE, GL_TREAD, GL_WHEEL, GL_HOVER, GL_WALKER, GL_LEG, GL_MELTY, GL_TAG, GL_SKULL, GL_FLAG, GL_CROWN, GL_PIT, GL_SCREW, GL_RAMP, GL_BUMPER, GL_DECK, GL_FLIP_PANEL, GL_DROP, GL_BEAM, GL_WIND, GL_BRACKET, GL_TWO_BOTS, GL_PLUS, GL_CHECK.
- Compile-time `HUD_DIV` (1 or 2): with 2 the buffer is 640 x 360, fonts bake at half size, and every `hud_` call divides its coordinates, so the whole HUD layer is authored at 1280 x 720 regardless. Default 1 on the Mac and the browser, 2 when the negotiated height is 540 or below.

Fonts as DO-OCRACY (F_TITLE 128, F_H1 64, F_H2 40, F_H3 26, F_MONO_L 30, F_MONO 21, F_MONO_S 15) plus F_STINGER (Archivo Black 56, atlas 1024 x 512) and F_STINGER_Q (Archivo Black 36, atlas 1024 x 512); drop F_LOGO. Only ASCII 32 to 127 is baked, so every string is plain caps.

### 10.4 Per viewport HUD strip (fight)

`hud_strip(vp, bot)` draws at the bottom of each viewport: 72 px tall in halves and full, 56 px in quadrants, a char rect at alpha 220 with a 2 px seat colour rule on top (never a panel shadow, which would read as a card in the world). Halves, x relative to the viewport:

| Element | Rect | Notes |
|---|---|---|
| seat chip | (8, 8, 36, 56) | seat colour fill, the number in `F_H2` tape ink; a 4 Hz flicker while stunned; a rust frame while red tagged; a count numeral replaces the number while counting out |
| bot name | (52, 6) | `F_MONO_S` bone, the auto-generated name |
| HP bar | (52, 26, 400, 18) | `hud_bar` on slab, fill in seat colour above 50 HP, hazard to 25, rust below and pulsing 2 Hz; a bone ghost segment shows the last 0.6 s of damage taken (fighting-game drain); lost segments stay as steel; a rust RED TAG strip (a 6 px bar with RED TAG in `F_MONO_S` tape ink) sits over the bar's right end while red tagged |
| HP number | right of the bar | `F_MONO_L` bone |
| weapon gauge | (470, 10, 52, 52) | `hud_ring`: spin for spinners, drum and saw (full ring pulses); FLIPPER cooldown and FLAME fuel as a vertical tank; HAMMER, SPIKE and CRUSHER cooldown as a short bar; a lift angle wedge for LIFTER; nothing for WEDGE and NONE; a cooldown is a rust ring unwinding; the weapon glyph sits at the ring centre |
| speed | (530, 26, 60, 18) | 6 ticks in ash lit by |v| / top |
| boost | (530, 48, 60, 8) | teal when ready, rust unwinding while cooling; hidden on TREADS and LEG TRACKS |
| status word | (470, 62) | `F_MONO_S` in teal or rust: SPINNING UP, FULL SPIN, EMPTY, STUNNED, INVERTED, BURNING, RED TAG, LIFTED, CLAMPED; halves and full only |
| mode chip | right end | KING OF THE HILL score or the 2V2 team pool when relevant |

Quadrants (56 px): chip 28 x 40, HP bar (40, 20, 280, 14), ring 36 px, speed 40 x 12, no status word (the ring colour carries it), no boost bar (a 3 pip row instead). Strip content stops 110 px short of the inner seam in quadrants so the PIP never covers it.

In the viewport: damage numbers are world anchored and projected with that viewport's camera, `F_MONO_L` for taps and hits and `F_H2` from heavy, in the attacker's seat colour on a 2 px char shadow for damage you dealt and rust for damage you took, rising 40 px over 48 frames with alpha fading over the last 18, at most 6 live per viewport, clipped to the viewport with `hud_clip`. The count-out numeral is `F_TITLE` rust at the viewport centre with B TO RIGHT YOURSELF or GET MOVING under it in `F_MONO`. The off-screen enemy chevron is section 9.2. A 24 px caution tape strip runs along the top of a viewport while its bot is inside a hazard tell zone.

### 10.5 Shared overlays

Match clock: a char chip 120 x 56 at (580, 6) with a 6 px shadow, seconds left in `F_H1` bone, a 4 px hazard progress bar under the digits, rust and blinking 2 Hz during CLOSING TIME; in quadrants it is 120 x 44 on the top seam above the PIP. Stingers: one per viewport at a time, 36 frames (6 in from the left by 20 px, 24 hold, 6 out), a char bar 72 px tall (48 in quadrants) across the viewport at 35 percent of its height with a 6 px solid shadow in the triggering seat's colour, text in `F_STINGER` (halves and full) or `F_STINGER_Q` (quadrants) bone, centred, under 24 characters, never two fonts; a new stinger replaces the old. Full screen stingers (FIGHT, TIME, OPS INBOUND) are drawn once per viewport at the same moment. Seams are 4 px caution tape lines. The KO card, judges card, results and pause are their own screens below.

### 10.6 Wireframe key

Every wireframe is 80 columns by 30 rows: one character is 16 x 24 pixels at 1280 x 720. `/` is caution tape, `.` inside a region is the 3D scene showing through, `=` is a panel rule or a filled bar, `[A]` is a keycap, `>` is the cursor, `o` is a pip. Pixel positions are in the legend under each frame. Every panel has square corners and a 6 px `C_SHADOW` offset shadow (`hud_panel`). Font slots are named.
### 10.7 TITLE and attract

```
+------------------------------------------------------------------------------+
|//////////////////////////////////////////////////////////////////////////////|
|                             :                                                |
|  [MARK ]  HEATSYNC LABS ARCADE SPINNY PANCAKE [=========.] [.=======] YEETING|
|  [160px]                    :   . . . . . . . . . . . . . . . . . . . . . . .|
|  [bone ]  RED TAG           :   .                                            |
|  [flame]  (F_TITLE 128)     :   .      attract fight, THE BAY, ONE CAMERA    |
|                             :   .      two MEDIUM AIs, random presets        |
|           RUMBLE            :   .      no scrim on this side                 |
|           (F_TITLE hazard)  :   .                                            |
|                             :   .                                            |
|                             :   .                                            |
|  FOURTEEN DAYS. THEN IT IS  :   .                                            |
|  FREE. OR IT IS A BOT.      :   .                                            |
|                             :   .                                            |
|  +------------------------+ :   .                                            |
|  | > FIGHT     1 TO 4 PADS| :   .                                            |
|  +------------------------+ :   .                                            |
|    SOLO    YOU VS THE       :   .                                            |
|            PRACTICE BOT     :   .                                            |
|    HOW IT WORKS   18 S      :   .                                            |
|    HALL OF FAME             :   .                                            |
|    SETTINGS                 :   .                                            |
|                             :   .                                            |
|                             :   .                                            |
|                             :   .                                            |
|                             :   .                                            |
|  [A] SELECT  [B] BACK       :                 BEST EVENING: 4 WINS, SEAT 2   |
|                             :                                                |
|//////////////////////////////////////////////////////////////////////////////|
+------------------------------------------------------------------------------+
```

Legend: tape (0, 0, 1280, 24) and (0, 696, 1280, 24) scrolling 30 px/s. Scrim: char at alpha 120 over the left column (0, 0, 480, 720) only (the `:` edge); the attract fight shows clean on the right. Mark 160 px at (64, 56), flame bone, via `hud_logo`, the only call in the cart. Eyebrow HEATSYNC LABS ARCADE at (290, 70) `F_MONO_S` ash. RED TAG at (290, 96) `F_TITLE` bone with a 3 px char shadow; RUMBLE at (290, 206) `F_TITLE` hazard. Tagline at (290, 300) `F_H3` bone with shadow, one of TAGLINES rotating every 6 s. Menu rows at x 80, y 360 + 44 per row, `F_H2` bone; the hint column at x 290 in `F_MONO` ash (1 TO 4 PADS, YOU VS THE PRACTICE BOT, 18 S, empty, empty). Cursor: a hazard block 380 x 40 with a 6 px shadow whose y `hud_tween`s to the focused row over 10 frames; focused text is tape ink. Attract HP bars: two 200 x 10 ash bars at (496, 32) and (1064, 32) with the attract bot names in `F_MONO_S` ash beside them. Keycap hint at (80, 660). BEST EVENING at the right in `F_MONO_S` ash from `save_t`, absent when empty. Nine strings on screen. SOLO goes straight to the garage with the practice bot panel. HOW IT WORKS plays the rules flash.

### 10.8 LOBBY

```
+------------------------------------------------------------------------------+
|  PICK A MODE                                           EVENING: 2 - 1 - 0 - 0|
| +-----------+ +-----------+ +-----------+ +-----------+ +-----------+ +-----+|
| | DEATHMATCH| | KO BY FLIP| | LAST BOT  | | KING OF   | | 2V2       | |TOURN||
| | [spinner] | | [flipper] | | STANDING  | | THE HILL  | | [2 bots]  | |AMENT||
| |           | |           | | [skull]   | | [crown]   | |           | |[brk]||
| +-----------+ +-----------+ +-----------+ +-----------+ +-----------+ +-----+|
|   1 TO 4. HP ZERO. LAST ONE UP.                                              |
|                                                                              |
| +------------------+ +------------------+ +------------------+ +------------+|
| | 1 [pad]   amber  | | 2 [pad]    live  | | 3          bone  | | 4     rust ||
| |                  | |                  | |                  | |            ||
| |  ..............  | |  ..............  | |                  | |            ||
| |  . turntable  .  | |  . turntable  .  | |     PRESS A      | |   PRESS A  ||
| |  . last bot   .  | |  . last bot   .  | |     TO JOIN      | |   TO JOIN  ||
| |  . 3D view    .  | |  . 3D view    .  | |                  | |            ||
| |  ..............  | |  ..............  | |                  | |            ||
| |                  | |                  | |                  | |            ||
| | POLITE DOORSTOP  | | YEETING BOX      | |                  | |            ||
| | ROLL OUT  13 TAGS| | LAUNCH    12 TAGS| |                  | |            ||
| | o o              | | o                | | FILL WITH BOT    | | FILL WITH  ||
| | READY            | | ARCADE  < > TANK | |   < OFF >        | | BOT < OFF >||
| +------------------+ +------------------+ +------------------+ +------------+|
|   hold [B] 1 S TO LEAVE (ring fills around the B cap on your own card)       |
|                                                                              |
|          ONE CAMERA < AUTO >     SPLIT < AUTO >     COMEBACK < AUTO >        |
|                                                                              |
|          [START] GO TO THE GARAGE        auto in 25 S once a seat is ready   |
|                                                                              |
|//////////////////////// LOBBY: EMPTY IN 10 (only when no seat) //////////////|
+------------------------------------------------------------------------------+
```

Legend: eyebrow PICK A MODE at (32, 28); EVENING tally `F_MONO_S` ash right aligned at 1248. Mode cards: 6 cards 190 x 100 at y 40, x 20 + 206 per card, `F_H3` name on two lines where needed, a 16 px glyph; any seated pad's Left and Right moves the mode cursor (last press wins); the chosen card lifts (shadow 6 to 10 px, frame hazard); MODE_BLURB for the chosen mode at (32, 150) `F_MONO` ash. Seat cards: 4 cards 300 x 220 at y 220, x 20 + 310 per card, with a 3D viewport (x+16, 252, 268, 120) inside each occupied card showing that seat's last bot on its turntable (scissored render of the garage scene, up to 4 small viewports). Bot name `F_H3` bone, preset label and tags `F_MONO_S` ash, wins as 10 px bone pips, control scheme stepper (ARCADE or TANK) and READY (A toggles; a ready card gets a live rule). Empty cards show PRESS A TO JOIN pulsing at 1 Hz and the FILL WITH BOT stepper (moved by any seated pad's Right stick while holding Y, or by the unseated pad itself). Steppers row at y 504 (ONE CAMERA, SPLIT, COMEBACK) moved by any seated pad. Start by any ready seat goes to the garage; auto start 25 s after the first READY; an empty lobby returns to the title after 10 s with the tape count; an idle seated pad drops after 60 s with a visible 10 to 0 count on its card.

### 10.9 GARAGE, 2 players (vertical halves)

```
+------------------------------------------------------------------------------+
| 1 SPINNY PANCAKE                   CHOP | 2 YEETING BOX                LAUNCH|
| FLIPPED 3 TIMES: TRY LOWBOY OR SIX WHEEL | (no hint on a first build)        |
| [t][t][t][t][t][t][t][t][t][t][t][t][t][t][t][ ][ ][ ][ ][ ] 15 SPENT. 5 LEFT|
|                                          |                                   |
|   ....................................   |  ...............................  |
|   .                                 .   |   .                             .  |
|   .        3D turntable bay         .   |   .     3D turntable bay        .  |
|   .     bot rotates 20 deg/s         .   |   .                            .  |
|   .  right stick scrubs, RT fires    .   |   .                            .  |
|   .                                 .   |   .                             .  |
|   .                                 .   |   .                             .  |
|   .                                 .   |   .                             .  |
|   ....................................   |  ...............................  |
|                                          |                                   |
|   CHASSIS     <   LOWBOY        >   3    |   CHASSIS    <   BOX         >  2 |
| > DRIVE       <   FOUR WHEEL    >   3    |   DRIVE      <   TWO WHEEL   >  2 |
|   WEAPON      <   HORIZONTAL    >   7    |   WEAPON     <   FLIPPER     >  6 |
|   ARMOUR      <   ALUMINIUM     >   2    | > ARMOUR     <   STEEL       >  4 |
|   PAINT       <   SCHEME 2      >        |   PAINT      <   SCHEME 1    >    |
|                                          |                                   |
|   FOUR WHEELS, ZERO DRAMA                |   HEAVY. YOU WILL FEEL IT TURNING |
|                                          |                                   |
|   SPEED  [=========++---]  teal delta    |   SPEED  [=====--........] rust   |
|   HIT    [===========---]                |   HIT    [=======.........]       |
|   TOUGH  [=====---------]                |   TOUGH  [==========++....] teal  |
|                                          |                                   |
|   [A] LOCK IN  [START] PRESET  [X] RANDOM BOT  [Y] CONTROLS |  LOCKED (live) |
|                                          |                                   |
|/////////////////////////////////  0:42  /////////////////////////////////////|
+------------------------------------------------------------------------------+
```

Legend per 640 panel (x relative): header row 32 px: seat chip 28 px, bot name `F_H3` bone, preset label `F_MONO_S` ash right aligned at 624. Hint line (rebuild only) at y 36 `F_MONO_S` bone. Tag bar at (16, 72): 20 to 24 `hud_tag` glyphs 14 x 10 at 16 px pitch, the spent string `F_MONO_S` bone at its right. 3D viewport (16, 100, 608, 230) rendered with the split recipe before the HUD pass. Rows from y 350, 34 px each: label `F_H3`, value `F_MONO_L`, tag cost right aligned in hazard (rust when this row pushes the total over); the cursor is a 4 px seat colour bar at the row's left and the row text in bone (others ash); illegal options in smoke with a one-word reason. Flavour line at y 540 `F_MONO` bone. Bars at y 580, 606, 632 (140 x 10 with `F_MONO_S` words). Keycap hints at y 672. Locked state: the panel gets a 3 px live frame, rows dim to alpha 180, hints become LOCKED and [B] RELEASE. Timer tape (0, 696, 1280, 24) shrinks from both ends toward x 640 with the seconds in a char chip at the centre.

### 10.10 GARAGE, 4 players (quadrants)

```
+------------------------------------------------------------------------------+
| 1 SPINNY PANCAKE  [t][t][t][t][t][t][t][t]| 2 YEETING BOX  [t][t][t][t][t][t]|
|  ....................  CHASSIS < LOWBOY > 3|  ................ CHASSIS < BOX |
|  .                .> DRIVE  < FOUR WH > 3|  .              .  DRIVE  < TWO W |
|  .   turntable    .  WEAPON < HORIZ   > 7|  .  turntable   .> WEAPON < FLIPP |
|  .                .  ARMOUR < ALUMIN  > 2|  .              .  ARMOUR < STEEL |
|  .                .  PAINT  < SCHEME 2 > |  .              .  PAINT  < SCHEM |
|  .                  .                    |  .              .                 |
|  .                .  SPEED [========--]  |  .              .  SPEED [=====--]|
|  .                .  HIT   [==========]  |  .              .  HIT   [=======]|
|  .                .  TOUGH [=====-----]  |  .              .  TOUGH [=======]|
|  ....................  FOUR WHEELS, ZERO  |  ................  GRAVITY, BUT  |
|                      DRAMA               |                    FOR THEM       |
| [A] LOCK [START] PRESET [X] RANDOM [Y] CTL| [A] LOCK [START] PRESET [X] RAND |
|============================================|=================================|
| 3 TOASTY BOWL    [t][t][t][t][t][t][t][t]| 4 PRACTICE BOT (AI) EASY <MEDIUM> |
|  ....................  CHASSIS < SHELL  > 4| ................ CHASSIS < WEDGE|
|  .                .  DRIVE  < HOVER   > 5|  .              .  DRIVE  < TREAD |
|  .   turntable    .  WEAPON < FLAME   > 4|  .  turntable   .  WEAPON < WEDGE |
|  .                .  ARMOUR < UHMW    > 3|  .              .  ARMOUR < STEEL |
|  .                .  PAINT  < SCHEME 3 > |  .              .  PAINT  < SCHEM |
|  .                  .                    |  .              .                 |
|  .                .  SPEED [=========-]  |  .              .  SPEED [====---]|
|  .                .  HIT   [=====-----]  |  .              .  HIT   [======-]|
|  .                .  TOUGH [=======---]  |  .              .  TOUGH [=======]|
|  ....................  LEAF BLOWER. DO NOT |  ................  LOCKED       |
|                      ASK                 |                   (cycles 5 s,    |
|  [A] LOCK [START] PRESET [X] RANDOM [Y] CTL|                  then locks)    |
|///////////////////////////////////  0:17 ////////////////////////////////////|
+------------------------------------------------------------------------------+
```

Legend per 640 x 360 quadrant (x, y relative): header row 24 px with the chip and bot name `F_MONO_L`, tag bar from x 300 with 12 px tags; 3D viewport (16, 32, 300, 230); rows from x 330, y 36, 30 px each, `F_MONO_L` values (names truncate at 9 characters here, so the garage names in parts.h have a 9 character short form: FOUR WH, HORIZ, ALUMIN, FLIPP, TREAD, SCHEM 2); bars at (330, 196), (330, 216), (330, 236), 120 px wide; flavour line at (330, 262) wrapped to two lines `F_MONO_S`; keycap hints at y 330; the hint line replaces the flavour line for the first 5 s of a rebuild. The seam is 4 px tape at y 358 and x 638. Timer tape at y 696.

### 10.11 GARAGE, 3 players (two top, one bottom left, the match card bottom right)

```
+------------------------------------------------------------------------------+
| 1 SPINNY PANCAKE  [t][t][t][t][t][t][t][t]| 2 YEETING BOX  [t][t][t][t][t][t]|
|  ....................  CHASSIS < LOWBOY > 3|  ................ CHASSIS < BOX |
|  .                .> DRIVE  < FOUR WH > 3|  .              .  DRIVE  < TWO W |
|  .   turntable    .  WEAPON < HORIZ   > 7|  .  turntable   .> WEAPON < FLIPP |
|  .                .  ARMOUR < ALUMIN  > 2|  .              .  ARMOUR < STEEL |
|  .                .  PAINT  < SCHEME 2 > |  .              .  PAINT  < SCHEM |
|  .                  .                    |  .              .                 |
|  .                .  SPEED [========--]  |  .              .  SPEED [=====--]|
|  .                .  HIT   [==========]  |  .              .  HIT   [=======]|
|  .                .  TOUGH [=====-----]  |  .              .  TOUGH [=======]|
|  ....................  FOUR WHEELS, ZERO  |  ................  GRAVITY, BUT  |
|                      DRAMA               |                    FOR THEM       |
| [A] LOCK [START] PRESET [X] RANDOM [Y] CTL| [A] LOCK [START] PRESET [X] RAND |
|============================================|=================================|
| 3 TOASTY BOWL     [t][t][t][t][t][t][t][t]|   NEXT FIGHT                     |
|  ....................  CHASSIS < SHELL  > 4|                                 |
|  .                .  DRIVE  < HOVER   > 5|   DEATHMATCH        3 BOTS        |
|  .   turntable      .  WEAPON < FLAME   > 4|                                 |
|  .                .  ARMOUR < UHMW    > 3|   FIGHT LENGTH      <   75 S  >   |
|  .                .  PAINT  < SCHEME 3 > |   ONE CAMERA        <  AUTO   >   |
|  .                  .                    |   COMEBACK          <  AUTO   >   |
|  .                  .  SPEED [=========-]  |                                 |
|  .                .  HIT   [=====-----]  |   EVENING   1: o o   2: o   3:    |
|  .                  .  TOUGH [=======---]  |                                 |
|  ....................  LEAF BLOWER. DO NOT |  ANY SEAT: UP DOWN PICKS A ROW, |
|                      ASK                 |   LEFT RIGHT STEPS IT             |
|  [A] LOCK [START] PRESET [X] RANDOM [Y] CTL|                                 |
|///////////////////////////////////  0:17 ////////////////////////////////////|
+------------------------------------------------------------------------------+
```

Legend: quadrants 1 to 3 are the 4 player panels. The fourth quadrant (640, 360, 640, 360) is the match card: eyebrow NEXT FIGHT at (680, 372); mode name `F_H3` bone and the player count at (680, 410); three `menu_row`s at (680, 456), (680, 492), (680, 528) 560 px wide for FIGHT LENGTH (60, 75, 90, 120 S), ONE CAMERA (AUTO, OFF, ALWAYS), COMEBACK (AUTO, ON, OFF), stepped by any seat with the Right stick while holding Y, or by an unseated pad; the evening tally with pips at (680, 576); a two-line hint in `F_MONO_S` ash at (680, 620). During the fight this quadrant holds the big minimap (10.15).

### 10.12 ARENA VOTE

```
+------------------------------------------------------------------------------+
|  PICK AN ARENA           DEATHMATCH  75 S          ONE CAMERA < AUTO >  0:06 |
|                                                                              |
| +----------------------+ +----------------------+ +----------------------+   |
| | THE BAY              | | THE LASER ROOM       | | THE FREE PILE        |   |
| | +------------------+ | | +------------------+ | | +------------------+ |   |
| | | painted floor    | | | | painted floor    | | | | painted floor    | |   |
| | | thumb 352x120    | | | | 352x120, dark,   | | | | 352x120, mound   | |   |
| | | white VCT, orange| | | | one red square   | | | | as a brown block | |   |
| | | red rects, pit   | | | |                  | | | |                  | |   |
| | +------------------+ | | +------------------+ | | +------------------+ |   |
| | PIT. TWO HAMMERS.    | | SAWS. FIRE. A LASER. | | A MOUND. SCREWS.     |   |
| | [pit][hammer] [1][3] | | [saw][flame][beam][2]| | [drop][screw]        |   |
| +----------------------+ +----------------------+ +----------------------+   |
|                                                                              |
| +----------------------+ +----------------------+ +----------------------+   |
| | THE DOCK             | | THE SHOP             | | THE ROOF             |   |
| | +------------------+ | | +------------------+ | | +------------------+ |   |
| | | painted floor    | | | | painted floor    | | | | painted floor    | |   |
| | | 352x120, deck    | | | | 352x120, aisle   | | | | 352x120, seams,  | |   |
| | | band in red      | | | | grid, benches    | | | | open edges taped | |   |
| | |                  | | | |                  | | | |                  | |   |
| | +------------------+ | | +------------------+ | | +------------------+ |   |
| | A DECK. RAIN. A GAP. | | BENCHES THAT BREAK.  | | NO RAILS. WIND.      |   |
| | [flip][pit][deck][4] | | [hammer][saw]        | | [wind][edge]         |   |
| +----------------------+ +----------------------+ +----------------------+   |
|                                                                              |
|  STICK PICKS. MAJORITY WINS. TIES GO RANDOM.                                 |
|                                                                              |
|////////////////////////////////// 8 S ///////////////////////////////////////|
+------------------------------------------------------------------------------+
```

Legend: eyebrow PICK AN ARENA at (32, 28); mode and length `F_MONO` ash at (420, 28); ONE CAMERA stepper (`menu_row`, any seat, Left and Right while holding Y) at (880, 20, 240, 36); timer chip `F_H2` hazard at (1180, 20). Six cards 384 x 232 at x 32, 448, 864 and y 72, 376: name `F_H3` bone at (x+16, y+8); the thumbnail at (x+16, y+40, 352, 120) is `paint_floor` run at thumbnail scale straight into the HUD buffer (tiles, hazard rings, pit tape, OPS tape, deck and mound as flat colour blocks), so the vote screen is one HUD pass with no 3D viewports; ARENA_BLURB `F_MONO_S` ash at (x+16, y+172); hazard glyphs 16 px at (x+16, y+200), no hazard words; voter chips 24 px at the card's bottom right. Each seat's stick moves a 3 px frame in its colour between cards; the leading card lifts (shadow 10 px). At 0, or 2 s after every seat has voted, the winning card lifts, the others dim, 1 s, then countdown; ties pick at random with a 0.5 s slot machine flicker over the tied cards.

### 10.13 COUNTDOWN (2 players, halves)

```
+------------------------------------------------------------------------------+
|                                   +--1:15--+                                 |
|                                  |  (bone) |                                 |
|                                   +--------+                                 |
|                                          |                                   |
|                                          |                                   |
|    ....................................|...................................  |
|     .   chase cam, seat 1 bot in its    |   chase cam, seat 2, weapon      . |
|    .   corner, spinner spinning up,    |   spinning up allowed, no drive   . |
|     .   no drive                        |                                  . |
|     .                                   |                                  . |
|     .                                   |                                  . |
|     .               3                  |                 3                 . |
|     .          (F_TITLE 128)            |           (F_TITLE 128)          . |
|     .                                   |                                  . |
|     .                                   |                                  . |
|     .                                   |                                  . |
|     .                                   |                                  . |
|     .                                   |                                  . |
|     .                                   |                                  . |
|     .                                   |                                  . |
|     .                                   |                                  . |
|     .                                   |                                  . |
|     .                                   |                                  . |
|    ....................................|...................................  |
|                                          |                                   |
|                                          |                                   |
|==========================================|===================================|
| [1] SPINNY PANCAKE  [==========] 85 (o) |[2] YEETING BOX [==========] 100 (o)|
|     speed ......  boost ====   FULL SPIN |    speed ......  boost ====       |
+------------------------------------------------------------------------------+
```

Legend: the layout is already split and the strips are live. The digit sits at each viewport centre, 3 then 2 then 1 with SFX_COUNT and a 1.08x scale pop over 0.15 s, then FIGHT in `F_STINGER` on a hazard bar across every viewport for 0.6 s with SFX_HORN; the clock reads the fight length in bone and goes live on FIGHT. The music cuts from GARAGE to ARENA on FIGHT.

### 10.14 FIGHT, 2 players (vertical halves)

```
+------------------------------------------------------------------------------+
|                                   +---42---+                                 |
|                                   | (F_H1) |                                 |
|                                   +--------+                                 |
|                                 +--------------+                             |
|                                 | . . . . . .  |  minimap 200x112            |
|                                 | .  #   o   . |  over the seam              |
|                                 | .   ^^     . |                             |
|                                 | . o . . . .  |                             |
|                                 +--------------+                             |
|                                          |                                   |
|          chase cam seat 1, FOV 60        |     chase cam seat 2              |
|                                          |                                   |
|                                          |              24                   |
|                               -30        |     (damage number rising,        |
|                     (damage taken, rust) |      seat 1 amber, F_H2)          |
|                                        > |                                   |
|                        (chevron, teal)   |                                   |
|                                          |                                   |
| +--------------------------------------+ |                                   |
| |            AIRBORNE                 | |   stinger bar 72 px, 0.6 s         |
| +--------------------------------------+ |                                   |
|                                          |              B TO RIGHT YOURSELF  |
|                                          |                     7             |
|                                          |          (count, F_TITLE rust)    |
|                                          |                                   |
|                                          |                                   |
|==========================================|===================================|
| [1] SPINNY PANCAKE                      |[2] YEETING BOX                     |
| [1] [########......] 58  (o) ====  .....  |[2] [#############..] 88  (o) === |
| [1]  BOOST ====  FULL SPIN               |[2]  BOOST ..   RED TAG            |
+------------------------------------------------------------------------------+
```

Legend: clock chip (580, 6, 120, 56); minimap (540, 66, 200, 112); the seam is 4 px tape at x 638. Each half has its strip at (vx, 648, 640, 72) per section 10.4 (the ghost drain is the bone segment to the right of the fill; RED TAG is the rust strip over the bar's right end). The stinger bar sits at viewport y 400 to 472. Damage numbers are projected with that half's camera and clipped to it. The chevron is at the right edge of seat 1's half on the line to seat 2. In ONE CAMERA the two halves become one view with both strips at the bottom (left and right 640) and the minimap hidden; a 3 px ring in seat colour sits under each bot in the world.

### 10.15 FIGHT, 3 players (two top, one bottom left, the big map bottom right)

```
+------------------------------------------------------------------------------+
|                                   +---42---+                                 |
|   chase cam seat 1, FOV 70        +--------+   chase cam seat 2              |
|                                          |                                   |
|                                          |                                   |
|        +------------------+              |                                   |
|        |    BIG HIT       |  (36 px)     |                       +14         |
|        +------------------+              |                                   |
|                                          |                              >    |
|                                          |                                   |
|                                          |                                   |
|                                          |                                   |
|                                          |                                   |
| [1] SPINNY PANCAKE [#####....] 48 (o) =  | [2] YEETING BOX [#######..] 80 (o)|
|==========================================|===================================|
|                                       |  THE BAY   DEATHMATCH   EVENING 2-1-0|
|   chase cam seat 3                      |  +------------------------------+  |
|                                        |  |  .  .  .  .  .  .  .  .  .  . |  |
|                                       |  |  .  [P]   o 2       .  .  .  . |  |
|                                        |  |  .  .  .  .  .  .  .  .  .  . |  |
|                                       |  |  .  .  .  ^^  .  .  .  .  .  . |  |
|                                       |  |  .  o 1   ^^  .  .  .  .  .  . |  |
|                 10                   |  |  .  .  .  .  .  .  o 3  .  .  . |  |
|            GET MOVING               |  |  .  .  .  [P]   .  .  .  .  .  . |  |
|                                     |  |  .  .  .  .  .  .  .  .  .  .  . |  |
|                                         |  +------------------------------+  |
|                                          |   big minimap 420 x 236, names    |
| [3] TOASTY BOWL  [###.......] 22 (o) RED |                                   |
|==========================================|===================================|
+------------------------------------------------------------------------------+
```

Legend: quadrants 1 to 3 as the 4 player layout; the fourth quadrant (640, 360, 640, 360) holds the match card (`F_MONO_S` ash at (660, 372): arena, mode, evening tally) and the big minimap 420 x 236 at (750, 420) with bot names in `F_MONO_S` beside the squares. The clock stays at (580, 6, 120, 44) on the top seam. Quadrant strips are 56 px at the bottom of each quadrant.

### 10.16 FIGHT, 4 players (quadrants with the PIP)

```
+------------------------------------------------------------------------------+
|                                   +---42---+                                 |
|   chase cam seat 1, FOV 70        +--------+   chase cam seat 2              |
|                                          |                                   |
|                                          |                                   |
|                                          |                      +14          |
|        +------------------+              |                                   |
|        |    INVERTED      |  stinger 48  |                                   |
|        +------------------+              |                                   |
|                                          |                               >   |
|                   7                      |                                   |
|           (count, F_TITLE rust)          |                                   |
|                                          |                                   |
| [1] THUMPY LUNCHBOX [######....] 61 [=] | [2] TOASTY BOWL [########..] 80 (o)|
|=================================+---------------+============================|
|                                 | . o . . . . . | PIP 200x112 over the seams |
|   chase cam seat 3              | .  ^^   o  . |   chase cam seat 4          |
|                                 | . o . . . o . |                            |
|                                 +---------------+                            |
|                                          |                                   |
|                                          |                                   |
|                                          |                                   |
|                                          |                                   |
|                                          |                                   |
|                                          |                                   |
|                                          |                                   |
|                                          |                                   |
| [3] DIZZY PANCAKE  [#########.] 90 (o) | [4] POLITE DOORSTOP [##########] 100|
|==========================================|===================================|
+------------------------------------------------------------------------------+
```

Legend: clock (580, 6, 120, 44); PIP (540, 304, 200, 112) over the centre seams; quadrant strips 56 px at y 304 to 360 and 664 to 720 whose content stops 110 px short of the inner seam; stinger bars 48 px in `F_STINGER_Q` at quadrant y 150 to 198; the seams are 4 px tape lines.

### 10.17 FIGHT, 1 player (full) and ONE CAMERA

```
+------------------------------------------------------------------------------+
|                                   +---42---+                +--------------+ |
|                                   +--------+                |  minimap     | |
|                                                             |  240 x 135   | |
|                                                             |  . o   ^^  . | |
|                                                             |  .  o  .   . | |
|                                                             +--------------+ |
|                                                                              |
|                                                                              |
|       chase cam, FOV 60 (1 player)                                           |
|       or the shared 55 degree camera (ONE CAMERA, no minimap)                |
|                                                                              |
|                                                                              |
|                          [seat 1 bot]                                        |
|                                                                              |
|                                              [practice bot]     18           |
|                                                                              |
|                       [pit, tape border, amber ring blinking]                |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|==============================================================================|
| [1] SPINNY PANCAKE  [#######...] 72  (o) ====  .....    [AI] POLITE DOORSTOP |
| [1]  BOOST ====  FULL SPIN                              [AI] [#####.....] 52 |
+------------------------------------------------------------------------------+
```

Legend: 1 player: the player's strip is 640 wide at the bottom left and the practice bot gets a 320 px short strip (chip, name, HP) at the bottom right; minimap 240 x 135 at (1024, 16). ONE CAMERA with 2 players: the same full frame with both full strips along the bottom in seat order, no minimap, a 3 px seat colour ring under each bot in the world. The tape wipe plays on every switch.

### 10.18 KO slow-mo (shared camera) and mini-KO

```
+------------------------------------------------------------------------------+
|//////////////////////////////////////////////////////////////////////////////|
|                                                                              |
|                                                                              |
|                                                                              |
|          ................................................................    |
|          .   shared orbit camera, 30 deg/s, victim centred, victor in   .    |
|          .   frame, timescale 0.2 interpolated, sparks held on the      .    |
|          .   first frozen frame then released                           .    |
|          .                                                              .    |
|          .     [victor bot]              [victim bursting: panels fly,  .    |
|          .                                wheels roll, smoke to black]  .    |
|          .                                                              .    |
|          ................................................................    |
|                                                                              |
|                                   K.O.                                       |
|                          (F_TITLE bone, 6 px shadow                          |
|                           in the victor's seat colour)                       |
|                                                                              |
|                              YEETING BOX                                     |
|                              KO BY FLIP                                      |
|                        (F_H3 bone, F_H3 rust)                                |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
| [1] SPINNY PANCAKE  [#######...] 72           [2] YEETING BOX  [..........] 0|
|   (strips at alpha 120, vignette tinted seat 1 amber at alpha 60)            |
|//////////////////////////////////////////////////////////////////////////////|
+------------------------------------------------------------------------------+
```

Legend: tape bands slide in from the top and bottom over the 0.4 s cut and scroll at 60 px/s. K.O. lands at 0.3 s into the slow-mo at (640, 260) centred, scaled 1.3x to 1.0x over 0.2 s (drawn with a 4 px bone outline that is removed at 0.2 s). The victim's bot name at (640, 400) and the cause at (640, 432): KO BY FLIP, PITTED, COUNTED OUT, OFF THE ROOF, DECK COUNT, HP ZERO. Music fades to the drums stem; the KO gong covers the gap. Then a hard cut to results.

A mini-KO (a non-final elimination in a free-for-all) is 0.6 s at timescale 0.4 with no camera change: the victim's viewport shows the KO stinger with the name and cause in the stinger slot for 1.5 s and then becomes the hazard seat view (10.19); every other viewport sees the burst in the world and the victim's dot vanish from the minimap.

### 10.19 Hazard seat viewport (one quadrant, 640 x 360)

```
+--------------------------------------+
| [3] OUT      YOU HAVE THE HAMMERS    |
|       +------------------------+     |
|       |  [H]              [H]  |     |
|       |                        |     |
|       |     o 1                |     |
|       |            . 2         |     |
|       |                 ^ 4    |     |
|       |         [pit]          |     |
|       |                        |     |
|       |  [H]              [H]  |     |
|       +------------------------+     |
|  < STICK PICKS A HAMMER  [A] SWING   |
|      COOLDOWN 1.2 S                  |
|                                      |
+--------------------------------------+
```

Legend (quadrant relative; this frame is 40 x 15 characters, one quadrant): badge at (8, 8) with the seat chip and OUT in `F_MONO` bone, the line YOU HAVE THE HAMMERS (or YOU HAVE THE FLAME, YOU HAVE THE FLIPPER) `F_MONO_S` ash at (60, 10); a top-down arena map 420 x 236 at (110, 56) with the arena outline in steel, the hazards this seat can fire as glyphs (the chosen one glowing teal), live bots as 8 px seat colour squares with heading ticks, the pit as a tape framed square; the hint `F_MONO_S` ash at (16, 316) and (16, 334). In halves the same view fills the half at 2x. Stick left or right (or up and down) picks the hazard in arena order; A fires; a rust ring unwinds over the chosen glyph during its cooldown.

### 10.20 TIME and the judges card

```
+------------------------------------------------------------------------------+
|                                     TIME                                     |
|                               (F_TITLE bone)                                 |
|                                                                              |
|   camera pulls up to the ONE CAMERA pose over 0.6 s; bots keep idling        |
|                                                                              |
|                                                   +-------------------------+|
|                                                    |  J U D G E S            |
|                                                    |                         |
|                                                    |  DAMAGE          7      |
|                                                    |  [1] [==========   ] 5.2|
|                                                    |  [2] [======       ] 1.8|
|                                                    |                         |
|                                                    |  AGGRESSION      4      |
|                                                    |  [1] [=====        ] 1.9|
|                                                    |  [2] [=======      ] 2.1|
|                                                    |                         |
|                                                    |  CONTROL         4      |
|                                                    |  [1] [===          ] 1.0|
|                                                    |  [2] [========     ] 3.0|
|                                                    |                         |
|                                                    |  [1] 8.1    [2] 6.9     |
|                                                    |                         |
|                                                    |  WINNER: SPINNY PANCAKE |
|                                                    |  (or UNDERDOG RULE chip |
|                                                    |   plus the tie line)    |
|                                                   +-------------------------+|
|                                                                              |
|                                                                              |
|                                                                              |
+------------------------------------------------------------------------------+
```

Legend: TIME `F_TITLE` bone at y 48. The judges card is 400 x 480 at (816, 144), sliding in from the right over 0.3 s: eyebrow JUDGES; three criteria with their weights `F_H3` bone; one bar per bot (up to four) in seat colour with the value `F_MONO`; bars fill in sequence over 0.6 s each (damage, aggression, control) with SFX_MENU_MOVE ticks; totals `F_H2`; the winner chip lands in live with SFX_MENU_OK, or the chip reads UNDERDOG RULE with TIE. CHEAPER BOT WINS. THAT IS THE RULE. (or TIE. FEWER FLIPS WINS. or DRAW. NOBODY TELL THE TREASURER.) under it. Total 3 s, A skips.

### 10.21 RESULTS and the rematch menu

```
+------------------------------------------------------------------------------+
|//////////////////////////////////////////////////////////////////////////////|
|  THE BAY . DEATHMATCH . 0:48 LEFT                          EVENING: 3 - 1 - 0|
|  POLITE DOORSTOP WINS                                                        |
|  (F_H1, seat colour)                                                         |
|  KO BY FLIP. TURNS OUT GEOMETRY IS A WEAPON.          (F_H3 hazard)          |
|                                                                              |
|  +------------------------+   +--------------------------------------------+ |
|  |                        |   | 1 [2] POLITE DOORSTOP            WINNER    | |
|  |   winner turntable     |   |   DEALT 112  BIGGEST 31  AIR 0.0 S  FLIPS 0| |
|  |   viewport 420 x 300   |   |   THE DOORSTOP DID IT                      | |
|  |   confetti fx2 burst   |   | 2 [1] SPINNY PANCAKE             FLIPPED   | |
|  |                        |   |   DEALT 64   BIGGEST 30  AIR 4.2 S  FLIPS 3| |
|  |                        |   |   SPINNER WITH A CEILING FAN COMPLEX       | |
|  |                        |   | 3 [3] TOASTY BOWL                PITTED    | |
|  |                        |   |   DEALT 22   BIGGEST 9   AIR 6.0 S  FLIPS 2| |
|  |                        |   |   LEFT THE CHAT                            | |
|  |                        |   | 4 [AI] HUMBLE SKELETON           COUNTED   | |
|  |                        |   |   DEALT 8    BIGGEST 8   AIR 0.0 S  FLIPS 0| |
|  |                        |   |   ARRIVED EVENTUALLY                       | |
|  +------------------------+   +--------------------------------------------+ |
|                                                                              |
|  +------------------------+     (timeout only)                               |
|  | > REBUILD    (AUTO 16) |     DAMAGE [=======] 7  AGGRESSION [===.] 3      |
|  |   SAME BOTS     [1]    |     CONTROL [==..] 2    UNDERDOG RULE            |
|  |   NEW ARENA     [2]    |     TIE. CHEAPER BOT WINS. THAT IS THE RULE.     |
|  |   LOBBY                |                                                  |
|  +------------------------+                                                  |
|                                                                              |
|  [A] VOTE   [B] BACK                                WINS  1: 3   2: 1   3: 0 |
|//////////////////////////////////////////////////////////////////////////////|
+------------------------------------------------------------------------------+
```

Legend: eyebrow (64, 40) `F_MONO_S` ash; the winner line `F_H1` in the winner's seat colour at (64, 64) ("SEAT 2 WINS. POLITE DOORSTOP." when two names could confuse); the cause line at (64, 136) `F_H3` hazard from KO_CAUSE_LINES chosen by cause. Winner turntable viewport (64, 180, 420, 300), the garage scene with the winner's bot, `fx2_burst` confetti in the seat colour on arrival. Rows panel (520, 180, 696, 330): up to 4 rows of 76 px sliding in from +80 px with a 0.2 s stagger, SFX_MENU_MOVE per landing, ranked by placement; rank `F_H2` smoke, seat chip, bot name `F_H3` bone, placement word (WINNER, FLIPPED, PITTED, COUNTED, OFF ROOF, TIMED OUT, HP ZERO) `F_MONO_S` ash, stats `F_MONO_S` bone, the award `F_MONO_S` hazard; the winner row gets the seat colour as its left rule. Judges bars at (520, 540) only on a timeout, with the UNDERDOG RULE chip and the tie line. The rematch menu is a 400 x 176 panel at (64, 520) with four `menu_row`s: REBUILD (default, AUTO counts from 20), SAME BOTS, NEW ARENA, LOBBY; each seat's cursor is a 20 px chip at the row's right; the majority wins when every seat has pressed A or at 0, REBUILD winning ties. Results accept no input for the first 2 s; after 40 s of silence the game goes to the lobby. TOURNAMENT shows the bracket in place of the turntable: names in seat chips joined by caution tape, the next pairing in hazard. Hall of fame records are updated here.

### 10.22 SETTINGS

```
+------------------------------------------------------------------------------+
|//////////////////////////////////////////////////////////////////////////////|
|                                                                              |
|                 +----------------------------------------------+             |
|                 |  SETTINGS                                    |             |
|                 |                                              |             |
|                 |  > SPLIT                        <  AUTO   >  |             |
|                 |    ONE CAMERA                   <  AUTO   >  |             |
|                 |    FIGHT LENGTH                 <  75 S   >  |             |
|                 |    COMEBACK                     <  AUTO   >  |             |
|                 |    SHAKE                        <   2     >  |             |
|                 |    HIT STOP                     <  ON     >  |             |
|                 |    STINGER SIZE                 <  BIG    >  |             |
|                 |    SFX                          <   8     >  |             |
|                 |    MUSIC                        <   6     >  |             |
|                 |    COLOURBLIND                  <  OFF    >  |             |
|                 |    RUMBLE                       <  ON     >  |             |
|                 |    STICKS   1 ARCADE  2 TANK  3 ARCADE  4 ARCADE           |
|                 |    RESET SAVE                  hold [A] 2 S  |             |
|                 |                                              |             |
|                 |  SAVED TO THE CABINET                        |             |
|                 |  [B] BACK                                    |             |
|                 +----------------------------------------------+             |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|//////////////////////////////////////////////////////////////////////////////|
+------------------------------------------------------------------------------+
```

Legend: one card 720 x 540 at (280, 72); 13 `menu_row`s 32 px apart from y 136; values: SPLIT (AUTO, HALVES, QUADRANTS), ONE CAMERA (AUTO, OFF, ALWAYS), FIGHT LENGTH (60 S, 75 S, 90 S, 120 S), COMEBACK (AUTO, ON, OFF), SHAKE (0, 1, 2, 3), HIT STOP (ON, OFF), STINGER SIZE (BIG, SMALL), SFX (0 to 10), MUSIC (0 to 10), COLOURBLIND (OFF, ON: live teal gets a bone hatch wherever it means a seat), RUMBLE (ON, OFF), STICKS (one stepper per seated pad, ARCADE or TANK), RESET SAVE (hold A 2 s with a fill ring). B leaves. Every change rewrites `save_t`; SAVED TO THE CABINET appears in `F_MONO_S` ash for 1 s after a write. Nothing scrolls: the rule is a 2 x 3 grid or a card of at most 14 rows.

### 10.23 HALL OF FAME

```
+------------------------------------------------------------------------------+
|//////////////////////////////////////////////////////////////////////////////|
|                                                                              |
|            +------------------------------------------------------+          |
|            |  HALL OF FAME                                        |          |
|            |                                                      |          |
|            |  FASTEST KO        0:18   [2] SPINNY PANCAKE         |          |
|            |  LONGEST STREAK    5      [1] POLITE DOORSTOP        |          |
|            |                                                      |          |
|            |  WINS BY BUILD                                       |          |
|            |  ROLL OUT  [============] 12    TOAST   [=] 1        |          |
|            |  LAUNCH    [=========] 9        SPIN    [===] 3      |          |
|            |  CHOP      [====] 4             CUSTOM  [=======] 7  |          |
|            |  STOMP     [==] 2                                    |          |
|            |                                                      |          |
|            |  KOS BY CAUSE                                        |          |
|            |  HP ZERO 20   FLIP 6   PIT 4   ROOF 2   COUNT 3   DECK 1        |
|            |                                                      |          |
|            |  MATCHES 41                                          |          |
|            |                                                      |          |
|            |  HOLD B 3 S: WIPE   (ring fills around the B cap)    |          |
|            |  [A] BACK                                            |          |
|            +------------------------------------------------------+          |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|//////////////////////////////////////////////////////////////////////////////|
+------------------------------------------------------------------------------+
```

Legend: one card 880 x 480 at (200, 96). Title row HALL OF FAME `F_H2` bone; two record rows `F_MONO_L` with the time or count, the seat chip in the recorded seat colour and the recorded bot name; WINS BY BUILD as 7 short bars (6 presets plus CUSTOM) scaled to the largest; KOS BY CAUSE as one `F_MONO` line; MATCHES. HOLD B 3 S: WIPE clears the hall of fame (not settings) with a fill ring and SFX_MENU_BACK. No text entry anywhere. Empty records read NONE YET.

### 10.24 Controls card

```
+------------------------------------------------------------------------------+
|                                                                              |
|      [DRIVE]-----\                                        /----[WEAPON]      |
|       left stick  \        _______________________       /      A or RT      |
|                    \      /                       \     /                    |
|                   ( L )  |    bone pad outline     |  (Y) (B)----[BRAKE]     |
|                    /      \_______________________/  (X) (A)      B          |
|   [BOOST]---------/            |  START  |              \                    |
|    L or LT                     +---------+               \----[TAUNT]  X     |
|                                     |                                        |
|                                  [PAUSE]                                     |
|                                                                              |
|   FLIPPED? B TO RIGHT YOURSELF.  STUCK? OPS IS COMING.                       |
|   KEYBOARD: ARROWS, Z WEAPON, X BRAKE, A TAUNT, Q BOOST, ENTER PAUSE         |
+------------------------------------------------------------------------------+
```

Legend: a 640 x 360 card (this frame is 40 x 14 characters) in halves and full, a 320 x 180 variant with four callouts (DRIVE, WEAPON, BRAKE, BOOST) and no lines in quadrants; char background, a bone pad outline from `hud_rect`, `hud_circle` and `hud_line`, six callouts in hazard squares joined by rust lines, two words each. The keyboard line appears only for seat 0 under RetroArch or the browser. Shown on first join (3 s), under Y, in the pause menu, and as the last card of the rules flash.

### 10.25 Rules flash (HOW IT WORKS)

```
+------------------------------------------------------------------------------+
|                                                                              |
|         +------------------------------------------------------------+       |
|       |//////////////////////////////////////////////////////////////|       |
|         |                                                            |       |
|         |   [o][o][#][o][o][o][o]                HOW IT WORKS 3 OF 7 |       |
|         |                                                            |       |
|         |                                                            |       |
|         |                    HIT THE SIDE                            |       |
|         |                    (F_H2 bone)                             |       |
|         |                                                            |       |
|         |                     +----------------+                     |       |
|         |                     |      0.6       |   front plate, dim  |       |
|         |           1.0 --->  |                |  <--- 1.0           |       |
|         |                     |   bot (top)    |          +24        |       |
|         |                     |                |      (bone, rising) |       |
|         |                     +----------------+                     |       |
|         |                            1.0                             |       |
|         |                                                            |       |
|         |                                                            |       |
|         |   [A] NEXT   [B] SKIP                                      |       |
|         +------------------------------------------------------------+       |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
|                                                                              |
+------------------------------------------------------------------------------+
```

Legend: `card(190, 60, 900, 500, step, 7)` centred, tape header, step dots, a caption of at most three words in `F_H2` at y 250, a picture built from primitives animating along the card's local time, 2.5 s per card, A advances, B exits, 0.3 s wipe between cards. Shown from the title row and the lobby's Y, and once automatically before the first garage of a fresh save (`save.rules_seen`). Cards:

1. BUILD A BOT: three part glyphs slide in and add up (2 + 2 + 7), the tag bar fills to 11 and shakes at 21.
2. HOLD TO SPIN: an A keycap presses, a spinner ring fills from 0 to 1 over 2 s with the whine, then sparks on a second square.
3. HIT THE SIDE: a top-down bot with its front plate marked 0.6 and sides 1.0; a hit flashes +24 on the side, +14 on the front.
4. FLIPPED? B: a bot square tumbles, a 10 to 0 count shrinks beside it in rust, a B keycap presses, it rights itself.
5. HAZARDS HURT: a pit square with the amber tell blinking 1.5 s then opening; a hammer glyph swings; a bot dot drives round it; a hover glyph floats over the pit.
6. LAST BOT STANDING: four seat squares, three go grey one by one, the last gets the KO stinger; small text LOSERS GET THE HAMMERS.
7. The controls card.

No other tutorial text exists except the rebuild hint.

### 10.26 Pause

A 12 px frame in the pauser's seat colour around the whole screen, the 3D and particles frozen (timescale 0), PAUSED in `F_H1` centred at y 240, a 400 x 180 panel at (440, 320) with four `menu_row`s RESUME, CONTROLS, SETTINGS, QUIT TO LOBBY (QUIT needs a 1 s hold on A with a fill ring). Any seat can move the cursor and confirm; the music ducks to 0.3.
## 11. Audio (audio.h, audio_band.cpp, audio_pcm.c, tools/bake_music.cpp)

### 11.1 Shape

Music is baked to WAV at build time by a native tool that links the Bellows band, and mixed at runtime by `wc_pcm_mixer.h`, because the full band costs about 1 ms per frame on a Mac and that is most of a Pi's CPU budget. Bellows stays live only for the twelve SFX, the four spinner whine voices and the crowd bed. A `MUSIC=live` build keeps the band's songs live for composing on the Mac.

```c
/* audio.h: the whole contract between C and the band */
enum { SFX_MENU_MOVE, SFX_MENU_OK, SFX_MENU_BACK, SFX_LOCK, SFX_COUNT, SFX_HORN,
       SFX_HIT_LIGHT, SFX_HIT_HEAVY, SFX_CLASH, SFX_LAUNCH, SFX_KLAXON, SFX_KO, SFX_COUNT_ };
void audio_play(int sfx);                 /* one shot; a few have variants chosen by audio_play_var(sfx, var) */
void audio_play_var(int sfx, int var);    /* 0 default, 1 low variant, 2 tell variant, 3 short */
void audio_music(int mode);               /* 0 off, 1 GARAGE, 2 ARENA, 3 OVERTIME */
void audio_whine(int slot, float level, float pitch);   /* 4 continuous spinner voices; level 0 silences */
void audio_crowd(float level);            /* 0..1 bed */
void audio_slowmo(int on);                /* drums stem only while on, immediate */
void audio_tension(int on);               /* fades the lead stem in while any human bot is under 30 HP */
void audio_duck(float gain);              /* pause: 0.3 */
void audio_render(float *ring, uint32_t cap, uint32_t *cursor, uint32_t rate, double dt_ms);
```

`audio_render` mixes the PCM stems first (`wc_mixer_mix` into the ring region), then renders the Bellows SFX band over the same frames. Frames per render are `rate * dt_ms / 1000` with a fractional carry, capped at half the ring (AUDIO_CAP 8192, stereo f32). Every `I_BUZZ` gets an `I_BUZZ_OFF`; `ev_[]` is raised to 128 with its guard.

### 11.2 Songs (data in audio_band.cpp, rendered by tools/bake_music.cpp)

All songs are in E minor so `DiatonicBelow` works unchanged.

| Mode | Name | BPM | Key | Layers | Bars | Loop | Ships as |
|---|---|---|---|---|---|---|---|
| 1 GARAGE | Shelf Life | 100 | E minor | PAD, HYPNO (slow four), HAT sparse, SUB, LEAD on section B | 16 (A, B) | 0 | garage.wav |
| 2 ARENA | Red Tag | 150 | E minor | KICK, BREAK, ACID, HAT, STAB; LEAD and COUNTER on sections 3 and 5 | 32 (INTRO, BUILD, DROP, DROP+LEAD) | 1 | arena_base.wav, arena_lead.wav, arena_drums.wav |
| 3 OVERTIME | Doors Lock | 170 | E minor | DROP union plus ROLL every section, LEAD | 8 (RUSH) | 0 | overtime.wav |

Section data:

```
GARAGE  A: 8 bars "Em Em C G Em Em D Bm"  L_PAD|L_HYPNO|L_HAT|L_SUB
        B: 8 bars "Em Em C G Am Am D Em"  L_PAD|L_HYPNO|L_HAT|L_SUB|L_LEAD  LEAD_SHELF
ARENA   INTRO: 8 "Em Em Em Em C C D D"    L_KICK|L_HAT|L_ACID
        BUILD: 8 same chords              DROP without L_LEAD
        DROP:  8 same                     DROP|L_LEAD           LEAD_RED_TAG
        DROP2: 8 same                     DROP|L_LEAD|L_COUNTER LEAD_RED_TAG
OVERTIME RUSH: 8 "Em Em C D Em Em C D"    DROP|L_LEAD|L_STAB|L_ROLL  LEAD_RED_TAG

LEAD_SHELF   = "B4:8 -:8 | G4:4 A4:4 B4:8 | E5:8 -:8 | D5:4 B4:4 G4:8 | B4:8 -:8 | G4:4 A4:4 B4:8 | E5:4 D5:4 B4:4 A4:4 | G4:16 |"
LEAD_RED_TAG = "E5:2 -:2 E5:2 G5:2 -:2 A5:2 -:2 G5:2 | E5:2 -:2 D5:2 -:2 B4:4 D5:4 | E5:2 -:2 E5:2 G5:2 -:2 B5:2 -:2 C6:2 | B5:4 A5:4 G5:4 F#5:4 | E5:2 -:2 E5:2 G5:2 -:2 A5:2 -:2 G5:2 | E5:2 -:2 D5:2 -:2 B4:4 D5:4 | G5:2 -:2 A5:2 B5:2 -:2 D6:2 -:2 E6:2 | D6:4 B5:4 G5:4 E5:4 |"
```

Stems: `tools/bake_music.cpp` is compiled natively with `-DMUSIC_BAKE` and links audio_band.cpp; for each mode it calls `band.Init(24000)`, `SetMode`, and renders exactly `bars * 4 * samples_per_beat` frames so the loop joins without a click. ARENA is rendered three times with layer masks: base (everything but LEAD, COUNTER, KICK, BREAK, ROLL, HAT, PERC), drums (KICK, BREAK, ROLL, HAT, PERC only), lead (LEAD and COUNTER only), all the same length. Format 16 bit stereo 24 kHz (the mixer resamples with its 16.16 step): about 1.2 MB garage, 3 x 0.9 MB arena, 0.6 MB overtime, 4.4 MB total in the .wasc. Runtime: `audio_pcm.c` loads the five WAVs with `wc_mixer_load_wav` from assets, starts the three ARENA stems on the same frame with `wc_mixer_play(id, vol, 1)`, and drives gains with `wc_mixer_set_volume` through 6 frame ramps: base 1.0, drums 1.0, lead 0.0 until `audio_tension(1)` fades it to 1.0 over 0.5 s; `audio_slowmo(1)` ramps base and lead to 0.15 and drums to 1.0. Mode changes: GARAGE to ARENA on FIGHT (hard cut, all stems from frame 0), ARENA to OVERTIME at 15 s left (on the next beat: `audio_pcm.c` tracks the play position and waits for the next multiple of the beat length), to GARAGE on results with a 0.5 s fade.

### 11.3 SFX patches (Bellows, live)

Members: `VoicePool<Modal, 4> clang_` (material 0 bar, decay 0.6, strike_hardness 0.9, brightness 0.7) for metal hits with a random pitch so repeats never sound sampled; `Kick impact_` (decay 0.5, drive 4.0, click_tune 9) separate from any music; `VoicePool<Va, 4> spark_` (shape 1, detune 40, cutoff 6000, resonance 0.6, env_amount 3, f_decay 0.08, decay 0.12, sustain 0) for sparks and saw contact; `Va buzz_` with a mandatory BUZZ_OFF for the klaxon and horn; `VoicePool<Fm, 4> bells_` (4 op algorithm 3) for menu sounds and the KO gong at Mtof(43) with a long release; `Va whine_[4]` (shape 0 saw, cutoff 1800, resonance 0.3, drift 0.02, sustain 1) for the spinner whines, frequency `80 + pitch * 600` Hz, level ramped per span; `NoiseGen crowd_` pink through an Svf bandpass at 800 Hz with a 0.3 Hz tremolo, gain ramped toward `crowd_target_`. Rng labels "rtr::clang", "rtr::spark", "rtr::buzz", "rtr::bells", "rtr::crowd". Pad pool 3 voices, no plate reverb (a short delay only), `Saturator<1>`, `BELLOWS_FAST_MATH=1`, every `Mix` guarded by `Active()`. Pi budget for the live band: under 1 ms per frame.

| SFX | Events |
|---|---|
| MENU_MOVE | hat 900 Hz 0.25, bell Mtof(84) 0.3 |
| MENU_OK | bell Mtof(72) 0.6, +50 ms bell Mtof(79) 0.6; low variant Mtof(67) and Mtof(72) at 0.4 (pickup, taunt) |
| MENU_BACK | bell Mtof(67) 0.5, +60 ms bell Mtof(62) 0.5 |
| LOCK | impact 60 Hz 0.6, bell Mtof(76) 0.7, +40 ms bell Mtof(83) 0.7 |
| COUNT | bell Mtof(69) 0.8 (the FIGHT horn is HORN) |
| HORN | buzz Mtof(45) 1.0, +30 ms buzz Mtof(52), +600 ms BUZZ_OFF; low variant Mtof(38) with the OFF at 400 ms for OPS |
| HIT_LIGHT | spark Mtof(60 + rnd 12) 0.5, clang Mtof(70 + rnd 6) 0.4; low variant drops the clang (landing, wall) |
| HIT_HEAVY | impact 42 Hz 1.0, clang Mtof(55 + rnd 6) 0.9, spark 0.8, +20 ms clang Mtof(62) 0.5 |
| CLASH | impact 38 Hz 1.0, two clangs 30 ms apart 1.0, spark 1.0 at 0, 40 and 90 ms; at 0.6 for the spinner wall hit |
| LAUNCH | spark Mtof(48) 0.7 sweeping (two pushes rising), bell Mtof(88) 0.3 |
| KLAXON | buzz Mtof(50) 0.7, +150 ms BUZZ_OFF; tell variant twice at 0 and 400 ms at 0.5; short variant 80 ms; the inverted tick is a hat 1200 Hz |
| KO | impact 36 Hz 1.0, bell Mtof(43) 1.0 (gong, release 2.5 s), +80 ms clang Mtof(50) 0.8, +120 ms spark 0.6 |

After any patch change: `WAV=build/out.wav node tools/shot.mjs build/red-tag-rumble.wasc build/shots/wav 60,600 tools/scripts/2p-fight.json`, compare per stem RMS and the limiter; the sim is deterministic so the same script always produces the same hits at the same frames and the WAV is diffable.

## 12. Controls

W3C Standard Gamepad; the RetroArch core delivers the same bit layout and the keyboard arrives as pad 0 with arrows, Z (A), X (B), A (X), S (Y), Enter (Start), right Shift (Select), Q and W (L and R). Dead zone 0.18 radial; triggers digital at 0.5 (16384). The d-pad drives the same axes as the left stick everywhere. `WC_FLAG_KEYBOARD` is never set.

| Input | Arcade scheme (default) | Tank scheme (per seat) |
|---|---|---|
| left stick, d-pad | drive: Y throttle (reverse 0.8), X steer; HOVER and MELTY: world direction relative to the camera | left side power |
| right stick | camera nudge, 30 degrees, springs back; HOVER aim; turntable scrub in the garage | right side power |
| A or RT | weapon: hold for HORIZONTAL, DRUM, FLAME, LIFTER, MELTY; tap for FLIPPER, HAMMER, SPIKE, CRUSHER; tap toggles SAW | same |
| B | brake (hard stop, 0.3 s); self-right when inverted; hold 1 s in the lobby or garage to leave | same |
| X | taunt (0.5 s wiggle, +1 aggression); RANDOM BOT in the garage; OPS saw for the second eliminated player in a final | same |
| Y (hold) | control card on your viewport; HOW IT WORKS in the lobby; modifier for the Right stick steppers | same |
| L or LT | boost 1.5x for 0.8 s, 4 s cooldown (2 s red tagged), not on TREADS or LEG TRACKS | same |
| RT in the garage | test-fire the weapon on the turntable | same |
| Start | pause (fight), presets (garage), go (lobby) | same |
| Select | nothing (reserved) | same |

Menus: stick or d-pad moves, A selects, B backs, Start is go in the lobby and preset in the garage, X is RANDOM BOT in the garage. Hazard seat: stick picks, A fires. The control card is section 10.24.

## 13. Performance budget (Raspberry Pi 5 class, 60 fps target)

| Item | Budget | How |
|---|---|---|
| sim | 0.5 ms | 5 bodies, 25 pair tests, 12 hazards, 24 debris; no allocation; sin table |
| AI | 0.2 ms | 10 Hz think, 60 Hz PD steer, staggered |
| world_sync and three.c traversal | 1.5 ms | under 300 objects in the scene; dents are geometry swaps, not rebuilds; interpolation is 5 bodies |
| fx | 0.3 ms | 400 cube sparks, 160 smoke quads, 4 flash quads; two instanced meshes |
| HUD paint | 2.0 ms at HUD_DIV 1, 0.6 at 2 | strips, numbers, minimap, stingers; tape only on 24 px bands; dirty rect gating |
| HUD upload | 3.7 MB at 1280 x 720, 0.9 MB at 640 x 360 | HUD_DIV 2 under 540p; version bump only when dirty |
| audio | 1.0 ms | PCM stems (microseconds) plus the trimmed SFX band |
| draw calls | under 150 per view, under 400 per frame | auto instancing on wheels and panels; props grouped per cell; one spark mesh, one smoke mesh, one crowd mesh |
| triangles | under 15k per view, under 25k on screen | section 14.2 budgets |
| fill | 1280 x 720 once plus the HUD quad | splits do not add fill; no shadow maps; no render targets; two point lights |
| worst case | 4 player THE SHOP | render at 30 fps with the sim fixed at 60: when the measured frame exceeds 20 ms for 30 frames render every other frame (`G.render_half`), back when under 12 ms for 120; invisible in the UI |
| program compile | all at PH_BOOT | `t3_renderer_compile` per scene; `t3_renderer_generated_missing` empty on frame 1 |
| memory | 64 MB initial, growth allowed | music WAVs 4.4 MB, fonts, floor textures under 12 MB |
| resolution | `wc_negotiate_resolution(&host_info, &W, &H, 1280, 720)` in `wc_init` | the renderer, viewport rects and HUD_DIV follow W and H; 1080p is capped at 720p; 540p and 360p are honoured |

Measure on the box with the core's `wasmcart_perf_log` option and `t3_renderer_info(R)->calls` read after each view; `t3_renderer_set_gpu_timing` where V3D supports it.

## 14. Source layout, signatures, render plan, save data

### 14.1 Folder

```
red-tag-rumble/
  build.sh  compile_flags.txt  package.json  .gitignore  LICENSE  HANDOFF.md  README.md
  assets/fonts/*.ttf   assets/brand/hsl-mark-paths.json   assets/music/*.wav (baked, committed)
  src/
    main.c          host glue: wc_info, negotiate resolution, pads and seats, the frame (accumulator, timescale, steps, render,
                    HUD, audio), event drain into fx, audio, stingers, rumble and camera, save struct, hash log
    game.c/.h       evening state: seats, mode, builds, scores, bracket, phase machine, menu state (title, lobby, vote, rematch,
                    settings, hall, rules flash, pause), hazard seats, hall of fame updates
    parts.h         the catalogue: chassis, drive, weapon, armour, paint tables with stats, costs, flavour, adjectives and nouns;
                    build_t; budget rules; presets; random build; bot_name
    bot.c/.h        build_t to body_t and stats; the three bars; hint picker; dent geometry generation; part mesh builders
    sim.c/.h        match_t, sim_init, sim_step, sim_snapshot, sim_over, sim_hash; collision, weapons, hits, hazards, debris,
                    pickups, red tag, win conditions; rtr_sin table; no three.c, no HUD, no host
    arena.c/.h      the six arena_def_t tables with their ASCII maps as comments, tile rect lists, visibility tables, timing helpers
    ai.c/.h         perception ring, utility scorer, steering, difficulty, habits; drives OPS and the attract
    world.c/.h      three.c scenes: arenas, garage, bots, debris, particles, crowd, lights, textures, paint_floor; world_sync
    fx.c/.h         sparks, smoke, flashes, debris visuals on wall time; the hit stop freeze; the Hit Contract driver
    camera.c/.h     chase, ONE CAMERA, KO orbit, punch and shake, viewport rects per layout, wall push-out
    draw.c          the HUD layer: strips, stingers, numbers, minimap, every screen in section 10
    hud.c/.h        copied from DO-OCRACY with the additions in 10.3; glyphs.h
    content.h       every string (section 16)
    juice.h         every feel constant (section 5.4)
    audio.h         the C API (11.1)
    audio_band.cpp  Bellows SFX band plus the song data (songs live only with MUSIC=live or in the bake tool)
    audio_pcm.c     wc_pcm_mixer.h implementation, stems, mode, fades, beat alignment
    gen/rtr_gl.c    generated program table, committed
  tools/
    shot.mjs        the headless rig, extended (15.4)
    scripts/        2p-fight.json  4p-fight.json  garage-2p.json  title.json  menu.json
                    drive-bay.json  drive-laser.json  drive-pile.json  drive-dock.json  drive-shop.json  drive-roof.json
    smoke.c         pipeline smoke test, table symbol rtr_gl
    states.txt      15.3
    gen-logo.mjs  serve.mjs
    bake_music.cpp  native: links audio_band.cpp, renders each mode and stem to assets/music/*.wav
    balance.c       native: links sim.c, ai.c, arena.c, bot.c, parts.h; round robin, check-arenas, replay
    lint.sh         runs the house lint script over content.h, parts.h, HANDOFF.md and README.md
  third_party/three.c (submodule bd2f28c)  third_party/wasmcart (submodule c38b39d)  third_party/bellows  third_party/stb_truetype.h
  web/index.html  web/vendor/
  .github/workflows/pages.yml
```

sim.c, bot.c, arena.c, ai.c, parts.h, content.h and juice.h include nothing from three.c, hud.h or wasmcart and compile natively with `cc`; that line is what the headless build and the replay depend on.

### 14.2 Render plan (world.c, fx.c)

Lights, fixed for every scene: one hemisphere (sky bone 0xece3d3, ground char 0x15120f, intensity 1.2), one directional sun (colour per arena, intensity 2.0, no shadow), two point lights (amber and teal accents per arena, intensity per arena, 0 where an arena wants only one). The light vector is `l1201` everywhere, no shadow casters, no `s` suffix, no fog. No shadow maps: every bot, OPS and every debris piece has a blob quad (circle geometry, basic material 0x000000 at opacity 0.35, transparent, at y 0.01, scaled by `1 - y/3`). The garage and the title use the same three lights.

```
scene
  arena_root[6]      built at boot, only the voted one attached to the scene
    floor            plane, standard+map: the painted texture
    walls            boxes or lexan planes (THE ROOF omits two); rails with caution tape texture (basic+map)
    hazards[]        group per hazard: base mesh, moving part, emissive tell mesh (emissive colour is a uniform, not a map)
    props[]          grouped by 4 x 4 cell (16 groups, visible flag per viewport from vis[])
    ops_gate         lexan L that slides when OPS wakes
    crowd            one t3_instanced_mesh of 240 flat quads in two rows, basic+map+trans, 3 frame atlas, tinted by the arena light
  bots[5]
    root group       position and heading from the sim interpolated by alpha; rotated 180 about local z when inverted
      chassis        box, lathe (SHELL), or 6 thin boxes (FRAME) in the paint body colour
      band           a 0.08 u ring box in the seat colour, the one colour that never changes
      number[3]      basic+map quads with the seat digit, top and both sides
      panels[6]      each a mesh whose geometry is swapped among 4 states (clean, dent1, dent2, dent3); hidden at state 4
      drive          wheels (shared cylinder 12 segments, auto instanced), treads (two boxes with a scrolling stripe map), hover skirt (torus), walker legs (6 boxes, gait animated), leg track pods, melty body (the chassis spins in render)
      weapon         pivot group: disc plus teeth (HORIZONTAL), drum, hammer arm, flipper plate, lifter forks, saw disc, flame nozzle plus emissive cone, crusher jaws, spike rod
      spin_ring[2]   basic+trans rings at the weapon radius, alpha spin * 160 and a 15 degree copy at 50
      led            MELTY heading LED, emissive teal, blinks 4 Hz
      blob
  debris             one t3_instanced_mesh of 24 boxes, basic+inst+icol, matrix and colour per instance, unused instances scaled to 0;
                     pickup rings are 24 basic+trans ring meshes toggled visible
  fx_sparks          one t3_instanced_mesh of 400 unit cubes, basic+inst+icol; no billboarding needed
  fx_smoke           one t3_instanced_mesh of 160 horizontal quads, basic+inst+icol+trans; no billboarding needed
  fx_flash[4]        four camera-facing quads (one per viewport), basic additive white; the only billboards
  garage_root        4 turntables, pegboard wall, shelf props, a tag board; attached in the garage, lobby, title and results
hud_scene            the full screen quad, basic+map+trans+nearest
```

Dents are pre-authored by code: `bot_dent_geometries` builds the three dented states per panel from the clean box by pushing 2 to 4 vertex clusters in by 0.02, 0.05 and 0.09, seeded per panel. Scorch and scrape marks are up to 4 small basic+trans quads parented to the panel. Bot assembly budget: chassis under 60 triangles, each panel under 40, wheels 48, weapons under 200, so a bot is under 900 triangles with every part; four bots plus OPS under 5000; arena under 8000 including props; crowd 480; particles 1200; total under 15k per view. Every lit material is MeshStandard (roughness 0.6, metalness 0.2 for painted panels; 0.3 and 0.8 for steel and titanium; 0.9 and 0 for plywood). The HUD quad is the only nearest filtered map.

Frame, in `wc_render`:

```
dt = clamp(delta_ms / 1000, 0, 0.05); acc += dt * G.timescale
while (acc >= DT && steps < 4) { feed pads and AI into bots[].in; sim_step(&M); acc -= DT; steps++ }
drain M.ev[]: fx (sparks, flash, debris), numbers, stingers, SFX, rumble, camera punch and shake, hall of fame
world_sync(&M, acc / DT): bot roots interpolated, panel geometry, weapon poses, hazard poses, debris matrices, blobs
cam_update(&M, dt)
t3_renderer_set_scissor_test(R, true); t3_renderer_set_auto_clear(R, true)
for each viewport: world_set_cell_visibility(cell of cam[i]); face fx_flash[i] to cam[i]; aspect; viewport; scissor; render
t3_renderer_set_scissor_test(R, false); t3_renderer_set_viewport(R, 0, 0, W, H); t3_renderer_set_auto_clear(R, false)
draw_hud(dt); if (hud dirty) hud_tex->version++
t3_renderer_render(R, hud_scene, hud_cam)
audio_render(ring, AUDIO_CAP, &cursor, rate, delta_ms)
every 60 sim frames: wc_log the sim hash; every 120 render frames: wc_log t3_renderer_generated_missing
```

### 14.3 Key signatures

```c
/* parts.h */
typedef struct { uint8_t chassis, drive, weapon, weapon2, armour, paint, decal; } build_t;
int  build_cost(const build_t*);  int build_budget(const build_t*);  int build_legal(const build_t*, int players);
int  option_legal(const build_t*, int row, int option, int players, const char **reason);
void build_preset(build_t*, int i); void build_random(build_t*, uint32_t *rng, int players);
void build_bars(const build_t*, float *speed, float *hit, float *tough);
void bot_name(char out[24], const build_t*);
/* sim.h */
void sim_init(match_t*, const match_setup_t*); void sim_step(match_t*); void sim_snapshot(const match_t*, match_t*);
int  sim_over(const match_t*); uint32_t sim_hash(const match_t*);
float rtr_sin(float), rtr_cos(float), rtr_atan2(float, float);
/* bot.h */
void bot_body_from_build(body_t*, const build_t*); float bot_hp_max(const build_t*);
int  bot_hint(const bot_t*, const match_t*, char out[48]);
/* arena.h */
extern const arena_def_t ARENAS[6]; int arena_cell(const arena_def_t*, float x, float z);
float arena_floor_y(const arena_def_t*, float x, float z); int hazard_phase(const hazard_def_t*, float haz_clock, float *phase_t);
/* ai.h */
void ai_init(ai_t*, int difficulty, int seat); void ai_think(ai_t*, const match_t*, pad_state_t *out);  /* every frame; thinks every 6th */
void ai_pick_build(build_t*, int difficulty, const build_t *human_last, const uint8_t habits[8], uint32_t *rng);
/* world.h */
void world_init(void); void world_attach_arena(int); void world_attach_garage(void);
void world_sync(const match_t*, float alpha); void world_bot_build(int slot, const build_t*, uint32_t seat_colour);
void world_set_cell_visibility(int cell, const arena_def_t*); void paint_floor(const arena_def_t*, int w, int h, uint8_t *rgba);
/* fx.h */
void fx_hit(const event_t*, int band); void fx_update(float dt, int frozen); void fx_face(int viewport);
/* camera.h */
void cam_layout(int players, int one_camera, rect_t out[4], int *n);
void cam_update(const match_t*, float dt); void cam_punch(int seat, float nx, float nz, float amount);
void cam_shake(int seat, float amp); void cam_ko(int victim, int victor); int cam_enemy_offscreen(int seat, float *edge_x, float *edge_y);
/* draw.c */
void draw_hud(float dt);    /* dispatches on game phase; per viewport strips project with cam[i] */
void hud_strip(const rect_t *vp, const bot_t*, int compact); void stinger(int viewport, const char *s, uint32_t seat_colour);
```

### 14.4 Save struct

```c
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
static save_t save;          /* info.save_ptr = &save; info.save_size = sizeof save; set by hand after the info fields */
```

All zero means empty; a bad magic, version or crc resets to defaults (split AUTO, one camera AUTO, fight 75, comeback AUTO, shake 2, hit stop on, stinger big, sfx 8, music 6, colourblind off, rumble on, ai MEDIUM, rules unseen, arcade sticks). Written whole whenever a setting changes, a build locks, or results are shown. `shot.mjs` never passes save data, so headless runs start empty.

## 15. Build, states, flags, test rig, balance

### 15.1 Folder setup

Follow section 5 of the platform map exactly. Do not copy qrcodegen or config.json.

### 15.2 build.sh changes (relative to DO-OCRACY's build.sh)

| Line | DO-OCRACY | RED TAG RUMBLE |
|---|---|---|
| 2 | `# Builds DO-OCRACY as a wasmcart GL cart (build/do-ocracy.wasc).` | `# Builds RED TAG RUMBLE as a wasmcart GL cart (build/red-tag-rumble.wasc).` |
| 46 | `--name hsx` | `--name rtr` (and the missing-file test on `src/gen/rtr_gl.c`) |
| 53 | `SRCS="$ROOT/tools/smoke.c $ROOT/src/gen/hsx_gl.c"` | `SRCS="$ROOT/tools/smoke.c $ROOT/src/gen/rtr_gl.c"` |
| 55 | `SRCS="$(ls "$ROOT"/src/*.c) $ROOT/src/gen/hsx_gl.c $ROOT/third_party/qrcodegen/qrcodegen.c"; NAME=do-ocracy; GAMEF="-DWC_USE_GL -DWC_USE_NET_PEER"` | `SRCS="$(ls "$ROOT"/src/*.c) $ROOT/src/gen/rtr_gl.c"; NAME=red-tag-rumble; GAMEF="-DWC_USE_GL"` plus `-DMUSIC_LIVE=1` when `MUSIC=live` and `-DHUD_DIV=2` when `HUD_DIV=2` |
| 61 | `em++ -c "$ROOT/src/audio_bellows.cpp" ...` | `em++ -c "$ROOT/src/audio_band.cpp" -o "$OUT/obj/game/audio_band.o" $OPT -std=c++17 -fno-exceptions -fno-rtti -DBELLOWS_FAST_MATH=1 -I "$ROOT/third_party/bellows" -I "$ROOT/src"` |
| 67 | `-I "$ROOT/third_party/qrcodegen"` | removed |
| 79 to 84 | the config.json block | replaced by `cp "$ROOT"/assets/music/*.wav "$OUT/assets/"` (skipped when `MUSIC=live`) |
| 88 to 90 | `--name "DO-OCRACY" --version 0.1.0 --width 1280 --height 720 --players 4 --controls dpad,left_stick,a,b,x,start --ws relay.clasp.to` | `--name "RED TAG RUMBLE" --version 0.1.0 --width 1280 --height 720 --players 4 --controls dpad,left_stick,right_stick,a,b,x,y,l,r,left_trigger,right_trigger,start` and no `--ws` |
| 95 | `cp "$OUT/$NAME.wasc" "$ROOT/web/do-ocracy.wasc"` | `cp "$OUT/$NAME.wasc" "$ROOT/web/red-tag-rumble.wasc"` |
| new | | `./build.sh balance`: `cc -O2 -std=gnu11 -DRTR_HEADLESS -I src src/sim.c src/bot.c src/arena.c src/ai.c tools/balance.c -lm -o build/balance` |
| new | | `./build.sh music`: `clang++ -std=c++17 -O2 -DMUSIC_BAKE -I third_party/bellows -I src tools/bake_music.cpp src/audio_band.cpp -o build/bake_music && build/bake_music assets/music` |
| new | | `./build.sh lint`: `tools/lint.sh` |

Keep `check-imports.mjs` (the cart may import only `gl.*`, `env.wc_*`, `emscripten_notify_memory_growth` and WASI). `tools/smoke.c` declares `extern const struct t3_gen_table rtr_gl;`. `compile_flags.txt` drops `-DWC_USE_NET_PEER` and the qrcodegen include. `package.json` name `red-tag-rumble`. `web/index.html` loads `red-tag-rumble.wasc`.

### 15.3 tools/states.txt starting list

```
standard+l1201
standard+instbig+l1201
standard+map+l1201
standard+map+instbig+l1201
standard+trans+l1201
basic
basic+instbig
basic+map
basic+map+trans
basic+trans
basic+trans+instbig
basic+inst+icol
basic+instbig+icol
basic+inst+icol+trans
basic+instbig+icol+trans
basic+map+trans+nearest
```

Whether an explicit `t3_instanced_mesh` reports `inst` or `instbig`, and whether additive blending adds a token, is decided by the first headless run: `t3_renderer_generated_missing` names the exact state and the unused lines are deleted. Run `REGEN_STATES=1 ./build.sh` once after the list settles and commit `src/gen/rtr_gl.c`.

### 15.4 wc_info flags and shot.mjs

`info.flags = WC_FLAG_AUDIO_F32 | WC_FLAG_DETERMINISTIC`. No `WC_FLAG_KEYBOARD`, no `WC_FLAG_NET_PEER`. Export `wc_set_seed` and feed it into the match RNG seed. `gpu_api 1`. `save_ptr` and `save_size` set after the other fields.

shot.mjs changes:

```js
const BTN = { A: 1, B: 2, X: 4, Y: 8, L: 16, R: 32, START: 64, SELECT: 128,
              UP: 256, DOWN: 512, LEFT: 1024, RIGHT: 2048, L3: 4096, R3: 8192 };
const pads = [0, 1, 2, 3].map((i) => ({ connected: process.env.STATION ? i === 0 : true, buttons: 0,
              leftX: 0, leftY: 0, rightX: 0, rightY: 0, leftTrigger: 0, rightTrigger: 0 }));
// script fields: from, to, pad, buttons[], lx, ly, rx, ry, lt, rt (triggers 0..32767); frame ranges inclusive
```

`STATION=1` marks only pad 0 connected, which is what the RetroArch core reports; the 4p script run under `STATION=1` must still seat four players. `CartHost._writePads` reads exactly those property names.

### 15.5 Scripts

Frames at 60 Hz. The title accepts input from frame 1; the attract starts at 90. Lobby seats on A; READY is a second A; Start goes to the garage; the first lock in an evening is an A held 60 frames.

tools/scripts/2p-fight.json:

```json
[
  { "from": 30,  "to": 32,  "pad": 0, "buttons": ["A"] },
  { "from": 60,  "to": 62,  "pad": 0, "buttons": ["A"] },
  { "from": 70,  "to": 72,  "pad": 1, "buttons": ["A"] },
  { "from": 90,  "to": 92,  "pad": 0, "buttons": ["A"] },
  { "from": 94,  "to": 96,  "pad": 1, "buttons": ["A"] },
  { "from": 110, "to": 112, "pad": 0, "buttons": ["START"] },
  { "from": 200, "to": 202, "pad": 1, "buttons": ["START"] },
  { "from": 230, "to": 300, "pad": 0, "buttons": ["A"] },
  { "from": 230, "to": 300, "pad": 1, "buttons": ["A"] },
  { "from": 330, "to": 332, "pad": 0, "buttons": ["RIGHT"] },
  { "from": 330, "to": 332, "pad": 1, "buttons": ["RIGHT"] },
  { "from": 600, "to": 800, "pad": 0, "ly": -32767 },
  { "from": 600, "to": 800, "pad": 1, "ly": -32767 },
  { "from": 800, "to": 1500, "pad": 0, "buttons": ["A"], "lx": 6000, "ly": -32767 },
  { "from": 800, "to": 1500, "pad": 1, "buttons": ["A"], "lx": -6000, "ly": -32767 },
  { "from": 900, "to": 1500, "pad": 1, "lt": 32767 },
  { "from": 1500, "to": 1560, "pad": 0, "buttons": ["B"] },
  { "from": 1560, "to": 2400, "pad": 0, "ly": -32767, "rt": 32767 },
  { "from": 1560, "to": 2400, "pad": 1, "ly": 20000, "lx": -20000, "buttons": ["A"] }
]
```

Seat 1 keeps ROLL OUT (preset 1 by seat), seat 2 presses Start once and so cycles LAUNCH to CHOP; both lock; both vote THE LASER ROOM (one RIGHT from THE BAY); both drive forward; seat 2 holds the spinner and boosts. Shots: 120 (lobby, two seats), 260 (garage, two panels), 340 (vote), 560 (countdown), 900 (early fight), 1500 (mid fight with hits), 2400 (late fight or results). Run: `node tools/shot.mjs build/red-tag-rumble.wasc build/shots/s2 120,260,340,560,900,1500,2400 tools/scripts/2p-fight.json`.

tools/scripts/4p-fight.json: the same opening with pads 0 to 3 each pressing A at 60, 66, 72, 78 and again at 90 to 108 for READY, Start at 120; every pad holding A 230 to 300 (seat presets 1 to 4); the vote left alone (THE BAY); four stick drives toward the centre (pad 0 ly -32767; pad 1 ly -32767 lx -8000; pad 2 ly -32767 lx 8000; pad 3 ly -32767) with A held from 700; pad 2 pressing X every 120 frames from 900 (taunts); pad 3 pressing B at 1000 (brake); pad 1 holding L from 1100 (boost). Shots: 260 (four garage quadrants), 560, 900, 1500, 2400. Run it twice, once with `STATION=1`.

tools/scripts/garage-2p.json: the opening through the lobby, then pad 0 pressing DOWN twice and RIGHT every 30 frames (cycling the WEAPON row), pad 1 pressing X at 250 (RANDOM BOT) and START at 300 (preset). Shots at 262, 292, 322 to see the bars animate and the slot machine.

tools/scripts/title.json: empty; shots at 60 and 700 (attract fight visible to the right of the scrim); a `STATION=1` run proves pads 1 to 3 are not required to be connected to be seated (the 4p script under `STATION=1` must still seat four).

tools/scripts/menu.json: walks SETTINGS (DOWN x4, RIGHT, B), HALL OF FAME (shot), and the rules flash (A seven times, a shot per card).

tools/scripts/drive-<arena>.json, one per arena (bay, laser, pile, dock, shop, roof): one seat, SOLO with the practice bot set to EASY and parked by a `PRACTICE=off` script flag that main.c honours only under the rig, a lap along the walls and a crossing of every hazard for 90 s (5400 frames) as a sequence of stick segments, a shot every 300 frames. The console must show no `[stuck]` nudges and no missing states before an arena ships.

### 15.6 Deploy

Copy `web/red-tag-rumble.wasc` to `/userdata/roms/wasmcart/` on the Batocera box, restart EmulationStation; `batocera.conf` carries `wasmcart.gfxbackend=gl` and `wasmcart.rewind=0`. Logs: `wasmcart.log` beside the cart and `/userdata/system/logs/es_launch_stderr.log`. Set the Internal Resolution option to 960x540 for the first Pi run and compare the perf log at 720p.

### 15.7 Balance tool

```sh
build/balance --robin 20 [arenas=all|bay|laser|pile|dock|shop|roof] [hazards=on|off|both]
build/balance --check-arenas
build/balance --replay tools/scripts/2p-fight.json --setup "<the [sim] setup line the cart logged>"
```

`--robin` runs every preset pair with two MEDIUM AIs, both spawn sides, 20 matches each, prints a 6 x 6 win matrix per arena and overall, and exits non zero on the guardrails in section 8.6. Native, about 50k steps per second, under a minute for the full matrix; it runs in CI before packing. `--check-arenas` is section 6.6. `--replay` rebuilds the match from the setup line the cart logs on `sim_init` (`[sim] setup mode=0 arena=1 seed=7 start=612 b0=1,3,1,4 b1=2,1,9,2 ai=0,0`), feeds the script's pad values from frame `start` onward, and prints `[sim] f=%d h=%08x` every 60 frames; the lines must match the cart's log byte for byte.

## 16. content.h (every string, one file, one lint)

Every string is ASCII caps, under its stated length, no dashes, no names, roles only. `tools/lint.sh` runs the house lint over content.h and parts.h. Buffers: stinger 24, speech 32, hint 48, award 44, award sub 48, tagline 56, tip 48, blurb 32, hook 24, cause 48, bot name 24.

```c
/* stingers, under 24 chars, 4 to 6 per trigger; the SFX is in the comment */
static const char *ST_FIGHT[]    = { "FIGHT", "GO BUILD SOMETHING", "DOORS LOCK AT TEN", "BE EXCELLENT. OR NOT", "MIND THE PIT" };            /* HORN */
static const char *ST_BIGHIT[]   = { "BIG HIT", "THAT WAS LOAD BEARING", "RED TAG THAT", "INSURANCE SAYS NO", "FREE PILE IT", "OUCH" };      /* HIT_HEAVY */
static const char *ST_CLASH[]    = { "CLASH", "SPARKS ARE FREE", "BOTH OF YOU STOP", "SAFETY GLASSES ON", "WHO SIGNED OFF ON THIS" };        /* CLASH */
static const char *ST_AIRBORNE[] = { "AIRBORNE", "NOT A FLYING ROBOT", "CHECK YOUR BALANCE", "GRAVITY IS A TOOL", "THAT IS NOT A DRONE" };   /* LAUNCH */
static const char *ST_INVERTED[] = { "INVERTED", "WRONG WAY UP", "TEN SECONDS", "WIGGLE SOMETHING", "SHOW SOME MOVEMENT" };                  /* KLAXON */
static const char *ST_PIT[]      = { "PITTED", "INTO THE PIT", "DOWN TO STORAGE", "THAT IS NOT A FLOOR", "WE WILL FIND IT LATER" };          /* KO */
static const char *ST_EDGE[]     = { "OFF THE ROOF", "NO WALL THERE", "THE ALLEY HAS IT NOW", "GRAVITY WINS AGAIN" };                        /* KO */
static const char *ST_DECK[]     = { "DECK COUNT", "COME DOWN FROM THERE", "HIGH GROUND. TEN.", "THAT IS NOT A SHELF" };                     /* KLAXON */
static const char *ST_DUCK[]     = { "DUCK", "LASER SAFETY FIRST", "EYES ONLY FOR LOW BOTS", "MIND THE BEAM" };                              /* KLAXON */
static const char *ST_KO[]       = { "KO", "KNOCKOUT", "PARTS BIN", "CALL IT", "IT IS PARTS NOW", "SHELF IT" };                               /* KO */
static const char *ST_COUNT[]    = { "COUNTED OUT", "NO MOVEMENT. OUT.", "NAP TIME IS OVER", "TEN. DONE." };                                 /* KO */
static const char *ST_OPS[]      = { "OPS INBOUND", "CLEAR THE AISLE", "OPS HAS NOTES", "OPERATIONS IS COMING" };                             /* HORN low */
static const char *ST_OPS_OUT[]  = { "OPS OUT", "BACK TO THE DESK", "CARRY ON" };                                                            /* MENU_BACK */
static const char *ST_TIME[]     = { "TIME", "DOORS LOCK", "JUDGES", "THAT IS TEN PM" };                                                      /* HORN */
static const char *ST_REDTAG[]   = { "RED TAGGED", "14 DAYS TO LIVE", "MARKED FOR THE PILE", "IT IS ANGRY NOW" };                             /* KLAXON tell */
static const char *ST_PICKUP[]   = { "FREE PILE", "ONE PERSON'S PANEL", "STILL GOOD" };                                                      /* MENU_OK low */
static const char *ST_VACUUM     = "NOT A VACUUM";                                                                                           /* MENU_BACK, once */
static const char *ST_HILL[]     = { "HILL MOVED", "NEW HILL", "OVER THERE" };                                                               /* MENU_MOVE */
/* KO causes, under 16, shown under K.O. and as the placement word */
static const char *KO_CAUSE[]    = { "HP ZERO", "KO BY FLIP", "PITTED", "OFF THE ROOF", "COUNTED OUT", "DECK COUNT" };
/* results cause lines, under 48, chosen by cause (two for judges) */
static const char *KO_CAUSE_LINES[] = { "HP ZERO. IT HAD A GOOD RUN.", "KO BY FLIP. TURNS OUT GEOMETRY IS A WEAPON.", "PITTED. CHECK THE BASEMENT.",
  "OFF THE ROOF. NOT COVERED BY INSURANCE.", "COUNTED OUT. WE WAITED. WE REALLY DID.", "DECK COUNT. THE HIGH GROUND WAS A TRAP.",
  "JUDGES. THE UNDERDOG RULE APPLIED.", "JUDGES. THE DAMAGE BAR DOES NOT LIE." };
/* OPS speech, under 32 */
static const char *OPS_LINES[]   = { "CLEAR THE AISLE", "THAT IS NOT WHERE THAT GOES", "I AM NOT MAD. I AM OPERATIONS", "WHO LEFT THIS HERE", "THIS IS A WALKWAY", "DID YOU SIGN THE SHEET" };
/* title taglines, under 56 */
static const char *TAGLINES[]    = { "FOURTEEN DAYS. THEN IT IS FREE. OR IT IS A BOT.", "BUILT FROM OTHER PEOPLE'S UNFINISHED PROJECTS.", "A JANITORIAL CO-OP WITH AN ARENA ATTACHED.",
  "NEVER TELL SOMEONE NOT TO MAKE. A WEAPON.", "THERE IS ONLY ONE PLACE TO USE A BANDSAW. HERE.", "TWENTY TAGS. ZERO REFUNDS.", "OPS WOULD LIKE THE AISLE CLEAR.", "NO BRAKE CLEANER IN THE ARENA." };
/* attract tips, under 48 */
static const char *ATTRACT_TIPS[] = { "HOLD A TO SPIN UP. LET GO TO KEEP SPIN.", "B RIGHTS A FLIPPED HAMMER OR FLIPPER.", "HOVER FLOATS OVER SAWS. AND CANNOT PUSH.",
  "WALKERS GET 4 EXTRA TAGS AND STEP OVER PITS.", "RED TAG: UNDER 30 HP YOU HIT HARDER FOR 8 S.", "DROPPED PANELS ARE HP. DRIVE OVER THEM.", "OPS WAKES IF NOBODY HITS ANYONE FOR 12 S." };
/* mode blurbs, under 32, lobby */
static const char *MODE_BLURB[]  = { "1 TO 4. HP ZERO. LAST ONE UP.", "NO HP. FLIP OR FALL.", "LOSERS GET THE HAMMERS.", "HOLD THE SQUARE. 30 POINTS.", "2V2. FRIENDLY FIRE IS HALF.", "3 FIGHTS. NOBODY SITS OUT." };
/* arena hooks, under 24, vote cards */
static const char *ARENA_BLURB[] = { "PIT. TWO HAMMERS.", "SAWS. FIRE. A LASER.", "A MOUND. SCREWS.", "A DECK. RAIN. A GAP.", "BENCHES THAT BREAK.", "NO RAILS. WIND." };
/* garage hints, under 48 */
static const char *HINT_FLIPPED  = "FLIPPED %d TIMES: TRY LOWBOY OR SIX WHEEL";
static const char *HINT_SPINNER  = "A SPINNER ATE YOU: STEEL IS FOUR TAGS";
static const char *HINT_HAZARD   = "PUSHED INTO HAZARDS TWICE: TREADS DO NOT MOVE";
static const char *HINT_COUNTED  = "COUNTED OUT: KEEP MOVING. WIGGLE COUNTS";
static const char *HINT_CHEAP    = "YOU LEFT %d TAGS ON THE SHELF";
static const char *HINT_WON      = "YOU WON. CHANGE NOTHING. OR EVERYTHING.";
static const char *HINT_TIPS[]   = { "WEDGES WIN BY NOT HAVING A WEAPON", "HOVER IGNORES THE FLOOR. ALL OF IT", "HAMMERS LOVE INVERTED BOTS", "SPIN DOWN AND YOUR WEAPON IS A TARGET",
  "TAUNTING IS A JUDGING CRITERION. REALLY", "THE PIT OPENS AT ONE MINUTE LEFT" };
/* awards, one per player, priority order, name under 44, sub under 48 */
static const struct { const char *name, *sub; } AWARDS[] = {
  { "THE DOORSTOP DID IT",                "WON WITH NO WEAPON. ON PURPOSE." },            /* won with the WEDGE weapon */
  { "GRAVITY ENTHUSIAST",                 "MOST FLIPS LANDED." },                         /* most flips landed on others */
  { "SPINNER WITH A CEILING FAN COMPLEX", "MOST SPINNER DAMAGE." },                       /* most spinner damage */
  { "RED TAG HERO",                       "WON AFTER THE RED TAG." },                     /* won while or after red tag */
  { "LEFT THE CHAT",                      "RING OUT OR PIT. GONE." },                     /* KO by pit or edge */
  { "TAUNTED THE WRONG BOT",              "TAUNTED. THEN ATE 30." },                      /* taunt then a 30 plus hit within 60 frames */
  { "THE FLOOR IS LOAD BEARING",          "MOST TIME SPENT PUSHING." },                   /* most push frames */
  { "JANITOR",                            "MOST PANELS PICKED UP." },                     /* most pickups */
  { "SCARY BUT MILD",                     "MOST BURN. LEAST DAMAGE." },                   /* most burn dealt, least damage */
  { "DID NOT READ THE SIGN",              "MOST HAZARD DAMAGE TAKEN." },                  /* most hazard damage taken */
  { "WALL ENTHUSIAST",                    "MOST WALL HITS." },                            /* most wall hits */
  { "ARRIVED EVENTUALLY",                 "SLOWEST BUILD. STILL HERE." },                 /* slowest build, survived to time */
  { "SHELF STABLE",                       "UNDER 12 TAGS. SURVIVED." },                   /* under 12 tags, survived */
  { "ATTENDED",                           "WAS PRESENT." },                               /* fallback */
};
/* judges, under 48 */
static const char *JUDGE_UNDERDOG = "UNDERDOG RULE";
static const char *JUDGE_TIE_COST = "TIE. CHEAPER BOT WINS. THAT IS THE RULE.";
static const char *JUDGE_TIE_FLIP = "TIE. FEWER FLIPS WINS.";
static const char *JUDGE_DRAW     = "DRAW. NOBODY TELL THE TREASURER.";
/* rules flash captions, three words or fewer */
static const char *FLASH_CAPTION[] = { "BUILD A BOT", "HOLD TO SPIN", "HIT THE SIDE", "FLIPPED? B", "HAZARDS HURT", "LAST BOT STANDING", "CONTROLS" };
static const char *FLASH_SMALL_6   = "LOSERS GET THE HAMMERS";
/* names */
static const char *WEAPON_ADJ[]  = { "HUMBLE", "POLITE", "POINTY", "HELPFUL", "GRINDY", "TOASTY", "THUMPY", "YEETING", "DRUMMY", "SPINNY", "CLINGY" };   /* WP_NONE..WP_CRUSHER */
static const char *MELTY_ADJ     = "DIZZY";
static const char *CHASSIS_NOUN[] = { "BOX", "DOORSTOP", "PANCAKE", "BOWL", "SKELETON", "LUNCHBOX" };
/* control card */
static const char *CARD_LINE      = "FLIPPED? B TO RIGHT YOURSELF.  STUCK? OPS IS COMING.";
static const char *CARD_KEYS      = "KEYBOARD: ARROWS, Z WEAPON, X BRAKE, A TAUNT, Q BOOST, ENTER PAUSE";
/* placement words, under 10 */
static const char *PLACE_WORD[]   = { "WINNER", "HP ZERO", "FLIPPED", "PITTED", "OFF ROOF", "COUNTED", "DECK", "TIMED OUT" };
```

Part names, short names (9 characters for quadrant rows), flavour lines and preset blurbs live in parts.h beside their stats and are included in the lint pass. Humour placement rule: jokes live only in part flavour lines, stingers, the rebuild hint, awards and OPS lines. Nothing else talks.
## 17. Work breakdown

Estimates are lines of new or changed code. Items are in build order; the feel gate at item 7 is a hard stop.

| # | Item | Files and functions | Lines |
|---|---|---|---|
| 1 | Folder, submodules, copies, build.sh edits, smoke cart with `rtr_gl`, first `REGEN_STATES` build, `wc_negotiate_resolution`, two cubes in halves and quadrants with the split recipe, a screenshot | build.sh, compile_flags.txt, package.json, tools/smoke.c, src/main.c (`wc_init`, `wc_render`, viewport loop), tools/states.txt, web/index.html | 320 |
| 2 | parts.h catalogue and the bars; bot names; legality | parts.h (`build_cost`, `build_budget`, `build_legal`, `option_legal`, `build_preset`, `build_random`, `build_bars`, `bot_name`), tools/test_parts.c printing every preset's cost, bars and name | 420 |
| 3 | sim.c core: bodies, sin table, drive, walls, bot vs bot, push, event queue, hash, step order, win conditions for HP and count-out | sim.c (`sim_init`, `sim_step`, `rtr_sin`, `collide_*`, `drive_step`, `integrate`, `win_check`, `sim_hash`), sim.h, bot.c (`bot_body_from_build`) | 900 |
| 4 | THE BAY in arena.c with its map comment, tile list and spawns; `balance --check-arenas` on it | arena.c, arena.h, tools/balance.c (`check_arenas`) | 380 |
| 5 | world.c: lights, scene, bots from primitives, blobs, interpolation, `paint_floor`, THE BAY geometry; camera.c chase with wall push-out and layouts; main.c frame with the accumulator and timescale | world.c (`world_init`, `world_bot_build`, `world_sync`, `paint_floor`), camera.c (`cam_layout`, `cam_update`), main.c | 900 |
| 6 | fx.c and juice.h: the Hit Contract driver, sparks as cubes with the hit stop freeze, flashes, smoke, fire, debris visuals; rumble and camera punch and shake from events; damage numbers in draw.c | fx.c (`fx_hit`, `fx_update`, `fx_face`), juice.h, camera.c (`cam_punch`, `cam_shake`), main.c (`drain_events`), draw.c (`numbers`) | 520 |
| 7 | FEEL GATE: a WEDGE ram at 12 damage in halves fires every part of the Hit Contract (shake, punch, sparks, number, SFX placeholder click, rumble) and feels heavy. If it does not, stop and tune juice.h before any weapon is added. | none new; a day of tuning | 0 |
| 8 | Weapons one at a time in the order WEDGE ram, HORIZONTAL (spin, ring, clash, wall penalty), FLIPPER (height, inversion, count, self-right), HAMMER, DRUM, SPIKE, SAW, LIFTER, FLAME, CRUSHER, MELTY, secondaries; each with its stinger and a `balance --robin` run | sim.c (`weapon_step`, `hit_test`, `apply_hit`, `clash_check`, `ram_check`, `landing`), world.c (weapon meshes and poses), content.h | 1100 |
| 9 | hud.c copy and additions; the fight HUD: strips with ghost drain, status word and RED TAG strip; clock; stingers; minimap; chevron; count-out; tape seams; HUD_DIV; `hud_dirty` | hud.c, hud.h, glyphs.h (`hud_tag`, `hud_tri`, `menu_row`, `card`, `hud_dirty`), draw.c (`hud_strip`, `stinger`, `minimap`, `clock_chip`, `chevron`) | 760 |
| 10 | KO flow: final KO slow-mo with interpolation, orbit camera, tape wipe, KO card with name and cause, strips fade, results card with rows and awards, the four-row rematch vote, hall of fame updates | game.c (phase machine), camera.c (`cam_ko`), draw.c (`ko_card`, `results_screen`, `rematch_menu`), content.h | 620 |
| 11 | Title with the left-column scrim and attract, lobby with mode cards and seat turntables, garage panels (halves, quadrants, the 3 player match card), arena vote with painted thumbnails, countdown, settings, hall of fame, rules flash, control card, pause, the save struct | draw.c (`title_screen`, `lobby_screen`, `garage_panel`, `vote_screen`, `settings_screen`, `hall_screen`, `flash_card`, `controls_card`, `pause_overlay`), game.c (menu state), main.c (menu input reader, save load and write, crc) | 1500 |
| 12 | ai.c: perception ring, scorer with all eight goals, steering, difficulty, counter-picks, habits; the practice bot panel; attract driver | ai.c (`ai_init`, `ai_think`, `ai_pick_build`), game.c (attract) | 560 |
| 13 | OPS: build, zone and gate, wake triggers, LEADER TAX, carry, punish, leave, speech panel, tournament driving | sim.c (`ops_step`), ai.c (OPS goals), world.c (gate), draw.c (speech panel) | 300 |
| 14 | Arenas 2 to 6 with maps, hazards (every type in 6.2), tile lists, vis tables, benches and bumpers that break, the mound, ramps and the deck, wind, the laser; `check-arenas` green on all six; the six drive scripts | arena.c, sim.c (`hazard_step`), world.c (hazard meshes and tells), tools/scripts/drive-*.json | 1100 |
| 15 | ONE CAMERA with hysteresis and the tape cut; the 1 player and 3 player layouts; per-cell visibility; the 30 fps render fallback | camera.c (`one_camera_update`), main.c (`render_half`), world.c (`world_set_cell_visibility`) | 260 |
| 16 | Modes: KO BY FLIP, LAST BOT STANDING with hazard seats and mini-KO, KING OF THE HILL, 2V2 with team pools and friendly fire, TOURNAMENT bracket and bye coin flip, judges with the tie chain, CLOSING TIME | game.c (`mode_rules`, `bracket`), sim.c (`hill_step`, `closing`), draw.c (`judges_card`, `bracket_card`, `hazard_seat_view`) | 700 |
| 17 | Audio: audio.h, the SFX band with the 12 patches, whines, crowd bed, `audio_pcm.c` stems with fades and beat alignment, `tools/bake_music.cpp`, the three songs, `MUSIC=live` | audio.h, audio_band.cpp, audio_pcm.c, tools/bake_music.cpp, build.sh (`music` target) | 900 |
| 18 | shot.mjs extension (BTN, right stick, triggers, STATION), the five scripts, acceptance shots, WAV check | tools/shot.mjs, tools/scripts/*.json | 180 |
| 19 | `balance --robin` and `--replay`, the setup log line, the hash log, CI wiring | tools/balance.c (`robin`, `replay`, a 60 line script JSON reader), main.c (`log_setup`), .github/workflows/pages.yml | 420 |
| 20 | RED TAG overdrive: timer, multipliers, strip, stinger, sparks, COMEBACK setting, AUTO rule | sim.c, draw.c (`hud_strip`), game.c (settings) | 120 |
| 21 | Juice pass against section 5 with the 2p and 4p scripts: spark colours, smoke and fire, dents and shedding, bursts, the spinner ring, crowd frames, confetti, menu pops | fx.c, world.c (`bot_dent_geometries`), draw.c | 400 |
| 22 | Content pass: every table in section 16 and parts.h, `tools/lint.sh`, the lint in CI | content.h, parts.h, tools/lint.sh | 260 |
| 23 | Pi pass: HUD_DIV 2 at 540p, perf log, draw call counts per view, 4 player THE SHOP at 30 fps render, audio under 1 ms, deploy notes in HANDOFF.md | main.c, hud.c, HANDOFF.md | 200 |
| 24 | FREE PILE pickups: pickup debris flag and rings, heal, cap, plus burst, NOT A VACUUM, the PICKUP AI goal, JANITOR award; behind COMEBACK | sim.c (`pickup_step`), world.c (rings), ai.c, draw.c | 180 |
| 25 | KO replay from the snapshot ring, only if item 23 shows headroom | main.c, game.c, draw.c | 220 |

Total about 13,400 lines, of which sim.c is about 2,500, draw.c about 2,600 and world.c about 1,900.

## 18. Acceptance checks

Every check is a screenshot from the rig (`node tools/shot.mjs build/red-tag-rumble.wasc build/shots/<set> <frames> tools/scripts/<script>`) or a console line. The sim is deterministic under the rig's seed 7, so a frame always shows the same picture once the build settles.

| # | Script and frame | Must show |
|---|---|---|
| A1 | title.json 60 | the mark at (64, 56) in bone flame, RED TAG and RUMBLE, one tagline, five menu rows with the hazard cursor on FIGHT, the hint column reading 1 TO 4 PADS and YOU VS THE PRACTICE BOT, the scrim ending at x 480, no text on the right half |
| A2 | title.json 700 | the attract fight visible on the right with two ash HP bars and two bot names; the left column unchanged; the console has no `missing` line since frame 1 |
| A3 | 2p-fight.json 120 | the lobby with seats 1 and 2 occupied, both turntable viewports rendering a bot, DEATHMATCH lifted, MODE_BLURB under the cards, seats 3 and 4 reading PRESS A TO JOIN with FILL WITH BOT OFF |
| A4 | 2p-fight.json 260 | two garage halves; seat 1 panel reads POLITE DOORSTOP with ROLL OUT and 13 tags filled; seat 2 reads SPINNY PANCAKE with CHOP and 15 tags; both LOCKED with live frames; the timer tape shrunk from both ends; no row shows a dash |
| A5 | garage-2p.json 262, 292, 322 | the WEAPON row changed between shots, the three bars show a teal or rust delta segment at 292, seat 2's rows mid slot machine at 262 and a different legal build at 322 |
| A6 | 2p-fight.json 340 | the vote screen with six cards, THE LASER ROOM lifted with chips 1 and 2, every card showing a painted thumbnail (THE BAY white with orange and red rectangles, THE LASER ROOM dark, THE ROOF with taped open edges), the timer chip under 0:08, no 3D viewport on this screen |
| A7 | 2p-fight.json 560 | halves, THE LASER ROOM dark with teal lights, a 3 or 2 at each half centre, both strips live with full HP, the clock reading 1:15 in bone |
| A8 | 2p-fight.json 900 | both bots moving toward the centre, the spinner ring visible on seat 2's bot, the minimap over the seam with two dots and four saw glyphs, the clock under 1:11 |
| A9 | 2p-fight.json 1500 | at least one damage number or ghost drain visible, sparks or a flash in one half, the HP bars unequal, the console showing `[sim] f=` hash lines every 60 frames |
| A10 | 2p-fight.json 2400 | either the results card (winner line in seat colour, a KO_CAUSE_LINES line, two rows with awards, the four-row rematch menu with REBUILD focused and an AUTO count) or the fight in CLOSING TIME with a rust blinking clock |
| A11 | 4p-fight.json 260 | four garage quadrants, four different bot names, tag bars across each header, the seam tape at x 638 and y 358 |
| A12 | 4p-fight.json 900 | quadrants with the PIP at (540, 304), the clock on the top seam, four strips of 56 px whose content stops short of the PIP, at least one stinger (a taunt landed at 900 does not stinger; the first ram does) |
| A13 | 4p-fight.json 1500 | at least one elimination or heavy hit: a mini-KO stinger, a hazard seat view in one quadrant, or an INVERTED count |
| A14 | STATION=1 4p-fight.json 260 | identical to A11: four seats with only pad 0 connected |
| A15 | menu.json | the settings card with 13 rows and the cursor moved four rows down; the hall of fame reading NONE YET; seven rules cards each with a different caption and the step dots advancing |
| A16 | drive-<arena>.json every 300 | the lone bot at a different position each shot, every hazard crossed by the end, the console with no `[stuck]` line and no `missing` line for all six arenas |
| A17 | console, 2p-fight.json | `[sim] setup ...` once, `[sim] f=N h=...` every 60 frames, and `build/balance --replay tools/scripts/2p-fight.json --setup "<line>"` prints the same hashes |
| A18 | `build/balance --robin 20` | exit 0: no preset over 65 or under 35 percent, WEDGE over 40 percent against HORIZONTAL, HOVER under 45 percent against TREADS in THE BAY and over 55 percent in THE LASER ROOM |
| A19 | `build/balance --check-arenas` | exit 0 for all six arenas: gaps, vis tables, spawn clearance, hazard reach |
| A20 | `WAV=build/out.wav` 2p-fight.json | the WAV has the GARAGE loop until the countdown, a hard cut to ARENA on FIGHT, SFX_HIT events at the hit frames, and the limiter never clips (peak under 0 dBFS) |
| A21 | feel gate (item 7) | a WEDGE ram at 12 damage in halves shows a shake, a punch, 12 sparks spawning frozen then released, a bone number, a rumble call in the log (`[rumble] pad 1 0.40 0.20 80`), and SFX_HIT_LIGHT in the WAV; the reviewer calls it heavy |
| A22 | the station | pads 2 to 4 seat; 60 fps in halves in THE BAY; THE SHOP in quadrants holds the sim at 60 with render at 30; `t3_renderer_generated_missing` empty on frame 1 of every scene; the perf log shows the HUD upload under 2 ms at 540p |
| A23 | `tools/lint.sh` | zero findings over content.h, parts.h, HANDOFF.md and README.md |
| A24 | any screen | the HeatSync mark appears only on the title via `hud_logo` with a bone flame; never on a bot, never recoloured |

## 19. Risks

1. The feel gate fails on a Pi because wall-time particles and camera shake run at 30 fps render while the sim holds 60. Mitigation: shake and punch are evaluated on wall time and the band table is tuned at 30 fps render first; the gate is run on the station, not only the Mac.
2. Determinism breaks between native and wasm through a stray libm call or a different FMA contraction. Mitigation: `-ffp-contract=off` on both builds, the sin table, `grep -n "sinf\|cosf\|atan2f\|expf" src/sim.c src/bot.c src/arena.c src/ai.c` in `tools/lint.sh`, and the hash check in CI.
3. The HUD upload dominates on V3D. Mitigation: HUD_DIV 2 at 540p, `hud_dirty` gating, and the 30 fps render fallback; if it still dominates, split the strips into a 1280 x 72 texture uploaded every frame and the rest uploaded when dirty.
4. State table churn: additive blending and explicit instancing may add tokens the starting list does not have. Mitigation: `t3_renderer_compile` at boot with the missing log on frame 1 of every scene, and `REGEN_STATES=1` committed once the list settles.
5. The vote thumbnails drift from the real floors. Mitigation: they are the same `paint_floor` run at a different scale, never a separate drawing.
6. Balance: HORIZONTAL dominates or HOVER never loses to TREADS. Mitigation: the guardrails in `--robin` and the tuning order (tags, then self damage and wall penalty, then base damage); the catalogue has 20 percent headroom in costs.
7. Four live turntable viewports in the lobby plus the garage scene cost a frame budget on a Pi. Mitigation: the lobby renders the turntables at half rate (every other frame) and the vote screen has no 3D at all.
8. Hazard seats and mini-KOs make free-for-all matches end with three people pressing A at hammers that never land. Mitigation: pulverizer reach 1.2 u, the floor flipper and flame jets are also on the seat list, and OPS LEADER TAX keeps the last two bots moving.
9. The music bake tool and the live band drift apart. Mitigation: one audio_band.cpp compiled two ways; `MUSIC=live` is a build flag, not a copy.
10. Pickups and RED TAG make 1v1 feel unfair. Mitigation: COMEBACK AUTO is OFF in 1v1; both levers are one toggle.
11. The sim struct grows and `memcpy` snapshots every 2 s cost cache. Mitigation: `match_t` stays under 8 KB; the snapshot ring is two slots.
12. Scope: 13,000 lines is three to four weeks of focused work. Mitigation: items 1 to 10 are a playable 2 player cart in THE BAY with one weapon family; everything after is additive and each item ships on its own.

## 20. Things an implementer must not get wrong

1. Seat on A, B, X, Y or Start edges from all four pads; never read `connected` (the RetroArch core reports only pad 0 connected). Run the 4p script under `STATION=1`.
2. Do not set `WC_FLAG_KEYBOARD`; the keyboard is pad 0 on every host. Do not set `WC_FLAG_NET_PEER`; no `--ws`.
3. The light vector is `l1201` in every scene; inert point lights stay at intensity 0 rather than being removed. Every lit material needs plain and `+instbig` states. No shadow maps anywhere.
4. Split screen: scissor test on, viewport and scissor equal, auto clear on per view, y from the top; scissor off, full viewport, auto clear off for the HUD pass.
5. The sim never sees dt, wall time, the camera or the renderer; `G.timescale` scales the accumulator, never DT; hit stop is a sim counter; prev transforms are stored at the start of every tick and the render interpolates by `acc / DT`.
6. No libm trig in sim.c, bot.c, arena.c or ai.c; `sqrtf` only. The hash log and `balance --replay` must agree.
7. Caps are code, not comments: 400 sparks, 160 smoke, 24 debris, 6 numbers per viewport, one stinger per viewport, 128 screen bursts, strings at their buffer lengths.
8. Every feel number lives in juice.h; every string lives in content.h or parts.h; `tools/lint.sh` runs on both.
9. Audio frames per render are time based with a fractional carry, capped at half the ring; the PCM mixer gets the same frame count; every `I_BUZZ` gets an OFF; anything two players can fire at once is pooled.
10. Honour the negotiated resolution with a 1280 x 720 cap and switch HUD_DIV at 540p; the HUD is authored at 1280 x 720 coordinates regardless.
11. CRUSHER is refused in 1v1 by `build_legal`, not by a note. HORIZONTAL is refused on WEDGE. MELTY has no weapon row.
12. The HeatSync mark is drawn only by `hud_logo` on the title marquee, flame bone, sun untouched; never on a bot, never recoloured, never on the garage wall.
13. `save_t` is static with magic, version and crc; all zero means empty; the hall of fame wipe clears records, not settings; `shot.mjs` never passes save data.
14. Nothing scrolls. If a menu needs more than a 2 x 3 grid or a 14 row card, cut it.
15. Jokes live only in part flavour, stingers, the rebuild hint, awards and OPS lines. Safety lines (the control card, the count-out prompt) are never jokes.
16. The vote screen has no 3D viewports; the lobby, garage and results keep their turntables.
17. The judges tie chain is cheaper build, fewer frames inverted, DRAW, and the card says which rule applied.
18. COMEBACK AUTO means ON in free-for-all and 2V2 and OFF in any 1v1, and it governs both RED TAG and pickups.
