// ds2 - which textures the game's 2D layer draws (for the second screen's effect icons and, later, hiding the HUD
// pieces it shows).
//
// The sprite draw sub_82F51340 (r3 = the layout's sprite element) takes each sprite's texture object from its texture
// table (element +32, entry + 4) or from a per-sprite override (element +56, a virtual call that returns one), then
// hands it to sub_82F3BF70 (r3 = the 2D renderer, r4 = the texture object) before the sprite's transform
// (sub_82F3E4F0, ds2_render_size.cpp).
//
// DS2_SPRITE_PROBE=1 (debug launches): logs each distinct texture object passed to sub_82F3BF70 (up to 400) with its
// first 0x60 bytes and the strings its words point to. Texture objects (class 0x82108798) keep no name: +0x18 GPU
// address, +0x28 / +0x2C width / height (Thor, 2026-11-21). So it also logs each distinct texture override object of
// a sprite element (element +56; its vtable slot 2 returns the texture): icons that change at run time (effects,
// equipment) come that way, and the override may know its icon id.
#include "ds2_hud_sprites.h"

#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <set>
#include <tuple>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

extern "C" void __imp__sub_82F3BF70(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82F51340(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82F52870(PPCContext& __restrict ctx, uint8_t* base);

namespace {

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}

// A printable ASCII or UTF-16BE string at a (guest) address, or "".
std::string StringAt(const uint8_t* base, uint32_t a) {
  if (a < 0x40000000u || a >= 0xC0000000u) return {};
  std::string s;
  for (uint32_t i = 0; i < 48; ++i) {
    const uint8_t c = base[a + i];
    if (c == 0) break;
    if (c < 0x20 || c > 0x7E) return {};
    s += char(c);
  }
  if (s.size() >= 3) return "'" + s + "'";
  s.clear();
  for (uint32_t i = 0; i < 48; ++i) {
    const uint8_t hi = base[a + 2 * i], lo = base[a + 2 * i + 1];
    if (hi == 0 && lo == 0) break;
    if (hi != 0 || lo < 0x20 || lo > 0x7E) return {};
    s += char(lo);
  }
  return s.size() >= 3 ? "u'" + s + "'" : std::string();
}

void Probe(const uint8_t* base, uint32_t tex, uint32_t lr) {
  static std::mutex mutex;
  static std::unordered_set<uint32_t> seen;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.size() >= 400 || !seen.insert(tex).second) return;
  }
  if (tex < 0x40000000u || tex >= 0xC0000000u) {
    REXLOG_INFO("ds2 sprite probe: texture {:08X} (lr {:08X})", tex, lr);
    return;
  }
  std::string words, strings;
  for (uint32_t off = 0; off < 0x60; off += 4) {
    const uint32_t w = Load32(base, tex + off);
    words += fmt::format(" {:08X}", w);
    std::string s = StringAt(base, w);
    if (s.empty() && w >= 0x40000000u && w < 0xC0000000u) {
      // One level further: a pointer to a string pointer (a name object).
      for (uint32_t k = 0; k < 0x10 && s.empty(); k += 4) {
        const uint32_t w2 = Load32(base, w + k);
        std::string s2 = StringAt(base, w2);
        if (!s2.empty()) s = fmt::format("[+{:X}]->{}", k, s2);
      }
    }
    if (!s.empty()) strings += fmt::format(" +{:02X}:{}", off, s);
  }
  REXLOG_INFO("ds2 sprite probe: texture {:08X} (lr {:08X}):{} |{}", tex, lr, words, strings);
}

void ProbeOverride(const uint8_t* base, uint32_t element) {
  if (element < 0x40000000u || element >= 0xC0000000u) return;
  const uint32_t provider = Load32(base, element + 56);
  if (provider < 0x40000000u || provider >= 0xC0000000u) return;
  static std::mutex mutex;
  static std::unordered_set<uint32_t> seen;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.size() >= 300 || !seen.insert(provider).second) return;
  }
  const uint32_t vtable = Load32(base, provider);
  const uint32_t get = vtable >= 0x82000000u && vtable < 0x84000000u ? Load32(base, vtable + 8) : 0;
  std::string words;
  for (uint32_t off = 0; off < 0x80; off += 4) words += fmt::format(" {:08X}", Load32(base, provider + off));
  REXLOG_INFO("ds2 sprite probe: override {:08X} vtable {:08X} get {:08X} (element {:08X}, flags {:02X}):{}", provider,
              vtable, get, element, base[element + 67], words);
}

