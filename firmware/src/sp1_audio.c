/*
 * wakes-sp1 — audio output. See sp1_audio.h for the topology and the rules.
 *
 * The TAS2505 and CS42L42 register sequences below are ported VERBATIM from
 * chattock/sp1-tape-looper (MIT), which took them from TI's SLAU472C application guide
 * and from Tim Knapen's SP-1-dev wiki respectively, and proved them on this hardware.
 * Attributed in NOTICE. Do not "clean up" a register value without hardware to test
 * it on: a wrong write here does not fail loudly, it produces silence or noise.
 */
#include "sp1_audio.h"
#include "sp1_board.h"
#include "sp1_power.h"          /* sp1_wdt_feed() */
#if defined(CONFIG_SP1_PLAITS)
#include "sp1_synth.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/sys/atomic.h>
#include <string.h>

/* ============================================================================
 *  I2S stream
 * ========================================================================== */
#define SR_HZ        48000u
/* 240 frames = 5 ms = exactly 20 Plaits blocks of 12 (M3). It was 256 in M2, which
 * Plaits' 12-sample block does not divide. Nothing else depends on the size: the gain
 * ramp and the budget are computed from it. */
#define BLK_FRAMES   240u                                   /* 5.00 ms per block */
#define BLK_BYTES    (BLK_FRAMES * 2u * sizeof(int16_t))    /* stereo, 16-bit    */

/* 8 blocks: the nrfx TX queue (4) + two EasyDMA buffers + the one being filled + one
 * spare for the re-prime. tape-looper measured the structural peak at exactly 7. */
K_MEM_SLAB_DEFINE_STATIC(tx_slab, BLK_BYTES, 8, 4);

static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));
static const struct device *const i2c_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));

#define TAS2505_ADDR 0x18u

/* ---- speaker-only level trim (M3) ----
 * Measured 2026-09-21 (logs/sp1-20260921-120917.log): the speaker starts to distort
 * at the -6 dBFS tone step and is clean at -9; the headphones are clean all the way
 * to 0 dBFS. So the speaker path alone is trimmed by 9 dB, in the
 * TAS2505's own DAC volume (page 0, reg 0x41: signed, 0.5 dB steps, 0 = 0 dB), which
 * the CS42L42 headphone path never sees. Digital full scale then lands where the
 * speaker was last heard clean.
 *
 * Most likely the class-D stage runs out of supply voltage at +18 dB gain. If the
 * speaker is fed straight from the battery, its clip point drops as the battery runs
 * down, so re-check near empty before trimming this any closer. Tune by ear in 0.5 dB
 * steps (-14 = -7 dB, -18 = -9 dB).
 *
 * ---- M4a: -9 dB -> -6 dB, i.e. +3 dB louder (Adara: "clipping a bit is acceptable") ----
 * That is exactly the step where M2b first heard distortion, so peaks WILL clip, in the
 * TAS2505's own digital path, before the class-D stage. That is the right place for it:
 * it is repeatable and battery-independent, unlike clipping against the supply rail.
 * The headphone / line path is a different chip and is untouched.
 *
 * ⚠️ A louder class-D stage draws more peak current from the same rail that feeds the
 * ladders and the faders, and rail sag on that rail is this board's known hazard
 * (see the sag notes in sp1_controls.c). Sag from a BUTTON is already rejected or
 * corrected; sag from the amp is a new source and has never been measured. SP1_SAG_AUDIO_K in
 * sp1_controls.c is the knob for it and is 0 until Adara measures it -- see the note
 * there, which says what to look for in the log. */
#define SP1_SPK_ATTEN_HALF_DB  (-12)
#define CS42L42_ADDR 0x48u

/* ---- thread state ----
 * PARKED is ZERO on purpose: sp1_audio_stop() can be called from the power-on gate's
 * power-off path before sp1_audio_init() has ever run, and must see "nothing to stop"
 * from plain static initialisation. */
enum { ST_PARKED = 0, ST_RUNNING = 1 };
static volatile int  state;
static atomic_t      run_req;
static volatile bool started;
static K_SEM_DEFINE(start_sem, 0, 1);

static struct sp1_audio_stats st;
static bool thread_made;

static void jack_reset(void);   /* headphone detect, defined with its poll below */
static bool jack_enabled;       /* ditto; cleared by start() and stop() */

/* Per-report window of block costs (M3): the max since boot is dominated by the one
 * worst block ever -- an ISR landing mid-block, the first render after an engine
 * reset -- and hides the steady cost that decides whether an engine fits. Written by
 * the audio thread, read-and-cleared by main; a block landing mid-read just goes into
 * the next window. Diagnostics, not control. */
static volatile uint32_t win_max, win_sum, win_n;

/* The same window, by section (issue #22; sp1_audio.h). sec_n is its own count so this
 * window and the one above can be read-and-cleared independently. */
static volatile uint32_t sec_sum[SP1_SEC_N], sec_max[SP1_SEC_N], sec_n;
static volatile uint32_t over_n, over_run_max;
static uint32_t over_run;         /* audio thread only: the run in progress */
static uint32_t cyc_budget;       /* copy of st.cyc_budget for the audio thread */

