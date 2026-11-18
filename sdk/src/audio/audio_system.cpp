/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/assert.h>
#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_system.h>
#include <rex/audio/flags.h>
#include <rex/audio/xma/decoder.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/stream.h>
#include <rex/string/buffer.h>
#include <rex/system/thread_state.h>
#include <rex/thread.h>
#include <rex/cvar.h>

#include <atomic>
#include <chrono>
#include <cstring>

// DS2 port diagnostic: does the game's audio callback produce fresh frames? Every 10 s: callbacks and their time,
// frames submitted, frames byte-identical to one of the previous four (a mixer that is not keeping up resubmits
// its ring buffers: heard as a buzz at 1024-sample period).
namespace rex::audio::sdl {
extern std::atomic<uint64_t> g_sdl_pull_calls, g_sdl_pull_frames, g_sdl_pull_max, g_sdl_pull_min,
    g_sdl_silent_frames;
}  // namespace rex::audio::sdl
namespace {
struct AudioDiag {
  std::atomic<uint64_t> callbacks{0}, callback_ns{0}, callback_max_ns{0}, submitted{0}, stale{0};
  // audio_pace_callbacks: dispatches that ran at once because the queue was low, and the driver
  // queue level seen at dispatch (min/max).
  std::atomic<uint64_t> catch_up{0}, level_min{~0ull}, level_max{0};
  uint64_t recent[4] = {};
  uint32_t recent_pos = 0;
  std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};
AudioDiag g_audio_diag;
uint64_t HashFrame(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; i += 8) {
    uint64_t v;
    std::memcpy(&v, p + i, 8);
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}
}  // namespace

REXCVAR_DEFINE_INT32(
    audio_maxqframes, 8, "Audio",
    "Max buffered audio frames (range 4-64). Lower reduces latency but may cause stuttering.");
// DS2 port: the host device takes its data in bursts (Android: 1024 samples = 4 frames every
// 21.3 ms), and every frame it takes frees a slot, so the worker ran the guest's render callback
// four times back to back. The console calls it every 5.33 ms. A game whose mixer polls the play
// position and keeps only a few frames ahead of it sees the cursor jump by its whole ring and
// mixes nothing: DS2's FMOD (a 4 x 256-sample ring, polled every 1 ms) refilled 3 blocks in 15
// and XAudio2 replayed stale blocks, the "metallic static" (2026-11-18). With pacing on, a
// callback runs no earlier than one frame period after the previous one, unless the device queue
// is about to run dry (fewer than audio_pace_low_frames queued); then it catches up, but never
// closer than audio_pace_min_gap_us to the previous callback, so a mixer polling every ~1 ms still
// sees each step (back-to-back catch-ups of 4-5 frames brought the static back, 2026-11-18).
REXCVAR_DEFINE_BOOL(audio_pace_callbacks, true, "Audio",
                    "Run the guest's audio callbacks one frame period (5.33 ms) apart, as on the "
                    "console, instead of in bursts when the host device takes several frames at once");
// Thor: the device takes 768-960 samples (3-4 frames) 50 times a second. Pacing produces exactly
// as fast as the device takes, so the queue only refills above the low mark through catch-ups;
// with a mark of 2 it ran 1-5 frames deep (one 5 ms silent gap in 20 s once), with 4 a 13.6 ms
// stall emptied it (3 silent frames in 30 s); 6 keeps it 3-6 deep (16 ms of margin), and the
// minimum gap keeps the catch-ups safe for FMOD.
REXCVAR_DEFINE_INT32(audio_pace_low_frames, 6, "Audio",
                     "audio_pace_callbacks: below this many queued frames callbacks catch up (spaced "
                     "audio_pace_min_gap_us) instead of waiting for their turn")
    .range(0, 64);
REXCVAR_DEFINE_INT32(audio_pace_min_gap_us, 2000, "Audio",
                     "audio_pace_callbacks: shortest time between two guest audio callbacks, catch-ups "
                     "included (DS2's FMOD polls the play position every ~1.06 ms)")
    .range(0, 5333);

