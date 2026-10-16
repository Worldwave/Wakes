/* wakes-sp1 — faders and button ladders. See sp1_controls.h. */
#include "sp1_controls.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <stdio.h>

#include "sp1_board.h"

/* io-channels indices on zephyr_user. Battery is 0 and STAYS 0 -- everything else
 * appends after it, so sp1_batt.c never has to be renumbered. */
#define IO_LADDER_TRACKS 1
#define IO_LADDER_VOL    2
#define IO_FADER_1       3
#define IO_FADER_2       4
#define IO_FADER_3       5
#define IO_FADER_4       6

#define SPEC(idx) ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), idx)

static const struct adc_dt_spec lad_spec[2] = { SPEC(IO_LADDER_TRACKS),
						SPEC(IO_LADDER_VOL) };
static const struct adc_dt_spec fad_spec[SP1_NUM_FADERS] = {
	SPEC(IO_FADER_1), SPEC(IO_FADER_2), SPEC(IO_FADER_3), SPEC(IO_FADER_4)
};

static bool ok;

static uint16_t lad_raw[2];
static uint16_t fad_raw[SP1_NUM_FADERS];      /* last VALID fader sample */
static void track_touch(void);                 /* shutdown suppression: below the scan */
static uint16_t fad_scratch[SP1_NUM_FADERS];  /* this scan, maybe sagged */
static bool     fad_valid;
static enum sp1_rail rail;
static uint32_t scan_errors;   /* dropped scans; see sp1_controls_scan() */

/* ============================================================================
 *  THRESHOLD TABLES — measured on the one SP-1, two sessions a day apart
 *    2026-09-19  logs/sp1-20260919-182353.log  (6-sample means, 20 states)
 *    2026-09-20  logs/sp1-20260920-193610.log  (38/100-sample means, 22 states) <- CURRENT
 *
 *  ---- the model has been tested OUT OF SAMPLE, and held ----
 *  Session 1's fit (A = 3605) was used to MODEL three combinations it had never
 *  seen. Session 2 then measured them for the first time:
 *
 *      T1+T2+T3   predicted 1099   measured 1104   error -5
 *      T1+T2+T4   predicted 1476   measured 1480   error -4
 *      T1+T3+T4   predicted 1626   measured 1628   error -2
 *
 *  Refit on all 13 measured combinations: A = 3604, rms 2.24, worst 4.3 -- the
 *  ceiling moved by one count. The 13 entries still marked "modelled" all contain
 *  PLAY plus two or more buttons; they exist only so those chords are REJECTED.
 *
 *  ---- repeatability ----
 *  19 states measured on both days drift -1 to +3 counts (mean +1.1). Against a
 *  40-count match window that is negligible, but the drift is systematic rather
 *  than random, so re-run calibration if a future session shows it growing.
 *
 *  ⚠️ VALID ONLY UNDER THIS ADC CONFIGURATION: gain 1/6 + ADC_REF_INTERNAL
 *  (0.6 V => 3.6 V full scale), 20 us acquisition, 2x oversampling. sp1-midi's
 *  published tables were taken at gain 1/4 + VDD/4 and are NOT interchangeable.
 *
 *  ---- how these numbers were obtained ----
 *  Five single presses were measured directly. Each button puts its own resistor
 *  in parallel to the divider node, so CONDUCTANCES add and readings do not. With
 *  x = r/(A-r) for a ceiling A, x is additive across pressed buttons and
 *
 *      reading = A * X / (1 + X),    X = sum of the pressed buttons' x
 *
 *  Session 1 fitted A = 3605 on ten combinations; session 2 refitted A = 3604 on
 *  thirteen, after the model had correctly predicted three it had never seen (see
 *  the block at the top). The model is as accurate as the measurement, so the
 *  "modelled" entries are interpolations of a validated curve, not guesses.
 *
 *  ---- what is safe to decode, and what is not ----
 *  Every SINGLE button is at least 53 counts from the nearest other state
 *  (PLAY is the tightest: 1813, between 1755 and 1866; T1 has 189). Its usable
 *  window is -28/+26 against a measured excursion of -5/+8 over 38 samples, so
 *  single-press decoding has a 3x margin at its tightest point.
 *
 *  Combinations are a different story, and this corrects a claim inherited from
 *  the knowledgebase. In the T-only region they separate cleanly (>= 61 counts).
 *  Once PLAY joins, the ladder is already near its ceiling and the states crowd
 *  to 26-55 counts apart. "Eight thresholds each, combinations decodable" is
 *  therefore only half true, and was never measured before now.
 *
 *  So the decoder ACTS on single presses only. Chords are recognised in order to
 *  be IGNORED rather than mistaken for a single -- which is the whole reason the
 *  31-state table exists. The UI does not need chords: "••" is the shift, and it
 *  is a direct GPIO off this rail entirely.
 * ============================================================================ */
#define THRESHOLDS_MEASURED 1

