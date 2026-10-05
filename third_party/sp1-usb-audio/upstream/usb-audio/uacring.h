/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* SPSC stereo-frame ring and USB packet regulator. Pure C99. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The producer's block (audio.h; prep.sh defines it for the whole app). The
 * fill saw-tooths by one block, so the constants follow it. */
#ifndef AUDIO_BLK_FRAMES
#define AUDIO_BLK_FRAMES  128
#endif

/* 2048: room above the dead band for a render-step burst (see uacring.c). */
#define UACRING_CAPACITY  2048u
/* TARGET (average fill) and the dead band around it, per block size. The fill
 * must stay above empty with everything at once: the regulator parked at the
 * bottom of its dead band (-200 ppm does that), half a block of saw-tooth, and
 * one block still rendering when its push is due -- a render time, up to the
 * worst seen (RENDER_PEAK in test_uacring: 3417 us at 128, a whole block at
 * 256), plus a packet.
 *   256: 640 / +/-192, ~13 ms -- as first proven on hardware (2026-09-23). The
 *        band once had to cover a render step as well; claims took that out
 *        of the measurement, but these values are left as proven.
 *   128: 416 / +/-64, ~8.7 ms. The claimed fill does not step with render time,
 *        so the band covers drift only. Underflow starts between 320 and 352
 *        (test_uacring at 128); 416 is one 64-frame step clear of the last
 *        pass, the margin 640 has at 256. */
#if AUDIO_BLK_FRAMES == 128
#define UACRING_TARGET_DEF      416u
#define UACRING_HYSTERESIS_DEF   64u
#else
#define UACRING_TARGET_DEF      640u
#define UACRING_HYSTERESIS_DEF  192u
#endif
/* Overridable for test_uacring sweeps only. */
#ifndef UACRING_TARGET
#define UACRING_TARGET      UACRING_TARGET_DEF
#endif
#ifndef UACRING_HYSTERESIS
#define UACRING_HYSTERESIS  UACRING_HYSTERESIS_DEF
#endif
/* Priming ends at TARGET + half a producer block, and drops any overshoot. The
 * fill saw-tooths down a whole block between pushes, so priming at TARGET left
 * its AVERAGE half a block low -- and the regulator, which sees the average,
 * then corrected for minutes (test_uacring, 92 one-sided corrections at 0 ppm). */
#define UACRING_PRIME     (UACRING_TARGET + AUDIO_BLK_FRAMES / 2u)

#ifndef UACRING_BARRIER
#define UACRING_BARRIER() do { } while (0)
#endif

struct uacring_stats {
	bool on;
	uint32_t packets;
	uint32_t n47;
	uint32_t n49;
	uint32_t under;
	uint32_t over;
	uint32_t fill;
};

struct uacring {
	int16_t data[UACRING_CAPACITY * 2u];
	volatile uint32_t write;
	volatile uint32_t read;
	volatile uint32_t claimed;  /* write + the block being rendered (uacring_claim) */
	uint32_t blk;           /* frames per claim: the fill's saw-tooth */
	uint32_t ema_q;         /* smoothed fill, Q(EMA_SHIFT) */
	uint32_t since_fix;     /* packets since the last 47/49 correction */
	volatile bool streaming;
	bool priming;
	bool silence;
	volatile uint32_t packets;
	volatile uint32_t n47;
	volatile uint32_t n49;
	volatile uint32_t under;
	volatile uint32_t over;
};

void uacring_init(struct uacring *r);
void uacring_stream(struct uacring *r, bool enabled);
/* Producer: at the START of each render, when the I2S slot is taken, the frames
 * that render will push. Optional -- without it the regulator steers on pushes. */
void uacring_claim(struct uacring *r, uint32_t frames);
bool uacring_push(struct uacring *r, const int16_t *lr, uint32_t frames);
uint32_t uacring_packet_frames(struct uacring *r);
bool uacring_pop(struct uacring *r, int16_t *lr, uint32_t frames);
void uacring_stats(const struct uacring *r, struct uacring_stats *st);
