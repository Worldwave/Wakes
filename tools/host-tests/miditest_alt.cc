// wakes-sp1 M5a: host checks of MIDI in, against tools/host-tests/midi-alt.ini -- omni,
// legato on, portamento t20, bend range 7, no sustain pedal, no smoothing, velocity -> LEVEL
// and aftertouch -> TIMBRE 50 %. miditest.cc covers the shipped script.
#include <cmath>
#include <cstdio>

extern "C" {
#include "sp1_midi.h"
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); \
  printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint32_t kBlocks = 20;
static sp1_midi_frame fr[kBlocks];
static float off[SP1_MIDI_AUDIO_DESTS];

static void send(uint8_t s, uint8_t a, uint8_t b = 0) {
  const uint8_t m[3] = { s, a, b };
  const uint8_t t = s & 0xF0;
  sp1_midi_push(m, (t == 0xC0 || t == 0xD0) ? 2 : 3, 0);
}
static void block() {
  const bool work = sp1_midi_audio_begin(0, kBlocks, 0u, off);
  for (uint32_t j = 0; j < kBlocks; ++j) {
    fr[j] = sp1_midi_frame{ false, false, 0.0f, 0.0f };
    if (work) {
      sp1_midi_audio_block(j, &fr[j]);
      sp1_midi_audio_lpg(1.0f, false);
    }
  }
}
static bool any_trig() {
  for (uint32_t j = 0; j < kBlocks; ++j) { if (fr[j].trig) return true; }
  return false;
}
static const sp1_midi_frame& last() { return fr[kBlocks - 1]; }

int main() {
  static_assert(SP1_MIDI_CHANNEL == SP1_MIDI_OMNI && SP1_MIDI_LEGATO == 2 &&
                SP1_MIDI_PORTAMENTO == 20 && SP1_MIDI_SUSTAIN_CC == -1 &&
                SP1_MIDI_VELOCITY_DEST == SP1_MIDI_D_LEVEL &&
                SP1_MIDI_AFTERTOUCH_DEST == SP1_MIDI_D_TIMBRE,
                "built against the wrong script: hostbuild.sh should use midi-alt.ini");
  sp1_midi_port(true);

  // ---- §1 omni, velocity -> gate height ----
  printf("§1 omni, velocity -> LEVEL\n");
  send(0x95, 60, 64);                          // channel 6
  block();
  CHECK(any_trig(), "omni: channel 6 should play");
  CHECK(fabsf(last().gate - 64.0f / 127.0f) < 1e-4f, "velocity 64 -> gate %.3f", last().gate);

  // ---- §2 Yarns portamento t20: constant time, exponential ----
  // t(40) = (tmin^.25 + (tmax^.25 - tmin^.25) * 40/127)^4, tmin 0.75 ms, tmax 6 s.
  printf("§2 portamento t20\n");
  const float a = powf(3.0f / 4000.0f, 0.25f), b = powf(6.0f, 0.25f);
  const float r = a + (b - a) * 40.0f / 127.0f;
  const float t = r * r * r * r;
  const int steps = int(t * 4000.0f + 0.5f);
  printf("   glide time %.1f ms = %d Plaits blocks\n", t * 1000.0f, steps);
  send(0x95, 60, 0);                           // release the first note
  block();
  send(0x95, 72, 100);                         // from silence: glides up from 60
  int k = 0;
  float half = -100.0f;
  for (int i = 0; i < steps / 20 + 3; ++i) {
    block();
    for (uint32_t j = 0; j < kBlocks; ++j) {
      if (++k == steps / 2) half = fr[j].note;
    }
  }
  const float x = float(steps / 2) / (t * 4000.0f);
  // Yarns' lut_env_expo: 1 - exp(-4x), normalised at x = 255/256 (its last two entries).
  const float expect = 12.0f * (1.0f - expf(-4.0f * x)) /
                       (1.0f - expf(-4.0f * 255.0f / 256.0f));
  CHECK(fabsf(half - expect) < 0.25f, "mid-glide %.2f, Yarns' curve says %.2f", half, expect);
  CHECK(fabsf(last().note - 12.0f) < 1e-3f, "glide should end on +12, %.3f", last().note);

  // ---- §3 legato on: a note over a held one does not strike; returning does not either ----
  printf("§3 legato on\n");
  send(0x90, 74, 100);                         // 72 still held
  block();
  CHECK(!any_trig(), "legato: an overlapping note must not strike");
  send(0x80, 74, 0);
  block();
  CHECK(!any_trig(), "legato: returning to the held key must not strike");
  send(0x80, 72, 0);
  block();
  CHECK(last().gate == 0.0f, "all keys up");
  send(0x90, 72, 100);
  block();
  CHECK(any_trig(), "legato: a note from silence strikes");

  // ---- §4 no sustain pedal: CC 64 is just an unbound CC ----
  printf("§4 sustain off\n");
  send(0xB0, 64, 127);
  send(0x80, 72, 0);
  block();
  CHECK(last().gate == 0.0f, "sustain = off: CC 64 must not hold the note");

  // ---- §5 aftertouch -> TIMBRE 50 %, no smoothing ----
  printf("§5 aftertouch -> TIMBRE 50%%\n");
  send(0xD0, 127);
  block();
  block();                                     // the smoothing (off) takes it next block
  CHECK(fabsf(off[SP1_MIDI_D_TIMBRE] - 0.5f) < 1e-4f, "AT 127 at 50%% -> +0.5, %.4f",
        off[SP1_MIDI_D_TIMBRE]);
  send(0xB0, 3, 127);                          // and the CC on the same parameter adds
  block();
  block();
  CHECK(fabsf(off[SP1_MIDI_D_TIMBRE] - 1.0f) < 1e-4f, "CC + AT clamp at +1");

  // ---- §6 bend range 7 ----
  printf("§6 bend range 7\n");
  send(0xE0, 0x00, 0x00);
  for (int i = 0; i < 40; ++i) block();        // let §3's last glide (135 ms) finish
  CHECK(fabsf(last().note - 12.0f + 7.0f) < 1e-3f, "full down: -7 from the note, %.3f",
        last().note);

  printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all checks passed", fails,
         fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
