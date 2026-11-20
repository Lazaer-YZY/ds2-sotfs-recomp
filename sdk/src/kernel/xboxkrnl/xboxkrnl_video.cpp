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

// Disable warnings about unused parameters for kernel functions
#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <sys/resource.h>
#include <time.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <dirent.h>
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <array>
#include <atomic>
#include <chrono>
#include <string>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/private.h>
#include <rex/kernel/xboxkrnl/rtl.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <rex/runtime.h>
#include <rex/system/export_resolver.h>
#include <rex/system/kernel_state.h>
#include <rex/system/frame_breakdown.h>
#include <rex/system/xtypes.h>
#include <rex/ui/flags.h>

namespace {
// Display gamma type: 0 - linear, 1 - sRGB (CRT), 2 - BT.709 (HDTV), 3 - power
constexpr uint32_t kDisplayGammaType = 2;
// Display gamma power (used with gamma type 3)
constexpr double kDisplayGammaPower = 2.22222233;

uint32_t GetConfiguredVideoModeWidth() {
  int32_t configured_width = REXCVAR_GET(video_mode_width);
  if (!rex::cvar::HasNonDefaultValue("video_mode_width")) {
    if (rex::cvar::HasNonDefaultValue("window_width") && REXCVAR_GET(window_width) > 0) {
      configured_width = REXCVAR_GET(window_width);
    } else {
      int32_t preset_width = 0;
      int32_t preset_height = 0;
      if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                         preset_height)) {
        configured_width = preset_width;
      }
    }
  }
  return uint32_t(std::clamp(configured_width, 640, 0x0FFF));
}

uint32_t GetConfiguredVideoModeHeight() {
  int32_t configured_height = REXCVAR_GET(video_mode_height);
  if (!rex::cvar::HasNonDefaultValue("video_mode_height")) {
    if (rex::cvar::HasNonDefaultValue("window_height") && REXCVAR_GET(window_height) > 0) {
      configured_height = REXCVAR_GET(window_height);
    } else {
      int32_t preset_width = 0;
      int32_t preset_height = 0;
      if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                         preset_height)) {
        configured_height = preset_height;
      }
    }
  }
  return uint32_t(std::clamp(configured_height, 480, 0x0FFF));
}

float GetConfiguredVideoModeRefreshRate() {
  double refresh_rate_hz = std::clamp(REXCVAR_GET(video_mode_refresh_rate), 24.0, 240.0);
  return float(refresh_rate_hz);
}

void WarnNoGpuEmulation(const char* export_name, std::atomic<bool>& warned) {
  if (!warned.exchange(true)) {
    REXKRNL_WARN("{}: no GPU emulation loaded (gpu_plugin not set); call ignored", export_name);
  }
}
}  // namespace

