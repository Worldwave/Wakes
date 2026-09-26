# wakes-sp1 — the whole control map

Every control, on every page, in one place. This is the **reference**: what each control does.
`docs/UI-SPEC.md` is the **design record**: why it does that, what was considered instead, and
what is still open. When the two disagree, the firmware is right and both documents are wrong —
say so in a test log.

Accurate as of **M4a**. Nothing below is read by the build.

---

## The hardware, once

| | |
|---|---|
| **F1–F4** | four faders, bottom to top, 0–3701 counts |
| **T1–T4** | the model row's four buttons, and its four LEDs |
| **PLAY** | the fifth button on that ladder |
| **VOL+ · VOL− · FFWD · RWD** | the second ladder |
| **`••`** | its own GPIO, so it reads independently of everything else |
| **8 LEDs** | the **model row** (T1–T4) and the **play row** (four, `••` end → PLAY end) |

**Two modules, one set of controls.** `T4` swaps between **PLAITS** (the voice) and **MARBLES**
(the random generators). Both always run and both always feed the audio; the swap only changes
what the controls and the LEDs are pointed at. The device starts on PLAITS.

**Three ways `••` is used**, and they never collide because they are told apart by *time*:

| gesture | meaning |
|---|---|
| held, with another control used | **SHIFT** — the layer named below |
| tapped (≤ 300 ms, nothing else touched) | page: one tap leaves SETTINGS, two enter it — **and the battery, below** |
| held 3 s with nothing else touched | power off (animation; release cancels) |
| held 30 s, unconditionally | **force power off** — the safety backstop; the model row pulses from 25 s |

⚠️ **The `••` hold behaviours are not to be changed** — not the shift hold, not the shutdown
hold, not the backstop — except on Adara's explicit instruction. `firmware/src/sp1_ui_timing.h`
and `docs/SAFETY.md` explain what went wrong the one time a condition was put on power-off.

### The battery display — a `••` tap, anywhere (M4c)

Pressing `••` shows the **charge** on the **play row**, for 1.5 s, as the *same picture* STANDBY
draws while charging: filling from the `••` end toward PLAY.

| | |
|---|---|
| **while ON** | it appears the moment `••` goes down and overlays the play row, and **fades out over 300 ms** — on release, or at 1.5 s if you keep holding |
| **while OFF** | a press shorter than the 1.5 s power-on hold flashes the bar **on release**, then the device stays off. Presses under **100 ms** are ignored, so a brush of the button does nothing |
| **cancelled by** | using any other control — that one CUTS instantly rather than fading, because you have asked to see something else. The battery is what you get when you press `••` and *only* `••` |
| **not in STANDBY** | the charge bar is already on that row, live; flashing a static copy over it would only pause the breathing |

⚠️ **This adds nothing to the power path and takes nothing away from it.** The ON overlay is a
draw, not a state; the OFF flash happens in `sp1_power_on_hold()`'s early-release branch **after**
the decision not to power on, with the return value untouched. The 1.5 s power-on hold, the 3 s
shutdown and the backstop are all unchanged — see the backstop in `sp1_power_tick()`
(`firmware/src/sp1_power.c`). (The backstop itself moved 20 s → **30 s** in M4e, at Adara's
request: a threshold change, never a condition.)

⚠️ The bar is `sp1_led_bar()`, and STANDBY's charge bar now calls the same function, so the two
pictures cannot drift apart.

---

## What can be pressed together

| | |
|---|---|
| **`••` + anything** | always available: `••` is on its own GPIO, so it is not part of either ladder |
| **T1–T4 and PLAY** | on one ladder (AIN0). Combinations are electrically *decodable*, but the decoder **dispatches single presses only** — a recognised chord resolves to "nothing pressed". A chord resolved to its nearest single would fire a real action from a fumbled press, and above PLAY the states sit only 26–55 counts apart. `sp1_chord_mask()` exposes them if a later page wants one. |
| **VOL+ · VOL− · FFWD · RWD** | on the other ladder (AIN1), which has **no usable combinations at all** — not a policy, the hardware |
| **faders + anything** | always; a fader read during a button press is discarded (or, for FFWD / RWD, corrected) so a press cannot bend a parameter |

