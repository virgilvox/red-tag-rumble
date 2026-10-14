/* audio_pcm.c: the wc_pcm_mixer side of the sound (spec 11.1, 11.2).
 *
 * The songs are baked to WAV by tools/bake_music.cpp and played here as
 * stems through wc_pcm_mixer.h; the Bellows band stays live only for the
 * SFX, the whines and the crowd, and is rendered over the stems by
 * audio_band_render, which also owns the master limiter.
 *
 * A song is a list of segments, each a bar count and up to three sounds
 * (base, drums, lead) that start on the same frame. ARENA is STOMP then
 * BREAK, looping from the top: one 8 bar base stem under both, a kit stem
 * per section and one riff stem, so the 16 bar song costs 4 files of 8 bars
 * (see HANDOFF: the size budget). Every audio_music call and every switch
 * that lands on a beat writes a [music] line to the log. The
 * sequencer walks the mixer's own 16.16 step so a segment change, the loop
 * and the OVERTIME switch land on an exact source frame: the mix is split
 * at the boundary, the channels are restarted there, and their position is
 * carried so the stems never drift from each other or from the beat.
 *
 * Gains: base 1.0, drums 1.0, lead 0.0 until audio_tension(1) fades it in
 * over 0.5 s; audio_slowmo(1) ramps base and lead to 0.15 over 6 frames;
 * audio_duck for the pause; a 0.5 s crossfade into GARAGE or silence; a hard
 * cut into ARENA; OVERTIME on the next beat.
 *
 * MUSIC=live (-DMUSIC_LIVE): no WAVs, the band plays the songs itself and
 * the same calls drive its stem gains. Owned by the audio agent (item 17). */
#include "audio.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef MUSIC_LIVE
#define WC_PCM_MIXER_IMPLEMENTATION
#include "wc_pcm_mixer.h"
#include "wasmcart.h"
#endif

#define STEMS 3
#define MAX_SEG 4

#if defined(RTR_AUDIO_PROBE) && !defined(MUSIC_LIVE)
#include <stdio.h>
static uint64_t probe_out;   /* output frames rendered so far, for the log */
#endif
#ifndef MUSIC_LIVE
#include <stdio.h>           /* snprintf for the [music] log lines */
#endif

static double carry;
static uint32_t host_rate = 48000;
static int sfx_level = 8, music_level = 6;
static float scratch[8192 * 2];
static int cur_mode;              /* the mode the game asked for last */
static int tension, slowmo;
static float duck_t = 1.0f;
static float hit_duck_left;                    /* seconds left of the hit duck (audio_duck_hit) */
#define HIT_DUCK_GAIN 0.63f                    /* 4 dB under the bed */
#define HIT_DUCK_S 0.15f

#ifndef MUSIC_LIVE
/* -- the baked stems ------------------------------------------------ */
enum { W_GARAGE, W_OVERTIME, W_BASE, W_STOMP, W_BREAK, W_RIFF, W_COUNT };
static const char *const WAV_NAME[W_COUNT] = {
  "garage.wav", "overtime.wav", "arena_base.wav", "arena_stomp.wav", "arena_break.wav", "arena_riff.wav" };
static int snd[W_COUNT];

/* ARENA is the half time STOMP verse then the breakbeat BREAK chorus, looping
 * from the top: one base stem under both, a kit stem per section, and the
 * riser riff as the lead stem that audio_tension fades in */
typedef struct { int bars; int wav[STEMS]; } seg_t;        /* wav: W_* or -1 */
typedef struct { int nseg, loop; seg_t seg[MAX_SEG]; } song_t;
static const song_t SONGS[4] = {
  { 0, 0, { { 0, { -1, -1, -1 } } } },
  { 1, 0, { { 16, { W_GARAGE, -1, -1 } } } },
  { 2, 0, { { 8, { W_BASE, W_STOMP, W_RIFF } }, { 8, { W_BASE, W_BREAK, W_RIFF } } } },
  { 1, 0, { { 8, { W_OVERTIME, -1, -1 } } } },
};
/* the stems are baked to the ceiling for 16 bit resolution; this is where a
 * song sits in the cart before the MUSIC row: the fight 6 to 10 dB under the
 * hit sounds (tools/bake_music.cpp prints the numbers), the garage a menu */
