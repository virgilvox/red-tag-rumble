/* audio_band.cpp: RED TAG RUMBLE's Bellows band (spec 11.2, 11.3).
 *
 * Two jobs in one translation unit so the bake and the cart never drift:
 *
 *   1. The live SFX band, always compiled: twelve patches on metal (a Modal
 *      bar pool for clangs), a dedicated impact kick, a square noise burst for
 *      sparks and saw contact, a buzzer for horns and klaxons, FM bells for
 *      the menus and a gong for the KO, four continuous spinner whines, and a
 *      crowd bed of filtered pink noise that swells on big hits. Everything
 *      two players can fire at once is pooled. A slap delay, no plate.
 *
 *   2. The songs, compiled only with MUSIC_BAKE (tools/bake_music.cpp renders
 *      them to assets/music/NAME.wav) or MUSIC_LIVE (composing on the Mac, the
 *      cart plays them live). Written the DO-OCRACY way: a form of sections,
 *      a chord per bar, a layer mask per section and a written lead line; a
 *      16 step conductor plays drums, acid, pad, arpeggio, stabs and the lead
 *      from those. All three songs are in E minor so DiatonicBelow holds.
 *
 *        1 GARAGE    "Shelf Life"   100 bpm   pad, slow arpeggio, sparse hat, sub, a lead on B
 *        2 ARENA     "Red Tag"      150 bpm   intro, build, drop, drop with a counter line
 *        3 OVERTIME  "Doors Lock"   170 bpm   the drop with a roll every section
 *
 * Compiled with em++ -std=c++17 -fno-exceptions -fno-rtti -DBELLOWS_FAST_MATH=1
 * for the cart and clang++ -DMUSIC_BAKE for the bake. Nothing here touches
 * libc++: <math.h> and <stdint.h> only. Owned by the audio agent (item 17). */
#include "bellows/config.h"
#include "bellows/core/prng.h"
#include "bellows/dsp/filters.h"
#include "bellows/dsp/noise.h"
#include "bellows/engines/drums.h"
#include "bellows/engines/fm.h"
#include "bellows/engines/modal.h"
#include "bellows/engines/va.h"
#include "bellows/fx/delay.h"
#include "bellows/fx/dynamics.h"
#include "bellows/fx/saturator.h"
#include "bellows/voicepool.h"
#include <math.h>
#include <stdint.h>

#if defined(MUSIC_BAKE) || defined(MUSIC_LIVE)
#define RTR_SONGS 1
#include "bellows/engines/pluck.h"
#include "bellows/fx/modfx.h"
#else
#define RTR_SONGS 0
#endif

extern "C" {
#include "audio.h"
}

