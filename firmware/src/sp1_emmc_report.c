/*
 * wakes-sp1 — the eMMC report (M6, #43). See sp1_emmc_report.h.
 *
 * Began as the read-only probe (2026-10-07, logs/sp1-20261007-152119.log): that run's
 * full EXT_CSD and block-0 hex are in the log and private/docs/M6-PRST-PLAN.md, so the
 * report now prints the decoded lines and only dumps a block it cannot classify.
 *
 * EXT_CSD field positions are JEDEC JESD84-B51 (eMMC 5.1), section 7.4. MBR and GPT
 * layouts are the usual ones; FAT and exFAT boot sectors per Microsoft's specs.
 */
#include "sp1_emmc_report.h"
#include "sp1_emmc.h"
#include "sp1_console.h"

#include <zephyr/kernel.h>
#include <string.h>

static uint8_t blk[SP1_EMMC_BLOCK];
static uint8_t aux[SP1_EMMC_BLOCK];

static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
	       ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

/* printk is paced line by line: CDC ACM drops what it cannot send (sp1_console.c).
 * No watchdog feed: this runs in the storage thread (sp1_emmc.h). */
#define LINE(...) do { printk(__VA_ARGS__); sp1_console_pace(); } while (0)

static void hexdump(const char *tag, const uint8_t *d, uint32_t n)
{
	for (uint32_t o = 0; o < n; o += 16u) {
		char s[16 * 3 + 1];
		char *p = s;
		for (uint32_t i = 0; i < 16u && o + i < n; i++) {
			static const char hx[] = "0123456789abcdef";
			*p++ = ' ';
			*p++ = hx[d[o + i] >> 4];
			*p++ = hx[d[o + i] & 15u];
		}
		*p = '\0';
		LINE("EMMC %s %03x:%s\n", tag, (unsigned)o, s);
	}
}

/* Printable copy of a fixed-width text field. */
static const char *text(const uint8_t *p, uint32_t n)
{
	static char s[40];
	if (n >= sizeof(s)) {
		n = sizeof(s) - 1u;
	}
	for (uint32_t i = 0; i < n; i++) {
		s[i] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
	}
	s[n] = '\0';
	return s;
}

static void fill_counts(const uint8_t *d, uint32_t *zeros, uint32_t *ffs)
{
	*zeros = 0;
	*ffs = 0;
	for (uint32_t i = 0; i < SP1_EMMC_BLOCK; i++) {
		*zeros += (d[i] == 0x00u);
		*ffs += (d[i] == 0xffu);
	}
}

/* A volume boot sector: what filesystem, if any, starts at `lba`. Reads into aux. */
static void boot_sector(const char *what, uint32_t lba)
{
	if (!sp1_emmc_read_block(lba, aux)) {
		LINE("EMMC %s boot sector (block %u): READ FAILED\n", what, (unsigned)lba);
		return;
	}
	const bool sig = (aux[510] == 0x55u && aux[511] == 0xaau);
	if (memcmp(&aux[3], "EXFAT   ", 8) == 0) {
		LINE("EMMC %s: exFAT, %u-byte sectors, %u sectors/cluster, volume %u sectors, "
		     "sig %s\n", what, 1u << aux[108], 1u << aux[109], (unsigned)le32(&aux[72]),
		     sig ? "55aa" : "MISSING");
	} else if (memcmp(&aux[82], "FAT32", 5) == 0) {
		LINE("EMMC %s: FAT32, OEM \"%s\"", what, text(&aux[3], 8));
		LINE(" label \"%s\", %u-byte sectors, %u sectors/cluster, %u reserved, %u FATs"
		     " of %u sectors, %u sectors total, sig %s\n", text(&aux[71], 11),
		     le16(&aux[11]), aux[13], le16(&aux[14]), aux[16], (unsigned)le32(&aux[36]),
		     (unsigned)le32(&aux[32]), sig ? "55aa" : "MISSING");
	} else if (memcmp(&aux[54], "FAT", 3) == 0) {
		LINE("EMMC %s: %s,", what, text(&aux[54], 8));
		LINE(" OEM \"%s\",", text(&aux[3], 8));
		LINE(" label \"%s\", %u-byte sectors, %u sectors/cluster, sig %s\n",
		     text(&aux[43], 11), le16(&aux[11]), aux[13], sig ? "55aa" : "MISSING");
	} else if (memcmp(&aux[3], "NTFS", 4) == 0) {
		LINE("EMMC %s: NTFS\n", what);
	} else {
		LINE("EMMC %s: no FAT/exFAT/NTFS boot sector (sig %s)\n", what,
		     sig ? "55aa" : "none");
	}
	hexdump("vbr", aux, 64);
}

