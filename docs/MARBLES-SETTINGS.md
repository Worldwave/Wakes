# Marbles settings — models, ranges, scales

The Marbles-side companion to `docs/PLAITS-ENGINES.md`: everything that is a *choice* rather
than a fader, what each choice does, the LED glyph that flashes when you change it, and whether
it is included in the firmware.

⚠️ **This file is NOT read by the build**, unlike `config/engines.csv` for Plaits. The tables below are the
authority for what the firmware *should* do; `firmware/src/sp1_marbles_ui.c` holds the same
glyphs and inclusion flags in C. If a third consumer ever appears, generate them from here the
way `tools/gen_engines.py` generates the engine list from `config/engines.csv` — until then, changing a glyph or an
inclusion flag means changing both.

## LED glyphs

Four LEDs on the model row, T1 first. **Full brightness only.** Half-brightness glyphs were
tried in M4 and dropped in M4a: they are hard to tell apart from full in average room light,
which is the same reason the engine flash moved to 33 % for its `◐`.

| symbol | meaning |
|---|---|
| `●` | full brightness |
| `○` | off |

---

## t models — `[E]`, T1 on the t page

One press steps to the next; there is **no long press**. Marbles splits these into two banks of
three and hides the second behind a 2 s hold of `[E]`; here all six are one ring (Adara, M4a).

Glyphs are free-form, like the engine list's, and deliberately avoid the counting bars used by
the destination and scale flashes — so a model flash is never mistaken for a routing flash.

**t2 is the master clock in every model** — it fires on every tick, and it is what the play-row
clock and the Y divider follow. The models differ in what t1 and t3 do with those ticks.
Descriptions read off `marbles/random/t_generator.cc`, named function by named function.

| # | Glyph | Model | Marbles bank | What t1 and t3 do | `t BIAS` in this model | On |
|---|---|---|---|---|---|---|
| 1 | `●○○●` | coin toss | 1 | **Complementary.** Every tick goes to exactly one of t1 / t3 — never both, never neither. The classic Marbles behaviour. | which of the two wins the toss | yes |
| 2 | `○●●○` | clusters | 1 | A **random pair of clock divisions**, re-drawn every few bars from a table of 17 ratios (1:1, 1:2, 1:4, 3:2, 5:4, 1:16 …), so pulses drift in and out of coincidence in clumps. | how far from 1:1 the drawn ratios may wander; below the centre the pair is swapped, so which side is the fast one flips | yes |
| 3 | `●○●○` | drums | 1 | Steps through Marbles' **drum-pattern tables** — a repeating rhythm rather than a random one, re-picked at the end of each pattern. | how far into the pattern bank it roams: the centre is the plainest pattern, the ends the busiest. Below the centre only every other pattern is chosen | yes |
| 4 | `○●○●` | independent Bernoulli | 2 | Like coin toss but each side tosses its **own** coin, so a tick can fire both or neither. | each side's own probability | yes |
| 5 | `●●○●` | divider | 2 | A **fixed** division pair chosen directly by the fader: t1 : t3 from 8 : 1, through 1 : 1 at the centre, to 1 : 8. Not random at all. | *is* the selector — 17 steps with hysteresis | yes |
| 6 | `●○●●` | three states | 2 | A **three-way** choice per tick: t1, t3, or neither. | how often "neither" wins, and then which side | yes |

`t BIAS` (t page F2) has a 10 % centre detent because in most of the six the centre is the
neutral choice — the even split, 1 : 1, the plainest pattern. `JITTER` (F3) is the same in all
six: it smears the master clock's timing.

---

## t range — `[B]`, FFWD / RWD on any Marbles page

Multiplies the master clock. **No wrap**: at either end the press is ignored and the log says
`(end)`.

| Glyph | Range | Tempo at RATE centre |
|---|---|---|
| `●○○○` | ×0.25 | 30 BPM |
| `○●●○` | ×1 | **120 BPM** (the default) |
| `○○○●` | ×4 | 480 BPM |

RATE itself (t page F1) is ±5 octaves around the centre, so the reachable span is about 1 BPM to
1 900 BPM — capped in `sp1_marbles.cc` at a 500 Hz master clock, far past anything musical, so a
ratchet at the top of ×4 cannot run the phase away.

---

## X diversity — `[N]`, T1 on the X page

How X1, X2 and X3 respond to the shared SPREAD / BIAS / STEPS faders.

