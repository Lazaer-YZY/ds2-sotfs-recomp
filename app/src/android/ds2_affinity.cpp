// ds2 - CPU placement of the game's threads on Android, see ds2_android.h (adapted from the Lightning Returns port,
// where both rules were measured on the same AYN Thor).
//
// Snapdragon 8 Gen 2: cpus 0-2 Cortex-A510 (efficiency), 3-6 A715/A710, 7 X3 (prime).
// 1. Every thread the process creates from start-up on stays off the efficiency cores and off a lone prime core: the
//    middle cluster (LR: worker threads on the little cores stretched the jobs the main thread waits for; with the
//    prime core allowed the threads straddled two speeds; middle only paced every frame, -22 points of process CPU).
// 2. The guest main thread stays there too. LR let it use the prime core at 60 fps (prime + middle; never the prime
//    core alone: core_ctl parks it and a thread allowed only there waits for it, 50-65 ms bursts), but DS2's main
//    thread needs ~4.6 ms per frame: off the prime core, 60 fps held with no frame over 17 ms, 8.32 -> 8.14 W, CPU
//    peak 89.6 -> 87.6 C (2026-11-18). DS2_AFFINITY (hex mask, "all") and DS2_MAIN_PRIME (0 / 1 / 2) override.

#include "android/ds2_android.h"

#include <rex/logging.h>

#include <android/log.h>
#include <dirent.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <set>
#include <map>
#include <thread>

#include "ds2_framerate.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace ds2::android {

namespace {

std::vector<long> CpuMaxKhz() {
  std::vector<long> max_khz;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    char path[96];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
    FILE* f = std::fopen(path, "r");
    if (!f) break;
    long khz = 0;
    if (std::fscanf(f, "%ld", &khz) != 1) khz = 0;
    std::fclose(f);
    max_khz.push_back(khz);
  }
  return max_khz;
}

std::string CpuList(const cpu_set_t& set) {
  std::string cpus;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &set)) cpus += (cpus.empty() ? "" : ",") + std::to_string(cpu);
  }
  return cpus;
}

}  // namespace

namespace {
// The mask every game thread was given at startup (PinThreadsToFastCores), for the keeper below; unset when the game
// runs on all cores.
std::atomic<bool> g_default_mask_set{false};
cpu_set_t g_default_mask;
}  // namespace

std::string PinThreadsToFastCores() {
  const char* env = std::getenv("DS2_AFFINITY");
  const std::string want = env ? env : "";
  if (want == "all" || want == "0") return "all cores (DS2_AFFINITY=" + want + ")";
  cpu_set_t set;
  CPU_ZERO(&set);
  if (!want.empty()) {
    const unsigned long long mask = std::strtoull(want.c_str(), nullptr, 16);
    for (int cpu = 0; cpu < 64 && cpu < CPU_SETSIZE; ++cpu) {
      if ((mask >> cpu) & 1) CPU_SET(cpu, &set);
    }
  } else {
    const std::vector<long> max_khz = CpuMaxKhz();
    if (max_khz.empty()) return "all cores (no cpufreq info)";
    const long slowest = *std::min_element(max_khz.begin(), max_khz.end());
    const long fastest = *std::max_element(max_khz.begin(), max_khz.end());
    int fastest_count = 0, middle_count = 0;
    for (long khz : max_khz) {
      fastest_count += khz == fastest;
      middle_count += khz > slowest && khz < fastest;
    }
    // Skip the prime tier only when it is small (at most 2 CPUs) and the middle tier keeps at least 4.
    const bool skip_prime = fastest > slowest && fastest_count <= 2 && middle_count >= 4;
    for (size_t cpu = 0; cpu < max_khz.size(); ++cpu) {
      if (max_khz[cpu] > slowest && !(skip_prime && max_khz[cpu] == fastest)) CPU_SET(int(cpu), &set);
    }
    // Fewer than 4 fast cores (2 big + 6 little: Dimensity 700-1080, Helio G99, Exynos 1280): the main, GPU
    // command, draw, job, audio and loader threads would all share two cores. Leave every core to the scheduler,
    // which keeps the busiest threads on the big ones.
    if (CPU_COUNT(&set) > 0 && CPU_COUNT(&set) < 4) {
      const std::string note = "all cores (only " + std::to_string(CPU_COUNT(&set)) + " fast ones: cpus " +
                               CpuList(set) + ")";
      __android_log_print(ANDROID_LOG_INFO, "ds2recomp", "game threads on %s", note.c_str());
      return note;
    }
  }
  if (CPU_COUNT(&set) == 0) return "all cores (one cluster or empty mask)";
  std::string note;
  if (sched_setaffinity(0, sizeof(set), &set) != 0) {
    note = "all cores (sched_setaffinity failed: " + std::string(std::strerror(errno)) + ")";
  } else {
    note = "cpus " + CpuList(set) + (want.empty() ? " (middle cluster)" : " (DS2_AFFINITY)");
    g_default_mask = set;
    g_default_mask_set = true;
  }
  __android_log_print(ANDROID_LOG_INFO, "ds2recomp", "game threads on %s", note.c_str());
  return note;
}

