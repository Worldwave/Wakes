/*
 * wakes-sp1 — storage on the eMMC (M6, #43). See sp1_store.h.
 *
 * FatFs (ChaN, R0.16, through Zephyr's fs layer) sits on a disk driver here that reads
 * and writes single blocks through sp1_emmc.c. Every console line starts "STORE" (this
 * file) or "EMMC" (sp1_emmc_report.c).
 */
#include "sp1_store.h"
#include "sp1_emmc.h"
#include "sp1_console.h"
#if defined(CONFIG_SP1_STORAGE_TEST)
#include "sp1_emmc_report.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/drivers/disk.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/app_version.h>
#include <ff.h>
#include <errno.h>
#include <string.h>

#define DISK_NAME  "EMMC"            /* = CONFIG_FS_FATFS_CUSTOM_MOUNT_POINTS */
#define MNT        "/EMMC:"
#define DIR_WAKES  MNT "/WAKES"

/* The partition starts one erase group in: 8192 blocks = 4 MB (EXT_CSD
 * HC_ERASE_GRP_SIZE 8 x 512 KB on this card, logs/sp1-20261007-152119.log). */
#define PART_START 8192u

#define LINE(...) do { printk(__VA_ARGS__); sp1_console_pace(); } while (0)

static volatile bool stop;
static atomic_t busy;

/* What main's storage_gate() reads (sp1_store.h). */
static atomic_t phase = ATOMIC_INIT(SP1_STORE_READY);
static atomic_t fmt_result = ATOMIC_INIT(SP1_STORE_FMT_NONE);
static atomic_t progress;
static bool     counting;            /* a format is running: count its writes */
static uint32_t fmt_written;
static uint32_t fmt_expected = 1u;

/* Every block write goes through here, so a format's progress can be counted. */
static bool wr(uint32_t block, const uint8_t *buf)
{
	if (!sp1_emmc_write_block(block, buf)) {
		return false;
	}
	if (counting) {
		fmt_written++;
		const uint32_t p = fmt_written * 255u / fmt_expected;
		atomic_set(&progress, (atomic_val_t)(p > 254u ? 254u : p));   /* 255 = stamped */
	}
	return true;
}

/* ---------------------------------------------------------------- the disk */

/* FatFs sees blocks [view_base, view_base + view_count). Normally the whole card; while
 * formatting, the partition alone, so FatFs builds a volume that starts at the
 * partition's first block (format_card). */
static uint32_t view_base;
static uint32_t view_count;

static int d_init(struct disk_info *d)
{
	ARG_UNUSED(d);
	return sp1_emmc_ready() ? 0 : -EIO;     /* the job powers the card up itself */
}

static int d_status(struct disk_info *d)
{
	ARG_UNUSED(d);
	return sp1_emmc_ready() ? DISK_STATUS_OK : DISK_STATUS_UNINIT;
}

static int d_read(struct disk_info *d, uint8_t *buf, uint32_t start, uint32_t n)
{
	ARG_UNUSED(d);
	if (start >= view_count || n > view_count - start ||
	    !sp1_emmc_read_blocks(view_base + start, buf, n)) {
		return -EIO;
	}
	return 0;
}

static int d_write(struct disk_info *d, const uint8_t *buf, uint32_t start, uint32_t n)
{
	ARG_UNUSED(d);
	if (start >= view_count || n > view_count - start) {
		return -EIO;
	}
	if (counting) {                          /* a format: block by block, for the bar */
		for (uint32_t i = 0; i < n; i++) {
			if (!wr(view_base + start + i, buf + i * SP1_EMMC_BLOCK)) {
				return -EIO;
			}
		}
		return 0;
	}
	return sp1_emmc_write_blocks(view_base + start, buf, n) ? 0 : -EIO;
}

static int d_ioctl(struct disk_info *d, uint8_t cmd, void *buf)
{
	ARG_UNUSED(d);
	switch (cmd) {
	case DISK_IOCTL_GET_SECTOR_COUNT:
		*(uint32_t *)buf = view_count;
		return 0;
	case DISK_IOCTL_GET_SECTOR_SIZE:
		*(uint32_t *)buf = SP1_EMMC_BLOCK;
		return 0;
	case DISK_IOCTL_GET_ERASE_BLOCK_SZ:       /* in blocks: f_mkfs aligns the data area */
		*(uint32_t *)buf = PART_START;
		return 0;
	case DISK_IOCTL_CTRL_SYNC:                /* the card's write cache is off */
	case DISK_IOCTL_CTRL_DEINIT:              /* the job powers the card down itself */
		return 0;
	case DISK_IOCTL_CTRL_INIT:
		return sp1_emmc_ready() ? 0 : -EIO;
	default:
		return -EINVAL;
	}
}