| Glyph | Mode | What it does | On |
|---|---|---|---|
| `●●●○` | identical | all three follow the faders together | yes |
| `○●○○` | bump | X2 follows the faders; X1 and X3 go the opposite way | yes |
| `●○●○` | tilt | X1 and X3 tilt in opposite directions; X2 sits at the middle | yes |

The diversity flash is drawn from the mode rather than from a glyph table (`main.c`): a lit LED
means *that output moves with the faders*, so the glyph is a picture of the mode.

---

## X and Y voltage range — `[J]`

`[J]` is **T4 on the MARBLES SETTINGS page**, and it sets the range for **X and Y together**. The
glyph is the size of the range, so a wider range lights more LEDs.

⚠️ **One control, not two, since M4e** (Adara). Through M4c there were two — X's on `••` + T4 of
the X page and Y's on SETTINGS T4. In Marbles' own manual `[J]` is a *single* button and Y's range
is that same button with a modifier held, so two independent settings were never in the hardware
being copied, and keeping them cost a button for no expressive gain. `••` + T4 is **Y's
destination** now, beside the other six destination buttons. SETTINGS **T1 is free**.

| Glyph | Range | Notes | On |
|---|---|---|---|
| `●○○○` | 0 – 2 V | Two octaves on V/Oct. | yes |
| `●●●○` | 0 – 5 V | Five octaves on V/Oct. | yes |
| `●●●●` | −5 – +5 V | Ten volts, centred on zero. Marbles' own default. | yes |
| `●○●○` | INTELLIGENT | The **default** since M4c. Picks each output's range from what it is routed to **and from the engine currently selected** — see below. | yes |

⚠️ **The range does not decide whether the output is quantized. STEPS `[M]` does** — above its
centre the channel emits scale degrees, below it a smooth lag-filtered voltage, in every range.
An earlier version of this table said "quantized to the scale" for the two unipolar ranges, which
read as though the range chose it; it never did.

### INTELLIGENT

| routed to | range | why |
|---|---|---|
| V/Oct | **0 – 2 V** | two octaves. 0–5 V is a ten-octave leap and 10 V is unplayable |
| LEVEL, TRIG | **0 – 5 V** | one-sided by nature — a negative level or gate means nothing (Adara) |
| FM | **±5 V** | always signed, whatever the engine |
| TIMBRE, MORPH, HARMONICS | **±5 V** if that parameter is **bipolar on the current engine**, else **0 – 5 V** | a bipolar parameter's centre is its neutral point, so modulation should be able to go either side of it; a unipolar one starts at zero and only has one direction to go |

⚠️ **The polarity comes from each engine's detents** (`docs/PLAITS-ENGINES.md`) — a detent marks
exactly a parameter whose centre is its neutral point, which is what "bipolar" means — compiled by
`gen_engines.py` into `SP1_ENGINE_TABLE[].centre` (bit 0x1 HARMONICS/F4, 0x2 TIMBRE/F2, 0x4
MORPH/F3). There is no second table and there must never be one.

⚠️ **It is resolved PER CHANNEL, once per audio block**, so X1 on V/Oct and X2 on FM sit at
0–2 V and ±5 V *at the same time* — Marbles' range is a per-group setting, and a generated override
of `marbles/random/x_y_generator.cc` makes its one `switch` ask `sp1_mrb_channel_range()` instead.
All of the policy is in `IntelligentRange()` in `sp1_marbles.cc`; the override holds none, and a
group set to an explicit range still behaves exactly as upstream.

⚠️ **A range change takes effect from a channel's NEXT value, not retroactively.** Marbles applies
the range when a voltage is *generated*, so after an engine change or a re-route the channel finishes
the value it is on (and, with STEPS smooth, ramps across the two). One clock tick, by design.

---

## Scales — `••` + FFWD / RWD on the X page

Marbles puts scale selection on a long press of `[J]`; here it is the shifted FFWD / RWD rocker,
because there are no long presses in this UI (Adara, M4a). **No wrap**, and **excluded scales
are skipped**. Glyphs are those of slots 1–7 in the default `config/engines.csv`, as Adara asked. They're a fixed
copy: editing that file doesn't change them.

The scale is what X quantizes to when `STEPS` (X page F3) is above its centre. Below the centre
X is smooth and the scale does nothing.

