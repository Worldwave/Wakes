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
// uses it. Attributed in NOTICE. Everything else -- the CC offsets, 14-bit pairs, RPN 0, the
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

// This audio block's events, each placed at a Plaits block.
Event block_ev[kQueue];
uint8_t block_at[kQueue];
uint32_t block_n, block_next;
uint32_t prev_begin;
bool prev_begin_valid;

inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

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
  sustained[note] = false;
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
  }
  if (gate) {
    gate = false;
    tail = true;
  }
}

// ---- CC offsets ---------------------------------------------------------------------------
// A CC is a second hand on its fader, read the way Marbles' INTELLIGENT range reads its
// destination (M4c; Adara's M5a test notes): a BIPOLAR parameter takes a CENTRED CC -- 64 =
// no offset, 0 = a whole travel down, 127 = a whole travel up -- and a UNIPOLAR one a
// ONE-SIDED CC -- 0 = no offset, 127 = a whole travel up -- so the whole CC range does
// something, and a host knob resting at 0 leaves the fader in charge.
//   centred, 7-bit: 64 -> 0, 0 -> -1, 127 -> +1 (63 steps up, 64 down);
//            14-bit (CC 0-31 + fine half on N+32): 8192 -> 0, 0 -> -1, 16383 -> +1.
//   one-sided, 7-bit: v / 127; 14-bit: v / 16383.
// A CC that has not arrived since the last neutral is no offset either way.
float CcOffset(int d, bool centred) {
  if (!cc_present[d]) {
    return 0.0f;
  }
  if (!cc_has_lsb[d]) {
    if (!centred) {
      return static_cast<float>(cc_msb[d]) / 127.0f;
    }
    const int v = cc_msb[d] - 64;
    return v >= 0 ? static_cast<float>(v) / 63.0f : static_cast<float>(v) / 64.0f;
  }
  const int raw = (cc_msb[d] << 7) | cc_lsb[d];
  if (!centred) {
    return static_cast<float>(raw) / 16383.0f;
  }
  const int v = raw - 8192;
  return v >= 0 ? static_cast<float>(v) / 8191.0f : static_cast<float>(v) / 8192.0f;
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
  target_bi[d] = Clamp(CcOffset(d, true) + bound, -1.0f, 1.0f);
  target_uni[d] = Clamp(CcOffset(d, false) + bound, -1.0f, 1.0f);
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

// Back to neutral: the port went away, or ON was entered. Keys are released (LEVEL closes
// through its tail), every controller returns to centre (offsets glide back), the bend range
// returns to the script's. The pitch stays where it is until the tail has ended, so a
// release does not jump; then it, too, returns.
void Neutral() {
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
}

extern "C" float sp1_midi_offset(int d) {
  if (d < 0 || d >= SP1_MIDI_DESTS) {
    return 0.0f;
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
  // Disconnected, or ON entered: drop whatever queued and go back to neutral.
  const uint32_t downs = port_downs;
  const uint32_t rr = reset_req;
  if (downs != port_downs_seen || rr != reset_seen) {
    port_downs_seen = downs;
    reset_seen = rr;
    q_tail = q_head;
    Neutral();
  }

  // Take this block's messages and place each at its Plaits block (sp1_midi.h, "timing").
  const uint32_t span = cycles - prev_begin;
  const bool timed = prev_begin_valid && cycles != 0u && span != 0u;
  block_n = 0;
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
    block_ev[block_n] = e;
    block_at[block_n] = static_cast<uint8_t>(at);
    ++block_n;
  }
  prev_begin = cycles;
  prev_begin_valid = true;

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
    const float t = Target(d, engine_centre);
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
  while (block_next < block_n && block_at[block_next] <= j) {
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

extern "C" void sp1_midi_audio_lpg(float gain, bool bypassed) {
  if (tail && !gate && (bypassed || gain < kTailEnd)) {
    tail = false;
  }
}
