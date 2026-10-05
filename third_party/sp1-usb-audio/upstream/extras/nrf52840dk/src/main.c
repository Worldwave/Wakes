/* SPDX-License-Identifier: Apache-2.0
 *
 * UAC2 tone sample: a 1 kHz sine at -12 dBFS, 48 kHz stereo, to the host as a
 * USB audio input.
 *
 * The producer stands in for a real audio path. On an SP-1 the audio thread is
 * paced by the I2S codec's clock, one block at a time; here a thread is paced by
 * the system timer and renders however many frames the timer says are due.
 * Either way the audio runs on a clock that is not the host's USB frame clock,
 * which is exactly the drift the packet regulator (uacring.c) absorbs.
 */
#include <math.h>
#include <stdint.h>

#include <sample_usbd.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/usb/usbd.h>

#include "usb_audio.h"

#define RATE        48000u
#define TONE_PERIOD 48u          /* 1 kHz at 48 kHz: a whole cycle in 48 frames */
#define MAX_CHUNK   256u
#define TICK_MS     2

static int16_t sine[TONE_PERIOD];
static int16_t chunk[MAX_CHUNK * 2u];

int main(void)
{
	for (uint32_t i = 0; i < TONE_PERIOD; i++) {
		/* -12 dBFS: a quarter of full scale. */
		sine[i] = (int16_t)(8192.0f * sinf(2.0f * 3.14159265f * (float)i / TONE_PERIOD));
	}

	usb_audio_init();
	struct usbd_context *ctx = sample_usbd_init_device(usb_audio_usbd_msg);

	if (ctx == NULL || usbd_enable(ctx) != 0) {
		printk("usb init failed\n");
		return 0;
	}

	const uint64_t hz = (uint64_t)sys_clock_hw_cycles_per_sec();
	const uint64_t start = k_cycle_get_64();
	uint64_t produced = 0;
	uint32_t phase = 0;

	for (;;) {
		k_msleep(TICK_MS);

		/* Frames due by the system clock, so the rate is exactly 48 kHz on
		 * that clock however the sleeps round. */
		uint64_t due = (k_cycle_get_64() - start) * RATE / hz;

		while (produced < due) {
			uint32_t n = (uint32_t)MIN(due - produced, (uint64_t)MAX_CHUNK);

			/* Tell the regulator what is coming BEFORE rendering it, so a
			 * slow render never looks like clock drift. On an SP-1 this is
			 * where the audio thread takes its I2S slot. */
			usb_audio_claim(n);
			for (uint32_t i = 0; i < n; i++) {
				chunk[i * 2u] = sine[phase];
				chunk[i * 2u + 1u] = sine[phase];
				phase = (phase + 1u) % TONE_PERIOD;
			}
			usb_audio_push(chunk, n);
			produced += n;
		}
	}
	return 0;
}
