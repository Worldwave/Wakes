/*
 * wakes-sp1 — the stored log: everything printk writes, kept in RAM until a terminal reads it.
 *
 * WHY: the console rides on the one USB-C port. While a USB host such as the OP-XY holds that
 * port, nobody reads the console and every line is lost -- exactly the sessions that most need
 * a log. So every character printk writes is also kept here, newest last, and when a terminal
 * next opens the console (tools/console-log.py, DTR rising) the console plays the stored log back
 * after the banner, then carries on live. Keep Wakes ON after the session, plug it into the PC,
 * start the logger: the session is in the file.
 *
 *   - CONFIG_SP1_LOGBUF_KB of RAM (96 KB: ~14 minutes at a session's ~120 bytes/s). When it is
 *     full the OLDEST lines go; the newest are always there.
 *   - Every stored line starts with the device's uptime, "[+hh:mm:ss.mmm]", because the
 *     logger's own timestamps on a played-back line are the time of the dump, not of the event.
 *   - It sits in .noinit, so a soft reset -- the watchdog, a fault, sp1_power_off with USB --
 *     does not clear it IF the bootloader leaves RAM alone (unverified): the log then carries
 *     on across the reset with a marker line, and the minutes before an unexplained reset are
 *     in it (issue #22). A cold start, or a reset that did clear RAM, starts a fresh log --
 *     the header is checked, nothing is trusted blindly. SYSTEM_OFF loses it, as off does.
 *   - Played back in paced chunks that end on a line, from the main thread: CDC ACM drops what
 *     it cannot send (sp1_console.c, "why anything long has to be paced"), and the loops must
 *     keep feeding the watchdog. A full buffer takes a few seconds; the controls answer a little
 *     slowly meanwhile.
 *
 * Nothing here is on the power path, and the audio thread never prints (rule since M2).
 */
#ifndef SP1_LOGBUF_H
#define SP1_LOGBUF_H

#include <stdbool.h>
#include <stdint.h>

/* Once, in main(), after the console is up and before anything else prints. Wraps printk's
 * output hook; `resetreas` goes into the marker line when an older log was kept. */
void sp1_logbuf_init(uint32_t resetreas);

/* Stop / resume storing (the console's re-printed banner is not stored twice). */
void sp1_logbuf_hold(bool hold);

/* Play the stored log back: start it (a terminal just attached), then a step per control
 * tick until it is done. Main thread only. */
void sp1_logbuf_replay_start(void);
void sp1_logbuf_replay_stop(void);
bool sp1_logbuf_replaying(void);
void sp1_logbuf_replay_step(void);

#endif /* SP1_LOGBUF_H */
