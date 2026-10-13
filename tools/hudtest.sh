#!/usr/bin/env bash
# Builds the native HUD test (tools/hudtest.c) against hud.c and draw_menus.c
# with the fake menu state, then writes every menu scene to build/shots/menus-*.png.
#
#   tools/hudtest.sh              # build and shoot every scene
#   tools/hudtest.sh garage-2p    # one scene
#
# Needs the generated logo header, so run ./build.sh once first (build/obj/game/logo_mesh.h).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
OUT=build/hudtest_obj
mkdir -p "$OUT" build/shots
[ -f build/obj/game/logo_mesh.h ] || node tools/gen-logo.mjs assets/brand/hsl-mark-paths.json build/obj/game/logo_mesh.h
INC=(-I src -I third_party -I third_party/three.c/include -I third_party/wasmcart/include -I build/obj/game)
CF=(-std=gnu11 -O1 -g -Wall -Wno-unused-function -ffp-contract=off)
# HUD_DIV=2 tools/hudtest.sh builds the half size buffer the station uses at 540p
[ "${HUD_DIV:-1}" = 2 ] && CF+=(-DHUD_DIV=2)
# hud.c wants the host asset imports as externs; hudtest.c provides them
cc "${CF[@]}" -D__wasm__ -Wno-ignored-attributes -Wno-unknown-attributes "${INC[@]}" -c src/hud.c -o "$OUT/hud.o"
cc "${CF[@]}" -DRTR_FAKE_MENUS "${INC[@]}" -c src/draw_menus.c -o "$OUT/draw_menus.o"
cc "${CF[@]}" "${INC[@]}" -c src/game.c -o "$OUT/game.o"
cc "${CF[@]}" "${INC[@]}" -c src/arena.c -o "$OUT/arena.o"
cc "${CF[@]}" -DRTR_HEADLESS -I src -c src/sim.c -o "$OUT/sim.o"
cc "${CF[@]}" -DRTR_HEADLESS -I src -c src/bot.c -o "$OUT/bot.o"
cc "${CF[@]}" -DRTR_FAKE_MENUS "${INC[@]}" -c tools/hudtest.c -o "$OUT/hudtest.o"
cc "$OUT"/hud.o "$OUT"/draw_menus.o "$OUT"/game.o "$OUT"/arena.o "$OUT"/sim.o "$OUT"/bot.o "$OUT"/hudtest.o -lm -o build/hudtest
build/hudtest build/shots "${1:-}" "${HUD_DIV:-1}"
