// wakes-sp1 M5a round 4: host checks of the MIDI script's `pickup` = shared and takeover
// (Adara), against the shipped script with only that line changed. hostbuild.sh builds this
// file twice: miditest_pickup_shared and miditest_pickup_takeover. (Pickup sum is the offset
// maths, miditest.cc.)
//
// Drives the real MIDI core, both UIs and their pickup the way main.c's control loop does:
// audio block, UI tick, sp1_midi_main_tick, sp1_pui_midi / sp1_mui_midi, then the params.
// Expected values come from Plaits' catch-up (pot_controller.h) worked as its continuous
// form: moving up, (1 - value) / (1 - control) stays constant until the two meet at the top;
// moving down, value / control does, until they meet at the bottom. The real steps are
// discrete, so the checks allow a couple of hundredths.
#include <cmath>
#include <cstdio>

extern "C" {
#include "sp1_midi.h"
#include "sp1_synth.h"
#include "sp1_plaits_ui.h"
#include "sp1_marbles_ui.h"
#include "sp1_marbles.h"
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); \
  printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint32_t kBlocks = 20;
static float moff[SP1_MIDI_AUDIO_DESTS];
// Virtual analog, slot 1: TIMBRE and HARMONICS bipolar, MORPH unipolar -- so MORPH (F3, CC 9)
// reads its stored value straight through, no detent.
static const uint8_t kCentre = 0x3;
static uint16_t raw[4];

static uint16_t R(float x) { return static_cast<uint16_t>(x * 3701.0f + 0.5f); }

static void cc(uint8_t n, uint8_t v) {
  const uint8_t m[3] = { 0xB0, n, v };
  sp1_midi_push(m, 3, 0);
}

// One control tick, in main.c's order, with one audio block before it.
static void tick() {
  if (sp1_midi_audio_begin(0, kBlocks, kCentre, moff)) {
    for (uint32_t j = 0; j < kBlocks; ++j) {
      sp1_midi_frame f;
      sp1_midi_audio_block(j, &f);
      sp1_midi_audio_lpg(0.0f, false);
    }
  }
  sp1_pui_tick(8, raw, true, false, false);
  sp1_midi_main_tick(8);
  sp1_pui_midi();
  sp1_mui_midi();
}
static void run(int n) { for (int i = 0; i < n; ++i) tick(); }

// Move fader i to x the way a hand does: over `ticks` ticks, then let the low-pass settle.
static void fader(int i, float x, int ticks = 60) {
  const float from = static_cast<float>(raw[i]) / 3701.0f;
  for (int k = 1; k <= ticks; ++k) {
    raw[i] = R(from + (x - from) * static_cast<float>(k) / static_cast<float>(ticks));
    tick();
  }
  run(60);
}

static sp1_synth_params P() {
  sp1_synth_params p;
  sp1_pui_params(&p);
  return p;
}
static float morph() { return P().morph; }

static void start() {
  raw[0] = R(0.5f); raw[1] = R(0.5f); raw[2] = R(0.2f); raw[3] = R(0.0f);
  sp1_pui_init();
  sp1_pui_enter(raw);                          // BASE seeded from the faders: MORPH 0.2
  sp1_mui_init();
  sp1_mui_enter(raw, false);
  run(60);
}

