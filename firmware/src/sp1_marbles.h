/*
 * wakes-sp1 — the Marbles generators (M4): a C interface around Marbles' t and X/Y
 * sections, driving Plaits.
 *
 * The DSP is Mutable Instruments Marbles by Emilie Gillet (MIT), vendored unmodified in
 * third_party/eurorack/marbles (random/, ramp/, resources). Only its generators are
 * used: no inputs, no external clock, no UI, no settings storage.
 *
 * ---- the 4 kHz rule, now 2 kHz (#32) ----
 * Marbles runs natively at 32 kHz. Here it runs ONCE PER PLAITS BLOCK, because Plaits reads
 * its modulation and TRIG inputs only once per block -- anything faster is thrown away.
 * That was 4 kHz with 12-sample blocks (host-verified: the same random sequence as at
 * 32 kHz; gate edges on a 0.25 ms grid) and is 2 kHz with 24 (CONFIG_SP1_PLAITS_BLOCK;
 * edges on a 0.5 ms grid). The sample rate it is given follows the block size.
 *
 * ---- X and Y at 1 kHz (issue #22) ----
 * t and the master ramp run at the Plaits block rate; X1-X3 and Y are generated at 1 kHz,
 * from every SP1_SYNTH_BLOCKS_PER_MS-th ramp sample, and brought back to the block rate. Marbles renders the whole block before
 * Plaits sees any of it, so nothing is late: a STEPPED output changes on the exact 4 kHz
 * sample its clock wrapped (host: bit-identical to 4 kHz), and a SMOOTH one is
 * interpolated between its 1 kHz values. Glides therefore differ from 4 kHz in their
 * first millisecond or two (host, 120 BPM, STEPS 0.45: 0.19 V worst of a 10 V span,
 * 0.014 V rms), and more so at very fast clocks. Adara accepted that concession.
 *
 * ---- threads ----
 *   sp1_marbles_init()                  main, once, before audio starts
 *   sp1_marbles_render(), *_frame()     AUDIO THREAD ONLY (called from sp1_synth)
 *   everything else                     main thread; lock-free hand-off, read once
 *                                       per audio block
 */
#ifndef SP1_MARBLES_H
#define SP1_MARBLES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Marbles samples rendered per audio block: one per Plaits block. */
#define SP1_MARBLES_MAX_FRAMES 20u

/* Selectable scales: Marbles' six presets plus one free slot for a user scale
 * (docs/MARBLES-SETTINGS.md). Not Marbles' own limit of six -- only the SELECTED scale
 * is loaded into a quantizer, so this number is ours to choose (sp1_marbles.cc). */
#define SP1_MARBLES_SCALES 7

/* X/Y voltage ranges ([J]). The first three are Marbles': 0..2 V, 0..5 V, -5..+5 V.
 *
 * ---- INTELLIGENT (M4c, Adara) ----
 * Not a range but a RULE: each output gets the range that suits what it is routed to AND
 * the parameter of the engine currently selected --
 *
 *   destination                     range        why
 *   FM                              -5..+5 V     FM is a signed offset by definition
 *   TIMBRE / MORPH / HARMONICS      -5..+5 V     if that parameter is BIPOLAR on this
 *                                                engine: its centre is the neutral point
 *                                   0..5 V       if it is unipolar: there is no centre to
 *                                                modulate around, so only push it up
 *   V/Oct                           0..2 V       two octaves. 0..5 V is a five-octave
 *                                                leap and +-5 V is ten
 *   LEVEL, TRIG                     0..5 V       one-sided inputs
 *   none                            -5..+5 V     nothing reads it; keep the widest
 *
 * Re-evaluated every block, so it follows an engine change while the sequence plays --
 * deliberately (Adara). ⚠️ Which parameters are bipolar is NOT a new table: it is the
 * detent table in tools/gen_engines.py, compiled into SP1_ENGINE_TABLE[].centre and
 * handed to us as sp1_marbles_params::engine_centre. */
enum sp1_mrb_range {
	SP1_MRB_RANGE_NARROW = 0,
	SP1_MRB_RANGE_POSITIVE,
	SP1_MRB_RANGE_FULL,
	SP1_MRB_RANGE_INTELLIGENT,
	SP1_MRB_RANGE_COUNT
};

/* Which t output clocks the X section (X SHIFT F2). EACH = X1 on t1, X2 on t2, X3 on
 * t3 (Marbles' own default with nothing patched). */
