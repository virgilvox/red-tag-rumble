# RED TAG RUMBLE: graphics deep design (models, damage, effects, arena)

Lens: what the bots, the damage, the effects and the arenas look like, and what they must look like. Written against main at 22c05b6 in the snapshot worktree `_wt/rtr-deep-graphics` (nothing committed; a temporary draw counter was built there and reverted). Every claim below has a file and line, a screenshot under `build/shots/` of that worktree, or a number from the counter. The physics, controls and audio lenses are written separately; section 7 is the interface this design needs from them.

Shot sets referenced: `gp-<frame>.png` is `tools/scripts/garage-parts.json` at the frames of `garage-parts.frames`; `f2-<frame>.png` is `2p-fight.json`; `f4-<frame>.png` is `4p-fight.json`; `ks-<frame>.png` is `keyboard-solo.json` under `STATION=1`; `feel-<frame>.png` is `feel.json`.

## 0. Findings in one page

1. Ghost robots. `world_sync` (src/world.c:1888 to 1891) forces every slot below `m->nbots` visible and draws it, and `sim_init` (src/sim.c:1944 to 1951) sets `nbots = OPS_SLOT + 1 = 5` whenever OPS is present, which is every real match. In a 1 or 2 player fight the zeroed sim slots 2 and 3 (x = z = 0, alive 0, hp 0) are drawn as dead chassis at the arena origin, wearing whatever meshes those slots last held: the boot warm up set (a bone LOWBOY with a HORIZONTAL and a bare FRAME MELTY, src/world.c:2019 to 2023) or the previous 4 player session's bots. `f2-680.png`, `f2-1100.png`, `f2-900.png` and `ks-700.png` all show a pale slab with the digits 3 and 4 and a teal MELTY LED sitting on the LASER ROOM bed or in THE BAY's centre. The sim has no body there, so a player who drives into it passes through it. This is the "robots on the field that do not register as rigid bodies" report. Fix in section 1.6 (one line in world.c plus a guard).
2. Damage does not show because the panel system cannot show it. Dents move interior vertices of a 4 x 4 plane by 0.02 to 0.09 u with the border pinned (src/world.c:152 to 178), invisible from a chase camera 3.5 u back. The per hit local dent (src/sim.c:455 to 460) requires `dmg > 20`, and `HIT_CAP` (src/sim.h:159) caps every weapon hit at 20 percent of 100 HP, so on every chassis but SHELL that branch never runs: panels only dent by the HP thresholds 70, 50, 30, all six at once. A shed panel (state 4) reveals a body face of the same shape in a mixed version of the same paint, so nothing visibly leaves. `f2-2600.png`: ROLL OUT at 1 HP, intact silhouette, smoke only.
3. Scorch decals are dead code: `world_scorch` (src/world.c:1934) has no caller anywhere in src.
4. Debris colour is the seat index. `shed_panel` and `burst` (src/sim.c:376, 395) pass `b->seat` as the `colour` argument of `debris_spawn` (src/sim.c:343), and `debris_colour` (src/world.c:1691) reads it as a palette: seat 1's plates fall in its paint, seat 2's in its armour tint, seat 3's in char steel and seat 4's in cardboard brown.
5. The render budget is already blown by the crowd, not the bots. Measured (section 1.1): 22.2k to 22.5k triangles per viewport in a 2 player LASER ROOM fight against the 15k per view of spec 14.2. `crowd_geometry` builds each figure from four boxes (48 triangles, src/world.c:181 to 196) and `build_crowd` places 240 of them with frustum culling off: 11.5k triangles per view for a background. Spec 14.2 asked for 240 flat quads (480 triangles).
6. The garage lights the wrong side. The garage sun sits at (22, 12, 6) (src/world.c:1862) while every turntable camera looks from +z toward -z (src/camera.c:557 to 578), so the faces the player sees are in the hemisphere's char ground light. Every char or steel paint reads black (`gp-172.png`, `gp-422.png`, `gp-1392.png`).
7. Models are primitive boxes with no edges, no bolts, no mechanism: a WEDGE weapon is a slab the size of the bot, TREADS are two dark boxes with a 4 px stripe map, HORIZONTAL is a flat disc with three slivers, LEG TRACKS and CRUSHER do not read at all, armour changes only a tint, paint schemes 1 and 2 are nearly identical dark bodies. Section 1.2 has the per part list.
8. `wheel_hit` (the TWO WHEEL side hit, src/sim.c:764) has no render consumer: nothing wobbles.
9. The HORIZONTAL spin ring at `J_SPIN_RING_A 160` is a bone halo wider than the bot that hides the weapon it announces (`f2-1100.png`, `f2-3243.png`).

## 1. Current state

### 1.1 Measured render cost (a temporary `[draw]` probe reading `t3_renderer_info` after each viewport render, every 60 frames)

| Scene | Calls per viewport | Triangles per viewport | Spec 14.2 |
|---|---|---|---|
| 2 player, THE LASER ROOM, countdown and fight (2p-fight 540 to 660) | 75 to 81 | 22,233 to 22,481 | under 150 calls, under 15k |
| 4 player, THE BAY, mid fight (4p-fight 1980 to 2160) | 63 to 86 | 20,629 to 21,939 | same; 4 views on screen 84k against the 25k on screen line |
| 1 player full view, THE BAY (keyboard-solo 780 to 1200) | 31 to 73 | 19,019 to 21,013 | |
| Garage, 4 turntables (garage-parts 180 to 300) | 31 to 39 | 7,757 to 8,261 | |
| Boot title before the arena attaches (2p-fight 120 to 240) | 34 to 41 | 7,701 to 8,201 | |

The crowd is 11,520 of every arena number (240 x 48). Without it an arena with two bots is about 10.7k, so the bots themselves are nowhere near the limit: a CHOP (LOWBOY, FOUR WHEEL, HORIZONTAL) is roughly 900 triangles today (four wheels at 80, six visible panels at 32, chassis, disc 48, hub 32, three teeth, the blur disc at 80, two rings at 48, blob, seat ring). Draw calls are fine everywhere (auto instancing of the shared `g_box`, `g_cyl12` and material cache works). `skipped` was 0 in every log: no missing render state.

### 1.2 What each part looks like now, and what is weak

Scale: 1 u is about one metre of arena; a BOX chassis is 1.0 x 0.8 x 0.40 u (src/parts.h:81). The chase camera sits 3.5 u back and 2.2 u up with a 36 to 50 degree fov (src/camera.c:321 to 323), so a bot is about 180 px wide in a half, 120 px in a quadrant. Anything under 0.03 u is sub pixel in a quadrant.

Chassis (`build_chassis`, src/world.c:1244 to 1345):

| Part | Now | Weak |
|---|---|---|
| BOX | one `g_box` in the paint, panels 0.012 u proud of it, a band box at the bottom | no edge, no lid line, no latches; reads as a dark brick (`gp-422.png` seat 2) |
| WEDGE | a half box plus `wedge_geometry` (5 faces) | the slope is a flat plane with no lip, no skids, no bolt line; with the WEDGE weapon in front it becomes two stacked slopes (`gp-892.png` seat 1) |
| LOWBOY | the default box at H 0.25 | nothing says server chassis: no vents, no handles, no rack ears (`gp-172.png` seat 1) |
| SHELL | a 6 point lathe at 14 segments (src/world.c:1264) | a smooth dome with no rim, no rivets, no hinge; the dome plane digits are a nice touch and should stay (`gp-222.png` seat 1) |
| FRAME | four posts and eight rails of 0.07 u tube, an electronics box, a teal LED (src/world.c:1271 to 1278) | reads well in the garage; in a fight the 0.07 tubes vanish and the box inside is the bot (`gp-272.png` seat 1) |
| LUNCHBOX | a tall box with a handle and a tape seam | fine silhouette; the handle is three steel sticks (`gp-372.png` seat 1) |

Drives (`build_drive`, src/world.c:1010 to 1080):

| Part | Now | Weak |
|---|---|---|
| TWO WHEEL | two `g_cyl12` tyres r 0.24 x 0.14 wide, a hub cylinder, a sphere caster | tyres are flat black discs; the hub is the only highlight; no tread, no spokes, no motor (`gp-782.png` seat 2) |
| FOUR WHEEL | four r 0.17 tyres | same; the wheel loss at HP 30 just hides one (src/world.c:1582) |
| SIX WHEEL | six r 0.15 tyres | same; all wheels one shade, no axles |
| TREADS | a 0.26 x 0.30 box per side with a 32 x 8 stripe map scrolled in v, three roller cylinders inside (src/world.c:1030 to 1040) | the stripe is 4 px wide and reads as dark box; no sprocket, no road wheels showing, no tread segments; the scroll is invisible (`gp-832.png` seat 1, `f2-680.png`) |
| HOVER | a `g_torus` skirt r 0.6 L, two cylinders on top (src/world.c:1053 to 1061) | the char torus under a char body is a black blob; no skirt pleats, no fan intake; the body floats 0.22 u over a donut (`gp-482.png` seat 1) |
| WALKER | six 0.08 x 0.34 leg boxes with rust feet and a hip bar, gait in `sync_drive` | the only drive that reads; the legs are straight sticks with no knee (`gp-632.png` seat 3) |
| LEG TRACKS | per side two tilted "knee" boxes and two 0.24 x 0.22 x 0.5 pods in the tread map | unreadable: a black mass under the bot (`gp-482.png` seat 4, `gp-732.png` seat 2) |
| MELTY | two wheels and four bright edge bars at the corners | the bars are the look; fine, but the LED heading intent at 0.06 u is a dot |

