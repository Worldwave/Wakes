/*
 * wakes-sp1 — where the CPU goes, per thread (M5b diagnostics). See sp1_threads.h.
 *
 * The CPU line (sp1_audio.c) times the audio thread's sections with the cycle counter, from
 * the start of a block to its end -- so anything that PREEMPTS the audio thread is counted in
 * whichever section it lands in. The USB stack runs above audio: under a CC flood its cost
 * shows up as "engine" or "routing" and its real share is invisible (Adara's OP-XY session,
 * 2026-10-02: ~480 CCs/s, then the watchdog). Zephyr's thread runtime statistics, timed with
 * the same cycle counter (CONFIG_THREAD_RUNTIME_STATS_USE_TIMING_FUNCTIONS), split it out:
 * every thread's share of the last report window. Interrupt time is counted in the thread it
 * interrupted. Cost: a few cycles per thread switch.
 *
 * The shares are of WALL time, from the system clock. The cycle counter that times each
 * thread stops while the CPU sleeps, so the kernel's own total leaves the sleep out: divided
 * by that, idle read ~4 % where the CPU really slept ~13-17 % (Adara's logs, 2026-10-02: MIDI
 * tick spacing measured on the same counter came out 17 % short at 71 % load). `sleep` is the
 * rest of the window: the CPU halted, waiting for an interrupt.
 */
#include "sp1_threads.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#define MAX_THREADS 16

static struct {
	const struct k_thread *t;
	uint64_t cycles;          /* execution cycles at the last report */
	uint64_t delta;           /* ... in this window */
	bool     seen;
} tab[MAX_THREADS];

static void collect(const struct k_thread *t, void *user)
{
	ARG_UNUSED(user);
	k_thread_runtime_stats_t s;
	if (k_thread_runtime_stats_get((k_tid_t)t, &s) != 0) {
		return;
	}
	int free_slot = -1;
	for (int i = 0; i < MAX_THREADS; i++) {
		if (tab[i].t == t) {
			tab[i].delta = s.execution_cycles - tab[i].cycles;
			tab[i].cycles = s.execution_cycles;
			tab[i].seen = true;
			return;
		}
		if (tab[i].t == NULL && free_slot < 0) {
			free_slot = i;
		}
	}
	if (free_slot >= 0) {                 /* first sight: a baseline, no delta yet */
		tab[free_slot].t = t;
		tab[free_slot].cycles = s.execution_cycles;
		tab[free_slot].delta = 0u;
		tab[free_slot].seen = true;
	}
}

void sp1_threads_report(void)
{
	for (int i = 0; i < MAX_THREADS; i++) {
		tab[i].seen = false;
	}
	k_thread_foreach_unlocked(collect, NULL);

	for (int i = 0; i < MAX_THREADS; i++) {
		if (tab[i].t != NULL && !tab[i].seen) {
			tab[i].t = NULL;             /* the thread has gone */
		}
	}
	/* The window: wall time since last time, from the system clock (it never stops), in CPU
	 * cycles (64 MHz). What the threads ran is the kernel's total on the cycle counter; the
	 * difference is sleep. */
	static uint32_t wall_prev;
	static uint64_t ran_prev;
	static bool have_prev;
	k_thread_runtime_stats_t all;
	if (k_thread_runtime_stats_all_get(&all) != 0) {
		return;
	}
	const uint32_t wall_now = k_cycle_get_32();   /* wraps in 36 h: the delta does not care */
	const uint64_t total = (uint64_t)(uint32_t)(wall_now - wall_prev) * 64000000ull /
			       (uint64_t)CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC;
	const uint64_t ran = all.execution_cycles - ran_prev;
	const bool first = !have_prev;
	have_prev = true;
	wall_prev = wall_now;
	ran_prev = all.execution_cycles;
	if (first || total == 0u) {
		return;
	}
	char line[200];
	int n = snprintk(line, sizeof(line), "THREADS");
	for (int i = 0; i < MAX_THREADS && n < (int)sizeof(line) - 24; i++) {
		if (tab[i].t == NULL) {
			continue;
		}
		const char *name = k_thread_name_get((k_tid_t)tab[i].t);
		const uint32_t pm = (uint32_t)((tab[i].delta * 1000u + total / 2u) / total);
		n += snprintk(line + n, sizeof(line) - (size_t)n, " %s=%u.%u",
			      (name != NULL && name[0] != '\0') ? name : "?", pm / 10u, pm % 10u);
	}
	const uint64_t slept = total > ran ? total - ran : 0u;
	const uint32_t spm = (uint32_t)((slept * 1000u + total / 2u) / total);
	printk("%s sleep=%u.%u (%% of the window)\n", line, spm / 10u, spm % 10u);
}
