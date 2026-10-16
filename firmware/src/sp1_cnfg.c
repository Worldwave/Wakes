/* wakes-sp1 — the config files on the drive (#52). See sp1_cnfg.h. */
/* midi.cnfg's reader needs the destinations' names (SP1_MIDI_NAME): a second, small copy of
 * that table besides sp1_midi.cc's. Defined before the first include of sp1_midi_gen.h. */
#define SP1_MIDI_GEN_TABLES
#include "sp1_cnfg.h"
#include "sp1_logs_gen.h"
#include "sp1_engines_gen.h"
#include "sp1_audio_gen.h"

#include <string.h>

_Static_assert(SP1_ENGINE_SLOTS == SP1_CNFG_SLOTS, "engines.cnfg has 24 slots");
_Static_assert(SP1_ENGINE_EMPTY == SP1_CNFG_EMPTY, "one value for an empty slot");
_Static_assert(SP1_PLAITS_ENGINE_COUNT <= 32, "the used-engine set is a 32-bit mask");

/* ---------------------------------------------------------------- the ini reader */

static bool space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Trim s in place; returns the new start. */
static char *trim(char *s)
{
	while (space(*s)) {
		s++;
	}
	size_t n = strlen(s);
	while (n > 0u && space(s[n - 1u])) {
		s[--n] = '\0';
	}
	return s;
}

static void lower_str(char *s)
{
	for (; *s != '\0'; s++) {
		*s = lower(*s);
	}
}

static bool same(const char *a, const char *b)
{
	for (; *a != '\0' && lower(*a) == lower(*b); a++, b++) {
	}
	return lower(*a) == lower(*b);
}

void sp1_cnfg_begin(struct sp1_cnfg_parser *p, sp1_cnfg_entry_fn entry,
		    sp1_cnfg_problem_fn problem, void *ctx)
{
	memset(p, 0, sizeof(*p));
	p->entry = entry;
	p->problem = problem;
	p->ctx = ctx;
}

static void line_done(struct sp1_cnfg_parser *p)
{
	p->line++;
	p->buf[p->n] = '\0';
	const bool overlong = p->overlong;
	p->n = 0u;
	p->overlong = false;
	if (overlong) {
		p->problem(p->ctx, p->line, "(a long line)",
			   "over 160 characters: the line is ignored");
		return;
	}
	char *s = p->buf;
	/* A UTF-8 byte order mark (Notepad and Excel write one). */
	if (p->line == 1 && (unsigned char)s[0] == 0xEFu && (unsigned char)s[1] == 0xBBu &&
	    (unsigned char)s[2] == 0xBFu) {
		s += 3;
	}
	s = trim(s);
	if (*s == '\0' || *s == ';' || *s == '#') {
		return;
	}
	if (*s == '[') {
		char *end = strchr(s, ']');
		if (end == NULL) {
			p->section[0] = '\0';
			p->problem(p->ctx, p->line, s, "a section name needs a closing ]: the "
				   "settings under it are ignored");
			return;
		}
		*end = '\0';
		char *name = trim(s + 1);
		if (strlen(name) >= sizeof(p->section)) {
			p->section[0] = '\0';
			p->problem(p->ctx, p->line, name, "is not a section: the settings under it "
				   "are ignored");
			return;
		}
		strcpy(p->section, name);
		lower_str(p->section);
		return;
	}
	char *eq = strchr(s, '=');
	if (eq == NULL) {
		p->problem(p->ctx, p->line, s, "is not a setting (key = value): ignored");
		return;
	}
	*eq = '\0';
	char *key = trim(s);
	char *val = eq + 1;
	/* An inline comment: ; or # at the start of the value or after whitespace (Python's
	 * configparser, which checks the defaults at build time, reads them the same way). */
	for (char *c = val; *c != '\0'; c++) {
		if ((*c == ';' || *c == '#') && (c == val || space(c[-1]))) {
			*c = '\0';
			break;
		}
	}
	val = trim(val);
	if (*key == '\0') {
		p->problem(p->ctx, p->line, "(nothing before the =)", "a setting needs a name: "
			   "ignored");
		return;
	}
	lower_str(key);
	p->entry(p->ctx, p->line, p->section, key, val);
}

void sp1_cnfg_feed(struct sp1_cnfg_parser *p, const char *text, size_t n)
{
	for (size_t i = 0u; i < n; i++) {
		const char c = text[i];
		if (c == '\n') {
			line_done(p);
		} else if (p->n < SP1_CNFG_LINE_MAX) {
			p->buf[p->n++] = (c == '\0') ? ' ' : c;
		} else {
			p->overlong = true;
		}
	}
}

void sp1_cnfg_end(struct sp1_cnfg_parser *p)
{
	if (p->n > 0u || p->overlong) {
		line_done(p);
	}
}

/* ---------------------------------------------------------------- logs.cnfg */

