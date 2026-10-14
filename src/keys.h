/* keys.h: the name a pad's button has on this pad, for every keycap in the game
 * (docs/design/deep-controls.md 4.1). The browser keyboard and the RetroArch
 * keyboard are pad 0 with the pad bits, and their letters are other keys:
 * keyboard Z is pad A, keyboard X is pad B, keyboard A is pad X. The host says
 * which pad is a keyboard through wc_pad_name ("Keyboard"); a pad that names
 * nothing gets both names, keyboard first. Owned by the HUD agent; bodies in
 * keys.c. Not headless (it asks the host). */
#ifndef RTR_KEYS_H
#define RTR_KEYS_H
#include <stdint.h>

/* the buttons a prompt can name; KN_STICK is the drive, KN_DPAD the menu cursor */
enum { KN_A, KN_B, KN_X, KN_Y, KN_L, KN_R, KN_START, KN_SELECT, KN_DPAD, KN_STICK, KN_RT, KN_LT, KN_COUNT };
/* what the host says the pad is */
enum { KIND_PAD, KIND_KEYBOARD, KIND_UNKNOWN };

/* wc_pad_name for the four pads into a cache: main.c calls it once a second, and draw_hud keeps it
 * fresh by itself every 60 frames, so a cart without the main.c call still reads the right kind */
void        keys_poll(void);
/* KIND_KEYBOARD when the host's name starts with KEYBOARD (any case), KIND_PAD for any other name,
 * KIND_UNKNOWN for no name, no pad (an AI seat) or a pad out of range */
int         keys_kind(int pad);
/* "Z", "A" or "Z OR A": the name for one button on that pad (a static string) */
const char *key_name(int pad, int which);
/* the seat's pad (KIND_UNKNOWN for an AI or empty seat) */
int         key_seat_pad(int seat);
/* a keycap with the name at (x, y), 24 px tall; a KIND_UNKNOWN pad draws two caps, keyboard first,
 * with a slash between them. Returns the total width. */
int         key_cap(int x, int y, int pad, int which, uint32_t fill, uint32_t ink);
int         key_cap_seat(int x, int y, int seat, int which, uint32_t fill, uint32_t ink);
/* the width key_cap would draw */
int         key_cap_w(int pad, int which);
/* a 16 px keycap in F_MONO_S for the quadrant strips and the compact widget; same rules */
int         key_cap_small(int x, int y, int pad, int which, uint32_t fill, uint32_t ink);
int         key_cap_small_w(int pad, int which);
/* a keycap followed by a label, as the menus draw their hints; returns the width */
int         key_hint_pad(int x, int y, int pad, int which, const char *label, uint32_t ink);
/* text with one %s drawn as the keycap of (pad, which): the text before it, the cap, the text after it.
 * Returns the width; key_text_w measures without drawing. The font's line is centred on the cap. */
int         key_text(int font, int x, int y, const char *fmt, int pad, int which, uint32_t ink, int a);
int         key_text_w(int font, const char *fmt, int pad, int which);
/* fmt with the plain name in place of %s (for a line that cannot carry a cap) */
void        key_format(char *out, int n, const char *fmt, int pad, int which);

#endif
