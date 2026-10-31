/**
 * @file    platform/ps5_libc.cpp
 * @brief   libc functions the PS5's modules do not export to a title and the
 *          PS5 platform layer does not supply, but that linked code names
 *          (from the Lost Odyssey PS5 port's os/ps5_libc.cpp). ps5/link-title.sh
 *          keeps these names local.
 * @license BSD 3-Clause License
 */
#include <rex/platform.h>

#if REX_PLATFORM_PS5

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <unistd.h>

extern "C" {

int ffs(int value) {
  return __builtin_ffs(value);
}

// FFmpeg's avpriv_tempfile without mkstemp; the XMA decoder never asks for a
// temporary file, and a failure here is reported, not fatal.
char *tempnam(const char *, const char *) {
  errno = ENOSYS;
  return nullptr;
}

// Only libkernel_sys exports pathconf, which a title does not load (its import
// would be null). libc++'s filesystem asks it for name and path lengths; the
// console's filesystems take FreeBSD's limits.
// The SDK's STFS/SVOD reader (Xbox content packages) converts their UTC
// timestamps with it.
time_t timegm(struct tm *tm) {
  // Normalise the month into the year, then count days from 1970-01-01
  // (Howard Hinnant's days_from_civil).
  long long year = 1900LL + tm->tm_year + tm->tm_mon / 12;
  int month = tm->tm_mon % 12;
  if (month < 0) {
    month += 12;
    --year;
  }
  const unsigned m = unsigned(month) + 1;
  year -= m <= 2;
  const long long era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = unsigned(year - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long long days = era * 146097 + doe - 719468 + (tm->tm_mday - 1);
  return time_t(days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec);
}

// re:Blue's crash handler names its backtrace frames. The console has no
// symbol table to offer, so each frame is its address (the log's base address
// turns them into offsets). One malloc'd block, as glibc's: free() it whole.
char **backtrace_symbols(void *const *frames, int count) {
  if (count <= 0)
    return nullptr;
  constexpr size_t kLine = 2 + 16 + 1;
  auto *block = static_cast<char *>(std::malloc(size_t(count) * (sizeof(char *) + kLine)));
  if (!block)
    return nullptr;
  auto **lines = reinterpret_cast<char **>(block);
  char *text = block + size_t(count) * sizeof(char *);
  for (int i = 0; i < count; ++i) {
    lines[i] = text + size_t(i) * kLine;
    std::snprintf(lines[i], kLine, "0x%016llx", (unsigned long long)uintptr_t(frames[i]));
  }
  return lines;
}

// A title cannot start processes or own a terminal. re:Blue's restart after a
// settings change (platform/reboot.cpp) and SDL's process API ask; both handle
// the failure. isatty only decides whether the log is coloured.
pid_t fork(void) {
  errno = ENOSYS;
  return -1;
}

pid_t setsid(void) {
  errno = EPERM;
  return -1;
}

int isatty(int) {
  errno = ENOTTY;
  return 0;
}

long pathconf(const char *, int name) {
  switch (name) {
  case _PC_NAME_MAX:
    return 255;
  case _PC_PATH_MAX:
    return 1024;
  case _PC_LINK_MAX:
    return 32767;
  default:
    errno = EINVAL;
    return -1;
  }
}
}

#endif // REX_PLATFORM_PS5
