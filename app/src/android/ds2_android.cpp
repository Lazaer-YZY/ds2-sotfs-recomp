// ds2 - Android native glue.
//
// - Custom Vulkan driver loading (Turnip packages via libadrenotools, or a direct ICD), handed to ReXGlue through
//   rex::ui::vulkan::SetInstanceProcAddrOverride before the GPU plugin creates its Vulkan instance.
// - The on-screen touch pad as a synthetic input device (merged into guest user 0 with any physical pad).
// - JNI exports for com.ds2recomp.android.RuntimeActivity (see android/README.md).
//
// Adapted from the Lightning Returns port (src/android/android_glue.cpp), itself adapted from Lost Odyssey
// Recomp (GPL-3.0).

#if defined(__ANDROID__)

#include "android/ds2_android.h"
#include "ds2_cheats.h"
#include "ds2_effects.h"
#include "ds2_framerate.h"
#include "ds2_frame_overlay.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <android/log.h>
#include <dlfcn.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>
#include <jni.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_system.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/vulkan/loader_override.h>

#if defined(DS2_HAS_ADRENOTOOLS)
#include <adrenotools/driver.h>
#endif

#define DS2_ALOG(prio, ...) __android_log_print(prio, "ds2", __VA_ARGS__)

namespace in = rex::input;
using rex::X_RESULT;
using rex::X_STATUS;

// "<GPU name>: <reason>" when the Vulkan device or renderer could not be set up (sdk/src/ui/vulkan/vulkan_device.cpp).
extern "C" char rex_vulkan_failure_reason[256];

namespace ds2::android {

namespace {

const char* EnvOr(const char* name, const char* fallback) {
  const char* v = std::getenv(name);
  return (v && *v) ? v : fallback;
}

bool InstallOverride(void* gipa, const char* driver, const char* how) {
  rex::ui::vulkan::SetInstanceProcAddrOverride(gipa);
  DS2_ALOG(ANDROID_LOG_INFO, "vulkan: active driver = custom '%s' (%s)", driver, how);
  REXLOG_INFO("vulkan: active driver = custom '{}' ({})", driver, how);
  return true;
}

#if defined(DS2_HAS_ADRENOTOOLS)
// Direct ICD loading: the package's library in the app's own linker namespace, entered through
// vk_icdGetInstanceProcAddr. Null when it cannot be opened that way, so the caller falls back to libadrenotools.
void* OpenCustomVulkanIcd(const std::string& path, const char* driver) {
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    const char* err = dlerror();
    DS2_ALOG(ANDROID_LOG_WARN, "vulkan: custom driver '%s' could not be opened as a Vulkan ICD (%s); trying libadrenotools",
             driver, err ? err : "unknown error");
    return nullptr;
  }
  void* gipa = dlsym(handle, "vk_icdGetInstanceProcAddr");
  if (!gipa) {
    DS2_ALOG(ANDROID_LOG_WARN, "vulkan: custom driver '%s' exports no vk_icdGetInstanceProcAddr; trying libadrenotools",
             driver);
    dlclose(handle);
    return nullptr;
  }
  // The Khronos loader negotiates before any other call; Mesa only records it.
  using NegotiateFn = int (*)(uint32_t*);
  if (auto negotiate = reinterpret_cast<NegotiateFn>(dlsym(handle, "vk_icdNegotiateLoaderICDInterfaceVersion"))) {
    uint32_t version = 7;
    negotiate(&version);
  }
  return gipa;
}
#endif

// --- Touch pad --------------------------------------------------------------------------------------------------

struct TouchState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0;
  uint8_t right_trigger = 0;
  int16_t left_x = 0, left_y = 0, right_x = 0, right_y = 0;
};

std::mutex g_touch_mutex;
TouchState g_touch;
std::atomic<uint32_t> g_touch_packet{0};

constexpr in::DeviceId kTouchPad = in::DeviceId(0x445332544F554348ull);  // "DS2TOUCH"

class TouchPad final : public in::InputDriver {
 public:
  TouchPad() : InputDriver(nullptr, 0) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  void EnumerateDevices(std::vector<in::DeviceInfo>& out) override {
    in::DeviceInfo info;
    info.id = kTouchPad;
    info.name = "DS2 touch controls";
    info.synthetic = true;
    out.push_back(info);
  }

