# Wakes

Synthesizer firmware for the Teenage Engineering SP-1, the unreleased stem player. It runs
ports of two Mutable Instruments modules together on the SP-1's nRF52840:

- **Plaits** is the voice: 21 of its 24 synthesis engines, one at a time.
- **Marbles** provides random gates and voltages (`t`, `X1`–`X3`, `Y`), routed into Plaits.

Everything is played from the SP-1's own controls: four faders, the track and play buttons, the
rocker, the `••` button as shift, and eight LEDs.

Unofficial community work. Not affiliated with or endorsed by Teenage Engineering or Mutable
Instruments. Nothing here is sold. Flashing custom firmware is at your own risk.

## Status

**v0.4.6 (M4e), pre-release.** M4d was played on hardware for an hour with no audio dropouts or
resets. M4e's first build, v0.4.5, has been flashed and smoke-tested. v0.4.6 adds three UI changes
from that testing: Unpatch no longer fires the button's ordinary action on release, a
completed rip fades back to the page, and output select moves to PLAITS SETTINGS T4 so that
`••` + T4 is Unpatch only. It awaits the full M4e hardware test.
All testing so far has been on a single SP-1 unit.

## Before you flash

Read [`docs/SAFETY.md`](docs/SAFETY.md) first. The SP-1 has no hardware reset pin.

- **Recovery is part of the SP-1's bootloader, not of Wakes.** Holding Track 1 + Track 4 while
  plugging in USB-C enters Teenage Engineering's update mode regardless of the firmware installed.
- Wakes never writes flash below `0x20000`, where the bootloader lives.
- Holding `••` for 30 s always powers the device off.

## Building

Needs Zephyr **v4.3.1** and Zephyr SDK **0.17.4**, both pinned. The workspace path must not
contain spaces. Clone the repository into a folder named `wakes-sp1`, which is the path
`west.yml` expects.

```sh
mkdir sp1-ws && cd sp1-ws
git clone https://github.com/Worldwave/Wakes wakes-sp1
west init -l wakes-sp1
west update
cd zephyr && git apply ../wakes-sp1/zephyr-patches/nordic-cmsis-system-core-clock.patch && cd ..
pip install -r zephyr/scripts/requirements.txt
west build -p always -b stem_player wakes-sp1/firmware -- -DBOARD_ROOT="$PWD/wakes-sp1"
```

The firmware is `build/zephyr/wakes-sp1.bin`. The Zephyr patch is required and is lost on every
`west update`. Windows specifics and troubleshooting: [`docs/BUILD.md`](docs/BUILD.md).

Host tests (Linux or macOS, no hardware needed):

```sh
tools/host-tests/hostbuild.sh routetest.cc uitest.c   # prints the path of each test binary
```

## Flashing

1. Open <https://solderless.engineering> in a browser and connect the SP-1 over USB-C.
2. Hold Track 1 + Track 4 while plugging in. The four track lights go solid.
3. Select the `.bin`, flash, then unplug and replug.

## Documentation

| | |
|---|---|
| [`docs/UI-PAGES.md`](docs/UI-PAGES.md) | every control on every page |
| [`config/engines.csv`](config/engines.csv) | **which engine is in each of the 24 slots**; an empty slot is skipped. Read by the build |
| [`docs/PLAITS-ENGINES.md`](docs/PLAITS-ENGINES.md) | how to edit that file, every available engine, its cost, faders and detents |
| [`docs/MARBLES-SETTINGS.md`](docs/MARBLES-SETTINGS.md) | Marbles models, ranges and scales |
| [`docs/DEFAULTS.md`](docs/DEFAULTS.md) | every parameter's default, and what a reset restores |
| [`docs/SAFETY.md`](docs/SAFETY.md) | rules for anyone changing the firmware |
| [`docs/BUILD.md`](docs/BUILD.md) | full build setup |

## Licence

MIT, Copyright (c) 2026 Adara Barami | Worldwave. See [`LICENSE`](LICENSE).

Plaits, Marbles and stmlib are by Emilie Gillet (MIT) and are included unmodified in
`third_party/eurorack/`. Changes to them are applied at build time. The board support builds on
chattock/sp1-tape-looper, timknapen/SP-1-dev and ericlewis/sp1-midi (all MIT). Details are in
[`NOTICE`](NOTICE), and the licence texts are in [`LICENSES/`](LICENSES/).
