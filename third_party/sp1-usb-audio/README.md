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

Nothing here is compiled yet. M5c is "USB audio out": the SP-1's output, sent to the host. Nothing
in Wakes receives audio from the host.

## What is here, and what Wakes uses it for

| path | what it is | for Wakes |
|---|---|---|
| `usb-audio/uacring.{c,h}` | the ring buffer and the packet regulator: 47, 48 or 49 frames per 1 ms packet, following the drift between the SP-1's 3.072 MHz audio clock and the host's USB frames | **to be built.** Tuned upstream for 128- and 256-frame producer blocks; Wakes renders 96 (2 ms), so its constants need retuning (its host test fails at 96 as it stands) |
| `usb-audio/usb_audio.{c,h}` | glue between Zephyr's UAC2 class, the ring and the fast path, incl. stop on VBUS removed / bus reset and re-prime on resume | reference for M5c's glue; which class Wakes uses is still open (UAC1 or UAC2) |
| `test/test_uacring.c`, `test/run.sh` | host tests for the regulator | to be run by `tools/host-tests`, at 96 |
| `module/nrf-usbd-isofast/` | the ISO IN fast path: a patch against nRF Connect SDK's `udc_nrf.c` (`sdk-zephyr` `fd9204a0`), with that base kept as `patches/udc_nrf.c.upstream` | already ported to Zephyr 4.3.1 in `zephyr-patches/udc_nrf-fast-paths.patch` (`CONFIG_UDC_NRF_ISO_IN_FAST`, off). `udc_nrf.c.upstream` is the base for checking that port against 4.3.1's driver |
| `docs/` | how it works, stock driver vs fast path (measured on an SP-1), lessons from the bench | reference |
| `sp1/`, `extras/nrf52840dk/` | Ryan's complete SP-1 test firmware (a tone to speaker and USB) and a dev-board version | reference only; never built here |

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
`module/nrf-usbd-isofast/patches/` against `zephyr-patches/udc_nrf-fast-paths.patch`), rebuild,
run the host tests, update the commit above, and test on hardware.
