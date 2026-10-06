/* wakes-sp1 — the PLAITS page. See sp1_plaits_ui.h.
 *
 * The catch-up algorithm and its constants are from Mutable Instruments Plaits,
 * plaits/pot_controller.h (Emilie Gillet, MIT). The octave-range note mapping is from
 * plaits/ui.cc. Both attributed in NOTICE. */
#include "sp1_plaits_ui.h"

#include "sp1_marbles.h"      /* the scale quantizer FREQUENCY borrows (M4b) */
#include "sp1_midi.h"         /* MIDI's offsets and the quantizer bypass (M5a) */
#include "sp1_ui_timing.h"

#include <math.h>
#include <stddef.h>

/* ---- tuning ---------------------------------------------------------------- */
#define FADER_FULL        3701.0f  /* measured top of travel, M1d (SP1_FADER_FULL) */
#define FADER_LP          0.125f   /* one-pole per tick, ~65 ms at 8 ms ticks      */
#define CATCH_MOVE        0.005f   /* pot_controller.h: movement that counts        */
/* ---- a jump this big in ONE tick is not a hand, it is a bad sample (M4e) ----
 * A fader has ~35 mm of travel. Moving a quarter of it inside one 8 ms tick is 1.1 m/s at
 * the fader cap; nobody does that, and the low-pass above makes it slower still. So a
 * delta over this threshold is treated as a re-SEED of the catch-up reference rather than
 * as movement, and pickup does not drag the stored value along behind it.
 *
 * ⚠️ This is the second half of the M4e attenuverter fix and it is the belt to the
 * braces. The real bug was in main.c: the control rail is raised and sp1_pui_enter() was
 * called before any scan had run, so `pos` was seeded from a stale (post-STANDBY: zero)
 * reading and the next few ticks of the low-pass climbing to the true positions looked
 * exactly like a hand sweeping every fader across its whole travel. Pickup applied that
 * to every catching layer and the attenuverters came up sitting on the faders.
 *
 * main.c now scans until the sample is valid before seeding, which fixes it at source.
 * This makes it unable to happen again from ANY single bad sample -- a dropped scan, a
 * rail transient, a future caller that forgets. Cheap insurance on a bug that has now
 * been chased twice (M4a, M4e). */
#define CATCH_JUMP        0.25f    /* of travel, in one tick: a sampling artefact   */
#define CATCH_MATCH       0.005f   /* pot_controller.h: close enough to track       */
#define DETENT_BIPOLAR    0.10f    /* Adara: 10 % of travel                          */
#define DETENT_FREQ       0.05f    /* Adara: 5 % for FREQUENCY                       */
/* ...and 10 % while MIDI is active (Adara, M5a test notes): with a keyboard playing through
 * FREQUENCY, F1's centre IS the keyboard's tuning -- C4 = note 60 -- and 5 % of travel was too
 * narrow to land on reliably. Only while MIDI is active, so the fader's feel without MIDI is
 * unchanged. Outside the detent the travel is re-stretched, so the ends are still reached. */
#define DETENT_FREQ_MIDI  0.10f
#define LEVEL_OFF         0.05f    /* UI-SPEC: SETTINGS F4 below 5 % = disconnected  */
#define TAP_MAX_MS        300u     /* a "••" press this short, untouched, is a tap   */
#define DOUBLE_TAP_MS     400u     /* second tap must START within this of the 1st   */
#define BREATH_MS         1200u    /* SETTINGS page breathing period                 */

/* ---- the engine list: GENERATED from config/engines.csv ----
 * Which engine is in which slot comes from the user's CSV; each slot's glyph, and each
 * engine's Plaits index and centre detents, from the fixed tables in tools/gen_engines.py
 * (run by the build). The reasons for each detent are in docs/PLAITS-ENGINES.md. Edit
 * the CSV, not this file. */
#include "sp1_engines_gen.h"

#define C_H 0x1u   /* HARMONICS (F4) */
#define C_T 0x2u   /* TIMBRE    (F2) */
#define C_M 0x4u   /* MORPH     (F3) */