// As with normal Microsoft, there are like twelve different ways to access
// the audio APIs. Early games use XMA*() methods almost exclusively to touch
// decoders. Later games use XAudio*() and direct memory writes to the XMA
// structures (as opposed to the XMA* calls), meaning that we have to support
// both.
//
// For ease of implementation, most audio related processing is handled in
// AudioSystem, and the functions here call off to it.
// The XMA*() functions just manipulate the audio system in the guest context
// and let the normal AudioSystem handling take it, to prevent duplicate
// implementations. They can be found in xboxkrnl_audio_xma.cc

namespace rex::audio {

namespace {
constexpr std::chrono::milliseconds kWorkerShutdownTimeout{500};
}  // namespace

AudioSystem::AudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()),
      function_dispatcher_(function_dispatcher),
      worker_running_(false) {
  std::memset(clients_, 0, sizeof(clients_));

  queued_frames_ = std::min(
      static_cast<uint32_t>(kMaximumQueuedFrames),
      std::max(static_cast<uint32_t>(REXCVAR_GET(audio_maxqframes)), static_cast<uint32_t>(4)));

  for (size_t i = 0; i < kMaximumClientCount; ++i) {
    client_semaphores_[i] = rex::thread::Semaphore::Create(0, queued_frames_);
    assert_not_null(client_semaphores_[i]);
    wait_handles_[i] = client_semaphores_[i].get();
  }
  shutdown_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(shutdown_event_);
  wait_handles_[kMaximumClientCount] = shutdown_event_.get();

  xma_decoder_ = std::make_unique<rex::audio::XmaDecoder>(function_dispatcher_);

  resume_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(resume_event_);
}

AudioSystem::~AudioSystem() {
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }
}

X_STATUS AudioSystem::Setup(system::KernelState* kernel_state) {
  X_STATUS result = xma_decoder_->Setup(kernel_state);
  if (result) {
    return result;
  }

  worker_running_ = true;
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        WorkerThreadMain();
        return 0;
      }));

  worker_thread_->set_name("Audio Worker");
  worker_thread_->Create();

  return X_STATUS_SUCCESS;
}