/* Bit per button within a ladder-0 reading. */
#define M_T1   0x01u
#define M_T2   0x02u
#define M_T3   0x04u
#define M_T4   0x08u
#define M_PLAY 0x10u

struct sp1_ladder_entry {
	uint16_t centre;
	uint8_t  mask;
};

static const struct sp1_ladder_entry ladder0[] = {
	{  210, M_T1 },                              /* measured */
	{  399, M_T2 },                              /* measured */
	{  570, M_T1|M_T2 },                         /* measured */
	{  728, M_T3 },                              /* measured */
	{  865, M_T1|M_T3 },                         /* measured */
	{  990, M_T2|M_T3 },                         /* measured */
	{ 1104, M_T1|M_T2|M_T3 },                    /* measured */
	{ 1215, M_T4 },                              /* measured */
	{ 1311, M_T1|M_T4 },                         /* measured */
	{ 1398, M_T2|M_T4 },                         /* measured */
	{ 1480, M_T1|M_T2|M_T4 },                    /* measured */
	{ 1558, M_T3|M_T4 },                         /* measured */
	{ 1628, M_T1|M_T3|M_T4 },                    /* measured */
	{ 1693, M_T2|M_T3|M_T4 },                    /* measured */
	{ 1755, M_T1|M_T2|M_T3|M_T4 },               /* measured */
	{ 1813, M_PLAY },                            /* measured */
	{ 1866, M_T1|M_PLAY },                       /* measured */
	{ 1917, M_T2|M_PLAY },                       /* modelled */
	{ 1965, M_T1|M_T2|M_PLAY },                  /* modelled */
	{ 2013, M_T3|M_PLAY },                       /* modelled */
	{ 2055, M_T1|M_T3|M_PLAY },                  /* modelled */
	{ 2096, M_T2|M_T3|M_PLAY },                  /* modelled */
	{ 2134, M_T1|M_T2|M_T3|M_PLAY },             /* modelled */
	{ 2170, M_T4|M_PLAY },                       /* measured */
	{ 2209, M_T1|M_T4|M_PLAY },                  /* modelled */
	{ 2242, M_T2|M_T4|M_PLAY },                  /* modelled */
	{ 2273, M_T1|M_T2|M_T4|M_PLAY },             /* modelled */
	{ 2305, M_T3|M_T4|M_PLAY },                  /* modelled */
	{ 2333, M_T1|M_T3|M_T4|M_PLAY },             /* modelled */
	{ 2361, M_T2|M_T3|M_T4|M_PLAY },             /* modelled */
	{ 2387, M_T1|M_T2|M_T3|M_T4|M_PLAY },        /* modelled */
};

/* Ladder 1 is single-press only -- there is no combination to decode, which the
 * measured values bear out: four states 329 to 602 counts apart. The mask here is
 * just an index tag, not a bitfield to be combined. */
#define M_VOL_UP   0x01u
#define M_VOL_DOWN 0x02u
#define M_FFWD     0x04u
#define M_RWD      0x08u

static const struct sp1_ladder_entry ladder1[] = {
	{  403, M_RWD },        /* measured */
	{  729, M_VOL_DOWN },   /* measured */
	{ 1216, M_FFWD },       /* measured */
	{ 1817, M_VOL_UP },     /* measured */
};

/* How far a reading may sit from a centre and still match it. Well above the
 * 11-count worst observed spread, well below half the 26-count tightest gap in
 * the crowded PLAY region -- so a reading in a genuine dead zone matches nothing
 * and is reported as "no press" rather than as its nearest neighbour. */
#define LADDER_MAX_DIST 40u

/* A press must decode identically on two consecutive scans (~18 ms) before it is
 * reported. Cheap insurance against the one transitional sample you get while a
 * contact is closing, and short enough to stay imperceptible on a musical control.
 * sp1-tape-looper's author reached the same conclusion by a different route. */
#define DEBOUNCE_SCANS 2u

static bool held[SP1_BTN_COUNT];
static bool went_down[SP1_BTN_COUNT];
static bool went_up[SP1_BTN_COUNT];

/* Last decoded ladder-0 state, chords included. Exposed for diagnostics and for a
 * later UI that might want chords; the button API deliberately never sees them. */
static uint8_t chord_mask;

static void decode_buttons(void);

/* Nearest-centre match. Returns the mask, or 0 for "matches nothing". */
static uint8_t ladder_decode(const struct sp1_ladder_entry *tab, size_t n,
			     uint16_t v)
{
	uint32_t best_d = 0xFFFFFFFFu;
	uint8_t  best_m = 0u;

	for (size_t i = 0; i < n; i++) {
		const uint32_t d = (v > tab[i].centre)
				 ? (uint32_t)(v - tab[i].centre)
				 : (uint32_t)(tab[i].centre - v);
		if (d < best_d) {
			best_d = d;
			best_m = tab[i].mask;
		}
	}
	return (best_d <= LADDER_MAX_DIST) ? best_m : 0u;
}

