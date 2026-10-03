/*
 * wakes-sp1 — USB CDC ACM console. See sp1_console.h.
 *
 * Uses Zephyr's device_next USB stack, the path chattock/sp1-tape-looper proved on this
 * board and this Zephyr version. The legacy CONFIG_USB_DEVICE_STACK is smaller but
 * deprecated. Since M5a the USB device itself -- identity, every function on it -- is
 * sp1_usbd.c; this file is only the CDC ACM console that rides on it.
 */
#include "sp1_console.h"
#include "sp1_batt.h"
#include "sp1_power.h"
#include "sp1_controls.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include "sp1_usbd.h"
#include "sp1_logbuf.h"
#include <zephyr/app_version.h>   /* generated from firmware/VERSION */

static bool up;

/* The console device itself, so DTR can be watched. */
static const struct device *cdc = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

/* ---- why the banner is stored rather than just printed ----
 * Printing it once at boot loses it. USB enumeration takes hundreds of
 * milliseconds and the host may not open the port for seconds after that, so the
 * banner goes out before anyone can receive it -- confirmed on hardware
 * 2026-09-20, where a capture showed nine buffered STATUS lines flushed on attach
 * but no banner. We cannot wait for a host either: the device has to boot
 * unplugged.
 *
 * So: keep the contents, and re-print them whenever DTR rises, i.e. whenever a
 * terminal attaches. Attaching minutes later still tells you why the device last
 * booted. */
static struct {
	uint32_t resetreas;
	uint32_t fault_reason;
	uint32_t fault_pc;
	bool     had_fault;
	bool     valid;
} boot_info;

static bool dtr_prev;

static void print_banner(void)
{
	printk("\n=== Wakes v" APP_VERSION_STRING " ===\n");
	printk("built %s %s\n", __DATE__, __TIME__);

	/* nRF52840 POWER.RESETREAS -- ALL documented bits.
	 *
	 * An earlier version decoded only 0-3 and 16, and so printed no reason at
	 * all for 0x00100000 (bit 20, VBUS) -- which is the most common wake of all
	 * on this device: plugging in USB while off. A reset-reason decoder that is
	 * silent about the usual case is worse than none, because it reads as an
	 * unrecognised value. Keep this complete. */
	printk("resetreas 0x%08x%s%s%s%s%s%s%s%s%s\n", boot_info.resetreas,
	       (boot_info.resetreas & (1u << 0))  ? " pin"      : "",
	       (boot_info.resetreas & (1u << 1))  ? " watchdog" : "",
	       (boot_info.resetreas & (1u << 2))  ? " soft"     : "",
	       (boot_info.resetreas & (1u << 3))  ? " lockup"   : "",
	       (boot_info.resetreas & (1u << 16)) ? " off-wake:button" : "",
	       (boot_info.resetreas & (1u << 17)) ? " off-wake:lpcomp" : "",
	       (boot_info.resetreas & (1u << 18)) ? " debug-if" : "",
	       (boot_info.resetreas & (1u << 19)) ? " off-wake:nfc" : "",
	       (boot_info.resetreas & (1u << 20)) ? " off-wake:usb"  : "");
	if (boot_info.resetreas == 0u) {
		/* Cleared every boot, so an all-zero value means a genuine cold
		 * start: battery inserted, or the bootloader handed off without a
		 * recorded cause. */
		printk("          (cold start / no recorded cause)\n");
	}

	if (boot_info.had_fault) {
		printk("!! previous boot FAULTED: reason %u pc 0x%08x\n",
		       boot_info.fault_reason, boot_info.fault_pc);
	}
	printk("usb  %s\n", sp1_usb_present() ? "present" : "absent");
	printk("-----------------\n");
}

/* Status cadence, set per state by the loops. 0 disables status lines. */
static uint32_t status_period_ms = 1000u;

/* ---- raw control capture (M1d-a) ----
 * ~20 Hz is fast enough to catch a press and see a fader sweep as a curve, and slow
 * enough to stay readable. Deliberately a firehose; off by default.
 *
 * 48 ms, not 50: the control tick is 8 ms, so an accumulator threshold of 50 fires on
 * the 7th tick and the real period is 56 ms. 48 is exactly 6 ticks, which makes the
 * `t=` column advance in a clean 48 ms step and the cadence match what is printed in
 * the header instead of being 12 % slower than advertised. */
