/*
 * wakes-sp1 — MIDI in (M5a): notes into Plaits, CCs onto both modules.
 *
 * The plan, and every decision behind it, is private/docs/M5-PLAN.md. The user-facing side is
 * config/midi.ini (the MIDI script, read at build time by tools/gen_midi.py into
 * sp1_midi_gen.h) and docs/MIDI.md.
 *
 * Framework-agnostic, like sp1_synth.cc: no Zephyr header, so the host suite runs it as is.
 * The USB side (feldd's class, our sp1_usbd.c) only ever calls the two producer functions.
 *
 * ---- threads ----
 *   sp1_midi_push()                         the USB INTERRUPT (#32: the bulk OUT fast path,
 *                                           zephyr-patches/udc_nrf-fast-paths.patch; the
 *                                           usbd thread before). Never block, never take a
 *                                           lock: a lock-free ring, drop-and-count when full.
 *   sp1_midi_port()                         the usbd thread (enable / disable / bus events).
 *   sp1_midi_audio_*()                      AUDIO THREAD ONLY (from sp1_synth_render). This
 *                                           is where every message is parsed and every
 *                                           piece of MIDI state lives, so nothing is shared
 *                                           mid-update.
 *   everything else                         main thread; reads single words the audio
 *                                           thread publishes.
 *
 * ---- timing ----
 * Each message is stamped with the system clock (SP1_MIDI_STAMP_HZ) when USB hands it over.
 * At the start of each audio block the audio thread takes everything that arrived since the
 * previous block started and places each message at the matching one of that block's twenty
 * Plaits blocks. Latency is therefore CONSTANT -- one audio block plus the I2S queue -- and
 * the timing jitter is USB's own (~1 ms), not the 5 ms of the audio block.
 * The stamp is NOT the CPU's cycle counter: that one stops while the CPU sleeps (idle), so a
 * span measured with it misses the idle part of the block and a message was placed up to the
 * idle time early -- ~0.8 ms at 17 % idle. Measured on hardware (Adara's logs, 2026-10-02):
 * tick spacing at 135 BPM read 15.4 ms at 71 % load and 18.5 ms at 97 %, where 18.52 is right.
 * The system clock (the RTC on the nRF52840, 30.5 us) never stops, and 30.5 us is an eighth
 * of a Plaits block.
 *
 * ---- pickup: how a CC and its fader share a parameter (Adara, M5a round 4) ----
 * The MIDI script's `pickup` setting, fixed at build time (SP1_MIDI_PICKUP). All three use
 * Plaits' catch-up (plaits/pot_controller.h), as the faders do:
 *   sum       the CC is an OFFSET on top of the fader, read by polarity (below). The UIs and
 *             the synth add sp1_midi_offset() / the audio block's offsets.
 *   shared    the CC is a POSITION, 0 = the bottom, 1 = the top, on the fader's OWN stored
 *             value: each tick the UIs hand every value to sp1_midi_drive(), which moves it
 *             when the CC moves -- catching up first if the two do not match -- and the
 *             fader catches up after it in turn. No offset at all, so nothing to read by
 *             polarity and nothing that returns to neutral at a disconnect.
 *   takeover  as shared, and while the port is up a fader whose parameter has a CC rests
 *             (sp1_midi_fader_held); after the port goes, it catches up.
 * MODEL has no fader: it is an offset in every setting. [bind] sources are offsets in every
 * setting too.
 *
 * ---- how a CC reads in sum: by the parameter's polarity (Adara's M5a test notes) ----
 * The rule Marbles' INTELLIGENT range uses (M4c), applied to CCs: a BIPOLAR parameter -- one
 * whose centre is its neutral point -- takes a CENTRED CC (64 = no change, 0 / 127 = half a
 * fader's travel down / up, i.e. from a centred fader exactly to either end); a UNIPOLAR one
 * takes a ONE-SIDED CC (0 = no change, 127 = a whole travel up, i.e. from a fader at 0 exactly
 * to the top). Either way, from the fader's neutral position every CC value does something. Which is which is tools/gen_midi.py's POLARITY table; for TIMBRE, MORPH and
 * HARMONICS it is the playing engine's detent bits (SP1_ENGINE_TABLE[].centre), exactly as
 * INTELLIGENT reads them. Both readings are kept, and the reader picks at the moment it
 * applies the offset. When an engine change flips a reading, the offset stays where it was
 * and catches up with the host knob as it moves (sp1_midi.cc, PickupTarget) -- nothing jumps.
 *
 * ---- MIDI clock and transport -> Marbles (M5b; M5 plan, B8, C5, C6) ----
 * The script's `clock = on` (the default). Clock (F8, 24 per beat), Start (FA), Continue (FB)
 * and Stop (FC) are queued and placed like every other message. Once per audio block the audio
 * thread turns the ticks into a POSITION IN BEATS for each Plaits block: the tempo is the
 * average tick interval over the last beat, and between ticks the position moves at that tempo
 * but never past the next tick -- so a host that stops sending ticks leaves Marbles waiting, as
 * a Eurorack Marbles waits on a stopped clock. sp1_marbles.cc turns the position into Marbles'
 * external-clock ramp through Marbles' own RATE ratio table (1/4 ... 4, x the t range).
 *   - The first tick or Start makes Marbles' clock EXTERNAL, until the port goes (C5).
 *   - Start stops Marbles and arms it: the next tick is beat 1, where it resets and runs, on
 *     that tick's Plaits block. Continue runs it without a reset. Stop stops it. PLAY still
 *     runs and stops it locally; the next Start / Stop wins (C6). Song Position: ignored.
 *   - Pulling the cable while MIDI clocks Marbles STOPS Marbles and gives it back its own
 *     clock (C5).
 * Clock is not a channel message: it never makes MIDI "active" (no quantizer bypass, no
 * wider FREQUENCY detent).
 *
 * ---- three states (M5 plan, B6) ----
 *   port up      the host has enabled our MIDI interface. Drives the plug/unplug prompt.
 *   active       a valid message has arrived on our channel since the port came up, or a
 *                MIDI note's release is still sounding. Drives the note path, the CC
 *                offsets and the FREQUENCY quantizer bypass (Adara, C8).
 *   external clock  M5b, not in this build.
 * A disconnect -- the host disabling the interface, a bus reset, suspend, VBUS gone -- and
 * every entry to ON put MIDI back to NEUTRAL: notes released (LEVEL closes through its own
 * tail first), every CC offset glides back to zero, bend to centre, bend range to the
 * script's.
 */
