# Building wakes-sp1

Windows host. Zephyr **v4.3.1** + Zephyr SDK **0.17.4**, pinned in `west.yml`. Linux and
macOS follow the same steps with the obvious path changes; the short version is in the
README.

The examples use **`C:\sp1-ws`** as the workspace — the folder that *contains* the
`wakes-sp1` clone. Any location works as long as its path has **no spaces** (see the
appendix). Substitute your own path wherever `C:\sp1-ws` appears.

Fresh machine? Skip to [Appendix: first-time setup](#appendix-first-time-setup).

---

## The build

```powershell
cd C:\sp1-ws
west build -p always -b stem_player wakes-sp1\firmware -- -DBOARD_ROOT="C:/sp1-ws/wakes-sp1"
```

Artifact: **`build\zephyr\wakes-sp1.bin`**. (`CONFIG_KERNEL_BIN_NAME="wakes-sp1"` in
`firmware/prj.conf` renames the outputs from Zephyr's default `zephyr.*`. The
`build\zephyr\` directory name itself comes from the CMake target and is unaffected.)

### Three things that are not optional

**Quote `BOARD_ROOT` and use forward slashes.** Unquoted, the argument splits at the
drive colon and CMake receives `BOARD_ROOT=C:` plus a stray path. Observed:

```
-DBOARD_ROOT=C: '\sp1-ws\wakes-sp1'
-- Configuring incomplete, errors occurred!
```

**Native Windows CMake and Ninja, not MinGW/MSYS2.**

```powershell
Get-Command cmake, ninja -All | Select-Object Name, Source
```

Anything resolving inside `C:\mingw64\bin` or an MSYS2 tree does POSIX↔Windows path
translation and mangles drive-letter arguments — the same split as above, even with correct
quoting. Delete `build\` after changing toolchains; a failed configure leaves a cache
that keeps reasserting old paths.

**The Zephyr patch must be applied.** See below.

## The local Zephyr patch — required, and not our bug

`soc/nordic/Kconfig` selects `CMSIS_CORE_HAS_SYSTEM_CORE_CLOCK`, but
`modules/cmsis_6/Kconfig` only permits that symbol for an NXP series, leaving it
unreachable for every Nordic build and fatal on a fresh Kconfig pass. It reproduces on
stock `samples/hello_world` for `nrf52840dk/nrf52840`, and was unfixed on Zephyr `main`
as of 2026-09-14.

```powershell
cd C:\sp1-ws\zephyr
git apply ..\wakes-sp1\zephyr-patches\nordic-cmsis-system-core-clock.patch
```

**Reapply after any `west update` that touches the Zephyr tree** — patches live outside
west's manifest and do not survive. `zephyr-patches/README.md` has the details.

---

## Where new code tends to break

The failure modes that recur on this board:

| Symptom | Where to look |
|---|---|
| unknown board `stem_player` | the `-DBOARD_ROOT` path; that `boards\teenageengineering\stem_player\` is intact |
| unknown Kconfig symbol | a `prj.conf` option renamed between Zephyr versions |
| `CMSIS_CORE_HAS_SYSTEM_CORE_CLOCK` unreachable | the Zephyr patch was not applied, or `west update` reverted it |
| `NRF_P0` / `GPIO_PIN_CNF_*` undeclared | the `<soc.h>` include in `sp1_board.h` |
| `struct arch_esf` mismatch | fatal-handler signature; check what v4.3.1 expects |
| DTS parse error | the board DTS or `firmware/app.overlay` |
| a `pwm*`/`adc` node not found | the DTS label, and that the matching `CONFIG_*` driver is enabled |
| `zephyr,deferred-init` unknown property, or `device_init()` undeclared | see **Deferred PWM init** below — there is a fallback |
| pinctrl error on a `PSEL` | spelling in `stem_player-pinctrl.dtsi` |
| missing module | `west.yml`'s allowlist — `cmsis`, `cmsis_6`, `hal_nordic` are all required |
| **LEDs work but wrong ones, or wrong order** | **not a build error.** pinctrl channel order vs LED index — see the comment block in `sp1_board.h` |
| **ADC reads nonsense** | **not a build error.** Channel `reg` vs `io-channels` index vs the index used in C |

The last two are the expensive ones, because they build cleanly and fail on hardware.

### USB console fallback

`firmware/CMakeLists.txt` includes
`${ZEPHYR_BASE}/samples/subsys/usb/common/common.cmake` and `sp1_console.c` includes
`<sample_usbd.h>` to use `sample_usbd_init_device()`. This is the path
sp1-tape-looper proved on this board and this Zephyr version, but it is a dependency on
Zephyr's **samples** tree, which moves between releases.

If it does not resolve:

- `CONFIG_SAMPLE_USBD_VID` undefined → sp1-tape-looper hit this on v4.3.x and shimmed it
  in `boards/.../Kconfig.stem_player` with a `default 0x2fe3`. Harmless; the device
  enumerates under Zephyr's VID instead of ours.
- `sample_usbd.h` not found → check the `common.cmake` path exists in your Zephyr
  checkout; the directory has moved before.
- Whole helper unavailable → build the USBD context directly with
  `USBD_DEVICE_DEFINE` / `USBD_CONFIGURATION_DEFINE` / `USBD_DESC_*_DEFINE` in
  `sp1_console.c`, which removes the samples dependency entirely. More boilerplate, no
  behaviour change.

**Do not fall back to `CONFIG_USB_DEVICE_STACK` (legacy).** It is smaller, but deprecated,
and mass storage later needs device_next anyway — we would only migrate twice.

**The console must never block waiting for a host.** No waiting on DTR: the device has to
boot unplugged, which is most of the time.

### Deferred PWM init, and its fallback

`pwm2` / `pwm3` carry `zephyr,deferred-init` so that pinctrl does not claim the LED pins
before `main()`. This is not cosmetic: the PWM peripheral drives its idle level until the
first duty write, and with `nordic,invert` on those channels that level **lights the
LEDs**. It flashed every LED for a few milliseconds on every boot, which showed up as a
flicker whenever `••` was tapped while the device was off.

If `zephyr,deferred-init` or `device_init()` is not available on this Zephyr version,
**do not just delete it and accept the flash.** The fallback achieves the same thing
without the property:

1. In the pinctrl file, make the node's **`pinctrl-0` (default) state the one with
   `low-power-enable`** — i.e. swap the `_default` and `_sleep` bodies — so the pins are
   left disconnected when the driver initialises.
2. In `sp1_led_init()`, call `pinctrl_apply_state(..., PINCTRL_STATE_SLEEP)` to switch to
   the driving configuration at the moment we commit to booting.

Same property, more moving parts. Prefer the DT property if it exists.

## Sanity-check before flashing

Run the same gate CI runs, from the workspace root:

```powershell
python wakes-sp1\tools\ci\check_image.py build
```

It refuses an image that is not linked at `0x20000`, that overruns the app slot, or that
has dropped any constraint from `docs/SAFETY.md` (`CONFIG_USE_DT_CODE_PARTITION`,
`CONFIG_WATCHDOG`, `CONFIG_REBOOT`, the bootloader's clock source, …), and prints every
check and the binary's size either way. It needs
`pyelftools`, which Zephyr's `requirements.txt` already installs. If it fails, fix the build,
never the check.

Also **compare the size against the previous build** rather than an absolute figure. A jump
of tens or hundreds of KB with no new subsystem means something leaked in and is worth
understanding before it reaches the device.

If the vector table sits at `0x0`, `CONFIG_USE_DT_CODE_PARTITION=y` did not take effect
and **the device will not boot**.

## Flashing

1. <https://solderless.engineering>
2. USB-C connected
3. Hold **Track 1 + Track 4** while plugging in. Four track lights solid.
4. Select `wakes-sp1.bin`, flash, unplug and replug.

That same combination is the **recovery path**, and it lives in the bootloader rather
than in our firmware, so it works even if our app is completely broken.

---

## Appendix: first-time setup

> ### The workspace path must contain no spaces
>
> Zephyr's CMake/devicetree tooling breaks on them, and `subst` / directory junctions do
> **not** work around it: CMake's `REALPATH` and Python's `os.path.realpath` both resolve
> back through the alias to the real path. Move the workspace instead — and keep **one**
> working tree. Two trees plus manual sync is how a stale binary reaches the device.

### Host prerequisites

```powershell
winget install Kitware.CMake
winget install Ninja-build.Ninja
winget install Python.Python.3.12
winget install Git.Git
```

```powershell
cmake --version      # need 3.20.0+
ninja --version
python --version     # need 3.10+
git --version
```

### Windows Python gotchas — both of these will bite you

**"Python was not found" even though Python is installed and on PATH.** That exact
wording comes from Windows' **App Execution Alias** — a stub `python.exe` in
`%LOCALAPPDATA%\Microsoft\WindowsApps` whose only job is to redirect you to the Microsoft
Store. It sits early in PATH and shadows the real interpreter.

```powershell
Get-Command python, python3, py -All | Select-Object Name, Source
```

If the first `python` resolves under `WindowsApps`, that's it. Fix: **Settings → Apps →
Advanced app settings → App execution aliases → turn off `python.exe` and `python3.exe`**,
then open a new shell. (`py --version` working while `python` fails confirms it — the `py`
launcher bypasses the stub.)

**Do not install Store Python as a workaround.** It runs under a filesystem
virtualization layer that breaks parts of Zephyr's tooling.

**`pip` / `west` not recognised.** Three relevant directories, and the third surprises
people:

| What | Where |
|---|---|
| `python.exe` | `%LOCALAPPDATA%\Programs\Python\Python3xx\` |
| `pip.exe` (bundled) | `%LOCALAPPDATA%\Programs\Python\Python3xx\Scripts\` |
| `west.exe` (from `pip install --user`) | `%APPDATA%\Python\Python3xx\Scripts\` ← **roaming** |

```powershell
python -m site --user-base     # append \Scripts
```

**The habit that sidesteps all of it:** `python -m pip`, `python -m west`. Module
invocation resolves through the interpreter you are actually running — immune to PATH
ordering, alias stubs, and the local-vs-roaming split. PATH edits only reach **newly
opened** shells.

### west and the workspace

```powershell
python -m pip install --user west
python -m west --version

mkdir C:\sp1-ws
cd C:\sp1-ws
git clone https://github.com/Worldwave/Wakes wakes-sp1
west init -l wakes-sp1
west update
west zephyr-export
python -m pip install -r zephyr\scripts\requirements.txt
```

`west.yml` sets `self: path: wakes-sp1`, so the clone **must be named `wakes-sp1`** and
the **workspace root is its parent** — init from `C:\sp1-ws`, not from inside
`wakes-sp1\`.

**⚠️ "detected dubious ownership".** On a filesystem that does not record ownership
(exFAT, some network and external drives), `west update` fails on each newly cloned
project until you trust it:

```powershell
git config --global --add safe.directory C:/sp1-ws/zephyr
git config --global --add safe.directory C:/sp1-ws/modules/hal/cmsis
git config --global --add safe.directory C:/sp1-ws/modules/hal/cmsis_6
git config --global --add safe.directory C:/sp1-ws/modules/hal/nordic
git config --global --add safe.directory C:/sp1-ws/wakes-sp1
```

These are **global** git config entries — they only mark paths as trusted. Undo with
`git config --global --unset-all safe.directory`.

### Zephyr SDK 0.17.4

```powershell
cd C:\sp1-ws\zephyr
west sdk install --version 0.17.4
west sdk list
```

If that subcommand is unavailable, download `zephyr-sdk-0.17.4_windows-x86_64.7z` from
the [Zephyr SDK releases](https://github.com/zephyrproject-rtos/sdk-ng/releases/tag/v0.17.4),
extract to `%USERPROFILE%\zephyr-sdk-0.17.4`, and run `setup.cmd` inside it.
