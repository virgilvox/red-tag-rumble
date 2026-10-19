/* content.h: every string in RED TAG RUMBLE, one file, one lint (spec section 16,
 * plus the fixed UI labels of section 10). Owned by the content pass (item 22);
 * every agent adds its strings here, never inline in a .c file.
 *
 * Rules: ASCII caps, no dashes, no emoji, roles never names, jokes only in part
 * flavour, stingers, the rebuild hint, awards and OPS lines. Buffers: stinger 24,
 * speech 32, hint 48, award 44, award sub 48, tagline 56, tip 48, blurb 32,
 * hook 24, cause 48, bot name 24. Headless safe: no three.c, hud.h or wasmcart. */
#ifndef RTR_CONTENT_H
#define RTR_CONTENT_H

#define STR static const __attribute__((unused))
#define LEN(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* stingers, under 24 chars, 4 to 6 per trigger; the SFX is in the comment */
STR char *ST_FIGHT[]    = { "FIGHT", "GO BUILD SOMETHING", "DOORS LOCK AT TEN", "BE EXCELLENT. OR NOT", "MIND THE PIT" };            /* HORN */
STR char *ST_BIGHIT[]   = { "BIG HIT", "THAT WAS LOAD BEARING", "RED TAG THAT", "INSURANCE SAYS NO", "FREE PILE IT", "OUCH" };      /* HIT_HEAVY */
STR char *ST_CLASH[]    = { "CLASH", "SPARKS ARE FREE", "BOTH OF YOU STOP", "SAFETY GLASSES ON", "WHO SIGNED OFF ON THIS" };        /* CLASH */
STR char *ST_AIRBORNE[] = { "AIRBORNE", "NOT A FLYING ROBOT", "CHECK YOUR BALANCE", "GRAVITY IS A TOOL", "THAT IS NOT A DRONE" };   /* LAUNCH */
STR char *ST_INVERTED[] = { "INVERTED", "WRONG WAY UP", "TEN SECONDS", "WIGGLE SOMETHING", "SHOW SOME MOVEMENT" };                  /* KLAXON */
STR char *ST_PIT[]      = { "PITTED", "INTO THE PIT", "DOWN TO STORAGE", "THAT IS NOT A FLOOR", "WE WILL FIND IT LATER" };          /* KO */
STR char *ST_EDGE[]     = { "OFF THE ROOF", "NO WALL THERE", "THE ALLEY HAS IT NOW", "GRAVITY WINS AGAIN" };                        /* KO */
STR char *ST_DECK[]     = { "DECK COUNT", "COME DOWN FROM THERE", "HIGH GROUND. TEN.", "THAT IS NOT A SHELF" };                     /* KLAXON */
STR char *ST_DUCK[]     = { "DUCK", "LASER SAFETY FIRST", "EYES ONLY FOR LOW BOTS", "MIND THE BEAM" };                              /* KLAXON */
STR char *ST_KO[]       = { "KO", "KNOCKOUT", "PARTS BIN", "CALL IT", "IT IS PARTS NOW", "SHELF IT" };                               /* KO */
STR char *ST_COUNT[]    = { "COUNTED OUT", "NO MOVEMENT. OUT.", "NAP TIME IS OVER", "TEN. DONE." };                                 /* KO */
STR char *ST_OPS[]      = { "OPS INBOUND", "CLEAR THE AISLE", "OPS HAS NOTES", "OPERATIONS IS COMING" };                             /* HORN low */
STR char *ST_OPS_OUT[]  = { "OPS OUT", "BACK TO THE DESK", "CARRY ON" };                                                            /* MENU_BACK */
STR char *ST_TIME[]     = { "TIME", "DOORS LOCK", "JUDGES", "THAT IS TEN PM" };                                                      /* HORN */
STR char *ST_REDTAG[]   = { "RED TAGGED", "14 DAYS TO LIVE", "MARKED FOR THE PILE", "IT IS ANGRY NOW" };                             /* KLAXON tell */
STR char *ST_PICKUP[]   = { "FREE PILE", "ONE PERSON'S PANEL", "STILL GOOD" };                                                      /* MENU_OK low */
STR char *ST_VACUUM     = "NOT A VACUUM";                                                                                           /* MENU_BACK, once */
STR char *ST_HILL[]     = { "HILL MOVED", "NEW HILL", "OVER THERE" };                                                               /* MENU_MOVE */
STR char *ST_WEDGED[]   = { "WEDGED", "UNDER YOU", "DOORSTOPPED", "GET OFF THE PLATE" };                                            /* UNDERRIDE, the victim's view */
STR char *ST_LIMPING[]  = { "LIMPING", "WHEEL GONE SOFT", "ONE SIDE SLOW", "DRIVE IT HOME" };                                         /* WHEEL_HIT, the victim's view */
/* KO causes, under 16, shown under K.O. and as the placement word; indexed by KO_* in sim.h */
STR char *KO_CAUSE[]    = { "HP ZERO", "KO BY FLIP", "PITTED", "OFF THE ROOF", "COUNTED OUT", "DECK COUNT" };
/* results cause lines, under 48, chosen by cause (two for judges) */
STR char *KO_CAUSE_LINES[] = { "HP ZERO. IT HAD A GOOD RUN.", "KO BY FLIP. TURNS OUT GEOMETRY IS A WEAPON.", "PITTED. CHECK THE BASEMENT.",
  "OFF THE ROOF. NOT COVERED BY INSURANCE.", "COUNTED OUT. WE WAITED. WE REALLY DID.", "DECK COUNT. THE HIGH GROUND WAS A TRAP.",
  "JUDGES. THE UNDERDOG RULE APPLIED.", "JUDGES. THE DAMAGE BAR DOES NOT LIE." };
