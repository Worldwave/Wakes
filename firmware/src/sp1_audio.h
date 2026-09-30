/*
 * wakes-sp1 — audio output: codecs, 48 kHz I2S, output level, headphone detect, and
 * the block loop that runs the synth voice (M3) or the fallback test tone.
 *
 * ---- clock topology (TE's own, and the only one proven on this board) ----
 * The 3.072 MHz oscillator (P0.13) drives the I2S bit clock. The CS42L42 headphone
 * codec is the FRAME master: it divides that clock by 64 to make a frame clock of
 * exactly 48 000 Hz. The nRF52840 I2S peripheral and the TAS2505 speaker amp are both
 * CLOCK SLAVES. sp1-tape-looper found the hard way that having the nRF master the
 * clocks instead (at ~47 619 Hz) crackled on the speaker and gave only noise on the
 * headphones.
 *
 * ---- bring-up order, copied exactly from the proven path. DO NOT "TIDY" IT ----
 *   1. release both codec resets, 20 ms
 *   2. oscillator ON, 5 ms          -> the bit clock now exists
 *   3. start the I2S stream         -> nRF waits as a slave; there is no frame yet
 *   4. configure the TAS2505        -> its PLL locks to the BIT clock, already live
 *   5. configure the CS42L42        -> it starts mastering the FRAME clock, and the
 *                                      stream the nRF queued in step 3 begins to flow
 * Step 3 before step 5 looks backwards and is not: the amp only needs the bit clock,
 * the nRF just waits for frames, and this is the order that works on hardware.
 *
 * ---- priority: audio OUTRANKS main (M3) ----
 * Audio runs at K_PRIO_PREEMPT(0); CONFIG_MAIN_THREAD_PRIORITY=1 in prj.conf puts the
 * main loop below it. In M2 both were 0 and time-sliced (TIMESLICE_PRIORITY=0, 20 ms
 * slices), which was fine for a sine at ~7 % of the CPU and not for Plaits at ~50 %:
 * a 430 us ADC scan or a printk stalled by the host could then sit in front of an
 * audio block. Now the audio thread takes the CPU whenever a block is due, and main
 * runs in the gaps.
 *
 * ---- and therefore the audio thread must NEVER spin ----
 * The main loop is what feeds the watchdog and runs power-off. A thread that spins
 * ABOVE it starves both.
 *
 * What an OVERRUN does (voice slower than real time) -- checked against the pinned
 * i2s_nrfx.c, and not what an earlier version of this comment claimed: the driver
 * misses a buffer ("next buffers not supplied"), the stream goes to I2S_STATE_ERROR,
 * i2s_write() then fails at once, and the write-failure path SLEEPS 2 ms per failure
 * and re-primes after 8. So main still gets time: the device stays up with broken,
 * stuttering audio and a slower control loop, and power-off keeps working. It does
 * NOT starve into a watchdog reset -- so when debugging an overrun in M4, look for
 * `fail`/`rst` climbing on the AUD line, not for a reboot. CONFIG_SP1_PLAITS=n builds
 * the known-good tone firmware.
 *
 * tape-looper's write-failure path does `free; continue` with no sleep, which spins if
 * writes fail instantly. Every path through this thread blocks or sleeps instead. See
 * sp1_audio.c.
 *
 * ---- start/stop are bounded, always ----
 * sp1_audio_stop() is on the POWER-OFF path (sp1_quiesce_peripherals calls it). It
 * never waits more than 50 ms for the thread, whatever state the thread is in, and
 * power-off proceeds regardless -- rule 5a: power-off does not depend on anything
 * else succeeding.
 */
#ifndef SP1_AUDIO_H
#define SP1_AUDIO_H

#include <stdint.h>
#include <stdbool.h>

/* Create the audio thread (it parks immediately) and enable the DWT cycle counter.
 * Once, at boot, after the power-on gate. Safe to call stop() before this. */
void sp1_audio_init(void);

/* Bring the audio path up: resets released, oscillator on, stream started, both
 * codecs configured. Call on every entry to ON -- the way into STANDBY powers all of
 * it down. Bounded: never blocks longer than ~1 s, feeds the watchdog while it waits.
 * Returns 0 on success, negative if the stream could not be started. */
int sp1_audio_start(void);

/* Stop the stream. On the power-off path: waits at most 50 ms for the thread, then
 * returns REGARDLESS. Safe to call when audio was never started. */
void sp1_audio_stop(void);

