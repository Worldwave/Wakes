/* wakes-sp1 — the stored log's ring and the file's cursor. See sp1_logring.h. */
#include "sp1_logring.h"

#include <string.h>

/* a < b, positions compared by difference (sp1_logring.h). */
static bool before(uint32_t a, uint32_t b)
{
	return (int32_t)(a - b) < 0;
}

/* The index in data[] of absolute position p, which must be held. */
static uint32_t at(const struct sp1_logring *r, uint32_t p)
{
	return (r->head + r->size - (r->total - p)) % r->size;
}

void sp1_logring_init(struct sp1_logring *r, uint32_t size)
{
	memset(r, 0, sizeof(*r));
	r->size = size;
}

bool sp1_logring_valid(struct sp1_logring *r, uint32_t size)
{
	if (r->size != size || size == 0u || r->head >= size || r->len > size ||
	    r->paused > 1u || r->gap > 1u || r->discard > 1u) {
		return false;
	}
	/* A cursor ahead of the writer is nonsense: start the file at the oldest line held. */
	if (before(r->total, r->frd)) {
		r->frd = r->total - r->len;
	}
	if (r->gap && (before(r->gap_to, r->gap_from) || before(r->total, r->gap_to))) {
		r->gap = 0u;
	}
	if (r->paused && before(r->total, r->gap_from)) {
		r->paused = 0u;
	}
	return true;
}

void sp1_logring_put(struct sp1_logring *r, char *data, char c)
{
	data[r->head] = c;
	r->head = (r->head + 1u == r->size) ? 0u : r->head + 1u;
	if (r->len < r->size) {
		r->len++;
	}
	r->total++;
}

uint32_t sp1_logring_oldest(const struct sp1_logring *r)
{
	return (r->head + r->size - r->len) % r->size;
}

void sp1_logring_pause(struct sp1_logring *r)
{
	if (r->paused) {
		return;
	}
	/* An ON with logging off: none of its lines are owed, the ones since the skip included. */
	if (r->discard) {
		r->frd = r->total;
		r->gap = 0u;
	}
	/* A closed gap the cursor has not reached yet stretches to cover this one too: the
	 * lines between them (an ON whose log never reached the drive) go with it. */
	if (!r->gap) {
		r->gap_from = r->total;
	}
	r->gap = 0u;
	r->paused = 1u;
}

void sp1_logring_resume(struct sp1_logring *r)
{
	if (!r->paused) {
		return;
	}
	r->paused = 0u;
	r->gap_to = r->total;
	r->gap = (r->gap_to != r->gap_from) ? 1u : 0u;
}

void sp1_logring_skip(struct sp1_logring *r)
{
	r->frd = r->total;
	r->gap = 0u;
	r->discard = 1u;
	if (r->paused) {
		r->gap_from = r->total;
	}
}

void sp1_logring_keep(struct sp1_logring *r)
{
	r->discard = 0u;
}

static size_t take_lines(struct sp1_logring *r, const char *data, char *out, size_t max,
			 uint32_t *lost, bool before_standby);

size_t sp1_logring_take(struct sp1_logring *r, const char *data, char *out, size_t max,
			uint32_t *lost)
{
	return take_lines(r, data, out, max, lost, false);
}

size_t sp1_logring_take_before_standby(struct sp1_logring *r, const char *data, char *out,
				       size_t max, uint32_t *lost)
{
	return take_lines(r, data, out, max, lost, true);
}

static size_t take_lines(struct sp1_logring *r, const char *data, char *out, size_t max,
			 uint32_t *lost, bool before_standby)
{
	bool resync = false;

	/* 1. Overwritten before the file took it. The gap's own bytes are not a loss. */
	const uint32_t oldest = r->total - r->len;
	if (before(r->frd, oldest)) {
		uint32_t gone = oldest - r->frd;
		if (r->gap || r->paused) {
			const uint32_t gend = r->paused ? r->total : r->gap_to;
			const uint32_t lo = before(r->frd, r->gap_from) ? r->gap_from : r->frd;
			const uint32_t hi = before(oldest, gend) ? oldest : gend;
			if (before(lo, hi)) {
				gone -= hi - lo;
			}
		}
		*lost += gone;
		r->frd = oldest;
		resync = true;                       /* mid-line, most likely */
	}

	uint32_t limit = r->total;
	bool cut = false;
	if (before_standby) {
		/* Only what the last ON still owes ITS file: up to where STANDBY began, never
		 * across. gap_from marks that whether or not STANDBY printed anything (an empty
		 * STANDBY leaves gap_from == gap_to, and no gap to jump), and while it lasts. */
		if (!before(r->frd, r->gap_from)) {
			return 0u;
		}
		limit = r->gap_from;
		cut = true;
	} else {
		/* 2. Over a closed gap. Resume lands between lines, so no resync after it. */
		if (r->gap && !before(r->frd, r->gap_from)) {
			if (before(r->frd, r->gap_to)) {
				r->frd = r->gap_to;
				resync = false;
			}
			r->gap = 0u;
		}

		/* 3. How far the file may read: to the gap, if there is one ahead. While paused,
		 * everything from gap_from on is STANDBY's (the cursor waits there for the
		 * resume); after step 2 a closed gap is always ahead of the cursor. */
		if (r->paused) {
			if (!before(r->frd, r->gap_from)) {
				return 0u;
			}
			limit = r->gap_from;
			cut = true;
		} else if (r->gap) {
			limit = r->gap_from;
			cut = true;
		}
	}

	if (resync) {
		while (before(r->frd, limit) && data[at(r, r->frd)] != '\n') {
			r->frd++;
			(*lost)++;
		}
		if (!before(r->frd, limit)) {
			return 0u;
		}
		r->frd++;                            /* past the '\n' */
		(*lost)++;
	}
	if (!before(r->frd, limit)) {
		return 0u;
	}

	/* 4. One line, or a max-byte piece of a long one. */
	const uint32_t avail = limit - r->frd;
	const uint32_t scan = avail < max ? avail : (uint32_t)max;
	uint32_t i = at(r, r->frd);
	size_t n = 0u;
	for (uint32_t k = 0u; k < scan; k++) {
		if (data[i] == '\n') {
			n = (size_t)k + 1u;
			break;
		}
		i = (i + 1u == r->size) ? 0u : i + 1u;
	}
	if (n == 0u) {
		if (avail >= max) {
			n = max;
		} else if (cut) {
			n = avail;                   /* the gap cuts this line */
		} else {
			return 0u;                   /* still being written */
		}
	}
	i = at(r, r->frd);
	for (size_t k = 0u; k < n; k++) {
		out[k] = data[i];
		i = (i + 1u == r->size) ? 0u : i + 1u;
	}
	r->frd += (uint32_t)n;
	return n;
}
