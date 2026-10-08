/*
 * wakes-sp1 — the USB device. See sp1_usbd.h.
 *
 * The bring-up sequence (descriptors, configuration, class registration, the IAD code
 * triple) is derived from Zephyr's samples/subsys/usb/common/sample_usbd_init.c,
 * Copyright (c) 2023 Nordic Semiconductor ASA, SPDX-License-Identifier: Apache-2.0 --
 * so THIS FILE is Apache-2.0, not MIT (NOTICE). Changed from the sample: our own vendor and
 * product IDs instead of Zephyr's test VID, full speed only (the nRF52840 has no high
 * speed), no BOS descriptor, and a bus-event callback that feeds MIDI.
 */
#include "sp1_usbd.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>

#if defined(CONFIG_SP1_USB_AUDIO)
#include "sp1_uac.h"
#endif

#if defined(CONFIG_SP1_MIDI)
#include <cmsis_core.h>
#include "sp1_midi.h"
#include "sp1_midi_usb.h"
#include "usb_rt_parse.h"

BUILD_ASSERT(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC == SP1_MIDI_STAMP_HZ,
	     "MIDI stamps are the system clock: SP1_MIDI_STAMP_HZ must match it");
#endif

USBD_DEVICE_DEFINE(sp1_usbd,
		   DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_SP1_USB_VID, CONFIG_SP1_USB_PID);

USBD_DESC_LANG_DEFINE(sp1_lang);
USBD_DESC_MANUFACTURER_DEFINE(sp1_mfr, CONFIG_SP1_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(sp1_product, CONFIG_SP1_USB_PRODUCT);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(sp1_sn)));
USBD_DESC_CONFIG_DEFINE(sp1_fs_cfg_desc, "FS Configuration");

/* Self-powered (we run from our own battery), as the sample's default was. */
USBD_CONFIGURATION_DEFINE(sp1_fs_config, USB_SCD_SELF_POWERED,
			  CONFIG_SP1_USB_MAX_POWER, &sp1_fs_cfg_desc);

static volatile bool host_configured;

bool sp1_usbd_host(void)
{
	return host_configured;
}

/* ---- bus events ----
 * The class callbacks report enable / disable / suspend / resume. A pulled cable and a bus
 * reset are reported here instead; both mean every MIDI controller should go back to
 * neutral, which sp1_midi_port(false) asks for. Runs in the usbd thread. */
static void msg_cb(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	ARG_UNUSED(ctx);
	/* A host is attached once it has CONFIGURED us (sp1_usbd_host). */
	switch (msg->type) {
	case USBD_MSG_CONFIGURATION:
		host_configured = (msg->status != 0);
		break;
	case USBD_MSG_VBUS_REMOVED:
	case USBD_MSG_RESET:
		host_configured = false;
		break;
	default:
		break;
	}
#if defined(CONFIG_SP1_MIDI)
	switch (msg->type) {
	case USBD_MSG_VBUS_REMOVED:
	case USBD_MSG_RESET:
	case USBD_MSG_SUSPEND:
		sp1_midi_port(false);
		break;
	default:
		break;
	}
#endif
#if defined(CONFIG_SP1_USB_AUDIO)
	/* M5c: a pulled cable or a bus reset closes the audio stream, a resume re-primes it. */
	sp1_uac_bus_event(msg->type);
#endif
}

#if defined(CONFIG_SP1_USB_THREADS_BELOW_AUDIO)
/* ---- the USB threads below the audio thread (#32) ----
 * Zephyr starts its two USB threads -- the driver's `udc_nrfx` and the stack's `usbd` -- at
 * K_PRIO_COOP(8), above the audio thread (K_PRIO_PREEMPT(0)), so every bit of their work
 * lands inside an audio block: enumeration made one 2 ms block run ~5 ms over. MIDI no longer
 * needs them (it is received and stamped in the USB interrupt, CONFIG_UDC_NRF_OUT_FAST), so
 * they move to main's priority: enumeration, the console and control requests then run in
 * the time audio leaves, and cannot push a block over. The stack synchronises them with
 * mutexes, events and queues, because applications call it from preemptible threads. */
