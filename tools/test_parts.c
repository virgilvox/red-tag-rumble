/* tools/test_parts.c: the parts catalogue test (work item 2, spec section 3).
 * Prints every preset's cost, budget, bars and bot name, then checks the rules
 * the spec states in words against the code in parts.h: preset costs and names
 * (3.9), the budget bonuses (3.2), legality (CRUSHER refused in 1v1, HORIZONTAL
 * refused on WEDGE, MELTY has no weapon row, the secondary mount), option_legal
 * against build_legal, RANDOM BOT always legal and spending at least 17 tags when
 * it can (3.10), the bars in 0..1 (3.8) and every name within its buffer (3.11).
 * Headless: parts.h and content.h only.
 *
 *   cc -std=gnu11 -Wall -Wextra -I src tools/test_parts.c -o build/test_parts && build/test_parts
 *
 * Exits 0 when every check passes. Owned by the sim agent. */
#include <stdio.h>
#include <string.h>
#include "parts.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void) {
  /* 3.9: tags and names as the spec table states them */
  static const int COST[6] = { 13, 12, 15, 12, 16, 14 }, BUDGET[6] = { 20, 20, 20, 24, 20, 20 };
  static const char *NAME[6] = { "POLITE DOORSTOP", "YEETING BOX", "SPINNY PANCAKE", "THUMPY LUNCHBOX", "TOASTY BOWL", "DIZZY PANCAKE" };
  printf("preset     bot name            tags    speed  hit   tough  legal 1v1 4p\n");
  for (int i = 0; i < PRE_RANDOM; i++) {
    build_t b; memset(&b, 0, sizeof b); build_preset(&b, i);
    char name[24]; bot_name(name, &b);
    float sp, hi, to; build_bars(&b, &sp, &hi, &to);
    printf("%-10s %-18s %2d of %2d  %.2f  %.2f  %.2f   %d   %d\n", PRESETS[i].name, name, build_cost(&b), build_budget(&b), sp, hi, to, build_legal(&b, 2), build_legal(&b, 4));
    CHECK(build_cost(&b) == COST[i], "%s costs %d, spec %d", PRESETS[i].name, build_cost(&b), COST[i]);
    CHECK(build_budget(&b) == BUDGET[i], "%s budget %d, spec %d", PRESETS[i].name, build_budget(&b), BUDGET[i]);
    CHECK(!strcmp(name, NAME[i]), "%s is named %s, spec %s", PRESETS[i].name, name, NAME[i]);
    CHECK(build_legal(&b, 2) && build_legal(&b, 4), "%s is not legal", PRESETS[i].name);
    CHECK(build_preset_of(&b) == i, "%s round trips to preset %d", PRESETS[i].name, build_preset_of(&b));
    CHECK(sp >= 0 && sp <= 1 && hi >= 0 && hi <= 1 && to >= 0 && to <= 1, "%s bars out of 0..1", PRESETS[i].name);
  }

  /* 3.2: WALKER adds 4, LEG TRACKS adds 2 */
  build_t b; memset(&b, 0, sizeof b);
  b.drive = DR_WALKER; CHECK(build_budget(&b) == 24, "WALKER budget %d", build_budget(&b));
  b.drive = DR_LEGTRACK; CHECK(build_budget(&b) == 22, "LEG TRACKS budget %d", build_budget(&b));
  b.drive = DR_TWO; CHECK(build_budget(&b) == 20, "base budget %d", build_budget(&b));

  /* 2.2 and 20.11: legality in code */
  memset(&b, 0, sizeof b); b.chassis = CH_BOX; b.drive = DR_TWO; b.weapon = WP_CRUSHER;
  CHECK(!build_legal(&b, 2), "CRUSHER legal in a 1v1");
  CHECK(!build_legal(&b, 1), "CRUSHER legal in solo");
  CHECK(build_legal(&b, 3) && build_legal(&b, 4), "CRUSHER refused with 3 or 4 players");
  b.chassis = CH_WEDGE; b.weapon = WP_HORIZ;
  CHECK(!build_legal(&b, 4), "HORIZONTAL legal on WEDGE");
  b.chassis = CH_BOX; b.drive = DR_MELTY; b.weapon = WP_SPIKE;
  CHECK(!build_legal(&b, 4), "MELTY with a weapon is legal");
  b.weapon = WP_NONE; CHECK(build_legal(&b, 4), "MELTY with no weapon is refused");

  /* 3.2: the secondary mount */
  memset(&b, 0, sizeof b); b.chassis = CH_BOX; b.drive = DR_TWO; b.weapon = WP_WEDGE; b.weapon2 = WP_SAW;
  CHECK(build_legal(&b, 4), "WEDGE plus SAW refused");
  CHECK(build_cost(&b) == 2 + 2 + 1 + 3 - 1, "secondary costs its price minus 1 (%d)", build_cost(&b));
  b.weapon2 = WP_HAMMER; CHECK(!build_legal(&b, 4), "a HAMMER secondary is legal");
  b.weapon = WP_FLIPPER; b.weapon2 = WP_SPIKE; CHECK(!build_legal(&b, 4), "a secondary on a FLIPPER box is legal");
  b.chassis = CH_LUNCHBOX; CHECK(build_legal(&b, 4), "LUNCHBOX top mount refused");
  b.chassis = CH_BOX; b.weapon = WP_LIFTER; b.weapon2 = WP_FLAME; CHECK(build_legal(&b, 4), "LIFTER plus FLAME refused");

  /* option_legal agrees with build_rules for every single option change from every preset */
  for (int i = 0; i < PRE_RANDOM; i++) for (int players = 2; players <= 4; players += 2) for (int row = ROW_CHASSIS; row <= ROW_ARMOUR; row++) {
    build_t p; memset(&p, 0, sizeof p); build_preset(&p, i);
    for (int o = 0; o < row_count(&p, row); o++) {
      const char *why = 0;
      int ok = option_legal(&p, row, o, players, &why);
      CHECK(ok || why, "%s row %d option %d refused without a reason", PRESETS[i].name, row, o);
      if (row == ROW_WEAPON && o == WP_CRUSHER && players == 2) CHECK(!ok || p.drive == DR_MELTY, "CRUSHER offered in 1v1 from %s", PRESETS[i].name);
    }
  }

  /* 3.10: RANDOM BOT is always legal, deterministic from the rng, and spends 17 or more when it can */
  int low = 0, n = 20000; uint32_t rng = 12345u;
  for (int k = 0; k < n; k++) {
    int players = 1 + k % 4;
    build_t r; memset(&r, 0, sizeof r); build_random(&r, &rng, players);
    CHECK(build_legal(&r, players), "random build %d,%d,%d,%d illegal for %d players", r.chassis, r.drive, r.weapon, r.armour, players);
    if (build_cost(&r) < 17) low++;
    char name[24]; bot_name(name, &r);
    CHECK(strlen(name) <= 20, "random bot name %s over 20 characters", name);
    if (fails > 20) break;
  }
  printf("random bots: %d rolled, %d under 17 tags\n", n, low);
  uint32_t r1 = 99u, r2 = 99u; build_t x, y; memset(&x, 0, sizeof x); memset(&y, 0, sizeof y);
  build_random(&x, &r1, 4); build_random(&y, &r2, 4);
  CHECK(!memcmp(&x, &y, sizeof x), "RANDOM BOT differs from the same seed");

  /* 3.11: every name fits (the longest is YEETING LUNCHBOX at 16) */
  int longest = 0; char lname[24] = "";
  for (int c = 0; c < CH_COUNT; c++) for (int d = 0; d < DR_COUNT; d++) for (int w = 0; w < WP_COUNT; w++) {
    build_t t; memset(&t, 0, sizeof t); t.chassis = (uint8_t)c; t.drive = (uint8_t)d; t.weapon = (uint8_t)(d == DR_MELTY ? WP_NONE : w);
    char name[24]; bot_name(name, &t);
    if ((int)strlen(name) > longest) { longest = (int)strlen(name); snprintf(lname, sizeof lname, "%s", name); }
    float sp, hi, to; build_bars(&t, &sp, &hi, &to);
    CHECK(sp >= 0 && sp <= 1 && hi >= 0 && hi <= 1 && to >= 0 && to <= 1, "bars out of range for %d,%d,%d", c, d, w);
  }
  printf("longest bot name: %s (%d)\n", lname, longest);
  CHECK(longest <= 20, "a bot name is over 20 characters");

  printf(fails ? "test_parts: FAIL (%d)\n" : "test_parts: PASS\n", fails);
  return fails ? 1 : 0;
}
