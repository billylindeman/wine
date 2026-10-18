/*
 * wineweb shim <sys/un.h> — passthrough to the real header plus a POSIX
 * sockaddr_un. wasip1's stub has no sun_path, but server.c lays out the
 * socket address with it (the address is never bound on wasm — connect is a
 * wire shape — yet the layout code must compile). We preempt wasi's private
 * guard so its stub definition is skipped; pinned to wasi-sdk 34.
 */
#ifndef __WINEWEB_SHIM_SYS_UN_H
#define __WINEWEB_SHIM_SYS_UN_H

#define __wasilibc___struct_sockaddr_un_h /* skip wasi's sun_path-less stub */
#include_next <sys/un.h>

struct sockaddr_un
{
    sa_family_t sun_family;
    char sun_path[108];
};

#endif /* __WINEWEB_SHIM_SYS_UN_H */
