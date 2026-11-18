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

#include <android/native_window.h>

#include <algorithm>

#include <rex/cvar.h>
#include <rex/ui/surface_android.h>

// Defined in vulkan_presenter.cpp (present on every platform's config).
REXCVAR_DECLARE(int32_t, present_max_height);

namespace rex {
namespace ui {

AndroidNativeWindowSurface::AndroidNativeWindowSurface(ANativeWindow* window) : window_(window) {
  if (window_) {
    ANativeWindow_acquire(window_);
  }
}

AndroidNativeWindowSurface::~AndroidNativeWindowSurface() {
  if (window_) {
    ANativeWindow_release(window_);
  }
}

bool AndroidNativeWindowSurface::GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const {
  if (!window_) {
    return false;
  }
  int32_t width = ANativeWindow_getWidth(window_);
  int32_t height = ANativeWindow_getHeight(window_);
  if (width <= 0 || height <= 0) {
    return false;
  }
  // present_max_height: report a smaller size of the same shape. The
  // presenter lays out and sizes its swapchain from this; Android's
  // compositor scales the smaller buffers to the window.
  if (int32_t max_height = REXCVAR_GET(present_max_height); max_height > 0 && height > max_height) {
    width = std::max<int32_t>(1, int32_t((int64_t(width) * max_height + height / 2) / height));
    height = max_height;
  }
  width_out = uint32_t(width);
  height_out = uint32_t(height);
  return true;
}

}  // namespace ui
}  // namespace rex
