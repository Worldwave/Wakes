/*
 * wakes-sp1 — UI timing constants, in one place so the feel can be tuned
 * without hunting through the state machines.
 */
#ifndef SP1_UI_TIMING_H
#define SP1_UI_TIMING_H

/* Control-loop period. Everything below is quantised to this, so keep fade and
 * gap values comfortably larger than one tick. */
#define SP1_TICK_MS              8u

/* ---- "••" shutdown gesture ----
 * Total time to power off = HOLD + RAMP_UP + 4 x (FADE + GAP)
 *                         = 3000 + 500 + 4 x 200 = 4300 ms.
 * Long on purpose: "••" is also the shift for the whole UI, so shutdown must be
 * deliberate and it must be cancellable right up to the last moment. */
#define SP1_PWR_HOLD_MS       3000u   /* hold before the animation starts     */
#define SP1_PWR_RAMP_UP_MS     500u   /* model row fades up together          */
#define SP1_PWR_FADE_MS        150u   /* per-LED fade to dark                 */
#define SP1_PWR_GAP_MS          50u   /* pause between LEDs                   */
#define SP1_PWR_CANCEL_FADE_MS 120u   /* fade back to dark when cancelled     */

/* ---- THE BACKSTOP. Read this before touching anything above. ----
 * A continuous "••" hold this long powers the device off NO MATTER WHAT: whatever
 * the shift layer thinks, whatever state the gesture is in, armed or not.
 *
 * It exists because of a real failure on 2026-09-19. Shift suppression -- a UX
 * nicety -- was wired to the shutdown gesture through a control-layer heuristic,
 * and when that heuristic latched on, the device could not be turned off at all.
 * On a device with no reset pin and no runtime DFU trigger, that is very close to
 * the one outcome the whole safety floor exists to prevent: the only remaining way
 * out was to unplug it and wait for the battery to die.
 *
 * So: power-off must never be reachable only through a conditional. Anything that
 * can say "not now" gets overruled here.
 *
 * ---- why 20 s and not 6 ----
 * 6 s collided with the real use of the shift layer. Holding "••" IS the
 * modulation-offset gesture, and it gets held for extended stretches while working on
 * parameters -- during which the device would power itself off. A backstop that fires
 * during ordinary playing is not a safety net, it is a fault.
 *
 * 30 s is still far past any accidental hold -- nobody rests a thumb on a button for
 * thirty seconds by mistake -- so the escape hatch survives intact: longer to wait,
 * never unavailable.
 *
 * ⚠️ 20 s -> 30 s in M4e (Adara), because the SHIFT layer now has a 2 s Unpatch hold in
 * it and long working sessions on the shift pages were still reaching the backstop. The
 * change is to the THRESHOLD ONLY -- exactly as when 6 s became 20 s -- and the warning
 * below moved with it so the last 5 s still warn.
 *
 * ⚠️ Do not "fix" that collision by making this conditional on the shift layer being
 * idle. That is precisely the mistake of 2026-09-19: a heuristic deciding whether
 * power-off is allowed. Raising the threshold costs patience; conditioning it costs the
 * guarantee. The warning below is the other half of the answer.
 *
 * ⚠️ Do not add a condition to this. That is the entire point of it. */
#define SP1_PWR_FORCE_HOLD_MS 30000u

/* ---- the warning, so a 20 s force-off is never a surprise ----
 * Unconditional timing is non-negotiable; being startled by it is not. From
 * SP1_PWR_FORCE_WARN_MS the model row PULSES, meaning "release now or I power off".
 * Releasing resets the counter, so a long shift session only needs a lift of the thumb.
 *
 * Deliberately a pulse and not the shutdown ramp: it must not be mistaken for the
 * ordinary 3 s animation, which by this point has either completed or been suppressed.
 *
 * ---- and it ENDS in the shutdown animation (M2b, Adara) ----
 * The last pulse rises to full and, instead of falling back, hands over to the
 * ordinary shutdown's per-LED fade-out (T1 -> T4), which finishes exactly at
 * SP1_PWR_FORCE_HOLD_MS. So a backstop power-off looks like a power-off.
 *
 * All of it is drawn as a pure function of the hold time. It does NOT gate the
 * force-off, which still fires at SP1_PWR_FORCE_HOLD_MS whatever was drawn.
 *
 * The pulse period is chosen so a pulse PEAK lands exactly where the fade-out starts:
 * (SEQ_START - WARN) must be an odd number of half-pulses. 4200 ms = 7.5 x 560 ms --
 * which is why WARN moved 15 s -> 25 s with HOLD's 20 s -> 30 s: the GAP between them is
 * what the relation constrains, not either value, so keeping it at 5 s keeps the
 * hand-over exact and leaves the animation byte-identical.
 * sp1_power.c BUILD_ASSERTs that, so retuning any of these cannot silently put a
 * jump into the hand-over. */
