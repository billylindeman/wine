/*
 * wineweb shim <sys/ioctl.h> — passthrough to the real header plus the TTY
 * window-size ioctl, which wasip1 declares the struct for but never defines
 * (env.c's terminal-size probe). On wasm there is no terminal to probe; the
 * call fails honestly.
 */
#ifndef __WINEWEB_SHIM_SYS_IOCTL_H
#define __WINEWEB_SHIM_SYS_IOCTL_H

#include_next <sys/ioctl.h>

#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
#endif

#endif /* __WINEWEB_SHIM_SYS_IOCTL_H */
