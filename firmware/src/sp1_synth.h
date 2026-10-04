/*
 * wakes-sp1 — the synth voice (M3): a C interface around the Plaits DSP.
 *
 * The DSP is Mutable Instruments Plaits by Emilie Gillet (MIT), vendored unmodified in
 * third_party/eurorack/. This file and sp1_synth.cc are the only code that touches it.
 *
 * ---- threads ----
 *   sp1_synth_init()          main thread, once, before audio starts
 *   sp1_synth_render()        AUDIO THREAD ONLY
 *   everything else           main thread; lock-free hand-off, read once per block
 *
 * Parameters are double-buffered (see sp1_synth_set_params). A trigger is a counter
 * the main thread increments and the audio thread compares, so a press is never lost
 * and never doubled, whatever the timing.
 *
 * ---- M3 scope ----
 * ONE engine: 2-op FM (Plaits engine index 10, "FM" in Plaits 1.0's bank). All 24 are
 * compiled in -- Voice::Init sets them all up -- but only FM can be selected until M4.
 * An engine that overruns the budget makes the audio stutter (see sp1_audio.h); it
 * does not reset the device.
 *
 * Controls: see sp1_plaits_ui.h (pages, pickup, centre detents). This file only
 * turns a finished sp1_synth_params into Plaits' Patch and Modulations.
 */
#ifndef SP1_SYNTH_H
#define SP1_SYNTH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Plaits renders in blocks of this many samples; sp1_synth_render() needs a multiple. */
#if defined(CONFIG_SP1_PLAITS_BLOCK)
#define SP1_SYNTH_BLOCK ((uint32_t)CONFIG_SP1_PLAITS_BLOCK)
#else
#define SP1_SYNTH_BLOCK 12u
#endif
/* Plaits blocks per millisecond (4 at 12 samples, 2 at 24): the unit every block-counted
 * time constant of ours is scaled by (#32). */
#define SP1_SYNTH_BLOCKS_PER_MS (48u / SP1_SYNTH_BLOCK)

/* Engine the voice is constructed with, before the UI publishes its first parameters
 * (plaits/dsp/voice.cc order): virtual analog. Only a placeholder: the UI starts on the
 * first filled slot of config/engines.csv, whatever that is, and publishes it before the
 * first block. */
#define SP1_SYNTH_ENGINE_INITIAL 8

/* Construct and initialise the voice. Idempotent. */
void sp1_synth_init(void);

/* AUDIO THREAD. Render `frames` samples of the main output (Plaits OUT) into `out`,
 * one int16 per frame. `frames` must be a multiple of SP1_SYNTH_BLOCK. */
void sp1_synth_render(int16_t *out, uint32_t frames);

/* Everything the UI decides, already mapped to Plaits' own units (M3b). The UI layer
 * (sp1_plaits_ui) owns pages, pickup, centre detents and the octave-range logic; the
 * synth just renders what it is given. */
