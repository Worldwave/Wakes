/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio.h"
#include "gesture.h"
#include "power.h"
#include "usb_audio.h"
#include "wdt.h"

#include <math.h>
#include <sample_usbd.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usbd.h>

#define TONE_PERIOD 48u
#define USB_GAIN_Q8 128

static int16_t sine[TONE_PERIOD];

static void render(int16_t *lr, uint32_t frames)
{
	static int16_t usb[AUDIO_BLK_FRAMES * 2];
	static uint32_t phase;
	static int32_t vol_prev = 45;
	const int32_t vol_target = audio_volume_q8();
	const int32_t vol_gd = vol_target - vol_prev;

	if (frames > AUDIO_BLK_FRAMES) frames = AUDIO_BLK_FRAMES;
	for (uint32_t i = 0; i < frames; i++) {
		const int32_t s = sine[phase];
		phase = (phase + 1u) % TONE_PERIOD;

		/* The ramp spreads each volume step across one block so a Vol press does
		 * not click. */
		const int32_t g = vol_prev + vol_gd * (int32_t)(i + 1u) /
				  (int32_t)frames;
		const int16_t speaker = (int16_t)((s * g) >> 8);
		const int16_t host = (int16_t)((s * USB_GAIN_Q8) >> 8);
		lr[i * 2u] = speaker;
		lr[i * 2u + 1u] = speaker;
		usb[i * 2u] = host;
		usb[i * 2u + 1u] = host;
	}
	vol_prev = vol_target;
	usb_audio_push(usb, frames);
}

int main(void)
{
	/* The stock bootloader's roughly 5 s watchdog is already counting. */
	wdt_ensure_started();
	feed_wdt();
	power_boot();

	for (uint32_t i = 0; i < TONE_PERIOD; i++) {
		/* -12 dBFS before either output gain. */
		sine[i] = (int16_t)(8192.0f *
			sinf(2.0f * 3.14159265f * (float)i / (float)TONE_PERIOD));
	}

	feed_wdt();
	usb_audio_init();
	struct usbd_context *ctx = sample_usbd_init_device(usb_audio_usbd_msg);
	if (ctx != NULL) (void)usbd_enable(ctx);
	feed_wdt();
	audio_start(render);

	for (;;) {
		feed_wdt();
		power_poll();
		k_msleep(GESTURE_TICK_MS);
	}
	return 0;
}