#ifndef SP1_MIDI_H
#define SP1_MIDI_H

#include <stdbool.h>
#include <stdint.h>

#include "sp1_midi_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The Plaits parameters the AUDIO thread applies (the first destinations of the generated
 * enum, FREQUENCY .. LEVEL -- sp1_midi.cc checks that). */
#define SP1_MIDI_AUDIO_DESTS (SP1_MIDI_D_LEVEL + 1)

/* Wakes' own delay from a MIDI message arriving to its sound at the output, in ms -- what
 * the clock leads by with `clock_lead = auto` (M5b). Counted from the code, not measured:
 * (CONFIG_I2S_NRFX_TX_BLOCK_COUNT + 3) audio blocks --
 *   1 block              the message is placed in the NEXT audio block, at its own moment
 *   queue + 2 blocks     when a block starts rendering, the I2S queue is full, the DMA holds
 *                        one more, and one is playing.
 * 5 ms x (4 + 3) = 35 ms through M5a, 5 ms x (2 + 3) = 25 ms in v0.5.0, 2 ms x (2 + 3) =
 * 10 ms since #32. Taken from the build settings; the host suites, which have none, get the
 * default's 10. Section 7 of the M5 test issue measures it. */
#if defined(CONFIG_SP1_AUDIO_BLOCK_FRAMES) && defined(CONFIG_I2S_NRFX_TX_BLOCK_COUNT)
#define SP1_MIDI_OUTPUT_LATENCY_MS \
	((CONFIG_I2S_NRFX_TX_BLOCK_COUNT + 3) * (CONFIG_SP1_AUDIO_BLOCK_FRAMES / 48))
#else
#define SP1_MIDI_OUTPUT_LATENCY_MS 10
#endif

#if defined(CONFIG_SP1_MIDI)

