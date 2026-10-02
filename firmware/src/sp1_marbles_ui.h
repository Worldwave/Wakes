/*
 * wakes-sp1 — the MARBLES pages (M4). Spec: docs/UI-SPEC.md "MARBLES page" and
 * docs/UI-PAGES.md.
 *
 * Pure C, no Zephyr: main.c feeds it fader counts, the "••" state and decoded button
 * actions; it hands back a finished sp1_marbles_params and the routing into Plaits.
 *
 * ---- pages and layers (F1 / F2 / F3 / F4) ----
 *   t page BASE      RATE [A]          t BIAS [D]*       JITTER [C]        DEJA VU [H]*
 *   t page SHIFT     (reserved)        gate length       gate length rand  LENGTH [I]
 *   X page BASE      SPREAD [K]        X BIAS [L]*       STEPS [M]*        DEJA VU [H]*
 *   X page SHIFT     (reserved)        (reserved)        (reserved)        LENGTH [I]
 *   SETTINGS (Y)     Y SPREAD          Y BIAS*           Y STEPS*          Y RATE (12)
 *   * = 10 % centre detent. LENGTH is ONE value shown on both SHIFT pages (Marbles
 *   has one loop-length knob). SETTINGS is the same page from either side.
 *   "••" held = SHIFT of the current page; "••" double-tap latches SETTINGS, a single
 *   tap returns -- the same grammar as the PLAITS page. Pickup (Plaits' catch-up) on
 *   every layer.
 *
 *   ⚠️ X SHIFT F1 and F2 were SCALE and X CLOCK in M4 and are unbound in M4a (Adara):
 *   scale selection moved to "••" + FFWD / RWD, and X keeps Marbles' own clock source
 *   (each X output on its own t output) with no way to change it. Both fields are still
 *   in sp1_marbles_params so M5 can re-expose them; only the faders went away.
 *
 * ---- routing into Plaits (applied only while the clock runs; sp1_synth) ----
 *   t1-t3     one destination each (t page SHIFT T1-T3), the gate read as 0 / +5 V:
 *             TRIG, LEVEL, FM, TIMBRE, MORPH, HARMONICS or none
 *   X1-X3, Y  one destination each (X page SHIFT T1-T3, SETTINGS T1): FM, TIMBRE,
 *             MORPH, HARMONICS, V/Oct, LEVEL or none
 *   Scaling is Plaits' own CV calibration (plaits/settings.cc, per 5 V): V/Oct and FM
 *   60 semitones, TIMBRE / MORPH 1.6, HARMONICS 1.0 -- each through its attenuverter
 *   (HARMONICS has one since M4a), V/Oct unattenuated. Outputs on one destination add
 *   up and the sum is CLAMPED to what one output could do on its own, so stacking
 *   three gates on TIMBRE cannot drive Plaits past its range (Adara, M4a).
 *
 * ---- track row ----
 *   BASE: Marbles' output LEDs -- T1-T3 = t1-t3 gates (t page) or |X1-X3| (X page),
 *   T4 = |Y| always. Moving a fader shows the page's four values for 1.2 s instead.
 *   SHIFT: the SHIFT layer's values. SETTINGS: the Y values, breathing.
 */
#ifndef SP1_MARBLES_UI_H
#define SP1_MARBLES_UI_H

#include <stdbool.h>
#include <stdint.h>
#include "sp1_marbles.h"

enum sp1_mui_page { SP1_MUI_PAGE_T = 0, SP1_MUI_PAGE_X };

enum sp1_mui_layer {
	SP1_MUI_T_BASE = 0,
	SP1_MUI_T_SHIFT,
	SP1_MUI_X_BASE,
	SP1_MUI_X_SHIFT,
	SP1_MUI_Y,               /* the SETTINGS page */
	SP1_MUI_LAYERS
};

/* Every destination a Marbles output can reach. The ORDER a tap cycles them, and the
 * LED glyph, differ between the two sides -- see sp1_mui_dest_pattern() and
 * sp1_mui_t_dest_pattern(). This enum is only the set. */
enum sp1_mui_dest {
	SP1_DEST_NONE = 0,
	SP1_DEST_FM,             /* FM input, x the FM attenuverter (PLAITS SHIFT F1)  */
	SP1_DEST_TIMBRE,
	SP1_DEST_MORPH,
	SP1_DEST_HARM,           /* x the HARMONICS attenuverter (PLAITS SHIFT F4, M4a) */
	SP1_DEST_VOCT,           /* V/Oct: 12 semitones per volt, unattenuated         */
	SP1_DEST_LEVEL,          /* LEVEL (the VCA / accent input)                     */
	SP1_DEST_TRIG,           /* t outputs only                                     */
	SP1_DEST_COUNT
};

