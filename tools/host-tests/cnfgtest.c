/* wakes-sp1 #52: host checks of the config-file reader (sp1_cnfg.c) and the log file's cursor
 * into the RAM log (sp1_logring.c) -- the cases that would take hours, or a broken drive, to
 * make on the device. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SP1_CNFG_TEMPLATES
#include "sp1_cnfg.h"
#include "sp1_logring.h"
#include "sp1_logs_gen.h"
#include "sp1_engines_gen.h"
#include "sp1_audio_gen.h"

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); \
	printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

/* ---------------------------------------------------------------- the reader */

static int problems_seen;
static int last_line;
static char last_what[200];

static void problem(void *ctx, int line, const char *what, const char *why)
{
	(void)ctx;
	problems_seen++;
	last_line = line;
	snprintf(last_what, sizeof(last_what), "%s", what);
	if (getenv("CNFGTEST_VERBOSE")) {
		printf("    line %d: %s -- %s\n", line, what, why);
	}
}

/* Read `text` over the defaults, `chunk` bytes at a time (0 = all at once). */
static int read_logs(const char *text, size_t chunk, struct sp1_logs_cfg *c)
{
	struct sp1_logs_reader r;
	problems_seen = 0;
	last_line = 0;
	last_what[0] = '\0';
	sp1_logs_defaults(c);
	sp1_logs_begin(&r, c, problem, NULL);
	const size_t n = strlen(text);
	if (chunk == 0u) {
		chunk = n ? n : 1u;
	}
	for (size_t i = 0u; i < n; i += chunk) {
		sp1_logs_feed(&r, text + i, (i + chunk <= n) ? chunk : n - i);
	}
	const int bad = sp1_logs_end(&r);
	CHECK(bad == problems_seen, "reader counted %d problems, reported %d", bad, problems_seen);
	return bad;
}

static void reader(void)
{
	printf("[1] logs.cnfg: the reader\n");
	struct sp1_logs_cfg c, d;
	sp1_logs_defaults(&d);
	CHECK((d.cats & ~SP1_LOG_ALL) == 0u, "the shipped defaults: unknown bits 0x%02x", d.cats);
	/* The checks below hold whatever config/logs.cnfg ships with: a line that must be
	 * IGNORED sets the opposite of its default, so it would show if it were applied. */
	const char *not_midi = (d.cats & SP1_LOG_MIDI) ? "off" : "on";
	const char *not_usb = (d.cats & SP1_LOG_USB) ? "off" : "on";
	char f[96];

	/* The template the firmware writes when the file is missing reads back as exactly the built-in defaults, with
	 * no problem -- whatever chunk size the drive hands over. */
	const size_t chunks[] = { 0u, 1u, 7u, 128u };
	for (size_t k = 0u; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
		const int bad = read_logs(SP1_LOGS_TEMPLATE, chunks[k], &c);
		CHECK(bad == 0, "template, chunk %u: %d problems (line %d: %s)", (unsigned)chunks[k], bad,
		      last_line, last_what);
		CHECK(c.enabled == d.enabled && c.cats == d.cats,
		      "template, chunk %u: %d/0x%02x, defaults %d/0x%02x", (unsigned)chunks[k], c.enabled,
		      c.cats, d.enabled, d.cats);
	}

	/* Overrides, case, CRLF, a BOM, inline comments, no newline at the end. */
	CHECK(read_logs("\xEF\xBB\xBF; Format: ini\r\n[LOGS]\r\nEnabled = OFF ; quiet\r\n"
			"midi=off\r\nusb = on # yes\r\nstorage = off", 0u, &c) == 0,
	      "overrides: %d problems (line %d: %s)", problems_seen, last_line, last_what);
	CHECK(!c.enabled && c.cats == ((d.cats & (SP1_LOG_PERFORMANCE | SP1_LOG_CONTROLS)) |
				       SP1_LOG_USB),
	      "overrides: got %d/0x%02x", c.enabled, c.cats);

	/* A bad value keeps its default; the rest of the file still applies. */
	CHECK(read_logs("[logs]\nenabled = maybe\nmidi = off\n", 0u, &c) == 1 && last_line == 2,
	      "bad value: %d problems, line %d", problems_seen, last_line);
	CHECK(c.enabled == d.enabled && !(c.cats & SP1_LOG_MIDI), "bad value: got %d/0x%02x",
	      c.enabled, c.cats);
	CHECK(strcmp(last_what, "enabled = maybe") == 0, "bad value named as \"%s\"", last_what);

	/* The same key twice: the FIRST is used (Adara's rule, #52). */
	CHECK(read_logs("[logs]\nmidi = off\nmidi = on\n", 0u, &c) == 1 && last_line == 3,
	      "twice: %d problems, line %d", problems_seen, last_line);
	CHECK(!(c.cats & SP1_LOG_MIDI), "twice: the first did not win (0x%02x)", c.cats);
	/* ...also when the first one was unreadable: it still counts as set. */
	snprintf(f, sizeof(f), "[logs]\nusb = ?\nusb = %s\n", not_usb);
	CHECK(read_logs(f, 0u, &c) == 2 && (c.cats & SP1_LOG_USB) == (d.cats & SP1_LOG_USB),
	      "twice, first bad: %d problems, 0x%02x", problems_seen, c.cats);

	/* Unknown key, unknown section (reported once, its keys ignored), outside any section,
	 * no '=', a section without its ']'. */
	CHECK(read_logs("[logs]\nverbose = on\n", 0u, &c) == 1 && last_line == 2,
	      "unknown key: %d problems, line %d", problems_seen, last_line);
	CHECK(read_logs("[midi]\nchannel = 3\nclock = off\n[logs]\nmidi = off\n", 0u, &c) == 1 &&
	      last_line == 2 && !(c.cats & SP1_LOG_MIDI),
	      "unknown section: %d problems, line %d, 0x%02x", problems_seen, last_line, c.cats);
	snprintf(f, sizeof(f), "midi = %s\n[logs]\n", not_midi);
	CHECK(read_logs(f, 0u, &c) == 1 && (c.cats & SP1_LOG_MIDI) == (d.cats & SP1_LOG_MIDI),
	      "outside a section: %d problems, 0x%02x", problems_seen, c.cats);
	CHECK(read_logs("[logs]\njust words\n", 0u, &c) == 1 && last_line == 2,
	      "no '=': %d problems, line %d", problems_seen, last_line);
	snprintf(f, sizeof(f), "[logs\nmidi = %s\n", not_midi);
	CHECK(read_logs(f, 0u, &c) == 2 && (c.cats & SP1_LOG_MIDI) == (d.cats & SP1_LOG_MIDI),
	      "no ']': %d problems, 0x%02x", problems_seen, c.cats);

	/* A line over the limit is one problem, and the line after it is read normally --
	 * also when the long line arrives a byte at a time. */
	char longf[600];
	memset(longf, 0, sizeof(longf));
	strcpy(longf, "[logs]\n; ");
	memset(longf + strlen(longf), 'x', 300);
	strcat(longf, "\nusb = off\n");
	CHECK(read_logs(longf, 1u, &c) == 1 && last_line == 2 && !(c.cats & SP1_LOG_USB),
	      "long line: %d problems, line %d, 0x%02x", problems_seen, last_line, c.cats);

	/* An empty file and a file of comments: the defaults, no problem. */
	CHECK(read_logs("", 0u, &c) == 0 && c.enabled == d.enabled && c.cats == d.cats, "empty file");
	CHECK(read_logs("; nothing\n# here\n\n", 0u, &c) == 0 && c.cats == d.cats, "comments");

	char b[48];
	CHECK(strcmp(sp1_logs_describe(SP1_LOG_ALL, b, sizeof(b)),
		     "performance controls midi usb storage") == 0, "describe all: %s", b);
	CHECK(strcmp(sp1_logs_describe(0u, b, sizeof(b)), "none") == 0, "describe none: %s", b);
}

