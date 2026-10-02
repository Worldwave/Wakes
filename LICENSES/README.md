# Licence texts

Wakes' own code, tools and documentation are MIT, Copyright (c) 2026 Adara Barami | Worldwave —
see [`../LICENSE`](../LICENSE). This folder holds the full licence text of everything else in the
repository or in a built firmware image. [`../NOTICE`](../NOTICE) says what each one covers and
what was changed.

| File | Holder | Covers |
|---|---|---|
| `MIT-Mutable-Instruments.txt` | Emilie Gillet | `third_party/eurorack/` (Plaits, Marbles, stmlib), `firmware/src/plaits_ovr/`, `firmware/src/sp1_marbles_scales.inc`, the build-time copies made by `firmware/CMakeLists.txt`, and the adapted parts of `sp1_marbles.cc`, `sp1_plaits_ui.c`, `sp1_marbles_ui.c` and `sp1_midi.cc` (ported from Yarns) |
| `MIT-feldd.txt` | Benjamin Reece | `third_party/feldd/` (the USB-MIDI 1.0 class and its packet parser) and the build-time patched copy of `usb_midi1.c` |
| `MIT-sp1-tape-looper.txt` | chattock | the board definition, power-off and watchdog sequence, and codec bring-up adapted from chattock/sp1-tape-looper |
| `MIT-SP-1-dev.txt` | Tim Knapen | the SP-1 pin map and hardware documentation from timknapen/SP-1-dev |
| `MIT-sp1-midi.txt` | Eric Lewis | the original `stem_player` Zephyr board that the board definition descends from |
| `Apache-2.0.txt` | the Zephyr Project contributors | `boards/teenageengineering/stem_player/Kconfig.defconfig`, `Kconfig.stem_player` and `stem_player_defconfig` (inherited from Zephyr's board template), `zephyr-patches/`, `firmware/src/sp1_usbd.c` (derived from Zephyr's USB sample helper), and the Zephyr RTOS compiled into every firmware image |

A built firmware image also contains other Zephyr modules (CMSIS, Nordic's `hal_nordic`) under
their own licences. Release archives will carry a generated bill of materials listing them.
