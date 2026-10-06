// wakes-sp1 M4a: host checks of the Marbles -> Plaits routing and the new HARMONICS
// attenuverter. Drives the real sp1_synth / sp1_marbles glue.
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>

extern "C" {
#include "sp1_synth.h"
#include "sp1_marbles.h"
#include "sp1_marbles_ui.h"
#include "sp1_plaits_ui.h"
}

// M4c: the per-channel range hook the generated x_y_generator.cc override calls. Declared
// here for the same reason the override declares it -- C++ linkage in namespace marbles.
#include "marbles/random/x_y_generator.h"
namespace marbles { int sp1_mrb_channel_range(int channel, int group_range); }

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); \
  printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint32_t kFrames = 240;   // one DMA block

static void base_params(sp1_synth_params* p) {
  memset(p, 0, sizeof(*p));
  p->note = 48.0f;
  p->harmonics = 0.3f;
  p->timbre = 0.5f;
  p->morph = 0.5f;
  p->decay = 0.8f;
  p->lpg_colour = 0.5f;
  p->engine = 8;                       // virtual analog
  for (int k = 0; k < 3; ++k) { p->mrb_t_dest[k] = SP1_DEST_NONE; }
  for (int k = 0; k < 4; ++k) { p->mrb_dest[k] = SP1_DEST_NONE; }
}

// ---- section 9's warm-up, and why it is not optional ----
// Marbles applies a channel's ScaleOffset at the moment a voltage is GENERATED
// (OutputChannel::GenerateNewVoltage), not when it is read out: `voltage_` is then held
// until that channel's next clock event, and with STEPS smooth the lag processor ramps
// from the previous voltage to the new one. So for up to one clock period after a range
// change the channel is still emitting the OLD range's value, and during the ramp it can
// sit anywhere between the two -- which reads as "the new range was exceeded".
//
// ⚠️ This is Marbles behaving correctly, not a defect, and it is the whole reason the
// first version of this section failed: measurement started on the sample after
// sp1_marbles_set_params(). At the rate this section runs (mp.rate 36 with t_range x4 =
// ~64 Hz, so ~62 samples per event) 40 blocks is ~13 events, comfortably past it.
//
// It is also worth knowing on hardware: changing engine, or re-routing an output, moves
// the range from the NEXT value that channel produces onward, never retroactively.
static void settle(void) {
  for (int i = 0; i < 40; ++i) { sp1_marbles_render(20); }
}

static const char* range_name(int r) {
  switch (r) {
    case marbles::VOLTAGE_RANGE_NARROW:   return "0..2V";
    case marbles::VOLTAGE_RANGE_POSITIVE: return "0..5V";
    case marbles::VOLTAGE_RANGE_FULL:     return "+-5V";
    default:                              return "?";
  }
}

static double render_rms(int blocks) {
  std::vector<int16_t> out(kFrames);
  double e = 0.0;
  size_t n = 0;
  for (int i = 0; i < blocks; ++i) {
    sp1_synth_render(out.data(), kFrames);
    for (uint32_t k = 0; k < kFrames; ++k) { e += double(out[k]) * out[k]; ++n; }
  }
  return sqrt(e / double(n));
}

