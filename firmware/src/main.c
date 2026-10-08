/*
 * ============================================================================
 *  wakes-sp1  —  M0 · M1a (LEDs) · M1b (STANDBY) · M1c (console) · M1d (controls)
 * ============================================================================
 *  This milestone does nothing musical on purpose. It exists to prove four
 *  things on real hardware before a single line of DSP is written:
 *
 *    1. the image builds, flashes, and boots from 0x20000
 *    2. the LEDs are under our control (so later milestones have an output)
 *    3. "••" long-press powers the device down cleanly
 *    4. the device can always be recovered into the bootloader
 *
 *  THE SP-1 HAS NO HARDWARE RESET PIN. If an app hangs without a power-off
 *  path, the only way back is disconnecting the battery, which means opening
 *  the device. That is why (3) is in the first milestone and not "later".
 *
 *  ---- RECOVERY, WHICH DOES NOT DEPEND ON THIS CODE ----
 *  Hold Track 1 + Track 4 while plugging in USB-C. That scan lives in the TE
 *  bootloader at 0x00000-0x1FFFF, which this firmware never writes. A
 *  completely broken app cannot take it away.
 *
 *  ---- THE SP-1 "BIG FIVE" (per chattock/sp1-tape-looper) ----
 *    - app lives at 0x20000
 *    - the watchdog is fed in under 5 s, always
 *    - bootloader-owned clocks and peripherals are NOT re-initialised
 *    - SYSTEM_OFF is the return path to the bootloader
 *    - RESETREAS is cleared at boot and again before SYSTEM_OFF
 *
 *  ---- WHAT YOU SHOULD SEE ----
 *    ---- THE PLAY ROW: dB METER IN FRONT, CLOCK BEHIND (M2b) ----
 *    The dB meter fills from the "••" end toward PLAY. Behind it, dim (~10 %), a
 *    single LED steps from the "••" end toward PLAY four times a second -- a hard
 *    step, no fade -- so ON is visible even in silence. See sp1_playrow.h.
 *
 *    ---- THE MODEL ROW IS A FOUR-FADER METER (M1d) ----
 *    T1's brightness follows fader 1, T2's follows fader 2, T3 follows 3, T4
 *    follows 4 -- all four live, all the time. Push one fader and one LED
 *    brightens. Charger status is NOT on this row in ON any more; STANDBY shows
 *    it, and the console prints usb=/chg=.
 *
 *    Pressing a track button should leave the row alone. A press sags the rail
 *    that feeds the faders, so those samples are discarded and the meter holds.
 *
 *    Turning it ON takes a 1.5 s hold of "••": 0.5 s SILENT, then a 1 s fill on
 *    the model row. A brief brush lands inside the silent window and shows
 *    nothing at all -- that blackout is what fixes the tap-flicker.
 *
 *    ---- STANDBY (M1b) ----
 *    Plugged in and off, the device sits in STANDBY rather than sleeping, because
 *    SYSTEM_OFF with VBUS already high has no wake edge. The play row is a charge
 *    bar filling from the "••" end toward PLAY -- breathing while charging, solid
 *    when complete -- and T2/T3 show plugged / charging, dim.
 *
 *    Hold "••" for 3 s: the model row (T1-T4) fades UP to full together over
 *    500 ms, then fades dark one LED at a time. Keep holding to the end and the
 *    device powers off. RELEASE AT ANY POINT BEFORE THE LAST LED FINISHES AND
 *    THE SHUTDOWN IS CANCELLED. Press "••" again to wake it.
 *
 *    Powering off WHILE PLUGGED IN soft-resets instead of sleeping, so the
 *    heartbeat restarts. That is correct, not a fault: SYSTEM_OFF with VBUS
 *    already high has no wake edge and the device would stay dark until the
 *    cable was pulled. Unplug first if you want it to actually sleep.
 * ============================================================================
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/sys/reboot.h>

#include "sp1_board.h"
#include "sp1_led.h"
#include "sp1_power.h"
#include "sp1_ui_timing.h"
#include "sp1_batt.h"
#include "sp1_standby.h"
#include "sp1_usbd.h"
#include "sp1_console.h"
#include "sp1_logbuf.h"
#include "sp1_threads.h"
#include "sp1_controls.h"
#include "sp1_display.h"
#include "sp1_calib.h"
#include "sp1_audio.h"
#include "sp1_meter.h"
#include "sp1_playrow.h"
#if defined(CONFIG_SP1_STORAGE)
#include "sp1_store.h"
#endif
#if defined(CONFIG_SP1_PLAITS)
#include "sp1_synth.h"
#include "sp1_plaits_ui.h"
#include "sp1_marbles.h"
#include "sp1_marbles_ui.h"
#include "sp1_prst.h"            /* M6 (#50): the ROTC defaults main.c owns */
#include "sp1_release_guard.h"
#include "sp1_midi.h"
#endif
#if defined(CONFIG_SP1_USB_AUDIO)
#include "sp1_uac.h"
#include "sp1_audio_gen.h"      /* config/audio.ini */
#endif

/* ---- USB audio out's level (M5c; config/audio.ini [usb] level) ----
 * `parked`: while a host takes USB audio out (sp1_uac_live), VOL is set to audio.ini's
 * parked_level and its buttons do nothing; when the host stops, VOL goes back to where the
 * user left it. Wakes has ONE output level for every output, so this parks the headphones
 * (and the speaker, if it is on) too -- deliberately: a separate USB level would be a second
 * gain pass over every block (~0.3 % of the CPU, Adara: use this avenue instead). The level
 * slews as VOL always does, so parking does not click. `vol`: nothing here happens.
 * Across ON sessions the state stays, so a session started while a host records is parked
 * on its first tick. */
static bool vol_parked;
#if defined(CONFIG_SP1_USB_AUDIO)
static int vol_saved;

static int usb_parked_db(void)
{
	return SP1_AUDIO_USB_PARKED_DB;
}

static void usb_level_park(bool usb_live)
{
	if (!SP1_AUDIO_USB_LEVEL_PARKED) {
		return;
	}
	if (usb_live && !vol_parked) {
		vol_saved = sp1_audio_level_get();
		sp1_audio_level_step(SP1_AUDIO_USB_PARKED_STEP - vol_saved);
		vol_parked = true;
		printk("LEVEL parked at %d dBFS: a host takes USB audio out, VOL locked"
		       " (config/audio.ini)\n", (int)SP1_AUDIO_USB_PARKED_DB);
	} else if (!usb_live && vol_parked) {
		sp1_audio_level_step(vol_saved - sp1_audio_level_get());
		vol_parked = false;
		printk("LEVEL back to step %d: no host takes USB audio out, VOL unlocked\n",
		       vol_saved);
	}
}
#else
static int usb_parked_db(void) { return 0; }
#endif

#define WDT_NODE DT_ALIAS(watchdog0)

/* ---- "••" tap: the battery on the play row (M4c; the fade is M4e) ----
 * Not Plaits-specific -- it works in the fallback build too, which is the build most likely
 * to be debugging a battery.
 *
 * Two timers, because it leaves three different ways:
 *   batt_flash_ms  counting down while "••" is HELD, capped at SP1_BATT_FLASH_MS
 *   batt_fade_ms   the SP1_BATT_FLASH_FADE_MS fade-out, started by a release OR by
 *                  batt_flash_ms reaching 0
 * and a third: another control touched CUTS both to zero at once.
 *
 * ⚠️ Through M4c there was one timer and a release set it to 0, so the display CUT. Since a
 * tap is under 300 ms against a 1500 ms display, the release always won -- which is why the
 * tail fade written for it was unreachable and Adara saw no fade at all. Two timers is the
 * fix; one timer cannot express "it ended" and "it was cancelled" differently. */
static uint32_t batt_flash_ms;
static uint32_t batt_fade_ms;

/* Control loop period comes from sp1_ui_timing.h, as does every gesture timing. */
#define TICK_MS            SP1_TICK_MS
/* The old heartbeat is now the clock layer of the play row, with its cross-fade
 * removed (M2b): see sp1_playrow.{c,h}. */

#if defined(CONFIG_SP1_PLAITS)
/* FFWD burst subdivision as a power of two: 1/(1 << g_burst_div). Kept across ON
 * sessions (RAM), default 1/32. "••" + FFWD finer, "••" + RWD coarser (M3d). */
#define SP1_BURST_DIV_MAX 7            /* 1/128 */
static uint8_t g_burst_div = SP1_PRST_DEF_BURST;   /* 1/32  */

/* Output select (M3f): OUT / AUX / OUT+AUX / OUTxAUX, cycled by T4 on the PLAITS
 * SETTINGS panel (#11; it was "••" + T4 until v0.4.6). RAM only, like the burst
 * division: a real power-off returns to OUT.
 * Shown by flashing one track LED per mode, T1 = OUT ... T4 = OUTxAUX. */
static uint8_t g_out_mode = SP1_PRST_DEF_OUT;

static const char *const out_mode_name[SP1_OUT_COUNT] = {
	"OUT", "AUX", "OUT+AUX", "OUTxAUX",
};
BUILD_ASSERT(SP1_OUT_COUNT == 4, "one track LED per output mode");

static void faders_raw(uint16_t raw[SP1_NUM_FADERS])
{
	for (int i = 0; i < SP1_NUM_FADERS; i++) {
		raw[i] = sp1_fader_raw(i);
	}
}

/* ---- M4: two modules on one row of buttons (docs/UI-SPEC.md v0.9) ----
 * T4 swaps between them; PLAITS is where the device starts. RAM only. */
enum sp1_module { SP1_MODULE_PLAITS = 0, SP1_MODULE_MARBLES };
static enum sp1_module g_module = SP1_MODULE_PLAITS;
static bool g_seeded;                  /* Marbles' random stream seeded this boot */

/* Per-ON-session control state (reset on every entry to ON). */
static bool     burst_was;             /* FFWD burst running (PLAITS, clock stopped) */
static bool     ffwd_consumed;         /* FFWD pressed with "••": not transport      */
static uint32_t burst_n0;
static uint32_t rip_ms;                /* "••" + PLAY held, towards SP1_RIP_HOLD_MS  */
static bool     rip_armed;             /* this PLAY press may still become ROTC      */
static bool     rip_glyph;             /* ...and it opened the PRST glyph (PRST on)  */
static bool     rip_show_page;         /* rip done, still held: draw the page        */
/* PRST's browser (#50): the slot glyph on show, and whether this "••" hold changed slot. */
static bool     prst_showing;
static uint32_t prst_show_ms;
static bool     prst_changed;
static bool     prst_in_hold;          /* the glyph was opened in THIS "••" hold */
static uint32_t beats_seen;            /* Marbles t2 ticks already shown             */
static uint32_t trig_print0;           /* TRIG edges at the last AUD line            */

static const char *const t_range_name[3] = { "x0.25", "x1", "x4" };
static const char *const x_range_name[SP1_MRB_RANGE_COUNT] = {
	"0..2 V", "0..5 V", "-5..+5 V", "INTELLIGENT",
};
static const char *const diversity_name[3] = { "identical", "bump", "tilt" };

/* ================= UNPATCH: "••" held + Tn held (Adara, M4e) =================
 * The one long press in this UI, and deliberately so: it is destructive, it reaches ONE
 * cable rather than the whole patch, and it must be hard to do by accident.
 *
 *   PLAITS   T1-T4 name a PARAMETER -- FREQUENCY, TIMBRE, MORPH, HARMONICS, in fader
 *            order under the four SHIFT attenuverters -- and every Marbles output aimed
 *            at it is disconnected.
 *   MARBLES  T1-T3 name an OUTPUT (t1-t3 on the t page, X1-X3 on the X page) and T4
 *            names Y; that output's destination goes to `none`.
 *
 * ⚠️ FREQUENCY covers BOTH V/Oct and FM, because both of them modulate the pitch and
 * V/Oct has no other button to live under. So T1 is "stop anything changing my pitch",
 * which is the useful thing to be able to say in one gesture.
 *
 * ⚠️ Because T1-T4 now carry two meanings under "••", their ORDINARY action moved to the
 * button's RELEASE and fires only if Unpatch did not (`unpatch_eat`). Unavoidable with
 * two meanings on one button, and it is why every shift-layer T-button below uses
 * sp1_button_released() instead of sp1_button_pressed().
 *
 * ⚠️ It cannot weaken the power path. "••" + a button is already a shift use, so the 3 s
 * shutdown is suppressed exactly as before, and the SP1_PWR_FORCE_HOLD_MS backstop is
 * unconditional and untouched (rule 5a). 2 s against 30 s cannot be confused.
 *
 * The animation is Adara's, to the millisecond -- fade out, two 80 % blinks, a third that
 * latches to 100 %, then a sweep dark from the middle outwards, all as fades. It starts at
 * SP1_UNPATCH_START_MS and the clear commits when it ENDS. Letting go earlier cancels and
 * clears nothing, like every other hold here. */
static uint32_t unpatch_ms;             /* how long "••" + the tracked button is held */
static int      unpatch_btn = -1;       /* 0..3 = T1..T4, -1 = nothing being held     */
/* The button whose hold committed: its release is eaten, however long after the commit
 * it comes (issue #2 -- see sp1_release_guard.h). */
static struct sp1_release_guard unpatch_eat = { -1 };

/* The four track LEDs at `t` ms into the animation (0 .. SP1_UNPATCH_ANIM_MS). */
static void unpatch_levels(uint32_t t, uint8_t lv[4])
{
	const uint32_t B = SP1_UNPATCH_BLINK_LEVEL;
	uint32_t mid = 0u, out = 0u;      /* T2/T3 and T1/T4 -- the sweep separates them */
	uint32_t p0 = SP1_UNPATCH_OUT_MS;
	uint32_t v;

	if (t < p0) {
		/* Fade whatever was showing down to black. We do not know what that was
		 * (the row is a live meter), so fade from full: the cross-fade in
		 * sp1_display_flash covers the difference and the eye reads it as "off". */
		v = 255u - (t * 255u) / p0;
		mid = out = v;
	} else if (t < p0 + 4u * SP1_UNPATCH_BLINK_MS) {
		/* Two blinks to 80 %: up, down, up, down. */
		const uint32_t k = (t - p0) / SP1_UNPATCH_BLINK_MS;
		const uint32_t f = (t - p0) % SP1_UNPATCH_BLINK_MS;
		v = (k & 1u) ? B - (f * B) / SP1_UNPATCH_BLINK_MS
			     : (f * B) / SP1_UNPATCH_BLINK_MS;
		mid = out = v;
	} else if (t < p0 + 4u * SP1_UNPATCH_BLINK_MS + SP1_UNPATCH_LATCH_MS) {
		/* The third blink, which does not come back down: 0 -> 100 % and hold. */
		const uint32_t f = t - p0 - 4u * SP1_UNPATCH_BLINK_MS;
		v = (f * 255u) / SP1_UNPATCH_LATCH_MS;
		mid = out = v > 255u ? 255u : v;
	} else {
		/* Sweep dark from the middle outwards: T2/T3 fade first, then T1/T4, each
		 * over half the sweep, so the darkness travels rather than just dimming. */
		const uint32_t f = t - p0 - 4u * SP1_UNPATCH_BLINK_MS - SP1_UNPATCH_LATCH_MS;
		const uint32_t half = SP1_UNPATCH_SWEEP_MS / 2u;
		if (f < half) {
			mid = 255u - (f * 255u) / half;
			out = 255u;
		} else {
			const uint32_t g = f - half;
			mid = 0u;
			out = g < half ? 255u - (g * 255u) / half : 0u;
		}
	}
	lv[0] = (uint8_t)out;
	lv[1] = (uint8_t)mid;
	lv[2] = (uint8_t)mid;
	lv[3] = (uint8_t)out;
}

