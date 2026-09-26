# Default states — every parameter, and what "rip out the cables" resets

**What this is for.** `••` + PLAY held 3 s ("rip out the cables", ROTC) resets *the module on
show*. Right now it resets what M3c/M4 happened to put in reach; this document lists **every
parameter of both modules** so the reset target can be decided deliberately, parameter by
parameter, rather than inherited.

**Status: decided (Adara) and IMPLEMENTED in M4d.** The ROTC column describes the firmware again.
The four **REVIEW** notes left in place are the consequences of the decisions, all confirmed by
Adara.

**Three scopes**, because "default" means three different things on this device:

| scope | meaning |
|---|---|
| **boot** | what a cold power-on gives you. All RAM state; nothing persists yet (M5 will change that) |
| **ON** | re-applied on every entry to ON, even from standby — the transport-ish things that must not survive a power cycle |
| **ROTC** | what `••` + PLAY held 3 s restores, on the module on show |
| **kept** | value unchanged by ROTC |

Accurate as of **M4e**: every row here is implemented. (Through M4b this file was the spec rather
than a description; it is now both.)
⚠️ ROTC is no longer "a modulation reset, not a patch wipe" — with the engine and BASE in scope it
**is** a patch wipe, and that wording was corrected in `sp1_plaits_ui.h`, `sp1_marbles_ui.h`,
`docs/UI-SPEC.md` and `docs/UI-PAGES.md`.

---

## PLAITS

### BASE faders — the patch

| | parameter | range | boot | ROTC | notes |
|---|---|---|---|---|---|
| F1 | FREQUENCY | per OCTAVE range | **the fader's position** | fader set to 0.5, or C4, frequency unlocked (see SETTINGS F1 on ROTC) | Seeded from the physical fader on the first ON after boot only |
| F2 | TIMBRE | 0…1 | the fader's position | slot 1's neutral position (contextual to the engine) | meaning is per engine |
| F3 | MORPH | 0…1 | the fader's position | slot 1's neutral position (contextual to the engine) | meaning is per engine |
| F4 | HARMONICS | 0…1 | the fader's position | slot 1's neutral position (contextual to the engine) | meaning is per engine |

**ROTC is a full patch reset, not a modulation reset.** It returns the engine to slot 1 and the
four BASE faders to that engine's neutral state: **0.5 for a bipolar parameter, 0 for a unipolar
one**, and F1 to the middle of its travel with the range open and the quantizer off.

**Yes, F1's middle is exactly C4.** In range mode 10 the note is `60 + t × 48` where
`t = 2 × detent(F1) − 1`, so F1 at 0.5 gives t = 0 and MIDI **60 = C4** — and the 5 % centre
detent means it is a real landing spot rather than a coincidence of fader position.

**Which parameters are bipolar is already known per engine**, so nothing new has to be decided:
each engine's detents (listed in `docs/PLAITS-ENGINES.md`) mark exactly the parameters whose
centre is a neutral point, and they are compiled into `SP1_ENGINE_TABLE[].centre`. On virtual
analog, slot 1 by default, that gives F2 → 0.5, F3 → 0, F4 → 0.5.

⚠️ **REVIEW 1 — the faders will not match after a ROTC, by design.** ROTC sets the stored values;
the physical faders stay where your hands left them. Pickup then catches each one up on its next
movement, which is the same behaviour ROTC has always had for the SHIFT and SETTINGS layers. So
after a ROTC the instrument sounds neutral while the faders still look wrong. That is correct and
is the only way to do it without motorised faders. Confirmed as intended (Adara).

⚠️ **REVIEW 2 — neutral is per engine, and ROTC is a moment in time.** Because ROTC also returns
you to slot 1, "the engine's neutral state" is unambiguous at the moment it fires. But if you then
step to an engine with a different bipolar set, the values stay where ROTC put them and are no
longer that engine's neutral. The alternative — re-neutralising on every engine change — would
mean losing your patch just by browsing engines, which is clearly worse. Leaving it as a
moment-in-time reset (Adara, confirmed).

### SHIFT faders — Plaits' attenuverters

