# RED TAG RUMBLE: handoff

A split-screen bot builder and arena combat cart for wasmcart. One to four pads, three.c for the 3D, a CPU painted HUD composited in a second pass, Bellows for the SFX, baked WAV stems for the music, a deterministic 60 Hz sim that also builds natively for the balance tool.

The complete specification is `docs/design/FINAL-bots.md`. Read section 20 (things an implementer must not get wrong) before touching anything. `docs/design/combat-brief.md` is the research it came from. The platform, audio and HUD maps live beside the spec in the planning folder of the parent checkout.

## Status

Feature complete and playable from boot to the rematch vote, for 1 to 4 pads, in all six modes and all six arenas. The six subsystem branches (sim, world, draw-fight, draw-menus, audio, ai) are merged on main and their worktrees removed. Three integration stages followed: the phase machine (R3a), the fight feel and the modes (R3b), and a polish pass with these docs (R3c).

- `src/game_flow.c` runs the phase machine of 10.1: boot, title with the attract fight, settings, hall of fame, the rules flash, lobby, garage, arena vote, countdown, fight with pause, KO, judges, results and the rematch vote. Every phase change logs `[phase] N at frame F`.
- `src/game.c` keeps the state that has no renderer in it (the save struct with its crc, seats, the settings rows) so `tools/hudtest.c` still links it natively. Keep renderer, camera and audio calls out of it.
- Seats are packed when the lobby closes: seat i is match slot i and turntable i for the rest of the evening. In TOURNAMENT each pair plays in slots 0 and 1 with `bot_t.seat` set to the real seat.
- `main.c` drain_events turns every sim event into fx, audio, rumble, camera shake and punch, damage numbers and stingers. The station build logs one `[hits] f= n= dmg= rumble=` summary per sim second (right after the `[sim]` hash line, only when something happened); the per event `[hit]` and `[rumble]` lines are an `EXTRA_CFLAGS=-DRTR_TRACE` build, which also logs `[hud]` rect and `[memt]` heap lines once a second and `[mem]` at every phase change. Feel numbers live in juice.h; sim.h STOP_* mirror its J_STOP_* column and a static assert in main.c keeps them equal.
- The perf pass (the October audit): the HUD uploads only what changed (see Platform facts), the 30 fps fallback releases and resets per phase, every rebuild leak is closed (the soak below), the floor textures no longer copy at first upload, the material cache compares the whole tuple and grows, and the cart builds with `-Wshadow` (and clean under `-Wextra -Wno-unused-parameter`, which is not on by default).
- Hazard seats, the mini-KO hand-over to the hazard seat view, the TOURNAMENT bracket with the 3 player bye coin flip and the final's OPS crew live in game_flow.c.
- The pause menu belongs to the pad that paused; other pads cannot walk its cursor.

## What is verified