static const struct disk_operations disk_ops = {
	.init = d_init, .status = d_status, .read = d_read, .write = d_write,
	.ioctl = d_ioctl,
};

static struct disk_info disk = { .name = DISK_NAME, .ops = &disk_ops };

/* ---------------------------------------------------------------- the volume */

static FATFS fat;
static struct fs_mount_t mnt = {
	.type = FS_FATFS,
	.fs_data = &fat,
	.mnt_point = MNT,
	/* ⚠️ Never format on mount. The only formatter is format_card(), fresh only. */
	.flags = FS_MOUNT_FLAG_NO_FORMAT,
};
static bool mounted;

static int mount_vol(void)
{
	if (!mounted) {
		const int rc = fs_mount(&mnt);
		mounted = (rc == 0);
		return rc;
	}
	return 0;
}

static void unmount_vol(void)
{
	if (mounted) {
		(void)fs_unmount(&mnt);
		mounted = false;
	}
}

static int write_file(const char *path, const void *d, size_t n)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	int rc = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE);
	if (rc != 0) {
		return rc;
	}
	rc = fs_truncate(&f, 0);
	if (rc == 0) {
		const ssize_t w = fs_write(&f, d, n);
		rc = (w == (ssize_t)n) ? 0 : (w < 0 ? (int)w : -EIO);
	}
	const int rc2 = fs_close(&f);
	return rc != 0 ? rc : rc2;
}

static int read_file(const char *path, void *d, size_t max, size_t *got)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	*got = 0;
	int rc = fs_open(&f, path, FS_O_READ);
	if (rc != 0) {
		return rc;
	}
	const ssize_t r = fs_read(&f, d, max);
	rc = (r < 0) ? (int)r : 0;
	*got = (r > 0) ? (size_t)r : 0u;
	(void)fs_close(&f);
	return rc;
}

/* ---------------------------------------------------------------- fresh */

#if defined(CONFIG_SP1_FRESH)

#define STAMP DIR_WAKES "/FRESH.ID"

static uint8_t sec[SP1_EMMC_BLOCK];      /* block 0, the boot sectors, the MBR */

/* Different for every build of the fresh image, so each one formats once. */
static const char image_id[] = "Wakes v" APP_VERSION_STRING " fresh, built "
			       __DATE__ " " __TIME__;

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* One FAT32 partition at PART_START, the MBR written last. */
static int format_card(void)
{
	const uint32_t total = sp1_emmc_sectors();
	if (total < 4u * PART_START) {
		return -EINVAL;
	}

	/* The progress bar's denominator: what f_mkfs writes for FAT32 (R0.16) -- the boot
	 * sector, its backup and the two FSINFO sectors, both FATs, the root cluster -- plus
	 * our block 0 twice, the two HiddSec patches, then WAKES' own cluster (mkdir zeroes
	 * all 64 blocks of it) and the stamp. Hardware 2026-10-07: 2035 writes, against
	 * 1966 before the mkdir cluster was counted (logs/sp1-20261007-173828.log). */
	const uint32_t clusters = (total - PART_START) / 64u;
	const uint32_t sz_fat = ((clusters + 2u) * 4u + SP1_EMMC_BLOCK - 1u) / SP1_EMMC_BLOCK;
	fmt_expected = 4u + 2u * sz_fat + 64u + 4u + 64u + 6u;
	fmt_written = 0u;
	atomic_set(&progress, 0);
	counting = true;

	/* 1. Invalidate block 0 first: a format cut short then leaves NO partition table,
	 *    rather than an old one pointing at a half-made volume. */
	memset(sec, 0, sizeof(sec));
	if (!wr(0, sec)) {
		return -EIO;
	}

	/* 2. FAT32 into the partition, which FatFs sees as a whole disk. */
	view_base = PART_START;
	view_count = total - PART_START;
	MKFS_PARM opt = {
		.fmt = FM_FAT32 | FM_SFD,    /* no table of its own: we write the MBR   */
		.n_fat = 2,                  /* as a computer's formatter would         */
		.align = 0,                  /* from the disk: PART_START (erase group) */
		.n_root = 0,                 /* FAT32: root is a cluster chain          */
		.au_size = 32768,            /* 32 KB clusters                          */
	};
	int rc = fs_mkfs(FS_FATFS, (uintptr_t)DISK_NAME ":", &opt, 0);
	view_base = 0;
	view_count = total;
	if (rc != 0) {
		return rc;
	}

	/* 3. BPB_HiddSec (offset 28) = the partition's start, in the boot sector and its
	 *    backup (BPB_BkBootSec, offset 50). FatFs ignores it; other systems may not. */
	if (!sp1_emmc_read_block(PART_START, sec) || memcmp(&sec[82], "FAT32", 5) != 0) {
		return -EIO;
	}
	const uint32_t backup = (uint32_t)sec[50] | ((uint32_t)sec[51] << 8);
	put32(&sec[28], PART_START);
	if (!wr(PART_START, sec)) {
		return -EIO;
	}
	if (backup != 0u) {
		if (!sp1_emmc_read_block(PART_START + backup, sec)) {
			return -EIO;
		}
		put32(&sec[28], PART_START);
		if (!wr(PART_START + backup, sec)) {
			return -EIO;
		}
	}

	/* 4. The MBR: one partition, type 0x0C (FAT32, LBA). CHS fields are the
	 *    "beyond CHS" marker; everything reads the LBA fields. */
	memset(sec, 0, sizeof(sec));
	put32(&sec[440], k_cycle_get_32() ^ total);      /* disk signature */
	uint8_t *e = &sec[446];
	e[0] = 0x00;
	e[1] = 0xfe; e[2] = 0xff; e[3] = 0xff;
	e[4] = 0x0c;
	e[5] = 0xfe; e[6] = 0xff; e[7] = 0xff;
	put32(&e[8], PART_START);
	put32(&e[12], total - PART_START);
	sec[510] = 0x55;
	sec[511] = 0xaa;
	return wr(0, sec) ? 0 : -EIO;
}

