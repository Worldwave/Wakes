/*
 * wakes-sp1 — storage on the eMMC (M6, #43). See sp1_store.h.
 *
 * FatFs (ChaN, R0.16, through Zephyr's fs layer) sits on a disk driver here that reads
 * and writes single blocks through sp1_emmc.c. Every console line starts "STORE" (this
 * file), "CNFG" (the config files), "LOG" (the log on the drive) or "EMMC"
 * (sp1_emmc_report.c).
 */
#include "sp1_store.h"
#include "sp1_emmc.h"
#include "sp1_console.h"
#include "sp1_cnfg.h"
#include "sp1_logbuf.h"
#if defined(CONFIG_SP1_FRESH)
#define SP1_CNFG_TEMPLATES          /* the fresh image writes the config files */
#include "sp1_engines_gen.h"        /* ...engines.cnfg among them */
#endif
#include "sp1_logs_gen.h"
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
#define DIR_CNFG   DIR_WAKES "/CNFG"
#define DIR_LOGS   DIR_WAKES "/LOGS"

/* The partition starts one erase group in: 8192 blocks = 4 MB (EXT_CSD
 * HC_ERASE_GRP_SIZE 8 x 512 KB on this card, logs/sp1-20261007-152119.log). */
#define PART_START 8192u

#define LINE(...) do { printk(__VA_ARGS__); sp1_console_pace(); } while (0)

static volatile bool stop;
/* Set by sp1_store_abort and cleared only by the thread, once it has let go of the log file
 * and the volume (log_teardown): sp1_store_on_enter() clears `stop`, so it cannot say this. */
static volatile bool torn;

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

#if defined(CONFIG_SP1_STORAGE_TEST)
/* Where the data region starts, from the last mount (FATFS database): the file test's
 * mismatch report turns a cluster into an LBA with it. */
static uint32_t r_data;
#endif

/* Jobs waiting for the thread (a bit each), so a request made while another job runs is
 * queued rather than dropped: turning ON must always take the card back from a host. */
#define JOB_ON   BIT(0)
#define JOB_HOST BIT(1)
#define JOB_SAVE BIT(2)
#define JOB_LOGEND BIT(3)
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
	k_mutex_unlock(&card_lock);
	atomic_inc(&activity);
	return ok ? 0 : -EIO;
}