void PlaceMainThread(bool sixty_fps) {
  (void)sixty_fps;
  int mode = 0;  // middle cluster like every other thread (see 2. above); DS2_MAIN_PRIME=2 restores prime + middle
  if (const char* v = std::getenv("DS2_MAIN_PRIME"); v && v[0]) mode = v[0] - '0';
  if (mode != 1 && mode != 2) return;
  const std::vector<long> max_khz = CpuMaxKhz();
  if (max_khz.empty()) return;
  const long fastest = *std::max_element(max_khz.begin(), max_khz.end());
  const long slowest = *std::min_element(max_khz.begin(), max_khz.end());
  cpu_set_t set;
  CPU_ZERO(&set);
  for (size_t cpu = 0; cpu < max_khz.size(); ++cpu) {
    if (mode == 1 ? max_khz[cpu] == fastest : (max_khz[cpu] > slowest || slowest == fastest)) CPU_SET(int(cpu), &set);
  }
  // The busiest thread whose name starts with "Main XThread" is the game's main thread.
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return;
  int best_tid = 0;
  unsigned long long best_ticks = 0;
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    char path[64], stat[512] = {};
    std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);
    FILE* f = std::fopen(path, "r");
    if (!f) continue;
    const size_t n = std::fread(stat, 1, sizeof(stat) - 1, f);
    std::fclose(f);
    stat[n] = 0;
    const char* open = std::strchr(stat, '(');
    const char* close = std::strrchr(stat, ')');
    if (!open || !close || std::strncmp(open + 1, "Main XThread", 12) != 0) continue;
    // After ") ": state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime.
    unsigned long long utime = 0, stime = 0;
    if (std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) != 2) continue;
    if (utime + stime >= best_ticks) {
      best_ticks = utime + stime;
      best_tid = std::atoi(e->d_name);
    }
  }
  closedir(dir);
  if (!best_tid) {
    REXLOG_INFO("ds2 affinity: no Main XThread found");
    return;
  }
  if (sched_setaffinity(best_tid, sizeof(set), &set) == 0) {
    REXLOG_INFO("ds2 affinity: main thread {} on cpus {} ({})", best_tid, CpuList(set),
                mode == 1 ? "prime only" : "prime + middle");
  } else {
    REXLOG_WARN("ds2 affinity: main thread {}: sched_setaffinity failed: {}", best_tid, std::strerror(errno));
  }
}

extern "C" std::atomic<int> rex_swap_thread_tid;

