/* keys.c: the key name layer (keys.h, docs/design/deep-controls.md 4.1). Every
 * keycap and key hint in draw.c and draw_menus.c asks here what to print. */
#include "keys.h"
#include "hud.h"
#include "game.h"
#include "wasmcart.h"
#include <stdio.h>
#include <string.h>

static uint8_t kind_of[4] = { KIND_UNKNOWN, KIND_UNKNOWN, KIND_UNKNOWN, KIND_UNKNOWN };
static int polled;

/* KIND_PAD: the W3C pad's own letters */
static const char *const NAME_PAD[KN_COUNT] = { "A", "B", "X", "Y", "L", "R", "START", "SELECT", "D PAD", "STICK", "RT", "LT" };
/* KIND_KEYBOARD: the browser and RetroArch defaults (web/index.html); a keyboard has no triggers, so RT and LT
 * are the weapon and boost keys */
static const char *const NAME_KEY[KN_COUNT] = { "Z", "X", "A", "S", "Q", "W", "ENTER", "SHIFT", "ARROWS", "ARROWS", "Z", "Q" };
/* KIND_UNKNOWN: both, keyboard first (on the station the keyboard is pad 0 and a pad player knows their letters) */
static const char *const NAME_BOTH[KN_COUNT] = { "Z OR A", "X OR B", "A OR X", "S OR Y", "Q OR L", "W OR R", "ENTER OR START",
                                                 "SHIFT OR SELECT", "ARROWS OR D PAD", "ARROWS OR STICK", "Z OR RT", "Q OR LT" };

static int starts_keyboard(const char *s) {
  static const char k[] = "KEYBOARD";
  for (int i = 0; k[i]; i++) {
    char c = s[i];
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c != k[i]) return 0;
  }
  return 1;
}

void keys_poll(void) {
  for (int i = 0; i < 4; i++) {
    char buf[32];
    memset(buf, 0, sizeof buf);
    int n = wc_pad_name((unsigned)i, buf, (unsigned)sizeof buf - 1);
    if (n <= 0 || !buf[0]) kind_of[i] = KIND_UNKNOWN;
    else kind_of[i] = starts_keyboard(buf) ? KIND_KEYBOARD : KIND_PAD;
  }
  polled = 1;
}

int keys_kind(int pad) {
  if (pad < 0 || pad > 3) return KIND_UNKNOWN;
  if (!polled) keys_poll();
  return kind_of[pad];
}

static int which_ok(int which) { return which >= 0 && which < KN_COUNT; }

const char *key_name(int pad, int which) {
  if (!which_ok(which)) return "";
  switch (keys_kind(pad)) {
    case KIND_PAD: return NAME_PAD[which];
    case KIND_KEYBOARD: return NAME_KEY[which];
    default: return NAME_BOTH[which];
  }
}

int key_seat_pad(int seat) {
  if (seat < 0 || seat > 3 || G.seats[seat].kind != SEAT_HUMAN) return -1;
  return G.seats[seat].pad;
}

/* -- caps ---------------------------------------------------------- */
static int cap_w(const char *k, int small) {
  int w = hud_text_w(F_MONO_S, k) + (small ? 8 : 14);
  int min = small ? 16 : 24;
  return w < min ? min : w;
}

static int one_cap(int x, int y, const char *k, int small, uint32_t fill, uint32_t ink) {
  if (!small) return hud_keycap(x, y, k, fill, ink);
  int w = cap_w(k, 1);
  hud_rect(x + 1, y + 1, w, 16, C_SHADOW, 255);
  hud_rect(x, y, w, 16, fill, 255);
  hud_text(F_MONO_S, x + w / 2, y - 1, k, ink, 255, AL_CENTER);
  return w;
}

#define SLASH_W 13   /* the "/" between the two caps of a pad that named nothing */

static int caps_w(int pad, int which, int small) {
  if (!which_ok(which)) return 0;
  if (keys_kind(pad) == KIND_UNKNOWN) return cap_w(NAME_KEY[which], small) + SLASH_W + cap_w(NAME_PAD[which], small);
  return cap_w(key_name(pad, which), small);
}