static void account_sections(uint32_t cyc)
{
	uint32_t s[SP1_SEC_N] = { 0u };
#if defined(CONFIG_SP1_PLAITS)
	struct sp1_synth_profile p;
	sp1_synth_last_profile(&p);
	s[SP1_SEC_ENG]  = p.eng;
	s[SP1_SEC_MRB]  = p.mrb;
	s[SP1_SEC_POST] = p.post;
	s[SP1_SEC_RTE]  = p.total - p.mrb - p.eng - p.post;
	s[SP1_SEC_OUT]  = cyc - p.total;
#else
	s[SP1_SEC_OUT]  = cyc;
#endif
	for (int i = 0; i < SP1_SEC_N; i++) {
		sec_sum[i] += s[i];
		if (s[i] > sec_max[i]) { sec_max[i] = s[i]; }
	}
	sec_n++;

	if (cyc > cyc_budget) {
		over_n++;
		if (++over_run > over_run_max) { over_run_max = over_run; }
	} else {
		over_run = 0u;
	}
}

/* ============================================================================
 *  The test tone. Integer only: a phase accumulator into a sine table with linear
 *  interpolation. ~10 cycles per sample, no libm, no floating point.
 * ========================================================================== */

#if !defined(CONFIG_SP1_PLAITS)   /* the test tone exists only in the fallback build */
/* round(32767 * sin(2*pi*i/256)) for i = 0..255, plus a guard entry equal to entry 0
 * so the interpolation never reads past the end. GENERATED, not typed. */
static const int16_t SINE[257] = {
	     0,    804,   1608,   2410,   3212,   4011,   4808,   5602,   6393,   7179,   7962,   8739,
	  9512,  10278,  11039,  11793,  12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
	 18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,  23170,  23731,  24279,  24811,
	 25329,  25832,  26319,  26790,  27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
	 30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,  32137,  32285,  32412,  32521,
	 32609,  32678,  32728,  32757,  32767,  32757,  32728,  32678,  32609,  32521,  32412,  32285,
	 32137,  31971,  31785,  31580,  31356,  31113,  30852,  30571,  30273,  29956,  29621,  29268,
	 28898,  28510,  28105,  27683,  27245,  26790,  26319,  25832,  25329,  24811,  24279,  23731,
	 23170,  22594,  22005,  21403,  20787,  20159,  19519,  18868,  18204,  17530,  16846,  16151,
	 15446,  14732,  14010,  13279,  12539,  11793,  11039,  10278,   9512,   8739,   7962,   7179,
	  6393,   5602,   4808,   4011,   3212,   2410,   1608,    804,      0,   -804,  -1608,  -2410,
	 -3212,  -4011,  -4808,  -5602,  -6393,  -7179,  -7962,  -8739,  -9512, -10278, -11039, -11793,
	-12539, -13279, -14010, -14732, -15446, -16151, -16846, -17530, -18204, -18868, -19519, -20159,
	-20787, -21403, -22005, -22594, -23170, -23731, -24279, -24811, -25329, -25832, -26319, -26790,
	-27245, -27683, -28105, -28510, -28898, -29268, -29621, -29956, -30273, -30571, -30852, -31113,
	-31356, -31580, -31785, -31971, -32137, -32285, -32412, -32521, -32609, -32678, -32728, -32757,
	-32767, -32757, -32728, -32678, -32609, -32521, -32412, -32285, -32137, -31971, -31785, -31580,
	-31356, -31113, -30852, -30571, -30273, -29956, -29621, -29268, -28898, -28510, -28105, -27683,
	-27245, -26790, -26319, -25832, -25329, -24811, -24279, -23731, -23170, -22594, -22005, -21403,
	-20787, -20159, -19519, -18868, -18204, -17530, -16846, -16151, -15446, -14732, -14010, -13279,
	-12539, -11793, -11039, -10278,  -9512,  -8739,  -7962,  -7179,  -6393,  -5602,  -4808,  -4011,
	 -3212,  -2410,  -1608,   -804,      0,
};

/* 440 Hz: round(440 * 2^32 / 48000). */
#define TONE_INC 39370534u
#endif /* !CONFIG_SP1_PLAITS */

/* OUTPUT LEVEL (M3: for the synth too, not only the tone).
 * 0 dBFS then 3 dB steps: round(32768 * 2^(-n/2)), step 0 clamped to 32767, last
 * entry mute. Even steps are exact powers of two -- exactly on a meter band edge --
 * and odd steps sit exactly halfway through a band in dB. GENERATED. */
static const int16_t TONE_AMP[SP1_LEVEL_STEPS] = {
	32767, 23170, 16384, 11585, 8192, 5793, 4096, 2896, 2048, 1448, 1024, 0,
};
static const int16_t TONE_DB_X10[SP1_LEVEL_STEPS] = {
	0, -30, -60, -90, -120, -151, -181, -211, -241, -271, -301, -9990,
};

static volatile bool tone_on;
static volatile int  tone_step = SP1_LEVEL_DEFAULT;   /* the OUTPUT LEVEL step */
#if !defined(CONFIG_SP1_PLAITS)
static uint32_t      phase;
#endif

/* Peak |sample| since the meter last read it. Written by the audio thread, read-and-
 * cleared by the control loop. */
static atomic_t peak;

static void publish_peak(uint32_t p)
{
	/* Atomic max. Bounded: the only other writer is the meter's once-per-tick swap,
	 * so this retries at most a couple of times and cannot spin. */
	atomic_val_t cur = atomic_get(&peak);
	while ((uint32_t)cur < p) {
		if (atomic_cas(&peak, cur, (atomic_val_t)p)) {
			break;
		}
		cur = atomic_get(&peak);
	}
}

