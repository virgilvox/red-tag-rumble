/* audio_pcm.c: the wc_pcm_mixer side of the sound (spec 11.1, 11.2).
 *
 * The songs are baked to WAV by tools/bake_music.cpp and played here as
 * stems through wc_pcm_mixer.h; the Bellows band stays live only for the
 * SFX, the whines and the crowd, and is rendered over the stems by
 * audio_band_render, which also owns the master limiter.
 *
 * A song is a list of segments, each a bar count and up to three sounds
 * (base, drums, lead) that start on the same frame. ARENA is BREAK then
 * STOMP, looping from the top: one 8 bar base stem under both, a kit stem
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
 * the same calls drive its stem gains.
 *
 * The fight routing (DEEP-plan 9) lives here too, in C, because it reads
 * the match: the drain's audio_hit, audio_play_at, audio_tell and the rest
 * work out the pan from world x and hand the band its numbers;
 * audio_fight_update reads match_t once per render frame and drives every
 * seat's weapon, drive and damage loop and the two nearest live hazards,
 * and polls the edges the sim does not report yet (a spin drop, the flame
 * lighting, the spike, the crusher, the saw switch) so the band sounds the
 * same with or without the sim's DEEP events. A tell that arrives twice
 * (the event and the poll) inside 8 frames plays once. Owned by the audio
 * agent. */
#include "audio.h"
#include "sim.h"
#include "arena.h"
#include "juice.h"
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
#ifndef MUSIC_LIVE
static float band_duck = 1.0f;                 /* the band's duck envelope on the stems (audio_duck_band) */
#endif
#define HIT_DUCK_GAIN 0.63f                    /* 4 dB under the bed */
#define HIT_DUCK_S 0.15f

#ifndef MUSIC_LIVE
/* -- the baked stems ------------------------------------------------ */
enum { W_GARAGE, W_OVERTIME, W_BASE, W_STOMP, W_BREAK, W_RIFF, W_COUNT };
static const char *const WAV_NAME[W_COUNT] = {
  "garage.wav", "overtime.wav", "arena_base.wav", "arena_stomp.wav", "arena_break.wav", "arena_riff.wav" };
static int snd[W_COUNT];

/* ARENA is the breakbeat BREAK then the half time STOMP, looping from the top
 * (it opens at 156 bpm when the fight is fastest, deep-audio 5.2.4): one base
 * stem under both, a kit stem per section, and the riser riff as the lead
 * stem that audio_tension fades in */
typedef struct { int bars; int wav[STEMS]; } seg_t;        /* wav: W_* or -1 */
typedef struct { int nseg, loop; seg_t seg[MAX_SEG]; } song_t;
static const song_t SONGS[4] = {
  { 0, 0, { { 0, { -1, -1, -1 } } } },
  { 1, 0, { { 16, { W_GARAGE, -1, -1 } } } },
  { 2, 0, { { 8, { W_BASE, W_BREAK, W_RIFF } }, { 8, { W_BASE, W_STOMP, W_RIFF } } } },
  { 1, 0, { { 8, { W_OVERTIME, -1, -1 } } } },
};
/* the stems are baked to the ceiling for 16 bit resolution; this is where a
 * song sits in the cart before the MUSIC row: the fight 6 to 10 dB under the
 * hit sounds (tools/bake_music.cpp prints the numbers), the garage a menu */
static const float MODE_TRIM[4] = { 0.0f, 0.5f, 0.62f, 0.6f };
static const char *const MUSIC_NAME[4] = { "off", "GARAGE", "ARENA", "OVERTIME" };

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
static float stem_vol(int i) { return g[i] * fade * duck * band_duck * level_gain(music_level) * MODE_TRIM[play_mode]; }