/* ---------------------------------------------------------------- categories */

static void categories(void)
{
	printf("[2] the log's categories\n");
	static const struct {
		const char *line;
		uint8_t cat;
	} T[] = {
		{ "[+00:01:02.345] CPU eng=60.1/70.2\n", SP1_LOG_PERFORMANCE },
		{ "[+00:01:02.345] MISS/ms eng=3\n", SP1_LOG_PERFORMANCE },
		{ "[+00:01:02.345] THREADS main=1.0\n", SP1_LOG_PERFORMANCE },
		{ "[+00:01:02.345] ENGINE 3: 2-op FM\n", SP1_LOG_CONTROLS },
		{ "[+00:01:02.345] DEJA VU t on\n", SP1_LOG_CONTROLS },
		{ "[+00:01:02.345] MIDI port enabled\n", SP1_LOG_MIDI },
		{ "[+00:01:02.345] CLOCK run\n", SP1_LOG_MIDI },
		{ "[+00:01:02.345] UAC open=1\n", SP1_LOG_USB },
		{ "[+00:01:02.345] STORE volume ok\n", SP1_LOG_STORAGE },
		{ "[+00:01:02.345] LOG 60 s: 12 flushes\n", SP1_LOG_STORAGE },
		/* Always written. */
		{ "[+00:01:02.345] PWR  shutdown suppressed\n", 0u },
		{ "[+00:01:02.345] BOOT on battery\n", 0u },
		{ "[+00:01:02.345] CNFG logs.cnfg line 3: x -- y\n", 0u },
		{ "[+00:01:02.345] [ON] batt raw=1234\n", 0u },
		{ "[+00:01:02.345] resetreas 0x00000001\n", 0u },
		{ "[+00:01:02.345] Singles: 209\n", 0u },
		{ "[+00:01:02.345] USBx\n", 0u },
		{ "=== RESET (resetreas 0x2) ===\n", 0u },
		{ "AUD start rc=0\n", SP1_LOG_PERFORMANCE },          /* no stamp */
		{ "", 0u },
	};
	for (size_t i = 0u; i < sizeof(T) / sizeof(T[0]); i++) {
		const uint8_t c = sp1_log_category(T[i].line, strlen(T[i].line));
		CHECK(c == T[i].cat, "\"%.40s\": 0x%02x, expected 0x%02x", T[i].line, c, T[i].cat);
	}
}

