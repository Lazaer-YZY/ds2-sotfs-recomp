// ds2 - the bottom screen of dual-screen devices (CompanionScreen.java): what the game's HUD shows, as one line of
// key=value pairs, polled by the screen's own drawing thread (CompanionSurface.java) through
// RuntimeActivity.nativeCompanionState. Never called from a game thread.
//
// Reads take no runtime lock (the Lightning Returns port's lesson, its DECISIONS.md 2026-11-12: a low-priority
// reader that asked the guest memory's page tables held the lock every game allocation takes and stalled the game).
// They copy through process_vm_readv on our own process instead: the kernel copies what is mapped and fails on
// anything that is not, so a stale pointer during a load cannot fault this thread.
//
// The state line (CompanionState.java ignores unknown keys and keeps defaults for missing ones):
//   v=1                       format version
//   player=0|1                the local player exists (ds2_player.cpp); nothing else is sent without it
//   pid                       its object's address (hex): a load or a warp builds a new one
//   hp, hpmax, hpcap          HP, its full maximum, its maximum while hollowed (= hpmax when human)
//   st, stmax                 stamina (floats, rounded)
//   souls                     the HUD's soul count
//   tol=<name>:<cur>:<max>,.. status build-up the game's HUD shows as bars (l01_06_tolerance): only non-zero ones
//   hud=0|1                   the game's item slots were drawn in the last 0.5 s (0: menus, cutscenes, loading)
//   gauge=0|1                 the game's HP gauge was drawn in the last 0.5 s (its HUD is up: the player can act)
//   ui=0|1                    another layout drew in the last 0.7 s (menus, dialogs, messages; 0 while the loading
//                             screen shows); hud=0 and ui=0 for a while: a cutscene, a movie, a warp (the second
//                             screen dims)
//   eqL, eqR, eqU, eqD, eqN   the icons in the left hand, right hand, spell, item and next-item slots as the game's
//                             HUD draws them: FNV-1a 64 of the icon texture data in hex (0 = empty); the second
//                             screen's icons.json maps them to icon / item ids (ds2_hud_sprites.cpp)
//   fx=<hash>,...             64x64 textures drawn in the last 0.5 s (effect icons among them, mapped the same way)
//   cnt, name                 the belt item's count and name as the HUD draws them (the game's language)
//   hit, hitage, combo        the last hit on an enemy (HP lost between two polls while the game shows an enemy's or
//                             the boss's bar), ms since, and the sum of
//                             hits less than 3 s apart; tgt=<hp>:<max> that enemy now; taken = the player's last loss
//
// Debug launches (development builds forward DS2_* extras as environment variables):
//   DS2_COMPANION_PROBE=1     logs the player object's words +0x140..+0x260 whenever they change (every 250 ms)
//   DS2_STATUS_TEST="t,OFF:FRAC,..."  from t seconds after the player appears, for DS2_STATUS_TEST_SECS (60) s,
//                             holds the float at player+OFF at FRAC times the float at player+OFF+8 (a build-up and
//                             its maximum), so the game's own HUD shows which bar each offset is.
//   DS2_EQUIP_PROBE="t,ID,ID,..."  t seconds after the player appears: every int32 equal to one of the item ids in the
//                             heaps (with the nearest class word behind it), then every 250 ms the words that changed
//                             within 0x100 of each hit and in the player object's first 0x800 bytes (equipment
//                             cycling with the d-pad shows the active-slot fields).
//   DS2_EQUIP_PROBE2=t        t seconds after the player appears: the objects the player points to (two levels, the
//                             equipment and player data among them) are logged, then every 250 ms the words that
//                             changed in each (objects with more than 16 changes in a tick are reported as noisy).
#if defined(__ANDROID__)

#include "android/ds2_android.h"
#include "ds2_hud_sprites.h"
#include "ds2_player.h"

#include <rex/logging.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <android/log.h>
#include <jni.h>
#include <sched.h>
#include <sys/uio.h>
#include <unistd.h>

