/*
 * wakes-sp1 — storage on the eMMC (M6, #43; CONFIG_SP1_STORAGE): a FAT32 volume and the
 * thread that owns it. PRST's presets will live here; today it runs the fresh format
 * and the storage test.
 *
 * ---- the card ----
 * One MBR partition, FAT32, starting at block 8192 (4 MB: the card's erase group, so the
 * volume and its data area sit on erase boundaries), 32 KB clusters, two FATs, volume
 * label "SP-1" (Adara: what the computer calls the drive) -- the layout an SD card
 * formatter makes, so a computer sees an ordinary drive. Mounted at "/EMMC:". Long file
 * names (Adara: presets are "<name>.prst", ini inside): /EMMC:/WAKES/...
 *
 * ---- formatting: ONLY wakes-sp1-fresh (Adara, 2026-10-07) ----
 * "Flashing wakes-sp1-fresh formats the eMMC. Flashing a normal wakes-sp1 bin does not
 * touch the eMMC." Enforced at build level: without CONFIG_SP1_FRESH the image contains
 * no formatter at all (CONFIG_FILE_SYSTEM_MKFS off, so FatFs is built without f_mkfs),
 * mounting never formats (FS_MOUNT_FLAG_NO_FORMAT, and CONFIG_FS_FATFS_MOUNT_MKFS off),
 * and tools/ci/check_image.py fails a normal image that links f_mkfs.
 * The fresh image formats on its first entry to ON and writes its own build ID to
 * /EMMC:/WAKES/FRESH.ID; while that file matches, it never formats again. ⚠️ So flashing
 * the SAME fresh file twice formats once: the second flash is indistinguishable from a
 * reboot (the image cannot see its own flashing without writing the nRF's flash).
 *
 * ---- threading ----
 * Everything runs in one thread at K_PRIO_PREEMPT(5): below audio (0) and below main
 * and the USB threads (1), so it only ever uses CPU they leave idle -- no cost to the
 * audio block. It does NOT feed the watchdog (sp1_emmc.h). A format takes several
 * seconds, during which main waits in storage_gate() -- still feeding the watchdog and
 * still running sp1_power_tick(), so the 30 s backstop is untouched (rule 5a).
 */
#ifndef SP1_STORE_H
#define SP1_STORE_H

#include <stdint.h>

/* Once at boot, before any other call: registers the disk and starts the thread. */
void sp1_store_init(void);

/* ---- the fresh format holds ON entry (Adara, 2026-10-07) ----
 * "Formatting the eMMC should hold up the rest of the UI while the device is being turned
 * on. We can still turn the SP-1 off during the process in case it hangs, using the 30s
 * SHFT hold backstop." main.c's storage_gate() waits on these between queuing the job and
 * starting audio, drawing the progress bar from sp1_store_progress(). Fresh builds only:
 * a normal image never formats, so it never waits. */
enum sp1_store_phase {
	SP1_STORE_READY = 0,     /* no job, or the job is past its fresh step             */
	SP1_STORE_CHECKING,      /* job queued: powering the card, reading FRESH.ID        */
	SP1_STORE_FORMATTING,    /* formatting                                            */
};
enum sp1_store_format {
	SP1_STORE_FMT_NONE = 0,  /* no format this ON entry                               */
	SP1_STORE_FMT_DONE,      /* formatted and stamped                                 */
	SP1_STORE_FMT_FAILED,    /* tried and failed (or stopped): formats again next ON  */
};
enum sp1_store_phase sp1_store_phase(void);
enum sp1_store_format sp1_store_format_result(void);
/* 0-255 through a format: the format's block writes so far against the count FatFs will
 * make for this card (exact for FAT32: both FATs, the root cluster, the boot sectors). */
uint8_t sp1_store_progress(void);

/* ---- is there a filesystem to use? (Adara, 2026-10-07) ----
 * "On boot, check if the partition is formatted. If it's not formatted, don't format unless
 * it's Fresh. In this state, boot, but disable the PRST feature and key combos related to
 * it. As we add the PRST feature and related combos, we should build them with a check of
 * whether the partition is formatted or not."
 * So EVERY PRST feature and key combo asks sp1_store_volume() and does nothing unless it
 * says OK. "Formatted" = a FAT volume FatFs can mount (wakes-sp1-fresh's, or a FAT32 a
 * computer made); WAKES/ is created when first needed. Set by the ON job a fraction of a
 * second after ON, and by the drive job in STANDBY. UNKNOWN means "not looked yet" --
 * treat it as NO, never as yes. Without a volume the drive is not offered either: a
 * computer shown a raw card offers to format it, possibly as exFAT, which Wakes cannot
 * read. */
