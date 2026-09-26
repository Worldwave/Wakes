/* wakes-sp1 M4a: host checks of the two UI layers (pure C, no Zephyr). */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "sp1_marbles_ui.h"
#include "sp1_plaits_ui.h"
#include "sp1_release_guard.h"

/* ---- stubs for the parts that live in the audio thread ---- */
uint8_t sp1_marbles_last_gates(void) { return 0u; }
float sp1_marbles_last_volts(int k) { (void)k; return 0.0f; }
float sp1_marbles_bpm(float rate, int r) { (void)r; return 120.0f * powf(2.0f, rate / 12.0f); }
const char *sp1_marbles_model_name(int m)
{
	static const char *const n[6] = { "coin toss", "clusters", "drums",
					  "independent", "divider", "three states" };
	return (m >= 0 && m < 6) ? n[m] : "?";
}
/* The real quantizer lives in sp1_marbles.cc and is exercised by routetest; here it is
 * stubbed as "no scale loaded", which is the identity. */
void  sp1_marbles_plaits_scale(int s) { (void)s; }
float sp1_marbles_plaits_quantize(float st) { return st; }
int   sp1_marbles_plaits_degrees(float *st, int max) { (void)st; (void)max; return 0; }
const char *sp1_marbles_scale_name(int s)
{
	static const char *const n[7] = { "major", "minor", "pentatonic", "pelog",
					  "bhairav", "shri", "free slot" };
	return (s >= 0 && s < 7) ? n[s] : "?";
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); \
	printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const char *glyph(const uint8_t lv[4])
{
	static char s[16];
	s[0] = 0;
	for (int i = 0; i < 4; i++) {
		strcat(s, lv[i] >= 200u ? "#" : (lv[i] > 0u ? "o" : "."));
	}
	return s;
}

/* ---- issue #2: main.c's Unpatch tick, reduced to its button logic ----
 * The order is main.c's: the scan gives held[] and the release edges; Unpatch counts
 * the hold and arms the guard on commit; the release handlers run; the guard is
 * disarmed at the end of the tick. `old` replays M4e's flag, cleared every tick, to show
 * this test catches the bug. Returns how many ordinary shift actions fired. */
#define RG_HOLD_TICKS 250       /* 2.0 s of 8 ms ticks: SP1_UNPATCH_HOLD_MS */
struct rg_step { int ticks; int held; };   /* held: -1 = no T button, else 0..3 */
static int rg_run(const struct rg_step *steps, int n, bool old)
{
	struct sp1_release_guard g;
	sp1_rg_init(&g);
	bool flag = false;
	int prev = -1, hold = 0, fired = 0;
	for (int k = 0; k < n; k++) {
		for (int t = 0; t < steps[k].ticks; t++) {
			const int now = steps[k].held;
			const int released = (prev >= 0 && now != prev) ? prev : -1;
			/* Unpatch: the same button held for RG_HOLD_TICKS commits once */
			hold = (now >= 0 && now == prev) ? hold + 1 : 0;
			if (now >= 0 && hold == RG_HOLD_TICKS) {
				if (old) { flag = true; } else { sp1_rg_arm(&g, now); }
			}
			/* the release handlers */
			if (released >= 0 && !(old ? flag : sp1_rg_eats(&g, released))) {
				fired++;
			}
			/* end of tick */
			if (old) {
				flag = false;
			} else if (sp1_rg_button(&g) >= 0) {
				sp1_rg_tick_end(&g, now == sp1_rg_button(&g));
			}
			prev = now;
		}
	}
	return fired;
}

static uint16_t raw_mid[4] = { 1850, 1850, 1850, 1850 };
static uint16_t raw_top[4] = { 3701, 3701, 3701, 3701 };

