/* hud.c: the CPU painted HUD layer. Copied from DO-OCRACY and extended per spec
 * 10.3 and FINAL-frontend 5. Every public coordinate is in 1280 x 720
 * reference space and divided by HUD_DIV here; blend() and the hud_rect fast
 * path are the only pixel writers, and both honour the clip rect and the
 * global alpha multiplier. Owned by the HUD agent (item 9). */
#include "hud.h"
#include "juice.h"
#include "wasmcart.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#include "logo_mesh.h"
#include "glyphs.h"

uint8_t *hud_px;

/* reference space to buffer space */
#define D(v)  ((v) / HUD_DIV)
#define DF(v) ((float)(v) / (float)HUD_DIV)

typedef struct {
  stbtt_bakedchar ch[96];
  uint8_t *atlas;
  int aw, ah, px, ascent;
} font_t;
static font_t fonts[F_COUNT];

static int clip_x0, clip_y0, clip_x1 = HUD_W, clip_y1 = HUD_H;   /* buffer space */
static int alpha_mul = 255;
static int dirty_on, dirty_x0, dirty_y0, dirty_x1, dirty_y1;      /* reference space */
static uint16_t glyph_rows[GL_COUNT][16];

static uint8_t *load_asset(const char *name, int *len) {
  int n = wc_asset_size(name, (unsigned)strlen(name));
  if (n <= 0) return NULL;
  uint8_t *b = malloc((size_t)n);
  if (wc_load_asset(name, (unsigned)strlen(name), b, (unsigned)n) != n) { free(b); return NULL; }
  if (len) *len = n;
  return b;
}

static void bake(int id, const uint8_t *ttf, int ref_px) {
  font_t *f = &fonts[id];
  int px = ref_px / HUD_DIV;
  f->px = px;
  f->aw = px >= 80 ? 2048 : 1024;
  f->ah = px >= 80 ? 1024 : px >= 40 ? 512 : 256;
  f->atlas = calloc((size_t)f->aw * f->ah, 1);
  stbtt_BakeFontBitmap(ttf, 0, (float)px, f->atlas, f->aw, f->ah, 32, 96, f->ch);
  stbtt_fontinfo fi;
  stbtt_InitFont(&fi, ttf, 0);
  int a, d, g;
  stbtt_GetFontVMetrics(&fi, &a, &d, &g);
  f->ascent = (int)(a * stbtt_ScaleForPixelHeight(&fi, (float)px) + 0.5f);
}

void hud_init(void) {
  hud_px = calloc(HUD_W * HUD_H, 4);
  uint8_t *archivo = load_asset("ArchivoBlack-Regular.ttf", NULL);
  uint8_t *plex = load_asset("IBMPlexMono-SemiBold.ttf", NULL);
  uint8_t *plexb = load_asset("IBMPlexMono-Bold.ttf", NULL);
  if (!archivo || !plex || !plexb) { WC_LOG("hud: missing font assets"); return; }
  bake(F_TITLE, archivo, 128);
  bake(F_H1, archivo, 64);
  bake(F_H2, archivo, 40);
  bake(F_H3, archivo, 26);
  bake(F_MONO_L, plexb, 30);
  bake(F_MONO, plex, 21);
  bake(F_MONO_S, plexb, 15);
  bake(F_STINGER, archivo, 56);
  bake(F_STINGER_Q, archivo, 36);
  free(archivo); free(plex); free(plexb);
  for (int g = 0; g < GL_COUNT; g++) for (int r = 0; r < 16; r++) {
    uint16_t bits = 0;
    const char *row = GLYPH_SRC[g][r];
    for (int c = 0; c < 16 && row[c]; c++) if (row[c] == '#') bits |= (uint16_t)(1u << c);
    glyph_rows[g][r] = bits;
  }
  hud_clip_clear();
}

void hud_clear(void) { memset(hud_px, 0, HUD_W * HUD_H * 4); }

/* -- pixel writers ------------------------------------------------ */

