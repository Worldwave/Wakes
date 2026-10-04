// wakes-sp1 — the synth voice. See sp1_synth.h.
//
// Glue around plaits::Voice (Mutable Instruments Plaits, Emilie Gillet, MIT),
// vendored in third_party/eurorack. Compiled with the same flags as Plaits itself
// (see firmware/CMakeLists.txt), because it instantiates the Voice.
//
// Deliberately includes NO Zephyr headers: the DSP layer stays framework-agnostic.

#include "sp1_synth.h"
#include "sp1_marbles.h"
#include "sp1_marbles_ui.h"
#if defined(CONFIG_SP1_MIDI)
#include "sp1_midi.h"
#endif

#include <atomic>
#include <cmath>
#include <new>

#include "stmlib/dsp/dsp.h"
#include "stmlib/utils/buffer_allocator.h"
#include "plaits/dsp/voice.h"

namespace {

// Same 16 KB arena Plaits' own firmware gives the voice (plaits.cc: shared_buffer).
char arena[16384];

// ⚠️ Constructed with placement new in sp1_synth_init(), NOT as a global object.
// Every engine is polymorphic, so construction is what sets their vtable pointers.
// A global would rely on the C++ static-initialisation pass having run before the
// first Render; if it ever did not, the first virtual call would jump through a null
// pointer -> fatal -> reboot. Constructing explicitly removes that dependency.
alignas(plaits::Voice) unsigned char voice_mem[sizeof(plaits::Voice)];
plaits::Voice* voice = nullptr;

plaits::Patch patch;
plaits::Modulations mods;

// ---- main -> audio hand-off ----
// Double buffer: main writes the buffer the audio thread is NOT reading, then flips
// `live`. The audio thread copies buf[live] at the top of each block. Single core and
// the audio thread outranks main, so a copy can never be interrupted by a writer, and
// a writer only ever touches the other buffer.
sp1_synth_params buf[2];
volatile uint32_t live;
volatile uint32_t trig_count;          // incremented by main
uint32_t trig_seen;                    // audio thread only
int trig_blocks_left;                  // audio thread only

// A TRIG is held high for 2 ms of Plaits blocks (8 x 12 samples, 4 x 24). Plaits
// delays its trigger input by kTriggerDelay and needs one block above 0.3 to fire; 2 ms
// is also a normal eurorack trigger width.
const int kTrigBlocks = static_cast<int>(2u * SP1_SYNTH_BLOCKS_PER_MS);

// ---- running clock / burst (audio thread owns the state; main writes the requests) ----
volatile uint32_t samples_per_32nd = 3600;   // burst period, samples (name kept:
                                             // it is the period of the CURRENT division)
float clock_bpm = 120.0f;                    // main thread
uint32_t burst_div = 32;                     // main thread
void recompute_burst_period() {
  // Period of a 1/div note: a whole note is 4 beats = 48000 * 60 * 4 / bpm samples.
  float p = 11520000.0f / (clock_bpm * static_cast<float>(burst_div));
  if (p < static_cast<float>(plaits::kBlockSize)) {
    p = static_cast<float>(plaits::kBlockSize);
  }
  samples_per_32nd = static_cast<uint32_t>(p + 0.5f);
}

// ---- Marbles -> Plaits (M4), audio thread ----
uint8_t mrb_gates_prev;                      // t1..t3 of the previous block (raw)
bool trig_level_prev;                        // TRIG input level of the previous block
volatile uint32_t trig_edges;                // rising edges sent to Plaits (log)
volatile int burst_req;                      // main: 1 while FFWD is held
int burst_on;                                // audio thread
int32_t burst_countdown;                     // samples to the next burst TRIG (stopped)
volatile uint32_t burst_count;               // audio writes, main reads

// ---- the burst's grid while Marbles RUNS (M4e), audio thread ----
// Marbles' master ramp is a 0..1 phase that wraps once per clock tick = once per BEAT.
// A 1/div note is div/4 of a beat, so four ramp cycles are one whole note and
// floor(position-in-whole-note x div) ticks over exactly div times per whole note. That
// makes the burst an exact subdivision of the clock by construction -- there is no
// accumulator to drift and no rate to multiply.
// ---- FREQUENCY held between TRIGs (M4e), audio thread ----
// Latched at each TRIG edge while sp1_synth_params.note_hold is set, so a quantized note
// keeps its pitch for its whole decay and F1 chooses the NEXT one.
float note_held;
bool note_held_valid;

float burst_ramp_prev;                       // last frame's ramp, to spot the wrap
uint32_t burst_cycle;                        // ramp cycles into the whole note, 0..3
uint32_t burst_idx_prev;                     // last grid index seen
bool burst_locked;                           // the grid index above is valid

// Position within a whole note, 0..1, from the ramp cycle count and this frame's phase.
inline float BurstWhole(uint32_t cycle, float ramp) {
  return (static_cast<float>(cycle & 3u) + ramp) * 0.25f;
}

volatile int output_mode = SP1_OUT_MAIN;     // main writes, audio reads per block

// ---- Marbles -> Plaits scaling and clamping (M4a) ----
// Per 5 V, from Plaits' own default CV calibration (plaits/settings.cc): V/Oct and FM
// 60 semitones, TIMBRE and MORPH 1.6, HARMONICS 1.0. A t GATE is read as 0 or +5 V,
// which is what a gate is worth on a CV input.
const float kVoltPerOct = 12.0f;             // semitones per volt
const float kTimbrePerVolt = 0.32f;          // 1.6 per 5 V
const float kHarmPerVolt = 0.2f;             // 1.0 per 5 V
const float kGateVolts = 5.0f;
// ⚠️ Outputs sharing a destination SUM, so three gates on TIMBRE would ask Plaits for
// three times its full modulation range. Each destination's sum is clamped to what ONE
// output could produce on its own (Adara, M4a: "a limiter on all attenuverter input
// levels so stacked gates don't glitch audio"). A clamp, not a compressor: these are
// control values and Plaits constrains them again internally, so clamping is the
// consistent behaviour and costs nothing.
const float kMaxSemis = 60.0f;               // +-5 V of V/Oct or FM
const float kMaxTimbre = 1.6f;
const float kMaxHarm = 1.0f;

inline float Clamp(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

inline float Clamp01(float v) {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// The LEVEL fader (SETTINGS F4): below this it is DISCONNECTED (sp1_plaits_ui.c, LEVEL_OFF).
const float kLevelOff = 0.05f;

inline int16_t Sat16(int32_t v) {
  return static_cast<int16_t>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v));
}
int output_prev = SP1_OUT_MAIN;               // audio thread

// ---- OUT+AUX limiter (M3g) ----
// OUT and AUX each leave Plaits' own limiter with peaks up to ~0.8 of full scale, so
// their sum -- even at -3 dB -- ran hot (Adara, M3f). The sum now has a peak limiter
// of its own, with Plaits' limiter constants (stmlib/dsp/limiter.h: attack 0.05,
// release 0.00002 per sample) and Plaits' ceiling (0.8 of full scale). Unlike
// Plaits' limiter it applies no gain below the ceiling, so a quiet sum is untouched.
// Audio thread only; runs only while OUT+AUX is selected (or being faded to/from).
//
// Issue #22: `kSumCeiling / sum_peak` was a float DIVIDE on every sample spent limiting,
// and OUT+AUX measured ~4 points of the block over OUT. The reciprocal of sum_peak is now
// carried from sample to sample and refined with two Newton steps (the same scheme as
// plaits::Sp1Limiter in the vendored voice.h): sum_peak moves by at most 5 % of the gap
// per sample, so the error is ~1e-6 -- and a real divide is taken at the onset of
// limiting or after any jump the steps could not close. Same attack, release, ceiling.
const float kQ15 = 1.0f / 32768.0f;          // int16 -> +-1
const float kSumCeiling = 0.8f;
float sum_peak = 0.0f;
float sum_recip = 1.0f;
bool sum_recip_valid = false;

inline float Limit(float s) {
  SLOPE(sum_peak, fabsf(s), 0.05f, 0.00002f);
  if (sum_peak <= kSumCeiling) {
    sum_recip_valid = false;
    return s;
  }
  const float e = 1.0f - sum_peak * sum_recip;
  if (!sum_recip_valid || e > 0.25f || e < -0.25f) {
    sum_recip = 1.0f / sum_peak;
    sum_recip_valid = true;
  } else {
    sum_recip = plaits::Sp1Limiter::Newton(sum_peak, sum_recip);
  }
  return s * kSumCeiling * sum_recip;
}

// ---- the drive's transfer curve, as a table (issue #22) ----
// stmlib::SoftClip, tabulated: M4b evaluated it per sample (two range tests and a float
// DIVIDE, 14 cycles on its own) on OUT and on AUX, and the log measured +8-10 % of the
// block for it. A table read with linear interpolation is a few multiply-adds, and any
// other memoryless curve would cost exactly the same -- which is what a future set of
// drive shapes would plug into.
//
// SoftClip is odd and exactly +-1 beyond |x| = 3, so only 0..3 is stored. 512 segments:
// linear interpolation's error is h^2/8 x max|f''|, about 2e-6 of full scale -- under a
// tenth of one 16-bit step, so the table and the formula give the same int16 output to
// within that rounding. constexpr, so the table is built by the compiler and lives in
// flash; no generator script, and it cannot drift from the formula written here, which
// is stmlib::SoftLimit's.
constexpr int kShapeSegments = 512;
constexpr float kShapeSpan = 3.0f;
struct ShapeTable {
  float v[kShapeSegments + 1];
};
constexpr ShapeTable MakeSoftClip() {
  ShapeTable t{};
  for (int i = 0; i <= kShapeSegments; ++i) {
    const float x = kShapeSpan * static_cast<float>(i) / kShapeSegments;
    t.v[i] = x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
  }
  return t;
}
constexpr ShapeTable kSoftClip = MakeSoftClip();
static_assert(kSoftClip.v[0] == 0.0f && kSoftClip.v[kShapeSegments] == 1.0f,
              "SoftClip is 0 at 0 and exactly 1 at |x| = 3");

inline float Shape(float x) {
  const float a = fabsf(x) * (kShapeSegments / kShapeSpan);
  float y = 1.0f;
  if (a < static_cast<float>(kShapeSegments)) {
    const int i = static_cast<int>(a);
    const float fr = a - static_cast<float>(i);
    y = kSoftClip.v[i] + (kSoftClip.v[i + 1] - kSoftClip.v[i]) * fr;
  }
  return x < 0.0f ? -y : y;
}

// ---- the soft-clip drive (M4b; one channel since issue #22) ----
// ONE drive, after the output select and before the OUT+AUX limiter (Adara): OUT, AUX,
// OUT+AUX or OUTxAUX is chosen first and the result is driven, so the sum and the ring
// product distort as one signal -- the intermodulation is the point. The limiter stays
// LAST because it bounds what leaves the device. The stage is applied only while the
// gain is above 1: at g == 1 it is skipped rather than run (see sp1_synth.h).
volatile int drive_step;                     // main writes, audio reads per block
float drive_gain_prev = 1.0f;                // audio thread: last block's gain

// M4e (Adara): UNEVEN steps, so the first is still a fattener and the last is a fuzz.
// A flat 6 dB ladder would reach the same top but give up the subtle bottom end.
const int kDriveDb[SP1_DRIVE_STEPS] = { 0, 3, 8, 15, 24 };

inline float DriveGain(int step) {
  if (step <= 0) {
    return 1.0f;
  }
  if (step > SP1_DRIVE_STEPS - 1) {
    step = SP1_DRIVE_STEPS - 1;
  }
  // 10^(dB/20), computed here rather than tabulated: it runs once per block.
  return powf(10.0f, static_cast<float>(kDriveDb[step]) / 20.0f);
}

// ---- what the voice mixes before its one gate (issue #22) ----
// The output select is a set of weights the voice applies BEFORE its low-pass gate
// (see the vendored plaits/dsp/voice.h): OUT, AUX, their sum at -3 dB (as M3f's OUT+AUX),
// or their product x 2 (as M3f's OUTxAUX). A mode change is a cross-fade of the weights
// across one DMA block, so it never clicks.
plaits::Voice::OutputMix MixFor(int mode) {
  switch (mode) {
    case SP1_OUT_AUX:  return { 0.0f, 1.0f, 0.0f };
    case SP1_OUT_SUM:  return { 0.7071f, 0.7071f, 0.0f };
    case SP1_OUT_RING: return { 0.0f, 0.0f, 1.0f };
    default:           return { 1.0f, 0.0f, 0.0f };
  }
}

plaits::Voice::OutputMix MixLerp(const plaits::Voice::OutputMix& a,
                                 const plaits::Voice::OutputMix& b, float t) {
  return { a.out + (b.out - a.out) * t, a.aux + (b.aux - a.aux) * t,
           a.ring + (b.ring - a.ring) * t };
}

// One output sample after the gate: the drive, then the OUT+AUX limiter (Adara: the
// limiter stays LAST, it bounds what leaves the device), saturated to 16 bits. `in` is
// the voice's unclipped output in 16-bit scale (see the vendored voice.h); with both
// stages off it is converted exactly as Plaits' own output stage did.
template <bool kLimit, bool kDrive>
inline int16_t Stage(float in, float g) {
  if (!kLimit && !kDrive) {
    return Sat16(1 + static_cast<int32_t>(in));
  }
  float v = in * kQ15;
  if (kDrive) {
    v = Shape(v * g);
  }
  if (kLimit) {
    v = Limit(v);
  }
  return Sat16(static_cast<int32_t>(v * 32768.0f));
}

// The same with the drive state known only at run time: the one block in which the
// drive gain ramps.
inline int16_t StageAny(bool limit, bool drive, float in, float g) {
  if (limit) {
    return drive ? Stage<true, true>(in, g) : Stage<true, false>(in, g);
  }
  return drive ? Stage<false, true>(in, g) : Stage<false, false>(in, g);
}

// A whole Plaits block in a steady state -- no drive ramp -- which is every block but
// one per change. Both switches are template arguments, so the per-sample loop carries
// no branch on either.
template <bool kLimit, bool kDrive>
void StageBlock(const float* in, int16_t* out, float g) {
  for (size_t i = 0; i < plaits::kBlockSize; ++i) {
    out[i] = Stage<kLimit, kDrive>(in[i], g);
  }
}

inline void StageBlockAny(bool limit, bool drive, const float* in, int16_t* out,
                          float g) {
  if (limit) {
    drive ? StageBlock<true, true>(in, out, g) : StageBlock<true, false>(in, out, g);
  } else {
    drive ? StageBlock<false, true>(in, out, g) : StageBlock<false, false>(in, out, g);
  }
}

// ---- Marbles -> Plaits routing, resolved once per DMA block (issue #22) ----
// The routing depends only on where each output is sent, which cannot change inside a
// DMA block, so the "which destination, what scale, is it patched" decisions are taken
// once per DMA block into a short list, and each Plaits block just walks the list. The
// list keeps the old summation order (t1..t3, then X1..X3, Y) and the same products, so
// the sums are bit-identical to the per-block switches this replaces; the cost grows
// with the number of routed outputs (a patch setting), never with modulation.
enum RouteDest { kRouteNote, kRouteFm, kRouteTimbre, kRouteMorph, kRouteHarm,
                 kRouteLevel, kRouteCount };

struct Route {
  uint8_t source;       // 0..2 = t1..t3 (a gate, 0 or +5 V), 3..6 = X1..X3, Y
  uint8_t dest;         // RouteDest
  float scale;          // per volt, as Plaits' own CV calibration (see kVoltPerOct)
};

struct Routing {
  Route route[7];
  int count;
  bool fm_patched, timbre_patched, morph_patched, harm_patched, level_routed;
};

void AddRoute(Routing* r, int source, int dest) {
  float scale;
  RouteDest d;
  switch (dest) {
    case SP1_DEST_VOCT:   d = kRouteNote;   scale = kVoltPerOct; break;
    case SP1_DEST_FM:     d = kRouteFm;     scale = kVoltPerOct; r->fm_patched = true; break;
    case SP1_DEST_TIMBRE: d = kRouteTimbre; scale = kTimbrePerVolt; r->timbre_patched = true; break;
    case SP1_DEST_MORPH:  d = kRouteMorph;  scale = kTimbrePerVolt; r->morph_patched = true; break;
    case SP1_DEST_HARM:   d = kRouteHarm;   scale = kHarmPerVolt; r->harm_patched = true; break;
    case SP1_DEST_LEVEL:  d = kRouteLevel;  scale = 0.2f; r->level_routed = true; break;
    default: return;      // none, and TRIG (handled by the gate mask)
  }
  r->route[r->count].source = static_cast<uint8_t>(source);
  r->route[r->count].dest = static_cast<uint8_t>(d);
  r->route[r->count].scale = scale;
  ++r->count;
}

Routing ResolveRouting(const sp1_synth_params& c) {
  Routing r = {};
  for (int t = 0; t < 3; ++t) {
    // A t output is a gate: V/Oct is not one of its destinations (as before).
    if (c.mrb_t_dest[t] != SP1_DEST_VOCT) {
      AddRoute(&r, t, c.mrb_t_dest[t]);
    }
  }
  for (int k = 0; k < 4; ++k) {
    AddRoute(&r, 3 + k, c.mrb_dest[k]);
  }
  return r;
}

}  // namespace

