# RED TAG RUMBLE: controls, attack clarity and teaching

A spec from the controls lens of the October 2026 deep pass. Built against main at 22c05b6 in a detached worktree (`games/_wt/rtr-deep-controls`); every line number below is from that tree. Nothing here was committed. The evidence is the Chrome keyboard play (`build/play/*.png`, `build/play/log.txt`), the rig fixtures (`build/shots/{kb,ks,f2,f4,pause,menu,feel}-*.png`) and two probe scripts written for this pass (`build/probe-chop.json`, `build/probe-rollout.json`, shots `build/shots/{chop,roll}-*.png`). All of those live under the worktree's `build/`, which is not committed; the probe scripts are reproduced in section 7 so the implementers can recreate them.

House rules apply to every string in this document: ASCII caps for on screen strings, no dashes, no emoji, GANTRY colours only (char, bone, hazard amber, rust, live teal, plus the existing slab, steel, ash and smoke tokens of hud.h).

## 0. The short version

1. **A first time player on the default build cannot attack at all.** `game_seat_join` (game.c:52) gives seat i preset i, so the first pad to sit down gets ROLL OUT: WEDGE chassis, TREADS, WEDGE weapon, STEEL. The WEDGE weapon is `WI_PASSIVE` (parts.h:104); `weapon_machine` returns at sim.c:652 for WP_WEDGE, and TREADS has no boost (parts.h:94). So A does nothing, L does nothing, the gauge draws a smoke coloured wedge glyph and nothing else (draw.c:485 to 488), and the controls card still says WEAPON A OR RT. The only attack this bot has is a ram at closing speed over 1.5 u/s with the nose within 30 degrees of the contact (sim.c:957 to 959), which nothing on screen says. This is the single biggest cause of "it is not clear how to attack".
2. **Every keycap in the game names the pad button, and on the keyboard the pad letters are other keys.** Keyboard A is pad X, keyboard X is pad B (index.html:106 to 107). So a keyboard player who reads `A LOCK IN` in the garage and presses the A key gets RANDOM BOT (game_flow.c:989), and one who reads `X RANDOM BOT` and holds the X key for a second takes the whole table back to the lobby (game_flow.c:944 to 947). `FLIPPED? B` on the rules card, `B TO RIGHT YOURSELF` under the count and `A READY` in the lobby have the same trap. The host can tell the cart which pad is a keyboard (`wc_pad_name`, wasmcart.h:377 to 387, returns "Keyboard"), and the cart never asks (no call site in src/).
3. **Nothing on screen says whether a hit landed, who it landed on, or why it was weak.** Every damage number is bone in every viewport (main.c:335, 349, 354 pass `C_BONE`; the spec's 10.4 wanted the attacker's seat colour for dealt and rust for taken). The only "you were hit" flash is the viewport white on the huge and clash bands (fx.c:153 to 156). A 0.5 zone wedge front hit, a 0.6 front plate hit and a 1.5 exposed hit all look the same.
4. **A spinner driver's own wall hit is the most confusing damage in the game.** In the CHOP probe the keyboard player held Z, drove forward, met the wall and lost 15 HP and all spin (sim.c:1047, `WP_HORIZ_WALL_DMG`) with a rust 15 and sparks, status word back to SPINNING UP (`build/shots/chop-1100.png`). No prompt says the weapon hit the wall.
5. **The weapon's state is a 44 px ring with a 15 px word under it**, no key on it, nothing that says READY or the input kind (hold, tap, toggle); in quadrants the word is gone. The hammer wind up, the flipper fire, the spike pop and the saw toggle make no sound (main.c's drain has no `EV_WEAPON_TELL` case; fx.c:218 is `break;`).
6. **The rules flash and the attract teach things the sim does not do.** `HOLD A TO SPIN UP. LET GO TO KEEP SPIN.` (content.h:46) is false: spin falls at half the spin up rate when released (`spinner_follow`, sim.c:641). Card 3 says the front plate is 0.6 and the side 1.0; the WEDGE chassis front is 0.5 (sim.c:486) and a lifted or airborne bot is 1.5, which is where the big numbers come from. The attack coach below replaces the tip and the two cards with beats tied to the live fight.

Everything in sections 3 and 4 follows from those six.

## 1. Current state, verified against the code

### 1.1 Input path

- `read_pads` (main.c:162 to 207) copies `wc_pad_t` into `PADS[i].cur`; the d-pad becomes full left stick deflections when the stick rests in the 0.18 dead zone (main.c:178 to 189), both sides for a TANK seat (main.c:182 to 185); triggers are 0..255 with `TRIGGER_ON 128` (sim.h:35). Menu nav is edge with repeat after 18 then every 8 (main.c:201 to 202).
- The sim reads the weapon as `IN_A || rt > TRIGGER_ON` (sim.c:544), boost as `IN_L || lt > TRIGGER_ON` (sim.c:546 to 549), brake and taunt as B and X edges (sim.c:564 to 565), self right as a B edge for `SR_B` weapons and an A edge at spin over 0.5 for the DRUM (sim.c:1525 to 1526).
- Keyboard map (web/index.html:106 to 107): arrows, Z=A, X=B, A=X, S=Y, Enter=Start, right Shift=Select, Q=L, W=R. The RetroArch core delivers the keyboard as pad 0 with the same bits (HANDOFF Platform facts; the memory note says raw keyboard never reaches a cart). Neither host sets a stick or a trigger for it.
- `wc_pad_name(pad, buf, len)` (wasmcart.h:377 to 387) is a weak host import: "the SDL controller name, Gamepad API id, or Keyboard". CartHostWeb copies `pad.name` from what the page passes (CartHostWeb.js:2444 to 2448, 2604); index.html passes `name: g.id` for a gamepad and nothing for the keyboard pad (index.html:126 to 131). The Node rig passes nothing. The cart has no call site.

### 1.2 What the fight HUD shows about the weapon

`gauge` (draw.c:445 to 494) in a 44 px box (36 in quadrants):

| Weapon | Drawn | Missing |
|---|---|---|
| HORIZONTAL, DRUM, SAW, MELTY | steel ring, spin ring in the status colour, live pulse at 0.97, glyph | the 0.5 threshold at which a hit is possible (sim.c:516 to 517); that the SAW is a toggle and whether it is on (the ring reads `b->spin`, which is 0 after a toggle off anyway); the spin spent per hit (0.7 HORIZ, 0.6 DRUM, all for MELTY) |
| FLIPPER, FLAME | vertical tank (cooldown refilling, fuel), glyph, rust cooldown ring | READY; the input kind |
| HAMMER, SPIKE, CRUSHER | glyph (hazard while winding up), 6 px bar, rust cooldown ring | READY; TAP |
| LIFTER | a wedge of lift angle, glyph | HOLD, and that it carries only a victim facing away (sim.c:781) |
| WEDGE, NONE | a smoke glyph | that the attack is a ram, and when the ram is armed (closing over 1.5 u/s and the 90 frame `RAM_CD`) |

The status word (draw.c:426 to 433, 551; strings content.h:187) is `F_MONO_S` at (474, 54) in the strip and absent in quadrants. The boost bar (draw.c:543 to 549) has no label; in quadrants it is three pips (draw.c:559 to 563).

### 1.3 Hit feedback

