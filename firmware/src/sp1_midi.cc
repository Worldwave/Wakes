// wakes-sp1 — MIDI in (M5a). See sp1_midi.h.
//
// The monophonic note handling is PORTED from Mutable Instruments Yarns (Emilie Gillet, MIT),
// eurorack commit 08460a69, configured as one part in the 1M layout:
//   - NoteOn / NoteOff below are the VOICE_ALLOCATION_MODE_MONO branches of
//     yarns/part.cc Part::InternalNoteOn / InternalNoteOff, with Yarns' legato modes and the
//     hold pedal (Part::ControlChange, kCCHoldPedal);
//   - Glide and Refresh are yarns/voice.cc Voice::NoteOn and Voice::Refresh: the portamento
//     (both of Yarns' shapes, and its table, in closed form) and the pitch bend.
// The note stack is stmlib's (third_party/eurorack/stmlib/algorithms/note_stack.h), as Yarns
// uses it. The CC pickup is Plaits' pot catch-up (plaits/pot_controller.h, CatchUp below).
// Attributed in NOTICE. Everything else -- the CC offsets, 14-bit pairs, RPN 0, the
// smoothing, the threading -- is ours.
//
// Deliberately includes NO Zephyr headers (sp1_midi.h).

// The generated tables (SP1_MIDI_KIND, SP1_MIDI_NAME, SP1_MIDI_CC_DEST) are compiled here
// and nowhere else; sp1_midi.h includes the same header for the enum, so ask first.
#define SP1_MIDI_GEN_TABLES
#include "sp1_midi.h"

#include <atomic>
#include <cmath>

// GCC 12's -Wstringop-overflow reports `sorted_ptr_[size_] = free_slot` in NoteStack::NoteOn
// as a write past the array. It is not: by that line a full stack has already dropped its
// oldest note, so size_ <= capacity - 1 against an array of capacity + 1. GCC cannot see
// that bound through a uint8_t. Silenced for the vendored header only, which stays
// byte-identical to upstream.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#include "stmlib/algorithms/note_stack.h"
#pragma GCC diagnostic pop

static_assert(SP1_MIDI_D_FREQUENCY == 0 && SP1_MIDI_D_LEVEL == 10 &&
              SP1_MIDI_AUDIO_DESTS == 11,
              "tools/gen_midi.py: the Plaits audio-thread destinations must come first, "
              "FREQUENCY .. LEVEL, in that order");