void AudioSystem::WorkerThreadMain() {
  // Initialize driver and ringbuffer.
  Initialize();

  // Main run loop.
  uint32_t diag_pump_count = 0;
  // audio_pace_callbacks: earliest time of each client's next callback.
  const bool pace = REXCVAR_GET(audio_pace_callbacks);
  const size_t pace_low = size_t(std::max(0, int(REXCVAR_GET(audio_pace_low_frames))));
  const auto pace_min_gap =
      std::chrono::microseconds(std::max(0, int(REXCVAR_GET(audio_pace_min_gap_us))));
  constexpr auto kFramePeriod = std::chrono::nanoseconds(1000000000ll * 256 / 48000);
  std::chrono::steady_clock::time_point next_due[kMaximumClientCount] = {};
  std::chrono::steady_clock::time_point last_start[kMaximumClientCount] = {};
  while (worker_running_) {
    // These handles signify the number of submitted samples. Once we reach
    // 64 samples, we wait until our audio backend releases a semaphore
    // (signaling a sample has finished playing)
    // Not alertable (DS2 port): an alertable wait re-wakes every 1 ms to look for queued APCs
    // (threading_posix.cpp, kAlertablePollSlice), ~1,000 wake-ups a second here, and nothing
    // queues an APC to this host thread (the guest never sees its handle).
    auto result = rex::thread::WaitAny(wait_handles_, rex::countof(wait_handles_), false,
                                       std::chrono::milliseconds(500));
    if (result.first == rex::thread::WaitResult::kFailed) {
      REXAPU_WARN("AudioWorker: WaitAny failed");
      continue;
    }

    if (result.first == rex::thread::WaitResult::kTimeout) {
      if (diag_pump_count < 5) {
        REXAPU_NOISY_DEBUG("AudioWorker: WaitAny timed out (no semaphore signals)");
      }
    }

    if (result.first == thread::WaitResult::kSuccess && result.second == kMaximumClientCount) {
      // Shutdown event signaled.
      if (paused_) {
        pause_fence_.Signal();
        thread::Wait(resume_event_.get(), false);
      }

      continue;
    }

    // Number of clients pumped
    bool pumped = false;
    if (result.first == rex::thread::WaitResult::kSuccess) {
      auto index = result.second;

      auto global_lock = global_critical_region_.Acquire();
      uint32_t client_callback = clients_[index].callback;
      uint32_t client_callback_arg = clients_[index].wrapped_callback_arg;
      AudioDriver* client_driver = clients_[index].driver;
      global_lock.unlock();

      if (pace && client_callback) {
        // The slot is taken; wait for this frame's turn (at most one period, so a shutdown is
        // noticed within ~5 ms) unless the device is about to run out of queued frames.
        const auto now = std::chrono::steady_clock::now();
        const size_t level = client_driver ? client_driver->QueuedFrameCount() : SIZE_MAX;
        if (level != SIZE_MAX) {
          AudioDiag& d = g_audio_diag;
          if (level < d.level_min.load(std::memory_order_relaxed)) d.level_min.store(level);
          if (level > d.level_max.load(std::memory_order_relaxed)) d.level_max.store(level);
        }
        // Its turn on the schedule, or a catch-up when the queue is low; either way not closer
        // than the minimum gap to the previous callback.
        const bool catch_up = level < pace_low;
        if (catch_up) g_audio_diag.catch_up.fetch_add(1, std::memory_order_relaxed);
        const auto due = std::max(catch_up ? now : next_due[index], last_start[index] + pace_min_gap);
        if (now < due) {
          rex::thread::Sleep(std::chrono::duration_cast<std::chrono::microseconds>(due - now));
        }
      }

      if (client_callback) {
        if (diag_pump_count < 10) {
          REXAPU_DEBUG("AudioWorker: dispatching callback {:08X} with arg {:08X} for client {}",
                       client_callback, client_callback_arg, index);
        }
        SCOPE_profile_cpu_i("apu", "rex::audio::AudioSystem->client_callback");
        uint64_t args[] = {client_callback_arg};
        const auto cb_start = std::chrono::steady_clock::now();
        function_dispatcher_->Execute(worker_thread_->thread_state(), client_callback, args,
                                      rex::countof(args));
        const uint64_t cb_ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                            std::chrono::steady_clock::now() - cb_start)
                                            .count());
        g_audio_diag.callbacks.fetch_add(1, std::memory_order_relaxed);
        if (pace) {
          // On the schedule one period after the previous turn, and never sooner than half a
          // period after this callback started (after a stall the schedule resumes from here
          // instead of running the missed turns back to back).
          next_due[index] = std::max(next_due[index] + kFramePeriod, cb_start + kFramePeriod / 2);
          last_start[index] = cb_start;
        }
        g_audio_diag.callback_ns.fetch_add(cb_ns, std::memory_order_relaxed);
        if (cb_ns > g_audio_diag.callback_max_ns.load(std::memory_order_relaxed)) {
          g_audio_diag.callback_max_ns.store(cb_ns, std::memory_order_relaxed);
        }
        if (diag_pump_count < 10) {
          REXAPU_DEBUG("AudioWorker: callback returned for client {}", index);
        }
        diag_pump_count++;
      } else {
        REXAPU_DEBUG("AudioWorker: semaphore signaled for client {} but callback is 0", index);
      }

      pumped = true;
    }

    if (!worker_running_) {
      break;
    }

    if (!pumped) {
      SCOPE_profile_cpu_i("apu", "Sleep");
      rex::thread::Sleep(std::chrono::milliseconds(500));
    }
  }
  worker_running_ = false;

  // TODO(benvanik): call module API to kill?
}

int AudioSystem::FindFreeClient() {
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      return i;
    }
  }

  return -1;
}

void AudioSystem::Initialize() {}