/* ---- state ------------------------------------------------------------------ */
static float stored[SP1_PUI_LAYERS][4];   /* pot-space 0..1, before detents     */
static bool  catching[SP1_PUI_LAYERS][4];
static float pos[4];                      /* smoothed physical fader, 0..1       */
static float prev[4];                     /* catch-up reference, per fader       */

static enum sp1_pui_layer page = SP1_PUI_BASE;
static enum sp1_pui_layer active = SP1_PUI_BASE;
static bool  base_seeded;
static int   slot = SP1_ENGINE_DEFAULT_SLOT;   /* position in the generated table */

static bool     fnc_was;
static bool     press_dirty;              /* activity seen during this press     */
static uint32_t press_ms;
static uint32_t since_tap_ms = 0xFFFFFFFFu;
static uint32_t breath_ms;
static int      oct_q = 4;                /* hysteresis state, octave mode 9     */
static int      deg_q;                    /* ... and for its degree sweep (M4b)  */
static int      scale = SP1_PUI_SCALE_OFF;
static bool     level_was;

/* The MIDI destination behind each fader of each layer, for pickup shared / takeover
 * (sp1_midi.h): there the CC moves the stored value itself, as a second hand on the fader. */
static const int8_t midi_dest[SP1_PUI_LAYERS][4] = {
	[SP1_PUI_BASE]     = { SP1_MIDI_D_FREQUENCY, SP1_MIDI_D_TIMBRE, SP1_MIDI_D_MORPH,
			       SP1_MIDI_D_HARMONICS },
	[SP1_PUI_SHIFT]    = { SP1_MIDI_D_FM_ATTENUVERTER, SP1_MIDI_D_TIMBRE_ATTENUVERTER,
			       SP1_MIDI_D_MORPH_ATTENUVERTER, SP1_MIDI_D_HARMONICS_ATTENUVERTER },
	[SP1_PUI_SETTINGS] = { SP1_MIDI_D_OCTAVE_RANGE, SP1_MIDI_D_LPG_COLOUR,
			       SP1_MIDI_D_LPG_DECAY, SP1_MIDI_D_LEVEL },
};