namespace rex::kernel::xboxkrnl {
using namespace rex::system;

REX_EXPORT_STUB(__imp__VdBlockUntilGUIIdle);
REX_EXPORT_STUB(__imp__VdDisplayFatalError);
REX_EXPORT_STUB(__imp__VdEnableClosedCaption);
REX_EXPORT_STUB(__imp__VdEnableDisablePowerSavingMode);
REX_EXPORT_STUB(__imp__VdGenerateGPUCSCCoefficients);
REX_EXPORT_STUB(__imp__VdGetClosedCaptionReadyStatus);
REX_EXPORT_STUB(__imp__VdGetDisplayModeOverride);
REX_EXPORT_STUB(__imp__VdInitializeScaler);
REX_EXPORT_STUB(__imp__VdQuerySystemCommandBuffer);
REX_EXPORT_STUB(__imp__VdReadDVERegisterUlong);
REX_EXPORT_STUB(__imp__VdReadWriteHSIOCalibrationFlag);
REX_EXPORT_STUB(__imp__VdRegisterGraphicsNotification);
REX_EXPORT_STUB(__imp__VdRegisterXamGraphicsNotification);
REX_EXPORT_STUB(__imp__VdSendClosedCaptionData);
REX_EXPORT_STUB(__imp__VdSetCGMSOption);
REX_EXPORT_STUB(__imp__VdSetColorProfileAdjustment);
REX_EXPORT_STUB(__imp__VdSetCscMatricesOverride);
REX_EXPORT_STUB(__imp__VdSetHDCPOption);
REX_EXPORT_STUB(__imp__VdSetMacrovisionOption);
REX_EXPORT_STUB(__imp__VdSetSystemCommandBuffer);
REX_EXPORT_STUB(__imp__VdSetWSSData);
REX_EXPORT_STUB(__imp__VdSetWSSOption);
REX_EXPORT_STUB(__imp__VdTurnDisplayOff);
REX_EXPORT_STUB(__imp__VdTurnDisplayOn);
REX_EXPORT_STUB(__imp__VdWriteDVERegisterUlong);
REX_EXPORT_STUB(__imp__VdInitializeEDRAM);
REX_EXPORT_STUB(__imp__VdReadEEDIDBlock);
REX_EXPORT_STUB(__imp__VdEnumerateVideoModes);
REX_EXPORT_STUB(__imp__VdEnableHDCP);
REX_EXPORT_STUB(__imp__VdRegisterHDCPNotification);
REX_EXPORT_STUB(__imp__VdGetDisplayDiscoveryData);
REX_EXPORT_STUB(__imp__VdStartDisplayDiscovery);
REX_EXPORT_STUB(__imp__VdSetHDCPRevocationList);
REX_EXPORT_STUB(__imp__VdEnableWMAProOverHDMI);
REX_EXPORT_STUB(__imp__VdQueryRealVideoMode);
REX_EXPORT_STUB(__imp__VdSetCGMSState);
REX_EXPORT_STUB(__imp__VdSetSCMSState);
REX_EXPORT_STUB(__imp__VdGetOption);
REX_EXPORT_STUB(__imp__VdSetOption);
REX_EXPORT_STUB(__imp__VdQueryVideoCapabilities);
REX_EXPORT_STUB(__imp__VdGet3dVideoFormat);
REX_EXPORT_STUB(__imp__VdGetWSS2Data);
REX_EXPORT_STUB(__imp__VdSet3dVideoFormat);
REX_EXPORT_STUB(__imp__VdSetWSS2Data);
REX_EXPORT_STUB(__imp__VdSetStudioRGBMode);

// https://web.archive.org/web/20150805074003/https://www.tweakoz.com/orkid/
// http://www.tweakoz.com/orkid/dox/d3/d52/xb360init_8cpp_source.html
// https://github.com/Free60Project/xenosfb/
// https://github.com/Free60Project/xenosfb/blob/master/src/xe.h
// https://github.com/gligli/libxemit
// https://web.archive.org/web/20090428095215/https://msdn.microsoft.com/en-us/library/bb313877.aspx
// https://web.archive.org/web/20100423054747/https://msdn.microsoft.com/en-us/library/bb313961.aspx
// https://web.archive.org/web/20100423054747/https://msdn.microsoft.com/en-us/library/bb313878.aspx
// https://web.archive.org/web/20090510235238/https://msdn.microsoft.com/en-us/library/bb313942.aspx
// https://svn.dd-wrt.com/browser/src/linux/universal/linux-3.8/drivers/gpu/drm/radeon/radeon_ring.c?rev=21595
// https://www.microsoft.com/en-za/download/details.aspx?id=5313 -- "Stripped
// Down Direct3D: Xbox 360 Command Buffer and Resource Management"

void VdGetCurrentDisplayGamma_entry(mapped_u32 type_ptr, mapped_f32 power_ptr) {
  // 1 - sRGB.
  // 2 - TV (BT.709).
  // 3 - use the power written to *power_ptr.
  // Anything else - linear.
  // Used in D3D SetGammaRamp/SetPWLGamma to adjust the ramp for the display.
  *type_ptr = kDisplayGammaType;
  *power_ptr = float(kDisplayGammaPower);
}

struct X_D3DPRIVATE_RECT {
  rex::be<uint32_t> x1;  // 0x0
  rex::be<uint32_t> y1;  // 0x4
  rex::be<uint32_t> x2;  // 0x8
  rex::be<uint32_t> y2;  // 0xC
};
static_assert_size(X_D3DPRIVATE_RECT, 0x10);

struct X_D3DFILTER_PARAMETERS {
  rex::be<float> nyquist;         // 0x0
  rex::be<float> flicker_filter;  // 0x4
  rex::be<float> beta;            // 0x8
};
static_assert_size(X_D3DFILTER_PARAMETERS, 0xC);

struct X_D3DPRIVATE_SCALER_PARAMETERS {
  X_D3DPRIVATE_RECT scaler_source_rect;                 // 0x0
  rex::be<uint32_t> scaled_output_width;                // 0x10
  rex::be<uint32_t> scaled_output_height;               // 0x14
  rex::be<uint32_t> vertical_filter_type;               // 0x18
  X_D3DFILTER_PARAMETERS vertical_filter_parameters;    // 0x1C
  rex::be<uint32_t> horizontal_filter_type;             // 0x28
  X_D3DFILTER_PARAMETERS horizontal_filter_parameters;  // 0x2C
};
static_assert_size(X_D3DPRIVATE_SCALER_PARAMETERS, 0x38);

struct X_DISPLAY_INFO {
  rex::be<uint16_t> front_buffer_width;              // 0x0
  rex::be<uint16_t> front_buffer_height;             // 0x2
  uint8_t front_buffer_color_format;                 // 0x4
  uint8_t front_buffer_pixel_format;                 // 0x5
  X_D3DPRIVATE_SCALER_PARAMETERS scaler_parameters;  // 0x8
  rex::be<uint16_t> display_window_overscan_left;    // 0x40
  rex::be<uint16_t> display_window_overscan_top;     // 0x42
  rex::be<uint16_t> display_window_overscan_right;   // 0x44
  rex::be<uint16_t> display_window_overscan_bottom;  // 0x46
  rex::be<uint16_t> display_width;                   // 0x48
  rex::be<uint16_t> display_height;                  // 0x4A
  rex::be<float> display_refresh_rate;               // 0x4C
  rex::be<uint32_t> display_interlaced;              // 0x50
  uint8_t display_color_format;                      // 0x54
  rex::be<uint16_t> actual_display_width;            // 0x56
};
static_assert_size(X_DISPLAY_INFO, 0x58);

void VdGetCurrentDisplayInformation_entry(ppc_ptr_t<X_DISPLAY_INFO> display_info) {
  X_VIDEO_MODE mode;
  VdQueryVideoMode(&mode);
  display_info.Zero();
  display_info->front_buffer_width = (uint16_t)mode.display_width;
  display_info->front_buffer_height = (uint16_t)mode.display_height;

  display_info->scaler_parameters.scaler_source_rect.x2 = mode.display_width;
  display_info->scaler_parameters.scaler_source_rect.y2 = mode.display_height;
  display_info->scaler_parameters.scaled_output_width = mode.display_width;
  display_info->scaler_parameters.scaled_output_height = mode.display_height;
  display_info->scaler_parameters.horizontal_filter_type = 1;
  display_info->scaler_parameters.vertical_filter_type = 1;

  uint16_t overscan_x = uint16_t(uint32_t(mode.display_width) / 4);
  uint16_t overscan_y = uint16_t(uint32_t(mode.display_height) / 4);
  display_info->display_window_overscan_left = overscan_x;
  display_info->display_window_overscan_top = overscan_y;
  display_info->display_window_overscan_right = overscan_x;
  display_info->display_window_overscan_bottom = overscan_y;
  display_info->display_width = (uint16_t)mode.display_width;
  display_info->display_height = (uint16_t)mode.display_height;
  display_info->display_refresh_rate = mode.refresh_rate;
  display_info->actual_display_width = (uint16_t)mode.display_width;
}

void VdQueryVideoMode(X_VIDEO_MODE* video_mode) {
  // Exposed as CVARs so the guest can observe custom display settings.
  uint32_t display_width = GetConfiguredVideoModeWidth();
  uint32_t display_height = GetConfiguredVideoModeHeight();
  float refresh_rate_hz = GetConfiguredVideoModeRefreshRate();

  std::memset(video_mode, 0, sizeof(X_VIDEO_MODE));
  video_mode->display_width = display_width;
  video_mode->display_height = display_height;
  video_mode->is_interlaced = 0;
  video_mode->is_widescreen = display_width * 3 >= display_height * 4;
  video_mode->is_hi_def = display_width >= 1280 || display_height >= 720;
  video_mode->refresh_rate = refresh_rate_hz;
  video_mode->video_standard = 1;  // NTSC
  video_mode->unknown_0x8a = 0x4A;
  video_mode->unknown_0x01 = 0x01;
}

void VdQueryVideoMode_entry(ppc_ptr_t<X_VIDEO_MODE> video_mode) {
  VdQueryVideoMode(video_mode);
}

u32 VdQueryVideoFlags_entry() {
  X_VIDEO_MODE mode;
  VdQueryVideoMode(&mode);

  uint32_t flags = 0;
  flags |= mode.is_widescreen ? 1 : 0;
  flags |= mode.display_width >= 1024 ? 2 : 0;
  flags |= mode.display_width >= 1920 ? 4 : 0;

  return flags;
}

u32 VdSetDisplayMode_entry(u32 flags) {
  // Often 0x40000000.

  // 0?ccf000 00000000 00000000 000000r0

  // r: 0x00000002 |     1
  // f: 0x08000000 |    27
  // c: 0x30000000 | 28-29
  // ?: 0x40000000 |    30

  // r: 1 = Resolution is 720x480 or 720x576
  // f: 1 = Texture format is k_2_10_10_10 or k_2_10_10_10_AS_16_16_16_16
  // c: Color space (0 = RGB, 1 = ?, 2 = ?)
  // ?: (always set?)

  return 0;
}

u32 VdSetDisplayModeOverride_entry(u32 unk0, u32 unk1, f64 refresh_rate, u32 unk3, u32 unk4) {
  // refresh_rate = 0, 50, 59.9, etc.
  return 0;
}

u32 VdInitializeEngines_entry(u32 unk0, u32 callback, mapped_void arg, mapped_u32 pfp_ptr,
                              mapped_u32 me_ptr) {
  // r3 = 0x4F810000
  // r4 = function ptr (cleanup callback?)
  // r5 = function arg
  // r6 = PFP Microcode
  // r7 = ME Microcode
  return 1;
}

void VdShutdownEngines_entry() {
  // Ignored for now.
  // Games seem to call an Initialize/Shutdown pair to query info, then
  // re-initialize.
}

u32 VdGetGraphicsAsicID_entry() {
  // Games compare for < 0x10 and do VdInitializeEDRAM, else other
  // (retrain/etc).
  return 0x11;
}

u32 VdEnableDisableClockGating_entry(u32 enabled) {
  // Ignored, as it really doesn't matter.
  return 0;
}

void VdSetGraphicsInterruptCallback_entry(u32 callback, mapped_void user_data) {
  // callback takes 2 params
  // r3 = bool 0/1 - 0 is normal interrupt, 1 is some acquire/lock mumble
  // r4 = user_data (r4 of VdSetGraphicsInterruptCallback)
  auto* graphics_system = REX_KERNEL_STATE()->emulator()->graphics_system();
  if (!graphics_system) {
    static std::atomic<bool> warned{false};
    WarnNoGpuEmulation("VdSetGraphicsInterruptCallback", warned);
    return;
  }
  graphics_system->SetInterruptCallback(callback, user_data.guest_address());
}

void VdInitializeRingBuffer_entry(mapped_void ptr, i32 size_log2) {
  // r3 = result of MmGetPhysicalAddress
  // r4 = log2(size)
  // Buffer pointers are from MmAllocatePhysicalMemory with WRITE_COMBINE.
  auto* graphics_system = REX_KERNEL_STATE()->emulator()->graphics_system();
  if (!graphics_system) {
    static std::atomic<bool> warned{false};
    WarnNoGpuEmulation("VdInitializeRingBuffer", warned);
    return;
  }
  graphics_system->InitializeRingBuffer(ptr.guest_address(), size_log2);
}

void VdEnableRingBufferRPtrWriteBack_entry(mapped_void ptr, i32 block_size_log2) {
  // r4 = log2(block size), 6, usually --- <=19
  auto* graphics_system = REX_KERNEL_STATE()->emulator()->graphics_system();
  if (!graphics_system) {
    static std::atomic<bool> warned{false};
    WarnNoGpuEmulation("VdEnableRingBufferRPtrWriteBack", warned);
    return;
  }
  graphics_system->EnableReadPointerWriteBack(ptr.guest_address(), block_size_log2);
}

void VdGetSystemCommandBuffer_entry(mapped_void p0_ptr, mapped_void p1_ptr) {
  p0_ptr.Zero(0x94);
  memory::store_and_swap<uint32_t>(p0_ptr, 0xBEEF0000);
  memory::store_and_swap<uint32_t>(p1_ptr, 0xBEEF0001);
}

void VdSetSystemCommandBufferGpuIdentifierAddress_entry(mapped_void unk) {
  // r3 = 0x2B10(d3d?) + 8
}

// VdVerifyMEInitCommand
// r3
// r4 = 19
// no op?

u32 VdInitializeScalerCommandBuffer_entry(
    u32 scaler_source_xy,                                      // ((uint16_t)y << 16) | (uint16_t)x
    u32 scaler_source_wh,                                      // ((uint16_t)h << 16) | (uint16_t)w
    u32 scaled_output_xy,                                      // ((uint16_t)y << 16) | (uint16_t)x
    u32 scaled_output_wh,                                      // ((uint16_t)h << 16) | (uint16_t)w
    u32 front_buffer_wh,                                       // ((uint16_t)h << 16) | (uint16_t)w
    u32 vertical_filter_type,                                  // 7?
    ppc_ptr_t<X_D3DFILTER_PARAMETERS> vertical_filter_params,  //
    u32 horizontal_filter_type,                                // 7?
    ppc_ptr_t<X_D3DFILTER_PARAMETERS> horizontal_filter_params,  //
    mapped_void unk9,                                            //
    mapped_void dest_ptr,  // Points to the first 80000000h where the memcpy
                           // sources from.
    u32 dest_count         // Count in words.
) {
  // We could fake the commands here, but I'm not sure the game checks for
  // anything but success (non-zero ret).
  // For now, we just fill it with NOPs.
  auto dest = dest_ptr.as_array<uint32_t>();
  for (size_t i = 0; i < dest_count; ++i) {
    dest[i] = 0x80000000;
  }
  return (uint32_t)dest_count;
}

struct BufferScaling {
  rex::be<uint16_t> fb_width;
  rex::be<uint16_t> fb_height;
  rex::be<uint16_t> bb_width;
  rex::be<uint16_t> bb_height;
};
void AppendParam(string::StringBuffer* string_buffer, ppc_ptr_t<BufferScaling> param) {
  string_buffer->AppendFormat("{:08X}(scale {}x{} -> {}x{}))", param.guest_address(),
                              uint16_t(param->bb_width), uint16_t(param->bb_height),
                              uint16_t(param->fb_width), uint16_t(param->fb_height));
}

u32 VdCallGraphicsNotificationRoutines_entry(u32 unk0, ppc_ptr_t<BufferScaling> args_ptr) {
  assert_true(unk0 == 1);

  // TODO(benvanik): what does this mean, I forget:
  // callbacks get 0, r3, r4

  return 0;
}

u32 VdIsHSIOTrainingSucceeded_entry() {
  // BOOL return value
  return 1;
}

u32 VdPersistDisplay_entry(u32 unk0, mapped_u32 unk1_ptr) {
  // unk1_ptr needs to be populated with a pointer passed to
  // MmFreePhysicalMemory(1, *unk1_ptr).
  if (unk1_ptr) {
    auto heap = REX_KERNEL_MEMORY()->LookupHeapByType(true, 16 * 1024);
    uint32_t unk1_value;
    heap->Alloc(64, 32, memory::kMemoryAllocationReserve | memory::kMemoryAllocationCommit,
                memory::kMemoryProtectNoAccess, false, &unk1_value);
    *unk1_ptr = unk1_value;
  }

  return 1;
}

u32 VdRetrainEDRAMWorker_entry(u32 unk0) {
  return 0;
}

u32 VdRetrainEDRAM_entry(u32 unk0, u32 unk1, u32 unk2, u32 unk3, u32 unk4, u32 unk5) {
  return 0;
}

// Start time of each of the game's presented frames (one writer: the thread calling VdSwap), read by the overlay.
namespace {
constexpr uint32_t kGuestFrameRing = 512;
std::array<std::atomic<uint64_t>, kGuestFrameRing> g_guest_frame_ns{};
std::atomic<uint32_t> g_guest_frames{0};
}  // namespace

int RecentGuestFrameTimesMs(float* out, int max) {
  const uint32_t n = g_guest_frames.load(std::memory_order_acquire);
  const uint32_t frames = std::min<uint32_t>({n ? n - 1 : 0, uint32_t(std::max(max, 0)), kGuestFrameRing - 1});
  for (uint32_t k = 0; k < frames; ++k) {
    const uint32_t i = n - frames + k;  // frame i - 1 ends where frame i starts
    const uint64_t a = g_guest_frame_ns[(i - 1) % kGuestFrameRing].load(std::memory_order_relaxed);
    const uint64_t b = g_guest_frame_ns[i % kGuestFrameRing].load(std::memory_order_relaxed);
    out[k] = b > a ? float(double(b - a) / 1e6) : 0.0f;
  }
  return int(frames);
}

// Frame cap: the game's swaps at most one per N emulated vertical blanks (0 = off). DS2 sets it from its frame rate
// option (app/src/ds2_framerate.cpp). The wait is in the guest's own present call, so the game is throttled at once
// (no extra frames queued for the GPU).
REXCVAR_DEFINE_INT32(swap_min_vblanks, 0, "GPU",
                     "Frame cap: at least this many emulated vertical blanks between the game's swaps (0 = off)")
    .range(0, 8)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(frame_timeline, false, "GPU",
                    "Diagnostics: record the events that pace a frame (game swap and frame cap, vertical blanks, "
                    "interrupts, the game's waits for the command processor, the command thread's waits) and log "
                    "the last 200 ms when frames start to jitter (8 of the last 30 over 18 ms, none over 30; at most 12 dumps, "
                    "20 s apart) and once while steady, for comparison");
REXCVAR_DEFINE_UINT32(frame_breakdown_ms, 0, "GPU",
                      "Diagnostics: for every game frame longer than this many ms, log how the presenting thread "
                      "spent it (CPU, run queue, guest waits by call site, sleeps, frame cap), plus a summary of all "
                      "frames every 10 s; 0 = off")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {
uint64_t g_last_swap_vblank = 0;

extern "C" void (*rex_gpu_timeline_observer)(uint32_t kind, uint32_t value);
// frame_timeline: the late-frame window and the dumps.
void FrameTimelineOnSwap(uint64_t now_ns, uint64_t interval_ns) {
  namespace fb = rex::system::frame_breakdown;
  const bool on = REXCVAR_GET(frame_timeline);
  if (on != fb::g_timeline_on.load(std::memory_order_relaxed)) {
    fb::g_timeline_on.store(on, std::memory_order_relaxed);
    static void (*const kObserver)(uint32_t, uint32_t) = [](uint32_t kind, uint32_t value) {
      static const char kKinds[] = "?kKwWP";
      rex::system::frame_breakdown::TimelineRecord(kind < 6 ? kKinds[kind] : '?', value);
    };
    rex_gpu_timeline_observer = on ? kObserver : nullptr;
  }
  if (!on) return;
  static uint64_t late_bits = 0;  // the last 30 frames, 1 = over 18 ms
  static uint64_t long_bits = 0;  // the last 30 frames, 1 = over 30 ms (loading, hitches: not the pacing jitter)
  static uint32_t frames = 0, dumps = 0, steady_dumps = 0;
  static uint64_t last_dump_ns = 0;
  late_bits = ((late_bits << 1) | (interval_ns > 18000000 ? 1 : 0)) & ((uint64_t(1) << 30) - 1);
  long_bits = ((long_bits << 1) | (interval_ns > 30000000 ? 1 : 0)) & ((uint64_t(1) << 30) - 1);
  ++frames;
  const int late = __builtin_popcountll(late_bits);
  if (frames > 3600 && dumps < 12 && late >= 8 && !long_bits && now_ns - last_dump_ns > 20000000000ull) {
    ++dumps;
    last_dump_ns = now_ns;
    fb::TimelineDump(200.0, "jitter: 8+ of the last 30 frames over 18 ms");
  } else if (frames > 3600 && steady_dumps < 1 && late == 0 && frames % 600 == 0) {
    ++steady_dumps;
    fb::TimelineDump(200.0, "steady: none of the last 30 frames over 18 ms");
  }
}

// frame_breakdown_ms: the presenting thread's clocks at its previous swap and the 10-s window sums.
struct FrameBreakdown {
  bool primed = false;
  uint64_t swap_ns = 0, cpu_ns = 0, run_ns = 0, queue_ns = 0, wait_ns = 0, delay_ns = 0, proc_ns = 0;
  uint32_t waits = 0;
  long nvcsw = 0, nivcsw = 0, minflt = 0, majflt = 0;
  uint64_t window_start_ns = 0;
  uint32_t frames = 0, slow = 0;
  double sum_ms = 0, sum_cpu = 0, sum_queue = 0, sum_wait = 0, sum_delay = 0, sum_cap = 0, sum_proc = 0,
         sum_other = 0, max_ms = 0;
  // Thread groups (ThreadGroups below): CPU and run-queue time per frame, summed over the window.
  double sum_group_run[4] = {}, sum_group_queue[4] = {};
  uint64_t sum_group_slices[4] = {};
};
FrameBreakdown g_fb;

// The threads every frame of the game thread depends on, by group: the game's job workers, Havok's job threads, the
// game's draw-command thread and the SDK's GPU command thread. Their CPU and run-queue time (schedstat) per game frame
// tell a late frame's waits apart: workers busy (real work), workers runnable but not running (core contention), or
// neither (wake-up latency). The thread list is refreshed with each 10-s window.
struct ThreadGroups {
  static constexpr int kGroups = 4;
  static constexpr const char* kPrefix[kGroups] = {"MtTaskWorkerThr", "Havok Job Threa", "DrawCommandThre",
                                                   "GPU Commands"};
  static constexpr const char* kLabel[kGroups] = {"workers", "havok", "draw", "gpu"};
  struct Entry {
    int fd;
    int group;
  };
  std::vector<Entry> entries;
  uint64_t last_run[kGroups] = {}, last_queue[kGroups] = {}, last_slices[kGroups] = {};
  bool primed = false;
  // The /proc scan (~40 file reads, 1-2 ms) runs on a helper thread, not in the game's swap; the swap adopts the
  // new list at its next frame (Adopt).
  std::mutex pending_mutex;
  std::vector<Entry> pending;
  std::atomic<bool> has_pending{false}, scanning{false};

  void Refresh() {
    if (scanning.exchange(true)) return;
    std::thread([this] {
      std::vector<Entry> list = Scan();
      {
        std::lock_guard<std::mutex> lock(pending_mutex);
        for (const Entry& e : pending) close(e.fd);  // an unadopted older list
        pending = std::move(list);
      }
      has_pending.store(true, std::memory_order_release);
      scanning.store(false);
    }).detach();
  }
  // On the swap thread: switch to a finished scan (the next frame primes the new list's totals).
  void Adopt() {
    if (!has_pending.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(pending_mutex);
#if defined(__linux__) || defined(__ANDROID__)
    for (const Entry& e : entries) close(e.fd);
#endif
    entries = std::move(pending);
    pending.clear();
    has_pending.store(false, std::memory_order_relaxed);
    primed = false;
  }
  static std::vector<Entry> Scan() {
    std::vector<Entry> entries;
#if defined(__linux__) || defined(__ANDROID__)
    DIR* dir = opendir("/proc/self/task");
    if (!dir) return entries;
    while (dirent* d = readdir(dir)) {
      if (d->d_name[0] < '0' || d->d_name[0] > '9') continue;
      char path[96], comm[32] = {};
      std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", d->d_name);
      FILE* f = std::fopen(path, "r");
      if (!f) continue;
      const size_t n = std::fread(comm, 1, sizeof(comm) - 1, f);
      std::fclose(f);
      comm[n] = 0;
      for (int g = 0; g < kGroups; ++g) {
        if (std::strncmp(comm, kPrefix[g], std::strlen(kPrefix[g])) != 0) continue;
        std::snprintf(path, sizeof(path), "/proc/self/task/%s/schedstat", d->d_name);
        const int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) entries.push_back({fd, g});
        break;
      }
    }
    closedir(dir);
#endif
    return entries;
  }
  // Group totals now (CPU time, run-queue time, times scheduled in); false when unavailable.
  bool Read(uint64_t run[kGroups], uint64_t queue[kGroups], uint64_t slices[kGroups]) {
    for (int g = 0; g < kGroups; ++g) run[g] = queue[g] = slices[g] = 0;
#if defined(__linux__) || defined(__ANDROID__)
    if (entries.empty()) return false;
    for (const Entry& e : entries) {
      char buf[96];
      const ssize_t n = pread(e.fd, buf, sizeof(buf) - 1, 0);
      if (n <= 0) continue;
      buf[n] = 0;
      char* end = nullptr;
      run[e.group] += std::strtoull(buf, &end, 10);
      queue[e.group] += std::strtoull(end, &end, 10);
      slices[e.group] += std::strtoull(end, nullptr, 10);
    }
    return true;
#else
    return false;
#endif
  }
};
ThreadGroups g_groups;

// Busy share of each CPU since the previous call (/proc/stat), e.g. "0:12 1:8 ... 7:64" in percent; empty if unreadable.
std::string CpuBusySinceLast() {
  static std::vector<std::pair<uint64_t, uint64_t>> last;  // per CPU: busy, total jiffies
  std::vector<std::pair<uint64_t, uint64_t>> now;
#if defined(__linux__) || defined(__ANDROID__)
  if (FILE* f = std::fopen("/proc/stat", "r")) {
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
      if (std::strncmp(line, "cpu", 3) != 0 || line[3] < '0' || line[3] > '9') continue;
      unsigned long long v[8] = {};
      int cpu = -1;
      if (std::sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &cpu, &v[0], &v[1], &v[2], &v[3],
                      &v[4], &v[5], &v[6], &v[7]) < 5 || cpu < 0) {
        continue;
      }
      if (size_t(cpu) >= now.size()) now.resize(size_t(cpu) + 1);
      const uint64_t idle = v[3] + v[4];
      uint64_t total = 0;
      for (unsigned long long x : v) total += x;
      now[size_t(cpu)] = {total - idle, total};
    }
    std::fclose(f);
  }
#endif
  std::string out;
  if (last.size() == now.size()) {
    for (size_t cpu = 0; cpu < now.size(); ++cpu) {
      const uint64_t total = now[cpu].second - last[cpu].second;
      const uint64_t busy = now[cpu].first - last[cpu].first;
      // An offline CPU (core_ctl parks some) shows no time at all.
      out += fmt::format("{}{}:{}", out.empty() ? "" : " ", cpu, total ? fmt::format("{}", 100 * busy / total) : "off");
    }
  }
  last = std::move(now);
  return out;
}

