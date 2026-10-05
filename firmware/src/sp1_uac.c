/*
 * Copyright (c) 2026 Ryan Gilmore
 * Copyright (c) 2026 Adara Barami | Worldwave
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* wakes-sp1 -- USB audio OUT (M5c). See sp1_uac.h.
 *
 * The SP-1's output goes to the host as a USB Audio Class 1.0 input: 48 kHz, 16-bit,
 * stereo, the selected output configuration after VOL (M5-PLAN, A and E2). Nothing is
 * received from the host: there is no ISO OUT endpoint and no feedback endpoint.
 *
 * ---- why a hand-rolled UAC1 class ----
 * One class, the cheapest on CPU and the most stable across the OP-XY and computers (Adara,
 * 2026-10-05). The audio data costs the same either way: every packet goes through the ISO IN
 * fast path in the USB interrupt (zephyr-patches/udc_nrf-fast-paths.patch), and a class only
 * runs on the host's occasional control requests. What differs is stability. UAC2 requires an
 * Interface Association Descriptor, and an IAD in front of an Audio function is what feldd found
 * hard-faults the OP-XY (third_party/feldd). UAC1 has no IAD and no clock entities -- the same
 * class family as the USB-MIDI 1.0 port the OP-XY already accepts -- and every OS has carried a
 * UAC1 driver for decades. Zephyr has no UAC1 class, so this is one, in the shape of feldd's
 * MIDI class (static descriptors, a usbd_class_api, no devicetree node).
 *
 * ---- what is Ryan's ----
 * Derived from Ryan Gilmore's sp1-usb-audio (third_party/sp1-usb-audio, Apache-2.0): the ring
 * and packet regulator are his uacring.c (with Wakes' word-copy patch and 96-frame tuning), and
 * the fill function, the stream start/stop and the bus-event handling below follow his
 * usb_audio.c, which glues the same pieces to Zephyr's UAC2 class instead.
 *
 * ---- threads ----
 *   - the class callbacks and sp1_uac_bus_event(): the usbd thread (below audio, #32)
 *   - fill(): the USB interrupt, once a millisecond while the stream is open
 *   - sp1_uac_claim() / sp1_uac_push(): the audio thread
 * The ring is single-producer / single-consumer (uacring.h); the usbd thread only starts and
 * stops it, with the interrupt held off while the consumer side is reset.
 *
 * ---- CPU (Adara's go-ahead, 2026-10-05) ----
 * Only while the host has the stream open: the 1 kHz SOF interrupt (the driver turns it on with
 * the fast path and off again, so a device that is not streaming pays nothing), one 192-byte
 * word copy in it, and one 384-byte word copy per 2 ms block in the audio thread. ~0.65 points
 * for the copies by the Cortex-M4 disassembly; measure the whole on hardware. */
#include "sp1_uac.h"

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/net_buf.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/drivers/usb/udc_nrf_fast.h>

#include "sp1_uac_tuning.h"     /* before uacring.h, as the ring itself is built */
#include "uacring.h"

/* ---- USB Device Class Definition for Audio Devices, Release 1.0 ---- */
#define UAC_CLASS               0x01    /* bInterfaceClass: Audio */
#define UAC_SUBCLASS_CONTROL    0x01
#define UAC_SUBCLASS_STREAMING  0x02
/* A.5 / A.6 / A.8: class-specific descriptor subtypes */
#define UAC_AC_HEADER           0x01
#define UAC_AC_INPUT_TERMINAL   0x02
#define UAC_AC_OUTPUT_TERMINAL  0x03
#define UAC_AS_GENERAL          0x01
#define UAC_AS_FORMAT_TYPE      0x02
#define UAC_EP_GENERAL          0x01
/* Terminal types (USB Audio Terminal Types 1.0): the stream to the host is a USB streaming
 * output terminal, fed by an input terminal that is the SP-1's line output. "Line connector"
 * is the conventional type for an instrument's capture input; hosts name the input after it.
 * (Ryan's UAC2 build uses "synthesizer", 0x0713, and worked on macOS.) */