uint32_t sp1_audio_take_peak(void)
{
	return (uint32_t)atomic_set(&peak, 0);   /* returns the previous value */
}

/* ---- output gain, slewed so a level change never clicks (M2b) ----
 * M2 switched the gain on a block boundary: a 3 dB step, or the tone starting or
 * stopping, was an instantaneous amplitude jump, which is an audible click. Now the
 * gain moves by at most GAIN_SLEW per block and is interpolated linearly ACROSS the
 * block, per sample, so the waveform envelope is continuous.
 *
 * GAIN_SLEW = 8192 (Q15) per 5 ms block: silence <-> full scale in 4 blocks, 20 ms;
 * one 3 dB step in 1-2 blocks. Fast enough to feel immediate on a button, slow enough
 * that no step is a discontinuity. Costs one multiply-add per sample.
 *
 * This is the pattern for the M3 output volume too: never write a gain the audio
 * path uses directly -- write a TARGET, and let the block loop slew to it. */
#define GAIN_SLEW 8192

static int32_t gain_cur;          /* Q15, audio thread only */

/* The peak check steps by 4 and Plaits renders in 12s. */
BUILD_ASSERT(BLK_FRAMES % 4u == 0u, "fill_block's peak check steps by 4");
#if defined(CONFIG_SP1_PLAITS)
BUILD_ASSERT(BLK_FRAMES % SP1_SYNTH_BLOCK == 0u, "block must be whole Plaits blocks");
#endif

/* Mono source for one block, before gain: the synth's OUT, or the test tone. */
static int16_t src[BLK_FRAMES];

#if !defined(CONFIG_SP1_PLAITS)
static void tone_render(int16_t *out)
{
	uint32_t ph = phase;
	for (uint32_t n = 0; n < BLK_FRAMES; n++) {
		const uint32_t idx  = ph >> 24;                       /* 8-bit index  */
		const int32_t  frac = (int32_t)((ph >> 8) & 0xFFFFu); /* 16-bit frac  */
		const int32_t  a0   = SINE[idx];
		out[n] = (int16_t)(a0 + (((SINE[idx + 1u] - a0) * frac) >> 16));
		ph += TONE_INC;
	}
	phase = ph;
}
#endif

/* Fill one block. The meter's entire audio-path cost is the two lines marked METER. */
static void fill_block(int16_t *b)
{
	/* Read the controls ONCE per block, so a change lands on a block boundary and
	 * a half-updated value is never seen mid-block. */
#if defined(CONFIG_SP1_PLAITS)
	const int32_t target = TONE_AMP[tone_step];

	/* The voice renders EVERY block, even at level 0: its envelopes, LPG and filters
	 * keep running, so turning the level back up never lands mid-glitch, and the
	 * cycle counter always measures the real cost. */
	sp1_synth_render(src, BLK_FRAMES);
#else
	const int32_t target = tone_on ? TONE_AMP[tone_step] : 0;

	if (target == 0 && gain_cur == 0) {
		memset(b, 0, BLK_BYTES);
		return;          /* silence publishes nothing, so the meter falls to blank */
	}
	tone_render(src);
#endif

	/* Gain at the start and end of this block; linear in between, as a Q8
	 * accumulator so the block size is free. |d| <= 8192, so d << 8 fits easily. */
	const int32_t g0 = gain_cur;
	int32_t d = target - g0;
	if (d >  GAIN_SLEW) { d =  GAIN_SLEW; }
	if (d < -GAIN_SLEW) { d = -GAIN_SLEW; }
	gain_cur = g0 + d;

	int32_t       gq   = g0 << 8;
	const int32_t step = (d << 8) / (int32_t)BLK_FRAMES;
	uint32_t      pk   = 0u;

	/* Four frames per outer pass so the peak check runs on every 4th sample with no
	 * per-sample branch (M2b). */
	for (uint32_t i = 0; i < BLK_FRAMES; i += 4u) {
		int16_t o = 0;
		for (uint32_t j = 0; j < 4u; j++) {
			const uint32_t n = i + j;
			o = (int16_t)(((int32_t)src[n] * (gq >> 8)) >> 15);
			gq += step;

			b[2u * n]      = o;                                /* left  */
			b[2u * n + 1u] = o;                                /* right */
		}
		const uint32_t mag = (uint32_t)(o < 0 ? -o : o);        /* METER */
		if (mag > pk) { pk = mag; }                             /* METER */
	}
	publish_peak(pk);
}

static void prime(int n)
{
	for (int i = 0; i < n; i++) {
		void *blk;
		if (k_mem_slab_alloc(&tx_slab, &blk, K_NO_WAIT) != 0) {
			return;
		}
		memset(blk, 0, BLK_BYTES);
		if (i2s_write(i2s_dev, blk, BLK_BYTES) != 0) {
			k_mem_slab_free(&tx_slab, blk);
			return;
		}
	}
}

/* ============================================================================
 *  The audio thread.
 *
 *  ⚠️ EVERY PATH BLOCKS OR SLEEPS. From M3 this thread OUTRANKS the main loop
 *  (audio 0, main 1; see sp1_audio.h), and the main loop feeds the watchdog and runs
 *  power-off. See the note in sp1_audio.h: a spin
 *  here on battery is a boot loop with no working power button.
 *     - parked:          k_sem_take(K_FOREVER)          sleeps
 *     - steady state:    i2s_write blocks until the DMA drains a block (5 ms)
 *     - slab exhausted:  k_mem_slab_alloc(K_MSEC(100))  sleeps, then re-checks stop
 *     - write failure:   k_msleep(2) BEFORE retrying    <-- tape-looper had none
 * ========================================================================== */