/* ---- USB side (the interrupt, and the usbd thread for port changes) ---- */
/* The stamps' clock, ticks per second: the system clock (CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC,
 * which sp1_usbd.c checks). Any rate works -- placement only uses ratios of spans -- but the
 * tick-spacing diagnostics convert with it. */
#ifndef SP1_MIDI_STAMP_HZ
#define SP1_MIDI_STAMP_HZ 32768u
#endif

/* One channel message (status byte first; `len` 2 or 3) as it came off the wire, stamped
 * with the system clock (SP1_MIDI_STAMP_HZ, "timing" above). Already validated
 * (usb_rt_parse.c). A real-time byte (clock / transport) has `len` 1. */
void sp1_midi_push(const uint8_t msg[3], uint8_t len, uint32_t cycles);
/* MMC transport (M5b round 2): feed every USB-MIDI packet that is SysEx (CIN 0x4-0x7). When a
 * packet completes an MMC Play / Deferred Play or Stop / Pause (F0 7F <device> 06 <cmd> F7),
 * returns the byte to push as real-time (len 1) -- Play and Stop as two codes MIDI leaves
 * undefined, handled with the clock. 1 = a whole SysEx that is not MMC transport (count it as
 * ignored); 0 = nothing finished yet. The OP-XY sends its transport this way. */
uint8_t sp1_midi_mmc_feed(const uint8_t pkt[4]);
/* The host enabled (up) or lost (down) our MIDI interface. */
void sp1_midi_port(bool up);

/* ---- main thread ---- */
/* On every entry to ON: forget anything that queued while OFF and start from neutral. */
void sp1_midi_on_enter(void);
/* Once per control tick, before the UI computes its parameters: advances the control-loop
 * smoothing of the Marbles offsets. */
void sp1_midi_main_tick(uint32_t elapsed_ms);
/* A destination's offset in FADER-TRAVEL units, -1 .. +1 (a whole fader's travel either
 * way). Control-loop kinds come smoothed; stepped kinds and the Plaits ones come as their
 * target, for decisions (LEVEL's connect threshold, range mode 9) the control loop makes. */
float sp1_midi_offset(int dest);
bool  sp1_midi_active(void);
bool  sp1_midi_port_up(void);
/* Pickup shared / takeover, after sp1_midi_main_tick: let destination `dest`'s CC move `*value`
 * (fader space, 0..1, before detents) by Plaits' catch-up. Returns true when it moved it, and
 * then the caller's fader must re-check its own pickup against the new value. Always false in
 * sum, for MODEL, for -1 and for a CC that has not arrived. */
bool  sp1_midi_drive(int dest, float *value);
/* Pickup takeover: the port is up and `dest` has a CC, so its fader must not move it. */
bool  sp1_midi_fader_held(int dest);

struct sp1_midi_stats {
	uint32_t received;    /* messages queued by USB                       */
	uint32_t dropped;     /* ...and lost to a full queue                  */
	uint32_t notes;       /* note-ons on our channel                      */
	uint32_t ccs;         /* control changes on our channel               */
	uint32_t ignored;     /* other channels and unused message types      */
	uint8_t  held;        /* keys down now                                */
	uint8_t  bend_range;  /* semitones now (script, or the host's RPN 0)  */
	bool     clock_ext;   /* Marbles follows MIDI clock (M5b)             */
	uint16_t bpm10;       /* the host's tempo x 10, 0 = not measured yet  */
	uint32_t ticks;       /* clock ticks received                         */
	uint32_t starts, continues, stops;   /* transport messages acted on   */
	/* Diagnostics (M5b): the clock's tick spacing over the last 5 s, from the USB
	 * timestamps, in us; transport bytes as received; fresh starts of the line. */
	uint32_t iv_n, iv_min_us, iv_avg_us, iv_max_us;
	uint32_t rx_start, rx_cont, rx_stop, line_resets;
	uint32_t mmc_play, mmc_stop;  /* MMC Play / Stop acted on (M5b round 2)    */
};
/* USB packets neither validator took (not channel voice, not clock / transport), and the
 * last of them -- sp1_usbd.c. Diagnostics: what a host sends that Wakes ignores. */
