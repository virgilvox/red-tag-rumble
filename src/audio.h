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
void audio_crowd(float level);            /* 0..1 bed */
void audio_slowmo(int on);                /* drums stem only while on, immediate */
void audio_tension(int on);               /* fades the lead stem in while any human bot is under 30 HP */
void audio_duck(float gain);              /* pause: 0.3 */
/* part sounds in the garage (spec 3.1): the part's own sound when a row cycles or RT test fires */
void audio_part(int row, int option);
void audio_render(float *ring, uint32_t cap, uint32_t *cursor, uint32_t rate, double dt_ms);

/* the Bellows side of audio_render: audio_pcm.c calls this after mixing the
 * PCM stems into frames [0, n) of the scratch buffer (interleaved stereo) */
void audio_band_render(float *stereo, int frames, uint32_t rate);
void audio_band_init(uint32_t rate);

#ifdef __cplusplus
}
#endif
#endif
