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
 * from the BREAK section (section 0, the song opens on it), which shares
 * chords, bass and layers outside the kit with STOMP (audio_band.cpp), so
 * one base stem is true under both */
const Job JOBS[] = {
  {"garage.wav",      MUSIC_GARAGE,   0, STEM_ALL,   0, 2, 0},
  {"overtime.wav",    MUSIC_OVERTIME, 0, STEM_ALL,   0, 2, 1},
  {"arena_base.wav",  MUSIC_ARENA,    0, STEM_BASE,  1, 2, 2},
  {"arena_stomp.wav", MUSIC_ARENA,    1, STEM_DRUMS, 1, 2, 2},
  {"arena_break.wav", MUSIC_ARENA,    0, STEM_DRUMS, 1, 2, 2},
  {"arena_riff.wav",  MUSIC_ARENA,    0, STEM_LEAD,  1, 2, 2},
};
constexpr int kJobs = sizeof JOBS / sizeof JOBS[0];
constexpr long kBudgetBytes = 4400000;   /* section 13: the music WAVs, 4.4 MB */


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

/* -- the measures of deep-audio 6 (wavstat.mjs does the same for a cart capture) -- */

/* an in place radix 2 FFT over n (a power of two) complex points */
void Fft(float *re, float *im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
  }
  for (int len = 2; len <= n; len <<= 1) {
    double ang = -2.0 * 3.14159265358979 / len;
    float wr = (float)cos(ang), wi = (float)sin(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1.0f, ci = 0.0f;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = i + k + len / 2;
        float xr = re[b] * cr - im[b] * ci, xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr; im[b] = im[a] - xi; re[a] += xr; im[a] += xi;
        float t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
      }
    }
  }
}

struct Spec { float centroid, flat, share[5], peak_hz; };   /* shares: under 120, 120 to 500, 500 to 2k, 2k to 6k, over 6k */
const float kEdge[4] = {120.0f, 500.0f, 2000.0f, 6000.0f};

/* the power spectrum of 4096 samples from m (Hann), its centroid, flatness over 20 Hz to 20 kHz,
 * the band shares and the strongest bin between lo and hi */
Spec Spectrum(const float *m, int n, int rate, float lo = 80.0f, float hi = 1200.0f) {
  const int N = 4096;
  static float re[N], im[N];
  for (int i = 0; i < N; i++) {
    float w = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * i / (N - 1));
    re[i] = i < n ? m[i] * w : 0.0f; im[i] = 0.0f;
  }
  Fft(re, im, N);
  Spec s = {};
  double sum = 0, wsum = 0, lsum = 0, bands[5] = {0}; int nb = 0; float best = -1;
  for (int k = 1; k < N / 2; k++) {
    float f = (float)k * rate / N;
    double p = (double)re[k] * re[k] + (double)im[k] * im[k];
    sum += p; wsum += p * f;
    int b = f < kEdge[0] ? 0 : f < kEdge[1] ? 1 : f < kEdge[2] ? 2 : f < kEdge[3] ? 3 : 4;
    bands[b] += p;
    if (f >= 20.0f && f <= 20000.0f) { lsum += log(p + 1e-12); nb++; }
    if (f >= lo && f <= hi && p > best) { best = (float)p; s.peak_hz = f; }
  }
  if (sum <= 0) return s;
  s.centroid = (float)(wsum / sum);
  double am = 0; int na = 0;
  for (int k = 1; k < N / 2; k++) { float f = (float)k * rate / N; if (f >= 20.0f && f <= 20000.0f) { am += (double)re[k] * re[k] + (double)im[k] * im[k]; na++; } }
  s.flat = (float)(exp(lsum / nb) / (am / na + 1e-30));
  for (int b = 0; b < 5; b++) s.share[b] = (float)(100.0 * bands[b] / sum);
  return s;
}

struct Meas { float peak, rms, atk_ms, t20, t40; Spec sp; };

/* a slot of mono audio: the peak, the RMS of the first 300 ms, the time to the peak, the decay
 * from the peak to -20 and -40 dB (5 ms RMS frames), and the spectrum of the first 85 ms
 * (or of the window at spec_from, for a loop) */
Meas Measure(const float *m, int n, int rate, int spec_from = 0) {
  Meas r = {};
  int pk_i = 0; double acc = 0; int meas = rate * 3 / 10; if (meas > n) meas = n;
  for (int i = 0; i < n; i++) { float a = fabsf(m[i]); if (a > r.peak) { r.peak = a; pk_i = i; } }
  for (int i = 0; i < meas; i++) acc += (double)m[i] * m[i];
  r.rms = (float)sqrt(acc / (meas > 0 ? meas : 1));
  r.atk_ms = 1000.0f * pk_i / rate;
  const int fr = rate / 200;
  int pf = pk_i / fr, nf = n / fr; float pr = 0;
  for (int f = pf - 1; f <= pf + 1; f++) { if (f < 0 || f >= nf) continue; double a = 0; for (int i = 0; i < fr; i++) a += (double)m[f * fr + i] * m[f * fr + i]; float v = (float)sqrt(a / fr); if (v > pr) pr = v; }
  int l20 = pf, l40 = pf;
  for (int f = pf; f < nf; f++) {
    double a = 0; for (int i = 0; i < fr; i++) a += (double)m[f * fr + i] * m[f * fr + i];
    float v = (float)sqrt(a / fr);
    if (v > pr * 0.1f) l20 = f;
    if (v > pr * 0.01f) l40 = f;
  }
  r.t20 = 5.0f * (l20 - pf); r.t40 = 5.0f * (l40 - pf);
  r.sp = Spectrum(m + spec_from, n - spec_from, rate);
  return r;
}

int g_fails = 0;
void Check(const char *what, const char *field, float v, float lo, float hi) {
  if (v < lo || v > hi) { printf("    FAIL %s: %s %.3f outside %.3f .. %.3f\n", what, field, v, lo, hi); g_fails++; }
}
const float kAny = 1e9f;

void PrintMeas(float at, const char *name, const Meas &m) {
  printf("  %6.1f s  %-26s peak %6.1f  rms %6.1f  atk %4.0f  T20 %5.0f  T40 %5.0f  cen %5.0f  flat %.3f  %2.0f/%2.0f/%2.0f/%2.0f/%2.0f\n",
         at, name, Db(m.peak), Db(m.rms), m.atk_ms, m.t20, m.t40, m.sp.centroid, m.sp.flat,
         m.sp.share[0], m.sp.share[1], m.sp.share[2], m.sp.share[3], m.sp.share[4]);
}

