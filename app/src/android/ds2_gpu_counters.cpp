// ds2 - Adreno hardware counters (debug launches: --es DS2_GPU_COUNTERS 1; period DS2_GPU_COUNTERS_S, default 10).
//
// Ported from the Lightning Returns port (src/android/android_glue.cpp, StartGpuCounterSampler). Through the kernel
// driver, so it works with Turnip and Qualcomm's driver alike, without root: IOCTL_KGSL_PERFCOUNTER_GET reserves a
// counter for a countable (the kernel programs the select register), IOCTL_KGSL_PERFCOUNTER_READ reads the running
// 64-bit values. Reads need /sys/class/kgsl/kgsl-3d0/perfcounter = 1 (group-writable by adb's shell user; set it
// before the launch and back to 0 afterwards; a reboot resets it too). The counters are global (the whole GPU), so
// read them on a steady view. Countables: Adreno 7xx numbering from Mesa's a7xx_perfcntrs.xml. The GBIF/VBIF groups
// are held by the kernel (EBUSY). Every period, one line of values per second and one of derived ratios.

#include "ds2_android.h"

#include <rex/logging.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace ds2::android {

namespace {

struct KgslPerfcounterGet {
  uint32_t groupid, countable, offset, offset_hi, pad;
};
struct KgslPerfcounterReadGroup {
  uint32_t groupid, countable;
  uint64_t value;
};
struct KgslPerfcounterRead {
  KgslPerfcounterReadGroup* reads;
  uint32_t count;
  uint32_t pad[2];
};
#define DS2_KGSL_IOC_PERFCOUNTER_GET _IOWR(0x09, 0x38, KgslPerfcounterGet)
#define DS2_KGSL_IOC_PERFCOUNTER_READ _IOWR(0x09, 0x3B, KgslPerfcounterRead)

struct GpuCountable {
  uint32_t group, countable;
  const char* name;
};
// Groups: 0 CP, 8 UCHE, 9 TP, 0xA SP, 0xB RB, 0x7 VFD, 0x3 RAS, 0x4 PC, 0x10 LRZ.
constexpr GpuCountable kGpuCountables[] = {
    {0x0, 0, "cp_always"},          {0x0, 2, "cp_busy"},
    {0xA, 0, "sp_busy"},            {0xA, 1, "sp_alu_working"},     {0xA, 2, "sp_efu_working"},
    {0xA, 4, "sp_stall_tp"},        {0xA, 5, "sp_stall_uche"},      {0xA, 6, "sp_stall_rb"},
    {0xA, 7, "sp_non_exec"},        {0xA, 9, "sp_wave_ctx_cycles"}, {0xA, 8, "sp_wave_contexts"},
    {0xA, 37, "sp_fs_tex_instr"},   {0xA, 40, "sp_fs_full_alu"},    {0xA, 41, "sp_fs_half_alu"},
    {0xA, 39, "sp_fs_efu"},         {0xA, 52, "sp_icl1_miss"},      {0xA, 68, "sp_starved_by_tp"},
    {0xA, 74, "sp_any_eu_fs"},      {0xA, 76, "sp_any_eu_vs"},      {0xA, 35, "sp_vs_full_alu"},
    {0xA, 43, "sp_vs_instr"},       {0xA, 44, "sp_fs_instr"},       {0xA, 14, "sp_fs_duration"},
    {0xA, 15, "sp_vs_duration"},
    {0x9, 0, "tp_busy"},            {0x9, 1, "tp_stall_uche"},      {0x9, 6, "tp_l1_req"},
    {0x9, 7, "tp_l1_miss"},         {0x9, 10, "tp_out_px"},         {0x9, 26, "tp_out_bilinear"},
    {0x9, 27, "tp_out_mip"},        {0x9, 28, "tp_out_aniso"},      {0x9, 12, "tp_filter_32bit"},
    {0x9, 44, "tp_l1_bank_conflict"},
    {0x8, 0, "uche_busy"},          {0x8, 1, "uche_stall_arb"},     {0x8, 4, "uche_rd_beats_tp"},
    {0x8, 2, "uche_vbif_lat_cyc"},  {0x8, 3, "uche_vbif_lat_n"},
    {0xB, 0, "rb_busy"},            {0xB, 44, "rb_stall_ccu"},      {0xB, 5, "rb_starve_sp"},
    {0xB, 14, "rb_c_read"},         {0xB, 15, "rb_c_write"},        {0xB, 12, "rb_z_read"},
    {0xB, 13, "rb_z_write"},        {0xB, 22, "rb_ps_invocations"},
};

}  // namespace