  X_RESULT GetDeviceState(in::DeviceId id, in::X_INPUT_STATE* out_state) override {
    if (id != kTouchPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_state) {
      TouchState t;
      {
        std::lock_guard lock(g_touch_mutex);
        t = g_touch;
      }
      std::memset(out_state, 0, sizeof(*out_state));
      out_state->packet_number = g_touch_packet.load(std::memory_order_relaxed);
      out_state->gamepad.buttons = t.buttons;
      out_state->gamepad.left_trigger = t.left_trigger;
      out_state->gamepad.right_trigger = t.right_trigger;
      out_state->gamepad.thumb_lx = t.left_x;
      out_state->gamepad.thumb_ly = t.left_y;
      out_state->gamepad.thumb_rx = t.right_x;
      out_state->gamepad.thumb_ry = t.right_y;
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetDeviceCapabilities(in::DeviceId id, uint32_t flags, in::X_INPUT_CAPABILITIES* out_caps) override {
    (void)flags;
    if (id != kTouchPad) return X_ERROR_DEVICE_NOT_CONNECTED;
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;     // XINPUT_DEVTYPE_GAMEPAD
      out_caps->sub_type = 0x01; // XINPUT_DEVSUBTYPE_GAMEPAD
      out_caps->gamepad.buttons = 0xF3FF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = int16_t(0xFFC0);
      out_caps->gamepad.thumb_ly = int16_t(0xFFC0);
      out_caps->gamepad.thumb_rx = int16_t(0xFFC0);
      out_caps->gamepad.thumb_ry = int16_t(0xFFC0);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT SetDeviceVibration(in::DeviceId id, in::X_INPUT_VIBRATION* vibration) override {
    (void)vibration;
    return id == kTouchPad ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
  }

  X_RESULT GetDeviceKeystroke(in::DeviceId id, uint32_t flags, in::X_INPUT_KEYSTROKE* out_keystroke) override {
    (void)flags;
    (void)out_keystroke;
    return id == kTouchPad ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
  }
};

std::atomic<int64_t> g_launch_ms{-1};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

namespace {
char g_failure_file[512] = {};
struct sigaction g_previous_abort {};

void OnAbort(int sig, siginfo_t* info, void* context) {
  if (rex_vulkan_failure_reason[0] && g_failure_file[0]) {
    const int fd = open(g_failure_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0660);
    if (fd >= 0) {
      size_t n = 0;
      while (n < sizeof(rex_vulkan_failure_reason) && rex_vulkan_failure_reason[n]) ++n;
      (void)!write(fd, rex_vulkan_failure_reason, n);
      close(fd);
    }
  }
  // Hand over to the previous handler (the runtime's or the system's crash reporting).
  sigaction(SIGABRT, &g_previous_abort, nullptr);
  if (g_previous_abort.sa_flags & SA_SIGINFO) {
    if (g_previous_abort.sa_sigaction) g_previous_abort.sa_sigaction(sig, info, context);
  } else if (g_previous_abort.sa_handler != SIG_DFL && g_previous_abort.sa_handler != SIG_IGN &&
             g_previous_abort.sa_handler) {
    g_previous_abort.sa_handler(sig);
  }
  raise(SIGABRT);
}
}  // namespace

void InstallGraphicsFailureSaver() {
  // The renderer's setup ends in rex::FatalError (abort) when it fails on the GPU thread, before any activity
  // callback can run: save the noted reason where GpuDriverStore.takeGraphicsFailure reads it on the next start.
  const char* dir = std::getenv("REX_ANDROID_APP_DIR");
  if (!dir || !*dir) return;
  std::snprintf(g_failure_file, sizeof(g_failure_file), "%sgraphics_failure.txt", dir);
  struct sigaction action {};
  action.sa_sigaction = OnAbort;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&action.sa_mask);
  sigaction(SIGABRT, &action, &g_previous_abort);
}

void ReportGraphicsFailure() {
  const char* reason = rex_vulkan_failure_reason;
  if (!reason[0]) return;  // not a graphics failure (game files, memory): the log tells
  REXLOG_ERROR("ds2 android: the renderer could not start: {}", reason);
  auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!env || !activity) return;
  jclass type = env->GetObjectClass(activity);
  jmethodID method = type ? env->GetMethodID(type, "reportGraphicsFailure", "(Ljava/lang/String;)V") : nullptr;
  if (method) {
    if (jstring text = env->NewStringUTF(reason)) {
      env->CallVoidMethod(activity, method, text);
      env->DeleteLocalRef(text);
    }
  }
  if (env->ExceptionCheck()) env->ExceptionClear();
  if (type) env->DeleteLocalRef(type);
  env->DeleteLocalRef(activity);
}

bool LoadCustomVulkanDriver() {
  const char* driver = std::getenv("DS2_VK_CUSTOM_DRIVER");
  if (!driver || !*driver) {
    DS2_ALOG(ANDROID_LOG_INFO, "vulkan: active driver = system (no custom driver selected)");
    REXLOG_INFO("vulkan: active driver = system (no custom driver selected)");
    return false;
  }
#if !defined(DS2_HAS_ADRENOTOOLS)
  DS2_ALOG(ANDROID_LOG_WARN, "vulkan: built without libadrenotools, ignoring '%s'; active driver = system", driver);
  return false;
#else
  // Adreno 650 / Turnip draw-ordering workaround; keep an explicit TU_DEBUG.
  const char* sync_draw = std::getenv("DS2_VK_ADRENO650_SYNC_DRAW");
  if (sync_draw && std::strcmp(sync_draw, "1") == 0 && !std::getenv("TU_DEBUG")) {
    setenv("TU_DEBUG", "syncdraw", 0);
  }
  // Turnip reserves one of the Adreno 6xx bindless sets for dynamic offsets unless told not to; Mesa reads driconf
  // options from the environment.
  setenv("tu_dont_reserve_descriptor_set", "true", 0);

  const char* hook_dir = EnvOr("DS2_NATIVE_LIB_DIR", "");
  const char* driver_dir = EnvOr("DS2_CUSTOM_DRIVER_DIR", "");
  const char* mode = EnvOr("DS2_VK_CUSTOM_DRIVER_MODE", "adrenotools");

  if (std::strcmp(mode, "icd") == 0) {
    if (void* gipa = OpenCustomVulkanIcd(std::string(driver_dir) + driver, driver)) {
      return InstallOverride(gipa, driver, "direct Vulkan ICD");
    }
  }

  void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hook_dir, driver_dir, driver,
                                            nullptr, nullptr);
  if (!handle) {
    const char* err = dlerror();
    DS2_ALOG(ANDROID_LOG_ERROR, "vulkan: custom driver '%s' failed to load (hooks '%s', dir '%s': %s); active driver = system",
             driver, hook_dir, driver_dir, err ? err : "no dlerror");
    REXLOG_ERROR("vulkan: custom driver '{}' failed to load; active driver = system", driver);
    return false;
  }
  void* gipa = dlsym(handle, "vkGetInstanceProcAddr");
  if (!gipa) {
    REXLOG_ERROR("vulkan: custom driver '{}' has no vkGetInstanceProcAddr; active driver = system", driver);
    return false;
  }
  // libadrenotools resolves the package lazily: an unloadable .so still ends up on the system driver, which the
  // GPU plugin's device log line reveals.
  return InstallOverride(gipa, driver, "libadrenotools");
#endif
}