/* OPS speech, under 32 */
STR char *OPS_LINES[]   = { "CLEAR THE AISLE", "THAT IS NOT WHERE THAT GOES", "I AM NOT MAD. I AM OPERATIONS", "WHO LEFT THIS HERE", "THIS IS A WALKWAY", "DID YOU SIGN THE SHEET" };
/* title taglines, under 56 */
STR char *TAGLINES[]    = { "FOURTEEN DAYS. THEN IT IS FREE. OR IT IS A BOT.", "BUILT FROM OTHER PEOPLE'S UNFINISHED PROJECTS.", "A JANITORIAL CO-OP WITH AN ARENA ATTACHED.",
  "NEVER TELL SOMEONE NOT TO MAKE. A WEAPON.", "THERE IS ONLY ONE PLACE TO USE A BANDSAW. HERE.", "TWENTY TAGS. ZERO REFUNDS.", "OPS WOULD LIKE THE AISLE CLEAR.", "NO BRAKE CLEANER IN THE ARENA." };
/* attract tips, under 48 */
/* tips 0 and 1 carry the weapon key (KN_A) and the brake key (KN_B) as %s, drawn as keycaps for pad 0 */
STR char *ATTRACT_TIPS[] = { "HOLD %s TO SPIN UP. A HIT SPENDS THE SPIN.", "%s RIGHTS A FLIPPED HAMMER OR FLIPPER.", "HOVER FLOATS OVER SAWS. AND CANNOT PUSH.",
  "WALKERS GET 4 EXTRA TAGS AND STEP OVER PITS.", "RED TAG: UNDER 30 HP YOU HIT HARDER FOR 8 S.", "DROPPED PANELS ARE HP. DRIVE OVER THEM.", "OPS WAKES IF NOBODY HITS ANYONE FOR 12 S." };
