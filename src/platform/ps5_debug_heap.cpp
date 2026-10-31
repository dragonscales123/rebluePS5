/**
 * @file    platform/ps5_debug_heap.cpp
 * @brief   A checking allocator for the PS5 title, built only with
 *          -DREBLUE_PS5_DEBUG_HEAP=ON (ps5/build.sh: REBLUE_PS5_DEBUG_HEAP=ON).
 *
 * The title's malloc is the PS5 platform layer's dlmalloc in direct memory,
 * reached through the linker's --wrap=malloc and the rest. That heap aborts
 * when it trips over a damaged block, long after whatever damaged it. This
 * file defines every __wrap_ the platform's heap_wrap.o does, so that member
 * is not linked, and puts a checked block around each allocation instead:
 *
 *   [ header: magic, size, sequence, allocating stack, list links ][ fence ]
 *   [ the caller's bytes ][ guard ]
 *
 * - a checker thread walks every live block five times a second and reports
 *   the first damaged one (overrun past its end, underrun into its fence,
 *   smashed header);
 * - each block also keeps the platform dlmalloc's own words around it as they
 *   were when it was allocated (its chunk header just before, and the footer
 *   just after: dlmalloc's FOOTERS, both fixed while the chunk is in use), so a
 *   write into the gaps between blocks, which the guards do not cover, shows
 *   up too;
 * - abort() is defined here, so when dlmalloc (or anything) aborts, every
 *   block is checked first and the damaged ones are named;
 * - free checks the block, fills it with 0xDD and holds it in a quarantine
 *   for a while; a block written to after its free is reported when it
 *   leaves the quarantine;
 * - reports go to /app0/boot.txt and klog, with the allocating code as
 *   "[ n] 0x..." lines that ps5-work/symbolize.sh reads: frame 0 is the
 *   direct caller of malloc or of operator new (both are replaced here, so a
 *   C++ allocation names its own call site without unwinding); with
 *   BD_DEBUG_HEAP_STACKS=1 in ps5-env.txt, the frames after it are the full
 *   stack from the console's unwinder (off by default: it cannot be called
 *   from inside malloc while the title starts).
 *
 * Blocks still come from the platform heap (ps5_heap_memalign), so memory is
 * as before plus 160 bytes a block and the quarantine. Pointers libc made
 * (strdup, realpath, ...) stay libc's, as with the platform's wraps.
 *
 * @license BSD 3-Clause License
 */
#include <rex/platform.h>

#if REX_PLATFORM_PS5 && defined(REBLUE_PS5_DEBUG_HEAP)

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <new>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <ps5platform/heap.h>

extern "C" {
void *__real_malloc(size_t bytes);
void *__real_calloc(size_t count, size_t bytes);
void *__real_realloc(void *pointer, size_t bytes);
void __real_free(void *pointer);
int __real_posix_memalign(void **out, size_t alignment, size_t bytes);
__attribute__((weak)) size_t __real_malloc_usable_size(const void *pointer);
int ps5_backtrace(void **frames, int size);
extern int sceKernelUsleep(uint32_t microseconds);
}

namespace bd::ps5 {
void Breadcrumb(const char *line);
}

