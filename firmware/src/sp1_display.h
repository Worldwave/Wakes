/*
 * wakes-sp1 — the model row (T1-T4) as a shared display surface.
 *
 * On a device with no screen, four LEDs have to do several jobs, so they are
 * arbitrated by priority with transients that expire rather than by whoever wrote
 * last. Highest priority first:
 *
 *   1. the shutdown animation  — owned by sp1_power.c; main() simply does not call
 *                                into here while it runs
 *   2. a transient overlay     — e.g. the selected model, shown briefly after a
 *                                change, then gone
 *   3. the value readout       — one parameter, as a bar. For parameters that are
 *                                NOT one of the four faders (shift layer, M1d-c)
 *   4. the meter / idle        — the resting display, redrawn every tick
 *
 * ---- THE RESTING DISPLAY IS A FOUR-CHANNEL METER, NOT A BAR ----
 * One fader per LED: F1→T1, F2→T2, F3→T3, F4→T4, all four at once, each LED's
 * brightness being its own fader's position. Per Adara, and it is the simpler thing
 * in every respect: no "which fader moved last" detection, no movement threshold, no
 * arbitration between four sources that all want the whole row.
 *
 * An earlier version showed the most-recently-moved fader as a bar across all four
 * LEDs. That was a misreading of the spec and it is not coming back. It needed a
 * movement detector to decide whose turn it was, and that detector was what turned
 * stray ADC noise into visible LED flicker during the M1d-a failure — with the
 * BTN_COM rail unpowered, button presses coupled blips into the fader pins, the
 * blips crossed the movement threshold, and the row jumped between four bars.
 * Reading all four continuously has no such state to be wrong about.
 *
 * `sp1_display_value()` survives for the shift layer, where the parameter being
 * adjusted has no LED of its own. There a bar is right, and it is also what
 * pickup/catch needs: you can SEE where the stored value sits relative to the
 * fader, which is otherwise invisible and maddening.
 */
#ifndef SP1_DISPLAY_H
#define SP1_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

void sp1_display_init(void);

/* The four-channel meter: level[i] is LED i's brightness, 0-255. This is the
 * resting display — set it every tick from the fader positions and it simply
 * tracks them. Same storage as sp1_display_set_idle(). */
void sp1_display_meter(const uint8_t level[4]);

/* Show a value as a bar across the row, 0-255. For a parameter with no LED of its
 * own — the shift layer. NOT for the four faders; those get the meter above.
 *
 * unipolar: fills from T1 toward T4.
 * bipolar:  grows outward from the centre -- left half for negative, right for
 *           positive, with 128 reading as a dim centre notch. Attenuverters and
 *           Marbles' BIAS are bipolar, and a fill-from-the-left bar would make
 *           "zero" look like "empty" rather than "centred".
 *
 * Holds for SP1_DISP_VALUE_HOLD_MS after the last update, then fades out. */
void sp1_display_value(uint8_t v, bool bipolar);

/* Show an arbitrary 4-LED pattern for `ms`, overriding the value readout.
 * Used for the model index, and anything else that needs to say something briefly. */
void sp1_display_transient(const uint8_t level[4], uint16_t ms);

/* ENGINE FLASH (M3b/M3c, Adara). On every engine change the row jumps to the engine's
 * pattern at once, holds it for SP1_DISP_ENGINE_HOLD_MS, then CROSS-FADES back into
 * whatever the page is showing over SP1_DISP_ENGINE_FADE_MS. A new change restarts it
 * from the new pattern, so scrolling through engines shows each one instantly without
 * the row blinking back to the page in between.
 *
 * The pattern itself comes from the Glyph column of config/engines.csv (free-form:
 * off / half / full per LED, unique per engine), already turned into levels by the
 * caller -- see sp1_pui_engine_leds(). */
void sp1_display_engine(const uint8_t level[4]);

/* The same overlay with its own timing (M4): `level` at once, held `hold_ms`, then
 * cross-faded into the page over `fade_ms`. sp1_display_engine() is this with
 * SP1_DISP_ENGINE_HOLD_MS / SP1_DISP_ENGINE_FADE_MS. Calling it every tick with a
 * hold longer than a tick keeps `level` on the row (the rip animation). */
void sp1_display_flash(const uint8_t level[4], uint32_t hold_ms, uint32_t fade_ms);

/* Resting display when nothing else is showing. Defaults to dark. Identical
 * storage to sp1_display_meter(); use whichever name says what you mean. */
void sp1_display_set_idle(const uint8_t level[4]);

/* Age the transient and value timers without drawing. Call on every tick where
 * something else owns the row (the shutdown animation), so an overlay does not
 * outlive its duration and reappear when the row is handed back. */
void sp1_display_age(uint32_t elapsed_ms);

/* Call once per control tick. Renders whatever currently wins. The resting layer is
 * redrawn EVERY call, so a live meter stays live; transients and value readouts snap
 * back to it rather than cross-fading, because fading to a moving target looks like
 * lag. */
void sp1_display_tick(uint32_t elapsed_ms);

#endif /* SP1_DISPLAY_H */