#define UAC_TT_USB_STREAMING    0x0101
#define UAC_TT_LINE_CONNECTOR   0x0603
/* A.10 / A.9: format and requests */
#define UAC_FORMAT_TYPE_I       0x01
#define UAC_FORMAT_PCM          0x0001
#define UAC_SET_CUR             0x01
#define UAC_GET_CUR             0x81
#define UAC_GET_MIN             0x82
#define UAC_GET_MAX             0x83
#define UAC_GET_RES             0x84
#define UAC_EP_SAMPLING_FREQ    0x01    /* endpoint control selector */

#define IT_ID                   1
#define OT_ID                   2
#define SAMPLE_RATE             48000u
#define CHANNELS                2u
#define FRAME_BYTES             (CHANNELS * 2u)
/* 49 frames: the largest packet the regulator sends (uacring.h). */
#define MAX_PACKET              (49u * FRAME_BYTES)
/* Isochronous, asynchronous (bmAttributes bits 1:0 = 01, 3:2 = 01): the SP-1's own 3.072 MHz
 * clock paces the audio and the regulator sizes packets 47/48/49 to it -- no feedback
 * endpoint, so none of the host-side quirks that come with one. */
#define EP_ISO_ASYNC            0x05
/* A hint: the stack assigns the real address at usbd_init, and on the nRF52840 the only
 * isochronous IN endpoint is 0x88 (udc_nrf's ISO caps), which the fast path serves. */
#define EP_IN_HINT              0x81

BUILD_ASSERT(MAX_PACKET <= UDC_NRF_ISO_IN_FAST_MAX, "a packet must fit the fast path's buffer");
BUILD_ASSERT(AUDIO_BLK_FRAMES == CONFIG_SP1_AUDIO_BLOCK_FRAMES,
	     "the ring is tuned for another block (sp1_uac_tuning.h)");

struct uac_ac_header {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint16_t bcdADC;
	uint16_t wTotalLength;
	uint8_t bInCollection;
	uint8_t baInterfaceNr1;
} __packed;

struct uac_input_terminal {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bTerminalID;
	uint16_t wTerminalType;
	uint8_t bAssocTerminal;
	uint8_t bNrChannels;
	uint16_t wChannelConfig;
	uint8_t iChannelNames;
	uint8_t iTerminal;
} __packed;

struct uac_output_terminal {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bTerminalID;
	uint16_t wTerminalType;
	uint8_t bAssocTerminal;
	uint8_t bSourceID;
	uint8_t iTerminal;
} __packed;

struct uac_as_general {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bTerminalLink;
	uint8_t bDelay;
	uint16_t wFormatTag;
} __packed;

struct uac_format_type_i {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bFormatType;
	uint8_t bNrChannels;
	uint8_t bSubframeSize;
	uint8_t bBitResolution;
	uint8_t bSamFreqType;
	uint8_t tSamFreq[3];
} __packed;

/* The Audio 1.0 endpoint descriptor: the standard 7 bytes + bRefresh + bSynchAddress. feldd
 * found that strict hosts drop a port whose audio-class endpoint is the 7-byte kind. The stack
 * rewrites bEndpointAddress and wMaxPacketSize through the 7-byte layout, which is a prefix. */
struct uac_iso_ep {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bEndpointAddress;
	uint8_t bmAttributes;
	uint16_t wMaxPacketSize;
	uint8_t bInterval;
	uint8_t bRefresh;
	uint8_t bSynchAddress;
} __packed;

struct uac_cs_iso_ep {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bmAttributes;
	uint8_t bLockDelayUnits;
	uint16_t wLockDelay;
} __packed;

