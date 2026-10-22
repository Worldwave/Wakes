# Marbles settings — models, ranges, scales

The Marbles-side companion to `docs/PLAITS-ENGINES.md`: everything that is a *choice* rather
than a fader, what each choice does, the LED glyph that flashes when you change it, and whether
it is included in the firmware.

⚠️ **This file is NOT read by the build**, unlike `config/engines.cnfg` for Plaits. The tables below are the
authority for what the firmware *should* do; `firmware/src/sp1_marbles_ui.c` holds the same
glyphs and inclusion flags in C. If a third consumer ever appears, generate them from here the
way `tools/gen_engines.py` generates the engine list from `config/engines.cnfg` — until then, changing a glyph or an
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
because there are no long presses in this UI (Adara, M4a). **No wrap**, and **empty slots are
skipped**. Glyphs are those of engine slots 1–7, as Adara asked, and belong to the SLOT, not to
the scale in it.

The scale is what X quantizes to when `STEPS` (X page F3) is above its centre. Below the centre
X is smooth and the scale does nothing.

### The scales are a file: `scales.cnfg` (#44)

Since #44 the seven slots come from **`WAKES/CNFG/scales.cnfg`** on Wakes' drive, read at every
ON; `config/scales.cnfg` in the repository is the built-in default (and the text Wakes writes to
the drive when the file is missing). The file explains its own format to users. In short: one
`[1]`–`[7]` section per slot, each holding a scale in the **Scala `.scl` format**, so a scale
built in [Scale Workshop](https://sevish.com/scaleworkshop/) and exported as `.scl` can be pasted
under a slot as it is.

```
[1]
plaits = 32             Wakes: Plaits plays the degrees of weight >= 32 (default 0: all)
tune_by_ear = on        Wakes: Plaits' root is the note tuned by ear (default on; #30)
! a Scala comment
major                   the description: the scale's name in the log
12                      pitches that follow, the period included (1-64)
 100.0   16             cents (has a ".") or a ratio (3/2, 2), then Wakes' WEIGHT 0-255
 ...
2/1                     the last pitch is the period: 2/1 = the octave
```

- The root (1/1) is implicit, as in Scala, and always weight 255. Pitches may come in any order.
- A **weight** after a pitch is Wakes' extra (Scala tools ignore text after a pitch); a pitch
  without one is 255. Any other text after a pitch is ignored.
- **64 pitches at most**, the period included: 31-EDO, 53-EDO fit. Upstream Marbles holds 16
  (`kMaxDegrees`, 16-bit masks); Wakes replaces its quantizer (`src/plaits_ovr/marbles/random/
  quantizer.{h,cc}`: 64 and 64-bit masks, nothing else changed) and keeps ONE quantizer per
  output instead of six (generated override of `output_channel.{h,cc}`), which pays for the
  bigger tables. The quantizer only runs when an output draws a new value (a clock tick), not
  per sample, so a long scale costs no CPU to speak of.
- Any period, not only the octave: 3/1 (Bohlen-Pierce) works on both modules.
- **All or nothing**, like `engines.cnfg`: a problem anywhere in the file and Wakes uses the
  built-in scales, every problem named in the log (`CNFG scales.cnfg line N: …`). The one
  exception (Adara): a slot above `[7]` is **dropped** and the log says the file is
  **overloaded**; the rest loads.
- **PRST stores the slot** (its tokens `major` … `free` mean slots 1–7, unchanged so every saved
  preset still loads). Change what a slot holds and the presets that used it play the new scale.
  A preset on a slot that is now empty: Marbles takes the first filled slot, Plaits' quantizer off.
- `logs.cnfg`'s `scales = on` logs the bank at every ON: each slot's name, period, degrees in
  cents with their weights, and what Plaits plays (`SCALES …` lines).

### The built-in bank

Slots 1–6 are Marbles' own (`marbles/settings.cc`, Emilie Gillet, MIT), written as Scala. Slot 7
holds a whole-tone scale (Adara): six equal 200-cent steps, every weight 255, so STEPS keeps
all six and Plaits plays all six; tune by ear on.

| # | Glyph | Scale | Written as | Plaits plays |
|---|---|---|---|---|
| 1 | `●○○○` | major | 12-TET cents, Marbles' weights (C 255 · G 192 · E, B 128 · D, A 96 · F 64 · the rest 8–24) | weight ≥ 32 |
| 2 | `●●○○` | minor | 12-TET cents, Marbles' weights | weight ≥ 32 |
| 3 | `●●●○` | pentatonic | 12-TET cents, Marbles' weights | weight ≥ 32 |
| 4 | `●●●●` | pelog | Marbles' cents: 153 · 315 · 552 · 705.96 · 848.04 · 1058.04 | all |
| 5 | `○●●●` | bhairav | just ratios: 256/243 · 9/8 · 6/5 · 5/4 · 4/3 · 45/32 · 3/2 · 128/81 · 27/16 · 9/5 · 15/8 | weight ≥ 32 |
| 6 | `○○●●` | shri | the same ratios, shri's weights | weight ≥ 32 |
| 7 | `○○○●` | whole-tone | cents: 200 · 400 · 600 · 800 · 1000, no weights (all 255) | all |

Marbles stores its scales as 4-decimal voltages (a semitone is `0.0833`, Komal Re `0.0752`). The
Scala form writes what those numbers stand for — 100 cents, 256/243 — so every degree moved by
**at most 0.12 cents** (9/5 in the ragas; 0.04 cents in the 12-TET scales). Inaudible, and
`cnfgtest` §7 checks the bound.

### Weights and STEPS

**Weight is how long a degree survives as STEPS goes up**, not loudness or probability of a
single note. Marbles' quantizer has seven levels, thresholds 0 · 16 · 32 · 64 · 128 · 192 · 255:
past STEPS' centre, turning it up raises the threshold, and the degrees whose weight falls below
it drop out, until only the root (255) is left. Marbles' own scales use this to make a scale
*sound* like itself — in `major`, the root is 255, the fifth 192, the third and seventh 128, and
the accidentals 4–24, so chromatic notes appear as occasional colour rather than as equals.

A **flat** scale (every weight 255, which is what a pasted Scale Workshop scale is) stays whole at
every STEPS setting above the centre.

### Plaits borrows the same scales (M4b)

The PLAITS SETTINGS page's T2 / T3 quantize **Plaits' FREQUENCY fader** to one of these
scales. Plaits keeps its **own** index into the slots, so the two modules can be in different
scales at once, and it runs on its own `marbles::Quantizer` instance in the main thread —
nothing shared with the four the audio thread is using.

**There is no "amount" control (Adara): each scale quantizes to the note set its NAME means.**
That is not the same as "every degree in the table". Marbles stores major as *twelve* weighted
degrees, so admitting all of them would quantize to a chromatic scale and make four of the six
scales sound identical. Each scale's **`plaits =`** weight picks the set; the quantizer is loaded
with only those degrees. Measured by sweeping F1 across the full range and collecting the
distinct pitches, in semitones above the root (`routetest`):

| scale | `plaits` | notes per period | degrees (semitones) |
|---|---|---|---|
| major | 32 | 7 | 0 · 2 · 4 · 5 · 7 · 9 · 11 |
| minor | 32 | 7 | 0 · 2 · 3 · 5 · 7 · 8 · 10 |
| pentatonic | 32 | 5 | 0 · 2 · 5 · 7 · 9 |
| pelog | 0 (all) | 7 | 0 · **1.53** · **3.15** · **5.52** · **7.06** · **8.48** · **10.58** |
| bhairav | 32 | 7 | 0 · **0.90** · **3.86** · **4.98** · **7.02** · **7.92** · **10.88** |
| shri | 32 | 7 | 0 · **2.04** · **3.16** · **4.98** · **7.02** · **9.06** · **10.18** |

⚠️ **Pelog needs 0**: its seven degrees *are* the scale, and at 32 it would lose two of them.
⚠️ **0 on major, minor, bhairav or shri admits all twelve degrees** — chromatic.

Until #44 this was `scale_level` (a Marbles level per scale, 3 = weight ≥ 32, 1 = all) with the
whole scale loaded: the same notes, but only the seven level thresholds were possible.

**Three of these are microtonal.** Pelog, bhairav and shri put degrees at 1.53, 0.90, 3.16…
semitones — nowhere near the semitone grid. That is what they are for, and it means Plaits'
pitch will not be a MIDI note number when they are selected. Octave mode 9 with a scale steps
through the degrees across nine PERIODS (nine octaves for every built-in scale).

### Why there can be seven, and how a new file takes effect

Marbles gives every output channel six quantizer slots and preloads all six, so six is its
ceiling. We load **only the selected scale**, into slot 0 of all four channels (since #44 the
only slot: the other five are overridden away), from the audio
thread when the selection changes (`LoadScaleIfNeeded` in `sp1_marbles.cc`). The count is
therefore ours to pick and costs one `Quantizer::Init` per change, which is a table recompute of
about a thousand cycles for four channels.

⚠️ It has to be **all four** channels. `XYGenerator::LoadScale(index, scale)` walks only the
three X channels, so M4 left Y's quantizer holding Marbles' empty default scale. Nothing showed,
because Y STEPS defaults below its centre where Y uses the lag processor and never quantizes.

The bank (`sp1_scales.c`) keeps **two copies**: the ON job writes the one not in use and flips,
**once per ON job**, before PRST checks its slots — so no reader ever sees a scale half-written.
Every install bumps a generation number; the audio thread and Plaits' quantizer reload their slot
when it changes (one compare per audio block). Main then moves a selection off a slot the new
file emptied (`sp1_mui_scales_settle`, `sp1_pui_scales_settle`).

`tools/host-tests/uitest` fails if two slot glyphs collide.

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