Weapons (`build_weapon`, src/world.c:1090 to 1231; posed in `pose_weapon` 1459 to 1528):

| Part | Now | Weak |
|---|---|---|
| WEDGE | a second `wedge_geometry` W x 0.55 x (ride + 0.95 H) steel slope with an amber lip strip at the tip | as tall as the chassis and 0.55 u deep: it is a second bot stacked on the front; no hinge, no skids, no scuffed edge; the amber strip is the one thing that reads (`gp-892.png` all four) |
| SPIKE | a block, a rod cylinder and a cone tip; the rod translates 0.45 u in `active` | fine mechanism, too thin (r 0.05) to see from the chase camera |
| LIFTER | two forks 0.08 x 0.05 x 0.62 along the floor, a crossbar, a hinge block; pivot rotates by `lift` | under a BOX the forks are below the body line and invisible (`gp-992.png` seat 2); on the WEDGE they stick out and read |
| SAW | an arm, an axle and a 0.3 r disc with ten tooth boxes | good shape; the disc is bright steel and reads; the arm is a stick |
| FLAME | a rust tank box, a nozzle tube, a bright collar and a glass cone | reads; the tank is a plain box, no hose, no valve, no pilot glow (`gp-1092.png`) |
| HAMMER | two posts, a 1.1 u arm, a rust head with an amber stripe; windup, slam, return | the best weapon on screen; the head is a plain box, no pivot pin, no counterweight (`gp-172.png` seat 4) |
| FLIPPER | a 0.05 thick plate hinged at the back, an amber lip | the plate is the whole lid so at rest it is invisible; no hinges, no piston, no gap line (`gp-1192.png` seat 2) |
| DRUM | a W wide cylinder with four tooth bars, rotates about x | fine but the teeth are flush bars; no end plates or bearing blocks |
| HORIZONTAL | a 0.55 r disc of 0.06 u, a hub, three 0.14 x 0.08 x 0.22 teeth, a glass blur disc | from above it is a flat grey pancake; the teeth are slivers; the bone ring halo at full spin covers everything (`gp-1292.png`, `f2-1100.png`) |
| CRUSHER | a fixed lower jaw, two upper arms, a rust bar and one cone tooth | reads as four rust bricks in a pile (`gp-1292.png` seat 1) |

Armour (`armour_mat`, src/world.c:954 to 964): a material tint mixed 50 to 60 percent into the body colour and a roughness/metalness pair. Side by side STEEL, ALUMINIUM and WEAR PLATE are three greys (`gp-1502.png`, `gp-1752.png`). The spec promised "BRIGHT DENTS. VERY VISIBLE" for aluminium and "nearly no dents" for wear plate; neither can show since dents do not show.

Paint (src/world.c:945, 1295 to 1303): body and stripe from `PAINT_SCHEMES`; the stripe is a 0.18 W wide box on the lid, decals are three 0.08 u hazard bars or a 0.14 u tape band. Scheme 1 (seat body, char stripe) and scheme 2 (char body, seat stripe) are the only two that differ; 3 (bone) and 4 (steel) are grey bots with an invisible stripe (`gp-1912.png`, `gp-2162.png`). The seat band is 0.08 u and sits under the panels, so on seats 3 and 4 (bone, rust) it is lost against bone and rust paint.

Digits (`mat_digit`, src/world.c:276 to 284): captured from the HUD font into 128 x 128 textures, placed on both sides and the lid. These read well and are the one thing that identifies a bot at a glance. Keep.

### 1.3 Damage visuals today

- Panels: six planes per bot, four pre-built geometries each (clean, dent 1, 2, 3), swapped by visibility in `sync_panels` (src/world.c:1530). Dents are interior vertex pushes of 0.02, 0.05, 0.09 u (src/world.c:155). The silhouette never changes. At HP 70, 50, 30 every panel moves to the same state (src/sim.c:405 to 406).
- Shed: at HP 20 (50 for FRAME and AR_NONE, then every 10 for AR_NONE) one random panel goes to state 4 and a sim debris plate flies (src/sim.c:368 to 378). The body face beneath is a solid box in the same mixed paint: nothing is exposed.
- Burst at 0 HP: all panels and the wheels become debris (src/sim.c:388 to 399); `sync_bot` hides wheels, treads and weapons (src/world.c:1576 to 1579) and leaves the chassis box. `f2-3243.png`: the KO victim is an intact amber box with a hidden drive, standing on nothing.
- Debris: one instanced `g_box` with per kind scales (src/world.c:1715); wheels are 0.3 x 0.3 x 0.14 boxes; colour by the seat index bug (finding 4).
- Smoke and fire: `fx_bot_damage_state` is called with absolute HP (src/world.c:1597); grey puffs under 50, black and fire under 30 (src/fx.c:243 to 264), from the top centre of the bot whatever side was hit. Soft discs faced per viewport. They read (`f4-1800.png`).
- Sparks: `spawn_sparks` sends every spark in a 50 degree cone around the reflected normal with dy 0.7 up, speed 4 to 9, gravity 14, one bounce (src/fx.c:89 to 114). Streaks along their velocity (src/fx.c:316 to 328). Hit stop holds them at the contact (held = 1). Colour by victim armour. They read as bright thin lines (`feel-864.png`). They do not follow the weapon: a hammer blow and a spinner graze spray the same cone back toward the attacker.
- Flash: one additive bone disc of `0.55 x band flash` for one frame then three shrinking frames (src/fx.c:152, 279). It is the pale blob behind the sparks in `f2-3243.png`; it adds little.
- Scorch: `world_scorch` paints two char circles into the floor texture and bumps the version (src/world.c:1934 to 1946) and nobody calls it.
- Spinner tells: two rings at the weapon radius at `spin x 160/255` and `x 50/255` alpha (src/world.c:1586 to 1593) and a glass blur disc at 50/255 (src/world.c:1472). The ring is bone, 0.1 u wide, and at full spin is the brightest thing in the frame.
- Launch: six dust cubes at the launch point, ten on landing (src/fx.c:202 to 203). Nothing follows the flying bot.

### 1.4 Effects today (fx.c)

One instanced unit cube mesh of 400 for sparks, chips, fire, dust, confetti and pickup cubes; one instanced disc mesh of 160 for smoke; four flash discs. Everything is `basic+inst+icol` (unlit, per instance colour). The budget line is kept (spec 13). What is missing is direction and shape: every particle is a cube or a disc, every burst a cone.

### 1.5 Arena look today

- Floors: painted 1024 wide canvases (`paint_floor`, src/world.c:344 to 424) with tiles, tape, amber rings, spawn squares and a wall shadow pool. Good and cheap. Keep.
- Walls: lexan planes at 35 percent opacity with rails and posts (src/world.c:493 to 523). The lexan reads as flat green glass with no reflection, no edge highlight, no smudges (`f2-680.png` top). On THE ROOF a brick parapet.
- Lights: hemisphere 1.2, sun 2.0 at (6, 12, 4), two points per arena (src/world.c:1976 to 1986, src/arena.c:71 to 254). No shadows (by design); blob shadows are circles at alpha 0.35 scaled by `1 - y/3` (src/world.c:1363, 1439 to 1441). A circle under a rectangular bot reads as a floating puck.
- Hazards: emissive tells pulse at 2 Hz (src/world.c:1732), the pit doors slide, the pulverizer swings, the saws rise and spin, the flame cones scale, the laser beam slides. These read (`f2-680.png`, `f2-3100.png`). The killsaw slot tell is a 0.02 u thick emissive plate flush with the floor: easy to miss from a chase camera.
- Crowd: 240 soup figures, 3 frames, instanced; finding 5.
- Props: benches, shelves, boxes, HVAC, signs, servers from boxes (src/world.c:548 to 608). Fine for a background.

### 1.6 Bugs to fix before any new art (small, high value)

