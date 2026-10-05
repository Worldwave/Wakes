// wakes-sp1 — a word-wide zero fill for Plaits' engine buffers (issue #36).
//
// The toolchain's picolibc is built for size: its memset stores ONE BYTE per loop pass,
// ~6.6 cycles a byte on this board. Plaits clears its shared engine RAM through it on
// every engine change -- Particle's 16 KB diffuser took ~1.7 ms, String's delay lines
// ~1.1 ms, inside a 2 ms audio block (logs/sp1-20261005-100440.log, -102258.log). Two
// generated overrides (firmware/CMakeLists.txt) send exactly those fills here instead:
// plaits::FxEngine::Clear() and plaits::DelayLine::Reset().
//
// ⚠️ This file is compiled with -fno-tree-loop-distribute-patterns. Without it GCC
// recognises the loops below as a fill and turns them back into a call to memset -- the
// byte loop this replaces. tools/ci/check_image.py fails an image where that happened.
// Also -fno-unroll-loops: the body is unrolled by hand below.
//
// ⚠️ Deliberately NOT a replacement for the C library's memset: that one also zeroes .bss
// before main(), where a fault is a boot loop on a device with no reset pin. This runs
// only from Plaits -- Voice::Init on the main thread, and engine changes on the audio
// thread -- and only on the buffers those two helpers own.
//
// Declared at block scope inside the two helpers, which names plaits::sp1_zero (the same
// trick as marbles::sp1_mrb_channel_range), so the overrides stay one passage each.
//
// Deliberately includes NO Zephyr headers: the DSP layer stays framework-agnostic.

#include <stddef.h>
#include <stdint.h>

namespace plaits {

void sp1_zero(void* dst, size_t n) {
  uint8_t* b = static_cast<uint8_t*>(dst);
  // Plaits' BufferAllocator hands out unaligned pointers in general: bytes up to a word
  // boundary, words, then the bytes left over.
  while (n != 0 && (reinterpret_cast<uintptr_t>(b) & 3u) != 0) {
    *b++ = 0;
    --n;
  }
  uint32_t* w = reinterpret_cast<uint32_t*>(b);
  for (; n >= 32; n -= 32) {
    w[0] = 0; w[1] = 0; w[2] = 0; w[3] = 0;
    w[4] = 0; w[5] = 0; w[6] = 0; w[7] = 0;
    w += 8;
  }
  for (; n >= 4; n -= 4) {
    *w++ = 0;
  }
  b = reinterpret_cast<uint8_t*>(w);
  while (n != 0) {
    *b++ = 0;
    --n;
  }
}

}  // namespace plaits
