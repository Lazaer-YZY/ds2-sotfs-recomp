// ds2 - frame rate options: 30 / 40 / 60 fps and "Smoother 60" (60 fps on the 120 Hz panel mode).
//
// Dark Souls II has no frame limiter of its own in this port: it renders as fast as the presentation lets it (one or
// two vertical blanks per frame, its frame time follows). The options cap the game's swaps (VdSwap) to a whole number
// of the emulated vertical blanks (SDK cvar swap_min_vblanks) and, for 40 fps and Smoother 60, run that vertical blank
// and the panel at 120 Hz while the game is told 60 Hz (video_mode_refresh_rate, put back after setup), as the
// Lightning Returns port does:
//   30 fps          60 Hz   2 vertical blanks per frame
//   40 fps          120 Hz  3
//   60 fps          60 Hz   1
//   Smoother 60     120 Hz  2  (a frame that misses 16.7 ms shows after 25 ms instead of 33)
// Settings: DS2_FPS ("30" / "40" / "60") and DS2_VBLANK120 ("1") at start from the CTRL dialog (RuntimeActivity);
// nativeSetOption("fps") changes the cap while running (the vertical blank rate only at the next start).

#pragma once

#include <string_view>

namespace ds2::framerate {

// Before the runtime is set up (the vertical blank thread reads the refresh rate when it starts).
void Startup();
// After setup, before the game runs: the game sees 60 Hz again; applies the cap.
void AfterSetup();
// Any thread: "30", "40" or "60".
void SetFrameRate(std::string_view fps);
// The frame period the cap aims at, in ms (16.7 at 60 fps, 25 at 40, 33.3 at 30).
double TargetPeriodMs();

}  // namespace ds2::framerate