void AudioSystem::Shutdown() {
  if (!worker_running_) {
    return;
  }

  // Shut down XMA decoder first - its worker can stall in FFmpeg
  if (xma_decoder_) {
    xma_decoder_->Shutdown();
  }

  worker_running_ = false;
  shutdown_event_->Set();
  if (worker_thread_) {
    // The worker may be stuck inside a guest callback that is itself blocked on
    // guest objects (e.g. KeWaitForMultipleObjects), so terminating is the last
    // resort. Give it a chance to unwind first: TerminateThread abandons any
    // lock the thread holds, including the CRT heap lock.
    rex::thread::Thread* host_thread = worker_thread_->thread();
    bool exited = host_thread && rex::thread::Wait(host_thread, false, kWorkerShutdownTimeout) ==
                                     rex::thread::WaitResult::kSuccess;
    if (!exited) {
      REXAPU_WARN("Audio worker did not exit within {} ms; terminating",
                  kWorkerShutdownTimeout.count());
      worker_thread_->Terminate(0);
    }
    worker_thread_.reset();
  }

  // Destroy all active client drivers (closes SDL audio devices, stopping
  // callback threads) before the semaphores they reference are destroyed.
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      DestroyDriver(clients_[i].driver);
      if (clients_[i].wrapped_callback_arg) {
        memory()->SystemHeapFree(clients_[i].wrapped_callback_arg);
      }
      clients_[i] = {nullptr, 0, 0, 0, false};
    }
  }
}

X_STATUS AudioSystem::RegisterClient(uint32_t callback, uint32_t callback_arg, size_t* out_index) {
  REXAPU_DEBUG("AudioSystem::RegisterClient: callback={:08X} callback_arg={:08X}", callback,
               callback_arg);
  auto global_lock = global_critical_region_.Acquire();

  auto index = FindFreeClient();
  assert_true(index >= 0);
  REXAPU_DEBUG("AudioSystem::RegisterClient: using client index={} queued_frames={}", index,
               queued_frames_);

  auto client_semaphore = client_semaphores_[index].get();
  auto ret = client_semaphore->Release(queued_frames_, nullptr);
  assert_true(ret);

  AudioDriver* driver;
  auto result = CreateDriver(index, client_semaphore, &driver);
  if (XFAILED(result)) {
    return result;
  }
  assert_not_null(driver);

  uint32_t ptr = memory()->SystemHeapAlloc(0x4);
  memory::store_and_swap<uint32_t>(memory()->TranslateVirtual(ptr), callback_arg);

  clients_[index] = {driver, callback, callback_arg, ptr, true};

  if (out_index) {
    *out_index = index;
  }

  return X_STATUS_SUCCESS;
}

