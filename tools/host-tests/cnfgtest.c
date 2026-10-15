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
	CHECK(d.enabled && d.cats == SP1_LOG_ALL, "the shipped defaults are on/all (got %d/0x%02x)",
	      d.enabled, d.cats);

	/* The template the fresh image writes reads back as exactly the built-in defaults, with
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
	CHECK(!c.enabled && c.cats == (SP1_LOG_ALL & ~(SP1_LOG_MIDI | SP1_LOG_STORAGE)),
	      "overrides: got %d/0x%02x", c.enabled, c.cats);

	/* A bad value keeps its default; the rest of the file still applies. */
	CHECK(read_logs("[logs]\nenabled = maybe\nmidi = off\n", 0u, &c) == 1 && last_line == 2,
	      "bad value: %d problems, line %d", problems_seen, last_line);
	CHECK(c.enabled && !(c.cats & SP1_LOG_MIDI), "bad value: got %d/0x%02x", c.enabled, c.cats);
	CHECK(strcmp(last_what, "enabled = maybe") == 0, "bad value named as \"%s\"", last_what);

	/* The same key twice: the FIRST is used (Adara's rule, #52). */
	CHECK(read_logs("[logs]\nmidi = off\nmidi = on\n", 0u, &c) == 1 && last_line == 3,
	      "twice: %d problems, line %d", problems_seen, last_line);
	CHECK(!(c.cats & SP1_LOG_MIDI), "twice: the first did not win (0x%02x)", c.cats);
	/* ...also when the first one was unreadable: it still counts as set. */
	CHECK(read_logs("[logs]\nusb = ?\nusb = off\n", 0u, &c) == 2 && (c.cats & SP1_LOG_USB),
	      "twice, first bad: %d problems, 0x%02x", problems_seen, c.cats);

	/* Unknown key, unknown section (reported once, its keys ignored), outside any section,
	 * no '=', a section without its ']'. */
	CHECK(read_logs("[logs]\nverbose = on\n", 0u, &c) == 1 && last_line == 2,
	      "unknown key: %d problems, line %d", problems_seen, last_line);
	CHECK(read_logs("[midi]\nchannel = 3\nclock = off\n[logs]\nmidi = off\n", 0u, &c) == 1 &&
	      last_line == 2 && !(c.cats & SP1_LOG_MIDI),
	      "unknown section: %d problems, line %d, 0x%02x", problems_seen, last_line, c.cats);
	CHECK(read_logs("midi = off\n[logs]\n", 0u, &c) == 1 && (c.cats & SP1_LOG_MIDI),
	      "outside a section: %d problems, 0x%02x", problems_seen, c.cats);
	CHECK(read_logs("[logs]\njust words\n", 0u, &c) == 1 && last_line == 2,
	      "no '=': %d problems, line %d", problems_seen, last_line);
	CHECK(read_logs("[logs\nmidi = off\n", 0u, &c) == 2 && (c.cats & SP1_LOG_MIDI),
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
	CHECK(read_logs("", 0u, &c) == 0 && c.enabled && c.cats == d.cats, "empty file");
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

	/* The template fresh writes: no problem, exactly the built-in stack, 16 engines. */
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

int main(void)
{
	reader();
	categories();
	ring();
	engines();
	printf(fails ? "cnfgtest: %d FAILED\n" : "cnfgtest: all passed\n", fails);
	return fails ? 1 : 0;
}
