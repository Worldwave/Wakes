/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "uacring.h"

#include <string.h>

#define RING_MASK       (UACRING_CAPACITY - 1u)
/* The regulator must follow CLOCK DRIFT only -- a few hundred ppm at most -- and
 * never a render-timing step. Corrected at 32 ms smoothing and +/-24 frames it
 * sent ~150 ms of 47-frame packets whenever a chord started: to the host, our
 * clock ran 2% slow, and macOS's correction of that was a click in the
 * recording (bench, 2026-09-23). Now: ~1 s smoothing (alpha 1/1024 per packet),
 * a dead band (UACRING_HYSTERESIS, uacring.h) and at most one correction per
 * FIX_EVERY packets: 20 frames a second, ~417 ppm, twice the worst crystal.
 * A render step never reaches the regulator: it steers on the CLAIMED fill
 * (below). Steered on pushes, a chord starting shifted the fill by up to a
 * render time, and the band had to be wider than that -- +/-192, where a band
 * of 64 still corrected for seconds after every step (test_uacring, 2026-09-23). */
#define EMA_SHIFT       10u
#define HYSTERESIS      UACRING_HYSTERESIS
#define FIX_EVERY       50u

/* What the regulator steers: the fill counted to the CLAIMED position, which
 * advances when the audio thread takes an I2S slot -- a time the audio clock
 * paces -- instead of when the render finishes. A chord starting makes renders
 * longer and every push later, but moves no claim, so this does not step. */
static uint32_t nominal_fill(const struct uacring *r)
{
	uint32_t claimed = r->claimed;
	uint32_t write = r->write;
	UACRING_BARRIER();
	/* A producer that never claims is steered on its pushes, as before. */
	if ((int32_t)(claimed - write) < 0) claimed = write;
	return claimed - r->read;
}

/* What is really there to send. */
static uint32_t fill(const struct uacring *r)
{
	uint32_t write = r->write;
	UACRING_BARRIER();
	return write - r->read;
}

void uacring_init(struct uacring *r)
{
	memset(r, 0, sizeof *r);
}

void uacring_stream(struct uacring *r, bool enabled)
{
	r->streaming = false;
	UACRING_BARRIER();
	if (!enabled) return;

	/* Drain by moving the CONSUMER's index only. The USB thread that calls this
	 * can preempt a push mid-copy; zeroing `write` here would let that push
	 * finish by storing its stale write + frames, leaving write - read larger
	 * than the ring and the free-space check wrapped. A push in flight now just
	 * adds valid frames after the drain point. */
	r->read = r->write;
	r->ema_q = 0;
	r->since_fix = 0;
	r->packets = 0;
	r->n47 = 0;
	r->n49 = 0;
	r->under = 0;
	r->over = 0;
	r->priming = true;
	r->silence = true;
	UACRING_BARRIER();
	r->streaming = true;
}

void uacring_claim(struct uacring *r, uint32_t frames)
{
	/* From `write`, which only this thread moves: a failed or skipped push
	 * leaves nothing behind for the next claim to inherit. */
	uint32_t claimed = r->write + frames;
	r->blk = frames;
	UACRING_BARRIER();
	r->claimed = claimed;
}

bool uacring_push(struct uacring *r, const int16_t *lr, uint32_t frames)
{
	if (!r->streaming) return false;

	uint32_t write = r->write;
	uint32_t read = r->read;
	UACRING_BARRIER();
	if (frames > UACRING_CAPACITY - (write - read)) {
		r->over++;
		return false;
	}

	uint32_t first = UACRING_CAPACITY - (write & RING_MASK);
	if (first > frames) first = frames;
	memcpy(&r->data[(write & RING_MASK) * 2u], lr,
	       first * 2u * sizeof(int16_t));
	if (first < frames) {
		memcpy(r->data, &lr[first * 2u],
		       (frames - first) * 2u * sizeof(int16_t));
	}
	UACRING_BARRIER();
	r->write = write + frames;
	r->blk = frames;
	return true;
}

uint32_t uacring_packet_frames(struct uacring *r)
{
	uint32_t level = nominal_fill(r);

	if (r->priming) {
		if (level < UACRING_PRIME) {
			r->silence = true;
			return 48;
		}
		r->priming = false;
		/* The fill steps a whole block per claim while nothing drains, so it
		 * crosses PRIME by up to a block. Drop the overshoot -- stream-start
		 * audio, already a discontinuity -- so the fill averages TARGET from
		 * the first packet: the dead band is too narrow to absorb a block
		 * of start-up error without seconds of corrections. */
		uint32_t skip = level - UACRING_PRIME;
		uint32_t avail = fill(r);
		if (skip > avail) skip = avail;
		UACRING_BARRIER();
		r->read += skip;
		level -= skip;
		/* Start the average AT the fill's mean, half a block below this
		 * sample, not at zero: from zero, a slow average reads "empty" for
		 * seconds and sends 47s the whole time. */
		r->ema_q = (level - r->blk / 2u) << EMA_SHIFT;
		r->since_fix = 0;
	}

	int32_t delta = (int32_t)(level << EMA_SHIFT) - (int32_t)r->ema_q;
	r->ema_q = (uint32_t)((int32_t)r->ema_q + delta / (1 << EMA_SHIFT));
	if (r->since_fix < FIX_EVERY) r->since_fix++;

	uint32_t frames = 48;
	uint32_t smooth = r->ema_q >> EMA_SHIFT;
	if (r->since_fix >= FIX_EVERY) {
		if (smooth > UACRING_TARGET + HYSTERESIS) {
			frames = 49;
		} else if (smooth < UACRING_TARGET - HYSTERESIS) {
			frames = 47;
		}
		if (frames != 48) r->since_fix = 0;
	}
	if (fill(r) < frames) {
		r->silence = true;
		return 48;
	}
	if (frames == 47) r->n47++;
	if (frames == 49) r->n49++;
	r->silence = false;
	return frames;
}

bool uacring_pop(struct uacring *r, int16_t *lr, uint32_t frames)
{
	r->packets++;
	uint32_t read = r->read;
	uint32_t write = r->write;
	UACRING_BARRIER();
	if (r->silence || r->priming || write - read < frames) {
		memset(lr, 0, frames * 2u * sizeof(int16_t));
		if (!r->priming) r->under++;
		return false;
	}

	uint32_t first = UACRING_CAPACITY - (read & RING_MASK);
	if (first > frames) first = frames;
	memcpy(lr, &r->data[(read & RING_MASK) * 2u],
	       first * 2u * sizeof(int16_t));
	if (first < frames) {
		memcpy(&lr[first * 2u], r->data,
		       (frames - first) * 2u * sizeof(int16_t));
	}
	UACRING_BARRIER();
	r->read = read + frames;
	return true;
}

void uacring_stats(const struct uacring *r, struct uacring_stats *st)
{
	/* Unlike fill(), this is sampled by neither SPSC owner. Read the consumer
	 * first so a concurrent pop can only make the displayed fill too high. */
	uint32_t read = r->read;
	UACRING_BARRIER();
	uint32_t write = r->write;
	uint32_t level = write - read;
	if (level > UACRING_CAPACITY) level = 0;

	st->on = r->streaming;
	st->packets = r->packets;
	st->n47 = r->n47;
	st->n49 = r->n49;
	st->under = r->under;
	st->over = r->over;
	st->fill = level;
}
