/* wakes-sp1 M6 (#50): host checks of the PRST slot format and its field check.
 * Links the real sp1_prst.c + both UI files; the audio-thread parts are stubbed as in uitest. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sp1_prst.h"

/* ---- stubs for the parts that live in the audio thread (as uitest.c) ---- */
uint8_t sp1_marbles_last_gates(void) { return 0u; }
float sp1_marbles_last_volts(int k) { (void)k; return 0.0f; }
float sp1_marbles_bpm(float rate, int r) { (void)r; return 120.0f * powf(2.0f, rate / 12.0f); }
const char *sp1_marbles_model_name(int m) { (void)m; return "?"; }
void  sp1_marbles_plaits_scale(int s) { (void)s; }
float sp1_marbles_plaits_quantize(float st) { return st; }
int   sp1_marbles_plaits_degrees(float *st, int max) { (void)st; (void)max; return 0; }
const char *sp1_marbles_scale_name(int s) { (void)s; return "?"; }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); \
	printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint16_t raw_mid[4] = { 1850, 1850, 1850, 1850 };
static char text[SP1_PRST_TEXT_MAX];

/* Marbles slots a file does not carry: X BASE F4 and X SHIFT F4 are the t page's (canon()),
 * X SHIFT F1-F3 are reserved. They come back as their defaults whatever they were. */
static bool carried_m(int l, int i)
{
	return !((l == SP1_MUI_X_BASE && i == 3) || l == SP1_MUI_X_SHIFT);
}

/* Field-by-field difference, positions within half the file's last digit. Returns the
 * number of differences and prints the first few. */
static int diff(const struct sp1_prst *a, const struct sp1_prst *b, const char *what)
{
	int n = 0;
#define D(cond, ...) do { if (cond) { if (n < 4) { printf("    %s: ", what); \
	printf(__VA_ARGS__); printf("\n"); } n++; } } while (0)
	D(a->plaits.slot != b->plaits.slot, "engine slot %d vs %d", a->plaits.slot, b->plaits.slot);
	for (int l = 0; l < SP1_PUI_LAYERS; l++) {
		for (int i = 0; i < 4; i++) {
			D(fabsf(a->plaits.v[l][i] - b->plaits.v[l][i]) > 0.00005f,
			  "plaits v[%d][%d] %.5f vs %.5f", l, i, a->plaits.v[l][i], b->plaits.v[l][i]);
		}
	}
	D(a->plaits.scale != b->plaits.scale, "quantizer %d vs %d", a->plaits.scale, b->plaits.scale);
	D(a->out_mode != b->out_mode, "output %d vs %d", a->out_mode, b->out_mode);
	D(a->burst_div != b->burst_div, "burst %d vs %d", a->burst_div, b->burst_div);
	D(a->drive != b->drive, "drive %d vs %d", a->drive, b->drive);
	for (int l = 0; l < SP1_MUI_LAYERS; l++) {
		for (int i = 0; i < 4; i++) {
			if (carried_m(l, i)) {
				D(fabsf(a->marbles.v[l][i] - b->marbles.v[l][i]) > 0.00005f,
				  "marbles v[%d][%d] %.5f vs %.5f", l, i, a->marbles.v[l][i],
				  b->marbles.v[l][i]);
			}
		}
	}
	const struct sp1_mui_patch *x = &a->marbles, *y = &b->marbles;
	D(x->model != y->model, "t model %d vs %d", x->model, y->model);
	D(x->t_range != y->t_range, "t range %d vs %d", x->t_range, y->t_range);
	D(x->range != y->range, "[J] %d vs %d", x->range, y->range);
	D(x->diversity != y->diversity, "diversity %d vs %d", x->diversity, y->diversity);
	D(x->scale != y->scale, "scale %d vs %d", x->scale, y->scale);
	D(x->dv_t != y->dv_t || x->dv_x != y->dv_x, "deja vu toggles");
	for (int k = 0; k < 3; k++) {
		D(x->t_dest[k] != y->t_dest[k], "t%d %d vs %d", k + 1, x->t_dest[k], y->t_dest[k]);
	}
	for (int k = 0; k < 4; k++) {
		D(x->dest[k] != y->dest[k], "x%d %d vs %d", k + 1, x->dest[k], y->dest[k]);
	}
