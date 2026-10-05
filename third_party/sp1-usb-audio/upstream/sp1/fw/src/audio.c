/* SP-1 audio bring-up.
 *
 * Ported from chattock/sp1-tape-looper, firmware/src/main.c (MIT), at 2565e79:
 * tas2505_configure(), hp_codec_init() on its HP_TIM_TEST path, audio_thread()
 * and audio_init(). Register values and the init ORDER are unchanged; that is
 * the part proven on hardware. The CS42L42 sequence is Tim Knapen's
 * (github.com/timknapen/SP-1-dev/wiki/I2C).
 *
 *   Copyright (c) chattock, sp1-tape-looper contributors. MIT License.
 *
 * Clock topology: the 3.072 MHz oscillator (enabled on P0.13) drives SCLK; the
 * CS42L42 is frame master and divides it by 64 to an exact 48 kHz LRCK; the
 * nRF I2S and the TAS2505 are both slaves.
 */
/*
 * Copyright (c) 2026 Ryan Gilmore
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "audio.h"
#include "usb_audio.h"
#include "wdt.h"

#include <cmsis_core.h>
#include <hal/nrf_gpio.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/kernel.h>

#define CS42_RST_PORT NRF_P0
#define CS42_RST_PIN  15u
#define TAS_RST_PORT  NRF_P0
#define TAS_RST_PIN   9u      /* an NFC pin: the board's uicr reclaims it as GPIO */
#define OSC_EN_PORT   NRF_P0
#define OSC_EN_PIN    13u
#define CS42L42_ADDR  0x48u
#define TAS2505_ADDR  0x18u
#define BLK_BYTES     (AUDIO_BLK_FRAMES * 2 * (int)sizeof(int16_t))

/* nrfx TX queue (2), two EasyDMA buffers, the block being filled, plus slack. */
K_MEM_SLAB_DEFINE_STATIC(tx_slab, BLK_BYTES, 8, 4);

static const struct device *const i2c_bus = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct device *const i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));

static volatile uint16_t vol_target = 45;   /* main thread writes, audio thread reads */
static audio_render_fn render_cb;
static bool cs42_up;
static bool i2s_started;
static int8_t hp_in = -1;

static void gpio_drive_high(NRF_GPIO_Type *port, uint32_t pin)
{
	port->OUTSET = 1u << pin;
	port->PIN_CNF[pin] =
		(GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos) |
		(GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos) |
		(GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos);
	port->OUTSET = 1u << pin;
}

static void gpio_drive_low(NRF_GPIO_Type *port, uint32_t pin)
{
	port->OUTCLR = 1u << pin;
}

/* BOOT CLICK, 2026-09-16. The speaker used to click as it came up: the DAC was
 * unmuted on page 0 BEFORE any analog block existed, class-D gain was set to
 * +18 dB and attenuation to 0 dB BEFORE the driver powered up, and every analog
 * enable was a back-to-back I2C write with nothing allowed to settle. The amp
 * was being switched on into a live path -- amp before preamp.
 *
 * Fixed by muting across the bring-up and unmuting last, plus two settling
 * delays. NOT a volume problem: master volume is applied digitally -- a boot
 * volume ramp would change nothing. This diverges from the looper's init order
 * deliberately (Ryan, 2026-09-16). */
/* ---- TAS2505 speaker amp (TI SLAU472C §5.1). PLL locked to BCLK:
 * 3.072 MHz x J32 = 98.304 MHz / (NDAC2 x MDAC8 x DOSR128) = 48000 Hz. ---- */
static int tas_wr(uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte(i2c_bus, TAS2505_ADDR, reg, val);
}

