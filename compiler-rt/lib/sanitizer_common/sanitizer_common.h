//===-- sanitizer_common.h --------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is shared between run-time libraries of sanitizers.
//
// It declares common functions and classes that are used in both runtimes.
// Implementation of some functions are provided in sanitizer_common, while
// others must be defined by run-time library itself.
//===----------------------------------------------------------------------===//
#ifndef SANITIZER_COMMON_H
#define SANITIZER_COMMON_H

#include "sanitizer_flags.h"
#include "sanitizer_interface_internal.h"
#include "sanitizer_internal_defs.h"
#include "sanitizer_libc.h"
#include "sanitizer_list.h"
#include "sanitizer_mutex.h"

#if defined(_MSC_VER) && !defined(__clang__)
extern "C" void _ReadWriteBarrier();
#pragma intrinsic(_ReadWriteBarrier)
#endif

namespace __sanitizer {
struct StackTrace;
struct AddressInfo;

// Constants.
const uptr kWordSize = SANITIZER_WORDSIZE / 8;
const uptr kWordSizeInBits = 8 * kWordSize;

#if defined(__powerpc__) || defined(__powerpc64__)
  const uptr kCacheLineSize = 128;
#else
  const uptr kCacheLineSize = 64;
#endif

const uptr kMaxPathLength = 4096;

const uptr kMaxThreadStackSize = 1 << 30;  // 1Gb

static const uptr kErrorMessageBufferSize = 1 << 16;

// Denotes fake PC values that come from JIT/JAVA/etc.
// For such PC values __tsan_symbolize_external() will be called.
const u64 kExternalPCBit = 1ULL << 60;

extern const char *SanitizerToolName;  // Can be changed by the tool.

extern atomic_uint32_t current_verbosity;
INLINE void SetVerbosity(int verbosity) {
  atomic_store(&current_verbosity, verbosity, memory_order_relaxed);
}
INLINE int Verbosity() {
  return atomic_load(&current_verbosity, memory_order_relaxed);
}

uptr GetPageSize();
extern uptr PageSizeCached;
INLINE uptr GetPageSizeCached() {
  if (!PageSizeCached)
    PageSizeCached = GetPageSize();
  return PageSizeCached;
}
uptr GetMmapGranularity();
uptr GetMaxVirtualAddress();
// Threads
uptr GetTid();
uptr GetThreadSelf();
void GetThreadStackTopAndBottom(bool at_initialization, uptr *stack_top,
                                uptr *stack_bottom);
void GetThreadStackAndTls(bool main, uptr *stk_addr, uptr *stk_size,
                          uptr *tls_addr, uptr *tls_size);

// Memory management
void *MmapOrDie(uptr size, const char *mem_type, bool raw_report = false);
void *CustomMmapOrDie(uptr size, const char *mem_type, bool raw_report, int reg_num, int va_bits);
void *InitialMmapOrDie(uptr size, const char *mem_type, bool raw_report, int va_bits);
INLINE void *MmapOrDieQuietly(uptr size, const char *mem_type) {
  return MmapOrDie(size, mem_type, /*raw_report*/ true);
}
void UnmapOrDie(void *addr, uptr size);
void *MmapFixedNoReserve(uptr fixed_addr, uptr size,
                         const char *name = nullptr);
void *MmapNoReserveOrDie(uptr size, const char *mem_type);
void *MmapFixedOrDie(uptr fixed_addr, uptr size);
void *MmapFixedNoAccess(uptr fixed_addr, uptr size, const char *name = nullptr);
void *MmapNoAccess(uptr size);
// Map aligned chunk of address space; size and alignment are powers of two.
void *MmapAlignedOrDie(uptr size, uptr alignment, const char *mem_type);
// Disallow access to a memory range.  Use MmapFixedNoAccess to allocate an
// unaccessible memory.
bool MprotectNoAccess(uptr addr, uptr size);
bool MprotectReadOnly(uptr addr, uptr size);

// Find an available address space.
uptr FindAvailableMemoryRange(uptr size, uptr alignment, uptr left_padding);

// Used to check if we can map shadow memory to a fixed location.
bool MemoryRangeIsAvailable(uptr range_start, uptr range_end);
// Releases memory pages entirely within the [beg, end] address range. Noop if
// the provided range does not contain at least one entire page.
void ReleaseMemoryPagesToOS(uptr beg, uptr end);
void IncreaseTotalMmap(uptr size);
void DecreaseTotalMmap(uptr size);
uptr GetRSS();
void NoHugePagesInRegion(uptr addr, uptr length);
void DontDumpShadowMemory(uptr addr, uptr length);
// Check if the built VMA size matches the runtime one.
void CheckVMASize();
void RunMallocHooks(const void *ptr, uptr size);
void RunFreeHooks(const void *ptr);

// InternalScopedBuffer can be used instead of large stack arrays to
// keep frame size low.
// FIXME: use InternalAlloc instead of MmapOrDie once
// InternalAlloc is made libc-free.
template <typename T>
class InternalScopedBuffer {
 public:
  explicit InternalScopedBuffer(uptr cnt) {
    cnt_ = cnt;
    ptr_ = (T *)MmapOrDie(cnt * sizeof(T), "InternalScopedBuffer");
  }
  ~InternalScopedBuffer() { UnmapOrDie(ptr_, cnt_ * sizeof(T)); }
  T &operator[](uptr i) { return ptr_[i]; }
  T *data() { return ptr_; }
  uptr size() { return cnt_ * sizeof(T); }

