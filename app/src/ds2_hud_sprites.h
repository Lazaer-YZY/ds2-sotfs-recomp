// ds2 - what the game's 2D layer draws (ds2_hud_sprites.cpp).
#pragma once

#include <cstdint>
#include <string>

namespace ds2::hud_sprites {

// Called by the 2D sprite transform hook (ds2_render_size.cpp) for sprites from the sprite draw: m = the guest
// address of the sprite's matrix (+0 scale x, +12 translation x, +20 scale y, +28 translation y; 1280x720 canvas).
void OnSpriteTransform(uint8_t* base, uint32_t m);

// The second screen's capture of the game's HUD icons (on while the second screen shows; RuntimeActivity sets it).
void SetCapture(bool on);
// While capturing: hide on the main screen what the second screen shows (HP / stamina gauge, soul counter, item slots,
// status build-up bars, effect icons). CTRL "Hide them on the main screen".
void SetHide(bool on);
// Equipment slots as the game's HUD draws them: FNV-1a 64 hashes of the icon textures' data (ds2_companion_assets.py
// maps them to icon ids), 0 = the slot shows no icon. Any thread.
struct Slots {
  uint64_t left, right, spell, item, next;
  bool hud;  // the item slots' layout was drawn in the last 0.5 s (false: menus, cutscenes, loading; keep the last values)
  bool ui;   // a layout other than the HUD's drew a sprite in the last 0.7 s (menus, dialogs, messages; not while the
             // loading screen shows)
  bool gauge;  // the HP gauge's layout was drawn in the last 0.5 s (the game shows its HUD: the player can act)
};
Slots ReadSlots();
// The covenant icon the HP gauge's emblem drew in the last second (data hash of the 64x64 vi_*.tpf texture), 0 if none.
uint64_t CovenantIcon();
// Milliseconds since the game last drew an enemy's or the boss's HP bar (0xFFFFFFFF: never). The game shows one for
// what the player hits; the second screen's damage figures count only HP drops while one shows.
uint32_t FoeBarAgeMs();
// Hashes of the 64x64 textures the 2D layer drew in the last 0.5 s (effect icons among them), up to max; returns the
// count.
int ReadSmallIcons(uint64_t* out, int max);
// Guest address of the texture data (16 KB, Xenos-tiled DXT5) of a slot icon the HUD drew recently, 0 if unknown.
uint32_t IconAddress(uint64_t hash);
// The belt item's count and name as the HUD draws them under its card (UTF-8; empty when not drawn lately).
void ReadItemText(std::string* count, std::string* name);

}  // namespace ds2::hud_sprites
