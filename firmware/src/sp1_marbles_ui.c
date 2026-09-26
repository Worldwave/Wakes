/* wakes-sp1 — the MARBLES pages. See sp1_marbles_ui.h.
 *
 * The catch-up algorithm and its constants are from Mutable Instruments Plaits,
 * plaits/pot_controller.h (Emilie Gillet, MIT) -- the same as sp1_plaits_ui.c. The
 * loop-length table and the DEJA VU / quantizer hysteresis follow marbles/marbles.cc
 * (Emilie Gillet, MIT). Attributed in NOTICE. */
#include "sp1_marbles_ui.h"

#include "sp1_ui_timing.h"

#include <math.h>
#include <stddef.h>

/* ---- tuning (the same values as the PLAITS page) ---------------------------- */
#define FADER_FULL        3701.0f
#define FADER_LP          0.125f
#define CATCH_MOVE        0.005f
#define CATCH_MATCH       0.005f
/* A jump this big in ONE tick is a sampling artefact, not a hand (M4e). The reasoning,
 * and the bug it is the second half of the fix for, is in sp1_plaits_ui.c -- kept there
 * because that is where the symptom was, and it is the same number for the same reason. */
#define CATCH_JUMP        0.25f
#define DETENT_BIPOLAR    0.10f
#define TAP_MAX_MS        300u
#define DOUBLE_TAP_MS     400u
#define BREATH_MS         1200u
#define MOVE_SHOW         0.02f    /* fader travel that brings up the value overlay */

/* marbles/marbles.cc: DEJA VU loop lengths along the LENGTH knob. */
static const uint8_t loop_length[] = {
	1,
	2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
	3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	5, 5, 5, 5,
	6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	7, 7,
	8, 8, 8, 8, 8, 8, 8, 8, 8,
	10, 10, 10,
	12, 12, 12, 12, 12, 12, 12,
	14, 14,
	16
};
#define LOOP_STEPS ((int)(sizeof(loop_length) / sizeof(loop_length[0])))

/* ---- the t models: ONE ring of six (Adara, M4a) ----
 * Marbles splits these into two banks of three and hides the second behind a long
 * press of [E]. Here [E] simply steps all six and there is no long press. Glyphs are
 * free-form and full-brightness, chosen so none of them is one of the counting bars
 * used by the destination and scale flashes. docs/MARBLES-SETTINGS.md is the table. */
#define SP1_MUI_MODELS 6
static const uint8_t model_glyph[SP1_MUI_MODELS] = {
	0x9u,   /* ●○○● coin toss (complementary Bernoulli): two opposite ends  */
	0x6u,   /* ○●●○ clusters: bunched in the middle                        */
	0xAu,   /* ●○●○ drums: a beat                                          */
	0x5u,   /* ○●○● independent Bernoulli: the offbeat twin of drums        */
	0xDu,   /* ●●○● divider: a gap where the division falls                */
	0xBu,   /* ●○●● three states: three of four                            */
};

/* ---- the scales ----
 * Marbles ships six (sp1_marbles_scales.inc, from marbles/settings.cc); slot 7 is the
 * free one for Adara to fill. Glyphs are those of engine slots 1-7 (SLOT_GLYPHS in
 * tools/gen_engines.py), as she asked; a copy. "on" is the inclusion column of
 * docs/MARBLES-SETTINGS.md.
 *
 * ⚠️ This table and that document must agree; the document is the authority and
 * explains the degree / weight format so a custom scale can be written. Unlike the
 * engine list it is NOT build-generated -- if it grows a third consumer, generate it. */
static const uint8_t scale_glyph[SP1_MUI_SCALES] = {
	0x8u,   /* ●○○○ 1 major      */
	0xCu,   /* ●●○○ 2 minor      */
	0xEu,   /* ●●●○ 3 pentatonic */
	0xFu,   /* ●●●● 4 pelog      */
	0x7u,   /* ○●●● 5 bhairav    */
	0x3u,   /* ○○●● 6 shri       */
	0x1u,   /* ○○○● 7 free slot  */
};
static const bool scale_on[SP1_MUI_SCALES] = {
	true, true, true, true, true, true,
	false,  /* the free slot: chromatic until Adara defines it (M4a) */
};

/* ---- defaults ----
 * Marbles' own where it has one (marbles/settings.cc); the rest chosen so that PLAY
 * with no edits plays something: coin toss, 120 BPM = RATE at its centre, and the routing
 * in boot_routing() below -- t2 -> TRIG since M4d, X1 -> V/Oct. */