void sp1_midi_usb_rejects(uint32_t *count, uint8_t last[4]);
/* Clock / transport bytes that came in CIN 0x5 packets rather than the spec's CIN 0xF, and
 * were taken anyway (sp1_usbd.c). */
uint32_t sp1_midi_usb_rt_cin5(void);
void sp1_midi_get_stats(struct sp1_midi_stats *out);

/* ---- audio thread ---- */
/* What MIDI asks of one Plaits block: the per-sample-rate things -- a strike, the gate,
 * and the pitch, which glides and bends and so must move every Plaits block (a glide stepped
 * at 200 Hz would be audible). */
struct sp1_midi_frame {
	bool  trig;           /* strike TRIG in this block                              */
	bool  owns_level;     /* LEVEL is connected by MIDI (a key, or its release tail) */
	float gate;           /* LEVEL from MIDI: the gate height while a key is down     */
	float note;           /* semitones to add to the pitch: note - 60, portamento, bend */
};
/* Start of an audio block of `blocks` Plaits blocks; `cycles` = the stamps' clock now
 * (SP1_MIDI_STAMP_HZ; 0 = untimed, which places every message at the start of the block).
 *
 * `engine_centre` = the playing engine's SP1_ENGINE_TABLE[].centre bits (polarity, above).
 * Fills `off` with this audio block's CC offsets for the Plaits parameters, in fader-travel
 * units, SMOOTHED ONCE PER AUDIO BLOCK (5 ms): constant across the block, so the synth applies
 * them once, not per Plaits block. Returns false when MIDI has nothing at all to do this
 * block -- nothing queued, no key held or sounding, every offset at zero -- and then `off` is
 * all zeros and sp1_midi_audio_block() / _lpg() need not be called: idle MIDI costs one call
 * per audio block. Returns true otherwise, and from then on every block costs the same. */
bool sp1_midi_audio_begin(uint32_t cycles, uint32_t blocks, uint8_t engine_centre,
			  float off[SP1_MIDI_AUDIO_DESTS]);
/* Plaits block `j` of this audio block (only when sp1_midi_audio_begin returned true). */
void sp1_midi_audio_block(uint32_t j, struct sp1_midi_frame *out);
/* After Plaits rendered that block: its low-pass gate. Ends MIDI's hold on LEVEL once the
 * gate has closed after the last key (or at once when the engine bypasses its LPG). */
void sp1_midi_audio_lpg(float gain, bool bypassed);

/* MIDI clock for this audio block (M5b, above), after sp1_midi_audio_begin and before
 * Marbles renders. Always valid, whatever sp1_midi_audio_begin returned. */
#define SP1_MIDI_TP_NONE     0
#define SP1_MIDI_TP_START    1   /* beat 1 is in this block: reset and run              */
#define SP1_MIDI_TP_CONTINUE 2   /* run, no reset                                       */
#define SP1_MIDI_TP_STOP     3   /* stop (Stop, Start arming, or the cable pulled)      */
struct sp1_midi_clock {
	bool         external;   /* Marbles' clock is MIDI's this block                     */
	uint8_t      transport;  /* SP1_MIDI_TP_*: the last one in this block                */
	const float *beats;      /* per Plaits block, the position in beats, 0 .. 48;
				  * beat 1 of a Start is 0 (valid while `external`)          */
};
void sp1_midi_audio_clock(struct sp1_midi_clock *out);

#else  /* !CONFIG_SP1_MIDI: no MIDI function on the device; the UI sees it as never used */

static inline void  sp1_midi_on_enter(void) { }
static inline void  sp1_midi_main_tick(uint32_t elapsed_ms) { (void)elapsed_ms; }
static inline float sp1_midi_offset(int dest) { (void)dest; return 0.0f; }
static inline bool  sp1_midi_active(void) { return false; }
static inline bool  sp1_midi_port_up(void) { return false; }
static inline bool  sp1_midi_drive(int dest, float *value)
{
	(void)dest;
	(void)value;
	return false;
}
static inline bool  sp1_midi_fader_held(int dest) { (void)dest; return false; }

#endif /* CONFIG_SP1_MIDI */

#ifdef __cplusplus
}
#endif

#endif /* SP1_MIDI_H */
