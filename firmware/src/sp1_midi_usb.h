/*
 * wakes-sp1 — the two calls feldd's USB-MIDI class makes into Wakes (M5a).
 *
 * third_party/feldd/src/usb_midi1.c is vendored unmodified; third_party/feldd/patches/
 * usb_midi1.patch makes its receive path and its enable/disable/suspend/resume callbacks
 * call these instead of feldd's own clock router and MIDI-thru. Implemented in sp1_usbd.c.
 *
 * Both run in Zephyr's usbd thread (cooperative, above the audio thread): short, never
 * blocking, no locks.
 */
#ifndef SP1_MIDI_USB_H
#define SP1_MIDI_USB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A received bulk-OUT buffer: whole 4-byte USB-MIDI 1.0 event packets, `len` bytes. */
void sp1_midi_usb_rx(const uint8_t *data, size_t len);

/* The host enabled (or resumed) our MIDI interface: up; disabled or suspended: down. */
void sp1_midi_usb_port(bool up);

#endif /* SP1_MIDI_USB_H */