uint64_t ClockNs(clockid_t id) {
  timespec ts{};
  clock_gettime(id, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// Called after the frame cap's wait (cap_ns long) with the swap's timestamp.
void FrameBreakdownOnSwap(uint64_t now_ns, uint64_t cap_ns, uint32_t frame) {
  namespace fb = rex::system::frame_breakdown;
  const uint32_t threshold_ms = REXCVAR_GET(frame_breakdown_ms);
  fb::g_on.store(threshold_ms != 0, std::memory_order_relaxed);
  if (!threshold_ms) {
    g_fb.primed = false;
    return;
  }
  fb::ThreadWaits& w = fb::CurrentThreadWaits();
  uint64_t run_ns = 0, queue_ns = 0;
  fb::ThreadSchedStat(run_ns, queue_ns);
  const uint64_t cpu_ns = ClockNs(CLOCK_THREAD_CPUTIME_ID);
  const uint64_t proc_ns = ClockNs(CLOCK_PROCESS_CPUTIME_ID);
  rusage ru{};
#if defined(RUSAGE_THREAD)
  getrusage(RUSAGE_THREAD, &ru);
#endif
  uint64_t group_run[ThreadGroups::kGroups], group_queue[ThreadGroups::kGroups],
      group_slices[ThreadGroups::kGroups];
  g_groups.Adopt();
  const bool groups_ok = g_groups.Read(group_run, group_queue, group_slices) && g_groups.primed;
  double group_run_ms[ThreadGroups::kGroups] = {}, group_queue_ms[ThreadGroups::kGroups] = {};
  uint64_t group_slices_d[ThreadGroups::kGroups] = {};
  bool groups_valid = groups_ok;
  for (int g = 0; g < ThreadGroups::kGroups && groups_ok; ++g) {
    // A listed thread that exited takes its time out of the total: skip this frame, find the threads again.
    if (group_run[g] < g_groups.last_run[g] || group_queue[g] < g_groups.last_queue[g] ||
        group_slices[g] < g_groups.last_slices[g]) {
      groups_valid = false;
    }
  }
  if (groups_ok && !groups_valid) g_groups.Refresh();
  if (groups_valid) {
    for (int g = 0; g < ThreadGroups::kGroups; ++g) {
      group_run_ms[g] = (group_run[g] - g_groups.last_run[g]) / 1e6;
      group_queue_ms[g] = (group_queue[g] - g_groups.last_queue[g]) / 1e6;
      group_slices_d[g] = group_slices[g] - g_groups.last_slices[g];
    }
  }
  if (g_fb.primed) {
    const double ms = (now_ns - g_fb.swap_ns) / 1e6;
    const double cpu = (cpu_ns - g_fb.cpu_ns) / 1e6;
    const double queue = (queue_ns - g_fb.queue_ns) / 1e6;
    const double wait = (w.wait_ns - g_fb.wait_ns) / 1e6;
    const double delay = (w.delay_ns - g_fb.delay_ns) / 1e6;
    const double cap = cap_ns / 1e6;
    const double proc = (proc_ns - g_fb.proc_ns) / 1e6;
    const double other = ms - cpu - queue - wait - delay - cap;
    if (!g_fb.window_start_ns) g_fb.window_start_ns = now_ns;
    ++g_fb.frames;
    g_fb.sum_ms += ms;
    g_fb.sum_cpu += cpu;
    g_fb.sum_queue += queue;
    g_fb.sum_wait += wait;
    g_fb.sum_delay += delay;
    g_fb.sum_cap += cap;
    g_fb.sum_proc += proc;
    g_fb.sum_other += other;
    g_fb.max_ms = std::max(g_fb.max_ms, ms);
    std::string groups;
    for (int g = 0; g < ThreadGroups::kGroups && groups_valid; ++g) {
      g_fb.sum_group_run[g] += group_run_ms[g];
      g_fb.sum_group_queue[g] += group_queue_ms[g];
      g_fb.sum_group_slices[g] += group_slices_d[g];
      groups += fmt::format("{}{} {:.1f}/{:.1f} x{}", g ? ", " : "; threads cpu/run queue/runs: ",
                            ThreadGroups::kLabel[g], group_run_ms[g], group_queue_ms[g], group_slices_d[g]);
    }
    if (ms > double(threshold_ms)) {
      ++g_fb.slow;
      std::string sites;
      fb::WaitSite sorted[fb::ThreadWaits::kSites];
      std::copy(std::begin(w.sites), std::end(w.sites), sorted);
      std::sort(std::begin(sorted), std::end(sorted),
                [](const fb::WaitSite& a, const fb::WaitSite& b) { return a.ns > b.ns; });
      for (const fb::WaitSite& s : sorted) {
        if (!s.count || s.ns < 200000) continue;
        sites += fmt::format(" {}{:08X} {:.1f}ms x{};", (s.lr & 1) ? "delay@" : "", s.lr & ~1u, s.ns / 1e6,
                             s.count);
      }
      REXKRNL_INFO("frame breakdown: frame {} {:.1f} ms at={:.3f}: cpu {:.1f} + run queue {:.1f} + waits {:.1f} "
                   "({}) + delays {:.1f} + frame cap {:.1f} + other {:.1f}; process cpu {:.1f} ms; switches "
                   "{}/{} (voluntary/forced), faults {}/{}; waits by site:{}{}",
                   frame, ms, now_ns / 1e9, cpu, queue, wait, w.waits - g_fb.waits, delay, cap, other, proc,
                   ru.ru_nvcsw - g_fb.nvcsw, ru.ru_nivcsw - g_fb.nivcsw, ru.ru_minflt - g_fb.minflt,
                   ru.ru_majflt - g_fb.majflt, sites, groups);
    }
    if (now_ns - g_fb.window_start_ns >= 10000000000ull) {
      const double n = std::max<uint32_t>(g_fb.frames, 1);
      REXKRNL_INFO("frame breakdown (10 s): {} frames, avg {:.2f} ms (max {:.1f}, {} over {} ms) = cpu {:.2f} + run "
                   "queue {:.2f} + waits {:.2f} + delays {:.2f} + frame cap {:.2f} + other {:.2f}; process cpu "
                   "{:.1f} ms per frame",
                   g_fb.frames, g_fb.sum_ms / n, g_fb.max_ms, g_fb.slow, threshold_ms, g_fb.sum_cpu / n,
                   g_fb.sum_queue / n, g_fb.sum_wait / n, g_fb.sum_delay / n, g_fb.sum_cap / n,
                   g_fb.sum_other / n, g_fb.sum_proc / n);
      if (groups_valid) {
        // Runs = times a thread of the group was scheduled in; queue / runs is the average wait for a core per run
        // (tens of us: wake-up latency; milliseconds: queued behind other threads).
        std::string avg;
        for (int g = 0; g < ThreadGroups::kGroups; ++g) {
          const double runs = double(g_fb.sum_group_slices[g]);
          avg += fmt::format("{}{} {:.2f}/{:.2f} ({:.0f} runs, {:.0f} us queued each)", g ? ", " : "",
                             ThreadGroups::kLabel[g], g_fb.sum_group_run[g] / n, g_fb.sum_group_queue[g] / n,
                             runs / n, runs ? g_fb.sum_group_queue[g] * 1000.0 / runs : 0.0);
        }
        REXKRNL_INFO("frame breakdown (10 s): threads cpu/run queue per frame: {} ({} threads); cpu busy %: {}",
                     avg, g_groups.entries.size(), CpuBusySinceLast());
      }
      FrameBreakdown next;
      next.window_start_ns = now_ns;
      g_fb = next;
      g_groups.Refresh();  // threads come and go; the next frame primes the new list
    }
  }
  if (!g_groups.primed) {
    // An empty list (early in the boot) is rescanned at most once a second.
    static uint64_t last_scan_ns = 0;
    if (g_groups.entries.empty() && now_ns - last_scan_ns > 1000000000ull) {
      last_scan_ns = now_ns;
      g_groups.Refresh();
    }
    g_groups.Read(group_run, group_queue, group_slices);
    g_groups.primed = !g_groups.entries.empty();
  }
  for (int g = 0; g < ThreadGroups::kGroups; ++g) {
    g_groups.last_run[g] = group_run[g];
    g_groups.last_queue[g] = group_queue[g];
    g_groups.last_slices[g] = group_slices[g];
  }
  g_fb.primed = true;
  g_fb.swap_ns = now_ns;
  g_fb.cpu_ns = cpu_ns;
  g_fb.run_ns = run_ns;
  g_fb.queue_ns = queue_ns;
  g_fb.wait_ns = w.wait_ns;
  g_fb.delay_ns = w.delay_ns;
  g_fb.waits = w.waits;
  g_fb.proc_ns = proc_ns;
  g_fb.nvcsw = ru.ru_nvcsw;
  g_fb.nivcsw = ru.ru_nivcsw;
  g_fb.minflt = ru.ru_minflt;
  g_fb.majflt = ru.ru_majflt;
  w.ResetSites();
}
}  // namespace

// The host thread id of the game's presenting thread (the last VdSwap caller; 0 before the first swap), for app hooks
// that treat the game's main thread differently (DS2: the GPU-wait timing for the prime-core governor). extern "C".
extern "C" __attribute__((visibility("default"))) std::atomic<int> rex_swap_thread_tid{0};
// Game frames presented (VdSwap calls) and the frame cap's total wait in them, for app-side pacing decisions (DS2: the
// prime-core governor).
extern "C" __attribute__((visibility("default"))) std::atomic<uint64_t> rex_swap_count{0};
extern "C" __attribute__((visibility("default"))) std::atomic<uint64_t> rex_swap_cap_ns_total{0};
// Called on the presenting thread for every game frame (after the frame cap), with the frame's interval and the cap's
// wait; set by the app (DS2: the prime-core governor), nullptr = none.
extern "C" __attribute__((visibility("default"))) std::atomic<void (*)(uint64_t interval_ns, uint64_t cap_ns)>
    rex_swap_observer{nullptr};

void VdSwap_entry(mapped_void buffer_ptr,      // ptr into primary ringbuffer
                  mapped_void fetch_ptr,       // frontbuffer Direct3D 9 texture header fetch
                  mapped_void unk2,            // system writeback ptr
                  mapped_void unk3,            // buffer from VdGetSystemCommandBuffer
                  mapped_void unk4,            // from VdGetSystemCommandBuffer (0xBEEF0001)
                  mapped_u32 frontbuffer_ptr,  // ptr to frontbuffer address
                  mapped_u32 texture_format_ptr, mapped_u32 color_space_ptr, mapped_u32 width,
                  mapped_u32 height) {
  uint64_t cap_ns = 0;
#if defined(__linux__) || defined(__ANDROID__)
  {
    thread_local const int tid = int(syscall(SYS_gettid));
    rex_swap_thread_tid.store(tid, std::memory_order_relaxed);
  }
#endif
  rex::system::frame_breakdown::TimelineMark('S');
  if (const int32_t min_vblanks = REXCVAR_GET(swap_min_vblanks); min_vblanks > 0) {
    if (auto* graphics_system = REX_KERNEL_STATE()->emulator()->graphics_system()) {
      const uint64_t target = g_last_swap_vblank + uint64_t(min_vblanks);
      if (graphics_system->vblank_count() < target) {
        const uint64_t t0 = rex::system::frame_breakdown::NowNs();
        graphics_system->WaitForVblankCount(target, 200);
        cap_ns = rex::system::frame_breakdown::NowNs() - t0;
      }
      g_last_swap_vblank = graphics_system->vblank_count();
    }
  }
  {
    const uint32_t i = g_guest_frames.load(std::memory_order_relaxed);
    const uint64_t now_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now().time_since_epoch())
                                         .count());
    const uint64_t previous_ns =
        i ? g_guest_frame_ns[(i - 1) % kGuestFrameRing].load(std::memory_order_relaxed) : now_ns;
    g_guest_frame_ns[i % kGuestFrameRing].store(now_ns, std::memory_order_relaxed);
    g_guest_frames.store(i + 1, std::memory_order_release);
    rex::system::frame_breakdown::TimelineMark('C');
    rex_swap_cap_ns_total.fetch_add(cap_ns, std::memory_order_relaxed);
    rex_swap_count.fetch_add(1, std::memory_order_release);
    if (auto* observer = rex_swap_observer.load(std::memory_order_acquire)) observer(now_ns - previous_ns, cap_ns);
    FrameBreakdownOnSwap(now_ns, cap_ns, i);
    FrameTimelineOnSwap(now_ns, now_ns - previous_ns);
  }
  // All of these parameters are REQUIRED.
  assert(buffer_ptr);
  assert(fetch_ptr);
  assert(frontbuffer_ptr);
  assert(texture_format_ptr);
  assert(width);
  assert(height);

  namespace xenos = rex::graphics::xenos;

  xenos::xe_gpu_texture_fetch_t gpu_fetch;
  memory::copy_and_swap_32_unaligned(&gpu_fetch,
                                     reinterpret_cast<uint32_t*>(fetch_ptr.host_address()), 6);

  // The fetch constant passed is not a true GPU fetch constant, but rather, the
  // fetch constant stored in the Direct3D 9 texture header, which contains the
  // address in one of the virtual mappings of the physical memory rather than
  // the physical address itself. We're emulating swapping in the GPU subsystem,
  // which works with GPU memory addresses (physical addresses directly) from
  // proper fetch constants like ones used to bind textures to shaders, not CPU
  // MMU addresses, so translation from virtual to physical is needed.
  uint32_t frontbuffer_virtual_address = gpu_fetch.base_address << 12;
  assert_true(*frontbuffer_ptr == frontbuffer_virtual_address);
  uint32_t frontbuffer_physical_address =
      REX_KERNEL_MEMORY()->GetPhysicalAddress(frontbuffer_virtual_address);
  assert_true(frontbuffer_physical_address != UINT32_MAX);
  if (frontbuffer_physical_address == UINT32_MAX) {
    // Xenia-specific safety check.
    REXKRNL_ERROR("VdSwap: Invalid front buffer virtual address 0x{:08X}",
                  frontbuffer_virtual_address);
    return;
  }
  gpu_fetch.base_address = frontbuffer_physical_address >> 12;

  auto texture_format = rex::graphics::xenos::TextureFormat(texture_format_ptr.value());
  auto color_space = *color_space_ptr;
  assert_true(texture_format == rex::graphics::xenos::TextureFormat::k_8_8_8_8 ||
              texture_format == rex::graphics::xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16);
  assert_true(color_space == 0);  // RGB(0)
  assert_true(*width == 1 + gpu_fetch.size_2d.width);
  assert_true(*height == 1 + gpu_fetch.size_2d.height);
  {
    // The game's front buffer size, logged when it changes (what the video mode and resolution settings gave).
    static std::atomic<uint32_t> logged_size{0};
    const uint32_t size = (uint32_t(*width) << 16) | (uint32_t(*height) & 0xFFFF);
    if (logged_size.exchange(size, std::memory_order_relaxed) != size) {
      REXKRNL_INFO("VdSwap: front buffer {}x{}", uint32_t(*width), uint32_t(*height));
    }
  }

  // The caller seems to reserve 64 words (256b) in the primary ringbuffer
  // for this method to do what it needs. We just zero them out and send a
  // token value. It'd be nice to figure out what this is really doing so
  // that we could simulate it, though due to TCR I bet all games need to
  // use this method.
  buffer_ptr.Zero(64 * 4);

  uint32_t offset = 0;
  auto dwords = buffer_ptr.as_array<uint32_t>();

  // Write in the GPU texture fetch.
  dwords[offset++] =
      xenos::MakePacketType0(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0, 6);
  dwords[offset++] = gpu_fetch.dword_0;
  dwords[offset++] = gpu_fetch.dword_1;
  dwords[offset++] = gpu_fetch.dword_2;
  dwords[offset++] = gpu_fetch.dword_3;
  dwords[offset++] = gpu_fetch.dword_4;
  dwords[offset++] = gpu_fetch.dword_5;

  dwords[offset++] = xenos::MakePacketType3(xenos::PM4_XE_SWAP, 4);
  dwords[offset++] = rex::graphics::xenos::kSwapSignature;
  dwords[offset++] = frontbuffer_physical_address;

  dwords[offset++] = *width;
  dwords[offset++] = *height;

  // Fill the rest of the buffer with NOP packets.
  for (uint32_t i = offset; i < 64; i++) {
    dwords[i] = xenos::MakePacketType2();
  }
}