static void usb_thread_find(const struct k_thread *t, void *user_data)
{
	k_tid_t *found = user_data;
	const char *name = k_thread_name_get((k_tid_t)t);

	if (name == NULL) {
		return;
	}
	if (strcmp(name, "udc_nrfx") == 0) {
		found[0] = (k_tid_t)t;
	} else if (strcmp(name, "usbd") == 0) {
		found[1] = (k_tid_t)t;
	} else if (strcmp(name, "usbd_msc") == 0) {
		found[2] = (k_tid_t)t;
	}
}

static int usb_threads_below_audio(void)
{
	k_tid_t found[3] = { NULL, NULL, NULL };
	int n = 0;

	k_thread_foreach_unlocked(usb_thread_find, found);
	for (int i = 0; i < 2; i++) {
		if (found[i] != NULL) {
			k_thread_priority_set(found[i], K_PRIO_PREEMPT(CONFIG_MAIN_THREAD_PRIORITY));
			n++;
		}
	}
	/* M6 drive mode: the mass-storage class runs every SCSI command -- and with it every
	 * eMMC transfer -- in its own thread, which Zephyr starts at the system work queue's
	 * COOPERATIVE priority. Left there, a host polling the drive while ON would run inside
	 * audio blocks. One step below main: in STANDBY it gets everything main leaves; in ON
	 * it only answers "no medium". Not counted in `n`, which the boot log reads as "the
	 * two core USB threads moved". */
	if (found[2] != NULL) {
		k_thread_priority_set(found[2], K_PRIO_PREEMPT(CONFIG_MAIN_THREAD_PRIORITY + 1));
	}
	return n;
}
#endif

static int threads_demoted;   /* for the boot log: how many of the two were moved */

int sp1_usbd_threads_demoted(void)
{
	return threads_demoted;
}

