/*
 * wakes-sp1 — the SP-1's eMMC (M6, #43): Toshiba THGBMNG5D1LBAIL, 4 GB, wired 1-bit
 * (CLK, CMD, DAT0, RST_n, VCCQ -- sp1_board.h). The nRF52840 has no SD/MMC peripheral,
 * so the bus is bit-banged.
 *
 * Protocol from chattock/sp1-tape-looper firmware/src/sp1_emmc.c (MIT), itself ported
 * from Tim Knapen's SP-1-dev emmc.c (MIT): the command framing, the response sampling
 * points, the init sequence and the write framing are theirs, proven on this board.
 * Payloads ride SPIM3 DMA at 16 MHz with bit-banged fallbacks (sp1_emmc.c, "fast
 * transfer"); every block read is CRC-checked and every block write must get the card's
 * "accepted" token.
 *
 * ⚠️ Commands sent: 0, 1, 2, 3, 7, 8, 12, 13, 16, 17, 18, 24, 25. Nothing that touches EXT_CSD
 * (CMD6), nothing that erases (CMD35/36/38), nothing that partitions. The card's OTP
 * settings are never written.
 *
 * ⚠️ NOT watchdog-fed: this runs in the storage thread (sp1_store.c), and a background
 * thread that fed the watchdog would hide a hung main loop. Every wait here is bounded.
 *
 * One caller at a time (the storage thread); sp1_emmc_abort() may come from main.
 */
#ifndef SP1_EMMC_H
#define SP1_EMMC_H

#include <stdbool.h>
#include <stdint.h>

#define SP1_EMMC_BLOCK 512u

/* What came back during identification, for the report. */
struct sp1_emmc_ident {
	uint32_t ocr;            /* CMD1's final OCR                                    */
	uint32_t ocr_tries;      /* CMD1s until ready                                    */
	uint32_t ready_ms;       /* power-on to ready                                    */
	uint8_t  cid[16];        /* CID[127:0], MSB first                                */
	int8_t   cid_shift;      /* bit alignment the CID's CRC7 matched at (0 expected), */
	                         /* or -128 if no alignment matched                      */
};

/* Counters since sp1_emmc_init(). */
struct sp1_emmc_stats {
	uint32_t cmd_retries;    /* a command that needed more than one try              */
	uint32_t crc_errs;       /* data blocks whose CRC16 did not match (then retried) */
	uint32_t hunt_timeouts;  /* a data block's start bit never came (100 ms)          */
	uint32_t busy_timeouts;  /* the card stayed busy after a write (500 ms)           */
	uint32_t spim_timeouts;  /* an SPIM3 transfer did not end (5 ms)                 */
	uint32_t wr_rejects;     /* writes the card did not accept (then retried)        */
	uint32_t wr_busy_max_us; /* longest programming busy after a write               */
	uint32_t multi_blocks;   /* blocks moved inside CMD18 / CMD25 bursts             */
	uint32_t multi_fallbacks;/* bursts that had to finish block by block            */
};

/* Power the card, identify it, select it, block length 512, read its size. Bounded:
 * gives up after ~1 s of CMD1 (JEDEC's allowance). `id` may be NULL. False = not
 * usable; call sp1_emmc_power_down() either way. Clears a previous abort. */
bool sp1_emmc_init(struct sp1_emmc_ident *id);

bool sp1_emmc_ready(void);

/* User-area size in blocks (EXT_CSD SEC_COUNT), 0 before a good init. */
uint32_t sp1_emmc_sectors(void);

/* CMD13: card status (bits 31..0). */
bool sp1_emmc_status(uint32_t *status);

/* CMD8 SEND_EXT_CSD: the 512-byte extended CSD. */
bool sp1_emmc_read_ext_csd(uint8_t buf[SP1_EMMC_BLOCK]);

/* CMD17 / CMD24: one block of the user area, by block number (sector-addressed card).
 * Read: CRC-checked. Write: the card's CRC-status token must say "accepted", then its
 * programming busy is waited out (bounded). Both retried up to 3 times. */
bool sp1_emmc_read_block(uint32_t block, uint8_t buf[SP1_EMMC_BLOCK]);
bool sp1_emmc_write_block(uint32_t block, const uint8_t buf[SP1_EMMC_BLOCK]);

/* `n` consecutive blocks in one CMD18 / CMD25 burst (fast transfer). A burst that fails
 * part-way is stopped and the rest done block by block through the calls above, so the
 * result is the same as n single calls, only faster. `buf` must be in RAM for reads
 * (SPIM3 DMAs into it); writes copy through a RAM frame, so any source works. */
bool sp1_emmc_read_blocks(uint32_t block, uint8_t *buf, uint32_t n);
bool sp1_emmc_write_blocks(uint32_t block, const uint8_t *buf, uint32_t n);

void sp1_emmc_get_stats(struct sp1_emmc_stats *s);

/* Release the bus pins (disconnected inputs: nothing may back-feed an unpowered rail),
 * RST_n low, VCCQ off. Safe at any time, and when init was never run. */
void sp1_emmc_power_down(void);

/* From the power-off path (sp1_store_abort): power down AND make every call fail until
 * the next sp1_emmc_init(), so a storage thread caught mid-operation cannot drive the
 * pins of a card whose rail is gone. */
void sp1_emmc_abort(void);

#endif /* SP1_EMMC_H */
