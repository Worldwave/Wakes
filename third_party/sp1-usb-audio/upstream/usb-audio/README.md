# usb-audio — the USB audio code

The four files that send audio to the computer. Both firmwares in this repo build
these same files: [`../sp1/`](../sp1/README.md) and
[`../extras/nrf52840dk/`](../extras/nrf52840dk/README.md).

| File | What it does |
|---|---|
| [`usb_audio.c`](usb_audio.c), [`usb_audio.h`](usb_audio.h) | connects Zephyr's UAC2 class to the ring: starts and stops the stream, sends one packet per USB frame (stock driver) or hands it to the fast-path driver |
| [`uacring.c`](uacring.c), [`uacring.h`](uacring.h) | the ring buffer and the packet regulator that sizes each packet 47, 48 or 49 frames to follow clock drift |

To use them in your own firmware: call `usb_audio_init()` before `usbd_enable()`,
pass `usb_audio_usbd_msg` to `sample_usbd_init_device()`, and from your audio
thread call `usb_audio_claim(frames)` before rendering each block and
`usb_audio_push(lr, frames)` after. The details are in
[`../docs/ON-THE-SP-1.md`](../docs/ON-THE-SP-1.md) §3.

The regulator's host tests are in [`../test/`](../test/run.sh).
