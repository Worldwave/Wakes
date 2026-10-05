/* power.c — see power.h.
 *
 * Ported from feldd (firmware/app/src/main.c): charger_init(), the charge-
 * standby gate with its press feedback and charge gauge, boot_signature(),
 * enter_bootloader()'s park-and-SYSTEM_OFF, the side-row rest LED, track press
 * feedback and the ••+T1 battery peek. The LED, ADC and button drivers are
 * feldd's own files, staged by prep.sh. Every LED behaviour here is feldd's;
 * the one deviation is gesture_cancel()'s per-tick reset (gesture.h). */
/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "power.h"

#include "audio.h"
#include "buttons.h"
#include "controls.h"
#include "gesture.h"
#include "led.h"
#include "sp1_board.h"
#include "wdt.h"

#include <hal/nrf_gpio.h>
#include <nrfx.h>
#include <zephyr/kernel.h>

#define PIN_TRS_RING    NRF_GPIO_PIN_MAP(0, 23)   /* PNP base: HIGH = current source off */
#define ADC_BATTERY     6                          /* controls.c channel order */
#define BOOT_SIG_SWEEPS 2                          /* feldd v0.6.2-beta */
#define LED_SIDE1       4                          /* led.c: 4..7 = side LED1..4 */

static struct gesture_state gs;
static struct vol_state vs;
static bool vol_up_held, vol_dn_held;

FUNC_NORETURN static void system_off(void);

/* ---- LED writes: only on change, lit before dark ----
 *
 * power_poll used to rewrite all eight LEDs every 10 ms tick. Two costs, both
 * avoided here:
 *   - LIT BEFORE DARK. Zephyr's nRF PWM driver stops a PWM instance the moment
 *     all its channels are dark, then busy-waits up to a period (1.024 ms) to
 *     restart it (pwm_nrfx.c). A single LED stepping along a row -- press
 *     feedback moving between Tracks -- would hit that on every step if the
 *     old LED went dark first. Both rows are one PWM instance each
 *     (app.overlay: PWM2 Track, PWM3 side).
 *   - ONLY ON CHANGE. A write that changes nothing still ends in a PWM sequence
 *     playback. The cache is power_poll's alone: power_boot() forgets it, since
 *     the boot and gate paths drive LEDs directly. */
static int8_t led_cache[LED_COUNT];

static void led_cache_forget(void)
{
	for (int i = 0; i < LED_COUNT; i++) led_cache[i] = -1;
}

static void led_put(int i, bool on)
{
	if (led_cache[i] == (int8_t)on) return;
	led_idx(i, on);
	led_cache[i] = (int8_t)on;
}

static void row_put(int base, unsigned bits)
{
	for (int i = 0; i < 4; i++) if ((bits >> i) & 1u) led_put(base + i, true);
	for (int i = 0; i < 4; i++) if (!((bits >> i) & 1u)) led_put(base + i, false);
}

static void side_row(unsigned bits)
{
	row_put(LED_SIDE1, bits);
}

/* feldd's boot_signature(): two Track 1->4 sweeps at full brightness. */
static void boot_signature(void)
{
	led_set_brightness(LED_BRIGHTNESS_FULL);
	for (int i = 0; i < 4; i++) led_idx(i, false);
	for (int pass = 0; pass < BOOT_SIG_SWEEPS; pass++) {
		for (int i = 0; i < 4; i++) {
			feed_wdt();
			led_idx(i, true);
			k_msleep(80);
			led_idx(i, false);
		}
	}
	led_set_brightness(LED_BRIGHTNESS_DEFAULT);
}

/* The power-on hold (gesture.h), with feldd's gate feedback: Track LED 1 while
 * •• is held; the charge gauge on the side row while released on USB, the ADC
 * and BTN_COM rail brought up only then, never on battery. */
static void power_on_gate(void)
{
	struct wake_state w;
	wake_init(&w);
	bool adc_up = false;
	int batt_raw = -1;

	led_set_brightness(LED_BRIGHTNESS_FULL);
	for (unsigned tick = 0;; tick++) {
		feed_wdt();
		bool held = nrf_gpio_pin_read(SP1_FUNC_BTN) == 0;
		bool usb = nrf_gpio_pin_read(SP1_CHG_NPGOOD) == 0;
		switch (wake_step(&w, held, usb)) {
		case WAKE_BOOT:
			side_row(0);
			led_idx(0, false);
			led_set_brightness(LED_BRIGHTNESS_DEFAULT);
			return;                    /* the •• still held is ignored by gesture_step */
		case WAKE_OFF:
			system_off();              /* nothing is running yet to shut down */
		case WAKE_WAIT:
			break;
		}
		led_idx(0, held);
		if (!held && usb) {
			if (!adc_up) {
				(void)controls_init();
				batt_raw = controls_read_raw(ADC_BATTERY);
				adc_up = true;
			} else if (tick % GAUGE_SAMPLE_TICKS == 0) {
				batt_raw = controls_read_raw(ADC_BATTERY);
			}
			side_row(charge_gauge_bits(battery_pct(batt_raw),
						   nrf_gpio_pin_read(SP1_CHG_NCHG) == 0, tick));
		}
		k_msleep(WAKE_TICK_MS);
	}
}

