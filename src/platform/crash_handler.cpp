/**
 * @file    platform/crash_handler.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/crash_handler.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <rex/types.h>
#include <string>
#include <typeinfo>

#include <rex/exception_handler.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/version.h>
#if defined(__PROSPERO__)
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#endif

#include "core/logging.h"
#include "platform/fatal_dialog.h"

#if defined(_WIN32)
// Linker-provided base of THIS module (the host exe). Its ADDRESS is the
// post-ASLR load base. Using it avoids including Windows.h (project rule) while
// still reducing a faulting host PC to an RVA that the .map/.pdb resolves
// offline.
extern "C" const unsigned char __ImageBase;

// Exported by kernel32/ntdll. Declared locally to avoid pulling in Windows.h.
// x64 has a single calling convention so __stdcall is a no-op there.
extern "C"
    __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(
        unsigned long frames_to_skip, unsigned long frames_to_capture,
        void **back_trace, unsigned long *back_trace_hash);
#else
#include <cxxabi.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace bd::platform {
namespace {

// Guest virtual address space is the low 4 GB of the host mapping.
constexpr u64 kGuestAddressSpaceSize = 0x100000000ull;

// Generous upper bound on the host exe image span (it embeds all recompiled
// guest code, so it is large). Used only to decide whether a host address
// reduces to an in-module RVA, and oversizing it never misclassifies an
// in-module address.
constexpr u64 kHostImageSpan = 0x20000000ull; // 512 MiB

#if defined(_WIN32)
u64 HostModuleBase() { return reinterpret_cast<u64>(&__ImageBase); }
#else
// No module base RVA scheme on Linux. backtrace_symbols prints symbol names
// directly, so the base is only used to suppress bogus RVA lines.
u64 HostModuleBase() { return 0; }
#endif

// Resolved at install, never on the crash path: the handler must not call into
// the filesystem while unwinding a fault.
std::string g_host_module_name;

const char *HostModuleName() {
  return g_host_module_name.empty() ? "host" : g_host_module_name.c_str();
}

// Set by whichever crash path reports first. A second entry, whether a fault
// inside a handler or the abort() that ends the terminate handler, skips
// reporting and goes straight to dying, so no path can loop.
std::atomic_flag s_reporting = ATOMIC_FLAG_INIT;

#if !defined(_WIN32)
// Restore the kernel's default action for 'sig' and take it here. Returning
// from a POSIX handler instead (what the SDK dispatcher does when no handler
// claims the fault) re-executes the faulting instruction forever: on Linux
// there is no equivalent of EXCEPTION_CONTINUE_SEARCH handing off to the OS.
// Dying through the default action is also what produces the core dump.
[[noreturn]] void DieWithDefaultDisposition(int sig) {
  struct sigaction dfl{};
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, nullptr);

  // A signal is blocked for the duration of its own handler, so unblock it and
  // the raise below comes back here rather than after an unreachable return.
  sigset_t unblock;
  sigemptyset(&unblock);
  sigaddset(&unblock, sig);
  pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);

  raise(sig);
  _exit(128 + sig); // only reachable if the default action did not fire
}
#endif

const char *ExceptionCodeName(rex::arch::Exception::Code code) {
  using Code = rex::arch::Exception::Code;
  switch (code) {
  case Code::kAccessViolation:
    return "ACCESS_VIOLATION";
  case Code::kIllegalInstruction:
    return "ILLEGAL_INSTRUCTION";
  default:
    return "UNKNOWN";
  }
}

const char *AvOperationName(rex::arch::Exception::AccessViolationOperation op) {
  using Op = rex::arch::Exception::AccessViolationOperation;
  switch (op) {
  case Op::kRead:
    return "read";
  case Op::kWrite:
    return "write";
  default:
    return "unknown";
  }
}

// Walk the faulting thread's stack (the VEH runs inline on it) and log each
// return address with its module-relative RVA, giving the call chain that
// led to the separately logged faulting PC. The top frames are the crash
// handler itself, so the faulting caller is the first module+RVA below the
// OS dispatch frames.
void LogBacktrace(u64 base) {
#if defined(_WIN32)
  void *frames[32] = {};
  const unsigned short n = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
  if (!n)
    return;
  BD_CRITICAL("backtrace ({} frames, top frames are the crash handler):", n);
  for (unsigned short i = 0; i < n; ++i) {
    const u64 a = reinterpret_cast<u64>(frames[i]);
    if (a >= base && a - base < kHostImageSpan) {
      BD_CRITICAL("    [{:>2}] {:#018x}  ({}+{:#010x})", i, a, HostModuleName(),
                  a - base);
    } else {
      BD_CRITICAL("    [{:>2}] {:#018x}", i, a);
    }
  }
#else
  (void)base;
  void *frames[32] = {};
  const int n = backtrace(frames, 32);
  if (n <= 0)
    return;
  BD_CRITICAL("backtrace ({} frames, top frames are the crash handler):", n);
  char **syms = backtrace_symbols(frames, n);
  for (int i = 0; i < n; ++i) {
    const u64 a = reinterpret_cast<u64>(frames[i]);
    if (syms && syms[i]) {
      BD_CRITICAL("    [{:>2}] {:#018x}  {}", i, a, syms[i]);
    } else {
      BD_CRITICAL("    [{:>2}] {:#018x}", i, a);
    }
  }
  free(syms);
#endif
}

// An indirect call to a garbage target faults on instruction fetch AFTER
// pushing the return address, so the caller is still at [sp] when rip is
// unwalkable - which is also when backtrace() faults and takes the whole frame
// list with it. Emitted in backtrace_symbols' module(+0xoffset) form so
// dbg-reblue.sh symbolizes these unchanged.
void LogStackCodeAddresses(const rex::arch::HostThreadContext *ctx,
                           [[maybe_unused]] u64 base) {
  if (!ctx)
    return;
#if REX_ARCH_AMD64
  const u64 sp = ctx->int_registers[4];
#elif REX_ARCH_ARM64
  const u64 sp = ctx->sp;
#else
  return;
#endif
#if REX_ARCH_AMD64 || REX_ARCH_ARM64
  if (!sp || (sp % sizeof(u64)) != 0)
    return;
  // Never leave sp's own page: the next one can be the guard page, and a second
  // fault in here costs the whole report.
  constexpr u64 kPageSize = 4096;
  const u64 limit = std::min(sp + 64 * sizeof(u64), (sp | (kPageSize - 1)) + 1);
  BD_CRITICAL("stack code addresses (sp {:#018x}, first is the return address "
              "of the call that faulted):",
              sp);
  u32 found = 0;
  for (u64 p = sp; p + sizeof(u64) <= limit; p += sizeof(u64)) {
    u64 v = 0;
    std::memcpy(&v, reinterpret_cast<const void *>(p), sizeof(v));
    if (!v)
      continue;
#if defined(_WIN32)
    if (v < base || v - base >= kHostImageSpan)
      continue;
    BD_CRITICAL("    [sp+{:#05x}] {:#018x}  ({}+{:#010x})", p - sp, v,
                HostModuleName(), v - base);
#else
    Dl_info info{};
    // dladdr resolves mapped module addresses only, so it doubles as the
    // is-this-code test: stack data words fail to resolve.
    if (!dladdr(reinterpret_cast<void *>(v), &info) || !info.dli_fbase ||
        !info.dli_fname) {
      continue;
    }
    const char *slash = std::strrchr(info.dli_fname, '/');
    BD_CRITICAL("    [sp+{:#05x}] {:#018x}  {}(+{:#010x})", p - sp, v,
                slash ? slash + 1 : info.dli_fname,
                v - reinterpret_cast<u64>(info.dli_fbase));
#endif
    if (++found >= 24)
      break;
  }
  if (!found)
    BD_CRITICAL("    (no module addresses on the stack)");
#endif
}

// Read the register file straight off the public HostThreadContext members -
// the SDK's GetRegisterName/GetStringFromValue helpers are header-declared but
// not exported by the runtime lib, so format the values here instead.
void LogRegisters(const rex::arch::HostThreadContext *ctx) {
  if (!ctx)
    return;
#if REX_ARCH_AMD64
  static const char *const kNames[16] = {
      "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
      "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
  BD_CRITICAL("    rip = {:#018x}   eflags = {:#010x}", ctx->rip, ctx->eflags);
  for (int i = 0; i < 16; ++i) {
    BD_CRITICAL("    {:>3} = {:#018x}", kNames[i], ctx->int_registers[i]);
  }
#elif REX_ARCH_ARM64
  BD_CRITICAL("    pc = {:#018x}   sp = {:#018x}   pstate = {:#010x}", ctx->pc,
              ctx->sp, ctx->pstate);
  for (int i = 0; i < 31; ++i) {
    BD_CRITICAL("    x{:<2} = {:#018x}", i, ctx->x[i]);
  }
#endif
}

// Chained after the SDK MMIO handler, so it only sees faults the runtime
// declined. Returns false: never recover, let the OS produce its default
// termination.
#if defined(__PROSPERO__)
// PS5 bring-up: which logical processor each guest thread believes it is on,
// and what the game's audio engine has per processor (the engine at
// 0x827854FC keeps two renderer pointers per processor at +12, and each
// processor's worker thread at +332). A null mix buffer in the audio mixer
// pointed here.
u32 GuestU32(u8 *membase, u32 va) {
  u32 v;
  std::memcpy(&v, membase + va, 4);
  return __builtin_bswap32(v);
}

void DumpGuest(u8 *mb, const char *what, u32 va, u32 len) {
  if (va < 0x10000 || va >= 0xE0000000u || len == 0)
    return;
  BD_CRITICAL("  {} @ {:08X}:", what, va);
  for (u32 off = 0; off < len; off += 32) {
    std::string line;
    for (u32 i = 0; i < 32 && off + i < len; i += 4)
      line += fmt::format(" {:08X}", GuestU32(mb, va + off + i));
    BD_CRITICAL("    +{:03X}{}", off, line);
  }
}

// The faulting guest context (volatile registers; the recompiled code keeps
// r14-r31 in host registers), the guest stack frame, and the object the
// audio mixer was working on: sub_82684D18 keeps its voice (guest r31) in
// host r12 and voice+184 in host r15 in this build.
void LogGuestContextPs5(const rex::arch::HostThreadContext *hc) {
  auto *rt = rex::Runtime::instance();
  u8 *mb = rt ? rt->virtual_membase() : nullptr;
  auto *self = rex::system::XThread::GetCurrentThread();
  if (!mb || !self || !self->thread_state())
    return;
  const ::PPCContext *c = self->thread_state()->context();
  if (!c)
    return;
  BD_CRITICAL("guest context: r1={:08X} r3={:08X} r4={:08X} r5={:08X} r6={:08X} "
              "r7={:08X} r8={:08X} r9={:08X} r10={:08X} r11={:08X} r12={:08X} r13={:08X}",
              c->r1.u32, c->r3.u32, c->r4.u32, c->r5.u32, c->r6.u32, c->r7.u32,
              c->r8.u32, c->r9.u32, c->r10.u32, c->r11.u32, c->r12.u32, c->r13.u32);
  DumpGuest(mb, "guest stack frame", c->r1.u32, 0xE0);
#if REX_ARCH_AMD64
  if (!hc)
    return;
  const u64 r12 = hc->int_registers[12], r15 = hc->int_registers[15];
  if (r15 == r12 + 184 && r12 < 0xE0000000u) {
    const u32 voice = static_cast<u32>(r12);
    DumpGuest(mb, "mixer voice (host r12)", voice, 0x120);
    const u32 count = mb[voice + 56];
    const u32 elems = GuestU32(mb, voice + 188);
    BD_CRITICAL("  voice: {} output(s) at {:08X}, +184={:08X}", count, elems,
                GuestU32(mb, voice + 184));
    for (u32 i = 0; i < count && i < 8; ++i)
      DumpGuest(mb, fmt::format("output {}", i).c_str(), elems + i * 88, 88);
    DumpGuest(mb, "voice+184 target", GuestU32(mb, voice + 184), 0x40);
  } else {
    BD_CRITICAL("  (host r12/r15 are not the mixer voice: {:#x} {:#x})", r12, r15);
  }
#endif
}

void LogGuestThreadsPs5() {
  auto *rt = rex::Runtime::instance();
  auto *ks = REX_KERNEL_STATE();
  u8 *mb = rt ? rt->virtual_membase() : nullptr;
  if (!ks || !mb)
    return;
  auto *self = rex::system::XThread::GetCurrentThread();
  BD_CRITICAL("guest threads (* faulted):");
  for (auto &t : ks->object_table()->GetObjectsByType<rex::system::XThread>(
           rex::system::XObject::Type::Thread)) {
    const u32 pcr = t->pcr_ptr();
    const u32 kt = t->guest_object();
    const auto *cp = t->creation_params();
    BD_CRITICAL("  {}{:08X} '{}' guest={} start={:08X} flags={:08X} pcr={:08X} "
                "pcr.cpu={} kthread={:08X} kthread.cpu={}",
                t.get() == self ? "*" : " ", t->handle(), t->name(),
                t->is_guest_thread() ? 1 : 0, cp->start_address,
                cp->creation_flags, pcr, pcr ? int(mb[pcr + 0x10C]) : -1, kt,
                kt ? int(mb[kt + 0xBF]) : -1);
  }
  const u32 engine = GuestU32(mb, 0x827854FC);
  BD_CRITICAL("audio engine {:08X}", engine);
  if (engine == 0 || engine >= 0xE0000000u)
    return;
  BD_CRITICAL("  +300={:08X} +304={:08X}", GuestU32(mb, engine + 300),
              GuestU32(mb, engine + 304));
  for (u32 cpu = 0; cpu < 6; ++cpu) {
    const u32 r0 = GuestU32(mb, engine + 12 + cpu * 8);
    const u32 r1 = GuestU32(mb, engine + 16 + cpu * 8);
    const u32 th = GuestU32(mb, engine + (cpu + 83) * 4);
    auto buf = [&](u32 r) {
      return r && r < 0xE0000000u
                 ? fmt::format("{:08X}[+12={:08X} +16={:08X} +20={:08X}]", r,
                               GuestU32(mb, r + 12), GuestU32(mb, r + 16),
                               GuestU32(mb, r + 20))
                 : fmt::format("{:08X}", r);
    };
    BD_CRITICAL("  cpu{} thread={:08X} renderer0={} renderer1={}", cpu, th,
                buf(r0), buf(r1));
  }
}
#endif

bool CrashHandler(rex::arch::Exception *ex, void * /*data*/) {
#if defined(_WIN32)
  if (s_reporting.test_and_set(std::memory_order_acq_rel))
    return false;
#else
  const int sig = ex->code() == rex::arch::Exception::Code::kIllegalInstruction
                      ? SIGILL
                      : SIGSEGV;
  if (s_reporting.test_and_set(std::memory_order_acq_rel))
    DieWithDefaultDisposition(sig);
#endif

  const u64 host_base = HostModuleBase();

  BD_CRITICAL("================ reblue host crash ================");
  BD_CRITICAL("build: {}", REXGLUE_BUILD_TITLE);
  BD_CRITICAL("module base: {} @ {:#018x}", HostModuleName(), host_base);
  BD_CRITICAL("exception: {} @ host pc {:#018x}", ExceptionCodeName(ex->code()),
              ex->pc());
  if (host_base && ex->pc() >= host_base &&
      ex->pc() - host_base < kHostImageSpan) {
    BD_CRITICAL("faulting RVA: {}+{:#010x}", HostModuleName(),
                ex->pc() - host_base);
  }

  if (ex->code() == rex::arch::Exception::Code::kAccessViolation) {
    const u64 fa = ex->fault_address();
    BD_CRITICAL("fault address: {:#018x} ({})", fa,
                AvOperationName(ex->access_violation_operation()));

    // Map a guest range fault back to its guest VA, the actionable triage line
    // for a fault inside recompiled code.
    auto *rt = rex::Runtime::instance();
    u8 *membase = rt ? rt->virtual_membase() : nullptr;
    const u64 base = reinterpret_cast<u64>(membase);
    if (membase && fa >= base && fa < base + kGuestAddressSpaceSize) {
      BD_CRITICAL("guest fault VA: {:#010x}", static_cast<u32>(fa - base));
    }
  }

#if defined(__PROSPERO__)
  LogGuestThreadsPs5();
  LogGuestContextPs5(ex->thread_context());
#endif

  BD_CRITICAL("registers:");
  LogRegisters(ex->thread_context());
  LogStackCodeAddresses(ex->thread_context(), host_base);
  // Flush before the unwind, not just after it: backtrace() faults on a garbage
  // rip, and what is already logged outweighs the frames it might add.
  rex::FlushLogging();
  LogBacktrace(host_base);
  BD_CRITICAL("===================================================");

  // Drain sinks before the OS tears us down, since a crash bypasses OnShutdown.
  rex::FlushLogging();

  const bool in_module = host_base && ex->pc() >= host_base &&
                         ex->pc() - host_base < kHostImageSpan;
  const std::string where =
      in_module ? fmt::format("{}+{:#010x}", HostModuleName(),
                              ex->pc() - host_base)
                : fmt::format("{:#018x}", ex->pc());
  ShowFatalError("reblue Crashed",
                 fmt::format("reblue hit a fatal error and has to close.\n\n"
                             "{} at {}",
                             ExceptionCodeName(ex->code()), where));
#if !defined(_WIN32)
  DieWithDefaultDisposition(sig);
#endif
  return false;
}