| Bug | Where | Fix |
|---|---|---|
| ghost bots at the arena origin | src/world.c:1890 `if (!b->visible) world_bot_visible(i, 1);` | a slot is drawn only if it is a live sim bot: `if (bt->hp_max <= 0 && !bt->is_ops) { if (b->visible) world_bot_visible(i, 0); continue; }` and never force visibility from `world_sync`; `game_flow.c:459 to 463` stays the owner of visibility. Acceptance: `f2-680` shows an empty bed; a probe that drives slot 0 through (0, 0) in a 2 player match passes nothing |
| scorch never painted | src/world.c:1934 | main.c `drain_events` calls `world_scorch` on EV_HAZ_HIT from HZ_FLAME and HZ_LASER (r 0.5), on EV_BURST (r 0.9) and on EV_KO with cause KO_HP (r 0.9); section 4.3 adds the bot decals |
| debris colour is the seat | src/sim.c:376, 395 | pass 0 (body paint) for DB_PANEL and 2 (steel) for DB_WHEEL; the physics owner changes it, the renderer's `debris_colour` table stays |
| local dent never triggers | src/sim.c:455 | `dmg >= BAND_HEAVY_MIN` (11 after armour); section 7.1 |
| `wheel_hit` has no look | src/world.c `sync_drive` | section 4.6 |
| crowd 11.5k triangles | src/world.c:181 to 196 | section 5.4 |
| garage sun behind the bots | src/world.c:1862 | section 3.8 |

## 2. Principles and budget for the redesign

1. Silhouette first. A part must read at 120 px wide in a quadrant. Every weapon gets a second colour (bright steel, rust or hazard amber) on its working edge and a dark pivot so motion is visible. No detail under 0.03 u unless it is a line of them.
2. Paint on the plates, raw frame under them. The chassis body becomes char steel (0x3a342b, rough 0.5, metal 0.6) and the paint scheme lives on the six panels, the lid and the band. A missing panel then reveals raw steel, bolt holes and internals: damage shows by subtraction, which is free.
3. Bevels from geometry, not from maps. `soup_t` already gives flat normals; a chamfered box (section 3.1) costs 44 triangles and catches the sun on every edge, which is what makes a dark bot read in a dark arena.
4. One draw per part where possible: teeth, bolts, tread lugs are soup geometries, never one mesh per tooth.
5. Budget per bot: 1,200 triangles with every part, 1,500 for the heaviest legal build (SIX WHEEL plus HORIZONTAL plus a 2ND), under 40 scene objects per bot. Five bots 7,000. Arena under 8,000 with the crowd at 2,400. Particles 1,200. Target per view: under 15k in a 2 player fight and under 60 calls, which the crowd fix alone almost reaches (22.2k minus 9.1k = 13.1k before the new bots).
6. Zero new shader programs unless named in section 6; everything here is reachable with `standard+l1201`, `standard+map+l1201`, `basic+map+trans`, `basic+trans`, `basic+inst+icol` and their `instbig` and `ds` variants, which are all in `tools/states.txt` already.

## 3. Model design

Coordinates: local +z forward, +x right, y up from the floor; `ride_y` is the chassis underside height set by the drive (src/world.c:1016 to 1073). L, W, H from `CHASSIS[]`.

### 3.1 The geometry kit (new static helpers in world.c, next to `wedge_geometry`)

All return a `t3_geometry` built with `soup_t`; all are cached in a tuple keyed cache like `mat_ex` (a `geo_slot_t { int kind; float a, b, c, d; int n; t3_geometry *g; }` array) so a rebuild never allocates twice.

| Helper | Shape | Triangles | Used by |
|---|---|---|---|
| `chamfer_box(w, h, d, c)` | box with every edge cut at 45 degrees by c | 44 | chassis bodies, weapon heads, hazard blocks |
| `plate(w, h, t, state, seed)` | a panel with thickness t: front 4 x 4 (32), four edge strips (8); states 1 to 3 as section 4.1 | 40 | the six panels |
| `tyre(r, w, seg)` | lathe of a 6 point profile (rounded shoulders, a shallow centre groove), seg 12 | 120 | wheels |
| `tooth_ring(n, r, w, h, d, tilt)` | n wedge teeth around radius r, each 4 faces, leading face tilted | 8n | HORIZONTAL (n 4), DRUM (n 6), SAW (n 12), killsaws |
| `hex_bolts(list, r, t)` | hexagonal bolt heads at the listed local points, each a 6 sided cap and 6 side quads | 18 per bolt | panels, wedge lip, hammer pivot |
| `tread_loop(L, h, w, n)` | a closed belt: top, bottom, two rounded ends (8 segment half cylinders) with n raised lugs as thin boxes on the outer face | 64 + 12n | TREADS, LEG TRACKS pods |
| `skirt(r, pleats)` | lathe of a 5 point profile (bulge out then tuck under) at 12 segments, with `pleats` shallow radial dents in the profile ring | 120 | HOVER |
| `leg(seg_a, seg_b, foot)` | two chamfer boxes and a foot pad under one pivot group with a knee group | 44 x 3 | WALKER, LEG TRACKS |
| `lathe_rim(r, h)` | an 8 point rolled rim ring | 96 | SHELL rim, HOVER top ring |

`soup_quad` already winds counter clockwise seen from outside; the chamfer and plate builders must keep that (the wedge comment at src/world.c:135 to 137 is the rule).

### 3.2 Chassis

Common to all box shapes: body `chamfer_box(W, H, L, 0.035)` in char steel; a 0.02 u recessed lid line (a thin char box 0.01 proud at `ride + H - 0.03`); the seat band as `chamfer_box(W + 0.04, 0.08, L + 0.04, 0.012)` at `ride + 0.10` in the seat colour (unchanged height, now bevelled so it catches light); 8 hex bolts on each side panel's corners (section 3.1); a belly pan when inverted: a char plate with two motor cans (`g_cyl8` r 0.06 x W 0.6, bright steel) and a battery `chamfer_box(0.3, 0.1, 0.18)` in rust with a hazard amber stripe, visible from below and after a side panel is stripped (section 4.2). Today an inverted bot shows a blank bone plane (`ks-1800.png`).

| Chassis | Specific |
|---|---|
| BOX | two latches: `chamfer_box(0.08, 0.05, 0.03)` bright steel on the front face at +-0.25 W, y `ride + 0.6 H`; two rubber corner bumpers `chamfer_box(0.06, H, 0.06)` char at the rear corners |
| WEDGE | the slope stays a soup wedge but gets a 0.04 u lip bar across its front edge (bright steel), two skid runners under the tip (`chamfer_box(0.06, 0.03, 0.3)` steel at x +-0.3 W), and three bolt heads along the lip; the back half body is the chamfer box |
| LOWBOY | the body at H 0.25 gets four vent slots on each side (char boxes 0.12 x 0.03 x 0.01 recessed, one draw each shared geometry), two rack ears `chamfer_box(0.05, 0.2, 0.05)` bright steel at the front corners, a 2U handle bar on the lid (`g_cyl8` r 0.015, bright) |
| SHELL | lathe stays at 14 segments but the profile gains a rolled rim: `{0,0} {0.42L,0} {0.5L,0.3H} {0.52L,0.36H} {0.5L,0.42H} {0.45L,0.8H} {0.25L,H} {0,H}` (8 points, 196 triangles); twelve rivets round the rim (`hex_bolts`); a hinge block on the back. The dome plane digits stay (src/world.c:1235) |
| FRAME | tube radius 0.05 (was 0.07 boxes) as `g_cyl8` so they catch a highlight, cross braces on the sides (two diagonals per side), the electronics box as a bone `chamfer_box` with a teal LED and a rust battery beside it; armoured FRAME panels at 0.55 size are kept |
| LUNCHBOX | the body as two stacked chamfer boxes (lid 0.3 H in the stripe colour, base 0.7 H in the paint) with a char hinge line and a bone handle arch (`g_torus` half: `t3_torus_geometry_new(0.18, 0.025, 6, 10, PI)`), two latches at the front |

OPS (src/world.c:1414 to 1421): LUNCHBOX at 1.4 L, STEEL, TREADS, LIFTER and SAW. Keep the build; add a hazard amber beacon `g_sphere` r 0.08 emissive 2.0 on a 0.3 mast that blinks at 2 Hz when `ops_state != OPS_PARKED`, the tape bumper stays, and an "OPS" plate on both sides (already there through `mat_digit[4]`).

### 3.3 Drives

