/* bake_music: renders the three songs of spec 11.2 to assets/music as the
 * stems audio_pcm.c sequences, and every SFX to one preview WAV.
 *
 *   bake_music <assets/music dir> [preview dir]
 *   MUSIC_RATE=24000 (default) sets the stem rate; the preview is at 48 kHz
 *
 * Built natively by ./build.sh music with -DMUSIC_BAKE against audio_band.cpp,
 * so the songs the cart would play live (MUSIC=live) and the WAVs are one
 * source. Stems are 16 bit mono at 24 kHz:
 *
 *   garage.wav              GARAGE, 16 bars, the loop
 *   overtime.wav            OVERTIME, 8 bars, the loop
 *   arena_intro_base.wav    ARENA INTRO, everything but drums and lead
 *   arena_intro_drums.wav   ARENA INTRO, drums
 *   arena_base.wav          ARENA BUILD/DROP body, 8 bar loop, base
 *   arena_drums.wav         the same, drums
 *   arena_lead.wav          ARENA DROP, 8 bar loop, lead
 *   arena_lead2.wav         ARENA DROP2, 8 bar loop, lead and counter
 *
 * A loop file is the second pass of its section so the tails of the end
 * are already under the start and the join is seamless; a one shot file
 * (the intro pair) is the first pass with a 5 ms fade on its tail. Every
 * ARENA file carries the same gain, measured on the full mix, so the stems
 * sum in the mixer to a peak of -1 dBFS and never clip. The tool prints
 * peak and RMS per file and per SFX. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

const Job JOBS[] = {
  {"garage.wav",            MUSIC_GARAGE,   0, STEM_ALL,   0, 2, 0},
  {"overtime.wav",          MUSIC_OVERTIME, 0, STEM_ALL,   0, 2, 1},
  {"arena_intro_base.wav",  MUSIC_ARENA,    0, STEM_BASE,  1, 1, 2},
  {"arena_intro_drums.wav", MUSIC_ARENA,    0, STEM_DRUMS, 1, 1, 2},
  {"arena_base.wav",        MUSIC_ARENA,    1, STEM_BASE,  1, 2, 2},
  {"arena_drums.wav",       MUSIC_ARENA,    1, STEM_DRUMS, 1, 2, 2},
  {"arena_lead.wav",        MUSIC_ARENA,    2, STEM_LEAD,  1, 2, 2},
  {"arena_lead2.wav",       MUSIC_ARENA,    3, STEM_LEAD,  1, 2, 2},
};
constexpr int kJobs = sizeof JOBS / sizeof JOBS[0];

/* the ARENA gain is measured on the full mix of every section so the stems
 * sum to the same ceiling; GARAGE and OVERTIME are measured on themselves */
float GroupGain(int group, int rate, float ceiling) {
  float peak = 0;
  if (group == 2) {
    for (int s = 0; s < 4; s++) {
      Wav w = RenderStem(MUSIC_ARENA, s, STEM_ALL, 1, s == 0 ? 1 : 2, rate);
      float p, r; Stats(w.mono, w.n, &p, &r);
      if (p > peak) peak = p;
      free(w.mono);
    }
  } else {
    Wav w = RenderStem(group == 0 ? MUSIC_GARAGE : MUSIC_OVERTIME, 0, STEM_ALL, 0, 2, rate);
    float p, r; Stats(w.mono, w.n, &p, &r);
    peak = p;
    free(w.mono);
  }
  return peak > 1e-6f ? ceiling / peak : 1.0f;
}

const char *const SFX_NAME[SFX_COUNT_] = {"MENU_MOVE", "MENU_OK", "MENU_BACK", "LOCK", "COUNT", "HORN",
                                          "HIT_LIGHT", "HIT_HEAVY", "CLASH", "LAUNCH", "KLAXON", "KO"};
const char *const VAR_NAME[4] = {"", " low", " tell", " short"};

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

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: bake_music <assets/music dir> [preview dir]\n"); return 2; }
  const char *dir = argv[1];
  int rate = 24000;
  if (const char *e = getenv("MUSIC_RATE")) { int r = atoi(e); if (r >= 8000 && r <= 48000) rate = r; }
  const float ceiling = 0.891f;   /* -1 dBFS */
  audio_band_init((uint32_t)rate);
  float gain[3];
  for (int g = 0; g < 3; g++) gain[g] = GroupGain(g, rate, ceiling);
  gain[0] *= 0.5f;   /* the garage sits 6 dB under the fight: it is a menu */
  printf("stems at %d Hz mono 16 bit; gains garage %.3f arena %.3f overtime %.3f\n", rate, gain[0], gain[2], gain[1]);
  /* the balance table: every song's stems against its full mix, at the group gain */
  printf("balance (peak / rms dBFS at the group gain):\n");
  for (int mode = MUSIC_GARAGE; mode <= MUSIC_OVERTIME; mode++) {
    int group = mode == MUSIC_GARAGE ? 0 : mode == MUSIC_ARENA ? 2 : 1;
    for (int s = 0; s < audio_song_sections(mode); s++) {
      printf("  mode %d section %d:", mode, s);
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
  for (int j = 0; j < kJobs; j++) {
    const Job &jb = JOBS[j];
    Wav w = RenderStem(jb.mode, jb.section, jb.stem, jb.hold, jb.passes, rate);
    float pk, rms; Stats(w.mono, w.n, &pk, &rms);
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, jb.name);
    if (!WriteWav(path, w.mono, w.n, 1, rate, gain[jb.group])) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    long bytes = 44 + (long)w.n * 2;
    total_bytes += bytes;
    printf("  %-24s %6.1f s  %7ld bytes  peak %6.1f dBFS  rms %6.1f dBFS\n", jb.name, w.n / (float)rate, bytes, Db(pk * gain[jb.group]), Db(rms * gain[jb.group]));
    free(w.mono);
  }
  printf("total %.2f MB in %d files\n", total_bytes / 1048576.0, kJobs);
  if (argc > 2) Preview(argv[2]);
  return 0;
}