static inline void blend(int x, int y, uint32_t rgb, int a) {
  if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
  a = a * alpha_mul / 255;
  if (a <= 0) return;
  uint8_t *p = hud_px + ((HUD_H - 1 - y) * HUD_W + x) * 4;
  int sr = (rgb >> 16) & 255, sg = (rgb >> 8) & 255, sb = rgb & 255;
  if (a >= 255 || p[3] == 0) { p[0] = sr; p[1] = sg; p[2] = sb; p[3] = a > 255 ? 255 : a; return; }
  int da = p[3] * (255 - a) / 255;
  int oa = a + da;
  p[0] = (sr * a + p[0] * da) / oa;
  p[1] = (sg * a + p[1] * da) / oa;
  p[2] = (sb * a + p[2] * da) / oa;
  p[3] = oa;
}

/* buffer space rect */
static void rect_b(int x, int y, int w, int h, uint32_t rgb, int a) {
  int x0 = x < clip_x0 ? clip_x0 : x, y0 = y < clip_y0 ? clip_y0 : y;
  int x1 = x + w > clip_x1 ? clip_x1 : x + w, y1 = y + h > clip_y1 ? clip_y1 : y + h;
  if (x0 >= x1 || y0 >= y1) return;
  a = a * alpha_mul / 255;
  if (a <= 0) return;
  if (a >= 255) {
    uint8_t c[4] = { (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255 };
    uint32_t v; memcpy(&v, c, 4);
    for (int yy = y0; yy < y1; yy++) {
      uint32_t *row = (uint32_t *)(hud_px + ((HUD_H - 1 - yy) * HUD_W) * 4);
      for (int xx = x0; xx < x1; xx++) row[xx] = v;
    }
    return;
  }
  int keep = alpha_mul; alpha_mul = 255;   /* already applied */
  for (int yy = y0; yy < y1; yy++) for (int xx = x0; xx < x1; xx++) blend(xx, yy, rgb, a);
  alpha_mul = keep;
}

void hud_rect(int x, int y, int w, int h, uint32_t rgb, int a) {
  int x0 = D(x), y0 = D(y), x1 = D(x + w), y1 = D(y + h);
  rect_b(x0, y0, x1 - x0, y1 - y0, rgb, a);
}

void hud_frame(int x, int y, int w, int h, int t, uint32_t rgb, int a) {
  hud_rect(x, y, w, t, rgb, a);
  hud_rect(x, y + h - t, w, t, rgb, a);
  hud_rect(x, y + t, t, h - 2 * t, rgb, a);
  hud_rect(x + w - t, y + t, t, h - 2 * t, rgb, a);
}

/* GANTRY card: 2px rule, solid 6px offset shadow, square corners */
void hud_panel(int x, int y, int w, int h, uint32_t bg, uint32_t border) {
  hud_rect(x + 6, y + 6, w, h, C_SHADOW, 255);
  hud_rect(x, y, w, h, bg, 255);
  hud_frame(x, y, w, h, 2, border, 255);
}

/* repeating-linear-gradient(-45deg, hazard 0 24px, tape-dark 24px 48px) */
void hud_tape(int x, int y, int w, int h, float phase) {
  int ph = (int)DF(phase);
  int bx = D(x), by = D(y), bw = D(x + w) - bx, bh = D(y + h) - by;
  const int period = 48 / HUD_DIV, half = 24 / HUD_DIV;
  for (int yy = by; yy < by + bh; yy++) {
    if (yy < clip_y0 || yy >= clip_y1) continue;
    for (int xx = bx; xx < bx + bw; xx++) {
      int k = ((xx + yy + ph) % period + period) % period;
      blend(xx, yy, k < half ? C_HAZARD : C_TAPE, 255);
    }
  }
  /* a tape thinner than 12 px (the seams, the bracket chains) is all stripe; the frame would swallow it */
  if (w >= 12 && h >= 12) hud_frame(x, y, w, h, 2, C_TAPE, 255);
}

void hud_bar(int x, int y, int w, int h, float frac, uint32_t fill, uint32_t bg) {
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  hud_rect(x, y, w, h, bg, 255);
  hud_rect(x + 2, y + 2, (int)((w - 4) * frac), h - 4, fill, 255);
}

/* -- text --------------------------------------------------------- */

int hud_font_h(int font) { return fonts[font].px * HUD_DIV; }

static float text_w_b(int font, const char *s) {
  font_t *f = &fonts[font];
  if (!f->atlas) return 0;
  float w = 0;
  for (; *s; s++) {
    int c = (unsigned char)*s;
    if (c < 32 || c > 127) c = '?';
    w += f->ch[c - 32].xadvance;
  }
  return w;
}

int hud_text_w(int font, const char *s) { return (int)(text_w_b(font, s) * HUD_DIV + 0.5f); }

/* y is the top of the line box (cap height starts about a third down) */
int hud_text(int font, int x, int y, const char *s, uint32_t rgb, int a, int align) {
  font_t *f = &fonts[font];
  if (!f->atlas) return 0;
  float wb = text_w_b(font, s);
  int w = (int)(wb * HUD_DIV + 0.5f);
  float cx = DF(x) - (align == AL_CENTER ? wb * 0.5f : align == AL_RIGHT ? wb : 0);
  float cy = DF(y) + (float)f->ascent;
  for (; *s; s++) {
    int c = (unsigned char)*s;
    if (c < 32 || c > 127) c = '?';
    stbtt_bakedchar *b = &f->ch[c - 32];
    int gx = (int)floorf(cx + b->xoff + 0.5f), gy = (int)floorf(cy + b->yoff + 0.5f);
    for (int yy = b->y0; yy < b->y1; yy++) {
      const uint8_t *row = f->atlas + yy * f->aw;
      int py = gy + yy - b->y0;
      if (py < clip_y0 || py >= clip_y1) continue;
      for (int xx = b->x0; xx < b->x1; xx++) {
        int cov = row[xx];
        if (cov) blend(gx + xx - b->x0, py, rgb, cov * a / 255);
      }
    }
    cx += b->xadvance;
  }
  return w;
}

int hud_text_wrap(int font, int x, int y, int w, const char *s, uint32_t rgb, int a, int lh) {
  char line[256], word[128];
  int ln = 0, used = 0;
  line[0] = 0;
  while (*s) {
    int wn = 0;
    while (*s == ' ') s++;
    while (*s && *s != ' ' && wn < 126) word[wn++] = *s++;
    word[wn] = 0;
    if (!wn) break;
    char trial[384];
    snprintf(trial, sizeof trial, ln ? "%s %s" : "%s%s", line, word);
    if (ln && hud_text_w(font, trial) > w) {
      hud_text(font, x, y + used, line, rgb, a, AL_LEFT);
      used += lh;
      snprintf(line, sizeof line, "%s", word);
    } else {
      snprintf(line, sizeof line, "%s", trial);
    }
    ln = (int)strlen(line);
  }
  if (ln) { hud_text(font, x, y + used, line, rgb, a, AL_LEFT); used += lh; }
  return used;
}

/* text with a 3 px solid shadow in C_SHADOW at 80 percent of a, then the text */
int hud_text_sh(int font, int x, int y, const char *s, uint32_t rgb, int a, int align) {
  hud_text(font, x + 3, y + 3, s, C_SHADOW, a * 4 / 5, align);
  return hud_text(font, x, y, s, rgb, a, align);
}

int hud_text_runs(int font, int x, int y, const run_t *runs, int a, int align) {
  int total = 0;
  for (const run_t *r = runs; r->s; r++) total += hud_text_w(font, r->s);
  int cx = align == AL_CENTER ? x - total / 2 : align == AL_RIGHT ? x - total : x;
  for (const run_t *r = runs; r->s; r++) cx += hud_text_sh(font, cx, y, r->s, r->c, a, AL_LEFT);
  return total;
}

void hud_set_alpha(int mul) { alpha_mul = mul < 0 ? 0 : mul > 255 ? 255 : mul; }

void hud_clip(int x, int y, int w, int h) {
  clip_x0 = D(x) < 0 ? 0 : D(x);
  clip_y0 = D(y) < 0 ? 0 : D(y);
  clip_x1 = D(x + w) > HUD_W ? HUD_W : D(x + w);
  clip_y1 = D(y + h) > HUD_H ? HUD_H : D(y + h);
}
void hud_clip_clear(void) { clip_x0 = 0; clip_y0 = 0; clip_x1 = HUD_W; clip_y1 = HUD_H; }

/* a GANTRY eyebrow: tracked mono caps */
void hud_eyebrow(int x, int y, const char *s, uint32_t c, int align) {
  char buf[160];
  int n = 0;
  for (const char *p = s; *p && n < 150; p++) { buf[n++] = *p; if (p[1]) buf[n++] = ' '; }
  buf[n] = 0;
  hud_text(F_MONO_S, x, y, buf, c, 255, align);
}

/* a key cap: the button name in a box, sized to fit */
int hud_keycap(int x, int y, const char *k, uint32_t fill, uint32_t ink) {
  int w = hud_text_w(F_MONO_S, k) + 14;
  if (w < 24) w = 24;
  hud_rect(x + 2, y + 2, w, 24, C_SHADOW, 255);
  hud_rect(x, y, w, 24, fill, 255);
  hud_text(F_MONO_S, x + w / 2, y + 3, k, ink, 255, AL_CENTER);
  return w;
}

/* -- easing ------------------------------------------------------- */

float hud_ease_out(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; float u = 1 - t; return 1 - u * u * u; }
float hud_ease_inout(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; if (t < 0.5f) return 4 * t * t * t; float u = -2 * t + 2; return 1 - u * u * u / 2; }
float hud_ease_back(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; float u = t - 1; return 1 + 2.70158f * u * u * u + 1.70158f * u * u; }
void hud_tween(float *cur, float target, float speed, float dt) {
  *cur += (target - *cur) * (1 - expf(-speed * dt));
  if (fabsf(target - *cur) < 0.5f) *cur = target;
}

/* -- shapes ------------------------------------------------------- */

void hud_circle(int cx, int cy, int r, uint32_t rgb, int a) {
  int bx = D(cx), by = D(cy), br = D(r);
  for (int y = -br - 1; y <= br + 1; y++) for (int x = -br - 1; x <= br + 1; x++) {
    float d = sqrtf((float)(x * x + y * y)) - br;
    if (d > 0.5f) continue;
    float cov = d < -0.5f ? 1.f : 0.5f - d;
    blend(bx + x, by + y, rgb, (int)(a * cov));
  }
}

/* anti-aliased thick line: distance to the segment, per pixel in its box */
void hud_line(float x0, float y0, float x1, float y1, float t, uint32_t rgb, int a) {
  x0 = DF(x0); y0 = DF(y0); x1 = DF(x1); y1 = DF(y1); t = DF(t);
  float r = t * 0.5f;
  int bx0 = (int)floorf(fminf(x0, x1) - r - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + r + 1);
  int by0 = (int)floorf(fminf(y0, y1) - r - 1), by1 = (int)ceilf(fmaxf(y0, y1) + r + 1);
  if (bx0 < clip_x0) bx0 = clip_x0;
  if (by0 < clip_y0) by0 = clip_y0;
  if (bx1 > clip_x1 - 1) bx1 = clip_x1 - 1;
  if (by1 > clip_y1 - 1) by1 = clip_y1 - 1;
  float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
  for (int y = by0; y <= by1; y++) for (int x = bx0; x <= bx1; x++) {
    float px = x + 0.5f - x0, py = y + 0.5f - y0;
    float u = l2 > 0 ? (px * dx + py * dy) / l2 : 0;
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    float ex = px - u * dx, ey = py - u * dy;
    float d = sqrtf(ex * ex + ey * ey) - r;
    if (d > 0.5f) continue;
    float cov = d < -0.5f ? 1.f : 0.5f - d;
    blend(x, y, rgb, (int)(a * cov));
  }
}

void hud_chevron(float x, float y, float dx, float dy, float size, float t, uint32_t rgb, int a) {
  float l = sqrtf(dx * dx + dy * dy);
  if (l < 1e-4f) return;
  dx /= l; dy /= l;
  float tipx = x + dx * size * 0.5f, tipy = y + dy * size * 0.5f;
  float bx = x - dx * size * 0.5f, by = y - dy * size * 0.5f;
  float nx = -dy * size * 0.6f, ny = dx * size * 0.6f;
  hud_line(bx + nx, by + ny, tipx, tipy, t, rgb, a);
  hud_line(bx - nx, by - ny, tipx, tipy, t, rgb, a);
}

void hud_ring(int cx, int cy, int r, int t, float frac, uint32_t rgb, int a) {
  int bx = D(cx), by = D(cy), br = D(r), bt = D(t) < 1 ? 1 : D(t);
  float lim = frac * 6.2831853f;
  for (int y = -br - 1; y <= br + 1; y++) for (int x = -br - 1; x <= br + 1; x++) {
    float d = sqrtf((float)(x * x + y * y));
    if (d > br + 0.5f || d < br - bt - 0.5f) continue;
    float ang = atan2f((float)x, (float)-y);
    if (ang < 0) ang += 6.2831853f;
    if (ang > lim) continue;
    float cov = 1.f;
    if (d > br - 0.5f) cov = br + 0.5f - d;
    else if (d < br - bt + 0.5f) cov = d - (br - bt - 0.5f);
    blend(bx + x, by + y, rgb, (int)(a * cov));
  }
}

/* one filled triangle, 2 x 2 supersampled edges */
void hud_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgb, int a) {
  x0 = DF(x0); y0 = DF(y0); x1 = DF(x1); y1 = DF(y1); x2 = DF(x2); y2 = DF(y2);
  int bx0 = (int)floorf(fminf(x0, fminf(x1, x2))), bx1 = (int)ceilf(fmaxf(x0, fmaxf(x1, x2)));
  int by0 = (int)floorf(fminf(y0, fminf(y1, y2))), by1 = (int)ceilf(fmaxf(y0, fmaxf(y1, y2)));
  if (bx0 < clip_x0) bx0 = clip_x0;
  if (by0 < clip_y0) by0 = clip_y0;
  if (bx1 > clip_x1 - 1) bx1 = clip_x1 - 1;
  if (by1 > clip_y1 - 1) by1 = clip_y1 - 1;
  float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
  if (fabsf(area) < 1e-6f) return;
  float sgn = area > 0 ? 1.f : -1.f;
  static const float sub[4][2] = { { 0.25f, 0.25f }, { 0.75f, 0.25f }, { 0.25f, 0.75f }, { 0.75f, 0.75f } };
  for (int y = by0; y <= by1; y++) for (int x = bx0; x <= bx1; x++) {
    int in = 0;
    for (int s = 0; s < 4; s++) {
      float px = x + sub[s][0], py = y + sub[s][1];
      float e0 = ((x1 - x0) * (py - y0) - (px - x0) * (y1 - y0)) * sgn;
      float e1 = ((x2 - x1) * (py - y1) - (px - x1) * (y2 - y1)) * sgn;
      float e2 = ((x0 - x2) * (py - y2) - (px - x2) * (y0 - y2)) * sgn;
      if (e0 >= 0 && e1 >= 0 && e2 >= 0) in++;
    }
    if (in) blend(x, y, rgb, a * in / 4);
  }
}

