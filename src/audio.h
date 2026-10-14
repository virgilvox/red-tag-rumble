/* audio.h: the whole contract between C and the band (spec section 11.1,
 * docs/design/deep-audio.md 4.1, DEEP-plan section 9).
 * Owned by the audio agent: bodies in audio_band.cpp (Bellows SFX, the
 * fight voices and loops, crowd, the song data) and audio_pcm.c
 * (wc_pcm_mixer stems, modes, fades, beat alignment, the duck, the event
 * routing and audio_fight_update, and audio_render which mixes both).
 * Headless safe: this header includes nothing from three.c, hud.h,
 * wasmcart or sim.h, because juice.h names the SFX ids. */
#ifndef RTR_AUDIO_H
#define RTR_AUDIO_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { SFX_MENU_MOVE, SFX_MENU_OK, SFX_MENU_BACK, SFX_LOCK, SFX_COUNT, SFX_HORN,
       SFX_HIT_LIGHT, SFX_HIT_HEAVY, SFX_CLASH, SFX_LAUNCH, SFX_KLAXON, SFX_KO, SFX_COUNT_ };

/* variants for audio_play_var */
enum { SFXV_DEFAULT, SFXV_LOW, SFXV_TELL, SFXV_SHORT };

/* music modes for audio_music */
enum { MUSIC_OFF, MUSIC_GARAGE, MUSIC_ARENA, MUSIC_OVERTIME };

/* called once from wc_init after the assets are reachable; rate is the host rate */
void audio_init(uint32_t rate);
/* settings 0..10 (SFX and MUSIC rows) */
void audio_set_levels(int sfx, int music);

void audio_play(int sfx);                 /* one shot; a few have variants chosen by audio_play_var(sfx, var) */
void audio_play_var(int sfx, int var);    /* 0 default, 1 low variant, 2 tell variant, 3 short */
void audio_music(int mode);               /* 0 off, 1 GARAGE, 2 ARENA, 3 OVERTIME */
/* the old spinner whine (kept one release): drives the slot's spinner loop
 * only while audio_fight_update is not driving the loops */
void audio_whine(int slot, float level, float pitch);
/* the crowd bed: a swell to level (0..1) that settles back to the ambient
 * murmur over about 2 s; the murmur itself follows the music mode (a fight
 * has a crowd, a garage does not) */
void audio_crowd(float level);
void audio_slowmo(int on);                /* drums stem only while on, immediate; every fight loop pitches down 40 percent */
void audio_tension(int on);               /* fades the lead stem in while any human bot is under 30 HP */
void audio_duck(float gain);              /* pause: 0.3 */
void audio_duck_hit(void);                /* the old hit duck: the music drops 4 dB for 150 ms */
/* part sounds in the garage (spec 3.1): the part's own sound when a row cycles or RT test fires */
void audio_part(int row, int option);
void audio_render(float *ring, uint32_t cap, uint32_t *cursor, uint32_t rate, double dt_ms);

/* -- the fight voices (DEEP-plan 9, deep-audio.md 3 and 4.1) -------- */
/* impact kinds for audio_hit: what hit, on top of the band */
enum { HK_GENERIC, HK_SPINNER, HK_SAW, HK_HAMMER, HK_SPIKE, HK_RAM, HK_FLIPPER, HK_FLAME, HK_CRUSHER, HK_LIFTER, HK_OPS,
       HK_HAZ_PULVERIZER, HK_HAZ_KILLSAW, HK_HAZ_FLAME, HK_HAZ_SCREW, HK_HAZ_LASER, HK_HAZ_DROP, HK_HAZ_PIT, HK_WALL, HK_LAND, HK_COUNT_ };
/* weapon loop kinds for audio_weapon; drive kinds for audio_drive */
enum { WK_NONE, WK_SPIN_HORIZ, WK_SPIN_DRUM, WK_MELTY, WK_SAW, WK_FLAME, WK_HAMMER_ARM, WK_CRUSHER_SERVO, WK_LIFTER_SERVO, WK_SCRAPE, WK_COUNT_ };
enum { DK_NONE, DK_WHEEL_TWO, DK_WHEEL_FOUR, DK_WHEEL_SIX, DK_TREAD, DK_HOVER, DK_WALKER, DK_LEGTRACK, DK_MELTY, DK_COUNT_ };
/* one shots that are not impacts, after the stingers */
enum { SFX_TELL_HAMMER = SFX_COUNT_, SFX_TELL_FLIPPER, SFX_TELL_SPIKE, SFX_TELL_CRUSHER, SFX_BITE_CRUSHER, SFX_RELEASE_CRUSHER, SFX_LIFT, SFX_LIFT_DROP,
       SFX_FLAME_IGNITE, SFX_FLAME_OUT, SFX_SPIN_KILL, SFX_SPIN_READY, SFX_SHED, SFX_BURST, SFX_SELFRIGHT, SFX_BOOST, SFX_STUCK, SFX_FALL, SFX_LAUNCH_WHOOSH,
       SFX_LAUNCH_WEDGE, SFX_BENCH, SFX_BUMPER, SFX_UNDERRIDE, SFX_RIDE_DROP, SFX_WHEEL_HIT, SFX_DENT, SFX_SCRAPE, SFX_SKID, SFX_GRIND_BITE, SFX_BURN_START,
       SFX_BURN_END, SFX_DIG_IN, SFX_SAW_TOGGLE, SFX_COUNT2_ };