/* ---- PRST's slot glyph (Adara, #50) ----
 * One LED per slot, T1..T4: that LED ramps 100 % -> 0 every SP1_PRST_RAMP_MS (10 Hz), the
 * other three face LEDs BLACK (Adara) -- sp1_display_flash() is opaque while it holds, so
 * nothing of the page shows through. The play row carries on as normal (Adara: face LEDs
 * only). `t` = ms since it appeared. */
static void prst_glyph_levels(int slot, uint32_t t, uint8_t lv[4])
{
	const uint32_t f = t % SP1_PRST_RAMP_MS;
	for (int i = 0; i < 4; i++) {
		lv[i] = (i == slot) ? (uint8_t)(255u - (f * 255u) / SP1_PRST_RAMP_MS) : 0u;
	}
}

/* ---- ROTC's animation past the glyph (Adara, #50): t = ms since PLAY went down ----
 * black SP1_ROTC_BLACK_MS, fade up to full over SP1_ROTC_RISE_MS, then the Unpatch
 * animation; the wipe commits at SP1_RIP_HOLD_MS (sp1_ui_timing.h checks the sum). */
BUILD_ASSERT(SP1_ROTC_UNPATCH_AT + SP1_UNPATCH_ANIM_MS == SP1_RIP_HOLD_MS,
	     "ROTC: glyph + black + rise + Unpatch must be SP1_RIP_HOLD_MS (Adara: still 3 s)");
BUILD_ASSERT(SP1_PRST_GLYPH_MS == 16u * SP1_PRST_RAMP_MS, "16 ramps (Adara)");

static void rotc_levels(uint32_t t, uint8_t lv[4])
{
	const uint32_t rise_at = SP1_PRST_GLYPH_MS + SP1_ROTC_BLACK_MS;
	if (t >= SP1_ROTC_UNPATCH_AT) {
		unpatch_levels(t - SP1_ROTC_UNPATCH_AT, lv);
		return;
	}
	const uint32_t v = (t < rise_at) ? 0u : ((t - rise_at) * 255u) / SP1_ROTC_RISE_MS;
	for (int i = 0; i < 4; i++) {
		lv[i] = (uint8_t)(v > 255u ? 255u : v);
	}
}

/* ---- ROTC with PRST off (Adara, #50): a slower fade where the glyph would be ----
 * No slot to show, so the first SP1_PRST_GLYPH_MS are a slow fade of the face LEDs from what
 * the page shows (the SHIFT layer, "••" being held) down to black; then the same black, rise
 * and Unpatch animation as with PRST on. Past the glyph's span it is rotc_levels(). */
static void rotc_or_fade_levels(uint32_t t, bool marbles, uint8_t lv[4])
{
	if (t >= SP1_PRST_GLYPH_MS) {
		rotc_levels(t, lv);
		return;
	}
	(marbles ? sp1_mui_leds : sp1_pui_leds)(lv);
	const uint32_t k = 256u - (t * 256u) / SP1_PRST_GLYPH_MS;
	for (int i = 0; i < 4; i++) {
		lv[i] = (uint8_t)(((uint32_t)lv[i] * k) >> 8);
	}
}

#if defined(CONFIG_SP1_PLAITS)
/* The tempo the synth's FFWD burst uses while Marbles is stopped (running, the burst locks to
 * Marbles' ramp): Marbles' RATE, or the host's beat while MIDI clocks Marbles (M5b). */
static float tempo_bpm(void)
{
#if defined(CONFIG_SP1_MIDI)
	struct sp1_midi_stats ms;
	sp1_midi_get_stats(&ms);
	if (ms.clock_ext && ms.bpm10 != 0u) {
		return (float)ms.bpm10 * 0.1f;
	}
#endif
	return sp1_mui_bpm();
}
#endif

/* ================= the MIDI prompt (M5a, Adara; M5 plan B9) =================
 * MIDI plugged in: the Unpatch animation REVERSED -- light gathers from T1/T4 inwards, drops,
 * blinks, and rises to full: the cable going in. Unplugged: the Unpatch animation as it is,
 * because a disconnect really is every MIDI cable coming out (everything MIDI did goes back
 * to neutral). The picture is unpatch_levels() run backwards or forwards, so the two can never
 * drift apart.
 *
 * "Plugged in" = the HOST ENABLED the MIDI port, not the first message: the first note would
 * otherwise fire a 750 ms animation over the playing, and a charger never enumerates so it
 * never prompts. Held SP1_MIDI_PROMPT_SETTLE_MS first, because hosts reset the bus while
 * enumerating; and "unplugged" is only shown after "plugged in" was, so a flap cannot play it
 * alone. Entering ON with a host attached shows "plugged in" once (C10).
 *
 * ⚠️ Cosmetic and nothing else: it draws through sp1_display_flash like every other overlay,
 * so the shutdown animation and the backstop warning always win (rule 5a), and a rip or an
 * Unpatch hold on the same row makes it stand down. */
#if defined(CONFIG_SP1_MIDI)
static uint32_t midi_up_ms;            /* how long the port has been up, capped      */
static bool     midi_prompted;         /* "plugged in" shown for this connection     */
static int      midi_anim;             /* +1 plugged in (reversed), -1 unplugged, 0  */
static uint32_t midi_anim_ms;
static bool     midi_port_was;
static bool     midi_active_was;
static int      midi_eslot_was;        /* the engine PLAYING at the last tick */
static int      midi_sel_was;          /* ...and the one T2/T3 selected        */
static uint32_t midi_drop0;            /* queue drops before this ON (device was off) */
static bool     midi_clk_was;          /* Marbles was on MIDI's clock at the last tick */
static uint32_t midi_tp_seen[5];       /* starts, continues, stops, MMC play / stop logged */

/* The prompt's state on entry to ON. (MIDI itself was reset before audio started.) */
static void midi_enter(void)
{
	midi_up_ms = 0u;
	midi_prompted = false;
	midi_anim = 0;
	midi_port_was = sp1_midi_port_up();
	midi_active_was = false;
	midi_eslot_was = sp1_pui_eslot();
	midi_sel_was = sp1_pui_slot();
	{
		struct sp1_midi_stats ms;
		sp1_midi_get_stats(&ms);
		midi_drop0 = ms.dropped;
		midi_clk_was = false;
		midi_tp_seen[0] = ms.starts;
		midi_tp_seen[1] = ms.continues;
		midi_tp_seen[2] = ms.stops;
		midi_tp_seen[3] = ms.mmc_play;
		midi_tp_seen[4] = ms.mmc_stop;
	}
}

/* One control tick. `busy` = something else owns the track row (shutdown, rip, Unpatch). */
static void midi_tick(uint32_t dt, bool busy)
{
	const bool up = sp1_midi_port_up();
	if (up != midi_port_was) {
		midi_port_was = up;
		/* The script's pickup (config/midi.ini), so a log says which one was flashed. */
		static const char *const pickup[] = {
			[SP1_MIDI_PICKUP_SUM]      = "sum: CCs offset the faders",
			[SP1_MIDI_PICKUP_SHARED]   = "shared: CCs and faders share each value",
			[SP1_MIDI_PICKUP_TAKEOVER] = "takeover: faders with a CC rest until unplugged",
		};
		if (up) {
			printk("MIDI port enabled by the host (pickup %s)\n",
			       pickup[SP1_MIDI_PICKUP]);
			/* A host can start Marbles before PLAY ever has (M5b), so seed its
			 * random stream now if PLAY has not: the moment a host enumerates
			 * varies as much as the moment of a first PLAY. */
			if (!g_seeded && !sp1_marbles_running()) {
				sp1_marbles_seed(k_cycle_get_32());
				g_seeded = true;
			}
		} else {
			printk("MIDI port gone\n");
		}
	}
	const bool active = sp1_midi_active();
	if (active != midi_active_was) {
		midi_active_was = active;
		printk("MIDI %s\n", active
		       ? "active: notes and CCs reach Wakes, FREQUENCY quantizer bypassed"
		       : "neutral: notes released, offsets back to zero");
	}

	/* ---- MIDI clock and transport (M5b): log what the host did to Marbles ---- */
	{
		struct sp1_midi_stats ms;
		sp1_midi_get_stats(&ms);
		if (ms.clock_ext != midi_clk_was) {
			midi_clk_was = ms.clock_ext;
			printk("CLOCK %s\n", ms.clock_ext
			       ? "external: Marbles follows MIDI clock, RATE picks the ratio"
			       : "own: Marbles back on its RATE tempo");
		}
		if (ms.starts != midi_tp_seen[0]) {
			midi_tp_seen[0] = ms.starts;
			sp1_playrow_clock_reset();
			printk("CLOCK run (MIDI Start, beat 1): %u.%u BPM\n", ms.bpm10 / 10u,
			       ms.bpm10 % 10u);
		}
		if (ms.continues != midi_tp_seen[1]) {
			midi_tp_seen[1] = ms.continues;
			printk("CLOCK run (MIDI Continue)\n");
		}
		if (ms.stops != midi_tp_seen[2]) {
			midi_tp_seen[2] = ms.stops;
			printk("CLOCK stop (MIDI Stop)\n");
		}
		/* MMC (M5b round 2): the OP-XY's transport. With MIDI clock arriving, Play
		 * waits for beat 1 like Start (its CLOCK run line follows); without, Marbles
		 * plays on its own RATE tempo. */
		if (ms.mmc_play != midi_tp_seen[3]) {
			midi_tp_seen[3] = ms.mmc_play;
			printk("CLOCK MMC Play: %s\n", ms.clock_ext
			       ? "waiting for beat 1 on the host's clock"
			       : "run on Marbles' own RATE tempo (no MIDI clock arriving)");
			if (!ms.clock_ext) {
				sp1_playrow_clock_reset();
			}
		}
		if (ms.mmc_stop != midi_tp_seen[4]) {
			midi_tp_seen[4] = ms.mmc_stop;
			printk("CLOCK stop (MMC Stop)\n");
		}
	}

	/* ---- MODEL moved by MIDI: the engine flash, exactly as T2/T3 draw it (Adara) ----
	 * Same call, so the same hold and fade -- one animation for "the engine changed",
	 * whatever changed it. Only when the engine PLAYING changed while the selection did
	 * not: a T2/T3 press draws its own flash. Any module, so a change made by the host is
	 * seen even from the Marbles pages. Stands down while something else owns the row. */
	const int es = sp1_pui_eslot();
	const int sel = sp1_pui_slot();
	if (es != midi_eslot_was && sel == midi_sel_was) {
		printk("MODEL %s (MIDI)\n", sp1_pui_engine_name());
		if (!busy) {
			uint8_t g[4];
			sp1_pui_engine_leds(g);
			sp1_display_engine(g);
		}
	}
	midi_eslot_was = es;
	midi_sel_was = sel;

	if (up) {
		if (midi_up_ms < SP1_MIDI_PROMPT_SETTLE_MS) {
			midi_up_ms += dt;
		}
	} else {
		midi_up_ms = 0u;
	}
	if (up && !midi_prompted && midi_up_ms >= SP1_MIDI_PROMPT_SETTLE_MS) {
		midi_prompted = true;
		midi_anim = busy ? 0 : 1;
		midi_anim_ms = 0u;
	} else if (!up && midi_prompted) {
		midi_prompted = false;
		midi_anim = busy ? 0 : -1;
		midi_anim_ms = 0u;
	}
	if (midi_anim == 0) {
		return;
	}
	if (busy) {
		midi_anim = 0;                 /* the hold owns the row: drop, not queue */
		return;
	}
	uint8_t lv[4];
	midi_anim_ms += dt;
	if (midi_anim_ms >= SP1_UNPATCH_ANIM_MS) {
		/* Ends where its last frame is -- full for "plugged in", dark for
		 * "unplugged" -- and fades back to the page from there. */
		unpatch_levels(midi_anim > 0 ? 0u : SP1_UNPATCH_ANIM_MS, lv);
		sp1_display_flash(lv, 0u, SP1_UNPATCH_FADEBACK_MS);
		midi_anim = 0;
		return;
	}
	unpatch_levels(midi_anim > 0 ? SP1_UNPATCH_ANIM_MS - midi_anim_ms : midi_anim_ms, lv);
	sp1_display_flash(lv, 100u, SP1_UNPATCH_CANCEL_MS);
}
#endif

/* Which T1-T4 is held with "••", or -1. Only ONE at a time: the ladder decodes single
 * presses only (a chord reads as nothing pressed), so this cannot be ambiguous. */
static int unpatch_held_button(void)
{
	for (int i = 0; i < 4; i++) {
		if (sp1_button_held((enum sp1_button)(SP1_BTN_T1 + i))) {
			return i;
		}
	}
	return -1;
}

/* ---- PLAITS' FREQUENCY quantizer (M4b): the SETTINGS page's own scale ----
 * Plaits keeps its OWN index into the same seven scales Marbles offers, plus an OFF
 * entry, and only INCLUDED scales are reachable (docs/MARBLES-SETTINGS.md owns the
 * table). No wrap, like every other stepper on this device. */
static const char *plaits_scale_name(void)
{
	const int s = sp1_pui_scale();
	return s < 0 ? "off (every semitone)" : sp1_marbles_scale_name(s);
}

static void plaits_scale_glyph(uint8_t lv[4])
{
	const int s = sp1_pui_scale();
	if (s < 0) {
		lv[0] = lv[1] = lv[2] = lv[3] = 0u;    /* ○○○○ = off */
	} else {
		sp1_mui_scale_glyph(s, lv);
	}
}

static void plaits_scale_step(int dir)
{
	int s = sp1_pui_scale();
	for (;;) {
		s += dir > 0 ? 1 : -1;
		if (s < SP1_PUI_SCALE_OFF || s >= SP1_MUI_SCALES) {
			printk("QUANTIZE %s (end)\n", plaits_scale_name());
			return;
		}
		if (s == SP1_PUI_SCALE_OFF || sp1_mui_scale_included(s)) {
			break;
		}
	}
	sp1_pui_set_scale(s);
	uint8_t slv[4];
	plaits_scale_glyph(slv);
	sp1_display_engine(slv);

	/* ---- the interlock (Adara, M4b) ----
	 * A quantizer and a Marbles output both deciding the pitch is never wanted. Turning
	 * a scale ON therefore UNPATCHES every V/Oct route. (The other direction, routing to
	 * V/Oct, turns the scale off and opens the range -- see marbles_buttons.) */
	if (s != SP1_PUI_SCALE_OFF) {
		const int n = sp1_mui_unpatch_voct();
		if (n > 0) {
			printk("QUANTIZE %s: unpatched %d V/Oct route%s\n",
			       plaits_scale_name(), n, n == 1 ? "" : "s");
			return;
		}
	}
	printk("QUANTIZE %s\n", plaits_scale_name());
}