/* 16x16 one-bit glyphs drawn at integer scale s */
void hud_glyph(int x, int y, int id, int s, uint32_t rgb, int a) {
  if (id < 0 || id >= GL_COUNT) return;
  if (s < 1) s = 1;
  for (int r = 0; r < 16; r++) {
    uint16_t bits = glyph_rows[id][r];
    if (!bits) continue;
    for (int c = 0; c < 16; c++) if (bits & (1u << c)) {
      if (s == 1 && HUD_DIV == 1) blend(x + c, y + r, rgb, a);
      else hud_rect(x + c * s, y + r * s, s, s, rgb, a);
    }
  }
}

/* a row of step dots: n dots, 10 px, pitch 22, centred on cx; dot k < step filled C_STEEL_HI,
   k == step is a 60 px bar filling by frac in C_HAZARD, k > step hollow 2 px C_STEEL */
void hud_steps(int cx, int y, int n, int step, float frac) {
  int total = (n - 1) * 22 + 10 + 50;
  int x = cx - total / 2;
  for (int k = 0; k < n; k++) {
    if (k < step) { hud_rect(x, y, 10, 10, C_STEEL_HI, 255); x += 22; }
    else if (k == step) { hud_rect(x, y, 60, 10, C_STEEL, 255); hud_rect(x, y, (int)(60 * (frac < 0 ? 0 : frac > 1 ? 1 : frac)), 10, C_HAZARD, 255); x += 72; }
    else { hud_frame(x, y, 10, 10, 2, C_STEEL, 255); x += 22; }
  }
}

