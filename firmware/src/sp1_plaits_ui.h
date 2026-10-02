/*
 * wakes-sp1 — the PLAITS page: fader layers, pickup, centre detents (M3b).
 *
 * Spec: docs/UI-SPEC.md, "PLAITS page" (approved by Adara), plus the centre-detent rules
 * she set on 2026-09-21. Pure C, no Zephyr: main.c feeds it fader counts and the "••"
 * state once per control tick, and it hands the synth a finished sp1_synth_params.
 *
 * ---- three layers on four faders ----
 *
 *   layer      how                        F1            F2          F3            F4
 *   BASE       default                    FREQUENCY     TIMBRE      MORPH         HARMONICS
 *   SHIFT      while "••" is HELD         FM att.       TIMBRE att. MORPH att.    HARMONICS att.
 *   SETTINGS   "••" DOUBLE-TAP (latched)  OCTAVE range  LPG colour  LPG decay     LEVEL
 *              single tap "••" -> BASE
 *   "••" + PLAY held 3 s: SHIFT and SETTINGS back to defaults (sp1_pui_reset_mods)
 *
 * Holding "••" always gives SHIFT, from either page. All FOUR faders there are
 * attenuverters, one under each BASE parameter; with PLAY as the trigger and nothing
 * patched to the CV inputs, Plaits turns them into the depth of its internal decay
 * envelope on pitch / TIMBRE / MORPH -- and, since M4a, HARMONICS as well (Plaits has
 * no HARMONICS attenuverter; firmware/CMakeLists.txt overrides voice.{h,cc} to add
 * one). Every attenuverter is bipolar and defaults to its CENTRE, which is a gain of
 * ZERO: nothing modulates until it is turned up.
 *
 * ---- M4a, Adara's page rearrangement ----
 * FINE TUNE is gone (it only ever acted in one of Plaits' eleven frequency-range
 * modes), the OCTAVE range moved from SETTINGS F4 to SETTINGS F1, and LEVEL moved from
 * SHIFT F4 to SETTINGS F4 to free F4 for the new attenuverter. LEVEL keeps its
 * behaviour: below 5 % it DISCONNECTS level (normal plucked notes); above, the VCA is
 * held open by LEVEL -- the drone (UI-SPEC: the flag is what matters, not the value).
 *
 * ---- pickup: Plaits' own catch-up, lifted from plaits/pot_controller.h ----
 * Each layer keeps its own four values. When a layer becomes active, a fader whose
 * position does not match its stored value is CATCHING: moving it moves the value
 * RELATIVELY, skewed so value and fader converge at the end of travel, and once they
 * meet (within 0.5 %) it TRACKS. Nothing ever jumps. This is POT_STATE_CATCHING_UP,
 * with Plaits' thresholds (0.005 movement, 0.005 match) and skew formula, applied to
 * every layer rather than only on return from the hidden parameter.
 *
 * ---- centre detents ----
 * A band around the fader's centre that reads as exactly the centre value, with the
 * rest of the travel stretched so both ends are still reached. Widths:
 *   - 10 % of travel for every bipolar parameter;
 *   - 5 % for FREQUENCY, in the range modes where its centre means something.
 * WHICH base-layer parameters are bipolar depends on the engine: the detent table in
 * tools/gen_engines.py, derived from the Plaits manual and each engine's source (the
 * reasons are in docs/PLAITS-ENGINES.md).
 * Unipolar parameters use the full 0..100 % with no detent.
 */
#ifndef SP1_PLAITS_UI_H
#define SP1_PLAITS_UI_H

#include <stdint.h>
#include <stdbool.h>
#include "sp1_synth.h"

enum sp1_pui_layer {
	SP1_PUI_BASE = 0,
	SP1_PUI_SHIFT,
	SP1_PUI_SETTINGS,
	SP1_PUI_LAYERS
};