static const float MODE_TRIM[4] = { 0.0f, 0.5f, 0.4f, 0.45f };
static const char *const MODE_NAME[4] = { "off", "GARAGE", "ARENA", "OVERTIME" };

/* the playing song */
static int play_mode;             /* 0 none */
static int seg;                   /* segment index */
static uint64_t pos16;            /* position in the segment, source frames 16.16 */
static uint32_t step16;           /* the mixer's step for these sounds */
static int src_rate;              /* the WAVs' rate */
static int spb;                   /* source frames per sixteenth */
static int ch[STEMS];             /* mixer channels, -1 none */
static float g[STEMS], gt[STEMS], gr[STEMS];   /* stem gain, target, ramp seconds */
static float fade, fade_t, fade_r;             /* song fade and its ramp */
static float duck;                             /* ramped duck */
static int pending_mode = -1;                  /* a switch waiting for the next beat */
/* the outgoing song during a crossfade */
static int out_ch[STEMS];
static float out_fade, out_g[STEMS], out_secs = 0.5f, out_trim = 1.0f;

/* the settings rows, 0..10, as a fader law: 6 is -3 dB, 3 is -8 dB */
static float level_gain(int level) { float x = level / 10.0f; return x <= 0.0f ? 0.0f : powf(x, 0.7f); }
static float stem_vol(int i) { return g[i] * fade * duck * level_gain(music_level) * MODE_TRIM[play_mode]; }

static void music_log(const char *what, int mode) {
  char m[64];
  int n = snprintf(m, sizeof m, "[music] %s %s", what, MODE_NAME[mode & 3]);
  wc_log(m, (unsigned)(n > 0 ? n : 0));
}

static void seg_frames(int *frames, int *beat) {
  const song_t *s = &SONGS[play_mode];
  *frames = s->seg[seg].bars * 16 * spb;
  *beat = 4 * spb;
}

static void stop_song(void) {
  for (int i = 0; i < STEMS; i++) if (ch[i] >= 0) { wc_mixer_stop(ch[i]); ch[i] = -1; }
}

/* start a segment's sounds on the same frame, carrying the fraction so the
 * stems agree with the sequencer's clock to the sample */
static void start_segment(uint64_t frac) {
  stop_song();
  const song_t *s = &SONGS[play_mode];
  if (s->nseg == 0) return;
  const seg_t *sg = &s->seg[seg];
  pos16 = frac;
  step16 = 0;
  for (int i = 0; i < STEMS; i++) {
    if (sg->wav[i] < 0 || snd[sg->wav[i]] < 0) continue;
    int c = wc_mixer_play(snd[sg->wav[i]], stem_vol(i), 0);
    if (c < 0) continue;
    /* the mixer's step assumes a 48 kHz host; correct it for the real rate */
    if (host_rate != WC_MIXER_RATE) wc_mixer_set_pitch(c, (float)WC_MIXER_RATE / (float)host_rate);
    wc_mixer_channels[c].pos_frac = frac;
    step16 = wc_mixer_channels[c].step_frac;
    ch[i] = c;
  }
  if (!step16) step16 = (uint32_t)(((uint64_t)src_rate << 16) / host_rate);
}

static void begin_song(int mode, float fade_from) {
  play_mode = mode;
  seg = 0;
  spb = audio_song_step(mode, (uint32_t)src_rate);
  fade = fade_from; fade_t = 1.0f; fade_r = 0.5f;
  start_segment(0);
}

/* the segment after this one, or the loop */
static void next_segment(uint64_t frac) {
  const song_t *s = &SONGS[play_mode];
  if (++seg >= s->nseg) seg = s->loop;
  start_segment(frac);
}