static bool tas2505_configure(void)
{
	int rc = 0;

	rc |= tas_wr(0x00, 0x00);
	rc |= tas_wr(0x01, 0x01);          /* software reset */
	k_msleep(5);

	rc |= tas_wr(0x00, 0x01);
	rc |= tas_wr(0x02, 0x00);          /* LDO 1.8 V, level shifters up */

	rc |= tas_wr(0x00, 0x00);
	rc |= tas_wr(0x04, 0x07);          /* PLL_CLKIN = BCLK, CODEC_CLKIN = PLL */
	rc |= tas_wr(0x05, 0x91);          /* PLL on, P=1, R=1 */
	rc |= tas_wr(0x06, 0x20);          /* J = 32 */
	rc |= tas_wr(0x07, 0x00);          /* D = 0 */
	rc |= tas_wr(0x08, 0x00);
	k_msleep(15);                      /* PLL lock */
	rc |= tas_wr(0x0B, 0x82);          /* NDAC = 2 */
	rc |= tas_wr(0x0C, 0x88);          /* MDAC = 8 */
	rc |= tas_wr(0x0D, 0x00);
	rc |= tas_wr(0x0E, 0x80);          /* DOSR = 128 */
	rc |= tas_wr(0x1B, 0x00);          /* I2S, 16-bit, slave */
	rc |= tas_wr(0x1C, 0x00);
	rc |= tas_wr(0x3C, 0x02);          /* PRB_P2, mono */
	rc |= tas_wr(0x3F, 0x90);          /* DAC up, left -> left, soft-step */
	rc |= tas_wr(0x41, 0x00);          /* digital gain 0 dB */
	/* The DAC stays MUTED here -- its power-on-reset state -- through the whole
	 * analog bring-up below. The unmute moved to the end of this function; see
	 * the boot-click note above. */

	rc |= tas_wr(0x00, 0x01);
	rc |= tas_wr(0x01, 0x10);          /* analog reference on */
	rc |= tas_wr(0x0A, 0x00);          /* common mode 0.9 V */
	/* Let the common-mode node charge before anything drives a load. These were
	 * back-to-back I2C writes, which is the click. */
	k_msleep(20);
	rc |= tas_wr(0x0C, 0x04);          /* DAC -> output mixer */
	rc |= tas_wr(0x16, 0x00);
	rc |= tas_wr(0x18, 0x00);
	rc |= tas_wr(0x09, 0x20);          /* HP driver up */
	rc |= tas_wr(0x10, 0x00);
	rc |= tas_wr(0x2E, 0x00);          /* speaker attenuation 0 dB */
	rc |= tas_wr(0x30, 0x30);          /* class-D gain +18 dB */
	rc |= tas_wr(0x2D, 0x02);          /* speaker driver up */
	/* And let the driver settle before the DAC is allowed to feed it. */
	k_msleep(20);

	rc |= tas_wr(0x00, 0x00);
	/* Unmute LAST, into an already-settled analog path, and let the DAC's own
	 * soft-step (0x3F bit 4, set above) do the ramp. */
	rc |= tas_wr(0x40, 0x04);          /* unmute */
	k_msleep(10);
	return rc == 0;
}

/* ---- CS42L42 headphones: paged register protocol ---- */
static bool tpw(uint16_t reg, uint8_t val)
{
	uint8_t p[2] = { 0x00, (uint8_t)(reg >> 8) };
	uint8_t b[2] = { (uint8_t)reg, val };
	if (i2c_write(i2c_bus, p, 2, CS42L42_ADDR) != 0) return false;
	return i2c_write(i2c_bus, b, 2, CS42L42_ADDR) == 0;
}

static bool tpr(uint16_t reg, uint8_t *val)
{
	uint8_t p[2] = { 0x00, (uint8_t)(reg >> 8) };
	uint8_t o = (uint8_t)reg;
	if (i2c_write(i2c_bus, p, 2, CS42L42_ADDR) != 0) return false;
	return i2c_write_read(i2c_bus, CS42L42_ADDR, &o, 1, val, 1) == 0;
}