extern "C" void sp1_synth_init(void) {
  if (voice) {
    return;
  }
  sp1_marbles_init();                  // M4: Marbles' generators, stopped
  voice = new (voice_mem) plaits::Voice();
  stmlib::BufferAllocator allocator(arena, sizeof(arena));
  voice->Init(&allocator);

  // Plaits' factory defaults for everything the SP-1 cannot reach yet
  // (plaits/settings.cc: lpg_colour 0, decay 128/256; octave 255 = full range).
  patch.note = 60.0f;
  patch.harmonics = 0.0f;
  patch.timbre = 0.5f;
  patch.morph = 0.5f;
  patch.frequency_modulation_amount = 0.0f;
  patch.timbre_modulation_amount = 0.0f;
  patch.morph_modulation_amount = 0.0f;
  patch.harmonics_modulation_amount = 0.0f;   // ours (M4a): see the voice.h override
  patch.engine = SP1_SYNTH_ENGINE_INITIAL;
  patch.decay = 0.5f;
  patch.lpg_colour = 0.0f;

  // Nothing is patched except TRIG: PLAY is the trigger, so the LPG gates notes
  // rather than free-running (UI-SPEC: "an unpatched TRIG makes the LPG free-run").
  // LEVEL stays unpatched -> accent 0.8 and the LPG envelope fires on each TRIG.
  mods.engine = 0.0f;
  mods.note = 0.0f;
  mods.frequency = 0.0f;
  mods.harmonics = 0.0f;
  mods.timbre = 0.0f;
  mods.morph = 0.0f;
  mods.trigger = 0.0f;
  mods.level = 0.0f;
  mods.frequency_patched = false;
  mods.timbre_patched = false;
  mods.morph_patched = false;
  mods.harmonics_patched = false;             // ours (M4a)
  mods.trigger_patched = true;
  mods.level_patched = false;

  sp1_synth_params d;
  d.note = 60.0f;
  d.harmonics = 0.0f;
  d.timbre = 0.5f;
  d.morph = 0.5f;
  d.timbre_mod = d.fm_mod = d.morph_mod = d.harm_mod = 0.0f;
  d.decay = 0.5f;
  d.lpg_colour = 0.0f;
  d.level = 0.0f;
  d.level_patched = 0;
  d.level_pos = 0.0f;
  d.engine_centre = 0u;
  d.freq_per_travel = 96.0f;
  d.engine = SP1_SYNTH_ENGINE_INITIAL;
  for (int k = 0; k < 3; ++k) {
    d.mrb_t_dest[k] = SP1_DEST_NONE;
  }
  for (int k = 0; k < 4; ++k) {
    d.mrb_dest[k] = SP1_DEST_NONE;
  }
  buf[0] = d;
  buf[1] = d;
  live = 0;

  trig_seen = trig_count;
  trig_blocks_left = 0;
}