#undef D
	return n;
}

/* A patch nothing like the defaults: every field off its default value. */
static void unusual(struct sp1_prst *p)
{
	sp1_prst_rip(p, -1);
	p->plaits.slot = 6;                                     /* chords */
	for (int l = 0; l < SP1_PUI_LAYERS; l++) {
		for (int i = 0; i < 4; i++) {
			p->plaits.v[l][i] = 0.1234f + 0.05f * (float)(l * 4 + i);
		}
	}
	p->plaits.v[SP1_PUI_BASE][0] = 1.0f;                    /* both ends survive */
	p->plaits.v[SP1_PUI_BASE][1] = 0.0f;
	p->plaits.scale = 3;                                    /* pelog */
	p->out_mode = SP1_OUT_RING;
	p->burst_div = 7;
	p->drive = 4;
	for (int l = 0; l < SP1_MUI_LAYERS; l++) {
		for (int i = 0; i < 4; i++) {
			if (carried_m(l, i)) {
				p->marbles.v[l][i] = 0.9876f - 0.04f * (float)(l * 4 + i);
			}
		}
	}
	p->marbles.model = 5;
	p->marbles.t_range = 2;
	p->marbles.range = SP1_MRB_RANGE_FULL;
	p->marbles.diversity = 2;
	p->marbles.scale = 5;
	p->marbles.dv_t = 0;
	p->marbles.dv_x = 1;
	p->marbles.t_dest[0] = SP1_DEST_HARM;
	p->marbles.t_dest[1] = SP1_DEST_LEVEL;
	p->marbles.t_dest[2] = SP1_DEST_TRIG;
	p->marbles.dest[0] = SP1_DEST_FM;
	p->marbles.dest[1] = SP1_DEST_TIMBRE;
	p->marbles.dest[2] = SP1_DEST_MORPH;
	p->marbles.dest[3] = SP1_DEST_LEVEL;   /* not V/Oct: the quantizer is on (M4b) */
}

/* Everything a parse returns must be safe to hand to the UIs: every value finite and in
 * its range, every choice one the firmware can hold. */
static bool safe(const struct sp1_prst *p)
{
	for (int l = 0; l < SP1_PUI_LAYERS; l++) {
		for (int i = 0; i < 4; i++) {
			const float v = p->plaits.v[l][i];
			if (!(v >= 0.0f && v <= 1.0f)) { return false; }
		}
	}
	for (int l = 0; l < SP1_MUI_LAYERS; l++) {
		for (int i = 0; i < 4; i++) {
			const float v = p->marbles.v[l][i];
			if (!(v >= 0.0f && v <= 1.0f)) { return false; }
		}
	}
	if (strcmp(sp1_pui_slot_name(p->plaits.slot), "") == 0) { return false; }
	if (p->plaits.scale != SP1_PUI_SCALE_OFF && !sp1_mui_scale_included(p->plaits.scale)) {
		return false;
	}
	if (p->out_mode < 0 || p->out_mode >= SP1_OUT_COUNT) { return false; }
	if (p->burst_div < 0 || p->burst_div > 7) { return false; }
	if (p->drive < 0 || p->drive >= SP1_DRIVE_STEPS) { return false; }
	const struct sp1_mui_patch *m = &p->marbles;
	if (m->model < 0 || m->model > 5 || m->t_range < 0 || m->t_range > 2) { return false; }
	if (m->range < 0 || m->range >= SP1_MRB_RANGE_COUNT) { return false; }
	if (m->diversity < 0 || m->diversity > 2 || !sp1_mui_scale_included(m->scale)) {
		return false;
	}
	if ((m->dv_t != 0 && m->dv_t != 1) || (m->dv_x != 0 && m->dv_x != 1)) { return false; }
	for (int k = 0; k < 3; k++) {
		if (!sp1_mui_dest_allowed(true, m->t_dest[k])) { return false; }
	}
	for (int k = 0; k < 4; k++) {
		if (!sp1_mui_dest_allowed(false, m->dest[k])) { return false; }
		/* the V/Oct interlock (M4b): never a quantizer AND a V/Oct route */
		if (m->dest[k] == SP1_DEST_VOCT && p->plaits.scale != SP1_PUI_SCALE_OFF) {
			return false;
		}
	}
	return true;
}