| | parameter | range | boot | ROTC |
|---|---|---|---|---|
| F1 | FM attenuverter | −1…+1 | 0 | 0 |
| F2 | TIMBRE attenuverter | −1…+1 | 0 | 0 |
| F3 | MORPH attenuverter | −1…+1 | 0 | 0 |
| F4 | HARMONICS attenuverter | −1…+1 | 0 | 0 |

Never seeded from the faders, at boot or ever. Gain 0 means nothing modulates until asked.

### SETTINGS faders

| | parameter | range | boot | ROTC | notes |
|---|---|---|---|---|---|
| F1 | OCTAVE / FREQUENCY range | 11 modes | 10 = full range (Plaits default) | 10 | forced to 10 when something is routed to V/Oct |
| F2 | LPG colour | 0…1 | 0 | 0 | |
| F3 | LPG decay | 0…1 | 0.5 (Plaits default) | 0.5 | |
| F4 | LEVEL | off, or 0…1 | unpatched (< 5 %) | unpatched (set fader to 0) | off ≠ 0: off means TRIG plucks, on means the VCA is held open |

### Discrete settings

| parameter | range | boot | ON | ROTC | notes |
|---|---|---|---|---|---|
| engine | 24 slots, 21 filled by default | slot 1 | — | slot 1 | the first filled slot of `config/engines.csv` |
| FREQUENCY quantization | off + 7 scales | off | — | off | forced off when something is routed to V/Oct |
| output select | OUT / AUX / OUT+AUX / OUT×AUX | OUT | OUT | OUT | RAM only |
| soft-clip drive | off, +3 / +8 / +15 / +24 dB | off | off | off | forced off on every entry to ON (this will change when ON is set to reload savestates in M5). Uneven steps since M4e; `••`+VOL− steps DOWN rather than toggling |
| burst division | 1/1 … 1/128 | 1/32 | 1/32 | 1/32 | RAM only. The ratchet is gone (M4e): FFWD is a phase-locked burst running or stopped |
| output level (VOL) | 0 … −30 dB, mute | −12 dBFS | −12 dBFS | kept | re-applied on every ON |
| module on show | PLAITS / MARBLES | PLAITS | kept | kept | Plaits is the face module, showing every ON. ROTC should not affect our current location. If we ROTC from Marbles, stay on Marbles. ROTC on Plaits, stay on Plaits. |

**Decided: ROTC resets the engine, output select, the drive and the division; VOL and the module
on show are kept.** That is a coherent split — everything that shapes the sound resets, while the
two controls that are about *monitoring and where you are standing* do not.

⚠️ **REVIEW 3 — the drive, the output mode and the division are reachable from both modules, but
ROTC only resets the module on show.** The drive is `••` + VOL, which works on the MARBLES page
too; output select and the division are PLAITS-page controls. Since ROTC only ever touches the
foreground module (your decision, and the right one), a ROTC on MARBLES will leave a drive
engaged.

**Resolved, twice.** M4d cleared the drive from **either** module. Adara corrected that in M4e:
*"Drive does not apply to Marbles, it is not making audio, only control signals. We only apply
overdrive to Plaits."* So the drive is a PLAITS parameter that merely happens to be reachable
from both pages, and **only a PLAITS ROTC clears it** — as with output select and the division.
A ROTC on MARBLES now leaves the drive alone, deliberately.

### Not reachable at all

| parameter | why |
|---|---|
| FINE TUNE | removed in M4a; it only acted in range mode 9 |
| per-engine "hidden" CV routing | Plaits' own; not applicable without jacks |
| user data (wavetables, DX7 patches) | shimmed away — upstream reads STM32 flash |

- Adara: We'll add custom wavetables in file editing in M5.

---

## MARBLES

### t section

| | parameter | range | boot | ROTC | notes |
|---|---|---|---|---|---|
| F1 | RATE `[A]` | ±5 octaves | centre = 120 BPM | centre | the one BASE fader ROTC does reset |
| F2 | t BIAS `[D]` | 0…1, detent | centre | centre | |
| F3 | JITTER `[C]` | 0…1 | 0 | 0 | |
| F4 | DEJA VU `[H]` | 0…1, detent | centre = locked loop | centre | one value, shared with the X page |
| t SHIFT F1 | (free) | — | — | — | |
| t SHIFT F2 | gate length | 0…1 | 0.5 | 0.5 | |
| t SHIFT F3 | gate-length randomness | 0…1 | 0 | 0 | |
| t SHIFT F4 | LENGTH `[I]` | 1…16 steps | 8 | 8 | one value, shared with the X page |
| T1 | t model `[E]` | 1-6 | 1 (coin toss) | 1 | |
| RWD/FFWD | t range `[B]` | ×0.25 / ×1 / ×4 | ×1 | ×1 | |