So the combination space is exactly: **`••` + one button**, on either ladder. That is what the
`••` + … rows below are, and it is the whole supply.

---

## PLAITS

### Faders

| layer | how you get there | F1 | F2 | F3 | F4 |
|---|---|---|---|---|---|
| **BASE** | the default | **FREQUENCY** (5 % centre detent) | **TIMBRE** | **MORPH** | **HARMONICS** |
| **SHIFT** | `••` held | **FM** attenuverter | **TIMBRE** attenuverter | **MORPH** attenuverter | **HARMONICS** attenuverter |
| **SETTINGS** | `••` double-tap (latched; one tap returns) | **OCTAVE range** (11 steps) | **LPG colour** | **LPG decay** | **LEVEL** |

- F2–F4 on BASE mean something different per engine, and some of them get a 10 % centre detent.
  `docs/PLAITS-ENGINES.md` has both, per engine.
- **Every SHIFT fader is an attenuverter**: bipolar, 10 % centre detent, centre = a gain of
  **zero**. With PLAY as the trigger and nothing routed to that input, the attenuverter becomes
  the **depth of Plaits' internal decay envelope** on that parameter. The HARMONICS one is an
  addition of ours — Plaits' panel has no such control (M4a; `firmware/CMakeLists.txt` overrides
  `voice.{h,cc}`).
- **LEVEL below 5 % is DISCONNECTED**, which is not the same as zero: disconnected means TRIG
  plucks the note through the LPG, connected means the VCA is held open by LEVEL — the drone.
- **OCTAVE range** is Plaits' own eleven modes, `plaits/ui.cc` verbatim: 0 = LFO, 1–8 = one
  octave each ±7 semitones, 9 = octaves quantized, 10 = the full 96-semitone range (the default).