static const struct {
	const char *key;
	uint8_t bit;            /* SP1_LOG_*, or 0 for `enabled` */
} LOG_KEYS[] = {
	{ "enabled",     0u },
	{ "performance", SP1_LOG_PERFORMANCE },
	{ "controls",    SP1_LOG_CONTROLS },
	{ "midi",        SP1_LOG_MIDI },
	{ "usb",         SP1_LOG_USB },
	{ "storage",     SP1_LOG_STORAGE },
};
#define SEEN_ENABLED 0x80u

void sp1_logs_defaults(struct sp1_logs_cfg *c)
{
	c->enabled = SP1_LOGS_DEF_ENABLED != 0;
	c->cats = (uint8_t)SP1_LOGS_DEF_CATS;
}

static void report(struct sp1_logs_reader *r, int line, const char *what, const char *why)
{
	r->problems++;
	r->problem(r->ctx, line, what, why);
}

static void logs_entry(void *ctx, int line, const char *section, const char *key,
		       const char *value)
{
	struct sp1_logs_reader *r = ctx;
	if (section[0] == '\0') {
		report(r, line, key, "is not under [logs]: ignored");
		return;
	}
	if (strcmp(section, "logs") != 0) {
		if (strcmp(section, r->bad_section) != 0) {
			strcpy(r->bad_section, section);   /* both < SP1_CNFG_NAME_MAX */
			report(r, line, section, "is not a section of logs.cnfg (expected [logs]): "
			       "its settings are ignored");
		}
		return;
	}
	for (size_t i = 0u; i < sizeof(LOG_KEYS) / sizeof(LOG_KEYS[0]); i++) {
		if (strcmp(key, LOG_KEYS[i].key) != 0) {
			continue;
		}
		const uint8_t seen = LOG_KEYS[i].bit ? LOG_KEYS[i].bit : SEEN_ENABLED;
		if (r->seen & seen) {
			report(r, line, key, "is set twice: the first one is used");
			return;
		}
		r->seen |= seen;
		bool on;
		if (same(value, "on")) {
			on = true;
		} else if (same(value, "off")) {
			on = false;
		} else {
			size_t k = strlen(key);
			memcpy(r->what, key, k);
			memcpy(r->what + k, " = ", 3u);
			const size_t v = strlen(value);    /* key + value < SP1_CNFG_LINE_MAX */
			memcpy(r->what + k + 3u, value, v + 1u);
			report(r, line, r->what, "must be on or off: the default is used");
			return;
		}
		if (LOG_KEYS[i].bit == 0u) {
			r->cfg->enabled = on;
		} else if (on) {
			r->cfg->cats |= LOG_KEYS[i].bit;
		} else {
			r->cfg->cats &= (uint8_t)~LOG_KEYS[i].bit;
		}
		return;
	}
	report(r, line, key, "is not a logs.cnfg setting: ignored");
}

static void logs_problem(void *ctx, int line, const char *what, const char *why)
{
	report(ctx, line, what, why);
}

void sp1_logs_begin(struct sp1_logs_reader *r, struct sp1_logs_cfg *c,
		    sp1_cnfg_problem_fn problem, void *ctx)
{
	memset(r, 0, sizeof(*r));
	r->cfg = c;
	r->problem = problem;
	r->ctx = ctx;
	sp1_cnfg_begin(&r->p, logs_entry, logs_problem, r);
}

void sp1_logs_feed(struct sp1_logs_reader *r, const char *text, size_t n)
{
	sp1_cnfg_feed(&r->p, text, n);
}

int sp1_logs_end(struct sp1_logs_reader *r)
{
	sp1_cnfg_end(&r->p);
	return r->problems;
}

const char *sp1_logs_describe(uint8_t cats, char *buf, size_t size)
{
	size_t n = 0u;
	buf[0] = '\0';
	for (size_t i = 1u; i < sizeof(LOG_KEYS) / sizeof(LOG_KEYS[0]); i++) {
		if (!(cats & LOG_KEYS[i].bit)) {
			continue;
		}
		const size_t k = strlen(LOG_KEYS[i].key);
		if (n + k + 2u > size) {
			break;
		}
		if (n > 0u) {
			buf[n++] = ' ';
		}
		memcpy(buf + n, LOG_KEYS[i].key, k + 1u);
		n += k;
	}
	if (n == 0u && size >= 5u) {
		memcpy(buf, "none", 5u);
	}
	return buf;
}

/* ---------------------------------------------------------------- engines.cnfg */

int sp1_engine_index(const char *name, size_t n)
{
	for (int i = 0; i < SP1_PLAITS_ENGINE_COUNT; i++) {
		if (sp1_engine_name_eq(SP1_PLAITS_ENGINES[i].name, name, n)) {
			return i;
		}
	}
	return -1;
}

void sp1_engines_defaults(uint8_t out[SP1_CNFG_SLOTS])
{
	static const uint8_t def[SP1_CNFG_SLOTS] = SP1_ENGINE_DEFAULT_STACK;
	memcpy(out, def, SP1_CNFG_SLOTS);
}