static void hp_codec_init(void)
{
	uint8_t id[3] = { 0 }, pll;

	gpio_drive_low(CS42_RST_PORT, CS42_RST_PIN);   /* without it the codec NAKs */
	k_msleep(5);
	gpio_drive_high(CS42_RST_PORT, CS42_RST_PIN);
	k_msleep(10);

	(void)tpr(0x1001, &id[0]);
	(void)tpr(0x1002, &id[1]);
	(void)tpr(0x1003, &id[2]);
	if (id[0] != 0x42) return;
	(void)tpw(0x1508, 0x10);   /* PLL Control 3 */
	(void)tpw(0x1504, 0x80);   /* PLL Division Fractional Byte 2 */
	(void)tpw(0x1505, 0x3E);   /* PLL Division Integer */
	(void)tpw(0x150A, 0x7D);   /* PLL Calibration Ratio */
	(void)tpw(0x1009, 0x00);   /* MCLK Control */
	(void)tpw(0x1201, 0x01);   /* MCLK Source Select */
	(void)tpw(0x120A, 0x01);   /* Input ASRC Clock Select */
	(void)tpw(0x120B, 0x01);   /* Output ASRC Clock Select */
	(void)tpw(0x1501, 0x01);   /* PLL start */
	(void)tpw(0x1107, 0x01);   /* Oscillator Switch: SCLK */
	for (int t = 0; t < 10; t++) {
		k_msleep(1);
		if (tpr(0x1109, &pll) && pll == 0x02) break;
	}
	(void)tpw(0x1007, 0x13);   /* Serial Port SRC Control */
	(void)tpw(0x1203, 0x1F);   /* FSYNC Pulse Width Lower (64-SCLK frame) */
	(void)tpw(0x1205, 0x3F);   /* FSYNC Period Lower */
	(void)tpw(0x1207, 0x34);   /* ASP Clock Config: MASTER */
	(void)tpw(0x1208, 0x1A);   /* ASP Frame Configuration */
	(void)tpw(0x2A02, 0x02);   /* Channel 1 */
	(void)tpw(0x2A05, 0x42);   /* Channel 2 */
	(void)tpw(0x2601, 0x4C);   /* SRC Input Sample Rate */
	(void)tpw(0x2609, 0x4C);   /* SRC Output Sample Rate */
	(void)tpw(0x2A01, 0x0C);   /* ASP Receive Enable */
	(void)tpw(0x240E, 0x01);   /* Equalizer Input Mute Control */
	(void)tpw(0x2301, 0x00);   /* Mixer A 0 dB */
	(void)tpw(0x2303, 0x00);   /* Mixer B 0 dB */
	(void)tpw(0x1101, 0x96);   /* power up */
	k_msleep(10);
	(void)tpw(0x1121, 0x41);   /* Headset switch control */
	(void)tpw(0x1B74, 0x03);   /* Misc detect control */
	(void)tpw(0x1129, 0x01);   /* Headset clamp disable */
	(void)tpw(0x2001, 0x0D);   /* HP mute all */
	(void)tpw(0x1F06, 0x84);   /* DAC Control 2 */
	(void)tpw(0x2301, 0x00);
	(void)tpw(0x2303, 0x00);
	(void)tpw(0x1B73, 0xC2);   /* Tip Sense Control */
	(void)tpw(0x1B75, 0x9F);   /* Mic detect control 1 */
	(void)tpw(0x2001, 0x01);   /* unmute headphones */
	cs42_up = true;
}

/* ---- speaker on/off: TAS2505 page 1, reg 0x2D (0x02 driver up, 0x00 off) ---- */
static void tas_set_speaker(bool on)
{
	(void)tas_wr(0x00, 0x01);
	(void)tas_wr(0x2D, on ? 0x02 : 0x00);
	(void)tas_wr(0x00, 0x00);
}

/* Headphones in the jack: CS42L42 DET_STATUS1 (page 0x1B, reg 0x77) bit 7, per
 * Tim Knapen's wiki and the looper. 1 plugged, 0 not, -1 read failed. */
static int hp_detect(void)
{
	uint8_t v;
	if (!tpr(0x1B77, &v)) return -1;
	return (v >> 7) & 1;
}

void audio_set_volume_q8(uint16_t q8)
{
	vol_target = q8 > 256 ? 256 : q8;
}

uint16_t audio_volume_q8(void)
{
	return vol_target;
}

void audio_hp_poll(void)
{
	static int div, cand = -1, cnt;
	if (!cs42_up || ++div < 4) return;
	div = 0;
	int c = hp_detect();
	if (c < 0) return;                     /* a failed read holds */
	if (c != cand) {
		cand = c;
		cnt = 1;
		return;
	}
	if (++cnt >= 3 && c != hp_in) {
		hp_in = (int8_t)c;
		tas_set_speaker(!c);
	}
}

/* ---- power-down: the looper's power_off() order ---- */
void audio_shutdown(void)
{
	/* SYSTEM_OFF stops only the nRF. Left powered, the amp and codec drain the
	 * battery for days and can murmur with no clock. Mute both output stages
	 * first so they discharge quietly, then reset them. */
	(void)tpw(0x2001, 0x0D);           /* CS42L42 HP: mute all */
	(void)tas_wr(0x00, 0x01);
	(void)tas_wr(0x30, 0x00);          /* TAS2505 class-D driver: mute */
	k_msleep(30);
	(void)tas_wr(0x00, 0x00);
	(void)tas_wr(0x01, 0x01);          /* TAS2505 software reset */
	gpio_drive_low(CS42_RST_PORT, CS42_RST_PIN);
	gpio_drive_low(OSC_EN_PORT, OSC_EN_PIN);   /* it draws through SYSTEM_OFF */
}

/* ---- the I2S thread ---- */
static K_THREAD_STACK_DEFINE(audio_stack, 1536);
static struct k_thread audio_tcb;

/* Blocks written before the I2S start: exactly the driver's TX queue, which is
 * also what sets output latency (a new block plays after the ones queued). */
