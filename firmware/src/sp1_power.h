/*
 * wakes-sp1 — watchdog, function-button power control, and the clean path
 * back to the bootloader.
 *
 * THE SP-1 HAS NO HARDWARE RESET PIN. SYSTEM_OFF is the only clean way back
 * to the bootloader from a running app, which is why power-off is implemented
 * in M0 rather than "later". See docs/SAFETY.md.
 */
#ifndef SP1_POWER_H
#define SP1_POWER_H

#include <stdint.h>
#include <stdbool.h>

/* Reload every watchdog channel. Safe whether the WDT was armed by the
 * bootloader or by us. Must be called at least every ~4 s, from every loop
 * that can block. */
void sp1_wdt_feed(void);

/* Read and clear POWER->RESETREAS. Call once, early, before anything else
 * reads it. Returns the value as found at boot. */
uint32_t sp1_resetreas_take(void);

/* Enable battery charging and configure the charger status pins. Call early at
 * boot: without this the charger's nCE may not be asserted and the device will
 * not charge while plugged in. */
void sp1_charger_init(void);

/* ---- no charging while ON with a USB HOST attached (M5a, Adara, from the OP-XY test) ----
 * A USB host that runs on its own battery -- the OP-XY -- was charging the SP-1 from that
 * battery for the whole session, and the SP-1 got warm doing it. So while ON and a host has
 * configured the device (sp1_usbd_host), charging is OFF (nCE high). Everything else is as it
 * always was (Adara): ON on a plain charger charges; STANDBY charges, host or not -- plugging
 * in while off, or powering off while plugged into a host, lands in STANDBY and charges. Every
 * way out of ON turns charging back on: sp1_quiesce_peripherals(), which both STANDBY and
 * SYSTEM_OFF go through, STANDBY's own entry, and sp1_charger_init() at every boot.
 *
 * The BQ24232 is a power-path charger: nCE high disables BATTERY CHARGING only ("connect CE
 * to a high logic level to disable battery charging", datasheet rev. H), and the system keeps
 * running from USB while it is plugged in. So ON with a host neither charges nor drains the
 * battery.
 *
 * ⚠️ Fail-safe direction: charging ON. Boot, a watchdog reset, a fault and every power-off path
 * all leave nCE LOW. Only the ON loop ever raises it. */
void sp1_charger_enable(bool on);
bool sp1_charger_enabled(void);

/* True when USB power is present (nPGOOD low). */
bool sp1_usb_present(void);

/* True while the battery is charging (nCHG low). */
bool sp1_charging(void);

/* Configure the "••" function button as a pulled-up input. */
void sp1_fnc_cfg_input(void);

/* True while "••" is held (the pin reads low). */
bool sp1_fnc_pressed(void);

/* The power-ON hold, shared by the boot path and STANDBY so the gesture looks
 * and feels identical from either.
 *
 * Blocks while "••" is held: silent for SP1_PWR_ON_DARK_MS with NOTHING drawn,
 * then fills the model row over SP1_PWR_ON_FILL_MS as progress.
 *
 * Returns true if the hold completed -- the caller should proceed to ON -- and
 * false if "••" was released early, in which case nothing was drawn if the
 * release happened inside the dark window. On success the shutdown gesture is
 * left DISARMED until "••" is released, so a continuous press cannot roll from
 * power-on straight into power-off.
 *
 * With CONFIG_SP1_DRIVE, every block the computer reads or writes restarts the fill, so
 * ON needs SP1_PWR_ON_FILL_MS without a transfer (M6, #43): turning ON takes the card
 * from the computer. No time limit; the hold blocks for as long as the transfers last.
 *
 * Does not power anything off; the caller decides what an early release means. */
bool sp1_power_on_hold(void);

/* Boot wrapper around the above. Call ONCE at boot, after sp1_led_init().
 *
 * If "••" is not held this is a cold boot / reset / USB insert: arms the shutdown
 * gesture and returns immediately. If "••" IS held we were almost certainly woken
 * by it, so the hold is required and an early release goes back to SYSTEM_OFF.
 *
 * SAFETY NET: the gate only engages when "••" is held AT BOOT, so plugging in USB
 * always boots normally. No bug in the hold logic can make the device
 * unreachable, and T1+T4 at power-on is untouched regardless. */
