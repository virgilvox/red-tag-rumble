# Combat brief: a split-screen bot-building arcade game for wasmcart

Design-research input for a separate game in its own folder beside DO-OCRACY. Not a survey. Every section ends in numbers an implementer can type in. Where real robot combat and a good arcade game disagree, the arcade wins and the note says so.

Hard constraints this brief respects everywhere:

- wasmcart cart in C, three.c for the 3D, 1280x720, CPU-drawn HUD texture composited over the scene (same two-pass trick DO-OCRACY uses).
- 1 to 4 W3C gamepads, no phones, no CLASP, no network of any kind.
- Raspberry Pi class performance. Budget assumption: 60 fps with 4 bots, 1 arena, about 25k triangles on screen total across all viewports, no shadow maps, no post-process passes other than the HUD composite. Split-screen multiplies draw calls, not triangle budget, so every arena is built so a quadrant view sees at most a third of it.
- Local only, so nothing waits on anything. Every menu is a state machine driven by pad input polled once per frame.

House rules for every string in the game: no em dashes, no en dashes, no emoji, no real people's names. HeatSync flavour is general hackerspace culture only (laser room, Free Pile, the loud fan, the lab door, roles like Treasurer or Operations, never a named person). GANTRY theme: char, bone, hazard amber fill, rust, live teal, square corners, solid offset shadows, caution tape. The HeatSync mark is used as-is or not at all.

Working title suggestions, all placeholder: SCRAP TUESDAY, FREE PILE FIGHT, BENCH WAR. Pick one that fits on a cabinet marquee in one word pair.

## 0. The one-paragraph pitch

Four people at a cabinet. Press any button to drop in. Sixty seconds in a build bay choosing a chassis, a drivetrain, a weapon and armour against a 20-point budget. Then ninety seconds in a hackerspace-themed arena with hazards on a timer, a house robot that shows up when you turn the fight boring, and a KO that goes slow-mo with the camera punched into the hit. Lose a match, rebuild in thirty seconds, go again. The build is the strategy and the drive is the skill, and both are readable to someone walking past.

## 1. Weapon archetypes

Design principle: real combat robots decide fights in one or two hits because kinetic energy scales with the square of tip speed. An arcade game needs fights to last 60 to 120 seconds and let a loser come back. So every weapon in this list trades the real thing's lethality for a readable rhythm: spin up, commit, recover. Hit stun and knockback carry the feel, HP carries the pacing.

Shared damage model:

- Every bot has HP 100 plus armour. Armour is a percentage reduction on incoming hits, not a second bar.
- A hit does `base * speed_factor * armour_mult * zone_mult`. `speed_factor` is relative closing speed of the weapon tip against the target, clamped 0.4 to 1.6. `zone_mult` is 1.0 on sides and rear, 0.6 on the front plate, 1.5 on the underside (exposed only when flipped or lifted) and 2.0 on the weapon itself when a spinner is hit while spun down.
- Weapon-on-weapon between two spinners: both take 60 percent of the computed hit, both get a long hit stun (0.5 s), both lose all spin. This is the real "spinner clash" and it is the single most satisfying moment in robot combat; the game must sell it with the biggest spark burst and the loudest SFX.
- Every hit has hit stun (victim cannot drive for N ms), knockback (impulse along hit normal, scaled by mass ratio), and self-damage for the attacker on some weapons.
- Hit stop: on any hit over 15 damage the whole sim freezes for 3 frames (50 ms). Over 30 damage, 6 frames. This is cheap and reads as weight.

| Weapon | Base dmg | Charge / spin-up | Cooldown | Hit stun | Knockback | Self dmg | Counterplay |
|---|---|---|---|---|---|---|---|
| Horizontal spinner | 30 at full spin | 2.0 s to full, spin shown by a ring | none while spun up | 0.35 s | very high, lateral | 8 on every hit, 15 on wall hit | wedge under it, hit it while spinning down, grind it on a wall |
| Vertical spinner / drum | 24 | 1.5 s | none | 0.4 s | high, vertical (lifts target) | 5 | low wedge, attack from the side, spinner clash |
| Flipper (pneumatic) | 6 | instant | 1.8 s, with a visible gauge refilling | 0.6 s airborne | huge, vertical launch | 0 | drive at it from the side, never show it your front |
| Hammer / axe | 28 | 0.25 s wind-up with a tell | 1.2 s | 0.5 s | low | 4 on a miss (hits floor) | step out during the wind-up |
| Lifter (electric) | 4 | hold to raise, 0.6 s to full | none, but it is slow | 0.3 s while held | low; it carries | 0 | reverse hard, hit it from the side while it is committed |
| Saw | 5 per tick, 6 ticks/s while in contact | 0.8 s spin-up | none | 0.1 s per tick | tiny | 2 per second in contact | back off, saws need dwell time |
| Wedge / rammer | 10, scaled by speed up to 20 | none | none | 0.25 s | high, forward | 0 | out-turn it; it has no reach, and it bounces off walls |
| Flamethrower | 3 per tick plus burn 2/s for 3 s | none, hold to fire | fuel: 4 s of fire, 3 s to refill | 0 | 0 | 0 | ignore it and hit back; fire is a pressure tool and a crowd pleaser, not a killer |
| Crusher | 18 on close, then 6/s while held | 0.4 s to clamp | 2.0 s | victim locked for up to 2 s | none; it holds | 0 | the only escape is a teammate or a hazard, so crusher is banned in 1v1 ranked, allowed in 2v2 and free-for-all |
| Pneumatic spike | 22 | 0.15 s | 1.0 s | 0.3 s | medium, forward | 0 | side-step; it is a straight line |

