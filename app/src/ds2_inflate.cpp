// ds2 - the game's zlib inflate_fast (sub_827820A8) in native code.
//
// Dark Souls II decompresses its resource archives with zlib 1.2.3 (the PowerPC "pre-increment" build: end of
// block sets mode 11, errors mode 27). inflate_fast(strm, start) is the hot loop of every load, of the streaming
// while playing (the resource post-processing thread spent ~0.6 s of CPU in it in 12 s at Majula) and of the
// sound-zone switches (~29 ms of CPU across the game thread, the resource thread and the task workers in one switch,
// 2026-11-19). Recompiled it decodes byte by byte through big-endian guest loads; this is zlib 1.2.3's inffast.c
// working on the same guest structures: z_stream (next_in 0, avail_in 4, next_out 12, avail_out 16, msg 24,
// state 28) and inflate_state (mode 0, wsize 40, whave 44, write 48, window 52, hold 56, bits 60, lencode 76,
// distcode 80, lenbits 84, distbits 88), 32-bit big-endian fields; code entries are 4 bytes: op, bits, val (16-bit
// big-endian). The output is identical byte for byte (deflate decoding is deterministic) and so is the state it
// leaves. Errors (invalid codes, a distance too far back) are left to the original: nothing is written back before
// the end, so the original redoes the call from the same state, writes the same bytes and sets the game's own
// error message.
//
// ds2_native_inflate_verify (diagnostic): each call runs the native version, keeps its result, restores the output
// and runs the original, then compares the output bytes and the state; mismatches are logged.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>

REXCVAR_DEFINE_BOOL(ds2_native_inflate, true, "DS2",
                    "The game's zlib inflate_fast (sub_827820A8) runs as native code (same output and state); false = "
                    "the recompiled original");
REXCVAR_DEFINE_BOOL(ds2_native_inflate_verify, false, "DS2",
                    "Diagnostic: run both the native and the original inflate_fast on every call and compare their "
                    "output and state (the original's result is kept)");
REXCVAR_DEFINE_BOOL(ds2_inflate_stats, false, "DS2",
                    "Diagnostic: time every inflate_fast call (native or original) and log the totals every 4096 "
                    "calls: calls, output MB, ms, MB/s");

extern "C" void __imp__sub_827820A8(PPCContext& __restrict ctx, uint8_t* base);

namespace {

inline uint32_t Ld32(const uint8_t* base, uint32_t a) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + a));
}
inline void St32(uint8_t* base, uint32_t a, uint32_t v) {
  *reinterpret_cast<uint32_t*>(base + a) = __builtin_bswap32(v);
}

struct Code {
  uint32_t op, bits, val;
};
inline Code LoadCode(const uint8_t* p) {
  return Code{p[0], p[1], uint32_t(p[2]) << 8 | p[3]};
}

constexpr uint32_t kModeType = 11;