namespace {

// ---- facts ----------------------------------------------------------------------------
const float kRefreshHz = 4000.0f;            // one Plaits block: Yarns refreshes at 4 kHz too
const uint8_t kStackSize = 12;               // Yarns' mono_allocator_
const float kTailEnd = 1e-3f;                // LPG gain at which the release has ended (-60 dB)

// ---- the queue: USB thread -> audio thread ----------------------------------------------
// Single producer (the usbd thread), single consumer (the audio thread), one core. The
// producer outranks the consumer, so a push can land in the middle of a drain; that is
// safe because each side only ever writes its own index, and the slot is written before
// the head moves (compiler fence; a Cortex-M4 needs no CPU barrier on one core).
struct Event {
  uint8_t msg[3];
  uint8_t len;
  uint32_t cycles;
};
const uint32_t kQueue = 128;                 // 1 KB; a 64-byte USB packet is 16 events
Event queue[kQueue];
volatile uint32_t q_head;                    // producer
volatile uint32_t q_tail;                    // consumer
volatile uint32_t st_received, st_dropped;

volatile uint32_t port_up;                   // USB thread writes
volatile uint32_t port_downs;                // ...and counts every loss of the port
volatile uint32_t reset_req;                 // main thread: entry to ON

// ---- audio-thread state ------------------------------------------------------------------
stmlib::NoteStack<kStackSize> stack;
// Note-ons not yet matched by a note-off, per pitch. The stack holds a pitch ONCE, so without
// this a sequencer's overlapping notes of the same pitch -- the next step's ON before the last
// step's OFF, which is what lengthening notes past the step does -- lost the second note: its
// ON changed nothing, and the FIRST note's OFF then ended it (Adara, M5a, from the OP-XY).
uint8_t on_count[128];
bool sustained[128];                         // released while the pedal was down
bool sustain_down;
uint32_t port_downs_seen, reset_seen;

bool session;                                // a message on our channel since the port came up
bool gate;                                   // a key is down (after priority and pedal)
bool tail;                                   // released, LPG still closing
bool trig_pending;
uint8_t velocity;                            // latest note's, Yarns' mod_velocity_
uint8_t aftertouch;

// Yarns Voice: the glide, in semitones.
float note_source = 60.0f, note_target = 60.0f, note_now = 60.0f;
float porta_phase = 1.0f, porta_inc = 0.0f;
bool porta_expo;

// Pitch bend and RPN 0.
uint16_t bend = 8192;
uint8_t bend_semis = SP1_MIDI_BEND_RANGE;
uint8_t bend_cents;
float bend_scale = SP1_MIDI_BEND_RANGE / 8192.0f;   // semitones per bend step, kept current
uint8_t rpn_msb = 127, rpn_lsb = 127;        // 127/127 = RPN null
bool nrpn_selected;

// CC values, per destination: the coarse (MSB) and fine (LSB) halves.
uint8_t cc_msb[SP1_MIDI_DESTS];
uint8_t cc_lsb[SP1_MIDI_DESTS];
bool cc_has_lsb[SP1_MIDI_DESTS];
bool cc_present[SP1_MIDI_DESTS];             // a CC has arrived since the last neutral

// Published to the main thread (single words).
// Offsets in fader-travel units, BOTH ways a CC can read (sp1_midi.h, "polarity"): the
// consumer picks by the parameter's polarity at the moment it applies it, because for TIMBRE,
// MORPH and HARMONICS that depends on the engine playing.
volatile float target_bi[SP1_MIDI_DESTS];    // centred: 64 = 0, 0 = -1, 127 = +1
volatile float target_uni[SP1_MIDI_DESTS];   // one-sided: 0 = 0, 127 = +1
// ...and, for pickup shared / takeover, where the CC IS: 0 = the bottom, 1 = the top.
volatile float pub_pos[SP1_MIDI_DESTS];
volatile uint8_t pub_has[SP1_MIDI_DESTS];    // pub_pos is valid (written after it)
volatile uint8_t engine_centre_now;          // last engine centre bits the audio thread saw
volatile uint32_t pub_active;
volatile uint32_t st_notes, st_ccs, st_ignored;
volatile uint8_t pub_held, pub_bend_range;

// Smoothed in the audio thread, ONCE PER AUDIO BLOCK (Plaits) ...
float smooth[SP1_MIDI_AUDIO_DESTS];
float smooth_coef = 1.0f;
uint32_t smooth_coef_blocks;                 // the block length smooth_coef was made for
bool settled = true;                         // every smoothed offset is exactly zero

// Yarns' lut_env_expo (yarns/resources/lookup_tables.py): 1 - exp(-4x) at 257 points,
// the last repeated, normalised to its largest value. Built once in the audio thread.
float env_expo[257];
// ... and in the control loop (Marbles).
float main_smooth[SP1_MIDI_DESTS];

// ---- pickup shared / takeover: the CC as a second hand on the fader's value (main thread) ----
// cc_pos is the CC's position smoothed in the control loop (stepped kinds not smoothed), and
// each destination keeps its own catch-up state against the value the UI hands
// sp1_midi_drive(), exactly as each fader keeps one against its stored value.
float cc_pos[SP1_MIDI_DESTS];
bool cc_on[SP1_MIDI_DESTS];                  // cc_pos is valid: the CC has arrived
struct Drive {
  bool have;                                 // prev is valid
  bool catching;                             // the value and the CC do not match yet
  float prev;                                // the CC's position at its last counted movement
};
Drive drive[SP1_MIDI_DESTS];

// ---- MODEL: at most one change every 50 ms (Adara, M5a test round 1) ----
// Every engine change costs one over-budget audio block -- the engine's own initialisation,
// the same as a T2/T3 press -- and a swept MODEL CC changed engine many times a second (the
// round-1 log: up to 16 overruns per 5 s, a 160 % block). So the MODEL offset the UI sees moves
// at most every kModelStepMs: a sweep still lands on the right engine, with a twentieth of a
// second between engines at most.
const uint32_t kModelStepMs = 50;
float model_held;                            // the MODEL offset the UI sees
uint32_t model_since_ms = kModelStepMs;      // since model_held last moved

// ---- MIDI clock -> Marbles (M5b; sp1_midi.h) ----
const uint32_t kPpqn = 24;                   // MIDI clock: ticks per beat
const uint32_t kClockWrap = kPpqn * 48;      // 48 beats: a whole period of every ratio
                                             // Marbles can ask for (q in 1,2,3,4,8,12,16)
const uint32_t kClockAvg = kPpqn;            // tempo averaged over one beat of ticks
const uint32_t kClockLost = 8000;            // 2 s without a tick: re-measure the tempo
const uint32_t kMaxBlocks = 64;              // Plaits blocks per audio block, at most
Event rt_ev[kQueue];                         // this block's clock / transport messages
uint8_t rt_at[kQueue];
uint32_t rt_n;
bool clk_ext;                                // Marbles' clock is MIDI's (C5)
bool clk_armed;                              // Start: the next tick is beat 1
bool clk_had_tick;                           // clk_since measures from a real tick
uint32_t clk_ticks;                          // since beat 1, mod kClockWrap
float clk_frac;                              // between ticks, 0 .. 1
float clk_inc;                               // per Plaits block, in ticks (0 = not known)
uint16_t clk_iv[kClockAvg];                  // the last tick intervals, in Plaits blocks
uint32_t clk_iv_sum, clk_iv_n, clk_iv_i;
uint32_t clk_since;                          // Plaits blocks since the last tick
uint8_t clk_op;                              // this block's transport (SP1_MIDI_TP_*)
float clk_beats[kMaxBlocks];
volatile uint32_t pub_clk_ext, pub_bpm10, st_ticks, st_starts, st_conts, st_stops;

// This audio block's events, each placed at a Plaits block.
Event block_ev[kQueue];
uint8_t block_at[kQueue];
uint32_t block_n, block_next;
uint32_t prev_begin;
bool prev_begin_valid;

inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- the pickup setting (config/midi.ini `pickup`, Adara, M5a round 4) ----
// sum: a CC is an OFFSET on its fader. shared / takeover: a CC is a POSITION on the fader's
// own value, which the UI moves through sp1_midi_drive() -- so there it is no offset at all.
// MODEL has no fader and is an offset in every setting.
constexpr bool CcIsOffset(int d) {
  return SP1_MIDI_PICKUP == SP1_MIDI_PICKUP_SUM || d == SP1_MIDI_D_MODEL;
}

// ---- Plaits' catch-up: plaits/pot_controller.h, POT_STATE_CATCHING_UP (Emilie Gillet, MIT) ----
// The one the faders already use (sp1_plaits_ui.c), here for the CCs. A control that does not
// match its value moves the value THE SAME WAY from where it is, skewed so the two meet at the
// end the hand is heading for; once they meet, the control just follows. One step: the value
// `s`, the control's reading at its last counted movement `prev`, and its reading `now`, on a
// range lo..hi. A value outside that range (an offset held from the other reading, below)
// widens it, so the two still meet at the far end.
const float kCatchMove = 0.005f;             // pot_controller.h: movement that counts
const float kCatchMatch = 0.005f;            // pot_controller.h: close enough to follow
float CatchUp(float s, float prev, float now, float lo, float hi) {
  lo = fminf(fminf(lo, s), fminf(prev, now));
  hi = fmaxf(fmaxf(hi, s), fmaxf(prev, now));
  const float w = hi - lo;
  if (w <= 0.0f) {
    return now;
  }
  const float inv = 1.0f / w;
  const float sn = (s - lo) * inv;
  const float pn = (prev - lo) * inv;
  const float dn = (now - prev) * inv;
  const float skew = Clamp(dn > 0.0f ? (1.001f - sn) / (1.001f - pn)
                                     : (0.001f + sn) / (0.001f + pn), 0.1f, 10.0f);
  return lo + w * Clamp(sn + skew * dn, 0.0f, 1.0f);
}

// ---- Yarns' portamento, voice.cc Voice::NoteOn, in closed form ----------------------------
// yarns/resources/lookup_tables.py builds lut_portamento_increments as 128 values of an
// increment whose time is (tmin^g + (tmax^g - tmin^g) * i/127)^(1/g)... with g = 1/4,
// tmin = 3 / 4000 s and tmax = 6 s: i.e. the glide TIME for table index i is
//   t(i) = (tmin^0.25 + (tmax^0.25 - tmin^0.25) * i / 127)^4.
// Yarns indexes it with (setting << 1) for both shapes.
float PortamentoTime(int index) {
  const float a = powf(3.0f / 4000.0f, 0.25f);
  const float b = powf(6.0f, 0.25f);
  const float r = a + (b - a) * static_cast<float>(index) / 127.0f;
  return r * r * r * r;
}

// Voice::NoteOn: glide from wherever the pitch is now to `note`.
void Glide(uint8_t note, uint8_t vel, int portamento, bool trigger) {
  note_source = note_now;
  note_target = static_cast<float>(note);
  if (!portamento) {
    note_source = note_target;
  }
  porta_phase = 0.0f;
  if (portamento <= 50) {
    // Constant TIME, exponential shape (lut_env_expo).
    porta_inc = 1.0f / (PortamentoTime(portamento << 1) * kRefreshHz);
    porta_expo = true;
  } else {
    // Constant RATE, linear: Yarns scales the increment by an octave over the interval,
    // 1536 / (delta + 1) with delta in 1/128 semitone.
    const float base = 1.0f / (PortamentoTime((portamento - 51) << 1) * kRefreshHz);
    porta_inc = base * 12.0f / (fabsf(note_target - note_source) + 1.0f / 128.0f);
    porta_expo = false;
  }
  if (!portamento) {
    porta_phase = 1.0f;
    porta_inc = 0.0f;
  }
  velocity = vel;
  gate = true;
  tail = false;
  if (trigger) {
    trig_pending = true;
  }
}

// Voice::Refresh: advance the glide one 4 kHz step.
void Refresh() {
  if (porta_inc > 0.0f) {
    porta_phase += porta_inc;
    if (porta_phase >= 1.0f) {
      porta_phase = 1.0f;
      porta_inc = 0.0f;
      note_source = note_target;
    }
  }
  // Yarns reads lut_env_expo with Interpolate824 -- a table, not exp(). Same cost on every
  // block, gliding or not (Adara's rule), and about a seventh of what expf() cost here.
  float k = porta_phase;
  if (porta_expo) {
    const float x = porta_phase * 256.0f;
    int i = static_cast<int>(x);
    if (i > 255) {
      i = 255;
    }
    k = env_expo[i] + (env_expo[i + 1] - env_expo[i]) * (x - static_cast<float>(i));
  }
  note_now = note_source + (note_target - note_source) * k;
}

const stmlib::NoteStackFlags kPriority =
    static_cast<stmlib::NoteStackFlags>(SP1_MIDI_PRIORITY);

uint8_t Top() {
  return stack.note_by_priority(kPriority).note;
}

// ---- Yarns part.cc, the 1M (mono) branches ------------------------------------------------
void NoteOn(uint8_t note, uint8_t vel) {
  // Playing: held by a note-on, or by the pedal after its note-off.
  const bool again = (on_count[note] > 0u || sustained[note]) && stack.size() > 0u &&
                     Top() == note;
  sustained[note] = false;
  if (on_count[note] < 255u) {
    ++on_count[note];
  }
  if (again) {
    // The pitch that is playing, played again before its note-off (ours, not Yarns'):
    // legato off strikes it again, as any new note; legato on / auto ties it -- an overlap of
    // the same pitch is one long note. Either way the note-off of the first one will not end
    // it (on_count).
    stack.NoteOn(note, vel);
    if (SP1_MIDI_LEGATO == 0) {
      Glide(note, vel, SP1_MIDI_PORTAMENTO, true);
    }
    return;
  }
  const uint8_t before = Top();
  stack.NoteOn(note, vel);
  const stmlib::NoteEntry& after = stack.note_by_priority(kPriority);
  if (before != after.note) {
    const bool legato = stack.size() > 1;
    Glide(after.note, after.velocity,
          (SP1_MIDI_LEGATO == 1) && !legato ? 0 : SP1_MIDI_PORTAMENTO,
          (SP1_MIDI_LEGATO == 0) || !legato);
  }
}

void InternalNoteOff(uint8_t note) {
  const uint8_t before = Top();
  stack.NoteOff(note);
  if (stack.size() == 0) {
    // No key is pressed: close the gate. LEVEL then closes through the LPG's own tail.
    if (gate) {
      gate = false;
      tail = true;
    }
  } else {
    const stmlib::NoteEntry& after = stack.note_by_priority(kPriority);
    if (before != after.note) {
      // Back to a key that is still held: slide there, and in legato off strike again.
      Glide(after.note, after.velocity, SP1_MIDI_PORTAMENTO, SP1_MIDI_LEGATO == 0);
    }
  }
}

void NoteOff(uint8_t note) {
  // Only the LAST outstanding note-on of a pitch releases it. A note-off with none
  // outstanding is a stray (the stack would not hold it either).
  if (on_count[note] == 0u) {
    return;
  }
  if (--on_count[note] > 0u) {
    return;
  }
  if (sustain_down) {
    // Yarns: flagged, and removed once the pedal is released.
    for (uint8_t i = 1; i <= stack.max_size(); ++i) {
      if (stack.note(i).note == note) {
        sustained[note] = true;
        return;
      }
    }
    return;
  }
  InternalNoteOff(note);
}

void Sustain(bool down) {
  if (sustain_down && !down) {
    sustain_down = false;
    for (int n = 0; n < 128; ++n) {
      if (sustained[n]) {
        sustained[n] = false;
        InternalNoteOff(static_cast<uint8_t>(n));
      }
    }
  }
  sustain_down = down;
}

void AllNotesOff() {
  stack.Clear();
  for (int n = 0; n < 128; ++n) {
    sustained[n] = false;
    on_count[n] = 0u;
  }
  if (gate) {
    gate = false;
    tail = true;
  }
}

// ---- CC offsets ---------------------------------------------------------------------------
// A CC is a second hand on its fader, read the way Marbles' INTELLIGENT range reads its
// destination (M4c; Adara's M5a test notes): a BIPOLAR parameter takes a CENTRED CC and a
// UNIPOLAR one a ONE-SIDED CC, each scaled so that FROM THE FADER'S NEUTRAL POSITION the CC's
// whole range spans the parameter's whole range -- every CC value does something:
//   one-sided: neutral is the bottom; 0 = no offset, 127 = a whole travel up.
//   centred:   neutral is the middle, half a travel from either end; 64 = no offset,
//              0 = HALF a travel down, 127 = half a travel up. (Round 1 used a whole
//              travel, so from a centred fader CC 32 already reached the bottom and 96 the
//              top, and a quarter of the CC range at each end did nothing -- Adara, round 2.)
//   centred, 7-bit: 64 -> 0, 0 -> -0.5, 127 -> +0.5 (63 steps up, 64 down);
//            14-bit (CC 0-31 + fine half on N+32): 8192 -> 0, 0 -> -0.5, 16383 -> +0.5.
//   one-sided, 7-bit: v / 127; 14-bit: v / 16383.
// A CC that has not arrived since the last neutral is no offset either way.
const float kCentredSpan = 0.5f;             // fader travel from the middle to either end
float CcOffset(int d, bool centred) {
  if (!cc_present[d]) {
    return 0.0f;
  }
  if (!cc_has_lsb[d]) {
    if (!centred) {
      return static_cast<float>(cc_msb[d]) / 127.0f;
    }
    const int v = cc_msb[d] - 64;
    return kCentredSpan *
           (v >= 0 ? static_cast<float>(v) / 63.0f : static_cast<float>(v) / 64.0f);
  }
  const int raw = (cc_msb[d] << 7) | cc_lsb[d];
  if (!centred) {
    return static_cast<float>(raw) / 16383.0f;
  }
  const int v = raw - 8192;
  return kCentredSpan *
         (v >= 0 ? static_cast<float>(v) / 8191.0f : static_cast<float>(v) / 8192.0f);
}

void Publish(int d) {
  // Bound sources (the MIDI script's [bind]) push the parameter up from where it is, by
  // their depth, whichever way the CC reads. Aimed at LEVEL they shape the gate height
  // instead (GateHeight()).
  float bound = 0.0f;
  if (SP1_MIDI_VELOCITY_DEST == d && d != SP1_MIDI_D_LEVEL) {
    bound += SP1_MIDI_VELOCITY_DEPTH * static_cast<float>(velocity) / 127.0f;
  }
  if (SP1_MIDI_AFTERTOUCH_DEST == d && d != SP1_MIDI_D_LEVEL) {
    bound += SP1_MIDI_AFTERTOUCH_DEPTH * static_cast<float>(aftertouch) / 127.0f;
  }
  const bool offset = CcIsOffset(d);
  target_bi[d] = Clamp((offset ? CcOffset(d, true) : 0.0f) + bound, -1.0f, 1.0f);
  target_uni[d] = Clamp((offset ? CcOffset(d, false) : 0.0f) + bound, -1.0f, 1.0f);
  // The position is the one-sided reading: 0 = the bottom, 127 / 16383 = the top.
  if (cc_present[d]) {
    pub_pos[d] = CcOffset(d, false);
    std::atomic_signal_fence(std::memory_order_seq_cst);
    pub_has[d] = 1u;
  } else {
    pub_has[d] = 0u;
  }
}

// Is destination `d` bipolar right now? Fixed for most; for TIMBRE / MORPH / HARMONICS it is
// the playing engine's detent bit -- the same test INTELLIGENT makes (sp1_marbles.cc).
inline bool Bipolar(int d, uint8_t centre) {
  const uint8_t p = SP1_MIDI_POLARITY[d];
  return p == 1u || ((p & 0x10u) != 0u && (centre & (p & 0x0Fu)) != 0u);
}

inline float Target(int d, uint8_t centre) {
  return Bipolar(d, centre) ? target_bi[d] : target_uni[d];
}

// ---- pickup sum: when an engine change re-reads a CC (Adara, M5a rounds 3 and 4) ----
// TIMBRE, MORPH and HARMONICS read their CC centred or one-sided depending on the engine, so an
// engine change can give the SAME host knob position a different meaning -- a jump. Instead, on
// that flip the offset stays where it was and the CC CATCHES UP with it, Plaits' way
// (CatchUp): moving the host knob moves the offset the same way from where it is, until the two
// meet. Round 3 HELD the offset until the knob crossed it, and the knob felt dead for most of
// its travel (Adara: "too unresponsive"); now every movement does something at once.
//   - Only a CC that has arrived can catch; a reset or a disconnect clears every catch.
//   - The other destinations' readings never flip, so they never catch.
//   - Only in sum: in shared / takeover the CC is a position, which reads the same on every
//     engine, so there is nothing to flip.
bool pk_known[SP1_MIDI_AUDIO_DESTS];         // pk_bi is valid
bool pk_bi[SP1_MIDI_AUDIO_DESTS];            // the reading at the last block
volatile bool pk_catch[SP1_MIDI_AUDIO_DESTS];
volatile float pk_val[SP1_MIDI_AUDIO_DESTS]; // the offset while it catches up
float pk_prev[SP1_MIDI_AUDIO_DESTS];         // the reading at the last counted movement

// AUDIO THREAD, once per audio block: destination d's offset with pickup applied.
float PickupTarget(int d, uint8_t centre) {
  const float r = Target(d, centre);
  if ((SP1_MIDI_POLARITY[d] & 0x10u) == 0u) {
    return r;
  }
  const bool bi = Bipolar(d, centre);
  if (!pk_known[d]) {
    pk_known[d] = true;
    pk_bi[d] = bi;
  } else if (bi != pk_bi[d]) {
    // What the knob means RIGHT NOW under the reading being left -- not the offset applied
    // at the last block, which lags a CC that arrived during it.
    const float was = pk_bi[d] ? target_bi[d] : target_uni[d];
    pk_bi[d] = bi;
    if (pk_catch[d]) {
      pk_prev[d] = r;                          // keep the value; measure from the new reading
    } else if (cc_present[d] && fabsf(r - was) >= kCatchMatch) {
      pk_val[d] = was;
      pk_prev[d] = r;
      pk_catch[d] = true;
    }
  }
  if (pk_catch[d]) {
    if (!cc_present[d]) {
      pk_catch[d] = false;
    } else if (fabsf(r - pk_prev[d]) > kCatchMove) {
      const float v = CatchUp(pk_val[d], pk_prev[d], r, bi ? -kCentredSpan : 0.0f,
                              bi ? kCentredSpan : 1.0f);
      pk_val[d] = v;
      pk_prev[d] = r;
      if (fabsf(v - r) < kCatchMatch) {
        pk_catch[d] = false;
      }
    }
  }
  return pk_catch[d] ? pk_val[d] : r;
}

void PublishAll() {
  for (int d = 0; d < SP1_MIDI_DESTS; ++d) {
    Publish(d);
  }
}

void ResetControllers() {
  for (int d = 0; d < SP1_MIDI_DESTS; ++d) {
    cc_msb[d] = 64;
    cc_lsb[d] = 0;
    cc_has_lsb[d] = false;
    cc_present[d] = false;
  }
  for (int d = 0; d < SP1_MIDI_AUDIO_DESTS; ++d) {
    pk_catch[d] = false;                       // nothing left to catch up with
  }
  aftertouch = 0;
  bend = 8192;
  rpn_msb = rpn_lsb = 127;
  nrpn_selected = false;
  Sustain(false);
  PublishAll();
}

// The gate's height. 1 unless velocity or aftertouch is bound to LEVEL.
float GateHeight() {
  float h = 1.0f;
  if (SP1_MIDI_VELOCITY_DEST == SP1_MIDI_D_LEVEL) {
    h *= 1.0f + SP1_MIDI_VELOCITY_DEPTH * (static_cast<float>(velocity) / 127.0f - 1.0f);
  }
  if (SP1_MIDI_AFTERTOUCH_DEST == SP1_MIDI_D_LEVEL) {
    h *= 1.0f + SP1_MIDI_AFTERTOUCH_DEPTH * (static_cast<float>(aftertouch) / 127.0f - 1.0f);
  }
  return Clamp(h, 0.0f, 1.0f);
}

// The bend range as semitones per bend step, recomputed only when it changes (no divide
// on the per-block path).
void UpdateBendScale() {
  bend_scale = (static_cast<float>(bend_semis) + static_cast<float>(bend_cents) * 0.01f) *
               (1.0f / 8192.0f);
}

bool RpnBendSelected() {
  return !nrpn_selected && rpn_msb == 0 && rpn_lsb == 0;
}

void ControlChange(uint8_t cc, uint8_t v) {
  if (SP1_MIDI_SUSTAIN_CC >= 0 && cc == SP1_MIDI_SUSTAIN_CC) {
    Sustain(v >= 64);
    return;
  }
  switch (cc) {
    // ---- RPN 0, pitch bend sensitivity (Adara, round 3: "make RPN work") ----
    case 101: rpn_msb = v; nrpn_selected = false; return;
    case 100: rpn_lsb = v; nrpn_selected = false; return;
    case 99:
    case 98: nrpn_selected = true; return;   // NRPN data is for other gear
    case 6:
      if (RpnBendSelected()) {
        bend_semis = v > 24 ? 24 : v;
        bend_cents = 0;
      }
      return;
    case 38:
      if (RpnBendSelected()) {
        bend_cents = v > 99 ? 99 : v;
      }
      return;
    case 96:
      if (RpnBendSelected() && bend_semis < 24) {
        ++bend_semis;
      }
      return;
    case 97:
      if (RpnBendSelected() && bend_semis > 0) {
        --bend_semis;
      }
      return;
    // ---- channel mode messages ----
    case 120:                                  // all sound off
    case 123:                                  // all notes off
    case 124: case 125: case 126: case 127:    // omni / mono / poly: notes off too
      AllNotesOff();
      return;
    case 121:                                  // reset all controllers (RP-015)
      ResetControllers();
      return;
    default:
      break;
  }
  int d;
  if (cc >= 32 && cc < 64) {
    d = SP1_MIDI_CC_DEST[cc - 32];             // the fine half of a 14-bit pair
    if (d < 0) {
      ++st_ignored;
      return;
    }
    cc_lsb[d] = v;
    cc_has_lsb[d] = true;
    cc_present[d] = true;
  } else {
    d = SP1_MIDI_CC_DEST[cc];
    if (d < 0) {
      ++st_ignored;
      return;
    }
    // A new coarse half resets the fine half (MIDI 1.0), so a host that only sends the
    // coarse half reads as 7-bit.
    cc_msb[d] = v;
    cc_lsb[d] = 0;
    cc_has_lsb[d] = false;
    cc_present[d] = true;
  }
  st_ccs = st_ccs + 1u;
  Publish(d);
}

// noinline: this runs once per MESSAGE. Inlined into sp1_midi_audio_block, its register
// pressure spilled ~30 instructions onto the path that runs every Plaits block.
__attribute__((noinline)) void Process(const Event& e) {
  const uint8_t status = e.msg[0];
  const uint8_t type = status & 0xF0u;
  if (SP1_MIDI_CHANNEL != SP1_MIDI_OMNI && (status & 0x0Fu) != SP1_MIDI_CHANNEL) {
    st_ignored = st_ignored + 1u;
    return;
  }
  const uint8_t d1 = e.msg[1] & 0x7Fu;
  const uint8_t d2 = e.len > 2 ? (e.msg[2] & 0x7Fu) : 0u;
  switch (type) {
    case 0x90:
      if (d2 > 0) {
        session = true;
        st_notes = st_notes + 1u;
        NoteOn(d1, d2);
        if (SP1_MIDI_VELOCITY_DEST >= 0) {
          Publish(SP1_MIDI_VELOCITY_DEST);
        }
        break;
      }
      // fall through: a note-on at velocity 0 is a note-off
    case 0x80:
      session = true;
      NoteOff(d1);
      break;
    case 0xB0:
      session = true;
      ControlChange(d1, d2);
      break;
    case 0xE0:
      session = true;
      bend = static_cast<uint16_t>((d2 << 7) | d1);
      break;
    case 0xD0:
      session = true;
      aftertouch = d1;
      if (SP1_MIDI_AFTERTOUCH_DEST >= 0) {
        Publish(SP1_MIDI_AFTERTOUCH_DEST);
      }
      break;
    default:
      st_ignored = st_ignored + 1u;            // poly pressure, program change
      break;
  }
  // Published as soon as a message changes them (a cost per message, not per block); the
  // end of a release is published by sp1_midi_audio_begin.
  pub_active = (session || gate || tail) ? 1u : 0u;
  pub_held = stack.size();
  pub_bend_range = bend_semis;
  UpdateBendScale();
}

// ---- MIDI clock (M5b) ---------------------------------------------------------------------
void ClockForgetTempo() {
  clk_iv_sum = clk_iv_n = clk_iv_i = 0;
}

// One tick, at the Plaits block being built.
void ClockTick() {
  st_ticks = st_ticks + 1u;
  clk_ext = true;                              // the first tick makes the clock external
  if (clk_armed) {
    // Start was received: this tick is beat 1.
    clk_armed = false;
    clk_ticks = 0;
    clk_op = SP1_MIDI_TP_START;
    st_starts = st_starts + 1u;
  } else {
    clk_ticks = (clk_ticks + 1u) % kClockWrap;
  }
  clk_frac = 0.0f;
  // The interval since the last tick, into the tempo. One that is much longer than the
  // tempo so far is a pause (the host stopped its clock), not a new tempo: start measuring
  // afresh rather than average it in.
  if (clk_had_tick) {
    const uint32_t iv = clk_since;
    const bool pause = iv >= kClockLost ||
                       (clk_iv_n >= 4u && iv * clk_iv_n > 2u * clk_iv_sum);
    if (pause) {
      ClockForgetTempo();
    } else if (iv > 0u) {
      if (clk_iv_n == kClockAvg) {
        clk_iv_sum -= clk_iv[clk_iv_i];
      } else {
        ++clk_iv_n;
      }
      clk_iv[clk_iv_i] = static_cast<uint16_t>(iv);
      clk_iv_sum += iv;
      clk_iv_i = (clk_iv_i + 1u) % kClockAvg;
      // Ticks per Plaits block. One divide per tick, not per block.
      clk_inc = static_cast<float>(clk_iv_n) / static_cast<float>(clk_iv_sum);
      pub_bpm10 = (100000u * clk_iv_n + clk_iv_sum / 2u) / clk_iv_sum;   // 10000 / avg
    }
  }
  clk_had_tick = true;
  clk_since = 0;
}

void ClockRealTime(uint8_t b) {
  if (!SP1_MIDI_CLOCK) {
    return;
  }
  switch (b) {
    case 0xF8:
      ClockTick();
      break;
    case 0xFA:                                 // Start: wait for beat 1, from the top
      clk_ext = true;
      clk_armed = true;
      clk_ticks = 0;
      clk_frac = 0.0f;
      clk_op = SP1_MIDI_TP_STOP;
      break;
    case 0xFB:                                 // Continue: from where it is
      clk_ext = true;
      clk_armed = false;
      clk_op = SP1_MIDI_TP_CONTINUE;
      st_conts = st_conts + 1u;
      break;
    case 0xFC:
      clk_armed = false;
      clk_op = SP1_MIDI_TP_STOP;
      st_stops = st_stops + 1u;
      break;
    default:
      break;
  }
}

// AUDIO THREAD, once per audio block: the clock messages at their Plaits blocks, and the
// position in beats at each. Every block costs the same, ticking or not (Adara's rule).
void ClockBlock(uint32_t blocks) {
  if (blocks > kMaxBlocks) {
    blocks = kMaxBlocks;
  }
  uint32_t r = 0;
  for (uint32_t j = 0; j < blocks; ++j) {
    while (r < rt_n && rt_at[r] <= j) {
      ClockRealTime(rt_ev[r++].msg[0]);
    }
    // Between ticks, at the tempo -- but never past the next tick: a late tick holds the
    // position, a stopped clock stops it. Armed for beat 1, it holds at 0.
    if (!clk_armed) {
      clk_frac += clk_inc;
      if (clk_frac > 1.0f) {
        clk_frac = 1.0f;
      }
    }
    if (clk_since < 0xFFFFu) {
      ++clk_since;
    }
    clk_beats[j] = (static_cast<float>(clk_ticks) + clk_frac) * (1.0f / kPpqn);
  }
  pub_clk_ext = clk_ext ? 1u : 0u;
}

// Back to neutral: the port went away, or ON was entered. Keys are released (LEVEL closes
// through its tail), every controller returns to centre (offsets glide back), the bend range
// returns to the script's. The pitch stays where it is until the tail has ended, so a
// release does not jump; then it, too, returns.
void Neutral() {
  // The clock: a Marbles that MIDI was clocking stops, and has its own clock again (C5).
  if (clk_ext) {
    clk_op = SP1_MIDI_TP_STOP;
  }
  clk_ext = clk_armed = clk_had_tick = false;
  clk_ticks = 0;
  clk_frac = 0.0f;
  clk_inc = 0.0f;
  ClockForgetTempo();
  pub_bpm10 = 0;
  session = false;
  AllNotesOff();
  ResetControllers();
  velocity = 0;
  PublishAll();
  bend_semis = SP1_MIDI_BEND_RANGE;
  bend_cents = 0;
  UpdateBendScale();
  trig_pending = false;
}

}  // namespace