int sp1_usbd_init(void)
{
#if defined(CONFIG_SP1_USB_THREADS_BELOW_AUDIO)
	threads_demoted = usb_threads_below_audio();
#endif
	int err = usbd_add_descriptor(&sp1_usbd, &sp1_lang);
	if (err == 0) {
		err = usbd_add_descriptor(&sp1_usbd, &sp1_mfr);
	}
	if (err == 0) {
		err = usbd_add_descriptor(&sp1_usbd, &sp1_product);
	}
	IF_ENABLED(CONFIG_HWINFO, (
		if (err == 0) {
			err = usbd_add_descriptor(&sp1_usbd, &sp1_sn);
		}
	))
	if (err == 0) {
		err = usbd_add_configuration(&sp1_usbd, USBD_SPEED_FS, &sp1_fs_config);
	}
	if (err == 0) {
		/* Every compiled-in class: CDC ACM (devicetree), feldd's MIDI class
		 * (USBD_DEFINE_CLASS in usb_midi1.c) and USB audio out (wakes_uac, sp1_uac.c),
		 * in that order -- linker order is by name (sp1_uac.c says why it matters).
		 * Mass storage is held back from that list and added LAST (M6): by name it would
		 * land between CDC and MIDI and renumber every interface after it. */
		static const char *const later[] = { "msc_0", NULL };
		err = usbd_register_all_classes(&sp1_usbd, USBD_SPEED_FS, 1, later);
	}
#if defined(CONFIG_SP1_DRIVE)
	if (err == 0) {
		err = usbd_register_class(&sp1_usbd, "msc_0", USBD_SPEED_FS, 1);
	}
#endif
	if (err == 0) {
		/* The function set's version (Kconfig SP1_USB_BCD_DEVICE): changes when the
		 * functions do, so the PID never has to. */
		err = usbd_device_set_bcd_device(&sp1_usbd, CONFIG_SP1_USB_BCD_DEVICE);
	}
	if (err != 0) {
		return err;
	}
	/* CDC ACM carries an Interface Association Descriptor, so the device says "look at
	 * the interfaces" (Miscellaneous / IAD), as the sample did. The MIDI function has no
	 * IAD of its own -- feldd's OP-XY fix -- and groups its two interfaces the MIDI 1.0
	 * way, through the AudioControl header. */
	usbd_device_set_code_triple(&sp1_usbd, USBD_SPEED_FS, USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	usbd_self_powered(&sp1_usbd, true);
	err = usbd_msg_register_cb(&sp1_usbd, msg_cb);
	if (err == 0) {
		err = usbd_init(&sp1_usbd);
	}
	if (err == 0) {
		err = usbd_enable(&sp1_usbd);
	}
	return err;
}

#if defined(CONFIG_SP1_MIDI)
/* Packets nothing took (sp1_midi.h, sp1_midi_usb_rejects). The USB interrupt writes (#32),
 * main reads. */
static volatile uint32_t rej_count;
static volatile uint8_t rej_last[4];
static volatile uint32_t rt_cin5;   /* real-time bytes taken from CIN 0x5 packets */

void sp1_midi_usb_rejects(uint32_t *count, uint8_t last[4])
{
	*count = rej_count;
	for (int k = 0; k < 4; k++) {
		last[k] = rej_last[k];
	}
}

uint32_t sp1_midi_usb_rt_cin5(void)
{
	return rt_cin5;
}

/* ---- the calls feldd's patched class makes (sp1_midi_usb.h) ---- */
void sp1_midi_usb_rx(const uint8_t *data, size_t len)
{
	/* One stamp for the whole buffer: it arrived in one USB transfer. The audio thread
	 * places it within its block from this (sp1_midi.h, "timing": the system clock, not the
	 * cycle counter, which stops while the CPU sleeps). */
	const uint32_t now = k_cycle_get_32();
	for (size_t i = 0; i + 4u <= len; i += 4u) {
		uint8_t msg[3];
		/* feldd's validators: a channel message (status nibble must match the packet's
		 * CIN, data bytes 7-bit), or -- M5b -- one of the four real-time bytes Marbles
		 * follows: Clock, Start, Continue, Stop. Everything else (SysEx, system common,
		 * active sensing, reset) is dropped here. */
		const uint8_t n = usb_midi_extract_voice(&data[i], msg);
		if (n != 0u) {
			sp1_midi_push(msg, n, now);
			continue;
		}
		msg[0] = usb_midi_extract_rt(&data[i]);
		/* ...and the same four bytes under CIN 0x5 ("single-byte system common"), which
		 * some hosts use for real-time instead of the spec's CIN 0xF -- suspected of the
		 * OP-XY (2026-10-02: ~192 ticks arrived in a 2.5 min session at 164 BPM, and Start /
		 * Stop never did). Counted separately, so the log says which form a host sends. */
		if (msg[0] == 0u && (data[i] & 0x0Fu) == 0x5u &&
		    (data[i + 1u] == 0xF8u || data[i + 1u] == 0xFAu ||
		     data[i + 1u] == 0xFBu || data[i + 1u] == 0xFCu)) {
			msg[0] = data[i + 1u];
			rt_cin5 = rt_cin5 + 1u;
		}
		if (msg[0] != 0u) {
			if (SP1_MIDI_CLOCK) {               /* the script's `clock` */
				sp1_midi_push(msg, 1u, now);
			}
			continue;
		}
		/* SysEx (CIN 0x4-0x7): MMC transport, which the OP-XY sends instead of Start /
		 * Stop (M5b round 2). A finished SysEx that is not MMC play / stop is counted
		 * as ignored below; the packets of one still being assembled are not. */
		const uint8_t cin = data[i] & 0x0Fu;
		bool ignored = true;
		if (cin >= 0x4u && cin <= 0x7u) {
			const uint8_t mmc = sp1_midi_mmc_feed(&data[i]);
			if (mmc > 1u) {
				if (SP1_MIDI_CLOCK) {
					msg[0] = mmc;
					sp1_midi_push(msg, 1u, now);
				}
				continue;
			}
			ignored = (mmc == 1u);
		}
		/* Neither: counted, and the last one kept for the log (diagnostics -- e.g. a
		 * host sending transport in a packet type we do not expect). An all-zero
		 * packet is padding, not a message. */
		if (ignored && (data[i] | data[i + 1u] | data[i + 2u] | data[i + 3u])) {
			for (int k = 0; k < 4; k++) {
				rej_last[k] = data[i + (size_t)k];
			}
			rej_count = rej_count + 1u;
		}
	}
}

void sp1_midi_usb_port(bool up)
{
	sp1_midi_port(up);
}
#endif
