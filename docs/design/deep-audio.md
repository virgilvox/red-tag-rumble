# RED TAG RUMBLE: battle sound, investigation and design spec

Lens: battle sound. Written against main at 22c05b6 in the detached snapshot
`/Users/obsidian/Projects/heatsync/games/_wt/rtr-deep-audio` (nothing committed there).
Every number below was measured on a native render in that snapshot:
`build/sfx_preview.wav`, `build/preview-{garage,arena,overtime}.wav` and the six stems
(`./build.sh music` equivalent: `clang++ ... -DMUSIC_BAKE tools/bake_music.cpp src/audio_band.cpp`),
analysed with the node script in this folder (`wavstat.mjs`: peak, 300 ms RMS, attack,
decay to -20 and -40 dB, spectral centroid, band energy shares, spectral flatness, crest)
and ffmpeg spectrograms (`sfx-0-20.png`, `preview-arena.png`). A measured prototype of the
proposed recipes is `proto.cpp` in this folder (rendered to `proto.png`).

Files this spec touches: `src/audio.h`, `src/audio_band.cpp`, `src/audio_pcm.c`,
`tools/bake_music.cpp`, `src/main.c` (drain_events), `src/game_flow.c` (fight_audio),
`src/sim.h` (event_t fields, two new event types, owned by the sim agent) and `src/juice.h`
(HIT_BANDS sfx column).

Summary verdict: the band is a well engineered menu and jingle instrument wearing a
combat label. Every battle sound is a tonal synth note (spectral flatness 0.000 to 0.005
on every one of the twelve patches); there is no noise layer anywhere in the hit path, so
nothing crunches, scrapes, hisses, grinds or explodes. The weapons do not have their own
voices (a saw is six xylophone dings a second, a flame is a ding every half second, a
hammer swing and a flipper fire are silent until they connect, a wedge is silent
always), half the sim's events reach no sound at all, and the fight music is a sub bass
mumble (70 percent of its energy under 120 Hz, 1 to 4 percent over 2 kHz, bed at -27 dBFS
RMS at the default rows). The fix is not retuning: it is a second generation of the band
with noise voices, per weapon continuous loops, velocity, a priority mixer and a measured
bake, plus three small sim fields so main.c knows what hit what and how fast.

## 1. Current state

### 1.1 Architecture (unchanged by this spec)

- `audio.h` is the whole contract: 12 `SFX_*` ids, 4 variants, `audio_play[_var]`,
  `audio_music`, `audio_whine(slot, level, pitch)`, `audio_crowd`, `audio_slowmo`,
  `audio_tension`, `audio_duck`, `audio_duck_hit`, `audio_part`, `audio_render`.
- `audio_pcm.c` mixes the six baked 22050 Hz mono stems through `wc_pcm_mixer`
  (`MODE_TRIM` garage 0.5, arena 0.4, overtime 0.45; a 4 dB, 150 ms duck on
  `audio_duck_hit`, `HIT_DUCK_GAIN 0.63`), then calls `audio_band_render`.
- `audio_band.cpp` `class Band` (lines 244 to 1092): the SFX voices, `Push`/`Fire` event
  sequencer (`Ev ev_[128]`, ms delays), `SfxSpan` mixes buses with fixed gains, a slap
  delay (`StereoDelay<300>`, 93 and 71 ms), `Saturator<1> punch_` on the impact bus,
  master `Compressor` (-10 dB, 2:1) and `Limiter` (-0.8 dB). The songs compile only with
  `MUSIC_BAKE` or `MUSIC_LIVE`.
- Call sites: `main.c drain_events` (lines 295 to 413) maps events to `HIT_BANDS[band].sfx`
  (`juice.h`) and the per event switch; `game_flow.c fight_audio` (1147 to 1164) drives the
  four whines and the tension flag once per render frame; the menus call `audio_play` and
  `audio_part` directly.
- Band cost measured in the bake: 0.448 ms per 800 frame render under a dense load on
  this Mac (`band cost` line of `bake_music`). The spec's Pi budget is under 1 ms a frame.

### 1.2 Every patch, how it is synthesised, when it fires, what it measures

Measured at SFX 10 MUSIC 10 in `build/sfx_preview.wav` (1 s a slot). Columns: peak dBFS,
RMS of the first 300 ms, attack to peak ms, decay from peak to -20 dB ms, spectral centroid
of the first 100 ms Hz, energy share percent in <120 / 120 to 500 / 500 to 2k / 2k to 6k /
>6k Hz, spectral flatness of the first 100 ms (0 is a pure tone, 1 is white noise).

| id, variant | recipe (`Band::Sfx`, audio_band.cpp 424 to 479) | fires from | peak | rms | atk | T20 | cen | bands | flat |
|---|---|---|---|---|---|---|---|---|---|
| MENU_MOVE | Hat 900 Hz 0.25 + FM bell E6 0.3 | every cursor move | -10.5 | -31.8 | 0 | 206 | 1667 | 0/0/78/22/0 | 0.000 |
| MENU_OK | bells E5, B5 (low: B4, E5) | confirm, pickup, taunt | -13.5 | -25.2 | 49 | 354 | 2363 | 0/0/55/43/2 | 0.001 |
| MENU_BACK | bells B4, E4 | back, OPS leave, NOT VACUUM | -15.1 | -27.1 | 2 | 224 | 867 | 1/31/67/1/0 | 0.000 |
| LOCK | impact kick E1 0.6 + bells E5, B5 | garage lock, vote lock | -6.7 | -19.8 | 5 | 286 | 892 | 63/5/15/16/1 | 0.000 |
| COUNT | bell E5 0.8 (short: + hat) | 3 2 1, T minus 10, count-out tick | -11.1 | -25.0 | 1 | 260 | 1470 | 0/0/84/16/0 | 0.000 |
| HORN | square buzz B2 then E3, 1.5 beats (low: E2 one beat) | FIGHT, OPS wake, respawn | -5.1 | -11.8 | 4 | 354 | 317 | 1/86/11/2/0 | 0.000 |
| HIT_LIGHT | spark (Va square, SVF 6 kHz) E4 + Modal bar E5 ring | tap and hit bands, saw ticks, flame ticks, panel shed | -3.4 | -18.8 | 2 | 192 | 1274 | 0/0/96/3/1 | 0.000 |
| HIT_LIGHT low | spark alone | landing, wall, light hazard | -9.1 | -24.3 | 1 | 42 | 1427 | 0/66/18/10/6 | 0.001 |
| HIT_HEAVY | impact kick E1 + Modal E3 + spark + Modal E4 | heavy and huge bands, heavy hazard | -2.0 | -15.2 | 2 | 144 | 390 | 28/53/19/0/0 | 0.000 |
| CLASH | kick + two Modals (E2, E3) + three sparks | spinner clash (low 0.6: spinner wall) | -1.7 | -13.7 | 1 | 168 | 573 | 20/46/32/2/1 | 0.002 |
| LAUNCH | three rising sparks E3 B3 E4 + bell | flipper connect, ramp, floor flipper | -8.8 | -22.9 | 49 | 114 | 613 | 2/81/12/4/1 | 0.003 |
| KLAXON | buzz Bb2 150 ms (tell x2, short 80 ms) | inverted, hazard tell, RED TAG, deck | -6.6 | -18.9 | 4 | 28 | 315 | 49/39/10/2/0 | 0.000 |
| KLAXON low | hat 1200 Hz 0.3 | the inverted count tick | -9.0 | -40.5 | 0 | 20 | 9501 | 0/0/0/28/71 | 0.000 |
| KO | kick E1 + FM gong E2 (2.5 s release) + Modal + spark | KO, mini KO | -3.0 | -12.4 | 97 | 206 | 146 | 62/32/6/0/0 | 0.000 |
| whines x4 | Va saw, SVF LP 1800, 80 + pitch x 600 Hz, 0.1 gain, no modulation | `fight_audio` level spin x 0.8 | -14.6 | -26.9 | | | 176 | 80/16/4/0/0 | 0.000 |
| crowd | pink noise, two SVF bandpasses 800 and 2200 Hz, 0.3 Hz tremolo | bed 0.12 in a fight, swells on huge, clash, KO | bed -30, swell -15 | -42 / -27 | | | 1300 | 1/14/64/20/1 | 0.008 |
| parts (36) | clangs, sparks, buzzes, bells per row | garage row cycle, RT test | -4 to -18 | | | | | | 0.000 to 0.005 |

Events with no sound at all today (`drain_events` default case): EV_SHED (a panel falls
off), EV_BURST (the KO debris), EV_SELFRIGHT, EV_WEAPON_TELL (hammer wind up, hammer
miss, flipper fire), EV_BOOST, EV_STUCK, EV_BENCH_BREAK, EV_BUMPER_BREAK, EV_HILL_SCORE.
`fx_event` draws sparks for EV_SHED and chips for the two breaks, so the picture moves
while the sound does not.

### 1.3 How the fight actually sounds (the old rig capture)

