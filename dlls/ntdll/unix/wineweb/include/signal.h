/*
 * wineweb shim <signal.h> — passthrough to the wasi emulated-signal header
 * plus the POSIX surface its non-upstream branch omits but Wine's unix side
 * uses: sigset_t (wasi only defines it under _POSIX_SOURCE-family defines we
 * do not set — and its value is a documented placeholder), the mask
 * how-values, sigaction, and the alternate-signal-stack type/call. Masking
 * and handler registration are no-ops on wasm and sigaltstack fails honestly
 * (docs/02 §7: no async signals; exception dispatch is ours).
 */
#ifndef __WINEWEB_SHIM_SIGNAL_H
#define __WINEWEB_SHIM_SIGNAL_H

#include_next <signal.h>

#include <__typedef_sigset_t.h> /* wasi's documented sigset_t placeholder */

#ifndef SIG_BLOCK
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif

#ifndef SIGPIPE
#define SIGPIPE 13
#endif

struct sigaction
{
    void (*sa_handler)(int);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};

#define SA_NOCLDSTOP 1
#define SA_NOCLDWAIT 2
#define SA_SIGINFO   4
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
int sigemptyset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);

#ifndef SS_ONSTACK
#define SS_ONSTACK 1
#define SS_DISABLE 2
typedef struct { void *ss_sp; size_t ss_size; int ss_flags; } stack_t;
int sigaltstack(const stack_t *ss, stack_t *old_ss);
#endif

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset);

#endif /* __WINEWEB_SHIM_SIGNAL_H */
