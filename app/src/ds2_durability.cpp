// ds2 - weapon durability: SotFS's double damage on character hits fixed (ds2_durability_fix), plus diagnostics.
//
// Measured on the Thor (2026-11-20, Forest of Fallen Giants, a dagger against a hollow, ds2_durability_log): every hit
// on a character runs the weapon-strike handler sub_823F3930 twice (in the same frame or one frame apart) and each
// applies the full damage (2 x 0.2 per hit), at 60 fps and at the console's 30 fps alike; a hit on the map (a wall)
// runs it once (1 x 0.5). This is the SotFS bug eur0pa documented on PC ("SotFS applies the damage twice
// regardless", DS2Fix64, which halved it) and FromSoftware fixed in SotFS 1.02 for PC / PS4 / Xbox One; this 360
// build never got it. The fix halves what sub_823F3930 applies when the target is not the map (kind != 2), so a hit
// on a character costs what one application costs, also when one swing hits several characters. Hits on the map,
// armour and shield damage (sub_823F4248 / sub_823F4358), and the "broke" handling are unchanged.
//
// The game's durability damage path (SotFS on the 360; the same structure as the PC build's
// ComputeWeaponDurabilityDamage / ApplyDurabilityDamage that eur0pa's DS2Fix64 halved):
//   sub_823F4518(this, type r4, damage f1)            virtual: every equipped item (4 armour slots via sub_823F4248,
//                                                      2 weapon slots via sub_823F3EC0, 4 more via sub_823F4358)
//   sub_823F4658(this, type r4, damage f1, slot r5)   virtual: one slot (0-4 weapons, 6-8, 16-19 armour)
//   sub_823F3EC0(this, slot r4, damage f1, flag r6)   weapons: damage x param factor x [[this+4]+148]+1044, negated
//   sub_823F3410(this, slot r4, delta f1, r6)          apply: current durability + delta, clamped to [0, max],
//                                                      stored; returns 1 when the item broke (sub_823F3540 /
//                                                      sub_823F3668 are the same for the other equipment kinds)
// ds2_durability_log: every call with its guest caller (lr), the arguments and the durability after.

#include "ds2_cheats.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>

REXCVAR_DEFINE_BOOL(ds2_durability_fix, true, "DS2",
                    "Weapon hits on characters cost half per application: SotFS applies them twice (fixed in SotFS "
                    "1.02 on PC / PS4 / Xbox One, never on the 360); false = the game's double cost");
REXCVAR_DEFINE_BOOL(ds2_durability_log, false, "DS2",
                    "Diagnostic: log every equipment durability damage call (sub_823F4518 / sub_823F4658 / "
                    "sub_823F3EC0 / sub_823F3410) with its caller, arguments and the durability after");

extern "C" void __imp__sub_823F4518(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_823F4658(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_823F3EC0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_823F3410(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_823F3930(PPCContext& __restrict ctx, uint8_t* base);

namespace {
inline uint32_t Ld32(const uint8_t* base, uint32_t a) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a));
}
inline float LdF(const uint8_t* base, uint32_t a) {
  const uint32_t u = Ld32(base, a);
  float f;
  __builtin_memcpy(&f, &u, 4);
  return f;
}
// Set while sub_823F3930 handles a hit on a character: its apply call (return address 823F3A74) is halved.
thread_local bool t_halve_strike = false;
constexpr uint32_t kStrikeApplyReturn = 0x823F3A74;
double NowMs() {
  return double(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()) /
         1000.0;
}
}  // namespace

REX_HOOK_RAW(sub_823F4518) {
  static const bool log = REXCVAR_GET(ds2_durability_log);
  if (log) {
    REXLOG_INFO("ds2 durability: {:.1f} all-slots lr={:08X} this={:08X} type={} damage={:.3f}", NowMs(),
                uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.s32, ctx.f1.f64);
  }
  __imp__sub_823F4518(ctx, base);
}