static void set_targets(void) {
  gt[0] = slowmo ? 0.15f : 1.0f; gr[0] = 0.1f;
  gt[1] = 1.0f; gr[1] = 0.1f;
  if (slowmo) { gt[2] = 0.15f; gr[2] = 0.1f; }
  else { gt[2] = tension ? 1.0f : 0.0f; gr[2] = 0.5f; }
}

static void load_stems(void) {
  wc_mixer_init();
  for (int i = 0; i < W_COUNT; i++) {
    snd[i] = -1;
    int n = wc_asset_size(WAV_NAME[i], (unsigned)strlen(WAV_NAME[i]));
    if (n <= 44) continue;
    unsigned char *b = (unsigned char *)malloc((size_t)n);
    if (!b) continue;
    if (wc_load_asset(WAV_NAME[i], (unsigned)strlen(WAV_NAME[i]), b, (unsigned)n) == n) snd[i] = wc_mixer_load_wav(b, n);
    free(b);
    if (snd[i] >= 0 && !src_rate) src_rate = wc_mixer_sounds[snd[i]].sample_rate;
  }
  if (!src_rate) src_rate = 22050;
  for (int i = 0; i < STEMS; i++) { ch[i] = -1; out_ch[i] = -1; g[i] = gt[i] = i == 2 ? 0.0f : 1.0f; gr[i] = 0.1f; }
  duck = 1.0f; fade = 1.0f; fade_t = 1.0f; fade_r = 0.5f;
}

/* one frame's worth of gain motion (dt seconds) and the volumes to the mixer */
static void ramp_gains(float dt) {
  for (int i = 0; i < STEMS; i++) {
    float d = gt[i] - g[i], step = gr[i] > 0 ? dt / gr[i] : 1.0f;
    if (d > step) g[i] += step; else if (d < -step) g[i] -= step; else g[i] = gt[i];
  }
  { float d = fade_t - fade, step = fade_r > 0 ? dt / fade_r : 1.0f;
    if (d > step) fade += step; else if (d < -step) fade -= step; else fade = fade_t; }
  { /* the pause duck ramps over 0.1 s; a hit duck drops in 20 ms and recovers over 0.1 s */
    float target = duck_t;
    if (hit_duck_left > 0.0f) { hit_duck_left -= dt; if (target > HIT_DUCK_GAIN) target = HIT_DUCK_GAIN; }
    float d = target - duck, step = d < 0 && hit_duck_left > 0.0f ? dt / 0.02f : dt / 0.1f;
    if (d > step) duck += step; else if (d < -step) duck -= step; else duck = target; }
  for (int i = 0; i < STEMS; i++) if (ch[i] >= 0) wc_mixer_set_volume(ch[i], stem_vol(i));
  if (out_fade > 0.0f) {
    out_fade -= dt / out_secs;
    if (out_fade <= 0.0f) { out_fade = 0.0f; for (int i = 0; i < STEMS; i++) if (out_ch[i] >= 0) { wc_mixer_stop(out_ch[i]); out_ch[i] = -1; } }
    else for (int i = 0; i < STEMS; i++) if (out_ch[i] >= 0) wc_mixer_set_volume(out_ch[i], out_g[i] * out_fade * duck * level_gain(music_level) * out_trim);
  }
  if (play_mode && fade <= 0.0f && fade_t <= 0.0f) { stop_song(); play_mode = 0; }
}

