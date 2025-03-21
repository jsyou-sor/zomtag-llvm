//===-- sanitizer_stoptheworld_linux_libcdep.cc ---------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// See sanitizer_stoptheworld.h for details.
// This implementation was inspired by Markus Gutschke's linuxthreads.cc.
//
//===----------------------------------------------------------------------===//

#include "sanitizer_platform.h"

#if SANITIZER_LINUX && (defined(__x86_64__) || defined(__mips__) || \
                        defined(__aarch64__) || defined(__powerpc64__) || \
                        defined(__s390__))

#include "sanitizer_stoptheworld.h"

#include "sanitizer_platform_limits_posix.h"
#include "sanitizer_atomic.h"

#include <errno.h>
#include <sched.h> // for CLONE_* definitions
#include <stddef.h>
#include <sys/prctl.h> // for PR_* definitions
#include <sys/ptrace.h> // for PTRACE_* definitions
#include <sys/types.h> // for pid_t
#include <sys/uio.h> // for iovec
#include <elf.h> // for NT_PRSTATUS
#if SANITIZER_ANDROID && defined(__arm__)
# include <linux/user.h>  // for pt_regs
#else
# ifdef __aarch64__
// GLIBC 2.20+ sys/user does not include asm/ptrace.h
#  include <asm/ptrace.h>
# endif
# include <sys/user.h>  // for user_regs_struct
# if SANITIZER_ANDROID && SANITIZER_MIPS
#   include <asm/reg.h>  // for mips SP register in sys/user.h
# endif
#endif
#include <sys/wait.h> // for signal-related stuff

#ifdef sa_handler
# undef sa_handler
#endif

#ifdef sa_sigaction
# undef sa_sigaction
#endif

#include "sanitizer_common.h"
#include "sanitizer_flags.h"
#include "sanitizer_libc.h"
#include "sanitizer_linux.h"
#include "sanitizer_mutex.h"
#include "sanitizer_placement_new.h"

// This module works by spawning a Linux task which then attaches to every
// thread in the caller process with ptrace. This suspends the threads, and
// PTRACE_GETREGS can then be used to obtain their register state. The callback
// supplied to StopTheWorld() is run in the tracer task while the threads are
// suspended.
// The tracer task must be placed in a different thread group for ptrace to
// work, so it cannot be spawned as a pthread. Instead, we use the low-level
// clone() interface (we want to share the address space with the caller
// process, so we prefer clone() over fork()).
//
// We don't use any libc functions, relying instead on direct syscalls. There
// are two reasons for this:
// 1. calling a library function while threads are suspended could cause a
// deadlock, if one of the treads happens to be holding a libc lock;
// 2. it's generally not safe to call libc functions from the tracer task,
// because clone() does not set up a thread-local storage for it. Any
// thread-local variables used by libc will be shared between the tracer task
// and the thread which spawned it.