static bool     raw_capture;
static uint32_t raw_acc;
static uint32_t raw_lines;
#define RAW_PERIOD_MS  48u
#define RAW_HEADER_EVERY 40u    /* re-print the legend so a long capture stays readable */

static void print_raw_legend(void)
{
	printk("# t=uptime_ms  lad0,lad1=ladder ADC  f1..f4=fader ADC (0-4095)"
	       "  rail: i=idle L=loaded\n");
	printk("# f1..f4 ARE ONLY MEANINGFUL ON rail=i LINES."
	       " On rail=L a button is down and the fader rail is sagging.\n");
}

void sp1_console_set_raw_capture(bool on)
{
	raw_capture = on;
	raw_acc = 0u;
	raw_lines = 0u;
	if (up && on) {
		printk("\n--- RAW CAPTURE ON (%u ms / %u Hz) ---\n",
		       RAW_PERIOD_MS, 1000u / RAW_PERIOD_MS);
		print_raw_legend();
		printk("# battery STATUS lines are suppressed while this is on,"
		       " so the capture stays one thing.\n");
	} else if (up) {
		printk("\n--- RAW CAPTURE OFF ---\n");
	}
}

bool sp1_console_raw_capture(void)
{
	return raw_capture;
}

void sp1_console_set_status_period(uint32_t ms)
{
	status_period_ms = ms;
}

/* ---- why anything long has to be paced ----
 * CDC ACM has a finite ring buffer and printk does not block waiting for the host
 * to drain it: past the end of the buffer, characters are simply DROPPED. A burst
 * of forty lines emitted inside one control tick overruns it.
 *
 * Observed on hardware 2026-09-19: the calibration report came out shredded
 * mid-line -- `{  209, "T1	{  399, "T2" },` -- and the boot banner in the same log
 * is cut at `lvl=25`. Both are the same overrun. It is a nasty failure because the
 * output looks almost right, so it reads as a formatting bug rather than as data
 * loss, and a table transcribed from it would be silently wrong.
 *
 * sp1_console_pace() yields the CPU briefly so the USB stack can move a packet.
 * Every multi-line dump must call it between lines. It is only safe from the
 * control loop -- never from the audio path once M2 lands. */
void sp1_console_pace(void)
{
	if (!up) {
		return;
	}
	/* One tick is enough: CDC ACM moves a 64-byte packet per USB frame (1 ms) and
	 * a line here is well under that. Two ticks would double a 40-line report's
	 * duration for no benefit. */
	k_msleep(1);
}

int sp1_console_init(void)
{
	if (sp1_usbd_init() != 0) {
		return -1;
	}
	up = true;
	/* Deliberately NOT waiting for DTR: the device must boot unplugged. Output
	 * before a host attaches is simply dropped. */
	return 0;
}

void sp1_console_banner(uint32_t resetreas, bool had_fault,
			uint32_t fault_reason, uint32_t fault_pc)
{
	boot_info.resetreas    = resetreas;
	boot_info.had_fault    = had_fault;
	boot_info.fault_reason = fault_reason;
	boot_info.fault_pc     = fault_pc;
	boot_info.valid        = true;

	if (!up) {
		return;
	}
	/* Print it now in case a host is already listening, and again on every DTR
	 * rise (see sp1_console_poll). Either may be the one that lands. */
	print_banner();
}

