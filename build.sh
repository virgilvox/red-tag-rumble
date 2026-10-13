#!/usr/bin/env bash
# Builds RED TAG RUMBLE as a wasmcart GL cart (build/red-tag-rumble.wasc).
#
#   ./build.sh                 # full build
#   ./build.sh smoke           # the pipeline smoke test cart (tools/smoke.c)
#   ./build.sh balance         # the native balance tool (build/balance), headless sim only
#   ./build.sh music           # bake the songs to assets/music/*.wav (needs clang++)
#   ./build.sh lint            # the house lint over strings and docs
#   REGEN_STATES=1 ./build.sh  # regenerate src/gen/rtr_gl.c from tools/states.txt
#   MUSIC=live ./build.sh      # keep the Bellows band's songs live (composing on the Mac)
#   HUD_DIV=2 ./build.sh       # force the half-size HUD buffer
#   EXTRA_CFLAGS=... EXTRA_SRCS=... ./build.sh   # a probe: extra flags and sources for the cart link
#
# Needs: emcc (emsdk), node >= 22, and the submodules
# (git submodule update --init).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
THREE="$ROOT/third_party/three.c"
WASMCART="$ROOT/third_party/wasmcart"
OUT="$ROOT/build"
TOBJ="$OUT/obj/three_core"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"
HEADLESS_SRCS="src/sim.c src/bot.c src/arena.c src/ai.c"

# -- the native targets need no emscripten ---------------------------
if [ "${1:-}" = lint ]; then
  exec "$ROOT/tools/lint.sh"
fi

if [ "${1:-}" = balance ]; then
  mkdir -p "$OUT"
  # the headless sources may include nothing from three.c, hud.h or wasmcart:
  # that line is what the native build and the replay depend on
  deps="$(cd "$ROOT" && cc -std=gnu11 -DRTR_HEADLESS -I src -MM $HEADLESS_SRCS tools/balance.c)"
  if printf '%s\n' "$deps" | grep -E 'three\.h|hud\.h|wasmcart|wc_[a-z_]*\.h|glyphs\.h|audio\.h|world\.h|fx\.h|camera\.h|draw\.h|game\.h|render\.h' >/dev/null; then
    echo "balance: a headless source includes a renderer, HUD or host header:" >&2
    printf '%s\n' "$deps" | grep -E 'three\.h|hud\.h|wasmcart|wc_[a-z_]*\.h|glyphs\.h|audio\.h|world\.h|fx\.h|camera\.h|draw\.h|game\.h|render\.h' >&2
    exit 1
  fi
  if (cd "$ROOT" && grep -nE '\b(sinf|cosf|atan2f|expf|tanf|powf|sin|cos|atan2|exp)\s*\(' $HEADLESS_SRCS | grep -v 'rtr_' ); then
    echo "balance: libm trig in a headless source (use rtr_sin, rtr_cos, rtr_atan2)" >&2
    exit 1
  fi
  (cd "$ROOT" && cc -O2 -std=gnu11 -Wall -Wno-unused-function -ffp-contract=off -DRTR_HEADLESS -I src \
    $HEADLESS_SRCS tools/balance.c -lm -o "$OUT/balance")
  echo "built build/balance"
  exit 0
fi

if [ "${1:-}" = music ]; then
  # the songs to assets/music/*.wav (16 bit mono, MUSIC_RATE Hz, default 24000)
  # and every SFX to build/sfx_preview.wav; the tool prints peak and RMS per file
  mkdir -p "$OUT" "$ROOT/assets/music"
  (cd "$ROOT" && clang++ -std=c++17 -O2 -Wall -fno-exceptions -fno-rtti -ffp-contract=off -DMUSIC_BAKE -DBELLOWS_FAST_MATH=1 \
    -I third_party/bellows -I src tools/bake_music.cpp src/audio_band.cpp -o "$OUT/bake_music")
  "$OUT/bake_music" "$ROOT/assets/music" "$OUT"
  echo "baked assets/music"
  exit 0
fi

# -- the cart --------------------------------------------------------
if ! command -v emcc >/dev/null; then
  [ -f "$HOME/emsdk/emsdk_env.sh" ] && source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
fi
command -v emcc >/dev/null || { echo "emcc not found: install emsdk (https://emscripten.org)" >&2; exit 1; }
[ -f "$THREE/include/three.h" ] || git -C "$ROOT" submodule update --init
[ -d "$WASMCART/node_modules" ] || (cd "$WASMCART" && npm install --silent)

mkdir -p "$TOBJ" "$OUT/obj/game"
OPT="-O2 -DNDEBUG -flto -msimd128 -msse2"

