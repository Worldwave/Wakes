# tools/host-tests — the host suites

Two test programs that run the **real firmware sources** on a desktop, with no Zephyr and no
hardware. They exist because most of this firmware's logic is ordinary C and C++ that can be
proved on a laptop, and because there is exactly one SP-1 (`docs/SAFETY.md`).

| | what it links | what it checks |
|---|---|---|
| `uitest.c` | `sp1_plaits_ui.c` + `sp1_marbles_ui.c`, with stubs | pages, layers, pickup, detents, rings, glyph uniqueness, scale stepping, the DEJA VU toggles, the V/Oct interlock, both rip wipes, `[J]`, Unpatch eating its own release (`sp1_release_guard.h`) |
| `routetest.cc` | all of Plaits and Marbles (with the overrides applied) + `sp1_synth.cc` + `sp1_marbles.cc` + both UI objects | routing, TRIG edges, the summing clamp, the HARMONICS attenuverter, the FREQUENCY quantizer's note sets, the soft-clip drive, the deferred re-seed, INTELLIGENT's per-channel ranges |
| `miditest.cc` | the same + `sp1_midi.cc`, against the shipped `config/midi.ini` with its `pickup` set to `sum` | MIDI in (M5a): note priority and legato off, the LEVEL hand-back through a real LPG, sustain, 7- and 14-bit CCs read by polarity, the catch-up across an engine change, smoothing, pitch bend and RPN 0, channel filter, placement inside the audio block, queue overflow, disconnect to neutral, the quantizer bypass, stepped targets, Marbles offsets |
| `miditest_pickup.cc` | the same, against the shipped script with `pickup` set to `shared`, then `takeover`: two binaries, `miditest_pickup_shared` and `miditest_pickup_takeover` | a CC as a second hand on the fader's value: its first value only a reference, Plaits' catch-up both ways between CC and fader, a rip under a CC, a page not on show, Marbles not on show, MODEL still an offset, a disconnect leaving the values; takeover's resting faders and their catch-up after unplugging |
| `miditest_clock.cc` | the same as `miditest.cc`, with real cycle-counter stamps on every tick | MIDI clock and transport into Marbles (M5b): tempo and position from ticks, ±1 ms of USB jitter, a stopped clock waiting, Start arming beat 1 on its own Plaits block, Marbles' beats at the host's tempo, RATE as Marbles' ratio table (×4, ×1/4), Stop / Continue, a stalled clock, PLAY on the host's clock, and the cable pulled (Marbles stops, own clock again) |
| `miditest_alt.cc` | the same, built against `midi-alt.ini` (pickup `sum`) | omni, legato on, Yarns' portamento curve, velocity -> LEVEL, aftertouch -> TIMBRE, no sustain pedal, bend range |
| `third_party/feldd/test/test_usb_rt_parse.c` | feldd's packet validator | feldd's own test, unmodified |
| `uactest.c` | Ryan Gilmore's USB audio ring and packet regulator (`third_party/sp1-usb-audio`, unmodified) at Wakes' tuning, `firmware/src/sp1_uac_tuning.h` | USB audio out (M5c): ±200 ppm of clock drift, a heavy patch starting and stopping, a 200 % block every 997, with no underflow, no lost frame, a margin left in the ring, at most 20 packet-size corrections a second and none from load alone; priming, underflow, overflow |

```sh
tools/host-tests/hostbuild.sh routetest.cc uitest.c miditest.cc miditest_alt.cc \
    miditest_pickup.cc miditest_clock.cc ../../third_party/feldd/test/test_usb_rt_parse.c \
    uactest.c
cd /tmp/wakes-sp1-host && ./routetest && ./uitest && ./miditest && ./miditest_alt \
    && ./miditest_pickup_shared && ./miditest_pickup_takeover && ./miditest_clock \
    && ./test_usb_rt_parse && ./uactest
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

## Re-tuning USB audio (`uactest`)

The ring's target fill and dead band are tuned for the audio block, in
`firmware/src/sp1_uac_tuning.h`, which `#error`s if `CONFIG_SP1_AUDIO_BLOCK_FRAMES` changes.
To re-tune, change the block and the render times at the top of `uactest.c` if they changed,
then sweep: each run rebuilds `uactest` with one setting and prints a summary line.

```sh
for T in 288 304 320 336 352 368 384; do
  EXTRA_UAC_FLAGS="-DUACRING_TARGET=${T}u -DUACRING_HYSTERESIS=32u" \
      sh tools/host-tests/hostbuild.sh uactest.c >/dev/null && /tmp/wakes-sp1-host/uactest -s
done
```

Take the first target with `under 0` and add half a block (upstream's margin), then rebuild
without `EXTRA_UAC_FLAGS` and run the full `uactest`. The reasoning and the 96-frame sweep are
in the header.