`/Users/obsidian/Projects/heatsync/games/red-tag-rumble/build/out-feel.wav` (a 2p feel
capture, 15 s): nine of fifteen seconds have over 90 percent of their energy under
120 Hz (slots 3, 4, 6, 8: 95 to 96 percent, centroid 61 to 80 Hz, peaks at 41 and 82 Hz,
the E1 impact kick and the bass). One hit second (slot 11) peaks at -0.8 dBFS with
76 percent of its energy at 120 to 500 Hz. Nothing in the capture has more than 18
percent of its energy over 2 kHz. A fight is a sub drone with occasional xylophone notes.

## 2. Problems, with evidence

P1. No noise anywhere in the hit path. Flatness 0.000 on HIT_LIGHT, HIT_HEAVY, CLASH,
KO (table 1.2); the spectrogram `sfx-0-20.png` shows every hit as horizontal harmonic
lines and no vertical broadband bar. The "spark" voice is `VoicePool<Va, 4> spark_`
(audio_band.cpp 265 to 270): a detuned square through a lowpass, i.e. a synth pluck; its
first 100 ms has 96 percent of its energy in 500 Hz to 2 kHz at one pitch. A metal impact
is a 3 to 10 ms broadband crack, a 40 to 120 ms body, and a ring; the band plays the ring.

P2. HIT_LIGHT reads as a chime, not a hit. Modal bar at Mtof(76 + palette) = E5 and up,
T20 192 ms, 96 percent in 500 Hz to 2 kHz, attack 2 ms with no transient over 2 kHz.
Every tap (1 to 5 damage) and hit (6 to 10 damage) plays this, and taps are most hits in
a fight (saw ticks 4, flame ticks 2, rams 9). The owner's "battle sound effects aren't
very good" is this sound, dozens of times a fight.

P3. HIT_HEAVY and CLASH are the same dull thud. HEAVY: 28 / 53 / 19 / 0 / 0, centroid
390 Hz; CLASH: 20 / 46 / 32 / 2 / 1. No energy over 2 kHz, so nothing cuts through the
music, which is why R5 had to duck the stems and raise the bus to 0.85 (HANDOFF risk 3).
The impact `Kick` at E1 (41 Hz) with `click_tune 9` is a music kick; it adds the 41 and
82 Hz lines seen in every hit of the capture.

P4. The weapons have no voices. `event_t` carries no weapon id (sim.h 100: type, a, b,
zone, x, z, y, dmg, nx, nz), and `drain_events` maps on damage only, so: a SAW tick
(every 10 frames, sim.c 735) is HIT_LIGHT six times a second; a FLAME tick (every 30
frames, 838) is HIT_LIGHT twice a second with no roar; a HAMMER wind up (EV_WEAPON_TELL
zone 0, 688) and a HAMMER miss (zone 1, 683) are silent; a FLIPPER press (672) is silent
until it connects; a SPIKE stab is a chime; a CRUSHER bite, hold and release are silent;
a LIFTER engage and carry are silent; a WEDGE ram is HIT_LIGHT or HIT_HEAVY with no scrape
and the wedge hop (ram_check 965 to 969, `v->b.vy = 1.5`) emits nothing. The owner's
"it's not clear how to attack" has a sound half: pressing the weapon button makes no
sound for five of ten weapons.

P5. The spinner whine is a steady saw. `Whine()` (402 to 411) sets one frequency; the
level ramps by 0.18 a span; there is no wobble, no load dip on contact, no spin down
sweep and no sound for the spin killing hit (EV_SPIN_WALL and EV_CLASH drop spin to 0,
SPIN_BLEED drops it 0.6 to 0.7 on a landed hit, sim.c 760 to 764; the whine just gets
quieter at 0.18 a span). Measured whine: 80 percent of its energy under 120 Hz at low
pitch (cutoff 1800 Hz on a saw at 80 to 260 Hz), the sound that filled the capture's
floor to -7 dBFS (HANDOFF risk 3). The spinner's one tell, the thing a player must read
to time an approach, is a hum.

P6. The drive is silent. Nothing in the band reads `b->drive_t`, `|v|` or the drive
type. TREADS ("LOUD. SLOW." per its own flavour text) sounds the same as HOVER: nothing.

P7. Damage state is silent. `fx_bot_damage_state` (fx.c 232) starts smoke at 50 HP and
fire at 30, the HUD bar goes amber and rust, the sound stays the same until the KO.

P8. KO is a gong. 62 percent under 120 Hz, attack 97 ms (the FM gong swells in),
T40 902 ms, flatness 0.000: a temple bell. No blast, no debris rain, no crowd roar beyond
a 10 dB louder pink noise (crowd swell -27 dBFS RMS, same spectrum as the bed).

P9. Hazards share two generic hits. `EV_HAZ_HIT` picks HIT_HEAVY at 15 damage and over,
HIT_LIGHT low under (main.c 370 to 372): a pulverizer, a killsaw, a flame grate, a screw
conveyor, a laser and a pit all sound like the bot hits. Tells are the same klaxon.
Active hazards (killsaw spinning, flame grate lit, screw turning, laser sweeping) have no
loop; `M.haz[i].active` is never read by audio.

P10. Crowd is a filtered noise fader. Bed at -42 dBFS RMS is inaudible under the music at
MUSIC 6; the swell is the same noise 15 dB louder. No "oooh", no voices, no stomp, so the
big hits get no audience.

P11. Mix: everything is centred, everything is tonal and in E, every hit has the slap
delay. Pitches are drawn from the song palette (`Pal`, 227), so a fight is a sequence of
E minor chords: musical, and exactly wrong for a scrap yard. The slap (93 / 71 ms,
feedback 0.22) smears every transient.

P12. Levels and the duck. At the default rows a HIT_LIGHT peaks -4.6 dBFS and the arena
bed peaks -12.6 (bake "music under the hits"): the hits are loud enough; they are the
wrong shape. The hit duck is 4 dB for 150 ms for every band (audio_pcm.c 56, 57) and does
not touch the whines or the crowd, so a KO ducks the music exactly as much as a tap.

P13. One SFX per band per frame (`sfx_band_done`, main.c 309) stops a clash from
doubling but not a saw from chiming at 6 Hz, nor two taps in consecutive frames.

P14. The inverted count tick (KLAXON low) is -40.5 dBFS RMS: a hat alone at 0.3. It is
inaudible in a fight.

P15. Music (section 5): the arena bed is a sub mumble. Per second analysis of
`preview-arena.wav`: 50 to 57 percent of energy under 120 Hz in the verse seconds, 1 to
4 percent over 2 kHz, bed RMS -22 to -29 dBFS, centroid 250 to 780 Hz. The bake's own
balance line says arena_base 83 / 17 / 0, stomp 56 / 32 / 11, break 49 / 35 / 16.
HANDOFF risk 3 already names the missing top end; this confirms it with numbers.

## 3. The design

### 3.1 Principles

1. Noise first. Every impact is three layers: a transient (white noise, 3 to 12 ms, 2 to
   8 kHz), a body (pink or brown noise, 40 to 120 ms, 80 to 900 Hz, plus a pitched thud
   for the heavy bands) and a tail (a Modal ring and a debris crackle). The ring is
   seasoning, not the meal.
2. Every weapon has a voice: a continuous loop while it is live (spinner, saw, flame,
   drive) or a tell plus a strike (hammer, flipper, spike, crusher, lifter). Pressing the
   weapon button always makes a sound within one frame.
3. Velocity. Every one shot takes a 0 to 1 velocity from the sim (closing speed, damage,
   drop height) that sets level, transient brightness and body length together, so a
   graze and a wreck are the same patch at different velocities, not two patches.
4. Variation without a sampler: pitch, decay and filter draws per fire from the
   `rtr::clang` and `rtr::spark` rngs, plus or minus 15 percent, never a palette note.
   Pitches are no longer E minor chord tones. The stingers (menus, lock, count, horn, KO
   card) keep the palette; the fight does not.
5. Priority mixer: every voice has a priority and a pool; a new fire steals the quietest
   voice of lower priority, never a KO or a clash.
6. Pan by world x: `pan = clamp(x / (arena.w * 0.5), -1, 1) * 0.6`, applied in `Mix`.
   Split screen means the world is the only honest reference; it also separates two
   simultaneous fights in a 4 pad match.
7. Measured, not assumed: every patch has a target peak, RMS, flatness and centroid in
   the bake's table (section 6) and the bake fails on a miss.
8. Headless rule unchanged: `sim.c` emits events and state; it never calls audio. All
   audio decisions live in main.c, game_flow.c and the band.

### 3.2 New Bellows voices (audio_band.cpp)

All are cheap compositions of `bellows::NoiseGen`, `bellows::Svf`, `bellows::ExpDecay`
and `bellows::Rng` (dsp/noise.h, dsp/filters.h, dsp/envelopes.h, core/prng.h). Per sample
cost of a Burst is one noise draw, one SVF and two multiplies; the current `Va` spark
voice runs two BLEP oscillators, a filter and two ADSRs, about ten times the work.
Prototyped and measured in `proto.cpp`.

