/* bake_music: renders the three songs of spec 11.2 to assets/music as the
 * stems audio_pcm.c sequences, every SFX to one preview WAV, and a 20 s
 * preview of each song under the hit sounds.
 *
 *   bake_music <assets/music dir> [preview dir]
 *   MUSIC_RATE=22050 (default) sets the stem rate; the previews are at 48 kHz
 *
 * Built natively by ./build.sh music with -DMUSIC_BAKE against audio_band.cpp,
 * so the songs the cart would play live (MUSIC=live) and the WAVs are one
 * source. Stems are 16 bit mono:
 *
 *   garage.wav         GARAGE, 16 bars, the loop
 *   overtime.wav       OVERTIME, 8 bars, the loop
 *   arena_base.wav     ARENA, 8 bar loop: the bass and everything that is not the kit or the riff
 *   arena_stomp.wav    ARENA STOMP (the half time verse), 8 bar loop: the kit and the metal
 *   arena_break.wav    ARENA BREAK (the breakbeat chorus), 8 bar loop: the kit, the metal, the stabs
 *   arena_riff.wav     ARENA, 8 bar loop: the riser riff (the lead stem)
 *
 * A loop file is the second pass of its section so the tails of the end are
 * already under the start and the join is seamless. Every ARENA file carries
 * the same gain, measured on the full mix of both sections, so the stems sum
 * in the mixer to the ceiling and never clip. The tool prints sample peak,
 * true peak (4x oversampled), RMS and a rough spectral balance per file, the
 * SFX peaks, and how far the fight music sits under the hit sounds at the
 * cart's default levels. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bellows/dsp/filters.h"

extern "C" {
#include "audio.h"
}

namespace {

struct Wav { float *mono; int n; };

float Db(float x) { return x > 1e-9f ? 20.0f * log10f(x) : -120.0f; }

void Stats(const float *m, int n, float *peak, float *rms) {
  double acc = 0; float pk = 0;
  for (int i = 0; i < n; i++) { float a = fabsf(m[i]); if (a > pk) pk = a; acc += (double)m[i] * m[i]; }
  *peak = pk; *rms = n ? (float)sqrt(acc / n) : 0.0f;
}

/* the true peak: the signal 4x oversampled with an 8 tap windowed sinc
 * between the samples, as a meter does, so the ceiling holds after the
 * host's reconstruction filter */
float TruePeak(const float *m, int n) {
  float pk = 0;
  static float k[3][8];
  static bool made = false;
  if (!made) {
    for (int p = 0; p < 3; p++) for (int t = 0; t < 8; t++) {
      float x = (float)(t - 3) - (p + 1) * 0.25f;
      float s = x == 0.0f ? 1.0f : sinf(3.14159265f * x) / (3.14159265f * x);
      float w = 0.5f + 0.5f * cosf(3.14159265f * x / 4.5f);
      k[p][t] = s * w;
    }
    made = true;
  }
  for (int i = 0; i < n; i++) {
    float a = fabsf(m[i]); if (a > pk) pk = a;
    for (int p = 0; p < 3; p++) {
      float y = 0;
      for (int t = 0; t < 8; t++) { int j = i + t - 3; if (j >= 0 && j < n) y += m[j] * k[p][t]; }
      a = fabsf(y); if (a > pk) pk = a;
    }
  }
  return pk;
}

/* a rough spectral balance: energy under 250 Hz, 250 to 2500, over 2500, as
 * percentages of the total (second order Butterworth splits) */
void Balance(const float *m, int n, int rate, float *lo, float *mid, float *hi) {
  bellows::Svf lp, hp;
  lp.Init((float)rate); lp.SetMode(bellows::SvfMode::kLp); lp.Set(250.0f, 0.7071f);
  hp.Init((float)rate); hp.SetMode(bellows::SvfMode::kHp); hp.Set(2500.0f, 0.7071f);
  double el = 0, eh = 0, et = 0;
  for (int i = 0; i < n; i++) {
    float l = lp.Process(m[i]), h = hp.Process(m[i]);
    el += (double)l * l; eh += (double)h * h; et += (double)m[i] * m[i];
  }
  if (et <= 0) { *lo = *mid = *hi = 0; return; }
  double em = et - el - eh; if (em < 0) em = 0;
  *lo = (float)(100.0 * el / et); *mid = (float)(100.0 * em / et); *hi = (float)(100.0 * eh / et);
}