REX_HOOK_RAW(sub_823F4658) {
  static const bool log = REXCVAR_GET(ds2_durability_log);
  if (log) {
    REXLOG_INFO("ds2 durability: {:.1f} one-slot lr={:08X} this={:08X} type={} slot={} damage={:.3f}", NowMs(),
                uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.s32, ctx.r5.s32, ctx.f1.f64);
  }
  __imp__sub_823F4658(ctx, base);
}

REX_HOOK_RAW(sub_823F3EC0) {
  static const bool log = REXCVAR_GET(ds2_durability_log);
  if (log) {
    REXLOG_INFO("ds2 durability: {:.1f} weapon lr={:08X} slot={} damage={:.3f} flag={}", NowMs(), uint32_t(ctx.lr),
                ctx.r4.s32, ctx.f1.f64, ctx.r6.u32 & 0xFF);
  }
  __imp__sub_823F3EC0(ctx, base);
}

REX_HOOK_RAW(sub_823F3410) {
  static const bool log = REXCVAR_GET(ds2_durability_log);
  // Benches with god mode (DS2_GODMODE=1, ds2_hp_probe.cpp): equipment does not wear, or a long fight breaks the
  // weapon and armour and the enemies stop dying (2026-11-20).
  static const bool bench_no_wear = [] {
    const char* god = std::getenv("DS2_GODMODE");
    return god && god[0] == '1';
  }();
  if (t_halve_strike && uint32_t(ctx.lr) == kStrikeApplyReturn) {
    ctx.f1.f64 *= 0.5;  // f1 = the (negative) change; a float value, halved exactly
  }
  // The CTRL cheat "No equipment wear" (ds2_cheats.cpp) does the same.
  if ((bench_no_wear || ds2::cheats::NoWear()) && ctx.f1.f64 < 0) ctx.f1.f64 = 0;
  if (!log) {
    __imp__sub_823F3410(ctx, base);
    return;
  }
  const uint32_t lr = uint32_t(ctx.lr), slot = ctx.r4.u32;
  const double delta = ctx.f1.f64;
  __imp__sub_823F3410(ctx, base);
  // The function ends by reading the current durability back into f1 (when the slot holds an item).
  REXLOG_INFO("ds2 durability: {:.1f} apply lr={:08X} slot={} delta={:.3f} -> durability {:.3f} broke={}", NowMs(),
              lr, slot, delta, ctx.f1.f64, ctx.r3.u32);
}

// The weapon struck something (virtual; r4 = the hit record: +89 hand, +384 target kind in the top nibble; r6 = the
// attack's parameters, +24 the durability damage).
REX_HOOK_RAW(sub_823F3930) {
  static const bool log = REXCVAR_GET(ds2_durability_log);
  static const bool fix = REXCVAR_GET(ds2_durability_fix);
  const uint32_t hit = ctx.r4.u32;
  // Only a hit with a hand (+89) applies anything; the target kind is the top nibble of +384 (2 = the map).
  const bool character_hit = hit && base[hit + 89] != 0 && (Ld32(base, hit + 384) >> 28) != 2;
  if (log && hit && base[hit + 89] != 0) {
    const uint32_t atk = ctx.r6.u32;
    REXLOG_INFO("ds2 durability: {:.1f} weapon-hit lr={:08X} this={:08X} hit={:08X} hand={} kind={:08X} atk={:08X} "
                "atk_damage={:.3f}",
                NowMs(), uint32_t(ctx.lr), ctx.r3.u32, hit, hit ? base[hit + 89] : 0, hit ? Ld32(base, hit + 384) : 0,
                atk, atk ? LdF(base, atk + 24) : 0.0f);
  }
  const bool outer = t_halve_strike;  // a nested strike restores the outer call's state
  t_halve_strike = fix && character_hit;
  __imp__sub_823F3930(ctx, base);
  t_halve_strike = outer;
}