/* mode blurbs, under 32, lobby */
STR char *MODE_BLURB[]  = { "1 TO 4. HP ZERO. LAST ONE UP.", "NO HP. FLIP OR FALL.", "LOSERS GET THE HAMMERS.", "HOLD THE SQUARE. 30 POINTS.", "2V2. FRIENDLY FIRE IS HALF.", "3 FIGHTS. NOBODY SITS OUT." };
/* arena hooks, under 24, vote cards */
STR char *ARENA_BLURB[] = { "PIT. TWO HAMMERS.", "SAWS. FIRE. A LASER.", "A MOUND. SCREWS.", "A DECK. RAIN. A GAP.", "BENCHES THAT BREAK.", "NO RAILS. WIND." };
/* garage hints, under 48 */
STR char *HINT_FLIPPED  = "FLIPPED %d TIMES: TRY LOWBOY OR SIX WHEEL";
STR char *HINT_SPINNER  = "A SPINNER ATE YOU: STEEL IS FOUR TAGS";
STR char *HINT_HAZARD   = "PUSHED INTO HAZARDS TWICE: TREADS DO NOT MOVE";
STR char *HINT_COUNTED  = "COUNTED OUT: KEEP MOVING. WIGGLE COUNTS";
STR char *HINT_CHEAP    = "YOU LEFT %d TAGS ON THE SHELF";
STR char *HINT_WON      = "YOU WON. CHANGE NOTHING. OR EVERYTHING.";
STR char *HINT_TIPS[]   = { "WEDGES WIN BY NOT HAVING A WEAPON", "HOVER IGNORES THE FLOOR. ALL OF IT", "HAMMERS LOVE INVERTED BOTS", "SPIN DOWN AND YOUR WEAPON IS A TARGET",
  "TAUNTING IS A JUDGING CRITERION. REALLY", "THE PIT OPENS AT ONE MINUTE LEFT" };
/* awards, one per player, priority order, name under 44, sub under 48 */
STR struct { const char *name, *sub; } AWARDS[] = {
  { "THE DOORSTOP DID IT",                "WON WITH NO WEAPON. ON PURPOSE." },            /* won with the WEDGE weapon */
  { "GRAVITY ENTHUSIAST",                 "MOST FLIPS LANDED." },                         /* most flips landed on others */
  { "SPINNER WITH A CEILING FAN COMPLEX", "MOST SPINNER DAMAGE." },                       /* most spinner damage */
  { "RED TAG HERO",                       "WON AFTER THE RED TAG." },                     /* won while or after red tag */
  { "LEFT THE CHAT",                      "RING OUT OR PIT. GONE." },                     /* KO by pit or edge */
  { "TAUNTED THE WRONG BOT",              "TAUNTED. THEN ATE 30." },                      /* taunt then a 30 plus hit within 60 frames */
  { "THE FLOOR IS LOAD BEARING",          "MOST TIME SPENT PUSHING." },                   /* most push frames */
  { "JANITOR",                            "MOST PANELS PICKED UP." },                     /* most pickups */
  { "SCARY BUT MILD",                     "MOST BURN. LEAST DAMAGE." },                   /* most burn dealt, least damage */
  { "DID NOT READ THE SIGN",              "MOST HAZARD DAMAGE TAKEN." },                  /* most hazard damage taken */
  { "WALL ENTHUSIAST",                    "MOST WALL HITS." },                            /* most wall hits */
  { "ARRIVED EVENTUALLY",                 "SLOWEST BUILD. STILL HERE." },                 /* slowest build, survived to time */
  { "SHELF STABLE",                       "UNDER 12 TAGS. SURVIVED." },                   /* under 12 tags, survived */
  { "ATTENDED",                           "WAS PRESENT." },                               /* fallback */
};
enum { AW_DOORSTOP, AW_GRAVITY, AW_SPINNER, AW_REDTAG, AW_LEFT, AW_TAUNTED, AW_FLOOR, AW_JANITOR, AW_MILD, AW_SIGN, AW_WALL, AW_ARRIVED, AW_SHELF, AW_ATTENDED, AW_COUNT };
/* judges, under 48 */
STR char *JUDGE_UNDERDOG = "UNDERDOG RULE";
STR char *JUDGE_TIE_COST = "TIE. CHEAPER BOT WINS. THAT IS THE RULE.";
STR char *JUDGE_TIE_FLIP = "TIE. FEWER FLIPS WINS.";
STR char *JUDGE_DRAW     = "DRAW. NOBODY TELL THE TREASURER.";
/* rules flash captions, three words or fewer */
STR char *FLASH_CAPTION[] = { "BUILD A BOT", "HOLD TO SPIN", "HIT THE SIDE", "FLIPPED? %s", "HAZARDS HURT", "LAST BOT STANDING", "CONTROLS" };   /* %s: the brake key of pad 0 */
STR char *FLASH_SMALL_6   = "LOSERS GET THE HAMMERS";
/* names */
STR char *WEAPON_ADJ[]  = { "HUMBLE", "POLITE", "POINTY", "HELPFUL", "GRINDY", "TOASTY", "THUMPY", "YEETING", "DRUMMY", "SPINNY", "CLINGY" };   /* WP_NONE..WP_CRUSHER */
STR char *MELTY_ADJ     = "DIZZY";
STR char *CHASSIS_NOUN[] = { "BOX", "DOORSTOP", "PANCAKE", "BOWL", "SKELETON", "LUNCHBOX" };
/* control card (deep-controls 4.8): %s is the brake key's cap */
STR char *CARD_LINE      = "FLIPPED? %s RIGHTS YOU.  STUCK? OPS IS COMING.";
STR char *CARD_KB_LINE   = "W TEST FIRES IN THE GARAGE. S HOLDS THIS CARD";
STR char *S_WEAPON_LINE  = "WEAPON: ";
/* placement words, under 10 */
STR char *PLACE_WORD[]   = { "WINNER", "HP ZERO", "FLIPPED", "PITTED", "OFF ROOF", "COUNTED", "DECK", "TIMED OUT" };

