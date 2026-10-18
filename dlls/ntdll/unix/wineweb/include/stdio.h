/*
 * wineweb shim <stdio.h> — passthrough plus the GNU asprintf declaration
 * loader.c/server.c use for path building. wasi-libc ships the definition
 * (vasprintf/asprintf) but only declares it under _GNU_SOURCE.
 */
#ifndef __WINEWEB_SHIM_STDIO_H
#define __WINEWEB_SHIM_STDIO_H

#include_next <stdio.h>

#ifndef __WINEWEB_SHIM_STDIO_DECLS
#define __WINEWEB_SHIM_STDIO_DECLS
int asprintf(char **strp, const char *fmt, ...);
#endif

#endif /* __WINEWEB_SHIM_STDIO_H */