// ==== USB thread ==========================================================================
extern "C" void sp1_midi_push(const uint8_t msg[3], uint8_t len, uint32_t cycles) {
  const uint32_t h = q_head;
  if (h - q_tail >= kQueue) {
    st_dropped = st_dropped + 1u;
    return;
  }
  Event& e = queue[h % kQueue];
  e.msg[0] = msg[0];
  e.msg[1] = len > 1 ? msg[1] : 0u;
  e.msg[2] = len > 2 ? msg[2] : 0u;
  e.len = len;
  e.cycles = cycles;
  std::atomic_signal_fence(std::memory_order_seq_cst);
  q_head = h + 1u;
  st_received = st_received + 1u;
}

extern "C" void sp1_midi_port(bool up) {
  if (!up && port_up) {
    port_downs = port_downs + 1u;
  }
  port_up = up ? 1u : 0u;
}

// ==== main thread =========================================================================
extern "C" void sp1_midi_on_enter(void) {
  reset_req = reset_req + 1u;
}

extern "C" void sp1_midi_main_tick(uint32_t elapsed_ms) {
  if (model_since_ms < kModelStepMs) {
    model_since_ms += elapsed_ms;
  }
  const float mt = Target(SP1_MIDI_D_MODEL, engine_centre_now);
  if (mt != model_held && model_since_ms >= kModelStepMs) {
    model_held = mt;
    model_since_ms = 0;
  }
  const float tau = static_cast<float>(SP1_MIDI_SMOOTH_MS);
  const float k = tau <= 0.0f ? 1.0f : 1.0f - expf(-static_cast<float>(elapsed_ms) / tau);
  for (int d = 0; d < SP1_MIDI_DESTS; ++d) {
    if (SP1_MIDI_KIND[d] == SP1_MIDI_K_MAIN) {
      const float t = Target(d, engine_centre_now);
      main_smooth[d] += (t - main_smooth[d]) * k;
      if (fabsf(main_smooth[d]) < 1e-6f && t == 0.0f) {
        main_smooth[d] = 0.0f;
      }
    }
  }
  if (SP1_MIDI_PICKUP != SP1_MIDI_PICKUP_SUM) {
    // The positions sp1_midi_drive() reads. A CC that has just arrived starts AT its
    // position: smoothing it up from 0 would be a movement the hand never made.
    for (int d = 0; d < SP1_MIDI_DESTS; ++d) {
      if (pub_has[d] == 0u) {
        cc_on[d] = false;
        continue;
      }
      std::atomic_signal_fence(std::memory_order_seq_cst);
      const float t = pub_pos[d];
      if (!cc_on[d] || SP1_MIDI_KIND[d] == SP1_MIDI_K_STEP) {
        cc_on[d] = true;
        cc_pos[d] = t;
        continue;
      }
      float v = cc_pos[d] + (t - cc_pos[d]) * k;
      if (fabsf(v - t) < 1e-6f) {
        v = t;
      }
      cc_pos[d] = v;
    }
  }
}