#define SP1_PWR_FORCE_WARN_MS  25000u
#define SP1_PWR_FORCE_PULSE_MS   560u
#define SP1_PWR_FORCE_SEQ_MS   (4u * (SP1_PWR_FADE_MS + SP1_PWR_GAP_MS))   /* 800 */
#define SP1_PWR_FORCE_SEQ_START (SP1_PWR_FORCE_HOLD_MS - SP1_PWR_FORCE_SEQ_MS)

/* ---- Power-ON hold gate ----
 * Wake from SYSTEM_OFF is a hardware level-sense on P0.27: the chip wakes and
 * resets the instant "••" goes low, with no CPU running to time a hold. So the
 * hold cannot be enforced BEFORE waking -- it is enforced immediately after, and
 * a release before the threshold sends the device straight back to sleep. The
 * user cannot tell the difference: a short press does nothing.
 *
 * DARK_MS is a deliberate blackout at the start of the hold: nothing is DRAWN until
 * it has elapsed, so a tap never reaches the drawing code. FILL_MS is the visible
 * progress fill after that.
 *
 * ⚠️ The blackout alone did NOT stop the tap-flicker (confirmed on hardware,
 * M1b). The remaining flash happens BEFORE any of our code draws: Zephyr
 * initialises the PWM peripherals pre-main, pinctrl claims the LED pins, and the
 * peripheral drives them to its idle level for the few ms until our first duty
 * write. With `nordic,invert` on those channels that idle level lights the LEDs.
 * The fix is to defer PWM device init until we have decided to boot -- see
 * sp1_led_init() and `zephyr,deferred-init` in the board DTS. */
#define SP1_PWR_ON_DARK_MS     500u   /* silent, nothing drawn                */
#define SP1_PWR_ON_FILL_MS    1000u   /* model row fills as progress          */
#define SP1_PWR_ON_HOLD_MS    (SP1_PWR_ON_DARK_MS + SP1_PWR_ON_FILL_MS)

/* ---- STANDBY (off, plugged in) ----
 * The play row is a charge bar filling from the "••" end toward PLAY. It breathes
 * while charging and sits solid when complete. Status lights live on the model
 * row at T2/T3, dim so they read as indicators rather than as UI. */
#define SP1_STANDBY_STATUS_LEVEL   38u   /* ~15 % of 255 -- T2/T3 status       */
#define SP1_STANDBY_BAR_LEVEL      77u   /* ~30 % of 255 -- the charge bar     */
#define SP1_STANDBY_BREATH_MS    2200u   /* one full breath cycle              */
#define SP1_STANDBY_BREATH_DEPTH   60u   /* peak-to-trough at FULL brightness;
                                          * scaled down with the bar level so a
                                          * dim bar does not breathe to nothing */
#define SP1_STANDBY_POLL_MS      1000u   /* battery re-read in STANDBY         */

/* ---- "••" tap: the battery, for 1.5 s (Adara, M4c) ----
 * On the PLAY ROW, drawn exactly as STANDBY's charge bar (sp1_led_bar at
 * SP1_STANDBY_BAR_LEVEL), so the same picture means the same thing wherever you see it.
 *
 * It appears the instant "••" goes DOWN, anywhere, on either module -- and is CANCELLED by
 * any other control being touched, so it can never be in the way of the shift layer you
 * were reaching for. That is what makes it free: it costs nothing to show because it gets
 * out of the way by itself.
 *
 * ⚠️ It ALWAYS leaves by a SP1_BATT_FLASH_FADE_MS fade (M4e, Adara): on "••" release, or
 * at SP1_BATT_FLASH_MS if "••" is still held. Through M4c a release CUT it instantly --
 * and since a tap is under 300 ms against a 1500 ms display, the release always won and
 * the fade at the end was unreachable. That is why it looked like it never faded: it
 * never got there.
 *
 * ⚠️ The one exception stays a hard cut: another control being touched. That is a CANCEL,
 * not the end of a display -- you have asked for something else and want to see it now.
 *
 * ⚠️ It lives on the PLAY row on purpose. The shutdown animation, the 20 s warning pulse
 * and every shift-page indication own the MODEL row, so this cannot collide with any of
 * them -- and it must not, because "••" down is also how a shutdown begins.
 *
 * ⚠️ From OFF it is drawn on RELEASE, never during the press. The 0.5 s
 * SP1_PWR_ON_DARK_MS blackout exists so a tap shows nothing at all, and drawing during the
 * press would undo it. A press shorter than SP1_BATT_FLASH_MIN_MS is a brush and still
 * shows nothing, so the pins are never claimed for one. */
