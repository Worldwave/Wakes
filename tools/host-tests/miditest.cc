// wakes-sp1 M5a: host checks of MIDI in, against the shipped script (config/midi.ini) with
// its pickup set to SUM (hostbuild.sh): this suite checks the offset maths.
// Drives the real sp1_midi core directly, then through sp1_synth / both UIs.
// miditest_alt.cc covers the settings this script leaves at their defaults;
// miditest_pickup.cc the shipped script under pickup shared and takeover.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern "C" {
#include "sp1_midi.h"
#include "sp1_synth.h"
#include "sp1_plaits_ui.h"
#include "sp1_marbles_ui.h"
#include "sp1_engines_gen.h"
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); \
  printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint32_t kBlocks = 20;          // Plaits blocks per audio block
static sp1_midi_frame fr[kBlocks];
static float moff[SP1_MIDI_AUDIO_DESTS];      // this audio block's smoothed CC offsets
static bool work;                            // sp1_midi_audio_begin: anything to do
static uint8_t centre;                       // the "engine's" detent bits (0x2 = TIMBRE bipolar)
static float lpg_gain = 1.0f;                // what "Plaits" reports after each block

static void send(uint8_t s, uint8_t a, uint8_t b = 0, uint32_t cyc = 0) {
  const uint8_t m[3] = { s, a, b };
  const uint8_t t = s & 0xF0;
  sp1_midi_push(m, (t == 0xC0 || t == 0xD0) ? 2 : 3, cyc);
}
static void cc(uint8_t n, uint8_t v) { send(0xB0, n, v); }
static void on(uint8_t n, uint8_t v = 100) { send(0x90, n, v); }
static void off(uint8_t n) { send(0x80, n, 0); }

// One audio block of the MIDI core, as sp1_synth_render drives it: nothing per Plaits
// block when sp1_midi_audio_begin says there is nothing to do.
static void block(uint32_t cyc = 0) {
  work = sp1_midi_audio_begin(cyc, kBlocks, centre, moff);
  for (uint32_t j = 0; j < kBlocks; ++j) {
    fr[j] = sp1_midi_frame{ false, false, 0.0f, 0.0f };
    if (work) {
      sp1_midi_audio_block(j, &fr[j]);
      sp1_midi_audio_lpg(lpg_gain, false);
    }
  }
}
static bool any_trig() {
  for (uint32_t j = 0; j < kBlocks; ++j) { if (fr[j].trig) return true; }
  return false;
}
static const sp1_midi_frame& last() { return fr[kBlocks - 1]; }

// Pull the port and plug it back: the next block puts everything back to neutral, and with
// the LPG reported closed the release ends at once.
static void reset() {
  sp1_midi_port(true);
  sp1_midi_port(false);
  sp1_midi_port(true);
  lpg_gain = 0.0f;
  for (int i = 0; i < 40; ++i) block();
  lpg_gain = 1.0f;
}

static double render_rms(int blocks) {
  std::vector<int16_t> o(240);
  double e = 0.0;
  for (int i = 0; i < blocks; ++i) {
    sp1_synth_render(o.data(), 240);
    for (int k = 0; k < 240; ++k) e += double(o[k]) * o[k];
  }
  return sqrt(e / (240.0 * blocks));
}