// ---- pickup shared / takeover: one step of the CC's catch-up on the value `*s` ----
// The fader's own pickup (sp1_plaits_ui.c), with the CC as the pot:
//   - The CC's first position after it arrives is only a reference: nothing moves until the
//     host knob does, so a host that sends its knobs' positions when it connects moves nothing.
//   - While the CC matches the value, it follows: the value goes where the CC goes.
//   - When something else moves the value -- the fader, a rip, a SHIFT reset -- the CC no
//     longer matches it and catches up again, Plaits' way (CatchUp).
// The UI tells its fader the value moved; the fader then catches up with it the same way.
extern "C" bool sp1_midi_drive(int d, float* s) {
  if (SP1_MIDI_PICKUP == SP1_MIDI_PICKUP_SUM || d < 0 || d >= SP1_MIDI_DESTS || CcIsOffset(d)) {
    return false;
  }
  Drive& k = drive[d];
  if (!cc_on[d]) {
    k.have = false;
    return false;
  }
  const float c = cc_pos[d];
  if (!k.have) {
    k.have = true;
    k.prev = c;
    k.catching = fabsf(*s - c) >= kCatchMatch;
    return false;
  }
  if (!k.catching && fabsf(*s - k.prev) >= kCatchMatch) {
    k.catching = true;                         // moved by something else since
  }
  if (!k.catching) {
    if (c == k.prev) {
      return false;
    }
    *s = c;
    k.prev = c;
    return true;
  }
  if (fabsf(c - k.prev) <= kCatchMove) {
    return false;                              // not a movement yet (it accumulates)
  }
  *s = CatchUp(*s, k.prev, c, 0.0f, 1.0f);
  k.prev = c;
  if (fabsf(*s - c) < kCatchMatch) {
    k.catching = false;
  }
  return true;
}