 private:
  T *ptr_;
  uptr cnt_;
  // Disallow copies and moves.
  InternalScopedBuffer(const InternalScopedBuffer &) = delete;
  InternalScopedBuffer &operator=(const InternalScopedBuffer &) = delete;
  InternalScopedBuffer(InternalScopedBuffer &&) = delete;
  InternalScopedBuffer &operator=(InternalScopedBuffer &&) = delete;
};

class InternalScopedString : public InternalScopedBuffer<char> {
 public:
  explicit InternalScopedString(uptr max_length)
      : InternalScopedBuffer<char>(max_length), length_(0) {
    (*this)[0] = '\0';
  }
  uptr length() { return length_; }
  void clear() {
    (*this)[0] = '\0';
    length_ = 0;
  }
  void append(const char *format, ...);

 private:
  uptr length_;
};

// Simple low-level (mmap-based) allocator for internal use. Doesn't have
// constructor, so all instances of LowLevelAllocator should be
// linker initialized.
class LowLevelAllocator {
 public:
  // Requires an external lock.
  void *Allocate(uptr size);
 private:
  char *allocated_end_;
  char *allocated_current_;
};
typedef void (*LowLevelAllocateCallback)(uptr ptr, uptr size);
// Allows to register tool-specific callbacks for LowLevelAllocator.
// Passing NULL removes the callback.
void SetLowLevelAllocateCallback(LowLevelAllocateCallback callback);

// IO
void RawWrite(const char *buffer);
bool ColorizeReports();
void RemoveANSIEscapeSequencesFromString(char *buffer);
void Printf(const char *format, ...);
void Report(const char *format, ...);
void SetPrintfAndReportCallback(void (*callback)(const char *));
#define VReport(level, ...)                                              \
  do {                                                                   \
    if ((uptr)Verbosity() >= (level)) Report(__VA_ARGS__); \
  } while (0)
#define VPrintf(level, ...)                                              \
  do {                                                                   \
    if ((uptr)Verbosity() >= (level)) Printf(__VA_ARGS__); \
  } while (0)

// Can be used to prevent mixing error reports from different sanitizers.
extern StaticSpinMutex CommonSanitizerReportMutex;

struct ReportFile {
  void Write(const char *buffer, uptr length);
  bool SupportsColors();
  void SetReportPath(const char *path);

  // Don't use fields directly. They are only declared public to allow
  // aggregate initialization.