struct uac_descriptors {
	struct usb_if_descriptor ac;            /* AudioControl, no endpoints */
	struct uac_ac_header ac_header;
	struct uac_input_terminal it;
	struct uac_output_terminal ot;
	struct usb_if_descriptor as0;           /* AudioStreaming, zero bandwidth */
	struct usb_if_descriptor as1;           /* AudioStreaming, the stream */
	struct uac_as_general as_general;
	struct uac_format_type_i format;
	struct uac_iso_ep ep;
	struct uac_cs_iso_ep cs_ep;
};

#define AC_TOTAL_LENGTH (sizeof(struct uac_ac_header) + sizeof(struct uac_input_terminal) + \
			 sizeof(struct uac_output_terminal))

static struct uac_descriptors desc = {
	.ac = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,                  /* stamped at usbd_init */
		.bAlternateSetting = 0,
		.bNumEndpoints = 0,
		.bInterfaceClass = UAC_CLASS,
		.bInterfaceSubClass = UAC_SUBCLASS_CONTROL,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	.ac_header = {
		.bLength = sizeof(struct uac_ac_header),
		.bDescriptorType = USB_DESC_CS_INTERFACE,
		.bDescriptorSubtype = UAC_AC_HEADER,
		.bcdADC = sys_cpu_to_le16(0x0100),
		.wTotalLength = sys_cpu_to_le16(AC_TOTAL_LENGTH),
		.bInCollection = 1,
		.baInterfaceNr1 = 1,                    /* the AS interface; fixed up in init */
	},
	.it = {
		.bLength = sizeof(struct uac_input_terminal),
		.bDescriptorType = USB_DESC_CS_INTERFACE,
		.bDescriptorSubtype = UAC_AC_INPUT_TERMINAL,
		.bTerminalID = IT_ID,
		.wTerminalType = sys_cpu_to_le16(UAC_TT_LINE_CONNECTOR),
		.bAssocTerminal = 0,
		.bNrChannels = CHANNELS,
		.wChannelConfig = sys_cpu_to_le16(0x0003),      /* left front, right front */
		.iChannelNames = 0,
		.iTerminal = 0,
	},
	.ot = {
		.bLength = sizeof(struct uac_output_terminal),
		.bDescriptorType = USB_DESC_CS_INTERFACE,
		.bDescriptorSubtype = UAC_AC_OUTPUT_TERMINAL,
		.bTerminalID = OT_ID,
		.wTerminalType = sys_cpu_to_le16(UAC_TT_USB_STREAMING),
		.bAssocTerminal = 0,
		.bSourceID = IT_ID,
		.iTerminal = 0,
	},
	.as0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 1,                  /* stamped at usbd_init */
		.bAlternateSetting = 0,
		.bNumEndpoints = 0,
		.bInterfaceClass = UAC_CLASS,
		.bInterfaceSubClass = UAC_SUBCLASS_STREAMING,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	.as1 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 1,                  /* stamped at usbd_init */
		.bAlternateSetting = 1,
		.bNumEndpoints = 1,
		.bInterfaceClass = UAC_CLASS,
		.bInterfaceSubClass = UAC_SUBCLASS_STREAMING,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	.as_general = {
		.bLength = sizeof(struct uac_as_general),
		.bDescriptorType = USB_DESC_CS_INTERFACE,
		.bDescriptorSubtype = UAC_AS_GENERAL,
		.bTerminalLink = OT_ID,
		.bDelay = 1,
		.wFormatTag = sys_cpu_to_le16(UAC_FORMAT_PCM),
	},
	.format = {
		.bLength = sizeof(struct uac_format_type_i),
		.bDescriptorType = USB_DESC_CS_INTERFACE,
		.bDescriptorSubtype = UAC_AS_FORMAT_TYPE,
		.bFormatType = UAC_FORMAT_TYPE_I,
		.bNrChannels = CHANNELS,
		.bSubframeSize = 2,
		.bBitResolution = 16,
		.bSamFreqType = 1,                      /* one discrete rate */
		.tSamFreq = { SAMPLE_RATE & 0xFFu, (SAMPLE_RATE >> 8) & 0xFFu,
			      (SAMPLE_RATE >> 16) & 0xFFu },
	},
	.ep = {
		.bLength = sizeof(struct uac_iso_ep),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = EP_IN_HINT,         /* assigned at usbd_init: 0x88 */
		.bmAttributes = EP_ISO_ASYNC,
		.wMaxPacketSize = sys_cpu_to_le16(MAX_PACKET),
		.bInterval = 1,                         /* every 1 ms frame */
		.bRefresh = 0,
		.bSynchAddress = 0,
	},
	.cs_ep = {
		.bLength = sizeof(struct uac_cs_iso_ep),
		.bDescriptorType = USB_DESC_CS_ENDPOINT,
		.bDescriptorSubtype = UAC_EP_GENERAL,
		/* No sampling-frequency control advertised: there is one rate. A host that sets
		 * it anyway is answered (control_to_dev / control_to_host below). */
		.bmAttributes = 0,
		.bLockDelayUnits = 0,
		.wLockDelay = 0,
	},
};