namespace {

constexpr int kBlock = BELLOWS_BLOCK_SIZE;
float Mtof(float n) { return 440.0f * powf(2.0f, (n - 69.0f) / 12.0f); }

/* -- the songs (data) ----------------------------------------------
 * Notes are NAME+OCTAVE:LENGTH in sixteenths, '-' is a rest, '|' ends a bar. */

enum Layer : uint16_t {
  L_ACID = 1,     /* the 303 line */
  L_KICK = 2,     /* four on the floor */
  L_BREAK = 4,    /* breakbeat kick and snare */
  L_HAT = 8,
  L_PERC = 16,    /* claps on the backbeat */
  L_PAD = 32,
  L_HYPNO = 64,   /* the arpeggio */
  L_LEAD = 128,
  L_COUNTER = 256,
  L_STAB = 512,   /* rave chord stabs */
  L_ROLL = 1024,  /* snare roll into the next section */
  L_SUB = 2048,   /* a soft held sub instead of the acid line */
};

#ifdef MUSIC_BAKE
/* the stems: drums, lead, and everything else */
constexpr uint16_t MASK_DRUMS = L_KICK | L_BREAK | L_ROLL | L_HAT | L_PERC;
constexpr uint16_t MASK_LEAD = L_LEAD | L_COUNTER;
constexpr uint16_t MASK_BASE = static_cast<uint16_t>(~(MASK_DRUMS | MASK_LEAD));
#endif

const char* const LEAD_SHELF =
  "B4:8 -:8 | G4:4 A4:4 B4:8 | E5:8 -:8 | D5:4 B4:4 G4:8 | "
  "B4:8 -:8 | G4:4 A4:4 B4:8 | E5:4 D5:4 B4:4 A4:4 | G4:16 |";
const char* const LEAD_RED_TAG =
  "E5:2 -:2 E5:2 G5:2 -:2 A5:2 -:2 G5:2 | E5:2 -:2 D5:2 -:2 B4:4 D5:4 | "
  "E5:2 -:2 E5:2 G5:2 -:2 B5:2 -:2 C6:2 | B5:4 A5:4 G5:4 F#5:4 | "
  "E5:2 -:2 E5:2 G5:2 -:2 A5:2 -:2 G5:2 | E5:2 -:2 D5:2 -:2 B4:4 D5:4 | "
  "G5:2 -:2 A5:2 B5:2 -:2 D6:2 -:2 E6:2 | D6:4 B5:4 G5:4 E5:4 |";

struct SectionDef { const char* tag; int bars; const char* chords; uint16_t layers; const char* lead; };

constexpr uint16_t DROP = L_KICK | L_BREAK | L_HAT | L_PERC | L_ACID | L_STAB;
const SectionDef GARAGE[] = {
  {"A", 8, "Em Em C G Em Em D Bm", L_PAD | L_HYPNO | L_HAT | L_SUB, nullptr},
  {"B", 8, "Em Em C G Am Am D Em", L_PAD | L_HYPNO | L_HAT | L_SUB | L_LEAD, LEAD_SHELF},
};
const SectionDef ARENA[] = {
  {"INTRO", 8, "Em Em Em Em C C D D", L_KICK | L_HAT | L_ACID, nullptr},
  {"BUILD", 8, "Em Em Em Em C C D D", DROP | L_ROLL, nullptr},
  {"DROP", 8, "Em Em Em Em C C D D", DROP | L_LEAD, LEAD_RED_TAG},
  {"DROP2", 8, "Em Em Em Em C C D D", DROP | L_LEAD | L_COUNTER, LEAD_RED_TAG},
};
const SectionDef OVERTIME[] = {
  {"RUSH", 8, "Em Em C D Em Em C D", DROP | L_LEAD | L_STAB | L_ROLL, LEAD_RED_TAG},
};
constexpr int kGarageN = sizeof GARAGE / sizeof GARAGE[0];
constexpr int kArenaN = sizeof ARENA / sizeof ARENA[0];
constexpr int kOvertimeN = sizeof OVERTIME / sizeof OVERTIME[0];
constexpr int kLoop[4] = {0, 0, 1, 0};
constexpr float kBpm[4] = {0.0f, 100.0f, 150.0f, 170.0f};
/* the trims tools/bake_music.cpp measured (its "gains" line: the full mix of
 * each song at -1 dBFS, the garage 6 dB under), so the live songs sit where
 * the baked stems do; re-copy after any patch change */
constexpr float kSongTrim[4] = {0.0f, 0.403f, 0.365f, 0.308f};

const SectionDef* FormOf(int mode, int* count) {
  if (mode == 3) { *count = kOvertimeN; return OVERTIME; }
  if (mode == 2) { *count = kArenaN; return ARENA; }
  *count = kGarageN; return GARAGE;
}

int StepSamples(int mode, float sr) {
  if (mode < 1 || mode > 3) return 1;
  return static_cast<int>(sr * 60.0f / (kBpm[mode] * 4.0f) + 0.5f);
}

#if RTR_SONGS
struct Note { int16_t step; int8_t midi; uint8_t dur; };   /* step from the section start; midi 0 = rest */
struct Section {
  const SectionDef* def;
  int roots[8];
  bool minor[8];
  Note lead[96];
  int nlead;
};

int PitchClass(char c) {
  switch (c) { case 'C': return 0; case 'D': return 2; case 'E': return 4; case 'F': return 5; case 'G': return 7; case 'A': return 9; case 'B': return 11; }
  return 0;
}

void ParseSection(const SectionDef* d, Section* s) {
  s->def = d;
  const char* c = d->chords;
  for (int b = 0; b < 8 && *c; b++) {
    while (*c == ' ') c++;
    int pc = PitchClass(*c++);
    if (*c == '#') { pc = (pc + 1) % 12; c++; } else if (*c == 'b') { pc = (pc + 11) % 12; c++; }
    s->minor[b] = *c == 'm';
    while (*c && *c != ' ') c++;
    s->roots[b] = pc;
  }
  s->nlead = 0;
  if (!d->lead) return;
  const char* p = d->lead;
  int step = 0;
  while (*p && s->nlead < 96) {
    while (*p == ' ') p++;
    if (*p == '|') { step = ((step + 15) / 16) * 16; p++; continue; }
    if (!*p) break;
    int midi = 0;
    if (*p == '-') { p++; }
    else {
      int pc = PitchClass(*p++);
      if (*p == '#') { pc++; p++; } else if (*p == 'b') { pc--; p++; }
      int oct = *p++ - '0';
      midi = 12 * (oct + 1) + pc;
    }
    int dur = 4;
    if (*p == ':') { p++; dur = 0; while (*p >= '0' && *p <= '9') dur = dur * 10 + (*p++ - '0'); }
    s->lead[s->nlead++] = Note{static_cast<int16_t>(step), static_cast<int8_t>(midi), static_cast<uint8_t>(dur)};
    step += dur;
  }
}

/* E minor: the counter line sits a diatonic third below the lead */
int DiatonicBelow(int midi) {
  static const int kScale[7] = {4, 6, 7, 9, 11, 0, 2};   /* E F# G A B C D */
  int pc = midi % 12;
  for (int i = 0; i < 7; i++) if (kScale[i] == pc) {
    int down = kScale[(i + 5) % 7];
    int d = pc - down;
    if (d <= 0) d += 12;
    return midi - d;
  }
  return midi - 3;
}

/* the acid line: semitones over the bar's root, accents, -1 rests */
const int8_t ACID_N[16] = {0, -1, 12, 0, 0, 1, -1, 12, 0, -1, 10, 0, 12, 0, 7, 3};
const uint8_t ACID_A[16] = {1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 0};
/* the breakbeat: kick, snare and ghost notes (velocity, 0 = none) */
const float BRK_K[16] = {1, 0, .7f, 0, 0, 0, 0, 0, 0, 0, 1, .6f, 0, 0, 0, 0};
const float BRK_S[16] = {0, 0, 0, 0, 1, 0, 0, .3f, 0, .25f, 0, 0, 1, 0, 0, .4f};
#endif

/* -- the band ---------------------------------------------------- */

/* Active() for mono voices and pools alike */
template <class P> bool Live(P& p) { return p.Active(); }
template <class V, int N> bool Live(bellows::VoicePool<V, N>& p) { return p.ActiveCount() > 0; }

enum Inst { I_CLANG, I_IMPACT, I_SPARK, I_BUZZ, I_BUZZ_OFF, I_BELL, I_GONG, I_HAT, I_CLAP, I_WHINE_BLIP };
struct Ev { int at; int inst; float freq, vel; int gen; };
constexpr int kMaxEv = 128;

class Band {
 public:
  void Init(float sr) {
    sr_ = sr;
    rng_clang_.Init("rtr::clang");
    rng_spark_.Init("rtr::spark");
    rng_buzz_.Init("rtr::buzz");
    rng_bells_.Init("rtr::bells");
    rng_crowd_.Init("rtr::crowd");
    rng_whine_.Init("rtr::whine");
    rng_kit_.Init("rtr::kit");

    /* metal: a struck bar with a random pitch per hit so repeats never read as a sample */
    bellows::Modal::Params mp;
    mp.material = 0.0f; mp.decay = 0.6f; mp.strike_hardness = 0.9f; mp.brightness = 0.7f; mp.level = 0.6f;
    for (int i = 0; i < 4; ++i) clang_.at(i).Init(sr, &rng_clang_, mp);

    /* the impact kick, separate from any music */
    bellows::Kick::Params kp; kp.decay = 0.5f; kp.drive = 4.0f; kp.click_tune = 9.0f; kp.pitch_decay = 0.05f;
    impact_.Init(sr, kp);

    /* sparks and saw contact: a detuned square through a snapping filter */
    bellows::Va::Params sp;
    sp.shape = 1.0f; sp.detune = 40.0f; sp.cutoff = 6000.0f; sp.resonance = 0.25f; sp.filter_type = 1.0f; sp.env_amount = 3.0f;
    sp.attack = 0.001f; sp.decay = 0.12f; sp.sustain = 0.0f; sp.release = 0.05f;
    sp.f_attack = 0.001f; sp.f_decay = 0.08f; sp.f_sustain = 0.0f; sp.f_release = 0.05f;
    for (int i = 0; i < 4; ++i) spark_.at(i).Init(sr, &rng_spark_, sp);

    /* the buzzer: horns and klaxons; every I_BUZZ gets an I_BUZZ_OFF */
    bellows::Va::Params bz;
    bz.shape = 1.0f; bz.detune = 18.0f; bz.cutoff = 2400.0f; bz.resonance = 0.1f; bz.filter_type = 1.0f;
    bz.attack = 0.004f; bz.decay = 0.1f; bz.sustain = 0.7f; bz.release = 0.06f;
    buzz_.Init(sr, &rng_buzz_, bz);

    /* bells for the menus, a gong for the KO */
    bellows::Fm::Params bell;
    bell.ops = 4; bell.algorithm = 3; bell.brightness = 0.7f;
    bell.attack = 0.001f; bell.decay = 0.5f; bell.sustain = 0.0f; bell.release = 0.4f;
    bell.m_decay = 0.3f; bell.m_sustain = 0.1f;
    for (int i = 0; i < 4; ++i) bells_.at(i).Init(sr, bell);
    bellows::Fm::Params gong;
    gong.ops = 4; gong.algorithm = 3; gong.brightness = 0.6f; gong.feedback = 0.1f;
    gong.attack = 0.002f; gong.decay = 2.0f; gong.sustain = 0.0f; gong.release = 2.5f;
    gong.m_attack = 0.002f; gong.m_decay = 1.2f; gong.m_sustain = 0.15f; gong.m_release = 2.0f;
    gong.ratio[1] = 1.41f; gong.ratio[3] = 2.76f;
    gong_.Init(sr, gong);

    /* four spinner whines: a drifting saw under a steady lowpass. Every Va in
     * the live band runs its SVF, not the ladder: the ladder costs six times
     * as much per voice and four whines run for a whole fight */
    bellows::Va::Params wh;
    wh.shape = 0.0f; wh.detune = 6.0f; wh.cutoff = 1800.0f; wh.resonance = 0.3f; wh.drift = 0.02f; wh.filter_type = 1.0f;
    wh.attack = 0.04f; wh.decay = 0.1f; wh.sustain = 1.0f; wh.release = 0.25f; wh.vel_level = 0.0f;
    for (int i = 0; i < 4; ++i) { whine_[i].Init(sr, &rng_whine_, wh); whine_[i].SetParams(wh); whine_gain_[i] = whine_target_[i] = 0.0f; whine_on_[i] = false; }

    /* the crowd: pink noise through two bandpasses with a slow tremolo */
    crowd_.Init(sr, bellows::NoiseColor::kPink, &rng_crowd_);
    crowd_bp1_.Init(sr); crowd_bp1_.SetMode(bellows::SvfMode::kBp); crowd_bp1_.Set(800.0f, 1.1f);
    crowd_bp2_.Init(sr); crowd_bp2_.SetMode(bellows::SvfMode::kBp); crowd_bp2_.Set(2200.0f, 1.6f);
    crowd_lfo_ = 0.0f; crowd_gain_ = 0.0f; crowd_swell_ = 0.0f; crowd_bed_ = 0.0f;

    bellows::Hat::Params hp; hp.decay = 0.05f; hp.tone = 1.0f;
    tick_.Init(sr, hp);
    bellows::Snare::Params cp; cp.tone = 0.2f; cp.snap = 0.9f; cp.decay = 0.14f;
    clap_.Init(sr, &rng_kit_, cp);

    bellows::StereoDelay<300>::Params sd;
    sd.time_l = 0.093f; sd.time_r = 0.071f; sd.feedback = 0.22f; sd.cross_feedback = 0.1f; sd.damping = 4200.0f; sd.mix = 1.0f;
    slap_.Init(sr, sd);
    bellows::Saturator<1>::Params st; st.drive = 1.7f; st.tone = 0.1f;
    punch_.Init(sr, st);
    bellows::Compressor<>::Params cmp; cmp.threshold_db = -10.0f; cmp.ratio = 2.0f; cmp.knee_db = 6.0f; cmp.attack = 0.004f; cmp.release = 0.12f;
    comp_.Init(sr, cmp);
    bellows::Limiter<>::Params lim; lim.ceiling_db = -0.8f; lim.release = 0.06f;
    limiter_.Init(sr, lim);

#if RTR_SONGS
    for (int i = 0; i < kGarageN; i++) ParseSection(&GARAGE[i], &garage_[i]);
    for (int i = 0; i < kArenaN; i++) ParseSection(&ARENA[i], &arena_[i]);
    for (int i = 0; i < kOvertimeN; i++) ParseSection(&OVERTIME[i], &overtime_[i]);
    rng_snare_.Init("rtr::kit::snare");
    rng_acid_.Init("rtr::acid");
    rng_str_.Init("rtr::hypno");
    rng_ctr_.Init("rtr::counter");
    rng_jit_.Init("rtr::jitter");

    bellows::Hat::Params oh; oh.decay = 0.22f; oh.tone = 0.95f;
    ohat_.Init(sr, oh);
    bellows::Fm::Params pad;
    pad.ops = 2; pad.algorithm = 1; pad.feedback = 0.05f; pad.brightness = 0.25f;
    pad.attack = 0.6f; pad.decay = 0.8f; pad.sustain = 0.85f; pad.release = 0.9f;
    pad.m_attack = 0.6f; pad.m_decay = 1.0f; pad.m_sustain = 0.4f; pad.m_release = 0.8f;
    pad.ratio[0] = 1.0f; pad.ratio[1] = 2.0f; pad.level[1] = 0.35f;
    for (int i = 0; i < 6; ++i) pad_.at(i).Init(sr, pad);
    bellows::Fm::Params stb;
    stb.ops = 2; stb.algorithm = 1; stb.feedback = 0.45f; stb.brightness = 0.7f;
    stb.attack = 0.002f; stb.decay = 0.18f; stb.sustain = 0.0f; stb.release = 0.12f;
    stb.ratio[0] = 1.0f; stb.ratio[1] = 1.0f; stb.level[1] = 0.8f;
    for (int i = 0; i < 4; ++i) stab_.at(i).Init(sr, stb);
    String::Params hy; hy.damp = 0.18f; hy.pick_pos = 0.2f; hy.decay = 0.5f; hy.level = 0.9f;
    for (int i = 0; i < 4; ++i) hypno_.at(i).Init(sr, &rng_str_, hy);
    bellows::Va::Params ctr; ctr.shape = 3.0f; ctr.cutoff = 1400.0f; ctr.detune = 4.0f; ctr.attack = 0.01f; ctr.sustain = 0.6f; ctr.release = 0.15f;
    counter_.Init(sr, &rng_ctr_, ctr);
    bellows::StereoDelay<500>::Params d;
    d.time_l = 0.33f; d.time_r = 0.22f; d.feedback = 0.38f; d.cross_feedback = 0.15f; d.damping = 3200.0f; d.mix = 1.0f;
    echo_.Init(sr, d);
    bellows::Chorus<>::Params ch; ch.rate = 1.2f; ch.depth = 0.45f; ch.mix = 0.35f;
    chorus_.Init(sr, ch);
    bellows::Saturator<1>::Params sat; sat.drive = 2.2f; sat.tone = 0.1f;
    acid_sat_.Init(sr, sat);
    bellows::Saturator<1>::Params ds; ds.drive = 1.6f; ds.tone = 0.0f;
    drum_sat_.Init(sr, ds);
    SetGenre(G_GARAGE);
    mode_ = 0; pending_mode_ = -1; mask_ = 0xffff; hold_ = false;
    g_base_ = g_base_t_ = 1.0f; g_drums_ = g_drums_t_ = 1.0f; g_lead_ = g_lead_t_ = 1.0f;
#endif
    nev_ = 0; frame_ = 0; buzz_gen_ = 0; duck_ = 1.0f;
    ready_ = true;
  }

