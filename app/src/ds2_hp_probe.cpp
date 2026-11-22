// ds2 - bench helper: find where the game keeps the player's HP (the owner, 2026-11-20: busy benches need a character
// that does not die; no reference port documents the field). Off unless DS2_HP_PROBE is set; debug launches only.
//
// DS2_HP_PROBE="t1,t2,...": at those seconds after the module launched, scan the guest's readable memory (virtual
// heaps and one view of physical memory, from /proc/self/maps) for big-endian int32 pairs: a current value at a and a
// maximum at a+4 / a+8 / a+12 with 1 <= current <= maximum and 100 <= maximum <= 99999 (a classic cheat search). The
// first scan records the candidates; every later scan keeps those whose maximum is unchanged and whose current value
// never rose, and marks the ones that fell. After each later scan the survivors that fell are logged (with the words
// around them) once there are few enough: run it where an enemy keeps hitting the character. DS2_HP_PROBE_FULL=1: the
// first scan keeps only pairs whose current value equals the maximum (the character at full HP then).
//
// What it found (2026-11-20, Forest of Fallen Giants, a hollow hitting the bench character): every character object
// (ChrCtrl in the PC version's layout) keeps HP at +0xFC, a minimum of -99999 at +0x100, the full maximum at +0x104
// and the cap (lowered while hollowed) at +0x108, with a pointer to the object itself at +0xF0 (HP 380 -> 156 -> 0 at
// 42B1751C; maximum 826, cap 537 = the 65 % red bar).
//
// DS2_GODMODE=1 (benches only): keeps the player's HP at its cap so a scripted busy bench is not cut short by deaths.
// The player's object is the character whose maximum is DS2_GODMODE_MAXHP (826 for the bench character); it is found
// by the signature above (a full scan, only when the remembered object no longer matches, e.g. after a load or a
// respawn, and every 30 s), and every 50 ms its HP is set back to the cap when it fell. Characters found are logged
// (object, its first word, HP / maximum / cap) to tell the player's class apart later. The thread runs at nice 19.
#include "ds2_hp_probe.h"
#include "ds2_player.h"