void WrapTouchInput(rex::RuntimeConfig& config) {
  if (!config.input_factory) return;
  auto base = config.input_factory;
  config.input_factory = [base](bool tool_mode) -> std::unique_ptr<rex::system::IInputSystem> {
    auto system = base(tool_mode);
    if (system && !tool_mode) {
      // The default factory is CreateDefaultInputSystem (sdk/src/ui/rex_app.cpp), a rex::input::InputSystem.
      static_cast<in::InputSystem*>(system.get())->AddDriver(std::make_unique<TouchPad>());
    }
    return system;
  };
}

void NoteModuleLaunched() { g_launch_ms.store(NowMs(), std::memory_order_relaxed); }

void ApplyDefaults() {
  // Android defaults for settings whose code default does not fit this port. Anything set in ds2.toml, a REX_*
  // variable or on the command line wins (DECISIONS.md, 2026-11-17).
  static const std::pair<const char*, const char*> kDefaults[] = {
      // mfmsr/mtmsrd "interrupts off" regions as per-thread state instead of one process-wide mutex (DS2_switch's
      // tested release setting; the mutex is taken tens of thousands of times per second otherwise).
      {"ds2_msr_per_thread", "true"},
      // The Xenos "0 * x = 0" multiply emulation off (LR measured -12 % GPU time; DS2 at the Things Betwixt spawn
      // 51.0 -> 56.7 fps, image unchanged, no black/NaN pixels). Set both to true for exact shader math.
      {"vulkan_shader_xenos_multiply", "false"},
      {"vulkan_shader_xenos_multiply_vertex", "false"},
      // Turnip exposes no sparse residency on Adreno: the non-sparse shared-memory path.
      {"vulkan_sparse_shared_memory", "false"},
      // The save list (Continue / Load) draws each character's portrait on the GPU (128x76 resolve) and copies it on
      // the CPU into the list's texture; without a readback the CPU read stale memory and the portraits were noise
      // (a phone and the Thor, 2026-11-19). Only that size is read back: the menus pay one short GPU wait per portrait.
      {"readback_resolve_sizes", "128x76"},
      // Present on its own thread (strict hand-off: no frame dropped, the GPU command thread waits only if the
      // previous frame is still unpainted) with the swapchain image waited for on the CPU there, so the GPU command
      // thread never blocks on the display and the paint carries no display dependency into the single GPU queue.
      // The owner's 41 fps bonfire-room state (2026-11-20): no free swapchain image on any gameplay frame, that thread
      // blocked ~20 ms per frame, the GPU 67 % busy and its clock lowered to 615 MHz. Measured: neutral at a GPU-bound
      // view (57.9 fps; 0.0 % of frames over 21 ms against 0.1-0.3 %), every frame under 18 ms at a lighter view with a
      // starved swapchain (DECISIONS.md, 2026-11-20 evening). The give-up safety net (DS2_switch's, built for 9-11 fps)
      // would fire in ordinary GPU-bound play, where waiting for the presenter is simply the throttle: off.
      {"present_hilo_propio", "true"},
      {"present_hilo_espera_media_max_us", "0"},
      {"present_esperar_adquisicion_en_cpu", "true"},
      // ReXGlue's default invalidates every guest page uploaded to the GPU at each frame end, so every vertex page a
      // frame draws was copied again (memcpy) and re-protected (mprotect) every frame. The write watches (the game's
      // stores fault, file reads / allocations / protection changes trigger the callbacks) keep the copies coherent;
      // the LR port runs this way. Gate view, live A/B: GPU command thread 10.0 -> 8.8 ms CPU per frame, process
      // -2.2 ms, GPU 16.32 -> 16.23 ms, 57.9 -> 59.7 fps (DECISIONS.md, 2026-11-20 evening).
      {"clear_memory_page_state", "false"},
      // Pre-rotation: the swapchain in the panel's native orientation (portrait on the Thor's top screen and on
      // phones) and the image rotated while painting, so the compositor has nothing to rotate. In the owner's sessions the
      // display driver refused the rotated game layer and SurfaceFlinger rotated it on the GPU every frame (burning
      // gate 41 fps); pre-rotated, the layer goes to the display hardware unrotated (gate view 58.5 fps, no frame over
      // 21 ms; DECISIONS.md, 2026-11-20 night).
      {"present_pre_rotation", "true"},
      // No fragment shading rate state in the guest pipelines: Eco mode (its only user) is gone. A player's phone
      // (Snapdragon 8s Gen 4) flickered with Eco on and, together with the performance overlay, crashed at a bonfire
      // and a fog wall; the saving was small anyway (-5.7 % GPU time on the Thor). DECISIONS.md, 2026-11-20.
      {"vulkan_dynamic_fragment_shading_rate", "false"},
      // Draws that cannot write anything (no pixel shader, no depth / stencil test or write, no memexport, no occlusion
      // query) are left out: ~290 of ~1,600 per frame at the burning gate (1-vertex point lists), each of which went
      // through the whole draw path on the CPU and the GPU (DECISIONS.md, 2026-11-21 night).
      {"vulkan_skip_no_effect_draws", "true"},
      // Movies (game:\movie\*.wmv, drawn by the game's movie player with this pixel shader, seen only while a movie
      // file is open) keep their 16:9 shape on a wide screen instead of stretching across it (the owner's S25,
      // 2026-11-21: the prologue 1.22x wide at 1568x720), scaled up to the full width with the top and bottom cropped
      // (the movies' own letterbox bars; the owner: "could it better use screen real estate?"). No effect at 16:9.
      {"gpu_keep_aspect_pixel_shaders", "47094F2C2592E81B"},
  };
  for (const auto& [name, value] : kDefaults) {
    if (rex::cvar::GetFlagInfo(name) == nullptr) {
      REXLOG_WARN("ds2 android: no setting named {} (default skipped)", name);
      continue;
    }
    // Respect any value ds2.toml, a REX_* variable or the command line set, even one equal to the code default
    // (HasNonDefaultValue cannot tell "set to the default" from "not set").
    if (rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault) continue;
    rex::cvar::SetFlagByName(name, value);
  }
  REXLOG_INFO("ds2 android: {} defaults applied", std::size(kDefaults));
  // The kernel in every log: kernels before 4.17 behave differently in the guest memory code (issue #4).
  {
    struct utsname name {};
    if (uname(&name) == 0) {
      REXLOG_INFO("ds2 android: kernel {} {}, page size {}", name.release, name.machine, sysconf(_SC_PAGESIZE));
    }
  }
  // "Debug logging" off in the CTRL dialog (DS2_DIAG=0, the public build's default): the GPU command thread's
  // per-draw timers and slow-frame lines and the WAIT_REG_MEM statistics are off (Majula arch view: that thread
  // 9.52 -> 9.34 ms per frame over four interleaved pairs). The switch existed but nothing native read DS2_DIAG
  // until 2026-11-19. Info-level log lines stay (the heartbeats are what a crash report needs).
  if (const char* diag = std::getenv("DS2_DIAG"); diag && diag[0] == '0') {
    static const std::pair<const char*, const char*> kQuiet[] = {
        {"vulkan_slow_frame_ms", "0"},
        {"gpu_log_wait_reg_mem", "false"},
    };
    for (const auto& [name, value] : kQuiet) {
      if (rex::cvar::GetFlagInfo(name) == nullptr ||
          rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault) {
        continue;
      }
      rex::cvar::SetFlagByName(name, value);
    }
    REXLOG_INFO("ds2 android: debug logging off (GPU timers and wait statistics off)");
  } else if (diag && diag[0] == '1') {
    // Debug logging on (the development build's default; never the public build's): the owner's play sessions also
    // record the frame-pacing timeline and per-frame breakdowns of late frames (the pacing jitter mode shows up in
    // real play far more often than in scripted runs; DECISIONS.md 2026-11-19 night).
    static const std::pair<const char*, const char*> kSessionDiagnostics[] = {
        {"frame_timeline", "true"},
        {"frame_breakdown_ms", "20"},
        {"vulkan_slow_frame_ms", "20"},
        // GPU time per frame by kind (draws, transfers, resolves, ...) and pass / draw counts every 10 s: the late frames
        // in the owner's busy fights are GPU-bound (2026-11-20). Costs ~0.4 ms of GPU time per frame.
        {"vulkan_gpu_timing", "true"},
    };
    for (const auto& [name, value] : kSessionDiagnostics) {
      if (rex::cvar::GetFlagInfo(name) == nullptr ||
          rex::cvar::GetFlagSource(name) != rex::cvar::Source::kDefault) {
        continue;
      }
      rex::cvar::SetFlagByName(name, value);
    }
    REXLOG_INFO("ds2 android: debug logging on (frame timeline and late-frame breakdowns on)");
  }
}

}  // namespace ds2::android