static void fresh(void)
{
	static char found[sizeof(image_id) + 8];
	size_t n = 0;
	const int mrc = mount_vol();
	int rc = mrc;
	if (rc == 0) {
		rc = read_file(STAMP, found, sizeof(found) - 1u, &n);
		found[rc == 0 ? n : 0u] = '\0';
		if (rc == 0 && strcmp(found, image_id) == 0) {
			LINE("STORE fresh: formatted by this image already -- not formatting\n");
			return;
		}
	}
	if (mrc != 0) {
		LINE("STORE fresh: no Wakes filesystem on the card -- FORMATTING\n");
	} else if (rc != 0) {
		LINE("STORE fresh: a filesystem with no fresh stamp -- FORMATTING\n");
	} else {
		LINE("STORE fresh: formatted by \"%s\" -- FORMATTING\n", found);
	}
	unmount_vol();

	/* From here main's storage_gate() shows the progress bar and holds the UI. */
	atomic_set(&fmt_result, SP1_STORE_FMT_FAILED);   /* until it is stamped */
	atomic_set(&phase, SP1_STORE_FORMATTING);
	const uint32_t t0 = k_uptime_get_32();
	rc = format_card();
	LINE("STORE fresh: format %s (%d) in %u ms: FAT32 at block %u, %u blocks\n",
	     rc == 0 ? "done" : "FAILED", rc, (unsigned)(k_uptime_get_32() - t0),
	     (unsigned)PART_START, (unsigned)(sp1_emmc_sectors() - PART_START));
	if (rc == 0 && !stop) {
		rc = mount_vol();
		if (rc == 0) {
			rc = fs_mkdir(DIR_WAKES);
		}
		if (rc == 0) {
			/* The stamp goes LAST: if anything before it failed, the next ON
			 * formats again. */
			rc = write_file(STAMP, image_id, strlen(image_id));
		}
		LINE("STORE fresh: %s \"%s\"%s\n", rc == 0 ? "stamped" : "stamp FAILED",
		     image_id, rc == 0 ? "" : " -- will format again at the next ON");
	}
	counting = false;
	LINE("STORE fresh: %u block writes against %u expected\n", (unsigned)fmt_written,
	     (unsigned)fmt_expected);
	if (rc == 0 && !stop) {
		atomic_set(&progress, 255);
		atomic_set(&fmt_result, SP1_STORE_FMT_DONE);
	}
}

#endif /* CONFIG_SP1_FRESH */

/* ---------------------------------------------------------------- the test */

#if defined(CONFIG_SP1_STORAGE_TEST)

