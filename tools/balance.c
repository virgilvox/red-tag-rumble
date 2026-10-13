/* tools/balance.c: the native balance tool (spec 15.7). Links sim.c, bot.c,
 * arena.c and ai.c with -DRTR_HEADLESS and nothing from three.c, hud.h or
 * wasmcart. Owned by the balance agent (item 19) and the arena agent (item 4,
 * --check-arenas). Stub: parses the verbs and exits.
 *
 *   build/balance --robin 20 [arenas=all|bay|laser|pile|dock|shop|roof] [hazards=on|off|both]
 *   build/balance --check-arenas
 *   build/balance --replay tools/scripts/2p-fight.json --setup "<the [sim] setup line the cart logged>"
 */
#include <stdio.h>
#include <string.h>
#include "sim.h"
#include "bot.h"
#include "arena.h"
#include "ai.h"

static int usage(void) {
  fprintf(stderr, "usage: balance --robin N [arenas=...] [hazards=on|off|both] | --check-arenas | --replay script.json --setup \"line\"\n");
  return 2;
}

int main(int argc, char **argv) {
  if (argc < 2) return usage();
  if (!strcmp(argv[1], "--check-arenas")) {
    for (int i = 0; i < ARENA_COUNT; i++) printf("%-16s %2.0f x %2.0f  hazards %d props %d  (checker not written yet)\n", ARENAS[i].name, ARENAS[i].w, ARENAS[i].d, ARENAS[i].nhaz, ARENAS[i].nprops);
    return 0;
  }
  if (!strcmp(argv[1], "--robin")) {
    /* one match step to prove the headless link */
    match_setup_t s = { 0 };
    s.mode = MODE_DEATHMATCH; s.arena = AR_BAY; s.players = 2; s.fight_len_s = 75; s.seed = 7;
    build_preset(&s.builds[0], PRE_ROLL_OUT); build_preset(&s.builds[1], PRE_LAUNCH);
    static match_t m;
    sim_init(&m, &s);
    for (int i = 0; i < 60; i++) sim_step(&m);
    char line[256];
    sim_setup_line(&s, 0, line, sizeof line);
    printf("%s\n[sim] f=%d h=%08x\n(robin not written yet)\n", line, m.frame, sim_hash(&m));
    return 0;
  }
  if (!strcmp(argv[1], "--replay")) { printf("(replay not written yet)\n"); return 0; }
  if (!strcmp(argv[1], "--parts")) {
    for (int i = 0; i < PRE_RANDOM; i++) {
      build_t b; build_preset(&b, i);
      char name[24]; bot_name(name, &b);
      float sp, hi, to; build_bars(&b, &sp, &hi, &to);
      printf("%-9s %-18s %2d of %2d tags  speed %.2f hit %.2f tough %.2f  legal %d\n", PRESETS[i].name, name, build_cost(&b), build_budget(&b), sp, hi, to, build_legal(&b, 4));
    }
    return 0;
  }
  return usage();
}
