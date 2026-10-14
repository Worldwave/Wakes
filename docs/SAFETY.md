# Safety rules for SP-1 firmware development

## The recovery path, and why it can't be taken away

Hold **Track 1 + Track 4** while plugging in USB-C. The four track lights come on
solid and the device is in firmware-loading mode. Flash from
<https://solderless.engineering>.

That button scan lives in the **TE bootloader at `0x00000`–`0x1FFFF`**, which no
application can write. As long as we respect that boundary, **no bug in our
firmware can make the device unrecoverable** — not a hang, not a hard fault, not
a corrupt image.

This is the single most important property of the project. Protect it.

## The rules

**1. Never write flash below `0x20000`.** The app image lives at `0x20000`
(`slot0_partition`), placed there by `CONFIG_USE_DT_CODE_PARTITION=y` in the board
defconfig. Remove that option and the vector table lands at `0x0` and the device
will not boot.

**2. Feed the watchdog in under 5 s, from every loop that can block.** Use
`sp1_wdt_feed()`, which reloads all eight channels — the bootloader may have armed
channels the app does not own. The watchdog is the final backstop for a hang.

**3. Do not re-initialise bootloader-owned clocks or peripherals.** In particular
leave `CONFIG_CLOCK_CONTROL_NRF_K32SRC_SYNTH=y` alone: no 32.768 kHz crystal is
driven for the app, and LFCLK is synthesised.

**4. `SYSTEM_OFF` is the return path to the bootloader.** With no reset pin, an app
without a working power-off leaves battery disconnection — which means opening the
device — as the only way out. An early community firmware shipped without it and
this is exactly what happened. `sp1_power_off()` implements the full sequence.

**5. Clear `RESETREAS` at boot and again immediately before `SYSTEM_OFF`.**

**6. Never spin in a fault handler.** `k_sys_fatal_error_handler()` must reboot.
Zephyr's default is to halt, which on this device is indistinguishable from a brick
until you open it.

**7. Power down the external chips before `SYSTEM_OFF`.** `SYSTEM_OFF` stops only
the nRF. The TAS2505 amp, CS42L42 codec, 3.072 MHz oscillator and eMMC I/O rail are
separate chips held up by retained GPIO levels. Leaving them powered drains the
battery overnight, and a powered amp with no clock can murmur audibly.

**8. Clear every LED before `SYSTEM_OFF`.** It freezes GPIO levels; anything still
lit stays lit into sleep.

**9. Nothing the user can configure reaches the power path.** The config files on the
drive (`WAKES/CNFG/*.cnfg`, issue #52) are read at every ON and a user can put anything in
them. They never name power-off or its timings, the watchdog, the flash layout, or the USB
descriptors, and none of those is ever read from the drive. A bad line keeps its built-in
default; a file that crashes the device at ON can be deleted from a computer in STANDBY,
because drive mode never reads `CNFG/`.

Rules 1–5 and 7 are the "BIG FIVE" as established by the author of
`chattock/sp1-tape-looper` on real hardware. We did not discover them; we are
inheriting them, and they were expensive to find.

## Know this before your first flash

**The stock TE firmware is not publicly redistributable, and flashing over it is
one-way.** You keep the ability to flash any community firmware — the device is
never stranded — but the stock stem-player experience does not come back.

## Unresolved: the page at `0xFF000`

The SP-1 knowledgebase reports page `0xFF` (the last 4 KB) as reserved by the
bootloader, presumed MBR settings. `sp1-tape-looper` uses it as a Zephyr settings
partition. Both cannot be right.

Until that is resolved, **wakes-sp1 declares the partition but never writes it.**
We have no persistent settings yet, so there is no cost to waiting. When we do need
storage, the eMMC is the safe answer.

## Build-and-flash checklist

Before every flash to hardware:

- [ ] `CONFIG_USE_DT_CODE_PARTITION=y` still set
- [ ] no partition in the DTS overlaps `0x00000`–`0x1FFFF`
- [ ] the watchdog is fed in every loop that can block
- [ ] `k_sys_fatal_error_handler()` reboots rather than halting
- [ ] power-off still works — test it *every time*, not just when you change it
- [ ] you can still enter the bootloader with Track 1 + Track 4