namespace {
// The threads SetPrimeAssist moves and the two placements. Searched again whenever the governor is configured (off the
// main thread) and when the presenting thread is no longer the cached main thread.
struct PrimeThreads {
  std::vector<int> tids;
  int main_tid = 0;
  cpu_set_t with_prime, middle;
  bool ready = false;
};
std::mutex g_prime_mutex;
PrimeThreads g_prime_threads;
// Threads that already had exactly the prime + middle mask when the governor was set up (not created under it: some
// Android / driver threads); the switch-off reset leaves them alone.
std::vector<int> g_prime_preexisting;

bool IsPrimeCpu(const cpu_set_t& with_prime, const cpu_set_t& middle, int cpu) {
  return CPU_ISSET(cpu, &with_prime) && !CPU_ISSET(cpu, &middle);
}

bool FindPrimeThreads(PrimeThreads& p) {
  p.ready = false;
  const std::vector<long> max_khz = CpuMaxKhz();
  if (max_khz.empty()) return false;
  const long fastest = *std::max_element(max_khz.begin(), max_khz.end());
  const long slowest = *std::min_element(max_khz.begin(), max_khz.end());
  int fastest_count = 0, middle_count = 0;
  for (long khz : max_khz) {
    fastest_count += khz == fastest;
    middle_count += khz > slowest && khz < fastest;
  }
  // Only with a small prime tier kept off by PinThreadsToFastCores (at most 2 CPUs, at least 4 in the middle tier);
  // on other layouts the game threads already run on every fast core and there is nothing to switch.
  if (!(fastest > slowest && fastest_count <= 2 && middle_count >= 4)) return false;
  CPU_ZERO(&p.with_prime);
  CPU_ZERO(&p.middle);
  for (size_t cpu = 0; cpu < max_khz.size(); ++cpu) {
    if (max_khz[cpu] > slowest) CPU_SET(int(cpu), &p.with_prime);
    if (max_khz[cpu] > slowest && max_khz[cpu] < fastest) CPU_SET(int(cpu), &p.middle);
  }
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return false;
  p.tids.clear();
  // The main thread: the one presenting the game's frames (rex_swap_thread_tid); before the first frame, the busiest
  // "Main XThread" (threads the main thread creates inherit its name).
  const int swap_tid = rex_swap_thread_tid.load(std::memory_order_relaxed);
  p.main_tid = 0;
  int busiest = 0;
  unsigned long long main_ticks = 0;
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    const int tid = std::atoi(e->d_name);
    char path[64], stat[512] = {};
    std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);
    FILE* f = std::fopen(path, "r");
    if (!f) continue;
    const size_t n = std::fread(stat, 1, sizeof(stat) - 1, f);
    std::fclose(f);
    stat[n] = 0;
    const char* open = std::strchr(stat, '(');
    const char* close = std::strrchr(stat, ')');
    if (!open || !close) continue;
    if (tid == swap_tid) p.main_tid = tid;
    if (std::strncmp(open + 1, "GPU Commands", 12) == 0 || std::strncmp(open + 1, "DrawCommandThre", 15) == 0 ||
        std::strncmp(open + 1, "Presenter Paint", 15) == 0) {
      p.tids.push_back(tid);
      continue;
    }
    if (std::strncmp(open + 1, "Main XThread", 12) != 0) continue;
    unsigned long long utime = 0, stime = 0;
    if (std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) != 2) continue;
    if (utime + stime >= main_ticks) {
      main_ticks = utime + stime;
      busiest = tid;
    }
  }
  closedir(dir);
  if (!p.main_tid) p.main_tid = busiest;
  if (p.main_tid) p.tids.push_back(p.main_tid);
  p.ready = !p.tids.empty();
  return p.ready;
}

// After a switch back to the middle cluster (on a helper thread: a /proc scan): threads that one of the moved threads
// created while the prime core was allowed inherited that mask; put every such thread back on the middle cluster.
std::vector<int> ThreadsWithMask(const cpu_set_t& want) {
  std::vector<int> tids;
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return tids;
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    const int tid = std::atoi(e->d_name);
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (sched_getaffinity(tid, sizeof(mask), &mask) == 0 && CPU_EQUAL(&mask, &want)) tids.push_back(tid);
  }
  closedir(dir);
  return tids;
}

void ResetInheritedPrime(cpu_set_t with_prime, cpu_set_t middle, std::vector<int> moved, std::vector<int> keep) {
  std::thread([with_prime, middle, moved, keep] {
    DIR* dir = opendir("/proc/self/task");
    if (!dir) return;
    int reset = 0;
    std::string names;
    while (dirent* e = readdir(dir)) {
      if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
      const int tid = std::atoi(e->d_name);
      if (std::find(moved.begin(), moved.end(), tid) != moved.end()) continue;
      if (std::find(keep.begin(), keep.end(), tid) != keep.end()) continue;
      cpu_set_t mask;
      CPU_ZERO(&mask);
      if (sched_getaffinity(tid, sizeof(mask), &mask) != 0) continue;
      bool has_prime = false;
      for (int cpu = 0; cpu < CPU_SETSIZE && !has_prime; ++cpu) {
        has_prime = CPU_ISSET(cpu, &mask) && IsPrimeCpu(with_prime, middle, cpu);
      }
      // Only masks the governor can have produced (the middle cluster + the prime core); others are left alone.
      if (has_prime && CPU_EQUAL(&mask, &with_prime) && sched_setaffinity(tid, sizeof(middle), &middle) == 0) {
        ++reset;
        char path[64], comm[32] = {};
        std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
        if (FILE* f = std::fopen(path, "r")) {
          const size_t n = std::fread(comm, 1, sizeof(comm) - 1, f);
          std::fclose(f);
          comm[n] = 0;
          if (n && comm[n - 1] == '\n') comm[n - 1] = 0;
        }
        names += std::string(names.empty() ? "" : ", ") + comm;
      }
    }
    closedir(dir);
    if (reset) {
      REXLOG_INFO("ds2 affinity: prime core off: {} threads created meanwhile back on the middle cluster: {}", reset,
                  names);
    }
  }).detach();
}
}  // namespace

