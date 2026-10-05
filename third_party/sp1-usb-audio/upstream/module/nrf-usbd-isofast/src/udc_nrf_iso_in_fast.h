/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief nRF USBD isochronous IN fast path.
 *
 * Streaming isochronous IN through the UDC API costs, every 1 ms frame: a SOF
 * event to the usbd thread, an enqueue to the driver thread, a DMA-finished
 * event back to the driver thread, and a completion to the usbd thread -- four
 * thread wake-ups to move one packet. On an nRF52840 streaming 48 kHz stereo
 * 16-bit audio that measured ~18% of the 64 MHz CPU.
 *
 * With the fast path registered, the driver asks the application for the next
 * packet directly in the USBD interrupt at SOF, and DMAs it from its own buffer
 * in the same interrupt. No net_buf, no event, no thread wake-up.
 *
 * The class driver (e.g. UAC2) still owns the endpoint: enumeration, alternate
 * setting and enable/disable are unchanged, and the fast path only runs while
 * the ISO IN endpoint is enabled. While it is registered, do not enqueue buffers
 * on the ISO IN endpoint through the UDC API; they would not be sent.
 */

#ifndef UDC_NRF_ISO_IN_FAST_H_
#define UDC_NRF_ISO_IN_FAST_H_

#include <stdbool.h>
#include <stdint.h>

/** Largest packet the fast path sends: the ISO IN half of the ISO buffer. */
#define UDC_NRF_ISO_IN_FAST_MAX 512

/**
 * @brief Produce the next ISO IN packet.
 *
 * Called from the USBD interrupt at every SOF while the fast path is registered
 * and the ISO IN endpoint is enabled. Must not block, and should be short: it
 * runs at interrupt priority, once a millisecond.
 *
 * @param buf       Write the packet here. Word aligned, in RAM.
 * @param max       Capacity of @p buf in bytes.
 * @param user_data As given to udc_nrf_iso_in_fast_set().
 *
 * @return Bytes to send this frame, at most @p max. 0 sends nothing: the host
 *         then gets no data (or a zero-length packet with NRF_USBD_ISO_IN_ZLP).
 */
typedef uint16_t (*udc_nrf_iso_in_fill_t)(uint8_t *buf, uint16_t max, void *user_data);

/**
 * @brief Register or remove the ISO IN fast path.
 *
 * @param fill         Packet producer, or NULL to return to the UDC API path.
 * @param user_data    Passed to @p fill.
 * @param suppress_sof Also stop posting UDC_EVT_SOF to the stack while the fast
 *                     path is active, saving the usbd thread wake-up every
 *                     frame. Only valid when no class needs SOF events -- for
 *                     example no ISO OUT or feedback endpoint in UAC2.
 */
void udc_nrf_iso_in_fast_set(udc_nrf_iso_in_fill_t fill, void *user_data, bool suppress_sof);

/** Counters, for diagnostics. */
struct udc_nrf_iso_in_fast_stats {
	uint32_t packets;   /**< packets DMA'd to the ISO IN buffer */
	uint32_t replaced;  /**< packets filled but not DMA'd before the next SOF */
	uint32_t empty;     /**< frames where @ref udc_nrf_iso_in_fill_t returned 0 */
};

void udc_nrf_iso_in_fast_stats_get(struct udc_nrf_iso_in_fast_stats *stats);

#endif /* UDC_NRF_ISO_IN_FAST_H_ */
