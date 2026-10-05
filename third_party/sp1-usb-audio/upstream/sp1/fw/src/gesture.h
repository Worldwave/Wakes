/* gesture.h — •• held -> power off.
 *
 * A •• press wakes through SYSTEM_OFF, and Track 1+4 held from off enters DFU.
 * Without a power-off, a firmware that feeds the watchdog can only be left by
 * draining the battery or by SWD. There is deliberately no in-firmware DFU
 * combo (Ryan, 2026-09-14): power off, then use the bootloader.
 *
 * Pure logic; power.c does the I/O. The hold is feldd's (~5 s, main.c),
 * rescaled from its ~8 ms tick to this firmware's 10 ms. */
/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GESTURE_TICK_MS      10
#define GESTURE_OFF_TICKS    500    /* 5 s */

struct gesture_state {
	int func_ticks;
	bool func_armed;   /* •• must read released once before a hold counts */
};

void gesture_init(struct gesture_state *s);

/* One tick. func_low: the •• pin reads pressed. True: power off now. */
bool gesture_step(struct gesture_state *s, bool func_low);

/* A •• combo press (•• + a button) is consumed and must not count toward the
 * power-off, as feldd's combo_dispatch resets its hold. Call every tick the
 * combo is held, before gesture_step: feldd resets only on the press edge, so a
 * long ••+T1 battery peek there would still power off at 5 s. */
void gesture_cancel(struct gesture_state *s);

/* ---- power-on hold ----
 * The SP-1 wakes on ANY •• press, and USB plug-in and the post-flash reset
 * reach the app too. A bump must not boot the firmware and drain the battery,
 * so every boot except a watchdog recovery waits for a 1.5 s •• hold first. On
 * battery, a release before then goes straight back to off; on USB it waits,
 * as feldd parks after a plug-in. The looper's M18 fix. */
#define WAKE_TICK_MS   20
#define WAKE_HOLD_MS   1500

enum wake { WAKE_WAIT, WAKE_BOOT, WAKE_OFF };

struct wake_state {
	int held_ms;
};

void wake_init(struct wake_state *s);

/* One WAKE_TICK_MS tick of the boot gate. */
enum wake wake_step(struct wake_state *s, bool func_low, bool usb_present);

/* ---- charge gauge, shown on the side row while waiting on USB ----
 * feldd's battery_pct() and charge_gauge() (main.c). Its calibration is marked
 * PROVISIONAL there: fitted against the stock curve, divider assumed ~1/2. */
#define BATT_RAW_EMPTY      1962
#define BATT_RAW_FULL       2378
#define GAUGE_BLINK_TICKS   24     /* feldd: 12 ticks of 40 ms; here 24 of 20 ms */
#define GAUGE_SAMPLE_TICKS  100    /* feldd re-reads ~every 2 s */

/* 0..100 from a raw AIN4 read, or -1 if the read failed. */
int battery_pct(int raw);

/* Bit i = side LED i+1. Charging: whole quarters solid, the next one blinking.
 * Not charging (charge complete): all four solid. */
unsigned charge_gauge_bits(int pct, bool charging, unsigned tick);

/* ---- master volume, on Vol+ / Vol- ----
 * The looper's (sp1-tape-looper main.c): a 16-step perceptual table, ~3 dB a
 * step, Q8 (256 = unity), booting at index 10 = 45 -- its proven-clean speaker
 * level; the TAS2505's small speaker distorts well below full scale. A press
 * steps once; holding repeats after 500 ms, then every 110 ms. Not saved. */
#define VOL_STEPS        15
#define VOL_DEFAULT_IDX  10
#define VOL_REPEAT_MS    500
#define VOL_EVERY_MS     110

struct vol_state {
	int idx;
	int dir;            /* the direction held last tick: -1, 0, +1 */
	uint32_t held_ms;   /* when that hold began */
	uint32_t last_ms;   /* when it last stepped */
};

void vol_init(struct vol_state *s);
/* One tick. dir: +1 Vol+ held, -1 Vol- held, 0 neither. True if idx changed. */
bool vol_step(struct vol_state *s, int dir, uint32_t now_ms);
uint16_t vol_q8(int idx);