namespace ds2::android {
namespace {

std::atomic<uint8_t*> g_base{nullptr};

bool Copy(uint32_t addr, void* dst, size_t len) {
  uint8_t* base = g_base.load(std::memory_order_relaxed);
  if (!base || addr < 0x10000 || uint64_t(addr) + len > 0x100000000ull) return false;
  struct iovec local{dst, len};
  struct iovec remote{base + addr, len};
  return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == ssize_t(len);
}

bool U32(uint32_t addr, uint32_t* v) {
  uint32_t raw;
  if (!Copy(addr, &raw, 4)) return false;
  *v = __builtin_bswap32(raw);
  return true;
}

bool F32(uint32_t addr, float* f) {
  uint32_t u;
  if (!U32(addr, &u)) return false;
  std::memcpy(f, &u, 4);
  return std::isfinite(*f);
}

// The local player's object: of the live player objects (NPCs are some too), the one of character type 1 that still
// looks like one (class word, self pointer); 0 if none.
uint32_t Player() {
  uint32_t players[8];
  const int n = ds2::player::Players(players, 8);
  for (int i = 0; i < n; ++i) {
    const uint32_t obj = players[i];
    uint32_t cls = 0, self = 0, type = 0;
    if (U32(obj, &cls) && cls == ds2::player::kClass && U32(obj + ds2::player::kSelf, &self) && self == obj &&
        U32(obj + ds2::player::kType, &type) && type == 1)
      return obj;
  }
  return 0;
}

// Status build-up: seven (current, minimum 0, maximum 100) float triples after stamina, +0x158 to +0x1A0 (Thor probe,
// 2026-11-21; +0x14C is a full 500.3 triple, +0x1AC rises to 0.3 and is not a build-up, unlike PC 1.0.10's layout).
// Mapped with DS2_STATUS_TEST against the game's own bars, one triple at a time (DUAL_SCREEN_NOTES.md): +0x158 poison
// (green drop), +0x17C curse, +0x188 toxic (the poison flask in red), +0x1A0 petrification (white crystal). The game
// draws no bar for +0x164, +0x170 or +0x194, so neither does the second screen; bleed is not found yet.
struct Buildup {
  const char* name;
  uint32_t offset;
};
constexpr Buildup kBuildups[] = {
    {"poison", 0x158}, {"toxic", 0x188}, {"curse", 0x17C}, {"petrify", 0x1A0},
};

void AppendBuildups(uint32_t player, std::string& out) {
  std::string list;
  for (const Buildup& b : kBuildups) {
    float cur = 0, max = 0;
    if (!F32(player + b.offset, &cur) || !F32(player + b.offset + 8, &max) || max <= 0 || cur <= 0) continue;
    char item[64];
    std::snprintf(item, sizeof(item), "%s%s:%.1f:%.1f", list.empty() ? "" : ",", b.name, cur, max);
    list += item;
  }
  if (!list.empty()) out += ";tol=" + list;
}

// DS2_COMPANION_PROBE=1 (debug): logs the slots whenever they change, the player's coming and going and every HP drop
// the damage figures consider. To logcat (tag ds2recomp): REXLOG lines from this JNI thread never reach ds2.log (Thor,
// 2026-11-22).
bool CompanionProbe() {  // read every time: the second screen polls before the launch extras are in the environment
  const char* v = std::getenv("DS2_COMPANION_PROBE");
  return v && *v == '1';
}

// Damage figures: HP drops of the characters between two polls (the reader runs every 100 ms; this thread only).
// A drop counts as the player's hit only if it is one (the owner, 2026-11-22: "last hit 3070 and chain 3350 ... target
// popping up with nothing targeted"):
// - the same character: same address and construction serial (the game builds new characters where it just destroyed
//   others) and the same maximum HP, and the old HP not above it;
// - the game shows an enemy's or the boss's HP bar within ~0.4 s of the drop (it shows one for what the player hits;
//   not for characters hurt elsewhere, killed or reset by the game's scripts).
struct Track {
  uint32_t obj = 0, serial = 0;
  int32_t hp = 0, max = 0;
  uint32_t seen = 0;
};
Track g_tracks[512];
struct Pending {  // a drop waiting for the game's bar
  uint32_t obj = 0, poll = 0;
  int32_t lost = 0, hp = 0, max = 0;
};
Pending g_pending[16];
constexpr uint32_t kPendingPolls = 4;    // how long a drop waits for the bar
constexpr uint32_t kFoeBarRecentMs = 2500;  // a bar this recent (it stays up a while after a hit) confirms a drop
uint32_t g_poll = 0;
int32_t g_last_hit = 0, g_combo = 0, g_player_hp = -1, g_taken = 0;
uint32_t g_last_target = 0;
std::chrono::steady_clock::time_point g_last_hit_time;
bool g_no_player = true;  // set while there is no player (title screen, loading): the next one starts afresh

void CountHit(const Pending& d, std::chrono::steady_clock::time_point now) {
  const bool chain = g_last_hit && now - g_last_hit_time < std::chrono::seconds(3);
  g_combo = chain ? g_combo + d.lost : d.lost;
  g_last_hit = d.lost;
  g_last_target = d.obj;
  g_last_hit_time = now;
}

void TrackDamage(uint32_t player, int32_t player_hp, int32_t player_cap, std::string& out) {
  // A load, a warp or the title screen in between: the figures start afresh (the owner, 2026-11-22: last hit and taken
  // persisted from the previous game; a warp or a reload builds a new player object, and it may reuse the address).
  static uint32_t s_player = 0;
  if (player != s_player || g_no_player) {
    s_player = player;
    g_no_player = false;
    g_player_hp = -1;
    g_taken = g_last_hit = g_combo = 0;
    g_last_target = 0;
    for (Track& k : g_tracks) k = Track{};  // the old area's characters (their addresses get reused)
    for (Pending& d : g_pending) d = Pending{};
  }
  ++g_poll;
  const bool probe = CompanionProbe();
  uint32_t chars[512], serials[512];
  const int n = ds2::player::Characters(chars, 512, serials);
  const auto now = std::chrono::steady_clock::now();
  for (int i = 0; i < n; ++i) {
    const uint32_t c = chars[i];
    uint32_t cls = 0, hp = 0, max = 0, self = 0;
    if (c == player || !U32(c, &cls) || cls != ds2::player::kCharacterClass || !U32(c + ds2::player::kSelf, &self) ||
        self != c || !U32(c + ds2::player::kHp, &hp) || !U32(c + ds2::player::kHpMax, &max))
      continue;
    Track* t = nullptr;
    Track* free_slot = nullptr;
    for (Track& k : g_tracks) {
      if (k.obj == c && k.serial == serials[i]) {
        t = &k;
        break;
      }
      if (!free_slot && (k.obj == 0 || k.obj == c || g_poll - k.seen > 50)) free_slot = &k;
    }
    if (!t) {  // first seen: its HP is the baseline
      if (!free_slot) continue;
      t = free_slot;
      *t = Track{c, serials[i], int32_t(hp), int32_t(max), g_poll};
      continue;
    }
    t->seen = g_poll;
    const int32_t lost = t->hp - int32_t(hp);
    if (lost > 0) {
      // A killing blow leaves the HP below 0 (Thor: 56 -> -13); the game keeps it above its minimum, -99999.
      const bool same = int32_t(max) == t->max && t->max > 0 && t->hp <= t->max && int32_t(hp) > -100000;
      if (probe || (!same && lost >= 500))
        __android_log_print(ANDROID_LOG_INFO, "ds2recomp", "companion hp drop: %d on %08X #%u (%d -> %d of %d, was of %d)%s",
                            lost, c, serials[i], t->hp, int32_t(hp), int32_t(max), t->max, same ? "" : " ignored");
      if (same) {
        Pending* slot = &g_pending[0];
        for (Pending& d : g_pending)
          if (d.poll == 0 || g_poll - d.poll > g_poll - slot->poll) {
            slot = &d;
            if (d.poll == 0) break;
          }
        *slot = Pending{c, g_poll, lost, int32_t(hp), int32_t(max)};
      }
    }
    t->hp = int32_t(hp);
    t->max = int32_t(max);
  }
  // Drops are counted in the order seen, once the game's bar confirms them, or dropped after kPendingPolls.
  const uint32_t bar_age = ds2::hud_sprites::FoeBarAgeMs();
  for (;;) {
    Pending* first = nullptr;
    for (Pending& d : g_pending)
      if (d.poll && (!first || d.poll < first->poll)) first = &d;
    if (!first) break;
    if (bar_age < kFoeBarRecentMs) {
      CountHit(*first, now);
    } else if (g_poll - first->poll >= kPendingPolls) {
      if (probe || first->lost >= 500)
        __android_log_print(ANDROID_LOG_INFO, "ds2recomp", "companion hp drop: %d on %08X (%d of %d) not counted: no enemy "
                          "bar (last %u ms ago)", first->lost, first->obj, first->hp, first->max, bar_age);
    } else {
      break;  // wait for the bar
    }
    *first = Pending{};
  }
  // A drop from above the (hollowed) cap is the game clamping HP after a load, not damage (Thor, 2026-11-22: a 50 %
  // hollow character loaded with 546 and was cut to 274: "Taken 272").
  if (g_player_hp >= 0 && player_hp < g_player_hp && g_player_hp <= player_cap) g_taken = g_player_hp - player_hp;
  g_player_hp = player_hp;
  if (g_taken) out += ";taken=" + std::to_string(g_taken);
  if (!g_last_hit) return;
  const long age = long(std::chrono::duration_cast<std::chrono::milliseconds>(now - g_last_hit_time).count());
  char buf[160];
  std::snprintf(buf, sizeof(buf), ";hit=%d;hitage=%ld;combo=%d", g_last_hit, age, g_combo);
  out += buf;
  uint32_t cls = 0, hp = 0, max = 0;
  if (U32(g_last_target, &cls) && cls == ds2::player::kCharacterClass && U32(g_last_target + ds2::player::kHp, &hp) &&
      U32(g_last_target + ds2::player::kHpMax, &max)) {
    std::snprintf(buf, sizeof(buf), ";tgt=%d:%d", int32_t(hp), int32_t(max));
    out += buf;
  }
}

// Text for the state line: ';' and '=' would end a field.
std::string Clean(std::string s) {
  for (char& c : s)
    if (c == ';' || c == '=') c = ',';
  return s;
}

std::string State() {
  std::string out = "v=1";
  const uint32_t p = Player();
  static int last_player = -1;
  if (CompanionProbe() && int(p != 0) != last_player)
    __android_log_print(ANDROID_LOG_INFO, "ds2recomp", "companion state: player=%d", p ? 1 : 0);
  last_player = int(p != 0);
  if (!p) {
    g_no_player = true;
    return out + ";player=0";
  }
  uint32_t hp = 0, hpmax = 0, hpcap = 0;
  float st = 0, stmax = 0;
  if (!U32(p + ds2::player::kHp, &hp) || !U32(p + ds2::player::kHpMax, &hpmax) ||
      !U32(p + ds2::player::kHpCap, &hpcap) || !F32(p + ds2::player::kStamina, &st) ||
      !F32(p + ds2::player::kStaminaMax, &stmax))
    return out + ";player=0";
  char buf[256];
  std::snprintf(buf, sizeof(buf), ";player=1;pid=%x;hp=%d;hpmax=%d;hpcap=%d;st=%.0f;stmax=%.0f", p, int32_t(hp),
                int32_t(hpmax), int32_t(hpcap), st, stmax);
  out += buf;
  uint32_t link = 0, cls = 0, souls = 0;
  if (U32(p + ds2::player::kSoulsLink, &link) && link > ds2::player::kSoulsLinkOffset) {
    const uint32_t obj = link - ds2::player::kSoulsLinkOffset;
    if (U32(obj, &cls) && cls == ds2::player::kSoulsClass && U32(obj + ds2::player::kSouls, &souls)) {
      out += ";souls=" + std::to_string(souls);
    }
  }
  AppendBuildups(p, out);
  const ds2::hud_sprites::Slots slots = ds2::hud_sprites::ReadSlots();
  char eq[200];
  std::snprintf(eq, sizeof(eq), ";hud=%d;ui=%d;gauge=%d;eqL=%llx;eqR=%llx;eqU=%llx;eqD=%llx;eqN=%llx", slots.hud ? 1 : 0,
                slots.ui ? 1 : 0, slots.gauge ? 1 : 0,
                (unsigned long long)slots.left, (unsigned long long)slots.right, (unsigned long long)slots.spell,
                (unsigned long long)slots.item, (unsigned long long)slots.next);
  out += eq;
  static std::string last_eq;
  if (CompanionProbe() && last_eq != eq) {
    last_eq = eq;
    __android_log_print(ANDROID_LOG_INFO, "ds2recomp", "companion state: slots%s", eq);
  }
  std::string count, name;
  ds2::hud_sprites::ReadItemText(&count, &name);
  if (!count.empty()) out += ";cnt=" + Clean(count);
  if (!name.empty()) out += ";name=" + Clean(name);
  TrackDamage(p, int32_t(hp), std::min(int32_t(hpcap), int32_t(hpmax)), out);
  uint64_t small[12];
  const int n = ds2::hud_sprites::ReadSmallIcons(small, 12);
  {  // DS2_PLAYER_PROBE=1 (debug): the line as the second screen gets it, every 3 s
    static const bool pprobe = [] {
      const char* v = std::getenv("DS2_PLAYER_PROBE");
      return v && *v == '1';
    }();
    static auto last = std::chrono::steady_clock::now();
    if (pprobe && std::chrono::steady_clock::now() - last > std::chrono::seconds(3)) {
      last = std::chrono::steady_clock::now();
      REXLOG_INFO("ds2 player probe: state {}", out);
    }
  }
  if (n) {
    out += ";fx=";
    for (int i = 0; i < n; ++i) {
      char h[24];
      std::snprintf(h, sizeof(h), "%s%llx", i ? "," : "", (unsigned long long)small[i]);
      out += h;
    }
  }
  return out;
}

// --- debug -------------------------------------------------------------------------------------------------------
void ProbeLoop() {
  constexpr uint32_t kLo = 0x140, kHi = 0x260;
  uint32_t prev[(kHi - kLo) / 4] = {};
  uint32_t prev_player = 0;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const uint32_t p = Player();
    if (!p) continue;
    std::string changed;
    int n = 0;
    for (uint32_t i = 0; i < (kHi - kLo) / 4; ++i) {
      uint32_t v = 0;
      if (!U32(p + kLo + i * 4, &v)) break;
      if (p == prev_player && v == prev[i]) continue;
      float f;
      std::memcpy(&f, &v, 4);
      char item[48];
      std::snprintf(item, sizeof(item), " +%03X=%08X(%g)", kLo + i * 4, v, f);
      changed += item;
      prev[i] = v;
      ++n;
    }
    if (n) REXLOG_INFO("ds2 companion probe: player {:08X}{}:{}", p, p == prev_player ? "" : " (new)", changed);
    prev_player = p;
  }
}

void StatusTest(std::string spec) {
  const double t = std::strtod(spec.c_str(), nullptr);
  std::vector<std::pair<uint32_t, float>> items;
  for (size_t at = spec.find(','); at != std::string::npos; at = spec.find(',', at + 1)) {
    char* end = nullptr;
    const uint32_t off = uint32_t(std::strtoul(spec.c_str() + at + 1, &end, 16));
    if (end && *end == ':') items.emplace_back(off, std::strtof(end + 1, nullptr));
  }
  uint32_t p = 0;
  while (!(p = Player())) std::this_thread::sleep_for(std::chrono::milliseconds(500));
  std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(t * 1000)));
  const char* secs = std::getenv("DS2_STATUS_TEST_SECS");
  const int ticks = (secs && *secs ? std::atoi(secs) : 60) * 10;
  // DS2_STATUS_TEST_STEP=s: one item at a time, s seconds each (the previous one set back to 0), in the given order.
  const char* step_env = std::getenv("DS2_STATUS_TEST_STEP");
  const int step = step_env && *step_env ? std::atoi(step_env) * 10 : 0;
  REXLOG_INFO("ds2 companion: status test on player {:08X}, {} values for {} s{}", p, items.size(), ticks / 10,
              step ? fmt::format(", {} s each", step / 10) : std::string());
  uint8_t* base = g_base.load();
  auto put = [&](uint32_t off, float frac) {
    float max = 0;
    if (!F32(p + off + 8, &max) || max <= 0) return;
    const float v = frac * max;
    uint32_t u;
    std::memcpy(&u, &v, 4);
    u = __builtin_bswap32(u);
    std::memcpy(base + p + off, &u, 4);
  };
  size_t current = SIZE_MAX;
  for (int i = 0; i < ticks && Player() == p; ++i) {
    if (step) {
      const size_t k = size_t(i / step);
      if (k >= items.size()) break;
      if (k != current) {
        if (current != SIZE_MAX) put(items[current].first, 0);
        current = k;
        REXLOG_INFO("ds2 companion: status test now +{:X} at {:.2f}", items[k].first, items[k].second);
      }
      put(items[k].first, items[k].second);
    } else {
      for (const auto& [off, frac] : items) put(off, frac);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (current != SIZE_MAX) put(items[current].first, 0);
  REXLOG_INFO("ds2 companion: status test done");
}

// DS2_PLAYER_PROBE=1 (debug): every 3 s, each live player object (character type at +0x1C: 1 the local player, 4 an
// NPC built as a player; ids +0x14 / +0x18; HP, cap, maximum, stamina, the souls link) and the state line.
void PlayerProbe() {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(3));
    uint32_t players[8];
    const int n = ds2::player::Players(players, 8);
    std::string line;
    for (int i = 0; i < n; ++i) {
      const uint32_t p = players[i];
      uint32_t type = 0, id1 = 0, id2 = 0, hp = 0, cap = 0, max = 0, link = 0, cls = 0;
      float st = 0, stmax = 0;
      U32(p, &cls);
      U32(p + 0x14, &id1);
      U32(p + 0x18, &id2);
      U32(p + 0x1C, &type);
      U32(p + ds2::player::kHp, &hp);
      U32(p + ds2::player::kHpCap, &cap);
      U32(p + ds2::player::kHpMax, &max);
      F32(p + ds2::player::kStamina, &st);
      F32(p + ds2::player::kStaminaMax, &stmax);
      U32(p + ds2::player::kSoulsLink, &link);
      line += fmt::format(" [{:08X} cls {:08X} type {} id {:X}/{:X} hp {}/{}/{} st {:.0f}/{:.0f} link {:08X}]", p, cls,
                          type, id1, id2, int32_t(hp), int32_t(cap), int32_t(max), st, stmax, link);
    }
    REXLOG_INFO("ds2 player probe: newest {:08X}, {} alive:{}", ds2::player::Object(), n, line);
  }
}