static void eng_report(struct sp1_engines_reader *r, int line, const char *what, const char *why)
{
	r->problems++;
	r->problem(r->ctx, line, what, why);
}

static void eng_problem(void *ctx, int line, const char *what, const char *why)
{
	eng_report(ctx, line, what, why);
}

/* "<key> = <value>" into r->what, for a problem that is about the whole line. */
static const char *eng_line(struct sp1_engines_reader *r, const char *key, const char *value)
{
	const size_t k = strlen(key);
	const size_t v = strlen(value);            /* key + value < SP1_CNFG_LINE_MAX */
	memcpy(r->what, key, k);
	memcpy(r->what + k, " = ", 3u);
	memcpy(r->what + k + 3u, value, v + 1u);
	return r->what;
}

static void eng_entry(void *ctx, int line, const char *section, const char *key,
		      const char *value)
{
	struct sp1_engines_reader *r = ctx;
	if (section[0] == '\0') {
		eng_report(r, line, key, "is not under [engines]");
		return;
	}
	if (strcmp(section, "engines") != 0) {
		if (strcmp(section, r->bad_section) != 0) {
			strcpy(r->bad_section, section);   /* both < SP1_CNFG_NAME_MAX */
			eng_report(r, line, section, "is not a section of engines.cnfg (expected "
				   "[engines])");
		}
		return;
	}
	/* The slot: 1-24, digits only. */
	int slot = 0;
	const char *k = key;
	for (; *k >= '0' && *k <= '9' && slot <= SP1_CNFG_SLOTS; k++) {
		slot = slot * 10 + (*k - '0');
	}
	if (*k != '\0' || k == key || slot < 1 || slot > SP1_CNFG_SLOTS) {
		eng_report(r, line, key, "is not a slot (1-24)");
		return;
	}
	const uint32_t bit = 1u << (slot - 1);
	if (r->seen & bit) {
		eng_report(r, line, key, "is set twice");
		return;
	}
	r->seen |= bit;
	if (value[0] == '\0' || same(value, "none")) {
		return;                                    /* an empty slot */
	}
	const int e = sp1_engine_index(value, strlen(value));
	if (e < 0) {
		eng_report(r, line, eng_line(r, key, value), "is not an engine name (the list is at "
			   "the end of the file)");
		return;
	}
	if (r->used & (1u << e)) {
		eng_report(r, line, eng_line(r, key, value), "uses an engine that is already in "
			   "another slot (each engine can be used once)");
		return;
	}
	r->used |= 1u << e;
	r->stack[slot - 1] = (uint8_t)e;
	r->filled++;
}

void sp1_engines_begin(struct sp1_engines_reader *r, sp1_cnfg_problem_fn problem, void *ctx)
{
	memset(r, 0, sizeof(*r));
	memset(r->stack, SP1_CNFG_EMPTY, sizeof(r->stack));
	r->problem = problem;
	r->ctx = ctx;
	sp1_cnfg_begin(&r->p, eng_entry, eng_problem, r);
}

void sp1_engines_feed(struct sp1_engines_reader *r, const char *text, size_t n)
{
	sp1_cnfg_feed(&r->p, text, n);
}

int sp1_engines_end(struct sp1_engines_reader *r, uint8_t out[SP1_CNFG_SLOTS], int *filled)
{
	sp1_cnfg_end(&r->p);
	if (r->problems == 0 && r->filled == 0) {
		eng_report(r, r->p.line, "(the whole file)", "names no engine: T2/T3 would have "
			   "nothing to play");
	}
	if (r->problems == 0) {
		memcpy(out, r->stack, SP1_CNFG_SLOTS);
		*filled = r->filled;
	} else {
		sp1_engines_defaults(out);
		int n = 0;
		for (int i = 0; i < SP1_CNFG_SLOTS; i++) {
			n += (out[i] != SP1_CNFG_EMPTY);
		}
		*filled = n;
	}
	return r->problems;
}

/* ---------------------------------------------------------------- the log's categories */

/* Every tag the firmware prints, by category (the first word of the line). A tag that is
 * not here -- BOOT, PWR, CHARGE, JACK, CTRL, CALIBRATION, CNFG, the "[ON]" status line --
 * is always written. A new printk tag lands there too until it is added. */