// Returns false (nothing written to the stream or state, output bytes possibly written) on any error, for the
// original to handle.
bool InflateFastNative(uint8_t* base, uint32_t strm, uint32_t start) {
  const uint32_t state = Ld32(base, strm + 28);
  const uint32_t next_in = Ld32(base, strm + 0);
  const uint32_t avail_in = Ld32(base, strm + 4);
  const uint32_t next_out = Ld32(base, strm + 12);
  const uint32_t avail_out = Ld32(base, strm + 16);
  const uint8_t* in = base + next_in;
  const uint8_t* const last = in + (avail_in - 5);
  uint8_t* out = base + next_out;
  const uint8_t* const beg = out - (start - avail_out);
  uint8_t* const end = out + (avail_out - 257);
  const uint32_t wsize = Ld32(base, state + 40);
  const uint32_t whave = Ld32(base, state + 44);
  const uint32_t write = Ld32(base, state + 48);
  const uint8_t* const window = base + Ld32(base, state + 52);
  uint32_t hold = Ld32(base, state + 56);
  uint32_t bits = Ld32(base, state + 60);
  const uint8_t* const lcode = base + Ld32(base, state + 76);
  const uint8_t* const dcode = base + Ld32(base, state + 80);
  const uint32_t lmask = (1u << Ld32(base, state + 84)) - 1;
  const uint32_t dmask = (1u << Ld32(base, state + 88)) - 1;
  bool end_of_block = false;
  do {
    if (bits < 15) {
      hold += uint32_t(*in++) << bits;
      bits += 8;
      hold += uint32_t(*in++) << bits;
      bits += 8;
    }
    Code here = LoadCode(lcode + 4 * (hold & lmask));
    for (;;) {  // dolen
      uint32_t op = here.bits;
      hold >>= op;
      bits -= op;
      op = here.op;
      if (op == 0) {  // literal
        *out++ = uint8_t(here.val);
        break;
      }
      if (op & 16) {  // length base
        uint32_t len = here.val;
        op &= 15;
        if (op) {
          if (bits < op) {
            hold += uint32_t(*in++) << bits;
            bits += 8;
          }
          len += hold & ((1u << op) - 1);
          hold >>= op;
          bits -= op;
        }
        if (bits < 15) {
          hold += uint32_t(*in++) << bits;
          bits += 8;
          hold += uint32_t(*in++) << bits;
          bits += 8;
        }
        here = LoadCode(dcode + 4 * (hold & dmask));
        for (;;) {  // dodist
          op = here.bits;
          hold >>= op;
          bits -= op;
          op = here.op;
          if (op & 16) {  // distance base
            uint32_t dist = here.val;
            op &= 15;
            if (bits < op) {
              hold += uint32_t(*in++) << bits;
              bits += 8;
              if (bits < op) {
                hold += uint32_t(*in++) << bits;
                bits += 8;
              }
            }
            dist += hold & ((1u << op) - 1);
            hold >>= op;
            bits -= op;
            op = uint32_t(out - beg);  // max distance in output
            const uint8_t* from;
            if (dist > op) {  // copy from the window
              op = dist - op;  // distance back in the window
              if (op > whave) {
                return false;  // "invalid distance too far back"
              }
              from = window;
              if (write == 0) {
                from += wsize - op;
                if (op < len) {
                  len -= op;
                  do {
                    *out++ = *from++;
                  } while (--op);
                  from = out - dist;
                }
              } else if (write < op) {  // wrap around the window
                from += wsize + write - op;
                op -= write;
                if (op < len) {
                  len -= op;
                  do {
                    *out++ = *from++;
                  } while (--op);
                  from = window;
                  if (write < len) {
                    op = write;
                    len -= op;
                    do {
                      *out++ = *from++;
                    } while (--op);
                    from = out - dist;
                  }
                }
              } else {  // contiguous in the window
                from += write - op;
                if (op < len) {
                  len -= op;
                  do {
                    *out++ = *from++;
                  } while (--op);
                  from = out - dist;
                }
              }
              while (len > 2) {
                *out++ = *from++;
                *out++ = *from++;
                *out++ = *from++;
                len -= 3;
              }
              if (len) {
                *out++ = *from++;
                if (len > 1) *out++ = *from++;
              }
            } else {
              from = out - dist;  // copy from the output (overlapping: byte by byte, as zlib)
              do {
                *out++ = *from++;
                *out++ = *from++;
                *out++ = *from++;
                len -= 3;
              } while (len > 2);
              if (len) {
                *out++ = *from++;
                if (len > 1) *out++ = *from++;
              }
            }
            break;
          }
          if ((op & 64) == 0) {  // 2nd level distance code
            here = LoadCode(dcode + 4 * (here.val + (hold & ((1u << op) - 1))));
            continue;
          }
          return false;  // "invalid distance code"
        }
        break;
      }
      if ((op & 64) == 0) {  // 2nd level length code
        here = LoadCode(lcode + 4 * (here.val + (hold & ((1u << op) - 1))));
        continue;
      }
      if (op & 32) {  // end of block
        end_of_block = true;
        break;
      }
      return false;  // "invalid literal/length code"
    }
  } while (!end_of_block && in < last && out < end);

  // Return the unused bytes (on entry bits < 8, so in won't go too far back).
  const uint32_t len = bits >> 3;
  in -= len;
  bits -= len << 3;
  hold &= (1u << bits) - 1;

  if (end_of_block) St32(base, state + 0, kModeType);
  St32(base, strm + 0, uint32_t(in - base));
  St32(base, strm + 12, uint32_t(out - base));
  St32(base, strm + 4, uint32_t((base + next_in + avail_in) - in));
  St32(base, strm + 16, uint32_t((base + next_out + avail_out) - out));
  St32(base, state + 56, hold);
  St32(base, state + 60, bits);
  return true;
}

std::atomic<bool> g_logged{false};
std::atomic<uint64_t> g_stat_calls{0}, g_stat_ns{0}, g_stat_bytes{0};

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

void AddStats(uint8_t* base, uint32_t strm, uint32_t avail_out_before, uint64_t ns, bool native) {
  const uint32_t produced = avail_out_before - Ld32(base, strm + 16);
  g_stat_ns.fetch_add(ns, std::memory_order_relaxed);
  g_stat_bytes.fetch_add(produced, std::memory_order_relaxed);
  const uint64_t calls = g_stat_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  if ((calls & 4095) == 0) {
    const double ms = double(g_stat_ns.load()) / 1e6, mb = double(g_stat_bytes.load()) / 1048576.0;
    REXLOG_INFO("ds2 inflate stats ({}): {} calls, {:.1f} MB out, {:.1f} ms, {:.0f} MB/s", native ? "native" : "original",
                calls, mb, ms, ms > 0 ? mb / (ms / 1000.0) : 0.0);
  }
}
std::atomic<uint32_t> g_verify_calls{0}, g_verify_mismatches{0};