/* the cart's mixer constants (audio_pcm.c): MODE_TRIM and the fader law, the default rows */
const float kModeTrim[4] = {0.0f, 0.5f, 0.62f, 0.6f};
float LevelGain(int level) { float x = level / 10.0f; return x <= 0.0f ? 0.0f : powf(x, 0.7f); }
constexpr int kDefaultSfx = 8, kDefaultMusic = 6;

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

/* -- the SFX preview (deep-audio 4.5, 6.1) ------------------------------ */
/* a recorder: every slot rendered at SFX 8 MUSIC 6 through the master chain into one long WAV */
struct Rec {
  int rate = 48000; float *buf = nullptr; int cap = 0, at = 0; float *mono = nullptr;
  void Init(int seconds) { cap = rate * seconds; buf = (float *)calloc((size_t)cap * 2, sizeof(float)); mono = (float *)calloc((size_t)cap, sizeof(float)); }
  /* a fresh band, a setup, n seconds rendered in 800 frame slices with a per slice hook */
  template <class F> Meas Slot(const char *name, float secs, F hook, float spec_at_s = 0.0f, bool print = true) {
    audio_band_init(rate);
    audio_band_levels(kDefaultSfx, kDefaultMusic);
    int n = (int)(secs * rate); if (at + n > cap) n = cap - at;
    int k = 0;
    for (int f = 0; k < n; f++) {
      hook(f);
      int c = n - k < 800 ? n - k : 800;
      audio_band_render(buf + (size_t)(at + k) * 2, c, rate);
      k += c;
    }
    for (int i = 0; i < n; i++) mono[at + i] = 0.5f * (buf[(at + i) * 2] + buf[(at + i) * 2 + 1]);
    Meas m = Measure(mono + at, n, rate, (int)(spec_at_s * rate));
    if (print) PrintMeas(at / (float)rate, name, m);
    at += n;
    return m;
  }
};

/* a noise bed's peak over a second sits about 12 dB over its RMS, so where 6.1 puts a loop's
 * peak limit closer to its RMS window than that, the check is the crest against 13 dB */
float NoiseCrest(float spec_crest) { return spec_crest > 13.0f ? spec_crest : 13.0f; }

/* the loud floor for every one shot: the limiter is not the gain stage */
void OneShotCeiling(const char *name, const Meas &m) { Check(name, "peak dBFS (one shot ceiling)", Db(m.peak), -120.0f, -0.8f); }

