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

namespace ds2::effects {

namespace {
// The depth of field composite of Things Betwixt (and, presumably, the game's only one; log other candidates if a
// scene still shows the blur with this off).
constexpr uint64_t kDepthOfFieldComposite[] = {0x314624F986DCD563ull};
// Eco mode's scene shading rate (DECISIONS.md, 2026-11-18 night).
constexpr const char* kEcoRate = "2x2";
}  // namespace

void SetDepthOfField(bool on) {
  if (on) {
    rex::graphics::SetSkippedPixelShaders(nullptr, 0);
  } else {
    rex::graphics::SetSkippedPixelShaders(kDepthOfFieldComposite, uint32_t(std::size(kDepthOfFieldComposite)));
  }
  REXLOG_INFO("ds2: depth of field {}", on ? "on" : "off");
}

void SetEcoMode(bool on) {
  // The coarse shading rate of the 3D scene (SDK gpu_eco_shading_rate, read by the GPU thread at
  // every command buffer start). DS2_ECO_RATE overrides the rate for tests ("2x1", "1x2", "2x2").
  const char* rate = std::getenv("DS2_ECO_RATE");
  const std::string value = on ? std::string(rate && rate[0] ? rate : kEcoRate) : std::string();
  if (!rex::cvar::SetFlagByName("gpu_eco_shading_rate", value)) {
    REXLOG_WARN("ds2: Eco mode: no gpu_eco_shading_rate in this runtime");
    return;
  }
  REXLOG_INFO("ds2: Eco mode {}{}", on ? "on, scene shading rate " : "off", value);
}

void Apply() {
  bool on = REXCVAR_GET(ds2_depth_of_field);
  if (const char* env = std::getenv("DS2_DEPTH_OF_FIELD"); env && env[0]) on = env[0] == '1';
  SetDepthOfField(on);
  if (const char* env = std::getenv("DS2_ECO"); env && env[0] == '1') SetEcoMode(true);
}

}  // namespace ds2::effects
