// ds2 - simple cheats for the CTRL dialog (the owner, 2026-11-21: "some simple cheats in CTRL menu"; asked for in
// Lazaer-YZY/ds2-sotfs-recomp#3).
//
// The player's character object (PlayerCtrl in the PC version's layout) starts with the word 0x8201F2B8 (enemies:
// 0x8201B9C8), points to itself at +0xF0 and keeps HP at +0xFC, its minimum (-99999) at +0x100, the full maximum at
// +0x104 and the cap (lowered while hollowed) at +0x108 (found by ds2_hp_probe.cpp, DECISIONS.md 2026-11-20). The
// thread below finds that object by a scan of the guest heaps (only while a cheat is on and the remembered object is
// gone: after a load or a respawn) and then, every 50 ms:
//   - "hp": HP back to the cap, and the minimum raised to 1 so a single big hit cannot kill (restored when off);
//   - "stamina": stamina back to its maximum (float at +0x140, maximum at +0x148);
// AddSouls (the CTRL button "Add 10,000 souls") adds to the souls the HUD shows (see kSouls).
// "wear": ds2_durability.cpp drops every durability loss while NoWear() is true.
//
// DS2_CHEAT_PROBE="t0,t1" (debug): from t0 seconds after start, log where the player object is referenced from, then
// every 250 ms until t1 the words of its first 0x400 bytes that changed (to find fields such as stamina).
#include "ds2_cheats.h"

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace ds2::cheats {
namespace {

constexpr uint32_t kPlayerClass = 0x8201F2B8;
constexpr uint32_t kSelf = 0xF0, kHp = 0xFC, kHpMin = 0x100, kHpCap = 0x108;
constexpr uint32_t kHpMinDefault = 0xFFFE7961;  // -99999
// Stamina (float): current at +0x140, minimum -9999 at +0x144, maximum at +0x148 (found by DS2_CHEAT_PROBE on the Thor,
// 2026-11-21: 920 -> 620 on each roll, refilling to 920 within ~1.5 s).
constexpr uint32_t kStamina = 0x140, kStaminaMax = 0x148;
// Souls: the player's object points at +0x374 to +0x28 of an object of class 0x820252C4 that keeps the souls at
// +0x138 (DS2_SOULS_PROBE / a written test value: the HUD counted up to it, 2026-11-21); +0x140 / +0x148 hold the same
// number on a fresh save but are left alone (soul memory and similar).
constexpr uint32_t kSoulsLink = 0x374, kSoulsLinkOffset = 0x28, kSoulsClass = 0x820252C4, kSouls = 0x138;

std::atomic<bool> g_hp{false}, g_stamina{false}, g_wear{false};
rex::memory::Memory* g_memory = nullptr;
uint8_t* g_base = nullptr;

inline uint32_t Load(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, g_base + a, 4);
  return __builtin_bswap32(v);
}
inline void Store(uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(g_base + a, &v, 4);
}
inline float LoadF(uint32_t a) {
  const uint32_t u = Load(a);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// Whether [lo, hi) is readable guest memory (the runtime's page tables: reading a page it keeps inaccessible would
// fault into its access-violation handler).
bool Readable(uint32_t lo, uint32_t hi) {
  rex::memory::BaseHeap* heap = g_memory->LookupHeap(lo);
  return heap && g_memory->LookupHeap(hi - 1) == heap &&
         heap->QueryRangeAccess(lo, hi - 1) != rex::memory::PageAccess::kNoAccess;
}

bool IsPlayer(uint32_t obj) {
  return obj && Readable(obj, obj + 0x400) && Load(obj) == kPlayerClass && Load(obj + kSelf) == obj;
}

// The heaps' readable 64 KB chunks in [lo, hi), merged.
std::vector<std::pair<uint32_t, uint32_t>> Ranges(uint32_t lo, uint32_t hi) {
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  constexpr uint32_t kStep = 0x10000;
  for (uint64_t a = lo; a < hi; a += kStep) {
    const uint32_t c = uint32_t(a);
    if (!Readable(c, c + kStep)) continue;
    if (!ranges.empty() && ranges.back().second == c) {
      ranges.back().second = c + kStep;
    } else {
      ranges.emplace_back(c, c + kStep);
    }
  }
  return ranges;
}

uint32_t FindPlayer() {
  for (const auto& [lo, hi] : Ranges(0x40000000u, 0xA0000000u)) {
    for (uint32_t a = lo; a + 0x400 <= hi; a += 4) {
      if (Load(a) == kPlayerClass && Load(a + kSelf) == a) return a;
    }
  }
  return 0;
}

// --- probe (debug) ------------------------------------------------------------------------------------------------
void LogReferences(uint32_t player) {
  std::vector<uint32_t> refs;
  std::vector<std::pair<uint32_t, uint32_t>> ranges = Ranges(0x40000000u, 0xA0000000u);
  for (const auto& r : Ranges(0x82000000u, 0x84000000u)) ranges.push_back(r);
  for (const auto& [lo, hi] : ranges) {
    for (uint32_t a = lo; a + 4 <= hi && refs.size() < 48; a += 4) {
      if (Load(a) == player && (a < player || a >= player + 0x400)) refs.push_back(a);
    }
  }
  std::string list;
  for (uint32_t r : refs) list += fmt::format(" {:08X}", r);
  REXLOG_INFO("ds2 cheat probe: player object {:08X} referenced from {} places:{}", player, refs.size(), list);
  // Static words (the executable's image) that point at most 0x4000 below a reference: an object holding the player
  // pointer that a global points to (a pointer chain from a fixed address).
  const auto image = Ranges(0x82000000u, 0x84000000u);
  for (size_t i = 0; i < refs.size() && i < 12; ++i) {
    const uint32_t ref = refs[i];
    std::string hits;
    int n = 0;
    for (const auto& [lo, hi] : image) {
      for (uint32_t a = lo; a + 4 <= hi && n < 8; a += 4) {
        const uint32_t w = Load(a);
        if (w <= ref && ref - w < 0x4000) {
          hits += fmt::format(" [{:08X}]={:08X}+{:X}", a, w, ref - w);
          ++n;
        }
      }
    }
    REXLOG_INFO("ds2 cheat probe:   ref {:08X}: static pointers to its object:{}", ref, n ? hits : std::string(" none"));
  }
}

void Probe(double t0, double t1) {
  const auto start = std::chrono::steady_clock::now();
  auto at = [&](double t) { return start + std::chrono::milliseconds(int64_t(t * 1000)); };
  std::this_thread::sleep_until(at(t0));
  uint32_t player = FindPlayer();
  if (!player) {
    REXLOG_INFO("ds2 cheat probe: no player object at {:.0f} s", t0);
    return;
  }
  REXLOG_INFO("ds2 cheat probe: player {:08X} HP {} / cap {}", player, Load(player + kHp), Load(player + kHpCap));
  LogReferences(player);
  uint32_t prev[256];
  for (int i = 0; i < 256; ++i) prev[i] = Load(player + i * 4);
  std::string first;
  for (int i = 0; i < 256; ++i) {
    first += fmt::format(" {:08X}", prev[i]);
    if (i % 16 == 15) {
      REXLOG_INFO("ds2 cheat probe: +{:03X}:{}", (i - 15) * 4, first);
      first.clear();
    }
  }
  for (double t = t0 + 0.25; t <= t1 && IsPlayer(player); t += 0.25) {
    std::this_thread::sleep_until(at(t));
    std::string changed;
    int n = 0;
    for (int i = 0; i < 256; ++i) {
      const uint32_t v = Load(player + i * 4);
      if (v == prev[i]) continue;
      float f;
      std::memcpy(&f, &v, 4);
      if (n++ < 40) changed += fmt::format(" +{:03X}={:08X}({:g})", i * 4, v, f);
      prev[i] = v;
    }
    if (n) REXLOG_INFO("ds2 cheat probe: {:.2f} s, {} changed:{}", t, n, changed);
  }
  REXLOG_INFO("ds2 cheat probe: done");
}

// DS2_SOULS_PROBE="t,value" (debug): at t seconds, every int32 equal to value (the souls shown in the HUD) in the
// heaps, with the nearest word behind it (within 0x800 bytes) that points into the executable's image (an object's
// class word): "hit <addr> class <word> +<offset>". Runs on saves with different soul counts share the souls field's
// (class, offset) pair.
void SoulsProbe(double t, uint32_t value) {
  std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(t * 1000)));
  int hits = 0;
  for (const auto& [lo, hi] : Ranges(0x40000000u, 0xA0000000u)) {
    for (uint32_t a = lo + 0x800; a + 4 <= hi && hits < 20000; a += 4) {
      if (Load(a) != value) continue;
      uint32_t cls = 0, off = 0;
      for (uint32_t back = 4; back <= 0x800; back += 4) {
        const uint32_t w = Load(a - back);
        if (w >= 0x82000000u && w < 0x83700000u && (w & 3) == 0) {
          cls = w;
          off = back;
          break;
        }
      }
      if (!cls) continue;
      ++hits;
      REXLOG_INFO("ds2 souls probe: hit {:08X} class {:08X} +{:X}", a, cls, off);
    }
  }
  REXLOG_INFO("ds2 souls probe: {} hits for {}", hits, value);
}