#define DEF_RATE        0.5f     /* 120 BPM                                         */
#define DEF_LENGTH      0.78f    /* 8 steps                                         */
#define DEF_STEPS       0.66f    /* quantized to the scale, every degree            */
/* [J] starts INTELLIGENT (Adara, M4c): the range follows what each output is routed to and
 * the engine's parameter polarity, which is right far more often than any fixed choice.
 * 0..2 V was the M4-M4d default, chosen because X1 -> V/Oct.
 *
 * ⚠️ ONE value for X AND Y since M4e (Adara). In Marbles' own manual [J] is a single
 * button and Y's range is the same button with a modifier held, so two independent
 * settings were never in the hardware we are copying. They are now one control on the
 * MARBLES SETTINGS page, T4. */
#define DEF_RANGE       SP1_MRB_RANGE_INTELLIGENT

/* ---- state ------------------------------------------------------------------- */
static float stored[SP1_MUI_LAYERS][4];
static bool  catching[SP1_MUI_LAYERS][4];
static float pos[4];
static float prev[4];
static float shown[4];                     /* fader positions last reflected      */
static uint32_t show_ms;                   /* value overlay on BASE, counts down  */

static enum sp1_mui_page page = SP1_MUI_PAGE_T;
static bool  settings;                     /* SETTINGS latched                    */
static enum sp1_mui_layer active = SP1_MUI_T_BASE;

static int model, t_range, range, diversity, scale;   /* `range` = [J], X and Y */
static bool dv_t, dv_x;                    /* [F] and [G]: DEJA VU per side        */
static struct sp1_mui_routing route;

/* ---- F4 is ONE control, shared by the t and X pages (Adara, M4b) ----
 * Marbles has one DEJA VU knob and one LENGTH knob; M4 gave each of them two slots and
 * copied between them every tick. Copying makes the VALUES agree but leaves the two pages
 * with separate PICKUP state, so switching pages could put F4 back into catch-up for a
 * value that had not moved. These map the X page's F4 onto the t page's slot, so there is
 * only ever one value and one pickup state.
 *   BASE  F4 = DEJA VU [H]   ·   SHIFT F4 = LENGTH [I] */
static enum sp1_mui_layer canon(enum sp1_mui_layer l, int i)
{
	if (i == 3) {
		if (l == SP1_MUI_X_BASE)  { return SP1_MUI_T_BASE; }
		if (l == SP1_MUI_X_SHIFT) { return SP1_MUI_T_SHIFT; }
	}
	return l;
}
#define VAL(l, i) (stored[canon((l), (i))][(i)])
#define CAT(l, i) (catching[canon((l), (i))][(i)])

/* The order a tap cycles the destinations, per side. BOTH rings start at NONE (Adara's
 * correction to docs/UI-PAGES.md, M4b), so the two sides read the same way round and
 * "disconnect this output" is always one step back from the first real destination. */
static const uint8_t x_ring[] = {
	SP1_DEST_NONE, SP1_DEST_FM, SP1_DEST_TIMBRE, SP1_DEST_MORPH,
	SP1_DEST_HARM, SP1_DEST_VOCT, SP1_DEST_LEVEL,
};
static const uint8_t t_ring[] = {
	SP1_DEST_NONE, SP1_DEST_TRIG, SP1_DEST_LEVEL, SP1_DEST_FM,
	SP1_DEST_TIMBRE, SP1_DEST_MORPH, SP1_DEST_HARM,
};
#define X_RING_N ((int)(sizeof(x_ring) / sizeof(x_ring[0])))
#define T_RING_N ((int)(sizeof(t_ring) / sizeof(t_ring[0])))

static bool     fnc_was;
static bool     press_dirty;
static uint32_t press_ms;
static uint32_t since_tap_ms = 0xFFFFFFFFu;
static uint32_t breath_ms;

/* zone selectors with hysteresis (stmlib HysteresisQuantizer2 behaviour) */
static int q_ydiv, q_length;

