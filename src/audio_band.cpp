/* audio_band.cpp: RED TAG RUMBLE's Bellows band (spec 11.2, 11.3).
 *
 * Two jobs in one translation unit so the bake and the cart never drift:
 *
 *   1. The live SFX band, always compiled: twelve patches on metal (a Modal
 *      bar pool for clangs), a dedicated impact kick, a square noise burst for
 *      sparks and saw contact, a buzzer for horns and klaxons, FM bells for
 *      the menus and a gong for the KO, four continuous spinner whines, and a
 *      crowd bed of filtered pink noise that swells on big hits. Everything
 *      two players can fire at once is pooled. A slap delay, no plate. Every
 *      stinger is tuned to the songs' palette (E minor: E G B D, the impacts
 *      on E1) and its follow-up events sit on the fight's sixteenth grid, so
 *      a hit lands in key and in time with the music under it.
 *
 *   2. The songs, compiled only with MUSIC_BAKE (tools/bake_music.cpp renders
 *      them to assets/music/NAME.wav) or MUSIC_LIVE (composing on the Mac, the
 *      cart plays them live). A form of sections, a chord root per bar, a
 *      layer mask per section, a drum pattern and a bass pattern per section;
 *      a 16 step conductor plays the kit, the metal, the distorted bass, the
 *      stabs, the pad, the riser riff and the sirens from those. Everything
 *      is in E, Phrygian flavoured (the F and the Bb in the bass) so it stays
 *      dark; the stingers share the pitch set.
 *
 *        1 GARAGE    "Shelf Life"   100 bpm   a low industrial groove: heavy kick, anvil and clinks,
 *                                             distorted sub bass, sparse stabs, a drone on the B half
 *        2 ARENA     "Red Tag"      156 bpm   a half time stomp verse and a breakbeat chorus over one
 *                                             distorted bass riff, metal on the off beats, a riser riff
 *                                             stem that the game fades in for the last 20 s or low HP
 *        3 OVERTIME  "Doors Lock"   172 bpm   a pulsing eighth note bass whose filter climbs the whole
 *                                             loop, a gated noise riser, two tritone sirens, a roll
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

/* -- the songs (data) ---------------------------------------------- */

enum Layer : uint16_t {
  L_BASS = 1,      /* the distorted bass riff */
  L_KICK = 2,
  L_SNARE = 4,
  L_HAT = 8,
  L_METAL = 16,    /* clinks and the anvil */
  L_STAB = 32,     /* square stabs through a saturator */
  L_PAD = 64,      /* a low detuned drone, one chord per bar */
  L_LEAD = 128,    /* the riser riff */
  L_SIREN = 256,   /* two sweeping saws a tritone apart */
  L_RISER = 512,   /* gated pink noise through a climbing bandpass */
  L_ROLL = 1024,   /* snare roll over the back half of the last bar */
};

#ifdef MUSIC_BAKE
/* the stems: the kit, the metal and the stabs are the drums stem (slow motion
 * keeps it alone); the riff is the lead stem; everything else is the base */
constexpr uint16_t MASK_DRUMS = L_KICK | L_SNARE | L_HAT | L_METAL | L_STAB | L_ROLL;
constexpr uint16_t MASK_LEAD = L_LEAD;
constexpr uint16_t MASK_BASE = static_cast<uint16_t>(~(MASK_DRUMS | MASK_LEAD));
#endif

/* drum patterns: velocity per sixteenth, 0 is none. k kick, s snare, h closed
 * hat, o open hat, m metal clink, a anvil */
