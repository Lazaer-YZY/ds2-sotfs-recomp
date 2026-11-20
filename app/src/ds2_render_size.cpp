// ds2 - the game's render resolution (CTRL dialog "Render resolution": 540p), and diagnostics for larger sizes.
//
// DS2 ignores the video mode it is told about (video_mode_width/height 960x540, 1920x1080, 1600x720: its front buffer
// stayed 1280x720, 2026-11-20); its screen size comes from its own renderer setup. Game init (sub_8249C7C0) passes the
// screen rect {0, 0, 1280, 720} to sub_82EF1698, and sub_82EF47E0 copies the renderer descriptor whose default
// constructor (sub_82EF0160) sets 1280x720 at +4 / +8. Rewriting both before the originals run makes every screen-sized
// target follow (main color and depth, the D3D back buffer and front buffer, the half / quarter / eighth post-processing
// chain), and the game lays out its HUD and menus for the new size itself.
//
// ds2_render_height=540: 960x540, measured at the Things Betwixt spawn on the Thor (Stable Turnip): 6.36 M GPU cycles
// per frame instead of 7.70 (-17 %; the GPU clock fell from 615 to 475 MHz at 60 fps), screenshots PASS, HUD and menus
// laid out correctly. Larger sizes (1920x1080, or 1600x720 for a 20:9 phone) stay on the loading screen: one shared
// pool for the post-processing chain (object 407B5AF4 at 720p: 0x2F8000 bytes at B0F83000) is never set up there
// (fields all 0), every placement in it fails (sub_82F07B70 returns 0) and a thread reads through the null result. The
// size is therefore limited to the console's pixel count until that pool is enlarged; wider screens also stretch the
// menus sideways (loading screen at 1600x720), which needs the UI's aspect corrected as well.
//
// Diagnostics (ds2_surface_probe): every distinct render target / depth surface the game sets up (sub_829DB778, the D3D
// library's surface setup: r3 width, r4 height, r5 format, r6 multisample) with its guest call chain (the return
// addresses read from the guest stack: each frame's saved LR at its caller's stack pointer - 8, as the __savegprlr
// prologues leave it), and the render target pool placements (sub_82F07B70) with each pool's fields.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <tuple>

REXCVAR_DEFINE_INT32(ds2_render_height, 0, "DS2",
                     "The game's screen height (0 = the console's 720; 540 = 960x540). At most the console's pixel "
                     "count for now (larger screens leave a post-processing pool unset). Takes effect on restart");
REXCVAR_DEFINE_INT32(ds2_render_width, 0, "DS2",
                     "The game's screen width (0 = 16:9 of ds2_render_height). Takes effect on restart");
REXCVAR_DEFINE_BOOL(ds2_surface_probe, false, "DS2",
                    "Diagnostic: log each distinct render target / depth surface the game sets up (size, format, "
                    "multisample, guest call chain) once, and the render target pool placements");

extern "C" void __imp__sub_829DB778(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82EF1698(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82EF47E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82F07B70(PPCContext& __restrict ctx, uint8_t* base);

namespace {
constexpr uint32_t kConsoleWidth = 1280;
constexpr uint32_t kConsoleHeight = 720;

inline uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}

inline void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + addr, &value, 4);
}

bool PlausibleStack(uint32_t sp) { return sp >= 0x10000u && sp < 0xC0000000u && (sp & 3) == 0; }
bool PlausibleCode(uint32_t pc) { return pc >= 0x82000000u && pc < 0x84000000u; }

// The wanted screen size (width a multiple of 16, height of 4), or 0x0 for the console's.
void WantedScreen(uint32_t& width, uint32_t& height) {
  const int32_t h = REXCVAR_GET(ds2_render_height), w = REXCVAR_GET(ds2_render_width);
  height = h > 0 ? uint32_t(h) & ~3u : kConsoleHeight;
  width = w > 0 ? (uint32_t(w) + 15) & ~15u : ((height * 16 / 9) + 15) & ~15u;
  if (width == kConsoleWidth && height == kConsoleHeight) {
    width = height = 0;
  } else if (height < 360 || width < 640 || uint64_t(width) * height > uint64_t(kConsoleWidth) * kConsoleHeight) {
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      REXLOG_WARN("ds2 render size: {}x{} is not supported (360p up to the console's pixel count); staying 1280x720",
                  width, height);
    }
    width = height = 0;
  }
}