/* ---- helpers ------------------------------------------------------------------ */
static float clamp01(float x)
{
	return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

static float to01(uint16_t raw)
{
	return clamp01((float)raw * (1.0f / FADER_FULL));
}

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

/* stmlib::HysteresisQuantizer2(n, hysteresis, symmetric = false) */
static int zone(float v, int n, float hyst, int *state)
{
	const float x = clamp01(v) * (float)n - 0.5f;
	const float h = x > (float)*state ? -hyst : hyst;
	int q = (int)(x + h + 0.5f);
	q = q < 0 ? 0 : (q > n - 1 ? n - 1 : q);
	*state = q;
	return q;
}

static bool is_bipolar(enum sp1_mui_layer l, int i)
{
	switch (l) {
	case SP1_MUI_T_BASE: return i == 1 || i == 3;
	case SP1_MUI_X_BASE: return i >= 1;
	case SP1_MUI_Y:      return i == 1 || i == 2;
	default:             return false;
	}
}

/* Faders with nothing bound to them. They read dark and are ignored. X SHIFT F1 (was
 * SCALE) and F2 (was X CLOCK) joined the list in M4a -- see the header. */
static bool is_reserved(enum sp1_mui_layer l, int i)
{
	return (l == SP1_MUI_T_SHIFT && i == 0) || (l == SP1_MUI_X_SHIFT && i <= 2);
}

static enum sp1_mui_layer base_of(enum sp1_mui_page p)
{
	return p == SP1_MUI_PAGE_X ? SP1_MUI_X_BASE : SP1_MUI_T_BASE;
}

static void activate(enum sp1_mui_layer l)
{
	active = l;
	for (int i = 0; i < 4; i++) {
		prev[i] = pos[i];
		CAT(l, i) = fabsf(VAL(l, i) - pos[i]) >= CATCH_MATCH;
		shown[i] = pos[i];
	}
}

static void defaults_pages(void)
{
	stored[SP1_MUI_T_SHIFT][0] = 0.0f;
	stored[SP1_MUI_T_SHIFT][1] = 0.5f;       /* gate length 128/256 (Marbles)   */
	stored[SP1_MUI_T_SHIFT][2] = 0.0f;       /* no randomness                   */
	stored[SP1_MUI_T_SHIFT][3] = DEF_LENGTH;
	stored[SP1_MUI_X_SHIFT][0] = 0.0f;                 /* reserved (M4a)        */
	stored[SP1_MUI_X_SHIFT][1] = 0.0f;                 /* reserved (M4a)        */
	stored[SP1_MUI_X_SHIFT][2] = 0.0f;                 /* reserved              */
	/* X_SHIFT[3] is not a slot: F4 there addresses T_SHIFT[3] (LENGTH), see canon(). */
	stored[SP1_MUI_Y][0] = 0.5f;                       /* Marbles: 128/256      */
	stored[SP1_MUI_Y][1] = 0.5f;
	stored[SP1_MUI_Y][2] = 0.0f;
	stored[SP1_MUI_Y][3] = 6.5f / 12.0f;               /* 1/8 of t2 (128/256)   */
	q_ydiv = 6;
	q_length = 57;
	model = 0;
	t_range = 1;
	range = DEF_RANGE;
	diversity = 0;
	scale = 0;                                         /* major                 */
	/* [F] and [G]: Marbles applies DEJA VU to both sections unless you turn one off. */
	dv_t = true;
	dv_x = true;
}

/* ---- boot routing vs rip routing: they are NOT the same any more (Adara, M4d) ----
 * docs/DEFAULTS.md is the spec. Boot gives you something that plays the moment you press
 * PLAY; a rip gives you a blank patch bay. */
static void boot_routing(void)
{
	/* ⚠️ TRIG comes from t2, the MASTER clock, which fires on every tick in every
	 * model -- so this is a steady stream of notes with no rhythmic randomness. That
	 * is deliberate (Adara, M4d): it is the most predictable "press PLAY and hear
	 * something" default there is, and t1 / t3 are then yours to route. It replaces
	 * M4b's t1 + t3, which gave the models' random rhythm straight away. */
	route.t_dest[0] = SP1_DEST_NONE;
	route.t_dest[1] = SP1_DEST_TRIG;
	route.t_dest[2] = SP1_DEST_NONE;
	/* X1 -> V/Oct (no attenuverter, so Plaits' internal envelope keeps TIMBRE /
	 * MORPH / FM / HARMONICS); X2, X3 and Y start disconnected. */
	route.dest[0] = SP1_DEST_VOCT;
	route.dest[1] = SP1_DEST_NONE;
	route.dest[2] = SP1_DEST_NONE;
	route.dest[3] = SP1_DEST_NONE;
}

static void rip_routing(void)
{
	/* Every destination disconnected: "rip out the cables" now means what it says.
	 * ⚠️ So after a rip on MARBLES, pressing PLAY makes no sound until something is
	 * dialled back in. Intended (Adara, M4d). */
	for (int i = 0; i < 3; i++) { route.t_dest[i] = SP1_DEST_NONE; }
	for (int i = 0; i < 4; i++) { route.dest[i] = SP1_DEST_NONE; }
}

/* Every BASE fader back to its default. The rip resets these too (docs/DEFAULTS.md):
 * t RATE / BIAS / JITTER / DEJA VU and X SPREAD / BIAS / STEPS. */
static void defaults_base(void)
{
	stored[SP1_MUI_T_BASE][0] = DEF_RATE;
	stored[SP1_MUI_T_BASE][1] = 0.5f;
	stored[SP1_MUI_T_BASE][2] = 0.0f;
	stored[SP1_MUI_T_BASE][3] = 0.0f;      /* DEJA VU, shared with the X page */
	stored[SP1_MUI_X_BASE][0] = 0.5f;
	stored[SP1_MUI_X_BASE][1] = 0.5f;
	stored[SP1_MUI_X_BASE][2] = DEF_STEPS;
}

/* ---- API ---------------------------------------------------------------------- */
void sp1_mui_init(void)
{
	/* X_BASE[3] is not a slot: F4 there addresses T_BASE[3] (DEJA VU), see canon(). */
	defaults_base();
	defaults_pages();
	boot_routing();
	page = SP1_MUI_PAGE_T;
	settings = false;
	active = SP1_MUI_T_BASE;
}

void sp1_mui_rip(void)
{
	/* A full wipe of the module, minus where you are standing: the page you are on and
	 * whether SETTINGS is latched are kept (Adara, M4d). main.c re-seeds the random
	 * stream and re-draws the DEJA VU loop, and leaves the clock running.
	 * ⚠️ The faders do not move; pickup catches them up, as on PLAITS. */
	defaults_base();
	defaults_pages();
	rip_routing();
	activate(active);
}

void sp1_mui_enter(const uint16_t raw[4], bool fnc)
{
	for (int i = 0; i < 4; i++) {
		pos[i] = to01(raw[i]);
	}
	fnc_was = fnc;
	press_dirty = true;       /* a "••" already down is not a tap */
	press_ms = 0u;
	since_tap_ms = 0xFFFFFFFFu;
	show_ms = 0u;
	activate(fnc ? (page == SP1_MUI_PAGE_X ? SP1_MUI_X_SHIFT : SP1_MUI_T_SHIFT)
		     : (settings ? SP1_MUI_Y : base_of(page)));
}

uint32_t sp1_mui_tick(uint32_t elapsed_ms, const uint16_t raw[4], bool valid,
		      bool fnc, bool activity)
{
	uint32_t ev = 0u;

	if (valid) {
		for (int i = 0; i < 4; i++) {
			pos[i] += (to01(raw[i]) - pos[i]) * FADER_LP;
		}
	}

	/* ---- "••" taps: the PLAITS page's grammar ---- */
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
		if (tap && settings) {
			settings = false;
			since_tap_ms = 0xFFFFFFFFu;
			ev |= SP1_MUI_EV_PAGE;
		} else if (tap && since_tap_ms <= DOUBLE_TAP_MS + press_ms) {
			settings = true;
			since_tap_ms = 0xFFFFFFFFu;
			ev |= SP1_MUI_EV_PAGE;
		} else if (tap) {
			since_tap_ms = 0u;
		} else {
			since_tap_ms = 0xFFFFFFFFu;
		}
	}
	fnc_was = fnc;

	const enum sp1_mui_layer want =
		fnc ? (page == SP1_MUI_PAGE_X ? SP1_MUI_X_SHIFT : SP1_MUI_T_SHIFT)
		    : (settings ? SP1_MUI_Y : base_of(page));
	if (want != active) {
		activate(want);
		ev |= SP1_MUI_EV_LAYER;
	}

	/* ---- pickup, active layer only (pot_controller.h, POT_STATE_CATCHING_UP) ---- */
	if (valid) {
		for (int i = 0; i < 4; i++) {
			float *const s = &VAL(active, i);
			if (!CAT(active, i)) {
				*s = pos[i];
				prev[i] = pos[i];
			} else {
				const float delta = pos[i] - prev[i];
				if (fabsf(delta) >= CATCH_JUMP) {
					prev[i] = pos[i];        /* artefact: no movement */
				} else if (fabsf(delta) > CATCH_MOVE) {
					float skew = delta > 0.0f
						? (1.001f - *s) / (1.001f - prev[i])
						: (0.001f + *s) / (0.001f + prev[i]);
					if (skew < 0.1f)  { skew = 0.1f; }
					if (skew > 10.0f) { skew = 10.0f; }
					*s = clamp01(*s + skew * delta);
					prev[i] = pos[i];
					if (fabsf(*s - pos[i]) < CATCH_MATCH) {
						CAT(active, i) = false;
						ev |= (SP1_MUI_EV_CAUGHT & (0x10u << i));
					}
				}
			}
			/* Value overlay on BASE: a fader that has really moved. */
			if (fabsf(pos[i] - shown[i]) > MOVE_SHOW) {
				shown[i] = pos[i];
				show_ms = SP1_DISP_VALUE_HOLD_MS;
			}
		}
	}

	show_ms = show_ms > elapsed_ms ? show_ms - elapsed_ms : 0u;
	breath_ms = (breath_ms + elapsed_ms) % BREATH_MS;
	return ev;
}