// ---------------------------------------------------------------------------------------------------------------
// JNI: com.ds2recomp.android.RuntimeActivity
// ---------------------------------------------------------------------------------------------------------------

// 0 = nothing yet, 1 = the game is running, 3 = boot overlay may go. A placeholder until the renderer reports its
// first presented frame.
extern "C" __attribute__((visibility("default"))) JNIEXPORT jint JNICALL
Java_com_ds2recomp_android_RuntimeActivity_nativeBootProgress(JNIEnv*, jclass) {
  const int64_t launched = ds2::android::g_launch_ms.load(std::memory_order_relaxed);
  if (launched < 0) return 0;
  return ds2::android::NowMs() - launched >= 3000 ? 3 : 1;
}

extern "C" __attribute__((visibility("default"))) JNIEXPORT void JNICALL
Java_com_ds2recomp_android_RuntimeActivity_nativeSetTouchInput(JNIEnv*, jclass, jint buttons, jint leftTrigger,
                                                              jint rightTrigger, jint leftX, jint leftY, jint rightX,
                                                              jint rightY) {
  const ds2::android::TouchState next{
      static_cast<uint16_t>(buttons & 0xF3FF),
      static_cast<uint8_t>(std::clamp<jint>(leftTrigger, 0, 255)),
      static_cast<uint8_t>(std::clamp<jint>(rightTrigger, 0, 255)),
      static_cast<int16_t>(std::clamp<jint>(leftX, -32768, 32767)),
      static_cast<int16_t>(std::clamp<jint>(leftY, -32768, 32767)),
      static_cast<int16_t>(std::clamp<jint>(rightX, -32768, 32767)),
      static_cast<int16_t>(std::clamp<jint>(rightY, -32768, 32767)),
  };
  {
    std::lock_guard lock(ds2::android::g_touch_mutex);
    ds2::android::g_touch = next;
  }
  ds2::android::g_touch_packet.fetch_add(1, std::memory_order_relaxed);
}