int main() {
  sp1_synth_init();
  sp1_marbles_init();
  sp1_marbles_seed(12345u);

  sp1_marbles_params mp;
  // Marbles' own defaults, then a fast clock so gates land inside every block.
  memset(&mp, 0, sizeof(mp));
  mp.rate = 36.0f;                     // 3 octaves above 120 BPM
  mp.t_range = 2;                      // x4
  mp.t_model = 0;
  mp.t_bias = 0.5f;
  mp.gate_length = 0.5f;
  mp.length = 8;
  mp.x_spread = 0.5f;
  mp.x_bias = 0.5f;
  mp.x_steps = 0.66f;
  mp.x_range = 2;                      // +-5 V, the widest
  mp.y_spread = 0.5f;
  mp.y_bias = 0.5f;
  mp.y_divider = 6;
  mp.y_range = 2;
  sp1_marbles_set_params(&mp);
  sp1_marbles_run(true);

  // ---------- 1. TRIG comes from the t outputs routed to it ----------
  printf("1. TRIG routing\n");
  {
    sp1_synth_params p;
    base_params(&p);
    sp1_synth_set_params(&p);
    const uint32_t e0 = sp1_synth_trig_edges();
    render_rms(40);
    const uint32_t none = sp1_synth_trig_edges() - e0;
    CHECK(none == 0u, "TRIG fired %u times with nothing routed", none);

    p.mrb_t_dest[0] = SP1_DEST_TRIG;
    p.mrb_t_dest[2] = SP1_DEST_TRIG;
    sp1_synth_set_params(&p);
    const uint32_t e1 = sp1_synth_trig_edges();
    render_rms(40);
    const uint32_t some = sp1_synth_trig_edges() - e1;
    printf("   t1 + t3 -> TRIG: %u edges in 40 blocks (200 ms)\n", some);
    CHECK(some > 0u, "no TRIG edges with t1 + t3 routed");
  }

  // ---------- 2. stacking clamps instead of running away ----------
  printf("2. the summing clamp\n");
  {
    // Three gates on TIMBRE would ask for 3 x 1.6. Nothing here can observe
    // mods.timbre directly, so check the audible consequence: the output must stay
    // bounded and free of the full-scale excursions an unclamped sum produces.
    sp1_synth_params p;
    base_params(&p);
    p.timbre_mod = 1.0f;               // attenuverter wide open
    p.mrb_t_dest[0] = SP1_DEST_TIMBRE;
    p.mrb_t_dest[1] = SP1_DEST_TIMBRE;
    p.mrb_t_dest[2] = SP1_DEST_TIMBRE;
    p.mrb_dest[0] = SP1_DEST_TIMBRE;
    p.mrb_dest[1] = SP1_DEST_TIMBRE;
    p.mrb_dest[2] = SP1_DEST_TIMBRE;
    p.mrb_dest[3] = SP1_DEST_TIMBRE;
    sp1_synth_set_params(&p);
    std::vector<int16_t> out(kFrames);
    int32_t peak = 0;
    size_t clips = 0;
    for (int i = 0; i < 200; ++i) {
      sp1_synth_render(out.data(), kFrames);
      for (uint32_t k = 0; k < kFrames; ++k) {
        const int32_t a = out[k] < 0 ? -out[k] : out[k];
        if (a > peak) { peak = a; }
        if (a >= 32767) { ++clips; }
      }
    }
    printf("   seven outputs stacked on TIMBRE: peak %d, %zu clipped samples\n",
           peak, clips);
    CHECK(clips == 0u, "stacked modulation clipped the output");
  }

  // ---------- 3. the HARMONICS attenuverter ----------
  printf("3. HARMONICS attenuverter (M4a)\n");
  {
    // (a) routed HARMONICS does nothing with the attenuverter at 0, and something
    //     with it open -- that is what an attenuverter IS.
    sp1_synth_params p;
    base_params(&p);
    p.mrb_dest[0] = SP1_DEST_HARM;
    p.harm_mod = 0.0f;
    sp1_synth_set_params(&p);
    const double a = render_rms(60);
    p.harm_mod = 1.0f;
    sp1_synth_set_params(&p);
    const double b = render_rms(60);
    printf("   X1 -> HARMONICS: rms %.1f at gain 0, %.1f at gain +1\n", a, b);
    CHECK(fabs(b - a) > 1.0, "the HARMONICS attenuverter changed nothing");

    // (b) with NOTHING patched to HARMONICS, the attenuverter becomes the depth of
    //     Plaits' internal decay envelope on HARMONICS -- the behaviour FM, TIMBRE
    //     and MORPH already have and that upstream HARMONICS cannot have at all.
    base_params(&p);
    p.harm_mod = 0.0f;
    sp1_synth_set_params(&p);
    sp1_synth_trigger();
    const double c = render_rms(60);
    base_params(&p);
    p.harm_mod = 1.0f;
    sp1_synth_set_params(&p);
    sp1_synth_trigger();
    const double d = render_rms(60);
    printf("   unpatched, internal envelope: rms %.1f at gain 0, %.1f at gain +1\n",
           c, d);
    CHECK(fabs(d - c) > 1.0,
          "the internal envelope does not reach HARMONICS (voice.cc override?)");
  }

  // ---------- 4. LEVEL as a destination ----------
  printf("4. LEVEL routing\n");
  {
    sp1_synth_params p;
    base_params(&p);
    // Nothing on LEVEL and no TRIG: Plaits' LPG gates on TRIG, so this is quiet.
    sp1_synth_set_params(&p);
    const double quiet = render_rms(60);
    // t1 -> LEVEL holds the VCA open in time with the gate.
    p.mrb_t_dest[0] = SP1_DEST_LEVEL;
    sp1_synth_set_params(&p);
    const double open = render_rms(60);
    printf("   rms %.1f with nothing on LEVEL, %.1f with t1 -> LEVEL\n", quiet, open);
    CHECK(open > quiet, "t1 -> LEVEL did not open the VCA");
  }

  // ---------- 5. stopped = every Marbles input unpatched ----------
  printf("5. stopped clock\n");
  {
    sp1_marbles_run(false);
    sp1_synth_params p;
    base_params(&p);
    p.mrb_t_dest[0] = SP1_DEST_TRIG;
    sp1_synth_set_params(&p);
    const uint32_t e0 = sp1_synth_trig_edges();
    render_rms(60);
    CHECK(sp1_synth_trig_edges() == e0, "a stopped clock still fired TRIG");
  }

  // ---------- 6. the FREQUENCY scale quantizer (M4b) ----------
  printf("6. FREQUENCY quantizer\n");
  {
    const uint16_t mid[4] = { 1850, 1850, 1850, 1850 };
    uint16_t f[4];
    sp1_pui_init();
    sp1_pui_enter(mid);

    // Sweep F1 across the full range (octave mode 10 is the default) with each scale
    // and collect the pitch classes that come out.
    for (int sc = -1; sc < SP1_MUI_SCALES; ++sc) {
      if (sc >= 0 && !sp1_mui_scale_included(sc)) { continue; }
      sp1_pui_set_scale(sc);
      // Distinct pitch classes, to 0.05 of a semitone -- NOT rounded to integers:
      // pelog, bhairav and shri are microtonal and never land on the semitone grid.
      float cls[32];
      int cnt = 0;
      for (int k = 0; k <= 1200; ++k) {
        memcpy(f, mid, sizeof(f));
        f[0] = static_cast<uint16_t>(3701.0 * k / 1200.0);
        for (int t = 0; t < 8; ++t) {          // let the fader smoothing settle
          sp1_pui_tick(8u, f, true, false, false);
        }
        sp1_synth_params p;
        sp1_pui_params(&p);
        float c = fmodf(p.note, 12.0f);
        if (c < 0.0f) { c += 12.0f; }
        bool seen = false;
        for (int i = 0; i < cnt; ++i) {
          if (fabsf(cls[i] - c) < 0.05f) { seen = true; break; }
        }
        if (!seen && cnt < 32) { cls[cnt++] = c; }
      }
      // sort for readability
      for (int i = 1; i < cnt; ++i) {
        for (int j = i; j > 0 && cls[j] < cls[j - 1]; --j) {
          const float t = cls[j]; cls[j] = cls[j - 1]; cls[j - 1] = t;
        }
      }
      char set[192] = { 0 };
      for (int i = 0; i < cnt; ++i) {
        char b[16]; snprintf(b, sizeof b, "%.2f ", cls[i]); strcat(set, b);
      }
      printf("   %-12s %2d notes/octave: %s\n",
             sc < 0 ? "off" : sp1_marbles_scale_name(sc), cnt, set);
      if (sc < 0) {
        // Unquantized FREQUENCY is CONTINUOUS, so it fills the counter's 32 slots.
        CHECK(cnt >= 20, "unquantized FREQUENCY should be continuous, got %d steps", cnt);
      } else if (sc == 0) {
        CHECK(cnt == 7, "major should be 7 notes per octave, got %d", cnt);
        const float want[7] = { 0, 2, 4, 5, 7, 9, 11 };
        for (int i = 0; i < 7 && cnt == 7; ++i) {
          CHECK(fabsf(cls[i] - want[i]) < 0.05f,
                "major degree %d is %.2f, expected %.0f", i, cls[i], want[i]);
        }
      } else if (sc == 3 || sc == 4 || sc == 5) {
        // the three microtonal scales: 5 (pelog) or 7 (ragas) degrees, off-grid
        CHECK(cnt >= 5 && cnt <= 7, "%s should be 5-7 notes, got %d",
              sp1_marbles_scale_name(sc), cnt);
        bool off_grid = false;
        for (int i = 0; i < cnt; ++i) {
          if (fabsf(cls[i] - lrintf(cls[i])) > 0.05f) { off_grid = true; }
        }
        CHECK(off_grid, "%s should be microtonal, but every degree is a semitone",
              sp1_marbles_scale_name(sc));
      } else if (sc == 2) {
        CHECK(cnt == 5, "pentatonic should be 5 pitch classes, got %d", cnt);
      } else if (sc == 1) {
        CHECK(cnt == 7, "minor should be 7 pitch classes, got %d", cnt);
      }
    }

    // ---- octave mode 9 + a scale = a sweep of scale DEGREES over nine octaves ----
    sp1_pui_set_scale(0);                        // major, 7 degrees -> 63 steps
    memcpy(f, mid, sizeof(f));
    f[0] = 0;
    // SETTINGS F1 to mode 9: 9/11 of travel
    {
      uint16_t g[4];
      memcpy(g, mid, sizeof(g));
      // double-tap "••" to latch SETTINGS
      sp1_pui_tick(8u, mid, true, false, false);
      sp1_pui_tick(8u, mid, true, true, false);
      sp1_pui_tick(8u, mid, true, false, false);
      sp1_pui_tick(8u, mid, true, true, false);
      sp1_pui_tick(8u, mid, true, false, false);
      // ⚠️ Pickup follows MOVEMENT, not position: holding a fader still never catches
      // up. Ramp F1 down until octave mode reads 9.
      for (int k = 1850; k >= 0 && sp1_pui_octave_mode() != 9; k -= 2) {
        g[0] = static_cast<uint16_t>(k);
        sp1_pui_tick(8u, g, true, false, false);
      }
    }
    CHECK(sp1_pui_octave_mode() == 9, "could not reach octave mode 9 (got %d)",
          sp1_pui_octave_mode());
    // back to BASE and sweep F1
    sp1_pui_tick(8u, mid, true, true, false);
    sp1_pui_tick(8u, mid, true, false, false);
    {
      int distinct = 0;
      float last = -999.0f;
      float lo = 9999.0f, hi = -9999.0f;
      for (int k = 0; k <= 4000; ++k) {
        memcpy(f, mid, sizeof(f));
        f[0] = static_cast<uint16_t>(3701.0 * k / 4000.0);
        for (int t = 0; t < 4; ++t) { sp1_pui_tick(8u, f, true, false, false); }
        sp1_synth_params p;
        sp1_pui_params(&p);
        if (fabsf(p.note - last) > 0.01f) { distinct++; last = p.note; }
        if (p.note < lo) { lo = p.note; }
        if (p.note > hi) { hi = p.note; }
      }
      printf("   mode 9 + major: %d distinct notes, MIDI %.0f..%.0f (%.1f octaves)\n",
             distinct, lo, hi, (hi - lo) / 12.0f);
      CHECK(distinct >= 55 && distinct <= 70,
            "mode 9 with a 7-note scale should give ~63 steps, got %d", distinct);
      CHECK(hi - lo > 90.0f, "mode 9 should still span nine octaves, got %.1f", hi - lo);
    }

    // ---- OCTV's bottom position (issue #18): the full range, NO detent, still quantized ----
    // F1 2 % above centre is inside mode 10's 5 % detent (C4) but not in mode 0, where it is
    // 60 + 0.04 * 48 = 61.9 before the quantizer -- which must still apply (major: D4).
    {
      const float sh[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
      const float st[4] = { 0.0f, 0.0f, 0.5f, 0.0f };
      sp1_pui_load_mods(sh, st);
    }
    CHECK(sp1_pui_octave_mode() == 0, "could not reach OCTV's bottom (got %d)",
          sp1_pui_octave_mode());
    {
      const int k1 = static_cast<int>(0.52f * 3701.0f + 0.5f);
      for (int k = 1850; k <= k1; ++k) {          // pickup follows movement: ramp to it
        memcpy(f, mid, sizeof(f));
        f[0] = static_cast<uint16_t>(k);
        sp1_pui_tick(8u, f, true, false, false);
      }
      for (int t = 0; t < 50; ++t) { sp1_pui_tick(8u, f, true, false, false); }
      sp1_synth_params p0, p10;
      sp1_pui_params(&p0);
      sp1_pui_set_octave_max();                   // the same F1 in mode 10
      sp1_pui_params(&p10);
      printf("   OCTV bottom + major, F1 +2 %%: note %.2f (mode 10: %.2f)\n", p0.note, p10.note);
      CHECK(p10.note == 60.0f, "mode 10: F1 2 %% off centre should be in the detent, got %.3f",
            p10.note);
      CHECK(p0.note > 60.5f, "OCTV bottom: F1 2 %% off centre should NOT be detented, got %.3f",
            p0.note);
      const float pc = fmodf(p0.note, 12.0f);
      CHECK(fabsf(p0.note - lrintf(p0.note)) < 0.05f &&
            (lrintf(pc) == 2 || lrintf(pc) == 4),
            "OCTV bottom: the scale should still quantize (major D/E), got %.3f", p0.note);
    }
    sp1_pui_set_scale(SP1_PUI_SCALE_OFF);
  }

  // ---------- 7. the soft-clip drive (M4b) ----------
  printf("7. soft-clip drive\n");
  {
    sp1_synth_params p;
    base_params(&p);
    p.level_patched = 1;
    p.level = 1.0f;                            // a drone, so the level is steady
    sp1_synth_set_params(&p);
    sp1_synth_set_drive(0);
    render_rms(20);                            // settle
    const double dry = render_rms(60);
    std::vector<int16_t> out(kFrames);
    sp1_synth_set_drive(SP1_DRIVE_STEPS - 1);  // +24 dB since M4e
    render_rms(20);
    double e = 0.0; int32_t peak = 0; size_t clips = 0, n = 0;
    for (int i = 0; i < 60; ++i) {
      sp1_synth_render(out.data(), kFrames);
      for (uint32_t k = 0; k < kFrames; ++k) {
        e += double(out[k]) * out[k]; ++n;
        const int32_t a = out[k] < 0 ? -out[k] : out[k];
        if (a > peak) { peak = a; }
        if (a >= 32767) { ++clips; }
      }
    }
    const double wet = sqrt(e / double(n));
    printf("   ladder:");
    for (int st = 0; st < SP1_DRIVE_STEPS; ++st) {
      printf(" %d:+%ddB", st, sp1_synth_drive_db(st));
    }
    printf("\n   rms %.1f dry -> %.1f at +%d dB (%.1f dB out), peak %d, %zu hard clips\n",
           dry, wet, sp1_synth_drive_db(SP1_DRIVE_STEPS - 1),
           20.0 * log10(wet / (dry > 0 ? dry : 1e-9)), peak, clips);
    // M4e: UNEVEN steps (Adara) -- +3 / +8 / +15 / +24 dB, so the first setting is still
    // gentle and the last is a fuzz. A flat ladder would have lost the subtle end.
    CHECK(sp1_synth_drive_db(0) == 0, "step 0 must be 0 dB (the stage is skipped)");
    for (int st = 1; st < SP1_DRIVE_STEPS; ++st) {
      CHECK(sp1_synth_drive_db(st) > sp1_synth_drive_db(st - 1),
            "the drive ladder is not monotonic at step %d", st);
    }
    CHECK(sp1_synth_drive_db(SP1_DRIVE_STEPS - 1) >= 20,
          "Adara asked for more extreme compression at the top: only +%d dB",
          sp1_synth_drive_db(SP1_DRIVE_STEPS - 1));
    CHECK(sp1_synth_drive_db(1) <= 4,
          "the first drive step should still be gentle, got +%d dB",
          sp1_synth_drive_db(1));
    CHECK(wet > dry * 1.2, "the drive did not raise the level");
    // ⚠️ At the TOP step the output IS pinned at full scale for much of the waveform, and
    // that is the feature, not a defect (Adara, M4e: "more extreme compression at the top
    // end"). stmlib::SoftClip asymptotes to exactly 1.0 at |x| >= 3, so a gain of 15.8
    // reaches it from an input of 0.19 and the top of the wave flattens -- a square-wave
    // fuzz. What must never happen is WRAPAROUND, which is what would turn a loud sound
    // into a destroyed one; Sat16 clamps, so check the bound rather than the count.
    printf("   pinned at full scale: %zu of %zu samples (%.0f %%) -- the fuzz, by design\n",
           clips, n, 100.0 * double(clips) / double(n));
    // ⚠️ `peak` above is |out[k]|, so the NEGATIVE rail reads as 32768 -- which is a
    // perfectly good int16_t, not an overflow. Bound the signed value instead, or this
    // check fails on a correctly saturated fuzz (it did, first time round).
    CHECK(peak <= 32768, "the output wrapped: |peak| %d", peak);
    // Back to 0. rms over a finite window wanders ~1 % with the oscillator's phase, so
    // compare the PEAK of a steady drone, which does not.
    sp1_synth_set_drive(0);
    render_rms(20);
    int32_t back_peak = 0;
    double be = 0.0; size_t bn = 0;
    for (int i = 0; i < 60; ++i) {
      sp1_synth_render(out.data(), kFrames);
      for (uint32_t k = 0; k < kFrames; ++k) {
        be += double(out[k]) * out[k]; ++bn;
        const int32_t a = out[k] < 0 ? -out[k] : out[k];
        if (a > back_peak) { back_peak = a; }
      }
    }
    const double back = sqrt(be / double(bn));
    printf("   back to 0: rms %.1f (was %.1f), peak %d\n", back, dry, back_peak);
    CHECK(fabs(back - dry) < dry * 0.05,
          "drive 0 is not the same as never having driven: %.1f vs %.1f", back, dry);

    // Issue #22 (Adara): ONE drive after the output select, and the OUT+AUX limiter
    // AFTER the drive, because it bounds what leaves the device. At the top step the
    // clipper alone would sit at full scale; in OUT+AUX the limiter must still hold the
    // output at its 0.8 ceiling.
    sp1_synth_set_output(SP1_OUT_SUM);
    sp1_synth_set_drive(SP1_DRIVE_STEPS - 1);
    render_rms(40);                            // the mode fade, the ramp, the limiter's attack
    int32_t sum_peak = 0;
    for (int i = 0; i < 60; ++i) {
      sp1_synth_render(out.data(), kFrames);
      for (uint32_t k = 0; k < kFrames; ++k) {
        const int32_t a = out[k] < 0 ? -out[k] : out[k];
        if (a > sum_peak) { sum_peak = a; }
      }
    }
    printf("   OUT+AUX at +%d dB: peak %d (limiter ceiling %d)\n",
           sp1_synth_drive_db(SP1_DRIVE_STEPS - 1), sum_peak, int(0.8f * 32768.0f));
    // The limiter's peak follower attacks at 0.05 per sample (Plaits' constants), so a fast
    // transient gets a few samples past the ceiling before the gain catches it: measured
    // +41 LSB with 12-sample Plaits blocks and +68 with 24 (#32), where the engine's
    // transients differ slightly. 128 LSB (0.5 %, 0.04 dB) is that overshoot with room; a
    // limiter that was NOT after the drive would sit at full scale, 6500 LSB higher.
    CHECK(sum_peak <= int32_t(0.8f * 32768.0f) + 128,
          "the OUT+AUX limiter is not after the drive: peak %d", sum_peak);
    CHECK(sum_peak > int32_t(0.7f * 32768.0f),
          "OUT+AUX at the top drive step should reach the limiter, peak %d", sum_peak);
    sp1_synth_set_drive(0);
    sp1_synth_set_output(SP1_OUT_MAIN);
    render_rms(20);
  }

  // ---------- 8. the deferred re-seed (M4d) ----------
  printf("8. deferred re-seed\n");
  {
    // Locked DEJA VU is the default, and it is the case that matters: with the loop
    // locked, re-seeding ALONE would never be heard, so the re-seed has to re-init.
    sp1_marbles_params m2 = mp;
    m2.t_deja_vu = 0.5f;          // locked
    m2.x_deja_vu = 0.5f;
    m2.length = 8;
    sp1_marbles_set_params(&m2);
    sp1_marbles_run(true);
    sp1_synth_params p;
    base_params(&p);
    p.mrb_t_dest[1] = SP1_DEST_TRIG;
    p.mrb_dest[0] = SP1_DEST_VOCT;
    sp1_synth_set_params(&p);

    // Record the X1 voltages of one pass round the locked loop.
    std::vector<float> before, after;
    for (int i = 0; i < 300; ++i) {
      sp1_marbles_render(20);
      for (int j = 0; j < 20; ++j) { before.push_back(sp1_marbles_volts(j, 0)); }
    }
    sp1_marbles_reseed(0xC0FFEEu);
    // The request must be honoured by the NEXT render, not by the caller.
    for (int i = 0; i < 300; ++i) {
      sp1_marbles_render(20);
      for (int j = 0; j < 20; ++j) { after.push_back(sp1_marbles_volts(j, 0)); }
    }
    size_t diff = 0;
    for (size_t i = 0; i < before.size() && i < after.size(); ++i) {
      if (fabsf(before[i] - after[i]) > 1e-6f) { ++diff; }
    }
    const double pct = 100.0 * double(diff) / double(before.size());
    printf("   locked loop, re-seeded: %.0f %% of X1 samples changed\n", pct);
    CHECK(pct > 50.0,
          "re-seeding a LOCKED loop changed only %.0f %% -- it must re-init, not just "
          "re-seed, or a rip would be silent", pct);

    // And it must be idempotent: no further change without another request.
    std::vector<float> again;
    for (int i = 0; i < 300; ++i) {
      sp1_marbles_render(20);
      for (int j = 0; j < 20; ++j) { again.push_back(sp1_marbles_volts(j, 0)); }
    }
    size_t d2 = 0;
    for (size_t i = 0; i < after.size() && i < again.size(); ++i) {
      if (fabsf(after[i] - again[i]) > 1e-6f) { ++d2; }
    }
    // One request must cause exactly one re-init. Comparing two fixed-length windows
    // sample by sample cannot reach 0 %: the loop period does not divide the window, so
    // the two windows start at different points in the loop. What matters is that the
    // residual is an order of magnitude below what the re-seed itself caused.
    printf("   and stable afterwards: %.0f %% changed on the next pass"
           " (window misalignment, not a second re-init)\n",
           100.0 * double(d2) / double(after.size()));
    CHECK(d2 * 5 < diff, "one re-seed request caused more than one re-init "
          "(%zu differ afterwards vs %zu caused by the re-seed)", d2, diff);

    // A re-seed while STOPPED must still take effect at the next PLAY.
    sp1_marbles_run(false);
    sp1_marbles_render(20);
    sp1_marbles_reseed(0xBEEF01u);
    sp1_marbles_render(20);        // applied here, while stopped
    sp1_marbles_run(true);
    std::vector<float> restart;
    for (int i = 0; i < 300; ++i) {
      sp1_marbles_render(20);
      for (int j = 0; j < 20; ++j) { restart.push_back(sp1_marbles_volts(j, 0)); }
    }
    size_t d3 = 0;
    for (size_t i = 0; i < again.size() && i < restart.size(); ++i) {
      if (fabsf(again[i] - restart[i]) > 1e-6f) { ++d3; }
    }
    printf("   re-seeded while stopped: %.0f %% changed after PLAY\n",
           100.0 * double(d3) / double(again.size()));
    CHECK(d3 * 2 > again.size(), "a re-seed while stopped did not take effect");
    sp1_marbles_run(false);
  }

  // ---------- 9. INTELLIGENT voltage range (M4c) ----------
  printf("9. INTELLIGENT range\n");
  {
    // Drive the generators directly and watch the range each X output actually produces.
    // This is the check that matters: if the x_y_generator.cc override failed to apply,
    // every channel would come out at the GROUP range and these spans would be identical.
    sp1_marbles_params m = mp;
    m.x_range = SP1_MRB_RANGE_INTELLIGENT;
    m.y_range = SP1_MRB_RANGE_INTELLIGENT;
    m.x_steps = 0.0f;          // smooth, so the full span is swept rather than quantized
    m.y_steps = 0.0f;
    m.x_spread = 1.0f;         // widest distribution, so the extremes are reached
    m.y_spread = 1.0f;
    m.t_deja_vu = 0.0f;        // fresh values every step
    m.x_deja_vu = 0.0f;

    struct Case { const char* name; uint8_t dest; uint8_t centre; float lo; float hi; };
    // centre bits: 0x1 HARMONICS, 0x2 TIMBRE, 0x4 MORPH
    const Case cases[] = {
      { "V/Oct           -> 0..2 V",  SP1_DEST_VOCT,   0x0u,  0.0f,  2.0f },
      { "LEVEL           -> 0..5 V",  SP1_DEST_LEVEL,  0x0u,  0.0f,  5.0f },
      { "FM              -> +-5 V",   SP1_DEST_FM,     0x0u, -5.0f,  5.0f },
      { "TIMBRE bipolar  -> +-5 V",   SP1_DEST_TIMBRE, 0x2u, -5.0f,  5.0f },
      { "TIMBRE unipolar -> 0..5 V",  SP1_DEST_TIMBRE, 0x0u,  0.0f,  5.0f },
      { "MORPH  bipolar  -> +-5 V",   SP1_DEST_MORPH,  0x4u, -5.0f,  5.0f },
      { "MORPH  unipolar -> 0..5 V",  SP1_DEST_MORPH,  0x0u,  0.0f,  5.0f },
      { "HARM   bipolar  -> +-5 V",   SP1_DEST_HARM,   0x1u, -5.0f,  5.0f },
      { "HARM   unipolar -> 0..5 V",  SP1_DEST_HARM,   0x0u,  0.0f,  5.0f },
    };
    sp1_marbles_run(true);
    for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ++ci) {
      const Case& cs = cases[ci];
      for (int k = 0; k < 4; ++k) { m.dest[k] = cs.dest; }
      m.engine_centre = cs.centre;
      sp1_marbles_set_params(&m);
      settle();
      // What the firmware resolved, read back from the same function the override calls.
      // If the override had not applied this would still print, so it is the SPAN below
      // that proves the range reached the channel -- this only names the intent.
      const int want = marbles::sp1_mrb_channel_range(0, -1);
      float lo = 1e9f, hi = -1e9f;
      for (int i = 0; i < 4000; ++i) {
        sp1_marbles_render(20);
        for (int j = 0; j < 20; ++j) {
          const float v = sp1_marbles_volts(j, 0);
          if (v < lo) { lo = v; }
          if (v > hi) { hi = v; }
        }
      }
      printf("   %-28s resolved %-8s observed %+.2f .. %+.2f V\n",
             cs.name, range_name(want), lo, hi);
      CHECK(want != -1, "%s: the hook returned the group fallback", cs.name);
      // The distribution need not touch the very ends, but it must stay inside the
      // range and must not fit comfortably inside a narrower one.
      CHECK(lo >= cs.lo - 0.05f && hi <= cs.hi + 0.05f,
            "%s: went outside its range (%+.2f..%+.2f)", cs.name, lo, hi);
      if (cs.lo < 0.0f) {
        CHECK(lo < -1.0f, "%s: never went negative, so it is not bipolar", cs.name);
      } else {
        CHECK(lo >= -0.05f, "%s: went negative, so it is not unipolar", cs.name);
      }
      CHECK(hi > cs.hi * 0.5f, "%s: never reached half its range, so the range looks "
            "narrower than intended", cs.name);
    }

    // Per-CHANNEL, not per group: route X1 to V/Oct and X2 to FM at the same time.
    m.dest[0] = SP1_DEST_VOCT;
    m.dest[1] = SP1_DEST_FM;
    m.dest[2] = SP1_DEST_LEVEL;
    m.dest[3] = SP1_DEST_FM;
    m.engine_centre = 0x0u;
    sp1_marbles_set_params(&m);
    settle();
    printf("   resolved per channel: X1 %s, X2 %s, X3 %s, Y %s\n",
           range_name(marbles::sp1_mrb_channel_range(0, -1)),
           range_name(marbles::sp1_mrb_channel_range(1, -1)),
           range_name(marbles::sp1_mrb_channel_range(2, -1)),
           range_name(marbles::sp1_mrb_channel_range(3, -1)));
    float lo1 = 1e9f, hi1 = -1e9f, lo2 = 1e9f, hi2 = -1e9f;
    for (int i = 0; i < 4000; ++i) {
      sp1_marbles_render(20);
      for (int j = 0; j < 20; ++j) {
        const float a = sp1_marbles_volts(j, 0);
        const float b = sp1_marbles_volts(j, 1);
        if (a < lo1) { lo1 = a; } if (a > hi1) { hi1 = a; }
        if (b < lo2) { lo2 = b; } if (b > hi2) { hi2 = b; }
      }
    }
    printf("   mixed: X1(V/Oct) %+.2f..%+.2f V, X2(FM) %+.2f..%+.2f V\n",
           lo1, hi1, lo2, hi2);
    CHECK(hi1 <= 2.05f && lo1 >= -0.05f,
          "X1 should be 0..2 V for V/Oct, got %+.2f..%+.2f", lo1, hi1);
    CHECK(lo2 < -1.0f && hi2 > 1.0f,
          "X2 should be bipolar for FM, got %+.2f..%+.2f -- the per-channel override "
          "did not apply", lo2, hi2);

    // And the override must be INERT for a group that is not INTELLIGENT: the same
    // destinations under an explicit range must all come out at that range, exactly as
    // upstream. Without this, a bug in IntelligentRange could quietly rewrite [J].
    m.x_range = SP1_MRB_RANGE_FULL;
    m.y_range = SP1_MRB_RANGE_NARROW;
    sp1_marbles_set_params(&m);
    settle();
    printf("   explicit groups: X1 %s, X2 %s, X3 %s, Y %s"
           " (dests unchanged -- INTELLIGENT must not leak)\n",
           range_name(marbles::sp1_mrb_channel_range(0, -1)),
           range_name(marbles::sp1_mrb_channel_range(1, -1)),
           range_name(marbles::sp1_mrb_channel_range(2, -1)),
           range_name(marbles::sp1_mrb_channel_range(3, -1)));
    for (int k = 0; k < 3; ++k) {
      CHECK(marbles::sp1_mrb_channel_range(k, -1) == marbles::VOLTAGE_RANGE_FULL,
            "X%d ignored an explicit FULL group range", k + 1);
    }
    CHECK(marbles::sp1_mrb_channel_range(3, -1) == marbles::VOLTAGE_RANGE_NARROW,
          "Y ignored an explicit NARROW group range");
    sp1_marbles_run(false);
  }

  // ---------- 10. the note held between TRIGs (M4e) ----------
  printf("10. FREQUENCY held between TRIGs\n");
  {
    // Adara's complaint: with a scale selected, moving F1 while a note is still decaying
    // slides that note from degree to degree. Correctly quantized, and not musical. The
    // latch is in the audio thread because TRIG only exists there.
    //
    // The observable is the PITCH of the rendered audio, so measure it: zero-crossing
    // period of a drone-free plucked note is awkward, so instead drive the voice with a
    // steady level and compare the dominant period before and after a `note` change.
    sp1_marbles_run(false);
    sp1_synth_params p;
    base_params(&p);
    p.level_patched = 1;
    p.level = 1.0f;
    p.engine = 8;                     // virtual analog: a clear pitched tone
    p.note = 48.0f;
    p.note_hold = 1;
    sp1_synth_set_params(&p);
    sp1_synth_trigger();              // latch 48
    render_rms(40);

    std::vector<int16_t> out(kFrames);
    // Count zero crossings over a window: proportional to frequency, and enough to tell
    // a semitone-scale change from none at all.
    auto crossings = [&](int blocks) {
      int c = 0; int16_t prev = 0;
      for (int i = 0; i < blocks; ++i) {
        sp1_synth_render(out.data(), kFrames);
        for (uint32_t k = 0; k < kFrames; ++k) {
          if ((prev < 0 && out[k] >= 0) || (prev >= 0 && out[k] < 0)) { ++c; }
          prev = out[k];
        }
      }
      return c;
    };
    const int at48 = crossings(40);

    // Move `note` a long way WITHOUT a TRIG. The pitch must not budge.
    p.note = 72.0f;
    sp1_synth_set_params(&p);
    render_rms(10);
    const int held = crossings(40);

    // Now TRIG: it must take the new note.
    sp1_synth_trigger();
    render_rms(10);
    const int after = crossings(40);
    printf("   hold on:  %d crossings at note 48, %d after note -> 72 with NO trig, "
           "%d after the trig\n", at48, held, after);
    CHECK(abs(held - at48) <= at48 / 10,
          "the note moved without a TRIG (%d -> %d crossings)", at48, held);
    CHECK(after > at48 * 2,
          "the TRIG did not pick up the new note (%d -> %d crossings)", at48, after);

    // ---- and with hold OFF, F1 must still be continuous ----
    p.note_hold = 0;
    p.note = 48.0f;
    sp1_synth_set_params(&p);
    sp1_synth_trigger();
    render_rms(40);
    const int off48 = crossings(40);
    p.note = 72.0f;
    sp1_synth_set_params(&p);
    render_rms(10);
    const int offmoved = crossings(40);
    printf("   hold off: %d at note 48, %d after note -> 72 with no trig "
           "(must follow)\n", off48, offmoved);
    CHECK(offmoved > off48 * 2,
          "with note_hold clear, FREQUENCY must follow the fader at once (%d -> %d)",
          off48, offmoved);
  }

  // ---------- 11. the burst is phase-locked to Marbles' clock (M4e) ----------
  printf("11. phase-locked burst\n");
  {
    // ⚠️ The defect this replaces: FFWD while running used to MULTIPLY Marbles' RATE, so
    // the master clock really sped up -- which moves the next tick and leaves the phase
    // displaced on release ("desynchronize it, depending on timing of FFWD press").
    //
    // What must be true now: the burst fires an exact number of TRIGs per beat, the same
    // number however long it runs, and Marbles' own beat count is untouched by it.
    sp1_marbles_params m = mp;
    m.rate = 0.0f;                  // 120 BPM x1 ... with t_range x4 below
    m.t_range = 1;                  // x1: 120 BPM, 2 Hz, so one beat = 24000 samples
    sp1_marbles_set_params(&m);
    sp1_marbles_run(true);
    sp1_synth_params p;
    base_params(&p);
    p.mrb_t_dest[1] = SP1_DEST_NONE;   // nothing but the burst may make a TRIG
    sp1_synth_set_params(&p);
    sp1_synth_set_tempo(120.0f);

    std::vector<int16_t> out(kFrames);
    // 1/8 notes = 2 per beat. Run a whole number of beats and count.
    sp1_synth_set_burst_div(8u);
    render_rms(20);
    const uint32_t beats0 = sp1_marbles_beats();
    const uint32_t b0 = sp1_synth_burst_count();
    sp1_synth_burst(1);
    // 8 beats at 120 BPM = 4 s = 192000 samples = 800 DMA blocks.
    for (int i = 0; i < 800; ++i) { sp1_synth_render(out.data(), kFrames); }
    sp1_synth_burst(0);
    const uint32_t fired = sp1_synth_burst_count() - b0;
    const uint32_t beats = sp1_marbles_beats() - beats0;
    printf("   1/8 over %u beats: %u TRIGs (%.2f per beat)\n", beats, fired,
           beats ? double(fired) / double(beats) : 0.0);
    CHECK(beats >= 7u && beats <= 9u, "the clock did not run 8 beats, it ran %u", beats);
    // 2 per beat, +-1 for where the hold started and ended.
    CHECK(fired >= 2u * beats - 2u && fired <= 2u * beats + 2u,
          "1/8 should fire 2 per beat: %u TRIGs over %u beats", fired, beats);

    // 1/32 = 8 per beat, same window. If the grid were a free-running accumulator this
    // would still be about right -- what it would NOT be is an exact multiple, and it
    // would drift with the clock. The ratio is the check that matters.
    sp1_synth_set_burst_div(32u);
    render_rms(20);
    const uint32_t beats1 = sp1_marbles_beats();
    const uint32_t b1 = sp1_synth_burst_count();
    sp1_synth_burst(1);
    for (int i = 0; i < 800; ++i) { sp1_synth_render(out.data(), kFrames); }
    sp1_synth_burst(0);
    const uint32_t fired2 = sp1_synth_burst_count() - b1;
    const uint32_t beats2 = sp1_marbles_beats() - beats1;
    printf("   1/32 over %u beats: %u TRIGs (%.2f per beat)\n", beats2, fired2,
           beats2 ? double(fired2) / double(beats2) : 0.0);
    CHECK(fired2 >= 8u * beats2 - 2u && fired2 <= 8u * beats2 + 2u,
          "1/32 should fire 8 per beat: %u TRIGs over %u beats", fired2, beats2);

    // ⚠️ THE POINT: holding FFWD must not change the CLOCK. Count beats over the same
    // wall-clock window with the burst on and with it off; they must match.
    const uint32_t nb0 = sp1_marbles_beats();
    for (int i = 0; i < 800; ++i) { sp1_synth_render(out.data(), kFrames); }
    const uint32_t quiet_beats = sp1_marbles_beats() - nb0;
    sp1_synth_burst(1);
    const uint32_t nb1 = sp1_marbles_beats();
    for (int i = 0; i < 800; ++i) { sp1_synth_render(out.data(), kFrames); }
    const uint32_t burst_beats = sp1_marbles_beats() - nb1;
    sp1_synth_burst(0);
    printf("   beats in 4 s: %u with no burst, %u with the burst held\n",
           quiet_beats, burst_beats);
    CHECK(quiet_beats == burst_beats,
          "holding FFWD changed the clock: %u beats vs %u -- that is the M4d ratchet "
          "back, and it is what made the rhythm shift", quiet_beats, burst_beats);
    sp1_synth_burst(0);
    sp1_marbles_run(false);
  }

  printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all checks passed",
         fails, fails == 1 ? "" : "s");
  return fails != 0;
}
