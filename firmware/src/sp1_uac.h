/*
 * Copyright (c) 2026 Adara Barami | Worldwave
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* wakes-sp1 -- USB audio OUT (M5c): the SP-1's output, sent to the host as a USB Audio
 * Class 1.0 input ("wakes-sp1", 48 kHz, 16-bit stereo). Nothing travels the other way.
 * See sp1_uac.c. Apache-2.0 because sp1_uac.c is derived from Ryan Gilmore's usb_audio.c. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The producer: sp1_audio.c's audio thread, once per block. claim() when the block starts
 * rendering (the regulator steers on it, sp1_uac_tuning.h), push() with the block it wrote
 * to I2S -- after VOL, the same samples the codecs get. Both cost a load and a branch while
 * no host has the stream open. */
void sp1_uac_claim(uint32_t frames);
void sp1_uac_push(const int16_t *lr, uint32_t frames);

/* USB bus events, from sp1_usbd.c's message callback (the usbd thread): a pulled cable or a
 * bus reset closes the stream (the class never hears alternate setting 0 then), a resume
 * re-primes it (the ring filled while the host slept). Takes enum usbd_msg_type. */
void sp1_uac_bus_event(int type);

/* The host has the stream open (alternate setting 1). */
bool sp1_uac_open(void);

/* ...and is actually taking packets: open, and one went out in the last 20 ms. While this is
 * true, sp1_audio.c mutes the speaker and the MIDI clock leads by the USB path's delay; a
 * host that opened the stream and then went to sleep gets the speaker back. Any thread. */
bool sp1_uac_live(void);

struct sp1_uac_stats {
	bool open;              /* the host has the stream open (alternate setting 1) */
	uint32_t opens;         /* times it was opened since boot */
	uint32_t packets;       /* packets sent since the stream opened */
	uint32_t n47, n49;      /* packet-size corrections since it opened */
	uint32_t under;         /* packets with no audio to send, since it opened */
	uint32_t over;          /* blocks the ring had no room for, since it opened */
	uint32_t fill;          /* frames in the ring now */
	uint32_t target;        /* the fill the regulator holds (sp1_uac_tuning.h) */
	uint32_t replaced;      /* driver: packets superseded before their DMA (since boot) */
	uint32_t sr_requests;   /* sample-rate requests from the host (since boot) */
};
void sp1_uac_get_stats(struct sp1_uac_stats *st);

#ifdef __cplusplus
}
#endif