/* -- the HeatSync mark -------------------------------------------- */

/* even-odd scanline fill into a coverage grid (ss x ss supersampled) */
static void fill_rings(uint8_t *cov, int size, const float *pts, const int *ring, int nrings) {
  const int SS = 4;
  int gs = size * SS;
  float scale = (float)gs / 1000.f;
  float *xs = malloc(sizeof(float) * 4096);
  for (int sy = 0; sy < gs; sy++) {
    float y = (sy + 0.5f) / scale;
    int n = 0;
    for (int r = 0; r < nrings; r++) {
      int a0 = ring[r], a1 = ring[r + 1];
      for (int i = a0; i < a1; i++) {
        int j = i + 1 < a1 ? i + 1 : a0;
        float x0 = pts[i * 2], y0 = pts[i * 2 + 1], x1 = pts[j * 2], y1 = pts[j * 2 + 1];
        if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
          if (n < 4096) xs[n++] = x0 + (y - y0) / (y1 - y0) * (x1 - x0);
        }
      }
    }
    for (int i = 1; i < n; i++) { float v = xs[i]; int k = i - 1; while (k >= 0 && xs[k] > v) { xs[k + 1] = xs[k]; k--; } xs[k + 1] = v; }
    for (int i = 0; i + 1 < n; i += 2) {
      int xa = (int)(xs[i] * scale + 0.5f), xb = (int)(xs[i + 1] * scale + 0.5f);
      if (xa < 0) xa = 0;
      if (xb > gs) xb = gs;
      for (int sx = xa; sx < xb; sx++) cov[(sy / SS) * size + sx / SS]++;
    }
  }
  free(xs);
}

