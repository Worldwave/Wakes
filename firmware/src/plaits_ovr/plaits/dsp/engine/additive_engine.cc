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
// wakes-sp1 (M3f): a CPU-reduced copy of Plaits' additive engine. See the header.
//
// Upstream: third_party/eurorack/plaits/dsp/engine/additive_engine.cc (Emilie
// Gillet, MIT, above). UpdateAmplitudes() and the spectrum shaping are upstream's,
// line for line. Changes, all in how the partials are rendered:
//
//  1. ONE pass per output instead of three HarmonicOscillator objects. All three
//     upstream oscillators run at the same f0 from the same initial phase, so they
//     always share one phase; we keep one phase and one sine lookup per sample.
//
//  2. Clenshaw summation. Upstream generates each partial with the Chebyshev
//     recurrence T(k+1) = 2x T(k) - T(k-1), x = sin(phase), and accumulates
//     a(k) T(k): two multiply-adds per partial per sample. Clenshaw's algorithm
//     evaluates the same sum with one multiply-add and one add per partial:
//         b(k) = a(k) + 2x b(k+1) - b(k+2),   k = N..1
//         sum  = y(1) b(1) - y(0) b(2)      (y = the recurrence's two seeds)
//     Identical output up to float rounding (checked on the host against
//     upstream).
//
//  3. Partial amplitudes are ramped toward the current spectrum every kRampStride
//     samples instead of upstream's every sample.
//
//     ⚠️ M3f made them constant for the whole block, on the reasoning that the
//     one-pole in UpdateAmplitudes has a ~250 ms time constant so a block's ramp
//     is a fraction of a dB. That reasoning is wrong, and Adara heard it as zipper
//     noise on TIMBRE. What matters is not how big the change is but that it is a
//     STEP, landing on a block boundary, correlated with the fader: a step train
//     at the block rate. Measured on the host, the RMS second difference at a
//     block boundary was 14.8 % above the value just inside a block at M3f's
//     settings and 8.4 % above at M3g's, against 0.0 % for upstream. Ramping
//     brings it to 1.0 %. See the table in the header.
//
//  4. The spectrum (UpdateAmplitudes) is recomputed every kAmplitudeUpdatePeriod
//     blocks with its smoothing coefficient scaled to match, instead of every
//     block.

#include "plaits/dsp/engine/additive_engine.h"

#include <algorithm>

#include "stmlib/dsp/parameter_interpolator.h"
#include "plaits/dsp/oscillator/sine_oscillator.h"

