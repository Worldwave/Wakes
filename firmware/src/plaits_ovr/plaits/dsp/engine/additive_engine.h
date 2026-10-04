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
// wakes-sp1 (M3f): a CPU-reduced copy of Plaits' additive engine.
//
// Upstream: third_party/eurorack/plaits/dsp/engine/additive_engine.h (Emilie
// Gillet, MIT, above). This file SHADOWS it: firmware/CMakeLists.txt puts
// src/plaits_ovr before the eurorack root on the include path, so every Plaits
// file (voice.h included) sees this class layout, and compiles our .cc instead of
// upstream's. The vendored copy is not modified.
//
// Same sound design, same 24 + 8 partials, same controls. What changed is HOW the
// partials are summed -- see additive_engine.cc.

#ifndef PLAITS_DSP_ENGINE_ADDITIVE_ENGINE_H_
#define PLAITS_DSP_ENGINE_ADDITIVE_ENGINE_H_

#include "plaits/dsp/engine/engine.h"

namespace plaits {

const int kNumHarmonics = 36;       // amplitude slots, laid out as upstream:
                                    // [0..23] OUT, [24..35] AUX organ
const int kNumOutHarmonics = 24;    // OUT: harmonics 1..24
const int kNumAuxHarmonics = 12;    // AUX: harmonics 1..12 (8 organ drawbars)

// Recompute the spectrum once every this many 12-sample blocks. The smoothing
// coefficient is scaled by the same factor, so the response time to a fader move
// is unchanged.
//
// ---- why this went 2 -> 4 again in M4a, and what bought it back ----
// M3g spent +35 instructions/sample taking this from 4 to 2 because that measured
// "10 dB closer to upstream". That metric was misleading: nearly all of the
// difference it was measuring is LAG (our spectrum is computed up to a millisecond
// later), which is inaudible. The AUDIBLE defect was somewhere else -- the
// unramped coefficients below -- and Adara reported it from hardware as zipper
// noise on TIMBRE. So the update period pays for the ramp instead.
// #32: counted in blocks, so it is 48 samples (1 ms) worth of blocks: 4 of 12, 2 of 24.
#ifndef SP1_ADD_UPDATE_PERIOD
#define SP1_ADD_UPDATE_PERIOD (48 / static_cast<int>(kBlockSize))
#endif
const int kAmplitudeUpdatePeriod = SP1_ADD_UPDATE_PERIOD;

// ---- the zipper fix (M4a): ramp the coefficients inside the block ----
// Upstream gives every partial a stmlib::ParameterInterpolator, so an amplitude
// change is spread across the block. M3f's rewrite dropped that and held the
// coefficients constant for the whole block, which puts a STEP in the waveform at
// a block boundary every time the spectrum is recomputed -- a correlated step
// train, exactly what a moving TIMBRE sounded like.
//
// Measured on the host (a full TIMBRE sweep, MIDI 36, drone). The metric is the RMS
// second difference AT a 12-sample boundary over the same quantity just inside a
// block: 1.000 means the boundary is indistinguishable from anywhere else, i.e. no
// step. Projections from the M3f hardware run (419 instr/sample = 54.8 % avg /
// 64.0 % peak):
//
//   period  ramp            instr/sample   avg %   peak %   edge ratio (2 s sweep)
//   4       none (M3f)            419       54.8     64.0     1.148
//   2       none (M3g)            454       59.4     69.4     1.084
//   4       every 4 samples       465       60.8     71.0     1.010   <- M4a
//   4       every 2 samples       472       61.8     72.1     0.999   <- THIS BUILD
//   2       every 4 samples       500       65.4     76.4     1.007
//   4       every sample          503       65.8     76.9     0.999
//   2       every sample          539       70.4     82.3     1.000
//
// ---- why 4 -> 2 in M4e, and what the M4a number missed ----
// Adara, from hardware: "when HARMONICS is at minimum and MORPH is at maximum,
// Additive's TIMBRE modulation is steppy and clicky". Both true, and the reason is
// in UpdateAmplitudes: HARMONICS 0 gives bumps = 0 and MORPH 1 gives slope = 2.0,
// its maximum, and the gain is raised to the eighth power -- so ONE partial is
// alive at a time and its amplitude carries the entire output. A 4-sample staircase
// on an amplitude that quiet settings hide is, there, a staircase on the waveform.
//
// ⚠️ And M4a's 1.010 was measured on a 2 s sweep ONLY. A fast flick of F1 (the same
// sweep in 0.5 s, which is an ordinary hand movement) was never measured and is far
// worse: edge ratio 1.188 at HARMONICS 0 / MORPH 1, and 1.097 even at the reference
// setting -- both worse than M3f's 1.148, the defect M4a was built to fix. Measure
// the FAST gesture; a slow one hides a per-block artifact behind the spectrum's own
// movement.
//
// The ramp artifact, isolated properly by differencing against stride 1 (identical
// spectrum-update lag, so only the ramp differs) and band-limited to 16 kHz, on that
// fast flick:
//
//   setting                     stride 4    stride 2     peak error, stride 4 -> 2
//   HARMONICS 0 / MORPH 1       -41.2 dB    -52.2 dB     3375 -> 1159 counts
//   HARMONICS 0.3 / MORPH 0.6   -42.9 dB    -53.9 dB     1864 ->  621 counts
//
// 11 dB, and a peak error down from 13 % of full scale to 4.4 %, for +7.5
// instructions per sample (~1 % of the budget). Stride 1 buys the remaining ~52 dB
// for +36 instr/sample, which would put Additive at ~76.5 % peak -- past the ~75 %
// Adara set. 2 is the right trade and it is still under upstream's every-sample cost.
//
// ⚠️ Do NOT judge this by "how far is the output from upstream": that difference is
// -28 dB for stride 1, 2 and 4 alike, because it is almost entirely the LAG of the
// 4-block spectrum update. Same trap as M3g. Difference against another stride, not
// against upstream.
#ifndef SP1_ADD_RAMP_STRIDE
#define SP1_ADD_RAMP_STRIDE 2
#endif
const int kRampStride = SP1_ADD_RAMP_STRIDE;

class AdditiveEngine : public Engine {
 public:
  AdditiveEngine() { }
  ~AdditiveEngine() { }

  virtual void Init(stmlib::BufferAllocator* allocator);
  virtual void Reset();
  virtual void LoadUserData(const uint8_t* user_data) { }
  virtual void Render(const EngineParameters& parameters,
      float* out,
      float* aux,
      size_t size,
      bool* already_enveloped);

 private:
  void UpdateAmplitudes(
      float centroid,
      float slope,
      float bumps,
      float* amplitudes,
      const int* harmonic_indices,
      size_t num_harmonics);

  float* amplitudes_;
  // The live per-partial coefficients (amplitude x the anti-alias taper), ramped
  // toward the current spectrum inside the block. Upstream keeps the same state
  // inside its three HarmonicOscillators.
  float a_out_[kNumOutHarmonics];
  float a_aux_[kNumAuxHarmonics];
  float phase_;
  float frequency_;
  int update_counter_;

  DISALLOW_COPY_AND_ASSIGN(AdditiveEngine);
};

}  // namespace plaits

#endif  // PLAITS_DSP_ENGINE_ADDITIVE_ENGINE_H_
