/**
 * @file        rex/core/fault_context.cpp
 * @brief       Crash context for unresolved host faults (PC, address, guest
 *              thread, symbolised host backtrace)
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/exception_handler.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <iterator>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include <rex/logging.h>
#include <rex/platform.h>

#if REX_PLATFORM_WIN32
#include "platform_win.h"

#include <dbghelp.h>
#else
#include <cxxabi.h>
#include <dlfcn.h>
#include <unwind.h>

#include <cstdlib>
#endif

namespace rex::arch {

namespace {

constexpr size_t kMaxFrames = 24;

std::atomic<FaultContextProvider> fault_context_provider{nullptr};

struct FaultDispatchState {
  bool active = false;
  uint64_t host_pc = 0;
  uint64_t fault_address = 0;
  const void* platform_context = nullptr;
};
thread_local FaultDispatchState tls_fault;
thread_local uint64_t tls_last_logged_pc = 0;
thread_local uint64_t tls_last_logged_address = ~uint64_t(0);

std::mutex& SymbolMutex() {
  static std::mutex mutex;
  return mutex;
}

#if REX_PLATFORM_WIN32

using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
using SymInitializeFn = BOOL(WINAPI*)(HANDLE, PCSTR, BOOL);
using SymFromAddrFn = BOOL(WINAPI*)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);

struct DbgHelp {
  SymFromAddrFn sym_from_addr = nullptr;
  bool ok = false;
};

// dbghelp.dll is loaded on first use only (no link dependency).
DbgHelp& GetDbgHelp() {
  static DbgHelp dbghelp = [] {
    DbgHelp d;
    HMODULE module = LoadLibraryW(L"dbghelp.dll");
    if (!module) {
      return d;
    }
    auto set_options = reinterpret_cast<SymSetOptionsFn>(
        reinterpret_cast<void*>(GetProcAddress(module, "SymSetOptions")));
    auto initialize = reinterpret_cast<SymInitializeFn>(
        reinterpret_cast<void*>(GetProcAddress(module, "SymInitialize")));
    d.sym_from_addr = reinterpret_cast<SymFromAddrFn>(
        reinterpret_cast<void*>(GetProcAddress(module, "SymFromAddr")));
    if (!set_options || !initialize || !d.sym_from_addr) {
      return d;
    }
    set_options(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    d.ok = initialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
    return d;
  }();
  return dbghelp;
}

std::string Symbolize(uint64_t address) {
  std::string module_name = "?";
  uint64_t module_base = 0;
  HMODULE module = nullptr;
  if (GetModuleHandleExW(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(address), &module)) {
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(module, path, MAX_PATH);
    if (len) {
      std::string full(path, len);
      size_t slash = full.find_last_of("\\/");
      module_name = slash == std::string::npos ? full : full.substr(slash + 1);
    }
    module_base = reinterpret_cast<uint64_t>(module);
  }

  DbgHelp& dbghelp = GetDbgHelp();
  if (dbghelp.ok) {
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    DWORD64 displacement = 0;
    if (dbghelp.sym_from_addr(GetCurrentProcess(), address, &displacement, symbol)) {
      return fmt::format("{}!{}+0x{:X}", module_name, symbol->Name, displacement);
    }
  }
  if (module_base) {
    return fmt::format("{}+0x{:X}", module_name, address - module_base);
  }
  return "?";
}

size_t CaptureBacktrace(const void* platform_context, uint64_t* frames, size_t max_frames) {
#if REX_ARCH_AMD64
  if (platform_context) {
    // Unwind from the faulting context itself (not from the handler's stack).
    CONTEXT context = *static_cast<const EXCEPTION_POINTERS*>(platform_context)->ContextRecord;
    size_t count = 0;
    while (count < max_frames && context.Rip) {
      frames[count++] = context.Rip;
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
      if (function) {
        void* handler_data = nullptr;
        DWORD64 establisher_frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, function, &context,
                         &handler_data, &establisher_frame, nullptr);
      } else {
        // Leaf function (or no unwind info): return address is at [rsp].
        if (!context.Rsp) {
          break;
        }
        context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
        context.Rsp += 8;
      }
    }
    return count;
  }
#else
  (void)platform_context;
#endif
  void* raw[kMaxFrames];
  USHORT n = RtlCaptureStackBackTrace(0, DWORD(std::min(max_frames, kMaxFrames)), raw, nullptr);
  for (USHORT i = 0; i < n; ++i) {
    frames[i] = reinterpret_cast<uint64_t>(raw[i]);
  }
  return n;
}

#else  // POSIX / Android

std::string Symbolize(uint64_t address) {
  Dl_info info{};
  if (!dladdr(reinterpret_cast<void*>(address), &info) || !info.dli_fname) {
    return "?";
  }
  std::string module_name = info.dli_fname;
  size_t slash = module_name.find_last_of('/');
  if (slash != std::string::npos) {
    module_name = module_name.substr(slash + 1);
  }
  if (info.dli_sname && info.dli_saddr) {
    int status = 0;
    char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
    std::string name = (status == 0 && demangled) ? demangled : info.dli_sname;
    std::free(demangled);
    return fmt::format("{}!{}+0x{:X}", module_name, name,
                       address - reinterpret_cast<uint64_t>(info.dli_saddr));
  }
  return fmt::format("{}+0x{:X}", module_name,
                     address - reinterpret_cast<uint64_t>(info.dli_fbase));
}

struct UnwindState {
  uint64_t* frames;
  size_t count;
  size_t max;
};

_Unwind_Reason_Code UnwindCallback(struct _Unwind_Context* context, void* arg) {
  auto* state = static_cast<UnwindState*>(arg);
  uint64_t pc = uint64_t(_Unwind_GetIP(context));
  if (pc) {
    state->frames[state->count++] = pc;
  }
  return state->count >= state->max ? _URC_END_OF_STACK : _URC_NO_REASON;
}

// Unwinds from the handler through the signal frame into the faulting frames.
size_t CaptureBacktrace(const void* /*platform_context*/, uint64_t* frames, size_t max_frames) {
  UnwindState state{frames, 0, max_frames};
  _Unwind_Backtrace(UnwindCallback, &state);
  return state.count;
}

