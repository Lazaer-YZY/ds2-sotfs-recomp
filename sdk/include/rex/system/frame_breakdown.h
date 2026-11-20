#pragma once
// Frame breakdown (diagnostics, cvar frame_breakdown_ms in kernel/xboxkrnl/xboxkrnl_video.cpp): how the thread that
// presents the game's frames (VdSwap) spent each frame - running, waiting for the CPU (scheduler run queue), blocked
// in guest waits (by call site), sleeping, held by the frame cap - so a slow frame says whether the game's own work,
// another thread it waited for, or the CPU scheduler made it late. Off: one relaxed load per guest wait.

#include <atomic>
#include <cstdint>

namespace rex::system::frame_breakdown {

// Mirrors frame_breakdown_ms != 0 (set by VdSwap).
extern std::atomic<bool> g_on;

struct WaitSite {
  uint32_t lr = 0;  // the guest return address of the wait's caller
  uint32_t count = 0;
  uint64_t ns = 0;
};

struct ThreadWaits {
  uint64_t wait_ns = 0;   // XObject waits: events, semaphores, mutants, threads, critical sections
  uint64_t delay_ns = 0;  // KeDelayExecutionThread (sleeps and yields)
  uint32_t waits = 0;
  static constexpr int kSites = 6;
  WaitSite sites[kSites];  // the longest call sites since the last Reset
  void ResetSites() {
    for (WaitSite& s : sites) s = WaitSite();
  }
};

ThreadWaits& CurrentThreadWaits();
uint64_t NowNs();
void AddWait(bool delay, uint64_t ns);

class ScopedWait {
 public:
  explicit ScopedWait(bool delay = false)
      : delay_(delay), t0_(g_on.load(std::memory_order_relaxed) ? NowNs() : 0) {}
  ~ScopedWait() {
    if (t0_) AddWait(delay_, NowNs() - t0_);
  }
  ScopedWait(const ScopedWait&) = delete;
  ScopedWait& operator=(const ScopedWait&) = delete;

 private:
  bool delay_;
  uint64_t t0_;
};

// frame_timeline (diagnostics): a lock-free ring of timestamped events from the threads that pace a frame - the
// game's swap ('S' entry, 'C' after the frame cap), vertical blanks ('V'), guest interrupts ('I', value = source),
// the game's wait for the command processor ('r' / 'R'), and the command thread: waiting for commands ('k' / 'K'),
// WAIT_REG_MEM ('w' / 'W'), the swap packet ('P'). TimelineDump logs the events of the last span_ms.
extern std::atomic<bool> g_timeline_on;
void TimelineRecord(char kind, uint32_t value);
inline void TimelineMark(char kind, uint32_t value = 0) {
  if (g_timeline_on.load(std::memory_order_relaxed)) TimelineRecord(kind, value);
}
void TimelineDump(double span_ms, const char* why);

// The calling thread's scheduler statistics (/proc/thread-self/schedstat): time on a CPU and time waiting on a run
// queue, in ns. False where unavailable.
bool ThreadSchedStat(uint64_t& run_ns, uint64_t& queue_ns);

}  // namespace rex::system::frame_breakdown
