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

/* ---- who owns the card (drive mode, M6) ----
 * Two disks over one card: "EMMC" is Wakes' (FatFs, this thread) and "DRIVE" the host's
 * (the mass-storage class, its own thread). Exactly one may move data at a time, decided by
 * `owner` and enforced by `card_lock`, which every transfer on either disk holds -- so a
 * change of owner, made under the lock, waits for the transfer in flight and no other. */
enum owner { OWN_NONE = 0, OWN_WAKES, OWN_HOST };
static atomic_t owner = ATOMIC_INIT(OWN_NONE);
static K_MUTEX_DEFINE(card_lock);
static atomic_t activity;          /* host transfers, for STANDBY's LEDs */
static atomic_t vol_state = ATOMIC_INIT(SP1_STORE_VOL_UNKNOWN);

/* Where the volume's regions start, from the last mount (FATFS volbase/fatbase/database).
 * For the host-activity diagnostics below. */
static uint32_t r_vol, r_fat, r_data;

#if defined(CONFIG_SP1_DRIVE)
/* ---- what the host did with the card, per STANDBY visit (diagnostics, M6) ----
 * Hardware 2026-10-07 (logs/sp1-20261007-202944.log): after the host had the drive, two
 * Wakes test files read back with one 512-byte block of foreign data and no driver error.
 * Suspect: the host kept its cached FAT / directories across "no medium" and wrote them
 * back over Wakes' changes. These counters say whether it re-read the volume's metadata
 * after getting the card back, and what it wrote; printed when ON takes the card back. */
static struct {
	bool     any;
	bool     first_write;              /* the first command was a write            */
	uint32_t first_lba;
	uint32_t rd[4], wr[4];             /* blocks: before volume, boot area, FAT, data */
	uint32_t vbr_reads;                /* reads covering the volume's boot sector   */
} hs;

static unsigned region(uint32_t lba)
{
	return lba < r_vol ? 0u : (lba < r_fat ? 1u : (lba < r_data ? 2u : 3u));
}

static void host_note(bool write, uint32_t lba, uint32_t n)
{
	if (!hs.any) {
		hs.any = true;
		hs.first_write = write;
		hs.first_lba = lba;
	}
	if (write) {
		hs.wr[region(lba)] += n;
	} else {
		hs.rd[region(lba)] += n;
		if (lba <= r_vol && r_vol < lba + n) {
			hs.vbr_reads++;
		}
	}
}
#endif /* CONFIG_SP1_DRIVE */

/* Jobs waiting for the thread (a bit each), so a request made while another job runs is
 * queued rather than dropped: turning ON must always take the card back from a host. */
#define JOB_ON   BIT(0)
#define JOB_HOST BIT(1)
static atomic_t pending;

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
	if (start >= view_count || n > view_count - start) {
		return -EIO;
	}
	k_mutex_lock(&card_lock, K_FOREVER);
	const bool ok = atomic_get(&owner) == OWN_WAKES &&
			sp1_emmc_read_blocks(view_base + start, buf, n);
	k_mutex_unlock(&card_lock);
	return ok ? 0 : -EIO;
}

