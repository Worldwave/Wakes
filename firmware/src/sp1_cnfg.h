/*
 * wakes-sp1 — the config files on the drive (#52): WAKES/CNFG/<name>.cnfg.
 *
 * Pure C, no Zephyr, no storage: the storage thread reads a file a chunk at a time and feeds
 * it here; the host suite (tools/host-tests/cnfgtest.c) feeds it broken files.
 *
 * ---- the rules (Adara, #52) ----
 *   - config/<name>.cnfg in the repository is the built-in DEFAULT, compiled into every
 *     image (tools/gen_<name>.py, which fails the build on a bad default). End users never
 *     touch it: they edit the copy on the drive, which wakes-sp1-fresh writes.
 *   - Read at EVERY ON, before PRST and before audio. A line overrides its default; a line
 *     left out keeps it; a line that cannot be read keeps it too, and the log names it.
 *   - The same key twice: the FIRST is used, the later one is reported.
 *   - Nothing in a config file touches the power path, the watchdog, the flash layout or
 *     the USB descriptors (docs/SAFETY.md).
 *
 * ---- the format: ini ----
 *   [section]          names ignore case
 *   key = value        keys ignore case; ; or # after whitespace starts a comment
 *   ; comment / # comment
 * A UTF-8 byte order mark is skipped. Lines are at most SP1_CNFG_LINE_MAX bytes.
 */
#ifndef SP1_CNFG_H
#define SP1_CNFG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SP1_CNFG_LINE_MAX  160
#define SP1_CNFG_NAME_MAX  24

/* A problem with one line: `what` is the line (or its key), `why` the reason and what Wakes
 * does instead. The firmware prints "CNFG <file> line N: <what> -- <why>". */
typedef void (*sp1_cnfg_problem_fn)(void *ctx, int line, const char *what, const char *why);

/* One `key = value` (section, key lowercased; value trimmed, comment removed). */
typedef void (*sp1_cnfg_entry_fn)(void *ctx, int line, const char *section, const char *key,
				  const char *value);

struct sp1_cnfg_parser {
	sp1_cnfg_entry_fn   entry;
	sp1_cnfg_problem_fn problem;
	void *ctx;
	char  section[SP1_CNFG_NAME_MAX];
	char  buf[SP1_CNFG_LINE_MAX + 1];
	size_t n;
	int   line;
	bool  overlong;
};

void sp1_cnfg_begin(struct sp1_cnfg_parser *p, sp1_cnfg_entry_fn entry,
		    sp1_cnfg_problem_fn problem, void *ctx);
void sp1_cnfg_feed(struct sp1_cnfg_parser *p, const char *text, size_t n);
void sp1_cnfg_end(struct sp1_cnfg_parser *p);

/* ---------------------------------------------------------------- logs.cnfg */

/* What goes into the log file besides the lines that always do. */
#define SP1_LOG_PERFORMANCE  0x01u
#define SP1_LOG_CONTROLS     0x02u
#define SP1_LOG_MIDI         0x04u
#define SP1_LOG_USB          0x08u
#define SP1_LOG_STORAGE      0x10u
#define SP1_LOG_ALL          0x1Fu

struct sp1_logs_cfg {
	bool    enabled;
	uint8_t cats;           /* SP1_LOG_* bits */
};

/* The built-in defaults, from config/logs.cnfg (sp1_logs_gen.h). */
void sp1_logs_defaults(struct sp1_logs_cfg *c);

/* Read logs.cnfg's text over the defaults already in *c. Returns the number of problems. */
struct sp1_logs_reader {
	struct sp1_cnfg_parser p;
	struct sp1_logs_cfg *cfg;
	sp1_cnfg_problem_fn problem;
	void *ctx;
	uint8_t seen;           /* keys already set (the first wins) */
	char  bad_section[SP1_CNFG_NAME_MAX];
	char  what[SP1_CNFG_LINE_MAX + 4];
	int   problems;
};
void sp1_logs_begin(struct sp1_logs_reader *r, struct sp1_logs_cfg *c,
		    sp1_cnfg_problem_fn problem, void *ctx);