```
struct Burst   { NoiseGen n; Svf f; ExpDecay e; float gain; }          // Fire(vel): a shaped noise burst
   Init(rng, colour, SvfMode, hz, q, decay_s); P() = f(n()) * e() * gain; Active() = e.Level() > 1e-4
struct Crackle { NoiseGen n; Svf hp; ExpDecay e; Rng* r; float density; } // velvet impulses: sparks, debris, electrical fizz
   P(): x = r->Next() < density ? r->Bipolar() : 0; return hp(x) * e() * 4
struct Loop    { NoiseGen n; Svf f; float g, g_t; float lfo_ph; }       // a continuous noise bed with a level ramp and one LFO
struct Thud    = bellows::Kick, decay 0.14 to 0.30, drive 2.5 to 3.5, click_tune 4 to 6, pitch_decay 0.03
struct Ring    = bellows::Modal (bar or bell), decay 0.25 to 0.6, strike_hardness 0.8 to 0.95
```

Pools (template `VoicePool<V, N>` cannot hold a struct without the engine interface, so
the pools are plain arrays with the steal rule of 3.1.5):

| pool | type | count | used by | priority |
|---|---|---|---|---|
| `xt_[6]` | Burst white BP 2.5 to 8 kHz, 4 to 14 ms | 6 | every impact transient, hazard hits, breaks | by band |
| `body_[4]` | Burst pink LP 300 to 1200 Hz, 40 to 140 ms | 4 | crunch and wreck bodies, landings, wall | by band |
| `thud_[3]` | Kick | 3 | wreck, KO, hammer, pulverizer, landing over 4 u/s | by band |
| `ring_[4]` | Modal bar (existing `clang_`) | 4 | every impact tail, shed panel, bumper | low |
| `bell_[2]` | Modal bell material 2 | 2 | wreck, KO, crusher bite, chassis SHELL | by band |
| `crk_[4]` | Crackle HP 2.5 to 5 kHz | 4 | sparks (hit tails), debris rain, fizz | low |
| `deb_[2]` | Burst pink BP 1.8 kHz q 1.5, 500 ms | 2 | debris tail on wreck, KO, burst | low |
| `hiss_[2]` | Burst white BP 6 kHz, 150 to 300 ms | 2 | pneumatics (flipper, hammer tell, crusher), flame ignite | mid |
| `wpn_[4]` | weapon loop per seat slot: Va saw (existing `whine_`) plus a Loop bed | 4 | spinner, saw, flame, melty, lifter servo | always on |
| `drv_[4]` | drive loop per seat slot: Loop (one shared NoiseGen, four Svf) plus a Va triangle for wheel whine | 4 | tread, wheels, hover, walker, legtrack | always on |
| `dmg_[4]` | Crackle per seat slot | 4 | sparking fizz under 40 percent, death rattle | always on |
| `haz_[2]` | hazard loop: Loop | 2 | the two loudest active hazards near any bot | mid |
| `buzz_`, `bells_[4]`, `gong_`, `tick_` | existing | | stingers, menus | high (menus) |
| `crowd_` | existing bed plus `crowd_v_[3]` Formant voices (engines/formant.h, vowel 0 "a", pitch 110 to 180 Hz, breath 0.3) | 3 | oooh, roar | mid |

The impact bus (transient, body, thud, bell, ring) runs through `punch_` (Saturator drive
1.7 to 2.2) as today; the slap delay feeds only the stingers and the crowd (send 0 from
every impact voice: the delay smears transients, P11). Noise voices skip the delay.

### 3.3 Impacts: the three bands, with a velocity

`audio_hit(kind, band, vel, x, z)` replaces `audio_play(HIT_BANDS[band].sfx)` in
drain_events. `kind` is `HK_*` (section 4.1), `band` is BAND_TAP to BAND_KO, `vel` is 0 to 1
(section 4.2 computes it), x z pan.

Common velocity law: level = 0.55 + 0.45 vel; transient decay = 4 ms + 8 ms vel;
transient centre = 6 kHz - 3 kHz vel (a hard hit is darker and longer, a graze is a
click); body decay = 40 ms + 100 ms vel; ring velocity = 0.3 + 0.6 vel; a 15 percent
random spread on every frequency and decay from `rng_clang_`.

CLANK (BAND_TAP and BAND_HIT): `xt_` white BP 3.5 kHz q 0.6, 12 ms at vel; `ring_` bar at
a draw from {330, 440, 520, 660, 880} Hz times (1 + 0.15 r), decay 0.3, strike 0.95, vel
0.6; `crk_` sparks density 0.004, 250 ms, vel 0.5. No body, no thud. Measured prototype:
peak -11 dBFS at a 0.5 bus gain, centroid 1360 Hz, flatness 0.013, T20 110 ms. Target
after the bus trims: peak -8 dBFS at SFX 8 for BAND_HIT, -12 for BAND_TAP.

CRUNCH (BAND_HEAVY): CLANK plus `body_` pink LP 900 Hz q 0.8, 90 ms; `thud_` 70 Hz,
decay 0.18, drive 3.0; `bell_` at a draw from {110, 165, 196, 220} Hz, decay 0.5, vel
0.7; `crk_` 0.8. Prototype: 13 / 72 / 1 / 2 / 12 bands, flatness 0.081, T20 88 ms. Target
peak -4 dBFS.

WRECK (BAND_HUGE, BAND_CLASH): CRUNCH plus a second `ring_` an octave up, `thud_` at
50 Hz decay 0.25 drive 3.5, `deb_` debris tail 500 ms, `crk_` sparks density 0.008 for
300 ms, and a 1 frame pre delay on the ring (the transient alone for 16 ms, then the
metal: it reads as mass). Prototype: 46 / 38 / 5 / 2 / 10, flatness 0.078, T20 54 ms with
a 270 ms tail. Target peak -2 dBFS; this and KO are the only sounds allowed to touch the
limiter.

KIND colouring on top of the band (the weapon that hit, from `event_t.weapon`):

| HK kind | change to the band recipe |
|---|---|
| HK_SPINNER (HORIZ, DRUM, MELTY) | transient centre up 1 kHz, a second transient 8 ms later at 0.6 (the tooth skip), ring pitch from the high set, the attacker's whine loop gets a 120 ms load dip (section 3.4) |
| HK_SAW | no ring; the saw grind loop (3.4) gets a 150 ms bite burst: `xt_` 2 kHz q 2 at 0.7 |
| HK_HAMMER | thud always (even on BAND_HIT), body 140 ms, no sparks, bell at 110 Hz; the swing hiss has already played on the tell |
| HK_SPIKE | transient 8 kHz 4 ms (a stab), ring short (decay 0.15), a `hiss_` 80 ms at 0.4 (the retract) |
| HK_RAM (WEDGE, NONE chassis ram) | body dominant (pink LP 600 Hz, 120 ms), ring at 0.3 vel, no sparks; if `zone` says front plate, add the scrape loop start (3.4) |
| HK_FLIPPER | the hit itself is quiet (0.5 level): the launch and the landing carry the moment |
| HK_FLAME | no impact at all: the roar loop carries it; a tick plays `crk_` fizz 0.3 only |
| HK_CRUSHER | bite: `hiss_` 60 ms then bell 165 Hz decay 0.6 with a `body_` creak (pink BP 400 Hz q 4, 300 ms, LFO 6 Hz on the cutoff); the hold creak loops at 0.3 while `clamped_by` (3.4) |
| HK_LIFTER | servo whir (3.4) plus a soft CLANK at 0.4 vel on engage |
| HK_OPS | WRECK at vel 1 with the bell at 82 Hz (OPS is heavy) |
| HK_HAZARD_* | section 3.7 |

Armour colours the ring (victim `build.armour`, read in main.c): PLY swaps the bar ring
for `bell_` material 4 wood (decay 0.12, a thock); ALU ring +1 octave, decay 0.2; UHMW no
ring, body only; STEEL ring decay 0.45; TI ring decay 0.6 and +0.3 brightness; WEAR two
rings 7 cents apart (a plate buzz); NONE (bare frame) ring at 0.3 and a `crk_` rattle.

### 3.4 Weapon loops and tells

All loops are driven once per render frame from `game_flow.c fight_audio` through one call
per slot: `audio_weapon(slot, kind, level, pitch, load)` (replaces `audio_whine`). The
band ramps level over 2 spans and pitch over 1 span, so there are no zips. Slots 0 to 3
are the seat slots as today; OPS (slot 4) gets its own saw loop on `haz_[1]` while
`spin2 > 0.5`.