// ---- where the block went (issue #22; sp1_synth.h) ----
const volatile uint32_t* cyc_counter;        // NULL = no profile (host, or not set yet)
sp1_synth_profile prof;                      // audio thread: spans of the last render

inline uint32_t Now() {
  return cyc_counter ? *cyc_counter : 0u;
}
const volatile uint32_t* miss_counter;       // NULL = no miss profile (#32, diagnostics only)
inline uint32_t Miss() {
  return miss_counter ? *miss_counter : 0u;
}
uint32_t (*midi_clock)(void);                // NULL = untimed (sp1_synth_set_midi_clock)

extern "C" void sp1_synth_render(int16_t* out, uint32_t frames) {
  const uint32_t prof_t0 = Now();
  const uint32_t miss_t0 = Miss();
  uint32_t prof_eng = 0u;
  uint32_t prof_post = 0u;
  uint32_t miss_eng = 0u;
  uint32_t miss_post = 0u;
  prof = sp1_synth_profile{};
  if (!voice) {
    for (uint32_t i = 0; i < frames; ++i) {
      out[i] = 0;
    }
    return;
  }

  // Controls: one consistent set per DMA block.
  const uint32_t li = live;
  std::atomic_signal_fence(std::memory_order_seq_cst);
  const sp1_synth_params c = buf[li];
  patch.note = c.note;
  patch.harmonics = c.harmonics;
  patch.timbre = c.timbre;
  patch.morph = c.morph;
  patch.timbre_modulation_amount = c.timbre_mod;
  patch.frequency_modulation_amount = c.fm_mod;
  patch.morph_modulation_amount = c.morph_mod;
  patch.harmonics_modulation_amount = c.harm_mod;
  patch.decay = c.decay;
  patch.lpg_colour = c.lpg_colour;
  patch.engine = c.engine;
  // UI-SPEC: the LEVEL fader (SETTINGS F4 since M4a) below 5 % DISCONNECTS level --
  // the flag, not a zero value, is what changes Plaits' behaviour (LPG triggered by
  // TRIG vs VCA held open = drone). A Marbles output routed to LEVEL patches it too;
  // that is decided per block below.
  const bool level_fader_patched = c.level_patched != 0;

  // Which t outputs go to TRIG this block. Built here rather than carried as a mask so
  // the t destinations are one list with one meaning (Adara, M4a).
  uint8_t trig_mask = 0u;
  for (int t = 0; t < 3; ++t) {
    if (c.mrb_t_dest[t] == SP1_DEST_TRIG) {
      trig_mask |= static_cast<uint8_t>(1u << t);
    }
  }

  bool pulse_started = false;          // a new RWD / burst TRIG this audio block
  const uint32_t tc = trig_count;
  if (tc != trig_seen) {
    trig_seen = tc;
    trig_blocks_left = kTrigBlocks;    // a second press mid-pulse just extends it
    pulse_started = true;
  }

#if defined(CONFIG_SP1_MIDI)
  // ---- MIDI (M5a): this audio block's messages, each placed at its Plaits block, and the
  // CC offsets, smoothed once per audio block (sp1_midi.h). Taken BEFORE Marbles renders:
  // this block's clock ticks and transport are Marbles' clock (M5b). ----
  const uint32_t prof_mi0 = Now();
  float moff[SP1_MIDI_AUDIO_DESTS];
  const bool midi = sp1_midi_audio_begin(midi_clock ? midi_clock() : 0u, frames / plaits::kBlockSize, c.engine_centre,
                                         moff);
  {
    static_assert(SP1_MIDI_TP_START == SP1_MRB_TP_START &&
                  SP1_MIDI_TP_CONTINUE == SP1_MRB_TP_CONTINUE &&
                  SP1_MIDI_TP_STOP == SP1_MRB_TP_STOP, "one set of transport codes");
    sp1_midi_clock clk;
    sp1_midi_audio_clock(&clk);
    sp1_marbles_clock(clk.external ? clk.beats : nullptr, clk.transport);
  }
  prof.pre_midi = Now() - prof_mi0;
#endif

  // Marbles, one sample per Plaits block (the 4 kHz rule, sp1_marbles.h). Stopped,
  // it renders nothing and every Marbles input below stays unpatched.
  const bool mrb = sp1_marbles_running();
  const uint32_t prof_m0 = Now();
  const uint32_t miss_m0 = Miss();
  sp1_marbles_render(frames / plaits::kBlockSize);
  prof.mrb = Now() - prof_m0;
  prof.miss_mrb = Miss() - miss_m0;
  if (!mrb) {
    mrb_gates_prev = 0u;
  }
  // This block's routing and Marbles' frames (issue #22: see ResolveRouting).
  const uint32_t prof_r0 = Now();
  const Routing routing = ResolveRouting(c);
  prof.pre_route = Now() - prof_r0;
  const uint8_t* const mrb_gates = sp1_marbles_gate_frames();
  const float* const mrb_volts = sp1_marbles_volt_frames();
  const float* const mrb_ramp = sp1_marbles_ramp_frames();


  // ---- the burst (M4e): phase-locked to Marbles' master ramp while it runs ----
  // Stopped there is nothing to lock to, so the free-running accumulator stands: fire at
  // once on the rising edge, then count down per Plaits block.
  const int breq = burst_req;
  if (breq && !burst_on) {
    burst_countdown = 0;               // first TRIG in this very block (stopped)
    burst_locked = false;              // running: pick the grid up below
  }
  burst_on = breq;
  const int32_t sp32 = static_cast<int32_t>(samples_per_32nd);
  const int omode = output_mode;
  // Output select (issue #22: weights into the voice's one gate). A change cross-fades
  // from the mode being left (output_prev) across this whole DMA block.
  const plaits::Voice::OutputMix mix_now = MixFor(omode);
  const plaits::Voice::OutputMix mix_was = MixFor(output_prev);
  const bool fading = omode != output_prev;
  const float fade_step = static_cast<float>(plaits::kBlockSize) /
                          static_cast<float>(frames ? frames : plaits::kBlockSize);
  // The OUT+AUX limiter runs while OUT+AUX is selected or being faded to or from.
  const bool limiting = omode == SP1_OUT_SUM || output_prev == SP1_OUT_SUM;

  // Drive: one linear ramp across the DMA block, so a step never clicks. Both ends at
  // exactly 1.0 means the stage is off for the whole block and is skipped.
  const float drive_target = DriveGain(drive_step);
  const float drive_from = drive_gain_prev;
  const bool driving = drive_target > 1.0f || drive_from > 1.0f;
  const float drive_inc = frames ? (drive_target - drive_from) /
                                   static_cast<float>(frames) : 0.0f;
  float drive_g = drive_from;
  drive_gain_prev = drive_target;

#if defined(CONFIG_SP1_MIDI)
  // ---- MIDI's CC offsets (M5a), taken above ----
  // A CC is a second hand on its fader: the offset is in fader-travel units, so a unipolar
  // parameter takes it as is and an attenuverter (-1..+1, two units of value per unit of
  // travel) takes twice it. Clamped to the parameter's range, as the fader is. Applied ONCE
  // here: the offsets are constant across the audio block. With MIDI idle -- nothing plugged
  // in, or nothing ever sent -- `midi` is false and nothing below runs per Plaits block.
  if (midi) {
    patch.timbre = Clamp01(c.timbre + moff[SP1_MIDI_D_TIMBRE]);
    patch.morph = Clamp01(c.morph + moff[SP1_MIDI_D_MORPH]);
    patch.harmonics = Clamp01(c.harmonics + moff[SP1_MIDI_D_HARMONICS]);
    patch.frequency_modulation_amount =
        Clamp(c.fm_mod + 2.0f * moff[SP1_MIDI_D_FM_ATTENUVERTER], -1.0f, 1.0f);
    patch.timbre_modulation_amount =
        Clamp(c.timbre_mod + 2.0f * moff[SP1_MIDI_D_TIMBRE_ATTENUVERTER], -1.0f, 1.0f);
    patch.morph_modulation_amount =
        Clamp(c.morph_mod + 2.0f * moff[SP1_MIDI_D_MORPH_ATTENUVERTER], -1.0f, 1.0f);
    patch.harmonics_modulation_amount =
        Clamp(c.harm_mod + 2.0f * moff[SP1_MIDI_D_HARMONICS_ATTENUVERTER], -1.0f, 1.0f);
    patch.lpg_colour = Clamp01(c.lpg_colour + moff[SP1_MIDI_D_LPG_COLOUR]);
    patch.decay = Clamp01(c.decay + moff[SP1_MIDI_D_LPG_DECAY]);
  }
  // The LEVEL fader with its CC, and the FREQUENCY CC in semitones: per audio block too.
  const float midi_level_fader = (midi && level_fader_patched)
      ? Clamp01((c.level_pos + moff[SP1_MIDI_D_LEVEL] - kLevelOff) / (1.0f - kLevelOff))
      : 0.0f;
  const float midi_freq = midi ? moff[SP1_MIDI_D_FREQUENCY] * c.freq_per_travel : 0.0f;
#endif

  // Everything above runs once per audio block (#32: `pre`, Marbles excluded).
  prof.pre = Now() - prof_t0 - prof.mrb;
  prof.miss_pre = Miss() - miss_t0 - prof.miss_mrb;

  float v[plaits::kBlockSize];
  uint32_t j = 0;                      // Plaits block index = Marbles frame index
  while (frames >= plaits::kBlockSize) {
    bool new_edge = pulse_started && j == 0;
#if defined(CONFIG_SP1_MIDI)
    // ---- MIDI into Plaits (M5a), one Plaits block at a time: the strike, the gate and the
    // pitch. A note-on strikes TRIG here, joining every other TRIG source (RWD, the burst, a
    // routed t gate) through the same pulse and re-strike logic below. ----
    sp1_midi_frame mf = { false, false, 0.0f, 0.0f };
    if (midi) {
      sp1_midi_audio_block(j, &mf);
      if (mf.trig) {
        trig_blocks_left = kTrigBlocks;
        new_edge = true;
      }
    }
#endif
    if (burst_on && mrb) {
      // ---- running: the grid IS Marbles' clock ----
      const float ramp = mrb_ramp[j];
      if (!burst_locked) {
        // First frame of this hold. Quantise to the NEAREST grid point rather than
        // firing on the press: if we are in the first half of the current grid cell,
        // fire now (just after a boundary); otherwise let the next boundary do it.
        burst_cycle = 0u;
        burst_ramp_prev = ramp;
        const float pos = BurstWhole(burst_cycle, ramp) * static_cast<float>(burst_div);
        burst_idx_prev = static_cast<uint32_t>(pos);
        burst_locked = true;
        if (pos - static_cast<float>(burst_idx_prev) < 0.5f) {
          trig_blocks_left = kTrigBlocks;
          burst_count = burst_count + 1u;
          new_edge = true;
        }
      } else {
        if (ramp < burst_ramp_prev) {        // the master ramp wrapped: next beat
          burst_cycle = (burst_cycle + 1u) & 3u;
        }
        burst_ramp_prev = ramp;
        const uint32_t idx = static_cast<uint32_t>(
            BurstWhole(burst_cycle, ramp) * static_cast<float>(burst_div));
        if (idx != burst_idx_prev) {
          burst_idx_prev = idx;
          // Restart the pulse. At 1/32 the gap (>= ~1500 samples even at 240 BPM) is
          // far longer than the 96-sample pulse, so each burst TRIG is a clean edge.
          trig_blocks_left = kTrigBlocks;
          burst_count = burst_count + 1u;
          new_edge = true;
        }
      }
    } else if (burst_on) {
      // ---- stopped: the free-running accumulator (M3b) ----
      if (burst_countdown <= 0) {
        trig_blocks_left = kTrigBlocks;
        burst_countdown += sp32;
        burst_count = burst_count + 1u;
        new_edge = true;
      }
      burst_countdown -= static_cast<int32_t>(plaits::kBlockSize);
    }
    bool level = trig_blocks_left > 0;
    if (trig_blocks_left > 0) {
      --trig_blocks_left;
    }

    // ---- Marbles -> Plaits, only while the clock runs (Adara, M4) ----
    // Destinations sum, scaled as Plaits scales its own CV inputs (plaits/settings.cc,
    // default calibration, per 5 V): V/Oct and FM 60 semitones (12 per volt; FM then x
    // the FM attenuverter), TIMBRE and MORPH 1.6, HARMONICS 1.0 (x their attenuverters).
    // A t output counts as its gate, 0 or +5 V (M4a).
    //
    // ⚠️ A destination counts as PATCHED whether its gate is high or low. A low gate
    // contributes 0 V, not "nothing": if the patched flag followed the gate, Plaits would
    // hand the parameter back to its internal envelope between gates and the
    // attenuverter would flip meaning several times a second.
    float m[kRouteCount] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    bool fm_patched = false, timbre_patched = false, morph_patched = false;
    bool harm_patched = false, level_routed = false;
    if (mrb) {
      // TRIG: the routed t GATES (Marbles' gate lengths reach Plaits), and any
      // rising edge among them counts even if another routed gate is already high.
      // Edges are taken from the RAW gates, then masked: routing a t output on
      // while its gate is already high is not an edge (no stray TRIG).
      const uint8_t raw = mrb_gates[j];
      if (raw & static_cast<uint8_t>(~mrb_gates_prev) & trig_mask) {
        new_edge = true;
      }
      mrb_gates_prev = raw;
      level = level || (raw & trig_mask) != 0u;

      const float* const volts = &mrb_volts[4u * j];
      for (int i = 0; i < routing.count; ++i) {
        const Route& r = routing.route[i];
        const float value = r.source < 3u
            ? ((raw >> r.source) & 1u ? kGateVolts : 0.0f)
            : volts[r.source - 3u];
        m[r.dest] += r.scale * value;
      }
      fm_patched = routing.fm_patched;
      timbre_patched = routing.timbre_patched;
      morph_patched = routing.morph_patched;
      harm_patched = routing.harm_patched;
      level_routed = routing.level_routed;
    }
    // One output's worth, whatever is stacked on it (M4a).
    const float m_note = Clamp(m[kRouteNote], -kMaxSemis, kMaxSemis);
    const float m_fm = Clamp(m[kRouteFm], -kMaxSemis, kMaxSemis);
    const float m_timbre = Clamp(m[kRouteTimbre], -kMaxTimbre, kMaxTimbre);
    const float m_morph = Clamp(m[kRouteMorph], -kMaxTimbre, kMaxTimbre);
    const float m_harm = Clamp(m[kRouteHarm], -kMaxHarm, kMaxHarm);
    const float m_level = Clamp(m[kRouteLevel], 0.0f, 1.0f);
    // A new edge while TRIG is already high: one low block, so Plaits sees it.
    if (new_edge && trig_level_prev) {
      level = false;
    }
    if (level && !trig_level_prev) {
      trig_edges = trig_edges + 1u;
    }
    trig_level_prev = level;
    mods.trigger = level ? 1.0f : 0.0f;
    mods.note = m_note;
    mods.frequency = m_fm;
    mods.frequency_patched = fm_patched;
    mods.timbre = m_timbre;
    mods.morph = m_morph;
    mods.harmonics = m_harm;
    mods.timbre_patched = timbre_patched;
    mods.morph_patched = morph_patched;
    mods.harmonics_patched = harm_patched;
    // LEVEL comes from the SETTINGS fader, a Marbles route, or both (they add).
    mods.level_patched = level_fader_patched || level_routed;
    float level_value = c.level + m_level;
#if defined(CONFIG_SP1_MIDI)
    // ...and from MIDI (M5a): its CC moves the fader position, and a held key -- or the
    // release after it -- connects LEVEL and holds it open at the gate's height, so Plaits
    // sustains while the key is down and closes through its LPG when it comes up. Only
    // while a key or its release is sounding (Adara, C: option a), so a Marbles pattern
    // that only uses TRIG is not silenced by a MIDI port that is merely plugged in.
    if (midi) {
      level_value = midi_level_fader + m_level + (mf.owns_level ? mf.gate : 0.0f);
      mods.level_patched = mods.level_patched || mf.owns_level;
    }
#endif
    mods.level = Clamp(level_value, 0.0f, 1.0f);

    // ---- hold the note between TRIGs, while a scale is selected (M4e, Adara) ----
    // `new_edge` is every TRIG source at once -- RWD, the burst, and a routed Marbles t
    // gate -- so a held note is re-chosen by whichever of them fires, which is what
    // "once every TRIG" has to mean when three things can trigger.
    //
    // ⚠️ Only patch.note is latched. Marbles' V/Oct (mods.note) is deliberately NOT, and
    // cannot collide: selecting a scale unpatches every V/Oct route (the M4b interlock),
    // so the two are never live together.
    if (c.note_hold) {
      if (new_edge || !note_held_valid) {
        note_held = c.note;
        note_held_valid = true;
      }
      patch.note = note_held;
    } else {
      note_held_valid = false;         // re-latch on the first TRIG after it comes back
      patch.note = c.note;
    }
#if defined(CONFIG_SP1_MIDI)
    // MIDI pitch (M5a): added like a V/Oct cable, note 60 = +0, and OUTSIDE the latch --
    // in legato a new note arrives without a TRIG and must still be heard. The FREQUENCY
    // CC moves F1, scaled by the octave range (sp1_synth.h, freq_per_travel).
    if (midi) {
      patch.note += mf.note + midi_freq;
    }
#endif
    ++j;
    const uint32_t prof_e0 = Now();
    const uint32_t miss_e0 = Miss();
    if (fading) {
      const float t0 = static_cast<float>(j - 1u) * fade_step;
      voice->Render(patch, mods, MixLerp(mix_was, mix_now, t0),
                    MixLerp(mix_was, mix_now, t0 + fade_step), v, plaits::kBlockSize);
    } else {
      voice->Render(patch, mods, mix_now, mix_now, v, plaits::kBlockSize);
    }
    const uint32_t prof_e1 = Now();
    const uint32_t miss_e1 = Miss();
    prof_eng += prof_e1 - prof_e0;
    miss_eng += miss_e1 - miss_e0;
#if defined(CONFIG_SP1_MIDI)
    // When a released note's LPG has closed, MIDI lets go of LEVEL (sp1_midi.h).
    if (midi) {
      sp1_midi_audio_lpg(voice->sp1_lpg_gain(), voice->sp1_lpg_bypassed());
    }
#endif
    if (drive_inc == 0.0f) {
      StageBlockAny(limiting, driving, v, out, drive_g);
    } else {
      for (size_t i = 0; i < plaits::kBlockSize; ++i) {
        drive_g += drive_inc;
        out[i] = StageAny(limiting, drive_g > 1.0f, v[i], drive_g);
      }
    }
    prof_post += Now() - prof_e1;
    miss_post += Miss() - miss_e1;
    out += plaits::kBlockSize;
    frames -= plaits::kBlockSize;
  }
  output_prev = omode;
  prof.eng = prof_eng;
  prof.post = prof_post;
  prof.total = Now() - prof_t0;
  prof.miss_eng = miss_eng;
  prof.miss_post = miss_post;
  prof.miss_total = Miss() - miss_t0;
}

