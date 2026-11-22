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
// Wider screens (2026-11-21, the owner: "wider screens (for phone etc)"; Fable's static analysis, DECISIONS.md): the
// shared pool is the EDRAM left after the main colour and depth targets. sub_82EFE138 (renderer init) asks
// sub_82EFE958(manager, {width, height, count, EDRAM base}) for a W x ((2048 - X) * 5120 / (ceil(W/80) * 320)) & ~15
// target, X = the two main targets' tiles: 1280x608 at 720p (0x2F8000 bytes), only 1600x192 at 1600x720 (the chain
// needs ~0x3B1000 bytes, so placements failed) and an underflow at 1080p. The hook below asks for 768 rows when the
// screen has more pixels than the console's. The emulated EDRAM wraps at 2048 tiles, so the colour + depth of a
// W x 720 screen must fit in it (W <= 1760; 1080p cannot); the pool's tiles above 2048 then alias the top rows of the
// main colour target, which the game redraws every frame. The 3D view widens by itself (the camera's aspect is
// screen width / height, vertical FOV). The 2D layer is a fixed 1280x720 canvas whose primitives carry an anchor byte
// (X: 1 left, 2 right, 4 centre, 8 unscaled, 0 stretch; Y the same in the high nibble), stretch by default: on a wider
// screen ds2_ui_centre centres what would stretch, so the HUD and menus keep their 16:9 shape.
//
// Diagnostics (ds2_surface_probe): every distinct render target / depth surface the game sets up (sub_829DB778, the D3D
// library's surface setup: r3 width, r4 height, r5 format, r6 multisample) with its guest call chain (the return
// addresses read from the guest stack: each frame's saved LR at its caller's stack pointer - 8, as the __savegprlr
// prologues leave it), and the render target pool placements (sub_82F07B70) with each pool's fields.

#include "ds2_hud_sprites.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cmath>
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
REXCVAR_DEFINE_BOOL(ds2_ui_centre, true, "DS2",
                    "On a screen wider than 16:9, centre the 2D elements the game would stretch (HUD, menus, text) "
                    "so they keep their 16:9 shape");
REXCVAR_DEFINE_BOOL(ds2_ui_probe, false, "DS2",
                    "Diagnostic: log each distinct 2D sprite transform call (caller, anchor flags, the first matrix "
                    "rows) once");
REXCVAR_DEFINE_INT32(ds2_gpu_pool_extra_mb, -1, "DS2",
                     "MB added to the game's 241 MB GPU pool (-1 = by the screen's pixels: 64 MB per extra console screen). Takes effect on restart");
REXCVAR_DEFINE_BOOL(ds2_surface_probe, false, "DS2",
                    "Diagnostic: log each distinct render target / depth surface the game sets up (size, format, "
                    "multisample, guest call chain) once, and the render target pool placements");

