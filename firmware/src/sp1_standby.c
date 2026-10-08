/* wakes-sp1 — STANDBY. See sp1_standby.h. */
#include "sp1_standby.h"
#include "sp1_batt.h"
#include "sp1_led.h"
#include "sp1_power.h"
#include "sp1_ui_timing.h"
#include "sp1_console.h"
#if defined(CONFIG_SP1_DRIVE)
#include "sp1_store.h"
#include "sp1_usbd.h"
#endif

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
#if defined(CONFIG_SP1_DRIVE)
	/* Drive mode (M6): the activity chase on the model row, T1 T3 T2 T4. */
	static const uint8_t chase[4] = { 0u, 2u, 1u, 3u };
	uint32_t act_seen = sp1_store_activity();
	uint32_t step_ms = SP1_DRIVE_STEP_MS;
	unsigned chase_i = 0u;
	sp1_store_standby_enter();
#endif

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

#if defined(CONFIG_SP1_DRIVE)
		/* Drive mode: a host (not a charger) gets the card, once per visit. Then each
		 * tick that saw host transfers -- at most one step per SP1_DRIVE_STEP_MS --
		 * lights the next LED of the chase at 50 % and ramps it down. The transfer
		 * path only counts (sp1_store_activity); all of the drawing is here. */
		sp1_store_standby_tick(sp1_usbd_host());
		step_ms += SP1_TICK_MS;
		const uint32_t act = sp1_store_activity();
		if (act != act_seen && step_ms >= SP1_DRIVE_STEP_MS) {
			act_seen = act;
			step_ms = 0u;
			const int led = chase[chase_i++ & 3u];
			sp1_led_set(SP1_ROW_TRACK, led, SP1_DRIVE_LED_PEAK);
			sp1_led_fade(SP1_ROW_TRACK, led, 0u, SP1_DRIVE_RAMP_MS);
		}
#endif

		/* No status lights on the model row any more (Adara, M6): the play-row bar's
		 * breathing already says "charging", and the row belongs to the drive's
		 * activity. It stays dark otherwise. */

		sp1_led_tick(SP1_TICK_MS);
		k_msleep(SP1_TICK_MS);
	}
}
