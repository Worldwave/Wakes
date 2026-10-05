# Lessons from the bench

## The chord-start click was the packet sizer, not the driver

- Symptom: a click in the recording as chords started.
- Cause: the regulator read a render-timing step (a heavy chord makes each block
  finish later) as clock drift, and sent ~150 ms of 47-frame packets. To the host
  the clock briefly ran 2% slow, and macOS's correction was audible.
- Fix: ~1 s smoothing, a dead band, at most one correction per 50 packets, and
  steering on the claimed position instead of the push.
- Lesson: the regulator must only ever see clock drift. Anything that moves
  the fill for another reason has to be kept out of what it measures.

## A pulled cable does not stop the stream

- The UAC2 class only hears alternate setting 0. With the cable out the stream
  stayed "on", every block counted as an overflow, and the unit stayed on battery.
- Fix: stop on VBUS removed and on bus reset. Confirmed on hardware: the stream
  stops ~27 ms after the cable, no reset, restarts clean.

## Resume needs a re-prime

- Nothing drains while the host sleeps; a stream resumed on a full ring runs ~40
  ms late and drifts back for minutes.
- Status: the re-prime is written but **not exercised** — macOS closes the stream
  before it sleeps, so this path never ran on the bench.

## Latency comes from the audio path, not USB

- The USB ring adds ~8.7 ms (416 frames). The big win was elsewhere: a 2-block
  I2S queue and 128-sample blocks.
- The fast path does not reduce latency: the packet goes out on the next frame
  either way.

## Level

- USB audio goes out at its own fixed level (-6 dB by default), separate from
  the master volume. The volume buttons reach only the speaker and headphones.
- Why: a recording's level should not depend on where the volume happened to be
  set.
- -6 dB is close to the edge, not safe: the heaviest chord tested there peaked at
  33398 of 32767 and clipped one sample. The level trims from 0 to -18 dB over
  SysEx, so a firmware for louder material can sit lower.

## Not tried

- Windows. An output-only stream has no feedback endpoint, so it avoids the one
  Windows quirk Zephyr documents for asynchronous streams (its UAC2 driver
  expects a 4-byte explicit-feedback endpoint, against the USB spec; see Zephyr's
  `samples/subsys/usb/uac2_explicit_feedback`). Untested all the same.
- Linux.
- Host audio INTO the SP-1 (playback). With the only isochronous IN endpoint
  used by the capture stream, it would need implicit feedback, which Zephyr's
  `uac2_implicit_feedback` sample notes Windows' UAC2 driver does not support.