How each should feel, in one line each:

- Horizontal spinner: a chainsaw you cannot steer well. Fast when spun up, wobbly when spun up (reduce turn rate 30 percent at full spin to model gyroscopic effect), a sitting duck for two seconds after any clash. Full-spin hits throw the victim across a quarter of the arena. Hitting a wall at full spin costs you spin and 15 HP. It is the "high skill ceiling, high comedy floor" option.
- Vertical spinner / drum: the modern meta weapon on TV and the best all-rounder here. Hits pop the target up, which exposes their underside for a follow-up. It is also your self-righter: a flipped vertical spinner can hit the floor with the weapon to flip back (press weapon while inverted).
- Flipper: a crowd weapon. Launches on a parabola, target lands either upright or inverted (coin flip weighted by their chassis shape). Launch into a pit or over a wall is a KO. The gauge is a vertical tank that visibly refills; everyone around the cabinet can see when the flipper player is empty.
- Hammer / axe: telegraphed. The arm cocks back with a sound; land it and you dent the top plate (visual), miss it and you hit the floor with sparks. Best against inverted bots and anything slow.
- Lifter: a grappler. Gets under, raises, carries the target toward a hazard or wall. Low damage, high control score. Gives the judges' decision format a reason to exist.
- Saw: a dwell weapon. Rewards pinning. Pairs with high-traction drivetrains. Weak alone, nasty with a wedge front.
- Wedge / rammer: the no-weapon build. Lowest cost, invertible, fastest. Wins by control and pushing into hazards. Every real BattleBots season has one wedge in the top eight; the game should make it viable or the points budget is wrong.
- Flamethrower: lights the target (burn DOT, smoke, fire particles on the target for the duration). Terrifying to watch, mild in damage. Also cooks the crowd layer (background flicker). Fuel gauge same visual language as the flipper tank.
- Crusher: a hold. Target locked, both bots stop. Camera zooms. Two seconds of hydraulics sound, then release. The slowest and most dramatic weapon and a real tempo breaker, so limit it to multi-bot formats.
- Pneumatic spike: a punch. Reach of half a bot length, straight ahead, instant feel. The beginner's hammer: no wind-up to learn, less damage.

Weapon mount rules: one primary weapon per bot. Wedge and lifter can take a secondary (saw or flamethrower or spike) for extra points. Spinners are exclusive.

Invertibility: wedge, rammer, drum and saw bots drive inverted at 70 percent speed. Horizontal spinner inverted still spins but cannot steer. Flipper, hammer, lifter, crusher and spike cannot operate inverted, but hammer and flipper can self-right with the weapon (press weapon while inverted, 1.5 s animation, vulnerable). Any bot that stays inverted and immobile for 10 s is counted out. Players see a 10 to 0 count over their panel. This is the real BattleBots immobility rule and it gives the game its "KO by flip" format for free.

## 2. Drivetrain archetypes

Shared model: each drivetrain has a top speed (m/s in arena units where a bot is about 1.0 long), acceleration, turn rate (deg/s), traction (0 to 1, multiplies push force and scales how much knockback it eats), mass (added to chassis mass; mass ratio decides who moves in a shove), and a quirk.

| Drivetrain | Top speed | Accel | Turn | Traction | Mass | Push | Quirk and feel |
|---|---|---|---|---|---|---|---|
| 2 wheel | 6.0 | fast | 300 | 0.6 | light | low | nimble, slides, wheels are exposed (a hit on a wheel zone cuts speed 40 percent for 4 s). The default for beginners |
| 4 wheel | 5.0 | medium | 220 | 0.8 | medium | medium | the all-rounder; loses one wheel and keeps driving at 80 percent |
| 6 wheel | 4.2 | medium | 180 | 0.95 | heavy | high | climbs small ledges, nearly impossible to push, wide turning |
| Tank tread | 3.8 | slow | 160 | 1.0 | heavy | highest | the shove machine; also eats knockback (0.5x). Loud. Treads can be cut by saws (2x saw damage on tread zones) |
| Hover / omni | 5.5 | instant | 360 | 0.3 | light | none | strafes (left stick moves, right stick aims, see controls). Floats over killsaws and screws untouched. Cannot push at all; flipper launches it 1.5x further. The glass cannon |
| Walker / shuffler | 3.0 | slow | 120 | 0.7 | medium | medium | real BattleBots gave walkers a weight bonus; here walkers get +4 budget points. Gait animates, feet stomp with SFX, immune to pit edges (steps over a pit lip within 0.3 of a bot length) and it wiggles when hit |
| Legged tracks | 3.4 | slow | 140 | 0.9 | heavy | high | treads on articulated legs; climbs ramps and the upper deck faster than anything, +2 budget points |
| Melty brain | 4.5 | medium | n/a | 0.4 | light | none | the whole body spins; there is no front. Drive is by heading intent, a blinking teal heading LED shows "forward". Every edge of the bot is a horizontal spinner at 18 dmg. Cannot mount a weapon. Cannot be flipped (it is symmetric). Self-damage 10 on wall hits. Hardest to drive, the showoff pick |

