/*
 * wakes-sp1 — the config files on the drive (#52): WAKES/CNFG/<name>.cnfg.
 *
 * Pure C, no Zephyr, no storage: the storage thread reads a file a chunk at a time and feeds
 * it here; the host suite (tools/host-tests/cnfgtest.c) feeds it broken files.
 *
 * ---- the rules (Adara, #52) ----
 *   - config/<name>.cnfg in the repository is the built-in DEFAULT, compiled into every
 *     image (tools/gen_<name>.py, which fails the build on a bad default). End users never
 *     touch it: they edit the copy on the drive, which every image writes there when it is
 *     missing (Adara: no trip to the repo, no fresh flash, to get a file back).
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

/* ---------------------------------------------------------------- audio.cnfg */

/* How sound leaves the device: USB audio out's level and the speaker while a host takes it.
 * The built-in values are config/audio.cnfg's (SP1_AUDIO_CFG_DEFAULTS, sp1_audio_gen.h, in
 * this field order); sp1_audio.c holds the live copy (sp1_audio_set_config). */
struct sp1_audio_cfg {
	uint8_t parked;          /* [usb] level: 1 parked, 0 vol                         */
	int8_t  parked_db;       /* [usb] parked_level, 0 .. -30 in 3 dB steps          */
	uint8_t parked_step;     /* ...as VOL's step, -parked_db / 3                    */
	uint8_t speaker;         /* [usb] speaker: 1 on while a host takes USB audio out */
};
void sp1_audio_cfg_defaults(struct sp1_audio_cfg *c);

/* Line by line over the built-in values, like logs.cnfg. */
struct sp1_audio_reader {
	struct sp1_cnfg_parser p;
	struct sp1_audio_cfg *cfg;
	sp1_cnfg_problem_fn problem;
	void *ctx;
	uint8_t seen;
	char  bad_section[SP1_CNFG_NAME_MAX];
	char  what[SP1_CNFG_LINE_MAX + 4];
	int   problems;
};
void sp1_audio_reader_begin(struct sp1_audio_reader *r, struct sp1_audio_cfg *c,
			    sp1_cnfg_problem_fn problem, void *ctx);
void sp1_audio_reader_feed(struct sp1_audio_reader *r, const char *text, size_t n);
int  sp1_audio_reader_end(struct sp1_audio_reader *r);

/* ---------------------------------------------------------------- midi.cnfg */

#include "sp1_midi_gen.h"        /* SP1_MIDI_DESTS and the destination enum */

/* The MIDI script. The built-in values are config/midi.cnfg's (SP1_MIDI_CFG_DEFAULTS,
 * sp1_midi_gen.h, in this field order); sp1_midi.cc holds the live copy
 * (sp1_midi_set_config), which every thread reads and only main writes, at ON entry with the
 * audio thread stopped. */
struct sp1_midi_cfg {
	int8_t   channel;        /* 0-15, or SP1_MIDI_OMNI                              */
	uint8_t  priority;       /* stmlib NoteStackFlags: 0 last, 1 low, 2 high        */
	uint8_t  legato;         /* Yarns legato_mode: 0 off, 1 auto, 2 on              */
	uint8_t  portamento;     /* Yarns: 0 none, 1-50 time, 51-101 rate               */
	uint8_t  bend_range;     /* semitones, until RPN 0 says otherwise               */
	int8_t   sustain_cc;     /* -1 = no sustain pedal                               */
	uint8_t  smooth_ms;      /* 0-200                                               */
	uint8_t  pickup;         /* SP1_MIDI_PICKUP_SUM / _SHARED / _TAKEOVER           */
	uint8_t  clock;          /* 1 = Marbles follows MIDI clock + transport          */
	int16_t  clock_lead_ms;  /* 0-200; -1 auto, -2 notes                            */
	int8_t   vel_dest;       /* [bind] velocity: a destination, or -1               */
	int8_t   at_dest;        /* [bind] aftertouch                                   */
	float    vel_depth;      /* -1 .. 1                                             */
	float    at_depth;
	int8_t   dest_cc[SP1_MIDI_DESTS];   /* destination -> its CC (coarse half), or -1 */
	int8_t   cc_dest[128];              /* CC -> destination, or -1                 */
};
void sp1_midi_cfg_defaults(struct sp1_midi_cfg *c);

/* Line by line over the built-in values, like logs.cnfg -- plus the CC rule (Adara, #52):
 *   - a CC you give a parameter (or the sustain pedal) is TAKEN from whichever parameter had
 *     it by default, which gets none (a note in the log, not a problem);
 *   - two of your lines on the same CC: the FIRST keeps it, the later gets none (a problem);
 *   - CC 33-63 are the fine halves of 1-31 and cannot be given; reserved CCs neither.
 * `velocity = none` / `aftertouch = none` remove a binding. */
#define SP1_MIDI_WHO_SUSTAIN SP1_MIDI_DESTS
struct sp1_midi_reader {
	struct sp1_cnfg_parser p;
	struct sp1_midi_cfg *cfg;
	sp1_cnfg_problem_fn problem;
	void *ctx;
	uint16_t seen_midi;                      /* [midi] keys set (the first wins)       */
	uint32_t seen_dest;                      /* destinations set                       */
	uint8_t  seen_bind;
	int8_t   user_cc[SP1_MIDI_DESTS + 1];    /* -2 not given, -1 none, else the CC;    */
	int16_t  user_line[SP1_MIDI_DESTS + 1];  /*   index SP1_MIDI_WHO_SUSTAIN = sustain */
	uint8_t  order[SP1_MIDI_DESTS + 1];      /* the CC lines in file order             */
	uint8_t  n_order;
	char  bad_section[SP1_CNFG_NAME_MAX];
	char  what[SP1_CNFG_LINE_MAX + 4];
	char  why[160];
	int   problems;
	int   notes;                             /* defaults that gave up their CC         */
};
void sp1_midi_reader_begin(struct sp1_midi_reader *r, struct sp1_midi_cfg *c,
			   sp1_cnfg_problem_fn problem, void *ctx);
void sp1_midi_reader_feed(struct sp1_midi_reader *r, const char *text, size_t n);
/* Problems; `*notes` = defaults that gave up their CC to one of your lines. */
int  sp1_midi_reader_end(struct sp1_midi_reader *r, int *notes);

#endif /* SP1_CNFG_H */
