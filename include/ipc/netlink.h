/*
 *
 *      netlink.h
 *      Netlink socket family (AF_NETLINK) definitions
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_IPC_NETLINK_H_
#define INCLUDE_IPC_NETLINK_H_

#include <libs/glist/circular_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/*  Netlink socket address (Linux-compatible)                          */
/* ------------------------------------------------------------------ */

typedef struct sockaddr_nl {
        uint16_t nl_family; /* AF_NETLINK */
        uint16_t nl_pad;    /* zero */
        uint32_t nl_pid;    /* port ID (0 = kernel) */
        uint32_t nl_groups; /* multicast groups mask */
} sockaddr_nl_t;

/* ------------------------------------------------------------------ */
/*  Netlink message header (16 bytes, 4-byte aligned)                  */
/* ------------------------------------------------------------------ */

typedef struct nlmsghdr {
        uint32_t nlmsg_len;   /* Length of message including header */
        uint16_t nlmsg_type;  /* Message type */
        uint16_t nlmsg_flags; /* Flags (NLM_F_*) */
        uint32_t nlmsg_seq;   /* Sequence number */
        uint32_t nlmsg_pid;   /* Sending port ID */
} nlmsghdr_t;

#define NLMSG_HDRLEN            ((uint32_t)sizeof(nlmsghdr_t))
#define NLMSG_ALIGNTO           4U
#define NLMSG_ALIGN(len)        (((len) + NLMSG_ALIGNTO - 1) & ~(NLMSG_ALIGNTO - 1))
#define NLMSG_LENGTH(len)       (NLMSG_ALIGN(NLMSG_HDRLEN) + (uint32_t)(len))
#define NLMSG_DATA(nlh)         ((void *)((uint8_t *)(nlh) + NLMSG_ALIGN(NLMSG_HDRLEN)))
#define NLMSG_NEXT(nlh)         ((nlmsghdr_t *)((uint8_t *)(nlh) + NLMSG_ALIGN((nlh)->nlmsg_len)))
#define NLMSG_OK(nlh, len)      ((len) >= (int)sizeof(nlmsghdr_t) && (nlh)->nlmsg_len >= sizeof(nlmsghdr_t) && (nlh)->nlmsg_len <= (uint32_t)(len))
#define NLMSG_PAYLOAD(nlh, len) ((nlh)->nlmsg_len - NLMSG_ALIGN(NLMSG_HDRLEN))

/* ------------------------------------------------------------------ */
/*  Netlink message flags                                              */
/* ------------------------------------------------------------------ */

/* Standard flags */
#define NLM_F_REQUEST       0x0001 /* It is a request message */
#define NLM_F_MULTI         0x0002 /* Multipart message, terminated by NLMSG_DONE */
#define NLM_F_ACK           0x0004 /* Reply with ACK on success */
#define NLM_F_ECHO          0x0008 /* Echo this request */
#define NLM_F_DUMP_INTR     0x0010 /* Dump was inconsistent due to change */
#define NLM_F_DUMP_FILTERED 0x0020 /* Dump was filtered */

/* Modifiers for GET (dump) requests */
#define NLM_F_ROOT   0x0100 /* Specify tree root */
#define NLM_F_MATCH  0x0200 /* Return all matching */
#define NLM_F_ATOMIC 0x0400 /* Atomic GET */
#define NLM_F_DUMP   (NLM_F_ROOT | NLM_F_MATCH)

/* Modifiers for NEW requests (create/replace) */
#define NLM_F_REPLACE 0x0100 /* Replace existing object */
#define NLM_F_EXCL    0x0200 /* Fail if object already exists */
#define NLM_F_CREATE  0x0400 /* Create if it does not exist */
#define NLM_F_APPEND  0x0800 /* Add to end of list */

/* ------------------------------------------------------------------ */
/*  Netlink message types                                              */
/* ------------------------------------------------------------------ */

#define NLMSG_NOOP    0x0001 /* No-op, discard */
#define NLMSG_ERROR   0x0002 /* Error message */
#define NLMSG_DONE    0x0003 /* End of a multipart dump */
#define NLMSG_OVERRUN 0x0004 /* Data lost */

/* ------------------------------------------------------------------ */
/*  Netlink error message (follows nlmsghdr)                           */
/* ------------------------------------------------------------------ */

typedef struct nlmsgerr {
        int32_t    error;
        nlmsghdr_t msg; /* Original message header */
} nlmsgerr_t;

/* ------------------------------------------------------------------ */
/*  Netlink protocol families                                          */
/* ------------------------------------------------------------------ */

