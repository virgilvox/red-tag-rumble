/* audio_pcm.c: the wc_pcm_mixer side. Loads the five baked stems, drives the
 * music modes, fades and beat alignment, and owns audio_render, which mixes
 * the stems then the Bellows band into the host ring (spec 11.1, 11.2).
 * Owned by the audio agent (item 17). Stub: the ring timing is real (frames per
 * render are rate * dt_ms / 1000 with a fractional carry, capped at half the
 * ring), the output is silence. */
#include "audio.h"
#include <string.h>

static double carry;
static int sfx_level = 8, music_level = 6;
static float scratch[8192 * 2];

void audio_init(uint32_t rate) { audio_band_init(rate); carry = 0; }
void audio_set_levels(int sfx, int music) { sfx_level = sfx; music_level = music; }
int  audio_levels_sum(void) { return sfx_level + music_level; }   /* keeps the stub's state live until the band reads it */
void audio_music(int mode) { (void)mode; }
void audio_slowmo(int on) { (void)on; }
void audio_tension(int on) { (void)on; }
void audio_duck(float gain) { (void)gain; }

void audio_render(float *ring, uint32_t cap, uint32_t *cursor, uint32_t rate, double dt_ms) {
  double want = (double)rate * dt_ms / 1000.0 + carry;
  int frames = (int)want;
  carry = want - frames;
  if (frames <= 0) return;
  if ((uint32_t)frames > cap / 2) { frames = (int)(cap / 2); carry = 0; }
  if (frames > 8192) frames = 8192;
  memset(scratch, 0, (size_t)frames * 2 * sizeof(float));
  audio_band_render(scratch, frames, rate);
  uint32_t c = *cursor;
  for (int i = 0; i < frames; i++) {
    ring[c * 2] = scratch[i * 2];
    ring[c * 2 + 1] = scratch[i * 2 + 1];
    c = (c + 1) % cap;
  }
  *cursor = c;
}