/* mix n frames of stems at offset into the scratch, walking the song clock */
static void mix_stems(int offset, int n) {
  while (n > 0) {
    int frames = 0, beat = 1;
    if (play_mode) seg_frames(&frames, &beat);
    if (frames <= 0) {   /* silence, or a fading outgoing set: no clock to walk */
      uint32_t cur = (uint32_t)offset;
      wc_mixer_mix_f32(scratch, (uint32_t)(offset + n), &cur, n);
      return;
    }
    uint64_t len16 = (uint64_t)frames << 16, beat16 = (uint64_t)beat << 16;
    uint64_t until_end = (len16 - pos16 + step16 - 1) / step16;
    int take = n;
    if ((uint64_t)take > until_end) take = (int)until_end;
    int at_beat = 0;
    if (pending_mode >= 0) {
      uint64_t rem = beat16 - (pos16 % beat16);
      uint64_t until_beat = (rem + step16 - 1) / step16;
      if ((uint64_t)take >= until_beat) { take = (int)until_beat; at_beat = 1; }
    }
    if (take > 0) {
      uint32_t cur = (uint32_t)offset;
      wc_mixer_mix_f32(scratch, (uint32_t)(offset + take), &cur, take);
      pos16 += (uint64_t)take * step16;
      offset += take; n -= take;
    }
    if (at_beat) {
      int m = pending_mode; pending_mode = -1;
#ifdef RTR_AUDIO_PROBE
      printf("[audio] mode %d on the beat at out frame %llu (src pos %llu of beat %d)\n", m, (unsigned long long)(probe_out + (uint64_t)offset), (unsigned long long)(pos16 >> 16), beat);
#endif
      uint64_t frac = pos16 % beat16;
      music_log("on the beat", m);
      begin_song(m, 1.0f);
      pos16 = frac;
      for (int i = 0; i < STEMS; i++) if (ch[i] >= 0) wc_mixer_channels[ch[i]].pos_frac = frac;
      continue;
    }
    if (pos16 >= len16) next_segment(pos16 - len16);
  }
}

static void crossfade_to(int mode, float secs, float fade_from) {
  /* the playing stems become the outgoing set */
  if (out_fade > 0.0f) for (int i = 0; i < STEMS; i++) if (out_ch[i] >= 0) { wc_mixer_stop(out_ch[i]); out_ch[i] = -1; }
  for (int i = 0; i < STEMS; i++) { out_ch[i] = ch[i]; out_g[i] = g[i]; ch[i] = -1; }
  out_fade = play_mode ? fade : 0.0f;
  out_trim = MODE_TRIM[play_mode];
  out_secs = secs;
  pending_mode = -1;
  if (mode) begin_song(mode, fade_from); else play_mode = 0;
}
#endif

/* -- the public API ------------------------------------------------- */

void audio_init(uint32_t rate) {
  host_rate = rate ? rate : 48000;
  carry = 0;
  cur_mode = 0; tension = 0; slowmo = 0; duck_t = 1.0f;
  audio_band_init(host_rate);
  audio_band_levels(sfx_level, music_level);
#ifndef MUSIC_LIVE
  load_stems();
#endif
}

void audio_set_levels(int sfx, int music) {
  sfx_level = sfx < 0 ? 0 : sfx > 10 ? 10 : sfx;
  music_level = music < 0 ? 0 : music > 10 ? 10 : music;
  audio_band_levels(sfx_level, music_level);
}

void audio_music(int mode) {
  if (mode < MUSIC_OFF || mode > MUSIC_OVERTIME || mode == cur_mode) return;
  cur_mode = mode;
  audio_band_crowd_bed(mode >= MUSIC_ARENA ? 0.12f : 0.0f);
#ifdef MUSIC_LIVE
  audio_band_music(mode);
#else
  music_log("mode", mode);
  if (mode == MUSIC_OVERTIME && play_mode == MUSIC_ARENA) { pending_mode = mode; return; }
  if (mode == MUSIC_GARAGE || mode == MUSIC_OFF) { crossfade_to(mode, 0.5f, 0.0f); return; }
  /* a hard cut: ARENA on FIGHT, or anything from silence; the old stems get
   * 20 ms out so the cut itself never clicks */
  crossfade_to(mode, 0.02f, 1.0f);
#endif
}

