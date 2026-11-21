// ds2 - simple cheats for the CTRL dialog (infinite HP, infinite stamina, no equipment wear), see ds2_cheats.cpp.
#pragma once

#include <cstdint>
#include <string_view>

namespace rex {
class Runtime;
}

namespace ds2::cheats {

// Starts the cheat thread (idle while every cheat is off). DS2_CHEATS="hp,stamina,wear" turns cheats on at start;
// DS2_CHEAT_PROBE (debug) logs what the field search needs. Call once the runtime's memory exists.
void Start(rex::Runtime* runtime);
// Turns one cheat on or off while running ("hp", "stamina", "wear"); any thread.
void Set(std::string_view name, bool on);
// Equipment does not wear (read by ds2_durability.cpp on every durability change).
bool NoWear();
// Adds souls to the player's (on its own thread; capped at 999,999,999).
void AddSouls(uint32_t count);

}  // namespace ds2::cheats
