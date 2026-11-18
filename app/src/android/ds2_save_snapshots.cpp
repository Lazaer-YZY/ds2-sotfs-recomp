// ds2 - development builds only: snapshots of the owner's save while they play, so benches can start in the areas
// they reach (DS2 keeps a single save that it overwrites).
//
// RuntimeActivity sets DS2_SAVE_SNAPSHOTS=1 only in debuggable builds without the public defaults; without it this
// does nothing, so public releases never copy anything. A low-priority thread on an efficiency core checks the save
// files' sizes and times once a minute; when they changed since the last snapshot, have been still for 10 s, and the
// last snapshot is at least DS2_SAVE_SNAPSHOT_MIN_S (600) old, it copies <app files>/user/<xuid>/465307E4 to
// <app files>/save_snapshots/<YYYYMMDD-HHMMSS>/<xuid>/465307E4 (a copy that raced an autosave is deleted and
// retried later). All snapshots are kept (a variety of places to bench from; ~7 MB each, at most 6 an hour);
// tools/android/pull_save_snapshots.sh copies them to the PC.

#include "android/ds2_android.h"

#include <rex/logging.h>

#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace ds2::android {

namespace {

namespace fs = std::filesystem;

// Every save file (relative path, size, modification time) of the profiles that have a DS2 save, sorted.
std::string SaveSignature(const fs::path& user_dir) {
  std::vector<std::string> entries;
  std::error_code ec;
  for (const auto& profile : fs::directory_iterator(user_dir, ec)) {
    const fs::path save = profile.path() / "465307E4";
    if (!fs::is_directory(save / "00000001", ec)) continue;
    for (auto it = fs::recursive_directory_iterator(save, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
      if (!it->is_regular_file(ec)) continue;
      const auto time = static_cast<long long>(
          std::chrono::duration_cast<std::chrono::milliseconds>(it->last_write_time(ec).time_since_epoch()).count());
      entries.push_back(fs::relative(it->path(), user_dir, ec).string() + ' ' + std::to_string(it->file_size(ec)) +
                        ' ' + std::to_string(time));
    }
  }
  std::sort(entries.begin(), entries.end());
  std::string signature;
  for (const auto& entry : entries) signature += entry + '\n';
  return signature;
}

bool CopySaves(const fs::path& user_dir, const fs::path& dest) {
  std::error_code ec;
  for (const auto& profile : fs::directory_iterator(user_dir, ec)) {
    const fs::path save = profile.path() / "465307E4";
    if (!fs::is_directory(save / "00000001", ec)) continue;
    const fs::path to = dest / profile.path().filename() / "465307E4";
    fs::create_directories(to, ec);
    fs::copy(save, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
  }
  return true;
}

// The efficiency cores (lowest maximum clock): the copy never competes with the game's threads.
void MoveToEfficiencyCores() {
  long slowest = 0;
  std::vector<std::pair<int, long>> cpus;
  for (int cpu = 0; cpu < 16; ++cpu) {
    std::ifstream f("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cpufreq/cpuinfo_max_freq");
    long khz = 0;
    if (!(f >> khz)) continue;
    cpus.emplace_back(cpu, khz);
    slowest = slowest ? std::min(slowest, khz) : khz;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  for (const auto& [cpu, khz] : cpus) {
    if (khz == slowest) CPU_SET(cpu, &set);
  }
  if (CPU_COUNT(&set)) sched_setaffinity(0, sizeof(set), &set);
  setpriority(PRIO_PROCESS, 0, 10);
}

std::string Stamp() {
  const std::time_t now = std::time(nullptr);
  std::tm tm{};
  localtime_r(&now, &tm);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
  return buf;
}

}  // namespace

void StartSaveSnapshots() {
  const char* enabled = std::getenv("DS2_SAVE_SNAPSHOTS");
  const char* app_dir = std::getenv("REX_ANDROID_APP_DIR");
  if (!enabled || std::string(enabled) != "1" || !app_dir) return;
  int min_gap_s = 600;
  if (const char* v = std::getenv("DS2_SAVE_SNAPSHOT_MIN_S"); v && std::atoi(v) > 0) min_gap_s = std::atoi(v);
  const fs::path base(app_dir);
  REXLOG_INFO("ds2 save snapshots: on (development build), at most one per {} s into {}", min_gap_s,
              (base / "save_snapshots").string());
  std::thread([base, min_gap_s] {
    MoveToEfficiencyCores();
    const fs::path user_dir = base / "user";
    const fs::path root = base / "save_snapshots";
    const fs::path last_file = root / "last_signature.txt";
    std::error_code ec;
    fs::create_directories(root, ec);
    std::string last;
    {
      std::ifstream f(last_file, std::ios::binary);
      last.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    auto last_snapshot = std::chrono::steady_clock::now() - std::chrono::seconds(min_gap_s);
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(60));
      if (std::chrono::steady_clock::now() - last_snapshot < std::chrono::seconds(min_gap_s)) continue;
      const std::string before = SaveSignature(user_dir);
      if (before.empty() || before == last) continue;
      std::this_thread::sleep_for(std::chrono::seconds(10));
      if (SaveSignature(user_dir) != before) continue;  // still being written
      const fs::path dest = root / Stamp();
      const bool copied = CopySaves(user_dir, dest);
      if (!copied || SaveSignature(user_dir) != before) {
        fs::remove_all(dest, ec);
        REXLOG_INFO("ds2 save snapshots: the save changed during the copy; retrying later");
        continue;
      }
      last = before;
      last_snapshot = std::chrono::steady_clock::now();
      std::ofstream(last_file, std::ios::binary | std::ios::trunc) << last;
      REXLOG_INFO("ds2 save snapshots: {}", dest.filename().string());
    }
  }).detach();
}

}  // namespace ds2::android