namespace {
void PlacePrimeThreads(bool on, bool reset_inherited) {
  std::lock_guard<std::mutex> lock(g_prime_mutex);
  PrimeThreads& p = g_prime_threads;
  const int swap_tid = rex_swap_thread_tid.load(std::memory_order_relaxed);
  if ((!p.ready || (swap_tid && swap_tid != p.main_tid)) && !FindPrimeThreads(p)) return;
  int placed = 0;
  for (int tid : p.tids) placed += sched_setaffinity(tid, sizeof(cpu_set_t), on ? &p.with_prime : &p.middle) == 0;
  if (placed < int(p.tids.size()) && FindPrimeThreads(p)) {  // a thread went away: search again once
    placed = 0;
    for (int tid : p.tids) placed += sched_setaffinity(tid, sizeof(cpu_set_t), on ? &p.with_prime : &p.middle) == 0;
  }
  if (!on && reset_inherited) ResetInheritedPrime(p.with_prime, p.middle, p.tids, g_prime_preexisting);
  REXLOG_INFO("ds2 affinity: prime core {}: {} of {} threads (main {}, GPU commands, draw) on cpus {}",
              on ? "on" : "off", placed, p.tids.size(), p.main_tid, CpuList(on ? p.with_prime : p.middle));
}
}  // namespace

void SetPrimeAssist(bool on) { PlacePrimeThreads(on, false); }

// The prime-core governor ("Use the prime core when the game is busy", DS2_PRIME_ASSIST=1, the default): the X3 is
// allowed only while the game's main thread is near its frame budget. Measured on the Forest snapshot fight
// (2026-11-20): prime allowed always = main thread 6.6 instead of 8.0 ms CPU per frame and the job workers'
// run-queue wait 3.6 -> 1.3 ms per frame, but +1.0 W in a scene with slack (the scheduler moves a thread that busy
// to the biggest core whether or not the frame needs it). Decided per game frame on the main thread (the SDK calls
// rex_swap_observer from VdSwap): busy = the frame's interval - the frame cap's wait - the main thread's time in the
// game's GPU waits (ds2_gpu_wait.cpp). Wall time on purpose, not the thread's CPU time: the main thread's waits for
// the job workers queueing for the middle cores are what the X3 relieves (the owner's busy frames: ~9 ms CPU + ~7 ms of
// such waits). On after one such frame over 1.5x the target period (a hitch: the X3 is there for the next) or a
// 6-frame average over 84 % of it (14.0 ms at 60 fps); off after 10 s of 6-frame averages below 69 % (11.5 ms). The
// X3 saves ~1.4 ms, less than the gap, so it does not flip back and forth; a GPU-bound frame (its time in the GPU
// wait) does not count. Window averages over 100 ms (the first version) mixed a CPU hitch with GPU-bound frames
// around it and never switched on in the sound-zone walk.
extern "C" std::atomic<uint64_t> ds2_main_gpu_wait_ns;
extern "C" std::atomic<void (*)(uint64_t interval_ns, uint64_t cap_ns)> rex_swap_observer;

