# Vendored: Ryan Gilmore's sp1-usb-audio (M5c, USB audio out)

**Upstream, unmodified.** Everything under `upstream/` is the whole repository at the commit
below, byte for byte (`git archive`; the blobs were checked against upstream's). Do not edit it.
When Wakes needs a change to one of these files, it goes in a patch applied at build time to a
copy, as for `third_party/feldd/`.

| | repository | commit |
|---|---|---|
| `upstream/` | https://github.com/ryanmgilmore/sp1-usb-audio | `74f3c4401676c06488d309a2cecd250eaedf0ef8` |

Copyright (c) 2026 Ryan Gilmore. Apache-2.0 — `upstream/LICENSE`. `upstream/NOTICE` is carried
forward as Apache-2.0 asks: it credits Nordic Semiconductor for the driver in
`upstream/module/nrf-usbd-isofast/` and chattock (MIT) for the SP-1 audio bring-up in
`upstream/sp1/fw/src/audio.c`.

Not yet compiled into the firmware (the UAC1 class comes next); `uactest` builds the ring on the host. M5c is "USB audio out": the SP-1's output, sent to the host. Nothing
in Wakes receives audio from the host.

## What is here, and what Wakes uses it for

| path | what it is | for Wakes |
|---|---|---|
| `usb-audio/uacring.{c,h}` | the ring buffer and the packet regulator: 47, 48 or 49 frames per 1 ms packet, following the drift between the SP-1's 3.072 MHz audio clock and the host's USB frames | **used**, patched (below) and tuned for Wakes' 96-frame blocks in `firmware/src/sp1_uac_tuning.h` |
| `usb-audio/usb_audio.{c,h}` | glue between Zephyr's UAC2 class, the ring and the fast path, incl. stop on VBUS removed / bus reset and re-prime on resume | reference only: Wakes uses its own UAC1 class |
| `test/test_uacring.c`, `test/run.sh` | host tests for the regulator, on upstream's 128/256-frame bench | reference; it fails at 96 by design. Wakes' version with Wakes' render times is `tools/host-tests/uactest.c` |
| `module/nrf-usbd-isofast/` | the ISO IN fast path: a patch against nRF Connect SDK's `udc_nrf.c` (`sdk-zephyr` `fd9204a0`), with that base kept as `patches/udc_nrf.c.upstream` | already ported to Zephyr 4.3.1 in `zephyr-patches/udc_nrf-fast-paths.patch` (`CONFIG_UDC_NRF_ISO_IN_FAST`, off). `udc_nrf.c.upstream` is the base for checking that port against 4.3.1's driver |
| `docs/` | how it works, stock driver vs fast path (measured on an SP-1), lessons from the bench | reference |
| `sp1/`, `extras/nrf52840dk/` | Ryan's complete SP-1 test firmware (a tone to speaker and USB) and a dev-board version | reference only; never built here |

## What Wakes' patch changes

`patches/uacring.patch`, against `upstream/usb-audio/uacring.c`, applied STRICTLY to a copy at
build time (`tools/apply_patch.py`, as for feldd) and by `tools/host-tests/hostbuild.sh` for
`uactest`:

- `uacring_push` and `uacring_pop` copy one 32-bit word per stereo frame (`frames_copy`,
  `frames_zero`) instead of calling `memcpy` / `memset`. The SDK's picolibc copies one byte per
  loop pass, ~6.6 cycles a byte on the SP-1 (#36); at 576 bytes per 2 ms that was ~4 points of
  CPU, ~0.65 with the word copies (~1 cycle a byte, from the Cortex-M4 disassembly).
- The copy must be compiled with `-fno-tree-loop-distribute-patterns`, or GCC turns the loops
  back into library calls (checked: it does, for the silence fill). `tools/ci/check_image.py`
  fails an image where push or pop calls `memcpy`/`memset`.

Nothing else: the ring, the regulator and the priming are upstream's. Wakes' tuning for 96-frame
blocks is not a patch either; it is `firmware/src/sp1_uac_tuning.h`, force-included ahead of
`uacring.h`.

Where Wakes deliberately differs from upstream (M5-PLAN, section E):

- **USB audio follows VOL.** Upstream sends at a fixed −6 dB, independent of the volume.
- **The fast path is the only path.** Upstream treats it as optional; stock Zephyr costs ~18 % of
  the CPU, which Wakes does not have.

## Updating

From a clone of upstream:

```sh
git -C <clone> archive <commit> | tar -x -C third_party/sp1-usb-audio/upstream
```

after emptying `upstream/`. Then read the upstream diff since the commit above (in particular
`module/nrf-usbd-isofast/patches/` against `zephyr-patches/udc_nrf-fast-paths.patch`), check
that `patches/uacring.patch` still applies (the build stops if it does not), re-run `uactest`
and its sweep if the regulator changed, update the commit above, and test on hardware.
