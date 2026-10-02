# MIDI on Wakes

Plug the SP-1 into a computer, phone or a USB host like the OP-XY, and Wakes shows up as a
class-compliant **USB-MIDI** port called `wakes-sp1` — no driver. It listens; it does not send.

Nothing about it needs setting up on the device, and there are **no new pages**: notes play
Plaits straight away, and everything deeper is set in the MIDI script,
[`config/midi.ini`](../config/midi.ini), before the firmware is built.

## What MIDI does

| | |
|---|---|
| **Notes** | play Plaits. A note-on strikes **TRIG** and holds **LEVEL** open; a note-off lets LEVEL close through Plaits' own low-pass gate, so the release is the LPG's. |
| **Several keys** | Plaits is one voice: you hear the **newest** key. Let it go and you hear the previous one if it is still held (Yarns-style note priority). |
| **Pitch** | added to FREQUENCY like a V/Oct cable, with **note 60 (C4) adding nothing**. F1 on its centre detent is exactly C4, so a centred F1 plays the keyboard at its real pitch, and moving F1 transposes. While MIDI is in use the FREQUENCY detent is **10 %** of the fader's travel (5 % otherwise), so C4 is easy to land on. |
| **Pitch bend** | ±2 semitones, or whatever range the host sends (RPN 0, "pitch bend sensitivity"). |
| **Sustain pedal** | CC 64 holds released notes until it lifts. |
| **CCs** | each fader parameter on both modules, and Plaits' MODEL, has a CC (chart below). A CC is a **second hand on that fader**: it adds to where the fader is, as an offset, and the fader keeps working. How the CC reads depends on the parameter (below). Stepped parameters (OCTAVE range, LENGTH, Y divider, MODEL) move in whole steps; a MODEL change flashes the new engine's glyph -- the same flash as T2/T3 -- and MIDI changes the engine at most every 50 ms (each change costs the audio a moment, as a T2/T3 press does). MODEL offsets the engine T2/T3 select, so with MODEL away from 0 every engine glyph (T1, T2/T3, coming back from Marbles) shows the engine you **hear**, not the one selected. |

The faders, the shift layers, the attenuverters, Marbles and its routing all keep working while
MIDI plays. MIDI adds to them; it does not take anything over.

### How a CC reads: centred or one-sided

Each CC reads the way its parameter works -- the same rule Marbles' INTELLIGENT voltage range
uses for its outputs:

- **centred** parameters, whose middle is "nothing happening" (FREQUENCY, the attenuverters,
  RATE, BIAS, DEJA VU, STEPS): **64 = no change**, 0 and 127 = half a fader's travel down and
  up -- so with the fader on its centre, the CC sweeps the whole range, end to end.
- **one-sided** parameters (LPG colour and decay, LEVEL, JITTER, gate length, SPREAD, and the
  stepped ones): **0 = no change**, 127 = a whole travel up -- so with the fader at 0, the CC
  sweeps the whole range. A controller knob resting at 0 leaves the fader in charge.
- **TIMBRE, MORPH, HARMONICS: per engine.** Centred on an engine where that fader has a centre
  detent, one-sided where it doesn't (`docs/PLAITS-ENGINES.md` has the detents). Change engine
  and the CC is re-read the new way at once.

The fader and the CC add, and the result stops at the parameter's ends -- as a knob and a CV do
on the module. So a CC that has pushed a parameter all the way to an end leaves the fader
nothing to move until the CC comes back.

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
- **Unplugged** (or the host goes to sleep): the Unpatch animation as usual, and everything
  MIDI was doing goes back to neutral — keys released, every CC offset glides back to zero,
  bend to centre. The device is exactly as playable as before you plugged in.

Rip out the cables and Unpatch leave MIDI alone: MIDI is not a cable on the panel, and the
host still thinks its knobs are where it left them.

A charger never enumerates, so it never prompts and never turns MIDI on.

### Charging

While Wakes is **ON with a USB host attached** -- a computer, a phone, the OP-XY -- it does
**not charge**. It runs from USB power instead, so the battery neither charges nor drains. This
is for battery-powered hosts like the OP-XY, which would otherwise spend their own battery
charging the SP-1 for the whole session. Everything else charges as it always has: ON on a plain
charger, and STANDBY with anything -- plug in while off, or power off while plugged into a host,
and Wakes goes to STANDBY and charges.

## The CC chart (the shipped script)

"reads" in the generated chart says, for each CC, whether it is centred, one-sided or per
engine (above).

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
  slides without striking), `auto` (as `on`, with portamento only between overlapping notes).
  These are Yarns' three modes.
- `portamento` — `0`, `t1`–`t50` (constant time), `r0`–`r50` (constant rate). Yarns' scale:
  instant up to about 6 seconds.
- `bend_range` — 0–24 semitones, until the host sends its own.
- `sustain` — `cc N`, or `off`.
- `cc_smoothing` — 0–200 ms: how long a CC glides to a new value. This is what turns a CC's
  steps into a smooth movement instead of zipper noise.
- one line per parameter — `name = cc N`, or `none`.
- `[bind]` — `velocity` and `aftertouch` (channel pressure) can push a parameter by a depth
  (`velocity = timbre 40%`). Aimed at `level`, they set how far each note opens LEVEL —
  which Plaits also reads as accent, so `velocity = level` makes soft notes quieter and darker.
  You can also do this on the host instead: most DAWs can turn velocity or pressure into a CC
  and aim it at one of the CCs above.

## Not (yet) here

- **MIDI clock and transport** — following the host's tempo and start/stop with Marbles is the
  next step (M5b).
- **USB audio** out — after that (M5c).
- **Sending MIDI** back to the host, and **MIDI 2.0**: Wakes is a MIDI 1.0 device, which every
  MIDI 2.0 host also speaks.

The MIDI note handling is ported from Mutable Instruments **Yarns** (Émilie Gillet, MIT), and the
USB-MIDI class is **feldd**'s (Benjamin Reece, MIT) — see [`NOTICE`](../NOTICE).
