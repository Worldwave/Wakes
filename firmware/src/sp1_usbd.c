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

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/usb/usbd.h>

#if defined(CONFIG_SP1_MIDI)
#include <cmsis_core.h>
#include "sp1_midi.h"
#include "sp1_midi_usb.h"
#include "usb_rt_parse.h"
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
}

int sp1_usbd_init(void)
{
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
		/* Every compiled-in class: CDC ACM (devicetree) and feldd's MIDI class
		 * (USBD_DEFINE_CLASS in usb_midi1.c). */
		err = usbd_register_all_classes(&sp1_usbd, USBD_SPEED_FS, 1, NULL);
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
/* ---- the calls feldd's patched class makes (sp1_midi_usb.h) ---- */
void sp1_midi_usb_rx(const uint8_t *data, size_t len)
{
	/* One stamp for the whole buffer: it arrived in one USB transfer. The audio thread
	 * places it within its block from this (sp1_midi.h, "timing"). */
	const uint32_t now = DWT->CYCCNT;
	for (size_t i = 0; i + 4u <= len; i += 4u) {
		uint8_t msg[3];
		/* feldd's validator: channel messages only, status nibble must match the
		 * packet's CIN, data bytes 7-bit. SysEx, system common and real-time (M5b's
		 * clock) are not channel messages and are dropped here. */
		const uint8_t n = usb_midi_extract_voice(&data[i], msg);
		if (n != 0u) {
			sp1_midi_push(msg, n, now);
		}
	}
}

void sp1_midi_usb_port(bool up)
{
	sp1_midi_port(up);
}
#endif
