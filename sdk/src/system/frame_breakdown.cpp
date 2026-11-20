// Frame breakdown diagnostics: per-thread guest wait accounting (see rex/system/frame_breakdown.h).

#include <rex/system/frame_breakdown.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <mutex>
#include <string>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/system/thread_state.h>

#if defined(__linux__) || defined(__ANDROID__)
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace rex::system::frame_breakdown {

std::atomic<bool> g_on{false};
std::atomic<bool> g_timeline_on{false};

namespace {
struct TimelineEvent {
  uint64_t ns;
  uint32_t value;
  uint16_t thread;
  char kind;
};
constexpr size_t kTimelineSize = size_t(1) << 13;
TimelineEvent g_timeline[kTimelineSize];
std::atomic<uint64_t> g_timeline_head{0};
std::mutex g_timeline_threads_mutex;
std::vector<int> g_timeline_threads;
uint16_t TimelineThread() {
  thread_local uint16_t id = [] {
    std::lock_guard<std::mutex> lock(g_timeline_threads_mutex);
#if defined(__linux__) || defined(__ANDROID__)
    g_timeline_threads.push_back(int(syscall(SYS_gettid)));
#else
    g_timeline_threads.push_back(0);
#endif
    return uint16_t(g_timeline_threads.size() - 1);
  }();
  return id;
}
std::string ThreadName(int tid) {
#if defined(__linux__) || defined(__ANDROID__)
  char path[64];
  std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
  if (FILE* f = std::fopen(path, "r")) {
    char name[32] = {};
    size_t n = std::fread(name, 1, sizeof(name) - 1, f);
    std::fclose(f);
    while (n && (name[n - 1] == '\n' || name[n - 1] == ' ')) name[--n] = 0;
    return name;
  }
#endif
  return std::to_string(tid);
}
}  // namespace

void TimelineRecord(char kind, uint32_t value) {
  const uint64_t i = g_timeline_head.fetch_add(1, std::memory_order_relaxed);
  TimelineEvent& e = g_timeline[i & (kTimelineSize - 1)];
  e.ns = NowNs();
  e.value = value;
  e.thread = TimelineThread();
  e.kind = kind;
}

void TimelineDump(double span_ms, const char* why) {
  const uint64_t now = NowNs();
  const uint64_t from = now - uint64_t(span_ms * 1e6);
  const uint64_t head = g_timeline_head.load(std::memory_order_acquire);
  const uint64_t first = head > kTimelineSize ? head - kTimelineSize : 0;
  std::vector<TimelineEvent> events;
  for (uint64_t i = first; i < head; ++i) {
    const TimelineEvent e = g_timeline[i & (kTimelineSize - 1)];
    if (e.ns >= from && e.ns <= now) events.push_back(e);
  }
  std::string threads;
  {
    std::lock_guard<std::mutex> lock(g_timeline_threads_mutex);
    std::vector<bool> seen(g_timeline_threads.size());
    for (const TimelineEvent& e : events) {
      if (e.thread < seen.size() && !seen[e.thread]) {
        seen[e.thread] = true;
        threads += " T" + std::to_string(e.thread) + "=" + ThreadName(g_timeline_threads[e.thread]);
      }
    }
  }
  REXLOG_INFO("frame timeline ({}): {} events in the last {:.0f} ms;{}", why, events.size(), span_ms, threads);
  std::string line;
  int n = 0;
  for (const TimelineEvent& e : events) {
    char item[64];
    std::snprintf(item, sizeof(item), " %.2f T%u %c%s", (double(e.ns) - double(from)) / 1e6, e.thread, e.kind,
                  (e.kind == 'V' || e.kind == 'I') ? (" " + std::to_string(e.value)).c_str() : "");
    line += item;
    if (++n % 40 == 0) {
      REXLOG_INFO("frame timeline:{}", line);
      line.clear();
    }
  }
  if (!line.empty()) REXLOG_INFO("frame timeline:{}", line);
}

ThreadWaits& CurrentThreadWaits() {
  thread_local ThreadWaits waits;
  return waits;
}

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

void AddWait(bool delay, uint64_t ns) {
  ThreadWaits& w = CurrentThreadWaits();
  (delay ? w.delay_ns : w.wait_ns) += ns;
  ++w.waits;
  uint32_t lr = 0;
  if (auto* ts = rex::runtime::ThreadState::Get()) {
    if (auto* ctx = ts->context()) lr = uint32_t(ctx->lr);
  }
  if (delay) lr |= 1;  // odd = a delay (guest code addresses are 4-aligned)
  WaitSite* slot = nullptr;
  WaitSite* smallest = &w.sites[0];
  for (WaitSite& s : w.sites) {
    if (s.lr == lr && s.count) {
      slot = &s;
      break;
    }
    if (s.ns < smallest->ns) smallest = &s;
  }
  if (!slot) {
    if (smallest->count && smallest->ns >= ns) return;  // keep the longer sites
    *smallest = WaitSite();
    smallest->lr = lr;
    slot = smallest;
  }
  ++slot->count;
  slot->ns += ns;
}

bool ThreadSchedStat(uint64_t& run_ns, uint64_t& queue_ns) {
#if defined(__linux__) || defined(__ANDROID__)
  thread_local int fd = [] {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", int(syscall(SYS_gettid)));
    return open(path, O_RDONLY | O_CLOEXEC);
  }();
  if (fd < 0) return false;
  char buf[96];
  const ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
  if (n <= 0) return false;
  buf[n] = 0;
  char* end = nullptr;
  run_ns = std::strtoull(buf, &end, 10);
  queue_ns = std::strtoull(end, nullptr, 10);
  return true;
#else
  run_ns = queue_ns = 0;
  return false;
#endif
}

}  // namespace rex::system::frame_breakdown
