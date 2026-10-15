/* audio_band.cpp: RED TAG RUMBLE's Bellows band (spec 11.2, 11.3,
 * docs/design/deep-audio.md, DEEP-plan section 9).
 *
 * Two jobs in one translation unit so the bake and the cart never drift:
 *
 *   1. The live SFX band, always compiled. Two families:
 *
 *      The stingers (menus, lock, count, horn, klaxon, the KO gong) stay on
 *      the songs' palette (E minor: E G B D) with the slap delay, so the
 *      menus sound like the music.
 *
 *      The fight is noise first and owes nothing to the palette: every
 *      impact is a transient (white noise, 3 to 14 ms), a body (pink or
 *      brown noise, 40 to 140 ms, plus a pitched thud from the heavy band
 *      up) and a tail (a struck bar or bell and a spark crackle), with one
 *      velocity from the sim setting level, brightness and length together.
 *      Pitches and decays are drawn per fire from the band's own rngs,
 *      plus or minus 15 percent. Every weapon has a voice: a loop per seat
 *      (spinner whine with wobble, load dip and spin kill, saw spin and
 *      grind, flame roar, the wedge scrape, the crusher creak, the lifter
 *      whir) or a tell and a strike (hammer, flipper, spike, crusher). Every
 *      seat has a drive loop (treads, wheels, hover blower, walker steps)
 *      and a damage loop (the fizz under 40 percent, the rattle under 20,
 *      the burning crackle). Two hazard loops follow the nearest live
 *      hazards; the crowd is a bed with three formant voices and a stomp.
 *      One shots live in plain arrays with a priority steal rule (KO 5,
 *      clash and wreck 4, crunch and stingers 3, clank and tells 2, debris
 *      1; loops never steal or get stolen). Everything in the fight pans by
 *      world x. A duck envelope sized by the hit band pulls the music
 *      stems (audio_pcm.c reads it), the drive loops 6 dB and the other
 *      loops 3 dB.
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
 *        2 ARENA     "Red Tag"      156 bpm   a breakbeat chorus and a half time stomp verse over one
 *                                             distorted bass riff, metal on the off beats, a riser riff
 *                                             stem that the game fades in for the last 20 s or low HP
 *        3 OVERTIME  "Doors Lock"   172 bpm   a pulsing eighth note bass whose filter climbs the whole
 *                                             loop, a gated noise riser, two tritone sirens, a roll
 *
 * Compiled with em++ -std=c++17 -fno-exceptions -fno-rtti -DBELLOWS_FAST_MATH=1
 * for the cart and clang++ -DMUSIC_BAKE for the bake. Nothing here touches
 * libc++: <math.h> and <stdint.h> only. Owned by the audio agent. */