- Every layer keeps its own four values, and a fader that does not match the value it is
  showing **catches up** rather than jumping (Plaits' own `PotController` behaviour).
- ⚠️ **Only BASE is ever seeded from the physical faders**, and only on the first ON after boot.
  SHIFT and SETTINGS always start from defaults — attenuverters at zero.

### Buttons

| control | no `••` | with `••` held |
|---|---|---|
| **T1** | **flash the engine you are on** — "what am I playing?" | held 2 s: **UNPATCH FREQUENCY** |
| **T2** | previous engine (flashes its glyph) | held 2 s: **UNPATCH TIMBRE** |
| **T3** | next engine | held 2 s: **UNPATCH MORPH** |
| **T4** | **swap module** → MARBLES | **output select**: OUT → AUX → OUT+AUX → OUT×AUX → wrap · held 2 s: **UNPATCH HARMONICS** |
| **PLAY** | Marbles' clock **run / stop** | held 3 s: **rip out the cables** (see below) |
| **RWD** | one **TRIG** | **coarser** burst division |
| **FFWD** | held: **burst** — re-triggers on the 1/div grid, running or stopped | **finer** burst division |
| **VOL− / VOL+** | output level, 3 dB steps | **the soft-clip drive** — see below |

- The **division** is 1/1 … 1/128, default 1/32: the burst's rate. Set it **between** rolls:
  FFWD and RWD are one physical rocker, so `••` + RWD during a roll is ignored.
- ⚠️ **FFWD does the same thing whether or not the clock runs** (M4e). While it runs, the 1/div
  grid is read off **Marbles' own master ramp**, so a burst is exact subdivisions of the clock
  you can hear and holding it changes the clock not at all. Through M4d, FFWD while running was
  a RATCHET that multiplied Marbles' RATE — which is what made the rhythm shift, because
  changing the rate mid-cycle moves the next tick and releasing leaves the phase displaced.
  ⚠️ So FFWD now **re-triggers** the note Marbles is holding rather than making the sequence
  advance faster: a roll, not an arpeggio. Driving the clock at a subdivision instead is
  a separate option, parked for a community vote (Adara).
- **Rip out the cables** = `••` + PLAY held 3 s: a **full patch wipe** of the module on show
  (`docs/DEFAULTS.md` is the spec). On PLAITS: engine → slot 1, the four BASE faders → that
  engine's neutral state (0.5 for a bipolar parameter, 0 for a unipolar one, F1 → centre = C4),
  all four attenuverters → 0, SETTINGS → defaults, quantizer → off, output → OUT, division → 1/32.
  The row flickers twice, fades to black, and only commits at 3 s; letting go earlier cancels.
  ⚠️ Until M4b this kept the engine and BASE and was a modulation reset. It is a patch wipe now.
  ⚠️ **The faders do not move** — pickup catches them up on the next touch, so straight after a
  rip the instrument sounds neutral while the faders still look wrong. By design.
  ⚠️ The **soft-clip drive is cleared by a PLAITS rip only** (M4e). `••` + VOL reaches it from
  both pages, but Marbles makes no audio, so the drive is not part of its patch. VOL itself and
  which module you are on are kept by either rip.

### The SETTINGS panel's buttons

`••` double-tap latches SETTINGS; **a single `••` tap is the way out**, and it is the only
way out. That is deliberate: SETTINGS used to be a page where every base-layer button still
worked, so the engine or the module could be changed by accident while setting a parameter.
Nothing on it reaches anything but the panel now.

| control | on SETTINGS |
|---|---|
| **T1** | **flash the scale you are quantized to** (`○○○○` when off) — the panel's equivalent of the engine flash |
| **T2 / T3** | previous / next **FREQUENCY scale quantization**. **Engine select is unbound here** |
| **T4** | **unbound.** Not the module swap, so the panel cannot be left by accident |
| **PLAY · RWD · FFWD · VOL** | **unchanged and live.** You need to keep playing while you set a scale |
| **`••` + anything** | as on the base page — holding `••` shows the SHIFT layer, so the panel's own bindings are only on the unshifted buttons |

⚠️ **UNPATCH is not available on this panel** (M4e). Its T1–T4 are scale controls, not routings,
so there is no cable for the gesture to cut and an animation followed by nothing would be a lie.
Tap `••` to get back to the base page first.

#### FREQUENCY scale quantization (T2 / T3)

Restricts the notes F1 can select to one of the seven scales Marbles' X quantizer offers
(`docs/MARBLES-SETTINGS.md` has the table, the glyphs and the exact note sets). Plaits keeps
its **own** index into that table, so the two modules can be in different scales. `○○○○` = off,
first in the ring, and only *included* scales are reachable. No wrap.

- **It composes with the OCTAVE range rather than replacing it.** The range decides the span
  F1 covers; the scale decides which notes inside that span exist.
- **Three of the scales are microtonal** — pelog, bhairav and shri put their degrees at
  1.53, 0.90, 3.16… semitones, not on the semitone grid. That is the point of them.
- **OCTAVE mode 9 is special.** On its own it quantizes F1 to whole octaves. With a scale
  selected it becomes a sweep of **scale degrees** across those nine octaves — seven degrees
  gives 63 steps, about 59 fader counts each.
- **Mode 0 (LFO) is never quantized.** Its "notes" are rates.
- ⚠️ **Mutually exclusive with a Marbles output on V/Oct, in both directions.** Selecting a
  scale **unpatches every V/Oct route**; routing an output to V/Oct **turns the scale off and
  opens the range to maximum**, so that output predictably controls the pitch. Note the
  asymmetry: *locking* the octave range unpatches nothing, because a locked range and an
  external pitch source are a sensible pair — a quantizer and one are not.

#### The soft-clip drive (`••` + VOL, on either module, any page)

Gain **past** the output stage's maximum, into a soft clipper — the synth's overdrive.

| | |
|---|---|
| **`••` + VOL+** | one step **up**. The model row shows a bar |
| **`••` + VOL−** | one step **down**. Step 0 is off |

Four steps above off, and they are **uneven**: **+3 / +8 / +15 / +24 dB** (M4e). Adara asked
for more extreme compression at the top without losing a gentle first setting, so the ladder
opens at +3 dB — a fattener — and ends at +24 dB, where most of the waveform is flat.

⚠️ **VOL− no longer toggles** (M4e). It used to switch the stage off and restore the last
setting on a second press, which made one rocker mean "more" in one direction and "all or
nothing" in the other, with no way to come down a single step. Off is just the bottom of the
ladder now.

⚠️ **Step 4 is a square-wave fuzz and it is LOUD.** Measured: 58 % of samples sit pinned at
full scale and the output is +9.2 dB above dry. That is the requested extreme, not a fault —
but expect to pull VOL down when you reach for it.

It sits on Plaits' OUT and AUX separately, **before** the ring modulator and before the
OUT+AUX limiter, so the limiter catches what the clipper produces.

⚠️ **It is a saturator, not a volume control.** Measured: +12 dB in comes out +7.1 dB louder
and peaks stop just short of full scale without ever hard-clipping. Quiet material comes up,
loud material compresses and gains harmonics. Step 0 bypasses the stage completely — not
"clip at unity gain", which would attenuate by ~2 dB and colour everything. Costs ~+4.7 points
of the CPU budget at peak, and only while it is on. It resets to off on every power-on.

### LEDs

| | |
|---|---|
| **model row** | the active layer's four values. A bipolar parameter shows its **magnitude**, so the centre is dark and both extremes are full; the fader position says which side. SETTINGS **breathes** so a latched page is never mistaken for BASE. A change to a discrete setting (engine, output mode, division) takes the row over for 0.7 s and cross-fades back. |
| **play row** | the **dB meter**, filling from the `••` end, four 6.02 dB bands from −24 to 0 dBFS, with a dim "signal present" glint on LED 1 from −48 dB. Behind it, at ~10 %, Marbles' clock steps once per beat so ON is visible in silence. |

---

## MARBLES

Two base pages, **t** (`●●○○`) and **X** (`○○●●`), chosen with T2 and T3; a SHIFT layer for each;
and one SETTINGS page (Y) reached the same way as on PLAITS and identical from either side.

### Faders

| layer | how you get there | F1 | F2 | F3 | F4 |
|---|---|---|---|---|---|
| **t BASE** | T2 | **RATE** `[A]` (±5 oct) | **t BIAS** `[D]` * | **JITTER** `[C]` | **DEJA VU** `[H]` * |
| **t SHIFT** | T2, then `••` held | *(free)* | **gate length** | **gate length randomness** | **LENGTH** `[I]` |
| **X BASE** | T3 | **SPREAD** `[K]` | **X BIAS** `[L]` * | **STEPS** `[M]` * | **DEJA VU** `[H]` * |
| **X SHIFT** | T3, then `••` held | *(free)* | *(free)* | *(free)* | **LENGTH** `[I]` |
| **SETTINGS (Y)** | `••` double-tap | **Y SPREAD** | **Y BIAS** * | **Y STEPS** * | **Y divider** (12 steps) |

\* 10 % centre detent. **DEJA VU's centre is LOCK**, not its bottom: below the centre the loop is
recorded, above it the loop is replayed, at it the loop is frozen.

- **F4 is ONE control on both pages, not two that are kept in step.** Marbles has one DEJA VU
  knob and one LENGTH knob, so the X page's F4 addresses the t page's value rather than
  mirroring it. Mirroring made the two pages keep separate pickup state, which could put F4
  back into catch-up for a value that had not moved.
  LENGTH is 1 … 16 steps, through Marbles' own non-linear table.
- ⚠️ **PLAY rewinds the DEJA VU loop, it does not clear it** (Adara asked, M4e). Marbles'
  `RandomSequence::Reset()` only sets `step_ = length_ - 1`; the loop buffer is untouched. What
  happens is that PLAY calls `Reset()` on every channel — ours, and how PLAY starts both on a beat
  *and* at the top of the loop — so you always hear the loop from step 0 rather than from where you
  stopped. With a short loop that reads as "it cleared". Separately, while stopped every Marbles
  output reads 0 V and the routing is unpatched, so the pitch you hear then is not the sequence at
  all. Making PLAY resume where it stopped is deleting one call, at the cost of "PLAY always starts
  at the top of the loop" — worth trying both before deciding.
- **`[F]` and `[G]` (SETTINGS T2 / T3) decide which sections that one DEJA VU knob reaches.**
  Both start on. Turning one off makes that side ignore the knob and run free, which is what
  Marbles does with an unlit `[F]` / `[G]`. Glyph: `●●○○` t on, `○○●●` X on, `●●●●` both,
  `○○○○` neither — the page vocabulary, so it says which *side*.
- **X SHIFT F1–F3 are free** as of M4a. F1 was SCALE (now `••` + FFWD / RWD) and F2 was the X
  clock source (unbound; X keeps Marbles' own — X1 on t1, X2 on t2, X3 on t3). Both fields are
  still in `sp1_marbles_params` so M5 can re-expose them.

### Buttons

| control | t page | X page | SETTINGS (Y) |
|---|---|---|---|
| **T1** | `[E]` **next t model**, one of six | `[N]` **X diversity**: identical / bump / tilt | **unbound**, reserved |
| **T2** | → t page | → t page | `[F]` **DEJA VU on the t side** |
| **T3** | → X page | → X page | `[G]` **DEJA VU on the X side** |
| **T4** | **swap module** → PLAITS | **swap module** → PLAITS | `[J]` **voltage range, X *and* Y** |
| **PLAY** | clock **run / stop** | clock run / stop | clock run / stop |
| **RWD / FFWD** | `[B]` **t range** ×0.25 / ×1 / ×4, no wrap | the same | the same |
| **`••` + T1 / T2 / T3** | **t1 / t2 / t3 destination** · held 2 s: **UNPATCH** that output | **X1 / X2 / X3 destination** · held 2 s: **UNPATCH** that output | — |
| **`••` + T4** | **Y destination** · held 2 s: **UNPATCH Y** | **Y destination** · held 2 s: **UNPATCH Y** | — |
| **`••` + FFWD / RWD** | *(free)* | **next / previous scale**, no wrap, excluded scales skipped | *(free)* |
| **`••` + PLAY** | held 3 s: **rip out the cables** | the same | the same |
| **VOL− / VOL+** | output level | output level | output level |

⚠️ **T4 is `[J]` on the SETTINGS page, so the module swap is unavailable there.** Tap `••` once
to leave SETTINGS first. This is the only place a control is not where the rest of the UI puts it.

`[J]` wraps through four: 0–2 V `●○○○` · 0–5 V `●●●○` · ±5 V `●●●●` · **INTELLIGENT** `●○●○`, which
is the default. INTELLIGENT reads each output's destination *and the current engine's* parameter
polarity and gives that channel its own range — so X1 on V/Oct and X2 on FM sit at 0–2 V and ±5 V
simultaneously. `docs/MARBLES-SETTINGS.md` has the full mapping.

⚠️ **One `[J]` for X and Y since M4e** (Adara). There used to be two — X's on `••` + T4 of the X
page and Y's on SETTINGS T4 — but in Marbles' own manual `[J]` is a single button and Y's range is
that same button with a modifier held, so two independent settings were never in the hardware being
copied. Setting it sets both, and `••` + T4 is **Y's destination** instead, beside the other six
destination buttons where it belongs. SETTINGS **T1 is now free**.

### UNPATCH — `••` held, then a T button held (M4e)

The one long press in this UI. It cuts **one cable** rather than the whole patch, which is the
gesture that was missing between "change this destination" and "rip out everything".

| | what it clears |
|---|---|
| **PLAITS** T1 / T2 / T3 / T4 | every Marbles output aimed at **FREQUENCY / TIMBRE / MORPH / HARMONICS** — the button names a *parameter*, in fader order under the four SHIFT attenuverters |
| **MARBLES** t page T1–T3 | **t1 / t2 / t3**'s destination → `none` |
| **MARBLES** X page T1–T3 | **X1 / X2 / X3**'s destination → `none` |
| **MARBLES** T4, either page | **Y**'s destination → `none` |

⚠️ **PLAITS T1 covers V/Oct *and* FM.** Both modulate the pitch and V/Oct has no button of its
own, so T1 is "stop anything changing my pitch" in one gesture.

**Timing**, and it is Adara's to the millisecond: nothing for the first **1.25 s**, then a
**0.75 s** animation on the four track LEDs, and the clear commits when the animation **ends** —
at exactly **2 s**. Letting go earlier cancels and clears nothing, like every other hold here.

The animation, in order, all as fades rather than latches: the row fades out → two quick blinks
to 80 % → a third that rises to 100 % and holds → a sweep to dark **from the middle outwards**
(T2/T3 first, then T1/T4) → cross-fade back to the page.

⚠️ **Because T1–T4 now mean two things under `••`, their ordinary action fires on the button's
RELEASE**, and only if Unpatch did not happen. That is unavoidable with two meanings on one
button, and destination cycling does feel slightly different for it.

⚠️ **It is not available on either SETTINGS page** — there the T buttons are page controls, not
routings, so there is no cable to cut.

⚠️ **It cannot affect power-off.** `••` + a button is already a shift use, so the 3 s shutdown is
suppressed exactly as before, and the unconditional backstop is untouched. 2 s against 30 s.

⚠️ **There is exactly ONE long press in this UI: Unpatch, below** (M4e). M4a removed the last
one — Marbles' 2 s `[E]` hold for its second t-model bank, whose six models are one ring here —
and scale selection, which Marbles puts on a long press of `[J]`, is `••` + FFWD / RWD. Unpatch
is the deliberate exception, because it is destructive.

### Routing — where a Marbles output goes

Applied **only while the clock runs**. Stopped, every Marbles input to Plaits is unpatched and
Plaits behaves as if Marbles were not there.

Each t output takes one destination from its own ring, and each X / Y output from its own. Both
rings include **none**, so any output can be silenced.

| ring | order a press cycles it |
|---|---|
| **t1–t3** | none → TRIG → LEVEL → FM → TIMBRE → MORPH → HARMONICS → *(wrap)* |
| **X1–X3, Y** | none → FM → TIMBRE → MORPH → HARMONICS → V/Oct → LEVEL → *(wrap)* |

Destination glyphs — **note the identical glyphs between TRIG and V/Oct, it is intentional**

| destination | on t1–t3 | on X1–X3 / Y |
|---|---|---|
| none | `○○○○` | `○○○○` |
| FM | `●○○○` | `●○○○` |
| TIMBRE | `○●○○` | `○●○○` |
| MORPH | `○○●○` | `○○●○` |
| HARMONICS | `○○○●` | `○○○●` |
| LEVEL | `○○●●` | `○○●●` |
| TRIG | `○●●●` | *(t only)* |
| V/Oct | *(X / Y only)* | `○●●●` |

`○●●●` therefore means TRIG on the t page and V/Oct on the X page. They can never appear on the
same page. **If that ever reads as ambiguous on hardware, unify both sides on the t bar** — it is
one table in `sp1_marbles_ui.c`.

Scaling is Plaits' own CV calibration (`plaits/settings.cc`), per 5 V: **V/Oct and FM 60
semitones**, **TIMBRE and MORPH 1.6**, **HARMONICS 1.0**, and LEVEL 0 … 1. FM, TIMBRE, MORPH and
HARMONICS are then scaled by their attenuverters; V/Oct is not (Plaits has no V/Oct attenuverter,
which is exactly why X1 → V/Oct is the default: it leaves all four attenuverters free for the
internal envelope). A **t gate counts as 0 or +5 V** on a CV destination.

⚠️ **Outputs sharing a destination add up, and the sum is clamped** to what one output could
produce alone. Seven outputs on TIMBRE is a legitimate thing to try and must not be able to drive
Plaits past its range.

⚠️ **A destination is patched whether its gate is high or low.** A low gate is 0 V, not
"nothing" — if the patched flag followed the gate, Plaits would hand the parameter back to its
internal envelope between gates and the attenuverter would change meaning several times a second.

### Defaults, and what "rip out the cables" restores

| | |
|---|---|
| routing at **boot** | **t2 → TRIG** (the master clock, so every tick fires a note — the most predictable "press PLAY and hear something" default, with no rhythmic randomness); **X1 → V/Oct**; everything else → none |
| routing after a **rip** | **everything → none.** "Rip out the cables" means what it says, so PLAY makes no sound until something is dialled back in |
| a rip also | **re-seeds and re-draws the DEJA VU loop**, and **leaves the clock running** — its phase jumps once. Every t and X BASE fader resets; the page you are standing on is kept |
| t | coin toss, ×1 range, RATE centre = **120 BPM**, BIAS centre, no jitter, DEJA VU locked |
| X | SPREAD centre, BIAS centre, STEPS 0.66 (so X is quantized and Y is not), DEJA VU locked, `[J]` **INTELLIGENT** (X and Y), identical diversity, **major** |
| Y | SPREAD centre, BIAS centre, STEPS at its bottom (smooth), 1/8 of t2 — `[J]` is shared with X |
| LENGTH | 8 steps |

⚠️ **t2 is the master clock**, driving the play-row clock and the Y divider whatever it is routed
to. Having TRIG on it means a note on every tick with no rhythmic randomness — t1 and t3 are the
outputs the six models make interesting. That is a deliberate choice of default (Adara, M4d): it
is the most predictable starting point, and t1 / t3 are then yours to route.

### LEDs

| | |
|---|---|
| **model row, BASE** | **Marbles' own output LEDs**: on the t page T1–T3 are the t1–t3 gates; on the X page they are \|X1\|…\|X3\| against the selected range. **T4 is always \|Y\|.** Moving a fader shows that page's four values instead, for 1.2 s. |
| **model row, SHIFT / SETTINGS** | that layer's four values; free faders read dark. SETTINGS breathes. |
| **play row** | **Marbles' clock** in front, stepping once per t2 tick from the `••` end, with the dB meter behind it at ~10 %. (On PLAITS it is the other way round.) |

⚠️ While the clock is stopped, the X and Y LEDs hold their last voltage rather than going dark.
Known, and deliberate for now: it shows you where the sequence was.

---

## Unassigned — the backlog

Everything the two source modules can do that this firmware does not expose, and every control
that has nothing on it. This is the supply side for M5 and M6.

### Free controls

| where | what |
|---|---|
| PLAITS | `••` + T1, `••` + T2, `••` + T3 |
| PLAITS | T4 on the SETTINGS panel |
| MARBLES | t SHIFT F1 |
| MARBLES | X SHIFT F1, F2, F3 |
| MARBLES | t SHIFT T4 |
| MARBLES | `••` + FFWD / RWD on the t page and on SETTINGS |
| MARBLES | *(none — `••` + T4 became Y's destination in M4e)* |

Spent since M4a: PLAITS T1 (engine flash), PLAITS SETTINGS T1–T3 (scale), `••` + VOL± (the
drive) and MARBLES SETTINGS T2/T3 (`[F]` / `[G]`).

### Plaits features not exposed

| | why not |
|---|---|
| **MODEL CV input** | Plaits can be swept through its models by a voltage. T2/T3 do it here; a Marbles output on a MODEL destination would be a real addition, and a genuinely good one. Nothing prevents it: add a destination and a `mods.engine` term. |
| **Both outputs at once** | Plaits renders OUT and AUX every block and we pick one (or a mix) for a single mono speaker. `••` + T4's four modes are the whole of what one output can be. A second physical output is not available. |
| **User data** | wavetables and DX7 patches loaded through the ear-jack. `plaits/user_data.h` is **shimmed away** because upstream's reads STM32 flash at an address that does not exist on this chip. |
| **FINE TUNE** | exposed until M4a, removed: it only ever acted in octave-range mode 9, and mode 9 is the mode that quantizes F1 to whole octaves. |
| **6-op FM ×3** | compiled but left out of the default `config/engines.csv` (slots 22–24 are empty); a community DX7 firmware covers it. Type a name into an empty slot to get one. |

### Marbles features not exposed

| | why not |
|---|---|
| **the secret Markov t model** | excluded on purpose; see `docs/MARBLES-SETTINGS.md` |
| **`register_mode`** (shift-register / "constant" mode) | wired to `false`. Needs a control; candidate for M6 |
| **the scale recorder** | Marbles can learn a scale from CV. There is no CV |
| **external clock, T RESET, and every CV input** | there are no jacks |
| **X clock source** | unbound in M4a; the field remains |
| **Y DEJA VU and Y LENGTH** | fixed at 0 and 1: Y is a divided copy of t2, not a looping sequence of its own |
| **per-channel gate length** | Marbles has one pair of gate-length controls for all three t outputs, and so do we |

### Agreed but not built

| | milestone |
|---|---|
| **Save state on shutdown** — the attenuverters, the routing, the engine, Marbles' setup | M5 |
| **OUT × AUX with +90° on OUT and +2 dB** | parked |
| **a sequencer** | M6 |
