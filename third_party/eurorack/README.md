# Vendored: Mutable Instruments Plaits DSP, Marbles' generators, and the stmlib subset they need

**Upstream, unmodified.** Every file here is a byte-for-byte copy. Do not edit them —
override instead (see `firmware/src/plaits_shim/`), so an upstream update is a copy,
not a merge.

| | repository | commit |
|---|---|---|
| `plaits/` | https://github.com/pichenettes/eurorack | `08460a69a7e1f7a81c5a2abcc7189c9a6b7208d4` (2023-08-16) |
| `marbles/` (M4) | https://github.com/pichenettes/eurorack | the same commit |
| `stmlib/` | https://github.com/pichenettes/stmlib | `d18def816c51d1da0c108236928b2bbd25c17481` (2023-09-03) |

Copyright Emilie Gillet. MIT licence — per-file headers, `stmlib/LICENSE`, and the
eurorack README, whose licence section reads:

```
Code (AVR projects): GPL3.0.
Code (STM32F projects): MIT license.
Hardware: cc-by-sa-3.0
```

Plaits and Marbles are STM32F projects, so MIT. Nothing AVR is vendored.

## What is here, and what is not

Exactly the transitive closure of `plaits/dsp/voice.cc`, every engine, `plaits/resources.cc`
and the three stmlib sources they need (`atan.cc`, `units.cc`, `random.cc`) — computed
with `arm-zephyr-eabi-g++ -MM`, not by hand. Nothing from `plaits/` outside `dsp/` and
`resources.*`: the hardware drivers, UI and bootloader are STM32-specific and replaced by
our own firmware.

**Marbles (M4):** only the generators — `marbles/random/*`, `marbles/ramp/*` and
`marbles/resources.*`, the `-MM` closure of `t_generator.cc`, `x_y_generator.cc`,
`output_channel.cc`, `lag_processor.cc`, `quantizer.cc`, `discrete_distribution_quantizer.cc`,
`ramp_extractor.cc` and `resources.cc` — plus the two stmlib headers they add
(`utils/gate_flags.h`, `utils/ring_buffer.h`). Not vendored: `marbles.cc`, `ui.cc`,
`settings.cc`, `cv_reader*`, `clock_inputs`, the self-patching detector, `scale_recorder`,
`note_filter`, `io_buffer`, `drivers/`. `firmware/src/sp1_marbles.cc` does what `marbles.cc`
does for the generators; the six preset scales are copied from `settings.cc` into
`firmware/src/sp1_marbles_scales.inc`. `ramp_extractor.cc` (the external clock) is compiled
but never called.

**MIDI (M5a):** `stmlib/algorithms/note_stack.h`, from the same stmlib commit — the note
stack Yarns uses for monophonic note priority. Nothing from `yarns/` is vendored: its `Part`
and `Voice` are entangled with its settings, sequencer, arpeggiator, DAC and UI, so the ~100
lines Wakes needs (the 1M mono branch of `Part::InternalNoteOn/Off`, and `Voice::Refresh`'s
portamento and pitch bend) are PORTED into `firmware/src/sp1_midi.cc`, attributed there and
in NOTICE, from `yarns/part.cc` and `yarns/voice.cc` at the eurorack commit above.

⚠️ **`plaits/user_data.h` is deliberately absent.** Upstream's version includes STM32
headers and reads a raw flash address (`0x08007000`) that does not exist on the nRF52840 —
reading it would be a bus fault. `firmware/src/plaits_shim/plaits/user_data.h` replaces it
(no user wavetables / 6-op banks; the built-in ones are used). The shim directory is first on
the include path, which is what makes the override work.

## Updating

Re-clone at a new commit, recompute the closure with `-MM`, copy, update the commits above,
re-run `tools/plaits-bench`, and re-measure on hardware.

## Naming — upstream's derivative-works guideline

"Mutable Instruments is a registered trademark. The name "Mutable Instruments" should not
be used on any of the derivative works you create from these files." It also recommends not
keeping the original module name. So the firmware is **Wakes** (`wakes-sp1`); "Plaits" and "Mutable
Instruments" appear only as attribution — in NOTICE, here, and in comments — never as
the product's name, a boot banner, or a release title.