#define SP1_BATT_FLASH_MS       1500u   /* the longest it stays up, "••" still held */
#define SP1_BATT_FLASH_FADE_MS   300u   /* the fade-out, always (M4e)               */
#define SP1_BATT_FLASH_MIN_MS    100u   /* from OFF: shorter than this is a brush   */

/* ---- diagnostics cost control ----
 * A battery read is ~400 us (16x oversampled, 20 us acquisition) and a status
 * printk is a few hundred more. At 1 Hz that is ~0.07 % of the CPU, which is
 * nothing -- but in ON the concern is not the average, it is BLOCKING inside an
 * audio block (5 ms at 48 kHz / 240 frames, from M3). So in ON both are slowed right
 * down: nothing needs a per-second battery reading while you are playing.
 *
 * SP1_CONSOLE_PERIOD_OFF disables status lines entirely. M2 should use it once
 * audio is live unless actively debugging: printk over CDC ACM can block for an
 * unbounded time if the host stalls, and that is a dropout, not a percentage. */
#define SP1_BATT_POLL_ON_MS    10000u   /* battery re-read in ON               */
#define SP1_CONSOLE_PERIOD_STANDBY_MS 1000u
#define SP1_CONSOLE_PERIOD_ON_MS      5000u
#define SP1_CONSOLE_PERIOD_OFF           0u

/* ---- model-row display (T1-T4) ----
 * Long enough to read a fader value after you stop moving, short enough that the
 * row goes back to its resting display without feeling stuck. */
#define SP1_DISP_VALUE_HOLD_MS  1200u
#define SP1_DISP_FADE_MS         250u
#define SP1_DISP_MODEL_MS       1000u   /* model overlay after a change */

/* ---- engine flash (Adara, M3b): instant, brief, fades back into the page ----
 * Patterns: SLOT_GLYPHS in tools/gen_engines.py, one per slot. */
/* "••" + PLAY held: rip out the cables (Adara, M4) -- the module on show back to its
 * defaults. The track row flickers twice, fades to black, stays black, and -- only if
 * the hold lasts SP1_RIP_HOLD_MS -- the reset happens and the page fades back in.
 * Letting go earlier cancels. */
#define SP1_RIP_HOLD_MS         3000u   /* to complete                              */
#define SP1_RIP_FLICKER_MS        70u   /* each on / off of the two flickers        */
#define SP1_RIP_BLACK_MS         500u   /* black at the end of the hold             */
#define SP1_RIP_FADEBACK_MS      500u   /* after the reset: black -> the page       */
#define SP1_RIP_CANCEL_FADE_MS   150u   /* let go early: back to the page           */
/* ---- the fresh format at ON entry (Adara, M6 #43) ----
 * The UI waits while wakes-sp1-fresh formats the eMMC. The track row is a progress bar
 * through the format (sp1_led_bar, full brightness); when it completes the row flickers
 * twice, 0 -> 100 %, and fades quickly into the page. A format that fails leaves the bar
 * and fades into the page with no flicker -- the flicker means "done". Only the 30 s
 * backstop powers off meanwhile (storage_gate() in main.c). */
#define SP1_FMT_FLICKER_MS        70u   /* each off / on of the two flickers (as the rip) */
#define SP1_FMT_DONE_FADE_MS     250u   /* then full -> the page                          */
#define SP1_FMT_FAIL_FADE_MS     250u   /* failed: the bar -> the page                    */

/* ---- UNPATCH: "••" held + T1-T4 held (Adara, M4e) ----
 * Clears the routing belonging to that button -- on PLAITS the Marbles outputs aimed at
 * that parameter, on MARBLES that output's destination. The only destructive gesture
 * that reaches a single cable instead of the whole patch.
 *
 * Timing is Adara's, to the millisecond: nothing happens for the first
 * SP1_UNPATCH_START_MS, then a SP1_UNPATCH_ANIM_MS animation on the FOUR TRACK LEDs, and
 * the clear commits when that animation ENDS -- at exactly START + ANIM. Letting go
 * earlier cancels and clears nothing, which is how every other hold on this device
 * behaves. The phases, in order, are in main.c's unpatch_levels().
 *
 * ⚠️ THIS IS THE ONE LONG PRESS IN THE UI, and it is deliberate (Adara, M4e). M4a
 * removed the last one -- [E]'s 2 s model-bank hold, whose two banks are now one ring of
 * six -- and set "no long press anywhere"; this reinstates exactly one, for a destructive
 * action that must be hard to do by accident. It is modified by "••", so it cannot be hit
 * while playing. Do not add a second one without asking.
 *
 * ⚠️ Because T1-T4 now mean two things under "••", their ORDINARY shift action moved to
 * the button's RELEASE, and fires only if Unpatch did not. That is the cost of putting
 * two meanings on one button and there is no way around it.
 *
 * ⚠️ It must not weaken the power path. A "••" + button press is already a shift use, so
 * it suppresses the 3 s shutdown exactly as it did before; the SP1_PWR_FORCE_HOLD_MS
 * backstop is unconditional and is NOT affected by any of this (rule 5a). An Unpatch hold
 * is 2 s against a 30 s backstop, so the two cannot be confused.
 * ⚠️ And nothing in this file that concerns the "••" hold -- SP1_PWR_* above -- may be
 * changed except on Adara's explicit say-so. */
