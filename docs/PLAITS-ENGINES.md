# Plaits engines — choosing them, glyphs, detents

**Which engine sits in which slot is set in [`config/engines.csv`](../config/engines.csv)**, not in
this document. The build reads that file every time. This page is reference: what each engine is,
what it costs, and what each fader does on it.

## Choosing engines: `config/engines.csv`

The file has one row per slot, 24 at most, in slot order. T2/T3 step through the slots in that
order. The device starts on the first slot that has an engine, and "rip out the cables" returns
there.

| column | what it is | can you change it? |
|---|---|---|
| **Slot** | the slot number. It's the row's position, printed so the file reads like the device. | No |
| **Glyph** | the four-LED pattern flashed when the slot is selected, T1 first. It belongs to the slot, not to the engine in it. | No |
| **Engine** | the engine in that slot, named exactly as under "Available engines" below (upper/lower case doesn't matter). **Leave it empty** for an empty slot, which T2/T3 skip. | **Yes** |

- **To remove an engine**, clear its Engine cell. The slot stays and is skipped.
- **To add one**, type its name into an empty slot's Engine cell.
- **To reorder**, move the engine names between rows. Don't move whole rows: the Slot and Glyph
  columns have to stay where they are.

Each engine can appear once. Open the file in a spreadsheet or a text editor; GitHub shows it as
a table. If the file is wrong, for example a glyph edited or a whole row moved, the build stops
with a message naming the line and saying what's expected there.
`python tools/gen_engines.py --list` prints every engine name the file may use.

Leaving an engine out removes it from engine select. It does **not** make the firmware smaller:
Plaits' voice object contains every engine regardless.

### Glyphs

Four symbols, T1 first (left to right), one fixed glyph per slot. They're drawn by hand to be
recognisable, not counted in binary: full brightness for slots 1–16, half brightness from 17.

| symbol | or | meaning |
|---|---|---|
| `●` | `#` | full brightness |
| `◐` | `+` | half brightness (`SP1_ENGINE_LED_HALF` in `firmware/src/sp1_ui_timing.h`) |
| `○` | `.` | off |

Save the file as UTF-8. If a spreadsheet or editor turns the circles into `?` or garbage, the
build will say so; write the glyphs in their ASCII forms instead (`#..#` is the same glyph as
`●○○●`).

Slot 16's glyph is `○○○○` (all off); its flash shows as a brief dark blink.

### What you can't change here

Each slot's **glyph**, and each engine's **index inside Plaits** and **centre detents**, are fixed,
so the CSV can't change them. They live in `SLOT_GLYPHS` and `PLAITS_ENGINES` in
`tools/gen_engines.py`. The reasons for each detent are under "Why these detents" below.

## Available engines

The default `config/engines.csv` has the 16 original Plaits models first, in their panel order,
then five of the 8 models added in Plaits 1.2. Slots 22–24 are empty: the three **6-op FM** banks
are left out (a community DX7 firmware covers them), but any of them can go in an empty slot.

| Default slot | Engine | Plaits # | Detents | CPU (avg; M3b–M3f logs) |
|---|---|---|---|---|
| 1 | virtual analog | 8 | F2 F4 | 67.8 % |
| 2 | waveshaping | 9 | F4 | 52.2 % (61 % peak) |
| 3 | 2-op FM | 10 | F3 | 70.5 % (76 % peak) |
| 4 | grain / formant | 11 | F4 | 75.7 % |
| 5 | additive | 12 | - | M3f: 54.8 % (64 % peak); M4a: est. ~61 % / ~71 % |
| 6 | wavetable | 13 | - | 63.4 % |
| 7 | chords | 14 | - | 76.6 % |
| 8 | speech | 15 | - | 27–39 % (varies with MORPH/HARMONICS) |
| 9 | swarm | 16 | - | 73.9 % |
| 10 | filtered noise | 17 | F4 | 64.7 % |
| 11 | particle | 18 | F3 | 86.5 % at 6; M3f (3): 49–51 % avg, 83–100 % peaks below MORPH centre; M4: 2 particles |
| 12 | string | 19 | - | 68.9 % (74 % peak) at 2 strings (M3f) |
| 13 | modal | 20 | - | 49.1 % (54 % peak) at 8 modes (M3f); M3g: 12, est. ~65 % |
| 14 | bass drum | 21 | - | 67.1 % |
| 15 | snare drum | 22 | - | 68.3 % |
| 16 | hi-hat | 23 | - | 71.4 % |
| 17 | VA + VCF | 0 | F3 F4 | 66.4 % |
| 18 | phase distortion | 1 | - | 72.1 % |
| 19 | wave terrain | 5 | F3 | 62.6 % (one window) |
| 20 | string machine | 6 | F2 | 84.4 % (~98 % peak); M3g/M4: est. ~61 % at TIMBRE centre, ~76 % with the 2-tap chorus |
| 21 | chiptune | 7 | - | 27.7 % (one window) |
| — | 6-op FM A | 2 | F3 | ~78 % (one window) |
| — | 6-op FM B | 3 | F3 | not measured |
| — | 6-op FM C | 4 | F3 | not measured |

The Plaits # and Detents columns are copied from `tools/gen_engines.py` for reference; that file,
not this table, is what the build uses.

The CPU column is a mean cost of a 5 ms audio block,
from `logs/sp1-20260921-153722.log`. Above ~85 % leaves little room for the control loop.
Trims are Kconfig options in `firmware/prj.conf`, not this table: `CONFIG_SP1_STRING_VOICES`
(default 2, upstream 3), `CONFIG_SP1_PARTICLES` (2, upstream 6), `CONFIG_SP1_MODAL_MODES` (12,
upstream 24). "est." = projected from host instruction counts. From M4, add ~8 % while
Marbles' clock runs.

### ⚠️ Re-measured on hardware with the M4a–M4d build, Marbles running

From `logs/sp1-20260925-131409.log` (Adara's M4a–M4d session, ~1 hour, 723 488 audio blocks,
**`fail=0 rst=0` throughout**). Thirteen engines were visited. These supersede the column above
for those rows, because they were taken with **Marbles' clock running** — the real workload —
rather than with Plaits alone.

| Plaits # | slot | engine | avg % (M4a–M4d hardware) | column above |
|---|---|---|---|---|
| 5 | 19 | wave terrain | **82.3** | 62.6 % "one window" |
| 7 | 21 | chiptune | 73.7 | 27.7 % "one window" |
| 8 | 1 | virtual analog | 56.5 | 67.8 % |
| 9 | 2 | waveshaping | 55.9 | 52.2 % |
| 12 | 5 | additive | 67.4 | M4a est. ~61 % |
| 13 | 6 | wavetable | 73.9 | 63.4 % |
| 16 | 9 | swarm | **79.5** | 73.9 % |
| 17 | 10 | filtered noise | 66.1 | 64.7 % |
| 18 | 11 | particle | 47.6 | M3f (3 particles) 49–51 %; this build has 2 |
| 19 | 12 | string | 71.3 | 68.9 % at 2 strings |
| 20 | 13 | modal | 73.0 | M3g est. ~65 % at 12 modes |
| 21 | 14 | bass drum | 75.3 | 67.1 % |
| 23 | 16 | hi-hat | 77.0 | 71.4 % |

**Everything fits.** The heaviest is wave terrain at 82.3 %, and an hour of playing produced no
buffer failures and no resets.

⚠️ **Two lessons, both about the numbers rather than the code.**

1. **The "one window" figures were optimistic, badly so.** Wave terrain was recorded at 62.6 %
   and measures **82.3 %**; chiptune was recorded at 27.7 % and measures 73.7 %. A single short
   observation window catches whatever the engine happened to be doing at those fader positions.
   Do not quote a one-window figure as an engine's cost.
2. **Host-to-hardware projections ran 6–8 points optimistic** for the M3g/M4a-era trims:
   Additive was projected ~61 % and measures 67.4 %; Modal at 12 modes was projected ~65 % and
   measures 73.0 %. The %-per-instruction calibration used for these projections is derived
   from Additive's M3f run and evidently does not carry across builds as tightly as assumed.
   Treat a projection as ±8 points, not ±15 %, and re-measure on hardware before claiming an
   engine "fits".

⚠️ **Ignore the `max` and `ever` columns of the `AUD` line for this purpose.** They reach 170 %+
in this log, which is engine-change transients and interrupts landing inside a block, not the
engine's cost. **`avg` is the figure that decides whether an engine fits.**

## What each fader does, per engine

**In fader order, so the table reads the way the device is laid out** (Adara, M4a). F1 is
missing from it on purpose: **F1 is always pitch** — Plaits' V/OCT and frequency control, with
its range set by SETTINGS F1. Only F2–F4 change meaning with the engine.

| Default slot | Engine | F2 TIMBRE | F3 MORPH | F4 HARMONICS | AUX (SETTINGS T4) |
|---|---|---|---|---|---|
| 1 | virtual analog | variable square, from narrow pulse to full square to hardsync formants | variable saw, from triangle to saw with an increasingly wide notch | detuning between the two waves | sum of two hardsync'ed waveforms, shaped by MORPH |
| 2 | waveshaping | wavefolder amount | waveform asymmetry | waveshaper waveform | the same, with Warps' other wavefolder curve |
| 3 | 2-op FM | modulation index | feedback — operator 2 modulating its own phase, or operator 1's | frequency ratio | sub-oscillator |
| 4 | grain / formant | formant frequency | formant width and shape (the window's shape) | frequency ratio between formant 1 and 2 | filtered waveforms simulated by windowed sine waves |
| 5 | additive | index of the most prominent harmonic — a bit like a cutoff frequency | bump shape, from flat and wide to peaked and narrow | number of bumps in the spectrum, starting with one big bump | only the harmonics a Hammond organ's drawbars have |
| 6 | wavetable | row index — waves sorted by spectral brightness | column index | active bank | low-fi (5-bit) output |
| 7 | chords | chord inversion and transposition | waveform: string-machine waveforms below the centre, a wavetable scan above | chord type | the chord's root note |
| 8 | speech | species, from Daleks to chipmunks | phoneme or word-segment selection | crossfades formant filtering → SAM → LPC vowels, then banks of LPC words | the unfiltered vocal cords' signal |
| 9 | swarm | grain density | grain duration and overlap; at the top the grains merge into each other | amount of pitch randomisation | the same, with sine-wave oscillators |
| 10 | filtered noise | clock frequency | filter resonance | filter response, LP → BP → HP | two band-pass filters, separation set by HARMONICS |
| 11 | particle | particle density | filter type: a reverberating all-pass network below the centre, resonant band-pass filters above | amount of frequency randomisation | raw dust noise |
| 12 | string | excitation brightness, and the dust density while it drones | decay time (energy absorption) | inharmonicity: dispersive below ~25 % of travel, stiff above, harmonic in the small dead band between | the raw exciter signal |
| 13 | modal | excitation brightness and dust density | decay time (energy absorption) | amount of inharmonicity, or material | the raw exciter signal |
| 14 | bass drum | brightness | decay time | attack sharpness and amount of overdrive | a frequency-modulated triangle VCO turned into a sine by diodes |
| 15 | snare drum | balance between the drum's modes | decay time | balance of the harmonic and noisy components | two frequency-modulated sine VCOs mixed with high-passed noise |
| 16 | hi-hat | high-pass filter cutoff | decay time | balance of the metallic and the filtered noise | three pairs of square oscillators ring-modulating each other, through a clean linear VCA |
| 17 | VA + VCF | filter cutoff | waveform and sub level | resonance and filter character — gentle 24 dB/oct below the centre, harsh 12 dB/oct above | the 12 dB/oct high-pass output |
| 18 | phase distortion | distortion amount | distortion asymmetry | distortion frequency | the carrier runs free, making it phase modulation |
| 19 | wave terrain | path radius | path offset | terrain | the terrain's height read as phase distortion, sin(y+z) |
| 20 | string machine | chorus / filter amount | waveform | chord | voices 2 and 4, mostly |
| 21 | chiptune | arpeggio type, or chord inversion | pulse width / sync | chord | the NES triangle voice |
| — | 6-op FM A/B/C | modulator level(s) | envelope and modulation stretching ("time travel") | preset selection, within this bank | the same signal as OUT |

Sources: the Plaits manual's model table for F2–F4 and AUX, and the firmware 1.2 release notes
for the 1.2 models (VA + VCF onwards, and 6-op FM). **String is the one the manual never documented** — its row is read
off `plaits/dsp/physical_modelling/string_voice.cc`, where HARMONICS is the `structure`
argument (non-linearity is `(s−0.24)·4.166` below 0.24, `(s−0.26)·1.35` above 0.26, zero
between), TIMBRE is `brightness` squared, and MORPH is `damping`. The 6-op FM banks' AUX is
`aux[i] = out[i]` in `six_op_engine.cc`, i.e. the two outputs carry the same thing.

The rows follow the default slot order. Your own `config/engines.csv` may order them
differently; the per-engine descriptions don't change.

## Why these detents

A detent goes where the fader's centre is an **exact neutral point** — the zero of a signed
amount, or the point where one effect ends and a different one begins. It doesn't go on a
plain 0–1 amount or a selector: they'd lose 10 % of their travel for nothing. Worked out from
the Plaits manual and each engine's source (the manual doesn't say for several). The build takes
them from `PLAITS_ENGINES` in `tools/gen_engines.py`; change both together, and only if Plaits
itself changes:

| engine | detent | centre means (source) |
|---|---|---|
| VA + VCF | F4, F3 | F4: resonance = max(\|h−.5\|−.125, 0), so the centre is zero resonance (24 dB/oct below, 12 dB/oct above). F3: the centre is a plain square with no sub-oscillator. |
| 6-op FM ×3 | F3 | envelope time × 2^((.5−m)·8): the centre is the patch's own envelopes. |
| wave terrain | F3 | path offset 1.9m−1: the centre is (almost exactly) a centred path. |
| string machine | F2 | ensemble amount \|t−.5\|·2: the centre is dry, with filter on one side and chorus on the other. |
| virtual analog | F4, F2 | F4: detune 2.05h−1.025, so the centre is unison. F2: square wave, no sync ("narrow pulse … full square … hardsync"). |
| waveshaping | F4 | shape amount \|h−.5\|·2: the centre is no waveshaping. |
| 2-op FM | F3 | feedback 2m−1: the centre is no feedback ("before 12 o'clock, chaotic! past 12 o'clock, rough!"). |
| grain / formant | F4 | formant ratio −24+48h semitones: the centre is 1:1. |
| filtered noise | F4 | the 2nd filter sits at the same pitch as the 1st; response goes LP → BP (centre) → HP. |
| particle | F3 | all-pass network below the centre, resonant band-pass above; the centre is neither. |

⚠️ **The detents are also the unipolar/bipolar table.** A detent marks a parameter whose centre is
a neutral point — which is exactly what "bipolar" means — so `SP1_ENGINE_TABLE[].centre` is already
the per-engine polarity data that M4c's INTELLIGENT voltage range needs. Do not build a second
table for it; if a detent is wrong, the range will be wrong too.

All four attenuverters always have a detent (they're bipolar), and so does FREQUENCY (5 %).
Those don't depend on the engine, so they aren't in this table. FINE TUNE had one too and was
removed in M4a.
