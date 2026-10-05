/*
 * Copyright (c) 2026 Ryan Gilmore
 * Copyright (c) 2026 Adara Barami | Worldwave
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* uactest.c -- USB audio out (M5c): Ryan Gilmore's ring and packet regulator
 * (third_party/sp1-usb-audio/upstream/usb-audio/uacring.c, unmodified) at Wakes' tuning
 * (firmware/src/sp1_uac_tuning.h, force-included by hostbuild.sh as the firmware does).
 *
 * Derived from upstream's test/test_uacring.c (Apache-2.0); changed: the producer model uses
 * Wakes' measured render times instead of upstream's 128/256-frame bench, the overflow check
 * allows for a ring that is not a whole number of blocks (2048 / 96), and the worst case must
 * keep a margin of fill, not merely avoid running dry.
 *
 * Run with -s / --sweep-line to print one summary line instead of the checks: the sweep in
 * README.md rebuilds it with -DUACRING_TARGET / -DUACRING_HYSTERESIS to re-tune. */
#include "uacring.h"

#include <stdio.h>
#include <string.h>

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

/* The producer is sp1_audio.c's audio thread: a render starts when I2S frees a slot -- paced
 * by the audio clock (the 3.072 MHz oscillator), `ppm` off the host's -- claims the block,
 * renders it, then pushes it.
 *
 * Render times, from the hardware logs at 2 ms blocks (private notes, #32 / v0.6.0 rc1):
 *   LO   a light patch, ~50 % of the block
 *   HI   the heaviest patch, held at its WORST measured block (97.5 %), every block -- harsher
 *        than the 82 % it averages
 *   PEAK one block in 997 at 200 %: with one queued I2S block, the longest render the speaker
 *        survives. Longer, and the speaker drops out too.
 * steps: every 30 s the load goes LO -> HI and back, as a heavy patch starting and stopping
 * does. The regulator must not read any of it as clock drift (upstream: 2 % for ~150 ms was
 * an audible click on macOS). */
#define PERIOD_US   (BLK * 1000000.0 / 48000.0)
#define RENDER_LO   (PERIOD_US * 0.50)
#define RENDER_HI   (PERIOD_US * 0.975)
#define RENDER_PEAK (PERIOD_US * 2.0)

/* The worst case must leave this much in the ring, not just not run dry: ~0.7 ms. */
#define MARGIN_FRAMES 32u

struct result {
	uint32_t under, over, lost, fixes, n47, n49, win_max, fill_min;
	bool heard;
};

static struct result run_ppm(int ppm, bool steps)
{
	static struct uacring r;
	int16_t in[AUDIO_BLK_FRAMES * 2], out[49 * 2];
	uint32_t produced = 0, expected = 0, blocks = 0;
	const double period = PERIOD_US / (1.0 + ppm * 1e-6);
	double next_slot = 0.0, busy_until = 0.0, push_at = 0.0;
	bool rendering = false;
	uint32_t win_fix = 0, last_fix = 0;
	struct result s = { .fill_min = UINT32_MAX };

	uacring_init(&r);
	uacring_stream(&r, true);
	for (uint32_t ms = 0; ms < 600000u; ms++) {
		const double now = ms * 1000.0;
		const bool heavy = steps && (ms / 30000u) % 2u;
		for (;;) {
			if (rendering && push_at <= now) {
				pattern(in, produced, BLK);
				if (!uacring_push(&r, in, BLK)) {
					s.over++;
				}
				produced += BLK;
				rendering = false;
				busy_until = push_at;
				continue;
			}
			const double start = next_slot > busy_until ? next_slot : busy_until;
			if (!rendering && start <= now) {
				double render = heavy ? RENDER_HI : RENDER_LO;
				if (steps && ++blocks % 997u == 0u) {
					render = RENDER_PEAK;
				}
				uacring_claim(&r, BLK);
				rendering = true;
				push_at = start + render;
				next_slot += period;
				continue;
			}
			break;
		}

		uint32_t n = uacring_packet_frames(&r);
		if (uacring_pop(&r, out, n)) {
			if (!s.heard) {
				/* Priming drops its overshoot: at most one block, once. */
				expected = (uint16_t)out[0];
				CHECK(expected < BLK, "priming skips less than a block");
			}
			s.heard = true;
			for (uint32_t i = 0; i < n; i++) {
				if (out[i * 2u] != (int16_t)expected ||
				    out[i * 2u + 1u] != (int16_t)~expected) {
					s.lost++;
					expected = (uint16_t)out[i * 2u];
				}
				expected++;
			}
			/* What is left after this packet, once settled (5 s). */
			const uint32_t left = r.write - r.read;
			if (ms > 5000u && left < s.fill_min) {
				s.fill_min = left;
			}
		}
		CHECK(r.write - r.read <= UACRING_CAPACITY, "fill remains in range");

		/* Corrections per 1000 packets: the host must only ever see drift. */
		const uint32_t fixes = r.n47 + r.n49;
		win_fix += fixes - last_fix;
		last_fix = fixes;
		if (ms % 1000u == 999u) {
			if (win_fix > s.win_max) {
				s.win_max = win_fix;
			}
			win_fix = 0;
		}
	}
	s.under = r.under;
	s.n47 = r.n47;
	s.n49 = r.n49;
	s.fixes = r.n47 + r.n49;
	return s;
}