typedef struct { int size; uint32_t flame; uint8_t *sun, *ink; } logo_cache_t;
static logo_cache_t lcache[6];

static logo_cache_t *logo_get(int size, uint32_t flame) {
  for (int i = 0; i < 6; i++) if (lcache[i].sun && lcache[i].size == size && lcache[i].flame == flame) return &lcache[i];
  int slot = 0;
  for (int i = 0; i < 6; i++) if (!lcache[i].sun) { slot = i; break; }
  logo_cache_t *c = &lcache[slot];
  free(c->sun); free(c->ink);
  c->size = size; c->flame = flame;
  c->sun = calloc((size_t)size * size, 1);
  c->ink = calloc((size_t)size * size, 1);
  fill_rings(c->sun, size, logo_sun_pts, logo_sun_ring, LOGO_SUN_RINGS);
  fill_rings(c->ink, size, logo_flame_pts, logo_flame_ring, LOGO_FLAME_RINGS);
  return c;
}

void hud_logo(int x, int y, int size, uint32_t flame_rgb) {
  int bs = D(size), bx = D(x), by = D(y);
  logo_cache_t *c = logo_get(bs, flame_rgb);
  for (int yy = 0; yy < bs; yy++) for (int xx = 0; xx < bs; xx++) {
    int i = yy * bs + xx;
    if (c->sun[i]) blend(bx + xx, by + yy, C_LOGO_SUN, c->sun[i] * 255 / 16);
    if (c->ink[i]) blend(bx + xx, by + yy, flame_rgb, c->ink[i] * 255 / 16);
  }
}

