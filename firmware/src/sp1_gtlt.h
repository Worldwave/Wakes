/*
 * wakes-sp1 — GTLT, the t gate tilt (issue #20). MARBLES t SHIFT F1.
 *
 * Wakes' own parameter, not Marbles': it scales the HEIGHT of t1's and t3's gates around
 * t2, which is never touched. Pure C, no Zephyr; sp1_synth.cc applies it and the host
 * suites check it.
 *
 *   tilt 0 (the 10 % centre detent)   t1 100 %   t3 100 %   GTLT disengaged
 *   just past the detent              t1  50 %   t3  50 %   a steep ramp, SP1_GTLT_RAMP
 *   towards +1                        t1 -> 0 %  t3 -> 100 %
 *   towards -1                        t1 -> 100 %  t3 -> 0 %
 *
 * Decided with Adara on the issue (2026-10-06):
 * - A gate stays unipolar: 0 V low, +5 V high scaled by this gain. +5 V is already the
 *   positive maximum of every destination a t output can reach, so no INTELLIGENT range
 *   is involved; Plaits' attenuverters and the fader / CC positions do the rest.
 * - ⚠️ TRIG IGNORES GTLT. A t output routed to TRIG fires on every gate whatever GTLT is
 *   set to, so rhythm stays predictable. Only the CV destinations (LEVEL, FM, TIMBRE,
 *   MORPH, HARMONICS) see the gain -- sp1_synth.cc reads TRIG from the raw gate bits.
 * - Unsmoothed: it changes once per DMA block, like every other routed parameter.
 *
 * Cost: none per sample. The gain folds into each route's scale, which sp1_synth.cc
 * already resolves once per DMA block; at the detent the gain is exactly 1.0f, so the
 * output is bit-identical to a firmware without GTLT.
 */
#ifndef SP1_GTLT_H
#define SP1_GTLT_H

#include <stdbool.h>

/* How much of each half of the (post-detent) travel the drop to 50 % takes. 0.1 of a
 * half is ~4.5 % of the whole fader: steep, but a ramp rather than a step, so leaving
 * the detent never jumps a routed CV by half its height (Adara: "steep ramp"). */
#define SP1_GTLT_RAMP 0.1f

/* tilt: -1..+1, 0 = centre (the UI applies the detent). t: 0..2 = t1..t3. */
static inline float sp1_gtlt_gain(float tilt, int t)
{
	if (t == 1 || tilt == 0.0f) {
		return 1.0f;                       /* t2, or GTLT disengaged: exact */
	}
	const float a = tilt < 0.0f ? -tilt : tilt;
	if (a < SP1_GTLT_RAMP) {
		return 1.0f - 0.5f * (a / SP1_GTLT_RAMP);   /* both: 100 % -> 50 % */
	}
	float s = (a - SP1_GTLT_RAMP) / (1.0f - SP1_GTLT_RAMP);   /* 0..1 past the ramp */
	s = s > 1.0f ? 1.0f : s;
	/* + attenuates t1 and pushes t3; - the reverse. */
	const bool falls = (tilt > 0.0f) == (t == 0);
	return falls ? 0.5f * (1.0f - s) : 0.5f * (1.0f + s);
}

#endif /* SP1_GTLT_H */
