/* gesture.c — see gesture.h. */
/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "gesture.h"

void gesture_init(struct gesture_state *s)
{
	s->func_ticks = 0;
	s->func_armed = false;
}

bool gesture_step(struct gesture_state *s, bool func_low)
{
	/* A •• already down at boot (the wake press, or a hold through a reset)
	 * must not start a power-off: feldd's func_armed rule. */
	if (!s->func_armed) {
		if (!func_low) s->func_armed = true;
		s->func_ticks = 0;
		return false;
	}
	if (!func_low) {
		s->func_ticks = 0;
		return false;
	}
	return ++s->func_ticks >= GESTURE_OFF_TICKS;
}

void gesture_cancel(struct gesture_state *s)
{
	s->func_ticks = 0;
}

void wake_init(struct wake_state *s)
{
	s->held_ms = 0;
}

enum wake wake_step(struct wake_state *s, bool func_low, bool usb_present)
{
	if (func_low) {
		s->held_ms += WAKE_TICK_MS;
		return s->held_ms >= WAKE_HOLD_MS ? WAKE_BOOT : WAKE_WAIT;
	}
	s->held_ms = 0;
	return usb_present ? WAKE_WAIT : WAKE_OFF;
}

int battery_pct(int raw)
{
	if (raw < 0) return -1;
	int pct = (raw - BATT_RAW_EMPTY) * 100 / (BATT_RAW_FULL - BATT_RAW_EMPTY);
	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	return pct;
}

unsigned charge_gauge_bits(int pct, bool charging, unsigned tick)
{
	if (!charging) return 0xFu;
	int n_full = pct < 0 ? 0 : pct / 25;
	if (n_full > 4) n_full = 4;
	unsigned bits = (1u << n_full) - 1u;
	if (n_full < 4 && ((tick / GAUGE_BLINK_TICKS) & 1u)) bits |= 1u << n_full;
	return bits;
}

static const uint16_t vol_table[VOL_STEPS + 1] = {
	0, 2, 3, 4, 6, 8, 11, 16, 23, 32, 45, 64, 90, 128, 181, 256,
};

void vol_init(struct vol_state *s)
{
	s->idx = VOL_DEFAULT_IDX;
	s->dir = 0;
	s->held_ms = s->last_ms = 0;
}

bool vol_step(struct vol_state *s, int dir, uint32_t now_ms)
{
	bool step = false;
	if (dir == 0) {
		s->dir = 0;
		return false;
	}
	if (dir != s->dir) {                       /* a new press: step now */
		s->dir = dir;
		s->held_ms = s->last_ms = now_ms;
		step = true;
	} else if (now_ms - s->held_ms >= VOL_REPEAT_MS &&
		   now_ms - s->last_ms >= VOL_EVERY_MS) {
		s->last_ms = now_ms;                   /* held: repeat */
		step = true;
	}
	if (!step) return false;
	int n = s->idx + dir;
	if (n < 0) n = 0;
	if (n > VOL_STEPS) n = VOL_STEPS;
	if (n == s->idx) return false;
	s->idx = n;
	return true;
}

uint16_t vol_q8(int idx)
{
	if (idx < 0) idx = 0;
	if (idx > VOL_STEPS) idx = VOL_STEPS;
	return vol_table[idx];
}
