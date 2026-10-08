/*
 * wakes-sp1 — the SP-1's eMMC (M6, #43). See sp1_emmc.h.
 *
 * Derived from chattock/sp1-tape-looper firmware/src/sp1_emmc.c, Copyright (c) 2026
 * chattock, MIT, which is ported from timknapen/SP-1-dev emmc.c, Copyright (c) 2026
 * Tim Knapen, MIT. Licence texts: LICENSES/MIT-sp1-tape-looper.txt and
 * LICENSES/MIT-SP-1-dev.txt.
 *
 * ---- the sampling points, which are the whole trick ----
 * They are the tape-looper's, unchanged, because they are what works on this board.
 * The card changes CMD and DAT0 after a FALLING clock edge and the host reads them
 * while CLK is high, so:
 *   - a response is found by pulsing CLK and reading CMD after the falling edge. The
 *     first read with CLK high afterwards sees the START BIT AGAIN -- so a captured
 *     response begins with its start bit: index 0 = start, 1 = transmission, 2..7 =
 *     the command index (or 111111), then the payload MSB first.
 *     (The tape-looper's own R1 parsing -- READY_FOR_DATA = r1[3] & 0x01,
 *     SWITCH_ERROR = r1[4] & 0x80 -- is only right with this alignment.)
 *   - a data block is found by reading DAT0 with CLK high; the bit after the start bit
 *     arrives after the next falling edge, so there is no re-read there.
 *   - a write puts each bit on DAT0 while CLK is low; the card latches it on the
 *     rising edge. The card's CRC-status token and its busy are read like data.
 * Hardware 2026-10-07 (logs/sp1-20261007-152119.log, -173828.log): the CID's CRC7
 * matched at alignment 0; reads, writes and a FAT32 format ran with no error.
 *
 * ---- fast transfer (M6, Adara: "yes" to doing it with drive mode) ----
 * The tape-looper's, again: the 512-byte payloads (and their CRC16) ride SPIM3 + EasyDMA
 * -- SPI mode 0 is the same wire format as eMMC DAT0 at default speed -- while the start
 * bit hunt, the CRC-status token, the busy and every command stay bit-banged. SPIM3 is the
 * only SPIM above 8 MHz and nothing else in Wakes uses it. We run it at 16 MHz, inside the
 * card's 26 MHz default-speed limit (the looper's 32 MHz overclock glitched at 24 kHz).
 * After identification, commands drop the 1 us half-period (the looper's CMDFAST: same
 * edges, register-speed). Multi-block CMD18 / CMD25 + CMD12 replace one command per block.
 * EVERY fast step has a slow one behind it: a failed multi-block transfer finishes block by
 * block, a failed block is retried bit-banged (then at the 1 us clock), and a command that
 * misses its response at speed is retried at the identification clock.
 * ⚠️ nRF52840 anomaly 198 (SPIM3 TX data can be corrupted by CPU access to the same RAM
 * block): harmless here BY DESIGN -- the card checks the CRC16 we computed from the source
 * buffer before the DMA, rejects a corrupted block (token 101), and the block is retried.
 */
#include "sp1_emmc.h"
#include "sp1_board.h"

#include <zephyr/kernel.h>
#include <hal/nrf_gpio.h>
#include <string.h>

#define PIN_CLK   NRF_GPIO_PIN_MAP(0, SP1_EMMC_CLK_PIN)
#define PIN_DAT0  NRF_GPIO_PIN_MAP(0, SP1_EMMC_DAT0_PIN)
#define PIN_CMD   NRF_GPIO_PIN_MAP(0, SP1_EMMC_CMD_PIN)
#define PIN_RST   NRF_GPIO_PIN_MAP(1, SP1_EMMC_RST_PIN)    /* SP1_EMMC_RST_PORT = NRF_P1 */
#define PIN_VCCQ  NRF_GPIO_PIN_MAP(0, SP1_EMMC_VCCQ_PIN)   /* SP1_EMMC_VCCQ_PORT = NRF_P0 */

/* CLK, DAT0 and CMD through port 0's registers. A few NOPs of settle around each edge
 * (the tape-looper's EDGE_SETTLE; JEDEC's minimum clock high/low is 10 ns, a NOP is
 * 15.6 ns). The "slow" variants use a 1 us half-period: identification (400 kHz limit)
 * and the last retry of anything. */
#define P0_CLK   (1u << SP1_EMMC_CLK_PIN)
#define P0_DAT0  (1u << SP1_EMMC_DAT0_PIN)
#define P0_CMD   (1u << SP1_EMMC_CMD_PIN)
#define CLK_HI() (NRF_P0->OUTSET = P0_CLK)
#define CLK_LO() (NRF_P0->OUTCLR = P0_CLK)
#define DAT_HI() (NRF_P0->OUTSET = P0_DAT0)
#define DAT_LO() (NRF_P0->OUTCLR = P0_DAT0)
#define DAT0()   ((NRF_P0->IN >> SP1_EMMC_DAT0_PIN) & 1u)
#define CMD_IN() ((NRF_P0->IN >> SP1_EMMC_CMD_PIN) & 1u)
#define SETTLE() __asm__ volatile("nop\nnop\nnop")
#define SLOW_HALF_US 1u

