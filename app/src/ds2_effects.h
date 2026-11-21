// ds2 - optional post effects of the game that the player can turn off.
//
// Depth of field: the game runs its bokeh depth of field only when the GPU has headroom (it was absent at ~25 fps and
// appeared at ~50). On the Thor its focus puts the player character and the floor in front into the near blur, so the
// character looks smeared (the owner, 2026-11-17: "the forefront image looks quite blurry ... especially the coat"), and its
// full-resolution composite costs frames. Off by default: the composite draw (pixel shader 314624F986DCD563, which
// blends the half-resolution blurred scene into the 7e3 HDR scene by the focus map) is left out; the rest of the frame
// is unchanged. On Android the CTRL dialog's "Depth of field" setting (DS2_DEPTH_OF_FIELD=1 at start,
// nativeSetOption("dof") while running) turns it back on.

#pragma once

#include <string_view>

namespace ds2::effects {

// Applies ds2_depth_of_field (and DS2_DEPTH_OF_FIELD) at start.
void Apply();
// Any thread.
void SetDepthOfField(bool on);
// Anti-aliasing of the finished frame (CTRL dialog): "off", "fxaa" or "fxaa_extreme" (the SDK's swap post effect, from
// the next frame; at start the swap_post_effect cvar sets it). Any thread.
void SetAntiAliasing(std::string_view mode);

}  // namespace ds2::effects
