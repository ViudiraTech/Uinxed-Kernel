/*
 *
 *      socket.h
 *      BSD Socket API definitions header file
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SOCKET_H_
#define INCLUDE_SOCKET_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/*  Address families                                                   */
/* ------------------------------------------------------------------ */

#define AF_UNSPEC  0
#define AF_UNIX    1
#define AF_LOCAL   1
#define AF_INET    2
#define AF_INET6   10
#define AF_NETLINK 16

/* ------------------------------------------------------------------ */
/*  Socket types                                                       */
/* ------------------------------------------------------------------ */

#define SOCK_STREAM    1
#define SOCK_DGRAM     2
#define SOCK_RAW       3
#define SOCK_SEQPACKET 5
#define SOCK_NONBLOCK  0x800
#define SOCK_CLOEXEC   0x80000

/* ------------------------------------------------------------------ */
/*  Socket-level options (SOL_SOCKET)                                   */
/* ------------------------------------------------------------------ */

#define SOL_SOCKET 1

#define SO_DEBUG       1
#define SO_REUSEADDR   2
#define SO_TYPE        3
#define SO_ERROR       4
#define SO_DONTROUTE   5
#define SO_BROADCAST   6
#define SO_SNDBUF      7
#define SO_RCVBUF      8
#define SO_KEEPALIVE   9
#define SO_OOBINLINE   10
#define SO_LINGER      13
#define SO_PEERCRED    17
#define SO_RCVLOWAT    18
#define SO_SNDLOWAT    19
#define SO_RCVTIMEO    20
#define SO_SNDTIMEO    21
#define SO_ACCEPTCONN  30
#define SO_PASSCRED    16
#define SO_PEERSEC     31
#define SO_SNDBUFFORCE 32
#define SO_RCVBUFFORCE 33
#define SO_PROTOCOL    38
#define SO_DOMAIN      39

/* ------------------------------------------------------------------ */
/*  Message flags                                                      */
/* ------------------------------------------------------------------ */

#define MSG_OOB          0x0001
#define MSG_PEEK         0x0002
#define MSG_DONTROUTE    0x0004
#define MSG_CTRUNC       0x0008
#define MSG_PROXY        0x0010
#define MSG_TRUNC        0x0020
#define MSG_DONTWAIT     0x0040
#define MSG_EOR          0x0080
#define MSG_WAITALL      0x0100
#define MSG_FIN          0x0200
#define MSG_SYN          0x0400
#define MSG_CONFIRM      0x0800
#define MSG_RST          0x1000
#define MSG_ERRQUEUE     0x2000
#define MSG_NOSIGNAL     0x4000
#define MSG_MORE         0x8000
#define MSG_CMSG_CLOEXEC 0x40000000

/* ------------------------------------------------------------------ */
/*  Shutdown how                                                       */
/* ------------------------------------------------------------------ */

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

/* ------------------------------------------------------------------ */
/*  CMSG / ancillary data                                              */
/* ------------------------------------------------------------------ */

#define SCM_RIGHTS      0x01
#define SCM_CREDENTIALS 0x02

/* ------------------------------------------------------------------ */
/*  struct sockaddr - generic socket address                            */
/* ------------------------------------------------------------------ */

typedef struct sockaddr {
        uint16_t sa_family;
        char     sa_data[14];
} sockaddr_t;

/* ------------------------------------------------------------------ */
/*  struct sockaddr_un - UNIX domain socket address                     */
/* ------------------------------------------------------------------ */

#define UNIX_PATH_MAX 108

typedef struct sockaddr_un {
        uint16_t sun_family;
        char     sun_path[UNIX_PATH_MAX];
} sockaddr_un_t;

/* ------------------------------------------------------------------ */
/*  struct msghdr - message header for sendmsg/recvmsg                  */
/* ------------------------------------------------------------------ */

typedef struct iovec {
        void  *iov_base;
        size_t iov_len;
} iovec_t;

typedef struct msghdr {
        void         *msg_name;
        uint32_t      msg_namelen;
        struct iovec *msg_iov;
        size_t        msg_iovlen;
        void         *msg_control;
        size_t        msg_controllen;
        int           msg_flags;
} msghdr_t;

typedef struct cmsghdr {
        size_t cmsg_len;
        int    cmsg_level;
        int    cmsg_type;
} cmsghdr_t;

#define CMSG_ALIGN(len)     (((len) + sizeof(size_t) - 1) & (size_t) ~(sizeof(size_t) - 1))
#define CMSG_DATA(cmsg)     ((void *)((uint8_t *)(cmsg) + sizeof(cmsghdr_t)))
#define CMSG_FIRSTHDR(mhdr) ((mhdr)->msg_controllen >= sizeof(cmsghdr_t) ? (cmsghdr_t *)(mhdr)->msg_control : NULL)

/* ------------------------------------------------------------------ */
/*  struct linger                                                      */
/* ------------------------------------------------------------------ */

typedef struct linger {
        int l_onoff;
        int l_linger;
} linger_t;

