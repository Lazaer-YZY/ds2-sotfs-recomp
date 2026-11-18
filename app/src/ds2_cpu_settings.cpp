// ds2 - guest CPU settings, see ds2_cpu_settings.h.

#include "ds2_cpu_settings.h"

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(ds2_msr_per_thread, false, "DS2",
                    "mfmsr/mtmsrd regions as per-thread state instead of the process-wide mutex (read at start; "
                    "PORTING.md 5.1 of the Dead Space 2 Switch port)");
REXCVAR_DEFINE_BOOL(ds2_fpcr_ftz_fixed, false, "DS2",
                    "Keep FPCR flush-to-zero on for scalar code too (read at start; the guest code then never "
                    "rewrites FPCR). Changes denormal results of scalar float code");

extern "C" bool rex_msr_global_lock;
extern "C" bool rex_fpcr_ftz_fixed;

namespace ds2::cpu_settings {

void Configure() {
  rex_msr_global_lock = !REXCVAR_GET(ds2_msr_per_thread);
  rex_fpcr_ftz_fixed = REXCVAR_GET(ds2_fpcr_ftz_fixed);
  REXLOG_INFO("[ds2] FPCR flush-to-zero: {}", rex_fpcr_ftz_fixed ? "always on" : "switched by the guest code");
  REXLOG_INFO("[ds2] mtmsrd regions: {}", rex_msr_global_lock ? "process-wide mutex" : "per-thread state");
}

}  // namespace ds2::cpu_settings
