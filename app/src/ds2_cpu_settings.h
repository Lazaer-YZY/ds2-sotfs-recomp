// ds2 - guest CPU settings applied before the first guest instruction runs (from OnPostInitLogging).
//
// ds2_msr_per_thread: mfmsr/mtmsrd "interrupts off" regions as per-thread state instead of one process-wide mutex
//   (the codegen pch template reads rex_msr_global_lock).
// ds2_fpcr_ftz_fixed: keep FPCR flush-to-zero on for scalar code too, so guest code never rewrites FPCR.
#pragma once

namespace ds2::cpu_settings {
void Configure();
}  // namespace ds2::cpu_settings