| # | Glyph | Scale | Degrees (volts above the root, weight 0–255) | On |
|---|---|---|---|---|
| 1 | `●○○○` | major | C 0.0000/255 · C♯ 0.0833/16 · D 0.1667/96 · D♯ 0.2500/24 · E 0.3333/128 · F 0.4167/64 · F♯ 0.5000/8 · G 0.5833/192 · G♯ 0.6667/16 · A 0.7500/96 · A♯ 0.8333/24 · B 0.9167/128 | yes |
| 2 | `●●○○` | minor | C 0.0000/255 · C♯ 0.0833/16 · D 0.1667/96 · E♭ 0.2500/128 · E 0.3333/8 · F 0.4167/64 · F♯ 0.5000/4 · G 0.5833/192 · G♯ 0.6667/96 · A 0.7500/16 · B♭ 0.8333/128 · B 0.9167/16 | yes |
| 3 | `●●●○` | pentatonic | C 0.0000/255 · C♯ 0.0833/4 · D 0.1667/96 · E♭ 0.2500/4 · E 0.3333/4 · F 0.4167/140 · F♯ 0.5000/4 · G 0.5833/192 · G♯ 0.6667/4 · A 0.7500/96 · B♭ 0.8333/4 · B 0.9167/4 | yes |
| 4 | `●●●●` | pelog | 7 degrees, Javanese tuning — not 12-tone | yes |
| 5 | `○●●●` | bhairav | Hindustani raga | yes |
| 6 | `○○●●` | shri | Hindustani raga | yes |
| 7 | `○○○●` | **free slot** | chromatic, every degree at 255 — **Adara's to replace** | **no** |

Slots 1–6 are Marbles' own, verbatim in `firmware/src/sp1_marbles_scales.inc` (copied from
`marbles/settings.cc` and not to be edited). Slot 7 is ours, in `sp1_marbles.cc` as
`sp1_free_scale`.

### Writing a custom scale

```c
const Scale sp1_free_scale = {
  1.0f,        // base_interval: how far up the pattern repeats, in VOLTS.
               // 1.0 = one octave at 1 V/oct. 0.5 would repeat every tritone.
  12,          // num_degrees: how many entries below are used. Up to 16.
  {
    { 0.0000f, 255 },   // { volts above the root, weight }
    { 0.0833f,  16 },
    ...
  }
};
```

- **Volts, not semitones.** One semitone is `1/12 V = 0.0833333f`. Two semitones is
  `0.1666667f`, and so on. Non-equal tunings are just other numbers — that is how pelog works.
  Degrees must be in ascending order and below `base_interval`.
- **Weight is probability, not level.** It is how often the quantizer is willing to land on that
  degree: 255 = always available, 0 = never chosen, and everything between biases the sequence.
  It does **not** make that note louder. Marbles' own scales use it to make a scale *sound* like
  itself — in `major`, the root is 255, the fifth 192, the third and seventh 128, and the
  accidentals 4–24, so chromatic notes appear as occasional colour rather than as equals.
- A **flat** scale (every weight equal) is a chromatic run with no tonal centre. That is what
  the free slot holds today, which is why it ships **off**: it is a placeholder, not a choice.
- After editing it, set its `On` to `yes` here **and** `scale_on[6]` to `true` in
  `firmware/src/sp1_marbles_ui.c`.

### Plaits borrows the same scales (M4b)

The PLAITS SETTINGS page's T2 / T3 quantize **Plaits' FREQUENCY fader** to one of these
scales. Plaits keeps its **own** index into the table, so the two modules can be in different
scales at once, and it runs on its own `marbles::Quantizer` instance in the main thread —
nothing shared with the four the audio thread is using.

**There is no "amount" control (Adara): each scale quantizes to the note set its NAME means.**
That is not the same as "every degree in the table". Marbles stores major as *twelve* weighted
degrees, so admitting all of them would quantize to a chromatic scale and make four of the six
scales sound identical. `scale_level` in `sp1_marbles.cc` picks the weight threshold per scale
that yields the named set. Measured by sweeping F1 across the full range and collecting the
distinct pitches, in semitones above the root:

| scale | level | notes per octave | degrees (semitones) |
|---|---|---|---|
| major | 3 (≥32) | 7 | 0 · 2 · 4 · 5 · 7 · 9 · 11 |
| minor | 3 (≥32) | 7 | 0 · 2 · 3 · 5 · 7 · 8 · 10 |
| pentatonic | 3 (≥32) | 5 | 0 · 2 · 5 · 7 · 9 |
| pelog | 1 (all) | 7 | 0 · **1.53** · **3.15** · **5.52** · **7.06** · **8.48** · **10.58** |
| bhairav | 3 (≥32) | 7 | 0 · **0.90** · **3.86** · **4.98** · **7.02** · **7.92** · **10.88** |
| shri | 3 (≥32) | 7 | 0 · **2.04** · **3.16** · **4.98** · **7.02** · **9.06** · **10.17** |
| free slot | 1 (all) | 12 | every semitone, until you replace it |

