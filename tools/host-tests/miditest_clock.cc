// wakes-sp1 M5b: host checks of MIDI clock and transport into Marbles (sp1_midi.h, "MIDI
// clock"; M5 plan B8, C5, C6), against the shipped script as hostbuild.sh builds it.
//
// Ticks are pushed with stamps on the firmware's own stamp clock (SP1_MIDI_STAMP_HZ, the
// system clock: 32 768 Hz, an audio block = 163.84 counts, so the stamps are as coarse as on
// the device), exactly as the USB thread stamps them, so the audio thread places each one at
// its own Plaits block. Sections 1-4 drive the MIDI core alone; 5-9 the whole path through sp1_synth
// into Marbles, counting Marbles' t2 beats.
#include <cmath>
#include <cstdio>
#include <vector>

extern "C" {
#include "sp1_midi.h"
#include "sp1_synth.h"
#include "sp1_marbles.h"
#include "sp1_marbles_ui.h"
#include "sp1_plaits_ui.h"
}

static int fails;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  FAIL: "); \
  printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static const uint32_t kBlocks = 240u / SP1_SYNTH_BLOCK;   // Plaits blocks per 5 ms audio block
                                                         // (20 of 12 samples, 10 of 24; #32)
static const double kHz = SP1_MIDI_STAMP_HZ;            // the stamp clock
static const double kBlockCyc = kHz / 200.0;             // counts per audio block (5 ms)
static const uint32_t kJust = static_cast<uint32_t>(std::ceil(kHz / 64000.0));   // ~15 us
static uint32_t Ms(double ms) { return static_cast<uint32_t>(ms * kHz / 1000.0); }
static float moff[SP1_MIDI_AUDIO_DESTS];
static sp1_midi_clock clk;
static double now_t = kHz / 64.0;            // the stamp clock, exactly
static volatile uint32_t now_cyc = static_cast<uint32_t>(kHz / 64.0);   // ... as read
static void advance() {
  now_t += kBlockCyc;
  now_cyc = static_cast<uint32_t>(std::llround(now_t));
}
static uint32_t midi_now() { return now_cyc; }
static uint32_t lcg = 12345;

// The host's clock: ticks at `bpm`, each stamped at its time, +-jitter_cyc of USB jitter.
static double next_tick;                     // in cycles
static double tick_cyc = 0.0;                // 0 = not sending
static uint32_t jitter_cyc;

static void rt(uint8_t b, uint32_t stamp) {
  const uint8_t m[3] = { b, 0, 0 };
  sp1_midi_push(m, 1, stamp);
}
static void tempo(double bpm) {
  tick_cyc = bpm > 0.0 ? kHz * 60.0 / (bpm * 24.0) : 0.0;
}
// Queue every tick due before the audio block that starts at now_cyc.
static void host() {
  while (tick_cyc > 0.0 && next_tick < static_cast<double>(now_cyc)) {
    uint32_t j = 0;
    if (jitter_cyc) {
      lcg = lcg * 1664525u + 1013904223u;
      j = (lcg >> 8) % (2u * jitter_cyc);
    }
    const double s = next_tick + static_cast<double>(j) - static_cast<double>(jitter_cyc);
    rt(0xF8, static_cast<uint32_t>(s < static_cast<double>(now_cyc) - 1.0
                                   ? s : static_cast<double>(now_cyc) - 1.0));
    next_tick += tick_cyc;
  }
  if (tick_cyc <= 0.0) {
    next_tick = static_cast<double>(now_cyc);
  }
}
// One audio block of the MIDI core alone.
static void core() {
  advance();
  host();
  if (sp1_midi_audio_begin(now_cyc, kBlocks, 0u, moff)) {
    for (uint32_t j = 0; j < kBlocks; ++j) {
      sp1_midi_frame f;
      sp1_midi_audio_block(j, &f);
    }
  }
  sp1_midi_audio_clock(&clk);
}
// One audio block through the synth (and so Marbles).
static std::vector<int16_t> pcm(240);
static void synth() {
  advance();
  host();
  sp1_synth_render(pcm.data(), 240);
}
static void seconds(double s, void (*f)()) {
  for (int i = 0; i < static_cast<int>(s * 200.0 + 0.5); ++i) f();
}
static uint32_t bpm10() {
  sp1_midi_stats st;
  sp1_midi_get_stats(&st);
  return st.bpm10;
}
// Marbles' t2 beats over `s` seconds of synth blocks.
static uint32_t beats_over(double s) {
  const uint32_t b0 = sp1_marbles_beats();
  seconds(s, synth);
  return sp1_marbles_beats() - b0;
}
static void marbles_rate(float semitones) {
  sp1_marbles_params mp;
  sp1_mui_params(&mp);
  mp.rate = semitones;
  sp1_marbles_set_params(&mp);
}