/* PLAITS module buttons, except PLAY, T4 (module) and FFWD's burst, which main handles for
 * both modules. */
static void plaits_buttons(bool fnc, bool running, uint32_t dt)
{
	ARG_UNUSED(dt);

	/* ---- T1-T3 differ between the face page and the SETTINGS panel (M4b) ----
	 * ⚠️ Nothing on this module used to be page-aware, so on SETTINGS the engine could
	 * be changed by accident. Adara: make SETTINGS a less fragile panel -- the engine
	 * is not reachable there, T4 is not the module swap there, and tapping "••" is the
	 * exit. */
	const bool settings = (sp1_pui_page() == SP1_PUI_SETTINGS);
	const int step = !fnc && sp1_button_pressed(SP1_BTN_T3) ? 1
		       : !fnc && sp1_button_pressed(SP1_BTN_T2) ? -1 : 0;

	if (!settings) {
		/* T1: flash the engine we are on -- "what am I playing?" (Adara, M4b). */
		if (!fnc && sp1_button_pressed(SP1_BTN_T1)) {
			uint8_t elv[4];
			sp1_pui_engine_leds(elv);
			sp1_display_engine(elv);
			printk("ENGINE slot %d: %s (plaits %d)  [shown]%s\n",
			       sp1_pui_eslot() + 1, sp1_pui_engine_name(), sp1_pui_engine(),
			       sp1_pui_eslot() != sp1_pui_slot() ? "  (MODEL offset)" : "");
		}
		/* T2 previous engine, T3 next (UI-SPEC). */
		if (step != 0) {
			const int sl = sp1_pui_engine_step(step);
			uint8_t elv[4];
			sp1_pui_engine_leds(elv);    /* the engine PLAYING (MODEL offset included) */
			sp1_display_engine(elv);
			if (sp1_pui_eslot() != sl) {
				printk("ENGINE slot %d selected, slot %d playing: %s (plaits %d)"
				       "  (MODEL offset)\n", sl + 1, sp1_pui_eslot() + 1,
				       sp1_pui_engine_name(), sp1_pui_engine());
			} else {
				printk("ENGINE slot %d: %s (plaits %d)\n", sl + 1,
				       sp1_pui_engine_name(), sp1_pui_engine());
			}
		}
	} else {
		/* ---- SETTINGS: T1 shows the scale, T2 / T3 select it, T4 the output ---- */
		if (!fnc && sp1_button_pressed(SP1_BTN_T1)) {
			uint8_t slv[4];
			plaits_scale_glyph(slv);
			sp1_display_engine(slv);
			printk("QUANTIZE %s  [shown]\n", plaits_scale_name());
		}
		if (step != 0) {
			plaits_scale_step(step);
		}
		/* T4: output select, cycles forward and wraps (M3f). Moved here from
		 * "••" + T4 (#11), which left the shift layer's T4 to Unpatch alone. On
		 * PRESS: nothing else lives on this button on this panel. */
		if (!fnc && sp1_button_pressed(SP1_BTN_T4)) {
			g_out_mode = (uint8_t)((g_out_mode + 1u) % SP1_OUT_COUNT);
			sp1_synth_set_output((enum sp1_synth_output)g_out_mode);
			uint8_t sel[4] = { 0u, 0u, 0u, 0u };
			sel[g_out_mode] = SP1_ENGINE_LED_FULL;
			sp1_display_engine(sel);
			printk("OUTPUT %s\n", out_mode_name[g_out_mode]);
		}
	}

	/* ---- RWD: one TRIG, on press -- its own, whether or not Marbles runs (M4) ---- */
	if (!fnc && sp1_button_pressed(SP1_BTN_RWD)) {
		sp1_synth_trigger();
		printk("TRIG RWD%s\n", running ? " (with Marbles)" : "");
	}

	/* "••" + T4 is unbound on PLAITS: held, it is Unpatch-HARMONICS and nothing else
	 * (#11). Output select moved to SETTINGS T4, above. */

	/* ---- "••" + FFWD / RWD: the division, 1/1 .. 1/128 (M3d). It sets the burst
	 * (running or stopped since M4e). Not during a burst: FFWD and RWD are one rocker
	 * (Adara, M3d). ---- */
	int dstep = 0;
	if (fnc && sp1_button_pressed(SP1_BTN_FFWD)) {
		dstep = 1;
		ffwd_consumed = true;
	} else if (fnc && !burst_was && sp1_button_pressed(SP1_BTN_RWD)) {
		dstep = -1;
	}
	if (dstep != 0) {
		int d = (int)g_burst_div + dstep;
		d = d < 0 ? 0 : (d > SP1_BURST_DIV_MAX ? SP1_BURST_DIV_MAX : d);
		g_burst_div = (uint8_t)d;
		sp1_synth_set_burst_div(1u << g_burst_div);
		uint8_t bar[4];
		for (int i = 0; i < 4; i++) {
			/* idx 0 = half T1, 1 = full T1, 2 = full + half ... */
			const int lvl = (int)g_burst_div + 1 - 2 * i;
			bar[i] = lvl >= 2 ? SP1_ENGINE_LED_FULL
			       : lvl == 1 ? SP1_ENGINE_LED_HALF : 0u;
		}
		sp1_display_engine(bar);
		printk("DIVISION 1/%u (the burst's grid, locked to the clock while running)\n",
		       1u << g_burst_div);
	}
}

/* The other half of the interlock (Adara, M4b): a Marbles output taking over V/Oct opens
 * Plaits' FREQUENCY range fully and turns its quantizer OFF, so that output predictably
 * controls the pitch. Note the asymmetry Adara called out -- LOCKING the octave range does
 * NOT unpatch anything, because a locked range and an external pitch source are a
 * perfectly sensible combination; a QUANTIZER and one are not.
 * ⚠️ "Opens fully" means mode 10, WITH its centre detent, even from OCTV's bottom position
 * (full range, detent off -- issue #18). Adara: always the same result, so nobody is left
 * without the detent by a routing change and has to go into SETTINGS to get it back. */
static void voct_took_over(uint8_t dest)
{
	if (dest != SP1_DEST_VOCT) {
		return;
	}
	sp1_pui_set_octave_max();
	if (sp1_pui_scale() != SP1_PUI_SCALE_OFF) {
		sp1_pui_set_scale(SP1_PUI_SCALE_OFF);
		printk("QUANTIZE off, FREQUENCY range max: V/Oct is driven by Marbles\n");
	} else {
		printk("FREQUENCY range max: V/Oct is driven by Marbles\n");
	}
}

/* MARBLES module buttons, except PLAY and T4 (module), handled in main. */
/* "••" + T4 on either Marbles page: Y's destination (M4e -- it was SETTINGS T1). On
 * RELEASE, because T4 is also Unpatch-Y. */
static void marbles_y_dest_button(void)
{
	uint8_t lv[4];
	if (!sp1_button_released(SP1_BTN_T4) || sp1_rg_eats(&unpatch_eat, 3)) {
		return;
	}
	sp1_mui_dest_step(3);
	sp1_mui_dest_pattern(3, lv);
	sp1_display_engine(lv);
	struct sp1_mui_routing r;
	sp1_mui_routing(&r);
	printk("ROUTE Y -> %s\n", sp1_mui_dest_name(r.dest[3]));
	voct_took_over(r.dest[3]);
}

static void marbles_buttons(bool fnc, uint32_t dt)
{
	uint8_t lv[4];

	if (fnc) {
		if (sp1_mui_page() == SP1_MUI_PAGE_T) {
			/* t SHIFT: T1-T3 = t1-t3 destination (M4a: these were TRIG on/off
			 * toggles; they cycle the same ring the X side does, with TRIG as
			 * the default and LEVEL added). T4 = Y destination (M4e). */
			for (int t = 0; t < 3; t++) {
				if (sp1_button_released((enum sp1_button)(SP1_BTN_T1 + t)) &&
				    !sp1_rg_eats(&unpatch_eat, t)) {
					sp1_mui_t_dest_step(t);
					sp1_mui_t_dest_pattern(t, lv);
					sp1_display_engine(lv);
					struct sp1_mui_routing r;
					sp1_mui_routing(&r);
					printk("ROUTE t%d -> %s\n", t + 1,
					       sp1_mui_dest_name(r.t_dest[t]));
				}
			}
			marbles_y_dest_button();
		} else {
			/* X SHIFT: T1-T3 = X1-X3 destination; T4 = Y destination (M4e --
			 * [J] moved to the SETTINGS page and now covers X and Y together);
			 * FFWD / RWD = the next / previous included scale (M4a). */
			for (int x = 0; x < 3; x++) {
				if (sp1_button_released((enum sp1_button)(SP1_BTN_T1 + x)) &&
				    !sp1_rg_eats(&unpatch_eat, x)) {
					sp1_mui_dest_step(x);
					sp1_mui_dest_pattern(x, lv);
					sp1_display_engine(lv);
					struct sp1_mui_routing r;
					sp1_mui_routing(&r);
					printk("ROUTE X%d -> %s\n", x + 1,
					       sp1_mui_dest_name(r.dest[x]));
					voct_took_over(r.dest[x]);
				}
			}
			marbles_y_dest_button();
			const int sstep = sp1_button_pressed(SP1_BTN_FFWD) ? 1
					: sp1_button_pressed(SP1_BTN_RWD) ? -1 : 0;
			if (sstep != 0) {
				/* Whether it moved or not, this press was not transport. */
				ffwd_consumed = true;
				if (sp1_mui_scale_step(sstep)) {
					sp1_mui_scale_pattern(lv);
					sp1_display_engine(lv);
					printk("SCALE %d: %s\n", sp1_mui_scale() + 1,
					       sp1_marbles_scale_name(sp1_mui_scale()));
				} else {
					printk("SCALE %s (end)\n",
					       sp1_marbles_scale_name(sp1_mui_scale()));
				}
			}
		}
		return;
	}

	/* An FFWD press on MARBLES is [B], never the start of a burst -- even if T4 is
	 * pressed while it is still held. */
	if (sp1_button_pressed(SP1_BTN_FFWD)) {
		ffwd_consumed = true;
	}

	/* [B] on RWD / FFWD: clock range, no wrap (Adara). Every Marbles page. */
	const int rstep = sp1_button_pressed(SP1_BTN_FFWD) ? 1
			: sp1_button_pressed(SP1_BTN_RWD) ? -1 : 0;
	if (rstep != 0) {
		if (sp1_mui_t_range_step(rstep)) {
			static const uint8_t F = SP1_ENGINE_LED_FULL;
			const int r = sp1_mui_t_range();
			lv[0] = r == 0 ? F : 0u;
			lv[1] = lv[2] = r == 1 ? F : 0u;
			lv[3] = r == 2 ? F : 0u;
			sp1_display_engine(lv);
			printk("RANGE t %s: %d BPM\n", t_range_name[sp1_mui_t_range()],
			       (int)sp1_mui_bpm());
		} else {
			printk("RANGE t %s (end)\n", t_range_name[sp1_mui_t_range()]);
		}
	}

	if (sp1_mui_settings()) {
		/* ---- SETTINGS: T1 free, T2 = [F], T3 = [G], T4 = [J] for X AND Y ----
		 * ⚠️ T1 was Y's destination until M4e; it moved to "••" + T4 on the t and X
		 * pages, beside the other six destination buttons where it belongs. T1 is
		 * now unbound and reserved -- do not put anything on it without asking. */
		/* [F] / [G]: does the one DEJA VU knob apply to the t / X side? (M4b) */
		for (int side = 0; side < 2; side++) {
			if (sp1_button_pressed((enum sp1_button)(SP1_BTN_T2 + side))) {
				sp1_mui_deja_vu_toggle(side);
				sp1_mui_deja_vu_pattern(lv);
				sp1_display_engine(lv);
				printk("DEJA VU %s %s  (t %s, X %s)\n",
				       side == 0 ? "t" : "X",
				       sp1_mui_deja_vu(side) ? "on" : "off",
				       sp1_mui_deja_vu(0) ? "on" : "off",
				       sp1_mui_deja_vu(1) ? "on" : "off");
			}
		}
		/* [J], ONE range for X and Y (M4e, Adara): in Marbles' own manual Y's range
		 * is this same button with a modifier held, so two settings were never in the
		 * hardware being copied. */
		if (sp1_button_pressed(SP1_BTN_T4)) {
			sp1_mui_range_step();
			sp1_mui_range_pattern(sp1_mui_range(), lv);
			sp1_display_engine(lv);
			printk("RANGE X+Y %s\n", x_range_name[sp1_mui_range()]);
		}
		return;
	}

	/* ---- BASE ---- */
	if (sp1_button_pressed(SP1_BTN_T2) || sp1_button_pressed(SP1_BTN_T3)) {
		const enum sp1_mui_page p = sp1_button_pressed(SP1_BTN_T3)
					    ? SP1_MUI_PAGE_X : SP1_MUI_PAGE_T;
		sp1_mui_set_page(p);
		sp1_mui_page_pattern(lv);
		sp1_display_engine(lv);
		printk("MARBLES %s page\n", p == SP1_MUI_PAGE_X ? "X" : "t");
	}

	if (sp1_mui_page() == SP1_MUI_PAGE_X) {
		/* [N] diversity */
		if (sp1_button_pressed(SP1_BTN_T1)) {
			sp1_mui_diversity_step();
			static const uint8_t F = SP1_ENGINE_LED_FULL;
			const int d = sp1_mui_diversity();
			/* how X1..X3 follow the knobs: all alike / the middle one /
			 * the ends opposite */
			lv[0] = (d == 0 || d == 2) ? F : 0u;
			lv[1] = (d == 0 || d == 1) ? F : 0u;
			lv[2] = (d == 0 || d == 2) ? F : 0u;
			lv[3] = 0u;
			sp1_display_engine(lv);
			printk("DIVERSITY %s\n", diversity_name[d]);
		}
		return;
	}

	/* [E] on the t page: one press, the next of Marbles' six models. Marbles hides
	 * the second three behind a 2 s hold of [E]; M4a appends them to the same ring
	 * and drops the hold -- there is no long press anywhere in this UI (Adara). */
	if (sp1_button_pressed(SP1_BTN_T1)) {
		sp1_mui_model_tap();
		sp1_mui_model_pattern(lv);
		sp1_display_engine(lv);
		printk("MODEL %d: %s\n", sp1_mui_model() + 1,
		       sp1_marbles_model_name(sp1_mui_model()));
	}
	ARG_UNUSED(dt);
}
#endif