  // Protects fields below.
  StaticSpinMutex *mu;
  // Opened file descriptor. Defaults to stderr. It may be equal to
  // kInvalidFd, in which case new file will be opened when necessary.
  fd_t fd;
  // Path prefix of report file, set via __sanitizer_set_report_path.
  char path_prefix[kMaxPathLength];
  // Full path to report, obtained as <path_prefix>.PID
  char full_path[kMaxPathLength];
  // PID of the process that opened fd. If a fork() occurs,
  // the PID of child will be different from fd_pid.
  uptr fd_pid;

 private:
  void ReopenIfNecessary();
};
extern ReportFile report_file;

extern uptr stoptheworld_tracer_pid;
extern uptr stoptheworld_tracer_ppid;

enum FileAccessMode {
  RdOnly,
  WrOnly,
  RdWr
};

// Returns kInvalidFd on error.
fd_t OpenFile(const char *filename, FileAccessMode mode,
              error_t *errno_p = nullptr);
void CloseFile(fd_t);

// Return true on success, false on error.
bool ReadFromFile(fd_t fd, void *buff, uptr buff_size,
                  uptr *bytes_read = nullptr, error_t *error_p = nullptr);
bool WriteToFile(fd_t fd, const void *buff, uptr buff_size,
                 uptr *bytes_written = nullptr, error_t *error_p = nullptr);

bool RenameFile(const char *oldpath, const char *newpath,
                error_t *error_p = nullptr);

// Scoped file handle closer.
struct FileCloser {
  explicit FileCloser(fd_t fd) : fd(fd) {}
  ~FileCloser() { CloseFile(fd); }
  fd_t fd;
};

bool SupportsColoredOutput(fd_t fd);

// Opens the file 'file_name" and reads up to 'max_len' bytes.
// The resulting buffer is mmaped and stored in '*buff'.
// The size of the mmaped region is stored in '*buff_size'.
// The total number of read bytes is stored in '*read_len'.
// Returns true if file was successfully opened and read.
bool ReadFileToBuffer(const char *file_name, char **buff, uptr *buff_size,
                      uptr *read_len, uptr max_len = 1 << 26,
                      error_t *errno_p = nullptr);
// Maps given file to virtual memory, and returns pointer to it
// (or NULL if mapping fails). Stores the size of mmaped region
// in '*buff_size'.
void *MapFileToMemory(const char *file_name, uptr *buff_size);
void *MapWritableFileToMemory(void *addr, uptr size, fd_t fd, OFF_T offset);

bool IsAccessibleMemoryRange(uptr beg, uptr size);

// Error report formatting.
const char *StripPathPrefix(const char *filepath,
                            const char *strip_file_prefix);
// Strip the directories from the module name.
const char *StripModuleName(const char *module);

// OS
uptr ReadBinaryName(/*out*/char *buf, uptr buf_len);
uptr ReadBinaryNameCached(/*out*/char *buf, uptr buf_len);
uptr ReadLongProcessName(/*out*/ char *buf, uptr buf_len);
const char *GetProcessName();
void UpdateProcessName();
void CacheBinaryName();
void DisableCoreDumperIfNecessary();
void DumpProcessMap();
void PrintModuleMap();
bool FileExists(const char *filename);
const char *GetEnv(const char *name);
bool SetEnv(const char *name, const char *value);
const char *GetPwd();
char *FindPathToBinary(const char *name);
bool IsPathSeparator(const char c);
bool IsAbsolutePath(const char *path);
// Starts a subprocess and returs its pid.
// If *_fd parameters are not kInvalidFd their corresponding input/output
// streams will be redirect to the file. The files will always be closed
// in parent process even in case of an error.
// The child process will close all fds after STDERR_FILENO
// before passing control to a program.
pid_t StartSubprocess(const char *filename, const char *const argv[],
                      fd_t stdin_fd = kInvalidFd, fd_t stdout_fd = kInvalidFd,
                      fd_t stderr_fd = kInvalidFd);
// Checks if specified process is still running
bool IsProcessRunning(pid_t pid);
// Waits for the process to finish and returns its exit code.
// Returns -1 in case of an error.
int WaitForProcess(pid_t pid);

u32 GetUid();
void ReExec();
char **GetArgv();
void PrintCmdline();
bool StackSizeIsUnlimited();
uptr GetStackSizeLimitInBytes();
void SetStackSizeLimitInBytes(uptr limit);
bool AddressSpaceIsUnlimited();
void SetAddressSpaceUnlimited();
void AdjustStackSize(void *attr);
void PrepareForSandboxing(__sanitizer_sandbox_arguments *args);
void CovPrepareForSandboxing(__sanitizer_sandbox_arguments *args);
void SetSandboxingCallback(void (*f)());

void CoverageUpdateMapping();
void CovBeforeFork();
void CovAfterFork(int child_pid);

void InitializeCoverage(bool enabled, const char *coverage_dir);
void ReInitializeCoverage(bool enabled, const char *coverage_dir);

void InitTlsSize();
uptr GetTlsSize();

// Other
void SleepForSeconds(int seconds);
void SleepForMillis(int millis);
u64 NanoTime();
int Atexit(void (*function)(void));
void SortArray(uptr *array, uptr size);
void SortArray(u32 *array, uptr size);
bool TemplateMatch(const char *templ, const char *str);

// Exit
void NORETURN Abort();
void NORETURN Die();
void NORETURN
CheckFailed(const char *file, int line, const char *cond, u64 v1, u64 v2);
void NORETURN ReportMmapFailureAndDie(uptr size, const char *mem_type,
                                      const char *mmap_type, error_t err,
                                      bool raw_report = false);

// Set the name of the current thread to 'name', return true on succees.
// The name may be truncated to a system-dependent limit.
bool SanitizerSetThreadName(const char *name);
// Get the name of the current thread (no more than max_len bytes),
// return true on succees. name should have space for at least max_len+1 bytes.
bool SanitizerGetThreadName(char *name, int max_len);

// Specific tools may override behavior of "Die" and "CheckFailed" functions
// to do tool-specific job.
typedef void (*DieCallbackType)(void);

// It's possible to add several callbacks that would be run when "Die" is
// called. The callbacks will be run in the opposite order. The tools are
// strongly recommended to setup all callbacks during initialization, when there
// is only a single thread.
bool AddDieCallback(DieCallbackType callback);
bool RemoveDieCallback(DieCallbackType callback);

void SetUserDieCallback(DieCallbackType callback);

typedef void (*CheckFailedCallbackType)(const char *, int, const char *,
                                       u64, u64);
void SetCheckFailedCallback(CheckFailedCallbackType callback);

// Callback will be called if soft_rss_limit_mb is given and the limit is
// exceeded (exceeded==true) or if rss went down below the limit
// (exceeded==false).
// The callback should be registered once at the tool init time.
void SetSoftRssLimitExceededCallback(void (*Callback)(bool exceeded));

// Functions related to signal handling.
typedef void (*SignalHandlerType)(int, void *, void *);
bool IsHandledDeadlySignal(int signum);
void InstallDeadlySignalHandlers(SignalHandlerType handler);
// Alternative signal stack (POSIX-only).
void SetAlternateSignalStack();
void UnsetAlternateSignalStack();

// We don't want a summary too long.
const int kMaxSummaryLength = 1024;
// Construct a one-line string:
//   SUMMARY: SanitizerToolName: error_message
// and pass it to __sanitizer_report_error_summary.
void ReportErrorSummary(const char *error_message);
// Same as above, but construct error_message as:
//   error_type file:line[:column][ function]
void ReportErrorSummary(const char *error_type, const AddressInfo &info);
// Same as above, but obtains AddressInfo by symbolizing top stack trace frame.
void ReportErrorSummary(const char *error_type, const StackTrace *trace);

// Math
#if SANITIZER_WINDOWS && !defined(__clang__) && !defined(__GNUC__)
extern "C" {
unsigned char _BitScanForward(unsigned long *index, unsigned long mask);  // NOLINT
unsigned char _BitScanReverse(unsigned long *index, unsigned long mask);  // NOLINT
#if defined(_WIN64)
unsigned char _BitScanForward64(unsigned long *index, unsigned __int64 mask);  // NOLINT
unsigned char _BitScanReverse64(unsigned long *index, unsigned __int64 mask);  // NOLINT
#endif
}
#endif

INLINE uptr MostSignificantSetBitIndex(uptr x) {
  CHECK_NE(x, 0U);
  unsigned long up;  // NOLINT
#if !SANITIZER_WINDOWS || defined(__clang__) || defined(__GNUC__)
# ifdef _WIN64
  up = SANITIZER_WORDSIZE - 1 - __builtin_clzll(x);
# else
  up = SANITIZER_WORDSIZE - 1 - __builtin_clzl(x);
# endif
#elif defined(_WIN64)
  _BitScanReverse64(&up, x);
#else
  _BitScanReverse(&up, x);
#endif
  return up;
}

INLINE uptr LeastSignificantSetBitIndex(uptr x) {
  CHECK_NE(x, 0U);
  unsigned long up;  // NOLINT
#if !SANITIZER_WINDOWS || defined(__clang__) || defined(__GNUC__)
# ifdef _WIN64
  up = __builtin_ctzll(x);
# else
  up = __builtin_ctzl(x);
# endif
#elif defined(_WIN64)
  _BitScanForward64(&up, x);
#else
  _BitScanForward(&up, x);
#endif
  return up;
}

INLINE bool IsPowerOfTwo(uptr x) {
  return (x & (x - 1)) == 0;
}

INLINE uptr RoundUpToPowerOfTwo(uptr size) {
  CHECK(size);
  if (IsPowerOfTwo(size)) return size;

  uptr up = MostSignificantSetBitIndex(size);
  CHECK_LT(size, (1ULL << (up + 1)));
  CHECK_GT(size, (1ULL << up));
  return 1ULL << (up + 1);
}

INLINE uptr RoundUpTo(uptr size, uptr boundary) {
  RAW_CHECK(IsPowerOfTwo(boundary));
  return (size + boundary - 1) & ~(boundary - 1);
}

INLINE uptr RoundDownTo(uptr x, uptr boundary) {
  return x & ~(boundary - 1);
}

INLINE bool IsAligned(uptr a, uptr alignment) {
  return (a & (alignment - 1)) == 0;
}

INLINE uptr Log2(uptr x) {
  CHECK(IsPowerOfTwo(x));
  return LeastSignificantSetBitIndex(x);
}

// Don't use std::min, std::max or std::swap, to minimize dependency
// on libstdc++.
template<class T> T Min(T a, T b) { return a < b ? a : b; }
template<class T> T Max(T a, T b) { return a > b ? a : b; }
template<class T> void Swap(T& a, T& b) {
  T tmp = a;
  a = b;
  b = tmp;
}

// Char handling
INLINE bool IsSpace(int c) {
  return (c == ' ') || (c == '\n') || (c == '\t') ||
         (c == '\f') || (c == '\r') || (c == '\v');
}
INLINE bool IsDigit(int c) {
  return (c >= '0') && (c <= '9');
}
INLINE int ToLower(int c) {
  return (c >= 'A' && c <= 'Z') ? (c + 'a' - 'A') : c;
}

// A low-level vector based on mmap. May incur a significant memory overhead for
// small vectors.
// WARNING: The current implementation supports only POD types.
template<typename T>
class InternalMmapVectorNoCtor {
 public:
  void Initialize(uptr initial_capacity) {
    capacity_ = Max(initial_capacity, (uptr)1);
    size_ = 0;
    data_ = (T *)MmapOrDie(capacity_ * sizeof(T), "InternalMmapVectorNoCtor");
  }
  void Destroy() {
    UnmapOrDie(data_, capacity_ * sizeof(T));
  }
  T &operator[](uptr i) {
    CHECK_LT(i, size_);
    return data_[i];
  }
  const T &operator[](uptr i) const {
    CHECK_LT(i, size_);
    return data_[i];
  }
  void push_back(const T &element) {
    CHECK_LE(size_, capacity_);
    if (size_ == capacity_) {
      uptr new_capacity = RoundUpToPowerOfTwo(size_ + 1);
      Resize(new_capacity);
    }
    internal_memcpy(&data_[size_++], &element, sizeof(T));
  }
  T &back() {
    CHECK_GT(size_, 0);
    return data_[size_ - 1];
  }
  void pop_back() {
    CHECK_GT(size_, 0);
    size_--;
  }
  uptr size() const {
    return size_;
  }
  const T *data() const {
    return data_;
  }
  T *data() {
    return data_;
  }
  uptr capacity() const {
    return capacity_;
  }
  void resize(uptr new_size) {
    Resize(new_size);
    if (new_size > size_) {
      internal_memset(&data_[size_], 0, sizeof(T) * (new_size - size_));
    }
    size_ = new_size;
  }

