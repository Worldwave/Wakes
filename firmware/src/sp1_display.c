/* wakes-sp1 — model-row display arbitration. See sp1_display.h. */
#include "sp1_display.h"
#include "sp1_led.h"
#include "sp1_ui_timing.h"

#include <string.h>

static uint8_t  idle_level[4];

static uint8_t  value_level[4];
static uint32_t value_hold_ms;

static uint8_t  trans_level[4];
static uint32_t trans_ms;

void sp1_display_init(void)
{
	memset(idle_level, 0, sizeof(idle_level));
	memset(value_level, 0, sizeof(value_level));
	memset(trans_level, 0, sizeof(trans_level));
	value_hold_ms = 0u;
	trans_ms = 0u;
}

/* Fill `out` with a bar for `v`, growing from index 0. */
static void bar_unipolar(uint8_t v, uint8_t out[4])
{
	for (int i = 0; i < 4; i++) {
		const uint32_t lo = (uint32_t)i * 255u / 4u;
		const uint32_t hi = (uint32_t)(i + 1) * 255u / 4u;
		if (v >= hi) {
			out[i] = 255u;
		} else if (v > lo) {
			out[i] = (uint8_t)(((uint32_t)(v - lo) * 255u) / (hi - lo));
		} else {
			out[i] = 0u;
		}
	}
}

/* Bipolar: centre-out. 128 is the centre and shows a dim notch on the two inner
 * LEDs so "zero" is visibly a position rather than an absence. */
static void bar_bipolar(uint8_t v, uint8_t out[4])
{
	const int32_t signed_v = (int32_t)v - 128;            /* -128 .. +127 */
	const uint32_t mag = (uint32_t)(signed_v < 0 ? -signed_v : signed_v);
	const uint32_t scaled = (mag * 255u) / 128u;          /* 0 .. ~255 */

	memset(out, 0, 4);
	/* Inner pair is the centre notch, always faintly lit. */
	const uint8_t NOTCH = 24u;
	out[1] = NOTCH;
	out[2] = NOTCH;

	/* Two LEDs per side: inner fills first, then outer. */
	const uint32_t half = scaled > 255u ? 255u : scaled;
	const uint8_t inner = (half >= 128u) ? 255u
			    : (uint8_t)((half * 255u) / 128u);
	const uint8_t outer = (half <= 128u) ? 0u
			    : (uint8_t)(((half - 128u) * 255u) / 127u);

	if (signed_v < 0) {
		out[1] = inner > NOTCH ? inner : NOTCH;
		out[0] = outer;
	} else if (signed_v > 0) {
		out[2] = inner > NOTCH ? inner : NOTCH;
		out[3] = outer;
	}
}

void sp1_display_value(uint8_t v, bool bipolar)
{
	if (bipolar) {
		bar_bipolar(v, value_level);
	} else {
		bar_unipolar(v, value_level);
	}
	value_hold_ms = SP1_DISP_VALUE_HOLD_MS;
}

void sp1_display_transient(const uint8_t level[4], uint16_t ms)
{
	memcpy(trans_level, level, 4);
	trans_ms = ms;
}

/* ---- engine flash: see sp1_display.h ---- */
static uint8_t  eng_level[4];
static uint32_t eng_ms;                /* 0 = inactive; counts UP */
static uint32_t eng_hold = SP1_DISP_ENGINE_HOLD_MS;
static uint32_t eng_fade = SP1_DISP_ENGINE_FADE_MS;
static uint32_t eng_count;             /* every flash: sp1_display_flash_count() */

void sp1_display_flash(const uint8_t level[4], uint32_t hold_ms, uint32_t fade_ms)
{
	for (int i = 0; i < 4; i++) {
		eng_level[i] = level[i];
	}
	eng_hold = hold_ms;
	eng_fade = fade_ms > 0u ? fade_ms : 1u;
	eng_ms = 1u;                       /* (re)start: hold, then fade */
	eng_count++;
}

uint32_t sp1_display_flash_count(void)
{
	return eng_count;
}

void sp1_display_engine(const uint8_t level[4])
{
	sp1_display_flash(level, SP1_DISP_ENGINE_HOLD_MS, SP1_DISP_ENGINE_FADE_MS);
}