bool ProbeOn() {
  static const bool probe = [] {
    const char* v = std::getenv("DS2_SPRITE_PROBE");
    return v && *v == '1';
  }();
  return probe;
}

// DS2_SPRITE_PROBE=2: every distinct (texture, position) a sprite is drawn with, with the texture's size, GPU address
// (+0x18, a guest address in the physical mirror) and its first 16 data bytes, to match HUD icons against the icon
// files (the data is the TPF's, still tiled).
int Probe2() {
  static const int probe = [] {
    const char* v = std::getenv("DS2_SPRITE_PROBE");
    return v && *v == '2' ? 2 : 0;
  }();
  return probe;
}
thread_local uint32_t t_last_texture = 0;

// DS2_SPRITE_PROBE=3: the guest call chain of each 2D draw (return addresses from the PowerPC back chain: [r1] is the
// caller's caller's stack pointer, the caller saved its LR at that minus 8), logged once per (chain, texture size,
// position) for sprites and per (chain, text, position) for text, to find the level that draws one HUD layout.
int Probe3() {
  static const int probe = [] {
    const char* v = std::getenv("DS2_SPRITE_PROBE");
    return v && *v == '3' ? 3 : 0;
  }();
  return probe;
}
// The FE library (the 2D layer: element draws, the layout tree walk) and its generic dispatcher.
bool FeFrame(uint32_t ret) { return (ret >= 0x82F30000u && ret < 0x82F80000u) || ret == 0x827097E4u; }

// Return addresses up the guest stack from a draw: the first game-side ones (outside the FE library) say which game
// object (HUD widget, menu) is drawing.
std::string Chain(const uint8_t* base, uint32_t sp, uint32_t lr) {
  std::string out;
  uint32_t frame = sp;
  int game = 0;
  if (!FeFrame(lr)) {
    out = fmt::format("{:08X}", lr);
    ++game;
  }
  for (int n = 0; n < 48 && game < 3; ++n) {
    if (frame < 0x40000000u || frame >= 0xC0000000u) break;
    const uint32_t back = Load32(base, frame);
    if (back <= frame || back - frame > 0x20000) break;
    const uint32_t ret = Load32(base, back - 8);
    if (ret < 0x82000000u || ret >= 0x84000000u) break;
    if (!FeFrame(ret)) {
      out += fmt::format("{}{:08X}", out.empty() ? "" : " ", ret);
      ++game;
    }
    frame = back;
  }
  return out.empty() ? std::string("-") : out;
}
thread_local std::string t_chain;

// --- capture for the second screen --------------------------------------------------------------------------------
std::atomic<bool> g_capture{false};