// --- the cheat thread ---------------------------------------------------------------------------------------------
void Loop() {
  setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), 10);
  uint32_t player = 0;
  bool min_raised = false;
  auto last_scan = std::chrono::steady_clock::now() - std::chrono::hours(1);
  // What the cheats did, logged at most every 30 s when they did something.
  uint32_t hp_refills = 0, stamina_refills = 0, lowest_hp = UINT32_MAX;
  auto last_report = std::chrono::steady_clock::now();
  for (;;) {
    const bool hp = g_hp.load(std::memory_order_relaxed);
    const bool stamina = g_stamina.load(std::memory_order_relaxed);
    if (!IsPlayer(player)) {
      player = 0;
      min_raised = false;
    }
    if (min_raised && !hp && player) {
      Store(player + kHpMin, kHpMinDefault);
      min_raised = false;
    }
    if (!hp && !stamina) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!player && now - last_scan > std::chrono::seconds(3)) {
      last_scan = now;
      player = FindPlayer();
      if (player) REXLOG_INFO("ds2 cheats: player character at {:08X}", player);
    }
    if (!player) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }
    if (hp) {
      if (!min_raised) {
        Store(player + kHpMin, 1);
        min_raised = true;
      }
      const uint32_t cur = Load(player + kHp), cap = Load(player + kHpCap);
      if (cur > 0 && cap <= 99999 && cur < cap) {
        Store(player + kHp, cap);
        ++hp_refills;
        lowest_hp = std::min(lowest_hp, cur);
      }
    }
    if (stamina) {
      const float cur = LoadF(player + kStamina), max = LoadF(player + kStaminaMax);
      if (max > 0.0f && max < 100000.0f && cur < max) {
        Store(player + kStamina, Load(player + kStaminaMax));
        ++stamina_refills;
      }
    }
    if ((hp_refills || stamina_refills) && now - last_report > std::chrono::seconds(30)) {
      REXLOG_INFO("ds2 cheats: {} HP refills (lowest HP {}), {} stamina refills", hp_refills, lowest_hp,
                  stamina_refills);
      hp_refills = stamina_refills = 0;
      lowest_hp = UINT32_MAX;
      last_report = now;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

}  // namespace

