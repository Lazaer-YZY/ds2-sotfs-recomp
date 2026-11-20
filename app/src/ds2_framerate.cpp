// ds2 - frame rate options, see ds2_framerate.h.

#include "ds2_framerate.h"
#if defined(__ANDROID__)
#include "android/ds2_android.h"
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <string>

#if defined(DS2_PGO_GENERATE) || defined(DS2_PGO_SDK_GENERATE)
#include <dlfcn.h>
#include <sys/stat.h>
#endif
#if defined(DS2_PGO_GENERATE)
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);
#endif

namespace ds2::framerate {

namespace {
std::atomic<bool> g_vblank120{false};
std::string g_refresh_before = "60";

const char* Env(const char* name, const char* fallback) {
  const char* v = std::getenv(name);
  return v && v[0] ? v : fallback;
}

// Vertical blanks per game frame for a frame rate on this start's vertical blank rate.
int VblanksFor(std::string_view fps, bool vblank120) {
  if (vblank120) return fps == "40" ? 3 : fps == "30" ? 4 : 2;
  return fps == "30" || fps == "40" ? 2 : 1;  // 40 needs the 120 Hz vertical blank: 30 until the next start
}
}  // namespace

void Startup() {
  if (std::string_view(Env("DS2_VBLANK120", "0")) != "1") return;
  g_refresh_before = rex::cvar::GetFlagByName("video_mode_refresh_rate");
  if (rex::cvar::SetFlagByName("video_mode_refresh_rate", "120")) {
    g_vblank120 = true;
    REXLOG_INFO("ds2 frame rate: vertical blank at 120 Hz (was {})", g_refresh_before);
  } else {
    REXLOG_WARN("ds2 frame rate: the runtime refused video_mode_refresh_rate 120; staying at 60 Hz");
  }
}

#if defined(DS2_PGO_GENERATE) || defined(DS2_PGO_SDK_GENERATE)
// Instrumented build (DS2_PGO=generate and/or DS2_PGO_SDK=generate): the counters accumulate in memory; every 30 s
// they are written to <app files dir>/pgo/ds2.profraw (game code) and rexgpu.profraw (the GPU plugin, through its
// exported rexgpu_pgo_write), so the last write holds the whole session.
void StartPgoWriter() {
  const char* dir = std::getenv("REX_ANDROID_APP_DIR");
  if (!dir) return;
  static std::string path, gpu_path;
  const std::string pgo = std::string(dir) + "pgo";
  mkdir(pgo.c_str(), 0770);
  path = pgo + "/ds2.profraw";
  gpu_path = pgo + "/rexgpu.profraw";
  std::thread([] {
    using GpuWrite = int (*)(const char*);
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(30));
#if defined(DS2_PGO_GENERATE)
      __llvm_profile_set_filename(path.c_str());
      const int rc = __llvm_profile_write_file();
      REXLOG_INFO("ds2 PGO: profile written to {} (rc {})", path, rc);
#endif
      if (auto gpu_write = reinterpret_cast<GpuWrite>(dlsym(RTLD_DEFAULT, "rexgpu_pgo_write"))) {
        const int gpu_rc = gpu_write(gpu_path.c_str());
        REXLOG_INFO("ds2 PGO: GPU plugin profile written to {} (rc {})", gpu_path, gpu_rc);
      }
    }
  }).detach();
}
#endif

void AfterSetup() {
#if defined(DS2_PGO_GENERATE) || defined(DS2_PGO_SDK_GENERATE)
  StartPgoWriter();
#endif
  if (g_vblank120) {
    // The vertical blank thread has read 120; the game's VdQueryVideoMode reads the cvar and sees 60 again.
    rex::cvar::SetFlagByName("video_mode_refresh_rate", g_refresh_before.empty() ? "60" : g_refresh_before);
  }
  const std::string fps = Env("DS2_FPS", "60");
  SetFrameRate(fps);
#if defined(__ANDROID__)
  std::thread([sixty = fps == "60"] {
    std::this_thread::sleep_for(std::chrono::seconds(30));
    ds2::android::PlaceMainThread(sixty);
    // DS2_PRIME_ASSIST: 1 = the governor (default), 2 = always allowed (A/B), 0 = never.
    const char* prime = std::getenv("DS2_PRIME_ASSIST");
    const char* affinity = std::getenv("DS2_AFFINITY");
    const char* main_prime = std::getenv("DS2_MAIN_PRIME");
    if ((affinity && affinity[0]) || (main_prime && main_prime[0] && main_prime[0] != '0')) {
      REXLOG_INFO("ds2 prime core: left to DS2_AFFINITY / DS2_MAIN_PRIME");
    } else if (prime && prime[0] == '2') {
      ds2::android::SetPrimeAssist(true);
    } else {
      ds2::android::SetPrimeGovernor(prime && prime[0] == '1');
    }
    ds2::android::BoostCriticalThreads();
    ds2::android::ApplySchedHints();
    ds2::android::StartAffinityKeeper();
  }).detach();
#endif
}

namespace {
std::atomic<double> g_target_period_ms{1000.0 / 60.0};
}  // namespace

double TargetPeriodMs() { return g_target_period_ms.load(std::memory_order_relaxed); }

void SetFrameRate(std::string_view fps) {
  g_target_period_ms.store(fps == "30" ? 1000.0 / 30.0 : fps == "40" ? 1000.0 / 40.0 : 1000.0 / 60.0,
                           std::memory_order_relaxed);
  const int vblanks = VblanksFor(fps, g_vblank120.load());
  if (!rex::cvar::SetFlagByName("swap_min_vblanks", std::to_string(vblanks))) {
    REXLOG_WARN("ds2 frame rate: no swap_min_vblanks in this runtime; frame rate {} not applied", fps);
    return;
  }
  REXLOG_INFO("ds2 frame rate: {} fps ({} vertical blanks per frame at {} Hz)", fps, vblanks,
              g_vblank120.load() ? 120 : 60);
}

}  // namespace ds2::framerate