namespace {
std::atomic<bool> g_governor_allowed{false};
std::atomic<bool> g_prime_on{false};

void OnSwap(uint64_t interval_ns, uint64_t cap_ns) {
  static uint64_t gpu_last = ds2_main_gpu_wait_ns.load(std::memory_order_relaxed);
  static double recent[6] = {};
  static int recent_pos = 0;
  static double calm_ms = 0;
  static uint64_t frames = 0, on_frames = 0;
  const uint64_t gpu = ds2_main_gpu_wait_ns.load(std::memory_order_relaxed);
  const double gpu_ms = double(gpu - gpu_last) / 1e6;
  gpu_last = gpu;
  const double interval_ms = double(interval_ns) / 1e6;
  if (interval_ms > 1000.0) return;  // the first frame after a pause or a load screen without swaps
  const double cpu_ms = std::max(0.0, interval_ms - double(cap_ns) / 1e6 - gpu_ms);
  SchedHintsOnFrame(cpu_ms);
  recent[recent_pos++ % 6] = cpu_ms;
  double avg = 0;
  for (double v : recent) avg += v;
  avg /= 6.0;
  const double target = ds2::framerate::TargetPeriodMs();
  ++frames;
  if (!g_governor_allowed.load(std::memory_order_relaxed)) {
    if (g_prime_on.exchange(false)) PlacePrimeThreads(false, true);
    calm_ms = 0;
    return;
  }
  if (!g_prime_on.load(std::memory_order_relaxed)) {
    if (cpu_ms > 1.5 * target || avg > 0.84 * target) {
      g_prime_on = true;
      SetPrimeAssist(true);
      REXLOG_INFO("ds2 prime core: on (frame {:.1f} ms, busy {:.1f} ms, 6-frame average {:.1f} ms, target {:.1f})",
                  interval_ms, cpu_ms, avg, target);
      calm_ms = 0;
    }
    return;
  }
  ++on_frames;
  calm_ms = avg < 0.69 * target ? calm_ms + interval_ms : 0;
  if (calm_ms >= 10000.0) {
    g_prime_on = false;
    PlacePrimeThreads(false, true);
    REXLOG_INFO("ds2 prime core: off (6-frame busy average {:.1f} ms for 10 s; on {:.0f} % of the frames so far)",
                avg, 100.0 * double(on_frames) / double(frames));
    calm_ms = 0;
  }
}
}  // namespace

void SetPrimeGovernor(bool allowed) {
  // Called at start (a helper thread) and from the CTRL dialog (the UI thread), never on the main thread: search the
  // threads again here (a /proc scan) and start from the middle cluster (also after DS2_PRIME_ASSIST=2, "always").
  g_governor_allowed = allowed;
  g_prime_on = false;
  {
    std::lock_guard<std::mutex> lock(g_prime_mutex);
    if (FindPrimeThreads(g_prime_threads) && g_prime_preexisting.empty()) {
      g_prime_preexisting = ThreadsWithMask(g_prime_threads.with_prime);
    }
  }
  PlacePrimeThreads(false, false);
  REXLOG_INFO("ds2 prime core: {}", allowed ? "used when the game is busy (governor)" : "never");
  rex_swap_observer.store(&OnSwap, std::memory_order_release);
}

void BoostCriticalThreads() {
  // DS2_THREAD_NICE (e.g. -4; unset or 0 = off): the nice value for the two threads every frame waits on, the SDK's
  // "GPU Commands" thread and the busiest "Main XThread". Guest thread priorities are ignored on Android
  // (ignore_thread_priorities: SCHED_FIFO needs root), so both otherwise compete equally with the game's task
  // workers, Havok jobs and loaders on the four middle cores when streaming bursts fill them.
  const char* v = std::getenv("DS2_THREAD_NICE");
  const int nice = v && v[0] ? std::atoi(v) : 0;
  if (nice == 0) return;
  DIR* dir = opendir("/proc/self/task");
  if (!dir) return;
  int main_tid = 0;
  unsigned long long main_ticks = 0;
  std::vector<int> gpu_tids;
  while (dirent* e = readdir(dir)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
    char path[64], stat[512] = {};
    std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", e->d_name);
    FILE* f = std::fopen(path, "r");
    if (!f) continue;
    const size_t n = std::fread(stat, 1, sizeof(stat) - 1, f);
    std::fclose(f);
    stat[n] = 0;
    const char* open = std::strchr(stat, '(');
    const char* close = std::strrchr(stat, ')');
    if (!open || !close) continue;
    if (std::strncmp(open + 1, "GPU Commands", 12) == 0) {
      gpu_tids.push_back(std::atoi(e->d_name));
      continue;
    }
    if (std::strncmp(open + 1, "Main XThread", 12) != 0) continue;
    unsigned long long utime = 0, stime = 0;
    if (std::sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &utime, &stime) != 2) continue;
    if (utime + stime >= main_ticks) {
      main_ticks = utime + stime;
      main_tid = std::atoi(e->d_name);
    }
  }
  closedir(dir);
  if (main_tid) gpu_tids.push_back(main_tid);
  for (int tid : gpu_tids) {
    if (setpriority(PRIO_PROCESS, id_t(tid), nice) == 0) {
      REXLOG_INFO("ds2 affinity: thread {} nice {}{}", tid, nice, tid == main_tid ? " (main)" : " (GPU Commands)");
    } else {
      REXLOG_WARN("ds2 affinity: thread {} nice {}: setpriority failed: {}", tid, nice, std::strerror(errno));
    }
  }
}


