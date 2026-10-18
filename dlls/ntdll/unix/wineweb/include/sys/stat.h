/*
 * wineweb shim <sys/stat.h> — passthrough to the real header plus umask,
 * which wasi-libc does not declare (file.c queries the initial umask).
 */
#ifndef __WINEWEB_SHIM_SYS_STAT_H
#define __WINEWEB_SHIM_SYS_STAT_H

#include_next <sys/stat.h>

#ifndef __WINEWEB_SHIM_SYS_STAT_DECLS
#define __WINEWEB_SHIM_SYS_STAT_DECLS
mode_t umask(mode_t mask);
#endif

#endif /* __WINEWEB_SHIM_SYS_STAT_H */
