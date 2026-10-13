/* audio.h: the whole contract between C and the band (spec section 11.1).
 * Owned by the audio agent: bodies in audio_band.cpp (Bellows SFX, whines,
 * crowd, the song data) and audio_pcm.c (wc_pcm_mixer stems, modes, fades,
 * beat alignment, and audio_render which mixes both). Headless safe: this
 * header includes nothing from three.c, hud.h or wasmcart, because juice.h
 * names the SFX ids. */
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
void audio_whine(int slot, float level, float pitch);   /* 4 continuous spinner voices; level 0 silences */
/* the crowd bed: a swell to level (0..1) that settles back to the ambient
 * murmur over about 2 s; the murmur itself follows the music mode (a fight
 * has a crowd, a garage does not) */
void audio_crowd(float level);
void audio_slowmo(int on);                /* drums stem only while on, immediate */
void audio_tension(int on);               /* fades the lead stem in while any human bot is under 30 HP */
void audio_duck(float gain);              /* pause: 0.3 */
/* part sounds in the garage (spec 3.1): the part's own sound when a row cycles or RT test fires */
void audio_part(int row, int option);
void audio_render(float *ring, uint32_t cap, uint32_t *cursor, uint32_t rate, double dt_ms);

/* -- between audio_pcm.c and audio_band.cpp (not for the game) ------- */
/* the Bellows side of audio_render: audio_pcm.c calls this after mixing the
 * PCM stems into frames [0, n) of the scratch buffer (interleaved stereo);
 * the band adds its voices on top and runs the master limiter over the sum */
void audio_band_render(float *stereo, int frames, uint32_t rate);
void audio_band_init(uint32_t rate);
void audio_band_levels(int sfx, int music);    /* the two settings rows, 0..10 */
void audio_band_duck(float gain);              /* the pause duck on the band's sustained voices */
void audio_band_crowd_bed(float level);        /* the ambient murmur the swells settle back to */
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
#endif

#ifdef __cplusplus
}
#endif
#endif