  bool Ready() const { return ready_; }

  void SetLevels(int sfx, int music) {
    float s = sfx < 0 ? 0.0f : sfx > 10 ? 1.0f : sfx / 10.0f;
    float m = music < 0 ? 0.0f : music > 10 ? 1.0f : music / 10.0f;
    sfx_gain_ = s <= 0.0f ? 0.0f : powf(s, 0.7f);      /* the fader law of audio_pcm.c */
    music_gain_ = m <= 0.0f ? 0.0f : powf(m, 0.7f);
  }
  void SetDuck(float g) { duck_ = g < 0.0f ? 0.0f : g > 1.0f ? 1.0f : g; }
  void SetCrowdBed(float level) { crowd_bed_ = level < 0.0f ? 0.0f : level > 1.0f ? 1.0f : level; }
  void Crowd(float level) { if (level > crowd_swell_) crowd_swell_ = level > 1.0f ? 1.0f : level; }

  void Whine(int slot, float level, float pitch) {
    if (slot < 0 || slot > 3) return;
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    if (pitch < 0.0f) pitch = 0.0f;
    if (pitch > 1.5f) pitch = 1.5f;
    whine_target_[slot] = level;
    whine_[slot].SetFreq(80.0f + pitch * 600.0f);
    if (level > 0.0f && !whine_on_[slot]) { whine_[slot].NoteOn(80.0f + pitch * 600.0f, 1.0f); whine_on_[slot] = true; }
  }

  void Push(int delay_ms, int inst, float freq, float vel) {
    if (nev_ >= kMaxEv) return;
    int gen = buzz_gen_;
    if (inst == I_BUZZ) gen = ++buzz_gen_;   /* an OFF releases only the buzz it was pushed with */
    ev_[nev_++] = Ev{static_cast<int>(delay_ms * sr_ / 1000.0f), inst, freq, vel, gen};
  }
  void Buzz(int at_ms, float midi, float vel, int len_ms) { Push(at_ms, I_BUZZ, Mtof(midi), vel); Push(at_ms + len_ms, I_BUZZ_OFF, 0, 0); }