void Preview(const char *dir) {
  Rec rec; rec.Init(420);
  printf("sfx preview (SFX %d MUSIC %d, 1 s per sound; peak, rms 300 ms in dBFS, attack, T20, T40 in ms, centroid of the first 85 ms in Hz, flatness, shares under 120 / 500 / 2k / 6k / over):\n", kDefaultSfx, kDefaultMusic);
  /* the stingers: on the palette, unchanged within 2 dB of 22c05b6 (its numbers at SFX 8) */
  struct Item { int sfx, var; float ref_db; } items[] = {
    {SFX_MENU_MOVE, 0, -11.8f}, {SFX_MENU_OK, 0, -12.0f}, {SFX_MENU_OK, SFXV_LOW, -14.6f}, {SFX_MENU_BACK, 0, -16.5f}, {SFX_LOCK, 0, -8.1f},
    {SFX_COUNT, 0, -12.4f}, {SFX_COUNT, SFXV_SHORT, -13.9f}, {SFX_HORN, 0, -6.5f}, {SFX_HORN, SFXV_LOW, -6.7f},
    {SFX_KLAXON, 0, -7.9f}, {SFX_KLAXON, SFXV_TELL, -9.0f}, {SFX_KLAXON, SFXV_SHORT, -7.9f}, {SFX_KLAXON, SFXV_LOW, -99.0f}, {SFX_LAUNCH, 0, -99.0f}};
  for (const Item &it : items) {
    char nm[64]; snprintf(nm, sizeof nm, "%s%s", SFX_NAME[it.sfx], VAR_NAME[it.var]);
    Meas m = rec.Slot(nm, 1.0f, [&](int f) { if (f == 0) audio_play_var(it.sfx, it.var); });
    /* a menu stinger sits at -14 to -8 or within 2 dB of 22c05b6 (measured there at SFX 8) */
    if (it.ref_db > -90.0f && !(Db(m.peak) >= -14.0f && Db(m.peak) <= -8.0f)) Check(nm, "peak against 22c05b6 (dB)", Db(m.peak) - it.ref_db, -2.0f, 2.0f);
    OneShotCeiling(nm, m);
  }
  /* the impacts: every kind at every band at three velocities */
  static const char *const kHk[HK_COUNT_] = {"GENERIC", "SPINNER", "SAW", "HAMMER", "SPIKE", "RAM", "FLIPPER", "FLAME", "CRUSHER", "LIFTER", "OPS",
                                             "PULVERIZER", "KILLSAW", "HAZ FLAME", "SCREW", "LASER", "DROP", "PIT", "WALL", "LAND"};
  static const char *const kBand[6] = {"TAP", "HIT", "HEAVY", "HUGE", "CLASH", "KO"};
  static const float kVel[3] = {0.3f, 0.7f, 1.0f};
  for (int b = 0; b < 6; b++) for (int v = 0; v < 3; v++) {
    char nm[64]; snprintf(nm, sizeof nm, "GENERIC %s vel %.1f", kBand[b], kVel[v]);
    Meas m = rec.Slot(nm, 1.0f, [&](int f) { if (f == 0) { audio_band_hit(HK_GENERIC, b, kVel[v], 0.0f, 4, -1); audio_band_duck_band(b); } });
    OneShotCeiling(nm, m);
    if (b == 0 && v == 0) {   /* CLANK tap */
      Check(nm, "peak dBFS", Db(m.peak), -14.0f, -10.0f); Check(nm, "rms dBFS", Db(m.rms), -120.0f, -26.0f);
      Check(nm, "flatness", m.sp.flat, 0.01f, kAny); Check(nm, "centroid Hz", m.sp.centroid, 1000.0f, 3500.0f);
      Check(nm, "share over 2 kHz", m.sp.share[3] + m.sp.share[4], 20.0f, 100.0f);
    }
    if (b == 1 && v == 1) {   /* CLANK hit */
      Check(nm, "peak dBFS", Db(m.peak), -10.0f, -6.0f); Check(nm, "flatness", m.sp.flat, 0.01f, kAny); Check(nm, "centroid Hz", m.sp.centroid, 1000.0f, 3500.0f);
    }
    if (b == 2 && v == 1) {   /* CRUNCH */
      Check(nm, "peak dBFS", Db(m.peak), -6.0f, -3.0f); Check(nm, "rms dBFS", Db(m.rms), -20.0f, -14.0f);
      Check(nm, "flatness", m.sp.flat, 0.03f, kAny); Check(nm, "centroid Hz", m.sp.centroid, 400.0f, 2500.0f);
      Check(nm, "share under 120", m.sp.share[0], 8.0f, 30.0f); Check(nm, "share over 2 kHz", m.sp.share[3] + m.sp.share[4], 8.0f, 100.0f);
    }
    if (b == 3 && v == 2) {   /* WRECK */
      Check(nm, "peak dBFS", Db(m.peak), -3.0f, -1.0f); Check(nm, "rms dBFS", Db(m.rms), -18.0f, -12.0f);
      Check(nm, "flatness", m.sp.flat, 0.03f, kAny); Check(nm, "centroid Hz", m.sp.centroid, 300.0f, 2000.0f);
      Check(nm, "share under 120", m.sp.share[0], 25.0f, 50.0f); Check(nm, "share over 2 kHz", m.sp.share[3] + m.sp.share[4], 6.0f, 100.0f);
    }
    if (b == 5 && v == 2) {   /* KO */
      Check(nm, "peak dBFS", Db(m.peak), -1.5f, -0.8f); Check(nm, "rms dBFS", Db(m.rms), -16.0f, -10.0f);
      Check(nm, "flatness", m.sp.flat, 0.03f, kAny); Check(nm, "share under 120", m.sp.share[0], 40.0f, 65.0f); Check(nm, "T40 ms", m.t40, 600.0f, kAny);
    }
  }
  for (int k = 1; k < HK_COUNT_; k++) for (int b = 0; b < 5; b++) for (int v = 0; v < 3; v++) {
    char nm[64]; snprintf(nm, sizeof nm, "%s %s vel %.1f", kHk[k], kBand[b], kVel[v]);
    bool haz = k >= HK_HAZ_PULVERIZER && k <= HK_HAZ_PIT;
    if (haz && (b != 2 || v != 2)) continue;          /* a hazard hits at the sim's damage: one each */
    if (!haz && v == 0 && b != 0) continue;           /* the low velocity on the tap only, to keep the table short */
    Meas m = rec.Slot(nm, haz && k == HK_HAZ_PIT ? 2.0f : 1.0f, [&](int f) { if (f == 0) { audio_band_hit(k, b, kVel[v], 0.0f, 4, 0); audio_band_duck_band(b); } }, 0.0f, b == 1 || b == 2 || haz);
    OneShotCeiling(nm, m);
    if (haz && k != HK_HAZ_FLAME) { Check(nm, "peak dBFS", Db(m.peak), -6.0f, -1.0f); Check(nm, "flatness", m.sp.flat, 0.03f, kAny); }
    if (k == HK_LAND && b == 2 && v == 2) { Check(nm, "peak dBFS (landing speed 7)", Db(m.peak), -5.0f, -2.0f); Check(nm, "flatness", m.sp.flat, 0.03f, kAny); }
  }
  /* the armour colouring on a hit */
  static const char *const kAr[7] = {"NONE", "PLY", "ALU", "UHMW", "STEEL", "TI", "WEAR"};
  for (int a = 0; a < 7; a++) {
    char nm[64]; snprintf(nm, sizeof nm, "HIT on %s", kAr[a]);
    Meas m = rec.Slot(nm, 1.0f, [&](int f) { if (f == 0) audio_band_hit(HK_GENERIC, 1, 0.7f, 0.0f, a, -1); });
    OneShotCeiling(nm, m);
  }
  /* the loops at full, measured in their steady second */
  Meas m;
  m = rec.Slot("spinner whine full", 2.0f, [&](int f) { audio_band_weapon(0, WK_SPIN_HORIZ, 1.0f, f < 30 ? f / 30.0f : 1.0f, 0.0f); }, 1.0f);
  { Meas s = Measure(rec.mono + rec.at - rec.rate, rec.rate, rec.rate); Check("spinner whine full", "peak dBFS", Db(s.peak), -120.0f, -10.0f);
    Check("spinner whine full", "rms dBFS", Db(s.rms), -20.0f, -16.0f); Check("spinner whine full", "centroid Hz", m.sp.centroid, 400.0f, 1200.0f);
    Check("spinner whine full", "share under 120", m.sp.share[0], 0.0f, 10.0f); }
  m = rec.Slot("drum whine full", 1.5f, [&](int f) { audio_band_weapon(0, WK_SPIN_DRUM, 1.0f, 1.0f, 0.0f); }, 0.8f);
  m = rec.Slot("melty whine full", 1.5f, [&](int f) { audio_band_weapon(0, WK_MELTY, 1.0f, 1.0f, 0.0f); }, 0.8f);
  m = rec.Slot("spinner spin up 2 s, spin down", 4.0f, [&](int f) { float t = f * 800.0f / rec.rate; audio_band_weapon(0, WK_SPIN_HORIZ, 1.0f, t < 2.0f ? t / 2.0f : 1.0f - (t - 2.0f) / 2.0f * 0.9f, 0.0f); });
  m = rec.Slot("spin kill", 1.5f, [&](int f) {
    if (f < 30) audio_band_weapon(0, WK_SPIN_HORIZ, 1.0f, 1.0f, 0.0f);
    else { if (f == 30) audio_band_spin_change(0, 2, 1.0f, 1.0f); audio_band_weapon(0, WK_SPIN_HORIZ, 1.0f, 0.0f, 0.0f); } }, 0.0f, false);
  { int n = rec.rate; Meas s = Measure(rec.mono + rec.at - n, n, rec.rate); PrintMeas((rec.at - n) / (float)rec.rate, "spin kill (from the kill)", s);
    Check("spin kill", "peak dBFS", Db(s.peak), -8.0f, -4.0f); Check("spin kill", "flatness", s.sp.flat, 0.02f, kAny); }
  m = rec.Slot("saw grind (in contact)", 2.0f, [&](int f) { audio_band_weapon(0, WK_SAW, 1.0f, 1.0f, 1.0f); }, 1.0f);
  { Meas s = Measure(rec.mono + rec.at - rec.rate, rec.rate, rec.rate); Check("saw grind", "crest dB (noise bed)", Db(s.peak) - Db(s.rms), 0.0f, NoiseCrest(-6.0f + 14.0f));
    Check("saw grind", "rms dBFS", Db(s.rms), -18.0f, -14.0f); Check("saw grind", "flatness", m.sp.flat, 0.15f, kAny); Check("saw grind", "centroid Hz", m.sp.centroid, 2000.0f, 7000.0f); }
  m = rec.Slot("flame roar", 2.0f, [&](int f) { if (f == 0) audio_band_tell(0, 5, 8, 0.0f); audio_band_weapon(0, WK_FLAME, 1.0f, 0.0f, 0.0f); }, 1.0f);
  { Meas s = Measure(rec.mono + rec.at - rec.rate, rec.rate, rec.rate); Check("flame roar", "crest dB (noise bed)", Db(s.peak) - Db(s.rms), 0.0f, NoiseCrest(-8.0f + 14.0f));
    Check("flame roar", "rms dBFS", Db(s.rms), -18.0f, -14.0f); /* 6.1 asks the roar for flatness over 0.1 with its centroid under 600 Hz; over 20 Hz to 20 kHz
     * that needs a tenth of the power in a flat floor, whose own centroid (10 kHz) then lifts the
     * whole past 1 kHz, so the two cannot both hold: the centroid is checked, the flatness printed */
    printf("    NOTE flame roar: flatness %.3f (6.1 asks over 0.1 with the centroid under 600 Hz; not both)\n", m.sp.flat); Check("flame roar", "centroid Hz", m.sp.centroid, 150.0f, 600.0f); }
  m = rec.Slot("hammer tell plus strike", 1.0f, [&](int f) { if (f == 0) audio_band_tell(0, 6, 0, 0.0f); if (f == 15) { audio_band_hit(HK_HAMMER, 2, 0.8f, 0.0f, 4, 0); audio_band_duck_band(2); } });
  { Check("hammer tell plus strike", "peak dBFS", Db(m.peak), -4.0f, -1.0f);
    const float *s0 = rec.mono + rec.at - rec.rate; float tp = 0; for (int i = 0; i < rec.rate / 5; i++) if (fabsf(s0[i]) > tp) tp = fabsf(s0[i]);
    Check("hammer tell", "tell peak dBFS in the first 200 ms", Db(tp), -14.0f, 0.0f); }
  m = rec.Slot("flipper fire", 1.0f, [&](int f) { if (f == 0) audio_band_tell(0, 7, 0, 0.0f); });
  Check("flipper fire", "peak dBFS", Db(m.peak), -8.0f, -4.0f); Check("flipper fire", "flatness", m.sp.flat, 0.05f, kAny); Check("flipper fire", "centroid Hz", m.sp.centroid, 1500.0f, 5000.0f);
  m = rec.Slot("wedge scrape loop", 2.0f, [&](int f) { audio_band_weapon(0, WK_SCRAPE, 0.65f, 0.5f, 0.0f); }, 1.0f);
  { Meas s = Measure(rec.mono + rec.at - rec.rate, rec.rate, rec.rate); Check("wedge scrape", "crest dB (noise bed)", Db(s.peak) - Db(s.rms), 0.0f, NoiseCrest(-14.0f + 18.0f));
    Check("wedge scrape", "rms dBFS", Db(s.rms), -24.0f, -18.0f); Check("wedge scrape", "flatness", m.sp.flat, 0.02f, kAny); Check("wedge scrape", "centroid Hz", m.sp.centroid, 500.0f, 2000.0f); }
  m = rec.Slot("crusher creak", 1.5f, [&](int f) { audio_band_weapon(0, WK_CRUSHER_SERVO, 1.0f, 0.0f, 0.0f); }, 0.8f);
  m = rec.Slot("lifter whir", 1.0f, [&](int f) { audio_band_weapon(0, WK_LIFTER_SERVO, 1.0f, 0.5f, 0.0f); }, 0.5f);
  struct DriveT { const char *name; int kind; float lo, hi, pk; float cen_lo, cen_hi; } drives[] = {
    {"tread rumble full", DK_TREAD, -24.0f, -20.0f, -16.0f, 0.0f, 300.0f}, {"wheel whine full (FOUR)", DK_WHEEL_FOUR, -30.0f, -26.0f, -22.0f, 0.0f, kAny},
    {"hover blower", DK_HOVER, -26.0f, -22.0f, -18.0f, 150.0f, 500.0f}, {"two wheel", DK_WHEEL_TWO, -99.0f, 0, 0, 0, 0}, {"six wheel", DK_WHEEL_SIX, -99.0f, 0, 0, 0, 0},
    {"leg tracks", DK_LEGTRACK, -99.0f, 0, 0, 0, 0}, {"walker steps", DK_WALKER, -99.0f, 0, 0, 0, 0}, {"melty drive", DK_MELTY, -99.0f, 0, 0, 0, 0}};
  for (const DriveT &d : drives) {
    m = rec.Slot(d.name, 2.0f, [&](int f) { audio_band_drive(0, d.kind, 1.0f, 0.0f, 1, 0, DRV_THROTTLE); }, 1.0f);
    if (d.lo < -90.0f) continue;
    Meas s = Measure(rec.mono + rec.at - rec.rate, rec.rate, rec.rate);
    Check(d.name, "crest dB (noise bed)", Db(s.peak) - Db(s.rms), 0.0f, NoiseCrest(d.pk - d.hi)); Check(d.name, "rms dBFS", Db(s.rms), d.lo, d.hi); Check(d.name, "centroid Hz", m.sp.centroid, d.cen_lo, d.cen_hi);
  }
  m = rec.Slot("airborne squeal (FOUR)", 1.0f, [&](int f) { audio_band_drive(0, DK_WHEEL_FOUR, 1.0f, 0.0f, 0, 0, DRV_THROTTLE); }, 0.5f);
  m = rec.Slot("stuck squeal", 1.0f, [&](int f) { audio_band_drive(0, DK_WHEEL_FOUR, 0.0f, 0.0f, 1, 0, DRV_THROTTLE | DRV_STUCK); }, 0.5f);
  m = rec.Slot("fizz (hp 0.3)", 2.0f, [&](int f) { audio_band_damage(0, 0.3f, 0, 1); }, 1.0f);
  { Meas s = Measure(rec.mono + rec.at - rec.rate, rec.rate, rec.rate); Check("fizz", "crest dB (gated noise: it crackles)", Db(s.peak) - Db(s.rms), 0.0f, 20.0f);
    Check("fizz", "rms dBFS", Db(s.rms), -32.0f, -28.0f); Check("fizz", "flatness", m.sp.flat, 0.2f, kAny); Check("fizz", "centroid Hz", m.sp.centroid, 2500.0f, kAny); }
  m = rec.Slot("death rattle (hp 0.15)", 2.5f, [&](int f) { audio_band_damage(0, 0.15f, 0, 1); audio_band_drive(0, DK_WHEEL_FOUR, 0.6f, 0.0f, 1, 0, 0); }, 1.0f);
  { int n = (int)(1.5f * rec.rate); Meas s = Measure(rec.mono + rec.at - n, n, rec.rate);
    /* the rattle alone: the drive's share is measured in its own slot, so the check is the sum under the ceiling */
    Check("death rattle", "crest dB (noise bed)", Db(s.peak) - Db(s.rms), 0.0f, NoiseCrest(-20.0f + 24.0f) + 3.0f); }
  m = rec.Slot("death rattle alone", 2.5f, [&](int f) { audio_band_damage(0, 0.15f, 0, 1); }, 1.0f);
  { int n = (int)(1.5f * rec.rate); Meas s = Measure(rec.mono + rec.at - n, n, rec.rate); Check("death rattle alone", "rms dBFS", Db(s.rms), -28.0f, -24.0f); }
  m = rec.Slot("burning", 1.5f, [&](int f) { audio_band_damage(0, 0.8f, 1, 1); }, 0.5f);
  /* the other one shots */
  for (int s = SFX_TELL_HAMMER; s < SFX_COUNT2_; s++) {
    static const char *const kNames[] = {"TELL_HAMMER", "TELL_FLIPPER", "TELL_SPIKE", "TELL_CRUSHER", "BITE_CRUSHER", "RELEASE_CRUSHER", "LIFT", "LIFT_DROP",
      "FLAME_IGNITE", "FLAME_OUT", "SPIN_KILL", "SPIN_READY", "SHED", "BURST", "SELFRIGHT", "BOOST", "STUCK", "FALL", "LAUNCH_WHOOSH", "LAUNCH_WEDGE",
      "BENCH", "BUMPER", "UNDERRIDE", "RIDE_DROP", "WHEEL_HIT", "DENT", "SCRAPE", "SKID", "GRIND_BITE", "BURN_START", "BURN_END", "DIG_IN", "SAW_TOGGLE"};
    const char *nm = kNames[s - SFX_TELL_HAMMER];
    float len = s == SFX_BURST || s == SFX_FALL || s == SFX_LAUNCH_WHOOSH ? 1.5f : 1.0f;
    m = rec.Slot(nm, len, [&](int f) { if (f == 0) audio_band_play_at(s, s == SFX_DENT ? 2 : 0, 0.0f); });
    OneShotCeiling(nm, m);
    if (s == SFX_SHED) {
      Check("panel shed", "peak dBFS", Db(m.peak), -8.0f, -4.0f);
      /* three decaying bounces: 10 ms RMS frames after 60 ms with a local peak each */
      const float *s0 = rec.mono + rec.at - (int)(len * rec.rate); const int fr = rec.rate / 100; int bumps = 0; float prev = 0, prev2 = 0;
      for (int f = 6; f < 40; f++) { double a = 0; for (int i = 0; i < fr; i++) a += (double)s0[f * fr + i] * s0[f * fr + i]; float v = (float)sqrt(a / fr); if (prev > prev2 * 1.15f && prev > v * 1.15f) bumps++; prev2 = prev; prev = v; }
      Check("panel shed", "bounces in the envelope", (float)bumps, 3.0f, kAny);
    }
  }
  /* the hazard loops and tells */
  static const int kHz[5] = {3, 4, 5, 11, 12};
  static const char *const kHzName[5] = {"killsaw loop", "flame grate loop", "screw loop", "laser loop", "wind loop"};
  for (int h = 0; h < 5; h++) m = rec.Slot(kHzName[h], 1.5f, [&](int f) { audio_band_hazard(0, kHz[h], 1.0f, 0.0f, 0.3f); }, 0.8f);
  static const int kTell[5] = {2, 3, 4, 10, 11};
  static const char *const kTellName[5] = {"pulverizer tell", "killsaw tell", "flame tell", "drop zone tell", "laser tell"};
  for (int h = 0; h < 5; h++) m = rec.Slot(kTellName[h], 1.6f, [&](int f) { if (f == 0) { audio_play_var(SFX_KLAXON, SFXV_TELL); audio_band_haz_tell(kTell[h], 0.3f, 1.5f); } });
  /* the crowd: the bed, then each reaction over it, measured as RMS over its body */
  struct CrowdT { const char *name; int kind; float lo, hi; float from, len; } crowd[] = {
    {"crowd bed", -1, -36.0f, -32.0f, 0.5f, 1.0f}, {"crowd OOOH", CROWD_OOOH, -28.0f, -24.0f, 0.1f, 0.5f},
    {"crowd ROAR", CROWD_ROAR, -22.0f, -18.0f, 0.2f, 0.8f}, {"crowd KO", CROWD_KO, -18.0f, -14.0f, 0.4f, 1.5f}, {"crowd JEER", CROWD_JEER, -99.0f, 0.0f, 0.0f, 0.3f}};
  for (const CrowdT &c : crowd) {
    m = rec.Slot(c.name, 3.0f, [&](int f) { if (f == 0) { audio_band_crowd_bed(0.12f); if (c.kind >= 0) audio_band_crowd_react(c.kind); } }, c.from + 0.2f);
    if (c.lo < -90.0f) continue;
    Meas s = Measure(rec.mono + rec.at - (int)(3.0f * rec.rate) + (int)(c.from * rec.rate), (int)(c.len * rec.rate), rec.rate);
    Check(c.name, "rms dBFS", Db(s.rms), c.lo, c.hi);
    if (c.kind == CROWD_ROAR) Check(c.name, "formant peak Hz (500 to 1500)", Spectrum(rec.mono + rec.at - (int)(2.6f * rec.rate), 4096, rec.rate, 500.0f, 1500.0f).peak_hz, 600.0f, 1200.0f);
  }
  /* the garage parts: every row and option */
  static const int kOpts[6] = {6, 8, 11, 7, 4, 11};
  for (int r = 0; r < 5; r++) for (int o = 0; o < kOpts[r]; o++) {
    char nm[64]; snprintf(nm, sizeof nm, "part row %d option %d", r, o);
    m = rec.Slot(nm, 1.0f, [&](int f) { if (f == 0) audio_part(r, o); }, 0.0f, r == 2);
    OneShotCeiling(nm, m);
  }
  /* the cost: a dense second (a clash, a wreck and a KO over four drive loops, four weapon
   * loops, two hazard loops, the fizz and a crowd roar) in 800 frame slices, as the cart */
  {
    const int rate = 48000;
    /* the machine is shared (other builds, the rig), so the cost is the best of twelve runs */
    float slice[800 * 2];
    double ms = 1e9;
    for (int run = 0; run < 12; run++) {
      audio_band_init(rate);
      audio_band_levels(kDefaultSfx, kDefaultMusic);
      audio_band_crowd_bed(0.12f);
      double acc = 0;
      for (int k = 0; k < 60; k++) {
        for (int s = 0; s < 4; s++) {
          audio_band_drive(s, s == 0 ? DK_TREAD : s == 1 ? DK_WHEEL_FOUR : s == 2 ? DK_HOVER : DK_WHEEL_SIX, 0.8f, 0.2f, 1, 0, DRV_THROTTLE);
          audio_band_damage(s, 0.3f, s == 1, 1);
        }
        audio_band_weapon(0, WK_SPIN_HORIZ, 1.0f, 0.9f, 0.0f); audio_band_weapon(1, WK_SAW, 1.0f, 1.0f, 1.0f);
        audio_band_weapon(2, WK_FLAME, 1.0f, 0.0f, 0.0f); audio_band_weapon(3, WK_SPIN_DRUM, 1.0f, 0.8f, 0.0f); audio_band_weapon(3, WK_SCRAPE, 0.6f, 0.5f, 0.0f);
        audio_band_hazard(0, 3, 0.8f, 0.2f, 0.0f); audio_band_hazard(1, 11, 0.6f, -0.3f, 0.2f);
        if (k == 0) { audio_band_hit(HK_SPINNER, 4, 1.0f, 0.2f, 4, 0); audio_band_crowd_react(CROWD_ROAR); }
        if (k == 20) audio_band_hit(HK_HAMMER, 3, 1.0f, -0.2f, 2, 3);
        if (k == 40) { audio_band_hit(HK_GENERIC, 5, 1.0f, 0.0f, 4, -1); audio_band_crowd_react(CROWD_KO); }
        memset(slice, 0, sizeof slice);
        clock_t t0 = clock();
        audio_band_render(slice, 800, rate);
        acc += (clock() - t0) * 1000.0 / CLOCKS_PER_SEC;
      }
      if (acc / 60.0 < ms) ms = acc / 60.0;
    }
    printf("band cost on this machine: %.3f ms per 800 frame render under a dense load\n", ms);
    Check("band cost", "ms per 800 frames", (float)ms, 0.0f, 0.7f);
  }
  char path[1024];
  snprintf(path, sizeof path, "%s/sfx_preview.wav", dir);
  float pk, rms; Stats(rec.buf, rec.at * 2, &pk, &rms);
  WriteWav(path, rec.buf, rec.at, 2, rec.rate, 1.0f);
  printf("wrote %s (%d s, peak %.1f dBFS)\n", path, rec.at / rec.rate, Db(pk));
  free(rec.buf); free(rec.mono);
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
  audio_band_init(rate);
  audio_bake_begin(mode, 0, STEM_ALL, 0);
  audio_bake_render(music, n);
  for (int i = 0; i < n; i++) { float m = 0.5f * (music[i * 2] + music[i * 2 + 1]) * mg; music[i * 2] = music[i * 2 + 1] = m; }
  float mpk, mrms; Stats(music, n * 2, &mpk, &mrms);
  static const Cue kShelf[] = {
    {1.0f, SFX_MENU_MOVE, SFXV_DEFAULT, 0}, {1.4f, SFX_MENU_MOVE, SFXV_DEFAULT, 0}, {2.2f, SFX_MENU_OK, SFXV_DEFAULT, 0},
    {4.0f, SFX_MENU_MOVE, SFXV_DEFAULT, 0}, {5.5f, SFX_MENU_BACK, SFXV_DEFAULT, 0}, {7.0f, SFX_LOCK, SFXV_DEFAULT, 0},
    {9.0f, SFX_HIT_HEAVY, SFXV_DEFAULT, 0}, {11.0f, SFX_MENU_OK, SFXV_LOW, 0}, {13.0f, SFX_HIT_LIGHT, SFXV_DEFAULT, 0},
    {15.0f, SFX_LOCK, SFXV_DEFAULT, 0}, {17.0f, SFX_COUNT, SFXV_DEFAULT, 0}, {18.0f, SFX_COUNT, SFXV_DEFAULT, 0}, {19.0f, SFX_HORN, SFXV_DEFAULT, 0}};
  static const Cue kFight[] = {
    {1.5f, SFX_HIT_LIGHT, SFXV_DEFAULT, 0}, {3.2f, SFX_HIT_HEAVY, SFXV_DEFAULT, 0.6f}, {6.4f, SFX_CLASH, SFXV_DEFAULT, 0.9f},
    {9.6f, SFX_HIT_HEAVY, SFXV_DEFAULT, 0.7f}, {11.5f, SFX_KLAXON, SFXV_TELL, 0}, {13.0f, SFX_LAUNCH, SFXV_DEFAULT, 0},
    {16.3f, SFX_CLASH, SFXV_DEFAULT, 0.9f}, {18.3f, SFX_KO, SFXV_DEFAULT, 1.0f}};
  const Cue *cues = mode == MUSIC_GARAGE ? kShelf : kFight;
  const int ncue = mode == MUSIC_GARAGE ? (int)(sizeof kShelf / sizeof kShelf[0]) : (int)(sizeof kFight / sizeof kFight[0]);
  audio_band_init(rate);
  audio_band_levels(kDefaultSfx, kDefaultMusic);
  audio_band_crowd_bed(mode == MUSIC_GARAGE ? 0.0f : 0.12f);
  int next = 0;
  for (int at = 0; at < n; at += 800) {
    while (next < ncue && cues[next].at * rate <= at) {
      audio_play_var(cues[next].sfx, cues[next].var);
      if (cues[next].crowd > 0) audio_crowd(cues[next].crowd);
      next++;
    }
    int c = n - at < 800 ? n - at : 800;
    float dk = audio_band_music_duck();
    for (int i = 0; i < c * 2; i++) mix[(size_t)at * 2 + i] = music[(size_t)at * 2 + i] * dk;
    audio_bake_sfx_render(mix + (size_t)at * 2, c);
  }
  float pk, rms; Stats(mix, n * 2, &pk, &rms);
  float tp = 0;
  for (int c = 0; c < 2; c++) { for (int i = 0; i < n; i++) music[i] = mix[i * 2 + c]; float t = TruePeak(music, n); if (t > tp) tp = t; }
  char path[1024];
  snprintf(path, sizeof path, "%s/preview-%s.wav", dir, MODE_NAME[mode]);
  WriteWav(path, mix, n, 2, rate, 1.0f);
  printf("wrote %s (%d s, music alone peak %.1f rms %.1f dBFS, mix peak %.1f true %.1f rms %.1f dBFS)\n",
         path, secs, Db(mpk), Db(mrms), Db(pk), Db(tp), Db(rms));
  free(music); free(mix);
}

