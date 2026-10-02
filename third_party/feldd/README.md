# Vendored: feldd's USB-MIDI 1.0 device class (M5a)

**Upstream, unmodified.** Every file under `src/` and `test/` is a byte-for-byte copy. Do not
edit them. Our changes are `patches/usb_midi1.patch`, which the build applies to a copy in the
build directory (`firmware/CMakeLists.txt`, `tools/apply_patch.py`) — so an upstream update is
a copy plus a re-check of one patch, never a merge.

| | repository | commit |
|---|---|---|
| everything here | https://github.com/bnjreece/feldd-sp1-firmware | `b70a3a274b8d5c4b99d6ed1edb9f265d4588d967` (2026-07-20) |

Copyright (c) 2026 Benjamin Reece. MIT licence — `LICENSE`, which covers only feldd's own
code; none of feldd's third-party components (its Bluetooth radio image in particular) are
vendored here.

## What is here, and why

| file | upstream path | what we use it for |
|---|---|---|
| `src/usb_midi1.c` | `firmware/app/src/usb_midi1.c` | the USB-MIDI 1.0 class on Zephyr's `device_next` stack. **Patched** (below). |
| `src/usb_midi1.h` | `firmware/app/src/usb_midi1.h` | its header (`usb_midi1_send`, unused by Wakes for now) |
| `src/usb_rt_parse.c`, `.h` | `firmware/app/src/usb_rt_parse.*` | splits a 4-byte USB-MIDI packet into a channel message or a clock/transport byte, and rejects malformed packets. Unmodified. |
| `test/test_usb_rt_parse.c` | `firmware/test/test_usb_rt_parse.c` | its host test, run by `tools/host-tests` |

Why feldd's class and not Zephyr's own `usbd_midi2` (M5 plan, B2): in the pinned Zephyr 4.3.1
the MIDI 2.0 class's MIDI 1.0 alternate setting is not implemented, and it puts an Interface
Association Descriptor in front of the Audio function — which feldd found hard-faults the
Teenage Engineering OP-XY (firmware v1.1.18). feldd's class carries the fixes feldd made for
that host: no IAD on the MIDI function, the 9-byte audio-class endpoint descriptors that
strict hosts require, and a host-to-device endpoint (an output-only MIDI device also faults the
OP-XY).

## What the patch changes

`patches/usb_midi1.patch`, against `src/usb_midi1.c`:

- feldd's own consumers of incoming MIDI (its clock router, MIDI-thru to the TRS jack, its
  librarian) are not part of Wakes. The receive path hands each received buffer to
  `sp1_midi_usb_rx()` instead (`firmware/src/sp1_usbd.c`), which validates each packet with
  `usb_rt_parse.c` and queues it for the audio thread.
- `enable` / `disable` / `suspended` / `resumed` also report the port state to
  `sp1_midi_usb_port()`, which is how Wakes knows MIDI was plugged or unplugged.

Nothing else: the descriptors, the endpoint handling and the transmit path are feldd's.

## Updating

```sh
python tools/vendor/update_feldd.py <commit>
```

fetches the files listed above at `<commit>`, shows what changed upstream since the commit
recorded here, copies them in, and checks that the patch still applies (strictly — no fuzz).
Then review the diff, rebuild, run the host tests, update the commit above, and test on
hardware. Nothing changes until someone does that.
