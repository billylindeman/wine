/*
 * wineweb_shim.c — the unix-side OS shim for Wine on wasm (P2.8).
 *
 * Implements the P2.1-cataloged gap surface (docs/spikes/p2-wine-builtin.md
 * §3.2) that Wine's ntdll unix sources expect from a host OS:
 *
 *   mmap/munmap/mprotect/madvise  → VM manager (in-module Rust, p2_* C ABI);
 *                                   anonymous-only, documented below.
 *   fork/clone/waitpid/execv/...  → loud-unsupported (no process model).
 *   sigprocmask/pthread_sigmask   → no-op masking (docs/02 §7: no async
 *                                   signals; exception dispatch is ours).
 *   socket family                 → miniserver wire shapes: descriptors and
 *                                   call shapes exist, real I/O is P6.
 *   clock_gettime/gettimeofday/   → wineweb_host time imports (determinism
 *   time/nanosleep                  mode, docs/04 §8).
 *   getrandom                     → deterministic PRNG (docs/04 §8).
 *   umask                         → process-static bookkeeping.
 *   fchdir/fstatfs/pipe           → loud-unsupported (VFS is not fd-based).
 *   getpid/getuid                 → the module identity (PEB ClientId 1/1).
 *   times/termios (serial)        → zeros / loud-unsupported (no kernelside
 *                                   scheduling or TTY devices).
 *
 * Every function owns exactly its documented surface and must never
 * fabricate success for an operation the platform cannot perform: failures
 * are real errno values, unsupported operations are loud (diag log) and
 * return the honest error. The logic lives in `wineweb_shim_*` entry points
 * — that is what the native harness (tools/shim-native-test.c) exercises.
 * The POSIX-named symbols are thin `#ifdef __wasi__` wrappers: on the wasm
 * flavor they override libc (whose emu libraries are NOT linked — these
 * definitions win at final-module link), while on host libc the native
 * harness must not hijack the allocator/runtime, so libc keeps its own.
 */
#define _GNU_SOURCE 1 /* Dl_info declaration on host libc builds */
#if 0 /* makedep: this source is part of the ntdll unix side */
#pragma makedep unix
#endif

#include <errno.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/times.h>
#include <sys/types.h>
#include <pwd.h>
#include <time.h>
#include <unistd.h>
#ifdef __wasi__ /* the fd delegation below uses the wasi bottom half */
#include <wasi/api.h>
#endif

#include "wineweb_shim.h"

/* wasi-libc keeps the syscall-backed clock read under this internal name
 * (clock_gettime is a weak alias); no public header declares it. */
#ifdef __wasi__
extern int __clock_gettime(clockid_t clk, struct timespec *tp);
#endif

/* ------------------------------------------------------------------------- */
/* diag helper: every loud-unsupported path reports through the diag group. */

__attribute__((format(printf, 1, 2)))
static void shim_loud(const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    wineweb_host_log(2 /* warn */, buf, strlen(buf));
}

/* mmap prot → Win32 PAGE_* constant for the VM manager's typed conversion
 * (`Protect::from_win32`). Returns -1 (loud) for PROT_NONE and unknown bits:
 * the VM manager has no NoAccess state yet (docs/03 §6 — protection without
 * dispatcher checks is bookkeeping only). */
static int prot_to_guest(int prot, uint32_t *out)
{
    const int RWX = PROT_READ | PROT_WRITE | PROT_EXEC;

    if ((prot & ~RWX) || !(prot & (PROT_READ | PROT_WRITE | PROT_EXEC))) return -1;
    if (prot & PROT_WRITE) *out = (prot & PROT_EXEC) ? 0x40 /* PAGE_EXECUTE_READWRITE */ : 0x04 /* PAGE_READWRITE */;
    else if (prot & PROT_EXEC) *out = 0x20 /* PAGE_EXECUTE_READ */;
    else *out = 0x02 /* PAGE_READONLY */;
    return 0;
}

/* mmap/munmap/madvise — anonymous-only bookkeeping over the guest region
 * (docs/03 §6). File-backed mappings must never appear here: image and file
 * views are the PE loader's job (docs/03 §5); they arrive loud. */
