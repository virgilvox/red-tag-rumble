/* bot.h: build_t to body_t and stats, the hint picker, and the headless side of
 * dent geometry (spec 14.1). Owned by the sim agent (item 3) for the body and
 * stats, and by the garage agent (item 11) for bot_hint. Bodies in bot.c.
 * Headless safe: nothing from three.c, hud.h or wasmcart. The part mesh
 * builders live in world.c, not here. */
#ifndef RTR_BOT_H
#define RTR_BOT_H
#include "sim.h"

void  bot_body_from_build(body_t *b, const build_t *build);   /* mass, inertia, hl, hw, shape, clr, hgt, lips; position untouched */
/* the wedge kind (parts.h LIP_*) on a build's front given its lifter position, LIP_NONE for a plain face */
int   bot_wedge_kind(const build_t *build, float lift);
float bot_hp_max(const build_t *build);
/* the rebuild hint (3.1), by priority: flipped 3 or more, lost to a spinner on
 * NONE or PLYWOOD, pushed into a hazard twice, counted out, under 14 tags, else a
 * random tip. Returns 1 when a hint was written. */
int   bot_hint(const bot_t *b, const match_t *m, char out[48]);
/* set up bots[slot] of a match from a seat's build (name, hp, body, preset) */
void  bot_setup(bot_t *b, const build_t *build, int seat, int is_ai, int team, float x, float z, float heading);
/* the OPS house robot (section 7) */
void  bot_setup_ops(bot_t *b, float x, float z, float heading, int driven);
/* drive stats after quirks: top speed with boost, red tag, wheel hit, inverted scaling */
float bot_top_speed(const bot_t *b);
/* can this bot drive right now (not airborne, lifted, clamped, stunned, braking, locked or dead) */
int   bot_can_drive(const bot_t *b);
/* dented panel geometry seeds (14.2): 2 to 4 vertex clusters pushed in by 0.02, 0.05 and 0.09 */
typedef struct { float cx, cy, cz, r, depth; } dent_t;
int   bot_dent_pattern(int panel, int state, dent_t out[4]);

#endif