  /* the twelve patches of 11.3; var is SFXV_* */
  void Sfx(int s, int var) {
    const float r = rng_bells_.Next();
    const float low = var == SFXV_LOW ? 1.0f : 0.0f;
    switch (s) {
      case SFX_MENU_MOVE: Push(0, I_HAT, 900, 0.25f); Push(0, I_BELL, Mtof(84), 0.3f); break;
      case SFX_MENU_OK:
        if (low) { Push(0, I_BELL, Mtof(67), 0.4f); Push(50, I_BELL, Mtof(72), 0.4f); }
        else { Push(0, I_BELL, Mtof(72), 0.6f); Push(50, I_BELL, Mtof(79), 0.6f); }
        break;
      case SFX_MENU_BACK: Push(0, I_BELL, Mtof(67), 0.5f); Push(60, I_BELL, Mtof(62), 0.5f); break;
      case SFX_LOCK: Push(0, I_IMPACT, 60, 0.6f); Push(0, I_BELL, Mtof(76), 0.7f); Push(40, I_BELL, Mtof(83), 0.7f); break;
      case SFX_COUNT: Push(0, I_BELL, Mtof(69), 0.8f); break;
      case SFX_HORN:
        if (low) { Buzz(0, 38, 1.0f, 400); }                       /* OPS inbound */
        else { Push(0, I_BUZZ, Mtof(45), 1.0f); Push(30, I_BUZZ, Mtof(52), 1.0f); Push(600, I_BUZZ_OFF, 0, 0); }
        break;
      case SFX_HIT_LIGHT:
        Push(0, I_SPARK, Mtof(60 + r * 12), 0.5f);
        if (!low) Push(0, I_CLANG, Mtof(70 + rng_bells_.Next() * 6), 0.4f);   /* the low variant is a landing or a wall: no metal */
        break;
      case SFX_HIT_HEAVY:
        Push(0, I_IMPACT, 42, 1.0f); Push(0, I_CLANG, Mtof(55 + r * 6), 0.9f);
        Push(0, I_SPARK, Mtof(58 + rng_bells_.Next() * 10), 0.8f); Push(20, I_CLANG, Mtof(62), 0.5f);
        break;
      case SFX_CLASH: {
        const float g = low ? 0.6f : 1.0f;                           /* 0.6 for the spinner wall hit */
        Push(0, I_IMPACT, 38, 1.0f * g);
        Push(0, I_CLANG, Mtof(50 + r * 6), 1.0f * g); Push(30, I_CLANG, Mtof(57 + rng_bells_.Next() * 6), 1.0f * g);
        Push(0, I_SPARK, Mtof(62 + rng_bells_.Next() * 8), 1.0f * g); Push(40, I_SPARK, Mtof(66 + rng_bells_.Next() * 8), 1.0f * g);
        Push(90, I_SPARK, Mtof(70 + rng_bells_.Next() * 8), 1.0f * g);
        break;
      }
      case SFX_LAUNCH:
        Push(0, I_SPARK, Mtof(48), 0.7f); Push(45, I_SPARK, Mtof(56), 0.7f); Push(90, I_SPARK, Mtof(64), 0.6f);
        Push(0, I_BELL, Mtof(88), 0.3f);
        break;
      case SFX_KLAXON:
        if (var == SFXV_TELL) { Buzz(0, 50, 0.5f, 150); Buzz(400, 50, 0.5f, 150); }
        else if (var == SFXV_SHORT) { Buzz(0, 50, 0.7f, 80); }
        else if (low) { Push(0, I_HAT, 1200, 0.3f); }               /* the inverted count tick */
        else { Buzz(0, 50, 0.7f, 150); }
        break;
      case SFX_KO:
        Push(0, I_IMPACT, 36, 1.0f); Push(0, I_GONG, Mtof(43), 1.0f);
        Push(80, I_CLANG, Mtof(50), 0.8f); Push(120, I_SPARK, Mtof(55 + r * 6), 0.6f);
        break;
      default: break;
    }
  }

  /* the part's own sound when a garage row cycles or the weapon test fires
   * (3.1); rows and options are parts.h order */
  void Part(int row, int opt) {
    const float r = rng_bells_.Next();
    switch (row) {
      case 0:   /* chassis: the shell rings, bigger is lower */
        { static const float kPitch[6] = {64, 62, 60, 55, 70, 66};
          Push(0, I_CLANG, Mtof(kPitch[opt < 0 ? 0 : opt > 5 ? 5 : opt] + r * 2), opt == 3 ? 0.8f : 0.55f); }
        break;
      case 1:   /* drive */
        switch (opt) {
          case 0: Push(0, I_WHINE_BLIP, 240, 0.4f); break;                                         /* two wheel: a quick spin */
          case 1: Push(0, I_WHINE_BLIP, 200, 0.4f); Push(90, I_WHINE_BLIP, 200, 0.3f); break;
          case 2: Push(0, I_WHINE_BLIP, 170, 0.4f); Push(80, I_WHINE_BLIP, 170, 0.3f); Push(160, I_WHINE_BLIP, 170, 0.3f); break;
          case 3: Push(0, I_IMPACT, 55, 0.5f); Push(0, I_CLANG, Mtof(48), 0.4f); Push(140, I_CLANG, Mtof(48), 0.4f); break;  /* treads */
          case 4: Push(0, I_WHINE_BLIP, 420, 0.5f); Push(60, I_WHINE_BLIP, 520, 0.4f); break;      /* hover: the blower */
          case 5: Push(0, I_CLANG, Mtof(60), 0.4f); Push(180, I_CLANG, Mtof(58), 0.4f); break;     /* walker: two steps */
          case 6: Push(0, I_IMPACT, 55, 0.4f); Push(120, I_CLANG, Mtof(50), 0.4f); break;         /* leg tracks */
          default: Push(0, I_WHINE_BLIP, 300, 0.5f); Push(50, I_WHINE_BLIP, 460, 0.5f); Push(100, I_WHINE_BLIP, 620, 0.5f); break;  /* melty */
        }
        break;
      case 2: case 5:   /* weapon, 2nd weapon */
        switch (opt) {
          case 0: Push(0, I_HAT, 700, 0.2f); break;                                                 /* none */
          case 1: Push(0, I_CLANG, Mtof(52), 0.6f); break;                                          /* wedge */
          case 2: Push(0, I_SPARK, Mtof(70), 0.6f); Push(0, I_CLANG, Mtof(76), 0.5f); break;        /* spike */
          case 3: Buzz(0, 40, 0.5f, 220); Push(220, I_CLANG, Mtof(58), 0.4f); break;                /* lifter */
          case 4: Push(0, I_SPARK, Mtof(72), 0.7f); Push(50, I_SPARK, Mtof(74), 0.7f); Push(100, I_SPARK, Mtof(76), 0.7f); break;  /* saw */
          case 5: Push(0, I_SPARK, Mtof(40), 0.6f); Push(70, I_SPARK, Mtof(38), 0.5f); Push(140, I_SPARK, Mtof(36), 0.4f); break;  /* flame */
          case 6: Push(0, I_IMPACT, 48, 0.9f); Push(0, I_CLANG, Mtof(50), 0.8f); break;             /* hammer */
          case 7: Push(0, I_SPARK, Mtof(48), 0.7f); Push(45, I_SPARK, Mtof(56), 0.7f); Push(0, I_BELL, Mtof(88), 0.3f); break;  /* flipper */
          case 8: case 9:                                                                           /* drum, horizontal: a spin up */
            Push(0, I_WHINE_BLIP, 160, 0.5f); Push(90, I_WHINE_BLIP, 260, 0.5f); Push(180, I_WHINE_BLIP, 400, 0.5f); break;
          default: Buzz(0, 36, 0.6f, 180); Push(180, I_CLANG, Mtof(46), 0.7f); break;               /* crusher */
        }
        break;
      case 3:   /* armour: the material rings */
        { static const float kPitch[7] = {72, 50, 79, 60, 64, 83, 55};
          static const float kVel[7] = {0.2f, 0.35f, 0.5f, 0.3f, 0.6f, 0.6f, 0.7f};
          int o = opt < 0 ? 0 : opt > 6 ? 6 : opt;
          Push(0, I_CLANG, Mtof(kPitch[o] + r * 2), kVel[o]); }
        break;
      default:  /* paint */
        Push(0, I_BELL, Mtof(79 + (opt & 3) * 2), 0.35f);
        break;
    }
  }

#if RTR_SONGS
  /* 0 silence, 1 GARAGE, 2 ARENA, 3 OVERTIME. GARAGE to ARENA is a hard cut,
   * ARENA to OVERTIME waits for the next beat, anything to GARAGE or silence
   * applies now (the stem fade lives in the mixer path, not here). */
  void SetMode(int m) {
    if (m == mode_) return;
    pending_mode_ = m;
    if (m == 3 && mode_ == 2) return;      /* at the next beat, in Step */
    ApplyMode();
  }
  void SetStems(float base, float drums, float lead) { g_base_t_ = base; g_drums_t_ = drums; g_lead_t_ = lead; }

  /* the bake: the conductor at the start of one section, with a stem mask */
  void BakeBegin(int mode, int section, uint16_t mask, bool hold) {
    pending_mode_ = mode;
    ApplyMode();
    int count; FormOf(mode, &count);
    section_ = section < 0 ? 0 : section >= count ? count - 1 : section;
    mask_ = mask; hold_ = hold;
    g_base_ = g_base_t_ = 1.0f; g_drums_ = g_drums_t_ = 1.0f; g_lead_ = g_lead_t_ = 1.0f;
  }
#endif