void sp1_logs_feed(struct sp1_logs_reader *r, const char *text, size_t n);
int  sp1_logs_end(struct sp1_logs_reader *r);

/* The categories as words, for the log: "performance controls midi usb storage" or "none".
 * buf of at least 48 bytes. */
const char *sp1_logs_describe(uint8_t cats, char *buf, size_t size);

/* Which category a log line belongs to, from its tag -- the first word after the
 * "[+hh:mm:ss.mmm] " stamp (CPU, MIDI, STORE, ...). 0 = always written: startup, power,
 * battery, warnings, and any line without a known tag. */
uint8_t sp1_log_category(const char *line, size_t n);

/* ---------------------------------------------------------------- engines.cnfg */

/* Two engine names are the same engine when their letters and digits match, case ignored
 * (tools/gen_engines.py, name_key): "grain / formant", "grain-formant" and "Grain Formant"
 * are one engine -- so a PRST file saved under an older spelling still loads. `canon` is
 * NUL-terminated; `s` is `n` bytes. */
static inline bool sp1_engine_name_eq(const char *canon, const char *s, size_t n)
{
	size_t i = 0u;
	for (;;) {
		while (*canon != '\0' && !((*canon >= 'a' && *canon <= 'z') ||
					   (*canon >= 'A' && *canon <= 'Z') ||
					   (*canon >= '0' && *canon <= '9'))) {
			canon++;
		}
		while (i < n && !((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
				  (s[i] >= '0' && s[i] <= '9'))) {
			i++;
		}
		if (*canon == '\0' || i == n) {
			return *canon == '\0' && i == n;
		}
		const char a = (*canon >= 'A' && *canon <= 'Z') ? (char)(*canon - 'A' + 'a') : *canon;
		const char b = (s[i] >= 'A' && s[i] <= 'Z') ? (char)(s[i] - 'A' + 'a') : s[i];
		if (a != b) {
			return false;
		}
		canon++;
		i++;
	}
}

/* The stack: SP1_CNFG_SLOTS Plaits engine indices, SP1_CNFG_EMPTY for an empty slot (the
 * same values as sp1_engines_gen.h's SP1_ENGINE_DEFAULT_STACK; sp1_cnfg.c checks). */
#define SP1_CNFG_SLOTS 24
#define SP1_CNFG_EMPTY 255u

/* An engine of this build by name (sp1_engine_name_eq): its Plaits index, or -1. */
int sp1_engine_index(const char *name, size_t n);

/* The built-in stack (config/engines.cnfg at build time). */
void sp1_engines_defaults(uint8_t out[SP1_CNFG_SLOTS]);

/* Read engines.cnfg. The file is the WHOLE stack (Adara, #52): a slot it does not name is
 * empty. Unlike logs.cnfg, a stack is all or nothing -- half of one would play engines in
 * slots nobody chose -- so ANY problem (a name that is not an engine, an engine twice, a
 * slot outside 1-24, a slot twice, another section, no engine at all) makes the whole file
 * unusable and Wakes uses the built-in stack, every problem named in the log. */
struct sp1_engines_reader {
	struct sp1_cnfg_parser p;
	sp1_cnfg_problem_fn problem;
	void *ctx;
	uint8_t  stack[SP1_CNFG_SLOTS];
	uint32_t seen;          /* slots set, bit per slot             */
	uint32_t used;          /* engines used, bit per Plaits index  */
	char  bad_section[SP1_CNFG_NAME_MAX];
	char  what[SP1_CNFG_LINE_MAX + 4];
	int   problems;
	int   filled;
};
void sp1_engines_begin(struct sp1_engines_reader *r, sp1_cnfg_problem_fn problem, void *ctx);
void sp1_engines_feed(struct sp1_engines_reader *r, const char *text, size_t n);
/* The number of problems. `out` is the file's stack when there were none, otherwise the
 * built-in stack. `*filled` = engines in `out`. */
int  sp1_engines_end(struct sp1_engines_reader *r, uint8_t out[SP1_CNFG_SLOTS], int *filled);

#endif /* SP1_CNFG_H */
