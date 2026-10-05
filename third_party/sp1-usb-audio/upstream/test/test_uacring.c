/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* test_uacring.c -- host tests for the UAC2 capture ring and packet regulator. */
#include "uacring.h"

#include <stdio.h>
#include <string.h>

/* The producer's block: the firmware pushes one audio block per render. Built
 * at both sizes by prep.sh (-DAUDIO_BLK_FRAMES=256 for the other). */
#ifndef AUDIO_BLK_FRAMES
#define AUDIO_BLK_FRAMES 128u
#endif
#define BLK ((uint32_t)AUDIO_BLK_FRAMES)

static int checks, failures;

#define CHECK(cond, msg) do {                                                 \
	checks++;                                                               \
	if (!(cond)) { failures++;                                               \
		printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, (msg)); }       \
} while (0)

static void pattern(int16_t *lr, uint32_t first, uint32_t frames)
{
	for (uint32_t i = 0; i < frames; i++) {
		lr[i * 2u] = (int16_t)(first + i);
		lr[i * 2u + 1u] = (int16_t)~(first + i);
	}
}

/* The producer is the firmware's audio thread: it takes an I2S slot when one
 * frees -- paced by the audio clock, `ppm` off the host's -- claims the block,
 * renders it, then pushes it. Render time sets when the push lands.
 *
 * steps: render-timing steps, as a chord starting and stopping does -- every
 * 30 s renders go from light to near a whole block (RENDER_HI) and back, and one
 * block in 997 takes RENDER_PEAK: at 128-sample blocks the bench saw 3417 us, 28%
 * over the slot (worst-case patch re-struck, 2026-09-23); at 256 nothing above
 * ~4.7 ms, so a whole block is the bound. The regulator must not read any of it as clock drift: at
 * 2% for ~150 ms that was an audible click on macOS (2026-09-23). */
#define PERIOD_US  (BLK * 1000000.0 / 48000.0)
#define RENDER_LO  300.0
#define RENDER_HI   (BLK == 256u ? 4700.0 : 2500.0)  /* the heaviest held chords */
#define RENDER_PEAK (BLK == 256u ? 5333.0 : 3417.0)

static void run_ppm(int ppm, bool steps)
{
	struct uacring r;
	int16_t in[AUDIO_BLK_FRAMES * 2], out[49 * 2];
	uint32_t produced = 0, expected = 0, blocks = 0;
	/* Slot k frees at k * period on the audio clock, seen from the host's. */
	const double period = PERIOD_US / (1.0 + ppm * 1e-6);
	double next_slot = 0.0, busy_until = 0.0, push_at = 0.0;
	bool rendering = false;
	bool heard = false;
	uint32_t win_fix = 0, win_max = 0, last_fix = 0;

	uacring_init(&r);
	uacring_stream(&r, true);
	for (uint32_t ms = 0; ms < 600000u; ms++) {
		const double now = ms * 1000.0;
		const bool heavy = steps && (ms / 30000u) % 2u;
		for (;;) {
			if (rendering && push_at <= now) {
				pattern(in, produced, BLK);
				CHECK(uacring_push(&r, in, BLK), "producer block fits");
				produced += BLK;
				rendering = false;
				busy_until = push_at;
				continue;
			}
			const double start = next_slot > busy_until ? next_slot : busy_until;
			if (!rendering && start <= now) {
				double render = heavy ? RENDER_HI : RENDER_LO;
				if (steps && ++blocks % 997u == 0u) render = RENDER_PEAK;
				uacring_claim(&r, BLK);
				rendering = true;
				push_at = start + render;
				next_slot += period;
				continue;
			}
			break;
		}

		uint32_t n = uacring_packet_frames(&r);
		bool audio = uacring_pop(&r, out, n);
		if (audio) {
			if (!heard) {
				/* Priming drops its overshoot: at most one block, once. */
				expected = (uint16_t)out[0];
				CHECK(expected < BLK, "priming skips less than a block");
			}
			heard = true;
			for (uint32_t i = 0; i < n; i++) {
				if (out[i * 2u] != (int16_t)expected ||
				    out[i * 2u + 1u] != (int16_t)~expected) {
					CHECK(false, "output stream preserves every frame");
					return;
				}
				expected++;
			}
		}
		CHECK(r.write - r.read < UACRING_CAPACITY, "fill remains in range");

		/* Corrections per 1000 packets: the host must only ever see drift. */
		const uint32_t fixes = r.n47 + r.n49;
		win_fix += fixes - last_fix;
		last_fix = fixes;
		if (ms % 1000u == 999u) {
			if (win_fix > win_max) win_max = win_fix;
			win_fix = 0;
		}
	}
	CHECK(win_max <= 20, "at most 20 corrections a second (the host hears no clock jump)");
	if (steps && ppm == 0) {
		CHECK(r.n47 + r.n49 <= 60,
		      "render-timing steps are not read as drift (few corrections at 0 ppm)");
	}

	CHECK(heard, "stream left priming");
	CHECK(r.under == 0, "no underflow after priming");
	CHECK(r.over == 0, "no overflow while regulated");
	if (ppm < 0) CHECK(r.n47 > r.n49, "slow producer selects more 47-frame packets");
	if (ppm > 0) CHECK(r.n49 > r.n47, "fast producer selects more 49-frame packets");
	if (ppm == 0) {
		CHECK(r.n47 < 1000 && r.n49 < 1000, "zero ppm needs few corrections");
		CHECK(r.n47 > r.n49 ? r.n47 - r.n49 < 100 : r.n49 - r.n47 < 100,
		      "zero ppm corrections balance");
	}
}