static int h_write(struct disk_info *d, const uint8_t *buf, uint32_t start, uint32_t n)
{
	ARG_UNUSED(d);
	k_mutex_lock(&card_lock, K_FOREVER);
	const bool ok = atomic_get(&owner) == OWN_HOST && sp1_emmc_write_blocks(start, buf, n);
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
#if defined(CONFIG_SP1_STORAGE_TEST)
		if (mounted) {
			r_data = (uint32_t)fat.database;
		}
#endif
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

/* Used by the fresh stamp and the storage test now; by PRST (#43) next. */
__maybe_unused static int write_file(const char *path, const void *d, size_t n)
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

__maybe_unused static int read_file(const char *path, void *d, size_t max, size_t *got)
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

/* Verify `path` is a FOLDER; create it if nothing is there (Adara, #50; the log's and the
 * config files' folders too since #52). 0 when there is a folder to write into. Something
 * else with that name -- a computer can leave a FILE called "3" where slot 3's folder
 * should be -- is -ENOTDIR and is left alone: the write fails with that in the log rather
 * than touching a file it did not write. */
static int ensure_dir(const char *path)
{
	static struct fs_dirent st;
	int rc = fs_stat(path, &st);
	if (rc == 0) {
		if (st.type == FS_DIR_ENTRY_DIR) {
			return 0;
		}
		LINE("STORE %s is a file, not a folder -- not writing there\n", path);
		return -ENOTDIR;
	}
	if (rc != -ENOENT) {
		return rc;
	}
	rc = fs_mkdir(path);
	LINE("STORE no folder %s -- %s (%d)\n", path, rc == 0 ? "created" : "create FAILED", rc);
	return rc;
}

/* ---------------------------------------------------------------- CNFG (#52) */

#define LOGS_CNFG     DIR_CNFG "/logs.cnfg"
#define ENGINES_CNFG  DIR_CNFG "/engines.cnfg"
#define CNFG_READ_MAX 16384u          /* a config file is a few KB; never read a huge one */

static struct sp1_logs_cfg logs_cfg;

static void cnfg_problem(void *ctx, int line, const char *what, const char *why)
{
	LINE("CNFG %s line %d: %s -- %s\n", (const char *)ctx, line, what, why);
}

/* A file through a reader, CNFG_READ_MAX at most. 0 = read (*got bytes, *rerr if a read
 * failed part-way); -ENOENT = not on the drive; anything else = could not be opened. */
static int cnfg_read(const char *path, void (*feed)(void *r, const char *t, size_t n), void *r,
		     size_t *got, bool *rerr)
{
	static char chunk[128];
	*got = 0u;
	*rerr = false;
	struct fs_file_t f;
	fs_file_t_init(&f);
	const int rc = fs_open(&f, path, FS_O_READ);
	if (rc != 0) {
		return rc;
	}
	while (!stop && *got < CNFG_READ_MAX) {
		const ssize_t n = fs_read(&f, chunk, sizeof(chunk));
		if (n <= 0) {
			*rerr = n < 0;
			break;
		}
		feed(r, chunk, (size_t)n);
		*got += (size_t)n;
	}
	(void)fs_close(&f);
	return 0;
}

static void logs_feed(void *r, const char *t, size_t n)
{
	sp1_logs_feed(r, t, n);
}

/* logs.cnfg: line by line over the built-in defaults -- a bad line keeps its default. */
static void cnfg_load_logs(void)
{
	static struct sp1_logs_reader rd;      /* ~0.4 KB: off the thread's stack */
	sp1_logs_defaults(&logs_cfg);
	sp1_logs_begin(&rd, &logs_cfg, cnfg_problem, (void *)"logs.cnfg");
	size_t got;
	bool rerr;
	const int rc = cnfg_read(LOGS_CNFG, logs_feed, &rd, &got, &rerr);
	if (rc == -ENOENT) {
		LINE("CNFG logs.cnfg: not on the drive -- the built-in defaults\n");
	} else if (rc != 0) {
		LINE("CNFG logs.cnfg: cannot be opened (%d) -- the built-in defaults\n", rc);
	} else {
		const int bad = sp1_logs_end(&rd);
		LINE("CNFG logs.cnfg: %u B read%s, %d problem%s%s\n", (unsigned)got,
		     rerr ? " (then a read error)" : (got >= CNFG_READ_MAX ? " (the rest ignored)" : ""),
		     bad, bad == 1 ? "" : "s", bad ? " -- those lines keep their defaults" : "");
	}
	char d[48];
	LINE("CNFG logging to the drive %s: %s\n", logs_cfg.enabled ? "on" : "off",
	     sp1_logs_describe(logs_cfg.cats, d, sizeof(d)));
}

#if defined(CONFIG_SP1_PLAITS)
static void engines_feed(void *r, const char *t, size_t n)
{
	sp1_engines_feed(r, t, n);
}

static int stack_count(const uint8_t s[SP1_CNFG_SLOTS])
{
	int n = 0;
	for (int i = 0; i < SP1_CNFG_SLOTS; i++) {
		n += (s[i] != SP1_CNFG_EMPTY);
	}
	return n;
}

/* engines.cnfg: ALL OR NOTHING (sp1_cnfg.h). Missing, unreadable, or any problem in it ->
 * the built-in stack, every problem named. Installed before PRST reads its slots, so their
 * engine names resolve against it; ROTC and the start go to its first filled slot. */
static void cnfg_load_engines(void)
{
	static struct sp1_engines_reader rd;   /* ~0.4 KB: off the thread's stack */
	uint8_t stack[SP1_CNFG_SLOTS];
	int filled = 0;
	sp1_engines_begin(&rd, cnfg_problem, (void *)"engines.cnfg");
	size_t got;
	bool rerr;
	const int rc = cnfg_read(ENGINES_CNFG, engines_feed, &rd, &got, &rerr);
	if (rc != 0) {
		sp1_engines_defaults(stack);
		filled = stack_count(stack);
		if (rc == -ENOENT) {
			LINE("CNFG engines.cnfg: not on the drive -- the built-in stack\n");
		} else {
			LINE("CNFG engines.cnfg: cannot be opened (%d) -- the built-in stack\n", rc);
		}
	} else {
		int bad = sp1_engines_end(&rd, stack, &filled);
		if (bad == 0 && (rerr || got >= CNFG_READ_MAX)) {
			/* Only part of the file was read: part of a stack is not the user's stack. */
			bad = 1;
			sp1_engines_defaults(stack);
			filled = stack_count(stack);
		}
		LINE("CNFG engines.cnfg: %u B read%s, %d problem%s -- %s\n", (unsigned)got,
		     rerr ? " (then a read error)" : (got >= CNFG_READ_MAX ? " (too long)" : ""),
		     bad, bad == 1 ? "" : "s",
		     bad ? "the built-in stack instead (the whole file is set aside)"
			 : "your stack");
	}
	sp1_pui_set_stack(stack);
	int first = 0;
	while (first < SP1_CNFG_SLOTS - 1 && stack[first] == SP1_CNFG_EMPTY) {
		first++;
	}
	LINE("CNFG engine stack: %d engine%s, starting at slot %d\n", filled,
	     filled == 1 ? "" : "s", first + 1);
}
#endif

/* No volume: the built-in everything, so nothing from an earlier ON's files lingers. */
static void cnfg_defaults(void)
{
	sp1_logs_defaults(&logs_cfg);
#if defined(CONFIG_SP1_PLAITS)
	uint8_t stack[SP1_CNFG_SLOTS];
	sp1_engines_defaults(stack);
	sp1_pui_set_stack(stack);
#endif
}

/* Inside the ON job, the volume mounted: every config file, over the built-in defaults.
 * Never fails: a missing or unreadable file keeps the defaults. */
static void cnfg_load(void)
{
	cnfg_load_logs();
#if defined(CONFIG_SP1_PLAITS)
	if (!stop) {
		cnfg_load_engines();
	}
#endif
}

#if defined(CONFIG_SP1_FRESH)
/* wakes-sp1-fresh, after its format (Adara, #52): the config files, commented, in
 * WAKES/CNFG/. config/<name>.cnfg in the repository, byte for byte. */
static int cnfg_write_one(const char *path, const char *name, const char *text, size_t n)
{
	const int rc = write_file(path, text, n);
	LINE("STORE fresh: WAKES/CNFG/%s (%u B): %s (%d)\n", name, (unsigned)n,
	     rc == 0 ? "written" : "FAILED", rc);
	return rc;
}

static int cnfg_write_templates(void)
{
	int rc = ensure_dir(DIR_CNFG);
	if (rc == 0 && !stop) {
		rc = cnfg_write_one(LOGS_CNFG, "logs.cnfg", SP1_LOGS_TEMPLATE,
				    sizeof(SP1_LOGS_TEMPLATE) - 1u);
	}
#if defined(CONFIG_SP1_PLAITS)
	if (rc == 0 && !stop) {
		rc = cnfg_write_one(ENGINES_CNFG, "engines.cnfg", SP1_ENGINES_TEMPLATE,
				    sizeof(SP1_ENGINES_TEMPLATE) - 1u);
	}
#endif
	return stop ? -ECANCELED : rc;
}
#endif

/* ---------------------------------------------------------------- the log (#52) */

/* One file per session, NNNN.LOGS (Adara): the session number mod 10000, so 9999.LOGS is
 * followed by 0000.LOGS. The number itself keeps counting, in WAKES/LOGS/SESSION and in each
 * file's first line. The last SP1_STORE_LOG_KEEP sessions are kept: a new one deletes the
 * file of the session SP1_STORE_LOG_KEEP before it. */
#define LOG_SESSION    DIR_LOGS "/SESSION"
#define LOG_NAMES      10000u
#define LOG_SUMMARY_MS 60000u

static atomic_t log_live_a;            /* main reads it: sp1_store_log_live()        */
static bool     log_live;              /* the file is open, the card on and mounted  */
static bool     session_open;          /* this boot has opened (or started) its file */
static uint32_t session_n;             /* ...and its session number                  */
static char     log_path[sizeof(DIR_LOGS) + 16];
static struct fs_file_t log_f;
static uint32_t log_size;
static bool     log_full;
static uint32_t log_next;              /* uptime of the next flush                   */
static bool     piece_cont;            /* the last piece taken ended mid-line...     */
static bool     piece_keep;            /* ...and was (not) for the file              */
static char     piece[SP1_LOGBUF_TAKE_MAX];
/* What goes to the card: the ring is never handed to the disk directly, both because
 * printk keeps writing it and because of nRF52840 anomaly 198 (SPIM3's DMA source). */
static char     stage[1024];
static uint32_t sum_t0, sum_flushes, sum_bytes, sum_max_ms, sum_lost;

static void log_name(uint32_t n, char *buf, size_t size)
{
	snprintk(buf, size, DIR_LOGS "/%04u.LOGS", (unsigned)(n % LOG_NAMES));
}

/* WAKES/LOGS/SESSION: the last session's number. false if it is missing or not a number. */
static bool log_read_session(uint32_t *n)
{
	char t[16];
	size_t got = 0u;
	if (read_file(LOG_SESSION, t, sizeof(t) - 1u, &got) != 0) {
		return false;
	}
	uint32_t v = 0u;
	size_t i = 0u;
	while (i < got && (t[i] == ' ' || t[i] == '\t')) {
		i++;
	}
	const size_t start = i;
	for (; i < got && t[i] >= '0' && t[i] <= '9' && v < 429496729u; i++) {
		v = v * 10u + (uint32_t)(t[i] - '0');
	}
	if (i == start) {
		return false;
	}
	*n = v;
	return true;
}

static int log_put(const char *d, size_t n)
{
	const ssize_t w = fs_write(&log_f, d, n);
	if (w != (ssize_t)n) {
		return w < 0 ? (int)w : -EIO;
	}
	log_size += (uint32_t)n;
	return 0;
}

/* Into the file through the size cap: past it, one last line and nothing more this session. */
static int log_write(const char *d, size_t n)
{
	if (log_full || n == 0u) {
		return 0;
	}
	static const char full[] = "=== LOG FULL at 8 MB: the rest of this session is not on the "
				   "drive (the console still has it) ===\n";
	if (log_size + n + sizeof(full) > SP1_STORE_LOG_MAX) {
		log_full = true;
		return log_put(full, sizeof(full) - 1u);
	}
	return log_put(d, n);
}

/* The volume mounted, at the end of the ON job: open this session's file. The first time this
 * boot, a NEW session -- the next number -- unless the boot carries on a log kept across a
 * soft reset (the same session: the crash and what led to it stay in one file). Later ONs of
 * the same boot carry on too. */
static int log_open(void)
{
	int rc = ensure_dir(DIR_WAKES);
	if (rc == 0) {
		rc = ensure_dir(DIR_LOGS);
	}
	if (rc != 0) {
		return rc;
	}
	uint32_t last = 0u;
	const bool known = log_read_session(&last);
	bool fresh_session = false;
	if (!session_open) {
		if (sp1_logbuf_continued() && known) {
			session_n = last;                /* a soft reset: the same session */
		} else {
			session_n = known ? last + 1u : 1u;
			fresh_session = true;
		}
	}
	log_name(session_n, log_path, sizeof(log_path));
	if (fresh_session) {
		char t[16];
		const int n = snprintk(t, sizeof(t), "%u\n", (unsigned)session_n);
		rc = write_file(LOG_SESSION, t, (size_t)n);
		if (rc != 0) {
			return rc;
		}
		/* A file already under this name is from a session 10000 back (or a lost
		 * counter): this session starts it afresh. */
		(void)fs_unlink(log_path);
		if (session_n > SP1_STORE_LOG_KEEP) {
			char old[sizeof(log_path)];
			log_name(session_n - SP1_STORE_LOG_KEEP, old, sizeof(old));
			const int urc = fs_unlink(old);
			if (urc != 0 && urc != -ENOENT) {
				LINE("LOG could not delete %s (%d)\n", old + sizeof(DIR_LOGS), urc);
			}
		}
	}
	fs_file_t_init(&log_f);
	rc = fs_open(&log_f, log_path, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (rc != 0) {
		return rc;
	}
	rc = fs_seek(&log_f, 0, FS_SEEK_END);
	const off_t at = (rc == 0) ? fs_tell(&log_f) : -1;
	if (at < 0) {
		(void)fs_close(&log_f);
		return rc != 0 ? rc : (int)at;
	}
	log_size = (uint32_t)at;
	log_full = log_size >= SP1_STORE_LOG_MAX;
	piece_cont = false;
	if (fresh_session || log_size == 0u) {
		char h[160];
		const int n = snprintk(h, sizeof(h), "=== Wakes v" APP_VERSION_STRING ", built %s %s -- "
				       "log session %u -- resetreas 0x%08x ===\n", __DATE__, __TIME__,
				       (unsigned)session_n, sp1_logbuf_resetreas());
		rc = log_write(h, (size_t)n);
		if (rc != 0) {
			(void)fs_close(&log_f);
			return rc;
		}
	}
	session_open = true;
	log_live = true;
	atomic_set(&log_live_a, 1);
	LINE("LOG %s %u: WAKES/LOGS/%s (%u KB)\n", fresh_session ? "new session" : "carrying on session",
	     (unsigned)session_n, log_path + sizeof(DIR_LOGS), (unsigned)(log_size / 1024u));
	return 0;
}

/* Lines a filtered category leaves out; a long line's later pieces follow its first. */
static bool keep_piece(const char *d, size_t n)
{
	bool keep;
	if (piece_cont) {
		keep = piece_keep;
	} else {
		const uint8_t c = sp1_log_category(d, n);
		keep = (c == 0u) || (logs_cfg.cats & c) != 0u;
	}
	piece_cont = d[n - 1u] != '\n';
	piece_keep = keep;
	return keep;
}

/* Everything new in the RAM log, into the file, then synced: the file's size on the card is
 * then current, so a cut-off loses only what came after. */
static int log_flush(void)
{
	const uint32_t t0 = k_uptime_get_32();
	size_t used = 0u;
	uint32_t written = 0u;
	int rc = 0;
	for (;;) {
		uint32_t lost = 0u;
		const size_t n = sp1_logbuf_file_take(piece, sizeof(piece), &lost);
		if (lost > 0u) {
			sum_lost += lost;
			char m[112];
			const int k = snprintk(m, sizeof(m), "=== LOG: %u bytes lost here -- the RAM log "
					       "filled before the drive took them ===\n",
					       (unsigned)lost);
			if (used + (size_t)k > sizeof(stage)) {
				rc = log_write(stage, used);
				written += (uint32_t)used;
				used = 0u;
			}
			memcpy(stage + used, m, (size_t)k);
			used += (size_t)k;
			piece_cont = false;
		}
		if (n == 0u || rc != 0 || stop) {
			break;
		}
		if (!keep_piece(piece, n)) {
			continue;
		}
		if (used + n > sizeof(stage)) {
			rc = log_write(stage, used);
			written += (uint32_t)used;
			used = 0u;
			if (rc != 0) {
				break;
			}
		}
		memcpy(stage + used, piece, n);
		used += n;
	}
	if (rc == 0 && used > 0u) {
		rc = log_write(stage, used);
		written += (uint32_t)used;
	}
	if (rc == 0 && written > 0u) {
		rc = fs_sync(&log_f);
	}
	const uint32_t ms = k_uptime_get_32() - t0;
	sum_flushes++;
	sum_bytes += written;
	if (ms > sum_max_ms) {
		sum_max_ms = ms;
	}
	return stop ? -ECANCELED : rc;
}

/* Close the file and let go of the card: unmount, power down, nobody owns it. */
static void log_finish(void)
{
	if (log_live) {
		(void)fs_close(&log_f);
		log_live = false;
	}
	unmount_vol();
	if (!stop) {
		sp1_emmc_power_down();
	}
	atomic_set(&owner, OWN_NONE);
	atomic_set(&log_live_a, 0);
}

/* After sp1_store_abort: the card is already off and nobody owns it, so closing the file
 * cannot write (every disk call checks the owner); it frees the file and the volume. */
static void log_teardown(void)
{
	torn = false;
	if (log_live) {
		(void)fs_close(&log_f);
		log_live = false;
	}
	unmount_vol();
	atomic_set(&log_live_a, 0);
}

/* Every SP1_STORE_LOG_FLUSH_MS while ON, and once a minute the LOG line. */
static void log_tick(void)
{
	const uint32_t now = k_uptime_get_32();
	if ((int32_t)(now - log_next) < 0) {
		return;
	}
	log_next = ((int32_t)(now - log_next) > (int32_t)SP1_STORE_LOG_FLUSH_MS)
		   ? now + SP1_STORE_LOG_FLUSH_MS : log_next + SP1_STORE_LOG_FLUSH_MS;
	const int rc = log_flush();
	if (rc != 0) {
		if (!stop) {
			LINE("LOG off: writing %s failed (%d) -- no more log on the drive "
			     "until the next ON; the console still has it\n",
			     log_path + sizeof(DIR_LOGS), rc);
			log_finish();
		}
		return;
	}
	if (now - sum_t0 >= LOG_SUMMARY_MS) {
		LINE("LOG %u s: %u flushes, %u B written, longest %u ms, %u B lost; log file %u KB%s\n",
		     (unsigned)((now - sum_t0) / 1000u), (unsigned)sum_flushes, (unsigned)sum_bytes,
		     (unsigned)sum_max_ms, (unsigned)sum_lost, (unsigned)(log_size / 1024u),
		     log_full ? " (full)" : "");
		sum_t0 = now;
		sum_flushes = sum_bytes = sum_max_ms = sum_lost = 0u;
	}
}

/* The end of an ON session, after any save: the last lines, then let go of the card. */
static void log_end(void)
{
	if (!log_live) {
		return;
	}
	const uint32_t t0 = k_uptime_get_32();
	LINE("LOG closing %s (%u KB)\n", log_path + sizeof(DIR_LOGS),
	     (unsigned)(log_size / 1024u));
	const int rc = log_flush();
	log_finish();
	if (rc != 0 && !stop) {
		LINE("LOG the last flush FAILED (%d) after %u ms\n", rc,
		     (unsigned)(k_uptime_get_32() - t0));
	}
}

/* ---------------------------------------------------------------- fresh */

#if defined(CONFIG_SP1_FRESH)

#define STAMP DIR_WAKES "/FRESH.ID"

#if defined(CONFIG_SP1_PLAITS)
static int prst_write_defaults(void);    /* the PRST section, below */
#endif

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
#if defined(CONFIG_SP1_PLAITS)
	/* + PRST's folders and default slots (#50): five more directory clusters (PRST and
	 * 1..4, each zeroed by mkdir) and, per file (four slots and SLOT), roughly its data
	 * block, its directory entry and both FATs. An ESTIMATE until a log gives the count
	 * ("block writes against ... expected"); it only scales the progress bar. */
	fmt_expected += 5u * 64u + 5u * 4u;
#endif
	/* + WAKES/CNFG (#52): its directory cluster, logs.cnfg and engines.cnfg (~14 KB, 28
	 * blocks), the same estimate. */
	fmt_expected += 64u + 4u + 32u;
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
#if defined(CONFIG_SP1_PLAITS)
		if (rc == 0) {
			/* PRST (#50, Adara): the folders, and the ROTC patch in every slot. */
			rc = prst_write_defaults();
		}
#endif
		if (rc == 0) {
			/* #52 (Adara): the config files, for users to edit on the drive. */
			rc = cnfg_write_templates();
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

/* ---------------------------------------------------------------- PRST (#50) */

#if defined(CONFIG_SP1_PLAITS)

#define DIR_PRST  DIR_WAKES "/PRST"
#define SLOT_FILE DIR_PRST "/SLOT"
/* The save's temporary file, in the slot's own folder. Not a `.prst`, so it is never
 * loaded -- a save cut short between its write and the rename leaves it behind, and the
 * next save of that slot replaces it. */
#define TMP_NAME  "SAVE.TMP"
#define PATH_MAX_ (sizeof(DIR_PRST) + 16u + MAX_FILE_NAME + 1u)   /* "/<slot>/<name>" */

static void claim_for_wakes(void);

static atomic_t prst_state = ATOMIC_INIT(SP1_STORE_PRST_WAIT);
static atomic_t save_state = ATOMIC_INIT(SP1_STORE_SAVE_IDLE);
static struct sp1_prst prst_slot[SP1_PRST_SLOTS];
/* The file each slot was loaded from ("" = the folder had none): the one a save replaces. */
static char prst_name[SP1_PRST_SLOTS][MAX_FILE_NAME + 1];
static int prst_current;
static char load_text[SP1_PRST_TEXT_MAX];          /* this thread's */
static char save_text[SP1_PRST_TEXT_MAX];          /* main fills it, then queues */
static size_t save_len;
static int save_slot;

static int lower_c(int c)
{
	return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

/* Alphabetical as Adara decided: case ignored, otherwise byte order. */
static int name_cmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		const int x = lower_c((unsigned char)*a), y = lower_c((unsigned char)*b);
		if (x != y || x == 0) {
			return x - y;
		}
	}
}

static bool is_prst(const char *n)
{
	const size_t len = strlen(n);
	return len >= 5u && name_cmp(n + len - 5u, ".prst") == 0;
}

/* One slot: the first `.prst` of its folder, read and checked field by field. 0, or the
 * filesystem's error -- a missing folder is not one (that slot is the ROTC defaults). */
static int load_slot(int i)
{
	static struct fs_dirent ent;
	static char path[PATH_MAX_];
	char *const best = prst_name[i];
	best[0] = '\0';
	snprintk(path, sizeof(path), DIR_PRST "/%d", i + 1);
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	int rc = fs_opendir(&dir, path);
	if (rc == -ENOENT) {
		rc = 0;                               /* no folder: an empty slot */
	} else if (rc == 0) {
		for (;;) {
			rc = fs_readdir(&dir, &ent);
			if (rc != 0 || ent.name[0] == '\0' || stop) {
				break;
			}
			if (ent.type == FS_DIR_ENTRY_FILE && is_prst(ent.name) &&
			    (best[0] == '\0' || name_cmp(ent.name, best) < 0)) {
				strncpy(best, ent.name, MAX_FILE_NAME);
				best[MAX_FILE_NAME] = '\0';
			}
		}
		(void)fs_closedir(&dir);
	}
	if (rc != 0) {
		return rc;
	}
	struct sp1_prst_report r;
	if (best[0] == '\0') {
		sp1_prst_rip(&prst_slot[i], -1);
		LINE("STORE PRST %d: no .prst -- the ROTC defaults\n", i + 1);
		return 0;
	}
	size_t n = 0;
	snprintk(path, sizeof(path), DIR_PRST "/%d/%.255s", i + 1, best);
	rc = read_file(path, load_text, sizeof(load_text), &n);
	if (rc != 0) {
		return rc;
	}
	sp1_prst_parse(load_text, n, &prst_slot[i], &r);
	if (!r.format_ok) {
		LINE("STORE PRST %d: %s -- format %d is not %d: the ROTC defaults\n", i + 1, best,
		     r.format, sp1_prst_format());
	} else if (r.defaulted > 0) {
		LINE("STORE PRST %d: %s (%u B) -- %d of %d fields at their defaults, %d bad%s%s\n",
		     i + 1, best, (unsigned)n, r.defaulted, sp1_prst_fields(), r.bad,
		     r.first_bad ? ", first " : "", r.first_bad ? r.first_bad : "");
	} else {
		LINE("STORE PRST %d: %s (%u B)\n", i + 1, best, (unsigned)n);
	}
	return 0;
}

/* Inside the ON job, the volume mounted: SLOT and the four slots. */
static void prst_load_all(void)
{
	char t[8];
	size_t n = 0;
	prst_current = 0;
	if (read_file(SLOT_FILE, t, sizeof(t), &n) == 0) {
		size_t k = 0;
		while (k < n && (t[k] == ' ' || t[k] == '\t')) {
			k++;
		}
		if (k < n && t[k] >= '1' && t[k] <= '0' + SP1_PRST_SLOTS) {
			prst_current = t[k] - '1';
		}
	}
	for (int i = 0; i < SP1_PRST_SLOTS; i++) {
		const int rc = stop ? -ECANCELED : load_slot(i);
		if (rc != 0) {
			atomic_set(&prst_state, SP1_STORE_PRST_OFF);
			LINE("STORE PRST off: reading slot %d failed (%d) -- defaults, no save\n",
			     i + 1, rc);
			return;
		}
	}
	atomic_set(&prst_state, SP1_STORE_PRST_READY);
	LINE("STORE PRST on: slot %d current\n", prst_current + 1);
}

#if defined(CONFIG_SP1_FRESH)
/* wakes-sp1-fresh, right after its format (Adara, #50): WAKES/PRST/1..4, each holding N.prst
 * with the ROTC default patch, and SLOT = 1. Before the stamp, so a failure here formats
 * again at the next ON. In this thread, so its own text buffer. */
static int prst_write_defaults(void)
{
	static char path[PATH_MAX_];
	struct sp1_prst p;
	sp1_prst_rip(&p, -1);
	const int n = sp1_prst_write(&p, load_text, sizeof(load_text));
	if (n < 0) {
		return -ENOMEM;
	}
	int rc = ensure_dir(DIR_PRST);
	for (int i = 0; rc == 0 && i < SP1_PRST_SLOTS && !stop; i++) {
		snprintk(path, sizeof(path), DIR_PRST "/%c", (char)('1' + i));
		rc = ensure_dir(path);
		if (rc == 0) {
			snprintk(path, sizeof(path), DIR_PRST "/%c/%c.prst", (char)('1' + i),
				 (char)('1' + i));
			rc = write_file(path, load_text, (size_t)n);
		}
	}
	if (rc == 0 && !stop) {
		rc = write_file(SLOT_FILE, "1\n", 2u);
	}
	LINE("STORE fresh: PRST slots 1-4 = the ROTC patch (%d B each), SLOT 1: %s (%d)\n",
	     n, rc == 0 ? "written" : "FAILED", rc);
	return stop ? -ECANCELED : rc;
}
#endif

/* The save, at shutdown. With the log on (#52) the card is still on and mounted from the ON
 * job; otherwise it was powered down after it, and starts again. */
static void job_save(void)
{
	static char tmp[PATH_MAX_];
	static char dst[PATH_MAX_];
	const uint32_t t0 = k_uptime_get_32();
	const int i = save_slot;
	int rc = 0;
	if (!log_live) {
		claim_for_wakes();
		struct sp1_emmc_ident id;
		rc = sp1_emmc_init(&id) ? 0 : -EIO;
		view_base = 0;
		view_count = sp1_emmc_sectors();
		if (rc == 0 && !stop) {
			rc = mount_vol();
		}
	}
	snprintk(tmp, sizeof(tmp), DIR_PRST "/%d", i + 1);
	if (rc == 0 && !stop) {
		rc = ensure_dir(DIR_WAKES);
	}
	if (rc == 0 && !stop) {
		rc = ensure_dir(DIR_PRST);
	}
	if (rc == 0 && !stop) {
		rc = ensure_dir(tmp);
	}
	if (prst_name[i][0] == '\0') {
		snprintk(prst_name[i], sizeof(prst_name[i]), "%d.prst", i + 1);
	}
	snprintk(dst, sizeof(dst), DIR_PRST "/%d/%.255s", i + 1, prst_name[i]);
	snprintk(tmp, sizeof(tmp), DIR_PRST "/%d/" TMP_NAME, i + 1);
	if (rc == 0 && !stop) {
		rc = write_file(tmp, save_text, save_len);
	}
	if (rc == 0 && !stop) {
		rc = fs_rename(tmp, dst);              /* replaces dst (Zephyr's FAT rename) */
	}
	if (rc == 0 && !stop) {
		const char t[2] = { (char)('1' + i), '\n' };
		rc = write_file(SLOT_FILE, t, sizeof(t));
	}
	if (stop && rc == 0) {
		rc = -ECANCELED;
	}
	if (rc == 0) {
		prst_current = i;
	}
	LINE("STORE PRST save slot %d -> %s: %s (%d) in %u ms\n", i + 1, prst_name[i],
	     rc == 0 ? "done" : "FAILED", rc, (unsigned)(k_uptime_get_32() - t0));
	atomic_set(&save_state, rc == 0 ? SP1_STORE_SAVE_OK : SP1_STORE_SAVE_FAILED);
	/* Then the log's last lines (Adara: the save first, the log in the time left); main
	 * waits for sp1_store_log_live() to drop. */
	if (log_live) {
		log_end();
	} else {
		unmount_vol();
		if (!stop) {
			sp1_emmc_power_down();
		}
		atomic_set(&owner, OWN_NONE);
	}
}

#endif /* CONFIG_SP1_PLAITS */

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
	}
#else
	ARG_UNUSED(was_host);
#endif
}