static const struct {
	const char *tag;
	uint8_t cat;
} TAGS[] = {
	{ "AUD", SP1_LOG_PERFORMANCE },     { "CPU", SP1_LOG_PERFORMANCE },
	{ "BLK", SP1_LOG_PERFORMANCE },     { "MISS", SP1_LOG_PERFORMANCE },
	{ "SYN", SP1_LOG_PERFORMANCE },     { "THREADS", SP1_LOG_PERFORMANCE },
	{ "PAGE", SP1_LOG_CONTROLS },       { "SHIFT", SP1_LOG_CONTROLS },
	{ "ENGINE", SP1_LOG_CONTROLS },     { "MODULE", SP1_LOG_CONTROLS },
	{ "MODEL", SP1_LOG_CONTROLS },      { "ROUTE", SP1_LOG_CONTROLS },
	{ "RIP", SP1_LOG_CONTROLS },        { "UNPATCH", SP1_LOG_CONTROLS },
	{ "RANGE", SP1_LOG_CONTROLS },      { "LEVEL", SP1_LOG_CONTROLS },
	{ "QUANTIZE", SP1_LOG_CONTROLS },   { "SCALE", SP1_LOG_CONTROLS },
	{ "PICKUP", SP1_LOG_CONTROLS },     { "DRIVE", SP1_LOG_CONTROLS },
	{ "BURST", SP1_LOG_CONTROLS },      { "DIVISION", SP1_LOG_CONTROLS },
	{ "OUTPUT", SP1_LOG_CONTROLS },     { "TRIG", SP1_LOG_CONTROLS },
	{ "FREQUENCY", SP1_LOG_CONTROLS },  { "MARBLES", SP1_LOG_CONTROLS },
	{ "DEJA", SP1_LOG_CONTROLS },       { "DIVERSITY", SP1_LOG_CONTROLS },
	{ "TONE", SP1_LOG_CONTROLS },       { "SAG", SP1_LOG_CONTROLS },
	{ "MIDI", SP1_LOG_MIDI },           { "CLOCK", SP1_LOG_MIDI },
	{ "USB", SP1_LOG_USB },             { "UAC", SP1_LOG_USB },
	{ "SPEAKER", SP1_LOG_USB },
	{ "STORE", SP1_LOG_STORAGE },       { "EMMC", SP1_LOG_STORAGE },
	{ "PRST", SP1_LOG_STORAGE },        { "LOG", SP1_LOG_STORAGE },
};

uint8_t sp1_log_category(const char *line, size_t n)
{
	size_t i = 0u;
	/* "[+hh:mm:ss.mmm] " */
	if (n >= 2u && line[0] == '[' && line[1] == '+') {
		while (i < n && line[i] != ']') {
			i++;
		}
		i++;
		while (i < n && line[i] == ' ') {
			i++;
		}
	}
	size_t k = 0u;
	while (i + k < n && line[i + k] >= 'A' && line[i + k] <= 'Z') {
		k++;
	}
	if (k < 2u || k > 9u) {
		return 0u;
	}
	/* The tag is a whole word: "STORE ..." yes, "Singles" or "USBx" no. */
	if (i + k < n) {
		const char c = line[i + k];
		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
			return 0u;
		}
	}
	for (size_t t = 0u; t < sizeof(TAGS) / sizeof(TAGS[0]); t++) {
		if (strlen(TAGS[t].tag) == k && memcmp(TAGS[t].tag, line + i, k) == 0) {
			return TAGS[t].cat;
		}
	}
	return 0u;
}

/* ---------------------------------------------------------------- shared helpers */

/* A non-negative decimal, digits only, at most `max`. */
static bool parse_uint(const char *s, int max, int *out)
{
	int v = 0;
	if (*s == '\0') {
		return false;
	}
	for (; *s != '\0'; s++) {
		if (*s < '0' || *s > '9') {
			return false;
		}
		v = v * 10 + (*s - '0');
		if (v > max) {
			return false;
		}
	}
	*out = v;
	return true;
}

/* "cc N" (also "ccN", any case) -> N 0-127; "none" / "off" / "" -> -1 when allowed; else -2. */
static int parse_cc(const char *s, bool allow_none)
{
	if (allow_none && (s[0] == '\0' || same(s, "none") || same(s, "off"))) {
		return -1;
	}
	if (lower(s[0]) != 'c' || lower(s[1]) != 'c') {
		return -2;
	}
	s += 2;
	while (space(*s)) {
		s++;
	}
	int n;
	return parse_uint(s, 127, &n) ? n : -2;
}

/* MIDI's own CCs (tools/gen_midi.py RESERVED): bank select, data entry, (N)RPN, channel mode. */
static const char *cc_reserved(int n)
{
	if (n == 0 || n == 32) {
		return "bank select";
	}
	if (n == 6 || n == 38 || n == 96 || n == 97) {
		return "data entry";
	}
	if (n >= 98 && n <= 101) {
		return "parameter numbers (RPN / NRPN)";
	}
	if (n >= 120) {
		return "channel mode messages";
	}
	return NULL;
}

/* A small text builder for messages: no printf in this file. */
struct msg {
	char *b;
	size_t size, n;
};

static void msg_s(struct msg *m, const char *s)
{
	while (*s != '\0' && m->n + 1u < m->size) {
		m->b[m->n++] = *s++;
	}
	m->b[m->n] = '\0';
}

