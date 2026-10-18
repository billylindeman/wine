/*
 * wineweb shim <pthread.h> — passthrough to the real header plus pthread_exit,
 * which wasi-libc never declares (its thread wrapper returns instead), while
 * unix/thread.c calls it from teardown paths. Must never shadow or alter
 * anything the real header defines.
 */
#ifndef __WINEWEB_SHIM_PTHREAD_H
#define __WINEWEB_SHIM_PTHREAD_H

#include_next <pthread.h>

#ifndef __WINEWEB_SHIM_PTHREAD_DECLS
#define __WINEWEB_SHIM_PTHREAD_DECLS
_Noreturn void pthread_exit(void *retval);
#endif

#endif /* __WINEWEB_SHIM_PTHREAD_H */
