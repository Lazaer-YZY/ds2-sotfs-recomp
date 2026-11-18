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
#include <cerrno>
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
  }
  if (CPU_COUNT(&set) == 0) return "all cores (one cluster or empty mask)";
  std::string note;
  if (sched_setaffinity(0, sizeof(set), &set) != 0) {
    note = "all cores (sched_setaffinity failed: " + std::string(std::strerror(errno)) + ")";
  } else {
    note = "cpus " + CpuList(set) + (want.empty() ? " (middle cluster)" : " (DS2_AFFINITY)");
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

}  // namespace ds2::android
