/* wdt.h — the hardware watchdog the TE bootloader leaves running.
 *
 * The bootloader starts WDT0 (~5 s) before jumping to the app, and a running
 * nRF52840 WDT cannot be stopped or reconfigured. An app that never reloads it
 * resets every 5 s.
 *
 * Both functions are feldd's (main.c), which is hardware-proven; the looper
 * feeds the same way. */
/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

/* Start WDT0 at ~8 s only if nothing already did; a running one is left as the
 * bootloader configured it. Call first thing in main(). */
void wdt_ensure_started(void);

/* Reload every channel. Main thread: once per loop pass. */
void feed_wdt(void);

/* Milliseconds since the last feed_wdt(). Any thread: the audio thread reads it
 * to notice main being starved (audio.c, the starvation guard). */
uint32_t wdt_ms_since_feed(void);