#define INIT_TIMEOUT_MS   1000u   /* CMD1 until ready: JEDEC's 1 s                */
#define DATA_TIMEOUT_MS    100u   /* start bit of a data block                    */
#define BUSY_TIMEOUT_MS    500u   /* programming busy after a write / CMD12       */

#define R1_BITS_DATA  38u         /* a response followed by a data block (send_cmd) */
#define R1_BITS       48u
#define R2_BITS      138u         /* 136 + 2 spare, for the CID alignment check    */

/* Card-status bits in CMD24/25's own R1 that mean "this write must not go ahead": 31
 * ADDRESS_OUT_OF_RANGE, 30 ADDRESS_MISALIGN, 29 BLOCK_LEN_ERROR, 26 WP_VIOLATION, 25
 * DEVICE_IS_LOCKED. Deliberately NOT the whole JEDEC error set: COM_CRC_ERROR and
 * ILLEGAL_COMMAND report the PREVIOUS command, so after any retried command they would
 * refuse a perfectly good write. */
#define WRITE_FATAL 0xe6000000u

/* How a data block moves: SPIM3 DMA, bit-banged at register speed, or at 1 us. */
enum mode { M_SPIM, M_FAST, M_SLOW };

static bool     ready;
static volatile bool aborted;
static bool     cmd_fast;         /* identification done: commands at register speed */
static bool     spim_on;          /* SPIM3 configured (power_down undoes it)          */
static uint32_t rca;              /* relative card address, already << 16             */
static uint32_t sectors;
static struct sp1_emmc_stats st;

/* ---------------------------------------------------------------- CRCs */

static uint8_t crc7(const uint8_t *d, uint32_t n)
{
	uint8_t crc = 0;
	for (uint32_t i = 0; i < n; i++) {
		for (int b = 7; b >= 0; b--) {
			crc <<= 1;
			if ((((d[i] >> b) & 1u) ^ ((crc >> 7) & 1u)) != 0u) {
				crc ^= 0x09;
			}
			crc &= 0x7f;
		}
	}
	return (uint8_t)((crc << 1) | 1u);     /* with the end bit, as it sits in a frame */
}

/* CRC-16/XMODEM (0x1021), table-driven: the bit loop costs ~0.5 ms a block, which at
 * fast-transfer rates is most of the time. The table is built at init and checked
 * against the standard answer ("123456789" -> 0x31C3); init fails if it is wrong. */
static uint16_t crc_tab[256];

static void crc16_build(void)
{
	for (uint32_t i = 0; i < 256u; i++) {
		uint16_t c = (uint16_t)(i << 8);
		for (int b = 0; b < 8; b++) {
			c = (c & 0x8000u) ? (uint16_t)((c << 1) ^ 0x1021u) : (uint16_t)(c << 1);
		}
		crc_tab[i] = c;
	}
}

static uint16_t crc16(const uint8_t *d, uint32_t n)
{
	uint16_t crc = 0;
	while (n--) {
		crc = (uint16_t)((crc << 8) ^ crc_tab[((crc >> 8) ^ *d++) & 0xffu]);
	}
	return crc;
}

/* ---------------------------------------------------------------- SPIM3 */

#define SPIM_FREQ_16M 0x0A000000u

static void spim_setup(void)
{
	NRF_SPIM3->ENABLE = 0;
	NRF_SPIM3->PSEL.SCK = PIN_CLK;
	NRF_SPIM3->PSEL.MOSI = 0xffffffffu;      /* attached per transfer */
	NRF_SPIM3->PSEL.MISO = 0xffffffffu;
	NRF_SPIM3->PSEL.CSN = 0xffffffffu;
	NRF_SPIM3->FREQUENCY = SPIM_FREQ_16M;
	NRF_SPIM3->CONFIG = 0;                   /* MSB first, CPOL 0 / CPHA 0 (mode 0) */
	NRF_SPIM3->ORC = 0xff;                   /* idle high */
	spim_on = true;
}

/* One blocking DMA transfer with SPIM3 owning CLK and DAT0. On disable the pins fall
 * back to their GPIO latches (CLK low, DAT0 as configured), so the bit-banged phases
 * either side carry on. 516 bytes at 16 MHz is ~260 us. Bounded. */
