/* wakes-sp1 — the stored log. See sp1_logbuf.h. */
#include "sp1_logbuf.h"

#include <zephyr/kernel.h>
#include <zephyr/linker/section_tags.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/printk-hooks.h>

#define LB_SIZE   ((uint32_t)CONFIG_SP1_LOGBUF_KB * 1024u)
#define LB_MAGIC  0x574B4C47u              /* "WKLG" */
#define STEP_MAX  512u                     /* bytes played back per control tick, then
					    * on to the end of that line             */
#define PACE      48u                      /* bytes per 1 ms: under CDC ACM's 64 B / frame */

BUILD_ASSERT(CONFIG_SP1_LOGBUF_KB >= 4, "the stored log needs at least 4 KB");

/* ---- the buffer: survives a soft reset if RAM does (sp1_logbuf.h) ---- */
static __noinit struct {
	uint32_t magic;
	uint32_t size;
	uint32_t head;                     /* next byte written                      */
	uint32_t len;                      /* bytes held, <= size                    */
	uint32_t boots;                    /* boots this log has run across          */
} hdr;
static __noinit char data[LB_SIZE];

static printk_hook_fn_t next;           /* the console's own printk output         */
static struct k_spinlock lock;
static bool line_start = true;
static volatile bool held;

/* ---- playback (main thread) ---- */
static bool     rp_on;
static uint32_t rp_pos, rp_left;

static void put(char c)
{
	data[hdr.head] = c;
	hdr.head = (hdr.head + 1u == LB_SIZE) ? 0u : hdr.head + 1u;
	if (hdr.len < LB_SIZE) {
		hdr.len++;
	}
}

static void put_str(const char *s)
{
	while (*s != '\0') {
		put(*s++);
	}
}

static void put_dec(uint32_t v, int digits)
{
	char b[10];
	for (int i = digits - 1; i >= 0; i--) {
		b[i] = (char)('0' + v % 10u);
		v /= 10u;
	}
	for (int i = 0; i < digits; i++) {
		put(b[i]);
	}
}

/* "[+hh:mm:ss.mmm] " -- by hand: a printk hook must not printk, and this runs per line. */
static void put_stamp(void)
{
	const uint32_t ms = k_uptime_get_32();
	const uint32_t s = ms / 1000u;
	put('[');
	put('+');
	put_dec(s / 3600u, 2);
	put(':');
	put_dec((s / 60u) % 60u, 2);
	put(':');
	put_dec(s % 60u, 2);
	put('.');
	put_dec(ms % 1000u, 3);
	put(']');
	put(' ');
}

static int hook(int c)
{
	if (!held) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		if (line_start && c != '\n') {
			put_stamp();
		}
		put((char)c);
		line_start = (c == '\n');
		k_spin_unlock(&lock, key);
	}
	return next != NULL ? next(c) : c;
}

void sp1_logbuf_init(uint32_t resetreas)
{
	const bool kept = hdr.magic == LB_MAGIC && hdr.size == LB_SIZE &&
			  hdr.head < LB_SIZE && hdr.len <= LB_SIZE;
	if (!kept) {
		hdr.magic = LB_MAGIC;
		hdr.size = LB_SIZE;
		hdr.head = 0u;
		hdr.len = 0u;
		hdr.boots = 0u;
	}
	hdr.boots++;
	if (kept && hdr.len > 0u) {
		char m[96];
		snprintk(m, sizeof(m),
			 "\n=== RESET (resetreas 0x%08x): the log above is the boot before; "
			 "boot %u starts here ===\n", resetreas, hdr.boots);
		put_str(m);
	}
	line_start = true;
	next = __printk_get_hook();
	__printk_hook_install(hook);
}

void sp1_logbuf_hold(bool hold)
{
	held = hold;
}

/* Straight to the console, never back into the buffer. */
static void out_str(const char *s)
{
	while (next != NULL && *s != '\0') {
		next(*s++);
	}
}

void sp1_logbuf_replay_start(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	rp_left = hdr.len;
	rp_pos = (hdr.head + LB_SIZE - hdr.len) % LB_SIZE;
	k_spin_unlock(&lock, key);
	char m[112];
	snprintk(m, sizeof(m), "\n=== STORED LOG: %u bytes, oldest first, [+uptime] per line; "
		 "live output resumes after the end marker ===\n", rp_left);
	out_str(m);
	rp_on = true;
}

void sp1_logbuf_replay_stop(void)
{
	if (rp_on) {
		rp_on = false;
		out_str("\n=== STORED LOG: stopped ===\n");
	}
}

bool sp1_logbuf_replaying(void)
{
	return rp_on;
}

void sp1_logbuf_replay_step(void)
{
	if (!rp_on || next == NULL) {
		return;
	}
	uint32_t n = 0u;
	char c = 0;
	/* Up to STEP_MAX bytes, then on to the end of the line, so live lines printed between
	 * steps never land in the middle of a stored one. */
	while (rp_left > 0u && (n < STEP_MAX || c != '\n')) {
		c = data[rp_pos];
		rp_pos = (rp_pos + 1u == LB_SIZE) ? 0u : rp_pos + 1u;
		rp_left--;
		/* A kept log is not checksummed: show anything that is not text as '?'. UTF-8
		 * bytes (>= 0x80) pass through. */
		if (((unsigned char)c < 0x20u && c != '\n' && c != '\t') || c == 0x7f) {
			c = '?';
		}
		next(c);
		if (++n % PACE == 0u) {
			k_msleep(1);
		}
	}
	if (rp_left == 0u) {
		rp_on = false;
		out_str("=== STORED LOG: end ===\n");
	}
}
