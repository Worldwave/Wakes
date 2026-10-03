/*
 * wakes-sp1 — CPU per thread, for the log (M5b diagnostics).
 *
 * `THREADS audio=78.1 usbd=1.2 udc_nrfx=0.4 main=3.0 sysworkq=0.1 idle=17.2 ...`: each thread's
 * share of the time since the last call, idle included, from Zephyr's thread runtime statistics
 * on the cycle counter. The USB stack is `usbd` + `udc_nrfx`. Interrupts count in the thread
 * they interrupted. The first call only takes a baseline. Main thread, once per status window.
 */
#ifndef SP1_THREADS_H
#define SP1_THREADS_H

void sp1_threads_report(void);

#endif /* SP1_THREADS_H */