static const struct usb_desc_header *fs_desc[] = {
	/* No IAD (see the top of this file): an Audio 1.0 function is grouped by its
	 * AudioControl header's collection, exactly as the MIDI function is. */
	(struct usb_desc_header *)&desc.ac,
	(struct usb_desc_header *)&desc.ac_header,
	(struct usb_desc_header *)&desc.it,
	(struct usb_desc_header *)&desc.ot,
	(struct usb_desc_header *)&desc.as0,
	(struct usb_desc_header *)&desc.as1,
	(struct usb_desc_header *)&desc.as_general,
	(struct usb_desc_header *)&desc.format,
	(struct usb_desc_header *)&desc.ep,
	(struct usb_desc_header *)&desc.cs_ep,
	NULL,
};

/* What hosts may call the input (Adara, 2026-10-05). On the AudioControl and both
 * AudioStreaming interfaces and on the input terminal, so whichever one a host reads says the
 * same; many show the device's product string (CONFIG_SP1_USB_PRODUCT, "Wakes") instead. */
USBD_DESC_STRING_DEFINE(uac_name, "Wakes Audio Out", USBD_DUT_STRING_INTERFACE);

static struct uacring ring;
static volatile bool stream_open;
static volatile uint32_t opens;
static volatile uint32_t fills;         /* packets handed to the driver (the interrupt) */
static volatile uint32_t sr_requests;

/* ---- the fast path: the USB interrupt, once a millisecond while the stream is open ----
 * Ryan's uac2_fast_fill: short, lock-free, never blocking. uacring_pop() writes silence when
 * there is nothing to send (priming, an underflow), so every frame carries a whole packet. */
static uint16_t fill(uint8_t *buf, uint16_t max, void *user_data)
{
	ARG_UNUSED(user_data);
	uint32_t frames = uacring_packet_frames(&ring);

	fills = fills + 1u;     /* the host is running frames: sp1_uac_live() */

	if (frames * FRAME_BYTES > max) {
		frames = max / FRAME_BYTES;
	}
	(void)uacring_pop(&ring, (int16_t *)buf, frames);
	return (uint16_t)(frames * FRAME_BYTES);
}

/* ---- starting and stopping (the usbd thread) ---- */
static void ring_restart(void)
{
	/* The consumer is the USB interrupt: keep it out while the ring's consumer state is
	 * reset. A few microseconds. (The producer may be mid-push; uacring_stream() only moves
	 * the consumer's index, so that is safe -- uacring.c.) */
	const unsigned int key = irq_lock();

	uacring_stream(&ring, true);
	irq_unlock(key);
}

static void stream_on(void)
{
	ring_restart();
	stream_open = true;
	opens = opens + 1u;
	/* Registering also turns the SOF interrupt on; suppress SOF events to the stack, since
	 * no class here needs them and each one would wake the usbd thread. */
	udc_nrf_iso_in_fast_set(fill, NULL, true);
}