static void music_log(const char *what, int mode) {
  char m[64];
  int n = snprintf(m, sizeof m, "[music] %s %s", what, MUSIC_NAME[mode & 3]);
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
  band_duck = audio_band_music_duck();
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
    else for (int i = 0; i < STEMS; i++) if (out_ch[i] >= 0) wc_mixer_set_volume(out_ch[i], out_g[i] * out_fade * duck * band_duck * level_gain(music_level) * out_trim);
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
  audio_band_overtime(mode == MUSIC_OVERTIME);
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
  audio_band_slowmo(slowmo);
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


/* -- the fight routing (DEEP-plan 9) -------------------------------- */
/* the band reads these tables by number (audio_band.cpp mirrors them) */
_Static_assert(BAND_TAP == 0 && BAND_HIT == 1 && BAND_HEAVY == 2 && BAND_HUGE == 3 && BAND_CLASH == 4 && BAND_KO == 5, "audio_band.cpp B_* mirror juice.h BAND_*");
_Static_assert(WP_NONE == 0 && WP_WEDGE == 1 && WP_SPIKE == 2 && WP_LIFTER == 3 && WP_SAW == 4 && WP_FLAME == 5 && WP_HAMMER == 6 &&
               WP_FLIPPER == 7 && WP_DRUM == 8 && WP_HORIZ == 9 && WP_CRUSHER == 10, "audio_band.cpp W_* mirror parts.h WP_*");
_Static_assert(AR_NONE == 0 && AR_PLY == 1 && AR_ALU == 2 && AR_UHMW == 3 && AR_STEEL == 4 && AR_TI == 5 && AR_WEAR == 6, "audio_band.cpp A_* mirror parts.h AR_*");
_Static_assert(HZ_PIT == 1 && HZ_PULVERIZER == 2 && HZ_KILLSAW == 3 && HZ_FLAME == 4 && HZ_SCREW == 5 && HZ_FLOORFLIP == 9 && HZ_DROPZONE == 10 &&
               HZ_LASER == 11 && HZ_WIND == 12, "audio_band.cpp Z_* mirror arena.h HZ_*");
_Static_assert(TELL_WINDUP == 0 && TELL_HAMMER_MISS == 1 && TELL_DIG_IN == 2 && TELL_SPIKE_OUT == 3 && TELL_SPIKE_BACK == 4 && TELL_CLAMP_START == 5 &&
               TELL_CLAMP_BITE == 6 && TELL_CLAMP_RELEASE == 7 && TELL_FLAME_ON == 8 && TELL_FLAME_OUT == 9 && TELL_FLIPPER_RESET == 10 &&
               TELL_SAW_ON == 11 && TELL_SAW_OFF == 12, "audio_band.cpp T_* mirror sim.h TELL_*");
_Static_assert(SPIN_LOST_HIT == 0 && SPIN_LOST_CLASH == 1 && SPIN_LOST_WALL == 2 && SPIN_ARMED == 3 && SPIN_LOST_STUN == 4, "audio_band.cpp S_* mirror sim.h SPIN_*");

#define TELL_DEDUPE 8                          /* render frames inside which a second identical tell is the same tell */
#define STALE_RENDERS 6                        /* renders without audio_fight_update before the loops let go */
typedef struct {
  float spin, spin2, fuel, fuel2, lift;
  int16_t active, active2, clamp, cooldown, lock;
  uint8_t saw_on, lit, lit2;
  int kill_seen;                               /* frames since an EV_SPIN_CHANGE kill, so the poll does not double it */
  int tell_at[TELL_COUNT];                     /* upd_n of the last tell per zone */
} slot_audio_t;
static slot_audio_t sa[MAX_BOTS];
static const match_t *fm;                      /* the match audio_fight_update last saw */
static float half_w = 8.0f;
static int fight_live, fight_stale, upd_n = 100, last_frame = -1;
static int haz_idx[2] = { -1, -1 };

static float pan_x(float x) { float p = x / (half_w > 0.5f ? half_w : 0.5f); return rtr_clamp(p, -1.0f, 1.0f) * 0.6f; }
static int slot_ok(int slot) { return slot >= 0 && slot < MAX_BOTS; }

void audio_hit(int kind, int band, float vel, float x, float z, int armour, int attacker_slot) { (void)z; audio_band_hit(kind, band, vel, pan_x(x), armour, attacker_slot); }
void audio_play_at(int sfx, int var, float x, float z) { (void)z; audio_band_play_at(sfx, var, pan_x(x)); }
void audio_crowd_react(int kind) { audio_band_crowd_react(kind); }
void audio_duck_band(int band) { audio_band_duck_band(band); }
void audio_weapon(int slot, int kind, float level, float pitch, float load) { audio_band_weapon(slot, kind, level, pitch, load); }
void audio_drive(int slot, int kind, float speed, float turn, int grounded, int inverted) { audio_band_drive(slot, kind, speed, turn, grounded, inverted, 0); }
void audio_damage(int slot, float hp_frac, int burning, int alive) { audio_band_damage(slot, hp_frac, burning, alive); }
void audio_hazard(int slot, int type, float level, float x, float aux) { audio_band_hazard(slot, type, level, pan_x(x), pan_x(aux)); }
void audio_whine(int slot, float level, float pitch) { if (!fight_live) audio_band_whine(slot, level, pitch); }

/* a tell the sim reported and the poll found inside TELL_DEDUPE frames is one tell */
static int tell_fresh(int slot, int zone) {
  if (!slot_ok(slot) || zone < 0 || zone >= TELL_COUNT) return 1;
  if (upd_n - sa[slot].tell_at[zone] <= TELL_DEDUPE) return 0;
  sa[slot].tell_at[zone] = upd_n;
  return 1;
}

void audio_tell(int slot, int weapon, int tell_zone, float x, float z) {
  (void)z;
  if (weapon == WP_NONE && fm && slot_ok(slot) && slot < fm->nbots) {
    /* the sim branch sets the weapon on every tell; before it, zone 0 and 1 are the hammer or the flipper */
    const bot_t *b = &fm->bots[slot];
    int w1 = b->build.weapon, w2 = b->build.weapon2;
    weapon = (w1 == WP_HAMMER || w1 == WP_FLIPPER) ? w1 : (w2 == WP_HAMMER || w2 == WP_FLIPPER) ? w2 : w1;
  }
  if (!tell_fresh(slot, tell_zone)) return;
  audio_band_tell(slot, weapon, tell_zone, pan_x(x));
}

void audio_spin_change(int slot, int zone, float spin_before, float spin_lost) {
  if (slot_ok(slot)) sa[slot].kill_seen = TELL_DEDUPE;
  audio_band_spin_change(slot, zone, spin_before, spin_lost);
}

void audio_haz_tell(int haz_index, float x, float z) {
  (void)z;
  int arena = fm ? fm->arena : -1;
  if (arena < 0 || arena >= ARENA_COUNT || haz_index < 0 || haz_index >= ARENAS[arena].nhaz) return;
  const hazard_def_t *h = &ARENAS[arena].haz[haz_index];
  audio_band_haz_tell(h->type, pan_x(x), h->tell_s);
}

static int drive_kind(int d) {
  switch (d) {
    case DR_TWO: return DK_WHEEL_TWO;  case DR_FOUR: return DK_WHEEL_FOUR;  case DR_SIX: return DK_WHEEL_SIX;  case DR_TREAD: return DK_TREAD;
    case DR_HOVER: return DK_HOVER;  case DR_WALKER: return DK_WALKER;  case DR_LEGTRACK: return DK_LEGTRACK;  case DR_MELTY: return DK_MELTY;
    default: return DK_NONE;
  }
}

/* a fresh match (or the first update after a stale spell): the poll's memory is the current state, no edges */
static void slot_reset(int i, const bot_t *b) {
  slot_audio_t *s = &sa[i];
  int keep[TELL_COUNT];
  memcpy(keep, s->tell_at, sizeof keep);
  memset(s, 0, sizeof *s);
  memcpy(s->tell_at, keep, sizeof keep);
  s->spin = b->spin; s->spin2 = b->spin2; s->fuel = b->fuel; s->fuel2 = b->fuel2; s->lift = b->lift;
  s->active = b->active; s->active2 = b->active2; s->clamp = b->clamp; s->cooldown = b->cooldown; s->lock = b->lock;
  s->saw_on = b->saw_on;
}

/* one seat's weapon: the tone, the flame, the scrape, the creak, and the polled tells */
static void slot_weapon(int i, const bot_t *b, int adv) {
  slot_audio_t *s = &sa[i];
  const int w1 = b->build.weapon, w2 = b->build.weapon2;
  const int melty = b->build.drive == DR_MELTY && !b->is_ops;
  const float px = b->b.x;
  audio_band_weapon(i, WK_NONE, 0, 0, 0);
  /* the tone: the louder of the two weapons' spinners, the melty frame, OPS's saw, else the lifter */
  int tk = WK_NONE; float tl = 0.0f, tp = 0.0f, tload = 0.0f, spin_now = 0.0f;
  if (b->is_ops) { tk = WK_SAW; tp = b->spin2; tl = b->spin2 > 0.02f ? 1.0f : 0.0f; spin_now = b->spin2; }
  else if (melty) { tk = WK_MELTY; tp = b->spin; tl = 1.0f; spin_now = b->spin; }
  else {
    for (int which = 0; which < 2; which++) {
      int wid = which ? w2 : w1; float sp = which ? b->spin2 : b->spin;
      int k = wid == WP_HORIZ ? WK_SPIN_HORIZ : wid == WP_DRUM ? WK_SPIN_DRUM : wid == WP_SAW ? WK_SAW : WK_NONE;
      if (k == WK_NONE || (tk != WK_NONE && sp <= tp)) continue;
      tk = k; tp = sp; spin_now = sp;
      tl = k == WK_SAW ? ((b->saw_on || sp > 0.02f) ? 1.0f : 0.0f) : 1.0f;
      tload = k == WK_SAW && b->grind_t > 0 ? 1.0f : 0.0f;
    }
    if (tk == WK_NONE && (w1 == WP_LIFTER || w2 == WP_LIFTER)) {
      float moving = fabsf(b->lift - s->lift);
      if (adv && moving > 0.001f) { tk = WK_LIFTER_SERVO; tl = 1.0f; tp = b->lift; }
      else if (b->lift > 0.9f) { tk = WK_LIFTER_SERVO; tl = 0.6f; tp = 1.0f; }
    }
  }
  if (tk != WK_NONE) audio_band_weapon(i, tk, tl, tp, tload);
  /* a spin that falls over 0.3 in one frame with no EV_SPIN_CHANGE: the kill */
  float prev_spin = (b->is_ops || (!melty && w1 != WP_HORIZ && w1 != WP_DRUM && w1 != WP_SAW)) ? s->spin2 : s->spin;
  if (adv && (tk == WK_SPIN_HORIZ || tk == WK_SPIN_DRUM || tk == WK_MELTY || tk == WK_SAW) && s->kill_seen == 0 && prev_spin - spin_now > 0.3f)
    audio_band_spin_change(i, SPIN_LOST_CLASH, prev_spin, prev_spin - spin_now);
  if (s->kill_seen > 0) s->kill_seen--;
  if (adv) {
    /* the flame: lit while the fuel falls */
    for (int which = 0; which < 2; which++) {
      if ((which ? w2 : w1) != WP_FLAME) continue;
      float fuel = which ? b->fuel2 : b->fuel, *pf = which ? &s->fuel2 : &s->fuel;
      uint8_t *pl = which ? &s->lit2 : &s->lit;
      int lit = fuel < *pf - 0.5f;
      if (lit && !*pl) audio_tell(i, WP_FLAME, TELL_FLAME_ON, px, b->b.z);
      if (!lit && *pl && fuel <= 0.5f) audio_tell(i, WP_FLAME, TELL_FLAME_OUT, px, b->b.z);
      *pl = (uint8_t)lit; *pf = fuel;
    }
    /* the spike: out on the extend edge, back at the end */
    if (w1 == WP_SPIKE) {
      if (b->active > 0 && s->active == 0) audio_tell(i, WP_SPIKE, TELL_SPIKE_OUT, px, b->b.z);
      if (b->active == 0 && s->active > 0) audio_tell(i, WP_SPIKE, TELL_SPIKE_BACK, px, b->b.z);
    }
    if (w2 == WP_SPIKE) {
      if (b->active2 > 0 && s->active2 == 0) audio_tell(i, WP_SPIKE, TELL_SPIKE_OUT, px, b->b.z);
      if (b->active2 == 0 && s->active2 > 0) audio_tell(i, WP_SPIKE, TELL_SPIKE_BACK, px, b->b.z);
    }
    /* the crusher: the jaws start closing, and they let go when the lock runs out */
    if (w1 == WP_CRUSHER) {
      if (b->clamp > 0 && s->clamp == 0) audio_tell(i, WP_CRUSHER, TELL_CLAMP_START, px, b->b.z);
      if (b->lock == 0 && s->lock > 0) audio_tell(i, WP_CRUSHER, TELL_CLAMP_RELEASE, px, b->b.z);
    }
    /* the saw switch */
    if ((w1 == WP_SAW || w2 == WP_SAW) && b->saw_on != s->saw_on) audio_tell(i, WP_SAW, b->saw_on ? TELL_SAW_ON : TELL_SAW_OFF, px, b->b.z);
    /* the flipper is back */
    if (w1 == WP_FLIPPER && b->cooldown == 0 && s->cooldown > 0 && b->alive) audio_tell(i, WP_FLIPPER, TELL_FLIPPER_RESET, px, b->b.z);
    s->spin = b->spin; s->spin2 = b->spin2; s->lift = b->lift;
    s->active = b->active; s->active2 = b->active2; s->clamp = b->clamp; s->cooldown = b->cooldown; s->lock = b->lock; s->saw_on = b->saw_on;
  }
  if (s->lit || s->lit2) audio_band_weapon(i, WK_FLAME, 1.0f, 0.0f, 0.0f);
  /* the crusher's hold creaks on the bot in its jaws */
  if (b->clamped_by) audio_band_weapon(i, WK_CRUSHER_SERVO, 1.0f, 0.0f, 0.0f);
  /* the wedge scrape while it is under someone or digging in */
  if (b->riding || b->dig_t > 0) {
    float v = rtr_clamp(sqrtf(b->b.vx * b->b.vx + b->b.vz * b->b.vz) / 4.0f, 0.0f, 1.0f);
    audio_band_weapon(i, WK_SCRAPE, 0.35f + 0.3f * v, v, 0.0f);
  }
}

/* the two live continuous hazards nearest any seat bot inside 6 u, each held on its loop slot */
static void fight_hazards(const match_t *m) {
  const arena_def_t *a = &ARENAS[m->arena];
  float lvl[MAX_HAZ] = { 0 };
  for (int i = 0; i < a->nhaz && i < MAX_HAZ; i++) {
    const hazard_state_t *hs = &m->haz[i];
    int t = a->haz[i].type;
    if (!hs->active || hs->broken) continue;
    if (t != HZ_KILLSAW && t != HZ_FLAME && t != HZ_SCREW && t != HZ_LASER && t != HZ_WIND) continue;
    float best = 1e9f;
    if (t == HZ_LASER || t == HZ_WIND) best = 0.0f;
    else {
      float x0, z0, x1, z1; arena_hazard_rect(&a->haz[i], &x0, &z0, &x1, &z1);
      for (int j = 0; j < 4 && j < m->nbots; j++) {
        const bot_t *b = &m->bots[j];
        if (!b->alive || b->hp_max <= 0) continue;
        float dx = b->b.x < x0 ? x0 - b->b.x : b->b.x > x1 ? b->b.x - x1 : 0.0f;
        float dz = b->b.z < z0 ? z0 - b->b.z : b->b.z > z1 ? b->b.z - z1 : 0.0f;
        float d = sqrtf(dx * dx + dz * dz);
        if (d < best) best = d;
      }
    }
    if (best < 6.0f) lvl[i] = 1.0f - best / 6.0f;
  }
  /* keep a hazard on the loop slot it already has, then fill the free slot with the loudest */
  for (int k = 0; k < 2; k++) if (haz_idx[k] >= 0 && (haz_idx[k] >= a->nhaz || lvl[haz_idx[k]] <= 0.0f)) haz_idx[k] = -1;
  for (int k = 0; k < 2; k++) {
    if (haz_idx[k] >= 0) continue;
    int pick = -1;
    for (int i = 0; i < a->nhaz && i < MAX_HAZ; i++) if (lvl[i] > 0.0f && i != haz_idx[0] && i != haz_idx[1] && (pick < 0 || lvl[i] > lvl[pick])) pick = i;
    haz_idx[k] = pick;
  }
  for (int k = 0; k < 2; k++) {
    int i = haz_idx[k];
    if (i < 0) { audio_band_hazard(k, HZ_NONE, 0.0f, 0.0f, 0.0f); continue; }
    const hazard_def_t *h = &a->haz[i];
    float ax = h->type == HZ_LASER ? m->haz[i].aux : h->x;
    audio_band_hazard(k, h->type, lvl[i], pan_x(h->x), pan_x(ax));
  }
}

void audio_fight_update(const void *match) {
  if (!match) {
    if (fight_live) audio_band_loops(0);
    fight_live = 0; fm = NULL; last_frame = -1; haz_idx[0] = haz_idx[1] = -1;
    return;
  }
  const match_t *m = (const match_t *)match;
  int fresh = !fight_live || m != fm || m->frame < last_frame;
  fm = m; fight_live = 1; fight_stale = 0; upd_n++;
  if (m->arena < ARENA_COUNT) half_w = ARENAS[m->arena].w * 0.5f;
  int adv = m->frame != last_frame && !fresh;
  last_frame = m->frame;
  for (int i = 0; i < MAX_BOTS; i++) {
    const bot_t *b = &m->bots[i];
    if (fresh) slot_reset(i, b);
    int valid = i < m->nbots && (b->hp_max > 0 || b->is_ops);
    if (!valid) { audio_band_weapon(i, WK_NONE, 0, 0, 0); audio_band_drive(i, DK_NONE, 0, 0, 1, 0, 0); audio_band_damage(i, 1.0f, 0, 0); continue; }
    audio_band_slot_pan(i, pan_x(b->b.x));
    slot_weapon(i, b, adv);
    /* the drive: a dead bot is quiet, a falling one cuts out */
    int dk = b->alive ? drive_kind(b->build.drive) : DK_NONE;
    float sp = sqrtf(b->b.vx * b->b.vx + b->b.vz * b->b.vz), top = DRIVES[b->build.drive < DR_COUNT ? b->build.drive : 0].top;
    int grounded = !b->airborne && !b->lifted_by && b->ride_up < 0.5f;
    int flags = (b->boost > 0 ? DRV_BOOST : 0) | (b->skirt_t > 0 ? DRV_PINCH : 0) | (b->falling ? DRV_FALLING : 0) |
                (fabsf(b->drive_t) > 0.5f ? DRV_THROTTLE : 0) | ((b->pinned & 1) && fabsf(b->drive_t) > 0.5f && sp < 0.3f ? DRV_STUCK : 0);
    if (b->is_ops) dk = b->alive ? DK_WHEEL_SIX : DK_NONE;
    audio_band_drive(i, dk, top > 0.1f ? sp / top : 0.0f, fabsf(b->b.w) / 3.0f, grounded, b->inverted, flags);
    audio_band_damage(i, b->hp_max > 0 ? b->hp / b->hp_max : 1.0f, b->burn_t > 0, b->alive && !b->is_ops);
  }
  if (m->arena < ARENA_COUNT) fight_hazards(m);
}

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
  if (fight_live && ++fight_stale > STALE_RENDERS) audio_fight_update(NULL);   /* nobody drives the loops: let them go */
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
