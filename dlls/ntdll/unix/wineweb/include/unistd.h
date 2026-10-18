/*
 * wineweb shim <unistd.h> — passthrough to the real header plus the calls
 * Wine's unix sources use that wasi-libc does not declare (and the ones it
 * deprecates behind emulation flags we do set). Must never shadow or alter
 * anything the real header defines.
 */
#ifndef __WINEWEB_SHIM_UNISTD_H
#define __WINEWEB_SHIM_UNISTD_H

#include_next <unistd.h>

#ifndef __WINEWEB_SHIM_UNISTD_DECLS
#define __WINEWEB_SHIM_UNISTD_DECLS
int fchdir(int fd);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
pid_t fork(void);
pid_t setsid(void);
pid_t getpid(void);
uid_t getuid(void);
#endif

#endif /* __WINEWEB_SHIM_UNISTD_H */
