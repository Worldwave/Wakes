/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * UAC2 capture glue: 48 kHz, 16-bit stereo from the application to the host.
 */
#pragma once

#include <stdint.h>
#include <zephyr/usb/usbd.h>

void usb_audio_init(void);
/* Pass to sample_usbd_init_device(): stops the stream on a pulled cable or a
 * bus reset, and re-primes it on resume. */
void usb_audio_usbd_msg(struct usbd_context *const ctx, const struct usbd_msg *const msg);
/* The producer (one thread): optionally at the START of each block, the frames
 * it is about to render; then the rendered interleaved L/R frames. */
void usb_audio_claim(uint32_t frames);
void usb_audio_push(const int16_t *lr, uint32_t frames);