namespace plaits {

using namespace std;
using namespace stmlib;

void AdditiveEngine::Init(BufferAllocator* allocator) {
  amplitudes_ = allocator->Allocate<float>(kNumHarmonics);
  phase_ = 0.0f;
  frequency_ = 0.0f;
  update_counter_ = 0;
  fill(&a_out_[0], &a_out_[kNumOutHarmonics], 0.0f);
  fill(&a_aux_[0], &a_aux_[kNumAuxHarmonics], 0.0f);
}

void AdditiveEngine::Reset() {
  fill(
      &amplitudes_[0],
      &amplitudes_[kNumHarmonics],
      0.0f);
  fill(&a_out_[0], &a_out_[kNumOutHarmonics], 0.0f);
  fill(&a_aux_[0], &a_aux_[kNumAuxHarmonics], 0.0f);
  update_counter_ = 0;
}

void AdditiveEngine::UpdateAmplitudes(
    float centroid,
    float slope,
    float bumps,
    float* amplitudes,
    const int* harmonic_indices,
    size_t num_harmonics) {
  const float n = (static_cast<float>(num_harmonics) - 1.0f);
  const float margin = (1.0f / slope - 1.0f) / (1.0f + bumps);
  const float center = centroid * (n + margin) - 0.5f * margin;

  float sum = 0.001f;

  for (size_t i = 0; i < num_harmonics; ++i) {
    float order = fabsf(static_cast<float>(i) - center) * slope;
    float gain = 1.0f - order;
    gain += fabsf(gain);
    gain *= gain;

    float b = 0.25f + order * bumps;
    float bump_factor = 1.0f + Sine(b);

    gain *= bump_factor;
    gain *= gain;
    gain *= gain;

    int j = harmonic_indices[i];

    // Upstream: ONE_POLE(amplitudes[j], gain, 0.001f), called every 12-sample block.
    // Called every kAmplitudeUpdatePeriod blocks of kBlockSize here, so the coefficient
    // is scaled by the samples between updates over 12, to keep the same time constant.
    ONE_POLE(amplitudes[j], gain,
             0.001f * kAmplitudeUpdatePeriod * (static_cast<float>(kBlockSize) / 12.0f));
    sum += amplitudes[j];
  }

  sum = 1.0f / sum;

  for (size_t i = 0; i < num_harmonics; ++i) {
    amplitudes[harmonic_indices[i]] *= sum;
  }
}

namespace {

const int integer_harmonics[24] = {
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
  16, 17, 18, 19, 20, 21, 22, 23
};

const int organ_harmonics[8] = {
  0, 1, 2, 3, 5, 7, 9, 11
};

// Per-sub-step increment that walks the live coefficients a[] to the target for
// harmonics 1..n: amplitude times upstream's anti-aliasing taper (1 - 2 f_k), zero
// at and above Nyquist. `inv_steps` = 1 / (number of sub-steps in this block), so
// the last sub-step lands exactly on the target however long the block is.
template<int n>
inline void Increments(float f0, const float* amplitudes, const float* a,
                       float* da, float inv_steps) {
  for (int k = 0; k < n; ++k) {
    float f = f0 * static_cast<float>(k + 1);
    if (f >= 0.5f) {
      f = 0.5f;
    }
    da[k] = (amplitudes[k] * (1.0f - f * 2.0f) - a[k]) * inv_steps;
  }
}

// Clenshaw: sum_{m=0..n-1} a[m] y(m), where y obeys upstream's recurrence
// y(m+1) = 2x y(m) - y(m-1) and starts from y(0) = y_first, y(-1) = y_before.
// Upstream's first oscillator starts from (x, 1); its second, for harmonics 13+,
// from (Sine(13 p), Sine(12 p + 1/4)). Passing the same starting pair gives the
// same sum.
template<int n>
inline float Clenshaw(const float* a, float two_x, float y_first, float y_before) {
  float b1 = 0.0f;
  float b2 = 0.0f;
  for (int m = n - 1; m >= 0; --m) {
    const float b0 = (a[m] - b2) + two_x * b1;
    b2 = b1;
    b1 = b0;
  }
  return y_first * b1 - y_before * b2;
}

}  // namespace

void AdditiveEngine::Render(
    const EngineParameters& parameters,
    float* out,
    float* aux,
    size_t size,
    bool* already_enveloped) {
  const float f0 = NoteToFrequency(parameters.note);

  if (update_counter_ == 0) {
    const float centroid = parameters.timbre;
    const float raw_bumps = parameters.harmonics;
    const float raw_slope = (1.0f - 0.6f * raw_bumps) * parameters.morph;
    const float slope = 0.01f + 1.99f * raw_slope * raw_slope * raw_slope;
    const float bumps = 16.0f * raw_bumps * raw_bumps;
    UpdateAmplitudes(
        centroid,
        slope,
        bumps,
        &amplitudes_[0],
        integer_harmonics,
        24);
    UpdateAmplitudes(
        centroid,
        slope,
        bumps,
        &amplitudes_[24],
        organ_harmonics,
        8);
  }
  if (++update_counter_ >= kAmplitudeUpdatePeriod) {
    update_counter_ = 0;
  }

  float frequency = f0;
  if (frequency >= 0.5f) {
    frequency = 0.5f;
  }
  // Sub-steps at samples 0, kRampStride, 2*kRampStride ... inside this block; the
  // count is rounded UP so the increment is exact for any block size (ours is
  // always 12, which gives three sub-steps of a third each).
  const int ramp_steps =
      static_cast<int>((size + kRampStride - 1) / kRampStride);
  const float inv_steps = 1.0f / static_cast<float>(ramp_steps);
  float da_out[kNumOutHarmonics];
  float da_aux[kNumAuxHarmonics];
  Increments<kNumOutHarmonics>(frequency, &amplitudes_[0], a_out_, da_out,
                               inv_steps);
  Increments<kNumAuxHarmonics>(frequency, &amplitudes_[24], a_aux_, da_aux,
                               inv_steps);

  size_t i = 0;
  ParameterInterpolator fm(&frequency_, frequency, size);
  while (size--) {
    if (i++ % kRampStride == 0) {
      for (int k = 0; k < kNumOutHarmonics; ++k) { a_out_[k] += da_out[k]; }
      for (int k = 0; k < kNumAuxHarmonics; ++k) { a_aux_[k] += da_aux[k]; }
    }
    phase_ += fm.Next();
    if (phase_ >= 1.0f) {
      phase_ -= 1.0f;
    }
    const float x = SineNoWrap(phase_);
    const float two_x = 2.0f * x;
    // OUT: harmonics 1-12 and 13-24, exactly as upstream's two oscillators.
    *out++ = Clenshaw<12>(&a_out_[0], two_x, x, 1.0f) +
             Clenshaw<12>(&a_out_[12], two_x,
                          Sine(phase_ * 13.0f), Sine(phase_ * 12.0f + 0.25f));
    // AUX: the organ, harmonics 1-12 (8 of them non-zero), upstream's third.
    *aux++ = Clenshaw<kNumAuxHarmonics>(a_aux_, two_x, x, 1.0f);
  }
}

}  // namespace plaits
