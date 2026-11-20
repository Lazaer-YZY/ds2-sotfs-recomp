// ds2 - scheduler hints for the threads every frame waits on (experiments, off unless their variable is set).
//
// The owner's busy Majula play (2026-11-20): the game's job workers and Havok threads queued 9-10 ms per frame for a core
// while the process used ~2 of the 5 cores it may use. Qualcomm's core_ctl keeps 3 of the 4 middle cores active
// (min_cpus 3; the 4th only above 60 % cluster busy or 4 runnable tasks on average) and the prime core paused unless a
// big task needs it, and the energy-aware scheduler sees short bursts as small tasks. Two ways an app can tell the
// scheduler those threads matter:
// - DS2_UCLAMP_MIN=<1-1024>: sched_setattr's uclamp.min on the job workers, Havok threads, main, GPU command and draw
//   threads (the scheduler then treats each as needing at least that share of the biggest core's capacity).
// - DS2_ADPF=1: an Android performance hint session (APerformanceHint, Android 13) with the same threads; each frame
//   reports the main thread's busy time against DS2_ADPF_TARGET_PCT (75) % of the frame period.
#include "android/ds2_android.h"

#include "ds2_framerate.h"

#include <rex/logging.h>

#include <dirent.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern "C" std::atomic<int> rex_swap_thread_tid;

namespace ds2::android {
namespace {

// The threads a frame waits on, by name (comm is truncated to 15 characters).
std::vector<int> FrameThreads() {
  std::vector<int> tids;
  const int main_tid = rex_swap_thread_tid.load(std::memory_order_relaxed);
  if (main_tid) tids.push_back(main_tid);
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return tids;
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    char path[64], comm[32] = {};
    std::snprintf(path, sizeof(path), "/proc/self/task/%s/comm", e->d_name);
    FILE* f = std::fopen(path, "r");
    if (!f) continue;
    const size_t n = std::fread(comm, 1, sizeof(comm) - 1, f);
    std::fclose(f);
    comm[n] = 0;
    static const char* const kNames[] = {"MtTaskWorkerThr", "Havok Job Threa", "GPU Commands", "DrawCommandThre",
                                         "Presenter Paint"};
    for (const char* name : kNames) {
      if (std::strncmp(comm, name, std::strlen(name)) == 0) {
        const int tid = std::atoi(e->d_name);
        if (tid != main_tid) tids.push_back(tid);
        break;
      }
    }
  }
  closedir(dir);
  return tids;
}

struct SchedAttr {
  uint32_t size;
  uint32_t sched_policy;
  uint64_t sched_flags;
  int32_t sched_nice;
  uint32_t sched_priority;
  uint64_t sched_runtime, sched_deadline, sched_period;
  uint32_t sched_util_min, sched_util_max;
};
constexpr uint64_t kKeepPolicy = 0x08, kKeepParams = 0x10, kUtilClampMin = 0x20;

void ApplyUclampMin(const std::vector<int>& tids, int value) {
  int ok = 0, failed = 0, last_errno = 0;
  for (int tid : tids) {
    SchedAttr attr{};
    attr.size = sizeof(attr);
    attr.sched_flags = kKeepPolicy | kKeepParams | kUtilClampMin;
    attr.sched_util_min = uint32_t(value);
    if (syscall(__NR_sched_setattr, tid, &attr, 0) == 0) {
      ++ok;
    } else {
      ++failed;
      last_errno = errno;
    }
  }
  REXLOG_INFO("ds2 sched hints: uclamp.min {} on {} of {} frame threads{}", value, ok, tids.size(),
              failed ? fmt::format(" ({} failed: {})", failed, std::strerror(last_errno)) : std::string());
}

// APerformanceHint (libandroid, API 33), looked up at run time: the app's minimum API level is 26.
using GetManagerFn = void* (*)();
using CreateSessionFn = void* (*)(void*, const int32_t*, size_t, int64_t);
using ReportFn = int (*)(void*, int64_t);
std::atomic<void*> g_session{nullptr};
ReportFn g_report = nullptr;
double g_target_fraction = 0.75;

void StartAdpf(const std::vector<int>& tids) {
  void* lib = dlopen("libandroid.so", RTLD_NOW);
  auto get_manager = lib ? reinterpret_cast<GetManagerFn>(dlsym(lib, "APerformanceHint_getManager")) : nullptr;
  auto create = lib ? reinterpret_cast<CreateSessionFn>(dlsym(lib, "APerformanceHint_createSession")) : nullptr;
  g_report = lib ? reinterpret_cast<ReportFn>(dlsym(lib, "APerformanceHint_reportActualWorkDuration")) : nullptr;
  if (!get_manager || !create || !g_report) {
    REXLOG_WARN("ds2 sched hints: no APerformanceHint in libandroid (Android 13+)");
    return;
  }
  void* manager = get_manager();
  if (const char* pct = std::getenv("DS2_ADPF_TARGET_PCT"); pct && std::atoi(pct) > 0) {
    g_target_fraction = std::atoi(pct) / 100.0;
  }
  std::vector<int32_t> ids(tids.begin(), tids.end());
  const int64_t target_ns = int64_t(ds2::framerate::TargetPeriodMs() * g_target_fraction * 1e6);
  void* session = manager ? create(manager, ids.data(), ids.size(), target_ns) : nullptr;
  if (!session) {
    REXLOG_WARN("ds2 sched hints: the device created no performance hint session (manager {}, {} threads)",
                manager ? "yes" : "no", ids.size());
    return;
  }
  g_session.store(session, std::memory_order_release);
  REXLOG_INFO("ds2 sched hints: performance hint session with {} threads, target {:.1f} ms", ids.size(),
              target_ns / 1e6);
}

}  // namespace

void ApplySchedHints() {
  const char* uclamp = std::getenv("DS2_UCLAMP_MIN");
  const char* adpf = std::getenv("DS2_ADPF");
  const bool want_uclamp = uclamp && std::atoi(uclamp) > 0;
  const bool want_adpf = adpf && adpf[0] == '1';
  if (!want_uclamp && !want_adpf) return;
  const std::vector<int> tids = FrameThreads();
  if (want_uclamp) ApplyUclampMin(tids, std::min(1024, std::atoi(uclamp)));
  if (want_adpf) StartAdpf(tids);
}

void SchedHintsOnFrame(double busy_ms) {
  void* session = g_session.load(std::memory_order_acquire);
  if (!session) return;
  g_report(session, int64_t(std::max(busy_ms, 0.1) * 1e6));
}

}  // namespace ds2::android