  /* the music only, no SFX and no master chain: the stems must sum linearly */
  void RenderMusic(float* l, float* r, int n) {
    int i = 0;
    while (i < n) {
      int span = n - i;
#if RTR_SONGS
      if (mode_ > 0 && countdown_ <= 0) { Step(); countdown_ = samples_per_step_; }
      if (mode_ > 0 && span > countdown_) span = countdown_;
#endif
      if (span > kBlock) span = kBlock;
#if RTR_SONGS
      for (int k = 0; k < span; ++k) { yl_[k] = yr_[k] = 0.0f; }
      MusicSpan(l, r, i, span, 1.0f);
      echo_.Process(yl_, yr_, 0, span);
      for (int k = 0; k < span; ++k) { l[i + k] += 0.6f * yl_[k]; r[i + k] += 0.6f * yr_[k]; }
      if (mode_ > 0) countdown_ -= span;
#endif
      frame_ += static_cast<uint32_t>(span);
      i += span;
    }
  }

  /* the live band over a buffer that already holds the PCM stems */
  void Render(float* l, float* r, int n) {
    int i = 0;
    while (i < n) {
      int span = n - i;
#if RTR_SONGS
      if (mode_ > 0 && countdown_ <= 0) { Step(); countdown_ = samples_per_step_; }
      if (mode_ > 0 && span > countdown_) span = countdown_;
#endif
      if (span > kBlock) span = kBlock;
      Fire(span);
      SfxSpan(l, r, i, span);
#if RTR_SONGS
      if (mode_ > 0) countdown_ -= span;
#endif
      frame_ += static_cast<uint32_t>(span);
      i += span;
    }
    comp_.Process(l, r, 0, n);
    limiter_.Process(l, r, 0, n);
  }

 private:
  void Fire(int span) {
    int k = 0;
    for (int e = 0; e < nev_; ++e) {
      Ev& v = ev_[e];
      if (v.at <= 0) {
        switch (v.inst) {
          case I_CLANG: clang_.NoteOn(note_id_++ & 0xfff, v.freq, v.vel, frame_); break;
          case I_IMPACT: impact_.NoteOn(v.freq, v.vel); break;
          case I_SPARK: spark_.NoteOn(note_id_++ & 0xfff, v.freq, v.vel, frame_); break;
          case I_BUZZ: buzz_.NoteOn(v.freq, v.vel); buzz_live_ = true; break;
          case I_BUZZ_OFF: if (v.gen == buzz_gen_ && buzz_live_) { buzz_.NoteOff(); buzz_live_ = false; } break;
          case I_BELL: bells_.NoteOn(note_id_++ & 0xfff, v.freq, v.vel, frame_); break;
          case I_GONG: gong_.NoteOn(v.freq, v.vel); break;
          case I_HAT: tick_.NoteOn(v.freq, v.vel); break;
          case I_CLAP: clap_.NoteOn(1400.0f, v.vel); break;
          case I_WHINE_BLIP:   /* a garage spin up on the spark voice, tuned low and soft */
            spark_.NoteOn(note_id_++ & 0xfff, v.freq, v.vel * 0.6f, frame_); break;
        }
      } else {
        v.at -= span;
        ev_[k++] = v;
      }
    }
    nev_ = k;
  }

  template <class Part>
  void Mix(Part& part, float dry, float dly, float pan, float* l, float* r, int from, int n) {
    if (!Live(part)) return;
    for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
    part.Process(dl_, dr_, 0, n);
    float gl = pan > 0 ? 1.0f - pan : 1.0f, gr = pan < 0 ? 1.0f + pan : 1.0f;
    for (int i = 0; i < n; ++i) {
      float a = dl_[i] * gl, b = dr_[i] * gr;
      l[from + i] += dry * a; r[from + i] += dry * b;
      sl_[i] += dly * a; sr_buf_[i] += dly * b;
    }
  }

  /* SFX gains are measured, not the reference rack's: Bellows engines differ
   * by 10x in native loudness (tools/bake_music.cpp prints the peak and RMS
   * of every patch) */
  void SfxSpan(float* l, float* r, int from, int n) {
    for (int i = 0; i < n; ++i) { sl_[i] = sr_buf_[i] = 0.0f; }
    const float g = sfx_gain_;
    /* the impact bus: kick and clangs through one tanh for glue */
    if (impact_.Active() || Live(clang_)) {
      for (int i = 0; i < n; ++i) { bl_[i] = 0.0f; br_[i] = 0.0f; }
      impact_.Process(bl_, br_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] *= 0.75f; br_[i] *= 0.75f; }
      clang_.Process(bl_, br_, 0, n);
      punch_.Process(bl_, br_, 0, n);
      for (int i = 0; i < n; ++i) {
        float a = bl_[i] * 0.9f * g, b = br_[i] * 0.9f * g;
        l[from + i] += a; r[from + i] += b;
        sl_[i] += 0.25f * a; sr_buf_[i] += 0.25f * b;
      }
    }
    Mix(spark_, 0.4f * g, 0.3f * g, 0.0f, l, r, from, n);
    Mix(buzz_, 0.7f * g, 0.0f, 0.0f, l, r, from, n);
    Mix(bells_, 0.5f * g, 0.25f * g, 0.0f, l, r, from, n);
    Mix(gong_, 0.42f * g, 0.2f * g, 0.0f, l, r, from, n);
    Mix(tick_, 3.0f * g, 0.0f, 0.1f, l, r, from, n);
    Mix(clap_, 0.8f * g, 0.3f * g, -0.1f, l, r, from, n);
    /* the whines: a level ramp across the span so throttle changes never click */
    static const float kWhinePan[4] = {-0.3f, -0.1f, 0.1f, 0.3f};
    for (int w = 0; w < 4; ++w) {
      if (!whine_on_[w]) continue;
      float g0 = whine_gain_[w];
      float g1 = g0 + (whine_target_[w] - g0) * 0.18f;
      whine_gain_[w] = g1;
      if (whine_target_[w] <= 0.0f && g1 < 0.004f) { whine_[w].NoteOff(); whine_on_[w] = false; whine_gain_[w] = 0.0f; }
      if (!whine_[w].Active()) { whine_on_[w] = false; continue; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      whine_[w].Process(dl_, dr_, 0, n);
      float gl = kWhinePan[w] > 0 ? 1.0f - kWhinePan[w] : 1.0f, gr = kWhinePan[w] < 0 ? 1.0f + kWhinePan[w] : 1.0f;
      const float wg = 0.18f * g * duck_;
      for (int i = 0; i < n; ++i) {
        float t = g0 + (g1 - g0) * (static_cast<float>(i) / static_cast<float>(n));
        l[from + i] += dl_[i] * gl * t * wg; r[from + i] += dr_[i] * gr * t * wg;
      }
    }
    /* the crowd: a swell that settles back onto the bed */
    {
      crowd_swell_ *= 0.9988f;                       /* about 1.2 s to a tenth at 128 frame spans */
      if (crowd_swell_ < 0.002f) crowd_swell_ = 0.0f;
      float target = crowd_swell_ > crowd_bed_ ? crowd_swell_ : crowd_bed_;
      float c = target > crowd_gain_ ? 0.06f : 0.012f;
      crowd_gain_ += (target - crowd_gain_) * c;
      if (crowd_gain_ > 0.001f) {
        const float cg = 1.2f * g * duck_ * crowd_gain_;
        const float lfo_inc = 0.3f * 6.2831853f / sr_;
        for (int i = 0; i < n; ++i) {
          float x = crowd_.Process();
          float y = crowd_bp1_.Process(x) + 0.35f * crowd_bp2_.Process(x);
          crowd_lfo_ += lfo_inc;
          if (crowd_lfo_ > 6.2831853f) crowd_lfo_ -= 6.2831853f;
          float trem = 0.85f + 0.15f * bellows::fm::Sin(crowd_lfo_);
          float o = y * trem * cg;
          l[from + i] += o; r[from + i] += o * 0.9f;
        }
      }
    }
    slap_.Process(sl_, sr_buf_, 0, n);
    for (int i = 0; i < n; ++i) { l[from + i] += 0.5f * sl_[i]; r[from + i] += 0.5f * sr_buf_[i]; }
#if RTR_SONGS
    if (mode_ > 0) {
      for (int i = 0; i < n; ++i) { yl_[i] = yr_[i] = 0.0f; }
      const float mg = music_gain_ * duck_ * kSongTrim[mode_];
      MusicSpan(l, r, from, n, mg);
      echo_.Process(yl_, yr_, 0, n);
      const float eg = 0.6f * mg;
      for (int i = 0; i < n; ++i) { l[from + i] += eg * yl_[i]; r[from + i] += eg * yr_[i]; }
    }
#endif
  }

#if RTR_SONGS
  enum Genre { G_GARAGE, G_FIGHT, G_RUSH };
  using String = bellows::Pluck<70>;