### X section

| | parameter | range | boot | ROTC | notes |
|---|---|---|---|---|---|
| F1 | SPREAD `[K]` | 0…1 | centre | centre |  |
| F2 | X BIAS `[L]` | 0…1, detent | centre | centre | |
| F3 | STEPS `[M]` | 0…1, detent | 0.66 (quantized, every degree) | 0.66 | |
| F4 | DEJA VU `[H]` | — | — | — | the same control as the t page's F4 |
| X SHIFT F1–F3 | *(free)* | — | — | — | were SCALE and X CLOCK until M4a |
| X SHIFT F4 | LENGTH `[I]` | — | — | — | the same control as t SHIFT F4 |
| T1 | diversity `[N]` | identical / bump / tilt | **identical** | **identical** | |
| SETTINGS T4 | range `[J]`, **X and Y** | 0–2 V / 0–5 V / ±5 V / INTELLIGENT | **INTELLIGENT** | **INTELLIGENT** | M4c; ONE control for both groups since M4e. Per CHANNEL, so X1–X3 and Y can each be at a different range at once |
| `••`+FFWD/RWD | scale | 7 slots, 6 included | **major** | **major** | |

### Y section (SETTINGS page)

| | parameter | range | boot | ROTC |
|---|---|---|---|---|
| F1 | Y SPREAD | 0…1 | **centre** | **centre** |
| F2 | Y BIAS | 0…1, detent | **centre** | **centre** |
| F3 | Y STEPS | 0…1, detent | **0** (smooth, no quantizing) | **0** |
| F4 | Y divider | 1/64 … 1/1 of t2 | **1/8** (Marbles' own) | **1/8** |
| T2 | DEJA VU on t `[F]` | on / off | **on** | **on** |
| T3 | DEJA VU on X `[G]` | on / off | **on** | **on** |
| T1 | *(free)* | — | — | — |

### Routing

| parameter | boot | ROTC |
|---|---|---|
| t1 destination | none | none |
| t2 destination | TRIG | none |
| t3 destination | none | none |
| X1 destination | V/Oct | none |
| X2, X3, Y destination | none | none |

### Transport

| parameter | boot | ON | ROTC |
|---|---|---|---|
| clock running | stopped | stopped | kept — a running clock keeps running through a ROTC |
| random seed | seeded from the cycle counter at the first PLAY after boot | kept | re-seed |
| DEJA VU loop contents | filled at init | kept | kept |

**Decided: ROTC leaves the clock running and re-seeds.** Keeping the clock running is the right
call — a reset you can perform mid-performance without stopping is much more useful than one that
drops the groove.

⚠️ **REVIEW 4 — re-seeding alone does nothing while DEJA VU is locked, which is the default.**
The seed feeds the random stream; the DEJA VU *loop buffer* is filled from that stream when the
generators are initialised. At the default locked DEJA VU no new values are ever drawn, so a fresh
seed would never be heard — the same loop would keep playing. To actually get a new loop, ROTC has
to re-seed **and** re-initialise the generators, which is exactly what the first PLAY after boot
already does. **Reading "re-seed DEJA-VU" as "give me a new loop" and doing both**, so the rows
above should say *re-seed* and *re-drawn*. Tell me if you meant the narrower thing.

Second half of the same note: re-initialising the generators while the clock is **running** — and
ROTC does not stop it — will jump the clock's phase once, so the beat after a ROTC lands early or
late by up to one tick. The alternative is to defer the re-seed to the next t2 tick, which costs a
little complexity and makes ROTC's effect arrive up to a beat later. Phase jump taken (Adara).
⚠️ The re-seed itself is deferred into the audio thread rather than done from the button handler,
so the phase jump is the *only* effect — re-initialising the generators from main while the audio
thread is inside them would be a data race, not just a jump.

### Not reachable at all

| parameter | why |
|---|---|
| the secret Markov t model | excluded on purpose |
| `register_mode` (shift-register / "constant") | wired false; needs a control. Candidate for M6 |
| the scale recorder | needs CV |
| external clock, T RESET, every CV input | there are no jacks |
| X clock source | unbound in M4a; X keeps X1→t1, X2→t2, X3→t3 |
| Y DEJA VU length, Y loop length | fixed at 1: Y is a divided copy of t2, not a sequence of its own |
| per-channel gate length | Marbles has one pair for all three t outputs |

---

## Global / device

| parameter | boot | notes |
|---|---|---|
| power state | ON if `••` woke it, STANDBY if the cable did | on battery, a reset without `••` goes straight back to sleep |
| speaker trim | **−6 dB**, fixed (M4a) | not a runtime parameter |
| headphone auto-mute | on, fail-safe **speaker ON** | 3 I²C failures disable detection with the speaker on |
| audio-load sag correction `K` | **0** | awaiting a measurement on hardware |

---

## Summary: all twelve decided

Adara's answers, 2026-09-24. **This is the build list.**

| # | question | decision |
|---|---|---|
| 1 | PLAITS ROTC — BASE? | **engine → slot 1, and the four faders → that engine's neutral** (0.5 bipolar, 0 unipolar); F1 → centre = C4, range open, quantizer off |
| 2 | output select? | **reset to OUT** |
| 3 | soft-clip drive? | **reset to off — by a PLAITS ROTC only (M4e)** |
| 4 | burst / ratchet division? | **reset to 1/32** |
| 5 | VOL? | **kept** |
| 6–8 | X SPREAD / BIAS / STEPS? | **reset** to centre / centre / 0.66 |
| 9 | stop the clock? | **no, it keeps running** |
| 10 | re-seed? | **yes** (see REVIEW 4 — doing a full re-init so a new loop is actually heard) |
| 11 | re-draw the DEJA VU loop? | **yes**, as the consequence of 10 |
| 12 | reset both modules? | **no — only the foreground module** |

Routing on ROTC is the headline change: **every destination goes to `none`.** "Rip out the
cables" now means what it says.

⚠️ **Two consequences of the routing decision, both intended as far as I can tell, both worth a
sentence before they surprise anyone on hardware:**

- **After a ROTC on MARBLES, pressing PLAY makes no sound.** Nothing reaches Plaits until a
  destination is dialled back in. That is the point of a blank slate, but it is a change from M4b,
  where ROTC left t1 + t3 on TRIG and X1 on V/Oct.
- **At boot, TRIG comes from t2 — the master clock — so every tick fires a note.** That is the
  most predictable "press PLAY and hear something" default there is, but it has **no rhythmic
  randomness**: t2 fires on every beat in every model. t1 and t3 are the ones the models make
  interesting. If the intent was "press PLAY and hear a musical random rhythm", t1 (or t1 + t3) is
  that default; if it was "press PLAY and hear a steady stream of notes I can then re-route", t2
  is exactly right. Built as written.

---

## The original questions, for the record

1. PLAITS ROTC — re-seed BASE from the faders, or keep the patch? - completed above
2. PLAITS ROTC — reset **output select** to OUT? - completed above
3. PLAITS ROTC — reset the **soft-clip drive** to off? - completed above
4. PLAITS ROTC — reset the **burst / ratchet division** to 1/32? - completed above
5. PLAITS ROTC — reset **VOL** to −12 dBFS? - completed above
6. MARBLES ROTC — reset **X SPREAD**? - completed above
7. MARBLES ROTC — reset **X BIAS**? - completed above
8. MARBLES ROTC — reset **X STEPS**? - completed above 
9. MARBLES ROTC — **stop the clock**? - completed above
10. MARBLES ROTC — **re-seed the random stream** (a new loop, not the same one)? - completed above
11. MARBLES ROTC — re-draw the **DEJA VU loop contents**? - completed above
12. Device — should ROTC be able to reset **both** modules at once (it currently only touches
    the one on show), e.g. with a longer hold or from either page? - Adara: No, ROTC should only reset the current, foreground module.