/* ---------------------------------------------------------------- the ring */

#define RSZ 64u
static struct sp1_logring R;
static char data[RSZ];
static char got[4096];
static size_t got_n;
static uint32_t lost;

static void puts_r(const char *s)
{
	for (; *s; s++) {
		sp1_logring_put(&R, data, *s);
	}
}

/* Everything the file can take now, into got[] (appended). max = the piece size. */
static void drain(size_t max)
{
	char out[256];
	size_t n;
	while ((n = sp1_logring_take(&R, data, out, max, &lost)) > 0u) {
		memcpy(got + got_n, out, n);
		got_n += n;
		got[got_n] = '\0';
	}
}

/* ...only what the last ON still owes its own file (sp1_logring_take_before_standby). */
static void drain_old(size_t max)
{
	char out[256];
	size_t n;
	while ((n = sp1_logring_take_before_standby(&R, data, out, max, &lost)) > 0u) {
		memcpy(got + got_n, out, n);
		got_n += n;
		got[got_n] = '\0';
	}
}

static void reset_ring(void)
{
	sp1_logring_init(&R, RSZ);
	got_n = 0u;
	got[0] = '\0';
	lost = 0u;
}

static void ring(void)
{
	printf("[3] the log file's cursor\n");

	/* Whole lines only; a partial line waits for its end. */
	reset_ring();
	puts_r("one\ntwo\nthr");
	drain(32u);
	CHECK(strcmp(got, "one\ntwo\n") == 0 && lost == 0u, "lines: \"%s\" lost %u", got, lost);
	puts_r("ee\n");
	drain(32u);
	CHECK(strcmp(got, "one\ntwo\nthree\n") == 0, "partial completed: \"%s\"", got);

	/* A line longer than a piece comes in pieces. */
	reset_ring();
	puts_r("abcdefghijklmnop\n");
	drain(6u);
	CHECK(strcmp(got, "abcdefghijklmnop\n") == 0, "pieces: \"%s\"", got);

	/* STANDBY (Adara): pause..resume is skipped; lines before and after are kept. */
	reset_ring();
	puts_r("boot\n");
	sp1_logring_pause(&R);
	puts_r("[STANDBY] a\n[STANDBY] b\n");
	drain(32u);                     /* while still in STANDBY: only what came before */
	CHECK(strcmp(got, "boot\n") == 0, "paused: \"%s\"", got);
	sp1_logring_resume(&R);
	puts_r("on\n");
	drain(32u);
	CHECK(strcmp(got, "boot\non\n") == 0 && lost == 0u, "gap: \"%s\" lost %u", got, lost);

	/* A second STANDBY before the file caught up merges with the first (documented). */
	reset_ring();
	puts_r("a\n");
	sp1_logring_pause(&R);
	puts_r("s1\n");
	sp1_logring_resume(&R);
	puts_r("b\n");
	sp1_logring_pause(&R);
	puts_r("s2\n");
	sp1_logring_resume(&R);
	puts_r("c\n");
	drain(32u);
	CHECK(strcmp(got, "a\nc\n") == 0, "two gaps: \"%s\"", got);

	/* Overrun: what the ring overwrote is counted, and the file resumes on a whole line. */
	reset_ring();
	for (int i = 0; i < 20; i++) {
		char l[16];
		snprintf(l, sizeof(l), "line %02d\n", i);   /* 8 bytes each: 160 > 64 */
		puts_r(l);
	}
	drain(32u);
	CHECK(lost == 160u - 64u + 0u || lost == 160u - 64u + 8u,
	      "overrun: lost %u (expected 96, or 104 with the cut line)", lost);
	CHECK(got_n > 0u && got[got_n - 1u] == '\n' && strncmp(got, "line ", 5) == 0,
	      "overrun: resumes on a line: \"%s\"", got);
	CHECK(lost + got_n == 160u, "overrun: lost + taken = %u, expected 160", lost + (unsigned)got_n);

	/* Overrun inside a STANDBY gap: the gap's bytes are not a loss. */
	reset_ring();
	puts_r("before\n");                        /* 7 */
	drain(32u);
	sp1_logring_pause(&R);
	for (int i = 0; i < 30; i++) {
		puts_r("standby\n");               /* 240 bytes of STANDBY */
	}
	sp1_logring_resume(&R);
	puts_r("after\n");
	drain(32u);
	CHECK(strcmp(got, "before\nafter\n") == 0 && lost == 0u,
	      "gap overwritten: \"%s\" lost %u", got, lost);

	/* Logging off this ON: nothing so far is owed to the file. */
	reset_ring();
	puts_r("old\n");
	sp1_logring_skip(&R);
	puts_r("new\n");
	drain(32u);
	CHECK(strcmp(got, "new\n") == 0, "skip: \"%s\"", got);

	/* An ON with logging OFF, then STANDBY, then an ON with logging on (Adara's test,
	 * 0007.LOGS): none of the logging-off ON's lines reach the file -- not those before the
	 * ON job's decision, not those after it. */
	reset_ring();
	puts_r("boot\n");
	sp1_logring_pause(&R);                    /* plugged-in boot: STANDBY */
	puts_r("[STANDBY] a\n");
	sp1_logring_resume(&R);                   /* ON #1 */
	puts_r("CNFG logging off\n");
	sp1_logring_skip(&R);                     /* ...the ON job decides: off */
	puts_r("ON1 played\nPRST save\n");
	sp1_logring_pause(&R);                    /* STANDBY */
	puts_r("[STANDBY] b\n");
	sp1_logring_resume(&R);                   /* ON #2 */
	puts_r("CNFG logging on\n");
	sp1_logring_keep(&R);                     /* ...the ON job decides: on */
	puts_r("ON2 played\n");
	drain(32u);
	CHECK(strcmp(got, "CNFG logging on\nON2 played\n") == 0 && lost == 0u,
	      "logging off then on: \"%s\" lost %u", got, lost);

	/* Every ON a new file (Adara, 2026-10-16): what the last ON printed after its last flush
	 * goes to ITS file first, then the new file gets this ON only -- STANDBY in neither. */
	reset_ring();
	puts_r("ON1 played\n");
	drain(32u);                               /* ON1's flushes */
	puts_r("LOG closed\n");                   /* after the last one */
	sp1_logring_pause(&R);
	puts_r("[STANDBY] a\n");
	sp1_logring_resume(&R);
	puts_r("ON2\n");
	got_n = 0u;
	got[0] = '\0';
	drain_old(32u);
	CHECK(strcmp(got, "LOG closed\n") == 0, "catch-up: the old file gets \"%s\"", got);
	drain_old(32u);
	CHECK(strcmp(got, "LOG closed\n") == 0, "catch-up: stops at STANDBY: \"%s\"", got);
	got_n = 0u;
	got[0] = '\0';
	drain(32u);
	CHECK(strcmp(got, "ON2\n") == 0 && lost == 0u, "catch-up: the new file gets \"%s\" lost %u",
	      got, lost);

	/* A STANDBY that printed nothing (no gap to jump): the boundary still holds. */
	reset_ring();
	puts_r("tail\n");
	sp1_logring_pause(&R);
	sp1_logring_resume(&R);
	puts_r("new\n");
	drain_old(32u);
	CHECK(strcmp(got, "tail\n") == 0, "empty STANDBY, old: \"%s\"", got);
	got_n = 0u;
	got[0] = '\0';
	drain(32u);
	CHECK(strcmp(got, "new\n") == 0, "empty STANDBY, new: \"%s\"", got);

	/* Nothing owed (the last flush took everything): the old file gets nothing. */
	reset_ring();
	puts_r("all flushed\n");
	drain(32u);
	sp1_logring_pause(&R);
	puts_r("[STANDBY]\n");
	sp1_logring_resume(&R);
	puts_r("new\n");
	got_n = 0u;
	got[0] = '\0';
	drain_old(32u);
	CHECK(got_n == 0u, "nothing owed: \"%s\"", got);
	drain(32u);
	CHECK(strcmp(got, "new\n") == 0, "nothing owed, new: \"%s\"", got);

	/* A line STANDBY cut (the backstop mid-print) ends the old file as it is; still in
	 * STANDBY, nothing is taken. */
	reset_ring();
	puts_r("PWR 29 s\nhal");
	sp1_logring_pause(&R);
	drain_old(32u);
	CHECK(strcmp(got, "PWR 29 s\nhal") == 0, "cut line while paused: \"%s\"", got);
	puts_r("[STANDBY]\n");
	sp1_logring_resume(&R);
	puts_r("new\n");
	drain_old(32u);
	CHECK(strcmp(got, "PWR 29 s\nhal") == 0, "cut line: nothing more: \"%s\"", got);

	/* The old lines partly overwritten: counted, and the rest still goes to the old file. */
	reset_ring();
	for (int i = 0; i < 12; i++) {
		char l[16];
		snprintf(l, sizeof(l), "old %02d\n", i);      /* 7 bytes each: 84 > 64 */
		puts_r(l);
	}
	sp1_logring_pause(&R);
	sp1_logring_resume(&R);
	drain_old(32u);
	CHECK(lost > 0u && got_n > 0u && strncmp(got, "old ", 4) == 0 &&
	      strcmp(got + got_n - 7u, "old 11\n") == 0 && lost + got_n == 84u,
	      "old overrun: lost %u, got \"%s\"", lost, got);

	/* After a reset: a cursor ahead of the writer is pulled back; a bad header fails. */
	reset_ring();
	puts_r("kept\n");
	R.frd = R.total + 100u;
	CHECK(sp1_logring_valid(&R, RSZ), "valid: a sane ring");
	drain(32u);
	CHECK(strcmp(got, "kept\n") == 0, "valid: cursor clamped: \"%s\"", got);
	R.head = RSZ;
	CHECK(!sp1_logring_valid(&R, RSZ), "valid: head out of range");
	sp1_logring_init(&R, RSZ);
	CHECK(!sp1_logring_valid(&R, RSZ * 2u), "valid: another size");

	/* The 32-bit wrap of the position counter is harmless. */
	reset_ring();
	R.total = R.frd = 0xFFFFFFF0u;
	R.gap_from = R.gap_to = R.total;
	puts_r("wrap one\nwrap two\n");
	drain(32u);
	CHECK(strcmp(got, "wrap one\nwrap two\n") == 0 && lost == 0u, "wrap: \"%s\" lost %u", got,
	      lost);
}

