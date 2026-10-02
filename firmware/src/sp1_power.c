/*
 * wakes-sp1 — power and bootloader-return path.
 *
 * The shutdown sequence, the "feed all 8 WDT channels" trick, and the
 * requirement to power down the external chips before SYSTEM_OFF are taken
 * from chattock/sp1-tape-looper (MIT), whose author established them on real
 * hardware. Getting any of this wrong is how an SP-1 ends up needing its
 * battery disconnected. See NOTICE.
 */
#include "sp1_power.h"
#include "sp1_board.h"
#include "sp1_batt.h"
#include "sp1_led.h"
#include "sp1_ui_timing.h"
#include "sp1_audio.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>

void sp1_wdt_feed(void)
{
	/* Reload every channel rather than only the one we installed: the
	 * bootloader may have armed channels this app does not own. */
	for (int ch = 0; ch < 8; ch++)
		NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
}

uint32_t sp1_resetreas_take(void)
{
	uint32_t r = NRF_POWER->RESETREAS;
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;   /* write-1-to-clear */
	return r;
}

void sp1_charger_init(void)
{
	/* The two status pins are open-drain on the charger: inputs with pull-ups. */
	SP1_BQ_PORT->PIN_CNF[SP1_BQ_NCHG_PIN] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
	SP1_BQ_PORT->PIN_CNF[SP1_BQ_NPGOOD_PIN] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);

	/* nCE low = charging enabled. Drive the level before switching the pin to
	 * an output so the charger never sees a brief high. */
	SP1_BQ_PORT->OUTCLR = (1u << SP1_BQ_NCE_PIN);
	SP1_BQ_PORT->PIN_CNF[SP1_BQ_NCE_PIN] =
		(GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_DRIVE_S0S1    << GPIO_PIN_CNF_DRIVE_Pos)|
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
	SP1_BQ_PORT->OUTCLR = (1u << SP1_BQ_NCE_PIN);
}

void sp1_charger_enable(bool on)
{
	if (on) {
		SP1_BQ_PORT->OUTCLR = (1u << SP1_BQ_NCE_PIN);   /* nCE low: charging */
	} else {
		SP1_BQ_PORT->OUTSET = (1u << SP1_BQ_NCE_PIN);   /* nCE high: not charging */
	}
}

bool sp1_charger_enabled(void)
{
	return (SP1_BQ_PORT->OUT & (1u << SP1_BQ_NCE_PIN)) == 0u;
}

bool sp1_usb_present(void)
{
	/* ---- USB power present: the charger's view OR the nRF's own VBUS detector ----
	 * nPGOOD is the BQ24232's "input power good", and it lags a real VBUS arrival by
	 * the charger's deglitch time. The nRF52840's POWER.USBREGSTATUS.VBUSDETECT is
	 * the VBUS comparator itself, valid whether or not the USB peripheral is enabled.
	 *
	 * The OR only ever makes "present" true MORE often, and every caller is really
	 * asking "is VBUS up?" -- above all sp1_power_off(), which must not SYSTEM_OFF
	 * with VBUS high (no wake edge: dark until the cable is pulled). That constraint
	 * is about VBUS, not about the charger's opinion of it, so the VBUS detector is
	 * the right signal and nPGOOD was a proxy. With the lag, a USB wake that reached
	 * the boot decision first could SYSTEM_OFF with the cable already in. */
	const bool pgood = (SP1_BQ_PORT->IN & (1u << SP1_BQ_NPGOOD_PIN)) == 0u;
	const bool vbus  = (NRF_POWER->USBREGSTATUS &
			    POWER_USBREGSTATUS_VBUSDETECT_Msk) != 0u;
	return pgood || vbus;
}

bool sp1_charging(void)
{
	return (SP1_BQ_PORT->IN & (1u << SP1_BQ_NCHG_PIN)) == 0u;
}

void sp1_fnc_cfg_input(void)
{
	SP1_FNC_PORT->PIN_CNF[SP1_FNC_PIN] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
}

bool sp1_fnc_pressed(void)
{
	return (SP1_FNC_PORT->IN & (1u << SP1_FNC_PIN)) == 0u;
}

/* Arm "••" to wake the chip out of SYSTEM_OFF by sensing the low level. */
static void fnc_arm_wake(void)
{
	SP1_FNC_PORT->PIN_CNF[SP1_FNC_PIN] =
		(GPIO_PIN_CNF_DIR_Input     << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_PULL_Pullup   << GPIO_PIN_CNF_PULL_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos)|
		(GPIO_PIN_CNF_SENSE_Low     << GPIO_PIN_CNF_SENSE_Pos);
}