/* 4 KB: Plaits' engines keep block-sized scratch on the stack, and FPU_SHARING adds
 * the floating-point context to every switch. 1.5 KB was sized for the M2 sine. */
static K_THREAD_STACK_DEFINE(audio_stack, 4096);
static struct k_thread audio_tcb;

static void audio_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	for (;;) {
		(void)k_sem_take(&start_sem, K_FOREVER);
		state = ST_RUNNING;

		/* Every session fades in from silence. Without this, whatever level the
		 * last session ended at plays out for ~20 ms before slewing to the new
		 * target (review finding). The voice itself carries on where it was;
		 * the fade-in makes that inaudible. */
		gain_cur = 0;

		struct i2s_config cfg = {
			/* 16-bit samples in the codec-mastered 64-SCLK frame: the nRF shifts
			 * the 16 MSBs of each 32-SCLK half-frame, which both codecs decode.
			 * Proven in tape-looper. */
			.word_size      = 16,
			.channels       = 2,
			.format         = I2S_FMT_DATA_FORMAT_I2S,
			.options        = I2S_OPT_FRAME_CLK_SLAVE | I2S_OPT_BIT_CLK_SLAVE,
			.frame_clk_freq = SR_HZ,
			.mem_slab       = &tx_slab,
			.block_size     = BLK_BYTES,
			/* 500 ms, not tape-looper's 2000: short enough that a stop request is
			 * noticed promptly even with the clocks dead, long enough to ride out
			 * the ~80 ms before the CS42L42 starts mastering the frame clock. */
			.timeout        = 500,
		};

		st.cfg_rc = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
		if (st.cfg_rc != 0) {
			goto park;
		}
		prime(4);
		if (i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START) != 0) {
			goto park;
		}
		started = true;

		int wfail = 0;
		while (atomic_get(&run_req)) {
			void *blk;
			if (k_mem_slab_alloc(&tx_slab, &blk, K_MSEC(100)) != 0) {
				continue;                     /* slept 100 ms; re-check stop */
			}

			const uint32_t c0 = DWT->CYCCNT;
			fill_block((int16_t *)blk);
			const uint32_t cyc = DWT->CYCCNT - c0;
			if (cyc > st.cyc_max) { st.cyc_max = cyc; }
			if (cyc > win_max) { win_max = cyc; }
			win_sum += cyc;
			win_n++;
			account_sections(cyc);

			if (i2s_write(i2s_dev, blk, BLK_BYTES) != 0) {
				k_mem_slab_free(&tx_slab, blk);
				st.write_fails++;

				/* ⚠️ THE LINE tape-looper DID NOT HAVE. If writes fail
				 * instantly (stream in an error state), `free; continue`
				 * alone is a tight loop above the main loop's priority. */
				k_msleep(2);

				if (++wfail >= 8) {
					wfail = 0;
					st.restarts++;
					(void)i2s_trigger(i2s_dev, I2S_DIR_TX,
							  I2S_TRIGGER_DROP);
					/* Let the DROP finish before re-priming. START issued
					 * ~30 us after DROP can land while the driver is still
					 * stopping, fail, and push the stream into ERROR --
					 * a repeating fail/restart cycle. Inherited from
					 * tape-looper; cheap to close. */
					k_msleep(5);
					prime(4);
					(void)i2s_trigger(i2s_dev, I2S_DIR_TX,
							  I2S_TRIGGER_START);
				}
				continue;
			}
			wfail = 0;
			st.blocks++;
		}
park:
		(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		started = false;
		state = ST_PARKED;
	}
}

/* ============================================================================
 *  GPIO helpers
 * ========================================================================== */