void audio_slowmo(int on) {
  slowmo = on ? 1 : 0;
#ifdef MUSIC_LIVE
  audio_band_stems(slowmo ? 0.15f : 1.0f, 1.0f, slowmo ? 0.15f : tension ? 1.0f : 0.0f);
#else
  set_targets();
#endif
}

void audio_tension(int on) {
  tension = on ? 1 : 0;
#ifdef MUSIC_LIVE
  audio_band_stems(slowmo ? 0.15f : 1.0f, 1.0f, slowmo ? 0.15f : tension ? 1.0f : 0.0f);
#else
  set_targets();
#endif
}

void audio_duck(float gain) {
  duck_t = gain < 0.0f ? 0.0f : gain > 1.0f ? 1.0f : gain;
  audio_band_duck(duck_t);
}

/* a landed hit: the stems drop HIT_DUCK_GAIN for HIT_DUCK_S so the clang sits over the music (11.3) */
void audio_duck_hit(void) { if (duck_t >= 0.99f) hit_duck_left = HIT_DUCK_S; }

#ifdef RTR_AUDIO_PROBE
/* a scripted run of every call for the rig (EXTRA_CFLAGS=-DRTR_AUDIO_PROBE=1 or 2),
 * so WAV=build/out.wav node tools/shot.mjs ... can be measured; not in the game */
static int probe_frame;
static void probe_step(void) {
  int f = probe_frame++;
#if RTR_AUDIO_PROBE == 1
  switch (f) {
    case 10: audio_music(MUSIC_GARAGE); break;
    case 120: audio_play(SFX_MENU_MOVE); break;
    case 150: audio_play(SFX_MENU_OK); break;
    case 200: audio_part(2, 9); break;   /* the HORIZONTAL spin up */
    case 300: audio_music(MUSIC_ARENA); break;
    case 360: audio_play(SFX_HIT_LIGHT); break;
    case 420: audio_play(SFX_HIT_HEAVY); audio_crowd(0.8f); break;
    case 480: audio_play(SFX_CLASH); audio_crowd(1.0f); break;
    case 600: audio_tension(1); break;
    case 800: audio_slowmo(1); break;
    case 890: audio_slowmo(0); break;
    case 1100: audio_play(SFX_KO); audio_crowd(1.0f); break;
    case 1200: audio_duck(0.3f); break;
    case 1260: audio_duck(1.0f); break;
    case 1400: audio_music(MUSIC_OVERTIME); break;
    case 1600: audio_music(MUSIC_GARAGE); break;
    case 1750: audio_music(MUSIC_OFF); break;
    default: break;
  }
  if (f >= 500 && f < 700) audio_whine(0, 0.8f, (f - 500) / 200.0f);
  if (f == 700) audio_whine(0, 0.0f, 1.0f);
#else
  if (f == 60) audio_music(MUSIC_ARENA);
  if (f == 2000) audio_tension(1);
#endif
}
#endif

void audio_render(float *ring, uint32_t cap, uint32_t *cursor, uint32_t rate, double dt_ms) {
#ifdef RTR_AUDIO_PROBE
  probe_step();
#endif
  double want = (double)rate * dt_ms / 1000.0 + carry;
  int frames = (int)want;
  carry = want - frames;
  if (frames <= 0) return;
  if ((uint32_t)frames > cap / 2) { frames = (int)(cap / 2); carry = 0; }
  if (frames > 8192) frames = 8192;
  memset(scratch, 0, (size_t)frames * 2 * sizeof(float));
#ifndef MUSIC_LIVE
  ramp_gains((float)frames / (float)rate);
  mix_stems(0, frames);
#endif
  audio_band_render(scratch, frames, rate);
#if defined(RTR_AUDIO_PROBE) && !defined(MUSIC_LIVE)
  probe_out += (uint64_t)frames;
#endif
  uint32_t c = *cursor;
  for (int i = 0; i < frames; i++) {
    ring[c * 2] = scratch[i * 2];
    ring[c * 2 + 1] = scratch[i * 2 + 1];
    c = (c + 1) % cap;
  }
  *cursor = c;
}