/* Survives a warm reset so a crash leaves a trace for the next boot. */
#if defined(CONFIG_SP1_FRESH)
/* ---- the fresh format holds ON entry (M6 #43, Adara 2026-10-07) ----
 * "Formatting the eMMC should hold up the rest of the UI while the device is being turned
 * on. We can still turn the SP-1 off during the process in case it hangs, using the 30s
 * SHFT hold backstop." So between queuing the storage job and starting audio, main waits
 * here: through the check (~0.1 s, nothing drawn), and through a format (several seconds)
 * with the track row as a progress bar, ending in two 0 -> 100 % flickers and a quick
 * fade into the page (sp1_ui_timing.h). A normal image never formats and never waits.
 *
 * ⚠️ POWER: sp1_power_tick() runs every tick with "••", so the 30 s backstop works exactly
 * as in the ON loop -- it is evaluated first and unconditionally (rule 5a). Only the
 * ordinary 3 s gesture is held off, through the existing shift suppression, because a
 * format must not be cut short by an ordinary press (Adara). If the backstop completes
 * while plugged in, sp1_power_tick() has already quiesced (which stops the format) and
 * this returns false: the caller goes to STANDBY. Unplugged it powers off itself.
 * The watchdog is fed here, by main -- never by the storage thread (sp1_emmc.h). */
static bool storage_gate(void)
{
	int64_t last = k_uptime_get();
	bool announced = false;
	uint32_t end_ms = 0u;          /* into the completion animation */

	for (;;) {
		sp1_wdt_feed();
		const int64_t now = k_uptime_get();
		int64_t delta = now - last;
		last = now;
		if (delta < 1) {
			delta = 1;
		} else if (delta > 4 * TICK_MS) {
			delta = 4 * TICK_MS;
		}
		const uint32_t dt = (uint32_t)delta;

		const bool fnc = sp1_fnc_pressed();
		if (fnc) {
			(void)sp1_shift_used();   /* no 3 s shutdown mid-format; the backstop stays */
		}
		const enum sp1_power_result pwr = sp1_power_tick(dt, fnc);
		if (pwr == SP1_PWR_TO_STANDBY) {
			printk("STORE format interrupted by the 30 s hold -- STANDBY\n");
			return false;
		}
		const bool row_free = (pwr != SP1_PWR_ANIMATING);
		sp1_console_poll(dt, "FORMAT");

		const enum sp1_store_phase ph = sp1_store_phase();
		if (ph == SP1_STORE_FORMATTING) {
			if (!announced) {
				printk("STORE formatting: the UI waits; only the 30 s \"••\" hold "
				       "powers off\n");
				announced = true;
			}
			if (row_free) {
				sp1_led_bar(SP1_ROW_TRACK, sp1_store_progress(), 255u);
			}
		} else if (ph == SP1_STORE_READY) {
			const enum sp1_store_format fr = sp1_store_format_result();
			if (fr == SP1_STORE_FMT_NONE) {
				return true;                       /* nothing to show */
			}
			if (fr == SP1_STORE_FMT_FAILED) {
				/* No flicker: the bar as it stood, faded into the page. */
				uint8_t lv[4];
				const uint32_t p = sp1_store_progress();
				for (int i = 0; i < 4; i++) {
					const uint32_t lo = (uint32_t)i * 255u / 4u;
					const uint32_t hi = (uint32_t)(i + 1) * 255u / 4u;
					lv[i] = (uint8_t)(p >= hi ? 255u
						: (p > lo ? (p - lo) * 255u / (hi - lo) : 0u));
				}
				sp1_display_flash(lv, 0u, SP1_FMT_FAIL_FADE_MS);
				return true;
			}
			/* Done: off, on, off, on -- then full into the page. */
			end_ms += dt;
			const uint32_t step = end_ms / SP1_FMT_FLICKER_MS;
			if (step >= 4u) {
				static const uint8_t full[4] = { 255u, 255u, 255u, 255u };
				sp1_display_flash(full, 0u, SP1_FMT_DONE_FADE_MS);
				return true;
			}
			if (row_free) {
				sp1_led_bar(SP1_ROW_TRACK, (step & 1u) ? 255u : 0u, 255u);
			}
		}
		/* SP1_STORE_CHECKING: draw nothing -- the power-on fill finishes fading. */

		sp1_led_tick(dt);
		k_msleep(TICK_MS);
	}
}
#endif

#if defined(CONFIG_SP1_STORAGE) && defined(CONFIG_SP1_PLAITS)
/* ---- PRST (M6, #50): ON waits for the slots, shutdown saves one ----
 * Adara: "Hold ON entry until the four slots are read, with a time limit of 10 seconds. If it
 * can't load from storage, disable PRST and move to default patch." The storage thread reads
 * them in the ON job (sp1_store.h); main waits here, between the fresh format and audio, with
 * the power path exactly as in storage_gate(): sp1_power_tick() every tick, so the 30 s
 * backstop is untouched (rule 5a), and the ordinary 3 s gesture held off by shift
 * suppression. Nothing is drawn: the power-on fill finishes fading.
 *
 * g_prst_on is decided HERE, once per ON session: an answer that arrives after the limit is
 * ignored, and with PRST off nothing is loaded and nothing is saved. */
static bool g_prst_on;
static int  g_prst_slot;

static bool prst_gate(void)
{
	int64_t last = k_uptime_get();
	uint32_t waited = 0u;
	g_prst_on = false;
	for (;;) {
		const enum sp1_store_prst st = sp1_store_prst();
		if (st == SP1_STORE_PRST_READY) {
			g_prst_on = true;
			g_prst_slot = sp1_store_prst_current();
			printk("PRST on: slot %d, read in %u ms\n", g_prst_slot + 1, (unsigned)waited);
			return true;
		}
		if (st == SP1_STORE_PRST_OFF) {
			printk("PRST off this session: the patch in memory, nothing saved\n");
			return true;
		}
		if (waited >= SP1_PRST_LOAD_MS) {
			printk("PRST off this session: the slots took over %u ms\n",
			       (unsigned)SP1_PRST_LOAD_MS);
			return true;
		}

		sp1_wdt_feed();
		const int64_t now = k_uptime_get();
		int64_t delta = now - last;
		last = now;
		if (delta < 1) {
			delta = 1;
		} else if (delta > 4 * TICK_MS) {
			delta = 4 * TICK_MS;
		}
		const uint32_t dt = (uint32_t)delta;
		const bool fnc = sp1_fnc_pressed();
		if (fnc) {
			(void)sp1_shift_used();   /* no 3 s shutdown while loading; the backstop stays */
		}
		if (sp1_power_tick(dt, fnc) == SP1_PWR_TO_STANDBY) {
			printk("PRST load interrupted by the 30 s hold -- STANDBY\n");
			return false;
		}
		sp1_console_poll(dt, "PRST");
		sp1_led_tick(dt);
		k_msleep(TICK_MS);
		waited += dt;
	}
}

/* Load a slot from RAM. The faders do not move; pickup catches them up (Adara, as after a
 * rip). Safe while audio runs: the same calls a rip makes, and the output / burst / drive
 * changes are slewed by the synth as from the buttons. */
static void prst_load(const struct sp1_prst *p)
{
	sp1_pui_put(&p->plaits);
	sp1_mui_put(&p->marbles);
	g_out_mode = (uint8_t)p->out_mode;
	sp1_synth_set_output((enum sp1_synth_output)g_out_mode);
	g_burst_div = (uint8_t)p->burst_div;
	sp1_synth_set_burst_div(1u << g_burst_div);
	sp1_synth_set_drive(p->drive);
}

/* At ON, before audio starts. Wakes always comes up in PLAITS (Adara: the module and page
 * are not part of a slot). */
static void prst_apply(const struct sp1_prst *p)
{
	prst_load(p);
	g_module = SP1_MODULE_PLAITS;
}

/* Every PRST feature and key combination asks this (Adara, #43/#50): a volume that mounted
 * AND a slot loaded this ON session. UNKNOWN counts as no. */
static bool prst_available(void)
{
	return g_prst_on && sp1_store_volume() == SP1_STORE_VOL_OK;
}

/* "••" + PLAY's next slot (#50): loaded at once from RAM, the old slot's unsaved changes
 * dropped. The module and page on show are kept. Saved at the next shutdown. */
static void prst_select(int slot)
{
	g_prst_slot = slot;
	prst_load(sp1_store_prst_slot(slot));
	printk("PRST slot %d: %s\n", slot + 1, sp1_pui_engine_name());
}

static void prst_gather(struct sp1_prst *p)
{
	sp1_pui_get(&p->plaits);
	sp1_mui_get(&p->marbles);
	p->out_mode = g_out_mode;
	p->burst_div = g_burst_div;
	p->drive = sp1_synth_drive();
}

/* ---- the save: sp1_power_tick() calls this when the shutdown animation completes ----
 * Adara: "the shutdown animation completes -> the UI freezes -> save -> OFF or STANDBY",
 * with the UI locked and the LEDs off, limit 4 s. The UI is frozen because main is in here;
 * audio stops first (sp1_quiesce_peripherals() would stop it a moment later anyway), so the
 * storage thread has the CPU. Never called by the 30 s backstop (sp1_power.h). */
static void prst_save_at_shutdown(void)
{
	if (!g_prst_on) {
		return;
	}
	g_prst_on = false;
	sp1_leds_all_off();
	sp1_audio_stop();
	struct sp1_prst p;
	prst_gather(&p);
	const uint32_t t0 = k_uptime_get_32();
	if (!sp1_store_prst_save(g_prst_slot, &p)) {
		printk("PRST save: not possible (storage not ready) -- nothing saved\n");
		return;
	}
	while (sp1_store_save_result() == SP1_STORE_SAVE_BUSY &&
	       k_uptime_get_32() - t0 < SP1_PRST_SAVE_MS) {
		sp1_wdt_feed();
		k_msleep(TICK_MS);
	}
	const enum sp1_store_save r = sp1_store_save_result();
	printk("PRST save slot %d: %s after %u ms\n", g_prst_slot + 1,
	       r == SP1_STORE_SAVE_OK ? "saved" : (r == SP1_STORE_SAVE_BUSY
					     ? "NOT finished in time -- powering off anyway"
					     : "FAILED"),
	       (unsigned)(k_uptime_get_32() - t0));
}
#elif defined(CONFIG_SP1_PLAITS)
/* Without storage there is no PRST: "••" + PLAY is ROTC alone. */
#define SP1_PRST_SLOTS 4
static int g_prst_slot;
static bool prst_available(void) { return false; }
static void prst_select(int slot) { ARG_UNUSED(slot); }
#endif

static __noinit uint32_t g_fault_key;
static __noinit uint32_t g_fault_reason;
static __noinit uint32_t g_fault_pc;
#define FAULT_KEY_VALID 0xFA17FA17u

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	/* Never spin here. A hung app on a device with no reset pin is the one
	 * outcome we are trying to make impossible. Reboot instead; the
	 * bootloader is intact and Track1+Track4 still works. */
	g_fault_reason = reason;
	g_fault_pc = esf ? esf->basic.pc : 0u;
	g_fault_key = FAULT_KEY_VALID;
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

static void wdt_prewarn(const struct device *dev, int channel)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(channel);
	/* Reset is imminent. Nothing to flush in M0. */
}

