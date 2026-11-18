// ds2 - load/hang telemetry, see ds2_watchdog.h.

#include "ds2_watchdog.h"

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/filesystem.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xfile.h>
#include <rex/system/xthread.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <malloc.h>
#include <deque>
#include <filesystem>
#include <unistd.h>
#include <map>
#include <set>
#include <string>
#include <thread>

REXCVAR_DEFINE_BOOL(ds2_watchdog, true, "DS2",
                    "Load/hang telemetry in the main log: snapshots of guest threads, open files and memory, "
                    "file open/close events and stall alarms")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// The runtime's list of export call counters (src/system/ppc_types.cpp); libmain.so's own copy of the inline
// rex::ppc::ExportCallCounterHead() is empty.
extern "C" rex::ppc::ExportCallCounter* rex_export_call_counter_head();
// Counting is off unless asked for (sdk/include/rex/hook.h, REX_EXPORT_COUNT).
extern "C" void rex_export_stats_enable(bool on);

REXCVAR_DEFINE_BOOL(ds2_export_stats, false, "DS2",
                    "DIAGNOSTIC: every heartbeat, log how many times each kernel/XAM export was called since the "
                    "previous one");
REXCVAR_DEFINE_INT32(ds2_watchdog_snapshot_s, 10, "DS2", "Seconds between [ds2 wd] snapshots (0 = only on stalls)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(ds2_watchdog_heartbeat_s, 2, "DS2",
                     "Seconds between one-line [ds2 wd] hb heartbeats: vblank counter and the shaders the GPU command "
                     "processor last bound (0 = off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);


namespace ds2::watchdog {


namespace {

std::thread g_thread;
std::atomic<bool> g_run{false};
rex::Runtime* g_runtime = nullptr;

// The main log can lose its last seconds when the console is switched off by holding POWER (data handed to the
// file system but not committed). logs/ds2_wd_last.txt is rewritten and fsync'ed on every heartbeat, so after a
// forced power-off it still says when the process was last alive and what it was doing.
std::deque<std::string> g_events;  // recent file events, watchdog thread only

void Note(std::string line) {
  g_events.push_back(std::move(line));
  while (g_events.size() > 24) g_events.pop_front();
}

void WriteDurable(const std::string& heartbeat) {
  static const std::filesystem::path path = [] {
    const auto dir = rex::filesystem::GetExecutableFolder();
    if (dir.empty()) return std::filesystem::path();
    std::error_code ec;
    std::filesystem::create_directories(dir / "logs", ec);
    return dir / "logs" / "ds2_wd_last.txt";
  }();
  if (path.empty()) return;
  // Written to a temporary file and renamed, so a failing write (the kernel ran out of resources) leaves the
  // previous version instead of an empty file.
  const std::string tmp = path.string() + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) return;
  std::fprintf(f, "%s\nrecent file events:\n", heartbeat.c_str());
  for (const auto& e : g_events) std::fprintf(f, "  %s\n", e.c_str());
  const bool ok = std::fflush(f) == 0 && fsync(fileno(f)) == 0 && !std::ferror(f);
  std::fclose(f);
  if (ok) {
    std::remove(path.string().c_str());
    std::rename(tmp.c_str(), path.string().c_str());
  }
}

// One line: is the process alive (this line keeps coming), does the guest see vblanks, and is the GPU command
// processor still moving (the shaders it binds change while it executes the game's draws).
void LogHeartbeat(int seconds, size_t threads, size_t files) {
  uint32_t vblank = 0;
  uint64_t vs = 0, ps = 0;
  bool paused = false;
  auto* igs = g_runtime ? g_runtime->graphics_system() : nullptr;
  if (auto* gs = static_cast<rex::graphics::GraphicsSystem*>(igs)) {
    if (auto* cp = gs->command_processor()) {
      vblank = cp->counter();
      paused = cp->is_paused();
      if (auto* v = cp->active_vertex_shader()) vs = v->ucode_data_hash();
      if (auto* p = cp->active_pixel_shader()) ps = p->ucode_data_hash();
    }
  }
  const struct mallinfo mi = mallinfo();
  // The game's presented frames (VdSwap) in the last ~2 s: frame rate and the slowest frame.
  // Frame-time histogram of the same frames (hist=, upper bounds 18/21/26/34/45/inf ms): a run's 1% low and frames
  // over 45 ms come from summing these (tools/android/frame_stats.py).
  double fps = 0, frame_max_ms = 0;
  int hist[6] = {};
  {
    static float ms[511];
    const int n = rex::kernel::xboxkrnl::RecentGuestFrameTimesMs(ms, 511);
    static const float kBounds[5] = {18.0f, 21.0f, 26.0f, 34.0f, 45.0f};
    double sum = 0;
    int count = 0;
    for (int i = n - 1; i >= 0 && sum < 2000.0; --i, ++count) {
      sum += ms[i];
      frame_max_ms = std::max(frame_max_ms, double(ms[i]));
      int bin = 0;
      while (bin < 5 && ms[i] > kBounds[bin]) ++bin;
      ++hist[bin];
    }
    if (sum > 0) fps = 1000.0 * count / sum;
  }
  const std::string line = fmt::format(
      "[ds2 wd] t={}s hb fps={:.1f} max_ms={:.0f} hist={}/{}/{}/{}/{}/{} vblank={} cp_vs={:016X} cp_ps={:016X}{} "
      "threads={} files={} malloc_mb={}",
      seconds, fps, frame_max_ms, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], vblank, vs, ps,
      paused ? " cp_paused" : "", threads, files,
      size_t(mi.uordblks) >> 20);
  REXLOG_INFO("{}", line);
  // Kernel/XAM exports called since the previous heartbeat (all calls, sorted by count).
  if (REXCVAR_GET(ds2_export_stats)) {
    std::vector<std::pair<uint64_t, const char*>> called;
    for (auto* c = rex_export_call_counter_head(); c; c = c->next) {
      const uint64_t now_calls = c->calls.load(std::memory_order_relaxed);
      if (now_calls != c->reported) {
        called.emplace_back(now_calls - c->reported, c->name);
        c->reported = now_calls;
      }
    }
    std::sort(called.begin(), called.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string list;
    for (const auto& [n, name] : called) {
      list += fmt::format(" {}={}", name, n);
    }
    REXLOG_INFO("[ds2 wd] t={}s exports:{}", seconds, list);
  }
  const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  char stamp[32] = {};
  std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
  WriteDurable(std::string(stamp) + " " + line);
}

constexpr int kStallSeconds = 5;
constexpr int kStallRepeatSeconds = 15;

void LogMemory(const char* tag) {
  const struct mallinfo mi = mallinfo();
  REXLOG_INFO("[ds2 wd] {} memory: malloc arena {} MB in use {} MB", tag, size_t(mi.arena) >> 20,
              size_t(mi.uordblks) >> 20);
}

template <typename Threads>
void LogThreads(const Threads& threads, bool error) {
  for (auto& t : threads) {
    const auto* cp = t->creation_params();
    auto* st = t->thread_state();
    if (!st || !st->context()) {
      REXLOG_INFO("[ds2 wd]   thread 0x{:X} entry=0x{:08X} '{}' no context", t->thread_id(), cp->start_address,
                  t->name());
      continue;
    }
    const auto& c = *st->context();
    if (error) {
      REXLOG_ERROR("[ds2 wd]   thread 0x{:X} entry=0x{:08X} '{}' main={} running={} lr=0x{:08X} r1=0x{:08X} "
                   "r3=0x{:08X} ctr=0x{:08X} last_indirect=0x{:08X}",
                   t->thread_id(), cp->start_address, t->name(), t->main_thread(), t->is_running(),
                   static_cast<uint32_t>(c.lr), c.r1.u32, c.r3.u32, c.ctr.u32, c.last_indirect_target);
    } else {
      REXLOG_INFO("[ds2 wd]   thread 0x{:X} entry=0x{:08X} '{}' main={} running={} lr=0x{:08X} r1=0x{:08X} "
                  "r3=0x{:08X} ctr=0x{:08X} last_indirect=0x{:08X}",
                  t->thread_id(), cp->start_address, t->name(), t->main_thread(), t->is_running(),
                  static_cast<uint32_t>(c.lr), c.r1.u32, c.r3.u32, c.ctr.u32, c.last_indirect_target);
    }
  }
}

void Main() {
  const int every = REXCVAR_GET(ds2_watchdog_snapshot_s);
  const int heartbeat = REXCVAR_GET(ds2_watchdog_heartbeat_s);
  std::multiset<std::string> files_before;
  uint64_t sig_before = 0;
  int still = 0, since_alarm = 0, since_snapshot = 0, seconds = 0;
  bool alarmed = false;
  REXLOG_INFO("[ds2 wd] started (snapshot every {} s, stall after {} s)", every, kStallSeconds);
  LogMemory("start");
  rex::FlushLogging();

  while (g_run.load()) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!g_run.load()) break;
    ++seconds;
    auto* kernel = rex::system::kernel_state();
    if (!kernel) continue;

    auto threads = kernel->object_table()->GetObjectsByType<rex::system::XThread>();
    auto files = kernel->object_table()->GetObjectsByType<rex::system::XFile>();
    bool wrote = false;

    // Open guest files: what the loader is working on. Logged as differences, so a load leaves a trail.
    std::multiset<std::string> files_now;
    for (auto& f : files) files_now.insert(f->path());
    if (files_now != files_before) {
      for (const auto& p : files_now)
        if (files_before.count(p) < files_now.count(p)) {
          REXLOG_INFO("[ds2 wd] t={}s file + {}", seconds, p);
          Note(fmt::format("t={}s + {}", seconds, p));
        }
      for (const auto& p : files_before)
        if (files_now.count(p) < files_before.count(p)) {
          REXLOG_INFO("[ds2 wd] t={}s file - {}", seconds, p);
          Note(fmt::format("t={}s - {}", seconds, p));
        }
      wrote = true;
    }

    // Signature of where every guest thread is (sorted by id: the table order is not stable).
    std::map<uint32_t, uint64_t> per_thread;
    for (auto& t : threads) {
      auto* st = t->thread_state();
      if (!st || !st->context()) continue;
      const auto& c = *st->context();
      per_thread[t->thread_id()] = static_cast<uint64_t>(c.lr) ^ (static_cast<uint64_t>(c.r1.u32) << 20) ^
                                   (static_cast<uint64_t>(c.r3.u32) << 40);
    }
    uint64_t sig = 1469598103934665603ull;
    for (const auto& [id, v] : per_thread) {
      sig = (sig ^ id) * 1099511628211ull;
      sig = (sig ^ v) * 1099511628211ull;
    }
    sig = (sig ^ files_now.size()) * 1099511628211ull;

    if (heartbeat > 0 && seconds % heartbeat == 0) {
      LogHeartbeat(seconds, threads.size(), files_now.size());
      wrote = true;
    }

    if (every > 0 && ++since_snapshot >= every) {
      since_snapshot = 0;
      REXLOG_INFO("[ds2 wd] t={}s snapshot: {} guest threads, {} open files", seconds, threads.size(),
                  files_now.size());
      LogMemory("snapshot");
      LogThreads(threads, false);
      wrote = true;
    }

    if (sig != sig_before || files_now != files_before) {
      if (alarmed) {
        REXLOG_WARN("[ds2 wd] t={}s moving again after {} s", seconds, still);
        wrote = true;
      }
      alarmed = false;
      still = 0;
      since_alarm = 0;
    } else {
      ++still;
      ++since_alarm;
      if (still >= kStallSeconds && (!alarmed || since_alarm >= kStallRepeatSeconds)) {
        since_alarm = 0;
        alarmed = true;
        REXLOG_ERROR("[ds2 wd] t={}s STALL: {} s without any change in {} guest threads and {} open files",
                     seconds, still, threads.size(), files_now.size());
        LogMemory("stall");
        LogThreads(threads, true);
        wrote = true;
      }
    }
    sig_before = sig;
    files_before = std::move(files_now);
    if (wrote) rex::FlushLogging();
  }
}

}  // namespace

void Start(rex::Runtime* runtime) {
  if (!REXCVAR_GET(ds2_watchdog) || g_run.exchange(true)) return;
  if (REXCVAR_GET(ds2_export_stats)) rex_export_stats_enable(true);
  g_runtime = runtime;
  g_thread = std::thread(Main);
}

void Stop() {
  if (!g_run.exchange(false)) return;
  if (g_thread.joinable()) g_thread.join();
}

}  // namespace ds2::watchdog