static void drive_high(NRF_GPIO_Type *port, uint32_t pin)
{
	port->OUTSET = (1u << pin);
	port->PIN_CNF[pin] =
		(GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)   |
		(GPIO_PIN_CNF_DRIVE_S0S1    << GPIO_PIN_CNF_DRIVE_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
	port->OUTSET = (1u << pin);
}

static void drive_low(NRF_GPIO_Type *port, uint32_t pin)
{
	port->OUTCLR = (1u << pin);
}

/* ============================================================================
 *  TAS2505 speaker amp -- VERBATIM from sp1-tape-looper (TI SLAU472C, sec. 5.1).
 *  The amp's DAC runs from a PLL locked to the BIT clock:
 *    f_PLL = 3.072 MHz x 32 = 98.304 MHz; DAC_FS = f_PLL / (2 x 8 x 128) = 48 000 Hz.
 *  The bit clock must already be running, which is why this comes after the stream.
 * ========================================================================== */
/* ⚠️ EVERY I2C HELPER FEEDS THE WATCHDOG, and the configure routines bail on the
 * first failure. Zephyr's nRF TWIM driver times each transfer out after 500 ms
 * (CONFIG_I2C_NRFX_TRANSFER_TIMEOUT, default checked against the pinned v4.3.1). The
 * TAS2505 sequence is 33 writes, so a stuck bus -- SDA shorted, a slave holding SCL --
 * used to mean ~16 s inside sp1_audio_start() with no feed: a watchdog reset, and no
 * power gesture handled for the duration. Feeding per transfer makes rule 3 hold
 * whatever the bus does; bailing makes a dead bus cost ~1 s instead of ~16. */
static int tas_wr(uint8_t reg, uint8_t val)
{
	sp1_wdt_feed();
	return i2c_reg_write_byte(i2c_bus, TAS2505_ADDR, reg, val);
}

/* Bail out of tas2505_configure() on the first failed write: past that point the bus
 * is either dead or the chip is not answering, and 32 more 500 ms timeouts help
 * nobody. tape-looper ORs the results and carries on; this is a deliberate change. */
#define TAS_W(reg, val) do { if (tas_wr((reg), (val)) != 0) { return false; } } while (0)

static bool tas2505_configure(void)
{

	TAS_W(0x00, 0x00);          /* page 0 */
	TAS_W(0x01, 0x01);          /* software reset */
	k_msleep(5);

	TAS_W(0x00, 0x01);          /* page 1 */
	TAS_W(0x02, 0x00);          /* LDO 1.8 V, level shifters up */

	TAS_W(0x00, 0x00);          /* page 0: clocking + interface */
	TAS_W(0x04, 0x07);          /* PLL_CLKIN = BCLK, CODEC_CLKIN = PLL */
	TAS_W(0x05, 0x91);          /* PLL powered, P=1, R=1 */
	TAS_W(0x06, 0x20);          /* PLL J = 32 -> 98.304 MHz */
	TAS_W(0x07, 0x00);          /* PLL D MSB */
	TAS_W(0x08, 0x00);          /* PLL D LSB */
	k_msleep(15);                      /* PLL lock */
	TAS_W(0x0B, 0x82);          /* NDAC = 2, powered */
	TAS_W(0x0C, 0x88);          /* MDAC = 8, powered */
	TAS_W(0x0D, 0x00);          /* DOSR MSB */
	TAS_W(0x0E, 0x80);          /* DOSR = 128 -> 48 000 Hz */
	TAS_W(0x1B, 0x00);          /* I2S, 16-bit, slave */
	TAS_W(0x1C, 0x00);          /* data slot offset 0 */
	TAS_W(0x3C, 0x02);          /* DAC processing block PRB_P2 (mono) */
	TAS_W(0x3F, 0x90);          /* DAC powered, left -> left, soft-step */
	TAS_W(0x41, (uint8_t)(int8_t)SP1_SPK_ATTEN_HALF_DB);  /* speaker-only trim */
	TAS_W(0x40, 0x04);          /* DAC unmuted */

	TAS_W(0x00, 0x01);          /* page 1: analog, routing, driver */
	TAS_W(0x01, 0x10);          /* master analog reference on */
	TAS_W(0x0A, 0x00);          /* output common mode 0.9 V */
	TAS_W(0x0C, 0x04);          /* DAC -> output mixer */
	TAS_W(0x16, 0x00);          /* HP volume 0 dB */
	TAS_W(0x18, 0x00);          /* AINL volume / mixer */
	TAS_W(0x09, 0x20);          /* HP driver up */
	TAS_W(0x10, 0x00);          /* HP unmuted */
	TAS_W(0x2E, 0x00);          /* speaker attenuation 0 dB */
	TAS_W(0x30, 0x30);          /* class-D gain +18 dB (tape-looper's value) */
	TAS_W(0x2D, 0x02);          /* speaker driver up */

	TAS_W(0x00, 0x00);          /* back to page 0 */
	k_msleep(10);

	return true;
}

/* ============================================================================
 *  CS42L42 headphone codec, and FRAME-CLOCK MASTER -- VERBATIM from sp1-tape-looper's
 *  live path (HP_TIM_TEST 1), which is Tim Knapen's SP-1-dev wiki sequence.
 *
 *  ⚠️ tape-looper's own comments above that code describe an OLDER design with the
 *  oscillator OFF and the nRF as master. That path is dead code there. This is the
 *  live one: oscillator ON, CS42L42 mastering the frame. Do not port the stale comment.
 * ========================================================================== */
static bool cs_pw(uint16_t reg, uint8_t val)   /* paged write */
{
	sp1_wdt_feed();                                /* see the note above tas_wr */
	const uint8_t p[2] = { 0x00, (uint8_t)(reg >> 8) };
	const uint8_t b[2] = { (uint8_t)reg, val };
	if (i2c_write(i2c_bus, p, 2, CS42L42_ADDR) != 0) {
		return false;
	}
	return i2c_write(i2c_bus, b, 2, CS42L42_ADDR) == 0;
}

static bool cs_pr(uint16_t reg, uint8_t *val)  /* paged read */
{
	sp1_wdt_feed();
	const uint8_t p[2] = { 0x00, (uint8_t)(reg >> 8) };
	const uint8_t o = (uint8_t)reg;
	if (i2c_write(i2c_bus, p, 2, CS42L42_ADDR) != 0) {
		return false;
	}
	return i2c_write_read(i2c_bus, CS42L42_ADDR, &o, 1, val, 1) == 0;
}

/* Same rule as TAS_W: once the chip has identified itself, the first failed write
 * ends the attempt. tape-looper discards every result ((void)); this is deliberate. */
#define CS_W(reg, val) do { if (!cs_pw((reg), (val))) { return false; } } while (0)

static bool cs42l42_configure(void)
{
	/* Hard reset pulse -- without it the codec can be wedged and NAK everything. */
	drive_low(SP1_CS42_RST_PORT, SP1_CS42_RST_PIN);
	k_msleep(5);
	drive_high(SP1_CS42_RST_PORT, SP1_CS42_RST_PIN);
	k_msleep(10);

	uint8_t id = 0;
	if (!cs_pr(0x1001, &id) || id != 0x42) {
		return false;                      /* not a CS42L42: leave it alone */
	}

	CS_W(0x1508, 0x10);   /* PLL Control 3 */
	CS_W(0x1504, 0x80);   /* PLL Division Fractional Byte 2 */
	CS_W(0x1505, 0x3E);   /* PLL Division Integer */
	CS_W(0x150A, 0x7D);   /* PLL Calibration Ratio */
	CS_W(0x1009, 0x00);   /* MCLK Control */
	CS_W(0x1201, 0x01);   /* MCLK Source Select */
	CS_W(0x120A, 0x01);   /* Input ASRC Clock Select */
	CS_W(0x120B, 0x01);   /* Output ASRC Clock Select */
	CS_W(0x1501, 0x01);   /* PLL Control 1: start */
	CS_W(0x1107, 0x01);   /* Oscillator Switch (the 3.072 MHz osc drives SCLK) */
	for (int t = 0; t < 10; t++) {         /* wait for "SCLK selected" (0x02) */
		uint8_t s = 0;
		k_msleep(1);
		if (cs_pr(0x1109, &s) && s == 0x02) {
			break;
		}
	}
	CS_W(0x1007, 0x13);   /* Serial Port SRC Control */
	CS_W(0x1203, 0x1F);   /* FSYNC Pulse Width Lower (64-SCLK frame) */
	CS_W(0x1205, 0x3F);   /* FSYNC Period Lower */
	CS_W(0x1207, 0x34);   /* ASP Clock Config: MASTER */
	CS_W(0x1208, 0x1A);   /* ASP Frame Configuration */
	CS_W(0x2A02, 0x02);   /* Channel 1: 24-bit */
	CS_W(0x2A05, 0x42);   /* Channel 2: phase + 24-bit */
	CS_W(0x2601, 0x4C);   /* SRC Input Sample Rate */
	CS_W(0x2609, 0x4C);   /* SRC Output Sample Rate */
	CS_W(0x2A01, 0x0C);   /* ASP Receive Enable */
	CS_W(0x240E, 0x01);   /* Equalizer Input Mute Control */
	CS_W(0x2301, 0x00);   /* Mixer A vol 0 dB */
	CS_W(0x2303, 0x00);   /* Mixer B vol */
	CS_W(0x1101, 0x96);   /* power up the codec */
	k_msleep(10);                /* HP amp operational after 10 ms */
	CS_W(0x1121, 0x41);   /* Headset switch control */
	CS_W(0x1B74, 0x03);   /* Miscellaneous detect control */
	CS_W(0x1129, 0x01);   /* Headset clamp disable */
	CS_W(0x2001, 0x0D);   /* HP Control: mute all */
	CS_W(0x1F06, 0x84);   /* DAC Control 2 */
	CS_W(0x2301, 0x00);   /* Mixer A vol again */
	CS_W(0x2303, 0x00);   /* Mixer B vol again */
	CS_W(0x1B73, 0xC2);   /* Tip Sense Control */
	CS_W(0x1B75, 0x9F);   /* Mic detect control 1 */
	CS_W(0x2001, 0x01);   /* UNMUTE headphones */
	return true;
}

/* ============================================================================
 *  Public API
 * ========================================================================== */
void sp1_audio_init(void)
{
	/* DWT cycle counter: the instrument that says how much of each block the audio
	 * work actually costs. M2 measures the baseline before Plaits arrives. */
	/* CMSIS 6 names (this build uses modules/hal/cmsis_6, checked against the
	 * pinned tree). `CoreDebug` still exists there, but only as a compatibility
	 * alias for DCB; use the native name rather than lean on the alias. */
	DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0u;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

	/* The flash cache's hit/miss counters (issue #22): diagnostics only, read and
	 * cleared with the CPU line. Profiling does not change what the cache does. */
	NRF_NVMC->ICACHECNF |= NVMC_ICACHECNF_CACHEPROFEN_Msk;
	NRF_NVMC->IHIT = 0u;
	NRF_NVMC->IMISS = 0u;

	/* 64 MHz, a constant rather than SystemCoreClock: one fewer dependency on a
	 * symbol this build only reaches through a local Zephyr patch. */
	st.cyc_budget = (uint32_t)((64000000ull * BLK_FRAMES) / SR_HZ);   /* 320 000 */
	cyc_budget = st.cyc_budget;
#if defined(CONFIG_SP1_PLAITS)
	sp1_synth_set_cycle_counter(&DWT->CYCCNT);
#endif

#if defined(CONFIG_SP1_PLAITS)
	/* Construct the voice and set up all its engines, here in the main thread at
	 * boot, so the audio thread never pays for it inside a block. */
	sp1_synth_init();
#endif

	if (!thread_made) {
		thread_made = true;
		(void)k_thread_create(&audio_tcb, audio_stack,
				      K_THREAD_STACK_SIZEOF(audio_stack),
				      audio_thread, NULL, NULL, NULL,
				      K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	}
}

int sp1_audio_start(void)
{
	/* No jack polling until this bring-up has succeeded (jack_reset() re-enables
	 * it). An early return below must not leave the previous session's detection
	 * running against codecs that are held in reset. */
	jack_enabled = false;

	/* The thread may still be finishing a previous stop (up to one 500 ms write
	 * timeout if the clocks died under it). Wait for it to park -- bounded, and
	 * feeding the watchdog -- rather than racing it. */
	for (int i = 0; i < 120 && state != ST_PARKED; i++) {
		sp1_wdt_feed();
		k_msleep(5);
	}
	if (state != ST_PARKED || !thread_made) {
		return -EBUSY;
	}
	if (!device_is_ready(i2s_dev) || !device_is_ready(i2c_bus)) {
		return -ENODEV;
	}

	/* 1. release both codec resets */
	drive_high(SP1_CS42_RST_PORT, SP1_CS42_RST_PIN);
	drive_high(SP1_TAS_RST_PORT,  SP1_TAS_RST_PIN);
	k_msleep(20);

	/* 2. oscillator on: the bit clock now exists */
	drive_high(SP1_OSC_EN_PORT, SP1_OSC_EN_PIN);
	k_msleep(5);

	/* 3. start the stream (the nRF waits as a slave for a frame clock) */
	started = false;
	atomic_set(&run_req, 1);
	k_sem_give(&start_sem);
	for (int i = 0; i < 100 && !started; i++) {
		k_msleep(2);
	}
	sp1_wdt_feed();

	/* 4. speaker amp: PLL locks to the bit clock, already running */
	st.tas_ok = tas2505_configure();
	sp1_wdt_feed();

	/* 5. headphone codec: starts mastering the frame clock -> audio flows */
	st.hp_ok = cs42l42_configure();
	sp1_wdt_feed();

	/* Speaker is ON after bring-up; the jack poll decides from here. */
	jack_reset();

	return started ? 0 : -EIO;
}

void sp1_audio_stop(void)
{
	/* ⚠️ ON THE POWER-OFF PATH. Ask, wait at most 50 ms, and return regardless.
	 * The caller cuts the codec resets and the oscillator straight after; if the
	 * thread is still inside a write, that write simply times out and the thread
	 * parks on its own. Power-off never waits on the audio thread. */
	atomic_set(&run_req, 0);
	jack_enabled = false;          /* the codecs are about to be powered down */
	for (int i = 0; i < 10 && state != ST_PARKED; i++) {
		k_msleep(5);
	}
}

void sp1_audio_halt_for_system_off(void)
{
	/* ⚠️ ONLY EVER CALLED ON THE WAY TO SYSTEM_OFF (or the reset that replaces it
	 * when plugged). Never on the way to STANDBY -- see below.
	 *
	 * 1. Halt the I2S peripheral at REGISTER level. sp1_audio_stop() waits at most
	 *    50 ms and then lets power-off proceed; if the stream was stalled (e.g. the
	 *    CS42L42 never mastered the frame clock) the thread can still be inside a
	 *    500 ms i2s_write when quiesce cuts the clocks. A slave-mode STOP then waits
	 *    for a clock edge that never comes, and the chip would enter SYSTEM_OFF with
	 *    I2S DMA still active. Doing it here removes any dependence on the thread.
	 *    The driver's view of the peripheral is left inconsistent, which is fine:
	 *    the next thing to run is a fresh boot.
	 *
	 * 2. Disconnect the I2S pins' input buffers. With CONFIG_I2S the driver's
	 *    pinctrl makes the clock pins connected, un-pulled INPUTS. Power-off turns the
	 *    oscillator off and holds the CS42L42 in reset, so both clock lines are left
	 *    undriven, and SYSTEM_OFF keeps the pin configuration -- a floating CMOS input
	 *    can sit mid-rail and draw current all night. This is new in M2 (before it,
	 *    nothing configured these pins). Rule 7 is about exactly this kind of drain.
	 *
	 * Why NOT on the STANDBY path: the Zephyr nRF I2S driver applies its pinctrl
	 * state ONCE, at boot init (checked in the pinned v4.3.1 source). Disconnecting
	 * the pins going into STANDBY would leave them disconnected on the next ON, and
	 * audio would silently stop working after the first STANDBY. */
	NRF_I2S->TASKS_STOP = 1u;
	NRF_I2S->ENABLE = 0u;

	/* Power-on reset value: input, buffer disconnected, no pull. */
	const uint32_t off = (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos);
	NRF_P0->PIN_CNF[12] = off;   /* SCK  (bit clock)   */
	NRF_P0->PIN_CNF[11] = off;   /* LRCK (frame clock) */
	NRF_P1->PIN_CNF[9]  = off;   /* SDOUT              */
}

void sp1_audio_tone_set(bool on) { tone_on = on; }
bool sp1_audio_tone_on(void)     { return tone_on; }
int  sp1_audio_level_get(void)   { return tone_step; }

void sp1_audio_level_step(int delta)
{
	int s = tone_step + delta;
	if (s < 0) { s = 0; }
	if (s > SP1_LEVEL_STEPS - 1) { s = SP1_LEVEL_STEPS - 1; }
	tone_step = s;
}

int sp1_audio_level_db_x10(void)
{
	return TONE_DB_X10[tone_step];
}

void sp1_audio_take_cycles(uint32_t *max, uint32_t *avg)
{
	/* The audio thread outranks this one, so without the lock it could run between
	 * the reads and the clears, and the reported sum and count would not match. A
	 * few instructions under the lock; an ISR can still run, but never touches these. */
	k_sched_lock();
	const uint32_t n = win_n, sum = win_sum, mx = win_max;
	win_n = 0u;
	win_sum = 0u;
	win_max = 0u;
	k_sched_unlock();
	*max = mx;
	*avg = n ? (sum / n) : 0u;
}

void sp1_audio_take_sections(struct sp1_audio_sections *out)
{
	uint32_t sum[SP1_SEC_N];

	/* Same lock, same reason, as sp1_audio_take_cycles(). */
	k_sched_lock();
	const uint32_t n = sec_n;
	for (int i = 0; i < SP1_SEC_N; i++) {
		sum[i] = sec_sum[i];
		out->max[i] = sec_max[i];
		sec_sum[i] = 0u;
		sec_max[i] = 0u;
	}
	sec_n = 0u;
	out->over = over_n;
	out->over_run = over_run_max;
	out->icache_hit = NRF_NVMC->IHIT;
	out->icache_miss = NRF_NVMC->IMISS;
	NRF_NVMC->IHIT = 0u;
	NRF_NVMC->IMISS = 0u;
	over_n = 0u;
	over_run_max = 0u;
	k_sched_unlock();

	for (int i = 0; i < SP1_SEC_N; i++) {
		out->avg[i] = n ? (sum[i] / n) : 0u;
	}
}

/* ============================================================================
 *  Headphone detect -> speaker mute (M3). Ported from sp1-tape-looper's auto-mute.
 *
 *  The CS42L42's tip sense is already configured by the bring-up above (0x1B73/74/75,
 *  Tim Knapen's sequence). DET_STATUS1 (0x1B77) bit 7 reads 1 while a plug is in.
 *  While it is, the TAS2505's class-D driver is powered down (page 1, reg 0x2D).
 *
 *  ⚠️ FAIL-SAFE IS "SPEAKER ON". The one outcome this must never produce is a device
 *  that is silent with nothing plugged in. So:
 *    - the speaker is on after every bring-up, before the first read;
 *    - a change needs JACK_DEBOUNCE identical reads in a row (~120 ms);
 *    - `spk_on` records what the AMP is actually doing, separately from what the jack
 *      says. A state change is committed only once the driver write SUCCEEDED; a
 *      failed write is retried on the next poll instead of being believed. (Review
 *      finding: committing regardless could leave the speaker off after an unplug,
 *      and nothing would ever retry.)
 *    - JACK_MAX_FAILS I2C failures in an ON session -- in total, not in a row --
 *      disable detection for the rest of the session, with one more attempt to put
 *      the speaker on if it is off. "In total" because a bus that alternates
 *      fail/success would never trip "in a row", and each failure can stall the main
 *      loop for up to ~1 s of I2C timeouts, which would slow the 20 s power-off
 *      backstop (rule 5a) roughly 30-fold. So the worst case this can cost the main
 *      loop is bounded: ~JACK_MAX_FAILS seconds per session, once.
 *  Main thread only -- the audio thread never touches I2C after bring-up.
 * ========================================================================== */
#define JACK_POLL_MS    40u
#define JACK_DEBOUNCE   3
#define JACK_MAX_FAILS  3

static int      jack_state;      /* 1 plugged, 0 unplugged, -1 unknown */
static int      jack_cand, jack_cnt, jack_fails;
static uint32_t jack_ms;
static bool     spk_on;          /* what the TAS2505 driver is actually set to */

static bool speaker_driver(bool on)
{
	/* Every access selects its page first; always try to leave page 0 selected,
	 * even if the driver write itself failed. */
	const bool ok = (tas_wr(0x00, 0x01) == 0) &&          /* page 1 */
			(tas_wr(0x2D, on ? 0x02 : 0x00) == 0); /* driver up/down */
	(void)tas_wr(0x00, 0x00);                              /* page 0 */
	if (ok) {
		spk_on = on;
	}
	return ok;
}

static void jack_reset(void)
{
	jack_state = -1;
	jack_cand = -1;
	jack_cnt = 0;
	jack_fails = 0;
	jack_ms = 0u;
	spk_on = true;                            /* tas2505_configure() left it up */
	jack_enabled = st.tas_ok && st.hp_ok;     /* no codec, nothing to ask */
}

/* Count one I2C failure; true if detection has just been given up on. */
static bool jack_fail(void)
{
	if (++jack_fails < JACK_MAX_FAILS) {
		return false;
	}
	jack_enabled = false;
	jack_state = -1;
	if (!spk_on) {
		(void)speaker_driver(true);           /* last attempt: speaker ON */
	}
	return true;
}

int sp1_audio_jack_poll(uint32_t elapsed_ms)
{
	if (!jack_enabled) {
		return -1;
	}
	if ((jack_ms += elapsed_ms) < JACK_POLL_MS) {
		return -1;
	}
	jack_ms = 0u;

	uint8_t v = 0;
	if (!cs_pr(0x1B77, &v)) {
		return jack_fail() ? 2 : -1;
	}

	const int c = (v >> 7) & 1;
	if (c != jack_cand) {
		jack_cand = c;
		jack_cnt = 1;
		return -1;
	}
	if (jack_cnt < JACK_DEBOUNCE) {
		jack_cnt++;
	}
	if (jack_cnt < JACK_DEBOUNCE || c == jack_state) {
		return -1;
	}
	/* Debounced change. Commit it only if the amp actually did what we asked. */
	if (!speaker_driver(c == 0)) {
		return jack_fail() ? 2 : -1;          /* retried on the next poll */
	}
	jack_state = c;
	return c;
}

int sp1_audio_jack_state(void) { return jack_state; }

void sp1_audio_stats(struct sp1_audio_stats *out)
{
	*out = st;
	out->running = (state == ST_RUNNING) && started;
}