void AudioSystem::SubmitFrame(size_t index, uint32_t samples_ptr) {
  SCOPE_profile_cpu_f("apu");

  static uint32_t submit_count = 0;
  if (submit_count < 10) {
    REXAPU_DEBUG("AudioSystem::SubmitFrame called: index={} samples_ptr={:08X}", index,
                 samples_ptr);
    submit_count++;
  }

  {
    // 256 samples x 6 channels of float.
    const auto* frame = memory_->TranslateVirtual<const uint8_t*>(samples_ptr);
    const uint64_t h = HashFrame(frame, 256 * 6 * sizeof(float));
    static const uint8_t kZero[256 * 6 * sizeof(float)] = {};
    const bool silent = std::memcmp(frame, kZero, sizeof(kZero)) == 0;
    AudioDiag& d = g_audio_diag;
    bool stale = false;
    for (uint64_t r : d.recent) stale |= (r == h);
    d.recent[d.recent_pos++ & 3] = h;
    d.submitted.fetch_add(1, std::memory_order_relaxed);
    if (stale && !silent) d.stale.fetch_add(1, std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    if (now - d.last >= std::chrono::seconds(10)) {
      const uint64_t cbs = d.callbacks.exchange(0), ns = d.callback_ns.exchange(0),
                     mx = d.callback_max_ns.exchange(0), sub = d.submitted.exchange(0),
                     st = d.stale.exchange(0);
      namespace s = rex::audio::sdl;
      const uint64_t pulls = s::g_sdl_pull_calls.exchange(0), pulled = s::g_sdl_pull_frames.exchange(0),
                     pmax = s::g_sdl_pull_max.exchange(0), pmin = s::g_sdl_pull_min.exchange(~0ull),
                     silent_frames = s::g_sdl_silent_frames.exchange(0), cu = d.catch_up.exchange(0),
                     lmin = d.level_min.exchange(~0ull), lmax = d.level_max.exchange(0);
      REXAPU_INFO("[audio diag] 10 s: {} callbacks (avg {:.2f} ms, max {:.2f} ms), {} frames submitted, {} ({:.0f} %) "
                  "identical to one of the last 4; device pulls {} ({} sample frames, min {} max {} each), {} "
                  "silent frames (queue empty); queue at dispatch {}-{}, {} catch-up dispatches",
                  cbs, cbs ? ns / 1e6 / cbs : 0.0, mx / 1e6, sub, st, sub ? 100.0 * st / sub : 0.0, pulls,
                  pulls ? pulled / pulls : 0, pmin == ~0ull ? 0 : pmin, pmax, silent_frames,
                  lmin == ~0ull ? 0 : lmin, lmax, cu);
      d.last = now;
    }
  }

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  assert_true(clients_[index].driver != NULL);
  (clients_[index].driver)->SubmitFrame(samples_ptr);
}

void AudioSystem::UnregisterClient(size_t index) {
  SCOPE_profile_cpu_f("apu");

  auto global_lock = global_critical_region_.Acquire();
  assert_true(index < kMaximumClientCount);
  DestroyDriver(clients_[index].driver);
  memory()->SystemHeapFree(clients_[index].wrapped_callback_arg);
  clients_[index] = {nullptr, 0, 0, 0, false};

  // Drain the semaphore of its count.
  auto client_semaphore = client_semaphores_[index].get();
  rex::thread::WaitResult wait_result;
  do {
    wait_result = rex::thread::Wait(client_semaphore, false, std::chrono::milliseconds(0));
  } while (wait_result == rex::thread::WaitResult::kSuccess);
  assert_true(wait_result == rex::thread::WaitResult::kTimeout);
}

bool AudioSystem::Save(stream::ByteStream* stream) {
  stream->Write(kAudioSaveSignature);

  // Count the number of used clients first.
  // Any gaps should be handled gracefully.
  uint32_t used_clients = 0;
  for (size_t i = 0; i < kMaximumClientCount; i++) {
    if (clients_[i].in_use) {
      used_clients++;
    }
  }

  stream->Write(used_clients);
  for (uint32_t i = 0; i < kMaximumClientCount; i++) {
    auto& client = clients_[i];
    if (!client.in_use) {
      continue;
    }

    stream->Write(i);
    stream->Write(client.callback);
    stream->Write(client.callback_arg);
    stream->Write(client.wrapped_callback_arg);
  }

  return true;
}

bool AudioSystem::Restore(stream::ByteStream* stream) {
  if (stream->Read<uint32_t>() != kAudioSaveSignature) {
    REXAPU_ERROR("AudioSystem::Restore - Invalid magic value!");
    return false;
  }

  uint32_t num_clients = stream->Read<uint32_t>();
  for (uint32_t i = 0; i < num_clients; i++) {
    auto id = stream->Read<uint32_t>();
    assert_true(id < kMaximumClientCount);

    auto& client = clients_[id];

    // Reset the semaphore and recreate the driver ourselves.
    if (client.driver) {
      UnregisterClient(id);
    }

    client.callback = stream->Read<uint32_t>();
    client.callback_arg = stream->Read<uint32_t>();
    client.wrapped_callback_arg = stream->Read<uint32_t>();

    client.in_use = true;

    auto client_semaphore = client_semaphores_[id].get();
    auto ret = client_semaphore->Release(queued_frames_, nullptr);
    assert_true(ret);

    AudioDriver* driver = nullptr;
    auto status = CreateDriver(id, client_semaphore, &driver);
    if (XFAILED(status)) {
      REXAPU_ERROR(
          "AudioSystem::Restore - Call to CreateDriver failed with status "
          "{:08X}",
          status);
      return false;
    }

    assert_not_null(driver);
    client.driver = driver;
  }

  return true;
}

void AudioSystem::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  // Kind of a hack, but it works.
  shutdown_event_->Set();
  pause_fence_.Wait();

  xma_decoder_->Pause();
}

void AudioSystem::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_event_->Set();

  xma_decoder_->Resume();
}

}  // namespace rex::audio
