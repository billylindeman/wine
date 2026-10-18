/*
 * wineweb shim <pwd.h> — wasi has no passwd database. loader.c's
 * set_home_dir falls back to the environment when getpwuid returns NULL,
 * which is exactly the wasm behavior (identity comes from the browser
 * context later, docs/04 §4). Honest shape: getpwuid always fails.
 */
#ifndef __WINEWEB_SHIM_PWD_H
#define __WINEWEB_SHIM_PWD_H

#include <sys/types.h>

struct passwd
{
    char *pw_name;
    char *pw_dir;
    char *pw_shell;
};

struct passwd *getpwuid(uid_t uid);

#endif /* __WINEWEB_SHIM_PWD_H */