static int caps(int x, int y, int pad, int which, int small, uint32_t fill, uint32_t ink) {
  if (!which_ok(which)) return 0;
  if (keys_kind(pad) != KIND_UNKNOWN) return one_cap(x, y, key_name(pad, which), small, fill, ink);
  int w = one_cap(x, y, NAME_KEY[which], small, fill, ink);
  hud_text(F_MONO_S, x + w + SLASH_W / 2, small ? y - 1 : y + 3, "/", C_ASH, 255, AL_CENTER);
  w += SLASH_W;
  return w + one_cap(x + w, y, NAME_PAD[which], small, fill, ink);
}

int key_cap(int x, int y, int pad, int which, uint32_t fill, uint32_t ink) { return caps(x, y, pad, which, 0, fill, ink); }
int key_cap_seat(int x, int y, int seat, int which, uint32_t fill, uint32_t ink) { return caps(x, y, key_seat_pad(seat), which, 0, fill, ink); }
int key_cap_w(int pad, int which) { return caps_w(pad, which, 0); }
int key_cap_small(int x, int y, int pad, int which, uint32_t fill, uint32_t ink) { return caps(x, y, pad, which, 1, fill, ink); }
int key_cap_small_w(int pad, int which) { return caps_w(pad, which, 1); }

int key_hint_pad(int x, int y, int pad, int which, const char *label, uint32_t ink) {
  int w = key_cap(x, y, pad, which, C_BONE, C_TAPE);
  if (label && *label) { hud_text(F_MONO_S, x + w + 8, y + 4, label, ink, 255, AL_LEFT); w += 8 + hud_text_w(F_MONO_S, label); }
  return w;
}

/* -- text with a cap in it ---------------------------------------- */
/* split fmt at its %s: head and tail (either may be empty); returns 0 when there is no %s */
static int split_fmt(const char *fmt, char *head, int hn, const char **tail) {
  const char *p = strstr(fmt, "%s");
  if (!p) { snprintf(head, (size_t)hn, "%s", fmt); *tail = ""; return 0; }
  int n = (int)(p - fmt); if (n > hn - 1) n = hn - 1;
  memcpy(head, fmt, (size_t)n); head[n] = 0;
  *tail = p + 2;
  return 1;
}

/* punctuation sits against the cap; a word gets a space */
static int tail_gap(const char *tail) { return (tail[0] == ',' || tail[0] == '.' || tail[0] == ':') ? 2 : 6; }

int key_text_w(int font, const char *fmt, int pad, int which) {
  char head[96]; const char *tail;
  if (!split_fmt(fmt, head, sizeof head, &tail)) return hud_text_w(font, head);
  int small = font == F_MONO_S;
  int w = hud_text_w(font, head) + (head[0] ? 6 : 0) + caps_w(pad, which, small);
  if (tail[0]) w += tail_gap(tail) + hud_text_w(font, tail);
  return w;
}

int key_text(int font, int x, int y, const char *fmt, int pad, int which, uint32_t ink, int a) {
  char head[96]; const char *tail;
  if (!split_fmt(fmt, head, sizeof head, &tail)) { hud_text(font, x, y, head, ink, a, AL_LEFT); return hud_text_w(font, head); }
  int small = font == F_MONO_S;
  int fh = hud_font_h(font), ch = small ? 16 : 24;
  int cy = y + fh / 2 - ch / 2 + (small ? 1 : 2);          /* the cap centred on the line's body */
  int cx = x;
  if (head[0]) { hud_text(font, cx, y, head, ink, a, AL_LEFT); cx += hud_text_w(font, head) + 6; }
  cx += caps(cx, cy, pad, which, small, C_BONE, C_TAPE);
  if (tail[0]) { cx += tail_gap(tail); hud_text(font, cx, y, tail, ink, a, AL_LEFT); cx += hud_text_w(font, tail); }
  return cx - x;
}

void key_format(char *out, int n, const char *fmt, int pad, int which) {
  char head[96]; const char *tail;
  if (!split_fmt(fmt, head, sizeof head, &tail)) { snprintf(out, (size_t)n, "%s", head); return; }
  snprintf(out, (size_t)n, "%s%s%s", head, key_name(pad, which), tail);
}
