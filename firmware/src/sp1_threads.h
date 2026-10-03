/*
 * wakes-sp1 — CPU per thread, for the log (M5b diagnostics).
 *
 * `THREADS audio=72.0 usbd=0.3 udc_nrfx=0.4 main=6.9 sysworkq=0.0 idle=3.2 sleep=17.2 ...`:
 * each thread's share of WALL time since the last call, from Zephyr's thread runtime statistics
 * on the cycle counter; `sleep` = the CPU halted, which that counter does not see (sp1_threads.c). The USB stack is `usbd` + `udc_nrfx`. Interrupts count in the thread
 * they interrupted. The first call only takes a baseline. Main thread, once per status window.
 */
#ifndef SP1_THREADS_H
#define SP1_THREADS_H

void sp1_threads_report(void);

#endif /* SP1_THREADS_H */