- Numbers: `number_all` (main.c:263 to 267) paints the same bone number in every live viewport; `damage_number` big from the hit band up (main.c:335). Wall damage is rust (main.c:354, 358). Nothing distinguishes dealt from taken.
- Viewport white: huge and clash only (fx.c:153 to 156).
- Shake and punch per band (main.c:317 to 324), rumble (main.c:331 to 332), sparks from the victim's armour (fx.c).
- The stinger for a hit: BIG HIT at the huge band on the victim's viewport (main.c:336).
- Zone: `hit_zone` (sim.c:482 to 499) returns 0.5 (WEDGE chassis front), 0.6 (box front plate, contact in local z over 0.6 hl and |x| under hw), 1.0 side and rear, 1.5 exposed (lifted, airborne over 0.15, self righting, or inverted against a hammer, drum or spike), 2.0 on a spun down spinner's weapon circle, times 0.8 on a SHELL, times 0.7 for a hammer on a LUNCHBOX. The event carries `zone` times 10 in `event_t.zone` (sim.h:98 to 100) and main.c never reads it.
- Spinner hits need spin over `SPINNER_HIT_MIN` 0.5 (sim.c:94, 516) and bleed spin after each (sim.c:757 to 759); a HORIZONTAL at over 0.5 spin hitting a wall loses all spin and 15 HP (sim.c:1047), an EV_SPIN_WALL the drain shows as a rust number with a 0.6 CLASH sound (main.c:356 to 359).

### 1.4 Inverted, count, self right

`countout` (draw.c:299 to 321) draws the numeral and one of: `GET MOVING`, `B TO RIGHT YOURSELF` when `can_self_right` (draw.c:297: `selfright != SR_NONE || MELTY`), `INVERTED. OPS COMES AT 5` in a 1v1 that is not KO BY FLIP, `INVERTED. NO SELF RIGHT` otherwise. The sim rights a HAMMER or FLIPPER on a B edge (90 frames) and a DRUM on an A edge at spin over 0.5 (sim.c:1525 to 1526). So the DRUM prompt says B and the key is A, and it only works after the spin is up, which holding A does but an A *edge* needs a release and a second press. The garage warns `NO SELF RIGHT: A FLIP ENDS THIS BOT` on helpless builds (draw_menus.c:484 to 488, 525).

### 1.5 Teaching surfaces

- Rules flash (draw_menus.c:1170 to 1342): seven cards, keycaps `A`, `B`, callouts `A OR RT`, `B`, `L OR LT`; keyboard line only on the last card and only `if (seat == 0)` (draw_menus.c:1144), which the flash passes as 0 always (draw_menus.c:1306). Shown once on a fresh save, skippable by any button (game_flow.c:649 to 652, 1346 to 1362).
- Controls card (draw_menus.c:1123 to 1154): six callouts round a pad outline, `CARD_LINE` and `CARD_KEYS` (content.h:92 to 93). Shown 2 s on the first join (`FIRST_CARD_S`, game_flow.c:37, 264) over the lobby turntable (draw_menus.c:355 to 360) or the garage turntable, under Y in the garage after 0.4 s (game_flow.c:956) and in the fight while Y is held (game_flow.c:1176), and from the pause menu (draw.c:1370 to 1376).
- Garage key hints (draw_menus.c:567 to 573): `A LOCK IN`, `START FIGHT`, `X RANDOM BOT`, `Y PRESET . HOLD: CONTROLS`; lobby (draw_menus.c:274, 281, 314 to 316): `X ARCADE`, `A READY`, `B HOLD 1 S TO LEAVE`, `Y HOW IT WORKS`; title (draw_menus.c:201 to 203): `A START SELECT`.
- The weapon row's flavour line (parts.h:101 to 114) is a joke per house rule, and the only place the input kind exists is the `input` column the player never sees.
- Attract tips (content.h:46 to 47), one every 8 s on the title.
- The web page's aside (index.html:78 to 85) is the only complete keyboard map and it is off the canvas, which the station does not have.

## 2. Playtest log: every moment of not knowing

### 2.1 Keyboard, Chrome, first time path (`node tools/play.mjs`, `build/play/`)

The script is a stand in for a new player: it reads nothing, taps Z on the title, Enter through, edits two rows, drives with the arrows, taps Z and Q.

| Moment | What happened | Evidence |
|---|---|---|
| Lobby | Seat 1 is POLITE DOORSTOP (ROLL OUT). The card says `A READY` and `X ARCADE`. A keyboard player pressing the A key here flips the scheme to TANK (game_flow.c:735). | `build/play/03-lobby.png` |
| Garage | Rows read WEDGE TREADS WEDGE STEEL. The footer says `A LOCK IN` `START FIGHT` `X RANDOM BOT`. Nothing says this bot has no button weapon. The script's two Down plus Right edits (DRIVE to HOVER, WEAPON to SPIKE) are what the test does, not what a player would. | `build/play/04-garage.png`, `05-garage-edited.png` |
| Horn | Full view; the practice bot is a dot at the far wall; the chevron is a 24 px triangle on the viewport edge (draw.c:257 to 274) with no label. | `08-fight-0.png`, `build/shots/kb-700.png` |
| Z | The SPIKE fired (rust cooldown ring at `15-weapon.png`); no sound, nothing in reach, no READY afterwards. | `15-weapon.png` |
| Q | The boost bar went bone then rust; nothing says BOOST, no sound (`EV_BOOST` has no drain case). | `16-boost.png` |
| 11 s in | Player 79 HP, practice bot 100. The player took 11 and has no idea from what (a walker hammer; the number was bone like every other number). | `19-fight-11s.png` |
| 55 s | KO, player 0, enemy 15. Eighty seconds of Z and arrows against an EASY bot dealt 85 damage in a few lucky collisions; the log shows 17 of 33 damage seconds were 4 point hammer misses by the bot itself. | `20-fight-end.png`, `log.txt` lines 69 to 237 |

### 2.2 Keyboard, rig, spinner build (`build/probe-chop.json`, `STATION=1`)

Y twice in the garage picks CHOP (LOWBOY, FOUR WHEEL, HORIZONTAL, ALUMINIUM), A held from frame 700, Up from 760.

| Frame | Screen | Not known |
|---|---|---|
| 700 | ring empty, no word; the enemy is a dot across the pit | whether holding Z does anything yet |
| 850 | ring full teal, `FULL SPIN`, a world ring on the disc, the player is heading at the far wall | that the disc only bites above half, and that the enemy stepped aside (EASY holds off 180 frames at the horn) |
| 1100 | HP 85 to 70, rust 15 and sparks at the wall, ring back to a third, `SPINNING UP`, a teal chevron at the bottom edge pointing behind | why 15 HP went: the spinner hit the lexan (sim.c:1047) |

No `[hits]` line in 840 frames: the bot never met the enemy. `build/shots/chop-{700,850,1100}.png`.

### 2.3 Keyboard, rig, default build (`build/probe-rollout.json`)

ROLL OUT, A held, L tapped, Up for 12 s. No boost (TREADS), no weapon, no hits; the bot reached the wall, took 0 and sat there with wall sparks (`build/shots/roll-1100.png`). The chevron pointed behind. A player would conclude the game has no attack.

### 2.4 Pad, rig, the fixtures

- `2p-fight.json` 900: halves, ROLL OUT (gauge: a smoke wedge) against CHOP (`FULL SPIN`); the strips are the only reading of weapon state (`build/shots/f2-900.png`).
- `feel.json` 866: a 12 ram shows `12` in bone in both halves and `STUNNED` on the victim's strip. The attacker's viewport shows the same number as the victim's (`build/shots/feel-866.png`). No way to tell who dealt it.
- `4p-fight.json` 900: quadrants, two `10`s in bone, gauges 36 px, no status words, a `T` glyph for the hammer that reads as a spike (`build/shots/f4-900.png`).
- `pause.json` 1660: the full controls card; seat 1's strip beneath it shows the wedge build's empty gauge while the card says WEAPON A OR RT (`build/shots/pause-1660.png`).
- `menu.json`: card 2 HOLD TO SPIN shows an `A` keycap and a ring; card 3 HIT THE SIDE shows 0.6 front and 1.0 sides; card 7 is the controls card with the keyboard line (`build/shots/menu-{330,375,525}.png`).

