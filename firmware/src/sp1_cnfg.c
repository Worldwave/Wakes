/* wakes-sp1 — the config files on the drive (#52). See sp1_cnfg.h. */
#include "sp1_cnfg.h"
#include "sp1_logs_gen.h"
#include "sp1_engines_gen.h"

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
