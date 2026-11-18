// ds2 - Android platform glue (implemented in ds2_android.cpp).
#pragma once

#if defined(__ANDROID__)

#include <rex/runtime.h>

#include <string>

namespace ds2::android {

// Custom Vulkan driver (Turnip) from DS2_VK_CUSTOM_DRIVER*, before the GPU plugin creates its instance.
bool LoadCustomVulkanDriver();
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
// DS2_THREAD_NICE: scheduler priority (nice) of the GPU command thread and the game's main thread (off by default).
void BoostCriticalThreads();
// Development builds only (DS2_SAVE_SNAPSHOTS=1, set by RuntimeActivity): periodic copies of the owner's save for
// benches in other areas (ds2_save_snapshots.cpp).
void StartSaveSnapshots();
// Debug launches (DS2_GPU_COUNTERS=1): Adreno hardware counters every DS2_GPU_COUNTERS_S s in the log
// (ds2_gpu_counters.cpp).
void StartGpuCounters();

}  // namespace ds2::android

#endif