#define AUDIO_PRIME CONFIG_I2S_NRFX_TX_BLOCK_COUNT

static void prime(int n, k_timeout_t wait)
{
	for (int i = 0; i < n; i++) {
		void *blk;
		if (k_mem_slab_alloc(&tx_slab, &blk, wait) != 0) break;
		memset(blk, 0, BLK_BYTES);
		if (i2s_write(i2s_dev, blk, BLK_BYTES) != 0) k_mem_slab_free(&tx_slab, blk);
	}
}

static void audio_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	struct i2s_config cfg = {
		.word_size = 16,   /* 16 MSBs of each 32-SCLK half-frame */
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_FRAME_CLK_SLAVE | I2S_OPT_BIT_CLK_SLAVE,
		.frame_clk_freq = AUDIO_SR,
		.mem_slab = &tx_slab,
		.block_size = BLK_BYTES,
		.timeout = 2000,
	};

	if (!device_is_ready(i2s_dev) || i2s_configure(i2s_dev, I2S_DIR_TX, &cfg) != 0) return;
	prime(AUDIO_PRIME, K_FOREVER);
	(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	i2s_started = true;

	int wfail = 0;
	for (;;) {
		/* STARVATION GUARD (2026-09-24). Main feeds the watchdog; this thread
		 * outranks it. When a render needs the whole CPU, main gets nothing and
		 * the watchdog resets the unit. If main has not fed it for 1.5 s, step
		 * aside for 20 ms: main finishes its pass and feeds it. The I2S queue runs
		 * dry -- a dropout, which the wfail path below recovers -- instead of a
		 * reset. Idle otherwise: main feeds every few ms. A truly hung main still
		 * stops feeding, so the watchdog still catches it. */
		if (wdt_ms_since_feed() > 1500u) k_msleep(20);

		void *blk;
		if (k_mem_slab_alloc(&tx_slab, &blk, K_FOREVER) != 0) continue;
		/* The slot's time is the audio clock's; the push after the render is
		 * late by the render. The USB regulator steers on this claim. */
		usb_audio_claim(AUDIO_BLK_FRAMES);
		render_cb(blk, AUDIO_BLK_FRAMES);
		/* The USB copy was pushed by render_cb itself (main.c): a second output
		 * at its own level, not this block. */
		if (i2s_write(i2s_dev, blk, BLK_BYTES) != 0) {
			k_mem_slab_free(&tx_slab, blk);
			/* A stopped stream fails every write forever: drop, re-prime, restart. */
			if (++wfail >= 8) {
				wfail = 0;
				(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
				prime(AUDIO_PRIME, K_NO_WAIT);
				(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
			}
			continue;
		}
		wfail = 0;
	}
}

void audio_start(audio_render_fn render)
{
	render_cb = render;

	/* The looper's order: resets, osc + I2S + TAS2505, then the CS42L42. */
	gpio_drive_high(CS42_RST_PORT, CS42_RST_PIN);
	gpio_drive_high(TAS_RST_PORT, TAS_RST_PIN);
	k_msleep(20);
	gpio_drive_high(OSC_EN_PORT, OSC_EN_PIN);
	k_msleep(5);

	/* PREEMPTIBLE, on purpose. Zephyr's USB threads (usbd, the nRF driver's) are
	 * K_PRIO_COOP(8) and outrank it, so USB work can land mid-render. Made
	 * K_PRIO_COOP(7) to stop that, a render that needs the whole CPU then starved
	 * main and the USB threads for 5 s and the WATCHDOG RESET the unit. Under
	 * overload a preemptible audio thread fails as a late block -- a glitch --
	 * instead. */
	k_thread_create(&audio_tcb, audio_stack, K_THREAD_STACK_SIZEOF(audio_stack),
			audio_thread, NULL, NULL, NULL, K_PRIO_PREEMPT(0), 0, K_NO_WAIT);
	k_thread_name_set(&audio_tcb, "audio");

	for (int i = 0; i < 100 && !i2s_started; i++) k_msleep(2);
	(void)tas2505_configure();
	hp_codec_init();

	/* Auto-mute boot state, the looper's: a majority of 5 reads, 8 ms apart. */
	if (cs42_up) {
		int votes = 0, reads = 0;
		for (int i = 0; i < 5; i++) {
			int c = hp_detect();
			if (c >= 0) {
				reads++;
				votes += c;
			}
			k_msleep(8);
		}
		hp_in = (reads > 0 && votes * 2 > reads) ? 1 : 0;
		tas_set_speaker(!hp_in);
	}
}
