/*
 * wineweb shim <sys/wait.h> — wasi has no wait header; Wine's unix sources
 * include it unconditionally (server.c). Process creation is loud-unsupported
 * on the wasm flavor (docs/03 §7); this header exists so the sources compile
 * and so waitpid's honest failure has a real declaration. The status macros
 * are the standard POSIX shapes for any future user.
 */
#ifndef __WINEWEB_SHIM_SYS_WAIT_H
#define __WINEWEB_SHIM_SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG    1
#define WUNTRACED  2

#define WEXITSTATUS(status) (((status) & 0xff00) >> 8)
#define WTERMSIG(status)    ((status) & 0x7f)
#define WSTOPSIG(status)    WEXITSTATUS(status)
#define WIFEXITED(status)   (WTERMSIG(status) == 0)
#define WIFSTOPPED(status)  (((status) & 0xff) == 0x7f)
#define WIFSIGNALED(status) (((signed char)(((status) & 0x7f) + 1) >> 1) > 0)

pid_t waitpid(pid_t pid, int *status, int options);

#endif /* __WINEWEB_SHIM_SYS_WAIT_H */