struct DrumPat { float k[16], s[16], h[16], o[16], m[16], a[16]; };
enum { P_GARAGE, P_STOMP, P_BREAK, P_RUSH, P_COUNT };
const DrumPat DRUMS[P_COUNT] = {
  /* GARAGE: the kick on one, the and of two and three; snare on two and four
   * doubled by the anvil; off beat hats; clinks scattered */
  {{1, 0, 0, 0, 0, 0, .8f, 0, .9f, 0, 0, 0, 0, 0, 0, 0},
   {0, 0, 0, 0, .9f, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0},
   {0, 0, .35f, 0, 0, 0, .35f, 0, 0, 0, .35f, 0, 0, 0, .35f, 0},
   {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
   {0, 0, .3f, 0, 0, 0, 0, .4f, 0, 0, .5f, 0, 0, 0, 0, .3f},
   {0, 0, 0, 0, .6f, 0, 0, 0, 0, 0, 0, 0, .8f, 0, 0, 0}},
  /* STOMP: half time, kick on one and the and of three, snare on three with
   * the anvil, clinks on every off beat */
  {{1, 0, 0, .5f, 0, 0, 0, 0, 0, 0, .9f, 0, 0, 0, 0, 0},
   {0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, .3f, 0},
   {.45f, 0, .3f, 0, 0, 0, .3f, 0, .45f, 0, .3f, 0, 0, 0, .3f, 0},
   {0, 0, 0, 0, .5f, 0, 0, 0, 0, 0, 0, 0, .5f, 0, 0, 0},
   {0, 0, .5f, 0, 0, 0, .35f, 0, 0, 0, .5f, 0, 0, 0, .35f, 0},
   {0, 0, 0, 0, 0, 0, 0, 0, .8f, 0, 0, 0, 0, 0, 0, 0}},
  /* BREAK: the breakbeat, sixteenth hats with the open hat on the off beats,
   * metal on the and of two and four, the anvil under the backbeat */
  {{1, 0, .7f, 0, 0, 0, 0, 0, 0, 0, 1, .6f, 0, 0, 0, 0},
   {0, 0, 0, 0, 1, 0, 0, .3f, 0, .25f, 0, 0, 1, 0, 0, .4f},
   {.4f, .28f, 0, .28f, .4f, .28f, 0, .28f, .4f, .28f, 0, .28f, .4f, .28f, 0, .28f},
   {0, 0, .45f, 0, 0, 0, .45f, 0, 0, 0, .45f, 0, 0, 0, .45f, 0},
   {0, 0, 0, 0, 0, 0, .6f, 0, 0, 0, 0, 0, 0, 0, .6f, 0},
   {0, 0, 0, 0, .6f, 0, 0, 0, 0, 0, 0, 0, .6f, 0, 0, 0}},
  /* RUSH: four on the floor with a pushed kick, backbeat snare and ghosts,
   * sixteenth hats, open on the off beats */
  {{1, 0, 0, 0, 1, 0, .6f, 0, 1, 0, 0, 0, 1, 0, .6f, 0},
   {0, 0, 0, 0, .9f, 0, 0, .3f, 0, 0, 0, 0, 1, 0, 0, .35f},
   {.42f, .3f, 0, .3f, .42f, .3f, 0, .3f, .42f, .3f, 0, .3f, .42f, .3f, 0, .3f},
   {0, 0, .45f, 0, 0, 0, .45f, 0, 0, 0, .45f, 0, 0, 0, .45f, 0},
   {0, 0, .5f, 0, 0, 0, 0, 0, 0, 0, .5f, 0, 0, 0, 0, 0},
   {0, 0, 0, 0, .5f, 0, 0, 0, 0, 0, 0, 0, .7f, 0, 0, 0}},
};

/* bass patterns: semitones over the bar's root (-1 rest) and the gate in
 * sixteenths */
struct BassPat { int8_t n[16]; uint8_t g[16]; };
enum { B_GARAGE, B_FIGHT, B_PULSE, B_COUNT };
const BassPat BASSES[B_COUNT] = {
  /* GARAGE: root, root, root, the minor third, the flat two, root */
  {{0, -1, -1, 0, -1, -1, 0, -1, 3, -1, 1, -1, 0, -1, -1, -1},
   {3, 0, 0, 1, 0, 0, 2, 0, 1, 0, 2, 0, 3, 0, 0, 0}},
  /* FIGHT: a stomping sixteenth riff with the tritone on the and of three
   * and the flat two pushing into the next bar */
  {{0, -1, 0, 0, -1, -1, 0, 0, 3, -1, 6, -1, 0, -1, 1, 1},
   {2, 0, 1, 1, 0, 0, 1, 1, 1, 0, 2, 0, 2, 0, 1, 1}},
  /* PULSE: every eighth, a push on the last sixteenth */
  {{0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, 0},
   {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 1}},
};

struct SectionDef { const char* tag; int bars; const char* chords; uint16_t layers; int drums, bass; };

constexpr uint16_t KIT = L_KICK | L_SNARE | L_HAT | L_METAL;
const SectionDef GARAGE[] = {
  {"A", 8, "Em Em Em Em Em Em F F", L_BASS | KIT | L_STAB, P_GARAGE, B_GARAGE},
  {"B", 8, "Em Em Em Em G G F F", L_BASS | KIT | L_STAB | L_PAD, P_GARAGE, B_GARAGE},
};
/* the two ARENA sections share chords, bass and layers outside the kit so the
 * one baked base stem is true under both */
const SectionDef ARENA[] = {
  {"STOMP", 8, "Em Em Em Em Em Em G F", L_BASS | KIT | L_LEAD, P_STOMP, B_FIGHT},
  {"BREAK", 8, "Em Em Em Em Em Em G F", L_BASS | KIT | L_STAB | L_LEAD | L_ROLL, P_BREAK, B_FIGHT},
};
const SectionDef OVERTIME[] = {
  {"RUSH", 8, "Em Em F F Em Em G F", L_BASS | KIT | L_STAB | L_SIREN | L_RISER | L_ROLL, P_RUSH, B_PULSE},
};
constexpr int kGarageN = sizeof GARAGE / sizeof GARAGE[0];
constexpr int kArenaN = sizeof ARENA / sizeof ARENA[0];
constexpr int kOvertimeN = sizeof OVERTIME / sizeof OVERTIME[0];
constexpr int kLoop[4] = {0, 0, 0, 0};
constexpr float kBpm[4] = {0.0f, 100.0f, 156.0f, 172.0f};
/* the trims tools/bake_music.cpp measured (its "gains" line: the full mix of
 * each song at the ceiling) times the mode trims audio_pcm.c applies to the
 * stems, so the live songs sit where the baked stems do; re-copy after any
 * patch change */
constexpr float kSongTrim[4] = {0.0f, 0.440f, 0.277f, 0.355f};

/* the fight's grid for the stingers: a beat, an eighth, a sixteenth in ms */
constexpr int kBeatMs = static_cast<int>(60000.0f / kBpm[2] + 0.5f);
constexpr int kEighthMs = kBeatMs / 2;
constexpr int kSixteenthMs = kBeatMs / 4;

/* the riser riff: a two bar motif in semitones over E4, transposed up through
 * the chord tones every two bars (E, G, B, E) while its filter opens */
struct Note { int16_t step; int8_t semis; uint8_t dur; };
const Note RIFF[] = {
  {0, 0, 2}, {3, 0, 1}, {4, 3, 2}, {6, 5, 2}, {8, 7, 2}, {10, 10, 2}, {12, 7, 2}, {14, 5, 1}, {15, 3, 1},
  {16, 0, 2}, {19, 0, 1}, {20, 3, 2}, {22, 7, 2}, {24, 12, 4}, {28, 10, 2}, {30, 7, 2},
};
constexpr int kRiffN = sizeof RIFF / sizeof RIFF[0];
constexpr int RIFF_T[4] = {0, 3, 7, 12};

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
struct Section {
  const SectionDef* def;
  int roots[8];
  bool minor[8];
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
}
#endif

/* the stinger palette: E minor chord tones (E G B D) above a base E, with a
 * hair of detune from the same draw so repeats never read as one sample */
float Pal(int base_e, float r) {
  static const int kTone[4] = {0, 3, 7, 10};
  int i = static_cast<int>(r * 4.0f) & 3;
  return static_cast<float>(base_e + kTone[i]) + (r * 4.0f - static_cast<float>(i)) * 0.4f - 0.2f;
}
constexpr float kImpactHz = 41.2f;   /* E1 */

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
    rng_bass_.Init("rtr::bass");
    rng_metal_.Init("rtr::metal");
    rng_stab_.Init("rtr::stab");
    rng_pad_.Init("rtr::pad");
    rng_lead_.Init("rtr::lead");
    rng_siren_.Init("rtr::siren");
    rng_riser_.Init("rtr::riser");
    rng_jit_.Init("rtr::jitter");

    kick_.Init(sr);
    snare_.Init(sr, &rng_snare_);
    hat_.Init(sr);
    ohat_.Init(sr);
    bellows::Modal::Params clink;
    clink.material = 0.0f; clink.decay = 0.35f; clink.strike_hardness = 0.95f; clink.brightness = 0.75f; clink.level = 0.6f;
    for (int i = 0; i < 4; ++i) metal_.at(i).Init(sr, &rng_metal_, clink);
    bellows::Modal::Params anvil;
    anvil.material = 0.0f; anvil.decay = 0.9f; anvil.strike_hardness = 0.9f; anvil.brightness = 0.45f; anvil.level = 0.6f;
    anvil_.Init(sr, &rng_metal_, anvil);
    bass_.Init(sr, &rng_bass_);
    bellows::Va::Params stb;
    stb.shape = 1.0f; stb.detune = 18.0f; stb.cutoff = 300.0f; stb.resonance = 0.3f; stb.filter_type = 1.0f; stb.env_amount = 2.8f;
    stb.attack = 0.002f; stb.decay = 0.22f; stb.sustain = 0.0f; stb.release = 0.08f;
    stb.f_attack = 0.001f; stb.f_decay = 0.11f; stb.f_sustain = 0.0f; stb.f_release = 0.08f;
    for (int i = 0; i < 4; ++i) stab_.at(i).Init(sr, &rng_stab_, stb);
    bellows::Va::Params pad;
    pad.shape = 0.0f; pad.detune = 22.0f; pad.cutoff = 420.0f; pad.resonance = 0.15f; pad.filter_type = 1.0f; pad.env_amount = 0.0f;
    pad.attack = 0.8f; pad.decay = 1.0f; pad.sustain = 0.8f; pad.release = 1.2f; pad.vel_level = 0.0f;
    for (int i = 0; i < 3; ++i) pad_.at(i).Init(sr, &rng_pad_, pad);
    lead_.Init(sr, &rng_lead_);
    bellows::Va::Params sir;
    sir.shape = 0.0f; sir.detune = 0.0f; sir.cutoff = 2200.0f; sir.resonance = 0.2f; sir.filter_type = 1.0f;
    sir.attack = 0.3f; sir.decay = 0.1f; sir.sustain = 1.0f; sir.release = 0.3f; sir.vel_level = 0.0f;
    for (int i = 0; i < 2; ++i) siren_[i].Init(sr, &rng_siren_, sir);
    riser_.Init(sr, bellows::NoiseColor::kPink, &rng_riser_);
    riser_bp_.Init(sr); riser_bp_.SetMode(bellows::SvfMode::kBp); riser_bp_.Set(300.0f, 2.0f);
    bellows::StereoDelay<500>::Params d;
    d.time_l = 0.29f; d.time_r = 0.19f; d.feedback = 0.36f; d.cross_feedback = 0.15f; d.damping = 3000.0f; d.mix = 1.0f;
    echo_.Init(sr, d);
    bellows::Chorus<>::Params ch; ch.rate = 0.9f; ch.depth = 0.4f; ch.mix = 0.3f;
    chorus_.Init(sr, ch);
    bellows::Saturator<2>::Params bs; bs.drive = 6.0f; bs.tone = 0.4f;
    bass_sat_.Init(sr, bs);
    bellows::Saturator<1>::Params ss; ss.drive = 3.0f; ss.tone = 0.1f;
    stab_sat_.Init(sr, ss);
    bellows::Saturator<1>::Params ls; ls.drive = 2.5f; ls.tone = 0.15f;
    lead_sat_.Init(sr, ls);
    bellows::Saturator<1>::Params ds; ds.drive = 1.8f; ds.tone = 0.0f;
    drum_sat_.Init(sr, ds);
    /* a fresh band after a bake render: the delay lines empty, every held
     * voice let go (a Va keeps its envelope stage across Init, so a siren
     * left sounding would leak into the next stem) */
    echo_.Reset();
    siren_[0].NoteOff(); siren_[1].NoteOff(); bass_.NoteOff(); lead_.NoteOff();
    for (int i = 0; i < 3; ++i) pad_.at(i).NoteOff();
    mode_ = 0; pending_mode_ = -1; mask_ = 0xffff; hold_ = false;
    g_base_ = g_base_t_ = 1.0f; g_drums_ = g_drums_t_ = 1.0f; g_lead_ = g_lead_t_ = 1.0f;
    siren_on_ = false; siren_rel_ = 0; npad_ = 0; cur_lay_ = 0; riser_pulse_ = 0.0f; siren_ph_[0] = 0.0f; siren_ph_[1] = 0.0f;
    bass_gate_ = 0; lead_gate_ = 0;
#endif
    slap_.Reset();
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

  /* the twelve patches of 11.3 on the songs' palette; var is SFXV_*. Pitches
   * are E minor chord tones, impacts sit on E1, and every follow-up event is
   * a sixteenth, an eighth or a beat of the fight tempo away */
  void Sfx(int s, int var) {
    const float r = rng_bells_.Next();
    const float low = var == SFXV_LOW ? 1.0f : 0.0f;
    switch (s) {
      case SFX_MENU_MOVE: Push(0, I_HAT, 900, 0.25f); Push(0, I_BELL, Mtof(88), 0.3f); break;
      case SFX_MENU_OK:
        if (low) { Push(0, I_BELL, Mtof(71), 0.4f); Push(kSixteenthMs / 2, I_BELL, Mtof(76), 0.4f); }
        else { Push(0, I_BELL, Mtof(76), 0.6f); Push(kSixteenthMs / 2, I_BELL, Mtof(83), 0.6f); }
        break;
      case SFX_MENU_BACK: Push(0, I_BELL, Mtof(71), 0.5f); Push(kSixteenthMs / 2, I_BELL, Mtof(64), 0.5f); break;
      case SFX_LOCK: Push(0, I_IMPACT, kImpactHz, 0.6f); Push(0, I_BELL, Mtof(76), 0.7f); Push(kSixteenthMs / 2, I_BELL, Mtof(83), 0.7f); break;
      case SFX_COUNT:
        /* the countdown and the T minus 10 ticks on E5; the short variant
         * is the count-out tick */
        if (var == SFXV_SHORT) { Push(0, I_BELL, Mtof(76), 0.5f); Push(0, I_HAT, 1100, 0.2f); }
        else Push(0, I_BELL, Mtof(76), 0.8f);
        break;
      case SFX_HORN:
        if (low) { Buzz(0, 40, 1.0f, kBeatMs); }                   /* OPS inbound: E2 for a beat */
        else { Push(0, I_BUZZ, Mtof(47), 1.0f); Push(30, I_BUZZ, Mtof(52), 1.0f); Push(kBeatMs * 3 / 2, I_BUZZ_OFF, 0, 0); }   /* B2 into E3, a beat and a half */
        break;
      case SFX_HIT_LIGHT:
        Push(0, I_SPARK, Mtof(Pal(64, r)), 0.9f);
        if (!low) Push(0, I_CLANG, Mtof(Pal(76, rng_bells_.Next())), 0.8f);   /* the low variant is a landing or a wall: no metal */
        break;
      case SFX_HIT_HEAVY:
        Push(0, I_IMPACT, kImpactHz, 1.0f); Push(0, I_CLANG, Mtof(Pal(52, r)), 0.9f);
        Push(0, I_SPARK, Mtof(Pal(64, rng_bells_.Next())), 0.8f); Push(kSixteenthMs / 4, I_CLANG, Mtof(64), 0.5f);
        break;
      case SFX_CLASH: {
        const float g = low ? 0.6f : 1.0f;                           /* 0.6 for the spinner wall hit */
        Push(0, I_IMPACT, kImpactHz, 1.0f * g);
        Push(0, I_CLANG, Mtof(Pal(40, r)), 1.0f * g); Push(30, I_CLANG, Mtof(Pal(52, rng_bells_.Next())), 1.0f * g);
        Push(0, I_SPARK, Mtof(Pal(64, rng_bells_.Next())), 1.0f * g); Push(kSixteenthMs / 2, I_SPARK, Mtof(Pal(64, rng_bells_.Next())), 1.0f * g);
        Push(kSixteenthMs, I_SPARK, Mtof(Pal(76, rng_bells_.Next())), 1.0f * g);
        break;
      }
      case SFX_LAUNCH:
        Push(0, I_SPARK, Mtof(52), 0.7f); Push(kSixteenthMs / 2, I_SPARK, Mtof(59), 0.7f); Push(kSixteenthMs, I_SPARK, Mtof(64), 0.6f);
        Push(0, I_BELL, Mtof(88), 0.3f);
        break;
      case SFX_KLAXON:
        /* Bb2, the tritone against E, as the OVERTIME sirens; the tell
         * repeats one beat later */
        if (var == SFXV_TELL) { Buzz(0, 46, 0.5f, 150); Buzz(kBeatMs, 46, 0.5f, 150); }
        else if (var == SFXV_SHORT) { Buzz(0, 46, 0.7f, 80); }
        else if (low) { Push(0, I_HAT, 1200, 0.3f); }               /* the inverted count tick */
        else { Buzz(0, 46, 0.7f, 150); }
        break;
      case SFX_KO:
        Push(0, I_IMPACT, kImpactHz, 1.0f); Push(0, I_GONG, Mtof(40), 1.0f);
        Push(kSixteenthMs, I_CLANG, Mtof(52), 0.8f); Push(kEighthMs, I_SPARK, Mtof(Pal(52, r)), 0.6f);
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
  void Render(float* l, float* r, int n) { RenderInner(l, r, n, true); }
  /* the bake's preview: the SFX and the master chain only, no conductor, so
   * the song can come from RenderMusic at the cart's stem gains */
  void RenderSfx(float* l, float* r, int n) { RenderInner(l, r, n, false); }

 private:
  void RenderInner(float* l, float* r, int n, bool songs) {
    int i = 0;
    while (i < n) {
      int span = n - i;
#if RTR_SONGS
      if (songs && mode_ > 0 && countdown_ <= 0) { Step(); countdown_ = samples_per_step_; }
      if (songs && mode_ > 0 && span > countdown_) span = countdown_;
#endif
      if (span > kBlock) span = kBlock;
      Fire(span);
      SfxSpan(l, r, i, span, songs);
#if RTR_SONGS
      if (songs && mode_ > 0) countdown_ -= span;
#endif
      frame_ += static_cast<uint32_t>(span);
      i += span;
    }
    comp_.Process(l, r, 0, n);
    limiter_.Process(l, r, 0, n);
  }

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
   * of every patch). The buses are trimmed so a hit sits about 6 dB over the
   * arena bed (the stems duck 4 dB under every hit, audio_pcm.c) and only
   * CLASH and KO reach the ceiling; the stems' trims are in audio_pcm.c */
  void SfxSpan(float* l, float* r, int from, int n, bool songs) {
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
        float a = bl_[i] * 0.85f * g, b = br_[i] * 0.85f * g;
        l[from + i] += a; r[from + i] += b;
        sl_[i] += 0.25f * a; sr_buf_[i] += 0.25f * b;
      }
    }
    Mix(spark_, 0.5f * g, 0.22f * g, 0.0f, l, r, from, n);
    Mix(buzz_, 0.6f * g, 0.0f, 0.0f, l, r, from, n);
    Mix(bells_, 0.5f * g, 0.25f * g, 0.0f, l, r, from, n);
    Mix(gong_, 0.5f * g, 0.2f * g, 0.0f, l, r, from, n);
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
      const float wg = 0.1f * g * duck_;
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
        const float cg = 0.9f * g * duck_ * crowd_gain_;
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
    if (songs && mode_ > 0) {
      for (int i = 0; i < n; ++i) { yl_[i] = yr_[i] = 0.0f; }
      const float mg = music_gain_ * duck_ * kSongTrim[mode_];
      MusicSpan(l, r, from, n, mg);
      echo_.Process(yl_, yr_, 0, n);
      const float eg = 0.6f * mg;
      for (int i = 0; i < n; ++i) { l[from + i] += eg * yl_[i]; r[from + i] += eg * yr_[i]; }
    }
#else
    (void)songs;
#endif
  }