/* a 16 bit PCM WAV with TPDF dither */
bool WriteWav(const char *path, const float *data, int frames, int channels, int rate, float gain) {
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  int n = frames * channels;
  uint32_t bytes = (uint32_t)n * 2;
  unsigned char h[44];
  memcpy(h, "RIFF", 4); uint32_t sz = 36 + bytes; memcpy(h + 4, &sz, 4); memcpy(h + 8, "WAVEfmt ", 8);
  uint32_t fmt = 16; memcpy(h + 16, &fmt, 4);
  uint16_t pcm = 1, ch = (uint16_t)channels, bits = 16, align = (uint16_t)(channels * 2);
  memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
  uint32_t r = (uint32_t)rate, br = (uint32_t)(rate * channels * 2);
  memcpy(h + 24, &r, 4); memcpy(h + 28, &br, 4); memcpy(h + 32, &align, 2); memcpy(h + 34, &bits, 2);
  memcpy(h + 36, "data", 4); memcpy(h + 40, &bytes, 4);
  fwrite(h, 1, 44, f);
  uint32_t seed = 0x9e3779b9u;
  int16_t *out = (int16_t *)malloc((size_t)n * 2);
  for (int i = 0; i < n; i++) {
    seed = seed * 1664525u + 1013904223u; float d1 = (seed >> 8) / 16777216.0f;
    seed = seed * 1664525u + 1013904223u; float d2 = (seed >> 8) / 16777216.0f;
    float v = data[i] * gain * 32767.0f + (d1 - d2);
    if (v > 32767.0f) v = 32767.0f; if (v < -32768.0f) v = -32768.0f;
    out[i] = (int16_t)lrintf(v);
  }
  fwrite(out, 2, (size_t)n, f);
  free(out);
  fclose(f);
  return true;
}

/* render a section of a mode: passes * bars of 16 steps, mono, keep the last pass */
Wav RenderStem(int mode, int section, int stem, int hold, int passes, int rate) {
  int spb = audio_song_step(mode, (uint32_t)rate);
  int bars = audio_song_bars(mode, section);
  int frames_section = bars * 16 * spb;
  /* without hold the form runs on through its sections: the whole form per pass */
  int frames_pass = frames_section;
  if (!hold) { frames_pass = 0; for (int s = 0; s < audio_song_sections(mode); s++) frames_pass += audio_song_bars(mode, s) * 16 * spb; }
  int total = frames_pass * passes;
  float *st = (float *)malloc((size_t)total * 2 * sizeof(float));
  audio_bake_begin(mode, section, stem, hold);
  audio_bake_render(st, total);
  Wav w; w.n = frames_pass; w.mono = (float *)malloc((size_t)frames_pass * sizeof(float));
  int from = total - frames_pass;
  for (int i = 0; i < frames_pass; i++) w.mono[i] = 0.5f * (st[(from + i) * 2] + st[(from + i) * 2 + 1]);
  free(st);
  if (passes == 1) {   /* a one shot: fade the tail so the cut never clicks */
    int fl = rate * 5 / 1000;
    for (int i = 0; i < fl && i < w.n; i++) w.mono[w.n - 1 - i] *= (float)i / (float)fl;
  }
  return w;
}

struct Job { const char *name; int mode, section, stem, hold, passes; int group; };

/* groups: 0 GARAGE, 1 OVERTIME, 2 ARENA; the ARENA base and riff are baked
 * from the STOMP section, which shares chords, bass and layers outside the
 * kit with BREAK (audio_band.cpp), so one base stem is true under both */