/* ---- buttons ---------------------------------------------------------------------- */
bool sp1_mui_set_page(enum sp1_mui_page p)
{
	if (p == page) {
		return false;
	}
	page = p;
	if (!settings && active != SP1_MUI_T_SHIFT && active != SP1_MUI_X_SHIFT) {
		activate(base_of(page));
	}
	return true;
}

void sp1_mui_model_tap(void)
{
	model = (model + 1) % SP1_MUI_MODELS;
}

void sp1_mui_diversity_step(void) { diversity = (diversity + 1) % 3; }
/* [J], one control for both groups (M4e). */
void sp1_mui_range_step(void)     { range = (range + 1) % SP1_MRB_RANGE_COUNT; }

bool sp1_mui_t_range_step(int dir)
{
	const int r = t_range + (dir > 0 ? 1 : -1);
	if (r < 0 || r > 2) {
		return false;                       /* no wrap (Adara) */
	}
	t_range = r;
	return true;
}

/* One step along a destination ring. A value that is somehow not in the ring (only
 * reachable if a ring is edited without its defaults) restarts at the first entry. */
static uint8_t ring_next(const uint8_t *ring, int n, uint8_t cur)
{
	for (int i = 0; i < n; i++) {
		if (ring[i] == cur) {
			return ring[(i + 1) % n];
		}
	}
	return ring[0];
}