/* `base` with the line for `key` replaced by "key = value" (value NULL: the line removed). */
static int with(const char *base, const char *key, const char *value, char *out, size_t size)
{
	size_t at = 0;
	const char *p = base;
	const size_t kl = strlen(key);
	while (*p) {
		const char *e = strchr(p, '\n');
		const size_t n = e ? (size_t)(e - p + 1) : strlen(p);
		if (strncmp(p, key, kl) == 0 && p[kl] == ' ' && p[kl + 1] == '=') {
			if (value) {
				at += (size_t)snprintf(out + at, size - at, "%s = %s\n", key, value);
			}
		} else {
			memcpy(out + at, p, n);
			at += n;
		}
		p += n;
	}
	out[at] = '\0';
	return (int)at;
}

int main(void)
{
	struct sp1_prst a, b, d;
	struct sp1_prst_report rep;

	/* ---------- 1. the defaults: one source, and the rip still gives them ---------- */
	printf("1. ROTC defaults\n");
	sp1_pui_init();
	sp1_mui_init();
	sp1_pui_enter(raw_mid);
	sp1_mui_enter(raw_mid, false);
	sp1_pui_rip();
	sp1_mui_rip();
	sp1_prst_rip(&d, -1);
	a = d;
	sp1_pui_get(&a.plaits);
	sp1_mui_get(&a.marbles);
	CHECK(diff(&a, &d, "rip vs sp1_prst_rip") == 0, "the rip and the PRST defaults differ");
	/* docs/DEFAULTS.md, spot checks: slot 1 = virtual analog, TIMBRE and HARMONICS bipolar */
	CHECK(d.plaits.slot == 0 && strcmp(sp1_pui_slot_name(0), "virtual analog") == 0,
	      "slot 1 is not virtual analog");
	CHECK(d.plaits.v[SP1_PUI_BASE][0] == 0.5f && d.plaits.v[SP1_PUI_BASE][1] == 0.5f &&
	      d.plaits.v[SP1_PUI_BASE][2] == 0.0f && d.plaits.v[SP1_PUI_BASE][3] == 0.5f,
	      "virtual analog's neutral BASE is wrong");
	/* #50: t2 -> TRIG and X2 -> V/Oct, every other cable out */
	CHECK(d.marbles.t_dest[0] == SP1_DEST_NONE && d.marbles.t_dest[1] == SP1_DEST_TRIG &&
	      d.marbles.t_dest[2] == SP1_DEST_NONE && d.marbles.dest[0] == SP1_DEST_NONE &&
	      d.marbles.dest[1] == SP1_DEST_VOCT && d.marbles.dest[2] == SP1_DEST_NONE &&
	      d.marbles.dest[3] == SP1_DEST_NONE, "the ROTC routing is not t2 TRIG + X2 V/Oct");
	CHECK(d.out_mode == SP1_OUT_MAIN && d.burst_div == 5 && d.drive == 0,
	      "main's defaults wrong");
	/* boot is NOT the rip: t2 -> TRIG and X1 -> V/Oct (M4d) */
	sp1_mui_init();
	sp1_mui_get(&b.marbles);
	CHECK(b.marbles.t_dest[1] == SP1_DEST_TRIG && b.marbles.dest[0] == SP1_DEST_VOCT,
	      "boot routing lost");
	/* a different engine's neutral: chords has no bipolar parameter */
	sp1_prst_rip(&b, 6);
	CHECK(b.plaits.slot == 6 && b.plaits.v[SP1_PUI_BASE][1] == 0.0f &&
	      b.plaits.v[SP1_PUI_BASE][3] == 0.0f, "chords' neutral BASE is wrong");
	sp1_prst_rip(&b, 23);    /* an empty slot (config/engines.csv) -> slot 1 */
	CHECK(b.plaits.slot == 0, "an empty slot was accepted");

	/* ---------- 2. write -> read gives the patch back ---------- */
	printf("2. round trip\n");
	int n = sp1_prst_write(&d, text, sizeof(text));
	CHECK(n > 0, "the defaults do not fit");
	printf("   the default patch, %d bytes:\n", n);
	for (const char *p = text; *p;) {
		const char *e = strchr(p, '\n');
		printf("     | %.*s\n", (int)(e ? e - p : (long)strlen(p)), p);
		p = e ? e + 1 : p + strlen(p);
	}
	sp1_prst_parse(text, (size_t)n, &a, &rep);
	CHECK(rep.format_ok && rep.defaulted == 0 && rep.bad == 0,
	      "defaults: format_ok %d defaulted %d bad %d", rep.format_ok, rep.defaulted, rep.bad);
	CHECK(diff(&a, &d, "defaults") == 0, "the defaults do not survive a round trip");

	unusual(&b);
	n = sp1_prst_write(&b, text, sizeof(text));
	sp1_prst_parse(text, (size_t)n, &a, &rep);
	CHECK(rep.format_ok && rep.defaulted == 0, "unusual: defaulted %d (first bad %s)",
	      rep.defaulted, rep.first_bad ? rep.first_bad : "-");
	CHECK(diff(&a, &b, "unusual") == 0, "an unusual patch does not survive a round trip");
	/* ...and through the UIs: put, get, write, read */
	sp1_pui_put(&b.plaits);
	sp1_mui_put(&b.marbles);
	a = b;
	sp1_pui_get(&a.plaits);
	sp1_mui_get(&a.marbles);
	CHECK(diff(&a, &b, "through the UIs") == 0, "put/get changed the patch");
	CHECK(sp1_pui_slot() == 6 && sp1_pui_scale() == 3 && sp1_mui_model() == 5 &&
	      sp1_mui_range() == SP1_MRB_RANGE_FULL, "the UIs did not take the patch");
	/* the largest patch the format can produce fits, with room to spare */
	CHECK(n < SP1_PRST_TEXT_MAX / 2, "a patch is %d bytes, close to the buffer", n);

	/* ---------- 3. the field check: one bad field, the rest still loads ---------- */
	printf("3. field check\n");
	unusual(&b);
	const int good_n = sp1_prst_write(&b, text, sizeof(text));
	(void)good_n;
	static const struct { const char *key, *value; } bad[] = {
		{ "timbre", "nan" }, { "timbre", "NaN" }, { "timbre", "inf" },
		{ "timbre", "-0.1000" }, { "timbre", "1.0001" }, { "timbre", "1e-1" },
		{ "timbre", "0.5abc" }, { "timbre", "" }, { "timbre", "." },
		{ "timbre", "0x1" }, { "timbre", "0.5 0.6" }, { "timbre", "\x01\xff" },
		{ "rate", "99999999999999999999" }, { "rate", "+0.5" },
		{ "output", "speaker" }, { "drive", "+30db" }, { "burst", "1/3" },
		{ "quantizer", "free" },            /* the free scale slot is not included */
		{ "scale", "off" },                 /* Marbles' scale has no "off" */
		{ "scale", "free" },
		{ "t1", "voct" },                   /* V/Oct is not on the t ring */
		{ "x1", "trig" },                   /* TRIG is not on the X ring */
		{ "y", "banana" }, { "t_range", "x2" }, { "range", "0-10v" },
		{ "deja_vu_t", "yes" }, { "t_model", "7" },
	};
	char buf[SP1_PRST_TEXT_MAX + 64];
	for (size_t k = 0; k < sizeof(bad) / sizeof(bad[0]); k++) {
		const int m = with(text, bad[k].key, bad[k].value, buf, sizeof(buf));
		sp1_prst_parse(buf, (size_t)m, &a, &rep);
		CHECK(rep.format_ok && rep.bad == 1 && rep.defaulted == 1 && rep.first_bad &&
		      strcmp(rep.first_bad, bad[k].key) == 0,
		      "%s = '%s': bad %d defaulted %d first %s", bad[k].key, bad[k].value,
		      rep.bad, rep.defaulted, rep.first_bad ? rep.first_bad : "-");
		CHECK(safe(&a), "%s = '%s' left an unsafe patch", bad[k].key, bad[k].value);
		/* exactly one field moved, and to its default */
		struct sp1_prst want = b;
		struct sp1_prst defs;
		sp1_prst_rip(&defs, b.plaits.slot);
		/* copy the default into `want` by re-parsing a file without that key */
		const int m2 = with(text, bad[k].key, NULL, buf, sizeof(buf));
		sp1_prst_parse(buf, (size_t)m2, &want, &rep);
		CHECK(rep.defaulted == 1 && rep.bad == 0, "%s: removing it did not default it",
		      bad[k].key);
		CHECK(diff(&a, &want, bad[k].key) == 0, "%s = '%s' is not 'that field's default'",
		      bad[k].key, bad[k].value);
		(void)defs;
	}
	/* the default IS the ROTC one: a bad TIMBRE on chords is chords' neutral (0, unipolar);
	 * on virtual analog it is 0.5 (bipolar) -- Adara: neutral for the LOADED engine */
	int m = with(text, "timbre", "nan", buf, sizeof(buf));
	sp1_prst_parse(buf, (size_t)m, &a, &rep);
	CHECK(a.plaits.slot == 6 && a.plaits.v[SP1_PUI_BASE][1] == 0.0f,
	      "chords: bad TIMBRE -> %.3f, not 0", a.plaits.v[SP1_PUI_BASE][1]);
	char buf2[SP1_PRST_TEXT_MAX + 64];
	with(text, "engine", "Virtual Analog", buf2, sizeof(buf2));     /* case ignored */
	m = with(buf2, "timbre", "nan", buf, sizeof(buf));
	sp1_prst_parse(buf, (size_t)m, &a, &rep);
	CHECK(a.plaits.slot == 0 && a.plaits.v[SP1_PUI_BASE][1] == 0.5f && rep.bad == 1,
	      "virtual analog: bad TIMBRE -> %.3f, not 0.5", a.plaits.v[SP1_PUI_BASE][1]);
	CHECK(fabsf(a.plaits.v[SP1_PUI_BASE][2] - b.plaits.v[SP1_PUI_BASE][2]) < 0.0001f,
	      "a GOOD BASE field was replaced");
	/* an unknown engine -> slot 1 (Adara); its good faders still load */
	m = with(text, "engine", "west coast complex", buf, sizeof(buf));
	sp1_prst_parse(buf, (size_t)m, &a, &rep);
	CHECK(a.plaits.slot == 0 && rep.bad == 1 && rep.first_bad &&
	      strcmp(rep.first_bad, "engine") == 0, "an unknown engine is not slot 1");
	CHECK(fabsf(a.plaits.v[SP1_PUI_BASE][2] - b.plaits.v[SP1_PUI_BASE][2]) < 0.0001f &&
	      a.marbles.model == 5, "an unknown engine took the rest of the file with it");
	/* the V/Oct interlock: the quantizer goes, the route stays */
	m = with(text, "x2", "voct", buf, sizeof(buf));
	sp1_prst_parse(buf, (size_t)m, &a, &rep);
	CHECK(a.marbles.dest[1] == SP1_DEST_VOCT && a.plaits.scale == SP1_PUI_SCALE_OFF &&
	      rep.bad == 1 && rep.first_bad && strcmp(rep.first_bad, "quantizer") == 0,
	      "quantizer + V/Oct: scale %d, x2 %d, bad %d", a.plaits.scale, a.marbles.dest[1],
	      rep.bad);
	/* every value missing: all defaults, nothing called bad */
	sp1_prst_parse("[wakes]\nformat = 1\n", 19, &a, &rep);
	CHECK(rep.format_ok && rep.defaulted == sp1_prst_fields() && rep.bad == 0,
	      "an empty slot: defaulted %d bad %d", rep.defaulted, rep.bad);
	CHECK(diff(&a, &d, "empty") == 0, "an empty slot is not the ROTC default");

	/* ---------- 4. the format version ---------- */
	printf("4. format version\n");
	static const char *const fmts[] = {
		NULL, "2", "0", "1x", "", "01x", " ", "999999999", "-1",
	};
	for (size_t k = 0; k < sizeof(fmts) / sizeof(fmts[0]); k++) {
		m = with(text, "format", fmts[k], buf, sizeof(buf));
		sp1_prst_parse(buf, (size_t)m, &a, &rep);
		CHECK(!rep.format_ok && rep.defaulted == sp1_prst_fields(),
		      "format '%s' was accepted", fmts[k] ? fmts[k] : "(none)");
		CHECK(diff(&a, &d, "format") == 0, "format '%s': not the whole default slot",
		      fmts[k] ? fmts[k] : "(none)");
	}
	/* `format` must be in [wakes]: the same line elsewhere is not a version */
	m = with(text, "format", NULL, buf, sizeof(buf));
	strcat(buf, "[plaits]\nformat = 1\n");
	sp1_prst_parse(buf, strlen(buf), &a, &rep);
	CHECK(!rep.format_ok, "a format line outside [wakes] counted");
	/* "01" is a decimal 1: accepted */
	m = with(text, "format", "01", buf, sizeof(buf));
	sp1_prst_parse(buf, (size_t)m, &a, &rep);
	CHECK(rep.format_ok && rep.defaulted == 0, "format 01 rejected");

	/* ---------- 5. the text as people edit it ---------- */
	printf("5. hand-edited text\n");
	{
		/* CRLF, tabs, comments, blank lines, case, unknown keys and sections, keys before
		 * any section, a duplicate (the last wins), no newline at the end */
		const char *t =
			"; my preset\r\n"
			"stray = 1\r\n"
			"\r\n"
			"[ WAKES ]\r\n"
			"\tFormat\t=\t1\r\n"
			"[Plaits]\r\n"
			"# a comment\r\n"
			"Engine = CHORDS\r\n"
			"timbre = 0.25\r\n"
			"timbre=0.75\r\n"
			"wobble = 0.3\r\n"
			"[future]\r\n"
			"timbre = 0.1\r\n"
			"[marbles]\r\n"
			"t1 = Level\r\n"
			"rate = 1";
		sp1_prst_parse(t, strlen(t), &a, &rep);
		CHECK(rep.format_ok && rep.bad == 0, "hand-edited: format_ok %d bad %d",
		      rep.format_ok, rep.bad);
		CHECK(a.plaits.slot == 6, "Engine = CHORDS not read");
		CHECK(fabsf(a.plaits.v[SP1_PUI_BASE][1] - 0.75f) < 1e-6f,
		      "duplicate: timbre %.3f, not the last (0.75)", a.plaits.v[SP1_PUI_BASE][1]);
		CHECK(a.marbles.t_dest[0] == SP1_DEST_LEVEL, "t1 = Level not read");
		CHECK(a.marbles.v[SP1_MUI_T_BASE][0] == 1.0f, "a last line without a newline lost");
		/* a section header that never closes: what follows belongs to no section */
		const char *u = "[wakes]\nformat = 1\n[plaits\ntimbre = 0.75\n";
		sp1_prst_parse(u, strlen(u), &a, &rep);
		CHECK(rep.format_ok && a.plaits.v[SP1_PUI_BASE][1] == 0.5f,
		      "a key under a broken section header was used");
		/* NUL bytes inside the text are just bad characters */
		char z[64] = "[wakes]\nformat = 1\n[plaits]\ntimbre = 0.2\0\n";
		sp1_prst_parse(z, 41, &a, &rep);
		CHECK(rep.bad == 1, "a NUL inside a value was accepted");
		/* NULL / zero length */
		sp1_prst_parse(NULL, 0, &a, &rep);
		CHECK(!rep.format_ok && safe(&a), "NULL text");
	}

	/* ---------- 6. the writer never overruns ---------- */
	printf("6. writer bounds\n");
	unusual(&b);
	n = sp1_prst_write(&b, text, sizeof(text));
	for (int size = 0; size <= n; size++) {
		char small[SP1_PRST_TEXT_MAX + 8];
		memset(small, 0x5a, sizeof(small));
		const int r = sp1_prst_write(&b, small, (size_t)size);
		CHECK(r == -1, "size %d (needs %d): returned %d", size, n + 1, r);
		for (int k = size; k < (int)sizeof(small); k++) {
			if (small[k] != 0x5a) {
				CHECK(0, "size %d: wrote byte %d", size, k);
				break;
			}
		}
	}
	CHECK(sp1_prst_write(&b, text, (size_t)n + 1u) == n, "exact size refused");
	/* garbage in a patch never reaches the file: NaN writes as 0, unknown choices are left
	 * out (and so load their default) */
	b.plaits.v[SP1_PUI_BASE][1] = NAN;
	b.marbles.dest[3] = 99;
	n = sp1_prst_write(&b, text, sizeof(text));
	sp1_prst_parse(text, (size_t)n, &a, &rep);
	CHECK(rep.bad == 0 && rep.defaulted == 1 && a.plaits.v[SP1_PUI_BASE][1] == 0.0f &&
	      a.marbles.dest[3] == SP1_DEST_NONE, "garbage in a patch was written");

	/* ---------- 7. the UIs' own sanitising (put) ---------- */
	printf("7. put sanitises\n");
	sp1_prst_rip(&b, -1);
	b.plaits.slot = 23;                     /* empty */
	b.plaits.v[SP1_PUI_SHIFT][0] = NAN;
	b.plaits.v[SP1_PUI_SHIFT][1] = 7.0f;
	b.plaits.scale = 42;
	b.marbles.v[SP1_MUI_Y][0] = INFINITY;
	b.marbles.model = -3;
	b.marbles.scale = 6;                    /* not included */
	b.marbles.t_dest[0] = SP1_DEST_VOCT;
	b.marbles.dest[0] = SP1_DEST_TRIG;
	sp1_pui_put(&b.plaits);
	sp1_mui_put(&b.marbles);
	sp1_pui_get(&a.plaits);
	sp1_mui_get(&a.marbles);
	a.out_mode = b.out_mode; a.burst_div = b.burst_div; a.drive = b.drive;
	CHECK(safe(&a), "put kept something unsafe");
	CHECK(a.plaits.slot == 0 && a.plaits.v[SP1_PUI_SHIFT][0] == 0.0f &&
	      a.plaits.v[SP1_PUI_SHIFT][1] == 1.0f && a.plaits.scale == SP1_PUI_SCALE_OFF,
	      "plaits put");
	CHECK(a.marbles.model == d.marbles.model && a.marbles.scale == d.marbles.scale &&
	      a.marbles.t_dest[0] == d.marbles.t_dest[0] && a.marbles.dest[0] == d.marbles.dest[0]
	      && a.marbles.v[SP1_MUI_Y][0] == d.marbles.v[SP1_MUI_Y][0], "marbles put");
	/* the parameters the synth gets from a sanitised patch are finite */
	{
		struct sp1_synth_params sp;
		struct sp1_marbles_params mp;
		sp1_pui_params(&sp);
		sp1_mui_params(&mp);
		CHECK(isfinite(sp.note) && isfinite(sp.timbre) && isfinite(sp.fm_mod) &&
		      isfinite(mp.rate) && isfinite(mp.y_spread), "non-finite synth params");
	}

	/* ---------- 8. nothing a card can hold crashes it or gets through unchecked ---------- */
	printf("8. corrupt files\n");
	unusual(&b);
	n = sp1_prst_write(&b, text, sizeof(text));
	int unsafe = 0, runs = 0;
	/* every truncation of a good file */
	for (int len = 0; len <= n; len++) {
		sp1_prst_parse(text, (size_t)len, &a, &rep);
		unsafe += !safe(&a);
		runs++;
	}
	/* random byte damage, and pure noise */
	srand(50);
	for (int k = 0; k < 20000; k++) {
		memcpy(buf, text, (size_t)n);
		const int hits = 1 + rand() % 8;
		for (int h = 0; h < hits; h++) {
			buf[rand() % n] = (char)(rand() & 0xff);
		}
		sp1_prst_parse(buf, (size_t)n, &a, &rep);
		unsafe += !safe(&a);
		runs++;
	}
	for (int k = 0; k < 2000; k++) {
		const int len = rand() % (int)sizeof(buf);
		for (int i = 0; i < len; i++) {
			buf[i] = (char)(rand() & 0xff);
		}
		sp1_prst_parse(buf, (size_t)len, &a, &rep);
		unsafe += !safe(&a);
		runs++;
	}
	CHECK(unsafe == 0, "%d of %d corrupt files gave an unsafe patch", unsafe, runs);
	printf("   %d corrupt files, all safe\n", runs);

	printf(fails ? "\nprsttest: %d FAILED\n" : "\nprsttest: all passed\n", fails);
	return fails ? 1 : 0;
}
