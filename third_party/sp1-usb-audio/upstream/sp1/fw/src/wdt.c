/* wdt.c — see wdt.h. */
/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wdt.h"

#include <nrfx.h>
#include <zephyr/kernel.h>

static volatile uint32_t last_feed_ms;

void wdt_ensure_started(void)
{
	/* If the bootloader ever hands over a STOPPED WDT, feeding is a silent
	 * no-op and there is no hang backstop at all; start one. */
	if (NRF_WDT->RUNSTATUS & WDT_RUNSTATUS_RUNSTATUS_Msk) return;
	NRF_WDT->CONFIG = (WDT_CONFIG_SLEEP_Run << WDT_CONFIG_SLEEP_Pos);
	NRF_WDT->CRV = 8u * 32768u - 1u;   /* ~8 s at 32.768 kHz */
	NRF_WDT->RREN = WDT_RREN_RR0_Msk;
	NRF_WDT->TASKS_START = 1u;
}

void feed_wdt(void)
{
	/* Writing a channel that is not enabled is harmless, and which ones the
	 * bootloader enabled is not ours to know. */
	for (int ch = 0; ch < 8; ch++) NRF_WDT->RR[ch] = WDT_RR_RR_Reload;
	last_feed_ms = k_uptime_get_32();
}

uint32_t wdt_ms_since_feed(void)
{
	return k_uptime_get_32() - last_feed_ms;
}