void power_boot(void)
{
	/* Why we booted -- read BEFORE clearing. A stale reset reason or DFU flag
	 * can make the bootloader misfire after a watchdog reset. */
	uint32_t reas = NRF_POWER->RESETREAS;
	led_cache_forget();                /* nothing is known to be lit yet */
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	NRF_POWER->GPREGRET = 0;

	(void)led_init();
	/* Without this the battery never charges, even on USB, and a flat cell
	 * brown-out-thrashes the boot. */
	nrf_gpio_pin_clear(SP1_CHG_NCE);
	nrf_gpio_cfg_output(SP1_CHG_NCE);
	nrf_gpio_pin_clear(SP1_CHG_NCE);
	nrf_gpio_cfg_input(SP1_CHG_NCHG, NRF_GPIO_PIN_PULLUP);
	nrf_gpio_cfg_input(SP1_CHG_NPGOOD, NRF_GPIO_PIN_PULLUP);

	nrf_gpio_cfg_input(SP1_FUNC_BTN, NRF_GPIO_PIN_PULLUP);
	gesture_init(&gs);
	vol_init(&vs);
	audio_set_volume_q8(vol_q8(vs.idx));

	/* A watchdog recovery was mid-session: resume without the hold. */
	if (!(reas & POWER_RESETREAS_DOG_Msk)) power_on_gate();

	boot_signature();
	(void)controls_init();             /* power the ladder rail, set up the SAADC */
	(void)buttons_init();
	led_idx(LED_SIDE1, true);          /* lit through boot; power_poll takes over from here */
	led_cache_forget();                /* the paths above wrote LEDs directly */
}

FUNC_NORETURN static void power_off(void)
{
	audio_shutdown();
	system_off();
}

/* feldd's enter_bootloader(): park every output, wait for the •• release, arm
 * sense-low on it, clear RESETREAS, SYSTEM_OFF. */
FUNC_NORETURN static void system_off(void)
{
	/* SYSTEM_OFF retains driven levels: a lit LED would stay lit and drain the
	 * cell (feldd's 0.7.0-beta "LEDs never turn off" report). */
	for (int i = 0; i < LED_COUNT; i++) led_idx(i, false);
	nrf_gpio_cfg_output(SP1_BTN_COM);
	nrf_gpio_pin_clear(SP1_BTN_COM);
	nrf_gpio_pin_set(PIN_TRS_RING);
	nrf_gpio_cfg_output(PIN_TRS_RING);

	/* Arming sense-low on a pin still held low wakes the chip the instant
	 * SYSTEM_OFF latches, and the power-off silently fails. */
	while (nrf_gpio_pin_read(SP1_FUNC_BTN) == 0) {
		feed_wdt();
		k_msleep(20);
	}
	k_msleep(60);
	nrf_gpio_cfg_sense_input(SP1_FUNC_BTN, NRF_GPIO_PIN_PULLUP,
				 NRF_GPIO_PIN_SENSE_LOW);
	NRF_POWER->RESETREAS = 0xFFFFFFFFu;
	__DSB();
	NRF_POWER->SYSTEMOFF = 1u;
	__DSB();
	for (;;) { }
}

void power_poll(void)
{
	static unsigned tick;
	tick++;

	/* The ladder debounce, as feldd's control loop runs it. Vol+/- are consumed
	 * here; the committed track state drives the LEDs below. */
	struct button_event evt[BTN_COUNT * 2];
	int nev = buttons_scan(evt, ARRAY_SIZE(evt));
	/* Vol+ (5) and Vol- (6): the looper's master volume (gesture.h). */
	for (int i = 0; i < nev; i++) {
		if (evt[i].idx == 5) vol_up_held = evt[i].pressed;
		if (evt[i].idx == 6) vol_dn_held = evt[i].pressed;
	}

	int vdir = vol_up_held == vol_dn_held ? 0 : (vol_up_held ? 1 : -1);
	if (vol_step(&vs, vdir, k_uptime_get_32())) audio_set_volume_q8(vol_q8(vs.idx));
	audio_hp_poll();

	bool func_down = nrf_gpio_pin_read(SP1_FUNC_BTN) == 0;
	int trk = buttons_track_committed();          /* -1 none, 0 Play, 1..4 Track */
	if (func_down && trk >= 0) gesture_cancel(&gs);
	if (gesture_step(&gs, func_down)) power_off();

	/* feldd's track row: the ••+T1 battery peek keeps it dark; otherwise light
	 * only the held Track's own LED. */
	bool batt_peek = func_down && trk == 1;
	unsigned track_bits = !batt_peek && trk >= 1 && trk <= 4 ? 1u << (trk - 1) : 0;
	row_put(0, track_bits);
	/* The side row: feldd's charge gauge during the ••+T1 peek, then feldd's
	 * rest dot. tick / 2 keeps the gauge's ~0.5 s blink at this loop's 10 ms tick. */
	if (batt_peek) {
		side_row(charge_gauge_bits(battery_pct(controls_read_raw(ADC_BATTERY)),
					   nrf_gpio_pin_read(SP1_CHG_NCHG) == 0, tick / 2));
	} else {
		side_row(1u); /* feldd's running/rest LED at default brightness */
	}
}