### 2.5 The list the task asked for

| Question a player has | Answered today? | Where it should be answered (section 4) |
|---|---|---|
| How do I attack | no (default build has no button attack; keycaps say A, the key is Z) | 4.1 key names, 4.2 widget, 4.3 coach beat 2, 4.10 default build |
| Is the weapon ready | partly (ring, tank, bar; no READY, no threshold, no input kind) | 4.2 |
| Did my hit land | no (bone number in every viewport) | 4.4 |
| Where do I hit them | once, in a flash card nobody keeps | 4.3 beat 3, 4.5 weak side |
| Why did I take damage | no (bone number; rust for walls and hazards only) | 4.4 taken edge and the reason words |
| How do I right myself | partly (B TO RIGHT YOURSELF; wrong for DRUM; pad letter) | 4.6 |
| What is boost | no (unlabelled bar) | 4.7 |
| What does my weapon do | no (a joke line) | 4.9 |

## 3. Problems, ranked, with the fix each one gets

| # | Problem | Evidence | Fix |
|---|---|---|---|
| P1 | The first seat's default bot has no button attack and no boost | game.c:52, parts.h:104, sim.c:652 | 4.10: seat 1's first evening preset is LAUNCH; ROLL OUT's gauge gets a RAM widget (4.2) |
| P2 | Keycaps say pad letters to keyboard players; A, X and B are all other keys | index.html:106, draw_menus.c:567 to 573, 274, 281, 1144, 1201, 1259; content.h:92 to 93, 191, 252 to 253 | 4.1: one `key_name(pad, button)` layer fed by `wc_pad_name`, keyboard names when the host says Keyboard, both when it says nothing |
| P3 | No dealt or taken distinction, no hit tick, no glancing reason | main.c:335, 349, 354; fx.c:153 | 4.4 |
| P4 | Weapon readiness is a ring with no key and no READY; no input kind | draw.c:445 to 494 | 4.2 |
| P5 | Spinner wall hit is silent about its cause | sim.c:1047, main.c:356 | 4.4 reason word `WALL` and the coach beat 2 line |
| P6 | Weapon tells make no sound (hammer wind up, flipper and spike fire, saw toggle, boost, self right) | main.c drain has no EV_WEAPON_TELL, EV_BOOST, EV_SELFRIGHT; fx.c:218 | 4.11: the events already exist; the audio lens owns the sounds, this doc lists the hooks |
| P7 | DRUM self right prompt says B; the key is A and needs spin over 0.5 | draw.c:297, 312; sim.c:1526 | 4.6 |
| P8 | Attract tip `LET GO TO KEEP SPIN` is false | content.h:46, sim.c:641 | 4.3 replaces the tip text |
| P9 | Chevron has no label and no distance; a new player drives at the wall | draw.c:257 to 293; probes 2.2 and 2.3 | 4.5 adds the enemy's name chip to the chevron and the coach beat 1 says TURN TOWARD THE ARROW |
| P10 | The garage cannot tell you what a weapon does or how to use it | parts.h:101 to 114, draw_menus.c:559 | 4.9 |
| P11 | The keyboard line on the controls card is tied to `seat == 0`, not to the keyboard pad; absent in quadrants and in the lobby's compact card | draw_menus.c:1144 to 1151 | 4.8 |
| P12 | Boost has no name on screen and no sound | draw.c:543 to 549 | 4.7, 4.11 |
| P13 | Brake is invisible | sim.c:564 | 4.7: a one shot chip when B is pressed at speed, plus the coach's optional fourth line |

## 4. The design

### 4.1 Key names: one layer, three outcomes

A new file pair `src/keys.h`, `src/keys.c` (not headless; lives with draw), owned by the HUD agent.

```c
/* keys.h: the name a pad's button has on this pad, for every keycap in the game */
enum { KN_A, KN_B, KN_X, KN_Y, KN_L, KN_R, KN_START, KN_SELECT, KN_DPAD, KN_STICK, KN_RT, KN_LT, KN_COUNT };
enum { KIND_PAD, KIND_KEYBOARD, KIND_UNKNOWN };
void        keys_poll(void);                        /* once a second from main.c: wc_pad_name per pad, 32 byte buffer */
int         keys_kind(int pad);                     /* KIND_* */
const char *key_name(int pad, int which);           /* "Z", "A", "Z OR A"; a static per (pad, which) */
int         key_cap(int x, int y, int pad, int which, uint32_t fill, uint32_t ink);   /* hud_keycap with the name; a KIND_UNKNOWN pad draws two caps "Z" "A" with 4 px between them and returns the total width */
int         key_cap_seat(int x, int y, int seat, int which, uint32_t fill, uint32_t ink);   /* the seat's pad, or KIND_UNKNOWN for an AI seat */
```

Rules:

- `keys_kind`: `wc_pad_name` returns a string starting with `KEYBOARD` (case insensitive) means KIND_KEYBOARD; any other non empty string means KIND_PAD; empty means KIND_UNKNOWN. The web page sets `name: 'Keyboard'` on pad 0 whenever `keyBits` is set and no gamepad occupies slot 0 (index.html:129 to 131, one line). The libretro core's answer is not in this repo; until it is confirmed on the station, the station is KIND_UNKNOWN and the fallback is the design, not an error.
- Names per kind. KIND_PAD: `A B X Y L R START SELECT`, `D PAD`, `STICK`, `RT`, `LT`. KIND_KEYBOARD: `Z X A S Q W ENTER SHIFT`, `ARROWS`, `ARROWS`, and RT and LT are `Z` and `Q` (the keyboard has no triggers, so a prompt that says `A OR RT` says `Z` on the keyboard). KIND_UNKNOWN: `Z OR A`, `X OR B`, `A OR X`, `S OR Y`, `Q OR L`, `W OR R`, `ENTER OR START`, `ARROWS OR STICK`. Two caps, keyboard first, because on the station the keyboard is pad 0 and a pad player already knows their letters.
- A key name on the HUD is never the only cue: the widget (4.2) also draws the glyph and the state word, so a wrong guess from KIND_UNKNOWN costs one press.
- Every `hud_keycap(...,"A"...)` and `key_hint(..."A"...)` call in draw_menus.c and draw.c goes through `key_cap_seat` or `key_cap`: title (201 to 203, pad = the title owner or 0), lobby card (247, 274, 281, 315 to 316, 336), garage footer (564 to 573, 624 to 633), rules flash (1201, 1259, 1337 to 1339), pause close (draw.c:1373), judges and results `HOLD A` chips, `CARD_CALLOUT_SUB` (4.8). `content.h` strings that embed a letter (`S_RIGHT_YOURSELF`, `S_FILL_HINT`, `S_GARAGE_HINT_1`, `S_AI_TOGGLE_HINT`, `S_HOLD_A_2S`, `S_HOLD_A_QUIT`, `S_HOLD_B_WIPE`, `S_HOLD_B_LEAVE`, `CARD_LINE`, `ATTRACT_TIPS[0]`, `ATTRACT_TIPS[1]`, `FLASH_CAPTION[3]`) become `%s` formats or lose the letter and get a drawn keycap beside them. The lint keeps checking lengths; a `%s` of `ENTER OR START` (14) is the longest substitution, so buffers grow by 14 where a letter was.

### 4.2 The weapon readiness widget

Replaces `gauge` (draw.c:445 to 494) in both strip sizes. Box 96 x 56 in halves and full at (470, 8) relative to the strip, 72 x 40 in quadrants at (400, 8); the speed ticks move right by 52 (halves) and 36 (quadrants); the status word under the ring moves into the widget. The strip is already 72 px (56) tall, so nothing grows.