/* ---------------------------------------------------------------- engines.cnfg */

static uint8_t estack[SP1_CNFG_SLOTS];
static int efilled;

static int read_engines(const char *text, size_t chunk)
{
	struct sp1_engines_reader r;
	problems_seen = 0;
	last_line = 0;
	last_what[0] = '\0';
	sp1_engines_begin(&r, problem, NULL);
	const size_t n = strlen(text);
	if (chunk == 0u) {
		chunk = n ? n : 1u;
	}
	for (size_t i = 0u; i < n; i += chunk) {
		sp1_engines_feed(&r, text + i, (i + chunk <= n) ? chunk : n - i);
	}
	const int bad = sp1_engines_end(&r, estack, &efilled);
	CHECK(bad == problems_seen, "engines: counted %d, reported %d", bad, problems_seen);
	return bad;
}

static bool is_default(void)
{
	static const uint8_t def[SP1_CNFG_SLOTS] = SP1_ENGINE_DEFAULT_STACK;
	return memcmp(estack, def, sizeof(def)) == 0;
}

static int idx(const char *name)
{
	return sp1_engine_index(name, strlen(name));
}

static void engines(void)
{
	printf("[4] engines.cnfg: names and the stack\n");

	/* Names: letters and digits, case ignored -- spaces, hyphens and / + do not matter. */
	static const struct {
		const char *name;
		const char *as;
	} SAME[] = {
		{ "virtual-analog", "virtual analog" }, { "VIRTUAL ANALOG", "virtual analog" },
		{ "grain-formant", "grain / formant" }, { "grain-/-formant", "grain / formant" },
		{ "VA-VCF", "VA + VCF" },               { "va + vcf", "VA + VCF" },
		{ "2-op-FM", "2-op FM" },               { "6-op-fm-c", "6-op FM C" },
		{ "hi hat", "hi-hat" },                 { "string-machine", "string machine" },
		{ "stringmachine", "string machine" },
	};
	for (size_t i = 0u; i < sizeof(SAME) / sizeof(SAME[0]); i++) {
		CHECK(idx(SAME[i].name) >= 0 && idx(SAME[i].name) == idx(SAME[i].as),
		      "name \"%s\" -> %d, \"%s\" -> %d", SAME[i].name, idx(SAME[i].name), SAME[i].as,
		      idx(SAME[i].as));
	}
	CHECK(idx("string") != idx("string machine"), "string is not string machine");
	CHECK(idx("6-op FM") < 0 && idx("") < 0 && idx("moog") < 0 && idx("-") < 0,
	      "partial / empty / unknown names are no engine");

	/* The template written when the file is missing: no problem, exactly the built-in stack, 16 engines. */
	const size_t chunks[] = { 0u, 1u, 128u };
	for (size_t k = 0u; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
		CHECK(read_engines(SP1_ENGINES_TEMPLATE, chunks[k]) == 0 && is_default() &&
		      efilled == 16, "template, chunk %u: %d problems (line %d: %s), %d engines",
		      (unsigned)chunks[k], problems_seen, last_line, last_what, efilled);
	}

	/* A user's stack: the file is the WHOLE stack; any spelling of a name. */
	CHECK(read_engines("; Format: ini\r\n[ENGINES]\r\n1 = chords ; ●○○○\r\n3 = Grain-Formant\r\n"
			   "7 =\r\n8 = none\r\n24 = VA-VCF\r\n", 0u) == 0 && efilled == 3 &&
	      estack[0] == idx("chords") && estack[2] == idx("grain / formant") &&
	      estack[23] == idx("VA + VCF") && estack[1] == SP1_CNFG_EMPTY &&
	      estack[6] == SP1_CNFG_EMPTY && estack[7] == SP1_CNFG_EMPTY,
	      "user stack: %d problems (line %d: %s), %d engines", problems_seen, last_line,
	      last_what, efilled);

	/* Anything wrong: the WHOLE file is set aside for the built-in stack, every problem
	 * reported on its line. */
	static const struct {
		const char *text;
		int problems;
		int line;
	} BAD[] = {
		{ "[engines]\n1 = chords\n2 = moog\n", 1, 3 },             /* not an engine   */
		{ "[engines]\n1 = chords\n5 = Chords\n", 1, 3 },           /* engine twice    */
		{ "[engines]\n25 = chords\n", 1, 2 },                      /* no slot 25      */
		{ "[engines]\n0 = chords\n", 1, 2 },                       /* no slot 0       */
		{ "[engines]\nx = chords\n", 1, 2 },                       /* not a slot      */
		{ "[engines]\n1 = chords\n1 = speech\n", 1, 3 },           /* slot twice      */
		{ "[engine]\n1 = chords\n", 1, 2 },                       /* not [engines]   */
		{ "1 = chords\n[engines]\n2 = speech\n", 1, 1 },           /* outside [engines] */
		{ "[engines]\n1 =\n2 = none\n", 1, 3 },                    /* no engine at all */
		{ "", 1, 0 },                                              /* an empty file    */
		{ "[engines]\n1 = chords\njust words\n", 1, 3 },           /* not a setting    */
	};
	for (size_t i = 0u; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
		const int bad = read_engines(BAD[i].text, i % 2 ? 1u : 0u);
		CHECK(bad == BAD[i].problems && last_line == BAD[i].line && is_default() &&
		      efilled == 16, "bad file %u: %d problems (want %d), line %d (want %d), %s",
		      (unsigned)i, bad, BAD[i].problems, last_line, BAD[i].line,
		      is_default() ? "default stack" : "NOT the default stack");
	}
}