extern "C" void sp1_synth_set_cycle_counter(const volatile uint32_t* counter) {
  cyc_counter = counter;
}

extern "C" void sp1_synth_set_miss_counter(const volatile uint32_t* counter) {
  miss_counter = counter;
}

extern "C" void sp1_synth_set_midi_clock(uint32_t (*now)(void)) {
  midi_clock = now;
}

extern "C" void sp1_synth_last_profile(sp1_synth_profile* out) {
  *out = prof;
}

extern "C" void sp1_synth_set_params(const sp1_synth_params* p) {
  const uint32_t next = live ^ 1u;
  buf[next] = *p;
  // Compiler barrier: `buf` is not volatile, so without this the compiler may move
  // the struct stores after the flip. No CPU barrier is needed on one M4 core.
  std::atomic_signal_fence(std::memory_order_seq_cst);
  live = next;                          // the flip is the publish
}

extern "C" void sp1_synth_trigger(void) {
  trig_count = trig_count + 1u;        // main thread is the only writer
}

extern "C" void sp1_synth_set_tempo(float bpm) {
  if (bpm < 1.0f) bpm = 1.0f;         // Marbles' RATE reaches ~4 BPM at x0.25
  if (bpm > 2000.0f) bpm = 2000.0f;
  if (bpm != clock_bpm) {
    clock_bpm = bpm;
    recompute_burst_period();
  }
}