| Drive | Build | Animation (sync_drive) |
|---|---|---|
| TWO WHEEL | two `tyre(0.24, 0.16)` in rubber 0x1d1915 with a `standard+map` tread canvas (32 x 8 lugs, repeat 8 round the tyre); hub `g_cyl8` r 0.11 bright steel with four spoke boxes 0.03 x 0.09 x 0.02 char; a motor can `g_cyl8` r 0.06 x 0.2 inboard of each wheel; a skid caster sphere stays | wheel spin as now; `wheel_hit` wobble section 4.6 |
| FOUR WHEEL | four `tyre(0.17, 0.14)` with hubs; an axle `g_cyl8` r 0.025 bright between each pair | spin; the wheel loss at HP 30 drops the wheel as debris (section 7.1, EV_SHED zone 8 to 15 = wheel index) and shows a bare axle stub |
| SIX WHEEL | six `tyre(0.15, 0.13)`; the middle pair sits 0.01 lower (real six wheel drives rock on the middle pair); axles | spin; a 1.5 degree body rock on throttle change (`drive_t` delta, src/sim.h:83) |
| TREADS | `tread_loop(L + 0.15, 0.3, 0.26, 10)` per side in rubber with lugs in 0x4a4338; a drive sprocket `tooth_ring(8, 0.1, 0.26, 0.03, 0.04)` at each end, visible through a 0.04 u gap under the belt; three road wheels `g_cyl8` r 0.08 bright between | lug scroll: the loop geometry is static and the lugs are a second thin instanced strip? No: scroll the tread canvas as now (`offset.y`) on the belt faces, and rotate the two sprockets by `speed / 0.1`; that is what reads as treads moving |
| HOVER | `skirt(0.6 max(L, W), 8)` in rubber, a bone `lathe_rim` at the skirt top, two fan intakes `g_cyl8` r 0.14 x 0.05 on the lid with a 4 blade cross (two boxes) char inside, a rust fuel can; the body floats at `ride_y 0.22` as now | fan cross spins at 60 rad/s while `alive`; the skirt scales y by `1 - 0.25 x (skirt_t / 24)` when pinched (src/sim.h:89 `skirt_t`); a 1 Hz 0.01 u bob and a 2 degree lean into the velocity (`vx, vz` dotted with right and forward) |
| WALKER | six `leg()` groups: hip `chamfer_box(0.1, 0.1, 0.1)` at the body edge, thigh 0.08 x 0.2 at 25 degrees out, knee group, shin 0.07 x 0.22, rust foot pad 0.16 x 0.04 x 0.14; a steel hip bar | the gait keeps `b->gait` (src/world.c:1544) and adds a knee bend: shin rotation `0.5 x max(0, cos(ph))`, the foot stays flat (shin counter rotates the pad); a 0.03 u body bob stays |
| LEG TRACKS | per side two `leg()` groups ending in a `tread_loop(0.5, 0.22, 0.24, 6)` pod instead of a foot; the knee is the visible joint | the pods pitch +-0.2 with the gait (the knee), the pod lug canvas scrolls |
| MELTY | two tyres, four `chamfer_box(0.12, 0.9H, 0.3, 0.02)` edge bars in bright steel with a hazard amber chevron decal on each (basic+map quad), the LED becomes a 0.1 u emissive bar on the lid | body yaw from `spin` as now; at spin over 0.5 the four bars get a `basic+trans` blur ring (section 4.8) |

### 3.4 Weapons: build, idle and firing animation per 60 Hz state

The pivot convention stays (`wpivot`, `w2pivot`). Every weapon has four visual states driven from `bot_t` fields the sim already exports (src/sim.h:71 to 72): READY (cooldown 0, idle), TELL (windup or spin up), STRIKE (active), RECOVER (cooldown). A weapon that is READY shows it: a 2 frame bone glint quad (`basic+trans` 0.12 x 0.12 at the working edge, alpha 0.8 then 0) the frame `cooldown` reaches 0, and a resting pose that differs from the recover pose. This is the graphics half of "it is not clear how to attack": the weapon visibly cocks.

| Weapon | Build | READY | TELL | STRIKE | RECOVER |
|---|---|---|---|---|---|
| WEDGE | plate `plate(1.05W, 0.5, 0.03)` steel at 28 degrees from the floor, tip at z L/2 + 0.42 (was 0.55 deep and as tall as the bot); two skids under the tip; a hinge bar `g_cyl8` r 0.03 across the chassis front; a bright lip bar; three bolts | the plate rests 0.01 above the floor | none (passive) | on EV_RAM the plate dips 0.02 for 6 frames and the hinge bar flashes | none |
| SPIKE | rod r 0.08 (was 0.05) with a square guide block, a bright 0.22 cone tip, a rust spring collar (`g_torus` r 0.06) | tip retracted 0.05 inside the guide, glint on the tip | none (tap) | rod extends 0.55 over `active` 9 frames: out in 3, hold 3, back in 3 (was a triangle over 9) | collar compresses (scale y 0.6) over the first 20 cooldown frames |
| LIFTER | forks 0.1 x 0.06 x 0.62 with a bevelled toe, a crossbar, two hinge blocks and a hydraulic ram (`g_cyl8` r 0.04 bright, length follows the lift) | forks 0.02 off the floor | `lift` 0 to 1 rotates -0.9 as now; the ram extends | carried bot pose stays | forks lower over 24 frames |
| SAW | disc r 0.3 as `g_cyl12` 0.03 plus `tooth_ring(12, 0.3, 0.03, 0.06, 0.05, 0.3)`; arm `chamfer_box(0.12, 0.1, 0.72, 0.02)`; a belt guard half ring at the hub | disc idles at 2 rad/s (the motor is live); a bone glint on the top tooth | `saw_on`: spin to 50 rad/s over `spin` as now; the blur disc (4.8) fades in over spin | on an EV_HIT with wid SAW: 2 frames of 6 sparks per tick along the tangent (4.4) | none |
| FLAME | tank as a rust `g_cyl8` r 0.14 x 0.3 lying across the back with a bone valve, a hose (`g_torus` quarter, rubber), nozzle tube, a pilot: a 0.03 u amber emissive sphere at the nozzle (always on when fuel > 0) | pilot on | none | cone opacity 0.55 as now plus a `basic+inst+icol` fire cube emitter at the cone tip (12 per second, kind PK_FIRE, drifting forward at 3 u/s) so the flame has grain; the pilot goes bone | fuel under 30 percent: the pilot flickers at 8 Hz |
| HAMMER | arm `chamfer_box(0.1, 0.1, 1.1, 0.02)`, head `chamfer_box(0.4, 0.3, 0.36, 0.04)` rust with a bright striking face plate 0.42 x 0.3 x 0.02 and a hazard amber band; pivot pin `g_cyl8` r 0.05 bright through two gusseted posts; a rust counterweight block behind the pivot | arm at -0.6 (as now) with a 0.02 rad breathing bob at 1 Hz | `windup` 15 frames: raise to -1.8 as now, plus the counterweight dips and a tell glint on the face plate at frame 10 | `active` 4 frames: slam to `hammer_max`; frame 0 of the slam stretches the head 1.15 along the arm (squash and stretch, scale only); on contact (EV_HIT wid HAMMER) the head holds 2 frames | 72 frames: return to -0.6 with a 0.05 rad overshoot at frame 20 |
| FLIPPER | plate `plate(0.95W, 0.6L, 0.04)` steel over the front half, two visible hinge knuckles `g_cyl8` r 0.04 at the back edge, a piston `g_cyl8` r 0.035 from the chassis floor to the plate's underside (length follows the angle), a bright lip, and a 0.02 u shadow gap line round the plate so it reads as a separate plate at rest | plate 0.02 raised (a visible gap), piston at rest | none (tap) | `active` 4: plate to -1.1 as now, the piston extends to 0.45; add 4 frames of hold (render only, the sim stays at 4) | 108: back over the first 30 percent as now; the plate lands with a 0.03 bounce at frame 34 |
| DRUM | drum `g_cyl12` r 0.3 x (W - 0.1) with two bright end plates `g_cyl12` r 0.32 x 0.03 and `tooth_ring(6, 0.3, W - 0.1, 0.06, 0.08, 0.5)`; bearing blocks `chamfer_box(0.1, 0.3, 0.3)` either side with a bolt each | drum idles at 1 rad/s | `spin`: rotation as now; blur cylinder (4.8) | hit: sparks up and back (4.4); the drum jolts -0.1 rad for 2 frames | none |
| HORIZONTAL | disc `g_cyl12` r 0.5 x 0.05 steel with `tooth_ring(4, 0.5, 0.05, 0.1, 0.16, 0.4)` in bright steel (four fat teeth, not three slivers), a 0.2 r hub cap with 6 bolts, a bearing tower `chamfer_box(0.2, 0.22, 0.2)`; a hazard amber ring decal on the disc top (basic+map quad) | disc idles at 0.5 rad/s; the amber ring is visible | spin up: rotation as now; the amber ring becomes the blur ring (4.8); the spin rings stay but at 90/255 and 30/255 alpha (section 4.8) and 0.05 wide | hit: tangent sparks; `knock` reaction already moves the bot | spin bleed shows as the blur ring thinning |
| CRUSHER | a fixed lower jaw `chamfer_box(0.5, 0.08, 0.9)` with three bright teeth (`tooth_ring` is wrong here: three `g_cone` r 0.05 h 0.1 pointing up), an upper jaw as two arms joined by a bar with three teeth pointing down, a pivot pin, a hydraulic ram `g_cyl8` from the chassis lid to the upper arm | jaw open at -0.5, ram retracted | `clamp` 24 frames closes as now; the ram extends | `lock`: the jaw at -0.05 with a 4 Hz 0.01 rad shiver (the victim is held) | cooldown: opens over 30 frames |