struct sp1_mui_routing {
	uint8_t t_dest[3];       /* t1, t2, t3: enum sp1_mui_dest (TRIG by default)    */
	uint8_t dest[4];         /* X1, X2, X3, Y: enum sp1_mui_dest                   */
};

/* Once, at boot: everything to the defaults (see sp1_mui_rip). */
void sp1_mui_init(void);

/* On entering the MARBLES module (T4, or ON while it is the module): pickup is
 * re-evaluated against the faders. `fnc` = "••" is down now (that press is not a tap). */
void sp1_mui_enter(const uint16_t raw[4], bool fnc);

/* One control tick while MARBLES is the module. Same inputs as sp1_pui_tick. */
#define SP1_MUI_EV_PAGE    0x01u     /* SETTINGS latched / released                   */
#define SP1_MUI_EV_LAYER   0x02u     /* active layer changed                          */
#define SP1_MUI_EV_CAUGHT  0xF0u     /* bit 4+i: fader i just caught up               */
uint32_t sp1_mui_tick(uint32_t elapsed_ms, const uint16_t raw[4], bool valid,
		      bool fnc, bool activity);
/* Every tick, whichever module is on show, after sp1_midi_main_tick: MIDI CCs move the
 * stored values themselves when the script's pickup is shared or takeover (sp1_midi.h).
 * Does nothing in sum. */
void sp1_mui_midi(void);

/* ---- buttons (main decodes presses; these apply them and return what changed) ---- */
bool sp1_mui_set_page(enum sp1_mui_page p);        /* T2 / T3; true if it changed    */
/* [E] tap: the next of Marbles' SIX t models. Marbles hides the second three behind a
 * long press of [E]; here they are simply the second half of one ring and there is no
 * long press anywhere in the UI (Adara, M4a). */
void sp1_mui_model_tap(void);
void sp1_mui_diversity_step(void);                 /* [N] on the X page              */
bool sp1_mui_t_range_step(int dir);                /* [B], no wrap; true if changed  */
/* ---- [J]: ONE voltage range for X and Y (M4e, Adara) ----
 * MARBLES SETTINGS page, T4. Wraps through the four ranges in sp1_marbles.h.
 *
 * ⚠️ There used to be two, X's on "••" + T4 of the X page and Y's on SETTINGS T4. Marbles'
 * own [J] is a SINGLE button -- Y's range is that same button with a modifier held -- so
 * two independent settings were never in the hardware being copied, and keeping them cost
 * a button for no expressive gain. "••" + T4 now selects Y's DESTINATION on both pages,
 * which is what that slot is really needed for. */
void sp1_mui_range_step(void);
void sp1_mui_t_dest_step(int t);                   /* 0..2: t1..t3, t SHIFT T1-T3    */
void sp1_mui_dest_step(int out);                   /* 0..2 X1..X3, 3 Y               */
/* [F] (side 0, t) and [G] (side 1, X) on the SETTINGS page: does DEJA VU apply to that
 * section? Both on by default. Off means that side ignores the knob and runs free, which
 * is what Marbles does with an unlit [F] / [G] (Adara, M4b). One knob, two switches. */
void sp1_mui_deja_vu_toggle(int side);
bool sp1_mui_deja_vu(int side);
/* Disconnect every X / Y output routed to V/Oct; returns how many. Called when Plaits'
 * FREQUENCY quantizer is switched on: a quantizer and an external pitch source fighting
 * over the same note is never wanted (Adara, M4b). */
int  sp1_mui_unpatch_voct(void);

/* ---- UNPATCH: "••" held + Tn held 2 s (M4e, Adara) ----
 * Clears ONE output's destination and returns what was there (SP1_DEST_NONE if nothing
 * was, which is a no-op rather than an error). The t page's T1-T3 reach t1-t3, the X page's
 * reach X1-X3, and T4 reaches Y from either page -- the same buttons that CYCLE those
 * destinations on a short press, so the gesture reads as "this button's cable".
 *
 * docs/UI-PAGES.md has the animation; sp1_ui_timing.h has the timing constants and the
 * note about this being the one long press in the whole UI. */
uint8_t sp1_mui_unpatch_t(int t);                  /* t = 0..2                       */
uint8_t sp1_mui_unpatch_x(int out);                /* out = 0..2 X1-X3, 3 = Y        */

/* Clear every output -- t, X and Y -- aimed at one destination. The PLAITS side of the
 * same gesture: there the button names a PARAMETER, so everything pointing at it goes.
 * Returns how many were cleared. */
int  sp1_mui_unpatch_dest(uint8_t dest);