int main(void)
{
	struct uacring r;
	int16_t block[AUDIO_BLK_FRAMES * 2], out[49 * 2];
	struct uacring_stats st;
	static const int ppms[] = { -200, -50, 0, 50, 200 };

	for (uint32_t i = 0; i < sizeof ppms / sizeof ppms[0]; i++) {
		run_ppm(ppms[i], false);
		run_ppm(ppms[i], true);
	}

	uacring_init(&r);
	pattern(block, 0, BLK);
	CHECK(!uacring_push(&r, block, BLK), "disabled ring rejects input");
	uacring_stream(&r, true);
	CHECK(uacring_push(&r, block, BLK), "enabled ring accepts input");
	CHECK(uacring_packet_frames(&r) == 48 && !uacring_pop(&r, out, 48),
	      "priming emits 48 silent frames");
	CHECK(r.under == 0, "priming silence is not an underflow");
	/* Top up to one block short of UACRING_PRIME; the next block reaches it. */
	while (r.write - r.read + BLK < UACRING_PRIME) {
		CHECK(uacring_push(&r, block, BLK), "priming block fits");
		CHECK(uacring_packet_frames(&r) == 48 && !uacring_pop(&r, out, 48),
		      "still priming below UACRING_PRIME");
	}
	CHECK(uacring_push(&r, block, BLK), "second block reaches prime target");
	uint32_t n = uacring_packet_frames(&r);
	CHECK(uacring_pop(&r, out, n), "audio starts after target fill");
	r.read = r.write;
	n = uacring_packet_frames(&r);
	CHECK(n == 48 && !uacring_pop(&r, out, n) && r.under == 1,
	      "underflow emits one silent 48-frame packet and counts it");
	uacring_stream(&r, false);
	CHECK(!r.streaming, "disable stops streaming");
	uacring_stream(&r, true);
	uacring_stats(&r, &st);
	CHECK(st.on && st.fill == 0 && st.packets == 0, "re-enable resets ring and counters");
	for (uint32_t b = 0; b < UACRING_CAPACITY / BLK; b++) {
		CHECK(uacring_push(&r, block, BLK), "overflow setup block");
	}
	CHECK(!uacring_push(&r, block, BLK) && r.over == 1,
	      "overflow drops the whole block and counts it");
	CHECK(r.write - r.read == UACRING_CAPACITY, "overflow does not partly write");

	printf("uacring (%u-frame blocks): %d checks, %d failures\n", (unsigned)BLK, checks, failures);
	return failures ? 1 : 0;
}
