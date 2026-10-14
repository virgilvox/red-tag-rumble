/* hud.h: the 2D layer. One RGBA buffer composited over the viewports every
 * frame, styled after HeatSync's GANTRY theme (char and bone, one amber
 * accent, square corners, solid offset shadows, caution tape between things).
 *
 * Copied from DO-OCRACY and extended per spec 10.3 and FINAL-frontend 5.
 * Owned by the HUD agent (item 9): bodies in hud.c, glyph bitmaps in glyphs.h.
 * Every coordinate passed to a hud_ call is in 1280 x 720 space whatever
 * HUD_DIV is; hud.c divides. */
#ifndef RTR_HUD_H
#define RTR_HUD_H
#include <stdint.h>
#include <stdbool.h>

#ifndef HUD_DIV
#define HUD_DIV 1            /* 1: a 1280 x 720 buffer; 2: 640 x 360, fonts baked at half size */
#endif
#define HUD_REF_W 1280
#define HUD_REF_H 720
#define HUD_W (HUD_REF_W / HUD_DIV)
#define HUD_H (HUD_REF_H / HUD_DIV)

/* GANTRY tokens (new-hsl/src/styles/tokens.css) */
#define C_CHAR      0x15120f
#define C_GRIME     0x1d1915
#define C_SLAB      0x26211b
#define C_SLAB_HI   0x2f2920
#define C_STEEL     0x3a342b
#define C_STEEL_HI  0x4a4338
#define C_BONE      0xece3d3
#define C_ASH       0xa79d8c
#define C_SMOKE     0x756c5e
#define C_HAZARD    0xf2ab1e
#define C_HAZARD_DP 0xc98714
#define C_RUST      0xc2542c
#define C_LIVE      0x5fb892
#define C_FAULT     0xd24f3a
#define C_VIOLET    0x9b7fd0
#define C_INFO      0x5e93c4
#define C_TAPE      0x14110d
#define C_SHADOW    0x0a0806
#define C_INK_OK    0x7fd0aa
#define C_INK_ERR   0xef7b68
#define C_WHITE     0xffffff
#define C_LOGO_SUN  0xf99a1c   /* the mark's own orange, never the UI amber */

/* fonts (10.3): DO-OCRACY's seven plus the two stinger faces; F_LOGO is dropped */
enum { F_TITLE, F_H1, F_H2, F_H3, F_MONO_L, F_MONO, F_MONO_S, F_STINGER, F_STINGER_Q, F_COUNT };
enum { AL_LEFT, AL_CENTER, AL_RIGHT };

/* 16 x 16 one-bit glyphs (10.3), data in glyphs.h */
enum { GL_PAD, GL_KEYS, GL_BOT, GL_AI, GL_SPINNER, GL_DRUM, GL_FLIPPER, GL_HAMMER, GL_FLAME, GL_SAW, GL_SPIKE, GL_LIFTER,
       GL_CRUSHER, GL_WEDGE, GL_TREAD, GL_WHEEL, GL_HOVER, GL_WALKER, GL_LEG, GL_MELTY, GL_TAG, GL_SKULL, GL_FLAG, GL_CROWN,
       GL_PIT, GL_SCREW, GL_RAMP, GL_BUMPER, GL_DECK, GL_FLIP_PANEL, GL_DROP, GL_BEAM, GL_WIND, GL_BRACKET, GL_TWO_BOTS,
       GL_PLUS, GL_CHECK, GL_COUNT };

/* a coloured run of text; a caption is an array of runs, terminated by s == NULL */
typedef struct { const char *s; uint32_t c; } run_t;

extern uint8_t *hud_px;     /* RGBA, HUD_W x HUD_H, rows stored bottom up (GL texture order) */

void hud_init(void);
void hud_clear(void);
void hud_rect(int x, int y, int w, int h, uint32_t rgb, int a);
void hud_frame(int x, int y, int w, int h, int t, uint32_t rgb, int a);
void hud_panel(int x, int y, int w, int h, uint32_t bg, uint32_t border);
void hud_tape(int x, int y, int w, int h, float phase);
void hud_bar(int x, int y, int w, int h, float frac, uint32_t fill, uint32_t bg);
int  hud_text(int font, int x, int y, const char *s, uint32_t rgb, int a, int align);
int  hud_text_w(int font, const char *s);
int  hud_font_h(int font);
/* text wrapped to a width; returns the height used */
int  hud_text_wrap(int font, int x, int y, int w, const char *s, uint32_t rgb, int a, int lh);
void hud_circle(int cx, int cy, int r, uint32_t rgb, int a);
void hud_line(float x0, float y0, float x1, float y1, float t, uint32_t rgb, int a);
/* a chevron pointing along (dx, dy), centred on (x, y) */
void hud_chevron(float x, float y, float dx, float dy, float size, float t, uint32_t rgb, int a);
void hud_ring(int cx, int cy, int r, int t, float frac, uint32_t rgb, int a);
/* the HeatSync mark: two colours, untouched (sun disc, then flame and arrow). The only
 * caller in the cart is the title marquee. Never on a bot, never recoloured. */
void hud_logo(int x, int y, int size, uint32_t flame_rgb);
/* RGBA image of the mark (caller frees); kept for parity with DO-OCRACY, unused here */
uint8_t *hud_logo_image(int size, uint32_t flame_rgb, uint32_t bg_rgb, int bg_a);

