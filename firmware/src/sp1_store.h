/*
 * wakes-sp1 — storage on the eMMC (M6, #43; CONFIG_SP1_STORAGE): a FAT32 volume and the
 * thread that owns it. PRST's presets will live here; today it runs the fresh format
 * and the storage test.
 *
 * ---- the card ----
 * One MBR partition, FAT32, starting at block 8192 (4 MB: the card's erase group, so the
 * volume and its data area sit on erase boundaries), 32 KB clusters, two FATs -- the
 * layout an SD card formatter makes, so a computer reading the card later sees an
 * ordinary drive. Mounted at "/EMMC:". 8.3 names only (no LFN): /EMMC:/WAKES/...
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

/* Every entry to ON (main thread): queue the ON job -- the fresh check and format
 * (CONFIG_SP1_FRESH), the report and file test (CONFIG_SP1_STORAGE_TEST). Returns at
 * once; skipped, with a log line, if the previous job is still running. */
void sp1_store_on_enter(void);

/* From sp1_quiesce_peripherals(), on every way out of ON: stop the job and power the
 * card down now. A job cut short by this fails its remaining steps; a format cut short
 * leaves no FRESH.ID, so the fresh image formats again at the next ON. Never waits. */
void sp1_store_abort(void);

#endif /* SP1_STORE_H */
