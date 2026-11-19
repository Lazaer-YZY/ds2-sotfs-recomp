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
#include <jni.h>

#include <SDL3/SDL_gamepad.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/input/input_driver.h>
#include <rex/input/input_system.h>
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

// Options from the app's CTRL dialog. None are wired yet; they are logged so the plumbing can be checked.
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
  } else if (key == "dof") {
    ds2::effects::SetDepthOfField(val == "1" || val == "true");
  } else if (key == "eco") {
    ds2::effects::SetEcoMode(val == "1" || val == "true");
  } else {
    REXLOG_INFO("ds2: option {} = {} (not wired)", key, val);
  }
  if (n) env->ReleaseStringUTFChars(name, n);
  if (v) env->ReleaseStringUTFChars(value, v);
}

#endif  // __ANDROID__
