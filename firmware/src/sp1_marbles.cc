// wakes-sp1 — the Marbles generators. See sp1_marbles.h.
//
// Glue around marbles::TGenerator and marbles::XYGenerator (Mutable Instruments
// Marbles, Emilie Gillet, MIT), vendored in third_party/eurorack/marbles. What this
// file does is what marbles/marbles.cc's Process() does for the module, minus every
// input: no CV reader, no clock inputs, no self-patching detector, no scale recorder.
// The y-divider ratios below are copied from marbles.cc; the preset scales from
// marbles/settings.cc (sp1_marbles_scales.inc). Attributed in NOTICE.
//
// Deliberately includes NO Zephyr headers: the DSP layer stays framework-agnostic.

#include "sp1_marbles.h"
/* For enum sp1_mui_dest, which INTELLIGENT reads (M4c). sp1_synth.cc includes it for the
 * same reason -- the destination list is UI vocabulary that the DSP glue has to act on. */
#include "sp1_marbles_ui.h"
#include "sp1_synth.h"          // SP1_SYNTH_BLOCK: one Marbles sample per Plaits block (#32)

#include <atomic>
#include <cmath>

#include "marbles/random/random_generator.h"
#include "marbles/random/random_stream.h"
#include "marbles/random/t_generator.h"
#include "marbles/random/x_y_generator.h"

namespace marbles {
#include "sp1_marbles_scales.inc"

// ---- the seventh scale: Adara's slot (M4a) ----
// sp1_marbles_scales.inc is verbatim from marbles/settings.cc and stays that way, so
// the free slot lives here. Chromatic, every degree equally likely -- safe and useful
// as it stands, and marked OFF in docs/MARBLES-SETTINGS.md until it is replaced. The
// format: base_interval in volts (1.0 = an octave at 1 V/oct), how many degrees, then
// each degree as { volts above the root, weight 0-255 }. Weight is how often the STEPS
// quantizer picks that degree, NOT a level: 255 = always available, 0 = never. One
// semitone = 1/12 V = 0.0833333f.
const Scale sp1_free_scale = {
  1.0f,
  12,
  {
    { 0.0000f, 255 },  // C
    { 0.0833f, 255 },  // C#
    { 0.1667f, 255 },  // D
    { 0.2500f, 255 },  // D#
    { 0.3333f, 255 },  // E
    { 0.4167f, 255 },  // F
    { 0.5000f, 255 },  // F#
    { 0.5833f, 255 },  // G
    { 0.6667f, 255 },  // G#
    { 0.7500f, 255 },  // A
    { 0.8333f, 255 },  // A#
    { 0.9167f, 255 },  // B
  }
};

const Scale* const sp1_scales[SP1_MARBLES_SCALES] = {
  &preset_scales[0], &preset_scales[1], &preset_scales[2],
  &preset_scales[3], &preset_scales[4], &preset_scales[5],
  &sp1_free_scale,
};
}