std::string TypeName(const std::type_info &ti) {
#if defined(_WIN32)
  return ti.name();
#else
  int status = 0;
  char *demangled = abi::__cxa_demangle(ti.name(), nullptr, nullptr, &status);
  std::string out = (status == 0 && demangled) ? demangled : ti.name();
  std::free(demangled);
  return out;
#endif
}

// An uncaught C++ exception (bad_alloc, a filesystem or plume throw) ends the
// process through std::terminate, which no signal handler sees until abort()
// has already discarded the exception. Naming it here is usually the whole
// diagnosis.
[[noreturn]] void TerminateHandler() {
  if (s_reporting.test_and_set(std::memory_order_acq_rel))
    std::_Exit(3);

  std::string detail = "std::terminate with no active exception";
  if (std::exception_ptr active = std::current_exception()) {
    try {
      std::rethrow_exception(active);
    } catch (const std::exception &e) {
      detail = fmt::format("uncaught {}: {}", TypeName(typeid(e)), e.what());
    } catch (...) {
      detail = "uncaught exception of non-std type";
    }
  }

  BD_CRITICAL("================ reblue host crash ================");
  BD_CRITICAL("build: {}", REXGLUE_BUILD_TITLE);
  BD_CRITICAL("{}", detail);
  LogBacktrace(HostModuleBase());
  BD_CRITICAL("===================================================");
  rex::FlushLogging();

  ShowFatalError(
      "reblue Crashed",
      fmt::format("reblue hit a fatal error and has to close.\n\n{}", detail));
  std::abort();
}

