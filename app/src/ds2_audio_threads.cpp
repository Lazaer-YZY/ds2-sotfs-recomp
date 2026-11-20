// ds2 - FMOD's threads at audio priority.
//
// DS2's FMOD Ex 4.40 runs its threads (the mixer that polls the output every ~1 ms and mixes 256-sample blocks, the
// stream and loader threads) through FMOD::Thread::callback, sub_8285C200, each thread's entry (reached only through
// the thread trampoline's indirect call: ds2_register.cpp registers it, so this hook sees every FMOD thread). Guest
// thread priorities are ignored on Android, so in busy scenes the mixer competed equally with the game's job workers
// on the same four cores; a late mix is heard (XAudio2 then replays a block of FMOD's 4-block ring). The mixer now
// raises its scheduler priority once (ds2_fmod_thread_nice -10, like the SDK's audio_thread_nice for its audio worker
// and XMA decoder), the music stream a little (ds2_fmod_stream_nice -4); the bank loader and file threads keep the
// default so a bank load does not outrank the game's main thread. All are named "FMOD thread" for profiles.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#if defined(__ANDROID__) || defined(__linux__)
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

REXCVAR_DEFINE_INT32(ds2_fmod_thread_nice, -10, "DS2",
                     "Scheduler nice value of FMOD's mixer thread (Android/Linux; 0 = unchanged)")
    .range(-20, 19);
REXCVAR_DEFINE_INT32(ds2_fmod_stream_nice, -4, "DS2",
                     "Scheduler nice value of FMOD's stream thread (music; Android/Linux; 0 = unchanged); the bank "
                     "loader and file threads keep the default")
    .range(-20, 19);

extern "C" void __imp__sub_8285C200(PPCContext& __restrict ctx, uint8_t* base);

REX_HOOK_RAW(sub_8285C200) {
#if defined(__ANDROID__) || defined(__linux__)
  thread_local bool raised = false;
  if (!raised) {
    raised = true;
    prctl(PR_SET_NAME, "FMOD thread", 0, 0, 0);
    const int tid = int(syscall(SYS_gettid));
    // FMOD keeps the thread's name at +5 of the thread object: "FMOD mixer thread", "FMOD stream thread",
    // "FMOD nonblocking thread (0)" (the bank loader), "FMOD file thread" (Thor, 2026-11-20).
    char fmod_name[40] = {};
    if (const uint32_t obj = ctx.r3.u32) {
      for (int i = 0; i < 39; ++i) {
        const uint8_t c = base[obj + 5 + i];
        if (c < 0x20 || c >= 0x7F) break;
        fmod_name[i] = char(c);
      }
    }
    const bool mixer = std::strncmp(fmod_name, "FMOD mixer", 10) == 0;
    const bool stream = std::strncmp(fmod_name, "FMOD stream", 11) == 0;
    const int nice = mixer ? REXCVAR_GET(ds2_fmod_thread_nice) : stream ? REXCVAR_GET(ds2_fmod_stream_nice) : 0;
    if (std::getenv("DS2_FMOD_THREAD_DUMP")) {
      // Diagnostic: printable strings in the first 0x400 bytes of the thread object (FMOD keeps the thread's name).
      std::string found;
      const uint32_t obj = ctx.r3.u32;
      std::string run;
      for (uint32_t i = 0; obj && i < 0x400; ++i) {
        const uint8_t c = base[obj + i];
        if (c >= 0x20 && c < 0x7F) {
          run += char(c);
        } else {
          if (run.size() >= 5) found += fmt::format(" +{:X}:'{}'", i - uint32_t(run.size()), run);
          run.clear();
        }
      }
      REXLOG_INFO("ds2: FMOD thread {} object {:08X} strings:{}", tid, obj, found);
    }
    if (nice != 0) {
      if (setpriority(PRIO_PROCESS, id_t(tid), nice) == 0) {
        REXLOG_INFO("ds2: {} ({}, object {:08X}): nice {}", fmod_name, tid, ctx.r3.u32, nice);
      } else {
        REXLOG_WARN("ds2: FMOD thread {}: nice {} refused ({})", tid, nice, std::strerror(errno));
      }
    }
  }
#endif
  __imp__sub_8285C200(ctx, base);
}
