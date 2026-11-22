// ds2 - optional post effects, see ds2_effects.h.

#include "ds2_effects.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <string>

REXCVAR_DEFINE_BOOL(ds2_depth_of_field, false, "DS2",
                    "The game's depth of field (bokeh blur composite). false = left out: sharper, and faster");

namespace rex::graphics {
void SetSkippedPixelShaders(const uint64_t* hashes, uint32_t count);  // the GPU plugin (command_processor.cpp)
}

// The GPU plugin's live swap post effect (sdk/src/graphics/plugin_main.cpp): 0 off, 1 FXAA, 2 FXAA extreme; returns 0
// while the renderer is not up.
extern "C" int32_t lr_set_post_effect(int32_t choice);

namespace ds2::effects {

namespace {
// The depth of field composite of Things Betwixt (and, presumably, the game's only one; log other candidates if a
// scene still shows the blur with this off).
constexpr uint64_t kDepthOfFieldComposite[] = {0x314624F986DCD563ull};
}  // namespace

void SetAntiAliasing(std::string_view mode) {
  const int32_t choice = mode == "fxaa_extreme" ? 2 : mode == "fxaa" ? 1 : 0;
  // FXAA on the finished 3D scene before the game's 2D layer (vulkan_scene_fxaa, the resolve feed of the scene), so
  // the HUD's and menus' text stays sharp; the swap post effect (FXAA on the whole finished frame) stays off.
  lr_set_post_effect(0);
  const bool applied = rex::cvar::SetFlagByName("vulkan_scene_fxaa", choice == 2 ? "2" : choice == 1 ? "1" : "0");
  REXLOG_INFO("ds2: anti-aliasing {}{}", choice == 2 ? "FXAA (strong)" : choice == 1 ? "FXAA" : "off",
              applied ? "" : " (vulkan_scene_fxaa not found: the start setting applies)");
}

void SetDepthOfField(bool on) {
  if (on) {
    rex::graphics::SetSkippedPixelShaders(nullptr, 0);
  } else {
    rex::graphics::SetSkippedPixelShaders(kDepthOfFieldComposite, uint32_t(std::size(kDepthOfFieldComposite)));
  }
  REXLOG_INFO("ds2: depth of field {}", on ? "on" : "off");
}

void Apply() {
  bool on = REXCVAR_GET(ds2_depth_of_field);
  if (const char* env = std::getenv("DS2_DEPTH_OF_FIELD"); env && env[0]) on = env[0] == '1';
  SetDepthOfField(on);
}

}  // namespace ds2::effects