static bool exactly_one_bit(uint8_t m)
{
	return m != 0u && (m & (uint8_t)(m - 1u)) == 0u;
}

/* A single-channel blocking read. Kept per-channel rather than one multi-channel
 * sequence for two reasons: the ladder/fader/ladder ordering below has to be exact,
 * and nrfx cannot oversample a multi-channel sequence.
 *
 * 2x oversampling, chosen NOW rather than later on purpose. sp1-tape-looper's author
 * found that "audio/USB activity couples noise into the shared BTN_COM rail, so a
 * single 12-bit sample can land a band boundary off", and settled on 2x plus a sticky
 * debounce. Since the thresholds have not been measured yet, this is the moment to
 * fix the sampling -- they will be measured WITH it, and must not change afterwards.
 *
 * Do not raise it further: the same author found 4x across 6 channels stole enough
 * CPU to break their eMMC streamer. 2x is the settled figure on this board. */
static bool read_one(const struct adc_dt_spec *spec, uint16_t *out)
{
	int16_t buf = 0;
	struct adc_sequence seq = { .buffer = &buf, .buffer_size = sizeof(buf) };

	if (adc_sequence_init_dt(spec, &seq) < 0) {
		return false;
	}
	seq.oversampling = 1;      /* 2^1 = 2 conversions averaged */
	if (adc_read(spec->dev, &seq) < 0) {
		return false;
	}
	*out = (buf < 0) ? 0u : (uint16_t)buf;
	return true;
}

void sp1_controls_rail_on(void)
{
	/* ⚠️ THE RAIL MUST BE POWERED OR EVERY CONTROL READS ZERO.
	 *
	 * P1.10 (BTN_COM) supplies BOTH resistor ladders AND the four faders. Until
	 * it is driven high all six of those channels read ~0. Omitting this was the
	 * whole M1d-a failure: the capture came back as zeros, no fader moved the
	 * display, and button presses coupled tiny blips into the unpowered fader pins
	 * that tripped the movement detector and made the LEDs flicker. One missing
	 * line, three symptoms that looked like three separate bugs.
	 *
	 * sp1-midi models this as a fixed regulator that boots on; we drive it
	 * explicitly, because sp1_power_off() drives it back low on the way to
	 * SYSTEM_OFF (a rail feeding two resistor ladders is a standing current drain
	 * through sleep). So this is idempotent and called from two places: once at
	 * init, and again on every entry to ON, so the control loop never depends on
	 * what the last state transition happened to leave behind.
	 *
	 * OUTSET is written before AND after PIN_CNF on purpose: before, so the pin is
	 * never briefly driven low as it becomes an output; after, because that is the
	 * write that actually matters. */
	SP1_BTN_COM_PORT->OUTSET = (1u << SP1_BTN_COM_PIN);
	SP1_BTN_COM_PORT->PIN_CNF[SP1_BTN_COM_PIN] =
		(GPIO_PIN_CNF_DIR_Output    << GPIO_PIN_CNF_DIR_Pos)  |
		(GPIO_PIN_CNF_DRIVE_S0S1    << GPIO_PIN_CNF_DRIVE_Pos)|
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
	SP1_BTN_COM_PORT->OUTSET = (1u << SP1_BTN_COM_PIN);
}

int sp1_controls_init(void)
{
	sp1_controls_rail_on();

	/* Let the rail and the divider networks settle before anything is believed. */
	k_msleep(2);

	for (int i = 0; i < 2; i++) {
		if (!adc_is_ready_dt(&lad_spec[i]) ||
		    adc_channel_setup_dt(&lad_spec[i]) < 0) {
			return -1;
		}
	}
	for (int i = 0; i < SP1_NUM_FADERS; i++) {
		if (!adc_is_ready_dt(&fad_spec[i]) ||
		    adc_channel_setup_dt(&fad_spec[i]) < 0) {
			return -1;
		}
	}
	ok = true;
	return 0;
}

/* Rail load from the two instantaneous ladder reads.
 *
 * ---- MEASURED 2026-09-19, capture sp1-20260919-180243.log ----
 * Idle is an exact 0 on both ladders, with no scatter at all across 1124 idle
 * samples. The lowest real press measured 209 (ladder 0) and 400 (ladder 1), and
 * nothing on either ladder ever exceeded 1817 of 4095 -- the divider simply cannot
 * reach the top of the range.
 *
 * So the earlier "or near the TOP extreme" half of this test was dead code written
 * against a guess about which way the ladder idles. Removed: a condition that can
 * never be true is worse than no condition, because the next reader assumes it
 * guards something.
 *
 * SP1_RAIL_IDLE_MAX sits between the measured idle (0) and the measured lowest
 * press (209) with about 90 counts of margin on each side. Erring high would freeze
 * the faders on noise; erring low would let a real press through as a valid fader
 * sample. 120 is the midpoint and both margins are ~15x the observed idle noise. */
#define SP1_RAIL_IDLE_MAX 120u

