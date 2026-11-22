// ds2 - the local player's character object, for the second screen (android/ds2_companion_android.cpp).
//
// The game builds the player's PlayerCtrl in sub_82522B60 (it calls the CharacterCtrl constructor sub_824BB278, then
// stores the class word 0x8201F2B8 at +0 and clears +0x370 / +0x374) and destroys it in sub_82522EC0 (the deleting
// destructor: class word again, base destructor sub_824BC268, free when bit 0 of r4 is set). Both hooks only record
// the object's address, so finding the player costs nothing (the CTRL cheats scan the heaps instead,
// ds2_cheats.cpp). Readers still check the class word and the self pointer at +0xF0 before each use: the destructor
// can also be reached another way, and the base destructor overwrites the class word.
#include "ds2_player.h"

#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>

extern "C" void __imp__sub_82522B60(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82522EC0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_824BB278(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_824BC268(PPCContext& __restrict ctx, uint8_t* base);

namespace ds2::player {
void AddCharacter(uint32_t obj);
void RemoveCharacter(uint32_t obj);
namespace {
// Live player objects, newest last (offline play has one; phantoms or preview characters may add some).
constexpr int kSlots = 8;
std::atomic<uint32_t> g_live[kSlots];
std::atomic<uint32_t> g_newest{0};

void Add(uint32_t obj) {
  for (auto& slot : g_live) {
    uint32_t empty = 0;
    if (slot.compare_exchange_strong(empty, obj, std::memory_order_acq_rel)) break;
  }
  g_newest.store(obj, std::memory_order_release);
}

void Remove(uint32_t obj) {
  for (auto& slot : g_live) {
    uint32_t expected = obj;
    slot.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
  }
  uint32_t newest = obj;
  if (g_newest.compare_exchange_strong(newest, 0, std::memory_order_acq_rel)) {
    // The newest one went away: fall back to another live one, if any.
    for (auto& slot : g_live) {
      const uint32_t other = slot.load(std::memory_order_acquire);
      if (other) {
        g_newest.store(other, std::memory_order_release);
        break;
      }
    }
  }
}
}  // namespace

// Every character (lock-free slots; 0 = free, kClaimed = being filled) with a serial number per construction: the
// game builds new characters at the addresses of ones it just destroyed, and the second screen's damage figures must
// not take the new one's HP for a hit on the old one (the owner, 2026-11-22: "last hit 3070 and chain 3350").
constexpr int kCharacterSlots = 512;
constexpr uint32_t kClaimed = 1;
std::atomic<uint32_t> g_characters[kCharacterSlots];
std::atomic<uint32_t> g_character_serials[kCharacterSlots];
std::atomic<uint32_t> g_character_serial{0};

void AddCharacter(uint32_t obj) {
  const uint32_t serial = g_character_serial.fetch_add(1, std::memory_order_relaxed) + 1;
  for (int i = 0; i < kCharacterSlots; ++i) {
    uint32_t empty = 0;
    if (g_characters[i].compare_exchange_strong(empty, kClaimed, std::memory_order_acq_rel)) {
      g_character_serials[i].store(serial, std::memory_order_relaxed);
      g_characters[i].store(obj, std::memory_order_release);  // the serial is visible with the object
      return;
    }
  }
}

void RemoveCharacter(uint32_t obj) {
  for (auto& slot : g_characters) {
    uint32_t expected = obj;
    if (slot.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) return;
  }
}

uint32_t Object() { return g_newest.load(std::memory_order_acquire); }

int Characters(uint32_t* out, int max, uint32_t* serials) {
  int n = 0;
  for (int i = 0; i < kCharacterSlots && n < max; ++i) {
    const uint32_t c = g_characters[i].load(std::memory_order_acquire);
    if (c <= kClaimed) continue;
    if (serials) serials[n] = g_character_serials[i].load(std::memory_order_relaxed);
    out[n++] = c;
  }
  return n;
}

int Players(uint32_t* out, int max) {
  int n = 0;
  for (auto& slot : g_live) {
    const uint32_t c = slot.load(std::memory_order_acquire);
    if (c && n < max) out[n++] = c;
  }
  return n;
}

int LiveCount() {
  int n = 0;
  for (auto& slot : g_live) n += slot.load(std::memory_order_relaxed) != 0;
  return n;
}

}  // namespace ds2::player

REX_HOOK_RAW(sub_82522B60) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82522B60(ctx, base);
  ds2::player::Add(self);
  REXLOG_INFO("ds2 player: object {:08X} created ({} alive)", self, ds2::player::LiveCount());
}

// CharacterCtrl's constructor (class word 0x8201B9C8) and destructor: every character, enemies included (the second
// screen's damage figures).
REX_HOOK_RAW(sub_824BB278) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_824BB278(ctx, base);
  ds2::player::AddCharacter(self);
}

REX_HOOK_RAW(sub_824BC268) {
  ds2::player::RemoveCharacter(ctx.r3.u32);
  __imp__sub_824BC268(ctx, base);
}

REX_HOOK_RAW(sub_82522EC0) {
  const uint32_t self = ctx.r3.u32;
  ds2::player::Remove(self);
  REXLOG_INFO("ds2 player: object {:08X} destroyed ({} alive)", self, ds2::player::LiveCount());
  __imp__sub_82522EC0(ctx, base);
}