int main() {
  static_assert(SP1_MIDI_CLOCK == 1, "the shipped script follows MIDI clock");
  sp1_midi_port(true);
  core();

  // ---- §1 tempo and position from steady ticks ----
  printf("§1 tempo and position, 120 BPM\n");
  CHECK(!clk.external, "no tick yet: not external");
  tempo(120.0);
  next_tick = now_cyc + static_cast<double>(kJust);
  core();
  core();
  CHECK(clk.external, "the first tick makes the clock external");
  seconds(2.0, core);
  CHECK(bpm10() >= 1198u && bpm10() <= 1202u, "tempo 120.0, measured %u.%u", bpm10() / 10u,
        bpm10() % 10u);
  {
    // 24 ticks a beat: one second later the position has moved two beats.
    const float b0 = clk.beats[0];
    seconds(1.0, core);
    float d = clk.beats[0] - b0;
    if (d < 0.0f) d += 48.0f;
    CHECK(fabsf(d - 2.0f) < 0.02f, "one second at 120 BPM = 2 beats, moved %.3f", d);
  }

  // ---- §2 USB jitter: +-1 ms on every tick ----
  printf("§2 +-1 ms jitter\n");
  jitter_cyc = Ms(1.0);
  tempo(97.0);
  seconds(3.0, core);
  CHECK(bpm10() >= 965u && bpm10() <= 975u, "97 BPM through +-1 ms jitter, measured %u.%u",
        bpm10() / 10u, bpm10() % 10u);
  {
    bool mono = true;
    float last = clk.beats[0];
    for (int i = 0; i < 400; ++i) {
      core();
      for (uint32_t j = 0; j < kBlocks; ++j) {
        float d = clk.beats[j] - last;
        if (d < -24.0f) d += 48.0f;          // the 48-beat wrap
        if (d < 0.0f) mono = false;
        last = clk.beats[j];
      }
    }
    CHECK(mono, "the position never runs backwards under jitter");
  }
  jitter_cyc = 0;

  // ---- §3 a stopped clock: the position waits for the next tick ----
  printf("§3 the host stops its clock, then starts it again\n");
  tempo(120.0);
  seconds(1.0, core);
  tempo(0.0);
  core();
  const float held = clk.beats[kBlocks - 1];
  seconds(1.0, core);
  {
    float d = clk.beats[kBlocks - 1] - held;
    if (d < 0.0f) d += 48.0f;
    CHECK(d <= 1.0f / 24.0f + 1e-4f, "no ticks for 1 s: at most one tick further, moved %.4f",
          d);
  }
  tempo(120.0);
  seconds(0.5, core);
  CHECK(bpm10() >= 1195u && bpm10() <= 1205u, "the pause is not averaged into the tempo: %u.%u",
        bpm10() / 10u, bpm10() % 10u);

  // ---- §4 Start arms: the next tick is beat 1 ----
  printf("§4 Start\n");
  rt(0xFA, now_cyc + kJust);                   // Start, then the next tick 10 ms later
  // Half-way into the next block, plus a fifth of a Plaits block: a stamp is as coarse as the
  // device's (30.5 us), so a tick exactly ON a Plaits block's edge may read as the one before.
  next_tick = now_cyc + kBlockCyc + 0.5 * kBlockCyc + 0.2 * kBlockCyc / kBlocks;
  core();                                      // the block holding Start
  CHECK(clk.transport == SP1_MIDI_TP_STOP, "Start stops Marbles until beat 1 (%u)",
        clk.transport);
  core();                                      // the block holding beat 1
  CHECK(clk.transport == SP1_MIDI_TP_START, "beat 1's block says START (%u)", clk.transport);
  {
    int first = -1;
    for (uint32_t j = 0; j < kBlocks; ++j) {
      if (clk.beats[j] > 0.0f && first < 0) first = static_cast<int>(j);
    }
    CHECK(first == int(kBlocks / 2), "the position holds at 0 until beat 1's own Plaits block (half way), moved "
          "at %d", first);
  }

  // ---- §5 through the synth: Marbles follows the host's tempo ----
  printf("§5 Marbles on MIDI clock\n");
  sp1_synth_set_cycle_counter(&now_cyc);
  sp1_synth_set_midi_clock(midi_now);
  sp1_synth_init();
  sp1_pui_init();
  sp1_mui_init();
  {
    sp1_synth_params sp;
    sp1_pui_params(&sp);
    for (int k = 0; k < 3; ++k) sp.mrb_t_dest[k] = SP1_DEST_NONE;
    for (int k = 0; k < 4; ++k) sp.mrb_dest[k] = SP1_DEST_NONE;
    sp1_synth_set_params(&sp);
  }
  marbles_rate(0.0f);                          // RATE centre: ratio 1, a tick a beat
  CHECK(!sp1_marbles_running(), "Marbles starts stopped");
  rt(0xFA, now_cyc + kJust);
  next_tick = now_cyc + kBlockCyc + 0.25 * kBlockCyc + 0.2 * kBlockCyc / kBlocks;   // §4
  synth();
  synth();                                     // beat 1: Marbles resets and runs
  CHECK(sp1_marbles_running(), "MIDI Start + beat 1 runs Marbles");
  {
    const uint8_t* g = sp1_marbles_gate_frames();
    int rise = -1;
    for (uint32_t j = 0; j < kBlocks; ++j) {
      if ((g[j] & 2u) && rise < 0) rise = static_cast<int>(j);
    }
    CHECK(rise == int(kBlocks / 4), "Marbles' first beat lands on beat 1's own Plaits block (a quarter in), got %d",
          rise);
  }
  {
    const uint32_t n = beats_over(10.0);
    CHECK(n >= 19u && n <= 21u, "10 s at 120 BPM, ratio 1: 20 Marbles beats, got %u", n);
  }
  tempo(90.0);
  synth();
  beats_over(1.0);
  {
    const uint32_t n = beats_over(8.0);
    CHECK(n >= 11u && n <= 13u, "8 s at 90 BPM: 12 beats, got %u", n);
  }

  // ---- §6 RATE picks the ratio (Marbles' own table, through its hysteresis) ----
  printf("§6 RATE = the ratio\n");
  tempo(120.0);
  marbles_rate(60.0f);                         // the top: x4
  beats_over(0.5);
  {
    const uint32_t n = beats_over(5.0);
    CHECK(n >= 39u && n <= 41u, "RATE at the top: 4 per beat, 40 in 5 s, got %u", n);
  }
  marbles_rate(-60.0f);                        // the bottom: x1/4
  beats_over(4.0);
  {
    const uint32_t n = beats_over(16.0);
    CHECK(n >= 7u && n <= 9u, "RATE at the bottom: one per 4 beats, 8 in 16 s, got %u", n);
  }
  marbles_rate(0.0f);
  beats_over(4.0);

  // ---- §7 Stop, Continue ----
  printf("§7 Stop and Continue\n");
  rt(0xFC, now_cyc + kJust);
  synth();
  CHECK(!sp1_marbles_running(), "MIDI Stop stops Marbles");
  CHECK(beats_over(2.0) == 0u, "stopped: no beats while the clock keeps ticking");
  rt(0xFB, now_cyc + kJust);
  synth();
  CHECK(sp1_marbles_running(), "MIDI Continue runs Marbles");
  {
    const uint32_t n = beats_over(5.0);
    CHECK(n >= 9u && n <= 11u, "after Continue it follows the clock: 10 in 5 s, got %u", n);
  }

  // ---- §8 the host's clock stops without Stop: Marbles waits ----
  printf("§8 a stalled clock\n");
  tempo(0.0);
  synth();
  CHECK(beats_over(3.0) <= 1u, "no ticks for 3 s: Marbles waits (at most the beat in flight)");
  CHECK(sp1_marbles_running(), "...still running, waiting");
  tempo(120.0);
  synth();
  {
    const uint32_t n = beats_over(5.0);
    CHECK(n >= 9u && n <= 11u, "ticks again: Marbles carries on, 10 in 5 s, got %u", n);
  }

  // ---- §9 PLAY while the host clocks Marbles, then pulling the cable ----
  printf("§9 PLAY, and the cable pulled\n");
  sp1_marbles_run(false);                      // PLAY: stop
  synth();
  CHECK(!sp1_marbles_running(), "PLAY stops it locally");
  sp1_marbles_run(true);                       // PLAY: run, on the host's clock still
  {
    const uint32_t n = beats_over(5.0);
    CHECK(n >= 9u && n <= 11u, "PLAY runs it on the host's clock: 10 in 5 s, got %u", n);
  }
  tempo(0.0);
  sp1_midi_port(false);
  synth();
  synth();
  CHECK(!sp1_marbles_running(), "cable pulled while MIDI clocked Marbles: Marbles stops (C5)");
  {
    sp1_midi_stats st;
    sp1_midi_get_stats(&st);
    CHECK(!st.clock_ext, "...and has its own clock again");
  }
  sp1_marbles_run(true);                       // PLAY: its own 120 BPM (RATE centre)
  {
    const uint32_t n = beats_over(5.0);
    CHECK(n >= 9u && n <= 11u, "PLAY afterwards: its own RATE tempo, 10 in 5 s, got %u", n);
  }
  sp1_marbles_run(false);
  sp1_midi_port(true);

  // ---- §10 a DAW's jittery clock, and the lead (Adara: Bitwig at 164 BPM) ----
  // Every tick +-8 ms late or early. Each Marbles beat is timed at its Plaits block, mapped
  // back to the time a message would have had to ARRIVE to land there (one audio block
  // earlier), and compared with the host's true beat. Wakes reads the clock
  // SP1_MIDI_OUTPUT_LATENCY_MS ahead to make up its own delay, so the beats should come
  // that much early in arrival terms -- and steady, far steadier than the ticks.
  printf("§10 164 BPM with +-8 ms of jitter: the line and the lead\n");
  synth();
  synth();
  jitter_cyc = Ms(8.0);                        // +-8 ms
  tempo(164.0);
  rt(0xFA, now_cyc + kJust);
  next_tick = now_cyc + 0.6 * kBlockCyc;       // beat 1's true time
  const double beat1 = next_tick, beat_cyc = tick_cyc * 24.0;
  std::vector<double> off;
  int beat = -1;
  bool t2_was = false;
  for (int i = 0; i < 200 * 30; ++i) {         // 30 s
    synth();
    const uint8_t* g = sp1_marbles_gate_frames();
    for (uint32_t j = 0; j < kBlocks; ++j) {
      const bool t2 = (g[j] & 2u) != 0u;
      if (t2 && !t2_was) {
        ++beat;
        const double at = static_cast<double>(now_cyc) - kBlockCyc + j * (kBlockCyc / kBlocks);
        if (beat >= 8) {                       // the line settled
          off.push_back((at - (beat1 + beat * beat_cyc)) / (kHz / 1000.0));   // ms
        }
      }
      t2_was = t2;
    }
  }
  {
    double m = 0.0, v = 0.0, lo = 1e9, hi = -1e9;
    for (double o : off) { m += o; lo = o < lo ? o : lo; hi = o > hi ? o : hi; }
    m /= off.size();
    for (double o : off) v += (o - m) * (o - m);
    const double sd = sqrt(v / off.size());
    printf("   %zu beats: %.1f ms vs the host's beat (lead %d ms), sd %.2f ms, range %.1f .. %.1f\n",
           off.size(), m, SP1_MIDI_OUTPUT_LATENCY_MS, sd, lo, hi);
    CHECK(off.size() >= 70u, "Marbles kept time for 30 s: %zu beats", off.size());
    CHECK(fabs(m + SP1_MIDI_OUTPUT_LATENCY_MS) < 1.5,
          "on average the lead early: %.1f ms (want -%d)", m, SP1_MIDI_OUTPUT_LATENCY_MS);
    CHECK(sd < 2.0, "the jitter mostly gone: sd %.2f ms against ticks of +-8 ms (sd 4.6)", sd);
    CHECK(hi - lo < 8.0, "no beat strays: spread %.1f ms", hi - lo);
  }
  CHECK(bpm10() >= 1630u && bpm10() <= 1650u, "the line's tempo through the jitter: %u.%u",
        bpm10() / 10u, bpm10() % 10u);
  jitter_cyc = 0;

  // ---- §11 MMC transport: what the OP-XY sends instead of Start / Stop ----
  printf("§11 MMC Play / Stop\n");
  {
    const uint8_t play1[4] = { 0x04, 0xF0, 0x7F, 0x7F }, play2[4] = { 0x07, 0x06, 0x02, 0xF7 };
    const uint8_t stop2[4] = { 0x07, 0x06, 0x01, 0xF7 };
    const uint8_t ident[4] = { 0x07, 0x06, 0x01, 0xF7 };    // after F0 7E 7F: not MMC
    const uint8_t uni1[4] = { 0x04, 0xF0, 0x7E, 0x7F };
    const uint8_t long1[4] = { 0x04, 0xF0, 0x43, 0x10 }, longc[4] = { 0x04, 0x01, 0x02, 0x03 };
    const uint8_t end1[4] = { 0x05, 0xF7, 0x00, 0x00 };
    CHECK(sp1_midi_mmc_feed(play1) == 0u, "a SysEx's first packet finishes nothing");
    CHECK(sp1_midi_mmc_feed(play2) == 0xF9u, "F0 7F 7F 06 02 F7 = MMC Play");
    CHECK(sp1_midi_mmc_feed(play1) == 0u && sp1_midi_mmc_feed(stop2) == 0xFDu,
          "F0 7F 7F 06 01 F7 = MMC Stop (the OP-XY's, from its log)");
    CHECK(sp1_midi_mmc_feed(uni1) == 0u && sp1_midi_mmc_feed(ident) == 1u,
          "a Universal NON-real-time SysEx is not MMC: ignored (1)");
    CHECK(sp1_midi_mmc_feed(long1) == 0u && sp1_midi_mmc_feed(longc) == 0u &&
          sp1_midi_mmc_feed(longc) == 0u && sp1_midi_mmc_feed(end1) == 1u,
          "a long SysEx is skipped to its end and ignored");
    CHECK(sp1_midi_mmc_feed(end1) == 0u, "a stray end finishes nothing");
  }
  // Through the synth. No MIDI clock (the OP-XY sends none): Play runs Marbles on its own tempo.
  tempo(0.0);
  rt(0xFC, now_cyc + kJust);
  seconds(3.0, synth);                         // > 2 s without a tick
  CHECK(!sp1_marbles_running(), "stopped before the test");
  marbles_rate(0.0f);
  rt(0xF9, now_cyc + kJust);                   // MMC Play, as sp1_usbd.c queues it
  synth();
  CHECK(sp1_marbles_running(), "MMC Play with no clock arriving runs Marbles");
  {
    sp1_midi_stats st;
    sp1_midi_get_stats(&st);
    CHECK(!st.clock_ext && st.mmc_play == 1u, "...on its OWN clock (ext %d, plays %u)",
          st.clock_ext, st.mmc_play);
    const uint32_t n = beats_over(5.0);
    CHECK(n >= 9u && n <= 11u, "...at its RATE tempo, 120 BPM: 10 in 5 s, got %u", n);
  }
  rt(0xFD, now_cyc + kJust);                   // MMC Stop
  synth();
  CHECK(!sp1_marbles_running(), "MMC Stop stops Marbles");
  // With MIDI clock arriving, Play is a Start: it waits for the next tick, beat 1.
  tempo(120.0);
  next_tick = now_cyc + static_cast<double>(kJust);
  seconds(2.0, synth);
  CHECK(!sp1_marbles_running(), "clock alone does not start it");
  rt(0xF9, now_cyc + kJust);
  next_tick = now_cyc + kBlockCyc + 0.5 * kBlockCyc;
  synth();
  CHECK(!sp1_marbles_running(), "MMC Play with clock arriving: waits for beat 1");
  synth();
  CHECK(sp1_marbles_running(), "...and runs on the next tick");
  {
    const uint32_t n = beats_over(5.0);
    CHECK(n >= 9u && n <= 11u, "on the host's clock: 10 in 5 s at 120 BPM, got %u", n);
  }
  rt(0xFD, now_cyc + kJust);
  synth();
  CHECK(!sp1_marbles_running(), "MMC Stop stops it there too");

  // ---- §12 a burst at connect (Adara's Bitwig log): no silly tempo, no restart storm ----
  printf("§12 a burst of ticks, then the real clock\n");
  {
    sp1_midi_stats st;
    sp1_midi_get_stats(&st);
    const uint32_t resets0 = st.line_resets;
    tempo(0.0);
    seconds(3.0, synth);                       // a pause: the line starts afresh
    for (int k = 0; k < 20; ++k) rt(0xF8, now_cyc + kJust + Ms(0.5 * k));   // 0.5 ms apart
    synth();
    synth();
    sp1_midi_get_stats(&st);
    CHECK(st.bpm10 < 3000u, "a burst of ticks 0.5 ms apart is no tempo: %u.%u BPM shown",
          st.bpm10 / 10u, st.bpm10 % 10u);
    tempo(120.0);
    next_tick = now_cyc + static_cast<double>(kJust);
    seconds(3.0, synth);
    sp1_midi_get_stats(&st);
    CHECK(st.line_resets - resets0 <= 2u, "the real clock after it does not restart the line "
          "over and over: %u restarts", st.line_resets - resets0);
    CHECK(st.bpm10 >= 1195u && st.bpm10 <= 1205u, "...and its tempo is read right: %u.%u",
          st.bpm10 / 10u, st.bpm10 % 10u);
  }

  printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all checks passed", fails,
         fails == 1 ? "" : "s");
  return fails ? 1 : 0;
}
