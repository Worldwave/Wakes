# sp1-usb-audio

USB audio out for the Teenage Engineering SP-1: 48 kHz, 16-bit stereo from the
SP-1 to a computer, as a class-compliant USB audio input. No driver on the host.

This is for people writing their own firmware for the SP-1 on Zephyr. It
explains how USB audio out works, gives you the code that does it, and records
what went wrong along the way so you don't have to find it again. The code comes
from an SP-1 firmware that has shipped USB audio in its default build since
September 2026.

## Start here

[`sp1/`](sp1/README.md) is a complete SP-1 firmware: it plays a 1 kHz tone
through the speaker, at the master volume on Vol+/Vol-, and to the computer over
USB at a fixed level. It has everything an SP-1 firmware needs besides the audio:
power on and off, the watchdog, the charge gauge. Build it, flash it, and record
the tone on your computer — then copy the parts you need into your own firmware.
[`docs/ON-THE-SP-1.md`](docs/ON-THE-SP-1.md) walks through it.

## The short version

- **Stock Zephyr already does USB audio.** Its UAC2 class (`zephyr,uac2`) on the
  nRF52840's stock USB driver is all this needs. Nothing here has to be patched to
  get sound out.
- **What you do have to write** is the part that sizes packets: 47, 48 or 49
  frames per 1 ms, to follow the drift between the audio clock and the host's.
  [`docs/HOW-IT-WORKS.md`](docs/HOW-IT-WORKS.md).
- **The patched driver is optional.** It cuts the USB stack's CPU cost from ~18%
  to ~0, which matters only for firmware short of CPU. Which to use, and what
  each costs: [`docs/STOCK-OR-FAST-PATH.md`](docs/STOCK-OR-FAST-PATH.md).

## What is here

| | |
|---|---|
| [`sp1/`](sp1/README.md) | **the SP-1 firmware**: tone to speaker and USB, with power, watchdog and LEDs |
| [`usb-audio/`](usb-audio/README.md) | the USB audio code itself, shared by both firmwares: the glue and the packet regulator |
| [`module/nrf-usbd-isofast/`](module/nrf-usbd-isofast/README.md) | the optional fast-path driver for Zephyr's nRF USB driver |
| [`docs/ON-THE-SP-1.md`](docs/ON-THE-SP-1.md) | a walkthrough of the SP-1 firmware: clocking, the USB node, where the calls go |
| [`docs/STOCK-OR-FAST-PATH.md`](docs/STOCK-OR-FAST-PATH.md) | which driver to use, measured, with the trade-offs of each |
| [`docs/HOW-IT-WORKS.md`](docs/HOW-IT-WORKS.md) | the pieces and where each comes from |
| [`docs/LESSONS.md`](docs/LESSONS.md) | what went wrong on the way, and the fixes |
| [`test/`](test/run.sh) | host tests for the regulator (any C compiler) |
| [`extras/nrf52840dk/`](extras/nrf52840dk/README.md) | the same USB side on a Nordic dev board, with no SP-1 code around it — the smallest version to read |

## Where it has been tested

- **On an SP-1, with macOS:** the regulator, the glue and the fast-path driver,
  as part of a full firmware. Streams clean under full synth load; stops cleanly
  when the cable is pulled.
- **Not tried:** Windows, Linux, and resuming from a host that suspends without
  closing the stream.
- **The SP-1 firmware in `sp1/` has run on an SP-1** with both the stock driver
  and the fast path: power, tone, volume, headphones, a clean USB recording and
  power-off all checked on each. Not yet tried there: unplugging mid-stream.
- **The dev-board version** has been built with both drivers, not run.

## Licence

Apache-2.0 ([`LICENSE`](LICENSE)), as Zephyr. The driver in
`module/nrf-usbd-isofast/` is Nordic Semiconductor's, modified; see
[`NOTICE`](NOTICE).
