# config/ — Wakes' built-in defaults

**These files are not the settings on your Wakes.** They are the defaults every firmware image is
built with. You do not need to edit them, fork this repository or build anything to change how
your Wakes behaves.

## To change your Wakes' settings

Edit the copies on Wakes' own drive:

1. Plug Wakes into a computer while it is off (STANDBY). It shows up as a drive.
2. Open `WAKES/CNFG/` and edit the file in a text editor.
3. Eject the drive and turn Wakes ON. It reads the files at every ON.

If a file is missing from `WAKES/CNFG/`, Wakes writes it there with these defaults at the next ON,
so deleting a file is how you reset it. Each file explains its own settings, and a line Wakes
cannot read keeps its default and is named in the log (`WAKES/LOGS/`).

| File | What it sets |
|---|---|
| `engines.cnfg` | which Plaits engine sits in each of the 24 slots, and what every engine does |
| `midi.cnfg` | MIDI over USB: channel, note priority, legato, portamento, which CC moves which parameter |
| `audio.cnfg` | USB audio out: its level (parked or on VOL) and the speaker while a host records |
| `logs.cnfg` | the log Wakes writes to its drive, and what goes in it |

## When to edit these

Only to build your own firmware with **different defaults**: the values a Wakes uses when its drive
has no file, and the text it writes there when one is missing. The build checks every file here and
stops, naming the line, if one is wrong, so a broken default can never ship.

You can also copy a file from here to `WAKES/CNFG/` to get a clean one back, though deleting it
from the drive does the same thing.
