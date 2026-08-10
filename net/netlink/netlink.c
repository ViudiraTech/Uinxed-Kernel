/*
 *
 *      netlink.c
 *      Netlink socket family (AF_NETLINK) implementation
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/kobject/kobject.h>
#include <libs/list/circular_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <net/core/netdev.h>
#include <net/netlink/netlink.h>
#include <net/socket.h>
#include <process/process.h>
#include <process/sched.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define NL_RECV_QUEUE_MAX 1024 // secondary cap; rcvbuf is the primary limit
#define NL_BROADCAST_MAX  256  // bounded per-protocol socket registry
#define NL_PROTO_MAX      32   // NETLINK_MAX rounded up

#define AF_UNSPEC         0
#define ARPHRD_ETHER      1
#define IF_OPER_DOWN      2U
#define IF_OPER_UP        6U
#define RT_TABLE_MAIN     254U
#define RTPROT_KERNEL     2U
#define RTPROT_BOOT       3U
#define RT_SCOPE_UNIVERSE 0U
#define RT_SCOPE_LINK     253U
#define RTN_UNICAST       1U
#define RTMSG_BUF_SIZE    256U

/* ------------------------------------------------------------------ */
/*  Multicast group entry                                               */
/* ------------------------------------------------------------------ */

typedef struct nl_mcast_entry {
        struct socket *sk;     // subscriber socket
        uint32_t       groups; // subscribed groups bitmask for this socket
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

static nl_msg_t *nl_msg_alloc(const void *data, uint32_t len, uint32_t sender_pid, uint32_t sender_groups, uint32_t sender_uid,
                              uint32_t sender_gid)
{
    nl_msg_t *msg;

    if (!data || len == 0) return NULL;

    msg = calloc(1, sizeof(nl_msg_t));
    if (!msg) return NULL;

    msg->data = malloc(len);
    if (!msg->data) {
        free(msg);
        return NULL;
    }

    memcpy(msg->data, data, len);
    msg->len           = len;
    msg->sender_pid    = sender_pid;
    msg->sender_groups = sender_groups;
    msg->sender_uid    = sender_uid;
    msg->sender_gid    = sender_gid;
    msg->refcount      = 1;
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

typedef struct rtnl_dump_context {
        struct socket *sk;
        uint32_t       seq;
        int32_t        ifindex;
        uint32_t       destination;
        uint32_t       emitted;
        int            has_destination;
        int            multipart;
        int            error;
} rtnl_dump_context_t;

typedef struct rtnl_device_info {
        char     name[NETDEV_NAME_MAX];
        uint8_t  address[6];
        uint32_t mtu;
        uint32_t flags;
        uint32_t ipv4_address;
        uint32_t ipv4_netmask;
        uint32_t ipv4_gateway;
        uint32_t ifindex;
} rtnl_device_info_t;

static uint32_t rtnl_be32(uint32_t value)
{
    return __builtin_bswap32(value);
}

static uint8_t rtnl_prefix_length(uint32_t mask)
{
    uint8_t length = 0;
    while (mask) {
        length += (uint8_t)(mask & 1U);
        mask >>= 1;
    }
    return length;
}

static uint32_t rtnl_interface_flags(uint32_t flags)
{
    uint32_t result = 0;
    if (flags & NETDEV_F_UP) result |= IFF_UP;
    if (flags & NETDEV_F_BROADCAST) result |= IFF_BROADCAST;
    if (flags & NETDEV_F_RUNNING) result |= IFF_RUNNING;
    return result;
}

static int rtnl_add_attr(uint8_t *buffer, uint32_t capacity, uint32_t *length, uint16_t type, const void *data, uint16_t data_length)
{
    uint32_t attr_length = RTA_LENGTH(data_length);
    uint32_t padded      = RTA_ALIGN(attr_length);
    if (*length > capacity || padded > capacity - *length) return -EMSGSIZE;
    rtattr_t *attr = (rtattr_t *)(buffer + *length);
    attr->rta_len  = (uint16_t)attr_length;
    attr->rta_type = type;
    if (data_length) memcpy(RTA_DATA(attr), data, data_length);
    if (padded > attr_length) memset(buffer + *length + attr_length, 0, padded - attr_length);
    *length += padded;
    return EOK;
}

static int rtnl_queue_message(struct socket *sk, uint16_t type, uint16_t flags, uint32_t seq, const void *payload, uint32_t payload_length)
{
    uint32_t length = NLMSG_LENGTH(payload_length);
    uint8_t *buffer = calloc(1, NLMSG_ALIGN(length));
    if (!buffer) return -ENOMEM;
    nlmsghdr_t *header  = (nlmsghdr_t *)buffer;
    header->nlmsg_len   = length;
    header->nlmsg_type  = type;
    header->nlmsg_flags = flags;
    header->nlmsg_seq   = seq;
    header->nlmsg_pid   = 0;
    if (payload_length) memcpy(NLMSG_DATA(header), payload, payload_length);
    int result = netlink_unicast(sk, buffer, NLMSG_ALIGN(length), 0);
    free(buffer);
    return result;
}

static int rtnl_queue_error(struct socket *sk, const nlmsghdr_t *request, int error)
{
    nlmsgerr_t response;
    memset(&response, 0, sizeof(response));
    response.error = error;
    response.msg   = *request;
    return rtnl_queue_message(sk, NLMSG_ERROR, 0, request->nlmsg_seq, &response, sizeof(response));
}

static void rtnl_snapshot_device(net_device_t *device, rtnl_device_info_t *info)
{
    spin_lock(&device->lock);
    strncpy(info->name, device->name, sizeof(info->name) - 1);
    memcpy(info->address, device->address, sizeof(info->address));
    info->mtu          = device->mtu;
    info->flags        = device->flags;
    info->ipv4_address = device->ipv4_address;
    info->ipv4_netmask = device->ipv4_netmask;
    info->ipv4_gateway = device->ipv4_gateway;
    info->ifindex      = device->ifindex;
    spin_unlock(&device->lock);
}

static void rtnl_emit_link(net_device_t *device, void *opaque)
{
    rtnl_dump_context_t *context                 = opaque;
    rtnl_device_info_t   info                    = {0};
    uint8_t              payload[RTMSG_BUF_SIZE] = {0};
    uint32_t             length                  = sizeof(ifinfomsg_t);
    if (context->error) return;
    rtnl_snapshot_device(device, &info);
    if (context->ifindex && context->ifindex != (int32_t)info.ifindex) return;
    ifinfomsg_t *message = (ifinfomsg_t *)payload;
    message->ifi_family  = AF_UNSPEC;
    message->ifi_type    = ARPHRD_ETHER;
    message->ifi_index   = (int32_t)info.ifindex;
    message->ifi_flags   = rtnl_interface_flags(info.flags);
    if (rtnl_add_attr(payload, sizeof(payload), &length, IFLA_IFNAME, info.name, (uint16_t)(strlen(info.name) + 1))
        || rtnl_add_attr(payload, sizeof(payload), &length, IFLA_MTU, &info.mtu, sizeof(info.mtu))
        || rtnl_add_attr(payload, sizeof(payload), &length, IFLA_ADDRESS, info.address, sizeof(info.address))) {
        context->error = -EMSGSIZE;
        return;
    }
    uint8_t operstate = (info.flags & NETDEV_F_RUNNING) ? IF_OPER_UP : IF_OPER_DOWN;
    if (rtnl_add_attr(payload, sizeof(payload), &length, IFLA_OPERSTATE, &operstate, sizeof(operstate))) {
        context->error = -EMSGSIZE;
        return;
    }
    context->error = rtnl_queue_message(context->sk, RTM_NEWLINK, context->multipart ? NLM_F_MULTI : 0, context->seq, payload, length);
    if (!context->error) context->emitted++;
}

static void rtnl_emit_address(net_device_t *device, void *opaque)
{
    rtnl_dump_context_t *context                 = opaque;
    rtnl_device_info_t   info                    = {0};
    uint8_t              payload[RTMSG_BUF_SIZE] = {0};
    uint32_t             length                  = sizeof(ifaddrmsg_t);
    if (context->error) return;
    rtnl_snapshot_device(device, &info);
    if (!info.ipv4_address || (context->ifindex && context->ifindex != (int32_t)info.ifindex)) return;
    ifaddrmsg_t *message   = (ifaddrmsg_t *)payload;
    message->ifa_family    = AF_INET;
    message->ifa_prefixlen = rtnl_prefix_length(info.ipv4_netmask);
    message->ifa_scope     = RT_SCOPE_UNIVERSE;
    message->ifa_index     = info.ifindex;
    uint32_t address       = rtnl_be32(info.ipv4_address);
    uint32_t broadcast     = rtnl_be32(info.ipv4_address | ~info.ipv4_netmask);
    if (rtnl_add_attr(payload, sizeof(payload), &length, IFA_ADDRESS, &address, sizeof(address))
        || rtnl_add_attr(payload, sizeof(payload), &length, IFA_LOCAL, &address, sizeof(address))
        || rtnl_add_attr(payload, sizeof(payload), &length, IFA_BROADCAST, &broadcast, sizeof(broadcast))
        || rtnl_add_attr(payload, sizeof(payload), &length, IFA_LABEL, info.name, (uint16_t)(strlen(info.name) + 1))) {
        context->error = -EMSGSIZE;
        return;
    }
    context->error = rtnl_queue_message(context->sk, RTM_NEWADDR, context->multipart ? NLM_F_MULTI : 0, context->seq, payload, length);
    if (!context->error) context->emitted++;
}

static int rtnl_emit_one_route(rtnl_dump_context_t *context, const rtnl_device_info_t *info, int is_default)
{
    uint8_t  payload[RTMSG_BUF_SIZE] = {0};
    uint32_t length                  = sizeof(rtmsg_t);
    rtmsg_t *message                 = (rtmsg_t *)payload;
    message->rtm_family              = AF_INET;
    message->rtm_dst_len             = is_default ? 0 : rtnl_prefix_length(info->ipv4_netmask);
    message->rtm_table               = RT_TABLE_MAIN;
    message->rtm_protocol            = is_default ? RTPROT_BOOT : RTPROT_KERNEL;
    message->rtm_scope               = is_default ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
    message->rtm_type                = RTN_UNICAST;
    uint32_t ifindex                 = info->ifindex;
    uint32_t source                  = rtnl_be32(info->ipv4_address);
    if (rtnl_add_attr(payload, sizeof(payload), &length, RTA_OIF, &ifindex, sizeof(ifindex))) return -EMSGSIZE;
    if (is_default) {
        uint32_t gateway = rtnl_be32(info->ipv4_gateway);
        if (rtnl_add_attr(payload, sizeof(payload), &length, RTA_GATEWAY, &gateway, sizeof(gateway))) return -EMSGSIZE;
    } else {
        uint32_t destination = rtnl_be32(info->ipv4_address & info->ipv4_netmask);
        if (rtnl_add_attr(payload, sizeof(payload), &length, RTA_DST, &destination, sizeof(destination))) return -EMSGSIZE;
    }
    if (rtnl_add_attr(payload, sizeof(payload), &length, RTA_PREFSRC, &source, sizeof(source))) return -EMSGSIZE;
    return rtnl_queue_message(context->sk, RTM_NEWROUTE, context->multipart ? NLM_F_MULTI : 0, context->seq, payload, length);
}

static void rtnl_emit_routes(net_device_t *device, void *opaque)
{
    rtnl_dump_context_t *context = opaque;
    rtnl_device_info_t   info    = {0};
    if (context->error || (!context->multipart && context->emitted)) return;
    rtnl_snapshot_device(device, &info);
    if (!info.ipv4_address || !info.ipv4_netmask || !(info.flags & NETDEV_F_UP)
        || (context->ifindex && context->ifindex != (int32_t)info.ifindex))
        return;
    int connected = !context->has_destination || ((context->destination & info.ipv4_netmask) == (info.ipv4_address & info.ipv4_netmask));
    if (connected)
        context->error = rtnl_emit_one_route(context, &info, 0);
    else if (info.ipv4_gateway)
        context->error = rtnl_emit_one_route(context, &info, 1);
    else
        return;
    if (!context->error) context->emitted++;
    if (context->multipart && connected && !context->error && info.ipv4_gateway) {
        context->error = rtnl_emit_one_route(context, &info, 1);
        if (!context->error) context->emitted++;
    }
}

static int rtnl_parse_route_request(const nlmsghdr_t *request, rtnl_dump_context_t *context)
{
    uint32_t payload_length = NLMSG_PAYLOAD(request, 0);
    uint32_t offset         = NLMSG_ALIGN(sizeof(rtmsg_t));
    while (offset < payload_length) {
        if (payload_length - offset < sizeof(rtattr_t)) return -EINVAL;
        rtattr_t *attr = (rtattr_t *)((uint8_t *)request + NLMSG_ALIGN(NLMSG_HDRLEN) + offset);
        if (attr->rta_len < sizeof(rtattr_t) || attr->rta_len > payload_length - offset) return -EINVAL;
        uint32_t data_length = attr->rta_len - RTA_ALIGN(sizeof(rtattr_t));
        if (attr->rta_type == RTA_DST && data_length >= sizeof(uint32_t)) {
            uint32_t destination;
            memcpy(&destination, RTA_DATA(attr), sizeof(destination));
            context->destination     = rtnl_be32(destination);
            context->has_destination = 1;
        } else if (attr->rta_type == RTA_OIF && data_length >= sizeof(uint32_t)) {
            uint32_t ifindex;
            memcpy(&ifindex, RTA_DATA(attr), sizeof(ifindex));
            context->ifindex = (int32_t)ifindex;
        }
        offset += RTA_ALIGN(attr->rta_len);
    }
    return EOK;
}

static int rtnl_handle_request(struct socket *sk, const nlmsghdr_t *request)
{
    if (!(request->nlmsg_flags & NLM_F_REQUEST)) return rtnl_queue_error(sk, request, -EINVAL);
    rtnl_dump_context_t context = {
        .sk              = sk,
        .seq             = request->nlmsg_seq,
        .ifindex         = 0,
        .destination     = 0,
        .emitted         = 0,
        .has_destination = 0,
        .multipart       = (request->nlmsg_flags & NLM_F_DUMP) == NLM_F_DUMP,
        .error           = 0,
    };
    uint32_t payload_length = NLMSG_PAYLOAD(request, 0);
    uint32_t minimum_length;
    if (request->nlmsg_type == RTM_GETLINK)
        minimum_length = sizeof(ifinfomsg_t);
    else if (request->nlmsg_type == RTM_GETADDR)
        minimum_length = sizeof(ifaddrmsg_t);
    else if (request->nlmsg_type == RTM_GETROUTE)
        minimum_length = sizeof(rtmsg_t);
    else
        return rtnl_queue_error(sk, request, -EOPNOTSUPP);
    if (payload_length < minimum_length) return rtnl_queue_error(sk, request, -EINVAL);
    uint8_t family = payload_length ? *((uint8_t *)request + NLMSG_ALIGN(NLMSG_HDRLEN)) : AF_UNSPEC;
    if (family != AF_UNSPEC && family != AF_INET && request->nlmsg_type != RTM_GETLINK) return rtnl_queue_error(sk, request, -EAFNOSUPPORT);
    if (!context.multipart) {
        if (request->nlmsg_type == RTM_GETLINK && payload_length >= sizeof(ifinfomsg_t))
            context.ifindex = ((ifinfomsg_t *)((uint8_t *)request + NLMSG_ALIGN(NLMSG_HDRLEN)))->ifi_index;
        else if (request->nlmsg_type == RTM_GETADDR && payload_length >= sizeof(ifaddrmsg_t))
            context.ifindex = (int32_t)((ifaddrmsg_t *)((uint8_t *)request + NLMSG_ALIGN(NLMSG_HDRLEN)))->ifa_index;
    }
    if (request->nlmsg_type == RTM_GETROUTE && rtnl_parse_route_request(request, &context)) return rtnl_queue_error(sk, request, -EINVAL);
    switch (request->nlmsg_type) {
        case RTM_GETLINK :
            netdev_iterate(rtnl_emit_link, &context);
            break;
        case RTM_GETADDR :
            netdev_iterate(rtnl_emit_address, &context);
            break;
        case RTM_GETROUTE :
            netdev_iterate(rtnl_emit_routes, &context);
            break;
        default :
            return rtnl_queue_error(sk, request, -EOPNOTSUPP);
    }
    if (context.error) return context.error;
    if (context.multipart) return rtnl_queue_message(sk, NLMSG_DONE, NLM_F_MULTI, request->nlmsg_seq, NULL, 0);
    if (!context.emitted) return rtnl_queue_error(sk, request, -ENODEV);
    if (request->nlmsg_flags & NLM_F_ACK) return rtnl_queue_error(sk, request, 0);
    return EOK;
}

/* Assign a unique port ID for auto-bind */
static uint32_t nl_alloc_pid(void)
{
    uint32_t pid;

    spin_lock(&nl_pid_lock);
    pid = ++nl_pid_counter;
    if (pid == 0) pid = ++nl_pid_counter; // never assign 0 (reserved for kernel)
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
        nl_sock_t *other = nl_sk(tab->entries[i].sk);
        if (ns && other && ns->nl_pid && other->nl_pid == ns->nl_pid) {
            spin_unlock(&tab->lock);
            return -EADDRINUSE;
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
            if (i < tab->count - 1) memmove(&tab->entries[i], &tab->entries[i + 1], (tab->count - i - 1) * sizeof(nl_mcast_entry_t));
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
    if (!sk) {
        plogk("netlink: Socket alloc failed (protocol=%u)\n", (unsigned)protocol);
        return NULL;
    }

    ns = calloc(1, sizeof(nl_sock_t));
    if (!ns) {
        plogk("netlink: Socket state alloc failed (protocol=%u)\n", (unsigned)protocol);
        free(sk);
        return NULL;
    }

    /* Initialise generic socket fields */
    sk->state    = SOCK_STATE_UNCONNECTED;
    sk->family   = AF_NETLINK;
    sk->type     = SOCK_DGRAM; // netlink is datagram-oriented
    sk->protocol = (uint16_t)protocol;
    sk->flags    = 0;
    sk->refcount = 1;
    /*
     * Generic socket teardown wakes waiters for every family.  Initialise
     * the common wait queue here as well as in AF_UNIX/INET allocators.
     */
    wait_queue_init(&sk->waitq);
    sk->priv     = ns;
    sk->sndbuf   = NL_SOCK_RECV_BUF_SIZE;
    sk->rcvbuf   = NL_SOCK_RECV_BUF_SIZE;
    sk->rcvlowat = 1;
    sk->sndlowat = 1;

    process_t *process = process_current();
    if (process) {
        sk->pid = process->task ? (uint32_t)process->task->pid : 0;
        sk->uid = process->uid;
        sk->gid = process->gid;
    }

    /* Wrapper functions to match socket_t polymorphic signatures */
    /* (socket_read takes 5 params; netlink ops take 6 with flags) */
    sk->socket_read  = netlink_wrap_read;
    sk->socket_write = netlink_wrap_write;
    sk->socket_poll  = netlink_wrap_poll;
    sk->socket_close = netlink_wrap_close;

    /* Initialise netlink-specific fields */
    ns->nl_pid           = 0; // unbound
    ns->nl_groups        = 0;
    ns->nl_protocol      = protocol;
    ns->nl_seq           = 0;
    ns->nl_bound         = 0;
    ns->recv_queue       = NULL;
    ns->recv_queue_len   = 0;
    ns->recv_queue_max   = NL_RECV_QUEUE_MAX;
    ns->recv_queue_bytes = 0;
    ns->sk               = sk;

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
    ns->recv_queue       = clist_free(ns->recv_queue);
    ns->recv_queue_len   = 0;
    ns->recv_queue_bytes = 0;
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
        return -EINVAL; // already bound
    }

    /* Set or auto-assign port ID */
    if (addr->nl_pid == 0) {
        uint32_t candidate = sk->pid;
        if (!candidate || nl_mcast_find_by_pid(ns->nl_protocol, candidate)) candidate = nl_alloc_pid();
        ns->nl_pid = candidate;
    } else {
        /* Check if PID is already in use */
        struct socket *existing = nl_mcast_find_by_pid(ns->nl_protocol, addr->nl_pid);
        if (existing && existing != sk) {
            plogk("netlink: Bind failed, pid %u already in use.\n", (unsigned)addr->nl_pid);
            spin_unlock(&sk->lock);
            return -EADDRINUSE;
        }
        ns->nl_pid = addr->nl_pid;
    }

    /*
     * The protocol registry also owns unicast port IDs, so group-zero
     * sockets must be present as well.
     */
    int ret = nl_mcast_subscribe(ns->nl_protocol, sk, addr->nl_groups);
    if (ret != EOK) {
        ns->nl_pid = 0;
        spin_unlock(&sk->lock);
        return ret;
    }
    ns->nl_groups = addr->nl_groups;

    ns->nl_bound = 1;
    spin_unlock(&sk->lock);

    return EOK;
}

int netlink_getsockname(struct socket *sk, sockaddr_nl_t *addr)
{
    nl_sock_t *ns;
    if (!sk || !addr) return -EINVAL;
    ns = nl_sk(sk);
    if (!ns) return -EINVAL;

    spin_lock(&sk->lock);
    memset(addr, 0, sizeof(*addr));
    addr->nl_family = AF_NETLINK;
    addr->nl_pid    = ns->nl_pid;
    addr->nl_groups = ns->nl_groups;
    spin_unlock(&sk->lock);
    return EOK;
}

static int nl_queue_datagram(struct socket *sk, const void *data, uint32_t len, uint32_t sender_pid, uint32_t sender_groups, uint32_t sender_uid,
                             uint32_t sender_gid)
{
    nl_sock_t *ns = nl_sk(sk);
    nl_msg_t  *msg;
    clist_t    node;
    task_t    *blocked = NULL;

    if (!ns || !data || !len) return -EINVAL;
    msg = nl_msg_alloc(data, len, sender_pid, sender_groups, sender_uid, sender_gid);
    if (!msg) {
        plogk("netlink: Datagram message allocation failed (len=%u)\n", len);
        return -ENOMEM;
    }
    node = clist_alloc(msg);
    if (!node) {
        plogk("netlink: Datagram queue node allocation failed (len=%u)\n", len);
        nl_msg_put(msg);
        return -ENOMEM;
    }

    spin_lock(&ns->recv_lock);
    if (ns->recv_queue_len >= ns->recv_queue_max || len > sk->rcvbuf || ns->recv_queue_bytes > sk->rcvbuf - len) {
        static uint64_t last_log;
        if (sched_ticks() - last_log >= 1000) {
            plogk("netlink: Receive queue overflow, dropping datagram (len=%u)\n", len);
            last_log = sched_ticks();
        }
        ns->overrun = 1;
        if (!ns->no_enobufs) sk->so_error = -ENOBUFS;
        spin_unlock(&ns->recv_lock);
        free(node);
        nl_msg_put(msg);
        return -ENOBUFS;
    }

    if (!ns->recv_queue) {
        ns->recv_queue = node;
    } else {
        clist_t tail = clist_tail(ns->recv_queue);
        tail->next   = node;
        node->prev   = tail;
    }
    ns->recv_queue_len++;
    ns->recv_queue_bytes += len;
    blocked          = ns->blocked_task;
    ns->blocked_task = NULL;
    spin_unlock(&ns->recv_lock);

    if (blocked) task_wakeup(blocked);
    /*
     * Most netlink consumers, including eudevd, wait through epoll rather
     * than blocking directly in recvmsg(2).  Queueing a datagram must publish
     * POLLIN through the socket's VFS poll source; waking only blocked_task
     * leaves epoll_wait(-1) asleep while uevents accumulate in this queue.
     */
    if (sk->node) vfs_poll_notify(sk->node, 0x0001U);
    return EOK;
}

static int nl_broadcast_datagram(uint32_t protocol, uint32_t groups, const void *data, uint32_t len, uint32_t sender_pid, uint32_t sender_uid,
                                 uint32_t sender_gid)
{
    nl_mcast_table_t *tab;
    int               delivered   = 0;
    int               first_error = EOK;

    if (protocol >= NL_PROTO_MAX) return -EPROTONOSUPPORT;
    if (!data || !len || !groups) return -EINVAL;
    tab = &nl_mcast[protocol];

    /*
     * netlink_close removes the socket under this same lock before freeing
     * private state, making each table entry stable for the delivery call.
     */
    spin_lock(&tab->lock);
    for (uint32_t i = 0; i < tab->count; i++) {
        if (!tab->entries[i].sk || !(tab->entries[i].groups & groups)) continue;
        int ret = nl_queue_datagram(tab->entries[i].sk, data, len, sender_pid, groups, sender_uid, sender_gid);
        if (!ret)
            delivered++;
        else if (!first_error)
            first_error = ret;
    }
    spin_unlock(&tab->lock);

    if (delivered) return delivered;
    return first_error ? first_error : -ESRCH;
}

/* ------------------------------------------------------------------ */
/*  Sendmsg                                                            */
/* ------------------------------------------------------------------ */

int netlink_sendmsg(struct socket *sk, const void *buf, size_t len, const sockaddr_nl_t *addr, uint32_t addrlen, int flags)
{
    nl_sock_t *ns;

    (void)flags;

    if (!sk || !buf) return -EINVAL;
    if (!len || len > UINT32_MAX) return -EMSGSIZE;

    ns = nl_sk(sk);
    if (!ns) return -EINVAL;

    if (!ns->nl_bound) {
        sockaddr_nl_t local = {.nl_family = AF_NETLINK};
        int           ret   = netlink_bind(sk, &local, sizeof(local));
        if (ret) return ret;
    }

    if (addr && (addrlen < sizeof(sockaddr_nl_t) || addr->nl_family != AF_NETLINK)) return -EINVAL;

    /*
     * NETLINK_KOBJECT_UEVENT deliberately does not carry nlmsghdr.  A
     * privileged userspace relay may inject the same NUL-separated ABI.
     */
    if (ns->nl_protocol == NETLINK_KOBJECT_UEVENT) {
        /*
         * A nonzero destination port is userspace-to-userspace unicast.
         * libudev uses this to hand structured "libudev" records from the
         * main udevd process to idle workers.  Those records intentionally do
         * not use the kernel action@/devpath wire format and must not pass
         * through the privileged raw-uevent validator below.
         */
        if (addr && addr->nl_pid != 0) {
            struct socket *dest = nl_mcast_find_by_pid(ns->nl_protocol, addr->nl_pid);
            if (!dest) return -ECONNREFUSED;
            int ret = nl_queue_datagram(dest, buf, (uint32_t)len, ns->nl_pid, addr->nl_groups, sk->uid, sk->gid);
            return ret ? ret : (int)len;
        }

        /*
         * Processed udev notifications use the same structured header and a
         * multicast destination.  Relay them as userspace messages; the raw
         * action@/devpath validation is only for synthetic kernel uevents.
         */
        static const char libudev_prefix[8] = "libudev";
        if (len >= sizeof(libudev_prefix) && memcmp(buf, libudev_prefix, sizeof(libudev_prefix)) == 0) {
            if (sk->uid != 0) return -EPERM;
            uint32_t groups = addr ? addr->nl_groups : ns->nl_groups;
            if (!groups) return -EINVAL;
            int ret = nl_broadcast_datagram(ns->nl_protocol, groups, buf, (uint32_t)len, ns->nl_pid, sk->uid, sk->gid);
            return (ret >= 0 || ret == -ESRCH) ? (int)len : ret;
        }

        size_t nul = 0;
        while (nul < len && ((const char *)buf)[nul]) nul++;
        if (nul == len || nul < 3 || nul >= 128) return -EINVAL;

        const char *at = NULL;
        for (size_t i = 0; i < nul; i++) {
            if (((const char *)buf)[i] == '@') {
                at = (const char *)buf + i;
                break;
            }
        }
        if (!at || at[1] != '/') return -EINVAL;
        char   action_name[16];
        size_t action_len = (size_t)(at - (const char *)buf);
        if (!action_len || action_len >= sizeof(action_name)) return -EINVAL;
        memcpy(action_name, buf, action_len);
        action_name[action_len] = '\0';
        enum kobject_action action;
        if (kobject_action_type(action_name, &action)) return -EINVAL;
        (void)action;

        if (sk->uid != 0) return -EPERM;
        uint32_t groups = addr ? addr->nl_groups : ns->nl_groups;
        if (!groups) groups = 1U;
        int ret = nl_broadcast_datagram(ns->nl_protocol, groups, buf, (uint32_t)len, ns->nl_pid, sk->uid, sk->gid);
        return (ret >= 0 || ret == -ESRCH) ? (int)len : ret;
    }

    if (len < NLMSG_HDRLEN) return -EINVAL;
    nlmsghdr_t *nlh = (nlmsghdr_t *)buf;

    /* Validate the header */
    if (nlh->nlmsg_len < NLMSG_HDRLEN) return -EINVAL;
    if (nlh->nlmsg_len > len) return -EINVAL;

    uint32_t nlhdr_len = nlh->nlmsg_len;

    /* Kernel (pid=0) is always allowed */
    /* Userspace sends: nl_pid must be set to the sender's pid */
    if (addr && addrlen >= sizeof(sockaddr_nl_t)) {
        /* Send to a specific destination */
        uint32_t       dest_pid = addr->nl_pid;
        struct socket *dest_sk;

        if (dest_pid == 0) {
            if (ns->nl_protocol == NETLINK_ROUTE) {
                size_t offset = 0;
                while (offset < len) {
                    nlmsghdr_t *request   = (nlmsghdr_t *)((uint8_t *)buf + offset);
                    size_t      remaining = len - offset;
                    if (!NLMSG_OK(request, remaining)) return -EINVAL;
                    int result = rtnl_handle_request(sk, request);
                    if (result) return result;
                    offset += NLMSG_ALIGN(request->nlmsg_len);
                }
                return (int)len;
            }
            return (int)nlhdr_len;
        }

        dest_sk = nl_mcast_find_by_pid(ns->nl_protocol, dest_pid);
        if (!dest_sk) return -ECONNREFUSED;

        int ret = nl_queue_datagram(dest_sk, buf, nlhdr_len, ns->nl_pid, addr->nl_groups, sk->uid, sk->gid);
        return ret ? ret : (int)nlhdr_len;
    }

    /* No destination ?multicast if groups are set, else error */
    if (addrlen == 0 || !addr) {
        /* Send as a request to kernel (pid=0) */
        if (ns->nl_protocol == NETLINK_ROUTE) {
            size_t offset = 0;
            while (offset < len) {
                nlmsghdr_t *request   = (nlmsghdr_t *)((uint8_t *)buf + offset);
                size_t      remaining = len - offset;
                if (!NLMSG_OK(request, remaining)) return -EINVAL;
                int result = rtnl_handle_request(sk, request);
                if (result) return result;
                offset += NLMSG_ALIGN(request->nlmsg_len);
            }
            return (int)len;
        }
        return (int)nlhdr_len;
    }

    return (int)nlhdr_len;
}

/* ------------------------------------------------------------------ */
/*  Recvmsg                                                            */
/* ------------------------------------------------------------------ */

int netlink_recvmsg_kern(struct socket *sk, void *buf, size_t len, sockaddr_nl_t *addr, int flags, uint32_t *sender_uid, uint32_t *sender_gid,
                         int *msg_flags)
{
    nl_sock_t *ns;
    nl_msg_t  *msg;
    int        is_nonblock;
    int        peek;
    uint32_t   copy_len;
    uint32_t   full_len;

    if (!sk || !buf) return -EINVAL;

    ns = nl_sk(sk);
    if (!ns) return -EINVAL;

    is_nonblock = ((flags & MSG_DONTWAIT) || (sk->flags & SOCK_NONBLOCK)) ? 1 : 0;
    peek        = (flags & MSG_PEEK) ? 1 : 0;

    spin_lock(&ns->recv_lock);

    /* Wait for messages */
    while (ns->recv_queue_len == 0) {
        if (ns->overrun && !ns->no_enobufs) {
            ns->overrun = 0;
            spin_unlock(&ns->recv_lock);
            return -ENOBUFS;
        }
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

        /*
         * Block current task on this socket.
         * Keep recv_lock held while setting blocked_task so that
         * netlink_broadcast/unicast can't miss the wakeup.
         */
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
    full_len = msg->len;
    copy_len = full_len;
    if (copy_len > len) copy_len = (uint32_t)len;

    if (copy_len) memcpy(buf, msg->data, copy_len);

    /* Fill in sender address */
    if (addr) {
        memset(addr, 0, sizeof(*addr));
        addr->nl_family = AF_NETLINK;
        addr->nl_pid    = msg->sender_pid;
        addr->nl_groups = msg->sender_groups;
    }
    if (sender_uid) *sender_uid = msg->sender_uid;
    if (sender_gid) *sender_gid = msg->sender_gid;
    if (msg_flags) *msg_flags = copy_len < full_len ? MSG_TRUNC : 0;

    if (!peek) {
        /* Remove from queue */
        ns->recv_queue = clist_delete(ns->recv_queue, msg);
        ns->recv_queue_len--;
        ns->recv_queue_bytes -= full_len;
        nl_msg_put(msg);
    }

    spin_unlock(&ns->recv_lock);

    return (flags & MSG_TRUNC) ? (int)full_len : (int)copy_len;
}

int netlink_recvmsg(struct socket *sk, void *buf, size_t len, sockaddr_nl_t *addr, uint32_t *addrlen, int flags)
{
    sockaddr_nl_t sender;
    int           msg_flags;
    int           ret;

    ret = netlink_recvmsg_kern(sk, buf, len, addr ? &sender : NULL, flags, NULL, NULL, &msg_flags);
    if (ret < 0 || !addr) return ret;
    if (!addrlen) return -EFAULT;

    uint32_t user_len;
    if (copy_from_user(&user_len, addrlen, sizeof(user_len))) return -EFAULT;
    uint32_t copy_len = user_len < sizeof(sender) ? user_len : sizeof(sender);
    if (copy_len && copy_to_user(addr, &sender, copy_len)) return -EFAULT;
    uint32_t actual_len = sizeof(sender);
    if (copy_to_user(addrlen, &actual_len, sizeof(actual_len))) return -EFAULT;
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
        if (events & 0x0001) revents |= 0x0001; // POLLIN
    }
    if (sk->so_error && (events & 0x0008)) revents |= 0x0008; // POLLERR
    /* Netlink sockets are always writable (dgram) */
    if (events & 0x0004) revents |= 0x0004; // POLLOUT

    spin_unlock(&ns->recv_lock);

    return revents & (int)events;
}

/* ------------------------------------------------------------------ */
/*  setsockopt / getsockopt                                            */
/* ------------------------------------------------------------------ */

#define SOL_NETLINK 270

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
            if (ival <= 0 || ival > 32) return -EINVAL;

            spin_lock(&sk->lock);
            ns->nl_groups |= (1U << (uint32_t)(ival - 1));
            int ret = nl_mcast_subscribe(ns->nl_protocol, sk, ns->nl_groups);
            spin_unlock(&sk->lock);
            return ret;
        }

        case NETLINK_DROP_MEMBERSHIP : {
            if (optlen < sizeof(int)) return -EINVAL;
            if (copy_from_user(&ival, optval, sizeof(int))) return -EFAULT;
            if (ival <= 0 || ival > 32) return -EINVAL;

            spin_lock(&sk->lock);
            ns->nl_groups &= ~(1U << (uint32_t)(ival - 1));
            int ret = nl_mcast_subscribe(ns->nl_protocol, sk, ns->nl_groups);
            spin_unlock(&sk->lock);
            return ret;
        }

        case NETLINK_NO_ENOBUFS :
        case NETLINK_BROADCAST_ERROR :
        case NETLINK_PKTINFO :
            if (optlen < sizeof(int)) return -EINVAL;
            if (copy_from_user(&ival, optval, sizeof(int))) return -EFAULT;
            spin_lock(&sk->lock);
            if (optname == NETLINK_NO_ENOBUFS)
                ns->no_enobufs = ival != 0;
            else if (optname == NETLINK_BROADCAST_ERROR)
                ns->broadcast_error = ival != 0;
            else
                ns->packet_info = ival != 0;
            spin_unlock(&sk->lock);
            return EOK;

        case NETLINK_CAP_ACK :
            /* Accept but ignore */
            return EOK;

        default :
            return -ENOPROTOOPT;
    }
}