namespace {

constexpr uint64_t kMagicLive = 0xB1DE'D0A9'0000'A11Cull;
constexpr uint64_t kMagicFreed = 0xB1DE'D0A9'0000'F7EEull;
constexpr int kFrames = 6;
constexpr size_t kFence = 16;  // bytes of 0xFB just before the caller's bytes
constexpr size_t kGuard = 32;  // bytes of 0xFD just after them
constexpr uint8_t kFenceByte = 0xFB;
constexpr uint8_t kGuardByte = 0xFD;
constexpr uint8_t kFreedByte = 0xDD;
constexpr size_t kFreedFillMax = size_t(1) << 20;  // fill (and check) at most 1 MiB
constexpr size_t kQuarantineCount = 16384;
constexpr size_t kQuarantineBytes = size_t(256) << 20;

struct Header {
  uint64_t magic;
  void *raw;  // what ps5_heap_memalign returned
  size_t size;
  uint64_t sequence;
  Header *prev;
  Header *next;
  void *frames[kFrames];
  // dlmalloc's words around the block when it was allocated: the chunk's head
  // (just before raw; its PINUSE bit follows the previous chunk, so it is
  // masked) and its footer (the next chunk's prev_foot). 0: not checked (a
  // chunk dlmalloc mapped on its own).
  size_t dl_head;
  size_t dl_foot;
  uint8_t fence[kFence];
};
static_assert(sizeof(Header) == 128, "the header keeps the caller's bytes 32-aligned");

// A spin lock: pthread mutexes on FreeBSD allocate on first use, which would
// call back into malloc.
std::atomic_flag g_lock = ATOMIC_FLAG_INIT;
struct Lock {
  Lock() {
    while (g_lock.test_and_set(std::memory_order_acquire)) {
    }
  }
  ~Lock() { g_lock.clear(std::memory_order_release); }
};

Header *g_live = nullptr;
uint64_t g_sequence = 0;
size_t g_live_count = 0;
std::atomic<bool> g_reported{false};
// Full stacks per allocation (BD_DEBUG_HEAP_STACKS=1), set once the title is
// up; until then, and by default, only the direct caller is recorded.
std::atomic<bool> g_stacks{false};

Header *g_quarantine[kQuarantineCount];
size_t g_quarantine_head = 0;  // next slot to fill
size_t g_quarantine_used = 0;
size_t g_quarantine_bytes = 0;

// One stack capture at a time, and none from inside one (the unwinder may
// allocate). Not thread_local: the PS5 toolchain builds with -femulated-tls,
// whose first touch of a thread_local on each thread calls malloc, which
// would come straight back here.
std::atomic<bool> g_unwinding{false};

inline Header *HeaderOf(const void *pointer) {
  return reinterpret_cast<Header *>(static_cast<uint8_t *>(const_cast<void *>(pointer)) -
                                    sizeof(Header));
}
inline uint8_t *Data(Header *h) { return reinterpret_cast<uint8_t *>(h + 1); }

void Say(const char *line) {
  bd::ps5::Breadcrumb(line);
  std::fprintf(stderr, "%s\n", line);
}

void SayFrames(const char *title, void *const *frames, int count) {
  char line[96];
  std::snprintf(line, sizeof(line), "  %s:", title);
  Say(line);
  for (int i = 0; i < count; ++i) {
    if (!frames[i])
      break;
    std::snprintf(line, sizeof(line), "    [%2d] 0x%016llx", i, (unsigned long long)uintptr_t(frames[i]));
    Say(line);
  }
}

// dlmalloc 2.8.6 (the platform's mspace.c): a chunk starts two words before
// the memory it returns; its head is the second word; with FOOTERS the next
// chunk's first word (prev_foot) is this chunk's footer.
constexpr size_t kDlFlagBits = 7;
inline size_t *DlHeadAt(void *raw) { return reinterpret_cast<size_t *>(raw) - 1; }
inline size_t *DlFootAt(void *raw, size_t head) {
  return reinterpret_cast<size_t *>(static_cast<uint8_t *>(raw) - 2 * sizeof(size_t) +
                                    (head & ~kDlFlagBits));
}
void RecordDl(Header *h) {
  const size_t head = *DlHeadAt(h->raw);
  // An mmapped chunk (no in-use bits) has no footer to watch.
  if ((head & 3) == 0 || (head & ~kDlFlagBits) < sizeof(Header) + h->size) {
    h->dl_head = h->dl_foot = 0;
    return;
  }
  h->dl_head = head;
  h->dl_foot = *DlFootAt(h->raw, head);
}
const char *CheckDl(Header *h, long long *offset, unsigned *value) {
  if (!h->dl_head)
    return nullptr;
  if ((*DlHeadAt(h->raw) & ~size_t(1)) != (h->dl_head & ~size_t(1))) {
    *offset = static_cast<long long>(reinterpret_cast<uint8_t *>(DlHeadAt(h->raw)) - Data(h));
    *value = unsigned(*DlHeadAt(h->raw) & 0xFF);
    return "the heap's chunk header before the block changed (a write into the gap before it)";
  }
  size_t *foot = DlFootAt(h->raw, h->dl_head);
  if (*foot != h->dl_foot) {
    *offset = static_cast<long long>(reinterpret_cast<uint8_t *>(foot) - Data(h));
    *value = unsigned(*foot & 0xFF);
    return "the heap's chunk footer after the block changed (a write into the gap after it)";
  }
  return nullptr;
}

// What is wrong with a live block, or nullptr. *offset is the first bad byte
// relative to the caller's pointer.
const char *CheckLive(Header *h, long long *offset, unsigned *value) {
  if (h->magic != kMagicLive) {
    *offset = -static_cast<long long>(sizeof(Header));
    *value = 0;
    return "header smashed (magic)";
  }
  for (size_t i = 0; i < kFence; ++i) {
    if (h->fence[i] != kFenceByte) {
      *offset = -static_cast<long long>(kFence - i);
      *value = h->fence[i];
      return "underrun into the fence before the block";
    }
  }
  const uint8_t *guard = Data(h) + h->size;
  for (size_t i = 0; i < kGuard; ++i) {
    if (guard[i] != kGuardByte) {
      *offset = static_cast<long long>(h->size + i);
      *value = guard[i];
      return "overrun past the end of the block";
    }
  }
  return CheckDl(h, offset, value);
}

void ReportOne(const char *what, Header *h, long long offset, unsigned value) {
  char line[256];
  Say("DEBUG HEAP: damaged block found");
  std::snprintf(line, sizeof(line),
                "  %s: block %p, %zu bytes, allocation #%llu, first bad byte at offset %lld = 0x%02x",
                what, static_cast<void *>(Data(h)), h->size, (unsigned long long)h->sequence, offset,
                value);
  Say(line);
  // The bytes around the damage, for a hint of what was written.
  if (offset >= 0) {
    const uint8_t *at = Data(h) + offset;
    char hex[3 * 32 + 1] = {};
    for (int i = 0; i < 32; ++i)
      std::snprintf(hex + 3 * i, 4, "%02x ", at[i]);
    std::snprintf(line, sizeof(line), "  bytes from there: %s", hex);
    Say(line);
  }
  SayFrames("allocated at (frame 0: the direct caller of malloc or operator new)", h->frames, kFrames);
}

[[noreturn]] void Report(const char *what, Header *h, long long offset, unsigned value,
                         bool with_current_stack) {
  if (g_reported.exchange(true)) {
    for (;;)
      sceKernelUsleep(1000000);
  }
  // Reports can come from inside the allocator's lock; what follows may
  // allocate, and the title stops afterwards anyway.
  g_lock.clear(std::memory_order_release);
  ReportOne(what, h, offset, value);
  if (with_current_stack && g_stacks.load()) {
    void *frames[16] = {};
    const int n = ps5_backtrace(frames, 16);
    SayFrames("found by (this stack)", frames, n);
  }
  Say("DEBUG HEAP: stopping the title (ps5-work/symbolize.sh boot.txt names the frames)");
  std::fflush(nullptr);
  __builtin_trap();
}

void Unlink(Header *h) {
  if (h->prev)
    h->prev->next = h->next;
  else
    g_live = h->next;
  if (h->next)
    h->next->prev = h->prev;
  h->prev = h->next = nullptr;
  --g_live_count;
}

// Checks a block leaving the quarantine: nothing may have written to it.
void CheckFreed(Header *h) {
  const size_t filled = h->size < kFreedFillMax ? h->size : kFreedFillMax;
  const uint8_t *data = Data(h);
  for (size_t i = 0; i < filled; ++i) {
    if (data[i] != kFreedByte)
      Report("written after it was freed", h, (long long)i, data[i], false);
  }
  const uint8_t *guard = data + h->size;
  for (size_t i = 0; i < kGuard; ++i) {
    if (guard[i] != kGuardByte)
      Report("overrun past the end (found after its free)", h, (long long)(h->size + i), guard[i], false);
  }
  long long offset;
  unsigned value;
  if (const char *what = CheckDl(h, &offset, &value))
    Report(what, h, offset, value, false);
}

__attribute__((noinline)) void *Allocate(size_t bytes, size_t alignment, void *caller) {
  if (alignment < 32)
    alignment = 32;
  if (bytes > SIZE_MAX / 2) {
    errno = ENOMEM;
    return nullptr;
  }
  // The header sits right before the caller's bytes, so an alignment above
  // the header's size needs room to slide.
  const size_t slack = alignment > sizeof(Header) ? alignment : 0;
  const size_t total = sizeof(Header) + slack + bytes + kGuard;
  void *raw = ps5_heap_memalign(alignment, total);
  if (!raw)
    return nullptr;
  uintptr_t data = reinterpret_cast<uintptr_t>(raw) + sizeof(Header);
  data = (data + alignment - 1) & ~uintptr_t(alignment - 1);
  Header *h = reinterpret_cast<Header *>(data - sizeof(Header));
  h->raw = raw;
  h->size = bytes;
  std::memset(h->frames, 0, sizeof(h->frames));
  h->frames[0] = caller;
  if (g_stacks.load(std::memory_order_relaxed) &&
      !g_unwinding.exchange(true, std::memory_order_acquire)) {
    void *frames[kFrames + 2] = {};
    const int n = ps5_backtrace(frames, kFrames + 2);
    // Skip this function and the wrap that called it; frame 0 stays the
    // direct caller.
    for (int i = 3; i < n && i - 2 < kFrames; ++i)
      h->frames[i - 2] = frames[i];
    g_unwinding.store(false, std::memory_order_release);
  }
  std::memset(h->fence, kFenceByte, kFence);
  std::memset(Data(h) + bytes, kGuardByte, kGuard);
  RecordDl(h);
  {
    Lock lock;
    h->magic = kMagicLive;
    h->sequence = ++g_sequence;
    h->prev = nullptr;
    h->next = g_live;
    if (g_live)
      g_live->prev = h;
    g_live = h;
    ++g_live_count;
  }
  return Data(h);
}

void Release(void *pointer) {
  Header *h = HeaderOf(pointer);
  Header *evict = nullptr;
  {
    Lock lock;
    long long offset;
    unsigned value;
    if (h->magic == kMagicFreed)
      Report("freed twice", h, 0, 0, true);
    if (const char *what = CheckLive(h, &offset, &value))
      Report(what, h, offset, value, true);
    Unlink(h);
    h->magic = kMagicFreed;
    std::memset(Data(h), kFreedByte, h->size < kFreedFillMax ? h->size : kFreedFillMax);
    // Into the quarantine; the oldest block leaves when it is full.
    if (g_quarantine_used == kQuarantineCount || g_quarantine_bytes > kQuarantineBytes) {
      const size_t oldest = (g_quarantine_head + kQuarantineCount - g_quarantine_used) % kQuarantineCount;
      evict = g_quarantine[oldest];
      g_quarantine[oldest] = nullptr;
      --g_quarantine_used;
      g_quarantine_bytes -= evict->size;
    }
    g_quarantine[g_quarantine_head] = h;
    g_quarantine_head = (g_quarantine_head + 1) % kQuarantineCount;
    ++g_quarantine_used;
    g_quarantine_bytes += h->size;
    if (evict)
      CheckFreed(evict);
  }
  if (evict)
    ps5_heap_free(evict->raw);
}

// Ours: a pointer the platform heap owns. (A libc fallback pointer is not.)
inline bool Ours(const void *pointer) { return ps5_heap_owns(pointer); }

void *CheckerThread(void *) {
  for (;;) {
    sceKernelUsleep(200000);
    Lock lock;
    long long offset;
    unsigned value;
    for (Header *h = g_live; h; h = h->next) {
      if (const char *what = CheckLive(h, &offset, &value))
        Report(what, h, offset, value, false);
    }
    for (size_t i = 0; i < kQuarantineCount; ++i) {
      Header *h = g_quarantine[i];
      if (!h)
        continue;
      const uint8_t *guard = Data(h) + h->size;
      for (size_t g = 0; g < kGuard; ++g) {
        if (guard[g] != kGuardByte)
          Report("overrun past the end (of a freed block)", h, (long long)(h->size + g), guard[g], false);
      }
      if (const char *what = CheckDl(h, &offset, &value))
        Report(what, h, offset, value, false);
    }
  }
  return nullptr;
}

// Every block, now, without waiting for the lock (the caller is about to stop
// the title): up to eight damaged ones are reported. Returns how many.
int ScanAll() {
  int found = 0;
  long long offset;
  unsigned value;
  for (Header *h = g_live; h && found < 8; h = h->next) {
    if (const char *what = CheckLive(h, &offset, &value)) {
      ReportOne(what, h, offset, value);
      ++found;
    }
  }
  for (size_t i = 0; i < kQuarantineCount && found < 8; ++i) {
    Header *h = g_quarantine[i];
    if (!h)
      continue;
    const char *what = CheckDl(h, &offset, &value);
    const uint8_t *guard = Data(h) + h->size;
    for (size_t g = 0; !what && g < kGuard; ++g) {
      if (guard[g] != kGuardByte) {
        what = "overrun past the end (of a freed block)";
        offset = (long long)(h->size + g);
        value = guard[g];
      }
    }
    const size_t filled = h->size < 4096 ? h->size : 4096;
    for (size_t b = 0; !what && b < filled; ++b) {
      if (Data(h)[b] != kFreedByte) {
        what = "written after it was freed";
        offset = (long long)b;
        value = Data(h)[b];
      }
    }
    if (what) {
      ReportOne(what, h, offset, value);
      ++found;
    }
  }
  return found;
}

__attribute__((constructor(102))) void StartChecker() {
  pthread_t thread;
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&thread, &attributes, CheckerThread, nullptr) == 0)
    Say("DEBUG HEAP: on (guards, quarantine, checker every 200 ms)");
  else
    Say("DEBUG HEAP: on, but the checker thread did not start");
  pthread_attr_destroy(&attributes);
  // ps5-env.txt has been read by now (platform/ps5_title.cpp, constructor 101).
  if (const char *stacks = std::getenv("BD_DEBUG_HEAP_STACKS"); stacks && stacks[0] == '1') {
    g_stacks = true;
    Say("DEBUG HEAP: full allocation stacks on (BD_DEBUG_HEAP_STACKS=1)");
  }
}

} // namespace