static void stream_off(void)
{
	if (!stream_open) {
		return;
	}
	/* Unregister first (and with it the SOF interrupt): from here the interrupt no longer
	 * reads the ring, so stopping it races nothing. */
	udc_nrf_iso_in_fast_set(NULL, NULL, true);
	stream_open = false;

	const unsigned int key = irq_lock();

	uacring_stream(&ring, false);
	irq_unlock(key);
}

void sp1_uac_bus_event(int type)
{
	switch (type) {
	case USBD_MSG_VBUS_REMOVED:
	case USBD_MSG_RESET:
		/* The class hears a stream stop only as alternate setting 0. A pulled cable never
		 * sends one, and a bus reset returns every interface to 0 without telling the
		 * class (Ryan: "the stream stayed on and the unit stayed on battery"). The host
		 * selects the stream again when it wants it. */
		stream_off();
		break;
	case USBD_MSG_RESUME:
		/* Nothing drained while the host slept, so the ring is full: a stream resumed on
		 * that would run ~40 ms late and drift back for minutes. Re-prime it. ⚠️ Not yet
		 * exercised anywhere (upstream: macOS closes the stream before it sleeps). */
		if (stream_open) {
			ring_restart();
		}
		break;
	default:
		break;
	}
}

bool sp1_uac_open(void)
{
	return stream_open;
}

bool sp1_uac_live(void)
{
	/* "Open" is only what the host last SAID. A host that opened the stream and then
	 * stopped running the bus -- asleep, suspended, or stalled -- calls fill() no more,
	 * because fill() runs at each start-of-frame. So: live while fill() has run since the
	 * previous call. One caller (sp1_audio.c's speaker check, every 40 ms ~ 40 frames). */
	static uint32_t seen;
	const uint32_t n = fills;
	const bool moved = (n != seen);

	seen = n;
	return stream_open && moved;
}

/* ---- the producer (the audio thread) ---- */
void sp1_uac_claim(uint32_t frames)
{
	if (stream_open) {
		uacring_claim(&ring, frames);
	}
}

void sp1_uac_push(const int16_t *lr, uint32_t frames)
{
	if (stream_open) {
		(void)uacring_push(&ring, lr, frames);
	}
}

void sp1_uac_get_stats(struct sp1_uac_stats *st)
{
	struct uacring_stats rs;
	struct udc_nrf_iso_in_fast_stats ds;

	uacring_stats(&ring, &rs);
	udc_nrf_iso_in_fast_stats_get(&ds);
	st->open = stream_open;
	st->opens = opens;
	st->packets = rs.packets;
	st->n47 = rs.n47;
	st->n49 = rs.n49;
	st->under = rs.under;
	st->over = rs.over;
	st->fill = rs.fill;
	st->target = UACRING_TARGET;
	st->replaced = ds.replaced;
	st->sr_requests = sr_requests;
}

/* ---- usbd_class_api (the usbd thread) ---- */
static void uac_update(struct usbd_class_data *const c_data, uint8_t iface, uint8_t alternate)
{
	ARG_UNUSED(c_data);
	if (iface != desc.as0.bInterfaceNumber) {
		return;
	}
	/* The stack has already enabled (alternate 1) or disabled (alternate 0) the endpoint
	 * when this runs (usbd_interface_set_alt), so the fast path finds it ready. */
	if (alternate == 1u) {
		stream_on();
	} else {
		stream_off();
	}
}

static bool is_sample_rate_request(const struct usb_setup_packet *const setup)
{
	return setup->RequestType.type == USB_REQTYPE_TYPE_CLASS &&
	       setup->RequestType.recipient == USB_REQTYPE_RECIPIENT_ENDPOINT &&
	       (setup->wIndex & 0xFFu) == desc.ep.bEndpointAddress &&
	       (setup->wValue >> 8) == UAC_EP_SAMPLING_FREQ;
}