void RegisterVideoExports(rex::runtime::ExportResolver* export_resolver,
                          KernelState* kernel_state) {
  auto memory = kernel_state->memory();

  // VdGlobalDevice (4b)
  // Pointer to a global D3D device. Games only seem to set this, so we don't
  // have to do anything. We may want to read it back later, though.
  uint32_t pVdGlobalDevice = memory->SystemHeapAlloc(4, 32, memory::kSystemHeapPhysical);
  export_resolver->SetVariableMapping("xboxkrnl.exe", 0x01BE, pVdGlobalDevice);
  memory::store_and_swap<uint32_t>(memory->TranslateVirtual(pVdGlobalDevice), 0);

  // VdGlobalXamDevice (4b)
  // Pointer to the XAM D3D device, which we don't have.
  uint32_t pVdGlobalXamDevice = memory->SystemHeapAlloc(4, 32, memory::kSystemHeapPhysical);
  export_resolver->SetVariableMapping("xboxkrnl.exe", 0x01BF, pVdGlobalXamDevice);
  memory::store_and_swap<uint32_t>(memory->TranslateVirtual(pVdGlobalXamDevice), 0);

  // VdGpuClockInMHz (4b)
  // GPU clock. Xenos is 500MHz. Hope nothing is relying on this timing...
  uint32_t pVdGpuClockInMHz = memory->SystemHeapAlloc(4, 32, memory::kSystemHeapPhysical);
  export_resolver->SetVariableMapping("xboxkrnl.exe", 0x01C0, pVdGpuClockInMHz);
  memory::store_and_swap<uint32_t>(memory->TranslateVirtual(pVdGpuClockInMHz), 500);

  // VdHSIOCalibrationLock (28b)
  // CriticalSection.
  uint32_t pVdHSIOCalibrationLock = memory->SystemHeapAlloc(28, 32, memory::kSystemHeapPhysical);
  export_resolver->SetVariableMapping("xboxkrnl.exe", 0x01C1, pVdHSIOCalibrationLock);
  auto hsio_lock = memory->TranslateVirtual<X_RTL_CRITICAL_SECTION*>(pVdHSIOCalibrationLock);
  xeRtlInitializeCriticalSectionAndSpinCount(hsio_lock, pVdHSIOCalibrationLock, 10000);
}

}  // namespace rex::kernel::xboxkrnl

