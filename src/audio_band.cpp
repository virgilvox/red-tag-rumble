/* audio_band.cpp: the Bellows SFX band (12 patches, 4 spinner whines, the
 * crowd bed) and the song data (spec 11.2, 11.3). Compiled with em++ -std=c++17
 * -fno-exceptions -fno-rtti -DBELLOWS_FAST_MATH=1; natively with -DMUSIC_BAKE
 * by tools/bake_music.cpp; the songs stay live in the cart with -DMUSIC_LIVE.
 * Owned by the audio agent (item 17). Stub: renders silence. Nothing here may
 * touch libc++; <math.h> and <stdint.h> only. */
#include <stdint.h>
#include <math.h>
#include "audio.h"

static uint32_t band_rate = 48000;

extern "C" void audio_band_init(uint32_t rate) { band_rate = rate ? rate : 48000; }

extern "C" void audio_band_render(float *stereo, int frames, uint32_t rate) {
  (void)rate;
  (void)stereo; (void)frames;
}

extern "C" void audio_play(int sfx) { (void)sfx; }
extern "C" void audio_play_var(int sfx, int var) { (void)sfx; (void)var; }
extern "C" void audio_whine(int slot, float level, float pitch) { (void)slot; (void)level; (void)pitch; }
extern "C" void audio_crowd(float level) { (void)level; }
extern "C" void audio_part(int row, int option) { (void)row; (void)option; }