uint8_t *hud_logo_image(int size, uint32_t flame_rgb, uint32_t bg_rgb, int bg_a) {
  logo_cache_t *c = logo_get(size, flame_rgb);
  uint8_t *img = malloc((size_t)size * size * 4);
  for (int i = 0; i < size * size; i++) {
    float r = ((bg_rgb >> 16) & 255) / 255.f, g = ((bg_rgb >> 8) & 255) / 255.f, b = (bg_rgb & 255) / 255.f;
    float a = bg_a / 255.f;
    float s = c->sun[i] / 16.f, k = c->ink[i] / 16.f;
    r = r * (1 - s) + (C_LOGO_SUN >> 16 & 255) / 255.f * s; g = g * (1 - s) + (C_LOGO_SUN >> 8 & 255) / 255.f * s;
    b = b * (1 - s) + (C_LOGO_SUN & 255) / 255.f * s; a = a + s * (1 - a);
    r = r * (1 - k) + (flame_rgb >> 16 & 255) / 255.f * k; g = g * (1 - k) + (flame_rgb >> 8 & 255) / 255.f * k;
    b = b * (1 - k) + (flame_rgb & 255) / 255.f * k; a = a + k * (1 - a);
    img[i * 4] = (uint8_t)(r * 255); img[i * 4 + 1] = (uint8_t)(g * 255);
    img[i * 4 + 2] = (uint8_t)(b * 255); img[i * 4 + 3] = (uint8_t)(a * 255);
  }
  return img;
}

/* -- screen space particles --------------------------------------- */