static void drive_low(NRF_GPIO_Type *port, uint32_t pin)
{
	port->OUTCLR = (1u << pin);
	port->PIN_CNF[pin] =
		(GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
		(GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos);
	port->OUTCLR = (1u << pin);
}

void sp1_quiesce_peripherals(void)
{
	/* Stop the I2S stream BEFORE pulling its clocks out from under it. Bounded at
	 * 50 ms and returns regardless of the audio thread's state -- this function is
	 * on every power-off path, and rule 5a says power-off does not wait on anything
	 * else succeeding. Safe when audio was never started. */
	sp1_audio_stop();

	/* The speaker amp, headphone codec, audio oscillator and eMMC I/O rail are
	 * separate chips held up by retained GPIO levels. Leaving them powered is
	 * what drains the battery overnight, and a clockless amp murmurs audibly.
	 * Safe to call when they were never powered up. */
	drive_low(SP1_CS42_RST_PORT,  SP1_CS42_RST_PIN);   /* codec in reset    */
	drive_low(SP1_TAS_RST_PORT,   SP1_TAS_RST_PIN);    /* amp in reset      */
	drive_low(SP1_EMMC_VCCQ_PORT, SP1_EMMC_VCCQ_PIN);  /* eMMC rail off     */
	drive_low(SP1_OSC_EN_PORT,    SP1_OSC_EN_PIN);     /* 3.072 MHz osc off */

	/* Leaving ON: charging back on (sp1_power.h, "no charging while ON"). STANDBY charges,
	 * and SYSTEM_OFF retains GPIO levels, so a device switched off on battery must not keep
	 * nCE high into the next plug-in. */
	sp1_charger_enable(true);
}

void sp1_power_off(void)
{
	/* The visible animation already ran in sp1_power_tick(). Everything dark
	 * now, unconditionally: SYSTEM_OFF freezes GPIO levels, so any LED still
	 * lit would stay lit into sleep. */
	sp1_leds_all_off();

	/* Wait for the finger to leave the button, or the level-sense we are
	 * about to arm would wake us again immediately. */
	while (sp1_fnc_pressed()) {
		sp1_wdt_feed();
		k_msleep(20);
	}
	k_msleep(60);            /* debounce the release */
	sp1_leds_all_off();      /* re-assert dark immediately before sleep */

	/* SYSTEM_OFF stops the nRF only. The speaker amp, headphone codec, audio
	 * oscillator and eMMC I/O rail are separate chips held up by retained
	 * GPIO levels; leaving them powered is what drains the battery overnight
	 * and what makes a clockless amp murmur. M0 never powers them up, but the
	 * bootloader may have, so quiesce them unconditionally. */
	sp1_quiesce_peripherals();
	/* Register-level I2S halt + pins disconnected. Here and NOT in quiesce: quiesce
	 * also runs on the way to STANDBY, where this would break the next ON's audio. */
	sp1_audio_halt_for_system_off();
	drive_low(SP1_BTN_COM_PORT, SP1_BTN_COM_PIN);      /* ladder rail off   */

	fnc_arm_wake();
	sp1_wdt_feed();

	/* CRITICAL: you cannot SYSTEM_OFF while USB is plugged in. With VBUS
	 * already high there is no wake EDGE, so the device would go dark and stay
	 * dark until the cable is pulled -- indistinguishable from a brick.
	 *
	 * Callers reached via the shutdown gesture go to STANDBY instead and never
	 * get here (see sp1_power_tick). This reset is the backstop for any other
	 * caller -- it re-enters the app, which sees USB present and lands in
	 * standby anyway. Charging continues either way: nCE is a latched GPIO
	 * level. (Approach inherited from chattock/sp1-tape-looper, MIT.) */
	if (sp1_usb_present()) {
		/* Clear on this path too. The banner's RESETREAS decode is treated as
		 * load-bearing, and leaving the old bits set means the next boot ORs
		 * the previous reason together with the new soft-reset bit and reports
		 * both as if they happened at once. */
		NRF_POWER->RESETREAS = 0xFFFFFFFFu;
		__DSB();
		NVIC_SystemReset();
	}

	NRF_POWER->RESETREAS = 0xFFFFFFFFu;   /* clear before SYSTEM_OFF */
	__DSB();
	NRF_POWER->SYSTEMOFF = 1u;
	__DSB();

	for (;;) { /* CPU is off; "••" wakes the device via the bootloader. */ }
}


/* ========================================================================
 *  The "••" shutdown gesture. See sp1_power.h for the specified behaviour.
 * ======================================================================== */

enum pwr_phase {
	PWR_IDLE = 0,     /* not held, or held but not long enough      */
	PWR_RAMP_UP,      /* model row fading up together               */
	PWR_FADE_SEQ,     /* fading dark one at a time                  */
	PWR_SUPPRESSED,   /* "••" was used as a shift during this hold  */
};

static enum pwr_phase phase;
static uint32_t       held_ms;
static uint32_t       phase_ms;
static int            fade_index;

/* The shutdown gesture stays disarmed until "••" has been released once. Holding
 * it for 2 s to power ON would otherwise roll straight into the 3 s shutdown
 * hold and turn the device back off -- 5 s of one continuous press would boot and
 * shut down again, which reads as "it won't turn on". */
static bool armed;

/* Continuous "••" hold, accumulated regardless of phase, armed state or
 * suppression. Feeds the unconditional backstop in sp1_power_tick(). */
static uint32_t force_held_ms;

/* The last warning pulse must PEAK exactly where the fade-out starts, or the
 * hand-over jumps. See sp1_ui_timing.h. */
BUILD_ASSERT(((SP1_PWR_FORCE_SEQ_START - SP1_PWR_FORCE_WARN_MS) %
	      SP1_PWR_FORCE_PULSE_MS) == SP1_PWR_FORCE_PULSE_MS / 2u,
	     "backstop warning pulse must peak at the start of the fade-out");
BUILD_ASSERT(SP1_PWR_FORCE_SEQ_START > SP1_PWR_FORCE_WARN_MS,
	     "fade-out must start inside the warning window");

/* One suppression message per hold. Must NOT be derived from `phase` -- see
 * sp1_shift_used(). Cleared on release. */
static bool suppress_announced;

static void gesture_reset(void);

bool sp1_shift_used(void)
{
	/* Any control touched while "••" is held means "••" is being used as a SHIFT,
	 * not as a power gesture, so shutdown is suppressed for the rest of this hold.
	 * `armed`/`gesture_reset()` clear it on release, so the next hold starts fresh.
	 *
	 * ---- ⚠️ A RUNNING ANIMATION IS NO LONGER CANCELLABLE THIS WAY ----
	 * It used to be: touching a control mid-animation cancelled the shutdown, on the
	 * reasoning that reaching for a fader states intent more clearly than continuing
	 * to hold. That reasoning is fine; the risk asymmetry behind it is not.
	 *
	 *   a false cancel  -> the device will not turn off. Proven, twice, and on a
	 *                      device with no reset pin the exit is to drain the battery.
	 *   a missed cancel -> the device turns off when you meant to shift. Annoying,
	 *                      and undone by turning it back on.
	 *
	 * Those are not comparable, so the animation now ends only on RELEASE -- which
	 * is unambiguous, needs no decoding, and is already the documented escape hatch.
	 * Suppression still does its real job: it stops the animation STARTING while
	 * "••" is in use as a modifier, which is the case the shift layer actually cares
	 * about, since shift chords are short presses rather than 3-second holds.
	 *
	 * To restore the old behaviour, drop this guard -- but read the two lines above
	 * first. */
	if (phase != PWR_IDLE) {
		return false;
	}
	/* Announce the transition INTO suppression, once per hold. When shutdown
	 * stopped working there was nothing in the log to say why, and the cause had
	 * to be inferred from reset reasons. One line ends that: if the device will
	 * not power off, the log now says whether suppression is the reason.
	 *
	 * ⚠️ The flag, not `phase`, is what makes this once-per-hold. Keying off
	 * `phase != PWR_SUPPRESSED` looked equivalent and was not: while `armed` is
	 * false, sp1_power_tick() calls gesture_reset() every tick, which puts `phase`
	 * back to PWR_IDLE, so the guard re-opened and this printed at the full tick
	 * rate -- ~125 lines a second, into a ring buffer that drops rather than
	 * blocks. That is precisely the flood the line was meant to avoid, and it
	 * would corrupt the log exactly when someone is reading it to find out why
	 * shutdown is suppressed. Cleared only on release. */
	const bool first = !suppress_announced;

	suppress_announced = true;
	phase = PWR_SUPPRESSED;
	held_ms = 0u;
	phase_ms = 0u;
	fade_index = 0;

	/* The caller prints WHICH control, because only it can see the controls layer.
	 * Returning "this is the first time this hold" is what keeps that to one line. */
	return first;
}

static void gesture_reset(void)
{
	phase = PWR_IDLE;
	held_ms = 0u;
	phase_ms = 0u;
	fade_index = 0;
}

bool sp1_power_on_hold(void)
{
	uint32_t held = 0u;

	/* DO NOT initialise the LEDs here. The PWM devices are deferred-init, so
	 * during the silent window their pins are still untouched -- and that, not
	 * the blackout alone, is what makes a tap invisible. sp1_led_init() is
	 * called below only once the hold has survived the dark window. */

	while (held < SP1_PWR_ON_HOLD_MS) {
		sp1_wdt_feed();

		if (!sp1_fnc_pressed()) {
			/* ---- released early ----
			 * If that happened inside the dark window the LED pins were
			 * never even claimed, so there is nothing to clear and nothing
			 * was ever visible.
			 *
			 * A DELIBERATE tap, though, shows the battery for 1.5 s and then
			 * goes back to sleep (Adara, M4c): the one piece of information
			 * worth having from a device that is off.
			 *
			 * ⚠️ SAFETY (docs/SAFETY.md, rule 4). This branch
			 * already returned false and the caller already powered off; all
			 * that changes is a BOUNDED delay in front of it. The loop runs
			 * a fixed number of iterations, feeds the watchdog on every one,
			 * and cannot be re-entered. The return value is unchanged, so
			 * the power-off path is exactly as it was -- and nothing here is
			 * conditional on anything that could say "not now".
			 *
			 * ⚠️ Drawn on RELEASE, never during the press: the dark window
			 * exists so a tap shows nothing, and the pins are not claimed at
			 * all for a press shorter than SP1_BATT_FLASH_MIN_MS. A brush
			 * still does nothing, which is the whole point of the blackout.
			 *
			 * ⚠️ And not while plugged in, because this same function serves
			 * STANDBY's "••" press: there the charge bar is ALREADY on the
			 * play row, live and breathing, so the flash would add nothing
			 * visible and would freeze the breathing for 1.5 s -- a hiccup
			 * that reads as a fault. Skipping it is cosmetic only; the
			 * early-release return below is untouched either way. */
			if (held >= SP1_BATT_FLASH_MIN_MS && !sp1_usb_present()) {
				(void)sp1_led_init();
				sp1_batt_sample();
				const uint8_t lvl = sp1_batt_level();
				uint32_t left = SP1_BATT_FLASH_MS;
				while (left > 0u) {
					sp1_wdt_feed();
					uint32_t b = SP1_STANDBY_BAR_LEVEL;
					if (left < SP1_BATT_FLASH_FADE_MS) {
						b = (b * left) / SP1_BATT_FLASH_FADE_MS;
					}
					sp1_led_bar(SP1_ROW_PLAY, lvl, (uint8_t)b);
					k_msleep(SP1_TICK_MS);
					left = left > SP1_TICK_MS ? left - SP1_TICK_MS : 0u;
				}
			}
			sp1_leds_all_off();
			return false;
		}

		if (held >= SP1_PWR_ON_DARK_MS) {
			/* Past the silent window: now it is safe to claim the pins.
			 * Idempotent, so this costs nothing after the first tick. */
			(void)sp1_led_init();

			/* Past the silent window: fill the model row in proportion
			 * to the remaining hold. A direct level per tick rather
			 * than a fade, so it tracks the finger exactly. */
			const uint32_t into = held - SP1_PWR_ON_DARK_MS;
			const uint32_t pct = (into * 255u) / SP1_PWR_ON_FILL_MS;
			for (int i = 0; i < SP1_LEDS_PER_ROW; i++) {
				const uint32_t lo =
					(uint32_t)i * 255u / SP1_LEDS_PER_ROW;
				const uint32_t hi =
					(uint32_t)(i + 1) * 255u / SP1_LEDS_PER_ROW;
				uint8_t level = 0u;
				if (pct >= hi) {
					level = 255u;
				} else if (pct > lo) {
					level = (uint8_t)(((pct - lo) * 255u)
							  / (hi - lo));
				}
				sp1_led_set(SP1_ROW_TRACK, i, level);
			}
		}

		k_msleep(SP1_TICK_MS);
		held += SP1_TICK_MS;
	}

	/* Accepted, so the device is booting: make sure the rows are up even if the
	 * fill never ran (it always does, but do not depend on that). */
	(void)sp1_led_init();

	/* Accepted. Clear the fill and DISARM the shutdown gesture until the
	 * finger comes off -- see `armed`. */
	sp1_led_fade_row(SP1_ROW_TRACK, 0, SP1_PWR_CANCEL_FADE_MS);
	armed = false;
	return true;
}

void sp1_power_on_gate(void)
{
	if (!sp1_fnc_pressed()) {
		/* Cold boot, reset, or USB insert -- nothing to gate. */
		armed = true;
		return;
	}
	if (!sp1_power_on_hold()) {
		sp1_power_off();          /* never returns */
	}
}

enum sp1_power_result sp1_power_tick(uint32_t elapsed_ms, bool fnc_held)
{
	/* ================================================================
	 *  THE BACKSTOP — evaluated before everything else, on purpose.
	 *
	 *  A continuous hold of SP1_PWR_FORCE_HOLD_MS powers the device off
	 *  unconditionally: before the `armed` check, before the suppression check,
	 *  before the state machine. Nothing below can veto it.
	 *
	 *  This is here because on 2026-09-19 the device reached a state where it
	 *  could not be turned off at all. Shift suppression latched on and, since
	 *  suppression returned SP1_PWR_NONE forever, the gesture never progressed.
	 *  With no reset pin and no runtime DFU trigger, the only way out was to
	 *  unplug and drain the battery.
	 *
	 *  The lesson is not "fix the heuristic" -- it is that power-off must not
	 *  depend on a heuristic being correct. Every conditional in this function is
	 *  a UX refinement; this is the guarantee underneath them.
	 *
	 *  ⚠️ NEVER put a condition on this block.
	 * ================================================================ */
	if (!fnc_held) {
		force_held_ms = 0u;
		suppress_announced = false;
	} else {
		force_held_ms += elapsed_ms;

		/* ---- the warning window, ending in the shutdown animation ----
		 * Past SP1_PWR_FORCE_WARN_MS, pulse the model row: "release now or I power
		 * off". The last pulse peaks exactly at SP1_PWR_FORCE_SEQ_START and hands
		 * over to the ordinary shutdown's per-LED fade-out, which ends exactly at
		 * SP1_PWR_FORCE_HOLD_MS. See sp1_ui_timing.h.
		 *
		 * DRAWING ONLY. Every level is a pure function of force_held_ms, so there is
		 * no animation state here that could stall, and nothing here decides
		 * whether the force-off below fires. It fires at SP1_PWR_FORCE_HOLD_MS
		 * whatever this drew.
		 *
		 * Returning SP1_PWR_ANIMATING is what stops the display layer fighting for
		 * the row -- main() already treats that as "the power layer owns T1-T4".
		 *
		 * Skipped while the ordinary animation runs, which cannot reach 15 s anyway
		 * (it commits at ~4.3 s); the guard is there so the two can never overlap
		 * if those timings are ever retuned. */
		if (force_held_ms >= SP1_PWR_FORCE_WARN_MS &&
		    force_held_ms < SP1_PWR_FORCE_HOLD_MS &&
		    phase != PWR_RAMP_UP && phase != PWR_FADE_SEQ) {
			if (force_held_ms < SP1_PWR_FORCE_SEQ_START) {
				/* Pulse. Starts dark at WARN, peaks at SEQ_START. */
				const uint32_t into =
					force_held_ms - SP1_PWR_FORCE_WARN_MS;
				const uint32_t t = into % SP1_PWR_FORCE_PULSE_MS;
				const uint32_t half = SP1_PWR_FORCE_PULSE_MS / 2u;
				const uint32_t tri = (t < half) ? t
						   : (SP1_PWR_FORCE_PULSE_MS - t);
				const uint8_t lvl = (uint8_t)((tri * 255u) / half);

				for (int i = 0; i < SP1_LEDS_PER_ROW; i++) {
					sp1_led_set(SP1_ROW_TRACK, i, lvl);
				}
			} else {
				/* The ordinary shutdown's fade-out, T1 first, drawn from
				 * the clock rather than the fade engine. */
				const uint32_t u = force_held_ms - SP1_PWR_FORCE_SEQ_START;
				for (int i = 0; i < SP1_LEDS_PER_ROW; i++) {
					const uint32_t t0 = (uint32_t)i *
						(SP1_PWR_FADE_MS + SP1_PWR_GAP_MS);
					uint8_t lvl = 0u;
					if (u < t0) {
						lvl = 255u;
					} else if (u < t0 + SP1_PWR_FADE_MS) {
						lvl = (uint8_t)(255u - ((u - t0) * 255u)
								/ SP1_PWR_FADE_MS);
					}
					sp1_led_set(SP1_ROW_TRACK, i, lvl);
				}
			}
			return SP1_PWR_ANIMATING;
		}

		if (force_held_ms >= SP1_PWR_FORCE_HOLD_MS) {
			/* Commit. The fade-out above has already finished drawing, but
			 * this does not depend on it having run: it is reached on time
			 * whatever the animation, the gesture or the shift layer did. */
			sp1_leds_all_off();
			if (sp1_usb_present()) {
				sp1_quiesce_peripherals();
				gesture_reset();
				armed = false;
				force_held_ms = 0u;
				return SP1_PWR_TO_STANDBY;
			}
			sp1_power_off();          /* never returns */
		}
	}

	if (!armed) {
		/* Waiting for the power-on press to be released. */
		if (!fnc_held) {
			armed = true;
		}
		gesture_reset();
		return SP1_PWR_NONE;
	}

	if (!fnc_held) {
		/* Released. Anything in progress is cancelled -- including a
		 * shutdown animation partway through, which is the specified
		 * escape hatch. Restore the row so the cancel is visible. */
		if (phase == PWR_RAMP_UP || phase == PWR_FADE_SEQ) {
			sp1_led_fade_row(SP1_ROW_TRACK, 0, SP1_PWR_CANCEL_FADE_MS);
		}
		gesture_reset();
		return SP1_PWR_NONE;
	}

	switch (phase) {
	case PWR_SUPPRESSED:
		return SP1_PWR_NONE;

	case PWR_IDLE:
		held_ms += elapsed_ms;
		if (held_ms >= SP1_PWR_HOLD_MS) {
			phase = PWR_RAMP_UP;
			phase_ms = 0u;
			sp1_led_fade_row(SP1_ROW_TRACK, 255, SP1_PWR_RAMP_UP_MS);
			return SP1_PWR_ANIMATING;
		}
		return SP1_PWR_NONE;

	case PWR_RAMP_UP:
		phase_ms += elapsed_ms;
		if (phase_ms >= SP1_PWR_RAMP_UP_MS) {
			phase = PWR_FADE_SEQ;
			phase_ms = 0u;
			fade_index = 0;
			sp1_led_fade(SP1_ROW_TRACK, 0, 0, SP1_PWR_FADE_MS);
		}
		return SP1_PWR_ANIMATING;

	case PWR_FADE_SEQ:
		phase_ms += elapsed_ms;
		if (phase_ms >= (uint32_t)(SP1_PWR_FADE_MS + SP1_PWR_GAP_MS)) {
			phase_ms = 0u;
			fade_index++;
			if (fade_index >= SP1_LEDS_PER_ROW) {
				/* Last LED finished with "••" still held: commit.
				 *
				 * Plugged in, the destination is STANDBY, not
				 * SYSTEM_OFF -- and no reset is needed, because we
				 * are already awake. Unplugged, it is a real
				 * SYSTEM_OFF. */
				if (sp1_usb_present()) {
					sp1_quiesce_peripherals();
					gesture_reset();
					armed = false;   /* wait for the release */
					/* ⚠️ Clear the backstop accumulator HERE too, not
					 * only on the backstop's own path.
					 * sp1_power_tick() is called from the ON loop and
					 * nowhere else, so while the device sits in
					 * STANDBY there is no tick to run the
					 * `!fnc_held -> 0` branch: it cannot decay,
					 * however long the button stays up. Leaving
					 * ~4300 ms here made the NEXT power-on cross
					 * 6000 after only ~1.7 s of hold and get thrown
					 * straight back to STANDBY -- "it won't turn
					 * on" -- and, because firing self-clears it,
					 * only on alternate attempts. */
					force_held_ms = 0u;
					return SP1_PWR_TO_STANDBY;
				}
				sp1_power_off();      /* never returns */
			}
			sp1_led_fade(SP1_ROW_TRACK, fade_index, 0, SP1_PWR_FADE_MS);
		}
		return SP1_PWR_ANIMATING;
	}

	return SP1_PWR_NONE;
}