static void msg_i(struct msg *m, int v)
{
	char t[12];
	int k = 0;
	unsigned u = (unsigned)(v < 0 ? -v : v);
	do {
		t[k++] = (char)('0' + u % 10u);
		u /= 10u;
	} while (u != 0u && k < 11);
	if (v < 0) {
		t[k++] = '-';
	}
	while (k > 0 && m->n + 1u < m->size) {
		m->b[m->n++] = t[--k];
	}
	m->b[m->n] = '\0';
}

static const char *key_value(char *buf, const char *key, const char *value)
{
	struct msg m = { buf, SP1_CNFG_LINE_MAX + 4u, 0u };
	msg_s(&m, key);
	msg_s(&m, " = ");
	msg_s(&m, value);
	return buf;
}

/* ---------------------------------------------------------------- audio.cnfg */

_Static_assert(sizeof(struct sp1_audio_cfg) == 4u, "SP1_AUDIO_CFG_DEFAULTS: one value a field");

void sp1_audio_cfg_defaults(struct sp1_audio_cfg *c)
{
	static const struct sp1_audio_cfg def = SP1_AUDIO_CFG_DEFAULTS;
	*c = def;
}

static void audio_report(struct sp1_audio_reader *r, int line, const char *what, const char *why)
{
	r->problems++;
	r->problem(r->ctx, line, what, why);
}

static void audio_problem(void *ctx, int line, const char *what, const char *why)
{
	audio_report(ctx, line, what, why);
}

static void audio_entry(void *ctx, int line, const char *section, const char *key,
			const char *value)
{
	struct sp1_audio_reader *r = ctx;
	if (section[0] == '\0') {
		audio_report(r, line, key, "is not under [usb]: ignored");
		return;
	}
	if (strcmp(section, "usb") != 0) {
		if (strcmp(section, r->bad_section) != 0) {
			strcpy(r->bad_section, section);
			audio_report(r, line, section, "is not a section of audio.cnfg (expected [usb]): "
				     "its settings are ignored");
		}
		return;
	}
	static const char *const KEYS[] = { "level", "parked_level", "speaker" };
	int k = -1;
	for (int i = 0; i < 3; i++) {
		if (strcmp(key, KEYS[i]) == 0) {
			k = i;
		}
	}
	if (k < 0) {
		audio_report(r, line, key, "is not an audio.cnfg setting: ignored");
		return;
	}
	if (r->seen & (1u << k)) {
		audio_report(r, line, key, "is set twice: the first one is used");
		return;
	}
	r->seen |= (uint8_t)(1u << k);
	if (k == 0) {
		if (same(value, "parked")) {
			r->cfg->parked = 1u;
		} else if (same(value, "vol")) {
			r->cfg->parked = 0u;
		} else {
			audio_report(r, line, key_value(r->what, key, value),
				     "must be parked or vol: the default is used");
		}
	} else if (k == 1) {
		/* -6, -6 dB, -6db; 0. One of VOL's steps: 0 .. -30, 3 dB apart. */
		char t[16];
		size_t n = 0u;
		const char *s = value;
		bool neg = false;
		if (*s == '-' || *s == '+') {
			neg = (*s == '-');
			s++;
		}
		while (*s >= '0' && *s <= '9' && n < sizeof(t) - 1u) {
			t[n++] = *s++;
		}
		t[n] = '\0';
		while (space(*s)) {
			s++;
		}
		int db;
		const bool unit_ok = (*s == '\0') || same(s, "db");
		if (n == 0u || !unit_ok || !parse_uint(t, 99, &db) || (db != 0 && !neg) ||
		    db > 30 || db % 3 != 0) {
			audio_report(r, line, key_value(r->what, key, value),
				     "must be one of VOL's steps (0, -3, -6 ... -30): the default is used");
		} else {
			r->cfg->parked_db = (int8_t)(-db);
			r->cfg->parked_step = (uint8_t)(db / 3);
		}
	} else {
		if (same(value, "on")) {
			r->cfg->speaker = 1u;
		} else if (same(value, "off")) {
			r->cfg->speaker = 0u;
		} else {
			audio_report(r, line, key_value(r->what, key, value),
				     "must be on or off: the default is used");
		}
	}
}

void sp1_audio_reader_begin(struct sp1_audio_reader *r, struct sp1_audio_cfg *c,
			    sp1_cnfg_problem_fn problem, void *ctx)
{
	memset(r, 0, sizeof(*r));
	sp1_audio_cfg_defaults(c);
	r->cfg = c;
	r->problem = problem;
	r->ctx = ctx;
	sp1_cnfg_begin(&r->p, audio_entry, audio_problem, r);
}

void sp1_audio_reader_feed(struct sp1_audio_reader *r, const char *text, size_t n)
{
	sp1_cnfg_feed(&r->p, text, n);
}

int sp1_audio_reader_end(struct sp1_audio_reader *r)
{
	sp1_cnfg_end(&r->p);
	return r->problems;
}

/* ---------------------------------------------------------------- midi.cnfg */

_Static_assert(SP1_MIDI_DESTS <= 31, "seen_dest is a 32-bit mask");