void Verify(PPCContext& ctx, uint8_t* base, uint32_t strm, uint32_t start) {
  const uint32_t state = Ld32(base, strm + 28);
  const uint32_t next_out = Ld32(base, strm + 12);
  const uint32_t avail_out = Ld32(base, strm + 16);
  const uint32_t fields_strm[] = {0, 4, 12, 16, 24};
  const uint32_t fields_state[] = {0, 56, 60};
  uint32_t before_strm[5], before_state[3];
  for (int i = 0; i < 5; ++i) before_strm[i] = Ld32(base, strm + fields_strm[i]);
  for (int i = 0; i < 3; ++i) before_state[i] = Ld32(base, state + fields_state[i]);
  std::vector<uint8_t> out_before(base + next_out, base + next_out + avail_out);
  const bool native_ok = InflateFastNative(base, strm, start);
  uint32_t native_strm[5], native_state[3];
  for (int i = 0; i < 5; ++i) native_strm[i] = Ld32(base, strm + fields_strm[i]);
  for (int i = 0; i < 3; ++i) native_state[i] = Ld32(base, state + fields_state[i]);
  std::vector<uint8_t> out_native(base + next_out, base + next_out + avail_out);
  // Restore and run the original.
  std::memcpy(base + next_out, out_before.data(), avail_out);
  for (int i = 0; i < 5; ++i) St32(base, strm + fields_strm[i], before_strm[i]);
  for (int i = 0; i < 3; ++i) St32(base, state + fields_state[i], before_state[i]);
  ctx.r3.u64 = strm;
  ctx.r4.u64 = start;
  __imp__sub_827820A8(ctx, base);
  const uint32_t calls = ++g_verify_calls;
  if (!native_ok) {
    REXLOG_INFO("ds2 native inflate verify: call {} left to the original (stream error)", calls);
    return;
  }
  bool same = std::memcmp(out_native.data(), base + next_out, avail_out) == 0;
  for (int i = 0; i < 5; ++i) same &= native_strm[i] == Ld32(base, strm + fields_strm[i]);
  for (int i = 0; i < 3; ++i) same &= native_state[i] == Ld32(base, state + fields_state[i]);
  if (!same) {
    const uint32_t n = ++g_verify_mismatches;
    if (n <= 20) {
      REXLOG_WARN("ds2 native inflate verify: MISMATCH on call {} (next_out {:08X}, avail_out {}): native "
                  "in {:08X} out {:08X} hold {:08X} bits {} mode {}, original in {:08X} out {:08X} hold {:08X} "
                  "bits {} mode {}",
                  calls, next_out, avail_out, native_strm[0], native_strm[2], native_state[1], native_state[2],
                  native_state[0], Ld32(base, strm + 0), Ld32(base, strm + 12), Ld32(base, state + 56),
                  Ld32(base, state + 60), Ld32(base, state + 0));
    }
  }
  if ((calls & 4095) == 0) {
    REXLOG_INFO("ds2 native inflate verify: {} calls, {} mismatches", calls, g_verify_mismatches.load());
  }
}

}  // namespace

REX_HOOK_RAW(sub_827820A8) {
  static const bool enabled = REXCVAR_GET(ds2_native_inflate);
  static const bool stats = REXCVAR_GET(ds2_inflate_stats);
  const uint32_t strm = ctx.r3.u32;
  const uint32_t start = ctx.r4.u32;
  if (stats) {
    const uint32_t avail_out = Ld32(base, strm + 16);
    const uint64_t t0 = NowNs();
    if (!enabled || !InflateFastNative(base, strm, start)) {
      ctx.r3.u64 = strm;
      ctx.r4.u64 = start;
      __imp__sub_827820A8(ctx, base);
    }
    AddStats(base, strm, avail_out, NowNs() - t0, enabled);
    return;
  }
  if (!enabled) {
    __imp__sub_827820A8(ctx, base);
    return;
  }
  if (!g_logged.exchange(true)) {
    REXLOG_INFO("ds2: native zlib inflate_fast on (sub_827820A8){}",
                REXCVAR_GET(ds2_native_inflate_verify) ? ", verifying against the original" : "");
  }
  if (REXCVAR_GET(ds2_native_inflate_verify)) {
    Verify(ctx, base, strm, start);
    return;
  }
  if (!InflateFastNative(base, strm, start)) {
    __imp__sub_827820A8(ctx, base);
  }
}