#if !defined(_WIN32)
const char *SignalName(int sig) {
  switch (sig) {
  case SIGABRT:
    return "SIGABRT (abort, failed assert, or uncaught exception)";
  case SIGBUS:
    return "SIGBUS (misaligned or unmapped memory access)";
  case SIGFPE:
    return "SIGFPE (arithmetic fault)";
  default:
    return "fatal signal";
  }
}

// SIGSEGV/SIGILL stay with the SDK dispatcher, which needs first refusal on
// them to service guest MMIO. These three have no owner, so take them directly:
// without this the process vanishes with nothing in the log at all.
void FatalSignalHandler(int sig, siginfo_t *info, void * /*context*/) {
  if (s_reporting.test_and_set(std::memory_order_acq_rel))
    DieWithDefaultDisposition(sig);

  BD_CRITICAL("================ reblue host crash ================");
  BD_CRITICAL("build: {}", REXGLUE_BUILD_TITLE);
  BD_CRITICAL("signal: {} {}", sig, SignalName(sig));
  if (info && (sig == SIGBUS || sig == SIGFPE)) {
    BD_CRITICAL("fault address: {:#018x}",
                reinterpret_cast<u64>(info->si_addr));
  }
  LogBacktrace(HostModuleBase());
  BD_CRITICAL("===================================================");
  rex::FlushLogging();

  ShowFatalError("reblue Crashed",
                 fmt::format("reblue hit a fatal error and has to close.\n\n{}",
                             SignalName(sig)));
  DieWithDefaultDisposition(sig);
}
#endif

} // namespace