uint32_t NowMs() {
  using namespace std::chrono;
  return uint32_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Icon texture -> hash cache (render thread only). Validated by four sample words: texture memory is reused.
struct CacheEntry {
  uint32_t tex = 0, gpu = 0, sample = 0;
  uint64_t hash = 0;
};
CacheEntry g_cache[256];

uint32_t Sample(const uint8_t* base, uint32_t gpu, uint32_t size) {
  return Load32(base, gpu + size / 4) ^ Load32(base, gpu + size / 2) ^ Load32(base, gpu + 3 * (size / 4)) ^
         Load32(base, gpu + size - 16);
}

uint64_t TextureHash(const uint8_t* base, uint32_t tex, uint32_t gpu, uint32_t size) {
  const uint32_t sample = Sample(base, gpu, size);
  CacheEntry& e = g_cache[((gpu >> 12) ^ (tex >> 4)) & 255];
  if (e.tex == tex && e.gpu == gpu && e.sample == sample) return e.hash;
  uint64_t h = 0xCBF29CE484222325ull;
  for (uint32_t k = 0; k < size; ++k) h = (h ^ base[gpu + k]) * 0x100000001B3ull;
  e.tex = tex;
  e.gpu = gpu;
  e.sample = sample;
  e.hash = h;
  return h;
}

struct Seen {
  std::atomic<uint64_t> hash{0};
  std::atomic<uint32_t> ms{0};
  std::atomic<uint32_t> gpu{0};
};
Seen g_slots[5];  // left, right, spell, item, next
std::atomic<uint32_t> g_hud_ms{0};
std::atomic<uint32_t> g_gauge_ms{0};  // the HP gauge's layout (the emblem) drawn: the game shows its HUD
std::atomic<uint32_t> g_foe_bar_ms{0};  // an enemy's or the boss's HP bar drawn
Seen g_small[12];

void SeeSmall(uint64_t h, uint32_t now) {
  int free = -1, oldest = 0;
  for (int i = 0; i < 12; ++i) {
    if (g_small[i].hash.load(std::memory_order_relaxed) == h) {
      g_small[i].ms.store(now, std::memory_order_relaxed);
      return;
    }
    const uint32_t age = now - g_small[i].ms.load(std::memory_order_relaxed);
    if (free < 0 && age > 500) free = i;
    if (age > now - g_small[oldest].ms.load(std::memory_order_relaxed)) oldest = i;
  }
  const int i = free >= 0 ? free : oldest;
  g_small[i].hash.store(h, std::memory_order_relaxed);
  g_small[i].ms.store(now, std::memory_order_relaxed);
}

// Flags of the layout root being drawn (its signature textures, seen in the last 1.5 s); hiding section below.
uint32_t CurrentFlags(uint32_t now);
enum : uint32_t { kEmblem = 1, kGaugeAny = 2, kIcons = 4, kSouls = 8, kSlots = 16, kEffect = 32, kBoss = 64 };

// Positions are the sprite transforms' translations on the 1280x720 canvas; the item-select layout slides and zooms
// its cards while switching, hence boxes (Thor, DS2_SPRITE_PROBE=2: left 114-130 / 474-495, right 267-282 / 439-474,
// item 191-220 / 508-540, next item 275-296 / 588 at scale 0.43; the spell card sits above the item card).
// Only icons the item slots' own layout draws count (the owner, 2026-11-22: the cards emptied in the menu and the inventory
// put a lifegem into the spell card: menus draw icons at the same places).
void Capture(uint8_t* base, uint32_t tex, float tx, float ty, float scale) {
  if (tex < 0x40000000u || tex >= 0xC0000000u) return;
  const uint32_t w = Load32(base, tex + 0x28), h = Load32(base, tex + 0x2C);
  const bool icon = w == 64 && h == 128, small = w == 64 && h == 64;
  if (!icon && !small) return;
  const uint32_t now = NowMs();
  if (icon && (CurrentFlags(now) & (kSlots | kBoss)) != kSlots) return;
  const uint32_t gpu = Load32(base, tex + 0x18);
  if (gpu < 0xA0000000u) return;
  const uint64_t hash = TextureHash(base, tex, gpu, 0x4000);  // 64x128 and 64x64 DXT5: 16 KB tiled
  if (small) {
    SeeSmall(hash, now);
    return;
  }
  int slot = -1;
  if (scale < 0.6f && tx >= 250 && tx <= 330 && ty >= 560 && ty <= 620) slot = 4;        // next item
  else if (tx >= 90 && tx <= 160 && ty >= 450 && ty <= 520) slot = 0;                   // left hand
  else if (tx >= 245 && tx <= 305 && ty >= 415 && ty <= 495) slot = 1;                   // right hand
  else if (scale >= 0.6f && tx >= 170 && tx <= 240 && ty >= 495 && ty <= 565) slot = 3;  // item
  else if (tx >= 160 && tx <= 245 && ty >= 340 && ty <= 470) slot = 2;                   // spell
  if (slot < 0) return;
  g_slots[slot].gpu.store(gpu, std::memory_order_relaxed);
  g_slots[slot].hash.store(hash, std::memory_order_relaxed);
  g_slots[slot].ms.store(now, std::memory_order_relaxed);
}

// The belt item's count and name (text elements under its card; the element keeps its UTF-16 text at +0x2C).
std::mutex g_text_mutex;
std::string g_item_count, g_item_name;
std::atomic<uint32_t> g_text_ms{0};

std::string Utf16At(const uint8_t* base, uint32_t a) {
  std::string out;
  if (a < 0x40000000u || a >= 0xC0000000u) return out;
  for (uint32_t i = 0; i < 64; ++i) {
    const uint32_t c = (uint32_t(base[a + 2 * i]) << 8) | base[a + 2 * i + 1];
    if (!c) break;
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | (c >> 6));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xE0 | (c >> 12));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

void CaptureText(const uint8_t* base, uint32_t element, float x, float y) {
  if (y < 620 || y > 672 || x < 170 || x > 420) return;  // under the item card (count ~196, name ~239; y ~645)
  if (NowMs() - g_hud_ms.load(std::memory_order_relaxed) > 500) return;  // only while the item slots show
  std::string text = Utf16At(base, Load32(base, element + 0x2C));
  std::lock_guard<std::mutex> lock(g_text_mutex);
  (x < 225 ? g_item_count : g_item_name) = std::move(text);
  g_text_ms.store(NowMs(), std::memory_order_relaxed);
}

// --- hiding the moved HUD on the main screen ------------------------------------------------------------------------
// The FE library draws a layout through sub_82F55418(node) (recursively for sub-layouts: an element that is a layout
// calls it again); sub_82F5C4A8(node) walks the node's elements (node+32, then element+24) and calls each one's virtual
// draw. The outermost sub_82F55418 node of a draw is its layout's root. A root is recognised by a texture only its HUD
// layout uses (data hashes of the first 16 KB, as ds2_companion_assets.py computes them); everything drawn under a
// recognised root is hidden: sprites by a zero-size transform (the draw still runs, so its texture is still seen), text
// by skipping the draw. Boss / enemy bars, menus and messages are other layouts and stay.
std::atomic<bool> g_hide{false};
thread_local int t_depth = 0;
thread_local uint32_t t_root = 0;

constexpr uint64_t kHpGauge = 0xAAA936237759D0C2ull;   // FE_HPgage_01 (256x128): emblem, bars; build-up bars too
constexpr uint64_t kSoulGauge = 0x668FEA515C9A43D2ull; // Soul_Gauge (64x64)
constexpr uint64_t kKeyMenu = 0x0B66E03641F4C718ull;   // L_keyMenu (256x128): the item slots' cards
constexpr uint64_t kIconTex = 0x8D0E182F13280DA4ull;   // Icon_tex (256x256): the build-up icons
constexpr uint64_t kBossHp = 0x68EC488F9012E9D0ull;    // Boss_hp (128x64): the boss bar (l01_17_hp_boss, which also
                                                       // draws from L_keyMenu: not the item slots)
constexpr uint64_t kEffectIcons[] = {  // menu/tex/Icon/effect/EI_*.tpf, disc 1 and 2
    0x24984BB6C672A21Eull, 0xA7432B63E6462DF4ull, 0xCBA43751E20BD3CEull, 0xBAFEA98102E7C240ull, 0xFA78653F036B5EFAull,
    0xCC3C0D63C930DC18ull, 0x665CBA1F1BD326E9ull, 0x54E949FB7B012CB2ull, 0x20BAAC952398B94Dull, 0x80EBC37C26FAD92Cull,
    0x0BE2FBCEC7BFB52Dull, 0xA8A3BFAE647EC003ull, 0x80C2959F32387A8Aull, 0x055EA49F4958D965ull, 0x9B5392B2721EE02Full,
    0x34B56EF1DAA40405ull, 0x166D8AED616F9466ull, 0xEE47EBB9FF983AA0ull, 0x73443DE2941A9CECull, 0x16076FB8D3EA2DACull,
    0x91A4EF9EA4516306ull, 0x45AC3C5EB8D8959Full, 0x84FCAD76040A2CEBull, 0x07212DFA65BAABF7ull, 0x10049DE63C53EEC0ull,
    0xE6922CCA0B99A5C1ull, 0xAE40A725D5136A93ull};
struct RootInfo {
  uint32_t root = 0, flags = 0, ms = 0;
};
RootInfo g_roots[64];  // render thread only

RootInfo& RootEntry(uint32_t root, uint32_t now) {
  RootInfo* oldest = &g_roots[0];
  for (RootInfo& r : g_roots) {
    if (r.root == root) return r;
    if (now - r.ms > now - oldest->ms) oldest = &r;
  }
  *oldest = RootInfo{root, 0, now};
  return *oldest;
}

uint32_t CurrentFlags(uint32_t now) {
  if (!t_root) return 0;
  for (const RootInfo& r : g_roots)
    if (r.root == t_root) return now - r.ms < 1500 ? r.flags : 0;
  return 0;
}

// The layouts the second screen takes over. Not the status build-up (FE_HPgage_01 + Icon_tex): it stays on the main
// screen where the game puts it (the owner, 2026-11-22: "we could keep those in main screen where they normally sit").
bool HiddenFlags(uint32_t f) {
  if (f & kBoss) return false;
  return (f & kEmblem) || (f & kSouls) || (f & kSlots) || (f & kEffect);
}

// Whether the layout being drawn is one of the HUD's (render thread).
bool HudRoot(uint32_t now) {
  if (!t_root) return false;
  for (const RootInfo& r : g_roots)
    if (r.root == t_root) return now - r.ms < 1500 && HiddenFlags(r.flags);
  return false;
}

// Whether what is drawn under the current root is hidden (render thread).
bool HideCurrent(uint32_t now) {
  return g_hide.load(std::memory_order_relaxed) && g_capture.load(std::memory_order_relaxed) && HudRoot(now);
}

// Whether a layout with this signature was hidden in the last 1.5 s (render thread).
bool RecentlyHidden(uint32_t flag, uint32_t now) {
  for (const RootInfo& r : g_roots)
    if ((r.flags & flag) && now - r.ms < 1500 && HiddenFlags(r.flags)) return true;
  return false;
}

// Texts of the hidden layouts that are drawn outside their roots' walk (Thor: the item's count and name, the soul
// count and the souls just gained kept showing): hidden by where they are while their layout is hidden.
bool HideTextAt(float x, float y, uint32_t now) {
  if (!g_hide.load(std::memory_order_relaxed)) return false;
  if (x >= 170 && x <= 420 && y >= 620 && y <= 672) return RecentlyHidden(kSlots, now);   // item count / name
  if (x >= 900 && x <= 1180 && y >= 590 && y <= 700) return RecentlyHidden(kSouls, now);  // souls, souls gained
  return false;
}

// Marks the current root by the texture a sprite draws (while capturing); returns whether the sprite is hidden.
bool ClassifyAndHide(uint8_t* base, uint32_t tex, uint32_t m, float tx, float ty) {
  if (!t_root) return false;
  const uint32_t now = NowMs();
  uint32_t flag = 0;
  if (tex >= 0x40000000u && tex < 0xC0000000u) {
    const uint32_t w = Load32(base, tex + 0x28), h = Load32(base, tex + 0x2C);
    const bool sig = (w == 256 && (h == 128 || h == 256)) || (w == 64 && h == 64) || (w == 128 && h == 64);
    const uint32_t gpu = sig ? Load32(base, tex + 0x18) : 0;
    if (gpu >= 0xA0000000u) {
      const uint64_t hash = TextureHash(base, tex, gpu, 0x4000);
      if (hash == kHpGauge) flag = kGaugeAny | (tx < 260 && ty < 180 ? kEmblem : 0);
      else if (hash == kKeyMenu) flag = kSlots;
      else if (hash == kIconTex) flag = kIcons;
      else if (hash == kSoulGauge) flag = kSouls;
      else if (hash == kBossHp) flag = kBoss;
      else
        for (uint64_t e : kEffectIcons)
          if (hash == e) flag = kEffect;
    }
  }
  RootInfo& r = RootEntry(t_root, now);
  if (flag) {
    const uint32_t before = r.flags;
    r.flags |= flag;
    r.ms = now;
    static const bool probe = [] {
      const char* v = std::getenv("DS2_SPRITE_PROBE");
      return v && *v == '4';
    }();
    if (probe && r.flags != before)
      REXLOG_INFO("ds2 hide probe: root {:08X} flags {:02X} (texture {:08X} at {:g} {:g}){}", t_root, r.flags, tex, tx, ty,
                  HiddenFlags(r.flags) ? " hidden" : "");
  } else if (now - r.ms > 1500) {
    r.flags = 0;  // the root's signature stopped: its memory may hold another layout now
  }
  if ((r.flags & kSlots) && !(r.flags & kBoss) && now - r.ms < 1500) g_hud_ms.store(now, std::memory_order_relaxed);
  // An enemy's HP bar (l01_13_hp_enemy: FE_HPgage_01 without the player's emblem or the build-up icons) or the boss
  // bar: the game shows them for what the player hits (the damage figures count only those hits).
  if ((flag & kBoss) || ((flag & kGaugeAny) && !(r.flags & (kEmblem | kIcons))))
    g_foe_bar_ms.store(now, std::memory_order_relaxed);
  if ((r.flags & kEmblem) && now - r.ms < 1500) g_gauge_ms.store(now, std::memory_order_relaxed);
  if (!g_hide.load(std::memory_order_relaxed) || !(now - r.ms < 1500 && HiddenFlags(r.flags))) return false;
  // Zero-size transform: the sprite's quad collapses (the matrix is the draw's own copy on the stack).
  for (uint32_t off : {0u, 4u, 8u, 16u, 20u, 24u}) std::memset(base + m + off, 0, 4);
  return true;
}

// --- other layouts ----------------------------------------------------------------------------------------------------
// A sprite of a layout that is not one of the HUD's (menus, dialogs, messages, the loading screen, or drawn outside a
// layout's walk): the game shows more than its HUD. Neither the HUD's slots nor anything else for a while means a
// cutscene or a movie (the second screen dims; CompanionView).
std::atomic<uint32_t> g_ui_ms{0};
// The loading screen (its scroll, 16_Loading in menu/20, 1024x1024 DXT1) counts as quiet: a warp dims from the moment
// the HUD goes until it is back, rather than lifting the dim for the loading screen (Thor, 2026-11-22).
constexpr uint64_t kLoadingScreen = 0x1EA88B150CE4DA42ull;
std::atomic<uint32_t> g_loading_ms{0};

void NoteUi(uint8_t* base, uint32_t tex, uint32_t now) {
  if (tex >= 0x40000000u && tex < 0xC0000000u && Load32(base, tex + 0x28) == 1024 && Load32(base, tex + 0x2C) == 1024) {
    const uint32_t gpu = Load32(base, tex + 0x18);
    if (gpu >= 0xA0000000u && TextureHash(base, tex, gpu, 0x4000) == kLoadingScreen)
      g_loading_ms.store(now, std::memory_order_relaxed);
  }
  if (HudRoot(now)) return;
  g_ui_ms.store(now, std::memory_order_relaxed);
  // DS2_SPRITE_PROBE=5 (debug): once a second, the other layouts that drew (root, first texture size, sprites).
  static const bool probe = [] {
    const char* v = std::getenv("DS2_SPRITE_PROBE");
    return v && *v == '5';
  }();
  if (!probe) return;
  struct Count {
    uint32_t root, w, h, n;
  };
  static Count counts[16];
  static int used = 0;
  static uint32_t since = now;
  uint32_t w = 0, h = 0;
  if (tex >= 0x40000000u && tex < 0xC0000000u) {
    w = Load32(base, tex + 0x28);
    h = Load32(base, tex + 0x2C);
  }
  int i = 0;
  while (i < used && counts[i].root != t_root) ++i;
  if (i == used && used < 16) counts[used++] = Count{t_root, w, h, 0};
  if (i < used) ++counts[i].n;
  if (now - since >= 1000) {
    std::string line;
    for (int k = 0; k < used; ++k) line += fmt::format(" {:08X}:{}x{}:{}", counts[k].root, counts[k].w, counts[k].h, counts[k].n);
    REXLOG_INFO("ds2 ui probe:{}", line);
    used = 0;
    since = now;
  }
}

}  // namespace