/* Once, at boot: factory defaults for every layer (Plaits' own: attenuverters at
 * centre = gain 0, LEVEL disconnected, LPG colour 0, decay 0.5, full frequency range). */
void sp1_pui_init(void);

/* ---- the ONE place SHIFT and SETTINGS get their values (M4a) ----
 * Three callers: boot, "rip out the cables", and coming back from a module swap. They
 * all go through here so that when M5 makes these persist, loading a stored snapshot
 * is a change to ONE call site rather than a hunt.
 *
 * ⚠️ The faders never seed these layers. Only BASE is seeded from the physical faders,
 * and only on the first ON after boot: an attenuverter that picked up wherever the
 * fader happened to be sitting would start the instrument modulating something the
 * user did not ask for (Adara, from hardware). They start at gain 0 and stay there
 * until a fader is moved WHILE the layer is showing, which pickup then catches. */
void sp1_pui_default_mods(void);
void sp1_pui_load_mods(const float shift[4], const float settings[4]);

/* On every entry to ON. `raw` = current fader counts. Page -> BASE. The base layer
 * TRACKS the faders the first time after boot; after that it keeps its values and any
 * fader moved while the device was off is picked up, not jumped to. */
void sp1_pui_enter(const uint16_t raw[4]);

/* On coming back to the PLAITS module from MARBLES (T4, M4): page kept, pickup
 * re-evaluated against where the faders are now. `fnc` = "••" is down (not a tap). */
void sp1_pui_resume(const uint16_t raw[4], bool fnc);

/* One control tick.
 *   raw[]        fader counts (sp1_fader_raw: never a sagged reading)
 *   valid        this tick's fader sample is trustworthy (sp1_faders_valid)
 *   fnc          "••" is down
 *   activity     any other control touched since "••" went down (a hold with activity
 *                is a shift, never a tap)
 * Returns a bitmask of SP1_PUI_EV_* for the log. */
#define SP1_PUI_EV_PAGE    0x01u     /* BASE <-> SETTINGS                           */
#define SP1_PUI_EV_LAYER   0x02u     /* active layer changed (incl. SHIFT)           */
#define SP1_PUI_EV_CAUGHT  0xF0u     /* bit 4+i: fader i just caught up              */
#define SP1_PUI_EV_LEVEL   0x100u    /* LEVEL connected/disconnected                 */
uint32_t sp1_pui_tick(uint32_t elapsed_ms, const uint16_t raw[4], bool valid,
		      bool fnc, bool activity);

/* What the synth should play, from all three layers. */
void sp1_pui_params(struct sp1_synth_params *out);

/* Track-row LEDs for the ACTIVE layer: each LED shows its parameter's stored value
 * (0..255), so a fader that has not caught up shows where it has to go. SETTINGS
 * breathes, so a latched page is never mistaken for BASE. */
void sp1_pui_leds(uint8_t out[4]);
/* The PAGE you are standing on (BASE or SETTINGS), ignoring SHIFT. For a view that
 * must not be the SHIFT layer while "••" is still held -- the end of a rip (issue #4). */
void sp1_pui_page_leds(uint8_t out[4]);

enum sp1_pui_layer sp1_pui_active(void);
enum sp1_pui_layer sp1_pui_page(void);         /* BASE or SETTINGS */
const char *sp1_pui_layer_name(enum sp1_pui_layer l);
int  sp1_pui_octave_mode(void);                /* 0..10, Plaits' range setting */

