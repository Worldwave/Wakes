// wakes-sp1 (issue #22): a full replacement of Plaits' voice.h (Emilie Gillet, MIT,
// notice below), shadowing third_party/eurorack/plaits/dsp/voice.h through the include
// path. It carries M4a's HARMONICS attenuverter unchanged, and replaces the two output
// channels' post-processing with ONE low-pass gate on a mix of OUT and AUX:
//
//   The SP-1 plays OUT, AUX, OUT+AUX or OUTxAUX through a single output -- never OUT and
//   AUX to separate outputs -- so Plaits' second LPG only ever fed a mix. The LPG is a
//   linear filter and VCA with the same envelope on both channels, so running it once on
//   the mix gives the same result as mixing two LPG outputs (Adara). LEVEL and TRIG
//   drive it exactly as they drove OUT's.
//
//   OUT, AUX: the same as upstream to within float rounding (at most 1-2 LSB).
//   OUT+AUX:  the same, except that the sum is no longer clipped per channel before it is
//             added: the voice hands the SP-1 an unclipped value and the one 16-bit
//             saturation happens after the drive and the OUT+AUX limiter.
//   OUTxAUX:  the product is taken BEFORE the gate, like the sum (Adara: the LPG is the
//             subtractive stage at the end of the chain; nothing here means to multiply
//             two already-gated signals). This one is a deliberate change of sound: upstream
//             gated each channel and M3f multiplied the results, so the envelope reached
//             the product twice and each side was low-pass filtered before multiplying.
//             Level at full envelope is unchanged (product x 2, as M3f's ring modulator).
//
//   Every mode costs the same: one gate, and the same mixing arithmetic on every sample.
//
// Plaits' per-channel limiter (engines with a negative output gain) is kept per channel,
// before the mix, with its per-sample divide replaced by a Newton reciprocal (see
// Sp1Limiter below; < 1e-5 relative, below one 16-bit step).

// Copyright 2016 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Main synthesis voice.

#ifndef PLAITS_DSP_VOICE_H_
#define PLAITS_DSP_VOICE_H_

#include "stmlib/stmlib.h"

#include "stmlib/dsp/filter.h"
#include "stmlib/dsp/limiter.h"
#include "stmlib/utils/buffer_allocator.h"

#include "plaits/dsp/engine/additive_engine.h"
#include "plaits/dsp/engine/bass_drum_engine.h"
#include "plaits/dsp/engine/chord_engine.h"
#include "plaits/dsp/engine/engine.h"
#include "plaits/dsp/engine/fm_engine.h"
#include "plaits/dsp/engine/grain_engine.h"
#include "plaits/dsp/engine/hi_hat_engine.h"
#include "plaits/dsp/engine/modal_engine.h"
#include "plaits/dsp/engine/noise_engine.h"
#include "plaits/dsp/engine/particle_engine.h"
#include "plaits/dsp/engine/snare_drum_engine.h"
#include "plaits/dsp/engine/speech_engine.h"
#include "plaits/dsp/engine/string_engine.h"
#include "plaits/dsp/engine/swarm_engine.h"
#include "plaits/dsp/engine/virtual_analog_engine.h"
#include "plaits/dsp/engine/waveshaping_engine.h"
#include "plaits/dsp/engine/wavetable_engine.h"

#include "plaits/dsp/engine2/chiptune_engine.h"
#include "plaits/dsp/engine2/phase_distortion_engine.h"
#include "plaits/dsp/engine2/six_op_engine.h"
#include "plaits/dsp/engine2/string_machine_engine.h"
#include "plaits/dsp/engine2/virtual_analog_vcf_engine.h"
#include "plaits/dsp/engine2/wave_terrain_engine.h"

#include "plaits/dsp/envelope.h"

#include "plaits/dsp/fx/low_pass_gate.h"

namespace plaits {

const int kMaxEngines = 24;
const int kMaxTriggerDelay = 8;
const int kTriggerDelay = 5;

class ChannelPostProcessor {
 public:
  ChannelPostProcessor() { }
  ~ChannelPostProcessor() { }
  
  void Init() {
    lpg_.Init();
    Reset();
  }
  
  void Reset() {
    limiter_.Init();
  }
  
  void Process(
      float gain,
      bool bypass_lpg,
      float low_pass_gate_gain,
      float low_pass_gate_frequency,
      float low_pass_gate_hf_bleed,
      float* in,
      short* out,
      size_t size,
      size_t stride) {
    if (gain < 0.0f) {
      limiter_.Process(-gain, in, size);
    }
    const float post_gain = (gain < 0.0f ? 1.0f : gain) * -32767.0f;
    if (!bypass_lpg) {
      lpg_.Process(
          post_gain * low_pass_gate_gain,
          low_pass_gate_frequency,
          low_pass_gate_hf_bleed,
          in,
          out,
          size,
          stride);
    } else {
      while (size--) {
        *out = stmlib::Clip16(1 + static_cast<int32_t>(*in++ * post_gain));
        out += stride;
      }
    }
  }
  
 private:
  stmlib::Limiter limiter_;
  LowPassGate lpg_;
  
  DISALLOW_COPY_AND_ASSIGN(ChannelPostProcessor);
};

struct Patch {
  float note;
  float harmonics;
  float timbre;
  float morph;
  float frequency_modulation_amount;
  float timbre_modulation_amount;
  float morph_modulation_amount;
  // wakes-sp1 (M4a): HARMONICS gets an attenuverter of its own. Upstream has none.
  float harmonics_modulation_amount;

  int engine;
  float decay;
  float lpg_colour;

  // Proof to sp1_synth.cc's static_assert that this override was compiled.
  static const bool kSp1HarmonicsAttenuverter = true;
};

struct Modulations {
  float engine;
  float note;
  float frequency;
  float harmonics;
  float timbre;
  float morph;
  float trigger;
  float level;