REX_EXPORT(__imp__VdGetCurrentDisplayGamma, rex::kernel::xboxkrnl::VdGetCurrentDisplayGamma_entry)
REX_EXPORT(__imp__VdGetCurrentDisplayInformation,
           rex::kernel::xboxkrnl::VdGetCurrentDisplayInformation_entry)
REX_EXPORT(__imp__VdQueryVideoMode, rex::kernel::xboxkrnl::VdQueryVideoMode_entry)
REX_EXPORT(__imp__VdQueryVideoFlags, rex::kernel::xboxkrnl::VdQueryVideoFlags_entry)
REX_EXPORT(__imp__VdSetDisplayMode, rex::kernel::xboxkrnl::VdSetDisplayMode_entry)
REX_EXPORT(__imp__VdSetDisplayModeOverride, rex::kernel::xboxkrnl::VdSetDisplayModeOverride_entry)
REX_EXPORT(__imp__VdInitializeEngines, rex::kernel::xboxkrnl::VdInitializeEngines_entry)
REX_EXPORT(__imp__VdShutdownEngines, rex::kernel::xboxkrnl::VdShutdownEngines_entry)
REX_EXPORT(__imp__VdGetGraphicsAsicID, rex::kernel::xboxkrnl::VdGetGraphicsAsicID_entry)
REX_EXPORT(__imp__VdEnableDisableClockGating,
           rex::kernel::xboxkrnl::VdEnableDisableClockGating_entry)