void sp1_mui_t_dest_step(int t)
{
	if (t >= 0 && t < 3) {
		route.t_dest[t] = ring_next(t_ring, T_RING_N, route.t_dest[t]);
	}
}

void sp1_mui_dest_step(int out)
{
	if (out >= 0 && out < 4) {
		route.dest[out] = ring_next(x_ring, X_RING_N, route.dest[out]);
	}
}

void sp1_mui_deja_vu_toggle(int side)
{
	if (side == 0) {
		dv_t = !dv_t;
	} else {
		dv_x = !dv_x;
	}
}

bool sp1_mui_deja_vu(int side) { return side == 0 ? dv_t : dv_x; }

int sp1_mui_unpatch_dest(uint8_t dest)
{
	int n = 0;
	if (dest == SP1_DEST_NONE) {
		return 0;               /* "clear everything aimed at nothing" clears nothing */
	}
	for (int k = 0; k < 3; k++) {
		if (route.t_dest[k] == dest) {
			route.t_dest[k] = SP1_DEST_NONE;
			n++;
		}
	}
	for (int k = 0; k < 4; k++) {
		if (route.dest[k] == dest) {
			route.dest[k] = SP1_DEST_NONE;
			n++;
		}
	}
	return n;
}

int sp1_mui_unpatch_voct(void)
{
	/* ⚠️ X / Y only, deliberately: V/Oct is not in the t ring at all (a gate is not a
	 * pitch), so walking the t outputs here would be dead code pretending to be a
	 * safety net. The M4b interlock calls this. */
	int n = 0;
	for (int k = 0; k < 4; k++) {
		if (route.dest[k] == SP1_DEST_VOCT) {
			route.dest[k] = SP1_DEST_NONE;
			n++;
		}
	}
	return n;
}