#define BOOTS    DIR_WAKES "/BOOTS.TXT"
#define TESTFILE DIR_WAKES "/TEST.BIN"
#define TEST_KB  512u
/* 16 KB chunks: FatFs hands a whole-sector write straight to the disk, so each one is a
 * 32-block burst -- what fast transfer is for. Test builds only. */
#define CHUNK    16384u
static uint8_t bulk[CHUNK];

static uint8_t pattern(uint32_t i, uint32_t seed)
{
	return (uint8_t)(i * 131u + seed * 7u + (i >> 9));
}

static void file_test(void)
{
	int rc = fs_mkdir(DIR_WAKES);
	if (rc != 0 && rc != -EEXIST) {
		LINE("STORE test: mkdir WAKES FAILED (%d)\n", rc);
		return;
	}

	/* A counter that survives power cycles and reflashes: read, +1, write, read back. */
	char txt[16];
	size_t n = 0;
	uint32_t boots = 0;
	if (read_file(BOOTS, txt, sizeof(txt) - 1u, &n) == 0) {
		for (size_t i = 0; i < n && txt[i] >= '0' && txt[i] <= '9'; i++) {
			boots = boots * 10u + (uint32_t)(txt[i] - '0');
		}
	}
	boots++;
	snprintk(txt, sizeof(txt), "%u\n", (unsigned)boots);
	rc = write_file(BOOTS, txt, strlen(txt));
	char back[16];
	if (rc == 0) {
		rc = read_file(BOOTS, back, sizeof(back) - 1u, &n);
	}
	const bool same = (rc == 0 && n == strlen(txt) && memcmp(back, txt, n) == 0);
	LINE("STORE test: ON entries on this card: %u (%s)\n", (unsigned)boots,
	     same ? "written, read back" : "FAILED");

	/* 512 KB of a pattern, in 16 KB chunks: write, read back, compare, delete. The
	 * pattern is generated outside the timed span so the figure is the transfer's. */
	struct fs_file_t f;
	fs_file_t_init(&f);
	uint32_t wms = 0;
	rc = fs_open(&f, TESTFILE, FS_O_CREATE | FS_O_WRITE);
	for (uint32_t c = 0; rc == 0 && c < TEST_KB * 1024u / CHUNK && !stop; c++) {
		for (uint32_t i = 0; i < CHUNK; i++) {
			bulk[i] = pattern(c * CHUNK + i, boots);
		}
		const uint32_t t0 = k_uptime_get_32();
		const ssize_t w = fs_write(&f, bulk, CHUNK);
		wms += k_uptime_get_32() - t0;
		rc = (w == (ssize_t)CHUNK) ? 0 : -EIO;
	}
	if (fs_close(&f) != 0 && rc == 0) {
		rc = -EIO;
	}
	if (rc != 0 || stop) {
		LINE("STORE test: write %u KB FAILED (%d)\n", (unsigned)TEST_KB, rc);
		return;
	}
	uint32_t rms = 0;
	uint32_t bad = 0;
	fs_file_t_init(&f);
	rc = fs_open(&f, TESTFILE, FS_O_READ);
	for (uint32_t c = 0; rc == 0 && c < TEST_KB * 1024u / CHUNK && !stop; c++) {
		const uint32_t t0 = k_uptime_get_32();
		const ssize_t r = fs_read(&f, bulk, CHUNK);
		rms += k_uptime_get_32() - t0;
		rc = (r == (ssize_t)CHUNK) ? 0 : -EIO;
		for (uint32_t i = 0; rc == 0 && i < CHUNK; i++) {
			bad += (bulk[i] != pattern(c * CHUNK + i, boots));
		}
	}
	(void)fs_close(&f);
	LINE("STORE test: %u KB written in %u ms (%u KB/s), read back in %u ms (%u KB/s):"
	     " %s\n", (unsigned)TEST_KB, (unsigned)wms,
	     wms ? (unsigned)(TEST_KB * 1000u / wms) : 0u, (unsigned)rms,
	     rms ? (unsigned)(TEST_KB * 1000u / rms) : 0u,
	     rc != 0 ? "read FAILED" : (bad ? "MISMATCH" : "identical"));
	if (bad) {
		LINE("STORE test: %u bytes differ\n", (unsigned)bad);
	}
	rc = fs_unlink(TESTFILE);
	LINE("STORE test: TEST.BIN %s\n", rc == 0 ? "deleted" : "delete FAILED");

	struct fs_statvfs sv;
	if (fs_statvfs(MNT, &sv) == 0) {
		const uint64_t unit = (uint64_t)sv.f_frsize;
		LINE("STORE test: volume %u MB, free %u MB, %u-byte clusters\n",
		     (unsigned)((sv.f_blocks * unit) >> 20), (unsigned)((sv.f_bfree * unit) >> 20),
		     (unsigned)unit);
	}
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	if (fs_opendir(&dir, DIR_WAKES) == 0) {
		struct fs_dirent ent;
		while (fs_readdir(&dir, &ent) == 0 && ent.name[0] != '\0') {
			LINE("STORE test:   WAKES/%s  %u B\n", ent.name, (unsigned)ent.size);
		}
		(void)fs_closedir(&dir);
	}
}

