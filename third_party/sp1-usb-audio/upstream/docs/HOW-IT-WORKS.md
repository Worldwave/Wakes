# How USB audio out works on the SP-1

## The pieces, and where each comes from

| Piece | What it does | From | Licence |
|---|---|---|---|
| UAC2 class | makes the device a USB audio device: descriptors, alternate settings, the ISO IN endpoint | Zephyr, `device_next` stack (`subsys/usb/device_next/class/usbd_uac2.c`), configured by devicetree ([`sp1/fw/app.overlay`](../sp1/fw/app.overlay)) | Apache-2.0 |
| nRF USB driver | moves packets over the nRF52840's USB hardware | Zephyr, `drivers/usb/udc/udc_nrf.c`, written by Nordic | Apache-2.0 |
| ring + regulator | holds rendered audio; sizes each 1 ms packet 47/48/49 frames | this repo, [`usb-audio/uacring.c`](../usb-audio/uacring.c) | Apache-2.0 |
| glue | connects the class's callbacks to the ring | [`usb-audio/usb_audio.c`](../usb-audio/usb_audio.c) | Apache-2.0 |
| fast path (optional) | sends each packet from the driver's interrupt | [`module/nrf-usbd-isofast/`](../module/nrf-usbd-isofast/README.md) | Apache-2.0 |

## The stream

- One UAC2 function: a clock source fixed at 48 kHz, an input terminal
  ("synthesizer"), a USB streaming output terminal, and one audio-streaming
  interface with 2-byte subslots, 16-bit. The host sees an audio INPUT.
- No ISO OUT and no feedback endpoint. So there is no way for the device to tell
  the host its real rate — the device adjusts packet sizes instead
  ("asynchronous" by packet size).
- The host opens the stream by selecting the interface's alternate setting 1;
  the class calls `terminal_update_cb`, which starts the ring priming.

## Why packets are 47, 48 or 49 frames

There are two clocks. On the SP-1 a 3.072 MHz oscillator paces the audio; the
computer paces USB frames, one per millisecond. They never agree exactly, so
sending exactly 48 frames every millisecond would slowly overfill or drain the
ring. Instead the device watches how full the ring is and now and then sends one
frame more or less.

- The ring's average fill is tracked with ~1 s smoothing. Outside a dead band
  around the target it sends one 47- or 49-frame packet, at most one per 50
  packets: 20 frames a second, ~417 ppm, about twice the worst crystal.
- Target fill with 128-frame producer blocks: 416 frames, ~8.7 ms.
- It steers on the **claimed** position — where the audio thread took its I2S
  slot — not on when rendering finished, so a heavy chord does not look like
  clock drift. See [`LESSONS.md`](LESSONS.md), "the chord-start click".

## Each 1 ms frame, stock path

1. The driver's start-of-frame interrupt posts an event (`CONFIG_UDC_ENABLE_SOF=y`
   is required for the nRF driver to schedule any ISO IN).
2. The class calls `sof_cb`, which pops a packet into a slab buffer and calls
   `usbd_uac2_send()`.
3. The driver sends it; the class returns the buffer through `buf_release_cb`.

That is four thread wake-ups per millisecond, ~18% of the SP-1's 64 MHz CPU. For
most firmware that is fine.

## Each 1 ms frame, with the fast path

1. At start-of-frame the patched driver's interrupt calls your fill callback,
   which pops the packet straight into a driver-owned buffer.
2. The same interrupt starts the DMA.
3. When it finishes, the driver just releases the DMA channel.

No buffer to allocate, no send call, no event, no thread wake-up. The glue can
also tell the driver to stop posting SOF events, since nothing else needs them.

## Stopping and restarting

- The class reports a stream stopping only when the host selects alternate
  setting 0. A pulled cable or a bus reset never does, so the glue also stops
  the stream on `USBD_MSG_VBUS_REMOVED` and `USBD_MSG_RESET`.
- On `USBD_MSG_RESUME` it re-primes, because the ring filled while the host slept.