/* ---- UNPATCH, one output at a time ("••" + hold Tn, M4e) ----
 * The Marbles side of the gesture: the button names an OUTPUT and its destination goes
 * to `none`. Returns the destination that WAS there, so main can name it in the log --
 * SP1_DEST_NONE meaning there was nothing patched, which is not an error, just a no-op
 * with an animation in front of it. */
uint8_t sp1_mui_unpatch_t(int t)
{
	if (t < 0 || t >= 3) {
		return SP1_DEST_NONE;
	}
	const uint8_t was = route.t_dest[t];
	route.t_dest[t] = SP1_DEST_NONE;
	return was;
}

uint8_t sp1_mui_unpatch_x(int out)
{
	if (out < 0 || out >= 4) {
		return SP1_DEST_NONE;
	}
	const uint8_t was = route.dest[out];
	route.dest[out] = SP1_DEST_NONE;
	return was;
}

bool sp1_mui_scale_included(int s)
{
	return s >= 0 && s < SP1_MUI_SCALES && scale_on[s];
}

void sp1_mui_scale_glyph(int s, uint8_t lv[4])
{
	const uint8_t bits = (s >= 0 && s < SP1_MUI_SCALES) ? scale_glyph[s] : 0x0u;
	for (int i = 0; i < 4; i++) {
		lv[i] = (bits & (uint8_t)(8u >> i)) ? SP1_ENGINE_LED_FULL : 0u;
	}
}

bool sp1_mui_scale_step(int dir)
{
	int s = scale;
	for (;;) {
		s += dir > 0 ? 1 : -1;
		if (s < 0 || s >= SP1_MUI_SCALES) {
			return false;            /* no wrap, like [B] (Adara) */
		}
		if (scale_on[s]) {
			scale = s;
			return true;
		}
	}
}

/* ---- outputs ---------------------------------------------------------------------- */
void sp1_mui_params(struct sp1_marbles_params *p)
{
	const float *tb = stored[SP1_MUI_T_BASE];
	const float *ts = stored[SP1_MUI_T_SHIFT];
	const float *xb = stored[SP1_MUI_X_BASE];
	const float *y  = stored[SP1_MUI_Y];
	/* X SHIFT holds only LENGTH now, and LENGTH is mirrored into the t SHIFT layer. */

	p->rate = (clamp01(tb[0]) - 0.5f) * 120.0f;   /* Marbles: 120 BPM at centre, +-5 oct */
	p->t_range = t_range;
	p->t_model = model;
	p->t_bias = detent(tb[1], DETENT_BIPOLAR);
	p->t_jitter = clamp01(tb[2]);
	/* ONE knob (t BASE F4, shared with the X page), gated by [F] and [G]. Off means
	 * that side ignores it and runs free, which is Marbles' own behaviour. */
	const float deja_vu = detent(tb[3], DETENT_BIPOLAR);   /* centre = locked loop */
	p->t_deja_vu = dv_t ? deja_vu : 0.0f;
	p->gate_length = clamp01(ts[1]);
	p->gate_length_rand = clamp01(ts[2]);
	p->length = loop_length[zone(ts[3], LOOP_STEPS, 0.25f, &q_length)];

	p->x_spread = clamp01(xb[0]);
	p->x_bias = detent(xb[1], DETENT_BIPOLAR);
	p->x_steps = detent(xb[2], DETENT_BIPOLAR);    /* centre = raw, unquantized */
	p->x_deja_vu = dv_x ? deja_vu : 0.0f;
	p->x_diversity = diversity;
	p->x_range = range;
	/* For INTELLIGENT (M4c). engine_centre is filled by main.c, which is the only place
	 * that knows both modules. */
	for (int k = 0; k < 4; k++) {
		p->dest[k] = route.dest[k];
	}
	p->engine_centre = 0u;
	p->x_scale = scale;
	/* X keeps Marbles' own clocking -- X1 on t1, X2 on t2, X3 on t3 -- with no control
	 * to change it (Adara, M4a: X SHIFT F2 unbound, X's clock source left alone). */
	p->x_clock = SP1_MRB_XCLK_EACH;

	p->y_spread = clamp01(y[0]);
	p->y_bias = detent(y[1], DETENT_BIPOLAR);
	p->y_steps = detent(y[2], DETENT_BIPOLAR);
	p->y_divider = zone(y[3], 12, 0.1f, &q_ydiv);
	p->y_range = range;   /* one [J] (M4e) */
}