/* -- FINAL-frontend 5 additions ----------------------------------- */
/* text with a 3 px solid shadow in C_SHADOW at 80 percent of a, then the text */
int  hud_text_sh(int font, int x, int y, const char *s, uint32_t rgb, int a, int align);
/* runs drawn left to right; total width measured with hud_text_w for align; shadowed */
int  hud_text_runs(int font, int x, int y, const run_t *runs, int a, int align);
/* global alpha multiplier applied inside blend(): a = a * mul / 255; default 255 */
void hud_set_alpha(int mul);
/* clip rect applied inside blend() and the hud_rect fast path; hud_clip_clear resets to the screen */
void hud_clip(int x, int y, int w, int h);
void hud_clip_clear(void);
/* a GANTRY eyebrow: tracked mono caps (a space between every character) */
void hud_eyebrow(int x, int y, const char *s, uint32_t c, int align);
/* a key cap: the button name in a box, sized to fit; returns its width */
int  hud_keycap(int x, int y, const char *k, uint32_t fill, uint32_t ink);
/* easing: t in 0..1 */
float hud_ease_out(float t);      /* 1 - (1-t)^3 */
float hud_ease_inout(float t);    /* t<.5 ? 4t^3 : 1 - (-2t+2)^3 / 2 */
float hud_ease_back(float t);     /* 1 + 2.70158*(t-1)^3 + 1.70158*(t-1)^2 (overshoots to about 1.1) */
/* exponential tween: cur += (target - cur) * (1 - exp(-speed * dt)); snaps when within 0.5 */
void  hud_tween(float *cur, float target, float speed, float dt);
/* 16x16 one-bit glyphs drawn at integer scale s (16*s px square) */
void hud_glyph(int x, int y, int id, int s, uint32_t rgb, int a);
/* a row of step dots: n dots, 10 px, pitch 22, centred on cx; dot k < step filled C_STEEL_HI,
   k == step is a 60 px bar filling by frac in C_HAZARD, k > step hollow 2 px C_STEEL */
void hud_steps(int cx, int y, int n, int step, float frac);
/* screen space particles (J_FX2_CAP), separate from the world fx in fx.c */
enum { FX2_SPARK, FX2_CONFETTI, FX2_RING };
void fx2_burst(int kind, float x, float y, uint32_t c);
void fx2_draw(float dt);

/* -- spec 10.3 additions for this cart ---------------------------- */
/* the 14 x 10 tag glyph with a notched corner: hazard fill when filled, char with a
 * 1 px steel frame when free, live frame for a bonus tag; rust when over is set */
void hud_tag(int x, int y, int filled, int bonus, int over);
/* one filled triangle, for the off-screen chevron and the control card arrows */
void hud_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgb, int a);
/* label left in F_H3, value right in F_MONO_L, 14 px chevrons either side of a choice value
 * (amber when a direction is available: can_left, can_right), the focus treatment is a hazard
 * block with tape ink text and a 6 px shadow (the caller tweens its y); disabled rows draw in smoke */
void menu_row(int x, int y, int w, const char *label, const char *value, int focused, int enabled, int is_choice, int can_left, int can_right);
/* a panel with a 24 px tape header and a step dot row at the bottom (10 px bone squares, the current one hazard) */
void card(int x, int y, int w, int h, int step, int total);
/* the dirty rect: the pixel writers above record what they touch, hud_clear erases only what the
 * previous frame painted and marks it, and main.c uploads only the dirty rows (glTexSubImage2D) or
 * nothing on a clean frame. hud_dirty is a no-op kept so the drawers compile: a hint wider than
 * what was written would only widen the upload. Reference space in and out. */
void hud_dirty(int x, int y, int w, int h);
int  hud_dirty_get(int *x, int *y, int *w, int *h);    /* the bounding box; 0 when no pixel changed since the last reset */
/* the changed pixels as bands of HUD_BAND buffer rows: on[b] with the columns [x0[b], x1[b]) of
 * band b (arrays of HUD_NBANDS); returns the number of bands on, 0 when nothing changed */
#define HUD_BAND 8
#define HUD_NBANDS ((HUD_H + HUD_BAND - 1) / HUD_BAND)
int  hud_dirty_bands(uint8_t *on, int16_t *x0, int16_t *x1);
/* a band set as up to max rects in buffer space (x0, y0 inclusive, x1, y1 exclusive, y top down):
 * runs of bands whose columns nearly agree; the last rect grows over the rest when the slots run
 * out; returns how many */
int  hud_band_rects(const uint8_t *on, const int16_t *bx0, const int16_t *bx1, int *x0, int *y0, int *x1, int *y1, int max);
void hud_dirty_reset(void);
/* the fixed seat colours in GANTRY order (2.1): hazard, live, bone, rust */
static const uint32_t SEAT_RGB[4] __attribute__((unused)) = { C_HAZARD, C_LIVE, C_BONE, C_RUST };
/* a seat chip: colour fill, the number in tape ink; ai draws the char hatch and the AI glyph */
void hud_seat_chip(int x, int y, int w, int h, int seat, int font, int ai);

#endif
