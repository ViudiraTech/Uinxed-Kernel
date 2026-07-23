/*
 *
 *      netlink.c
 *      Netlink socket family (AF_NETLINK) implementation
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/vfs.h>
#include <ipc/netlink.h>
#include <ipc/socket.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/glist/circular_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/task.h>
#include <proc/uaccess.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define NL_RECV_QUEUE_MAX 256 /* max messages queued per socket */
#define NL_BROADCAST_MAX  64  /* max sockets in a multicast group */
#define NL_PROTO_MAX      32  /* NETLINK_MAX rounded up */

/* ------------------------------------------------------------------ */
/*  Multicast group entry                                               */
/* ------------------------------------------------------------------ */

typedef struct nl_mcast_entry {
        struct socket *sk;     /* subscriber socket */
        uint32_t       groups; /* subscribed groups bitmask for this socket */
} nl_mcast_entry_t;

/* ------------------------------------------------------------------ */
/*  Per-protocol multicast table                                        */
/* ------------------------------------------------------------------ */

typedef struct nl_mcast_table {
        nl_mcast_entry_t entries[NL_BROADCAST_MAX];
        uint32_t         count;
        spinlock_t       lock;
} nl_mcast_table_t;

static nl_mcast_table_t nl_mcast[NL_PROTO_MAX];

/* ------------------------------------------------------------------ */
/*  Auto-assigned port ID counter                                       */
/* ------------------------------------------------------------------ */

static uint32_t   nl_pid_counter;
static spinlock_t nl_pid_lock;

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static nl_sock_t *nl_sk(struct socket *sk)
{
    if (!sk || !sk->priv) return NULL;
    return (nl_sock_t *)sk->priv;
}

static nl_msg_t *nl_msg_alloc(const void *data, uint32_t len)
{
    nl_msg_t *msg;

    if (!data || len < NLMSG_HDRLEN) return NULL;

    msg = calloc(1, sizeof(nl_msg_t));
    if (!msg) return NULL;

    msg->data = malloc(len);
    if (!msg->data) {
        free(msg);
        return NULL;
    }

    memcpy(msg->data, data, len);
    msg->len      = len;
    msg->refcount = 1;
    return msg;
}

static void nl_msg_free(nl_msg_t *msg)
{
    if (!msg) return;
    if (msg->data) free(msg->data);
    free(msg);
}

static void nl_msg_put(nl_msg_t *msg)
{
    if (!msg) return;
    if (--msg->refcount == 0) nl_msg_free(msg);
}

/* Assign a unique port ID for auto-bind */
static uint32_t nl_alloc_pid(void)
{
    uint32_t pid;

    spin_lock(&nl_pid_lock);
    pid = ++nl_pid_counter;
    if (pid == 0) pid = ++nl_pid_counter; /* never assign 0 (reserved for kernel) */
    spin_unlock(&nl_pid_lock);

    return pid;
}

/* Find a socket by PID in a protocol's multicast table */
static struct socket *nl_mcast_find_by_pid(uint32_t protocol, uint32_t pid)
{
    nl_mcast_table_t *tab;

    if (protocol >= NL_PROTO_MAX) return NULL;
    tab = &nl_mcast[protocol];