void sp1_mui_routing(struct sp1_mui_routing *out)
{
	*out = route;
}

float sp1_mui_bpm(void)
{
	return sp1_marbles_bpm((clamp01(stored[SP1_MUI_T_BASE][0]) - 0.5f) * 120.0f, t_range);
}

static uint8_t volt_led(float v, int range)
{
	/* INTELLIGENT resolves per output, so there is no one full scale to show against:
	 * use 5 V, the widest any of its choices reaches. */
	const float full = range == SP1_MRB_RANGE_NARROW ? 2.0f : 5.0f;
	const float x = clamp01(fabsf(v) / full);
	return (uint8_t)(x * 255.0f + 0.5f);
}

static void leds_of(enum sp1_mui_layer l, uint8_t out[4])
{
	const bool base = l == SP1_MUI_T_BASE || l == SP1_MUI_X_BASE;
	if (base && show_ms == 0u) {
		/* Marbles' own output LEDs; T4 = Y on both pages. */
		if (page == SP1_MUI_PAGE_T) {
			const uint8_t g = sp1_marbles_last_gates();
			for (int i = 0; i < 3; i++) {
				out[i] = (g & (1u << i)) ? 255u : 0u;
			}
		} else {
			for (int i = 0; i < 3; i++) {
				out[i] = volt_led(sp1_marbles_last_volts(i), range);
			}
		}
		out[3] = volt_led(sp1_marbles_last_volts(3), range);
		return;
	}

	uint32_t k = 256u;
	if (l == SP1_MUI_Y) {
		const uint32_t half = BREATH_MS / 2u;
		const uint32_t tri = breath_ms < half ? breath_ms : BREATH_MS - breath_ms;
		k = 90u + (166u * tri) / half;      /* the PLAITS SETTINGS breathing */
	}
	for (int i = 0; i < 4; i++) {
		float x;
		if (is_reserved(l, i)) {
			x = 0.0f;
		} else if (is_bipolar(l, i)) {
			x = 2.0f * fabsf(detent(VAL(l, i), DETENT_BIPOLAR) - 0.5f);
		} else {
			x = clamp01(VAL(l, i));
		}
		const uint32_t v = (uint32_t)(x * 255.0f + 0.5f);
		out[i] = (uint8_t)((v * k) >> 8);
	}
}

void sp1_mui_leds(uint8_t out[4]) { leds_of(active, out); }
void sp1_mui_page_leds(uint8_t out[4])
{
	leds_of(settings ? SP1_MUI_Y : base_of(page), out);
}

/* ---- flash patterns ---------------------------------------------------------------- */
#define F SP1_ENGINE_LED_FULL

static void set4(uint8_t o[4], uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	o[0] = a; o[1] = b; o[2] = c; o[3] = d;
}

void sp1_mui_page_pattern(uint8_t o[4])
{
	if (page == SP1_MUI_PAGE_X) {
		set4(o, 0, 0, F, F);
	} else {
		set4(o, F, F, 0, 0);
	}
}

/* A 4-bit glyph, T1 = bit 3 (the ● nearest the model row's left end). */
static void glyph4(uint8_t o[4], uint8_t bits)
{
	for (int i = 0; i < 4; i++) {
		o[i] = (bits & (uint8_t)(8u >> i)) ? F : 0u;
	}
}

void sp1_mui_dest_pattern(int out, uint8_t o[4])
{
	/* X / Y: one LED under the Plaits fader of that parameter -- F1 FM (the
	 * FREQUENCY fader's attenuverter) ... F4 HARMONICS -- plus ○●●● V/Oct and
	 * ○○●● LEVEL (Adara, M4 / M4a). */
	const int d = (out >= 0 && out < 4) ? route.dest[out] : SP1_DEST_NONE;
	switch (d) {
	case SP1_DEST_FM:     glyph4(o, 0x8u); break;   /* ●○○○ */
	case SP1_DEST_TIMBRE: glyph4(o, 0x4u); break;   /* ○●○○ */
	case SP1_DEST_MORPH:  glyph4(o, 0x2u); break;   /* ○○●○ */
	case SP1_DEST_HARM:   glyph4(o, 0x1u); break;   /* ○○○● */
	case SP1_DEST_VOCT:   glyph4(o, 0x7u); break;   /* ○●●● */
	case SP1_DEST_LEVEL:  glyph4(o, 0x3u); break;   /* ○○●● */
	default:              glyph4(o, 0x0u); break;   /* ○○○○ none */
	}
}