struct sp1_synth_params {
	float note;                    /* MIDI semitones, final (range mode applied)  */
	float harmonics, timbre, morph;          /* 0..1                            */
	/* Attenuverters, -1..+1. HARMONICS' is ours (M4a): Plaits has no attenuverter on
	 * that input, so voice.{h,cc} are overridden to give it one, with the same
	 * internal-envelope behaviour as FM / TIMBRE / MORPH. */
	float timbre_mod, fm_mod, morph_mod, harm_mod;
	float decay, lpg_colour;                 /* 0..1 (settings page)            */
	float level;                   /* 0..1, only meaningful if level_patched      */
	int   level_patched;           /* 0: LPG triggered by PLAY; 1: VCA held open  */
	/* ---- what MIDI needs to apply its offsets in the audio thread (M5a) ----
	 * A MIDI CC moves a parameter like a second hand on its fader (sp1_midi.h), smoothed
	 * per Plaits block. LEVEL's CC acts on the fader POSITION -- below 5 % is
	 * "disconnected" -- so the raw position is passed alongside; level_patched above has
	 * already counted the CC (the control loop decides the connect threshold). And
	 * FREQUENCY's CC moves F1, whose semitones per unit of travel depend on the octave
	 * range: 96 in the full range, 14 in modes 1-8, 120 in LFO mode, and 0 in mode 9,
	 * where F1 is a switch and the control loop applies the CC before quantizing. */
	float level_pos;               /* SETTINGS F4, 0..1, as stored               */
	/* The playing engine's bipolar parameters, SP1_ENGINE_TABLE[].centre (0x1 HARMONICS,
	 * 0x2 TIMBRE, 0x4 MORPH): decides how a MIDI CC on those three reads (sp1_midi.h). */
	uint8_t engine_centre;
	float freq_per_travel;         /* semitones per whole fader travel           */
	int   engine;                  /* Plaits engine index                         */
	/* Marbles -> Plaits (M4). Applied ONLY while Marbles' clock runs
	 * (sp1_marbles_running); stopped, every one of these inputs is unpatched. */
	uint8_t mrb_t_dest[3];         /* t1, t2, t3: enum sp1_mui_dest values         */
	uint8_t mrb_dest[4];           /* X1, X2, X3, Y: enum sp1_mui_dest values      */
	/* ---- hold the note between TRIGs (M4e, Adara) ----
	 * Set while a FREQUENCY scale is selected. `note` above is already quantized by
	 * sp1_pui_params in the main thread; this asks the AUDIO thread to LATCH it at each
	 * TRIG edge and use the latched value until the next one.
	 *
	 * ⚠️ It has to happen here, not in the UI, because TRIG is only known in the audio
	 * thread. Without it, moving F1 while a note is still decaying slides that note's
	 * pitch from degree to degree -- correctly quantized and, as Adara put it, not as
	 * pleasurable as hoped. With it, F1 chooses the NEXT note.
	 *
	 * ⚠️ 0 while no scale is selected: unquantized FREQUENCY stays continuous, which is
	 * what a fader with no grid should do. */
	int     note_hold;
};

/* Publish a complete parameter set. Main thread. The audio thread picks it up at its
 * next block, whole: two buffers and an index flip, so it can never see half an
 * update (it outranks main, so it is never interrupted by a writer mid-copy). */
void sp1_synth_set_params(const struct sp1_synth_params *p);

/* Fire the TRIG input once (RWD pressed). Works whether or not Marbles runs; while it
 * runs, this and the routed t gates both reach TRIG, and a new edge that lands while
 * TRIG is already high is re-struck (one low Plaits block, 0.5 ms) so it is never lost. */
void sp1_synth_trigger(void);

/* ---- the TRIG burst (M3b; tempo from Marbles since M4; PHASE-LOCKED since M4e) ----
 * A BURST fires TRIGs at 1/div notes of the tempo for as long as it is held on. Timing is
 * quantised to Plaits' block: 24 samples, 0.5 ms (#32; 12 samples, 0.25 ms, before).
 *
 * ⚠️ Where the grid comes from depends on whether Marbles' clock is RUNNING (M4e, Adara):
 *
 *   stopped -> a free-running accumulator at 1/div of `clock_bpm`, first TRIG on the
 *              press. There is nothing to lock to, so this is unchanged from M3b.
 *   running -> the 1/div grid is read off MARBLES' OWN MASTER RAMP
 *              (sp1_marbles_ramp), so a burst is exact subdivisions of the clock you can
 *              hear. It cannot drift, however long FFWD is held, and releasing leaves the
 *              clock exactly where it would have been.
 *
 * ⚠️ THE RATE-MULTIPLYING RATCHET IS GONE (M4e). Through M4d, FFWD while running added
 * semitones to Marbles' RATE, so the master clock really sped up. That is why the rhythm
 * shifted: multiplying the rate mid-cycle moves the next tick, and releasing leaves the
 * phase wherever it happened to land -- "desynchronize it, depending on timing of FFWD
 * press" (Adara). A phase-locked burst cannot do that, because it never touches the
 * clock.
 *
 * ⚠️ So FFWD while running now RE-TRIGGERS the note Marbles is holding rather than making
 * the SEQUENCE advance faster. That is a real change in what FFWD is: a roll, not an
 * arpeggio. Driving Marbles' clock at a subdivision instead is parked for a community vote
 * and is deliberately NOT built (Adara).
 *
 * ⚠️ The first TRIG of a running burst is quantised to the NEAREST grid point, not fired
 * on the press: less than half a grid period past a boundary fires now, more than half
 * waits for the next one. Worst case is half a period -- 31 ms at 1/32 and 120 BPM -- and
 * it means a burst never lands off the grid it is supposed to define. */