void sp1_midi_cfg_defaults(struct sp1_midi_cfg *c)
{
	static const struct sp1_midi_cfg def = SP1_MIDI_CFG_DEFAULTS;
	*c = def;
}

/* SP1_MIDI_NAME[d] is "<section> <key>". */
static bool dest_is(int d, const char *section, const char *key)
{
	const char *n = SP1_MIDI_NAME[d];
	const size_t s = strlen(section);
	return strncmp(n, section, s) == 0 && n[s] == ' ' && strcmp(n + s + 1, key) == 0;
}

static const char *dest_key(int d)
{
	const char *n = strchr(SP1_MIDI_NAME[d], ' ');
	return n != NULL ? n + 1 : SP1_MIDI_NAME[d];
}

static void midi_report(struct sp1_midi_reader *r, int line, const char *what, const char *why)
{
	r->problems++;
	r->problem(r->ctx, line, what, why);
}

static void midi_problem(void *ctx, int line, const char *what, const char *why)
{
	midi_report(ctx, line, what, why);
}

static bool once(struct sp1_midi_reader *r, int line, const char *key, uint32_t *seen,
		 uint32_t bit)
{
	if (*seen & bit) {
		midi_report(r, line, key, "is set twice: the first one is used");
		return false;
	}
	*seen |= bit;
	return true;
}

/* A CC value for `who` (a destination, or the sustain pedal): checked, then queued for the
 * collision pass at the end. */
static void take_cc(struct sp1_midi_reader *r, int line, int who, const char *key,
		    const char *value)
{
	const int n = parse_cc(value, true);
	struct msg m = { r->why, sizeof(r->why), 0u };
	if (n == -2) {
		midi_report(r, line, key_value(r->what, key, value),
			    "is not `cc N` with N 0-127 (or none): the default is kept");
		return;
	}
	if (n >= 0 && cc_reserved(n) != NULL) {
		msg_s(&m, "CC ");
		msg_i(&m, n);
		msg_s(&m, " is reserved by MIDI (");
		msg_s(&m, cc_reserved(n));
		msg_s(&m, "): the default is kept");
		midi_report(r, line, key_value(r->what, key, value), r->why);
		return;
	}
	if (n >= 33 && n <= 63) {
		msg_s(&m, "CC ");
		msg_i(&m, n);
		msg_s(&m, " is the fine half of 14-bit CC ");
		msg_i(&m, n - 32);
		msg_s(&m, "; use CC ");
		msg_i(&m, n - 32);
		msg_s(&m, ": the default is kept");
		midi_report(r, line, key_value(r->what, key, value), r->why);
		return;
	}
	r->user_cc[who] = (int8_t)n;
	r->user_line[who] = (int16_t)line;
	r->order[r->n_order++] = (uint8_t)who;
}

static void midi_section(struct sp1_midi_reader *r, int line, const char *key, const char *value)
{
	static const char *const KEYS[] = {
		"channel", "note_priority", "legato", "portamento", "bend_range", "sustain",
		"cc_smoothing", "pickup", "clock", "clock_lead",
	};
	int k = -1;
	for (int i = 0; i < (int)(sizeof(KEYS) / sizeof(KEYS[0])); i++) {
		if (strcmp(key, KEYS[i]) == 0) {
			k = i;
		}
	}
	if (k < 0) {
		midi_report(r, line, key, "is not a [midi] setting: ignored");
		return;
	}
	uint32_t seen = r->seen_midi;
	const bool first = once(r, line, key, &seen, 1u << k);
	r->seen_midi = (uint16_t)seen;
	if (!first) {
		return;
	}
	struct sp1_midi_cfg *c = r->cfg;
	const char *why = NULL;
	int v;
	switch (k) {
	case 0:   /* channel: 1-16 or omni */
		if (same(value, "omni")) {
			c->channel = SP1_MIDI_OMNI;
		} else if (parse_uint(value, 16, &v) && v >= 1) {
			c->channel = (int8_t)(v - 1);
		} else {
			why = "must be 1-16 or omni: the default is used";
		}
		break;
	case 1:   /* note_priority */
		if (same(value, "last")) {
			c->priority = 0u;
		} else if (same(value, "low")) {
			c->priority = 1u;
		} else if (same(value, "high")) {
			c->priority = 2u;
		} else {
			why = "must be last, low or high: the default is used";
		}
		break;
	case 2:   /* legato: off 0, auto 1, on 2 (Yarns) */
		if (same(value, "off")) {
			c->legato = 0u;
		} else if (same(value, "auto")) {
			c->legato = 1u;
		} else if (same(value, "on")) {
			c->legato = 2u;
		} else {
			why = "must be off, on or auto: the default is used";
		}
		break;
	case 3:   /* portamento: 0, t1-t50 (time), r0-r50 (rate) */
		if (strcmp(value, "0") == 0) {
			c->portamento = 0u;
		} else if (lower(value[0]) == 't' && parse_uint(value + 1, 50, &v) && v >= 1) {
			c->portamento = (uint8_t)v;
		} else if (lower(value[0]) == 'r' && parse_uint(value + 1, 50, &v)) {
			c->portamento = (uint8_t)(51 + v);
		} else {
			why = "must be 0, t1-t50 or r0-r50: the default is used";
		}
		break;
	case 4:   /* bend_range: 0-24 */
		if (parse_uint(value, 24, &v)) {
			c->bend_range = (uint8_t)v;
		} else {
			why = "must be 0-24 semitones: the default is used";
		}
		break;
	case 5:   /* sustain: cc N, or off */
		take_cc(r, line, SP1_MIDI_WHO_SUSTAIN, key, value);
		return;
	case 6:   /* cc_smoothing: 0-200 ms */
		if (parse_uint(value, 200, &v)) {
			c->smooth_ms = (uint8_t)v;
		} else {
			why = "must be 0-200 (ms): the default is used";
		}
		break;
	case 7:   /* pickup */
		if (same(value, "sum")) {
			c->pickup = SP1_MIDI_PICKUP_SUM;
		} else if (same(value, "shared")) {
			c->pickup = SP1_MIDI_PICKUP_SHARED;
		} else if (same(value, "takeover")) {
			c->pickup = SP1_MIDI_PICKUP_TAKEOVER;
		} else {
			why = "must be sum, shared or takeover: the default is used";
		}
		break;
	case 8:   /* clock */
		if (same(value, "on")) {
			c->clock = 1u;
		} else if (same(value, "off")) {
			c->clock = 0u;
		} else {
			why = "must be on or off: the default is used";
		}
		break;
	default:  /* clock_lead: notes, auto, or 0-200 ms */
		if (same(value, "notes")) {
			c->clock_lead_ms = -2;
		} else if (same(value, "auto")) {
			c->clock_lead_ms = -1;
		} else if (parse_uint(value, 200, &v)) {
			c->clock_lead_ms = (int16_t)v;
		} else {
			why = "must be notes, auto or 0-200 (ms): the default is used";
		}
		break;
	}
	if (why != NULL) {
		midi_report(r, line, key_value(r->what, key, value), why);
	}
}

