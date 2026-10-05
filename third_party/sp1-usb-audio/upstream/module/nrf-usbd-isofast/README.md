# nrf-usbd-isofast — an ISO IN fast path for Zephyr's nRF52840 USB driver

A copy of Zephyr's `drivers/usb/udc/udc_nrf.c` plus one patch of about 100 lines.
**Optional:** stock Zephyr sends USB audio fine. Use this only if you need the
CPU back.

## What it changes

- An opt-in call, `udc_nrf_iso_in_fast_set(fill, ctx, suppress_sof)`
  ([`src/udc_nrf_iso_in_fast.h`](src/udc_nrf_iso_in_fast.h)).
- At each start-of-frame, while the ISO IN endpoint is enabled, the driver's
  interrupt calls `fill` for the next packet and starts its DMA in the same
  interrupt, through the driver's existing DMA scheduler.
- No net_buf, no event, no thread wake-up for ISO IN. Optionally no SOF event to
  the stack either, when no class needs one.
- Unchanged: enumeration, descriptors, alternate settings, every other endpoint,
  and the driver's behaviour when no callback is registered.

## Measured on an SP-1 (64 MHz Cortex-M4, 48 kHz 16-bit stereo, macOS)

| | Zephyr's driver | With the fast path |
|---|---|---|
| USB threads' share of the CPU, stream open | usbd 11% + udc_nrfx 7% | 0 + 0 |
| Six synth voices + stream, repeated chords | resets | no reset |
| Stream integrity | clean until overload | clean |

## When stock is better

The full comparison, with measurements and the trade-offs of each, is
[`../../docs/STOCK-OR-FAST-PATH.md`](../../docs/STOCK-OR-FAST-PATH.md). In short:

1. There is no ISO IN audio. MIDI, CDC and HID gain nothing.
2. CPU is not scarce.
3. The packet cannot come cheaply and safely from an interrupt: `fill` runs at
   USBD interrupt priority every 1 ms and must be short, lock-free, non-blocking.
4. Another class needs SOF (ISO OUT, a feedback endpoint): most of the gain is
   suppressing the per-frame SOF wake-up.
5. You are on a different Zephyr. This is one file at one base; see below.

It does **not** lower latency.

## Base, and why it matters

- Forked from nRF Connect SDK's Zephyr (`sdk-zephyr`) at `fd9204a0`
  (NCS v3.3.0, Zephyr 4.3.99). The unmodified file is
  [`patches/udc_nrf.c.upstream`](patches/udc_nrf.c.upstream).
- [`tools/check.sh`](tools/check.sh) proves `src/udc_nrf.c` equals that file plus
  [`patches/udc_nrf-iso-in-fast.patch`](patches/udc_nrf-iso-in-fast.patch).
- **Upstream Zephyr v4.3.1's copy differs by ~330 lines**, including how it
  handles a DMA transfer still running at start-of-frame, which the fast path
  depends on. The patch applies there cleanly, and that is misleading: on another
  base it needs review and hardware testing, not just applying.

## Using it

1. `CONFIG_UDC_NRF=n`, `CONFIG_UDC_NRF_ISOFAST=y`, `CONFIG_UDC_ENABLE_SOF=y`.
2. `rsource` this directory's [`Kconfig`](Kconfig) from your app's Kconfig and
   `include()` its [`CMakeLists.txt`](CMakeLists.txt) from your app's.
3. Register once, before enabling USB. Do not also enqueue on the ISO IN endpoint
   through the UDC API. [`../../usb-audio/usb_audio.c`](../../usb-audio/usb_audio.c)
   shows both paths.

## Upstream

The plan is to offer this to Zephyr as an opt-in, Kconfig-gated API. Merged
there, the copy here goes away: you build stock Zephyr and register the callback.
The open question for Zephyr's USB maintainers is whether they want an
nRF-specific call like this one, or a generic hook in the USB device controller
API that other chips could implement too.
