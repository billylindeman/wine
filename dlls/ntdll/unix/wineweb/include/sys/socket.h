/*
 * wineweb shim <sys/socket.h> — the wasi gap-filler for Wine's unix socket
 * code. Includes the real wasi header, then adds what wasip1 leaves out but
 * the ntdll unix socket code (and virtual.c's recvmsg) references: the
 * ancillary data type/macros, the out-of-band constants, the socket-call
 * declarations wasip1 excludes, and the errno codes wasi's errno set lacks
 * (values in a private range — they exist only to be mapped to NT statuses
 * by sock_error and never leave the module).
 *
 * Owned by the wineweb shim (P2.8); must never redefine anything the real
 * header already provides — every addition is guarded.
 */
#ifndef __WINEWEB_SHIM_SYS_SOCKET_H
#define __WINEWEB_SHIM_SYS_SOCKET_H

#include_next <sys/socket.h>

#include <errno.h>
#include <stddef.h>

/* ---- errno values wasi does not define (private range; see file header) --- */
#ifndef ESOCKTNOSUPPORT
#define ESOCKTNOSUPPORT 0x2001
#define EPFNOSUPPORT    0x2002
#define ESHUTDOWN       0x2003
#endif

/* ---- ancillary data (wasi's msghdr exists; cmsghdr does not) -------------- */
#if !defined(CMSG_FIRSTHDR) && !defined(__struct_cmsghdr_defined)
#define __struct_cmsghdr_defined
struct cmsghdr
{
    socklen_t cmsg_len;
    int cmsg_level;
    int cmsg_type;
};

#define __WINEWEB_CMSG_ALIGN(len) (((len) + sizeof (size_t) - 1) & (size_t) ~(sizeof (size_t) - 1))
#define __WINEWEB_CMSG_NXTHDR(mhdr, cmsg)                                   \
    (((char *)(cmsg) + __WINEWEB_CMSG_ALIGN((cmsg)->cmsg_len) +             \
      sizeof (struct cmsghdr) >                                             \
      (char *)(mhdr)->msg_control + (mhdr)->msg_controllen)                 \
     ? (struct cmsghdr *) 0                                                 \
     : (struct cmsghdr *) ((char *)(cmsg) + __WINEWEB_CMSG_ALIGN((cmsg)->cmsg_len)))
#define CMSG_DATA(cmsg) ((void *)((struct cmsghdr *)(cmsg) + 1))
#define CMSG_FIRSTHDR(mhdr)                                                 \
    ((size_t)(mhdr)->msg_controllen >= sizeof (struct cmsghdr)              \
     ? (struct cmsghdr *)(mhdr)->msg_control : (struct cmsghdr *) 0)
#define CMSG_NXTHDR(mhdr, cmsg) __WINEWEB_CMSG_NXTHDR(mhdr, cmsg)
#define CMSG_SPACE(len) (__WINEWEB_CMSG_ALIGN(len) + __WINEWEB_CMSG_ALIGN(sizeof (struct cmsghdr)))
#define CMSG_LEN(len)   (__WINEWEB_CMSG_ALIGN(sizeof (struct cmsghdr)) + (len))
#endif /* !CMSG_FIRSTHDR */

/* ---- constants wasip1's header omits (values = Linux, for familiarity) ---- */
#ifndef SOCK_RAW
#define SOCK_RAW 3
#endif
#ifndef MSG_OOB
#define MSG_OOB 0x0001
#endif
#ifndef SO_KEEPALIVE
#define SO_KEEPALIVE 9
#endif
#ifndef SO_OOBINLINE
#define SO_OOBINLINE 10
#endif
#ifndef SO_NO_CHECK
#define SO_NO_CHECK 11
#endif
#ifndef SO_BROADCAST
#define SO_BROADCAST 6
#endif
#ifndef SO_SNDBUF
#define SO_SNDBUF 7
#endif
#ifndef SO_RCVBUF
#define SO_RCVBUF 8
#endif
#ifndef SO_LINGER
#define SO_LINGER 13
#endif
#ifndef SIOCATMARK
#define SIOCATMARK 0x8907
#endif
#ifndef IPPROTO_ICMPV6
#define IPPROTO_ICMPV6 58
#endif
#ifndef SCM_RIGHTS
#define SCM_RIGHTS 0x01
#endif

/* ---- socket calls wasip1 excludes ----------------------------------------- */
#ifndef __WINEWEB_SHIM_SOCKET_DECLS
#define __WINEWEB_SHIM_SOCKET_DECLS
#ifndef PF_UNIX
#define PF_UNIX 1
#define AF_UNIX PF_UNIX
#endif
int socket (int, int, int);
int socketpair (int, int, int, int [2]);
int connect (int, const struct sockaddr *, socklen_t);
int bind (int, const struct sockaddr *, socklen_t);
int listen (int, int);
int getsockname (int, struct sockaddr *__restrict, socklen_t *__restrict);
int getpeername (int, struct sockaddr *__restrict, socklen_t *__restrict);
int setsockopt (int, int, int, const void *, socklen_t);
ssize_t sendto (int, const void *, size_t, int, const struct sockaddr *, socklen_t);
ssize_t recvfrom (int, void *__restrict, size_t, int, struct sockaddr *__restrict, socklen_t *__restrict);
ssize_t sendmsg (int, const struct msghdr *, int);
ssize_t recvmsg (int, struct msghdr *, int);
#endif

#endif /* __WINEWEB_SHIM_SYS_SOCKET_H */