extern "C" {

void *__wrap_malloc(size_t bytes) {
  if (void *p = Allocate(bytes, 32, __builtin_return_address(0)))
    return p;
  return __real_malloc(bytes);
}

void *__wrap_calloc(size_t count, size_t bytes) {
  if (bytes != 0 && count > SIZE_MAX / bytes) {
    errno = ENOMEM;
    return nullptr;
  }
  if (void *p = Allocate(count * bytes, 32, __builtin_return_address(0))) {
    std::memset(p, 0, count * bytes);
    return p;
  }
  return __real_calloc(count, bytes);
}

void __wrap_free(void *pointer) {
  if (!pointer)
    return;
  if (Ours(pointer))
    Release(pointer);
  else
    __real_free(pointer);
}

size_t __wrap_malloc_usable_size(const void *pointer) {
  if (!pointer)
    return 0;
  if (Ours(pointer))
    return HeaderOf(pointer)->size;
  return __real_malloc_usable_size ? __real_malloc_usable_size(pointer) : 0;
}

void *__wrap_realloc(void *pointer, size_t bytes) {
  if (!pointer)
    return __wrap_malloc(bytes);
  if (bytes == 0)
    bytes = 1;  // FreeBSD gives a zero-size request a minimum-size object
  if (!Ours(pointer))
    return __real_realloc(pointer, bytes);
  void *moved = Allocate(bytes, 32, __builtin_return_address(0));
  if (!moved)
    moved = __real_malloc(bytes);
  if (!moved)
    return nullptr;
  const size_t kept = HeaderOf(pointer)->size;
  std::memcpy(moved, pointer, kept < bytes ? kept : bytes);
  Release(pointer);
  return moved;
}

void *__wrap_reallocf(void *pointer, size_t bytes) {
  void *const moved = __wrap_realloc(pointer, bytes);
  if (!moved)
    __wrap_free(pointer);
  return moved;
}

void *__wrap_reallocarray(void *pointer, size_t count, size_t bytes) {
  if (bytes != 0 && count > SIZE_MAX / bytes) {
    errno = ENOMEM;
    return nullptr;
  }
  return __wrap_realloc(pointer, count * bytes);
}

int __wrap_posix_memalign(void **out, size_t alignment, size_t bytes) {
  if (alignment < sizeof(void *) || (alignment & (alignment - 1)) != 0)
    return EINVAL;
  if (void *p = Allocate(bytes, alignment, __builtin_return_address(0))) {
    *out = p;
    return 0;
  }
  return __real_posix_memalign(out, alignment, bytes);
}

void *__wrap_aligned_alloc(size_t alignment, size_t bytes) {
  void *pointer = nullptr;
  const int result =
      __wrap_posix_memalign(&pointer, alignment < sizeof(void *) ? sizeof(void *) : alignment, bytes);
  if (result != 0) {
    errno = result;
    return nullptr;
  }
  return pointer;
}

void *__wrap_memalign(size_t alignment, size_t bytes) {
  return __wrap_aligned_alloc(alignment, bytes);
}

// libc's getdelim grows the caller's buffer with libc's realloc, which must
// never see one of these blocks (as the platform's own wrap).
ssize_t __wrap_getdelim(char **line, size_t *capacity, int delimiter, FILE *stream) {
  if (!line || !capacity || !stream) {
    errno = EINVAL;
    return -1;
  }
  if (!*line)
    *capacity = 0;
  size_t length = 0;
  for (;;) {
    const int c = fgetc(stream);
    if (c == EOF) {
      if (length == 0 || ferror(stream))
        return -1;
      break;
    }
    if (length + 2 > *capacity) {
      size_t grown = *capacity < 64 ? 128 : *capacity * 2;
      if (grown > size_t(SSIZE_MAX)) {
        errno = EOVERFLOW;
        return -1;
      }
      char *const moved = static_cast<char *>(__wrap_realloc(*line, grown));
      if (!moved)
        return -1;
      *line = moved;
      *capacity = grown;
    }
    (*line)[length++] = char(c);
    if (c == (unsigned char)delimiter)
      break;
  }
  (*line)[length] = '\0';
  return ssize_t(length);
}

ssize_t __wrap_getline(char **line, size_t *capacity, FILE *stream) {
  return __wrap_getdelim(line, capacity, '\n', stream);
}

// dlmalloc, libc++ and the rest call abort() when they give up; the title's
// own definition takes their references, so a full check runs first.
void abort(void) {
  static std::atomic<bool> entered{false};
  if (!entered.exchange(true)) {
    char line[128];
    std::snprintf(line, sizeof(line), "DEBUG HEAP: abort() called from 0x%016llx; checking every block",
                  (unsigned long long)uintptr_t(__builtin_return_address(0)));
    Say(line);
    void *caller[1] = {__builtin_return_address(0)};
    SayFrames("abort() called by", caller, 1);
    const bool already = g_reported.exchange(true);
    const int found = already ? 0 : ScanAll();
    std::snprintf(line, sizeof(line), "DEBUG HEAP: %d damaged block(s) found at abort()", found);
    Say(line);
    std::fflush(nullptr);
    raise(SIGABRT);  // the runtime's crash handler logs its own backtrace
  }
  signal(SIGABRT, SIG_DFL);
  raise(SIGABRT);
  for (;;)
    sceKernelUsleep(1000000);
}

} // extern "C"

