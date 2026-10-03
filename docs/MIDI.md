# MIDI on Wakes

Plug the SP-1 into a computer, phone or a USB host like the OP-XY, and Wakes shows up as a
class-compliant **USB-MIDI** port called `wakes-sp1` — no driver. It listens; it does not send.

Nothing about it needs setting up on the device, and there are **no new pages**: notes play
Plaits straight away, and everything deeper is set in the MIDI script,
[`config/midi.ini`](../config/midi.ini), before the firmware is built.

## What MIDI does

| | |
|---|---|
| **Notes** | play Plaits. A note-on strikes **TRIG** and holds **LEVEL** open — as far as the note's **velocity** says, out of the box, so soft notes are quieter and darker (Plaits reads LEVEL as accent too); a note-off lets LEVEL close through Plaits' own low-pass gate, so the release is the LPG's. |
| **Several keys** | Plaits is one voice: you hear the **newest** key. Let it go and you hear the previous one if it is still held. (`note_priority` in the script can make it the lowest or the highest key held instead.) The same pitch played again before its note-off -- a sequencer's notes longer than its step -- strikes again with `legato = off` and is tied into one long note with `on` / `auto`; it ends at its **last** note-off. Even a note shorter than a quarter of a millisecond opens the gate. |
| **Pitch** | added to FREQUENCY like a V/Oct cable, with **note 60 (C4) adding nothing**. F1 on its centre detent is exactly C4, so a centred F1 plays the keyboard at its real pitch, and moving F1 transposes. While MIDI is in use the FREQUENCY detent is **10 %** of the fader's travel (5 % otherwise), so C4 is easy to land on. |
| **Pitch bend** | ±2 semitones, or whatever range the host sends (RPN 0, "pitch bend sensitivity"). |
| **Sustain pedal** | CC 64 holds released notes until it lifts. |
| **CCs** | each fader parameter on both modules, and Plaits' MODEL, has a CC (chart below). A CC is a **second hand on that fader**; how the two hands share the parameter is the script's `pickup` setting (below). Stepped parameters (OCTAVE range, LENGTH, Y divider, MODEL) move in whole steps; a MODEL change flashes the new engine's glyph -- the same flash as T2/T3 -- and MIDI changes the engine at most every 50 ms (each change costs the audio a moment, as a T2/T3 press does). MODEL offsets the engine T2/T3 select, so with MODEL away from 0 every engine glyph (T1, T2/T3, coming back from Marbles) shows the engine you **hear**, not the one selected. |

The faders, the shift layers, the attenuverters, Marbles and its routing all keep working while
MIDI plays (with one exception you choose: `pickup = takeover`, below).

### Pickup: how a CC and its fader share a parameter

The `pickup` line in the MIDI script picks one of three. All three use **Plaits' own pickup** --
the "catch-up" its knobs do after you load a preset: a control that does not match the value
never makes it jump. Moving it moves the value **the same way, from where it is**, a little
faster or slower so that the two meet at the end you are heading for, and from then on the value
simply follows the control.

- **`shared`** (the default): the CC and the fader are **two hands on one value**. A CC reads
  just like the fader -- 0 is the bottom, 127 the top, 64 the middle (inside the centre detent
  where there is one). Move either and it takes the value from wherever the other one left it,
  catching up with your hand. The fader's LEDs show the value, so you can see where the host put
  it.
- **`sum`**: the CC is an **offset** on top of the fader -- fader + CC, as a knob and a CV add
  on the module. How the CC reads depends on the parameter (below). Unplug and every offset
  glides back to zero.
- **`takeover`**: while MIDI is plugged in, the faders whose parameter has a CC **rest**, and only
  the CC moves it -- catching up with where the fader left it. Unplug and the faders work again,
  catching up with where the CC left it. (A parameter your script gives no CC keeps its fader.)

In `shared` and `takeover` the CC moves the parameter itself, so unplugging leaves it where the
CC put it, as letting go of a fader would. Either way, a host knob's **first** value after
plugging in -- including the snapshot some hosts send when they connect -- only tells Wakes where
the knob is; nothing moves until you turn it.

MODEL has no fader, so it is an offset from the engine T2/T3 selected in every setting, as are
the `[bind]` sources (velocity, aftertouch).

### How a CC reads in `sum`: centred or one-sided

Each CC reads the way its parameter works -- the same rule Marbles' INTELLIGENT voltage range
uses for its outputs:

- **centred** parameters, whose middle is "nothing happening" (FREQUENCY, the attenuverters,
  RATE, BIAS, DEJA VU, STEPS): **64 = no change**, 0 and 127 = half a fader's travel down and
  up -- so with the fader on its centre, the CC sweeps the whole range, end to end.
- **one-sided** parameters (LPG colour and decay, LEVEL, JITTER, gate length, SPREAD, and the
  stepped ones): **0 = no change**, 127 = a whole travel up -- so with the fader at 0, the CC
  sweeps the whole range. A controller knob resting at 0 leaves the fader in charge.