namespace ds2::hud_sprites {

void SetCapture(bool on) { g_capture.store(on, std::memory_order_relaxed); }
void SetHide(bool on) { g_hide.store(on, std::memory_order_relaxed); }

Slots ReadSlots() {
  const uint32_t now = NowMs();
  auto get = [&](int i) -> uint64_t {
    return now - g_slots[i].ms.load(std::memory_order_relaxed) < 500 ? g_slots[i].hash.load(std::memory_order_relaxed)
                                                                     : 0;
  };
  Slots s;
  s.left = get(0);
  s.right = get(1);
  s.spell = get(2);
  s.item = get(3);
  s.next = get(4);
  // The slot layout drawn lately with at least one icon: menus keep drawing the layout without its icons (Thor,
  // 2026-11-22: the cards emptied in the inventory and the Status screen).
  s.hud = now - g_hud_ms.load(std::memory_order_relaxed) < 500 && (s.left || s.right || s.spell || s.item || s.next);
  s.ui = now - g_ui_ms.load(std::memory_order_relaxed) < 700 && now - g_loading_ms.load(std::memory_order_relaxed) >= 700;
  s.gauge = now - g_gauge_ms.load(std::memory_order_relaxed) < 500;
  return s;
}

uint32_t FoeBarAgeMs() {
  const uint32_t t = g_foe_bar_ms.load(std::memory_order_relaxed);
  return t ? NowMs() - t : 0xFFFFFFFFu;
}

uint32_t IconAddress(uint64_t hash) {
  for (const Seen& s : g_slots)
    if (hash && s.hash.load(std::memory_order_relaxed) == hash) return s.gpu.load(std::memory_order_relaxed);
  return 0;
}

void ReadItemText(std::string* count, std::string* name) {
  if (NowMs() - g_text_ms.load(std::memory_order_relaxed) > 500) {
    count->clear();
    name->clear();
    return;
  }
  std::lock_guard<std::mutex> lock(g_text_mutex);
  *count = g_item_count;
  *name = g_item_name;
}

int ReadSmallIcons(uint64_t* out, int max) {
  const uint32_t now = NowMs();
  int n = 0;
  for (int i = 0; i < 12 && n < max; ++i) {
    const uint64_t h = g_small[i].hash.load(std::memory_order_relaxed);
    if (h && now - g_small[i].ms.load(std::memory_order_relaxed) < 500) out[n++] = h;
  }
  return n;
}

void OnSpriteTransform(uint8_t* base, uint32_t m) {
  if (Probe3() && !t_chain.empty()) {
    auto rf = [&](uint32_t a) {
      const uint32_t u = Load32(base, a);
      float v;
      std::memcpy(&v, &u, 4);
      return v;
    };
    const uint32_t tex = t_last_texture;
    uint32_t w = 0, h = 0;
    if (tex >= 0x40000000u && tex < 0xC0000000u) {
      w = Load32(base, tex + 0x28);
      h = Load32(base, tex + 0x2C);
    }
    const int x = int(rf(m + 12)), y = int(rf(m + 28));
    static std::mutex mutex;
    static std::set<std::string> seen;
    const std::string key = fmt::format("{} {}x{} {} {}", t_chain, w, h, x / 40, y / 40);
    bool fresh;
    {
      std::lock_guard<std::mutex> lock(mutex);
      fresh = seen.size() < 1500 && seen.insert(key).second;
    }
    if (fresh) REXLOG_INFO("ds2 chain probe: sprite {}x{} at {} {} | {}", w, h, x, y, t_chain);
  }
  if (g_capture.load(std::memory_order_relaxed)) {
    auto rf = [&](uint32_t a) {
      const uint32_t u = Load32(base, a);
      float v;
      std::memcpy(&v, &u, 4);
      return v;
    };
    const float tx = rf(m + 12), ty = rf(m + 28);
    Capture(base, t_last_texture, tx, ty, rf(m));
    ClassifyAndHide(base, t_last_texture, m, tx, ty);
    NoteUi(base, t_last_texture, NowMs());
  }
  if (!Probe2()) return;
  const uint32_t tex = t_last_texture;
  auto f = [&](uint32_t a) {
    const uint32_t u = Load32(base, a);
    float v;
    std::memcpy(&v, &u, 4);
    return v;
  };
  const int tx = int(std::lround(f(m + 12))), ty = int(std::lround(f(m + 28)));
  static std::mutex mutex;
  static std::set<std::tuple<uint32_t, int, int>> seen;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (seen.size() >= 3000 || !seen.emplace(tex, tx / 2, ty / 2).second) return;
  }
  uint32_t w = 0, h = 0, gpu = 0;
  std::string head;
  if (tex >= 0x40000000u && tex < 0xC0000000u) {
    gpu = Load32(base, tex + 0x18);
    w = Load32(base, tex + 0x28);
    h = Load32(base, tex + 0x2C);
    if (gpu >= 0xA0000000u) {
      for (uint32_t k = 0; k < 16; ++k) head += fmt::format("{:02X}", base[gpu + k]);
      // Item icons (64x128 DXT5, 16 KB): FNV-1a 64 of the whole data, as ds2_companion_assets.py computes it.
      if (w == 64 && h == 128) {
        uint64_t fnv = 0xCBF29CE484222325ull;
        for (uint32_t k = 0; k < 0x4000; ++k) fnv = (fnv ^ base[gpu + k]) * 0x100000001B3ull;
        head += fmt::format(" fnv {:016X}", fnv);
      }
    }
  }
  REXLOG_INFO("ds2 sprite probe2: tex {:08X} {}x{} gpu {:08X} data {} at {} {} scale {:g} {:g}", tex, w, h, gpu, head, tx,
              ty, f(m), f(m + 20));
}

}  // namespace ds2::hud_sprites