/* ------------------------------------------------------------------ */
/*  struct ucred - user credentials                                     */
/* ------------------------------------------------------------------ */

typedef struct ucred {
        uint32_t pid;
        uint32_t uid;
        uint32_t gid;
} ucred_t;

/* ------------------------------------------------------------------ */
/*  Socket state machine                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    SOCK_STATE_FREE,
    SOCK_STATE_UNCONNECTED,
    SOCK_STATE_LISTENING,
    SOCK_STATE_CONNECTING,
    SOCK_STATE_CONNECTED,
    SOCK_STATE_DISCONNECTING,
} socket_state_t;

/* ------------------------------------------------------------------ */
/*  Socket internal buffer                                              */
/* ------------------------------------------------------------------ */

#define SOCK_BUF_SIZE 65536
#define SOCK_BUF_MAX  262144

typedef struct sock_buf {
        uint8_t   *data;
        uint32_t   head;
        uint32_t   tail;
        uint32_t   size;
        uint32_t   capacity;
        spinlock_t lock;
} sock_buf_t;

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

typedef struct socket    socket_t;
typedef struct sock_peer sock_peer_t;

/* ------------------------------------------------------------------ */
/*  Socket structure                                                    */
/* ------------------------------------------------------------------ */

struct socket {
        socket_state_t state;
        uint16_t       family;
        uint16_t       type;
        uint16_t       protocol;
        uint32_t       flags;

        /* Buffers */
        sock_buf_t recv_buf;
        sock_buf_t send_buf;

        /* Peer */
        socket_t     *peer;
        sockaddr_un_t local_addr;
        sockaddr_un_t peer_addr;
        uint32_t      local_addr_len;
        uint32_t      peer_addr_len;

        /* Connection queue (for listening sockets) */
        socket_t **accept_queue;
        uint32_t   accept_queue_len;
        uint32_t   accept_queue_cap;
        uint32_t   backlog;
        spinlock_t accept_lock;

        /* Shutdown */
        uint32_t shutdown_mask;

        /* Options */
        uint32_t sndbuf;
        uint32_t rcvbuf;
        uint32_t rcvlowat;
        uint32_t sndlowat;
        uint32_t linger_on;
        uint32_t linger_time;
        int      passcred;
        int      reuseaddr;

        /* Credentials */
        uint32_t pid;
        uint32_t uid;
        uint32_t gid;

        /* Error */
        int so_error;

        /* VFS */
        struct vfs_node *node;
        uint32_t         refcount;

        /* Polymorphic operations */
        int (*socket_read)(socket_t *sk, void *buf, size_t sz, void *addr, uint32_t *addrlen);
        int (*socket_write)(socket_t *sk, const void *buf, size_t sz, const void *addr, uint32_t addrlen);
        int (*socket_poll)(socket_t *sk, size_t events);
        int (*socket_close)(socket_t *sk);

        /* Lock */
        spinlock_t lock;
};

/* ------------------------------------------------------------------ */
/*  Socket system call interface                                       */
/* ------------------------------------------------------------------ */

int64_t sys_socket(uint32_t family, uint32_t type, uint32_t protocol);
int64_t sys_bind(int fd, const sockaddr_un_t *addr, uint32_t addrlen);
int64_t sys_listen(int fd, int backlog);
int64_t sys_accept(int fd, sockaddr_un_t *addr, uint32_t *addrlen, int flags);
int64_t sys_connect(int fd, const sockaddr_un_t *addr, uint32_t addrlen);
int64_t sys_sendto(int fd, const void *buf, size_t len, int flags, const sockaddr_un_t *addr, uint32_t addrlen);
int64_t sys_recvfrom(int fd, void *buf, size_t len, int flags, sockaddr_un_t *addr, uint32_t *addrlen);
int64_t sys_sendmsg(int fd, const msghdr_t *msg, int flags);
int64_t sys_recvmsg(int fd, msghdr_t *msg, int flags);
int64_t sys_shutdown(int fd, int how);
int64_t sys_socketpair(int domain, int type, int protocol, int sv[2]);
int64_t sys_getsockname(int fd, sockaddr_un_t *addr, uint32_t *addrlen);
int64_t sys_getpeername(int fd, sockaddr_un_t *addr, uint32_t *addrlen);
int64_t sys_setsockopt(int fd, int level, int optname, const void *optval, uint32_t optlen);
int64_t sys_getsockopt(int fd, int level, int optname, void *optval, uint32_t *optlen);

/* Socket initialization */
void socket_init(void);

/* Attach a socket to a process fd table */
int socket_fd_install(socket_t *sk);

/* Find a socket by fd in the current process */
socket_t *socket_from_fd(int fd);

/* Sendmmsg/recvmmsg */
int64_t sys_sendmmsg(int fd, void *msgvec, uint32_t vlen, int flags);
int64_t sys_recvmmsg(int fd, void *msgvec, uint32_t vlen, int flags, void *timeout);

#endif /* INCLUDE_SOCKET_H_ */