static const int ppms[] = { -200, -50, 0, 50, 200 };

static void sweep_line(void)
{
	uint32_t under = 0, over = 0, lost = 0, win_max = 0, fix0 = 0, fill_min = UINT32_MAX;

	for (uint32_t i = 0; i < sizeof ppms / sizeof ppms[0]; i++) {
		for (int st = 0; st < 2; st++) {
			struct result s = run_ppm(ppms[i], st);
			under += s.under;
			over += s.over;
			lost += s.lost;
			if (s.win_max > win_max) win_max = s.win_max;
			if (ppms[i] == 0 && st) fix0 = s.fixes;
			if (s.fill_min < fill_min) fill_min = s.fill_min;
		}
	}
	printf("TARGET %4u  HYST %3u  (%.2f ms)  under %u  over %u  lost %u  "
	       "fixes/s max %u  fixes at 0 ppm with steps %u  lowest fill %u\n",
	       UACRING_TARGET, UACRING_HYSTERESIS, UACRING_TARGET / 48.0,
	       under, over, lost, win_max, fix0, fill_min);
}

int main(int argc, char **argv)
{
	struct uacring r;
	int16_t block[AUDIO_BLK_FRAMES * 2], out[49 * 2];
	struct uacring_stats st;

	if (argc > 1 && (!strcmp(argv[1], "-s") || !strcmp(argv[1], "--sweep-line"))) {
		sweep_line();
		return 0;
	}

	printf("== uactest: %u-frame blocks, TARGET %u (%.2f ms), dead band +/-%u\n",
	       (unsigned)BLK, UACRING_TARGET, UACRING_TARGET / 48.0, UACRING_HYSTERESIS);

	for (uint32_t i = 0; i < sizeof ppms / sizeof ppms[0]; i++) {
		for (int steps = 0; steps < 2; steps++) {
			const int ppm = ppms[i];
			struct result s = run_ppm(ppm, steps);

			CHECK(s.heard, "stream left priming");
			CHECK(s.lost == 0, "output stream preserves every frame");
			CHECK(s.under == 0, "no underflow after priming");
			CHECK(s.over == 0, "producer block fits (no overflow while regulated)");
			CHECK(s.fill_min >= MARGIN_FRAMES,
			      "the worst case keeps a margin in the ring, not just above empty");
			CHECK(s.win_max <= 20,
			      "at most 20 corrections a second (the host hears no clock jump)");
			if (ppm < 0) CHECK(s.n47 > s.n49, "slow producer selects more 47-frame packets");
			if (ppm > 0) CHECK(s.n49 > s.n47, "fast producer selects more 49-frame packets");
			if (ppm == 0) {
				CHECK(s.n47 < 1000 && s.n49 < 1000, "zero ppm needs few corrections");
				CHECK(s.n47 > s.n49 ? s.n47 - s.n49 < 100 : s.n49 - s.n47 < 100,
				      "zero ppm corrections balance");
			}
			if (steps && ppm == 0) {
				CHECK(s.fixes <= 60,
				      "load steps and 200 % blocks are not read as drift");
			}
			printf("  %+4d ppm %-6s under %u  47s %u  49s %u  fixes/s max %u  lowest fill %u\n",
			       ppm, steps ? "steps" : "steady", s.under, s.n47, s.n49, s.win_max,
			       s.fill_min);
		}
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
	CHECK(uacring_push(&r, block, BLK), "the block that reaches the prime target fits");
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
	/* 2048 is not a whole number of 96-frame blocks: fill with whole blocks while they fit. */
	const uint32_t whole = UACRING_CAPACITY / BLK;
	for (uint32_t b = 0; b < whole; b++) {
		CHECK(uacring_push(&r, block, BLK), "overflow setup block");
	}
	CHECK(!uacring_push(&r, block, BLK) && r.over == 1,
	      "a block that does not fit is dropped whole and counted");
	CHECK(r.write - r.read == whole * BLK, "overflow does not partly write");

	printf("uactest: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
