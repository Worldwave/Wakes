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
 *   sp1_midi_push(), sp1_midi_port()        USB thread (Zephyr's usbd thread, cooperative,
 *                                           ABOVE audio). Never block, never take a lock:
 *                                           a lock-free ring, drop-and-count when full.
 *   sp1_midi_audio_*()                      AUDIO THREAD ONLY (from sp1_synth_render). This
 *                                           is where every message is parsed and every
 *                                           piece of MIDI state lives, so nothing is shared
 *                                           mid-update.
 *   everything else                         main thread; reads single words the audio
 *                                           thread publishes.
 *
 * ---- timing ----
 * Each message is stamped with the cycle counter when USB hands it over. At the start of
 * each audio block the audio thread takes everything that arrived since the previous block
 * started and places each message at the matching one of that block's twenty Plaits
 * blocks. Latency is therefore CONSTANT -- one audio block plus the I2S queue -- and the
 * timing jitter is USB's own (~1 ms), not the 5 ms of the audio block.
 *
 * ---- how a CC reads: by the parameter's polarity (Adara's M5a test notes) ----
 * The rule Marbles' INTELLIGENT range uses (M4c), applied to CCs: a BIPOLAR parameter -- one
 * whose centre is its neutral point -- takes a CENTRED CC (64 = no change, 0 / 127 = half a
 * fader's travel down / up, i.e. from a centred fader exactly to either end); a UNIPOLAR one
 * takes a ONE-SIDED CC (0 = no change, 127 = a whole travel up, i.e. from a fader at 0 exactly
 * to the top). Either way, from the fader's neutral position every CC value does something. Which is which is tools/gen_midi.py's POLARITY table; for TIMBRE, MORPH and
 * HARMONICS it is the playing engine's detent bits (SP1_ENGINE_TABLE[].centre), exactly as
 * INTELLIGENT reads them. Both readings are kept, and the reader picks at the moment it
 * applies the offset, so an engine change re-reads the CC the right way at once.
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

#if defined(CONFIG_SP1_MIDI)

/* ---- USB thread ---- */
/* One channel message (status byte first; `len` 2 or 3) as it came off the wire, stamped
 * with the free-running cycle counter. Already validated (usb_rt_parse.c). */
void sp1_midi_push(const uint8_t msg[3], uint8_t len, uint32_t cycles);
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

struct sp1_midi_stats {
	uint32_t received;    /* messages queued by USB                       */
	uint32_t dropped;     /* ...and lost to a full queue                  */
	uint32_t notes;       /* note-ons on our channel                      */
	uint32_t ccs;         /* control changes on our channel               */
	uint32_t ignored;     /* other channels and unused message types      */
	uint8_t  held;        /* keys down now                                */
	uint8_t  bend_range;  /* semitones now (script, or the host's RPN 0)  */
};
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
/* Start of an audio block of `blocks` Plaits blocks; `cycles` = the cycle counter now
 * (0 on the host, which then places every message at the start of the block).
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

#else  /* !CONFIG_SP1_MIDI: no MIDI function on the device; the UI sees it as never used */

static inline void  sp1_midi_on_enter(void) { }
static inline void  sp1_midi_main_tick(uint32_t elapsed_ms) { (void)elapsed_ms; }
static inline float sp1_midi_offset(int dest) { (void)dest; return 0.0f; }
static inline bool  sp1_midi_active(void) { return false; }
static inline bool  sp1_midi_port_up(void) { return false; }

#endif /* CONFIG_SP1_MIDI */

#ifdef __cplusplus
}
#endif

#endif /* SP1_MIDI_H */