  void SetGenre(Genre g) {
    genre_ = g;
    acid_p_ = bellows::Va::Params();
    acid_p_.shape = 0.0f; acid_p_.detune = 0.0f; acid_p_.resonance = g == G_GARAGE ? 0.15f : 0.82f;
    acid_p_.cutoff = g == G_GARAGE ? 260.0f : 400.0f; acid_p_.env_amount = g == G_GARAGE ? 0.4f : 2.6f;
    acid_p_.attack = 0.002f; acid_p_.decay = 0.18f; acid_p_.sustain = g == G_GARAGE ? 0.7f : 0.25f; acid_p_.release = 0.05f;
    acid_p_.f_attack = 0.001f; acid_p_.f_decay = 0.14f; acid_p_.f_sustain = 0.0f; acid_p_.f_release = 0.05f;
    acid_p_.sub = g == G_GARAGE ? 0.8f : 0.2f;
    acid_.Init(sr_, &rng_acid_, acid_p_);

    bellows::Fm::Params lead;
    bellows::Kick::Params kp;
    bellows::Snare::Params sp;
    bellows::Hat::Params hp;
    if (g == G_GARAGE) {
      /* a soft glassy bell lead for the shelf */
      lead.ops = 2; lead.algorithm = 1; lead.feedback = 0.0f; lead.brightness = 0.45f;
      lead.ratio[0] = 1; lead.ratio[1] = 3.5f; lead.level[1] = 0.5f;
      lead.attack = 0.01f; lead.decay = 1.2f; lead.sustain = 0.25f; lead.release = 0.8f;
      kp.decay = 0.25f; kp.drive = 1.2f; kp.pitch_decay = 0.06f;
      sp.decay = 0.16f; sp.tone = 0.5f; sp.snap = 0.2f;
      hp.decay = 0.035f; hp.tone = 1.2f;
    } else {
      /* a hard saw-ish FM lead with feedback, heavy drums */
      lead.ops = 2; lead.algorithm = 1; lead.feedback = g == G_RUSH ? 0.65f : 0.55f; lead.brightness = 0.6f;
      lead.ratio[0] = 1; lead.ratio[1] = 1; lead.level[1] = 0.7f;
      lead.attack = 0.003f; lead.decay = 0.25f; lead.sustain = 0.55f; lead.release = 0.08f;
      kp.decay = 0.4f; kp.pitch_decay = 0.06f; kp.drive = g == G_RUSH ? 4.0f : 3.2f; kp.click_tune = 7.0f;
      sp.decay = 0.2f; sp.tone = 0.45f; sp.snap = 0.28f;
      hp.decay = 0.04f; hp.tone = 1.25f;
    }
    lead_.Init(sr_, lead);
    kick_.Init(sr_, kp);
    snare_.Init(sr_, &rng_snare_, sp);
    hat_.Init(sr_, hp);
  }

  void ApplyMode() {
    mode_ = pending_mode_;
    pending_mode_ = -1;
    section_ = 0; bar_ = 0; step_ = 0; pass_ = 0; countdown_ = 0; lead_off_ = 0; acid_gate_ = 0;
    mask_ = 0xffff; hold_ = false;
    acid_.NoteOff(); lead_.NoteOff(); counter_.NoteOff(); PadRelease();
    if (mode_ <= 0) { mode_ = 0; return; }
    samples_per_step_ = StepSamples(mode_, sr_);
    SetGenre(mode_ == 3 ? G_RUSH : mode_ == 2 ? G_FIGHT : G_GARAGE);
    bellows::StereoDelay<500>::Params d;
    float beat = 60.0f / kBpm[mode_];
    d.time_l = beat * 0.75f; d.time_r = beat * 0.5f; d.feedback = mode_ == 1 ? 0.48f : 0.36f;
    d.cross_feedback = 0.15f; d.damping = 3200.0f; d.mix = 1.0f;
    echo_.SetParams(d);
    last_chord_ = -1;
  }

  const Section& Current(int* count) const {
    if (mode_ == 3) { *count = kOvertimeN; return overtime_[section_]; }
    if (mode_ == 2) { *count = kArenaN; return arena_[section_]; }
    *count = kGarageN; return garage_[section_];
  }

  void PadRelease() { for (int i = 0; i < npad_; i++) pad_.NoteOff(pad_ids_[i]); npad_ = 0; }

  /* smooth voice leading: each chord tone goes to the octave nearest the last voicing */
  void PadChord(int root, bool minor, float vel) {
    int tones[3] = {root, (root + (minor ? 3 : 4)) % 12, (root + 7) % 12};
    PadRelease();
    float center = pad_center_, sum = 0;
    for (int k = 0; k < 3; k++) {
      int best = 0;
      float bd = 1e9f;
      for (int oct = 3; oct <= 5; oct++) {
        int m = 12 * (oct + 1) + tones[k];
        if (m < 50 || m > 76) continue;
        float d = fabsf(static_cast<float>(m) - center);
        if (d < bd) { bd = d; best = m; }
      }
      if (!best) best = 60 + tones[k];
      int id = 1000 + (note_id_++ & 0xfff);
      pad_.NoteOn(id, Mtof(static_cast<float>(best)), vel, frame_);
      pad_ids_[npad_++] = id;
      sum += static_cast<float>(best);
    }
    pad_center_ = sum / 3.0f;
  }

