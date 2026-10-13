/* hudtest.c: the native HUD test for the menu screens. Builds hud.c and
 * draw_menus.c on the Mac (no three.c, no wasmcart host), drives every fake
 * menu scene (draw_menus.c under RTR_FAKE_MENUS) for a few frames and writes
 * each as a PNG with the HUD composited over a plain scene colour, so every
 * screen can be looked at without the cart. The holes the 3D turntables show
 * through read as the lighter scene colour.
 *
 *   ./build.sh hudtest            # build/hudtest, then build/shots/menus-<scene>.png
 *   build/hudtest build/shots [scene-name]
 *
 * hud.c is compiled with -D__wasm__ so wasmcart.h declares the asset imports
 * as externs; the three below satisfy them from assets/fonts. */
#include "hud.h"
#include "game.h"
#include "draw_menus.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

save_t SAVE;

static const char *FONT_DIR = "assets/fonts";

void wc_log(const char *ptr, unsigned len) { fprintf(stderr, "[cart] %.*s\n", (int)len, ptr); }

static FILE *open_asset(const char *path, unsigned len) {
  char p[512];
  snprintf(p, sizeof p, "%s/%.*s", FONT_DIR, (int)len, path);
  return fopen(p, "rb");
}
int wc_asset_size(const char *path, unsigned len) {
  FILE *f = open_asset(path, len);
  if (!f) return -1;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fclose(f);
  return (int)n;
}
int wc_load_asset(const char *path, unsigned len, void *dest, unsigned max) {
  FILE *f = open_asset(path, len);
  if (!f) return -1;
  size_t n = fread(dest, 1, max, f);
  fclose(f);
  return (int)n;
}

/* -- a stored-deflate PNG writer  -- */
static uint32_t crc_table[256];
static void crc_init(void) {
  for (uint32_t n = 0; n < 256; n++) {
    uint32_t c = n;
    for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
    crc_table[n] = c;
  }
}
static uint32_t crc32b(uint32_t c, const uint8_t *p, size_t n) {
  c = ~c;
  for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 255] ^ (c >> 8);
  return ~c;
}
static void put32(FILE *f, uint32_t v) { uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; fwrite(b, 1, 4, f); }
static void chunk(FILE *f, const char *type, const uint8_t *data, size_t n) {
  put32(f, (uint32_t)n);
  uint8_t t[4]; memcpy(t, type, 4);
  fwrite(t, 1, 4, f);
  if (n) fwrite(data, 1, n, f);
  uint32_t c = crc32b(0, t, 4);
  if (n) c = crc32b(c, data, n);
  put32(f, c);
}
static void write_png(const char *path, const uint8_t *rgba, int w, int h) {
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); return; }
  static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  fwrite(sig, 1, 8, f);
  uint8_t ihdr[13] = { 0 };
  ihdr[0] = (uint8_t)(w >> 24); ihdr[1] = (uint8_t)(w >> 16); ihdr[2] = (uint8_t)(w >> 8); ihdr[3] = (uint8_t)w;
  ihdr[4] = (uint8_t)(h >> 24); ihdr[5] = (uint8_t)(h >> 16); ihdr[6] = (uint8_t)(h >> 8); ihdr[7] = (uint8_t)h;
  ihdr[8] = 8; ihdr[9] = 6;
  chunk(f, "IHDR", ihdr, 13);
  /* raw scanlines with filter byte 0, in stored deflate blocks of at most 65535 bytes */
  size_t row = (size_t)w * 4 + 1, raw_n = row * (size_t)h;
  uint8_t *raw = malloc(raw_n);
  for (int y = 0; y < h; y++) { raw[y * row] = 0; memcpy(raw + y * row + 1, rgba + (size_t)y * w * 4, (size_t)w * 4); }
  size_t blocks = (raw_n + 65534) / 65535;
  size_t z_n = 2 + raw_n + blocks * 5 + 4;
  uint8_t *z = malloc(z_n);
  size_t o = 0;
  z[o++] = 0x78; z[o++] = 0x01;
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
  for (size_t off = 0; off < raw_n; off += 65535) {
    size_t n = raw_n - off > 65535 ? 65535 : raw_n - off;
    z[o++] = off + n >= raw_n ? 1 : 0;
    z[o++] = (uint8_t)n; z[o++] = (uint8_t)(n >> 8);
    z[o++] = (uint8_t)~n; z[o++] = (uint8_t)(~n >> 8);
    memcpy(z + o, raw + off, n); o += n;
  }
  uint32_t adler = (b << 16) | a;
  z[o++] = (uint8_t)(adler >> 24); z[o++] = (uint8_t)(adler >> 16); z[o++] = (uint8_t)(adler >> 8); z[o++] = (uint8_t)adler;
  chunk(f, "IDAT", z, o);
  chunk(f, "IEND", 0, 0);
  fclose(f);
  free(raw); free(z);
}

/* composite the HUD over the scene colour; the holes read as a lighter slab with a grid */
static void composite(uint8_t *out) {
  for (int y = 0; y < HUD_H; y++) for (int x = 0; x < HUD_W; x++) {
    const uint8_t *p = hud_px + ((HUD_H - 1 - y) * HUD_W + x) * 4;
    int grid = ((x / 8) + (y / 8)) & 1;
    uint32_t bg = grid ? 0x3a3027 : 0x342b23;
    int br = (bg >> 16) & 255, bgc = (bg >> 8) & 255, bb = bg & 255;
    int a = p[3];
    uint8_t *q = out + ((size_t)y * HUD_W + x) * 4;
    q[0] = (uint8_t)((p[0] * a + br * (255 - a)) / 255);
    q[1] = (uint8_t)((p[1] * a + bgc * (255 - a)) / 255);
    q[2] = (uint8_t)((p[2] * a + bb * (255 - a)) / 255);
    q[3] = 255;
  }
}

int main(int argc, char **argv) {
  const char *out_dir = argc > 1 ? argv[1] : "build/shots";
  const char *only = argc > 2 && argv[2][0] ? argv[2] : 0;
  const char *tag = argc > 3 && argv[3][0] == '2' ? "menus-half" : "menus";
  crc_init();
  hud_init();
  uint8_t *img = malloc((size_t)HUD_W * HUD_H * 4);
  int n = fake_menus_count();
  for (int i = 0; i < n; i++) {
    const char *name = fake_menus_setup(i);
    if (only && strcmp(only, name)) continue;
    /* 40 frames at 60 Hz so the tweens settle and the animations are mid flight */
    for (int f = 0; f < 40; f++) {
      hud_clear();
      hud_dirty_reset();
      fake_menus_draw(f / 60.0f + 0.66f, 1.0f / 60);
    }
    composite(img);
    char path[512];
    snprintf(path, sizeof path, "%s/%s-%s.png", out_dir, tag, name);
    write_png(path, img, HUD_W, HUD_H);
    printf("wrote %s\n", path);
  }
  free(img);
  return 0;
}