void sp1_mui_t_dest_pattern(int t, uint8_t o[4])
{
	/* ONE glyph vocabulary for both sides (Adara's correction to docs/UI-PAGES.md,
	 * M4b): the four CV destinations sit under the Plaits fader they modulate, on the
	 * t page exactly as on the X page. The counting bar M4a used on t is gone.
	 * ⚠️ TRIG therefore shares ○●●● with the X page's V/Oct. That is intentional and
	 * documented: the two never appear on the same page, and the page you are on tells
	 * you which it is. */
	const int d = (t >= 0 && t < 3) ? route.t_dest[t] : SP1_DEST_NONE;
	switch (d) {
	case SP1_DEST_FM:     glyph4(o, 0x8u); break;   /* ●○○○ */
	case SP1_DEST_TIMBRE: glyph4(o, 0x4u); break;   /* ○●○○ */
	case SP1_DEST_MORPH:  glyph4(o, 0x2u); break;   /* ○○●○ */
	case SP1_DEST_HARM:   glyph4(o, 0x1u); break;   /* ○○○● */
	case SP1_DEST_TRIG:   glyph4(o, 0x7u); break;   /* ○●●● */
	case SP1_DEST_LEVEL:  glyph4(o, 0x3u); break;   /* ○○●● */
	default:              glyph4(o, 0x0u); break;   /* ○○○○ none */
	}
}

void sp1_mui_model_pattern(uint8_t o[4])
{
	glyph4(o, model_glyph[model >= 0 && model < SP1_MUI_MODELS ? model : 0]);
}

void sp1_mui_scale_pattern(uint8_t o[4])
{
	glyph4(o, scale_glyph[scale >= 0 && scale < SP1_MUI_SCALES ? scale : 0]);
}

void sp1_mui_deja_vu_pattern(uint8_t o[4])
{
	/* The page vocabulary: ●●○○ is the t page, ○○●● is the X page. */
	glyph4(o, (uint8_t)((dv_t ? 0xCu : 0u) | (dv_x ? 0x3u : 0u)));
}

void sp1_mui_range_pattern(int r, uint8_t o[4])
{
	/* The three fixed ranges draw their own SIZE: 0-2 V one LED, 0-5 V three, +-5 V all
	 * four. INTELLIGENT has no size, so it gets a glyph of its own (Adara: ●○●○). */
	if (r == SP1_MRB_RANGE_NARROW) {
		set4(o, F, 0, 0, 0);
	} else if (r == SP1_MRB_RANGE_POSITIVE) {
		set4(o, F, F, F, 0);
	} else if (r == SP1_MRB_RANGE_INTELLIGENT) {
		set4(o, F, 0, F, 0);
	} else {
		set4(o, F, F, F, F);
	}
}

#undef F

/* ---- state ------------------------------------------------------------------------ */
enum sp1_mui_page  sp1_mui_page(void)   { return page; }
enum sp1_mui_layer sp1_mui_active(void) { return active; }
bool sp1_mui_settings(void)             { return settings; }
int  sp1_mui_model(void)                { return model; }
int  sp1_mui_t_range(void)              { return t_range; }
int  sp1_mui_range(void)                { return range; }
int  sp1_mui_diversity(void)            { return diversity; }
int  sp1_mui_scale(void)                { return scale; }

const char *sp1_mui_layer_name(enum sp1_mui_layer l)
{
	switch (l) {
	case SP1_MUI_T_BASE:  return "t";
	case SP1_MUI_T_SHIFT: return "t shift";
	case SP1_MUI_X_BASE:  return "X";
	case SP1_MUI_X_SHIFT: return "X shift";
	case SP1_MUI_Y:       return "settings (Y)";
	default:              return "?";
	}
}

const char *sp1_mui_dest_name(int d)
{
	switch (d) {
	case SP1_DEST_FM:     return "FM";
	case SP1_DEST_VOCT:   return "V/Oct";
	case SP1_DEST_TIMBRE: return "TIMBRE";
	case SP1_DEST_MORPH:  return "MORPH";
	case SP1_DEST_HARM:   return "HARMONICS";
	case SP1_DEST_LEVEL:  return "LEVEL";
	case SP1_DEST_TRIG:   return "TRIG";
	default:              return "none";
	}
}