  void Step() {
    if (pending_mode_ >= 0 && step_ % 4 == 0) ApplyMode();   /* OVERTIME lands on a beat */
    if (mode_ <= 0) return;
    int count;
    const Section* sec = &Current(&count);
    const int s = step_ % 16;
    if (s == 0 && step_ > 0) {
      if (++bar_ >= sec->def->bars) {
        bar_ = 0;
        if (hold_) pass_++;
        else if (++section_ >= count) { section_ = kLoop[mode_]; pass_++; }
        sec = &Current(&count);
      }
    }
    const uint16_t lay = sec->def->layers & mask_;
    const bool last_bar = bar_ == sec->def->bars - 1;
    const bool intro = mode_ == 2 && section_ == 0;
    const int root = sec->roots[bar_ % 8];
    const bool minor = sec->minor[bar_ % 8];
    const float jit = 0.06f * rng_jit_.Next();
    const float soft = mode_ == 1 ? 0.6f : 1.0f;

    /* drums */
    bool rolling = (lay & L_ROLL) && last_bar;
    if (rolling) {
      float up = 0.5f + 0.5f * s / 16.0f;
      snare_.NoteOn(190.0f, up);
    }
    if ((lay & L_KICK) && s % 4 == 0 && !(rolling && s >= 12)) kick_.NoteOn(50.0f, 0.95f * soft);
    if (lay & L_BREAK) {
      if (BRK_K[s] > 0) kick_.NoteOn(50.0f, BRK_K[s]);
      if (BRK_S[s] > 0 && !rolling) snare_.NoteOn(190.0f, BRK_S[s] + jit);
      if (genre_ == G_RUSH && (s == 6 || s == 14)) kick_.NoteOn(50.0f, 0.7f);
      if (last_bar && s >= 12 && !rolling) snare_.NoteOn(190.0f, 0.35f + 0.1f * (s - 12));   /* fill */
    }
    if (lay & L_HAT) {
      if (mode_ == 1) {
        if (s == 6 || s == 14) ohat_.NoteOn(330.0f, 0.4f + jit);        /* sparse: the offbeat open hat */
        if (s % 4 == 0) hat_.NoteOn(330.0f, 0.3f);
      } else if (intro) {
        if (s % 4 == 2) ohat_.NoteOn(330.0f, 0.5f + jit);
        if (s % 4 == 0) hat_.NoteOn(330.0f, 0.25f);
      } else {
        if (s % 4 == 2) ohat_.NoteOn(330.0f, 0.4f + jit);
        else hat_.NoteOn(330.0f, (s % 2 ? 0.28f : 0.4f) + jit);
      }
    }
    if ((lay & L_PERC) && (s == 4 || s == 12)) {
      Push(0, I_CLAP, 0, 0.55f); Push(10, I_CLAP, 0, 0.45f); Push(21, I_CLAP, 0, 0.35f);
    }

    /* the acid line, its filter riding a slow sine; the intro opens up */
    if ((lay & L_ACID) || (lay & L_SUB)) {
      float cut;
      if (lay & L_SUB) cut = 260.0f;
      else if (intro) cut = 220.0f + 1700.0f * (bar_ * 16 + s) / 128.0f;
      else cut = 450.0f + 1500.0f * (0.5f + 0.5f * sinf((bar_ * 16 + s) * 6.2831853f / 128.0f));
      acid_p_.cutoff = cut;
      acid_p_.env_amount = (lay & L_SUB) ? 0.4f : (ACID_A[s] ? 3.2f : 2.0f);
      acid_.SetParams(acid_p_);
      int base = 36 + root;
      if (base > 47) base -= 12;
      if (lay & L_SUB) {
        if (s == 0) acid_.NoteOn(Mtof(static_cast<float>(base)), 0.6f);
        if (s == 12) acid_.NoteOff();
      } else if (ACID_N[s] >= 0) {
        acid_.NoteOn(Mtof(static_cast<float>(base + ACID_N[s])), ACID_A[s] ? 0.95f : 0.7f);
        acid_gate_ = (s < 15 && ACID_N[s + 1] < 0) ? 2 : 1;
      }
    }
    if (acid_gate_ > 0 && --acid_gate_ == 0 && !(lay & L_SUB)) acid_.NoteOff();

    /* pad: one held chord per bar, voice led */
    int chord_id = bar_ * 100 + section_;
    if ((lay & L_PAD) && s == 0 && chord_id != last_chord_) { PadChord(root, minor, 0.62f); last_chord_ = chord_id; }
    if (!(lay & L_PAD) && npad_) PadRelease();

    /* the arpeggio: a slow rising four on the shelf, three against four in a fight */
    if (lay & L_HYPNO) {
      int third = root + (minor ? 3 : 4), fifth = root + 7;
      if (mode_ == 1) {
        if (s % 4 == 0) {
          int fig[4] = {root, third, fifth, root + 12};
          int m = 60 + ((fig[(s / 4) % 4] + 12) % 12) + ((s / 4) % 4 == 3 ? 12 : 0);
          hypno_.NoteOn(note_id_++ & 0xfff, Mtof(static_cast<float>(m)), 0.45f + jit, frame_);
        }
      } else {
        int fig[3] = {root + 12, fifth, third + 12};
        int m = 64 + ((fig[step_ % 3] - 4 + 24) % 24);
        float acc = s % 4 == 0 ? 0.6f : 0.42f;
        hypno_.NoteOn(note_id_++ & 0xfff, Mtof(static_cast<float>(m)), acc + jit, frame_);
      }
    }

    /* rave stabs: a minor seventh on the downbeat and the offbeat after it */
    if ((lay & L_STAB) && (s == 0 || s == 6 || (bar_ % 2 == 1 && s == 10))) {
      int ch[4] = {root, root + (minor ? 3 : 4), root + 7, root + (minor ? 10 : 11)};
      for (int k = 0; k < 4; k++) stab_.NoteOn(note_id_++ & 0xfff, Mtof(static_cast<float>(60 + (ch[k] + 8) % 12 + 4)), 0.55f, frame_);
    }

    /* the written lead, and a counter line a third below where the section asks */
    if (lay & L_LEAD) {
      int at = bar_ * 16 + s;
      for (int k = 0; k < sec->nlead; k++) {
        const Note& nt = sec->lead[k];
        if (nt.step == at) {
          if (nt.midi) {
            lead_.NoteOn(Mtof(static_cast<float>(nt.midi)), mode_ == 1 ? 0.5f : 0.7f);
            if (lay & L_COUNTER) counter_.NoteOn(Mtof(static_cast<float>(DiatonicBelow(nt.midi) - 12)), 0.5f);
          } else { lead_.NoteOff(); counter_.NoteOff(); }
          lead_off_ = nt.dur;
        }
      }
      if (lead_off_ > 0 && --lead_off_ == 0) { lead_.NoteOff(); counter_.NoteOff(); }
    } else if (s == 0) { lead_.NoteOff(); counter_.NoteOff(); }
    ++step_;
  }

  template <class Part>
  void MixM(Part& part, float dry, float dly, float pan, float* l, float* r, int from, int n) {
    if (!Live(part)) return;
    for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
    part.Process(dl_, dr_, 0, n);
    float gl = pan > 0 ? 1.0f - pan : 1.0f, gr = pan < 0 ? 1.0f + pan : 1.0f;
    for (int i = 0; i < n; ++i) {
      float a = dl_[i] * gl, b = dr_[i] * gr;
      l[from + i] += dry * a; r[from + i] += dry * b;
      yl_[i] += dly * a; yr_[i] += dly * b;
    }
  }

  /* the song parts into l and r with the echo send in yl_ yr_; master is the
   * music level (1.0 in the bake), the stem gains ramp over about 6 frames */
  void MusicSpan(float* l, float* r, int from, int n, float master) {
    g_base_ += (g_base_t_ - g_base_) * 0.12f;
    g_drums_ += (g_drums_t_ - g_drums_) * 0.12f;
    g_lead_ += (g_lead_t_ - g_lead_) * 0.12f;
    const float gd = master * g_drums_, gb = master * g_base_, gl = master * g_lead_;
    if (kick_.Active() || snare_.Active() || hat_.Active() || ohat_.Active()) {
      /* the drum bus: kick, snare and hats through one tanh, so the kick's
       * peaks stop ruling the ceiling and the kit glues */
      for (int i = 0; i < n; ++i) { bl_[i] = 0.0f; br_[i] = 0.0f; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      kick_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] += 0.8f * dl_[i]; br_[i] += 0.8f * dr_[i]; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      snare_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] += 1.3f * dl_[i]; br_[i] += 1.3f * dr_[i]; yl_[i] += 0.25f * gd * dl_[i]; yr_[i] += 0.25f * gd * dr_[i]; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      hat_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] += 3.5f * dl_[i] * 0.82f; br_[i] += 3.5f * dr_[i]; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      ohat_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] += 2.0f * dl_[i]; br_[i] += 2.0f * dr_[i] * 0.88f; }
      drum_sat_.Process(bl_, br_, 0, n);
      for (int i = 0; i < n; ++i) { l[from + i] += gd * bl_[i]; r[from + i] += gd * br_[i]; }
    }
    if (acid_.Active()) {
      for (int i = 0; i < n; ++i) { bl_[i] = 0.0f; br_[i] = 0.0f; }
      acid_.Process(bl_, br_, 0, n);
      acid_sat_.Process(bl_, br_, 0, n);
      const float ag = (mode_ == 1 ? 0.6f : 3.0f) * gb;
      const float as = mode_ == 1 ? 0.0f : 0.4f * gb;
      for (int i = 0; i < n; ++i) {
        l[from + i] += ag * bl_[i]; r[from + i] += ag * br_[i];
        yl_[i] += as * bl_[i]; yr_[i] += as * br_[i];
      }
    }
    MixM(pad_, 0.03f * gb, 0.012f * gb, 0.0f, l, r, from, n);
    MixM(stab_, 0.7f * gb, 0.4f * gb, 0.0f, l, r, from, n);
    MixM(hypno_, 0.5f * gb, 0.9f * gb, -0.22f, l, r, from, n);
    if (lead_.Active()) {
      for (int i = 0; i < n; ++i) { bl_[i] = 0.0f; br_[i] = 0.0f; }
      lead_.Process(bl_, br_, 0, n);
      chorus_.Process(bl_, br_, 0, n);
      const float lg = 2.5f * gl, ls = 1.0f * gl;
      for (int i = 0; i < n; ++i) {
        l[from + i] += lg * bl_[i]; r[from + i] += lg * br_[i];
        yl_[i] += ls * bl_[i]; yr_[i] += ls * br_[i];
      }
    }
    MixM(counter_, 0.5f * gl, 0.4f * gl, 0.24f, l, r, from, n);
  }