/* [bind]: velocity / aftertouch = <parameter> [depth%], or none. */
static void bind_section(struct sp1_midi_reader *r, int line, const char *key, const char *value)
{
	const int src = strcmp(key, "velocity") == 0 ? 0 : (strcmp(key, "aftertouch") == 0 ? 1 : -1);
	if (src < 0) {
		midi_report(r, line, key, "is not a [bind] source (velocity or aftertouch): ignored");
		return;
	}
	uint32_t seen = r->seen_bind;
	const bool first = once(r, line, key, &seen, 1u << src);
	r->seen_bind = (uint8_t)seen;
	if (!first) {
		return;
	}
	int dest = -1;
	int depth = 0;          /* none: depth 0, as gen_midi.py writes an unbound source */
	if (!same(value, "none")) {
		depth = 100;
		char name[SP1_CNFG_NAME_MAX];
		size_t n = 0u;
		const char *s = value;
		while (*s != '\0' && !space(*s) && n < sizeof(name) - 1u) {
			name[n++] = lower(*s++);
		}
		name[n] = '\0';
		bool ok = n > 0u && (*s == '\0' || space(*s));   /* the name ends at a space */
		while (space(*s)) {
			s++;
		}
		if (ok && *s != '\0') {
			/* "[-]N%" */
			char t[8];
			size_t k = 0u;
			const bool neg = (*s == '-');
			if (neg) {
				s++;
			}
			while (*s >= '0' && *s <= '9' && k < sizeof(t) - 1u) {
				t[k++] = *s++;
			}
			t[k] = '\0';
			while (space(*s)) {
				s++;
			}
			int pct = 0;
			ok = k > 0u && *s == '%' && s[1] == '\0' && parse_uint(t, 100, &pct);
			depth = neg ? -pct : pct;
		}
		for (int d = 0; ok && d < SP1_MIDI_DESTS && dest < 0; d++) {
			if (strcmp(dest_key(d), name) == 0) {
				dest = d;
			}
		}
		if (!ok || dest < 0) {
			midi_report(r, line, key_value(r->what, key, value),
				    "is not `<parameter> [depth%]` with a parameter from the lists above "
				    "and a depth of -100% to 100% (or none): the default is used");
			return;
		}
	}
	if (src == 0) {
		r->cfg->vel_dest = (int8_t)dest;
		r->cfg->vel_depth = (float)depth * 0.01f;
	} else {
		r->cfg->at_dest = (int8_t)dest;
		r->cfg->at_depth = (float)depth * 0.01f;
	}
}