Secondary mounts (src/world.c:1104 to 1115) keep their bracket logic; the bracket becomes a `chamfer_box` with two bolts.

### 3.5 Armour

The six panels carry the scheme's body paint tinted by the armour (as `armour_mat` does) but the plates now have thickness, bevels, bolts and material contrast so the armour reads at a glance:

| Armour | Plate look |
|---|---|
| NONE | no plates: the raw char body with its bolt holes (dark hex decals) and internals visible through the FRAME; on a box chassis the lid stays painted, the sides are bare steel with the paint only on the band and lid |
| PLYWOOD | plates in a plywood tint 0.6 mixed, rough 0.9, with a bone edge strip (the ply layers) 0.01 u and four countersunk bolts; dents are splits (section 4.1 state 2 opens a crack line decal) |
| ALUMINIUM | plates metal 0.8 rough 0.25 (brighter than now), a brushed look from a 64 x 64 canvas (fine horizontal lines, `standard+map`), eight bolts; dents are the deepest (x 1.4 depth) and brightest (the dent rim gets a bone scratch decal): "BRIGHT DENTS" |
| UHMW | plates in bone 0.55 mixed, rough 0.55, no bolts (UHMW is bolted from inside): a clean slab with rounded 0.03 chamfers; dents are scrapes (white scratch decals, depth x 0.5) |
| STEEL | plates char steel tinted, metal 0.8 rough 0.3, eight bolts, a weld bead line along the top edge (a 0.015 rust strip); dents x 1.0 with rust scorch decals |
| TITANIUM | plates with a blue grey tint 0x8d97a3 mixed 0.5 and a heat tint stripe (a `basic+map+trans` quad with a bone to amber to teal gradient, 0.08 x 0.6 along each side plate): the titanium rainbow; dents x 0.8 |
| WEAR PLATE | plates 0.05 thick (the others 0.03), rust tint, twelve bolts, a hazard amber corner triangle on each plate (decal); dents x 0.3 and never state 3: "nearly no dents" |

### 3.6 Paint

Keep the four schemes but make every one two tone and visible:

- Scheme 1: seat colour plates and lid, char band stripe along the lid centre (0.18 W x 0.015, stays), char frame.
- Scheme 2: char plates, seat colour lid and stripe, seat band.
- Scheme 3: bone plates, seat colour lid; the band in seat colour; a seat colour chevron on the front plate.
- Scheme 4: steel plates (0x6b655a), hazard amber lid stripe x 2 width, hazard amber front chevron.

The stripe moves from a box to a `basic+map` decal quad on the lid so it can carry the chevron and the seat number plate: a 0.26 x 0.16 plate (bone on char, the seat digit from `mat_digit`, which stays) on the lid's rear and both sides. Decal sets (hazard stripes, tape band, seat number) stay as they are and move to the same decal quad layer at +0.006 above the lid. The seat band is bevelled (3.2) and on bone and rust paints it gets a char pin stripe above and below (two 0.01 boxes) so it reads on seats 3 and 4.

### 3.7 Scale and attachment rules

- Every weapon's pivot is bolted to a visible mount: a bracket `chamfer_box` with two bolts that touches the chassis. `build_weapon` already seats every weapon on every chassis (R4 models-fx); the brackets make the seating read.
- Weapon reach is unchanged (the sim hitboxes in `WEAPONS[]` are the truth): the WEDGE plate tip at L/2 + 0.42 is inside `hb_z1 0.6`; the HAMMER head centre at 1.05 matches `hb_z0 0.5 .. z1 1.0`; the HORIZONTAL disc r 0.5 is under `hb_r 0.55`. A render reach that exceeds the hitbox is a lie the player feels: the acceptance in section 8 checks each.
- Wheels never intersect the body: `build_wheel` x offsets stay at W/2 + 0.08.

### 3.8 Lighting the models so they read

- Garage: the sun moves to the camera side: `t3_object_set_position(sun, 10, 14, 16)` in `world_attach_garage` (direction (0.37, 0.52, 0.59) toward the origin, 31 degrees elevation, front right of every turntable camera), hemisphere 1.3, and the amber point light is placed 2.5 u in front of the turntables at y 2.0 with intensity 5 so the front faces get warmth. Acceptance: `gp-172` seat 2's WEDGE front slope luminance over 35 percent of its lid.
- Arena: sun stays at (6, 12, 4) but intensity per arena is the arena's own (unchanged); the hemisphere ground colour rises from 0x15120f to 0x2a241e so undersides and shaded plates are readable and bots stop going black on the far side of a wall shadow.
- A rim: not available without a third light type; the bevels do that job.

## 4. Damage that shows

### 4.1 Panels: three dent states and fall off (sim `panel_state` 0 to 4, unchanged meaning)

`plate(w, h, t, state, seed)` replaces `panel_geometry`. The silhouette changes per state, which is the whole point:

| State | Geometry | Extra |
|---|---|---|
| 0 clean | flat plate | bolts all present |
| 1 dented | one crease: the two centre rows of the 4 x 4 front face pushed in 0.06 x armour factor, the crease running at a seeded angle; the top edge strip bowed out 0.02 | a 0.2 x 0.1 scratch decal at the crease |
| 2 bent | state 1 plus one seeded corner pulled out 0.1 along the normal and the plate rotated 5 degrees about its opposite edge (the plate is no longer flush: a visible wedge of shadow opens under it); one bolt hidden | a scorch decal 0.25 r at the corner |
| 3 hanging | the plate rotated 14 degrees about its bottom edge (sides and back) or its rear edge (lid), two bolts hidden, the crease deepened to 0.1; it visibly flaps: `sync_panels` adds a 3 Hz 1 degree swing while the bot moves (speed over 0.5) | the raw steel and a bolt hole row show in the gap |
| 4 gone | plate hidden; the sim's debris plate flies (section 4.10); the body face shows bolt holes (six dark hex decals, pre-built hidden with the panel) and the internals of 4.2 | 8 sparks along `nx, nz` (fx_event EV_SHED, already) plus 4 chips in the plate's colour |

Depth factors per armour from 3.5 (ALU 1.4, STEEL 1.0, TI 0.8, UHMW 0.5 and scrapes only, WEAR 0.3 and no state 3, PLY 1.0 with a crack decal). The four geometries per panel are still pre-built at `bot_rebuild` (one plate per state, hidden), so a state change is a visibility swap, never a rebuild (spec 13).

Which panel dents: the sim's local dent (src/sim.c:455 to 460) once the threshold is `>= BAND_HEAVY_MIN` (section 7.1). The HP thresholds (70, 50, 30) stay as a floor so a bot at 30 HP is never clean.

### 4.2 Exposed internals when a side is stripped

Pre-built hidden at `bot_rebuild`, shown when the panel of that side is state 4 (or state 3 for a glimpse):

- Side plates: a wiring loom (three `g_cyl8` r 0.012 tubes in rust, teal and bone along the inside of the body, 36 triangles each), a motor can behind each wheel position.
- Front and back plates: the battery `chamfer_box(0.3, 0.1, 0.18)` rust with an amber stripe behind the front, the ESC box (bone) behind the back, a fan disc.
- Lid: a `basic+trans` char 0.4 x 0.4 quad at `deck - 0.02` (the dark hole) with the loom crossing it.
- A bare FRAME shows these from the start (its "electronics box" at src/world.c:1275 becomes the ESC and battery).

Cost: under 160 triangles per bot, all hidden until needed.

### 4.3 Scorch and scratch decals at hit locations

A 128 x 128 `basic+map+trans` atlas canvas built in `make_textures` (src/world.c:251): quadrant 0 a scorch blob (char to transparent), 1 three bone scratches, 2 a bolt hole (char hex with a bone rim), 3 a crack (bone zigzag). Each bot owns `decals[8]`: eight 0.2 to 0.3 u quads with their own cloned material (opacity and the atlas offset are per decal; `t3_material` has `map` and the texture has `offset` and `repeat`, so clone the texture handle per decal or use four materials and pick by kind: four materials x one quad kind, chosen at placement, is simpler and keeps the draw count at 4 per bot).

Placement: `world_bot_decal(slot, kind, lx, ly, lz, nx, ny, nz, size)` in local bot space, called from main.c's drain on EV_HIT and EV_RAM in the heavy and huge bands (scratch), on EV_HAZ_HIT from HZ_FLAME and HZ_LASER and on burn ticks (scorch, from `burn_t > 0` in `sync_bot`, one every 30 frames), on EV_SHED (bolt holes, done by 4.1). The decal sits 0.008 u off the hit panel's plane (`panel_place` gives the plane; the hit point is `e->x, e->z` minus the bot position rotated by -heading; `ly` from the weapon table of 4.4). The oldest decal is recycled; decals fade to 0.6 opacity over 10 s, never to 0 (damage is permanent in a fight).