void sp1_power_on_gate(void);

enum sp1_power_result {
	SP1_PWR_NONE = 0,      /* nothing in progress; the caller owns the LEDs   */
	SP1_PWR_ANIMATING,     /* shutdown animation running; leave the LEDs be   */
	SP1_PWR_TO_STANDBY,    /* completed while PLUGGED -- caller -> STANDBY    */
};

/* Run one tick of the shutdown gesture. Call once per control tick from the main
 * loop, passing the tick period and whether "••" is currently held.
 *
 * The gesture, per the UI spec:
 *   - "••" held SP1_PWR_HOLD_MS with no other control touched -> shutdown begins
 *   - the model row fades UP to full over 500 ms, together
 *   - then fades dark one at a time: 150 ms fade, 50 ms gap
 *   - releasing "••" before the LAST LED finishes CANCELS the shutdown
 *
 * Call sp1_shift_used() from the control layer whenever ANY other control is
 * touched while "••" is held -- a fader moved, any button pressed. That means "••"
 * is being used as a SHIFT, so shutdown is suppressed for the rest of the hold,
 * cancelling the animation if it had already begun. Released and held again, the
 * gesture starts fresh.
 *
 * The shutdown gesture stays disarmed until "••" has been released once after
 * boot. Without that, holding "••" for 2 s to turn the device ON would roll
 * straight into the 3 s shutdown hold and power it back off.
 *
 * Returns SP1_PWR_ANIMATING while the animation runs, so the caller knows not to
 * draw on the model row.
 *
 * On completion: UNPLUGGED it calls sp1_power_off() and never returns. PLUGGED it
 * quiesces the peripherals, disarms the gesture and returns SP1_PWR_TO_STANDBY --
 * no reset is involved, the device is simply already awake. */
enum sp1_power_result sp1_power_tick(uint32_t elapsed_ms, bool fnc_held);

/* ---- PRST's save at shutdown (M6, #50) ----
 * Called once when the ordinary shutdown animation COMPLETES, before the peripherals are
 * quiesced -- on the way to STANDBY and to SYSTEM_OFF alike. Never from the 30 s backstop:
 * the forced power-off does not wait for a save (rule 5a). The hook itself must be bounded
 * and feed the watchdog (main.c's waits at most 4 s). NULL = none. */
void sp1_power_set_save_hook(void (*fn)(void));

/* Tell the shutdown gesture that "••" was used as a modifier during this hold.
 * Suppresses shutdown until "••" is released.
 *
 * ⚠️ Only effective BEFORE the animation starts. Once the shutdown animation is
 * running, the only thing that stops it is releasing "••" -- see the comment in
 * sp1_shift_used() for the risk asymmetry behind that.
 *
 * Returns true the FIRST time it suppresses a given hold, so the caller can log
 * which control was responsible without repeating it every tick. */
bool sp1_shift_used(void);

/* Put the external chips to sleep: codec and amp into reset, eMMC I/O rail and
 * the 3.072 MHz oscillator off. Called on the way to both STANDBY and SYSTEM_OFF.
 * Safe when they were never powered up. */
void sp1_quiesce_peripherals(void);

/* ⚠️ M2 NOTE: this drives the codec/amp resets low and the 3.072 MHz oscillator
 * off, and NOTHING currently turns them back on when leaving STANDBY for ON --
 * there is no audio yet, so nothing notices. When M2 brings up I2S, entering ON
 * must re-release those resets and re-enable the oscillator. */

/* Power the board down and return control to the bootloader. Quiesces the
 * external chips, arms "••" as the wake source, then SYSTEM_OFF. Never
 * returns. Called by sp1_power_tick(); call directly only to force a shutdown
 * with no animation. */
void sp1_power_off(void) __attribute__((noreturn));

#endif /* SP1_POWER_H */
