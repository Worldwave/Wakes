/*
 * wakes-sp1 — the stored log's ring and the log file's cursor into it (#52).
 *
 * Pure C, no Zephyr: sp1_logbuf.c owns the memory and the lock and calls these under it;
 * the host suite (tools/host-tests/cnfgtest.c) drives them through overruns and gaps that
 * would take hours to make on the device.
 *
 * Positions are ABSOLUTE: `total` counts every byte ever written (mod 2^32), and the file's
 * cursor and the STANDBY gap are values of it. Compared by difference, so the wrap at 4 GB
 * (about a year at a session's ~120 B/s) is harmless.
 *
 * ---- what the file gets ----
 * Whole lines, oldest first, from `frd` on. Not:
 *   - STANDBY (Adara, #52): sp1_logring_pause() at STANDBY entry and _resume() at ON entry
 *     mark a gap the cursor jumps over. Boot lines before the first STANDBY are kept.
 *   - Lines the ring overwrote before the file took them: counted in `lost` (bytes; the
 *     gap's own bytes are not counted), and the cursor moves to the next whole line.
 */
#ifndef SP1_LOGRING_H
#define SP1_LOGRING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct sp1_logring {
	uint32_t size;        /* bytes of data[]                                   */
	uint32_t head;        /* next byte written                                 */
	uint32_t len;         /* bytes held, <= size                               */
	uint32_t total;       /* bytes ever written                                */
	uint32_t frd;         /* the file's cursor: the next byte it takes         */
	uint32_t gap_from;    /* STANDBY's lines are [gap_from, gap_to)            */
	uint32_t gap_to;
	uint8_t  paused;      /* in STANDBY: nothing from gap_from on, yet         */
	uint8_t  gap;         /* a closed gap waits for the cursor                 */
	uint8_t  discard;     /* logging is off this ON: its lines are not owed    */
	uint8_t  pad;
};

/* An empty ring over `size` bytes; the cursor at the start. */
void sp1_logring_init(struct sp1_logring *r, uint32_t size);

/* A ring found in .noinit after a reset: true if every field is consistent (then the file's
 * cursor is clamped into what the ring still holds), false if it must be re-initialised. */
bool sp1_logring_valid(struct sp1_logring *r, uint32_t size);

void sp1_logring_put(struct sp1_logring *r, char *data, char c);

/* The oldest byte still held, as an index into data[]. */
uint32_t sp1_logring_oldest(const struct sp1_logring *r);

/* STANDBY entry / ON entry. A second pause before a resume keeps the first. */
void sp1_logring_pause(struct sp1_logring *r);
void sp1_logring_resume(struct sp1_logring *r);

/* Logging is off this ON: the file is not owed anything written so far, NOR anything written
 * until the ON ends -- the pause at STANDBY entry drops this ON's lines too, so they cannot
 * reach the next ON's file (found in Adara's test, 0007.LOGS, 2026-10-15). Until _keep(). */
void sp1_logring_skip(struct sp1_logring *r);
/* Logging is on this ON: from here the lines are owed to the file again. */
void sp1_logring_keep(struct sp1_logring *r);

/* The next line for the file into out[0..max), max >= 2: its length (with its '\n'), or 0
 * if no whole line is waiting. A line longer than max comes in max-byte pieces. A partial
 * line is taken only where a gap cuts it. `*lost` += bytes the ring overwrote first. */
size_t sp1_logring_take(struct sp1_logring *r, const char *data, char *out, size_t max,
			uint32_t *lost);
/* As _take, but only lines from before the last STANDBY began: what the previous ON still owes
 * its own file (its closing lines, or a 30 s backstop's tail) when this ON starts a new one.
 * 0 once the cursor reaches STANDBY; never crosses it. */
size_t sp1_logring_take_before_standby(struct sp1_logring *r, const char *data, char *out,
				       size_t max, uint32_t *lost);

#endif /* SP1_LOGRING_H */