// pickup takeover: while the port is up, a fader whose parameter has a CC rests.
extern "C" bool sp1_midi_fader_held(int d) {
  return SP1_MIDI_PICKUP == SP1_MIDI_PICKUP_TAKEOVER && d >= 0 && d < SP1_MIDI_DESTS &&
         !CcIsOffset(d) && SP1_MIDI_DEST_CC[d] >= 0 && port_up != 0u;
}

extern "C" float sp1_midi_offset(int d) {
  if (d < 0 || d >= SP1_MIDI_DESTS) {
    return 0.0f;
  }
  if (d == SP1_MIDI_D_MODEL) {
    return model_held;
  }
  if (d < SP1_MIDI_AUDIO_DESTS && pk_catch[d]) {
    return pk_val[d];                          // while the CC catches up
  }
  return SP1_MIDI_KIND[d] == SP1_MIDI_K_MAIN ? main_smooth[d] : Target(d, engine_centre_now);
}

extern "C" bool sp1_midi_active(void) { return pub_active != 0u; }
extern "C" bool sp1_midi_port_up(void) { return port_up != 0u; }

extern "C" void sp1_midi_get_stats(struct sp1_midi_stats* out) {
  out->received = st_received;
  out->dropped = st_dropped;
  out->notes = st_notes;
  out->ccs = st_ccs;
  out->ignored = st_ignored;
  out->held = pub_held;
  out->bend_range = pub_bend_range;
  out->clock_ext = pub_clk_ext != 0u;
  out->bpm10 = static_cast<uint16_t>(pub_bpm10);
  out->ticks = st_ticks;
  out->starts = st_starts;
  out->continues = st_conts;
  out->stops = st_stops;
}