namespace {

using marbles::GroupSettings;
using marbles::Ratio;

// marbles/marbles.cc: the Y clock as a division of t2.
const Ratio kYDividerRatios[12] = {
  { 1, 64 }, { 1, 48 }, { 1, 32 }, { 1, 24 }, { 1, 16 }, { 1, 12 },
  { 1, 8 }, { 1, 6 }, { 1, 4 }, { 1, 3 }, { 1, 2 }, { 1, 1 },
};

marbles::RandomGenerator random_generator;
marbles::RandomStream random_stream;
marbles::TGenerator t_generator;
marbles::XYGenerator xy_generator;
bool initialised;

// Marbles' own per-block buffers (marbles.cc), sized for one audio block.
const uint32_t kN = SP1_MARBLES_MAX_FRAMES;
float ramp_buffer[kN * 4];
bool gate_buffer[kN * 2];
float volt_buffer[kN * 4];
stmlib::GateFlags no_clock[kN];          // no external clock, ever

// ---- X / Y at 1 kHz (issue #22; sp1_marbles.h) ----
const uint32_t kXYDecim = SP1_SYNTH_BLOCKS_PER_MS;   // Plaits blocks per 1 kHz X/Y sample
                                                     // (4 of 12 samples, 2 of 24; #32)
const uint32_t kXYN = kN / kXYDecim;
float xy_ramp_buffer[kXYN * 4];                // every 4th ramp sample (+ Y's divider)
float xy_volt_buffer[kXYN * 4];
float xy_prev_volts[4];                        // last 1 kHz value of the previous group
float xy_prev_src[3] = { 1.0f, 1.0f, 1.0f };   // last 4 kHz sample of master, t1, t3

uint8_t frame_gates[kN];                 // audio thread
float frame_volts[kN][4];
// The master ramp, per frame. Exposed so the FFWD burst can be an exact subdivision of
// the clock rather than a second, independent oscillator (M4e -- sp1_synth.h).
float frame_ramp[kN];

// ---- the active scale (audio thread) ----
// Marbles gives every OutputChannel six quantizer slots and preloads all six. We use
// ONE slot and load the selected scale into it on change instead, which costs nothing
// while the scale is not changing and lifts the six-scale ceiling -- there are seven
// now and the count is ours to pick. Loading happens in the AUDIO thread, from the
// published parameters, so main can never be rewriting a quantizer's table while the
// audio thread reads it.
//
// ⚠️ It must be loaded into ALL FOUR channels, Y included. XYGenerator::LoadScale(i,
// scale) only walks the three X channels, so M4 left Y's quantizer holding Marbles'
// empty default scale -- invisible only because Y STEPS defaults below its centre,
// where Y uses the lag processor and never quantizes.
int loaded_scale = -1;                   // -1 = nothing loaded yet

// Deferred re-seed (M4d): main records, the audio thread performs. See sp1_marbles.h.
volatile uint32_t reseed_val;
volatile uint32_t reseed_req;            // main increments, audio compares
uint32_t reseed_seen;                    // audio thread

// ---- the scale Plaits' FREQUENCY quantizes to (MAIN thread only, M4b) ----
// Its own Quantizer instance, so it shares no state with the four the audio thread uses.
//
// `scale_level` picks how selective the quantizer is, per scale. marbles::Quantizer takes
// an `amount` that chooses a weight threshold from {0, 16, 32, 64, 128, 192, 255} (level 1
// .. 7; level 0 is no quantization at all), and a scale's own weights decide which of its
// degrees survive. Level 3 (weight >= 32) is what yields the note set each scale's NAME
// means -- verified against the tables:
//     major       7  C  D  E  F  G  A  B
//     minor       7  C  D  Eb F  G  Ab Bb
//     pentatonic  5  C  D  F  G  A
//     bhairav     7  and shri 7, both their own seven-degree ragas
// Pelog and the free slot are the exceptions: their degrees are already exactly the scale,
// so they take level 1 (every degree) -- at level 3 pelog would lose two of its seven.
// ⚠️ Level 1 on major/minor/bhairav/shri would admit all twelve degrees, i.e. quantize to
// a chromatic scale and make four of the six scales sound identical. Do not "simplify"
// this to one constant.
const int scale_level[SP1_MARBLES_SCALES] = { 3, 3, 3, 1, 3, 3, 1 };
const uint8_t scale_threshold[8] = { 0, 0, 16, 32, 64, 128, 192, 255 };  // by level
marbles::Quantizer plaits_quantizer;
int plaits_scale = -1;

// ---- main -> audio hand-off (same scheme as sp1_synth) ----
sp1_marbles_params buf[2];
volatile uint32_t live;
volatile bool run_req;
volatile uint32_t start_count;           // main: incremented on every start
uint32_t start_seen;                     // audio
bool t2_was;                             // audio

// ---- an external clock: MIDI's (M5b; sp1_marbles_clock), audio thread ----
const float* ext_beats;                  // this block's position in beats, or null
bool ext_was;                            // the last block was external too
bool ext_resync;                         // align the ramp and the clock at the next block

// ---- audio -> main, for LEDs and the play-row clock ----
volatile uint8_t last_gates;
volatile float last_volts[4];
volatile uint32_t beats;

marbles::VoltageRange Range(int r) {
  return r == SP1_MRB_RANGE_NARROW ? marbles::VOLTAGE_RANGE_NARROW
       : r == SP1_MRB_RANGE_POSITIVE ? marbles::VOLTAGE_RANGE_POSITIVE
       : marbles::VOLTAGE_RANGE_FULL;
}

// ---- INTELLIGENT: the range, resolved PER CHANNEL (M4c) ----
// Marbles' voltage range is a per-GROUP setting, and XYGenerator::Process writes it into
// each channel's ScaleOffset itself, every block. A generated override of
// x_y_generator.cc (firmware/CMakeLists.txt) makes that switch ask us instead, once per
// channel per block, through sp1_mrb_channel_range() below. Everything intelligent
// happens here, in the audio thread, from the published parameters -- the override is
// three lines and holds no policy.
//
// ⚠️ Marbles' OutputChannel already owns its scale_offset_ per instance, so nothing about
// this fights the library; only Process's per-group assignment had to be intercepted.
uint8_t cur_range[4] = {
  marbles::VOLTAGE_RANGE_FULL, marbles::VOLTAGE_RANGE_FULL,
  marbles::VOLTAGE_RANGE_FULL, marbles::VOLTAGE_RANGE_FULL,
};

SP1_HOT marbles::VoltageRange IntelligentRange(uint8_t dest, uint8_t centre) {
  // The bits are SP1_ENGINE_TABLE[].centre: 0x1 = HARMONICS (F4), 0x2 = TIMBRE (F2),
  // 0x4 = MORPH (F3). A detent means the parameter's centre is its neutral point, which
  // is exactly what makes it bipolar.
  switch (dest) {
    case SP1_DEST_VOCT:
      return marbles::VOLTAGE_RANGE_NARROW;        // 0..2 V, two octaves
    case SP1_DEST_LEVEL:
    case SP1_DEST_TRIG:
      return marbles::VOLTAGE_RANGE_POSITIVE;      // 0..5 V, one-sided
    case SP1_DEST_FM:
      return marbles::VOLTAGE_RANGE_FULL;          // always signed
    case SP1_DEST_TIMBRE:
      return (centre & 0x2u) ? marbles::VOLTAGE_RANGE_FULL
                             : marbles::VOLTAGE_RANGE_POSITIVE;
    case SP1_DEST_MORPH:
      return (centre & 0x4u) ? marbles::VOLTAGE_RANGE_FULL
                             : marbles::VOLTAGE_RANGE_POSITIVE;
    case SP1_DEST_HARM:
      return (centre & 0x1u) ? marbles::VOLTAGE_RANGE_FULL
                             : marbles::VOLTAGE_RANGE_POSITIVE;
    default:
      return marbles::VOLTAGE_RANGE_FULL;          // nothing reads it
  }
}

marbles::ClockSource XClock(int c) {
  switch (c) {
    case SP1_MRB_XCLK_T1: return marbles::CLOCK_SOURCE_INTERNAL_T1;
    case SP1_MRB_XCLK_T2: return marbles::CLOCK_SOURCE_INTERNAL_T2;
    case SP1_MRB_XCLK_T3: return marbles::CLOCK_SOURCE_INTERNAL_T3;
    default:              return marbles::CLOCK_SOURCE_INTERNAL_T1_T2_T3;
  }
}

int Clamp(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

void Defaults(sp1_marbles_params* p) {
  // marbles/settings.cc defaults where they exist (model 0, x1 range, gate length
  // 128/256, y divider 128/256 -> 1/8, y range full); see sp1_marbles_ui.c for
  // the values the UI actually starts with.
  p->rate = 0.0f;
  p->t_range = 1;
  p->t_model = 0;
  p->t_bias = 0.5f;
  p->t_jitter = 0.0f;
  p->t_deja_vu = 0.0f;
  p->gate_length = 0.5f;
  p->gate_length_rand = 0.0f;
  p->length = 8;
  p->x_spread = 0.5f;
  p->x_bias = 0.5f;
  p->x_steps = 0.5f;
  p->x_deja_vu = 0.0f;
  p->x_diversity = 0;
  p->x_range = SP1_MRB_RANGE_FULL;
  p->x_scale = 0;
  for (int k = 0; k < 4; ++k) { p->dest[k] = 0; }
  p->engine_centre = 0u;
  p->x_clock = SP1_MRB_XCLK_EACH;
  p->y_spread = 0.5f;
  p->y_bias = 0.5f;
  p->y_steps = 0.0f;
  p->y_divider = 6;
  p->y_range = SP1_MRB_RANGE_FULL;
}

}  // namespace

namespace {
void InitGenerators() {
  // One Marbles sample per Plaits block: 4 kHz at 12 samples, 2 kHz at 24 (#32).
  const float sr = 48000.0f / static_cast<float>(SP1_SYNTH_BLOCK);
  t_generator.Init(&random_stream, sr);
  // X/Y run at 1 kHz (RenderXY1k). The rate only reaches XYGenerator's external-clock
  // extractor, which is never used; it is set to the truth anyway.
  xy_generator.Init(&random_stream, sr / kXYDecim);
  for (int k = 0; k < 4; ++k) {
    xy_prev_volts[k] = 0.0f;               // OutputChannel::Init starts at 0 V too
  }
  // XYGenerator::Init resets every quantizer to Marbles' empty default scale, so the
  // next render reloads whichever scale is selected.
  loaded_scale = -1;
}

// Audio thread. Put scale `s` in slot 0 of all four channels if it is not there
// already. Quantizer::Init is a table recompute -- no allocation, ~a thousand cycles
// for four channels -- and only runs when the selection changes.
SP1_HOT void LoadScaleIfNeeded(int s) {
  if (s < 0 || s >= SP1_MARBLES_SCALES) {
    s = 0;
  }
  if (s == loaded_scale) {
    return;
  }
  for (int ch = 0; ch < 4; ++ch) {
    xy_generator.LoadScale(ch, 0, *marbles::sp1_scales[s]);
  }
  loaded_scale = s;
}
}  // namespace

namespace {

// X1-X3 and Y at 1 kHz (issue #22; sp1_marbles.h), written into volt_buffer at 4 kHz
// exactly as XYGenerator::Process would have written it.
//
// Each output follows one of the three 4 kHz ramps (the X clock source decides which;
// Y divides X2's). A STEPPED output only changes when its ramp wraps, so the new value
// goes in on the very 4 kHz sample of that wrap -- known here because the whole block's
// ramps exist before Plaits reads any of it -- and the result is bit-identical to 4 kHz.
// A SMOOTH output is interpolated linearly across the four samples of its group.
SP1_HOT void RenderXY1k(marbles::ClockSource clk, const GroupSettings& x,
                const GroupSettings& y, bool* reset, const marbles::Ramps& ramps,
                uint32_t n) {
  const uint32_t groups = n / kXYDecim;
  const float* src[3] = { ramps.master, ramps.slave[0], ramps.slave[1] };

  // Every fourth ramp sample: the LAST of each group, so a wrap anywhere in the group has
  // happened by the time the 1 kHz generator sees it.
  marbles::Ramps d;
  d.master = &xy_ramp_buffer[0];
  d.external = &xy_ramp_buffer[kXYN];
  d.slave[0] = &xy_ramp_buffer[kXYN * 2];
  d.slave[1] = &xy_ramp_buffer[kXYN * 3];
  for (uint32_t g = 0; g < groups; ++g) {
    const uint32_t k = g * kXYDecim + kXYDecim - 1u;
    d.master[g] = ramps.master[k];
    d.slave[0][g] = ramps.slave[0][k];
    d.slave[1][g] = ramps.slave[1][k];
  }
  xy_generator.Process(clk, x, y, reset, no_clock, d, xy_volt_buffer, groups);

  // Which 4 kHz ramp drives each output (XYGenerator::Process's channel_ramp; Y divides
  // channel 1's): 0 master, 1 t1, 2 t3.
  int ch_src[4];
  switch (clk) {
    case marbles::CLOCK_SOURCE_INTERNAL_T1: ch_src[0] = ch_src[1] = ch_src[2] = 1; break;
    case marbles::CLOCK_SOURCE_INTERNAL_T2: ch_src[0] = ch_src[1] = ch_src[2] = 0; break;
    case marbles::CLOCK_SOURCE_INTERNAL_T3: ch_src[0] = ch_src[1] = ch_src[2] = 2; break;
    default: ch_src[0] = 1; ch_src[1] = 0; ch_src[2] = 2; break;
  }
  ch_src[3] = ch_src[1];

  // Stepped or smooth, per output, as OutputChannel decides it: STEPS at or above its
  // centre, after the X group's spread across channels (x_y_generator.cc).
  bool stepped[4];
  for (int ch = 0; ch < 3; ++ch) {
    float amount = 1.0f;
    if (x.control_mode == marbles::CONTROL_MODE_BUMP) {
      amount = ch == 1 ? 1.0f : -1.0f;
    } else if (x.control_mode == marbles::CONTROL_MODE_TILT) {
      amount = static_cast<float>(ch) - 1.0f;
    }
    stepped[ch] = 0.5f + (x.steps - 0.5f) * amount >= 0.5f;
  }
  stepped[3] = y.steps >= 0.5f;

  // Where each source ramp wraps inside each group (-1: it does not).
  int8_t wrap_at[3][kXYN];
  for (int s = 0; s < 3; ++s) {
    float prev = xy_prev_src[s];
    for (uint32_t g = 0; g < groups; ++g) {
      wrap_at[s][g] = -1;
      for (uint32_t i = 0; i < kXYDecim; ++i) {
        const float ph = src[s][g * kXYDecim + i];
        if (ph < prev && wrap_at[s][g] < 0) {
          wrap_at[s][g] = static_cast<int8_t>(i);
        }
        prev = ph;
      }
    }
    xy_prev_src[s] = prev;
  }

  for (int ch = 0; ch < 4; ++ch) {
    float prev = xy_prev_volts[ch];
    for (uint32_t g = 0; g < groups; ++g) {
      const float now = xy_volt_buffer[g * 4 + ch];
      float* o = &volt_buffer[(g * kXYDecim) * 4 + ch];
      if (stepped[ch]) {
        // A change with no wrap in the group (a reset, a range change) takes effect
        // at the start of the group.
        const int w = now != prev ? wrap_at[ch_src[ch]][g] : -1;
        for (uint32_t i = 0; i < kXYDecim; ++i) {
          o[i * 4] = static_cast<int>(i) < w ? prev : now;
        }
      } else {
        const float step = (now - prev) * (1.0f / kXYDecim);
        for (uint32_t i = 0; i < kXYDecim; ++i) {
          o[i * 4] = prev + step * static_cast<float>(i + 1);
        }
      }
      prev = now;
    }
    xy_prev_volts[ch] = prev;
  }
}

}  // namespace

// Called by the generated override of x_y_generator.cc, once per channel per block, from
// the AUDIO thread. `group_range` is what upstream would have used, and is returned
// unchanged for a channel index that cannot happen -- so a future Marbles with more
// channels degrades to upstream's behaviour rather than reading past the array.
//
// ⚠️ In namespace marbles, not extern "C": the override declares it at block scope inside
// XYGenerator::Process, and a block-scope extern declaration names an entity in the
// innermost enclosing namespace. `extern "C"` is not permitted at block scope at all.
namespace marbles {
// M5b: called by the generated override of t_generator.cc, from TGenerator::Process, in
// external-clock mode with no gate stream -- i.e. when the clock is MIDI's. Fills Marbles'
// external ramp from the beat position, at the ratio Marbles' own RATE code just chose:
// one ramp cycle per q/p beats. Returns true when the ramp starts afresh -- a reset, or the
// clock just became external -- so the override aligns Marbles to it rather than reading the
// jump from the last ramp as one enormous step of the clock.
SP1_HOT bool sp1_mrb_external_ramp(const Ratio& ratio, bool* reset, float* ramp, size_t size) {
  const float k = static_cast<float>(ratio.p) / static_cast<float>(ratio.q);
  for (size_t i = 0; i < size; ++i) {
    const float x = (ext_beats ? ext_beats[i] : 0.0f) * k;
    ramp[i] = x - floorf(x);
  }
  const bool fresh = ext_resync || *reset;
  ext_resync = false;
  return fresh;
}

SP1_HOT int sp1_mrb_channel_range(int channel, int group_range) {
  if (channel < 0 || channel > 3) {
    return group_range;
  }
  return static_cast<int>(cur_range[channel]);
}
}  // namespace marbles

extern "C" void sp1_marbles_init(void) {
  if (initialised) {
    return;
  }
  random_generator.Init(1);
  random_stream.Init(&random_generator);
  InitGenerators();
  for (uint32_t i = 0; i < kN; ++i) {
    no_clock[i] = stmlib::GATE_FLAG_LOW;
  }
  Defaults(&buf[0]);
  buf[1] = buf[0];
  live = 0;
  initialised = true;
}


extern "C" void sp1_marbles_seed(uint32_t seed) {
  // Main thread, clock stopped: the audio thread does not touch the generators then.
  // The generators are re-initialised AFTER seeding, because their DEJA VU loop
  // buffers are filled from the random stream at Init -- seeding alone would leave
  // the same first loop on every power-on.
  if (run_req) {
    return;
  }
  random_generator.Init(seed ? seed : 1u);
  InitGenerators();
}

extern "C" void sp1_marbles_set_params(const sp1_marbles_params* p) {
  const uint32_t next = live ^ 1u;
  buf[next] = *p;
  std::atomic_signal_fence(std::memory_order_seq_cst);
  live = next;
}

extern "C" void sp1_marbles_run(bool on) {
  if (on && !run_req) {
    start_count = start_count + 1u;
  }
  run_req = on;
}

extern "C" SP1_HOT void sp1_marbles_clock(const float* beats, int transport) {
  // Audio thread. The run state is PLAY's own, so either can change it (C6); a START is a
  // start_count like PLAY's, so the next render resets and puts the clock at the end of its
  // cycle -- and with the ramp still until beat 1, that cycle ends on beat 1's sample.
  ext_beats = beats;
  if (beats && !ext_was) {
    ext_resync = true;                   // a cable just went in
  }
  ext_was = beats != nullptr;
  switch (transport) {
    case SP1_MRB_TP_START:
      start_count = start_count + 1u;
      run_req = true;
      break;
    case SP1_MRB_TP_CONTINUE:
      if (!run_req) {
        run_req = true;
        ext_resync = true;
      }
      break;
    case SP1_MRB_TP_STOP:
      run_req = false;
      break;
    default:
      break;
  }
}

extern "C" bool sp1_marbles_running(void) {
  return run_req;
}

extern "C" SP1_HOT void sp1_marbles_render(uint32_t n) {
  if (n > kN) {
    n = kN;
  }

  // A re-seed requested while we were running: do it here, where nothing else is inside
  // the generators. Before the stopped-clock early return, so a rip while stopped still
  // takes effect at the next PLAY.
  const uint32_t rr = reseed_req;
  if (rr != reseed_seen && initialised) {
    reseed_seen = rr;
    random_generator.Init(reseed_val ? reseed_val : 1u);
    random_stream.Init(&random_generator);
    InitGenerators();
    t2_was = false;
    last_gates = 0u;
  }

  if (!run_req || !initialised) {
    for (uint32_t j = 0; j < n; ++j) {
      frame_gates[j] = 0u;
      frame_ramp[j] = 0.0f;
      frame_volts[j][0] = frame_volts[j][1] = 0.0f;
      frame_volts[j][2] = frame_volts[j][3] = 0.0f;
    }
    t2_was = false;
    last_gates = 0u;
    // PLAY restarts on a beat: its first sample is a tick, so it must read as a wrap.
    xy_prev_src[0] = xy_prev_src[1] = xy_prev_src[2] = 1.0f;
    return;
  }

  const uint32_t li = live;
  std::atomic_signal_fence(std::memory_order_seq_cst);
  const sp1_marbles_params c = buf[li];

  // PLAY: restart on a beat and from the start of the DEJA VU loop. Marbles' own
  // reset puts the sequences back to step 0 and makes the t1/t3 decision for the
  // NEXT tick (its Bernoulli gates are scheduled one tick ahead); the master clock is
  // then put at the end of its cycle, so that next tick is this very sample: t2 rises
  // and t1/t3 fire as the loop's first step decided.
  const uint32_t sc = start_count;
  bool reset = false;
  if (sc != start_seen) {
    start_seen = sc;
    reset = true;
    t_generator.set_master_phase(1.0f);
    t2_was = false;
  }

  // ---- t section (marbles.cc) ----
  t_generator.set_model(static_cast<marbles::TGeneratorModel>(Clamp(c.t_model, 0, 5)));
  t_generator.set_range(static_cast<marbles::TGeneratorRange>(Clamp(c.t_range, 0, 2)));
  // RATE can ask for a master clock faster than the 4 kHz this runs at (x4 range, RATE
  // near the top): TGenerator subtracts one cycle per sample at most, so its phase would
  // run away and then drain as a 4 kHz buzz. Cap the master clock at 1/8 cycle per sample
  // (500 Hz) -- far past anything musical.
  //
  // ⚠️ M4e removed the FFWD `ratchet` term that used to be added here. Multiplying the
  // rate mid-cycle is exactly what made the beat move when FFWD was pressed while the
  // clock ran, and left the phase displaced on release. FFWD is a phase-locked burst now
  // (sp1_synth.h) and never touches the clock. The cap stays: it costs one compare and it
  // is the only thing standing between a future rate source and a runaway phase.
  const float range_k = c.t_range == 0 ? 0.5f : (c.t_range == 2 ? 8.0f : 2.0f);
  const float max_st = 12.0f * log2f(48000.0f / static_cast<float>(SP1_SYNTH_BLOCK) *
                                     0.125f / range_k);
  float r = c.rate;
  if (!(r <= max_st)) {                // also catches NaN
    r = max_st;
  }
  t_generator.set_rate(r);
  t_generator.set_bias(c.t_bias);
  t_generator.set_jitter(c.t_jitter);
  t_generator.set_deja_vu(c.t_deja_vu);
  t_generator.set_length(Clamp(c.length, 1, 16));
  t_generator.set_pulse_width_mean(c.gate_length);
  t_generator.set_pulse_width_std(c.gate_length_rand);

  marbles::Ramps ramps;
  ramps.master = &ramp_buffer[0];
  ramps.external = &ramp_buffer[kN];
  ramps.slave[0] = &ramp_buffer[kN * 2];
  ramps.slave[1] = &ramp_buffer[kN * 3];

  // External (M5b): no gate stream -- the override fills the ramp from ext_beats instead.
  bool t_reset = reset;
  const bool ext = ext_beats != nullptr;
  t_generator.Process(ext, &t_reset, ext ? nullptr : no_clock, ramps, gate_buffer, n);

  // ---- X / Y sections (marbles.cc) ----
  // Resolve the four channels' ranges for this block, before Process asks for them.
  // A non-INTELLIGENT group simply gives all of its channels the same thing, which is
  // what upstream did.
  for (int k = 0; k < 4; ++k) {
    const int grp = (k < 3) ? c.x_range : c.y_range;
    cur_range[k] = (grp == SP1_MRB_RANGE_INTELLIGENT)
        ? static_cast<uint8_t>(IntelligentRange(c.dest[k], c.engine_centre))
        : static_cast<uint8_t>(Range(grp));
  }

  GroupSettings x;
  x.control_mode = static_cast<marbles::ControlMode>(Clamp(c.x_diversity, 0, 2));
  // The group value is now only a fallback if the override ever failed to apply.
  x.voltage_range = Range(c.x_range);
  x.register_mode = false;
  x.register_value = 0.0f;
  x.spread = c.x_spread;
  x.bias = c.x_bias;
  x.steps = c.x_steps;
  x.deja_vu = c.x_deja_vu;
  LoadScaleIfNeeded(c.x_scale);
  x.scale_index = 0;                     // the one slot we load (see loaded_scale)
  x.length = Clamp(c.length, 1, 16);
  x.ratio.p = 1;
  x.ratio.q = 1;

  GroupSettings y;
  y.control_mode = marbles::CONTROL_MODE_IDENTICAL;
  y.voltage_range = Range(c.y_range);
  y.register_mode = false;
  y.register_value = 0.0f;
  y.spread = c.y_spread;
  y.bias = c.y_bias;
  y.steps = c.y_steps;
  y.deja_vu = 0.0f;
  y.scale_index = x.scale_index;
  y.length = 1;
  y.ratio = kYDividerRatios[Clamp(c.y_divider, 0, 11)];

  bool xy_reset = reset;
  const marbles::ClockSource xclk = XClock(c.x_clock);
  if (n % kXYDecim != 0u) {
    // Not whole groups of four (never, at 20 per block): the plain 4 kHz path.
    xy_generator.Process(xclk, x, y, &xy_reset, no_clock, ramps, volt_buffer, n);
  } else {
    RenderXY1k(xclk, x, y, &xy_reset, ramps, n);
  }

  // ---- per Plaits block: t1 / t2 / t3 and X1..X3, Y (marbles.cc's DAC order) ----
  uint32_t b = beats;
  for (uint32_t j = 0; j < n; ++j) {
    const bool t1 = gate_buffer[2 * j];
    const bool t3 = gate_buffer[2 * j + 1];
    const bool t2 = ramps.master[j] < 0.5f;
    frame_gates[j] = static_cast<uint8_t>((t1 ? 1u : 0u) | (t2 ? 2u : 0u) | (t3 ? 4u : 0u));
    frame_ramp[j] = ramps.master[j];
    for (int k = 0; k < 4; ++k) {
      frame_volts[j][k] = volt_buffer[4 * j + k];
    }
    if (t2 && !t2_was) {
      ++b;
    }
    t2_was = t2;
  }
  beats = b;
  last_gates = frame_gates[n - 1];
  for (int k = 0; k < 4; ++k) {
    last_volts[k] = frame_volts[n - 1][k];
  }
}

extern "C" const uint8_t* sp1_marbles_gate_frames(void) {
  return frame_gates;
}

extern "C" const float* sp1_marbles_volt_frames(void) {
  return &frame_volts[0][0];
}

extern "C" const float* sp1_marbles_ramp_frames(void) {
  return frame_ramp;
}

extern "C" uint8_t sp1_marbles_gates(uint32_t j) {
  return j < kN ? frame_gates[j] : 0u;
}

extern "C" float sp1_marbles_ramp(uint32_t j) {
  return j < kN ? frame_ramp[j] : 0.0f;
}

extern "C" float sp1_marbles_volts(uint32_t j, int k) {
  return (j < kN && k >= 0 && k < 4) ? frame_volts[j][k] : 0.0f;
}

extern "C" uint8_t sp1_marbles_last_gates(void) {
  return last_gates;
}

extern "C" float sp1_marbles_last_volts(int k) {
  return (k >= 0 && k < 4) ? last_volts[k] : 0.0f;
}

extern "C" uint32_t sp1_marbles_beats(void) {
  return beats;
}

extern "C" float sp1_marbles_bpm(float rate, int t_range) {
  const float k = t_range == 0 ? 0.25f : (t_range == 2 ? 4.0f : 1.0f);
  return 120.0f * k * exp2f(rate * (1.0f / 12.0f));
}

extern "C" const char* sp1_marbles_model_name(int m) {
  static const char* const names[6] = {
    "bernoulli (coin toss)", "clusters", "drums",
    "independent bernoulli", "divider", "three states",
  };
  return (m >= 0 && m < 6) ? names[m] : "?";
}

// ---- the scales, lent to Plaits (M4b). MAIN THREAD ONLY. ----
extern "C" void sp1_marbles_reseed(uint32_t seed) {
  reseed_val = seed;
  reseed_req = reseed_req + 1u;          // main thread is the only writer
}

extern "C" void sp1_marbles_plaits_scale(int s) {
  if (s < 0 || s >= SP1_MARBLES_SCALES) {
    plaits_scale = -1;
    return;
  }
  if (s == plaits_scale) {
    return;
  }
  plaits_quantizer.Init(*marbles::sp1_scales[s]);
  plaits_scale = s;
}

extern "C" float sp1_marbles_plaits_quantize(float semitones) {
  if (plaits_scale < 0) {
    return semitones;
  }
  // Marbles' quantizer works in VOLTS at 1 V/octave, Plaits' note is in semitones.
  // `hysteresis` true, unlike the X/Y channels: this is a fader under a finger, and
  // without it a note parked on a boundary flickers between two degrees.
  const float amount =
      static_cast<float>(scale_level[plaits_scale]) / 7.0f;   // -> that level exactly
  return plaits_quantizer.Process(semitones * (1.0f / 12.0f), amount, true) * 12.0f;
}

extern "C" int sp1_marbles_plaits_degrees(float* semitones, int max) {
  if (plaits_scale < 0 || max <= 0) {
    return 0;
  }
  const marbles::Scale& sc = *marbles::sp1_scales[plaits_scale];
  const uint8_t t = scale_threshold[scale_level[plaits_scale]];
  int n = 0;
  for (int i = 0; i < sc.num_degrees && n < max; ++i) {
    if (sc.degree[i].weight >= t) {
      semitones[n++] = sc.degree[i].voltage * 12.0f;
    }
  }
  return n;
}

extern "C" const char* sp1_marbles_scale_name(int s) {
  static const char* const names[SP1_MARBLES_SCALES] = {
    "major", "minor", "pentatonic", "pelog", "bhairav", "shri", "free slot",
  };
  return (s >= 0 && s < SP1_MARBLES_SCALES) ? names[s] : "?";
}

// The generators take the sample rate as a parameter; only the external-clock code
// (ramp_extractor.cc, never called here) hard-codes 32 kHz. See sp1_marbles.h, "the 4 kHz rule".
static_assert(SP1_MARBLES_MAX_FRAMES * 12u == 240u,
              "one Marbles sample per Plaits block of a 240-frame audio block, at the "
              "smallest Plaits block (12; at 24 a 240-frame block uses half of them)");
static_assert(SP1_MARBLES_MAX_FRAMES % SP1_SYNTH_BLOCKS_PER_MS == 0u,
              "whole 1 kHz X/Y groups");