int main() {
  static_assert(SP1_MIDI_PICKUP == SP1_MIDI_PICKUP_SUM, "built by hostbuild.sh with pickup = sum");
  // ---- §1 idle: nothing sent, nothing applied ----
  printf("§1 idle\n");
  sp1_midi_port(true);
  block();
  CHECK(!work && !sp1_midi_active(), "MIDI has work with nothing sent");
  CHECK(sp1_midi_port_up(), "port not up");

  // ---- §2 a note: TRIG, LEVEL held, pitch as V/Oct with 60 = +0 ----
  printf("§2 one note\n");
  on(64);
  block();
  CHECK(fr[0].trig, "note-on did not strike TRIG in its block");
  CHECK(work && last().owns_level && last().gate == 1.0f,
        "LEVEL not held open: owns %d gate %.2f", last().owns_level, last().gate);
  CHECK(fabsf(last().note - 4.0f) < 1e-4f, "E4 should be +4 semitones, got %.3f", last().note);
  CHECK(sp1_midi_active(), "not active after a note");
  block();
  CHECK(!any_trig(), "TRIG struck twice for one note");

  // ---- §3 Yarns mono, legato off (the script's default): last-note priority, every new
  // note strikes, and so does falling back to a key still held (C4) ----
  printf("§3 last-note priority, legato off\n");
  on(67);
  block();
  CHECK(any_trig() && fabsf(last().note - 7.0f) < 1e-4f, "G4 over E4: trig %d note %.2f",
        any_trig(), last().note);
  on(62);
  block();
  CHECK(any_trig() && fabsf(last().note - 2.0f) < 1e-4f, "D4 newest: note %.2f", last().note);
  off(62);
  block();
  CHECK(any_trig() && fabsf(last().note - 7.0f) < 1e-4f,
        "release D4 -> back to G4, struck again: trig %d note %.2f", any_trig(), last().note);
  off(67);
  block();
  CHECK(fabsf(last().note - 4.0f) < 1e-4f, "release G4 -> back to E4: %.2f", last().note);
  off(64);
  block();
  CHECK(last().gate == 0.0f && last().owns_level, "release: gate closes, LEVEL kept for the tail");
  lpg_gain = 0.5f;
  block();
  CHECK(last().owns_level, "LEVEL handed back while the LPG is still open");
  lpg_gain = 0.0f;
  block();
  CHECK(!last().owns_level, "LEVEL not handed back after the LPG closed");
  CHECK(fabsf(last().note - 4.0f) < 1e-4f, "pitch should stay on the last note");
  lpg_gain = 1.0f;

  // ---- §4 sustain pedal (CC 64) ----
  printf("§4 sustain pedal\n");
  on(60);
  cc(64, 127);
  off(60);
  block();
  CHECK(last().gate > 0.0f, "pedal down: a released key should keep sounding");
  cc(64, 0);
  block();
  CHECK(last().gate == 0.0f, "pedal up: the held note should end");
  reset();

  // ---- §5 CCs read by polarity, the INTELLIGENT rule (Adara, M5a test notes) ----
  // TIMBRE is per engine: centred where the engine gives F2 a centre detent, one-sided
  // where it does not. `centre` stands in for the engine's detent bits.
  printf("§5 CC offsets: centred / one-sided, 7- and 14-bit\n");
  centre = 0x2;                                // an engine where TIMBRE is bipolar
  cc(3, 127);                                  // TIMBRE, coarse only: 7-bit
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE] - 0.5f) < 1e-3f, "centred 127 -> +1/2 travel, %.4f",
        moff[SP1_MIDI_D_TIMBRE]);
  cc(3, 0);
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE] + 0.5f) < 1e-3f, "centred 0 -> -1/2 travel");
  cc(3, 64);
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE]) < 1e-4f, "centred 64 -> 0 (neutral)");
  cc(3, 96);
  block();
  CHECK(fabsf(sp1_midi_offset(SP1_MIDI_D_TIMBRE) - 0.5f * 32.0f / 63.0f) < 1e-4f,
        "centred 7-bit 96 -> %.4f", sp1_midi_offset(SP1_MIDI_D_TIMBRE));
  cc(35, 0);                                   // its fine half: now 14-bit, 96 << 7
  block();
  CHECK(fabsf(sp1_midi_offset(SP1_MIDI_D_TIMBRE) - 0.5f * 4096.0f / 8191.0f) < 1e-4f,
        "centred 14-bit 12288 -> %.4f", sp1_midi_offset(SP1_MIDI_D_TIMBRE));
  cc(35, 127);
  cc(3, 127);                                  // a new coarse half resets the fine half
  block();
  CHECK(fabsf(sp1_midi_offset(SP1_MIDI_D_TIMBRE) - 0.5f) < 1e-4f, "MSB after LSB -> 7-bit +1/2");
  cc(35, 127);
  block();
  CHECK(fabsf(sp1_midi_offset(SP1_MIDI_D_TIMBRE) - 0.5f) < 1e-4f, "16383 -> +1/2");
  cc(3, 0); cc(35, 0);
  block();
  CHECK(fabsf(sp1_midi_offset(SP1_MIDI_D_TIMBRE) + 0.5f) < 1e-4f, "0 -> -1/2");
  // Adara's round-2 case: HARMONICS on virtual analog (bipolar), fader on its centre. The
  // parameter is centre + offset, and EVERY CC value must move it: 0 reaches 0, 127 reaches
  // 1, and 32 / 96 land a quarter of the way in -- not already at the ends.
  centre = 0x1;
  {
    const uint8_t v[4] = { 0, 32, 96, 127 };
    const float want[4] = { 0.0f, 0.25f, 0.754f, 1.0f };
    for (int k = 0; k < 4; ++k) {
      cc(14, v[k]);
      block();
      const float h = 0.5f + sp1_midi_offset(SP1_MIDI_D_HARMONICS);
      CHECK(fabsf(h - want[k]) < 0.01f, "HARMONICS from centre, CC 14 = %u -> %.3f (want %.3f)",
            v[k], h, want[k]);
    }
  }
  centre = 0x2;

  centre = 0;                                  // an engine where TIMBRE is unipolar
  reset();                                     // (the catch-up across the change is §5b's)
  cc(3, 0); cc(35, 0);
  block();
  CHECK(fabsf(sp1_midi_offset(SP1_MIDI_D_TIMBRE)) < 1e-4f,
        "the same CC 0 on a unipolar engine is NO offset, %.4f",
        sp1_midi_offset(SP1_MIDI_D_TIMBRE));
  cc(3, 64);
  block();
  CHECK(fabsf(sp1_midi_offset(SP1_MIDI_D_TIMBRE) - 64.0f / 127.0f) < 1e-4f,
        "one-sided 7-bit 64 -> %.4f", sp1_midi_offset(SP1_MIDI_D_TIMBRE));
  cc(35, 0);
  block();
  CHECK(fabsf(sp1_midi_offset(SP1_MIDI_D_TIMBRE) - 8192.0f / 16383.0f) < 1e-4f,
        "one-sided 14-bit 8192 -> %.4f", sp1_midi_offset(SP1_MIDI_D_TIMBRE));
  cc(3, 127); cc(35, 127);
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE] - 1.0f) < 1e-3f, "one-sided 16383 -> +1");
  // Fixed polarities: LPG colour is one-sided, FREQUENCY centred, whatever the engine.
  cc(24, 0);
  block();
  CHECK(sp1_midi_offset(SP1_MIDI_D_LPG_COLOUR) == 0.0f, "LPG colour CC 0 -> no offset");
  cc(15, 0);
  block();
  CHECK(fabsf(sp1_midi_offset(SP1_MIDI_D_FREQUENCY) + 0.5f) < 1e-4f,
        "FREQUENCY CC 0 -> -1/2 travel (-48 st: C4 down to the bottom of the full range)");
  cc(15, 64);
  block();
  CHECK(sp1_midi_offset(SP1_MIDI_D_FREQUENCY) == 0.0f, "FREQUENCY CC 64 -> 0");
  // Nothing sent = no offset, either way.
  CHECK(sp1_midi_offset(SP1_MIDI_D_MORPH) == 0.0f, "an unsent CC is no offset");
  reset();

  // ---- §5b pickup when an engine change re-reads a CC (Adara, rounds 3 and 4) ----
  // TIMBRE centred on one engine, one-sided on the next: the SAME knob position means a
  // different offset. The offset stays through the change, and catches up with the knob
  // Plaits' way: every movement moves it the same way at once, skewed so the two meet at the
  // end the knob is heading for (round 3 held it until the knob crossed it: "too
  // unresponsive"). Expected values are pot_controller.h's formula worked by hand.
  printf("§5b CC catch-up across an engine change\n");
  centre = 0x2;                                // TIMBRE centred
  block();
  cc(3, 96);                                   // centred: +0.254
  for (int i = 0; i < 100; ++i) block();
  const float before = moff[SP1_MIDI_D_TIMBRE];
  CHECK(fabsf(before - 0.5f * 32.0f / 63.0f) < 1e-3f, "centred 96 -> %.3f", before);
  centre = 0;                                  // engine change: TIMBRE now one-sided
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE] - before) < 1e-3f,
        "no jump on the engine change: still %.3f (one-sided 96 would be 0.756)",
        moff[SP1_MIDI_D_TIMBRE]);
  CHECK(fabsf(sp1_midi_offset(SP1_MIDI_D_TIMBRE) - before) < 1e-3f, "kept while catching");
  cc(3, 40);                                   // knob 0.756 -> 0.315: down, so the offset
  for (int i = 0; i < 100; ++i) block();       // goes down too, scaled to meet it at 0:
  {                                            // 0.254 - 0.441 * 0.255 / 0.757 = 0.105
    const float v = moff[SP1_MIDI_D_TIMBRE];
    CHECK(fabsf(v - 0.105f) < 0.005f, "the knob moved down: the offset follows at once, "
          "%.3f (0.105; round 3 held 0.254)", v);
  }
  cc(3, 0);                                    // the knob's bottom: the two meet at 0
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE]) < 1e-3f, "met at the bottom: 0, %.3f",
        moff[SP1_MIDI_D_TIMBRE]);
  cc(3, 64);
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE] - 64.0f / 127.0f) < 1e-3f, "caught: tracks the knob, %.3f",
        moff[SP1_MIDI_D_TIMBRE]);
  // An offset outside the new reading's range: one-sided 0.504 flipped to centred, whose top
  // is 0.5. The range widens to it, so moving the knob to the top meets it there.
  centre = 0x2;
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE] - 64.0f / 127.0f) < 1e-3f, "kept across the flip back");
  cc(3, 127);
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE] - 0.5f) < 1e-3f, "met at the top: 0.5, %.3f",
        moff[SP1_MIDI_D_TIMBRE]);
  cc(3, 32);                                   // tracking: centred -0.25
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE] + 0.25f) < 1e-3f, "centred 32 -> -0.25, %.3f",
        moff[SP1_MIDI_D_TIMBRE]);
  // ...and below it: centred -0.25 flipped to one-sided, which bottoms out at 0. The offset is
  // already at the end a downward knob heads for, so DOWN leaves it; UP catches it up, and the
  // two meet at the top.
  centre = 0;
  cc(3, 10);
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE] + 0.25f) < 1e-3f, "below the range: still -0.25, %.3f",
        moff[SP1_MIDI_D_TIMBRE]);
  cc(3, 0);
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE] + 0.25f) < 1e-3f, "knob down further: still -0.25");
  cc(3, 64);                                   // up 0.504 on a range widened to 1.25:
  for (int i = 0; i < 100; ++i) block();       // -0.25 + 1.25 * 1.25 * 0.403 = 0.380
  {
    const float v = moff[SP1_MIDI_D_TIMBRE];
    CHECK(fabsf(v - 0.380f) < 0.005f, "the knob moved up: the offset rises with it, %.3f "
          "(0.380)", v);
  }
  cc(3, 127);
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE] - 1.0f) < 1e-3f, "met at the top: 1, %.3f",
        moff[SP1_MIDI_D_TIMBRE]);
  cc(3, 20);
  for (int i = 0; i < 100; ++i) block();
  CHECK(fabsf(moff[SP1_MIDI_D_TIMBRE] - 20.0f / 127.0f) < 1e-3f, "and tracks from there");
  // A reset clears a catch.
  centre = 0x2;
  cc(3, 127);
  for (int i = 0; i < 100; ++i) block();
  centre = 0;
  block();
  reset();
  CHECK(sp1_midi_offset(SP1_MIDI_D_TIMBRE) == 0.0f, "a disconnect leaves nothing held");
  centre = 0;

  // ---- §6 smoothing: cc_smoothing = 10 ms, a one-pole once per audio block ----
  // The message reaches the target in the first block and the smoothing from the second:
  // two smoothing steps of 5 ms after that are one time constant.
  printf("§6 smoothing (10 ms)\n");
  cc(9, 127);                                  // MORPH
  for (int i = 0; i < 3; ++i) block();
  const float s10 = moff[SP1_MIDI_D_MORPH];
  CHECK(fabsf(s10 - (1.0f - expf(-1.0f))) < 0.02f, "after one time constant: %.3f (0.632)", s10);
  CHECK(s10 > 0.0f && s10 < 1.0f, "a step must glide, not jump");
  reset();
  for (int i = 0; i < 100; ++i) block();
  CHECK(!work, "after a reset and the glide home, MIDI should be idle again");

  // ---- §7 pitch bend and RPN 0 ----
  printf("§7 pitch bend, RPN 0\n");
  send(0xE0, 0x7F, 0x7F);                      // full up, script range +-2
  block();
  CHECK(fabsf(last().note - 2.0f * 8191.0f / 8192.0f) < 1e-3f, "bend +2: %.4f", last().note);
  cc(101, 0); cc(100, 0); cc(6, 12);           // host: bend range 12
  block();
  CHECK(fabsf(last().note - 12.0f * 8191.0f / 8192.0f) < 1e-3f, "RPN 0 = 12: %.4f", last().note);
  cc(38, 50);                                  // +50 cents
  block();
  CHECK(fabsf(last().note - 12.5f * 8191.0f / 8192.0f) < 1e-3f, "12 st 50 c: %.4f", last().note);
  cc(101, 127); cc(100, 127); cc(6, 3);        // RPN null: data entry must do nothing
  block();
  CHECK(fabsf(last().note - 12.5f * 8191.0f / 8192.0f) < 1e-3f, "data entry after RPN null");
  cc(101, 0); cc(100, 0); cc(99, 1); cc(6, 3); // an NRPN select also deselects
  block();
  CHECK(fabsf(last().note - 12.5f * 8191.0f / 8192.0f) < 1e-3f, "data entry after NRPN select");
  cc(101, 0); cc(100, 0); cc(96, 0);           // increment
  block();
  struct sp1_midi_stats st;
  sp1_midi_get_stats(&st);
  CHECK(st.bend_range == 13, "data increment: %u", st.bend_range);
  cc(121, 0);                                  // reset all controllers: bend to centre
  block();
  CHECK(fabsf(last().note) < 1e-4f, "reset all controllers: bend %.4f", last().note);
  reset();
  sp1_midi_get_stats(&st);
  CHECK(st.bend_range == 2, "disconnect: bend range back to the script's, %u", st.bend_range);

  // ---- §8 channel filter, all notes off ----
  printf("§8 channel, all notes off\n");
  sp1_midi_get_stats(&st);
  const uint32_t ign0 = st.ignored;
  send(0x91, 60, 100);                         // channel 2: not ours
  block();
  sp1_midi_get_stats(&st);
  CHECK(!any_trig() && st.ignored == ign0 + 1, "channel 2 should be ignored");
  on(60); on(64);
  cc(123, 0);
  block();
  CHECK(last().gate == 0.0f, "all notes off");
  reset();

  // ---- §9 timing: a message lands at its own Plaits block ----
  printf("§9 placement inside the audio block\n");
  block(1000);                                 // previous block began at 1000
  send(0x90, 60, 100, 1500);                   // halfway to the next
  block(2000);
  int at = -1;
  for (uint32_t j = 0; j < kBlocks; ++j) { if (fr[j].trig && at < 0) at = int(j); }
  CHECK(at == 10, "half-way message should strike at block 10, struck at %d", at);
  send(0x80, 60, 0, 2500);
  send(0x90, 62, 100, 2999);
  block(3000);
  at = -1;
  for (uint32_t j = 0; j < kBlocks; ++j) { if (fr[j].trig && at < 0) at = int(j); }
  CHECK(at == 19, "a message just before the block began goes last, struck at %d", at);
  reset();

  // ---- §10 the queue: full means dropped and counted, never blocked ----
  printf("§10 queue overflow\n");
  sp1_midi_get_stats(&st);
  const uint32_t drop0 = st.dropped;
  for (int i = 0; i < 200; ++i) send(0x92, 60, 0);   // channel 3, harmless
  sp1_midi_get_stats(&st);
  CHECK(st.dropped == drop0 + 72, "128 queued, 72 dropped: %u", st.dropped - drop0);
  block();

  // ---- §11 disconnect = neutral; the pitch waits for the release ----
  printf("§11 disconnect\n");
  on(72);
  cc(3, 127);
  block();
  sp1_midi_port(false);
  block();
  CHECK(last().gate == 0.0f && last().owns_level, "pulled: key released, tail still sounding");
  CHECK(fabsf(last().note - 12.0f) < 1e-4f, "pitch must not jump during the release");
  CHECK(sp1_midi_offset(SP1_MIDI_D_TIMBRE) == 0.0f, "CC offset target not neutral");
  CHECK(sp1_midi_active(), "still active while the release sounds");
  lpg_gain = 0.0f;
  for (int i = 0; i < 100; ++i) block();
  CHECK(!sp1_midi_active() && !work, "neutral after the tail and the glide home: no work");
  CHECK(fabsf(last().note) < 1e-4f, "pitch back to +0");
  lpg_gain = 1.0f;
  sp1_midi_port(true);

  // ---- §12 through the synth: a note sounds, and LEVEL is handed back ----
  printf("§12 through sp1_synth\n");
  sp1_synth_init();
  sp1_pui_init();
  const uint16_t raw[4] = { 1850, 1850, 1850, 0 };
  sp1_pui_enter(raw);
  sp1_mui_init();
  sp1_mui_enter(raw, false);
  sp1_synth_params sp;
  sp1_pui_params(&sp);
  for (int k = 0; k < 3; ++k) sp.mrb_t_dest[k] = SP1_DEST_NONE;
  for (int k = 0; k < 4; ++k) sp.mrb_dest[k] = SP1_DEST_NONE;
  sp1_synth_set_params(&sp);
  const double idle = render_rms(40);
  on(60);
  const double held = render_rms(40);
  off(60);
  render_rms(800);                             // 4 s: the LPG closes, LEVEL is handed back
  const double after = render_rms(20);
  sp1_synth_trigger();                         // RWD: pings the LPG only if LEVEL is free
  const double ping = render_rms(40);
  printf("   rms idle %.1f, key held %.1f, after release %.1f, RWD ping %.1f\n",
         idle, held, after, ping);
  CHECK(idle < 5.0 && held > 300.0, "a held note should sound");   // 1 LSB: Plaits' DC
  CHECK(after < 5.0, "the release should end in silence");
  CHECK(ping > 100.0, "RWD after a MIDI note must ping the LPG (LEVEL handed back)");

  // ---- §13 the quantizer stands aside while MIDI is active (C8) ----
  printf("§13 quantizer bypass\n");
  sp1_pui_set_scale(0);
  sp1_pui_params(&sp);
  CHECK(sp1_midi_active() && sp.note_hold == 0,
        "MIDI active: quantizer bypassed, no latch (active %d, hold %d)",
        sp1_midi_active(), sp.note_hold);
  sp1_midi_port(false);
  sp1_synth_set_params(&sp);
  render_rms(400);                             // release ends, neutral
  sp1_pui_params(&sp);
  CHECK(!sp1_midi_active() && sp.note_hold == 1, "after disconnect the scale is back");
  sp1_midi_port(true);
  on(60);
  render_rms(2);
  sp1_pui_params(&sp);
  CHECK(sp.note_hold == 0, "a new note: bypassed again");
  off(60);
  sp1_pui_set_scale(SP1_PUI_SCALE_OFF);

  // ---- §14 stepped targets, LEVEL and FREQUENCY, in the control loop ----
  printf("§14 OCTAVE range, MODEL, LEVEL, FREQUENCY detent\n");
  sp1_pui_params(&sp);
  CHECK(sp1_pui_octave_mode() == 10 && sp.freq_per_travel == 96.0f, "full range: 96 st/travel");
  // OCTAVE range is one-sided: from the fader's mode, up only. Put the fader at LFO first.
  {
    const float sh[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
    const float st[4] = { 0.0f, 0.0f, 0.5f, 0.0f };
    sp1_pui_load_mods(sh, st);
  }
  CHECK(sp1_pui_octave_mode() == 0, "fader at LFO mode, got %d", sp1_pui_octave_mode());
  sp1_pui_params(&sp);
  CHECK(sp.freq_per_travel == 120.0f, "LFO: 120 st/travel");
  cc(102, 127);                                // a whole travel up
  render_rms(1);
  CHECK(sp1_pui_octave_mode() == 10, "OCTAVE CC 127 -> full range, got %d",
        sp1_pui_octave_mode());
  cc(102, 0);
  render_rms(1);
  CHECK(sp1_pui_octave_mode() == 0, "OCTAVE CC 0 -> the fader's own mode, got %d",
        sp1_pui_octave_mode());
  sp1_pui_default_mods();
  // MODEL is one-sided too: 0 = the selection, 127 = the last filled slot.
  int lastf = -1;
  for (int i = 0; i < SP1_ENGINE_SLOTS; ++i) {
    if (SP1_ENGINE_TABLE[i].on) lastf = i;
  }
  const int sel = sp1_pui_engine();
  const int sel_slot = sp1_pui_slot();
  cc(105, 127);
  render_rms(1);
  sp1_midi_main_tick(8);                       // first change: at once
  CHECK(sp1_pui_engine() == SP1_ENGINE_TABLE[lastf].plaits,
        "MODEL 127 -> last filled slot (%d), got engine %d", lastf, sp1_pui_engine());
  CHECK(sp1_pui_eslot() == lastf && sp1_pui_slot() == sel_slot,
        "the PLAYING slot moves (the flash's cue), the selection does not");
  {
    // The glyph is the engine you HEAR (Adara, round 2): MODEL at the top, so stepping the
    // selection on with T3 still plays -- and must still show -- the last filled slot.
    // (Not T2: from the first slot it wraps round to the last, where MODEL already is.)
    uint8_t shown[4], want[4];
    sp1_pui_engine_leds(shown);
    sp1_pui_slot_leds(lastf, want);
    CHECK(memcmp(shown, want, 4) == 0, "engine glyph = the PLAYING slot's");
    sp1_pui_engine_step(+1);
    sp1_pui_engine_leds(shown);
    sp1_pui_slot_leds(sp1_pui_eslot(), want);
    CHECK(memcmp(shown, want, 4) == 0 && sp1_pui_eslot() != sp1_pui_slot(),
          "after T3 with MODEL offset: the glyph shows the slot playing, not the selection");
    sp1_pui_engine_step(-1);                   // back to the original selection
  }
  cc(105, 0);
  render_rms(1);
  sp1_midi_main_tick(8);
  CHECK(sp1_pui_engine() == SP1_ENGINE_TABLE[lastf].plaits,
        "MODEL may change at most every 50 ms: 8 ms after the last change it must hold");
  sp1_midi_main_tick(48);
  CHECK(sp1_pui_engine() == sel, "MODEL 0 -> the selection, once 50 ms have passed");
  sp1_pui_params(&sp);
  CHECK(sp.level_patched == 0, "LEVEL fader at 0 is disconnected");
  cc(26, 127);                                 // LEVEL CC pushes the fader up
  render_rms(1);
  sp1_pui_params(&sp);
  CHECK(sp.level_patched == 1 && sp.level_pos == 0.0f,
        "LEVEL CC should connect LEVEL: patched %d pos %.2f", sp.level_patched, sp.level_pos);
  cc(26, 0);                                   // one-sided: 0 is no offset
  render_rms(1);
  sp1_pui_params(&sp);
  CHECK(sp.level_patched == 0, "LEVEL CC 0 -> back to the fader (disconnected)");
  // FREQUENCY's detent: 5 % without MIDI, 10 % with (Adara). F1 at 4 % of travel above
  // centre: outside a 5 % band (+-2.5 %), inside a 10 % one (+-5 %).
  {
    const uint16_t f1 = (uint16_t)(0.54f * 3701.0f + 0.5f);
    const uint16_t r2[4] = { f1, 1850, 1850, 0 };
    for (int i = 0; i < 200; ++i) sp1_pui_tick(8, r2, true, false, false);
    CHECK(sp1_midi_active(), "MIDI should be active for this check");
    sp1_pui_params(&sp);
    CHECK(sp.note == 60.0f, "MIDI active: F1 4 %% off centre is inside the 10 %% detent -> C4, "
          "got %.3f", sp.note);
    sp1_midi_port(false);
    render_rms(800);                           // 4 s: the release ends, MIDI goes neutral
    sp1_pui_params(&sp);
    CHECK(!sp1_midi_active(), "MIDI should be neutral 4 s after the port went");
    CHECK(sp.note > 60.5f,
          "MIDI gone: the 5 %% detent again, F1 4 %% off centre is above C4, got %.3f", sp.note);
    sp1_midi_port(true);
    const uint16_t r3[4] = { 1850, 1850, 1850, 0 };
    for (int i = 0; i < 200; ++i) sp1_pui_tick(8, r3, true, false, false);
    on(60); off(60);
    render_rms(2);
  }

  // ---- §15 Marbles, smoothed in the control loop ----
  printf("§15 Marbles RATE (centred), LENGTH (one-sided)\n");
  sp1_marbles_params mp;
  sp1_mui_params(&mp);
  const float rate0 = mp.rate;
  cc(27, 127);                                 // RATE: a whole travel up
  render_rms(1);
  sp1_midi_main_tick(1);                       // 1 ms of a 10 ms glide
  sp1_mui_params(&mp);
  CHECK(mp.rate > rate0 && mp.rate < 60.0f, "RATE glides in the control loop: %.1f", mp.rate);
  for (int i = 0; i < 50; ++i) sp1_midi_main_tick(8);
  sp1_mui_params(&mp);
  CHECK(fabsf(mp.rate - 60.0f) < 0.1f, "RATE CC 127 from centre -> +60 st, the top, %.2f",
        mp.rate);
  const int len0 = mp.length;
  cc(103, 0);                                  // LENGTH: one-sided, 0 = no offset
  render_rms(1);
  sp1_midi_main_tick(8);
  sp1_mui_params(&mp);
  CHECK(mp.length == len0, "LENGTH CC 0 -> the fader's length (%d), got %d", len0, mp.length);
  cc(103, 127);                                // stepped: at once, no glide
  render_rms(1);
  sp1_midi_main_tick(8);
  sp1_mui_params(&mp);
  CHECK(mp.length == 16, "LENGTH CC 127 -> 16 steps at once, got %d", mp.length);
  cc(27, 64); cc(103, 0);
  render_rms(1);

  printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all checks passed", fails,
         fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