/* ---- sag compensation for FFWD / RWD (M3d) ---- see sp1_controls_scan(). */
#define SP1_LADDER_A      3604.0f   /* ladder model full scale (refit, 13 combos) */
#define SP1_SAG_K         0.0178f   /* rail droop per unit load X (PLAY, measured) */
#define SP1_SAG_STABLE    8u        /* before/after ladder-1 agreement, counts     */
#define SP1_SAG_WINDOW    40u       /* same +-40 window the decoder uses           */

/* ---- audio-load sag compensation (M4a) -- ADARA'S KNOB, 0 until measured ----
 * The speaker trim went from -9 dB to -6 dB in M4a, so the class-D stage now draws up
 * to twice the peak current. Everything above handles sag caused by a BUTTON, which is
 * an event: it is detected and the sample is either discarded or corrected. Amplifier
 * sag is not an event -- it is continuous and proportional to how loud the thing is --
 * so no rejection can catch it. It would show up as all four faders reading slightly
 * LOW while the speaker is loud, i.e. a parameter that drifts with volume.
 *
 * Same shape as the button correction: true = loaded * (1 + K * load), where `load` is
 * sp1_controls_set_audio_load()'s 0..1 (the play-row meter's level, which main feeds in
 * every tick). At 0 this is exactly the M4 behaviour, bit for bit -- nothing changes
 * until a number is measured.
 *
 * ---- how to measure it (the run Adara asked for) ----
 * Park all four faders at mid travel, headphones OUT so the speaker is live, and hold a
 * loud sustained note (LEVEL up so the VCA stays open) with VOL at maximum. Compare the
 * `f1..f4` values in the console's raw lines against the same faders in silence. The
 * shift, divided by the reading and by the meter level, is K. If the shift is under
 * ~5 counts, leave this at 0: the correction would be noise. Do NOT guess a value --
 * the last time a sag number was inferred rather than measured it was wrong by 7x. */
#define SP1_SAG_AUDIO_K   0.0f

static bool fad_compensated;
static bool comp_logged;
static uint16_t last_idle[SP1_NUM_FADERS];
static float audio_load;                      /* 0..1, set by main from the meter */

/* Gain that undoes the amplifier's share of the rail droop. Exactly 1.0 while
 * SP1_SAG_AUDIO_K is 0, so the compiler folds it away until a value is measured. */
static float audio_gain(void)
{
	return 1.0f + SP1_SAG_AUDIO_K * audio_load;
}

static bool near(uint16_t v, uint16_t c)
{
	return (v > c ? v - c : c - v) <= SP1_SAG_WINDOW;
}

static bool sag_compensable(uint16_t b0, uint16_t b1, uint16_t a0, uint16_t a1)
{
	if (b0 > SP1_RAIL_IDLE_MAX || a0 > SP1_RAIL_IDLE_MAX) {
		return false;                              /* ladder 0 busy: reject */
	}
	if ((b1 > a1 ? b1 - a1 : a1 - b1) > SP1_SAG_STABLE) {
		return false;                              /* changing mid-scan     */
	}
	return (near(b1, 1216u) && near(a1, 1216u)) ||      /* FFWD (measured) */
	       (near(b1, 403u) && near(a1, 403u));          /* RWD  (measured) */
}

bool sp1_faders_compensated(void) { return fad_compensated; }

void sp1_controls_set_audio_load(float load)
{
	audio_load = load < 0.0f ? 0.0f : (load > 1.0f ? 1.0f : load);
}

static enum sp1_rail classify_rail(uint16_t l0, uint16_t l1)
{
	return (l0 <= SP1_RAIL_IDLE_MAX && l1 <= SP1_RAIL_IDLE_MAX)
		? SP1_RAIL_IDLE : SP1_RAIL_LOADED;
}