const Job JOBS[] = {
  {"garage.wav",      MUSIC_GARAGE,   0, STEM_ALL,   0, 2, 0},
  {"overtime.wav",    MUSIC_OVERTIME, 0, STEM_ALL,   0, 2, 1},
  {"arena_base.wav",  MUSIC_ARENA,    0, STEM_BASE,  1, 2, 2},
  {"arena_stomp.wav", MUSIC_ARENA,    0, STEM_DRUMS, 1, 2, 2},
  {"arena_break.wav", MUSIC_ARENA,    1, STEM_DRUMS, 1, 2, 2},
  {"arena_riff.wav",  MUSIC_ARENA,    0, STEM_LEAD,  1, 2, 2},
};
constexpr int kJobs = sizeof JOBS / sizeof JOBS[0];
constexpr long kBudgetBytes = 4400000;   /* section 13: the music WAVs, 4.4 MB */

/* the mode trims audio_pcm.c applies to the stems (its MODE_TRIM): the fight
 * music sits 6 to 10 dB under the hit sounds at the default levels */
const float kModeTrim[4] = {0.0f, 0.5f, 0.4f, 0.45f};
/* the fader law of audio_pcm.c and the band, and the default rows */
float LevelGain(int level) { float x = level / 10.0f; return x <= 0.0f ? 0.0f : powf(x, 0.7f); }
constexpr int kDefaultSfx = 8, kDefaultMusic = 6;

/* the group gain is measured on the full mix of every section of the song so
 * the stems sum to the same ceiling */
float GroupGain(int group, int rate, float ceiling) {
  int mode = group == 0 ? MUSIC_GARAGE : group == 1 ? MUSIC_OVERTIME : MUSIC_ARENA;
  float peak = 0;
  if (group == 2) {
    for (int s = 0; s < audio_song_sections(mode); s++) {
      Wav w = RenderStem(mode, s, STEM_ALL, 1, 2, rate);
      float p = TruePeak(w.mono, w.n);
      if (p > peak) peak = p;
      free(w.mono);
    }
  } else {
    Wav w = RenderStem(mode, 0, STEM_ALL, 0, 2, rate);
    peak = TruePeak(w.mono, w.n);
    free(w.mono);
  }
  return peak > 1e-6f ? ceiling / peak : 1.0f;
}

const char *const SFX_NAME[SFX_COUNT_] = {"MENU_MOVE", "MENU_OK", "MENU_BACK", "LOCK", "COUNT", "HORN",
                                          "HIT_LIGHT", "HIT_HEAVY", "CLASH", "LAUNCH", "KLAXON", "KO"};
const char *const VAR_NAME[4] = {"", " low", " tell", " short"};
const char *const MODE_NAME[4] = {"off", "garage", "arena", "overtime"};

/* one SFX alone at the given levels: its peak and 300 ms RMS at 48 kHz */
void SfxAlone(int sfx, int var, int sfx_level, float *peak, float *rms) {
  const int rate = 48000, n = rate;
  float *buf = (float *)calloc((size_t)n * 2, sizeof(float));
  audio_band_init(rate);
  audio_band_levels(sfx_level, kDefaultMusic);
  audio_play_var(sfx, var);
  audio_band_render(buf, n, rate);
  float pk = 0; double acc = 0; int meas = rate * 3 / 10;
  for (int k = 0; k < meas; k++) { float v = buf[k * 2]; float a = fabsf(v); if (a > pk) pk = a; acc += (double)v * v; }
  *peak = pk; *rms = (float)sqrt(acc / meas);
  free(buf);
}