REX_EXPORT(__imp__VdSetGraphicsInterruptCallback,
           rex::kernel::xboxkrnl::VdSetGraphicsInterruptCallback_entry)
REX_EXPORT(__imp__VdInitializeRingBuffer, rex::kernel::xboxkrnl::VdInitializeRingBuffer_entry)
REX_EXPORT(__imp__VdEnableRingBufferRPtrWriteBack,
           rex::kernel::xboxkrnl::VdEnableRingBufferRPtrWriteBack_entry)
REX_EXPORT(__imp__VdGetSystemCommandBuffer, rex::kernel::xboxkrnl::VdGetSystemCommandBuffer_entry)
REX_EXPORT(__imp__VdSetSystemCommandBufferGpuIdentifierAddress,
           rex::kernel::xboxkrnl::VdSetSystemCommandBufferGpuIdentifierAddress_entry)
REX_EXPORT(__imp__VdInitializeScalerCommandBuffer,
           rex::kernel::xboxkrnl::VdInitializeScalerCommandBuffer_entry)
REX_EXPORT(__imp__VdCallGraphicsNotificationRoutines,
           rex::kernel::xboxkrnl::VdCallGraphicsNotificationRoutines_entry)
REX_EXPORT(__imp__VdIsHSIOTrainingSucceeded, rex::kernel::xboxkrnl::VdIsHSIOTrainingSucceeded_entry)
REX_EXPORT(__imp__VdPersistDisplay, rex::kernel::xboxkrnl::VdPersistDisplay_entry)
REX_EXPORT(__imp__VdRetrainEDRAMWorker, rex::kernel::xboxkrnl::VdRetrainEDRAMWorker_entry)
REX_EXPORT(__imp__VdRetrainEDRAM, rex::kernel::xboxkrnl::VdRetrainEDRAM_entry)
REX_EXPORT(__imp__VdSwap, rex::kernel::xboxkrnl::VdSwap_entry)
