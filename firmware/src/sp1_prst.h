/*
 * wakes-sp1 — PRST presets: what a slot holds, and its file format (M6, #50).
 *
 * A slot is a `.prst` file of ini text on the eMMC (WAKES/PRST/1 .. 4, one file each; the
 * storage side is sp1_store.h). This file is the format alone: turning the patch into text
 * and text back into a patch. Pure C, no Zephyr, no storage -- the host suite
 * (tools/host-tests/prsttest.c) drives it with corrupt files that would be miserable to
 * make on the device.
 *
 * ---- what a slot holds (Adara, #50) ----
 * Everything except VOL: PLAITS' engine, every fader and attenuverter, the quantizer, the
 * output mode, the burst division and the drive; MARBLES' settings, ranges, routing and
 * BPM. NOT the clock's run/stop, the module or page on show (Wakes always comes up in
 * PLAITS with the clock stopped), or Marbles' seed and DEJA VU loop (too temporary).
 *
 * ---- the format ----
 * Sections and keys from ONE table, tools/gen_prst.py (generated into sp1_prst_gen.h):
 *
 *     [wakes]
 *     format = 1
 *     [plaits]
 *     engine = virtual analog
 *     timbre = 0.5000
 *     ...
 *
 * Fader values are positions 0..1 with four decimals; choices are lowercase tokens; the
 * engine is its NAME from config/engines.csv (Adara: by name, so a slot survives an edited
 * engine list). Reading ignores case, blank lines and lines starting ';' or '#'. A key the
 * table does not know is ignored, so is a section. If a key appears twice, the last wins.
 *
 * ---- the field check (Adara, #50) ----
 * "If a save file's field contains an impossible parameter -- NaN or a corrupt value -- load
 * the ROTC default for that parameter instead to prevent crashing or bootloops." So every
 * field is checked on its own and the rest of the file still loads:
 *   - a fader value must be a plain decimal number in 0..1 (no NaN, no infinity, no
 *     exponent, nothing after it);
 *   - a choice must be one of its tokens, and pass its check (an included scale; a
 *     destination on that output's ring);
 *   - the engine must name a filled slot of config/engines.csv; otherwise slot 1;
 *   - a missing key takes its default too.
 * The ROTC default comes from the rip code itself (sp1_pui_rip_patch, sp1_mui_rip_patch),
 * never from a second table -- and for a BASE fader it is the neutral value of the engine
 * that was LOADED (Adara: what a rip on that engine gives).
 * A format version this firmware does not know, or none at all, loads the WHOLE slot from
 * defaults: a newer format's keys could mean something else here.
 */
#ifndef SP1_PRST_H
#define SP1_PRST_H

#include <stdbool.h>
#include <stddef.h>

#include "sp1_plaits_ui.h"
#include "sp1_marbles_ui.h"

/* main.c's part of the PLAITS patch, at their ROTC defaults (docs/DEFAULTS.md). main.c's rip
 * uses these too, so there is one set. */
#define SP1_PRST_DEF_OUT    SP1_OUT_MAIN
#define SP1_PRST_DEF_BURST  5u        /* 1/32: g_burst_div, 1/(1 << n)  */
#define SP1_PRST_DEF_DRIVE  0         /* off                            */

struct sp1_prst {
	struct sp1_pui_patch plaits;
	int out_mode;                 /* enum sp1_synth_output                         */
	int burst_div;                /* 0..7: the FFWD burst at 1/(1 << burst_div)    */
	int drive;                    /* 0..SP1_DRIVE_STEPS-1                          */
	struct sp1_mui_patch marbles;
};

/* The ROTC default patch -- what ROTC on both modules gives -- with PLAITS' BASE at the
 * neutral state of `slot`'s engine (slot 1 if that slot is empty). wakes-sp1-fresh writes
 * this, for slot 1, into all four slots. */
void sp1_prst_rip(struct sp1_prst *p, int slot);

/* The patch as text, into buf (NUL-terminated). Returns its length, or -1 if `size` is too
 * small. SP1_PRST_TEXT_MAX always fits. */
#define SP1_PRST_TEXT_MAX 2048
int sp1_prst_write(const struct sp1_prst *p, char *buf, size_t size);

/* Text (need not be NUL-terminated) back into a patch, every field checked. ALWAYS leaves
 * a complete, safe patch in *out, whatever the text was. */
struct sp1_prst_report {
	int format;                   /* the file's format, -1 if it had none          */
	bool format_ok;               /* false: the whole slot is the defaults          */
	int defaulted;                /* fields that took their default (missing + bad) */
	int bad;                      /* ...of which present but rejected               */
	const char *first_bad;        /* key of the first rejected field, or NULL       */
};
void sp1_prst_parse(const char *text, size_t len, struct sp1_prst *out,
		    struct sp1_prst_report *rep);

/* How many fields a slot file has (tools/gen_prst.py), for the log and the tests. */
int sp1_prst_fields(void);
/* The format this firmware writes and reads (tools/gen_prst.py). */
int sp1_prst_format(void);

#endif /* SP1_PRST_H */