// ==== audio thread ========================================================================
extern "C" bool sp1_midi_audio_begin(uint32_t cycles, uint32_t blocks, uint8_t engine_centre,
                                     float off[SP1_MIDI_AUDIO_DESTS]) {
  engine_centre_now = engine_centre;
  static bool once;
  if (!once) {
    once = true;
    stack.Init();
    ResetControllers();
    float e[257];
    for (int i = 0; i < 257; ++i) {
      const float x = static_cast<float>(i < 256 ? i : 255) / 256.0f;
      e[i] = 1.0f - expf(-4.0f * x);
    }
    for (int i = 0; i < 257; ++i) {
      env_expo[i] = e[i] / e[255];
    }
  }
  clk_op = SP1_MIDI_TP_NONE;
  rt_n = 0;
  // Disconnected, or ON entered: drop whatever queued and go back to neutral.
  const uint32_t downs = port_downs;
  const uint32_t rr = reset_req;
  if (downs != port_downs_seen || rr != reset_seen) {
    port_downs_seen = downs;
    reset_seen = rr;
    q_tail = q_head;
    block_n = block_next = 0;                  // nothing deferred survives it either
    Neutral();
  }

  // Take this block's messages and place each at its Plaits block (sp1_midi.h, "timing").
  // Anything the last audio block deferred (the minimum gate, sp1_midi_audio_block) goes
  // first, at its first Plaits block.
  const uint32_t span = cycles - prev_begin;
  const bool timed = prev_begin_valid && cycles != 0u && span != 0u;
  uint32_t carried = 0;
  for (uint32_t i = block_next; i < block_n; ++i) {
    block_ev[carried] = block_ev[i];
    block_at[carried] = 0u;
    ++carried;
  }
  block_n = carried;
  block_next = 0;
  while (q_tail != q_head && block_n < kQueue) {
    const Event e = queue[q_tail % kQueue];
    std::atomic_signal_fence(std::memory_order_seq_cst);
    q_tail = q_tail + 1u;
    uint32_t at = 0;
    if (timed && blocks > 0u) {
      const int32_t after_prev = static_cast<int32_t>(e.cycles - prev_begin);
      const int32_t after_now = static_cast<int32_t>(e.cycles - cycles);
      if (after_now >= 0) {
        at = blocks - 1u;                       // stamped after we looked: last block
      } else if (after_prev > 0) {
        at = static_cast<uint32_t>(
            (static_cast<uint64_t>(after_prev) * blocks) / span);
        if (at >= blocks) {
          at = blocks - 1u;
        }
      }
    }
    if (e.len == 1u) {
      // Clock and transport (M5b): not channel messages, handled in ClockBlock below.
      rt_ev[rt_n] = e;
      rt_at[rt_n] = static_cast<uint8_t>(at);
      ++rt_n;
      continue;
    }
    block_ev[block_n] = e;
    block_at[block_n] = static_cast<uint8_t>(at);
    ++block_n;
  }
  prev_begin = cycles;
  prev_begin_valid = true;
  ClockBlock(blocks);

  // ---- the CC offsets: smoothed ONCE PER AUDIO BLOCK (5 ms) ----
  // A one-pole at 200 Hz with the script's time constant. That is a finer step than the
  // faders get (the control loop publishes every 8 ms), and Plaits interpolates each
  // parameter across its own 12-sample block on top. Per Plaits block it cost ~260
  // instructions x 20, for nothing audible. A message lands in the target at once and the
  // smoothing picks it up at the next audio block: up to 5 ms more, on CCs only.
  if (blocks != smooth_coef_blocks) {
    smooth_coef_blocks = blocks;
    const float tau = static_cast<float>(SP1_MIDI_SMOOTH_MS) * 1e-3f;
    smooth_coef = tau <= 0.0f
        ? 1.0f
        : 1.0f - expf(-static_cast<float>(blocks) / (tau * kRefreshHz));
  }
  bool zero = true;
  for (int d = 0; d < SP1_MIDI_AUDIO_DESTS; ++d) {
    const float t = PickupTarget(d, engine_centre);
    float v = smooth[d] + (t - smooth[d]) * smooth_coef;
    if (fabsf(v - t) < 1e-6f) {
      v = t;
    }
    smooth[d] = v;
    off[d] = v;
    zero = zero && v == 0.0f;
  }
  settled = zero;

  const bool active = session || gate || tail;
  if (!active && note_target != 60.0f) {
    // The release has ended after a disconnect: the pitch returns too, without a glide.
    note_source = note_target = note_now = 60.0f;
    porta_phase = 1.0f;
    porta_inc = 0.0f;
  }
  pub_active = active ? 1u : 0u;
  pub_held = stack.size();
  pub_bend_range = bend_semis;

  // Nothing queued, nothing held or sounding, nothing gliding home: the synth skips the
  // per-Plaits-block calls altogether (issue #21: MIDI costs nothing until it is used).
  return block_n > 0u || active || !settled;
}