Floor scorch: `world_scorch` is finally called (section 1.6), with the flame grate's and the laser's hit points and the KO.

### 4.4 Sparks: from the contact point, along the impulse, with gravity and bounce

Gravity 14 and the one bounce at 0.3 already exist (src/fx.c:291, 301). What changes is the origin and the direction, by weapon. `fx_hit` gets the weapon from the event (`e->wid`, section 7.1) and a per weapon spray table in juice.h:

```c
typedef struct { float y;      /* contact height over the victim floor */
                 float along;  /* 1: along the attacker's forward; 0: along the hit normal */
                 float tangent;/* 1: along the weapon tangent (spinners) */
                 float up;     /* vertical component of the cone axis */
                 float cone;   /* half angle, degrees */
                 float speed;  /* multiplier on J_SPARK_SPEED_* */ } spark_spray_t;
static const spark_spray_t SPRAY[WP_COUNT + 2] = {
  /* NONE */     { 0.25f, 0.0f, 0.0f, 0.7f, 50.0f, 1.0f },
  /* WEDGE */    { 0.08f, 1.0f, 0.0f, 0.3f, 35.0f, 1.0f },   /* a low fan forward along the plate, sparks skid on the floor */
  /* SPIKE */    { 0.30f, 1.0f, 0.0f, 0.2f, 20.0f, 1.3f },   /* a tight jet along the rod */
  /* LIFTER */   { 0.10f, 0.0f, 0.0f, 0.6f, 60.0f, 0.5f },
  /* SAW */      { 0.35f, 0.0f, 1.0f, 0.9f, 25.0f, 1.2f },   /* a rooster tail up the disc tangent */
  /* FLAME */    { 0.30f, 1.0f, 0.0f, 0.5f, 40.0f, 0.4f },
  /* HAMMER */   { 0.45f, 0.0f, 0.0f, 0.1f, 80.0f, 1.1f },   /* a flat radial fan from under the head */
  /* FLIPPER */  { 0.15f, 1.0f, 0.0f, 0.8f, 45.0f, 0.8f },
  /* DRUM */     { 0.30f, 0.6f, 0.0f, 1.0f, 40.0f, 1.3f },   /* up and over the drum */
  /* HORIZ */    { 0.40f, 0.0f, 1.0f, 0.2f, 20.0f, 1.5f },   /* a long tangent streak, the signature spinner spray */
  /* CRUSHER */  { 0.30f, 0.0f, 0.0f, 0.5f, 60.0f, 0.4f },
  /* MELTY */    { 0.25f, 0.0f, 1.0f, 0.3f, 25.0f, 1.4f },
  /* hazard */   { 0.15f, 0.0f, 0.0f, 0.8f, 60.0f, 1.0f } };
```

The axis is `along * attacker_forward + tangent * (nz, -nx) * spin_sign + (1 - along - tangent) * (-n)` plus `up` in y, normalised; `J_SPARK_INHERIT` keeps adding the victim's velocity. The HORIZONTAL tangent sign follows its visual spin direction (`spin_vis` increases, so clockwise from above: tangent at the contact is `(nz, -nx)`). Spark counts per band stay (`HIT_BANDS`); the hit band and up add a 2 frame `impact star`: four crossed `basic+trans` bone streak quads (0.5 x 0.06 x band flash) at the contact, replacing the additive disc as the visible flash (the disc stays at half size as a glow behind it).

Bounce: sparks that bounce on the floor at over 3 u/s leave a 0.05 r scorch dot through `world_scorch`? No: that is a 4 MB texture version bump per dot. The floor stays clean; the bounce is enough.

### 4.5 Smoke under 40 percent, fire under 20 (was 50 and 30 HP absolute)

`fx_bot_damage_state` takes `hp / hp_max` (world.c passes the fraction) and a side: grey puffs from 0.40, char puffs and fire from 0.20, both from the most damaged panel's position (the first panel at state 3 or 4, else the lid) so smoke pours out of the hole. Fire cubes gain a flicker in size (`0.5 + 0.5 k` stays) and one in three is char (embers). A BURNING bot (flame or grate) keeps its fire whatever its HP (already). Rates: `J_SMOKE_RATE_50` becomes `J_SMOKE_RATE_40 = 4`, `J_SMOKE_RATE_30` becomes `J_SMOKE_RATE_20 = 7`, `J_FIRE_RATE_30` becomes `J_FIRE_RATE_20 = 10`.

### 4.6 Wobbling wheels when a drive takes damage

- TWO WHEEL: while `wheel_hit > 0` the wheel on the hit side (section 7.1 `wheel_hit_side`) wobbles: rotation z = PI/2 + 0.14 sin(3 x wheel_spin), y offset 0.02 sin(3 x wheel_spin), and the body rolls 2 degrees toward it. The sim's 40 percent speed cut (src/sim.c:764) finally has a face.
- FOUR WHEEL at HP 30: the wheel that is lost becomes a sim debris wheel (7.1) and the axle stub stays; the body sags 0.03 on that corner (a 2 degree roll).
- TREADS: a thrown track at HP 20 on a bot whose side panel is state 4: the belt on that side is hidden and a debris DB_TREAD (a long rubber box 0.9 x 0.04 x 0.26) spawns; the sprockets keep turning bare (7.1 needs the kind).
- WALKER and LEG TRACKS at under 0.3 HP: a limp: leg 3 (seeded) stays bent and the gait amplitude on that side halves.
- HOVER at under 0.3: the skirt sags (scale y 0.8) and the bob doubles.

### 4.7 Screen flash and hit stop per band

Hit stop is a sim counter and stays (`STOP_*`, mirrored in juice.h). The viewport white flashes stay (`J_BAND_HUGE_WHITE_A 40`, clash 60). Changes: the heavy band gets a 1 frame white at alpha 16 on the victim's viewport only (a tick you feel), and the KO's white holds 2 frames. The impact star of 4.4 is the per band flash; `hb->flash` keeps its size meaning.

### 4.8 Spinner blur disc

Replace the bone halo. For HORIZONTAL, DRUM, SAW and MELTY the fast spinning geometry crossfades to a blur solid as spin rises:

- `spin < 0.3`: the real disc and teeth, no blur.
- `0.3 to 0.7`: a `basic+trans` blur ring (ring geometry at the tooth radius, 0.16 wide, in the tooth colour at alpha 0.3 x spin) fades in; the teeth fade out at 0.6 (visibility off at 0.65: they would strobe at 12 fps anyway).
- `over 0.7`: the blur ring at alpha 0.45 plus three "ghost teeth": a 64 x 64 radial streak canvas on a disc quad (`basic+map+trans`) rotating at a tenth of the visual rate so it reads as a spinning blur, not a solid.

The two existing rings (src/world.c:1354 to 1358) become the "danger ring" of spec 5: 0.05 wide, alpha `spin x 90/255` and `x 30/255`, and sit 0.01 above the floor under the weapon radius (a floor ring, not a halo through the bot). DRUM uses a blur cylinder (`g_cyl12` r 0.33, glass) instead of a ring. MELTY blurs its four edge bars into a ring at the bar radius.

### 4.9 Flipper launch trail

On EV_LAUNCH (fx_event): the six dust cubes stay, plus a `PK_TRAIL` kind: eight seat colour cubes spawned along the victim's launch velocity (vy 7, forward 3) with no gravity for 8 frames then J_SPARK_GRAVITY, life 0.3 s, size 0.06, streaked like sparks; and a launch streak quad: one `basic+trans` bone quad 0.12 x 1.2 at the plate tip, aligned to the launch velocity, alpha 0.6 to 0 over 8 frames. While the victim is airborne (`b.y > 0.05`) its blob shadow stays on the floor under it (already scaled by `1 - y/3`); it should also drift with the velocity so the player sees where it lands: blob position `x + vx x (y / 7)`.

### 4.10 The KO burst with parts scattering

The sim already sheds every panel and the wheels as debris and emits EV_BURST then EV_KO. The visuals:

- Debris shapes: the one instanced box mesh becomes three instanced meshes: plates (`chamfer_box(0.45, 0.03, 0.35, 0.01)`, 24 instances), wheels (`tyre(0.17, 0.14)` low poly at 8 segments, 8 instances) and chips (the box, 24). `sync_debris` routes by `d->kind`. Plates spin about a random axis as now; wheels roll (rotation about their axle from `d->x, d->z` travel) and land flat on their side after the bounce (`d->bounced` is in debris_t).
- Colour: body paint for plates (after the colour fix), rubber for wheels, armour tint for chips.
- The frame that stays: with the panels gone the chassis shows its raw steel, internals (4.2), a 3 s smoke column (12 puffs per second from the lid, J_SMOKE_LIFE x 1.5, rising 1.2) and fire at `J_FIRE_RATE_20` for 180 frames, then it settles to an ember (one fire cube per second) for the rest of the KO camera.
- Floor: `world_scorch(x, z, 0.9)` under the frame.
- Sparks: the KO band's 40 sparks spray up (cone axis up, 70 degrees) and the four crossed streaks of the impact star at 1.4 size, 3 frames.

## 5. Arena look

### 5.1 Lighting, blob shadows and an AO ring (no shadow maps)

- Blob: the circle becomes an oriented rounded rectangle: a 16 x 16 canvas (char, soft rounded square, alpha falloff) on a `basic+map+trans` quad scaled to (1.3 W, 1.3 L) and rotated by the bot's heading (`place_root` has `h`), offset 0.12 u along -sun (toward (-0.45, 0, -0.3)) so it reads as a cast shadow with a light direction. Alpha 0.4 at y 0, `1 - y/3` as now. 2 triangles, same state as today's blob (`basic+trans` becomes `basic+map+trans`, in the table).
- AO ring: a second quad, same canvas, scale (0.8 W, 0.8 L), alpha 0.3, directly under the bot (no offset): the contact darkening. Together they cost 4 triangles and 2 draws per bot.
- Debris plates get a 0.3 r blob while airborne (the instanced blob is one more `basic+map+trans+instbig` mesh of 24: a state in the table).
- Wall shadow pools are already in the floor paint (src/world.c:415 to 423). Add a pool under every bench and shelf prop: paint a 0.4 u soft rectangle under each `PR_BENCH`, `PR_SHELF`, `PR_HVAC`, `PR_SERVER` footprint in `paint_floor` (the prop list is in the arena def).

### 5.2 Reflections off lexan

No env map (a cube texture and a `+env` state are not in the budget). The trick: a second plane 0.02 u inside each lexan panel with a `basic+map+trans` 256 x 32 "reflection" canvas: four soft bone streaks (the strip lights) and a faint crowd row of char blocks, repeat along the wall, alpha 0.14, `depth_write` off. It parallaxes against the floor as the camera moves because it is a real plane, which is all a reflection needs to read. Plus a 0.02 u bright edge strip at the top of each panel (a bone `basic` box at alpha 1: the lit edge of the sheet) and three "smudge" decals per wall from the scorch atlas at alpha 0.08. THE DOCK (FL_WET) also gets its floor roughness 0.25 (already) and a bone 0.06 x 0.06 emissive strip reflection painted into the floor canvas under each strip light.

### 5.3 Hazard tells

Every hazard tell gets a floor component the chase camera can see (the emissive pulses stay):

