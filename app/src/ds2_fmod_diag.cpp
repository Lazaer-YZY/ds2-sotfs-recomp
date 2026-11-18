// ds2 - DIAGNOSTIC (off by default): how FMOD's mixer sees the output's play position.
//
// The game's sound is FMOD Ex 4.40 with a polled output: the "FMOD mixer thread" wakes every millisecond (Sleep(1)
// in FMOD::Thread::callback, sub_8285C200) and runs OutputPolled::updateMixer (sub_82877D58, reached through the
// thread's function pointer). updateMixer asks the output plugin for the play cursor (its getPosition, sub_828E9D38,
// called through the description's function pointer): XAudio2's IXAudio2SourceVoice::GetState on FMOD's looping
// source voice, SamplesPlayed modulo the ring length (output + 708). It then mixes every block from the last one
// it mixed (this + 300) up to the cursor's block. If the cursor advances slower than the sound plays, XAudio2 keeps
// looping blocks FMOD has not refilled: the repeated, "static" sound (58-70 % of the submitted 256-sample frames
// identical to one of the previous four, 2026-11-18).
//
// With ds2_fmod_diag on, both functions are wrapped (they run unchanged) and every 2 s one line reports: getPosition
// calls, how often the position changed, how far it advanced (should be ~96,000 samples in 2 s at 48 kHz), the ring
// length, the smallest and largest step, and updateMixer's calls and blocks mixed. Every 10 s the next 24 positions
// are listed with their times. Both are reached only through indirect calls (ds2_register.cpp registers sub_X), so
// these hooks see every call without a new codegen.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdint>
#include <string>

REXCVAR_DEFINE_BOOL(ds2_fmod_diag, false, "DS2",
                    "DIAGNOSTIC: log FMOD's output play position (getPosition) and blocks mixed every 2 s");

extern "C" void __imp__sub_828E9D38(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82877D58(PPCContext& __restrict ctx, uint8_t* base);

namespace {

inline uint32_t Ld(const uint8_t* base, uint32_t a) {
  if (a < 0x10000u) return 0;
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a));
}

uint64_t NowUs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// Only FMOD's mixer thread calls these (single writer); the numbers are a diagnostic.
struct Stats {
  uint64_t window_start_us = 0;
  uint64_t calls = 0, changes = 0, advanced = 0;
  uint32_t last_pos = 0, ring = 0, min_step = UINT32_MAX, max_step = 0;
  bool have_last = false;
  uint64_t mixer_calls = 0, mixer_blocks = 0;
  uint32_t max_block = 0;
  int windows = 0;
  int trace_left = 0;
  std::string trace;
  uint64_t trace_t0 = 0;
} g;

void MaybeReport(uint64_t now) {
  if (!g.window_start_us) {
    g.window_start_us = now;
    return;
  }
  if (now - g.window_start_us < 2000000) return;
  const double secs = double(now - g.window_start_us) / 1e6;
  REXLOG_INFO(
      "[fmod diag] {:.2f} s: getPosition {} calls, position changed {} times, advanced {} samples ({:.0f}/s), ring "
      "{}, step min {} max {}; updateMixer {} calls, {} blocks mixed (last block seen up to {})",
      secs, g.calls, g.changes, g.advanced, double(g.advanced) / secs, g.ring, g.min_step == UINT32_MAX ? 0 : g.min_step,
      g.max_step, g.mixer_calls, g.mixer_blocks, g.max_block);
  g.calls = g.changes = g.advanced = 0;
  g.min_step = UINT32_MAX;
  g.max_step = 0;
  g.mixer_calls = g.mixer_blocks = 0;
  g.window_start_us = now;
  if (++g.windows % 5 == 0) {
    g.trace_left = 24;
    g.trace.clear();
    g.trace_t0 = now;
  }
}

}  // namespace

// Output getPosition(output, unsigned int* pcm): r3 = the output, r4 = where the cursor goes.
REX_HOOK_RAW(sub_828E9D38) {
  static const bool enabled = REXCVAR_GET(ds2_fmod_diag);
  const uint32_t output = ctx.r3.u32;
  const uint32_t pcm_ptr = ctx.r4.u32;
  __imp__sub_828E9D38(ctx, base);
  if (!enabled || !pcm_ptr) return;
  const uint64_t now = NowUs();
  const uint32_t pos = Ld(base, pcm_ptr);
  g.ring = Ld(base, output + 708);
  ++g.calls;
  if (g.have_last && pos != g.last_pos) {
    const uint32_t step = g.ring && pos < g.last_pos ? pos + g.ring - g.last_pos : pos - g.last_pos;
    ++g.changes;
    g.advanced += step;
    if (step < g.min_step) g.min_step = step;
    if (step > g.max_step) g.max_step = step;
  }
  g.last_pos = pos;
  g.have_last = true;
  if (g.trace_left > 0) {
    g.trace += fmt::format(" {}@{:.2f}", pos, double(now - g.trace_t0) / 1000.0);
    if (--g.trace_left == 0) REXLOG_INFO("[fmod diag] positions (sample@ms):{}", g.trace);
  }
  MaybeReport(now);
}

// OutputPolled::updateMixer(this): mixes from the last block mixed (this + 300) up to the cursor's block.
REX_HOOK_RAW(sub_82877D58) {
  static const bool enabled = REXCVAR_GET(ds2_fmod_diag);
  const uint32_t self = ctx.r3.u32;
  const uint32_t before = enabled ? Ld(base, self + 300) : 0;
  __imp__sub_82877D58(ctx, base);
  if (!enabled) return;
  const uint32_t after = Ld(base, self + 300);
  ++g.mixer_calls;
  if (after != before) {
    const uint32_t blocks = after > before ? after - before : after + (g.max_block + 1) - before;
    g.mixer_blocks += blocks;
  }
  if (after > g.max_block) g.max_block = after;
}