extern "C" void sp1_midi_audio_block(uint32_t j, sp1_midi_frame* f) {
  // ---- the minimum gate: one Plaits block (0.25 ms) ----
  // A strike ends this block's messages; the rest wait for the next block. Otherwise a
  // note-off landing in the same block as its note-on -- a very short note, or one that
  // shares a USB packet (one timestamp) with its own note-off -- struck TRIG with LEVEL
  // already back at 0, and Plaits' gate never opened: a missed note (Adara, from the OP-XY).
  // One block at full LEVEL opens the gate most of the way (envelope.h, ProcessLP: 0.6 per
  // block). Deferred messages carry over into the next audio block if need be.
  while (block_next < block_n && block_at[block_next] <= j && !trig_pending) {
    Process(block_ev[block_next++]);
  }
  // Every MIDI block costs the same from here on, whether anything moves or not.
  Refresh();
  f->note = note_now - 60.0f + (static_cast<float>(bend) - 8192.0f) * bend_scale;
  f->trig = trig_pending;
  trig_pending = false;
  f->owns_level = gate || tail;
  f->gate = gate ? GateHeight() : 0.0f;
}

extern "C" void sp1_midi_audio_clock(sp1_midi_clock* out) {
  out->external = clk_ext;
  out->transport = clk_op;
  out->beats = clk_beats;
}

extern "C" void sp1_midi_audio_lpg(float gain, bool bypassed) {
  if (tail && !gate && (bypassed || gain < kTailEnd)) {
    tail = false;
  }
}