  void clear() { size_ = 0; }
  bool empty() const { return size() == 0; }

  const T *begin() const {
    return data();
  }
  T *begin() {
    return data();
  }
  const T *end() const {
    return data() + size();
  }
  T *end() {
    return data() + size();
  }

 private:
  void Resize(uptr new_capacity) {
    CHECK_GT(new_capacity, 0);
    CHECK_LE(size_, new_capacity);
    T *new_data = (T *)MmapOrDie(new_capacity * sizeof(T),
                                 "InternalMmapVector");
    internal_memcpy(new_data, data_, size_ * sizeof(T));
    T *old_data = data_;
    data_ = new_data;
    UnmapOrDie(old_data, capacity_ * sizeof(T));
    capacity_ = new_capacity;
  }

  T *data_;
  uptr capacity_;
  uptr size_;
};

template<typename T>
class InternalMmapVector : public InternalMmapVectorNoCtor<T> {
 public:
  explicit InternalMmapVector(uptr initial_capacity) {
    InternalMmapVectorNoCtor<T>::Initialize(initial_capacity);
  }
  ~InternalMmapVector() { InternalMmapVectorNoCtor<T>::Destroy(); }
  // Disallow evil constructors.
  InternalMmapVector(const InternalMmapVector&);
  void operator=(const InternalMmapVector&);
};

// HeapSort for arrays and InternalMmapVector.
template<class Container, class Compare>
void InternalSort(Container *v, uptr size, Compare comp) {
  if (size < 2)
    return;
  // Stage 1: insert elements to the heap.
  for (uptr i = 1; i < size; i++) {
    uptr j, p;
    for (j = i; j > 0; j = p) {
      p = (j - 1) / 2;
      if (comp((*v)[p], (*v)[j]))
        Swap((*v)[j], (*v)[p]);
      else
        break;
    }
  }
  // Stage 2: swap largest element with the last one,
  // and sink the new top.
  for (uptr i = size - 1; i > 0; i--) {
    Swap((*v)[0], (*v)[i]);
    uptr j, max_ind;
    for (j = 0; j < i; j = max_ind) {
      uptr left = 2 * j + 1;
      uptr right = 2 * j + 2;
      max_ind = j;
      if (left < i && comp((*v)[max_ind], (*v)[left]))
        max_ind = left;
      if (right < i && comp((*v)[max_ind], (*v)[right]))
        max_ind = right;
      if (max_ind != j)
        Swap((*v)[j], (*v)[max_ind]);
      else
        break;
    }
  }
}

// Works like std::lower_bound: finds the first element that is not less
// than the val.
template <class Container, class Value, class Compare>
uptr InternalLowerBound(const Container &v, uptr first, uptr last,
                        const Value &val, Compare comp) {
  while (last > first) {
    uptr mid = (first + last) / 2;
    if (comp(v[mid], val))
      first = mid + 1;
    else
      last = mid;
  }
  return first;
}

enum ModuleArch {
  kModuleArchUnknown,
  kModuleArchI386,
  kModuleArchX86_64,
  kModuleArchX86_64H,
  kModuleArchARMV6,
  kModuleArchARMV7,
  kModuleArchARMV7S,
  kModuleArchARMV7K,
  kModuleArchARM64
};

// When adding a new architecture, don't forget to also update
// script/asan_symbolize.py and sanitizer_symbolizer_libcdep.cc.
inline const char *ModuleArchToString(ModuleArch arch) {
  switch (arch) {
    case kModuleArchUnknown:
      return "";
    case kModuleArchI386:
      return "i386";
    case kModuleArchX86_64:
      return "x86_64";
    case kModuleArchX86_64H:
      return "x86_64h";
    case kModuleArchARMV6:
      return "armv6";
    case kModuleArchARMV7:
      return "armv7";
    case kModuleArchARMV7S:
      return "armv7s";
    case kModuleArchARMV7K:
      return "armv7k";
    case kModuleArchARM64:
      return "arm64";
  }
  CHECK(0 && "Invalid module arch");
  return "";
}

const uptr kModuleUUIDSize = 16;

// Represents a binary loaded into virtual memory (e.g. this can be an
// executable or a shared object).
class LoadedModule {
 public:
  LoadedModule()
      : full_name_(nullptr),
        base_address_(0),
        max_executable_address_(0),
        arch_(kModuleArchUnknown),
        instrumented_(false) {
    internal_memset(uuid_, 0, kModuleUUIDSize);
    ranges_.clear();
  }
  void set(const char *module_name, uptr base_address);
  void set(const char *module_name, uptr base_address, ModuleArch arch,
           u8 uuid[kModuleUUIDSize], bool instrumented);
  void clear();
  void addAddressRange(uptr beg, uptr end, bool executable);
  bool containsAddress(uptr address) const;