enum sp1_mrb_xclock { SP1_MRB_XCLK_EACH = 0, SP1_MRB_XCLK_T1, SP1_MRB_XCLK_T2, SP1_MRB_XCLK_T3 };

/* Everything the UI decides, already in Marbles' units. */
struct sp1_marbles_params {
	/* t section */
	float rate;          /* semitones around 2 Hz (120 BPM at 1x): -60..+60     */
	/* ⚠️ `ratchet` is GONE (M4e). It added semitones here while FFWD was held and the
	 * clock ran, so the master clock really sped up -- which moved the beat on press and
	 * left the phase displaced on release. FFWD is a phase-locked burst now and never
	 * touches the clock; see sp1_synth.h. Making the SEQUENCE advance at a subdivision
	 * is a separate idea, parked for a community vote, and deliberately not built. */
	int   t_range;       /* [B] 0 = x0.25, 1 = x1, 2 = x4                       */
	int   t_model;       /* [E] 0..5: coin toss, clusters, drums | independent,
	                      *      divider, three states                          */
	float t_bias, t_jitter, t_deja_vu;          /* 0..1                           */
	float gate_length, gate_length_rand;        /* 0..1 (Marbles' [E]-hold pair)  */
	int   length;        /* DEJA VU loop length [I], 1..16 steps                */
	/* X section */
	float x_spread, x_bias, x_steps, x_deja_vu; /* 0..1                           */
	int   x_diversity;   /* [N] 0 identical, 1 bump, 2 tilt                     */
	int   x_range;       /* [J] enum sp1_mrb_range                              */
	int   x_scale;       /* 0..SP1_MARBLES_SCALES-1                             */
	int   x_clock;       /* enum sp1_mrb_xclock -- pinned to EACH since M4a      */
	/* ---- what the outputs are routed to, for INTELLIGENT (M4c) ----
	 * X1, X2, X3, Y as enum sp1_mui_dest, and the current engine's bipolar-parameter
	 * bits (SP1_ENGINE_TABLE[].centre: 0x1 HARMONICS, 0x2 TIMBRE, 0x4 MORPH). Ignored
	 * unless the matching range is SP1_MRB_RANGE_INTELLIGENT. */
	uint8_t dest[4];
	uint8_t engine_centre;
	/* Y */
	float y_spread, y_bias, y_steps;            /* 0..1                           */
	int   y_divider;     /* 0..11 -> 1/64 ... 1/1 of t2                        */
	int   y_range;       /* [J] enum sp1_mrb_range                              */
};

/* Construct the generators and load the scales. Idempotent. */
void sp1_marbles_init(void);

/* Seed the random stream (main thread, before the first run). Different every power-on
 * if the caller passes something that varies, e.g. the cycle counter at the first PLAY.
 * Re-inits the generators too, so the DEJA VU loop is re-drawn rather than kept. */
void sp1_marbles_seed(uint32_t seed);

/* ---- re-seed while the clock is RUNNING ("rip out the cables", M4d) ----
 * Same effect as sp1_marbles_seed, but SAFE to call mid-flight: it only records the
 * request, and the audio thread performs it at the top of its next block, before it
 * touches the generators.
 *
 * ⚠️ sp1_marbles_seed() itself must not be called while audio is running. It
 * re-initialises TGenerator and XYGenerator, and the audio thread outranks main, so it
 * could be preempted half way through rebuilding a structure the audio thread then reads.
 *
 * ⚠️ Re-seeding a running clock JUMPS ITS PHASE once -- the beat after a rip lands early
 * or late by up to one tick. Accepted deliberately (Adara, M4d) in preference to delaying
 * the rip's effect until the next t2 tick.
 *
 * ⚠️ And it must re-INIT, not just re-seed: the DEJA VU loop buffer is filled from the
 * stream when the generators are initialised, and at the default LOCKED deja vu no new
 * values are ever drawn -- so a fresh seed on its own would never be heard. */
void sp1_marbles_reseed(uint32_t seed);

/* Publish a complete parameter set (double-buffered, like sp1_synth_set_params). */
void sp1_marbles_set_params(const struct sp1_marbles_params *p);

/* Run / stop the clock (PLAY). Starting puts the master clock one sample before a
 * beat, so the first t2 tick -- and whatever t1/t3 decide on it -- comes at once. */
void sp1_marbles_run(bool on);

