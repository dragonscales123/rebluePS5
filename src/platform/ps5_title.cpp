/**
 * @file    platform/ps5_title.cpp
 * @brief   PlayStation 5 homebrew title setup (docs/PS5.md), from the Lost
 *          Odyssey PS5 port's os/ps5_title.cpp.
 *
 * Everything here runs from the first static constructor, before the SDK's
 * main: standard error to klog, the IEEE floating-point state other x86-64
 * systems start in (a title starts with flush-to-zero and denormals-are-zero,
 * and the platform's pthread_create copies the creator's state to each
 * thread), the working directory set to the title folder, Vulkan resolved
 * through the RADV linked into the title (volk, which plume uses), and the
 * ps5-env.txt switches (a title gets no environment).
 *
 * /app0/boot.txt records each step and any early crash (signal, rip, stack)
 * before the runtime's own log exists.
 *
 * @license BSD 3-Clause License
 */
#include <rex/platform.h>

#if REX_PLATFORM_PS5

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <typeinfo>

#include <fcntl.h>
#include <unistd.h>

#include <ps5platform/fp.h>
#include <ps5platform/klog.h>

#include <volk.h>

extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name);
extern "C" int sceSystemServiceHideSplashScreen(void);
extern "C" int sceSystemServiceLoadExec(const char *path, const char *const *argv);
extern "C" int sceKernelUsleep(uint32_t microseconds);
extern "C" int sceKernelDebugOutText(int channel, const char *text);

