// ds2 - bench helper: find the player's HP field in guest memory (DS2_HP_PROBE, debug launches), see ds2_hp_probe.cpp.
#pragma once

namespace rex {
class Runtime;
}

namespace ds2::hp_probe {
// Starts the probe thread if DS2_HP_PROBE lists at least two scan times (seconds after this call).
void Start(rex::Runtime* runtime);
}  // namespace ds2::hp_probe
