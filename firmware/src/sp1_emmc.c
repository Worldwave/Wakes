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
 * Hardware 2026-10-07 (logs/sp1-20261007-152119.log): the CID's CRC7 matched at
 * alignment 0, and 64 block reads came back with no CRC error.
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

/* Command phase: a 1 us half-period through the GPIO HAL, a few hundred kHz -- inside
 * the 400 kHz identification limit. Commands are short, so it stays slow throughout. */
#define CMD_HALF_US  1u

/* Data phase: CLK and DAT0 through port 0's registers, a few NOPs of settle around each
 * edge (the tape-looper's EDGE_SETTLE; JEDEC's minimum clock high/low time is 10 ns, a
 * NOP is 15.6 ns). After an error the retries run with a 1 us half-period instead. */
#define P0_CLK   (1u << SP1_EMMC_CLK_PIN)
#define P0_DAT0  (1u << SP1_EMMC_DAT0_PIN)
#define CLK_HI() (NRF_P0->OUTSET = P0_CLK)
#define CLK_LO() (NRF_P0->OUTCLR = P0_CLK)
#define DAT_HI() (NRF_P0->OUTSET = P0_DAT0)
#define DAT_LO() (NRF_P0->OUTCLR = P0_DAT0)
#define DAT0()   ((NRF_P0->IN >> SP1_EMMC_DAT0_PIN) & 1u)
#define SETTLE() __asm__ volatile("nop\nnop\nnop")
#define SLOW_HALF_US 1u

#define INIT_TIMEOUT_MS   1000u   /* CMD1 until ready: JEDEC's 1 s                */
#define DATA_TIMEOUT_MS    100u   /* start bit of a data block                    */
#define BUSY_TIMEOUT_MS    500u   /* programming busy after a write               */
#define BLOCK_TRIES          3

#define R1_BITS_DATA  38u         /* a response followed by a data block (send_cmd) */
#define R1_BITS       48u
#define R2_BITS      138u         /* 136 + 2 spare, for the CID alignment check    */

/* Card-status bits in CMD24's own R1 that mean "this write must not go ahead": 31
 * ADDRESS_OUT_OF_RANGE, 30 ADDRESS_MISALIGN, 29 BLOCK_LEN_ERROR, 26 WP_VIOLATION, 25
 * DEVICE_IS_LOCKED. Deliberately NOT the whole JEDEC error set: COM_CRC_ERROR and
 * ILLEGAL_COMMAND report the PREVIOUS command, so after any retried command they would
 * refuse a perfectly good write. */
#define WRITE_FATAL 0xe6000000u

static bool     ready;
static volatile bool aborted;
static uint32_t rca;              /* relative card address, already << 16 */
static uint32_t sectors;
static struct sp1_emmc_stats st;

/* ---------------------------------------------------------------- command phase */

static void clk_pulse(void)
{
	nrf_gpio_pin_set(PIN_CLK);
	k_busy_wait(CMD_HALF_US);
	nrf_gpio_pin_clear(PIN_CLK);
	k_busy_wait(CMD_HALF_US);
}

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

static uint16_t crc16(const uint8_t *d, uint32_t n)      /* CRC-16/XMODEM, 0x1021 */
{
	uint16_t crc = 0;
	while (n--) {
		crc ^= (uint16_t)(*d++) << 8;
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
					      : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

/* Send one command; capture `nbits` of the response (0 = none expected) into `resp`,
 * MSB first, index 0 = the start bit (see the top of the file). False = no response.
 *
 * ⚠️ For a command followed by a data block FROM the card (CMD8, CMD17), capture
 * R1_BITS_DATA and no more: the block can start on DAT0 a couple of clocks after the
 * response, and every extra clock here is one the data hunt never sees (the
 * tape-looper found this the hard way). */
static bool send_cmd(uint8_t index, uint32_t arg, uint8_t *resp, uint32_t nbits)
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
		clk_pulse();
	}
	nrf_gpio_cfg_output(PIN_CMD);
	for (int i = 0; i < 6; i++) {
		for (int b = 7; b >= 0; b--) {
			if ((f[i] >> b) & 1u) {
				nrf_gpio_pin_set(PIN_CMD);
			} else {
				nrf_gpio_pin_clear(PIN_CMD);
			}
			clk_pulse();
		}
	}
	nrf_gpio_cfg_input(PIN_CMD, NRF_GPIO_PIN_PULLUP);

	if (nbits == 0u) {
		return true;
	}
	bool found = false;
	for (int t = 0; t < 200 && !found; t++) {
		clk_pulse();
		found = (nrf_gpio_pin_read(PIN_CMD) == 0u);
	}
	if (!found) {
		return false;
	}
	memset(resp, 0, (nbits + 7u) / 8u);
	for (uint32_t i = 0; i < nbits; i++) {
		nrf_gpio_pin_set(PIN_CLK);
		k_busy_wait(CMD_HALF_US);
		const uint32_t bit = nrf_gpio_pin_read(PIN_CMD);
		nrf_gpio_pin_clear(PIN_CLK);
		k_busy_wait(CMD_HALF_US);
		resp[i / 8u] |= (uint8_t)(bit << (7u - (i % 8u)));
	}
	return true;
}

