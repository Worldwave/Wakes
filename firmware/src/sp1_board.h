/*
 * wakes-sp1 — SP-1 hardware pin map.
 *
 * Pin numbers from timknapen/SP-1-dev src/stemplayer_pins.h (MIT).
 * The LED port/pin pairs are the map chattock/sp1-tape-looper (MIT) marks as
 * hardware-verified; note that TimK's header lists the playback LEDs in a
 * different order. Trust the verified map, not the header ordering.
 */
#ifndef SP1_BOARD_H
#define SP1_BOARD_H

#include <zephyr/kernel.h>
#include <soc.h>

/* ---- Function button "••" : direct GPIO, pull-up, active low ---- */
#define SP1_FNC_PORT        NRF_P0
#define SP1_FNC_PIN         27u

/* ---- Button-ladder common rail (powers both resistor ladders) ---- */
#define SP1_BTN_COM_PORT    NRF_P1
#define SP1_BTN_COM_PIN     10u

/* ---- BQ24232 battery charger ----
 * The charger IC runs the CC/CV profile, termination, safety timer and thermal
 * regulation autonomously. Firmware CANNOT set the charge current: ISET is a
 * current-MONITORING output (an ADC input here), not a control. The part is
 * hardware-configured for 500 mA max USB draw. Our whole job is: hold nCE low
 * to enable charging, and read the two status pins. */
#define SP1_BQ_PORT         NRF_P0
#define SP1_BQ_NCE_PIN      21u   /* out, ACTIVE LOW: drive low = charging on   */
#define SP1_BQ_NCHG_PIN     22u   /* in, open-drain, LOW = charging now         */
#define SP1_BQ_NPGOOD_PIN   24u   /* in, open-drain, LOW = USB power present    */
/* ISET P1.00 and BATT_LEVEL P0.28/AIN4 are ADC inputs; used from M1 on. */

/* ---- Reset / enable lines for the chips around the nRF ---- */
#define SP1_TAS_RST_PORT    NRF_P0   /* TAS2505 speaker amp, active low  */
#define SP1_TAS_RST_PIN     9u
#define SP1_CS42_RST_PORT   NRF_P0   /* CS42L42 headphone codec, act low */
#define SP1_CS42_RST_PIN    15u
#define SP1_OSC_EN_PORT     NRF_P0   /* 3.072 MHz audio osc, active high  */
#define SP1_OSC_EN_PIN      13u
#define SP1_EMMC_VCCQ_PORT  NRF_P0   /* eMMC I/O rail, active high        */
#define SP1_EMMC_VCCQ_PIN   14u
/* eMMC bus (M6, #43): 1-bit MMC, bit-banged -- the nRF52840 has no SD/MMC peripheral.
 * CLK, DAT0 and CMD must stay on port 0: sp1_emmc.c clocks data through NRF_P0's
 * registers directly. Used only by sp1_emmc.c. */
#define SP1_EMMC_CLK_PIN    6u       /* P0.06                              */
#define SP1_EMMC_DAT0_PIN   7u       /* P0.07                              */
#define SP1_EMMC_CMD_PIN    8u       /* P0.08                              */
#define SP1_EMMC_RST_PORT   NRF_P1   /* RST_n, active low                  */
#define SP1_EMMC_RST_PIN    8u       /* P1.08                              */
#define SP1_BT_RST_PORT     NRF_P0   /* CYBT module, active low           */
#define SP1_BT_RST_PIN      10u

/* ---- LEDs: owned by PWM, NOT by GPIO ----
 * Both rows are driven by hardware PWM (PWM2 = track row, PWM3 = play row) and
 * their pins are claimed by pinctrl. DO NOT write these pins as GPIO -- it
 * fights the PWM peripheral and produces flicker or a stuck LED. Use the
 * sp1_led.h API.
 *
 * Recorded here only because the pinctrl CHANNEL ORDER in
 * boards/teenageengineering/stem_player/stem_player-pinctrl.dtsi must match the
 * LED indices, and changing one without the other scrambles the display:
 *
 *   track row (PWM2), the "model row" above T1-T4:
 *     ch0 = P0.29   ch1 = P0.26   ch2 = P1.15   ch3 = P1.14
 *   play row (PWM3), the side row / VU meter:
 *     ch0 = P1.13   ch1 = P0.00   ch2 = P1.12   ch3 = P0.01
 *
 * This is the order sp1-tape-looper marks as hardware-verified. TimK's
 * stemplayer_pins.h lists the play LEDs differently -- trust this one. */

#endif /* SP1_BOARD_H */
