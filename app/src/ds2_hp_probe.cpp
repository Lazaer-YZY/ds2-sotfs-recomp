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

}  // namespace

void Start(rex::Runtime* runtime) {
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
