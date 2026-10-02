/*
 * wakes-sp1 — the USB device: one context, our own identity, every function on it (M5a).
 *
 * Replaces Zephyr's sample helper (samples/subsys/usb/common/sample_usbd_init.c), which the
 * console used through M4e. That helper HARD-CODES Zephyr's shared test vendor ID 0x2fe3 --
 * fine for a sample, wrong for a firmware people install -- so the device's identity is ours
 * now: CONFIG_SP1_USB_VID / _PID / _MANUFACTURER / _PRODUCT in firmware/Kconfig.
 *
 * Functions on the device, registered from what is compiled in:
 *   CDC ACM     the console (sp1_console.c)                         always
 *   USB-MIDI    feldd's MIDI 1.0 class (third_party/feldd)          CONFIG_SP1_MIDI
 *
 * ⚠️ Every change to that list needs a NEW product ID. Hosts -- Windows especially -- cache a
 * device's descriptors by VID/PID and keep using the old set.
 *
 * It also listens for the bus events the class callbacks do not cover (a pulled cable, a bus
 * reset) and reports them to MIDI, which puts itself back to neutral (sp1_midi.h).
 */
#ifndef SP1_USBD_H
#define SP1_USBD_H

#include <stdbool.h>

/* Bring up the USB device and enable it. Returns 0 on success. Never waits for a host. */
int sp1_usbd_init(void);

/* A USB HOST has configured the device -- a computer, a phone, the OP-XY -- as opposed to
 * USB power alone (a wall charger never configures anything). From the host's SET_CONFIGURATION
 * until a bus reset, a configuration of 0, or the cable coming out. Stays true while the host
 * is asleep (suspended): it is still attached. Read from the control loop. */
bool sp1_usbd_host(void);

#endif /* SP1_USBD_H */