static void decode_ext_csd(const uint8_t *x)
{
	static const char *const rev[] = { "4.0", "4.1", "4.2", "4.3", "?", "4.41", "4.5",
					   "5.0", "5.1" };
	const uint32_t sectors = le32(&x[212]);
	LINE("EMMC EXT_CSD rev %u (eMMC %s), %u sectors = %u MB, device type 0x%02x\n",
	     x[192], x[192] < ARRAY_SIZE(rev) ? rev[x[192]] : "?", (unsigned)sectors,
	     (unsigned)(sectors / 2048u), x[196]);
	LINE("EMMC EXT_CSD boot partitions 2 x %u KB, RPMB %u KB, boot config 0x%02x,"
	     " partitioning support 0x%02x, setting completed %u\n",
	     x[226] * 128u, x[168] * 128u, x[179], x[160], x[155]);
	LINE("EMMC EXT_CSD erase group %u KB (HC, def %u), WP group %u, trim mult %u,"
	     " sec features 0x%02x\n", x[224] * 512u, x[175], x[221], x[230], x[229]);
	LINE("EMMC EXT_CSD reliable write: param 0x%02x set 0x%02x sectors %u\n",
	     x[166], x[167], x[222]);
	LINE("EMMC EXT_CSD cache %u (ctrl %u), power-off notify %u, RST_n function %u,"
	     " HPI 0x%02x, BKOPS support %u\n", (unsigned)le32(&x[249]), x[33], x[34], x[162],
	     x[503], x[502]);
	LINE("EMMC EXT_CSD bus width %u, HS timing %u, life A 0x%02x B 0x%02x,"
	     " pre-EOL 0x%02x, firmware %02x%02x%02x%02x%02x%02x%02x%02x\n",
	     x[183], x[185], x[268], x[269], x[267], x[261], x[260], x[259], x[258], x[257],
	     x[256], x[255], x[254]);
}

static void decode_cid(const struct sp1_emmc_ident *id)
{
	const uint8_t *c = id->cid;
	hexdump("cid", c, 16);
	if (id->cid_shift == -128) {
		LINE("EMMC CID: CRC7 matched at NO alignment -- response sampling is off\n");
	} else {
		LINE("EMMC CID: CRC7 ok at alignment %d%s\n", id->cid_shift,
		     id->cid_shift == 0 ? " (as expected)" : " -- NOT the expected 0");
	}
	/* MDT: month in the high nibble; the year nibble counts from 1997, or from 2013 on
	 * an EXT_CSD rev > 4 part -- printed raw, the rev is on the EXT_CSD line. */
	LINE("EMMC CID: MID 0x%02x, CBX %u, OID 0x%02x, product \"%s\", rev %u.%u,"
	     " serial 0x%08x, month %u year code %u\n", c[0], c[1] & 3u, c[2],
	     text(&c[3], 6), c[9] >> 4, c[9] & 15u,
	     (unsigned)(((uint32_t)c[10] << 24) | ((uint32_t)c[11] << 16) |
			((uint32_t)c[12] << 8) | c[13]), c[14] >> 4, c[14] & 15u);
}

static void decode_gpt(void)
{
	if (!sp1_emmc_read_block(1, aux) || memcmp(aux, "EFI PART", 8) != 0) {
		LINE("EMMC GPT: no header at block 1\n");
		return;
	}
	const uint32_t entries_lba = le32(&aux[72]);
	const uint32_t n = le32(&aux[80]);
	const uint32_t size = le32(&aux[84]);
	LINE("EMMC GPT: %u entries of %u bytes at block %u\n", (unsigned)n, (unsigned)size,
	     (unsigned)entries_lba);
	if (size != 128u || !sp1_emmc_read_block(entries_lba, blk)) {
		return;
	}
	uint32_t first[4] = { 0 };
	for (uint32_t i = 0; i < 4u && i < n; i++) {
		const uint8_t *e = &blk[i * 128u];
		bool used = false;
		for (int k = 0; k < 16; k++) {
			used |= (e[k] != 0u);
		}
		if (!used) {
			continue;
		}
		char name[37];
		for (int k = 0; k < 36; k++) {          /* UTF-16LE, ASCII only */
			const uint8_t ch = e[56 + 2 * k];
			name[k] = (ch >= 0x20 && ch < 0x7f) ? (char)ch : (ch ? '?' : '\0');
		}
		name[36] = '\0';
		first[i] = le32(&e[32]);
		LINE("EMMC GPT p%u: blocks %u..%u \"%s\"\n", (unsigned)(i + 1),
		     (unsigned)first[i], (unsigned)le32(&e[40]), name);
	}
	for (uint32_t i = 0; i < 4u; i++) {
		if (first[i] != 0u) {
			char what[8] = "p?";
			what[1] = (char)('1' + i);
			boot_sector(what, first[i]);
		}
	}
}