void sp1_synth_set_tempo(float bpm);
void sp1_synth_set_burst_div(uint32_t div);
void sp1_synth_burst(int on);
uint32_t sp1_synth_burst_count(void);     /* TRIGs fired by bursts, for the log */
/* Rising edges actually delivered to Plaits' TRIG input since boot, from every
 * source (RWD, the burst, Marbles' t gates). For the log (M4). */
uint32_t sp1_synth_trig_edges(void);

/* ---- output select (M3f, UI-SPEC "Output select") ----
 * Plaits renders two signals per engine, OUT and AUX; this picks what reaches the
 * codec. Cycled by T4 on the PLAITS SETTINGS panel. Applied from the next Plaits block.
 *   OUT      Plaits' OUT                         (the default)
 *   AUX      Plaits' AUX
 *   SUM      (OUT + AUX) x 0.71 (-3 dB) through a peak limiter with Plaits' own
 *            constants and ceiling (0.8 of full scale; M3g). Below the ceiling it
 *            is untouched, so a quiet sum keeps its level
 *   RING     OUT x AUX x 2, saturated -- the product of two signals near full scale
 *            is quieter than either; x2 is the UI-SPEC's starting gain.
 *
 * Since issue #22 the mode is a set of mixing weights applied INSIDE the voice, BEFORE
 * its one low-pass gate (see the vendored plaits/dsp/voice.h): the chain is
 * OUT/AUX -> sum or product -> LPG -> drive -> OUT+AUX limiter, then the output level
 * (sp1_audio.c). Every mode costs the same. OUT, AUX and OUT+AUX sound as before;
 * OUTxAUX is gated once AFTER the multiply (Adara), where it used to multiply two
 * gated channels. */
enum sp1_synth_output {
	SP1_OUT_MAIN = 0,
	SP1_OUT_AUX,
	SP1_OUT_SUM,
	SP1_OUT_RING,
	SP1_OUT_COUNT
};
void sp1_synth_set_output(enum sp1_synth_output o);

/* ---- the soft-clip drive (M4b, Adara) ----
 * Extra gain PAST the output stage's maximum, into a soft clipper. "••" + VOL+ steps it
 * up, "••" + VOL- switches the whole stage off (and back on at the last setting), on
 * either module. Unshifted VOL+/- is still the ordinary output level.
 *
 * ONE drive channel since issue #22 (Adara): the output select comes first, so OUT, AUX,
 * OUT+AUX or OUTxAUX is driven as one signal -- the sum and the ring product distort
 * together, and the intermodulation that produces is intended. The OUT+AUX limiter
 * stays AFTER the drive: it bounds what leaves the device. (Through M4e the drive sat on
 * OUT and AUX separately, before the mix; that cost two clippers and sounded different
 * in OUT+AUX and OUTxAUX.) The curve is a table (sp1_synth.cc), so other shapes would
 * cost the same.
 *
 * ⚠️ This is a saturator, not a volume control. stmlib::SoftClip is already curving at
 * x = 1, so full-scale peaks come DOWN (+3 dB of drive maps 1.0 -> 0.91) while quiet
 * material comes up: it compresses and adds harmonics. Step 0 bypasses the stage
 * entirely and is bit-identical to having no drive code at all -- not "SoftClip at unity
 * gain", which would attenuate by ~2.2 dB and colour the sound.
 *
 * ---- the steps are UNEVEN (M4e, Adara) ----
 * +3 / +8 / +15 / +24 dB, not 3 dB apart. Adara wanted "more extreme compression at the
 * top end" without losing a gentle first setting, so the ramp opens at +3 dB -- a
 * fattener -- and ends at +24 dB, where most of the waveform is flat and it is a fuzz.
 * Even 6 dB steps would have reached +24 too, but the bottom of the range would have been
 * +6 dB and there would be no subtle setting left.
 *
 * Changes are slewed across one DMA block. sp1_synth_drive_db() is the table, for the log
 * and for the UI -- do not recompute step x 3 anywhere. */
