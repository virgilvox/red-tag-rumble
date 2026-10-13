# RED TAG RUMBLE

A split-screen robot combat cart for wasmcart, built at HeatSync Labs for the shop's game station. One to four gamepads. Sixty seconds in the garage spending twenty red tags on a chassis, a drive, a weapon and armour, then a fight in one of six hackerspace arenas with hazards on a timer, a house robot that clears the aisle when the fight stalls, and a KO that goes slow motion.

A red tag on a project means fourteen days and then it is free for the taking or it is trash. Everything on the garage shelf is red tagged. Build a bot from other people's abandoned projects, drive it into another bot made of other people's abandoned projects, and whatever survives goes back on the shelf.

## Play it

- Browser: `npm run serve` and open the page; pads or the keyboard as pad 1 (arrows, Z, X, A, S, Enter).
- Game station: copy `web/red-tag-rumble.wasc` to `/userdata/roms/wasmcart/` on the Batocera box.

## Build it

Needs emsdk at `~/emsdk`, node 22 or newer, and the submodules (`git submodule update --init`).

```sh
./build.sh                 # build/red-tag-rumble.wasc
./build.sh balance         # the native balance tool
node tools/shot.mjs build/red-tag-rumble.wasc build/shots/s 120 tools/scripts/2p-fight.json
```

`HANDOFF.md` has the file ownership, the header contract and every build and test command. `docs/design/FINAL-bots.md` is the design specification.

## Stack

three.c (three.js r186 in C99) for the 3D, a CPU painted 1280 x 720 HUD composited in a second render pass, Bellows for the live sound effects, baked WAV stems for the music, and a deterministic 60 Hz simulation that compiles natively for balancing. The same `.wasc` runs in the browser, in RetroArch and on the station.

MIT licensed. Fonts under the SIL Open Font License (assets/fonts/OFL.txt).