namespace __sanitizer {

COMPILER_CHECK(sizeof(SuspendedThreadID) == sizeof(pid_t));

// Structure for passing arguments into the tracer thread.
struct TracerThreadArgument {
  StopTheWorldCallback callback;
  void *callback_argument;
  // The tracer thread waits on this mutex while the parent finishes its
  // preparations.
  BlockingMutex mutex;
  // Tracer thread signals its completion by setting done.
  atomic_uintptr_t done;
  uptr parent_pid;
};

// This class handles thread suspending/unsuspending in the tracer thread.
class ThreadSuspender {
 public:
  explicit ThreadSuspender(pid_t pid, TracerThreadArgument *arg)
    : arg(arg)
    , pid_(pid) {
      CHECK_GE(pid, 0);
    }
  bool SuspendAllThreads();
  void ResumeAllThreads();
  void KillAllThreads();
  SuspendedThreadsList &suspended_threads_list() {
    return suspended_threads_list_;
  }
  TracerThreadArgument *arg;
 private:
  SuspendedThreadsList suspended_threads_list_;
  pid_t pid_;
  bool SuspendThread(SuspendedThreadID thread_id);
};

bool ThreadSuspender::SuspendThread(SuspendedThreadID tid) {
  // Are we already attached to this thread?
  // Currently this check takes linear time, however the number of threads is
  // usually small.
  if (suspended_threads_list_.Contains(tid))
    return false;
  int pterrno;
  if (internal_iserror(internal_ptrace(PTRACE_ATTACH, tid, nullptr, nullptr),
                       &pterrno)) {
    // Either the thread is dead, or something prevented us from attaching.
    // Log this event and move on.
    VReport(1, "Could not attach to thread %d (errno %d).\n", tid, pterrno);
    return false;
  } else {
    VReport(2, "Attached to thread %d.\n", tid);
    // The thread is not guaranteed to stop before ptrace returns, so we must
    // wait on it. Note: if the thread receives a signal concurrently,
    // we can get notification about the signal before notification about stop.
    // In such case we need to forward the signal to the thread, otherwise
    // the signal will be missed (as we do PTRACE_DETACH with arg=0) and
    // any logic relying on signals will break. After forwarding we need to
    // continue to wait for stopping, because the thread is not stopped yet.
    // We do ignore delivery of SIGSTOP, because we want to make stop-the-world
    // as invisible as possible.
    for (;;) {
      int status;
      uptr waitpid_status;
      HANDLE_EINTR(waitpid_status, internal_waitpid(tid, &status, __WALL));
      int wperrno;
      if (internal_iserror(waitpid_status, &wperrno)) {
        // Got a ECHILD error. I don't think this situation is possible, but it
        // doesn't hurt to report it.
        VReport(1, "Waiting on thread %d failed, detaching (errno %d).\n",
                tid, wperrno);
        internal_ptrace(PTRACE_DETACH, tid, nullptr, nullptr);
        return false;
      }
      if (WIFSTOPPED(status) && WSTOPSIG(status) != SIGSTOP) {
        internal_ptrace(PTRACE_CONT, tid, nullptr,
                        (void*)(uptr)WSTOPSIG(status));
        continue;
      }
      break;
    }
    suspended_threads_list_.Append(tid);
    return true;
  }
}

void ThreadSuspender::ResumeAllThreads() {
  for (uptr i = 0; i < suspended_threads_list_.thread_count(); i++) {
    pid_t tid = suspended_threads_list_.GetThreadID(i);
    int pterrno;
    if (!internal_iserror(internal_ptrace(PTRACE_DETACH, tid, nullptr, nullptr),
                          &pterrno)) {
      VReport(2, "Detached from thread %d.\n", tid);
    } else {
      // Either the thread is dead, or we are already detached.
      // The latter case is possible, for instance, if this function was called
      // from a signal handler.
      VReport(1, "Could not detach from thread %d (errno %d).\n", tid, pterrno);
    }
  }
}

void ThreadSuspender::KillAllThreads() {
  for (uptr i = 0; i < suspended_threads_list_.thread_count(); i++)
    internal_ptrace(PTRACE_KILL, suspended_threads_list_.GetThreadID(i),
                    nullptr, nullptr);
}

bool ThreadSuspender::SuspendAllThreads() {
  ThreadLister thread_lister(pid_);
  bool added_threads;
  bool first_iteration = true;
  do {
    // Run through the directory entries once.
    added_threads = false;
    pid_t tid = thread_lister.GetNextTID();
    while (tid >= 0) {
      if (SuspendThread(tid))
        added_threads = true;
      tid = thread_lister.GetNextTID();
    }
    if (thread_lister.error() || (first_iteration && !added_threads)) {
      // Detach threads and fail.
      ResumeAllThreads();
      return false;
    }
    thread_lister.Reset();
    first_iteration = false;
  } while (added_threads);
  return true;
}

// Pointer to the ThreadSuspender instance for use in signal handler.
static ThreadSuspender *thread_suspender_instance = nullptr;

// Synchronous signals that should not be blocked.
static const int kSyncSignals[] = { SIGABRT, SIGILL, SIGFPE, SIGSEGV, SIGBUS,
                                    SIGXCPU, SIGXFSZ };

static void TracerThreadDieCallback() {
  // Generally a call to Die() in the tracer thread should be fatal to the
  // parent process as well, because they share the address space.
  // This really only works correctly if all the threads are suspended at this
  // point. So we correctly handle calls to Die() from within the callback, but
  // not those that happen before or after the callback. Hopefully there aren't
  // a lot of opportunities for that to happen...
  ThreadSuspender *inst = thread_suspender_instance;
  if (inst && stoptheworld_tracer_pid == internal_getpid()) {
    inst->KillAllThreads();
    thread_suspender_instance = nullptr;
  }
}

// Signal handler to wake up suspended threads when the tracer thread dies.
static void TracerThreadSignalHandler(int signum, void *siginfo, void *uctx) {
  SignalContext ctx = SignalContext::Create(siginfo, uctx);
  Printf("Tracer caught signal %d: addr=0x%zx pc=0x%zx sp=0x%zx\n", signum,
         ctx.addr, ctx.pc, ctx.sp);
  ThreadSuspender *inst = thread_suspender_instance;
  if (inst) {
    if (signum == SIGABRT)
      inst->KillAllThreads();
    else
      inst->ResumeAllThreads();
    RAW_CHECK(RemoveDieCallback(TracerThreadDieCallback));
    thread_suspender_instance = nullptr;
    atomic_store(&inst->arg->done, 1, memory_order_relaxed);
  }
  internal__exit((signum == SIGABRT) ? 1 : 2);
}

// Size of alternative stack for signal handlers in the tracer thread.
static const int kHandlerStackSize = 4096;

// This function will be run as a cloned task.
static int TracerThread(void* argument) {
  TracerThreadArgument *tracer_thread_argument =
      (TracerThreadArgument *)argument;

  internal_prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
  // Check if parent is already dead.
  if (internal_getppid() != tracer_thread_argument->parent_pid)
    internal__exit(4);

  // Wait for the parent thread to finish preparations.
  tracer_thread_argument->mutex.Lock();
  tracer_thread_argument->mutex.Unlock();

  RAW_CHECK(AddDieCallback(TracerThreadDieCallback));

  ThreadSuspender thread_suspender(internal_getppid(), tracer_thread_argument);
  // Global pointer for the signal handler.
  thread_suspender_instance = &thread_suspender;

  // Alternate stack for signal handling.
  InternalScopedBuffer<char> handler_stack_memory(kHandlerStackSize);
  //struct sigaltstack handler_stack;
  stack_t handler_stack;
  internal_memset(&handler_stack, 0, sizeof(handler_stack));
  handler_stack.ss_sp = handler_stack_memory.data();
  handler_stack.ss_size = kHandlerStackSize;
  internal_sigaltstack(&handler_stack, nullptr);

  // Install our handler for synchronous signals. Other signals should be
  // blocked by the mask we inherited from the parent thread.
  for (uptr i = 0; i < ARRAY_SIZE(kSyncSignals); i++) {
    __sanitizer_sigaction act;
    internal_memset(&act, 0, sizeof(act));
    act.sigaction = TracerThreadSignalHandler;
    act.sa_flags = SA_ONSTACK | SA_SIGINFO;
    internal_sigaction_norestorer(kSyncSignals[i], &act, 0);
  }

  int exit_code = 0;
  if (!thread_suspender.SuspendAllThreads()) {
    VReport(1, "Failed suspending threads.\n");
    exit_code = 3;
  } else {
    tracer_thread_argument->callback(thread_suspender.suspended_threads_list(),
                                     tracer_thread_argument->callback_argument);
    thread_suspender.ResumeAllThreads();
    exit_code = 0;
  }
  RAW_CHECK(RemoveDieCallback(TracerThreadDieCallback));
  thread_suspender_instance = nullptr;
  atomic_store(&tracer_thread_argument->done, 1, memory_order_relaxed);
  return exit_code;
}

class ScopedStackSpaceWithGuard {
 public:
  explicit ScopedStackSpaceWithGuard(uptr stack_size) {
    stack_size_ = stack_size;
    guard_size_ = GetPageSizeCached();
    // FIXME: Omitting MAP_STACK here works in current kernels but might break
    // in the future.
    guard_start_ = (uptr)MmapOrDie(stack_size_ + guard_size_,
                                   "ScopedStackWithGuard");
    CHECK(MprotectNoAccess((uptr)guard_start_, guard_size_));
  }
  ~ScopedStackSpaceWithGuard() {
    UnmapOrDie((void *)guard_start_, stack_size_ + guard_size_);
  }
  void *Bottom() const {
    return (void *)(guard_start_ + stack_size_ + guard_size_);
  }