/* Block 0: MBR, a bare FAT volume, sp1-tape-looper's index, or something else. */
static void decode_block0(void)
{
	if (!sp1_emmc_read_block(0, blk)) {
		LINE("EMMC block 0: READ FAILED\n");
		return;
	}

	const uint32_t magic = le32(blk);
	if ((magic >> 16) == 0x5345u) {          /* 'S' 'E' ..: the looper's META_MAGIC */
		LINE("EMMC block 0: sp1-tape-looper's song index (magic 0x%08x \"%s\")\n",
		     (unsigned)magic, text(blk, 4));
	}
	const bool sig = (blk[510] == 0x55u && blk[511] == 0xaau);
	if (!sig) {
		uint32_t z, f;
		fill_counts(blk, &z, &f);
		LINE("EMMC block 0: no 55aa signature, so no MBR (%u x 00, %u x ff)\n",
		     (unsigned)z, (unsigned)f);
		hexdump("b0", blk, 64);
		return;
	}
	if ((blk[0] == 0xebu || blk[0] == 0xe9u) &&
	    (memcmp(&blk[82], "FAT", 3) == 0 || memcmp(&blk[54], "FAT", 3) == 0 ||
	     memcmp(&blk[3], "EXFAT", 5) == 0)) {
		LINE("EMMC block 0: a filesystem with no partition table\n");
		boot_sector("vol", 0);
		return;
	}
	LINE("EMMC block 0: MBR, disk signature 0x%08x\n", (unsigned)le32(&blk[440]));
	uint32_t start[4] = { 0 };
	bool gpt = false;
	for (uint32_t i = 0; i < 4u; i++) {
		const uint8_t *e = &blk[446u + 16u * i];
		if (e[4] == 0u) {
			continue;
		}
		start[i] = le32(&e[8]);
		gpt |= (e[4] == 0xeeu);
		LINE("EMMC MBR p%u: status 0x%02x type 0x%02x, blocks %u + %u (%u MB)\n",
		     (unsigned)(i + 1), e[0], e[4], (unsigned)start[i], (unsigned)le32(&e[12]),
		     (unsigned)(le32(&e[12]) / 2048u));
	}
	if (gpt) {
		decode_gpt();
		return;
	}
	for (uint32_t i = 0; i < 4u; i++) {
		if (start[i] != 0u) {
			char what[4] = "p?";
			what[1] = (char)('1' + i);
			boot_sector(what, start[i]);
		}
	}
}

void sp1_emmc_report_init(bool ok, const struct sp1_emmc_ident *id)
{
	LINE("EMMC init %s: %u CMD1s, %u ms to ready, OCR 0x%08x (%s), %u blocks\n",
	     ok ? "ok" : "FAILED", (unsigned)id->ocr_tries, (unsigned)id->ready_ms,
	     (unsigned)id->ocr, (id->ocr & 0x40000000u) ? "sector mode" : "byte mode",
	     (unsigned)sp1_emmc_sectors());
	if (!ok) {
		struct sp1_emmc_stats s;
		sp1_emmc_get_stats(&s);
		LINE("EMMC gave up (command retries %u)\n", (unsigned)s.cmd_retries);
		return;
	}
	decode_cid(id);
}

void sp1_emmc_report_card(void)
{
	uint32_t status = 0;
	if (sp1_emmc_status(&status)) {
		static const char *const state[] = { "idle", "ready", "ident", "stby", "tran",
						     "data", "rcv", "prg", "dis", "btst", "slp" };
		const uint32_t s = (status >> 9) & 15u;
		LINE("EMMC status 0x%08x: state %s%s\n", (unsigned)status,
		     s < ARRAY_SIZE(state) ? state[s] : "?",
		     /* JEDEC error bits: 31-26, 24-19, 16, 15, 7 */
		     (status & 0xfdf98080u) ? ", ERROR BITS SET" : "");
	} else {
		LINE("EMMC status: no response\n");
	}

	if (sp1_emmc_read_ext_csd(aux)) {
		decode_ext_csd(aux);
	} else {
		LINE("EMMC EXT_CSD: READ FAILED\n");
	}
	decode_block0();
}

void sp1_emmc_report_stats(void)
{
	struct sp1_emmc_stats s;
	sp1_emmc_get_stats(&s);
	LINE("EMMC errors: command retries %u, read CRC %u, write rejects %u, timeouts: "
	     "data %u busy %u spim %u; longest write busy %u us\n", (unsigned)s.cmd_retries,
	     (unsigned)s.crc_errs, (unsigned)s.wr_rejects, (unsigned)s.hunt_timeouts,
	     (unsigned)s.busy_timeouts, (unsigned)s.spim_timeouts,
	     (unsigned)s.wr_busy_max_us);
	LINE("EMMC fast transfer: %u blocks in multi-block bursts, %u bursts finished "
	     "block by block\n", (unsigned)s.multi_blocks, (unsigned)s.multi_fallbacks);
	if (IS_ENABLED(CONFIG_SP1_EMMC_VERIFY)) {
		LINE("EMMC verify: %u written blocks read back wrong, %u right after a rewrite\n",
		     (unsigned)s.verify_fails, (unsigned)s.verify_fixed);
	}
}