void Set(std::string_view name, bool on) {
  if (name == "hp") {
    g_hp = on;
  } else if (name == "stamina") {
    g_stamina = on;
  } else if (name == "wear") {
    g_wear = on;
  } else {
    return;
  }
  REXLOG_INFO("ds2 cheats: {} {}", name, on ? "on" : "off");
}

bool NoWear() { return g_wear.load(std::memory_order_relaxed); }

void AddSouls(uint32_t count) {
  if (!g_memory) return;
  // On its own thread: finding the player can scan the heaps for a second or two.
  std::thread([count] {
    const uint32_t player = FindPlayer();
    const uint32_t object = player ? Load(player + kSoulsLink) - kSoulsLinkOffset : 0;
    if (!object || !Readable(object, object + 0x200) || Load(object) != kSoulsClass) {
      REXLOG_WARN("ds2 cheats: souls not found (player {:08X}, object {:08X})", player, object);
      return;
    }
    const uint32_t before = Load(object + kSouls);
    const uint32_t after = uint32_t(std::min<uint64_t>(uint64_t(before) + count, 999999999u));
    Store(object + kSouls, after);
    REXLOG_INFO("ds2 cheats: souls {} -> {}", before, after);
  }).detach();
}

void Start(rex::Runtime* runtime) {
  if (!runtime || !runtime->memory()) return;
  g_memory = runtime->memory();
  g_base = runtime->memory()->virtual_membase();
  if (const char* list = std::getenv("DS2_CHEATS"); list && *list) {
    const std::string s(list);
    for (const char* name : {"hp", "stamina", "wear"}) {
      if (s.find(name) != std::string::npos) Set(name, true);
    }
  }
  std::thread(Loop).detach();
  // DS2_ADD_SOULS_AT="t" (debug): AddSouls(10000) at t seconds, to test the CTRL button's path.
  if (const char* at = std::getenv("DS2_ADD_SOULS_AT"); at && *at) {
    const double t = std::strtod(at, nullptr);
    std::thread([t] {
      std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(t * 1000)));
      AddSouls(10000);
    }).detach();
  }
  if (const char* spec = std::getenv("DS2_SOULS_PROBE"); spec && *spec) {
    char* end = nullptr;
    const double t = std::strtod(spec, &end);
    const uint32_t value = (end && *end == ',') ? uint32_t(std::strtoul(end + 1, nullptr, 10)) : 0;
    if (value) std::thread(SoulsProbe, t, value).detach();
  }
  if (const char* spec = std::getenv("DS2_CHEAT_PROBE"); spec && *spec) {
    char* end = nullptr;
    const double t0 = std::strtod(spec, &end);
    const double t1 = (end && *end == ',') ? std::strtod(end + 1, nullptr) : t0 + 10;
    std::thread(Probe, t0, t1).detach();
  }
}

}  // namespace ds2::cheats
