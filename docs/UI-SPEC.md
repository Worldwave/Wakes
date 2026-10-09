# wakes-sp1 — UI specification v0.17

> **For the flat control map — every control on every page, with nothing else in the way —
> read `docs/UI-PAGES.md`.** This file is the design record: why each decision went the way it
> did, what was rejected, and what is still open. `docs/MARBLES-SETTINGS.md` is the same split
> for Marbles' models, ranges and scales.

v0.17 — 2026-10-06: **GTLT, the t gate tilt, on MARBLES t SHIFT F1** (#20, Adara). Wakes' own
parameter: bipolar with a 10 % centre detent, where it does nothing. Just past the detent t1's and
t3's gates drop steeply to half height; towards + t1 falls to 0 and t3 returns to full, towards −
the reverse. t2 is never touched. A gate stays 0 V / +5 V — +5 V is the positive maximum of every
destination a t output can reach — and GTLT scales the +5 V; the attenuverters and fader / CC
positions do the rest. **TRIG ignores GTLT**, so rhythm stays predictable. Unsmoothed. MIDI: CC 89,
7-bit, centred. Costs nothing per sample: the gain is folded into the routing once per audio block.
**Pickup gets a 3 % lock on every layer change, on both modules** (Adara: a GTLT edit was carrying
into CLOK). After `••` goes down or comes up, a fader's new parameter takes nothing until the fader
has moved 3 % from where the finger is — Plaits' own `POT_STATE_LOCKING` threshold — so the fader's
smoothing tail and a finger still resting on it no longer reach the other layer. Whether the fader
then tracks or catches up is unchanged.

v0.16 — 2026-10-06: **OCTV loses Plaits' LFO range** (#18, Adara). The bottom 1/11 of SETTINGS F1
is now the **full range with no centre detent**: F1 sweeps through C4 without the flat spot and can
be set finely around it. The scale quantizer applies there exactly as in the full range. Routing
Marbles to V/Oct still opens the range to the full range *with* the detent, whatever OCTV was on.

v0.15 — 2026-09-26: **Output select moves to PLAITS SETTINGS T4** (#11, v0.4.6), on press.
`••` + T4 on PLAITS is now Unpatch-HARMONICS only: a short press does nothing, so the shift
layer's T buttons carry exactly one job each.

v0.14 — 2026-09-25: **M4e** (Adara's M4c/M4d hardware notes). **UNPATCH**: `••` held plus a
T button held 2 s clears that button's routing — on PLAITS the parameter's (T1 = FREQUENCY, i.e.
V/Oct *and* FM), on MARBLES that output's. ⚠️ **The one long press in the UI**, reinstated
deliberately after M4a removed them all, and it moves every shift-layer T-button's ordinary
action to the button's RELEASE. **One `[J]` for X and Y**, on MARBLES SETTINGS T4; `••` + T4
becomes **Y's destination** on both pages and SETTINGS T1 is freed. **FFWD is one mechanism**:
a burst phase-locked to Marbles' master ramp, running or stopped — ⚠️ the rate-multiplying
RATCHET is gone, and with it the rhythm shift it caused. **FREQUENCY is held between TRIGs**
while a scale is selected, so a quantized note keeps its pitch for its whole decay. The drive
becomes an up/down pair with **uneven steps** (+3 / +8 / +15 / +24 dB) and no toggle, and is
cleared by a **PLAITS** rip only. The battery display always **fades** out over 300 ms; its
OFF-state brush threshold is 100 ms. The backstop moves **20 s → 30 s** (warning at 25 s).
Additive's `kRampStride` 4 → **2**. Plus the M4e fix for the attenuverters picking up fader
positions on entry to ON — which was never pickup, but `sp1_pui_enter()` being called before
any control scan had run.

v0.13 — 2026-09-25: **M4c** (Adara). `[J]` gains **INTELLIGENT** (`●○●○`) and it is the default
for both X and Y: each output's voltage range is chosen from what it is routed to *and from the
currently selected engine's* parameter polarity — V/Oct 0–2 V, LEVEL and TRIG 0–5 V, FM ±5 V,
TIMBRE / MORPH / HARMONICS ±5 V when bipolar on that engine and 0–5 V when not. ⚠️ Marbles' range
is a per-GROUP setting; a generated override of `marbles/random/x_y_generator.cc` makes it
**per channel**, so X1 on V/Oct and X2 on FM hold different ranges at the same time. The polarity
comes from each engine's detents (`docs/PLAITS-ENGINES.md`) — there is no second table.
Also: a **`••` tap shows the battery** on the play row for 1.5 s, from ON and from OFF, drawn with
the same `sp1_led_bar()` as STANDBY's charge bar and cancelled by any other input. ⚠️ The power
gestures are untouched: the OFF flash happens on release, after the decision not to power on, with
`sp1_power_on_hold()`'s return value unchanged.

v0.12 — 2026-09-24: **M4d** (Adara). **"Rip out the cables" becomes a full patch wipe**, per
`docs/DEFAULTS.md`: on PLAITS the engine returns to slot 1 and the BASE faders to that engine's
neutral state; on MARBLES every routing destination goes to `none` and the random stream is
re-seeded with the DEJA VU loop re-drawn, while the clock keeps running. Boot TRIG moves from
t1 + t3 to **t2**. The soft-clip drive is cleared by a rip on either module; VOL and the module on
show are kept.

v0.11 — 2026-09-24: **M4b** (Adara). PLAITS: T1 flashes the engine; the SETTINGS panel gets its
own buttons — T1 shows the scale, T2/T3 select a **FREQUENCY scale quantization** drawn from
Marbles' scales, engine select and the module swap are unbound there, and a `••` tap is the only
exit. `••` + VOL is a **soft-clip drive**, gain past maximum. MARBLES: one DEJA VU and one LENGTH
shared by both pages, with `[F]` / `[G]` on SETTINGS T2/T3 deciding which sections DEJA VU
reaches. Both destination rings start at `none` and the four CV destinations now share ONE glyph
vocabulary across the two sides (Adara's own edit to `docs/UI-PAGES.md`). New: `docs/DEFAULTS.md`.

v0.10 — 2026-09-24: **M4a** (Adara). PLAITS: FINE TUNE removed, OCTAVE range to SETTINGS F1,
LEVEL to SETTINGS F4, and the freed SHIFT F4 is a **new HARMONICS attenuverter** (an addition to
Plaits, which has none). MARBLES: t1–t3 get **destinations** instead of TRIG on/off toggles, with
LEVEL added to both rings and every summed destination clamped; the two t model banks become one
ring of six and the 2 s hold is gone; scale selection is `••` + FFWD / RWD on the X page; X SHIFT
F1 and F2 unbound. Speaker **+3 dB**. **No long press anywhere in the UI.**

v0.9 — 2026-09-22: **M4, Marbles** (Adara), built: all outputs; T4 alone swaps modules, T1
freed; PLAY = clock run/stop, no TRIG; Marbles reaches Plaits only while the clock runs;
per-page SHIFT routing; SETTINGS = Y page; FFWD = burst while stopped, ratchet while
running; `••`+PLAY held 3 s = "rip out the cables" on the module on show; the device starts
on the first engine of the table (virtual analog) with the clock at 120 BPM.

v0.8d — 2026-09-21: **`••`+FFWD / `••`+RWD = FFWD burst subdivision finer / coarser, 1/1 … 1/128**
(default 1/32); faders stay live while FFWD or RWD is held (sag corrected).
v0.8c — 2026-09-21: engine LED patterns are free-form (○ off, ◐ half, ● full, unique per
engine) in `docs/PLAITS-ENGINES.md`; **`••` + PLAY held 1.5 s resets the SHIFT and SETTINGS
pages to defaults** (replaces the patch wipe on that gesture).
v0.8b — 2026-09-21: **engine order, LED patterns, enable flags and detents now live in
`docs/PLAITS-ENGINES.md`** (read by the build); RWD = TRIG, FFWD = 1/32 burst, clock 100 BPM;
bipolar parameters display as magnitude (centre dark, both ends full); engine flash holds
0.7 s; SETTINGS breathes 35–100 %.
v0.8 — 2026-09-21: centre detents settled (10 % / FREQUENCY 5 %, per-engine); pickup on
every layer; PLAITS shift + settings pages built (M3b).

v0.7 — 2026-09-20. v0.1 by Adara; v0.2/v0.3 add hardware findings.
**Decisions 1–6 are now settled** and folded into the body. Nothing here is built yet.
**Nothing here is built.** Items marked ❓ need a decision; ⚠️ marks a collision
between the spec and the hardware.

---

## Shift: `••` only, and why (SETTLED — option A)

T1–T4 and Play sit on a resistor ladder fed by a common rail (P1.10) **that also
feeds the four faders**. Pressing a ladder button sags that rail and every fader
reading sags with it. That rules out hold-a-track-button-and-move-a-fader as a
gesture: the faders would be wrong for as long as the hold lasts.

`••` is on a direct GPIO (P0.27) and is **not** powered through the ladder rail.
Holding it does not disturb the faders at all. So `••` carries every shift.

**The `••` grammar**

| Gesture | Result |
|---|---|
| **Hold** `••` | modulation-offset layer (attenuverters + LEVEL) while held |
| **Double-tap** `••` | latch into the SETTINGS page |
| **Single tap** `••` (from SETTINGS) | back to the module's base page |

Double-tap to reach settings is right because that page is rarely visited, and
latching means the faders are free while editing — no hold, no sag.

## All 24 models, on 4 LEDs (SETTLED — option 1)

The 16-model count is Plaits **1.0/1.1**. The code in `pichenettes/eurorack` master is
**1.2, with 24**: the original 16 plus 8 added in 1.2 (classic waveshapes + filter,
phase distortion, 6-op FM ×3 banks, wave terrain, string machine, chiptune).

Dropping to 16 would have saved nothing — Additive (995 instr/sample) and Modal (858),
the two most expensive engines, are both in the *original* 16, while every 1.2 addition
is cheap-to-mid (423–582).

**Display: 4 front LEDs, binary index × 2 brightness tiers = 32 slots, 24 used.**
~~Dim tier = models 0–15, bright tier = models 16–23.~~ **v0.8 (Adara): BRIGHT = models 0–15,
DIM = 16–23**, T1 = most significant bit. Zeros are dark in the bright tier and *faint* in the
dim tier, so model 0 (all dark) and 16 (all faint) stay distinct. Shown as a **flash on every
model change**: instant, held **0.7 s** (v0.8b), then cross-faded back into the page over ~350 ms, so fast
scrolling shows each model at once without blinking back in between. Levels to be judged on
hardware. Keeps the side row free for VU.

> For reference, Plaits itself uses 8 bicolour LEDs: **position** = `engine % 8`,
> **colour** = `engine / 8` (3 banks). Brightness is our substitute for colour.

Brightness must be gamma-corrected: perceived brightness is roughly the square of duty
cycle, so a linear ramp looks wrong. A 256-byte table covers it. The two tiers need
enough separation to read at a glance under the gamma curve — pick them on hardware.

## T1 + T4: your UI use is fine — we just skip one convenience feature

**Normal taps on T1 and T4, including both at once, are ordinary SP-1 usage and are
completely safe. Nothing in the UI spec needs to change.**

"DFU" is Device Firmware Update — dropping into the bootloader so the device can be
reflashed. There are two separate ways in, and only one of them is ours:

**1. The bootloader's own scan, at power-on.** Hold T1+T4 *while plugging in USB-C*.
This lives in the bootloader at `0x00000`–`0x1FFFF`, runs before our application
exists, and is the recovery path. Nothing we write can affect it.

**2. An app-level shortcut.** `sp1-tape-looper` adds an `enter_dfu()` that watches for
a **3-second T1+T4 hold while the firmware is running** and resets into the
bootloader. It is purely a convenience so you don't have to unplug and replug.

Only #2 is worth skipping. With T1 and T4 as the module-tab buttons, a user resting
on both for three seconds would fall out of the synth into firmware-loading mode.
Skipping it costs nothing: path #1 always works.

**Rule: do not implement a runtime T1+T4 DFU trigger.** Module tabbing on T1/T4
stays exactly as v0.1 specifies.

## ✅ Solved for free: the fader/page mismatch

Four faders serve roughly 16 parameters across pages. When you switch pages the
physical fader position no longer matches the stored value, and the parameter would
jump on the first touch.

Plaits already solves this. `plaits/pot_controller.h` implements `PotController`
with `POT_STATE_CATCHING_UP`, `Lock()`, `Unlock()`, `Realign()` and hidden-parameter
handling — a catch-up/pickup model where a parameter does not move until the fader
passes through its stored value. **Lift it directly**; it is the same problem Plaits
has with its own alternate-parameter mode.

---

## Module and page structure (v0.9 draft, M4 — Adara 2026-09-22)

```
                         T4: swap module (both modules)
          ┌──────────────────┐            ┌─────────────────────────────┐
          │      PLAITS      │  ◄── T4 ──►│           MARBLES           │
          └──────────────────┘            └─────────────────────────────┘
            T2 ◄─► T3                       T2 = t page      T3 = X page
          engine prev/next                  (left, ●●○○)     (right, ○○●●)
            T1: disabled                    T1 = [E] on t · [N] on X
```

**T1 is no longer a module tab** (Adara, M4): with two modules, T4 alone swaps. T1 does
nothing on PLAITS for now and carries a Marbles button on each Marbles page.

**LED assignment**
- 4 front (track-row) LEDs: PLAITS — the active layer's fader values; MARBLES — its
  outputs (see "MARBLES page"). Flashes (engine, page, routing, output mode) overlay both.
- 4 side (play-row) LEDs: dB meter in the foreground on PLAITS, the clock on MARBLES; the
  other one as the dim background layer.

---

## PLAITS page

**Buttons**

| | Action |
|---|---|
| T2 / T3 | previous / next model |
| T1 | nothing (v0.9) |
| T4 | swap to MARBLES (v0.9) |
| `••` + T4 | ~~output select~~ → PLAITS **SETTINGS T4** (v0.15). Held 2 s: UNPATCH HARMONICS |
| `••` + T1 / T2 / T3 | free (v0.10) — the whole spare combination supply on this module |

~~`••`+T2 / `••`+T3 toggle t1 → TRIG / X1 → V/Oct~~ — v0.9: all routing moved to the
MARBLES pages, and it only exists while the clock runs (see "Play").

Toggling a routing off must clear the corresponding `Modulations::*_patched` flag, not
merely zero the signal. Plaits changes behaviour on those flags — an unpatched TRIG makes
the LPG free-run instead of gating, and an unpatched V/Oct changes how the note is
derived. This is the most musically important detail in the whole spec.

**Faders — base layer**

| | Parameter |
|---|---|
| F1 | FREQUENCY |
| F2 | TIMBRE |
| F3 | MORPH |
| F4 | HARMONICS |

**Faders — `••` held: modulation offsets.** v0.8 put FM / TIMBRE / MORPH / LEVEL here so each
attenuverter sat under the base parameter it modulates, and accepted LEVEL-on-F4 as the one
mismatch. **v0.10 removes the mismatch**: LEVEL moved to SETTINGS F4 and F4 became the
attenuverter the layout always wanted.

| | Parameter | `Patch` field |
|---|---|---|
| F1 | FM attenuverter | `frequency_modulation_amount` (bipolar) |
| F2 | TIMBRE attenuverter | `timbre_modulation_amount` (bipolar) |
| F3 | MORPH attenuverter | `morph_modulation_amount` (bipolar) |
| F4 | **HARMONICS attenuverter** (v0.10) | `harmonics_modulation_amount` — **ours**, see below |

**The HARMONICS attenuverter does not exist in Plaits.** Its HARMONICS CV is summed in raw, and
— the part that matters musically — HARMONICS is the one parameter the internal decay envelope can
never reach, because reaching it is what an *unpatched* attenuverter means on the other three. Two
generated overrides in `firmware/CMakeLists.txt` add `harmonics_modulation_amount` to `Patch` and
`harmonics_patched` to `Modulations`, then put HARMONICS through the same `ApplyModulations()` as
TIMBRE and MORPH with the plain decay envelope as its internal source. Everything the other three
attenuverters do, it now does: self-patching, unpatching, and being the envelope depth when
nothing is routed. Cost: nothing measurable.

**LEVEL is SETTINGS F4 from v0.10. Below 5 % it is disconnected** — clear `Modulations::level_patched`, do not
merely zero the value. The distinction is large in `voice.cc`:

- `level_patched == false` → `p.accent = 0.8f` fixed, and a TRIG fires
  `lpg_envelope_.Trigger()`. Normal percussive behaviour.
- `level_patched == true` → `compressed_level = 1.3·level/(0.3+|level|)` drives the VCA
  continuously, and the LPG envelope is **not** triggered. A static fader above the
  threshold therefore holds the VCA open — **this is the drone**.

Exactly the behaviour you asked for, and the flag is what makes it work rather than the
value. v0.9: an X or Y output routed to TIMBRE or MORPH (or FM, if FREQUENCY ends up
meaning FM) is scaled by that attenuverter; unrouted, the attenuverter keeps Plaits'
internal-envelope behaviour.

**Faders — `••` double-tap: SETTINGS page**

This answers your open question. In Plaits every pot has a *hidden* second parameter,
bound in `plaits/ui.cc:72–79`:

Plaits gives every pot a *hidden* second parameter, bound in `plaits/ui.cc:72–79`, and v0.8 used
that mapping 1:1. **v0.10 rearranges it** (Adara) so the page is ordered by what you reach for:

| | v0.10 | was (v0.8, Plaits' own hidden mapping) |
|---|---|---|
| F1 | **OCTAVE / FREQUENCY range** (`octave_`) | FINE TUNE |
| F2 | LPG colour (`lpg_colour`) — VCFA→VCA response | the same |
| F3 | LPG decay (`decay`) — ringing / envelope decay | the same |
| F4 | **LEVEL** (moved from SHIFT F4, 5 % disconnect intact) | OCTAVE / FREQUENCY range |

**FINE TUNE is gone.** It is not a loss: in Plaits it only ever acts in octave-range mode 9, and
mode 9 is precisely the mode that quantizes F1 to whole octaves — everywhere else upstream ignores
it. With it removed, mode 9 is fixed at fine tune's centre, which puts C4 on the middle step. The
range control moving to F1 also puts it under the fader it governs, which is where it belongs.

## MARBLES page (v0.9, M4 — Adara 2026-09-22; built in M4)

All of Marbles' outputs are generated: **t1, t2, t3, X1, X2, X3 and Y**. No inputs: the
external clock and external-CV processing are not used. Marbles runs once per Plaits block
(4 kHz), host-verified to give the same random sequence as its native 32 kHz.

Panel letters are the Marbles manual's: [A] RATE, [B] clock range, [C] JITTER, [D] t BIAS,
[E] t model, [H] DEJA VU, [I] LENGTH, [J] voltage range (hold 2 s on the module: scales),
[K] SPREAD, [L] X BIAS, [M] STEPS, [N] channel diversity (identical / bump / tilt).
⚠️ v0.2–v0.8 used [I] for X range and [M] for X mode; those letters were wrong.

**Marbles only reaches Plaits while the clock runs.** Stopped, Plaits gets our own TRIGs
(RWD, the FFWD burst) and our own LEVEL, exactly as in M3; every Marbles route is
unpatched. Running, the routes below are patched in.

### Base page — left (t, `●●○○`) and right (X, `○○●●`)

| | t page | X page |
|---|---|---|
| F1 | RATE [A] (120 BPM at centre, ±5 octaves) | SPREAD [K] |
| F2 | BIAS [D] (detent) | BIAS [L] (detent) |
| F3 | JITTER [C] | STEPS [M] (detent: centre = raw, below smooth, above quantized) |
| F4 | DEJA VU [H] (detent: centre = locked loop) | DEJA VU [H] (detent) |
| T1 | [E] t model: one press = **the next of all six** (v0.10; the 2 s bank hold is gone) | [N] diversity |
| T2 / T3 | t page / X page | t page / X page |
| T4 | swap to PLAITS | swap to PLAITS |
| RWD / FFWD | [B] clock range ÷4 / ×1 / ×4, **stops at the ends** (no wrap) | same |
| PLAY | clock run / stop | same |

Models, one ring of six (v0.10): coin toss (complementary Bernoulli), clusters, drums,
independent Bernoulli, divider, three states — Marbles' two banks of three, appended. Flash: a
free-form full-brightness glyph per model, tabulated in `docs/MARBLES-SETTINGS.md`. ⚠️ The
half-brightness "other bank" glyphs of v0.9 are gone: Adara found dimmed glyphs hard to tell
apart in room light, which is the same finding that took the engine flash's `◐` to 33 %.

**Track row, default: Marbles' own output LEDs.** t page: T1–T3 = t1–t3 gates, a high gate lit
at its GTLT height (v0.17, #20) with 0 % shown at 8 % so a gate firing TRIG never goes dark; X page:
T1–T3 = |X1|–|X3| over the selected range; **T4 = |Y| on both pages.** Stopped, X and Y hold
their last voltage (as the module does). Moving a fader shows the page's four values for
1.2 s (bipolar ones as magnitude, as on PLAITS). The page pattern flashes on a page change and
on arriving from PLAITS.

### `••` held — SHIFT, per page

| | t page SHIFT | X page SHIFT |
|---|---|---|
| T1 / T2 / T3 | **t1 / t2 / t3 destination**: none `○○○○` → TRIG `○●●●` → LEVEL `○○●●` → FM `●○○○` → TIMBRE `○●○○` → MORPH `○○●○` → HARMONICS `○○○●`. v0.11: the same glyphs as the X side (Adara) — the counting bar v0.10 used on t is gone, and TRIG deliberately shares `○●●●` with the X page's V/Oct | X1 / X2 / X3 destination: FM `●○○○` → TIMBRE `○●○○` → MORPH `○○●○` → HARMONICS `○○○●` → V/Oct `○●●●` → LEVEL `○○●●` (v0.10) → none `○○○○`. One LED under the Plaits fader of that parameter |
| T4 | **Y destination** (v0.14) · held 2 s: **UNPATCH Y** | **Y destination** (v0.14) · held 2 s: **UNPATCH Y**. [J] moved to SETTINGS T4 and now covers X **and** Y |
| F1 | **GTLT** (v0.17, #20): tilts t1's and t3's gate height around t2; 10 % centre detent = off; not on TRIG | **free** (v0.10) — was SCALE, now `••` + FFWD / RWD |
| F2 | gate length (Marbles' [E]-hold) | **free** (v0.10) — was X CLOCK; X keeps Marbles' own clocking and there is no control for it |
| F3 | gate-length randomness | reserved |
| F4 | LENGTH [I] (5 … 16 steps) | LENGTH [I] — the same value |
| RWD / FFWD with `••` | free | **next / previous included SCALE** (v0.10), no wrap |

⚠️ **v0.9's TRIG on/off toggles are gone.** t1–t3 now cycle a ring with TRIG as its first entry
and `none` as its last, so the old on/off is still one press away in either direction — and a t
gate can now also be a LEVEL gate or a stepped modulation. **t2 defaults to `none`**: it is the
master clock, driving the play-row clock and the Y divider whatever it is routed to, so routing it
to TRIG as well doubles every note the loop already plays.

### `••` double-tap — SETTINGS: the Y page, identical from both sides

| | |
|---|---|
| F1 / F2 / F3 / F4 | Y SPREAD / Y BIAS (detent) / Y STEPS (detent) / Y RATE (12 zones: 1/64 … 1/1 of t2) |
| T1 | **free** (v0.14) — Y's destination moved to `••` + T4 on the t and X pages |
| T2 / T3 | v0.11: `[F]` and `[G]` — does the one DEJA VU knob reach the t / X section? |
| T4 | **[J] voltage range, X *and* Y** (v0.14: one control, as Marbles' own `[J]` is) |
| RWD / FFWD | [B] clock range, as on the base pages |

The page breathes, like the PLAITS SETTINGS page. A single `••` tap returns.

### How the routes reach Plaits

| destination | Plaits input | scaling |
|---|---|---|
| FM | FM CV (`Modulations::frequency`), `frequency_patched` | 12 semitones per volt, **× the FM attenuverter** (PLAITS SHIFT F1) |
| TIMBRE | TIMBRE CV, `timbre_patched` | ±5 V → ±1.6, **× the TIMBRE attenuverter** (PLAITS SHIFT F2) |
| MORPH | MORPH CV, `morph_patched` | ±5 V → ±1.6, **× the MORPH attenuverter** (PLAITS SHIFT F3) |
| HARMONICS | HARMONICS CV, `harmonics_patched` (ours) | ±5 V → ±1, **× the HARMONICS attenuverter** (PLAITS SHIFT F4, v0.10) |
| LEVEL | LEVEL (`Modulations::level`), `level_patched` | 0 … 1; a t gate holds the VCA open for the gate's length. Adds to the SETTINGS F4 fader |
| V/Oct | V/Oct (`Modulations::note`) | 12 semitones per volt, unattenuated |
| TRIG | TRIG, as gates | t gates (their lengths reach Plaits), OR-ed; a new edge while TRIG is high is re-struck with one low Plaits block (0.5 ms) |

Scaling is Plaits' own for its CV inputs (`plaits/settings.cc`, default calibration). A **t gate
counts as 0 or +5 V** on any CV destination. Outputs on one destination add up (Adara: "attenuvert
them in Plaits") and **the sum is clamped to what one output could produce alone** (v0.10) — seven
outputs on TIMBRE is a legitimate thing to try and must not be able to drive Plaits past its
range. ⚠️ The attenuverters default to 0, so a route to FM / TIMBRE / MORPH / HARMONICS does
nothing until PLAITS SHIFT F1–F4 is raised — and while routed, that attenuverter no longer drives
Plaits' internal envelope. ⚠️ A destination counts as patched whether its gate is high or low: a
low gate is 0 V, not "nothing", or the attenuverter would change meaning several times a second.

### Defaults — also what `••` + PLAY held 3 s restores on MARBLES ("rip out the cables")

coin toss · **t2 → TRIG** (v0.12; t1 + t3 until v0.11) · **X1 → V/Oct** *(boot only — a rip routes **X2** to V/Oct instead, with t2 → TRIG and everything else out; #50)* (V/Oct has no attenuverter, so TIMBRE,
MORPH and FM stay on Plaits' internal envelope) · X2, X3, Y → none · RATE at centre =
**120 BPM** · clock range ×1 · `[J]` **INTELLIGENT**, one setting for X and Y (v0.13/v0.14; X was
0–2 V and Y was ±5 V before it existed) · STEPS just above centre, so X is quantized and Y is not ·
scale major · each X on its own t · gate length 50 % · LENGTH 8 · Y: spread / bias centre, steps 0,
1/8 of t2. The rip
resets routing, the buttons' settings, both SHIFT pages, the Y page and RATE; the other base
faders keep their values.

### ⚠️ DEJA VU LOCK is at F4's *centre*, not its bottom

Freeing T4 is the right call, but not for the reason given. The lock is **not** in the
bottom 10 % of the knob. From `marbles.cc:244–257`:

```c
const float d = fabsf(deja_vu - 0.5f);
if (d > 0.03f)      ui.set_deja_vu_lock(false);
else if (d < 0.02f) ui.set_deja_vu_lock(true);
```

A deadband around **12 o'clock** snaps to exactly 0.5, and 0.5 *is* the locked loop.
The bottom of the travel is `deja_vu = 0`, which is **fully random — the opposite of
locked.**

The DEJA VU *button* then gates the whole thing (`marbles.cc:295–298`):

| Button state | Effective value |
|---|---|
| `DEJA_VU_OFF` | `0.0` — knob ignored, no recycling |
| `DEJA_VU_ON` | the knob value |
| `DEJA_VU_LOCKED` | forced to `0.5` — knob ignored, locked loop |

**So: treat the button as permanently `DEJA_VU_ON` and bind F4 straight to the knob
value with the ±3 % centre deadband.** F4 then reaches all three behaviours on its own:

| F4 position | Behaviour |
|---|---|
| bottom | 0 — fully random, effectively off (matches your v0.1 intent) |
| **centre** | **locked loop** |
| top | probability of jumping around within the loop |

That is what genuinely makes the DEJA VU button redundant, and it is why freeing T4 costs
nothing. It also means F4 needs the same centre-snap treatment as the bipolar faders
below — the lock is unreachable by feel otherwise.

## Transport, state and power

### Play button (v0.9, M4 — Adara 2026-09-22): Marbles' clock, nothing else

| Gesture | Result |
|---|---|
| **Press** Play | start Marbles' clock (on a beat: the first t2 tick is immediate, and the DEJA VU loop restarts from its first step); the next press stops it. On either module |
| `••` + Play | **PRST** (#50): the current slot's glyph; pressed again while it shows, the next slot (4 → 1), loaded at once. Letting go of `••` leaves the browser: the glyph ends and the page you were on returns. With `••` down first, PLAY does not touch the clock |
| `••` + Play held 3 s | **rip out the cables** on the module on show: PLAITS — the full patch wipe (docs/DEFAULTS.md); MARBLES — the defaults above. Not after a PRST slot change in the same `••` hold |

The rip animation, on the track row, all while held (#50; through v0.7.2 two flickers, a
fade and black): the PRST slot glyph for 1.6 s, black 0.25 s, a rise to full over 0.4 s, the
Unpatch animation (0.75 s). At 3 s the reset happens and the page fades back in over 0.5 s.
With PRST off the glyph's 1.6 s are a slow fade of the face LEDs to black instead.
Letting go during the glyph is just a PRST press; letting go after it cancels (the SHIFT
screen returns over 150 ms). It is a shift use, so it never starts a power-off; the 30 s
backstop still applies.

~~Tap = TRIG, hold = clock~~ (v0.1–v0.8, built in M3): **Play no longer fires a TRIG** —
RWD does, and the FFWD burst. Simpler: one button, one job.

### FFWD / RWD on PLAITS (v0.9)

| | clock stopped | clock running |
|---|---|---|
| RWD | one TRIG | one TRIG (its own, on top of Marbles' gates) |
| FFWD held | TRIG burst at the division of Marbles' tempo | **ratchet**: Marbles' own clock runs at the division (1/32 = 8 ticks per beat) — t gates and X / Y values at that rate |
| `••` + FFWD / RWD | division finer / coarser, 1/1 … 1/128 (not during a burst or ratchet) | same |

At 1/4 the ratchet changes nothing, and at 1/2 and 1/1 it slows the clock: the division is
a note value, not a multiplier. The master clock is capped at 500 Hz.

### Output select — four modes on PLAITS SETTINGS T4 (SETTLED)

Every engine renders two signals: `Voice::Frame` carries `out` and `aux`, and
`Engine::Render()` fills both. SETTINGS T4 **cycles forward and wraps** — no reverse control.
It was `••`+T4 from M3f to v0.14; v0.15 (#11) moved it to the panel's free T4 so that the
shift layer's T4 is Unpatch and nothing else.

**M3g:** OUT+AUX runs through a peak limiter (Plaits' own limiter constants, ceiling 0.8 of
full scale; no gain change below it) — it was too hot in M3f.

⏳ **Idea, parked for after M4 (Adara, 2026-09-22) — do not build yet:** for OUT×AUX, which can
be quiet, rotate OUT's phase by +90° (a Hilbert-style all-pass network) before the multiply,
then add +2 dB. The aim is interesting timbres where AUX carries low frequencies. Cost to be
measured when it's picked up.

**Built in M3f** (`sp1_synth_set_output`): OUT+AUX is scaled ×0.71 (−3 dB) and saturated;
OUT×AUX is ×2 and saturated. A change cross-fades over one 5 ms audio block. The track row
flashes one LED for the mode — T1 OUT, T2 AUX, T3 OUT+AUX, T4 OUT×AUX — through the
engine-flash path. RAM only: a real power-off returns to OUT.

| | Mode | Cost per sample |
|---|---|---|
| 1 | `OUT` | free |
| 2 | `AUX` | free |
| 3 | `OUT + AUX` (summed) | 1 add |
| 4 | `OUT × AUX` (ring mod) | 1 multiply |

XOR is **dropped**. It was a chiptune/circuit-bend idea rather than a Plaits one, it
needed an arbitrary choice of integer representation to even be defined, and it needed a
limiter purely to be safe near a built-in speaker. Not worth a mode.

`OUT × AUX` still needs a gain-up (start ×2–×4, and it varies by engine): both signals sit
around ±1 so the product is smaller than either. Note `aux` is often *derived* from `out`
rather than independent — virtual analog does `aux[i] = (aux[i] - out[i]) * 0.5f` — so
ring-modding them is tamer than the classic effect. Musical where aux is a genuinely
different timbre; don't expect textbook behaviour everywhere.

### ⚠️ Cross phase-modulation: not reachable from the output stage

Proposed: AUX phase-mods OUT, then OUT phase-mods AUX.

**Phase modulation needs the oscillator's phase accumulator, and by the time we hold these
buffers the oscillators have already run.** `Engine::Render()` hands us two finished blocks
of audio. There is no phase left to modulate — modulating it for real means going *inside*
each engine, which is 24 separate surgeries and abandons the "lift Plaits unmodified"
approach that makes this port tractable at all.

**What is achievable cheaply is an approximation**, and it is a real technique rather than
a fudge: phase modulation of a signal is equivalent to a time-varying delay, so

```
out_pm[n] = out[n - d(n)]      where d(n) = depth · aux[n]
aux_pm[n] = aux[n - e(n)]      where e(n) = depth · out[n]
```

with short interpolated delay lines (64 samples each, ~512 bytes total). Cost is roughly
**10–15 cycles/sample for both directions — about 1 %** of the budget. Cheap.

Three honest caveats:

- It modulates the phase of an **already-bandlimited signal**, not an oscillator, so the
  sideband structure differs from true PM and it will alias somewhat. At small depths it
  reads as FM-like timbre; at large depths, as flanging.
- **Both directions must read the *unmodulated* originals.** Feeding each the other's
  modulated output makes it recursive and it can run away.
- It needs a **depth control**, which we have nowhere to put yet. The settings page is the
  natural home.

**Recommendation: defer, as you offered.** Not because of CPU — it is nearly free — but
because it is an approximation of what you pictured, and you should hear it before it
earns a permanent slot. It is also a mode-5 nicety while M1c through M3 are the critical
path. Revisit once there is a spare control and something to listen to.

### Bipolar faders: attenuverters and BIAS

Both Plaits' attenuverters and Marbles' BIAS controls are bipolar: **centre = 0, up =
positive, down = negative**. Plaits already maps this — `plaits/ui.cc:80–85` inits the
attenuverter pots with `scale 2.0, offset -1.0`, turning a 0…1 reading into −1…+1. Use
the same `PotController` with the same scale/offset for every bipolar fader in both
modules, so pickup behaves identically everywhere.

⚠️ **The SP-1's faders have no centre detent.** On Plaits' panel you can feel 12
o'clock; on a slide fader you cannot, so exact zero is unreachable by feel and an
attenuverter that never quite reaches 0 is maddening.

**Add a centre deadband that snaps to exact 0.** Marbles already uses this idiom for its
DEJA VU knob (`marbles.cc:244–257`): a ±0.03 band around 0.5 with the surrounding range
rescaled so nothing is lost.

### ✅ Centre detents (SETTLED 2026-09-21, Adara) — built in M3b

"Centre detent" = a band around the fader's centre that reads as exactly the centre value,
with the rest of the travel stretched so both ends are still reached.

- **10 % of travel** for every bipolar parameter: all three attenuverters, FINE TUNE, and
  the base-layer parameters of engines whose centre is an exact neutral point.
- **5 %** for FREQUENCY, in the range modes where its centre means something: full range
  (centre = **C4, MIDI 60**) and the single-octave ranges (centre = that octave's C). Not in
  the quantized-octave range (where F1 is a switch), and not at the bottom of OCTV, which is
  the full range with the detent deliberately off (v0.16).
- **Unipolar parameters use the whole fader, 0–100 %, no detent.**
- **Contextual:** which of HARMONICS / TIMBRE / MORPH is bipolar depends on the engine. The
  table, with the reason for each entry from the manual and the engine source, is in
  `firmware/src/sp1_plaits_ui.c` (`CENTRE[]`). Summary: VA+VCF H,M · 6-op ×3 M · wave terrain
  M · string machine T · VA H,T · waveshaping H · FM M · grain H · filtered noise H ·
  particle M · all others none.
- **Marbles (to settle when Marbles is built):** candidates are both **BIAS** controls (centre
  = 50 %), **STEPS** (centre = raw, between smooth and quantized), and **DEJA VU** (centre =
  locked loop, already settled below). RATE, JITTER, SPREAD look unipolar. Verify against
  `marbles/` source before building, as was done for Plaits.

### ✅ Pickup on every layer (built in M3b)

`PotController`'s catch-up (`POT_STATE_CATCHING_UP`, its 0.005 thresholds and skew
formula) runs for all three PLAITS layers, not only on return from a hidden parameter:
after any layer change a mismatched fader moves its value *relatively* and converges with
it by the end of travel. Nothing jumps. The track row shows the active layer's values, so
the target is always visible; the SETTINGS page breathes so a latched page is never
mistaken for BASE.

⚠️ **FINE TUNE acts only in the quantized-octave range (mode 9)**, as in Plaits
(`plaits/ui.cc`), where F1 becomes an octave switch and FINE is ±7 semitones around C. In
the default full range it has no effect. ❓ Open: make it a global ±1 semitone trim instead?

### Power states: OFF / STANDBY / ON

**Naming: "standby."** Conventional, instantly understood, and it matches the
vocabulary the SP-1 community already uses — `sp1-tape-looper` calls its equivalent the
"charge-standby gauge". `SP1_STATE_STANDBY` in code. ("Dock" is the more characterful
alternative if you'd rather; "sleep" is wrong, since the device is awake enough to
render a display.)

**This completes something that was previously a workaround.** You cannot `SYSTEM_OFF`
while USB is plugged — with VBUS already high there is no wake edge, so the device goes
dark until the cable is pulled. M0 papered over that with a soft reset. Standby is what
that transition should actually land in, and once it exists no reset is needed at all:
the device is already running, so it just changes state.

| From | Event | To |
|---|---|---|
| OFF | USB plugged in | **STANDBY** (not ON) |
| OFF | `••` held 2 s | ON |
| STANDBY | `••` held 2 s | ON |
| STANDBY | USB unplugged | OFF (`SYSTEM_OFF` — now possible, no VBUS) |
| ON | `••` held 3 s, unplugged | OFF |
| ON | `••` held 3 s, plugged | **STANDBY** |
| ON | USB plugged / unplugged | stays ON |

**Display in standby: the play row is a charge bar**, filling from the `••` end toward
PLAY — the four side LEDs sit physically between those two buttons, so the bar runs the
length of that gap. PWM gives fractional fill, so four LEDs read as a continuous bar
rather than four steps.

- charging → the bar breathes slowly at the current fill level
- complete → solid
- the bar *is* the battery indicator; a second numeric display would add nothing

**And on demand, from anywhere: a `••` tap** (v0.13, M4c). The same bar, on the same row, for
1.5 s — while ON it overlays the play row from the moment `••` goes down and fades out; while OFF a
press shorter than the 1.5 s power-on hold (but at least 150 ms) flashes it **on release** and the
device stays off. Any other control cancels it, so the battery is what a press of `••` *alone*
gives you. `sp1_led_bar()` draws both this and the standby bar, so they cannot drift apart. In
STANDBY the flash is skipped — the bar is already there and breathing, and a static copy over it
would read as a 1.5 s stall.

⚠️ **It must stay a pure draw.** The ON overlay holds no state; the OFF flash sits in
`sp1_power_on_hold()`'s early-release branch, *after* the decision not to power on, and does not
touch the return value. The 1.5 s power-on hold, the 3 s shutdown and the 20 s backstop are exactly
as they were — rule 5a.

Model row stays dark in standby, and fills during the `••` hold exactly as it does at
boot — the same gesture gets the same feedback everywhere.

**Three things to get right:**

- **Feed the watchdog in standby.** It is a running state, not sleep.
- Standby draws more than `SYSTEM_OFF`, so a little of the charge current goes to the
  running CPU. Irrelevant while plugged, but do quiesce the codecs, oscillator and eMMC
  on entering it — a powered amp with no clock murmurs.
- ❓ Needs the battery ADC (AIN4) and the divider calibration, so this lands **after**
  M1b brings up the SAADC. The state machine can be built first with a stubbed
  percentage.

### Turning ON: the 2 s hold, and why it cannot work the obvious way

Wake from `SYSTEM_OFF` is a **hardware level-sense** on P0.27. The chip wakes and resets
the instant `••` goes low, and there is no CPU running to time a hold against. The hold
therefore cannot be enforced *before* waking.

What is implemented instead: wake on the edge regardless, then gate at boot —
`sp1_power_on_gate()` requires 2 s of continuous hold and calls `sp1_power_off()` on an
early release. Indistinguishable from the outside: a brush does nothing and shows
nothing. The model row fills as progress so the wait reads as deliberate rather than as
a dead device.

**Safety net:** the gate only engages when `••` is held *at boot*, so plugging in USB
always boots normally. No bug in the hold logic can make the device unreachable, and
T1+T4 at power-on is untouched regardless.

**One bug this surfaced:** holding `••` for 2 s to turn ON would have rolled straight
into the 3 s shutdown hold — 5 s of a single continuous press would boot the device and
immediately power it off, which reads as "it won't turn on". The shutdown gesture now
stays **disarmed until `••` is released once** after boot.

### Save state on shutdown (question 10) — yes. ⏳ NOT BUILT (status 2026-09-22)

Nothing persists yet: every power-off returns every setting to its default. Two docs disagree
on where it goes — this section says the last app-slot page (`0xFE000`), `docs/SAFETY.md`
("Unresolved: the page at `0xFF000`") says eMMC — to be settled in its own milestone, not
inside M4.

Possible, and there is a clean place for it that sidesteps the `0xFF000` dispute:
**carve the last 4 KB page out of the app slot itself.** Shrink `slot0_partition` from
`0xDF000` to `0xDE000` and put a settings page at `0xFE000`. That is inside the region
the bootloader hands us, so there is no conflict with the disputed page, and we use well
under 500 KB of the slot.

Three constraints that shape the design:

**Flash writes stall the CPU.** The nRF52840 has a single flash bank, so an NVMC page
erase (~85 ms) halts execution. That is catastrophic mid-audio and completely fine at
shutdown, where `sp1_power_off()` already spends ~320 ms on the LED sweep. **Save only at
shutdown.**

**Endurance is 10,000 erase cycles per page.** One erase per power-off means 10,000 power
cycles — ample. Periodic autosave would burn through it, so: no autosave, ever.

**A firmware reflash wipes it.** The flasher writes pages `0x20`–`0xFE`, which includes
ours. Settings reset when you update firmware. Acceptable, and it doubles as a free
factory reset.

Plaits and Marbles both have a `Settings` class (`plaits/settings.cc`, `marbles/settings.cc`)
with the struct layout and checksumming already written; only the flash backend needs
replacing.

### Wipe patch (question 11)

`••` + Play is cleanly detectable — `••` is a separate GPIO, so it does not collide with
any Play behaviour above.

⚠️ But a bare two-button combo with no confirmation will eventually wipe a patch someone
wanted. ❓ Suggest `••` + Play **held ~1.5 s**, with the four front LEDs draining as a
progress indicator so it is visibly cancellable. Same gesture, no accidents.

### Battery UI (question 8)

**Charge-level indicator and low-battery warning, both wanted.** Two problems to solve.

**Where does it go?** Front LEDs are the model display, side LEDs are the VU meter, and
neither should be stolen in normal use. Proposed:

- **Plugged in and powered off:** a charge-standby display owns all 8 LEDs. This is also
  forced on us — see the `SYSTEM_OFF` note under Charging: you cannot sleep while plugged.
- **Running:** battery level shown on the side (VU) row while `••` is held, since that
  layer is already a shift.
- **Low battery:** overrides everything with a distinct slow pulse on the front row. It
  has to be unmissable, because the alternative is the device dying mid-performance.

**⚠️ The divider ratio is unknown — but you do not need a multimeter.**

I asked for a hand measurement last round. Retracting that: it would mean opening the
device to reach the cell, which is not worth the risk on a one-off unit, and there is a
better way that needs no disassembly.

**The charger gives us a known reference for free.** The BQ24232 terminates at **4.20 V**,
and it tells us when it has: `nCHG` goes high (charge complete) while `nPGOOD` stays low
(still plugged). At that moment the cell is at 4.20 V by definition.

So the calibration is one reading, taken by the firmware:

1. Charge to full. Track LED 1 lit, LED 2 dark = plugged and complete.
2. Unplug (so the charger is not holding the terminal voltage up).
3. Read **raw AIN4** and print it over the console.
4. `V_adc = raw / 4095 × 3.6 V` (gain 1/6, 0.6 V internal reference → 3.6 V full scale).
   `divider_ratio = 4.20 / V_adc`.

That needs the console, so it is an **M1 task**, not something for you to do by hand now.
A /2 divider would read ≈2.10 V ≈ raw 2389; anything near that confirms the guess.

A second caveat regardless of ratio: **Li-ion voltage is not linear in charge.** Percentage
from voltage alone is rough, and worse under load. Standard fix is a small voltage→percent
lookup table for Li-ion, sampled at a consistent load. Fine for a 4-LED indicator; do not
promise a number.

Low-battery cutoff is the one place we should exceed the reference firmwares —
`sp1-tape-looper` has no cutoff I could find, and holding a Li-ion cell in deep discharge
is the only way our firmware can damage this battery. The BQ24232 cannot protect the cell
from our own load.

### LED animations and PWM

**Yes, and better than the reference firmwares do it.** The nRF52840 has four PWM
peripherals of four channels each; the 8 LEDs split exactly across two (PWM2 and
PWM3, as `ericlewis/sp1-midi` wires them). Hardware PWM with EasyDMA plays a
brightness sequence with **no CPU cost at all** — which matters when the audio
deadline is 1333 cycles.

Note `sp1-tape-looper` deliberately drives the LEDs as raw GPIO with *software* PWM,
and had to promote its LED timer to a zero-latency IRQ to stop visible flicker at
low duty. **We should not inherit that choice.** Hardware PWM is smoother, free,
and removes an interrupt that would otherwise compete with audio.

Brightness must be gamma-corrected — LED perceived brightness is roughly the square
of duty cycle, and linear ramps look wrong. A 256-entry gamma table is 256 bytes.

### Charging

**The safety is in silicon, not software — this is lower-risk than it looks.**

The BQ24232 is a complete charger IC: it runs the CC/CV profile, termination, safety
timer and thermal regulation autonomously. **Firmware cannot set the charge current.**
Per the SP-1-dev wiki, ISET (P1.00) is *"the current monitoring output"* — an input to
the MCU through a divider, not a control. The part is hardware-configured for 500 mA
max USB draw.

So the firmware's entire role is:

| Pin | Direction | Firmware job |
|---|---|---|
| nCE P0.21 | out, active low | **drive low to enable charging** — the one thing we must not forget |
| nPGOOD P0.24 | in, open-drain, low = USB good | UI |
| nCHG P0.22 | in, open-drain, low = charging | UI |
| ISET P1.00 | in (ADC) | optional: display actual charge current |
| BATT AIN4 P0.28 | in (ADC, divider) | battery gauge + **low-battery shutdown** |

`sp1-tape-looper`'s `charger_init()` does the nCE/nPGOOD/nCHG part correctly and is
worth lifting verbatim. But it does **not** read ISET and has no low-battery cutoff
I could find. **That cutoff is the one place we should exceed what's out there** —
holding a Li-ion cell in deep discharge is the only way firmware can damage this
battery, and the charger IC can't protect against our own load.

**One non-obvious gotcha to carry over:** you cannot `SYSTEM_OFF` while USB is
plugged in — with VBUS already high there is no wake edge and the device goes dark
until a replug. tape-looper's answer is `NVIC_SystemReset()` into a charge-standby
screen instead. We need the same.

### Startup sound (SETTLED — synthesised, four chosen models)

Synthesised by Plaits at boot: pick one of four engines at random, fire one TRIG, let the
LPG decay. ~20 bytes per patch, no sample storage, and it self-tests the whole audio path
— a silent or wrong chime means the codecs or I²S failed before you touch a control.

**Your four, mapped to engine indices in `voice.cc`'s registration order:**

| Your name | Engine | idx | instr/sample |
|---|---|---|---|
| Hardsync | Virtual analog (classic) | **8** | 465 |
| Filter-noise | Filtered noise | **17** | 426 |
| Chord | Chords | **14** | 596 |
| Rings | **String (inharmonic)** | **19** | 600 |

**SETTLED: String (engine 19)**, the plucked three-voice one, not the modal resonator.

Note "Hardsync" is not a model — it is a *region* of the virtual-analog engine's TIMBRE
control ("narrow pulse to full square to hardsync formants", per the manual). So that
patch is engine 8 with TIMBRE up high. Worth storing the H/T/M values deliberately rather
than randomly, which is the point below.

**Retraction:** I advised excluding the expensive engines from the boot chime. That was
overcautious and I was wrong. At boot there is no sequencer, no UI load and no other
audio — it is the moment of *maximum* CPU headroom, so even Modal at 858 is comfortable.
It is arguably the best thing to boot with, because if the heaviest engine renders cleanly
at startup you have learned something real. **Modal is fine. Use it if you want it.**

Do store hand-picked H/T/M per patch rather than randomising them: those three parameters
mean something completely different in every engine, so random values give an ugly chime
about half the time. Four fixed patches, ~20 bytes each.

### File transfer (SETTLED — CDC now, MSC later)

CDC + a browser page, the way `solderless.engineering` and `feldd` already do it over
WebSerial. CDC is already planned for M1, so this is nearly free. Mass storage stays on the
list for save-state management and eventual sample support — revisit when eMMC lands.

Scale check that makes this easier than expected: a Plaits **wave terrain is 4096 bytes** —
a 64×64 grid of `int8_t` (`wave_terrain_engine.cc:54`). One flash page. Even a slow CDC
link moves one instantly, and hundreds fit in spare app flash without touching eMMC.

Only two engines implement `LoadUserData()`: wave terrain (a custom terrain becomes a
**9th** alongside the 8 built-ins — `num_terrains = user_terrain_ ? 9 : 8`) and six-op FM
(DX7 SysEx banks). Stock Plaits allows **one 4 KB user page total**, tagged to a single
engine slot. We have 892 KB and a 4 GB eMMC, so holding many terrains and many DX7 banks
at once, switchable from the panel, is a genuine improvement on the original module.

### Manuals

Mutable Instruments closed in 2022 and `mutable-instruments.net` is gone. The
official archive is **`pichenettes.github.io/mutable-instruments-documentation`**,
and I have pulled from it:

- Plaits manual — all 16 original models with per-model HARMONICS / TIMBRE / MORPH
  behaviour, the LED scheme, the alternate-parameter mode
- Plaits 1.2 firmware notes — the 8 added models and the user-data transfer story
- Marbles manual — full panel reference, every knob and button by its panel letter,
  t1/t2/t3 and X1/X2/X3, DEJA VU, the Y generator

I have the content, not PDFs. Enough to specify against; say the word for a deeper
pull on any single model.

---

## Open decisions

**Settled:** shift gesture (`••` only) · 24 models on 4 LEDs, 2 brightness tiers ·
T1+T4 · pickup/catch on all faders · bipolar faders with a centre snap · Play = tap-TRIG
/ hold-clock · `••`-hold F4 = LEVEL with a 5 % disconnect · ~~wipe = `••`+Play held~~ **`••`+Play held 1.5 s = reset SHIFT + SETTINGS to defaults (v0.8c)** ·
Marbles T4 reserved · DEJA VU lock at F4 centre · boot chime = String / Hardsync /
Filter-noise / Chord · output select = 4 forward-cycling modes on `••`+T4 (XOR dropped) · CDC transfer now, MSC later ·
LED engine on hardware PWM with gamma · 3 s cancellable shutdown · 2 s power-on hold ·
**standby** as the off-and-charging state · **`••`-hold suppression**: touching any other
control while `••` is held marks it a shift, suppressing shutdown for the rest of that
hold and cancelling the animation if it had started — implemented in `sp1_shift_used()`,
and the control layer must call it from M1c on.

**Still open:**

- ⏳ **Cross phase-modulation as a 5th output mode** — deferred by agreement; cheap in CPU
  but an approximation of true PM, and it needs a depth control
- ⏳ **Battery divider ratio** — calibrated in firmware once the SAADC is up
- ✅ Marbles SETTINGS page — the Y page (v0.9, M4)
- ⏳ Mass storage — deferred
- ⏳ **OUT×AUX: +90° phase rotation on OUT, +2 dB** — after M4 (see "Output select")
- ⏳ **Save state** — not built; its own milestone after M4 (see "Save state on shutdown")

**Hardware status, 2026-09-20.** M0 ✅ · M1a ✅ · M1b ✅ — all fully closed on hardware.
Charge-complete (T3 going out at 100 %) and the reflash round-trip are both confirmed. The
only outstanding build is the tap-flicker fix (deferred PWM init) plus the widened
`sp1_shift_used()`.