Layout, halves: a 40 px ring (36 in quadrants) at the left of the box, the weapon glyph at its centre, a keycap of 24 px to the right of the ring at the top, and a state word in `F_MONO_S` under the keycap. The keycap is the weapon key (`key_cap_seat(seat, KN_A)`) for every weapon but WEDGE and NONE, and the drive key (`KN_STICK`) for the ram. In quadrants the keycap and the word are 14 px and share a line.

Per weapon, from the sim fields in 6.1:

| Weapon | Ring | Keycap | Word, colour | Notes |
|---|---|---|---|---|
| HORIZONTAL, DRUM, MELTY | spin 0..1; the first half of the ring is steel until spin passes `SPINNER_HIT_MIN` 0.5, then the ring is live; a 2 px rust tick at 0.5 marks the bite line | A | `HOLD` in ash under 0.5, `SPIN UP` in hazard from 0.5 to 0.95, `READY` in live at 0.95 and over, `SPENT` in rust for 20 frames after a landed hit drops spin below 0.5 | the pulse at 0.97 stays; the world ring (world.c:1585) stays |
| SAW | spin; a 2 px rust tick at 0.5 | A | `TAP: ON` in ash while off, `ON` in live while on and over 0.5, `SPIN UP` in hazard while on and under 0.5 | the tick replaces the toggle ambiguity |
| FLIPPER | cooldown unwinding (rust) then full live | A | `TAP` in live when `cooldown == 0`, `n.n S` in rust while cooling (108 frames at 60 Hz) | |
| HAMMER | as FLIPPER; while `windup > 0` the ring is hazard | A | `TAP` live, `SWING` hazard during wind up and active, `n.n S` rust cooling | 72 frames |
| SPIKE | as FLIPPER | A | `TAP` live, `n.n S` rust | 60 frames |
| CRUSHER | as FLIPPER; `CLAMP` hazard while `clamp > 0`, `HOLD n S` while `lock > 0` | A | `TAP` live | 120 frames |
| LIFTER | lift 0..1 fills the ring; live when `lift > 0.3` (the hitbox line, sim.c:519) | A | `HOLD` ash, `LIFT` live, `CARRY n.n S` live while `carry_t > 0` (max 180 frames) | |
| FLAME | fuel 0..1 as the ring; rust under 0.1 | A | `HOLD` ash with fuel, `EMPTY` rust at 0, `REFILL` hazard while rising from 0 | tank removed; the ring does both |
| WEDGE, NONE | closing meter: `ram_closing` (6.1) over 1.5 fills the ring live; `RAM_CD` unwinds it in rust after a ram | STICK | `RAM` in ash when nothing is close, `DRIVE AT THEM` in hazard when the enemy is within 3 u and the nose is within 30 degrees, `HIT` live for 20 frames after an EV_RAM, `n.n S` rust during the cooldown | the only attack this build has gets a widget like every other |
| secondary | a 16 px glyph at the box corner as today (draw.c:490 to 493), live while its own state is ready | | | |

Colours: ring base steel, fill in the seat colour below the ready line and live at ready, rust for cooling and spent, hazard for a transient (wind up, spin up, refill). The word's colour follows the ring.

When inverted and the weapon has `inv_operate == 0`: the ring goes smoke, the word is `INVERTED`, and the self right prompt (4.6) takes over.

### 4.3 The attack coach: three beats in the first fight

A per save, one time coach, `SAVE.coach_done` (a new `uint8_t` next to `rules_seen`, game.h:51; `save_defaults` zeroes it; the hall of fame wipe does not touch it, settings RESET SAVE does). It runs only for a human seat in `PH_FIGHT`, only in the first fight after a fresh save (`SAVE.matches == 0` at the horn), only on the first human seat (the keyboard on the station), and never in KO BY FLIP or a 3 or 4 player match (quadrants have no room). It lives in game_flow.c (`coach_update`, called from `fight_update`) and draws in draw.c (`coach_draw(vp, seat)`) as a 24 px tape topped chip 360 x 64 above the strip, centred, char at alpha 220, the beat's line in `F_MONO` bone with a keycap from 4.1 and a live tick when done.

| Beat | Line | Done when | Max |
|---|---|---|---|
| 1 | `DRIVE WITH {ARROWS or STICK}. TURN TOWARD THE ARROW` (the chevron turns live and grows to 32 px while this beat is up) | `drive_t > 0.5` for 60 frames and the enemy is inside the viewport (`!cam_enemy_offscreen`) for 30 frames | 12 s, then it gives up and the next beat shows |
| 2 | by weapon: spinner `HOLD {Z} TO SPIN UP. KEEP OFF THE WALL`; SAW `TAP {Z}: SAW ON`; FLIPPER HAMMER SPIKE CRUSHER `TAP {Z} WHEN THEY ARE CLOSE`; LIFTER `HOLD {Z} UNDER THEIR SIDE`; FLAME `HOLD {Z}. IT IS SHORT`; WEDGE NONE `DRIVE INTO THEIR SIDE. SPEED IS THE WEAPON` | spinner: spin over 0.5; tap weapons: one fire (`active > 0` or `windup > 0` or `clamp > 0`); LIFTER: lift over 0.3; FLAME: fuel fell; ram: `ram_closing > 1.5` once | 15 s |
| 3 | `HIT THEIR SIDE, NOT THE FRONT` with the weak side indicator (4.5) forced on | one landed EV_HIT or EV_RAM by this seat with `zone >= 1.0` | 20 s, or the first landed hit of any zone ends it with `FRONT. TRY THE SIDE` for 2 s |

Each beat is dismissed on its done condition with a 0.3 s live tick and `SFX_MENU_OK` low, never by a button (the card and the pause are the player's; this is the game's). The beats pause with the pause menu and are skipped by a KO. After beat 3 the coach writes `SAVE.coach_done = 1` and commits. A second human in the same fight gets nothing (two chips in halves would cover both strips).

The attract tip `HOLD A TO SPIN UP. LET GO TO KEEP SPIN.` becomes `HOLD {Z} TO SPIN UP. A HIT SPENDS THE SPIN.` and `B RIGHTS A FLIPPED HAMMER OR FLIPPER.` becomes `{X} RIGHTS A FLIPPED HAMMER OR FLIPPER.`; both through 4.1 with pad 0's kind. Rules card 2's keycap and card 4's keycap use `key_cap(..., 0, KN_A)` and `KN_B`.

### 4.4 Hit markers

Three things, all driven from the drain (main.c:302 to 339) with the event's `a`, `b`, `zone` and the new `reason` (6.1):