namespace bd::ps5 {

// One line to /app0/boot.txt (opened afresh by the first line of a run,
// written through and closed each time, so it survives a crash) and to klog.
// Safe before main and before the runtime's log exists.
void Breadcrumb(const char *line) {
  static bool started = false;
  const int flags = O_WRONLY | O_CREAT | (started ? O_APPEND : O_TRUNC);
  started = true;
  const int fd = open("/app0/boot.txt", flags, 0666);
  if (fd >= 0) {
    (void)write(fd, line, std::strlen(line));
    (void)write(fd, "\n", 1);
    (void)close(fd);
  }
  char text[512];
  std::snprintf(text, sizeof(text), "[re:Blue] %s\n", line);
  (void)sceKernelDebugOutText(0, text);
}

[[noreturn]] void ExitTitle(int status) {
  std::fprintf(stderr, "exit status %d: asking the shell to close the title\n", status);
  std::fflush(nullptr);
  (void)sceSystemServiceLoadExec("exit", nullptr);
  for (;;)
    sceKernelUsleep(100000);
}

namespace {

void ReadEnvFile() {
  FILE *env = std::fopen("/app0/ps5-env.txt", "r");
  if (!env) {
    Breadcrumb("main: no ps5-env.txt");
    return;
  }
  char line[512];
  while (std::fgets(line, sizeof(line), env)) {
    line[std::strcspn(line, "\r\n")] = '\0';
    char *equals = std::strchr(line, '=');
    if (line[0] == '#' || !equals)
      continue;
    *equals = '\0';
    setenv(line, equals + 1, 1);
    char crumb[600];
    std::snprintf(crumb, sizeof(crumb), "main: ps5-env.txt %s=%s", line, equals + 1);
    Breadcrumb(crumb);
  }
  std::fclose(env);
}

void InitializeTitle() {
  Breadcrumb("title: started");
  (void)ps5_klog_capture_stderr("[re:Blue] ");
  Breadcrumb("title: stderr to klog");
  ps5_fp_ieee();
  {
    // A title may not chdir on the console (EPERM); every path the title
    // opens is absolute under /app0, so only a different failure is news.
    const int rc = chdir("/app0");
    if (rc != 0 && errno != EPERM) {
      char line[96];
      std::snprintf(line, sizeof(line), "title: chdir /app0 failed, errno=%d", errno);
      Breadcrumb(line);
    }
  }
  (void)sceSystemServiceHideSplashScreen();
  Breadcrumb("title: splash hidden");
  // plume keeps a loader volk already has (ps5/patches/plume-ps5.patch).
  volkInitializeCustom(reinterpret_cast<PFN_vkGetInstanceProcAddr>(vk_icdGetInstanceProcAddr));
  Breadcrumb("title: Vulkan entry points from RADV");
  // re:Blue keeps everything beside the executable (portable layout): settings,
  // saves, logs, caches, game/. HOME points there too for anything that asks.
  setenv("HOME", "/app0", 0);
  ReadEnvFile();
}

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGSYS, SIGABRT, SIGTRAP};

// Before the runtime installs its own handlers (guest MMIO and SEH), and as
// the fallback after them: where the title died, in boot.txt.
void EarlyCrash(int signal, siginfo_t *info, void *context) {
  const auto *uc = static_cast<const ucontext_t *>(context);
  const uintptr_t anchor = reinterpret_cast<uintptr_t>(&Breadcrumb);
  char line[256];
  std::snprintf(line, sizeof(line),
                "CRASH signal=%d code=%d addr=%p rip=0x%lx rsp=0x%lx anchor(Breadcrumb)=0x%lx", signal,
                info ? info->si_code : 0, info ? info->si_addr : nullptr,
                (unsigned long)uc->uc_mcontext.mc_rip, (unsigned long)uc->uc_mcontext.mc_rsp,
                (unsigned long)anchor);
  Breadcrumb(line);
  const auto *stack = reinterpret_cast<const uintptr_t *>(uc->uc_mcontext.mc_rsp);
  int found = 0;
  for (int i = 0; i < 512 && found < 24; ++i) {
    const uintptr_t value = stack[i];
    if (value >= 0x400000 && value < 0x10400000) {
      std::snprintf(line, sizeof(line), "  stack[%d]=0x%lx", i, (unsigned long)value);
      Breadcrumb(line);
      ++found;
    }
  }
  struct sigaction fallback{};
  fallback.sa_handler = SIG_DFL;
  sigaction(signal, &fallback, nullptr);
}

// An uncaught exception ends in abort(); name it first.
[[noreturn]] void Terminate() {
  char line[400];
  const char *what = "no exception";
  const char *type = "";
  if (auto current = std::current_exception()) {
    try {
      std::rethrow_exception(current);
    } catch (const std::exception &e) {
      what = e.what();
      type = typeid(e).name();
    } catch (...) {
      what = "non-std exception";
    }
  }
  std::snprintf(line, sizeof(line), "TERMINATE: uncaught %s: %s", type, what);
  Breadcrumb(line);
  std::abort();
}

} // namespace
} // namespace bd::ps5

__attribute__((constructor(101))) static void ReblueFirstConstructor() {
  bd::ps5::Breadcrumb("title loaded: static constructors running");
  std::set_terminate(bd::ps5::Terminate);
  struct sigaction action{};
  action.sa_sigaction = bd::ps5::EarlyCrash;
  action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&action.sa_mask);
  for (int signal : bd::ps5::kFatalSignals)
    sigaction(signal, &action, nullptr);
  bd::ps5::InitializeTitle();
}

// The CRT (PS5_Vulkan's app_crt.cpp) calls this after main returns.
extern "C" void catchReturnFromMain(int status) {
  bd::ps5::ExitTitle(status);
}

// ps5/link-title.sh links with --wrap=exit --wrap=_exit --wrap=_Exit, so a quit
// from deep inside the runtime (or SDL, or a library) takes the same way out.
extern "C" [[noreturn]] void __wrap_exit(int status) {
  bd::ps5::ExitTitle(status);
}
extern "C" [[noreturn]] void __wrap__exit(int status) {
  bd::ps5::ExitTitle(status);
}
extern "C" [[noreturn]] void __wrap__Exit(int status) {
  bd::ps5::ExitTitle(status);
}

#endif // REX_PLATFORM_PS5
