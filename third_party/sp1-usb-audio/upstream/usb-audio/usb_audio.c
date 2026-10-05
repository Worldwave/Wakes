/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UAC2 capture glue. The application pushes audio into a ring (uacring.c); once
 * per 1 ms USB frame one packet leaves it, sized 47, 48 or 49 frames to keep the
 * ring's average fill steady. That sizing is the whole clock-drift answer: the
 * audio is paced by one clock, USB frames by the host's, and there is no feedback
 * endpoint to tell the host which one is right.
 *
 * Two ways to send the packet:
 *  - stock Zephyr (default): the class's SOF callback pops a packet into a slab
 *    buffer and hands it to usbd_uac2_send();
 *  - CONFIG_UDC_NRF_ISOFAST (isofast.conf): the patched driver calls
 *    uac2_fast_fill() from the USBD interrupt at SOF and DMAs what it returns --
 *    no buffer, no send, no thread wake-up.
 */
#include "usb_audio.h"
#include "uacring.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/usb/udc_buf.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/class/usbd_uac2.h>
#ifdef CONFIG_UDC_NRF_ISOFAST
#include "udc_nrf_iso_in_fast.h"
#endif

#define UAC2_OUT_TERMINAL_ID UAC2_ENTITY_ID(DT_NODELABEL(uac2_usb_out))
/* 49 stereo 16-bit frames, the largest packet the regulator sends. */
#define UAC2_MAX_PACKET      196u

static const struct device *const uac2_dev =
	DEVICE_DT_GET(DT_NODELABEL(uac2_synth));
static struct uacring ring;
/* The stock path's packet buffers. The class asserts a release callback is
 * registered for any IN endpoint, so both paths keep them. */
K_MEM_SLAB_DEFINE_STATIC(uac2_tx_slab,
	ROUND_UP(UAC2_MAX_PACKET, UDC_BUF_GRANULARITY), 4, UDC_BUF_ALIGN);

#ifdef CONFIG_UDC_NRF_ISOFAST
/* Runs in the USBD interrupt, once a millisecond, while the capture endpoint
 * is enabled: short, lock-free, never blocking. Silence until the stream is on
 * -- the endpoint can be enabled a moment before terminal_update_cb runs. */
static uint16_t uac2_fast_fill(uint8_t *buf, uint16_t max, void *user_data)
{
	ARG_UNUSED(user_data);
	uint32_t frames = ring.streaming ? uacring_packet_frames(&ring) : 48u;

	if (frames * 4u > max) {
		frames = max / 4u;
	}
	if (ring.streaming) {
		(void)uacring_pop(&ring, (int16_t *)buf, frames);
	} else {
		memset(buf, 0, frames * 4u);
	}
	return (uint16_t)(frames * 4u);
}
#endif

static void uac2_sof_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(user_data);
#ifndef CONFIG_UDC_NRF_ISOFAST
	if (!ring.streaming) {
		return;
	}

	uint32_t frames = uacring_packet_frames(&ring);
	void *buf;

	if (k_mem_slab_alloc(&uac2_tx_slab, &buf, K_NO_WAIT) != 0) {
		return;
	}
	(void)uacring_pop(&ring, buf, frames);
	if (usbd_uac2_send(uac2_dev, UAC2_OUT_TERMINAL_ID, buf,
			   (uint16_t)(frames * 2u * sizeof(int16_t))) != 0) {
		k_mem_slab_free(&uac2_tx_slab, buf);
	}
#endif
}

static void stream_set(bool enabled)
{
#ifdef CONFIG_UDC_NRF_ISOFAST
	/* The consumer is the USBD interrupt: keep it out while the ring's
	 * consumer state is reset. A few microseconds. */
	unsigned int key = irq_lock();

	uacring_stream(&ring, enabled);
	irq_unlock(key);
#else
	uacring_stream(&ring, enabled);
#endif
}

static void uac2_terminal_update_cb(const struct device *dev, uint8_t terminal,
				    bool enabled, bool microframes, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(microframes);
	ARG_UNUSED(user_data);
	if (terminal == UAC2_OUT_TERMINAL_ID) {
		stream_set(enabled);
	}
}

void usb_audio_usbd_msg(struct usbd_context *const ctx, const struct usbd_msg *const msg)
{
	ARG_UNUSED(ctx);
	switch (msg->type) {
	case USBD_MSG_VBUS_REMOVED:
	case USBD_MSG_RESET:
		/* The class reports a stream stopping only when the host selects
		 * alternate setting 0. A pulled cable never does, and a bus reset
		 * returns every interface to 0 without telling the class. The host
		 * selects the stream again when it wants it. */
		if (ring.streaming) {
			stream_set(false);
		}
		break;
	case USBD_MSG_RESUME:
		/* Nothing drained while the host slept, so the ring is full: a stream
		 * resumed on that would run ~40 ms late. Re-prime it. */
		if (ring.streaming) {
			stream_set(true);
		}
		break;
	default:
		break;
	}
}

static void uac2_buf_release_cb(const struct device *dev, uint8_t terminal,
				void *buf, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(terminal);
	ARG_UNUSED(user_data);
	k_mem_slab_free(&uac2_tx_slab, buf);
}

static const struct uac2_ops uac2_ops = {
	.sof_cb = uac2_sof_cb,
	.terminal_update_cb = uac2_terminal_update_cb,
	.buf_release_cb = uac2_buf_release_cb,
};

void usb_audio_init(void)
{
	uacring_init(&ring);
	usbd_uac2_set_ops(uac2_dev, &uac2_ops, NULL);
#ifdef CONFIG_UDC_NRF_ISOFAST
	/* Suppress SOF events too: this function has no ISO OUT or feedback
	 * endpoint, so no class needs them, and each one wakes the usbd thread. */
	udc_nrf_iso_in_fast_set(uac2_fast_fill, NULL, true);
#endif
}

void usb_audio_claim(uint32_t frames)
{
	uacring_claim(&ring, frames);
}

void usb_audio_push(const int16_t *lr, uint32_t frames)
{
	if (ring.streaming) {
		(void)uacring_push(&ring, lr, frames);
	}
}