⚠️ **Pelog is the one that needs level 1**: its seven degrees *are* the scale, and at level 3 it
would lose two of them. ⚠️ **Level 1 on major, minor, bhairav or shri admits all twelve
degrees** — chromatic. Do not collapse `scale_level` to a single constant.

**Three of these are microtonal.** Pelog, bhairav and shri put degrees at 1.53, 0.90, 3.16…
semitones — nowhere near the semitone grid. That is what they are for, and it means Plaits'
pitch will not be a MIDI note number when they are selected.

### Why there can be seven

Marbles gives every output channel six quantizer slots and preloads all six, so six is its
ceiling. We load **only the selected scale**, into slot 0 of all four channels, from the audio
thread when the selection changes (`LoadScaleIfNeeded` in `sp1_marbles.cc`). The count is
therefore ours to pick and costs one `Quantizer::Init` per change, which is a table recompute of
about a thousand cycles for four channels.

⚠️ It has to be **all four** channels. `XYGenerator::LoadScale(index, scale)` walks only the
three X channels, so M4 left Y's quantizer holding Marbles' empty default scale. Nothing showed,
because Y STEPS defaults below its centre where Y uses the lag processor and never quantizes.

### Writing a scale of your own (Adara asked, M4e)

Slot 7, `sp1_free_scale` in `sp1_marbles.cc`, is yours. It ships as a chromatic placeholder with
`scale_on[6] = false`, so it exists and is skipped.

⚠️ **Do not add to `sp1_marbles_scales.inc`** — that file is vendored verbatim from
`marbles/settings.cc` and must stay byte-identical to upstream. Edit `sp1_free_scale` instead.

The format is Marbles' own `Scale` (`marbles/random/quantizer.h`):

```c
{
  1.0f,            // base_interval: the octave, in VOLTS. 1.0 = 1 V/oct.
  12,              // num_degrees: how many entries below are used (max 16)
  {
    { 0.0000f, 255 },   // { voltage within the interval, weight 0-255 }
    { 0.0833f, 16  },   // 0.0833 V = one semitone at 1 V/oct
    ...
  }
}
```

- **Degrees are VOLTAGES, not semitones.** A semitone is 1/12 = 0.08333 V. That is exactly what
  makes the microtonal scales above possible: pelog's degrees are not multiples of 1/12.
- **Weight is how strongly a degree attracts**, and a threshold decides which weights count at
  all. That is why `scale_level` exists. **If you add a scale, give it a level too** — or the
  simplest thing: weight every degree **255** and set its level to **1**, which means "all of
  these, always" and is how to write a scale you want taken literally.
- To put it in the ring: `scale_on[6] = true` and give it a glyph in `scale_glyph[]`
  (`sp1_marbles_ui.c`). Both are already wired. Going beyond seven means bumping
  `SP1_MUI_SCALES` and `SP1_MARBLES_SCALES` and adding a row to each of those three tables.
- `tools/host-tests/uitest` fails if two glyphs collide, so a mistake there is caught before it
  reaches hardware.

---

## Y divider — SETTINGS F4

Y is clocked by a division of t2. Twelve steps with hysteresis, Marbles' own ratios:

`1/64 · 1/48 · 1/32 · 1/24 · 1/16 · 1/12 · **1/8** · 1/6 · 1/4 · 1/3 · 1/2 · 1/1`

The default is 1/8, which is Marbles' own (128/256 on its knob).

---

## Deliberately not included

| | |
|---|---|
| **The secret Markov mode** | Marbles hides a Markov-chain `t` mode behind a further gesture. Excluded on purpose (Adara): the six models above are the documented set and the SP-1 has no spare gesture to hide a seventh behind now that long presses are gone. |
| **External clock and CV inputs** | There are no jacks. `ramp_extractor.cc` is vendored but never called — it is the only part of Marbles that hard-codes 32 kHz, which is why the 4 kHz rule is safe. |
| **`register_mode`** (Marbles' shift-register / "constant" mode) | Reachable in the code (`GroupSettings::register_mode`) and wired to `false`. It needs a control to turn it on; no page has a spare button. Candidate for M6. |
| **X clock source** (`[not lettered]`) | Marbles can clock all three X outputs from one t output. The field is still in `sp1_marbles_params`; its fader was unbound in M4a and X keeps Marbles' own default — X1 on t1, X2 on t2, X3 on t3. |
| **Settings storage** | M5. Nothing Marbles-side persists across power-off yet. |
