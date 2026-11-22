/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <dlfcn.h>
#include <optional>
#include <algorithm>
#include <cstdlib>
#include <rex/filesystem.h>
#include <cstdio>
#include <filesystem>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Public/ShaderLang.h>
#include <rex/thread.h>
#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/spirv_translator.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/vulkan/command_processor.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/frame_breakdown.h>
#include <time.h>
#include <rex/graphics/vulkan/pipeline_cache.h>
#include <rex/graphics/vulkan/render_target_cache.h>
#include <rex/graphics/vulkan/shader.h>
#include <rex/graphics/vulkan/shared_memory.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/types.h>
#include <rex/memory/utils.h>
#include <rex/ui/flags.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/util.h>

// Legacy backend compatibility aliases for shared readback controls.
REXCVAR_DECLARE(bool, store_shaders);

REXCVAR_DEFINE_BOOL(vulkan_reuse_texture_descriptor_sets, false, "GPU/Vulkan",
                    "Reuse a texture/sampler descriptor set written earlier in the frame when a draw binds the same "
                    "views and samplers (upstream wrote both sets for every draw)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(resolve_dump_dir, "", "GPU/Vulkan",
                      "DIAGNOSTIC: folder (relative to the executable) where every resolve of one frame is written "
                      "as raw guest memory, with index.txt. Empty = off");
REXCVAR_DEFINE_INT32(resolve_dump_every_s, 30, "GPU/Vulkan", "DIAGNOSTIC: seconds between resolve dumps");
REXCVAR_DEFINE_BOOL(vulkan_resolve_verify_lazy, false, "GPU/Vulkan",
                    "DIAGNOSTIC: with resolve_dump_dir, every lazy resolve of the dumped frame is written at once "
                    "from its fed texture and dumped, then done again eagerly and dumped right after, to compare "
                    "the two byte for byte.");
REXCVAR_DEFINE_BOOL(vulkan_direct_resolve_7e3_to_8888_verify, false, "GPU/Vulkan",
                    "DIAGNOSTIC: with resolve_dump_dir (and vulkan_resolve_feed_lazy_dump off), every direct "
                    "7e3 -> k_8_8_8_8 resolve of the dumped frame is done again the old way (EDRAM dump + full "
                    "resolve) and dumped right after it, to compare the two byte for byte.");
REXCVAR_DEFINE_INT32(resolve_dump_start_s, 0, "GPU/Vulkan",
                     "DIAGNOSTIC: no resolve dump before this many seconds after the first frame");
REXCVAR_DEFINE_INT32(resolve_dump_max, 10, "GPU/Vulkan", "DIAGNOSTIC: maximum number of resolve dumps");
REXCVAR_DEFINE_INT32(resolve_dump_max_mb, 96, "GPU/Vulkan", "DIAGNOSTIC: host buffer for one resolve dump, MB");
REXCVAR_DEFINE_BOOL(vulkan_readback_resolve, false, "GPU/Vulkan",
                    "Read render-to-texture results on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_memexport_coalesce_barriers, true, "GPU/Vulkan",
                    "Consecutive memexport draws whose vertex fetches don't read the memory the "
                    "previous ones exported share one barrier instead of one each (each ends the "
                    "render pass on a tiler).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Also given to CommandProcessor for IsReadbackMemexportEnabled's non-default check.
namespace {
constexpr bool kVulkanReadbackMemexportDefault = false;
}  // namespace

REXCVAR_DEFINE_STRING(readback_resolve_sizes, "", "GPU/Vulkan",
                      "Resolves whose destination is one of these sizes (\"WxH\" in pixels, comma-separated) are "
                      "copied back to guest memory at once, even with readback_resolve = none: for the few small "
                      "render-to-texture results a game reads on the CPU (DS2: the 128x76 character portraits of "
                      "the save list), without the cost of reading back every resolve")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_readback_memexport, kVulkanReadbackMemexportDefault, "GPU/Vulkan",
                    "Read data written by memory export in shaders on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_async_skip_incomplete_frames, true, "GPU/Vulkan",
                    "When async shader compilation is enabled, skip presenting frames that "
                    "used placeholder pipelines to avoid visible flashing")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_submit_on_primary_buffer_end, true, "GPU/Vulkan",
                    "Submit command buffer when PM4 primary buffer ends, so the GPU starts on a "
                    "frame before its end (on Turnip: steadier frame pacing, less GPU busy time). "
                    "Qualcomm's v819.2 driver showed a black title frame every 8-16 frames with "
                    "it; the Android app always runs Turnip on Adreno.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(vulkan_submit_every_draws, 0, "GPU/Vulkan",
                      "Submit the commands recorded so far after this many draws in one submission, so the GPU "
                      "starts on a frame while the command thread is still translating the rest of it (DS2 submits "
                      "~1.6 times a frame otherwise, and the GPU idled a third of a heavy frame). 0 = only at the "
                      "game's primary buffer ends and before waits.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_UINT32(vulkan_submit_before_wait_min_draws, 1, "GPU/Vulkan",
                      "Before the GPU command thread waits (out of commands, or a WAIT_REG_MEM "
                      "packet polling memory or a register), submit the commands recorded so far "
                      "if they hold at least this many draws, so the GPU works while the thread "
                      "sleeps. Lightning Returns waits once per frame for a guest memory counter; "
                      "without this the GPU idled until the wait ended. 0 = never.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(vulkan_gpu_stall_log_ms, 250, "GPU/Vulkan",
                     "Log blocking waits for submitted GPU work and frames (time between swaps) that last "
                     "at least this many ms: a GPU hang the kernel driver reset and recovered from shows as "
                     "one (the Adreno 830's GMEM hangs, 2026-11-22), long before a lost device; loads and "
                     "the app in the background do too. 0 = off.");
REXCVAR_DEFINE_BOOL(vulkan_fence_poll, false, "GPU/Vulkan",
                    "Poll in-flight submission fences with zero-timeout waits whenever a submission "
                    "begins, to reclaim resources early (Xenia's behaviour). Off: completion is "
                    "learned from the blocking frame-in-flight waits (and explicit awaits) only. "
                    "On Turnip over KGSL a zero-timeout fence wait blocks until the fence signals, "
                    "so polling made the command thread wait for the GPU before every new "
                    "submission: the Thor's Ark screen view went from 42 to 33.3 ms per frame "
                    "with it off.");
REXCVAR_DEFINE_BOOL(vulkan_depth_4x_as_1x, false, "GPU/Vulkan",
                    "Host render targets: draw a 4x MSAA depth-only draw (no pixel shader) into a "
                    "depth surface never drawn with color into the 1x surface twice as wide and "
                    "tall that the same EDRAM tiles hold (2x2 samples laid out like 2x2 pixels), "
                    "with the viewport and scissor doubled, instead of into a 4x render target "
                    "whose contents must be transferred to and from the 1x one. Lightning Returns "
                    "clears its shadow maps this way. Depth is taken at the pixel centres of the "
                    "doubled grid instead of the 4x sample positions. Not with resolution scaling. "
                    "Idea from Xendroid+ (vulkan_depth_4x_as_1x).");
REXCVAR_DEFINE_UINT32(vulkan_fence_wait_sleep_us, 0, "GPU/Vulkan",
                      "When the GPU thread must wait for a submission, poll the fences with "
                      "sleeps of this many microseconds instead of a blocking vkWaitForFences "
                      "(0 = blocking wait).");
REXCVAR_DEFINE_BOOL(vulkan_dynamic_constant_offsets, false, "GPU/Vulkan",
                    "Bind the guest constant buffers as UNIFORM_BUFFER_DYNAMIC: one descriptor set per uniform "
                    "pool page, the per-draw offsets passed as dynamic offsets (no descriptor set allocation or "
                    "write when constants change). Read once at GPU setup.");

REXCVAR_DEFINE_BOOL(vulkan_dynamic_constant_offsets_per_change_sets, false, "GPU/Vulkan",
                    "DIAGNOSTIC, with vulkan_dynamic_constant_offsets: keep the UNIFORM_BUFFER_DYNAMIC layout, but "
                    "write a descriptor set per constants change like the default path (exact offsets and ranges "
                    "in the descriptors, all dynamic offsets zero). Tells a driver problem with dynamic uniform "
                    "buffers from a problem with the per-page sets and their offsets. Read once at GPU setup.");

REXCVAR_DEFINE_BOOL(vulkan_submission_start_full_barrier, false, "GPU/Vulkan",
                    "Diagnostic: a full pipeline barrier at the start of every submission.");

REXCVAR_DEFINE_BOOL(vulkan_dynamic_rendering, true, "GPU/Vulkan",
                    "Use VK_KHR_dynamic_rendering for Vulkan GPU emulation when supported by the "
                    "device (falls back to render passes otherwise)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_INT32(gpu_log_viewport_width, 0, "GPU/Vulkan",
                     "Diagnostic: log draws whose viewport is exactly this wide into a wider render target, once per "
                     "shader pair (0 = off; -1 = every draw, the first 400 shader pairs)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(gpu_keep_aspect_pixel_shaders, "", "GPU/Vulkan",
                      "Pixel shader hashes (hex, comma-separated) whose draws keep a 16:9 picture when their viewport "
                      "is wider (e.g. a game's movie player on a wide screen): scaled up to the full width with the top "
                      "and bottom cropped, or pillarboxed with gpu_keep_aspect_pillarbox");
REXCVAR_DEFINE_BOOL(gpu_keep_aspect_pillarbox, false, "GPU/Vulkan",
                    "gpu_keep_aspect_pixel_shaders: a centred 16:9 viewport instead of filling the width");
REXCVAR_DEFINE_BOOL(vulkan_skip_no_effect_draws, false, "GPU/Vulkan",
                    "Skip draws that cannot write anything (no pixel shader, no depth / stencil test or write, no "
                    "memexport, no occlusion query). Can change while running.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(gpu_eco_shading_rate, "", "GPU",
                      "Eco mode: shade the 3D scene (depth-tested draws into full-resolution, "
                      "non-MSAA render targets) once per 2x2 (\"2x2\"), 2x1 or 1x2 pixel block "
                      "through VK_KHR_fragment_shading_rate; HUD, text and post-processing stay at "
                      "full rate, geometry edges stay sharp. Lower GPU load and power, softer "
                      "image. Empty = off. Can change while running.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(spirv_texture_exp_adjust_specialization, true, "GPU/Vulkan",
                    "With texture sign specialization: a pixel shader whose textures all have "
                    "exponent adjustment 0 gets a variant without the per-component multiply by "
                    "2^exp_adjust after every fetch (exact).");
REXCVAR_DEFINE_BOOL(gpu_float24_absolute_depth_bias, true, "GPU",
                    "kD24FS8 (float24) depth with host render targets: apply the constant "
                    "polygon offset, which the console adds to depth as an absolute value "
                    "(Direct3D 9), as a vertex shader depth offset instead of the host depth "
                    "bias. A float32 depth buffer scales the constant bias with the depth's "
                    "exponent, so the conversion (exact for guest depth 0.5-1) gave depth near 0 "
                    "a fraction of the console's bias: shadow acne (pixelated, flickering "
                    "shadows) and decal z-fighting. The slope term stays in the host depth bias.");
REXCVAR_DEFINE_BOOL(vulkan_depth_only_vertex_no_interpolators, false, "GPU/Vulkan",
                    "Draws without a pixel shader (shadow casters, depth pre-pass) use a vertex "
                    "shader variant that writes no interpolators: with pipeline libraries each "
                    "vertex shader otherwise computes every interpolator it writes, read or not.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(gpu_log_float24_depth_bias, false, "GPU",
                    "Log the first 32 distinct polygon offsets of kD24FS8 depth draws (offset, "
                    "slope scale, host depth range).");
REXCVAR_DEFINE_BOOL(vulkan_gpu_timing, false, "GPU",
                    "Log a per-frame GPU time breakdown (Vulkan timestamp queries between render "
                    "passes: draws, render target transfers, resolves, texture loads, uploads, "
                    "swap, other, presenter paint) and per-frame counts of render passes, "
                    "transfers, resolves, texture loads, draws and submissions every 10 s")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(gpu_debug_skip_vertex_shader, "", "GPU/Vulkan",
                      "Diagnostics: skip every draw whose vertex shader has one of these ucode hashes (hex, "
                      "comma-separated). Can change while running")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(gpu_debug_zero_scissor_vertex_shader, "", "GPU/Vulkan",
                      "Diagnostics: draws whose vertex shader has one of these ucode hashes (hex, comma-separated) "
                      "run with a zero-area scissor: vertex fetch, vertex shading and primitive assembly still happen, "
                      "rasterization and pixel shading do not (vertex side vs pixel side of an effect's cost). Can "
                      "change while running")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(gpu_debug_skip_pixel_shader, "", "GPU/Vulkan",
                      "Diagnostics: skip every draw whose pixel shader has one of these ucode hashes (hex, "
                      "comma-separated). Can change while running")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(vulkan_vertex_buffer_skip_same_range, false, "GPU/Vulkan",
                    "A vertex fetch whose address and size match what that fetch constant used "
                    "last (in any earlier frame) is not requested from the shared memory again. "
                    "Unsafe: vertex data the game rewrites in place is never re-uploaded. Off (as "
                    "upstream: once per frame): no measurable CPU cost on the Majula walk.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(gpu_debug_no_depth_test_pixel_shader, "", "GPU/Vulkan",
                      "Diagnostics: draws whose pixel shader has this ucode hash (hex) run without the "
                      "depth test and depth writes (does an effect fail the depth test?)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_BOOL(vulkan_lr_skip_battle_grey, false, "GPU/Vulkan",
                    "Lightning Returns, diagnostic only: leave out the battle-exit grey filter "
                    "overlay, which flickers / shows line noise on Adreno. Hides the symptom; the "
                    "cause is not known yet")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_attachment_barrier_between_passes, false, "GPU",
                    "Attachment write -> attachment read/write barrier between render "
                    "target cache render pass instances.");
REXCVAR_DEFINE_UINT32(vulkan_gpu_trace_frames_window, 4, "GPU",
                      "The 10-second GPU timing window (from start) at which vulkan_gpu_trace_frames "
                      "traces; 4 = about 40 s after start.");
REXCVAR_DEFINE_UINT32(vulkan_gpu_trace_frames, 0, "GPU",
                      "Trace this many consecutive frames (vulkan_gpu_timing), once, from "
                      "about 40 s after start.");
REXCVAR_DEFINE_UINT32(vulkan_slow_frame_ms, 30, "GPU",
                      "Log the command thread's breakdown (shader translation, pipelines, textures, "
                      "render target updates, copies, submits, fence and WAIT_REG_MEM waits) of "
                      "every frame whose time between swaps exceeds this many ms; 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
namespace {
// gpu_debug_skip_vertex_shader / gpu_debug_skip_pixel_shader, changeable while running (to attribute GPU time to an
// effect in one session at one view): the setter's thread parses a new value in the change callback and publishes it
// under a mutex with a generation number; the command thread copies it when the generation moved (it never reads the
// setting's string itself, which the setter may be rewriting).
struct DebugSkipLists {
  std::vector<uint64_t> vertex, pixel, zero_scissor_vertex;
};
std::mutex g_debug_skip_mutex;
DebugSkipLists g_debug_skip_pending;
std::atomic<uint32_t> g_debug_skip_generation{0};
std::vector<uint64_t> ParseHexList(std::string_view list) {
  std::vector<uint64_t> hashes;
  for (size_t i = 0; i < list.size();) {
    size_t j = list.find(',', i);
    if (j == std::string_view::npos) j = list.size();
    if (uint64_t h = std::strtoull(std::string(list.substr(i, j - i)).c_str(), nullptr, 16)) hashes.push_back(h);
    i = j + 1;
  }
  return hashes;
}
const DebugSkipLists& CurrentDebugSkipLists() {
  static const bool registered = [] {
    {
      std::lock_guard<std::mutex> lock(g_debug_skip_mutex);
      g_debug_skip_pending.vertex = ParseHexList(REXCVAR_GET(gpu_debug_skip_vertex_shader));
      g_debug_skip_pending.pixel = ParseHexList(REXCVAR_GET(gpu_debug_skip_pixel_shader));
      g_debug_skip_pending.zero_scissor_vertex = ParseHexList(REXCVAR_GET(gpu_debug_zero_scissor_vertex_shader));
      g_debug_skip_generation.fetch_add(1, std::memory_order_release);
    }
    rex::cvar::RegisterChangeCallback("gpu_debug_zero_scissor_vertex_shader",
                                      [](std::string_view, std::string_view value) {
                                        std::lock_guard<std::mutex> lock(g_debug_skip_mutex);
                                        g_debug_skip_pending.zero_scissor_vertex = ParseHexList(value);
                                        g_debug_skip_generation.fetch_add(1, std::memory_order_release);
                                      });
    rex::cvar::RegisterChangeCallback("gpu_debug_skip_vertex_shader", [](std::string_view, std::string_view value) {
      std::lock_guard<std::mutex> lock(g_debug_skip_mutex);
      g_debug_skip_pending.vertex = ParseHexList(value);
      g_debug_skip_generation.fetch_add(1, std::memory_order_release);
    });
    rex::cvar::RegisterChangeCallback("gpu_debug_skip_pixel_shader", [](std::string_view, std::string_view value) {
      std::lock_guard<std::mutex> lock(g_debug_skip_mutex);
      g_debug_skip_pending.pixel = ParseHexList(value);
      g_debug_skip_generation.fetch_add(1, std::memory_order_release);
    });
    return true;
  }();
  (void)registered;
  static DebugSkipLists current;
  static uint32_t seen = 0;
  const uint32_t generation = g_debug_skip_generation.load(std::memory_order_acquire);
  if (generation != seen) {
    std::lock_guard<std::mutex> lock(g_debug_skip_mutex);
    current = g_debug_skip_pending;
    seen = generation;
  }
  return current;
}

inline uint64_t CpNowNs() {
#if defined(__aarch64__)
  // The architected counter directly (a few ns): the vdso clock_gettime was ~10% of the command
  // thread's samples with these per-draw timers on (Thor, 2026-11-12). Resolution 1/CNTFRQ
  // (52 ns at 19.2 MHz), plenty for the per-frame breakdown.
  static const double ns_per_tick = [] {
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? 1e9 / double(f) : 1.0;
  }();
  uint64_t t;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
  return uint64_t(double(t) * ns_per_tick);
#else
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
#endif
}
// Feeds only the "GPU slow frame" breakdown: with vulkan_slow_frame_ms 0 (the Android app's
// "Debug logging" off) no clock reads (several per draw, one per vertex fetch).
struct CpScopedTimer {
  uint64_t& acc;
  uint64_t t0;
  explicit CpScopedTimer(uint64_t& a)
      : acc(a), t0(REXCVAR_GET(vulkan_slow_frame_ms) ? CpNowNs() : 0) {}
  ~CpScopedTimer() {
    if (t0) acc += CpNowNs() - t0;
  }
};
}  // namespace
REXCVAR_DEFINE_BOOL(vulkan_gpu_trace_all_pixel_shaders, false, "GPU",
                    "With a traced frame (vulkan_gpu_trace_frame / vulkan_gpu_trace_frames), also "
                    "log every pixel shader it used, in draw order, with draw count, primitive "
                    "types, indices, GPU time and pass (to find an effect's shader for "
                    "gpu_debug_skip_pixel_shader)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(vulkan_gpu_trace_frame, false, "GPU",
                    "With vulkan_gpu_timing, log the GPU command sequence of one frame (render "
                    "passes with their draw counts and end reasons, resolves, texture loads, "
                    "uploads) every 10 s")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(vulkan_render_area_restrict, false, "GPU",
                    "Restrict the render area of render target cache render passes (guest draws, "
                    "render target ownership transfers, resolve clears) to the EDRAM-tile-aligned "
                    "union of the scissors and rectangles their commands write, instead of the "
                    "whole EDRAM-tall framebuffer, so tile-based GPUs load and store less")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Set by the Vulkan device code (sdk/src/ui/vulkan/vulkan_device.cpp) for the Android app's GPU driver page.
extern "C" char rex_vulkan_failure_reason[256];

namespace rex::graphics::vulkan {

namespace {

// glslang default built-in resource limits.
constexpr TBuiltInResource kGlslangDefaultTBuiltInResource = {
    /* .maxLights = */ 32,
    /* .maxClipPlanes = */ 6,
    /* .maxTextureUnits = */ 32,
    /* .maxTextureCoords = */ 32,
    /* .maxVertexAttribs = */ 64,
    /* .maxVertexUniformComponents = */ 4096,
    /* .maxVaryingFloats = */ 64,
    /* .maxVertexTextureImageUnits = */ 32,
    /* .maxCombinedTextureImageUnits = */ 80,
    /* .maxTextureImageUnits = */ 32,
    /* .maxFragmentUniformComponents = */ 4096,
    /* .maxDrawBuffers = */ 32,
    /* .maxVertexUniformVectors = */ 128,
    /* .maxVaryingVectors = */ 8,
    /* .maxFragmentUniformVectors = */ 16,
    /* .maxVertexOutputVectors = */ 16,
    /* .maxFragmentInputVectors = */ 15,
    /* .minProgramTexelOffset = */ -8,
    /* .maxProgramTexelOffset = */ 7,
    /* .maxClipDistances = */ 8,
    /* .maxComputeWorkGroupCountX = */ 65535,
    /* .maxComputeWorkGroupCountY = */ 65535,
    /* .maxComputeWorkGroupCountZ = */ 65535,
    /* .maxComputeWorkGroupSizeX = */ 1024,
    /* .maxComputeWorkGroupSizeY = */ 1024,
    /* .maxComputeWorkGroupSizeZ = */ 64,
    /* .maxComputeUniformComponents = */ 1024,
    /* .maxComputeTextureImageUnits = */ 16,
    /* .maxComputeImageUniforms = */ 8,
    /* .maxComputeAtomicCounters = */ 8,
    /* .maxComputeAtomicCounterBuffers = */ 1,
    /* .maxVaryingComponents = */ 60,
    /* .maxVertexOutputComponents = */ 64,
    /* .maxGeometryInputComponents = */ 64,
    /* .maxGeometryOutputComponents = */ 128,
    /* .maxFragmentInputComponents = */ 128,
    /* .maxImageUnits = */ 8,
    /* .maxCombinedImageUnitsAndFragmentOutputs = */ 8,
    /* .maxCombinedShaderOutputResources = */ 8,
    /* .maxImageSamples = */ 0,
    /* .maxVertexImageUniforms = */ 0,
    /* .maxTessControlImageUniforms = */ 0,
    /* .maxTessEvaluationImageUniforms = */ 0,
    /* .maxGeometryImageUniforms = */ 0,
    /* .maxFragmentImageUniforms = */ 8,
    /* .maxCombinedImageUniforms = */ 8,
    /* .maxGeometryTextureImageUnits = */ 16,
    /* .maxGeometryOutputVertices = */ 256,
    /* .maxGeometryTotalOutputComponents = */ 1024,
    /* .maxGeometryUniformComponents = */ 1024,
    /* .maxGeometryVaryingComponents = */ 64,
    /* .maxTessControlInputComponents = */ 128,
    /* .maxTessControlOutputComponents = */ 128,
    /* .maxTessControlTextureImageUnits = */ 16,
    /* .maxTessControlUniformComponents = */ 1024,
    /* .maxTessControlTotalOutputComponents = */ 4096,
    /* .maxTessEvaluationInputComponents = */ 128,
    /* .maxTessEvaluationOutputComponents = */ 128,
    /* .maxTessEvaluationTextureImageUnits = */ 16,
    /* .maxTessEvaluationUniformComponents = */ 1024,
    /* .maxTessPatchComponents = */ 120,
    /* .maxPatchVertices = */ 32,
    /* .maxTessGenLevel = */ 64,
    /* .maxViewports = */ 16,
    /* .maxVertexAtomicCounters = */ 0,
    /* .maxTessControlAtomicCounters = */ 0,
    /* .maxTessEvaluationAtomicCounters = */ 0,
    /* .maxGeometryAtomicCounters = */ 0,
    /* .maxFragmentAtomicCounters = */ 8,
    /* .maxCombinedAtomicCounters = */ 8,
    /* .maxAtomicCounterBindings = */ 1,
    /* .maxVertexAtomicCounterBuffers = */ 0,
    /* .maxTessControlAtomicCounterBuffers = */ 0,
    /* .maxTessEvaluationAtomicCounterBuffers = */ 0,
    /* .maxGeometryAtomicCounterBuffers = */ 0,
    /* .maxFragmentAtomicCounterBuffers = */ 1,
    /* .maxCombinedAtomicCounterBuffers = */ 1,
    /* .maxAtomicCounterBufferSize = */ 16384,
    /* .maxTransformFeedbackBuffers = */ 4,
    /* .maxTransformFeedbackInterleavedComponents = */ 64,
    /* .maxCullDistances = */ 8,
    /* .maxCombinedClipAndCullDistances = */ 8,
    /* .maxSamples = */ 4,
    /* .maxMeshOutputVerticesNV = */ 256,
    /* .maxMeshOutputPrimitivesNV = */ 512,
    /* .maxMeshWorkGroupSizeX_NV = */ 32,
    /* .maxMeshWorkGroupSizeY_NV = */ 1,
    /* .maxMeshWorkGroupSizeZ_NV = */ 1,
    /* .maxTaskWorkGroupSizeX_NV = */ 32,
    /* .maxTaskWorkGroupSizeY_NV = */ 1,
    /* .maxTaskWorkGroupSizeZ_NV = */ 1,
    /* .maxMeshViewCountNV = */ 4,
    /* .maxDualSourceDrawBuffersEXT = */ 1,
    /* .limits = */
    {
        /* .nonInductiveForLoops = */ 1,
        /* .whileLoops = */ 1,
        /* .doWhileLoops = */ 1,
        /* .generalUniformIndexing = */ 1,
        /* .generalAttributeMatrixVectorIndexing = */ 1,
        /* .generalVaryingIndexing = */ 1,
        /* .generalSamplerIndexing = */ 1,
        /* .generalVariableIndexing = */ 1,
        /* .generalConstantMatrixVectorIndexing = */ 1,
    },
};

const char* GetSwapFxaaComputeSource(bool extreme_quality) {
  return extreme_quality ? R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_fxaa_size;
  vec2 xe_fxaa_size_inv;
};

layout(set = 0, binding = 0) uniform sampler2D xe_fxaa_source;
layout(set = 1, binding = 0, rgb10_a2) writeonly uniform image2D xe_fxaa_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);
const float kEdgeThreshold = 0.063;
const float kEdgeThresholdMin = 0.0312;
const float kSpanMax = 12.0;
const float kDirReduceMul = 0.125;
const float kDirReduceMin = 1.0 / 128.0;

float SampleLuma(vec2 uv) {
  return textureLod(xe_fxaa_source, uv, 0.0).a;
}

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, xe_fxaa_size))) {
    return;
  }

  vec2 uv = (vec2(pixel) + vec2(0.5)) * xe_fxaa_size_inv;
  vec2 texel = xe_fxaa_size_inv;
  vec4 rgbm = textureLod(xe_fxaa_source, uv, 0.0);

  float lumaM = rgbm.a;
  float lumaNW = SampleLuma(uv + vec2(-texel.x, -texel.y));
  float lumaNE = SampleLuma(uv + vec2( texel.x, -texel.y));
  float lumaSW = SampleLuma(uv + vec2(-texel.x,  texel.y));
  float lumaSE = SampleLuma(uv + vec2( texel.x,  texel.y));

  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  float lumaRange = lumaMax - lumaMin;

  vec3 result = rgbm.rgb;
  if (lumaRange >= max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max(
        (lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kDirReduceMul),
        kDirReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-kSpanMax), vec2(kSpanMax)) * texel;

    vec3 rgbA =
        0.5 *
        (textureLod(xe_fxaa_source, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
         textureLod(xe_fxaa_source, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 rgbB =
        rgbA * 0.5 +
        0.25 *
            (textureLod(xe_fxaa_source, uv + dir * -0.5, 0.0).rgb +
             textureLod(xe_fxaa_source, uv + dir * 0.5, 0.0).rgb);
    float lumaB = dot(rgbB, kLumaWeights);
    result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
  }

  imageStore(xe_fxaa_dest, ivec2(pixel), vec4(result, 1.0));
}
)"
                         : R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_fxaa_size;
  vec2 xe_fxaa_size_inv;
};

layout(set = 0, binding = 0) uniform sampler2D xe_fxaa_source;
layout(set = 1, binding = 0, rgb10_a2) writeonly uniform image2D xe_fxaa_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);
const float kEdgeThreshold = 0.166;
const float kEdgeThresholdMin = 0.0833;
const float kSpanMax = 8.0;
const float kDirReduceMul = 0.125;
const float kDirReduceMin = 1.0 / 128.0;

float SampleLuma(vec2 uv) {
  return textureLod(xe_fxaa_source, uv, 0.0).a;
}

void main() {
  uvec2 pixel = gl_GlobalInvocationID.xy;
  if (any(greaterThanEqual(pixel, xe_fxaa_size))) {
    return;
  }

  vec2 uv = (vec2(pixel) + vec2(0.5)) * xe_fxaa_size_inv;
  vec2 texel = xe_fxaa_size_inv;
  vec4 rgbm = textureLod(xe_fxaa_source, uv, 0.0);

  float lumaM = rgbm.a;
  float lumaNW = SampleLuma(uv + vec2(-texel.x, -texel.y));
  float lumaNE = SampleLuma(uv + vec2( texel.x, -texel.y));
  float lumaSW = SampleLuma(uv + vec2(-texel.x,  texel.y));
  float lumaSE = SampleLuma(uv + vec2( texel.x,  texel.y));

  float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
  float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
  float lumaRange = lumaMax - lumaMin;

  vec3 result = rgbm.rgb;
  if (lumaRange >= max(kEdgeThresholdMin, lumaMax * kEdgeThreshold)) {
    vec2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max(
        (lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * kDirReduceMul),
        kDirReduceMin);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, vec2(-kSpanMax), vec2(kSpanMax)) * texel;

    vec3 rgbA =
        0.5 *
        (textureLod(xe_fxaa_source, uv + dir * (1.0 / 3.0 - 0.5), 0.0).rgb +
         textureLod(xe_fxaa_source, uv + dir * (2.0 / 3.0 - 0.5), 0.0).rgb);
    vec3 rgbB =
        rgbA * 0.5 +
        0.25 *
            (textureLod(xe_fxaa_source, uv + dir * -0.5, 0.0).rgb +
             textureLod(xe_fxaa_source, uv + dir * 0.5, 0.0).rgb);
    float lumaB = dot(rgbB, kLumaWeights);
    result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
  }

  imageStore(xe_fxaa_dest, ivec2(pixel), vec4(result, 1.0));
}
)";
}

const char* GetSwapApplyGammaTablePixelRbSwapSource() {
  return R"(#version 450
layout(set = 0, binding = 0) uniform textureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;

layout(location = 0) out vec4 xe_apply_gamma_color;

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 255.0 + vec3(0.5)).bgr;
  xe_apply_gamma_color = vec4(
      texelFetch(xe_apply_gamma_ramp, int(source.r)).b,
      texelFetch(xe_apply_gamma_ramp, int(source.g)).g,
      texelFetch(xe_apply_gamma_ramp, int(source.b)).r,
      1.0);
}
)";
}

const char* GetSwapApplyGammaPwlPixelRbSwapSource() {
  return R"(#version 450
layout(set = 0, binding = 0) uniform utextureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;

layout(location = 0) out vec4 xe_apply_gamma_color;

float ApplyPwl(uint source_value_10b, uint channel) {
  uint source_value_base = source_value_10b >> 3u;
  uvec4 pwl = texelFetch(xe_apply_gamma_ramp, int(source_value_base * 3u + channel));
  float value = float(pwl.x) + float((source_value_10b & 7u) * pwl.y) * 0.125;
  return clamp(value * 1.52737048e-05, 0.0, 1.0);
}

void main() {
  ivec2 pixel = ivec2(gl_FragCoord.xy);
  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 1023.0 + vec3(0.5)).bgr;
  xe_apply_gamma_color = vec4(
      ApplyPwl(source.r, 0u),
      ApplyPwl(source.g, 1u),
      ApplyPwl(source.b, 2u),
      1.0);
}
)";
}

const char* GetSwapApplyGammaTableComputeRbSwapSource() {
  return R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_apply_gamma_size;
};

layout(set = 0, binding = 0) uniform textureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;
layout(set = 2, binding = 0, rgb10_a2) writeonly uniform image2D xe_apply_gamma_dest;

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), xe_apply_gamma_size))) {
    return;
  }

  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 255.0 + vec3(0.5)).bgr;
  imageStore(
      xe_apply_gamma_dest, pixel,
      vec4(texelFetch(xe_apply_gamma_ramp, int(source.r)).b,
           texelFetch(xe_apply_gamma_ramp, int(source.g)).g,
           texelFetch(xe_apply_gamma_ramp, int(source.b)).r, 1.0));
}
)";
}

const char* GetSwapApplyGammaPwlComputeRbSwapSource() {
  return R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_apply_gamma_size;
};

layout(set = 0, binding = 0) uniform utextureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;
layout(set = 2, binding = 0, rgb10_a2) writeonly uniform image2D xe_apply_gamma_dest;

float ApplyPwl(uint source_value_10b, uint channel) {
  uint source_value_base = source_value_10b >> 3u;
  uvec4 pwl = texelFetch(xe_apply_gamma_ramp, int(source_value_base * 3u + channel));
  float value = float(pwl.x) + float((source_value_10b & 7u) * pwl.y) * 0.125;
  return clamp(value * 1.52737048e-05, 0.0, 1.0);
}

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), xe_apply_gamma_size))) {
    return;
  }

  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 1023.0 + vec3(0.5)).bgr;
  imageStore(xe_apply_gamma_dest, pixel,
             vec4(ApplyPwl(source.r, 0u), ApplyPwl(source.g, 1u),
                  ApplyPwl(source.b, 2u), 1.0));
}
)";
}

const char* GetSwapApplyGammaTableFxaaLumaComputeRbSwapSource() {
  return R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_apply_gamma_size;
};

layout(set = 0, binding = 0) uniform textureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;
layout(set = 2, binding = 0, rgba16f) writeonly uniform image2D xe_apply_gamma_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), xe_apply_gamma_size))) {
    return;
  }

  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 255.0 + vec3(0.5)).bgr;
  vec3 rgb = vec3(texelFetch(xe_apply_gamma_ramp, int(source.r)).b,
                  texelFetch(xe_apply_gamma_ramp, int(source.g)).g,
                  texelFetch(xe_apply_gamma_ramp, int(source.b)).r);
  imageStore(xe_apply_gamma_dest, pixel, vec4(rgb, dot(rgb, kLumaWeights)));
}
)";
}

const char* GetSwapApplyGammaPwlFxaaLumaComputeRbSwapSource() {
  return R"(#version 450
layout(local_size_x = 16, local_size_y = 8, local_size_z = 1) in;

layout(push_constant) uniform XeApplyGammaRampConstants {
  uvec2 xe_apply_gamma_size;
};

layout(set = 0, binding = 0) uniform utextureBuffer xe_apply_gamma_ramp;
layout(set = 1, binding = 0) uniform texture2D xe_apply_gamma_source;
layout(set = 2, binding = 0, rgba16f) writeonly uniform image2D xe_apply_gamma_dest;

const vec3 kLumaWeights = vec3(0.299, 0.587, 0.114);

float ApplyPwl(uint source_value_10b, uint channel) {
  uint source_value_base = source_value_10b >> 3u;
  uvec4 pwl = texelFetch(xe_apply_gamma_ramp, int(source_value_base * 3u + channel));
  float value = float(pwl.x) + float((source_value_10b & 7u) * pwl.y) * 0.125;
  return clamp(value * 1.52737048e-05, 0.0, 1.0);
}

void main() {
  ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
  if (any(greaterThanEqual(uvec2(pixel), xe_apply_gamma_size))) {
    return;
  }

  uvec3 source = uvec3(texelFetch(xe_apply_gamma_source, pixel, 0).rgb * 1023.0 + vec3(0.5)).bgr;
  vec3 rgb = vec3(ApplyPwl(source.r, 0u), ApplyPwl(source.g, 1u),
                  ApplyPwl(source.b, 2u));
  imageStore(xe_apply_gamma_dest, pixel, vec4(rgb, dot(rgb, kLumaWeights)));
}
)";
}

bool CompileGlslToSpirvInternal(EShLanguage stage, std::string_view source,
                                std::vector<uint32_t>& spirv_out, std::string& error_out) {
  static std::once_flag glslang_initialize_once;
  std::call_once(glslang_initialize_once, []() { glslang::InitializeProcess(); });

  const char* source_c_str = source.data();
  glslang::TShader shader(stage);
  shader.setStrings(&source_c_str, 1);
  shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 450);
  shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
  shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

  EShMessages messages = EShMessages(EShMsgSpvRules | EShMsgVulkanRules);
  if (!shader.parse(&kGlslangDefaultTBuiltInResource, 450, false, messages)) {
    error_out = "glslang shader parse failed";
    if (const char* shader_log = shader.getInfoLog();
        shader_log != nullptr && shader_log[0] != '\0') {
      error_out += ": ";
      error_out += shader_log;
    }
    return false;
  }

  glslang::TProgram program;
  program.addShader(&shader);
  if (!program.link(messages)) {
    error_out = "glslang program link failed";
    if (const char* program_log = program.getInfoLog();
        program_log != nullptr && program_log[0] != '\0') {
      error_out += ": ";
      error_out += program_log;
    }
    return false;
  }

  const glslang::TIntermediate* intermediate = program.getIntermediate(stage);
  if (intermediate == nullptr) {
    error_out = "glslang produced no stage intermediate";
    return false;
  }

  glslang::SpvOptions spv_options = {};
  spv_options.disableOptimizer = true;
  spv_options.optimizeSize = false;
  glslang::GlslangToSpv(*intermediate, spirv_out, &spv_options);
  if (spirv_out.empty()) {
    error_out = "glslang produced empty SPIR-V";
    return false;
  }
  return true;
}

}  // namespace

// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/vulkan_spirv/apply_gamma_pwl_cs.h"
#include "../shaders/vulkan_spirv/apply_gamma_pwl_fxaa_luma_ps.h"
#include "../shaders/vulkan_spirv/apply_gamma_pwl_fxaa_luma_cs.h"
#include "../shaders/vulkan_spirv/apply_gamma_pwl_ps.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_cs.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_fxaa_luma_ps.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_fxaa_luma_cs.h"
#include "../shaders/vulkan_spirv/apply_gamma_table_ps.h"
#include "../shaders/vulkan_spirv/fullscreen_cw_vs.h"
#include "../shaders/vulkan_spirv/resolve_downscale_cs.h"
}  // namespace shaders

const VkDescriptorPoolSize VulkanCommandProcessor::kDescriptorPoolSizeUniformBuffer = {
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    SpirvShaderTranslator::kConstantBufferCount* kLinkedTypeDescriptorPoolSetCount};

const VkDescriptorPoolSize VulkanCommandProcessor::kDescriptorPoolSizeUniformBufferDynamic = {
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
    SpirvShaderTranslator::kConstantBufferCount* kDynamicConstantsDescriptorPoolSetCount};

namespace {
// vulkan_dynamic_constant_offsets: the range of a dynamic uniform buffer descriptor is fixed for all offsets, so
// it's the largest size of each constant buffer (256 float constants for either stage). All are far below the
// minimum maxUniformBufferRange (16384).
VkDeviceSize GetDynamicConstantBufferRange(uint32_t constant_buffer_index) {
  switch (constant_buffer_index) {
    case SpirvShaderTranslator::kConstantBufferSystem:
      return sizeof(SpirvShaderTranslator::SystemConstants);
    case SpirvShaderTranslator::kConstantBufferFloatVertex:
    case SpirvShaderTranslator::kConstantBufferFloatPixel:
      return sizeof(float) * 4 * 256;
    case SpirvShaderTranslator::kConstantBufferBoolLoop:
      return sizeof(uint32_t) * (8 + 32);
    case SpirvShaderTranslator::kConstantBufferFetch:
      return sizeof(uint32_t) * 6 * 32;
    default:
      return 0;
  }
}
}  // namespace

const VkDescriptorPoolSize VulkanCommandProcessor::kDescriptorPoolSizeStorageBuffer = {
    VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kLinkedTypeDescriptorPoolSetCount};

// 2x descriptors for texture images because of unsigned and signed bindings.
const VkDescriptorPoolSize VulkanCommandProcessor::kDescriptorPoolSizeTextures[2] = {
    {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2 * kLinkedTypeDescriptorPoolSetCount},
    {VK_DESCRIPTOR_TYPE_SAMPLER, kLinkedTypeDescriptorPoolSetCount},
};

VulkanCommandProcessor::VulkanCommandProcessor(VulkanGraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state),
      deferred_command_buffer_(*this),
      transient_descriptor_allocator_uniform_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeUniformBuffer, 1, kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_storage_buffer_(
          static_cast<const ui::vulkan::VulkanProvider*>(graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeStorageBuffer, 1, kLinkedTypeDescriptorPoolSetCount),
      transient_descriptor_allocator_uniform_buffer_dynamic_(
          static_cast<const ui::vulkan::VulkanProvider*>(graphics_system->provider())
              ->vulkan_device(),
          &kDescriptorPoolSizeUniformBufferDynamic, 1, kDynamicConstantsDescriptorPoolSetCount),
      transient_descriptor_allocator_textures_(
          static_cast<const ui::vulkan::VulkanProvider*>(graphics_system->provider())
              ->vulkan_device(),
          kDescriptorPoolSizeTextures, uint32_t(rex::countof(kDescriptorPoolSizeTextures)),
          kLinkedTypeDescriptorPoolSetCount) {
  legacy_readback_memexport_cvar_name_ = "vulkan_readback_memexport";
  legacy_readback_memexport_cvar_default_ = kVulkanReadbackMemexportDefault;
}

VulkanCommandProcessor::~VulkanCommandProcessor() = default;

void VulkanCommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  InvalidateAllVertexBufferResidency();
  cache_clear_requested_ = true;
}

void VulkanCommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void VulkanCommandProcessor::InvalidateAllVertexBufferResidency() {
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  for (VertexBufferState& state : vertex_buffer_states_) {
    state.address = UINT32_MAX;
    state.size = UINT32_MAX;
  }
}

void VulkanCommandProcessor::InvalidateVertexBufferResidency(uint32_t vfetch_index) {
  if (vfetch_index >= vertex_buffer_states_.size()) {
    return;
  }
  vertex_buffers_in_sync_[vfetch_index >> 6] &= ~(uint64_t(1) << (vfetch_index & 63));
}

void VulkanCommandProcessor::InvalidateVertexBufferResidencyRange(uint32_t first_vfetch,
                                                                  uint32_t last_vfetch) {
  if (first_vfetch > last_vfetch) {
    std::swap(first_vfetch, last_vfetch);
  }
  if (first_vfetch >= vertex_buffer_states_.size()) {
    return;
  }
  last_vfetch = std::min(last_vfetch, uint32_t(vertex_buffer_states_.size() - 1));
  for (uint32_t vfetch_index = first_vfetch; vfetch_index <= last_vfetch; ++vfetch_index) {
    InvalidateVertexBufferResidency(vfetch_index);
  }
}

void VulkanCommandProcessor::LogPipelineCountersIfDue() {
  uint64_t now_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
  if (!pipeline_counters_last_log_ns_) {
    pipeline_counters_last_log_ns_ = now_ns;
    return;
  }
  if (now_ns - pipeline_counters_last_log_ns_ < uint64_t(10) * 1000000000) {
    return;
  }
  pipeline_counters_last_log_ns_ = now_ns;
  VulkanPipelineCache::DrawTimeCounters counters = pipeline_cache_->TakeDrawTimeCounters();
  if (counters.pipelines_queued || counters.pipelines_sync || counters.translations ||
      counters.translations_async || counters.libraries_created || counters.pipelines_linked ||
      counters.pipelines_linked_sync ||
      counter_draws_skipped_pending_ ||
      counter_frames_held_) {
    REXGPU_INFO(
        "Pipelines (10 s): {} queued for the creation threads, {} created synchronously on the "
        "GPU thread ({:.1f} ms), {} shader translations on the GPU thread ({:.1f} ms) and {} on "
        "the creation threads ({:.1f} ms), {} draws "
        "skipped while pending, {} of {} frames held; {} libraries compiled ({:.0f} ms), {} "
        "pipelines linked ({:.0f} ms incl. library compiles), {} linked on the GPU thread from "
        "cached libraries ({:.1f} ms)",
        counters.pipelines_queued, counters.pipelines_sync,
        double(counters.pipelines_sync_us) / 1000.0, counters.translations,
        double(counters.translations_us) / 1000.0, counters.translations_async,
        double(counters.translations_async_us) / 1000.0, counter_draws_skipped_pending_,
        counter_frames_held_, counter_frames_held_ + counter_frames_presented_,
        counters.libraries_created, double(counters.libraries_created_us) / 1000.0,
        counters.pipelines_linked, double(counters.pipelines_linked_us) / 1000.0,
        counters.pipelines_linked_sync, double(counters.pipelines_linked_sync_us) / 1000.0);
  }
  if (counters.sign_specialized_draws || counters.sign_runtime_draws ||
      counters.sign_specialized_created) {
    REXGPU_INFO(
        "Texture sign specialization (10 s): {} draws specialized, {} with the runtime variant "
        "while theirs was pending or failed, {} variants created",
        counters.sign_specialized_draws, counters.sign_runtime_draws,
        counters.sign_specialized_created);
  }
  counter_draws_skipped_pending_ = 0;
  counter_frames_held_ = 0;
  counter_frames_presented_ = 0;
}

void VulkanCommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                     uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
}

bool VulkanCommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                                uint32_t packet, uint32_t count) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }

  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  uint32_t sample_count_addr = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  auto* sample_counts =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_addr);
  if (!sample_counts) {
    DisableHostOcclusionQueries();
    return true;
  }

  auto write_fallback_result = [sample_counts]() -> bool {
    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
    if (fake_sample_count < 0) {
      return true;
    }
    bool is_end_via_z_pass =
        sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;
    bool is_end_via_z_fail =
        sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;
    std::memset(sample_counts, 0, sizeof(xenos::xe_gpu_depth_sample_counts));
    if (is_end_via_z_pass || is_end_via_z_fail) {
      sample_counts->ZPass_A = fake_sample_count;
      sample_counts->Total_A = fake_sample_count;
    }
    return true;
  };

  bool is_end_via_z_pass =
      sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;
  bool is_end_via_z_fail =
      sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;
  bool is_end = is_end_via_z_pass || is_end_via_z_fail;

  if (!is_end) {
    if (active_occlusion_query_.valid &&
        active_occlusion_query_.sample_count_address != sample_count_addr) {
      DisableHostOcclusionQueries();
      return write_fallback_result();
    }
    if (!BeginGuestOcclusionQuery(sample_count_addr)) {
      return write_fallback_result();
    }
    return true;
  }

  if (!active_occlusion_query_.valid ||
      active_occlusion_query_.sample_count_address != sample_count_addr) {
    DisableHostOcclusionQueries();
    return write_fallback_result();
  }

  if (!EndGuestOcclusionQuery(sample_count_addr)) {
    return write_fallback_result();
  }
  return true;
}

std::string VulkanCommandProcessor::GetWindowTitleText() const {
  std::ostringstream title;
  title << "Vulkan";
  if (render_target_cache_) {
    switch (render_target_cache_->GetPath()) {
      case RenderTargetCache::Path::kHostRenderTargets:
        title << " - FBO";
        break;
      case RenderTargetCache::Path::kPixelShaderInterlock:
        title << " - FSI";
        break;
      default:
        break;
    }
    uint32_t draw_resolution_scale_x =
        texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
    uint32_t draw_resolution_scale_y =
        texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
    if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
      title << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
    }
  }
  title << " - HEAVILY INCOMPLETE, early development";
  return title.str();
}

bool VulkanCommandProcessor::CompileGlslToSpirv(VkShaderStageFlagBits stage,
                                                std::string_view source,
                                                std::vector<uint32_t>& spirv_out,
                                                std::string& error_out) const {
  EShLanguage glslang_stage;
  switch (stage) {
    case VK_SHADER_STAGE_VERTEX_BIT:
      glslang_stage = EShLangVertex;
      break;
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
      glslang_stage = EShLangTessControl;
      break;
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT:
      glslang_stage = EShLangTessEvaluation;
      break;
    case VK_SHADER_STAGE_GEOMETRY_BIT:
      glslang_stage = EShLangGeometry;
      break;
    case VK_SHADER_STAGE_FRAGMENT_BIT:
      glslang_stage = EShLangFragment;
      break;
    case VK_SHADER_STAGE_COMPUTE_BIT:
      glslang_stage = EShLangCompute;
      break;
    default:
      error_out = fmt::format("Unsupported Vulkan shader stage mask {}", uint32_t(stage));
      return false;
  }
  return CompileGlslToSpirvInternal(glslang_stage, source, spirv_out, error_out);
}

bool VulkanCommandProcessor::SetupContext() {
  // A failed setup ends in rex::FatalError (an abort) on this thread: leave a reason for the Android app, whose abort
  // handler saves it for the GPU driver page of the next start, unless a more specific one was noted already.
  struct SetupFailureNote {
    VulkanCommandProcessor* self;
    bool ok = false;
    ~SetupFailureNote() {
      if (ok || rex_vulkan_failure_reason[0]) return;
      const ui::vulkan::VulkanDevice* device = self->GetVulkanDevice();
      std::snprintf(rex_vulkan_failure_reason, sizeof(rex_vulkan_failure_reason),
                    "%s: the renderer could not be set up", device ? device->properties().deviceName : "Vulkan");
    }
  } setup_failure_note{this};
  // Boot time per setup stage (one line, once), to see what grows with caches.
  const auto setup_start_time = std::chrono::steady_clock::now();
  auto setup_ms_since_start = [&setup_start_time]() {
    return double(std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - setup_start_time)
                      .count()) /
           1000.0;
  };
  double setup_ms_shared_memory = 0.0, setup_ms_render_targets = 0.0, setup_ms_pipelines = 0.0,
         setup_ms_textures = 0.0;
  if (!CommandProcessor::SetupContext()) {
    REXGPU_ERROR("Failed to initialize base command processor context");
    return false;
  }
  InvalidateAllVertexBufferResidency();

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const ui::vulkan::VulkanDevice::Properties& device_properties = vulkan_device->properties();
  REXGPU_INFO("VulkanCommandProcessor: mid-frame submissions {}",
              REXCVAR_GET(vulkan_submit_on_primary_buffer_end) ? "on" : "off");

  // The unconditional inclusion of the vertex shader stage also covers the case
  // of manual index / factor buffer fetch (the system constants and the shared
  // memory are needed for that) in the tessellation vertex shader when
  // fullDrawIndexUint32 is not supported.
  guest_shader_pipeline_stages_ =
      VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  guest_shader_vertex_stages_ = VK_SHADER_STAGE_VERTEX_BIT;
  if (device_properties.tessellationShader) {
    guest_shader_pipeline_stages_ |= VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                                     VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT;
    guest_shader_vertex_stages_ |=
        VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  }
  if (!device_properties.vertexPipelineStoresAndAtomics) {
    // For memory export from vertex shaders converted to compute shaders.
    guest_shader_pipeline_stages_ |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    guest_shader_vertex_stages_ |= VK_SHADER_STAGE_COMPUTE_BIT;
  }

  // 16384 is bigger than any single uniform buffer that Xenia needs, but is the
  // minimum maxUniformBufferRange, thus the safe minimum amount.
  const size_t uniform_buffer_pool_page_size =
      rex::align(std::max(ui::GraphicsUploadBufferPool::kDefaultPageSize, size_t(16384)),
                 size_t(device_properties.minUniformBufferOffsetAlignment));
  uniform_buffer_pool_ = std::make_unique<ui::vulkan::VulkanUploadBufferPool>(
      vulkan_device, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, uniform_buffer_pool_page_size);
  // Read once: the type is a part of the descriptor set layout, thus of every pipeline layout and pipeline.
  // Five dynamic uniform buffers are within the minimum maxDescriptorSetUniformBuffersDynamic (8).
  constants_dynamic_offsets_ = REXCVAR_GET(vulkan_dynamic_constant_offsets);
  // The pool may make its buffers larger than this, never smaller.
  constants_dynamic_page_size_ = uniform_buffer_pool_page_size;
  constants_dynamic_descriptor_sets_.clear();
  constants_dynamic_per_page_sets_ =
      constants_dynamic_offsets_ && !REXCVAR_GET(vulkan_dynamic_constant_offsets_per_change_sets);
  constants_dynamic_binds_logged_ = 0;
  if (constants_dynamic_offsets_) {
    REXGPU_INFO("Guest constant buffers: dynamic uniform buffer offsets (vulkan_dynamic_constant_offsets), {}",
                constants_dynamic_per_page_sets_ ? "one set per pool page"
                                                 : "DIAGNOSTIC one set per change, zero offsets");
  }

  // Descriptor set layouts that don't depend on the setup of other subsystems.
  VkShaderStageFlags guest_shader_stages =
      guest_shader_vertex_stages_ | VK_SHADER_STAGE_FRAGMENT_BIT;
  // Empty.
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = 0;
  descriptor_set_layout_create_info.pBindings = nullptr;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_empty_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create an empty Vulkan descriptor set layout");
    return false;
  }
  // Guest draw constants.
  VkDescriptorSetLayoutBinding
      descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferCount] = {};
  for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
    VkDescriptorSetLayoutBinding& constants_binding = descriptor_set_layout_bindings_constants[i];
    constants_binding.binding = i;
    constants_binding.descriptorType = constants_dynamic_offsets_
                                           ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                           : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    constants_binding.descriptorCount = 1;
    constants_binding.pImmutableSamplers = nullptr;
  }
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferSystem]
      .stageFlags =
      guest_shader_stages |
      (device_properties.tessellationShader ? VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT : 0) |
      (device_properties.geometryShader ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferFloatVertex]
      .stageFlags = guest_shader_vertex_stages_;
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferFloatPixel]
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferBoolLoop]
      .stageFlags = guest_shader_stages;
  descriptor_set_layout_bindings_constants[SpirvShaderTranslator::kConstantBufferFetch].stageFlags =
      guest_shader_stages;
  descriptor_set_layout_create_info.bindingCount =
      uint32_t(rex::countof(descriptor_set_layout_bindings_constants));
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings_constants;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &descriptor_set_layout_constants_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan descriptor set layout for guest draw "
        "constant buffers");
    return false;
  }
  // Transient: storage buffer for compute shaders.
  VkDescriptorSetLayoutBinding descriptor_set_layout_binding_transient;
  descriptor_set_layout_binding_transient.binding = 0;
  descriptor_set_layout_binding_transient.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_set_layout_binding_transient.descriptorCount = 1;
  descriptor_set_layout_binding_transient.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  descriptor_set_layout_binding_transient.pImmutableSamplers = nullptr;
  descriptor_set_layout_create_info.bindingCount = 1;
  descriptor_set_layout_create_info.pBindings = &descriptor_set_layout_binding_transient;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageBufferCompute)]) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan descriptor set layout for a storage buffer "
        "bound to the compute shader");
    return false;
  }
  // Transient: two storage buffers for compute shaders.
  VkDescriptorSetLayoutBinding descriptor_set_layout_bindings_transient_pair[2];
  descriptor_set_layout_bindings_transient_pair[0] = descriptor_set_layout_binding_transient;
  descriptor_set_layout_bindings_transient_pair[1] = descriptor_set_layout_binding_transient;
  descriptor_set_layout_bindings_transient_pair[1].binding = 1;
  descriptor_set_layout_create_info.bindingCount = 2;
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings_transient_pair;
  if (dfn.vkCreateDescriptorSetLayout(
          device, &descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layouts_single_transient_[size_t(
              SingleTransientDescriptorLayout::kStorageBufferPairCompute)]) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan descriptor set layout for two storage "
        "buffers bound to the compute shader");
    return false;
  }

  // Merge the driver pipeline cache saved by previous runs before creating the built-in
  // pipelines (render target, texture load and presentation ones), which otherwise compile from
  // scratch at every start. The shader storage reuses this merge instead of loading the file again.
  const double setup_ms_before_driver_cache = setup_ms_since_start();
  setup_driver_pipeline_cache_path_.clear();
  setup_driver_pipeline_cache_size_ = 0;
  // The GPU thread may start before the executable module is registered, so the title ID isn't
  // known here; a recompiled game has a single title, so take the only driver cache file in the
  // storage folder (the shader storage merges its title's file later if this one was different).
  if (REXCVAR_GET(store_shaders) && kernel_state_ && kernel_state_->emulator() &&
      !kernel_state_->emulator()->cache_root().empty()) {
    std::filesystem::path storage_dir =
        VulkanPipelineCache::GetDriverPipelineCachePath(kernel_state_->emulator()->cache_root(), 0)
            .parent_path();
    std::filesystem::path found_path;
    size_t found_count = 0;
    std::error_code directory_error;
    for (std::filesystem::directory_iterator it(storage_dir, directory_error);
         !directory_error && it != std::filesystem::directory_iterator();
         it.increment(directory_error)) {
      std::string name = it->path().filename().string();
      static constexpr std::string_view kSuffix = ".vk.pipelinecache";
      if (name.size() > kSuffix.size() &&
          name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0) {
        found_path = it->path();
        ++found_count;
      }
    }
    if (found_count == 1) {
      setup_driver_pipeline_cache_path_ = found_path;
      setup_driver_pipeline_cache_size_ =
          VulkanPipelineCache::MergeDriverPipelineCacheFile(vulkan_device, found_path);
    }
  }
  setup_ms_shared_memory = setup_ms_since_start();
  shared_memory_ =
      std::make_unique<VulkanSharedMemory>(*this, *memory_, guest_shader_pipeline_stages_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("Failed to initialize shared memory");
    return false;
  }

  primitive_processor_ =
      std::make_unique<VulkanPrimitiveProcessor>(*register_file_, *memory_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the geometric primitive processor");
    return false;
  }

  uint32_t shared_memory_binding_count_log2 =
      SpirvShaderTranslator::GetSharedMemoryStorageBufferCountLog2(
          device_properties.maxStorageBufferRange);
  uint32_t shared_memory_binding_count = UINT32_C(1) << shared_memory_binding_count_log2;

  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x, draw_resolution_scale_y);
  if (!draw_resolution_scale_not_clamped) {
    REXGPU_WARN(
        "The requested draw resolution scale is not supported by the "
        "emulator, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }
  if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
    REXGPU_WARN(
        "Vulkan draw resolution scaling is experimental and may not affect all "
        "titles correctly");
  }
  if (!device_properties.fragmentStoresAndAtomics) {
    REXGPU_ERROR(
        "Vulkan fragmentStoresAndAtomics is required for GPU emulation and "
        "D3D12 parity, but unsupported by the selected device");
    std::snprintf(rex_vulkan_failure_reason, sizeof(rex_vulkan_failure_reason), "%s: no fragmentStoresAndAtomics",
                  device_properties.deviceName);
    return false;
  }
  if (!device_properties.vertexPipelineStoresAndAtomics) {
    if (REXCVAR_GET(vulkan_require_vertex_pipeline_stores_and_atomics)) {
      REXGPU_ERROR(
          "Vulkan vertexPipelineStoresAndAtomics is required "
          "(vulkan_require_vertex_pipeline_stores_and_atomics=true), but unsupported by the selected device");
      std::snprintf(rex_vulkan_failure_reason, sizeof(rex_vulkan_failure_reason),
                    "%s: no vertexPipelineStoresAndAtomics", device_properties.deviceName);
      return false;
    }
    REXGPU_WARN(
        "Vulkan vertexPipelineStoresAndAtomics is not supported by the device; draws whose vertex shader "
        "exports to memory will be left out");
  }
  if (!device_properties.geometryShader) {
    if (REXCVAR_GET(vulkan_require_geometry_shader)) {
      REXGPU_ERROR(
          "Vulkan geometryShader is required for GPU emulation "
          "(vulkan_require_geometry_shader=true), but unsupported by the "
          "selected device");
      return false;
    }
    REXGPU_WARN(
        "Vulkan geometryShader is not supported by the device; primitive "
        "fallback conversion/expansion paths will be used");
  }
  if (!device_properties.fillModeNonSolid) {
    if (REXCVAR_GET(vulkan_require_fill_mode_non_solid)) {
      REXGPU_ERROR(
          "Vulkan fillModeNonSolid is required for GPU emulation "
          "(vulkan_require_fill_mode_non_solid=true), but unsupported by the "
          "selected device");
      return false;
    }
    REXGPU_WARN(
        "Vulkan fillModeNonSolid is not supported by the device; line/point "
        "polygon modes will fall back to solid fill");
  }

  // Requires the transient descriptor set layouts.
  setup_ms_render_targets = setup_ms_since_start();
  render_target_cache_ = std::make_unique<VulkanRenderTargetCache>(
      *register_file_, *memory_, draw_resolution_scale_x, draw_resolution_scale_y, *this);
  if (!render_target_cache_->Initialize(shared_memory_binding_count)) {
    REXGPU_ERROR("Failed to initialize the render target cache");
    return false;
  }
  // Lazy resolve dumps are written before anything reads their memory.
  shared_memory_->SetRangeRequestListener(render_target_cache_.get());

  // Shared memory and EDRAM descriptor set layout.
  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  const bool shared_memory_texel_buffer = SpirvShaderTranslator::UseSharedMemoryTexelBuffer(
      device_properties.maxStorageBufferRange, device_properties.maxTexelBufferElements);
  REXGPU_INFO("Shared memory shader reads: {}",
              shared_memory_texel_buffer
                  ? "one R32_UINT texel buffer (512 MB)"
                  : fmt::format("{} storage buffer(s)", shared_memory_binding_count));
  VkDescriptorSetLayoutBinding shared_memory_and_edram_descriptor_set_layout_bindings[3];
  shared_memory_and_edram_descriptor_set_layout_bindings[0].binding = 0;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorType =
      VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].descriptorCount =
      shared_memory_binding_count;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].stageFlags = guest_shader_stages;
  shared_memory_and_edram_descriptor_set_layout_bindings[0].pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo shared_memory_and_edram_descriptor_set_layout_create_info;
  shared_memory_and_edram_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  shared_memory_and_edram_descriptor_set_layout_create_info.pNext = nullptr;
  shared_memory_and_edram_descriptor_set_layout_create_info.flags = 0;
  shared_memory_and_edram_descriptor_set_layout_create_info.pBindings =
      shared_memory_and_edram_descriptor_set_layout_bindings;
  if (edram_fragment_shader_interlock) {
    // EDRAM.
    shared_memory_and_edram_descriptor_set_layout_bindings[1].binding = 1;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].descriptorCount = 1;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;
    shared_memory_and_edram_descriptor_set_layout_bindings[1].pImmutableSamplers = nullptr;
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 2;
  } else {
    shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount = 1;
  }
  if (shared_memory_texel_buffer) {
    VkDescriptorSetLayoutBinding& texel_binding =
        shared_memory_and_edram_descriptor_set_layout_bindings
            [shared_memory_and_edram_descriptor_set_layout_create_info.bindingCount++];
    texel_binding.binding = SpirvShaderTranslator::kSharedMemoryTexelBufferBinding;
    texel_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    texel_binding.descriptorCount = 1;
    texel_binding.stageFlags = guest_shader_stages;
    texel_binding.pImmutableSamplers = nullptr;
  }
  if (dfn.vkCreateDescriptorSetLayout(
          device, &shared_memory_and_edram_descriptor_set_layout_create_info, nullptr,
          &descriptor_set_layout_shared_memory_and_edram_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan descriptor set layout for the shared memory "
        "and the EDRAM");
    return false;
  }

  setup_ms_pipelines = setup_ms_since_start();
  pipeline_cache_ = std::make_unique<VulkanPipelineCache>(
      *this, *register_file_, *render_target_cache_, guest_shader_vertex_stages_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the graphics pipeline cache");
    return false;
  }

  setup_ms_textures = setup_ms_since_start();
  // Requires the transient descriptor set layouts.
  texture_cache_ =
      VulkanTextureCache::Create(*register_file_, *shared_memory_, draw_resolution_scale_x,
                                 draw_resolution_scale_y, *this, guest_shader_pipeline_stages_);
  if (!texture_cache_) {
    REXGPU_ERROR("Failed to initialize the texture cache");
    return false;
  }

  // Shared memory and EDRAM common bindings.
  VkDescriptorPoolSize descriptor_pool_sizes[2];
  descriptor_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  descriptor_pool_sizes[0].descriptorCount =
      shared_memory_binding_count + uint32_t(edram_fragment_shader_interlock);
  descriptor_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  descriptor_pool_sizes[1].descriptorCount = 1;
  VkDescriptorPoolCreateInfo descriptor_pool_create_info;
  descriptor_pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  descriptor_pool_create_info.pNext = nullptr;
  descriptor_pool_create_info.flags = 0;
  descriptor_pool_create_info.maxSets = 1;
  descriptor_pool_create_info.poolSizeCount = shared_memory_texel_buffer ? 2 : 1;
  descriptor_pool_create_info.pPoolSizes = descriptor_pool_sizes;
  if (dfn.vkCreateDescriptorPool(device, &descriptor_pool_create_info, nullptr,
                                 &shared_memory_and_edram_descriptor_pool_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the Vulkan descriptor pool for shared memory and "
        "EDRAM");
    return false;
  }
  VkDescriptorSetAllocateInfo descriptor_set_allocate_info;
  descriptor_set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  descriptor_set_allocate_info.pNext = nullptr;
  descriptor_set_allocate_info.descriptorPool = shared_memory_and_edram_descriptor_pool_;
  descriptor_set_allocate_info.descriptorSetCount = 1;
  descriptor_set_allocate_info.pSetLayouts = &descriptor_set_layout_shared_memory_and_edram_;
  if (dfn.vkAllocateDescriptorSets(device, &descriptor_set_allocate_info,
                                   &shared_memory_and_edram_descriptor_set_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to allocate the Vulkan descriptor set for shared memory and "
        "EDRAM");
    return false;
  }
  VkDescriptorBufferInfo
      shared_memory_descriptor_buffers_info[SharedMemory::kBufferSize / (128 << 20)];
  uint32_t shared_memory_binding_range =
      SharedMemory::kBufferSize >> shared_memory_binding_count_log2;
  for (uint32_t i = 0; i < shared_memory_binding_count; ++i) {
    VkDescriptorBufferInfo& shared_memory_descriptor_buffer_info =
        shared_memory_descriptor_buffers_info[i];
    shared_memory_descriptor_buffer_info.buffer = shared_memory_->buffer();
    shared_memory_descriptor_buffer_info.offset = shared_memory_binding_range * i;
    shared_memory_descriptor_buffer_info.range = shared_memory_binding_range;
  }
  VkWriteDescriptorSet write_descriptor_sets[3];
  uint32_t write_descriptor_set_count = 1 + uint32_t(edram_fragment_shader_interlock);
  VkWriteDescriptorSet& write_descriptor_set_shared_memory = write_descriptor_sets[0];
  write_descriptor_set_shared_memory.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  write_descriptor_set_shared_memory.pNext = nullptr;
  write_descriptor_set_shared_memory.dstSet = shared_memory_and_edram_descriptor_set_;
  write_descriptor_set_shared_memory.dstBinding = 0;
  write_descriptor_set_shared_memory.dstArrayElement = 0;
  write_descriptor_set_shared_memory.descriptorCount = shared_memory_binding_count;
  write_descriptor_set_shared_memory.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write_descriptor_set_shared_memory.pImageInfo = nullptr;
  write_descriptor_set_shared_memory.pBufferInfo = shared_memory_descriptor_buffers_info;
  write_descriptor_set_shared_memory.pTexelBufferView = nullptr;
  VkDescriptorBufferInfo edram_descriptor_buffer_info;
  if (edram_fragment_shader_interlock) {
    edram_descriptor_buffer_info.buffer = render_target_cache_->edram_buffer();
    edram_descriptor_buffer_info.offset = 0;
    edram_descriptor_buffer_info.range = VK_WHOLE_SIZE;
    VkWriteDescriptorSet& write_descriptor_set_edram = write_descriptor_sets[1];
    write_descriptor_set_edram.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_descriptor_set_edram.pNext = nullptr;
    write_descriptor_set_edram.dstSet = shared_memory_and_edram_descriptor_set_;
    write_descriptor_set_edram.dstBinding = 1;
    write_descriptor_set_edram.dstArrayElement = 0;
    write_descriptor_set_edram.descriptorCount = 1;
    write_descriptor_set_edram.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write_descriptor_set_edram.pImageInfo = nullptr;
    write_descriptor_set_edram.pBufferInfo = &edram_descriptor_buffer_info;
    write_descriptor_set_edram.pTexelBufferView = nullptr;
  }
  if (shared_memory_texel_buffer) {
    VkBufferViewCreateInfo view_create_info = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};
    view_create_info.buffer = shared_memory_->buffer();
    view_create_info.format = VK_FORMAT_R32_UINT;
    view_create_info.offset = 0;
    view_create_info.range = SharedMemory::kBufferSize;
    if (dfn.vkCreateBufferView(device, &view_create_info, nullptr,
                               &shared_memory_texel_buffer_view_) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to create the shared memory texel buffer view");
      return false;
    }
    VkWriteDescriptorSet& write_texel = write_descriptor_sets[write_descriptor_set_count++];
    write_texel.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write_texel.pNext = nullptr;
    write_texel.dstSet = shared_memory_and_edram_descriptor_set_;
    write_texel.dstBinding = SpirvShaderTranslator::kSharedMemoryTexelBufferBinding;
    write_texel.dstArrayElement = 0;
    write_texel.descriptorCount = 1;
    write_texel.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    write_texel.pImageInfo = nullptr;
    write_texel.pBufferInfo = nullptr;
    write_texel.pTexelBufferView = &shared_memory_texel_buffer_view_;
  }
  dfn.vkUpdateDescriptorSets(device, write_descriptor_set_count, write_descriptor_sets, 0,
                             nullptr);

  // Swap objects.

  // Gamma ramp, either device-local and host-visible at once, or separate
  // device-local texel buffer and host-visible upload buffer.
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
  // Try to create a device-local host-visible buffer first, to skip copying.
  constexpr uint32_t kGammaRampSize256EntryTable = sizeof(uint32_t) * 256;
  constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
  constexpr uint32_t kGammaRampSize = kGammaRampSize256EntryTable + kGammaRampSizePWL;
  VkBufferCreateInfo gamma_ramp_host_visible_buffer_create_info;
  gamma_ramp_host_visible_buffer_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  gamma_ramp_host_visible_buffer_create_info.pNext = nullptr;
  gamma_ramp_host_visible_buffer_create_info.flags = 0;
  gamma_ramp_host_visible_buffer_create_info.size = kGammaRampSize * kMaxFramesInFlight;
  gamma_ramp_host_visible_buffer_create_info.usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT;
  gamma_ramp_host_visible_buffer_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  gamma_ramp_host_visible_buffer_create_info.queueFamilyIndexCount = 0;
  gamma_ramp_host_visible_buffer_create_info.pQueueFamilyIndices = nullptr;
  if (dfn.vkCreateBuffer(device, &gamma_ramp_host_visible_buffer_create_info, nullptr,
                         &gamma_ramp_buffer_) == VK_SUCCESS) {
    bool use_gamma_ramp_host_visible_buffer = false;
    VkMemoryRequirements gamma_ramp_host_visible_buffer_memory_requirements;
    dfn.vkGetBufferMemoryRequirements(device, gamma_ramp_buffer_,
                                      &gamma_ramp_host_visible_buffer_memory_requirements);
    uint32_t gamma_ramp_host_visible_buffer_memory_types =
        gamma_ramp_host_visible_buffer_memory_requirements.memoryTypeBits &
        (vulkan_device->memory_types().device_local & vulkan_device->memory_types().host_visible);
    VkMemoryAllocateInfo gamma_ramp_host_visible_buffer_memory_allocate_info;
    // Prefer a host-uncached (because it's write-only) memory type, but try a
    // host-cached host-visible device-local one as well.
    if (rex::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types &
                ~vulkan_device->memory_types().host_cached,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info.memoryTypeIndex)) ||
        rex::bit_scan_forward(
            gamma_ramp_host_visible_buffer_memory_types,
            &(gamma_ramp_host_visible_buffer_memory_allocate_info.memoryTypeIndex))) {
      VkMemoryAllocateInfo* gamma_ramp_host_visible_buffer_memory_allocate_info_last =
          &gamma_ramp_host_visible_buffer_memory_allocate_info;
      gamma_ramp_host_visible_buffer_memory_allocate_info.sType =
          VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      gamma_ramp_host_visible_buffer_memory_allocate_info.pNext = nullptr;
      gamma_ramp_host_visible_buffer_memory_allocate_info.allocationSize =
          gamma_ramp_host_visible_buffer_memory_requirements.size;
      VkMemoryDedicatedAllocateInfo gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
      if (vulkan_device->extensions().ext_1_1_KHR_dedicated_allocation) {
        gamma_ramp_host_visible_buffer_memory_allocate_info_last->pNext =
            &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info;
        gamma_ramp_host_visible_buffer_memory_allocate_info_last =
            reinterpret_cast<VkMemoryAllocateInfo*>(
                &gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info);
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.sType =
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.pNext = nullptr;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.image = VK_NULL_HANDLE;
        gamma_ramp_host_visible_buffer_memory_dedicated_allocate_info.buffer = gamma_ramp_buffer_;
      }
      if (dfn.vkAllocateMemory(device, &gamma_ramp_host_visible_buffer_memory_allocate_info,
                               nullptr, &gamma_ramp_buffer_memory_) == VK_SUCCESS) {
        if (dfn.vkBindBufferMemory(device, gamma_ramp_buffer_, gamma_ramp_buffer_memory_, 0) ==
            VK_SUCCESS) {
          if (dfn.vkMapMemory(device, gamma_ramp_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
                              &gamma_ramp_upload_mapping_) == VK_SUCCESS) {
            use_gamma_ramp_host_visible_buffer = true;
            gamma_ramp_upload_memory_size_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info.allocationSize;
            gamma_ramp_upload_memory_type_ =
                gamma_ramp_host_visible_buffer_memory_allocate_info.memoryTypeIndex;
          }
        }
        if (!use_gamma_ramp_host_visible_buffer) {
          dfn.vkFreeMemory(device, gamma_ramp_buffer_memory_, nullptr);
          gamma_ramp_buffer_memory_ = VK_NULL_HANDLE;
        }
      }
    }
    if (!use_gamma_ramp_host_visible_buffer) {
      dfn.vkDestroyBuffer(device, gamma_ramp_buffer_, nullptr);
      gamma_ramp_buffer_ = VK_NULL_HANDLE;
    }
  }
  if (gamma_ramp_buffer_ == VK_NULL_HANDLE) {
    // Create separate buffers for the shader and uploading.
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
            ui::vulkan::util::MemoryPurpose::kDeviceLocal, gamma_ramp_buffer_,
            gamma_ramp_buffer_memory_)) {
      REXGPU_ERROR("Failed to create the gamma ramp buffer");
      return false;
    }
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, kGammaRampSize * kMaxFramesInFlight, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            ui::vulkan::util::MemoryPurpose::kUpload, gamma_ramp_upload_buffer_,
            gamma_ramp_upload_buffer_memory_, &gamma_ramp_upload_memory_type_,
            &gamma_ramp_upload_memory_size_)) {
      REXGPU_ERROR("Failed to create the gamma ramp upload buffer");
      return false;
    }
    if (dfn.vkMapMemory(device, gamma_ramp_upload_buffer_memory_, 0, VK_WHOLE_SIZE, 0,
                        &gamma_ramp_upload_mapping_) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to map the gamma ramp upload buffer");
      return false;
    }
  }

  // Gamma ramp buffer views.
  uint32_t gamma_ramp_frame_count =
      gamma_ramp_upload_buffer_ == VK_NULL_HANDLE ? kMaxFramesInFlight : 1;
  VkBufferViewCreateInfo gamma_ramp_buffer_view_create_info;
  gamma_ramp_buffer_view_create_info.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
  gamma_ramp_buffer_view_create_info.pNext = nullptr;
  gamma_ramp_buffer_view_create_info.flags = 0;
  gamma_ramp_buffer_view_create_info.buffer = gamma_ramp_buffer_;
  // 256-entry table.
  gamma_ramp_buffer_view_create_info.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSize256EntryTable;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset = kGammaRampSize * i;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info, nullptr,
                               &gamma_ramp_buffer_views_[i * 2]) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to create a 256-entry table gamma ramp buffer view");
      return false;
    }
  }
  // Piecewise linear.
  gamma_ramp_buffer_view_create_info.format = VK_FORMAT_R16G16_UINT;
  gamma_ramp_buffer_view_create_info.range = kGammaRampSizePWL;
  for (uint32_t i = 0; i < gamma_ramp_frame_count; ++i) {
    gamma_ramp_buffer_view_create_info.offset = kGammaRampSize * i + kGammaRampSize256EntryTable;
    if (dfn.vkCreateBufferView(device, &gamma_ramp_buffer_view_create_info, nullptr,
                               &gamma_ramp_buffer_views_[i * 2 + 1]) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to create a PWL gamma ramp buffer view");
      return false;
    }
  }

  // Swap descriptor set layouts.
  VkDescriptorSetLayoutBinding swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.binding = 0;
  swap_descriptor_set_layout_binding.descriptorCount = 1;
  swap_descriptor_set_layout_binding.stageFlags =
      VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
  swap_descriptor_set_layout_binding.pImmutableSamplers = nullptr;
  VkDescriptorSetLayoutCreateInfo swap_descriptor_set_layout_create_info;
  swap_descriptor_set_layout_create_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  swap_descriptor_set_layout_create_info.pNext = nullptr;
  swap_descriptor_set_layout_create_info.flags = 0;
  swap_descriptor_set_layout_create_info.bindingCount = 1;
  swap_descriptor_set_layout_create_info.pBindings = &swap_descriptor_set_layout_binding;
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_sampled_image_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the presentation sampled image descriptor set "
        "layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_uniform_texel_buffer_) !=
      VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the presentation uniform texel buffer descriptor set "
        "layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  swap_descriptor_set_layout_binding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_combined_image_sampler_) !=
      VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the presentation combined image sampler descriptor "
        "set layout");
    return false;
  }
  swap_descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  if (dfn.vkCreateDescriptorSetLayout(device, &swap_descriptor_set_layout_create_info, nullptr,
                                      &swap_descriptor_set_layout_storage_image_) != VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create the presentation storage image descriptor set "
        "layout");
    return false;
  }

  // Swap descriptor pool.
  std::array<VkDescriptorPoolSize, 4> swap_descriptor_pool_sizes;
  VkDescriptorPoolCreateInfo swap_descriptor_pool_create_info;
  swap_descriptor_pool_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  swap_descriptor_pool_create_info.pNext = nullptr;
  swap_descriptor_pool_create_info.flags = 0;
  swap_descriptor_pool_create_info.maxSets = 0;
  swap_descriptor_pool_create_info.poolSizeCount = 0;
  swap_descriptor_pool_create_info.pPoolSizes = swap_descriptor_pool_sizes.data();
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_sampled_image =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info.poolSizeCount++];
    swap_descriptor_pool_size_sampled_image.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    // Source images.
    swap_descriptor_pool_size_sampled_image.descriptorCount = kMaxFramesInFlight;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight;
  }
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_combined_image_sampler =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info.poolSizeCount++];
    swap_descriptor_pool_size_combined_image_sampler.type =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    swap_descriptor_pool_size_combined_image_sampler.descriptorCount = kMaxFramesInFlight;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight;
  }
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_storage_image =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info.poolSizeCount++];
    swap_descriptor_pool_size_storage_image.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    swap_descriptor_pool_size_storage_image.descriptorCount = kMaxFramesInFlight * 2;
    swap_descriptor_pool_create_info.maxSets += kMaxFramesInFlight * 2;
  }
  // 256-entry table and PWL gamma ramps. If the gamma ramp buffer is
  // host-visible, for multiple frames.
  uint32_t gamma_ramp_buffer_view_count = 2 * gamma_ramp_frame_count;
  {
    VkDescriptorPoolSize& swap_descriptor_pool_size_uniform_texel_buffer =
        swap_descriptor_pool_sizes[swap_descriptor_pool_create_info.poolSizeCount++];
    swap_descriptor_pool_size_uniform_texel_buffer.type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    swap_descriptor_pool_size_uniform_texel_buffer.descriptorCount = gamma_ramp_buffer_view_count;
    swap_descriptor_pool_create_info.maxSets += gamma_ramp_buffer_view_count;
  }
  if (dfn.vkCreateDescriptorPool(device, &swap_descriptor_pool_create_info, nullptr,
                                 &swap_descriptor_pool_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the presentation descriptor pool");
    return false;
  }

  // Swap descriptor set allocation.
  VkDescriptorSetAllocateInfo swap_descriptor_set_allocate_info;
  swap_descriptor_set_allocate_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  swap_descriptor_set_allocate_info.pNext = nullptr;
  swap_descriptor_set_allocate_info.descriptorPool = swap_descriptor_pool_;
  swap_descriptor_set_allocate_info.descriptorSetCount = 1;
  swap_descriptor_set_allocate_info.pSetLayouts = &swap_descriptor_set_layout_uniform_texel_buffer_;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_gamma_ramp_[i]) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to allocate the gamma ramp descriptor sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts = &swap_descriptor_set_layout_sampled_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_source_[i]) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to allocate the presentation source image descriptor sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts =
      &swap_descriptor_set_layout_combined_image_sampler_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_fxaa_source_[i]) != VK_SUCCESS) {
      REXGPU_ERROR(
          "Failed to allocate the presentation FXAA source image descriptor "
          "sets");
      return false;
    }
  }
  swap_descriptor_set_allocate_info.pSetLayouts = &swap_descriptor_set_layout_storage_image_;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_destination_storage_[i]) != VK_SUCCESS) {
      REXGPU_ERROR(
          "Failed to allocate the presentation destination storage image "
          "descriptor sets");
      return false;
    }
  }
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (dfn.vkAllocateDescriptorSets(device, &swap_descriptor_set_allocate_info,
                                     &swap_descriptors_fxaa_destination_storage_[i]) !=
        VK_SUCCESS) {
      REXGPU_ERROR(
          "Failed to allocate the presentation FXAA destination storage image "
          "descriptor sets");
      return false;
    }
  }

  // Gamma ramp descriptor sets.
  VkWriteDescriptorSet gamma_ramp_write_descriptor_set;
  gamma_ramp_write_descriptor_set.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  gamma_ramp_write_descriptor_set.pNext = nullptr;
  gamma_ramp_write_descriptor_set.dstBinding = 0;
  gamma_ramp_write_descriptor_set.dstArrayElement = 0;
  gamma_ramp_write_descriptor_set.descriptorCount = 1;
  gamma_ramp_write_descriptor_set.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
  gamma_ramp_write_descriptor_set.pImageInfo = nullptr;
  gamma_ramp_write_descriptor_set.pBufferInfo = nullptr;
  for (uint32_t i = 0; i < gamma_ramp_buffer_view_count; ++i) {
    gamma_ramp_write_descriptor_set.dstSet = swap_descriptors_gamma_ramp_[i];
    gamma_ramp_write_descriptor_set.pTexelBufferView = &gamma_ramp_buffer_views_[i];
    dfn.vkUpdateDescriptorSets(device, 1, &gamma_ramp_write_descriptor_set, 0, nullptr);
  }

  // Linear sampler for FXAA.
  VkSamplerCreateInfo swap_sampler_create_info;
  swap_sampler_create_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  swap_sampler_create_info.pNext = nullptr;
  swap_sampler_create_info.flags = 0;
  swap_sampler_create_info.magFilter = VK_FILTER_LINEAR;
  swap_sampler_create_info.minFilter = VK_FILTER_LINEAR;
  swap_sampler_create_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  swap_sampler_create_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  swap_sampler_create_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  swap_sampler_create_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  swap_sampler_create_info.mipLodBias = 0.0f;
  swap_sampler_create_info.anisotropyEnable = VK_FALSE;
  swap_sampler_create_info.maxAnisotropy = 1.0f;
  swap_sampler_create_info.compareEnable = VK_FALSE;
  swap_sampler_create_info.compareOp = VK_COMPARE_OP_NEVER;
  swap_sampler_create_info.minLod = 0.0f;
  swap_sampler_create_info.maxLod = 0.0f;
  swap_sampler_create_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  swap_sampler_create_info.unnormalizedCoordinates = VK_FALSE;
  if (dfn.vkCreateSampler(device, &swap_sampler_create_info, nullptr,
                          &swap_sampler_linear_clamp_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the presentation FXAA sampler");
    return false;
  }

  // Gamma ramp application pipeline layout.
  std::array<VkDescriptorSetLayout, kSwapApplyGammaDescriptorSetCount>
      swap_apply_gamma_descriptor_set_layouts{};
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetRamp] =
      swap_descriptor_set_layout_uniform_texel_buffer_;
  swap_apply_gamma_descriptor_set_layouts[kSwapApplyGammaDescriptorSetSource] =
      swap_descriptor_set_layout_sampled_image_;
  VkPipelineLayoutCreateInfo swap_apply_gamma_pipeline_layout_create_info;
  swap_apply_gamma_pipeline_layout_create_info.sType =
      VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  swap_apply_gamma_pipeline_layout_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_layout_create_info.flags = 0;
  swap_apply_gamma_pipeline_layout_create_info.setLayoutCount =
      uint32_t(swap_apply_gamma_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_apply_gamma_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 0;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges = nullptr;
  if (dfn.vkCreatePipelineLayout(device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
                                 &swap_apply_gamma_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the gamma ramp application pipeline layout");
    return false;
  }

  // Gamma ramp application compute pipeline layout.
  std::array<VkDescriptorSetLayout, kSwapApplyGammaComputeDescriptorSetCount>
      swap_apply_gamma_compute_descriptor_set_layouts{};
  swap_apply_gamma_compute_descriptor_set_layouts[kSwapApplyGammaComputeDescriptorSetRamp] =
      swap_descriptor_set_layout_uniform_texel_buffer_;
  swap_apply_gamma_compute_descriptor_set_layouts[kSwapApplyGammaComputeDescriptorSetSource] =
      swap_descriptor_set_layout_sampled_image_;
  swap_apply_gamma_compute_descriptor_set_layouts[kSwapApplyGammaComputeDescriptorSetDestination] =
      swap_descriptor_set_layout_storage_image_;
  VkPushConstantRange swap_apply_gamma_compute_push_constant_range;
  swap_apply_gamma_compute_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  swap_apply_gamma_compute_push_constant_range.offset = 0;
  swap_apply_gamma_compute_push_constant_range.size = sizeof(SwapApplyGammaConstants);
  swap_apply_gamma_pipeline_layout_create_info.setLayoutCount =
      uint32_t(swap_apply_gamma_compute_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_apply_gamma_compute_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 1;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges =
      &swap_apply_gamma_compute_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
                                 &swap_apply_gamma_compute_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the gamma ramp application compute pipeline layout");
    return false;
  }

  // FXAA compute pipeline layout.
  std::array<VkDescriptorSetLayout, kSwapFxaaDescriptorSetCount> swap_fxaa_descriptor_set_layouts{};
  swap_fxaa_descriptor_set_layouts[kSwapFxaaDescriptorSetSource] =
      swap_descriptor_set_layout_combined_image_sampler_;
  swap_fxaa_descriptor_set_layouts[kSwapFxaaDescriptorSetDestination] =
      swap_descriptor_set_layout_storage_image_;
  VkPushConstantRange swap_fxaa_push_constant_range;
  swap_fxaa_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  swap_fxaa_push_constant_range.offset = 0;
  swap_fxaa_push_constant_range.size = sizeof(SwapFxaaConstants);
  swap_apply_gamma_pipeline_layout_create_info.setLayoutCount =
      uint32_t(swap_fxaa_descriptor_set_layouts.size());
  swap_apply_gamma_pipeline_layout_create_info.pSetLayouts =
      swap_fxaa_descriptor_set_layouts.data();
  swap_apply_gamma_pipeline_layout_create_info.pushConstantRangeCount = 1;
  swap_apply_gamma_pipeline_layout_create_info.pPushConstantRanges = &swap_fxaa_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &swap_apply_gamma_pipeline_layout_create_info, nullptr,
                                 &swap_fxaa_pipeline_layout_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the FXAA compute pipeline layout");
    return false;
  }

  // Gamma application render pass. Doesn't make assumptions about outer usage
  // (explicit barriers must be used instead) for simplicity of use in different
  // scenarios with different pipelines.
  VkAttachmentDescription swap_apply_gamma_render_pass_attachment;
  swap_apply_gamma_render_pass_attachment.flags = 0;
  swap_apply_gamma_render_pass_attachment.format = ui::vulkan::VulkanPresenter::kGuestOutputFormat;
  swap_apply_gamma_render_pass_attachment.samples = VK_SAMPLE_COUNT_1_BIT;
  swap_apply_gamma_render_pass_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swap_apply_gamma_render_pass_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  swap_apply_gamma_render_pass_attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  swap_apply_gamma_render_pass_attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  swap_apply_gamma_render_pass_attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  swap_apply_gamma_render_pass_attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkAttachmentReference swap_apply_gamma_render_pass_color_attachment;
  swap_apply_gamma_render_pass_color_attachment.attachment = 0;
  swap_apply_gamma_render_pass_color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkSubpassDescription swap_apply_gamma_render_pass_subpass = {};
  swap_apply_gamma_render_pass_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  swap_apply_gamma_render_pass_subpass.colorAttachmentCount = 1;
  swap_apply_gamma_render_pass_subpass.pColorAttachments =
      &swap_apply_gamma_render_pass_color_attachment;
  VkSubpassDependency swap_apply_gamma_render_pass_dependencies[2];
  for (uint32_t i = 0; i < 2; ++i) {
    VkSubpassDependency& swap_apply_gamma_render_pass_dependency =
        swap_apply_gamma_render_pass_dependencies[i];
    swap_apply_gamma_render_pass_dependency.srcSubpass = i ? 0 : VK_SUBPASS_EXTERNAL;
    swap_apply_gamma_render_pass_dependency.dstSubpass = i ? VK_SUBPASS_EXTERNAL : 0;
    swap_apply_gamma_render_pass_dependency.srcStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    swap_apply_gamma_render_pass_dependency.dstStageMask =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    swap_apply_gamma_render_pass_dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    swap_apply_gamma_render_pass_dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    swap_apply_gamma_render_pass_dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
  }
  VkRenderPassCreateInfo swap_apply_gamma_render_pass_create_info;
  swap_apply_gamma_render_pass_create_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  swap_apply_gamma_render_pass_create_info.pNext = nullptr;
  swap_apply_gamma_render_pass_create_info.flags = 0;
  swap_apply_gamma_render_pass_create_info.attachmentCount = 1;
  swap_apply_gamma_render_pass_create_info.pAttachments = &swap_apply_gamma_render_pass_attachment;
  swap_apply_gamma_render_pass_create_info.subpassCount = 1;
  swap_apply_gamma_render_pass_create_info.pSubpasses = &swap_apply_gamma_render_pass_subpass;
  swap_apply_gamma_render_pass_create_info.dependencyCount =
      uint32_t(rex::countof(swap_apply_gamma_render_pass_dependencies));
  swap_apply_gamma_render_pass_create_info.pDependencies =
      swap_apply_gamma_render_pass_dependencies;
  if (dfn.vkCreateRenderPass(device, &swap_apply_gamma_render_pass_create_info, nullptr,
                             &swap_apply_gamma_render_pass_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the gamma ramp application render pass");
    return false;
  }

  // Gamma ramp application pipeline.
  // Using a graphics pipeline, not a compute one, because storage image support
  // is optional for VK_FORMAT_A2B10G10R10_UNORM_PACK32.

  enum SwapApplyGammaPixelShader {
    kSwapApplyGammaPixelShader256EntryTable,
    kSwapApplyGammaPixelShaderPWL,
    kSwapApplyGammaPixelShader256EntryTableFxaaLuma,
    kSwapApplyGammaPixelShaderPWLFxaaLuma,

    kSwapApplyGammaPixelShaderCount,
  };
  std::array<VkShaderModule, kSwapApplyGammaPixelShaderCount> swap_apply_gamma_pixel_shaders{};
  bool swap_apply_gamma_pixel_shaders_created =
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTable] =
           ui::vulkan::util::CreateShaderModule(vulkan_device, shaders::apply_gamma_table_ps,
                                                sizeof(shaders::apply_gamma_table_ps))) !=
          VK_NULL_HANDLE &&
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWL] =
           ui::vulkan::util::CreateShaderModule(vulkan_device, shaders::apply_gamma_pwl_ps,
                                                sizeof(shaders::apply_gamma_pwl_ps))) !=
          VK_NULL_HANDLE &&
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTableFxaaLuma] =
           ui::vulkan::util::CreateShaderModule(
               vulkan_device, shaders::apply_gamma_table_fxaa_luma_ps,
               sizeof(shaders::apply_gamma_table_fxaa_luma_ps))) != VK_NULL_HANDLE &&
      (swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWLFxaaLuma] =
           ui::vulkan::util::CreateShaderModule(
               vulkan_device, shaders::apply_gamma_pwl_fxaa_luma_ps,
               sizeof(shaders::apply_gamma_pwl_fxaa_luma_ps))) != VK_NULL_HANDLE;
  if (!swap_apply_gamma_pixel_shaders_created) {
    REXGPU_ERROR("Failed to create the gamma ramp application pixel shader modules");
    for (VkShaderModule swap_apply_gamma_pixel_shader : swap_apply_gamma_pixel_shaders) {
      if (swap_apply_gamma_pixel_shader != VK_NULL_HANDLE) {
        dfn.vkDestroyShaderModule(device, swap_apply_gamma_pixel_shader, nullptr);
      }
    }
    return false;
  }

  VkPipelineShaderStageCreateInfo swap_apply_gamma_pipeline_stages[2];
  swap_apply_gamma_pipeline_stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  swap_apply_gamma_pipeline_stages[0].pNext = nullptr;
  swap_apply_gamma_pipeline_stages[0].flags = 0;
  swap_apply_gamma_pipeline_stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  swap_apply_gamma_pipeline_stages[0].module = ui::vulkan::util::CreateShaderModule(
      vulkan_device, shaders::fullscreen_cw_vs, sizeof(shaders::fullscreen_cw_vs));
  if (swap_apply_gamma_pipeline_stages[0].module == VK_NULL_HANDLE) {
    REXGPU_ERROR("Failed to create the gamma ramp application vertex shader module");
    for (VkShaderModule swap_apply_gamma_pixel_shader : swap_apply_gamma_pixel_shaders) {
      assert_true(swap_apply_gamma_pixel_shader != VK_NULL_HANDLE);
      dfn.vkDestroyShaderModule(device, swap_apply_gamma_pixel_shader, nullptr);
    }
    return false;
  }
  swap_apply_gamma_pipeline_stages[0].pName = "main";
  swap_apply_gamma_pipeline_stages[0].pSpecializationInfo = nullptr;
  swap_apply_gamma_pipeline_stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  swap_apply_gamma_pipeline_stages[1].pNext = nullptr;
  swap_apply_gamma_pipeline_stages[1].flags = 0;
  swap_apply_gamma_pipeline_stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  // The fragment shader module will be specified later.
  swap_apply_gamma_pipeline_stages[1].pName = "main";
  swap_apply_gamma_pipeline_stages[1].pSpecializationInfo = nullptr;

  VkPipelineVertexInputStateCreateInfo swap_apply_gamma_pipeline_vertex_input_state = {};
  swap_apply_gamma_pipeline_vertex_input_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo swap_apply_gamma_pipeline_input_assembly_state;
  swap_apply_gamma_pipeline_input_assembly_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_input_assembly_state.pNext = nullptr;
  swap_apply_gamma_pipeline_input_assembly_state.flags = 0;
  swap_apply_gamma_pipeline_input_assembly_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  swap_apply_gamma_pipeline_input_assembly_state.primitiveRestartEnable = VK_FALSE;

  VkPipelineViewportStateCreateInfo swap_apply_gamma_pipeline_viewport_state;
  swap_apply_gamma_pipeline_viewport_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_viewport_state.pNext = nullptr;
  swap_apply_gamma_pipeline_viewport_state.flags = 0;
  swap_apply_gamma_pipeline_viewport_state.viewportCount = 1;
  swap_apply_gamma_pipeline_viewport_state.pViewports = nullptr;
  swap_apply_gamma_pipeline_viewport_state.scissorCount = 1;
  swap_apply_gamma_pipeline_viewport_state.pScissors = nullptr;

  VkPipelineRasterizationStateCreateInfo swap_apply_gamma_pipeline_rasterization_state = {};
  swap_apply_gamma_pipeline_rasterization_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_rasterization_state.polygonMode = VK_POLYGON_MODE_FILL;
  swap_apply_gamma_pipeline_rasterization_state.cullMode = VK_CULL_MODE_NONE;
  swap_apply_gamma_pipeline_rasterization_state.frontFace = VK_FRONT_FACE_CLOCKWISE;
  swap_apply_gamma_pipeline_rasterization_state.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo swap_apply_gamma_pipeline_multisample_state = {};
  swap_apply_gamma_pipeline_multisample_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_multisample_state.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState swap_apply_gamma_pipeline_color_blend_attachment_state = {};
  swap_apply_gamma_pipeline_color_blend_attachment_state.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
      VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo swap_apply_gamma_pipeline_color_blend_state = {};
  swap_apply_gamma_pipeline_color_blend_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_color_blend_state.attachmentCount = 1;
  swap_apply_gamma_pipeline_color_blend_state.pAttachments =
      &swap_apply_gamma_pipeline_color_blend_attachment_state;

  static const VkDynamicState kSwapApplyGammaPipelineDynamicStates[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };
  VkPipelineDynamicStateCreateInfo swap_apply_gamma_pipeline_dynamic_state;
  swap_apply_gamma_pipeline_dynamic_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  swap_apply_gamma_pipeline_dynamic_state.pNext = nullptr;
  swap_apply_gamma_pipeline_dynamic_state.flags = 0;
  swap_apply_gamma_pipeline_dynamic_state.dynamicStateCount =
      uint32_t(rex::countof(kSwapApplyGammaPipelineDynamicStates));
  swap_apply_gamma_pipeline_dynamic_state.pDynamicStates = kSwapApplyGammaPipelineDynamicStates;

  VkGraphicsPipelineCreateInfo swap_apply_gamma_pipeline_create_info;
  swap_apply_gamma_pipeline_create_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  swap_apply_gamma_pipeline_create_info.pNext = nullptr;
  swap_apply_gamma_pipeline_create_info.flags = 0;
  swap_apply_gamma_pipeline_create_info.stageCount =
      uint32_t(rex::countof(swap_apply_gamma_pipeline_stages));
  swap_apply_gamma_pipeline_create_info.pStages = swap_apply_gamma_pipeline_stages;
  swap_apply_gamma_pipeline_create_info.pVertexInputState =
      &swap_apply_gamma_pipeline_vertex_input_state;
  swap_apply_gamma_pipeline_create_info.pInputAssemblyState =
      &swap_apply_gamma_pipeline_input_assembly_state;
  swap_apply_gamma_pipeline_create_info.pTessellationState = nullptr;
  swap_apply_gamma_pipeline_create_info.pViewportState = &swap_apply_gamma_pipeline_viewport_state;
  swap_apply_gamma_pipeline_create_info.pRasterizationState =
      &swap_apply_gamma_pipeline_rasterization_state;
  swap_apply_gamma_pipeline_create_info.pMultisampleState =
      &swap_apply_gamma_pipeline_multisample_state;
  swap_apply_gamma_pipeline_create_info.pDepthStencilState = nullptr;
  swap_apply_gamma_pipeline_create_info.pColorBlendState =
      &swap_apply_gamma_pipeline_color_blend_state;
  swap_apply_gamma_pipeline_create_info.pDynamicState = &swap_apply_gamma_pipeline_dynamic_state;
  swap_apply_gamma_pipeline_create_info.layout = swap_apply_gamma_pipeline_layout_;
  swap_apply_gamma_pipeline_create_info.renderPass = swap_apply_gamma_render_pass_;
  swap_apply_gamma_pipeline_create_info.subpass = 0;
  swap_apply_gamma_pipeline_create_info.basePipelineHandle = VK_NULL_HANDLE;
  swap_apply_gamma_pipeline_create_info.basePipelineIndex = -1;
  std::array<VkShaderModule, 4> swap_apply_gamma_pipeline_pixel_shaders = {
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTable],
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWL],
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShader256EntryTableFxaaLuma],
      swap_apply_gamma_pixel_shaders[kSwapApplyGammaPixelShaderPWLFxaaLuma],
  };
  std::array<VkPipeline*, 4> swap_apply_gamma_pipelines = {
      &swap_apply_gamma_256_entry_table_pipeline_,
      &swap_apply_gamma_pwl_pipeline_,
      &swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_,
      &swap_apply_gamma_pwl_fxaa_luma_pipeline_,
  };
  std::array<VkResult, 4> swap_apply_gamma_pipeline_create_results;
  for (size_t i = 0; i < swap_apply_gamma_pipelines.size(); ++i) {
    swap_apply_gamma_pipeline_stages[1].module = swap_apply_gamma_pipeline_pixel_shaders[i];
    swap_apply_gamma_pipeline_create_results[i] = dfn.vkCreateGraphicsPipelines(
        device, vulkan_device->pipeline_cache(), 1, &swap_apply_gamma_pipeline_create_info,
        nullptr, swap_apply_gamma_pipelines[i]);
  }
  if (!vulkan_device->properties().imageViewFormatSwizzle) {
    auto create_rb_swap_pipeline = [&](const char* source, VkPipeline& pipeline_out,
                                       const char* pipeline_name) {
      std::vector<uint32_t> pixel_spirv;
      std::string compile_error;
      if (!CompileGlslToSpirv(VK_SHADER_STAGE_FRAGMENT_BIT, source, pixel_spirv, compile_error)) {
        REXGPU_WARN("Failed to compile {} shader to SPIR-V: {}", pipeline_name, compile_error);
        return;
      }
      VkShaderModule pixel_shader_module = ui::vulkan::util::CreateShaderModule(
          vulkan_device, pixel_spirv.data(), sizeof(uint32_t) * pixel_spirv.size());
      if (pixel_shader_module == VK_NULL_HANDLE) {
        REXGPU_WARN("Failed to create {} shader module", pipeline_name);
        return;
      }
      swap_apply_gamma_pipeline_stages[1].module = pixel_shader_module;
      if (dfn.vkCreateGraphicsPipelines(device, vulkan_device->pipeline_cache(), 1,
                                        &swap_apply_gamma_pipeline_create_info, nullptr,
                                        &pipeline_out) != VK_SUCCESS) {
        REXGPU_WARN("Failed to create {} pipeline", pipeline_name);
        pipeline_out = VK_NULL_HANDLE;
      }
      dfn.vkDestroyShaderModule(device, pixel_shader_module, nullptr);
    };
    create_rb_swap_pipeline(GetSwapApplyGammaTablePixelRbSwapSource(),
                            swap_apply_gamma_256_entry_table_rb_swap_pipeline_,
                            "swap gamma table red/blue fallback");
    create_rb_swap_pipeline(GetSwapApplyGammaPwlPixelRbSwapSource(),
                            swap_apply_gamma_pwl_rb_swap_pipeline_,
                            "swap gamma PWL red/blue fallback");
  }
  dfn.vkDestroyShaderModule(device, swap_apply_gamma_pipeline_stages[0].module, nullptr);
  for (VkShaderModule swap_apply_gamma_pixel_shader : swap_apply_gamma_pixel_shaders) {
    assert_true(swap_apply_gamma_pixel_shader != VK_NULL_HANDLE);
    dfn.vkDestroyShaderModule(device, swap_apply_gamma_pixel_shader, nullptr);
  }
  if (std::any_of(swap_apply_gamma_pipeline_create_results.begin(),
                  swap_apply_gamma_pipeline_create_results.end(),
                  [](VkResult result) { return result != VK_SUCCESS; })) {
    REXGPU_ERROR("Failed to create the gamma ramp application pipelines");
    return false;
  }

  // Gamma ramp application compute pipelines.
  swap_apply_gamma_compute_256_entry_table_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, swap_apply_gamma_compute_pipeline_layout_, shaders::apply_gamma_table_cs,
      sizeof(shaders::apply_gamma_table_cs));
  if (swap_apply_gamma_compute_256_entry_table_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline, keeping graphics fallback");
  }
  swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_ =
      ui::vulkan::util::CreateComputePipeline(
          vulkan_device, swap_apply_gamma_compute_pipeline_layout_,
          shaders::apply_gamma_table_fxaa_luma_cs, sizeof(shaders::apply_gamma_table_fxaa_luma_cs));
  if (swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline with luma output");
  }
  swap_apply_gamma_compute_pwl_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, swap_apply_gamma_compute_pipeline_layout_, shaders::apply_gamma_pwl_cs,
      sizeof(shaders::apply_gamma_pwl_cs));
  if (swap_apply_gamma_compute_pwl_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN("Failed to create the PWL gamma ramp application compute pipeline");
  }
  swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_ = ui::vulkan::util::CreateComputePipeline(
      vulkan_device, swap_apply_gamma_compute_pipeline_layout_,
      shaders::apply_gamma_pwl_fxaa_luma_cs, sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs));
  if (swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_ == VK_NULL_HANDLE) {
    REXGPU_WARN(
        "Failed to create the PWL gamma ramp application compute pipeline with "
        "luma output");
  }
  if (!vulkan_device->properties().imageViewFormatSwizzle) {
    auto create_rb_swap_compute_pipeline = [&](const char* source, VkPipeline& pipeline_out,
                                               const char* pipeline_name) {
      std::vector<uint32_t> compute_spirv;
      std::string compile_error;
      if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, source, compute_spirv, compile_error)) {
        REXGPU_WARN("Failed to compile {} shader to SPIR-V: {}", pipeline_name, compile_error);
        return;
      }
      pipeline_out = ui::vulkan::util::CreateComputePipeline(
          vulkan_device, swap_apply_gamma_compute_pipeline_layout_, compute_spirv.data(),
          sizeof(uint32_t) * compute_spirv.size());
      if (pipeline_out == VK_NULL_HANDLE) {
        REXGPU_WARN("Failed to create {} pipeline", pipeline_name);
      }
    };
    create_rb_swap_compute_pipeline(GetSwapApplyGammaTableComputeRbSwapSource(),
                                    swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_,
                                    "swap gamma table compute red/blue fallback");
    create_rb_swap_compute_pipeline(GetSwapApplyGammaPwlComputeRbSwapSource(),
                                    swap_apply_gamma_compute_pwl_rb_swap_pipeline_,
                                    "swap gamma PWL compute red/blue fallback");
    create_rb_swap_compute_pipeline(
        GetSwapApplyGammaTableFxaaLumaComputeRbSwapSource(),
        swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_,
        "swap gamma table FXAA-luma compute red/blue fallback");
    create_rb_swap_compute_pipeline(GetSwapApplyGammaPwlFxaaLumaComputeRbSwapSource(),
                                    swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_,
                                    "swap gamma PWL FXAA-luma compute red/blue fallback");
  }

  // FXAA compute pipelines, compiled to SPIR-V at runtime.
  std::vector<uint32_t> swap_fxaa_spirv;
  std::string swap_fxaa_compile_error;
  if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, GetSwapFxaaComputeSource(false),
                          swap_fxaa_spirv, swap_fxaa_compile_error)) {
    REXGPU_WARN("Failed to compile FXAA compute shader to SPIR-V: {}", swap_fxaa_compile_error);
  } else {
    swap_fxaa_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, swap_fxaa_pipeline_layout_, swap_fxaa_spirv.data(),
        sizeof(uint32_t) * swap_fxaa_spirv.size());
    if (swap_fxaa_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_WARN("Failed to create the FXAA compute pipeline");
    }
  }

  std::vector<uint32_t> swap_fxaa_extreme_spirv;
  std::string swap_fxaa_extreme_compile_error;
  if (!CompileGlslToSpirv(VK_SHADER_STAGE_COMPUTE_BIT, GetSwapFxaaComputeSource(true),
                          swap_fxaa_extreme_spirv, swap_fxaa_extreme_compile_error)) {
    REXGPU_WARN("Failed to compile extreme FXAA compute shader to SPIR-V: {}",
                swap_fxaa_extreme_compile_error);
  } else {
    swap_fxaa_extreme_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, swap_fxaa_pipeline_layout_, swap_fxaa_extreme_spirv.data(),
        sizeof(uint32_t) * swap_fxaa_extreme_spirv.size());
    if (swap_fxaa_extreme_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_WARN("Failed to create the extreme-quality FXAA compute pipeline");
    }
  }

  VkPipelineLayoutCreateInfo resolve_downscale_layout_create_info = {};
  resolve_downscale_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkDescriptorSetLayout resolve_downscale_set_layout =
      descriptor_set_layouts_single_transient_[size_t(
          SingleTransientDescriptorLayout::kStorageBufferPairCompute)];
  resolve_downscale_layout_create_info.setLayoutCount = 1;
  resolve_downscale_layout_create_info.pSetLayouts = &resolve_downscale_set_layout;
  VkPushConstantRange resolve_downscale_push_constant_range = {};
  resolve_downscale_push_constant_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  resolve_downscale_push_constant_range.offset = 0;
  resolve_downscale_push_constant_range.size = sizeof(ResolveDownscaleConstants);
  resolve_downscale_layout_create_info.pushConstantRangeCount = 1;
  resolve_downscale_layout_create_info.pPushConstantRanges = &resolve_downscale_push_constant_range;
  if (dfn.vkCreatePipelineLayout(device, &resolve_downscale_layout_create_info, nullptr,
                                 &resolve_downscale_pipeline_layout_) == VK_SUCCESS) {
    resolve_downscale_pipeline_ = ui::vulkan::util::CreateComputePipeline(
        vulkan_device, resolve_downscale_pipeline_layout_, shaders::resolve_downscale_cs,
        sizeof(shaders::resolve_downscale_cs));
    if (resolve_downscale_pipeline_ == VK_NULL_HANDLE) {
      REXGPU_WARN("Failed to create Vulkan resolve-downscale readback pipeline");
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                             resolve_downscale_pipeline_layout_);
    }
  } else {
    REXGPU_WARN("Failed to create Vulkan resolve-downscale pipeline layout");
  }

  occlusion_query_resources_available_ = InitializeOcclusionQueryResources();

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));

  double setup_ms_total = setup_ms_since_start();
  REXGPU_INFO(
      "Vulkan setup took {:.0f} ms: base {:.0f}, driver pipeline cache {:.0f}, shared memory and "
      "primitives {:.0f}, render targets {:.0f}, pipeline cache {:.0f}, textures and the rest {:.0f}",
      setup_ms_total, setup_ms_before_driver_cache,
      setup_ms_shared_memory - setup_ms_before_driver_cache,
      setup_ms_render_targets - setup_ms_shared_memory,
      setup_ms_pipelines - setup_ms_render_targets, setup_ms_textures - setup_ms_pipelines,
      setup_ms_total - setup_ms_textures);
  setup_failure_note.ok = true;
  return true;
}

void VulkanCommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();
  InvalidateAllVertexBufferResidency();
  ShutdownOcclusionQueryResources();
  GpuTimingShutdown();

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  DestroyScratchBuffer();

  for (auto& readback_pair : readback_buffers_) {
    ReadbackBuffer& readback = readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.mapped_data[i] && readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[i]);
      }
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, readback.buffers[i]);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, readback.memories[i]);
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  readback_buffers_.clear();
  for (auto& readback_pair : memexport_readback_buffers_) {
    ReadbackBuffer& readback = readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.mapped_data[i] && readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[i]);
      }
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, readback.buffers[i]);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, readback.memories[i]);
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  memexport_readback_buffers_.clear();

  resolve_downscale_buffer_size_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, resolve_downscale_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         resolve_downscale_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         resolve_downscale_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         resolve_downscale_pipeline_layout_);

  for (SwapFramebuffer& swap_framebuffer : swap_framebuffers_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                           swap_framebuffer.framebuffer);
  }
  DestroySwapFxaaSourceImage();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_fxaa_extreme_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device, swap_fxaa_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_pwl_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_pwl_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device, swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device,
      swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_compute_256_entry_table_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(
      dfn.vkDestroyPipeline, device, swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_pwl_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_256_entry_table_rb_swap_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_pwl_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_pwl_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_256_entry_table_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipeline, device,
                                         swap_apply_gamma_256_entry_table_fxaa_luma_pipeline_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyRenderPass, device,
                                         swap_apply_gamma_render_pass_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_fxaa_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_apply_gamma_compute_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyPipelineLayout, device,
                                         swap_apply_gamma_pipeline_layout_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroySampler, device, swap_sampler_linear_clamp_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         swap_descriptor_pool_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_descriptor_set_layout_uniform_texel_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_descriptor_set_layout_storage_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_descriptor_set_layout_combined_image_sampler_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         swap_descriptor_set_layout_sampled_image_);
  for (VkBufferView& gamma_ramp_buffer_view : gamma_ramp_buffer_views_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBufferView, device, gamma_ramp_buffer_view);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, gamma_ramp_upload_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                         gamma_ramp_upload_buffer_memory_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, gamma_ramp_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, gamma_ramp_buffer_memory_);

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorPool, device,
                                         shared_memory_and_edram_descriptor_pool_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBufferView, device,
                                         shared_memory_texel_buffer_view_);

  if (shared_memory_) {
    shared_memory_->SetRangeRequestListener(nullptr);
  }

  texture_cache_.reset();

  pipeline_cache_.reset();

  render_target_cache_.reset();

  primitive_processor_.reset();

  shared_memory_.reset();

  ClearTransientDescriptorPools();

  for (const auto& pipeline_layout_pair : pipeline_layouts_) {
    dfn.vkDestroyPipelineLayout(device, pipeline_layout_pair.second.GetPipelineLayout(), nullptr);
  }
  for (const auto& library_layout_pair : library_layouts_vertex_) {
    dfn.vkDestroyPipelineLayout(device, library_layout_pair.second, nullptr);
  }
  library_layouts_vertex_.clear();
  for (const auto& library_layout_pair : library_layouts_pixel_) {
    dfn.vkDestroyPipelineLayout(device, library_layout_pair.second, nullptr);
  }
  library_layouts_pixel_.clear();
  pipeline_layouts_.clear();
  for (const auto& descriptor_set_layout_pair : descriptor_set_layouts_textures_) {
    dfn.vkDestroyDescriptorSetLayout(device, descriptor_set_layout_pair.second, nullptr);
  }
  descriptor_set_layouts_textures_.clear();

  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_shared_memory_and_edram_);
  for (VkDescriptorSetLayout& descriptor_set_layout_single_transient :
       descriptor_set_layouts_single_transient_) {
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                           descriptor_set_layout_single_transient);
  }
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_constants_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyDescriptorSetLayout, device,
                                         descriptor_set_layout_empty_);

  uniform_buffer_pool_.reset();

  sparse_bind_wait_stage_mask_ = 0;
  sparse_buffer_binds_.clear();
  sparse_memory_binds_.clear();

  deferred_command_buffer_.Reset();
  for (const auto& command_buffer_pair : command_buffers_submitted_) {
    dfn.vkDestroyCommandPool(device, command_buffer_pair.second.pool, nullptr);
  }
  command_buffers_submitted_.clear();
  for (const CommandBuffer& command_buffer : command_buffers_writable_) {
    dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
  }
  command_buffers_writable_.clear();

  for (const auto& destroy_pair : destroy_framebuffers_) {
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
  }
  destroy_framebuffers_.clear();
  for (const auto& destroy_pair : destroy_image_views_) {
    dfn.vkDestroyImageView(device, destroy_pair.second, nullptr);
  }
  destroy_image_views_.clear();
  for (const auto& destroy_pair : destroy_buffers_) {
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
  }
  destroy_buffers_.clear();
  for (const auto& destroy_pair : destroy_images_) {
    dfn.vkDestroyImage(device, destroy_pair.second, nullptr);
  }
  destroy_images_.clear();
  for (const auto& destroy_pair : destroy_memory_) {
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
  }
  destroy_memory_.clear();

  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));
  frame_completed_ = 0;
  frame_current_ = 1;
  frame_open_ = false;

  for (const auto& semaphore : submissions_in_flight_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore.second, nullptr);
  }
  submissions_in_flight_semaphores_.clear();
  for (VkFence& fence : submissions_in_flight_fences_) {
    dfn.vkDestroyFence(device, fence, nullptr);
  }
  submissions_in_flight_fences_.clear();
  current_submission_wait_stage_masks_.clear();
  for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  current_submission_wait_semaphores_.clear();
  submission_completed_ = 0;
  submission_open_ = false;

  for (VkSemaphore semaphore : semaphores_free_) {
    dfn.vkDestroySemaphore(device, semaphore, nullptr);
  }
  semaphores_free_.clear();
  for (VkFence fence : fences_free_) {
    dfn.vkDestroyFence(device, fence, nullptr);
  }
  fences_free_.clear();

  device_lost_ = false;

  CommandProcessor::ShutdownContext();
}

void VulkanCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (frame_open_) {
      uint32_t float_constant_index = (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
        }
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                  6);
    }
    InvalidateVertexBufferResidency((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 2);
  }
}

void VulkanCommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                   uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  uint32_t end_index = start_index + num_registers - 1;

  auto range_has_any_constant_usage = [](const uint64_t* usage_map, uint32_t first_constant,
                                         uint32_t last_constant) -> bool {
    if (first_constant > last_constant) {
      return false;
    }
    uint32_t first_word = first_constant >> 6;
    uint32_t last_word = last_constant >> 6;
    uint32_t first_bit = first_constant & 63;
    uint32_t last_bit = last_constant & 63;
    if (first_word == last_word) {
      uint32_t bit_count = last_bit - first_bit + 1;
      uint64_t mask = bit_count == 64 ? UINT64_MAX : ((UINT64_C(1) << bit_count) - 1) << first_bit;
      return (usage_map[first_word] & mask) != 0;
    }
    if (usage_map[first_word] & (UINT64_MAX << first_bit)) {
      return true;
    }
    for (uint32_t word = first_word + 1; word < last_word; ++word) {
      if (usage_map[word]) {
        return true;
      }
    }
    uint64_t last_mask = last_bit == 63 ? UINT64_MAX : ((UINT64_C(1) << (last_bit + 1)) - 1);
    return (usage_map[last_word] & last_mask) != 0;
  };

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    if (frame_open_) {
      uint32_t first_float_constant = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      uint32_t last_float_constant = (end_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (first_float_constant < 256) {
        uint32_t last_vertex_constant = std::min(last_float_constant, 255u);
        if (range_has_any_constant_usage(current_float_constant_map_vertex_, first_float_constant,
                                         last_vertex_constant)) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
        }
      }
      if (last_float_constant >= 256) {
        uint32_t first_pixel_constant =
            first_float_constant >= 256 ? first_float_constant - 256 : 0;
        uint32_t last_pixel_constant = last_float_constant - 256;
        if (range_has_any_constant_usage(current_float_constant_map_pixel_, first_pixel_constant,
                                         last_pixel_constant)) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      }
    }
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
    uint32_t first_fetch_dword = start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    uint32_t last_fetch_dword = end_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(first_fetch_dword / 6, last_fetch_dword / 6);
    }
    InvalidateVertexBufferResidencyRange(first_fetch_dword / 2, last_fetch_dword / 2);
    return;
  }

  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void VulkanCommandProcessor::SparseBindBuffer(VkBuffer buffer, uint32_t bind_count,
                                              const VkSparseMemoryBind* binds,
                                              VkPipelineStageFlags wait_stage_mask) {
  if (!bind_count) {
    return;
  }
  SparseBufferBind& buffer_bind = sparse_buffer_binds_.emplace_back();
  buffer_bind.buffer = buffer;
  buffer_bind.bind_offset = sparse_memory_binds_.size();
  buffer_bind.bind_count = bind_count;
  sparse_memory_binds_.reserve(sparse_memory_binds_.size() + bind_count);
  sparse_memory_binds_.insert(sparse_memory_binds_.end(), binds, binds + bind_count);
  sparse_bind_wait_stage_mask_ |= wait_stage_mask;
}

void VulkanCommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_current_frame_ = UINT32_MAX;
}

void VulkanCommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_current_frame_ = UINT32_MAX;
}

void VulkanCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");
  lr_crossfade_this_frame_ = false;
  // vulkan_scene_fxaa: the front buffers and the frame size (VulkanRenderTargetCache::IsSceneFxaaResolve).
  if (frontbuffer_ptr && !IsRecentFrontBuffer(frontbuffer_ptr)) {
    recent_frontbuffers_[recent_frontbuffer_next_] = frontbuffer_ptr;
    recent_frontbuffer_next_ = (recent_frontbuffer_next_ + 1) % uint32_t(rex::countof(recent_frontbuffers_));
  }
  last_swap_width_ = frontbuffer_width;
  last_swap_height_ = frontbuffer_height;
  scene_fxaa_address_ = scene_fxaa_candidate_;
  scene_fxaa_candidate_ = 0;
  FinishResolveDump();  // DS2 diagnostic (resolve_dump_dir)
  CpFrameOnSwap();
  if (gpu_timing_enabled_) {
    GpuTimingOnSwap();
  }
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;

  if (!graphics_system_)
    return;
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    REXGPU_ERROR("XELOG_GPU PRESENT: NO PRESENTER");
    return;
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    REXGPU_ERROR("XELOG_GPU PRESENT: BeginSubmission FAILED");
    return;
  }

  bool skip_present_due_async_placeholder = REXCVAR_GET(async_shader_compilation) &&
                                            REXCVAR_GET(vulkan_async_skip_incomplete_frames) &&
                                            frame_used_async_placeholder_pipeline_;
  LogPipelineCountersIfDue();
  if (skip_present_due_async_placeholder) {
    ++counter_frames_held_;
  } else {
    ++counter_frames_presented_;
  }
  if (skip_present_due_async_placeholder) {
    static bool skipped_incomplete_frame_logged = false;
    if (!skipped_incomplete_frame_logged) {
      skipped_incomplete_frame_logged = true;
      REXGPU_WARN(
          "Skipping Vulkan frame presentation due to async placeholder draw "
          "usage in this frame");
    }
    EndSubmission(true);
    return;
  }

  SwapPostEffect swap_post_effect = GetActualSwapPostEffect();

  // Obtain the actual swap source texture size (resolution-scaled if it's a
  // resolve destination, or not otherwise).
  uint32_t frontbuffer_width_scaled, frontbuffer_height_scaled;
  uint32_t frontbuffer_width_unscaled = 0, frontbuffer_height_unscaled = 0;
  xenos::TextureFormat frontbuffer_format;
  bool swap_source_needs_rb_swap = false;
  VkImageView swap_texture_view = texture_cache_->RequestSwapTexture(
      frontbuffer_width_scaled, frontbuffer_height_scaled, frontbuffer_format,
      &frontbuffer_width_unscaled, &frontbuffer_height_unscaled, &swap_source_needs_rb_swap);
  if (swap_texture_view == VK_NULL_HANDLE) {
    REXGPU_ERROR("XELOG_GPU PRESENT: swap_texture_view=NULL");
    return;
  }
  // The swap gamma / FXAA pass samples source texels by pixel index, but swap
  // textures may be allocation-padded. Prefer the active frontbuffer region
  // from the swap packet, scaled proportionally to the actual source texture.
  auto get_active_swap_dimension = [](uint32_t packet_unscaled, uint32_t source_unscaled,
                                      uint32_t source_scaled) -> uint32_t {
    if (!source_scaled) {
      return 0;
    }
    uint32_t active_unscaled = packet_unscaled ? packet_unscaled : source_unscaled;
    if (!active_unscaled) {
      return source_scaled;
    }
    if (source_unscaled) {
      active_unscaled = std::min(active_unscaled, source_unscaled);
      uint64_t active_scaled =
          (uint64_t(active_unscaled) * source_scaled + (source_unscaled >> 1)) / source_unscaled;
      return uint32_t(std::clamp<uint64_t>(active_scaled, 1, source_scaled));
    }
    return std::min(active_unscaled, source_scaled);
  };
  uint32_t guest_output_width = get_active_swap_dimension(
      frontbuffer_width, frontbuffer_width_unscaled, frontbuffer_width_scaled);
  uint32_t guest_output_height = get_active_swap_dimension(
      frontbuffer_height, frontbuffer_height_unscaled, frontbuffer_height_scaled);
  if (!guest_output_width) {
    guest_output_width = frontbuffer_width_scaled
                             ? frontbuffer_width_scaled
                             : (frontbuffer_width ? frontbuffer_width : frontbuffer_width_unscaled);
  }
  if (!guest_output_height) {
    guest_output_height =
        frontbuffer_height_scaled
            ? frontbuffer_height_scaled
            : (frontbuffer_height ? frontbuffer_height : frontbuffer_height_unscaled);
  }
  bool swap_source_scaled = frontbuffer_width_unscaled && frontbuffer_height_unscaled &&
                            (frontbuffer_width_scaled != frontbuffer_width_unscaled ||
                             frontbuffer_height_scaled != frontbuffer_height_unscaled);
  if (texture_cache_->IsDrawResolutionScaled()) {
    static bool draw_scale_cache_mismatch_logged = false;
    uint32_t texture_scale_x = texture_cache_->draw_resolution_scale_x();
    uint32_t texture_scale_y = texture_cache_->draw_resolution_scale_y();
    uint32_t rt_scale_x =
        render_target_cache_ ? render_target_cache_->draw_resolution_scale_x() : texture_scale_x;
    uint32_t rt_scale_y =
        render_target_cache_ ? render_target_cache_->draw_resolution_scale_y() : texture_scale_y;
    if (!draw_scale_cache_mismatch_logged &&
        (texture_scale_x != rt_scale_x || texture_scale_y != rt_scale_y)) {
      draw_scale_cache_mismatch_logged = true;
      REXGPU_WARN(
          "Vulkan draw-scale mismatch: texture cache is {}x{}, render target "
          "cache is {}x{}",
          texture_scale_x, texture_scale_y, rt_scale_x, rt_scale_y);
    }
    static bool draw_scale_swap_sizes_logged = false;
    if (!draw_scale_swap_sizes_logged) {
      draw_scale_swap_sizes_logged = true;
      REXGPU_WARN(
          "Vulkan draw-scale swap sizing: packet={}x{}, src_scaled={}x{}, "
          "src_unscaled={}x{}, active={}x{}",
          frontbuffer_width, frontbuffer_height, frontbuffer_width_scaled,
          frontbuffer_height_scaled, frontbuffer_width_unscaled, frontbuffer_height_unscaled,
          guest_output_width, guest_output_height);
    }
  }
  if (texture_cache_->IsDrawResolutionScaled() && !swap_source_scaled) {
    static bool draw_scale_swap_unscaled_logged = false;
    if (!draw_scale_swap_unscaled_logged) {
      draw_scale_swap_unscaled_logged = true;
      REXGPU_WARN(
          "Vulkan draw resolution scaling is enabled, but the swap source is "
          "unscaled ({}x{}). This title may be presenting from an unscaled "
          "resolve path.",
          frontbuffer_width_scaled, frontbuffer_height_scaled);
    }
  }
  REXGPU_DEBUG(
      "XELOG_GPU PRESENT: swap_texture_view={:p} packet_size={}x{} src_size={}x{} "
      "src_unscaled={}x{} guest_output_size={}x{} format={}",
      static_cast<void*>(swap_texture_view), frontbuffer_width, frontbuffer_height,
      frontbuffer_width_scaled, frontbuffer_height_scaled, frontbuffer_width_unscaled,
      frontbuffer_height_unscaled, guest_output_width, guest_output_height,
      static_cast<uint32_t>(frontbuffer_format));

  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));

  // The present (guest output refresh: acquire, paint, queue present) runs on this thread: its time is logged with
  // the next frame's thread breakdown ("present"), to tell a blocking present from translation work.
  const uint64_t present_start_ns = CpNowNs();
  rex::system::frame_breakdown::TimelineMark('q');
  presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,
      [this, guest_output_width, guest_output_height, frontbuffer_format, swap_texture_view,
       swap_post_effect,
       swap_source_needs_rb_swap](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        // In case the swap command is the only one in the frame.
        if (!BeginSubmission(true)) {
          return false;
        }

        auto& vulkan_context =
            static_cast<ui::vulkan::VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        uint64_t guest_output_image_version = vulkan_context.image_version();

        const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
        const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
        const VkDevice device = vulkan_device->device();

        uint32_t swap_frame_index = uint32_t(frame_current_ % kMaxFramesInFlight);
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
        bool swap_source_requires_compute_rb_swap =
            !vulkan_device->properties().imageViewFormatSwizzle && swap_source_needs_rb_swap;
        auto select_swap_apply_gamma_compute_pipeline = [&](bool use_pwl,
                                                            bool use_fxaa_luma) -> VkPipeline {
          if (use_pwl) {
            if (use_fxaa_luma) {
              return swap_source_requires_compute_rb_swap
                         ? swap_apply_gamma_compute_pwl_fxaa_luma_rb_swap_pipeline_
                         : swap_apply_gamma_compute_pwl_fxaa_luma_pipeline_;
            }
            return swap_source_requires_compute_rb_swap
                       ? swap_apply_gamma_compute_pwl_rb_swap_pipeline_
                       : swap_apply_gamma_compute_pwl_pipeline_;
          }
          if (use_fxaa_luma) {
            return swap_source_requires_compute_rb_swap
                       ? swap_apply_gamma_compute_256_entry_table_fxaa_luma_rb_swap_pipeline_
                       : swap_apply_gamma_compute_256_entry_table_fxaa_luma_pipeline_;
          }
          return swap_source_requires_compute_rb_swap
                     ? swap_apply_gamma_compute_256_entry_table_rb_swap_pipeline_
                     : swap_apply_gamma_compute_256_entry_table_pipeline_;
        };
        VkPipeline swap_apply_gamma_compute_pipeline =
            select_swap_apply_gamma_compute_pipeline(use_pwl_gamma_ramp, use_fxaa);

        if (use_fxaa) {
          if (swap_apply_gamma_compute_pipeline == VK_NULL_HANDLE ||
              swap_fxaa_pipeline_ == VK_NULL_HANDLE ||
              swap_fxaa_extreme_pipeline_ == VK_NULL_HANDLE) {
            static bool fxaa_pipelines_unavailable_logged = false;
            if (!fxaa_pipelines_unavailable_logged) {
              if (swap_source_requires_compute_rb_swap) {
                REXGPU_WARN(
                    "Vulkan FXAA swap effect requested but FXAA compute "
                    "pipelines (including RB-swap fallback) are unavailable, "
                    "falling back to gamma only");
              } else {
                REXGPU_WARN(
                    "Vulkan FXAA swap effect requested but FXAA compute "
                    "pipelines are unavailable, falling back to gamma only");
              }
              fxaa_pipelines_unavailable_logged = true;
            }
            use_fxaa = false;
          } else if (!EnsureSwapFxaaSourceImage(guest_output_width, guest_output_height)) {
            static bool fxaa_source_image_failed_logged = false;
            if (!fxaa_source_image_failed_logged) {
              REXGPU_WARN(
                  "Failed to create the Vulkan FXAA source image, falling "
                  "back to gamma-only presentation");
              fxaa_source_image_failed_logged = true;
            }
            use_fxaa = false;
          }
        }
        if (!use_fxaa) {
          swap_apply_gamma_compute_pipeline =
              select_swap_apply_gamma_compute_pipeline(use_pwl_gamma_ramp, false);
        }
        if (swap_source_requires_compute_rb_swap &&
            swap_apply_gamma_compute_pipeline == VK_NULL_HANDLE) {
          static bool compute_rb_swap_fallback_missing_logged = false;
          if (!compute_rb_swap_fallback_missing_logged) {
            compute_rb_swap_fallback_missing_logged = true;
            REXGPU_WARN(
                "Vulkan imageViewFormatSwizzle is unavailable and the swap "
                "source needs red/blue swizzle, but the compute fallback "
                "pipeline is unavailable; falling back to graphics "
                "presentation path");
          }
        }
        bool use_compute_gamma = swap_apply_gamma_compute_pipeline != VK_NULL_HANDLE;

        // TODO(Triang3l): FXAA can result in more than 8 bits of precision.
        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        // Update the gamma ramp if it's out of date.
        uint32_t& gamma_ramp_frame_index_ref = use_pwl_gamma_ramp
                                                   ? gamma_ramp_pwl_current_frame_
                                                   : gamma_ramp_256_entry_table_current_frame_;
        if (gamma_ramp_frame_index_ref == UINT32_MAX) {
          constexpr uint32_t kGammaRampSize256EntryTable = sizeof(uint32_t) * 256;
          constexpr uint32_t kGammaRampSizePWL = sizeof(uint16_t) * 2 * 3 * 128;
          constexpr uint32_t kGammaRampSize = kGammaRampSize256EntryTable + kGammaRampSizePWL;
          uint32_t gamma_ramp_offset_in_frame =
              use_pwl_gamma_ramp ? kGammaRampSize256EntryTable : 0;
          uint32_t gamma_ramp_upload_offset =
              kGammaRampSize * swap_frame_index + gamma_ramp_offset_in_frame;
          uint32_t gamma_ramp_size =
              use_pwl_gamma_ramp ? kGammaRampSizePWL : kGammaRampSize256EntryTable;
          void* gamma_ramp_frame_upload =
              reinterpret_cast<uint8_t*>(gamma_ramp_upload_mapping_) + gamma_ramp_upload_offset;
          if (std::endian::native != std::endian::little && use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload =
                reinterpret_cast<reg::DC_LUT_PWL_DATA*>(gamma_ramp_frame_upload);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_entry = gamma_ramp_pwl_upload[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(gamma_ramp_frame_upload,
                        use_pwl_gamma_ramp ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                                           : static_cast<const void*>(gamma_ramp_256_entry_table()),
                        gamma_ramp_size);
          }
          bool gamma_ramp_has_upload_buffer = gamma_ramp_upload_buffer_memory_ != VK_NULL_HANDLE;
          VkPipelineStageFlags gamma_ramp_read_stage_mask =
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
          ui::vulkan::util::FlushMappedMemoryRange(
              vulkan_device,
              gamma_ramp_has_upload_buffer ? gamma_ramp_upload_buffer_memory_
                                           : gamma_ramp_buffer_memory_,
              gamma_ramp_upload_memory_type_, gamma_ramp_upload_offset,
              gamma_ramp_upload_memory_size_, gamma_ramp_size);
          if (gamma_ramp_has_upload_buffer) {
            // Copy from the host-visible buffer to the device-local one.
            PushBufferMemoryBarrier(gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                                    gamma_ramp_read_stage_mask, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
            SubmitBarriers(true);
            GpuTimingMark(GpuTimingCategory::kSwap);
            VkBufferCopy gamma_ramp_buffer_copy;
            gamma_ramp_buffer_copy.srcOffset = gamma_ramp_upload_offset;
            gamma_ramp_buffer_copy.dstOffset = gamma_ramp_offset_in_frame;
            gamma_ramp_buffer_copy.size = gamma_ramp_size;
            deferred_command_buffer_.CmdVkCopyBuffer(gamma_ramp_upload_buffer_, gamma_ramp_buffer_,
                                                     1, &gamma_ramp_buffer_copy);
            PushBufferMemoryBarrier(gamma_ramp_buffer_, gamma_ramp_offset_in_frame, gamma_ramp_size,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, gamma_ramp_read_stage_mask,
                                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, false);
          }
          // The device-local, but not host-visible, gamma ramp buffer doesn't
          // have per-frame sets of gamma ramps.
          gamma_ramp_frame_index_ref = gamma_ramp_has_upload_buffer ? 0 : swap_frame_index;
        }

        VkDescriptorSet swap_descriptor_source = swap_descriptors_source_[swap_frame_index];
        VkDescriptorImageInfo swap_descriptor_source_image_info;
        swap_descriptor_source_image_info.sampler = VK_NULL_HANDLE;
        swap_descriptor_source_image_info.imageView = swap_texture_view;
        swap_descriptor_source_image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet swap_descriptor_source_write;
        swap_descriptor_source_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        swap_descriptor_source_write.pNext = nullptr;
        swap_descriptor_source_write.dstSet = swap_descriptor_source;
        swap_descriptor_source_write.dstBinding = 0;
        swap_descriptor_source_write.dstArrayElement = 0;
        swap_descriptor_source_write.descriptorCount = 1;
        swap_descriptor_source_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        swap_descriptor_source_write.pImageInfo = &swap_descriptor_source_image_info;
        swap_descriptor_source_write.pBufferInfo = nullptr;
        swap_descriptor_source_write.pTexelBufferView = nullptr;
        dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_source_write, 0, nullptr);

        VkImageSubresourceRange guest_output_subresource_range =
            ui::vulkan::util::InitializeSubresourceRange();
        if (use_compute_gamma) {
          // Transition the destination image for compute writes. Contents are
          // fully overwritten, so old layout can always be UNDEFINED.
          PushImageMemoryBarrier(vulkan_context.image(), guest_output_subresource_range,
                                 vulkan_context.image_ever_written_previously()
                                     ? ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask
                                     : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 vulkan_context.image_ever_written_previously()
                                     ? ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask
                                     : 0,
                                 VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_GENERAL);
          if (use_fxaa) {
            PushImageMemoryBarrier(swap_fxaa_source_image_, guest_output_subresource_range,
                                   swap_fxaa_source_stage_mask_ ? swap_fxaa_source_stage_mask_
                                                                : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   swap_fxaa_source_access_mask_, VK_ACCESS_SHADER_WRITE_BIT,
                                   swap_fxaa_source_layout_, VK_IMAGE_LAYOUT_GENERAL);
          }
          SubmitBarriers(true);
          GpuTimingMark(GpuTimingCategory::kSwap);

          if (use_fxaa) {
            swap_fxaa_source_stage_mask_ = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            swap_fxaa_source_access_mask_ = VK_ACCESS_SHADER_WRITE_BIT;
            swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_GENERAL;
            swap_fxaa_source_image_submission_ = GetCurrentSubmission();
          }

          VkDescriptorSet swap_descriptor_destination_storage =
              swap_descriptors_destination_storage_[swap_frame_index];
          VkDescriptorImageInfo swap_descriptor_destination_storage_image_info;
          swap_descriptor_destination_storage_image_info.sampler = VK_NULL_HANDLE;
          swap_descriptor_destination_storage_image_info.imageView =
              use_fxaa ? swap_fxaa_source_image_view_ : vulkan_context.image_view();
          swap_descriptor_destination_storage_image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
          VkWriteDescriptorSet swap_descriptor_destination_storage_write;
          swap_descriptor_destination_storage_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          swap_descriptor_destination_storage_write.pNext = nullptr;
          swap_descriptor_destination_storage_write.dstSet = swap_descriptor_destination_storage;
          swap_descriptor_destination_storage_write.dstBinding = 0;
          swap_descriptor_destination_storage_write.dstArrayElement = 0;
          swap_descriptor_destination_storage_write.descriptorCount = 1;
          swap_descriptor_destination_storage_write.descriptorType =
              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
          swap_descriptor_destination_storage_write.pImageInfo =
              &swap_descriptor_destination_storage_image_info;
          swap_descriptor_destination_storage_write.pBufferInfo = nullptr;
          swap_descriptor_destination_storage_write.pTexelBufferView = nullptr;
          dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_destination_storage_write, 0,
                                     nullptr);

          std::array<VkDescriptorSet, kSwapApplyGammaComputeDescriptorSetCount>
              swap_apply_gamma_compute_descriptor_sets{};
          swap_apply_gamma_compute_descriptor_sets[kSwapApplyGammaComputeDescriptorSetRamp] =
              swap_descriptors_gamma_ramp_[2 * gamma_ramp_frame_index_ref +
                                           uint32_t(use_pwl_gamma_ramp)];
          swap_apply_gamma_compute_descriptor_sets[kSwapApplyGammaComputeDescriptorSetSource] =
              swap_descriptor_source;
          swap_apply_gamma_compute_descriptor_sets[kSwapApplyGammaComputeDescriptorSetDestination] =
              swap_descriptor_destination_storage;
          deferred_command_buffer_.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_COMPUTE, swap_apply_gamma_compute_pipeline_layout_, 0,
              uint32_t(swap_apply_gamma_compute_descriptor_sets.size()),
              swap_apply_gamma_compute_descriptor_sets.data(), 0, nullptr);
          SwapApplyGammaConstants swap_apply_gamma_constants = {
              {guest_output_width, guest_output_height}};
          deferred_command_buffer_.CmdVkPushConstants(
              swap_apply_gamma_compute_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
              sizeof(swap_apply_gamma_constants), &swap_apply_gamma_constants);
          BindExternalComputePipeline(swap_apply_gamma_compute_pipeline);
          uint32_t group_count_x = (guest_output_width + 15) / 16;
          uint32_t group_count_y = (guest_output_height + 7) / 8;
          deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y, 1);

          if (use_fxaa) {
            // Make the FXAA source image readable and bind a separate
            // destination storage descriptor targeting the guest output image.
            PushImageMemoryBarrier(swap_fxaa_source_image_, guest_output_subresource_range,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                                   VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            SubmitBarriers(true);
            swap_fxaa_source_stage_mask_ = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            swap_fxaa_source_access_mask_ = VK_ACCESS_SHADER_READ_BIT;
            swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            swap_fxaa_source_image_submission_ = GetCurrentSubmission();

            VkDescriptorSet swap_descriptor_fxaa_source =
                swap_descriptors_fxaa_source_[swap_frame_index];
            VkDescriptorImageInfo swap_descriptor_fxaa_source_image_info;
            swap_descriptor_fxaa_source_image_info.sampler = swap_sampler_linear_clamp_;
            swap_descriptor_fxaa_source_image_info.imageView = swap_fxaa_source_image_view_;
            swap_descriptor_fxaa_source_image_info.imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet swap_descriptor_fxaa_source_write;
            swap_descriptor_fxaa_source_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            swap_descriptor_fxaa_source_write.pNext = nullptr;
            swap_descriptor_fxaa_source_write.dstSet = swap_descriptor_fxaa_source;
            swap_descriptor_fxaa_source_write.dstBinding = 0;
            swap_descriptor_fxaa_source_write.dstArrayElement = 0;
            swap_descriptor_fxaa_source_write.descriptorCount = 1;
            swap_descriptor_fxaa_source_write.descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            swap_descriptor_fxaa_source_write.pImageInfo = &swap_descriptor_fxaa_source_image_info;
            swap_descriptor_fxaa_source_write.pBufferInfo = nullptr;
            swap_descriptor_fxaa_source_write.pTexelBufferView = nullptr;
            dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_fxaa_source_write, 0, nullptr);

            VkDescriptorSet swap_descriptor_fxaa_destination_storage =
                swap_descriptors_fxaa_destination_storage_[swap_frame_index];
            VkDescriptorImageInfo swap_descriptor_fxaa_destination_storage_image_info;
            swap_descriptor_fxaa_destination_storage_image_info.sampler = VK_NULL_HANDLE;
            swap_descriptor_fxaa_destination_storage_image_info.imageView =
                vulkan_context.image_view();
            swap_descriptor_fxaa_destination_storage_image_info.imageLayout =
                VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet swap_descriptor_fxaa_destination_storage_write =
                swap_descriptor_destination_storage_write;
            swap_descriptor_fxaa_destination_storage_write.dstSet =
                swap_descriptor_fxaa_destination_storage;
            swap_descriptor_fxaa_destination_storage_write.pImageInfo =
                &swap_descriptor_fxaa_destination_storage_image_info;
            dfn.vkUpdateDescriptorSets(device, 1, &swap_descriptor_fxaa_destination_storage_write,
                                       0, nullptr);

            std::array<VkDescriptorSet, kSwapFxaaDescriptorSetCount> swap_fxaa_descriptor_sets{};
            swap_fxaa_descriptor_sets[kSwapFxaaDescriptorSetSource] = swap_descriptor_fxaa_source;
            swap_fxaa_descriptor_sets[kSwapFxaaDescriptorSetDestination] =
                swap_descriptor_fxaa_destination_storage;
            deferred_command_buffer_.CmdVkBindDescriptorSets(
                VK_PIPELINE_BIND_POINT_COMPUTE, swap_fxaa_pipeline_layout_, 0,
                uint32_t(swap_fxaa_descriptor_sets.size()), swap_fxaa_descriptor_sets.data(), 0,
                nullptr);
            SwapFxaaConstants swap_fxaa_constants = {
                {guest_output_width, guest_output_height},
                {1.0f / float(guest_output_width), 1.0f / float(guest_output_height)}};
            deferred_command_buffer_.CmdVkPushConstants(
                swap_fxaa_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                sizeof(swap_fxaa_constants), &swap_fxaa_constants);
            BindExternalComputePipeline(swap_post_effect == SwapPostEffect::kFxaaExtreme
                                            ? swap_fxaa_extreme_pipeline_
                                            : swap_fxaa_pipeline_);
            deferred_command_buffer_.CmdVkDispatch(group_count_x, group_count_y, 1);
          }

          // Insert the release barrier.
          PushImageMemoryBarrier(vulkan_context.image(), guest_output_subresource_range,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                 VK_ACCESS_SHADER_WRITE_BIT,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                                 VK_IMAGE_LAYOUT_GENERAL,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);
        } else {
          // Make sure a framebuffer is available for the current guest output
          // image version.
          size_t swap_framebuffer_index = SIZE_MAX;
          size_t swap_framebuffer_new_index = SIZE_MAX;
          // Try to find the existing framebuffer for the current guest output
          // image version, or an unused (without an existing framebuffer, or
          // with one, but that has never actually been used dynamically) slot.
          for (size_t i = 0; i < swap_framebuffers_.size(); ++i) {
            const SwapFramebuffer& existing_swap_framebuffer = swap_framebuffers_[i];
            if (existing_swap_framebuffer.framebuffer != VK_NULL_HANDLE &&
                existing_swap_framebuffer.version == guest_output_image_version) {
              swap_framebuffer_index = i;
              break;
            }
            if (existing_swap_framebuffer.framebuffer == VK_NULL_HANDLE ||
                !existing_swap_framebuffer.last_submission) {
              swap_framebuffer_new_index = i;
            }
          }
          if (swap_framebuffer_index == SIZE_MAX) {
            if (swap_framebuffer_new_index == SIZE_MAX) {
              // Replace the earliest used framebuffer.
              swap_framebuffer_new_index = 0;
              for (size_t i = 1; i < swap_framebuffers_.size(); ++i) {
                if (swap_framebuffers_[i].last_submission <
                    swap_framebuffers_[swap_framebuffer_new_index].last_submission) {
                  swap_framebuffer_new_index = i;
                }
              }
            }
            swap_framebuffer_index = swap_framebuffer_new_index;
            SwapFramebuffer& new_swap_framebuffer = swap_framebuffers_[swap_framebuffer_new_index];
            if (new_swap_framebuffer.framebuffer != VK_NULL_HANDLE) {
              if (submission_completed_ >= new_swap_framebuffer.last_submission) {
                dfn.vkDestroyFramebuffer(device, new_swap_framebuffer.framebuffer, nullptr);
              } else {
                destroy_framebuffers_.emplace_back(new_swap_framebuffer.last_submission,
                                                   new_swap_framebuffer.framebuffer);
              }
              new_swap_framebuffer.framebuffer = VK_NULL_HANDLE;
            }
            VkImageView guest_output_image_view = vulkan_context.image_view();
            VkFramebufferCreateInfo swap_framebuffer_create_info;
            swap_framebuffer_create_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            swap_framebuffer_create_info.pNext = nullptr;
            swap_framebuffer_create_info.flags = 0;
            swap_framebuffer_create_info.renderPass = swap_apply_gamma_render_pass_;
            swap_framebuffer_create_info.attachmentCount = 1;
            swap_framebuffer_create_info.pAttachments = &guest_output_image_view;
            swap_framebuffer_create_info.width = guest_output_width;
            swap_framebuffer_create_info.height = guest_output_height;
            swap_framebuffer_create_info.layers = 1;
            if (dfn.vkCreateFramebuffer(device, &swap_framebuffer_create_info, nullptr,
                                        &new_swap_framebuffer.framebuffer) != VK_SUCCESS) {
              REXGPU_ERROR("Failed to create the Vulkan framebuffer for presentation");
              return false;
            }
            new_swap_framebuffer.version = guest_output_image_version;
            // The actual submission index will be set if the framebuffer is
            // actually used, not dropped due to some error.
            new_swap_framebuffer.last_submission = 0;
          }

          if (vulkan_context.image_ever_written_previously()) {
            // Insert a barrier after the last presenter's usage of the guest
            // output image. Will be overwriting all the contents, so oldLayout
            // layout is UNDEFINED. The render pass will do the layout
            // transition, but newLayout must not be UNDEFINED.
            PushImageMemoryBarrier(vulkan_context.image(), guest_output_subresource_range,
                                   ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                                   VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
          }

          // End the current render pass before inserting barriers and starting
          // a new one, and insert the barrier.
          SubmitBarriers(true);
          GpuTimingMark(GpuTimingCategory::kSwap);

          SwapFramebuffer& swap_framebuffer = swap_framebuffers_[swap_framebuffer_index];
          swap_framebuffer.last_submission = GetCurrentSubmission();

          VkRenderPassBeginInfo render_pass_begin_info;
          render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
          render_pass_begin_info.pNext = nullptr;
          render_pass_begin_info.renderPass = swap_apply_gamma_render_pass_;
          render_pass_begin_info.framebuffer = swap_framebuffer.framebuffer;
          render_pass_begin_info.renderArea.offset.x = 0;
          render_pass_begin_info.renderArea.offset.y = 0;
          render_pass_begin_info.renderArea.extent.width = guest_output_width;
          render_pass_begin_info.renderArea.extent.height = guest_output_height;
          render_pass_begin_info.clearValueCount = 0;
          render_pass_begin_info.pClearValues = nullptr;
          deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                        VK_SUBPASS_CONTENTS_INLINE);

          VkViewport viewport;
          viewport.x = 0.0f;
          viewport.y = 0.0f;
          viewport.width = float(guest_output_width);
          viewport.height = float(guest_output_height);
          viewport.minDepth = 0.0f;
          viewport.maxDepth = 1.0f;
          SetViewport(viewport);
          VkRect2D scissor;
          scissor.offset.x = 0;
          scissor.offset.y = 0;
          scissor.extent.width = guest_output_width;
          scissor.extent.height = guest_output_height;
          SetScissor(scissor);

          VkPipeline swap_apply_gamma_pipeline = use_pwl_gamma_ramp
                                                     ? swap_apply_gamma_pwl_pipeline_
                                                     : swap_apply_gamma_256_entry_table_pipeline_;
          if (!vulkan_device->properties().imageViewFormatSwizzle && swap_source_needs_rb_swap) {
            VkPipeline swap_apply_gamma_rb_swap_pipeline =
                use_pwl_gamma_ramp ? swap_apply_gamma_pwl_rb_swap_pipeline_
                                   : swap_apply_gamma_256_entry_table_rb_swap_pipeline_;
            if (swap_apply_gamma_rb_swap_pipeline != VK_NULL_HANDLE) {
              swap_apply_gamma_pipeline = swap_apply_gamma_rb_swap_pipeline;
            } else {
              static bool swap_rb_swap_fallback_missing_logged = false;
              if (!swap_rb_swap_fallback_missing_logged) {
                swap_rb_swap_fallback_missing_logged = true;
                REXGPU_WARN(
                    "Vulkan imageViewFormatSwizzle is unavailable and the "
                    "swap source needs red/blue swizzle, but the graphics "
                    "fallback RB-swap shader pipeline is unavailable");
              }
            }
          }
          BindExternalGraphicsPipeline(swap_apply_gamma_pipeline);

          std::array<VkDescriptorSet, kSwapApplyGammaDescriptorSetCount> swap_descriptor_sets{};
          swap_descriptor_sets[kSwapApplyGammaDescriptorSetRamp] =
              swap_descriptors_gamma_ramp_[2 * gamma_ramp_frame_index_ref +
                                           uint32_t(use_pwl_gamma_ramp)];
          swap_descriptor_sets[kSwapApplyGammaDescriptorSetSource] = swap_descriptor_source;
          deferred_command_buffer_.CmdVkBindDescriptorSets(
              VK_PIPELINE_BIND_POINT_GRAPHICS, swap_apply_gamma_pipeline_layout_, 0,
              uint32_t(swap_descriptor_sets.size()), swap_descriptor_sets.data(), 0, nullptr);

          deferred_command_buffer_.CmdVkDraw(3, 1, 0, 0);
          deferred_command_buffer_.CmdVkEndRenderPass();

          // Insert the release barrier.
          PushImageMemoryBarrier(vulkan_context.image(), guest_output_subresource_range,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalStageMask,
                                 VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalAccessMask,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                 ui::vulkan::VulkanPresenter::kGuestOutputInternalLayout);
        }

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue, and also need to submit the release barrier.
        EndSubmission(true);
        return true;
      });

  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  EndSubmission(true);
  cp_frame_.present_ns += CpNowNs() - present_start_ns;
  rex::system::frame_breakdown::TimelineMark('Q');
}

bool VulkanCommandProcessor::PushBufferMemoryBarrier(
    VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size, VkPipelineStageFlags src_stage_mask,
    VkPipelineStageFlags dst_stage_mask, VkAccessFlags src_access_mask,
    VkAccessFlags dst_access_mask, uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask && src_access_mask == dst_access_mask &&
      src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping buffer ranges into different
  // pipeline barrier commands.
  for (const VkBufferMemoryBarrier& other_buffer_memory_barrier :
       pending_barriers_buffer_memory_barriers_) {
    if (other_buffer_memory_barrier.buffer != buffer ||
        (size != VK_WHOLE_SIZE && offset + size <= other_buffer_memory_barrier.offset) ||
        (other_buffer_memory_barrier.size != VK_WHOLE_SIZE &&
         other_buffer_memory_barrier.offset + other_buffer_memory_barrier.size <= offset)) {
      continue;
    }
    if (other_buffer_memory_barrier.offset == offset && other_buffer_memory_barrier.size == size &&
        other_buffer_memory_barrier.srcAccessMask == src_access_mask &&
        other_buffer_memory_barrier.dstAccessMask == dst_access_mask &&
        other_buffer_memory_barrier.srcQueueFamilyIndex == src_queue_family_index &&
        other_buffer_memory_barrier.dstQueueFamilyIndex == dst_queue_family_index) {
      // The barrier is already pending.
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  VkBufferMemoryBarrier& buffer_memory_barrier =
      pending_barriers_buffer_memory_barriers_.emplace_back();
  buffer_memory_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  buffer_memory_barrier.pNext = nullptr;
  buffer_memory_barrier.srcAccessMask = src_access_mask;
  buffer_memory_barrier.dstAccessMask = dst_access_mask;
  buffer_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  buffer_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  buffer_memory_barrier.buffer = buffer;
  buffer_memory_barrier.offset = offset;
  buffer_memory_barrier.size = size;
  return true;
}

bool VulkanCommandProcessor::PushImageMemoryBarrier(
    VkImage image, const VkImageSubresourceRange& subresource_range,
    VkPipelineStageFlags src_stage_mask, VkPipelineStageFlags dst_stage_mask,
    VkAccessFlags src_access_mask, VkAccessFlags dst_access_mask, VkImageLayout old_layout,
    VkImageLayout new_layout, uint32_t src_queue_family_index, uint32_t dst_queue_family_index,
    bool skip_if_equal) {
  if (skip_if_equal && src_stage_mask == dst_stage_mask && src_access_mask == dst_access_mask &&
      old_layout == new_layout && src_queue_family_index == dst_queue_family_index) {
    return false;
  }

  // Separate different barriers for overlapping image subresource ranges into
  // different pipeline barrier commands.
  for (const VkImageMemoryBarrier& other_image_memory_barrier :
       pending_barriers_image_memory_barriers_) {
    if (other_image_memory_barrier.image != image ||
        !(other_image_memory_barrier.subresourceRange.aspectMask & subresource_range.aspectMask) ||
        (subresource_range.levelCount != VK_REMAINING_MIP_LEVELS &&
         subresource_range.baseMipLevel + subresource_range.levelCount <=
             other_image_memory_barrier.subresourceRange.baseMipLevel) ||
        (other_image_memory_barrier.subresourceRange.levelCount != VK_REMAINING_MIP_LEVELS &&
         other_image_memory_barrier.subresourceRange.baseMipLevel +
                 other_image_memory_barrier.subresourceRange.levelCount <=
             subresource_range.baseMipLevel) ||
        (subresource_range.layerCount != VK_REMAINING_ARRAY_LAYERS &&
         subresource_range.baseArrayLayer + subresource_range.layerCount <=
             other_image_memory_barrier.subresourceRange.baseArrayLayer) ||
        (other_image_memory_barrier.subresourceRange.layerCount != VK_REMAINING_ARRAY_LAYERS &&
         other_image_memory_barrier.subresourceRange.baseArrayLayer +
                 other_image_memory_barrier.subresourceRange.layerCount <=
             subresource_range.baseArrayLayer)) {
      continue;
    }
    if (other_image_memory_barrier.subresourceRange.aspectMask == subresource_range.aspectMask &&
        other_image_memory_barrier.subresourceRange.baseMipLevel ==
            subresource_range.baseMipLevel &&
        other_image_memory_barrier.subresourceRange.levelCount == subresource_range.levelCount &&
        other_image_memory_barrier.subresourceRange.baseArrayLayer ==
            subresource_range.baseArrayLayer &&
        other_image_memory_barrier.subresourceRange.layerCount == subresource_range.layerCount &&
        other_image_memory_barrier.srcAccessMask == src_access_mask &&
        other_image_memory_barrier.dstAccessMask == dst_access_mask &&
        other_image_memory_barrier.oldLayout == old_layout &&
        other_image_memory_barrier.newLayout == new_layout &&
        other_image_memory_barrier.srcQueueFamilyIndex == src_queue_family_index &&
        other_image_memory_barrier.dstQueueFamilyIndex == dst_queue_family_index) {
      // The barrier is already pending.
      current_pending_barrier_.src_stage_mask |= src_stage_mask;
      current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
      return true;
    }
    SplitPendingBarrier();
    break;
  }

  current_pending_barrier_.src_stage_mask |= src_stage_mask;
  current_pending_barrier_.dst_stage_mask |= dst_stage_mask;
  VkImageMemoryBarrier& image_memory_barrier =
      pending_barriers_image_memory_barriers_.emplace_back();
  image_memory_barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  image_memory_barrier.pNext = nullptr;
  image_memory_barrier.srcAccessMask = src_access_mask;
  image_memory_barrier.dstAccessMask = dst_access_mask;
  image_memory_barrier.oldLayout = old_layout;
  image_memory_barrier.newLayout = new_layout;
  image_memory_barrier.srcQueueFamilyIndex = src_queue_family_index;
  image_memory_barrier.dstQueueFamilyIndex = dst_queue_family_index;
  image_memory_barrier.image = image;
  image_memory_barrier.subresourceRange = subresource_range;
  return true;
}

bool VulkanCommandProcessor::SubmitBarriers(bool force_end_render_pass) {
  assert_true(submission_open_);
  SplitPendingBarrier();
  if (pending_barriers_.empty()) {
    if (force_end_render_pass) {
      pass_end_reason_ = kPassEndForced;
      EndRenderPass();
    }
    return false;
  }
  if (in_render_pass_ && gpu_timing_enabled_) {
    pass_end_reason_ = kPassEndBarrierBuffer;
    if (!pending_barriers_image_memory_barriers_.empty()) {
      switch (pending_barriers_image_memory_barriers_.front().newLayout) {
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL:
          pass_end_reason_ = kPassEndBarrierRead;
          break;
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
          pass_end_reason_ = kPassEndBarrierAttachment;
          break;
        case VK_IMAGE_LAYOUT_GENERAL:
          pass_end_reason_ = kPassEndBarrierGeneral;
          break;
        default:
          pass_end_reason_ = kPassEndBarrierTransfer;
          break;
      }
    }
  }
  EndRenderPass();
  for (auto it = pending_barriers_.cbegin(); it != pending_barriers_.cend(); ++it) {
    auto it_next = std::next(it);
    bool is_last = it_next == pending_barriers_.cend();
    // .data() + offset, not &[offset], for buffer and image barriers, because
    // if there are no buffer or image memory barriers in the last pipeline
    // barriers, the offsets may be equal to the sizes of the vectors.
    deferred_command_buffer_.CmdVkPipelineBarrier(
        it->src_stage_mask ? it->src_stage_mask : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        it->dst_stage_mask ? it->dst_stage_mask : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
        nullptr,
        uint32_t((is_last ? pending_barriers_buffer_memory_barriers_.size()
                          : it_next->buffer_memory_barriers_offset) -
                 it->buffer_memory_barriers_offset),
        pending_barriers_buffer_memory_barriers_.data() + it->buffer_memory_barriers_offset,
        uint32_t((is_last ? pending_barriers_image_memory_barriers_.size()
                          : it_next->image_memory_barriers_offset) -
                 it->image_memory_barriers_offset),
        pending_barriers_image_memory_barriers_.data() + it->image_memory_barriers_offset);
  }
  pending_barriers_.clear();
  pending_barriers_buffer_memory_barriers_.clear();
  pending_barriers_image_memory_barriers_.clear();
  current_pending_barrier_.buffer_memory_barriers_offset = 0;
  current_pending_barrier_.image_memory_barriers_offset = 0;
  return true;
}

void VulkanCommandProcessor::AttachmentBarrierBetweenPasses() {
  if (!REXCVAR_GET(vulkan_attachment_barrier_between_passes)) {
    return;
  }
  // Render pass instances aren't ordered against each other: a pass loading or
  // writing attachments written by an earlier pass needs a dependency, which
  // the render target cache omits when the attachment usage doesn't change.
  VkMemoryBarrier memory_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  memory_barrier.srcAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  memory_barrier.dstAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  constexpr VkPipelineStageFlags kAttachmentStages =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  deferred_command_buffer_.CmdVkPipelineBarrier(kAttachmentStages, kAttachmentStages, 0, 1,
                                                &memory_barrier, 0, nullptr, 0, nullptr);
}

void VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass(
    VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer) {
  SubmitBarriers(false);
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering =
      REXCVAR_GET(vulkan_dynamic_rendering) && vulkan_device->properties().dynamicRendering;

  if (use_dynamic_rendering) {
    if (in_render_pass_ && current_framebuffer_ == framebuffer &&
        current_render_pass_ == VK_NULL_HANDLE) {
      return;
    }
  } else {
    if (current_render_pass_ == render_pass && current_framebuffer_ == framebuffer) {
      return;
    }
  }

  if (in_render_pass_) {
    GpuTimingNotePassEnd(kPassEndFramebuffer);
    RenderPassAreaEnd();
    if (use_dynamic_rendering) {
      deferred_command_buffer_.CmdVkEndRendering();
    } else {
      deferred_command_buffer_.CmdVkEndRenderPass();
    }
    in_render_pass_ = false;
  }
  AttachmentBarrierBetweenPasses();

  current_render_pass_ = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  current_framebuffer_ = framebuffer;

  ++gpu_timing_counters_.render_passes;
  if (gpu_timing_enabled_) {
    gpu_timing_pass_tag_ =
        uint64_t(render_target_cache_->last_update_render_pass_key().key) |
        (uint64_t(render_target_cache_->last_update_framebuffer_pitch_tiles_at_32bpp()) << 32);
    ++gpu_timing_tag_stats_[gpu_timing_pass_tag_].passes;
    if (gpu_trace_active_) {
      gpu_trace_pass_draws_ = 0;
      GpuTrace("P" + GpuTimingDescribeTag(gpu_timing_pass_tag_));
    }
  }
  GpuTimingMark(GpuTimingCategory::kDraws);

  if (use_dynamic_rendering) {
    VkRenderingAttachmentInfo color_attachments[xenos::kMaxColorRenderTargets];
    VkRenderingAttachmentInfo depth_attachment;
    VkRenderingAttachmentInfo stencil_attachment;
    uint32_t color_attachment_count = 0;
    render_target_cache_->GetLastUpdateRenderingAttachments(
        color_attachments, &color_attachment_count, &depth_attachment, &stencil_attachment);
    bool has_depth = depth_attachment.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    bool has_stencil = stencil_attachment.sType == VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    if (gpu_timing_enabled_) {
      bool any_load = (has_depth && depth_attachment.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) ||
                      (has_stencil && stencil_attachment.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
      for (uint32_t i = 0; i < color_attachment_count; ++i) {
        any_load |= color_attachments[i].imageView != VK_NULL_HANDLE &&
                    color_attachments[i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD;
      }
      gpu_timing_counters_.render_passes_with_load += uint64_t(any_load);
    }

    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.pNext = nullptr;
    rendering_info.flags = 0;
    rendering_info.renderArea.offset.x = 0;
    rendering_info.renderArea.offset.y = 0;
    rendering_info.renderArea.extent = framebuffer->host_extent;
    rendering_info.layerCount = 1;
    rendering_info.viewMask = 0;
    rendering_info.colorAttachmentCount = color_attachment_count;
    rendering_info.pColorAttachments = color_attachment_count ? color_attachments : nullptr;
    rendering_info.pDepthAttachment = has_depth ? &depth_attachment : nullptr;
    rendering_info.pStencilAttachment = has_stencil ? &stencil_attachment : nullptr;
    deferred_command_buffer_.CmdVkBeginRendering(&rendering_info);
  } else {
    // All render target cache render passes load every attachment.
    ++gpu_timing_counters_.render_passes_with_load;
    VkRenderPassBeginInfo render_pass_begin_info;
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.pNext = nullptr;
    render_pass_begin_info.renderPass = render_pass;
    render_pass_begin_info.framebuffer = framebuffer->framebuffer;
    render_pass_begin_info.renderArea.offset.x = 0;
    render_pass_begin_info.renderArea.offset.y = 0;
    // TODO(Triang3l): Actual dirty width / height in the deferred command
    // buffer.
    render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
    render_pass_begin_info.clearValueCount = 0;
    render_pass_begin_info.pClearValues = nullptr;
    deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                  VK_SUBPASS_CONTENTS_INLINE);
  }
  in_render_pass_ = true;
  RenderPassAreaBegin(framebuffer->host_extent);
}

void VulkanCommandProcessor::SubmitBarriersAndEnterRenderTargetCacheRenderPass(
    VkRenderPass render_pass, const VulkanRenderTargetCache::Framebuffer* framebuffer,
    VkImageView transfer_dest_view, bool transfer_dest_is_depth) {
  SubmitBarriers(false);
  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  bool use_dynamic_rendering =
      REXCVAR_GET(vulkan_dynamic_rendering) && vulkan_device->properties().dynamicRendering;

  if (use_dynamic_rendering) {
    if (in_render_pass_ && current_framebuffer_ == framebuffer &&
        current_render_pass_ == VK_NULL_HANDLE) {
      return;
    }
  } else {
    if (current_render_pass_ == render_pass && current_framebuffer_ == framebuffer) {
      return;
    }
  }

  if (in_render_pass_) {
    GpuTimingNotePassEnd(kPassEndFramebuffer);
    RenderPassAreaEnd();
    if (use_dynamic_rendering) {
      deferred_command_buffer_.CmdVkEndRendering();
    } else {
      deferred_command_buffer_.CmdVkEndRenderPass();
    }
    in_render_pass_ = false;
  }
  AttachmentBarrierBetweenPasses();

  current_render_pass_ = use_dynamic_rendering ? VK_NULL_HANDLE : render_pass;
  current_framebuffer_ = framebuffer;

  // Transfer render passes always load the destination.
  ++gpu_timing_counters_.render_passes;
  ++gpu_timing_counters_.render_passes_with_load;
  if (gpu_timing_enabled_) {
    ++gpu_timing_tag_stats_[gpu_timing_pass_tag_].passes;
    if (gpu_trace_active_) {
      gpu_trace_pass_draws_ = 0;
      GpuTrace("P" + GpuTimingDescribeTag(gpu_timing_pass_tag_));
    }
  }
  GpuTimingMark(GpuTimingTransferCategory());

  if (use_dynamic_rendering) {
    VkRenderingAttachmentInfo color_attachment = {};
    VkRenderingAttachmentInfo depth_attachment = {};
    VkRenderingAttachmentInfo stencil_attachment = {};

    if (transfer_dest_is_depth) {
      depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      depth_attachment.pNext = nullptr;
      depth_attachment.imageView = transfer_dest_view;
      depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      depth_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
      depth_attachment.resolveImageView = VK_NULL_HANDLE;
      depth_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      depth_attachment.clearValue = {};
      stencil_attachment = depth_attachment;
    } else {
      color_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color_attachment.pNext = nullptr;
      color_attachment.imageView = transfer_dest_view;
      color_attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      color_attachment.resolveMode = VK_RESOLVE_MODE_NONE;
      color_attachment.resolveImageView = VK_NULL_HANDLE;
      color_attachment.resolveImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      color_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      color_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_attachment.clearValue = {};
    }

    VkRenderingInfo rendering_info = {};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.pNext = nullptr;
    rendering_info.flags = 0;
    rendering_info.renderArea.offset.x = 0;
    rendering_info.renderArea.offset.y = 0;
    rendering_info.renderArea.extent = framebuffer->host_extent;
    rendering_info.layerCount = 1;
    rendering_info.viewMask = 0;
    rendering_info.colorAttachmentCount = transfer_dest_is_depth ? 0 : 1;
    rendering_info.pColorAttachments = transfer_dest_is_depth ? nullptr : &color_attachment;
    rendering_info.pDepthAttachment = transfer_dest_is_depth ? &depth_attachment : nullptr;
    rendering_info.pStencilAttachment = transfer_dest_is_depth ? &stencil_attachment : nullptr;
    deferred_command_buffer_.CmdVkBeginRendering(&rendering_info);
  } else {
    VkRenderPassBeginInfo render_pass_begin_info;
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.pNext = nullptr;
    render_pass_begin_info.renderPass = render_pass;
    render_pass_begin_info.framebuffer = framebuffer->framebuffer;
    render_pass_begin_info.renderArea.offset.x = 0;
    render_pass_begin_info.renderArea.offset.y = 0;
    render_pass_begin_info.renderArea.extent = framebuffer->host_extent;
    render_pass_begin_info.clearValueCount = 0;
    render_pass_begin_info.pClearValues = nullptr;
    deferred_command_buffer_.CmdVkBeginRenderPass(&render_pass_begin_info,
                                                  VK_SUBPASS_CONTENTS_INLINE);
  }
  in_render_pass_ = true;
  RenderPassAreaBegin(framebuffer->host_extent);
}

void VulkanCommandProcessor::EndRenderPass() {
  assert_true(submission_open_);
  PassEndReason pass_end_reason = pass_end_reason_;
  pass_end_reason_ = kPassEndOther;
  if (!in_render_pass_) {
    return;
  }
  GpuTimingNotePassEnd(pass_end_reason);
  RenderPassAreaEnd();
  if (current_render_pass_ == VK_NULL_HANDLE) {
    deferred_command_buffer_.CmdVkEndRendering();
  } else {
    deferred_command_buffer_.CmdVkEndRenderPass();
  }
  current_render_pass_ = VK_NULL_HANDLE;
  current_framebuffer_ = nullptr;
  in_render_pass_ = false;
  // Work after a render pass that isn't categorized is "other" (mostly
  // barriers and layout transitions).
  GpuTimingMark(GpuTimingCategory::kOther);
}

void VulkanCommandProcessor::ExtendRenderPassArea(const VkRect2D& rect) {
  RenderPassArea& area = render_pass_area_;
  if (!area.active) {
    return;
  }
  int64_t x0 = std::max(int64_t(rect.offset.x), int64_t(0));
  int64_t y0 = std::max(int64_t(rect.offset.y), int64_t(0));
  int64_t x1 = std::min(int64_t(rect.offset.x) + int64_t(rect.extent.width),
                        int64_t(area.framebuffer_width));
  int64_t y1 = std::min(int64_t(rect.offset.y) + int64_t(rect.extent.height),
                        int64_t(area.framebuffer_height));
  if (x0 >= x1 || y0 >= y1) {
    return;
  }
  area.x0 = std::min(area.x0, uint32_t(x0));
  area.y0 = std::min(area.y0, uint32_t(y0));
  area.x1 = std::max(area.x1, uint32_t(x1));
  area.y1 = std::max(area.y1, uint32_t(y1));
}

void VulkanCommandProcessor::RenderPassAreaBegin(const VkExtent2D& framebuffer_extent) {
  RenderPassArea& area = render_pass_area_;
  area.active = true;
  area.restrict_enabled = REXCVAR_GET(vulkan_render_area_restrict);
  area.begin_render_area_offset = deferred_command_buffer_.GetLastRenderAreaOffset();
  area.framebuffer_width = framebuffer_extent.width;
  area.framebuffer_height = framebuffer_extent.height;
  area.x0 = UINT32_MAX;
  area.y0 = UINT32_MAX;
  area.x1 = 0;
  area.y1 = 0;
}

void VulkanCommandProcessor::RenderPassAreaEnd() {
  RenderPassArea& area = render_pass_area_;
  if (!area.active) {
    return;
  }
  area.active = false;
  // Align to whole EDRAM tiles of every attachment: a tile is 80x16 samples,
  // so 80x16 pixels is a whole number of tiles for any sample count and for
  // both 32bpp and 64bpp render targets (the framebuffer origin is the base
  // tile of every attachment). Pixels outside the render area are neither
  // loaded nor stored by the LOAD / STORE render pass, so they keep what the
  // images already contain, exactly as with the whole framebuffer as the area.
  uint32_t align_x =
      xenos::kEdramTileWidthSamples * render_target_cache_->draw_resolution_scale_x();
  uint32_t align_y =
      xenos::kEdramTileHeightSamples * render_target_cache_->draw_resolution_scale_y();
  uint32_t framebuffer_width = area.framebuffer_width;
  uint32_t framebuffer_height = area.framebuffer_height;
  VkRect2D touched;
  if (area.x0 < area.x1 && area.y0 < area.y1) {
    uint32_t x0 = area.x0 / align_x * align_x;
    uint32_t y0 = area.y0 / align_y * align_y;
    uint32_t x1 = std::min((area.x1 + (align_x - 1)) / align_x * align_x, framebuffer_width);
    uint32_t y1 = std::min((area.y1 + (align_y - 1)) / align_y * align_y, framebuffer_height);
    touched.offset.x = int32_t(x0);
    touched.offset.y = int32_t(y0);
    touched.extent.width = x1 - x0;
    touched.extent.height = y1 - y0;
  } else {
    // Nothing may have been written, but the render area must not be empty.
    touched.offset.x = 0;
    touched.offset.y = 0;
    touched.extent.width = std::min(align_x, framebuffer_width);
    touched.extent.height = std::min(align_y, framebuffer_height);
  }
  uint64_t framebuffer_pixels = uint64_t(framebuffer_width) * framebuffer_height;
  uint64_t touched_pixels = uint64_t(touched.extent.width) * touched.extent.height;
  GpuTimingCounters& c = gpu_timing_counters_;
  ++c.render_area_passes;
  c.render_area_framebuffer_width += framebuffer_width;
  c.render_area_framebuffer_height += framebuffer_height;
  c.render_area_framebuffer_pixels += framebuffer_pixels;
  c.render_area_touched_pixels += touched_pixels;
  if (area.restrict_enabled && area.begin_render_area_offset != SIZE_MAX) {
    deferred_command_buffer_.SetRecordedRenderArea(area.begin_render_area_offset, touched);
    c.render_area_restricted_passes += uint64_t(touched_pixels < framebuffer_pixels);
    c.render_area_width += touched.extent.width;
    c.render_area_height += touched.extent.height;
    c.render_area_pixels += touched_pixels;
  } else {
    c.render_area_width += framebuffer_width;
    c.render_area_height += framebuffer_height;
    c.render_area_pixels += framebuffer_pixels;
  }
}

void VulkanCommandProcessor::GpuTimingWriteTimestamp(GpuTimingCategory category) {
  // Never inside a render pass; consecutive work of one category stays in one
  // segment.
  uint64_t tag = 0;
  if (category == GpuTimingCategory::kDraws || category == GpuTimingCategory::kTransfers) {
    tag = gpu_timing_pass_tag_;
  } else if (category == GpuTimingCategory::kResolves ||
             category == GpuTimingCategory::kTextureLoads) {
    tag = gpu_timing_op_tag_;
  }
  if (in_render_pass_ || gpu_timing_current_.closed ||
      (category == gpu_timing_segment_category_ && tag == gpu_timing_segment_tag_)) {
    return;
  }
  // The last query is reserved for the end of the submission.
  if (gpu_timing_current_.timestamp_count + 1 >= kGpuTimingMaxTimestamps) {
    ++gpu_timing_overflows_;
    return;
  }
  uint32_t query = gpu_timing_current_.timestamp_count++;
  deferred_command_buffer_.CmdVkWriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                               gpu_timing_current_.pool, query);
  gpu_timing_current_.categories[query] = uint8_t(category);
  gpu_timing_current_.tags[query] = tag;
  gpu_timing_segment_category_ = category;
  gpu_timing_segment_tag_ = tag;
}

void VulkanCommandProcessor::GpuTimingBeginSubmission() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (!gpu_timing_support_) {
    const auto& ifn = vulkan_device->vulkan_instance()->functions();
    VkPhysicalDeviceProperties physical_device_properties;
    ifn.vkGetPhysicalDeviceProperties(vulkan_device->physical_device(),
                                      &physical_device_properties);
    uint32_t queue_family_count = 0;
    ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device->physical_device(),
                                                 &queue_family_count, nullptr);
    std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
    ifn.vkGetPhysicalDeviceQueueFamilyProperties(vulkan_device->physical_device(),
                                                 &queue_family_count, queue_families.data());
    uint32_t queue_family = vulkan_device->queue_family_graphics_compute();
    uint32_t valid_bits =
        queue_family < queue_family_count ? queue_families[queue_family].timestampValidBits : 0;
    float period = physical_device_properties.limits.timestampPeriod;
    if (!valid_bits || !(period > 0.0f)) {
      gpu_timing_support_ = -1;
      REXGPU_INFO(
          "GPU timing: timestamps unsupported on the graphics queue (timestampValidBits {}, "
          "timestampPeriod {}), logging counters only",
          valid_bits, period);
    } else {
      gpu_timing_support_ = 1;
      gpu_timing_period_ns_ = double(period);
      gpu_timing_valid_mask_ = valid_bits >= 64 ? UINT64_MAX : (UINT64_C(1) << valid_bits) - 1;
      REXGPU_INFO("GPU timing: timestamps on (timestampValidBits {}, timestampPeriod {} ns)",
                  valid_bits, period);
    }
  }
  if (gpu_timing_support_ < 0) {
    return;
  }
  GpuTimingSubmission submission;
  if (!gpu_timing_free_.empty()) {
    submission = std::move(gpu_timing_free_.back());
    gpu_timing_free_.pop_back();
  } else {
    VkQueryPoolCreateInfo query_pool_create_info = {};
    query_pool_create_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    query_pool_create_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    query_pool_create_info.queryCount = kGpuTimingMaxTimestamps;
    if (dfn.vkCreateQueryPool(device, &query_pool_create_info, nullptr, &submission.pool) !=
        VK_SUCCESS) {
      REXGPU_ERROR("GPU timing: failed to create a timestamp query pool, logging counters only");
      gpu_timing_support_ = -1;
      return;
    }
    submission.categories.resize(kGpuTimingMaxTimestamps);
    submission.tags.resize(kGpuTimingMaxTimestamps);
  }
  submission.timestamp_count = 0;
  submission.closed = false;
  submission.frame = gpu_timing_frame_index_;
  gpu_timing_current_ = std::move(submission);
  deferred_command_buffer_.CmdVkResetQueryPool(gpu_timing_current_.pool, 0,
                                               kGpuTimingMaxTimestamps);
  gpu_timing_segment_category_ = GpuTimingCategory::kCount;
  GpuTimingWriteTimestamp(GpuTimingCategory::kOther);
}

void VulkanCommandProcessor::GpuTimingEndSubmission() {
  if (gpu_timing_current_.closed) {
    return;
  }
  // Closes the last segment, using the reserved query.
  uint32_t query = gpu_timing_current_.timestamp_count++;
  deferred_command_buffer_.CmdVkWriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                               gpu_timing_current_.pool, query);
  gpu_timing_current_.categories[query] = uint8_t(GpuTimingCategory::kCount);
  gpu_timing_current_.closed = true;
}

void VulkanCommandProcessor::GpuTimingReadCompleted() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  while (!gpu_timing_submitted_.empty()) {
    auto& submission_pair = gpu_timing_submitted_.front();
    if (submission_pair.first > submission_completed_) {
      break;
    }
    GpuTimingSubmission& submission = submission_pair.second;
    uint32_t count = submission.timestamp_count;
    if (submission.frame != gpu_frame_accum_frame_) {
      GpuTimingFlushFrame();
      gpu_frame_accum_frame_ = submission.frame;
    }
    if (count >= 2) {
      gpu_timing_results_.resize(count);
      // The submission fence is signaled, so this doesn't wait.
      if (dfn.vkGetQueryPoolResults(device, submission.pool, 0, count, sizeof(uint64_t) * count,
                                    gpu_timing_results_.data(), sizeof(uint64_t),
                                    VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        for (uint32_t i = 0; i + 1 < count; ++i) {
          uint8_t category = submission.categories[i];
          if (category >= uint8_t(GpuTimingCategory::kCount)) {
            continue;
          }
          uint64_t ticks =
              (gpu_timing_results_[i + 1] - gpu_timing_results_[i]) & gpu_timing_valid_mask_;
          gpu_timing_category_ns_[category] += double(ticks) * gpu_timing_period_ns_;
          gpu_frame_accum_ns_[category] += double(ticks) * gpu_timing_period_ns_;
          if (category == uint8_t(GpuTimingCategory::kDraws) ||
              category == uint8_t(GpuTimingCategory::kTransfers)) {
            gpu_timing_tag_stats_[submission.tags[i]].ns += double(ticks) * gpu_timing_period_ns_;
          } else if (category == uint8_t(GpuTimingCategory::kResolves) ||
                     category == uint8_t(GpuTimingCategory::kTextureLoads)) {
            gpu_timing_op_stats_[submission.tags[i]].ns += double(ticks) * gpu_timing_period_ns_;
          }
        }
        ++gpu_timing_submissions_measured_;
        for (const auto& draw_query : submission.draw_queries) {
          if (draw_query.first == 0 || draw_query.first >= count) {
            continue;
          }
          uint64_t ticks = (gpu_timing_results_[draw_query.first] -
                            gpu_timing_results_[draw_query.first - 1]) &
                           gpu_timing_valid_mask_;
          gpu_trace_draw_ns_.emplace_back(draw_query.second,
                                          double(ticks) * gpu_timing_period_ns_);
        }
      }
    }
    submission.draw_queries.clear();
    gpu_timing_free_.push_back(std::move(submission));
    gpu_timing_submitted_.pop_front();
  }
}

void VulkanCommandProcessor::GpuTimingFlushFrame() {
  double total = 0.0;
  for (double ns : gpu_frame_accum_ns_) total += ns;
  gpu_last_frame_index_ = gpu_frame_accum_frame_;
  gpu_last_frame_ms_ = total * 1e-6;
  const uint32_t threshold_ms = REXCVAR_GET(vulkan_slow_frame_ms);
  if (threshold_ms && total > double(threshold_ms) * 1e6) {
    auto ms = [&](GpuTimingCategory c) { return gpu_frame_accum_ns_[size_t(c)] * 1e-6; };
    REXGPU_INFO("GPU slow frame on the GPU: frame {}: {:.1f} ms = draws {:.1f} + transfers {:.1f} + "
                "resolves {:.1f} + texture loads {:.1f} + uploads {:.1f} + swap {:.1f} + other {:.1f}",
                gpu_frame_accum_frame_, total * 1e-6, ms(GpuTimingCategory::kDraws),
                ms(GpuTimingCategory::kTransfers), ms(GpuTimingCategory::kResolves),
                ms(GpuTimingCategory::kTextureLoads), ms(GpuTimingCategory::kUploads),
                ms(GpuTimingCategory::kSwap), ms(GpuTimingCategory::kOther));
  }
  for (double& ns : gpu_frame_accum_ns_) ns = 0.0;
}

void VulkanCommandProcessor::GpuTimingOnSwap() {
  ++gpu_timing_frame_index_;
  ui::vulkan::VulkanPresenter* presenter =
      graphics_system_
          ? static_cast<ui::vulkan::VulkanPresenter*>(graphics_system_->presenter())
          : nullptr;
  uint64_t now_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count());
  uint64_t paint_ns = 0, paint_count = 0;
  if (gpu_trace_active_) {
    gpu_trace_active_ = false;
    REXGPU_INFO("GPU frame trace ({} chars; P = pass begin, )N + end reason f/R/a/g/t/b/F/o = "
                "framebuffer change / barrier before read (R) / attachment / general / transfer / "
                "buffer / forced / other, R = resolve, T = texture load, U = upload):",
                gpu_trace_.size());
    for (size_t offset = 0; offset < gpu_trace_.size(); offset += 1500) {
      REXGPU_INFO("GPU frame trace: {}", std::string_view(gpu_trace_).substr(offset, 1500));
    }
    gpu_trace_.clear();
    if (gpu_trace_frames_left_) {
      --gpu_trace_frames_left_;
      gpu_trace_armed_ = true;
    }
  }
  if (gpu_trace_armed_) {
    gpu_trace_armed_ = false;
    gpu_trace_active_ = true;
    gpu_trace_pass_draws_ = 0;
    gpu_trace_.clear();
    gpu_trace_draws_.clear();
    gpu_trace_draw_ns_.clear();
  }
  if (!gpu_timing_window_start_ns_) {
    // (Re)start the window from this swap.
    gpu_timing_window_start_ns_ = now_ns;
    std::fill(std::begin(gpu_timing_category_ns_), std::end(gpu_timing_category_ns_), 0.0);
    gpu_timing_submissions_measured_ = 0;
    gpu_timing_overflows_ = 0;
    gpu_timing_frames_ = 0;
    gpu_timing_counters_ = GpuTimingCounters();
    gpu_timing_tag_stats_.clear();
    gpu_timing_op_stats_.clear();
    gpu_timing_transfer_pairs_.clear();
    if (presenter) {
      presenter->SetPaintGpuTimingEnabled(true);
      presenter->TakePaintGpuTiming(paint_ns, paint_count);
    }
    return;
  }
  ++gpu_timing_frames_;
#if REX_PLATFORM_ANDROID || REX_PLATFORM_LINUX
  {
    static FILE* gpu_clock_file = std::fopen("/sys/class/kgsl/kgsl-3d0/gpuclk", "r");
    if (gpu_clock_file) {
      unsigned long long hz = 0;
      std::rewind(gpu_clock_file);
      std::fflush(gpu_clock_file);
      if (std::fscanf(gpu_clock_file, "%llu", &hz) == 1 && hz) {
        gpu_timing_clock_mhz_sum_ += double(hz) * 1.0e-6;
        ++gpu_timing_clock_samples_;
      }
    }
  }
#endif
  uint64_t elapsed_ns = now_ns - gpu_timing_window_start_ns_;
  if (elapsed_ns < UINT64_C(10000000000)) {
    return;
  }
  if (presenter) {
    presenter->TakePaintGpuTiming(paint_ns, paint_count);
  }
  double frames = double(gpu_timing_frames_);
  const GpuTimingCounters& c = gpu_timing_counters_;
  // Render target cache render pass instance render areas
  // (vulkan_render_area_restrict).
  std::string render_area_info;
  {
    double area_passes = double(std::max(c.render_area_passes, uint64_t(1)));
    double framebuffer_pixels = double(std::max(c.render_area_framebuffer_pixels, uint64_t(1)));
    render_area_info = fmt::format(
        "; render area per pass {:.0f}x{:.0f} of {:.0f}x{:.0f} avg = {:.1f}% of framebuffer "
        "pixels (writes need {:.1f}%), {:.1f} of {:.1f} passes restricted (restrict {})",
        double(c.render_area_width) / area_passes, double(c.render_area_height) / area_passes,
        double(c.render_area_framebuffer_width) / area_passes,
        double(c.render_area_framebuffer_height) / area_passes,
        double(c.render_area_pixels) / framebuffer_pixels * 100.0,
        double(c.render_area_touched_pixels) / framebuffer_pixels * 100.0,
        double(c.render_area_restricted_passes) / frames, double(c.render_area_passes) / frames,
        REXCVAR_GET(vulkan_render_area_restrict) ? "on" : "off");
  }
  if (gpu_timing_support_ > 0) {
    const double* ns = gpu_timing_category_ns_;
    double total_ns = 0.0;
    for (double category_ns : gpu_timing_category_ns_) {
      total_ns += category_ns;
    }
    auto ms = [frames](double value_ns) { return value_ns / frames * 1.0e-6; };
    REXGPU_INFO(
        "GPU timing ({:.1f} s, {} frames, per frame): total {:.2f} ms ({:.0f}% of wall time) = "
        "draws {:.2f} + transfers {:.2f} + resolves {:.2f} + texture loads {:.2f} + uploads "
        "{:.2f} + swap {:.2f} + other {:.2f}; presenter paint {:.2f} ms x {} paints; per frame "
        "{:.1f} render passes ({:.1f} with load), {:.1f} transfers, {:.1f} resolves, {:.1f} "
        "texture loads, {:.1f} uploads, {:.1f} draws, {:.2f} submissions ({} measured, {} "
        "dropped marks){}",
        double(elapsed_ns) * 1.0e-9, gpu_timing_frames_, ms(total_ns),
        total_ns / double(elapsed_ns) * 100.0, ms(ns[size_t(GpuTimingCategory::kDraws)]),
        ms(ns[size_t(GpuTimingCategory::kTransfers)]),
        ms(ns[size_t(GpuTimingCategory::kResolves)]),
        ms(ns[size_t(GpuTimingCategory::kTextureLoads)]),
        ms(ns[size_t(GpuTimingCategory::kUploads)]), ms(ns[size_t(GpuTimingCategory::kSwap)]),
        ms(ns[size_t(GpuTimingCategory::kOther)]),
        paint_count ? double(paint_ns) / double(paint_count) * 1.0e-6 : 0.0, paint_count,
        double(c.render_passes) / frames, double(c.render_passes_with_load) / frames,
        double(c.transfers) / frames, double(c.resolves) / frames,
        double(c.texture_loads) / frames, double(c.uploads) / frames, double(c.draws) / frames,
        double(c.submissions) / frames, gpu_timing_submissions_measured_, gpu_timing_overflows_,
        render_area_info);
    if (gpu_timing_clock_samples_) {
      double clock_mhz = gpu_timing_clock_mhz_sum_ / double(gpu_timing_clock_samples_);
      REXGPU_INFO("GPU work: average clock {:.0f} MHz, {:.2f} Mcycles per frame (total time x "
                  "clock)",
                  clock_mhz, ms(total_ns) * clock_mhz * 1.0e-3);
    }
    if (no_effect_draws_skipped_) {
      REXGPU_INFO("GPU draws skipped as writing nothing: {:.1f} per frame",
                  double(no_effect_draws_skipped_) / frames);
      no_effect_draws_skipped_ = 0;
    }
    GpuTimingLogPasses(frames);
    if (render_target_cache_) {
      render_target_cache_->LogAndResetLazyDumpStats(frames);
    }
    if (texture_cache_) {
      texture_cache_->LogMemoryStats();
    }
    GpuTraceLogDraws();
    gpu_trace_armed_ = REXCVAR_GET(vulkan_gpu_trace_frame);
    // vulkan_gpu_trace_frames: trace that many consecutive frames once, from
    // the 4th 10-second window (the title menu is up by then).
    if (REXCVAR_GET(vulkan_gpu_trace_frames) &&
        ++gpu_trace_windows_ == REXCVAR_GET(vulkan_gpu_trace_frames_window)) {
      gpu_trace_frames_left_ = REXCVAR_GET(vulkan_gpu_trace_frames) - 1;
      gpu_trace_armed_ = true;
    }
  } else {
    REXGPU_INFO(
        "GPU timing ({:.1f} s, {} frames, no timestamps): per frame {:.1f} render passes ({:.1f} "
        "with load), {:.1f} transfers, {:.1f} resolves, {:.1f} texture loads, {:.1f} uploads, "
        "{:.1f} draws, {:.2f} submissions{}",
        double(elapsed_ns) * 1.0e-9, gpu_timing_frames_, double(c.render_passes) / frames,
        double(c.render_passes_with_load) / frames, double(c.transfers) / frames,
        double(c.resolves) / frames, double(c.texture_loads) / frames,
        double(c.uploads) / frames, double(c.draws) / frames, double(c.submissions) / frames,
        render_area_info);
  }
  gpu_timing_window_start_ns_ = now_ns;
  std::fill(std::begin(gpu_timing_category_ns_), std::end(gpu_timing_category_ns_), 0.0);
  gpu_timing_submissions_measured_ = 0;
  gpu_timing_overflows_ = 0;
  gpu_timing_frames_ = 0;
  gpu_timing_counters_ = GpuTimingCounters();
  gpu_timing_tag_stats_.clear();
  gpu_timing_op_stats_.clear();
  gpu_timing_transfer_pairs_.clear();
  gpu_timing_clock_mhz_sum_ = 0.0;
  gpu_timing_clock_samples_ = 0;
}

std::string VulkanCommandProcessor::GpuTimingDescribeTag(uint64_t tag) {
  VulkanRenderTargetCache::RenderPassKey key;
  key.key = uint32_t(tag);
  std::string attachments;
  if (key.depth_and_color_used & 1) {
    attachments += fmt::format("D{}", uint32_t(key.depth_format));
  }
  xenos::ColorRenderTargetFormat color_formats[] = {key.color_0_view_format,
                                                    key.color_1_view_format,
                                                    key.color_2_view_format,
                                                    key.color_3_view_format};
  for (uint32_t j = 0; j < 4; ++j) {
    if (key.depth_and_color_used & (uint32_t(2) << j)) {
      attachments += fmt::format("{}C{}f{}", attachments.empty() ? "" : "+", j,
                                 uint32_t(color_formats[j]));
    }
  }
  return fmt::format("{}{} {}x p{}", (tag >> 63) ? "xfer " : ((tag >> 62) & 1) ? "clear " : "",
                     attachments.empty() ? "none" : attachments,
                     uint32_t(1) << uint32_t(key.msaa_samples), uint32_t(tag >> 32) & 0xFF);
}

void VulkanCommandProcessor::GpuTimingLogPasses(double frames) {
  const GpuTimingCounters& c = gpu_timing_counters_;
  const uint64_t* r = c.pass_end_reasons;
  auto per_frame = [frames](uint64_t value) { return double(value) / frames; };
  std::vector<std::pair<uint64_t, GpuTimingTagStats>> tags(gpu_timing_tag_stats_.begin(),
                                                           gpu_timing_tag_stats_.end());
  std::sort(tags.begin(), tags.end(),
            [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
  std::string tags_info;
  for (size_t i = 0; i < tags.size() && i < 14; ++i) {
    tags_info += fmt::format(" | {}: {:.2f} ms {:.1f} passes {:.1f} draws",
                             GpuTimingDescribeTag(tags[i].first),
                             tags[i].second.ns / frames * 1.0e-6, per_frame(tags[i].second.passes),
                             per_frame(tags[i].second.draws));
  }
  REXGPU_INFO(
      "GPU passes per frame: ends by framebuffer change {:.1f}, barrier before texture read "
      "{:.1f} / attachment use {:.1f} / general {:.1f} / transfer {:.1f} / buffer {:.1f}, forced "
      "{:.1f}, other {:.1f}; by signature (D/C = depth/color attachment, f = guest format, Nx = "
      "MSAA, p = pitch tiles at 32bpp){}",
      per_frame(r[kPassEndFramebuffer]), per_frame(r[kPassEndBarrierRead]),
      per_frame(r[kPassEndBarrierAttachment]), per_frame(r[kPassEndBarrierGeneral]),
      per_frame(r[kPassEndBarrierTransfer]), per_frame(r[kPassEndBarrierBuffer]),
      per_frame(r[kPassEndForced]), per_frame(r[kPassEndOther]), tags_info);
  std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> pairs(
      gpu_timing_transfer_pairs_.begin(), gpu_timing_transfer_pairs_.end());
  std::sort(pairs.begin(), pairs.end(),
            [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
  auto describe = [](uint32_t d) {
    return fmt::format("{}{} {}x p{}", (d >> 15) & 1 ? "D" : "C", (d >> 8) & 0x1F,
                       uint32_t(1) << ((d >> 13) & 3), d & 0xFF);
  };
  std::string pairs_info;
  for (size_t i = 0; i < pairs.size() && i < 12; ++i) {
    uint64_t pair = pairs[i].first;
    pairs_info += fmt::format(" | {} -> {}{}: {:.1f} x {:.0f} tiles",
                              describe(uint32_t(pair >> 16) & 0xFFFF),
                              describe(uint32_t(pair) & 0xFFFF), (pair >> 32) & 1 ? " +hostdepth" : "",
                              per_frame(pairs[i].second.first),
                              double(pairs[i].second.second) /
                                  double(std::max(pairs[i].second.first, uint64_t(1))));
  }
  REXGPU_INFO("GPU transfers per frame (source -> destination, count x average tiles):{}",
              pairs_info);
  std::vector<std::pair<uint64_t, GpuTimingTagStats>> ops(gpu_timing_op_stats_.begin(),
                                                          gpu_timing_op_stats_.end());
  std::sort(ops.begin(), ops.end(),
            [](const auto& a, const auto& b) { return a.second.ns > b.second.ns; });
  std::string ops_info;
  for (size_t i = 0; i < ops.size() && i < 16; ++i) {
    uint64_t tag = ops[i].first;
    if (!tag) {
      continue;
    }
    ops_info += fmt::format(" | {}{} {}x{} f{}: {:.2f} ms / {:.1f}", char(tag >> 56),
                            (tag >> 55) & 1 ? "d" : "", uint32_t(tag >> 24) & 0xFFFFFF,
                            uint32_t(tag) & 0xFFFFFF, uint32_t(tag >> 48) & 0x7F,
                            ops[i].second.ns / frames * 1.0e-6, per_frame(ops[i].second.passes));
  }
  REXGPU_INFO("GPU resolves (R, d = depth) and texture loads (T) per frame by size and format, "
              "ms / count:{}",
              ops_info);
}

void VulkanCommandProcessor::GpuTraceDrawTimestamp(uint64_t vertex_shader_hash,
                                                   uint64_t pixel_shader_hash,
                                                   uint32_t primitive_type,
                                                   uint32_t index_count) {
  // Inside the render pass (fine for timestamps; the Qualcomm driver renders
  // these passes directly to memory). Keeps the segment's category, so the
  // per-category totals are unchanged.
  if (gpu_timing_current_.pool == VK_NULL_HANDLE || gpu_timing_current_.closed ||
      gpu_timing_current_.timestamp_count + 1 >= kGpuTimingMaxTimestamps) {
    return;
  }
  uint32_t query = gpu_timing_current_.timestamp_count++;
  deferred_command_buffer_.CmdVkWriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                               gpu_timing_current_.pool, query);
  gpu_timing_current_.categories[query] = uint8_t(gpu_timing_segment_category_);
  gpu_timing_current_.tags[query] = gpu_timing_segment_tag_;
  gpu_timing_current_.draw_queries.emplace_back(query, uint32_t(gpu_trace_draws_.size()));
  GpuTraceDraw& draw = gpu_trace_draws_.emplace_back();
  draw.vertex_shader_hash = vertex_shader_hash;
  draw.pixel_shader_hash = pixel_shader_hash;
  draw.pass_tag = gpu_timing_pass_tag_;
  draw.primitive_type = primitive_type;
  draw.index_count = index_count;
}

void VulkanCommandProcessor::GpuTraceLogDraws() {
  if (gpu_trace_draw_ns_.empty()) {
    return;
  }
  double total_ns = 0.0;
  for (const auto& draw_ns : gpu_trace_draw_ns_) {
    total_ns += draw_ns.second;
  }
  std::vector<std::pair<uint32_t, double>> sorted = gpu_trace_draw_ns_;
  std::sort(sorted.begin(), sorted.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  // Time per pixel shader over the frame.
  std::unordered_map<uint64_t, std::pair<double, uint32_t>> by_pixel_shader;
  for (const auto& draw_ns : gpu_trace_draw_ns_) {
    if (draw_ns.first < gpu_trace_draws_.size()) {
      auto& entry = by_pixel_shader[gpu_trace_draws_[draw_ns.first].pixel_shader_hash];
      entry.first += draw_ns.second;
      ++entry.second;
    }
  }
  std::vector<std::pair<uint64_t, std::pair<double, uint32_t>>> shaders(by_pixel_shader.begin(),
                                                                        by_pixel_shader.end());
  std::sort(shaders.begin(), shaders.end(),
            [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
  std::string draws_info;
  for (size_t i = 0; i < sorted.size() && i < 20; ++i) {
    if (sorted[i].first >= gpu_trace_draws_.size()) {
      continue;
    }
    const GpuTraceDraw& draw = gpu_trace_draws_[sorted[i].first];
    draws_info += fmt::format(" | #{} {:.3f} ms VS {:016X} PS {:016X} prim {} n {} in {}",
                              sorted[i].first, sorted[i].second * 1.0e-6,
                              draw.vertex_shader_hash, draw.pixel_shader_hash,
                              draw.primitive_type, draw.index_count,
                              GpuTimingDescribeTag(draw.pass_tag));
  }
  std::string shaders_info;
  for (size_t i = 0; i < shaders.size() && i < 12; ++i) {
    shaders_info += fmt::format(" | PS {:016X}: {:.3f} ms in {} draws", shaders[i].first,
                                shaders[i].second.first * 1.0e-6, shaders[i].second.second);
  }
  REXGPU_INFO("GPU traced frame draws: {} draws timed, {:.2f} ms to the ends of draws; slowest "
              "(GPU time since the previous timestamp):{}",
              gpu_trace_draw_ns_.size(), total_ns * 1.0e-6, draws_info);
  REXGPU_INFO("GPU traced frame by pixel shader:{}", shaders_info);
  if (REXCVAR_GET(vulkan_gpu_trace_all_pixel_shaders)) {
    // Every pixel shader of the traced frame in draw order (cheap effects never make the top lists):
    // draws, GPU time, primitive types (bit per type), indices, the first draw's vertex shader and pass.
    struct AllEntry {
      uint64_t pixel_shader_hash, vertex_shader_hash, pass_tag;
      uint32_t draws, primitive_types, indices;
      double ns;
    };
    std::vector<AllEntry> all;
    std::unordered_map<uint64_t, size_t> all_index;
    std::vector<double> draw_ns(gpu_trace_draws_.size(), 0.0);
    for (const auto& d : gpu_trace_draw_ns_) {
      if (d.first < draw_ns.size()) draw_ns[d.first] = d.second;
    }
    for (size_t i = 0; i < gpu_trace_draws_.size(); ++i) {
      const GpuTraceDraw& draw = gpu_trace_draws_[i];
      auto [it, inserted] = all_index.emplace(draw.pixel_shader_hash, all.size());
      if (inserted) {
        all.push_back({draw.pixel_shader_hash, draw.vertex_shader_hash, draw.pass_tag, 0, 0, 0, 0.0});
      }
      AllEntry& e = all[it->second];
      ++e.draws;
      e.primitive_types |= 1u << (draw.primitive_type & 31);
      e.indices += draw.index_count;
      e.ns += draw_ns[i];
    }
    std::string line;
    for (size_t i = 0; i < all.size(); ++i) {
      line += fmt::format(" | PS {:016X} VS {:016X} {}x prim {:X} n {} {:.3f} ms in {}",
                          all[i].pixel_shader_hash, all[i].vertex_shader_hash, all[i].draws,
                          all[i].primitive_types, all[i].indices, all[i].ns * 1.0e-6,
                          GpuTimingDescribeTag(all[i].pass_tag));
      if (line.size() > 1400 || i + 1 == all.size()) {
        REXGPU_INFO("GPU traced frame, all pixel shaders ({} total):{}", all.size(), line);
        line.clear();
      }
    }
  }
  gpu_trace_draws_.clear();
  gpu_trace_draw_ns_.clear();
}

void VulkanCommandProcessor::GpuTimingShutdown() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (gpu_timing_current_.pool != VK_NULL_HANDLE) {
    dfn.vkDestroyQueryPool(device, gpu_timing_current_.pool, nullptr);
  }
  gpu_timing_current_ = GpuTimingSubmission();
  for (const auto& submission_pair : gpu_timing_submitted_) {
    dfn.vkDestroyQueryPool(device, submission_pair.second.pool, nullptr);
  }
  gpu_timing_submitted_.clear();
  for (const GpuTimingSubmission& submission : gpu_timing_free_) {
    dfn.vkDestroyQueryPool(device, submission.pool, nullptr);
  }
  gpu_timing_free_.clear();
}

VkDescriptorSet VulkanCommandProcessor::AllocateSingleTransientDescriptor(
    SingleTransientDescriptorLayout transient_descriptor_layout) {
  assert_true(frame_open_);
  VkDescriptorSet descriptor_set;
  std::vector<VkDescriptorSet>& transient_descriptors_free =
      single_transient_descriptors_free_[size_t(transient_descriptor_layout)];
  if (!transient_descriptors_free.empty()) {
    descriptor_set = transient_descriptors_free.back();
    transient_descriptors_free.pop_back();
  } else {
    const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
    [[maybe_unused]] const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    [[maybe_unused]] const VkDevice device = vulkan_device->device();
    bool is_storage_buffer =
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageBufferCompute ||
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageBufferPairCompute;
    ui::vulkan::LinkedTypeDescriptorSetAllocator& transient_descriptor_allocator =
        is_storage_buffer ? transient_descriptor_allocator_storage_buffer_
                          : transient_descriptor_allocator_uniform_buffer_;
    VkDescriptorPoolSize descriptor_count;
    descriptor_count.type =
        is_storage_buffer ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptor_count.descriptorCount =
        transient_descriptor_layout == SingleTransientDescriptorLayout::kStorageBufferPairCompute
            ? 2
            : 1;
    descriptor_set = transient_descriptor_allocator.Allocate(
        GetSingleTransientDescriptorLayout(transient_descriptor_layout), &descriptor_count, 1);
    if (descriptor_set == VK_NULL_HANDLE) {
      return VK_NULL_HANDLE;
    }
  }
  UsedSingleTransientDescriptor used_descriptor;
  used_descriptor.frame = frame_current_;
  used_descriptor.layout = transient_descriptor_layout;
  used_descriptor.set = descriptor_set;
  single_transient_descriptors_used_.emplace_back(used_descriptor);
  return descriptor_set;
}

VkDescriptorSetLayout VulkanCommandProcessor::GetTextureDescriptorSetLayout(bool is_vertex,
                                                                            size_t texture_count,
                                                                            size_t sampler_count) {
  std::lock_guard<std::recursive_mutex> layouts_lock(pipeline_layouts_mutex_);
  size_t binding_count = texture_count + sampler_count;
  if (!binding_count) {
    return descriptor_set_layout_empty_;
  }

  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = uint32_t(texture_count);
  texture_descriptor_set_layout_key.sampler_count = uint32_t(sampler_count);
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  auto it_existing = descriptor_set_layouts_textures_.find(texture_descriptor_set_layout_key);
  if (it_existing != descriptor_set_layouts_textures_.end()) {
    return it_existing->second;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  descriptor_set_layout_bindings_.clear();
  descriptor_set_layout_bindings_.reserve(binding_count);
  VkShaderStageFlags stage_flags =
      is_vertex ? guest_shader_vertex_stages_ : VK_SHADER_STAGE_FRAGMENT_BIT;
  for (size_t i = 0; i < texture_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(i);
    descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  for (size_t i = 0; i < sampler_count; ++i) {
    VkDescriptorSetLayoutBinding& descriptor_set_layout_binding =
        descriptor_set_layout_bindings_.emplace_back();
    descriptor_set_layout_binding.binding = uint32_t(texture_count + i);
    descriptor_set_layout_binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_layout_binding.descriptorCount = 1;
    descriptor_set_layout_binding.stageFlags = stage_flags;
  }
  VkDescriptorSetLayoutCreateInfo descriptor_set_layout_create_info;
  descriptor_set_layout_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  descriptor_set_layout_create_info.pNext = nullptr;
  descriptor_set_layout_create_info.flags = 0;
  descriptor_set_layout_create_info.bindingCount = uint32_t(binding_count);
  descriptor_set_layout_create_info.pBindings = descriptor_set_layout_bindings_.data();
  VkDescriptorSetLayout texture_descriptor_set_layout;
  if (dfn.vkCreateDescriptorSetLayout(device, &descriptor_set_layout_create_info, nullptr,
                                      &texture_descriptor_set_layout) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  descriptor_set_layouts_textures_.emplace(texture_descriptor_set_layout_key,
                                           texture_descriptor_set_layout);
  return texture_descriptor_set_layout;
}

const VulkanPipelineCache::PipelineLayoutProvider* VulkanCommandProcessor::GetPipelineLayout(
    size_t texture_count_pixel, size_t sampler_count_pixel, size_t texture_count_vertex,
    size_t sampler_count_vertex) {
  std::lock_guard<std::recursive_mutex> layouts_lock(pipeline_layouts_mutex_);
  PipelineLayoutKey pipeline_layout_key;
  pipeline_layout_key.texture_count_pixel = uint16_t(texture_count_pixel);
  pipeline_layout_key.sampler_count_pixel = uint16_t(sampler_count_pixel);
  pipeline_layout_key.texture_count_vertex = uint16_t(texture_count_vertex);
  pipeline_layout_key.sampler_count_vertex = uint16_t(sampler_count_vertex);
  {
    auto it = pipeline_layouts_.find(pipeline_layout_key);
    if (it != pipeline_layouts_.end()) {
      return &it->second;
    }
  }

  VkDescriptorSetLayout descriptor_set_layout_textures_vertex =
      GetTextureDescriptorSetLayout(true, texture_count_vertex, sampler_count_vertex);
  if (descriptor_set_layout_textures_vertex == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest vertex shaders",
        texture_count_vertex, sampler_count_vertex);
    return nullptr;
  }
  VkDescriptorSetLayout descriptor_set_layout_textures_pixel =
      GetTextureDescriptorSetLayout(false, texture_count_pixel, sampler_count_pixel);
  if (descriptor_set_layout_textures_pixel == VK_NULL_HANDLE) {
    REXGPU_ERROR(
        "Failed to obtain a Vulkan descriptor set layout for {} sampled images "
        "and {} samplers for guest pixel shaders",
        texture_count_pixel, sampler_count_pixel);
    return nullptr;
  }

  VkDescriptorSetLayout descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetCount];
  // Immutable layouts.
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
      descriptor_set_layout_shared_memory_and_edram_;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetConstants] =
      descriptor_set_layout_constants_;
  // Mutable layouts.
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
      descriptor_set_layout_textures_vertex;
  descriptor_set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
      descriptor_set_layout_textures_pixel;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  const bool library_layouts = pipeline_cache_->UsesPipelineLibraries();
  VkPipelineLayoutCreateInfo pipeline_layout_create_info;
  pipeline_layout_create_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipeline_layout_create_info.pNext = nullptr;
  pipeline_layout_create_info.flags =
      library_layouts ? VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT : 0;
  pipeline_layout_create_info.setLayoutCount = uint32_t(rex::countof(descriptor_set_layouts));
  pipeline_layout_create_info.pSetLayouts = descriptor_set_layouts;
  pipeline_layout_create_info.pushConstantRangeCount = 0;
  pipeline_layout_create_info.pPushConstantRanges = nullptr;
  VkPipelineLayout pipeline_layout;
  if (dfn.vkCreatePipelineLayout(device, &pipeline_layout_create_info, nullptr, &pipeline_layout) !=
      VK_SUCCESS) {
    REXGPU_ERROR(
        "Failed to create a Vulkan pipeline layout for guest drawing with {} "
        "pixel shader and {} vertex shader textures",
        texture_count_pixel, texture_count_vertex);
    return nullptr;
  }
  // Pipeline libraries: the pre-rasterization library only knows the vertex shader's texture set,
  // the fragment library only the pixel shader's (the other set is a hole).
  VkPipelineLayout pre_rasterization_library_layout = VK_NULL_HANDLE;
  VkPipelineLayout fragment_library_layout = VK_NULL_HANDLE;
  if (library_layouts) {
    pre_rasterization_library_layout =
        GetLibraryLayout(true, descriptor_set_layout_textures_vertex);
    fragment_library_layout = GetLibraryLayout(false, descriptor_set_layout_textures_pixel);
  }
  auto emplaced_pair = pipeline_layouts_.emplace(
      std::piecewise_construct, std::forward_as_tuple(pipeline_layout_key),
      std::forward_as_tuple(pipeline_layout, descriptor_set_layout_textures_vertex,
                            descriptor_set_layout_textures_pixel, pre_rasterization_library_layout,
                            fragment_library_layout));
  // unordered_map insertion doesn't invalidate element references.
  return &emplaced_pair.first->second;
}

VulkanCommandProcessor::ScratchBufferAcquisition VulkanCommandProcessor::AcquireScratchGpuBuffer(
    VkDeviceSize size, VkPipelineStageFlags initial_stage_mask, VkAccessFlags initial_access_mask) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || !size) {
    return ScratchBufferAcquisition();
  }

  uint64_t submission_current = GetCurrentSubmission();

  if (scratch_buffer_ != VK_NULL_HANDLE && size <= scratch_buffer_size_) {
    // Already used previously - transition.
    PushBufferMemoryBarrier(scratch_buffer_, 0, VK_WHOLE_SIZE, scratch_buffer_last_stage_mask_,
                            initial_stage_mask, scratch_buffer_last_access_mask_,
                            initial_access_mask);
    scratch_buffer_last_stage_mask_ = initial_stage_mask;
    scratch_buffer_last_access_mask_ = initial_access_mask;
    scratch_buffer_last_usage_submission_ = submission_current;
    scratch_buffer_used_ = true;
    return ScratchBufferAcquisition(*this, scratch_buffer_, initial_stage_mask,
                                    initial_access_mask);
  }

  size = rex::align(size, kScratchBufferSizeIncrement);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();

  VkDeviceMemory new_scratch_buffer_memory;
  VkBuffer new_scratch_buffer;
  // VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT for
  // texture loading.
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, size,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
          ui::vulkan::util::MemoryPurpose::kDeviceLocal, new_scratch_buffer,
          new_scratch_buffer_memory)) {
    REXGPU_ERROR("VulkanCommandProcessor: Failed to create a {} MB scratch GPU buffer", size >> 20);
    return ScratchBufferAcquisition();
  }

  if (submission_completed_ >= scratch_buffer_last_usage_submission_) {
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      dfn.vkDestroyBuffer(device, scratch_buffer_, nullptr);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      dfn.vkFreeMemory(device, scratch_buffer_memory_, nullptr);
    }
  } else {
    if (scratch_buffer_ != VK_NULL_HANDLE) {
      destroy_buffers_.emplace_back(scratch_buffer_last_usage_submission_, scratch_buffer_);
    }
    if (scratch_buffer_memory_ != VK_NULL_HANDLE) {
      destroy_memory_.emplace_back(scratch_buffer_last_usage_submission_, scratch_buffer_memory_);
    }
  }

  scratch_buffer_memory_ = new_scratch_buffer_memory;
  scratch_buffer_ = new_scratch_buffer;
  scratch_buffer_size_ = size;
  // Not used yet, no need for a barrier.
  scratch_buffer_last_stage_mask_ = initial_access_mask;
  scratch_buffer_last_access_mask_ = initial_stage_mask;
  scratch_buffer_last_usage_submission_ = submission_current;
  scratch_buffer_used_ = true;
  return ScratchBufferAcquisition(*this, new_scratch_buffer, initial_stage_mask,
                                  initial_access_mask);
}

void VulkanCommandProcessor::BindExternalGraphicsPipeline(VkPipeline pipeline,
                                                          bool keep_dynamic_depth_bias,
                                                          bool keep_dynamic_blend_constants,
                                                          bool keep_dynamic_stencil_mask_ref) {
  // Internal pipelines have no dynamic shading rate: the next guest draw sets it again.
  fragment_shading_rate_valid_ = false;
  if (!keep_dynamic_depth_bias) {
    dynamic_depth_bias_update_needed_ = true;
  }
  if (!keep_dynamic_blend_constants) {
    dynamic_blend_constants_update_needed_ = true;
  }
  if (!keep_dynamic_stencil_mask_ref) {
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
  }
  if (current_external_graphics_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  current_external_graphics_pipeline_ = pipeline;
  current_guest_graphics_pipeline_ = VK_NULL_HANDLE;
  current_guest_graphics_pipeline_layout_ = VK_NULL_HANDLE;
  // Its static state replaces the dynamic state of the library pipelines.
  current_library_dynamic_state_valid_ = false;
}

VkPipelineLayout VulkanCommandProcessor::GetLibraryLayout(bool vertex_stage,
                                                          VkDescriptorSetLayout textures_layout) {
  std::lock_guard<std::recursive_mutex> layouts_lock(pipeline_layouts_mutex_);
  auto& layouts = vertex_stage ? library_layouts_vertex_ : library_layouts_pixel_;
  auto it = layouts.find(textures_layout);
  if (it != layouts.end()) {
    return it->second;
  }
  VkDescriptorSetLayout set_layouts[SpirvShaderTranslator::kDescriptorSetCount];
  set_layouts[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
      descriptor_set_layout_shared_memory_and_edram_;
  set_layouts[SpirvShaderTranslator::kDescriptorSetConstants] = descriptor_set_layout_constants_;
  set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
      vertex_stage ? textures_layout : VK_NULL_HANDLE;
  set_layouts[SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
      vertex_stage ? VK_NULL_HANDLE : textures_layout;
  VkPipelineLayoutCreateInfo create_info = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  create_info.flags = VK_PIPELINE_LAYOUT_CREATE_INDEPENDENT_SETS_BIT_EXT;
  create_info.setLayoutCount = uint32_t(rex::countof(set_layouts));
  create_info.pSetLayouts = set_layouts;
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  VkPipelineLayout layout = VK_NULL_HANDLE;
  if (vulkan_device->functions().vkCreatePipelineLayout(vulkan_device->device(), &create_info,
                                                        nullptr, &layout) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create a Vulkan pipeline library layout");
    return VK_NULL_HANDLE;
  }
  layouts.emplace(textures_layout, layout);
  return layout;
}

VkPipelineLayout VulkanCommandProcessor::GetLibraryLayoutForTextures(bool vertex_stage,
                                                                     size_t texture_count,
                                                                     size_t sampler_count) {
  std::lock_guard<std::recursive_mutex> layouts_lock(pipeline_layouts_mutex_);
  VkDescriptorSetLayout textures_layout =
      GetTextureDescriptorSetLayout(vertex_stage, texture_count, sampler_count);
  if (textures_layout == VK_NULL_HANDLE) {
    return VK_NULL_HANDLE;
  }
  return GetLibraryLayout(vertex_stage, textures_layout);
}

void VulkanCommandProcessor::ApplyLibraryPipelineDynamicState(
    const VulkanPipelineCache::PipelineDynamicState& state) {
  const bool valid = current_library_dynamic_state_valid_;
  VulkanPipelineCache::PipelineDynamicState& current = current_library_dynamic_state_;
  if (!valid || current.cull_mode != state.cull_mode) {
    deferred_command_buffer_.CmdVkSetCullMode(state.cull_mode);
  }
  if (!valid || current.front_face != state.front_face) {
    deferred_command_buffer_.CmdVkSetFrontFace(state.front_face);
  }
  if (!valid || current.depth_test_enable != state.depth_test_enable) {
    deferred_command_buffer_.CmdVkSetDepthTestEnable(state.depth_test_enable);
  }
  if (!valid || current.depth_write_enable != state.depth_write_enable) {
    deferred_command_buffer_.CmdVkSetDepthWriteEnable(state.depth_write_enable);
  }
  if (!valid || current.depth_compare_op != state.depth_compare_op) {
    deferred_command_buffer_.CmdVkSetDepthCompareOp(state.depth_compare_op);
  }
  if (!valid || current.stencil_test_enable != state.stencil_test_enable) {
    deferred_command_buffer_.CmdVkSetStencilTestEnable(state.stencil_test_enable);
  }
  if (!valid || std::memcmp(current.stencil_front_ops, state.stencil_front_ops,
                            sizeof(state.stencil_front_ops)) ||
      current.stencil_front_compare_op != state.stencil_front_compare_op) {
    deferred_command_buffer_.CmdVkSetStencilOp(
        VK_STENCIL_FACE_FRONT_BIT, state.stencil_front_ops[0], state.stencil_front_ops[1],
        state.stencil_front_ops[2], state.stencil_front_compare_op);
  }
  if (!valid || std::memcmp(current.stencil_back_ops, state.stencil_back_ops,
                            sizeof(state.stencil_back_ops)) ||
      current.stencil_back_compare_op != state.stencil_back_compare_op) {
    deferred_command_buffer_.CmdVkSetStencilOp(
        VK_STENCIL_FACE_BACK_BIT, state.stencil_back_ops[0], state.stencil_back_ops[1],
        state.stencil_back_ops[2], state.stencil_back_compare_op);
  }
  if (!valid || current.depth_bias_enable != state.depth_bias_enable) {
    deferred_command_buffer_.CmdVkSetDepthBiasEnable(state.depth_bias_enable);
  }
  if (!valid || current.rasterizer_discard_enable != state.rasterizer_discard_enable) {
    deferred_command_buffer_.CmdVkSetRasterizerDiscardEnable(state.rasterizer_discard_enable);
  }
  if (!valid || current.polygon_mode != state.polygon_mode) {
    deferred_command_buffer_.CmdVkSetPolygonModeEXT(state.polygon_mode);
  }
  if (!valid || current.depth_clamp_enable != state.depth_clamp_enable) {
    deferred_command_buffer_.CmdVkSetDepthClampEnableEXT(state.depth_clamp_enable);
  }
  uint32_t count = state.color_attachment_count;
  if (count) {
    if (!valid || current.color_attachment_count != count ||
        std::memcmp(current.color_blend_enable, state.color_blend_enable,
                    sizeof(VkBool32) * count)) {
      deferred_command_buffer_.CmdVkSetColorBlendEnableEXT(0, count, state.color_blend_enable);
    }
    if (!valid || current.color_attachment_count != count ||
        std::memcmp(current.color_blend_equation, state.color_blend_equation,
                    sizeof(VkColorBlendEquationEXT) * count)) {
      deferred_command_buffer_.CmdVkSetColorBlendEquationEXT(0, count,
                                                             state.color_blend_equation);
    }
    if (!valid || current.color_attachment_count != count ||
        std::memcmp(current.color_write_mask, state.color_write_mask,
                    sizeof(VkColorComponentFlags) * count)) {
      deferred_command_buffer_.CmdVkSetColorWriteMaskEXT(0, count, state.color_write_mask);
    }
  }
  current = state;
  current_library_dynamic_state_valid_ = true;
}

void VulkanCommandProcessor::BindExternalComputePipeline(VkPipeline pipeline) {
  if (current_external_compute_pipeline_ == pipeline) {
    return;
  }
  deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  current_external_compute_pipeline_ = pipeline;
}

void VulkanCommandProcessor::SetViewport(const VkViewport& viewport) {
  if (!dynamic_viewport_update_needed_) {
    dynamic_viewport_update_needed_ |= dynamic_viewport_.x != viewport.x;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.y != viewport.y;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.width != viewport.width;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.height != viewport.height;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.minDepth != viewport.minDepth;
    dynamic_viewport_update_needed_ |= dynamic_viewport_.maxDepth != viewport.maxDepth;
  }
  if (dynamic_viewport_update_needed_) {
    dynamic_viewport_ = viewport;
    deferred_command_buffer_.CmdVkSetViewport(0, 1, &dynamic_viewport_);
    dynamic_viewport_update_needed_ = false;
  }
}

void VulkanCommandProcessor::SetScissor(const VkRect2D& scissor) {
  if (!dynamic_scissor_update_needed_) {
    dynamic_scissor_update_needed_ |= dynamic_scissor_.offset.x != scissor.offset.x;
    dynamic_scissor_update_needed_ |= dynamic_scissor_.offset.y != scissor.offset.y;
    dynamic_scissor_update_needed_ |= dynamic_scissor_.extent.width != scissor.extent.width;
    dynamic_scissor_update_needed_ |= dynamic_scissor_.extent.height != scissor.extent.height;
  }
  if (dynamic_scissor_update_needed_) {
    dynamic_scissor_ = scissor;
    deferred_command_buffer_.CmdVkSetScissor(0, 1, &dynamic_scissor_);
    dynamic_scissor_update_needed_ = false;
  }
}

void VulkanCommandProcessor::CpFrameOnSwap() {
  const uint64_t now = CpNowNs();
  auto& f = cp_frame_;
  const uint32_t threshold_ms = REXCVAR_GET(vulkan_slow_frame_ms);
  uint64_t cpu_ns = 0, run_ns = 0, queue_ns = 0;
  // Release builds too (one comparison per frame): the S25's GPU hangs froze the picture for 360-480 ms each before
  // the device was lost, with nothing in the log.
  if (int32_t long_ms = REXCVAR_GET(vulkan_gpu_stall_log_ms);
      long_ms > 0 && f.start_ns && now - f.start_ns >= uint64_t(long_ms) * 1000000) {
    static std::atomic<uint32_t> long_frames{0};
    const uint32_t n = long_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 30 || n % 50 == 0) {
      REXGPU_WARN("Long frame: {:.0f} ms between swaps ({} so far; a load, the app in the background, or a GPU "
                  "hang the driver reset)",
                  (now - f.start_ns) / 1e6, n);
    }
  }
  if (threshold_ms) {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    cpu_ns = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
    rex::system::frame_breakdown::ThreadSchedStat(run_ns, queue_ns);
  }
  if (f.start_ns && threshold_ms && now - f.start_ns > uint64_t(threshold_ms) * 1000000) {
    // The thread's view (frame_breakdown): CPU time, time waiting for a core, time waiting for the game's commands;
    // at = steady clock (s) to line it up with the game thread's "frame breakdown" lines.
    REXGPU_INFO("GPU slow frame (thread): {:.1f} ms between swaps at={:.3f}: cpu {:.1f} + run queue {:.1f} + "
                "waiting for commands {:.1f} + WAIT_REG_MEM {:.1f} + fence waits {:.1f}; present (wall) {:.1f}; "
                "last GPU frame {} {:.1f} ms (frame {})",
                (now - f.start_ns) / 1e6, rex::system::frame_breakdown::NowNs() / 1e9,
                (cpu_ns - f.cpu_ns_start) / 1e6, (queue_ns - f.queue_ns_start) / 1e6,
                (stall_ns_total_ - f.stall_ns_start) / 1e6,
                (wait_reg_mem_us_total_ - f.regmem_us_start) / 1e3, f.fence_us / 1e3, f.present_ns / 1e6,
                gpu_last_frame_index_, gpu_last_frame_ms_, gpu_timing_frame_index_);
    const double total = (now - f.start_ns) / 1e6;
    const double regmem = (wait_reg_mem_us_total_ - f.regmem_us_start) / 1e3;
    const double known = (f.translate_ns + f.textures_ns + f.pipeline_ns + f.rt_update_ns +
                          f.copy_ns + f.submit_ns + f.primitives_ns + f.vertex_buffers_ns +
                          f.memexport_ns) / 1e6 +
                         f.fence_us / 1e3 + regmem;
    REXGPU_INFO("GPU slow frame: {:.1f} ms between swaps = shader translation {:.1f} + pipelines "
                "{:.1f} + textures {:.1f} + render targets {:.1f} + copies {:.1f} ({}) + submits "
                "{:.1f} + fence waits {:.1f} + WAIT_REG_MEM {:.1f} + primitives/indices {:.1f} + "
                "vertex buffers {:.1f} ({:.1f} MB requested) + memexport watches {:.1f} ({} draws, "
                "{} to unknown destinations, {:.1f} MB) + other {:.1f}; {} draws",
                total, f.translate_ns / 1e6, f.pipeline_ns / 1e6, f.textures_ns / 1e6,
                f.rt_update_ns / 1e6, f.copy_ns / 1e6, f.copies, f.submit_ns / 1e6, f.fence_us / 1e3,
                regmem, f.primitives_ns / 1e6, f.vertex_buffers_ns / 1e6,
                f.vertex_buffer_bytes / 1048576.0, f.memexport_ns / 1e6, f.memexport_draws,
                f.memexport_unknown_draws, f.memexport_bytes / 1048576.0, total - known, f.draws);
  }
  f = CpFrameStats();
  f.start_ns = now;
  f.regmem_us_start = wait_reg_mem_us_total_;
  f.stall_ns_start = stall_ns_total_;
  f.cpu_ns_start = cpu_ns;
  f.run_ns_start = run_ns;
  f.queue_ns_start = queue_ns;
}

void VulkanCommandProcessor::NoteFenceWait(void* caller, uint64_t us) {
  cp_frame_.fence_us += us;
  auto& f = fence_wait_stats_;
  ++f.waits;
  f.us += us;
  f.max_us = std::max(f.max_us, us);
  for (auto& c : f.callers) {
    if (c.first == caller || c.first == nullptr) {
      c.first = caller;
      c.second += us;
      break;
    }
  }
  const auto now = std::chrono::steady_clock::now();
  if (f.start == std::chrono::steady_clock::time_point()) f.start = now;
  if (now - f.start >= std::chrono::seconds(10)) {
    std::string callers;
    Dl_info info{};
    for (const auto& c : f.callers) {
      if (!c.first) break;
      uintptr_t offset = 0;
      if (dladdr(c.first, &info) && info.dli_fbase) {
        offset = uintptr_t(c.first) - uintptr_t(info.dli_fbase);
      }
      callers += fmt::format(" +{:x} {:.1f} ms;", offset, c.second / 1000.0);
    }
    REXGPU_INFO("GPU fence waits on the command thread (10 s): {} waits, {:.1f} ms (longest "
                "{:.2f} ms), {} submissions before waits; by caller:{}",
                f.waits, f.us / 1000.0, f.max_us / 1000.0, wait_submissions_, callers);
    f = FenceWaitStats();
    f.start = now;
    wait_submissions_ = 0;
  }
}

void VulkanCommandProcessor::RewriteDepth4xAs1x(const VulkanShader* pixel_shader) {
  if (!REXCVAR_GET(vulkan_depth_4x_as_1x) ||
      render_target_cache_->GetPath() != RenderTargetCache::Path::kHostRenderTargets) {
    return;
  }
  const RegisterFile& regs = *register_file_;
  uint32_t& surface_info_value = register_file_->values[XE_GPU_REG_RB_SURFACE_INFO];
  reg::RB_SURFACE_INFO surface_info;
  surface_info.value = surface_info_value;
  if (surface_info.msaa_samples != xenos::MsaaSamples::k4X) {
    return;
  }
  // A depth surface drawn with color at least once belongs to a multisampled scene; depth-only
  // 4x surfaces are a title's trick (Lightning Returns clears its 1x shadow maps with a 4x
  // depth-only rectangle at half the resolution).
  const uint32_t depth_surface =
      regs.Get<reg::RB_DEPTH_INFO>().base_tiles() | (uint32_t(surface_info.surface_pitch) << 16);
  const uint32_t color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  if (color_mask) {
    depth_4x_scene_surfaces_.insert(depth_surface);
    return;
  }
  if (pixel_shader || depth_4x_scene_surfaces_.count(depth_surface) ||
      texture_cache_->draw_resolution_scale_x() != 1 ||
      texture_cache_->draw_resolution_scale_y() != 1 ||
      uint32_t(surface_info.surface_pitch) * 2 > xenos::kTexture2DCubeMaxWidthHeight) {
    return;
  }
  // The same EDRAM tiles hold the 2x2 samples of a 4x pixel like 2x2 pixels of a 1x surface
  // twice as wide: 1x, double pitch, and the draw's viewport, scissor and extent doubled.
  surface_info.msaa_samples = xenos::MsaaSamples::k1X;
  surface_info.surface_pitch = surface_info.surface_pitch * 2;
  surface_info_value = surface_info.value;
  draw_samples_as_pixels_shift_ = 1;
  render_target_cache_->SetDrawSamplesAsPixels(true);
  ++depth_4x_as_1x_draws_;
}

void VulkanCommandProcessor::PrepareForWait() {
  const uint32_t min_draws = REXCVAR_GET(vulkan_submit_before_wait_min_draws);
  if (min_draws && submission_open_ && submission_draws_ >= min_draws &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
    ++wait_submissions_;
  }
}

void VulkanCommandProcessor::OnPrimaryBufferEnd() {
  if (REXCVAR_GET(vulkan_submit_on_primary_buffer_end) && submission_open_ &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

Shader* VulkanCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

bool VulkanCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  (void)index_buffer_info;
  ++resolve_dump_draws_;
  auto draw_fail = [&](const char* stage) {
    auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
    REXGPU_ERROR(
        "Vulkan IssueDraw failed at {} "
        "(prim_type={}, index_count={}, source_select={}, major_mode={}, explicit_major={}, "
        "path_select={}, tess_mode={}, edram_mode={})",
        stage, uint32_t(prim_type), index_count, uint32_t(vgt_draw_initiator.source_select),
        uint32_t(vgt_draw_initiator.major_mode), uint32_t(major_mode_explicit),
        uint32_t(regs.Get<reg::VGT_OUTPUT_PATH_CNTL>().path_select),
        uint32_t(regs.Get<reg::VGT_HOS_CNTL>().tess_mode),
        uint32_t(regs.Get<reg::RB_MODECONTROL>().edram_mode));
    return false;
  };

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    return IssueCopy();
  }

  bool surface_pitch_is_zero = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0;

  const ui::vulkan::VulkanDevice::Properties& device_properties = GetVulkanDevice()->properties();

  memexport_ranges_.clear();

  // Vertex shader analysis.
  auto vertex_shader = static_cast<VulkanShader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return draw_fail("missing_vertex_shader");
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;
  if (memexport_used_vertex) {
    if (!device_properties.vertexPipelineStoresAndAtomics) {
      // Left out (logged once): the device cannot write memory from vertex shaders (Arm's Mali driver).
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXGPU_WARN(
            "Vertex shader memexport draw left out: no vertexPipelineStoresAndAtomics on this device "
            "(further ones are left out silently)");
      }
      return true;
    }
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (surface_pitch_is_zero && is_rasterization_done) {
    // Doesn't actually draw.
    // Unlikely that zero would even really be legal though.
    return true;
  }
  VulkanShader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<VulkanShader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      return true;
    }
  }
  // Diagnostics: skip every draw with the given vertex shader ucode hash
  // (gpu_debug_skip_vertex_shader, hex), to measure what those draws cost.
  {
    const DebugSkipLists& skip_lists = CurrentDebugSkipLists();
    if (!skip_lists.vertex.empty()) {
      const uint64_t h = vertex_shader->ucode_data_hash();
      for (uint64_t skip : skip_lists.vertex) {
        if (h == skip) return true;
      }
    }
    if (Shader* ps = active_pixel_shader()) {
      const uint64_t h = ps->ucode_data_hash();
      if (IsPixelShaderSkipped(h)) return true;
      for (uint64_t skip : skip_lists.pixel) {
        if (h == skip) return true;
      }
    }
  }
  // Lightning Returns, diagnostic: the battle-exit grey filter (a blurred,
  // desaturated full-screen overlay, pixel shader 8DA8D0C5199404FF, drawn after
  // the frame's crossfade by the blend constant) flickers / shows line noise on
  // Adreno. vulkan_lr_skip_battle_grey leaves it out to confirm that it is the
  // source; not a fix.
  if (REXCVAR_GET(vulkan_lr_skip_battle_grey)) {
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      uint32_t blend_control = regs.values[XE_GPU_REG_RB_BLENDCONTROL0 + i];
      // Color source CONSTANT_COLOR, destination ONE_MINUS_CONSTANT_COLOR.
      if ((blend_control & 0x1F) == 12 && ((blend_control >> 8) & 0x1F) == 13) {
        lr_crossfade_this_frame_ = true;
      }
    }
    if (lr_crossfade_this_frame_ && pixel_shader &&
        pixel_shader->ucode_data_hash() == UINT64_C(0x8DA8D0C5199404FF)) {
      return true;
    }
  }
  // vulkan_depth_4x_as_1x: a 4x depth-only draw into a depth surface never drawn with color is
  // drawn into the 1x surface its samples form (for this draw only; restored on every exit).
  struct SurfaceInfoRestore {
    VulkanCommandProcessor& cp;
    uint32_t surface_info;
    ~SurfaceInfoRestore() {
      cp.register_file_->values[XE_GPU_REG_RB_SURFACE_INFO] = surface_info;
      cp.draw_samples_as_pixels_shift_ = 0;
      cp.render_target_cache_->SetDrawSamplesAsPixels(false);
    }
  } surface_info_restore{*this, register_file_->values[XE_GPU_REG_RB_SURFACE_INFO]};
  RewriteDepth4xAs1x(pixel_shader);
  bool memexport_used_pixel = pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  if (memexport_used_pixel) {
    if (!device_properties.fragmentStoresAndAtomics) {
      REXGPU_ERROR(
          "Pixel shader memexport draw encountered without "
          "fragmentStoresAndAtomics support");
      return false;
    }
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);
  {
    static const uint64_t no_depth_test_pixel_shader_hash =
        std::strtoull(REXCVAR_GET(gpu_debug_no_depth_test_pixel_shader).c_str(), nullptr, 16);
    if (no_depth_test_pixel_shader_hash && pixel_shader &&
        pixel_shader->ucode_data_hash() == no_depth_test_pixel_shader_hash) {
      normalized_depth_control.z_enable = 0;
      normalized_depth_control.z_write_enable = 0;
    }
  }
  // vulkan_skip_no_effect_draws: a draw with no pixel shader (so no colour), no depth or stencil test or write, no
  // memexport and no occlusion query writes nothing. DS2 issues hundreds of them per frame (1-vertex point lists with
  // a constant-position vertex shader, B6C9863F710683EC; Fable, 2026-11-21); each still went through the whole draw
  // path on the CPU and the GPU.
  if (REXCVAR_GET(vulkan_skip_no_effect_draws) && !pixel_shader && !memexport_used_vertex &&
      !active_occlusion_query_.valid && !normalized_depth_control.z_enable &&
      !normalized_depth_control.z_write_enable && !normalized_depth_control.stencil_enable) {
    ++no_effect_draws_skipped_;
    return true;
  }

  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;
  // With pipeline libraries, each shader's translation should not depend on the shader it is
  // paired with, so that its library can be compiled ahead of time: the vertex shader exports
  // every interpolator it writes and the pixel shader reads every interpolator it uses. Only when
  // the vertex shader doesn't write something the pixel shader reads (the pixel shader must then
  // read zeros there, which needs the pair's intersection) both keep the intersection.
  uint32_t vertex_interpolator_mask = interpolator_mask;
  uint32_t pixel_interpolator_mask = interpolator_mask;
  if (pipeline_cache_->UsesCanonicalInterpolatorMasks()) {
    uint32_t pixel_reads =
        pixel_shader ? pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                              regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                              ps_param_gen_pos)
                     : 0;
    if (!(pixel_reads & ~vertex_shader->writes_interpolators())) {
      vertex_interpolator_mask = vertex_shader->writes_interpolators();
      pixel_interpolator_mask = pixel_reads;
    }
    // vulkan_depth_only_vertex_no_interpolators: a draw without a pixel shader (shadow casters,
    // depth pre-pass) reads no interpolators, so its vertex shader variant writes none and the
    // driver drops their computation (pipeline libraries link without cross-stage optimisation).
    // Position is Invariant, so it matches the full variant's bit for bit.
    if (!pixel_shader && REXCVAR_GET(vulkan_depth_only_vertex_no_interpolators)) {
      vertex_interpolator_mask = 0;
      pixel_interpolator_mask = 0;
    }
  }

  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  SpirvShaderTranslator::Modification vertex_shader_modification;
  SpirvShaderTranslator::Modification pixel_shader_modification;
  VulkanShader::VulkanTranslation* vertex_shader_translation;
  VulkanShader::VulkanTranslation* pixel_shader_translation;
  // With shader translation on the pipeline creation threads, a shader's bindings are read only
  // once its translation is complete (snapshot per draw: a creation thread may publish it at
  // any moment, and the bindings must not be read while it writes them).
  const bool translate_on_creation_threads =
      pipeline_cache_->TranslatesShadersOnCreationThreads();
  bool vertex_shader_translated = false;
  bool pixel_shader_translated = false;
  bool memexport_writes_possible = memexport_used_vertex || memexport_used_pixel;

  // Two iterations because a submission (even the current one - in which case
  // it needs to be ended, and a new one must be started) may need to be awaited
  // in case of a sampler count overflow, and if that happens, all subsystem
  // updates done previously must be performed again because the updates done
  // before the awaiting may be referencing objects destroyed by
  // CompletedSubmissionUpdated.
  for (uint32_t i = 0; i < 2; ++i) {
    if (!BeginSubmission(true)) {
      return draw_fail("begin_submission");
    }

    // Process primitives.
    bool primitives_processed;
    {
      CpScopedTimer primitives_timer(cp_frame_.primitives_ns);
      primitives_processed = primitive_processor_->Process(primitive_processing_result);
    }
    if (!primitives_processed) {
      return draw_fail("primitive_processing");
    }
    if (!primitive_processing_result.host_draw_vertex_count) {
      // Nothing to draw.
      return true;
    }
    if (primitive_processing_result.host_primitive_type == xenos::PrimitiveType::kTriangleFan) {
      // Vulkan uses the same fan-to-list conversion policy as D3D12.
      REXGPU_ERROR(
          "PrimitiveProcessor returned triangle fan for Vulkan draw; expected "
          "triangle list conversion for D3D12 parity");
      assert_always();
      return false;
    }
    // Tessellation and rectangle expansion variants for rasterization are
    // produced by the primitive processor and are handled by the Vulkan
    // pipeline cache.
    Shader::HostVertexShaderType host_vertex_shader_type =
        primitive_processing_result.host_vertex_shader_type;
    if (host_vertex_shader_type != Shader::HostVertexShaderType::kVertex &&
        host_vertex_shader_type != Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        host_vertex_shader_type != Shader::HostVertexShaderType::kRectangleListAsTriangleStrip &&
        !Shader::IsHostVertexShaderTypeDomain(host_vertex_shader_type)) {
      REXGPU_ERROR("Unsupported Vulkan host vertex shader type {}",
                   uint32_t(host_vertex_shader_type));
      return false;
    }

    // Shader modifications.
    vertex_shader_modification = pipeline_cache_->GetCurrentVertexShaderModification(
        *vertex_shader, primitive_processing_result.host_vertex_shader_type,
        vertex_interpolator_mask, ps_param_gen_pos != UINT32_MAX);
    pixel_shader_modification = pixel_shader ? pipeline_cache_->GetCurrentPixelShaderModification(
                                                   *pixel_shader, pixel_interpolator_mask,
                                                   ps_param_gen_pos, normalized_depth_control)
                                             : SpirvShaderTranslator::Modification(0);

    // Translate the shaders now to obtain the sampler bindings.
    vertex_shader_translation = static_cast<VulkanShader::VulkanTranslation*>(
        vertex_shader->GetOrCreateTranslation(vertex_shader_modification.value));
    pixel_shader_translation =
        pixel_shader ? static_cast<VulkanShader::VulkanTranslation*>(
                           pixel_shader->GetOrCreateTranslation(pixel_shader_modification.value))
                     : nullptr;
    if (translate_on_creation_threads) {
      // ConfigurePipeline analyses the ucode and queues the translation with the pipeline.
      vertex_shader_translated = vertex_shader_translation->is_translated();
      pixel_shader_translated =
          pixel_shader_translation != nullptr && pixel_shader_translation->is_translated();
      if ((vertex_shader_translated && !vertex_shader_translation->is_valid()) ||
          (pixel_shader_translated && !pixel_shader_translation->is_valid())) {
        return draw_fail("shader_translation");
      }
    } else {
      CpScopedTimer translate_timer(cp_frame_.translate_ns);
      if (!pipeline_cache_->EnsureShadersTranslated(vertex_shader_translation,
                                                    pixel_shader_translation)) {
        return draw_fail("shader_translation");
      }
      vertex_shader_translated = true;
      pixel_shader_translated = pixel_shader_translation != nullptr;
    }

    // Obtain the samplers. Note that the bindings don't depend on the shader
    // modification, so if on the second iteration of this loop it becomes
    // different for some reason (like a race condition with the guest in index
    // buffer processing in the primitive processor resulting in different host
    // vertex shader types), the bindings will stay the same.
    // TODO(Triang3l): Sampler caching and reuse for adjacent draws within one
    // submission.
    uint32_t samplers_overflowed_count = 0;
    for (uint32_t j = 0; j < 2; ++j) {
      std::vector<std::pair<VulkanTextureCache::SamplerParameters, VkSampler>>& shader_samplers =
          j ? current_samplers_pixel_ : current_samplers_vertex_;
      if (!i) {
        shader_samplers.clear();
      }
      const VulkanShader* shader = j ? pixel_shader : vertex_shader;
      if (!shader || !(j ? pixel_shader_translated : vertex_shader_translated)) {
        continue;
      }
      const std::vector<VulkanShader::SamplerBinding>& shader_sampler_bindings =
          shader->GetSamplerBindingsAfterTranslation();
      if (!i) {
        shader_samplers.reserve(shader_sampler_bindings.size());
        for (const VulkanShader::SamplerBinding& shader_sampler_binding : shader_sampler_bindings) {
          shader_samplers.emplace_back(texture_cache_->GetSamplerParameters(shader_sampler_binding),
                                       VK_NULL_HANDLE);
        }
      }
      for (std::pair<VulkanTextureCache::SamplerParameters, VkSampler>& shader_sampler_pair :
           shader_samplers) {
        // UseSampler calls are needed even on the second iteration in case the
        // submission was broken (and thus the last usage submission indices for
        // the used samplers need to be updated) due to an overflow within one
        // submission. Though sampler overflow is a very rare situation overall.
        bool sampler_overflowed;
        VkSampler shader_sampler =
            texture_cache_->UseSampler(shader_sampler_pair.first, sampler_overflowed);
        shader_sampler_pair.second = shader_sampler;
        if (shader_sampler == VK_NULL_HANDLE) {
          if (!sampler_overflowed || i) {
            // If !sampler_overflowed, just failed to create a sampler for some
            // reason.
            // If i == 1, an overflow has happened twice, can't recover from it
            // anymore (would enter an infinite loop otherwise if the number of
            // attempts was not limited to 2). Possibly too many unique samplers
            // in one draw, or failed to await submission completion.
            return draw_fail("sampler_acquisition");
          }
          ++samplers_overflowed_count;
        }
      }
    }
    if (!samplers_overflowed_count) {
      break;
    }
    assert_zero(i);
    // Free space for as many samplers as how many haven't been allocated
    // successfully - obtain the submission index that needs to be awaited to
    // reuse `samplers_overflowed_count` slots. This must be done after all the
    // UseSampler calls, not inside the loop calling UseSampler, because earlier
    // UseSampler calls may "mark for deletion" some samplers that later
    // UseSampler calls in the loop may actually demand.
    uint64_t sampler_overflow_await_submission =
        texture_cache_->GetSubmissionToAwaitOnSamplerOverflow(samplers_overflowed_count);
    assert_true(sampler_overflow_await_submission <= GetCurrentSubmission());
    CheckSubmissionFenceAndDeviceLoss(sampler_overflow_await_submission);
  }

  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;

  // Update the textures before most other work in the submission because
  // samplers depend on this (and in case of sampler overflow in a submission,
  // submissions must be split) - may perform dispatches and copying.
  // With shader translation on the pipeline creation threads
  // (vulkan_async_shader_translation) the texture mask and the sampler bindings of a shader
  // not translated yet are empty; if its pipeline gets published before this draw binds, the
  // draw is skipped below like a pending one.
  const bool shaders_pending_translation =
      !vertex_shader_translated || (pixel_shader_translation != nullptr && !pixel_shader_translated);
  uint32_t used_texture_mask =
      (vertex_shader_translated ? vertex_shader->GetUsedTextureMaskAfterTranslation() : 0) |
      (pixel_shader_translated ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  {
    CpScopedTimer textures_timer(cp_frame_.textures_ns);
    texture_cache_->RequestTextures(used_texture_mask);
  }

  const VulkanPipelineCache::PipelineLayoutProvider* pipeline_layout_provider;
  // Set up the render targets - this may perform dispatches and draws.
  render_target_cache_->SetUpdatePixelShader(pixel_shader);
  ++cp_frame_.draws;
  bool rt_updated;
  {
    CpScopedTimer rt_timer(cp_frame_.rt_update_ns);
    rt_updated = render_target_cache_->Update(is_rasterization_done, normalized_depth_control,
                                              normalized_color_mask, *vertex_shader);
  }
  if (!rt_updated) {
    return draw_fail("render_target_update");
  }
  if (render_target_cache_->last_update_draw_consumed()) {
    // gpu_redirect_msaa_depth_clears: the render target cache did this clear-like draw itself.
    return true;
  }

  // Create the pipeline (for this, need the render pass from the render target
  // cache), translating the shaders - doing this now to obtain the used
  // textures.
  VkPipeline pipeline;
  void* pipeline_handle = nullptr;
  // Sign classes of the pixel shader's textures (texture sign specialization), from the bindings
  // RequestTextures has just updated - the same signs the system constants get.
  uint32_t texture_sign_classes = 0;
  if (pixel_shader && !shaders_pending_translation &&
      SpirvShaderTranslator::IsTextureSignSpecializationEnabled()) {
    texture_sign_classes = VulkanPipelineCache::GetTextureSignClasses(
        pixel_shader->GetUsedTextureMaskAfterTranslation(), [this](uint32_t fetch_constant_index) {
          return texture_cache_->GetActiveTextureSwizzledSigns(fetch_constant_index);
        });
    // DS2 port: when every texture of the pixel shader has exponent adjustment 0, the specialized
    // variant drops the multiply by 2^exp_adjust (kTextureSignClassesExpAdjustZero).
    if (texture_sign_classes && REXCVAR_GET(spirv_texture_exp_adjust_specialization)) {
      bool exp_adjust_zero = true;
      uint32_t textures_remaining = pixel_shader->GetUsedTextureMaskAfterTranslation();
      uint32_t texture_index;
      while (exp_adjust_zero && rex::bit_scan_forward(textures_remaining, &texture_index)) {
        textures_remaining &= ~(UINT32_C(1) << texture_index);
        exp_adjust_zero = regs.GetTextureFetch(texture_index).exp_adjust == 0;
      }
      if (exp_adjust_zero) {
        texture_sign_classes |= SpirvShaderTranslator::kTextureSignClassesExpAdjustZero;
      }
    }
  }
  // The same for the vertex shader's first textures (Lightning Returns: fetch constant 16).
  uint32_t vertex_texture_sign_classes = 0;
  if (!shaders_pending_translation &&
      SpirvShaderTranslator::IsVertexTextureSignSpecializationEnabled()) {
    vertex_texture_sign_classes = VulkanPipelineCache::GetTextureSignClasses(
        vertex_shader->GetUsedTextureMaskAfterTranslation(),
        [this](uint32_t fetch_constant_index) {
          return texture_cache_->GetActiveTextureSwizzledSigns(fetch_constant_index);
        },
        SpirvShaderTranslator::kVertexTextureSignClassSlots);
  }
  bool pipeline_configured;
  {
    CpScopedTimer pipeline_timer(cp_frame_.pipeline_ns);
    pipeline_configured = pipeline_cache_->ConfigurePipeline(
        vertex_shader_translation, pixel_shader_translation, primitive_processing_result,
        normalized_depth_control, normalized_color_mask,
        render_target_cache_->last_update_render_pass_key(), pipeline, pipeline_layout_provider,
        &pipeline_handle, texture_sign_classes, vertex_texture_sign_classes);
  }
  if (!pipeline_configured) {
    return draw_fail("configure_pipeline");
  }
  bool pipeline_is_placeholder = false;
  // Reload the current handle state to observe async hot-swap completions that
  // may happen between pipeline configuration and binding.
  pipeline_cache_->GetPipelineAndLayoutByHandle(pipeline_handle, pipeline, pipeline_layout_provider,
                                                &pipeline_is_placeholder);
  if (REXCVAR_GET(async_shader_compilation) && pipeline_is_placeholder) {
    frame_used_async_placeholder_pipeline_ = true;
    ++counter_draws_skipped_pending_;
    return true;
  }
  if (shaders_pending_translation && pipeline != VK_NULL_HANDLE) {
    // Translated and published by a creation thread between the texture setup and here.
    ++counter_draws_skipped_pending_;
    return true;
  }
  if (pipeline == VK_NULL_HANDLE || pipeline_layout_provider == nullptr) {
    return draw_fail("pipeline_lookup");
  }
  if (current_guest_graphics_pipeline_ != pipeline) {
    deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    current_guest_graphics_pipeline_ = pipeline;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
  }
  if (const VulkanPipelineCache::PipelineDynamicState* library_dynamic_state =
          pipeline_cache_->GetPipelineDynamicState(pipeline_handle)) {
    ApplyLibraryPipelineDynamicState(*library_dynamic_state);
  } else {
    current_library_dynamic_state_valid_ = false;
  }

  // Update the graphics pipeline, and if the new graphics pipeline has a
  // different layout, invalidate incompatible descriptor sets before updating
  // current_guest_graphics_pipeline_layout_.
  auto pipeline_layout = static_cast<const PipelineLayout*>(pipeline_layout_provider);
  if (current_guest_graphics_pipeline_layout_ != pipeline_layout) {
    if (current_guest_graphics_pipeline_layout_) {
      // Keep descriptor set layouts for which the new pipeline layout is
      // compatible with the previous one (pipeline layouts are compatible for
      // set N if set layouts 0 through N are compatible).
      uint32_t descriptor_sets_kept = uint32_t(SpirvShaderTranslator::kDescriptorSetCount);
      if (current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_vertex_ref() !=
          pipeline_layout->descriptor_set_layout_textures_vertex_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept, uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesVertex));
      }
      if (current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_pixel_ref() !=
          pipeline_layout->descriptor_set_layout_textures_pixel_ref()) {
        descriptor_sets_kept = std::min(
            descriptor_sets_kept, uint32_t(SpirvShaderTranslator::kDescriptorSetTexturesPixel));
      }
    } else {
      // No or unknown pipeline layout previously bound - all bindings are in an
      // indeterminate state.
      current_graphics_descriptor_sets_bound_up_to_date_ = 0;
    }
    current_guest_graphics_pipeline_layout_ = pipeline_layout;
  }

  bool host_render_targets_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets;
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x()
                                    << draw_samples_as_pixels_shift_;
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y()
                                    << draw_samples_as_pixels_shift_;

  // Get dynamic rasterizer state.
  draw_util::ViewportInfo viewport_info;
  // Just handling maxViewportDimensions is enough - viewportBoundsRange[1] must
  // be at least 2 * max(maxViewportDimensions[0...1]) - 1, and
  // maxViewportDimensions must be greater than or equal to the size of the
  // largest possible framebuffer attachment (if the viewport has positive
  // offset and is between maxViewportDimensions and viewportBoundsRange[1],
  // GetHostViewportInfo will adjust ndc_scale/ndc_offset to clamp it, and the
  // clamped range will be outside the largest possible framebuffer anyway.
  // FIXME(Triang3l): Possibly handle maxViewportDimensions and
  // viewportBoundsRange separately because when using fragment shader
  // interlocks, framebuffers are not used, while the range may be wider than
  // dimensions? Though viewport bigger than 4096 - the smallest possible
  // maximum dimension (which is below the 8192 texture size limit on the Xbox
  // 360) - and with offset, is probably a situation that never happens in real
  // life. Or even disregard the viewport bounds range in the fragment shader
  // interlocks case completely - apply the viewport and the scissor offset
  // directly to pixel address and to things like ps_param_gen.
  draw_util::GetHostViewportInfo(
      regs, draw_resolution_scale_x, draw_resolution_scale_y, false,
      device_properties.maxViewportDimensions[0], device_properties.maxViewportDimensions[1], true,
      normalized_depth_control,
      host_render_targets_used && render_target_cache_->depth_float24_convert_in_pixel_shader(),
      host_render_targets_used, pixel_shader && pixel_shader->writes_depth(), viewport_info);

  // gpu_log_viewport_width (diagnostic): draws whose viewport is exactly that wide into a wider render target (a
  // screen-space pass sized for a fixed width, e.g. 1280 on a wider screen), once per shader pair.
  if (const int32_t log_width = REXCVAR_GET(gpu_log_viewport_width); log_width != 0) {
    const uint32_t pitch = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch;
    if (log_width < 0 || (viewport_info.xy_extent[0] == uint32_t(log_width) && pitch > uint32_t(log_width))) {
      static std::mutex mutex;
      static std::set<std::pair<uint64_t, uint64_t>> seen;
      const uint64_t vs = vertex_shader->ucode_data_hash(), ps = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
      std::lock_guard<std::mutex> lock(mutex);
      if (seen.size() < (log_width < 0 ? 400u : 40u) && seen.emplace(vs, ps).second) {
        REXGPU_INFO("Viewport {}x{} at {},{} into pitch {}: VS {:016X} PS {:016X} depth {} color mask {:08X}",
                    viewport_info.xy_extent[0], viewport_info.xy_extent[1], viewport_info.xy_offset[0],
                    viewport_info.xy_offset[1], pitch, vs, ps, uint32_t(normalized_depth_control.z_enable),
                    regs.values[XE_GPU_REG_RB_COLOR_MASK]);
      }
    }
  }

  // gpu_keep_aspect_pixel_shaders: these shaders' draws (a movie player's full-screen quad) keep the 16:9 shape when
  // their viewport is wider, so a movie is not stretched across a wide screen: by default the picture is scaled up to
  // the full width (clip-space Y scaled about the centre, the top and bottom cropped: a movie's letterbox bars), with
  // gpu_keep_aspect_pillarbox a centred 16:9 viewport.
  if (pixel_shader) {
    static const std::vector<uint64_t> pillarbox = [] {
      std::vector<uint64_t> hashes;
      const std::string list = REXCVAR_GET(gpu_keep_aspect_pixel_shaders);
      for (size_t pos = 0; pos < list.size();) {
        size_t end = list.find(',', pos);
        if (end == std::string::npos) end = list.size();
        const std::string token = list.substr(pos, end - pos);
        if (!token.empty()) hashes.push_back(std::strtoull(token.c_str(), nullptr, 16));
        pos = end + 1;
      }
      return hashes;
    }();
    if (!pillarbox.empty() &&
        std::find(pillarbox.begin(), pillarbox.end(), pixel_shader->ucode_data_hash()) != pillarbox.end()) {
      const uint32_t width = viewport_info.xy_extent[0], height = viewport_info.xy_extent[1];
      const uint32_t wanted = (height * 16 + 8) / 9;
      if (height && width > wanted + 1) {
        if (REXCVAR_GET(gpu_keep_aspect_pillarbox)) {
          viewport_info.xy_offset[0] += (width - wanted) / 2;
          viewport_info.xy_extent[0] = wanted;
        } else {
          const float zoom = float(width) * 9.0f / (16.0f * float(height));
          viewport_info.ndc_scale[1] *= zoom;
          viewport_info.ndc_offset[1] *= zoom;
        }
      }
    }
  }

  // gpu_float24_absolute_depth_bias: the console adds the constant polygon offset to depth as an
  // absolute value. A float32 host depth buffer scales the constant bias with the exponent of the
  // primitive's depth, and the float24 conversion factor is exact only for guest depth 0.5-1. Moving
  // the vertex position's NDC z by offset / (host depth range) adds exactly the offset to the
  // rasterized depth at every depth (the host maps guest [0, 2) to [0, 1) with host render
  // targets: half the offset in host units).
  float24_depth_bias_in_vertex_shader_ = false;
  if (host_render_targets_used && normalized_depth_control.z_enable &&
      regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24FS8 &&
      !(pixel_shader && pixel_shader->writes_depth()) &&
      !render_target_cache_->depth_float24_convert_in_pixel_shader() &&
      (REXCVAR_GET(gpu_float24_absolute_depth_bias) || REXCVAR_GET(gpu_log_float24_depth_bias))) {
    float poly_offset_scale, poly_offset;
    draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, poly_offset_scale,
                                             poly_offset);
    float host_depth_range = viewport_info.z_max - viewport_info.z_min;
    // A nearly flat depth range (sky or HUD squeezed to z 1..1+eps) would turn the offset into a
    // huge NDC shift that clips the whole draw: those keep the host depth bias.
    float ndc_offset_z =
        std::abs(host_depth_range) >= 1.0e-3f ? poly_offset * 0.5f / host_depth_range : 0.0f;
    if (poly_offset != 0.0f && ndc_offset_z != 0.0f && std::abs(ndc_offset_z) <= 0.01f) {
      if (REXCVAR_GET(gpu_log_float24_depth_bias)) {
        static std::mutex log_mutex;
        static std::vector<std::array<float, 4>> logged;
        std::lock_guard<std::mutex> lock(log_mutex);
        std::array<float, 4> entry = {poly_offset, poly_offset_scale, viewport_info.z_min,
                                      viewport_info.z_max};
        if (logged.size() < 32 && std::find(logged.begin(), logged.end(), entry) == logged.end()) {
          logged.push_back(entry);
          REXGPU_INFO(
              "float24 depth bias: offset {:.9g} slope scale {:.6g}, host depth {:.6g}..{:.6g} "
              "(ndc z {:.6g} + {:.9g}), polygonal {}, cull {}{}",
              poly_offset, poly_offset_scale, viewport_info.z_min, viewport_info.z_max,
              viewport_info.ndc_offset[2], ndc_offset_z, primitive_polygonal,
              uint32_t(regs.Get<reg::PA_SU_SC_MODE_CNTL>().cull_front) |
                  (uint32_t(regs.Get<reg::PA_SU_SC_MODE_CNTL>().cull_back) << 1),
              REXCVAR_GET(gpu_float24_absolute_depth_bias) ? "" : " (not applied)");
        }
      }
      if (REXCVAR_GET(gpu_float24_absolute_depth_bias)) {
        viewport_info.ndc_offset[2] += ndc_offset_z;
        float24_depth_bias_in_vertex_shader_ = true;
      }
    }
  }

  // Update dynamic graphics pipeline state.
  {
    // gpu_debug_zero_scissor_vertex_shader (diagnostics): the vertex side of these draws only.
    const DebugSkipLists& skip_lists = CurrentDebugSkipLists();
    debug_zero_scissor_ = false;
    if (!skip_lists.zero_scissor_vertex.empty()) {
      const uint64_t h = vertex_shader->ucode_data_hash();
      for (uint64_t hash : skip_lists.zero_scissor_vertex) debug_zero_scissor_ |= h == hash;
    }
  }
  UpdateDynamicState(viewport_info, primitive_polygonal, normalized_depth_control);

  // Eco mode (gpu_eco_shading_rate): coarse shading only for the 3D scene - depth-tested draws with
  // a pixel shader that does not write depth, into a full-resolution (>= 1024 pixels wide) 1x render
  // target. Shadow maps, half-resolution effects, post-processing, the HUD and text (no depth test)
  // keep one shader invocation per pixel; coverage and depth stay per pixel everywhere.
  if (pipeline_cache_->UsesDynamicFragmentShadingRate()) {
    uint32_t rate = 0;
    if (eco_shading_rate_ && normalized_depth_control.z_enable && pixel_shader &&
        !pixel_shader->writes_depth()) {
      auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
      if (rb_surface_info.msaa_samples == xenos::MsaaSamples::k1X &&
          rb_surface_info.surface_pitch >= 1024) {
        rate = eco_shading_rate_;
      }
    }
    if (!fragment_shading_rate_valid_ || rate != current_fragment_shading_rate_) {
      deferred_command_buffer_.CmdVkSetFragmentShadingRateKHR(rate ? (rate & 0xF) : 1,
                                                               rate ? (rate >> 4) : 1);
      current_fragment_shading_rate_ = rate;
      fragment_shading_rate_valid_ = true;
    }
  }

  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();

  // Whether to load the guest 32-bit (usually big-endian) vertex index
  // indirectly in the vertex shader if full 32-bit indices are not supported by
  // the host.
  bool shader_32bit_index_dma =
      !device_properties.fullDrawIndexUint32 &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32 &&
      primitive_processing_result.host_vertex_shader_type == Shader::HostVertexShaderType::kVertex;
  if (!device_properties.fullDrawIndexUint32 &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32 &&
      primitive_processing_result.host_vertex_shader_type !=
          Shader::HostVertexShaderType::kVertex) {
    // PrimitiveProcessor is expected to pre-convert this case to
    // ProcessedIndexBufferType::kHostConverted with host-endian 24-bit
    // indices. Keep drawing, but report if that pre-conversion didn't happen.
    static bool non_vertex_32bit_guest_dma_no_full_uint32_logged = false;
    if (!non_vertex_32bit_guest_dma_no_full_uint32_logged) {
      REXGPU_WARN(
          "Vulkan draw has host vertex shader type {} with guest DMA 32-bit "
          "indices on a device without fullDrawIndexUint32; expected "
          "PrimitiveProcessor to return host-converted indices",
          uint32_t(primitive_processing_result.host_vertex_shader_type));
      non_vertex_32bit_guest_dma_no_full_uint32_logged = true;
    }
  }

  // Update system constants before uploading them.
  UpdateSystemConstantValues(primitive_polygonal, primitive_processing_result,
                             shader_32bit_index_dma, 0, viewport_info, used_texture_mask,
                             normalized_depth_control, normalized_color_mask);

  // Update uniform buffers and descriptor sets after binding the pipeline with
  // the new layout.
  if (!UpdateBindings(vertex_shader, pixel_shader)) {
    return draw_fail("update_bindings");
  }

  // Ensure vertex buffers are resident.
  const Shader::ConstantRegisterMap& constant_map_vertex = vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      uint64_t vfetch_bit = uint64_t(1) << (vfetch_index & 63);
      if (vertex_buffers_in_sync_[vfetch_index >> 6] & vfetch_bit) {
        // Read without a request, possibly first in this submission: uploads of its pages later
        // in this submission must not be hoisted before this draw (vulkan_shared_memory_hoist_uploads).
        const VertexBufferState& synced_state = vertex_buffer_states_[vfetch_index];
        shared_memory_->TouchRange(synced_state.address << 2, synced_state.size << 2);
        continue;
      }
      xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
      switch (vfetch_constant.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
            break;
          }
          REXGPU_WARN(
              "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
              "This "
              "is incorrect behavior, but you can try bypassing this by "
              "launching Xenia with --gpu_allow_invalid_fetch_constants=true.",
              vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
        default:
          REXGPU_WARN("Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
      }
      VertexBufferState& state = vertex_buffer_states_[vfetch_index];
      if (REXCVAR_GET(vulkan_vertex_buffer_skip_same_range) &&
          state.address == vfetch_constant.address && state.size == vfetch_constant.size) {
        vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
        // Same as above: read without a request in this submission.
        shared_memory_->TouchRange(state.address << 2, state.size << 2);
        continue;
      }
      bool vertex_buffer_requested;
      {
        CpScopedTimer vertex_buffer_timer(cp_frame_.vertex_buffers_ns);
        cp_frame_.vertex_buffer_bytes += uint64_t(vfetch_constant.size) << 2;
        vertex_buffer_requested =
            shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2);
      }
      if (!vertex_buffer_requested) {
        REXGPU_ERROR(
            "Failed to request vertex buffer at 0x{:08X} (size {}) in the shared "
            "memory",
            vfetch_constant.address << 2, vfetch_constant.size << 2);
        return false;
      }
      state.address = vfetch_constant.address;
      state.size = vfetch_constant.size;
      vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
    }
  }

  // Does this draw's vertex fetch read memexport writes that no barrier has covered yet?
  bool reads_pending_memexport = true;
  if (REXCVAR_GET(vulkan_memexport_coalesce_barriers)) {
    reads_pending_memexport = false;
    const std::pair<uint32_t, uint32_t> pending = shared_memory_->pending_written_range();
    if (pending.second) {
      for (uint32_t i = 0;
           i < rex::countof(constant_map_vertex.vertex_fetch_bitmap) && !reads_pending_memexport;
           ++i) {
        uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
        uint32_t j;
        while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
          vfetch_bits_remaining &= ~(uint32_t(1) << j);
          const VertexBufferState& state = vertex_buffer_states_[i * 32 + j];
          const uint64_t start = uint64_t(state.address) << 2, length = uint64_t(state.size) << 2;
          if (length && start < uint64_t(pending.first) + pending.second &&
              uint64_t(pending.first) < start + length) {
            reads_pending_memexport = true;
            break;
          }
        }
      }
    }
  }

  // Synchronize the memory pages backing memory scatter export streams, and
  // calculate the range that includes the streams for the buffer barrier.
  if (resolve_dump_active_ && resolve_dump_draw_log_.size() < 4096) {
    // DS2 port diagnostic: every draw of the dump frame with its state, textures and pixel shader constants.
    char line[8192];
    int n = std::snprintf(
        line, sizeof(line),
        "d%u prim=%u count=%u vs=%016llX ps=%016llX surf=%08X c0=%08X c1=%08X c2=%08X c3=%08X depth=%08X "
        "mask=%08X blend0=%08X colorctl=%08X depthctl=%08X mode=%08X scissor=%08X/%08X",
        resolve_dump_draws_, uint32_t(prim_type), index_count,
        (unsigned long long)vertex_shader->ucode_data_hash(),
        (unsigned long long)(pixel_shader ? pixel_shader->ucode_data_hash() : 0),
        regs.values[XE_GPU_REG_RB_SURFACE_INFO], regs.values[XE_GPU_REG_RB_COLOR_INFO],
        regs.values[XE_GPU_REG_RB_COLOR1_INFO], regs.values[XE_GPU_REG_RB_COLOR2_INFO],
        regs.values[XE_GPU_REG_RB_COLOR3_INFO], regs.values[XE_GPU_REG_RB_DEPTH_INFO],
        regs.values[XE_GPU_REG_RB_COLOR_MASK], regs.values[XE_GPU_REG_RB_BLENDCONTROL0],
        regs.values[XE_GPU_REG_RB_COLORCONTROL], regs.values[XE_GPU_REG_RB_DEPTHCONTROL],
        regs.values[XE_GPU_REG_RB_MODECONTROL], regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_TL],
        regs.values[XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR]);
    if (pixel_shader && n > 0) {
      for (const auto& binding : pixel_shader->texture_bindings()) {
        if (n >= int(sizeof(line)) - 96) break;
        const uint32_t* f = &regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + binding.fetch_constant * 6];
        n += std::snprintf(line + n, sizeof(line) - n, " t%u=%08X,%08X,%08X,%08X", binding.fetch_constant, f[0],
                           f[1], f[2], f[3]);
      }
      n += std::snprintf(line + n, sizeof(line) - n, " alpha_ref=%g",
                         regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF));
      const auto& map = pixel_shader->constant_register_map();
      for (uint32_t i = 0; i < 256; ++i) {
        if (!(map.float_bitmap[i >> 6] & (uint64_t(1) << (i & 63)))) continue;
        if (n >= int(sizeof(line)) - 96) break;
        const uint32_t base = XE_GPU_REG_SHADER_CONSTANT_256_X + i * 4;
        float v[4];
        std::memcpy(v, &regs.values[base], sizeof(v));
        n += std::snprintf(line + n, sizeof(line) - n, " c%u=(%g,%g,%g,%g)", i, v[0], v[1], v[2], v[3]);
      }
      // The vertex shader's float constants too (vc = vertex constant).
      const auto& vmap = vertex_shader->constant_register_map();
      for (uint32_t i = 0; i < 256; ++i) {
        if (!(vmap.float_bitmap[i >> 6] & (uint64_t(1) << (i & 63)))) continue;
        if (n >= int(sizeof(line)) - 96) break;
        const uint32_t base = XE_GPU_REG_SHADER_CONSTANT_000_X + i * 4;
        float v[4];
        std::memcpy(v, &regs.values[base], sizeof(v));
        n += std::snprintf(line + n, sizeof(line) - n, " vc%u=(%g,%g,%g,%g)", i, v[0], v[1], v[2], v[3]);
      }
    }
    resolve_dump_draw_log_.emplace_back(line);
  }
  uint32_t memexport_extent_start = UINT32_MAX, memexport_extent_end = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    uint32_t memexport_range_base_bytes = memexport_range.base_address_dwords << 2;
    if (!shared_memory_->RequestRange(memexport_range_base_bytes, memexport_range.size_bytes)) {
      REXGPU_ERROR(
          "Failed to request memexport stream at 0x{:08X} (size {}) in the "
          "shared memory",
          memexport_range_base_bytes, memexport_range.size_bytes);
      return false;
    }
    memexport_extent_start = std::min(memexport_extent_start, memexport_range_base_bytes);
    memexport_extent_end =
        std::max(memexport_extent_end, memexport_range_base_bytes + memexport_range.size_bytes);
  }
  if (memexport_writes_possible && memexport_ranges_.empty()) {
    if (!shared_memory_->RequestRange(0, SharedMemory::kBufferSize)) {
      REXGPU_ERROR(
          "Failed to request full shared memory residency for unresolved "
          "memexport destinations");
      return false;
    }
  }

  ScratchBufferAcquisition guest_dma_index_scratch_buffer;
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      !shader_32bit_index_dma && memexport_writes_possible) {
    // Align with D3D12 behavior where memexporting draws using guest DMA indices
    // read a snapshot of indices to avoid read/write aliasing through shared
    // memory.
    VkDeviceSize guest_dma_index_size =
        VkDeviceSize(primitive_processing_result.host_draw_vertex_count) *
        (primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
             ? sizeof(uint16_t)
             : sizeof(uint32_t));
    if (guest_dma_index_size) {
      guest_dma_index_scratch_buffer = AcquireScratchGpuBuffer(
          guest_dma_index_size, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
      if (guest_dma_index_scratch_buffer.buffer() == VK_NULL_HANDLE) {
        return draw_fail("guest_dma_index_scratch_buffer");
      }

      shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
      SubmitBarriers(true);
      GpuTimingMark(GpuTimingCategory::kUploads);

      VkBufferCopy guest_dma_index_copy_region = {};
      guest_dma_index_copy_region.srcOffset = primitive_processing_result.guest_index_base;
      guest_dma_index_copy_region.dstOffset = 0;
      guest_dma_index_copy_region.size = guest_dma_index_size;
      deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),
                                               guest_dma_index_scratch_buffer.buffer(), 1,
                                               &guest_dma_index_copy_region);
      PushBufferMemoryBarrier(
          guest_dma_index_scratch_buffer.buffer(), 0, guest_dma_index_size,
          guest_dma_index_scratch_buffer.GetStageMask(), VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
          guest_dma_index_scratch_buffer.GetAccessMask(), VK_ACCESS_INDEX_READ_BIT);
      guest_dma_index_scratch_buffer.SetStageMask(VK_PIPELINE_STAGE_VERTEX_INPUT_BIT);
      guest_dma_index_scratch_buffer.SetAccessMask(VK_ACCESS_INDEX_READ_BIT);
    }
  }

  // Insert the shared memory barrier if needed.
  // TODO(Triang3l): Find some PM4 command that can be used for indication of
  // when memexports should be awaited instead of inserting the barrier in Use
  // every time if memory export was done in the previous draw?
  if (memexport_extent_start < memexport_extent_end) {
    shared_memory_->Use(
        VulkanSharedMemory::Usage::kGuestDrawReadWrite,
        std::make_pair(memexport_extent_start, memexport_extent_end - memexport_extent_start),
        !reads_pending_memexport);
  } else if (memexport_writes_possible) {
    // Stream constants can be invalid or dynamic, making the exact destination
    // unknown. Keep synchronization conservative similarly to D3D12 UAV
    // handling for memexport-capable draws.
    shared_memory_->Use(VulkanSharedMemory::Usage::kGuestDrawReadWrite,
                        std::make_pair(0u, SharedMemory::kBufferSize), !reads_pending_memexport);
  } else {
    shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  }

  // After all commands that may dispatch, copy or insert barriers, submit
  // the barriers (may end the render pass), and (re)enter the render pass
  // before drawing.
  SubmitBarriersAndEnterRenderTargetCacheRenderPass(
      render_target_cache_->last_update_render_pass(),
      render_target_cache_->last_update_framebuffer());

  // The draw can't write outside its scissor (set in UpdateDynamicState). If
  // the dynamic scissor state is unknown, be conservative.
  if (dynamic_scissor_update_needed_) {
    VkRect2D whole_framebuffer;
    whole_framebuffer.offset.x = 0;
    whole_framebuffer.offset.y = 0;
    whole_framebuffer.extent.width = UINT32_MAX >> 1;
    whole_framebuffer.extent.height = UINT32_MAX >> 1;
    ExtendRenderPassArea(whole_framebuffer);
  } else {
    ExtendRenderPassArea(dynamic_scissor_);
  }

  ++gpu_timing_counters_.draws;
  if (gpu_timing_enabled_) {
    ++gpu_timing_tag_stats_[gpu_timing_pass_tag_].draws;
    ++gpu_trace_pass_draws_;
  }
  // Draw.
  ++submission_draws_;
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kNone ||
      shader_32bit_index_dma) {
    deferred_command_buffer_.CmdVkDraw(primitive_processing_result.host_draw_vertex_count, 1, 0, 0);
  } else {
    std::pair<VkBuffer, VkDeviceSize> index_buffer;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        if (guest_dma_index_scratch_buffer.buffer() != VK_NULL_HANDLE) {
          index_buffer.first = guest_dma_index_scratch_buffer.buffer();
          index_buffer.second = 0;
        } else {
          index_buffer.first = shared_memory_->buffer();
          index_buffer.second = primitive_processing_result.guest_index_base;
        }
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer = primitive_processor_->GetConvertedIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer = primitive_processor_->GetBuiltinIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        return draw_fail("unexpected_index_buffer_type");
    }
    deferred_command_buffer_.CmdVkBindIndexBuffer(
        index_buffer.first, index_buffer.second,
        primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
            ? VK_INDEX_TYPE_UINT16
            : VK_INDEX_TYPE_UINT32);
    deferred_command_buffer_.CmdVkDrawIndexed(primitive_processing_result.host_draw_vertex_count, 1,
                                              0, 0, 0);
  }

  if (gpu_trace_active_) {
    GpuTraceDrawTimestamp(vertex_shader->ucode_data_hash(),
                          pixel_shader ? pixel_shader->ucode_data_hash() : 0, uint32_t(prim_type),
                          index_count);
  }

  // Invalidate textures in memexported memory and watch for changes.
  if (!memexport_ranges_.empty()) {
    CpScopedTimer memexport_timer(cp_frame_.memexport_ns);
    ++cp_frame_.memexport_draws;
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      cp_frame_.memexport_bytes += memexport_range.size_bytes;
      shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                        memexport_range.size_bytes);
    }
  } else if (memexport_writes_possible) {
    // Stream constants can be invalid or dynamic, so exact destinations may be
    // unknown. Keep invalidation conservative in this case.
    CpScopedTimer memexport_timer(cp_frame_.memexport_ns);
    ++cp_frame_.memexport_draws;
    ++cp_frame_.memexport_unknown_draws;
    shared_memory_->RangeWrittenByGpu(0, SharedMemory::kBufferSize);
  }

  if (IsReadbackMemexportEnabled(REXCVAR_GET(vulkan_readback_memexport)) &&
      !memexport_ranges_.empty()) {
    uint32_t memexport_total_size = 0;
    for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
      memexport_total_size += memexport_range.size_bytes;
    }
    if (memexport_total_size) {
      if (REXCVAR_GET(readback_memexport_fast)) {
        IssueDraw_MemexportReadbackFastPath(memexport_total_size);
      } else {
        IssueDraw_MemexportReadbackFullPath(memexport_total_size);
      }
    }
  }

  // vulkan_submit_every_draws: hand the GPU what is recorded so far once a submission holds that many draws.
  if (const uint32_t every = REXCVAR_GET(vulkan_submit_every_draws);
      every && submission_open_ && submission_draws_ >= every && CanEndSubmissionImmediately()) {
    EndSubmission(false);
    ++draw_count_submissions_;
  }

  return true;
}

bool VulkanCommandProcessor::IssueDraw_MemexportReadbackFullPath(uint32_t total_size) {
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  VkBuffer readback_buffer = VK_NULL_HANDLE;
  VkDeviceMemory readback_memory = VK_NULL_HANDLE;
  uint32_t readback_memory_type = UINT32_MAX;
  VkDeviceSize readback_memory_size = 0;
  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, total_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, readback_buffer, readback_memory,
          &readback_memory_type, &readback_memory_size)) {
    REXGPU_ERROR("Failed to create a Vulkan memexport readback buffer");
    return true;
  }

  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  SubmitBarriers(true);

  uint32_t readback_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    VkBufferCopy readback_region = {};
    readback_region.srcOffset = memexport_range.base_address_dwords << 2;
    readback_region.dstOffset = readback_offset;
    readback_region.size = memexport_range.size_bytes;
    deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(), readback_buffer, 1,
                                             &readback_region);
    readback_offset += memexport_range.size_bytes;
  }
  PushBufferMemoryBarrier(readback_buffer, 0, total_size, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_ACCESS_HOST_READ_BIT);

  if (!AwaitAllQueueOperationsCompletion()) {
    REXGPU_ERROR("Failed to await completion of Vulkan memexport readback");
    dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
    dfn.vkFreeMemory(device, readback_memory, nullptr);
    return true;
  }

  void* readback_mapping = nullptr;
  if (dfn.vkMapMemory(device, readback_memory, 0, VK_WHOLE_SIZE, 0, &readback_mapping) !=
      VK_SUCCESS) {
    REXGPU_ERROR("Failed to map a Vulkan memexport readback buffer");
    dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
    dfn.vkFreeMemory(device, readback_memory, nullptr);
    return true;
  }

  if (!(vulkan_device->memory_types().host_coherent & (uint32_t(1) << readback_memory_type))) {
    VkMappedMemoryRange readback_memory_range = {};
    readback_memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    readback_memory_range.memory = readback_memory;
    readback_memory_range.offset = 0;
    readback_memory_range.size = std::min(
        rex::round_up(VkDeviceSize(total_size), vulkan_device->properties().nonCoherentAtomSize),
        readback_memory_size);
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &readback_memory_range);
  }

  const uint8_t* readback_bytes = reinterpret_cast<const uint8_t*>(readback_mapping);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }
  dfn.vkUnmapMemory(device, readback_memory);
  dfn.vkDestroyBuffer(device, readback_buffer, nullptr);
  dfn.vkFreeMemory(device, readback_memory, nullptr);
  return true;
}

bool VulkanCommandProcessor::IssueDraw_MemexportReadbackFastPath(uint32_t total_size) {
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  const uint64_t readback_key =
      MakeMemexportReadbackKey(memexport_ranges_.front().base_address_dwords, total_size);
  ReadbackBuffer& readback = memexport_readback_buffers_[readback_key];
  readback.last_used_frame = frame_current_;

  auto ensure_readback_slot = [&](uint32_t index, uint32_t size) -> bool {
    if (readback.buffers[index] != VK_NULL_HANDLE && readback.memories[index] != VK_NULL_HANDLE &&
        readback.mapped_data[index] != nullptr && size <= readback.sizes[index]) {
      return true;
    }

    VkBuffer new_buffer = VK_NULL_HANDLE;
    VkDeviceMemory new_memory = VK_NULL_HANDLE;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kReadback, new_buffer, new_memory)) {
      return false;
    }

    void* new_mapping = nullptr;
    if (dfn.vkMapMemory(device, new_memory, 0, VK_WHOLE_SIZE, 0, &new_mapping) != VK_SUCCESS) {
      dfn.vkDestroyBuffer(device, new_buffer, nullptr);
      dfn.vkFreeMemory(device, new_memory, nullptr);
      return false;
    }

    if (readback.buffers[index] != VK_NULL_HANDLE || readback.memories[index] != VK_NULL_HANDLE) {
      if (!AwaitAllQueueOperationsCompletion()) {
        dfn.vkUnmapMemory(device, new_memory);
        dfn.vkDestroyBuffer(device, new_buffer, nullptr);
        dfn.vkFreeMemory(device, new_memory, nullptr);
        return false;
      }
      if (readback.mapped_data[index] != nullptr && readback.memories[index] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[index]);
      }
      if (readback.buffers[index] != VK_NULL_HANDLE) {
        dfn.vkDestroyBuffer(device, readback.buffers[index], nullptr);
      }
      if (readback.memories[index] != VK_NULL_HANDLE) {
        dfn.vkFreeMemory(device, readback.memories[index], nullptr);
      }
    }

    readback.buffers[index] = new_buffer;
    readback.memories[index] = new_memory;
    readback.mapped_data[index] = new_mapping;
    readback.sizes[index] = size;
    readback.submission_written[index] = 0;
    readback.written_size[index] = 0;
    return true;
  };

  const uint32_t write_index = readback.current_index;
  const uint32_t read_index = 1 - write_index;
  const uint32_t readback_size = AlignReadbackBufferSize(total_size);
  if (!ensure_readback_slot(write_index, readback_size)) {
    return IssueDraw_MemexportReadbackFullPath(total_size);
  }

  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  SubmitBarriers(true);

  uint32_t readback_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    VkBufferCopy readback_region = {};
    readback_region.srcOffset = memexport_range.base_address_dwords << 2;
    readback_region.dstOffset = readback_offset;
    readback_region.size = memexport_range.size_bytes;
    deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),
                                             readback.buffers[write_index], 1, &readback_region);
    readback_offset += memexport_range.size_bytes;
  }
  PushBufferMemoryBarrier(readback.buffers[write_index], 0, total_size,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
  readback.submission_written[write_index] = GetCurrentSubmission();
  readback.written_size[write_index] = total_size;

  CheckSubmissionFenceAndDeviceLoss(0);
  bool previous_slot_ready =
      readback.buffers[read_index] != VK_NULL_HANDLE &&
      readback.memories[read_index] != VK_NULL_HANDLE &&
      readback.mapped_data[read_index] != nullptr && total_size <= readback.sizes[read_index] &&
      total_size <= readback.written_size[read_index] && readback.submission_written[read_index] &&
      readback.submission_written[read_index] <= submission_completed_;
  if (!previous_slot_ready) {
    IssueDraw_MemexportReadbackFullPath(total_size);
    readback.current_index = read_index;
    return true;
  }

  VkMappedMemoryRange readback_memory_range = {};
  readback_memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
  readback_memory_range.memory = readback.memories[read_index];
  readback_memory_range.offset = 0;
  readback_memory_range.size = VK_WHOLE_SIZE;
  dfn.vkInvalidateMappedMemoryRanges(device, 1, &readback_memory_range);

  const uint8_t* readback_bytes =
      reinterpret_cast<const uint8_t*>(readback.mapped_data[read_index]);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }

  readback.current_index = read_index;
  return true;
}

bool VulkanCommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  ++cp_frame_.copies;
  CpScopedTimer copy_timer(cp_frame_.copy_ns);

  if (!BeginSubmission(true)) {
    return false;
  }

  ++gpu_timing_counters_.resolves;
  gpu_timing_in_resolve_ = true;
  bool result;
  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(vulkan_readback_resolve));
  if (readback_mode == ReadbackResolveMode::kDisabled && IsForcedReadbackResolveSize()) {
    readback_mode = ReadbackResolveMode::kFull;
    // Few by design (menus): logged so a per-frame match, which would stall every frame, shows up.
    static uint64_t forced_count = 0;
    if (++forced_count <= 3 || forced_count % 1000 == 0) {
      REXGPU_INFO("readback_resolve_sizes: resolve read back ({} so far, frame {})", forced_count, frame_current_);
    }
  }
  if (readback_mode == ReadbackResolveMode::kDisabled) {
    uint32_t written_address, written_length;
    result = render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                           written_address, written_length);
    if (result && resolve_dump_active_ && written_length) {
      // vulkan_resolve_verify_lazy: a lazy resolve is written now (from its fed texture), dumped, then done again
      // eagerly and dumped right after, to compare the two byte for byte.
      const bool verify_lazy =
          REXCVAR_GET(vulkan_resolve_verify_lazy) && render_target_cache_->last_resolve_lazy();
      if (verify_lazy) {
        render_target_cache_->FlushPendingResolveDumpsForVerify(written_address, written_length);
      }
      DumpResolve(written_address, written_length);
      if (verify_lazy) {
        rex::cvar::SetFlagByName("vulkan_resolve_feed_lazy_dump", "false");
        uint32_t verify_address, verify_length;
        if (render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, verify_address,
                                          verify_length) &&
            verify_length) {
          DumpResolve(verify_address, verify_length);
        }
        rex::cvar::SetFlagByName("vulkan_resolve_feed_lazy_dump", "true");
      }
      // vulkan_direct_resolve_7e3_to_8888_verify: the same resolve again through the EDRAM dump and the full
      // resolve shader, dumped right after, so the two outputs of one frame can be compared byte for byte.
      if (REXCVAR_GET(vulkan_direct_resolve_7e3_to_8888_verify) &&
          render_target_cache_->last_resolve_7e3_to_8888()) {
        rex::cvar::SetFlagByName("vulkan_direct_resolve_7e3_to_8888", "false");
        uint32_t verify_address, verify_length;
        if (render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, verify_address,
                                          verify_length) &&
            verify_length) {
          DumpResolve(verify_address, verify_length);
        }
        rex::cvar::SetFlagByName("vulkan_direct_resolve_7e3_to_8888", "true");
      }
    }
  } else {
    result = IssueCopy_ReadbackResolvePath(readback_mode);
  }
  gpu_timing_in_resolve_ = false;
  return result;
}

bool VulkanCommandProcessor::IsForcedReadbackResolveSize() {
  const std::string& sizes = REXCVAR_GET(readback_resolve_sizes);
  if (sizes.empty()) {
    return false;
  }
  // Parsed again only when the setting changes (it is hot-reloadable); resolves are a few dozen per frame.
  static std::string parsed_from;
  static std::vector<std::pair<uint32_t, uint32_t>> parsed;
  if (sizes != parsed_from) {
    parsed_from = sizes;
    parsed.clear();
    size_t start = 0;
    while (start < sizes.size()) {
      size_t end = sizes.find(',', start);
      if (end == std::string::npos) end = sizes.size();
      unsigned w = 0, h = 0;
      if (std::sscanf(sizes.substr(start, end - start).c_str(), " %ux%u", &w, &h) == 2 && w && h) {
        parsed.emplace_back(w, h);
      } else {
        REXGPU_WARN("readback_resolve_sizes: ignoring \"{}\" (expected WxH)", sizes.substr(start, end - start));
      }
      start = end + 1;
    }
  }
  const auto pitch = register_file_->Get<reg::RB_COPY_DEST_PITCH>();
  for (const auto& [w, h] : parsed) {
    if (pitch.copy_dest_pitch == w && pitch.copy_dest_height == h) {
      return true;
    }
  }
  return false;
}

bool VulkanCommandProcessor::IssueCopy_ReadbackResolvePath(ReadbackResolveMode readback_mode) {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  uint32_t written_address, written_length;
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, written_address,
                                     written_length)) {
    return false;
  }

  if (!written_length) {
    return true;
  }
  if (resolve_dump_active_) {
    DumpResolve(written_address, written_length);
  }

  if (!memory_->TranslatePhysical(written_address)) {
    return true;
  }

  if (readback_mode == ReadbackResolveMode::kDisabled) {
    return true;
  }

  auto ensure_readback_slot = [&](ReadbackBuffer& readback, uint32_t index, uint32_t size) -> bool {
    if (readback.buffers[index] != VK_NULL_HANDLE && size <= readback.sizes[index] &&
        readback.mapped_data[index] != nullptr) {
      return true;
    }

    VkBuffer new_buffer = VK_NULL_HANDLE;
    VkDeviceMemory new_memory = VK_NULL_HANDLE;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            ui::vulkan::util::MemoryPurpose::kReadback, new_buffer, new_memory)) {
      REXGPU_ERROR("Failed to create a {} MB Vulkan resolve readback buffer", size >> 20);
      return false;
    }

    void* new_mapping = nullptr;
    if (dfn.vkMapMemory(device, new_memory, 0, VK_WHOLE_SIZE, 0, &new_mapping) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to map a Vulkan resolve readback buffer");
      dfn.vkDestroyBuffer(device, new_buffer, nullptr);
      dfn.vkFreeMemory(device, new_memory, nullptr);
      return false;
    }

    if (readback.buffers[index] != VK_NULL_HANDLE || readback.memories[index] != VK_NULL_HANDLE) {
      if (!AwaitAllQueueOperationsCompletion()) {
        dfn.vkUnmapMemory(device, new_memory);
        dfn.vkDestroyBuffer(device, new_buffer, nullptr);
        dfn.vkFreeMemory(device, new_memory, nullptr);
        return false;
      }
      if (readback.mapped_data[index] != nullptr && readback.memories[index] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[index]);
      }
      if (readback.buffers[index] != VK_NULL_HANDLE) {
        dfn.vkDestroyBuffer(device, readback.buffers[index], nullptr);
      }
      if (readback.memories[index] != VK_NULL_HANDLE) {
        dfn.vkFreeMemory(device, readback.memories[index], nullptr);
      }
    }

    readback.buffers[index] = new_buffer;
    readback.memories[index] = new_memory;
    readback.mapped_data[index] = new_mapping;
    readback.sizes[index] = size;
    return true;
  };

  bool is_scaled = texture_cache_->IsDrawResolutionScaled();
  uint64_t resolve_key = MakeReadbackResolveKey(written_address, written_length);
  ReadbackBuffer& readback = readback_buffers_[resolve_key];
  readback.last_used_frame = frame_current_;
  uint32_t write_index = readback.current_index;
  uint32_t readback_size = AlignReadbackBufferSize(written_length);
  if (!ensure_readback_slot(readback, write_index, readback_size)) {
    return true;
  }

  if (is_scaled) {
    if (!resolve_downscale_pipeline_ || !resolve_downscale_pipeline_layout_) {
      return true;
    }

    reg::RB_COPY_DEST_INFO copy_dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
    const FormatInfo* format_info = FormatInfo::Get(uint32_t(copy_dest_info.copy_dest_format));
    uint32_t bits_per_pixel = format_info->bits_per_pixel;
    if (bits_per_pixel != 8 && bits_per_pixel != 16 && bits_per_pixel != 32 &&
        bits_per_pixel != 64) {
      return true;
    }

    uint32_t pixel_size_log2;
    if (!rex::bit_scan_forward(bits_per_pixel >> 3, &pixel_size_log2)) {
      return true;
    }
    uint32_t tile_size_1x = 32 * 32 * (uint32_t(1) << pixel_size_log2);
    uint32_t tile_count = written_length / tile_size_1x;
    if (!tile_count) {
      return true;
    }

    uint64_t scaled_start = 0, scaled_length = 0;
    if (!texture_cache_->GetScaledResolveRange(written_address, written_length, 0, scaled_start,
                                               scaled_length)) {
      return true;
    }
    if (!scaled_length) {
      return true;
    }

    VkBuffer scaled_resolve_buffer = texture_cache_->scaled_resolve_buffer();
    if (scaled_resolve_buffer == VK_NULL_HANDLE || scaled_start > uint64_t(UINT32_MAX)) {
      return true;
    }

    uint32_t downscale_buffer_size = AlignReadbackBufferSize(written_length);
    if (downscale_buffer_size > resolve_downscale_buffer_size_) {
      VkBuffer new_buffer = VK_NULL_HANDLE;
      VkDeviceMemory new_memory = VK_NULL_HANDLE;
      if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
              vulkan_device, downscale_buffer_size,
              VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
              ui::vulkan::util::MemoryPurpose::kDeviceLocal, new_buffer, new_memory)) {
        REXGPU_ERROR("Failed to create a {} MB Vulkan resolve downscale buffer",
                     downscale_buffer_size >> 20);
        return true;
      }
      if (resolve_downscale_buffer_ != VK_NULL_HANDLE ||
          resolve_downscale_buffer_memory_ != VK_NULL_HANDLE) {
        if (!AwaitAllQueueOperationsCompletion()) {
          dfn.vkDestroyBuffer(device, new_buffer, nullptr);
          dfn.vkFreeMemory(device, new_memory, nullptr);
          return true;
        }
        if (resolve_downscale_buffer_ != VK_NULL_HANDLE) {
          dfn.vkDestroyBuffer(device, resolve_downscale_buffer_, nullptr);
        }
        if (resolve_downscale_buffer_memory_ != VK_NULL_HANDLE) {
          dfn.vkFreeMemory(device, resolve_downscale_buffer_memory_, nullptr);
        }
      }
      resolve_downscale_buffer_ = new_buffer;
      resolve_downscale_buffer_memory_ = new_memory;
      resolve_downscale_buffer_size_ = downscale_buffer_size;
    }
    if (resolve_downscale_buffer_ == VK_NULL_HANDLE) {
      return true;
    }

    VkDescriptorSet descriptor_set = AllocateSingleTransientDescriptor(
        SingleTransientDescriptorLayout::kStorageBufferPairCompute);
    if (descriptor_set == VK_NULL_HANDLE) {
      return true;
    }

    VkDescriptorBufferInfo buffer_infos[2] = {};
    buffer_infos[0].buffer = scaled_resolve_buffer;
    buffer_infos[0].offset = 0;
    buffer_infos[0].range = VK_WHOLE_SIZE;
    buffer_infos[1].buffer = resolve_downscale_buffer_;
    buffer_infos[1].offset = 0;
    buffer_infos[1].range = written_length;

    VkWriteDescriptorSet descriptor_writes[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
      descriptor_writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptor_writes[i].dstSet = descriptor_set;
      descriptor_writes[i].dstBinding = i;
      descriptor_writes[i].descriptorCount = 1;
      descriptor_writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      descriptor_writes[i].pBufferInfo = &buffer_infos[i];
    }
    dfn.vkUpdateDescriptorSets(device, 2, descriptor_writes, 0, nullptr);

    texture_cache_->UseScaledResolveBufferForRead();
    SubmitBarriers(true);
    GpuTimingMark(GpuTimingCategory::kResolves);

    VkBufferMemoryBarrier pre_barrier = {};
    pre_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    pre_barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    pre_barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    pre_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    pre_barrier.buffer = resolve_downscale_buffer_;
    pre_barrier.offset = 0;
    pre_barrier.size = written_length;
    deferred_command_buffer_.CmdVkPipelineBarrier(
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &pre_barrier, 0, nullptr);

    BindExternalComputePipeline(resolve_downscale_pipeline_);

    ResolveDownscaleConstants constants;
    constants.scale_x = texture_cache_->draw_resolution_scale_x();
    constants.scale_y = texture_cache_->draw_resolution_scale_y();
    constants.pixel_size_log2 = pixel_size_log2;
    constants.tile_count = tile_count;
    constants.source_offset_bytes = uint32_t(scaled_start);
    constants.half_pixel_offset = (REXCVAR_GET(readback_resolve_half_pixel_offset) &&
                                   (constants.scale_x > 1 || constants.scale_y > 1))
                                      ? 1u
                                      : 0u;
    deferred_command_buffer_.CmdVkPushConstants(resolve_downscale_pipeline_layout_,
                                                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                                                &constants);
    deferred_command_buffer_.CmdVkBindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE,
                                                     resolve_downscale_pipeline_layout_, 0, 1,
                                                     &descriptor_set, 0, nullptr);
    deferred_command_buffer_.CmdVkDispatch(tile_count, 1, 1);

    VkBufferMemoryBarrier downscale_barrier = {};
    downscale_barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    downscale_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    downscale_barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    downscale_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    downscale_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    downscale_barrier.buffer = resolve_downscale_buffer_;
    downscale_barrier.offset = 0;
    downscale_barrier.size = written_length;
    deferred_command_buffer_.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1,
                                                  &downscale_barrier, 0, nullptr);

    VkBufferCopy readback_region = {};
    readback_region.srcOffset = 0;
    readback_region.dstOffset = 0;
    readback_region.size = written_length;
    deferred_command_buffer_.CmdVkCopyBuffer(resolve_downscale_buffer_,
                                             readback.buffers[write_index], 1, &readback_region);
  } else {
    shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
    SubmitBarriers(true);
    GpuTimingMark(GpuTimingCategory::kResolves);

    VkBufferCopy readback_region = {};
    readback_region.srcOffset = written_address;
    readback_region.dstOffset = 0;
    readback_region.size = written_length;
    deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(),
                                             readback.buffers[write_index], 1, &readback_region);
  }

  PushBufferMemoryBarrier(readback.buffers[write_index], 0, written_length,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                          VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);

  bool use_delayed_sync =
      readback_mode == ReadbackResolveMode::kFast || readback_mode == ReadbackResolveMode::kSome;
  uint32_t read_index = write_index;
  if (use_delayed_sync) {
    read_index = 1 - write_index;
  } else if (!AwaitAllQueueOperationsCompletion()) {
    return true;
  }

  bool is_cache_miss = false;
  if (use_delayed_sync && (readback.buffers[read_index] == VK_NULL_HANDLE ||
                           written_length > readback.sizes[read_index] ||
                           readback.mapped_data[read_index] == nullptr)) {
    is_cache_miss = true;
    read_index = write_index;
    if (!AwaitAllQueueOperationsCompletion()) {
      return true;
    }
  }

  bool should_copy = (readback_mode == ReadbackResolveMode::kSome) ? is_cache_miss : true;
  if (should_copy && readback.buffers[read_index] != VK_NULL_HANDLE &&
      written_length <= readback.sizes[read_index] && readback.mapped_data[read_index] != nullptr) {
    VkMappedMemoryRange readback_memory_range = {};
    readback_memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    readback_memory_range.memory = readback.memories[read_index];
    readback_memory_range.offset = 0;
    readback_memory_range.size = VK_WHOLE_SIZE;
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &readback_memory_range);

    uint8_t* destination = memory_->TranslatePhysical(written_address);
    if (destination) {
      std::memcpy(destination, readback.mapped_data[read_index], written_length);
    }
  }

  readback.current_index = 1 - readback.current_index;
  return true;
}

void VulkanCommandProcessor::DumpResolve(uint32_t written_address, uint32_t written_length) {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  if (resolve_dump_buffer_ == VK_NULL_HANDLE) {
    const uint32_t size = uint32_t(std::clamp(REXCVAR_GET(resolve_dump_max_mb), 4, 256)) << 20;
    if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
            vulkan_device, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, ui::vulkan::util::MemoryPurpose::kReadback,
            resolve_dump_buffer_, resolve_dump_memory_)) {
      REXGPU_ERROR("resolve dump: failed to create a {} MB buffer", size >> 20);
      resolve_dump_active_ = false;
      return;
    }
    if (dfn.vkMapMemory(vulkan_device->device(), resolve_dump_memory_, 0, VK_WHOLE_SIZE, 0,
                        &resolve_dump_mapping_) != VK_SUCCESS) {
      REXGPU_ERROR("resolve dump: failed to map the buffer");
      resolve_dump_active_ = false;
      return;
    }
    resolve_dump_size_ = size;
  }
  const uint32_t offset = rex::round_up(resolve_dump_used_, uint32_t(256));
  if (uint64_t(offset) + written_length > resolve_dump_size_) {
    REXGPU_INFO("resolve dump: buffer full, resolve of {} bytes at 0x{:08X} skipped", written_length,
                written_address);
    return;
  }
  const RegisterFile& regs = *register_file_;
  ResolveDumpEntry e;
  e.address = written_address;
  e.length = written_length;
  e.offset = offset;
  e.copy_dest_base = regs.values[XE_GPU_REG_RB_COPY_DEST_BASE];
  e.copy_dest_info = regs.values[XE_GPU_REG_RB_COPY_DEST_INFO];
  e.copy_dest_pitch = regs.values[XE_GPU_REG_RB_COPY_DEST_PITCH];
  e.copy_control = regs.values[XE_GPU_REG_RB_COPY_CONTROL];
  e.surface_info = regs.values[XE_GPU_REG_RB_SURFACE_INFO];
  e.color_info0 = regs.values[XE_GPU_REG_RB_COLOR_INFO];
  e.depth_info = regs.values[XE_GPU_REG_RB_DEPTH_INFO];
  e.draws_before = resolve_dump_draws_;
  resolve_dump_entries_.push_back(e);
  {
    char line[160];
    std::snprintf(line, sizeof(line), "RESOLVE r%03zu addr=%08X len=%u dest_info=%08X after_draw=%u",
                  resolve_dump_entries_.size() - 1, written_address, written_length, e.copy_dest_info,
                  resolve_dump_draws_);
    resolve_dump_draw_log_.emplace_back(line);
  }
  resolve_dump_used_ = offset + written_length;

  shared_memory_->Use(VulkanSharedMemory::Usage::kRead);
  SubmitBarriers(true);
  VkBufferCopy region = {};
  region.srcOffset = written_address;
  region.dstOffset = offset;
  region.size = written_length;
  deferred_command_buffer_.CmdVkCopyBuffer(shared_memory_->buffer(), resolve_dump_buffer_, 1, &region);
  PushBufferMemoryBarrier(resolve_dump_buffer_, offset, written_length, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
}

void VulkanCommandProcessor::FinishResolveDump() {
  // Called at the Swap: closes the dump frame (if any) and decides whether the next frame is one.
  const std::string dir_cvar = REXCVAR_GET(resolve_dump_dir);
  if (dir_cvar.empty()) {
    return;
  }
  std::filesystem::path dir(dir_cvar);
  if (dir.is_relative()) {
    const auto exe_dir = rex::filesystem::GetExecutableFolder();
    if (!exe_dir.empty()) dir = exe_dir / dir;
  }
  const uint64_t now_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count());
  if (resolve_dump_active_) {
    resolve_dump_active_ = false;
    const auto t0 = std::chrono::steady_clock::now();
    // Waits for this frame's guest work (the Swap's submission is not open yet).
    if (submission_open_) {
      EndSubmission(false);
    }
    if (!AwaitAllQueueOperationsCompletion()) {
      REXGPU_ERROR("resolve dump {}: GPU work did not complete", resolve_dump_count_);
    } else {
      const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
      VkMappedMemoryRange range = {};
      range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
      range.memory = resolve_dump_memory_;
      range.size = VK_WHOLE_SIZE;
      vulkan_device->functions().vkInvalidateMappedMemoryRanges(vulkan_device->device(), 1, &range);
      char sub[32];
      std::snprintf(sub, sizeof(sub), "dump_%02u", resolve_dump_count_);
      const std::filesystem::path out = dir / sub;
      std::error_code ec;
      std::filesystem::create_directories(out, ec);
      FILE* index = std::fopen((out / "index.txt").string().c_str(), "w");
      size_t written = 0;
      for (size_t i = 0; i < resolve_dump_entries_.size(); ++i) {
        const ResolveDumpEntry& e = resolve_dump_entries_[i];
        char name[64];
        std::snprintf(name, sizeof(name), "r%03zu_%08X.bin", i, e.address);
        if (FILE* f = std::fopen((out / name).string().c_str(), "wb")) {
          written += std::fwrite(static_cast<const uint8_t*>(resolve_dump_mapping_) + e.offset, 1, e.length, f);
          std::fclose(f);
        }
        if (index) {
          std::fprintf(index,
                       "%s addr=%08X len=%u dest_base=%08X dest_info=%08X dest_pitch=%08X copy_control=%08X "
                       "surface_info=%08X color_info0=%08X depth_info=%08X draws=%u\n",
                       name, e.address, e.length, e.copy_dest_base, e.copy_dest_info, e.copy_dest_pitch,
                       e.copy_control, e.surface_info, e.color_info0, e.depth_info, e.draws_before);
        }
      }
      if (index) {
        std::fprintf(index, "draws_total=%u\n", resolve_dump_draws_);
        // Gamma ramp the output applies (256-entry table and PWL), 10 bits per channel.
        std::fprintf(index, "gamma256:");
        for (uint32_t i = 0; i < 256; i += 15) {
          const auto& c = gamma_ramp_256_entry_table()[i];
          std::fprintf(index, " %u/%u/%u", c.color_10_red, c.color_10_green, c.color_10_blue);
        }
        std::fprintf(index, "\n");
        std::fclose(index);
      }
      if (FILE* dl = std::fopen((out / "draws.txt").string().c_str(), "w")) {
        for (const std::string& l : resolve_dump_draw_log_) {
          std::fprintf(dl, "%s\n", l.c_str());
        }
        std::fclose(dl);
      }
      REXGPU_INFO("resolve dump {}: {} resolves, {} draws, {:.1f} MB -> {} in {} ms", resolve_dump_count_,
                  resolve_dump_entries_.size(), resolve_dump_draws_, written / 1048576.0, out.string(),
                  std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                      .count());
      ++resolve_dump_count_;
    }
    resolve_dump_entries_.clear();
    resolve_dump_draw_log_.clear();
    resolve_dump_used_ = 0;
    resolve_dump_last_ms_ = now_ms;
  }
  resolve_dump_draws_ = 0;
  if (resolve_dump_count_ >= uint32_t(std::max(0, REXCVAR_GET(resolve_dump_max)))) {
    return;
  }
  const uint64_t every_ms = uint64_t(std::max(1, REXCVAR_GET(resolve_dump_every_s))) * 1000;
  if (!resolve_dump_last_ms_) {
    // First dump one interval after the first frame (or after resolve_dump_start_s).
    resolve_dump_first_ms_ = now_ms;
    resolve_dump_last_ms_ = now_ms;
    return;
  }
  if (now_ms - resolve_dump_first_ms_ < uint64_t(std::max(0, REXCVAR_GET(resolve_dump_start_s))) * 1000) {
    resolve_dump_last_ms_ = now_ms - every_ms;
    return;
  }
  if (now_ms - resolve_dump_last_ms_ >= every_ms) {
    resolve_dump_active_ = true;
  }
}

void VulkanCommandProcessor::EvictOldReadbackBuffers(
    std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map) {
  if (buffer_map.empty()) {
    return;
  }

  const uint64_t eviction_frame_floor = (frame_current_ > kReadbackBufferEvictionAgeFrames)
                                            ? (frame_current_ - kReadbackBufferEvictionAgeFrames)
                                            : 0;
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (auto it = buffer_map.begin(); it != buffer_map.end();) {
    ReadbackBuffer& readback = it->second;
    bool evict =
        buffer_map.size() > kMaxReadbackBuffers || readback.last_used_frame < eviction_frame_floor;
    if (!evict) {
      ++it;
      continue;
    }
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.mapped_data[i] && readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkUnmapMemory(device, readback.memories[i]);
      }
      if (readback.buffers[i] != VK_NULL_HANDLE) {
        dfn.vkDestroyBuffer(device, readback.buffers[i], nullptr);
      }
      if (readback.memories[i] != VK_NULL_HANDLE) {
        dfn.vkFreeMemory(device, readback.memories[i], nullptr);
      }
      readback.buffers[i] = VK_NULL_HANDLE;
      readback.memories[i] = VK_NULL_HANDLE;
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
    it = buffer_map.erase(it);
  }
}

bool VulkanCommandProcessor::InitializeOcclusionQueryResources() {
  active_occlusion_query_ = {};
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
  occlusion_query_pool_ = VK_NULL_HANDLE;
  occlusion_query_readback_buffer_ = VK_NULL_HANDLE;
  occlusion_query_readback_memory_ = VK_NULL_HANDLE;
  occlusion_query_readback_memory_type_ = UINT32_MAX;
  occlusion_query_readback_memory_size_ = 0;
  occlusion_query_readback_mapping_ = nullptr;

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  VkQueryPoolCreateInfo pool_info = {};
  pool_info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  pool_info.queryType = VK_QUERY_TYPE_OCCLUSION;
  pool_info.queryCount = kMaxOcclusionQueries;
  if (dfn.vkCreateQueryPool(device, &pool_info, nullptr, &occlusion_query_pool_) != VK_SUCCESS) {
    REXGPU_WARN(
        "VulkanCommandProcessor: Failed to create occlusion query pool, using fake sample counts");
    return false;
  }

  if (!ui::vulkan::util::CreateDedicatedAllocationBuffer(
          vulkan_device, sizeof(uint64_t) * kMaxOcclusionQueries, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          ui::vulkan::util::MemoryPurpose::kReadback, occlusion_query_readback_buffer_,
          occlusion_query_readback_memory_, &occlusion_query_readback_memory_type_,
          &occlusion_query_readback_memory_size_)) {
    REXGPU_WARN(
        "VulkanCommandProcessor: Failed to create occlusion query readback buffer, using fake "
        "sample counts");
    ShutdownOcclusionQueryResources();
    return false;
  }

  if (dfn.vkMapMemory(device, occlusion_query_readback_memory_, 0, VK_WHOLE_SIZE, 0,
                      reinterpret_cast<void**>(&occlusion_query_readback_mapping_)) != VK_SUCCESS) {
    REXGPU_WARN(
        "VulkanCommandProcessor: Failed to map occlusion query readback buffer, using fake sample "
        "counts");
    ShutdownOcclusionQueryResources();
    return false;
  }

  occlusion_query_resources_available_ = true;
  return true;
}

void VulkanCommandProcessor::ShutdownOcclusionQueryResources() {
  DisableHostOcclusionQueries();

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  VkDevice device = vulkan_device->device();

  if (occlusion_query_readback_mapping_ && occlusion_query_readback_memory_ != VK_NULL_HANDLE) {
    dfn.vkUnmapMemory(device, occlusion_query_readback_memory_);
  }
  occlusion_query_readback_mapping_ = nullptr;
  if (occlusion_query_readback_buffer_ != VK_NULL_HANDLE) {
    dfn.vkDestroyBuffer(device, occlusion_query_readback_buffer_, nullptr);
  }
  if (occlusion_query_readback_memory_ != VK_NULL_HANDLE) {
    dfn.vkFreeMemory(device, occlusion_query_readback_memory_, nullptr);
  }
  if (occlusion_query_pool_ != VK_NULL_HANDLE) {
    dfn.vkDestroyQueryPool(device, occlusion_query_pool_, nullptr);
  }

  occlusion_query_pool_ = VK_NULL_HANDLE;
  occlusion_query_readback_buffer_ = VK_NULL_HANDLE;
  occlusion_query_readback_memory_ = VK_NULL_HANDLE;
  occlusion_query_readback_memory_type_ = UINT32_MAX;
  occlusion_query_readback_memory_size_ = 0;
}

bool VulkanCommandProcessor::AcquireOcclusionQueryIndex(uint32_t& host_index_out) {
  if (occlusion_query_cursor_ >= kMaxOcclusionQueries) {
    occlusion_query_cursor_ = 0;
  }
  host_index_out = occlusion_query_cursor_++;
  return true;
}

void VulkanCommandProcessor::DisableHostOcclusionQueries() {
  if (active_occlusion_query_.valid && occlusion_query_pool_ != VK_NULL_HANDLE) {
    // The query began outside a render pass (BeginGuestOcclusionQuery), so it must end outside one, and only once:
    // it is cleared before EndSubmission, which would otherwise end it a second time. The old order (end inside the
    // open render pass, then a second vkCmdEndQuery in EndSubmission) crashed Arm's Mali driver in vkCmdEndQuery at
    // the first gameplay frames (issues #16 / #17: "Host fault" in libGLES_mali.so, address 0x38).
    const uint32_t host_index = active_occlusion_query_.host_index;
    active_occlusion_query_ = {};
    if (BeginSubmission(true)) {
      EndRenderPass();
      deferred_command_buffer_.CmdVkEndQuery(occlusion_query_pool_, host_index);
      EndSubmission(false);
    }
  }
  active_occlusion_query_ = {};
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
}

bool VulkanCommandProcessor::BeginGuestOcclusionQuery(uint32_t sample_count_address) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||
      occlusion_query_pool_ == VK_NULL_HANDLE || occlusion_query_readback_mapping_ == nullptr) {
    return false;
  }
  if (active_occlusion_query_.valid) {
    REXGPU_WARN(
        "VulkanCommandProcessor: Occlusion query begin issued while another query is active");
    DisableHostOcclusionQueries();
    return false;
  }

  uint32_t host_index = 0;
  if (!AcquireOcclusionQueryIndex(host_index)) {
    return false;
  }
  if (!BeginSubmission(true)) {
    return false;
  }

  EndRenderPass();

  DeferredCommandBuffer& command_buffer = deferred_command_buffer();
  command_buffer.CmdVkResetQueryPool(occlusion_query_pool_, host_index, 1);
  command_buffer.CmdVkBeginQuery(occlusion_query_pool_, host_index, 0);
  active_occlusion_query_.sample_count_address = sample_count_address;
  active_occlusion_query_.host_index = host_index;
  active_occlusion_query_.valid = true;
  return true;
}

bool VulkanCommandProcessor::EndGuestOcclusionQuery(uint32_t sample_count_address) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||
      !active_occlusion_query_.valid || occlusion_query_pool_ == VK_NULL_HANDLE ||
      occlusion_query_readback_mapping_ == nullptr) {
    return false;
  }

  uint32_t host_index = active_occlusion_query_.host_index;
  active_occlusion_query_ = {};

  if (!BeginSubmission(true)) {
    return false;
  }

  EndRenderPass();

  DeferredCommandBuffer& command_buffer = deferred_command_buffer();
  command_buffer.CmdVkEndQuery(occlusion_query_pool_, host_index);
  command_buffer.CmdVkCopyQueryPoolResults(occlusion_query_pool_, host_index, 1,
                                           occlusion_query_readback_buffer_,
                                           sizeof(uint64_t) * host_index, sizeof(uint64_t),
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

  if (!EndSubmission(false)) {
    return false;
  }
  if (!AwaitAllQueueOperationsCompletion()) {
    return false;
  }

  const ui::vulkan::VulkanDevice* vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  if (!(vulkan_device->memory_types().host_coherent &
        (uint32_t(1) << occlusion_query_readback_memory_type_))) {
    VkMappedMemoryRange memory_range = {};
    memory_range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    memory_range.memory = occlusion_query_readback_memory_;
    memory_range.offset = 0;
    memory_range.size =
        std::min(rex::round_up(VkDeviceSize(sizeof(uint64_t) * kMaxOcclusionQueries),
                               vulkan_device->properties().nonCoherentAtomSize),
                 occlusion_query_readback_memory_size_);
    dfn.vkInvalidateMappedMemoryRanges(device, 1, &memory_range);
  }

  const uint64_t* results = reinterpret_cast<const uint64_t*>(occlusion_query_readback_mapping_);
  uint64_t samples = NormalizeOcclusionSamples(results[host_index]);
  WriteGuestOcclusionResult(sample_count_address, samples);
  return true;
}

uint64_t VulkanCommandProcessor::NormalizeOcclusionSamples(uint64_t samples) const {
  if (samples == 0 || !texture_cache_) {
    return samples;
  }
  uint64_t scale_x = texture_cache_->draw_resolution_scale_x();
  uint64_t scale_y = texture_cache_->draw_resolution_scale_y();
  uint64_t scale = scale_x * scale_y;
  if (scale <= 1) {
    return samples;
  }
  return (samples + (scale >> 1)) / scale;
}

void VulkanCommandProcessor::WriteGuestOcclusionResult(uint32_t sample_count_address,
                                                       uint64_t samples) {
  auto* sample_counts =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_address);
  if (!sample_counts) {
    return;
  }
  uint32_t clamped = samples > uint64_t(UINT32_MAX) ? UINT32_MAX : uint32_t(samples);
  sample_counts->Total_A = clamped;
  sample_counts->Total_B = 0;
  sample_counts->ZPass_A = clamped;
  sample_counts->ZPass_B = 0;
  sample_counts->ZFail_A = 0;
  sample_counts->ZFail_B = 0;
  sample_counts->StencilFail_A = 0;
  sample_counts->StencilFail_B = 0;
}

void VulkanCommandProcessor::CheckSubmissionFenceAndDeviceLoss(uint64_t await_submission) {
  // Only report once, no need to retry a wait that won't succeed anyway.
  if (device_lost_) {
    return;
  }

  if (await_submission >= GetCurrentSubmission()) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = GetCurrentSubmission() - 1;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  size_t fences_total = submissions_in_flight_fences_.size();
  size_t fences_awaited = 0;
  // Blocking fence waits on the GPU command thread, by caller (logged with WAIT_REG_MEM's stats).
  struct FenceWaitTimer {
    VulkanCommandProcessor* cp;
    void* caller;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    ~FenceWaitTimer() {
      uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - t0)
                                 .count());
      cp->NoteFenceWait(caller, us);
    }
  };
  std::optional<FenceWaitTimer> fence_wait_timer;
  if (await_submission > submission_completed_) {
    // Statistics only ("GPU fence waits", the slow-frame breakdown): off with vulkan_slow_frame_ms 0.
    if (REXCVAR_GET(vulkan_slow_frame_ms)) {
      fence_wait_timer.emplace(FenceWaitTimer{this, __builtin_return_address(0)});
    }
    // Await in a blocking way if requested.
    // TODO(Triang3l): Await only one fence. "Fence signal operations that are
    // defined by vkQueueSubmit additionally include in the first
    // synchronization scope all commands that occur earlier in submission
    // order."
    VkResult wait_result;
    const auto stall_wait_start = std::chrono::steady_clock::now();
    uint32_t fence_wait_sleep_us = REXCVAR_GET(vulkan_fence_wait_sleep_us);
    if (fence_wait_sleep_us) {
      // Poll and sleep instead of a blocking driver wait (the driver may spin
      // on the CPU while the GPU finishes).
      while (true) {
        wait_result =
            dfn.vkWaitForFences(device, uint32_t(await_submission - submission_completed_),
                                submissions_in_flight_fences_.data(), VK_TRUE, 0);
        if (wait_result != VK_TIMEOUT) {
          break;
        }
        rex::thread::Sleep(std::chrono::microseconds(fence_wait_sleep_us));
      }
    } else {
      wait_result =
          dfn.vkWaitForFences(device, uint32_t(await_submission - submission_completed_),
                              submissions_in_flight_fences_.data(), VK_TRUE, UINT64_MAX);
    }
    if (int32_t stall_log_ms = REXCVAR_GET(vulkan_gpu_stall_log_ms); stall_log_ms > 0) {
      const int64_t waited_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - stall_wait_start)
                                    .count();
      if (waited_ms >= stall_log_ms) {
        static std::atomic<uint32_t> stalls{0};
        const uint32_t n = stalls.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 20 || n % 50 == 0) {
          REXGPU_WARN(
              "GPU stall: waited {} ms for submission {} ({} in flight, result {}); {} wait(s) of {}+ ms "
              "so far (a GPU hang the driver reset, or very long GPU work)",
              waited_ms, await_submission, await_submission - submission_completed_, int(wait_result), n,
              stall_log_ms);
        }
      }
    }
    if (wait_result == VK_SUCCESS) {
      fences_awaited += await_submission - submission_completed_;
    } else {
      REXGPU_ERROR("Failed to await submission completion Vulkan fences");
      if (wait_result == VK_ERROR_DEVICE_LOST) {
        device_lost_ = true;
      }
    }
  }
  // Check how far into the submissions the GPU currently is, in order because
  // submission themselves can be executed out of order, but Xenia serializes
  // that for simplicity.
  while (fences_awaited < fences_total && REXCVAR_GET(vulkan_fence_poll)) {
    VkResult fence_status =
        dfn.vkWaitForFences(device, 1, &submissions_in_flight_fences_[fences_awaited], VK_TRUE, 0);
    if (fence_status != VK_SUCCESS) {
      if (fence_status == VK_ERROR_DEVICE_LOST) {
        device_lost_ = true;
      }
      break;
    }
    ++fences_awaited;
  }
  if (device_lost_) {
    if (graphics_system_) {
      graphics_system_->OnHostGpuLossFromAnyThread(true);
    }
    return;
  }
  if (!fences_awaited) {
    // Not updated - no need to reclaim or download things.
    return;
  }
  // Reclaim fences.
  fences_free_.reserve(fences_free_.size() + fences_awaited);
  auto submissions_in_flight_fences_awaited_end = submissions_in_flight_fences_.cbegin();
  std::advance(submissions_in_flight_fences_awaited_end, fences_awaited);
  fences_free_.insert(fences_free_.cend(), submissions_in_flight_fences_.cbegin(),
                      submissions_in_flight_fences_awaited_end);
  submissions_in_flight_fences_.erase(submissions_in_flight_fences_.cbegin(),
                                      submissions_in_flight_fences_awaited_end);
  submission_completed_ += fences_awaited;

  // Reclaim semaphores.
  while (!submissions_in_flight_semaphores_.empty()) {
    const auto& semaphore_submission = submissions_in_flight_semaphores_.front();
    if (semaphore_submission.first > submission_completed_) {
      break;
    }
    semaphores_free_.push_back(semaphore_submission.second);
    submissions_in_flight_semaphores_.pop_front();
  }

  // Reclaim command pools.
  while (!command_buffers_submitted_.empty()) {
    const auto& command_buffer_pair = command_buffers_submitted_.front();
    if (command_buffer_pair.first > submission_completed_) {
      break;
    }
    command_buffers_writable_.push_back(command_buffer_pair.second);
    command_buffers_submitted_.pop_front();
  }

  if (!gpu_timing_submitted_.empty()) {
    GpuTimingReadCompleted();
  }

  shared_memory_->CompletedSubmissionUpdated();

  primitive_processor_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(submission_completed_);

  // Destroy objects scheduled for destruction.
  while (!destroy_framebuffers_.empty()) {
    const auto& destroy_pair = destroy_framebuffers_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyFramebuffer(device, destroy_pair.second, nullptr);
    destroy_framebuffers_.pop_front();
  }
  while (!destroy_image_views_.empty()) {
    const auto& destroy_pair = destroy_image_views_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyImageView(device, destroy_pair.second, nullptr);
    destroy_image_views_.pop_front();
  }
  while (!destroy_buffers_.empty()) {
    const auto& destroy_pair = destroy_buffers_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyBuffer(device, destroy_pair.second, nullptr);
    destroy_buffers_.pop_front();
  }
  while (!destroy_images_.empty()) {
    const auto& destroy_pair = destroy_images_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkDestroyImage(device, destroy_pair.second, nullptr);
    destroy_images_.pop_front();
  }
  while (!destroy_memory_.empty()) {
    const auto& destroy_pair = destroy_memory_.front();
    if (destroy_pair.first > submission_completed_) {
      break;
    }
    dfn.vkFreeMemory(device, destroy_pair.second, nullptr);
    destroy_memory_.pop_front();
  }
}

bool VulkanCommandProcessor::CanEndSubmissionImmediately() const {
  return !submission_open_ || (!scratch_buffer_used_ && !pipeline_cache_->IsCreatingPipelines());
}

bool VulkanCommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_lost_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame. Also check whether the device
  // is still available, and whether the await was successful.
  uint64_t await_submission =
      is_opening_frame ? closed_frame_submissions_[frame_current_ % kMaxFramesInFlight] : 0;
  CheckSubmissionFenceAndDeviceLoss(await_submission);
  if (device_lost_ || submission_completed_ < await_submission) {
    return false;
  }

  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ = std::max(frame_current_, uint64_t(kMaxFramesInFlight)) - kMaxFramesInFlight;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_; ++frame) {
      if (closed_frame_submissions_[frame % kMaxFramesInFlight] > submission_completed_) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;
    submission_draws_ = 0;

    // Start a new deferred command buffer - will submit it to the real one in
    // the end of the submission (when async pipeline object creation requests
    // are fulfilled).
    deferred_command_buffer_.Reset();
    if (REXCVAR_GET(vulkan_submission_start_full_barrier)) {
      // Diagnostic: order everything in this submission after everything in the
      // previous ones.
      VkMemoryBarrier full_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
      full_barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      full_barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      deferred_command_buffer_.CmdVkPipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                                                    &full_barrier, 0, nullptr, 0, nullptr);
    }

    // Reset cached state of the command buffer.
    dynamic_viewport_update_needed_ = true;
    dynamic_scissor_update_needed_ = true;
    dynamic_depth_bias_update_needed_ = true;
    fragment_shading_rate_valid_ = false;
    {
      // Eco mode, re-read per command buffer (it can change while running).
      const std::string& eco = REXCVAR_GET(gpu_eco_shading_rate);
      uint32_t eco_w = 1, eco_h = 1;
      if (eco.size() == 3 && eco[1] == 'x') {
        eco_w = std::clamp(uint32_t(eco[0] - '0'), uint32_t(1), uint32_t(2));
        eco_h = std::clamp(uint32_t(eco[2] - '0'), uint32_t(1), uint32_t(2));
      }
      eco_shading_rate_ = (eco_w > 1 || eco_h > 1) ? (eco_w | (eco_h << 4)) : 0;
    }
    dynamic_blend_constants_update_needed_ = true;
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
    current_render_pass_ = VK_NULL_HANDLE;
    current_framebuffer_ = nullptr;
    in_render_pass_ = false;
    render_pass_area_.active = false;
    current_guest_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_graphics_pipeline_ = VK_NULL_HANDLE;
    current_external_compute_pipeline_ = VK_NULL_HANDLE;
    current_guest_graphics_pipeline_layout_ = nullptr;
    current_library_dynamic_state_valid_ = false;
    current_graphics_descriptor_sets_bound_up_to_date_ = 0;

    bool gpu_timing_enabled = REXCVAR_GET(vulkan_gpu_timing);
    if (gpu_timing_enabled != gpu_timing_enabled_) {
      gpu_timing_enabled_ = gpu_timing_enabled;
      // Restart the logging window on the next swap.
      gpu_timing_window_start_ns_ = 0;
      if (graphics_system_ && graphics_system_->presenter()) {
        static_cast<ui::vulkan::VulkanPresenter*>(graphics_system_->presenter())
            ->SetPaintGpuTimingEnabled(gpu_timing_enabled);
      }
    }
    if (gpu_timing_enabled_) {
      GpuTimingBeginSubmission();
    }

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(GetCurrentSubmission());
  }

  if (is_opening_frame) {
    frame_open_ = true;
    frame_used_async_placeholder_pipeline_ = false;

    // Reset bindings that depend on transient data.
    std::memset(current_float_constant_map_vertex_, 0, sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
    std::memset(current_graphics_descriptor_sets_, 0, sizeof(current_graphics_descriptor_sets_));
    current_constant_buffers_up_to_date_ = 0;
    current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram] =
        shared_memory_and_edram_descriptor_set_;
    current_graphics_descriptor_set_values_up_to_date_ =
        UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram;

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    // FIXME(Triang3l): This will result in a memory leak if the guest is not
    // presenting.
    uniform_buffer_pool_->Reclaim(frame_completed_);
    while (!single_transient_descriptors_used_.empty()) {
      const UsedSingleTransientDescriptor& used_transient_descriptor =
          single_transient_descriptors_used_.front();
      if (used_transient_descriptor.frame > frame_completed_) {
        break;
      }
      single_transient_descriptors_free_[size_t(used_transient_descriptor.layout)].push_back(
          used_transient_descriptor.set);
      single_transient_descriptors_used_.pop_front();
    }
    while (!constants_transient_descriptors_used_.empty()) {
      const std::pair<uint64_t, VkDescriptorSet>& used_transient_descriptor =
          constants_transient_descriptors_used_.front();
      if (used_transient_descriptor.first > frame_completed_) {
        break;
      }
      constants_transient_descriptors_free_.push_back(used_transient_descriptor.second);
      constants_transient_descriptors_used_.pop_front();
    }
    while (!texture_transient_descriptor_sets_used_.empty()) {
      const UsedTextureTransientDescriptorSet& used_transient_descriptor_set =
          texture_transient_descriptor_sets_used_.front();
      if (used_transient_descriptor_set.frame > frame_completed_) {
        break;
      }
      auto it = texture_transient_descriptor_sets_free_.find(used_transient_descriptor_set.layout);
      if (it == texture_transient_descriptor_sets_free_.end()) {
        it = texture_transient_descriptor_sets_free_
                 .emplace(std::piecewise_construct,
                          std::forward_as_tuple(used_transient_descriptor_set.layout),
                          std::forward_as_tuple())
                 .first;
      }
      it->second.push_back(used_transient_descriptor_set.set);
      texture_transient_descriptor_sets_used_.pop_front();
    }

    EvictOldReadbackBuffers(readback_buffers_);
    EvictOldReadbackBuffers(memexport_readback_buffers_);

    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();
  }

  return true;
}

bool VulkanCommandProcessor::EndSubmission(bool is_swap) {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Make sure everything needed for submitting exist.
  if (submission_open_) {
    if (fences_free_.empty()) {
      VkFenceCreateInfo fence_create_info;
      fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      fence_create_info.pNext = nullptr;
      fence_create_info.flags = 0;
      VkFence fence;
      if (dfn.vkCreateFence(device, &fence_create_info, nullptr, &fence) != VK_SUCCESS) {
        REXGPU_ERROR("Failed to create a Vulkan fence");
        // Try to submit later. Completely dropping the submission is not
        // permitted because resources would be left in an undefined state.
        return false;
      }
      fences_free_.push_back(fence);
    }
    if (!sparse_memory_binds_.empty() && semaphores_free_.empty()) {
      VkSemaphoreCreateInfo semaphore_create_info;
      semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      semaphore_create_info.pNext = nullptr;
      semaphore_create_info.flags = 0;
      VkSemaphore semaphore;
      if (dfn.vkCreateSemaphore(device, &semaphore_create_info, nullptr, &semaphore) !=
          VK_SUCCESS) {
        REXGPU_ERROR("Failed to create a Vulkan semaphore");
        return false;
      }
      semaphores_free_.push_back(semaphore);
    }
    if (command_buffers_writable_.empty()) {
      CommandBuffer command_buffer;
      VkCommandPoolCreateInfo command_pool_create_info;
      command_pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      command_pool_create_info.pNext = nullptr;
      command_pool_create_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
      command_pool_create_info.queueFamilyIndex = vulkan_device->queue_family_graphics_compute();
      if (dfn.vkCreateCommandPool(device, &command_pool_create_info, nullptr,
                                  &command_buffer.pool) != VK_SUCCESS) {
        REXGPU_ERROR("Failed to create a Vulkan command pool");
        return false;
      }
      VkCommandBufferAllocateInfo command_buffer_allocate_info;
      command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      command_buffer_allocate_info.pNext = nullptr;
      command_buffer_allocate_info.commandPool = command_buffer.pool;
      command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      command_buffer_allocate_info.commandBufferCount = 1;
      if (dfn.vkAllocateCommandBuffers(device, &command_buffer_allocate_info,
                                       &command_buffer.buffer) != VK_SUCCESS) {
        REXGPU_ERROR("Failed to allocate a Vulkan command buffer");
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
        return false;
      }
      command_buffers_writable_.push_back(command_buffer);
    }
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    texture_cache_->EndFrame();

    primitive_processor_->EndFrame();
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    // A query still active here began outside a render pass: end the render pass first (#16 / #17).
    EndRenderPass();
    if (active_occlusion_query_.valid && occlusion_query_pool_ != VK_NULL_HANDLE) {
      deferred_command_buffer_.CmdVkEndQuery(occlusion_query_pool_,
                                             active_occlusion_query_.host_index);
      active_occlusion_query_ = {};
    }

    pipeline_cache_->EndSubmission();

    render_target_cache_->EndSubmission();

    primitive_processor_->EndSubmission();

    shared_memory_->EndSubmission();

    uniform_buffer_pool_->FlushWrites();

    // Submit sparse binds earlier, before executing the deferred command
    // buffer, to reduce latency.
    if (!sparse_memory_binds_.empty()) {
      sparse_buffer_bind_infos_temp_.clear();
      sparse_buffer_bind_infos_temp_.reserve(sparse_buffer_binds_.size());
      for (const SparseBufferBind& sparse_buffer_bind : sparse_buffer_binds_) {
        VkSparseBufferMemoryBindInfo& sparse_buffer_bind_info =
            sparse_buffer_bind_infos_temp_.emplace_back();
        sparse_buffer_bind_info.buffer = sparse_buffer_bind.buffer;
        sparse_buffer_bind_info.bindCount = sparse_buffer_bind.bind_count;
        sparse_buffer_bind_info.pBinds =
            sparse_memory_binds_.data() + sparse_buffer_bind.bind_offset;
      }
      assert_false(semaphores_free_.empty());
      VkSemaphore bind_sparse_semaphore = semaphores_free_.back();
      VkBindSparseInfo bind_sparse_info;
      bind_sparse_info.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO;
      bind_sparse_info.pNext = nullptr;
      bind_sparse_info.waitSemaphoreCount = 0;
      bind_sparse_info.pWaitSemaphores = nullptr;
      bind_sparse_info.bufferBindCount = uint32_t(sparse_buffer_bind_infos_temp_.size());
      bind_sparse_info.pBufferBinds =
          !sparse_buffer_bind_infos_temp_.empty() ? sparse_buffer_bind_infos_temp_.data() : nullptr;
      bind_sparse_info.imageOpaqueBindCount = 0;
      bind_sparse_info.pImageOpaqueBinds = nullptr;
      bind_sparse_info.imageBindCount = 0;
      bind_sparse_info.pImageBinds = 0;
      bind_sparse_info.signalSemaphoreCount = 1;
      bind_sparse_info.pSignalSemaphores = &bind_sparse_semaphore;
      VkResult bind_sparse_result;
      {
        ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
            vulkan_device->AcquireQueue(vulkan_device->queue_family_sparse_binding(), 0);
        bind_sparse_result =
            dfn.vkQueueBindSparse(queue_acquisition.queue(), 1, &bind_sparse_info, VK_NULL_HANDLE);
      }
      if (bind_sparse_result != VK_SUCCESS) {
        REXGPU_ERROR("Failed to submit Vulkan sparse binds");
        return false;
      }
      current_submission_wait_semaphores_.push_back(bind_sparse_semaphore);
      semaphores_free_.pop_back();
      current_submission_wait_stage_masks_.push_back(sparse_bind_wait_stage_mask_);
      sparse_bind_wait_stage_mask_ = 0;
      sparse_buffer_binds_.clear();
      sparse_memory_binds_.clear();
    }

    SubmitBarriers(true);

    if (gpu_timing_current_.pool != VK_NULL_HANDLE) {
      GpuTimingEndSubmission();
    }

    assert_false(command_buffers_writable_.empty());
    CommandBuffer command_buffer = command_buffers_writable_.back();
    if (dfn.vkResetCommandPool(device, command_buffer.pool, 0) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to reset a Vulkan command pool");
      return false;
    }
    VkCommandBufferBeginInfo command_buffer_begin_info;
    command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    command_buffer_begin_info.pNext = nullptr;
    command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    command_buffer_begin_info.pInheritanceInfo = nullptr;
    if (dfn.vkBeginCommandBuffer(command_buffer.buffer, &command_buffer_begin_info) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to begin a Vulkan command buffer");
      return false;
    }
    // DS2 port: uploads hoisted to the beginning of the submission
    // (vulkan_shared_memory_hoist_uploads), before the submission's deferred commands. Every
    // submission, mid-frame ones included, goes through here.
    shared_memory_->RecordHoistedUploads(command_buffer.buffer);
    deferred_command_buffer_.Execute(command_buffer.buffer);
    if (dfn.vkEndCommandBuffer(command_buffer.buffer) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to end a Vulkan command buffer");
      return false;
    }

    VkSubmitInfo submit_info;
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.pNext = nullptr;
    if (!current_submission_wait_semaphores_.empty()) {
      submit_info.waitSemaphoreCount = uint32_t(current_submission_wait_semaphores_.size());
      submit_info.pWaitSemaphores = current_submission_wait_semaphores_.data();
      submit_info.pWaitDstStageMask = current_submission_wait_stage_masks_.data();
    } else {
      submit_info.waitSemaphoreCount = 0;
      submit_info.pWaitSemaphores = nullptr;
      submit_info.pWaitDstStageMask = nullptr;
    }
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &command_buffer.buffer;
    submit_info.signalSemaphoreCount = 0;
    submit_info.pSignalSemaphores = nullptr;
    assert_false(fences_free_.empty());
    VkFence fence = fences_free_.back();
    if (dfn.vkResetFences(device, 1, &fence) != VK_SUCCESS) {
      REXGPU_ERROR("Failed to reset a Vulkan submission fence");
      return false;
    }
    VkResult submit_result;
    {
      ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
          vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
      CpScopedTimer submit_timer(cp_frame_.submit_ns);
      submit_result = dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit_info, fence);
    }
    if (submit_result != VK_SUCCESS) {
      REXGPU_ERROR("Failed to submit a Vulkan command buffer");
      if (submit_result == VK_ERROR_DEVICE_LOST && !device_lost_) {
        device_lost_ = true;
        if (graphics_system_) {
          graphics_system_->OnHostGpuLossFromAnyThread(true);
        }
      }
      return false;
    }
    uint64_t submission_current = GetCurrentSubmission();
    current_submission_wait_stage_masks_.clear();
    for (VkSemaphore semaphore : current_submission_wait_semaphores_) {
      submissions_in_flight_semaphores_.emplace_back(submission_current, semaphore);
    }
    current_submission_wait_semaphores_.clear();
    command_buffers_submitted_.emplace_back(submission_current, command_buffer);
    command_buffers_writable_.pop_back();
    ++gpu_timing_counters_.submissions;
    if (gpu_timing_current_.pool != VK_NULL_HANDLE) {
      gpu_timing_submitted_.emplace_back(submission_current, std::move(gpu_timing_current_));
      gpu_timing_current_ = GpuTimingSubmission();
    }
    // Increments the current submission number, going to the next submission.
    submissions_in_flight_fences_.push_back(fence);
    fences_free_.pop_back();

    submission_open_ = false;
  }

  if (is_closing_frame) {
    if (REXCVAR_GET(clear_memory_page_state) && shared_memory_) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }
    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kMaxFramesInFlight] = GetCurrentSubmission() - 1;

    if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      DestroyScratchBuffer();

      for (SwapFramebuffer& swap_framebuffer : swap_framebuffers_) {
        ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyFramebuffer, device,
                                               swap_framebuffer.framebuffer);
      }

      assert_true(command_buffers_submitted_.empty());
      for (const CommandBuffer& command_buffer : command_buffers_writable_) {
        dfn.vkDestroyCommandPool(device, command_buffer.pool, nullptr);
      }
      command_buffers_writable_.clear();

      ClearTransientDescriptorPools();

      uniform_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      render_target_cache_->ClearCache();

      // Not clearing the pipeline layouts and the descriptor set layouts as
      // they're referenced by pipelines, which are not destroyed.

      primitive_processor_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

void VulkanCommandProcessor::ClearTransientDescriptorPools() {
  texture_transient_descriptor_sets_free_.clear();
  texture_transient_descriptor_sets_used_.clear();
  transient_descriptor_allocator_textures_.Reset();

  constants_transient_descriptors_free_.clear();
  constants_transient_descriptors_used_.clear();
  // The uniform buffer pool pages the sets refer to are destroyed along with this (ClearCache, shutdown).
  constants_dynamic_descriptor_sets_.clear();
  transient_descriptor_allocator_uniform_buffer_dynamic_.Reset();
  for (std::vector<VkDescriptorSet>& transient_descriptors_free :
       single_transient_descriptors_free_) {
    transient_descriptors_free.clear();
  }
  single_transient_descriptors_used_.clear();
  transient_descriptor_allocator_storage_buffer_.Reset();
  transient_descriptor_allocator_uniform_buffer_.Reset();
}

void VulkanCommandProcessor::SplitPendingBarrier() {
  size_t pending_buffer_memory_barrier_count = pending_barriers_buffer_memory_barriers_.size();
  size_t pending_image_memory_barrier_count = pending_barriers_image_memory_barriers_.size();
  if (!current_pending_barrier_.src_stage_mask && !current_pending_barrier_.dst_stage_mask &&
      current_pending_barrier_.buffer_memory_barriers_offset >=
          pending_buffer_memory_barrier_count &&
      current_pending_barrier_.image_memory_barriers_offset >= pending_image_memory_barrier_count) {
    return;
  }
  pending_barriers_.emplace_back(current_pending_barrier_);
  current_pending_barrier_.src_stage_mask = 0;
  current_pending_barrier_.dst_stage_mask = 0;
  current_pending_barrier_.buffer_memory_barriers_offset = pending_buffer_memory_barrier_count;
  current_pending_barrier_.image_memory_barriers_offset = pending_image_memory_barrier_count;
}

void VulkanCommandProcessor::DestroyScratchBuffer() {
  assert_false(scratch_buffer_used_);

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  scratch_buffer_last_usage_submission_ = 0;
  scratch_buffer_last_access_mask_ = 0;
  scratch_buffer_last_stage_mask_ = 0;
  scratch_buffer_size_ = 0;
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyBuffer, device, scratch_buffer_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, scratch_buffer_memory_);
}

bool VulkanCommandProcessor::EnsureSwapFxaaSourceImage(uint32_t width, uint32_t height) {
  assert_true(submission_open_);
  if (!width || !height) {
    return false;
  }
  if (swap_fxaa_source_image_ != VK_NULL_HANDLE && swap_fxaa_source_image_width_ == width &&
      swap_fxaa_source_image_height_ == height) {
    return true;
  }

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  if (swap_fxaa_source_image_ != VK_NULL_HANDLE) {
    const uint64_t destroy_submission = swap_fxaa_source_image_submission_;
    const uint64_t deferred_destroy_submission = GetCurrentSubmission();
    if (submission_completed_ >= destroy_submission) {
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device,
                                             swap_fxaa_source_image_view_);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, swap_fxaa_source_image_);
      ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                             swap_fxaa_source_image_memory_);
    } else {
      if (swap_fxaa_source_image_view_ != VK_NULL_HANDLE) {
        destroy_image_views_.emplace_back(deferred_destroy_submission,
                                          swap_fxaa_source_image_view_);
        swap_fxaa_source_image_view_ = VK_NULL_HANDLE;
      }
      if (swap_fxaa_source_image_ != VK_NULL_HANDLE) {
        destroy_images_.emplace_back(deferred_destroy_submission, swap_fxaa_source_image_);
        swap_fxaa_source_image_ = VK_NULL_HANDLE;
      }
      if (swap_fxaa_source_image_memory_ != VK_NULL_HANDLE) {
        destroy_memory_.emplace_back(deferred_destroy_submission, swap_fxaa_source_image_memory_);
        swap_fxaa_source_image_memory_ = VK_NULL_HANDLE;
      }
    }
  }
  swap_fxaa_source_image_width_ = 0;
  swap_fxaa_source_image_height_ = 0;
  swap_fxaa_source_image_submission_ = 0;
  swap_fxaa_source_stage_mask_ = 0;
  swap_fxaa_source_access_mask_ = 0;
  swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImageCreateInfo image_create_info;
  image_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_create_info.pNext = nullptr;
  image_create_info.flags = 0;
  image_create_info.imageType = VK_IMAGE_TYPE_2D;
  image_create_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
  image_create_info.extent.width = width;
  image_create_info.extent.height = height;
  image_create_info.extent.depth = 1;
  image_create_info.mipLevels = 1;
  image_create_info.arrayLayers = 1;
  image_create_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_create_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
  image_create_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_create_info.queueFamilyIndexCount = 0;
  image_create_info.pQueueFamilyIndices = nullptr;
  image_create_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!ui::vulkan::util::CreateDedicatedAllocationImage(
          vulkan_device, image_create_info, ui::vulkan::util::MemoryPurpose::kDeviceLocal,
          swap_fxaa_source_image_, swap_fxaa_source_image_memory_)) {
    REXGPU_ERROR("Failed to create the FXAA source image");
    return false;
  }

  VkImageViewCreateInfo image_view_create_info;
  image_view_create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  image_view_create_info.pNext = nullptr;
  image_view_create_info.flags = 0;
  image_view_create_info.image = swap_fxaa_source_image_;
  image_view_create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  image_view_create_info.format = image_create_info.format;
  image_view_create_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
  image_view_create_info.subresourceRange = ui::vulkan::util::InitializeSubresourceRange();
  if (dfn.vkCreateImageView(device, &image_view_create_info, nullptr,
                            &swap_fxaa_source_image_view_) != VK_SUCCESS) {
    REXGPU_ERROR("Failed to create the FXAA source image view");
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, swap_fxaa_source_image_);
    ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device,
                                           swap_fxaa_source_image_memory_);
    return false;
  }

  swap_fxaa_source_image_width_ = width;
  swap_fxaa_source_image_height_ = height;
  swap_fxaa_source_image_submission_ = 0;
  swap_fxaa_source_stage_mask_ = 0;
  swap_fxaa_source_access_mask_ = 0;
  swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  return true;
}

void VulkanCommandProcessor::DestroySwapFxaaSourceImage() {
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImageView, device,
                                         swap_fxaa_source_image_view_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkDestroyImage, device, swap_fxaa_source_image_);
  ui::vulkan::util::DestroyAndNullHandle(dfn.vkFreeMemory, device, swap_fxaa_source_image_memory_);
  swap_fxaa_source_image_width_ = 0;
  swap_fxaa_source_image_height_ = 0;
  swap_fxaa_source_image_submission_ = 0;
  swap_fxaa_source_stage_mask_ = 0;
  swap_fxaa_source_access_mask_ = 0;
  swap_fxaa_source_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanCommandProcessor::UpdateDynamicState(const draw_util::ViewportInfo& viewport_info,
                                                bool primitive_polygonal,
                                                reg::RB_DEPTHCONTROL normalized_depth_control) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  uint32_t draw_resolution_scale_x =
      (texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1) << draw_samples_as_pixels_shift_;
  uint32_t draw_resolution_scale_y =
      (texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1) << draw_samples_as_pixels_shift_;

  // Window parameters.
  // http://ftp.tku.edu.tw/NetBSD/NetBSD-current/xsrc/external/mit/xf86-video-ati/dist/src/r600_reg_auto_r6xx.h
  // See r200UpdateWindow:
  // https://github.com/freedreno/mesa/blob/master/src/mesa/drivers/dri/r200/r200_state.c
  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();

  // Viewport.
  VkViewport viewport;
  if (viewport_info.xy_extent[0] && viewport_info.xy_extent[1]) {
    viewport.x = float(viewport_info.xy_offset[0]);
    viewport.y = float(viewport_info.xy_offset[1]);
    viewport.width = float(viewport_info.xy_extent[0]);
    viewport.height = float(viewport_info.xy_extent[1]);
  } else {
    // Vulkan viewport width must be greater than 0.0f, but the Xenia  viewport
    // may be empty for various reasons - set the viewport to outside the
    // framebuffer.
    viewport.x = -1.0f;
    viewport.y = -1.0f;
    viewport.width = 1.0f;
    viewport.height = 1.0f;
  }
  viewport.minDepth = viewport_info.z_min;
  viewport.maxDepth = viewport_info.z_max;
  SetViewport(viewport);

  // Scissor.
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;
  VkRect2D scissor_rect;
  scissor_rect.offset.x = int32_t(scissor.offset[0]);
  scissor_rect.offset.y = int32_t(scissor.offset[1]);
  scissor_rect.extent.width = scissor.extent[0];
  scissor_rect.extent.height = scissor.extent[1];
  if (debug_zero_scissor_) {
    scissor_rect.extent.width = 0;
    scissor_rect.extent.height = 0;
  }
  SetScissor(scissor_rect);

  if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    // Depth bias.
    float depth_bias_constant_factor, depth_bias_slope_factor;
    draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, depth_bias_slope_factor,
                                             depth_bias_constant_factor);
    depth_bias_constant_factor *=
        regs.Get<reg::RB_DEPTH_INFO>().depth_format == xenos::DepthRenderTargetFormat::kD24S8
            ? draw_util::kD3D10PolygonOffsetFactorUnorm24
            : draw_util::kD3D10PolygonOffsetFactorFloat24;
    if (float24_depth_bias_in_vertex_shader_) {
      // gpu_float24_absolute_depth_bias: in ndc_offset.z instead.
      depth_bias_constant_factor = 0.0f;
    }
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    depth_bias_slope_factor *= xenos::kPolygonOffsetScaleSubpixelUnit *
                               float(std::max(draw_resolution_scale_x, draw_resolution_scale_y));
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    dynamic_depth_bias_update_needed_ |=
        std::memcmp(&dynamic_depth_bias_constant_factor_, &depth_bias_constant_factor,
                    sizeof(float)) != 0;
    dynamic_depth_bias_update_needed_ |= std::memcmp(&dynamic_depth_bias_slope_factor_,
                                                     &depth_bias_slope_factor, sizeof(float)) != 0;
    if (dynamic_depth_bias_update_needed_) {
      dynamic_depth_bias_constant_factor_ = depth_bias_constant_factor;
      dynamic_depth_bias_slope_factor_ = depth_bias_slope_factor;
      deferred_command_buffer_.CmdVkSetDepthBias(dynamic_depth_bias_constant_factor_, 0.0f,
                                                 dynamic_depth_bias_slope_factor_);
      dynamic_depth_bias_update_needed_ = false;
    }

    // Blend constants.
    float blend_constants[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    dynamic_blend_constants_update_needed_ |=
        std::memcmp(dynamic_blend_constants_, blend_constants, sizeof(float) * 4) != 0;
    if (dynamic_blend_constants_update_needed_) {
      std::memcpy(dynamic_blend_constants_, blend_constants, sizeof(float) * 4);
      deferred_command_buffer_.CmdVkSetBlendConstants(dynamic_blend_constants_);
      dynamic_blend_constants_update_needed_ = false;
    }

    // Stencil masks and references.
    // Due to pretty complex conditions involving registers not directly related
    // to stencil (primitive type, culling), changing the values only when
    // stencil is actually needed. However, due to the way dynamic state needs
    // to be set in Vulkan, which doesn't take into account whether the state
    // actually has effect on drawing, and because the masks and the references
    // are always dynamic in Xenia guest pipelines, they must be set in the
    // command buffer before any draw.
    if (normalized_depth_control.stencil_enable) {
      Register stencil_ref_mask_front_reg, stencil_ref_mask_back_reg;
      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        if (GetVulkanDevice()->properties().separateStencilMaskRef) {
          stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
        } else {
          // Choose the back face values only if drawing only back faces.
          stencil_ref_mask_front_reg = regs.Get<reg::PA_SU_SC_MODE_CNTL>().cull_front
                                           ? XE_GPU_REG_RB_STENCILREFMASK_BF
                                           : XE_GPU_REG_RB_STENCILREFMASK;
          stencil_ref_mask_back_reg = stencil_ref_mask_front_reg;
        }
      } else {
        stencil_ref_mask_front_reg = XE_GPU_REG_RB_STENCILREFMASK;
        stencil_ref_mask_back_reg = XE_GPU_REG_RB_STENCILREFMASK;
      }
      auto stencil_ref_mask_front = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_front_reg);
      auto stencil_ref_mask_back = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_back_reg);
      // Compare mask.
      dynamic_stencil_compare_mask_front_update_needed_ |=
          dynamic_stencil_compare_mask_front_ != stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_front_ = stencil_ref_mask_front.stencilmask;
      dynamic_stencil_compare_mask_back_update_needed_ |=
          dynamic_stencil_compare_mask_back_ != stencil_ref_mask_back.stencilmask;
      dynamic_stencil_compare_mask_back_ = stencil_ref_mask_back.stencilmask;
      // Write mask.
      dynamic_stencil_write_mask_front_update_needed_ |=
          dynamic_stencil_write_mask_front_ != stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_front_ = stencil_ref_mask_front.stencilwritemask;
      dynamic_stencil_write_mask_back_update_needed_ |=
          dynamic_stencil_write_mask_back_ != stencil_ref_mask_back.stencilwritemask;
      dynamic_stencil_write_mask_back_ = stencil_ref_mask_back.stencilwritemask;
      // Reference.
      dynamic_stencil_reference_front_update_needed_ |=
          dynamic_stencil_reference_front_ != stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_front_ = stencil_ref_mask_front.stencilref;
      dynamic_stencil_reference_back_update_needed_ |=
          dynamic_stencil_reference_back_ != stencil_ref_mask_back.stencilref;
      dynamic_stencil_reference_back_ = stencil_ref_mask_back.stencilref;
    }
    // Using VK_STENCIL_FACE_FRONT_AND_BACK for higher safety when running on
    // the Vulkan portability subset without separateStencilMaskRef.
    if (dynamic_stencil_compare_mask_front_update_needed_ ||
        dynamic_stencil_compare_mask_back_update_needed_) {
      if (dynamic_stencil_compare_mask_front_ == dynamic_stencil_compare_mask_back_) {
        deferred_command_buffer_.CmdVkSetStencilCompareMask(VK_STENCIL_FACE_FRONT_AND_BACK,
                                                            dynamic_stencil_compare_mask_front_);
      } else {
        if (dynamic_stencil_compare_mask_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilCompareMask(VK_STENCIL_FACE_FRONT_BIT,
                                                              dynamic_stencil_compare_mask_front_);
        }
        if (dynamic_stencil_compare_mask_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilCompareMask(VK_STENCIL_FACE_BACK_BIT,
                                                              dynamic_stencil_compare_mask_back_);
        }
      }
      dynamic_stencil_compare_mask_front_update_needed_ = false;
      dynamic_stencil_compare_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_write_mask_front_update_needed_ ||
        dynamic_stencil_write_mask_back_update_needed_) {
      if (dynamic_stencil_write_mask_front_ == dynamic_stencil_write_mask_back_) {
        deferred_command_buffer_.CmdVkSetStencilWriteMask(VK_STENCIL_FACE_FRONT_AND_BACK,
                                                          dynamic_stencil_write_mask_front_);
      } else {
        if (dynamic_stencil_write_mask_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilWriteMask(VK_STENCIL_FACE_FRONT_BIT,
                                                            dynamic_stencil_write_mask_front_);
        }
        if (dynamic_stencil_write_mask_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilWriteMask(VK_STENCIL_FACE_BACK_BIT,
                                                            dynamic_stencil_write_mask_back_);
        }
      }
      dynamic_stencil_write_mask_front_update_needed_ = false;
      dynamic_stencil_write_mask_back_update_needed_ = false;
    }
    if (dynamic_stencil_reference_front_update_needed_ ||
        dynamic_stencil_reference_back_update_needed_) {
      if (dynamic_stencil_reference_front_ == dynamic_stencil_reference_back_) {
        deferred_command_buffer_.CmdVkSetStencilReference(VK_STENCIL_FACE_FRONT_AND_BACK,
                                                          dynamic_stencil_reference_front_);
      } else {
        if (dynamic_stencil_reference_front_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilReference(VK_STENCIL_FACE_FRONT_BIT,
                                                            dynamic_stencil_reference_front_);
        }
        if (dynamic_stencil_reference_back_update_needed_) {
          deferred_command_buffer_.CmdVkSetStencilReference(VK_STENCIL_FACE_BACK_BIT,
                                                            dynamic_stencil_reference_back_);
        }
      }
      dynamic_stencil_reference_front_update_needed_ = false;
      dynamic_stencil_reference_back_update_needed_ = false;
    }
  }

  // TODO(Triang3l): VK_EXT_extended_dynamic_state and
  // VK_EXT_extended_dynamic_state2.
}

void VulkanCommandProcessor::UpdateSystemConstantValues(
    bool primitive_polygonal,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    bool shader_32bit_index_dma, uint32_t compute_memexport_vertex_count,
    const draw_util::ViewportInfo& viewport_info, uint32_t used_texture_mask,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_stencilrefmask = regs.Get<reg::RB_STENCILREFMASK>();
  auto rb_stencilrefmask_bf = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto vgt_dma_size = regs.Get<reg::VGT_DMA_SIZE>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  auto vgt_indx_offset = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  auto vgt_max_vtx_indx = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);
  auto vgt_min_vtx_indx = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);

  bool edram_fragment_shader_interlock =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  // Get the color info register values for each render target. Also, for FSI,
  // exclude components that don't exist in the format from the write mask.
  // Don't exclude fully overlapping render targets, however - two render
  // targets with the same base address are used in the lighting pass of
  // 4D5307E6, for example, with the needed one picked with dynamic control
  // flow.
  reg::RB_COLOR_INFO color_infos[xenos::kMaxColorRenderTargets];
  float rt_clamp[4][4];
  // Two UINT32_MAX if no components actually existing in the RT are written.
  uint32_t rt_keep_masks[4][2];
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    color_infos[i] = color_info;
    if (edram_fragment_shader_interlock) {
      RenderTargetCache::GetPSIColorFormatInfo(
          color_info.color_format, (normalized_color_mask >> (i * 4)) & 0b1111, rt_clamp[i][0],
          rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3], rt_keep_masks[i][0], rt_keep_masks[i][1]);
    }
  }

  // Disable depth and stencil if it aliases a color render target (for
  // instance, during the XBLA logo in 58410954, though depth writing is already
  // disabled there).
  bool depth_stencil_enabled =
      normalized_depth_control.stencil_enable || normalized_depth_control.z_enable;
  if (edram_fragment_shader_interlock && depth_stencil_enabled) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (rb_depth_info.depth_base == color_infos[i].color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
        break;
      }
    }
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  // Vertex index shader loading.
  if (shader_32bit_index_dma) {
    flags |= SpirvShaderTranslator::kSysFlag_VertexIndexLoad;
  }
  if (primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA ||
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA ||
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad;
    if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32) {
      flags |= SpirvShaderTranslator ::kSysFlag_ComputeOrPrimitiveVertexIndexLoad32Bit;
    }
    // Point and rectangle expansion uses primitive restart only in the host
    // built-in strip index buffer; guest indices are not reset-based there.
    bool compute_or_primitive_vertex_index_reset =
        primitive_processing_result.host_primitive_reset_enabled &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kPointListAsTriangleStrip &&
        primitive_processing_result.host_vertex_shader_type !=
            Shader::HostVertexShaderType::kRectangleListAsTriangleStrip;
    if (compute_or_primitive_vertex_index_reset) {
      flags |= SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexReset;
    }
  }
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kTriangleDomainPatchIndexed) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeMemExportPatchIndexInRegister1;
  }
  if (primitive_processing_result.host_vertex_shader_type ==
      Shader::HostVertexShaderType::kTriangleDomainCPIndexed) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeMemExportTriangleCPIndexed;
  } else if (primitive_processing_result.host_vertex_shader_type ==
             Shader::HostVertexShaderType::kQuadDomainCPIndexed) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeMemExportQuadCPIndexed;
  }
  if (primitive_processing_result.tessellation_mode == xenos::TessellationMode::kAdaptive &&
      (primitive_processing_result.host_vertex_shader_type ==
           Shader::HostVertexShaderType::kTriangleDomainPatchIndexed ||
       primitive_processing_result.host_vertex_shader_type ==
           Shader::HostVertexShaderType::kQuadDomainPatchIndexed)) {
    flags |= SpirvShaderTranslator::kSysFlag_ComputeMemExportPatchIndexFromInvocation;
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= SpirvShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // Whether the primitive is polygonal, and gl_FrontFacing matters.
  if (primitive_polygonal) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive type.
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= SpirvShaderTranslator::kSysFlag_PrimitiveLine;
  }
  // MSAA sample count.
  flags |= uint32_t(rb_surface_info.msaa_samples)
           << SpirvShaderTranslator::kSysFlag_MsaaSamples_Shift;
  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= SpirvShaderTranslator::kSysFlag_DepthFloat24;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << SpirvShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing.
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    // Keep parity with D3D12: gamma targets in this path are converted via
    // explicit Xenos PWL gamma logic in shaders rather than via host sRGB
    // attachment conversion semantics.
    for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
      if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= SpirvShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  if (edram_fragment_shader_interlock && depth_stencil_enabled) {
    flags |= SpirvShaderTranslator::kSysFlag_FSIDepthStencil;
    if (normalized_depth_control.z_enable) {
      flags |= uint32_t(normalized_depth_control.zfunc)
               << SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess_Shift;
      if (normalized_depth_control.z_write_enable) {
        flags |= SpirvShaderTranslator::kSysFlag_FSIDepthWrite;
      }
    } else {
      // In case stencil is used without depth testing - always pass, and
      // don't modify the stored depth.
      flags |= SpirvShaderTranslator::kSysFlag_FSIDepthPassIfLess |
               SpirvShaderTranslator::kSysFlag_FSIDepthPassIfEqual |
               SpirvShaderTranslator::kSysFlag_FSIDepthPassIfGreater;
    }
    if (normalized_depth_control.stencil_enable) {
      flags |= SpirvShaderTranslator::kSysFlag_FSIStencilTest;
    }
    // Hint - if not applicable to the shader, will not have effect.
    if (alpha_test_function == xenos::CompareFunction::kAlways &&
        !rb_colorcontrol.alpha_to_mask_enable) {
      flags |= SpirvShaderTranslator::kSysFlag_FSIDepthStencilEarlyWrite;
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Index buffer address for loading in the shaders.
  if (flags & (SpirvShaderTranslator::kSysFlag_VertexIndexLoad |
               SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)) {
    dirty |=
        system_constants_.vertex_index_load_address != primitive_processing_result.guest_index_base;
    system_constants_.vertex_index_load_address = primitive_processing_result.guest_index_base;
  }

  // Primitive reset index for shader-side index loading.
  uint32_t vertex_index_reset =
      (flags & SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexReset)
          ? regs.Get<reg::VGT_MULTI_PRIM_IB_RESET_INDX>().reset_indx
          : 0;
  dirty |= system_constants_.vertex_index_reset != vertex_index_reset;
  system_constants_.vertex_index_reset = vertex_index_reset;
  dirty |= system_constants_.compute_memexport_vertex_count != compute_memexport_vertex_count;
  system_constants_.compute_memexport_vertex_count = compute_memexport_vertex_count;

  // Index or tessellation edge factor buffer endianness.
  xenos::Endian guest_index_endian = vgt_dma_size.swap_mode;
  if (vgt_draw_initiator.index_size == xenos::IndexFormat::kInt16 &&
      guest_index_endian != xenos::Endian::kNone && guest_index_endian != xenos::Endian::k8in16) {
    guest_index_endian =
        guest_index_endian == xenos::Endian::k8in32 ? xenos::Endian::k8in16 : xenos::Endian::kNone;
  }
  xenos::Endian index_endian =
      (flags & SpirvShaderTranslator::kSysFlag_ComputeOrPrimitiveVertexIndexLoad)
          ? guest_index_endian
          : primitive_processing_result.host_shader_index_endian;
  dirty |= system_constants_.vertex_index_endian != index_endian;
  system_constants_.vertex_index_endian = index_endian;

  dirty |= system_constants_.line_loop_closing_index !=
           primitive_processing_result.line_loop_closing_index;
  system_constants_.line_loop_closing_index = primitive_processing_result.line_loop_closing_index;

  // Vertex index offset.
  dirty |= system_constants_.vertex_base_index != vgt_indx_offset;
  system_constants_.vertex_base_index = vgt_indx_offset;

  // Vertex index range.
  dirty |= system_constants_.vertex_index_min != vgt_min_vtx_indx;
  dirty |= system_constants_.vertex_index_max != vgt_max_vtx_indx;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  // User clip planes (UCP_ENA_#), when not CLIP_DISABLE.
  // The shader knows only the total count - tightly packing the user clip
  // planes that are actually used.
  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  // Tessellation factor range, plus 1.0 according to
  // https://www.slideshare.net/blackdevilvikas/next-generation-graphics-programming-on-xbox-360.
  float tessellation_factor_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tessellation_factor_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  dirty |= system_constants_.tessellation_factor_range_min != tessellation_factor_min;
  dirty |= system_constants_.tessellation_factor_range_max != tessellation_factor_max;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  // Conversion to host normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x = float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y = float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min != point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max != point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] != point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] != point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader.
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_x << draw_samples_as_pixels_shift_)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_y << draw_samples_as_pixels_shift_)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / gamma.
  uint32_t textures_resolution_scaled = 0;
  {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
      uint32_t texture_signs_shift = 8 * (texture_index & 3);
      uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
      uint32_t texture_signs_shifted = uint32_t(texture_signs) << texture_signs_shift;
      uint32_t texture_signs_mask = ((UINT32_C(1) << 8) - 1) << texture_signs_shift;
      dirty |= (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
      texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
      textures_resolution_scaled |=
          uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index)) << texture_index;
    }
  }
  dirty |= system_constants_.textures_resolution_scaled != textures_resolution_scaled;
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;

  // Texture host swizzle in the shader.
  if (!GetVulkanDevice()->properties().imageViewFormatSwizzle) {
    uint32_t textures_remaining = used_texture_mask;
    uint32_t texture_index;
    while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
      textures_remaining &= ~(UINT32_C(1) << texture_index);
      uint32_t& texture_swizzles_uint = system_constants_.texture_swizzles[texture_index >> 1];
      uint32_t texture_swizzle_shift = 12 * (texture_index & 1);
      uint32_t texture_swizzle = texture_cache_->GetActiveTextureHostSwizzle(texture_index);
      uint32_t texture_swizzle_shifted = uint32_t(texture_swizzle) << texture_swizzle_shift;
      uint32_t texture_swizzle_mask = ((UINT32_C(1) << 12) - 1) << texture_swizzle_shift;
      dirty |= (texture_swizzles_uint & texture_swizzle_mask) != texture_swizzle_shifted;
      texture_swizzles_uint =
          (texture_swizzles_uint & ~texture_swizzle_mask) | texture_swizzle_shifted;
    }
  }

  // Alpha test.
  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;
  uint32_t alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (UINT32_C(1) << 8) : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  uint32_t edram_tile_dwords_scaled = xenos::kEdramTileWidthSamples *
                                      xenos::kEdramTileHeightSamples *
                                      (draw_resolution_scale_x * draw_resolution_scale_y);

  // EDRAM pitch for FSI render target writing.
  if (edram_fragment_shader_interlock) {
    // Align, then multiply by 32bpp tile size in dwords.
    uint32_t edram_32bpp_tile_pitch_dwords_scaled =
        ((rb_surface_info.surface_pitch *
          (rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1)) +
         (xenos::kEdramTileWidthSamples - 1)) /
        xenos::kEdramTileWidthSamples * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_32bpp_tile_pitch_dwords_scaled !=
             edram_32bpp_tile_pitch_dwords_scaled;
    system_constants_.edram_32bpp_tile_pitch_dwords_scaled = edram_32bpp_tile_pitch_dwords_scaled;
  }

  // Color exponent bias and FSI render target writing.
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets &&
        (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 &&
             !render_target_cache_->IsFixedRG16TruncatedToMinus1To1() ||
         color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16 &&
             !render_target_cache_->IsFixedRGBA16TruncatedToMinus1To1())) {
      // Remap from -32...32 to -1...1 by dividing the output values by 32,
      // losing blending correctness, but getting the full range.
      color_exp_bias -= 5;
    }
    float color_exp_bias_scale;
    *reinterpret_cast<int32_t*>(&color_exp_bias_scale) =
        UINT32_C(0x3F800000) + (color_exp_bias << 23);
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
    if (edram_fragment_shader_interlock) {
      dirty |= system_constants_.edram_rt_keep_mask[i][0] != rt_keep_masks[i][0];
      system_constants_.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
      dirty |= system_constants_.edram_rt_keep_mask[i][1] != rt_keep_masks[i][1];
      system_constants_.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
      if (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX) {
        uint32_t rt_base_dwords_scaled = color_info.color_base * edram_tile_dwords_scaled;
        dirty |= system_constants_.edram_rt_base_dwords_scaled[i] != rt_base_dwords_scaled;
        system_constants_.edram_rt_base_dwords_scaled[i] = rt_base_dwords_scaled;
        uint32_t format_flags = RenderTargetCache::AddPSIColorFormatFlags(color_info.color_format);
        dirty |= system_constants_.edram_rt_format_flags[i] != format_flags;
        system_constants_.edram_rt_format_flags[i] = format_flags;
        uint32_t blend_factors_ops =
            regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
        dirty |= system_constants_.edram_rt_blend_factors_ops[i] != blend_factors_ops;
        system_constants_.edram_rt_blend_factors_ops[i] = blend_factors_ops;
        // Can't do float comparisons here because NaNs would result in always
        // setting the dirty flag.
        dirty |=
            std::memcmp(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float)) != 0;
        std::memcpy(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float));
      }
    }
  }

  if (edram_fragment_shader_interlock) {
    uint32_t depth_base_dwords_scaled = rb_depth_info.depth_base * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_depth_base_dwords_scaled != depth_base_dwords_scaled;
    system_constants_.edram_depth_base_dwords_scaled = depth_base_dwords_scaled;

    // For non-polygons, front polygon offset is used, and it's enabled if
    // POLY_OFFSET_PARA_ENABLED is set, for polygons, separate front and back
    // are used.
    float poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
    float poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
    if (primitive_polygonal) {
      if (pa_su_sc_mode_cntl.poly_offset_front_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      if (pa_su_sc_mode_cntl.poly_offset_back_enable) {
        poly_offset_back_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
        poly_offset_back_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      }
    } else {
      if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
        poly_offset_back_scale = poly_offset_front_scale;
        poly_offset_back_offset = poly_offset_front_offset;
      }
    }
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    float poly_offset_scale_factor = xenos::kPolygonOffsetScaleSubpixelUnit *
                                     (std::max(draw_resolution_scale_x, draw_resolution_scale_y)
                                      << draw_samples_as_pixels_shift_);
    poly_offset_front_scale *= poly_offset_scale_factor;
    poly_offset_back_scale *= poly_offset_scale_factor;
    dirty |= system_constants_.edram_poly_offset_front_scale != poly_offset_front_scale;
    system_constants_.edram_poly_offset_front_scale = poly_offset_front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset != poly_offset_front_offset;
    system_constants_.edram_poly_offset_front_offset = poly_offset_front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale != poly_offset_back_scale;
    system_constants_.edram_poly_offset_back_scale = poly_offset_back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset != poly_offset_back_offset;
    system_constants_.edram_poly_offset_back_offset = poly_offset_back_offset;

    if (depth_stencil_enabled && normalized_depth_control.stencil_enable) {
      uint32_t stencil_front_reference_masks = rb_stencilrefmask.value & 0xFFFFFF;
      dirty |=
          system_constants_.edram_stencil_front_reference_masks != stencil_front_reference_masks;
      system_constants_.edram_stencil_front_reference_masks = stencil_front_reference_masks;
      uint32_t stencil_func_ops = (normalized_depth_control.value >> 8) & ((1 << 12) - 1);
      dirty |= system_constants_.edram_stencil_front_func_ops != stencil_func_ops;
      system_constants_.edram_stencil_front_func_ops = stencil_func_ops;

      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        uint32_t stencil_back_reference_masks = rb_stencilrefmask_bf.value & 0xFFFFFF;
        dirty |=
            system_constants_.edram_stencil_back_reference_masks != stencil_back_reference_masks;
        system_constants_.edram_stencil_back_reference_masks = stencil_back_reference_masks;
        uint32_t stencil_func_ops_bf = (normalized_depth_control.value >> 20) & ((1 << 12) - 1);
        dirty |= system_constants_.edram_stencil_back_func_ops != stencil_func_ops_bf;
        system_constants_.edram_stencil_back_func_ops = stencil_func_ops_bf;
      } else {
        dirty |= std::memcmp(system_constants_.edram_stencil_back,
                             system_constants_.edram_stencil_front, 2 * sizeof(uint32_t)) != 0;
        std::memcpy(system_constants_.edram_stencil_back, system_constants_.edram_stencil_front,
                    2 * sizeof(uint32_t));
      }
    }

    dirty |= system_constants_.edram_blend_constant[0] != regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    system_constants_.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    dirty |=
        system_constants_.edram_blend_constant[1] != regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    system_constants_.edram_blend_constant[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    dirty |= system_constants_.edram_blend_constant[2] != regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    system_constants_.edram_blend_constant[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    dirty |=
        system_constants_.edram_blend_constant[3] != regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
    system_constants_.edram_blend_constant[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  }

  if (dirty) {
    current_constant_buffers_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem);
  }
}

namespace {
// The float constants a shader uses (the set bits of its 4 x 64-bit map, in order), packed into a uniform buffer:
// each run of consecutive constants in one copy (one 16-byte copy per constant took ~6 % of the command thread).
void CopyUsedFloatConstants(uint8_t* mapping, const uint32_t* constants, const uint64_t map[4]) {
  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t entry = map[i];
    while (entry) {
      const uint32_t first = uint32_t(__builtin_ctzll(entry));
      const uint64_t shifted = entry >> first;
      const uint32_t count = ~shifted ? uint32_t(__builtin_ctzll(~shifted)) : 64 - first;
      std::memcpy(mapping, constants + (i << 8) + (first << 2), sizeof(float) * 4 * count);
      mapping += sizeof(float) * 4 * count;
      entry = first + count >= 64 ? 0 : entry & ~(((uint64_t(1) << count) - 1) << first);
    }
  }
}
}  // namespace

bool VulkanCommandProcessor::UpdateBindings(const VulkanShader* vertex_shader,
                                            const VulkanShader* pixel_shader) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;

  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();

  // Invalidate constant buffers and descriptors for changed data.

  // Float constants.
  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ on the Xbox
  // 360 (however, OpenGL ES on Adreno 200 on Android has different ranges).
  assert_true(regs[XE_GPU_REG_SQ_VS_CONST] == 0x000FF000 ||
              regs[XE_GPU_REG_SQ_VS_CONST] == 0x00000000);
  assert_true(regs[XE_GPU_REG_SQ_PS_CONST] == 0x000FF100 ||
              regs[XE_GPU_REG_SQ_PS_CONST] == 0x00000000);
  // Check if the float constant layout is still the same and get the counts.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] = float_constant_map_vertex.float_bitmap[i];
      // If no float constants at all, any buffer can be reused for them, so not
      // invalidating.
      if (float_constant_count_vertex) {
        current_constant_buffers_up_to_date_ &=
            ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
      }
    }
  }
  uint32_t float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] = float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          current_constant_buffers_up_to_date_ &=
              ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
  }

  // Write the new constant buffers.
  constexpr uint32_t kAllConstantBuffersMask =
      (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferCount) - 1;
  assert_zero(current_constant_buffers_up_to_date_ & ~kAllConstantBuffersMask);
  // A loop only for vulkan_dynamic_constant_offsets, where all the buffers must end up in one pool page (the
  // descriptor set is per page): one pass otherwise, up to three when a request crosses into a new page.
  uint32_t constant_buffer_upload_passes = 0;
  while ((current_constant_buffers_up_to_date_ & kAllConstantBuffersMask) !=
         kAllConstantBuffersMask) {
    if (++constant_buffer_upload_passes > 8) {
      assert_always();
      return false;
    }
    current_graphics_descriptor_set_values_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants);
    size_t uniform_buffer_alignment =
        size_t(vulkan_device->properties().minUniformBufferOffsetAlignment);
    // The pool page of the latest request in this pass.
    VkBuffer constants_last_buffer = VK_NULL_HANDLE;
    auto request_constant_buffer = [&](uint32_t constant_buffer_index, size_t size) -> uint8_t* {
      VkDescriptorBufferInfo& request_buffer_info =
          current_constant_buffer_infos_[constant_buffer_index];
      uint8_t* request_mapping = uniform_buffer_pool_->Request(
          frame_current_, size, uniform_buffer_alignment, request_buffer_info.buffer,
          request_buffer_info.offset);
      if (request_mapping && constants_dynamic_per_page_sets_) {
        // The descriptor has the fixed largest range, which must fit in the buffer at any dynamic offset: an
        // allocation too close to the end of the page is redone with the whole range (in the next page).
        size_t dynamic_range = size_t(GetDynamicConstantBufferRange(constant_buffer_index));
        if (size_t(request_buffer_info.offset) + dynamic_range > constants_dynamic_page_size_) {
          request_mapping = uniform_buffer_pool_->Request(
              frame_current_, std::max(size, dynamic_range), uniform_buffer_alignment,
              request_buffer_info.buffer, request_buffer_info.offset);
        }
        constants_last_buffer = request_buffer_info.buffer;
      }
      return request_mapping;
    };
    // System constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferSystem))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferSystem];
      uint8_t* mapping = request_constant_buffer(SpirvShaderTranslator::kConstantBufferSystem,
                                                 sizeof(SpirvShaderTranslator::SystemConstants));
      if (!mapping) {
        return false;
      }
      buffer_info.range = sizeof(SpirvShaderTranslator::SystemConstants);
      std::memcpy(mapping, &system_constants_, sizeof(SpirvShaderTranslator::SystemConstants));
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferSystem;
    }
    // Vertex shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferFloatVertex];
      // Even if the shader doesn't need any float constants, a valid binding
      // must still be provided (the pipeline layout always has float constants,
      // for both the vertex shader and the pixel shader), so if the first draw
      // in the frame doesn't have float constants at all, still allocate a
      // dummy buffer.
      size_t float_constants_size =
          sizeof(float) * 4 * std::max(float_constant_count_vertex, UINT32_C(1));
      uint8_t* mapping = request_constant_buffer(
          SpirvShaderTranslator::kConstantBufferFloatVertex, float_constants_size);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_size);
      CopyUsedFloatConstants(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_000_X], current_float_constant_map_vertex_);
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferFloatVertex;
    }
    // Pixel shader float constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferFloatPixel];
      size_t float_constants_size =
          sizeof(float) * 4 * std::max(float_constant_count_pixel, UINT32_C(1));
      uint8_t* mapping = request_constant_buffer(
          SpirvShaderTranslator::kConstantBufferFloatPixel, float_constants_size);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(float_constants_size);
      CopyUsedFloatConstants(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_256_X], current_float_constant_map_pixel_);
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferFloatPixel;
    }
    // Bool and loop constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferBoolLoop];
      constexpr size_t kBoolLoopConstantsSize = sizeof(uint32_t) * (8 + 32);
      uint8_t* mapping = request_constant_buffer(SpirvShaderTranslator::kConstantBufferBoolLoop,
                                                 kBoolLoopConstantsSize);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(kBoolLoopConstantsSize);
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], kBoolLoopConstantsSize);
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferBoolLoop;
    }
    // Fetch constants.
    if (!(current_constant_buffers_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch))) {
      VkDescriptorBufferInfo& buffer_info =
          current_constant_buffer_infos_[SpirvShaderTranslator::kConstantBufferFetch];
      constexpr size_t kFetchConstantsSize = sizeof(uint32_t) * 6 * 32;
      uint8_t* mapping = request_constant_buffer(SpirvShaderTranslator::kConstantBufferFetch,
                                                 kFetchConstantsSize);
      if (!mapping) {
        return false;
      }
      buffer_info.range = VkDeviceSize(kFetchConstantsSize);
      std::memcpy(mapping, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);
      current_constant_buffers_up_to_date_ |= UINT32_C(1)
                                              << SpirvShaderTranslator::kConstantBufferFetch;
    }
    if (constants_dynamic_per_page_sets_) {
      // Constant buffers left in an older pool page are uploaded again, next to the latest ones.
      for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
        if (current_constant_buffer_infos_[i].buffer != constants_last_buffer) {
          current_constant_buffers_up_to_date_ &= ~(UINT32_C(1) << i);
        }
      }
    }
  }

  // Textures and samplers.
  const std::vector<VulkanShader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  const std::vector<VulkanShader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  uint32_t sampler_count_vertex = uint32_t(samplers_vertex.size());
  uint32_t texture_count_vertex = uint32_t(textures_vertex.size());
  const std::vector<VulkanShader::SamplerBinding>* samplers_pixel;
  const std::vector<VulkanShader::TextureBinding>* textures_pixel;
  uint32_t sampler_count_pixel, texture_count_pixel;
  if (pixel_shader) {
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    sampler_count_pixel = uint32_t(samplers_pixel->size());
    texture_count_pixel = uint32_t(textures_pixel->size());
  } else {
    samplers_pixel = nullptr;
    textures_pixel = nullptr;
    sampler_count_pixel = 0;
    texture_count_pixel = 0;
  }
  // Fill the texture and sampler image infos of both stages: they decide whether the sets written last in this
  // frame can be reused (vulkan_reuse_texture_descriptor_sets), and are the writes otherwise.
  descriptor_write_image_info_.clear();
  descriptor_write_image_info_.reserve(texture_count_vertex + sampler_count_vertex + texture_count_pixel +
                                       sampler_count_pixel);
  size_t vertex_texture_image_info_offset = descriptor_write_image_info_.size();
  for (uint32_t i = 0; i < texture_count_vertex; ++i) {
    const VulkanShader::TextureBinding& texture_binding = textures_vertex[i];
    VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();
    descriptor_image_info.imageView = texture_cache_->GetActiveBindingOrNullImageView(
        texture_binding.fetch_constant, texture_binding.dimension, bool(texture_binding.is_signed));
    descriptor_image_info.imageLayout = descriptor_image_info.imageView != VK_NULL_HANDLE
                                            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                            : VK_IMAGE_LAYOUT_UNDEFINED;
  }
  size_t vertex_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (sampler_count_vertex) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>& sampler_pair :
         current_samplers_vertex_) {
      VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }
  size_t pixel_texture_image_info_offset = descriptor_write_image_info_.size();
  for (uint32_t i = 0; i < texture_count_pixel; ++i) {
    const VulkanShader::TextureBinding& texture_binding = (*textures_pixel)[i];
    VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();
    descriptor_image_info.imageView = texture_cache_->GetActiveBindingOrNullImageView(
        texture_binding.fetch_constant, texture_binding.dimension, bool(texture_binding.is_signed));
    descriptor_image_info.imageLayout = descriptor_image_info.imageView != VK_NULL_HANDLE
                                            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                            : VK_IMAGE_LAYOUT_UNDEFINED;
  }
  size_t pixel_sampler_image_info_offset = descriptor_write_image_info_.size();
  if (sampler_count_pixel) {
    for (const std::pair<VulkanTextureCache::SamplerParameters, VkSampler>& sampler_pair :
         current_samplers_pixel_) {
      VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();
      descriptor_image_info.sampler = sampler_pair.second;
    }
  }
  size_t image_info_end = descriptor_write_image_info_.size();

  // Upstream wrote both texture sets for every draw ("TODO(Triang3l): Reuse texture and sampler bindings if not
  // changed"). A set written earlier in this frame for the same layout and the same views and samplers is reused
  // (transient sets live until their frame completes on the GPU, so only within the frame; views and samplers in
  // use are not destroyed before that either).
  const bool reuse_texture_sets = REXCVAR_GET(vulkan_reuse_texture_descriptor_sets);
  auto texture_set_reusable = [&](uint32_t stage, uint32_t set_index, VkDescriptorSetLayout layout,
                                  uint32_t texture_count, size_t begin, size_t end) -> bool {
    const uint32_t bit = UINT32_C(1) << set_index;
    const TextureSetCache& cache = texture_set_cache_[stage];
    const size_t n = end - begin;
    if (reuse_texture_sets && cache.frame == frame_current_ && cache.set != VK_NULL_HANDLE &&
        cache.layout == layout && cache.texture_count == texture_count && cache.infos.size() == n &&
        (!n || !std::memcmp(cache.infos.data(), descriptor_write_image_info_.data() + begin,
                            n * sizeof(VkDescriptorImageInfo)))) {
      if (current_graphics_descriptor_sets_[set_index] != cache.set) {
        current_graphics_descriptor_sets_[set_index] = cache.set;
        current_graphics_descriptor_sets_bound_up_to_date_ &= ~bit;
      }
      current_graphics_descriptor_set_values_up_to_date_ |= bit;
      ++texture_set_cache_hits_;
      return true;
    }
    current_graphics_descriptor_set_values_up_to_date_ &= ~bit;
    current_graphics_descriptor_sets_bound_up_to_date_ &= ~bit;
    ++texture_set_cache_misses_;
    return false;
  };
  bool write_vertex_textures =
      (texture_count_vertex || sampler_count_vertex) &&
      !texture_set_reusable(0, SpirvShaderTranslator::kDescriptorSetTexturesVertex,
                            current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_vertex_ref(),
                            texture_count_vertex, vertex_texture_image_info_offset,
                            pixel_texture_image_info_offset);
  bool write_pixel_textures =
      (texture_count_pixel || sampler_count_pixel) &&
      !texture_set_reusable(1, SpirvShaderTranslator::kDescriptorSetTexturesPixel,
                            current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_pixel_ref(),
                            texture_count_pixel, pixel_texture_image_info_offset, image_info_end);
  if (!(texture_count_vertex || sampler_count_vertex)) {
    current_graphics_descriptor_set_values_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
  }
  if (!(texture_count_pixel || sampler_count_pixel)) {
    current_graphics_descriptor_set_values_up_to_date_ &=
        ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
  }
  {
    const uint64_t total = texture_set_cache_hits_ + texture_set_cache_misses_;
    if (total && total % 200000 < 2) {
      REXLOG_INFO("Texture descriptor sets: {} reused, {} written ({:.1f} % reused)", texture_set_cache_hits_,
                  texture_set_cache_misses_, 100.0 * double(texture_set_cache_hits_) / double(total));
    }
  }

  // Make sure new descriptor sets are bound to the command buffer.
  current_graphics_descriptor_sets_bound_up_to_date_ &=
      current_graphics_descriptor_set_values_up_to_date_;

  // Write the new descriptor sets.

  // Consecutive bindings updated via a single VkWriteDescriptorSet must have
  // identical stage flags, but for the constants they vary. Plus vertex and
  // pixel texture images and samplers.
  std::array<VkWriteDescriptorSet, SpirvShaderTranslator::kConstantBufferCount + 2 * 2>
      write_descriptor_sets;
  uint32_t write_descriptor_set_count = 0;
  uint32_t write_descriptor_set_bits = 0;
  assert_not_zero(current_graphics_descriptor_set_values_up_to_date_ &
                  (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetSharedMemoryAndEdram));
  // Constant buffers.
  if (constants_dynamic_per_page_sets_) {
    if (!(current_graphics_descriptor_set_values_up_to_date_ &
          (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants))) {
      // One descriptor set per uniform buffer pool page, written once; the draw's constants are selected by the
      // dynamic offsets in the bind below (bound_up_to_date was cleared above along with values_up_to_date).
      VkBuffer constants_buffer = current_constant_buffer_infos_[0].buffer;
      VkDescriptorSet constants_descriptor_set = VK_NULL_HANDLE;
      for (auto it = constants_dynamic_descriptor_sets_.crbegin();
           it != constants_dynamic_descriptor_sets_.crend(); ++it) {
        if (it->first == constants_buffer) {
          constants_descriptor_set = it->second;
          break;
        }
      }
      if (constants_descriptor_set == VK_NULL_HANDLE) {
        VkDescriptorPoolSize constants_descriptor_count;
        constants_descriptor_count.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        constants_descriptor_count.descriptorCount = SpirvShaderTranslator::kConstantBufferCount;
        constants_descriptor_set = transient_descriptor_allocator_uniform_buffer_dynamic_.Allocate(
            descriptor_set_layout_constants_, &constants_descriptor_count, 1);
        if (constants_descriptor_set == VK_NULL_HANDLE) {
          return false;
        }
        VkDescriptorBufferInfo dynamic_buffer_infos[SpirvShaderTranslator::kConstantBufferCount];
        VkWriteDescriptorSet dynamic_writes[SpirvShaderTranslator::kConstantBufferCount];
        for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
          dynamic_buffer_infos[i].buffer = constants_buffer;
          dynamic_buffer_infos[i].offset = 0;
          dynamic_buffer_infos[i].range = GetDynamicConstantBufferRange(i);
          VkWriteDescriptorSet& write_constants = dynamic_writes[i];
          write_constants.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          write_constants.pNext = nullptr;
          write_constants.dstSet = constants_descriptor_set;
          write_constants.dstBinding = i;
          write_constants.dstArrayElement = 0;
          write_constants.descriptorCount = 1;
          write_constants.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
          write_constants.pImageInfo = nullptr;
          write_constants.pBufferInfo = &dynamic_buffer_infos[i];
          write_constants.pTexelBufferView = nullptr;
        }
        dfn.vkUpdateDescriptorSets(device, SpirvShaderTranslator::kConstantBufferCount,
                                   dynamic_writes, 0, nullptr);
        constants_dynamic_descriptor_sets_.emplace_back(constants_buffer, constants_descriptor_set);
      }
      for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
        assert_true(current_constant_buffer_infos_[i].buffer == constants_buffer);
        current_constant_buffer_dynamic_offsets_[i] =
            uint32_t(current_constant_buffer_infos_[i].offset);
      }
      if (constants_dynamic_binds_logged_ < 8) {
        ++constants_dynamic_binds_logged_;
        REXGPU_INFO(
            "Dynamic constants bind {}: set {:#x} page buffer {:#x} offsets {} {} {} {} {} (page size {})",
            constants_dynamic_binds_logged_, uint64_t(constants_descriptor_set),
            uint64_t(constants_buffer), current_constant_buffer_dynamic_offsets_[0],
            current_constant_buffer_dynamic_offsets_[1], current_constant_buffer_dynamic_offsets_[2],
            current_constant_buffer_dynamic_offsets_[3], current_constant_buffer_dynamic_offsets_[4],
            constants_dynamic_page_size_);
      }
      write_descriptor_set_bits |= UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants;
      current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetConstants] =
          constants_descriptor_set;
    }
  } else if (!(current_graphics_descriptor_set_values_up_to_date_ &
               (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants))) {
    VkDescriptorSet constants_descriptor_set;
    if (!constants_transient_descriptors_free_.empty()) {
      constants_descriptor_set = constants_transient_descriptors_free_.back();
      constants_transient_descriptors_free_.pop_back();
    } else {
      VkDescriptorPoolSize constants_descriptor_count;
      // constants_dynamic_offsets_ here is the diagnostic per-change mode: same sets as the default path, of
      // the dynamic type.
      constants_descriptor_count.type = constants_dynamic_offsets_
                                            ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                            : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      constants_descriptor_count.descriptorCount = SpirvShaderTranslator::kConstantBufferCount;
      constants_descriptor_set =
          (constants_dynamic_offsets_ ? transient_descriptor_allocator_uniform_buffer_dynamic_
                                      : transient_descriptor_allocator_uniform_buffer_)
              .Allocate(descriptor_set_layout_constants_, &constants_descriptor_count, 1);
      if (constants_descriptor_set == VK_NULL_HANDLE) {
        return false;
      }
    }
    constants_transient_descriptors_used_.emplace_back(frame_current_, constants_descriptor_set);
    // Consecutive bindings updated via a single VkWriteDescriptorSet must have
    // identical stage flags, but for the constants they vary.
    for (uint32_t i = 0; i < SpirvShaderTranslator::kConstantBufferCount; ++i) {
      VkWriteDescriptorSet& write_constants = write_descriptor_sets[write_descriptor_set_count++];
      write_constants.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      write_constants.pNext = nullptr;
      write_constants.dstSet = constants_descriptor_set;
      write_constants.dstBinding = i;
      write_constants.dstArrayElement = 0;
      write_constants.descriptorCount = 1;
      write_constants.descriptorType = constants_dynamic_offsets_
                                           ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                           : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      write_constants.pImageInfo = nullptr;
      write_constants.pBufferInfo = &current_constant_buffer_infos_[i];
      write_constants.pTexelBufferView = nullptr;
      // Per-change diagnostic mode: the descriptor has the exact offset, the dynamic offset is zero.
      current_constant_buffer_dynamic_offsets_[i] = 0;
    }
    write_descriptor_set_bits |= UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetConstants;
    current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetConstants] =
        constants_descriptor_set;
  }
  // Vertex shader textures and samplers.
  if (write_vertex_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = WriteTransientTextureBindings(
        true, texture_count_vertex, sampler_count_vertex,
        current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_vertex_ref(),
        descriptor_write_image_info_.data() + vertex_texture_image_info_offset,
        descriptor_write_image_info_.data() + vertex_sampler_image_info_offset, write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |= UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex;
    current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetTexturesVertex] =
        write_textures[0].dstSet;
    TextureSetCache& cache = texture_set_cache_[0];
    cache.frame = frame_current_;
    cache.set = write_textures[0].dstSet;
    cache.layout = current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_vertex_ref();
    cache.texture_count = texture_count_vertex;
    cache.infos.assign(descriptor_write_image_info_.begin() + vertex_texture_image_info_offset,
                       descriptor_write_image_info_.begin() + pixel_texture_image_info_offset);
  }
  // Pixel shader textures and samplers.
  if (write_pixel_textures) {
    VkWriteDescriptorSet* write_textures =
        write_descriptor_sets.data() + write_descriptor_set_count;
    uint32_t texture_descriptor_set_write_count = WriteTransientTextureBindings(
        false, texture_count_pixel, sampler_count_pixel,
        current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_pixel_ref(),
        descriptor_write_image_info_.data() + pixel_texture_image_info_offset,
        descriptor_write_image_info_.data() + pixel_sampler_image_info_offset, write_textures);
    if (!texture_descriptor_set_write_count) {
      return false;
    }
    write_descriptor_set_count += texture_descriptor_set_write_count;
    write_descriptor_set_bits |= UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel;
    current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetTexturesPixel] =
        write_textures[0].dstSet;
    TextureSetCache& cache = texture_set_cache_[1];
    cache.frame = frame_current_;
    cache.set = write_textures[0].dstSet;
    cache.layout = current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_pixel_ref();
    cache.texture_count = texture_count_pixel;
    cache.infos.assign(descriptor_write_image_info_.begin() + pixel_texture_image_info_offset,
                       descriptor_write_image_info_.begin() + image_info_end);
  }
  // Write.
  if (write_descriptor_set_count) {
    dfn.vkUpdateDescriptorSets(device, write_descriptor_set_count, write_descriptor_sets.data(), 0,
                               nullptr);
  }
  // Only make valid if all descriptor sets have been allocated and written
  // successfully.
  current_graphics_descriptor_set_values_up_to_date_ |= write_descriptor_set_bits;

  // Bind the new descriptor sets.
  uint32_t descriptor_sets_needed = (UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetCount) - 1;
  if (!texture_count_vertex && !sampler_count_vertex) {
    descriptor_sets_needed &= ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesVertex);
  }
  if (!texture_count_pixel && !sampler_count_pixel) {
    descriptor_sets_needed &= ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTexturesPixel);
  }
  uint32_t descriptor_sets_remaining =
      descriptor_sets_needed & ~current_graphics_descriptor_sets_bound_up_to_date_;
  uint32_t descriptor_set_index;
  while (rex::bit_scan_forward(descriptor_sets_remaining, &descriptor_set_index)) {
    uint32_t descriptor_set_mask_tzcnt =
        rex::tzcnt(~(descriptor_sets_remaining | ((UINT32_C(1) << descriptor_set_index) - 1)));
    // The constants set is the only one with dynamic descriptors (one offset per binding, in binding order).
    bool bind_dynamic_constants =
        constants_dynamic_offsets_ &&
        descriptor_set_index <= uint32_t(SpirvShaderTranslator::kDescriptorSetConstants) &&
        descriptor_set_mask_tzcnt > uint32_t(SpirvShaderTranslator::kDescriptorSetConstants);
    deferred_command_buffer_.CmdVkBindDescriptorSets(
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        current_guest_graphics_pipeline_layout_->GetPipelineLayout(), descriptor_set_index,
        descriptor_set_mask_tzcnt - descriptor_set_index,
        current_graphics_descriptor_sets_ + descriptor_set_index,
        bind_dynamic_constants ? uint32_t(SpirvShaderTranslator::kConstantBufferCount) : 0,
        bind_dynamic_constants ? current_constant_buffer_dynamic_offsets_ : nullptr);
    if (descriptor_set_mask_tzcnt >= 32) {
      break;
    }
    descriptor_sets_remaining &= ~((UINT32_C(1) << descriptor_set_mask_tzcnt) - 1);
  }
  current_graphics_descriptor_sets_bound_up_to_date_ |= descriptor_sets_needed;

  return true;
}

uint32_t VulkanCommandProcessor::WriteTransientTextureBindings(
    bool is_vertex, uint32_t texture_count, uint32_t sampler_count,
    VkDescriptorSetLayout descriptor_set_layout, const VkDescriptorImageInfo* texture_image_info,
    const VkDescriptorImageInfo* sampler_image_info,
    VkWriteDescriptorSet* descriptor_set_writes_out) {
  assert_true(frame_open_);
  if (!texture_count && !sampler_count) {
    return 0;
  }
  TextureDescriptorSetLayoutKey texture_descriptor_set_layout_key;
  texture_descriptor_set_layout_key.texture_count = texture_count;
  texture_descriptor_set_layout_key.sampler_count = sampler_count;
  texture_descriptor_set_layout_key.is_vertex = uint32_t(is_vertex);
  VkDescriptorSet texture_descriptor_set;
  auto textures_free_it =
      texture_transient_descriptor_sets_free_.find(texture_descriptor_set_layout_key);
  if (textures_free_it != texture_transient_descriptor_sets_free_.end() &&
      !textures_free_it->second.empty()) {
    texture_descriptor_set = textures_free_it->second.back();
    textures_free_it->second.pop_back();
  } else {
    std::array<VkDescriptorPoolSize, 2> texture_descriptor_counts;
    uint32_t texture_descriptor_counts_count = 0;
    if (texture_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      texture_descriptor_count.descriptorCount = texture_count;
    }
    if (sampler_count) {
      VkDescriptorPoolSize& texture_descriptor_count =
          texture_descriptor_counts[texture_descriptor_counts_count++];
      texture_descriptor_count.type = VK_DESCRIPTOR_TYPE_SAMPLER;
      texture_descriptor_count.descriptorCount = sampler_count;
    }
    assert_not_zero(texture_descriptor_counts_count);
    texture_descriptor_set = transient_descriptor_allocator_textures_.Allocate(
        descriptor_set_layout, texture_descriptor_counts.data(), texture_descriptor_counts_count);
    if (texture_descriptor_set == VK_NULL_HANDLE) {
      return 0;
    }
  }
  UsedTextureTransientDescriptorSet& used_texture_descriptor_set =
      texture_transient_descriptor_sets_used_.emplace_back();
  used_texture_descriptor_set.frame = frame_current_;
  used_texture_descriptor_set.layout = texture_descriptor_set_layout_key;
  used_texture_descriptor_set.set = texture_descriptor_set;
  uint32_t descriptor_set_write_count = 0;
  if (texture_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = 0;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = texture_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_set_write.pImageInfo = texture_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  if (sampler_count) {
    VkWriteDescriptorSet& descriptor_set_write =
        descriptor_set_writes_out[descriptor_set_write_count++];
    descriptor_set_write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptor_set_write.pNext = nullptr;
    descriptor_set_write.dstSet = texture_descriptor_set;
    descriptor_set_write.dstBinding = texture_count;
    descriptor_set_write.dstArrayElement = 0;
    descriptor_set_write.descriptorCount = sampler_count;
    descriptor_set_write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_set_write.pImageInfo = sampler_image_info;
    descriptor_set_write.pBufferInfo = nullptr;
    descriptor_set_write.pTexelBufferView = nullptr;
  }
  assert_not_zero(descriptor_set_write_count);
  return descriptor_set_write_count;
}

}  // namespace rex::graphics::vulkan
