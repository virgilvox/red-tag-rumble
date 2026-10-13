# RED TAG RUMBLE: handoff

A split-screen bot builder and arena combat cart for wasmcart. One to four pads, three.c for the 3D, a CPU painted HUD composited in a second pass, Bellows for the SFX, baked WAV stems for the music, a deterministic 60 Hz sim that also builds natively for the balance tool.

The complete specification is `docs/design/FINAL-bots.md`. Read section 20 (things an implementer must not get wrong) before touching anything. `docs/design/combat-brief.md` is the research it came from. The platform, audio and HUD maps live beside the spec in the planning folder of the parent checkout.

## State of the tree

The six subsystem branches (sim, world, draw-fight, draw-menus, audio, ai) are merged on main. `src/game_flow.c` runs the phase machine of 10.1: boot, title with the attract fight, settings, hall of fame, the rules flash, lobby, garage, arena vote, countdown, fight with pause, KO, judges, results and the rematch vote. `src/game.c` keeps the state that has no renderer in it (the save struct with its crc, seats, the settings rows) so `tools/hudtest.c` still links it natively. The cube demo is gone.

Seats are packed when the lobby closes, so seat i is match slot i and turntable i for the rest of the evening. Under the rig a `"practice": 0` script entry makes `tools/shot.mjs` hold SELECT, L3 and R3 on pad 0 in the first frames; the cart reads that as PRACTICE=off (the practice bot is LAUNCH, parked, the fight is 90 s, the seed is the host seed), so `build/balance --replay` reproduces the cart's `[sim]` hash lines for the drive scripts. The first fight step of every scripted session is shot frame 668.

Scripts: `tools/scripts/menus.json` (title, lobby, two seats, garage picks, vote, countdown), `menu.json` (settings, hall of fame, the rules flash), `flow.json` (a 60 s fight through KO, results and the rematch menu), `title.json` (the attract), the 2p and 4p fights of 15.5 and the six drive scripts.

Not wired yet: the hazard seats, the mini-KO viewport hand-over to the hazard seat view, the TOURNAMENT bracket and OPS driving (item 16).

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
| `src/sim.h`, `src/sim.c` | sim (3, 8, 13, 16, 20, 24) | the trig table is real; everything else is a stub |
| `src/bot.h`, `src/bot.c` | sim (3) for the body and stats; menus (11) for `bot_hint` | |
| `src/arena.h`, `src/arena.c` | arena (4, 14) and the checker in `tools/balance.c` | the six tables carry sizes, names, floors, lights, spawns and OPS zones; hazards, props, tiles and vis tables are still empty |
| `src/ai.h`, `src/ai.c` | AI (12, 13) | only ai.c reads the fields of `ai_t` |
| `src/world.h`, `src/world.c`, `src/render.h` | world (5, 8, 13, 14, 15, 21) | `world_scene()`, the l1201 lights and `render_frame` (the viewport loop) are real |
| `src/fx.h`, `src/fx.c`, `src/juice.h` | fx (6, 21, 24) | |
| `src/camera.h`, `src/camera.c` | camera (5, 6, 10, 15) | `cam_layout`, the camera objects and `cam_turntable` are real |
| `src/hud.h`, `src/hud.c`, `src/glyphs.h` | HUD (9) | complete: DO-OCRACY's primitives plus every 10.3 and FINAL-frontend 5 addition, HUD_DIV |
| `src/draw.h`, `src/draw.c` | HUD (9, 10, 16) | the fight HUD, KO, judges, results, rematch, hazard seat, pause |
| `src/draw_menus.c` | menus (11) | boot, title, lobby, garage, vote, countdown, settings, hall, rules flash, controls card |
| `src/game.h`, `src/game.c` | game (10, 11, 16) | the globals, seat joining and the save helpers are real; the phase machine is empty |
| `src/content.h` | content (22); every agent adds its strings here | |
| `src/audio.h`, `src/audio_band.cpp`, `src/audio_pcm.c`, `tools/bake_music.cpp` | audio (17) | `audio_render` already does the time based frame count with the carry, into silence |
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
build/balance --parts | --robin 20 | --check-arenas | --replay script.json --setup "line"
```

Every build must pass with no new warnings. Look at every screenshot you take. If the console prints a missing render state (`t3_renderer_generated_missing`, logged every 120 frames), append it to `tools/states.txt`, regenerate, commit.

The rig scripts in `tools/scripts/` are shared: `2p-fight.json` (seats 1 and 2, ROLL OUT and CHOP, THE LASER ROOM, drives and hits) and `4p-fight.json` (four seats, THE BAY, drives, taunts, a brake and a boost). Their frame numbers are the acceptance frames of spec section 18.

## Facts the stubs already settle

- Pads: every pad is polled every frame; a seat is claimed on an A, B, X, Y or Start edge; `connected` is never read. `PADS[i]` carries the current and previous `pad_state_t`, the edges, and menu navigation events with repeat (18 frames then every 8). `pad_state_t.buttons` uses the `IN_*` bits, which equal `WC_BTN_*`.
- Resolution: `wc_negotiate_resolution` with a 1280 x 720 cap; `RTR_W`, `RTR_H` are the renderer size; every rect in the game is authored in 1280 x 720 reference space (`rect_t`) and scaled by `rect_scale` at the viewport call. The HUD is authored at 1280 x 720 whatever `HUD_DIV` is.
- The frame: `acc += dt * G.timescale`, up to four `sim_step`s per frame, `alpha = acc / DT` for `world_sync`, the event drain after each step, `render_frame`, `draw_hud`, the HUD quad only re-uploaded when `hud_dirty` was called, time based audio frames with a fractional carry.
- The split-screen recipe is in `render_frame` (world.c): scissor test on, auto clear on, per viewport equal viewport and scissor with y from the top, then scissor off, full viewport, auto clear off for the HUD pass. No shadow maps.
- Lights are fixed at `l1201`: one hemisphere, one sun, two points. An arena with one accent light sets the other's intensity to 0; it is never removed.
- `save_t` is `SAVE` in main.c, magic `RTR1`, crc over everything after the crc; all zero means empty and `save_defaults` resets it.
- The 10.4 strip rects collide: an HP bar of 400 px from x 52 ends at 452 and the F_MONO_L number right of it runs under the weapon gauge at 470. The demo strip uses a 340 px bar with the number at 400; the HUD agent should keep that or move the gauge.

## Deploy

Copy `web/red-tag-rumble.wasc` to `/userdata/roms/wasmcart/` on the Batocera box and restart EmulationStation. `batocera.conf` carries `wasmcart.gfxbackend=gl` and `wasmcart.rewind=0`. The Pages workflow publishes `web/` on every push to main.
