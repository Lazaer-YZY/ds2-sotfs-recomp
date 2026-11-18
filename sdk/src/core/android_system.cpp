/**
 * @file        rex/core/android_system.cpp
 * @brief       Minimal Android platform glue (API level, system init/shutdown)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

#include <android/api-level.h>

#include <atomic>
#include <string_view>

#include <rex/filesystem.h>
#include <rex/main_android.h>
#include <rex/memory/utils.h>
#include <rex/system.h>
#include <rex/thread.h>

namespace rex {

int32_t GetAndroidApiLevel() {
  static const int32_t api_level = android_get_device_api_level();
  return api_level;
}

// TODO(android): store the JNI application context (JavaVM*/jobject) once the
// app shell passes it in; content:// access and LaunchWebBrowser need it.
static std::atomic<bool> android_system_initialized_{false};

bool InitializeAndroidSystemForApplicationContext() {
  if (android_system_initialized_.exchange(true)) {
    return true;
  }
  memory::AndroidInitialize();
  thread::AndroidInitialize();
  filesystem::AndroidInitialize();
  return true;
}

void ShutdownAndroidSystem() {
  if (!android_system_initialized_.exchange(false)) {
    return;
  }
  filesystem::AndroidShutdown();
  thread::AndroidShutdown();
  memory::AndroidShutdown();
}

namespace filesystem {

void AndroidInitialize() {}
void AndroidShutdown() {}

bool IsAndroidContentUri(const std::string_view source) {
  return source.starts_with("content://");
}

// STUB: content:// URIs need ContentResolver.openFileDescriptor via JNI.
int OpenAndroidContentFileDescriptor(const std::string_view uri, const char* mode) {
  (void)uri;
  (void)mode;
  return -1;
}

}  // namespace filesystem

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