- **TIMBRE, MORPH, HARMONICS: per engine.** Centred on an engine where that fader has a centre
  detent, one-sided where it doesn't (`docs/PLAITS-ENGINES.md` has the detents). When an engine
  change flips how one of these reads, nothing jumps: the offset stays where it was and **catches
  up** with your knob as you turn it, Plaits' way (above).

The fader and the CC add, and the result stops at the parameter's ends -- as a knob and a CV do
on the module. So a CC that has pushed a parameter all the way to an end leaves the fader
nothing to move until the CC comes back.

MODEL reads one-sided in every setting: 0 = the engine T2/T3 selected.

### LEVEL, while a key is down — and only then

MIDI connects LEVEL from the first key until that key's release has finished, then hands it
back. A Marbles pattern that only uses TRIG keeps playing with MIDI plugged in, because an idle
MIDI port connects nothing.

### The scale quantizer stands aside

While MIDI is in use, FREQUENCY's scale quantizer is bypassed: the keyboard already chose its
notes. Your scale stays selected and comes back when MIDI is unplugged.

### Plugging in and unplugging

- **Plugged in**: the track row plays the Unpatch animation **in reverse** — a cable going in.
  It also plays once when you turn Wakes on with a host already attached.
- **Unplugged** (or the host goes to sleep): the Unpatch animation as usual, and MIDI goes
  back to neutral — keys released, bend to centre, and every offset (all of them in `sum`; MODEL
  and `[bind]` in the others) glides back to zero. In `shared` and `takeover` the values the CCs
  moved stay where they are, and in `takeover` the faders work again.

Rip out the cables and Unpatch leave MIDI alone: MIDI is not a cable on the panel, and the
host still thinks its knobs are where it left them.

A charger never enumerates, so it never prompts and never turns MIDI on.

### MIDI clock: Marbles follows the host

Send Wakes MIDI clock (on the OP-XY: turn **clock** on for Wakes in its device settings) and
Marbles locks to the host, the way a Eurorack Marbles does with a cable in its CLOCK input:

- From the first clock tick, **Marbles' clock is the host's**, until you unplug. The `CLOCK
  external` line in the log says when it took over.
- **RATE picks a ratio of the host's beat** instead of a tempo, from Marbles' own table: 1/4,
  1/3, 1/2, 2/3, **1** (the centre: one Marbles tick per beat), 3/2, 2, 3, 4. The t range setting
  multiplies that by 4 or ¼, as on the module.
- **Start** plays from the top: Marbles waits, and its first tick lands exactly on the host's
  beat 1. **Continue** carries on, **Stop** stops. **PLAY** still runs and stops Marbles on
  Wakes; whatever the host does next wins.
- **MMC** (MIDI Machine Control) Play and Stop work too — the OP-XY sends its play / stop this
  way. With MIDI clock arriving, Play waits for beat 1 like Start; with none (the OP-XY may send
  no clock), Play starts Marbles on its own RATE tempo, as PLAY does.
- If the host stops sending clock without a Stop, Marbles **waits** for the next tick.
- **Steady through a jittery clock.** DAWs make clock in chunks of their audio buffer, so ticks
  arrive a few ms early or late. Wakes fits a straight line through the last two beats of
  ticks and follows the line, not each tick: ±8 ms of tick jitter comes out as under 1 ms on
  Marbles' beats.
- **On time at the output.** Wakes plays everything about 25 ms after the MIDI that caused it
  (it places each message on time inside the next 5 ms audio block, and audio is queued ahead
  of the output). A clock is steady, so Marbles reads it that far **ahead** and its beats leave
  Wakes on the host's beat. Beat 1 after a Start is the exception — nothing said when it
  would come — so Marbles is on time from beat 2. `clock_lead` in the script sets the lead
  (`auto` = 25 ms); **leave your DAW's own clock offset for Wakes at 0**, or set `clock_lead = 0`
  and use the DAW's instead. Notes cannot be played early: give Wakes' track the DAW's usual
  hardware latency compensation for those.
- **Unplug** while the host is clocking Marbles: Marbles stops and goes back to its own RATE
  tempo.
- FFWD's burst follows the host's tempo too.

Clock alone does not count as "MIDI in use": it does not bypass the scale quantizer or widen
the FREQUENCY detent. `clock = off` in the script makes Wakes ignore clock and transport.

### Charging

While Wakes is **ON with a USB host attached** -- a computer, a phone, the OP-XY -- it does
**not charge**. It runs from USB power instead, so the battery neither charges nor drains. This
is for battery-powered hosts like the OP-XY, which would otherwise spend their own battery
charging the SP-1 for the whole session. Everything else charges as it always has: ON on a plain
charger, and STANDBY with anything -- plug in while off, or power off while plugged into a host,
and Wakes goes to STANDBY and charges.