| Hazard | Added tell (tell phase only) |
|---|---|
| KILLSAW | a `basic+trans` hazard amber quad over the slot (1.8 x 0.6) pulsing alpha 0 to 0.35 at 2 Hz; the saw teeth glint |
| FLAME | the grate quad pulses amber; the pilot spheres on the grate (four emissive 0.03 spheres) light |
| PULVERIZER | a growing char blob under the strike square (like the dropzone's) from 0 to 1.2 r over the tell; the arm twitch stays |
| PIT | the four corner tells blink as now plus the tape border quad pulses rust |
| FLOORFLIP | the panel edge glows teal (exists); add a 2 Hz 0.02 u panel lift |
| LASER | the pre-drawn beam line stays; add two amber beacons on the emitter posts |
| DROPZONE | the growing shadow stays (it is the right design); add a 2 Hz amber ring decal on the floor |
| BUMPER | the lamp flash stays; the post gets a tape band |

Active state: the hazard's emissive goes to full and a `cam_shake` is already fired by main.c on EV_HAZ_HIT.

### 5.4 Crowd

`crowd_geometry` becomes a flat silhouette: a torso trapezoid (2 triangles), a head hexagon (4), two arm quads (4): 10 triangles, three frames as now, instanced per frame, per instance shade as now. 240 x 10 = 2,400 per view instead of 11,520. The figures face the arena (they are billboards only in the sense that every wall's row has its own yaw, as now). Frame 2 (arms up) is what the KO and huge bands trigger through `world_crowd` (already wired). Add a 1 Hz 0.03 u bob to the back row by nudging the instance matrices in `world_tick` every 4 frames (cheap: 240 matrices per 4 frames).

### 5.5 Floor and walls

- Floor tile grout and specks stay; add tyre marks: 24 seeded 0.12 x 1.5 char streaks at alpha 40 along the long axis in `paint_floor` for every floor style but FL_ROOF (the arena has history).
- A 0.9 u tall char skirting board under the lexan (a `chamfer_box`), so the glass does not meet the floor on a hairline.
- THE ROOF skyline boxes get three window rows (already) and an amber rim light on their top edge.

## 6. Render budget after, and the render states to add

Per view estimate in a 2 player LASER ROOM fight: arena today minus crowd 10.7k, new crowd 2.4k, two bots at 1,200 plus OPS 1,400 = 3.8k, particles 1.2k, decals and blobs 0.1k: about 18.2k. That is over 15k by the arena itself; the LASER ROOM's honeycomb bed lip (`tex_honey` box) and the killsaw tooth boxes (16 x 12 = 192) are not it; the big remaining items are the 12 segment cylinders everywhere (`g_cyl12` 48 per instance; the roof bumpers, posts and turntables use them) and the three 24 segment `g_ring`s per bot plus the 24 pickup rings. Moves to land under 15k: `g_cyl8` for every prop cylinder (32), the ring at 16 segments (32), hazard tooth boxes as `tooth_ring`. Then about 15k. 4 player THE SHOP stays the worst case and the `render_half` fallback stays the safety net; the probe of section 1.1 should become a permanent `RTR_TRACE` log line (`[draw] f= v= calls= tris=`) so every later PR reports the number.

Objects: under 300 in the scene (spec 13). The new bots add about 25 objects each (plates 24 as today, decals 4, internals 6, bolts 1, mounts 2) and remove the per tooth meshes. Count with a probe before merging.

States to add to `tools/states.txt` (then `REGEN_STATES=1 ./build.sh`; the exact names come from `t3_renderer_generated_missing`, the tokens are from three.c docs/r186-renderer.md: `vcol`, `flat`, `emap`, `amap`):

| State | For |
|---|---|
| `basic+map+trans+instbig` | the instanced debris blobs (5.1) and the crowd if it ever gets an atlas |
| `standard+map+trans+l1201` | lit decals (scratch, scorch, titanium heat tint, the HORIZONTAL amber ring) so they darken in THE DOCK's night; `basic+map+trans` is the fallback if this is refused |
| `standard+vcol+l1201` | optional: two tone plates in one mesh (the ply edge strip, the weld bead) instead of extra boxes; skip if the object count is fine |

Everything else in this document draws with states already in the table: `standard+l1201`, `standard+instbig+l1201`, `standard+map+l1201`, `standard+map+rep+l1201` (tyre and tread canvases), `basic+trans`, `basic+map+trans`, `basic+inst+icol`, `basic+instbig+icol`, `standard+trans+ds+l1201` (lexan).

## 7. Interfaces

### 7.1 From the sim (owner: the physics lens; names proposed here, to be reconciled)

Fields and events the renderer and fx need. None of them change `sim_hash`.

| Change | Where | Why |
|---|---|---|
| `event_t.wid` (uint8_t): the weapon that made the hit: `WP_*`, 254 for MELTY, 255 for a ram with no weapon, a hazard, OPS's shove or a landing | src/sim.h:100; set in `ev_hit` callers (apply_damage has `weapon`, src/sim.c:419) | spark spray per weapon (4.4), the weapon's strike pose hold (3.4), decal kind (4.3), the audio lens wants the same byte |
| `event_t.y` for EV_HIT: the contact height, `v->b.y + SPRAY[wid].y` or the hit zone's height | src/sim.c:462 passes `v->b.y + 0.2f` today | sparks and the impact star at the real contact; the renderer can override from its own table if the sim prefers to leave it |
| EV_SHED carries `nx, nz` = the debris launch direction (unit) and `dmg` = the plate's last state | src/sim.c:377 (`ev_simple` passes 0) | the shed spark burst and bolt pop go the right way |
| EV_SHED for wheels: zone 8 + wheel index when the FOUR WHEEL loses a wheel at HP 30 and when TREADS throws a track; new debris kind `DB_TREAD` | src/sim.c `hp_thresholds`; sim.h debris kinds | 4.6 and 4.10 |
| local dent threshold `dmg >= BAND_HEAVY_MIN` and one dent state per heavy hit, two per huge | src/sim.c:455 | 4.1; today unreachable under `HIT_CAP` |
| `bot_t.wheel_hit_side` (int8_t, -1 left, +1 right) set with `wheel_hit` | src/sim.c:764, 965 | 4.6 |
| debris colour arguments 0 (paint) and 2 (rubber) instead of `b->seat` | src/sim.c:376, 395 | finding 4 |
| `bot_t.drive_t` and `drive_s` stay exported (already) | src/sim.h:83 | the SIX WHEEL rock and the HOVER lean |
| nothing for the ghost: it is a world.c bug | | section 1.6 |

Things the renderer reads today and must keep their meaning: `panel_state[6]` 0 to 4, `spin`, `spin2`, `windup`, `active`, `cooldown`, `lift`, `clamp`, `lock`, `fuel`, `burn_t`, `stun`, `inverted`, `lifted_by`, `clamped_by`, `taunt`, `alive`, `hp`, `hp_max`, `skirt_t`, `saw_on`, `red_tag_t`, the `b.p*` previous transforms for interpolation.

### 7.2 From the HUD (owner: the HUD lens)

| Call | Meaning |
|---|---|
| `damage_number(viewport, x, y, z, value, rgb, big)` (exists, src/draw.h:38) | unchanged; fx keeps the world anchor at the contact point, which 7.1 makes real |
| `hit_marker(viewport, x, y, z, band, rgb)` (new) | a ring at the projected contact: 16 px growing to 28 px over 8 frames, 3 px bone for tap and hit, 4 px hazard amber for heavy, 5 px rust for huge, drawn with `hud_ring`; CLASH uses the existing `damage_text` |
| `strip_hit(seat, band)` (new) | the seat's HP bar flashes bone for 2 frames then drains to the new value over 18 frames (`J_BARS_ANIM_FRAMES`), and the number pops 1.15x; the attacker's strip gets a 6 frame seat colour tick on its weapon gauge so "I hit" is readable in a quadrant |
| `viewport_white(vp, alpha)` (exists, src/draw.h:56) | unchanged, fed by `fx_white_alpha` |
| `stinger` (exists) | unchanged |

main.c's `drain_events` (src/main.c:295) is the only place that fans an event out; it gains the `hit_marker`, `strip_hit`, `world_bot_decal` and `world_scorch` calls next to the existing `fx_hit`, `number_all` and `cam_punch`.

### 7.3 Inside world.c and fx.c (owner: the models and fx implementers)

New entry points (declared in world.h and fx.h with one line comments, per the header contract):

```c
/* world.h */
void world_bot_decal(int slot, int kind, float lx, float ly, float lz, float nx, float ny, float nz, float size);   /* 4.3 */
void world_bot_hit(int slot, float nx, float nz, int wid, int band);     /* strike hold, lip flash, hanging plate kick (3.4, 4.1) */
int  world_draw_stats(unsigned *calls, unsigned *tris);                  /* the last viewport's counters for the [draw] log */
/* fx.h */
void fx_launch(int victim_slot, float x, float y, float z, float vx, float vy, float vz, uint32_t seat_colour);   /* 4.9 */
void fx_bot_damage_state(int slot, float hp_frac, int burning, float x, float y, float z, int side);            /* 4.5: replaces the absolute HP signature; add, do not change the old one */
```

`sync_bot` keeps calling the damage state with the fraction and the most damaged side; `pose_weapon` grows the READY glint and the strike hold; `sync_panels` grows the hanging swing and the internals visibility; `bot_rebuild` grows the decal quads, bolts, internals and the geometry cache.

## 8. Acceptance: screenshots and what they must show

Every shot is a rig capture at the fixed seed; "must" lines are what a reviewer checks by eye, plus the `[draw]` and `[hits]` lines of the log.

| Fixture and frame | Must show |
|---|---|
| `garage-parts.json` 172 to 422 (chassis) | every chassis with visible bevels and bolt heads on the front face; WEDGE with a lip and two skids; LOWBOY with vents and rack ears; SHELL with a rolled rim and rivets; FRAME with round tubes and cross braces; LUNCHBOX as lid plus base with a hinge line; the front face of a char paint bot is not black (its mean luminance over 25 percent of the lid's) |
| 482 to 832 (drives) | TREADS show lugs, a sprocket at each end and road wheels; HOVER shows a pleated skirt with a bone rim and two fan intakes; LEG TRACKS show knees over two pods; WALKER legs have a knee; wheels have tread and a hub with spokes |
| 892 to 1392 (weapons, with the RT test fire at +6) | WEDGE plate no taller than 0.5 u with a bright lip; FLIPPER plate shows a gap line, two hinge knuckles and a piston; HAMMER head has a bright face plate and a pivot pin; HORIZONTAL shows four fat teeth and an amber ring; CRUSHER reads as a jaw with teeth; SPIKE rod reads at r 0.08; the test fire frames show the TELL and STRIKE poses (a HAMMER windup at 892 + 15, the slam at + 19) |
| 1452 to 1752 (armour) | ALUMINIUM brighter than STEEL, WEAR PLATE with twelve bolts and amber corners, UHMW with no bolts, TITANIUM with the heat stripe, PLYWOOD with the bone edge: six different plates on the same bot |
| 1812 to 1962 (paint) | four schemes that differ in plate colour and lid; the seat number plate on both sides; the band readable on seat 3 (bone) and 4 (rust) |
| `2p-fight.json` 680 | the LASER ROOM bed is empty but for the killsaws: no ghost chassis at the centre; the draw log under 60 calls and under 15k triangles for each view |
| `feel.json` 863, 864, 866, 870 (the WEDGE ram; 12 on screen in this build, 14 in HANDOFF) | 864: an impact star at the contact and sparks held during the 2 frame stop; 866: the fan leaves low and forward along the plate, not back at the attacker; 870: sparks on the floor after one bounce, the hit marker ring at its 28 px end, a scratch decal on the victim's hit plate (heavy band) |
| a heavy HORIZONTAL hit (find with `EXTRA_CFLAGS=-DRTR_TRACE` from the `[hit] ... band=2` line of 2p-fight; today 1020 to 1080 carry eleven and nine hits) | tangent spark streak over 1 u long, the victim's hit plate at state 1 or 2 with a visible crease and corner, the disc blur ring instead of a halo |
| `2p-fight.json` 2600 (ROLL OUT at 1 HP) | at least two plates gone showing raw steel, wires and the battery; the remaining plates hanging; char smoke from the hole and fire; scorch and scratch decals; a floor scorch where the flame grate hit it (THE LASER ROOM has three grates) |
| `2p-fight.json` 3243 to 3290 (KO) | plates as plates and wheels as wheels in the paint and rubber colours, a smoke column and fire on the stripped frame, the floor scorch, the crowd at frame 2, the star at 1.4 size at 3243 |
| `keyboard-solo.json` 1800 (inverted) | the underside shows a belly pan, two motor cans, a battery and bolt heads, not a bare bone plane |
| `4p-fight.json` 700 (THE BAY, four views) | every view under 15k triangles and 60 calls; the oriented blob shadows under each bot with a shadow direction; the lexan reflection plane visible as bone streaks in the top views |
| `4p-fight.json` 1800 | the RED TAG bot's smoke comes from its damaged side, not the lid centre; the laser tell beacons |
| `drive-laser.json` any frame in the laser tell window (18.5 s) | amber beacons on both posts and the pre-drawn beam |
| boot, frame 1 | `t3_renderer_generated_missing` empty after every new state is in `tools/states.txt` |

Numbers to log in every PR that touches world.c or fx.c: the `[draw]` line of the first fight frame of 2p-fight, 4p-fight and keyboard-solo, and the scene object count from a probe (`t3_scene` traversal count) at the same frame.

## 9. Work order (what an implementer builds, in sequence)

1. The four bug fixes of 1.6 (ghost, scorch call, debris colour, dent threshold). Half a day. Ship first; the ghost alone answers one of the owner's lines.
2. The crowd rewrite (5.4) and the `[draw]` line under RTR_TRACE. One day. Proves the budget before the bots grow.
3. The geometry kit (3.1) and the chassis pass with paint on plates and raw steel bodies (3.2, 3.5, 3.6), garage lighting (3.8). Two days. The garage sweep is the acceptance.
4. The drive pass (3.3). One day.
5. The weapon pass with READY, TELL, STRIKE, RECOVER poses (3.4). Two days.
6. Damage: plates with states, internals, decals, the spray table, smoke by fraction, wheel wobble (4.1 to 4.6). Two days, in step with the sim fields of 7.1.
7. Blur discs, launch trail, the KO burst shapes (4.8 to 4.10). One day.
8. Arena: blobs, reflections, hazard tells, floor marks (5.1 to 5.5). One day.
9. Regenerate the fight fixtures (`build/balance --make-fight`) after the sim fields land, retake every acceptance shot, append any missing state, commit.