// Thread masks reset behind our back. Moving a process between cpusets (Android regroups an app, e.g. when the second
// screen's app takes focus, or a power daemon rewrites cpusets) resets every thread's affinity to the cpuset's cores,
// silently dropping PinThreadsToFastCores' mask and the prime governor's. The owner's sessions of 2026-11-20: 30 s after the
// start Android moved the game from /top-app to /foreground (on the Thor while the second screen's app was the top
// app, until he plugged the charger in); every thread's mask became all cores (reported without the cores core_ctl
// had paused: 0-5, 0-4+6), the job, Havok and audio threads ran on the efficiency cores (80 % busy, the jobs at twice
// their CPU time, the main thread waiting ~6 ms a frame) and up to 20 % of frames were late. Once a second a
// low-priority thread looks for game threads whose mask spans both the efficiency and the faster cores (none of ours
// does) and pins them again: the main, GPU command and draw threads to the governor's current mask, every other one to
// the startup mask. Android's own threads (the UI thread, binder, Java runtime, HWUI) and threads with any other mask
// (all efficiency cores, set on purpose) are left alone. cpuset moves are logged.
namespace {
bool IsAndroidThread(int tid, const std::string& comm) {
  if (tid == getpid()) return true;  // the Java main / UI thread
  static const char* const kPrefixes[] = {"binder:", "HwBinder:", "RenderThread", "hwuiTask", "Jit thread pool",
                                          "HeapTaskDaemon", "FinalizerDaemon", "FinalizerWatchd", "ReferenceQueueD",
                                          "Signal Catcher", "perfetto", "ADB-JDWP", "Profile Saver", "Binder:",
                                          "queued-work-loo", "GpuDriverPage", "android.", "VsyncReceiver",
                                          "InsetsAnimation", "Choreographer", "SDLActivity"};
  for (const char* prefix : kPrefixes) {
    if (comm.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

void KeepAffinity() {
  setpriority(PRIO_PROCESS, id_t(gettid()), 10);
  // The efficiency cores (the lowest maximum clock) and the faster ones.
  cpu_set_t little, fast;
  CPU_ZERO(&little);
  CPU_ZERO(&fast);
  {
    const std::vector<long> max_khz = CpuMaxKhz();
    const long slowest = max_khz.empty() ? 0 : *std::min_element(max_khz.begin(), max_khz.end());
    for (size_t cpu = 0; cpu < max_khz.size(); ++cpu) CPU_SET(int(cpu), max_khz[cpu] == slowest ? &little : &fast);
  }
  auto spans_clusters = [&little, &fast](const cpu_set_t& mask) {
    cpu_set_t a, b;
    CPU_AND(&a, &mask, &little);
    CPU_AND(&b, &mask, &fast);
    return CPU_COUNT(&a) > 0 && CPU_COUNT(&b) > 0;
  };
  std::string last_cpuset;
  uint64_t total_fixed = 0;
  bool first_pass = true;  // pins threads created before PinThreadsToFastCores (not resets)
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::vector<int> prime_tids;
    cpu_set_t prime_mask;
    bool prime_known = false;
    {
      std::lock_guard<std::mutex> lock(g_prime_mutex);
      if (g_prime_threads.ready) {
        prime_tids = g_prime_threads.tids;
        prime_mask = g_prime_on.load(std::memory_order_relaxed) ? g_prime_threads.with_prime : g_prime_threads.middle;
        prime_known = true;
      }
    }
    {
      std::string cpuset;
      if (FILE* f = std::fopen("/proc/self/cpuset", "r")) {
        char buf[64] = {};
        if (std::fgets(buf, sizeof(buf), f)) cpuset = buf;
        std::fclose(f);
        while (!cpuset.empty() && (cpuset.back() == '\n' || cpuset.back() == '\r')) cpuset.pop_back();
      }
      if (cpuset != last_cpuset) {
        if (!last_cpuset.empty()) REXLOG_INFO("ds2 affinity: cpuset {} -> {}", last_cpuset, cpuset);
        last_cpuset = cpuset;
      }
    }
    int fixed = 0, failed = 0;
    std::string names;
    if (DIR* dir = opendir("/proc/self/task")) {
      while (dirent* e = readdir(dir)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const int tid = std::atoi(e->d_name);
        cpu_set_t mask;
        CPU_ZERO(&mask);
        if (sched_getaffinity(tid, sizeof(mask), &mask) != 0 || !spans_clusters(mask)) continue;
        char path[64], comm[32] = {};
        std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
        if (FILE* f = std::fopen(path, "r")) {
          const size_t n = std::fread(comm, 1, sizeof(comm) - 1, f);
          std::fclose(f);
          comm[n] = 0;
          if (n && comm[n - 1] == '\n') comm[n - 1] = 0;
        }
        if (IsAndroidThread(tid, comm)) continue;
        const bool governed =
            prime_known && std::find(prime_tids.begin(), prime_tids.end(), tid) != prime_tids.end();
        const cpu_set_t& want = governed ? prime_mask : g_default_mask;
        if (sched_setaffinity(tid, sizeof(want), &want) == 0) {
          ++fixed;
          if (names.size() < 160) names += std::string(names.empty() ? "" : ", ") + comm;
        } else {
          ++failed;
        }
      }
      closedir(dir);
    }
    if (first_pass) {
      first_pass = false;
      if (fixed) REXLOG_INFO("ds2 affinity: keeper pinned {} threads created before the startup pin: {}", fixed, names);
      continue;
    }
    if (fixed || failed) {
      total_fixed += uint64_t(fixed);
      REXLOG_INFO("ds2 affinity: {} game thread masks had been reset (cpuset change), pinned again{}; "
                  "{} could not be pinned; {} so far: {}",
                  fixed, failed ? " (the cpuset excludes the fast cores?)" : "", failed, total_fixed, names);
    }
  }
}
}  // namespace

void StartAffinityKeeper() {
  const char* v = std::getenv("DS2_AFFINITY_KEEPER");
  if (v && v[0] == '0') return;
  if (!g_default_mask_set.load()) return;  // the game runs on all cores: nothing to keep
  std::thread(KeepAffinity).detach();
  REXLOG_INFO("ds2 affinity: keeper on (reset thread masks pinned again once a second)");
}

// Debug logging (the watchdog, every 10 s): where the game's threads ran. The owner's session of 2026-11-20 on battery had the
// efficiency cores 80 % busy, the middle cores 30 % and the prime core idle although the governor allowed it, with
// the job workers at twice their CPU time; plugged in, the same play had the little cores at 20 % and the prime core in
// use. The process's cpuset (and its cpus), core_ctl's active cores, and per thread group: the CPU time in the window
// split by the core each thread last ran on (/proc stat field 39; an approximation) and the affinity masks.
std::string PlacementReport() {
  auto read_line = [](const std::string& path) {
    std::string line;
    if (FILE* f = std::fopen(path.c_str(), "r")) {
      char buf[256] = {};
      if (std::fgets(buf, sizeof(buf), f)) line = buf;
      std::fclose(f);
    }
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    return line;
  };
  const std::string cpuset = read_line("/proc/self/cpuset");
  std::string out = "cpuset " + (cpuset.empty() ? std::string("?") : cpuset) + " (" +
                    (cpuset.empty() ? std::string("?") : read_line("/dev/cpuset" + cpuset + "/cpus")) + ")";
  for (int cpu : {3, 7}) {
    const std::string base = "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/core_ctl/";
    const std::string active = read_line(base + "active_cpus"), need = read_line(base + "need_cpus");
    if (!active.empty()) out += fmt::format("; core_ctl cpu{} active {} need {}", cpu, active, need);
  }
  struct Group {
    const char* label;
    int threads = 0;
    uint64_t ticks = 0;
    uint64_t by_cpu[16] = {};
    std::set<std::string> masks;
  };
  Group groups[] = {{"main"}, {"gpu"}, {"draw"}, {"workers"}, {"havok"}, {"audio"}, {"other"}};
  static std::map<int, uint64_t> last_ticks;
  std::map<int, uint64_t> now_ticks;
  struct Other {
    uint64_t ticks;
    std::string name;
    int cpu;
    std::string mask;
  };
  std::vector<Other> others;
  const int main_tid = rex_swap_thread_tid.load(std::memory_order_relaxed);
  if (DIR* dir = opendir("/proc/self/task")) {
    while (dirent* e = readdir(dir)) {
      if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
      const int tid = std::atoi(e->d_name);
      const std::string stat = read_line(std::string("/proc/self/task/") + e->d_name + "/stat");
      const size_t open = stat.find('('), close = stat.rfind(')');
      if (open == std::string::npos || close == std::string::npos) continue;
      const std::string comm = stat.substr(open + 1, close - open - 1);
      // After ") ": state(3) ... utime(14) stime(15) ... processor(39).
      std::vector<std::string> f;
      size_t pos = close + 2;
      while (pos < stat.size()) {
        const size_t sp = stat.find(' ', pos);
        f.push_back(stat.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos));
        if (sp == std::string::npos) break;
        pos = sp + 1;
      }
      if (f.size() < 37) continue;
      const uint64_t ticks = std::strtoull(f[11].c_str(), nullptr, 10) + std::strtoull(f[12].c_str(), nullptr, 10);
      const int cpu = std::atoi(f[36].c_str());
      now_ticks[tid] = ticks;
      const auto it = last_ticks.find(tid);
      const uint64_t delta = it == last_ticks.end() || ticks < it->second ? 0 : ticks - it->second;
      cpu_set_t mask;
      CPU_ZERO(&mask);
      const std::string mask_list = sched_getaffinity(tid, sizeof(mask), &mask) == 0 ? CpuList(mask) : "?";
      int g = 6;
      if (tid == main_tid) g = 0;
      else if (comm.rfind("GPU Commands", 0) == 0) g = 1;
      else if (comm.rfind("DrawCommandThre", 0) == 0) g = 2;
      else if (comm.rfind("MtTaskWorkerThr", 0) == 0) g = 3;
      else if (comm.rfind("Havok Job Threa", 0) == 0) g = 4;
      else if (comm.rfind("FMOD", 0) == 0 || comm.find("XMA") != std::string::npos ||
               comm.find("udio") != std::string::npos) g = 5;
      Group& gr = groups[g];
      ++gr.threads;
      gr.ticks += delta;
      if (cpu >= 0 && cpu < 16) gr.by_cpu[cpu] += delta;
      if (delta) gr.masks.insert(mask_list);
      if (g == 6 && delta) others.push_back({delta, comm, cpu, mask_list});
    }
    closedir(dir);
  }
  last_ticks = std::move(now_ticks);
  for (const Group& gr : groups) {
    if (!gr.ticks) continue;
    std::string where;
    for (int cpu = 0; cpu < 16; ++cpu) {
      if (gr.by_cpu[cpu]) where += fmt::format("{}{}:{}", where.empty() ? "" : " ", cpu, 100 * gr.by_cpu[cpu] / gr.ticks);
    }
    std::string masks;
    for (const std::string& m : gr.masks) masks += (masks.empty() ? "" : "|") + m;
    // Ticks are 10 ms: per 10 s window, ticks / 10 = % of one core.
    out += fmt::format("; {} {}x {:.0f}% of a core [last cpu %: {}] mask {}", gr.label, gr.threads, gr.ticks / 10.0,
                       where, masks);
  }
  std::sort(others.begin(), others.end(), [](const Other& a, const Other& b) { return a.ticks > b.ticks; });
  for (size_t i = 0; i < others.size() && i < 4; ++i) {
    out += fmt::format("; '{}' {:.0f}% cpu{} mask {}", others[i].name, others[i].ticks / 10.0, others[i].cpu,
                       others[i].mask);
  }
  return out;
}
}  // namespace ds2::android