#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace ds2::hp_probe {
namespace {

struct Candidate {
  uint32_t addr;      // guest address of the current value
  uint32_t cur;       // last current value
  uint32_t max;       // the maximum next to it
  uint8_t off;        // where the maximum is: +4, +8 or +12
  bool fell;          // the current value fell at least once
};

inline uint32_t Load(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}

rex::memory::Memory* g_memory = nullptr;

// Readable guest ranges [lo, hi) as guest addresses, from the runtime's page tables in 64 KB steps: the virtual heaps
// and the first physical view (0xA0000000; the other two alias the same memory). /proc/self/maps is not enough: a
// read of a guest page the runtime keeps inaccessible faults into its access-violation handler (the first god mode
// scanned that way and took the game down to 3 fps with ~1 million faults).
std::vector<std::pair<uint32_t, uint32_t>> GuestRanges(const uint8_t*) {
  std::vector<std::pair<uint32_t, uint32_t>> ranges;
  if (!g_memory) return ranges;
  constexpr uint32_t kStep = 0x10000;
  for (uint64_t a = kStep; a < 0xC0000000ull; a += kStep) {
    const uint32_t lo = uint32_t(a);
    rex::memory::BaseHeap* heap = g_memory->LookupHeap(lo);
    if (!heap || heap->QueryRangeAccess(lo, lo + kStep - 1) == rex::memory::PageAccess::kNoAccess) continue;
    if (!ranges.empty() && ranges.back().second == lo) {
      ranges.back().second = lo + kStep;
    } else {
      ranges.emplace_back(lo, lo + kStep);
    }
  }
  return ranges;
}

void LogSurvivors(const uint8_t* base, const std::vector<Candidate>& list, int scan) {
  size_t fell = 0;
  for (const Candidate& c : list) fell += c.fell;
  REXLOG_INFO("ds2 hp probe: scan {}: {} candidates left, {} of them fell", scan, list.size(), fell);
  if (fell == 0 || fell > 60) return;
  for (const Candidate& c : list) {
    if (!c.fell) continue;
    std::string words;
    for (int i = -8; i < 16; ++i) {
      const uint32_t a = c.addr + uint32_t(i * 4);
      words += (i == 0 ? " [" : " ") + fmt::format("{:08X}", Load(base, a)) + (i == 0 ? "]" : "");
    }
    REXLOG_INFO("ds2 hp probe:   {:08X} cur {} max {} (+{}); words -32..+60:{}", c.addr, c.cur, c.max, c.off, words);
  }
}

void Run(const uint8_t* base, std::vector<double> times, bool full) {
  const auto start = std::chrono::steady_clock::now();
  std::vector<Candidate> list;
  int scan = 0;
  for (double t : times) {
    std::this_thread::sleep_until(start + std::chrono::milliseconds(int64_t(t * 1000)));
    const auto t0 = std::chrono::steady_clock::now();
    ++scan;
    if (scan == 1) {
      for (const auto& [lo, hi] : GuestRanges(base)) {
        for (uint32_t a = lo; a + 16 <= hi; a += 4) {
          const uint32_t cur = Load(base, a);
          if (cur == 0 || cur > 99999) continue;
          for (uint8_t off : {uint8_t(4), uint8_t(8), uint8_t(12)}) {
            const uint32_t max = Load(base, a + off);
            if (max >= 100 && max <= 99999 && (full ? cur == max : cur <= max)) {
              list.push_back({a, cur, max, off, false});
            }
          }
        }
      }
    } else {
      size_t kept = 0;
      for (Candidate c : list) {
        const uint32_t cur = Load(base, c.addr), max = Load(base, c.addr + c.off);
        if (max != c.max || cur > c.cur) continue;
        c.fell |= cur < c.cur;
        c.cur = cur;
        list[kept++] = c;
      }
      list.resize(kept);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REXLOG_INFO("ds2 hp probe: scan {} at {:.0f} s took {:.0f} ms", scan, t, ms);
    if (scan > 1) LogSurvivors(base, list, scan);
  }
}

// --- god mode ---------------------------------------------------------------------------------------------------
constexpr uint32_t kHpMinMarker = 0xFFFE7961;  // -99999

struct Chr {
  uint32_t hp_addr, cls, hp, max, cap;
};

bool IsChr(const uint8_t* base, uint32_t a) {
  return Load(base, a + 4) == kHpMinMarker && Load(base, a - 12) == a - 0xFC;
}

std::vector<Chr> FindCharacters(const uint8_t* base) {
  std::vector<Chr> out;
  for (const auto& [lo, hi] : GuestRanges(base)) {
    // The object starts 0xFC before HP; keep the reads inside the range.
    for (uint32_t a = lo + 0x100; a + 16 <= hi; a += 4) {
      if (Load(base, a + 4) != kHpMinMarker || Load(base, a - 12) != a - 0xFC) continue;
      const uint32_t hp = Load(base, a), max = Load(base, a + 8), cap = Load(base, a + 12);
      if (max == 0 || max > 99999 || cap > max || hp > max) continue;
      out.push_back({a, Load(base, a - 0xFC), hp, max, cap});
    }
  }
  return out;
}

void GodMode(const uint8_t* base, uint32_t want_max) {
  setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), 19);
  uint8_t* mem = const_cast<uint8_t*>(base);
  uint32_t player = 0;
  int refills = 0;
  auto last_scan = std::chrono::steady_clock::now() - std::chrono::hours(1);
  for (;;) {
    const auto now = std::chrono::steady_clock::now();
    const bool valid = player && IsChr(base, player) && Load(base, player + 8) == want_max;
    if (!valid || now - last_scan > std::chrono::seconds(30)) {
      last_scan = now;
      const std::vector<Chr> chrs = FindCharacters(base);
      uint32_t found = 0;
      for (const Chr& c : chrs) {
        if (c.max == want_max) found = c.hp_addr;
      }
      if (found != player) {
        std::string list;
        for (size_t i = 0; i < chrs.size() && i < 24; ++i) {
          list += fmt::format(" {:08X}:{:08X} {}/{}/{}", chrs[i].hp_addr - 0xFC, chrs[i].cls, chrs[i].hp, chrs[i].max,
                              chrs[i].cap);
        }
        REXLOG_INFO("ds2 god mode: player HP {} ({} characters: object:first word hp/max/cap{}); {} refills so far",
                    found ? fmt::format("{:08X}", found) : std::string("not found"), chrs.size(), list, refills);
        player = found;
      }
    }
    if (player && IsChr(base, player)) {
      const uint32_t hp = Load(base, player), cap = Load(base, player + 12);
      if (hp > 0 && hp < cap) {
        const uint32_t be = __builtin_bswap32(cap);
        std::memcpy(mem + player, &be, 4);
        ++refills;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(player ? 50 : 2000));
  }
}

// DS2_EQUIP_PROBE="t1,t2,..." (debug): where the game keeps the equipped rings (the second screen names the item behind
// an effect icon; the owner, 2026-11-22: "you should only list the item equipped that's triggering it"). Ring item ids
// are 40xx000n (menu/text/english/itemname.fmg). At each time: the places where two ring ids are next to each other
// (the owner's save: Stone Ring, Life Ring, then the item belt), with 48 words around them; then the pointer paths (up
// to three steps, fields 0-0xFFC) from the player object to a block holding one of them, with the distance.
bool IsRing(uint32_t v) { return v >= 40000000u && v <= 40999999u && v % 10000 < 16; }

void EquipProbe(const uint8_t* base, std::vector<double> times) {
  setpriority(PRIO_PROCESS, uint32_t(syscall(SYS_gettid)), 19);
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t scan = 0; scan < times.size(); ++scan) {
    std::this_thread::sleep_until(t0 + std::chrono::milliseconds(int64_t(times[scan] * 1000)));
    const auto ranges = GuestRanges(base);
    auto readable = [&](uint32_t x, uint32_t n) {
      for (const auto& [lo, hi] : ranges)
        if (x >= lo && x + n <= hi) return true;
      return false;
    };
    std::vector<uint32_t> targets;
    for (const auto& [lo, hi] : ranges)
      for (uint32_t x = lo; x + 8 <= hi; x += 4)
        if (IsRing(Load(base, x)) && IsRing(Load(base, x + 4))) targets.push_back(x);
    REXLOG_INFO("ds2 equip probe: scan {}: {} ring pairs", scan, targets.size());
    for (size_t i = 0; i < targets.size() && i < 0; ++i) {
      const uint32_t x = targets[i];
      std::string ctx;
      for (int k = -24; k < 24; ++k) {
        const uint32_t y = x + k * 4;
        char w[16];
        std::snprintf(w, sizeof(w), "%s%08X%s", k == 0 ? " [" : " ", readable(y, 4) ? Load(base, y) : 0u, k == 0 ? "]" : "");
        ctx += w;
      }
      REXLOG_INFO("ds2 equip probe: scan {} pair at {:08X}:{}", scan, x, ctx);
    }
    // Pointer paths from the player object (type 1) to a block that holds a pair within 0x1000 bytes.
    uint32_t players[8];
    const int n = ds2::player::Players(players, 8);
    for (int i = 0; i < n; ++i) {
      const uint32_t p = players[i];
      uint32_t type = 0;
      if (!readable(p, 0x20) || (type = Load(base, p + ds2::player::kType)) != 1) continue;
      struct Node { uint32_t obj; uint32_t path[3]; int depth; };
      std::vector<Node> queue{{p, {0, 0, 0}, 0}};
      std::vector<uint32_t> seen{p};
      int found = 0;
      for (size_t q = 0; q < queue.size() && q < 20000 && found < 30; ++q) {
        const Node nd = queue[q];
        for (uint32_t t : targets)
          if (t >= nd.obj && t < nd.obj + 0x1000) {
            REXLOG_INFO("ds2 equip probe: scan {} player {:08X} path +{:X} +{:X} +{:X} (depth {}) -> {:08X}, pair at +{:X}",
                        scan, p, nd.path[0], nd.path[1], nd.path[2], nd.depth, nd.obj, t - nd.obj);
            ++found;
          }
        if (nd.depth == 3) continue;
        for (uint32_t off = 0; off < 0x1000; off += 4) {
          if (!readable(nd.obj + off, 4)) break;
          const uint32_t v = Load(base, nd.obj + off);
          if (v < 0x40000000u || v >= 0x50000000u || (v & 3) || !readable(v, 0x40)) continue;
          if (std::find(seen.begin(), seen.end(), v) != seen.end()) continue;
          seen.push_back(v);
          Node next = nd;
          next.obj = v;
          next.path[nd.depth] = off;
          next.depth = nd.depth + 1;
          queue.push_back(next);
        }
      }
      REXLOG_INFO("ds2 equip probe: scan {} player {:08X}: {} objects searched, {} paths", scan, p, queue.size(), found);
      // Every ring id in the player object and in the objects it points to (depth 1), with the 12 words from 8 before:
      // the equipped set should sit with the weapons' and armour's item ids.
      auto list = [&](uint32_t obj, uint32_t via) {
        int k = 0;
        for (uint32_t off = 0; off < 0x1000 && k < 6; off += 4) {
          if (!readable(obj + off, 4)) break;
          if (!IsRing(Load(base, obj + off))) continue;
          ++k;
          std::string ctx;
          for (int j = -8; j < 12; ++j) {
            const uint32_t y = obj + off + j * 4;
            char w[16];
            std::snprintf(w, sizeof(w), "%s%08X%s", j == 0 ? " [" : " ", readable(y, 4) ? Load(base, y) : 0u, j == 0 ? "]" : "");
            ctx += w;
          }
          REXLOG_INFO("ds2 equip probe: scan {} via +{:X} obj {:08X} +{:X}:{}", scan, via, obj, off, ctx);
          off += 44;  // past this block
        }
      };
      // The player object's equipment entries (20 bytes: item id, ?, ?, durability, ?), +0x400 to +0x800.
      for (uint32_t off = 0x400; off < 0x800; off += 20) {
        char row[96];
        std::snprintf(row, sizeof(row), "%08X %08X %08X %08X %08X", Load(base, p + off), Load(base, p + off + 4),
                      Load(base, p + off + 8), Load(base, p + off + 12), Load(base, p + off + 16));
        REXLOG_INFO("ds2 equip probe: scan {} player +{:03X}: {}", scan, off, row);
      }
      list(p, 0xFFFF);
      for (uint32_t off = 0; off < 0x1000; off += 4) {
        if (!readable(p + off, 4)) break;
        const uint32_t v = Load(base, p + off);
        if (v >= 0x40000000u && v < 0x50000000u && !(v & 3) && readable(v, 0x40)) list(v, off);
      }
    }
  }
}

}  // namespace