int main(void)
{
	/* Clear RESETREAS first, before anything else can consult it. Held for
	 * diagnostics in later milestones: bit0 pin reset, bit1 watchdog,
	 * bit2 soft reset, bit3 CPU lockup, bit16 wake from SYSTEM_OFF. */
	const uint32_t resetreas = sp1_resetreas_take();

	const bool     had_fault  = (g_fault_key == FAULT_KEY_VALID);
	const uint32_t last_reason = had_fault ? g_fault_reason : 0u;
	const uint32_t last_pc     = had_fault ? g_fault_pc     : 0u;
	g_fault_key = 0u;

	/* Feed before anything slow. The bootloader may have armed a watchdog with a
	 * shorter window than ours, and everything below -- including
	 * sp1_controls_init()'s settle delay and six adc_channel_setup_dt() calls --
	 * runs before our own wdt_setup(). Straight-line code needs feeding too, not
	 * just loops. */
	sp1_wdt_feed();

	sp1_fnc_cfg_input();
	sp1_charger_init();     /* before anything slow: the device should charge
	                         * from the moment it boots, plugged or not */
	(void)sp1_batt_init();
	(void)sp1_controls_init();
	sp1_display_init();

	/* NOTE: sp1_led_init() is deliberately NOT called here. The PWM devices are
	 * deferred-init, so the LED pins stay untouched until sp1_power_on_gate()
	 * decides we are really booting. Initialising them before that flashed every
	 * LED on a tap-and-release, because the peripheral drives its idle level --
	 * inverted on these channels -- until the first duty write. */

	/* Install our own timeout if the WDT is not already running. Either way
	 * we feed every channel, so a bootloader-armed watchdog is handled too. */
	const struct device *wdt = DEVICE_DT_GET(WDT_NODE);
	if (device_is_ready(wdt)) {
		struct wdt_timeout_cfg cfg = {
			.window.max = 4000,
			.callback = wdt_prewarn,
		};
		(void)wdt_install_timeout(wdt, &cfg);
		(void)wdt_setup(wdt, 0);
	}
	sp1_wdt_feed();

	/* Was "••" held at boot? If so this is a wake, not a cable insert, and the
	 * device should come up ON rather than in standby. Must be read BEFORE the
	 * gate, which blocks until the hold completes or is abandoned. */
	const bool woke_by_fnc = sp1_fnc_pressed();

	/* Power-ON hold gate, BEFORE anything else visible. If "••" is held we were
	 * woken by it and must see 2 s of hold or go straight back to sleep; if it
	 * is not held this returns at once. Never returns on an early release. */
	sp1_power_on_gate();

	/* Committed to running: claim the LED pins. Idempotent -- the gate has
	 * usually already done this on its way through the progress fill. */
	sp1_wdt_feed();
	const bool leds_ok = (sp1_led_init() == 0);

	/* Console after the gate too: USB enumeration is slow and pointless on a
	 * tap-and-release. Never blocks waiting for a host.
	 * USB bring-up is the slowest straight-line stretch in boot, so it is bracketed
	 * by feeds rather than trusted to finish quickly. */
	sp1_wdt_feed();
	(void)sp1_console_init();
	sp1_wdt_feed();
	/* Keep everything printed from here on, for whoever opens the console later -- the
	 * OP-XY holds the USB port during a session (sp1_logbuf.h). Before the banner, so
	 * the stored log starts with it. */
	sp1_logbuf_init(resetreas);
	sp1_console_banner(resetreas, had_fault, last_reason, last_pc);

	/* If the previous boot ended in a fault, pulse the model row twice. With no
	 * console yet this is the crash indicator, and a pulse is unmistakable
	 * against the steady charger-status LEDs the main loop drives afterwards. */
	if (had_fault) {
		for (int pulse = 0; pulse < 2; pulse++) {
			sp1_led_fade_row(SP1_ROW_TRACK, 255, 250);
			for (int t = 0; t < 250 / TICK_MS; t++) {
				sp1_led_tick(TICK_MS);
				sp1_wdt_feed();
				k_msleep(TICK_MS);
			}
			sp1_led_fade_row(SP1_ROW_TRACK, 0, 250);
			for (int t = 0; t < 250 / TICK_MS; t++) {
				sp1_led_tick(TICK_MS);
				sp1_wdt_feed();
				k_msleep(TICK_MS);
			}
		}
		sp1_leds_all_off();
	}
	ARG_UNUSED(leds_ok);

	/* ---- on battery, running requires "••" (M2) ----
	 * Every way the device can reset without the user asking -- the watchdog, a
	 * fault (the handler reboots), a CPU lockup -- used to land straight back in ON
	 * when unplugged, because only a USB boot goes to STANDBY. That was harmless
	 * while ON did nothing heavy. From M2, ON starts an audio thread that outranks
	 * the main loop. If THAT is what caused the reset, the device reboots into the
	 * same fault: a boot loop, on battery, with the power button's code never
	 * getting to run.
	 *
	 * So on battery the device only runs when "••" woke it. Anything else goes to
	 * SYSTEM_OFF and waits for "••" -- which still works, because it is a hardware
	 * wake, not code. A USB boot is unaffected: it lands in STANDBY, which starts no
	 * audio, so it cannot loop either way.
	 *
	 * This converts any future audio bug that reboots the device from "flattens the
	 * battery in a loop" into "turns off and waits". It does not rely on the audio
	 * code being right, which is the point. */
	/* Read USB presence ONCE and use the same answer for both decisions below. Two
	 * separate reads let an unplug landing between them boot ON, on battery, without
	 * "••" -- exactly the case this policy exists to prevent. */
	const bool plugged_at_boot = sp1_usb_present();

	if (!plugged_at_boot && !woke_by_fnc) {
		printk("BOOT on battery without \"••\" (resetreas 0x%08x):"
		       " powering off, press \"••\" to start\n", resetreas);
		k_msleep(50);              /* let the line leave, if a host is attached */
		sp1_power_off();           /* never returns */
	}

	/* The audio thread is created here and parks immediately. It does no work
	 * until ON starts it, and it is never created on a tap-and-release wake. */
	sp1_audio_init();
#if defined(CONFIG_SP1_STORAGE)
	sp1_store_init();              /* the thread parks until an ON entry queues a job */
#endif
#if defined(CONFIG_SP1_PLAITS)
	sp1_pui_init();                /* page defaults: once per boot, kept across ON */
	sp1_mui_init();
#endif
#if defined(CONFIG_SP1_STORAGE) && defined(CONFIG_SP1_PLAITS)
	sp1_power_set_save_hook(prst_save_at_shutdown);   /* PRST (#50) */
#endif

	/* ---- top-level state machine ----
	 * STANDBY: plugged in and not running. ON: running.
	 * Entered in STANDBY when we booted because the cable went in, which is
	 * exactly the "plugged in while off" case. */
	bool standby = plugged_at_boot && !woke_by_fnc;

	for (;;) {
		if (standby) {
			sp1_standby_run();     /* returns only when powering ON */
			standby = false;
		}

		uint32_t poll = 0;
		/* "••" is held at this instant if we just powered on by holding it, so
		 * seed the edge detector as PRESSED. Seeding it false would read the
		 * finger still on the button as a fresh press and mark the faders at a
		 * moment the user never chose. */
		bool fnc_prev = sp1_fnc_pressed();
		sp1_leds_all_off();
		/* Re-raise the control rail on every entry to ON. It is dropped on the
		 * way to SYSTEM_OFF and this loop cannot read a single fader or button
		 * without it -- the one omission that broke all of M1d-a. Idempotent. */
		sp1_controls_rail_on();
		bool charge_held = false;      /* charging switched off for a USB host (below) */
		/* Diagnostics are slowed right down in ON. Not for the average cost
		 * (~0.07 % at 1 Hz) but because a ~400 us ADC read or an unbounded
		 * printk inside a 5 ms audio block is a dropout, not a percentage. */
		sp1_console_set_status_period(SP1_CONSOLE_PERIOD_ON_MS);

		/* ---- M1d-a: measure the ladders, with the firmware doing the
		 * bookkeeping ----
		 * The raw firehose is gone. It produced clean plateaus but no labels,
		 * and reconstructing "which press was that" afterwards from memory is
		 * where the first attempt came apart. The guided run names one target
		 * at a time and records the answer against that name, so the table
		 * cannot be mis-assembled. Off once the tables exist. */
		sp1_controls_reset_buttons();

#if defined(CONFIG_SP1_STORAGE)
		/* M6 (#43): the eMMC job runs in the storage thread, below main and audio;
		 * this only queues it. Every way out of ON stops it (sp1_quiesce_peripherals). */
		sp1_store_on_enter();
#if defined(CONFIG_SP1_FRESH)
		/* The fresh image's format holds ON entry, before audio (storage_gate). */
		if (!storage_gate()) {
			standby = true;           /* the 30 s backstop, plugged in */
			continue;
		}
#endif
#if defined(CONFIG_SP1_PLAITS)
		/* PRST (#50): the current slot, read by the same job, applied before audio. */
		if (!prst_gate()) {
			standby = true;           /* the 30 s backstop, plugged in */
			continue;
		}
		if (g_prst_on) {
			prst_apply(sp1_store_prst_slot(g_prst_slot));
		}
#endif
#endif

		/* ---- M2: audio up on every entry to ON ----
		 * The way into STANDBY powers the codecs, the amp and the oscillator
		 * down (sp1_quiesce_peripherals), so they are brought back here each
		 * time, not once at boot. Bounded: sp1_audio_start() cannot hang ON.
		 * The tone always starts OFF -- nothing makes a sound until PLAY. */
		sp1_audio_tone_set(false);
#if defined(CONFIG_SP1_MIDI)
		/* ⚠️ BEFORE the audio thread starts: anything a host sent while the device was
		 * off is still in the queue, and the first audio block would play it. The audio
		 * thread honours this request at the top of that first block (sp1_midi.h). */
		sp1_midi_on_enter();
#endif
		{
			const int arc = sp1_audio_start();
			struct sp1_audio_stats as;
			sp1_audio_stats(&as);
			printk("AUD start rc=%d  i2s_cfg=%d  tas=%s  hp=%s  block %u ms x %u queued:"
			       " ~%u ms in to out\n", arc,
			       as.cfg_rc, as.tas_ok ? "ok" : "FAIL",
			       as.hp_ok ? "ok" : "FAIL",
			       (unsigned)(CONFIG_SP1_AUDIO_BLOCK_FRAMES / 48),
			       (unsigned)CONFIG_I2S_NRFX_TX_BLOCK_COUNT,
			       (unsigned)((CONFIG_I2S_NRFX_TX_BLOCK_COUNT + 3) *
					  (CONFIG_SP1_AUDIO_BLOCK_FRAMES / 48)));
#if defined(CONFIG_SP1_MIDI) && defined(CONFIG_SP1_USB_AUDIO)
			/* M5c: the same count for USB audio out (sp1_midi.h): what the MIDI clock
			 * makes up for while a host takes it. */
			printk("AUD USB audio out: ~%u.%u ms in to out (speaker ~%u)\n",
			       (unsigned)(SP1_MIDI_USB_OUTPUT_LATENCY_US / 1000),
			       (unsigned)((SP1_MIDI_USB_OUTPUT_LATENCY_US % 1000) / 100),
			       (unsigned)SP1_MIDI_OUTPUT_LATENCY_MS);
#endif
			/* #32: which USB path this build takes, so every log says it. */
			printk("USB midi=%s  threads=%s  audio out=%s\n",
			       IS_ENABLED(CONFIG_UDC_NRF_OUT_FAST) ? "in the interrupt (fast path)"
								   : "usbd thread",
			       sp1_usbd_threads_demoted() == 2 ? "below audio"
							      : "above audio (stock)",
			       IS_ENABLED(CONFIG_SP1_USB_AUDIO) ? "UAC1 48k/16/2, fast path" : "off");
		}
		sp1_playrow_reset();
		uint32_t aud_print = 0;
#if defined(CONFIG_SP1_PLAITS)
		/* Transport state: always starts OFF on entry to ON -- no burst, no
		 * burst, Marbles' clock stopped (PLAY starts it). */
		burst_was = false;
		ffwd_consumed = false;
		unpatch_ms = 0u;
		unpatch_btn = -1;
		sp1_rg_init(&unpatch_eat);
		burst_n0 = 0u;
		rip_ms = 0u;
		rip_armed = false;
		rip_glyph = false;
		rip_show_page = false;
		prst_showing = false;
		prst_changed = false;
		prst_in_hold = false;
		sp1_synth_set_burst_div(1u << g_burst_div);
		sp1_synth_set_output((enum sp1_synth_output)g_out_mode);
#if defined(CONFIG_SP1_MIDI)
		/* A host already attached gets its "plugged in" prompt after the power-on fill
		 * (C10). */
		midi_enter();
#endif
		sp1_synth_burst(0);
		sp1_marbles_run(false);
		beats_seen = sp1_marbles_beats();
		sp1_synth_set_tempo(sp1_mui_bpm());

		/* Pages start on BASE. The first ON after boot takes the faders as they
		 * are; later ones keep the values and pick the faders up.
		 *
		 * ⚠️ SCAN FIRST, and this is not optional (M4e). sp1_fader_raw() returns
		 * the last SCANNED value, and no scan has run since sp1_controls_rail_on()
		 * above -- the ON loop's first scan is further down. Coming from STANDBY the
		 * rail was off, so the stale reading is effectively zero.
		 *
		 * Seeding the UI from that made pos[] = 0 and prev[] = 0, and then the next
		 * few ticks of the fader low-pass climbing to the REAL positions looked
		 * exactly like a hand sweeping every fader across its whole travel. Pickup
		 * applied that relatively to every catching layer, and the attenuverters
		 * came up sitting on the faders -- the bug Adara reported twice (M4a fixed
		 * pickup, which was correct all along; it was being fed a lie).
		 *
		 * Bounded, feeds the watchdog, and gives up rather than blocking ON: a
		 * never-valid sample is still better seeded from a real reading than from a
		 * stale one, and CATCH_JUMP in both UI files catches whatever gets through. */
		{
			uint16_t raw[SP1_NUM_FADERS];
			for (int i = 0; i < 8 && !sp1_faders_valid(); i++) {
				sp1_wdt_feed();
				sp1_controls_scan();
				k_msleep(SP1_TICK_MS);
			}
			faders_raw(raw);
			sp1_pui_enter(raw);
			if (g_module == SP1_MODULE_MARBLES) {
				sp1_mui_enter(raw, true);
			}
		}
		sp1_playrow_set_fg(g_module == SP1_MODULE_MARBLES ? SP1_FG_CLOCK
								   : SP1_FG_METER);
		printk("SYN plaits engine=%d (%s)  module=%s  level=%d.%d dBFS\n",
		       sp1_pui_engine(), sp1_pui_engine_name(),
		       g_module == SP1_MODULE_MARBLES ? "marbles" : "plaits",
		       sp1_audio_level_db_x10() / 10,
		       (-sp1_audio_level_db_x10()) % 10);
#endif

		/* Drop any fader mark left over from the last time we were ON. Hygiene, not
		 * a bug fix: the mark is meant to be per-hold, and one can outlive a session
		 * because the mark is cleared on a "••" FALLING edge while a completed
		 * shutdown leaves ON with the button still DOWN -- so that edge never
		 * arrives. Coming back ON by holding "••" takes no fresh mark either
		 * (fnc_prev is seeded pressed), so a stale one could survive into the next
		 * hold.
		 *
		 * It cannot affect the gesture: through that whole window `armed` is false,
		 * so sp1_power_tick() calls gesture_reset() every tick and suppression has
		 * nothing to suppress; the release that arms it also clears the mark. The
		 * only reachable symptom is a spurious log line, which is reason enough.
		 *
		 * ⚠️ DO NOT read the three suppression lines in sp1-20260920-193610.log as
		 * evidence of this. They were REAL: Adara was deliberately disrupting
		 * shutdowns by moving faders, which is exactly what the fader path is for.
		 * I diagnosed them as this stale mark from the log alone and was wrong --
		 * "lad0=0 lad1=0 chord=0x00" rules a button out but says nothing about what
		 * it was, which is precisely why the line now names the path instead. */
		sp1_controls_activity_clear();

		/* ---- re-running calibration once the tables exist ----
		 * The tables are populated, so this normally does nothing. Build with
		 * -DSP1_FORCE_CALIB=1 to run it again -- a second device, a suspected
		 * drift, or re-measuring after any change to the ADC configuration
		 * (gain, reference, acquisition time, oversampling), every one of which
		 * invalidates the table. Deliberately a build flag and not a chord:
		 * every chord is a control the run is trying to measure.
		 *
		 * SP1_FORCE_CALIB lives in sp1_calib.h as a plain constant. It is NOT a
		 * `west build -- -D...` flag: after the `--`, -D sets a CMake variable
		 * rather than a compiler macro, so the #if never fired and the run
		 * silently did not happen. */
#if SP1_FORCE_CALIB
		const bool want_calib = true;
#else
		const bool want_calib = !sp1_controls_calibrated();
#endif
		if (want_calib) {
			/* The wizard owns the console while it runs -- a battery line
			 * landing between a prompt and its answer is exactly the kind of
			 * clutter that made the last capture hard to follow. Restored
			 * the moment the run finishes. */
			sp1_console_set_status_period(SP1_CONSOLE_PERIOD_OFF);
			sp1_calib_start();
		}
		bool calib_was_running = sp1_calib_running();

		/* ---- ON ----
		 * ⚠️ Timings use MEASURED elapsed time, not TICK_MS.
		 *
		 * One iteration is k_msleep(TICK_MS) PLUS eight blocking ADC reads
		 * (~430 us), the display, the LEDs and the console, so the real period is
		 * ~8.5 ms against a declared 8 -- every gesture constant ran ~6 % long.
		 * Worse, the calibration report paces ~50 lines inside a single tick,
		 * about 60 ms that a nominal accumulator does not see at all. A gesture
		 * timed in ticks quietly changes length with whatever else the loop is
		 * doing, and audio in M2 will make that worse, not better.
		 *
		 * Clamped, because a long stall must not hand the power gesture a giant
		 * jump and complete a hold the user never performed. */
		int64_t last_uptime = k_uptime_get();

		while (!standby) {
			sp1_wdt_feed();

			const int64_t now_uptime = k_uptime_get();
			int64_t delta = now_uptime - last_uptime;
			last_uptime = now_uptime;
			if (delta < 1) {
				delta = 1;                /* never let time stand still */
			} else if (delta > 4 * TICK_MS) {
				delta = 4 * TICK_MS;      /* clamp a stall */
			}
			const uint32_t dt = (uint32_t)delta;

			/* ---- ORDER MATTERS: scan, then decide shift, then power ----
			 * The shutdown gesture has to see THIS tick's control state, or
			 * a fader moved during the hold is noticed one tick late and, at
			 * the very end of the animation, not at all. */
			/* How loud the speaker is right now, for the audio-load part of
			 * the sag correction (M4a). Zero effect until its K is measured. */
			sp1_controls_set_audio_load((float)sp1_meter_level() / 255.0f);
			sp1_controls_scan();

			/* ---- "••" as a shift suppresses shutdown ----
			 * Touching any other control while "••" is held means it is
			 * being used as a modifier, not as a power gesture. This needs
			 * no threshold tables: a ladder press shows up as a loaded rail
			 * and faders are compared against a mark taken when "••" went
			 * down. Releasing "••" clears the mark so the next hold is
			 * judged fresh. */
			const bool fnc = sp1_fnc_pressed();
			if (fnc && !fnc_prev) {
				sp1_controls_activity_mark();
				/* ---- "••" down: show the battery for 1.5 s (Adara, M4c) ----
				 * Sampled here rather than displaying the up-to-10-s-old
				 * reading; ~400 us in the control loop, which already does
				 * this every 10 s. On the PLAY row, so it cannot collide
				 * with the shutdown animation or the shift pages. */
				sp1_batt_sample();
				batt_flash_ms = SP1_BATT_FLASH_MS;
				batt_fade_ms = 0u;
			} else if (!fnc && fnc_prev) {
				sp1_controls_activity_clear();
			}
			/* ---- how the battery display leaves (M4e, Adara) ----
			 * Released  -> fade out over SP1_BATT_FLASH_FADE_MS. The display has
			 *              done its job; fading is how it should end, and a tap is
			 *              always shorter than the display so this is the usual case.
			 * Timed out  -> the same fade, started below.
			 * Interfered -> CUT. Touching another control while "••" is held means
			 *              you were reaching for the shift layer and want to see
			 *              THAT; a 300 ms fade in front of it would be in the way. */
			if (batt_flash_ms > 0u || batt_fade_ms > 0u) {
				if (fnc && sp1_controls_activity()) {
					batt_flash_ms = 0u;
					batt_fade_ms = 0u;
				} else if (!fnc && batt_flash_ms > 0u) {
					batt_flash_ms = 0u;
					batt_fade_ms = SP1_BATT_FLASH_FADE_MS;
				}
			}
			if (fnc && sp1_controls_activity()) {
				if (sp1_shift_used()) {
					/* Name the culprit, once per hold. "Shutdown does
					 * not work" was diagnosed twice from reset reasons
					 * and inference; the ladder values and the decoded
					 * mask turn that into a single readable line. */
					char why[64];
					(void)sp1_controls_activity_describe(
						why, sizeof(why));
					printk("PWR  shutdown suppressed: %s"
					       "  (lad0=%u lad1=%u)\n",
					       why, sp1_ladder_raw(0),
					       sp1_ladder_raw(1));
				}
			}
			fnc_prev = fnc;

			/* The shutdown gesture owns the model row while it runs, so
			 * nothing else may write there until it returns false. When it
			 * completes while PLUGGED it returns true having quiesced the
			 * peripherals, and the destination is STANDBY rather than
			 * SYSTEM_OFF -- no reset, we are already awake. */
			const enum sp1_power_result pwr =
				sp1_power_tick(dt, fnc);

			if (pwr == SP1_PWR_TO_STANDBY) {
				standby = true;
				break;
			}
			const bool shutdown_active = (pwr == SP1_PWR_ANIMATING);

			if ((poll += dt) >= SP1_BATT_POLL_ON_MS) {
				poll = 0;
				sp1_batt_sample();
			}

			/* ---- one fader per LED ----
			 * F1 sets T1's brightness, F2 sets T2's, F3 T3, F4 T4. All
			 * four at once, every tick, with no "which one moved" logic.
			 *
			 * Only updated while the rail reads idle. A ladder press sags
			 * the rail that also feeds the faders, so a mid-press sample is
			 * junk; sp1_faders_valid() is false then and the meter holds its
			 * last good reading rather than dipping. That is the structural
			 * reason pressing a track button no longer disturbs the row. */
#if defined(CONFIG_SP1_PLAITS)
			/* The track row shows the ACTIVE page's four values (sp1_plaits_ui):
			 * a fader that has not been picked up yet shows where it has to go. */
			{
				uint8_t lv[SP1_NUM_FADERS];
				if (g_module == SP1_MODULE_MARBLES) {
					(rip_show_page ? sp1_mui_page_leds : sp1_mui_leds)(lv);
				} else {
					(rip_show_page ? sp1_pui_page_leds : sp1_pui_leds)(lv);
				}
				sp1_display_meter(lv);
			}
			if (0) {
#else
			if (sp1_faders_valid()) {
#endif
				const uint8_t meter[SP1_NUM_FADERS] = {
					sp1_fader_level(0), sp1_fader_level(1),
					sp1_fader_level(2), sp1_fader_level(3),
				};
				sp1_display_meter(meter);
			}

			if (!shutdown_active) {
				sp1_display_tick(dt);
			} else {
				/* The animation owns the row, but overlay timers must
				 * still run on wall clock or a cancelled shutdown hands
				 * back a stale model pattern instead of the meter. */
				sp1_display_age(dt);
			}

			/* After the scan (it reads this tick's values) and after the
			 * power gesture (which must never be starved by it). */
			sp1_calib_tick(dt);
			if (calib_was_running && !sp1_calib_running()) {
				calib_was_running = false;
				sp1_console_set_status_period(SP1_CONSOLE_PERIOD_ON_MS);
			}

			sp1_console_poll(dt, "ON");

			/* ---- no charging while a USB HOST is attached (M5a, Adara) ----
			 * A battery-powered host -- the OP-XY -- was charging the SP-1 from its own
			 * battery all session. ON + a host that configured us = charging off; a plain
			 * charger still charges, and STANDBY always does (sp1_power.h). Re-checked
			 * every tick, so plugging and unplugging while ON both follow. */
			{
				const bool host = sp1_usbd_host();
				if (host != charge_held) {
					charge_held = host;
					sp1_charger_enable(!host);
					printk("CHARGE %s\n", host
					       ? "off: a USB host is attached (the SP-1 runs from USB)"
					       : "on");
				}
			}

#if defined(CONFIG_SP1_PLAITS)
			/* ---- M3b / M4: the two modules (docs/UI-SPEC.md v0.9) ----
			 * PLAITS: BASE / SHIFT / SETTINGS, pickup and centre detents
			 * (sp1_plaits_ui). MARBLES: t and X pages, their SHIFT pages and
			 * the shared SETTINGS (Y) page (sp1_marbles_ui). Only the module
			 * on show is ticked; both always feed the audio thread.
			 * sp1_fader_raw() holds the last clean value while a ladder button
			 * sags the rail, so a press cannot bend a parameter. */
			{
				uint16_t raw[SP1_NUM_FADERS];
				faders_raw(raw);
				const bool valid = sp1_faders_valid();
				const bool act = sp1_controls_activity();
				const bool marbles = (g_module == SP1_MODULE_MARBLES);
				const bool running = sp1_marbles_running();

				if (!marbles) {
					const uint32_t ev = sp1_pui_tick(dt, raw, valid, fnc, act);
					if (ev & SP1_PUI_EV_LAYER) {
						if (sp1_pui_active() == SP1_PUI_SHIFT) {
							printk("SHIFT held\n");
						} else if (!(ev & SP1_PUI_EV_PAGE)) {
							printk("SHIFT released -> %s\n",
							       sp1_pui_layer_name(sp1_pui_active()));
						}
					}
					if (ev & SP1_PUI_EV_PAGE) {
						printk("PAGE %s\n",
						       sp1_pui_layer_name(sp1_pui_page()));
					}
					if (ev & SP1_PUI_EV_CAUGHT) {
						for (int i = 0; i < SP1_NUM_FADERS; i++) {
							if (ev & (0x10u << i)) {
								printk("PICKUP F%d caught (%s)\n", i + 1,
								       sp1_pui_layer_name(
									       sp1_pui_active()));
							}
						}
					}
					if (ev & SP1_PUI_EV_LEVEL) {
						printk("LEVEL %s\n", sp1_pui_level_connected()
						       ? "connected: VCA held open (drone)"
						       : "disconnected: TRIG plucks");
					}
				} else {
					const uint32_t ev = sp1_mui_tick(dt, raw, valid, fnc, act);
					if (ev & (SP1_MUI_EV_LAYER | SP1_MUI_EV_PAGE)) {
						printk("MARBLES %s\n",
						       sp1_mui_layer_name(sp1_mui_active()));
					}
					if (ev & SP1_MUI_EV_CAUGHT) {
						for (int i = 0; i < SP1_NUM_FADERS; i++) {
							if (ev & (0x10u << i)) {
								printk("PICKUP F%d caught (%s)\n", i + 1,
								       sp1_mui_layer_name(
									       sp1_mui_active()));
							}
						}
					}
				}

				/* ---- PLAY: Marbles' clock, run / stop (Adara, M4) ----
				 * On either module. Not with "••" down: that is the reset
				 * gesture below, and must never also flip the transport. */
				if (!fnc && sp1_button_pressed(SP1_BTN_PLAY)) {
					if (!running) {
						if (!g_seeded) {
							/* the moment of the first PLAY varies */
							sp1_marbles_seed(k_cycle_get_32());
							g_seeded = true;
						}
						sp1_marbles_run(true);
						sp1_playrow_clock_reset();
						beats_seen = sp1_marbles_beats();
						printk("CLOCK run: %d.%d BPM, %s, t range %s\n",
						       (int)sp1_mui_bpm(),
						       (int)(sp1_mui_bpm() * 10.0f) % 10,
						       sp1_marbles_model_name(sp1_mui_model()),
						       t_range_name[sp1_mui_t_range()]);
					} else {
						sp1_marbles_run(false);
						printk("CLOCK stop\n");
					}
				}

				/* ---- "••" + PLAY: PRST and ROTC (Adara, #50) ----
				 *   1. PLAY pressed with "••" held: the current slot's glyph at
				 *      once, its LED ramping at 10 Hz for 1.6 s, then the page.
				 *   2. PLAY pressed again while it shows: the NEXT slot (4 -> 1),
				 *      loaded at once -- the old slot's unsaved changes dropped, the
				 *      faders not moved (pickup) -- and its glyph shown. No
				 *      double-tap delay, so slots can be browsed.
				 *   3. PLAY kept held from a press that did not change slot: ROTC,
				 *      3 s in all -- the glyph, black, a rise to full, the Unpatch
				 *      animation, the wipe (rotc_levels, sp1_ui_timing.h).
				 *   4. After a slot change, no ROTC until "••" is released.
				 *   5. Letting go once ROTC's animation has begun (past the glyph)
				 *      fades back to the SHIFT screen; nothing is wiped.
				 * Every PRST part asks prst_available() (the volume is OK and this
				 * ON session loaded a slot); without it this is ROTC alone, the glyph's
				 * 1.6 s a slow fade to black instead (Adara; rotc_or_fade_levels).
				 *
				 * ROTC is a FULL PATCH WIPE of the module on show (docs/DEFAULTS.md);
				 * the page you are standing on is kept. A "••" + PLAY press is a
				 * shift use, so it can never start a power-off. */
				const bool play_down = sp1_button_held(SP1_BTN_PLAY);
				if (fnc && sp1_button_pressed(SP1_BTN_PLAY) && !shutdown_active) {
					if (prst_showing && prst_in_hold && prst_available()) {
						prst_select((g_prst_slot + 1) % SP1_PRST_SLOTS);
						prst_changed = true;
						prst_show_ms = 0u;            /* the new slot's glyph */
						rip_armed = false;
					} else {
						prst_showing = prst_available();
						prst_in_hold = prst_showing;
						prst_show_ms = 0u;
						rip_armed = !prst_changed;
						rip_glyph = prst_showing;
					}
					rip_ms = 0u;
				}

				if (rip_armed && fnc && play_down && !shutdown_active) {
					rip_ms += dt;
					if (rip_ms >= SP1_PRST_GLYPH_MS) {
						prst_showing = false;          /* ROTC's animation now */
					}
					if (rip_ms >= SP1_RIP_HOLD_MS) {
						rip_armed = false;
						if (marbles) {
							sp1_mui_rip();
							/* The rip routes X2 to V/Oct (#50), so the
							 * M4b interlock applies: the quantizer off,
							 * the range opened. */
							voct_took_over(SP1_DEST_VOCT);
							/* Re-seed AND re-draw the DEJA VU loop,
							 * deferred into the audio thread so it cannot
							 * race the generators. The clock keeps running
							 * and its phase jumps once (Adara, M4d). */
							sp1_marbles_reseed(k_cycle_get_32());
							g_seeded = true;
							printk("RIP marbles: t2 -> TRIG, X2 -> V/Oct, the rest "
							       "out, %d BPM, %s, re-seeded%s\n",
							       (int)sp1_mui_bpm(),
							       sp1_marbles_model_name(sp1_mui_model()),
							       sp1_marbles_running()
							       ? " (clock still running)" : "");
						} else {
							sp1_pui_rip();
							/* ⚠️ The drive is a PLAITS parameter and only a
							 * PLAITS rip clears it (Adara, M4e): Marbles
							 * makes no audio, so the drive is not part of
							 * its patch. */
							sp1_synth_set_drive(SP1_PRST_DEF_DRIVE);
							/* PLAITS-side output controls go back too
							 * (docs/DEFAULTS.md). VOL is deliberately kept. */
							g_out_mode = SP1_PRST_DEF_OUT;
							sp1_synth_set_output(
								(enum sp1_synth_output)g_out_mode);
							g_burst_div = SP1_PRST_DEF_BURST;   /* 1/32 */
							sp1_synth_set_burst_div(1u << g_burst_div);
							printk("RIP plaits: patch wiped -- %s, faders "
							       "neutral, attenuverters 0, quantizer off, "
							       "drive off, OUT, 1/32\n",
							       sp1_pui_engine_name());
						}
						static const uint8_t dark[4] = { 0u, 0u, 0u, 0u };
						sp1_display_flash(dark, 0u, SP1_RIP_FADEBACK_MS);
						/* ⚠️ Fade back to the PAGE, not the SHIFT layer "••"
						 * would otherwise show: the rip has just zeroed every
						 * attenuverter, which draws DARK, so the fade went
						 * black -> black and the rip looked unfinished until
						 * "••" was let go (issue #4). */
						rip_show_page = true;
					} else if (rip_ms >= SP1_PRST_GLYPH_MS || !rip_glyph) {
						uint8_t lv[4];
						rotc_or_fade_levels(rip_ms, marbles, lv);
						sp1_display_flash(lv, 100u, SP1_RIP_CANCEL_FADE_MS);
					}
				} else if (rip_armed) {
					/* PLAY (or "••") let go before the wipe. Within the glyph it
					 * was a tap: the glyph carries on. Past it -- or anywhere
					 * with PRST off, where the slow fade is ROTC's own -- ROTC
					 * is cancelled: back to the SHIFT screen. */
					if (rip_ms >= SP1_PRST_GLYPH_MS || !rip_glyph) {
						uint8_t lv[4];
						rotc_or_fade_levels(rip_ms, marbles, lv);
						sp1_display_flash(lv, 0u, SP1_RIP_CANCEL_FADE_MS);
						printk("RIP cancelled\n");
					}
					rip_armed = false;
					rip_ms = 0u;
				}
				if (!(fnc && play_down)) {
					/* The rip's hold is over: the ordinary layer rules again. */
					rip_show_page = false;
				}
				if (!fnc) {
					prst_changed = false;                  /* (4) */
					prst_in_hold = false;   /* (2) is per hold: the glyph may still fade */
				}

				/* The slot glyph, while it shows (and ROTC has not taken the row). */
				if (prst_showing) {
					prst_show_ms += dt;
					uint8_t lv[4];
					prst_glyph_levels(g_prst_slot, prst_show_ms, lv);
					if (prst_show_ms >= SP1_PRST_GLYPH_MS) {
						prst_showing = false;
						sp1_display_flash(lv, 0u, SP1_PRST_FADEBACK_MS);
					} else {
						sp1_display_flash(lv, 100u, SP1_PRST_FADEBACK_MS);
					}
				}

				/* ================= UNPATCH ("••" + Tn held, M4e) =================
				 * Runs BEFORE the button handlers. A commit arms `unpatch_eat`, which
				 * stays armed until that button reads UP, so the release -- whenever
				 * it comes -- reaches the handlers eaten (issue #2).
				 *
				 * Only where that button HAS a cable to cut: the PLAITS base page
				 * (its SETTINGS panel's T1-T4 are scale controls, not routings) and
				 * the MARBLES t and X pages (its SETTINGS T1-T4 are [F], [G] and
				 * [J]). Elsewhere the hold does nothing at all, which is the honest
				 * answer -- an animation followed by nothing would be a lie. */
				const bool unpatchable = fnc && !shutdown_active &&
					(marbles ? !sp1_mui_settings()
						 : sp1_pui_page() != SP1_PUI_SETTINGS);
				const int ubtn = unpatchable ? unpatch_held_button() : -1;
				if (ubtn >= 0 && ubtn == unpatch_btn) {
					if (unpatch_ms < SP1_UNPATCH_HOLD_MS) {
						unpatch_ms += dt;
						if (unpatch_ms >= SP1_UNPATCH_HOLD_MS) {
							/* ---- the animation has ended: commit ---- */
							int n = 0;
							const char *what = "";
							if (marbles) {
								const bool tpage = sp1_mui_page() ==
									SP1_MUI_PAGE_T;
								uint8_t was;
								if (ubtn == 3) {
									was = sp1_mui_unpatch_x(3);
									what = "Y";
								} else if (tpage) {
									was = sp1_mui_unpatch_t(ubtn);
									what = "t";
								} else {
									was = sp1_mui_unpatch_x(ubtn);
									what = "X";
								}
								n = (was != SP1_DEST_NONE) ? 1 : 0;
								printk("UNPATCH %s%s: %s\n", what,
								       ubtn == 3 ? "" :
								       (ubtn == 0 ? "1" :
									ubtn == 1 ? "2" : "3"),
								       n ? sp1_mui_dest_name(was)
									 : "nothing was patched");
							} else {
								/* PLAITS: the button names a parameter.
								 * T1 = FREQUENCY, which is V/Oct AND FM --
								 * both modulate the pitch and V/Oct has
								 * no button of its own. */
								static const char *const pn[4] = {
									"FREQUENCY", "TIMBRE",
									"MORPH", "HARMONICS",
								};
								if (ubtn == 0) {
									n = sp1_mui_unpatch_dest(
										SP1_DEST_VOCT);
									n += sp1_mui_unpatch_dest(
										SP1_DEST_FM);
								} else {
									n = sp1_mui_unpatch_dest(
										ubtn == 1 ? SP1_DEST_TIMBRE :
										ubtn == 2 ? SP1_DEST_MORPH :
											    SP1_DEST_HARM);
								}
								what = pn[ubtn];
								printk("UNPATCH %s: %d route%s cleared\n",
								       what, n, n == 1 ? "" : "s");
							}
							sp1_rg_arm(&unpatch_eat, ubtn);
							static const uint8_t dk[4] = { 0u, 0u, 0u, 0u };
							sp1_display_flash(dk, 0u,
									  SP1_UNPATCH_FADEBACK_MS);
						} else if (unpatch_ms >= SP1_UNPATCH_START_MS) {
							uint8_t lv[4];
							unpatch_levels(unpatch_ms -
								       SP1_UNPATCH_START_MS, lv);
							sp1_display_flash(lv, 100u,
									  SP1_UNPATCH_CANCEL_MS);
						}
					}
				} else {
					/* A different button, or none, or "••" released: the hold is
					 * over. Hand the row back if an animation had started. */
					if (unpatch_ms >= SP1_UNPATCH_START_MS &&
					    unpatch_ms < SP1_UNPATCH_HOLD_MS) {
						uint8_t lv[4];
						unpatch_levels(unpatch_ms - SP1_UNPATCH_START_MS, lv);
						sp1_display_flash(lv, 0u, SP1_UNPATCH_CANCEL_MS);
						printk("UNPATCH cancelled\n");
					}
					unpatch_btn = ubtn;
					unpatch_ms = 0u;
					/* ⚠️ unpatch_eat is NOT cleared here. It must survive until
					 * the RELEASE it is there to suppress has been seen by the
					 * button handlers below; it is disarmed after them. */
				}

				/* ---- T4 swaps module (Adara, M4). Not with "••" (Unpatch on
				 * PLAITS, Y's destination on MARBLES) and not on either SETTINGS
				 * page. ---- */
				/* ⚠️ T4 is not the module swap on either SETTINGS page: on
				 * MARBLES it is [J], and on PLAITS it is output select (#11) --
				 * so the panel cannot be left by accident (Adara, M4b). Tap "••"
				 * to leave. */
				if (!fnc && sp1_button_pressed(SP1_BTN_T4) &&
				    !(marbles && sp1_mui_settings()) &&
				    !(!marbles && sp1_pui_page() == SP1_PUI_SETTINGS)) {
					if (marbles) {
						g_module = SP1_MODULE_PLAITS;
						sp1_pui_resume(raw, fnc);
						uint8_t elv[4];
						sp1_pui_engine_leds(elv);
						sp1_display_engine(elv);
						sp1_playrow_set_fg(SP1_FG_METER);
						printk("MODULE plaits (%s)\n", sp1_pui_engine_name());
					} else {
						g_module = SP1_MODULE_MARBLES;
						sp1_mui_enter(raw, fnc);
						uint8_t plv[4];
						sp1_mui_page_pattern(plv);
						sp1_display_engine(plv);
						sp1_playrow_set_fg(SP1_FG_CLOCK);
						printk("MODULE marbles (%s page)\n",
						       sp1_mui_page() == SP1_MUI_PAGE_X ? "X" : "t");
					}
				} else if (!marbles) {
					plaits_buttons(fnc, running, dt);
				} else {
					marbles_buttons(fnc, dt);
				}
				/* ⚠️ Disarm only once the button is UP: its release has then been
				 * seen and eaten by the handlers above. M4e cleared the flag here
				 * on EVERY tick, and since the commit happens with the button still
				 * held, the release always arrived to a cleared flag (issue #2). */
				if (sp1_rg_button(&unpatch_eat) >= 0) {
					sp1_rg_tick_end(&unpatch_eat, sp1_button_held(
						(enum sp1_button)(SP1_BTN_T1 +
								  sp1_rg_button(&unpatch_eat))));
				}

				/* ---- FFWD on PLAITS: the burst, running or stopped (M4e) ----
				 * Recomputed every tick, so PLAY or a module change in the middle of
				 * a hold does the right thing.
				 *
				 * ⚠️ ONE mechanism now (Adara: "maybe we want to unify these features
				 * in general"). Through M4d this was a burst while stopped and a
				 * RATCHET while running -- the ratchet multiplying Marbles' RATE, so
				 * the clock genuinely sped up. That is what moved the beat: changing
				 * the rate mid-cycle shifts the next tick, and releasing leaves the
				 * phase wherever it landed. The burst is phase-locked to the master
				 * ramp instead (sp1_synth.h), so it is exact subdivisions of the
				 * clock and releasing changes nothing about the clock at all.
				 *
				 * ⚠️ So FFWD while running RE-TRIGGERS rather than making the sequence
				 * advance faster: a roll, not an arpeggio. Driving Marbles' clock at a
				 * subdivision is a separate idea, parked for a community vote, and is
				 * deliberately not built. */
				const bool ffwd = (g_module == SP1_MODULE_PLAITS) &&
						  sp1_button_held(SP1_BTN_FFWD) && !ffwd_consumed;
				const bool burst = ffwd;
				if (!sp1_button_held(SP1_BTN_FFWD)) {
					ffwd_consumed = false;
				}
				if (burst != burst_was) {
					burst_was = burst;
					sp1_synth_burst(burst);
					if (burst) {
						burst_n0 = sp1_synth_burst_count();
						printk("BURST on: 1/%u at %d BPM (%s)\n",
						       1u << g_burst_div, (int)sp1_mui_bpm(),
						       sp1_marbles_running()
						       ? "locked to Marbles' clock" : "free-running");
					} else {
						printk("BURST off: %u TRIGs\n",
						       sp1_synth_burst_count() - burst_n0);
					}
				}

#if defined(CONFIG_SP1_MIDI)
				/* ---- MIDI (M5a): the prompt, and the control-loop smoothing of
				 * the offsets the two UIs are about to read ---- */
				midi_tick(dt, shutdown_active || rip_ms > 0u || prst_showing ||
					  unpatch_ms >= SP1_UNPATCH_START_MS);
				sp1_midi_main_tick(dt);
				/* Pickup shared / takeover: the CCs move the stored values of
				 * BOTH modules, whichever is on show (nothing in sum). */
				sp1_pui_midi();
				sp1_mui_midi();
#endif

				/* ---- publish: Plaits, the routing, Marbles, the tempo ---- */
				struct sp1_synth_params sp;
				sp1_pui_params(&sp);
				struct sp1_mui_routing rt;
				sp1_mui_routing(&rt);
				for (int k = 0; k < 3; k++) {
					sp.mrb_t_dest[k] = rt.t_dest[k];
				}
				for (int k = 0; k < 4; k++) {
					sp.mrb_dest[k] = rt.dest[k];
				}
				sp.mrb_gtlt = rt.gtlt;
				sp1_synth_set_params(&sp);

				struct sp1_marbles_params mp;
				sp1_mui_params(&mp);
				/* INTELLIGENT (M4c) needs both modules: which parameters of
				 * the CURRENT engine are bipolar. Re-read every tick, so the
				 * range follows an engine change while the sequence plays. */
				mp.engine_centre = sp1_pui_engine_centre();
				sp1_marbles_set_params(&mp);
				sp1_synth_set_tempo(tempo_bpm());

				/* ---- the play-row clock steps on Marbles' t2 ---- */
				const uint32_t b = sp1_marbles_beats();
				if (b != beats_seen) {
					sp1_playrow_clock_step(b - beats_seen);
					beats_seen = b;
				}
			}
#else
			/* ---- fallback build: the M2 test tone. PLAY toggles it. ---- */
			if (sp1_button_pressed(SP1_BTN_PLAY)) {
				sp1_audio_tone_set(!sp1_audio_tone_on());
				printk("TONE %s\n", sp1_audio_tone_on() ? "on" : "off");
			}
#endif
#if defined(CONFIG_SP1_PLAITS)
			/* ---- "••" + VOL: gain PAST maximum, into the soft clipper ----
			 * (Adara, M4b; a plain up/down pair since M4e.) "••" + VOL+ steps up,
			 * "••" + VOL- steps DOWN, and step 0 is off. Works on either module and
			 * on any page. Unshifted VOL is still the output level, below.
			 *
			 * ⚠️ VOL- no longer TOGGLES (M4e, Adara). It used to switch the stage off
			 * and restore the last setting on a second press, which made one rocker
			 * mean "more" in one direction and "all or nothing" in the other -- and
			 * left no way to come down one step. "Off" is now simply the bottom of
			 * the same ladder, which is what a 4-step control should be, and
			 * g_drive_last is gone with it: there is nothing left to remember. */
			if (fnc && (sp1_button_pressed(SP1_BTN_VOL_UP) ||
				    sp1_button_pressed(SP1_BTN_VOL_DOWN)) &&
			    !shutdown_active) {
				const int dir = sp1_button_pressed(SP1_BTN_VOL_UP) ? 1 : -1;
				int d = sp1_synth_drive() + dir;
				d = d < 0 ? 0 : (d > SP1_DRIVE_STEPS - 1 ? SP1_DRIVE_STEPS - 1 : d);
				sp1_synth_set_drive(d);
				const int now = sp1_synth_drive();
				uint8_t bar[4];
				for (int i = 0; i < 4; i++) {
					bar[i] = (i < now) ? SP1_ENGINE_LED_FULL : 0u;
				}
				sp1_display_engine(bar);
				if (now == 0) {
					printk("DRIVE off (no soft clipping)\n");
				} else {
					printk("DRIVE step %d: +%d dB into the soft clipper\n",
					       now, sp1_synth_drive_db(now));
				}
			}
#endif
			/* ---- VOL-/VOL+: output level, 3 dB steps, slewed (no clicks) ----
			 * Two steps is exactly one meter band (6.02 dB). Shifted, VOL is the
			 * drive above instead, so this whole block stands down. */
			if (!fnc && vol_parked && (sp1_button_pressed(SP1_BTN_VOL_DOWN) ||
						   sp1_button_pressed(SP1_BTN_VOL_UP))) {
				/* config/audio.ini level = parked: a host is recording, so VOL stays
				 * where audio.ini put it until the host stops (usb_level_park). */
				printk("LEVEL locked at %d dBFS while a host takes USB audio out"
				       " (config/audio.ini)\n", (int)usb_parked_db());
			}
			if (!fnc && !vol_parked && sp1_button_pressed(SP1_BTN_VOL_DOWN)) {
				sp1_audio_level_step(+1);
			}
			if (!fnc && !vol_parked && sp1_button_pressed(SP1_BTN_VOL_UP)) {
				sp1_audio_level_step(-1);
			}
			if (!fnc && !vol_parked && (sp1_button_pressed(SP1_BTN_VOL_DOWN) ||
						    sp1_button_pressed(SP1_BTN_VOL_UP))) {
				const int d = sp1_audio_level_db_x10();
				if (d <= -9990) {
					printk("LEVEL muted (step %d)\n",
					       sp1_audio_level_get());
				} else {
					printk("LEVEL %s%d.%d dBFS (step %d)\n",
					       d < 0 ? "-" : "", (d < 0 ? -d : d) / 10,
					       (d < 0 ? -d : d) % 10,
					       sp1_audio_level_get());
				}
			}

			/* ---- headphones in -> speaker off (M3) ---- */
#if defined(CONFIG_SP1_USB_AUDIO)
			{
				const bool usb_live = sp1_uac_live();
#if defined(CONFIG_SP1_MIDI)
				/* M5c: the delay the MIDI clock makes up for is the path the host
				 * hears -- USB audio out while a host takes it, the speaker /
				 * headphones otherwise. */
				sp1_midi_set_output_usb(usb_live);
#endif
				usb_level_park(usb_live);
			}
#endif
			switch (sp1_audio_jack_poll(dt)) {
			case 1:
				printk("JACK headphones in: speaker off\n");
				break;
			case 0:
				/* Stays off while a host records USB audio out (event 3/4). */
				printk("JACK headphones out: speaker %s\n",
				       sp1_audio_speaker_on() ? "on" : "off");
				break;
			case 3:
				printk("SPEAKER off: a host is taking USB audio out\n");
				break;
			case 4:
				printk("SPEAKER on: no host is taking USB audio out\n");
				break;
			case 2:
				printk("JACK detect failed 3x: disabled, speaker on\n");
				break;
			default:
				break;
			}

			/* ---- the play row: meter in front, clock behind ----
			 * Everything expensive stays out of the audio path: the thread only
			 * tracks a running max; this turns it into LEDs with one CLZ. */
			if (batt_flash_ms > 0u || batt_fade_ms > 0u) {
				/* Same bar as STANDBY's: full while held, then the fade. */
				uint32_t b = SP1_STANDBY_BAR_LEVEL;
				if (batt_flash_ms == 0u) {
					b = (b * batt_fade_ms) / SP1_BATT_FLASH_FADE_MS;
				}
				sp1_led_bar(SP1_ROW_PLAY, sp1_batt_level(), (uint8_t)b);
				if (batt_flash_ms > 0u) {
					if (batt_flash_ms > dt) {
						batt_flash_ms -= dt;
					} else {
						/* Held the whole time: end in the same fade. */
						batt_flash_ms = 0u;
						batt_fade_ms = SP1_BATT_FLASH_FADE_MS;
					}
				} else {
					batt_fade_ms = batt_fade_ms > dt ? batt_fade_ms - dt : 0u;
				}
				/* No hand-back needed: sp1_playrow_tick redraws the whole row
				 * every tick, so the meter and clock come straight back. Calling
				 * sp1_playrow_reset() here would also restart Marbles' clock
				 * display at the "••" end, which would be wrong. */
			} else {
				sp1_playrow_tick(dt);
			}

			/* Audio health, every 5 s. cyc is the measured worst-case cost of
			 * generating one block against what the block allows -- the
			 * baseline Plaits will be measured against in M3. */
			if ((aud_print += dt) >= 5000u) {
				aud_print = 0;
				struct sp1_audio_stats as;
				sp1_audio_stats(&as);
				/* 64-bit: cyc_max * 10000 wraps a uint32 above ~126 % of
				 * the budget, which is exactly when this line matters. */
				const uint32_t pct_x100 = as.cyc_budget
					? (uint32_t)(((uint64_t)as.cyc_max * 10000u) /
						     as.cyc_budget) : 0u;
				/* The last 5 s on their own (M3): mean and worst block.
				 * The mean is what decides whether an engine fits; the
				 * all-time max is dominated by one-off events. */
				uint32_t wmax, wavg;
				sp1_audio_take_cycles(&wmax, &wavg);
				const uint32_t avg_x100 = as.cyc_budget
					? (uint32_t)(((uint64_t)wavg * 10000u) /
						     as.cyc_budget) : 0u;
				const uint32_t wmax_x100 = as.cyc_budget
					? (uint32_t)(((uint64_t)wmax * 10000u) /
						     as.cyc_budget) : 0u;
				printk("AUD run=%d blk=%u fail=%u rst=%u"
				       "  cyc avg=%u.%02u%% max=%u.%02u%%"
				       " ever=%u.%02u%% (budget %u/blk)  meter=%u jack=%d"
#if defined(CONFIG_SP1_PLAITS)
				       " eng=%d mrb=%d trig=%u drv=%d qnt=%d"
#endif
				       "\n",
				       as.running ? 1 : 0, as.blocks, as.write_fails,
				       as.restarts,
				       avg_x100 / 100u, avg_x100 % 100u,
				       wmax_x100 / 100u, wmax_x100 % 100u,
				       pct_x100 / 100u, pct_x100 % 100u,
				       as.cyc_budget, sp1_meter_level(),
				       sp1_audio_jack_state()
#if defined(CONFIG_SP1_PLAITS)
				       , sp1_pui_engine(), sp1_marbles_running() ? 1 : 0,
				       sp1_synth_trig_edges() - trig_print0,
				       sp1_synth_drive(), sp1_pui_scale()
#endif
				       );
#if defined(CONFIG_SP1_PLAITS)
				trig_print0 = sp1_synth_trig_edges();

				/* Where those blocks went (issue #22), as avg/max percent
				 * of the budget per section -- see sp1_audio.h. A line of
				 * its own: the AUD line is already as long as one CDC
				 * burst should be. The section maxima need not come from
				 * the same block, so they do not add up to AUD's max. */
				struct sp1_audio_sections sc;
				sp1_audio_take_sections(&sc);
				uint32_t a10[SP1_SEC_N], m10[SP1_SEC_N];
				for (int i = 0; i < SP1_SEC_N; i++) {
					a10[i] = as.cyc_budget
						? (uint32_t)(((uint64_t)sc.avg[i] * 1000u) /
							     as.cyc_budget) : 0u;
					m10[i] = as.cyc_budget
						? (uint32_t)(((uint64_t)sc.max[i] * 1000u) /
							     as.cyc_budget) : 0u;
				}
				printk("CPU eng=%u.%u/%u.%u mrb=%u.%u/%u.%u rte=%u.%u/%u.%u"
				       " post=%u.%u/%u.%u out=%u.%u/%u.%u  ovr=%u run=%u\n",
				       a10[SP1_SEC_ENG] / 10u, a10[SP1_SEC_ENG] % 10u,
				       m10[SP1_SEC_ENG] / 10u, m10[SP1_SEC_ENG] % 10u,
				       a10[SP1_SEC_MRB] / 10u, a10[SP1_SEC_MRB] % 10u,
				       m10[SP1_SEC_MRB] / 10u, m10[SP1_SEC_MRB] % 10u,
				       a10[SP1_SEC_RTE] / 10u, a10[SP1_SEC_RTE] % 10u,
				       m10[SP1_SEC_RTE] / 10u, m10[SP1_SEC_RTE] % 10u,
				       a10[SP1_SEC_POST] / 10u, a10[SP1_SEC_POST] % 10u,
				       m10[SP1_SEC_POST] / 10u, m10[SP1_SEC_POST] % 10u,
				       a10[SP1_SEC_OUT] / 10u, a10[SP1_SEC_OUT] % 10u,
				       m10[SP1_SEC_OUT] / 10u, m10[SP1_SEC_OUT] % 10u,
				       sc.over, sc.over_run);
				/* #32: rte's once-per-audio-block part (`pre`: params, MIDI begin,
				 * Marbles' clock, routing) as avg/max percent of the budget -- the
				 * cost that grows as blocks get shorter -- and, in a diagnostic build,
				 * flash-cache misses per ms of audio by section (rte here is the
				 * per-Plaits-block glue only) and for every thread. */
				{
					const uint32_t pa = as.cyc_budget ? (uint32_t)(((uint64_t)
						sc.pre_avg * 1000u) / as.cyc_budget) : 0u;
					const uint32_t pm = as.cyc_budget ? (uint32_t)(((uint64_t)
						sc.pre_max * 1000u) / as.cyc_budget) : 0u;
					/* B1: two parts of pre, avg percent: MIDI's begin + clock,
					 * and routing. The rest is params, mixes and the drive ramp. */
					const uint32_t pmi = as.cyc_budget ? (uint32_t)(((uint64_t)
						sc.pre_midi_avg * 1000u) / as.cyc_budget) : 0u;
					const uint32_t prt = as.cyc_budget ? (uint32_t)(((uint64_t)
						sc.pre_route_avg * 1000u) / as.cyc_budget) : 0u;
					/* #32: the slowest single engine call, in microseconds. */
					const uint32_t wus = sc.eng_worst / 64u;
					printk("BLK pre=%u.%u/%u.%u (midi=%u.%u route=%u.%u) plaits block %u"
					       "  slowest engine call %u us (plaits %u%s)\n",
					       pa / 10u, pa % 10u, pm / 10u, pm % 10u,
					       pmi / 10u, pmi % 10u, prt / 10u, prt % 10u,
					       (unsigned)SP1_SYNTH_BLOCK, wus, sc.eng_worst_engine,
					       sc.eng_worst_first ? ", first after a change" : "");
#if defined(CONFIG_SP1_PROFILE_ICACHE)
					/* Audio ms in the window: blocks x the block, which is the
					 * budget over 64 000 cycles per ms. */
					const uint32_t ms = (uint32_t)(((uint64_t)sc.blocks *
						as.cyc_budget) / 64000u);
					const uint32_t d = ms ? ms : 1u;
					const uint32_t all = sc.icache_hit + sc.icache_miss;
					const uint32_t pc10 = all ? (uint32_t)(((uint64_t)
						sc.icache_miss * 1000u) / all) : 0u;
					printk("MISS/ms eng=%u mrb=%u pre=%u rte=%u"
					       " post=%u out=%u  all=%u (%u.%u%% of fetches)\n",
					       sc.miss[SP1_SEC_ENG] / d, sc.miss[SP1_SEC_MRB] / d,
					       sc.miss_pre / d, sc.miss[SP1_SEC_RTE] / d,
					       sc.miss[SP1_SEC_POST] / d, sc.miss[SP1_SEC_OUT] / d,
					       sc.icache_miss / d, pc10 / 10u, pc10 % 10u);
#endif
				}
				/* ...and where the whole CPU went, per thread: the USB stack runs above
				 * audio, so the sections above cannot show its share (sp1_threads.h). */
				sp1_threads_report();
#endif
#if defined(CONFIG_SP1_MIDI)
				/* MIDI health, only while a host has the port or something
				 * arrived (M5a). Totals since boot, except drop: messages lost
				 * to a full queue since this ON began. While the device is off
				 * nothing drains the queue, so drops then mean nothing. */
				{
					static uint32_t rx_seen;
					struct sp1_midi_stats ms;
					sp1_midi_get_stats(&ms);
					if (sp1_midi_port_up() || ms.received != rx_seen) {
						rx_seen = ms.received;
						printk("MIDI port=%d act=%d rx=%u drop=%u notes=%u cc=%u"
						       " ign=%u held=%u bend=%u clk=%s %u.%u BPM ticks=%u\n",
						       sp1_midi_port_up() ? 1 : 0,
						       sp1_midi_active() ? 1 : 0, ms.received,
						       ms.dropped - midi_drop0, ms.notes, ms.ccs, ms.ignored,
						       ms.held, ms.bend_range, ms.clock_ext ? "midi" : "own",
						       ms.bpm10 / 10u, ms.bpm10 % 10u, ms.ticks);
					}
					/* Diagnostics (M5b): the host's tick spacing over the last
					 * 5 s from the USB timestamps (164 BPM = 15.244 ms), the
					 * transport as RECEIVED, fresh starts of the clock's line,
					 * and USB packets nothing took. Only when there is news. */
					static uint32_t rej_seen;
					uint32_t rej;
					uint8_t last[4];
					sp1_midi_usb_rejects(&rej, last);
					/* cin5: clock / transport that came as CIN 0x5 packets
					 * and was taken anyway (sp1_usbd.c) -- the OP-XY? */
					static uint32_t cin5_seen;
					const uint32_t cin5 = sp1_midi_usb_rt_cin5();
					if (ms.iv_n > 0u || rej != rej_seen || cin5 != cin5_seen) {
						rej_seen = rej;
						cin5_seen = cin5;
						printk("MIDI clk iv=%u.%03u/%u.%03u/%u.%03u ms (min/avg/max, "
						       "n=%u) rx start=%u cont=%u stop=%u mmc play=%u stop=%u"
						       " resets=%u cin5=%u rej=%u last=%02x %02x %02x %02x\n",
						       ms.iv_min_us / 1000u, ms.iv_min_us % 1000u,
						       ms.iv_avg_us / 1000u, ms.iv_avg_us % 1000u,
						       ms.iv_max_us / 1000u, ms.iv_max_us % 1000u, ms.iv_n,
						       ms.rx_start, ms.rx_cont, ms.rx_stop, ms.mmc_play,
						       ms.mmc_stop, ms.line_resets,
						       cin5, rej, last[0], last[1], last[2], last[3]);
						/* #32: the host's notes against its own clock (sp1_midi.h). */
						if (ms.skew_n > 0u) {
							const int32_t a = ms.skew_avg_us;
							const uint32_t m = (uint32_t)(a < 0 ? -a : a);
							printk("MIDI notes vs clock: %c%u.%u ms (sd %u.%u ms, n=%u)"
							       "  - = notes before their tick: the clock leaves late."
							       "  Marbles leads by %u.%u ms\n",
							       a < 0 ? '-' : '+', m / 1000u, (m % 1000u) / 100u,
							       ms.skew_sd_us / 1000u,
							       (ms.skew_sd_us % 1000u) / 100u, ms.skew_n,
							       ms.lead_us / 1000u, (ms.lead_us % 1000u) / 100u);
						}
					}
				}
#endif
#if defined(CONFIG_SP1_USB_AUDIO)
				/* USB audio out (M5c), while the host has the stream open and once more
				 * after it closes. Counts are since the stream opened (the ring's),
				 * except replaced and sr (since boot). fill hovers near the target when
				 * the clocks agree; 47s / 49s are the regulator following drift
				 * (~10 a second at +/-200 ppm, none from load); under means a packet
				 * went out silent, over a block found no room. */
				{
					static bool uac_was_open;
					static uint32_t uac_opens_seen;
					struct sp1_uac_stats us;
					sp1_uac_get_stats(&us);
					if (us.open || uac_was_open || us.opens != uac_opens_seen) {
						printk("UAC open=%d opens=%u pkts=%u fill=%u (target %u)"
						       " 47s=%u 49s=%u under=%u over=%u replaced=%u sr=%u\n",
						       us.open ? 1 : 0, us.opens, us.packets, us.fill,
						       us.target, us.n47, us.n49, us.under, us.over,
						       us.replaced, us.sr_requests);
					}
					uac_was_open = us.open;
					uac_opens_seen = us.opens;
				}
#endif
			}

			/* NOTE: charger status is NOT shown on the model row in ON any
			 * more. The row's resting state is the fader meter, and writing
			 * status LEDs into the same storage four times a second would
			 * simply fight it. STANDBY still shows plugged/charging properly
			 * -- that is the state where you are watching it charge -- and in
			 * ON the console's usb=/chg= fields cover it. */

			sp1_led_tick(dt);
			k_msleep(TICK_MS);
		}
	}

	return 0;
}