#endif /* CONFIG_SP1_STORAGE_TEST */

/* ---------------------------------------------------------------- the job */

static void job_on_enter(void)
{
	const uint32_t t0 = k_uptime_get_32();
	struct sp1_emmc_ident id;
	const bool ok = sp1_emmc_init(&id);
	view_base = 0;
	view_count = sp1_emmc_sectors();
#if defined(CONFIG_SP1_STORAGE_TEST)
	sp1_emmc_report_init(ok, &id);
#endif
#if defined(CONFIG_SP1_FRESH)
	if (ok && !stop) {
		fresh();
	}
#endif
	/* Past the fresh step, whatever happened: main's storage_gate() lets the UI go.
	 * The report and the file test below run with the UI live. */
	atomic_set(&phase, SP1_STORE_READY);
	if (ok && !stop) {
#if defined(CONFIG_SP1_STORAGE_TEST)
		if (!stop) {
			sp1_emmc_report_card();
		}
#endif
		const int rc = stop ? -ECANCELED : mount_vol();
		if (rc != 0 && !stop) {
			LINE("STORE no Wakes filesystem on the eMMC (%d) -- nothing written. "
			     "wakes-sp1-fresh creates one.\n", rc);
		}
#if defined(CONFIG_SP1_STORAGE_TEST)
		if (rc == 0 && !stop) {
			file_test();
		}
		sp1_emmc_report_stats();
#endif
	}
	unmount_vol();
	if (!stop) {
		sp1_emmc_power_down();
	}
	LINE("STORE %s after %u ms -- eMMC powered down\n",
	     stop ? "STOPPED by power-off" : (ok ? "done" : "eMMC not usable"),
	     (unsigned)(k_uptime_get_32() - t0));
}

/* ---------------------------------------------------------------- the thread */

static K_SEM_DEFINE(go, 0, 1);
static K_THREAD_STACK_DEFINE(store_stack, 4096);
static struct k_thread store_tcb;

static void store_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	for (;;) {
		k_sem_take(&go, K_FOREVER);
		job_on_enter();
#if defined(CONFIG_SP1_STORAGE_TEST)
		size_t unused = 0;
		if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
			LINE("STORE stack: %u of %u bytes never used\n", (unsigned)unused,
			     (unsigned)K_THREAD_STACK_SIZEOF(store_stack));
		}
#endif
		atomic_clear(&busy);
	}
}

void sp1_store_init(void)
{
	(void)disk_access_register(&disk);
	(void)k_thread_create(&store_tcb, store_stack, K_THREAD_STACK_SIZEOF(store_stack),
			      store_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	(void)k_thread_name_set(&store_tcb, "store");
}

void sp1_store_on_enter(void)
{
	if (!atomic_cas(&busy, 0, 1)) {
		printk("STORE the previous job is still running -- skipped\n");
		return;
	}
	stop = false;
	atomic_set(&fmt_result, SP1_STORE_FMT_NONE);
	atomic_set(&progress, 0);
	/* Set HERE, before the job can run, so main never reads a stale READY. */
	atomic_set(&phase, IS_ENABLED(CONFIG_SP1_FRESH) ? SP1_STORE_CHECKING : SP1_STORE_READY);
	k_sem_give(&go);
}

enum sp1_store_phase sp1_store_phase(void)
{
	return (enum sp1_store_phase)atomic_get(&phase);
}

enum sp1_store_format sp1_store_format_result(void)
{
	return (enum sp1_store_format)atomic_get(&fmt_result);
}

uint8_t sp1_store_progress(void)
{
	return (uint8_t)atomic_get(&progress);
}

void sp1_store_abort(void)
{
	stop = true;
	sp1_emmc_abort();
}
