// ds2 - the local player's character object, learned from the game's own constructor and destructor (see
// ds2_player.cpp). Readers (the second screen) validate it before every use: the object can go away at any time.
#pragma once

#include <cstdint>

namespace ds2::player {

// PlayerCtrl (the PC version's name) layout on the 360, found on the Thor (DECISIONS.md 2026-11-20/21) and matching
// Atvaark's DarkSoulsII.DebugView (PC 1.0.10) up to here:
constexpr uint32_t kClass = 0x8201F2B8;  // class word at +0 (enemies' CharacterCtrl: 0x8201B9C8)
constexpr uint32_t kSelf = 0xF0;         // points to the object itself
constexpr uint32_t kType = 0x1C;         // character type: 1 the local player, 4 NPCs built as players (Majula)
constexpr uint32_t kHp = 0xFC;           // int32 current HP
constexpr uint32_t kHpMax = 0x104;       // int32 full maximum
constexpr uint32_t kHpCap = 0x108;       // int32 maximum while hollowed (Thor, 2026-11-22: 922 of 1024 after two
                                         // deaths); kHpMax + 1 without a reduction (and for NPCs)
constexpr uint32_t kStamina = 0x140;     // float current stamina
constexpr uint32_t kStaminaMax = 0x148;  // float
// Souls: +0x374 points to +0x28 of an object of class 0x820252C4 that keeps the count at +0x138.
constexpr uint32_t kSoulsLink = 0x374, kSoulsLinkOffset = 0x28, kSoulsClass = 0x820252C4, kSouls = 0x138;

// The most recently constructed player object that has not been destroyed, 0 if none. Any thread; lock-free.
// Not necessarily the local player: NPCs are player objects too (type 4 at kType; Thor, 2026-11-22: in Majula the
// newest of three was an NPC with 1100 HP and 13040 stamina). Readers choose by kType from Players().
uint32_t Object();
// How many player objects are alive now (more than one: phantoms or a menu's preview character; for the log).
int LiveCount();
// Every player object alive (class kClass), up to max, in creation order of their slots; returns the count.
int Players(uint32_t* out, int max);

// Every character object alive (CharacterCtrl: the player, enemies, NPCs), from the base constructor sub_824BB278 and
// destructor sub_824BC268; up to max into out, returns the count. Enemies' class word is 0x8201B9C8; HP at +0xFC,
// maximum at +0x104 like the player's. Any thread (entries can go away at any time: check before reading). serials
// (optional) gets each one's construction number: a new character at a reused address has a new one.
int Characters(uint32_t* out, int max, uint32_t* serials = nullptr);
constexpr uint32_t kCharacterClass = 0x8201B9C8;

}  // namespace ds2::player