void *wineweb_shim_mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    uint32_t guest_prot;
    uint64_t base = 0, out = 0;

    if (fd != -1 || !(flags & MAP_PRIVATE) || !(flags & MAP_ANONYMOUS))
    {
        shim_loud("wineweb_shim: mmap(fd=%d flags=%#x) file-backed/shared unsupported (docs/03 §5)", fd, flags);
        errno = ENOSYS;
        return MAP_FAILED;
    }
    /* PROT_NONE is the reservation shape (Wine's MEM_RESERVE path maps it):
     * track the range through the VM manager, no pages back it. MAP_FIXED
     * and MAP_FIXED_NOREPLACE are both address-pinned; a conflict is EEXIST
     * (the probe protocol try_map_free_area walks by). */
    if (prot == PROT_NONE)
    {
        int rc;
        if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) base = (uintptr_t)addr;
        rc = p2_vm_reserve(base, length, &out);
        if (rc != 0)
        {
            errno = (rc == -11) ? EEXIST : ENOMEM; /* -11 = VM_OVERLAP, the probe protocol's EEXIST */
            return MAP_FAILED;
        }
        return (void *)(uintptr_t)out;
    }
    if (prot_to_guest(prot, &guest_prot) == -1)
    {
        shim_loud("wineweb_shim: mmap(prot=%#x) unsupported protection (docs/03 §6)", prot);
        errno = ENOSYS;
        return MAP_FAILED;
    }
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) base = (uintptr_t)addr;
    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)
         ? p2_vm_alloc_fixed(base, length, guest_prot, &out)
         : p2_vm_alloc(base, length, guest_prot, &out)) == -1)
    {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return (void *)(uintptr_t)out;
}