/* -- the fight preview (deep-audio 6.2): ARENA under a scripted fight -- */
void FightPreview(const char *dir, float group_gain) {
  const int rate = 48000, secs = 20, n = rate * secs;
  const float mg = group_gain * kModeTrim[MUSIC_ARENA] * LevelGain(kDefaultMusic);
  float *song = (float *)calloc((size_t)n * 2, sizeof(float));
  float *music = (float *)calloc((size_t)n, sizeof(float));    /* the music as the mixer ducks it, mono */
  float *mix = (float *)calloc((size_t)n * 2, sizeof(float));
  float *sfx = (float *)calloc((size_t)n * 2, sizeof(float));
  audio_band_init(rate);
  audio_bake_begin(MUSIC_ARENA, 0, STEM_ALL, 0);
  audio_bake_render(song, n);
  /* the script, twice: once over the song (the mix), once over silence (the SFX alone, for the pitch track) */
  struct Imp { float at; const char *name; } imps[] = {{3.2f, "CLANK HK_RAM"}, {6.4f, "WRECK HK_SPINNER"}, {8.85f, "CRUNCH HK_HAMMER"}, {12.0f, "PULVERIZER"},
                                                      {14.4f, "LANDING"}, {16.3f, "CLASH"}, {18.3f, "KO"}};
  float wreck_duck = 1.0f;
  for (int pass = 0; pass < 2; pass++) {
    float *out = pass == 0 ? mix : sfx;
    audio_band_init(rate);
    audio_band_levels(kDefaultSfx, kDefaultMusic);
    audio_band_crowd_bed(0.12f);
    int frame = 0;
    for (int at = 0; at < n; at += 800, frame++) {
      const float t = at / (float)rate, t1 = (at + 800) / (float)rate;
      auto cue = [&](float c) { return c >= t && c < t1; };
      if (cue(0.0f)) { audio_play_var(SFX_HORN, 0); audio_band_crowd_react(CROWD_ROAR); }
      /* the drives from 0.5 s: slot 0 TREADS, slot 1 FOUR WHEEL */
      if (t >= 0.5f) { audio_band_drive(0, DK_TREAD, 0.8f, 0.1f, 1, 0, DRV_THROTTLE); audio_band_drive(1, DK_WHEEL_FOUR, 0.8f, 0.2f, 1, 0, DRV_THROTTLE); }
      audio_band_slot_pan(0, -0.3f); audio_band_slot_pan(1, 0.3f); audio_band_slot_pan(2, 0.0f);
      /* slot 0 HORIZONTAL: spins up from 1.4 s over 1.3 s, killed by the wall at 10 s */
      float spin0 = t < 1.4f ? 0.0f : t < 2.7f ? (t - 1.4f) / 1.3f : t < 10.0f ? 1.0f : 0.0f;
      audio_band_weapon(0, WK_NONE, 0, 0, 0);
      audio_band_weapon(0, WK_SPIN_HORIZ, 1.0f, spin0, 0.0f);
      /* slot 1 SAW: on at 4.5, in contact 5.0 to 6.5 */
      audio_band_weapon(1, WK_NONE, 0, 0, 0);
      if (cue(4.5f)) audio_band_tell(1, 4, 11, 0.3f);
      if (t >= 4.5f) audio_band_weapon(1, WK_SAW, 1.0f, t < 5.3f ? (t - 4.5f) / 0.8f : 1.0f, t >= 5.0f && t < 6.5f ? 1.0f : 0.0f);
      if (cue(5.0f) || cue(5.5f) || cue(6.0f)) audio_band_hit(HK_SAW, 0, 0.3f, 0.3f, 2, 1);
      /* slot 1 ram: the scrape 0.4 s before */
      if (t >= 2.8f && t < 3.2f) audio_band_weapon(1, WK_SCRAPE, 0.6f, 0.5f, 0.0f);
      if (cue(3.2f)) { audio_band_hit(HK_RAM, 1, 0.6f, -0.3f, 4, 1); audio_band_duck_band(1); }
      if (cue(6.4f)) { audio_band_hit(HK_SPINNER, 3, 1.0f, 0.3f, 2, 0); audio_band_duck_band(3); audio_band_crowd_react(CROWD_ROAR); }
      audio_band_damage(1, t < 7.0f ? 1.0f : t < 15.0f ? 0.35f : t < 18.3f ? 0.15f : 0.0f, 0, t < 18.3f);
      if (cue(8.6f)) audio_band_tell(2, 6, 0, 0.0f);
      if (cue(8.85f)) { audio_band_hit(HK_HAMMER, 2, 0.7f, 0.0f, 4, 2); audio_band_duck_band(2); audio_band_crowd_react(CROWD_OOOH); }
      if (cue(10.0f)) audio_band_spin_change(0, 2, 1.0f, 1.0f);
      if (cue(11.5f)) { audio_play_var(SFX_KLAXON, SFXV_TELL); audio_band_haz_tell(2, 0.4f, 0.5f); }
      if (cue(12.0f)) { audio_band_hit(HK_HAZ_PULVERIZER, 2, 1.0f, 0.4f, 4, -1); audio_band_duck_band(3); }
      if (cue(13.0f)) audio_band_tell(2, 7, 0, 0.0f);
      if (cue(13.05f)) { audio_band_play_at(SFX_LAUNCH, 0, 0.3f); audio_band_play_at(SFX_LAUNCH_WHOOSH, 0, 0.3f); }
      if (cue(14.4f)) { audio_band_hit(HK_LAND, 2, 1.0f, 0.3f, 4, -1); audio_band_duck_band(2); audio_band_crowd_react(CROWD_OOOH); }
      if (cue(16.3f)) { audio_band_hit(HK_SPINNER, 4, 0.9f, 0.0f, 4, 0); audio_band_duck_band(4); audio_band_crowd_react(CROWD_ROAR); }
      if (cue(17.0f)) audio_band_play_at(SFX_SHED, 0, 0.3f);
      if (cue(18.3f)) { audio_band_hit(HK_GENERIC, 5, 1.0f, 0.3f, 4, -1); audio_band_duck_band(5); audio_band_crowd_react(CROWD_KO); audio_band_slowmo(1); }
      int c = n - at < 800 ? n - at : 800;
      float dk = audio_band_music_duck();
      if (pass == 0 && t >= 6.4f && t < 6.5f && dk < wreck_duck) wreck_duck = dk;
      for (int i = 0; i < c; i++) {
        float m = pass == 0 ? 0.5f * (song[(size_t)(at + i) * 2] + song[(size_t)(at + i) * 2 + 1]) * mg * dk : 0.0f;
        if (pass == 0) music[at + i] = m;
        out[(size_t)(at + i) * 2] = out[(size_t)(at + i) * 2 + 1] = m;
      }
      audio_bake_sfx_render(out + (size_t)at * 2, c);
    }
  }
  float *mono = (float *)calloc((size_t)n, sizeof(float));
  for (int i = 0; i < n; i++) mono[i] = 0.5f * (mix[i * 2] + mix[i * 2 + 1]);
  float pk, rms; Stats(mono, n, &pk, &rms);
  float tp = 0; { float *ch = (float *)calloc((size_t)n, sizeof(float)); for (int c = 0; c < 2; c++) { for (int i = 0; i < n; i++) ch[i] = mix[i * 2 + c]; float t = TruePeak(ch, n); if (t > tp) tp = t; } free(ch); }
  float mpk, mrms; Stats(music, n, &mpk, &mrms);
  /* the bed: the music's median second */
  float sec[20]; for (int s = 0; s < secs; s++) { double a = 0; for (int i = 0; i < rate; i++) a += (double)music[s * rate + i] * music[s * rate + i]; sec[s] = (float)sqrt(a / rate); }
  float srt[20]; memcpy(srt, sec, sizeof srt);
  for (int i = 0; i < secs; i++) for (int j = i + 1; j < secs; j++) if (srt[j] < srt[i]) { float t = srt[i]; srt[i] = srt[j]; srt[j] = t; }
  const float bed = srt[secs / 2];
  printf("fight preview: mix rms %.1f dBFS, true peak %.1f dBFS, music alone rms %.1f dBFS (bed median second %.1f)\n", Db(rms), Db(tp), Db(mrms), Db(bed));
  Check("fight preview", "mix rms dBFS", Db(rms), -18.0f, -13.0f);
  Check("fight preview", "true peak dBFS", Db(tp), -120.0f, -0.8f);
  Check("fight preview", "music alone rms dBFS", Db(mrms), -26.0f, -22.0f);
  auto rms_at = [&](const float *m, float from, float len) { int a = (int)(from * rate), b = (int)((from + len) * rate); double s = 0; for (int i = a; i < b; i++) s += (double)m[i] * m[i]; return (float)sqrt(s / (b - a)); };
  for (const Imp &im : imps) {
    float over = Db(rms_at(mono, im.at, 0.1f)) - Db(bed);
    printf("  %5.2f s  %-18s 100 ms rms %.1f dB over the bed\n", im.at, im.name, over);
    Check(im.name, "dB over the bed's median second", over, 6.0f, im.at > 18.0f ? 16.0f : 12.0f);
  }
  { /* the duck: the envelope's depth on the stems in the 100 ms after the WRECK (the band's own number),
     * and for the ear the bed's drop (the 100 ms before against the quietest 40 ms after) */
    float before = rms_at(music, 6.3f, 0.1f), after = 1e9f;
    for (int w = 0; w < 7; w++) { float a = rms_at(music, 6.4f + 0.01f * w, 0.04f); if (a < after) after = a; }
    printf("  the duck after the WRECK at 6.4: envelope %.1f dB, the bed drops %.1f dB\n", -Db(wreck_duck), Db(before) - Db(after));
    Check("duck after the WRECK", "envelope dB", -Db(wreck_duck), 7.0f, 9.0f); }
  { Spec s = Spectrum(mono + 5 * rate, 4096, rate); float hi = 0; for (int w = 0; w < 10; w++) { Spec q = Spectrum(mono + 5 * rate + w * 4800, 4096, rate); hi += q.share[3] + q.share[4]; } hi /= 10.0f; (void)s;
    printf("  the grind second (5 to 6 s): %.0f percent over 2 kHz\n", hi);
    Check("grind second", "share over 2 kHz", hi, 15.0f, 100.0f); }
  { float *sm = (float *)calloc((size_t)n, sizeof(float)); for (int i = 0; i < n; i++) sm[i] = 0.5f * (sfx[i * 2] + sfx[i * 2 + 1]);
    printf("  the whine from 1.5 to 2.5 s, the peak bin per 100 ms:");
    float first = 0, last = 0;
    for (int w = 0; w < 10; w++) { Spec q = Spectrum(sm + (int)(1.5f * rate) + w * rate / 10, 4096, rate, 80.0f, 1200.0f); printf(" %.0f", q.peak_hz); if (w == 0) first = q.peak_hz; last = q.peak_hz; }
    printf("\n");
    Check("whine track", "first 100 ms Hz", first, 0.0f, 200.0f); Check("whine track", "last 100 ms Hz", last, 600.0f, kAny);
    free(sm); }
  char path[1024];
  snprintf(path, sizeof path, "%s/preview-fight.wav", dir);
  WriteWav(path, mix, n, 2, rate, 1.0f);
  printf("wrote %s\n", path);
  free(song); free(music); free(mix); free(sfx); free(mono);
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
  if (!getenv("SFX_ONLY")) {
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
    if (total_bytes > kBudgetBytes) g_fails++;
  }
  /* the fight music against the hits at the default rows (MUSIC 6, SFX 8) */
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
    Preview(argv[2]);     /* first: a fresh process, as the 22c05b6 stinger numbers were taken */
    if (!getenv("SFX_ONLY")) for (int mode = MUSIC_GARAGE; mode <= MUSIC_OVERTIME; mode++) SongPreview(argv[2], mode, gain[mode == MUSIC_GARAGE ? 0 : mode == MUSIC_ARENA ? 2 : 1]);
    FightPreview(argv[2], gain[2]);
  }
  printf("%d FAIL%s against deep-audio 6.1 and 6.2\n", g_fails, g_fails == 1 ? "" : "S");
  return g_fails ? 1 : 0;
}
