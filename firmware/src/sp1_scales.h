/*
 * wakes-sp1 — the scale bank (#44): the seven scale slots Marbles' X quantizer and Plaits'
 * FREQUENCY quantizer both choose from.
 *
 * Pure C, no Zephyr: the UI files, sp1_marbles.cc, the storage thread and the host suites
 * all read it.
 *
 * ---- where the scales come from ----
 * config/scales.cnfg (Scala .scl blocks, one per slot) is the BUILT-IN bank: tools/gen_scales.py
 * turns it into sp1_scales_gen.h and fails the build on a bad default. The ON job reads
 * WAKES/CNFG/scales.cnfg from the drive at every ON (sp1_cnfg.h, sp1_scales_reader) and
 * installs the result here BEFORE PRST reads its slots, so a preset's scale is checked
 * against the scales in use. A slot the file leaves out is EMPTY: the scale steppers skip it.
 *
 * ---- threads ----
 * TWO copies, like the engine stack (sp1_plaits_ui.c): the storage thread writes the one not
 * in use and flips `live`, once per ON job, so no reader ever sees a scale half-written.
 * Every install bumps sp1_scales_gen(): the audio thread (Marbles' four quantizers) and main
 * (Plaits' quantizer) compare it and reload, since slot N may now hold another scale.
 */
#ifndef SP1_SCALES_H
#define SP1_SCALES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SP1_SCALE_SLOTS    7     /* Adara (#44): more than 7 in the file are dropped     */
#define SP1_SCALE_DEGREES  64    /* marbles::kMaxDegrees, raised from 16 (plaits_ovr)    */
#define SP1_SCALE_NAME_MAX 24    /* the Scala description, cut to fit, for the log       */

/* One scale, in Marbles' terms (marbles/random/quantizer.h, Scale). */
struct sp1_scale {
	float   period;                      /* volts at 1 V/oct: 1.0 = the octave (2/1)      */
	uint8_t n;                           /* degrees, the root included; 0 = EMPTY slot    */
	uint8_t plaits;                      /* Plaits plays the degrees of weight >= this    */
	uint8_t by_ear;                      /* 1: Plaits' root is Tune By Ear's (#30), 0: C  */
	uint8_t weight[SP1_SCALE_DEGREES];   /* 0-255: how selective STEPS must be to drop it */
	float   volts[SP1_SCALE_DEGREES];    /* rising, volts[0] = 0 (the root, weight 255)   */
	char    name[SP1_SCALE_NAME_MAX];
};

struct sp1_scale_bank {
	struct sp1_scale s[SP1_SCALE_SLOTS];
};

/* The built-in bank, config/scales.cnfg at build time. */
void sp1_scales_defaults(struct sp1_scale_bank *b);

/* Storage thread, once per ON job: the bank in use from now on. A bank with no scale at all is
 * replaced by the built-in one (Marbles' X always needs a scale to quantize to). */
void sp1_scales_install(const struct sp1_scale_bank *b);

/* Bumped by every install. */
uint32_t sp1_scales_gen(void);

/* The scale in a slot, or NULL if the slot is empty or out of range. The pointer is good
 * until the next install (the next ON). */
const struct sp1_scale *sp1_scale_at(int slot);
bool        sp1_scale_filled(int slot);
int         sp1_scale_first(void);            /* the first filled slot */
const char *sp1_scale_name(int slot);         /* "" for an empty slot  */

#ifdef __cplusplus
}
#endif

#endif /* SP1_SCALES_H */