How they feel against each other, the triangle that makes build choice matter:

- Treads beat wheels in a shove, wheels beat treads in a chase, hover beats both in a dodge and loses to anything that catches it.
- Spinner on treads is a wall. Spinner on hover is a dragonfly. Flipper on 2 wheel is the classic arcade build.
- Walker and legged tracks exist to make the hazard-heavy arenas (pit, screws) a different game; they are the only drivetrains that laugh at the pit.

Steering model for all non-hover drives: arcade car. Left stick Y is throttle (reverse allowed at 80 percent), left stick X is turn. Turn rate scales down to 60 percent at top speed so high-speed bots carve. Optional tank controls (two sticks) in settings per player. Hover and melty use left stick as direction of travel (world-relative to the player's camera forward).

Mass and shoving: when two bots are in contact and both drive into each other, the net push is `(push_a - push_b)` along the contact normal, where `push = traction * mass * drive_force`. A heavier tread bot moves a hover bot at almost full speed. Nobody can push a bot that is pressed into a corner, which is why the house robot exists (section 4).

## 3. Chassis, armour, materials and the points budget

Budget: 20 points. Every part costs. Overspend is not allowed. Underspend is allowed and gives nothing (do not reward it, or wedge-only builds dominate). The builder screen shows a 20-segment bar in hazard amber filling as you spend; the last two segments turn rust when you are near the cap.

Chassis (choose one), sets base mass, HP, and silhouette:

| Chassis | Cost | HP | Mass | Shape notes |
|---|---|---|---|---|
| Box | 2 | 100 | 1.0 | flat sides, easy to wedge, easy to armour, the baseline |
| Wedge | 3 | 90 | 1.0 | low front, gets under everything with a flat front, cannot mount horizontal spinner |
| Low profile (undercutter body) | 3 | 85 | 0.9 | spinners sit lower, harder to flip (flip odds 30 percent), 0.8x flipper launch |
| Shell (dome) | 4 | 110 | 1.1 | round, sheds hits (zone_mult 0.8 everywhere), rolls when flipped and self-rights 50 percent of the time |
| Frame (open tube) | 1 | 70 | 0.8 | the cheap one; parts show, parts fly off, light and fast. Everything you fit is exposed |
| Lunchbox (tall) | 2 | 100 | 1.0 | tall, tips easily (flip odds 70 percent), but hammers hit it for 0.7x because the top plate is far from the arm, and it is the only chassis that can take a top-mounted secondary weapon |

Armour (choose one plate set), applies to all zones:

| Armour | Cost | Reduction | Mass | Visual |
|---|---|---|---|---|
| None (bare frame) | 0 | 0 percent | +0 | parts fly off after 50 percent HP |
| Plywood | 1 | 10 percent | +0.1 | splinters on hit, chars under flame (the laser room joke) |
| Aluminium | 2 | 20 percent | +0.2 | bright dents, sparks are white |
| Steel | 4 | 35 percent | +0.5 | sparks are amber, deep dents |
| Hardox-style wear plate | 6 | 45 percent | +0.6 | nearly no dents, spinner hits on it stun the attacker 0.15 s longer |
| UHMW plastic | 3 | 15 percent plus 0.5x knockback and immune to burn | +0.2 | hits leave white scrapes, no sparks |
| Titanium | 5 | 30 percent | +0.3 | the light-and-strong pick, rainbow spark colour, the "expensive" look |

Drivetrain costs: 2 wheel 2, 4 wheel 3, 6 wheel 4, tank tread 5, hover 5, walker 1 (plus its +4 bonus budget), legged tracks 3 (plus +2 bonus), melty brain 6.

Weapon costs: wedge 1, spike 3, lifter 3, saw 3, flamethrower 4, hammer 5, flipper 6, vertical drum 6, horizontal spinner 7, crusher 7. Secondary weapon on wedge or lifter adds its cost minus 1.

Example legal builds, used as presets so a new player can press one button and go:

- SHOVE (preset 1): wedge chassis 3, tank tread 5, wedge 1, steel 4, remaining 7 unspent. Beginner. Immune to being pushed, invertible, boring and effective. Present it as "ROLL OUT" with a one-line gag.
- LAUNCH (preset 2): box 2, 2 wheel 2, flipper 6, aluminium 2, secondary none. Total 12. Fast flipper, the arcade classic.
- CHOP (preset 3): low profile 3, 4 wheel 3, horizontal spinner 7, aluminium 2. Total 15. Glass cannon spinner.
- STOMP (preset 4): lunchbox 2, walker 1 (+4), hammer 5, steel 4, spike secondary not allowed (hammer is primary only). Total 12 of 24.
- TOAST (preset 5): shell 4, hover 5, flamethrower 4, UHMW 3. Total 16. The troll build.
- SPIN (preset 6): low profile 3, melty brain 6, no weapon, titanium 5. Total 14. For the person who says "I have done this before."

Balance guardrails for the implementer:

- Run an AI vs AI round robin of the six presets (every pair, 20 matches each, both arenas with and without hazards) in headless mode before shipping. No preset should win more than 65 percent or less than 35 percent of its matches overall. Tune costs, not damage, first.
- A wedge-only build must beat a horizontal spinner at least 40 percent of the time. If it does not, the spinner self-damage and wall penalty are too low.
- Hover must lose to tread more than 55 percent in a no-hazard arena and win more than 55 percent in the killsaw arena. That is the whole point of hover.

Material joke layer: every material has a one-line Free Pile provenance shown in the builder ("plywood: the offcut bin by the laser"). Keep it to one line, bone text on char, and never more than one line on screen at a time.

## 4. Arena hazards, layouts and the house robot

Hazard rules of thumb from the TV shows that translate directly:

- Hazards are on timers and announced. The pit descends at a known time (the Robot Wars pit opened after a delay or on a button). A hazard that surprises a player is unfair; a hazard a player can read and use is strategy. Every hazard gets a 1.5 s tell: a light, a sound, a floor stripe pulsing.
- Hazards hurt everyone equally. Hover ignores floor hazards and that is its deal.
- Hazards are camera candy. The killsaws throwing sparks on an empty arena corner keep a quadrant interesting when nothing is happening there.

Hazard catalogue:

| Hazard | Behaviour | Damage | Tell |
|---|---|---|---|
| Pit | a 2x2 floor square that drops at T minus 60 s, then every 20 s cycles open 6 s, closed 14 s. A bot fully in the pit is KO. A bot half in can drive out with traction over 0.7 or a walker | KO | caution tape border, amber floor lights blink 1.5 s before opening, klaxon |
| Pulverizer hammers | two arena-edge hammers. In 2v2 or FFA, a knocked-out player gets to drive the hammers (a real BattleBots rule and the best spectator rule ever written). In 1v1, they auto-swing when a bot is under them | 20 | the arm twitches, a lamp above it goes teal |
| Killsaws | four floor slots on a 7 s cycle, saws rise for 1.5 s | 6 per tick, 4 ticks/s, cuts treads 2x | slot glows amber, grinding rises in pitch |
| Flame jets | floor grates, fire for 2 s on an 8 s cycle, also triggered when a bot is pushed over them at speed | 3 per tick plus burn | gas hiss, grate glows |
| Screws | two corner rotating screws that carry a bot along the wall toward a pulverizer | 3 per tick, carries | always on, a slow rumble |
| Ramps | fixed, launch bots that hit them over 4.0 m/s; landing inverted odds 50 percent | 0, fall damage 8 if from the upper deck | none, it is terrain |
| Bumpers | pinball-style posts that fire an impulse on contact | 4 | lamp flash and a bell |
| Upper deck | a raised platform with a 10 s count down if you are thrown onto it; drop off any side to escape; legged tracks climb it on purpose for the high ground | KO on count | a count over the player's panel |
| Floor flipper | a 1x1 panel that fires when a bot sits on it 1 s | 0, launch | panel colour pulses teal |
| Drop zone | a marked square; a bot immobile on it for 3 s gets a dropped object (a dead CRT, a mini fridge, a box of the Free Pile) | 25 | a shadow grows on the floor |

The house robot:

- Rule: the house robot is a referee with a grudge. It wakes when the match has been boring (no hit landed in 12 s) or when a bot is wedged in a corner for more than 4 s and nobody can move. It leaves its corner (a caution-taped Corner Patrol Zone like Robot Wars), goes to the problem, shoves or flips the bot into play, and returns. If a bot attacks the house robot, the house robot fights back for 5 s with a 40-damage shove-flip and then returns.
- Build: a tank-tread lunchbox with a lifter and a saw, 300 HP, immune to knockback, can be damaged (dents show) but never KO'd, and it leaves the arena at T minus 10 so the end is always between players.
- Name and flavour: no human name. It is "OPS" (the Operations role made into a bot, a GANTRY char body with a hazard amber stripe and a caution tape bumper). Its text stingers are Operations-flavoured: "CLEAR THE AISLE", "THAT IS NOT WHERE THAT GOES", "I AM NOT MAD, I AM OPERATIONS".
- Why it matters: couch fights stall when two players pin each other. The house robot is the un-stall mechanic and a comedy beat. Make its arrival a camera event (the arena's PIP map flashes, horn SFX, a quick full-screen 0.5 s "OPS INBOUND" stinger).

Six arena concepts, each with a one-line hook, size, hazards, and what the quadrant camera sees. Arena units: 1.0 is a bot length. Keep every arena inside a 24x16 or 20x20 footprint so a shared camera can frame it at 1280x720.

1. THE BAY (the default, teaching arena). 24x16 flat, polished concrete, white floor tiles with a few amber and rust rectangles (the DO-OCRACY tile language carried over so the two games look like siblings). Walls are low lexan with a caution tape rail. Hazards: two pulverizers on the long sides only, pit opens at T minus 60 in the centre. Shared camera fits the whole thing. Zero clutter; this is where new players learn.
2. THE LASER ROOM. 20x20, dark, teal task lights. Four killsaw slots in a square around a centre "honeycomb bed" platform that is 0.2 high (a ramp lip, bots bounce). Flame jets along one wall (the vent). A laser beam sweeps across the arena once every 30 s at 0.3 height; anything taller than low profile takes 10 and a scorch decal. Hover bots duck under nothing; low profile ducks. The visual signature: a teal line and a smoke haze.
3. THE FREE PILE. 24x16 with a mound in the middle of cardboard boxes and junk (static boxes with dynamic debris on hit, cheap). Bots on the mound are higher and can drop onto others (8 fall damage). Drop zone hazard in two corners. Screws along one short wall. Bumpers made from old servers. The messy arena; most props, least hazards.
4. THE DOCK. 20x16 outdoor-ish, a loading dock with a 1.0-high upper deck along one long wall reached by two ramps. Floor flipper in the middle. Pit at one end that is actually the roll-up door gap. Rain decal on floor (lower traction 0.85 for everyone except treads). Night lighting, one sodium-amber flood.
5. THE SHOP (the machine room). 20x20, four quadrants separated by low tool benches (0.4 high walls that spinners can hit, and that break away after 60 damage, opening the arena). Pulverizers on all four sides. Killsaws in the centre cross. The arena that changes shape during the match.
6. THE ROOF. 18x18 with no walls on two sides: fall off and you are out (ring-out, Smash-style). Bumpers along the open edges are the only protection, and they break. Wind gusts every 15 s push everything 0.5 m/s toward an open side for 3 s with a tell (flags on the HVAC units). Best for flippers, terror for hover. The sudden-death arena.

Arena art budget: one 2048 texture atlas per arena, baked vertex lighting, no realtime shadows, one directional light plus two coloured point lights for the GANTRY accents (amber and teal). Hazards are 200 to 600 triangles each. Props under 150 triangles. The crowd is a flat ring of billboarded silhouettes with three animation frames, swapped on hits.

## 5. Match formats

- DEATHMATCH (default): HP to zero, or counted out (immobile 10 s), or KO by hazard. Last bot standing in FFA. 90 s timer; at time, judges' decision.
- KO BY FLIP: HP off. Only inverted-and-immobile for 10 s, pit, ring-out and upper deck count. 60 s. The flipper and lifter format. Everyone's self-right animations matter here.
- LAST BOT STANDING (FFA 3 to 4 players): deathmatch with the eliminated players handed the pulverizer hammers and the flame jets. Keeps everyone at the cabinet busy until the end. Non-negotiable: eliminated players must always have something to press.
- KING OF THE HILL: a 3x3 hill square moves every 20 s. Hold it alone to earn a point per second. First to 30. Lifters and treads shine. HP damage still applies and a KO respawns you at your corner after 5 s.
- 2v2: teams by panel colour (amber and teal). Friendly fire on at 50 percent (real teams hit each other all the time and it is funny). Crusher allowed. Shared pulverizer control to the first eliminated teammate.
- JUDGES' DECISION (only on time-out): three bars shown on the result screen, aggression (time within 2 bot lengths of an enemy while moving toward them), damage (HP dealt), control (seconds the enemy was lifted, pinned, inverted or in a hazard). Each bar is worth up to 5 points; 15 total; the real BattleBots rubric weights damage highest, so damage is 7, aggression 4, control 4 here. Ties go to the lower-cost build (the underdog rule) and the screen says so.
- TOURNAMENT wrapper: with 3 or 4 players, a 3-match bracket (semis and a final, the losers fight a third-place match on the same card while the finalists rebuild). Bracket drawn with caution tape between names. The eliminated players take the house robot in the final (one drives, one runs its lifter). That is the "nobody sits out" rule.

Match flow timing: 3 s "READY" with bots in corners and weapons spinning up allowed, 90 s fight, 4 s KO slow-mo, 6 s result card, 30 s rebuild window (skippable when all players press A). A full three-player evening round is under four minutes.

## 6. Couch UX

Join-in:

- Attract screen is a live AI vs AI match in THE BAY with the title over it. Pressing any face button on any pad joins that pad as the next free panel colour: amber, teal, bone, rust (GANTRY order). The panel slides in from the pad's side of the screen with a solid offset shadow and the pad's number.
- Join is allowed any time in the lobby and the builder. Leave by holding B for 1 s on your own panel (a ring fills around the B icon so it is never accidental). Join is not allowed during a fight; the pad gets a one-line "NEXT MATCH" on its panel region and nothing else.
- Idle pad in the lobby for 60 s drops with a visible count from 10. Idle during a fight does nothing; the bot sits there and gets hit, which is correct.
- One player alone: the builder shows two panels, the second is the practice bot (section 9) with a three-way difficulty toggle on it. Starting a fight alone is a real 1v1 against the AI, never a lonely arena.

Split-screen layouts, chosen automatically by player count and format, overridable in settings:

- 1 player: full screen, chase camera behind the bot, PIP arena map top-right (240x135).
- 2 players: vertical halves, 640x720 each. Vertical halves are correct for a 16:9 cabinet because bot combat is about the gap between you and them, which is a horizontal problem; each half still gets more horizontal pixels than a quadrant. Twisted Metal and Mario Kart both default 2 player to horizontal halves; we break from that on purpose and say why: the HUD texture is 1280 wide and a vertical split keeps every player's HUD strip at the bottom of their half, out of the action.
- 3 players: three columns of 426 wide, or two top plus one bottom with the PIP arena map in the fourth quadrant. Default to the second; three columns gets too thin for the HUD type at 720p.
- 4 players: quadrants, 640x360 each. PIP arena map in the centre over the seams, 200x112, with the four bot dots in panel colours and the house robot as a char square.
- Shared camera option: when all bots fit inside a camera frustum at a reasonable zoom (all bots within 10 arena units of each other) the game can switch to a single shared top-down-ish camera (a 55 degree pitch, like Mario Kart Battle on the shared-camera mods, or Overcooked's fixed room view). Transition is a 0.4 s cut with a caution tape wipe, never a smooth zoom. Switch back to splits if any bot leaves the frame for 1 s. Default on for 2 players in THE BAY, off elsewhere, and a per-match toggle on the arena select screen labelled ONE CAMERA. This is the big win on a Pi: one render pass instead of four.
- Rendering cost: splits re-render the scene per viewport with a scissor, so a 4-player split in THE SHOP is the worst case. Budget it: 4 viewports times 6k triangles. Cull aggressively per viewport (every arena has a coarse 4x4 cell visibility table baked in).

Bot builder, under 60 s per player, simultaneous:

- One builder panel per player, same split layout as the fight so nobody has to learn two screen regions. The panel has five rows: CHASSIS, DRIVE, WEAPON, ARMOUR, PAINT. D-pad up and down picks a row, left and right cycles the option; the 3D bot in the panel rebuilds live (parts are separate meshes swapped in, no load). A is "lock in" with a one-second hold the first time and instant afterwards; B backs out of lock-in. Start toggles presets (the six from section 3, a seventh RANDOM). Y shows the control card (section 8) for that player's panel only.
- The budget bar runs across the top of the panel, 20 segments, amber fill, last two rust when near the cap, char when over (you cannot lock in while over, and the bar shakes). No stat tables. Every option shows a 3-word blurb and three tiny bars (SPEED, HIT, TOUGH) that move as you cycle. This is the Custom Robo lesson: side-by-side stat comparison is what players wanted and did not get, so the three bars show the delta in teal (gain) or rust (loss) as you hover an option before you commit.
- The 60 s timer is shared and shown as a caution tape strip shrinking along the bottom of the whole screen. When it hits zero, everyone's current build locks. When everyone has locked, the arena vote starts (each player's stick picks an arena card, majority wins, ties go random, 8 s).
- Paint row: four GANTRY-safe schemes per panel colour plus a decal set (hazard stripes, caution tape, a number, a shop-safety pictogram). Never the HeatSync mark on a bot. The mark is allowed once, at its original colours, on the attract screen cabinet marquee only.
- Rebuild between matches: the same panel, 30 s, with your last build pre-loaded and a one-line "what beat you" hint in bone text (for example "FLIPPED 3 TIMES: try LOW PROFILE or 6 WHEEL"). That hint is the only tutorial the game has and it is enough.

Lobby and option screens: everything is a card with a square corner and a solid offset shadow. Selected card lifts (the shadow grows by 4 px). No scrolling lists ever; if it does not fit in a 2x3 card grid it does not exist. Settings: controls per player (arcade or tank sticks, invert Y off by default), split layout override, one camera toggle, screen shake 0 to 3, hit stop on/off, text stinger size, SFX and music volume, colourblind mode (swaps teal for bone-outlined teal patterns). Rules screen is a 6-panel comic strip with one line each, shown once on first launch and under Y from the lobby.

## 7. What makes it feel AAA on a weak machine

Everything below is CPU-side cheap or a few extra draw calls. Nothing needs shaders beyond what three.c's standard materials give us.

- Hit feedback stack, in order, every hit: hit stop (3 or 6 frames), camera punch (the hit's viewport camera kicks 0.1 m along the hit normal and recovers over 0.3 s with an ease-out), screen shake (per viewport, amplitude by damage, 0.2 to 0.5 s, decays), spark burst (a 12 to 40 particle billboard emitter, colour by armour material, additive, lives 0.4 s), impact flash (a 1-frame white quad at the contact point, scaled by damage), SFX (a layered clang: a low thud sample plus a material-specific ring plus a crowd gasp sample at damage over 20). All six every hit. A 15-damage hit with all six reads as heavy; a 40-damage hit without them reads as nothing.
- Damage visuals: each bot has 6 armour panels as separate meshes. At HP 70, 50 and 30 the panels swap to dented variants (pre-authored, 3 states each, so 18 meshes per chassis, under 100 triangles each). At 50 a smoke emitter starts (grey billboards, 4 per second). At 30 the smoke goes black and a small fire emitter joins. At 20 a random panel detaches and becomes a physics debris piece that lives 4 s then fades. At 0 the bot bursts: all panels detach, wheels roll off, the chassis frame stays. Debris count hard-capped at 24 across the arena; oldest gets recycled.
- Spinner tell: a translucent ring at the weapon radius whose opacity tracks spin percent, with a subtle motion-blur fake (a second copy of the disc rotated 15 degrees at 30 percent alpha). This is cheaper than any blur and reads at 360 px tall.
- Announcer replaced by text stingers plus SFX: a 0.6 s full-viewport stinger on a char bar with a solid offset shadow, bone text, 56 px at full screen, 36 px in a quadrant. Triggers: FIGHT, BIG HIT (over 30), CLASH (spinner on spinner), AIRBORNE (flip), INVERTED (count starts), PIT, KO, OPS INBOUND, TIME. Each has an SFX (horn, gong, airhorn, klaxon). Never more than one stinger on screen per viewport, and a new one replaces the old. The copy is where the humour lives: pull from a table of 4 to 6 variants per trigger ("AIRBORNE", "NOT A FLYING ROBOT", "CHECK YOUR CENTRE OF MASS"). Keep each under 24 characters.
- Slow-mo KO: on the killing hit, sim time goes to 0.2 for 1.5 s, then 1.0. Every viewport switches to a shared KO camera that orbits the victim at 30 degrees per second, framed so both bots are in shot. The victor's panel colour tints the vignette. Then cut to the result card. This costs nothing and is the moment people remember.
- Replays if cheap: record pad input per frame (4 pads times 16 bytes times 5400 frames at 60 fps is 345 KB) plus the match seed, and replay the last 4 seconds by re-simulating from a saved sim snapshot taken every 2 s (a ring of two snapshots). Only worth it if the sim is deterministic, which it must be anyway for the headless balance tests. Show the replay once, 4 s, from the KO camera, with REPLAY in rust text and a caution tape frame, then the result card. Skippable with A.
- Crowd: a ring of billboarded silhouettes in two rows, 3 frames (idle, lean, arms up), swapped per hit intensity, tinted by the arena's light colour. Crowd gasps and cheers are 6 samples total. Cost: a few hundred quads in one draw call per viewport.
- Lighting: one directional light with a baked colour, two point lights per arena in amber and teal, emissive materials on hazard tells, a vertex-coloured floor so the GANTRY tiles need no texture. Flames and sparks are additive billboards. Fire on a bot is a single emitter. No shadows except a dark blob quad under each bot (the cheapest trick and the one that most sells "it is on the ground").
- Camera: chase camera at 3.5 behind and 2.2 up the bot, looking 0.6 ahead of it, with a 0.12 s lag so turns feel heavy. FOV 60 in halves, 70 in quadrants. On a melty brain, camera does not spin; it follows heading intent.
- Audio: music is a two-layer loop (drums and a bass synth) that adds a lead layer when any bot is under 30 HP, and drops to drums only during slow-mo. Spinner whine pitch follows spin percent and is the one continuous SFX; everything else is one-shot.
- Performance rules: particle cap 400 across all viewports, debris cap 24, stinger text pre-rasterised into the HUD atlas at two sizes, HUD redraw only on dirty regions (same as DO-OCRACY's hud.c). Target 60 fps in 2-player, accept 30 fps locked in 4-player THE SHOP with the switch visible nowhere (fixed-step sim at 60, render at whatever).

## 8. Controls on a W3C gamepad

Standard Gamepad mapping (W3C): buttons 0 to 3 are A B X Y (south east west north), 4 and 5 bumpers, 6 and 7 triggers, 8 Select, 9 Start, 10 and 11 stick clicks, 12 to 15 D-pad, axes 0 and 1 left stick, 2 and 3 right stick. The RetroArch core maps RetroPad to the same order; keyboard arrives as pad 1 on the game station, so one keyboard seat exists by accident and the control card should mention arrows and Z X for it.

Default arcade scheme:

- Left stick: drive (Y throttle, X steer). Hover and melty: left stick is direction of travel.
- Right stick: camera nudge only, or aim for hover. Not needed by most builds; new players never touch it.
- A or right trigger: weapon. Hold for spinners (spin up while held, spins down at half rate when released, so you can carry spin through a turn), hold for flamethrower and lifter, tap for flipper, hammer, spike, crusher, saw toggle.
- B: brake (hard stop, kills momentum, costs 0.3 s). Also self-right when inverted (if the build can).
- X: taunt. A 0.5 s bot wiggle with an SFX and a crowd noise. Zero mechanical effect except that it fills the aggression meter 1 point on the judges' card, which is a joke that is also true.
- Left trigger or left bumper: boost, 1.5x speed for 0.8 s, 4 s cooldown, visible as a small teal bar under the HP bar. Not available on treads.
- Start: pause (any player; the pauser's colour frames the screen; any player can unpause).
- Y: hold to show your control card during a fight (your viewport only, translucent).

Tank scheme (per player toggle): left stick Y is left side power, right stick Y is right side power. Weapon on A or right trigger as before. Boost on a bumper. This is how real combat robots are driven and the people who ask for it will ask loudly.

Dead zones: 0.18 radial on sticks. Triggers treated as digital at 0.5.

The one-screen control card (shown under Y, on first join, and as the sixth comic panel in the rules strip): a 640x360 card, char background, bone outline of a generic pad, with six callouts in hazard amber squares joined by rust lines, square corners, each callout is two words max: DRIVE, WEAPON, BRAKE, BOOST, TAUNT, PAUSE. Beneath, one line: "FLIPPED? B TO RIGHT YOURSELF. STUCK? OPS IS COMING." Nothing else. It must be readable in a quadrant at 320x180, so the card has a half-size variant with four callouts (DRIVE, WEAPON, BRAKE, BOOST).

## 9. Bot AI for single-player practice

A utility AI on a 10 Hz think rate with 60 Hz steering, nothing fancier.

- Perception: positions and headings of all bots, own HP and weapon state, enemy weapon type and spin state, a hazard timeline (the AI knows the cycle, like a player who has watched one match), and the arena's cell grid for navigation (no pathfinding beyond "steer toward target, slide along wall").
- Goals, scored every think: ATTACK (approach the enemy's weakest exposed zone, which for a spinner is its spun-down weapon, for a flipper its side, for anything inverted its underside), EVADE (back off and circle while the enemy weapon is at full spin or wound up), HAZARD PUSH (if enemy is within 3 units of an active hazard and I have push advantage, shove), RECOVER (if inverted and I can self-right, or if my spinner is down, retreat and spin up), BAIT (drive across a hazard cycle so the enemy follows into it; only on hard), TAUNT (when the enemy is inverted, 20 percent chance, the single most human thing it does).
- Three difficulties: EASY reacts 0.4 s late, never evades, uses a preset build from the first three; MEDIUM reacts 0.2 s late, evades spun-up spinners, picks a build that counters the player's last build (wedge against spinner, spinner against wedge, 6 wheel against flipper); HARD reacts 0.05 s late, uses hazards, baits, and remembers the player's habits across matches (if the player flips more than twice, it goes low profile next rebuild).
- Steering: arcade steering with a PD controller on heading error, a stuck detector (no progress for 2 s, reverse 1 s and pick a new approach angle, same as DO-OCRACY's bot_think recovery), and a corner escape (if within 1 unit of two walls, drive along the longer wall).
- Weapon rules: spinners hold A continuously unless RECOVER says otherwise; flippers fire only when the enemy's centre is within the flipper's 0.6-unit arc and the gauge is full; hammers fire when the enemy is inside 1.2 units and the predicted position in 0.25 s is still inside; flamethrowers fire in bursts of 1 s when within 2 units; crushers close on any contact.
- Personality: the AI also drives the house robot (ATTACK the stalled bot with a lifter, RETURN to the Corner Patrol Zone) and the attract screen (two MEDIUM AIs, random presets, random arena, so the attract screen is never the same twice).
- The AI must play a complete match in headless mode with no renderer, because the round-robin balance test in section 3 depends on it. Build it in a `sim/` folder with no dependency on three.c.

## 10. Structure note for the implementer

- Separate folder beside `do-ocracy/`, its own `build.sh`, its own cart. Share nothing by copy except the tile floor idea and `hud.c` primitives, which can be a common `shared/` include if both games agree on it; otherwise copy the file and note it.
- Suggested source split: `main.c` (host, pads, viewports, phase flow), `sim.c` (deterministic fixed-step physics and damage, no rendering, used by headless tests and replays), `ai.c`, `build.c` (the parts catalogue and budget), `arena.c` (six arena definitions and hazard timelines as data), `render.c` (three.c scene, split viewports, particles), `hud.c` and `draw.c` (CPU HUD), `content.h` (every stinger, blurb, joke, in one place so the house rules can be checked with one grep).
- Physics: a 2D rigid body sim on the arena plane (position, heading, velocity, angular velocity, mass, a convex hull per chassis) with a height channel for flips and launches (vertical velocity and an inverted flag). Full 3D physics is not needed and would cost the Pi too much. three.c only draws what the 2D sim says.
- Determinism: fixed-step 60 Hz, integer or consistent float math, one seeded RNG per match. Replays, headless balance tests and the attract screen all depend on this.

## Sources consulted

Public pages used to check real-world details; nothing here quotes private material.

- BattleBots season 3 rules: https://runamok.tech/rules/BB_season3_rules.html
- BattleBots arena hazards (IEEE Spectrum): https://spectrum.ieee.org/inside-story-of-battlebots
- BattleBots vertical spinners: https://battlebots.fandom.com/wiki/Vertical_Spinners
- NHRL common robot types: https://wiki.nhrl.io/wiki/index.php/Common_Robot_Types
- SparkFun weapon types: https://news.sparkfun.com/2454
- Robot Wars arena and house robots: https://robotwars.fandom.com/wiki/Robot_Wars_Arena/Series_8-10 and https://robotwars.fandom.com/wiki/House_Robots and https://robotwars.fandom.com/wiki/Corner_Patrol_Zones and https://robotwars.fandom.com/wiki/Drop_Zone
- Robot Arena 2: https://en.wikipedia.org/wiki/Robot_Arena_2:_Design_and_Destroy
- Robot Rumble 2 (Steam): https://store.steampowered.com/app/884180/Robot_Rumble_2/
- Translational drift (melty brain): https://en.wikipedia.org/wiki/Translational_drift and https://nothinglabs.com/?p=837 and https://wiki.nhrl.io/wiki/index.php/Project_LiftOff
- Custom Robo Arena review (part comparison UX): https://gamesradar.com/custom-robo-arena-review/2
- Armored Core VI garage structure: https://progameguides.com/armored-core-6/armored-core-6-mech-customization-guide/
