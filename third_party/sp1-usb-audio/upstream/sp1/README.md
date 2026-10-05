# SP-1 USB audio example

This is a standalone Zephyr firmware for the Teenage Engineering SP-1. It plays
a continuous 1 kHz sine wave at −12 dBFS through the speaker or headphones and
exposes the same tone to a computer as a 48 kHz, 16-bit stereo UAC2 input. The
speaker/headphone copy follows the SP-1 master volume; the USB copy stays at a
fixed −6 dB gain.

The example includes the complete SP-1 power baseline rather than only the USB
audio path. **It has run on an SP-1** (both USB drivers, macOS, September
2026): power on and off, the tone on the speaker, Vol+/Vol- including held
repeat, headphones muting the speaker, a clean fixed-level USB recording, and the
bootloader still reachable afterwards were all checked. Not yet tried: unplugging
USB mid-stream. Keep a known-good image to
flash back all the same.

## Controls and indicators

- Hold **•• for 1.5 seconds** to power on. Track LED 1 lights during the hold,
  followed by two Track 1→4 sweeps. Only recovery from a watchdog reset skips
  the hold. Releasing early on battery returns immediately to `SYSTEM_OFF`.
- Hold **•• for 5 seconds** to power off. A ••+button combination never counts
  toward that hold. Power-off mutes and resets the codecs, stops the audio
  oscillator, darkens every LED, parks the outputs, and arms •• as the wake pin.
- **Vol+ and Vol−** control only the speaker/headphone output. A press changes
  one of 16 perceptual steps; holding repeats after 500 ms and then every 110 ms.
  The firmware boots at the quiet 45/256 setting, so press Vol+ for more level.
- Plugging in headphones mutes the speaker. Removing them restores it after the
  same debounced jack detection used by the source firmware.
- The bottom side LED is the running indicator. While holding **••+Track 1**,
  the side row shows the battery charge gauge instead. While off on USB, whole
  quarters are solid and the next quarter blinks; all four are solid when full.

The audio thread uses 128-frame blocks, a two-block I2S transmit queue, and
preemptible priority 0. The CS42L42 is frame master; the nRF52840 and TAS2505 are
slaves to its 48 kHz clock. The watchdog starvation guard yields if the main
loop has not fed the watchdog for 1.5 seconds.

## Build

The build needs `west`, `ZEPHYR_BASE`, and a Zephyr SDK selected with
`ZEPHYR_TOOLCHAIN_VARIANT=zephyr` and `ZEPHYR_SDK_INSTALL_DIR`. It was developed
against the Zephyr in nRF Connect SDK v3.3.0 (`sdk-zephyr` commit `fd9204a0`) and
Zephyr SDK 0.17.0.

The `CONFIG_PM` Kconfig warning comes from the upstream `sp1` board's defconfig,
does not apply to the nRF52840, and is harmless.

`prep.sh` stages, but does not commit, two pinned dependencies:

- the `sp1` board from [softmodded/marisko](https://github.com/softmodded/marisko)
  at `b787d90`;
- the unmodified `led`, `controls`, `buttons`, and `sp1_board` hardware files
  from [bnjreece/feldd-sp1-firmware](https://github.com/bnjreece/feldd-sp1-firmware)
  at `b70a3a2`.

Set `SP1_BOARD_ROOT` and `FELDD_DIR` to existing checkouts at exactly those
commits. If either is unset, the script clones the pinned dependency into its
build cache. The staging directory must be outside every Git working tree; set
`SP1_BUILD_ROOT` if the default location is unsuitable.

```sh
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk-0.17.0
export ZEPHYR_BASE=/path/to/zephyr

./sp1/prep.sh
SP1_ISOFAST=1 ./sp1/prep.sh
```

The first command builds with Zephyr's stock nRF USB driver. The second replaces
it with the optional `nrf-usbd-isofast` driver. `prep.sh` emits a flashable bare
application `.bin` and audits the vector address, watchdog, power path, audio
path, UAC2 symbols, I2S queue, and selected USB driver after linking. The USB
descriptor uses Zephyr's test VID/PID (`0x2fe3`/`0x0001`); use your own for any
distributed product.

There is deliberately no CDC console. The power baseline does not need one, and
omitting it keeps this example focused on UAC2.

## Flash with the stock bootloader

1. Power the SP-1 off.
2. Hold Track 1 and Track 4, plug in USB-C, and keep holding until Track LED 1
   lights solid. Release the buttons. The power button is not part of this
   combination.
3. In Chrome or Edge, open [Solderless](https://solderless.engineering), choose
   its firmware utility, connect to the SP-1 bootloader, and upload the generated
   `.bin`. Alternatively, use the `softmodded/rome` CLI:

   ```sh
   rome flash -p /dev/your-serial-device path/to/sp1_usb_audio.bin
   ```

After flashing, hold •• for 1.5 seconds to start the example. To return to the
bootloader later, hold •• for 5 seconds to power off, then repeat the Track 1+4
procedure. Do not flash this untested example onto a unit that cannot be
recovered through its stock bootloader or SWD.