1. **Landed.** For the attacker's own viewport only: the number is drawn in the attacker's seat colour (`number_all` gains an `attacker_slot` and `damage_number` a per viewport colour: the viewport whose seat is the attacker gets the seat colour, the victim's gets rust, any other viewport bone). A 10 px live tick appears for 20 frames at the right end of the attacker's HP bar (strip x 452, y 26) and the weapon widget's word reads `HIT` for the same 20 frames. The number stays at the contact point (`e->x, e->y + 0.6, e->z`) as today.
2. **Taken.** The victim's viewport gets a 12 px rust frame inset 0 px (a `hud_frame` on the viewport rect, alpha 200 falling to 0 over 18 frames) for every EV_HIT, EV_RAM, EV_HAZ_HIT and EV_SPIN_WALL where the victim is a human seat; the huge band keeps its white on top. The viewport white at huge and clash stays as it is. This is the "why did I take damage" answer together with the reason word below.
3. **Glancing, and why.** When the attacker is a human seat and the hit's `zone < 1.0` or `reason != 0`, a 2 s word rides under the number in `F_MONO_S` rust, from the sim's reason (6.1): `FRONT ARMOUR` (zone 0.6 on a box front plate), `WEDGE` (zone 0.5 on a WEDGE chassis front, or an attacker's ram that was deflected by the victim's wedge hop), `SPINNING DOWN` (a spinner hit with `spin_or_1 < 0.75` at contact, which is every second hit after a bleed), `SHELL` (times 0.8), `ARMOUR` (the victim's reduction is 0.35 or over, the number was under half the raw), `WALL` (EV_SPIN_WALL, drawn in the attacker's own viewport since there is no victim). When the hit is on an exposed zone (1.5, 2.0) the word is `EXPOSED` in live instead, which teaches the flip and lift follow up. One word, the first that applies in that order.

The attack coach's beat 3 reads these reason words to say `FRONT. TRY THE SIDE`.

### 4.5 The weak side indicator on the enemy

A world anchored marker drawn per viewport (like the numbers, through `project` draw.c:98) on the enemy bot's weakest side: a 20 px live chevron (`hud_chevron`) 0.5 u out from the enemy's body on the side with the lowest armour panel state and the highest zone, pointing at it, 2 Hz pulse. The side is chosen from the victim's `panel_state` (sim.h:74) and `hit_zone` logic without calling the sim: rear or the side whose `panel_state` is lowest (a dented panel is weaker, state 4 is gone); never the front (0.6) and never the wedge front (0.5). When the enemy is exposed (`lifted_by`, airborne over 0.15, inverted against a hammer, drum or spike, or a spun down spinner), the chevron goes hazard and points at the underside or the disc, and the word `EXPOSED` sits on it for 1 s.

When it shows: for the whole fight on the evenings where `SAVE.matches < 6` ("the first evenings"), for 2 s after every glancing marker with a reason, during coach beat 3, and whenever Y is held (the card already shows under Y; the indicator joins it). Humans only, their own viewport only, never in the shared ONE CAMERA view (two of them would point at both bots) except under Y for that seat.

### 4.6 The self right prompt

`countout` (draw.c:311 to 312) becomes `self_right_prompt(b, seat)`:

| Build | Prompt | Source |
|---|---|---|
| HAMMER, FLIPPER | `{X} TO RIGHT YOURSELF` with a drawn keycap `key_cap_seat(seat, KN_B)`; after the press `RIGHTING` hazard for the 90 frames | sim.c:1525 |
| DRUM | spin under 0.5: `HOLD {Z} TO SPIN, THEN TAP IT`; spin over 0.5 and the button held: `LET GO, TAP {Z}`; released: `TAP {Z}`; after the press `RIGHTING` for 45 frames, and `SLIPPED` rust for 1 s on the 20 percent fail | sim.c:1526, 1520 |
| WEDGE, NONE, SAW, SPIKE | `DRIVES UPSIDE DOWN. KEEP MOVING` (inv_drive 1; the count does not run while moving, sim.c:1491 to 1492) | parts.h input column |
| HORIZONTAL | `UPSIDE DOWN. SPINS, CANNOT STEER` and in a 1v1 `OPS COMES AT 5` | sim.c:621 |
| LIFTER, CRUSHER, FLAME | `INVERTED. NO SELF RIGHT` or `INVERTED. OPS COMES AT 5` as today | draw.c:312 |
| SHELL on landing | nothing; the auto roll is 50 percent and instant | sim.c:1243 |

The prompt sits where it does now under the numeral; the keycap replaces the letter in the string. The garage warning (draw_menus.c:525) keeps its text.

### 4.7 Boost and brake prompts

- The boost bar (draw.c:543 to 549) gains the word `BOOST` in `F_MONO_S` ash at its left (x 530, y 58) and a keycap `key_cap_seat(seat, KN_L)` 20 px wide to its right when ready, and the word `BOOST` goes live while `boost > 0`. In quadrants the three pips get a 16 px `Q` or `L` cap to their right. Hidden on TREADS and LEG TRACKS as now, and in their place the word `NO BOOST` in smoke for the first 3 s of a fight so the player stops pressing it.
- A boost prompt: when the enemy is over 6 u away, the bot has been driving toward it for 90 frames and boost is ready, the widget's state word slot blinks `BOOST {Q}` once for 1.5 s, at most once per 20 s.
- Brake: a 20 frame bone chip `BRAKE` at the strip's right end on every B edge taken at speed over 1.5 u/s (sim.c:564 sets `brake = 18`; `brake > 0` is the trigger, 6.1). A brake prompt `{X} BRAKES` appears once when the bot is within 2 u of an open edge or a pit tell zone (the `in_tell_zone` test, draw.c:1258, extended with THE ROOF and THE DOCK open edges) at over 2 u/s, at most once per 20 s.

### 4.8 The controls card that matches the bindings

`controls_card` (draw_menus.c:1123 to 1154) keeps its pad outline and six callouts for a KIND_PAD seat. For KIND_KEYBOARD it draws a keyboard outline instead: a 300 x 110 bone frame with the arrow cluster at the right and the six keys `Z X A S Q W` as 24 px caps on the left and `ENTER` long cap below, callouts DRIVE `ARROWS`, WEAPON `Z`, BRAKE `X`, BOOST `Q`, TAUNT `A`, PAUSE `ENTER`, and a second line `W TEST FIRES IN THE GARAGE. S HOLDS THIS CARD`. For KIND_UNKNOWN it draws the pad outline with every callout's sub line as `Z OR A` and so on. `CARD_CALLOUT_SUB` becomes a per kind table: `CARD_SUB_PAD[6]` `{ "STICK OR D PAD", "A OR RT", "B", "L OR LT", "X", "START" }`, `CARD_SUB_KEYS[6]` `{ "ARROWS", "Z", "X", "Q", "A", "ENTER" }`, `CARD_SUB_BOTH[6]` `{ "ARROWS OR STICK", "Z OR A", "X OR B", "Q OR L", "A OR X", "ENTER OR START" }`. `CARD_KEYS` (content.h:93) is deleted; `CARD_LINE` becomes `FLIPPED? %s RIGHTS YOU.  STUCK? OPS IS COMING.` with the B name.

Extra rows on the full card, under the line: `WEAPON: ` then the seat's build's one line from 4.9, so the card answers "how do I attack" for this bot.

Where it shows: on the first join (2 s now; make it 3 s as the spec's 10.24 says, `FIRST_CARD_S` game_flow.c:37), on Y or S held in the garage and the fight (game_flow.c:956, 1176), from the pause menu (draw.c:1370), as the last rules card with the keyboard variant when pad 0 is KIND_KEYBOARD. The compact card (quadrants, the lobby) gets the same four callouts with the right names.

### 4.9 The garage says what a weapon does

A new table in parts.h beside `WEAPONS`, read by the garage's flavour slot and by the controls card:

```c
/* one line per weapon: input kind, what it does, reach in units; under 44 chars for the slot (3.1) */
static const char *WEAPON_HOWTO[WP_COUNT] = {
  "NO WEAPON. RAM THEIR SIDE AT SPEED",            /* NONE */
  "PASSIVE. RAM THEIR SIDE AT SPEED. REACH 0.6",   /* WEDGE: hb_z1 0.6 */
  "TAP. A JAB STRAIGHT AHEAD. REACH 1.0",          /* SPIKE: hb_z1 1.05 */
  "HOLD. GETS UNDER A TURNED BOT. REACH 0.8",      /* LIFTER */
  "TAP TO TOGGLE. GRINDS ON CONTACT. REACH 0.7",   /* SAW: circle at 0.7 */
  "HOLD. SHORT CONE, FUEL RUNS OUT. REACH 1.5",    /* FLAME */
  "TAP. 15 FRAME WIND UP, THEN DENTS. REACH 1.0",  /* HAMMER */
  "TAP. THROWS THEM. 1.8 S TO RELOAD. REACH 0.9",  /* FLIPPER: cooldown 108 */
  "HOLD TO SPIN. POPS THEM UP. REACH 0.6",         /* DRUM */
  "HOLD TO SPIN 2 S. HITS SIDEWAYS. REACH 0.7",    /* HORIZONTAL: 0.15 + 0.55 */
  "TAP. CLAMPS FOR 2 S, 3 OR 4 PLAYERS ONLY. REACH 0.9", /* CRUSHER */
};
static const char *MELTY_HOWTO = "HOLD TO SPIN. THE WHOLE BODY HITS. REACH 0.5";
```

The reach numbers come from `hb_z1` and `hb_z0 + hb_r` (parts.h:102 to 113) and the MELTY circle (sim.c:506). The lint's blurb length rule covers them (44 is under the 48 hint buffer; the slot draws `F_MONO` at x + 32 across 600 px, draw_menus.c:559, which fits 44 characters).

Where it shows: on the WEAPON row and the 2ND WEAPON row the flavour slot alternates the joke and the howto line, 2 s each (the flavour line already alternates with the flip warning two seconds in four, draw_menus.c:619, so the rhythm exists); when the cursor is on any other row the howto for the current weapon shows for 2 s after every weapon change (so a preset tap that lands on CHOP says HOLD TO SPIN). On the lobby seat card the howto's first word (`TAP`, `HOLD`, `PASSIVE`, `TAP TO TOGGLE`) sits after the preset label. On the results card nothing changes.

### 4.10 The first evening's default build

`game_seat_join` (game.c:52) gives seat i preset i and `save_defaults` (game.c:33) seeds `fav_build[i]` the same way. Change both so seat 1 of a fresh save starts on LAUNCH (preset 1, the flipper: a tap weapon with a visible launch, a boost, and the self right that the rules card teaches) and seat 2 on ROLL OUT; seats 3 and 4 stay CHOP and STOMP. The practice bot's pick already avoids the human's chassis and weapon, so the first solo fight becomes LAUNCH against something else. `tools/scripts/keyboard-solo.json`, `ks-medium.json`, `idle-start.json`, `menus.json`, `2p-fight.json`, `4p-fight.json` and the others that start from the seat presets change their recorded builds and hashes; the fight fixtures are regenerated with `build/balance --make-fight` as HANDOFF says.

The widget for ROLL OUT (4.2's RAM row) is still required: a player who picks ROLL OUT on purpose must see when the ram is armed.

### 4.11 Audio hooks this design needs (owned by the audio lens)

No new interface from audio; these are the events already in the queue that the drain (main.c:342 to 411) should hand to `audio_play_var` so the widget's state changes have a sound: `EV_WEAPON_TELL` with `zone 0` (hammer wind up, flipper fire; sim.c:672, 688) and `zone 1` (hammer miss; sim.c:683), `EV_BOOST` (sim.c:562), `EV_SELFRIGHT` with `zone 0` (sim.c:1521) and `zone 1` (the SHELL roll; sim.c:1243), and a new `EV_SAW_TOGGLE` (sim.c:666) and `EV_SPIN_READY` when a spinner crosses 0.5 upward (sim.c:663, 649). The whine (`audio_whine`, game_flow.c:1153) already follows spin.

## 5. Binding audit

Keyboard letters are index.html:106 to 107; pad bits are sim.h:21 to 34; "RetroArch keyboard" is pad 0 with d-pad bits and the eight buttons, no sticks, no triggers (HANDOFF Platform facts; `STATION=1` in the rig).

| Action | Pad | Keyboard (browser) | RetroArch keyboard | Shown on screen as | Mismatch and fix |
|---|---|---|---|---|---|
| drive | left stick or d-pad (main.c:178) | arrows | d-pad bits | `STICK OR D PAD` | fix: `ARROWS` on a keyboard pad (4.1) |
| drive, TANK | both sticks; d-pad both sides (main.c:182 to 185) | arrows both sides | same | `X ARCADE` / `X TANK` in the lobby | the X cap reads as the X key, which is pad B: hold 1 s leaves the lobby (game_flow.c:739 to 742). Fix: `key_cap_seat(KN_X)` draws `A` for a keyboard |
| weapon | A or RT (sim.c:544) | Z | Z (no RT) | `A OR RT` | fix: `Z` for a keyboard; `Z OR A` unknown |
| boost | L or LT (sim.c:546) | Q | Q (no LT) | `L OR LT` | fix: `Q` |
| brake, self right | B (sim.c:564, 1525) | X | X | `B` | fix: `X`; the DRUM prompt is wrong on every kind (4.6) |
| taunt, RANDOM BOT, scheme toggle | X (sim.c:565; game_flow.c:989, 735) | A | A | `X` | the trap of P2; fix 4.1 |
| controls card, preset tap, FILL WITH BOT modifier, HOW IT WORKS | Y (game_flow.c:951 to 960, 715 to 722, 1176) | S | S | `Y` | fix: `S` |
| pause, lock and go, ready and go, vote, skip | Start (game_flow.c:1171, 998, 738, 1070, 1353) | Enter | Enter | `START` | fix: `ENTER` |
| test fire | RT edge or R edge (game_flow.c:964) | W | W | not shown in the garage footer | fix: add `W TEST FIRE` (`key_cap_seat(KN_R)`) to the footer when the row is WEAPON or 2ND WEAPON |
| camera nudge | right stick (game_flow.c:1178 to 1180) | none | none | not shown | fine; nothing to fix |
| turntable scrub | right stick (game_flow.c:962) | none | none | not shown | fine |
| practice bot level | right stick on PAINT, or Y plus Left and Right (game_flow.c:977, 955) | S plus arrows | S plus d-pad | `HOLD Y, LEFT RIGHT SETS IT` | fix: `HOLD S, LEFT RIGHT SETS IT` through 4.1 |
| hazard seat | stick picks, A fires (game_flow.c:1190 to 1192) | arrows, Z | d-pad, Z | `STICK PICKS A HAMMER`, `SWING` | fix: `ARROWS PICK A HAMMER` for a keyboard |
| OPS crew (final) | driver's left stick, gunner's A and X (game_flow.c:1196 to 1197) | arrows, Z, A | d-pad, Z, A | nothing on screen says it | add a `YOU DRIVE OPS` / `YOU RUN THE SAW` chip in the hazard seat view with the keys; out of this lens's scope to draw but the strings go in content.h now |
| rules flash | A next, B or Start skip (game_flow.c:1352 to 1353) | Z, X, Enter | same | `A NEXT`, `B START SKIP` | fix through 4.1 with pad 0's kind |
| settings STICKS row | Left and Right toggle `SAVE.scheme[seat or pad]` (game_flow.c:1309 to 1314) | same | same | the row | fine |
| quit to lobby from pause | hold A 1 s (game_flow.c:1139 to 1141) | hold Z | hold Z | `HOLD A` | fix: `HOLD Z` |
| Select | nothing (12) | right Shift | | | fine |

Two further mismatches that are not letters:

- The sim's dead zone is radial 0.18 on the left stick (sim.c:539) but menu nav uses a 0.5 axis threshold (main.c:196 to 199), and `read_pads` only substitutes the d-pad when the stick is inside the dead zone (main.c:178). A pad with a drifting stick at 0.2 never gets its d-pad honoured in a fight. Fix: substitute whenever any d-pad bit is down, since a player pressing the d-pad means it.
- `pause_close` (game_flow.c:1113) copies the pads into `prev_in` so the closing B is not a brake; `title_select` and `join_seat` clear edges for the joining button (game_flow.c:262, 859). The garage's first join card is 2 s (`FIRST_CARD_S`) against the spec's 3 s; the lobby card covers the turntable and the name but `A READY` stays visible (draw_menus.c:356). Keep 3 s.

## 6. Interfaces

### 6.1 From the sim (sim.h, owned by the sim agent; additive, no signature changes)

New read only fields on `bot_t` (sim.h:64 to 90), written in sim.c, read by draw.c and the drain:

```c
  /* HUD readiness (controls pass): written every step, never read by the sim */
  float   ram_closing;        /* WEDGE and NONE: the closing speed toward the nearest enemy along my nose this step, 0 when the nose is off by over 30 degrees (ram_check's test, sim.c:957 to 959) */
  int16_t hit_flash_t;        /* 20 after this bot lands any EV_HIT or EV_RAM, counting down */
  int16_t spent_t;            /* 20 after a landed spinner hit dropped spin below SPINNER_HIT_MIN */
  uint8_t weapon_ready;       /* 1 when the weapon can bite now: spin > SPINNER_HIT_MIN, cooldown 0 and not winding, lift > 0.3, fuel > 0, saw on and over 0.5, ram armed (ram_closing > RAM_MIN_CLOSING and hit_cd clear) */
  uint8_t can_self_right;     /* 1 for SR_B builds, and for the DRUM only while spin > 0.5 */
  uint8_t self_right_key;     /* 0 none, 1 B, 2 A (the DRUM) */
  uint8_t last_hit_reason;    /* the HIT_WHY_* of the last hit this bot landed */
```

New constants in sim.h beside the band edges, so draw.c and main.c share them with the headless tool:

```c
/* why a landed hit was weak or strong (event_t.zone carries the zone times 10 already; this is the word) */
enum { HIT_WHY_NONE, HIT_WHY_FRONT, HIT_WHY_WEDGE, HIT_WHY_SPINNING_DOWN, HIT_WHY_SHELL, HIT_WHY_ARMOUR, HIT_WHY_WALL, HIT_WHY_EXPOSED, HIT_WHY_COUNT };
#define SPINNER_HIT_MIN_PUBLIC 0.5f    /* sim.c's SPINNER_HIT_MIN, exported for the ring's bite tick */
```

`event_t` (sim.h:100) gains `uint8_t reason;` after `zone` (the struct is zeroed by `ev_simple` and `ev_hit`, so every existing site stays correct). `weapon_hit` (sim.c:744 to 747) sets it from `zone`, `spin_or_1`, the victim's chassis and armour; `ram_check` (sim.c:962) sets `HIT_WHY_WEDGE` when the victim's wedge hop fires against the attacker (the attacker is the one who got nothing); `walls_step` (sim.c:1047) sets `HIT_WHY_WALL` on EV_SPIN_WALL. Priority: EXPOSED (zone 1.5 or 2.0), WALL, WEDGE (zone 0.5), FRONT (0.6), SPINNING_DOWN (`spin_or_1 < 0.75`), SHELL, ARMOUR (reduction 0.35 or over), NONE.

`sim_status_word` (sim.c:2064) is unchanged; the widget derives its words from the fields above and `STATUS_WORD` keeps serving the compact strip's colour.

A new event `EV_SPIN_READY` (a spinner or MELTY crossing 0.5 upward; sim.c:663 and 649) and `EV_SAW_TOGGLE` (sim.c:666) at the end of `ev_type_t` (sim.h:94 to 97), so the hash and every existing replay stay identical (events are not hashed, sim.c:2008 to 2018).

`balance --replay` and `--hashcheck` must still match the cart after these: nothing above changes a float the sim integrates; `ram_closing` and the timers are written from values the step already computes.

### 6.2 From the host (main.c, owned by integration)

- `keys_poll()` once a second in `wc_render` after `read_pads`; it calls `wc_pad_name` for the four pads with a 32 byte buffer and keeps `keys_kind`. Zero cost when the import is absent (the weak default returns 0).
- index.html: `name: 'Keyboard'` on `res[0]` when `keyBits` is set and `gps[0]` is absent (the keyboard shares slot 0 with a pad; a pad present wins and the player sees pad letters, which is right).
- `tools/shot.mjs`: a `"name": "Keyboard"` entry on a script's pad 0 is passed through as `name`, so the rig can shoot the keyboard variant; `STATION=1` without it stays KIND_UNKNOWN, which is the station until the core is checked.

### 6.3 HUD (draw.c, draw_menus.c, hud.h, owned by the HUD agent)

- `keys.h` as in 4.1.
- `weapon_widget(x, y, w, h, const bot_t *b, int seat, int compact)` replaces `gauge`.
- `hit_marks_draw(viewport, vp, dt)` draws the taken frame and the reason words from a ring like `NUMS` (draw.c:118 to 156), fed by `hit_mark_add(viewport, kind, reason, x, y, z, colour)` from the drain.
- `weak_side_draw(viewport, vp, seat)` after `chevron` in `fight_frame` (draw.c:1300).
- `coach_draw(vp, seat)` before the strip.
- `self_right_prompt` replaces the prompt lines of `countout` (draw.c:311 to 312).
- `damage_number` gains a colour per viewport: `number_all(x, y, z, value, attacker_slot, victim_slot, big)` in main.c picks the seat colour, rust or bone per viewport.

### 6.4 Content (content.h, owned by content)

New strings, all under the existing buffers: `WIDGET_WORD[]` `{ "HOLD", "SPIN UP", "READY", "SPENT", "TAP", "TAP: ON", "ON", "SWING", "CLAMP", "LIFT", "CARRY", "EMPTY", "REFILL", "RAM", "DRIVE AT THEM", "HIT", "INVERTED", "NO BOOST", "BOOST", "BRAKE" }`; `HIT_WHY[]` `{ "", "FRONT ARMOUR", "WEDGE", "SPINNING DOWN", "SHELL", "ARMOUR", "WALL", "EXPOSED" }` indexed by HIT_WHY_*; `COACH_1`, `COACH_2_*` per weapon kind, `COACH_3`, `COACH_3_FRONT` as in 4.3 with `%s` for the key; `S_SR_*` the six self right prompts of 4.6; `S_BOOST_PROMPT`, `S_BRAKE_PROMPT`; `CARD_SUB_PAD`, `CARD_SUB_KEYS`, `CARD_SUB_BOTH`; `WEAPON_HOWTO` and `MELTY_HOWTO` in parts.h. The lint rule for blurb lengths covers the howto lines at 44.

### 6.5 Save (game.h)

`uint8_t coach_done;` after `rules_seen` (game.h:51). `SAVE_VERSION` stays 1 only if the field fits in the existing struct size; it does not (every byte is used in order), so `SAVE_VERSION 2` and `save_valid` accepts version 1 by zeroing the new field (a 196 byte save becomes 197; the crc covers it).

## 7. Acceptance tests

Every test is a rig run with a script and a frame to look at, or a replay hash, so the integrator can run it without a person.

1. **Key names.** `node tools/shot.mjs ... 111,180 tools/scripts/keyboard-solo.json` with the script's pad 0 given `"name": "Keyboard"`: the garage footer at 111 reads `Z LOCK IN`, `ENTER FIGHT`, `A RANDOM BOT`, `S PRESET . HOLD: CONTROLS`, `W TEST FIRE`; the same run without the name reads `Z OR A LOCK IN` and so on; `2p-fight.json` with pad 1 named `Xbox Wireless Controller` reads `A LOCK IN` on seat 2's panel. The lobby card at `menus.json` 100 reads `Z READY` and `A ARCADE` for a keyboard pad.
2. **No destructive misread.** In the keyboard variant of test 1, pressing the A key in the garage at 150 rolls RANDOM BOT (unchanged behaviour) but the footer never showed `A LOCK IN`; grep the frame's strings in a `RTR_FAKE_MENUS` build or compare the PNG to the fixture.
3. **Widget states.** `build/probe-chop.json` (below) at 700: ring empty, cap `Z OR A`, word `HOLD`; 745 (spin 0.37): word `HOLD`, steel half; 790 (spin 0.75): word `SPIN UP` hazard, the ring past the rust tick; 850: `READY` live, the 0.97 pulse; 1100 after the wall: `SPENT` rust for 20 frames then `SPIN UP`, and the reason word `WALL` under the rust 15 in the player's viewport. `build/probe-rollout.json` at 700: cap `ARROWS OR STICK`, word `RAM` ash; a frame where the enemy is within 3 u ahead (drive a loop toward it: add `LEFT` 60 frames at 760, then `UP`): `DRIVE AT THEM` hazard; after an EV_RAM `HIT` live then `n.n S` rust for 90 frames.
4. **Flipper, hammer, spike, crusher words.** `keyboard-solo.json` (LAUNCH edits) at 1000 to 1003 (the A tap): `TAP` live before, `1.8 S` rust at 1004 counting down to `TAP` at 1112. `4p-fight.json` seat 4 (STOMP) in any frame with `windup > 0`: `SWING` hazard.
5. **Hit markers.** `feel.json` 863 to 870: the attacker's half shows `14` in hazard (seat 1's colour) with a live tick at its bar's end and the widget word `HIT`; the victim's half shows `14` in rust with a rust frame fading over 18 frames; the reason word under the attacker's number is `FRONT ARMOUR` or empty, per the contact's zone (print `e->zone` and `e->reason` with `RTR_TRACE`). `2p-fight.json` between 668 and 3243: at least one frame with `SPINNING DOWN` under a CHOP hit (the fixture has many bleed hits) and one with `EXPOSED` in live.
6. **Weak side.** `2p-fight.json` 900 with a fresh save (`SAVE.matches` 0 under the rig): a live chevron on the enemy's side or rear in each half, none on the front; with `SAVE_IN` of a save whose `matches` is 6: no chevron unless Y is held (`pause.json` 1660 holds Y on pad 1 at 200 in the garage, add a Y hold in the fight at 1500 and shoot 1520).
7. **Coach.** `keyboard-solo.json` on a fresh save: beat 1 chip at 700 (`DRIVE WITH ARROWS OR STICK. TURN TOWARD THE ARROW`, the chevron 32 px live), done when the enemy is in frame (the script drives Up then Left; expect the tick before 900); beat 2 `TAP Z OR A WHEN THEY ARE CLOSE` done at 1000 (the A tap); beat 3 `HIT THEIR SIDE, NOT THE FRONT` ends on the first landed hit; `SAVE_OUT` after the run then `SAVE_IN` into a second run: no chip at 700. `4p-fight.json`: no chip anywhere. `mode-flip.json`: no chip.
8. **Self right.** `build/balance --trace bay LAUNCH LAUNCH 3000` to find a frame where one bot is inverted, then the cart at that frame via `--make-fight`: the prompt reads `X OR B TO RIGHT YOURSELF` with a keycap; the same with a DRUM build (`build/balance --trace bay "DRUM BOX" ...` or a custom build line) reads the three DRUM lines in order as A is held, released and tapped; a LIFTER build reads `INVERTED. OPS COMES AT 5` in a 1v1 as today.
9. **Boost and brake.** `keyboard-solo.json` 700 to 703 (the L tap): `BOOST` live for 48 frames then the rust bar with the `Q OR L` cap returning at 944; `probe-rollout.json` 700 to 703 on TREADS: `NO BOOST` smoke for the first 3 s and nothing after. A B edge at speed (add `B` at 820 to `keyboard-solo.json`): `BRAKE` chip for 20 frames.
10. **Controls card variants.** `pause.json` 1660 with pad 0 named `Keyboard`: the keyboard outline and `Z X A S Q W ENTER` caps, the `WEAPON:` line reading ROLL OUT's howto; unnamed: the pad outline with `Z OR A` sub lines; `menu.json` 525 (the last rules card) the same by pad 0's kind.
11. **Garage howto.** `garage-parts.json` at the WEAPON row frames in `garage-parts.frames`: the flavour slot alternates the joke and the howto every 2 s; each howto line is under 44 characters (`./build.sh lint`).
12. **Default build.** `menus.json` 100 on a fresh save: seat 1's lobby card reads `YEETING BOX` `LAUNCH`, seat 2's `POLITE DOORSTOP` `ROLL OUT`; `build/balance --hashcheck` and the regenerated `2p-fight.json`, `4p-fight.json` replay bit exact against the cart's `[sim] f= h=` lines.
13. **Determinism.** `build/balance --replay tools/scripts/drive-bay.json --setup "<line>"` and the five other laps, `dpad-turn.json`, the regenerated fight fixtures: identical hashes before and after the sim fields of 6.1 (they add no integration).
14. **Dead zone fix.** `dpad-turn.json` with pad 0's `lx` set to 7000 (0.21) for the whole run: the bot still turns on RIGHT under the fix; today it does not (the d-pad is ignored outside the dead zone, main.c:178).
15. **Budget.** `TIMING=1` on `2p-fight.json`: the widget, the reason words and the weak side chevron add under 2 sub image calls a frame and under 3,000 KB/s over the R4 numbers in the `[rig]` lines; the strips are already uploaded as their own rects (HANDOFF Platform facts), so the widget rides inside them.

The two probe scripts used above, for `tools/scripts/`:

`probe-chop.json` (keyboard, `STATION=1`): A at 30, 60, 90; START at 110; Y at 140 and 170 (ROLL OUT to LAUNCH to CHOP); START at 240; A at 300; A held 700 to 1500; UP plus A 760 to 1500. Shots 700, 745, 790, 850, 1100.

`probe-rollout.json`: the same without the Y taps; A held 700 to 760, L at 700 to 703, UP plus A 760 to 1500. Shots 700, 790, 1100.

## 8. Notes for the other lenses

- **Bots that do not register as rigid bodies** (the owner's report), what this lens saw in the collision code while auditing the ram: `bots_collide` (sim.c:1128 to 1213) skips a pair when `|a.y - b.y| > 0.35` (sim.c:1138), so a bot on the DOCK deck, on the FREE PILE mound, or in the air after a wedge hop or a flipper passes through a bot on the floor; `hit_test` uses 0.6 (sim.c:861) so the weapons reach a little further than the bodies. A `lifted_by` or `clamped_by` bot is skipped entirely (sim.c:1132, 1136), so a bot OPS carries ghosts through everyone. A HOVER takes the whole separation and its velocity into the contact is dropped (sim.c:1150 to 1163), which a driver feels as the hover not being there. A dead chassis is a 0.5 mass prop (sim.c:1141) except after a pit or roof KO. Those are the candidates; the physics lens owns the fix.
- **Wedge physics**: the hop (sim.c:969 to 973) fires only on an EV_RAM with damage, once per 90 frames per victim, and only when the attacker's nose is within 30 degrees of the contact normal at over 1.5 u/s closing. A wedge that drives under a bot slowly does nothing. The widget's `DRIVE AT THEM` word (4.2) tells the driver the speed matters; whether the sim should also lift a slow wedge is the physics lens's call.
- **Audio**: section 4.11 lists the events the widget's state changes want a sound on; the drain has none of them today.
- **Models**: the hammer glyph (`GL_HAMMER`) reads as a spike at 16 px in quadrants (`build/shots/f4-900.png`); the widget draws glyphs at 16 px too, so a clearer 16 x 16 hammer bitmap in glyphs.h helps both.
