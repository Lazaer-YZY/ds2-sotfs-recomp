// ds2 - game threads that poll faster than they need to.
//
// The DLC thread ("DLCThread_Xbox360", function sub_82F4F1C0, started through the generic thread trampoline
// sub_826EB650 with r3 = its object) runs a small state machine:
//
//   while (!object->stop /* byte +4 */) {
//     switch (object->state /* +20 */) { case 2: sub_82F4E9D0; case 4: sub_82F4EE10; case 6: sub_82F4EF28; }
//     sub_826D3D18(1000);   // Sleep((1000 + 999) / 1000) = 1 ms
//   }
//
// Idle (any other state) it still wakes every millisecond: ~930 wake-ups a second for nothing (Thor profile,
// 2026-11-18). With ds2_dlc_idle_sleep_ms > 1 the same loop runs here, unchanged except that an idle pass sleeps
// that long; a pass that did work (state 2/4/6) keeps the 1 ms. A request that moves the state machine is seen at
// most one idle sleep later. Default 1 (off): ~750 fewer wake-ups a second (the 4 ms kernel tick A/B, 2026-11-18)
// made no difference measurable against the ~0.5 W run-to-run spread, so the game keeps its own loop. The thread function is reached only through the trampoline's indirect call
// (ds2_register.cpp registers sub_82F4F1C0), so this hook sees it without a new codegen.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>

REXCVAR_DEFINE_INT32(ds2_dlc_idle_sleep_ms, 1, "DS2",
                     "The game's DLC thread polls its state every 1 ms even when idle; sleep this long per idle "
                     "pass instead (1 = the game's own loop)")
    .range(1, 100);

extern "C" void __imp__sub_82F4F1C0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82F4E9D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82F4EE10(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_82F4EF28(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_826D3D18(PPCContext& __restrict ctx, uint8_t* base);

namespace {

inline uint32_t Ld(const uint8_t* base, uint32_t a) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a));
}

}  // namespace

REX_HOOK_RAW(sub_82F4F1C0) {
  const int idle_ms = REXCVAR_GET(ds2_dlc_idle_sleep_ms);
  const uint32_t self = ctx.r3.u32;
  if (idle_ms <= 1 || self < 0x10000u) {
    __imp__sub_82F4F1C0(ctx, base);
    return;
  }
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    REXLOG_INFO("ds2: DLC thread idle passes sleep {} ms (the game's own: 1 ms)", idle_ms);
  }
  // No frame of our own is needed: the callees save their registers below r1, as they would below the original's
  // frame, and nothing here lives in guest memory.
  while (base[self + 4] == 0) {
    const uint32_t state = Ld(base, self + 20);
    ctx.r3.u64 = self;
    bool busy = true;
    switch (state) {
      case 2:
        sub_82F4E9D0(ctx, base);
        break;
      case 4:
        sub_82F4EE10(ctx, base);
        break;
      case 6:
        sub_82F4EF28(ctx, base);
        break;
      default:
        busy = false;
        break;
    }
    ctx.r3.u64 = busy ? 1000u : uint32_t(idle_ms) * 1000u;
    sub_826D3D18(ctx, base);
  }
}