int netlink_packet_info_enabled(struct socket *sk)
{
    nl_sock_t *ns = nl_sk(sk);
    return ns ? ns->packet_info : 0;
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
            ival    = ns->packet_info;
            koptlen = sizeof(int);
            break;

        case NETLINK_BROADCAST_ERROR :
            ival    = ns->broadcast_error;
            koptlen = sizeof(int);
            break;

        case NETLINK_NO_ENOBUFS :
            ival    = ns->no_enobufs;
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
    (void)flags;
    return nl_broadcast_datagram(protocol, group, data, len, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/*  Kernel API: Unicast                                                */
/* ------------------------------------------------------------------ */

int netlink_unicast(struct socket *sk, const void *data, uint32_t len, int flags)
{
    (void)flags;
    return nl_queue_datagram(sk, data, len, 0, 0, 0, 0);
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
/*  Wrapper functions ?match socket_t polymorphic op signatures       */
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
#if CONFIG_NETLINK
    memset(nl_mcast, 0, sizeof(nl_mcast));

    for (int i = 0; i < NL_PROTO_MAX; i++) {
        nl_mcast[i].count = 0;
        memset(&nl_mcast[i].lock, 0, sizeof(nl_mcast[i].lock));
    }

    nl_pid_counter = 0;
    memset(&nl_pid_lock, 0, sizeof(nl_pid_lock));

    plogk("netlink: AF_NETLINK socket family registered.\n");
#endif
}