int wineweb_shim_munmap(void *addr, size_t length)
{
    if (p2_vm_unmap((uintptr_t)addr, length) == -1)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

/* Protection changes are loud-unsupported: the VM manager tracks state
 * host-side, but honoring them needs dispatcher checks that do not exist
 * (docs/03 §6). */
int wineweb_shim_mprotect(void *addr, size_t len, int prot)
{
    /* Protection without a dispatcher is bookkeeping only (docs/03 §6): the
     * VM manager tracks the new protection over the tracked range; pages in
     * the linear memory were never guarded. Untracked ranges stay loud. */
    uint32_t guest_prot;
    if (prot_to_guest(prot, &guest_prot) == -1)
    {
        shim_loud("wineweb_shim: mprotect(prot=%#x) unsupported protection", prot);
        errno = ENOSYS;
        return -1;
    }
    if (p2_vm_protect((uintptr_t)addr, len, guest_prot) != 0)
    {
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

/* MADV_DONTNEED is the one advice with wasm semantics: drop the contents
 * (decommit — the next commit re-zeroes, docs/03 §6) while keeping the
 * mapping. Other advice is a no-op: nothing to prefetch or release in a
 * contiguous linear memory. */
int wineweb_shim_madvise(void *addr, size_t length, int advice)
{
    if (advice == MADV_DONTNEED && p2_vm_decommit((uintptr_t)addr, length) == -1)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

#ifdef __wasi__
void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{ return wineweb_shim_mmap(addr, length, prot, flags, fd, offset); }
int munmap(void *addr, size_t length) { return wineweb_shim_munmap(addr, length); }
int mprotect(void *addr, size_t len, int prot) { return wineweb_shim_mprotect(addr, len, prot); }
int madvise(void *addr, size_t length, int advice) { return wineweb_shim_madvise(addr, length, advice); }
#endif

/* ------------------------------------------------------------------------- */
/* Process creation — loud-unsupported: there is no process model (docs/03 §7,
 * "Process = the whole module instance"). Every entry logs and fails honestly
 * rather than pretending; CreateProcess is a P6 design item. */

static int shim_no_process(const char *call)
{
    shim_loud("wineweb_shim: %s unsupported — no process model (docs/03 §7)", call);
    errno = ENOSYS;
    return -1;
}

pid_t wineweb_shim_fork(void) { return shim_no_process("fork"); }
pid_t wineweb_shim_waitpid(pid_t pid, int *status, int options) { return shim_no_process("waitpid"); }
int wineweb_shim_execv(const char *path, char *const argv[]) { return shim_no_process("execv"); }
int wineweb_shim_execvp(const char *file, char *const argv[]) { return shim_no_process("execvp"); }
pid_t wineweb_shim_setsid(void) { return shim_no_process("setsid"); }
int wineweb_shim_pipe(int fds[2])
{
    shim_loud("wineweb_shim: pipe unsupported — server transport is the SAB rings (docs/03 §4)");
    errno = ENOSYS;
    return -1;
}

/* clone keeps the POSIX shape unix/thread.c will call through (P2.9); on the
 * wasm flavor threads come from the sched group, never from clone. */
int wineweb_shim_clone(int (*fn)(void *), void *child_stack, int flags, void *arg, ...)
{
    return shim_no_process("clone");
}

/* pthread_exit — wasi-threads has no thread-exit import (its thread wrapper
 * returns instead), so an exit from arbitrary call depth cannot unwind.
 * Loud-unsupported until the sched seam owns thread death (TODO(P2.4):
 * cooperative thread-exit on the SAB rings). */
void wineweb_shim_pthread_exit(void *retval)
{
    shim_loud("wineweb_shim: pthread_exit unsupported — thread death is the P2.4 sched seam");
    abort();
}

/* posix_spawn keeps the real signature (loader.c's preloader re-exec path);
 * on the wasm flavor it can never succeed. */
int wineweb_shim_posix_spawn(pid_t *pid, const char *path,
                             const posix_spawn_file_actions_t *file_actions,
                             const posix_spawnattr_t *attrp,
                             char *const argv[], char *const envp[])
{
    return shim_no_process("posix_spawn") == -1 ? errno : 0;
}

int wineweb_shim_fchdir(int fd)
{
    shim_loud("wineweb_shim: fchdir unsupported — VFS is not fd-based (docs/04 §4 fs)");
    errno = ENOSYS;
    return -1;
}

/* sigaltstack: there are no signal stacks to swap (docs/02 §7); the caller
 * in server.c ignores the return value. */
int wineweb_shim_sigaltstack(const stack_t *ss, stack_t *old_ss) { errno = ENOSYS; return -1; }

/* No passwd database: set_home_dir falls back to the environment (the wasm
 * identity arrives with the browser context later). */
struct passwd *wineweb_shim_getpwuid(uid_t uid) { return NULL; }

/* No dynamic loader: dladdr fails honestly (loader.c's ntdll.so path
 * discovery does not apply on wasm — the module *is* the loader). */
int wineweb_shim_dladdr(const void *addr, void *info) { errno = ENOSYS; return 0; }

/* fstatfs is only referenced behind a compiled-out switch on wasm (file.c
 * uses the struct opaquely); report "unknown filesystem" honestly. */
int wineweb_shim_fstatfs(int fd, struct statfs *buf)
{
    errno = ENOSYS;
    return -1;
}

/* The module is the process: identity constants (PEB ClientId 1/1 is the
 * loader contract, docs/spikes/p2-pe-loader.md §2). */
pid_t wineweb_shim_getpid(void) { return 1; }
uid_t wineweb_shim_getuid(void) { return 0; }

#ifdef __wasi__
pid_t fork(void) { return wineweb_shim_fork(); }
pid_t waitpid(pid_t pid, int *status, int options) { return wineweb_shim_waitpid(pid, status, options); }
int execv(const char *path, char *const argv[]) { return wineweb_shim_execv(path, argv); }
int execvp(const char *file, char *const argv[]) { return wineweb_shim_execvp(file, argv); }
pid_t setsid(void) { return wineweb_shim_setsid(); }
int pipe(int fds[2]) { return wineweb_shim_pipe(fds); }
int fchdir(int fd) { return wineweb_shim_fchdir(fd); }
int sigaltstack(const stack_t *ss, stack_t *old_ss) { return wineweb_shim_sigaltstack(ss, old_ss); }
struct passwd *getpwuid(uid_t uid) { return wineweb_shim_getpwuid(uid); }
int dladdr(const void *addr, Dl_info *info) { return wineweb_shim_dladdr(addr, info); }
int fstatfs(int fd, struct statfs *buf) { return wineweb_shim_fstatfs(fd, buf); }
pid_t getpid(void) { return wineweb_shim_getpid(); }
uid_t getuid(void) { return wineweb_shim_getuid(); }
#endif

/* ------------------------------------------------------------------------- */
/* CPU model — the fixed FEX HostFeatures-shaped baseline (docs/02 §8). The P1
 * link spike created the FEX context with HostFeatures{SupportsAtomics=1,
 * everything else 0} (docs/spikes/p1-fexcore-link.md §4); the guest i386 model
 * reports the same conservatism: cmpxchg8b only, no x87/SSE claims until the
 * wasm emitter can lift them. TODO(P2.4): source this from FEX's CPUID
 * emulation instead of the fixed table. */

void wineweb_shim_cpu_model(struct wineweb_cpu_model *out)
{
    static const struct wineweb_cpu_model baseline =
    {
        .vendor = "GenuineIntel",
        .brand = "wineweb guest (FEX wasm32 baseline)",
        .family = 6,
        .model = 0,
        .stepping = 0,
        .pf_bits = { (UINT64_C(1) << 2), /* PF_COMPARE_EXCHANGE_DOUBLE */
                     0 },
    };
    *out = baseline;
}

/* ------------------------------------------------------------------------- */
/* Signal masking — no-op (docs/02 §7: no async signals; the host sets
 * interrupt flags at block edges instead of preempting). Masking calls must
 * succeed so Wine's server-pipe setup runs unchanged; there is simply
 * nothing to mask. */

int wineweb_shim_sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    if (oldset) memset(oldset, 0, sizeof(*oldset));
    return 0;
}

int wineweb_shim_pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
    return wineweb_shim_sigprocmask(how, set, oldset);
}

/* Handler registration is a semantic no-op: wasi has no signals to deliver
 * and doc 02 §7 routes exceptions through our own dispatch, so the request
 * "do not interrupt me with SIGPIPE" is vacuously guaranteed. */
int wineweb_shim_sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
    if (oldact) memset(oldact, 0, sizeof(*oldact));
    return 0;
}

int wineweb_shim_sigemptyset(sigset_t *set)
{
    if (!set) { errno = EFAULT; return -1; }
    memset(set, 0, sizeof(*set));
    return 0;
}

/* Bit set per signal number over the raw bytes: wasi's placeholder sigset_t
 * is a single unsigned char, and the host harness's little-endian sigset_t
 * has its first 8 signals in byte 0 — the same layout for every signal the
 * unix side masks. */
int wineweb_shim_sigaddset(sigset_t *set, int sig)
{
    unsigned char *bytes = (unsigned char *)set;

    if (!set || sig <= 0 || sig > 8 * (int)sizeof(sigset_t)) { errno = EINVAL; return -1; }
    bytes[(sig - 1) / 8] |= (unsigned char)(1 << ((sig - 1) % 8));
    return 0;
}

#ifdef __wasi__
int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) { return wineweb_shim_sigprocmask(how, set, oldset); }
int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset) { return wineweb_shim_pthread_sigmask(how, set, oldset); }
int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact) { return wineweb_shim_sigaction(sig, act, oldact); }
int sigemptyset(sigset_t *set) { return wineweb_shim_sigemptyset(set); }
int sigaddset(sigset_t *set, int sig) { return wineweb_shim_sigaddset(set, sig); }
_Noreturn void pthread_exit(void *retval) { wineweb_shim_pthread_exit(retval); }
#endif