void Start(rex::Runtime* runtime) {
  if (const char* spec = std::getenv("DS2_EQUIP_PROBE"); spec && *spec && runtime && runtime->memory()) {
    std::vector<double> times;
    for (const char* p = spec; *p;) {
      char* end = nullptr;
      const double t = std::strtod(p, &end);
      if (end == p) break;
      times.push_back(t);
      p = *end == ',' ? end + 1 : end;
    }
    g_memory = runtime->memory();
    REXLOG_INFO("ds2 equip probe: {} scans", times.size());
    std::thread(EquipProbe, runtime->memory()->virtual_membase(), times).detach();
  }
  if (const char* god = std::getenv("DS2_GODMODE"); god && god[0] == '1' && runtime && runtime->memory()) {
    const char* max = std::getenv("DS2_GODMODE_MAXHP");
    const uint32_t want = max ? uint32_t(std::strtoul(max, nullptr, 10)) : 0;
    g_memory = runtime->memory();
    REXLOG_INFO("ds2 god mode: on for the character with maximum HP {}", want);
    std::thread(GodMode, runtime->memory()->virtual_membase(), want).detach();
  }
  const char* spec = std::getenv("DS2_HP_PROBE");
  if (!spec || !*spec || !runtime || !runtime->memory()) return;
  std::vector<double> times;
  for (const char* p = spec; *p;) {
    char* end = nullptr;
    const double t = std::strtod(p, &end);
    if (end == p) break;
    times.push_back(t);
    p = *end == ',' ? end + 1 : end;
  }
  if (times.size() < 2) return;
  g_memory = runtime->memory();
  const uint8_t* base = runtime->memory()->virtual_membase();
  REXLOG_INFO("ds2 hp probe: {} scans from {:.0f} s", times.size(), times.front());
  const char* full = std::getenv("DS2_HP_PROBE_FULL");
  std::thread(Run, base, times, full && full[0] == '1').detach();
}

}  // namespace ds2::hp_probe