  bool harmonics_patched;   // wakes-sp1 (M4a)
  bool frequency_patched;
  bool timbre_patched;
  bool morph_patched;
  bool trigger_patched;
  bool level_patched;
};

// char (*__foo)[sizeof(HiHatEngine)] = 1;

// wakes-sp1 (issue #22): stmlib::Limiter with the per-sample `1.0f / peak_` replaced by
// a reciprocal carried from sample to sample. peak_ moves by at most 5 % of the gap per
// sample (the attack), so two Newton steps from the previous reciprocal land within
// ~1e-6 of the true value; a real divide is taken at onset or after any jump the steps
// could not close. Same attack, release, 0.8 output scale and state as upstream.
class Sp1Limiter {
 public:
  void Init() {
    peak_ = 0.5f;
    recip_ = 1.0f;
    recip_valid_ = false;
  }

  void Process(float pre_gain, float* in_out, size_t size) {
    while (size--) {
      float s = *in_out * pre_gain;
      SLOPE(peak_, fabsf(s), 0.05f, 0.00002f);
      float gain = 1.0f;
      if (peak_ > 1.0f) {
        gain = Reciprocal(peak_);
      } else {
        recip_valid_ = false;
      }
      *in_out++ = s * gain * 0.8f;
    }
  }

  static float Newton(float x, float r) {
    r = r * (2.0f - x * r);
    return r * (2.0f - x * r);
  }

 private:
  float Reciprocal(float x) {
    const float e = 1.0f - x * recip_;
    if (!recip_valid_ || e > 0.25f || e < -0.25f) {
      recip_ = 1.0f / x;
      recip_valid_ = true;
    } else {
      recip_ = Newton(x, recip_);
    }
    return recip_;
  }

  float peak_;
  float recip_;
  bool recip_valid_;
};


class Voice {
 public:
  Voice() { }
  ~Voice() { }
  
  // wakes-sp1 (issue #22): how much of OUT, AUX and OUTxAUX goes into the one gated
  // output. Render() interpolates from `from` to `to` across its block, which is how
  // the output-mode cross-fade is drawn.
  struct OutputMix {
    float out;
    float aux;
    float ring;
  };
  static const bool kSp1SingleLpg = true;   // checked in sp1_synth.cc
  // Render()'s output: what upstream fed into Clip16(1 + (int32_t) x) -- 16-bit scale,
  // NOT yet clipped, so a sum or a drive can go past full scale before it is limited.
  
  void Init(stmlib::BufferAllocator* allocator);
  void ReloadUserData() {
    reload_user_data_ = true;
  }
  void Render(
      const Patch& patch,
      const Modulations& modulations,
      const OutputMix& from,
      const OutputMix& to,
      float* out,
      size_t size);
  inline int active_engine() const { return previous_engine_index_; }
    
 private:
  void ComputeDecayParameters(const Patch& settings);
  
  inline float ApplyModulations(
      float base_value,
      float modulation_amount,
      bool use_external_modulation,
      float external_modulation,
      bool use_internal_envelope,
      float envelope,
      float default_internal_modulation,
      float minimum_value,
      float maximum_value) {
    float value = base_value;
    modulation_amount *= std::max(fabsf(modulation_amount) - 0.05f, 0.05f);
    modulation_amount *= 1.05f;
    
    float modulation = use_external_modulation
        ? external_modulation
        : (use_internal_envelope ? envelope : default_internal_modulation);
    value += modulation_amount * modulation;
    CONSTRAIN(value, minimum_value, maximum_value);
    return value;
  }

  VirtualAnalogEngine virtual_analog_engine_;
  WaveshapingEngine waveshaping_engine_;
  FMEngine fm_engine_;
  GrainEngine grain_engine_;
  AdditiveEngine additive_engine_;
  WavetableEngine wavetable_engine_;
  ChordEngine chord_engine_;
  SpeechEngine speech_engine_;

  SwarmEngine swarm_engine_;
  NoiseEngine noise_engine_;
  ParticleEngine particle_engine_;
  StringEngine string_engine_;
  ModalEngine modal_engine_;
  BassDrumEngine bass_drum_engine_;
  SnareDrumEngine snare_drum_engine_;
  HiHatEngine hi_hat_engine_;
  
  VirtualAnalogVCFEngine virtual_analog_vcf_engine_;
  PhaseDistortionEngine phase_distortion_engine_;
  SixOpEngine six_op_engine_;
  WaveTerrainEngine wave_terrain_engine_;
  StringMachineEngine string_machine_engine_;
  ChiptuneEngine chiptune_engine_;

  stmlib::HysteresisQuantizer2 engine_quantizer_;
  
  bool reload_user_data_;
  int previous_engine_index_;
  float engine_cv_;
  
  float previous_note_;
  bool trigger_state_;
  
  DecayEnvelope decay_envelope_;
  LPGEnvelope lpg_envelope_;
  
  float trigger_delay_line_[kMaxTriggerDelay];
  DelayLine<float, kMaxTriggerDelay> trigger_delay_;
  
  // wakes-sp1 (issue #22): ONE gate for the mix of OUT, AUX and their product; the
  // per-channel limiters kept.
  Sp1Limiter out_limiter_;
  Sp1Limiter aux_limiter_;
  LowPassGate lpg_;
  float mix_buffer_[kMaxBlockSize];
  
  EngineRegistry<kMaxEngines> engines_;
  
  float out_buffer_[kMaxBlockSize];
  float aux_buffer_[kMaxBlockSize];
  
  DISALLOW_COPY_AND_ASSIGN(Voice);
};

}  // namespace plaits

#endif  // PLAITS_DSP_VOICE_H_