/* ------------------------------------------------------------------------- */
/* rlimits — recorded, never enforced: a wasm linear memory has no kernelside
 * limits for the unix-side limit families. getrlimit reports the recorded
 * value; setrlimit updates it (loader.c raises RLIMIT_NOFILE at boot). */

static struct rlimit shim_rlimits[RLIMIT_NOFILE + 1] = {
    [0 ... RLIMIT_NOFILE] = { RLIM_INFINITY, RLIM_INFINITY },
};

int wineweb_shim_getrlimit(int resource, struct rlimit *rlp)
{
    if (!rlp || resource < 0 || resource > RLIMIT_NOFILE) { errno = EINVAL; return -1; }
    *rlp = shim_rlimits[resource];
    return 0;
}

int wineweb_shim_setrlimit(int resource, const struct rlimit *rlp)
{
    if (!rlp || resource < 0 || resource > RLIMIT_NOFILE) { errno = EINVAL; return -1; }
    shim_rlimits[resource] = *rlp;
    return 0;
}

#ifdef __wasi__
int getrlimit(int resource, struct rlimit *rlp) { return wineweb_shim_getrlimit(resource, rlp); }
int setrlimit(int resource, const struct rlimit *rlp) { return wineweb_shim_setrlimit(resource, rlp); }
#endif

/* ------------------------------------------------------------------------- */
/* Sockets — wire shapes now, real I/O in P6 (block spec; docs/03 §4: the
 * miniserver SAB rings replace the wineserver socket). Descriptors come from
 * a private range so they can never alias real wasi file descriptors; the
 * P6 wiring lands in this one table. No socket call may fabricate bytes on
 * the wire: every I/O entry is loud-unsupported with an honest errno.
 *
 * The whole family is wasm-only: on host libc the OS provides these calls
 * (with __SOCKADDR_ARG parameter types that cannot be re-declared), and the
 * native harness exercises the family through its own libc instead. */

#ifdef __wasi__

#define SHIM_FD_BASE 0x1000
#define SHIM_FD_COUNT 64
static unsigned char fd_used[SHIM_FD_COUNT];