/* The scale table, for the PLAITS SETTINGS page's own scale selector (M4b). Plaits keeps
 * its OWN index into the same seven scales, so the two modules can disagree. */
bool sp1_mui_scale_included(int scale);
void sp1_mui_scale_glyph(int scale, uint8_t lv[4]);
#define SP1_MUI_SCALES 7
/* "••" + FFWD / RWD on the X page: the next / previous INCLUDED scale, no wrap.
 * Returns true if it moved (false at either end). docs/MARBLES-SETTINGS.md owns the
 * table -- which scales exist, their glyphs and which are included. */
bool sp1_mui_scale_step(int dir);
/* "••" + PLAY held 3 s on MARBLES: a **FULL WIPE** of the module (Adara, M4d --
 * docs/DEFAULTS.md is the spec).
 *
 *   routing    -> EVERY destination to none. ⚠️ So PLAY makes no sound afterwards until
 *                 something is dialled back in. That is the point of the gesture's name
 *   every fader-> its default, BASE included (t RATE/BIAS/JITTER, X SPREAD/BIAS/STEPS,
 *                 the shared DEJA VU and LENGTH, the SHIFT pages and the Y page)
 *   buttons    -> coin toss, x1 range, [J] INTELLIGENT, identical, major, [F] and [G] on
 *   kept       -> which page you are standing on, and whether SETTINGS is latched
 *
 * main.c additionally re-seeds the random stream (sp1_marbles_reseed: a NEW DEJA VU loop,
 * not the same one with default settings) and leaves the clock RUNNING -- so its phase
 * jumps once. Both are Adara's calls; see sp1_marbles.h for why re-seeding alone would
 * have been inaudible.
 *
 * ⚠️ The faders do not move; pickup catches them up, as on PLAITS. */
void sp1_mui_rip(void);

/* ---- outputs ---- */
void sp1_mui_params(struct sp1_marbles_params *out);
void sp1_mui_routing(struct sp1_mui_routing *out);
/* The master clock's tempo (RATE and [B]), for the FFWD burst. */
float sp1_mui_bpm(void);
/* Track row, see above. */
void sp1_mui_leds(uint8_t out[4]);
/* The PAGE you are standing on (t, X or SETTINGS), ignoring SHIFT -- see
 * sp1_pui_page_leds(). */
void sp1_mui_page_leds(uint8_t out[4]);

/* ---- flash patterns (0 / SP1_ENGINE_LED_FULL levels; no half levels on this page --
 * Adara, M4a: dimmed glyphs are hard to tell apart in room light) ----
 * ONE glyph vocabulary for both sides (Adara's correction in M4b -- M4a's counting bar on
 * the t page is gone): one LED under the PLAITS fader of that parameter --
 * ●○○○ FM, ○●○○ TIMBRE, ○○●○ MORPH, ○○○● HARMONICS -- plus ○○●● LEVEL and ○○○○ none.
 * ⚠️ ○●●● is V/Oct on the X page and TRIG on the t page: a deliberate collision (Adara),
 * since neither destination exists on the other side, so context disambiguates it and
 * both keep the shortest glyph. docs/UI-PAGES.md spells the table out. */
void sp1_mui_page_pattern(uint8_t out[4]);          /* ●●○○ t, ○○●● X                */
void sp1_mui_dest_pattern(int out, uint8_t lv[4]);  /* X1..X3, Y                     */
void sp1_mui_t_dest_pattern(int t, uint8_t lv[4]);  /* t1..t3                        */
void sp1_mui_model_pattern(uint8_t lv[4]);          /* one of six, docs/MARBLES-SETTINGS.md */
void sp1_mui_scale_pattern(uint8_t lv[4]);          /* default engine glyphs 1-7    */
/* ●●○○ t on · ○○●● X on · ●●●● both · ○○○○ neither -- the page vocabulary, so the glyph
 * says which SIDE rather than needing to be learned (M4b). */
void sp1_mui_deja_vu_pattern(uint8_t lv[4]);
void sp1_mui_range_pattern(int range, uint8_t lv[4]);

/* ---- state, for main and the log ---- */
enum sp1_mui_page  sp1_mui_page(void);
enum sp1_mui_layer sp1_mui_active(void);
bool sp1_mui_settings(void);                        /* SETTINGS latched               */
int  sp1_mui_model(void);
int  sp1_mui_t_range(void);
int  sp1_mui_range(void);                           /* [J], X and Y (M4e)            */
int  sp1_mui_diversity(void);
int  sp1_mui_scale(void);
const char *sp1_mui_layer_name(enum sp1_mui_layer l);
const char *sp1_mui_dest_name(int dest);

#endif /* SP1_MARBLES_UI_H */