/* ---------------------------------------------------------------- audio.cnfg */

static struct sp1_audio_cfg acfg;

static int read_audio(const char *text, size_t chunk)
{
	struct sp1_audio_reader r;
	problems_seen = 0;
	last_line = 0;
	sp1_audio_reader_begin(&r, &acfg, problem, NULL);
	const size_t n = strlen(text);
	if (chunk == 0u) {
		chunk = n ? n : 1u;
	}
	for (size_t i = 0u; i < n; i += chunk) {
		sp1_audio_reader_feed(&r, text + i, (i + chunk <= n) ? chunk : n - i);
	}
	const int bad = sp1_audio_reader_end(&r);
	CHECK(bad == problems_seen, "audio: counted %d, reported %d", bad, problems_seen);
	return bad;
}

static void audio(void)
{
	printf("[5] audio.cnfg\n");
	struct sp1_audio_cfg d;
	sp1_audio_cfg_defaults(&d);
	CHECK(d.parked == SP1_AUDIO_USB_LEVEL_PARKED && d.parked_db == SP1_AUDIO_USB_PARKED_DB &&
	      d.parked_step == SP1_AUDIO_USB_PARKED_STEP && d.speaker == SP1_AUDIO_USB_SPEAKER,
	      "audio defaults differ from the generated ones");
	const size_t chunks[] = { 0u, 1u, 128u };
	for (size_t k = 0u; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
		CHECK(read_audio(SP1_AUDIO_TEMPLATE, chunks[k]) == 0 &&
		      memcmp(&acfg, &d, sizeof(d)) == 0,
		      "template, chunk %u: %d problems (line %d: %s)", (unsigned)chunks[k],
		      problems_seen, last_line, last_what);
	}
	CHECK(read_audio("[usb]\nlevel = VOL\nparked_level = -9 dB\nspeaker = on\n", 0u) == 0 &&
	      acfg.parked == 0u && acfg.parked_db == -9 && acfg.parked_step == 3u &&
	      acfg.speaker == 1u, "overrides: %d problems, %d/%d/%u/%u", problems_seen,
	      acfg.parked, acfg.parked_db, acfg.parked_step, acfg.speaker);
	CHECK(read_audio("[usb]\nparked_level = 0\n", 0u) == 0 && acfg.parked_step == 0u,
	      "0 dB is a step");
	CHECK(read_audio("[usb]\nparked_level = -30db\n", 0u) == 0 && acfg.parked_step == 10u,
	      "-30 dB is a step");
	/* Not steps: each keeps the default, one problem each. */
	static const char *const BADLVL[] = { "-7", "-33", "6", "+3", "-6 dBFS", "loud", "" };
	for (size_t i = 0u; i < sizeof(BADLVL) / sizeof(BADLVL[0]); i++) {
		char t[64];
		snprintf(t, sizeof(t), "[usb]\nparked_level = %s\n", BADLVL[i]);
		CHECK(read_audio(t, 0u) == 1 && acfg.parked_db == d.parked_db,
		      "parked_level = %s: %d problems, %d dB", BADLVL[i], problems_seen,
		      acfg.parked_db);
	}
	CHECK(read_audio("[usb]\nspeaker = maybe\nspeaker = on\nvolume = 3\n[out]\nx = 1\n", 0u)
	      == 4 && acfg.speaker == d.speaker,
	      "bad value / twice / unknown key / section: %d problems", problems_seen);
}