void InstallCrashStackForThread() {
#if !defined(_WIN32)
  // A stack overflow SIGSEGV has, by definition, no room left to run a handler
  // on the faulting stack. Deliberately leaked: the alt stack must outlive
  // every destructor the thread could still run.
  constexpr size_t kAltStackSize = 128 * 1024; // fmt formatting needs the room
  static thread_local void *s_alt_stack = nullptr;
  if (s_alt_stack)
    return;
  s_alt_stack = std::malloc(kAltStackSize);
  if (!s_alt_stack)
    return;

  stack_t ss{};
  ss.ss_sp = s_alt_stack;
  ss.ss_size = kAltStackSize;
  ss.ss_flags = 0;
  sigaltstack(&ss, nullptr);
#endif
}

void InstallTerminateHandler() {
  if (g_host_module_name.empty())
    g_host_module_name =
        rex::filesystem::GetExecutablePath().filename().string();

  std::set_terminate(&TerminateHandler);

#if !defined(_WIN32)
  InstallCrashStackForThread();

  struct sigaction sa{};
  sa.sa_sigaction = &FatalSignalHandler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  for (const int sig : {SIGABRT, SIGBUS, SIGFPE})
    sigaction(sig, &sa, nullptr);
#endif
}

void InstallCrashHandler() {
  rex::arch::ExceptionHandler::Install(&CrashHandler, nullptr);
  InstallTerminateHandler();

#if !defined(_WIN32)
  // The SDK installs SIGSEGV/SIGILL without SA_ONSTACK, so a stack overflow
  // fault re-faults inside its own handler and dies silently. Keep its
  // dispatcher, add the flag.
  for (const int sig : {SIGSEGV, SIGILL}) {
    struct sigaction current{};
    if (sigaction(sig, nullptr, &current) == 0 &&
        !(current.sa_flags & SA_ONSTACK)) {
      current.sa_flags |= SA_ONSTACK;
      sigaction(sig, &current, nullptr);
    }
  }
#endif

  BD_DEBUG("[crash] last-chance handler installed");
}

} // namespace bd::platform