static bool spim_xfer(const uint8_t *tx, uint32_t txn, uint8_t *rx, uint32_t rxn)
{
	NRF_SPIM3->PSEL.MOSI = tx ? PIN_DAT0 : 0xffffffffu;
	NRF_SPIM3->PSEL.MISO = rx ? PIN_DAT0 : 0xffffffffu;
	NRF_SPIM3->ENABLE = 7;
	NRF_SPIM3->TXD.PTR = (uint32_t)tx;
	NRF_SPIM3->TXD.MAXCNT = tx ? txn : 0u;
	NRF_SPIM3->RXD.PTR = (uint32_t)rx;
	NRF_SPIM3->RXD.MAXCNT = rx ? rxn : 0u;
	NRF_SPIM3->EVENTS_END = 0;
	NRF_SPIM3->TASKS_START = 1;
	const uint32_t c0 = k_cycle_get_32();
	while (NRF_SPIM3->EVENTS_END == 0u &&
	       k_cyc_to_us_floor32(k_cycle_get_32() - c0) < 5000u) {
	}
	/* ⚠️ Read END AGAIN after the time check. This thread is the lowest priority there
	 * is: at ~70 % audio load it can be held off for several ms BETWEEN reading END and
	 * reading the clock, and then a transfer that finished meanwhile looked timed out.
	 * Hardware 2026-10-07 (logs/sp1-20261007-193746.log): 38 such false timeouts in
	 * 512 KB, each one dropping its burst to block-by-block -- data intact, speed lost. */
	const bool done = (NRF_SPIM3->EVENTS_END != 0u);
	if (!done) {
		st.spim_timeouts++;
		NRF_SPIM3->TASKS_STOP = 1;
	}
	NRF_SPIM3->ENABLE = 0;
	return done;
}

/* ---------------------------------------------------------------- command phase */

static inline void half(bool slow)
{
	if (slow) {
		k_busy_wait(SLOW_HALF_US);
	} else {
		SETTLE();
	}
}

static inline void pulse(bool slow)
{
	CLK_HI();
	half(slow);
	CLK_LO();
	half(slow);
}

/* Send one command; capture `nbits` of the response (0 = none expected) into `resp`,
 * MSB first, index 0 = the start bit (see the top of the file). False = no response.
 *
 * ⚠️ For a command followed by a data block FROM the card (CMD8, CMD17, CMD18), capture
 * R1_BITS_DATA and no more: the block can start on DAT0 a couple of clocks after the
 * response, and every extra clock here is one the data hunt never sees (the
 * tape-looper found this the hard way). */
static bool send_cmd(uint8_t index, uint32_t arg, uint8_t *resp, uint32_t nbits, bool slow)
{
	if (aborted) {
		return false;
	}
	uint8_t f[6];
	f[0] = (uint8_t)(0x40u | (index & 0x3fu));
	f[1] = (uint8_t)(arg >> 24);
	f[2] = (uint8_t)(arg >> 16);
	f[3] = (uint8_t)(arg >> 8);
	f[4] = (uint8_t)arg;
	f[5] = crc7(f, 5);

	/* The gap before a command, on a RELEASED line: the tail of the previous response
	 * plus JEDEC's Nrc, without driving against the card's last bits. */
	nrf_gpio_cfg_input(PIN_CMD, NRF_GPIO_PIN_PULLUP);
	for (int i = 0; i < 24; i++) {
		pulse(slow);
	}
	NRF_P0->OUTSET = P0_CMD;
	nrf_gpio_cfg_output(PIN_CMD);
	for (int i = 0; i < 6; i++) {
		for (int b = 7; b >= 0; b--) {
			if ((f[i] >> b) & 1u) {
				NRF_P0->OUTSET = P0_CMD;
			} else {
				NRF_P0->OUTCLR = P0_CMD;
			}
			pulse(slow);
		}
	}
	nrf_gpio_cfg_input(PIN_CMD, NRF_GPIO_PIN_PULLUP);

	if (nbits == 0u) {
		return true;
	}
	bool found = false;
	for (int t = 0; t < 200 && !found; t++) {
		pulse(slow);
		found = (CMD_IN() == 0u);
	}
	if (!found) {
		return false;
	}
	memset(resp, 0, (nbits + 7u) / 8u);
	for (uint32_t i = 0; i < nbits; i++) {
		CLK_HI();
		half(slow);
		const uint32_t bit = CMD_IN();
		CLK_LO();
		half(slow);
		resp[i / 8u] |= (uint8_t)(bit << (7u - (i % 8u)));
	}
	return true;
}

/* Up to 8 tries; after identification the first four are at register speed and the
 * rest at the identification clock, so a speed problem costs time, not the command. */
static bool send_cmd_retry(uint8_t index, uint32_t arg, uint8_t *resp, uint32_t nbits)
{
	for (int t = 0; t < 8 && !aborted; t++) {
		const bool slow = !cmd_fast || t >= 4;
		if (send_cmd(index, arg, resp, nbits, slow)) {
			return true;
		}
		st.cmd_retries++;
		if (t == 0) {
			for (int c = 0; c < 16; c++) {
				pulse(slow);
			}
		} else {
			k_msleep(2);
		}
	}
	return false;
}