int main() {
#if SP1_MIDI_PICKUP == SP1_MIDI_PICKUP_SHARED
  printf("pickup = shared\n");
  sp1_midi_port(true);
  start();

  // ---- §1 a CC is no offset, and its first position is only a reference ----
  printf("§1 no offset; the first value moves nothing\n");
  cc(9, 100);                                  // MORPH, 0.787: far from the value (0.2)
  run(20);
  CHECK(moff[SP1_MIDI_D_MORPH] == 0.0f && sp1_midi_offset(SP1_MIDI_D_MORPH) == 0.0f,
        "shared: a CC must not be an offset (%.3f)", moff[SP1_MIDI_D_MORPH]);
  CHECK(fabsf(morph() - 0.2f) < 0.002f, "a host knob's first position must not move the "
        "value: %.3f", morph());

  // ---- §2 the CC catches up with the value, Plaits' way ----
  printf("§2 the CC catches up\n");
  cc(9, 110);                                  // up 0.079: 1 - v = 0.8 * 0.134 / 0.213
  run(20);
  CHECK(fabsf(morph() - 0.497f) < 0.02f, "CC up from 100 to 110: the value moves up at once "
        "and faster, %.3f (0.497; the CC itself is at 0.866)", morph());
  cc(9, 127);
  run(40);
  CHECK(morph() > 0.995f, "CC at the top: the two meet there, %.3f", morph());
  cc(9, 64);
  run(40);
  CHECK(fabsf(morph() - 64.0f / 127.0f) < 0.002f, "caught: the value follows the CC, %.3f",
        morph());
  CHECK(sp1_pui_catching(2), "the CC moved the value away from F3: F3 must catch up now");

  // ---- §3 ...and the fader catches up with the value the CC left ----
  printf("§3 the fader catches up after the CC\n");
  fader(2, 0.6f);                              // F3 0.2 -> 0.6: 1 - v = 0.496 * 0.4 / 0.8
  CHECK(fabsf(morph() - 0.752f) < 0.02f, "F3 up from 0.2 to 0.6: the value moves up from "
        "0.504, %.3f (0.752)", morph());
  fader(2, 1.0f);
  CHECK(morph() > 0.995f && !sp1_pui_catching(2), "F3 at the top: met, F3 follows (%.3f)",
        morph());
  fader(2, 0.3f);
  CHECK(fabsf(morph() - 0.3f) < 0.005f, "F3 follows: %.3f", morph());

  // ---- §4 the CC, left behind, catches up again ----
  printf("§4 the CC catches up again\n");
  cc(9, 54);                                   // CC 0.504 -> 0.425, down: v = 0.3 * 0.425 / 0.504
  run(30);
  CHECK(fabsf(morph() - 0.253f) < 0.02f, "CC down from 64 to 54: the value moves down from "
        "0.3, %.3f (0.253; the CC is at 0.425)", morph());
  cc(9, 0);
  run(40);
  CHECK(morph() < 0.005f, "CC at the bottom: met there, %.3f", morph());
  cc(9, 30);
  run(40);
  CHECK(fabsf(morph() - 30.0f / 127.0f) < 0.002f, "and the value follows it: %.3f", morph());

  // ---- §5 a rip moves the value under the CC: the CC catches up, nothing jumps ----
  printf("§5 rip, then the CC\n");
  sp1_pui_rip();                               // MORPH -> 0 (unipolar on virtual analog)
  run(2);
  CHECK(morph() == 0.0f, "rip: MORPH to its neutral, 0 (%.3f)", morph());
  cc(9, 40);                                   // CC 0.236 -> 0.315: 1 - v = 0.764 / 0.764 * 0.685
  run(30);
  CHECK(fabsf(morph() - 0.103f) < 0.02f, "CC up after the rip: from 0, %.3f (0.103; the CC "
        "is at 0.315)", morph());

  // ---- §6 a value on a page you are not standing on ----
  printf("§6 LPG colour (SETTINGS) from the BASE page\n");
  cc(24, 0);                                   // matches LPG colour's 0: follows at once
  run(10);
  cc(24, 64);
  run(40);
  CHECK(fabsf(P().lpg_colour - 64.0f / 127.0f) < 0.002f, "LPG colour follows its CC from "
        "another page: %.3f", P().lpg_colour);

  // ---- §7 Marbles, which is not on show ----
  printf("§7 Marbles RATE\n");
  cc(27, 64);                                  // 0.504 against RATE's 0.5: follows
  run(10);
  cc(27, 127);
  run(40);
  {
    sp1_marbles_params mp;
    sp1_mui_params(&mp);
    CHECK(fabsf(mp.rate - 60.0f) < 0.5f, "RATE CC 127 -> the top, +60 st (%.2f)", mp.rate);
  }

  // ---- §8 MODEL has no fader: still an offset ----
  printf("§8 MODEL\n");
  cc(105, 127);
  run(10);
  CHECK(sp1_midi_offset(SP1_MIDI_D_MODEL) == 1.0f && sp1_pui_eslot() != sp1_pui_slot(),
        "MODEL is an offset from the selection in every pickup");
  cc(105, 0);
  run(10);
  CHECK(sp1_pui_eslot() == sp1_pui_slot(), "MODEL 0 -> the selection");

  // ---- §9 unplugging leaves the values where MIDI put them ----
  printf("§9 disconnect\n");
  const float m9 = morph();
  const float c9 = P().lpg_colour;
  sp1_midi_port(false);
  run(100);
  CHECK(fabsf(morph() - m9) < 1e-6f && fabsf(P().lpg_colour - c9) < 1e-6f,
        "a disconnect leaves the values as the CCs left them (%.3f, %.3f)", morph(),
        P().lpg_colour);
  sp1_midi_port(true);
  run(2);
  cc(9, 127);                                  // a new connection: a reference again
  run(20);
  CHECK(fabsf(morph() - m9) < 1e-6f, "after plugging back in, the first value is a reference "
        "only (%.3f)", morph());
  CHECK(!sp1_midi_fader_held(SP1_MIDI_D_MORPH), "shared never rests a fader");
#elif SP1_MIDI_PICKUP == SP1_MIDI_PICKUP_TAKEOVER
  printf("pickup = takeover\n");
  start();                                     // port down: no MIDI yet

  // ---- §1 unplugged: the faders work ----
  printf("§1 unplugged\n");
  fader(2, 0.4f);
  CHECK(fabsf(morph() - 0.4f) < 0.005f, "no MIDI: F3 moves MORPH, %.3f", morph());

  // ---- §2 plugged in: the faders with a CC rest ----
  printf("§2 plugged in: F3 rests\n");
  sp1_midi_port(true);
  run(2);
  CHECK(sp1_midi_fader_held(SP1_MIDI_D_MORPH), "port up: MORPH's fader must rest");
  CHECK(!sp1_midi_fader_held(SP1_MIDI_D_MODEL), "MODEL has no fader to rest");
  fader(2, 0.7f);
  CHECK(fabsf(morph() - 0.4f) < 0.005f, "F3 moved to 0.7: MORPH must stay 0.4, %.3f",
        morph());

  // ---- §3 the CC takes over, catching up from the value the fader left ----
  printf("§3 the CC catches up\n");
  cc(9, 30);                                   // 0.236: a reference
  run(20);
  CHECK(fabsf(morph() - 0.4f) < 0.005f, "first value: a reference only, %.3f", morph());
  cc(9, 60);                                   // up to 0.472: 1 - v = 0.6 * 0.528 / 0.764
  run(30);
  CHECK(fabsf(morph() - 0.585f) < 0.02f, "CC up: the value moves up from 0.4, %.3f (0.585)",
        morph());
  cc(9, 127);
  run(40);
  cc(9, 50);
  run(40);
  CHECK(fabsf(morph() - 50.0f / 127.0f) < 0.002f, "met at the top, then follows the CC: %.3f",
        morph());

  // ---- §4 unplugged: the fader works again, catching up from the value the CC left ----
  printf("§4 unplugged: F3 catches up\n");
  sp1_midi_port(false);
  run(100);
  CHECK(!sp1_midi_fader_held(SP1_MIDI_D_MORPH), "port gone: F3 is free again");
  CHECK(fabsf(morph() - 50.0f / 127.0f) < 1e-6f, "unplugging leaves the value as the CC left "
        "it (%.3f)", morph());
  fader(2, 0.85f);                             // F3 0.7 -> 0.85: 1 - v = 0.606 * 0.15 / 0.3
  CHECK(fabsf(morph() - 0.697f) < 0.02f, "F3 up: the value moves up from 0.394, no jump to "
        "0.85, %.3f (0.697)", morph());
  fader(2, 1.0f);
  fader(2, 0.5f);
  CHECK(fabsf(morph() - 0.5f) < 0.005f, "met at the top, then F3 follows: %.3f", morph());
#else
#error "miditest_pickup.cc is for pickup = shared or takeover (hostbuild.sh)"
#endif

  printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all checks passed", fails,
         fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