#define SP1_DRIVE_STEPS 5              /* 0 = off, then +3 / +8 / +15 / +24 dB */
void sp1_synth_set_drive(int step);
int  sp1_synth_drive(void);
int  sp1_synth_drive_db(int step);     /* the dB at `step`, 0 at step 0 */

/* ---- where a block's cycles went (issue #22) ----
 * One number for the whole block cannot say whether an engine, Marbles or our own output
 * stage is the expensive part, and every optimisation has to be judged against that. So
 * sp1_synth_render() timestamps its sections from a free-running cycle counter it is
 * handed -- a pointer, because this layer includes no Zephyr or CMSIS header. NULL (the
 * default, and the host) turns the profile off and every figure reads 0.
 *
 *   mrb    Marbles' generators (sp1_marbles_render)
 *   eng    plaits::Voice::Render -- the engine, its LPG and Plaits' own limiter
 *   post   the drive, the output select and its limiter, the conversion to int16
 *   total  the whole call; total - mrb - eng - post is the routing between them
 *
 * An interrupt landing inside a section is counted in it: these are wall-clock spans.
 * About 45 counter reads per DMA block, under 0.1 % of the budget. */
struct sp1_synth_profile {
	uint32_t total, mrb, eng, post;
	/* #32: the part of `total - mrb - eng - post` spent BEFORE the Plaits loop -- once per
	 * audio block (params, MIDI begin, Marbles' clock, routing) -- so the per-block cost
	 * can be told from the per-Plaits-block glue. */
	uint32_t pre;
	/* ...and two parts of it (#32, B1): MIDI's per-block work (begin + the clock handed to
	 * Marbles) and routing's (ResolveRouting). The rest is params, mixes and drive setup. */
	uint32_t pre_midi, pre_route;
	/* #32, CONFIG_SP1_PROFILE_ICACHE: flash-cache misses over the same spans (0 if off). */
	uint32_t miss_total, miss_mrb, miss_eng, miss_post, miss_pre;
	/* #32: the slowest single voice->Render() in this audio block, its engine (Plaits
	 * number) and whether it was the first call after an engine change. */
	uint32_t eng_worst;
	uint8_t eng_worst_engine;
	bool eng_worst_first;
};
void sp1_synth_set_cycle_counter(const volatile uint32_t *counter);

/* The flash cache's miss counter (NVMC IMISS), or NULL (the default: no miss profile). */
void sp1_synth_set_miss_counter(const volatile uint32_t *counter);
/* MIDI's clock (sp1_midi.h, "timing"): the same clock the USB side stamps messages with --
 * NOT the cycle counter, which stops while the CPU sleeps. NULL (the default) = untimed. */
void sp1_synth_set_midi_clock(uint32_t (*now)(void));
/* AUDIO THREAD: the spans of the last sp1_synth_render() call. */
void sp1_synth_last_profile(struct sp1_synth_profile *out);


#ifdef __cplusplus
}
#endif

#endif /* SP1_SYNTH_H */