// --- equipment probe (debug) ----------------------------------------------------------------------------------------
void EquipProbe(std::string spec) {
  const double t = std::strtod(spec.c_str(), nullptr);
  std::vector<uint32_t> ids;
  for (size_t at = spec.find(','); at != std::string::npos; at = spec.find(',', at + 1))
    ids.push_back(uint32_t(std::strtoul(spec.c_str() + at + 1, nullptr, 10)));
  uint32_t p = 0;
  while (!(p = Player())) std::this_thread::sleep_for(std::chrono::milliseconds(500));
  std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(t * 1000)));
  REXLOG_INFO("ds2 equip probe: player {:08X}, {} ids", p, ids.size());
  // Hits: copy the heaps in 64 KB chunks (unmapped chunks fail and are skipped).
  std::vector<uint32_t> hits;
  std::vector<uint8_t> chunk(0x10000);
  for (uint64_t a = 0x40000000u; a < 0xA0000000u && hits.size() < 120; a += chunk.size()) {
    if (!Copy(uint32_t(a), chunk.data(), chunk.size())) continue;
    for (uint32_t o = 0; o + 4 <= chunk.size(); o += 4) {
      uint32_t v;
      std::memcpy(&v, chunk.data() + o, 4);
      v = __builtin_bswap32(v);
      for (uint32_t id : ids) {
        if (v != id || hits.size() >= 120) continue;
        const uint32_t at = uint32_t(a) + o;
        uint32_t cls = 0, back = 0;
        for (uint32_t b = 4; b <= 0x800 && at >= b; b += 4) {
          uint32_t w = 0;
          if (U32(at - b, &w) && w >= 0x82000000u && w < 0x83700000u && (w & 3) == 0) {
            cls = w;
            back = b;
            break;
          }
        }
        hits.push_back(at);
        REXLOG_INFO("ds2 equip probe: id {} at {:08X} (class {:08X} at -{:X}){}", id, at, cls, back,
                    at >= p && at < p + 0x800 ? fmt::format(" = player+{:X}", at - p) : std::string());
      }
    }
  }
  // Windows to watch: around each hit, and the player object.
  std::vector<std::pair<uint32_t, uint32_t>> windows{{p, p + 0x800}};
  for (uint32_t h : hits) windows.emplace_back(h >= 0x100 ? h - 0x100 : h, h + 0x100);
  std::vector<std::vector<uint32_t>> prev(windows.size());
  for (int tick = 0; tick < 4 * 90; ++tick) {
    for (size_t w = 0; w < windows.size(); ++w) {
      const auto [lo, hi] = windows[w];
      std::vector<uint32_t>& pv = prev[w];
      const bool first = pv.empty();
      pv.resize((hi - lo) / 4);
      std::string changed;
      int n = 0;
      for (uint32_t i = 0; i < pv.size(); ++i) {
        uint32_t v = 0;
        if (!U32(lo + i * 4, &v)) break;
        if (!first && v != pv[i] && n++ < 24) changed += fmt::format(" {:08X}={:08X}(was {:X})", lo + i * 4, v, pv[i]);
        pv[i] = v;
      }
      // The player object's moving parts (position, animation) change all the time: report it only when few did.
      if (n && (w > 0 || n < 12)) REXLOG_INFO("ds2 equip probe: window {} {} changed:{}", w, n, changed);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  REXLOG_INFO("ds2 equip probe: done");
}

bool HeapPtr(uint32_t v) { return v >= 0x40000000u && v < 0xA0000000u && (v & 3) == 0; }

void EquipProbe2(double t) {
  uint32_t p = 0;
  while (!(p = Player())) std::this_thread::sleep_for(std::chrono::milliseconds(500));
  std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(t * 1000)));
  struct Window {
    uint32_t addr, size, parent, offset;
    std::vector<uint8_t> prev;
    bool noisy = false;
    int noisy_ticks = 0;
  };
  std::vector<Window> windows;
  auto add = [&](uint32_t a, uint32_t size, uint32_t parent, uint32_t off) {
    for (const Window& w : windows)
      if (w.addr == a) return false;
    if (windows.size() >= 220) return false;
    std::vector<uint8_t> buf(size);
    if (!Copy(a, buf.data(), size)) return false;
    windows.push_back({a, size, parent, off, std::move(buf)});
    return true;
  };
  add(p, 0x800, 0, 0);
  const size_t level1_start = windows.size();
  for (uint32_t off = 0; off < 0x800; off += 4) {
    uint32_t v = 0;
    if (U32(p + off, &v) && HeapPtr(v) && (v < p || v >= p + 0x800)) add(v, 0x600, p, off);
  }
  const size_t level1_end = windows.size();
  for (size_t i = level1_start; i < level1_end; ++i) {
    const uint32_t obj = windows[i].addr;
    for (uint32_t off = 0; off < 0x100; off += 4) {
      uint32_t v = 0;
      if (U32(obj + off, &v) && HeapPtr(v) && v != p) add(v, 0x400, obj, off);
    }
  }
  for (size_t i = 0; i < windows.size(); ++i) {
    uint32_t cls = 0;
    U32(windows[i].addr, &cls);
    REXLOG_INFO("ds2 equip probe2: window {} {:08X} class {:08X} = [{:08X}+{:X}]", i, windows[i].addr, cls,
                windows[i].parent, windows[i].offset);
  }
  // The equipment controller ([player+0x2E4], class 0x8201F420; +0x1A0 / +0x1A4 the active left / right slot) and
  // what its first words point to, in full.
  auto dump = [&](const char* what, uint32_t a, uint32_t size) {
    std::vector<uint8_t> buf(size);
    if (!Copy(a, buf.data(), size)) return;
    for (uint32_t o = 0; o < size; o += 0x40) {
      std::string line;
      for (uint32_t k = 0; k < 0x40 && o + k < size; k += 4) {
        uint32_t v;
        std::memcpy(&v, buf.data() + o + k, 4);
        line += fmt::format(" {:08X}", __builtin_bswap32(v));
      }
      REXLOG_INFO("ds2 equip probe2: {} {:08X}+{:03X}:{}", what, a, o, line);
    }
  };
  uint32_t pdata = 0;
  if (U32(p + ds2::player::kSoulsLink, &pdata) && HeapPtr(pdata)) dump("pdata", pdata - 0x28, 0x800);
  uint32_t asm_obj = 0;
  if (U32(p + 0x2E4, &asm_obj) && HeapPtr(asm_obj)) {
    dump("asm", asm_obj, 0x600);
    for (uint32_t off = 0; off < 0x40; off += 4) {
      uint32_t v = 0;
      if (U32(asm_obj + off, &v) && HeapPtr(v)) dump(fmt::format("asm+{:X}", off).c_str(), v, 0x400);
    }
  }
  // Item ids (DS2_EQUIP_IDS="id,id,..."): every place in the heaps, at most 40 per id, with the window it is in.
  if (const char* list = std::getenv("DS2_EQUIP_IDS"); list && *list) {
    std::vector<uint32_t> ids;
    for (const char* c = list; *c;) {
      char* end = nullptr;
      ids.push_back(uint32_t(std::strtoul(c, &end, 10)));
      c = (*end == ',') ? end + 1 : end;
    }
    std::vector<int> found(ids.size());
    std::vector<uint32_t> last(ids.size()), stride(ids.size());
    std::vector<uint8_t> chunk(0x10000);
    for (uint64_t a = 0x40000000u; a < 0xA0000000u; a += chunk.size()) {
      if (!Copy(uint32_t(a), chunk.data(), chunk.size())) continue;
      for (uint32_t o = 0; o + 4 <= chunk.size(); o += 4) {
        uint32_t v;
        std::memcpy(&v, chunk.data() + o, 4);
        v = __builtin_bswap32(v);
        for (size_t k = 0; k < ids.size(); ++k) {
          if (v != ids[k]) continue;
          const uint32_t at = uint32_t(a) + o;
          // Parameter tables repeat an id at a fixed stride: skip the regular part of such a run.
          const uint32_t d = at - last[k];
          const bool regular = last[k] && d == stride[k];
          stride[k] = last[k] ? d : 0;
          last[k] = at;
          if (regular || found[k] >= 60) continue;
          ++found[k];
          std::string in;
          for (size_t w = 0; w < windows.size(); ++w)
            if (at >= windows[w].addr && at < windows[w].addr + windows[w].size)
              in += fmt::format(" w{}+{:X}", w, at - windows[w].addr);
          if (asm_obj && at >= asm_obj && at < asm_obj + 0x600) in += fmt::format(" asm+{:X}", at - asm_obj);
          std::string around;
          if (std::getenv("DS2_EQUIP_IDS_CONTEXT")) {
            for (int d = -0x20; d <= 0x40; d += 4) {
              uint32_t v2 = 0;
              U32(at + d, &v2);
              around += fmt::format(" {:08X}", v2);
            }
          }
          REXLOG_INFO("ds2 equip probe2: id {} at {:08X}{}{}", ids[k], at, in, around.empty() ? "" : " |" + around);
        }
      }
    }
  }
  std::vector<uint8_t> cur;
  for (int tick = 0; tick < 4 * 100; ++tick) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    for (size_t i = 0; i < windows.size(); ++i) {
      Window& w = windows[i];
      cur.resize(w.size);
      if (w.noisy || !Copy(w.addr, cur.data(), w.size)) continue;
      std::string changed;
      int n = 0;
      for (uint32_t o = 0; o + 4 <= w.size; o += 4) {
        if (std::memcmp(cur.data() + o, w.prev.data() + o, 4) == 0) continue;
        uint32_t a, b;
        std::memcpy(&a, cur.data() + o, 4);
        std::memcpy(&b, w.prev.data() + o, 4);
        if (++n <= 16) changed += fmt::format(" +{:X}={:08X}(was {:X})", o, __builtin_bswap32(a), __builtin_bswap32(b));
      }
      w.prev = cur;
      if (n > 16) {
        if (++w.noisy_ticks > 3) {
          w.noisy = true;
          REXLOG_INFO("ds2 equip probe2: w{} {:08X} noisy, dropped", i, w.addr);
        }
      }
      if (n && n <= 16) REXLOG_INFO("ds2 equip probe2: w{} {:08X}:{}", i, w.addr, changed);
    }
  }
  REXLOG_INFO("ds2 equip probe2: done");
}

}  // namespace