Spinner whine (WK_SPIN; HORIZ, DRUM, MELTY): `whine_` Va saw, detune 9 cents, sub 0.3,
SVF LP cutoff 600 + 1400 spin Hz, resonance 0.35, plus the `Loop` bed (white BP at 2 x
the fundamental, q 3, level 0.25) for the air. Pitch: HORIZ 90 to 820 Hz over spin 0 to 1
(the owner must hear the disc climb for the full 2.0 s spin up), DRUM 120 to 680 Hz,
MELTY 70 to 520 Hz with a 1.5 Hz beat. Wobble: a 6.5 Hz sine at 2.5 percent of pitch
while spin > 0.3 plus a 0.4 Hz drift of 1 percent (an unbalanced disc). Load: `load` is 0
to 1; a landed hit sets load 1 for 120 ms (pitch drops 12 percent, level drops 4 dB, the
transient is the hit); contact with a wall sets load 0.5 while pinned. Level: 0.15 +
0.55 spin at the seat's gain of 0.1 (today's 0.8 spin at 0.1; the whine must sit at
-18 dBFS RMS at full spin and SFX 8, not -7: P5). Spin down: when spin falls with the
button released, the loop follows pitch and adds a detent click (`ring_` 1200 Hz vel 0.12)
every 40 ms once spin < 0.35, so a spent disc ticks as it stops. Spin kill: when spin
drops by more than 0.3 in one frame (clash, wall, SPIN_BLEED), play the kill: `xt_`
transient at vel 1, a 300 ms pitch fall of the whine from its pitch to 60 Hz with level to
0 over 300 ms, `crk_` sparks 0.8, and a `hiss_` 200 ms at 0.4 (the motor dumping).
Prototype: `WHINE_wobble` and `WHINE_spindown` in proto.png (the arch with the wobble
teeth at 4.4 to 5.8 s).

Saw grind (WK_SAW): the saw spins up like a spinner (pitch 200 to 900 Hz, a thinner
voice: cutoff 2400, resonance 0.5, no sub). In contact (main.c keeps a 12 frame timer per
attacker that every HK_SAW tick resets) the grind loop plays: white BP 2.2 kHz q 2.5 with
a 41 Hz tremolo (the teeth) plus `Crackle` grit (density 0.05, 20 ms retrigger every
12 ms at a random 0.6 to 1.0). Level 0.5 in contact. Prototype `SAW_GRIND_1s`:
flatness 0.284, centroid 6.1 kHz, peak -1.8 dBFS at 0.25 bus (trim to -8). The tick's
damage number keeps its CLANK at vel 0.3 so the HP loss reads.

Flame (WK_FLAME): a roar loop while the cone is lit (`fuel` falling frame to frame):
brown noise LP 500 Hz q 0.7 at 0.6 with a 9 Hz 30 percent flutter, plus white BP 3 kHz
at 0.15 (the hiss). Ignite on the button edge with fuel: `hiss_` 120 ms at 0.8 then the
loop ramps in over 80 ms with a "whump" (`body_` pink LP 300 Hz, 120 ms, 0.6). Fuel out:
the loop drops over 100 ms with three `crk_` pops. A bot that is BURNING (`burn_t > 0`)
runs a crackle on its `dmg_` slot: density 0.02, HP 3 kHz, at 0.25, with random pops
(`xt_` 1 kHz 20 ms at 0.2 every 200 to 500 ms).

Hammer (tell and strike): EV_WEAPON_TELL zone 0 with `weapon == WP_HAMMER` plays the wind
up: `hiss_` 6 kHz 150 ms at 0.5 (the pneumatic charge) and a `whine_` sweep on the slot's
weapon loop 200 to 900 Hz over the 15 windup frames (the arm). The strike is the HK_HAMMER
impact; a miss (zone 1) plays the strike with no victim: thud 55 Hz at vel 0.8, `body_`
140 ms, a `ring_` 220 Hz at 0.3 (the floor), `crk_` 0.4. Prototype `HAMMER_thud`.

Flipper fire (EV_WEAPON_TELL zone 0, `weapon == WP_FLIPPER`): a hard pneumatic burst:
`hiss_` white BP 5 kHz q 0.9, 60 ms at 1.0, `body_` 40 ms at 0.5, then at +30 ms a plate
`ring_` 440 Hz at 0.5 with `xt_` 0.6 (the plate hitting its stop). On EV_LAUNCH the
victim gets a whoosh: `Loop` white BP sweeping 800 to 300 Hz over the airtime at 0.3
(the sim gives vy 7, about 1.4 s of air at g 10). Landing (EV_LAND with `speed`): CRUNCH
at vel = clamp(speed / 7, 0, 1) with the body at pink LP 400 Hz 140 ms and a `deb_`
rattle 300 ms; an inverted landing adds a second `ring_` 110 Hz at 0.6 100 ms later (the
shell settling). Prototype `FLIPPER_fire`.

Spike: EV_WEAPON_TELL zone 2 (new) on extend: `hiss_` 40 ms at 0.5 plus a `ring_` 880 Hz
at 0.2 click; the strike is HK_SPIKE; retract at active end: a 30 ms hiss at 0.3.

Crusher: zone 3 clamp start: a servo whine on the weapon loop, 300 to 180 Hz over the 24
frames at 0.3 with a 50 Hz Loop hum; zone 4 bite: the HK_CRUSHER recipe; hold: the creak
loop (pink BP 400 Hz q 4, 6 Hz cutoff LFO, 0.3) for the 120 lock frames with a `ring_`
165 Hz at 0.2 every 20 frames (the frame flexing); zone 5 release: `hiss_` 80 ms 0.5 and
a `ring_` 330 Hz 0.3.

Lifter: zone 6 engage: servo whir (Va triangle 160 Hz with a 4 Hz tremolo at 0.25 on the
weapon loop) for the lift, CLANK 0.4 on contact; carry: the whir at 0.15 plus the victim's
drive loop goes to a spinning wheels squeal (its wheels are off the floor: drv_ pitch
up 40 percent, level 0.4, no rumble); zone 7 release or drop: `body_` 80 ms 0.5 and a
landing CLANK at 0.5.