/* the SFX preview: every patch and variant, the whines and a crowd swell, at 48 kHz */
void Preview(const char *dir) {
  const int rate = 48000;
  audio_band_init(rate);
  audio_band_levels(10, 10);
  const int slot = rate;              /* 1 s per sound */
  struct Item { int sfx, var; } items[32]; int ni = 0;
  for (int s = 0; s < SFX_COUNT_; s++) {
    items[ni++] = {s, SFXV_DEFAULT};
    if (s == SFX_MENU_OK || s == SFX_HORN || s == SFX_HIT_LIGHT || s == SFX_CLASH) items[ni++] = {s, SFXV_LOW};
    if (s == SFX_COUNT) items[ni++] = {s, SFXV_SHORT};
    if (s == SFX_KLAXON) { items[ni++] = {s, SFXV_TELL}; items[ni++] = {s, SFXV_SHORT}; items[ni++] = {s, SFXV_LOW}; }
  }
  const int parts = 6 + 8 + 11 + 7 + 4;
  int total = (ni + 2 + parts + 4) * slot;
  float *buf = (float *)calloc((size_t)total * 2, sizeof(float));
  printf("sfx preview (gain 10/10, 1 s per sound):\n");
  int at = 0;
  for (int i = 0; i < ni; i++, at += slot) {
    audio_play_var(items[i].sfx, items[i].var);
    audio_band_render(buf + (size_t)at * 2, slot, rate);
    float pk = 0; double acc = 0; int meas = rate * 3 / 10;
    for (int k = 0; k < meas; k++) { float v = buf[(at + k) * 2]; float a = fabsf(v); if (a > pk) pk = a; acc += (double)v * v; }
    printf("  %6.1f s  %-10s%-6s peak %6.1f dBFS  rms(300ms) %6.1f dBFS\n", at / (float)rate, SFX_NAME[items[i].sfx], VAR_NAME[items[i].var], Db(pk), Db((float)sqrt(acc / meas)));
  }
  /* the whines: four slots sweeping up at rising levels, then off */
  {
    int n = slot * 2;
    for (int k = 0; k < n; k += 256) {
      float t = k / (float)n;
      for (int w = 0; w < 4; w++) audio_whine(w, t < 0.85f ? 0.25f + 0.25f * w : 0.0f, t * (0.6f + 0.1f * w));
      int c = n - k < 256 ? n - k : 256;
      audio_band_render(buf + (size_t)(at + k) * 2, c, rate);
    }
    float pk = 0; for (int k = 0; k < n; k++) { float a = fabsf(buf[(at + k) * 2]); if (a > pk) pk = a; }
    printf("  %6.1f s  whines x4 sweep        peak %6.1f dBFS\n", at / (float)rate, Db(pk));
    at += n;
  }
  /* the parts: every row and option */
  static const int kRows[5] = {0, 1, 2, 3, 4};
  static const int kOpts[5] = {6, 8, 11, 7, 4};
  for (int r = 0; r < 5; r++) for (int o = 0; o < kOpts[r]; o++, at += slot) {
    audio_part(kRows[r], o);
    audio_band_render(buf + (size_t)at * 2, slot, rate);
  }
  printf("  %6.1f s  parts: 36 row/option sounds, 1 s each\n", (at - parts * slot) / (float)rate);
  /* the crowd: a bed, then a swell to 1.0 */
  {
    audio_band_crowd_bed(0.12f);
    int n = total - at;
    for (int k = 0; k < n; k += 256) {
      if (k == rate * 2) audio_crowd(1.0f);
      int c = n - k < 256 ? n - k : 256;
      audio_band_render(buf + (size_t)(at + k) * 2, c, rate);
    }
    float pk = 0; for (int k = 0; k < n; k++) { float a = fabsf(buf[(at + k) * 2]); if (a > pk) pk = a; }
    printf("  %6.1f s  crowd bed then a swell  peak %6.1f dBFS\n", at / (float)rate, Db(pk));
  }
  /* the cost: a dense second (a clash, a heavy hit and a KO over four whines
   * and the crowd) rendered in 800 frame slices, as the cart does at 60 Hz */
  {
    float slice[800 * 2];
    for (int w = 0; w < 4; w++) audio_whine(w, 0.6f, 0.5f + 0.1f * w);
    audio_crowd(1.0f);
    clock_t t0 = clock();
    for (int k = 0; k < 60; k++) {
      if (k == 0) audio_play(SFX_CLASH);
      if (k == 20) audio_play(SFX_HIT_HEAVY);
      if (k == 40) audio_play(SFX_KO);
      memset(slice, 0, sizeof slice);
      audio_band_render(slice, 800, rate);
    }
    double ms = (clock() - t0) * 1000.0 / CLOCKS_PER_SEC / 60.0;
    for (int w = 0; w < 4; w++) audio_whine(w, 0.0f, 0.5f);
    printf("band cost on this machine: %.3f ms per 800 frame render under a dense load\n", ms);
  }
  char path[1024];
  snprintf(path, sizeof path, "%s/sfx_preview.wav", dir);
  float pk, rms; Stats(buf, total * 2, &pk, &rms);
  WriteWav(path, buf, total, 2, rate, 1.0f);
  printf("wrote %s (%d s, peak %.1f dBFS)\n", path, total / rate, Db(pk));
  free(buf);
}