bool CopyGuest(uint32_t addr, void* dst, size_t len) { return Copy(addr, dst, len); }

void StartCompanion(rex::Runtime* runtime) {
  if (!runtime || !runtime->memory()) return;
  g_base.store(runtime->memory()->virtual_membase());
  if (const char* probe = std::getenv("DS2_COMPANION_PROBE"); probe && *probe == '1') std::thread(ProbeLoop).detach();
  if (const char* spec = std::getenv("DS2_STATUS_TEST"); spec && *spec) std::thread(StatusTest, std::string(spec)).detach();
  if (const char* spec = std::getenv("DS2_EQUIP_PROBE"); spec && *spec) std::thread(EquipProbe, std::string(spec)).detach();
  if (const char* spec = std::getenv("DS2_EQUIP_PROBE2"); spec && *spec) std::thread(EquipProbe2, std::strtod(spec, nullptr)).detach();
  if (const char* v = std::getenv("DS2_PLAYER_PROBE"); v && *v == '1') std::thread(PlayerProbe).detach();
}

}  // namespace ds2::android

// The bottom screen's state line (see the top of this file). Called on CompanionSurface's drawing thread.
extern "C" __attribute__((visibility("default"))) JNIEXPORT jstring JNICALL
Java_com_ds2recomp_android_RuntimeActivity_nativeCompanionState(JNIEnv* env, jclass) {
  return env->NewStringUTF(ds2::android::State().c_str());
}

