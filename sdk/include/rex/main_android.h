#pragma once
/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <cstdint>

#include <rex/platform.h>

#if REX_PLATFORM_ANDROID

namespace rex {

// Device API level (android_get_device_api_level()), cached on first use.
int32_t GetAndroidApiLevel();

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