extern "C" void sp1_synth_set_burst_div(uint32_t div) {
  if (div < 1u) div = 1u;
  if (div > 128u) div = 128u;
  burst_div = div;
  recompute_burst_period();
}

extern "C" void sp1_synth_burst(int on) {
  burst_req = on ? 1 : 0;
}

extern "C" void sp1_synth_set_output(enum sp1_synth_output o) {
  output_mode = (o >= SP1_OUT_MAIN && o < SP1_OUT_COUNT) ? o : SP1_OUT_MAIN;
}

extern "C" void sp1_synth_set_drive(int step) {
  drive_step = step < 0 ? 0 : (step > SP1_DRIVE_STEPS - 1 ? SP1_DRIVE_STEPS - 1 : step);
}

extern "C" int sp1_synth_drive_db(int step) {
  if (step <= 0) {
    return 0;
  }
  if (step > SP1_DRIVE_STEPS - 1) {
    step = SP1_DRIVE_STEPS - 1;
  }
  return kDriveDb[step];
}

extern "C" int sp1_synth_drive(void) {
  return drive_step;
}

extern "C" uint32_t sp1_synth_trig_edges(void) {
  return trig_edges;
}

extern "C" uint32_t sp1_synth_burst_count(void) {
  return burst_count;
}

static_assert(SP1_SYNTH_BLOCK == plaits::kBlockSize,
              "SP1_SYNTH_BLOCK must match Plaits' kBlockSize");