    spin_lock(&tab->lock);
    for (uint32_t i = 0; i < tab->count; i++) {
        if (tab->entries[i].sk) {
            nl_sock_t *ns = nl_sk(tab->entries[i].sk);
            if (ns && ns->nl_pid == pid) {
                struct socket *sk = tab->entries[i].sk;
                spin_unlock(&tab->lock);
                return sk;
            }
        }
    }
    spin_unlock(&tab->lock);
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Multicast subscription management                                  */
/* ------------------------------------------------------------------ */

static int nl_mcast_subscribe(uint32_t protocol, struct socket *sk, uint32_t groups)
{
    nl_mcast_table_t *tab;

    if (protocol >= NL_PROTO_MAX) return -EPROTONOSUPPORT;

    /* Only the groups subscribed via bind; store them */
    nl_sock_t *ns = nl_sk(sk);
    if (ns) ns->nl_groups = groups;

    tab = &nl_mcast[protocol];

    spin_lock(&tab->lock);

    /* Check if already subscribed */
    for (uint32_t i = 0; i < tab->count; i++) {
        if (tab->entries[i].sk == sk) {
            tab->entries[i].groups = groups;
            spin_unlock(&tab->lock);
            return EOK;
        }
    }

    /* New subscription */
    if (tab->count >= NL_BROADCAST_MAX) {
        spin_unlock(&tab->lock);
        return -ENOBUFS;
    }

    tab->entries[tab->count].sk     = sk;
    tab->entries[tab->count].groups = groups;
    tab->count++;
    spin_unlock(&tab->lock);

    return EOK;
}

static void nl_mcast_unsubscribe(uint32_t protocol, struct socket *sk)
{
    nl_mcast_table_t *tab;

    if (protocol >= NL_PROTO_MAX) return;
    tab = &nl_mcast[protocol];

    spin_lock(&tab->lock);
    for (uint32_t i = 0; i < tab->count; i++) {
        if (tab->entries[i].sk == sk) {
            /* Remove by shifting remaining entries */
            if (i < tab->count - 1) { memmove(&tab->entries[i], &tab->entries[i + 1], (tab->count - i - 1) * sizeof(nl_mcast_entry_t)); }
            tab->count--;
            break;
        }
    }
    spin_unlock(&tab->lock);
}

/* ------------------------------------------------------------------ */
/*  Forward declarations for wrapper functions                          */
/* ------------------------------------------------------------------ */

static int netlink_wrap_read(struct socket *sk, void *buf, size_t sz, void *addr, uint32_t *addrlen);
static int netlink_wrap_write(struct socket *sk, const void *buf, size_t sz, const void *addr, uint32_t addrlen);
static int netlink_wrap_poll(struct socket *sk, size_t events);
static int netlink_wrap_close(struct socket *sk);

/* ------------------------------------------------------------------ */
/*  Socket lifecycle                                                   */
/* ------------------------------------------------------------------ */

struct socket *netlink_sock_alloc(uint32_t protocol)
{
    struct socket *sk;
    nl_sock_t     *ns;

    if (protocol >= NL_PROTO_MAX) return NULL;

    sk = calloc(1, sizeof(struct socket));
    if (!sk) return NULL;

    ns = calloc(1, sizeof(nl_sock_t));
    if (!ns) {
        free(sk);
        return NULL;
    }

    /* Initialise generic socket fields */
    sk->state    = SOCK_STATE_UNCONNECTED;
    sk->family   = AF_NETLINK;
    sk->type     = SOCK_DGRAM; /* netlink is datagram-oriented */
    sk->protocol = (uint16_t)protocol;
    sk->flags    = 0;
    sk->refcount = 1;
    sk->priv     = ns;

    /* Wrapper functions to match socket_t polymorphic signatures */
    /* (socket_read takes 5 params; netlink ops take 6 with flags) */
    sk->socket_read  = netlink_wrap_read;
    sk->socket_write = netlink_wrap_write;
    sk->socket_poll  = netlink_wrap_poll;
    sk->socket_close = netlink_wrap_close;

    /* Initialise netlink-specific fields */
    ns->nl_pid         = 0; /* unbound */
    ns->nl_groups      = 0;
    ns->nl_protocol    = protocol;
    ns->nl_seq         = 0;
    ns->nl_bound       = 0;
    ns->recv_queue     = NULL;
    ns->recv_queue_len = 0;
    ns->recv_queue_max = NL_RECV_QUEUE_MAX;
    ns->sk             = sk;