  const char *full_name() const { return full_name_; }
  uptr base_address() const { return base_address_; }
  uptr max_executable_address() const { return max_executable_address_; }
  ModuleArch arch() const { return arch_; }
  const u8 *uuid() const { return uuid_; }
  bool instrumented() const { return instrumented_; }

  struct AddressRange {
    AddressRange *next;
    uptr beg;
    uptr end;
    bool executable;

    AddressRange(uptr beg, uptr end, bool executable)
        : next(nullptr), beg(beg), end(end), executable(executable) {}
  };

  const IntrusiveList<AddressRange> &ranges() const { return ranges_; }

 private:
  char *full_name_;  // Owned.
  uptr base_address_;
  uptr max_executable_address_;
  ModuleArch arch_;
  u8 uuid_[kModuleUUIDSize];
  bool instrumented_;
  IntrusiveList<AddressRange> ranges_;
};

// List of LoadedModules. OS-dependent implementation is responsible for
// filling this information.
class ListOfModules {
 public:
  ListOfModules() : modules_(kInitialCapacity) {}
  ~ListOfModules() { clear(); }
  void init();
  const LoadedModule *begin() const { return modules_.begin(); }
  LoadedModule *begin() { return modules_.begin(); }
  const LoadedModule *end() const { return modules_.end(); }
  LoadedModule *end() { return modules_.end(); }
  uptr size() const { return modules_.size(); }
  const LoadedModule &operator[](uptr i) const {
    CHECK_LT(i, modules_.size());
    return modules_[i];
  }