/* Blend weight of the engine pattern, 256 = pattern only, 0 = page only. */
static uint32_t eng_weight(void)
{
	if (eng_ms == 0u) {
		return 0u;
	}
	if (eng_ms <= eng_hold) {
		return 256u;
	}
	const uint32_t t = eng_ms - eng_hold;
	if (t >= eng_fade) {
		return 0u;
	}
	return 256u - (t * 256u) / eng_fade;
}

static void eng_advance(uint32_t elapsed_ms)
{
	if (eng_ms != 0u) {
		eng_ms += elapsed_ms;
		if (eng_ms > eng_hold + eng_fade) {
			eng_ms = 0u;
		}
	}
}

void sp1_display_set_idle(const uint8_t level[4])
{
	memcpy(idle_level, level, 4);
}

void sp1_display_meter(const uint8_t level[4])
{
	/* Same storage as the idle display -- the meter IS the resting state of the
	 * row, not an overlay on top of it. Separate entry point because the callers
	 * mean different things and the next milestone may want to treat them
	 * differently (e.g. a meter that dims out after a period of no movement). */
	memcpy(idle_level, level, 4);
}

/* Decay the transient and value timers WITHOUT drawing anything.
 *
 * Call this on every tick where the row belongs to someone else -- the shutdown
 * animation owns it, so main() skips sp1_display_tick() entirely. Without this the
 * timers freeze for the whole gesture: show the model index, hold "••" past 3 s,
 * release to cancel, and a second-old overlay reappears instead of the meter,
 * because trans_ms never counted down. A timer measures wall clock, not render
 * opportunities. */
void sp1_display_age(uint32_t elapsed_ms)
{
	eng_advance(elapsed_ms);
	trans_ms = (trans_ms > elapsed_ms) ? (trans_ms - elapsed_ms) : 0u;
	value_hold_ms = (value_hold_ms > elapsed_ms)
			? (value_hold_ms - elapsed_ms) : 0u;
}

void sp1_display_tick(uint32_t elapsed_ms)
{
	/* Engine flash sits ON TOP of everything this row shows (the shutdown animation
	 * never reaches here: main skips this call while it owns the row). The page is
	 * still computed underneath, so the fade lands on the live page, not a snapshot. */
	const uint32_t ew = eng_weight();
	eng_advance(elapsed_ms);
	if (ew > 0u) {
		const uint8_t *under = (trans_ms > 0u) ? trans_level
				     : (value_hold_ms > 0u) ? value_level : idle_level;
		trans_ms = (trans_ms > elapsed_ms) ? (trans_ms - elapsed_ms) : 0u;
		value_hold_ms = (value_hold_ms > elapsed_ms)
				? (value_hold_ms - elapsed_ms) : 0u;
		for (int i = 0; i < 4; i++) {
			const int32_t d = (int32_t)eng_level[i] - (int32_t)under[i];
			sp1_led_set(SP1_ROW_TRACK, i,
				    (uint8_t)((int32_t)under[i] + ((d * (int32_t)ew) >> 8)));
		}
		return;
	}

	if (trans_ms > 0u) {
		trans_ms = (trans_ms > elapsed_ms) ? (trans_ms - elapsed_ms) : 0u;
		for (int i = 0; i < 4; i++) {
			sp1_led_set(SP1_ROW_TRACK, i, trans_level[i]);
		}
		return;
	}

	if (value_hold_ms > 0u) {
		value_hold_ms = (value_hold_ms > elapsed_ms)
				? (value_hold_ms - elapsed_ms) : 0u;
		for (int i = 0; i < 4; i++) {
			sp1_led_set(SP1_ROW_TRACK, i, value_level[i]);
		}
		return;
	}

	/* ---- idle, RENDERED EVERY TICK ----
	 *
	 * The idle layer used to be written once and then left alone, on the theory
	 * that whoever set it owns the row. That only works when idle is a static
	 * picture. It is not any more: idle is now the four-fader meter, which has to
	 * be redrawn continuously or it freezes at whatever it showed when the last
	 * transient expired.
	 *
	 * There is deliberately NO cross-fade on the handoff back to idle. Fading
	 * toward a target that is itself moving reads as lag rather than polish -- you
	 * push a fader and the LED arrives 250 ms behind your thumb. Transients and
	 * value readouts snap back to the meter, and the meter's own motion is the
	 * continuity. */
	for (int i = 0; i < 4; i++) {
		sp1_led_set(SP1_ROW_TRACK, i, idle_level[i]);
	}
}