    return sk;
}

void netlink_close(struct socket *sk)
{
    nl_sock_t *ns;
    clist_t    node;
    clist_t    next;

    if (!sk) return;

    ns = nl_sk(sk);
    if (!ns) return;

    /* Unsubscribe from multicast */
    nl_mcast_unsubscribe(ns->nl_protocol, sk);

    /* Free receive queue */
    spin_lock(&ns->recv_lock);
    for (node = ns->recv_queue; node; node = next) {
        next          = node->next;
        nl_msg_t *msg = node->data;
        nl_msg_put(msg);
    }
    ns->recv_queue     = clist_free(ns->recv_queue);
    ns->recv_queue_len = 0;
    spin_unlock(&ns->recv_lock);

    sk->priv = NULL;
    free(ns);
}

/* ------------------------------------------------------------------ */
/*  Bind                                                               */
/* ------------------------------------------------------------------ */

int netlink_bind(struct socket *sk, const sockaddr_nl_t *addr, uint32_t addrlen)
{
    nl_sock_t *ns;

    if (!sk || !addr) return -EINVAL;
    if (addrlen < sizeof(sockaddr_nl_t)) return -EINVAL;
    if (addr->nl_family != AF_NETLINK) return -EAFNOSUPPORT;

    ns = nl_sk(sk);
    if (!ns) return -EINVAL;

    spin_lock(&sk->lock);

    if (ns->nl_bound) {
        spin_unlock(&sk->lock);
        return -EINVAL; /* already bound */
    }

    /* Set or auto-assign port ID */
    if (addr->nl_pid == 0) {
        ns->nl_pid = nl_alloc_pid();
    } else {
        /* Check if PID is already in use */
        struct socket *existing = nl_mcast_find_by_pid(ns->nl_protocol, addr->nl_pid);
        if (existing && existing != sk) {
            spin_unlock(&sk->lock);
            return -EADDRINUSE;
        }
        ns->nl_pid = addr->nl_pid;
    }

    /* Subscribe to multicast groups */
    if (addr->nl_groups) {
        int ret = nl_mcast_subscribe(ns->nl_protocol, sk, addr->nl_groups);
        if (ret != EOK) {
            spin_unlock(&sk->lock);
            return ret;
        }
        ns->nl_groups = addr->nl_groups;
    }

    ns->nl_bound = 1;
    spin_unlock(&sk->lock);

    return EOK;
}

/* ------------------------------------------------------------------ */
/*  Sendmsg                                                            */
/* ------------------------------------------------------------------ */

int netlink_sendmsg(struct socket *sk, const void *buf, size_t len, const sockaddr_nl_t *addr, uint32_t addrlen, int flags)
{
    nl_sock_t  *ns;
    nlmsghdr_t *nlh;
    uint32_t    nlhdr_len;

    (void)flags;

    if (!sk || !buf) return -EINVAL;
    if (len < NLMSG_HDRLEN) return -EINVAL;

    ns = nl_sk(sk);
    if (!ns) return -EINVAL;

    nlh = (nlmsghdr_t *)buf;

    /* Validate the header */
    if (nlh->nlmsg_len < NLMSG_HDRLEN) return -EINVAL;
    if (nlh->nlmsg_len > len) return -EINVAL;

    nlhdr_len = nlh->nlmsg_len;

    /* Kernel (pid=0) is always allowed */
    /* Userspace sends: nl_pid must be set to the sender's pid */
    if (addr && addrlen >= sizeof(sockaddr_nl_t)) {
        /* Send to a specific destination */
        uint32_t       dest_pid = addr->nl_pid;
        struct socket *dest_sk;

        if (dest_pid == 0) {
            /* Sending to kernel — deliver to protocol handler */
            /* For now, kernel-side netlink receive is stubbed */
            return (int)nlhdr_len;
        }

        dest_sk = nl_mcast_find_by_pid(ns->nl_protocol, dest_pid);
        if (!dest_sk) return -ECONNREFUSED;

        return netlink_unicast(dest_sk, buf, nlhdr_len, 0);
    }

    /* No destination — multicast if groups are set, else error */
    if (addrlen == 0 || !addr) {
        /* Send as a request to kernel (pid=0) */
        /* Kernel-side receive not implemented; accept silently */
        return (int)nlhdr_len;
    }

    return (int)nlhdr_len;
}

/* ------------------------------------------------------------------ */
/*  Recvmsg                                                            */
/* ------------------------------------------------------------------ */

int netlink_recvmsg(struct socket *sk, void *buf, size_t len, sockaddr_nl_t *addr, uint32_t *addrlen, int flags)
{
    nl_sock_t *ns;
    nl_msg_t  *msg;
    int        is_nonblock;
    int        peek;
    uint32_t   copy_len;
    int        ret;

    if (!sk || !buf) return -EINVAL;

    ns = nl_sk(sk);
    if (!ns) return -EINVAL;

    is_nonblock = (flags & MSG_DONTWAIT) ? 1 : 0;
    peek        = (flags & MSG_PEEK) ? 1 : 0;

    spin_lock(&ns->recv_lock);

    /* Wait for messages */
    while (ns->recv_queue_len == 0) {
        if (is_nonblock) {
            spin_unlock(&ns->recv_lock);
            return -EAGAIN;
        }

        /* Block until a message arrives */
        /* Register with the socket's blocked-task tracking */
        spin_unlock(&ns->recv_lock);

        /* Use the socket's own blocking mechanism */
        spin_lock(&sk->lock);
        /* Check again after re-acquiring lock */
        spin_lock(&ns->recv_lock);
        if (ns->recv_queue_len > 0) {
            spin_unlock(&ns->recv_lock);
            spin_unlock(&sk->lock);
            goto dequeue;
        }

        /* Block current task on this socket.
                 * Keep recv_lock held while setting blocked_task so that
                 * netlink_broadcast/unicast can't miss the wakeup. */
        ns->blocked_task = current_task();
        spin_unlock(&ns->recv_lock);
        spin_unlock(&sk->lock);
        task_block();
        spin_lock(&sk->lock);
        spin_lock(&ns->recv_lock);
        ns->blocked_task = NULL;
        spin_unlock(&sk->lock);
    }

dequeue:
    /* Get the first message */
    {
        clist_t head = clist_head(ns->recv_queue);
        msg          = head ? head->data : NULL;
    }

    if (!msg) {
        spin_unlock(&ns->recv_lock);
        return -ENOMSG;
    }

    /* Copy to user */
    copy_len = msg->len;
    if (copy_len > len) copy_len = (uint32_t)len;

    memcpy(buf, msg->data, copy_len);

    /* Fill in sender address */
    if (addr && addrlen) {
        sockaddr_nl_t saddr;
        nlmsghdr_t   *nlh = (nlmsghdr_t *)msg->data;

        memset(&saddr, 0, sizeof(saddr));
        saddr.nl_family = AF_NETLINK;
        saddr.nl_pid    = nlh->nlmsg_pid;
        saddr.nl_groups = 0;

        uint32_t kaddrlen = sizeof(sockaddr_nl_t);
        uint32_t userlen;
        if (copy_from_user(&userlen, addrlen, sizeof(uint32_t))) {
            spin_unlock(&ns->recv_lock);
            return -EFAULT;
        }
        if (kaddrlen > userlen) kaddrlen = userlen;
        if (copy_to_user(addr, &saddr, kaddrlen)) {
            spin_unlock(&ns->recv_lock);
            return -EFAULT;
        }
        if (copy_to_user(addrlen, &kaddrlen, sizeof(uint32_t))) {
            spin_unlock(&ns->recv_lock);
            return -EFAULT;
        }
    }

    if (!peek) {
        /* Remove from queue */
        ns->recv_queue = clist_delete(ns->recv_queue, msg);
        ns->recv_queue_len--;
        nl_msg_put(msg);
    }

    spin_unlock(&ns->recv_lock);

    ret = (int)copy_len;
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Poll                                                               */
/* ------------------------------------------------------------------ */

int netlink_poll(struct socket *sk, size_t events)
{
    nl_sock_t *ns;
    int        revents = 0;

    if (!sk) return 0;

    ns = nl_sk(sk);
    if (!ns) return 0;

    spin_lock(&ns->recv_lock);

    if (ns->recv_queue_len > 0) {
        if (events & 0x0001) revents |= 0x0001; /* POLLIN */
    }
    /* Netlink sockets are always writable (dgram) */
    if (events & 0x0004) revents |= 0x0004; /* POLLOUT */

    spin_unlock(&ns->recv_lock);

    return revents & (int)events;
}

/* ------------------------------------------------------------------ */
/*  setsockopt / getsockopt                                            */
/* ------------------------------------------------------------------ */

#define SOL_NETLINK 270

#define NETLINK_ADD_MEMBERSHIP  1
#define NETLINK_DROP_MEMBERSHIP 2
#define NETLINK_PKTINFO         3
#define NETLINK_BROADCAST_ERROR 4
#define NETLINK_NO_ENOBUFS      5
#define NETLINK_LISTEN_ALL_NSID 8
#define NETLINK_CAP_ACK         10

int netlink_setsockopt(struct socket *sk, int optname, const void *optval, uint32_t optlen)
{
    nl_sock_t *ns;
    int        ival;

    if (!sk) return -EBADF;

    ns = nl_sk(sk);
    if (!ns) return -EINVAL;

    switch (optname) {
        case NETLINK_ADD_MEMBERSHIP : {
            if (optlen < sizeof(int)) return -EINVAL;
            if (copy_from_user(&ival, optval, sizeof(int))) return -EFAULT;
            if (ival < 0 || ival >= 32) return -EINVAL;

            spin_lock(&sk->lock);
            ns->nl_groups |= (1U << (uint32_t)ival);
            nl_mcast_subscribe(ns->nl_protocol, sk, ns->nl_groups);
            spin_unlock(&sk->lock);
            return EOK;
        }

        case NETLINK_DROP_MEMBERSHIP : {
            if (optlen < sizeof(int)) return -EINVAL;
            if (copy_from_user(&ival, optval, sizeof(int))) return -EFAULT;
            if (ival < 0 || ival >= 32) return -EINVAL;

            spin_lock(&sk->lock);
            ns->nl_groups &= ~(1U << (uint32_t)ival);
            nl_mcast_subscribe(ns->nl_protocol, sk, ns->nl_groups);
            spin_unlock(&sk->lock);
            return EOK;
        }

        case NETLINK_NO_ENOBUFS :
        case NETLINK_BROADCAST_ERROR :
        case NETLINK_CAP_ACK :
            /* Accept but ignore */
            return EOK;

        default :
            return -ENOPROTOOPT;
    }
}

int netlink_getsockopt(struct socket *sk, int optname, void *optval, uint32_t *optlen)
{
    nl_sock_t *ns;
    int        ival;
    uint32_t   koptlen;

    if (!sk) return -EBADF;
    if (!optval || !optlen) return -EINVAL;

    ns = nl_sk(sk);
    if (!ns) return -EINVAL;

    switch (optname) {
        case NETLINK_PKTINFO :
            ival    = 0;
            koptlen = sizeof(int);
            break;

        case NETLINK_BROADCAST_ERROR :
            ival    = 1;
            koptlen = sizeof(int);
            break;

        case NETLINK_NO_ENOBUFS :
            ival    = 1;
            koptlen = sizeof(int);
            break;

        case NETLINK_LISTEN_ALL_NSID :
            ival    = 0;
            koptlen = sizeof(int);
            break;

        default :
            return -ENOPROTOOPT;
    }

    if (copy_to_user(optval, &ival, sizeof(int))) return -EFAULT;
    if (copy_to_user(optlen, &koptlen, sizeof(uint32_t))) return -EFAULT;
    return EOK;
}

/* ------------------------------------------------------------------ */
/*  Kernel API: Broadcast                                              */
/* ------------------------------------------------------------------ */

int netlink_broadcast(uint32_t protocol, uint32_t group, const void *data, uint32_t len, int flags)
{
    nl_mcast_table_t *tab;
    int               delivered = 0;

    (void)flags;

    if (protocol >= NL_PROTO_MAX) return -EPROTONOSUPPORT;
    if (!data || len == 0) return -EINVAL;

    tab = &nl_mcast[protocol];

    spin_lock(&tab->lock);

    for (uint32_t i = 0; i < tab->count; i++) {
        struct socket *dest_sk = tab->entries[i].sk;
        if (!dest_sk) continue;

        /* Check if this socket is subscribed to any of the groups */
        if (!(tab->entries[i].groups & group)) continue;

        /* Deliver the message to this socket */
        nl_sock_t *dns = nl_sk(dest_sk);
        if (!dns) continue;

        nl_msg_t *msg = nl_msg_alloc(data, len);
        if (!msg) continue;

        spin_lock(&dns->recv_lock);

        /* Check queue limit */
        if (dns->recv_queue_len >= dns->recv_queue_max) {
            spin_unlock(&dns->recv_lock);
            nl_msg_put(msg);
            continue;
        }

        dns->recv_queue = clist_append(dns->recv_queue, msg);
        dns->recv_queue_len++;
        spin_unlock(&dns->recv_lock);

        /* Wake the blocked receiver */
        if (dns->blocked_task) {
            task_t *t         = dns->blocked_task;
            dns->blocked_task = NULL;
            task_wakeup(t);
        }

        delivered++;
    }

    spin_unlock(&tab->lock);

    return delivered;
}

/* ------------------------------------------------------------------ */
/*  Kernel API: Unicast                                                */
/* ------------------------------------------------------------------ */

int netlink_unicast(struct socket *sk, const void *data, uint32_t len, int flags)
{
    nl_sock_t *ns;
    nl_msg_t  *msg;

    (void)flags;

    if (!sk || !data || len == 0) return -EINVAL;

    ns = nl_sk(sk);
    if (!ns) return -EINVAL;

    msg = nl_msg_alloc(data, len);
    if (!msg) return -ENOMEM;

    spin_lock(&ns->recv_lock);

    if (ns->recv_queue_len >= ns->recv_queue_max) {
        spin_unlock(&ns->recv_lock);
        nl_msg_put(msg);
        return -ENOBUFS;
    }

    ns->recv_queue = clist_append(ns->recv_queue, msg);
    ns->recv_queue_len++;
    spin_unlock(&ns->recv_lock);

    /* Wake the blocked receiver */
    if (ns->blocked_task) {
        task_t *t        = ns->blocked_task;
        ns->blocked_task = NULL;
        task_wakeup(t);
    }

    return EOK;
}

/* ------------------------------------------------------------------ */
/*  Kernel API: Has listeners                                          */
/* ------------------------------------------------------------------ */

int netlink_has_listeners(uint32_t protocol, uint32_t group)
{
    nl_mcast_table_t *tab;

    if (protocol >= NL_PROTO_MAX) return 0;

    tab = &nl_mcast[protocol];

    spin_lock(&tab->lock);
    for (uint32_t i = 0; i < tab->count; i++) {
        if (tab->entries[i].groups & group) {
            spin_unlock(&tab->lock);
            return 1;
        }
    }
    spin_unlock(&tab->lock);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Wrapper functions — match socket_t polymorphic op signatures       */
/* ------------------------------------------------------------------ */

static int netlink_wrap_read(struct socket *sk, void *buf, size_t sz, void *addr, uint32_t *addrlen)
{
    return netlink_recvmsg(sk, buf, sz, addr, addrlen, 0);
}

static int netlink_wrap_write(struct socket *sk, const void *buf, size_t sz, const void *addr, uint32_t addrlen)
{
    return netlink_sendmsg(sk, buf, sz, addr, addrlen, 0);
}

static int netlink_wrap_poll(struct socket *sk, size_t events)
{
    return netlink_poll(sk, events);
}

static int netlink_wrap_close(struct socket *sk)
{
    netlink_close(sk);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Subsystem init                                                     */
/* ------------------------------------------------------------------ */

void netlink_init(void)
{
    memset(nl_mcast, 0, sizeof(nl_mcast));

    for (int i = 0; i < NL_PROTO_MAX; i++) {
        nl_mcast[i].count = 0;
        memset(&nl_mcast[i].lock, 0, sizeof(nl_mcast[i].lock));
    }

    nl_pid_counter = 0;
    memset(&nl_pid_lock, 0, sizeof(nl_pid_lock));

    plogk("netlink: AF_NETLINK socket family initialized\n");
}