/* ---------------------------------------------------------------- midi.cnfg */

static struct sp1_midi_cfg mcfg;
static int mnotes;

static int read_midi(const char *text, size_t chunk)
{
	struct sp1_midi_reader r;
	problems_seen = 0;
	last_line = 0;
	last_what[0] = '\0';
	sp1_midi_reader_begin(&r, &mcfg, problem, NULL);
	const size_t n = strlen(text);
	if (chunk == 0u) {
		chunk = n ? n : 1u;
	}
	for (size_t i = 0u; i < n; i += chunk) {
		sp1_midi_reader_feed(&r, text + i, (i + chunk <= n) ? chunk : n - i);
	}
	const int bad = sp1_midi_reader_end(&r, &mnotes);
	CHECK(bad + mnotes == problems_seen, "midi: counted %d + %d notes, reported %d", bad,
	      mnotes, problems_seen);
	return bad;
}

/* Field by field (the struct has padding), and the two CC tables agree with each other. */
static bool midi_same(const struct sp1_midi_cfg *a, const struct sp1_midi_cfg *b)
{
	return a->channel == b->channel && a->priority == b->priority && a->legato == b->legato &&
	       a->portamento == b->portamento && a->bend_range == b->bend_range &&
	       a->sustain_cc == b->sustain_cc && a->smooth_ms == b->smooth_ms &&
	       a->pickup == b->pickup && a->clock == b->clock &&
	       a->clock_lead_ms == b->clock_lead_ms && a->vel_dest == b->vel_dest &&
	       a->at_dest == b->at_dest && a->vel_depth == b->vel_depth &&
	       a->at_depth == b->at_depth &&
	       memcmp(a->dest_cc, b->dest_cc, sizeof(a->dest_cc)) == 0 &&
	       memcmp(a->cc_dest, b->cc_dest, sizeof(a->cc_dest)) == 0;
}