#include "bellows/config.h"
#include "bellows/core/prng.h"
#include "bellows/dsp/envelopes.h"
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
#include "bellows/fx/eq.h"
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
[[maybe_unused]] const DrumPat DRUMS[P_COUNT] = {
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
[[maybe_unused]] const BassPat BASSES[B_COUNT] = {
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
  {"BREAK", 8, "Em Em Em Em Em Em G F", L_BASS | KIT | L_STAB | L_LEAD | L_ROLL, P_BREAK, B_FIGHT},
  {"STOMP", 8, "Em Em Em Em Em Em G F", L_BASS | KIT | L_LEAD, P_STOMP, B_FIGHT},
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
[[maybe_unused]] constexpr float kSongTrim[4] = {0.0f, 0.440f, 0.277f, 0.355f};

/* the fight's grid for the stingers: a beat, an eighth, a sixteenth in ms */
constexpr int kBeatMs = static_cast<int>(60000.0f / kBpm[2] + 0.5f);
constexpr int kSixteenthMs = kBeatMs / 4;

/* the riser riff: a two bar motif in semitones over E4, transposed up through
 * the chord tones every two bars (E, G, B, E) while its filter opens */
struct Note { int16_t step; int8_t semis; uint8_t dur; };
[[maybe_unused]] const Note RIFF[] = {
  {0, 0, 2}, {3, 0, 1}, {4, 3, 2}, {6, 5, 2}, {8, 7, 2}, {10, 10, 2}, {12, 7, 2}, {14, 5, 1}, {15, 3, 1},
  {16, 0, 2}, {19, 0, 1}, {20, 3, 2}, {22, 7, 2}, {24, 12, 4}, {28, 10, 2}, {30, 7, 2},
};
[[maybe_unused]] constexpr int kRiffN = sizeof RIFF / sizeof RIFF[0];
[[maybe_unused]] constexpr int RIFF_T[4] = {0, 3, 7, 12};

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
[[maybe_unused]] float Pal(int base_e, float r) {
  static const int kTone[4] = {0, 3, 7, 10};
  int i = static_cast<int>(r * 4.0f) & 3;
  return static_cast<float>(base_e + kTone[i]) + (r * 4.0f - static_cast<float>(i)) * 0.4f - 0.2f;
}


/* -- the band ---------------------------------------------------- */

/* Active() for mono voices and pools alike */
template <class P> bool Live(P& p) { return p.Active(); }
template <class V, int N> bool Live(bellows::VoicePool<V, N>& p) { return p.ActiveCount() > 0; }

enum Inst { I_CLANG, I_IMPACT, I_BUZZ, I_BUZZ_OFF, I_BELL, I_GONG, I_HAT, I_CLAP };
struct Ev { int at; int inst; float freq, vel; int gen; };
constexpr int kMaxEv = 128;

/* the game's tables the band reads by number (audio_pcm.c static asserts
 * the C side against these): juice.h BAND_*, parts.h WP_* and AR_*,
 * arena.h HZ_*, sim.h TELL_* and SPIN_* */
enum { B_TAP, B_HIT, B_HEAVY, B_HUGE, B_CLASH, B_KO };
enum { W_NONE, W_WEDGE, W_SPIKE, W_LIFTER, W_SAW, W_FLAME, W_HAMMER, W_FLIPPER, W_DRUM, W_HORIZ, W_CRUSHER };
enum { A_NONE, A_PLY, A_ALU, A_UHMW, A_STEEL, A_TI, A_WEAR };
enum { Z_NONE, Z_PIT, Z_PULVERIZER, Z_KILLSAW, Z_FLAME, Z_SCREW, Z_RAMP, Z_BUMPER, Z_DECK, Z_FLOORFLIP, Z_DROPZONE, Z_LASER, Z_WIND };
enum { T_WINDUP, T_HAMMER_MISS, T_DIG_IN, T_SPIKE_OUT, T_SPIKE_BACK, T_CLAMP_START, T_CLAMP_BITE, T_CLAMP_RELEASE,
       T_FLAME_ON, T_FLAME_OUT, T_FLIPPER_RESET, T_SAW_ON, T_SAW_OFF };
enum { S_LOST_HIT, S_LOST_CLASH, S_LOST_WALL, S_ARMED, S_LOST_STUN };

/* -- the fight voices (deep-audio 3.2) -------------------------------- */
using bellows::NoiseColor;
using bellows::SvfMode;
constexpr float k2Pi = 6.28318530717959f;
enum Bus : uint8_t { BUS_IMPACT, BUS_FX, BUS_CROWD };
enum Prio : uint8_t { P_DEBRIS = 1, P_CLANK = 2, P_TELL = 2, P_CRUNCH = 3, P_STING = 3, P_WRECK = 4, P_KO = 5 };
constexpr int kSlots = 5;          /* four seats and OPS */
constexpr int kHazLoops = 2;

inline float PanL(float p) { return p > 0.0f ? 1.0f - p : 1.0f; }
inline float PanR(float p) { return p < 0.0f ? 1.0f + p : 1.0f; }
inline float Clamp01(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }

/* every one shot: a pre delay, an age for the steal rule, a pan, a bus */
struct Voice { int wait = 0, age = 0; float pan = 0.0f, gain = 0.0f; uint8_t prio = 0, bus = 0, live = 0; };
/* a shaped noise burst: transients, bodies, hiss, whooshes, debris tails */
struct Burst : Voice {
  bellows::NoiseGen n; bellows::Svf f; bellows::ExpDecay e;
  float f0 = 1000.0f, f1 = 1000.0f, q = 0.7f, lfo = 0.0f, lfo_hz = 0.0f, lfo_ph = 0.0f;
  int t = 0, att = 0, hold = 0, sweep = 0;
};
/* velvet impulses through a highpass: sparks, debris rattle, grit */
struct Crackle : Voice { bellows::Svf hp; bellows::ExpDecay e; float dens = 0.0f; int t = 0, hold = 0; };
/* a struck bar, bell or plank */
struct Ring : Voice { bellows::Modal m; float freq = 440.0f, vel = 0.5f; bool started = false; };
/* a pitched thud */
struct Thud : Voice { bellows::Kick k; float freq = 55.0f, vel = 1.0f; bool started = false; };
/* a swept tone: servo whirs, spin ups, the laser's rising tell, a squeal */
struct Sweep : Voice { bellows::Svf lp; float ph = 0.0f, f0 = 200.0f, f1 = 400.0f, cut = 2000.0f; int t = 0, len = 1, att = 1, rel = 1; uint8_t shape = 0; };
enum { SH_SINE, SH_SAW, SH_TRI };

/* a seat's weapon voice: the tone (spinner, saw, lifter) and the noise
 * layers (saw grind, flame roar, wedge scrape, crusher creak) */
struct WLoop {
  bellows::Va tone; bellows::Va::Params tp_; int tkind = WK_NONE; bool ton = false;
  float lvl = 0.0f, pit = 0.0f;              /* targets from audio_weapon */
  float tl = 0.0f, tp = 0.0f, ptp = 0.0f;   /* smoothed level and pitch, the pitch one update ago */
  float cut = 0.0f, det = 0.0f;              /* the tone's filter and detune as last set */
  int dip = 0, dip_len = 1;                  /* the landed hit's load dip, samples */
  int kill = 0, kill_len = 1; float kill_hz = 0.0f, kill_g = 0.0f, kill_to = 60.0f;
  int detent = 0, arm_sweep = 0;
  float wob = 0.0f, drift = 0.0f;
  bellows::NoiseGen n; bellows::Svf air, grind, roar, hiss, scrape, creak;
  float brown = 0.0f;
  float grind_l = 0.0f, grind_t = 0.0f, teeth = 0.0f, grit_env = 0.0f; int grind_hold = 0, grit = 0;
  float roar_l = 0.0f, roar_t = 0.0f, flut = 0.0f;
  float scr_l = 0.0f, scr_t = 0.0f, scr_rate = 1.0f, scr_ph = 0.0f;
  float crk_l = 0.0f, crk_t = 0.0f, crk_ph = 0.0f; int crk_tick = 0;
  float pan = 0.0f;
};
/* a seat's drive voice: the noise bed, a wheel whine, a squeal */
struct DLoop {
  int kind = DK_NONE, flags = 0;
  float spd_t = 0.0f, turn_t = 0.0f, gnd_t = 1.0f, inv_t = 0.0f;
  float spd = 0.0f, gnd = 1.0f, inv = 0.0f, lvl = 0.0f, boost = 0.0f, pinch = 0.0f, stuck = 0.0f;
  bellows::Svf bed, hiss, clk; float whine_ph = 0.0f, sq_ph = 0.0f, link_ph = 0.0f, link_env = 0.0f, clank = 0.0f, wob = 0.0f, trem = 0.0f;
  int step = 0, skid_cd = 0;
  float pan = 0.0f;
};
/* a seat's damage voice: the fizz, the rattle, the burning crackle */
struct DmgLoop {
  bellows::Svf hp; float hpf = 1.0f, fizz = 0.0f, burn = 0.0f, gate = 0.0f, gs = 0.0f; bool alive = false; int flick = 0;
  int burst = 0, next_burst = 0, next_pop = 0, next_burn = 0;
  float pan = 0.0f;
};
/* a hazard bed: the killsaw, the flame grate, the screw, the laser hum, the wind */
struct HLoop {
  int type = Z_NONE; float lvl_t = 0.0f, lvl = 0.0f, pan = 0.0f, aux = 0.0f;
  bellows::Svf a, b; float brown = 0.0f, ph = 0.0f, ph2 = 0.0f, lfo = 0.0f;
};
/* a crowd voice: many throats at once, so noise through three vowel formants
 * (a crowd has no one pitch), with its own swell and a slide */
struct Shout { bellows::Svf f[3]; float a[3] = {}, hz[3] = {}, g = 0.0f, g_peak = 0.0f, rise = 0.0f, slide = 0.0f; int t = 0, len = 0, rel = 1; bool on = false; };

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
    rng_noise_.Init("rtr::noise");
    rng_loop_.Init("rtr::loop");
    rng_dmg_.Init("rtr::dmg");
    rng_shout_.Init("rtr::shout");

    /* the buzzer: horns and klaxons; every I_BUZZ gets an I_BUZZ_OFF. Its
     * own saturator gives the horn a brass edge (3.9) */
    bellows::Va::Params bz;
    bz.shape = 1.0f; bz.detune = 18.0f; bz.cutoff = 2400.0f; bz.resonance = 0.1f; bz.filter_type = 1.0f;
    bz.attack = 0.004f; bz.decay = 0.1f; bz.sustain = 0.7f; bz.release = 0.06f;
    buzz_.Init(sr, &rng_buzz_, bz);
    bellows::Saturator<1>::Params bs; bs.drive = 3.0f; bs.tone = 0.2f;
    buzz_sat_.Init(sr, bs);

    /* bells for the menus, a gong for the KO card */
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

    /* the one shot pools */
    for (auto& v : bursts_) { v = Burst(); v.f.Init(sr); v.e.Init(sr); v.n.Init(sr, NoiseColor::kWhite, &rng_noise_); }
    for (auto& v : crk_) { v = Crackle(); v.hp.Init(sr); v.hp.SetMode(SvfMode::kHp); v.e.Init(sr); }
    for (auto& v : rings_) { v = Ring(); bellows::Modal::Params mp; v.m.Init(sr, &rng_clang_, mp); }
    for (auto& v : thuds_) { v = Thud(); v.k.Init(sr); }
    for (auto& v : sweeps_) { v = Sweep(); v.lp.Init(sr); v.lp.SetMode(SvfMode::kLp); }

    /* the loops */
    for (int s = 0; s < kSlots; ++s) {
      WLoop& w = wpn_[s];
      w = WLoop();
      w.tp_ = bellows::Va::Params();
      w.tp_.shape = 0.0f; w.tp_.detune = 9.0f; w.tp_.sub = 0.3f; w.tp_.cutoff = 600.0f; w.tp_.resonance = 0.35f; w.tp_.filter_type = 1.0f;
      w.tp_.drift = 0.02f; w.tp_.attack = 0.04f; w.tp_.decay = 0.1f; w.tp_.sustain = 1.0f; w.tp_.release = 0.25f; w.tp_.vel_level = 0.0f;
      w.tone.Init(sr, &rng_whine_, w.tp_);
      w.n.Init(sr, NoiseColor::kWhite, &rng_loop_);
      w.air.Init(sr); w.air.SetMode(SvfMode::kBp);
      w.grind.Init(sr); w.grind.SetMode(SvfMode::kBp); w.grind.Set(2200.0f, 0.8f);
      w.roar.Init(sr); w.roar.SetMode(SvfMode::kLp); w.roar.Set(500.0f, 0.7f);
      w.hiss.Init(sr); w.hiss.SetMode(SvfMode::kBp); w.hiss.Set(3000.0f, 1.0f);
      w.scrape.Init(sr); w.scrape.SetMode(SvfMode::kBp);
      w.creak.Init(sr); w.creak.SetMode(SvfMode::kBp);
      DLoop& d = drv_[s];
      d = DLoop();
      d.bed.Init(sr); d.bed.SetMode(SvfMode::kLp); d.bed.Set(300.0f, 0.8f);
      d.hiss.Init(sr); d.hiss.SetMode(SvfMode::kHp); d.hiss.Set(4000.0f, 0.7f);
      d.clk.Init(sr); d.clk.SetMode(SvfMode::kBp); d.clk.Set(2500.0f, 1.2f);
      d.step = static_cast<int>(0.1f * sr);
      DmgLoop& g = dmg_[s];
      g = DmgLoop();
      g.hp.Init(sr); g.hp.SetMode(SvfMode::kHp); g.hp.Set(3500.0f, 0.7f);
    }
    drive_noise_.Init(sr, NoiseColor::kPink, &rng_loop_);
    for (auto& h : haz_) { h = HLoop(); h.a.Init(sr); h.b.Init(sr); }
    haz_noise_.Init(sr, NoiseColor::kWhite, &rng_loop_);

    /* the crowd: pink noise through two bandpasses with a slow tremolo, three
     * formant voices, a rumble for overtime */
    crowd_.Init(sr, NoiseColor::kPink, &rng_crowd_);
    crowd_bp1_.Init(sr); crowd_bp1_.SetMode(SvfMode::kBp); crowd_bp1_.Set(800.0f, 1.1f);
    crowd_bp2_.Init(sr); crowd_bp2_.SetMode(SvfMode::kBp); crowd_bp2_.Set(2200.0f, 1.6f);
    crowd_rum_.Init(sr); crowd_rum_.SetMode(SvfMode::kLp); crowd_rum_.Set(120.0f, 0.7f);
    crowd_lfo_ = 0.0f; crowd_gain_ = 0.0f; crowd_swell_ = 0.0f; crowd_bed_ = 0.0f; crowd_brown_ = 0.0f;
    swell_t_ = 0.0f; swell_v_ = 0.0f; swell_hold_ = 0; swell_rel_ = 1.0f; hush_ = 0; stomp_n_ = 0; stomp_next_ = 0; clap_left_ = 0; crowd_ot_ = false;
    for (int i = 0; i < 3; ++i) {
      shout_[i] = Shout();
      for (auto& f : shout_[i].f) { f.Init(sr); f.SetMode(SvfMode::kBp); }
    }

    bellows::Hat::Params hp; hp.decay = 0.05f; hp.tone = 1.0f;
    tick_.Init(sr, hp);
    bellows::Snare::Params cp; cp.tone = 0.2f; cp.snap = 0.9f; cp.decay = 0.14f;
    clap_.Init(sr, &rng_kit_, cp);

    bellows::StereoDelay<300>::Params sd;
    sd.time_l = 0.093f; sd.time_r = 0.071f; sd.feedback = 0.22f; sd.cross_feedback = 0.1f; sd.damping = 4200.0f; sd.mix = 1.0f;
    slap_.Init(sr, sd);
    bellows::Saturator<1>::Params st; st.drive = 2.0f; st.tone = 0.1f;
    punch_.Init(sr, st);
    bellows::Compressor<>::Params cmp; cmp.threshold_db = -10.0f; cmp.ratio = 2.0f; cmp.knee_db = 6.0f; cmp.attack = 0.004f; cmp.release = 0.12f;
    comp_.Init(sr, cmp);
    bellows::Limiter<BELLOWS_SAMPLE_RATE, true>::Params lim; lim.ceiling_db = -0.9f; lim.release = 0.06f;
    limiter_.Init(sr, lim);

#if RTR_SONGS
    SongsInit(sr);
#endif
    slap_.Reset();
    nev_ = 0; frame_ = 0; buzz_gen_ = 0; duck_ = 1.0f; buzz_pan_ = 0.0f;
    slow_ = slow_t_ = 1.0f; dk_ = 0.0f; dk_depth_ = 0.0f; dk_stage_ = 0; dk_hold_ = 0; dk_rel_ = 0.18f;
    fall_recent_ = 0; whine_mode_ = false;
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
  /* the old swell: up to level and back over about 1.2 s */
  void Crowd(float level) { Swell(level > 1.0f ? 1.0f : level, 0.06f, 0, 1.2f); }
  void SetOvertime(bool on) { crowd_ot_ = on; }
  void SetSlow(bool on) { slow_t_ = on ? 0.6f : 1.0f; }
  float MusicDuck() const { return bellows::fm::DbToGain(-dk_); }

  /* -- one shot pools ------------------------------------------------ */
  template <class V, int N> V* Alloc(V (&pool)[N], uint8_t prio) {
    V* best = nullptr;
    for (auto& v : pool) if (!v.live) { best = &v; break; }
    if (!best) {
      /* steal: the lowest priority under this one, the oldest first; the
       * same priority only once that voice is 60 ms old */
      const int min_age = static_cast<int>(0.06f * sr_);
      for (auto& v : pool) {
        if (v.prio > prio || (v.prio == prio && v.age < min_age)) continue;
        if (!best || v.prio < best->prio || (v.prio == best->prio && v.age > best->age)) best = &v;
      }
    }
    return best;
  }
  int Ms(float ms) const { return ms <= 0.0f ? 0 : static_cast<int>(ms * 0.001f * sr_); }
  float Jit(float spread = 0.15f) { return 1.0f + spread * rng_clang_.Bipolar(); }

  Burst* B(float delay_ms, uint8_t prio, uint8_t bus, float pan, NoiseColor col, SvfMode mode, float hz, float q, float decay_s, float gain) {
    Burst* v = Alloc(bursts_, prio);
    if (!v) return nullptr;
    v->n.Init(sr_, col, &rng_noise_);
    v->f.SetMode(mode); v->f.Reset(); v->f.Set(hz, q);
    v->e.SetTime(decay_s); v->e.Trigger(1.0f);
    v->f0 = v->f1 = hz; v->q = q; v->lfo = 0.0f; v->lfo_hz = 0.0f; v->lfo_ph = 0.0f;
    v->t = 0; v->att = 0; v->hold = 0; v->sweep = 0;
    v->wait = Ms(delay_ms + base_ms_); v->age = 0; v->pan = pan; v->gain = gain * base_gain_; v->prio = prio; v->bus = bus; v->live = 1;
    return v;
  }
  void SweepTo(Burst* v, float hz1, float secs) { if (v) { v->f1 = hz1; v->sweep = Ms(secs * 1000.0f); } }
  void Shape(Burst* v, float att_s, float hold_s) { if (v) { v->att = Ms(att_s * 1000.0f); v->hold = Ms(hold_s * 1000.0f); } }
  void Wobble(Burst* v, float depth, float hz) { if (v) { v->lfo = depth; v->lfo_hz = hz; } }

  /* hz is a highpass corner, or with band set the centre of a wide bandpass (sparks that are not all air) */
  Crackle* C(float delay_ms, uint8_t prio, uint8_t bus, float pan, float dens, float hz, float hold_s, float decay_s, float gain, bool band = false) {
    Crackle* v = Alloc(crk_, prio);
    if (!v) return nullptr;
    v->hp.SetMode(band ? SvfMode::kBp : SvfMode::kHp); v->hp.Reset(); v->hp.Set(hz, band ? 0.8f : 0.7f);
    v->e.SetTime(decay_s); v->e.Trigger(1.0f);
    v->dens = dens; v->t = 0; v->hold = Ms(hold_s * 1000.0f);
    v->wait = Ms(delay_ms + base_ms_); v->age = 0; v->pan = pan; v->gain = gain * base_gain_; v->prio = prio; v->bus = bus; v->live = 1;
    return v;
  }
  /* material 0 bar, 2 bell, 3 glass, 4 wood */
  Ring* R(float delay_ms, uint8_t prio, uint8_t bus, float pan, float material, float hz, float vel, float decay, float bright = 0.7f, float hard = 0.9f, float level = 0.6f) {
    Ring* v = Alloc(rings_, prio);
    if (!v) return nullptr;
    bellows::Modal::Params mp; mp.material = material; mp.decay = decay; mp.brightness = bright; mp.strike_hardness = hard; mp.level = level;
    v->m.SetParams(mp);
    v->freq = hz; v->vel = vel; v->started = false;
    v->wait = Ms(delay_ms + base_ms_); v->age = 0; v->pan = pan; v->gain = base_gain_; v->prio = prio; v->bus = bus; v->live = 1;
    return v;
  }
  Thud* T(float delay_ms, uint8_t prio, uint8_t bus, float pan, float hz, float vel, float decay, float drive, float click = 5.0f, float pitch_decay = 0.03f) {
    Thud* v = Alloc(thuds_, prio);
    if (!v) return nullptr;
    bellows::Kick::Params kp; kp.decay = decay; kp.drive = drive; kp.click_tune = click; kp.pitch_decay = pitch_decay;
    v->k.SetParams(kp);
    v->freq = hz; v->vel = vel; v->started = false;
    v->wait = Ms(delay_ms + base_ms_); v->age = 0; v->pan = pan; v->gain = base_gain_; v->prio = prio; v->bus = bus; v->live = 1;
    return v;
  }
  Sweep* S(float delay_ms, uint8_t prio, uint8_t bus, float pan, uint8_t shape, float f0, float f1, float len_s, float gain, float att_s = 0.01f, float rel_s = 0.05f, float cut = 3000.0f) {
    Sweep* v = Alloc(sweeps_, prio);
    if (!v) return nullptr;
    v->lp.Reset(); v->lp.Set(cut, 0.8f);
    v->ph = 0.0f; v->f0 = f0; v->f1 = f1; v->cut = cut; v->shape = shape;
    v->t = 0; v->len = Ms(len_s * 1000.0f); if (v->len < 1) v->len = 1;
    v->att = Ms(att_s * 1000.0f); if (v->att < 1) v->att = 1;
    v->rel = Ms(rel_s * 1000.0f); if (v->rel < 1) v->rel = 1;
    v->wait = Ms(delay_ms + base_ms_); v->age = 0; v->pan = pan; v->gain = gain * base_gain_; v->prio = prio; v->bus = bus; v->live = 1;
    return v;
  }

  /* -- the stingers (3.9): the palette, the slap delay ------------------ */
  void Push(int delay_ms, int inst, float freq, float vel) {
    if (nev_ >= kMaxEv) return;
    int gen = buzz_gen_;
    if (inst == I_BUZZ) gen = ++buzz_gen_;   /* an OFF releases only the buzz it was pushed with */
    ev_[nev_++] = Ev{static_cast<int>(delay_ms * sr_ / 1000.0f), inst, freq, vel, gen};
  }
  void Buzz(int at_ms, float midi, float vel, int len_ms) { Push(at_ms, I_BUZZ, Mtof(midi), vel); Push(at_ms + len_ms, I_BUZZ_OFF, 0, 0); }

  /* the twelve patches of 11.3; var is SFXV_*. The menus, lock, count, horn
   * and klaxon stay on the palette; the three hit ids and the KO are the
   * fight's impacts now (deep-audio 4.1) */
  void Sfx(int s, int var) {
    const float low = var == SFXV_LOW ? 1.0f : 0.0f;
    switch (s) {
      case SFX_MENU_MOVE: Push(0, I_HAT, 900, 0.15f); Push(0, I_BELL, Mtof(88), 0.62f); break;   /* the hat down (it clicked), the bell up */
      case SFX_MENU_OK:
        if (low) { Push(0, I_BELL, Mtof(71), 0.4f); Push(kSixteenthMs / 2, I_BELL, Mtof(76), 0.4f); }
        else { Push(0, I_BELL, Mtof(76), 0.6f); Push(kSixteenthMs / 2, I_BELL, Mtof(83), 0.6f); }
        break;
      case SFX_MENU_BACK: Push(0, I_BELL, Mtof(71), 0.5f); Push(kSixteenthMs / 2, I_BELL, Mtof(64), 0.5f); break;
      case SFX_LOCK:
        /* a case latch under the bells: the crunch thud and a short transient */
        T(0, P_STING, BUS_IMPACT, 0.0f, 70.0f, 0.55f, 0.16f, 3.0f);
        B(0, P_STING, BUS_IMPACT, 0.0f, NoiseColor::kWhite, SvfMode::kBp, 3500.0f, 0.7f, 0.012f, 0.5f);
        Push(0, I_BELL, Mtof(76), 0.7f); Push(kSixteenthMs / 2, I_BELL, Mtof(83), 0.7f);
        break;
      case SFX_COUNT:
        /* the countdown and the T minus 10 ticks on E5; the short variant
         * is the stillness count tick */
        if (var == SFXV_SHORT) { Push(0, I_BELL, Mtof(76), 0.5f); Push(0, I_HAT, 1100, 0.3f); }
        else Push(0, I_BELL, Mtof(76), 0.8f);
        break;
      case SFX_HORN:
        buzz_pan_ = 0.0f;
        if (low) {                                                   /* OPS inbound: E2 for a beat over a 60 Hz hum */
          Buzz(0, 40, 1.0f, kBeatMs);
          S(0, P_STING, BUS_FX, 0.0f, SH_SINE, 60.0f, 60.0f, kBeatMs * 0.0012f, 0.35f, 0.02f, 0.15f);
        } else {                                                     /* B2 into E3, a beat and a half, an air burst on the onset */
          Push(0, I_BUZZ, Mtof(47), 1.0f); Push(30, I_BUZZ, Mtof(52), 1.0f); Push(kBeatMs * 3 / 2, I_BUZZ_OFF, 0, 0);
          B(0, P_STING, BUS_FX, 0.0f, NoiseColor::kWhite, SvfMode::kBp, 3000.0f, 0.8f, 0.09f, 0.3f);
        }
        break;
      case SFX_HIT_LIGHT: Hit(HK_GENERIC, low ? B_TAP : B_HIT, 0.6f, 0.0f, -1, -1); break;
      case SFX_HIT_HEAVY: Hit(HK_GENERIC, B_HEAVY, 0.6f, 0.0f, -1, -1); break;
      case SFX_CLASH: Hit(HK_GENERIC, low ? B_HUGE : B_CLASH, low ? 0.6f : 0.8f, 0.0f, -1, -1); break;
      case SFX_LAUNCH: Fx(SFX_LAUNCH, var, 0.0f); break;
      case SFX_KLAXON:
        /* Bb2, the tritone against E, as the OVERTIME sirens; the tell
         * repeats one beat later and pans to the hazard (audio_band_haz_tell) */
        buzz_pan_ = 0.0f;
        if (var == SFXV_TELL) { Buzz(0, 46, 0.5f, 150); Buzz(kBeatMs, 46, 0.5f, 150); }
        else if (var == SFXV_SHORT) { Buzz(0, 46, 0.7f, 80); }
        else if (low) { Push(0, I_BELL, Mtof(76), 0.5f); Push(0, I_HAT, 1200, 0.3f); }   /* the inverted count tick, heard (P14) */
        else { Buzz(0, 46, 0.7f, 150); }
        break;
      case SFX_KO: KoBlast(0.0f, 0); break;
      default: Fx(s, var, 0.0f); break;
    }
  }

  /* -- impacts (3.3): CLANK, CRUNCH, WRECK with one velocity ---------- */
  /* level = 0.55 + 0.45 vel; transient 4 + 8 vel ms at 6 kHz minus 3 kHz
   * vel; body 40 + 100 vel ms; ring velocity 0.3 + 0.6 vel; every pitch and
   * decay drawn plus or minus 15 percent. kind (HK_*) colours the band,
   * armour (A_*) colours the ring, att is the attacker's slot or -1 */
  void Hit(int kind, int band, float vel, float pan, int armour, int att) {
    vel = Clamp01(vel);
    if (band >= B_KO) { KoBlast(pan, kind == HK_HAZ_PIT ? 1 : 0); return; }
    if (kind >= HK_HAZ_PULVERIZER && kind <= HK_HAZ_PIT) { HazHit(kind, band, vel, pan); return; }
    if (kind == HK_FLAME) {   /* no impact: the roar carries it, a tick is a fizz */
      C(0, P_CLANK, BUS_FX, pan, 0.04f, 3500.0f, 0.04f, 0.12f, 0.12f, true);
      return;
    }
    if (kind == HK_OPS) { vel = 1.0f; if (band < B_HUGE) band = B_HUGE; }
    const bool wreck = band >= B_HUGE, crunch = band >= B_HEAVY;
    const uint8_t pr = wreck ? P_WRECK : crunch ? P_CRUNCH : P_CLANK;
    static const float kBandGain[5] = {1.2f, 1.5f, 1.0f, 1.6f, 1.45f};   /* measured against deep-audio 6.1 */
    float L = (0.55f + 0.45f * vel) * kBandGain[band < 0 ? 0 : band > 4 ? 4 : band];
    if (kind == HK_FLIPPER) L *= 0.5f;             /* the launch and the landing carry the moment */
    if (kind == HK_LIFTER) L *= 0.6f;
    if (kind == HK_WALL || kind == HK_LAND) L *= 0.85f;

    /* the transient */
    float xc = (6000.0f - 3000.0f * vel) * Jit(), xd = 3.0f * (0.004f + 0.008f * vel) * Jit();   /* -20 dB at 4 + 8 vel ms (ExpDecay takes T60) */
    float xg = kXt * L;
    if (kind == HK_SPINNER) xc += 1000.0f;
    if (kind == HK_SPIKE) { xc = 8000.0f; xd = 0.012f; }
    if (kind == HK_RAM || kind == HK_WALL) xg *= 0.7f;
    if (kind == HK_HAMMER) L *= 1.8f;
    if (kind == HK_RAM) L *= 1.75f;
    if (wreck) xd *= 1.4f;
    B(0, pr, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, xc, 0.6f, xd, xg);
    if (kind == HK_SPINNER) B(8, pr, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, xc * 1.1f, 0.7f, xd * 0.8f, xg * 0.6f);   /* the tooth skip */

    /* the body */
    const bool body = crunch || kind == HK_RAM || kind == HK_HAMMER || kind == HK_WALL || kind == HK_LAND || kind == HK_SAW;
    if (body) {
      float bh = 900.0f, bd = (crunch ? 3.0f : 1.5f) * (0.04f + 0.1f * vel), bg = kBody * L;
      NoiseColor bc = NoiseColor::kPink;
      switch (kind) {
        case HK_RAM: bh = 600.0f; bd = 0.18f; bg *= 2.0f; break;
        case HK_HAMMER: bh = 700.0f; bd = 0.21f; bg *= 1.2f; break;
        case HK_WALL: bh = 500.0f; bd = 0.15f; bg *= 1.2f; break;
        case HK_LAND: bh = 400.0f; bd = 0.21f; bg *= 1.5f; break;
        case HK_SAW: bh = 1600.0f; bd = 0.08f; bg *= 0.5f; break;
        default: break;
      }
      if (!crunch && kind != HK_HAMMER) bg *= 0.7f;
      if (crunch) bg *= wreck ? 6.6f : 8.0f;
      if (crunch && !wreck) { bh = 1500.0f; bc = NoiseColor::kWhite; }   /* a crunch is broadband: the low end is the thud's */
      B(0, pr, BUS_IMPACT, pan, bc, SvfMode::kLp, bh * Jit(), 0.8f, bd * Jit(), bg);
    }

    /* the thud */
    const bool thud = crunch || kind == HK_HAMMER || (kind == HK_LAND && vel > 0.57f) || (kind == HK_RAM && vel > 0.6f);
    if (thud) {
      float th = wreck ? 50.0f : kind == HK_HAMMER ? 55.0f : 70.0f;
      T(0, pr, BUS_IMPACT, pan, th * Jit(0.08f), Clamp01(L * (wreck ? 1.2f : crunch ? 1.2f : 0.6f)), wreck ? 0.4f : crunch ? 0.24f : 0.18f, wreck ? 3.5f : 3.0f);
    }

    /* the ring, coloured by the victim's armour */
    if (kind != HK_SAW && armour != A_UHMW) {
      static const float kClank[5] = {330.0f, 440.0f, 520.0f, 660.0f, 880.0f};
      float rh = kClank[static_cast<int>(rng_clang_.Next() * 5.0f) % 5] * Jit();
      if (kind == HK_SPINNER) rh *= 1.5f;
      if (crunch) rh *= 0.75f;
      float rv = (0.3f + 0.6f * vel) * (band == B_TAP ? 0.7f : crunch ? 0.7f : 1.0f), rdec = 0.3f, mat = 0.0f, bright = 0.7f;
      if (kind == HK_RAM || kind == HK_WALL || kind == HK_LAND) rv *= 0.5f;
      if (kind == HK_SPIKE) rdec = 0.15f;
      if (kind == HK_FLIPPER) rv *= 0.6f;
      switch (armour) {
        case A_PLY: mat = 4.0f; rdec = 0.6f; rh *= 0.5f; break;   /* a thock: wood decays 0.12 of the time */
        case A_ALU: rh *= 2.0f; rdec = 0.2f; break;
        case A_STEEL: rdec = 0.45f; break;
        case A_TI: rdec = 0.6f; bright = 0.95f; break;
        case A_NONE: rv *= 0.3f; break;
        default: break;
      }
      const float pre = wreck ? 16.0f : 0.0f;   /* the transient alone first, then the metal: mass */
      R(pre, pr, BUS_IMPACT, pan, mat, rh, Clamp01(rv), rdec * Jit(), bright);
      if (armour == A_WEAR) R(pre, pr, BUS_IMPACT, pan, mat, rh * 1.00405f, Clamp01(rv * 0.8f), rdec * Jit());   /* 7 cents: a plate buzz */
      if (wreck) R(pre, pr, BUS_IMPACT, pan, mat, rh * 2.0f * Jit(0.03f), Clamp01(rv * 0.7f), rdec * 0.8f);
      if (armour == A_NONE) C(0, pr, BUS_IMPACT, pan, 0.05f, 2000.0f, 0.05f, 0.15f, 0.25f, true);   /* the bare frame rattles */
    }
    /* the bell: the heavy band up, the hammer always, OPS at 82 Hz */
    if (crunch || kind == HK_HAMMER || kind == HK_CRUSHER) {
      static const float kBell[4] = {110.0f, 165.0f, 196.0f, 220.0f};
      float bh = kind == HK_OPS ? 82.0f : kind == HK_HAMMER ? 110.0f : kind == HK_CRUSHER ? 165.0f : kBell[static_cast<int>(rng_clang_.Next() * 4.0f) & 3];
      R(wreck ? 16.0f : 0.0f, pr, BUS_IMPACT, pan, 2.0f, bh * Jit(0.05f), (kind == HK_HAMMER && !crunch) ? 0.5f : 0.5f, kind == HK_CRUSHER ? 0.6f : 0.5f, 0.5f);
    }
    /* sparks: none from a hammer, a ram or a saw (it has its grind) */
    if (kind != HK_HAMMER && kind != HK_RAM && kind != HK_SAW && kind != HK_LAND) {
      float cd = wreck ? 0.03f : 0.02f, cg = (crunch ? 2.4f : 1.2f) * (wreck ? 0.6f : crunch ? 0.5f : 0.4f);   /* dense and low: sparks, not clicks */
      C(0, P_DEBRIS, BUS_IMPACT, pan, cd, 4000.0f, wreck ? 0.15f : 0.05f, wreck ? 0.3f : 0.25f, cg * L * 1.6f, true);
    }
    if (wreck) B(30, P_DEBRIS, BUS_FX, pan, NoiseColor::kPink, SvfMode::kBp, 1800.0f, 1.5f, 0.5f, 0.35f * L);   /* the debris tail */
    if (kind == HK_LAND) C(0, P_DEBRIS, BUS_IMPACT, pan, 0.06f, 3000.0f, 0.06f, 0.3f, (crunch ? 1.6f : 0.8f) * L, true);           /* the rattle as it settles */

    /* what hit */
    switch (kind) {
      case HK_SPINNER: if (att >= 0 && att < kSlots) { wpn_[att].dip = wpn_[att].dip_len = Ms(120); } break;
      case HK_SAW:
        B(0, P_CLANK, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, 2000.0f, 2.0f, 0.15f, 0.25f);   /* the bite */
        if (att >= 0 && att < kSlots) wpn_[att].grind_hold = Ms(200);
        break;
      case HK_SPIKE: B(30, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 5000.0f, 0.9f, 0.08f, 0.2f); break;   /* the retract */
      case HK_CRUSHER: {
        B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 5000.0f, 0.9f, 0.06f, 0.3f);
        Burst* c = B(40, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kBp, 400.0f, 4.0f, 0.3f, 0.5f);
        Wobble(c, 0.3f, 6.0f);
        break; }
      case HK_LIFTER: S(0, P_TELL, BUS_FX, pan, SH_TRI, 160.0f, 200.0f, 0.3f, 0.12f, 0.02f, 0.1f); break;
      case HK_RAM: {            /* plate on plate: the scrape as the noses slide (3.4) */
        Burst* b = B(0, pr, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, 1800.0f * Jit(), 1.5f, 0.06f + 0.1f * vel, 1.15f * L);
        SweepTo(b, 2600.0f, 0.12f); Wobble(b, 0.2f, 23.0f);
        break; }
      case HK_OPS: T(0, P_WRECK, BUS_IMPACT, pan, 45.0f, 0.8f, 0.3f, 3.0f); break;
      default: break;
    }
  }

  /* the KO (3.6): thud 40 Hz, the brown boom, the transient, the bell at
   * 82 Hz, the debris rain, then the gong on the palette 250 ms later. pit
   * is the pit floor (the body down to 200 Hz, a longer debris); a fall off
   * THE ROOF a moment ago lowpasses the whole blast two octaves */
  void KoBlast(float pan, int pit) {
    const bool far = fall_recent_ > 0;
    const float lp = far ? 0.25f : 1.0f;
    base_gain_ = pit ? 0.6f : 1.3f;   /* the pit floor is down a hole */
    T(0, P_KO, BUS_IMPACT, pan, 40.0f, 1.0f, 0.7f, 4.5f, 6.0f, 0.04f);
    T(0, P_KO, BUS_IMPACT, pan, 58.0f, 1.0f, 0.5f, 4.0f, 5.0f, 0.03f);
    B(0, P_KO, BUS_IMPACT, pan, NoiseColor::kBrown, SvfMode::kLp, (pit ? 200.0f : 250.0f) * lp, 0.7f, 0.8f, kBody * 20.0f);
    /* the crack of the blast skips the impact tanh: it is the one sound that may meet the limiter */
    B(0, P_KO, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 3000.0f * lp, 0.6f, far ? 0.06f : 0.04f, far ? 1.0f : 5.0f);
    R(16, P_KO, BUS_IMPACT, pan, 2.0f, 82.0f, 1.0f, 0.8f, 0.5f);
    if (!far) R(16, P_KO, BUS_IMPACT, pan, 0.0f, 330.0f * Jit(), 0.8f, 0.5f);
    C(0, P_KO, BUS_IMPACT, pan, 0.06f, 4000.0f, 0.1f, 0.35f, far ? 0.4f : 2.8f, true);   /* the sparks of the blast */
    Rain(pan, pit ? 0.6f : 1.2f, far ? 0.4f : 1.0f);
    Push(250, I_GONG, Mtof(40), 0.6f);
    base_gain_ = 1.0f;
  }

  /* the debris rain of a burst: a debris tail, four pieces landing, the crackle */
  void Rain(float pan, float secs, float level) {
    B(20, P_DEBRIS, BUS_FX, pan, NoiseColor::kPink, SvfMode::kBp, 1800.0f, 1.5f, secs * 0.6f, 0.55f * level);
    float at = 80.0f;
    for (int i = 0; i < 4; ++i) {
      float hz = 220.0f * powf(2.0f, 3.0f * rng_clang_.Next());
      R(at, P_DEBRIS, BUS_FX, pan + 0.2f * rng_clang_.Bipolar(), 0.0f, hz, (0.5f - 0.1f * i) * level, 0.12f);
      at += 80.0f + 220.0f * rng_clang_.Next();
    }
    C(0, P_DEBRIS, BUS_FX, pan, 0.04f, 3000.0f, secs * 0.5f, secs * 0.5f, 0.2f * level, true);
  }

  /* hazard hits (3.7) */
  void HazHit(int kind, int band, float vel, float pan) {
    switch (kind) {
      case HK_HAZ_PULVERIZER:   /* a wreck with the lows doubled */
        T(0, P_WRECK, BUS_IMPACT, pan, 48.0f, 1.0f, 0.3f, 3.5f);
        T(0, P_WRECK, BUS_IMPACT, pan, 72.0f, 0.6f, 0.2f, 3.0f);
        B(0, P_WRECK, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kLp, 700.0f, 0.8f, 0.16f, kBody * 1.3f);
        B(0, P_WRECK, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, 3500.0f, 0.6f, 0.04f, kXt * 1.4f);
        C(0, P_WRECK, BUS_IMPACT, pan, 0.05f, 4000.0f, 0.1f, 0.3f, 2.2f, true);
        R(16, P_WRECK, BUS_IMPACT, pan, 2.0f, 110.0f, 0.8f, 0.5f, 0.5f);
        B(30, P_DEBRIS, BUS_FX, pan, NoiseColor::kPink, SvfMode::kBp, 1800.0f, 1.5f, 0.4f, 0.4f);
        break;
      case HK_HAZ_KILLSAW:
        B(0, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, 2500.0f, 1.2f, 0.15f, 0.8f);
        Hit(HK_GENERIC, band < B_HEAVY ? B_HEAVY : band, 0.7f, pan, -1, -1);
        break;
      case HK_HAZ_FLAME: C(0, P_CLANK, BUS_FX, pan, 0.05f, 3500.0f, 0.1f, 0.2f, 0.25f, true); B(0, P_CLANK, BUS_FX, pan, NoiseColor::kBrown, SvfMode::kLp, 400.0f, 0.7f, 0.25f, 0.6f); break;
      case HK_HAZ_SCREW:        /* the flights grab it: a chewing crunch */
        B(0, P_CLANK, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kLp, 700.0f, 0.8f, 0.25f, kBody * 8.0f);
        B(0, P_CLANK, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, 3000.0f, 0.7f, 0.04f, kXt * 3.0f);
        C(0, P_CLANK, BUS_IMPACT, pan, 0.03f, 3500.0f, 0.08f, 0.2f, 0.8f, true);
        R(0, P_CLANK, BUS_IMPACT, pan, 0.0f, 260.0f * Jit(), 0.4f, 0.2f);
        break;
      case HK_HAZ_LASER:        /* the zap */
        B(0, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, 8000.0f, 0.8f, 0.06f, kXt * 2.0f);
        C(0, P_CRUNCH, BUS_IMPACT, pan, 0.05f, 4000.0f, 0.1f, 0.2f, 1.2f, true);
        R(0, P_CRUNCH, BUS_IMPACT, pan, 3.0f, 1760.0f, 0.4f, 0.4f);
        break;
      case HK_HAZ_DROP: {       /* the CRT, the fridge, the box */
        B(0, P_WRECK, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kLp, 300.0f, 0.8f, 0.3f, kBody * 4.0f);
        B(0, P_WRECK, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, 3000.0f, 0.6f, 0.04f, kXt * 1.8f);
        C(0, P_WRECK, BUS_IMPACT, pan, 0.05f, 3500.0f, 0.08f, 0.25f, 1.2f, true);
        B(30, P_DEBRIS, BUS_FX, pan, NoiseColor::kPink, SvfMode::kBp, 1800.0f, 1.5f, 0.6f, 0.4f);
        for (int i = 0; i < 3; ++i) R(40.0f + 90.0f * i, P_DEBRIS, BUS_FX, pan, 0.0f, 300.0f * Jit(0.4f), 0.5f - 0.1f * i, 0.3f);
        int pick = static_cast<int>(rng_clang_.Next() * 3.0f);
        if (pick == 0) R(0, P_CRUNCH, BUS_IMPACT, pan, 3.0f, 2000.0f, 0.6f, 0.6f);
        else if (pick == 1) T(0, P_CRUNCH, BUS_IMPACT, pan, 60.0f, 0.9f, 0.25f, 3.0f);
        else C(0, P_CRUNCH, BUS_IMPACT, pan, 0.06f, 1500.0f, 0.15f, 0.2f, 0.9f, true);
        break; }
      case HK_HAZ_PIT: KoBlast(pan, 1); break;
      default: Hit(HK_GENERIC, band, vel, pan, -1, -1); break;
    }
  }

  /* -- the other one shots (deep-audio 3.4 to 3.7) ------------------- */
  void Fx(int s, int var, float pan) {
    switch (s) {
      case SFX_LAUNCH:          /* the plate throws the bot: a pneumatic thump and the plate */
        B(0, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kLp, 500.0f, 0.8f, 0.12f, kBody * 2.0f);
        B(0, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, 4000.0f, 0.8f, 0.04f, kXt * 1.0f);
        R(0, P_CRUNCH, BUS_IMPACT, pan, 0.0f, 440.0f * Jit(), 0.5f, 0.3f);
        break;
      case SFX_LAUNCH_WEDGE:    /* the wedge tips them: the scrape lets go, the lip thumps */
        B(0, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kLp, 450.0f, 0.8f, 0.14f, kBody * 2.5f);
        B(0, P_CRUNCH, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 1500.0f, 2.0f, 0.16f, 0.9f);
        R(0, P_CRUNCH, BUS_IMPACT, pan, 0.0f, 300.0f * Jit(), 0.35f, 0.3f);
        break;
      case SFX_LAUNCH_WHOOSH: { /* the air under a flier */
        Burst* b = B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 800.0f, 1.2f, 0.4f, 0.7f);
        SweepTo(b, 300.0f, 1.2f); Shape(b, 0.15f, 0.8f);
        break; }
      case SFX_TELL_HAMMER: Tell(-1, W_HAMMER, T_WINDUP, pan); break;
      case SFX_TELL_FLIPPER: Tell(-1, W_FLIPPER, T_WINDUP, pan); break;
      case SFX_TELL_SPIKE: Tell(-1, W_SPIKE, var ? T_SPIKE_BACK : T_SPIKE_OUT, pan); break;
      case SFX_TELL_CRUSHER: Tell(-1, W_CRUSHER, T_CLAMP_START, pan); break;
      case SFX_BITE_CRUSHER: Tell(-1, W_CRUSHER, T_CLAMP_BITE, pan); break;
      case SFX_RELEASE_CRUSHER: Tell(-1, W_CRUSHER, T_CLAMP_RELEASE, pan); break;
      case SFX_FLAME_IGNITE: Tell(-1, W_FLAME, T_FLAME_ON, pan); break;
      case SFX_FLAME_OUT: Tell(-1, W_FLAME, T_FLAME_OUT, pan); break;
      case SFX_DIG_IN: Tell(-1, W_WEDGE, T_DIG_IN, pan); break;
      case SFX_SAW_TOGGLE: Tell(-1, W_SAW, var ? T_SAW_OFF : T_SAW_ON, pan); break;
      case SFX_LIFT:            /* 0 the lifter engages (a soft clank under the whir), 1 OPS's forks (hydraulic) */
        if (var == 1) { B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 4000.0f, 0.8f, 0.2f, 0.35f); T(0, P_TELL, BUS_IMPACT, pan, 55.0f, 0.5f, 0.2f, 2.5f); }
        S(0, P_TELL, BUS_FX, pan, SH_TRI, 140.0f, 190.0f, 0.4f, 0.12f, 0.03f, 0.1f);
        Hit(HK_GENERIC, B_TAP, 0.4f, pan, -1, -1);
        break;
      case SFX_LIFT_DROP:
        B(0, P_CLANK, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kLp, 400.0f, 0.8f, 0.08f, kBody * 0.5f);
        Hit(HK_GENERIC, B_HIT, 0.5f, pan, -1, -1);
        break;
      case SFX_SPIN_KILL: SpinKillShot(pan, 1.0f); break;
      case SFX_SPIN_READY:      /* armed: a bright tick that says the disc will bite */
        R(0, P_TELL, BUS_FX, pan, 3.0f, 2350.0f, 0.45f, 0.25f, 0.8f);
        B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 6000.0f, 1.0f, 0.03f, 0.3f);
        break;
      case SFX_SHED: {          /* a panel falls off: the ring, three bounces, the scatter */
        float hz = 440.0f * powf(2.0f, rng_clang_.Next());
        R(0, P_CRUNCH, BUS_IMPACT, pan, 0.0f, hz, 0.9f, 0.4f);
        R(90, P_DEBRIS, BUS_FX, pan, 0.0f, hz * 1.07f, 0.4f, 0.25f);
        R(160, P_DEBRIS, BUS_FX, pan, 0.0f, hz * 0.97f, 0.25f, 0.2f);
        R(210, P_DEBRIS, BUS_FX, pan, 0.0f, hz * 1.03f, 0.12f, 0.15f);
        C(0, P_DEBRIS, BUS_FX, pan, 0.04f, 3000.0f, 0.05f, 0.2f, 0.15f, true);
        B(0, P_DEBRIS, BUS_FX, pan, NoiseColor::kPink, SvfMode::kBp, 1800.0f, 1.5f, 0.3f, 0.4f);
        B(0, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, 3500.0f, 0.7f, 0.03f, kXt * 2.2f);
        break; }
      case SFX_BURST: Rain(pan, 1.2f, 1.0f); break;
      case SFX_SELFRIGHT:       /* 0 the arm rights it, 1 the SHELL rolls, 2 OPS rights it */
        if (var == 0) S(0, P_TELL, BUS_FX, pan, SH_SAW, 250.0f, 500.0f, 0.3f, 0.12f, 0.02f, 0.05f, 1500.0f);
        if (var == 2) B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 4000.0f, 0.8f, 0.25f, 0.35f);
        Hit(HK_GENERIC, B_HIT, 0.6f, pan, -1, -1);
        if (var == 0) { R(300, P_CLANK, BUS_IMPACT, pan, 0.0f, 440.0f * Jit(), 0.5f, 0.3f); }
        break;
      case SFX_BOOST: { Burst* b = B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 2500.0f, 0.9f, 0.25f, 1.0f); SweepTo(b, 5000.0f, 0.15f); break; }
      case SFX_STUCK:           /* the wheels squeal on the spot */
        C(0, P_TELL, BUS_FX, pan, 0.06f, 2500.0f, 0.2f, 0.2f, 0.18f, true);
        S(0, P_TELL, BUS_FX, pan, SH_SINE, 1900.0f, 1700.0f, 0.4f, 0.06f, 0.02f, 0.1f);
        break;
      case SFX_FALL: {          /* off the edge: the whoosh before the far KO */
        Burst* b = B(0, P_CRUNCH, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 1200.0f, 1.2f, 0.3f, 0.7f);
        SweepTo(b, 300.0f, 0.8f); Shape(b, 0.1f, 0.6f);
        fall_recent_ = Ms(2500.0f);
        break; }
      case SFX_BENCH:           /* wood: the plank, the splinters, the thump */
        R(0, P_CRUNCH, BUS_IMPACT, pan, 4.0f, 150.0f, 0.9f, 1.0f);
        C(0, P_DEBRIS, BUS_FX, pan, 0.05f, 2000.0f, 0.1f, 0.3f, 0.25f, true);
        B(0, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kLp, 600.0f, 0.8f, 0.1f, kBody * 0.8f);
        break;
      case SFX_BUMPER:
        R(0, P_CRUNCH, BUS_IMPACT, pan, 0.0f, 220.0f, 0.8f, 0.4f);
        R(70, P_CRUNCH, BUS_IMPACT, pan, 0.0f, 223.0f, 0.6f, 0.4f);
        B(0, P_DEBRIS, BUS_FX, pan, NoiseColor::kPink, SvfMode::kBp, 1800.0f, 1.5f, 0.5f, 0.4f);
        Hit(HK_GENERIC, B_HIT, 0.7f, pan, -1, -1);
        break;
      case SFX_UNDERRIDE:       /* the lip goes under: a scrape bite and the lift */
        B(0, P_CRUNCH, BUS_FX, pan, NoiseColor::kPink, SvfMode::kBp, 1200.0f, 2.0f, 0.16f, 1.4f);
        B(0, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kLp, 450.0f, 0.8f, 0.1f, kBody * 2.0f);
        R(0, P_CRUNCH, BUS_IMPACT, pan, 0.0f, 300.0f * Jit(), 0.3f, 0.25f);
        break;
      case SFX_RIDE_DROP:       /* the victim drops off the plate */
        B(0, P_CLANK, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kLp, 400.0f, 0.8f, 0.08f, kBody * 0.6f);
        Hit(HK_LAND, B_HIT, 0.5f, pan, -1, -1);
        break;
      case SFX_WHEEL_HIT:       /* rubber and an axle: a thwack and a boing */
        B(0, P_CLANK, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kLp, 250.0f, 0.8f, 0.08f, kBody * 0.8f);
        S(0, P_CLANK, BUS_FX, pan, SH_SINE, 180.0f, 110.0f, 0.15f, 0.3f, 0.002f, 0.1f);
        C(0, P_DEBRIS, BUS_FX, pan, 0.05f, 1500.0f, 0.03f, 0.1f, 0.15f, true);
        break;
      case SFX_DENT: {          /* a panel wears to state var (1..3): the crumple and the bent ring */
        float st = static_cast<float>(var < 1 ? 1 : var > 3 ? 3 : var);
        B(0, P_CLANK, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kBp, 700.0f, 1.5f, 0.06f, 0.3f + 0.1f * st);
        R(0, P_CLANK, BUS_IMPACT, pan, 2.0f, (520.0f - 60.0f * st) * Jit(), 0.15f + 0.1f * st, 0.2f, 0.4f);
        C(0, P_DEBRIS, BUS_FX, pan, 0.04f, 3500.0f, 0.02f, 0.1f, 0.12f, true);
        break; }
      case SFX_SCRAPE: {        /* 0 along the floor, 1 along a wall (brighter, with a clank) */
        Burst* b = B(0, P_CLANK, BUS_FX, pan, NoiseColor::kPink, SvfMode::kBp, var ? 2000.0f : 1300.0f, 2.0f, 0.22f, 1.0f);
        Wobble(b, 0.3f, 9.0f); Shape(b, 0.01f, 0.06f);
        if (var) Hit(HK_WALL, B_TAP, 0.4f, pan, -1, -1);
        break; }
      case SFX_SKID:            /* var is the drive: tyres squeal, treads grind, a hover slews */
        if (var == 3 || var == 6) { Burst* b = B(0, P_CLANK, BUS_FX, pan, NoiseColor::kPink, SvfMode::kLp, 600.0f, 0.8f, 0.2f, 0.5f); Shape(b, 0.02f, 0.1f); }
        else if (var == 4) { Burst* b = B(0, P_CLANK, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 900.0f, 1.0f, 0.25f, 0.3f); SweepTo(b, 500.0f, 0.3f); }
        else {
          S(0, P_CLANK, BUS_FX, pan, SH_SINE, 1600.0f, 1300.0f, 0.25f, 0.05f, 0.02f, 0.08f);
          B(0, P_CLANK, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 2500.0f, 3.0f, 0.25f, 0.3f);
        }
        break;
      case SFX_GRIND_BITE: B(0, P_CLANK, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, 2000.0f, 1.5f, 0.15f, 0.9f); break;
      case SFX_BURN_START:      /* it catches: a whump and the first crackle */
        B(0, P_TELL, BUS_FX, pan, NoiseColor::kPink, SvfMode::kLp, 300.0f, 0.8f, 0.18f, 2.0f);
        C(0, P_DEBRIS, BUS_FX, pan, 0.06f, 3500.0f, 0.15f, 0.2f, 0.3f, true);
        break;
      case SFX_BURN_END:        /* it goes out: a hiss and two pops */
        B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 4000.0f, 0.9f, 0.2f, 0.3f);
        B(60, P_DEBRIS, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 1000.0f, 1.0f, 0.02f, 0.3f);
        B(170, P_DEBRIS, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 1300.0f, 1.0f, 0.02f, 0.25f);
        break;
      default: break;
    }
  }

  /* the spin kill's one shot part: the motor dumping (3.4) */
  void SpinKillShot(float pan, float v) {
    B(0, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, 4000.0f, 0.6f, 0.03f, kXt * 2.0f * v);
    C(0, P_DEBRIS, BUS_FX, pan, 0.04f, 3500.0f, 0.08f, 0.25f, 0.35f * v, true);
    B(20, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 6000.0f, 1.0f, 0.2f, 0.6f * v);
  }

  /* -- tells (3.4): pressing the weapon button sounds in one frame ------- */
  void Tell(int slot, int weapon, int zone, float pan) {
    WLoop* w = slot >= 0 && slot < kSlots ? &wpn_[slot] : nullptr;
    switch (zone) {
      case T_WINDUP:
        if (weapon == W_FLIPPER) {        /* the pneumatic burst, the plate hitting its stop */
          B(0, P_CRUNCH, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 3500.0f, 0.9f, 0.18f, 2.6f);
          B(0, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kLp, 600.0f, 0.8f, 0.12f, kBody * 0.6f);
          R(30, P_CRUNCH, BUS_IMPACT, pan, 0.0f, 440.0f * Jit(0.05f), 0.25f, 0.3f);
          B(30, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kWhite, SvfMode::kBp, 3500.0f, 0.6f, 0.03f, kXt * 1.2f);
        } else {                          /* the hammer: the pneumatic charge and the arm swinging up */
          B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 6000.0f, 0.9f, 0.15f, 1.2f);
          S(0, P_TELL, BUS_FX, pan, SH_SAW, 200.0f, 900.0f, 0.25f, 0.3f, 0.02f, 0.04f, 2500.0f);
        }
        break;
      case T_HAMMER_MISS:                 /* the strike with no victim: the floor */
        T(0, P_CRUNCH, BUS_IMPACT, pan, 55.0f, 0.8f, 0.2f, 3.0f);
        B(0, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kLp, 600.0f, 0.8f, 0.14f, kBody);
        R(0, P_CRUNCH, BUS_IMPACT, pan, 0.0f, 220.0f * Jit(), 0.3f, 0.3f);
        C(0, P_DEBRIS, BUS_FX, pan, 0.04f, 3500.0f, 0.02f, 0.2f, 0.15f, true);
        break;
      case T_DIG_IN: {                    /* the wedge digs in: a scrape bite and the motors rev */
        Burst* b = B(0, P_TELL, BUS_FX, pan, NoiseColor::kPink, SvfMode::kBp, 900.0f, 2.0f, 0.2f, 0.6f);
        SweepTo(b, 1600.0f, 0.3f); Shape(b, 0.01f, 0.1f);
        S(0, P_TELL, BUS_FX, pan, SH_SAW, 120.0f, 260.0f, 0.33f, 0.14f, 0.02f, 0.08f, 1200.0f);
        C(0, P_DEBRIS, BUS_FX, pan, 0.04f, 3500.0f, 0.1f, 0.15f, 0.2f, true);
        break; }
      case T_SPIKE_OUT:
        B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 5000.0f, 0.9f, 0.04f, 0.5f);
        R(0, P_TELL, BUS_IMPACT, pan, 0.0f, 880.0f * Jit(0.05f), 0.2f, 0.15f);
        break;
      case T_SPIKE_BACK: B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 4000.0f, 0.9f, 0.03f, 0.3f); break;
      case T_CLAMP_START:                 /* the servo closes the jaws over 24 frames */
        S(0, P_TELL, BUS_FX, pan, SH_SAW, 300.0f, 180.0f, 0.4f, 0.12f, 0.02f, 0.06f, 1200.0f);
        S(0, P_TELL, BUS_FX, pan, SH_SINE, 50.0f, 50.0f, 0.4f, 0.25f, 0.02f, 0.06f);
        break;
      case T_CLAMP_BITE:
        B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 5000.0f, 0.9f, 0.06f, 0.4f);
        R(40, P_CRUNCH, BUS_IMPACT, pan, 2.0f, 165.0f, 0.7f, 0.6f, 0.5f);
        { Burst* c = B(40, P_CRUNCH, BUS_IMPACT, pan, NoiseColor::kPink, SvfMode::kBp, 400.0f, 4.0f, 0.3f, 0.5f); Wobble(c, 0.3f, 6.0f); }
        break;
      case T_CLAMP_RELEASE:
        B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 5000.0f, 0.9f, 0.08f, 0.5f);
        R(0, P_TELL, BUS_IMPACT, pan, 0.0f, 330.0f * Jit(0.05f), 0.3f, 0.3f);
        break;
      case T_FLAME_ON:                    /* the hiss, then the whump as the cone lights */
        B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 6000.0f, 0.9f, 0.12f, 0.8f);
        B(60, P_TELL, BUS_FX, pan, NoiseColor::kPink, SvfMode::kLp, 300.0f, 0.8f, 0.12f, 0.9f);
        if (w) w->roar_l = 0.0f;
        break;
      case T_FLAME_OUT:                   /* the fuel runs dry: three pops */
        for (int i = 0; i < 3; ++i) B(30.0f + 70.0f * i, P_DEBRIS, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 900.0f * Jit(0.3f), 1.0f, 0.02f, 0.35f);
        break;
      case T_FLIPPER_RESET:               /* the plate is back: a vent and a small tick */
        B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 6000.0f, 1.0f, 0.05f, 0.18f);
        R(0, P_TELL, BUS_FX, pan, 3.0f, 1320.0f, 0.12f, 0.15f);
        break;
      case T_SAW_ON: case T_SAW_OFF:      /* the switch */
        B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 4000.0f, 0.8f, 0.02f, 0.8f);
        R(0, P_TELL, BUS_FX, pan, 0.0f, zone == T_SAW_ON ? 1200.0f : 900.0f, 0.3f, 0.1f);
        break;
      default: break;
    }
  }

  /* EV_SPIN_CHANGE (not SPIN_ARMED) or the poll of audio_pcm.c: a landed hit
   * spends spin (the dip); a clash, a wall or a stun kills it (the fall) */
  void SpinChange(int slot, int zone, float before, float lost) {
    if (slot < 0 || slot >= kSlots) return;
    WLoop& w = wpn_[slot];
    if (zone == S_LOST_HIT && lost < 0.3f) { w.dip = w.dip_len = Ms(120); return; }
    float hz = w.ton ? ToneHz(w.tkind, w.tp) : 0.0f;
    if (hz > 0.0f && w.tl > 0.01f) {
      w.kill = w.kill_len = Ms(300); w.kill_hz = hz; w.kill_g = w.tl;
      float after = before - lost; if (after < 0.0f) after = 0.0f;
      w.kill_to = after > 0.05f ? ToneHz(w.tkind, after) : 60.0f;
      w.tl = 0.0f; w.tp = after;
    }
    SpinKillShot(w.pan, Clamp01(0.5f + lost));
  }

  /* a hazard's own tell beside the klaxon (3.7); tell_s is its length */
  void HazTell(int type, float pan, float tell_s) {
    buzz_pan_ = pan;
    float ts = tell_s > 0.2f ? tell_s : 1.0f;
    switch (type) {
      case Z_PULVERIZER: case Z_FLOORFLIP: {   /* the hiss charge */
        Burst* b = B(0, P_TELL, BUS_FX, pan, NoiseColor::kWhite, SvfMode::kBp, 2000.0f, 1.0f, 0.1f, 0.4f);
        SweepTo(b, 6000.0f, 0.3f); Shape(b, 0.25f, 0.05f);
        break; }
      case Z_KILLSAW: S(0, P_TELL, BUS_FX, pan, SH_SAW, 200.0f, 1200.0f, ts, 0.12f, 0.1f, 0.1f, 3000.0f); break;
      case Z_FLAME: B(0, P_TELL, BUS_FX, pan, NoiseColor::kPink, SvfMode::kLp, 300.0f, 0.8f, 0.12f, 0.6f); break;
      case Z_LASER: S(0, P_TELL, BUS_FX, pan, SH_SINE, 1000.0f, 3000.0f, ts, 0.12f, 0.05f, 0.05f); break;
      case Z_DROPZONE: S(0, P_TELL, BUS_FX, pan, SH_SAW, 400.0f, 250.0f, ts, 0.08f, 0.1f, 0.2f, 1500.0f); break;
      default: break;
    }
  }

  /* -- the crowd (3.8) ------------------------------------------------ */
  /* the bed rises to peak with a rate per span, holds, falls back over rel s */
  void Swell(float peak, float rate, int hold_ms, float rel_s) {
    if (peak >= swell_t_) { swell_t_ = peak; swell_rate_ = rate; swell_hold_ = Ms(static_cast<float>(hold_ms)); swell_rel_ = rel_s; }
  }
  /* vowel 0 "a", 1 "e", 3 "o" (the bass formant table of engines/formant.h, first three) */
  void Shouts(int n, float vowel, float len_s, float level, float rise_s, float slide = 0.0f) {
    static const float kF[4][3] = {{600.0f, 1040.0f, 2250.0f}, {400.0f, 1620.0f, 2400.0f}, {250.0f, 1750.0f, 2600.0f}, {400.0f, 750.0f, 2400.0f}};
    static const float kA[4][3] = {{1.0f, 0.45f, 0.35f}, {1.0f, 0.25f, 0.35f}, {1.0f, 0.1f, 0.2f}, {1.0f, 0.28f, 0.09f}};
    const int v = static_cast<int>(vowel) & 3;
    for (int i = 0; i < 3 && i < n; ++i) {
      Shout& s = shout_[i];
      const float spread = 1.0f + 0.06f * (i - 1) + 0.02f * rng_shout_.Bipolar();   /* three sections of the stand, not one singer */
      for (int k = 0; k < 3; ++k) { s.hz[k] = kF[v][k] * spread; s.a[k] = kA[v][k]; s.f[k].Reset(); s.f[k].Set(s.hz[k], k == 0 ? 3.0f : 5.0f); }
      s.on = true; s.t = 0; s.len = Ms(len_s * 1000.0f); s.rel = Ms(300.0f); s.g_peak = level; s.rise = rise_s; s.g = 0.0f; s.slide = slide;
    }
  }
  void CrowdReact(int kind) {
    switch (kind) {
      case CROWD_OOOH: Swell(0.3f, 0.12f, 150, 1.5f); Shouts(2, 3.0f, 0.6f, 0.4f, 0.15f, 0.11f); break;   /* "o", sliding down a tone */
      case CROWD_ROAR: Swell(0.55f, 0.2f, 300, 2.5f); Shouts(3, 0.0f, 1.2f, 0.4f, 0.4f, -0.05f); clap_left_ = Ms(1500); clap_g_ = 0.2f; break;
      case CROWD_KO:
        Swell(0.9f, 0.25f, 3000, 3.0f); Shouts(3, 0.0f, 2.5f, 0.55f, 0.3f, -0.05f); clap_left_ = Ms(3000); clap_g_ = 0.25f;
        stomp_n_ = 3000 / kBeatMs; stomp_next_ = Ms(static_cast<float>(kBeatMs));
        break;
      case CROWD_HUSH: hush_ = Ms(3200); break;
      case CROWD_JEER: Swell(0.3f, 0.2f, 100, 0.8f); Shouts(3, 1.0f, 0.3f, 0.3f, 0.05f); break;
      default: break;
    }
    if (kind != CROWD_HUSH) hush_ = 0;
  }

  /* -- the duck (3.10): sized by the band ------------------------------ */
  void DuckBand(int band) {
    static const float kDepth[6] = {1.0f, 2.5f, 5.0f, 8.0f, 8.0f, 12.0f};
    const int b = band < 0 ? 0 : band > 5 ? 5 : band;
    const float d = kDepth[b];
    if (dk_stage_ != 0 && d < dk_depth_ && d <= dk_) {                 /* a smaller hit inside a bigger duck: hold it a little longer */
      if (dk_stage_ == 3) { dk_stage_ = 2; dk_hold_ = Ms(60.0f); }
      else if (dk_stage_ == 2 && dk_hold_ < Ms(60.0f)) dk_hold_ = Ms(60.0f);
      return;
    }
    dk_depth_ = d; dk_stage_ = 1;
    dk_hold_ = Ms(b >= B_KO ? 400.0f : 60.0f);
    dk_rel_ = b >= B_KO ? 0.9f : 0.18f;
  }

  /* -- the loop setters (once per render frame from audio_fight_update) - */
  void Weapon(int slot, int kind, float level, float pitch, float load) {
    if (slot < 0 || slot >= kSlots) return;
    WLoop& w = wpn_[slot];
    level = Clamp01(level); pitch = Clamp01(pitch); load = Clamp01(load);
    switch (kind) {
      case WK_NONE:
        w.lvl = 0.0f; w.grind_t = 0.0f; w.roar_t = 0.0f; w.scr_t = 0.0f; w.crk_t = 0.0f;
        break;
      case WK_SPIN_HORIZ: case WK_SPIN_DRUM: case WK_MELTY: case WK_SAW: case WK_LIFTER_SERVO: case WK_HAMMER_ARM:
        if (kind != w.tkind) SetToneKind(w, kind);
        w.lvl = level; w.ptp = w.pit; w.pit = pitch;
        if (kind == WK_SAW) w.grind_t = load;
        break;
      case WK_FLAME: w.roar_t = level; break;
      case WK_SCRAPE: w.scr_t = level; w.scr_rate = 0.7f + 3.0f * pitch; break;
      case WK_CRUSHER_SERVO: w.crk_t = level; break;
      default: break;
    }
  }
  void Drive(int slot, int kind, float speed, float turn, int grounded, int inverted, int flags) {
    if (slot < 0 || slot >= kSlots) return;
    DLoop& d = drv_[slot];
    if (kind != d.kind) { d.kind = kind; d.lvl = 0.0f; }
    d.spd_t = speed < 0.0f ? 0.0f : speed > 1.5f ? 1.5f : speed;
    d.turn_t = Clamp01(turn);
    d.gnd_t = grounded ? 1.0f : 0.0f;
    d.inv_t = inverted ? 1.0f : 0.0f;
    d.flags = flags;
    if (kind == DK_WHEEL_TWO || kind == DK_WHEEL_FOUR || kind == DK_WHEEL_SIX) {
      if (d.skid_cd > 0) d.skid_cd--;
      if (grounded && turn > 0.7f && speed > 0.5f && d.skid_cd == 0) { Fx(SFX_SKID, 0, d.pan); d.skid_cd = 20; }
    }
  }
  void Damage(int slot, float hp, int burning, int alive) {
    if (slot < 0 || slot >= kSlots) return;
    DmgLoop& g = dmg_[slot];
    g.hpf = Clamp01(hp); g.burn = burning ? 1.0f : 0.0f; g.alive = alive != 0;
  }
  void Hazard(int slot, int type, float level, float pan, float aux) {
    if (slot < 0 || slot >= kHazLoops) return;
    HLoop& h = haz_[slot];
    if (type != h.type) { h.type = type; h.lvl = 0.0f; h.a.Reset(); h.b.Reset(); }
    h.lvl_t = Clamp01(level); h.pan = pan; h.aux = aux;
  }
  void SlotPan(int slot, float pan) {
    if (slot < 0 || slot >= kSlots) return;
    wpn_[slot].pan = drv_[slot].pan = dmg_[slot].pan = pan;
  }
  void LoopsOff() {
    for (int s = 0; s < kSlots; ++s) { Weapon(s, WK_NONE, 0, 0, 0); drv_[s].kind = DK_NONE; drv_[s].lvl = 0.0f; dmg_[s].alive = false; dmg_[s].burn = 0.0f; }
    for (auto& h : haz_) h.lvl_t = 0.0f;
  }
  /* the old spinner whine: a seat's horizontal spinner loop */
  void Whine(int slot, float level, float pitch) {
    if (slot < 0 || slot > 3) return;
    float spin = Clamp01(level / 0.8f);
    Weapon(slot, WK_SPIN_HORIZ, spin > 0.0f ? 1.0f : 0.0f, spin, 0.0f);
    (void)pitch;
  }

  /* -- the garage (3.9): the part's own sound; the weapon rows and the RT
   * test fire play the weapon's tell and its strike with no victim, the
   * first time a new player hears what the button does --------------- */
  void Part(int row, int opt) {
    const float r = rng_bells_.Next();
    switch (row) {
      case 0:   /* chassis: the shell rings, bigger is lower */
        { static const float kPitch[6] = {64, 62, 60, 55, 70, 66};
          R(0, P_STING, BUS_IMPACT, 0.0f, 0.0f, Mtof(kPitch[opt < 0 ? 0 : opt > 5 ? 5 : opt] + r * 2), opt == 3 ? 0.8f : 0.55f, 0.6f); }
        break;
      case 1:   /* drive */
        switch (opt) {
          case 0: Blip(0, 240.0f); break;                                                   /* two wheel: a quick spin */
          case 1: Blip(0, 200.0f); Blip(90, 200.0f); break;
          case 2: Blip(0, 170.0f); Blip(80, 170.0f); Blip(160, 170.0f); break;
          case 3:                                                                           /* treads: the thud, the links */
            T(0, P_STING, BUS_IMPACT, 0.0f, 55.0f, 0.5f, 0.2f, 3.0f);
            { Burst* b = B(0, P_STING, BUS_FX, 0.0f, NoiseColor::kPink, SvfMode::kLp, 260.0f, 0.8f, 0.3f, 0.9f); Shape(b, 0.02f, 0.15f); }
            R(0, P_STING, BUS_IMPACT, 0.0f, 0.0f, Mtof(48), 0.4f, 0.6f); R(140, P_STING, BUS_IMPACT, 0.0f, 0.0f, Mtof(48), 0.4f, 0.6f);
            break;
          case 4: { Burst* b = B(0, P_STING, BUS_FX, 0.0f, NoiseColor::kPink, SvfMode::kBp, 240.0f, 1.2f, 0.3f, 1.2f); SweepTo(b, 320.0f, 0.2f); Shape(b, 0.05f, 0.2f); break; }   /* hover: the blower */
          case 5: WalkStep(0, 0.0f); WalkStep(180, 0.0f); break;                                                /* walker: two steps */
          case 6: T(0, P_STING, BUS_IMPACT, 0.0f, 55.0f, 0.4f, 0.2f, 3.0f); R(120, P_STING, BUS_IMPACT, 0.0f, 0.0f, Mtof(50), 0.4f, 0.6f); break;   /* leg tracks */
          default: S(0, P_STING, BUS_FX, 0.0f, SH_SAW, 300.0f, 620.0f, 0.25f, 0.15f, 0.01f, 0.05f, 2500.0f); break;   /* melty */
        }
        break;
      case 2: case 5: TestFire(opt); break;   /* weapon, 2nd weapon */
      case 3:   /* armour: the material rings */
        { static const float kPitch[7] = {72, 50, 79, 60, 64, 83, 55};
          static const float kVel[7] = {0.2f, 0.35f, 0.5f, 0.3f, 0.6f, 0.6f, 0.7f};
          static const float kMat[7] = {0, 4, 0, 0, 0, 0, 0};
          int o = opt < 0 ? 0 : opt > 6 ? 6 : opt;
          R(0, P_STING, BUS_IMPACT, 0.0f, kMat[o], Mtof(kPitch[o] + r * 2), kVel[o], o == A_PLY ? 1.0f : 0.6f);
          if (o == A_WEAR) R(0, P_STING, BUS_IMPACT, 0.0f, 0.0f, Mtof(kPitch[o] + r * 2) * 1.00405f, kVel[o] * 0.8f, 0.6f); }
        break;
      default:  /* paint */
        Push(0, I_BELL, Mtof(79 + (opt & 3) * 2), 0.35f);
        break;
    }
  }
  void Blip(float at, float hz) { S(at, P_STING, BUS_FX, 0.0f, SH_SAW, hz, hz * 1.3f, 0.08f, 0.14f, 0.005f, 0.03f, 2000.0f); }
  void WalkStep(float at, float pan) {
    B(at, P_DEBRIS, BUS_FX, pan, NoiseColor::kPink, SvfMode::kLp, 300.0f, 0.8f, 0.08f, 1.0f);
    R(at, P_DEBRIS, BUS_FX, pan, 0.0f, 180.0f * Jit(0.05f), 0.25f, 0.2f);
  }
  void TestFire(int w) {
    switch (w) {
      case W_NONE: Push(0, I_HAT, 700, 0.2f); break;
      case W_WEDGE:
        R(0, P_STING, BUS_IMPACT, 0.0f, 0.0f, Mtof(52), 0.6f, 0.5f);
        { Burst* b = B(0, P_STING, BUS_FX, 0.0f, NoiseColor::kPink, SvfMode::kBp, 1300.0f, 2.0f, 0.2f, 0.5f); Wobble(b, 0.3f, 9.0f); }
        break;
      case W_SPIKE: Tell(-1, W_SPIKE, T_SPIKE_OUT, 0.0f); base_ms_ = 90.0f; Hit(HK_SPIKE, B_HIT, 0.6f, 0.0f, -1, -1); base_ms_ = 0.0f; break;
      case W_LIFTER:
        S(0, P_STING, BUS_FX, 0.0f, SH_TRI, 160.0f, 220.0f, 0.3f, 0.15f, 0.02f, 0.08f);
        base_ms_ = 300.0f; Hit(HK_GENERIC, B_TAP, 0.5f, 0.0f, -1, -1); base_ms_ = 0.0f;
        break;
      case W_SAW:
        Tell(-1, W_SAW, T_SAW_ON, 0.0f);
        S(0, P_STING, BUS_FX, 0.0f, SH_SAW, 200.0f, 900.0f, 0.45f, 0.12f, 0.02f, 0.05f, 2400.0f);
        B(450, P_STING, BUS_FX, 0.0f, NoiseColor::kWhite, SvfMode::kBp, 2200.0f, 2.5f, 0.25f, 0.5f);
        base_ms_ = 450.0f; Hit(HK_SAW, B_HIT, 0.5f, 0.0f, -1, -1); base_ms_ = 0.0f;
        break;
      case W_FLAME: {
        Tell(-1, W_FLAME, T_FLAME_ON, 0.0f);
        Burst* b = B(80, P_STING, BUS_FX, 0.0f, NoiseColor::kBrown, SvfMode::kLp, 500.0f, 0.7f, 0.25f, 1.2f); Shape(b, 0.08f, 0.35f);
        break; }
      case W_HAMMER: Tell(-1, W_HAMMER, T_WINDUP, 0.0f); base_ms_ = 260.0f; Hit(HK_HAMMER, B_HEAVY, 0.7f, 0.0f, -1, -1); base_ms_ = 0.0f; break;
      case W_FLIPPER: Tell(-1, W_FLIPPER, T_WINDUP, 0.0f); break;
      case W_DRUM: case W_HORIZ: {
        const float f0 = w == W_DRUM ? 120.0f : 90.0f, f1 = w == W_DRUM ? 680.0f : 820.0f;
        S(0, P_STING, BUS_FX, 0.0f, SH_SAW, f0, f1, 0.6f, 0.14f, 0.05f, 0.05f, 2000.0f);
        base_ms_ = 600.0f; Hit(HK_SPINNER, B_HEAVY, 0.7f, 0.0f, -1, -1); base_ms_ = 0.0f;
        break; }
      default:   /* crusher */
        Tell(-1, W_CRUSHER, T_CLAMP_START, 0.0f);
        base_ms_ = 400.0f; Tell(-1, W_CRUSHER, T_CLAMP_BITE, 0.0f); base_ms_ = 0.0f;
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
      tilt_.Process(l, r, i, i + span);
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
  /* bus trims at SFX 10, measured by tools/bake_music.cpp (deep-audio 6.1) */
  static constexpr float kXt = 1.0f, kBody = 1.0f;
  static constexpr float kPunchIn = 0.2f;
  static constexpr float kImpactBus = 0.28f, kFxBus = 0.5f, kCrowdBus = 0.5f;
  static constexpr float kWhine = 0.76f, kGrind = 1.5f, kRoar = 2.4f, kScrape = 1.1f, kCreak = 1.0f;
  static constexpr float kTread = 0.8f, kWheel = 0.45f, kWheelWhine = 0.08f, kHover = 0.9f;
  static constexpr float kFizz = 1.4f, kHaz = 0.75f, kCrowdBed = 5.0f, kShout = 0.1f;

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
          case I_BUZZ: buzz_.NoteOn(v.freq, v.vel); buzz_live_ = true; break;
          case I_BUZZ_OFF: if (v.gen == buzz_gen_ && buzz_live_) { buzz_.NoteOff(); buzz_live_ = false; } break;
          case I_BELL: bells_.NoteOn(note_id_++ & 0xfff, v.freq, v.vel, frame_); break;
          case I_GONG: gong_.NoteOn(v.freq, v.vel); break;
          case I_HAT: tick_.NoteOn(v.freq, v.vel); break;
          case I_CLAP: clap_.NoteOn(1400.0f, v.vel); break;
          default: break;
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
    float gl = PanL(pan), gr = PanR(pan);
    for (int i = 0; i < n; ++i) {
      float a = dl_[i] * gl, b = dr_[i] * gr;
      l[from + i] += dry * a; r[from + i] += dry * b;
      sl_[i] += dly * a; sr_buf_[i] += dly * b;
    }
  }

  /* -- the one shot runners: each adds its voice into its bus ---------- */
  float* BusL(uint8_t bus) { return bus == BUS_IMPACT ? il_ : bus == BUS_FX ? fl_ : cl_; }
  float* BusR(uint8_t bus) { return bus == BUS_IMPACT ? ir_ : bus == BUS_FX ? fr_ : cr_; }

  void RunBurst(Burst& v, int n) {
    v.age += n;
    int i = 0;
    if (v.wait > 0) { i = v.wait < n ? v.wait : n; v.wait -= i; if (i >= n) return; }
    float fc = v.f0;
    if (v.sweep > 0) { float x = v.t >= v.sweep ? 1.0f : static_cast<float>(v.t) / static_cast<float>(v.sweep); fc = v.f0 * bellows::fm::Pow(v.f1 / v.f0, x); }
    if (v.lfo > 0.0f) {
      fc *= 1.0f + v.lfo * bellows::fm::Sin(v.lfo_ph);
      v.lfo_ph += k2Pi * v.lfo_hz * static_cast<float>(n - i) / sr_;
      if (v.lfo_ph > k2Pi) v.lfo_ph -= k2Pi;
    }
    if (v.sweep > 0 || v.lfo > 0.0f) v.f.Set(fc, v.q);
    float* L = BusL(v.bus); float* Rr = BusR(v.bus);
    const float gl = PanL(v.pan) * v.gain, gr = PanR(v.pan) * v.gain;
    const int ah = v.att + v.hold;
    for (; i < n; ++i) {
      float a = v.t < v.att ? static_cast<float>(v.t + 1) / static_cast<float>(v.att) : v.t < ah ? 1.0f : v.e.Process();
      ++v.t;
      float y = v.f.Process(v.n.Process()) * a;
      L[i] += y * gl; Rr[i] += y * gr;
    }
    if (v.t >= ah && v.e.Level() < 1e-4f) v.live = 0;
  }
  void RunCrackle(Crackle& v, int n) {
    v.age += n;
    int i = 0;
    if (v.wait > 0) { i = v.wait < n ? v.wait : n; v.wait -= i; if (i >= n) return; }
    float* L = BusL(v.bus); float* Rr = BusR(v.bus);
    const float gl = PanL(v.pan) * v.gain * 4.0f, gr = PanR(v.pan) * v.gain * 4.0f;
    for (; i < n; ++i) {
      float a = v.t < v.hold ? 1.0f : v.e.Process();
      ++v.t;
      float x = rng_spark_.Next() < v.dens ? rng_spark_.Bipolar() : 0.0f;
      float y = v.hp.Process(x) * a;
      L[i] += y * gl; Rr[i] += y * gr;
    }
    if (v.t >= v.hold && v.e.Level() < 1e-4f) v.live = 0;
  }
  void RunRing(Ring& v, int n) {
    v.age += n;
    int i0 = 0;
    if (!v.started) {
      if (v.wait >= n) { v.wait -= n; return; }
      i0 = v.wait; v.wait = 0;
      v.m.NoteOn(v.freq, v.vel); v.started = true;
    }
    for (int i = i0; i < n; ++i) { tl_[i] = 0.0f; tr_[i] = 0.0f; }
    v.m.Process(tl_, tr_, i0, n);
    float* L = BusL(v.bus); float* Rr = BusR(v.bus);
    const float gl = PanL(v.pan) * v.gain, gr = PanR(v.pan) * v.gain;
    for (int i = i0; i < n; ++i) { L[i] += tl_[i] * gl; Rr[i] += tr_[i] * gr; }
    if (!v.m.Active()) v.live = 0;
  }
  void RunThud(Thud& v, int n) {
    v.age += n;
    int i0 = 0;
    if (!v.started) {
      if (v.wait >= n) { v.wait -= n; return; }
      i0 = v.wait; v.wait = 0;
      v.k.NoteOn(v.freq, v.vel); v.started = true;
    }
    for (int i = i0; i < n; ++i) { tl_[i] = 0.0f; tr_[i] = 0.0f; }
    v.k.Process(tl_, tr_, i0, n);
    float* L = BusL(v.bus); float* Rr = BusR(v.bus);
    const float gl = PanL(v.pan) * v.gain, gr = PanR(v.pan) * v.gain;
    for (int i = i0; i < n; ++i) { L[i] += tl_[i] * gl; Rr[i] += tr_[i] * gr; }
    if (!v.k.Active()) v.live = 0;
  }
  void RunSweep(Sweep& v, int n) {
    v.age += n;
    int i = 0;
    if (v.wait > 0) { i = v.wait < n ? v.wait : n; v.wait -= i; if (i >= n) return; }
    float x = static_cast<float>(v.t) / static_cast<float>(v.len);
    float hz = v.f0 * bellows::fm::Pow(v.f1 / v.f0, x > 1.0f ? 1.0f : x) * slow_;
    const float inc = hz / sr_;
    float* L = BusL(v.bus); float* Rr = BusR(v.bus);
    const float gl = PanL(v.pan) * v.gain, gr = PanR(v.pan) * v.gain;
    for (; i < n; ++i) {
      if (v.t >= v.len) { v.live = 0; break; }
      float a = v.t < v.att ? static_cast<float>(v.t) / static_cast<float>(v.att) : v.t > v.len - v.rel ? static_cast<float>(v.len - v.t) / static_cast<float>(v.rel) : 1.0f;
      ++v.t;
      v.ph += inc; if (v.ph >= 1.0f) v.ph -= 1.0f;
      float y;
      if (v.shape == SH_SAW) y = v.lp.Process(2.0f * v.ph - 1.0f);
      else if (v.shape == SH_TRI) y = 1.0f - 4.0f * fabsf(v.ph - 0.5f);
      else y = bellows::fm::Sin(k2Pi * v.ph);
      y *= a;
      L[i] += y * gl; Rr[i] += y * gr;
    }
  }

  /* -- the loops ------------------------------------------------------ */
  static float ToneHz(int kind, float p) {
    switch (kind) {
      case WK_SPIN_HORIZ: return 90.0f + 730.0f * p;
      case WK_SPIN_DRUM: return 120.0f + 560.0f * p;
      case WK_MELTY: return 70.0f + 450.0f * p;
      case WK_SAW: return 200.0f + 700.0f * p;
      case WK_LIFTER_SERVO: return 160.0f + 40.0f * p;
      case WK_HAMMER_ARM: return 200.0f + 700.0f * p;
      default: return 0.0f;
    }
  }
  static bool Spinner(int kind) { return kind == WK_SPIN_HORIZ || kind == WK_SPIN_DRUM || kind == WK_MELTY; }
  void SetToneKind(WLoop& w, int kind) {
    w.tkind = kind;
    bellows::Va::Params& p = w.tp_;
    p.shape = 0.0f; p.detune = 9.0f; p.sub = 0.0f; p.resonance = 0.0f; p.cutoff = 600.0f;
    if (kind == WK_SAW) { p.sub = 0.0f; p.resonance = 0.5f; p.cutoff = 2400.0f; p.detune = 6.0f; }
    if (kind == WK_LIFTER_SERVO) { p.shape = 2.0f; p.sub = 0.0f; p.resonance = 0.2f; p.cutoff = 1200.0f; p.detune = 4.0f; }
    if (kind == WK_HAMMER_ARM) { p.sub = 0.0f; p.cutoff = 1500.0f; }
    w.cut = p.cutoff; w.det = p.detune;
    w.tone.SetParams(p);
    if (!w.ton) { w.tone.NoteOn(ToneHz(kind, 0.0f), 1.0f); w.ton = true; }
  }
  /* the tone's level law (3.4): a spinner is 0.15 + 0.55 spin, silent at rest */
  static float ToneAmp(int kind, float lvl, float p) {
    if (lvl <= 0.0f) return 0.0f;
    switch (kind) {
      case WK_SPIN_HORIZ: case WK_SPIN_DRUM: case WK_MELTY: return p > 0.01f ? lvl * (0.15f + 0.55f * p) : 0.0f;
      case WK_SAW: return p > 0.01f ? lvl * (0.05f + 0.2f * p) : 0.0f;   /* thinner than a disc: the grind is the saw's voice */
      case WK_LIFTER_SERVO: return lvl * 0.25f;
      case WK_HAMMER_ARM: return lvl * 0.3f;
      default: return 0.0f;
    }
  }
  static float Coef(int n, float tau, float sr) { return 1.0f - bellows::fm::Exp(-static_cast<float>(n) / (tau * sr)); }

  void RunWeapon(int s, float* l, float* r, int from, int n, float g) {
    WLoop& w = wpn_[s];
    const float gl = PanL(w.pan) * g, gr = PanR(w.pan) * g;
    /* the tone */
    const float amp_t = ToneAmp(w.tkind, w.lvl, w.pit);
    const float t0 = w.tl;
    w.tl += (amp_t - w.tl) * 0.5f;
    if (w.tl < 1e-5f && amp_t <= 0.0f) w.tl = 0.0f;
    w.tp += (w.pit - w.tp) * 0.6f;
    if (w.ton && (w.tl > 0.0f || t0 > 0.0f || w.kill > 0)) {
      float hz = ToneHz(w.tkind, w.tp), g0 = t0, g1 = w.tl;
      if (Spinner(w.tkind) && w.tp > 0.3f) {
        hz *= 1.0f + 0.025f * bellows::fm::Sin(w.wob) + 0.01f * bellows::fm::Sin(w.drift);
      }
      w.wob += k2Pi * 6.5f * n / sr_; if (w.wob > k2Pi) w.wob -= k2Pi;
      w.drift += k2Pi * 0.4f * n / sr_; if (w.drift > k2Pi) w.drift -= k2Pi;
      if (w.tkind == WK_LIFTER_SERVO) { float tr = 1.0f - 0.25f * (0.5f + 0.5f * bellows::fm::Sin(w.wob * (4.0f / 6.5f))); g0 *= tr; g1 *= tr; }
      if (w.dip > 0) {
        float d = static_cast<float>(w.dip) / static_cast<float>(w.dip_len);
        hz *= 1.0f - 0.12f * d; g0 *= 1.0f - 0.37f * d; g1 *= 1.0f - 0.37f * d;
        w.dip -= n;
      }
      if (w.kill > 0) {
        float x0 = 1.0f - static_cast<float>(w.kill) / static_cast<float>(w.kill_len);
        w.kill -= n; if (w.kill < 0) w.kill = 0;
        float x1 = 1.0f - static_cast<float>(w.kill) / static_cast<float>(w.kill_len);
        hz = w.kill_hz * bellows::fm::Pow(w.kill_to / w.kill_hz, x0);
        g0 = w.kill_g * (1.0f - x0) + g0 * x0; g1 = w.kill_g * (1.0f - x1) + g1 * x1;
      }
      hz *= slow_;
      w.tone.SetFreq(hz);
      float cut = Spinner(w.tkind) ? (1000.0f + 5000.0f * w.tp) * slow_ : w.tp_.cutoff;   /* the teeth's harmonics come through as it climbs */
      float det = w.tkind == WK_MELTY ? 1731.0f * 1.5f / (hz > 20.0f ? hz : 20.0f) : w.tp_.detune;
      if (det > 60.0f) det = 60.0f;
      if (fabsf(cut - w.cut) > 0.03f * w.cut || fabsf(det - w.det) > 0.5f) {
        bellows::Va::Params p = w.tp_; p.cutoff = cut; p.detune = det; w.tone.SetParams(p); w.cut = cut; w.det = det;
      }
      for (int i = 0; i < n; ++i) { tl_[i] = 0.0f; tr_[i] = 0.0f; }
      w.tone.Process(tl_, tr_, 0, n);
      /* the air over a spinner: white noise at twice the fundamental */
      const bool air = Spinner(w.tkind);
      if (air) w.air.Set(2.0f * hz, 3.0f);
      const float inv = 1.0f / static_cast<float>(n);
      for (int i = 0; i < n; ++i) {
        float a = (g0 + (g1 - g0) * static_cast<float>(i) * inv) * kWhine;
        float y = tl_[i];
        if (air) y += 0.08f * w.air.Process(w.n.Process());
        l[from + i] += y * a * gl; r[from + i] += y * a * gr;
      }
      /* a spent disc ticks as it stops */
      if (Spinner(w.tkind) && w.kill == 0 && w.pit < 0.35f && w.pit > 0.03f && w.pit < w.ptp - 1e-5f) {
        w.detent -= n;
        if (w.detent <= 0) { R(0, P_DEBRIS, BUS_FX, w.pan, 0.0f, 1200.0f * Jit(0.05f), 0.12f, 0.08f); w.detent = Ms(40); }
      }
    }
    /* the noise layers */
    const float grind_tt = (w.tkind == WK_SAW && w.lvl > 0.0f) ? (w.grind_t > 0.0f || w.grind_hold > 0 ? 0.5f : 0.0f) : 0.0f;
    if (w.grind_hold > 0) w.grind_hold -= n;
    w.grind_l += (grind_tt - w.grind_l) * Coef(n, 0.02f, sr_);
    w.roar_l += (w.roar_t * 0.6f - w.roar_l) * Coef(n, 0.03f, sr_);
    w.scr_l += (w.scr_t - w.scr_l) * Coef(n, 0.03f, sr_);
    w.crk_l += (w.crk_t * 0.3f - w.crk_l) * Coef(n, 0.05f, sr_);
    const bool grind = w.grind_l > 1e-4f, roar = w.roar_l > 1e-4f, scr = w.scr_l > 1e-4f, crk = w.crk_l > 1e-4f;
    if (!(grind || roar || scr || crk)) return;
    float rf = 1.0f;
    if (roar) { w.flut += k2Pi * 9.0f * n / sr_; if (w.flut > k2Pi) w.flut -= k2Pi; rf = 1.0f + 0.3f * bellows::fm::Sin(w.flut); }
    float scr_norm = 1.0f;
    if (scr) {
      w.scr_ph += w.scr_rate * n / sr_; w.scr_ph -= floorf(w.scr_ph);
      float tri = 2.0f * fabsf(w.scr_ph - 0.5f), sweep = bellows::fm::Pow(2.5f, tri);
      w.scrape.Set(600.0f * sweep * slow_, 1.2f);
      scr_norm = 1.0f / sqrtf(sweep);   /* a wider band passes more noise: keep the level as it sweeps */
    }
    if (crk) {
      w.crk_ph += k2Pi * 6.0f * n / sr_; if (w.crk_ph > k2Pi) w.crk_ph -= k2Pi;
      w.creak.Set(400.0f * (1.0f + 0.3f * bellows::fm::Sin(w.crk_ph)) * slow_, 4.0f);
      w.crk_tick -= n;
      if (w.crk_tick <= 0) { if (w.crk_l > 0.05f) R(0, P_DEBRIS, BUS_FX, w.pan, 0.0f, 165.0f * Jit(0.05f), 0.2f, 0.4f); w.crk_tick = Ms(333); }
    }
    if (grind) w.grind.Set(2200.0f * slow_, 0.8f);
    const float teeth_inc = 41.0f * slow_ / sr_, gdec = bellows::fm::Exp(-1.0f / (0.02f * sr_));
    const float gk = w.grind_l * kGrind, rk = w.roar_l * kRoar * rf, sk = w.scr_l * kScrape * scr_norm, ck = w.crk_l * kCreak;
    for (int i = 0; i < n; ++i) {
      const float x = w.n.Process();
      float y = 0.0f;
      if (grind) {
        w.teeth += teeth_inc; if (w.teeth >= 1.0f) w.teeth -= 1.0f;
        float tri = 1.0f - 2.0f * fabsf(w.teeth - 0.5f);
        y += w.grind.Process(x) * (0.55f + 0.45f * tri) * gk;
        if (--w.grit <= 0) { w.grit_env = 0.6f + 0.4f * rng_loop_.Next(); w.grit = Ms(12); }
        w.grit_env *= gdec;
        if (rng_loop_.Next() < 0.5f) y += rng_loop_.Bipolar() * w.grit_env * 0.4f * gk;
      }
      if (roar) {
        w.brown = (w.brown + 0.02f * x) / 1.02f;
        y += w.roar.Process(w.brown * 2.0f + x * 0.25f) * rk + w.hiss.Process(x) * 0.08f * rk;
      }
      if (scr) {
        y += w.scrape.Process(x) * sk;
        if (rng_loop_.Next() < 0.1f) y += rng_loop_.Bipolar() * 0.02f * sk;
      }
      if (crk) y += w.creak.Process(x) * ck;
      l[from + i] += y * gl; r[from + i] += y * gr;
    }
  }

  void RunDrive(int s, float* l, float* r, int from, int n, float g) {
    DLoop& d = drv_[s];
    const float want = (d.kind != DK_NONE && !(d.flags & DRV_FALLING)) ? 1.0f : 0.0f;
    const float c = Coef(n, 0.06f, sr_);
    d.lvl += (want - d.lvl) * Coef(n, 0.05f, sr_);
    if (d.lvl < 1e-4f && want <= 0.0f) { d.lvl = 0.0f; return; }
    d.spd += (d.spd_t - d.spd) * c;
    d.gnd += (d.gnd_t - d.gnd) * c;
    d.inv += (d.inv_t - d.inv) * c;
    d.boost += (((d.flags & DRV_BOOST) ? 1.0f : 0.0f) - d.boost) * Coef(n, 0.05f, sr_);
    d.pinch += (((d.flags & DRV_PINCH) ? 1.0f : 0.0f) - d.pinch) * c;
    d.stuck += (((d.flags & DRV_STUCK) ? 1.0f : 0.0f) - d.stuck) * c;
    const float spd = d.spd > 1.0f ? 1.0f : d.spd, air = 1.0f - d.gnd;
    float pm = slow_ * (1.0f + 0.5f * d.boost) * (1.0f - 0.3f * d.inv * (spd > 0.1f ? 1.0f : 0.0f)) * (1.0f + 0.4f * air);
    /* the amplitude wobbles: the inverted grind, the rattle of a dying bot */
    d.trem += k2Pi * 3.0f * n / sr_; if (d.trem > k2Pi) d.trem -= k2Pi;
    d.wob += k2Pi * 0.5f * n / sr_; if (d.wob > k2Pi) d.wob -= k2Pi;
    float am = d.lvl * g;
    if (d.inv > 0.5f && spd > 0.1f) am *= 0.7f + 0.3f * bellows::fm::Sin(d.trem);
    if (dmg_[s].alive && dmg_[s].hpf < 0.2f) am *= 1.0f - 0.6f * (0.5f + 0.5f * bellows::fm::Sin(d.trem * (11.0f / 3.0f)));
    const float gl = PanL(d.pan) * am, gr = PanR(d.pan) * am;
    switch (d.kind) {
      case DK_TREAD: case DK_LEGTRACK: {
        d.bed.SetMode(SvfMode::kLp); d.bed.Set((180.0f + 120.0f * spd) * pm, 0.8f);
        const float amp = (0.12f + 0.88f * spd) * (d.kind == DK_TREAD ? 1.3f : 1.0f) * (1.0f - 0.6f * air) * kTread;
        const float link = (7.0f + 9.0f * spd) * pm / sr_, ldec = bellows::fm::Exp(-1.0f / (0.025f * sr_)), cdec = bellows::fm::Exp(-1.0f / (0.004f * sr_));
        const float clank = 0.12f * amp * (0.3f + 0.7f * spd);
        for (int i = 0; i < n; ++i) {
          d.link_ph += link; if (d.link_ph >= 1.0f) { d.link_ph -= 1.0f; d.link_env = 1.0f; d.clank = 1.0f; }
          d.link_env *= ldec; d.clank *= cdec;
          /* the bed with the link tremolo, and each link's steel clank (white noise through the hiss highpass) */
          float y = d.bed.Process(pink_[i]) * amp * (0.55f + 0.45f * d.link_env) + d.clk.Process(white_[i]) * d.clank * clank;
          l[from + i] += y * gl; r[from + i] += y * gr;
        }
        break; }
      case DK_WHEEL_TWO: case DK_WHEEL_FOUR: case DK_WHEEL_SIX: case DK_MELTY: {
        const float ratio = d.kind == DK_WHEEL_TWO ? 1.5f : d.kind == DK_WHEEL_SIX ? 0.667f : 1.0f;
        const bool melty = d.kind == DK_MELTY;
        d.bed.SetMode(SvfMode::kLp); d.bed.Set(400.0f * pm, 0.7f);
        const float bed = melty ? 0.0f : 0.3f * spd * d.gnd * kWheel;
        const float wh = (melty ? 0.05f : 0.08f) * (0.25f + 0.75f * spd) * (d.gnd + 0.4f * air) / 0.08f * kWheelWhine;
        const float inc = (melty ? 200.0f + 600.0f * spd : (120.0f + 500.0f * spd) * ratio) * pm / sr_;
        for (int i = 0; i < n; ++i) {
          d.whine_ph += inc; if (d.whine_ph >= 1.0f) d.whine_ph -= 1.0f;
          float y = (1.0f - 4.0f * fabsf(d.whine_ph - 0.5f)) * wh;
          if (bed > 0.0f) y += d.bed.Process(pink_[i]) * bed;
          l[from + i] += y * gl; r[from + i] += y * gr;
        }
        break; }
      case DK_HOVER: {
        const float thr = (d.flags & DRV_THROTTLE) ? 1.0f : 0.0f;
        d.bed.SetMode(SvfMode::kBp); d.bed.Set(220.0f * pm * (1.0f + 0.3f * thr) * (1.0f - 0.2f * d.pinch), 1.6f);
        const float amp = 1.0f * (1.0f + 0.1f * bellows::fm::Sin(d.wob)) * kHover;
        for (int i = 0; i < n; ++i) {
          float y = d.bed.Process(pink_[i]) * amp + d.hiss.Process(white_[i]) * 0.008f * kHover;
          l[from + i] += y * gl; r[from + i] += y * gr;
        }
        break; }
      case DK_WALKER:
        if (spd > 0.05f && d.gnd > 0.5f && d.lvl > 0.5f) {
          d.step -= n;
          if (d.step <= 0) { WalkStep(0, d.pan); d.step = Ms(300.0f / (0.3f + spd)); }
        }
        break;
      default: break;
    }
    /* the squeal: stuck on the spot */
    if (d.stuck > 0.01f) {
      const float inc = 1800.0f * pm / sr_, sq = 0.3f * d.stuck * kWheelWhine;
      for (int i = 0; i < n; ++i) {
        d.sq_ph += inc; if (d.sq_ph >= 1.0f) d.sq_ph -= 1.0f;
        float y = bellows::fm::Sin(k2Pi * d.sq_ph) * sq;
        l[from + i] += y * gl; r[from + i] += y * gr;
      }
    }
  }

  void RunDamage(int s, float* l, float* r, int from, int n, float g) {
    DmgLoop& d = dmg_[s];
    float want = 0.0f, dens = 0.0f;
    if (d.alive && d.hpf < 0.4f) {
      want = 0.18f; dens = 0.006f + 0.02f * (0.4f - d.hpf);
      if (d.hpf < 0.2f) dens *= 2.0f;
      d.next_burst -= n;
      if (d.next_burst <= 0) {
        if (d.burst <= 0 && d.fizz > 0.05f) { d.burst = Ms(300); R(0, P_DEBRIS, BUS_FX, d.pan, 3.0f, 2600.0f * Jit(0.05f), 0.05f, 0.15f); }
        d.next_burst = Ms(1500.0f + 2500.0f * rng_dmg_.Next());
      }
      if (d.burst > 0) { d.burst -= n; want = 0.2f; dens = 0.03f; }
      if (d.hpf < 0.2f) {
        d.next_pop -= n;
        if (d.next_pop <= 0) { B(0, P_DEBRIS, BUS_FX, d.pan, NoiseColor::kPink, SvfMode::kLp, 200.0f, 0.8f, 0.05f, 0.25f); d.next_pop = Ms(400.0f + 500.0f * rng_dmg_.Next()); }
      }
    }
    if (d.burn > 0.0f) {
      want = want > 0.25f ? want : 0.25f; dens += 0.02f;
      d.next_burn -= n;
      if (d.next_burn <= 0) { B(0, P_DEBRIS, BUS_FX, d.pan, NoiseColor::kWhite, SvfMode::kBp, 1000.0f, 1.0f, 0.02f, 0.2f); d.next_burn = Ms(200.0f + 300.0f * rng_dmg_.Next()); }
    }
    d.fizz += (want - d.fizz) * Coef(n, 0.05f, sr_);
    if (d.fizz < 1e-4f && want <= 0.0f) { d.fizz = 0.0f; return; }
    /* the fizz: highpassed noise under a gate that flickers every millisecond with probability set by dens */
    const float k = d.fizz * kFizz * g, gl = PanL(d.pan) * k, gr = PanR(d.pan) * k;
    const float p_on = dens * 25.0f > 0.9f ? 0.9f : dens * 25.0f, sm = 1.0f - bellows::fm::Exp(-1.0f / (0.0005f * sr_));
    for (int i = 0; i < n; ++i) {
      if (--d.flick <= 0) { d.gate = rng_dmg_.Next() < p_on ? 0.4f + 0.6f * rng_dmg_.Next() : 0.0f; d.flick = Ms(1.0f); }
      d.gs += (d.gate - d.gs) * sm;
      float y = d.hp.Process(white_[i]) * d.gs;
      l[from + i] += y * gl; r[from + i] += y * gr;
    }
  }

  void RunHazard(HLoop& h, float* l, float* r, int from, int n, float g) {
    h.lvl += (h.lvl_t - h.lvl) * Coef(n, 0.1f, sr_);
    if (h.lvl < 1e-4f && h.lvl_t <= 0.0f) { h.lvl = 0.0f; return; }
    const float k = h.lvl * kHaz * g, gl = PanL(h.pan) * k, gr = PanR(h.pan) * k;
    switch (h.type) {
      case Z_KILLSAW: {
        h.a.SetMode(SvfMode::kBp); h.a.Set(2860.0f * slow_, 2.5f);
        const float inc = 53.0f * slow_ / sr_;
        for (int i = 0; i < n; ++i) {
          h.ph += inc; if (h.ph >= 1.0f) h.ph -= 1.0f;
          float y = h.a.Process(haz_noise_.Process()) * (0.55f + 0.45f * (1.0f - 2.0f * fabsf(h.ph - 0.5f))) * 0.4f;
          l[from + i] += y * gl; r[from + i] += y * gr;
        }
        break; }
      case Z_FLAME: {
        h.a.SetMode(SvfMode::kLp); h.a.Set(500.0f * slow_, 0.7f);
        h.b.SetMode(SvfMode::kBp); h.b.Set(3000.0f, 1.0f);
        h.lfo += k2Pi * 9.0f * n / sr_; if (h.lfo > k2Pi) h.lfo -= k2Pi;
        const float rf = 0.5f * (1.0f + 0.3f * bellows::fm::Sin(h.lfo));
        for (int i = 0; i < n; ++i) {
          float x = haz_noise_.Process();
          h.brown = (h.brown + 0.02f * x) / 1.02f;
          float y = (h.a.Process(h.brown * 3.5f) * 1.2f + h.b.Process(x) * 0.25f) * rf;
          l[from + i] += y * gl; r[from + i] += y * gr;
        }
        break; }
      case Z_SCREW: case Z_WIND: {
        const bool wind = h.type == Z_WIND;
        h.a.SetMode(SvfMode::kLp); h.a.Set((wind ? 600.0f : 150.0f) * slow_, 0.8f);
        h.lfo += k2Pi * (wind ? 0.3f : 3.0f) * n / sr_; if (h.lfo > k2Pi) h.lfo -= k2Pi;
        const float a = (wind ? 0.6f : 0.3f) * (0.7f + 0.3f * bellows::fm::Sin(h.lfo)) * (wind ? 1.0f : 4.0f);
        for (int i = 0; i < n; ++i) {
          float y = h.a.Process(pink_[i]) * a;
          l[from + i] += y * gl; r[from + i] += y * gr;
        }
        break; }
      case Z_LASER: {
        h.b.SetMode(SvfMode::kHp); h.b.Set(8000.0f, 0.7f);
        /* the hum: a 60 Hz phasor turned a sample at a time, renormalised per span */
        const float w = k2Pi * 60.0f * slow_ / sr_, cw = bellows::fm::Cos(w), sw = bellows::fm::Sin(w);
        const float bl = PanL(h.aux) * k, br = PanR(h.aux) * k;
        { float m = h.ph * h.ph + h.ph2 * h.ph2; if (m < 0.5f || m > 2.0f) { h.ph = 1.0f; h.ph2 = 0.0f; } else { m = 1.0f / sqrtf(m); h.ph *= m; h.ph2 *= m; } }
        for (int i = 0; i < n; ++i) {
          float c = h.ph * cw - h.ph2 * sw, sn = h.ph * sw + h.ph2 * cw; h.ph = c; h.ph2 = sn;
          float hum = (sn + sn * c) * 0.15f;   /* the fundamental and its octave, sin 2t = 2 sin t cos t at half */
          float beam = h.b.Process(haz_noise_.Process()) * 0.12f;
          l[from + i] += hum * gl + beam * bl; r[from + i] += hum * gr + beam * br;
        }
        break; }
      default: break;
    }
  }

  void RunCrowd(float* l, float* r, int from, int n, float g) {
    /* the swell: rise, hold, fall back over rel */
    if (swell_t_ > 0.0f) {
      if (swell_v_ < swell_t_ - 0.01f) swell_v_ += (swell_t_ - swell_v_) * swell_rate_;
      else if (swell_hold_ > 0) swell_hold_ -= n;
      else { swell_t_ -= static_cast<float>(n) / (swell_rel_ * sr_); if (swell_t_ < 0.0f) swell_t_ = 0.0f; swell_v_ = swell_t_; }
    }
    if (hush_ > 0) hush_ -= n;
    float bed = hush_ > 0 ? 0.04f : crowd_ot_ && crowd_bed_ > 0.0f ? (crowd_bed_ > 0.2f ? crowd_bed_ : 0.2f) : crowd_bed_;
    float target = swell_v_ > bed ? swell_v_ : bed;
    float c = target > crowd_gain_ ? 0.06f : 0.012f;
    crowd_gain_ += (target - crowd_gain_) * c;
    const float cg = kCrowdBed * g * crowd_gain_;
    const float lfo_inc = 0.3f * k2Pi / sr_;
    const bool bed_on = crowd_gain_ > 0.001f, rumble = crowd_ot_ && crowd_bed_ > 0.0f;
    const bool clap = clap_left_ > 0;
    if (clap) clap_left_ -= n;
    const bool shouting = shout_[0].on || shout_[1].on || shout_[2].on;
    if (bed_on || rumble || clap || shouting) {
      for (int i = 0; i < n; ++i) {
        float x = crowd_.Process();
        cn_[i] = x;
        float o = 0.0f;
        if (bed_on) {
          float y = crowd_bp1_.Process(x) + 0.6f * crowd_bp2_.Process(x);   /* the voices and the hands: the stand has a top end */
          crowd_lfo_ += lfo_inc;
          if (crowd_lfo_ > k2Pi) crowd_lfo_ -= k2Pi;
          o += y * (0.85f + 0.15f * bellows::fm::Sin(crowd_lfo_)) * cg;
        }
        if (rumble) { crowd_brown_ = (crowd_brown_ + 0.02f * rng_crowd_.Bipolar()) / 1.02f; o += crowd_rum_.Process(crowd_brown_ * 3.5f) * 0.12f * kCrowdBed * g; }
        if (clap && rng_crowd_.Next() < 0.03f) o += rng_crowd_.Bipolar() * clap_g_ * 0.5f * g;
        cl_[i] += o; cr_[i] += o * 0.9f;
      }
    }
    /* the shouts */
    for (int k = 0; k < 3; ++k) {
      Shout& s = shout_[k];
      if (!s.on) continue;
      const float x0 = static_cast<float>(s.t) / static_cast<float>(s.len > 0 ? s.len : 1);
      s.t += n;
      if (s.t < s.len) { float step = s.g_peak * static_cast<float>(n) / (s.rise * sr_ + 1.0f); s.g = s.g + step < s.g_peak ? s.g + step : s.g_peak; }
      else { s.g -= s.g_peak * static_cast<float>(n) / static_cast<float>(s.rel); if (s.g <= 0.0f) { s.g = 0.0f; s.on = false; continue; } }
      if (s.slide != 0.0f) { float m = 1.0f - s.slide * (x0 > 1.0f ? 1.0f : x0); for (int f = 0; f < 3; ++f) s.f[f].Set(s.hz[f] * m, f == 0 ? 3.0f : 5.0f); }
      const float sg = s.g * kShout * g, pan = (k - 1) * 0.3f, gl = PanL(pan) * sg, gr = PanR(pan) * sg;
      for (int i = 0; i < n; ++i) {
        float y = s.f[0].Process(cn_[i]) * s.a[0] + s.f[1].Process(cn_[i]) * s.a[1] + s.f[2].Process(cn_[i]) * s.a[2];
        cl_[i] += y * gl; cr_[i] += y * gr;
      }
    }
    /* the stomp on the beat grid */
    if (stomp_n_ > 0) {
      stomp_next_ -= n;
      if (stomp_next_ <= 0) { T(0, P_STING, BUS_CROWD, 0.0f, 55.0f, 0.3f, 0.2f, 2.5f); stomp_n_--; stomp_next_ += Ms(static_cast<float>(kBeatMs)); }
    }
    (void)l; (void)r; (void)from;
  }

  void UpdateDuck(int n) {
    const float dt = static_cast<float>(n) / sr_;
    switch (dk_stage_) {
      case 1: dk_ += dk_depth_ * dt / 0.015f; if (dk_ >= dk_depth_) { dk_ = dk_depth_; dk_stage_ = 2; } break;
      case 2: dk_hold_ -= n; if (dk_hold_ <= 0) dk_stage_ = 3; break;
      case 3: dk_ -= dk_depth_ * dt / dk_rel_; if (dk_ <= 0.0f) { dk_ = 0.0f; dk_stage_ = 0; } break;
      default: break;
    }
  }

  void SfxSpan(float* l, float* r, int from, int n, bool songs) {
    UpdateDuck(n);
    slow_ += (slow_t_ - slow_) * Coef(n, 0.03f, sr_);
    if (fall_recent_ > 0) fall_recent_ -= n;
    for (int i = 0; i < n; ++i) { sl_[i] = sr_buf_[i] = 0.0f; il_[i] = ir_[i] = fl_[i] = fr_[i] = cl_[i] = cr_[i] = 0.0f; }
    const float g = sfx_gain_;
    const float de = dk_ / 8.0f > 1.0f ? 1.0f : dk_ / 8.0f;
    const float duck3 = bellows::fm::DbToGain(-3.0f * de), duck6 = bellows::fm::DbToGain(-6.0f * de);
    /* the shared noise for the drive beds and the hazard beds */
    for (int i = 0; i < n; ++i) { pink_[i] = drive_noise_.Process(); white_[i] = rng_loop_.Bipolar(); }
    /* the one shots into their buses */
    for (auto& v : bursts_) if (v.live) RunBurst(v, n);
    for (auto& v : crk_) if (v.live) RunCrackle(v, n);
    for (auto& v : rings_) if (v.live) RunRing(v, n);
    for (auto& v : thuds_) if (v.live) RunThud(v, n);
    for (auto& v : sweeps_) if (v.live) RunSweep(v, n);
    /* the impact bus through one tanh for glue: in at kPunchIn so the tanh
     * only rounds the loudest peaks and the velocity survives, out at the bus trim */
    for (int i = 0; i < n; ++i) { il_[i] *= kPunchIn; ir_[i] *= kPunchIn; }
    punch_.Process(il_, ir_, 0, n);
    const float ig = kImpactBus / kPunchIn * g * duck_, fg = kFxBus * g * duck_ * duck3;
    for (int i = 0; i < n; ++i) { l[from + i] += il_[i] * ig + fl_[i] * fg; r[from + i] += ir_[i] * ig + fr_[i] * fg; }
    /* the loops */
    const float wg = g * duck_ * duck3, dg = g * duck_ * duck6;
    for (int s = 0; s < kSlots; ++s) {
      RunWeapon(s, l, r, from, n, wg);
      RunDrive(s, l, r, from, n, dg);
      RunDamage(s, l, r, from, n, wg);
    }
    for (auto& h : haz_) RunHazard(h, l, r, from, n, wg);
    /* the crowd: the bed, the shouts, the stomp, into the slap */
    RunCrowd(l, r, from, n, g * duck_ * duck3);
    const float cg = kCrowdBus;
    for (int i = 0; i < n; ++i) {
      l[from + i] += cl_[i] * cg; r[from + i] += cr_[i] * cg;
      sl_[i] += 0.3f * cl_[i] * cg; sr_buf_[i] += 0.3f * cr_[i] * cg;
    }
    /* the stingers on the palette, with the slap */
    if (buzz_.Active()) {
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      buzz_.Process(dl_, dr_, 0, n);
      buzz_sat_.Process(dl_, dr_, 0, n);
      const float bg = 0.95f * g, gl = PanL(buzz_pan_), gr = PanR(buzz_pan_);
      for (int i = 0; i < n; ++i) { l[from + i] += dl_[i] * bg * gl; r[from + i] += dr_[i] * bg * gr; }
    }
    Mix(bells_, 0.5f * g, 0.25f * g, 0.0f, l, r, from, n);
    Mix(gong_, 0.5f * g, 0.2f * g, 0.0f, l, r, from, n);
    Mix(tick_, 3.0f * g, 0.0f, 0.1f, l, r, from, n);
    Mix(clap_, 0.8f * g, 0.3f * g, -0.1f, l, r, from, n);
    slap_.Process(sl_, sr_buf_, 0, n);
    for (int i = 0; i < n; ++i) { l[from + i] += 0.5f * sl_[i]; r[from + i] += 0.5f * sr_buf_[i]; }
#if RTR_SONGS
    if (songs && mode_ > 0) {
      for (int i = 0; i < n; ++i) { yl_[i] = yr_[i] = 0.0f; }
      const float mg = music_gain_ * duck_ * kSongTrim[mode_] * MusicDuck();
      for (int i = 0; i < n; ++i) { ml_[i] = mr_[i] = 0.0f; }
      MusicSpan(ml_, mr_, 0, n, mg);
      echo_.Process(yl_, yr_, 0, n);
      const float eg = 0.6f * mg;
      for (int i = 0; i < n; ++i) { ml_[i] += eg * yl_[i]; mr_[i] += eg * yr_[i]; }
      tilt_.Process(ml_, mr_, 0, n);
      for (int i = 0; i < n; ++i) { l[from + i] += ml_[i]; r[from + i] += mr_[i]; }
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
    } else if (mode_ == 2) {   /* the kit voiced for the brief (5.2.1): the click a cabinet shows, a snare that cracks */
      kp.decay = 0.26f; kp.drive = 7.0f; kp.click_tune = 12.0f; kp.pitch_decay = 0.035f; kick_hz_ = 48.0f;
      sp.tone = 0.7f; sp.decay = 0.2f; sp.snap = 0.4f; snare_hz_ = 210.0f;
      hp.decay = 0.045f; hp.tone = 1.2f; oh.decay = 0.22f; oh.tone = 1.2f;
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
    bp.shape = 0.0f; bp.detune = 6.0f; bp.sub = mode_ == 1 ? 0.3f : 0.5f; bp.cutoff = 400.0f;   /* the garage sub down (5.2.6) */ bp.resonance = 0.4f; bp.filter_type = 0.0f;
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
    lead2_.SetParams(lead_p_);
  }

  void ApplyMode() {
    mode_ = pending_mode_;
    pending_mode_ = -1;
    section_ = 0; bar_ = 0; step_ = 0; pass_ = 0; countdown_ = 0; bass_gate_ = 0; lead_gate_ = 0; cur_lay_ = 0;
    mask_ = 0xffff; hold_ = false;
    bass_.NoteOff(); lead_.NoteOff(); lead2_.NoteOff(); PadRelease(); SirenOff(); tube_ = false;
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

  /* a noise hit inside the song (the anvil's strike, the chain, the roll's crack); crackle makes it velvet */
  void MusicBurst(NoiseColor col, float hz, float q, float decay_s, float gain, bool crackle) {
    Burst* v = nullptr;
    for (auto& b : mx_) if (!b.live) { v = &b; break; }
    if (!v) { v = &mx_[0]; for (auto& b : mx_) if (b.age > v->age) v = &b; }
    v->n.Init(sr_, crackle ? NoiseColor::kVelvet : col, &rng_mx_);
    v->f.SetMode(SvfMode::kBp); v->f.Reset(); v->f.Set(hz, q);
    v->e.SetTime(decay_s); v->e.Trigger(1.0f);
    v->gain = gain * (crackle ? 3.0f : 1.0f); v->age = 0; v->t = 0; v->live = 1;
  }
  void MusicRun(Burst& v, float* L, float* Rr, int n) {
    v.age += n;
    for (int i = 0; i < n; ++i) { float y = v.f.Process(v.n.Process()) * v.e.Process() * v.gain; L[i] += y; Rr[i] += y; }
    if (v.e.Level() < 1e-4f) v.live = 0;
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
    if (rolling && s >= 12 && mode_ == 3) MusicBurst(NoiseColor::kWhite, 3000.0f, 0.8f, 0.06f, 0.6f, false);   /* the roll's last four hits crack (5.2.7) */
    tube_ = mode_ == 1 && section_ == 1;    /* the fluorescent tube over the garage's B half (5.2.6) */
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
      /* a dropped pipe, not a glockenspiel: high clinks off the palette, the anvil low with a hammer strike (5.2.2) */
      static const float kClink[4] = {1760.0f, 2349.0f, 2960.0f, 3520.0f};
      if (dp.m[s] > 0) metal_.NoteOn(note_id_++ & 0xfff, kClink[static_cast<int>(rng_metal_.Next() * 4.0f) & 3], dp.m[s] + jit, frame_);
      if (dp.a[s] > 0) { anvil_.NoteOn((bar_ & 1) ? 294.0f : 220.0f, dp.a[s]); MusicBurst(NoiseColor::kWhite, 4000.0f, 1.0f, 0.024f, 0.5f * dp.a[s], false); }
      /* the chain on the and of one and three of the STOMP */
      if (sec->def->drums == P_STOMP && (s == 3 || s == 11)) MusicBurst(NoiseColor::kWhite, 3000.0f, 0.8f, 0.12f, 0.4f, true);
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
    if (lead_gate_ > 0 && --lead_gate_ == 0) { lead_.NoteOff(); lead2_.NoteOff(); }
    if (lay & L_LEAD) {
      const int local = (bar_ & 1) * 16 + s;
      const int tr = RIFF_T[(bar_ / 2) & 3];
      for (int k = 0; k < kRiffN; k++) {
        if (RIFF[k].step != local) continue;
        lead_p_.cutoff = 500.0f * powf(2.0f, 0.42f * bar_);
        lead_.SetParams(lead_p_); lead2_.SetParams(lead_p_);
        lead_.NoteOn(Mtof(static_cast<float>(64 + tr + RIFF[k].semis)), RIFF[k].dur >= 4 ? 1.0f : 0.8f);
        lead2_.NoteOn(Mtof(static_cast<float>(52 + tr + RIFF[k].semis)), RIFF[k].dur >= 4 ? 0.5f : 0.4f);   /* the octave down */
        lead_gate_ = RIFF[k].dur;
      }
    } else if (s == 0) { lead_.NoteOff(); lead2_.NoteOff(); }

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
    const bool mx = mx_[0].live || mx_[1].live || mx_[2].live || mx_[3].live;
    if (kick_.Active() || snare_.Active() || hat_.Active() || ohat_.Active() || Live(metal_) || anvil_.Active() || Live(stab_) || mx) {
      for (int i = 0; i < n; ++i) { bl_[i] = 0.0f; br_[i] = 0.0f; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      kick_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] += 1.25f * dl_[i]; br_[i] += 1.25f * dr_[i]; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      snare_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] += 0.6f * dl_[i]; br_[i] += 0.6f * dr_[i]; yl_[i] += 0.2f * gd * dl_[i]; yr_[i] += 0.2f * gd * dr_[i]; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      hat_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] += 3.6f * dl_[i] * 0.85f; br_[i] += 3.6f * dr_[i]; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      ohat_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { bl_[i] += 2.0f * dl_[i]; br_[i] += 2.0f * dr_[i] * 0.88f; }
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
      /* the music's own noise: the anvil strike, the chain, the roll crack */
      for (auto& v : mx_) if (v.live) MusicRun(v, bl_, br_, n);
      drum_sat_.Process(bl_, br_, 0, n);
      for (int i = 0; i < n; ++i) { l[from + i] += gd * bl_[i]; r[from + i] += gd * br_[i]; }
    }
    if (bass_.Active()) {
      for (int i = 0; i < n; ++i) { bl_[i] = 0.0f; br_[i] = 0.0f; }
      bass_.Process(bl_, br_, 0, n);
      bass_sat_.Process(bl_, br_, 0, n);
      /* the guitar the brief implies: a bandpassed copy of the fuzz at 1.4 kHz (5.2.3) */
      for (int i = 0; i < n; ++i) { float g2 = 0.6f * bass_bp_.Process(bl_[i]); bl_[i] += g2; br_[i] += g2; }
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
      siren_sat_.Process(dl_, dr_, 0, n);
      for (int i = 0; i < n; ++i) { l[from + i] += 0.12f * gb * dl_[i] * 0.8f; r[from + i] += 0.12f * gb * dr_[i]; yl_[i] += 0.1f * gb * dl_[i]; yr_[i] += 0.1f * gb * dr_[i]; }
      for (int i = 0; i < n; ++i) { dl_[i] = 0.0f; dr_[i] = 0.0f; }
      siren_[1].Process(dl_, dr_, 0, n);
      siren_sat_.Process(dl_, dr_, 0, n);
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
    if (tube_) {
      const float tg = 0.1f * gb * 4.0f;
      for (int i = 0; i < n; ++i) { float x = rng_mx_.Next() < 0.003f ? rng_mx_.Bipolar() : 0.0f; float y = tube_hp_.Process(x) * tg; l[from + i] += y; r[from + i] += y; }
    }
    if (lead_.Active() || lead2_.Active()) {
      for (int i = 0; i < n; ++i) { bl_[i] = 0.0f; br_[i] = 0.0f; }
      lead_.Process(bl_, br_, 0, n);
      lead2_.Process(bl_, br_, 0, n);
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

#if RTR_SONGS
  void SongsInit(float sr) {
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
    clink.material = 0.0f; clink.decay = 0.5f; clink.strike_hardness = 0.95f; clink.brightness = 0.9f; clink.level = 0.5f;   /* metal that is metal (5.2.2) */
    for (int i = 0; i < 4; ++i) metal_.at(i).Init(sr, &rng_metal_, clink);
    bellows::Modal::Params anvil;
    anvil.material = 0.0f; anvil.decay = 1.1f; anvil.strike_hardness = 0.9f; anvil.brightness = 0.6f; anvil.level = 0.6f;
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
    bellows::Chorus<>::Params ch; ch.rate = 0.9f; ch.depth = 0.4f; ch.mix = 0.12f;
    chorus_.Init(sr, ch);
    bellows::Saturator<2>::Params bs; bs.drive = 9.0f; bs.tone = 0.6f;   /* the fuzz harmonics heard (5.2.3) */
    bass_sat_.Init(sr, bs);
    bellows::Saturator<1>::Params ss; ss.drive = 3.0f; ss.tone = 0.1f;
    stab_sat_.Init(sr, ss);
    bellows::Saturator<1>::Params ls; ls.drive = 6.0f; ls.tone = 0.3f;    /* a lead that is a second riff (5.2.4) */
    lead_sat_.Init(sr, ls);
    bellows::Saturator<1>::Params ds; ds.drive = 2.6f; ds.tone = 0.25f;   /* harmonics over 1 kHz, not a flattened kick (5.2.1) */
    drum_sat_.Init(sr, ds);
    bellows::Saturator<1>::Params sn; sn.drive = 3.0f; sn.tone = 0.2f;   /* the overtime sirens bite (5.2.7) */
    siren_sat_.Init(sr, sn);
    bass_bp_.Init(sr); bass_bp_.SetMode(SvfMode::kBp); bass_bp_.Set(1400.0f, 1.5f);
    lead2_.Init(sr, &rng_lead_);
    rng_mx_.Init("rtr::music::noise");
    for (auto& v : mx_) { v = Burst(); v.f.Init(sr); v.e.Init(sr); v.n.Init(sr, NoiseColor::kWhite, &rng_mx_); }
    tube_hp_.Init(sr); tube_hp_.SetMode(SvfMode::kHp); tube_hp_.Set(2500.0f, 0.7f);
    /* the master tilt on every stem (5.2.5): -2 dB at 100 Hz, +3 at 2.5 kHz, +3 over 6 kHz */
    bellows::Eq3::Params eq; eq.low_freq = 100.0f; eq.low_gain = -2.0f; eq.mid_freq = 2500.0f; eq.mid_gain = 3.0f; eq.mid_q = 0.8f; eq.high_freq = 6000.0f; eq.high_gain = 3.0f;
    tilt_.Init(sr, eq);
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
  }
#endif

  /* SFX voices */
  bellows::Rng rng_clang_, rng_spark_, rng_buzz_, rng_bells_, rng_crowd_, rng_whine_, rng_kit_, rng_noise_, rng_loop_, rng_dmg_, rng_shout_;
  bellows::Va buzz_;
  bellows::Saturator<1> buzz_sat_;
  bellows::VoicePool<bellows::Fm, 4> bells_;
  bellows::Fm gong_;
  Burst bursts_[16];
  Crackle crk_[6];
  Ring rings_[10];
  Thud thuds_[4];
  Sweep sweeps_[4];
  WLoop wpn_[kSlots];
  DLoop drv_[kSlots];
  DmgLoop dmg_[kSlots];
  HLoop haz_[kHazLoops];
  bellows::NoiseGen drive_noise_, haz_noise_;
  bellows::NoiseGen crowd_;
  bellows::Svf crowd_bp1_, crowd_bp2_, crowd_rum_;
  float crowd_lfo_ = 0.0f, crowd_gain_ = 0.0f, crowd_swell_ = 0.0f, crowd_bed_ = 0.0f, crowd_brown_ = 0.0f;
  float swell_t_ = 0.0f, swell_v_ = 0.0f, swell_rate_ = 0.06f, swell_rel_ = 1.0f, clap_g_ = 0.2f;
  int swell_hold_ = 0, hush_ = 0, stomp_n_ = 0, stomp_next_ = 0, clap_left_ = 0;
  bool crowd_ot_ = false;
  Shout shout_[3];
  bellows::Hat tick_;
  bellows::Snare clap_;
  bellows::StereoDelay<300> slap_;
  bellows::Saturator<1> punch_;
  bellows::Compressor<> comp_;
  bellows::Limiter<BELLOWS_SAMPLE_RATE, true> limiter_;   /* true peak: a fight is all transients, and their intersample overs are real */
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
  bellows::Saturator<1> stab_sat_, lead_sat_, drum_sat_, siren_sat_;
  bellows::Va lead2_;
  bellows::Svf bass_bp_, tube_hp_;
  bellows::Rng rng_mx_;
  Burst mx_[4];
  bool tube_ = false;
  bellows::Eq3 tilt_;
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
  float yl_[kBlock] = {}, yr_[kBlock] = {}, bl_[kBlock] = {}, br_[kBlock] = {}, ml_[kBlock] = {}, mr_[kBlock] = {};
#endif

  float dl_[kBlock] = {}, dr_[kBlock] = {}, sl_[kBlock] = {}, sr_buf_[kBlock] = {};
  float il_[kBlock] = {}, ir_[kBlock] = {}, fl_[kBlock] = {}, fr_[kBlock] = {}, cl_[kBlock] = {}, cr_[kBlock] = {};
  float tl_[kBlock] = {}, tr_[kBlock] = {}, pink_[kBlock] = {}, white_[kBlock] = {}, cn_[kBlock] = {};
  Ev ev_[kMaxEv];
  int nev_ = 0;
  int buzz_gen_ = 0;
  bool buzz_live_ = false;
  float buzz_pan_ = 0.0f;
  int note_id_ = 0;
  uint32_t frame_ = 0;
  float sr_ = 48000.0f;
  float sfx_gain_ = 0.8f, music_gain_ = 0.6f, duck_ = 1.0f;
  float slow_ = 1.0f, slow_t_ = 1.0f, base_ms_ = 0.0f, base_gain_ = 1.0f;
  float dk_ = 0.0f, dk_depth_ = 0.0f, dk_rel_ = 0.18f; int dk_stage_ = 0, dk_hold_ = 0;
  int fall_recent_ = 0;
  bool whine_mode_ = false;
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
  if (sfx < 0 || sfx >= SFX_COUNT2_) return;
  if (band.Ready()) band.Sfx(sfx, var);
  else if (npending < 16) { pending_sfx[npending][0] = sfx; pending_sfx[npending][1] = var; npending++; }
}

extern "C" void audio_crowd(float level) { if (band.Ready()) band.Crowd(level); }
extern "C" void audio_part(int row, int option) { if (band.Ready()) band.Part(row, option); }

/* the fight voices; the cart reaches these through audio_pcm.c, the bake directly */
static float PanClamp(float p) { return p < -1.0f ? -1.0f : p > 1.0f ? 1.0f : p; }
extern "C" void audio_band_hit(int kind, int b, float vel, float pan, int armour, int attacker_slot) { if (band.Ready()) band.Hit(kind, b, vel, PanClamp(pan), armour, attacker_slot); }
extern "C" void audio_band_play_at(int sfx, int var, float pan) {
  if (!band.Ready() || sfx < 0 || sfx >= SFX_COUNT2_) return;
  if (sfx < SFX_COUNT_) band.Sfx(sfx, var); else band.Fx(sfx, var, PanClamp(pan));
}
extern "C" void audio_band_tell(int slot, int weapon, int tell_zone, float pan) { if (band.Ready()) band.Tell(slot, weapon, tell_zone, PanClamp(pan)); }
extern "C" void audio_band_spin_change(int slot, int zone, float spin_before, float spin_lost) { if (band.Ready()) band.SpinChange(slot, zone, spin_before, spin_lost); }
extern "C" void audio_band_haz_tell(int type, float pan, float tell_s) { if (band.Ready()) band.HazTell(type, PanClamp(pan), tell_s); }
extern "C" void audio_band_crowd_react(int kind) { if (band.Ready()) band.CrowdReact(kind); }
extern "C" void audio_band_duck_band(int b) { if (band.Ready()) band.DuckBand(b); }
extern "C" void audio_band_weapon(int slot, int kind, float level, float pitch, float load) { if (band.Ready()) band.Weapon(slot, kind, level, pitch, load); }
extern "C" void audio_band_drive(int slot, int kind, float speed, float turn, int grounded, int inverted, int flags) { if (band.Ready()) band.Drive(slot, kind, speed, turn, grounded, inverted, flags); }
extern "C" void audio_band_damage(int slot, float hp_frac, int burning, int alive) { if (band.Ready()) band.Damage(slot, hp_frac, burning, alive); }
extern "C" void audio_band_hazard(int slot, int type, float level, float pan, float aux_pan) { if (band.Ready()) band.Hazard(slot, type, level, PanClamp(pan), PanClamp(aux_pan)); }
extern "C" void audio_band_slot_pan(int slot, float pan) { if (band.Ready()) band.SlotPan(slot, PanClamp(pan)); }
extern "C" void audio_band_loops(int on) { if (band.Ready() && !on) band.LoopsOff(); }
extern "C" void audio_band_slowmo(int on) { if (band.Ready()) band.SetSlow(on != 0); }
extern "C" void audio_band_overtime(int on) { if (band.Ready()) band.SetOvertime(on != 0); }
extern "C" float audio_band_music_duck(void) { return band.Ready() ? band.MusicDuck() : 1.0f; }
extern "C" void audio_band_whine(int slot, float level, float pitch) { if (band.Ready()) band.Whine(slot, level, pitch); }

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