// C++ allocations name their own call site: operator new is replaced here (the
// standard lets a program do that; libc++'s are weak), so frame 0 of a report
// is the code that said new, not libc++.
namespace {
void *NewBlock(size_t bytes, size_t alignment, void *caller) {
  if (bytes == 0)
    bytes = 1;
  if (void *p = Allocate(bytes, alignment, caller))
    return p;
  void *fallback = nullptr;
  if (__real_posix_memalign(&fallback, alignment < sizeof(void *) ? sizeof(void *) : alignment, bytes) == 0)
    return fallback;
  return nullptr;
}
void DeleteBlock(void *pointer) {
  if (!pointer)
    return;
  if (Ours(pointer))
    Release(pointer);
  else
    __real_free(pointer);
}
} // namespace

void *operator new(size_t bytes) {
  if (void *p = NewBlock(bytes, 32, __builtin_return_address(0)))
    return p;
  throw std::bad_alloc();
}
void *operator new[](size_t bytes) {
  if (void *p = NewBlock(bytes, 32, __builtin_return_address(0)))
    return p;
  throw std::bad_alloc();
}
void *operator new(size_t bytes, const std::nothrow_t &) noexcept {
  return NewBlock(bytes, 32, __builtin_return_address(0));
}
void *operator new[](size_t bytes, const std::nothrow_t &) noexcept {
  return NewBlock(bytes, 32, __builtin_return_address(0));
}
void *operator new(size_t bytes, std::align_val_t alignment) {
  if (void *p = NewBlock(bytes, size_t(alignment), __builtin_return_address(0)))
    return p;
  throw std::bad_alloc();
}
void *operator new[](size_t bytes, std::align_val_t alignment) {
  if (void *p = NewBlock(bytes, size_t(alignment), __builtin_return_address(0)))
    return p;
  throw std::bad_alloc();
}
void *operator new(size_t bytes, std::align_val_t alignment, const std::nothrow_t &) noexcept {
  return NewBlock(bytes, size_t(alignment), __builtin_return_address(0));
}
void *operator new[](size_t bytes, std::align_val_t alignment, const std::nothrow_t &) noexcept {
  return NewBlock(bytes, size_t(alignment), __builtin_return_address(0));
}
void operator delete(void *p) noexcept { DeleteBlock(p); }
void operator delete[](void *p) noexcept { DeleteBlock(p); }
void operator delete(void *p, size_t) noexcept { DeleteBlock(p); }
void operator delete[](void *p, size_t) noexcept { DeleteBlock(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { DeleteBlock(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { DeleteBlock(p); }
void operator delete(void *p, std::align_val_t) noexcept { DeleteBlock(p); }
void operator delete[](void *p, std::align_val_t) noexcept { DeleteBlock(p); }
void operator delete(void *p, size_t, std::align_val_t) noexcept { DeleteBlock(p); }
void operator delete[](void *p, size_t, std::align_val_t) noexcept { DeleteBlock(p); }
void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept { DeleteBlock(p); }
void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept { DeleteBlock(p); }

#endif // REX_PLATFORM_PS5 && REBLUE_PS5_DEBUG_HEAP