# three.c, built once and cached (the GL program tables only)
pids=()
for s in math core geometry curves animation raycaster loaders gltf renderer backend_gles gen_program gen/programs_core_gl gen/programs_dfg; do
  o="$TOBJ/${s//\//_}.o"
  if [ ! -f "$o" ] || [ "$THREE/src/$s.c" -nt "$o" ]; then
    emcc -c "$THREE/src/$s.c" -o "$o" $OPT -std=c99 -DT3_WASMCART -DT3_TABLE_CORE \
      -I "$WASMCART/include" -I "$THREE/include" -I "$THREE/src" &
    pids+=($!)
    while [ "$(jobs -r | wc -l)" -ge "$JOBS" ]; do sleep 0.05; done
  fi
done
fail=0; for p in "${pids[@]+"${pids[@]}"}"; do wait "$p" || fail=1; done
[ $fail = 0 ] || { echo "three.c compile failed" >&2; exit 1; }

# the r186 programs this game draws (tools/states.txt), as a C table.
# The table is committed, so a normal build never needs three.js. Regenerate
# only when asked (REGEN_STATES=1) or when it is missing; that needs
#   (cd third_party/three.c && npm install --ignore-scripts)
if [ "${REGEN_STATES:-0}" = 1 ] || [ ! -f "$ROOT/src/gen/rtr_gl.c" ]; then
  (cd "$ROOT" && node "$THREE/tools/gen-project-table.mjs" --name rtr --out src/gen --states tools/states.txt --cache build/t3-captures)
fi

# generated sources: the HeatSync mark as polylines
node "$ROOT/tools/gen-logo.mjs" "$ROOT/assets/brand/hsl-mark-paths.json" "$OUT/obj/game/logo_mesh.h"

GAMEF="-DWC_USE_GL"
[ "${MUSIC:-}" = live ] && GAMEF="$GAMEF -DMUSIC_LIVE=1"
[ "${HUD_DIV:-1}" = 2 ] && GAMEF="$GAMEF -DHUD_DIV=2"
GAMEF="$GAMEF ${EXTRA_CFLAGS:-}"

if [ "${1:-}" = smoke ]; then
  SRCS="$ROOT/tools/smoke.c $ROOT/src/gen/rtr_gl.c"; NAME=smoke
else
  SRCS="$(ls "$ROOT"/src/*.c) $ROOT/src/gen/rtr_gl.c ${EXTRA_SRCS:-}"; NAME=red-tag-rumble
fi

# the Bellows audio engine (header-only C++17, no exceptions, no RTTI, no libc++)
GAMEOBJ=""
if [ "$NAME" = red-tag-rumble ]; then
  em++ -c "$ROOT/src/audio_band.cpp" -o "$OUT/obj/game/audio_band.o" $OPT -std=c++17 -fno-exceptions -fno-rtti \
    -ffp-contract=off -DBELLOWS_FAST_MATH=1 $GAMEF -I "$ROOT/third_party/bellows" -I "$ROOT/src" -I "$WASMCART/include"
  GAMEOBJ="$OUT/obj/game/audio_band.o"
fi

emcc $OPT -std=gnu11 -Wall -Wno-unused-function -ffp-contract=off -DT3_WASMCART $GAMEF \
  -I "$WASMCART/include" -I "$THREE/include" -I "$ROOT/third_party" \
  -I "$ROOT/src" -I "$THREE/src" -I "$OUT/obj/game" \
  $SRCS $GAMEOBJ "$TOBJ"/*.o \
  -sSTANDALONE_WASM=1 -sEXPORTED_FUNCTIONS='["_wc_init","_wc_render","_wc_get_info","_wc_set_seed"]' \
  -sERROR_ON_UNDEFINED_SYMBOLS=0 -sINITIAL_MEMORY=67108864 -sALLOW_MEMORY_GROWTH=1 \
  --no-entry -o "$OUT/$NAME.wasm"

# a cart may import only gl, wc_* and WASI
WASMCART="$WASMCART" node "$THREE/wasm/check-imports.mjs" "$OUT/$NAME.wasm"

rm -rf "$OUT/assets" && mkdir -p "$OUT/assets"
cp "$ROOT"/assets/fonts/*.ttf "$OUT/assets/"
# the baked songs (tools/bake_music.cpp); a MUSIC=live build renders them in the cart instead
if [ "${MUSIC:-}" != live ]; then
  for w in "$ROOT"/assets/music/*.wav; do [ -f "$w" ] && cp "$w" "$OUT/assets/"; done
fi

# manifest: an offline cart, no --ws grant
node "$WASMCART/bin/wasmcart-pack.js" --wasm "$OUT/$NAME.wasm" --assets "$OUT/assets" \
  --name "RED TAG RUMBLE" --version 0.1.0 --width 1280 --height 720 --players 4 \
  --controls dpad,left_stick,right_stick,a,b,x,y,l,r,left_trigger,right_trigger,start \
  --output "$OUT/$NAME.wasc" >/dev/null
[ "$NAME" = red-tag-rumble ] && cp "$OUT/$NAME.wasc" "$ROOT/web/red-tag-rumble.wasc"
echo "built build/$NAME.wasc ($(wc -c < "$OUT/$NAME.wasc" | tr -d ' ') bytes)"