void sp1_console_poll(uint32_t elapsed_ms, const char *state)
{
	static uint32_t acc;
	static bool     announced_complete;

	if (!up) {
		return;
	}

	/* Re-print the banner whenever a terminal attaches. DTR rising is the host
	 * opening the port; the boot banner printed before that was almost certainly
	 * lost. Non-blocking: if DTR cannot be read we simply never re-print. */
	if (cdc != NULL && device_is_ready(cdc)) {
		uint32_t dtr = 0;
		if (uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr) == 0) {
			const bool now = (dtr != 0);
			if (now && !dtr_prev) {
				/* The banner again (not stored twice), then everything stored
				 * since boot -- an OP-XY session, a stretch on battery
				 * (sp1_logbuf.h). */
				if (boot_info.valid) {
					sp1_logbuf_hold(true);
					print_banner();
					sp1_logbuf_hold(false);
				}
				sp1_logbuf_replay_start();
			} else if (!now && dtr_prev) {
				sp1_logbuf_replay_stop();   /* nobody is listening any more */
			}
			dtr_prev = now;
		}
	}
	/* The stored log, a step per tick: paced for CDC ACM, and the loop goes on feeding
	 * the watchdog in between. */
	sp1_logbuf_replay_step();

	if (raw_capture) {
		raw_acc += elapsed_ms;
		if (raw_acc >= RAW_PERIOD_MS) {
			raw_acc = 0u;
			char line[96];
			if (sp1_controls_raw_line(line, sizeof(line)) > 0) {
				if (raw_lines % RAW_HEADER_EVERY == 0u) {
					print_raw_legend();
				}
				raw_lines++;
				printk("%s\n", line);
			}
		}

		/* ---- the capture owns the console while it runs ----
		 * Interleaving a battery STATUS line into a 20 Hz control capture
		 * every few seconds makes the capture unreadable exactly when you are
		 * trying to correlate lines against what your hands were doing. Adara
		 * asked for the control log to be the fast one and the battery to stay
		 * slow; the clean way to get that is for the slow one to stand down
		 * entirely rather than to race.
		 *
		 * Nothing is lost that matters: the battery is still SAMPLED on its own
		 * 10 s timer, so voltage and charge state stay current -- this
		 * suppresses the print, not the read -- and the boot banner is above
		 * this return, so a terminal attaching mid-capture still learns why the
		 * device booted.
		 *
		 * The CALIBRATION line is below it and is therefore also suppressed.
		 * That is fine and deliberate: the divider was measured in M1c and
		 * SP1_BATT_DIVIDER_MILLI is populated, so that line has no job left. If
		 * you ever need to re-measure it, turn the capture off -- the two
		 * calibrations were never meant to run at the same time. */
		return;
	}

	if (status_period_ms == 0u) {
		return;      /* status lines off; banner and calibration still ran */
	}
	acc += elapsed_ms;
	if (acc < status_period_ms) {
		return;
	}
	acc = 0u;

	const bool plugged  = sp1_usb_present();
	const bool charging = sp1_charging();

	printk("[%s] batt raw=%4u  %umV  lvl=%3u/255   usb=%d chg=%d\n",
	       state, sp1_batt_raw(), sp1_batt_mv(), sp1_batt_level(),
	       plugged ? 1 : 0, charging ? 1 : 0);

	/* ---- the calibration line ----
	 * The BQ24232 terminates at 4.20 V, and says so: nPGOOD low (USB good) with
	 * nCHG high (not charging) means charge complete, so the cell is at 4.20 V
	 * BY DEFINITION. That pins the unknown divider:
	 *
	 *     V_adc   = raw / 4095 * 3.6 V
	 *     divider = 4.20 / V_adc
	 *
	 * Printed continuously while complete rather than on the transition, because
	 * plugging in an already-full battery produces no transition to catch. */
	if (plugged && !charging && sp1_batt_valid()) {
		const uint32_t raw = sp1_batt_raw();
		if (raw > 0u) {
			/* divider x1000 = 4200 * 1000 * 4095 / (raw * 3600).
			 * Written as 4777500 / raw: the naive form overflows uint32
			 * (4200 * 1000 * 4095 = 1.72e10 against a 4.29e9 ceiling).
			 * 4200 * 4095 / 3600 = 4777.5, so x1000 gives 4777500. */
			const uint32_t div_milli = 4777500u / raw;
			printk("CALIBRATION  charge complete: raw=%u"
			       "  ==> divider = %u.%03u  (currently assuming %u.%03u)\n",
			       raw, div_milli / 1000u, div_milli % 1000u,
			       SP1_BATT_DIVIDER_MILLI / 1000u,
			       SP1_BATT_DIVIDER_MILLI % 1000u);
			if (!announced_complete) {
				announced_complete = true;
				printk("CALIBRATION  put this in"
				       " SP1_BATT_DIVIDER_MILLI (sp1_batt.h)"
				       " as an integer x1000, and rebuild.\n");
			}
		}
	} else {
		announced_complete = false;
	}
}