// An equipment icon's texture data (16 KB, Xenos-tiled DXT5 64x128) by the hash the state line gave for it, or null
// (no longer drawn, or its memory changed since: the copy must hash the same). The second screen decodes it.
extern "C" __attribute__((visibility("default"))) JNIEXPORT jbyteArray JNICALL
Java_com_ds2recomp_android_RuntimeActivity_nativeCompanionIcon(JNIEnv* env, jclass, jlong hash) {
  const uint32_t gpu = ds2::hud_sprites::IconAddress(uint64_t(hash));
  if (!gpu) return nullptr;
  std::vector<uint8_t> data(0x4000);
  if (!ds2::android::CopyGuest(gpu, data.data(), data.size())) return nullptr;
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint8_t b : data) h = (h ^ b) * 0x100000001B3ull;
  if (h != uint64_t(hash)) return nullptr;
  jbyteArray out = env->NewByteArray(jsize(data.size()));
  if (out) env->SetByteArrayRegion(out, 0, jsize(data.size()), reinterpret_cast<const jbyte*>(data.data()));
  return out;
}

// Pins the calling thread (the bottom screen's drawing thread) to the little cores: the CPUs with the lowest maximum
// clock (the Thor: cpu0-2), away from the game's threads on the middle and prime cores. Returns a line for the log.
extern "C" __attribute__((visibility("default"))) JNIEXPORT jstring JNICALL
Java_com_ds2recomp_android_RuntimeActivity_nativeCompanionPinLittleCores(JNIEnv* env, jclass) {
  long lowest = -1;
  long freq[64];
  int n = 0;
  for (; n < 64; ++n) {
    char path[96];
    std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", n);
    FILE* f = std::fopen(path, "r");
    if (!f) break;
    long v = 0;
    if (std::fscanf(f, "%ld", &v) != 1) v = 0;
    std::fclose(f);
    freq[n] = v;
    if (v > 0 && (lowest < 0 || v < lowest)) lowest = v;
  }
  cpu_set_t set;
  CPU_ZERO(&set);
  std::string cpus;
  int picked = 0;
  for (int i = 0; i < n; ++i) {
    if (freq[i] != lowest) continue;
    CPU_SET(i, &set);
    cpus += (cpus.empty() ? "" : ",") + std::to_string(i);
    ++picked;
  }
  std::string result;
  if (picked == 0 || picked == n) {
    result = "not pinned (no slower cores)";
  } else if (sched_setaffinity(0, sizeof(set), &set) == 0) {
    result = "pinned to cpu " + cpus;
  } else {
    result = "pinning to cpu " + cpus + " failed";
  }
  return env->NewStringUTF(result.c_str());
}

#endif  // __ANDROID__
