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

#include <rex/ui/surface.h>

struct ANativeWindow;

namespace rex {
namespace ui {

// Wraps the ANativeWindow owned by SDL's Android backend (SDLActivity). The
// presenter creates the VkSurfaceKHR itself via vkCreateAndroidSurfaceKHR.
// Holds its own reference (ANativeWindow_acquire/release): SDL releases the window when the
// activity pauses, and a presenter reconnecting to the stale pointer after a failed swapchain
// creation crashed in the Vulkan loader's incStrong (2026-11-12, surface churn at app start).
// With the reference the object stays valid; the dead surface only fails swapchain creation.
class AndroidNativeWindowSurface final : public Surface {
 public:
  explicit AndroidNativeWindowSurface(ANativeWindow* window);
  ~AndroidNativeWindowSurface() override;
  AndroidNativeWindowSurface(const AndroidNativeWindowSurface&) = delete;
  AndroidNativeWindowSurface& operator=(const AndroidNativeWindowSurface&) = delete;
  TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }
  ANativeWindow* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override;

 private:
  ANativeWindow* window_;
};

}  // namespace ui
}  // namespace rex
