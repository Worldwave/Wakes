// wakes-sp1: Plaits' chord_engine.h (Emilie Gillet, MIT, notice below), shadowing
// third_party/eurorack/plaits/dsp/engine/chord_engine.h. Two changes, both issue #22:
// the bounded organ/wavetable crossfade (Sp1Crossfade, below; was a generated override)
// and the shared waveform position:
//
// Issue #22: the five voices of Chords all step the SAME waveform position (MORPH) through
// the same interpolation every sample. ChordEngine now steps it once per sample and
// hands it to every active voice (WavetableOscillator::RenderShared). A voice returning
// from silence still holds the position it stopped at; for that one 12-sample block it
// renders exactly as upstream (its own glide from there), then joins the shared position.
// That block costs a few dozen cycles more; every other block costs the same whatever
// MORPH is doing. Host A/B: within a few LSB of upstream, pluck, drone and extreme
// modulation alike.

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
// Chords: wavetable and divide-down organ/string machine.

#ifndef PLAITS_DSP_ENGINE_CHORD_ENGINE_H_
#define PLAITS_DSP_ENGINE_CHORD_ENGINE_H_

#include "plaits/dsp/chords/chord_bank.h"
#include "plaits/dsp/engine/engine.h"
#include "plaits/dsp/oscillator/string_synth_oscillator.h"
#include "plaits/dsp/oscillator/wavetable_oscillator.h"

namespace plaits {

const int kChordNumHarmonics = 3;

class ChordEngine : public Engine {
 public:
  ChordEngine() { }
  ~ChordEngine() { }
  
  virtual void Init(stmlib::BufferAllocator* allocator);
  virtual void Reset();
  virtual void LoadUserData(const uint8_t* user_data) { }
  virtual void Render(const EngineParameters& parameters,
      float* out,
      float* aux,
      size_t size,
      bool* already_enveloped);

 private:
  void ComputeRegistration(float registration, float* amplitudes);
  int ComputeChordInversion(
      float inversion,
      float* ratios,
      float* amplitudes);
  
  StringSynthOscillator divide_down_voice_[kChordNumVoices];
  WavetableOscillator<128, 15> wavetable_voice_[kChordNumVoices];
  ChordBank chords_;
  
  float morph_lp_;
  float timbre_lp_;

  // wakes-sp1 (issue #22): the one waveform position all five wavetable voices share,
  // and its per-sample waves and blend for the current block.
 public:
  static const bool kSp1SharedWaveform = true;     // checked in sp1_synth.cc
 private:
  float sp1_waveform_ = 0.0f;
  const int16_t* sp1_wave_a_[kMaxBlockSize];
  const int16_t* sp1_wave_b_[kMaxBlockSize];
  float sp1_wave_fractional_[kMaxBlockSize];

  // wakes-sp1 (issue #22): one note's organ -> wavetable amount, 0..1. The slice is a
  // quarter of upstream's width; resting inside it resolves to the nearer side after
  // kSp1RestBlocks; every move is slew-limited; one note fades at a time.
 public:
  static const bool kSp1BoundedCrossfade = true;   // checked in sp1_synth.cc
 private:
  static const int kSp1RestBlocks = 200;           // 50 ms of 12-sample blocks
  static constexpr float kSp1Slew = 1.0f / 200.0f; // 0 -> 1 in 50 ms
  static constexpr float kSp1Moved = 0.25f;        // of the slice: fader noise is ~0.12
  float sp1_xf_[kChordNumVoices] = { };
  float sp1_ref_[kChordNumVoices] = { };
  int sp1_rest_[kChordNumVoices] = { };
  bool sp1_seen_[kChordNumVoices] = { };

  float Sp1Crossfade(int note, float morph, float fade_point) {
    // Upstream: 50 x (morph - fade_point), a 0.02 slice. Same midpoint, 0.005 wide.
    float p = 200.0f * (morph - fade_point - 0.0075f);
    CONSTRAIN(p, 0.0f, 1.0f);
    float& x = sp1_xf_[note];
    if (!sp1_seen_[note]) {
      sp1_seen_[note] = true;
      x = p;
      sp1_ref_[note] = p;
    }
    if (p - sp1_ref_[note] > kSp1Moved || sp1_ref_[note] - p > kSp1Moved) {
      sp1_ref_[note] = p;
      sp1_rest_[note] = 0;
    } else if (sp1_rest_[note] < kSp1RestBlocks) {
      ++sp1_rest_[note];
    }
    float target = p;
    if (p > 0.0f && p < 1.0f && sp1_rest_[note] >= kSp1RestBlocks) {
      target = sp1_ref_[note] >= 0.5f ? 1.0f : 0.0f;
    }
    if (x != target) {
      bool other_mid = false;
      for (int i = 0; i < kChordNumVoices; ++i) {
        if (i != note && sp1_xf_[i] > 0.0f && sp1_xf_[i] < 1.0f) {
          other_mid = true;
        }
      }
      if (!(other_mid && (x <= 0.0f || x >= 1.0f))) {
        float d = target - x;
        CONSTRAIN(d, -kSp1Slew, kSp1Slew);
        x += d;
      }
    }
    return x;
  }
  
  DISALLOW_COPY_AND_ASSIGN(ChordEngine);
};

}  // namespace plaits

#endif  // PLAITS_DSP_ENGINE_CHORD_ENGINE_H_