Wedge scrape (underride): while `scrape_t > 0` on a bot (section 4.3: the sim counts
frames a wedge plate is sliding under another body), the wedge's weapon loop plays the
scrape: pink BP sweeping 600 to 1500 Hz (q 3) with the sweep rate tied to closing speed,
level 0.35 + 0.3 vel, plus `crk_` grit at density 0.01. The hop (EV_RAM with zone front
and the victim's vy set) plays HK_RAM CRUNCH at vel 0.5 and a 60 ms `body_` lift. The
victim's drive loop goes to the airborne squeal for the hop frames. Prototype `WEDGE_scrape`.

Melty: the whine at the body pitch (70 to 520 Hz) with the 1.5 Hz beat; its hits are
HK_SPINNER with the ring from the low set.

### 3.5 Drive loops

`audio_drive(slot, kind, speed, turn, grounded)` once per frame from fight_audio:
`speed = |v| / top`, `turn = |w| / 3`, `grounded = !airborne && !lifted_by`. One shared
`NoiseGen` (pink) feeds four `Svf` lowpasses so the whole drive bed costs one noise draw
per sample.

| drive kind | recipe | level at full speed (SFX 8) |
|---|---|---|
| DK_TREAD (TREADS, LEG TRACKS) | pink LP 180 + 120 speed Hz, q 0.8, plus a 7 + 9 speed Hz square tremolo (the links), plus a `ring_` 90 Hz vel 0.08 every link beat | -22 dBFS RMS (TREADS is the loud one: 1.3x) |
| DK_WHEEL (TWO, FOUR, SIX) | pink LP 400 Hz at 0.3 speed plus a Va triangle wheel whine 120 + 500 speed Hz at 0.08 (SIX a fifth lower, TWO a fifth higher), a 30 ms `crk_` squeal at turn > 0.7 and speed > 0.5 (a skid) | -28 dBFS |
| DK_HOVER | the blower: pink BP 240 Hz q 1.2 at 0.35 constant with the pitch up 30 percent while `drive_t > 0.5`, plus a white HP 4 kHz at 0.08; a 0.5 Hz level wobble; the skirt pinch (`skirt_t > 0`) drops the pitch 20 percent | -24 dBFS |
| DK_WALKER | a servo step every 300 / (0.3 + speed) ms: `body_` pink LP 300 Hz 60 ms 0.35 plus a `ring_` 180 Hz vel 0.1; no bed | -26 dBFS |
| DK_MELTY | the whine owns it; the drive loop is the wheel whine at 0.05 | |

Boost (EV_BOOST): a `hiss_` 150 ms at 0.6 and the drive loop pitch up 50 percent for 20
frames. Stuck (EV_STUCK every 60 frames): a wheel squeal `crk_` 400 ms at 0.5 with the
wheel whine at 1.3x pitch. Airborne: the loop goes to the squeal (pitch up 40 percent,
level 0.4, no bed). Inverted and driving: pitch down 30 percent, a 3 Hz grind tremolo.
Wall contact at speed (EV_WALL with `speed`): CLANK at vel = speed / 6 with the body
dominant (pink LP 500 Hz, 100 ms); over 4 u/s (the 2 HP hit) CRUNCH; the pinned state
(`pinned & 1` with `drive_t > 0.5`) adds the stuck squeal at 0.3.

### 3.6 Damage states, panel shed, burst, KO, ring out

Per frame `audio_damage(slot, hp_frac, burning, alive)`:

- Under 40 percent: electrical fizz on `dmg_`: `Crackle` density 0.006 + 0.02 (0.4 - hp)
  HP 3.5 kHz at 0.18, bursts (a 300 ms run of density 0.08 at 0.3) every 1.5 to 4 s from
  the slot's rng, a `ring_` 2600 Hz vel 0.08 ping with every burst (an arcing relay).
- Under 20 percent: the death rattle joins: the drive loop gets a 11 Hz 60 percent
  tremolo (a missing tooth), `body_` pink LP 200 Hz 50 ms pops at 0.25 every 400 to
  900 ms, the fizz density doubles.
- EV_SHED (a panel falls off): `ring_` at a draw 440 to 880 Hz vel 0.7 then the panel
  bouncing: three `ring_` hits at 0.4, 0.25, 0.12 spaced 90, 70, 50 ms (the sim debris
  bounces: `debris_t.bounced` could drive these exactly; the fixed pattern is fine) plus
  `crk_` 0.5 and `deb_` 300 ms 0.4.
- EV_BURST (0 HP): the KO blast below is the burst; the eight debris pieces rain for
  1.2 s: `deb_` at 1.0, 4 `ring_` hits at random pitches 220 to 1760 Hz, vel 0.5 to 0.2,
  spread 80 to 300 ms apart, `crk_` density 0.012 for 1.2 s.
- KO (EV_KO, EV_MINI_KO): thud 40 Hz decay 0.35 drive 3.5 at 1.0; `body_` brown LP
  400 Hz 350 ms at 1.0 (the boom); `xt_` at 1.0; `bell_` 82 Hz decay 1.2 at 1.0; the
  burst debris rain; then at +250 ms the KO stinger on the palette (`gong_` E2 as today
  at 0.6, the only musical element) and the crowd roar (3.8). Prototype `KO_blast`:
  59 / 30 / 1 / 2 / 8, flatness 0.067, T20 64 ms with a 270 ms body. The music drops to
  the drums stem (`audio_slowmo`, unchanged) and ducks 12 dB (3.10).
- Ring out (THE ROOF edge, the pit): new EV_FALL (4.2) when `falling` is set: the
  victim's drive loop cuts, a whoosh (white BP 1200 to 300 Hz over 0.8 s at 0.4) and its
  whine spin down; the KO at the bottom is a CRUNCH at vel 1 with a long `deb_` (the bot
  lands on something far away: lowpass the whole KO 2 octaves for a roof fall). The pit
  (KO_PIT) is the KO blast with the body lowpassed to 200 Hz and a 600 ms `deb_`.
- Count out tick (EV_COUNT_TICK inverted): replace the -40 dB hat with the `COUNT` bell
  at 0.5 plus `tick_` 0.6 so it is heard (P14); the stillness count keeps COUNT short.
- Self right (EV_SELFRIGHT): servo whir 250 to 500 Hz over 300 ms at 0.3, then a CLANK at
  0.6 when the bot lands (`zone 1` is the SHELL roll: just the clank).

### 3.7 Hazards

`EV_HAZ_HIT` carries the hazard index in `b`; main.c already resolves the type. Tells
(`EV_HAZ_TELL`, zone 0 timed and 1 seat fired) keep the klaxon tell variant but each type
gets its own loop and hit:

| hazard | tell add | loop while `M.haz[i].active` (polled in fight_audio, the two nearest active hazards to any bot on `haz_[2]`) | hit |
|---|---|---|---|
| PULVERIZER | the hiss charge 300 ms | none | thud 48 Hz 1.0, `body_` 160 ms 1.0, `bell_` 110 Hz, `deb_` 400 ms: a WRECK with the lows doubled |
| KILLSAW | a spin up whine 200 to 1200 Hz over the tell | the saw grind bed at 0.4 with the 41 Hz teeth, pitched 1.3x | the grind bite burst plus CRUNCH vel 0.7 |
| FLAME grate | the ignite whump | the flame roar at 0.5 | fizz; the bot starts BURNING |
| SCREW | none | pink LP 150 Hz at 0.3 with a 3 Hz tremolo (the conveyor) | `body_` 80 ms 0.4 every 30 frame tick |
| PIT | none | none | the pit KO above |
| LASER | a rising sine 1 to 3 kHz over the tell (Va sine shape 3 on `haz_` at 0.2) | a 60 Hz hum at 0.15 plus a white HP 8 kHz at 0.06 that pans with `hs->aux` (the beam x) | zap: `xt_` 8 kHz 30 ms 0.9, `crk_` density 0.05 200 ms 0.8, a `ring_` 1760 Hz 0.4 |
| FLOORFLIP | the hiss charge | none | the flipper fire burst at 1.2x plus the launch whoosh |
| DROPZONE | a crane whine 400 to 250 Hz over the 90 frame tell | none | `body_` pink LP 300 Hz 200 ms 1.0, `deb_` 600 ms, three `ring_` at 0.5: the CRT, the fridge, the box (`hs->aux` picks: 0 adds a glass `bell_` material 3 at 2 kHz, 1 a 60 Hz thud, 2 a `Crackle` cardboard crunch) |
| BUMPER break, BENCH break | | | `EV_BUMPER_BREAK`: `ring_` 220 Hz 0.8 twice, `deb_` 500 ms; `EV_BENCH_BREAK`: wood `bell_` material 4 at 150 Hz 0.9, `crk_` 400 ms, `body_` 100 ms |
| WIND | | pink LP 600 Hz at 0.2 scaled by `|wind|`, a 0.3 Hz swell | none |
| HILL, MOUND, DECK, RAMP | | none | deck count tick: COUNT short; ramp launch: the whoosh |

### 3.8 Crowd

The bed stays pink noise (two bandpasses) at 0.12 in a fight but raised to -34 dBFS RMS
(the preview bed measured -42, inaudible: P10). Reactions are the bed plus three
`Formant` voices (engines/formant.h: vowel 0 to 1 morph, pitch 110, 140 and 180 Hz with
a 15 cent drift, breath 0.3, level 0.4) and a stomp:

- OOOH (BAND_HEAVY, a landing over 4 u/s, a spin kill): the bed to 0.5 over 150 ms and
  back over 1.5 s; one formant voice vowel 3 ("o") for 600 ms sliding down a tone.
- ROAR (BAND_HUGE, CLASH, a flip landing inverted, RED TAG): bed to 0.8 over 80 ms, back
  over 2.5 s; three formant voices vowel 0 ("a") for 1.2 s with a slow rise; `crk_`
  density 0.03 at 0.2 (clapping) for 1.5 s.
- KO: ROAR at 1.0 for 3 s with the stomp: `thud_` 55 Hz at 0.3 on every beat of the
  music grid (`kBeatMs`) for 3 s, then back to the bed over 3 s.
- Countdown: a hush (bed to 0.04) through 3 2 1, the roar at the horn (ROAR at 0.6).
- OVERTIME: the bed to 0.2 and a continuous low rumble (brown LP 120 Hz at 0.12).
- Taunt: three formant voices vowel 1 for 300 ms at 0.3 (a jeer), bed 0.3.

### 3.9 The stingers, retuned (menus, lock, count, horn, klaxon)

These stay on the song palette (E minor) and keep the slap delay; changes are small:

- MENU_MOVE: keep; drop the hat to 0.15 (it is 22 percent over 2 kHz and clicks).
- MENU_OK and MENU_BACK: keep.
- LOCK: swap the E1 impact kick for the CRUNCH thud at 70 Hz plus `xt_` 0.5 (a case
  latch) under the bells, so the lock sounds like the garage, not the fight.
- COUNT: keep E5; the short variant adds `tick_` at 0.6 (was 0.2, P14).
- HORN: keep the pitches; add a brass edge: Saturator drive 3 on the buzz (today the
  buzz is a clean square at 86 percent 120 to 500 Hz) and a `hiss_` air burst at 0.3 on
  the onset; the low variant (OPS) adds the 60 Hz hum under it.
- KLAXON: keep; the tell variant pans to the hazard x.
- KO card: the gong stays as the musical tail of the blast (3.6), 250 ms after it.
- Parts (garage): keep the table; swap every `I_SPARK` for a `Burst` or `Crackle` of the
  same length (the saw, flame, flipper, spinner spin ups), swap the treads' and hammer's
  E1 kick for the thud at 55 Hz, and add the flame ignite whump. The weapon test fire
  (RT in the garage) plays the weapon's tell and strike from 3.4 with no victim, which is
  the first time a new player hears what the button does.

### 3.10 Mix, priority, voice budget, duck rules

Buses and their trims at SFX 8 (`SfxSpan`), with the target levels the bake checks:

| bus | voices | trim | target (alone, SFX 8) | duck sends |
|---|---|---|---|---|
| impact | xt, body, thud, bell, ring (through `punch_`) | so WRECK peaks -2 dBFS, CRUNCH -4, CLANK hit -8, tap -12 | | sidechain source |
| weapon loops | wpn x4, haz x2 | whine full -18 dBFS RMS, grind -16, roar -16, scrape -20 | | ducked 3 dB by impacts |
| drive loops | drv x4 | tread -22, wheels -28, hover -24 | | ducked 6 dB by impacts, 3 dB by weapon loops of the same slot |
| damage | dmg x4, crk, deb | fizz -30, rattle -26, debris tail -16 peak | | ducked 3 dB by impacts |
| crowd | bed, formants, stomp | bed -34 RMS, OOOH -26, ROAR -20, KO -16 | | ducked 2 dB by impacts |
| stingers | buzz, bells, gong, tick | as today | | never ducked |
| music (audio_pcm.c) | stems | see section 5 | | the band's duck below |

Priority and steal (`Band::Alloc(pool, prio)`): KO 5, CLASH and WRECK 4, CRUNCH 3,
stingers 3, CLANK 2, tells and hazards 2, loops are never stolen (fixed slots), debris
and fizz 1. A fire at priority p takes a free voice, else the quietest voice with priority
under p, else nothing. One impact per victim slot per 2 frames (replaces the per band
rule: a saw's ticks collapse into the grind, two taps on the same bot in consecutive
frames become one at the higher velocity).

Duck rules (sidechain in `SfxSpan`, replacing `audio_duck_hit`'s single value):
`audio_duck_band(band)` sets the music target: TAP 1 dB, HIT 2.5 dB, HEAVY 5 dB, HUGE and
CLASH 8 dB, KO 12 dB; attack 15 ms, hold 60 ms, release 180 ms (KO: hold 400, release
900). The same envelope ducks the drive loops 6 dB and the weapon loops, damage and crowd
3 dB. The pause duck (0.3) and the slow motion stem change are unchanged. Slow motion
also pitches every loop down 40 percent (`audio_slowmo` sets a loop pitch multiplier of
0.6 ramped over 6 frames; the Va `SetFreq` and the Loop cutoffs read it), which is what
makes the KO slow motion sound slow.

CPU budget: the dense second of `bake_music` ("band cost" line, now with four drive loops,
four weapon loops, two hazard loops, the fizz, a clash, a wreck and a KO over the crowd)
stays under 0.7 ms on this Mac (today 0.448 with the old band). On the Pi the `[perf]` line
is the judge; if it misses, the first cut is `drv_` down to two voices (the two human
seats), then the formants, then the hazard loops.

## 4. Interfaces

### 4.1 audio.h additions (audio agent)

```c
/* impact kinds for audio_hit: what hit, on top of the band (5.1) */
enum { HK_GENERIC, HK_SPINNER, HK_SAW, HK_HAMMER, HK_SPIKE, HK_RAM, HK_FLIPPER, HK_FLAME, HK_CRUSHER, HK_LIFTER, HK_OPS,
       HK_HAZ_PULVERIZER, HK_HAZ_KILLSAW, HK_HAZ_FLAME, HK_HAZ_SCREW, HK_HAZ_LASER, HK_HAZ_DROP, HK_HAZ_PIT, HK_WALL, HK_LAND, HK_COUNT_ };
/* weapon loop kinds for audio_weapon; drive kinds for audio_drive */
enum { WK_NONE, WK_SPIN_HORIZ, WK_SPIN_DRUM, WK_MELTY, WK_SAW, WK_FLAME, WK_HAMMER_ARM, WK_CRUSHER_SERVO, WK_LIFTER_SERVO, WK_SCRAPE, WK_COUNT_ };
enum { DK_NONE, DK_WHEEL_TWO, DK_WHEEL_FOUR, DK_WHEEL_SIX, DK_TREAD, DK_HOVER, DK_WALKER, DK_LEGTRACK, DK_MELTY, DK_COUNT_ };
/* a one shot impact: kind, band (juice.h BAND_*), velocity 0..1, world x z for the pan, the victim's armour (AR_*, or -1) */
void audio_hit(int kind, int band, float vel, float x, float z, int armour);
/* the per slot loops, once per render frame (slot 0..3 seats, 4 OPS); level 0 silences, pitch 0..1, load 0..1 */
void audio_weapon(int slot, int kind, float level, float pitch, float load);
void audio_drive(int slot, int kind, float speed, float turn, int grounded, int inverted);
void audio_damage(int slot, float hp_frac, int burning, int alive);
/* one shots that are not impacts */
enum { SFX_TELL_HAMMER, SFX_TELL_FLIPPER, SFX_TELL_SPIKE, SFX_TELL_CRUSHER, SFX_BITE_CRUSHER, SFX_RELEASE_CRUSHER,
       SFX_LIFT_ENGAGE, SFX_LIFT_RELEASE, SFX_FLAME_IGNITE, SFX_FLAME_OUT, SFX_SPIN_KILL, SFX_SHED, SFX_BURST, SFX_SELFRIGHT,
       SFX_BOOST, SFX_STUCK, SFX_FALL, SFX_LAUNCH_WHOOSH, SFX_BENCH, SFX_BUMPER, SFX_HAZ_TELL_LASER, SFX_HAZ_TELL_DROP, SFX_HAZ_TELL_PNEU, ... };
void audio_play_at(int sfx, int var, float x, float z);     /* audio_play_var with a pan */
/* hazard loops: the two loudest active hazards, polled per frame; type HZ_*, level 0..1, x for the pan, aux (laser beam x) */
void audio_hazard(int slot, int type, float level, float x, float aux);
/* crowd reactions */
enum { CROWD_OOOH, CROWD_ROAR, CROWD_KO, CROWD_HUSH, CROWD_JEER };
void audio_crowd_react(int kind);
/* the band's own duck by band, replacing audio_duck_hit */
void audio_duck_band(int band);
```

`audio_whine` stays as a thin wrapper on `audio_weapon(slot, WK_SPIN_HORIZ, ...)` for one
release so nothing breaks while game_flow.c moves over. `SFX_HIT_LIGHT`, `SFX_HIT_HEAVY`
and `SFX_CLASH` stay in the enum (the parts table and the probe use them) but map to
`audio_hit(HK_GENERIC, band, 0.6, 0, 0, -1)`.

### 4.2 Sim event fields and new events (sim agent; coordinate with the physics list)

Add to `event_t` (sim.h 100), after `zone`: `uint8_t weapon; float speed;`. Both are
written by the existing emitters and cost nothing in the hash (events are not hashed).

- `weapon`: `WP_*` of the attacker's weapon for EV_HIT and EV_RAM (`weapon_hit` 719,
  `ram_check` 962, `flame_step` 838, the carry saw 926, OPS shove 1662); `WPX_MELTY 250`,
  `WPX_HAZARD 251` (EV_HAZ_HIT; the hazard type comes from `b`), `WPX_OPS 252`,
  `WPX_LAND 253`, `WPX_WALL 254`, 255 none. `apply_damage` gets `weapon` already as an
  argument; `ev_hit` gets one more parameter.
- `speed`: the closing speed in u/s for EV_HIT and EV_RAM (`closing_of`'s `|rel| + tip`
  before the clamp, so a stationary spinner on a standing target reports its tip speed
  3.6), `vn` for EV_WALL and EV_SPIN_WALL (1052), `|vy|` at touchdown for EV_LAND (1257,
  1259), `vy` at launch for EV_LAUNCH. main.c maps `vel = clamp(speed / 8, 0, 1)` for
  hits, `speed / 6` for walls, `speed / 7` for landings.
- EV_WEAPON_TELL `zone` codes: 0 hammer wind up or flipper fire (existing), 1 hammer
  miss (existing), new 2 spike extend (sim.c 692), 3 crusher clamp start (705), 4
  crusher bite (the closing frame that lands, 946 area), 5 crusher release (`lock` hits
  0), 6 lifter engage (`carry_t` 0 to 1), 7 lifter release (912 to 915), 8 flame ignite
  (`wb && fuel > 0` edge, 699), 9 flame out (`fuel` reaches 0 while held). `weapon` is
  set on every one so main.c never guesses between the primary and the secondary.
- New `EV_FALL` (a slot, zone 0 roof 1 pit): emitted where `falling` is set (sim.c 1476
  and 1781) so the whoosh starts before the KO.
- New `EV_SPIN_KILL` (a slot, zone 0 wall 1 clash 2 bleed 3 stun, speed = the spin
  before): emitted in resolve_static (1047), the clash (810) and the bleed (760 to 764)
  when spin falls by more than 0.3 in one step. main.c can also detect this by polling
  `spin`, so this event is optional; the field is nicer.
- New per bot field `int16_t scrape_t` in `bot_t` (sim internals block): frames remaining
  (set to 8 each step) while a wedge plate (CH_WEDGE or WP_WEDGE) is in contact with
  another body's front or side at closing over 0.5 u/s; the wedge physics work the
  physics lens is specifying is where this gets set (the underride state). fight_audio
  reads it for the scrape loop. If the physics agent adds an explicit underride state
  (`underride_by`, `under_t`), that replaces `scrape_t`.
- EV_LAND already carries `dmg`; keep it. EV_SHED carries the panel in `zone`; keep it.
- Flame on: no event needed; fight_audio polls `fuel` frame to frame (falling means lit)
  and the sim's `WP_FLAME_FUEL` constant. Saw in contact: main.c keeps a 12 frame timer
  per attacker that each HK_SAW tick resets. Spin, lift, clamp, lock, lifted_by,
  clamped_by, airborne, inverted, pinned, stuck_t, skirt_t, drive_t, b.vx vz w, burn_t,
  hp, hp_max, haz[].active, haz[].aux, wind_x wind_z are all already readable.

### 4.3 main.c drain_events (integration)

- The banded path: `audio_hit(kind_of(e), band, vel_of(e), e->x, e->z, armour_of(e->b))`
  and `audio_duck_band(band)`; `kind_of` from `e->weapon` and, for EV_HAZ_HIT, the hazard
  type; the victim slot debounce of 3.10 replaces `sfx_band_done`.
- EV_WEAPON_TELL: switch on `zone` and `weapon` to the SFX_TELL_* and the loop starts.
- EV_SHED, EV_BURST, EV_SELFRIGHT, EV_BOOST, EV_STUCK, EV_BENCH_BREAK, EV_BUMPER_BREAK,
  EV_FALL, EV_SPIN_KILL: `audio_play_at` of the matching id (today's default case goes).
- EV_HAZ_TELL: per type tell add from 3.7 beside the klaxon.
- Crowd: HEAVY and a hard landing call `audio_crowd_react(CROWD_OOOH)`, HUGE, CLASH, RED
  TAG and an inverted landing `CROWD_ROAR`, KO and MINI_KO `CROWD_KO`, taunt
  `CROWD_JEER`; the countdown `CROWD_HUSH` from game_flow.c.
- `juice.h HIT_BANDS.sfx` column becomes unused by the drain (keep it for the parts and
  the probe); add a `duck_db` column: 1, 2.5, 5, 8, 8, 12.

### 4.4 game_flow.c fight_audio (game agent)

Per slot 0..3 (and 4 for OPS's saw) each render frame: `audio_weapon` from `build.weapon`
(and `weapon2`, the louder of the two), `spin`, `saw_on`, `fuel` delta, `clamp`, `lift`,
`carry_t`, `scrape_t`, `hit load` (a 120 ms timer main.c sets on a landed spinner hit);
`audio_drive` from `build.drive`, `len2(vx, vz) / DRIVES[drive].top`, `|w| / 3`,
`airborne || lifted_by`, `inverted`; `audio_damage` from `hp / hp_max`, `burn_t > 0`,
`alive`; `audio_hazard` for the two active hazards nearest any seat bot (distance under
6 u, level by 1 - d / 6). The countdown calls `audio_crowd_react(CROWD_HUSH)` at 3 and
the horn `CROWD_ROAR`. `audio_slowmo(1)` also sets the loop pitch multiplier.

### 4.5 bake_music.cpp (audio agent)

`Preview()` renders every `SFX_*` id and variant, every `HK_*` kind at every band at vel
0.3, 0.7 and 1.0, every weapon loop at level 1 for 1 s with a spin up and a kill, every
drive loop at speed 1 for 1 s, the damage fizz and rattle, every hazard loop and hit, the
five crowd reactions, and prints per slot: peak, RMS 300 ms, attack, T20, centroid of the
first 100 ms, band shares and flatness (port the measures of `wavstat.mjs` into the tool
so the bake needs no node), and FAILs on the targets of section 6.
`SongPreview(arena)` gets the fight cue list of section 6.2. A new
`build/preview-fight.wav` is the 20 s acceptance mix.

## 5. The music

### 5.1 Measurements

| file | peak / true / RMS dBFS | low / mid / high percent (bake) | per second in the preview (MUSIC 6) |
|---|---|---|---|
| garage.wav (100 bpm, 16 bars) | -2.2 / -1.7 / -16.0 | 70 / 25 / 4 | bed RMS -22 to -26, 56 to 84 percent under 120 Hz, hats at -50 dB |
| arena_base.wav | -14.5 / -14.5 / -21.9 | 83 / 17 / 0 | |
| arena_stomp.wav | -6.7 / -5.0 / -20.7 | 56 / 32 / 11 | kick every 0.77 s (half time: a 78 bpm feel for the first 8 bars of every fight) |
| arena_break.wav | -6.3 / -4.5 / -18.2 | 49 / 35 / 16 | |
| arena_riff.wav | -9.7 / -9.7 / -22.1 | 5 / 90 / 5 | a chorused saw lead, in only under 30 HP or the last 20 s |
| preview-arena mix | music alone peak -13.0, RMS -27.4 | | 1 to 4 percent over 2 kHz in every second; the hits are the only top end |
| overtime.wav (172 bpm) | -2.4 / -1.5 / -14.9 | 61 / 30 / 10 | the one song with air: sirens at 2.5 kHz, the noise riser visible in `preview-overtime.png` |

Against the brief (industrial, heavy, robot combat): the arrangement is right (a
distorted bass riff, metal on the off beats, a riser, sirens, a roll), the tempo and the
stomp are right on paper, and the mix buries all of it. Heavy is not the same as low:
industrial heaviness lives at 100 to 300 Hz (the body of the kick and the bass fuzz) and
1.5 to 5 kHz (the distortion harmonics, the metal, the snare crack). This mix has the
sub and the fuzz's fundamental and nothing else. The hats are 3.0 x 0.85 into a tanh
after the kick has already filled it, so they vanish; the metal clinks are at 0.8 x 0.8
at `Pal(88)` with decay 0.35 (a glockenspiel, not a dropped pipe); the snare is tone 0.5
decay 0.17: no crack. The riff lead is a clean chorused saw: a trance lead over an
industrial bed.

### 5.2 Prescriptions (audio_band.cpp song data and voicing, then the bake)

1. Voice the kit for the brief (`Voice()`, 725 to 760): ARENA kick `decay 0.26, drive
   7.0, click_tune 12, pitch_decay 0.035` at 48 Hz (the click is the attack a cabinet
   speaker shows); snare `tone 0.7, decay 0.2, snap 0.5` at 210 Hz (the crack); hat
   `tone 1.2, decay 0.045`, open hat `decay 0.22`. In `MusicSpan` raise the hat bus to
   5.0 and the open hat to 2.6, the snare to 1.8, and move the kit's `drum_sat_` to
   `drive 2.6, tone 0.25` so the tanh adds harmonics above 1 kHz instead of flattening
   the kick. Target for the drums stem: 45 / 35 / 20.
2. The metal layer must be metal: `metal_` clink `decay 0.5, brightness 0.9, level 0.9`
   at pitches drawn from {1760, 2349, 2960, 3520} Hz (not the palette), the anvil
   `material 0, decay 1.1, brightness 0.6` at 220 and 294 Hz with a `Burst` white BP
   4 kHz 8 ms transient on every anvil hit (the hammer strike). Add a chain: a `Crackle`
   (density 0.02, 120 ms) on steps 3 and 11 of STOMP at 0.4.
3. The bass needs its fuzz harmonics heard: `bass_sat_ drive 6 -> 9, tone 0.4 -> 0.6`
   and a parallel bandpass copy (Svf BP 1.4 kHz q 1.5) of the saturated bass mixed at
   0.35 into the base stem (the "guitar" that the brief implies). Keep the sub 0.5.
   Target for the base stem: 65 / 30 / 5 (today 83 / 17 / 0).
4. Section order and tempo feel: ARENA opens on STOMP for 8 bars (12.3 s of half time)
   at the horn, when the fight is fastest. Either open on BREAK (`SONGS[2]` in
   audio_pcm.c and the `ARENA[]` order) or cut STOMP to 4 bars; the 156 bpm is right,
   the first 12 s at 78 are not. The riser riff stays a tension stem but gets
   `lead_sat_ drive 2.5 -> 6, tone 0.3`, `chorus_ mix 0.3 -> 0.12`, and an octave down
   copy at 0.5 (a lead that is a second riff, not a trance line).
5. Master tilt at the bake (`RenderStem`): a `bellows::Eq3` (fx/eq.h) on every stem,
   `low 100 Hz -2 dB, mid 2.5 kHz q 0.8 +3 dB, high 6 kHz +3 dB`, re-measured with the
   group gain so the ceiling holds. This alone takes the arena mix from 1 to 4 percent
   over 2 kHz to about 10 to 14, and it is one line per job.
6. Garage: it is a menu bed and the part sounds sit on top of it; drop the sub (`bass_p_
   sub 0.5 -> 0.3` for mode 1), bring the hats and clinks up with item 1 and 2 so the
   shelf has the air of a workshop, and add a slow `Crackle` (density 0.003, the
   fluorescent tube) at 0.1 on the B section. Keep 100 bpm.
7. Overtime: keep. Give the sirens the saturator (`drive 3`) so they bite, and let the
   roll's last four hits pass through a `Burst` white BP 3 kHz 20 ms transient.
8. Levels: once the SFX are rebalanced (hits 6 to 8 dB over the bed by shape, not by
   volume), `MODE_TRIM` arena 0.4 -> 0.56 (+3 dB) and overtime 0.45 -> 0.6, so the bed
   sits at -24 dBFS RMS at MUSIC 6 under a -16 dBFS fight. Re-copy `kSongTrim`.
9. Determinism and size: the stems stay 22050 Hz mono; items 1 to 7 change the
   content, not the format; check `total bytes` against the 4.4 MB line (today 4.36 MB,
   so the EQ must not lengthen anything, and it does not).

## 6. Acceptance

### 6.1 The SFX table (native, `./build.sh music`)

`build/sfx_preview.wav` holds every sound of section 3 at 1 s a slot, SFX 8 MUSIC 6, and
the tool prints the measures and FAILs if any of these miss:

| sound | peak dBFS | RMS 300 ms | flatness 0 to 100 ms | centroid 0 to 100 ms Hz | bands, percent |
|---|---|---|---|---|---|
| CLANK tap (vel 0.3) | -14 to -10 | under -26 | over 0.01 | 1000 to 3500 | over 2 kHz: over 20 |
| CLANK hit (vel 0.7) | -10 to -6 | | over 0.01 | 1000 to 3500 | |
| CRUNCH (vel 0.7) | -6 to -3 | -20 to -14 | over 0.03 | 400 to 2500 | under 120: 8 to 30; over 2 kHz: over 8 |
| WRECK (vel 1.0) | -3 to -1 | -18 to -12 | over 0.03 | 300 to 2000 | under 120: 25 to 50; over 2 kHz: over 6 |
| KO | -1.5 to -0.8 | -16 to -10 | over 0.03 | | under 120: 40 to 65; T40 over 600 ms |
| spinner whine full | under -10 | -20 to -16 | | 400 to 1200 | under 120: under 10 |
| spin kill | -8 to -4 | | over 0.02 | | |
| saw grind (in contact) | under -6 | -18 to -14 | over 0.15 | 2000 to 7000 | |
| flame roar | under -8 | -18 to -14 | over 0.1 | 150 to 600 | |
| hammer tell plus strike | -4 to -1 | | | | the tell at -14 or louder 250 ms before the strike |
| flipper fire | -8 to -4 | | over 0.05 | 1500 to 5000 | |
| landing (speed 7) | -5 to -2 | | over 0.03 | | |
| wedge scrape loop | under -14 | -24 to -18 | over 0.02 | 500 to 2000 | |
| tread rumble full | under -16 | -24 to -20 | | under 300 | |
| wheel whine full | under -22 | -30 to -26 | | | |
| hover blower | under -18 | -26 to -22 | | 150 to 500 | |
| fizz (hp 0.3) | under -24 | -32 to -28 | over 0.2 | over 2500 | |
| death rattle (hp 0.15) | under -20 | -28 to -24 | | | |
| panel shed | -8 to -4 | | | | three decaying bounces visible in the envelope |
| hazard hits (each) | -6 to -1 | | over 0.03 | | |
| crowd bed / OOOH / ROAR / KO | -34 / -26 / -20 / -16 RMS | | | | ROAR has formant peaks at 600 to 1200 Hz |
| every menu stinger | -14 to -8 | | | | unchanged from today within 2 dB |
| every one shot alone | never over -0.8 (the limiter is not the gain stage) | | | | |
| band cost line | under 0.7 ms per 800 frames on this Mac with the dense fight second | | | | |

### 6.2 The fight preview (native, 20 s, mixed with music)

`build/preview-fight.wav`: ARENA from the top at the cart's gains under this script
(seconds; two slots driving the whole time, slot 0 TREADS and HORIZONTAL, slot 1 FOUR
WHEEL and SAW): 0 horn and CROWD_ROAR 0.6; 0.5 both drive loops to speed 0.8; 1.0 slot 0
spin up over 2 s; 3.2 CLANK hit vel 0.6 (HK_RAM) on slot 0 with the scrape 0.4 s before it;
4.5 saw on, 5.0 saw grind 1.5 s with three HK_SAW ticks; 6.4 WRECK HK_SPINNER vel 1.0 on
slot 1, CROWD_ROAR, slot 0 load dip; 7.0 slot 1 at hp 0.35: fizz; 8.6 hammer tell on slot
1 (it has none: the script uses a third voice), 8.85 CRUNCH HK_HAMMER; 10.0 EV_SPIN_KILL
wall on slot 0; 11.5 klaxon tell plus the pulverizer charge; 12.0 pulverizer hit WRECK;
13.0 flipper fire, 13.05 launch whoosh, 14.4 landing speed 7 inverted, CROWD_OOOH; 15.0
slot 1 at hp 0.15: rattle; 16.3 CLASH; 17.0 panel shed; 18.3 KO on slot 1, CROWD_KO,
slow motion; 20 end. Measures printed and checked: mix RMS -18 to -13 dBFS; true peak
under -0.8; music alone RMS -26 to -22 (MUSIC 6, after 5.2.8); every impact's 100 ms RMS
6 to 10 dB over the median second of the bed; the duck visible as the bed's level in the
100 ms after the WRECK at 6.4 being 7 to 9 dB under the 100 ms before it; the loops
audible: the second from 5.0 to 6.0 (grind) has over 15 percent of its energy over 2 kHz
and the second 1.5 to 2.5 (whine spin up) has a tracked fundamental rising from under
200 Hz to over 600 Hz (the tool reports the peak bin per 100 ms).

### 6.3 In the cart (the rig)

- `WAV=build/out-2p.wav node tools/shot.mjs build/red-tag-rumble.wasc build/shots/a2p 700,900,1200 tools/scripts/2p-fight.json`
  then `node wavstat.mjs build/out-2p.wav 1`: no second with over 85 percent of its
  energy under 120 Hz once the fight starts (frame 668); at least one second with over
  12 percent over 2 kHz per 5 s of fight; peak never over -0.8 dBFS.
- `tools/scripts/feel.json` (the 14 damage WEDGE ram at 863): the capture shows the
  scrape loop starting before 863 and the CRUNCH at 863 with flatness over 0.03.
- `tools/scripts/ks-medium.json` with `STATION=1`: the practice bot's drive loop and
  spinner are audible in the capture (its seat is slot 1), the KO at 2782 has the blast
  and the crowd.
- `[hits]` lines unchanged by the audio change (the sim is untouched by the band; the
  `weapon` and `speed` fields change no hash): `build/balance --replay` against the
  cart's `[sim]` lines matches as before; regenerate the fight fixtures with
  `build/balance --make-fight` only if the physics lens changes the sim.
- `[perf]` on the station: no new `half=1` stretches attributable to audio (compare the
  `[perf]` lines of a 2p fight before and after; the band's share is the difference
  between a `MUSIC=live` build and the baked build, which the integrator can print by
  timing `audio_render` under `-DRTR_TRACE`).
- `./build.sh lint` clean (no lowercase in strings; the new ids are ALL CAPS; no non
  ASCII in any new comment).

## 7. Order of work and risks

1. Sim fields (`weapon`, `speed`, the EV_WEAPON_TELL zones, EV_FALL, EV_SPIN_KILL,
   `scrape_t`): half a day, no hash change, unblocks everything else; land first with the
   physics lens's wedge work.
2. Band voices (Burst, Crackle, Loop, the pools, the priority alloc, the pan) and the
   three impact recipes with `audio_hit`: the biggest win per hour; the old `audio_play`
   path keeps working through the wrappers so the cart never goes silent mid branch.
3. Weapon loops and tells, drive loops, damage states, KO, hazards, crowd: in that order
   (each is one `fight_audio` call plus one band voice).
4. The bake's measured table and the fight preview; then the music prescriptions
   (5.2.1 to 5.2.5 first, they are voicing and one EQ line; 5.2.4 is a data reorder).
5. Risks: CPU on the Pi (3.10 lists the cut order); the slow motion pitch multiplier
   must not touch the music stems (they are the mixer's); the 128 event cap in `Push`
   (`kMaxEv`) is enough since loops are not events but the KO rain pushes about 12; the
   formant crowd is the most expensive new voice (five SVFs per voice) and is the first
   to drop; `VoicePool` cannot hold the Burst structs, so the pools are plain arrays with
   the steal rule, which is 30 lines; the saw contact timer in main.c is a heuristic that
   a future `saw_contact` bot field would replace.