REX_HOOK_RAW(sub_82F3BF70) {
  t_last_texture = ctx.r4.u32;
  if (ProbeOn()) Probe(base, ctx.r4.u32, uint32_t(ctx.lr));
  __imp__sub_82F3BF70(ctx, base);
}

// The text draw (r3 = the layout's text element, r4 = the text?, r5 = its rectangle: floats +0 x0, +4 y0, +12 y1).
// DS2_SPRITE_PROBE=2 logs each distinct (text, position).
REX_HOOK_RAW(sub_82F52870) {
  if (Probe3()) {
    const uint32_t r = ctx.r5.u32;
    if (r >= 0x40000000u && r < 0xC0000000u) {
      auto rf = [&](uint32_t a) {
        const uint32_t u = Load32(base, a);
        float v;
        std::memcpy(&v, &u, 4);
        return v;
      };
      const std::string chain = Chain(base, ctx.r1.u32, uint32_t(ctx.lr));
      const std::string text = Utf16At(base, Load32(base, ctx.r3.u32 + 0x2C));
      const int x = int(rf(r)), y = int(rf(r + 4));
      static std::mutex mutex;
      static std::set<std::string> seen;
      const std::string key = fmt::format("{} {} {}", chain, x / 40, y / 40);
      bool fresh;
      {
        std::lock_guard<std::mutex> lock(mutex);
        fresh = seen.size() < 1500 && seen.insert(key).second;
      }
      if (fresh) REXLOG_INFO("ds2 chain probe: text '{}' at {} {} | {}", text, x, y, chain);
    }
  }
  bool hide = false;
  if (g_capture.load(std::memory_order_relaxed)) {
    const uint32_t r = ctx.r5.u32;
    if (r >= 0x40000000u && r < 0xC0000000u) {
      auto rf = [&](uint32_t a) {
        const uint32_t u = Load32(base, a);
        float v;
        std::memcpy(&v, &u, 4);
        return v;
      };
      CaptureText(base, ctx.r3.u32, rf(r), rf(r + 4));
      hide = HideTextAt(rf(r), rf(r + 4), NowMs());
    }
    hide = hide || HideCurrent(NowMs());
  }
  if (Probe2()) {
    const uint32_t s = ctx.r4.u32, r = ctx.r5.u32;
    std::string text = StringAt(base, s);
    if (text.empty() && s >= 0x40000000u && s < 0xC0000000u) {
      for (uint32_t k = 0; k < 0x10 && text.empty(); k += 4) {
        std::string t2 = StringAt(base, Load32(base, s + k));
        if (!t2.empty()) text = fmt::format("[+{:X}]{}", k, t2);
      }
    }
    auto f = [&](uint32_t a) {
      const uint32_t u = Load32(base, a);
      float v;
      std::memcpy(&v, &u, 4);
      return v;
    };
    const float x = r >= 0x40000000u && r < 0xC0000000u ? f(r) : -1, y = r >= 0x40000000u && r < 0xC0000000u ? f(r + 4) : -1;
    static std::mutex mutex;
    static std::set<std::tuple<std::string, int, int>> seen;
    bool fresh;
    {
      std::lock_guard<std::mutex> lock(mutex);
      fresh = seen.size() < 2000 && seen.emplace(text, int(x) / 4, int(y) / 4).second;
    }
    if (fresh) {
      REXLOG_INFO("ds2 text probe: {} at {:g} {:g} (element {:08X}, r4 {:08X}, lr {:08X})", text.empty() ? "?" : text, x, y,
                  ctx.r3.u32, s, uint32_t(ctx.lr));
      // The element's words and the strings they lead to (two levels), to find where it keeps its text.
      const uint32_t el = ctx.r3.u32;
      static std::mutex emutex;
      static std::unordered_set<uint32_t> elements;
      bool new_el;
      {
        std::lock_guard<std::mutex> lock(emutex);
        new_el = elements.size() < 300 && elements.insert(el).second;
      }
      if (new_el && el >= 0x40000000u && el < 0xC0000000u) {
        std::string words, strings;
        for (uint32_t off = 0; off < 0x80; off += 4) {
          const uint32_t w = Load32(base, el + off);
          words += fmt::format(" {:08X}", w);
          std::string st = StringAt(base, w);
          for (uint32_t k = 0; st.empty() && w >= 0x40000000u && w < 0xC0000000u && k < 0x20; k += 4) {
            std::string s2 = StringAt(base, Load32(base, w + k));
            if (!s2.empty()) st = fmt::format("[+{:X}]{}", k, s2);
          }
          if (!st.empty()) strings += fmt::format(" +{:X}:{}", off, st);
        }
        REXLOG_INFO("ds2 text probe: element {:08X}:{} |{}", el, words, strings);
      }
    }
  }
  // A text of a layout the second screen shows (the soul count, the item's count and name): skipped. Collapsing the
  // text transform instead (sub_82F3D538's matrix, as for sprites) left the texts visible (Thor, 2026-11-22); the
  // element keeps its text either way (the item's count and name read from it stayed current while hidden).
  if (hide) return;
  __imp__sub_82F52870(ctx, base);
}

// A layout node's draw (r3 = node); the outermost call of a draw is its layout's root.
extern "C" void __imp__sub_82F55418(PPCContext& __restrict ctx, uint8_t* base);
REX_HOOK_RAW(sub_82F55418) {
  if (t_depth++ == 0) t_root = ctx.r3.u32;
  __imp__sub_82F55418(ctx, base);
  if (--t_depth == 0) t_root = 0;
}

// The sprite draw (r3 = the layout's sprite element).
REX_HOOK_RAW(sub_82F51340) {
  if (Probe3()) t_chain = Chain(base, ctx.r1.u32, uint32_t(ctx.lr));
  if (ProbeOn()) ProbeOverride(base, ctx.r3.u32);
  __imp__sub_82F51340(ctx, base);
}