 private:
  void clear() {
    for (auto &module : modules_) module.clear();
    modules_.clear();
  }

  InternalMmapVector<LoadedModule> modules_;
  // We rarely have more than 16K loaded modules.
  static const uptr kInitialCapacity = 1 << 14;
};

// Callback type for iterating over a set of memory ranges.
typedef void (*RangeIteratorCallback)(uptr begin, uptr end, void *arg);

enum AndroidApiLevel {
  ANDROID_NOT_ANDROID = 0,
  ANDROID_KITKAT = 19,
  ANDROID_LOLLIPOP_MR1 = 22,
  ANDROID_POST_LOLLIPOP = 23
};

void WriteToSyslog(const char *buffer);

#if SANITIZER_MAC
void LogFullErrorReport(const char *buffer);
#else
INLINE void LogFullErrorReport(const char *buffer) {}
#endif

#if SANITIZER_LINUX || SANITIZER_MAC
void WriteOneLineToSyslog(const char *s);
void LogMessageOnPrintf(const char *str);
#else
INLINE void WriteOneLineToSyslog(const char *s) {}
INLINE void LogMessageOnPrintf(const char *str) {}
#endif

#if SANITIZER_LINUX
// Initialize Android logging. Any writes before this are silently lost.
void AndroidLogInit();
#else
INLINE void AndroidLogInit() {}
#endif

#if SANITIZER_ANDROID
void SanitizerInitializeUnwinder();
AndroidApiLevel AndroidGetApiLevel();
#else
INLINE void AndroidLogWrite(const char *buffer_unused) {}
INLINE void SanitizerInitializeUnwinder() {}
INLINE AndroidApiLevel AndroidGetApiLevel() { return ANDROID_NOT_ANDROID; }
#endif

INLINE uptr GetPthreadDestructorIterations() {
#if SANITIZER_ANDROID
  return (AndroidGetApiLevel() == ANDROID_LOLLIPOP_MR1) ? 8 : 4;
#elif SANITIZER_POSIX
  return 4;
#else
// Unused on Windows.
  return 0;
#endif
}

void *internal_start_thread(void(*func)(void*), void *arg);
void internal_join_thread(void *th);
void MaybeStartBackgroudThread();

// Make the compiler think that something is going on there.
// Use this inside a loop that looks like memset/memcpy/etc to prevent the
// compiler from recognising it and turning it into an actual call to
// memset/memcpy/etc.
static inline void SanitizerBreakOptimization(void *arg) {
#if defined(_MSC_VER) && !defined(__clang__)
  _ReadWriteBarrier();
#else
  __asm__ __volatile__("" : : "r" (arg) : "memory");
#endif
}

struct SignalContext {
  void *context;
  uptr addr;
  uptr pc;
  uptr sp;
  uptr bp;
  bool is_memory_access;