typedef struct { float x, y, vx, vy, life, max, size; uint32_t c; int kind; } fx2_t;
static fx2_t FX2[J_FX2_CAP];
static int fx2_next;
static uint32_t fx2_rng = 0x2545f491u;
static float fx2_rand(void) { fx2_rng ^= fx2_rng << 13; fx2_rng ^= fx2_rng >> 17; fx2_rng ^= fx2_rng << 5; return (float)(fx2_rng >> 8) / 16777216.f; }

static void fx2_add(int kind, float x, float y, float vx, float vy, float life, float size, uint32_t c) {
  fx2_t *p = &FX2[fx2_next++ % J_FX2_CAP];
  p->kind = kind; p->x = x; p->y = y; p->vx = vx; p->vy = vy; p->life = p->max = life; p->size = size; p->c = c;
}

void fx2_burst(int kind, float x, float y, uint32_t c) {
  static const uint32_t pal[6] = { C_HAZARD, C_LIVE, C_BONE, C_HAZARD, C_RUST, C_BONE };   /* GANTRY only */
  switch (kind) {
    case FX2_SPARK:
      for (int i = 0; i < 12; i++) {
        float ang = fx2_rand() * 6.2831853f, sp = 180 + fx2_rand() * 240;
        fx2_add(kind, x, y, cosf(ang) * sp, sinf(ang) * sp, 0.35f + fx2_rand() * 0.15f, 3, c);
      }
      break;
    case FX2_CONFETTI:
      for (int i = 0; i < 24; i++) {
        uint32_t col = c ? (i % 2 ? c : pal[i % 6]) : pal[i % 6];
        fx2_add(kind, x + (fx2_rand() - 0.5f) * 400, y, (fx2_rand() - 0.5f) * 120, 80 + fx2_rand() * 140, 2.0f + fx2_rand() * 0.6f, 6, col);
      }
      break;
    case FX2_RING:
      for (int i = 0; i < 16; i++) {
        float ang = i * 6.2831853f / 16;
        fx2_add(kind, x, y, cosf(ang) * 260, sinf(ang) * 260, 0.4f, 4, c);
      }
      break;
    default: break;
  }
}

void fx2_draw(float dt) {
  for (int i = 0; i < J_FX2_CAP; i++) {
    fx2_t *p = &FX2[i];
    if (p->life <= 0) continue;
    p->life -= dt;
    if (p->kind == FX2_SPARK) p->vy += 900 * dt;
    if (p->kind == FX2_CONFETTI) { p->vy += 300 * dt; p->vx *= 1 - 1.5f * dt; p->vy *= 1 - 1.5f * dt; }
    p->x += p->vx * dt; p->y += p->vy * dt;
    float f = p->life / p->max;
    int a = f < 0.3f ? (int)(255 * f / 0.3f) : 255;
    if (p->kind == FX2_CONFETTI) {
      int w = ((int)(p->life * 12) & 1) ? 6 : 3;
      hud_rect((int)p->x, (int)p->y, w, 3, p->c, a);
    } else {
      hud_rect((int)p->x, (int)p->y, (int)p->size, (int)p->size, p->c, a);
    }
    hud_dirty((int)p->x - 4, (int)p->y - 4, 16, 16);
  }
}

/* -- spec 10.3 additions ----------------------------------------- */

/* the 14 x 10 tag glyph with a notched corner */
void hud_tag(int x, int y, int filled, int bonus, int over) {
  uint32_t fill = filled ? (over ? C_RUST : C_HAZARD) : C_CHAR;
  uint32_t frame = bonus ? C_LIVE : C_STEEL;
  /* the body, minus a 3 px corner notch at the top left */
  hud_rect(x + 3, y, 11, 10, fill, 255);
  hud_rect(x, y + 3, 3, 7, fill, 255);
  if (!filled) {
    hud_rect(x + 3, y, 11, 1, frame, 255);
    hud_rect(x, y + 9, 14, 1, frame, 255);
    hud_rect(x + 13, y, 1, 10, frame, 255);
    hud_rect(x, y + 3, 1, 7, frame, 255);
    hud_rect(x + 1, y + 2, 1, 1, frame, 255);
    hud_rect(x + 2, y + 1, 1, 1, frame, 255);
  } else if (bonus) {
    hud_rect(x + 3, y, 11, 1, frame, 255);
    hud_rect(x, y + 9, 14, 1, frame, 255);
  }
  /* the string hole */
  hud_rect(x + 10, y + 4, 2, 2, filled ? C_TAPE : C_STEEL, 255);
}