static bool send_cmd_retry(uint8_t index, uint32_t arg, uint8_t *resp, uint32_t nbits)
{
	for (int t = 0; t < 8 && !aborted; t++) {
		if (send_cmd(index, arg, resp, nbits)) {
			return true;
		}
		st.cmd_retries++;
		if (t == 0) {
			for (int c = 0; c < 16; c++) {
				clk_pulse();
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

static inline void data_half(bool slow)
{
	if (slow) {
		k_busy_wait(SLOW_HALF_US);
	} else {
		SETTLE();
	}
}

/* One clock with the host's bit already on DAT0: setup, rising edge (the card latches),
 * high time, falling edge. */
static inline void wclk(bool slow)
{
	data_half(slow);
	CLK_HI();
	data_half(slow);
	CLK_LO();
}

/* One data block after its command's response: 512 bytes, then the card's CRC16, then
 * the end bit. True only if the CRC matched. */
static bool read_data(uint8_t *buf, bool slow)
{
	nrf_gpio_cfg_input(PIN_DAT0, NRF_GPIO_PIN_PULLUP);

	/* Start-bit hunt. The card only moves on our clock, so stopping to look at the
	 * time can never miss it. */
	const uint32_t t0 = k_uptime_get_32();
	bool got = false;
	while (!got) {
		for (int i = 0; i < 64; i++) {
			CLK_HI();
			data_half(slow);
			if (DAT0() == 0u) {
				got = true;              /* leave with CLK high */
				break;
			}
			CLK_LO();
			data_half(slow);
		}
		if (!got && ((k_uptime_get_32() - t0) >= DATA_TIMEOUT_MS || aborted)) {
			st.timeouts++;
			return false;
		}
	}
	CLK_LO();
	data_half(slow);

	for (uint32_t i = 0; i < SP1_EMMC_BLOCK; i++) {
		uint32_t byte = 0;
		for (int b = 7; b >= 0; b--) {
			CLK_HI();
			data_half(slow);
			byte |= DAT0() << b;
			CLK_LO();
			data_half(slow);
		}
		buf[i] = (uint8_t)byte;
	}
	uint32_t card_crc = 0;
	for (int b = 0; b < 16; b++) {
		CLK_HI();
		data_half(slow);
		card_crc = (card_crc << 1) | DAT0();
		CLK_LO();
		data_half(slow);
	}
	CLK_HI();                                /* end bit */
	data_half(slow);
	CLK_LO();
	data_half(slow);

	if (crc16(buf, SP1_EMMC_BLOCK) != (uint16_t)card_crc) {
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
			wclk(slow);
			data_half(slow);
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
			st.timeouts++;
			return false;
		}
		if (el >= 1u) {
			k_usleep(50);                /* a long program: let others run */
		}
	}
}

/* One data block TO the card after CMD24's response: Nwr idle, start bit, 512 bytes,
 * CRC16, end bit; then the card's CRC-status token (010 = accepted) and its busy.
 * True only if accepted and programmed. */
static bool write_data(const uint8_t *buf, bool slow)
{
	const uint16_t crc = crc16(buf, SP1_EMMC_BLOCK);

	DAT_HI();
	nrf_gpio_cfg(PIN_DAT0, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT,
		     NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
	for (int i = 0; i < 8; i++) {            /* Nwr: idle high, so no early start bit */
		wclk(slow);
	}
	DAT_LO();                                /* start bit */
	wclk(slow);
	for (uint32_t i = 0; i < SP1_EMMC_BLOCK; i++) {
		const uint32_t byte = buf[i];
		for (int b = 7; b >= 0; b--) {
			if ((byte >> b) & 1u) {
				DAT_HI();
			} else {
				DAT_LO();
			}
			wclk(slow);
		}
	}
	for (int b = 15; b >= 0; b--) {
		if ((crc >> b) & 1u) {
			DAT_HI();
		} else {
			DAT_LO();
		}
		wclk(slow);
	}
	DAT_HI();                                /* end bit */
	wclk(slow);
	nrf_gpio_cfg_input(PIN_DAT0, NRF_GPIO_PIN_PULLUP);   /* the card drives from here */

	/* CRC-status token: start bit (0), three status bits, end bit. */
	int status = -1;
	for (int i = 0; i < 16 && status < 0; i++) {
		CLK_HI();
		data_half(slow);
		const uint32_t start = DAT0();
		CLK_LO();
		data_half(slow);
		if (start == 0u) {
			status = 0;
			for (int k = 0; k < 3; k++) {
				CLK_HI();
				data_half(slow);
				status = (status << 1) | (int)DAT0();
				CLK_LO();
				data_half(slow);
			}
		}
	}
	/* Wait out the busy whatever the token said: a rejected block can still leave the
	 * card busy, and the next command must not land on it. */
	const bool done = busy_wait(slow);
	if (status != 0x2) {
		st.wr_rejects++;
		return false;
	}
	return done;
}

/* A command followed by one data block from the card, retried as a whole. */
static bool read_cmd_block(uint8_t index, uint32_t arg, uint8_t *buf)
{
	uint8_t r[(R1_BITS_DATA + 7u) / 8u];
	for (int t = 0; t < BLOCK_TRIES && !aborted; t++) {
		if (!send_cmd_retry(index, arg, r, R1_BITS_DATA)) {
			return false;
		}
		if (read_data(buf, t > 0)) {
			return true;
		}
	}
	return false;
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
	sectors = 0;
	aborted = false;

	nrf_gpio_cfg(PIN_CLK, NRF_GPIO_PIN_DIR_OUTPUT, NRF_GPIO_PIN_INPUT_DISCONNECT,
		     NRF_GPIO_PIN_NOPULL, NRF_GPIO_PIN_H0H1, NRF_GPIO_PIN_NOSENSE);
	nrf_gpio_pin_clear(PIN_CLK);
	nrf_gpio_pin_set(PIN_CMD);
	nrf_gpio_cfg_output(PIN_CMD);
	nrf_gpio_cfg_input(PIN_DAT0, NRF_GPIO_PIN_PULLUP);   /* driven only while writing */
	nrf_gpio_cfg_output(PIN_RST);
	nrf_gpio_cfg_output(PIN_VCCQ);

	/* Power and reset, as the tape-looper does it. RST_n only acts if the card's
	 * RST_n_FUNCTION is enabled -- it is not on this unit (EXT_CSD[162] = 0). */
	const uint32_t t_on = k_uptime_get_32();
	nrf_gpio_pin_set(PIN_VCCQ);
	k_msleep(10);
	nrf_gpio_pin_clear(PIN_RST);
	k_msleep(1);
	nrf_gpio_pin_set(PIN_RST);
	k_msleep(2);

	nrf_gpio_pin_set(PIN_CMD);
	for (int i = 0; i < 80; i++) {          /* >= 74 clocks before the first command */
		clk_pulse();
	}

	(void)send_cmd(0, 0, NULL, 0);          /* CMD0 GO_IDLE_STATE, no response */
	k_msleep(1);

	/* CMD1 SEND_OP_COND, sector addressing (HCS), until the power-up bit (OCR[31],
	 * response bit 8) is set. */
	uint8_t r[(R2_BITS + 7u) / 8u];
	bool up = false;
	while (!up && !aborted && (k_uptime_get_32() - t_on) < INIT_TIMEOUT_MS) {
		id->ocr_tries++;
		if (send_cmd(1, 0x40ff8000u, r, R1_BITS)) {
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

bool sp1_emmc_write_block(uint32_t block, const uint8_t buf[SP1_EMMC_BLOCK])
{
	if (!sp1_emmc_ready() || block >= sectors) {
		return false;
	}
	uint8_t r[(R1_BITS + 7u) / 8u];
	for (int t = 0; t < BLOCK_TRIES && !aborted; t++) {
		/* CMD24 WRITE_BLOCK. Its R1 reports this command's own errors (address out
		 * of range, write protect, ...): never send data after one of those. */
		if (!send_cmd_retry(24, block, r, R1_BITS)) {
			return false;
		}
		if ((resp_bits(r, 8, 32) & WRITE_FATAL) != 0u) {
			return false;
		}
		if (write_data(buf, t > 0)) {
			return true;
		}
	}
	return false;
}

void sp1_emmc_get_stats(struct sp1_emmc_stats *s)
{
	*s = st;
}

void sp1_emmc_power_down(void)
{
	ready = false;
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