static void midi_entry(void *ctx, int line, const char *section, const char *key,
		       const char *value)
{
	struct sp1_midi_reader *r = ctx;
	if (section[0] == '\0') {
		midi_report(r, line, key, "is not under a section: ignored");
		return;
	}
	if (strcmp(section, "midi") == 0) {
		midi_section(r, line, key, value);
		return;
	}
	if (strcmp(section, "bind") == 0) {
		bind_section(r, line, key, value);
		return;
	}
	if (strcmp(section, "plaits") == 0 || strcmp(section, "marbles") == 0) {
		for (int d = 0; d < SP1_MIDI_DESTS; d++) {
			if (dest_is(d, section, key)) {
				if (once(r, line, key, &r->seen_dest, 1u << d)) {
					take_cc(r, line, d, key, value);
				}
				return;
			}
		}
		struct msg m = { r->why, sizeof(r->why), 0u };
		msg_s(&m, "is not a parameter of [");
		msg_s(&m, section);
		msg_s(&m, "]: ignored");
		midi_report(r, line, key, r->why);
		return;
	}
	if (strcmp(section, r->bad_section) != 0) {
		strcpy(r->bad_section, section);
		midi_report(r, line, section, "is not a section of midi.cnfg (expected [midi], [plaits], "
			    "[marbles] or [bind]): its settings are ignored");
	}
}

void sp1_midi_reader_begin(struct sp1_midi_reader *r, struct sp1_midi_cfg *c,
			   sp1_cnfg_problem_fn problem, void *ctx)
{
	memset(r, 0, sizeof(*r));
	sp1_midi_cfg_defaults(c);
	r->cfg = c;
	r->problem = problem;
	r->ctx = ctx;
	for (int i = 0; i <= SP1_MIDI_DESTS; i++) {
		r->user_cc[i] = -2;
	}
	sp1_cnfg_begin(&r->p, midi_entry, midi_problem, r);
}

void sp1_midi_reader_feed(struct sp1_midi_reader *r, const char *text, size_t n)
{
	sp1_cnfg_feed(&r->p, text, n);
}

static const char *who_name(int who)
{
	return who == SP1_MIDI_WHO_SUSTAIN ? "the sustain pedal" : SP1_MIDI_NAME[who];
}

int sp1_midi_reader_end(struct sp1_midi_reader *r, int *notes)
{
	sp1_cnfg_end(&r->p);
	struct sp1_midi_cfg def;
	sp1_midi_cfg_defaults(&def);
	int8_t owner[128];
	int16_t owner_line[128];
	memset(owner, -1, sizeof(owner));
	memset(owner_line, 0, sizeof(owner_line));

	/* 1. Your lines, in file order: the first to name a CC keeps it. */
	for (int k = 0; k < r->n_order; k++) {
		const int who = r->order[k];
		const int cc = r->user_cc[who];
		if (cc < 0) {
			continue;
		}
		if (owner[cc] >= 0) {
			struct msg m = { r->why, sizeof(r->why), 0u };
			msg_s(&m, "CC ");
			msg_i(&m, cc);
			msg_s(&m, " is already given to ");
			msg_s(&m, who_name(owner[cc]));
			msg_s(&m, " on line ");
			msg_i(&m, owner_line[cc]);
			msg_s(&m, ": this one gets none");
			midi_report(r, r->user_line[who], who == SP1_MIDI_WHO_SUSTAIN ? "sustain"
				    : dest_key(who), r->why);
			r->user_cc[who] = -1;
			continue;
		}
		owner[cc] = (int8_t)who;
		owner_line[cc] = r->user_line[who];
	}

	/* 2. The defaults of everything you did not set: a CC one of your lines took is gone. */
	for (int who = 0; who <= SP1_MIDI_DESTS; who++) {
		if (r->user_cc[who] != -2) {
			continue;
		}
		const int cc = (who == SP1_MIDI_WHO_SUSTAIN) ? def.sustain_cc : def.dest_cc[who];
		if (cc < 0) {
			r->user_cc[who] = -1;
			continue;
		}
		if (owner[cc] >= 0) {
			struct msg m = { r->why, sizeof(r->why), 0u };
			msg_s(&m, "note: takes CC ");
			msg_i(&m, cc);
			msg_s(&m, " from ");
			msg_s(&m, who_name(who));
			msg_s(&m, ", which has no CC now");
			r->notes++;
			r->problem(r->ctx, owner_line[cc], who_name(owner[cc]), r->why);
			r->user_cc[who] = -1;
			continue;
		}
		owner[cc] = (int8_t)who;
		r->user_cc[who] = (int8_t)cc;
	}

	/* 3. The tables. */
	memset(r->cfg->cc_dest, -1, sizeof(r->cfg->cc_dest));
	for (int d = 0; d < SP1_MIDI_DESTS; d++) {
		r->cfg->dest_cc[d] = r->user_cc[d];
		if (r->user_cc[d] >= 0) {
			r->cfg->cc_dest[r->user_cc[d]] = (int8_t)d;
		}
	}
	r->cfg->sustain_cc = r->user_cc[SP1_MIDI_WHO_SUSTAIN];
	if (notes != NULL) {
		*notes = r->notes;
	}
	return r->problems;
}