#endif

}  // namespace

void SetFaultContextProvider(FaultContextProvider provider) {
  fault_context_provider.store(provider, std::memory_order_release);
}

void BeginFaultDispatch(uint64_t host_pc, uint64_t fault_address, const void* platform_context) {
  tls_fault = {true, host_pc, fault_address, platform_context};
}

void EndFaultDispatch() {
  tls_fault.active = false;
}

void LogCurrentFaultContext(const char* reason) {
  if (!tls_fault.active) {
    return;
  }
  if (tls_last_logged_pc == tls_fault.host_pc &&
      tls_last_logged_address == tls_fault.fault_address) {
    return;  // Already reported (e.g. by the guest memory path).
  }
  tls_last_logged_pc = tls_fault.host_pc;
  tls_last_logged_address = tls_fault.fault_address;

  std::lock_guard<std::mutex> lock(SymbolMutex());
  std::string out;
  out.reserve(4096);
  auto it = std::back_inserter(out);
  fmt::format_to(it, "==== Host fault: {} ====\n", reason ? reason : "unhandled");
  fmt::format_to(it, "  host pc: 0x{:016X} {}\n", tls_fault.host_pc,
                 Symbolize(tls_fault.host_pc));
  fmt::format_to(it, "  fault address: 0x{:016X}\n", tls_fault.fault_address);
  if (auto provider = fault_context_provider.load(std::memory_order_acquire)) {
    provider(out);
  }
  uint64_t frames[kMaxFrames];
  size_t count = CaptureBacktrace(tls_fault.platform_context, frames, kMaxFrames);
  fmt::format_to(it, "  host backtrace ({} frame(s)):\n", count);
  for (size_t i = 0; i < count; ++i) {
    fmt::format_to(it, "    #{:02} 0x{:016X} {}{}\n", i, frames[i], Symbolize(frames[i]),
                   frames[i] == tls_fault.host_pc ? "  <-- fault" : "");
  }
  out += "==== end of host fault ====";
  REXLOG_CRITICAL("{}", out);
  rex::FlushLogging();
}

}  // namespace rex::arch