void sp1_controls_scan(void)
{
	if (!ok) {
		return;
	}

	uint16_t before0 = 0, before1 = 0, after0 = 0, after1 = 0;
	bool all_ok = true;

	/* 1. ladders BEFORE */
	all_ok &= read_one(&lad_spec[0], &before0);
	all_ok &= read_one(&lad_spec[1], &before1);

	/* 2. faders */
	for (int i = 0; i < SP1_NUM_FADERS; i++) {
		all_ok &= read_one(&fad_spec[i], &fad_scratch[i]);
	}

	/* 3. ladders AFTER -- catches a press that landed mid-scan, which would
	 *    otherwise sag some faders and not others. */
	all_ok &= read_one(&lad_spec[0], &after0);
	all_ok &= read_one(&lad_spec[1], &after1);

	/* ---- a FAILED read is not data ----
	 * read_one() leaves *out untouched on failure, and the ladder locals start at
	 * 0, so discarding the return value made a failed ladder read indistinguishable
	 * from idle: the button would decode as released, and classify_rail() would
	 * report IDLE and promote a possibly-sagged fader sample into fad_raw as
	 * trustworthy. A failed fader read silently reused the previous scan's value
	 * from the static scratch buffer.
	 *
	 * So: drop the whole scan and keep last tick's state. Stale-but-real beats
	 * synthesised. The debounce above means one dropped scan changes nothing. */
	if (!all_ok) {
		scan_errors++;
		if (scan_errors == 1u || scan_errors % 128u == 0u) {
			printk("CTRL warn: ADC scan failed (%u total)\n", scan_errors);
		}
		return;
	}

	lad_raw[0] = after0;
	lad_raw[1] = after1;

	const enum sp1_rail r_before = classify_rail(before0, before1);
	const enum sp1_rail r_after  = classify_rail(after0, after1);
	rail = (r_before == SP1_RAIL_IDLE && r_after == SP1_RAIL_IDLE)
		? SP1_RAIL_IDLE : SP1_RAIL_LOADED;

	fad_valid = (rail == SP1_RAIL_IDLE);
	fad_compensated = false;
	if (fad_valid) {
		const float ag = audio_gain();
		for (int i = 0; i < SP1_NUM_FADERS; i++) {
			const float v = (float)fad_scratch[i] * ag + 0.5f;
			fad_raw[i] = (uint16_t)(v > 4095.0f ? 4095.0f : v);
		}
	} else if (sag_compensable(before0, before1, after0, after1)) {
		/* ---- FFWD / RWD held: correct the sag instead of discarding (M3d) ----
		 * Adara: faders must keep modulating while FFWD/RWD are held (the burst
		 * and the single TRIG are played WITH a fader). Rejection froze them.
		 *
		 * The rail droops in proportion to the ladder's load X = r / (A - r) (the
		 * same model that decodes the ladders), and the faders are ratiometric to
		 * that rail, so the fix is a gain: true = loaded * (1 + K * X). K is from
		 * the PLAY sag measurement (33 counts at mid-travel for X = 1.011, logs/
		 * sp1-20260920-193610.log). FFWD (X ~0.51) sags ~17 counts at mid-travel
		 * uncorrected, RWD (X ~0.13) ~4; the residual after correction should be a
		 * few counts, a few cents on F1.
		 *
		 * ONLY for a load that is (a) ladder 1 alone, (b) FFWD or RWD, (c) the same
		 * before and after the fader reads. A press landing mid-scan still fails
		 * (c) and is discarded exactly as before. Everything else -- T1-T4, PLAY,
		 * chords, VOL -- keeps plain rejection; K rests on one measured load and is
		 * not trusted beyond the light ones. */
		const uint32_t r = ((uint32_t)before1 + after1) / 2u;
		const float x = (float)r / (SP1_LADDER_A - (float)r);
		const float g = (1.0f + SP1_SAG_K * x) * audio_gain();
		for (int i = 0; i < SP1_NUM_FADERS; i++) {
			float v = (float)fad_scratch[i] * g + 0.5f;
			fad_raw[i] = (uint16_t)(v > 4095.0f ? 4095.0f : v);
		}
		/* One diagnostic line per press, so the correction can be checked from a
		 * log: the last clean reading before the press vs the first corrected one.
		 * With the faders held still they should agree to a few counts. */
		if (!comp_logged) {
			comp_logged = true;
			printk("SAG %s x=%d.%03d: idle %u %u %u %u -> corrected %u %u %u %u\n",
			       near(after1, 1216u) ? "FFWD" : "RWD",
			       (int)x, (int)(x * 1000.0f) % 1000,
			       last_idle[0], last_idle[1],
			       last_idle[2], last_idle[3],
			       (unsigned)((float)fad_scratch[0] * g + 0.5f),
			       (unsigned)((float)fad_scratch[1] * g + 0.5f),
			       (unsigned)((float)fad_scratch[2] * g + 0.5f),
			       (unsigned)((float)fad_scratch[3] * g + 0.5f));
		}
		fad_valid = true;
		fad_compensated = true;
	}
	if (rail == SP1_RAIL_IDLE) {
		comp_logged = false;
		for (int i = 0; i < SP1_NUM_FADERS; i++) {
			last_idle[i] = fad_raw[i];
		}
	}
	if (fad_valid) {
		track_touch();      /* a rejected scan is no evidence either way */
	}

	decode_buttons();
}

/* ---- decode ----
 * Both ladders are read every scan; the tables above turn a reading into a set of
 * pressed buttons. Only a SINGLE press is dispatched: a recognised chord resolves
 * to "nothing pressed", which is the point of carrying all 31 states rather than
 * just the five singles. A chord that resolved to its nearest single would fire a
 * real UI action from a fumbled press. */
static uint8_t  prev_mask0, prev_mask1;
static uint32_t stable0, stable1;

/* Set by sp1_controls_reset_buttons(), consumed per ladder once that ladder's
 * decode has settled. One flag per ladder -- see decode_buttons() for why a single
 * shared flag was wrong twice over. */
static bool swallow0, swallow1;

