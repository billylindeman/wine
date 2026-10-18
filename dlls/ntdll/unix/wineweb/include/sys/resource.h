/*
 * wineweb shim <sys/resource.h> — passthrough to the real header plus the
 * rlimit family. wasip1 ships the header but its non-upstream branch leaves
 * bits/resource.h empty: loader.c's set_max_limit compiles unconditionally,
 * so rlimit must exist. The values are never enforced on wasm (no rlimits in
 * a linear memory); setrlimit succeeds as a no-op bookkeeping call so boot
 * is not killed by a limit the platform structurally cannot hit, and
 * getrlimit reports the recorded values.
 */
#ifndef __WINEWEB_SHIM_SYS_RESOURCE_H
#define __WINEWEB_SHIM_SYS_RESOURCE_H

#include_next <sys/resource.h>

#ifndef RLIMIT_CPU
typedef unsigned long long rlim_t;
struct rlimit { rlim_t rlim_cur; rlim_t rlim_max; };

#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_RSS     5
#define RLIMIT_NPROC   6
#define RLIMIT_NOFILE  7
#define RLIMIT_AS      9
#define RLIM_INFINITY  ((rlim_t)-1)
#define RLIM_SAVED_MAX RLIM_INFINITY
#define RLIM_SAVED_CUR RLIM_INFINITY

int getrlimit(int resource, struct rlimit *rlp);
int setrlimit(int resource, const struct rlimit *rlp);
#endif

#endif /* __WINEWEB_SHIM_SYS_RESOURCE_H */
