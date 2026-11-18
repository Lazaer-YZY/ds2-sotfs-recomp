/**
 * @file        rex/core/fiber_posix.cpp
 * @brief       POSIX backend for rex::thread::Fiber (makecontext/swapcontext)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#if defined(__APPLE__) && !defined(_XOPEN_SOURCE)
// Darwin hides the deprecated ucontext API unless a POSIX/XSI feature level
// is requested before <ucontext.h> is included. fiber.h includes ucontext.h,
// so the feature macro must be set in this translation unit first.
#define _XOPEN_SOURCE 700
#endif

#include <rex/platform.h>
#if REX_PLATFORM_LINUX || REX_PLATFORM_MAC

#include <rex/thread/fiber.h>

#include <cassert>

namespace rex::thread {

thread_local Fiber* Fiber::tls_current_ = nullptr;

/*static*/ void Fiber::Trampoline() {
  // tls_current_ was updated by SwitchTo before swapcontext returned here.
  Fiber* f = tls_current_;
  f->entry_(f->arg_);
#if REX_PLATFORM_ANDROID
  // Entered via ret with no caller frame: returning is undefined (as with
  // uc_link == nullptr on the ucontext path), so fail loudly.
  __builtin_trap();
#endif
}

#if REX_PLATFORM_ANDROID
#if !REX_ARCH_ARM64
#error "Android fibers are only implemented for arm64"
#endif

// Bionic lacks the ucontext functions. rex_fiber_switch(&from->sp_, to->sp_)
// saves the AAPCS64 callee-saved state (x19-x30, d8-d15) and FPCR (guest
// rounding/flush mode, per fiber) on the current stack,
// stores SP, loads the target SP, restores its state and returns into it.
extern "C" void rex_fiber_switch(void** save_sp, void* load_sp);
asm(R"(
  .text
  .p2align 4
  .globl rex_fiber_switch
  .hidden rex_fiber_switch
  .type rex_fiber_switch, %function
rex_fiber_switch:
  sub sp, sp, #176
  stp x19, x20, [sp, #0]
  stp x21, x22, [sp, #16]
  stp x23, x24, [sp, #32]
  stp x25, x26, [sp, #48]
  stp x27, x28, [sp, #64]
  stp x29, x30, [sp, #80]
  stp d8, d9, [sp, #96]
  stp d10, d11, [sp, #112]
  stp d12, d13, [sp, #128]
  stp d14, d15, [sp, #144]
  mrs x9, fpcr
  str x9, [sp, #160]
  mov x2, sp
  str x2, [x0]
  mov sp, x1
  ldp x19, x20, [sp, #0]
  ldp x21, x22, [sp, #16]
  ldp x23, x24, [sp, #32]
  ldp x25, x26, [sp, #48]
  ldp x27, x28, [sp, #64]
  ldp x29, x30, [sp, #80]
  ldp d8, d9, [sp, #96]
  ldp d10, d11, [sp, #112]
  ldp d12, d13, [sp, #128]
  ldp d14, d15, [sp, #144]
  ldr x9, [sp, #160]
  msr fpcr, x9
  add sp, sp, #176
  ret
  .size rex_fiber_switch, .-rex_fiber_switch
)");

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();  // sp_ is filled by the first SwitchTo away from it.
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);
  auto top = reinterpret_cast<uintptr_t>(f->stack_.data() + f->stack_.size()) & ~uintptr_t(15);
  auto* frame = reinterpret_cast<uint64_t*>(top - 176);
  for (int i = 0; i < 22; ++i) {
    frame[i] = 0;
  }
  uint64_t fpcr;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  frame[20] = fpcr;  // [sp, #160]: new fibers start with the creator's FPCR.
  // First switch "returns" into Trampoline (x30 slot, [sp, #88]) with SP at the
  // 16-byte aligned top of the fiber stack; x29 = 0 terminates frame chains.
  frame[11] = reinterpret_cast<uint64_t>(&Fiber::Trampoline);
  f->sp_ = frame;
  return f;
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  rex_fiber_switch(&from->sp_, target->sp_);
}

#else  // !REX_PLATFORM_ANDROID

Fiber* Fiber::ConvertCurrentThread() {
  auto* f = new Fiber();
  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->is_thread_fiber_ = true;
  tls_current_ = f;
  return f;
}

Fiber* Fiber::Create(size_t stack_size, void (*entry)(void*), void* arg) {
  auto* f = new Fiber();
  f->entry_ = entry;
  f->arg_ = arg;
  f->stack_.resize(stack_size);

  if (getcontext(&f->context_) == -1) {
    delete f;
    return nullptr;
  }
  f->context_.uc_stack.ss_sp = f->stack_.data();
  f->context_.uc_stack.ss_size = f->stack_.size();
  f->context_.uc_link = nullptr;
  // Trampoline reads entry_/arg_ from tls_current_ — no pointer splitting needed.
  makecontext(&f->context_, &Fiber::Trampoline, 0);
  return f;
}

void Fiber::SwitchTo(Fiber* target) {
  Fiber* from = tls_current_;
  tls_current_ = target;
  swapcontext(&from->context_, &target->context_);
}

#endif  // REX_PLATFORM_ANDROID

void Fiber::Destroy() {
  // Thread fibers are destroyed from the owning thread itself.
  if (is_thread_fiber_) {
    tls_current_ = nullptr;
  } else {
    assert(this != tls_current_ && "Destroy called on the currently running fiber");
  }
  // No POSIX equivalent of ConvertFiberToThread; stack_ is freed by the vector destructor.
  delete this;
}

}  // namespace rex::thread

#endif  // REX_PLATFORM_LINUX || REX_PLATFORM_MAC