void sp1_controls_reset_buttons(void)
{
	/* Called on entry to ON. A button still physically down across the
	 * transition must ADOPT that state silently -- it must not surface as a
	 * fresh press, which is what clearing held[] on its own would produce on
	 * the very next scan. So the next decode updates held[] but emits no edges. */
	for (int b = 0; b < SP1_BTN_COUNT; b++) {
		held[b] = false;
		went_down[b] = false;
		went_up[b] = false;
	}
	prev_mask0 = prev_mask1 = 0u;
	stable0 = stable1 = 0u;
	chord_mask = 0u;
	swallow0 = true;
	swallow1 = true;
}

static void decode_buttons(void)
{
	bool now[SP1_BTN_COUNT] = { false };

	const uint8_t m0 = ladder_decode(ladder0, ARRAY_SIZE(ladder0), lad_raw[0]);
	const uint8_t m1 = ladder_decode(ladder1, ARRAY_SIZE(ladder1), lad_raw[1]);

	stable0 = (m0 == prev_mask0) ? (stable0 + 1u) : 0u;
	stable1 = (m1 == prev_mask1) ? (stable1 + 1u) : 0u;
	prev_mask0 = m0;
	prev_mask1 = m1;

	/* `stable0` counts REPEATS, so the number of samples in the run is
	 * stable0 + 1. Settled means DEBOUNCE_SCANS identical decodes in a row. */
	const bool settled0 = (stable0 + 1u >= DEBOUNCE_SCANS);
	const bool settled1 = (stable1 + 1u >= DEBOUNCE_SCANS);

	/* ⚠️ chord_mask is DEBOUNCED, and that matters far more than it looks.
	 *
	 * sp1_controls_activity() returns true on chord_mask != 0, and its caller uses
	 * that to suppress the shutdown gesture -- which LATCHES for the rest of the
	 * hold. So a single stray ADC sample landing inside any of the 31 +-40 count
	 * windows would kill the 3 s power-off gesture entirely, leaving only the 6 s
	 * backstop. Above 1557 those windows are effectively continuous (the largest
	 * gap is 81 counts, so the dead zone is about half a count), which makes a
	 * mid-transition sample while PLAY is being released quite enough to do it.
	 *
	 * Taking the raw instantaneous decode here was exactly the class of mistake
	 * that stranded the device once already: an un-debounced signal used to veto
	 * power-off. */
	chord_mask = settled0 ? m0 : 0u;

	if (settled0 && exactly_one_bit(m0)) {
		switch (m0) {
		case M_T1:   now[SP1_BTN_T1] = true;   break;
		case M_T2:   now[SP1_BTN_T2] = true;   break;
		case M_T3:   now[SP1_BTN_T3] = true;   break;
		case M_T4:   now[SP1_BTN_T4] = true;   break;
		case M_PLAY: now[SP1_BTN_PLAY] = true; break;
		default: break;
		}
	}
	if (settled1 && exactly_one_bit(m1)) {
		switch (m1) {
		case M_VOL_UP:   now[SP1_BTN_VOL_UP] = true;   break;
		case M_VOL_DOWN: now[SP1_BTN_VOL_DOWN] = true; break;
		case M_FFWD:     now[SP1_BTN_FFWD] = true;     break;
		case M_RWD:      now[SP1_BTN_RWD] = true;      break;
		default: break;
		}
	}

	/* ---- edges, with BOTH directions debounced ----
	 * Presses used to require two identical decodes while releases required none:
	 * any single sample that failed the gate -- landing 40 counts off, resolving to
	 * a chord, or matching nothing -- dropped held[] and fired went_up[]. One
	 * physical press then produced release-then-press, a double trigger for any UI
	 * that acts on sp1_button_pressed(). A release is now only believed once the
	 * decode has settled, exactly like a press, so an unsettled sample changes
	 * nothing at all. */
	for (int b = 0; b < SP1_BTN_COUNT; b++) {
		/* Each ladder is judged on its own settling. A button whose ladder has
		 * not settled this scan keeps its previous held[] state and emits no
		 * edge -- that, not a fallback to "released", is what makes one bad
		 * sample a no-op. */
		const bool on_ladder1 = (b >= SP1_BTN_VOL_UP);
		const bool settled    = on_ladder1 ? settled1 : settled0;
		bool *swallow         = on_ladder1 ? &swallow1 : &swallow0;

		if (!settled) {
			went_down[b] = false;
			went_up[b] = false;
			continue;
		}
		went_down[b] = (!*swallow && now[b] && !held[b]);
		went_up[b]   = (!*swallow && !now[b] && held[b]);
		held[b]      = now[b];
	}

	/* ⚠️ THE SWALLOW IS PER-LADDER, and consumed only when THAT ladder settles.
	 *
	 * Two earlier versions of this were wrong. Clearing a single flag on the first
	 * decode was useless, because reset_buttons() zeroes stable0 and so the first
	 * decode can never report a press: the flag was spent on a guaranteed no-op and
	 * the held button surfaced as a fresh press on the next scan. Gating that single
	 * flag on "either ladder settled" failed the same way -- the quiet ladder
	 * settles immediately at idle and consumes the flag on behalf of the busy one.
	 * A held T3 then still produced a spurious went_down. Verified by simulation. */
	if (settled0) { swallow0 = false; }
	if (settled1) { swallow1 = false; }
}