#if RTR_SONGS
  /* the kit and the synths voiced per song */
  void Voice() {
    bellows::Kick::Params kp;
    bellows::Snare::Params sp;
    bellows::Hat::Params hp, oh;
    bellows::Va::Params bp, lp;
    if (mode_ == 1) {
      kp.decay = 0.45f; kp.drive = 5.0f; kp.click_tune = 8.0f; kp.pitch_decay = 0.07f; kick_hz_ = 44.0f;
      sp.tone = 0.35f; sp.decay = 0.22f; sp.snap = 0.35f; snare_hz_ = 170.0f;
      hp.decay = 0.035f; hp.tone = 0.85f; oh.decay = 0.2f; oh.tone = 0.9f;
    } else if (mode_ == 2) {
      kp.decay = 0.32f; kp.drive = 5.5f; kp.click_tune = 9.0f; kp.pitch_decay = 0.05f; kick_hz_ = 46.0f;
      sp.tone = 0.5f; sp.decay = 0.17f; sp.snap = 0.3f; snare_hz_ = 190.0f;
      hp.decay = 0.035f; hp.tone = 1.0f; oh.decay = 0.16f; oh.tone = 1.0f;
    } else {
      kp.decay = 0.28f; kp.drive = 6.0f; kp.click_tune = 9.0f; kp.pitch_decay = 0.045f; kick_hz_ = 47.0f;
      sp.tone = 0.55f; sp.decay = 0.14f; sp.snap = 0.3f; snare_hz_ = 200.0f;
      hp.decay = 0.03f; hp.tone = 1.05f; oh.decay = 0.14f; oh.tone = 1.05f;
    }
    kick_.Init(sr_, kp);
    snare_.Init(sr_, &rng_snare_, sp);
    hat_.Init(sr_, hp);
    ohat_.Init(sr_, oh);
    /* the bass: a saw with its square sub an octave down through the ladder,
     * then six times into a tanh; the cutoff is written per step */
    bp.shape = 0.0f; bp.detune = 6.0f; bp.sub = 0.5f; bp.cutoff = 400.0f; bp.resonance = 0.4f; bp.filter_type = 0.0f;
    bp.env_amount = 1.4f; bp.f_attack = 0.001f; bp.f_decay = mode_ == 1 ? 0.14f : 0.09f; bp.f_sustain = 0.1f; bp.f_release = 0.05f;
    bp.attack = 0.002f; bp.decay = 0.2f; bp.sustain = 0.7f; bp.release = 0.04f; bp.vel_level = 0.4f;
    bass_p_ = bp;
    bass_.SetParams(bass_p_);
    /* the riser riff: a wide saw through the ladder, its cutoff set per bar */
    lp.shape = 0.0f; lp.detune = 14.0f; lp.cutoff = 500.0f; lp.resonance = 0.3f; lp.filter_type = 0.0f; lp.env_amount = 1.0f;
    lp.f_attack = 0.002f; lp.f_decay = 0.1f; lp.f_sustain = 0.2f; lp.f_release = 0.06f;
    lp.attack = 0.003f; lp.decay = 0.15f; lp.sustain = 0.5f; lp.release = 0.06f; lp.vel_level = 0.3f;
    lead_p_ = lp;
    lead_.SetParams(lead_p_);
  }

  void ApplyMode() {
    mode_ = pending_mode_;
    pending_mode_ = -1;
    section_ = 0; bar_ = 0; step_ = 0; pass_ = 0; countdown_ = 0; bass_gate_ = 0; lead_gate_ = 0; cur_lay_ = 0;
    mask_ = 0xffff; hold_ = false;
    bass_.NoteOff(); lead_.NoteOff(); PadRelease(); SirenOff();
    riser_pulse_ = 0.0f;
    if (mode_ <= 0) { mode_ = 0; return; }
    samples_per_step_ = StepSamples(mode_, sr_);
    Voice();
    bellows::StereoDelay<500>::Params d;
    float beat = 60.0f / kBpm[mode_];
    d.time_l = beat * 0.75f; d.time_r = beat * 0.5f; d.feedback = mode_ == 1 ? 0.42f : 0.34f;
    d.cross_feedback = 0.15f; d.damping = 3000.0f; d.mix = 1.0f;
    echo_.SetParams(d);
    last_chord_ = -1;
  }

  const Section& Current(int* count) const {
    if (mode_ == 3) { *count = kOvertimeN; return overtime_[section_]; }
    if (mode_ == 2) { *count = kArenaN; return arena_[section_]; }
    *count = kGarageN; return garage_[section_];
  }

  void PadRelease() { for (int i = 0; i < npad_; i++) pad_.NoteOff(pad_ids_[i]); npad_ = 0; }
  void SirenOff() { if (siren_on_) { siren_[0].NoteOff(); siren_[1].NoteOff(); siren_on_ = false; siren_rel_ = static_cast<int>(0.4f * sr_); } }

  /* the drone: the triad voiced low, E3 to D#4 */
  void PadChord(int root, bool minor) {
    int tones[3] = {root, root + (minor ? 3 : 4), root + 7};
    PadRelease();
    for (int k = 0; k < 3; k++) {
      int m = 52 + ((tones[k] - 4 + 24) % 12);
      int id = 1000 + (note_id_++ & 0xfff);
      pad_.NoteOn(id, Mtof(static_cast<float>(m)), 0.6f, frame_);
      pad_ids_[npad_++] = id;
    }
  }

  /* a stab: root, third, fifth and the seventh, voiced E3 to D#4 */
  void Stab(int root, bool minor, float vel) {
    int ch[4] = {root, root + (minor ? 3 : 4), root + 7, root + (minor ? 10 : 11)};
    for (int k = 0; k < 4; k++) {
      int m = 52 + ((ch[k] - 4 + 24) % 12);
      stab_.NoteOn(note_id_++ & 0xfff, Mtof(static_cast<float>(m)), vel, frame_);
    }
  }

  /* a stab velocity for this step, or 0: sparse on the shelf, on the one and
   * the and of two in a chorus, on the one and the and of two in the rush */
  float StabVel(int s) const {
    if (mode_ == 1) {
      if ((bar_ % 4) == 0 && s == 0) return 0.7f;
      if ((bar_ % 4) == 3 && s == 10) return 0.6f;
      return 0.0f;
    }
    if (s == 0) return mode_ == 3 ? 0.7f : 0.6f;
    if (s == 6) return 0.5f;
    if (mode_ == 2 && (bar_ & 1) && s == 11) return 0.45f;
    return 0.0f;
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
    cur_lay_ = lay;
    const bool last_bar = bar_ == sec->def->bars - 1;
    const int root = sec->roots[bar_ % 8];
    const bool minor = sec->minor[bar_ % 8];
    const float jit = 0.06f * rng_jit_.Next();
    const DrumPat& dp = DRUMS[sec->def->drums];
    const BassPat& bp = BASSES[sec->def->bass];
    const int pos = bar_ * 16 + s;                    /* the step within the section */
    const float through = pos / static_cast<float>(sec->def->bars * 16);

    /* drums */
    const bool rolling = (lay & L_ROLL) && last_bar && s >= 8;
    if (rolling) snare_.NoteOn(snare_hz_, 0.45f + 0.55f * (s - 8) / 8.0f);
    if (lay & L_KICK) {
      float v = dp.k[s];
      if (mode_ == 1 && (bar_ & 1) && s == 14) v = 0.7f;      /* the shelf's pushed kick every other bar */
      if (v > 0 && !(rolling && s >= 14)) kick_.NoteOn(kick_hz_, v);
    }
    if ((lay & L_SNARE) && !rolling && dp.s[s] > 0) snare_.NoteOn(snare_hz_, dp.s[s] + jit);
    if (lay & L_HAT) {
      float o = dp.o[s];
      if (mode_ == 1 && bar_ == 7 && s == 14) o = 0.5f;        /* the open hat that turns the shelf's form */
      if (o > 0) ohat_.NoteOn(330.0f, o + jit);
      else if (dp.h[s] > 0) hat_.NoteOn(330.0f, dp.h[s] + jit);
    }
    if (lay & L_METAL) {
      if (dp.m[s] > 0) metal_.NoteOn(note_id_++ & 0xfff, Mtof(Pal(88, rng_metal_.Next())), dp.m[s] + jit, frame_);
      if (dp.a[s] > 0) anvil_.NoteOn(Mtof((bar_ & 1) ? 71.0f : 76.0f), dp.a[s]);
    }

    /* stabs */
    if (lay & L_STAB) { float v = StabVel(s); if (v > 0 && !rolling) Stab(root, minor, v); }

    /* the bass: the gate closes first so a one step note ends as the next starts */
    if (bass_gate_ > 0 && --bass_gate_ == 0) bass_.NoteOff();
    if (lay & L_BASS) {
      int n = bp.n[s];
      if (mode_ == 1 && (bar_ % 4) == 3 && s == 12) n = 6;    /* the tritone turns the shelf's fourth bar */
      if (n >= 0) {
        float cut;
        if (mode_ == 3) cut = 180.0f * powf(2.0f, 3.5f * through);                  /* the riser: three and a half octaves up over the loop */
        else if (mode_ == 1) cut = 300.0f + (s == 0 ? 120.0f : 0.0f);
        else cut = 500.0f + (s == 0 || s == 10 ? 200.0f : 0.0f);
        bass_p_.cutoff = cut;
        bass_.SetParams(bass_p_);
        int base = 40 + root;                                   /* E2 and up, the sub an octave under; the F and Bb sit above */
        if (base > 47) base -= 12;
        float vel = (s == 0 || s == 10) ? 1.0f : 0.8f;
        bass_.NoteOn(Mtof(static_cast<float>(base + n)), vel);
        bass_gate_ = bp.g[s];
      }
    }

    /* the drone: one chord per bar */
    int chord_id = bar_ * 100 + section_;
    if ((lay & L_PAD) && s == 0 && chord_id != last_chord_) { PadChord(root, minor); last_chord_ = chord_id; }
    if (!(lay & L_PAD) && npad_) PadRelease();

    /* the riser riff: the motif over E4 climbs a chord tone every two bars
     * while the filter opens from 500 Hz to about 4 kHz */
    if (lead_gate_ > 0 && --lead_gate_ == 0) lead_.NoteOff();
    if (lay & L_LEAD) {
      const int local = (bar_ & 1) * 16 + s;
      const int tr = RIFF_T[(bar_ / 2) & 3];
      for (int k = 0; k < kRiffN; k++) {
        if (RIFF[k].step != local) continue;
        lead_p_.cutoff = 500.0f * powf(2.0f, 0.42f * bar_);
        lead_.SetParams(lead_p_);
        lead_.NoteOn(Mtof(static_cast<float>(64 + tr + RIFF[k].semis)), RIFF[k].dur >= 4 ? 1.0f : 0.8f);
        lead_gate_ = RIFF[k].dur;
      }
    } else if (s == 0) lead_.NoteOff();

    /* the sirens hold through a section that asks for them */
    if ((lay & L_SIREN) && !siren_on_) {
      siren_[0].NoteOn(Mtof(76.0f), 1.0f); siren_[1].NoteOn(Mtof(82.0f), 1.0f); siren_on_ = true;
    } else if (!(lay & L_SIREN)) SirenOff();

    /* the noise riser pulses on every eighth */
    if ((lay & L_RISER) && (s & 1) == 0) riser_pulse_ = 1.0f;
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
    /* the drum bus: kick, snare, hats, metal and the stabs through one tanh,
     * so the kick's peaks stop ruling the ceiling and the kit glues */
    if (kick_.Active() || snare_.Active() || hat_.Active() || ohat_.Active() || Live(metal_) || anvil_.Active() || Live(stab_)) {
      for (int i = 0; i < n; ++i) { bl_[i] = 0.0f; br_[i] = 0.0f; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      kick_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] += 0.9f * dl_[i]; br_[i] += 0.9f * dr_[i]; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      snare_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] += 1.4f * dl_[i]; br_[i] += 1.4f * dr_[i]; yl_[i] += 0.2f * gd * dl_[i]; yr_[i] += 0.2f * gd * dr_[i]; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      hat_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] += 3.0f * dl_[i] * 0.85f; br_[i] += 3.0f * dr_[i]; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      ohat_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] += 1.6f * dl_[i]; br_[i] += 1.6f * dr_[i] * 0.88f; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      metal_.Process(dl_, dr_, 0, n);
      anvil_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] += 0.8f * dl_[i] * 0.8f; br_[i] += 0.8f * dr_[i]; yl_[i] += 0.35f * gd * dl_[i]; yr_[i] += 0.35f * gd * dr_[i]; }
      if (Live(stab_)) {
        for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
        stab_.Process(dl_, dr_, 0, n);
        stab_sat_.Process(dl_, dr_, 0, n);
        for (int i = 0; i < n; ++i) { bl_[i] += 0.6f * dl_[i]; br_[i] += 0.6f * dr_[i]; yl_[i] += 0.3f * gd * dl_[i]; yr_[i] += 0.3f * gd * dr_[i]; }
      }
      drum_sat_.Process(bl_, br_, 0, n);
      for (int i = 0; i < n; ++i) { l[from + i] += gd * bl_[i]; r[from + i] += gd * br_[i]; }
    }
    if (bass_.Active()) {
      for (int i = 0; i < n; ++i) { bl_[i] = 0.0f; br_[i] = 0.0f; }
      bass_.Process(bl_, br_, 0, n);
      bass_sat_.Process(bl_, br_, 0, n);
      const float ag = 0.55f * gb;
      for (int i = 0; i < n; ++i) { l[from + i] += ag * bl_[i]; r[from + i] += ag * br_[i]; }
    }
    MixM(pad_, 0.1f * gb, 0.08f * gb, 0.0f, l, r, from, n);
    if (siren_on_ || siren_rel_ > 0) {
      if (!siren_on_) siren_rel_ -= n;
      /* two saws a tritone apart, each wandering three semitones either way */
      static const float kRate[2] = {0.4f, 0.55f};
      static const float kBase[2] = {76.0f, 82.0f};
      for (int k = 0; k < 2; ++k) {
        siren_ph_[k] += kRate[k] * 6.2831853f * n / sr_;
        if (siren_ph_[k] > 6.2831853f) siren_ph_[k] -= 6.2831853f;
        siren_[k].SetFreq(Mtof(kBase[k] + 3.0f * bellows::fm::Sin(siren_ph_[k])));
      }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      siren_[0].Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { l[from + i] += 0.12f * gb * dl_[i] * 0.8f; r[from + i] += 0.12f * gb * dr_[i]; yl_[i] += 0.1f * gb * dl_[i]; yr_[i] += 0.1f * gb * dr_[i]; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      siren_[1].Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { l[from + i] += 0.12f * gb * dl_[i]; r[from + i] += 0.12f * gb * dr_[i] * 0.8f; yl_[i] += 0.1f * gb * dl_[i]; yr_[i] += 0.1f * gb * dr_[i]; }
    }
    if ((cur_lay_ & L_RISER) || riser_pulse_ > 0.002f) {
      /* pink noise through a bandpass climbing from 300 Hz to about 5 kHz
       * over the section, gated on every eighth, louder as it climbs */
      int count; const Section& sec = Current(&count);
      const int sc = step_ > 0 ? (step_ - 1) % 16 : 0;      /* the step that is sounding */
      float pos = (bar_ * 16 + sc + (1.0f - countdown_ / static_cast<float>(samples_per_step_))) / static_cast<float>(sec.def->bars * 16);
      if (pos < 0.0f) pos = 0.0f;
      if (pos > 1.0f) pos = 1.0f;
      riser_bp_.Set(300.0f * powf(2.0f, 4.0f * pos), 2.0f);
      const float lvl = (cur_lay_ & L_RISER) ? (0.1f + 0.5f * pos * pos) * gb : 0.0f;
      const float dec = expf(-1.0f / (0.07f * sr_));
      for (int i = 0; i < n; ++i) {
        float y = riser_bp_.Process(riser_.Process()) * riser_pulse_ * lvl;
        riser_pulse_ *= dec;
        l[from + i] += y; r[from + i] += y;
      }
    }
    if (lead_.Active()) {
      for (int i = 0; i < n; ++i) { bl_[i] = 0.0f; br_[i] = 0.0f; }
      lead_.Process(bl_, br_, 0, n);
      lead_sat_.Process(bl_, br_, 0, n);
      chorus_.Process(bl_, br_, 0, n);
      const float lg = 1.2f * gl, ls = 0.6f * gl;
      for (int i = 0; i < n; ++i) {
        l[from + i] += lg * bl_[i]; r[from + i] += lg * br_[i];
        yl_[i] += ls * bl_[i]; yr_[i] += ls * br_[i];
      }
    }
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
  bellows::Rng rng_snare_, rng_bass_, rng_metal_, rng_stab_, rng_pad_, rng_lead_, rng_siren_, rng_riser_, rng_jit_;
  bellows::Kick kick_;
  bellows::Snare snare_;
  bellows::Hat hat_, ohat_;
  bellows::VoicePool<bellows::Modal, 4> metal_;
  bellows::Modal anvil_;
  bellows::Va bass_, lead_;
  bellows::Va::Params bass_p_, lead_p_;
  bellows::VoicePool<bellows::Va, 4> stab_;
  bellows::VoicePool<bellows::Va, 3> pad_;
  bellows::Va siren_[2];
  bellows::NoiseGen riser_;
  bellows::Svf riser_bp_;
  bellows::StereoDelay<500> echo_;
  bellows::Chorus<> chorus_;
  bellows::Saturator<2> bass_sat_;
  bellows::Saturator<1> stab_sat_, lead_sat_, drum_sat_;
  Section garage_[kGarageN];
  Section arena_[kArenaN];
  Section overtime_[kOvertimeN];
  uint16_t mask_ = 0xffff, cur_lay_ = 0;
  bool hold_ = false, siren_on_ = false;
  int siren_rel_ = 0;
  int mode_ = 0, pending_mode_ = -1, samples_per_step_ = 1, countdown_ = 0;
  int step_ = 0, bar_ = 0, section_ = 0, pass_ = 0, bass_gate_ = 0, lead_gate_ = 0, last_chord_ = -1;
  int pad_ids_[8] = {};
  int npad_ = 0;
  float kick_hz_ = 46.0f, snare_hz_ = 190.0f;
  float siren_ph_[2] = {}, riser_pulse_ = 0.0f;
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
extern "C" void audio_bake_sfx_render(float *stereo, int frames) {
  int done = 0;
  while (done < frames) {
    int chunk = frames - done > 4096 ? 4096 : frames - done;
    for (int i = 0; i < chunk; ++i) { L[i] = stereo[(done + i) * 2]; R[i] = stereo[(done + i) * 2 + 1]; }
    band.RenderSfx(L, R, chunk);
    for (int i = 0; i < chunk; ++i) { stereo[(done + i) * 2] = L[i]; stereo[(done + i) * 2 + 1] = R[i]; }
    done += chunk;
  }
}
#endif
