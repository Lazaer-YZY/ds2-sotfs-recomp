// ds2 - the game's clock reads (diagnostics).
//
// QueryPerformanceCounter is sub_829C37F0 (mftb into [r3]; 98 direct callers, all through the hookable name). With
// ds2_qpc_sites_log every thread counts its calls by guest call site (lr) and logs them every 10 s: a site read ~600
// times in 10 s on the main thread at 60 fps is read once per frame, a candidate for the game's frame-time step.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#if defined(__ANDROID__) || defined(__linux__)
#include <sys/prctl.h>
#endif

REXCVAR_DEFINE_BOOL(ds2_qpc_sites_log, false, "DS2",
                    "Diagnostic: count QueryPerformanceCounter (sub_829C37F0) calls per thread and guest call site; "
                    "logged every 10 s");

extern "C" void __imp__sub_829C37F0(PPCContext& __restrict ctx, uint8_t* base);

namespace {
struct Sites {
  static constexpr int kMax = 48;
  uint32_t lr[kMax] = {};
  uint32_t count[kMax] = {};
  int used = 0;
  uint64_t calls = 0;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
}  // namespace

REX_HOOK_RAW(sub_829C37F0) {
  static const bool log = REXCVAR_GET(ds2_qpc_sites_log);
  if (log) {
    thread_local Sites s;
    const uint32_t lr = uint32_t(ctx.lr);
    int i = 0;
    while (i < s.used && s.lr[i] != lr) ++i;
    if (i == s.used && s.used < Sites::kMax) s.lr[s.used++] = lr;
    if (i < s.used) ++s.count[i];
    ++s.calls;
    const auto now = std::chrono::steady_clock::now();
    if (now - s.last >= std::chrono::seconds(10)) {
      char name[32] = {};
#if defined(__ANDROID__) || defined(__linux__)
      prctl(PR_GET_NAME, name, 0, 0, 0);
#endif
      std::string list;
      for (int k = 0; k < s.used; ++k) {
        if (s.count[k]) list += fmt::format(" {:08X}={}", s.lr[k], s.count[k]);
      }
      REXLOG_INFO("ds2 qpc: {} calls in 10 s on '{}':{}", s.calls, name, list);
      s = Sites();
    }
  }
  __imp__sub_829C37F0(ctx, base);
}
