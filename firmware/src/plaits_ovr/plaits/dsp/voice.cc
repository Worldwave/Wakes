// wakes-sp1 (issue #22): a full replacement of Plaits' voice.cc (Emilie Gillet, MIT,
// notice below), shadowing third_party/eurorack/plaits/dsp/voice.cc through the include
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
//   OUTxAUX:  NOT merged. A product of two gated signals is not a gated product (each
//             side is low-pass filtered before multiplying), and one gate on the product
//             sounds different on plucked notes. OUTxAUX keeps two gates, exactly as
//             upstream; only the other three modes save the second one.
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

#include "plaits/dsp/voice.h"
#include "plaits/user_data.h"

namespace plaits {

using namespace std;
using namespace stmlib;

void Voice::Init(BufferAllocator* allocator) {
  engines_.Init();

  engines_.RegisterInstance(&virtual_analog_vcf_engine_, false, 1.0f, 1.0f);
  engines_.RegisterInstance(&phase_distortion_engine_, false, 0.7f, 0.7f);
  engines_.RegisterInstance(&six_op_engine_, true, 1.0f, 1.0f);
  engines_.RegisterInstance(&six_op_engine_, true, 1.0f, 1.0f);
  engines_.RegisterInstance(&six_op_engine_, true, 1.0f, 1.0f);
  engines_.RegisterInstance(&wave_terrain_engine_, false, 0.7f, 0.7f);
  engines_.RegisterInstance(&string_machine_engine_, false, 0.8f, 0.8f);
  engines_.RegisterInstance(&chiptune_engine_, false, 0.5f, 0.5f);
  
  engines_.RegisterInstance(&virtual_analog_engine_, false, 0.8f, 0.8f);
  engines_.RegisterInstance(&waveshaping_engine_, false, 0.7f, 0.6f);
  engines_.RegisterInstance(&fm_engine_, false, 0.6f, 0.6f);
  engines_.RegisterInstance(&grain_engine_, false, 0.7f, 0.6f);
  engines_.RegisterInstance(&additive_engine_, false, 0.8f, 0.8f);
  engines_.RegisterInstance(&wavetable_engine_, false, 0.6f, 0.6f);
  engines_.RegisterInstance(&chord_engine_, false, 0.8f, 0.8f);
  engines_.RegisterInstance(&speech_engine_, false, -0.7f, 0.8f);

  engines_.RegisterInstance(&swarm_engine_, false, -3.0f, 1.0f);
  engines_.RegisterInstance(&noise_engine_, false, -1.0f, -1.0f);
  engines_.RegisterInstance(&particle_engine_, false, -2.0f, 1.0f);
  engines_.RegisterInstance(&string_engine_, true, -1.0f, 0.8f);
  engines_.RegisterInstance(&modal_engine_, true, -1.0f, 0.8f);
  engines_.RegisterInstance(&bass_drum_engine_, true, 0.8f, 0.8f);
  engines_.RegisterInstance(&snare_drum_engine_, true, 0.8f, 0.8f);
  engines_.RegisterInstance(&hi_hat_engine_, true, 0.8f, 0.8f);
  
  for (int i = 0; i < engines_.size(); ++i) {
    // All engines will share the same RAM space.
    allocator->Free();
    engines_.get(i)->Init(allocator);
  }
  
  engine_quantizer_.Init(engines_.size(), 0.05f, true);
  previous_engine_index_ = -1;
  reload_user_data_ = false;
  engine_cv_ = 0.0f;
  
  out_limiter_.Init();
  aux_limiter_.Init();
  lpg_.Init();
  ring_lpg_out_.Init();
  ring_lpg_aux_.Init();
  mix_gate_live_ = true;
  ring_gates_live_ = true;

  decay_envelope_.Init();
  lpg_envelope_.Init();
  
  trigger_state_ = false;
  previous_note_ = 0.0f;
  
  trigger_delay_.Init(trigger_delay_line_);
}

void Voice::Render(
    const Patch& patch,
    const Modulations& modulations,
    const OutputMix& from,
    const OutputMix& to,
    float* out,
    size_t size) {
  // Trigger, LPG, internal envelope.
      
  // Delay trigger by 1ms to deal with sequencers or MIDI interfaces whose
  // CV out lags behind the GATE out.
  trigger_delay_.Write(modulations.trigger);
  float trigger_value = trigger_delay_.Read(kTriggerDelay);
  
  bool previous_trigger_state = trigger_state_;
  if (!previous_trigger_state) {
    if (trigger_value > 0.3f) {
      trigger_state_ = true;
      if (!modulations.level_patched) {
        lpg_envelope_.Trigger();
      }
      decay_envelope_.Trigger();
      engine_cv_ = modulations.engine;
    }
  } else {
    if (trigger_value < 0.1f) {
      trigger_state_ = false;
    }
  }
  if (!modulations.trigger_patched) {
    engine_cv_ = modulations.engine;
  }

  // Engine selection.
  int engine_index = engine_quantizer_.Process(
      patch.engine,
      engine_cv_);
  
  Engine* e = engines_.get(engine_index);
  
  if (engine_index != previous_engine_index_ || reload_user_data_) {
    UserData user_data;
    const uint8_t* data = user_data.ptr(engine_index);
    if (!data && engine_index >= 2 && engine_index <= 4) {
      data = fm_patches_table[engine_index - 2];
    }
    e->LoadUserData(data);
    e->Reset();

    out_limiter_.Init();   // upstream resets OUT's limiter only
    previous_engine_index_ = engine_index;
    reload_user_data_ = false;
  }
  EngineParameters p;

  bool rising_edge = trigger_state_ && !previous_trigger_state;
  float note = (modulations.note + previous_note_) * 0.5f;
  previous_note_ = modulations.note;
  const PostProcessingSettings& pp_s = e->post_processing_settings;

  if (modulations.trigger_patched) {
    p.trigger = (rising_edge ? TRIGGER_RISING_EDGE : TRIGGER_LOW) | \
      (trigger_state_ ? TRIGGER_HIGH : TRIGGER_LOW);
  } else {
    p.trigger = TRIGGER_UNPATCHED;
  }
  
  const float short_decay = (200.0f * kBlockSize) / kSampleRate *
      SemitonesToRatio(-96.0f * patch.decay);

  decay_envelope_.Process(short_decay * 2.0f);

  float compressed_level = 1.3f * modulations.level / (0.3f + fabsf(modulations.level));
  CONSTRAIN(compressed_level, 0.0f, 1.0f);
  p.accent = modulations.level_patched ? compressed_level : 0.8f;

  bool use_internal_envelope = modulations.trigger_patched;

  // Actual synthesis parameters.
  
  // wakes-sp1 (M4a): HARMONICS through the same attenuverter + internal-envelope
  // path as TIMBRE and MORPH. Upstream is: p.harmonics = patch.harmonics +
  // modulations.harmonics, constrained to 0..1.
  p.harmonics = ApplyModulations(
      patch.harmonics,
      patch.harmonics_modulation_amount,
      modulations.harmonics_patched,
      modulations.harmonics,
      use_internal_envelope,
      decay_envelope_.value(),
      0.0f,
      0.0f,
      1.0f);

  float internal_envelope_amplitude = 1.0f;
  float internal_envelope_amplitude_timbre = 1.0f;
  if (engine_index == 15) {
    internal_envelope_amplitude = 2.0f - p.harmonics * 6.0f;
    CONSTRAIN(internal_envelope_amplitude, 0.0f, 1.0f);
    speech_engine_.set_prosody_amount(
        !modulations.trigger_patched || modulations.frequency_patched ?
            0.0f : patch.frequency_modulation_amount);
    speech_engine_.set_speed( 
        !modulations.trigger_patched || modulations.morph_patched ?
            0.0f : patch.morph_modulation_amount);
  } else if (engine_index == 7) {
    if (modulations.trigger_patched && !modulations.timbre_patched) {
      // Disable internal envelope on TIMBRE, and enable the envelope generator
      // built into the chiptune engine.
      internal_envelope_amplitude_timbre = 0.0f;
      chiptune_engine_.set_envelope_shape(patch.timbre_modulation_amount);
    } else {
      chiptune_engine_.set_envelope_shape(ChiptuneEngine::NO_ENVELOPE);
    }
  }
  
  p.note = ApplyModulations(
      patch.note + note,
      patch.frequency_modulation_amount,
      modulations.frequency_patched,
      modulations.frequency,
      use_internal_envelope,
      internal_envelope_amplitude * \
          decay_envelope_.value() * decay_envelope_.value() * 48.0f,
      1.0f,
      -119.0f,
      120.0f);

  p.timbre = ApplyModulations(
      patch.timbre,
      patch.timbre_modulation_amount,
      modulations.timbre_patched,
      modulations.timbre,
      use_internal_envelope,
      internal_envelope_amplitude_timbre * decay_envelope_.value(),
      0.0f,
      0.0f,
      1.0f);

  p.morph = ApplyModulations(
      patch.morph,
      patch.morph_modulation_amount,
      modulations.morph_patched,
      modulations.morph,
      use_internal_envelope,
      internal_envelope_amplitude * decay_envelope_.value(),
      0.0f,
      0.0f,
      1.0f);

  bool already_enveloped = pp_s.already_enveloped;
  e->Render(p, out_buffer_, aux_buffer_, size, &already_enveloped);
  
  bool lpg_bypass = already_enveloped || \
      (!modulations.level_patched && !modulations.trigger_patched);
  
  // Compute LPG parameters.
  if (!lpg_bypass) {
    const float hf = patch.lpg_colour;
    const float decay_tail = (20.0f * kBlockSize) / kSampleRate *
        SemitonesToRatio(-72.0f * patch.decay + 12.0f * hf) - short_decay;
    
    if (modulations.level_patched) {
      lpg_envelope_.ProcessLP(compressed_level, short_decay, decay_tail, hf);
    } else {
      const float attack = NoteToFrequency(p.note) * float(kBlockSize) * 2.0f;
      lpg_envelope_.ProcessPing(attack, short_decay, decay_tail, hf);
    }
  } else {
    lpg_envelope_.Init();
  }
  
  // ---- wakes-sp1 (issue #22): one gate on the mix ----
  // Each channel's own limiter, exactly as ChannelPostProcessor::Process ran it, and ALWAYS
  // run: a limiter left idle while its channel is not heard would come back with a stale
  // peak and take ~1 s to release it.
  float out_gain = pp_s.out_gain;
  float aux_gain = pp_s.aux_gain;
  if (out_gain < 0.0f) {
    out_limiter_.Process(-out_gain, out_buffer_, size);
  }
  if (aux_gain < 0.0f) {
    aux_limiter_.Process(-aux_gain, aux_buffer_, size);
  }
  out_gain = out_gain < 0.0f ? 1.0f : out_gain;
  aux_gain = aux_gain < 0.0f ? 1.0f : aux_gain;

  // Upstream folded each channel's gain and the int16 scale into the gate's gain; here
  // the channel gains are applied before the mix and the -32767 once, by the gate.
  const float gate_gain = -32767.0f * lpg_envelope_.gain();
  const float inv = 1.0f / static_cast<float>(size);

  // ---- OUT, AUX, OUT+AUX: their weighted sum through ONE gate ----
  const bool mix_on = from.out != 0.0f || to.out != 0.0f ||
                      from.aux != 0.0f || to.aux != 0.0f;
  if (mix_on) {
    if (!mix_gate_live_) {
      lpg_.Init();
      mix_gate_live_ = true;
    }
    const float d_out = (to.out - from.out) * inv;
    const float d_aux = (to.aux - from.aux) * inv;
    float wo = from.out, wa = from.aux;
    for (size_t i = 0; i < size; ++i) {
      wo += d_out;
      wa += d_aux;
      mix_buffer_[i] = wo * out_buffer_[i] * out_gain + wa * aux_buffer_[i] * aux_gain;
    }
    if (!lpg_bypass) {
      lpg_.Process(
          gate_gain,
          lpg_envelope_.frequency(),
          lpg_envelope_.hf_bleed(),
          mix_buffer_,
          size);
    } else {
      for (size_t i = 0; i < size; ++i) {
        mix_buffer_[i] *= -32767.0f;
      }
    }
  } else {
    mix_gate_live_ = false;
    std::fill(&mix_buffer_[0], &mix_buffer_[size], 0.0f);
  }

  // ---- OUTxAUX: upstream's two gates, then the product (as M3f's ring modulator) ----
  const bool ring_on = from.ring != 0.0f || to.ring != 0.0f;
  if (ring_on) {
    if (!ring_gates_live_) {
      ring_lpg_out_.Init();
      ring_lpg_aux_.Init();
      ring_gates_live_ = true;
    }
    for (size_t i = 0; i < size; ++i) {
      out_buffer_[i] *= out_gain;
      aux_buffer_[i] *= aux_gain;
    }
    if (!lpg_bypass) {
      ring_lpg_out_.Process(gate_gain, lpg_envelope_.frequency(),
                            lpg_envelope_.hf_bleed(), out_buffer_, size);
      ring_lpg_aux_.Process(gate_gain, lpg_envelope_.frequency(),
                            lpg_envelope_.hf_bleed(), aux_buffer_, size);
    } else {
      for (size_t i = 0; i < size; ++i) {
        out_buffer_[i] *= -32767.0f;
        aux_buffer_[i] *= -32767.0f;
      }
    }
    const float d_ring = (to.ring - from.ring) * inv;
    float wr = from.ring;
    for (size_t i = 0; i < size; ++i) {
      wr += d_ring;
      // Upstream's two int16 channels, and M3f's (a x b) >> 14, saturated.
      const int32_t o16 = stmlib::Clip16(1 + static_cast<int32_t>(out_buffer_[i]));
      const int32_t a16 = stmlib::Clip16(1 + static_cast<int32_t>(aux_buffer_[i]));
      const int32_t ring = stmlib::Clip16((o16 * a16) >> 14);
      mix_buffer_[i] += wr * static_cast<float>(ring);
    }
  } else {
    ring_gates_live_ = false;
  }

  std::copy(&mix_buffer_[0], &mix_buffer_[size], out);
}

}  // namespace plaits