#define NETLINK_ROUTE          0  /* Routing/device hook */
#define NETLINK_UNUSED         1  /* Unused */
#define NETLINK_USERSOCK       2  /* Reserved for user-mode socket protocols */
#define NETLINK_FIREWALL       3  /* Unused (was ip_queue) */
#define NETLINK_SOCK_DIAG      4  /* Socket monitoring */
#define NETLINK_NFLOG          5  /* Netfilter/iptables ULOG */
#define NETLINK_XFRM           6  /* IPsec */
#define NETLINK_SELINUX        7  /* SELinux event notifications */
#define NETLINK_ISCSI          8  /* Open-iSCSI */
#define NETLINK_AUDIT          9  /* Auditing */
#define NETLINK_FIB_LOOKUP     10 /* Routing table lookup */
#define NETLINK_CONNECTOR      11 /* Kernel connector */
#define NETLINK_NETFILTER      12 /* Netfilter subsystem */
#define NETLINK_IP6_FW         13 /* IPv6 netfilter (unused) */
#define NETLINK_DNRTMSG        14 /* DECnet routing messages */
#define NETLINK_KOBJECT_UEVENT 15 /* Kernel object events (udev) */
#define NETLINK_GENERIC        16 /* Generic netlink family */
#define NETLINK_SCSITRANSPORT  18 /* SCSI transport */
#define NETLINK_ECRYPTFS       19 /* eCryptfs */
#define NETLINK_RDMA           20 /* RDMA */
#define NETLINK_CRYPTO         21 /* Crypto layer */
#define NETLINK_SMC            22 /* SMC protocol */
#define NETLINK_INET_DIAG      23 /* INET socket monitoring */
#define NETLINK_MAX            24

/* ------------------------------------------------------------------ */
/*  Netlink socket state — per-socket private data                     */
/* ------------------------------------------------------------------ */

#define NL_SOCK_RECV_BUF_SIZE (128 * 1024) /* 128KB default recv buffer */

typedef struct nl_sock {
        uint32_t nl_pid;       /* Our port ID (0 = unbound/kernel) */
        uint32_t nl_groups;    /* Multicast groups subscribed */
        uint32_t nl_protocol;  /* Netlink protocol (NETLINK_*) */
        uint32_t nl_seq;       /* Next outgoing sequence number */
        int      nl_bound : 1; /* Socket is bound */
        int      nl_pad   : 31;

        /* Receive queue: each entry is a complete nlmsghdr-framed message */
        /* stored as a contiguous allocation (header + payload) */
        clist_t    recv_queue;     /* circular list of nl_msg_t */
        uint32_t   recv_queue_len; /* number of messages queued */
        uint32_t   recv_queue_max; /* max messages (prevents DoS) */
        spinlock_t recv_lock;      /* protects recv_queue */

        /* Blocking support */
        void *blocked_task; /* task_t waiting on recv */

        /* Back-pointer to the generic socket */
        struct socket *sk;
} nl_sock_t;

/* ------------------------------------------------------------------ */
/*  Internal: one queued netlink message                               */
/* ------------------------------------------------------------------ */

typedef struct nl_msg {
        uint8_t *data;     /* nlmsghdr + payload */
        uint32_t len;      /* total length */
        uint32_t refcount; /* for potential shared delivery */
} nl_msg_t;

/* ------------------------------------------------------------------ */
/*  Global multicast table entry                                       */
/* ------------------------------------------------------------------ */

#define NL_MAX_MULTICAST_GROUPS 32 /* groups 0-31 */

/* ------------------------------------------------------------------ */
/*  Kernel netlink API                                                 */
/* ------------------------------------------------------------------ */

/* Initialise the netlink subsystem. Must be called before socket_init(). */
void netlink_init(void);

/*
 * Broadcast a netlink message to all sockets subscribed to the given
 * protocol and multicast group.
 *
 * @protocol: NETLINK_* family
 * @group:    multicast group bit (1 << N) or combination
 * @data:     pointer to nlmsghdr + payload
 * @len:      total message length
 * @flags:    allocation flags (0 = may sleep, 1 = atomic/GFP_ATOMIC)
 *
 * Returns number of sockets the message was delivered to, or negative errno.
 */
int netlink_broadcast(uint32_t protocol, uint32_t group, const void *data, uint32_t len, int flags);

/*
 * Send a unicast netlink message to a specific socket.
 *
 * @sk:   destination socket (nl_sock_t cast to socket_t)
 * @data: pointer to nlmsghdr + payload
 * @len:  total message length
 * @flags: allocation flags
 *
 * Returns 0 on success or negative errno.
 */
int netlink_unicast(struct socket *sk, const void *data, uint32_t len, int flags);

/*
 * Check if any process is listening on the given protocol+group multicast.
 * Returns non-zero if listeners exist, 0 otherwise.
 */
int netlink_has_listeners(uint32_t protocol, uint32_t group);

/* ------------------------------------------------------------------ */
/*  Internal: socket operations called from ipc/socket.c               */
/* ------------------------------------------------------------------ */

struct socket;

/* Allocate and initialise a new netlink socket */
struct socket *netlink_sock_alloc(uint32_t protocol);

/* Netlink-specific bind */
int netlink_bind(struct socket *sk, const sockaddr_nl_t *addr, uint32_t addrlen);

/* Netlink-specific sendmsg */
int netlink_sendmsg(struct socket *sk, const void *buf, size_t len, const sockaddr_nl_t *addr, uint32_t addrlen, int flags);

/* Netlink-specific recvmsg */
int netlink_recvmsg(struct socket *sk, void *buf, size_t len, sockaddr_nl_t *addr, uint32_t *addrlen, int flags);

/* Netlink-specific close cleanup */
void netlink_close(struct socket *sk);

/* Netlink-specific poll */
int netlink_poll(struct socket *sk, size_t events);

/* Netlink-specific setsockopt */
int netlink_setsockopt(struct socket *sk, int optname, const void *optval, uint32_t optlen);

/* Netlink-specific getsockopt */
int netlink_getsockopt(struct socket *sk, int optname, void *optval, uint32_t *optlen);

#endif /* INCLUDE_IPC_NETLINK_H_ */