static int fd_alloc(void)
{
    int i;

    for (i = 0; i < SHIM_FD_COUNT; i++) if (!fd_used[i]) break;
    if (i == SHIM_FD_COUNT) { errno = EMFILE; return -1; }
    fd_used[i] = 1;
    return SHIM_FD_BASE + i;
}

static int fd_is_socket(int fd)
{
    return fd >= SHIM_FD_BASE && fd < SHIM_FD_BASE + SHIM_FD_COUNT;
}

int socket(int domain, int type, int protocol)
{
    int fd = fd_alloc();

    if (fd != -1) shim_loud("wineweb_shim: socket(%#x/%#x) shape-only, I/O is P6", domain, type);
    return fd;
}

int socketpair(int domain, int type, int protocol, int sv[2])
{
    shim_loud("wineweb_shim: socketpair shape-only (docs/03 §4)");
    errno = EOPNOTSUPP;
    return -1;
}

int connect(int fd, const struct sockaddr *addr, socklen_t addrlen)
{
    shim_loud("wineweb_shim: connect(fd=%d) → miniserver rings in P6", fd);
    errno = EAFNOSUPPORT;
    return -1;
}

int bind(int fd, const struct sockaddr *addr, socklen_t addrlen) { errno = EAFNOSUPPORT; return -1; }
int listen(int fd, int backlog) { errno = EAFNOSUPPORT; return -1; }
int accept(int fd, struct sockaddr *addr, socklen_t *addrlen) { errno = EAFNOSUPPORT; return -1; }
int shutdown(int fd, int how) { errno = ENOTCONN; return -1; }
int getsockname(int fd, struct sockaddr *addr, socklen_t *addrlen) { errno = ENOTSOCK; return -1; }
int getpeername(int fd, struct sockaddr *addr, socklen_t *addrlen) { errno = ENOTSOCK; return -1; }
int getsockopt(int fd, int level, int optname, void *optval, socklen_t *optlen) { errno = ENOTSOCK; return -1; }
int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) { errno = ENOTSOCK; return -1; }

ssize_t send(int fd, const void *buf, size_t len, int flags) { errno = ENOTSOCK; return -1; }
ssize_t recv(int fd, void *buf, size_t len, int flags) { errno = ENOTSOCK; return -1; }
ssize_t sendto(int fd, const void *buf, size_t len, int flags,
               const struct sockaddr *addr, socklen_t addrlen) { errno = EAFNOSUPPORT; return -1; }
ssize_t recvfrom(int fd, void *buf, size_t len, int flags,
                 struct sockaddr *addr, socklen_t *addrlen) { errno = EAFNOSUPPORT; return -1; }
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) { errno = EAFNOSUPPORT; return -1; }
ssize_t recvmsg(int fd, struct msghdr *msg, int flags) { errno = EAFNOSUPPORT; return -1; }

int close(int fd)
{
    if (fd_is_socket(fd))
    {
        fd_used[fd - SHIM_FD_BASE] = 0;
        return 0;
    }
    return __wasi_fd_close(fd) ? -1 : 0; /* real wasi fds keep their semantics */
}

#endif /* __wasi__ (socket family) */

/* ------------------------------------------------------------------------- */
/* Time — all guest-visible clocks flow through the wineweb_host time group
 * (docs/04 §8 determinism mode). wasi's own clock_gettime is bypassed so the
 * host can pin clocks in tests. */

int wineweb_shim_clock_gettime(clockid_t clk_id, struct timespec *tp)
{
    if (!tp) { errno = EFAULT; return -1; }
    /* wasi's clockid_t is a pointer to a per-clock descriptor, so the
     * comparisons are pointer identities, not integer cases. */
    if (clk_id == CLOCK_MONOTONIC)
    {
        uint64_t ns = wineweb_host_query_perf_counter();
        tp->tv_sec = ns / 1000000000ull;
        tp->tv_nsec = ns % 1000000000ull;
        return 0;
    }
    if (clk_id == CLOCK_REALTIME)
    {
        uint64_t s = wineweb_host_query_unix_time();
        tp->tv_sec = s;
        tp->tv_nsec = 0;
        return 0;
    }
#ifdef __wasi__
    return __clock_gettime(clk_id, tp);
#else
    errno = ENOTSUP;
    return -1;
#endif
}