/* fixed UI labels (section 10), never jokes */
/* title (10.7) */
STR char *S_EYEBROW_TITLE = "HEATSYNC LABS ARCADE";
STR char *S_TITLE_1 = "RED TAG";
STR char *S_TITLE_2 = "RUMBLE";
STR char *TITLE_ROWS[]  = { "FIGHT", "SOLO", "HOW IT WORKS", "HALL OF FAME", "SETTINGS" };
STR char *TITLE_HINTS[] = { "1 TO 4 PADS", "YOU VS THE PRACTICE BOT", "18 S", "", "" };
STR char *S_BEST_EVENING = "BEST EVENING: %d WINS, SEAT %d";
STR char *S_KEY_SELECT = "SELECT";
STR char *S_KEY_BACK = "BACK";
STR char *S_KEY_NEXT = "NEXT";
STR char *S_KEY_SKIP = "SKIP";
STR char *S_KEY_VOTE = "VOTE";
STR char *S_KEY_CLOSE = "CLOSE";
STR char *S_KEY_UNDO = "UNDO";
STR char *S_KEY_ANY = "ANY BUTTON";
/* boot */
STR char *S_BOOT = "WARMING UP THE SHELF";
STR char *S_BOOT_GARAGE = "THE GARAGE";
/* lobby (10.8) */
STR char *S_PICK_A_MODE = "PICK A MODE";
STR char *S_EVENING = "EVENING %d . %d . %d . %d";
STR char *MODE_NAME[]   = { "DEATHMATCH", "KO BY FLIP", "LAST BOT STANDING", "KING OF THE HILL", "2V2", "TOURNAMENT" };
STR char *MODE_CARD_1[] = { "DEATHMATCH", "KO BY FLIP", "LAST BOT", "KING OF", "2V2", "TOURNAMENT" };
STR char *MODE_CARD_2[] = { "", "", "STANDING", "THE HILL", "", "" };
STR char *S_PRESS_A_TO_JOIN_1 = "PRESS A";
STR char *S_PRESS_A_TO_JOIN_2 = "TO JOIN";
STR char *S_FILL_WITH_BOT = "FILL WITH BOT";
STR char *S_FILL_HINT = "HOLD %s, LEFT RIGHT";                       /* %s: the Y key of pad 0 */
STR char *S_READY = "READY";
STR char *S_ARCADE = "ARCADE";
STR char *S_TANK = "TANK";
STR char *S_HOLD_B_LEAVE = "HOLD 1 S TO LEAVE";
STR char *S_TANK_NOTE = "TWO STICKS";
STR char *S_LOBBY_RULES = "HOW IT WORKS";
STR char *S_ONE_CAMERA = "ONE CAMERA";
STR char *S_SPLIT = "SPLIT";
STR char *S_COMEBACK = "COMEBACK";
STR char *S_GO_TO_GARAGE = "GO TO THE GARAGE";
STR char *S_AUTO_IN = "AUTO IN %d S";
STR char *S_LOBBY_EMPTY = "LOBBY: EMPTY IN %d";
STR char *S_NEXT_MATCH = "NEXT MATCH";
STR char *S_TAGS_N = "%d TAGS";
/* stepper values */
STR char *VAL_AUTO_ON_OFF[]      = { "AUTO", "ON", "OFF" };
STR char *VAL_ONE_CAMERA[]       = { "AUTO", "OFF", "ALWAYS" };
STR char *VAL_SPLIT[]            = { "AUTO", "HALVES", "QUADRANTS" };
STR char *VAL_FIGHT_LEN[]        = { "60 S", "75 S", "90 S", "120 S" };
STR char *VAL_ON_OFF[]           = { "ON", "OFF" };
STR char *VAL_STINGER[]          = { "BIG", "SMALL" };
STR char *VAL_DIFFICULTY[]       = { "EASY", "MEDIUM", "HARD" };
STR char *VAL_OFF_ON[]           = { "OFF", "ON" };
/* garage (10.9 to 10.11) */
STR char *ROW_NAME[]    = { "CHASSIS", "DRIVE", "WEAPON", "ARMOUR", "PAINT", "2ND WEAPON" };
STR char *S_ROW_BODY = "BODY";
STR char *S_TAGS_SPENT = "%d TAGS SPENT. %d LEFT.";
STR char *S_OVER_BUDGET = "OVER BUDGET BY %d";
STR char *S_LOCK_IN = "LOCK IN";
STR char *S_LOCK = "LOCK";
STR char *S_START_FIGHT = "FIGHT";
STR char *S_PRESET = "PRESET";
STR char *S_PRESET_HOLD_CONTROLS = "PRESET . HOLD: CONTROLS";
STR char *S_NOT_LEGAL = "NOT A LEGAL BUILD";
STR char *S_TOAST_CANNOT_LOCK = "CANNOT LOCK: %s";
STR char *S_TOAST_TIME_FALLBACK = "TIME UP. SEAT %d WAS %s, SO %s WAS LOADED";
STR char *S_RANDOM_BOT = "RANDOM BOT";
STR char *S_RANDOM = "RANDOM";
STR char *S_CONTROLS = "CONTROLS";
STR char *S_CTL = "CTL";
STR char *S_LOCKED = "LOCKED";
STR char *S_RELEASE = "RELEASE";
STR char *S_PRACTICE_BOT = "PRACTICE BOT (AI)";
STR char *S_NEXT_FIGHT = "NEXT FIGHT";
STR char *S_N_BOTS = "%d BOTS";
STR char *S_FIGHT_LENGTH = "FIGHT LENGTH";
STR char *S_GARAGE_HINT_1 = "HOLD %s: UP DOWN PICKS A ROW,";
STR char *S_GARAGE_HINT_2 = "LEFT RIGHT STEPS IT";
STR char *S_AI_TOGGLE_HINT = "HOLD %s, LEFT RIGHT SETS IT";
STR char *S_PRACTICE_ROW = "PRACTICE BOT";
STR char *S_TOAST_TIME_TRIMMED = "TIME UP. SEAT %d WAS %s, SO IT WAS TRIMMED";
/* the PAINT row: scheme number and decal set (3.7) */
STR char *DECAL_NAME[] = { "PLAIN", "STRIPES", "TAPE", "NUMBER" };
STR char *S_PAINT_ROW = "%d %s";
STR char *BAR_NAME[]    = { "SPEED", "HIT", "TOUGH" };
STR char *S_SCHEME = "SCHEME %d";
/* vote (10.12) */
STR char *S_PICK_AN_ARENA = "PICK AN ARENA";
STR char *S_VOTE_RULE_1 = "%s TO PICK.";                                 /* KN_STICK */
STR char *S_VOTE_RULE_2 = "%s TO VOTE. MAJORITY WINS. TIES GO RANDOM.";  /* KN_A */
STR char *ARENA_NAME[]  = { "THE BAY", "THE LASER ROOM", "THE FREE PILE", "THE DOCK", "THE SHOP", "THE ROOF" };
/* fight (10.4, 10.5) */
STR char *STATUS_WORD[] = { "", "SPINNING UP", "FULL SPIN", "EMPTY", "STUNNED", "INVERTED", "BURNING", "RED TAG", "LIFTED", "CLAMPED", "WEDGED", "LIMPING" };
/* SW_WEDGED and SW_LIMPING are the strip's own (a ridden bot, a hit wheel or track); sim_status_word never returns them */
enum { SW_NONE, SW_SPINNING_UP, SW_FULL_SPIN, SW_EMPTY, SW_STUNNED, SW_INVERTED, SW_BURNING, SW_RED_TAG, SW_LIFTED, SW_CLAMPED, SW_WEDGED, SW_LIMPING };
STR char *S_RED_TAG = "RED TAG";
STR char *S_BOOST = "BOOST";
STR char *S_RIGHT_YOURSELF = "%s TO RIGHT YOURSELF";                 /* %s: the brake key (HAMMER, FLIPPER) */
STR char *S_GET_MOVING = "GET MOVING";
STR char *S_NO_SELF_RIGHT = "INVERTED. NO SELF RIGHT";                 /* the truth under the count: nothing on the pad fixes this */
STR char *S_NO_SELF_RIGHT_OPS = "INVERTED. OPS COMES AT 5";            /* a 1v1: OPS lifts a helpless bot once the count passes 5 */
STR char *S_FLIP_METER = "NO HP. FLIP OR FALL";                        /* KO BY FLIP: the strip's bar slot when nobody is counting */
STR char *S_FLIPS_LABEL = "FLIPS";
STR char *S_ATTRACT_VS = "%s VS %s";
STR char *S_WARN_NO_SELF_RIGHT = "NO SELF RIGHT: A FLIP ENDS THIS BOT";   /* the garage header line on a build that cannot recover */
STR char *S_OPS = "OPS";
STR char *S_AI = "AI";
STR char *S_REPLAY = "REPLAY";
STR char *S_PAUSED = "PAUSED";
STR char *PAUSE_ROWS[]  = { "RESUME", "CONTROLS", "SETTINGS", "QUIT TO LOBBY" };
/* hazard seat (10.19) */
STR char *S_OUT = "OUT";
STR char *S_YOU_HAVE_HAMMERS = "YOU HAVE THE HAMMERS";
STR char *S_YOU_HAVE_FLAME = "YOU HAVE THE FLAME";
STR char *S_YOU_HAVE_FLIPPER = "YOU HAVE THE FLIPPER";
STR char *S_SEAT_PICK_HAMMER = "%s TO PICK A HAMMER";                   /* %s: the drive name, ARROWS or STICK */
STR char *S_SEAT_PICK_FLAME = "%s TO PICK A JET";
STR char *S_SEAT_PICK_FLIPPER = "%s TO PICK A PANEL";
STR char *S_OPS_DRIVER = "YOU DRIVE OPS";
STR char *S_OPS_GUNNER = "YOU RUN THE LIFTER AND THE SAW";
STR char *S_SEAT_SWING = "SWING";
STR char *S_SEAT_FIRE = "FIRE";
STR char *S_SEAT_COOLDOWN = "COOLDOWN %d.%d S";
/* KO, judges, results (10.18, 10.20, 10.21) */
STR char *S_KO_BIG = "K.O.";
STR char *S_TIME_BIG = "TIME";
STR char *S_JUDGES = "JUDGES";
STR char *JUDGE_ROW[]   = { "DAMAGE", "AGGRESSION", "CONTROL" };
STR char *S_WINNER_IS = "WINNER: %s";
STR char *S_X_WINS = "%s WINS";
STR char *S_SEAT_WINS = "SEAT %d WINS. %s.";
STR char *S_LEFT = "%d:%02d LEFT";
STR char *S_DEALT = "DEALT %d";
STR char *S_BIGGEST = "BIGGEST %d";
STR char *S_AIR = "AIR %d.%d S";
STR char *S_FLIPS = "FLIPS %d";
STR char *REMATCH_ROWS[] = { "REBUILD", "SAME BOTS", "NEW ARENA", "LOBBY" };
STR char *S_AUTO_N = "AUTO %d";
STR char *S_LOBBY_IN_N = "LOBBY IN %d";
STR char *S_WINS_TALLY = "WINS  1: %d   2: %d   3: %d   4: %d";
/* settings (10.22) */
STR char *S_SETTINGS = "SETTINGS";
STR char *SETTINGS_ROWS[] = { "SPLIT", "ONE CAMERA", "FIGHT LENGTH", "COMEBACK", "SHAKE", "HIT STOP", "STINGER SIZE", "SFX", "MUSIC", "COLOURBLIND", "RUMBLE", "STICKS", "RESET SAVE" };
STR char *S_HOLD_A_2S = "HOLD 2 S";
STR char *S_SAVED = "SAVED TO THE CABINET";
/* hall of fame (10.23) */
STR char *S_HALL = "HALL OF FAME";
STR char *S_FASTEST_KO = "FASTEST KO";
STR char *S_LONGEST_STREAK = "LONGEST STREAK";
STR char *S_WINS_BY_BUILD = "WINS BY BUILD";
STR char *S_KOS_BY_CAUSE = "KOS BY CAUSE";
STR char *S_MATCHES = "MATCHES %d";
STR char *S_NONE_YET = "NONE YET";
STR char *S_CUSTOM = "CUSTOM";
STR char *S_HOLD_B_WIPE = "HOLD 3 S: WIPE";
STR char *KO_SHORT[]    = { "HP ZERO", "FLIP", "PIT", "ROOF", "COUNT", "DECK" };
/* rules flash (10.25) */
STR char *S_HOW_IT_WORKS = "HOW IT WORKS";
STR char *S_N_OF_M = "%d OF %d";
/* controls card (10.24) callouts, two words each */
STR char *CARD_CALLOUT[] = { "DRIVE", "WEAPON", "BRAKE", "BOOST", "TAUNT", "PAUSE" };
STR char *CARD_SUB_PAD[]  = { "STICK OR D PAD", "A OR RT", "B", "L OR LT", "X", "START" };
STR char *CARD_SUB_KEYS[] = { "ARROWS", "Z", "X", "Q", "A", "ENTER" };
STR char *CARD_SUB_BOTH[] = { "ARROWS OR STICK", "Z OR A", "X OR B", "Q OR L", "A OR X", "ENTER OR START" };

