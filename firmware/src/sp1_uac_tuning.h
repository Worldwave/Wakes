/*
 * Copyright (c) 2026 Adara Barami | Worldwave
 *
 * SPDX-License-Identifier: MIT
 */
/* USB audio out (M5c): Wakes' tuning of Ryan Gilmore's ring and packet regulator
 * (third_party/sp1-usb-audio/upstream/usb-audio/uacring.{c,h}). Force-included ahead of
 * uacring.h wherever it is compiled -- the firmware and tools/host-tests/uactest.c -- so the
 * two cannot drift apart.
 *
 * Upstream tunes for 128- and 256-frame producer blocks; Wakes renders 96 (2 ms, one queued
 * I2S block: sp1_audio.c). Found by sweeping TARGET and HYSTERESIS against Wakes' own render
 * times (2026-10-05, uactest.c's model): light patch 50 % of the block, the heaviest held
 * patch 97.5 % (the worst block measured, v0.6.0 rc1), and one block in 997 at 4000 us --
 * 200 %, the most the one queued I2S block absorbs before the speaker itself runs dry. USB
 * audio must survive anything the speaker survives.
 *
 *   dead band +/-32: underflow at TARGET 320, first clean pass at 336 (lowest fill 7 frames)
 *   dead band +/-48: underflow at 336, first clean pass at 352
 *
 * The render time while held barely matters (the regulator steers on the CLAIMED position,
 * which the audio clock paces); the 4000 us block sets the target. 384 = the first pass plus
 * half a block, the margin upstream took at 128 (lowest fill 55 frames, ~1.1 ms). It is also
 * the ring's latency: 8.0 ms. Every passing setting made 0 corrections at 0 ppm with the
 * load stepping, and never more than 20 a second at +/-200 ppm. */
#pragma once

#if defined(CONFIG_SP1_AUDIO_BLOCK_FRAMES) && CONFIG_SP1_AUDIO_BLOCK_FRAMES != 96
#error "USB audio is tuned for 96-frame blocks: re-run tools/host-tests/uactest.c's sweep"
#endif

#define AUDIO_BLK_FRAMES    96
/* Overridable on the compiler's command line for uactest's sweep only (README there). */
#ifndef UACRING_TARGET
#define UACRING_TARGET      384u
#endif
#ifndef UACRING_HYSTERESIS
#define UACRING_HYSTERESIS  32u
#endif