#define SP1_UNPATCH_START_MS    1250u   /* silent; then the animation begins        */
#define SP1_UNPATCH_ANIM_MS     750u    /* fade out, blink, blink, latch, sweep     */
#define SP1_UNPATCH_HOLD_MS     (SP1_UNPATCH_START_MS + SP1_UNPATCH_ANIM_MS)   /* 2000 */
#define SP1_UNPATCH_FADEBACK_MS 250u    /* after the clear: back to the page        */
#define SP1_UNPATCH_CANCEL_MS   150u    /* let go early: back to the page           */
/* The animation's own phases, summing to SP1_UNPATCH_ANIM_MS. Fades, never latches --
 * Adara: "use fast fades instead of instant latches to make this animation feel better". */
#define SP1_UNPATCH_OUT_MS      150u    /* whatever was showing -> 0                */
#define SP1_UNPATCH_BLINK_MS    60u     /* one edge of one blink (up or down)       */
#define SP1_UNPATCH_LATCH_MS    120u    /* 0 -> 100 %, the third blink, and held    */
#define SP1_UNPATCH_SWEEP_MS    240u    /* 100 % -> dark, middle outwards           */
#define SP1_UNPATCH_BLINK_LEVEL 204u    /* 80 % of 255 (Adara)                      */

/* ---- the MIDI prompt (M5a, Adara; M5 plan B9) ----
 * MIDI plugged in -> the Unpatch animation REVERSED (the cable going in); unplugged -> the
 * Unpatch animation as it is. Same SP1_UNPATCH_ANIM_MS, same fade back to the page.
 * "Plugged in" is the host enabling the MIDI port, held this long, because hosts often reset
 * the bus two or three times while enumerating; "unplugged" is only shown after a "plugged
 * in" was. Also shown once on entry to ON when a host is already attached (C10). Cosmetic:
 * the shutdown animation and the backstop warning always win, and an Unpatch or rip hold on
 * the same row makes it stand down. */
#define SP1_MIDI_PROMPT_SETTLE_MS 250u
/* MIDI's MODEL CC moved the engine: the same engine flash as T2/T3 (SP1_DISP_ENGINE_HOLD_MS /
 * _FADE_MS), so there is one animation for "the engine changed" (Adara, M5a round 2 -- a
 * shorter one was hard to read). A sweep restarts it at each change, as scrolling T2/T3 does. */
#define SP1_DISP_ENGINE_HOLD_MS  700u   /* pattern shown solid (Adara: 0.7 s)      */
#define SP1_DISP_ENGINE_FADE_MS  350u   /* then cross-fades into the page         */
/* M3c: glyphs are drawn by hand (SLOT_GLYPHS) -- each LED off, half or full. */
#define SP1_ENGINE_LED_FULL      255u
/* The "◐" level: 33 % perceptual (Adara, M3e -- 50 % was too close to full to tell
 * apart in average room light). Also the "◐" of the burst-division bar. */
#define SP1_ENGINE_LED_HALF       84u   /* 33 % of 255, perceptual (sp1_led applies gamma) */

/* ---- the play row: two layers (M2b, Adara) ----
 * FOREGROUND is the dB meter while Plaits is selected, the clock while Marbles is.
 * BACKGROUND is the other one, capped at SP1_ROW_BG_MAX, so the row is never fully
 * dark while the device is ON -- even with every fader at zero and no sound.
 * Levels are perceptual (sp1_led applies gamma), same scale as the STANDBY values. */
#define SP1_ROW_BG_MAX           26u   /* ~10 % of 255 -- the background layer   */
#define SP1_METER_GLINT          38u   /* ~15 % -- "signal present", LED 1 only   */
/* The clock is Marbles' since M4: RATE at its centre is 120 BPM (Marbles' own default,
 * Adara), and the play-row clock steps on its t2 ticks (sp1_playrow_clock_step). */

#endif /* SP1_UI_TIMING_H */