static_assert(plaits::kBlockSize == 12 || plaits::kBlockSize == 24,
              "#32: only 12 and 24 are valid -- the time constants divide 48 by it");

// M3e trims (firmware/CMakeLists.txt, "Plaits overrides"): prove the generated header
// is the one every Plaits file compiled against.
static_assert(plaits::kNumStrings == CONFIG_SP1_STRING_VOICES,
              "String engine override not applied: check the include order in "
              "firmware/CMakeLists.txt");
static_assert(plaits::kNumParticles == CONFIG_SP1_PARTICLES,
              "Particle override not applied");
static_assert(plaits::kMaxNumModes == CONFIG_SP1_MODAL_MODES,
              "Modal override not applied");
static_assert(plaits::kMaxNumModes % plaits::kModeBatchSize == 0,
              "CONFIG_SP1_MODAL_MODES must be a multiple of 4: the resonator renders "
              "modes in batches of 4 and would silently drop the remainder");
// Only our additive_engine.h (src/plaits_ovr) defines this; upstream's does not.
static_assert(plaits::kAmplitudeUpdatePeriod >= 1,
              "Additive replacement not applied");
static_assert(plaits::Patch::kSp1HarmonicsAttenuverter,
              "voice.h override not applied: HARMONICS has no attenuverter");
static_assert(plaits::Ensemble::kSp1Override, "Ensemble replacement not applied");
static_assert(plaits::StringSynthOscillator::kSp1Override,
              "String-synth oscillator replacement not applied");
static_assert(plaits::Voice::kSp1LpgState,
              "voice.h replacement not applied: no LPG state for MIDI (M5a)");
static_assert(plaits::Voice::kSp1SingleLpg,
              "voice.h replacement not applied: two low-pass gates");
static_assert(plaits::VariableShapeOscillator::kSp1Override &&
                  plaits::VariableSawOscillator::kSp1Override,
              "oscillator replacements not applied: two divides per sample");
static_assert(plaits::ChordEngine::kSp1SharedWaveform &&
                  plaits::WavetableOscillator<128, 15>::kSp1Override,
              "chord engine replacement not applied: per-voice waveform");
static_assert(plaits::ChordEngine::kSp1BoundedCrossfade,
              "chord_engine.h override not applied: Chords' crossfade is unbounded");
