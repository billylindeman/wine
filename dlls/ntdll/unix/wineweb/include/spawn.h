/*
 * wineweb shim <spawn.h> — wasi has no posix_spawn; loader.c calls it for
 * the preloader re-exec path (loud-unsupported on wasm, docs/03 §7). The
 * spawnattr/file_actions types are opaque: only the pointer identity and the
 * init/destroy calls appear (and the attr calls only inside __APPLE__).
 */
#ifndef __WINEWEB_SHIM_SPAWN_H
#define __WINEWEB_SHIM_SPAWN_H

#include <sys/types.h>

typedef struct { int __unused; } posix_spawnattr_t;
typedef struct { int __unused; } posix_spawn_file_actions_t;

#define POSIX_SPAWN_RESETIDS   0x01
#define POSIX_SPAWN_SETPGROUP  0x02
#define POSIX_SPAWN_SETSIGDEF  0x04
#define POSIX_SPAWN_SETSIGMASK 0x08
#define POSIX_SPAWN_SETEXEC    0x40
#define POSIX_SPAWN_USEVFORK   0x40

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const argv[], char *const envp[]);
int posix_spawnattr_init(posix_spawnattr_t *attr);
int posix_spawnattr_destroy(posix_spawnattr_t *attr);
int posix_spawnattr_setflags(posix_spawnattr_t *attr, short flags);

#endif /* __WINEWEB_SHIM_SPAWN_H */
