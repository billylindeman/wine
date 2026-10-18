/*
 * wineweb shim <dlfcn.h> — passthrough plus dladdr, which wasip1's dlfcn
 * omits (loader.c's init_paths uses it to find the ntdll.so directory — a
 * path that does not exist on wasm). dladdr fails honestly (returns 0).
 */
#ifndef __WINEWEB_SHIM_DLFCN_H
#define __WINEWEB_SHIM_DLFCN_H

#include_next <dlfcn.h>

#ifndef __WINEWEB_SHIM_DLFCN_DECLS
#define __WINEWEB_SHIM_DLFCN_DECLS
typedef struct
{
    const char *dli_fname;
    void *dli_fbase;
    const char *dli_sname;
    void *dli_saddr;
} Dl_info;
int dladdr(const void *addr, Dl_info *info);
#endif

#endif /* __WINEWEB_SHIM_DLFCN_H */