## The CC chart (the shipped script)

With `pickup = sum`, a "reads" column in the generated chart says, for each CC, whether it is
centred, one-sided or per engine (above).

Every number here is a CC that keyboards and DAWs **do not send on their own**, so out of the box
nothing moves until you point something at it — map a knob on your controller, or a DAW
automation lane, to the number you want. CC 0–31 are 14-bit: Wakes also listens on CC N+32 for
the fine half, which is how hosts send high-resolution CCs. A host that only sends the coarse
half still works.

| CC | fine | parameter |
|---|---|---|
| 3 | 35 | TIMBRE |
| 9 | 41 | MORPH |
| 14 | 46 | HARMONICS |
| 15 | 47 | FREQUENCY |
| 20–23 | 52–55 | FM / TIMBRE / MORPH / HARMONICS attenuverters (PLAITS SHIFT) |
| 24 | 56 | LPG colour |
| 25 | 57 | LPG decay |
| 26 | 58 | LEVEL (PLAITS SETTINGS F4 — can connect it, like the fader) |
| 102 | | OCTAVE range |
| 105 | | MODEL: the engine, as an offset up through your filled slots in `config/engines.csv` |
| 27 | 59 | RATE |
| 16 | 48 | t BIAS |
| 85 | | JITTER |
| 31 | 63 | DEJA VU |
| 17 | 49 | gate length |
| 86 | | gate length randomness |
| 103 | | LENGTH |
| 28 | 60 | SPREAD |
| 29 | 61 | X BIAS |
| 30 | 62 | STEPS |
| 18 | 50 | Y SPREAD |
| 19 | 51 | Y BIAS |
| 87 | | Y STEPS |
| 104 | | Y divider |
| 64 | | sustain pedal |

Every build writes this chart for **its own** script next to the firmware —
`wakes-sp1-midi.md`, and `wakes-sp1-midi.csv` in the [midi.guide](https://midi.guide) column
layout — so a custom script always ships with a chart that matches it.

Left free on purpose, for your own script: CC 1 (mod wheel), 2 (breath), 4 (foot),
5 (portamento time) and 11 (expression).

## Changing it: the MIDI script

[`config/midi.ini`](../config/midi.ini) is commented line by line. In a fork, edit it — GitHub's
web editor is enough — and the CI build of your fork produces a `.bin` with your settings.
The build **refuses** a script with a mistake in it and names the line, so a typo cannot become a
firmware that quietly ignores you.

What it sets:

- `channel` — 1–16, or `omni`.
- `note_priority` — `last` (default), `low` or `high`.
- `legato` — `off` (every new note strikes; the default), `on` (a note played over a held one
  slides without striking), `auto` (as `on`, with portamento only between overlapping notes,
  so playing legato glides and playing detached jumps).
- `portamento` — `0`, `t1`–`t50` (constant time, an exponential glide), `r0`–`r50`
  (constant rate, a straight line, timed per octave). From about 1 ms up to about 2.6 s;
  the script lists the steps.
- `bend_range` — 0–24 semitones, until the host sends its own.
- `sustain` — `cc N`, or `off`.
- `cc_smoothing` — 0–200 ms: how long a CC glides to a new value. This is what turns a CC's
  steps into a smooth movement instead of zipper noise.
- `pickup` — `shared` (default), `sum` or `takeover` (above).
- `clock` — `on` (default): Marbles follows MIDI clock and transport (above); `off` ignores them.
- `clock_lead` — `auto` (25 ms, Wakes' own delay) or 0–200 ms: how far ahead of the host's
  clock Marbles runs (above).
- one line per parameter — `name = cc N`, or `none`.
- `[bind]` — `velocity` and `aftertouch` (channel pressure) can push a parameter by a depth
  (`velocity = timbre 40%`). Aimed at `level`, they set how far each note opens LEVEL —
  which Plaits also reads as accent, so soft notes are quieter and darker. **The shipped
  script has `velocity = level`** (100 %: the softest note barely opens it; at 50 % the
  softest still opens it half way). Comment that line out to play every note at full LEVEL.
  You can also do this on the host instead: most DAWs can turn velocity or pressure into a CC
  and aim it at one of the CCs above.

## Not (yet) here

- **USB audio** out (M5c).
- **Sending clock or transport** back to the host (PLAY driving the host), and **Song Position
  Pointer** (a Continue carries on from where Marbles is, not from the host's bar).
- **Sending MIDI** back to the host, and **MIDI 2.0**: Wakes is a MIDI 1.0 device, which every
  MIDI 2.0 host also speaks.

The MIDI note handling is ported from Mutable Instruments **Yarns** (Émilie Gillet, MIT), and the
USB-MIDI class is **feldd**'s (Benjamin Reece, MIT) — see [`NOTICE`](../NOTICE).
