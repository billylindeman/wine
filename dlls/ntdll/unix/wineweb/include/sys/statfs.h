/*
 * wineweb shim <sys/statfs.h> — wasi lacks the statfs family; file.c uses
 * the type opaquely (the fs-type switch is compiled out on wasm). The struct
 * is the linux shape so any future field use maps onto the documented layout.
 * fstatfs answers honestly: "unknown filesystem" (the VFS reports fs type
 * through bundle metadata later, docs/04 §4 fs group).
 */
#ifndef __WINEWEB_SHIM_SYS_STATFS_H
#define __WINEWEB_SHIM_SYS_STATFS_H

#include <sys/types.h>

struct statfs
{
    long f_type;
    long f_bsize;
    long f_blocks;
    long f_bfree;
    long f_bavail;
    long f_files;
    long f_ffree;
    long f_namelen;
    long f_frsize;
    long f_flags;
    long f_spare[4];
};

int statfs(const char *path, struct statfs *buf);
int fstatfs(int fd, struct statfs *buf);

#endif /* __WINEWEB_SHIM_SYS_STATFS_H */
