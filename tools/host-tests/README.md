# tools/host-tests — the host suites

Two test programs that run the **real firmware sources** on a desktop, with no Zephyr and no
hardware. They exist because most of this firmware's logic is ordinary C and C++ that can be
proved on a laptop, and because there is exactly one SP-1 (`docs/SAFETY.md`).

| | what it links | what it checks |
|---|---|---|
| `uitest.c` | `sp1_plaits_ui.c` + `sp1_marbles_ui.c`, with stubs | pages, layers, pickup, detents, rings, glyph uniqueness, scale stepping, the DEJA VU toggles, the V/Oct interlock, both rip wipes, `[J]`, Unpatch eating its own release (`sp1_release_guard.h`) |
| `routetest.cc` | all of Plaits and Marbles (with the overrides applied) + `sp1_synth.cc` + `sp1_marbles.cc` + both UI objects | routing, TRIG edges, the summing clamp, the HARMONICS attenuverter, the FREQUENCY quantizer's note sets, the soft-clip drive, the deferred re-seed, INTELLIGENT's per-channel ranges |
| `miditest.cc` | the same + `sp1_midi.cc`, against the shipped `config/midi.ini` | MIDI in (M5a): note priority and legato off, the LEVEL hand-back through a real LPG, sustain, 7- and 14-bit CCs, smoothing, pitch bend and RPN 0, channel filter, placement inside the audio block, queue overflow, disconnect to neutral, the quantizer bypass, stepped targets, Marbles offsets |
| `miditest_alt.cc` | the same, built against `midi-alt.ini` | omni, legato on, Yarns' portamento curve, velocity -> LEVEL, aftertouch -> TIMBRE, no sustain pedal, bend range |
| `third_party/feldd/test/test_usb_rt_parse.c` | feldd's packet validator | feldd's own test, unmodified |

```sh
tools/host-tests/hostbuild.sh routetest.cc uitest.c miditest.cc miditest_alt.cc \
    ../../third_party/feldd/test/test_usb_rt_parse.c          # prints each binary's path
cd /tmp/wakes-sp1-host && ./routetest && ./uitest && ./miditest && ./miditest_alt \
    && ./test_usb_rt_parse
```

Builds go to `$TMPDIR/wakes-sp1-host` (override with `SP1_HOST_BUILD_DIR`); nothing is written
into the repo. Every program exits non-zero on a failure, so they chain with `&&`.

## ⚠️ The harness must not be able to drift from the firmware

`mkovr.py` **parses `firmware/CMakeLists.txt`** and applies its real `sp1_plaits_override()`
patterns — the same "found exactly once or fail" rule, the same replacement text, the same
`CONFIG_*` values. `hostbuild.sh` then uses the firmware's include-path order (generated
overrides → `src/plaits_ovr` → `src/plaits_shim` → the eurorack root) and the firmware's Plaits
flags. Nothing about the overrides is restated by hand.

**This is not tidiness, it is the fix for a real failure.** M4c's INTELLIGENT section first
reported twelve failures with sentinel voltages. The firmware was correct and both configurations
built clean; the harness was compiling *malformed overrides* because an earlier build script did
its own CMake-string unescaping and dropped `\n`. A test harness that reimplements part of the
build can fail in ways that look exactly like a firmware bug, and it costs a long time to tell
apart. So:

- do not hand-copy an override into this directory;
- do not skip the archive's staleness check in `hostbuild.sh` (it watches every vendored source,
  our replacements -- headers included, since M5a changed `voice.h`'s layout -- `CMakeLists.txt`
  and `mkovr.py`) — a cached archive built from old overrides is
  the same failure by another route;
- `SP1_HOST_REBUILD=1` forces a full rebuild if you ever doubt it.

## ⚠️ `-DTEST` is required, and only for the host

`stmlib/dsp/dsp.h` uses the Cortex-M `ssat` / `usat` instructions for `Clip16` / `ClipU16` unless
`TEST` is defined. Without it the x86 assembler rejects them. It changes nothing the firmware
compiles.

## Warm-up, when a test measures Marbles' output range

Marbles applies a channel's `ScaleOffset` when a voltage is **generated**, not when it is read,
so for up to one clock period after a range change the channel still emits the previous range's
value — and with STEPS smooth the lag processor ramps between the two. `routetest`'s `settle()`
exists for that, and the comment above it explains why measuring from the first sample after
`sp1_marbles_set_params()` is wrong. The same one-tick lag is the hardware behaviour, so this is
worth understanding rather than working around.