static bool tables_agree(const struct sp1_midi_cfg *c)
{
	for (int cc = 0; cc < 128; cc++) {
		const int d = c->cc_dest[cc];
		if (d >= 0 && c->dest_cc[d] != cc) {
			return false;
		}
	}
	for (int d = 0; d < SP1_MIDI_DESTS; d++) {
		if (c->dest_cc[d] >= 0 && c->cc_dest[(int)c->dest_cc[d]] != d) {
			return false;
		}
		if (c->dest_cc[d] >= 0 && c->dest_cc[d] == c->sustain_cc) {
			return false;
		}
	}
	return true;
}

static void midi(void)
{
	printf("[6] midi.cnfg\n");
	struct sp1_midi_cfg d;
	sp1_midi_cfg_defaults(&d);
	CHECK(d.channel == SP1_MIDI_CHANNEL && d.pickup == SP1_MIDI_PICKUP &&
	      d.sustain_cc == SP1_MIDI_SUSTAIN_CC && d.dest_cc[SP1_MIDI_D_TIMBRE] == 3 &&
	      d.vel_dest == SP1_MIDI_VELOCITY_DEST && tables_agree(&d),
	      "midi defaults differ from the generated ones");

	/* The template written when the file is missing reads back as exactly the built-in script. */
	const size_t chunks[] = { 0u, 1u, 128u };
	for (size_t k = 0u; k < sizeof(chunks) / sizeof(chunks[0]); k++) {
		CHECK(read_midi(SP1_MIDI_TEMPLATE, chunks[k]) == 0 && mnotes == 0 &&
		      midi_same(&mcfg, &d), "template, chunk %u: %d problems (line %d: %s)",
		      (unsigned)chunks[k], problems_seen, last_line, last_what);
	}

	/* [midi] values, every form. */
	CHECK(read_midi("[midi]\nchannel = 3\nnote_priority = LOW\nlegato = auto\n"
			"portamento = t10\nbend_range = 12\ncc_smoothing = 0\npickup = takeover\n"
			"clock = off\nclock_lead = 25\n", 0u) == 0 &&
	      mcfg.channel == 2 && mcfg.priority == 1u && mcfg.legato == 1u &&
	      mcfg.portamento == 10u && mcfg.bend_range == 12u && mcfg.smooth_ms == 0u &&
	      mcfg.pickup == SP1_MIDI_PICKUP_TAKEOVER && mcfg.clock == 0u &&
	      mcfg.clock_lead_ms == 25, "[midi] overrides: %d problems (line %d: %s)",
	      problems_seen, last_line, last_what);
	CHECK(read_midi("[midi]\nchannel = omni\nportamento = r0\nclock_lead = notes\n", 0u) == 0 &&
	      mcfg.channel == SP1_MIDI_OMNI && mcfg.portamento == 51u && mcfg.clock_lead_ms == -2,
	      "omni / r0 / notes");
	static const char *const BADMIDI[] = {
		"channel = 0", "channel = 17", "note_priority = first", "legato = maybe",
		"portamento = t0", "portamento = t51", "portamento = r51", "bend_range = 25",
		"cc_smoothing = 201", "pickup = both", "clock = yes", "clock_lead = 201",
	};
	for (size_t i = 0u; i < sizeof(BADMIDI) / sizeof(BADMIDI[0]); i++) {
		char t[64];
		snprintf(t, sizeof(t), "[midi]\n%s\n", BADMIDI[i]);
		CHECK(read_midi(t, 0u) == 1 && midi_same(&mcfg, &d), "\"%s\": %d problems, %s",
		      BADMIDI[i], problems_seen, midi_same(&mcfg, &d) ? "default kept" : "CHANGED");
	}

	/* THE CC RULE (Adara, #52). 1: your line TAKES a default's CC -- MORPH's CC 9 to TIMBRE:
	 * MORPH has none, a note (not a problem); TIMBRE's own CC 3 is free. */
	CHECK(read_midi("[plaits]\ntimbre = cc 9\n", 0u) == 0 && mnotes == 1 &&
	      mcfg.dest_cc[SP1_MIDI_D_TIMBRE] == 9 && mcfg.dest_cc[SP1_MIDI_D_MORPH] == -1 &&
	      mcfg.cc_dest[9] == SP1_MIDI_D_TIMBRE && mcfg.cc_dest[3] == -1 && tables_agree(&mcfg),
	      "user takes a default's CC: %d problems, %d notes, timbre %d morph %d",
	      problems_seen - mnotes, mnotes, mcfg.dest_cc[SP1_MIDI_D_TIMBRE],
	      mcfg.dest_cc[SP1_MIDI_D_MORPH]);
	/* 2: two of YOUR lines on one CC: the first keeps it, the later gets none (a problem). */
	CHECK(read_midi("[plaits]\ntimbre = cc 70\nmorph = cc 70\n", 0u) == 1 && last_line == 3 &&
	      mcfg.dest_cc[SP1_MIDI_D_TIMBRE] == 70 && mcfg.dest_cc[SP1_MIDI_D_MORPH] == -1 &&
	      tables_agree(&mcfg), "two user lines, one CC: %d problems, line %d, morph %d",
	      problems_seen, last_line, mcfg.dest_cc[SP1_MIDI_D_MORPH]);
	/* ...across sections and against the sustain pedal, which is a line like any other. */
	CHECK(read_midi("[midi]\nsustain = cc 70\n[marbles]\nrate = cc 70\n", 0u) == 1 &&
	      mcfg.sustain_cc == 70 && mcfg.dest_cc[SP1_MIDI_D_RATE] == -1 && tables_agree(&mcfg),
	      "sustain first, rate later: sustain %d rate %d", mcfg.sustain_cc,
	      mcfg.dest_cc[SP1_MIDI_D_RATE]);
	/* 3: a user line taking the DEFAULT sustain CC 64 turns the pedal off (a note). */
	CHECK(read_midi("[plaits]\nlevel = cc 64\n", 0u) == 0 && mnotes == 1 &&
	      mcfg.sustain_cc == -1 && mcfg.dest_cc[SP1_MIDI_D_LEVEL] == 64 && tables_agree(&mcfg),
	      "user takes the pedal's CC: sustain %d", mcfg.sustain_cc);
	/* 4: `none` frees a CC, which a default then keeps nothing of. */
	CHECK(read_midi("[plaits]\ntimbre = none\nmorph = cc 3\n", 0u) == 0 && mnotes == 0 &&
	      mcfg.dest_cc[SP1_MIDI_D_TIMBRE] == -1 && mcfg.dest_cc[SP1_MIDI_D_MORPH] == 3 &&
	      tables_agree(&mcfg), "none then reuse: %d notes", mnotes);
	/* Reserved CCs, fine halves, nonsense: the default is kept. */
	static const char *const BADCC[] = { "cc 6", "cc 0", "cc 101", "cc 120", "cc 40", "cc 128",
					     "ccc", "7", "cc -1" };
	for (size_t i = 0u; i < sizeof(BADCC) / sizeof(BADCC[0]); i++) {
		char t[64];
		snprintf(t, sizeof(t), "[plaits]\ntimbre = %s\n", BADCC[i]);
		CHECK(read_midi(t, 0u) == 1 && mcfg.dest_cc[SP1_MIDI_D_TIMBRE] == 3,
		      "timbre = %s: %d problems, cc %d", BADCC[i], problems_seen,
		      mcfg.dest_cc[SP1_MIDI_D_TIMBRE]);
	}
	/* A parameter in the wrong section, an unknown one, the same key twice. */
	CHECK(read_midi("[marbles]\ntimbre = cc 70\n", 0u) == 1 && midi_same(&mcfg, &d),
	      "timbre under [marbles]");
	CHECK(read_midi("[plaits]\ncutoff = cc 70\n", 0u) == 1, "unknown parameter");
	CHECK(read_midi("[plaits]\ntimbre = cc 70\ntimbre = cc 71\n", 0u) == 1 &&
	      mcfg.dest_cc[SP1_MIDI_D_TIMBRE] == 70, "the same key twice: the first wins");

	/* [bind]. */
	CHECK(read_midi("[bind]\nvelocity = none\naftertouch = MORPH -50%\n", 0u) == 0 &&
	      mcfg.vel_dest == -1 && mcfg.at_dest == SP1_MIDI_D_MORPH && mcfg.at_depth == -0.5f,
	      "bind: velocity %d aftertouch %d %.2f", mcfg.vel_dest, mcfg.at_dest,
	      (double)mcfg.at_depth);
	CHECK(read_midi("[bind]\naftertouch = rate 100%\n", 0u) == 0 &&
	      mcfg.at_dest == SP1_MIDI_D_RATE && mcfg.at_depth == 1.0f, "bind to a Marbles parameter");
	static const char *const BADBIND[] = { "velocity = cutoff", "velocity = timbre 150%",
					      "velocity = timbre 50", "breath = timbre" };
	for (size_t i = 0u; i < sizeof(BADBIND) / sizeof(BADBIND[0]); i++) {
		char t[64];
		snprintf(t, sizeof(t), "[bind]\n%s\n", BADBIND[i]);
		CHECK(read_midi(t, 0u) == 1 && mcfg.vel_dest == d.vel_dest, "\"%s\": %d problems",
		      BADBIND[i], problems_seen);
	}
}

int main(void)
{
	reader();
	categories();
	ring();
	engines();
	audio();
	midi();
	printf(fails ? "cnfgtest: %d FAILED\n" : "cnfgtest: all passed\n", fails);
	return fails ? 1 : 0;
}
