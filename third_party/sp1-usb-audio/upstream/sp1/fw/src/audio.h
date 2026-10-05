/* SP-1 audio output: CS42L42 headphones + TAS2505 speaker, 48 kHz 16-bit stereo. */
/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdint.h>

#define AUDIO_SR         48000
/* 128 = 2.67 ms at 48 kHz: a new block plays after the ones queued, so latency
 * scales with block length. */
#define AUDIO_BLK_FRAMES 128          /* 2.67 ms at 48 kHz */

/* Fill `frames` interleaved stereo frames. Runs on the audio thread,
 * PREEMPT(0): no blocking, no I2C, no printk. */
typedef void (*audio_render_fn)(int16_t *lr, uint32_t frames);

void audio_start(audio_render_fn render);
/* Mute and reset both output chips, stop the oscillator. Before SYSTEM_OFF
 * only; main thread (it uses I2C). */
void audio_shutdown(void);

/* Master volume, Q8 (256 = unity). Main thread; the audio thread ramps to it
 * across one block, as the looper does, so a step never clicks. */
void audio_set_volume_q8(uint16_t q8);
/* Read the target volume for the audio thread's per-block ramp. */
uint16_t audio_volume_q8(void);

/* Headphone auto-mute of the speaker, the looper's: CS42L42 jack detect polled
 * every 4th call (~40 ms at the 10 ms tick), switching only after 3 equal reads;
 * a failed read holds. Main thread only -- the audio thread does no I2C. */
void audio_hp_poll(void);