/* Not advertised (desc.cs_ep), but answered: SET_CUR to 48 kHz is accepted; anything else
 * this function does not have is refused (a STALL), as the class convention is to say. */
static int uac_control_to_dev(struct usbd_class_data *const c_data,
			      const struct usb_setup_packet *const setup,
			      const struct net_buf *const buf)
{
	ARG_UNUSED(c_data);
	if (is_sample_rate_request(setup) && setup->bRequest == UAC_SET_CUR &&
	    buf != NULL && buf->len >= 3u) {
		sr_requests = sr_requests + 1u;
		if (sys_get_le24(buf->data) == SAMPLE_RATE) {
			return 0;
		}
	}
	errno = -ENOTSUP;
	return 0;
}

static int uac_control_to_host(struct usbd_class_data *const c_data,
			       const struct usb_setup_packet *const setup,
			       struct net_buf *const buf)
{
	ARG_UNUSED(c_data);
	if (is_sample_rate_request(setup) && setup->wLength >= 3u &&
	    net_buf_tailroom(buf) >= 3u) {
		switch (setup->bRequest) {
		case UAC_GET_CUR:
		case UAC_GET_MIN:
		case UAC_GET_MAX:
			sr_requests = sr_requests + 1u;
			net_buf_add_le24(buf, SAMPLE_RATE);
			return 0;
		case UAC_GET_RES:
			sr_requests = sr_requests + 1u;
			net_buf_add_le24(buf, 0u);
			return 0;
		default:
			break;
		}
	}
	errno = -ENOTSUP;
	return 0;
}

static void uac_disable(struct usbd_class_data *const c_data)
{
	ARG_UNUSED(c_data);
	stream_off();
}

static void uac_resumed(struct usbd_class_data *const c_data)
{
	ARG_UNUSED(c_data);
	sp1_uac_bus_event(USBD_MSG_RESUME);
}

/* After the stack has stamped bInterfaceNumber and bEndpointAddress
 * (usbd_init.c:init_configuration_inst). The AudioControl header's reference to the streaming
 * interface is not stamped -- the same fix-up feldd's MIDI class does. */
static int uac_init(struct usbd_class_data *const c_data)
{
	desc.ac_header.baInterfaceNr1 = desc.as0.bInterfaceNumber;

	/* The name, as Zephyr's CDC ACM class adds its interface string. Once only: a string
	 * node may be added to the context one time. Without it the function still works,
	 * nameless. */
	if (desc.ac.iInterface == 0u &&
	    usbd_add_descriptor(usbd_class_get_ctx(c_data), &uac_name) == 0) {
		const uint8_t idx = usbd_str_desc_get_idx(&uac_name);

		desc.ac.iInterface = idx;
		desc.as0.iInterface = idx;
		desc.as1.iInterface = idx;
		desc.it.iTerminal = idx;
	}
	uacring_init(&ring);
	return 0;
}

static void *uac_get_desc(struct usbd_class_data *const c_data, const enum usbd_speed speed)
{
	ARG_UNUSED(c_data);
	ARG_UNUSED(speed);       /* full speed only: the nRF52840 has no high speed */
	return fs_desc;
}

static const struct usbd_class_api uac_api = {
	.update = uac_update,
	.control_to_dev = uac_control_to_dev,
	.control_to_host = uac_control_to_host,
	.disable = uac_disable,
	.resumed = uac_resumed,
	.init = uac_init,
	.get_desc = uac_get_desc,
};

/* ⚠️ The NAME sets the interface order: usbd_register_all_classes() registers in linker
 * order, which sorts by name -- cdc_acm_0, usb_midi1, wakes_uac. So the console and the MIDI
 * port keep the interface numbers hosts already know, and the audio function comes last. On
 * Windows' legacy audio grouping, a new collection starts at each AudioControl interface (the
 * first interface's subclass may not repeat), so the MIDI and audio functions stay apart. */
USBD_DEFINE_CLASS(wakes_uac, &uac_api, NULL, NULL);