/* `n` bits of a captured response from bit `first`, as an integer, MSB first. */
static uint32_t resp_bits(const uint8_t *r, uint32_t first, uint32_t n)
{
	uint32_t v = 0;
	for (uint32_t i = first; i < first + n; i++) {
		v = (v << 1) | ((r[i / 8u] >> (7u - (i % 8u))) & 1u);
	}
	return v;
}

/* ---------------------------------------------------------------- data phase */

/* One data block from the card, after its command's response (or the previous block of
 * a CMD18): 512 bytes, the card's CRC16, the end bit. True only if the CRC matched. */
static bool read_data(uint8_t *buf, enum mode m)
{
	const bool slow = (m == M_SLOW);
	nrf_gpio_cfg_input(PIN_DAT0, NRF_GPIO_PIN_PULLUP);

	/* Start-bit hunt, bit-banged. The card only moves on our clock, so stopping to look
	 * at the time can never miss it. */
	const uint32_t t0 = k_uptime_get_32();
	bool got = false;
	while (!got) {
		for (int i = 0; i < 64; i++) {
			CLK_HI();
			half(slow);
			if (DAT0() == 0u) {
				got = true;              /* leave with CLK high */
				break;
			}
			CLK_LO();
			half(slow);
		}
		if (!got && ((k_uptime_get_32() - t0) >= DATA_TIMEOUT_MS || aborted)) {
			st.hunt_timeouts++;
			return false;
		}
	}
	CLK_LO();
	half(slow);

	uint8_t c2[2];
	if (m == M_SPIM) {
		/* The start bit is consumed, so the payload and CRC are byte-aligned: two DMA
		 * receives straight into the caller's buffer and c2 (no copy). */
		if (!spim_xfer(NULL, 0, buf, SP1_EMMC_BLOCK) || !spim_xfer(NULL, 0, c2, 2)) {
			/* counted in spim_xfer */
			return false;
		}
	} else {
		for (uint32_t i = 0; i < SP1_EMMC_BLOCK + 2u; i++) {
			uint32_t byte = 0;
			for (int b = 7; b >= 0; b--) {
				CLK_HI();
				half(slow);
				byte |= DAT0() << b;
				CLK_LO();
				half(slow);
			}
			if (i < SP1_EMMC_BLOCK) {
				buf[i] = (uint8_t)byte;
			} else {
				c2[i - SP1_EMMC_BLOCK] = (uint8_t)byte;
			}
		}
	}
	pulse(slow);                             /* end bit */

	if (crc16(buf, SP1_EMMC_BLOCK) != (uint16_t)((c2[0] << 8) | c2[1])) {
		st.crc_errs++;
		return false;
	}
	return true;
}

/* Wait while the card holds DAT0 low (programming). True once it lets go. The first
 * clock here carries the token's end bit; each read comes after a falling edge and its
 * settle, as the tape-looper's busy wait does -- read straight after the edge, it can
 * still see the previous bit and call the card free one clock early. */
static bool busy_wait(bool slow)
{
	nrf_gpio_cfg_input(PIN_DAT0, NRF_GPIO_PIN_PULLUP);
	const uint32_t c0 = k_cycle_get_32();
	const uint32_t t0 = k_uptime_get_32();
	for (;;) {
		for (int i = 0; i < 64; i++) {
			pulse(slow);
			if (DAT0() != 0u) {
				const uint32_t us = k_cyc_to_us_floor32(k_cycle_get_32() - c0);
				if (us > st.wr_busy_max_us) {
					st.wr_busy_max_us = us;
				}
				return true;
			}
		}
		const uint32_t el = k_uptime_get_32() - t0;
		if (el >= BUSY_TIMEOUT_MS || aborted) {
			st.busy_timeouts++;
			return false;
		}
		if (el >= 1u) {
			k_usleep(50);                /* a long program: let others run */
		}
	}
}

/* The frame a write puts on DAT0 through SPIM3: Nwr idle (0xFF), 7 idle bits + the start
 * bit (0xFE), the payload, its CRC16. ⚠️ It ends EXACTLY at the CRC's last bit: the end
 * bit is clocked by hand right after, because the card's token comes 2 clocks after it
 * and a trailing idle byte inside the DMA would let it fly past (the tape-looper). */
static uint8_t txf[2u + SP1_EMMC_BLOCK + 2u];

/* The last write_data()'s CRC-status token (-1 = none seen) and busy, for the verify
 * trace below. */
static int last_tok;
static uint32_t last_busy_us;

/* One data block TO the card after CMD24's response (or the previous block of a CMD25);
 * then its CRC-status token (010 = accepted) and its busy. True only if accepted and
 * programmed. */