 private:
  uptr stack_size_;
  uptr guard_size_;
  uptr guard_start_;
};

// We have a limitation on the stack frame size, so some stuff had to be moved
// into globals.
static __sanitizer_sigset_t blocked_sigset;
static __sanitizer_sigset_t old_sigset;

class StopTheWorldScope {
 public:
  StopTheWorldScope() {
    // Make this process dumpable. Processes that are not dumpable cannot be
    // attached to.
    process_was_dumpable_ = internal_prctl(PR_GET_DUMPABLE, 0, 0, 0, 0);
    if (!process_was_dumpable_)
      internal_prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
  }

  ~StopTheWorldScope() {
    // Restore the dumpable flag.
    if (!process_was_dumpable_)
      internal_prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
  }

 private:
  int process_was_dumpable_;
};

// When sanitizer output is being redirected to file (i.e. by using log_path),
// the tracer should write to the parent's log instead of trying to open a new
// file. Alert the logging code to the fact that we have a tracer.
struct ScopedSetTracerPID {
  explicit ScopedSetTracerPID(uptr tracer_pid) {
    stoptheworld_tracer_pid = tracer_pid;
    stoptheworld_tracer_ppid = internal_getpid();
  }
  ~ScopedSetTracerPID() {
    stoptheworld_tracer_pid = 0;
    stoptheworld_tracer_ppid = 0;
  }
};

void StopTheWorld(StopTheWorldCallback callback, void *argument) {
  StopTheWorldScope in_stoptheworld;
  // Prepare the arguments for TracerThread.
  struct TracerThreadArgument tracer_thread_argument;
  tracer_thread_argument.callback = callback;
  tracer_thread_argument.callback_argument = argument;
  tracer_thread_argument.parent_pid = internal_getpid();
  atomic_store(&tracer_thread_argument.done, 0, memory_order_relaxed);
  const uptr kTracerStackSize = 2 * 1024 * 1024;
  ScopedStackSpaceWithGuard tracer_stack(kTracerStackSize);
  // Block the execution of TracerThread until after we have set ptrace
  // permissions.
  tracer_thread_argument.mutex.Lock();
  // Signal handling story.
  // We don't want async signals to be delivered to the tracer thread,
  // so we block all async signals before creating the thread. An async signal
  // handler can temporary modify errno, which is shared with this thread.
  // We ought to use pthread_sigmask here, because sigprocmask has undefined
  // behavior in multithreaded programs. However, on linux sigprocmask is
  // equivalent to pthread_sigmask with the exception that pthread_sigmask
  // does not allow to block some signals used internally in pthread
  // implementation. We are fine with blocking them here, we are really not
  // going to pthread_cancel the thread.
  // The tracer thread should not raise any synchronous signals. But in case it
  // does, we setup a special handler for sync signals that properly kills the
  // parent as well. Note: we don't pass CLONE_SIGHAND to clone, so handlers
  // in the tracer thread won't interfere with user program. Double note: if a
  // user does something along the lines of 'kill -11 pid', that can kill the
  // process even if user setup own handler for SEGV.
  // Thing to watch out for: this code should not change behavior of user code
  // in any observable way. In particular it should not override user signal
  // handlers.
  internal_sigfillset(&blocked_sigset);
  for (uptr i = 0; i < ARRAY_SIZE(kSyncSignals); i++)
    internal_sigdelset(&blocked_sigset, kSyncSignals[i]);
  int rv = internal_sigprocmask(SIG_BLOCK, &blocked_sigset, &old_sigset);
  CHECK_EQ(rv, 0);
  uptr tracer_pid = internal_clone(
      TracerThread, tracer_stack.Bottom(),
      CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_UNTRACED,
      &tracer_thread_argument, nullptr /* parent_tidptr */,
      nullptr /* newtls */, nullptr /* child_tidptr */);
  internal_sigprocmask(SIG_SETMASK, &old_sigset, 0);
  int local_errno = 0;
  if (internal_iserror(tracer_pid, &local_errno)) {
    VReport(1, "Failed spawning a tracer thread (errno %d).\n", local_errno);
    tracer_thread_argument.mutex.Unlock();
  } else {
    ScopedSetTracerPID scoped_set_tracer_pid(tracer_pid);
    // On some systems we have to explicitly declare that we want to be traced
    // by the tracer thread.
#ifdef PR_SET_PTRACER
    internal_prctl(PR_SET_PTRACER, tracer_pid, 0, 0, 0);
#endif
    // Allow the tracer thread to start.
    tracer_thread_argument.mutex.Unlock();
    // NOTE: errno is shared between this thread and the tracer thread.
    // internal_waitpid() may call syscall() which can access/spoil errno,
    // so we can't call it now. Instead we for the tracer thread to finish using
    // the spin loop below. Man page for sched_yield() says "In the Linux
    // implementation, sched_yield() always succeeds", so let's hope it does not
    // spoil errno. Note that this spin loop runs only for brief periods before
    // the tracer thread has suspended us and when it starts unblocking threads.
    while (atomic_load(&tracer_thread_argument.done, memory_order_relaxed) == 0)
      sched_yield();
    // Now the tracer thread is about to exit and does not touch errno,
    // wait for it.
    for (;;) {
      uptr waitpid_status = internal_waitpid(tracer_pid, nullptr, __WALL);
      if (!internal_iserror(waitpid_status, &local_errno))
        break;
      if (local_errno == EINTR)
        continue;
      VReport(1, "Waiting on the tracer thread failed (errno %d).\n",
              local_errno);
      break;
    }
  }
}

// Platform-specific methods from SuspendedThreadsList.
#if SANITIZER_ANDROID && defined(__arm__)
typedef pt_regs regs_struct;
#define REG_SP ARM_sp

#elif SANITIZER_LINUX && defined(__arm__)
typedef user_regs regs_struct;
#define REG_SP uregs[13]

#elif defined(__i386__) || defined(__x86_64__)
typedef user_regs_struct regs_struct;
#if defined(__i386__)
#define REG_SP esp
#else
#define REG_SP rsp
#endif

#elif defined(__powerpc__) || defined(__powerpc64__)
typedef pt_regs regs_struct;
#define REG_SP gpr[PT_R1]

#elif defined(__mips__)
typedef struct user regs_struct;
# if SANITIZER_ANDROID
#  define REG_SP regs[EF_R29]
# else
#  define REG_SP regs[EF_REG29]
# endif

#elif defined(__aarch64__)
typedef struct user_pt_regs regs_struct;
#define REG_SP sp
#define ARCH_IOVEC_FOR_GETREGSET

#elif defined(__s390__)
typedef _user_regs_struct regs_struct;
#define REG_SP gprs[15]
#define ARCH_IOVEC_FOR_GETREGSET

#else
#error "Unsupported architecture"
#endif // SANITIZER_ANDROID && defined(__arm__)

int SuspendedThreadsList::GetRegistersAndSP(uptr index,
                                            uptr *buffer,
                                            uptr *sp) const {
  pid_t tid = GetThreadID(index);
  regs_struct regs;
  int pterrno;
#ifdef ARCH_IOVEC_FOR_GETREGSET
  struct iovec regset_io;
  regset_io.iov_base = &regs;
  regset_io.iov_len = sizeof(regs_struct);
  bool isErr = internal_iserror(internal_ptrace(PTRACE_GETREGSET, tid,
                                (void*)NT_PRSTATUS, (void*)&regset_io),
                                &pterrno);
#else
  bool isErr = internal_iserror(internal_ptrace(PTRACE_GETREGS, tid, nullptr,
                                &regs), &pterrno);
#endif
  if (isErr) {
    VReport(1, "Could not get registers from thread %d (errno %d).\n", tid,
            pterrno);
    return -1;
  }

  *sp = regs.REG_SP;
  internal_memcpy(buffer, &regs, sizeof(regs));
  return 0;
}

uptr SuspendedThreadsList::RegisterCount() {
  return sizeof(regs_struct) / sizeof(uptr);
}
} // namespace __sanitizer

#endif  // SANITIZER_LINUX && (defined(__x86_64__) || defined(__mips__)
        // || defined(__aarch64__) || defined(__powerpc64__)
        // || defined(__s390__)