uint8_t sp1_chord_mask(void)
{
	return chord_mask;
}

uint16_t sp1_fader_raw(int i)
{
	return (i >= 0 && i < SP1_NUM_FADERS) ? fad_raw[i] : 0u;
}

/* THIS scan's fader sample, sag and all. Only the sag measurement wants this --
 * everything else wants sp1_fader_raw(), which never returns a sagged reading. */
uint16_t sp1_fader_scratch(int i)
{
	return (i >= 0 && i < SP1_NUM_FADERS) ? fad_scratch[i] : 0u;
}
bool     sp1_faders_valid(void)              { return fad_valid; }
uint16_t sp1_ladder_raw(int which)           { return (which == 0 || which == 1)
							? lad_raw[which] : 0u; }
enum sp1_rail sp1_rail_state(void)           { return rail; }
bool sp1_controls_calibrated(void)           { return THRESHOLDS_MEASURED != 0; }

/* Range-checked, like the fader and ladder accessors. These take an enum, but an
 * enum is an int: a stale or computed value reads past the array otherwise, and the
 * UI layers from M3 on will index these from tables. */
static bool btn_ok(enum sp1_button b) { return (int)b >= 0 && (int)b < SP1_BTN_COUNT; }
bool sp1_button_held(enum sp1_button b)       { return btn_ok(b) && held[b]; }
bool sp1_button_pressed(enum sp1_button b)    { return btn_ok(b) && went_down[b]; }
bool sp1_button_released(enum sp1_button b)   { return btn_ok(b) && went_up[b]; }

int sp1_controls_raw_line(char *buf, size_t len)
{
	/* One line, fixed columns, easy to parse after the fact.
	 *
	 * `t` is uptime in ms. Without it a capture is a pile of numbers with no time
	 * axis: you cannot tell a slow deliberate sweep from a flick, cannot line a
	 * line up against "I pressed T2 about four seconds in", and cannot see where
	 * the logger reconnected after a gap. It costs one k_uptime_get().
	 *
	 * `rail` is here because a fader value is only meaningful when the rail is
	 * idle -- on rail=L a button is down, the shared rail is sagging, and f1..f4
	 * are low by an unknown amount. Those lines are still printed because the
	 * LADDER values on them are the whole point of the capture; it is only the
	 * fader columns that should be ignored.
	 *
	 * Note f1..f4 print fad_scratch, THIS scan's samples, sagged or not -- not the
	 * filtered fad_raw the UI uses. A capture is supposed to show what the ADC
	 * actually returned, including the sag, so the sag can be measured. */
	return snprintf(buf, len,
			"RAW t=%7u lad0=%4u lad1=%4u f1=%4u f2=%4u f3=%4u f4=%4u rail=%c",
			(uint32_t)k_uptime_get_32(),
			lad_raw[0], lad_raw[1],
			fad_scratch[0], fad_scratch[1],
			fad_scratch[2], fad_scratch[3],
			(rail == SP1_RAIL_IDLE) ? 'i' : 'L');
}

/* Fader level scaled to 0-255, for driving its own track LED.
 *
 * ⚠️ NOT `raw >> 4`. The faders top out at ~3701, not 4095 -- measured, and within
 * 4 counts across all four -- so a shift capped the LED at 231/255 and a fader
 * pushed fully up never reached full brightness. Scale against the measured top of
 * travel instead, and clamp, since a unit that reads slightly higher must not wrap.
 *
 * SP1_FADER_FULL is deliberately the measured value rather than 4095. If a future
 * unit measures differently this is the one constant to change. */
#define SP1_FADER_FULL 3701u

uint8_t sp1_fader_level(int i)
{
	const uint32_t v = (uint32_t)sp1_fader_raw(i);
	const uint32_t scaled = (v * 255u) / SP1_FADER_FULL;
	return (uint8_t)(scaled > 255u ? 255u : scaled);
}

/* ---- "was anything else touched?" ----
 *
 * This exists for one job: telling sp1_power.c that "••" is being used as a SHIFT
 * rather than as a power gesture. It deliberately does NOT need the threshold
 * tables, because it does not care WHICH control was touched, only THAT one was:
 *
 *   - any ladder button down  -> sp1_rail_state() == SP1_RAIL_LOADED
 *   - any fader moved         -> compared against a mark taken when "••" went down
 *
 * That is the whole reason it works in M1d-a, before any button decodes. */
static uint16_t mark_fad[SP1_NUM_FADERS];
static bool     marked;

/* Why sp1_controls_activity() last returned true -- for the suppression log line. */
enum { WHY_NONE = 0, WHY_BUTTON, WHY_CHORD, WHY_FADER };
static int      why_kind;
static int      why_index;
static uint16_t why_from, why_to;