/* fight HUD chips, the bracket card and the results eyebrow (draw.c, items 9, 10, 16) */
STR char *S_HILL = "HILL";
STR char *S_TEAM = "TEAM";
STR char *S_VS = "VS";
STR char *S_NEXT = "NEXT";
STR char *S_SEMIS = "SEMIS";
STR char *S_THIRD_PLACE = "THIRD PLACE";
STR char *S_FINAL = "FINAL";
STR char *S_BYE = "BYE";
STR char *S_CHAMPION = "CHAMPION";
STR char *S_RESULTS_EYEBROW = "%s . %s . %s";
STR char *S_HOLD_A_QUIT = "HOLD %s";                               /* %s: the weapon key of the pauser */
STR char *S_EVENING_LABEL = "EVENING";
STR char *S_ROW_2ND_SHORT = "2ND";

/* -- the attack lens (docs/design/deep-controls.md 4.2 to 4.9) ---- */
/* the weapon widget's state word (4.2), indexed by WW_* */
STR char *WIDGET_WORD[] = { "HOLD", "SPIN UP", "READY", "SPENT", "TAP", "TAP: ON", "ON", "SWING", "CLAMP", "LIFT", "CARRY", "EMPTY",
                            "REFILL", "RAM", "DRIVE AT THEM", "HIT", "INVERTED", "NO BOOST", "BOOST", "BRAKE", "RIGHTING", "SLIPPED",
                            "LET GO: TIP" };