/* ---- helpers ---------------------------------------------------------------- */
static float clamp01(float x)
{
	return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

/* ---- the engine that PLAYS (M5a) ----
 * `slot` is what T2/T3 selected; MIDI's MODEL CC offsets it, through the filled slots of
 * config/engines.csv in order, clamped at both ends (Plaits' own MODEL CV input is an offset
 * from the button-selected model too). A CC at either end reaches every filled slot from
 * any selection. Everything that depends on the engine actually sounding -- its detents,
 * Plaits' engine index, INTELLIGENT's polarity, the log -- uses this; the engine flash and
 * T2/T3 stepping keep using the selection. */
static int eslot(void)
{
	const float off = sp1_midi_offset(SP1_MIDI_D_MODEL);
	if (off == 0.0f) {
		return slot;
	}
	int filled[SP1_ENGINE_SLOTS];
	int n = 0, here = 0;
	for (int i = 0; i < SP1_ENGINE_SLOTS; i++) {
		if (SP1_ENGINE_TABLE[i].on) {
			if (i == slot) {
				here = n;
			}
			filled[n++] = i;
		}
	}
	if (n <= 1) {
		return slot;
	}
	const float step = off * (float)(n - 1);
	int k = here + (int)(step < 0.0f ? step - 0.5f : step + 0.5f);
	k = k < 0 ? 0 : (k > n - 1 ? n - 1 : k);
	return filled[k];
}

static float to01(uint16_t raw)
{
	return clamp01((float)raw * (1.0f / FADER_FULL));
}

/* Centre detent: |p - 0.5| <= w/2 reads exactly 0.5; the remaining travel on each
 * side is stretched linearly so 0 and 1 are still reached. Continuous everywhere. */
static float detent(float p, float w)
{
	const float half = 0.5f * w;
	const float d = clamp01(p) - 0.5f;
	const float a = fabsf(d);
	if (a <= half) {
		return 0.5f;
	}
	const float r = 0.5f * (a - half) / (0.5f - half);
	return d < 0.0f ? 0.5f - r : 0.5f + r;
}

/* Is fader i of layer l a bipolar parameter right now? FREQUENCY is a pitch, not a
 * signed amount, so it displays as a position even though it has a detent. */
static bool bipolar(enum sp1_pui_layer l, int i)
{
	switch (l) {
	case SP1_PUI_SHIFT:
		(void)i;
		return true;                             /* all four are attenuverters (M4a) */
	case SP1_PUI_SETTINGS:
		return false;                            /* range, colour, decay, LEVEL      */
	default: {
		const uint8_t c = SP1_ENGINE_TABLE[eslot()].centre;
		return (i == 1 && (c & C_T)) || (i == 2 && (c & C_M)) || (i == 3 && (c & C_H));
	}
	}
}

static void activate(enum sp1_pui_layer l)
{
	active = l;
	for (int i = 0; i < 4; i++) {
		prev[i] = pos[i];
		catching[l][i] = fabsf(stored[l][i] - pos[i]) >= CATCH_MATCH;
	}
}

/* Re-evaluate pickup for EVERY layer against where the faders are now, not just the
 * one about to be shown. A layer that has never been activated would otherwise be left
 * with catching[] at its zero-initialised false, which means "this fader matches, track
 * it" -- and the first time that layer came up, its four values would snap to the
 * faders. That is how an attenuverter could take a fader's position (M4a). */
static void pickup_all(void)
{
	for (int l = 0; l < SP1_PUI_LAYERS; l++) {
		for (int i = 0; i < 4; i++) {
			catching[l][i] = fabsf(stored[l][i] - pos[i]) >= CATCH_MATCH;
		}
	}
	for (int i = 0; i < 4; i++) {
		prev[i] = pos[i];
	}
}

/* ---- API --------------------------------------------------------------------- */
void sp1_pui_load_mods(const float shift[4], const float settings[4])
{
	for (int i = 0; i < 4; i++) {
		stored[SP1_PUI_SHIFT][i] = clamp01(shift[i]);
		stored[SP1_PUI_SETTINGS][i] = clamp01(settings[i]);
	}
}

void sp1_pui_default_mods(void)
{
	/* SHIFT: four attenuverters, all at centre -- a gain of exactly ZERO. */
	static const float kShift[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
	/* SETTINGS (M4a order): octave range 255/256 = full (plaits/settings.cc's own
	 * default), LPG colour 0, decay 128/256, LEVEL disconnected. */
	static const float kSettings[4] = { 255.0f / 256.0f, 0.0f, 0.5f, 0.0f };
	sp1_pui_load_mods(kShift, kSettings);
	/* The FREQUENCY quantizer is a SETTINGS-page value, so it resets with the page:
	 * off, i.e. every semitone available (M4b). */
	sp1_pui_set_scale(SP1_PUI_SCALE_OFF);
}

void sp1_pui_init(void)
{
	sp1_pui_default_mods();
	base_seeded = false;
	page = active = SP1_PUI_BASE;
}

void sp1_pui_enter(const uint16_t raw[4])
{
	for (int i = 0; i < 4; i++) {
		pos[i] = to01(raw[i]);
	}
	if (!base_seeded) {
		base_seeded = true;
		for (int i = 0; i < 4; i++) {
			stored[SP1_PUI_BASE][i] = pos[i];
		}
	}
	pickup_all();
	page = SP1_PUI_BASE;
	fnc_was = true;          /* ON is entered holding "••": that press is not a tap */
	press_dirty = true;
	press_ms = 0u;
	since_tap_ms = 0xFFFFFFFFu;
	activate(SP1_PUI_BASE);
}

void sp1_pui_resume(const uint16_t raw[4], bool fnc)
{
	for (int i = 0; i < 4; i++) {
		pos[i] = to01(raw[i]);
	}
	pickup_all();
	fnc_was = fnc;
	press_dirty = true;
	press_ms = 0u;
	since_tap_ms = 0xFFFFFFFFu;
	activate(fnc ? SP1_PUI_SHIFT : page);
}

uint32_t sp1_pui_tick(uint32_t elapsed_ms, const uint16_t raw[4], bool valid,
		      bool fnc, bool activity)
{
	uint32_t ev = 0u;

	if (valid) {
		for (int i = 0; i < 4; i++) {
			pos[i] += (to01(raw[i]) - pos[i]) * FADER_LP;
		}
	}

	/* ---- "••" taps: single tap in SETTINGS -> BASE, double tap in BASE -> SETTINGS.
	 * A tap is a press shorter than TAP_MAX_MS during which nothing else was touched.
	 * No tap has any other meaning, so detecting them delays nothing. */
	if (since_tap_ms != 0xFFFFFFFFu) {
		since_tap_ms += elapsed_ms;
	}
	if (fnc) {
		if (!fnc_was) {
			press_ms = 0u;
			press_dirty = false;
		}
		press_ms += elapsed_ms;
		if (activity) {
			press_dirty = true;
		}
	} else if (fnc_was) {
		const bool tap = !press_dirty && press_ms <= TAP_MAX_MS;
		if (tap && page == SP1_PUI_SETTINGS) {
			page = SP1_PUI_BASE;
			since_tap_ms = 0xFFFFFFFFu;
			ev |= SP1_PUI_EV_PAGE;
		} else if (tap && since_tap_ms <= DOUBLE_TAP_MS + press_ms) {
			/* second tap: it started within DOUBLE_TAP_MS of the first ending */
			page = SP1_PUI_SETTINGS;
			since_tap_ms = 0xFFFFFFFFu;
			ev |= SP1_PUI_EV_PAGE;
		} else if (tap) {
			since_tap_ms = 0u;
		} else {
			since_tap_ms = 0xFFFFFFFFu;
		}
	}
	fnc_was = fnc;

	const enum sp1_pui_layer want = fnc ? SP1_PUI_SHIFT : page;
	if (want != active) {
		activate(want);
		ev |= SP1_PUI_EV_LAYER;
	}

	/* ---- pickup, active layer only (pot_controller.h, POT_STATE_CATCHING_UP) ---- */
	if (valid) {
		for (int i = 0; i < 4; i++) {
			float *const s = &stored[active][i];
			if (sp1_midi_fader_held(midi_dest[active][i])) {
				/* Pickup takeover, MIDI plugged in: the CC has this parameter and
				 * the fader rests. Kept as a reference, so after the port goes it
				 * catches up from where it is rather than from where it was. */
				prev[i] = pos[i];
				catching[active][i] = fabsf(*s - pos[i]) >= CATCH_MATCH;
				continue;
			}
			if (!catching[active][i]) {
				*s = pos[i];
				prev[i] = pos[i];
				continue;
			}
			const float delta = pos[i] - prev[i];
			if (fabsf(delta) <= CATCH_MOVE) {
				continue;
			}
			if (fabsf(delta) >= CATCH_JUMP) {
				/* Not a hand (see CATCH_JUMP): take the new position as the
				 * reference and apply no movement. */
				prev[i] = pos[i];
				continue;
			}
			float skew = delta > 0.0f
				? (1.001f - *s) / (1.001f - prev[i])
				: (0.001f + *s) / (0.001f + prev[i]);
			if (skew < 0.1f)  { skew = 0.1f; }
			if (skew > 10.0f) { skew = 10.0f; }
			*s = clamp01(*s + skew * delta);
			prev[i] = pos[i];
			if (fabsf(*s - pos[i]) < CATCH_MATCH) {
				catching[active][i] = false;
				ev |= (SP1_PUI_EV_CAUGHT & (0x10u << i));
			}
		}
	}

	const bool level_now = stored[SP1_PUI_SETTINGS][3] >= LEVEL_OFF;
	if (level_now != level_was) {
		level_was = level_now;
		ev |= SP1_PUI_EV_LEVEL;
	}

	breath_ms = (breath_ms + elapsed_ms) % BREATH_MS;
	return ev;
}

void sp1_pui_midi(void)
{
	/* Every layer, not just the one on show: a host can move a SETTINGS value while you
	 * stand on BASE. A value MIDI moved on the active layer re-checks that fader's pickup,
	 * so the fader catches up with it rather than snapping it back. Other layers re-check
	 * when they are activated (activate()). */
	for (int l = 0; l < SP1_PUI_LAYERS; l++) {
		for (int i = 0; i < 4; i++) {
			float *const s = &stored[l][i];
			if (sp1_midi_drive(midi_dest[l][i], s) && l == (int)active) {
				catching[l][i] = fabsf(*s - pos[i]) >= CATCH_MATCH;
			}
		}
	}
}

int sp1_pui_octave_mode(void)
{
	/* SETTINGS F1 since M4a (it was F4). MIDI's OCTAVE range CC moves the fader position
	 * before it is cut into the eleven modes (M5a), so it steps through whole modes. */
	const float p = clamp01(stored[SP1_PUI_SETTINGS][0] +
				sp1_midi_offset(SP1_MIDI_D_OCTAVE_RANGE));
	int o = (int)(p * 11.0f);
	return o < 0 ? 0 : (o > 10 ? 10 : o);
}

void sp1_pui_params(struct sp1_synth_params *p)
{
	const float *b  = stored[SP1_PUI_BASE];
	const float *sh = stored[SP1_PUI_SHIFT];
	const float *st = stored[SP1_PUI_SETTINGS];
	const int es = eslot();
	const uint8_t c = SP1_ENGINE_TABLE[es].centre;
	/* ---- MIDI is driving the pitch: the quantizer stands aside (M5a, Adara) ----
	 * While MIDI is active the FREQUENCY scale is bypassed -- no quantizing, no note
	 * latch, and mode 9 back to whole octaves. The selected scale is kept and returns at
	 * disconnect. */
	const bool quantize = scale >= 0 && !sp1_midi_active();

	/* ---- FREQUENCY: Plaits' range modes, plaits/ui.cc ----
	 *  0      full range, free    60 + 48t              (NO detent -- Wakes, issue #18)
	 *  1..8   one octave, +-7 st  12*oct + 7t           (detent: the octave's C)
	 *  9      quantized octaves   53 + 14*fine + 12(q-4) (F1 is a switch: no detent)
	 *  10     full range (dflt)   60 + 48t              (detent: C4 = MIDI 60)
	 * where t = 2*F1 - 1.
	 *
	 * ⚠️ Mode 0 is NOT Plaits' mode 0 (the LFO range, -48.37 + 60t). Adara removed it in
	 * issue #18: at the bottom of OCTV it was easy to land in by accident and left the
	 * voice at sub-audio rates. In its place is mode 10 with the centre detent off, so F1
	 * sweeps through C4 without the detent's flat spot and can be set finely around it.
	 * The scale quantizer treats it exactly like mode 10. Routing Marbles to V/Oct still
	 * opens the range to mode 10, detent included (main.c, voct_took_over) -- Adara: one
	 * consistent result, rather than a range that depends on where OCTV happened to be.
	 *
	 * ⚠️ FINE TUNE was removed in M4a (Adara), so `fine` is fixed at its centre. That
	 * costs nothing musically: mode 9 is the mode that QUANTIZES F1 to whole octaves,
	 * and fine tune only ever acted there -- in every other mode Plaits ignored it. At
	 * centre the mode-9 formula reduces to 60 + 12(q-4), i.e. C4 at the middle step. */
	const int oct = sp1_pui_octave_mode();
	/* The scale, if one is selected (M4b). Fetched once: it also decides what mode 9
	 * means. `degrees` is empty when no scale is on. */
	float degrees[16];
	const int n_deg = quantize ? sp1_marbles_plaits_degrees(degrees, 16) : 0;
	/* MIDI's FREQUENCY CC (M5a). Mode 9 makes F1 a switch, so there the CC moves the
	 * position BEFORE it is quantized and steps like the fader does; everywhere else the
	 * audio thread adds it, smoothed, at freq_per_travel semitones per unit of travel. */
	const float f1 = (oct == 9)
		? clamp01(b[0] + sp1_midi_offset(SP1_MIDI_D_FREQUENCY)) : b[0];
	p->freq_per_travel = oct == 9 ? 0.0f : ((oct == 0 || oct == 10) ? 96.0f : 14.0f);
	float note;
	if (oct == 9 && n_deg > 0) {
		/* ---- mode 9 with a scale: sweep SCALE DEGREES across the nine octaves ----
		 * Upstream mode 9 quantizes F1 to whole octaves. With a scale selected that
		 * would be a no-op (an octave is degree 0 of every scale), so the mode
		 * becomes what it should be: F1 steps through every degree of the scale, in
		 * order, across the same nine octaves. Seven degrees gives 63 steps, about
		 * 59 fader counts each. Same asymmetric hysteresis as the octave version. */
		const int steps = 9 * n_deg;
		const float v = f1 * (float)steps - 0.5f;
		const float h = v > (float)deg_q ? -0.01f : 0.01f;
		int q = (int)(v + h + 0.5f);
		q = q < 0 ? 0 : (q > steps - 1 ? steps - 1 : q);
		deg_q = q;
		note = 60.0f + 12.0f * (float)(q / n_deg - 4) + degrees[q % n_deg];
	} else if (oct == 9) {
		/* stmlib HysteresisQuantizer2(9 steps, 0.01, asymmetric) */
		const float v = f1 * 9.0f - 0.5f;
		const float h = v > (float)oct_q ? -0.01f : 0.01f;
		int q = (int)(v + h + 0.5f);
		q = q < 0 ? 0 : (q > 8 ? 8 : q);
		oct_q = q;
		note = 53.0f + 0.5f * 14.0f + 12.0f * (float)(q - 4);   /* fine at centre */
	} else {
		const float t = (oct == 0)
			? 2.0f * clamp01(b[0]) - 1.0f
			: 2.0f * detent(b[0], sp1_midi_active() ? DETENT_FREQ_MIDI
							       : DETENT_FREQ) - 1.0f;
		note = (oct == 0 || oct == 10) ? 60.0f + t * 48.0f : t * 7.0f + (float)oct * 12.0f;
		if (n_deg > 0) {
			/* Modes 0-8 and 10: the range decides the SPAN, the scale decides
			 * which notes inside it exist. They compose (Adara). */
			note = sp1_marbles_plaits_quantize(note);
		}
	}
	p->note = note;
	/* ---- and ask the audio thread to HOLD it between TRIGs (M4e, Adara) ----
	 * Only while a scale is selected. The latch itself is in sp1_synth.cc because that is
	 * the only place TRIG exists; all the UI does is say "this note is on a grid, so it
	 * should not slide under a decaying voice". Without a scale, F1 stays continuous. */
	p->note_hold = quantize ? 1 : 0;

	p->timbre    = (c & C_T) ? detent(b[1], DETENT_BIPOLAR) : clamp01(b[1]);
	p->morph     = (c & C_M) ? detent(b[2], DETENT_BIPOLAR) : clamp01(b[2]);
	p->harmonics = (c & C_H) ? detent(b[3], DETENT_BIPOLAR) : clamp01(b[3]);

	/* Attenuverters: bipolar, always detented, -1..+1 (ui.cc: scale 2, offset -1).
	 * Since M4a every SHIFT fader sits under the BASE parameter it modulates:
	 * F1 FREQUENCY -> FM, F2 TIMBRE, F3 MORPH, F4 HARMONICS. The mismatch M3b had to
	 * accept (LEVEL on F4) is gone -- LEVEL moved to SETTINGS F4. */
	p->fm_mod     = 2.0f * detent(sh[0], DETENT_BIPOLAR) - 1.0f;
	p->timbre_mod = 2.0f * detent(sh[1], DETENT_BIPOLAR) - 1.0f;
	p->morph_mod  = 2.0f * detent(sh[2], DETENT_BIPOLAR) - 1.0f;
	p->harm_mod   = 2.0f * detent(sh[3], DETENT_BIPOLAR) - 1.0f;

	/* LEVEL (SETTINGS F4 since M4a): below 5 % disconnected; above, 0..1 over the
	 * remaining travel. MIDI's LEVEL CC moves the fader position (M5a), so it can connect
	 * a disconnected LEVEL exactly as pushing the fader up would; the threshold is decided
	 * here on its target, and the audio thread adds its smoothed value to level_pos. */
	const float lpos = st[3] + sp1_midi_offset(SP1_MIDI_D_LEVEL);
	p->level_pos = clamp01(st[3]);
	if (lpos < LEVEL_OFF) {
		p->level_patched = 0;
		p->level = 0.0f;
	} else {
		p->level_patched = 1;
		p->level = clamp01((st[3] - LEVEL_OFF) / (1.0f - LEVEL_OFF));
	}

	p->lpg_colour = clamp01(st[1]);
	p->decay      = clamp01(st[2]);
	p->engine     = SP1_ENGINE_TABLE[es].plaits;
	p->engine_centre = c;          /* how a MIDI CC on F2-F4 reads (sp1_midi.h) */
}

static void leds_of(enum sp1_pui_layer l, uint8_t out[4])
{
	uint32_t k = 256u;                         /* x/256 brightness factor */
	if (l == SP1_PUI_SETTINGS) {
		const uint32_t half = BREATH_MS / 2u;
		const uint32_t tri = breath_ms < half ? breath_ms : BREATH_MS - breath_ms;
		k = 90u + (166u * tri) / half;         /* 35 % .. 100 % (Adara: 20 points
		                                        * darker at the bottom than M3b's 55 %) */
	}
	for (int i = 0; i < 4; i++) {
		/* Bipolar parameters show their MAGNITUDE (Adara, M3b): full negative and
		 * full positive are both 100 %, the centre is dark; the fader position says
		 * which side. The detent applies, so the whole centre band reads dark. */
		const float x = bipolar(l, i)
			? 2.0f * fabsf(detent(stored[l][i], DETENT_BIPOLAR) - 0.5f)
			: clamp01(stored[l][i]);
		const uint32_t v = (uint32_t)(x * 255.0f + 0.5f);
		out[i] = (uint8_t)((v * k) >> 8);
	}
}

void sp1_pui_leds(uint8_t out[4])      { leds_of(active, out); }
void sp1_pui_page_leds(uint8_t out[4]) { leds_of(page, out); }

enum sp1_pui_layer sp1_pui_active(void) { return active; }
enum sp1_pui_layer sp1_pui_page(void)   { return page; }
bool sp1_pui_catching(int f)            { return f >= 0 && f < 4 && catching[active][f]; }
bool sp1_pui_level_connected(void)      { return stored[SP1_PUI_SETTINGS][3] >= LEVEL_OFF; }
int  sp1_pui_engine(void)               { return SP1_ENGINE_TABLE[eslot()].plaits; }
int  sp1_pui_slot(void)                 { return slot; }
uint8_t sp1_pui_engine_centre(void)     { return SP1_ENGINE_TABLE[eslot()].centre; }
int  sp1_pui_scale(void)                { return scale; }

void sp1_pui_set_scale(int s)
{
	scale = (s >= 0 && s < SP1_MARBLES_SCALES) ? s : SP1_PUI_SCALE_OFF;
	sp1_marbles_plaits_scale(scale);
}

void sp1_pui_set_octave_max(void)
{
	/* Mode 10, Plaits' full 96-semitone range -- and the value Plaits itself stores
	 * for it (255/256), so it matches what defaults_mods() writes. */
	stored[SP1_PUI_SETTINGS][0] = 255.0f / 256.0f;
	/* The fader no longer matches the value, so let pickup catch it up rather than
	 * having the next tick snap the range straight back. */
	catching[SP1_PUI_SETTINGS][0] = fabsf(stored[SP1_PUI_SETTINGS][0] - pos[0])
					>= CATCH_MATCH;
	prev[0] = pos[0];
}

void sp1_pui_rip(void)
{
	/* ---- "rip out the cables" on PLAITS: a FULL PATCH WIPE (Adara, M4d) ----
	 * ⚠️ Through M4b this was a modulation reset that deliberately kept BASE and the
	 * engine. docs/DEFAULTS.md changed that: it now wipes the patch. Engine back to
	 * slot 1, the four BASE faders to THAT engine's neutral state, everything on the
	 * SHIFT and SETTINGS layers to its default, and the FREQUENCY quantizer off.
	 *
	 * Neutral means: 0.5 for a BIPOLAR parameter -- its centre is the point where the
	 * effect is absent -- and 0 for a unipolar one, which has no such point. Which is
	 * which is per engine and is already known: the detent table in
	 * tools/gen_engines.py, compiled into SP1_ENGINE_TABLE[].centre. F1 goes to its
	 * centre, which in the full range is exactly MIDI 60, C4 (60 + 48*(2*0.5-1)), and
	 * has a 5 % detent there so it is a real landing spot.
	 *
	 * ⚠️ The FADERS DO NOT MOVE (Adara: by design). This sets the stored values; pickup
	 * then catches each fader up on its next movement, so straight after a rip the
	 * instrument sounds neutral while the faders still look wrong. Same as it has always
	 * been for SHIFT and SETTINGS, just far more visible now that BASE is in scope.
	 *
	 * ⚠️ Neutral is a MOMENT IN TIME (Adara). Step to an engine with a different bipolar
	 * set afterwards and the values stay where the rip put them. Re-neutralising on
	 * every engine change would mean losing a patch just by browsing engines. */
	slot = SP1_ENGINE_DEFAULT_SLOT;
	const uint8_t c = SP1_ENGINE_TABLE[slot].centre;
	stored[SP1_PUI_BASE][0] = 0.5f;                        /* FREQUENCY -> C4 */
	stored[SP1_PUI_BASE][1] = (c & C_T) ? 0.5f : 0.0f;     /* TIMBRE          */
	stored[SP1_PUI_BASE][2] = (c & C_M) ? 0.5f : 0.0f;     /* MORPH           */
	stored[SP1_PUI_BASE][3] = (c & C_H) ? 0.5f : 0.0f;     /* HARMONICS       */
	sp1_pui_default_mods();          /* attenuverters 0, SETTINGS, quantizer off */
	pickup_all();
}

void sp1_pui_engine_leds(uint8_t out[4])
{
	/* The engine PLAYING, not the one T2/T3 selected (Adara, M5a): with MIDI's MODEL CC
	 * offsetting the selection, the glyph has to show what you hear. Without a MODEL offset
	 * the two are the same slot. */
	sp1_pui_slot_leds(eslot(), out);
}
const char *sp1_pui_engine_name(void)   { return SP1_ENGINE_TABLE[eslot()].name; }
int  sp1_pui_eslot(void)                { return eslot(); }

void sp1_pui_slot_leds(int s, uint8_t out[4])
{
	static const uint8_t L[3] = { 0u, SP1_ENGINE_LED_HALF, SP1_ENGINE_LED_FULL };
	if (s < 0 || s >= SP1_ENGINE_SLOTS) {
		s = slot;
	}
	for (int i = 0; i < 4; i++) {
		const uint8_t k = SP1_ENGINE_TABLE[s].led[i];
		out[i] = L[k < 3u ? k : 0u];
	}
}

/* ---- engine select (T2 / T3), in SLOT order from config/engines.csv ----
 * A slot with no engine in the CSV is empty: it keeps its place and glyph, and is
 * skipped. The build guarantees at least one slot is filled. Wraps at both ends. */
int sp1_pui_engine_step(int dir)
{
	int s2 = slot;
	for (int n = 0; n < SP1_ENGINE_SLOTS; n++) {
		s2 = (s2 + (dir < 0 ? SP1_ENGINE_SLOTS - 1 : 1)) % SP1_ENGINE_SLOTS;
		if (SP1_ENGINE_TABLE[s2].on) {
			break;
		}
	}
	slot = s2;
	return slot;
}

const char *sp1_pui_layer_name(enum sp1_pui_layer l)
{
	switch (l) {
	case SP1_PUI_BASE:     return "base";
	case SP1_PUI_SHIFT:    return "shift";
	case SP1_PUI_SETTINGS: return "settings";
	default:               return "?";
	}
}