/* Halt the I2S peripheral at register level and disconnect its pins. ONLY on the way
 * to SYSTEM_OFF -- never STANDBY, because the driver applies its pinctrl once at boot
 * and would not restore the pins. See sp1_audio.c. */
void sp1_audio_halt_for_system_off(void);

/* Peak |sample| (0..32767) of everything produced since the last call, then reset.
 * For the meter. Lock-free: one atomic swap. */
uint32_t sp1_audio_take_peak(void);

/* ---- OUTPUT LEVEL (VOL+ / VOL-) ----
 * 3 dB steps, slewed in the audio path so a change never clicks. Step 0 = 0 dBFS; the
 * last step is mute. Every TWO steps is one meter band (6.02 dB). Applies to the synth
 * and to the fallback tone alike, and to speaker and headphones alike -- the speaker's
 * extra speaker trim is separate and fixed (SP1_SPK_ATTEN_HALF_DB in sp1_audio.c:
 * -9 dB in M3, -6 dB since M4a). */
#define SP1_LEVEL_STEPS     12      /* 0 .. 10 = 0 .. -30 dB, 11 = mute */
#if defined(CONFIG_SP1_PLAITS)
#define SP1_LEVEL_DEFAULT    2      /* -6 dBFS                                */
#else
#define SP1_LEVEL_DEFAULT    4      /* -12 dBFS for the test tone, as in M2   */
#endif

void sp1_audio_level_step(int delta);  /* +1 = quieter by 3 dB, -1 = louder */
int  sp1_audio_level_get(void);
int  sp1_audio_level_db_x10(void);     /* tenths of dBFS, -9990 = mute      */

/* ---- the fallback test tone (CONFIG_SP1_PLAITS=n only): 440 Hz, off at start ---- */
void sp1_audio_tone_set(bool on);
bool sp1_audio_tone_on(void);

/* ---- headphone detect -> speaker mute (M3) ----
 * Call from the main loop every tick while ON, after sp1_audio_start(). Polls the
 * CS42L42 jack sense every 40 ms, debounced, and powers the speaker down while a plug
 * is in. Returns -1 (nothing new), 0 (unplugged: speaker on), 1 (plugged: speaker
 * off), or 2 (detect failed repeatedly: disabled for this session, speaker ON). */
int sp1_audio_jack_poll(uint32_t elapsed_ms);
int sp1_audio_jack_state(void);        /* 1 in, 0 out, -1 unknown */

/* Block cost since the last call: worst and mean cycles per block. Read-and-clear. */
void sp1_audio_take_cycles(uint32_t *max, uint32_t *avg);

/* Where the blocks since the last call went (issue #22): mean and worst cycles per block
 * for each section, and how often the whole block ran over. Read-and-clear, like
 * sp1_audio_take_cycles(), and meant to be read in the same place.
 *
 *   ENG / MRB / POST   sp1_synth.h's sections
 *   RTE                the rest of sp1_synth_render(): Marbles -> Plaits routing, TRIG
 *   OUT                the rest of the block: gain ramp, stereo copy, meter peak
 *
 * `over` counts blocks that cost more than the budget; `over_run` is the longest run of
 * them in a row. A long run is what starves the main loop -- one late block is absorbed
 * by the I2S queue, a second of them is not. */
enum {
	SP1_SEC_ENG, SP1_SEC_MRB, SP1_SEC_RTE, SP1_SEC_POST, SP1_SEC_OUT, SP1_SEC_N
};
struct sp1_audio_sections {
	uint32_t avg[SP1_SEC_N];
	uint32_t max[SP1_SEC_N];
	uint32_t over;
	uint32_t over_run;
};
void sp1_audio_take_sections(struct sp1_audio_sections *out);

/* ---- diagnostics ---- */
struct sp1_audio_stats {
	uint32_t blocks;         /* blocks handed to I2S                         */
	uint32_t write_fails;    /* i2s_write errors                             */
	uint32_t restarts;       /* stream drop + re-prime after repeated fails  */
	uint32_t cyc_max;        /* worst cycles spent generating one block      */
	uint32_t cyc_budget;     /* cycles available per block                   */
	int      cfg_rc;         /* last i2s_configure() result                  */
	bool     running;
	bool     tas_ok;         /* every TAS2505 write ACKed                    */
	bool     hp_ok;          /* CS42L42 identified and configured            */
};
void sp1_audio_stats(struct sp1_audio_stats *out);

#endif /* SP1_AUDIO_H */