/* a 20 s preview of one song as the cart mixes it (the stem gain, the mode
 * trim, MUSIC 6) under the hit sounds at SFX 8, through the master chain.
 * The song is folded to mono as the stems are. Prints the music's own peak
 * and RMS at those gains and how far under the hits it sits. */
struct Cue { float at; int sfx, var; float crowd; };
void SongPreview(const char *dir, int mode, float group_gain) {
  const int rate = 48000, secs = 20, n = rate * secs;
  const float mg = group_gain * kModeTrim[mode] * LevelGain(kDefaultMusic);
  float *music = (float *)calloc((size_t)n * 2, sizeof(float));
  float *mix = (float *)calloc((size_t)n * 2, sizeof(float));
  /* the song, from the top of its form, rendered at 48 kHz */
  audio_band_init(rate);
  audio_bake_begin(mode, 0, STEM_ALL, 0);
  audio_bake_render(music, n);
  for (int i = 0; i < n; i++) { float m = 0.5f * (music[i * 2] + music[i * 2 + 1]) * mg; music[i * 2] = music[i * 2 + 1] = m; mix[i * 2] = mix[i * 2 + 1] = m; }
  float mpk, mrms; Stats(music, n * 2, &mpk, &mrms);
  /* the hits: the fight cues on both fight songs, the menu cues on the shelf */
  static const Cue kFight[] = {
    {1.5f, SFX_HIT_LIGHT, SFXV_DEFAULT, 0}, {3.2f, SFX_HIT_HEAVY, SFXV_DEFAULT, 0.6f}, {5.0f, SFX_HIT_LIGHT, SFXV_LOW, 0},
    {6.4f, SFX_CLASH, SFXV_DEFAULT, 0.9f}, {8.6f, SFX_HIT_LIGHT, SFXV_DEFAULT, 0}, {9.6f, SFX_HIT_HEAVY, SFXV_DEFAULT, 0.7f},
    {11.5f, SFX_KLAXON, SFXV_TELL, 0}, {13.0f, SFX_LAUNCH, SFXV_DEFAULT, 0}, {13.6f, SFX_HIT_LIGHT, SFXV_LOW, 0},
    {14.8f, SFX_HIT_HEAVY, SFXV_DEFAULT, 0.7f}, {16.3f, SFX_CLASH, SFXV_DEFAULT, 0.9f}, {18.3f, SFX_KO, SFXV_DEFAULT, 1.0f}};
  static const Cue kShelf[] = {
    {1.0f, SFX_MENU_MOVE, SFXV_DEFAULT, 0}, {1.4f, SFX_MENU_MOVE, SFXV_DEFAULT, 0}, {2.2f, SFX_MENU_OK, SFXV_DEFAULT, 0},
    {4.0f, SFX_MENU_MOVE, SFXV_DEFAULT, 0}, {5.5f, SFX_MENU_BACK, SFXV_DEFAULT, 0}, {7.0f, SFX_LOCK, SFXV_DEFAULT, 0},
    {9.0f, SFX_HIT_HEAVY, SFXV_DEFAULT, 0}, {11.0f, SFX_MENU_OK, SFXV_LOW, 0}, {13.0f, SFX_HIT_LIGHT, SFXV_DEFAULT, 0},
    {15.0f, SFX_LOCK, SFXV_DEFAULT, 0}, {17.0f, SFX_COUNT, SFXV_DEFAULT, 0}, {18.0f, SFX_COUNT, SFXV_DEFAULT, 0}, {19.0f, SFX_HORN, SFXV_DEFAULT, 0}};
  const Cue *cues = mode == MUSIC_GARAGE ? kShelf : kFight;
  const int ncue = mode == MUSIC_GARAGE ? (int)(sizeof kShelf / sizeof kShelf[0]) : (int)(sizeof kFight / sizeof kFight[0]);
  /* the band fresh at 48 kHz; audio_bake_begin left it in the song mode, so
   * put it back to silence and the cart's levels */
  audio_band_init(rate);
  audio_band_levels(kDefaultSfx, kDefaultMusic);
  audio_band_crowd_bed(mode == MUSIC_GARAGE ? 0.0f : 0.12f);
  if (mode != MUSIC_GARAGE) { audio_whine(0, 0.5f, 0.6f); audio_whine(1, 0.35f, 0.45f); }
  int next = 0;
  for (int at = 0; at < n; at += 800) {
    while (next < ncue && cues[next].at * rate <= at) {
      audio_play_var(cues[next].sfx, cues[next].var);
      if (cues[next].crowd > 0) audio_crowd(cues[next].crowd);
      next++;
    }
    int c = n - at < 800 ? n - at : 800;
    audio_bake_sfx_render(mix + (size_t)at * 2, c);
  }
  float pk, rms; Stats(mix, n * 2, &pk, &rms);
  float tp = 0;
  for (int c = 0; c < 2; c++) {   /* the true peak per channel */
    for (int i = 0; i < n; i++) music[i] = mix[i * 2 + c];
    float t = TruePeak(music, n); if (t > tp) tp = t;
  }
  char path[1024];
  snprintf(path, sizeof path, "%s/preview-%s.wav", dir, MODE_NAME[mode]);
  WriteWav(path, mix, n, 2, rate, 1.0f);
  printf("wrote %s (%d s, music alone peak %.1f rms %.1f dBFS, mix peak %.1f true %.1f rms %.1f dBFS)\n",
         path, secs, Db(mpk), Db(mrms), Db(pk), Db(tp), Db(rms));
  free(music); free(mix);
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: bake_music <assets/music dir> [preview dir]\n"); return 2; }
  const char *dir = argv[1];
  int rate = 22050;
  if (const char *e = getenv("MUSIC_RATE")) { int r = atoi(e); if (r >= 8000 && r <= 48000) rate = r; }
  const float ceiling = 0.841f;   /* -1.5 dBFS true peak, so the sample peak sits under -1 dBFS with room */
  audio_band_init((uint32_t)rate);
  float gain[3];
  for (int g = 0; g < 3; g++) gain[g] = GroupGain(g, rate, ceiling);
  printf("stems at %d Hz mono 16 bit; gains garage %.3f arena %.3f overtime %.3f\n", rate, gain[0], gain[2], gain[1]);
  printf("live trims for audio_band.cpp kSongTrim (gain times the mode trim): garage %.3f arena %.3f overtime %.3f\n",
         gain[0] * kModeTrim[MUSIC_GARAGE], gain[2] * kModeTrim[MUSIC_ARENA], gain[1] * kModeTrim[MUSIC_OVERTIME]);
  /* the balance table: every song's stems against its full mix, at the group gain */
  printf("balance (peak / rms dBFS at the group gain):\n");
  for (int mode = MUSIC_GARAGE; mode <= MUSIC_OVERTIME; mode++) {
    int group = mode == MUSIC_GARAGE ? 0 : mode == MUSIC_ARENA ? 2 : 1;
    for (int s = 0; s < audio_song_sections(mode); s++) {
      printf("  %-8s section %d:", MODE_NAME[mode], s);
      for (int stem = STEM_BASE; stem <= STEM_ALL; stem++) {
        Wav w = RenderStem(mode, s, stem, 1, 2, rate);
        float pk, rms; Stats(w.mono, w.n, &pk, &rms);
        printf("  %s %6.1f/%6.1f", stem == STEM_BASE ? "base" : stem == STEM_DRUMS ? "drums" : stem == STEM_LEAD ? "lead" : "all", Db(pk * gain[group]), Db(rms * gain[group]));
        free(w.mono);
      }
      printf("\n");
    }
  }
  long total_bytes = 0;
  printf("files (peak, true peak, rms in dBFS; low / mid / high energy percent):\n");
  for (int j = 0; j < kJobs; j++) {
    const Job &jb = JOBS[j];
    Wav w = RenderStem(jb.mode, jb.section, jb.stem, jb.hold, jb.passes, rate);
    float pk, rms; Stats(w.mono, w.n, &pk, &rms);
    float tp = TruePeak(w.mono, w.n);
    float lo, mid, hi; Balance(w.mono, w.n, rate, &lo, &mid, &hi);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, jb.name);
    if (!WriteWav(path, w.mono, w.n, 1, rate, gain[jb.group])) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    long bytes = 44 + (long)w.n * 2;
    total_bytes += bytes;
    printf("  %-18s %5.1f s  %7ld bytes  peak %6.1f  true %6.1f  rms %6.1f   %2.0f / %2.0f / %2.0f\n", jb.name, w.n / (float)rate, bytes,
           Db(pk * gain[jb.group]), Db(tp * gain[jb.group]), Db(rms * gain[jb.group]), lo, mid, hi);
    free(w.mono);
  }
  printf("total %ld bytes (%.2f MB, %.2f MiB) in %d files; the section 13 line is %.1f MB: %s\n", total_bytes, total_bytes / 1e6, total_bytes / 1048576.0, kJobs,
         kBudgetBytes / 1e6, total_bytes <= kBudgetBytes ? "within budget" : "OVER BUDGET");
  /* the fight music against the hits at the default rows (MUSIC 6, SFX 8):
   * the stems' ceiling times the mode trim times the fader, against each hit
   * alone through the band and the master chain */
  printf("music under the hits at MUSIC %d SFX %d (peak dBFS; the stems peak at the ceiling, the full mix with the lead in):\n", kDefaultMusic, kDefaultSfx);
  float music_peak[4] = {0};
  for (int mode = MUSIC_GARAGE; mode <= MUSIC_OVERTIME; mode++) {
    music_peak[mode] = ceiling * kModeTrim[mode] * LevelGain(kDefaultMusic);
    printf("  %-8s music peak %6.1f\n", MODE_NAME[mode], Db(music_peak[mode]));
  }
  static const int kHits[3] = {SFX_HIT_LIGHT, SFX_HIT_HEAVY, SFX_CLASH};
  for (int h = 0; h < 3; h++) {
    float pk, rms; SfxAlone(kHits[h], SFXV_DEFAULT, kDefaultSfx, &pk, &rms);
    printf("  %-10s peak %6.1f  rms(300ms) %6.1f  the arena music sits %4.1f dB under its peak\n", SFX_NAME[kHits[h]], Db(pk), Db(rms), Db(pk) - Db(music_peak[MUSIC_ARENA]));
  }
  { float pk, rms; SfxAlone(SFX_KO, SFXV_DEFAULT, kDefaultSfx, &pk, &rms); printf("  %-10s peak %6.1f  rms(300ms) %6.1f\n", SFX_NAME[SFX_KO], Db(pk), Db(rms)); }
  if (argc > 2) {
    for (int mode = MUSIC_GARAGE; mode <= MUSIC_OVERTIME; mode++) SongPreview(argv[2], mode, gain[mode == MUSIC_GARAGE ? 0 : mode == MUSIC_ARENA ? 2 : 1]);
    Preview(argv[2]);
  }
  return 0;
}
