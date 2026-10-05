/* power.h — boot hygiene, the battery charger, and the •• power-off.
 * See gesture.h for why power-off is not optional. Main thread only. */
/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* First thing in main(), after the watchdog: clear RESETREAS and GPREGRET,
 * enable charging, configure the •• pin, then hold the boot until •• has been
 * held 1.5 s (gesture.h). Returns only to boot; a bump on battery never returns. */
void power_boot(void);

/* Every GESTURE_TICK_MS. Does not return once a power-off starts. */
void power_poll(void);