void menu_row(int x, int y, int w, const char *label, const char *value, int focused, int enabled, int is_choice, int can_left, int can_right) {
  const int h = 40;
  uint32_t ink = enabled ? C_BONE : C_SMOKE;
  uint32_t val = enabled ? C_BONE : C_SMOKE;
  if (focused) {
    hud_rect(x + 6, y + 6, w, h, C_SHADOW, 255);
    hud_rect(x, y, w, h, enabled ? C_HAZARD : C_STEEL_HI, 255);
    ink = enabled ? C_TAPE : C_SMOKE;
    val = ink;
  }
  hud_text(F_H3, x + 16, y + 7, label, ink, 255, AL_LEFT);
  if (value && *value) {
    int vw = hud_text_w(F_MONO_L, value);
    int vx = x + w - 16;
    if (is_choice) {
      uint32_t cr = enabled && can_right ? (focused ? C_TAPE : C_HAZARD) : C_STEEL;
      uint32_t cl = enabled && can_left ? (focused ? C_TAPE : C_HAZARD) : C_STEEL;
      hud_chevron(vx - 7, y + h / 2, 1, 0, 14, 3, cr, 255);
      vx -= 24;
      hud_text(F_MONO_L, vx, y + 5, value, val, 255, AL_RIGHT);
      hud_chevron(vx - vw - 17, y + h / 2, -1, 0, 14, 3, cl, 255);
    } else {
      hud_text(F_MONO_L, vx, y + 5, value, val, 255, AL_RIGHT);
    }
  }
  hud_dirty(x, y, w + 6, h + 6);
}

void card(int x, int y, int w, int h, int step, int total) {
  hud_panel(x, y, w, h, C_SLAB, C_STEEL_HI);
  hud_tape(x + 2, y + 2, w - 4, 24, 0);
  if (total > 0) {
    int pitch = 22, tw = total * pitch - (pitch - 10);
    int sx = x + w / 2 - tw / 2, sy = y + h - 22;
    for (int k = 0; k < total; k++) hud_rect(sx + k * pitch, sy, 10, 10, k == step ? C_HAZARD : C_BONE, 255);
  }
  hud_dirty(x, y, w + 6, h + 6);
}

void hud_dirty(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) return;
  if (!dirty_on) { dirty_on = 1; dirty_x0 = x; dirty_y0 = y; dirty_x1 = x + w; dirty_y1 = y + h; return; }
  if (x < dirty_x0) dirty_x0 = x;
  if (y < dirty_y0) dirty_y0 = y;
  if (x + w > dirty_x1) dirty_x1 = x + w;
  if (y + h > dirty_y1) dirty_y1 = y + h;
}
int hud_dirty_get(int *x, int *y, int *w, int *h) {
  if (!dirty_on) return 0;
  if (x) *x = dirty_x0;
  if (y) *y = dirty_y0;
  if (w) *w = dirty_x1 - dirty_x0;
  if (h) *h = dirty_y1 - dirty_y0;
  return 1;
}
void hud_dirty_reset(void) { dirty_on = 0; }

void hud_seat_chip(int x, int y, int w, int h, int seat, int font, int ai) {
  uint32_t c = SEAT_RGB[seat & 3];
  hud_rect(x, y, w, h, c, 255);
  if (ai) {
    /* a char hatch over the chip (clipped to it), then the AI glyph in the corner */
    int cx0 = clip_x0, cy0 = clip_y0, cx1 = clip_x1, cy1 = clip_y1;
    hud_clip(x, y, w, h);
    for (int k = -h; k < w; k += 4) hud_line(x + k, y, x + k + h, y + h, 1, C_CHAR, 140);
    clip_x0 = cx0; clip_y0 = cy0; clip_x1 = cx1; clip_y1 = cy1;
    hud_glyph(x + w - 18, y + 2, GL_AI, 1, C_TAPE, 255);
  }
  char n[4];
  snprintf(n, sizeof n, "%d", (seat & 3) + 1);
  int fh = hud_font_h(font);
  hud_text(font, x + w / 2, y + (h - fh) / 2 - fh / 8, n, C_TAPE, 255, AL_CENTER);
}