static bool write_data(const uint8_t *buf, enum mode m)
{
	const bool slow = (m == M_SLOW);
	const uint16_t crc = crc16(buf, SP1_EMMC_BLOCK);

	DAT_HI();
	nrf_gpio_cfg(PIN_DAT0, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT,
		     NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
	if (m == M_SPIM) {
		txf[0] = 0xff;
		txf[1] = 0xfe;
		memcpy(&txf[2], buf, SP1_EMMC_BLOCK);   /* also makes a flash source DMA-able */
		txf[2u + SP1_EMMC_BLOCK] = (uint8_t)(crc >> 8);
		txf[3u + SP1_EMMC_BLOCK] = (uint8_t)crc;
		if (!spim_xfer(txf, sizeof(txf), NULL, 0)) {
			nrf_gpio_cfg_input(PIN_DAT0, NRF_GPIO_PIN_PULLUP);
			/* counted in spim_xfer */
			return false;
		}
	} else {
		for (int i = 0; i < 8; i++) {        /* Nwr: idle high, so no early start bit */
			half(slow);
			pulse(slow);
		}
		DAT_LO();                            /* start bit */
		half(slow);
		pulse(slow);
		for (uint32_t i = 0; i < SP1_EMMC_BLOCK + 2u; i++) {
			const uint32_t byte = (i < SP1_EMMC_BLOCK) ? buf[i]
				: (i == SP1_EMMC_BLOCK ? (uint32_t)(crc >> 8) : (uint32_t)(crc & 0xffu));
			for (int b = 7; b >= 0; b--) {
				if ((byte >> b) & 1u) {
					DAT_HI();
				} else {
					DAT_LO();
				}
				half(slow);
				pulse(slow);
			}
		}
	}
	DAT_HI();                                /* end bit (SPIM left DAT0 on its latch) */
	half(slow);
	pulse(slow);
	nrf_gpio_cfg_input(PIN_DAT0, NRF_GPIO_PIN_PULLUP);   /* the card drives from here */

	/* CRC-status token: start bit (0), three status bits, end bit. */
	int status = -1;
	for (int i = 0; i < 16 && status < 0; i++) {
		CLK_HI();
		half(slow);
		const uint32_t start = DAT0();
		CLK_LO();
		half(slow);
		if (start == 0u) {
			status = 0;
			for (int k = 0; k < 3; k++) {
				CLK_HI();
				half(slow);
				status = (status << 1) | (int)DAT0();
				CLK_LO();
				half(slow);
			}
		}
	}
	/* Wait out the busy whatever the token said: a rejected block can still leave the
	 * card busy, and the next command must not land on it. */
	const uint32_t b0 = k_cycle_get_32();
	const bool done = busy_wait(slow);
	last_tok = status;
	last_busy_us = k_cyc_to_us_floor32(k_cycle_get_32() - b0);
	if (status != 0x2) {
		st.wr_rejects++;
		return false;
	}
	return done;
}

/* A command followed by one data block from the card; the block retried bit-banged, then
 * at 1 us. */
static bool read_cmd_block(uint8_t index, uint32_t arg, uint8_t *buf)
{
	static const enum mode modes[] = { M_SPIM, M_FAST, M_SLOW };
	uint8_t r[(R1_BITS_DATA + 7u) / 8u];
	for (unsigned t = 0; t < ARRAY_SIZE(modes) && !aborted; t++) {
		if (!send_cmd_retry(index, arg, r, R1_BITS_DATA)) {
			return false;
		}
		if (read_data(buf, modes[t])) {
			return true;
		}
	}
	return false;
}

/* CMD12 STOP_TRANSMISSION. After a write burst it is R1b: the card holds DAT0 low while
 * it commits, and returning before that makes the next command miss its response. */
static bool stop_transmission(bool after_write)
{
	uint8_t r[(R1_BITS + 7u) / 8u];
	const bool ok = send_cmd_retry(12, 0, r, R1_BITS);
	return (after_write ? busy_wait(false) : true) && ok;
}

/* ---- burst READS are closed-ended (M6, proven 2026-10-07) ----
 * ⚠️ Open-ended CMD18 (the tape-looper's way: run until CMD12) RETURNS JUNK on this card.
 * CMD12 lands while the card is already fetching the block after the last one wanted, and
 * some later burst then hands back, as its SECOND block, the card's internal buffer -- with a
 * valid CRC, so nothing below this layer can see it. Measured on hardware
 * (logs/sp1-20261007-221018.log, 4 ON entries x 3 rounds of FAT scan + 512 KB read):
 * open-ended 5 bad blocks, every one at position 1 of its burst, every one the same
 * buffer-like bytes give or take a few bits; closed-ended 0. Earlier runs: the host's own
 * FAT scan set it up and Wakes' next read caught it (-202944, -210404, -215523).
 * So every burst read is CMD23 SET_BLOCK_COUNT + CMD18: the card stops itself after exactly
 * n blocks, and CMD12 is sent only to cut a failed burst short.
 * Burst WRITES stay open-ended (CMD25 ... CMD12): ~10 000 verified blocks, 0 wrong. */
static bool set_block_count(uint32_t n)
{
	uint8_t r[(R1_BITS + 7u) / 8u];
	return send_cmd_retry(23, n & 0xffffu, r, R1_BITS);
}

/* ---------------------------------------------------------------- API */

bool sp1_emmc_init(struct sp1_emmc_ident *id)
{
	struct sp1_emmc_ident local;
	if (id == NULL) {
		id = &local;
	}
	memset(id, 0, sizeof(*id));
	id->cid_shift = -128;
	memset(&st, 0, sizeof(st));
	ready = false;
	cmd_fast = false;
	sectors = 0;
	aborted = false;

	crc16_build();
	if (crc16((const uint8_t *)"123456789", 9) != 0x31c3u) {
		return false;                   /* never move data with a broken check */
	}

	nrf_gpio_cfg(PIN_CLK, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT,
		     NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
	CLK_LO();
	NRF_P0->OUTSET = P0_CMD;
	nrf_gpio_cfg_output(PIN_CMD);
	nrf_gpio_cfg_input(PIN_DAT0, NRF_GPIO_PIN_PULLUP);   /* driven only while writing */
	nrf_gpio_cfg_output(PIN_RST);
	nrf_gpio_cfg_output(PIN_VCCQ);
	spim_setup();

	/* Power and reset, as the tape-looper does it. RST_n only acts if the card's
	 * RST_n_FUNCTION is enabled -- it is not on this unit (EXT_CSD[162] = 0). */
	const uint32_t t_on = k_uptime_get_32();
	nrf_gpio_pin_set(PIN_VCCQ);
	k_msleep(10);
	nrf_gpio_pin_clear(PIN_RST);
	k_msleep(1);
	nrf_gpio_pin_set(PIN_RST);
	k_msleep(2);

	for (int i = 0; i < 80; i++) {          /* >= 74 clocks before the first command */
		pulse(true);
	}

	(void)send_cmd(0, 0, NULL, 0, true);    /* CMD0 GO_IDLE_STATE, no response */
	k_msleep(1);

	/* CMD1 SEND_OP_COND, sector addressing (HCS), until the power-up bit (OCR[31],
	 * response bit 8) is set. */
	uint8_t r[(R2_BITS + 7u) / 8u];
	bool up = false;
	while (!up && !aborted && (k_uptime_get_32() - t_on) < INIT_TIMEOUT_MS) {
		id->ocr_tries++;
		if (send_cmd(1, 0x40ff8000u, r, R1_BITS, true)) {
			id->ocr = resp_bits(r, 8, 32);
			up = (id->ocr & 0x80000000u) != 0u;
		}
		if (!up) {
			k_msleep(1);
		}
	}
	id->ready_ms = k_uptime_get_32() - t_on;
	if (!up) {
		return false;
	}

	/* CMD2 ALL_SEND_CID: R2, CID[127:0] from bit 8. Checked against the CID's own CRC7
	 * at three alignments; the report says which matched. */
	if (!send_cmd_retry(2, 0, r, R2_BITS)) {
		return false;
	}
	static const int8_t shifts[] = { 0, -1, 1 };
	for (unsigned s = 0; s < ARRAY_SIZE(shifts); s++) {
		uint8_t cid[16];
		for (uint32_t k = 0; k < 16u; k++) {
			cid[k] = (uint8_t)resp_bits(r, (uint32_t)(8 + shifts[s]) + 8u * k, 8);
		}
		if (crc7(cid, 15) == cid[15] || s == 0u) {
			memcpy(id->cid, cid, sizeof(cid));
		}
		if (crc7(cid, 15) == cid[15]) {
			id->cid_shift = shifts[s];
			break;
		}
	}

	rca = 1u << 16;                          /* CMD3 SET_RELATIVE_ADDR: we choose 1 */
	if (!send_cmd_retry(3, rca, r, R1_BITS) ||
	    !send_cmd_retry(7, rca, r, R1_BITS) ||     /* CMD7 SELECT_CARD -> transfer */
	    !send_cmd_retry(16, SP1_EMMC_BLOCK, r, R1_BITS)) {   /* CMD16 SET_BLOCKLEN */
		return false;
	}
	ready = true;
	cmd_fast = true;                         /* identification over: full speed */

	/* The size, from EXT_CSD SEC_COUNT [215:212]. */
	static uint8_t xcsd[SP1_EMMC_BLOCK];
	if (!read_cmd_block(8, 0, xcsd)) {
		ready = false;
		return false;
	}
	sectors = (uint32_t)xcsd[212] | ((uint32_t)xcsd[213] << 8) |
		  ((uint32_t)xcsd[214] << 16) | ((uint32_t)xcsd[215] << 24);
	return sectors != 0u;
}

bool sp1_emmc_ready(void)
{
	return ready && !aborted;
}

uint32_t sp1_emmc_sectors(void)
{
	return sp1_emmc_ready() ? sectors : 0u;
}

bool sp1_emmc_status(uint32_t *status)
{
	uint8_t r[(R1_BITS + 7u) / 8u];
	if (!send_cmd_retry(13, rca, r, R1_BITS)) {
		return false;
	}
	*status = resp_bits(r, 8, 32);
	return true;
}

bool sp1_emmc_read_ext_csd(uint8_t buf[SP1_EMMC_BLOCK])
{
	return sp1_emmc_ready() && read_cmd_block(8, 0, buf);
}

bool sp1_emmc_read_block(uint32_t block, uint8_t buf[SP1_EMMC_BLOCK])
{
	return sp1_emmc_ready() && block < sectors && read_cmd_block(17, block, buf);
}

static bool write_block_raw(uint32_t block, const uint8_t *buf)
{
	static const enum mode modes[] = { M_SPIM, M_FAST, M_SLOW };
	uint8_t r[(R1_BITS + 7u) / 8u];
	for (unsigned t = 0; t < ARRAY_SIZE(modes) && !aborted; t++) {
		/* CMD24 WRITE_BLOCK. Its R1 reports this command's own errors (address out
		 * of range, write protect, ...): never send data after one of those. */
		if (!send_cmd_retry(24, block, r, R1_BITS)) {
			return false;
		}
		if ((resp_bits(r, 8, 32) & WRITE_FATAL) != 0u) {
			return false;
		}
		if (write_data(buf, modes[t])) {
			return true;
		}
	}
	return false;
}

#if defined(CONFIG_SP1_EMMC_VERIFY)
/* ---- read every write back (diagnostics, M6) ----
 * Hardware 2026-10-07 (logs/sp1-20261007-202944.log, -210404.log): in about a third of ON
 * entries the 512 KB test file read back with one or two blocks still holding the card's
 * OLD contents -- every token "accepted", no CRC error, no timeout, all in CMD25 bursts.
 * So a write was lost or landed elsewhere. This reads each written block straight back,
 * and on a mismatch says where in its burst it was, what the card answered for it, and
 * whether a slow re-read agrees (a lost WRITE) or not (a READ problem); then rewrites it
 * on its own and checks again. */
static uint8_t vbuf[SP1_EMMC_BLOCK];
static uint8_t vbuf2[SP1_EMMC_BLOCK];

static bool read_back(uint32_t block, uint8_t *dst, enum mode m)
{
	uint8_t r[(R1_BITS_DATA + 7u) / 8u];
	return send_cmd_retry(17, block, r, R1_BITS_DATA) && read_data(dst, m);
}

static void hex16(char *out, const uint8_t *d)
{
	static const char hx[] = "0123456789abcdef";
	for (int i = 0; i < 16; i++) {
		out[3 * i] = ' ';
		out[3 * i + 1] = hx[d[i] >> 4];
		out[3 * i + 2] = hx[d[i] & 15u];
	}
	out[48] = '\0';
}

static bool verify_block(uint32_t block, const uint8_t *src, const char *how, uint32_t pos,
			 uint32_t n, int tok, uint32_t busy_us, uint32_t prev_busy_us)
{
	if (read_back(block, vbuf, M_SPIM) && memcmp(vbuf, src, SP1_EMMC_BLOCK) == 0) {
		return true;
	}
	st.verify_fails++;
	const bool slow_ok = read_back(block, vbuf2, M_SLOW);
	const bool slow_right = slow_ok && memcmp(vbuf2, src, SP1_EMMC_BLOCK) == 0;
	const bool same = slow_ok && memcmp(vbuf2, vbuf, SP1_EMMC_BLOCK) == 0;
	uint32_t diff = 0;
	for (uint32_t i = 0; i < SP1_EMMC_BLOCK; i++) {
		diff += (vbuf[i] != src[i]);
	}
	printk("EMMC VERIFY FAIL: %s LBA %u = block %u of %u; token %d, busy %u us (block before: "
	       "%u us); %u bytes differ; slow re-read %s\n", how, (unsigned)block, (unsigned)pos,
	       (unsigned)n, tok, (unsigned)busy_us, (unsigned)prev_busy_us, (unsigned)diff,
	       !slow_ok ? "FAILED" : (slow_right ? "is RIGHT -> a READ problem"
				     : (same ? "agrees -> the WRITE was lost"
					     : "differs from both")));
	char a[49], b[49];
	hex16(a, vbuf);
	hex16(b, src);
	printk("EMMC VERIFY FAIL: read  %s\nEMMC VERIFY FAIL: wrote %s\n", a, b);
	const bool fixed = write_block_raw(block, src) && read_back(block, vbuf, M_SPIM) &&
			   memcmp(vbuf, src, SP1_EMMC_BLOCK) == 0;
	if (fixed) {
		st.verify_fixed++;
	}
	printk("EMMC VERIFY: rewritten on its own: %s\n", fixed ? "now right" : "STILL WRONG");
	return fixed;
}
#endif

bool sp1_emmc_write_block(uint32_t block, const uint8_t buf[SP1_EMMC_BLOCK])
{
	if (!sp1_emmc_ready() || block >= sectors || !write_block_raw(block, buf)) {
		return false;
	}
#if defined(CONFIG_SP1_EMMC_VERIFY)
	return verify_block(block, buf, "CMD24", 0, 1, last_tok, last_busy_us, 0);
#else
	return true;
#endif
}

bool sp1_emmc_read_blocks(uint32_t block, uint8_t *buf, uint32_t n)
{
	if (!sp1_emmc_ready() || n == 0u || block >= sectors || n > sectors - block) {
		return false;
	}
	/* The card reads ahead in a CMD18; ending one on the very last block can trip
	 * ADDRESS_OUT_OF_RANGE, so the device's last block always goes on its own. */
	uint32_t multi = (block + n == sectors) ? n - 1u : n;
	uint32_t done = 0;
	if (multi >= 2u) {
		uint8_t r[(R1_BITS_DATA + 7u) / 8u];
		if (set_block_count(multi) && send_cmd_retry(18, block, r, R1_BITS_DATA)) {
			while (done < multi && read_data(buf + done * SP1_EMMC_BLOCK, M_SPIM)) {
				done++;
			}
			/* Complete: the card stopped by itself. Cut short: it must be told. */
			if (done < multi) {
				(void)stop_transmission(false);
			}
			st.multi_blocks += done;
			if (done < multi) {
				st.multi_fallbacks++;
			}
		}
	}
	for (; done < n; done++) {               /* the rest, or all of it, one by one */
		if (!sp1_emmc_read_block(block + done, buf + done * SP1_EMMC_BLOCK)) {
			return false;
		}
	}
	return true;
}

bool sp1_emmc_write_blocks(uint32_t block, const uint8_t *buf, uint32_t n)
{
	if (!sp1_emmc_ready() || n == 0u || block >= sectors || n > sectors - block) {
		return false;
	}
	uint32_t done = 0;
	if (n >= 2u) {
		uint8_t r[(R1_BITS + 7u) / 8u];
#if defined(CONFIG_SP1_EMMC_VERIFY)
		static int8_t btok[64];
		static uint32_t bbusy[64];
#endif
		if (send_cmd_retry(25, block, r, R1_BITS) &&
		    (resp_bits(r, 8, 32) & WRITE_FATAL) == 0u) {
			while (done < n && write_data(buf + done * SP1_EMMC_BLOCK, M_SPIM)) {
#if defined(CONFIG_SP1_EMMC_VERIFY)
				if (done < 64u) {
					btok[done] = (int8_t)last_tok;
					bbusy[done] = last_busy_us;
				}
#endif
				done++;
			}
			if (!stop_transmission(true)) {
				done = 0;                /* unsure what landed: rewrite it all */
			}
			st.multi_blocks += done;
			if (done < n) {
				st.multi_fallbacks++;
			}
#if defined(CONFIG_SP1_EMMC_VERIFY)
			for (uint32_t j = 0; j < done; j++) {
				const bool rec = j < 64u;
				if (!verify_block(block + j, buf + j * SP1_EMMC_BLOCK, "CMD25", j, n,
						  rec ? btok[j] : -2, rec ? bbusy[j] : 0u,
						  (rec && j > 0u) ? bbusy[j - 1u] : 0u)) {
					return false;
				}
			}
#endif
		}
	}
	for (; done < n; done++) {               /* rewriting a block is harmless */
		if (!sp1_emmc_write_block(block + done, buf + done * SP1_EMMC_BLOCK)) {
			return false;
		}
	}
	return true;
}

void sp1_emmc_get_stats(struct sp1_emmc_stats *s)
{
	*s = st;
}

void sp1_emmc_power_down(void)
{
	ready = false;
	cmd_fast = false;
	if (spim_on) {
		NRF_SPIM3->ENABLE = 0;
		NRF_SPIM3->PSEL.SCK = 0xffffffffu;
		NRF_SPIM3->PSEL.MOSI = 0xffffffffu;
		NRF_SPIM3->PSEL.MISO = 0xffffffffu;
		/* nRF52840 anomaly 195: SPIM3 keeps drawing current after disable unless
		 * this is written (nrfx_spim_uninit does the same). */
		*(volatile uint32_t *)0x4002F004 = 1;
		spim_on = false;
	}
	nrf_gpio_pin_clear(PIN_RST);
	nrf_gpio_cfg_default(PIN_CLK);
	nrf_gpio_cfg_default(PIN_CMD);
	nrf_gpio_cfg_default(PIN_DAT0);
	nrf_gpio_cfg_default(PIN_RST);
	nrf_gpio_cfg_output(PIN_VCCQ);
	nrf_gpio_pin_clear(PIN_VCCQ);            /* rail off; sp1_quiesce_peripherals agrees */
}

void sp1_emmc_abort(void)
{
	aborted = true;
	sp1_emmc_power_down();
}