  enum WriteFlag { UNKNOWN, READ, WRITE } write_flag;

  SignalContext(void *context, uptr addr, uptr pc, uptr sp, uptr bp,
                bool is_memory_access, WriteFlag write_flag)
      : context(context),
        addr(addr),
        pc(pc),
        sp(sp),
        bp(bp),
        is_memory_access(is_memory_access),
        write_flag(write_flag) {}

  static void DumpAllRegisters(void *context);

  // Creates signal context in a platform-specific manner.
  static SignalContext Create(void *siginfo, void *context);

  // Returns true if the "context" indicates a memory write.
  static WriteFlag GetWriteFlag(void *context);
};

void GetPcSpBp(void *context, uptr *pc, uptr *sp, uptr *bp);

void MaybeReexec();

template <typename Fn>
class RunOnDestruction {
 public:
  explicit RunOnDestruction(Fn fn) : fn_(fn) {}
  ~RunOnDestruction() { fn_(); }

 private:
  Fn fn_;
};

// A simple scope guard. Usage:
// auto cleanup = at_scope_exit([]{ do_cleanup; });
template <typename Fn>
RunOnDestruction<Fn> at_scope_exit(Fn fn) {
  return RunOnDestruction<Fn>(fn);
}

// Linux on 64-bit s390 had a nasty bug that crashes the whole machine
// if a process uses virtual memory over 4TB (as many sanitizers like
// to do).  This function will abort the process if running on a kernel
// that looks vulnerable.
#if SANITIZER_LINUX && SANITIZER_S390_64
void AvoidCVE_2016_2143();
#else
INLINE void AvoidCVE_2016_2143() {}
#endif

struct StackDepotStats {
  uptr n_uniq_ids;
  uptr allocated;
};

// The default value for allocator_release_to_os_interval_ms common flag to
// indicate that sanitizer allocator should not attempt to release memory to OS.
const s32 kReleaseToOSIntervalNever = -1;

}  // namespace __sanitizer

inline void *operator new(__sanitizer::operator_new_size_type size,
                          __sanitizer::LowLevelAllocator &alloc) {
  return alloc.Allocate(size);
}

#endif  // SANITIZER_COMMON_H
