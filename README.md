<p align="center">
  <img src="docs/images/wakes-logo.png" width="520"
       alt="Wakes: a love letter to Mutable Instruments, by Worldwave">
</p>

Synthesizer firmware for the Teenage Engineering SP-1, the unreleased stem player. It runs
ports of two Mutable Instruments modules together on the SP-1's own controls:

- **Plaits**, the voice: 21 of its 24 synthesis engines, one at a time.
- **Marbles**, random gates and voltages routed into Plaits to sequence it.

Unofficial community work. Not affiliated with or endorsed by Teenage Engineering or Mutable
Instruments. Nothing here is sold. Flashing custom firmware is at your own risk.

## 📖 Manual

**[Wakes – Manual (PDF)](docs/Wakes%20-%20Manual.pdf)**: every page, every control, and how
the two modules play together. Start here.

## Status

**v0.4.6, pre-release for beta testing.** Tested on a single SP-1 unit. Report problems with
the bug form under [Issues](https://github.com/Worldwave/Wakes/issues).

## Flashing

Read [`docs/SAFETY.md`](docs/SAFETY.md) first: the SP-1 has no hardware reset pin.

1. Download `wakes-sp1-v0.4.6.bin` from the
   [latest release](https://github.com/Worldwave/Wakes/releases).
2. Open <https://solderless.engineering> and connect the SP-1 over USB-C.
3. Hold **Track 1 + Track 4** while plugging in. The four track lights go solid.
4. Select the `.bin`, flash, then unplug and replug.

That Track 1 + Track 4 recovery lives in Teenage Engineering's bootloader, not in Wakes, so it
works whatever firmware is installed. Wakes never writes below `0x20000`, where the bootloader
lives, and holding `••` for 30 s always powers the device off.

## Building

Zephyr **v4.3.1** and Zephyr SDK **0.17.4**, both pinned. The workspace path must not contain
spaces, and the clone must be named `wakes-sp1`.

```sh
mkdir sp1-ws && cd sp1-ws
git clone https://github.com/Worldwave/Wakes wakes-sp1
west init -l wakes-sp1 && west update
cd zephyr && git apply ../wakes-sp1/zephyr-patches/nordic-cmsis-system-core-clock.patch && cd ..
pip install -r zephyr/scripts/requirements.txt
west build -p always -b stem_player wakes-sp1/firmware -- -DBOARD_ROOT="$PWD/wakes-sp1"
```

Output: `build/zephyr/wakes-sp1.bin`. The Zephyr patch is required and is lost on every
`west update`. Windows setup and troubleshooting: [`docs/BUILD.md`](docs/BUILD.md).

## Reference

| | |
|---|---|
| [`docs/UI-PAGES.md`](docs/UI-PAGES.md) | every control on every page, in detail |
| [`config/engines.csv`](config/engines.csv) | which engine sits in each of the 24 slots (editable) |
| [`docs/PLAITS-ENGINES.md`](docs/PLAITS-ENGINES.md) | every engine, its faders and its CPU cost |
| [`docs/MARBLES-SETTINGS.md`](docs/MARBLES-SETTINGS.md) | Marbles models, ranges and scales |
| [`docs/DEFAULTS.md`](docs/DEFAULTS.md) | every default, and what a reset restores |
| [`docs/SAFETY.md`](docs/SAFETY.md) | rules for anyone changing the firmware |

## Licence

MIT, Copyright (c) 2026 Adara Barami | Worldwave. See [`LICENSE`](LICENSE).

Plaits, Marbles and stmlib are by Émilie Gillet (MIT), included unmodified in
`third_party/eurorack/`. The board support builds on chattock/sp1-tape-looper,
timknapen/SP-1-dev and ericlewis/sp1-midi (all MIT). Details in [`NOTICE`](NOTICE) and
[`LICENSES/`](LICENSES/).
