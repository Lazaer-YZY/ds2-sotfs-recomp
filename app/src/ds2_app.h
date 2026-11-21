// ds2 - host application class. Starts from the `rexglue init` template; game-specific hooks go here.

#pragma once

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include "ds2_cheats.h"
#include "ds2_hp_probe.h"
#include "ds2_capture.h"
#include "ds2_cpu_settings.h"
#include "ds2_dlc.h"
#include "ds2_effects.h"
#include "ds2_frame_overlay.h"
#include "ds2_framerate.h"
#include "ds2_input_script.h"
#include "ds2_watchdog.h"
#if defined(__ANDROID__)
#include "android/ds2_android.h"
#endif

#include <filesystem>
#include <string>
#include <system_error>

class Ds2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
#if defined(__ANDROID__)
    // Before any runtime thread exists: every thread created from here on inherits the mask.
    ds2::android::PinThreadsToFastCores();
#endif
    return std::unique_ptr<Ds2App>(new Ds2App(ctx, "ds2", PPCImageConfig));
  }

 protected:
  // Portable layout: with no --game_data_root / --user_data_root, use the port folder (next to the executable on a
  // PC; on Android rex::filesystem::GetExecutableFolder() is the app's files dir, REX_ANDROID_APP_DIR):
  //   game_root/   the game folder (Default.xex, GameData*.bdt, ...)
  //   user/        saves and installed content (created by the runtime)
  // The Android app passes --game_data_root / --user_data_root explicitly (android/README.md).
  // Runs before ds2.toml is read, so only the command line overrides it (SDK ordering, sdk/src/ui/rex_app.cpp).
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto dir = rex::filesystem::GetExecutableFolder();
    if (dir.empty()) return;
    std::error_code ec;
    if (paths.game_data_root.empty() && std::filesystem::is_directory(dir / "game_root", ec))
      paths.game_data_root = dir / "game_root";
    if (rex::cvar::GetFlagByName("user_data_root").empty()) {
      paths.user_data_root = dir / "user";
      if (rex::cvar::GetFlagByName("cache_root").empty()) paths.cache_root = dir / "user" / "cache";
    }
  }

  // Without a GPU plugin the runtime drops the game's graphics ("no GPU emulation loaded"); default to xenos
  // unless the command line or ds2.toml chose otherwise.
  void OnPostInitLogging() override {
    SetIfDefault("gpu_plugin", "xenos");
    ds2::framerate::Startup();  // 120 Hz vertical blank for 40 fps / Smoother 60 (before the runtime is set up)
#if defined(__ANDROID__)
    ds2::android::ApplyDefaults();
#endif
    ResolveShaderLibrary();
    ds2::cpu_settings::Configure();
  }

  // Optional offline SPIR-V pack for the shader translator (sdk spirv_library): ds2_shader_library names it,
  // relative to the port folder. If that file is missing, the newest ds2_shaders_vNNN.spvlib (+ .idx) is used.
  // Without any pack, shaders are translated at run time.
  static void ResolveShaderLibrary() {
    if (rex::cvar::GetFlagInfo("ds2_shader_library") == nullptr) return;
    const auto dir = rex::filesystem::GetExecutableFolder();
    if (dir.empty()) return;
    std::error_code ec;
    auto usable = [](const std::filesystem::path& p) {
      std::error_code e;
      auto idx = p;
      idx += ".idx";
      return std::filesystem::is_regular_file(p, e) && std::filesystem::is_regular_file(idx, e);
    };
    std::string name = rex::cvar::GetFlagByName("ds2_shader_library");
    if (name.empty()) name = "ds2_shaders.spvlib";
    std::filesystem::path wanted(name);
    if (wanted.is_relative()) wanted = dir / wanted;
    if (usable(wanted)) {
      rex::cvar::SetFlagByName("ds2_shader_library", name);
      return;
    }
    std::filesystem::path best;
    int best_version = -1;
    for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator();
         it.increment(ec)) {
      const std::string file = it->path().filename().string();
      int version = -1;
      if (file == "ds2_shaders.spvlib") {
        version = 1 << 30;
      } else if (file.rfind("ds2_shaders_v", 0) == 0 && file.size() > 20 &&
                 file.compare(file.size() - 7, 7, ".spvlib") == 0) {
        const std::string digits = file.substr(13, file.size() - 20);
        if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos && digits.size() < 9)
          version = std::stoi(digits);
      }
      if (version > best_version && usable(it->path())) {
        best_version = version;
        best = it->path();
      }
    }
    if (best.empty()) {
      REXLOG_INFO("ds2: no shader pack ({} or ds2_shaders*.spvlib + .idx); shaders are translated at run time", name);
      return;
    }
    const std::string found = best.filename().string();
    rex::cvar::SetFlagByName("ds2_shader_library", found);
    REXLOG_INFO("ds2: shader pack {} not found, using {}", name, found);
  }

  static void SetIfDefault(const char* name, const char* value) {
    if (rex::cvar::GetFlagInfo(name) == nullptr || rex::cvar::HasNonDefaultValue(name)) return;
    if (rex::cvar::SetFlagByName(name, value)) REXLOG_INFO("ds2: default {} = {}", name, value);
  }

  // The SDK starts game:\default.xex and checks it on the host file system before the (case-insensitive) VFS
  // sees it (sdk/src/ui/rex_app.cpp). The DS2 disc has it as Default.xex, so on a case-sensitive host
  // (Linux, Android) the lowercase name is not found.
  void OnLoadXexImage(std::string& xex_image) override { xex_image = "game:\\Default.xex"; }

  // Automated tests: scripted virtual pad (off unless --ds2_input_script is given), see ds2_input_script.h.
  void OnPreSetup(rex::RuntimeConfig& config) override {
#if defined(__ANDROID__)
    // Runs before the GPU plugin loads, i.e. before any Vulkan instance exists.
    ds2::android::InstallGraphicsFailureSaver();
    ds2::android::LoadCustomVulkanDriver();
    ds2::android::WrapTouchInput(config);
#endif
    ds2::input_script::Wrap(config);
  }

  void OnSetupFailed() override {
#if defined(__ANDROID__)
    ds2::android::ReportGraphicsFailure();
#endif
  }

  void OnPostSetup() override {
    ds2::input_script::ApplyTimeScalar();
    ds2::effects::Apply();  // optional post effects (depth of field off by default)
    ds2::framerate::AfterSetup();  // the game sees 60 Hz again; frame cap
  }

  // DLC packages (dlc/ in the port folder) are unpacked on a worker thread on the first start while the UI thread
  // draws a progress bar; the game is launched when it is done.
  void LaunchModule() override {
    auto post = [this](std::function<void()> f) { app_context().CallInUIThreadDeferred(std::move(f)); };
    ds2::dlc::InstallWithProgress(runtime(), imgui_drawer(), post, [this]() { rex::ReXApp::LaunchModule(); });
  }

  void OnPostLaunchModule(rex::system::XThread* thread) override {
    (void)thread;
#if defined(__ANDROID__)
    ds2::android::NoteModuleLaunched();
    ds2::android::StartSaveSnapshots();  // development builds only
    ds2::android::StartGpuCounters();    // debug launches with DS2_GPU_COUNTERS=1 only
#endif
    // Performance overlay (CTRL dialog on Android; DS2_FRAME_OVERLAY=1): created on the UI thread when asked for.
    ds2::frame_overlay::Init(imgui_drawer(), [this](std::function<void()> f) {
      app_context().CallInUIThreadDeferred(std::move(f));
    });
    ds2::capture::Start(runtime());   // diagnostic frame capture, off unless --ds2_capture_dir is given
    ds2::watchdog::Start(runtime());  // load/hang telemetry in the log
    ds2::hp_probe::Start(runtime());  // bench helper, DS2_HP_PROBE only
    ds2::cheats::Start(runtime());    // CTRL cheats (idle while all are off)
  }
  void OnShutdown() override {
    ds2::frame_overlay::Shutdown();
    ds2::watchdog::Stop();
    ds2::capture::Stop();
  }
};