extern "C" void __imp__sub_826D86D8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82F19DC0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_829DB778(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_829FC1D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82EF0F20(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82EF1698(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82EF47E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82F07B70(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82EFE958(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82F3E4F0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82F3D538(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82F3C698(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82F3CAE0(PPCContext& __restrict ctx, uint8_t* base);

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

// EDRAM tiles (80x16 samples of 32 bits) of one 1x screen-sized target.
uint32_t ScreenTiles(uint32_t width, uint32_t height) { return ((width + 79) / 80) * ((height + 15) / 16); }

// The emulated EDRAM's tile count (the SDK's gpu_edram_tiles: 2048 = the console's 10 MB; 4096 or 8192 let a larger
// screen's colour, depth and post-processing pool sit side by side instead of wrapping onto each other).
uint32_t EdramTiles() {
  static const uint32_t tiles = [] {
    const uint32_t t = uint32_t(std::strtoul(rex::cvar::GetFlagByName("gpu_edram_tiles").c_str(), nullptr, 10));
    return t == 4096 || t == 8192 ? t : 2048u;
  }();
  return tiles;
}

// The wanted screen size (width a multiple of 16, height of 4), or 0x0 for the console's.
void WantedScreen(uint32_t& width, uint32_t& height) {
  const int32_t h = REXCVAR_GET(ds2_render_height), w = REXCVAR_GET(ds2_render_width);
  height = h > 0 ? uint32_t(h) & ~3u : kConsoleHeight;
  width = w > 0 ? (uint32_t(w) + 15) & ~15u : ((height * 16 / 9) + 15) & ~15u;
  if (width == kConsoleWidth && height == kConsoleHeight) {
    width = height = 0;
  } else if (height < 360 || width < 640 || 2 * ScreenTiles(width, height) > EdramTiles() ||
             (uint64_t(width) * height > uint64_t(kConsoleWidth) * kConsoleHeight && EdramTiles() == 2048)) {
    // More pixels than the console's need the larger EDRAM: with 2048 tiles the post-processing pool wraps onto the
    // main colour target and the top of the picture shows its contents (1600x720 on the Thor, 2026-11-21).
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      REXLOG_WARN("ds2 render size: {}x{} is not supported with {} EDRAM tiles (more pixels than 1280x720 need "
                  "gpu_edram_tiles 4096 or 8192); staying 1280x720", width, height, EdramTiles());
    }
    width = height = 0;
  }
}

// Whether the screen is wider than 16:9 (the 2D layer is centred then).
bool WideScreen() {
  static const bool wide = [] {
    uint32_t width, height;
    WantedScreen(width, height);
    return width && uint64_t(width) * 9 > uint64_t(height) * 16 + 16 * 9;
  }();
  return wide;
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

// sub_82EF0F20(app, ...): stores the app's screen rect {0, 0, w, h} at app+20 from app+228 / +232 (1280x720: the
// constructor sub_82EF14A0's defaults for config Title.Graphics.Screen.Width / Height, read by sub_82EF1698). The scene
// (sub_8253E760, the only reader, through the app at 0x8354AA90) sets that rect before drawing the screen fade
// (sub_82F1A3D8, a full-rect black quad): left at 1280x720, the fade from black covered only x 0-1280 on a wide screen
// (the right edge flashed bright for 0.7 s) and a 1280x720 block at 1080p (the owner's S25, 2026-11-21). Found by a
// Fable 5.2 static analysis; patched after the call.
REX_HOOK_RAW(sub_82EF0F20) {
  const uint32_t app = ctx.r3.u32;
  __imp__sub_82EF0F20(ctx, base);
  Resize(base, app + 28, "app screen rect");
}

// sub_82EF47E0(renderer, descriptor): copies the 32-byte descriptor (width +4, height +8; 1280x720 from its default
// constructor sub_82EF0160) and rounds the size up to multiples of 4.
REX_HOOK_RAW(sub_82EF47E0) {
  Resize(base, ctx.r4.u32 + 4, "descriptor");
  __imp__sub_82EF47E0(ctx, base);
}

// sub_829DB778(width, height, format, multisample, parameters, surface, ...): the D3D surface setup. It takes the EDRAM
// base from parameters+0 and keeps its low 12 bits in the surface's info word (+28, the RB_COLOR_INFO / RB_DEPTH_INFO
// value: base in bits 0-11). A 1920x1080 screen with gpu_edram_tiles 8192 places its post-processing pool beyond tile
// 4096, so bit 12 (the register's spare bit, read by the SDK's base_tiles()) is put back after the call.
REX_HOOK_RAW(sub_829DB778) {
  const uint32_t parameters = ctx.r7.u32, surface = ctx.r8.u32;
  const uint32_t wanted_base = (EdramTiles() > 4096 && parameters >= 0x10000u) ? Load32(base, parameters) : 0;
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
      REXLOG_INFO("ds2 surface probe: {}x{} format {:#x} msaa {} base {} from {}", width, height, format, msaa,
                  parameters >= 0x10000u ? Load32(base, parameters) : 0u, chain);
    }
  }
  __imp__sub_829DB778(ctx, base);
  if ((wanted_base & 0x1000u) && wanted_base < 8192u && surface >= 0x10000u) {
    Store32(base, surface + 28, Load32(base, surface + 28) | 0x1000u);
  }
}

// sub_829FC1D0(device, ..., render target index r5, ...): the D3D Clear. It clears a colour target through a 4x depth
// alias, writing RB_DEPTH_INFO (register 0x2002, a type-0 packet: the register number, then the value) as the target's
// info word & 0xFFF, so a target based at tile 4096 or above is cleared 4096 tiles too low. At 1080-high wide sizes
// the quarter-size post-processing target sits at 4862 (1920x1080: 3876): it kept stale contents and the scene came
// out lavender (found by Fable 5.2 from the frame traces, 2026-11-22). The register write is put right after the call:
// the commands it wrote end at the device's write pointer (+48, `stw r30,48(r31)`).
REX_HOOK_RAW(sub_829FC1D0) {
  const uint32_t device = ctx.r3.u32;
  const int32_t target = int32_t(ctx.r5.u32);
  uint32_t info = 0, start = 0;
  if (EdramTiles() > 4096 && device >= 0x10000u && target >= 0 && target < 4) {
    const uint32_t surface = Load32(base, device + (uint32_t(target) + 3204) * 4);
    if (surface >= 0x10000u) info = Load32(base, surface + 28);
    start = Load32(base, device + 48);
  }
  __imp__sub_829FC1D0(ctx, base);
  if (!(info & 0x1000u)) return;
  const uint32_t end = Load32(base, device + 48);
  // The dwords written are start+4 .. end; search back from the end (at most 1 KB, also when the write moved to a new
  // part of the buffer) for a type-0 packet that writes RB_DEPTH_INFO (alone, header 0x2002, or within a run of
  // registers: base register in bits 0-14, count - 1 in bits 16-29, bit 15 = all values to one register) with this
  // target's low 12 bits. Counted, and the first misses logged with the commands, since the S25 still showed single
  // frames of the G-buffer normals at 2336x1080 (2026-11-22, v0.1.17).
  static std::atomic<uint32_t> patched{0}, missed{0};
  const uint32_t span = (end > start && end - start <= 1024u) ? end - start : 1024u;
  bool done = false;
  for (uint32_t at = end - 4; !done && at + 4 > end - span && at >= 0x10000u; at -= 4) {
    const uint32_t header = Load32(base, at);
    if (header >> 30) continue;
    const uint32_t first = header & 0x7FFFu, count = ((header >> 16) & 0x3FFFu) + 1;
    const bool one_register = header & 0x8000u;
    if (count > 32 || first > 0x2002u || 0x2002u >= first + (one_register ? 1u : count)) continue;
    const uint32_t slot = at + 4 + (one_register ? 0u : (0x2002u - first) * 4);
    if (slot >= end + 4) continue;
    const uint32_t value = Load32(base, slot);
    if ((value & 0x1FFFu) != (info & 0xFFFu)) continue;
    Store32(base, slot, value | 0x1000u);
    done = true;
  }
  const uint32_t n = done ? patched.fetch_add(1) + 1 : 0;
  if (done && (n == 1 || !(n % 5000))) {
    REXLOG_INFO("ds2 render size: clear of the target at EDRAM {} given its bit 12 back ({} so far, {} missed)",
                info & 0x1FFFu, n, missed.load());
  }
  if (!done) {
    const uint32_t m = missed.fetch_add(1) + 1;
    if (m <= 6 || !(m % 1000)) {
      std::string words;
      const uint32_t from = end > start && end - start <= 256u ? start + 4 : end - 128;
      for (uint32_t at = from; at <= end && at >= 0x10000u; at += 4) words += fmt::format(" {:08X}", Load32(base, at));
      REXLOG_WARN("ds2 render size: clear of the target at EDRAM {} (info {:08X}) NOT patched ({} missed, {} patched); "
                  "commands {:08X}..{:08X}:{}", info & 0x1FFFu, info, m, patched.load(), start, end, words);
    }
  }
}

// sub_82F19DC0(heaps, sizes): the renderer init (sub_82EF1698) builds the engine's heap-size table on its stack (18
// words) and creates the heaps from it. Entry 12 (bytes 12-15, 0x0F100000 = 241 MB) is the GPU pool: one physical
// allocation at boot that the game's own allocator splits into render targets, textures and vertex buffers. The
// console's 720p fills most of it; a larger screen's targets left too little, so textures went missing (the tree by
// the hut drawn as flat beige surfaces) and then a vertex buffer, whose data the game copies in without checking: a
// write to address 0 (the owner's S25 at 2336x1080, 2026-11-22). The kernel used only 271 MB of its 512 MB, so the
// pool grows by 64 MB per extra console screen of pixels (2336x1080: +112 MB), in 16 MB steps.
REX_HOOK_RAW(sub_82F19DC0) {
  const uint32_t sizes = ctx.r4.u32;
  uint32_t width, height;
  WantedScreen(width, height);
  const uint64_t pixels = uint64_t(width) * height, console = uint64_t(kConsoleWidth) * kConsoleHeight;
  if (width && pixels > console && sizes >= 0x10000u && Load32(base, sizes + 12) == 0x0F100000u) {
    const int32_t forced = REXCVAR_GET(ds2_gpu_pool_extra_mb);
    const uint64_t extra_mb = forced >= 0 ? uint64_t(forced) : ((pixels - console) * 64 + console - 1) / console;
    const uint32_t extra = uint32_t(forced >= 0 ? extra_mb : (extra_mb + 15) & ~uint64_t(15)) << 20;
    Store32(base, sizes + 12, 0x0F100000u + extra);
    REXLOG_INFO("ds2 render size: GPU pool 241 MB -> {} MB for {}x{}", (0x0F100000u + extra) >> 20, width, height);
  }
  __imp__sub_82F19DC0(ctx, base);
}

// sub_826D86D8(buffer, size, usage, allocator): creates a vertex buffer (a 32-byte D3D header, then the data from the
// allocator's virtual Alloc, slot 40, align 16; allocator 0 = the default GPU allocator, entry 1 of the table at
// 0x834EC4F8). On failure it returns 0 with the header's address left 0, and model loading (sub_82F4AA28) copies into
// it anyway: the S25's write to address 0 near the hut at 2336x1080 (2026-11-22). Logs each failure (first 8) with the
// allocator's first words, to tell its pool from the kernel's physical memory.
REX_HOOK_RAW(sub_826D86D8) {
  const uint32_t size = ctx.r4.u32, given = ctx.r6.u32;
  __imp__sub_826D86D8(ctx, base);
  if (ctx.r3.u32 || !size) return;
  static std::atomic<uint32_t> failures{0};
  const uint32_t n = failures.fetch_add(1) + 1;
  if (n > 8) return;
  uint32_t allocator = given;
  if (!allocator) {
    const uint32_t table = Load32(base, 0x834EC4F0u);
    if (table >= 0x10000u) allocator = Load32(base, table + 4);
  }
  std::string words;
  if (allocator >= 0x10000u) {
    for (uint32_t i = 0; i < 16; ++i) words += fmt::format(" {:08X}", Load32(base, allocator + i * 4));
  }
  const uint32_t vtable = allocator >= 0x10000u ? Load32(base, allocator) : 0;
  REXLOG_WARN("ds2 memory: vertex buffer of {:#x} bytes not allocated ({} so far) by allocator {:08X} (vtable {:08X}, "
              "alloc {:08X}, from lr {:08X}):{}", size, n, allocator, vtable,
              vtable >= 0x10000u ? Load32(base, vtable + 40) : 0u, uint32_t(ctx.lr), words);
}

// sub_82EFE958(manager, desc {width, height, count, EDRAM base}): the post-processing chain's shared pool (see the top).
// With more pixels than the console's, ask for 768 rows (the console's 608 rows at 720p and a quarter more: the chain's
// placements scale with the width).
REX_HOOK_RAW(sub_82EFE958) {
  uint32_t width, height;
  WantedScreen(width, height);
  const uint32_t desc = ctx.r4.u32, manager = ctx.r3.u32;
  if (width && uint64_t(width) * height > uint64_t(kConsoleWidth) * kConsoleHeight) {
    const uint32_t rows = ((height * 608 / 720) * 5 / 4 + 15) & ~15u;
    REXLOG_INFO("ds2 render size: post-processing pool {}x{} (EDRAM base {}) -> {} rows", Load32(base, desc),
                Load32(base, desc + 4), Load32(base, desc + 12), rows);
    Store32(base, desc + 4, rows);
    __imp__sub_82EFE958(ctx, base);
    REXLOG_INFO("ds2 render size: pool memory {:08X} + {:X}, EDRAM {} + {} tiles", Load32(base, manager + 4 + 12),
                Load32(base, manager + 4 + 20), Load32(base, manager + 4 + 24), Load32(base, manager + 4 + 32));
    return;
  }
  __imp__sub_82EFE958(ctx, base);
}

// The 2D layer's transforms (anchor flags in r5): stretch (X flags 0) becomes centre on a wide screen.
namespace {
inline void CentreX(PPCContext& ctx) {
  static const bool centre = REXCVAR_GET(ds2_ui_centre);
  if (centre && WideScreen() && !(ctx.r5.u32 & 0x0F)) ctx.r5.u64 |= 4;
}

// ds2_ui_probe: each distinct (2D function, caller, anchor flags) once, with its time (which 2D call draws what).
void ProbeAnchor(const char* what, PPCContext& ctx) {
  static const bool probe = REXCVAR_GET(ds2_ui_probe);
  if (!probe) return;
  const std::string key = fmt::format("{} lr {:08X} flags {:02X}", what, uint32_t(ctx.lr), ctx.r5.u32 & 0xFF);
  bool fresh;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    fresh = g_seen.size() < 600 && g_seen.emplace(1, 0, 0, 0, key).second;
  }
  if (fresh) REXLOG_INFO("ds2 ui probe: {}", key);
}
}  // namespace

// Sprite transform: from the sprite draw (lr 0x82F5147C) only, not from the 2D reset (lr 0x82F3E5C8).
REX_HOOK_RAW(sub_82F3E4F0) {
  static const bool probe = REXCVAR_GET(ds2_ui_probe);
  ProbeAnchor("sprite", ctx);
  if (probe) {
    const uint32_t m = ctx.r4.u32;
    auto f = [&](uint32_t a) {
      const uint32_t u = Load32(base, a);
      float v;
      std::memcpy(&v, &u, 4);
      return v;
    };
    // Large sprites only (scale 10 or more: backgrounds, fades, bars, movies): the title screen's small animated ones
    // filled the 600 entries before the prologue movie.
    if (std::fabs(f(m)) >= 10.0f || std::fabs(f(m + 20)) >= 10.0f) {
      const std::string key = fmt::format("lr {:08X} flags {:02X} m {:g} {:g} {:g} {:g} | {:g} {:g} {:g} {:g}",
                                          uint32_t(ctx.lr), ctx.r5.u32 & 0xFF, f(m), f(m + 4), f(m + 8), f(m + 12),
                                          f(m + 16), f(m + 20), f(m + 24), f(m + 28));
      bool fresh;
      {
        std::lock_guard<std::mutex> lock(g_mutex);
        fresh = g_seen.size() < 600 && g_seen.emplace(0, 0, 0, 0, key).second;
      }
      if (fresh) REXLOG_INFO("ds2 ui probe: {}", key);
    }
  }
  // Full-screen layers keep stretching across the whole screen: the fade / background layer (a 32-unit quad scaled
  // 40 x 22.5 at 0,0 = 1280x720) and the cinematic bars (scale 48 from x -117); the probe showed no other sprite that
  // starts at the left edge with a scale above 30 (2026-11-21).
  if (uint32_t(ctx.lr) == 0x82F5147Cu) {
    const uint32_t m = ctx.r4.u32;
    ds2::hud_sprites::OnSpriteTransform(base, m);
    float sx, tx;
    const uint32_t usx = Load32(base, m), utx = Load32(base, m + 12);
    std::memcpy(&sx, &usx, 4);
    std::memcpy(&tx, &utx, 4);
    const bool full_screen = tx <= 1.0f && (sx >= 30.0f || sx <= -30.0f);
    if (!full_screen) CentreX(ctx);
  }
  __imp__sub_82F3E4F0(ctx, base);
}
// Text transform and text rectangle (sub_82F52870's text draw), and the rectangle hit test.
REX_HOOK_RAW(sub_82F3D538) {
  ProbeAnchor("text sub_82F3D538", ctx);
  CentreX(ctx);
  __imp__sub_82F3D538(ctx, base);
}
REX_HOOK_RAW(sub_82F3C698) {
  ProbeAnchor("rect sub_82F3C698", ctx);
  CentreX(ctx);
  __imp__sub_82F3C698(ctx, base);
}
REX_HOOK_RAW(sub_82F3CAE0) {
  ProbeAnchor("hit sub_82F3CAE0", ctx);
  CentreX(ctx);
  __imp__sub_82F3CAE0(ctx, base);
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