| Check | How | Result |
|---|---|---|
| Builds | `./build.sh`, `./build.sh balance`, `./build.sh music`, `HUD_DIV=2 ./build.sh`, the `RTR_FAKE_HUD`, `RTR_FAKE_MENUS` and `RTR_FAKE_SIM` probe builds | no warnings; no missing render state in any rig run |
| House lint | `./build.sh lint`, and the hackerspace deliverable lint over content.h, README.md and HANDOFF.md | zero findings |
| Determinism | `build/balance --hashcheck`; the cart's `[sim] f= h=` lines against `build/balance --replay` for drive-bay and 2p-fight | identical |
| Arenas | `build/balance --check-arenas` | PASS on all six |
| Balance | `build/balance --robin 20` (builtin driver) | PASS: every preset between 35 and 65 percent, no stalls, every KO cause occurs |
| Parts | `build/test_parts` | PASS |
| AI | `build/aitest` | 30 of 30 situations |
| Save | `SAVE_OUT` after `flow.json`, then `SAVE_IN` into `menu.json` | the hall of fame shows the fight's records after a cold boot (196 byte save; rechecked in the perf pass: MATCHES 1, the ROLL OUT win and the streak show, a cold boot shows zeros) |
| HUD upload | `TIMING=1 node tools/shot.mjs ...` (the `[rig]` lines count the GL texture uploads) and the cart's `[perf]` line | before: 60 full uploads a second in every phase, 216,000 KB/s. After: a 2p fight 25,000 to 40,000 KB/s in 7 to 9 sub-image calls a frame, a 4p THE SHOP fight 59,000 KB/s in about 11, the garage about 16,000 KB/s in 1.5, the vote and the hall of fame a few hundred KB/s; a clean frame uploads nothing. The frames are pixel identical to the full upload build (241, 700, 1200, 1201, 2400 of 2p-fight compared) |
| Memory soak | twelve rematches in one process (a soak script that presses A on two pads every 20 frames for 18000 frames: lock, vote, fire, results, REBUILD), the `[mem]` line at every countdown | before: the heap grew 7.7 KB per rematch (40,250,840 to 40,335,736 over 11). After: flat from the second match on (within 1 KB over eleven rematches); the live block count from a malloc wrapping probe is constant at 4313. The HUD ring and its shadows put the heap near 52 MB of the 64 MB initial memory (`EXTRA_CFLAGS=-DHUD_RING=2` saves 3.6 MB, `HUD_DIV=2` most of it) |
| Render fallback | `render_measure` on the host delta | trips after 30 frames over 20 ms, releases after 3 s at 60 Hz (doubling on a re-trip in the same phase), resets on every phase change; never trips in the rig (fixed 16.7 ms deltas) |
| Warnings | `-Wall -Wshadow` in build.sh (cart and balance); a probe compile of every src/*.c with `-Wextra -Wshadow -Wno-unused-parameter` | zero |
| Sound | every SFX of the 11.3 table has a call site (menus, lock, count, horn, hits by band, clash, launch, klaxon variants, KO); music modes, slow-mo, tension, duck, whines and crowd follow the phases | wired; `WAV=` captures checked in R3b |
| Screens | the R3c screenshot pass (title, attract, lobby, garage 2p and 4p with OVER BUDGET, vote, countdown, fights 1p to 4p, a big hit, KO, results, rematch, settings, hall of fame, the seven rules cards, the controls card, pause) | looked at; fixes listed in the R3c commit |

Robin 20 with the builtin driver (overall win percent, both sides, every arena, hazards on and off):

| ROLL OUT | LAUNCH | CHOP | STOMP | TOAST | SPIN |
|---|---|---|---|---|---|
| 41.8 | 43.8 | 44.1 | 37.6 | 49.6 | 63.6 |

WEDGE beats HORIZONTAL 82 percent. The two HOVER against TREADS guardrails are printed as GUARD with this driver (HOVER wins THE BAY 100 percent and THE LASER ROOM 8 percent).

## How to run each tool

```sh
./build.sh                      # the cart: build/red-tag-rumble.wasc, copied to web/
./build.sh balance              # build/balance, native, headless sources only
./build.sh music                # rebake assets/music/*.wav and build/sfx_preview.wav (peak and RMS per file)
./build.sh lint                 # the house lint
REGEN_STATES=1 ./build.sh       # regenerate src/gen/rtr_gl.c from tools/states.txt

build/balance --parts                       # every preset's cost, bars and name
build/balance --robin 20 [driver=ai] [arenas=bay] [hazards=on|off|both] [verbose=1]
build/balance --check-arenas | --hashcheck | --modes 4
build/balance --replay tools/scripts/drive-bay.json --setup "<the [sim] setup line from the cart log>"
build/balance --trace bay "ROLL OUT" SPIN 3000
build/balance --map bay out.ppm tools/scripts/drive-bay.json
build/balance --make-drive bay tools/scripts/drive-bay.json

cc -std=gnu11 -Wall -Wextra -I src tools/test_parts.c -o build/test_parts && build/test_parts
cc -std=gnu11 -O2 -Wall -DRTR_HEADLESS -I src src/sim.c src/bot.c src/arena.c src/ai.c tools/aitest.c -lm -o build/aitest && build/aitest
tools/hudtest.sh [scene]        # native HUD scenes to build/shots/menus-<scene>.png (HUD_DIV=2 for half size)

node tools/shot.mjs build/red-tag-rumble.wasc build/shots/x 120,260,900 tools/scripts/2p-fight.json
STATION=1 ...                   # only pad 0 reports connected, as the RetroArch core does
WAV=build/out.wav ...           # the audio the cart played
SAVE_OUT=build/s.sav ...        # the save region at the end of the run
SAVE_IN=build/s.sav ...         # boot with a save (never by default, so shots stay reproducible)
PACE=1 ...                      # real time, non-deterministic
```

The balance constants MELTY_TIP, SPIN_BLEED_* (sim.c) and WP_MELTY_DMG, WP_MELTY_SPIN_UP, WP_MELTY_WALL_DMG (parts.h) take `-D` overrides, so a sweep can build several balance binaries without editing the tree.

Scripts in `tools/scripts/` (frame numbers at seed 7):

| Script | What it drives |
|---|---|
| `title.json` | the title and the attract (title at 60, attract at 700, 30 s idle at 1900) |
| `menus.json` | title, lobby, two seats, garage picks, vote, countdown (lobby 100, garage 180, vote 340, countdown 560) |
| `menu.json` | settings (140), hall of fame (220), the seven rules cards (285 to 525) |
| `overbudget.json` | seat 1 builds 21 tags: OVER BUDGET BY 1 at 296 |
| `2p-fight.json` | the 15.5 2 player session: fight from 668, KO at 2234, results 2385, rematch 2745 |
| `pause.json` | 2p-fight with a pause at 1600 and the controls card at 1660 |
| `4p-fight.json` | four seats in THE BAY: garage 260, mini-KOs 1525 and 1733, final KO 2765 |
| `feel.json` | the feel gate: a 14 damage WEDGE ram at 863 |
| `2p-time.json` | a 60 s fight to TIME (4240) and the judges |
| `solo.json`, `3p-fight.json` | the 1 player full view and the 3 player layout |
| `mode-flip`, `mode-lbs`, `mode-hill`, `mode-2v2`, `mode-tourn`, `mode-tourn3` | each mode played to a result |
| `flow.json` | a 60 s fight through KO, results and the rematch menu |
| `drive-<arena>.json` | the six 90 s solo laps over every hazard (PRACTICE=off, replayable by balance) |

Under the rig a `"practice": 0` entry makes `tools/shot.mjs` hold SELECT, L3 and R3 on pad 0 in the first frames; the cart reads that as PRACTICE=off (the practice bot is LAUNCH, parked, the fight is 90 s, the seed is the host seed), so `build/balance --replay` reproduces the cart's hash lines. The first fight step of every scripted session is shot frame 668.

## Risks

1. Balance with the AI driving. `build/balance --robin 20 driver=ai` fails: SPIN about 83, TOAST 64, ROLL OUT 26 and STOMP 25 percent, and 40 stuck frames. MEDIUM against MEDIUM is close to deterministic, so most pairings land at 0 or 100 percent; MELTY damage, spin bleed and tip speed moved SPIN by only a few points in a sweep. The fix is in ai.c (how the AI approaches and escapes a MELTY, how slow builds close distance) before parts.h costs. The default robin, which A18 names, passes.
2. Pi performance is unmeasured. The 30 fps render fallback exists (render_measure: on after 30 frames over 20 ms, off after 3 s at 60 Hz, reset per phase; it reads the host delta, since a cart has no clock of its own and GPU time only shows as missed vsyncs), but nothing has run on the station yet. Check THE SHOP in quadrants first, and read the `[perf]` line: `half=1` for long stretches of a fight means the HUD compare plus the draw calls still do not fit; the next cut is `HUD_DIV=2`.
2a. Four GL programs still link during play rather than at boot (three when the first bot appears on a lobby turntable, one at the first countdown): `world_compile_all` compiles the scenes without bots, debris or fx. A warm up that builds one bot of every material bearing part, shows the debris and one spark and one puff before the compile, then hides them, belongs in world.c with fx.c's help.
3. The music set is 5.79 MiB against the 4.4 MB line of section 13. `MUSIC_RATE=22050` in the bake or dropping arena_lead2 brings it down.
4. KING OF THE HILL can run long in wall time: mini-KO slow motion stretches it, and idle pads respawn and get counted out in a loop.
5. The OPS-driven final is wired (sim_ops_driven, the driver's and gunner's pads) but no script drives OPS.

## The header contract

The headers in `src/` are the contract between the subsystem agents, who implement in parallel without talking to each other. Rules:

1. A header is owned by one agent (the table below). Another agent who needs a new entry point adds the declaration to that header with a one line comment and leaves the body to the owner, or adds it to their own file and declares it in their own header. Never change a signature that is already declared; add a new one.
2. Every shared type is complete in the header it lives in. `sim.h` carries the 4.3 data verbatim; `parts.h` carries the whole catalogue and every build rule as `static inline`; `content.h` carries every string; `juice.h` carries every feel constant.
3. The headless line: `sim.c`, `bot.c`, `arena.c`, `ai.c`, `parts.h`, `content.h`, `juice.h` and `audio.h` include nothing from three.c, hud.h or wasmcart and use no libm trig. `./build.sh balance` enforces both with a dependency scan and a grep, and it is what the native tool and the replay depend on.
4. Strings live in `content.h` or `parts.h`, feel numbers in `juice.h`, nothing inline. `./build.sh lint` checks the house rules (ASCII caps, no dashes, no emoji, glyph rows 16 wide, stinger and blurb lengths).

## Ownership

| Files | Owner (spec items) | Notes |
|---|---|---|
| `build.sh`, `compile_flags.txt`, `package.json`, `tools/shot.mjs`, `tools/smoke.c`, `tools/states.txt`, `src/gen/rtr_gl.c`, `web/index.html`, `.github/` | integration (1, 18, 23) | append a missing state to `tools/states.txt`, `REGEN_STATES=1 ./build.sh`, commit the regenerated table |
| `src/main.c` | integration (1); the event drain is shared with fx (6) | wc_info, resolution, pads and seats, the accumulator and timescale, the frame, the drain, the save region, the hash log |
| `src/parts.h` | parts (2) | catalogue tables, costs, legality, presets, RANDOM BOT, the three bars, bot_name; all static inline |
| `src/sim.h`, `src/sim.c` | sim (3, 8, 13, 16, 20, 24) | complete; MELTY_TIP and SPIN_BLEED_* take -D overrides for sweeps |
| `src/bot.h`, `src/bot.c` | sim (3) for the body and stats; menus (11) for `bot_hint` | |
| `src/arena.h`, `src/arena.c` | arena (4, 14) and the checker in `tools/balance.c` | all six arenas complete: hazards, props, tiles, vis tables, spawns and OPS zones |
| `src/ai.h`, `src/ai.c` | AI (12, 13) | only ai.c reads the fields of `ai_t` |
| `src/world.h`, `src/world.c`, `src/render.h` | world (5, 8, 13, 14, 15, 21) | complete; `render_frame` lives in render.c |
| `src/fx.h`, `src/fx.c`, `src/juice.h` | fx (6, 21, 24) | |
| `src/camera.h`, `src/camera.c` | camera (5, 6, 10, 15) | complete, including the KO orbit, ONE CAMERA and the results turntable |
| `src/hud.h`, `src/hud.c`, `src/glyphs.h` | HUD (9) | complete: DO-OCRACY's primitives plus every 10.3 and FINAL-frontend 5 addition, HUD_DIV |
| `src/draw.h`, `src/draw.c` | HUD (9, 10, 16) | the fight HUD, KO, judges, results, rematch, hazard seat, pause |
| `src/draw_menus.c`, `src/draw_menus.h` | menus (11) | boot, title, lobby, garage, vote, countdown, settings, hall, rules flash, controls card |
| `src/game.h`, `src/game.c`, `src/game_flow.c` | game (10, 11, 16) | game.c holds renderer-free state; the phase machine is game_flow.c |
| `src/content.h` | content (22); every agent adds its strings here | |
| `src/audio.h`, `src/audio_band.cpp`, `src/audio_pcm.c`, `tools/bake_music.cpp` | audio (17) | complete; music ships as eight baked stems |
| `tools/balance.c`, `tools/scripts/*.json` | balance (19), arena (4), integration (18) | `--parts` prints every preset's cost, bars and name |
| `tools/lint.sh` | content (22) | |
| `HANDOFF.md`, `README.md` | integration | |

## Build and test

```sh
./build.sh                      # build/red-tag-rumble.wasc, also copied to web/
REGEN_STATES=1 ./build.sh       # regenerate src/gen/rtr_gl.c from tools/states.txt (three.js is cached in the submodule)
./build.sh smoke                # the pipeline smoke cart, tools/smoke.c
./build.sh balance              # build/balance, native, headless sources only (fails if one includes a renderer or host header)
./build.sh music                # bake assets/music/*.wav (needs tools/bake_music.cpp)
./build.sh lint                 # the house lint
HUD_DIV=2 ./build.sh            # the half size HUD buffer
MUSIC=live ./build.sh           # keep the songs live in the cart
EXTRA_CFLAGS=-DX EXTRA_SRCS=probe.c ./build.sh   # a probe build

node tools/shot.mjs build/red-tag-rumble.wasc build/shots/s 120,260,340 tools/scripts/2p-fight.json
STATION=1 node tools/shot.mjs ...      # only pad 0 reports connected, as the RetroArch core does
WAV=build/out.wav node tools/shot.mjs ...
TIMING=1 node tools/shot.mjs ...       # a [rig] line every 60 frames: host ms per frame, texture uploads a second (calls, KB, partial), links, wasm memory
EXTRA_CFLAGS=-DRTR_TRACE ./build.sh    # the trace cart: per event [hit] and [rumble], [hud] rects and big writers, [memt] heap once a second
build/balance --parts | --robin 20 | --check-arenas | --replay script.json --setup "line"
```

The perf log: the station build prints `[perf] f= host_ms= half= hud_calls= hud_kb= cmp_kb=` every 5 s (the host frame delta average, the render fallback, the HUD uploads of the window: sub-image calls, KB sent, KB compared) and `[mem] f= match= used= arena= sbrk=` at every countdown. A soak is a rig run of a script that loops the evening (lock with A, A on the vote, hold A in the fight, A on results and the rematch) with the `[mem]` lines compared match to match; `used` must be flat from the second match on. To find a leak, wrap the allocator in a probe build (`EXTRA_CFLAGS="-Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free" EXTRA_SRCS=probe.c`, the probe records every live block with `G.match_index` and `G.phase` and dumps the blocks of the previous match still alive at the next countdown, grouped by phase and size): the size of a leftover names its struct, and the phase names the code path (an allocation in `game_start_match` is tagged with the vote, the phase before the countdown).

Every build must pass with no new warnings. Look at every screenshot you take. If the console prints a missing render state (`t3_renderer_generated_missing`, logged every 120 frames), append it to `tools/states.txt`, regenerate, commit.

The rig scripts in `tools/scripts/` are shared: `2p-fight.json` (seats 1 and 2, ROLL OUT and CHOP, THE LASER ROOM, drives and hits), `4p-fight.json` (four seats, THE BAY, drives, taunts, a brake and a boost) and `keyboard-solo.json` (pad 0 with d-pad bits and buttons only, as the station keyboard: lobby, a Y tap and two arrow rows in the garage, Start to lock and go, A on the vote, then a d-pad drive with a flipper and a boost). Their frame numbers are the acceptance frames of spec section 18, with two shifts since the controls pass: A locks on the press (no 1 s hold), so the garage shots of the 2p and 4p scripts are at 240 and the vote opens at 249; and nobody has voted when the vote opens, so every script presses A on the vote (308 for the 230 lock scripts) to keep the fight start at 668. Seat 2 of the 2p scripts taps Y at 200 to reach CHOP; Start in the garage locks and starts.

## Platform facts

- Pads: every pad is polled every frame; a seat is claimed on an A, B, X, Y or Start edge; `connected` is never read. `read_pads` turns the d-pad bits into left stick axes (both sides for a TANK seat) whenever the stick rests in the dead zone, so the station keyboard (pad 0, bits only) drives; the sim only ever sees axes. `PADS[i]` carries the current and previous `pad_state_t`, the edges, and menu navigation events with repeat (18 frames then every 8). `pad_state_t.buttons` uses the `IN_*` bits, which equal `WC_BTN_*`.
- Resolution: `wc_negotiate_resolution` with a 1280 x 720 cap; `RTR_W`, `RTR_H` are the renderer size; every rect in the game is authored in 1280 x 720 reference space (`rect_t`) and scaled by `rect_scale` at the viewport call. The HUD is authored at 1280 x 720 whatever `HUD_DIV` is.
- The frame: `acc += dt * G.timescale`, up to four `sim_step`s per frame, `alpha = acc / DT` for `world_sync`, the event drain after each step, `render_frame`, `draw_hud`, `hud_upload`, the HUD quad, time based audio frames with a fractional carry.
- The HUD upload (main.c `hud_upload`): the pixel writers in hud.c record what they touch per 8 row band (columns too), `hud_clear` wipes only what the previous frame painted and marks it, and `hud_dirty_bands` hands main.c the bands of the frame. The HUD textures are three GL objects the cart makes itself in wc_init (sRGB format, nearest, clamp; three.c samples them through `t3_texture_from_gl` and never uploads): a ring, so the texture written each frame was last read three frames ago and the driver never waits for the GPU on a partial write (a sub-image write to a busy texture stalls on Mesa V3D, and on the rig's host it cost 0.78 ms a call with one texture against 0.42 with three). Each ring entry keeps a shadow of its content and the bands it still has to catch up on; on a dirty frame the next entry gets its pending bands merged into rects (`hud_band_rects`: runs of bands whose columns nearly agree, so the seam down the middle, the clock chip and the strips are separate rects), each rect's bands compared against the shadow, only the bands that differ sent with `glTexSubImage2D` (texture unit 31, packed through a 192 row staging buffer, `t3_renderer_reset_bindings` after), and the quad's material is pointed at it. The compare is what makes the gate real for the menus, which repaint a full backdrop every frame. `hud_dirty()` is a no-op kept for the drawers; a hint wider than what was written would only widen the upload. Memory: three shadows and the staging buffer, 11.8 MB at HUD_DIV 1, 3 MB at HUD_DIV 2.
- The split-screen recipe is in `render_frame` (world.c): scissor test on, auto clear on, per viewport equal viewport and scissor with y from the top, then scissor off, full viewport, auto clear off for the HUD pass. No shadow maps.
- Lights are fixed at `l1201`: one hemisphere, one sun, two points. An arena with one accent light sets the other's intensity to 0; it is never removed.
- `save_t` is `SAVE` in main.c, magic `RTR1`, crc over everything after the crc; all zero means empty and `save_defaults` resets it.
- The 10.4 strip rects collide: an HP bar of 400 px from x 52 ends at 452 and the F_MONO_L number right of it runs under the weapon gauge at 470. The demo strip uses a 340 px bar with the number at 400; the HUD agent should keep that or move the gauge.

## Deploy

Copy `web/red-tag-rumble.wasc` to `/userdata/roms/wasmcart/` on the Batocera box and restart EmulationStation. `batocera.conf` carries `wasmcart.gfxbackend=gl` and `wasmcart.rewind=0`. The Pages workflow publishes `web/` on every push to main.