enum { WW_HOLD, WW_SPIN_UP, WW_READY, WW_SPENT, WW_TAP, WW_TAP_ON, WW_ON, WW_SWING, WW_CLAMP, WW_LIFT, WW_CARRY, WW_EMPTY,
       WW_REFILL, WW_RAM, WW_DRIVE_AT, WW_HIT, WW_INVERTED, WW_NO_BOOST, WW_BOOST, WW_BRAKE, WW_RIGHTING, WW_SLIPPED, WW_LET_GO, WW_COUNT };
STR char *S_WW_SECONDS = "%d.%d S";            /* a cooldown counting down */
STR char *S_WW_HOLD_N  = "HOLD %d S";          /* the CRUSHER's hold */
STR char *S_WW_CARRY_N = "CARRY %d.%d S";      /* the LIFTER's carry: the time left before the forks put it down */
STR char *S_BOOST_PROMPT = "BOOST %s";         /* the boost key, once per 20 s on a long drive at the enemy */
STR char *S_BRAKE_PROMPT = "%s BRAKES";        /* the brake key, once per 20 s near a drop at speed */
/* why a landed hit was weak or strong (4.4), indexed by HIT_WHY_* in sim.h */
STR char *HIT_WHY[] = { "", "FRONT ARMOUR", "WEDGE", "SPINNING DOWN", "SHELL", "ARMOUR", "WALL", "EXPOSED", "STRIPPED", "WHEEL" };
/* the attack coach (4.3): a line with the key as %s, then a plain second line */
STR char *COACH_1[2]        = { "DRIVE WITH %s", "TURN TOWARD THE ARROW" };
STR char *COACH_2_SPIN[2]   = { "HOLD %s TO SPIN UP.", "KEEP OFF THE WALL" };
STR char *COACH_2_SAW[2]    = { "TAP %s: SAW ON", "IT GRINDS WHAT IT TOUCHES" };
STR char *COACH_2_TAP[2]    = { "TAP %s WHEN THEY ARE CLOSE", "" };
STR char *COACH_2_LIFT[2]   = { "HOLD %s, DRIVE INTO THEM", "LET GO TO TIP" };
STR char *COACH_2_FLAME[2]  = { "HOLD %s. IT IS SHORT", "GET CLOSE FIRST" };
STR char *COACH_2_RAM[2]    = { "DRIVE INTO THEIR SIDE.", "SPEED IS THE WEAPON" };
STR char *COACH_3[2]        = { "HIT THEIR SIDE, NOT THE FRONT", "THE ARROW MARKS THE WEAK SIDE" };
STR char *COACH_3_FRONT     = "FRONT. TRY THE SIDE";
/* the self right prompt under the count (4.6) */
STR char *S_SR_DRUM_SPIN  = "HOLD %s TO SPIN, THEN TAP IT";
STR char *S_SR_DRUM_LET   = "LET GO, TAP %s";
STR char *S_SR_DRUM_TAP   = "TAP %s";
STR char *S_SR_DRIVES     = "DRIVES UPSIDE DOWN. KEEP MOVING";
STR char *S_SR_HORIZ      = "UPSIDE DOWN. SPINS, CANNOT STEER";
STR char *S_SR_HORIZ_OPS  = "SPINS, CANNOT STEER. OPS COMES AT 5";
STR char *S_TEST_FIRE     = "TEST FIRE";
/* one line per weapon for the garage and the controls card (4.9): input kind, what it does, reach; under 44 */
STR char *WEAPON_HOWTO[] = {
  "NO WEAPON. RAM THEIR SIDE AT SPEED",            /* NONE */
  "PASSIVE. RAM AT SPEED. SLIDES UNDER THEM",      /* WEDGE: the lip at 0.6 */
  "TAP. A JAB STRAIGHT AHEAD. REACH 1.0",          /* SPIKE: hb_z1 1.05 */
  "HOLD %s. DRIVE INTO THEM. LET GO TO TIP",       /* LIFTER: %s is the weapon key (howto_of fills it) */
  "TAP TO TOGGLE. GRINDS ON CONTACT. REACH 0.7",   /* SAW: the circle at 0.7 */
  "HOLD. SHORT CONE, FUEL RUNS OUT. REACH 1.5",    /* FLAME: the 1.5 cone */
  "TAP. A SHORT WIND UP, THEN DENTS. REACH 1.0",   /* HAMMER: 15 frames of wind up, hb_z1 1.0 */
  "TAP. THROWS THEM. 1.8 S RELOAD. REACH 0.9",     /* FLIPPER: cooldown 108 */
  "HOLD TO SPIN. POPS THEM UP. REACH 0.6",         /* DRUM: 0.55 + 0.3 */
  "HOLD TO SPIN 2 S. HITS SIDEWAYS. REACH 0.7",    /* HORIZONTAL: 0.15 + 0.55 */
  "TAP. CLAMPS 2 S. 3 OR 4 PLAYERS. REACH 0.9",    /* CRUSHER: hb_z1 0.9, not in a 1v1 */
};
STR char *MELTY_HOWTO = "HOLD TO SPIN. THE WHOLE BOT HITS. REACH 0.5";

#endif