/* ---- FREQUENCY scale quantization (M4b, Adara): SETTINGS T2 / T3 ----
 * Restricts the notes F1 can select to one of the scales Marbles' X quantizer offers
 * (docs/MARBLES-SETTINGS.md). -1 = off, otherwise an index into that 7-scale table.
 * Quantization happens in sp1_pui_params, in the MAIN thread, on the note F1 has already
 * produced -- so it composes with whatever OCTAVE range is set rather than replacing it.
 *
 * ⚠️ Two special cases, both deliberate:
 *   - OCTAVE range mode 9 (Plaits' "quantized octaves") becomes a sweep of SCALE DEGREES
 *     across its nine octaves rather than of octaves alone. That is the mode's whole
 *     point once a scale exists.
 *   - mode 0 (LFO) is never quantized. Its "notes" are rates.
 *
 * ⚠️ Mutually exclusive with a Marbles output routed to V/Oct, in BOTH directions
 * (main.c): selecting a scale unpatches every V/Oct route, and routing to V/Oct turns the
 * scale off and opens the range to maximum. A quantizer and an external pitch source
 * fighting over the same note is never what anyone wants.
 *
 * ⚠️ A selected scale also sets sp1_synth_params.note_hold (M4e, Adara), which makes the
 * AUDIO thread latch the note at each TRIG edge. Quantized notes stop sliding through the
 * scale while they decay; F1 chooses the next note instead of bending the current one.
 * The reason it cannot be done here is that TRIG is not visible from the main thread. */
#define SP1_PUI_SCALE_OFF (-1)
void sp1_pui_set_scale(int scale);
int  sp1_pui_scale(void);
/* Open the FREQUENCY range fully (mode 10). Called when something is routed to V/Oct. */
void sp1_pui_set_octave_max(void);
bool sp1_pui_catching(int fader);              /* in the active layer */
bool sp1_pui_level_connected(void);            /* SETTINGS F4 >= 5 % (M4a) */
/* ---- engines: slots from config/engines.csv; glyphs and detents fixed in gen_engines.py ---- */
int  sp1_pui_engine(void);                     /* Plaits engine index (voice.cc) */
/* Which of the CURRENT engine's parameters are bipolar, as SP1_ENGINE_TABLE[].centre:
 * 0x1 = HARMONICS (F4), 0x2 = TIMBRE (F2), 0x4 = MORPH (F3). A detent is placed exactly
 * where a parameter's centre is its neutral point, so this table IS the polarity table --
 * Marbles' INTELLIGENT voltage range reads it (M4c) and so does the rip's neutral state. */
uint8_t sp1_pui_engine_centre(void);
int  sp1_pui_slot(void);                       /* 0-based position in the list   */
/* The slot actually PLAYING: sp1_pui_slot() moved by MIDI's MODEL CC (M5a). Equal to the
 * selection while no MODEL CC is applied. */
int  sp1_pui_eslot(void);
/* A slot's glyph as LED levels (any slot; out of range = the selection). */
void sp1_pui_slot_leds(int slot, uint8_t out[4]);

/* "••" + PLAY held 3 s ("rip out the cables"): a **FULL PATCH WIPE** on PLAITS
 * (Adara, M4d — docs/DEFAULTS.md is the spec).
 *
 *   engine     -> slot 1 (virtual analog)
 *   BASE       -> that engine's neutral state: 0.5 for each BIPOLAR parameter, 0 for
 *                 each unipolar one, F1 to its centre = C4
 *   SHIFT      -> all four attenuverters to gain 0
 *   SETTINGS   -> range 10 (full), LPG colour 0, decay 0.5, LEVEL disconnected
 *   quantizer  -> off
 *
 * ⚠️ Through M4b this deliberately KEPT BASE and the engine and was described as "a
 * modulation reset, not a patch wipe". That is no longer true. main.c also resets the
 * output mode and the burst division, and the soft-clip drive from either module.
 *
 * ⚠️ The faders do not move; pickup catches them up (Adara: by design). */
void sp1_pui_rip(void);
const char *sp1_pui_engine_name(void);
/* The current slot's glyph as LED levels (fixed per slot, tools/gen_engines.py). */
void sp1_pui_engine_leds(uint8_t out[4]);
/* T3 = +1, T2 = -1. Wraps; skips empty slots. Returns the new SLOT. */
int  sp1_pui_engine_step(int dir);

#endif /* SP1_PLAITS_UI_H */
