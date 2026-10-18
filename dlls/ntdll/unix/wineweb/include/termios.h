/*
 * wineweb shim <termios.h> — wasi has no TTY layer; serial.c compiles
 * against the linux shapes. The serial device itself is loud-unsupported
 * (the wasm VFS has no COM ports; NtCreateFile fails at the VFS layer) —
 * these declarations exist so the driver compiles and its absence is an
 * honest errno, never a fabricated line state.
 */
#ifndef __WINEWEB_SHIM_TERMIOS_H
#define __WINEWEB_SHIM_TERMIOS_H

#include <sys/types.h>

#define NCCS 32

struct termios
{
    unsigned int c_iflag;
    unsigned int c_oflag;
    unsigned int c_cflag;
    unsigned int c_lflag;
    unsigned char c_line;
    unsigned char c_cc[NCCS];
    unsigned int c_ispeed;
    unsigned int c_ospeed;
};

/* c_iflag */
#define IGNBRK  0x01
#define BRKINT  0x02
#define IGNPAR  0x04
#define PARMRK  0x08
#define INPCK   0x10
#define ISTRIP  0x20
#define INLCR   0x40
#define IGNCR   0x80
#define ICRNL   0x100
#define IXON    0x400
#define IXANY   0x800
#define IXOFF   0x1000
/* c_oflag */
#define OPOST   0x0001
#define ONLCR   0x0004
/* c_lflag */
#define ISIG    0x0001
#define ICANON  0x0002
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define IEXTEN  0x8000
#define HUPCL   0x0400
#define NOFLSH  0x0080
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
/* c_cflag */
#define CS5     0x00
#define CS6     0x10
#define CS7     0x20
#define CS8     0x30
#define CSIZE   0x30
#define CSTOPB  0x40
#define CREAD   0x80
#define PARENB  0x100
#define PARODD  0x200
#define CLOCAL  0x400
#define CMSPAR  0x40000000
#define CRTSCTS 0x80000000
#define CBAUD   0x100f

/* c_cc indices (linux) */
#define VEOF    4
#define VTIME   5
#define VMIN    6
#define VSTART  8
#define VSTOP   9

/* baud rates (B0..B4000000, linux values) */
#define B0       0
#define B50      1
#define B75      2
#define B110     3
#define B134     4
#define B150     5
#define B200     6
#define B300     7
#define B600     8
#define B1200    9
#define B1800    10
#define B2400    11
#define B4800    12
#define B9600    13
#define B19200   14
#define B38400   15
#define B57600   0x1001
#define B115200  0x1002
#define B230400  0x1003
#define B460800  0x1004
#define B500000  0x1005
#define B576000  0x1006
#define B921600  0x1007
#define B1000000 0x1008
#define B1152000 0x1009
#define B1500000 0x100a
#define B2000000 0x100b
#define B2500000 0x100c
#define B3000000 0x100d
#define B3500000 0x100e
#define B4000000 0x100f

/* modem lines / ioctl requests (linux values) */
#define TIOCM_LE   0x001
#define TIOCM_DTR  0x002
#define TIOCM_RTS  0x004
#define TIOCM_ST   0x008
#define TIOCM_SR   0x010
#define TIOCM_CTS  0x020
#define TIOCM_CAR  0x040
#define TIOCM_RNG  0x080
#define TIOCM_DSR  0x100
#define TIOCMGET   0x5415
#define TIOCMSET   0x5418
#define TIOCMIWAIT 0x545c
#define TIOCSBRK   0x5427
#define TIOCCBRK   0x5428
#define TIOCSER    0x541e
#define TIOCSER_TEMT 0x01
#define TIOCSERGETLSR 0x545e
#define TIOCSSERIAL   0x541f

/* tcflush/tcflow selectors */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2
#define TCOOFF    0
#define TCOON     1
#define TCIOFF    2
#define TCION     3

struct serial_struct { int type; int line; int baud_base; };

int tcgetattr(int fd, struct termios *termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios *termios_p);
int cfsetispeed(struct termios *termios_p, unsigned int speed);
int cfsetospeed(struct termios *termios_p, unsigned int speed);
unsigned int cfgetospeed(const struct termios *termios_p);
int tcdrain(int fd);
int tcflush(int fd, int queue_selector);
int tcflow(int fd, int action);

#endif /* __WINEWEB_SHIM_TERMIOS_H */