/* ---- an external clock: MIDI's (M5b; sp1_midi.h) ----
 * AUDIO THREAD, before each sp1_marbles_render. `beats` = this block's position in beats,
 * one per Marbles sample (= Plaits block), or NULL for Marbles' own clock. While external,
 * RATE picks a RATIO of the beat from Marbles' own table (TGenerator::input_divider_ratios:
 * 1/4 ... 4, x4 or x1/4 with the t range) through Marbles' own hysteresis -- exactly the
 * module's external-clock path, with the ramp made from `beats` instead of by its 32 kHz
 * ramp extractor. `transport`:
 *   START     reset and run; the ramp holds still until beat 1, so the first tick lands on
 *             beat 1's own sample
 *   CONTINUE  run, no reset, the clock aligned to the beat position
 *   STOP      stop
 * The run state is the one PLAY sets, so either can change it (C6). */
#define SP1_MRB_TP_NONE     0
#define SP1_MRB_TP_START    1
#define SP1_MRB_TP_CONTINUE 2
#define SP1_MRB_TP_STOP     3
void sp1_marbles_clock(const float *beats, int transport);
bool sp1_marbles_running(void);

/* ---- audio thread ---- */
/* Render `n` (<= SP1_MARBLES_MAX_FRAMES) Marbles samples, one per Plaits block of the
 * current audio block. Does nothing (and the frames read as all-low / 0 V) when
 * stopped. */
void sp1_marbles_render(uint32_t n);
/* Frame j of the last render: gates bit0 = t1, bit1 = t2, bit2 = t3; volts[0..2] =
 * X1..X3, volts[3] = Y, in volts. */
uint8_t sp1_marbles_gates(uint32_t j);
float   sp1_marbles_volts(uint32_t j, int k);
/* The MASTER RAMP of frame j: a 0..1 phase that wraps once per clock tick, i.e. once per
 * beat (t2 is simply `ramp < 0.5`). 0 while stopped.
 *
 * Exposed for the FFWD burst, which reads its 1/div grid off this instead of running its
 * own accumulator (M4e) -- that is what makes a burst an exact subdivision of the clock
 * you can hear rather than a second oscillator beating against it. */
float   sp1_marbles_ramp(uint32_t j);

/* The same three, as the arrays themselves (issue #22), for a caller that reads every
 * frame of a block: gates[j], volts[4 * j + k], ramp[j]. One call per block instead of
 * five per frame. Valid until the next sp1_marbles_render(). */
const uint8_t *sp1_marbles_gate_frames(void);
const float   *sp1_marbles_volt_frames(void);
const float   *sp1_marbles_ramp_frames(void);

/* ---- main thread: for the LEDs and the play-row clock ---- */
/* The latest gates / voltages (updated once per audio block). */
uint8_t sp1_marbles_last_gates(void);
float   sp1_marbles_last_volts(int k);
/* t2 rising edges since boot (wraps). The play-row clock steps on these. */
uint32_t sp1_marbles_beats(void);
/* The master clock in beats per minute for the given rate / range, as the FFWD burst
 * uses it: 120 * 2^(rate/12), x0.25 / x1 / x4. */
float sp1_marbles_bpm(float rate, int t_range);

/* Names for the log. */
const char *sp1_marbles_model_name(int model);
const char *sp1_marbles_scale_name(int scale);

/* ---- the scales, lent to Plaits (M4b) ----
 * Plaits' FREQUENCY fader can be quantized to one of the same scales (docs/UI-PAGES.md,
 * PLAITS SETTINGS T2/T3). These run on their OWN quantizer instance, in the MAIN thread,
 * so nothing here touches the X/Y quantizers the audio thread is using.
 *
 * ⚠️ There is no "amount" control (Adara): each scale is quantized to the note set its
 * NAME means. That is not the same as every degree in the table -- Marbles stores major
 * as twelve weighted degrees, so admitting all of them would quantize to a chromatic
 * scale. The per-scale weight threshold that yields the named set is in sp1_marbles.cc
 * (`scale_level`), with the resulting note sets written out in
 * docs/MARBLES-SETTINGS.md. */
void  sp1_marbles_plaits_scale(int scale);      /* load; -1 or out of range = none  */
float sp1_marbles_plaits_quantize(float semitones);   /* identity if none is loaded */
/* The loaded scale's degrees as semitones above the root, ascending. Returns how many
 * were written, or 0 if no scale is loaded. For the octave-mode-9 degree sweep. */
int   sp1_marbles_plaits_degrees(float *semitones, int max);

#ifdef __cplusplus
}
#endif

#endif /* SP1_MARBLES_H */