static int d_write(struct disk_info *d, const uint8_t *buf, uint32_t start, uint32_t n)
{
	ARG_UNUSED(d);
	if (start >= view_count || n > view_count - start) {
		return -EIO;
	}
	k_mutex_lock(&card_lock, K_FOREVER);
	bool ok = atomic_get(&owner) == OWN_WAKES;
	if (ok && counting) {                    /* a format: block by block, for the bar */
		for (uint32_t i = 0; ok && i < n; i++) {
			ok = wr(view_base + start + i, buf + i * SP1_EMMC_BLOCK);
		}
	} else if (ok) {
		ok = sp1_emmc_write_blocks(view_base + start, buf, n);
	}
	k_mutex_unlock(&card_lock);
	return ok ? 0 : -EIO;
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

#if defined(CONFIG_SP1_DRIVE)
/* ---------------------------------------------------------------- the host's disk */

#include <zephyr/usb/class/usbd_msc.h>

/* The whole card, block 0 (the MBR) included, so the host sees the partition table and
 * mounts the FAT32 volume itself. "No medium" whenever the host does not own it -- which
 * is how the drive looks while ON (Adara: the interface stays, the card goes). */
#define HOST_DISK "DRIVE"

static int h_init(struct disk_info *d)
{
	ARG_UNUSED(d);
	return 0;                                /* the HOST job powers the card */
}

static int h_status(struct disk_info *d)
{
	ARG_UNUSED(d);
	return (atomic_get(&owner) == OWN_HOST && sp1_emmc_ready()) ? DISK_STATUS_OK
								      : DISK_STATUS_NOMEDIA;
}

static int h_read(struct disk_info *d, uint8_t *buf, uint32_t start, uint32_t n)
{
	ARG_UNUSED(d);
	k_mutex_lock(&card_lock, K_FOREVER);
	const bool ok = atomic_get(&owner) == OWN_HOST && sp1_emmc_read_blocks(start, buf, n);
	if (ok) {
		host_note(false, start, n);
	}
	k_mutex_unlock(&card_lock);
	atomic_inc(&activity);
	return ok ? 0 : -EIO;
}

static int h_write(struct disk_info *d, const uint8_t *buf, uint32_t start, uint32_t n)
{
	ARG_UNUSED(d);
	k_mutex_lock(&card_lock, K_FOREVER);
	const bool ok = atomic_get(&owner) == OWN_HOST && sp1_emmc_write_blocks(start, buf, n);
	if (ok) {
		host_note(true, start, n);
	}
	k_mutex_unlock(&card_lock);
	atomic_inc(&activity);
	return ok ? 0 : -EIO;
}

static int h_ioctl(struct disk_info *d, uint8_t cmd, void *buf)
{
	ARG_UNUSED(d);
	switch (cmd) {
	case DISK_IOCTL_GET_SECTOR_COUNT:
		*(uint32_t *)buf = sp1_emmc_sectors();
		return 0;
	case DISK_IOCTL_GET_SECTOR_SIZE:
		*(uint32_t *)buf = SP1_EMMC_BLOCK;
		return 0;
	case DISK_IOCTL_GET_ERASE_BLOCK_SZ:
		*(uint32_t *)buf = PART_START;
		return 0;
	case DISK_IOCTL_CTRL_SYNC:               /* the card's write cache is off */
	case DISK_IOCTL_CTRL_INIT:
	case DISK_IOCTL_CTRL_DEINIT:
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct disk_operations host_ops = {
	.init = h_init, .status = h_status, .read = h_read, .write = h_write,
	.ioctl = h_ioctl,
};

static struct disk_info host_disk = { .name = HOST_DISK, .ops = &host_ops };

/* T10 identity: vendor 8 characters, product 16, revision 4. */
USBD_DEFINE_MSC_LUN(sp1_drive, HOST_DISK, "Wakes", "SP-1 storage", "1.0");
#endif /* CONFIG_SP1_DRIVE */

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
		if (mounted) {
			r_vol = (uint32_t)fat.volbase;
			r_fat = (uint32_t)fat.fatbase;
			r_data = (uint32_t)fat.database;
		}
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
			/* The volume label: what a computer calls the drive instead of
			 * "Removable Disk" (Adara). FAT labels are upper case; '-' is legal. */
			const FRESULT fr = f_setlabel(DISK_NAME ":SP-1");
			rc = (fr == FR_OK) ? 0 : -EIO;
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
	unmount_vol();       /* the ON job mounts again and decides the volume state */
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

/* ---- the read-junk reproduction (M6 diagnostics) ----
 * Hardware 2026-10-07: after the host scanned the whole FAT (944 blocks in 16-block reads,
 * each stopped with CMD12), the second block of one of Wakes' 32-block CMD18 reads came
 * back as card-buffer junk with a valid CRC, while every write had verified. This repeats
 * the host's pattern on the device: scan FAT 1 in 16-block bursts, then read TEST.BIN in
 * 16 KB chunks against its pattern -- three rounds with open-ended bursts (CMD12, as so
 * far) and three closed-ended (CMD23). Bad blocks per way, where in the chunk, and what
 * the first one held. */
static uint32_t stress_round(uint32_t boots, uint32_t *pos_hist, uint8_t first[16],
			     bool *have_first)
{
	uint32_t bad = 0;
	k_mutex_lock(&card_lock, K_FOREVER);
	for (uint32_t lba = r_fat; lba + 16u <= r_fat + fat.fsize && !stop; lba += 16u) {
		(void)sp1_emmc_read_blocks(lba, bulk, 16u);
	}
	k_mutex_unlock(&card_lock);
	struct fs_file_t f;
	fs_file_t_init(&f);
	int rc = fs_open(&f, TESTFILE, FS_O_READ);
	for (uint32_t c = 0; rc == 0 && c < TEST_KB * 1024u / CHUNK && !stop; c++) {
		const ssize_t r = fs_read(&f, bulk, CHUNK);
		rc = (r == (ssize_t)CHUNK) ? 0 : -EIO;
		for (uint32_t b = 0; rc == 0 && b < CHUNK / SP1_EMMC_BLOCK; b++) {
			bool wrong = false;
			for (uint32_t i = 0; i < SP1_EMMC_BLOCK && !wrong; i++) {
				const uint32_t at = b * SP1_EMMC_BLOCK + i;
				wrong = (bulk[at] != pattern(c * CHUNK + at, boots));
			}
			if (wrong) {
				bad++;
				pos_hist[b]++;
				if (!*have_first) {
					*have_first = true;
					memcpy(first, &bulk[b * SP1_EMMC_BLOCK], 16);
				}
			}
		}
	}
	(void)fs_close(&f);
	return bad;
}

static void stress(uint32_t boots)
{
	static const char *const way[2] = { "open-ended (CMD12)", "closed-ended (CMD23)" };
	for (int w = 0; w < 2 && !stop; w++) {
		sp1_emmc_set_closed_ended(w == 1);
		uint32_t pos_hist[CHUNK / SP1_EMMC_BLOCK] = { 0 };
		uint8_t first[16] = { 0 };
		bool have_first = false;
		uint32_t bad = 0;
		const uint32_t t0 = k_uptime_get_32();
		for (int round = 0; round < 3 && !stop; round++) {
			bad += stress_round(boots, pos_hist, first, &have_first);
		}
		LINE("STORE stress %s: 3 rounds (FAT scan + 512 KB read) in %u ms, %u bad block(s)\n",
		     way[w], (unsigned)(k_uptime_get_32() - t0), (unsigned)bad);
		if (bad) {
			char hist[CHUNK / SP1_EMMC_BLOCK * 9 + 1];
			size_t o = 0;
			for (uint32_t b = 0; b < CHUNK / SP1_EMMC_BLOCK; b++) {
				if (pos_hist[b] != 0u && o + 10u < sizeof(hist)) {
					o += (size_t)snprintk(&hist[o], sizeof(hist) - o, " %u:%u",
							      (unsigned)b, (unsigned)pos_hist[b]);
				}
			}
			char hex[16 * 3 + 1];
			for (int i = 0; i < 16; i++) {
				static const char hx[] = "0123456789abcdef";
				hex[3 * i] = ' ';
				hex[3 * i + 1] = hx[first[i] >> 4];
				hex[3 * i + 2] = hx[first[i] & 15u];
			}
			hex[48] = '\0';
			LINE("STORE stress %s: bad by position in the chunk (pos:count)%s; first held%s\n",
			     way[w], hist, hex);
		}
	}
	sp1_emmc_set_closed_ended(true);     /* back to the default: the fix */
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
	uint32_t bad_blocks = 0;
	int32_t first_bad = -1;              /* block index within the file */
	uint8_t seen[32] = { 0 };            /* what the first bad block held */
	uint32_t sclust = 0;
	fs_file_t_init(&f);
	rc = fs_open(&f, TESTFILE, FS_O_READ);
	if (rc == 0) {
		sclust = (uint32_t)((FIL *)f.filep)->obj.sclust;
	}
	for (uint32_t c = 0; rc == 0 && c < TEST_KB * 1024u / CHUNK && !stop; c++) {
		const uint32_t t0 = k_uptime_get_32();
		const ssize_t r = fs_read(&f, bulk, CHUNK);
		rms += k_uptime_get_32() - t0;
		rc = (r == (ssize_t)CHUNK) ? 0 : -EIO;
		for (uint32_t b = 0; rc == 0 && b < CHUNK / SP1_EMMC_BLOCK; b++) {
			uint32_t diff = 0;
			for (uint32_t i = 0; i < SP1_EMMC_BLOCK; i++) {
				const uint32_t at = b * SP1_EMMC_BLOCK + i;
				diff += (bulk[at] != pattern(c * CHUNK + at, boots));
			}
			if (diff != 0u) {
				bad += diff;
				bad_blocks++;
				if (first_bad < 0) {
					first_bad = (int32_t)(c * (CHUNK / SP1_EMMC_BLOCK) + b);
					memcpy(seen, &bulk[b * SP1_EMMC_BLOCK], sizeof(seen));
				}
			}
		}
	}
	(void)fs_close(&f);
	LINE("STORE test: %u KB written in %u ms (%u KB/s), read back in %u ms (%u KB/s):"
	     " %s\n", (unsigned)TEST_KB, (unsigned)wms,
	     wms ? (unsigned)(TEST_KB * 1000u / wms) : 0u, (unsigned)rms,
	     rms ? (unsigned)(TEST_KB * 1000u / rms) : 0u,
	     rc != 0 ? "read FAILED" : (bad ? "MISMATCH" : "identical"));
	if (bad) {
		/* Where it went wrong and what was there instead: a directory sector reads as
		 * 8.3 names, a FAT sector as little-endian cluster numbers, another file's
		 * data as anything else. The file's clusters are 64 blocks each; if they are
		 * contiguous, block k is at LBA data + (sclust - 2) * 64 + k. */
		LINE("STORE test: %u bytes differ in %u block(s); first bad block %d of the file, "
		     "file starts at cluster %u (LBA %u if contiguous: %u)\n", (unsigned)bad,
		     (unsigned)bad_blocks, (int)first_bad, (unsigned)sclust,
		     (unsigned)(r_data + (sclust - 2u) * 64u),
		     (unsigned)(r_data + (sclust - 2u) * 64u + (uint32_t)first_bad));
		char hex[sizeof(seen) * 3 + 1];
		for (size_t i = 0; i < sizeof(seen); i++) {
			static const char hx[] = "0123456789abcdef";
			hex[3 * i] = ' ';
			hex[3 * i + 1] = hx[seen[i] >> 4];
			hex[3 * i + 2] = hx[seen[i] & 15u];
		}
		hex[sizeof(hex) - 1] = '\0';
		LINE("STORE test: it held:%s\n", hex);
	}
	stress(boots);
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

/* ---------------------------------------------------------------- the jobs */

/* Take the card for Wakes. Under the lock, so a host transfer in flight finishes first
 * (Adara: "after any write already in progress", bounded by the driver's own timeouts) and
 * no new one starts: from here the host's disk reads "no medium". */
static void claim_for_wakes(void)
{
	const bool was_host = atomic_get(&owner) == OWN_HOST;
	atomic_set(&owner, OWN_NONE);
	k_mutex_lock(&card_lock, K_FOREVER);
	atomic_set(&owner, OWN_WAKES);
	k_mutex_unlock(&card_lock);
#if defined(CONFIG_SP1_DRIVE)
	if (was_host) {
		LINE("STORE drive: the host lets go -- the card is Wakes' again\n");
		if (hs.any) {
			/* rd/wr = blocks before the volume (MBR) / boot area / FAT / data. */
			LINE("STORE drive: host first %s at LBA %u; VBR read %u x; "
			     "read %u/%u/%u/%u, wrote %u/%u/%u/%u (mbr/boot/fat/data)\n",
			     hs.first_write ? "WROTE" : "read", (unsigned)hs.first_lba,
			     (unsigned)hs.vbr_reads, (unsigned)hs.rd[0], (unsigned)hs.rd[1],
			     (unsigned)hs.rd[2], (unsigned)hs.rd[3], (unsigned)hs.wr[0],
			     (unsigned)hs.wr[1], (unsigned)hs.wr[2], (unsigned)hs.wr[3]);
		} else {
			LINE("STORE drive: the host did not touch the card\n");
		}
	}
#else
	ARG_UNUSED(was_host);
#endif
}

static void job_on_enter(void)
{
	const uint32_t t0 = k_uptime_get_32();
	claim_for_wakes();
	struct sp1_emmc_ident id;
	const bool ok = sp1_emmc_init(&id);      /* a fresh start whoever had the card */
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
	if (!ok && !stop) {
		atomic_set(&vol_state, SP1_STORE_VOL_NONE);
		LINE("STORE PRST off: the eMMC did not start\n");
	}
	if (ok && !stop) {
#if defined(CONFIG_SP1_STORAGE_TEST)
		if (!stop) {
			sp1_emmc_report_card();
		}
#endif
		/* Is it formatted? (Adara: if not, boot, but PRST and its combos are off;
		 * only wakes-sp1-fresh formats.) */
		const int rc = stop ? -ECANCELED : mount_vol();
		if (!stop) {
			atomic_set(&vol_state, rc == 0 ? SP1_STORE_VOL_OK : SP1_STORE_VOL_NONE);
			if (rc == 0) {
				LINE("STORE volume ok: PRST on\n");
			} else {
				LINE("STORE PRST off: no filesystem on the eMMC (%d) -- nothing "
				     "written; wakes-sp1-fresh creates one\n", rc);
			}
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
	atomic_set(&owner, OWN_NONE);
	LINE("STORE %s after %u ms -- eMMC powered down\n",
	     stop ? "STOPPED by power-off" : (ok ? "done" : "eMMC not usable"),
	     (unsigned)(k_uptime_get_32() - t0));
}

#if defined(CONFIG_SP1_DRIVE)
/* STANDBY with a host attached: power the card and, if it holds a filesystem, hand it to
 * the host until ON (claim_for_wakes) or power-off (sp1_store_abort). A card WITHOUT one
 * is not offered: a computer shown a raw card offers to format it (possibly as exFAT,
 * which Wakes cannot read), and only wakes-sp1-fresh formats (Adara). */
static void job_host(void)
{
	k_mutex_lock(&card_lock, K_FOREVER);
	struct sp1_emmc_ident id;
	const bool ok = !stop && sp1_emmc_init(&id);
	bool vol = false;
	if (ok) {
		/* Mount as Wakes for a moment: the same test the ON job uses, and it
		 * records where the volume's regions are for the diagnostics. */
		atomic_set(&owner, OWN_WAKES);
		view_base = 0;
		view_count = sp1_emmc_sectors();
		vol = (mount_vol() == 0);
		unmount_vol();
		atomic_set(&vol_state, vol ? SP1_STORE_VOL_OK : SP1_STORE_VOL_NONE);
	}
	memset(&hs, 0, sizeof(hs));
	if (vol) {
		atomic_set(&owner, OWN_HOST);
	} else {
		atomic_set(&owner, OWN_NONE);
		sp1_emmc_power_down();
	}
	k_mutex_unlock(&card_lock);
	if (vol) {
		LINE("STORE drive: the card is a USB drive (%u MB) until ON\n",
		     (unsigned)(sp1_emmc_sectors() / 2048u));
	} else if (ok) {
		LINE("STORE drive: no filesystem on the eMMC -- no drive (wakes-sp1-fresh "
		     "creates one)\n");
	} else {
		LINE("STORE drive: the eMMC did not start -- no drive this time\n");
	}
}
#endif

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
		for (;;) {
			const atomic_val_t jobs = atomic_get(&pending);
			if (jobs & JOB_ON) {             /* ON always wins: Wakes takes the card */
				atomic_and(&pending, ~JOB_ON);
				job_on_enter();
			} else if (jobs & JOB_HOST) {
				atomic_and(&pending, ~JOB_HOST);
#if defined(CONFIG_SP1_DRIVE)
				job_host();
#endif
			} else {
				break;
			}
#if defined(CONFIG_SP1_STORAGE_TEST)
			size_t unused = 0;
			if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
				LINE("STORE stack: %u of %u bytes never used\n",
				     (unsigned)unused,
				     (unsigned)K_THREAD_STACK_SIZEOF(store_stack));
			}
#endif
		}
	}
}

void sp1_store_init(void)
{
	(void)disk_access_register(&disk);
#if defined(CONFIG_SP1_DRIVE)
	(void)disk_access_register(&host_disk);
#endif
	(void)k_thread_create(&store_tcb, store_stack, K_THREAD_STACK_SIZEOF(store_stack),
			      store_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
	(void)k_thread_name_set(&store_tcb, "store");
}

void sp1_store_on_enter(void)
{
	stop = false;
	atomic_set(&fmt_result, SP1_STORE_FMT_NONE);
	atomic_set(&progress, 0);
	atomic_set(&vol_state, SP1_STORE_VOL_UNKNOWN);   /* PRST waits for the ON job's look */
	/* Set HERE, before the job can run, so main never reads a stale READY. */
	atomic_set(&phase, IS_ENABLED(CONFIG_SP1_FRESH) ? SP1_STORE_CHECKING : SP1_STORE_READY);
	atomic_and(&pending, ~JOB_HOST);
	atomic_or(&pending, JOB_ON);
	k_sem_give(&go);
}

#if defined(CONFIG_SP1_DRIVE)
static bool host_requested;              /* this STANDBY visit */

void sp1_store_standby_enter(void)
{
	host_requested = false;
}

void sp1_store_standby_tick(bool host)
{
	/* A host (not a charger) has configured the device: give it the card, once per
	 * STANDBY visit. */
	if (host && !host_requested) {
		host_requested = true;
		stop = false;
		atomic_or(&pending, JOB_HOST);
		k_sem_give(&go);
	}
}

uint32_t sp1_store_activity(void)
{
	return (uint32_t)atomic_get(&activity);
}
#endif

enum sp1_store_volume sp1_store_volume(void)
{
	return (enum sp1_store_volume)atomic_get(&vol_state);
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
	atomic_clear(&pending);
	atomic_set(&owner, OWN_NONE);
	sp1_emmc_abort();
}
