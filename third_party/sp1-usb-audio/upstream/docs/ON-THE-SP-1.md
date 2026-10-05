# Adding USB audio out to an SP-1 firmware

This page walks through what USB audio needs on an SP-1, pointing at where each
piece lives in [`../sp1/`](../sp1/README.md), the complete example firmware. If
you are adding USB audio to a firmware you already have, these are the parts to
copy. If you are starting from nothing, start from `sp1/` itself: it already has
the watchdog, power-off and LEDs an SP-1 firmware needs. Everything below was
worked out in an SP-1 firmware that ships USB audio in its default build.

## 1. The clock: the codec is the master, not the nRF

On the SP-1 a 3.072 MHz oscillator (enabled on P0.13) drives the I2S bit clock.
The CS42L42 headphone codec divides it by 64 to make an exact 48 kHz frame clock,
and **the nRF52840's I2S and the TAS2505 speaker amp are both slaves.** The board
definition most SP-1 projects build against has the nRF as I2S master, and that
crackles on the speaker and gives noise on the headphones.

So the I2S bit and frame clock pins become **inputs** (the `_S` pin selections):

```dts
&i2s0_default {
	group1 {
		psels = <NRF_PSEL(I2S_SCK_S, 0, 12)>,
			<NRF_PSEL(I2S_LRCK_S, 0, 11)>,
			<NRF_PSEL(I2S_SDOUT, 1, 9)>;
	};
};
```

and the I2S is configured as a slave on both clocks:

```c
.options = I2S_OPT_FRAME_CLK_SLAVE | I2S_OPT_BIT_CLK_SLAVE,
.word_size = 16, .channels = 2, .frame_clk_freq = 48000,
```

This is also why USB audio needs the packet regulator at all: the audio runs on
the SP-1's oscillator, USB frames run on the computer's clock, and nothing ties
the two together.

Bring-up order, as proven on hardware: release the codec (P0.15) and amp (P0.9)
resets, wait 20 ms, enable the oscillator, wait 5 ms, then start I2S and the
TAS2505 before initialising the CS42L42. The CS42L42 register sequence is Tim
Knapen's ([SP-1-dev wiki, I2C](https://github.com/timknapen/SP-1-dev/wiki/I2C));
the clocking and bring-up order come from
[chattock/sp1-tape-looper](https://github.com/chattock/sp1-tape-looper) (MIT).

## 2. The USB device node

The common `sp1` board definition has no USB device node. Add one in your app's
overlay; the UAC2 function then goes alongside it unchanged. Both are in
[`sp1/fw/app.overlay`](../sp1/fw/app.overlay):

```dts
zephyr_udc0: &usbd {
	compatible = "nordic,nrf-usbd";
	status = "okay";
};
```

## 3. Where the two calls go

Copy the four files in [`usb-audio/`](../usb-audio/README.md) into your app, add
the USB lines from [`sp1/fw/prj.conf`](../sp1/fw/prj.conf), and — as
[`sp1/fw/src/main.c`](../sp1/fw/src/main.c) and
[`sp1/fw/src/audio.c`](../sp1/fw/src/audio.c) do:

- at startup, call `usb_audio_init()` **before** `usbd_enable()`, and pass
  `usb_audio_usbd_msg` to `sample_usbd_init_device()` (or call it from your own
  USB message callback);
- in the audio thread, each time it takes a block for I2S, **before rendering**:
  `usb_audio_claim(frames)`;
- after rendering that block: `usb_audio_push(lr, frames)` with interleaved
  16-bit left/right frames.

The claim is what keeps a heavy render from being mistaken for clock drift
([`LESSONS.md`](LESSONS.md)). The regulator's settings are tuned for 128- or
256-frame blocks (`AUDIO_BLK_FRAMES`, default 128).

## 4. The audio thread's priority

Keep the audio thread **preemptible**, at `K_PRIO_PREEMPT(0)`. Zephyr's USB
threads are cooperative at priority 8 and outrank it, so USB work can land in the
middle of a render; the I2S queue absorbs that. Making the audio thread
cooperative stopped the interruptions, but under near-full load it then starved
the main thread and the USB threads until the watchdog reset the unit. A
preemptible audio thread fails as one late block instead.

## 5. Level

Push the USB copy at its own fixed level, not through the master volume, so a
recording never follows the volume buttons. The SP-1 firmware this comes from
renders once and writes two outputs: the speaker/headphone mix at the master
volume, and the USB stream at -6 dB by default, adjustable over SysEx.

## 6. Alongside USB-MIDI and a serial console

UAC2 fits beside USB-MIDI and a CDC serial console on the nRF52840: together
they use 3 of 7 IN endpoints, 2 of 7 OUT, and the one isochronous IN. The
nRF driver splits its isochronous buffer in half, 511 bytes a side, and a packet
is at most 196 bytes. The audio side adds under ~2 KB of RAM.

## 7. CPU

With the stock driver, streaming costs ~18% of the SP-1's 64 MHz CPU. If your
firmware has that to spare, stop here. If it does not, the optional
[fast-path driver](../module/nrf-usbd-isofast/README.md) takes it to ~0 —
[`STOCK-OR-FAST-PATH.md`](STOCK-OR-FAST-PATH.md) weighs the two.
