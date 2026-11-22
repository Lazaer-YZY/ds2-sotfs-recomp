// ds2 - Android platform glue (implemented in ds2_android.cpp).
#pragma once

#if defined(__ANDROID__)

#include <rex/runtime.h>

#include <string>

namespace ds2::android {

// Custom Vulkan driver (Turnip) from DS2_VK_CUSTOM_DRIVER*, before the GPU plugin creates its instance.
bool LoadCustomVulkanDriver();

// Setup failed: when the Vulkan code noted why ("<GPU name>: <reason>", e.g. a missing feature), hand it to the
// activity (RuntimeActivity.reportGraphicsFailure), which opens the GPU driver page with it once the game closes.
void ReportGraphicsFailure();
// Saves the noted reason when the process aborts during renderer setup (files/graphics_failure.txt; the next start's
// GPU driver page shows it). Call before the GPU plugin is set up.
void InstallGraphicsFailureSaver();
// Adds the on-screen touch pad (fed by RuntimeActivity.nativeSetTouchInput) as a synthetic input device.
void WrapTouchInput(rex::RuntimeConfig& config);
// Android defaults for settings ds2.toml / the command line did not set (see DECISIONS.md, 2026-11-17).
void ApplyDefaults();
// For RuntimeActivity.nativeBootProgress.
void NoteModuleLaunched();
// Process-wide CPU affinity for every thread created from now on: the middle cluster (ds2_affinity.cpp).
std::string PinThreadsToFastCores();
// At 60 fps the busiest "Main XThread" may also run on the prime core (prime + middle).
void PlaceMainThread(bool sixty_fps);
// "Use the prime core" (CTRL dialog, DS2_PRIME_ASSIST at start, default on): the busiest "Main XThread", "GPU
// Commands" and "DrawCommandThread" may also run on the prime core (prime + middle; the game's job workers stay on the
// middle cluster); off = middle cluster only, like every thread. Applied to the running threads at once.
void SetPrimeAssist(bool on);
// The prime-core governor: allowed = SetPrimeAssist(true) while the game's main thread is near its frame budget on
// the CPU side, off again after 10 s of slack; not allowed = middle cluster only.
void SetPrimeGovernor(bool allowed);
// DS2_THREAD_NICE: scheduler priority (nice) of the GPU command thread and the game's main thread (off by default).
void BoostCriticalThreads();

// Scheduler hints for the job, Havok, main, GPU command and draw threads (DS2_UCLAMP_MIN, DS2_ADPF; off by default),
// see ds2_sched_hints.cpp; SchedHintsOnFrame reports a frame's busy time to the hint session, if there is one.
void ApplySchedHints();
// Pins game threads again once a second when a cpuset change reset their masks (DS2_AFFINITY_KEEPER=0: off).
void StartAffinityKeeper();
// One line for the debug log: the process's cpuset, core_ctl's active cores and where each thread group ran since
// the previous call (ds2_affinity.cpp).
std::string PlacementReport();
void SchedHintsOnFrame(double busy_ms);
// Development builds only (DS2_SAVE_SNAPSHOTS=1, set by RuntimeActivity): periodic copies of the owner's save for
// benches in other areas (ds2_save_snapshots.cpp).
void StartSaveSnapshots();
// The second screen's reader (ds2_companion_android.cpp): guest memory for RuntimeActivity.nativeCompanionState, and
// the debug probes DS2_COMPANION_PROBE / DS2_STATUS_TEST. Call once the runtime's memory exists.
void StartCompanion(rex::Runtime* runtime);
// Lock-free copy of guest memory (process_vm_readv; false when not mapped).
bool CopyGuest(uint32_t addr, void* dst, size_t len);
// Debug launches (DS2_GPU_COUNTERS=1): Adreno hardware counters every DS2_GPU_COUNTERS_S s in the log
// (ds2_gpu_counters.cpp).
void StartGpuCounters();

}  // namespace ds2::android

#endif