/* ~2 % of full scale. Above the ADC scatter we see at rest (the faders sit at an
 * exact 0 when parked, and 2x oversampling keeps mid-travel quiet), and far below
 * any deliberate move -- a real shift gesture sweeps hundreds of counts. */
#define SP1_TOUCH_COUNTS 80u
/* ...and it must be away from the mark on this many VALID scans in a row (2 = 16 ms), the
 * buttons' own debounce. One reading 82 counts off on a fader parked at 0 suppressed a
 * shutdown on 2026-10-15 (0006.LOGS) with nobody touching it; the logged positions show no
 * sign of a worn fader (F4 is the quietest at the bottom), so a lone spike is the likely
 * cause. A real move lasts far longer than 16 ms, so shifting is unchanged. This only
 * decides whether the 3 s animation starts; the 30 s backstop never looks (rule 5a). */
#define SP1_TOUCH_SCANS  2u
static uint8_t touch_run[SP1_NUM_FADERS];   /* valid scans in a row past the threshold */

static void track_touch(void)
{
	for (int i = 0; i < SP1_NUM_FADERS; i++) {
		const uint16_t v = fad_raw[i];
		const uint16_t d = (v > mark_fad[i]) ? (uint16_t)(v - mark_fad[i])
						     : (uint16_t)(mark_fad[i] - v);
		if (!marked || d < SP1_TOUCH_COUNTS) {
			touch_run[i] = 0u;
		} else if (touch_run[i] < 255u) {
			touch_run[i]++;
		}
	}
}

void sp1_controls_activity_mark(void)
{
	for (int i = 0; i < SP1_NUM_FADERS; i++) {
		mark_fad[i] = fad_raw[i];
		touch_run[i] = 0u;
	}
	marked = true;
}

void sp1_controls_activity_clear(void)
{
	marked = false;
	for (int i = 0; i < SP1_NUM_FADERS; i++) {
		touch_run[i] = 0u;
	}
}

bool sp1_controls_activity(void)
{
	/* ---- why this no longer looks at `rail` ----
	 * It used to return true on rail == SP1_RAIL_LOADED, which means nothing more
	 * than "a ladder reads above 120 counts". That is not evidence of a press, it
	 * is evidence of a reading, and on 2026-09-19 it latched: suppression fired on
	 * every tick and the device could not be powered off at all.
	 *
	 * Now that the tables exist, ask the decoder instead. A DECODED button -- or a
	 * recognised chord -- is a real press, matched to within 40 counts of a known
	 * state and debounced over two scans. A ladder sitting at some value that
	 * matches nothing is no longer enough to veto anything.
	 *
	 * The backstop in sp1_power_tick() means a bug here can no longer strand the
	 * device either way. Both changes matter: this one makes the heuristic right,
	 * that one makes its correctness non-load-bearing. */
	for (int b = 0; b < SP1_BTN_COUNT; b++) {
		if (held[b]) {
			why_kind = WHY_BUTTON;
			why_index = b;
			return true;
		}
	}
	if (chord_mask != 0u) {
		why_kind = WHY_CHORD;  /* a recognised multi-press is still deliberate */
		return true;
	}
	if (!marked) {
		return false;
	}
	/* fad_raw is frozen while the rail is loaded, so a press cannot fake a fader
	 * move through sag -- the sagged samples were never stored. */
	for (int i = 0; i < SP1_NUM_FADERS; i++) {
		const uint16_t v = fad_raw[i];
		if (touch_run[i] >= SP1_TOUCH_SCANS) {   /* past the threshold, and stayed */
			why_kind = WHY_FADER;
			why_index = i;
			why_from = mark_fad[i];
			why_to = v;
			return true;
		}
	}
	return false;
}

/* ---- saying WHICH control, not just "a control" ----
 * The first version of the suppression line printed the ladder values and the chord
 * mask. On 2026-09-20 it printed `lad0=0 lad1=0 chord=0x00` three times -- which
 * rules OUT a button but says nothing about what it WAS, so the fader path had to be
 * inferred by elimination. This names the path and, for a fader, the move. */
int sp1_controls_activity_describe(char *buf, size_t len)
{
	static const char *const btn_name[SP1_BTN_COUNT] = {
		"PLAY", "T1", "T2", "T3", "T4", "VOL+", "VOL-", "FFWD", "RWD",
	};
	switch (why_kind) {
	case WHY_BUTTON:
		return snprintf(buf, len, "button %s held",
				(why_index >= 0 && why_index < SP1_BTN_COUNT)
				? btn_name[why_index] : "?");
	case WHY_CHORD:
		return snprintf(buf, len, "chord 0x%02X", chord_mask);
	case WHY_FADER:
		return snprintf(buf, len, "fader F%d moved %u -> %u since \"••\" went down",
				why_index + 1, why_from, why_to);
	default:
		return snprintf(buf, len, "unknown");
	}
}