extern "C" __attribute__((visibility("default"))) JNIEXPORT jboolean JNICALL
Java_com_ds2recomp_android_RuntimeActivity_nativeHasConnectedController(JNIEnv*, jclass) {
  return SDL_HasGamepad() ? JNI_TRUE : JNI_FALSE;
}

// The game's recent frame times for the app's performance overlay (PerfOverlayView, four times a second).
extern "C" __attribute__((visibility("default"))) JNIEXPORT jint JNICALL
Java_com_ds2recomp_android_RuntimeActivity_nativeFrameTimes(JNIEnv* env, jclass, jfloatArray out) {
  if (!out) return 0;
  const jsize capacity = env->GetArrayLength(out);
  if (capacity <= 0) return 0;
  std::vector<float> ms(size_t(std::min<jsize>(capacity, 512)));
  const int n = rex::kernel::xboxkrnl::RecentGuestFrameTimesMs(ms.data(), int(ms.size()));
  if (n > 0) env->SetFloatArrayRegion(out, 0, n, ms.data());
  return n > 0 ? n : 0;
}

// Options from the app's CTRL dialog (applied at once; unknown ones are logged).
extern "C" __attribute__((visibility("default"))) JNIEXPORT void JNICALL
Java_com_ds2recomp_android_RuntimeActivity_nativeSetOption(JNIEnv* env, jclass, jstring name, jstring value) {
  const char* n = name ? env->GetStringUTFChars(name, nullptr) : nullptr;
  const char* v = value ? env->GetStringUTFChars(value, nullptr) : nullptr;
  const std::string_view key = n ? n : "";
  const std::string_view val = v ? v : "";
  if (key == "overlay") {
    ds2::frame_overlay::Show(val == "1" || val == "true");
  } else if (key == "fps") {
    ds2::framerate::SetFrameRate(val);
  } else if (key == "aa") {
    ds2::effects::SetAntiAliasing(val);
  } else if (key == "dof") {
    ds2::effects::SetDepthOfField(val == "1" || val == "true");
  } else if (key == "cheat_add_souls") {
    ds2::cheats::AddSouls(uint32_t(std::strtoul(std::string(val).c_str(), nullptr, 10)));
  } else if (key.rfind("cheat_", 0) == 0) {
    ds2::cheats::Set(key.substr(6), val == "1" || val == "true");
  } else if (key == "prime") {
    ds2::android::SetPrimeGovernor(val == "1" || val == "true");
  } else {
    REXLOG_INFO("ds2: option {} = {} (not wired)", key, val);
  }
  if (n) env->ReleaseStringUTFChars(name, n);
  if (v) env->ReleaseStringUTFChars(value, v);
}

#endif  // __ANDROID__