enum sp1_store_volume {
	SP1_STORE_VOL_UNKNOWN = 0,
	SP1_STORE_VOL_OK,
	SP1_STORE_VOL_NONE,      /* no card, or no FAT volume on it: PRST is off           */
};
enum sp1_store_volume sp1_store_volume(void);

/* Every entry to ON (main thread): queue the ON job -- take the card from a host (drive
 * mode), the fresh check and format (CONFIG_SP1_FRESH), the report and file test
 * (CONFIG_SP1_STORAGE_TEST). Returns at once. Never dropped: if another job is running it
 * runs next, ahead of anything else queued. */
void sp1_store_on_enter(void);

#if defined(CONFIG_SP1_DRIVE)
#include <stdbool.h>

/* ---- drive mode (M6, Adara 2026-10-07) ----
 * In STANDBY with a host attached the card is a USB drive (automatic, no gesture); turning
 * ON takes it back after the transfer in flight. The mass-storage interface is always in
 * the descriptor: while the host does not own the card it reads "no medium", so going
 * STANDBY <-> ON never re-enumerates. ⚠️ A host that EJECTS the drive does not get it back
 * until the next USB reset (a replug): Zephyr's class keeps "ejected" until then, the way
 * a card reader keeps a removed card out.
 * sp1_standby_run() calls these: enter once per visit, tick every control tick with
 * sp1_usbd_host(). sp1_store_activity() counts the host's transfers, for the LEDs. */
void sp1_store_standby_enter(void);
void sp1_store_standby_tick(bool host);
uint32_t sp1_store_activity(void);
#endif

#if defined(CONFIG_SP1_PLAITS)
#include <stdbool.h>
#include "sp1_prst.h"

/* ---- PRST: the four slots (M6, #50) ----
 * WAKES/PRST/1 .. 4 on the volume, one `.prst` per folder, any name: the FOLDER decides the
 * slot (Adara). More than one file in a folder -> the first in alphabetical order (case
 * ignored, byte order), never the others. WAKES/PRST/SLOT holds the last-loaded slot's
 * number (missing or unreadable -> slot 1).
 *
 * LOAD: the ON job reads all four into RAM right after the volume check, so a slot change
 * never touches the card while playing. A folder with no `.prst` (or no folder) is that
 * slot at the ROTC defaults, PRST staying on. A card that fails -- it does not start, no
 * filesystem, a read error -- turns PRST off for the session. main.c waits for the answer
 * before audio starts, at most SP1_PRST_LOAD_MS (10 s, Adara), and treats a late answer
 * as off.
 *
 * SAVE: only at a normal shutdown (sp1_power_set_save_hook), into the current slot. The
 * text goes to a temporary file in the slot's folder (not ending in `.prst`, so never
 * loaded), which then replaces the last-loaded file -- or becomes `N.prst` in an empty
 * folder. Other files in the folder are never touched. Then SLOT. */
#define SP1_PRST_SLOTS   4
#define SP1_PRST_LOAD_MS 10000u
#define SP1_PRST_SAVE_MS 4000u

enum sp1_store_prst {
	SP1_STORE_PRST_WAIT = 0,     /* the ON job has not read the slots yet          */
	SP1_STORE_PRST_READY,        /* all four in RAM: sp1_store_prst_slot()         */
	SP1_STORE_PRST_OFF,          /* no card, no filesystem, or a read failed       */
};
enum sp1_store_prst sp1_store_prst(void);
/* After READY only. 0..3. */
int sp1_store_prst_current(void);
const struct sp1_prst *sp1_store_prst_slot(int slot);

/* Main thread, at shutdown: write `p` into `slot` and make it the current one. Queues the
 * job and returns; false if it could not be queued (PRST is not READY). Poll the result. */
bool sp1_store_prst_save(int slot, const struct sp1_prst *p);
enum sp1_store_save {
	SP1_STORE_SAVE_IDLE = 0,
	SP1_STORE_SAVE_BUSY,
	SP1_STORE_SAVE_OK,
	SP1_STORE_SAVE_FAILED,
};
enum sp1_store_save sp1_store_save_result(void);
#endif

/* From sp1_quiesce_peripherals(), on every way out of ON and out of STANDBY: stop the job,
 * take the card from a host, drop queued jobs, and power the card down now. A job cut short by this fails its remaining steps; a format cut short
 * leaves no FRESH.ID, so the fresh image formats again at the next ON. Never waits. */
void sp1_store_abort(void);

#endif /* SP1_STORE_H */