enum { CROWD_OOOH, CROWD_ROAR, CROWD_KO, CROWD_HUSH, CROWD_JEER };
void audio_hit(int kind, int band, float vel, float x, float z, int armour, int attacker_slot);   /* an impact; the attacker's loop takes the load dip */
void audio_play_at(int sfx, int var, float x, float z);                                           /* audio_play_var with a pan; var is the event's zone where the plan says so */
void audio_tell(int slot, int weapon, int tell_zone, float x, float z);                          /* EV_WEAPON_TELL: TELL_* by weapon */
void audio_spin_change(int slot, int zone, float spin_before, float spin_lost);                   /* EV_SPIN_CHANGE except SPIN_ARMED */
void audio_haz_tell(int haz_index, float x, float z);                                             /* the per type tell add beside the klaxon */
void audio_crowd_react(int kind);
void audio_duck_band(int band);                                                                   /* TAP 1 dB, HIT 2.5, HEAVY 5, HUGE and CLASH 8, KO 12; attack 15 ms, hold 60, release 180 (KO 400, 900) */
void audio_weapon(int slot, int kind, float level, float pitch, float load);                      /* the per slot loops; 0..3 seats, 4 OPS */
void audio_drive(int slot, int kind, float speed, float turn, int grounded, int inverted);
void audio_damage(int slot, float hp_frac, int burning, int alive);
void audio_hazard(int slot, int type, float level, float x, float aux);
void audio_fight_update(const void *match);   /* once per render frame in a fight: a match_t (audio.h stays free of sim.h; audio_pcm.c includes sim.h and casts); NULL silences every loop */

/* -- between audio_pcm.c and audio_band.cpp (not for the game) ------- */
/* the Bellows side of audio_render: audio_pcm.c calls this after mixing the
 * PCM stems into frames [0, n) of the scratch buffer (interleaved stereo);
 * the band adds its voices on top and runs the master limiter over the sum */
void audio_band_render(float *stereo, int frames, uint32_t rate);
void audio_band_init(uint32_t rate);
void audio_band_levels(int sfx, int music);    /* the two settings rows, 0..10 */
void audio_band_duck(float gain);              /* the pause duck on the band's sustained voices */
void audio_band_crowd_bed(float level);        /* the ambient murmur the swells settle back to */
/* the fight voices with the pan already worked out (-1 left .. 1 right);
 * the bake calls these directly, the cart through the audio_* routing in
 * audio_pcm.c. Slots are 0..3 seats and 4 OPS; hazard loop slots are 0..1 */
void audio_band_hit(int kind, int band, float vel, float pan, int armour, int attacker_slot);
void audio_band_play_at(int sfx, int var, float pan);
void audio_band_tell(int slot, int weapon, int tell_zone, float pan);   /* weapon is WP_* order of parts.h */
void audio_band_spin_change(int slot, int zone, float spin_before, float spin_lost);
void audio_band_haz_tell(int type, float pan, float tell_s);            /* type is HZ_* of arena.h */
void audio_band_crowd_react(int kind);
void audio_band_duck_band(int band);
void audio_band_weapon(int slot, int kind, float level, float pitch, float load);
void audio_band_drive(int slot, int kind, float speed, float turn, int grounded, int inverted, int flags);
void audio_band_damage(int slot, float hp_frac, int burning, int alive);
void audio_band_hazard(int slot, int type, float level, float pan, float aux_pan);
void audio_band_slot_pan(int slot, float pan);
void audio_band_loops(int on);                 /* 0 lets every fight loop fall silent (the fight is over or paused out) */
void audio_band_slowmo(int on);
void audio_band_whine(int slot, float level, float pitch);   /* the old whine on a seat's horizontal spinner loop */
void audio_band_overtime(int on);
/* the duck envelope's gain on the music stems (1 is none), read by
 * audio_pcm.c once per render and by the bake's fight preview */
float audio_band_music_duck(void);
/* audio_band_drive flags */
enum { DRV_BOOST = 1, DRV_PINCH = 2, DRV_STUCK = 4, DRV_THROTTLE = 8, DRV_FALLING = 16 };
/* the live songs (MUSIC_LIVE builds and the bake): mode as audio_music, stem
 * gains as the mixer would set them */
void audio_band_music(int mode);
void audio_band_stems(float base, float drums, float lead);
/* the song clock shared with the stem sequencer: samples per sixteenth at
 * rate for a mode, the section count, the bars of a section, the loop
 * section (11.2); the bake and audio_pcm.c both derive bar and beat
 * lengths from these so the baked loops and the beat alignment agree */
int audio_song_step(int mode, uint32_t rate);
int audio_song_sections(int mode);
int audio_song_bars(int mode, int section);
int audio_song_loop(int mode);

#ifdef MUSIC_BAKE
/* the bake (tools/bake_music.cpp): a fresh band at rate, the conductor put
 * at the start of one section of a mode with a layer mask (the stem) and
 * optionally held on that section forever, then rendered without the SFX
 * or the master chain so the stems sum linearly in the mixer */
enum { STEM_BASE, STEM_DRUMS, STEM_LEAD, STEM_ALL };
void audio_bake_begin(int mode, int section, int stem, int hold);
void audio_bake_render(float *stereo, int frames);
/* the preview: the SFX band and the master chain over frames that already
 * hold a song from audio_bake_render at the cart's gains; the conductor is
 * not stepped here, so the song comes from audio_bake_render alone */
void audio_bake_sfx_render(float *stereo, int frames);
#endif

#ifdef __cplusplus
}
#endif
#endif