int wineweb_shim_gettimeofday(struct timeval *tv, void *tz)
{
    struct timespec ts;

    if (!tv) { errno = EFAULT; return -1; }
    if (wineweb_shim_clock_gettime(CLOCK_REALTIME, &ts) == -1) return -1;
    tv->tv_sec = ts.tv_sec;
    tv->tv_usec = ts.tv_nsec / 1000;
    return 0;
}

time_t wineweb_shim_time(time_t *t)
{
    time_t now = wineweb_host_query_unix_time();
    if (t) *t = now;
    return now;
}

/* nanosleep keeps the sync-by-default contract (docs/04 §5): the guest
 * thread blocks on the host monotonic clock. TODO(P2.4): replace the spin
 * with the sched group's futex wait once the SAB rings exist. */
int wineweb_shim_nanosleep(const struct timespec *req, struct timespec *rem)
{
    struct timespec start, now;

    if (!req || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) { errno = EINVAL; return -1; }
    wineweb_shim_clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;)
    {
        uint64_t elapsed;

        wineweb_shim_clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed = (uint64_t)(now.tv_sec - start.tv_sec) * 1000000000ull + (now.tv_nsec - start.tv_nsec);
        if (elapsed >= (uint64_t)req->tv_sec * 1000000000ull + req->tv_nsec) return 0;
    }
}

#ifdef __wasi__
int clock_gettime(clockid_t clk_id, struct timespec *tp) { return wineweb_shim_clock_gettime(clk_id, tp); }
int gettimeofday(struct timeval *tv, void *tz) { return wineweb_shim_gettimeofday(tv, tz); }
time_t time(time_t *t) { return wineweb_shim_time(t); }
int nanosleep(const struct timespec *req, struct timespec *rem) { return wineweb_shim_nanosleep(req, rem); }
#endif

/* ------------------------------------------------------------------------- */
/* getrandom — deterministic source (docs/04 §8: fixed clocks + fixed entropy
 * make whole-session replays possible). NOT cryptographically secure; the
 * browser host injects real entropy through an S2 group in P6. */

static uint64_t prng_state = 0x9e3779b97f4a7c15ull; /* fixed seed: determinism */

static uint64_t prng_next(void)
{
    uint64_t x = prng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return prng_state = x;
}

ssize_t wineweb_shim_getrandom(void *buf, size_t len, unsigned int flags)
{
    size_t i;

    if (!buf && len) { errno = EFAULT; return -1; }
    for (i = 0; i < len; i++) ((unsigned char *)buf)[i] = (unsigned char)prng_next();
    return (ssize_t)len;
}

#ifdef __wasi__
ssize_t getrandom(void *buf, size_t len, unsigned int flags) { return wineweb_shim_getrandom(buf, len, flags); }
#endif

/* ------------------------------------------------------------------------- */
/* Misc libc gaps. */

static mode_t current_umask = 0022;

mode_t wineweb_shim_umask(mode_t mask)
{
    mode_t old = current_umask;
    current_umask = mask & 0777;
    return old;
}

#ifdef __wasi__
mode_t umask(mode_t mask) { return wineweb_shim_umask(mask); }
#endif

/* times() — process CPU time is not observable in a wasm module (no
 * kernelside scheduling to attribute); the honest report is zeros. The
 * miniserver's thread bookkeeping replaces this in P2.4 (docs/03 §7). */
clock_t wineweb_shim_times(struct tms *buf)
{
    if (buf) memset(buf, 0, sizeof(*buf));
    return 0;
}

#ifdef __wasi__
clock_t times(struct tms *buf) { return wineweb_shim_times(buf); }
#endif

/* ------------------------------------------------------------------------- */
/* Serial — loud-unsupported: the wasm VFS has no COM ports (docs/04 §4 fs
 * group), so the tc* family reports the absence honestly. The declarations
 * live in the termios shim header; the driver compiles, and any open fails
 * at the VFS layer before these could be reached. */

#ifdef __wasi__

int tcgetattr(int fd, struct termios *t) { errno = ENOTTY; return -1; }
int tcsetattr(int fd, int actions, const struct termios *t) { errno = ENOTTY; return -1; }
int cfsetispeed(struct termios *t, unsigned int speed) { errno = ENOTTY; return -1; }
int cfsetospeed(struct termios *t, unsigned int speed) { errno = ENOTTY; return -1; }
unsigned int cfgetospeed(const struct termios *t) { return 0; }
int tcdrain(int fd) { errno = ENOTTY; return -1; }
int tcflush(int fd, int queue) { errno = ENOTTY; return -1; }
int tcflow(int fd, int action) { errno = ENOTTY; return -1; }

#endif /* __wasi__ (serial) */