int main(void)
{
	uint8_t lv[4];
	struct sp1_mui_routing r;

	/* ---------- 1. MARBLES: the two destination rings ---------- */
	printf("1. destination rings\n");
	sp1_mui_init();
	sp1_mui_enter(raw_mid, false);
	sp1_mui_routing(&r);
	/* M4d: boot TRIG moved to t2, the master clock (Adara). */
	CHECK(r.t_dest[0] == SP1_DEST_NONE && r.t_dest[1] == SP1_DEST_TRIG &&
	      r.t_dest[2] == SP1_DEST_NONE, "t defaults wrong: %u %u %u",
	      r.t_dest[0], r.t_dest[1], r.t_dest[2]);
	CHECK(r.dest[0] == SP1_DEST_VOCT && r.dest[1] == SP1_DEST_NONE,
	      "X defaults wrong");

	printf("   t1: ");
	for (int i = 0; i <= 7; i++) {
		sp1_mui_routing(&r);
		sp1_mui_t_dest_pattern(0, lv);
		printf("%s[%s] ", sp1_mui_dest_name(r.t_dest[0]), glyph(lv));
		if (i < 7) { sp1_mui_t_dest_step(0); }
	}
	printf("\n");
	sp1_mui_routing(&r);
	CHECK(r.t_dest[0] == SP1_DEST_NONE, "the t ring is not 7 long");

	printf("   X1: ");
	for (int i = 0; i <= 7; i++) {
		sp1_mui_routing(&r);
		sp1_mui_dest_pattern(0, lv);
		printf("%s[%s] ", sp1_mui_dest_name(r.dest[0]), glyph(lv));
		if (i < 7) { sp1_mui_dest_step(0); }
	}
	printf("\n");
	sp1_mui_routing(&r);
	CHECK(r.dest[0] == SP1_DEST_VOCT, "X ring did not return to V/Oct after 7");

	/* Both rings must be able to reach every destination that belongs to them. */
	{
		int seen_t[SP1_DEST_COUNT] = { 0 }, seen_x[SP1_DEST_COUNT] = { 0 };
		for (int i = 0; i < 7; i++) {
			sp1_mui_routing(&r);
			seen_t[r.t_dest[1]]++;
			seen_x[r.dest[1]]++;
			sp1_mui_t_dest_step(1);
			sp1_mui_dest_step(1);
		}
		CHECK(seen_t[SP1_DEST_TRIG] && seen_t[SP1_DEST_LEVEL] && seen_t[SP1_DEST_NONE] &&
		      seen_t[SP1_DEST_FM] && seen_t[SP1_DEST_TIMBRE] && seen_t[SP1_DEST_MORPH] &&
		      seen_t[SP1_DEST_HARM], "t ring misses a destination");
		CHECK(!seen_t[SP1_DEST_VOCT], "V/Oct must not be on the t ring");
		CHECK(seen_x[SP1_DEST_VOCT] && seen_x[SP1_DEST_LEVEL] && seen_x[SP1_DEST_NONE] &&
		      seen_x[SP1_DEST_FM] && seen_x[SP1_DEST_TIMBRE] && seen_x[SP1_DEST_MORPH] &&
		      seen_x[SP1_DEST_HARM], "X ring misses a destination");
		CHECK(!seen_x[SP1_DEST_TRIG], "TRIG must not be on the X ring");
	}

	/* Glyphs must be unique within each ring, and never half-bright (Adara). */
	{
		int pat[8], n = 0;
		for (int i = 0; i < 7; i++) {
			sp1_mui_t_dest_pattern(1, lv);
			int p = 0;
			for (int k = 0; k < 4; k++) {
				CHECK(lv[k] == 0u || lv[k] == 255u, "t glyph half-bright");
				p = (p << 1) | (lv[k] ? 1 : 0);
			}
			for (int j = 0; j < n; j++) {
				CHECK(pat[j] != p, "duplicate t glyph 0x%x", p);
			}
			pat[n++] = p;
			sp1_mui_t_dest_step(1);
		}
		n = 0;
		for (int i = 0; i < 7; i++) {
			sp1_mui_dest_pattern(1, lv);
			int p = 0;
			for (int k = 0; k < 4; k++) {
				CHECK(lv[k] == 0u || lv[k] == 255u, "X glyph half-bright");
				p = (p << 1) | (lv[k] ? 1 : 0);
			}
			for (int j = 0; j < n; j++) {
				CHECK(pat[j] != p, "duplicate X glyph 0x%x", p);
			}
			pat[n++] = p;
			sp1_mui_dest_step(1);
		}
	}

	/* ---------- 2. models: one ring of six, unique glyphs ---------- */
	printf("2. models: ");
	{
		int pat[6], n = 0;
		sp1_mui_init();
		for (int i = 0; i < 6; i++) {
			sp1_mui_model_pattern(lv);
			printf("%d=%s ", sp1_mui_model() + 1, glyph(lv));
			int p = 0;
			for (int k = 0; k < 4; k++) {
				CHECK(lv[k] == 0u || lv[k] == 255u, "model glyph half-bright");
				p = (p << 1) | (lv[k] ? 1 : 0);
			}
			for (int j = 0; j < n; j++) {
				CHECK(pat[j] != p, "duplicate model glyph 0x%x", p);
			}
			pat[n++] = p;
			sp1_mui_model_tap();
		}
		printf("\n");
		CHECK(sp1_mui_model() == 0, "model ring is not 6 long");
	}

	/* ---------- 3. scales: included only, no wrap ---------- */
	printf("3. scales:  ");
	sp1_mui_init();
	CHECK(sp1_mui_scale() == 0, "scale does not start at major");
	CHECK(!sp1_mui_scale_step(-1), "scale wrapped below the first");
	for (int i = 0; i < 8; i++) {
		sp1_mui_scale_pattern(lv);
		printf("%s[%s] ", sp1_marbles_scale_name(sp1_mui_scale()), glyph(lv));
		if (!sp1_mui_scale_step(1)) {
			break;
		}
	}
	printf("\n");
	CHECK(sp1_mui_scale() == 5, "the free slot is off, so shri must be the top");
	CHECK(!sp1_mui_scale_step(1), "scale stepped past the last included one");

	/* ---------- 4. X SHIFT F1-F3 are unbound ---------- */
	printf("4. X SHIFT reserved faders\n");
	{
		struct sp1_marbles_params a, b;
		sp1_mui_init();
		sp1_mui_enter(raw_mid, true);              /* "••" held -> a SHIFT layer */
		sp1_mui_set_page(SP1_MUI_PAGE_X);
		for (int i = 0; i < 200; i++) {
			sp1_mui_tick(8u, raw_mid, true, true, false);
		}
		sp1_mui_params(&a);
		for (int i = 0; i < 400; i++) {
			sp1_mui_tick(8u, raw_top, true, true, false);
		}
		sp1_mui_params(&b);
		CHECK(a.x_scale == b.x_scale, "a fader moved the scale");
		CHECK(a.x_clock == b.x_clock && b.x_clock == SP1_MRB_XCLK_EACH,
		      "a fader moved the X clock source");
		CHECK(b.length != a.length, "X SHIFT F4 (LENGTH) stopped working");
	}

	/* ---------- 5. PLAITS: the attenuverters start at zero ---------- */
	printf("5. PLAITS layers\n");
	{
		struct sp1_synth_params p;
		sp1_pui_init();
		sp1_pui_enter(raw_top);                    /* every fader at the TOP */
		for (int i = 0; i < 10; i++) {
			sp1_pui_tick(8u, raw_top, true, false, false);
		}
		sp1_pui_params(&p);
		CHECK(fabsf(p.fm_mod) < 1e-4f && fabsf(p.timbre_mod) < 1e-4f &&
		      fabsf(p.morph_mod) < 1e-4f && fabsf(p.harm_mod) < 1e-4f,
		      "attenuverters took the fader positions: %.3f %.3f %.3f %.3f",
		      p.fm_mod, p.timbre_mod, p.morph_mod, p.harm_mod);
		CHECK(p.level_patched == 0, "LEVEL patched at boot with the faders up");
		CHECK(p.harmonics > 0.99f, "BASE did not seed from the faders");

		/* hold "••": the SHIFT layer appears, still at zero, and only catches up
		 * once a fader is MOVED while it is showing */
		for (int i = 0; i < 10; i++) {
			sp1_pui_tick(8u, raw_top, true, true, false);
		}
		sp1_pui_params(&p);
		CHECK(fabsf(p.fm_mod) < 1e-4f,
		      "SHIFT snapped to the faders on first show: %.3f", p.fm_mod);
		for (int i = 0; i < 400; i++) {
			sp1_pui_tick(8u, raw_mid, true, true, false);
		}
		sp1_pui_params(&p);
		printf("   after dragging F1 to centre while SHIFT shows: fm_mod %.3f\n",
		       p.fm_mod);
		/* Plaits' catch-up skew: from stored 0.5 with the fader at the top, half
		 * the fader's travel is applied, so a drag to centre lands near -0.45.
		 * What matters is that it MOVED and did not jump. */
		CHECK(p.fm_mod < -0.3f && p.fm_mod > -0.7f,
		      "pickup did not follow the fader down: %.3f", p.fm_mod);

		/* rip out the cables: back to zero on every attenuverter */
		sp1_pui_rip();
		sp1_pui_params(&p);
		CHECK(fabsf(p.fm_mod) < 1e-4f && fabsf(p.harm_mod) < 1e-4f,
		      "ROTC left an attenuverter at %.3f / %.3f", p.fm_mod, p.harm_mod);
		CHECK(p.level_patched == 0, "ROTC left LEVEL patched");
	}

	/* ---------- 6. PLAITS SETTINGS: range on F1, LEVEL on F4 ---------- */
	printf("6. PLAITS SETTINGS\n");
	{
		struct sp1_synth_params p;
		uint16_t f[4];
		sp1_pui_init();
		sp1_pui_enter(raw_mid);
		/* double-tap "••" to latch SETTINGS */
		sp1_pui_tick(8u, raw_mid, true, false, false);
		sp1_pui_tick(8u, raw_mid, true, true, false);
		sp1_pui_tick(8u, raw_mid, true, false, false);
		sp1_pui_tick(8u, raw_mid, true, true, false);
		sp1_pui_tick(8u, raw_mid, true, false, false);
		CHECK(sp1_pui_page() == SP1_PUI_SETTINGS, "double tap did not latch SETTINGS");
		const int oct_mid = sp1_pui_octave_mode();
		memcpy(f, raw_mid, sizeof(f));
		f[0] = 0;                                  /* F1 to the bottom */
		for (int i = 0; i < 400; i++) {
			sp1_pui_tick(8u, f, true, false, false);
		}
		CHECK(sp1_pui_octave_mode() < oct_mid,
		      "SETTINGS F1 is not the octave range (%d -> %d)",
		      oct_mid, sp1_pui_octave_mode());
		f[3] = 3701;                               /* F4 to the top = LEVEL up */
		for (int i = 0; i < 400; i++) {
			sp1_pui_tick(8u, f, true, false, false);
		}
		sp1_pui_params(&p);
		CHECK(p.level_patched == 1 && p.level > 0.9f,
		      "SETTINGS F4 is not LEVEL (patched %d level %.2f)",
		      p.level_patched, p.level);
	}

	/* ---------- 7. both rings start at NONE (Adara's M4b correction) ---------- */
	printf("7. ring order\n");
	sp1_mui_init();
	sp1_mui_routing(&r);
	{
		/* t1 starts at none, so one step forward must be TRIG */
		sp1_mui_routing(&r);
		CHECK(r.t_dest[0] == SP1_DEST_NONE, "t1 should boot disconnected");
		sp1_mui_t_dest_step(0);
		sp1_mui_routing(&r);
		CHECK(r.t_dest[0] == SP1_DEST_TRIG,
		      "one step forward from none is %s, not TRIG",
		      sp1_mui_dest_name(r.t_dest[0]));
		/* ... and six more come back to none */
		for (int i = 0; i < 6; i++) { sp1_mui_t_dest_step(0); }
		sp1_mui_routing(&r);
		CHECK(r.t_dest[0] == SP1_DEST_NONE, "the t ring does not start at none");
	}
	/* the four CV destinations share one glyph vocabulary across both sides now */
	{
		uint8_t a[4], b[4];
		sp1_mui_init();
		for (int n = 0; n < 7; n++) {
			sp1_mui_routing(&r);
			if (r.t_dest[0] == SP1_DEST_TIMBRE) { break; }
			sp1_mui_t_dest_step(0);
		}
		for (int n = 0; n < 7; n++) {
			sp1_mui_routing(&r);
			if (r.dest[0] == SP1_DEST_TIMBRE) { break; }
			sp1_mui_dest_step(0);
		}
		sp1_mui_t_dest_pattern(0, a);
		sp1_mui_dest_pattern(0, b);
		printf("   TIMBRE glyph: t %s, X %s\n", glyph(a), glyph(b));
		CHECK(memcmp(a, b, 4) == 0, "the two sides disagree on the TIMBRE glyph");
	}

	/* ---------- 8. F4 is ONE control across the two pages ---------- */
	printf("8. shared DEJA VU / LENGTH\n");
	{
		struct sp1_marbles_params a, b;
		uint16_t f[4];
		sp1_mui_init();
		sp1_mui_enter(raw_mid, false);
		sp1_mui_set_page(SP1_MUI_PAGE_T);
		for (int i = 0; i < 100; i++) {
			sp1_mui_tick(8u, raw_mid, true, false, false);
		}
		/* drag F4 up on the t page */
		memcpy(f, raw_mid, sizeof(f));
		f[3] = 3200;
		for (int i = 0; i < 400; i++) {
			sp1_mui_tick(8u, f, true, false, false);
		}
		sp1_mui_params(&a);
		/* switch to the X page and DO NOT touch the faders */
		sp1_mui_set_page(SP1_MUI_PAGE_X);
		for (int i = 0; i < 100; i++) {
			sp1_mui_tick(8u, f, true, false, false);
		}
		sp1_mui_params(&b);
		printf("   t page DEJA VU %.3f -> X page %.3f\n", a.t_deja_vu, b.x_deja_vu);
		CHECK(fabsf(a.t_deja_vu - b.x_deja_vu) < 1e-4f,
		      "DEJA VU is not shared: %.3f vs %.3f", a.t_deja_vu, b.x_deja_vu);
		CHECK(fabsf(a.t_deja_vu - b.t_deja_vu) < 1e-4f,
		      "switching page moved the t value");
		/* and the shared LENGTH, on the two SHIFT layers */
		sp1_mui_set_page(SP1_MUI_PAGE_T);
		for (int i = 0; i < 300; i++) {
			sp1_mui_tick(8u, f, true, true, false);
		}
		sp1_mui_params(&a);
		sp1_mui_set_page(SP1_MUI_PAGE_X);
		for (int i = 0; i < 100; i++) {
			sp1_mui_tick(8u, f, true, true, false);
		}
		sp1_mui_params(&b);
		CHECK(a.length == b.length, "LENGTH is not shared: %d vs %d",
		      a.length, b.length);
	}

	/* ---------- 9. [F] / [G] gate DEJA VU per side ---------- */
	printf("9. DEJA VU toggles: ");
	{
		struct sp1_marbles_params a;
		uint16_t f[4];
		sp1_mui_init();
		sp1_mui_enter(raw_mid, false);
		memcpy(f, raw_mid, sizeof(f));
		f[3] = 3200;                                /* DEJA VU well off centre */
		for (int i = 0; i < 400; i++) {
			sp1_mui_tick(8u, f, true, false, false);
		}
		sp1_mui_params(&a);
		CHECK(sp1_mui_deja_vu(0) && sp1_mui_deja_vu(1), "toggles do not start on");
		CHECK(a.t_deja_vu > 0.5f && a.x_deja_vu > 0.5f, "both sides should follow");
		sp1_mui_deja_vu_toggle(0);
		sp1_mui_params(&a);
		printf("t off -> t %.2f X %.2f; ", a.t_deja_vu, a.x_deja_vu);
		CHECK(a.t_deja_vu == 0.0f && a.x_deja_vu > 0.5f,
		      "[F] off should free only the t side");
		sp1_mui_deja_vu_toggle(1);
		sp1_mui_params(&a);
		printf("both off -> t %.2f X %.2f\n", a.t_deja_vu, a.x_deja_vu);
		CHECK(a.x_deja_vu == 0.0f, "[G] off should free the X side");
		uint8_t g[4];
		sp1_mui_deja_vu_pattern(g);
		CHECK(g[0] == 0u && g[3] == 0u, "both off should be dark");
	}

	/* ---------- 10. the V/Oct interlock, Plaits' half ---------- */
	printf("10. interlock\n");
	{
		sp1_pui_init();
		sp1_pui_enter(raw_mid);
		sp1_pui_set_scale(2);
		CHECK(sp1_pui_scale() == 2, "scale did not take");
		sp1_pui_set_octave_max();
		CHECK(sp1_pui_octave_mode() == 10, "octave range did not open to max: %d",
		      sp1_pui_octave_mode());
		sp1_pui_rip();
		CHECK(sp1_pui_scale() == SP1_PUI_SCALE_OFF, "ROTC left a scale selected");
		sp1_mui_init();
		sp1_mui_routing(&r);
		CHECK(r.dest[0] == SP1_DEST_VOCT, "X1 should default to V/Oct");
		CHECK(sp1_mui_unpatch_voct() == 1, "unpatch_voct did not clear X1");
		sp1_mui_routing(&r);
		CHECK(r.dest[0] == SP1_DEST_NONE, "X1 still routed after unpatch");
		CHECK(sp1_mui_unpatch_voct() == 0, "unpatch_voct is not idempotent");
	}

	/* ---------- 11. M4d: boot routing, and the rip as a full wipe ---------- */
	printf("11. boot routing / full-wipe rip\n");
	sp1_mui_init();
	sp1_mui_routing(&r);
	CHECK(r.t_dest[0] == SP1_DEST_NONE && r.t_dest[1] == SP1_DEST_TRIG &&
	      r.t_dest[2] == SP1_DEST_NONE,
	      "boot t routing should be none/TRIG/none, got %u/%u/%u",
	      r.t_dest[0], r.t_dest[1], r.t_dest[2]);
	CHECK(r.dest[0] == SP1_DEST_VOCT && r.dest[1] == SP1_DEST_NONE &&
	      r.dest[2] == SP1_DEST_NONE && r.dest[3] == SP1_DEST_NONE,
	      "boot X/Y routing should be V/Oct then none");
	{
		/* move things away from their defaults, then rip */
		struct sp1_marbles_params a, b;
		uint16_t f[4] = { 400, 3200, 3400, 3300 };
		sp1_mui_enter(raw_mid, false);
		sp1_mui_set_page(SP1_MUI_PAGE_X);
		for (int i = 0; i < 600; i++) {
			sp1_mui_tick(8u, f, true, false, false);
		}
		sp1_mui_t_dest_step(0);
		sp1_mui_dest_step(1);
		sp1_mui_model_tap();
		sp1_mui_scale_step(1);
		sp1_mui_deja_vu_toggle(0);
		sp1_mui_params(&a);
		CHECK(sp1_mui_model() != 0 && sp1_mui_scale() != 0 && !sp1_mui_deja_vu(0),
		      "the test did not actually move anything");

		sp1_mui_rip();
		sp1_mui_routing(&r);
		sp1_mui_params(&b);
		for (int i = 0; i < 3; i++) {
			CHECK(r.t_dest[i] == SP1_DEST_NONE, "rip left t%d routed", i + 1);
		}
		for (int i = 0; i < 4; i++) {
			CHECK(r.dest[i] == SP1_DEST_NONE, "rip left X/Y %d routed", i + 1);
		}
		CHECK(sp1_mui_model() == 0, "rip did not return the model to coin toss");
		CHECK(sp1_mui_scale() == 0, "rip did not return the scale to major");
		CHECK(sp1_mui_deja_vu(0) && sp1_mui_deja_vu(1), "rip left a DEJA VU toggle off");
		CHECK(fabsf(b.x_spread - 0.5f) < 1e-3f, "rip did not reset X SPREAD: %.3f",
		      b.x_spread);
		CHECK(fabsf(b.x_bias - 0.5f) < 1e-3f, "rip did not reset X BIAS");
		CHECK(fabsf(b.t_bias - 0.5f) < 1e-3f, "rip did not reset t BIAS");
		CHECK(b.t_jitter == 0.0f, "rip did not reset JITTER");
		printf("   marbles rip: every output disconnected, %d BPM, model 1, major\n",
		       (int)sp1_mui_bpm());
		/* and the page you were standing on is kept */
		CHECK(sp1_mui_page() == SP1_MUI_PAGE_X, "rip moved me off the X page");
	}

	/* ---------- 12. M4d: the PLAITS patch wipe ---------- */
	printf("12. PLAITS patch wipe\n");
	{
		struct sp1_synth_params p;
		sp1_pui_init();
		sp1_pui_enter(raw_top);
		for (int i = 0; i < 10; i++) {
			sp1_pui_tick(8u, raw_top, true, false, false);
		}
		/* move off the defaults: another engine, a scale, an attenuverter */
		sp1_pui_engine_step(1);
		sp1_pui_engine_step(1);
		sp1_pui_set_scale(1);
		for (int i = 0; i < 400; i++) {
			sp1_pui_tick(8u, raw_mid, true, true, false);
		}
		CHECK(sp1_pui_slot() != 0, "the test did not change the engine");

		sp1_pui_rip();
		sp1_pui_params(&p);
		CHECK(sp1_pui_slot() == 0, "rip did not return to slot 1, got %d",
		      sp1_pui_slot() + 1);
		CHECK(sp1_pui_scale() == SP1_PUI_SCALE_OFF, "rip left a scale selected");
		CHECK(fabsf(p.fm_mod) < 1e-4f && fabsf(p.harm_mod) < 1e-4f,
		      "rip left an attenuverter at %.3f / %.3f", p.fm_mod, p.harm_mod);
		CHECK(p.level_patched == 0, "rip left LEVEL patched");
		CHECK(sp1_pui_octave_mode() == 10, "rip did not open the range: %d",
		      sp1_pui_octave_mode());
		/* F1 centre in the full range is exactly C4 */
		CHECK(fabsf(p.note - 60.0f) < 1e-3f,
		      "rip should put FREQUENCY at C4 (60), got %.3f", p.note);
		/* slot 1 = virtual analog, detents F2 and F4 -> neutral 0.5 / 0 / 0.5 */
		printf("   slot 1 neutral: note %.1f timbre %.2f morph %.2f harmonics %.2f\n",
		       p.note, p.timbre, p.morph, p.harmonics);
		CHECK(fabsf(p.timbre - 0.5f) < 1e-3f, "F2 is bipolar on slot 1: expected 0.5");
		CHECK(p.morph == 0.0f, "F3 is unipolar on slot 1: expected 0");
		CHECK(fabsf(p.harmonics - 0.5f) < 1e-3f, "F4 is bipolar on slot 1: expected 0.5");
	}

	/* ---------- 13. M4c/M4e: [J], now ONE range for X and Y ---------- */
	printf("13. [J] voltage range\n");
	{
		static const char *nm[] = { "0-2V", "0-5V", "+-5V", "INTELLIGENT" };
		uint8_t lv[SP1_MRB_RANGE_COUNT][4];
		sp1_mui_init();
		/* Boot default (docs/DEFAULTS.md), and ONE control for both groups (M4e). */
		CHECK(sp1_mui_range() == SP1_MRB_RANGE_INTELLIGENT,
		      "[J] should boot INTELLIGENT, got %s", nm[sp1_mui_range()]);

		/* Steps through all four and wraps. */
		printf("   ring:");
		for (int i = 0; i < SP1_MRB_RANGE_COUNT; i++) {
			printf(" %s", nm[sp1_mui_range()]);
			sp1_mui_range_step();
		}
		printf(" -> %s\n", nm[sp1_mui_range()]);
		CHECK(sp1_mui_range() == SP1_MRB_RANGE_INTELLIGENT,
		      "[J] did not wrap after %d steps", SP1_MRB_RANGE_COUNT);

		/* ⚠️ The M4e point: X and Y are the SAME setting, and that has to be visible
		 * in what gets PUBLISHED, not just in the accessor -- sp1_marbles_params still
		 * carries two fields because Marbles' own GroupSettings does. */
		sp1_mui_range_step();                    /* off the default, to 0-2 V */
		{
			struct sp1_marbles_params mp;
			sp1_mui_params(&mp);
			printf("   published: x_range %s, y_range %s\n",
			       nm[mp.x_range], nm[mp.y_range]);
			CHECK(mp.x_range == mp.y_range,
			      "X and Y ranges must be one control (%d / %d)",
			      mp.x_range, mp.y_range);
			CHECK(mp.x_range == sp1_mui_range(),
			      "sp1_mui_params published %d, [J] says %d", mp.x_range,
			      sp1_mui_range());
		}

		/* Four glyphs, all distinct, INTELLIGENT = the dot-dot pattern (Adara). */
		printf("   glyphs:");
		for (int r = 0; r < SP1_MRB_RANGE_COUNT; r++) {
			sp1_mui_range_pattern(r, lv[r]);
			printf(" %s[", nm[r]);
			for (int k = 0; k < 4; k++) { printf("%c", lv[r][k] ? '#' : '.'); }
			printf("]");
		}
		printf("\n");
		for (int a = 0; a < SP1_MRB_RANGE_COUNT; a++) {
			for (int b = a + 1; b < SP1_MRB_RANGE_COUNT; b++) {
				CHECK(memcmp(lv[a], lv[b], 4) != 0,
				      "[J] glyphs for %s and %s are identical", nm[a], nm[b]);
			}
		}
		CHECK(lv[SP1_MRB_RANGE_INTELLIGENT][0] && !lv[SP1_MRB_RANGE_INTELLIGENT][1] &&
		      lv[SP1_MRB_RANGE_INTELLIGENT][2] && !lv[SP1_MRB_RANGE_INTELLIGENT][3],
		      "INTELLIGENT's glyph must be the dot-dot pattern Adara chose");

		/* A rip must put it back, on both groups. */
		sp1_mui_rip();
		CHECK(sp1_mui_range() == SP1_MRB_RANGE_INTELLIGENT,
		      "a rip left [J] at %s", nm[sp1_mui_range()]);

		/* The polarity table INTELLIGENT reads is the engine table's centre bits, not a
		 * second copy: slot 1 (virtual analog) has detents on F2 and F4, so 0x2 | 0x1. */
		sp1_pui_init();
		sp1_pui_rip();
		printf("   slot 1 centre bits: 0x%x (F2 TIMBRE 0x2, F4 HARMONICS 0x1)\n",
		       sp1_pui_engine_centre());
		CHECK(sp1_pui_engine_centre() == 0x3u,
		      "slot 1's centre bits should be 0x3, got 0x%x", sp1_pui_engine_centre());
	}

	/* ---------- 14. M4e: UNPATCH, one cable at a time ---------- */
	printf("14. unpatch\n");
	{
		struct sp1_mui_routing r;
		sp1_mui_init();
		/* Boot routing is t2 -> TRIG, X1 -> V/Oct (M4d). Put something on every
		 * output so each clear is visible. */
		for (int t = 0; t < 3; t++) { sp1_mui_t_dest_step(t); }   /* none -> TRIG */
		for (int x = 0; x < 4; x++) { sp1_mui_dest_step(x); }
		sp1_mui_routing(&r);
		printf("   before:");
		for (int t = 0; t < 3; t++) { printf(" t%d=%s", t + 1,
						     sp1_mui_dest_name(r.t_dest[t])); }
		for (int x = 0; x < 4; x++) { printf(" %s%s", x == 3 ? "Y=" : "X=",
						     sp1_mui_dest_name(r.dest[x])); }
		printf("\n");

		/* Each unpatch clears exactly ONE output and returns what was there. */
		for (int t = 0; t < 3; t++) {
			struct sp1_mui_routing before;
			sp1_mui_routing(&before);
			const uint8_t was = sp1_mui_unpatch_t(t);
			sp1_mui_routing(&r);
			CHECK(was == before.t_dest[t],
			      "unpatch t%d returned %s, was %s", t + 1,
			      sp1_mui_dest_name(was), sp1_mui_dest_name(before.t_dest[t]));
			CHECK(r.t_dest[t] == SP1_DEST_NONE, "t%d not cleared", t + 1);
			for (int k = 0; k < 3; k++) {
				if (k != t) {
					CHECK(r.t_dest[k] == before.t_dest[k],
					      "unpatch t%d also changed t%d", t + 1, k + 1);
				}
			}
			for (int k = 0; k < 4; k++) {
				CHECK(r.dest[k] == before.dest[k],
				      "unpatch t%d also changed X/Y %d", t + 1, k);
			}
		}
		for (int x = 0; x < 4; x++) {
			struct sp1_mui_routing before;
			sp1_mui_routing(&before);
			const uint8_t was = sp1_mui_unpatch_x(x);
			sp1_mui_routing(&r);
			CHECK(was == before.dest[x], "unpatch X/Y %d returned the wrong dest", x);
			CHECK(r.dest[x] == SP1_DEST_NONE, "X/Y %d not cleared", x);
			for (int k = 0; k < 4; k++) {
				if (k != x) {
					CHECK(r.dest[k] == before.dest[k],
					      "unpatch %d also changed %d", x, k);
				}
			}
		}
		/* Clearing something already clear is a no-op, not an error. */
		CHECK(sp1_mui_unpatch_t(0) == SP1_DEST_NONE, "unpatch of an empty t lied");
		CHECK(sp1_mui_unpatch_x(3) == SP1_DEST_NONE, "unpatch of an empty Y lied");
		/* Out of range must not touch anything. */
		CHECK(sp1_mui_unpatch_t(3) == SP1_DEST_NONE, "unpatch t3 (out of range)");
		CHECK(sp1_mui_unpatch_x(4) == SP1_DEST_NONE, "unpatch X4 (out of range)");

		/* ---- the PLAITS side: the button names a PARAMETER ---- */
		sp1_mui_init();
		/* Aim several outputs at TIMBRE, plus one at MORPH that must survive. */
		while (1) {
			struct sp1_mui_routing q;
			sp1_mui_routing(&q);
			if (q.t_dest[0] == SP1_DEST_TIMBRE) { break; }
			sp1_mui_t_dest_step(0);
		}
		while (1) {
			struct sp1_mui_routing q;
			sp1_mui_routing(&q);
			if (q.dest[1] == SP1_DEST_TIMBRE) { break; }
			sp1_mui_dest_step(1);
		}
		while (1) {
			struct sp1_mui_routing q;
			sp1_mui_routing(&q);
			if (q.dest[2] == SP1_DEST_MORPH) { break; }
			sp1_mui_dest_step(2);
		}
		int n = sp1_mui_unpatch_dest(SP1_DEST_TIMBRE);
		sp1_mui_routing(&r);
		printf("   PLAITS T2 (TIMBRE): %d route%s cleared, MORPH kept: %s\n",
		       n, n == 1 ? "" : "s", sp1_mui_dest_name(r.dest[2]));
		CHECK(n == 2, "TIMBRE had 2 routes, cleared %d", n);
		CHECK(r.t_dest[0] == SP1_DEST_NONE && r.dest[1] == SP1_DEST_NONE,
		      "a TIMBRE route survived");
		CHECK(r.dest[2] == SP1_DEST_MORPH, "unpatching TIMBRE cleared MORPH too");
		CHECK(sp1_mui_unpatch_dest(SP1_DEST_NONE) == 0,
		      "unpatching `none` must clear nothing -- every silent output matches it");

		/* ⚠️ T1 on PLAITS is FREQUENCY, which is V/Oct AND FM: both modulate the pitch
		 * and V/Oct has no button of its own. */
		sp1_mui_init();
		while (1) {
			struct sp1_mui_routing q;
			sp1_mui_routing(&q);
			if (q.dest[0] == SP1_DEST_VOCT) { break; }
			sp1_mui_dest_step(0);
		}
		while (1) {
			struct sp1_mui_routing q;
			sp1_mui_routing(&q);
			if (q.dest[1] == SP1_DEST_FM) { break; }
			sp1_mui_dest_step(1);
		}
		n = sp1_mui_unpatch_dest(SP1_DEST_VOCT) + sp1_mui_unpatch_dest(SP1_DEST_FM);
		sp1_mui_routing(&r);
		printf("   PLAITS T1 (FREQUENCY): %d route%s cleared (V/Oct + FM)\n",
		       n, n == 1 ? "" : "s");
		CHECK(n == 2, "FREQUENCY should clear both V/Oct and FM, cleared %d", n);
		CHECK(r.dest[0] == SP1_DEST_NONE && r.dest[1] == SP1_DEST_NONE,
		      "a pitch route survived T1");
	}

	/* ---------- 15. M4e: a one-tick fader jump is a bad sample, not a hand ---------- */
	printf("15. pickup jump rejection\n");
	{
		/* The M4e attenuverter bug in miniature: seed the UI from a ZERO reading (what
		 * sp1_fader_raw returns after STANDBY, before any scan), then let the faders
		 * read their real position. Through M4c that excursion was applied to every
		 * catching layer as movement and the attenuverters came up on the faders. */
		struct sp1_synth_params p;
		static const uint16_t raw_zero[4] = { 0u, 0u, 0u, 0u };
		sp1_pui_init();
		sp1_pui_enter(raw_zero);
		for (int i = 0; i < 200; i++) {
			sp1_pui_tick(8u, raw_top, true, false, false);
		}
		sp1_pui_params(&p);
		printf("   after a 0 -> full step on every fader: fm_mod %.3f timbre_mod %.3f "
		       "morph_mod %.3f harm_mod %.3f\n",
		       p.fm_mod, p.timbre_mod, p.morph_mod, p.harm_mod);
		CHECK(fabsf(p.fm_mod) < 0.02f && fabsf(p.timbre_mod) < 0.02f &&
		      fabsf(p.morph_mod) < 0.02f && fabsf(p.harm_mod) < 0.02f,
		      "a sampling artefact moved the attenuverters off zero");
		/* And a REAL movement must still be picked up -- the deadband must not have
		 * turned pickup off. Walk a fader up in ordinary steps. */
		{
			uint16_t raw[4];
			for (int i = 0; i < 4; i++) { raw[i] = raw_top[i]; }
			for (int i = 0; i < 400; i++) {
				/* hold "••" so SHIFT is the active layer */
				raw[0] = (uint16_t)(3700u - (uint32_t)i * 9u > 0u
						    ? 3700u - (uint32_t)i * 9u : 0u);
				sp1_pui_tick(8u, raw, true, true, false);
			}
			sp1_pui_params(&p);
			printf("   then a real sweep of F1 under SHIFT: fm_mod %.3f\n", p.fm_mod);
			CHECK(fabsf(p.fm_mod) > 0.1f,
			      "the deadband also blocked a real fader movement (%.3f)", p.fm_mod);
		}
	}

	/* ---------- 16. Unpatch eats its own release (issue #2) ---------- */
	printf("16. unpatch release\n");
	{
		/* Held well past the commit, then let go: the hardware report. */
		const struct rg_step past[] = { { 1, -1 }, { RG_HOLD_TICKS + 60, 0 }, { 5, -1 } };
		const int old_fired = rg_run(past, 3, true);
		const int new_fired = rg_run(past, 3, false);
		printf("   held 0.5 s past the commit, then released: M4e fired %d, now %d\n",
		       old_fired, new_fired);
		CHECK(old_fired == 1, "the model does not reproduce issue #2 (%d)", old_fired);
		CHECK(new_fired == 0, "the release after an Unpatch still fired (%d)", new_fired);

		/* Released on the very next tick after the commit. */
		const struct rg_step quick[] = { { 1, -1 }, { RG_HOLD_TICKS + 1, 2 }, { 5, -1 } };
		CHECK(rg_run(quick, 3, false) == 0, "a release one tick after the commit fired");

		/* The guard must not leak: the NEXT short press of the same button is its own. */
		const struct rg_step again[] = { { 1, -1 }, { RG_HOLD_TICKS + 30, 1 }, { 5, -1 },
						 { 20, 1 }, { 5, -1 } };
		CHECK(rg_run(again, 5, false) == 1,
		      "the press after an Unpatch was eaten too (guard leaked)");

		/* A short press never reaches Unpatch and always fires on release. */
		const struct rg_step tap[] = { { 1, -1 }, { 40, 3 }, { 5, -1 } };
		CHECK(rg_run(tap, 3, false) == 1, "a short shift press did not fire");

		/* Released one tick BEFORE the commit: cancelled, so its release is ordinary. */
		const struct rg_step early[] = { { 1, -1 }, { RG_HOLD_TICKS, 0 }, { 5, -1 } };
		CHECK(rg_run(early, 3, false) == 1, "a cancelled Unpatch ate the release");

		/* Only the armed button: another one's release is its own. */
		struct sp1_release_guard g;
		sp1_rg_init(&g);
		sp1_rg_arm(&g, 1);
		CHECK(sp1_rg_eats(&g, 1) && !sp1_rg_eats(&g, 0) && !sp1_rg_eats(&g, 3),
		      "the guard ate a button it was not armed for");
		sp1_rg_tick_end(&g, true);
		CHECK(sp1_rg_eats(&g, 1), "the guard dropped while its button was still held");
		sp1_rg_tick_end(&g, false);
		CHECK(!sp1_rg_eats(&g, 1), "the guard outlived its button's release");
	}

	printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all checks passed",
	       fails, fails == 1 ? "" : "s");
	return fails != 0;
}
