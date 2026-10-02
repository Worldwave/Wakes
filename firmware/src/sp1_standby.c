/* wakes-sp1 — STANDBY. See sp1_standby.h. */
#include "sp1_standby.h"
#include "sp1_batt.h"
#include "sp1_led.h"
#include "sp1_power.h"
#include "sp1_ui_timing.h"
#include "sp1_console.h"

#include <zephyr/kernel.h>

/* The bar now lives in sp1_led.c (sp1_led_bar), so the "••"-tap battery display in ON and
 * from OFF is the same picture as this one rather than a copy of it (M4c). */
static void draw_bar(uint8_t level, int brightness)
{
	sp1_led_bar(SP1_ROW_PLAY, level, (uint8_t)brightness);
}

void sp1_standby_run(void)
{
	/* STANDBY charges. Already true by every route in (sp1_quiesce_peripherals); said again
	 * here because charging is what STANDBY is for (sp1_power.h, "no charging while ON"). */
	sp1_charger_enable(true);
	uint32_t poll_ms   = SP1_STANDBY_POLL_MS;   /* force a sample on entry */
	uint32_t breath_ms = 0u;

	sp1_leds_all_off();
	sp1_console_set_status_period(SP1_CONSOLE_PERIOD_STANDBY_MS);
	sp1_batt_sample();

	/* ⚠️ STANDBY MUST NOT ACCEPT A HOLD FROM A FINGER THAT NEVER LEFT THE BUTTON.
	 *
	 * This is STANDBY's copy of the `armed` rule in sp1_power.c, and it was missing.
	 * Its absence produced an inescapable loop on 2026-09-19, which is what "the
	 * device fails to shut down, I can't reach OFF or STANDBY" actually was:
	 *
	 *   ON, plugged, "••" held 3 s  -> animation completes -> STANDBY, STILL HELD
	 *   STANDBY sees it held        -> power-on hold satisfied after 1.5 s -> ON
	 *   still held                  -> 6 s backstop fires -> STANDBY
	 *   ...                         -> forever, on a ~7.5 s period
	 *
	 * The device never settles, so from the outside it simply refuses to turn off,
	 * and the ON state dominates -- which is exactly what the console log showed
	 * (32 [ON] status lines against 3 [STANDBY]).
	 *
	 * Entering STANDBY with the button down is the NORMAL case, not the exception:
	 * both routes here (the animation commit and the backstop) complete precisely
	 * because the user was still holding. So arm only once the button is seen up. */
	bool fnc_armed = !sp1_fnc_pressed();

	for (;;) {
		sp1_wdt_feed();

		/* Unplugged: SYSTEM_OFF is possible again now that VBUS is low. */
		if (!sp1_usb_present()) {
			sp1_leds_all_off();
			sp1_power_off();          /* never returns */
		}

		if (!fnc_armed) {
			/* Still waiting for the release that got us here. Draw the
			 * standby display as usual, just ignore the button. */
			if (!sp1_fnc_pressed()) {
				fnc_armed = true;
			}
		} else if (sp1_fnc_pressed()) {
			/* "••" pressed: hand straight to the shared hold so the gesture
			 * is byte-identical to the one at boot. An early release simply
			 * drops back into standby -- unlike at boot, it must NOT power
			 * off, since we are already plugged in and awake. */
			if (sp1_power_on_hold()) {
				return;          /* -> ON */
			}
			/* Released early: redraw from scratch next tick. */
			sp1_leds_all_off();
			continue;
		}

		if ((poll_ms += SP1_TICK_MS) >= SP1_STANDBY_POLL_MS) {
			poll_ms = 0u;
			sp1_batt_sample();
		}
		/* Standby is where the CALIBRATION line appears, because this is the
		 * state the device sits in on the cable at full charge. */
		sp1_console_poll(SP1_TICK_MS, "STANDBY");

		const bool charging = sp1_charging();

		/* When the charger says complete, show FULL regardless of what our
		 * voltage estimate computes. nCHG is authoritative -- the BQ24232
		 * terminated at 4.20 V -- whereas our number carries divider error and
		 * ADC scatter, and would otherwise sit at ~98 % on a genuinely full
		 * cell. Trust the hardware signal over the inference. */
		const uint8_t level = (!charging) ? 255u : sp1_batt_level();

		if (charging) {
			/* Breathe the whole bar so "working on it" reads from across a
			 * room, without implying the level itself is moving.
			 *
			 * Depth is scaled to the bar brightness: at 30 % a fixed
			 * full-scale depth would swing most of the way to black. */
			breath_ms = (breath_ms + SP1_TICK_MS) % SP1_STANDBY_BREATH_MS;
			const uint32_t half = SP1_STANDBY_BREATH_MS / 2u;
			const uint32_t tri = breath_ms < half
				? breath_ms
				: (SP1_STANDBY_BREATH_MS - breath_ms);
			const uint32_t depth_max =
				(SP1_STANDBY_BREATH_DEPTH * SP1_STANDBY_BAR_LEVEL) / 255u;
			const uint32_t depth = (tri * depth_max) / half;
			draw_bar(level,
				 (int)(SP1_STANDBY_BAR_LEVEL - (depth_max - depth)));
		} else {
			/* Not charging: solid. On USB that means charge complete. */
			draw_bar(level, SP1_STANDBY_BAR_LEVEL);
		}

		/* Status lights, dim, on the model row: T2 = plugged, T3 = charging.
		 * T1/T4 stay dark so the pair reads as a symmetric indicator. */
		sp1_led_set(SP1_ROW_TRACK, 0, 0);
		sp1_led_set(SP1_ROW_TRACK, 1, SP1_STANDBY_STATUS_LEVEL);
		sp1_led_set(SP1_ROW_TRACK, 2,
			    charging ? SP1_STANDBY_STATUS_LEVEL : 0u);
		sp1_led_set(SP1_ROW_TRACK, 3, 0);

		sp1_led_tick(SP1_TICK_MS);
		k_msleep(SP1_TICK_MS);
	}
}