// A {width, height} pair at addr that is the console's 1280x720 becomes the wanted size.
void Resize(uint8_t* base, uint32_t addr, const char* what) {
  uint32_t width, height;
  WantedScreen(width, height);
  if (!width || !addr) return;
  if (Load32(base, addr) != kConsoleWidth || Load32(base, addr + 4) != kConsoleHeight) {
    REXLOG_INFO("ds2 render size: {} at {:08X} is {}x{}, left alone", what, addr, Load32(base, addr),
                Load32(base, addr + 4));
    return;
  }
  Store32(base, addr, width);
  Store32(base, addr + 4, height);
  REXLOG_INFO("ds2 render size: {} at {:08X} 1280x720 -> {}x{}", what, addr, width, height);
}

std::mutex g_mutex;
std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, std::string>> g_seen;
}  // namespace

// sub_82EF1698(renderer, rect): game init (sub_8249C7C0) passes the screen rect {0, 0, 1280, 720, 0}.
REX_HOOK_RAW(sub_82EF1698) {
  Resize(base, ctx.r4.u32 + 8, "screen rect");
  __imp__sub_82EF1698(ctx, base);
}

// sub_82EF47E0(renderer, descriptor): copies the 32-byte descriptor (width +4, height +8; 1280x720 from its default
// constructor sub_82EF0160) and rounds the size up to multiples of 4.
REX_HOOK_RAW(sub_82EF47E0) {
  Resize(base, ctx.r4.u32 + 4, "descriptor");
  __imp__sub_82EF47E0(ctx, base);
}

REX_HOOK_RAW(sub_829DB778) {
  static const bool probe = REXCVAR_GET(ds2_surface_probe);
  if (probe) {
    const uint32_t width = ctx.r3.u32, height = ctx.r4.u32, format = ctx.r5.u32, msaa = ctx.r6.u32;
    std::string chain = fmt::format("{:08X}", uint32_t(ctx.lr));
    uint32_t sp = ctx.r1.u32;
    for (int level = 0; level < 5 && PlausibleStack(sp); ++level) {
      const uint32_t caller_sp = Load32(base, sp);
      if (!PlausibleStack(caller_sp) || caller_sp <= sp) break;
      const uint32_t ret = Load32(base, caller_sp - 8);
      if (!PlausibleCode(ret)) break;
      chain += fmt::format(" < {:08X}", ret);
      sp = caller_sp;
    }
    bool fresh;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      fresh = g_seen.size() < 400 && g_seen.emplace(width, height, format, msaa, chain).second;
    }
    if (fresh) {
      REXLOG_INFO("ds2 surface probe: {}x{} format {:#x} msaa {} from {}", width, height, format, msaa, chain);
    }
  }
  __imp__sub_829DB778(ctx, base);
}

// sub_82F07B70(pool, ...): places a render target in a memory pool (fields +12 base, +16 cursor, +20 size; it returns
// 0 when the 4 KB-aligned size from sub_82F072E0 does not fit before +12 + +20).
REX_HOOK_RAW(sub_82F07B70) {
  static const bool probe = REXCVAR_GET(ds2_surface_probe);
  if (!probe) {
    __imp__sub_82F07B70(ctx, base);
    return;
  }
  const uint32_t pool = ctx.r3.u32, lr = uint32_t(ctx.lr);
  const uint32_t a12 = Load32(base, pool + 12), a16 = Load32(base, pool + 16), a20 = Load32(base, pool + 20);
  const uint32_t r4 = ctx.r4.u32, r5 = ctx.r5.u32, r6 = ctx.r6.u32, r7 = ctx.r7.u32, r8 = ctx.r8.u32;
  __imp__sub_82F07B70(ctx, base);
  static std::atomic<int> logged{0};
  const bool failed = (ctx.r3.u32 & 0xFF) == 0;
  if (failed || logged.fetch_add(1, std::memory_order_relaxed) < 40) {
    REXLOG_INFO("ds2 surface probe: pool {:08X} place {} lr={:08X} args {:08X} {:08X} {:08X} {:08X} {:08X}; pool "
                "+12 {:08X} +16 {:08X} +20 {:08X} -> +16 {:08X}",
                pool, failed ? "FAILED" : "ok", lr, r4, r5, r6, r7, r8, a12, a16, a20, Load32(base, pool + 16));
  }
}