#endif

  /* SFX voices */
  bellows::Rng rng_clang_, rng_spark_, rng_buzz_, rng_bells_, rng_crowd_, rng_whine_, rng_kit_;
  bellows::VoicePool<bellows::Modal, 4> clang_;
  bellows::Kick impact_;
  bellows::VoicePool<bellows::Va, 4> spark_;
  bellows::Va buzz_;
  bellows::VoicePool<bellows::Fm, 4> bells_;
  bellows::Fm gong_;
  bellows::Va whine_[4];
  float whine_gain_[4] = {}, whine_target_[4] = {};
  bool whine_on_[4] = {};
  bellows::NoiseGen crowd_;
  bellows::Svf crowd_bp1_, crowd_bp2_;
  float crowd_lfo_ = 0.0f, crowd_gain_ = 0.0f, crowd_swell_ = 0.0f, crowd_bed_ = 0.0f;
  bellows::Hat tick_;
  bellows::Snare clap_;
  bellows::StereoDelay<300> slap_;
  bellows::Saturator<1> punch_;
  bellows::Compressor<> comp_;
  bellows::Limiter<> limiter_;

#if RTR_SONGS
  bellows::Rng rng_snare_, rng_acid_, rng_str_, rng_ctr_, rng_jit_;
  bellows::Kick kick_;
  bellows::Snare snare_;
  bellows::Hat hat_, ohat_;
  bellows::Va acid_, counter_;
  bellows::Va::Params acid_p_;
  bellows::VoicePool<bellows::Fm, 6> pad_;
  bellows::VoicePool<bellows::Fm, 4> stab_;
  bellows::VoicePool<String, 4> hypno_;
  bellows::Fm lead_;
  bellows::StereoDelay<500> echo_;
  bellows::Chorus<> chorus_;
  bellows::Saturator<1> acid_sat_, drum_sat_;
  Section garage_[kGarageN];
  Section arena_[kArenaN];
  Section overtime_[kOvertimeN];
  Genre genre_ = G_GARAGE;
  uint16_t mask_ = 0xffff;
  bool hold_ = false;
  int mode_ = 0, pending_mode_ = -1, samples_per_step_ = 1, countdown_ = 0;
  int step_ = 0, bar_ = 0, section_ = 0, pass_ = 0, acid_gate_ = 0, lead_off_ = 0, last_chord_ = -1;
  int pad_ids_[8] = {};
  int npad_ = 0;
  float pad_center_ = 62.0f;
  float g_base_ = 1.0f, g_drums_ = 1.0f, g_lead_ = 1.0f, g_base_t_ = 1.0f, g_drums_t_ = 1.0f, g_lead_t_ = 1.0f;
  float yl_[kBlock] = {}, yr_[kBlock] = {};
#endif

  float dl_[kBlock] = {}, dr_[kBlock] = {}, sl_[kBlock] = {}, sr_buf_[kBlock] = {}, bl_[kBlock] = {}, br_[kBlock] = {};
  Ev ev_[kMaxEv];
  int nev_ = 0;
  int buzz_gen_ = 0;
  bool buzz_live_ = false;
  int note_id_ = 0;
  uint32_t frame_ = 0;
  float sr_ = 48000.0f;
  float sfx_gain_ = 0.8f, music_gain_ = 0.6f, duck_ = 1.0f;
  bool ready_ = false;
};

Band band;
int pending_sfx[16][2];
int npending;
float L[4096], R[4096];
float band_rate = 48000.0f;

}  // namespace

extern "C" void audio_band_init(uint32_t rate) {
  band_rate = static_cast<float>(rate ? rate : 48000);
  band.Init(band_rate);
  for (int i = 0; i < npending; ++i) band.Sfx(pending_sfx[i][0], pending_sfx[i][1]);
  npending = 0;
}

extern "C" void audio_band_levels(int sfx, int music) { band.SetLevels(sfx, music); }
extern "C" void audio_band_duck(float gain) { band.SetDuck(gain); }
extern "C" void audio_band_crowd_bed(float level) { band.SetCrowdBed(level); }

extern "C" void audio_play(int sfx) { audio_play_var(sfx, SFXV_DEFAULT); }

extern "C" void audio_play_var(int sfx, int var) {
  if (sfx < 0 || sfx >= SFX_COUNT_) return;
  if (band.Ready()) band.Sfx(sfx, var);
  else if (npending < 16) { pending_sfx[npending][0] = sfx; pending_sfx[npending][1] = var; npending++; }
}

extern "C" void audio_whine(int slot, float level, float pitch) { if (band.Ready()) band.Whine(slot, level, pitch); }
extern "C" void audio_crowd(float level) { if (band.Ready()) band.Crowd(level); }
extern "C" void audio_part(int row, int option) { if (band.Ready()) band.Part(row, option); }

extern "C" void audio_band_music(int mode) {
#if RTR_SONGS
  if (band.Ready()) band.SetMode(mode);
#else
  (void)mode;
#endif
}

extern "C" void audio_band_stems(float base, float drums, float lead) {
#if RTR_SONGS
  band.SetStems(base, drums, lead);
#else
  (void)base; (void)drums; (void)lead;
#endif
}

extern "C" int audio_song_step(int mode, uint32_t rate) { return StepSamples(mode, static_cast<float>(rate ? rate : 48000)); }
extern "C" int audio_song_sections(int mode) { int n; FormOf(mode, &n); return mode >= 1 && mode <= 3 ? n : 0; }
extern "C" int audio_song_bars(int mode, int section) {
  int n; const SectionDef* f = FormOf(mode, &n);
  if (mode < 1 || mode > 3 || section < 0 || section >= n) return 0;
  return f[section].bars;
}
extern "C" int audio_song_loop(int mode) { return mode >= 1 && mode <= 3 ? kLoop[mode] : 0; }

/* the band over frames [0, n) of an interleaved stereo buffer that already
 * holds the PCM stems; the master compressor and limiter run over the sum */
extern "C" void audio_band_render(float *stereo, int frames, uint32_t rate) {
  if (!band.Ready()) audio_band_init(rate);
  int done = 0;
  while (done < frames) {
    int chunk = frames - done > 4096 ? 4096 : frames - done;
    for (int i = 0; i < chunk; ++i) { L[i] = stereo[(done + i) * 2]; R[i] = stereo[(done + i) * 2 + 1]; }
    band.Render(L, R, chunk);
    for (int i = 0; i < chunk; ++i) { stereo[(done + i) * 2] = L[i]; stereo[(done + i) * 2 + 1] = R[i]; }
    done += chunk;
  }
}

#ifdef MUSIC_BAKE
extern "C" void audio_bake_begin(int mode, int section, int stem, int hold) {
  band.Init(band_rate);
  uint16_t mask = stem == STEM_BASE ? MASK_BASE : stem == STEM_DRUMS ? MASK_DRUMS : stem == STEM_LEAD ? MASK_LEAD : 0xffff;
  band.BakeBegin(mode, section, mask, hold != 0);
}
extern "C" void audio_bake_render(float *stereo, int frames) {
  int done = 0;
  while (done < frames) {
    int chunk = frames - done > 4096 ? 4096 : frames - done;
    for (int i = 0; i < chunk; ++i) { L[i] = 0.0f; R[i] = 0.0f; }
    band.RenderMusic(L, R, chunk);
    for (int i = 0; i < chunk; ++i) { stereo[(done + i) * 2] = L[i]; stereo[(done + i) * 2 + 1] = R[i]; }
    done += chunk;
  }
}
#endif