void StartGpuCounters() {
  const char* env = std::getenv("DS2_GPU_COUNTERS");
  if (!env || std::strcmp(env, "1") != 0) return;
  const char* period_env = std::getenv("DS2_GPU_COUNTERS_S");
  const int period_s = std::max(1, period_env ? std::atoi(period_env) : 10);
  std::thread([period_s] {
    int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
      REXLOG_WARN("ds2 GPU counters: open /dev/kgsl-3d0 failed (errno {})", errno);
      return;
    }
    std::vector<KgslPerfcounterReadGroup> reads;
    std::vector<const char*> names;
    std::string failed;
    for (const GpuCountable& c : kGpuCountables) {
      KgslPerfcounterGet get = {c.group, c.countable, 0, 0, 0};
      if (ioctl(fd, DS2_KGSL_IOC_PERFCOUNTER_GET, &get) != 0) {
        failed += std::string(failed.empty() ? "" : ", ") + c.name + " (errno " + std::to_string(errno) + ")";
        continue;
      }
      reads.push_back({c.group, c.countable, 0});
      names.push_back(c.name);
    }
    REXLOG_INFO("ds2 GPU counters: {} reserved, failed: {}", reads.size(), failed.empty() ? "none" : failed);
    if (reads.empty()) return;
    auto read_all = [&](std::vector<uint64_t>& out) {
      KgslPerfcounterRead rd = {reads.data(), uint32_t(reads.size()), {0, 0}};
      if (ioctl(fd, DS2_KGSL_IOC_PERFCOUNTER_READ, &rd) != 0) return false;
      out.resize(reads.size());
      for (size_t i = 0; i < reads.size(); ++i) out[i] = reads[i].value;
      return true;
    };
    std::vector<uint64_t> prev, cur;
    if (!read_all(prev)) {
      REXLOG_WARN("ds2 GPU counters: PERFCOUNTER_READ failed (errno {}); is kgsl-3d0/perfcounter 1?", errno);
      return;
    }
    auto prev_time = std::chrono::steady_clock::now();
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(period_s));
      if (!read_all(cur)) continue;
      const auto now = std::chrono::steady_clock::now();
      const double secs = std::max(1e-3, std::chrono::duration<double>(now - prev_time).count());
      auto delta = [&](const char* name) -> double {
        for (size_t i = 0; i < names.size(); ++i)
          if (std::strcmp(names[i], name) == 0) return double(cur[i] - prev[i]);
        return 0.0;
      };
      std::string line;
      for (size_t i = 0; i < names.size(); ++i) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s%s %.2fM", i ? ", " : "", names[i], double(cur[i] - prev[i]) / secs / 1e6);
        line += buf;
      }
      auto pct = [](double a, double b) { return b > 0 ? 100.0 * a / b : 0.0; };
      const double sp_busy = delta("sp_busy"), tp_busy = delta("tp_busy"), cp_busy = delta("cp_busy");
      const double rb_busy = delta("rb_busy"), uche_busy = delta("uche_busy");
      char derived[640];
      std::snprintf(derived, sizeof(derived),
                    "gpu busy %.1f%%; of gpu busy: sp %.1f%%, tp %.1f%%, rb %.1f%%, uche %.1f%%; of sp busy: alu "
                    "%.1f%% / efu %.1f%% / stall tp %.1f%% / stall uche %.1f%% / stall rb %.1f%% / non-exec %.1f%%; "
                    "tp stall uche %.1f%%, tp l1 miss %.1f%%, uche latency %.0f cyc; rb stall ccu %.1f%%, rb starve "
                    "sp %.1f%%; fs tex/alu %.3f; vs share eu-busy %.1f%% / instr %.1f%%; fs half-alu %.1f%%",
                    pct(cp_busy, delta("cp_always")), pct(sp_busy, cp_busy), pct(tp_busy, cp_busy),
                    pct(rb_busy, cp_busy), pct(uche_busy, cp_busy), pct(delta("sp_alu_working"), sp_busy),
                    pct(delta("sp_efu_working"), sp_busy), pct(delta("sp_stall_tp"), sp_busy),
                    pct(delta("sp_stall_uche"), sp_busy), pct(delta("sp_stall_rb"), sp_busy),
                    pct(delta("sp_non_exec"), sp_busy), pct(delta("tp_stall_uche"), tp_busy),
                    pct(delta("tp_l1_miss"), delta("tp_l1_req")),
                    delta("uche_vbif_lat_n") > 0 ? delta("uche_vbif_lat_cyc") / delta("uche_vbif_lat_n") : 0.0,
                    pct(delta("rb_stall_ccu"), rb_busy), pct(delta("rb_starve_sp"), rb_busy),
                    delta("sp_fs_full_alu") + delta("sp_fs_half_alu") > 0
                        ? delta("sp_fs_tex_instr") / (delta("sp_fs_full_alu") + delta("sp_fs_half_alu"))
                        : 0.0,
                    pct(delta("sp_any_eu_vs"), delta("sp_any_eu_vs") + delta("sp_any_eu_fs")),
                    pct(delta("sp_vs_instr"), delta("sp_vs_instr") + delta("sp_fs_instr")),
                    pct(delta("sp_fs_half_alu"), delta("sp_fs_full_alu") + delta("sp_fs_half_alu")));
      REXLOG_INFO("ds2 GPU counters ({:.1f} s, per second): {}", secs, line);
      REXLOG_INFO("ds2 GPU counters derived: {}", derived);
      prev.swap(cur);
      prev_time = now;
    }
  }).detach();
}

}  // namespace ds2::android
