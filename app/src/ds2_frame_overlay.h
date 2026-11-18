// ds2 - performance overlay: frame rate and a frame time graph of the game's own presented frames (its VdSwap
// calls), top right. Off by default; on Android the CTRL dialog's "Performance overlay" setting turns it on
// (DS2_FRAME_OVERLAY=1 at start, nativeSetOption("overlay") while running). Adapted from the Lightning Returns port.
//
// While any ImGui dialog exists the presenter paints from the UI thread (sdk/src/ui/rex_app.cpp), which can cost
// frames: the overlay is a diagnostic, not something to leave on for play.

#pragma once

#include <functional>

namespace rex::ui {
class ImGuiDrawer;
}

namespace ds2::frame_overlay {

// Call once the ImGui drawer exists (after the module launched). post_to_ui runs a function on the UI thread.
// Shows the overlay if DS2_FRAME_OVERLAY=1 or Show(true) was called before.
void Init(rex::ui::ImGuiDrawer* drawer, std::function<void(std::function<void()>)> post_to_ui);
// Any thread.
void Show(bool show);
// UI thread, before the drawer goes away.
void Shutdown();

}  // namespace ds2::frame_overlay
