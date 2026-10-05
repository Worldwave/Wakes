# extras/nrf52840dk — the USB side alone, on a Nordic dev board

**For an SP-1, use [`../../sp1/`](../../sp1/README.md).** This is the same USB
audio code with nothing SP-1-specific around it — no codec, power or LEDs — so it
is the shortest thing to read, and it runs on hardware you may already have. The
device appears on the host as a 48 kHz stereo audio input playing a 1 kHz sine at
-12 dBFS. It builds for any nRF52840 board with
Zephyr's USB device controller enabled. Built for `nrf52840dk/nrf52840`, both
driver paths, no warnings; not yet run on hardware.

## Build

Zephyr with the `device_next` USB stack (developed against nRF Connect SDK
v3.3.0's Zephyr, 4.3.99):

```sh
west build -b nrf52840dk/nrf52840 extras/nrf52840dk
# with the optional fast-path driver:
west build -b nrf52840dk/nrf52840 extras/nrf52840dk -- -DEXTRA_CONF_FILE=isofast.conf
```

On nRF Connect SDK, `west build` uses sysbuild by default and the image lands in
`build/nrf52840dk/zephyr/`; `--no-sysbuild` puts it in `build/zephyr/`.

Build from this repo's layout: it takes the shared code from `../../usb-audio`
and the module from `../../module/nrf-usbd-isofast`.

## Check it

Plug the board's nRF USB port into a computer. It appears as an audio input
called "UAC2 tone sample": on macOS in Audio MIDI Setup, on Linux in
`arecord -l`. Record from it in any audio app and you should hear a steady 1 kHz
tone.

## Not for an SP-1

**Do not flash this onto an SP-1.** The build refuses the `sp1` board on
purpose: it has no watchdog feed, so the SP-1's bootloader watchdog would reset
it every 5 s, and no power-off, so the unit could not easily get back to its
bootloader. [`../../sp1/`](../../sp1/README.md) is the SP-1 version, with both.

## Replace before shipping

The USB VID/PID are Zephyr's test IDs (`0x2fe3`/`0x0001`). Use your own.