static void job_on_enter(void)
{
	const uint32_t t0 = k_uptime_get_32();
	if (log_live) {
		log_teardown();          /* never expected: every way out of ON closes the log */
	}
	cnfg_defaults();             /* until (unless) the drive's files say otherwise (#52) */
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
#if defined(CONFIG_SP1_PLAITS)
	if (!ok) {
		atomic_set(&prst_state, SP1_STORE_PRST_OFF);
	}
#endif
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
				LINE("STORE volume ok\n");
			} else {
				LINE("STORE PRST off: no filesystem on the eMMC (%d) -- nothing "
				     "written; wakes-sp1-fresh creates one\n", rc);
			}
		}
		/* #52: the config files, every ON, before PRST and before audio. */
		if (rc == 0 && !stop) {
			cnfg_load();
		}
#if defined(CONFIG_SP1_PLAITS)
		/* PRST (#50): all four slots into RAM, before main starts audio. */
		if (rc == 0 && !stop) {
			prst_load_all();
		} else {
			atomic_set(&prst_state, SP1_STORE_PRST_OFF);
		}
#endif
#if defined(CONFIG_SP1_STORAGE_TEST)
		if (rc == 0 && !stop) {
			file_test();
		}
		sp1_emmc_report_stats();
#endif
	}
	/* #52: with logging on, the card stays on and mounted for the whole ON session (Adara)
	 * and the log is written every SP1_STORE_LOG_FLUSH_MS; every way out of ON lets go of
	 * it (log_end at shutdown, log_teardown after sp1_store_abort). Off: as before. */
	if (ok && !stop && atomic_get(&vol_state) == SP1_STORE_VOL_OK) {
		if (!logs_cfg.enabled) {
			sp1_logbuf_file_skip();
		} else {
			const int lrc = log_open();
			if (lrc != 0 && !stop) {
				LINE("LOG off: the log file in WAKES/LOGS/ cannot be opened (%d) -- no log on "
				     "the drive this ON\n", lrc);
			}
		}
	}
	if (log_live) {
		LINE("STORE done after %u ms -- the eMMC stays on for the log\n",
		     (unsigned)(k_uptime_get_32() - t0));
		sum_t0 = k_uptime_get_32();
		sum_flushes = sum_bytes = sum_max_ms = sum_lost = 0u;
		log_next = sum_t0;                 /* the first flush now: the boot so far */
		return;
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

/* Shutdown with PRST off but the log on: only the log's last lines. */
static void job_log_end(void)
{
	log_end();
}

#if defined(CONFIG_SP1_DRIVE)
/* STANDBY with a host attached: power the card and, if it holds a filesystem, hand it to
 * the host until ON (claim_for_wakes) or power-off (sp1_store_abort). A card WITHOUT one
 * is not offered: a computer shown a raw card offers to format it (possibly as exFAT,
 * which Wakes cannot read), and only wakes-sp1-fresh formats (Adara). */
static void job_host(void)
{
	if (log_live) {
		log_teardown();          /* never expected: STANDBY comes after a closed log */
	}
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
		/* With the log on, wake for its next flush (#52); otherwise only for a job. */
		k_timeout_t to = K_FOREVER;
		if (log_live) {
			const int32_t left = (int32_t)(log_next - k_uptime_get_32());
			to = left > 0 ? K_MSEC(left) : K_NO_WAIT;
		}
		(void)k_sem_take(&go, to);
		if (torn) {
			log_teardown();
		}
		for (;;) {
			const atomic_val_t jobs = atomic_get(&pending);
			if (jobs & JOB_ON) {             /* ON always wins: Wakes takes the card */
				atomic_and(&pending, ~JOB_ON);
				job_on_enter();
#if defined(CONFIG_SP1_PLAITS)
			} else if (jobs & JOB_SAVE) {
				atomic_and(&pending, ~JOB_SAVE);
				job_save();
#endif
			} else if (jobs & JOB_LOGEND) {
				atomic_and(&pending, ~JOB_LOGEND);
				job_log_end();
			} else if (jobs & JOB_HOST) {
				atomic_and(&pending, ~JOB_HOST);
#if defined(CONFIG_SP1_DRIVE)
				job_host();
#endif
			} else {
				break;
			}
			if (torn) {
				log_teardown();
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
		if (log_live && !torn) {
			log_tick();
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
#if defined(CONFIG_SP1_PLAITS)
	atomic_set(&prst_state, SP1_STORE_PRST_WAIT);
	atomic_set(&save_state, SP1_STORE_SAVE_IDLE);
#endif
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

#if defined(CONFIG_SP1_PLAITS)
enum sp1_store_prst sp1_store_prst(void)
{
	return (enum sp1_store_prst)atomic_get(&prst_state);
}

int sp1_store_prst_current(void)
{
	return prst_current;
}

const struct sp1_prst *sp1_store_prst_slot(int slot)
{
	return &prst_slot[(slot >= 0 && slot < SP1_PRST_SLOTS) ? slot : 0];
}

bool sp1_store_prst_save(int slot, const struct sp1_prst *p)
{
	if (atomic_get(&prst_state) != SP1_STORE_PRST_READY || slot < 0 ||
	    slot >= SP1_PRST_SLOTS) {
		return false;
	}
	const int n = sp1_prst_write(p, save_text, sizeof(save_text));
	if (n < 0) {
		return false;
	}
	save_len = (size_t)n;
	save_slot = slot;
	prst_slot[slot] = *p;
	atomic_set(&save_state, SP1_STORE_SAVE_BUSY);   /* a barrier: the text is in place */
	atomic_or(&pending, JOB_SAVE);
	k_sem_give(&go);
	return true;
}

enum sp1_store_save sp1_store_save_result(void)
{
	return (enum sp1_store_save)atomic_get(&save_state);
}
#endif

bool sp1_store_log_live(void)
{
	return atomic_get(&log_live_a) != 0;
}

void sp1_store_log_end(void)
{
	if (atomic_get(&log_live_a) != 0) {
		atomic_or(&pending, JOB_LOGEND);
		k_sem_give(&go);
	}
}

void sp1_store_abort(void)
{
	stop = true;
	torn = true;                 /* the thread lets go of the log file and the volume */
	atomic_clear(&pending);
	atomic_set(&owner, OWN_NONE);
	atomic_set(&log_live_a, 0);  /* nothing to wait for: the card is going off now */
	sp1_emmc_abort();
	k_sem_give(&go);
}
