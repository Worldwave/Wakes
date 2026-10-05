# Stock driver or fast path?

Both send the same audio. They differ in what it costs your firmware to send it,
and what you take on to get that cost down. **Start with stock.** Switch only if
you have measured that you need the CPU back.

## The short answer

| Your firmware... | Use |
|---|---|
| has CPU to spare while streaming | **stock** |
| sends no USB audio (MIDI, serial, HID only) | **stock** — the fast path does nothing for these |
| also receives audio from the host, or has a feedback endpoint | **stock**, or expect a much smaller gain |
| produces its audio in a thread and needs locks or heavy work to hand over a packet | **stock** |
| is short of CPU while streaming, and can hand over each packet in a few µs without locks | **fast path** |

## What each one does, every millisecond

**Stock.** The USB interrupt posts an event; Zephyr's `usbd` thread wakes and
asks your code for a packet; the driver's own thread wakes to start the transfer,
wakes again when it finishes, and hands the buffer back to `usbd`, which wakes a
second time to release it. Four thread wake-ups per packet, a thousand packets a
second.

**Fast path.** The USB interrupt calls your fill function, which copies the next
packet into a driver buffer, and the same interrupt starts the transfer. No
threads wake at all.

## Measured on an SP-1

64 MHz Cortex-M4, 48 kHz 16-bit stereo to macOS, the same synth firmware both ways:

| | Stock | Fast path |
|---|---|---|
| USB threads' share of the CPU, four voices playing | ~18% (`usbd` 11%, driver thread 7%) | ~0 |
| Time to render one audio block, four voices | ~3486 µs | 2923 µs — the same as with no stream open |
| Six voices plus the stream, repeated chords | the unit reset | 4139–4456 µs of a 5333 µs budget, no reset |
| Dropped or late packets | none, until overloaded | none |
| Latency | a packet leaves on the next USB frame | the same |

In that firmware the difference was five voices or six. That is the whole case
for the fast path: **it buys back CPU, nothing else.**

## Stock: positives and negatives

**Positives**
- **Nothing to maintain.** It is Zephyr's driver; every Zephyr and nRF Connect
  SDK release brings its fixes, and it is tested by far more people than this repo.
- **Portable.** The same code works on any chip Zephyr's UAC2 class supports.
- **Forgiving.** Your packet code runs in a thread. It can take a lock, wait
  briefly, or do real work, and a mistake shows up as an ordinary thread bug you
  can debug.
- **Plays well with others.** It does not care whether other classes need
  start-of-frame events or whether you also receive audio.

**Negatives**
- **CPU.** ~18% of a 64 MHz nRF52840 while the stream is open, whether or not
  anything is playing.
- **Scheduling pressure.** Those wake-ups land in the middle of your audio work.
  Zephyr's USB threads outrank a normal audio thread, so a render that was
  already close to its deadline can be pushed past it.

## Fast path: positives and negatives

**Positives**
- **The CPU comes back.** The USB audio cost goes to roughly zero.
- **Robust under load.** The packet is written by the interrupt itself, so a busy
  audio thread cannot make it late.
- **Opt-in and contained.** About 100 added lines, all behind the callback being
  registered. Built with no callback, the driver behaves exactly as stock.

**Negatives**
- **You carry a copy of a Zephyr driver.** It is one file forked at one version
  (nRF Connect SDK v3.3.0's Zephyr). Fixes Nordic makes after that do not reach
  you until someone rebases the patch by hand.
- **It is tied to that version.** Upstream Zephyr v4.3.1's copy of the file
  differs by ~330 lines, including the timing logic the fast path relies on. The
  patch *applies* there, which is misleading: on another Zephyr it needs review
  and testing, not just applying.
- **Your fill function runs in an interrupt,** a thousand times a second. It must
  be short, never block, and take no locks — copy from a ring buffer and return.
  Anything slower delays every other interrupt too: I2S, timers, the radio.
- **Bugs are harder to see.** A mistake in interrupt-level code does not show up
  as a thread you can inspect.
- **Most of the gain assumes nothing else needs start-of-frame events.** The
  biggest saving is skipping the `usbd` wake-up every frame. If you also receive
  audio from the host, or use a feedback endpoint, that wake-up has to stay.
- **Send direction only, nRF52840 only.** It does nothing for audio coming *into*
  the device, and the API is specific to Nordic's nRF52840 USB driver.
- **Less tested.** Proven on one SP-1 with macOS. Not yet tried on Windows or
  Linux, or across a host suspend with the stream open.

## How to decide on your own firmware

1. Build with stock and open the stream.
2. Turn on Zephyr's per-thread CPU accounting (`CONFIG_THREAD_RUNTIME_STATS=y`,
   `CONFIG_SCHED_THREAD_USAGE_ALL=y`, and on nRF52
   `CONFIG_THREAD_RUNTIME_STATS_USE_TIMING_FUNCTIONS=y`, since the default 32 kHz
   clock is too coarse for USB work).
3. Play your heaviest material and read the `usbd` and `udc_nrfx` threads' share.
4. If your audio still keeps up, you are done. If it does not, and those two
   threads are what tips it over, try the fast path. It is one config switch in
   either direction.

## If the fast path reaches Zephyr

The plan is to offer the fast path to Zephyr as an opt-in setting. If it is
accepted, most of its negatives go away: you would build stock Zephyr, each
release would carry a version written for its own driver, and Nordic's fixes and
Zephyr's testing would cover it. The interrupt-context rules would still apply,
and whether you need it would still depend on your CPU budget.
