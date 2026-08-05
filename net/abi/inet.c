/*
 *
 *      inet.c
 *      Network ABI implementation (inet)
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <ipc/socket.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <net/abi/inet.h>
#include <net/icmp.h>
#include <net/netdev.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <proc/sched.h>

static const struct inet_backend_ops *inet_ops;

typedef struct inet_core_socket {
        int             family;
        int             type;
        int             protocol;
        uint32_t        flags;
        uint32_t        local_address;
        uint32_t        remote_address;
        ipv6_address_t  local_address6;
        ipv6_address_t  remote_address6;
        uint16_t        local_port;
        uint16_t        remote_port;
        uint32_t        local_scope_id;
        uint32_t        remote_scope_id;
        int             listening;
        int             connecting;
        int             reuseaddr;
        int             keepalive;
        int             nodelay;
        int             v6only;
        int             ipv6_unicast_hops;
        int             ipv6_multicast_hops;
        int             ipv6_multicast_loop;
        int             ipv6_recvpktinfo;
        int             ipv6_recvhoplimit;
        int             ipv6_recvtclass;
        int             ipv6_tclass;
        int             ipv6_recverr;
        int             ipv6_mtu_discover;
        int             ipv6_freebind;
        int             ipv6_transparent;
        int             ip_ttl;
        uint32_t        sndbuf;
        uint32_t        rcvbuf;
        uint64_t        sndtimeo_ticks;
        uint64_t        rcvtimeo_ticks;
        uint8_t        *rx_data;
        size_t          rx_length;
        tcp_endpoint_t *pending_accept;
        wait_queue_t    wait;
        spinlock_t      event_lock;
        uint64_t        event_generation;
        void (*event_callback)(void *argument, uint32_t events);
        void *event_argument;
        union {
                udp_endpoint_t  *udp;
                tcp_endpoint_t  *tcp;
                icmp_endpoint_t *icmp;
        } endpoint;
} inet_core_socket_t;

#define INET_POLLIN        0x001
#define INET_POLLOUT       0x004
#define INET_POLLERR       0x008
#define INET_POLLHUP       0x010
#define INET_TICKS_PER_SEC 100U

static uint64_t inet_timeval_ticks(const socket_timeval_t *tv)
{
    if (!tv || tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000) return UINT64_MAX;
    if (!tv->tv_sec && !tv->tv_usec) return 0;
    uint64_t ticks = (uint64_t)tv->tv_sec * INET_TICKS_PER_SEC;
    ticks += ((uint64_t)tv->tv_usec * INET_TICKS_PER_SEC + 999999U) / 1000000U;
    return ticks ? ticks : 1;
}

static int inet_timed_out(uint64_t deadline)
{
    return deadline && sched_ticks() >= deadline;
}

static void inet_tcp_event(tcp_endpoint_t *endpoint, uint32_t events, void *context)
{
    (void)endpoint;
    (void)events;
    inet_core_socket_t *sock = context;
    spin_lock(&sock->event_lock);
    sock->event_generation++;
    void (*callback)(void *argument, uint32_t events) = sock->event_callback;
    void *argument                                    = sock->event_argument;
    spin_unlock(&sock->event_lock);
    wait_queue_wake_all(&sock->wait);
    uint32_t poll_events = 0;
    if (events & (TCP_READY_READ | TCP_READY_ACCEPT)) poll_events |= INET_POLLIN;
    if (events & TCP_READY_WRITE) poll_events |= INET_POLLOUT;
    if (events & TCP_READY_ERROR) poll_events |= INET_POLLERR;
    if (events & TCP_READY_HANGUP) poll_events |= INET_POLLHUP;
    if (callback) callback(argument, poll_events);
}

static void inet_udp_event(udp_endpoint_t *endpoint, uint32_t events, void *context)
{
    (void)endpoint;
    (void)events;
    inet_core_socket_t *sock = context;
    spin_lock(&sock->event_lock);
    sock->event_generation++;
    void (*callback)(void *argument, uint32_t events) = sock->event_callback;
    void *argument                                    = sock->event_argument;
    spin_unlock(&sock->event_lock);
    wait_queue_wake_all(&sock->wait);
    uint32_t poll_events = 0;
    if (events & UDP_READY_READ) poll_events |= INET_POLLIN;
    if (events & UDP_READY_WRITE) poll_events |= INET_POLLOUT;
    if (events & UDP_READY_ERROR) poll_events |= INET_POLLERR;
    if (callback) callback(argument, poll_events);
}

static void inet_icmp_event(icmp_endpoint_t *endpoint, uint32_t events, void *context)
{
    (void)endpoint;
    inet_core_socket_t *sock = context;
    spin_lock(&sock->event_lock);
    sock->event_generation++;
    void (*callback)(void *argument, uint32_t events) = sock->event_callback;
    void *argument                                    = sock->event_argument;
    spin_unlock(&sock->event_lock);
    wait_queue_wake_all(&sock->wait);
    uint32_t poll_events = 0;
    if (events & ICMP_READY_READ) poll_events |= INET_POLLIN;
    if (events & ICMP_READY_WRITE) poll_events |= INET_POLLOUT;
    if (callback) callback(argument, poll_events);
}

static uint64_t inet_event_snapshot(inet_core_socket_t *sock)
{
    spin_lock(&sock->event_lock);
    uint64_t generation = sock->event_generation;
    spin_unlock(&sock->event_lock);
    return generation;
}

static int inet_event_wait(inet_core_socket_t *sock, uint64_t generation, uint64_t deadline)
{
    spin_lock(&sock->event_lock);
    if (sock->event_generation != generation) {
        spin_unlock(&sock->event_lock);
        return EOK;
    }
    wait_queue_prepare(&sock->wait);
    spin_unlock(&sock->event_lock);
    if (deadline) return wait_queue_wait_timed(&sock->wait, deadline);
    wait_queue_sleep();
    return EOK;
}

static int inet_tcp_fill(inet_core_socket_t *sock)
{
    if (!sock->rx_data || sock->rx_length >= TCP_RX_BUFFER_MAX) return 0;
    int ret = tcp_receive(sock->endpoint.tcp, sock->rx_data + sock->rx_length, TCP_RX_BUFFER_MAX - sock->rx_length);
    if (ret > 0) sock->rx_length += (size_t)ret;
    return ret;
}

static uint16_t abi_be16(uint16_t value)
{
    return (uint16_t)((value << 8) | (value >> 8));
}

static uint32_t abi_be32(uint32_t value)
{
    return __builtin_bswap32(value);
}

static int inet6_is_any(const struct in6_addr *address)
{
    static const uint8_t zero[16];
    return memcmp(address->s6_addr, zero, sizeof(zero)) == 0;
}

static int inet6_is_mapped(const struct in6_addr *address)
{
    static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    return memcmp(address->s6_addr, prefix, sizeof(prefix)) == 0;
}

static uint32_t inet6_mapped_ipv4(const struct in6_addr *address)
{
    return ((uint32_t)address->s6_addr[12] << 24) | ((uint32_t)address->s6_addr[13] << 16) | ((uint32_t)address->s6_addr[14] << 8)
           | address->s6_addr[15];
}

static int inet_address(inet_core_socket_t *sock, const struct sockaddr *addr, uint32_t length, uint32_t *address, uint16_t *port,
                        ipv6_address_t *address6, uint32_t *scope_id, int binding, int *native6)
{
    if (!addr || length < sizeof(sa_family_t)) return -EINVAL;
    if (sock->family == AF_INET) {
        if (length < sizeof(sockaddr_in_t)) return -EINVAL;
        const sockaddr_in_t *in = (const sockaddr_in_t *)addr;
        if (in->sin_family != AF_INET) return -EAFNOSUPPORT;
        *address = abi_be32(in->sin_addr.s_addr);
        *port    = abi_be16(in->sin_port);
        if (scope_id) *scope_id = 0;
        if (native6) *native6 = 0;
        return EOK;
    }
    if (length < sizeof(sockaddr_in6_t)) return -EINVAL;
    const sockaddr_in6_t *in6 = (const sockaddr_in6_t *)addr;
    if (in6->sin6_family != AF_INET6) return -EAFNOSUPPORT;
    *port = abi_be16(in6->sin6_port);
    if (scope_id) *scope_id = in6->sin6_scope_id;
    if (address6) memcpy(address6->bytes, in6->sin6_addr.s6_addr, 16);
    if (inet6_is_any(&in6->sin6_addr)) {
        if (!binding) return -ENETUNREACH;
        *address = 0;
        if (native6) *native6 = 1;
        return EOK;
    }
    if (!inet6_is_mapped(&in6->sin6_addr)) {
        *address = 0;
        if (native6) *native6 = 1;
        return EOK;
    }
    if (sock->v6only) return -EAFNOSUPPORT;
    *address = inet6_mapped_ipv4(&in6->sin6_addr);
    if (native6) *native6 = 0;
    return EOK;
}

static void inet_make_address(sockaddr_in_t *addr, uint32_t address, uint16_t port)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family      = AF_INET;
    addr->sin_addr.s_addr = abi_be32(address);
    addr->sin_port        = abi_be16(port);
}

static void inet6_make_address(sockaddr_in6_t *addr, uint32_t address, uint16_t port, uint32_t scope_id)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin6_family   = AF_INET6;
    addr->sin6_port     = abi_be16(port);
    addr->sin6_scope_id = scope_id;
    if (address) {
        addr->sin6_addr.s6_addr[10] = 0xff;
        addr->sin6_addr.s6_addr[11] = 0xff;
        addr->sin6_addr.s6_addr[12] = (uint8_t)(address >> 24);
        addr->sin6_addr.s6_addr[13] = (uint8_t)(address >> 16);
        addr->sin6_addr.s6_addr[14] = (uint8_t)(address >> 8);
        addr->sin6_addr.s6_addr[15] = (uint8_t)address;
    }
}

static void inet6_make_native_address(sockaddr_in6_t *addr, const ipv6_address_t *address, uint16_t port, uint32_t scope_id)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin6_family   = AF_INET6;
    addr->sin6_port     = abi_be16(port);
    addr->sin6_scope_id = scope_id;
    memcpy(addr->sin6_addr.s6_addr, address->bytes, 16);
}

static void inet_make_socket_address(inet_core_socket_t *sock, struct sockaddr *addr, uint32_t address, uint16_t port, uint32_t scope_id)
{
    if (sock->family == AF_INET6)
        inet6_make_address((sockaddr_in6_t *)addr, address, port, scope_id);
    else
        inet_make_address((sockaddr_in_t *)addr, address, port);
}

static uint32_t inet_socket_address_size(const inet_core_socket_t *sock)
{
    return sock->family == AF_INET6 ? sizeof(sockaddr_in6_t) : sizeof(sockaddr_in_t);
}

static int core_create(int family, int type, int protocol, uint32_t flags, void **context)
{
    inet_core_socket_t *sock = calloc(1, sizeof(*sock));
    if (!sock) {
        plogk("inet: socket alloc failed.\n");
        return -ENOMEM;
    }
    sock->family              = family;
    sock->type                = type;
    sock->protocol            = protocol;
    sock->flags               = flags;
    sock->sndbuf              = SOCK_BUF_SIZE;
    sock->rcvbuf              = SOCK_BUF_SIZE;
    sock->ipv6_unicast_hops   = 64;
    sock->ipv6_multicast_hops = 1;
    sock->ipv6_multicast_loop = 1;
    sock->ip_ttl              = 64;
    wait_queue_init(&sock->wait);
    if (type == SOCK_DGRAM)
        sock->endpoint.udp = udp_open_family((uint16_t)family);
    else if (type == SOCK_STREAM)
        sock->endpoint.tcp = tcp_open_family((uint16_t)family);
    else if (type == SOCK_RAW)
        sock->endpoint.icmp = icmp_open();
    if ((type == SOCK_DGRAM && !sock->endpoint.udp) || (type == SOCK_STREAM && !sock->endpoint.tcp)
        || (type == SOCK_RAW && !sock->endpoint.icmp)) {
        plogk("inet: endpoint open failed (family=%u type=%u).\n", (unsigned)family, (unsigned)type);
        free(sock);
        return -ENOMEM;
    }
    if (type == SOCK_STREAM) {
        sock->rx_data = malloc(TCP_RX_BUFFER_MAX);
        if (!sock->rx_data) {
            plogk("inet: socket RX buffer alloc failed (%u bytes).\n", (unsigned)TCP_RX_BUFFER_MAX);
            tcp_close(sock->endpoint.tcp);
            free(sock);
            return -ENOMEM;
        }
        tcp_set_event_callback(sock->endpoint.tcp, inet_tcp_event, sock);
    } else if (type == SOCK_DGRAM) {
        udp_set_event_callback(sock->endpoint.udp, inet_udp_event, sock);
    } else {
        icmp_set_event_callback(sock->endpoint.icmp, inet_icmp_event, sock);
    }
    *context = sock;
    return EOK;
}

static void core_close(void *context)
{
    inet_core_socket_t *sock = context;
    if (!sock) return;
    if (sock->type == SOCK_DGRAM) {
        udp_set_event_callback(sock->endpoint.udp, NULL, NULL);
        udp_close(sock->endpoint.udp);
    } else if (sock->type == SOCK_STREAM) {
        tcp_set_event_callback(sock->endpoint.tcp, NULL, NULL);
        if (sock->pending_accept) tcp_close(sock->pending_accept);
        tcp_close(sock->endpoint.tcp);
        free(sock->rx_data);
    } else {
        icmp_set_event_callback(sock->endpoint.icmp, NULL, NULL);
        icmp_close(sock->endpoint.icmp);
    }
    free(sock);
}

static int core_bind(void *context, const struct sockaddr *addr, uint32_t length)
{
    inet_core_socket_t *sock = context;
    uint32_t            address;
    uint16_t            port;
    ipv6_address_t      address6;
    memset(&address6, 0, sizeof(address6));
    uint32_t scope_id;
    int      native6;
    int      ret = inet_address(sock, addr, length, &address, &port, &address6, &scope_id, 1, &native6);
    if (ret) return ret;
    if (sock->type == SOCK_RAW) {
        ret = icmp_bind(sock->endpoint.icmp, address);
        if (!ret) sock->local_address = address;
        return ret;
    }
    if (native6)
        ret = sock->type == SOCK_DGRAM ? udp_bind6(sock->endpoint.udp, &address6, port) : tcp_bind6(sock->endpoint.tcp, &address6, port);
    else
        ret = sock->type == SOCK_DGRAM ? udp_bind(sock->endpoint.udp, address, port) : tcp_bind(sock->endpoint.tcp, address, port);
    if (!ret) {
        sock->local_address  = address;
        sock->local_address6 = address6;
        if (sock->type == SOCK_DGRAM)
            sock->local_port = udp_local_port(sock->endpoint.udp);
        else {
            tcp_endpoint_info_t info;
            if (!tcp_get_info(sock->endpoint.tcp, &info)) sock->local_port = info.local_port;
        }
        sock->local_scope_id = scope_id;
    }
    return ret;
}

static int core_connect(void *context, const struct sockaddr *addr, uint32_t length, uint32_t flags)
{
    inet_core_socket_t *sock = context;
    if (addr && length >= sizeof(sa_family_t) && addr->sa_family == AF_UNSPEC) {
        if (sock->type != SOCK_DGRAM && sock->type != SOCK_RAW) return -EAFNOSUPPORT;
        int ret = sock->type == SOCK_RAW ? icmp_disconnect(sock->endpoint.icmp) : udp_disconnect(sock->endpoint.udp);
        if (!ret) {
            sock->remote_address  = 0;
            sock->remote_port     = 0;
            sock->remote_scope_id = 0;
            memset(&sock->remote_address6, 0, sizeof(sock->remote_address6));
        }
        return ret;
    }
    uint32_t       address;
    uint16_t       port;
    ipv6_address_t address6;
    memset(&address6, 0, sizeof(address6));
    uint32_t scope_id;
    int      native6;
    int      ret = inet_address(sock, addr, length, &address, &port, &address6, &scope_id, 0, &native6);
    if (ret) return ret;
    if (sock->type == SOCK_RAW) {
        ret = icmp_connect(sock->endpoint.icmp, address);
        if (!ret) sock->remote_address = address;
        return ret;
    }
    if (sock->type == SOCK_STREAM && sock->connecting) {
        tcp_state_t state = tcp_get_state(sock->endpoint.tcp);
        if (state == TCP_ESTABLISHED) {
            sock->connecting = 0;
            return -EISCONN;
        }
        if (state == TCP_SYN_SENT) return -EALREADY;
        sock->connecting = 0;
        int error        = tcp_get_error(sock->endpoint.tcp);
        return -(error ? error : ECONNREFUSED);
    }
    if (native6)
        ret = sock->type == SOCK_DGRAM ? udp_connect6(sock->endpoint.udp, &address6, port) : tcp_connect6(sock->endpoint.tcp, &address6, port);
    else
        ret = sock->type == SOCK_DGRAM ? udp_connect(sock->endpoint.udp, address, port) : tcp_connect(sock->endpoint.tcp, address, port);
    if (!ret || ret == -EINPROGRESS) {
        sock->remote_address  = address;
        sock->remote_address6 = address6;
        sock->remote_port     = port;
        sock->remote_scope_id = scope_id;
        if (sock->type == SOCK_DGRAM) sock->local_port = udp_local_port(sock->endpoint.udp);
    }
    if (sock->type == SOCK_STREAM && ret == -EINPROGRESS) {
        sock->connecting = 1;
        if (flags & SOCK_NONBLOCK) return ret;
        uint64_t deadline = sock->sndtimeo_ticks ? sched_ticks() + sock->sndtimeo_ticks : 0;
        while (tcp_get_state(sock->endpoint.tcp) == TCP_SYN_SENT) {
            if (inet_timed_out(deadline)) return -ETIMEDOUT;
            uint64_t generation = inet_event_snapshot(sock);
            if (tcp_get_state(sock->endpoint.tcp) != TCP_SYN_SENT) break;
            (void)inet_event_wait(sock, generation, deadline);
        }
        sock->connecting = 0;
        if (tcp_get_state(sock->endpoint.tcp) == TCP_ESTABLISHED) return EOK;
        int error = tcp_get_error(sock->endpoint.tcp);
        return -(error ? error : ECONNREFUSED);
    }
    return ret;
}

static int core_listen(void *context, int backlog)
{
    inet_core_socket_t *sock = context;
    if (sock->type != SOCK_STREAM) return -EOPNOTSUPP;
    tcp_endpoint_info_t info;
    if (!tcp_get_info(sock->endpoint.tcp, &info) && !info.local_port) {
        int bind_status = sock->family == AF_INET6 ? tcp_bind6(sock->endpoint.tcp, &sock->local_address6, 0) :
                                                     tcp_bind(sock->endpoint.tcp, sock->local_address, 0);
        if (bind_status) return bind_status;
        if (!tcp_get_info(sock->endpoint.tcp, &info)) sock->local_port = info.local_port;
    }
    int ret = tcp_listen(sock->endpoint.tcp, backlog < 0 ? 0U : (unsigned)backlog);
    if (!ret) sock->listening = 1;
    return ret;
}

static int core_accept(void *context, void **accepted, struct sockaddr *addr, uint32_t *addrlen, uint32_t flags)
{
    inet_core_socket_t *listener = context;
    if (listener->type != SOCK_STREAM) return -EOPNOTSUPP;
    tcp_endpoint_t *endpoint = listener->pending_accept;
    listener->pending_accept = NULL;
    uint64_t deadline        = listener->rcvtimeo_ticks ? sched_ticks() + listener->rcvtimeo_ticks : 0;
    while (!endpoint) {
        uint64_t generation = inet_event_snapshot(listener);
        endpoint            = tcp_accept(listener->endpoint.tcp);
        if (endpoint) break;
        if ((flags & SOCK_NONBLOCK) || inet_timed_out(deadline)) return -EAGAIN;
        (void)inet_event_wait(listener, generation, deadline);
    }
    inet_core_socket_t *sock = calloc(1, sizeof(*sock));
    if (!sock) {
        plogk("inet: accept socket alloc failed.\n");
        tcp_close(endpoint);
        return -ENOMEM;
    }
    sock->type                = SOCK_STREAM;
    sock->family              = listener->family;
    sock->protocol            = IPPROTO_TCP;
    sock->flags               = flags;
    sock->sndbuf              = listener->sndbuf;
    sock->rcvbuf              = listener->rcvbuf;
    sock->sndtimeo_ticks      = listener->sndtimeo_ticks;
    sock->rcvtimeo_ticks      = listener->rcvtimeo_ticks;
    sock->v6only              = listener->v6only;
    sock->ipv6_unicast_hops   = listener->ipv6_unicast_hops;
    sock->ipv6_multicast_hops = listener->ipv6_multicast_hops;
    sock->ipv6_multicast_loop = listener->ipv6_multicast_loop;
    sock->ipv6_recvpktinfo    = listener->ipv6_recvpktinfo;
    sock->ipv6_recvhoplimit   = listener->ipv6_recvhoplimit;
    sock->ipv6_recvtclass     = listener->ipv6_recvtclass;
    sock->ipv6_tclass         = listener->ipv6_tclass;
    sock->ipv6_recverr        = listener->ipv6_recverr;
    sock->ipv6_mtu_discover   = listener->ipv6_mtu_discover;
    wait_queue_init(&sock->wait);
    sock->local_address = listener->local_address;
    sock->local_port    = listener->local_port;
    sock->endpoint.tcp  = endpoint;
    sock->rx_data       = malloc(TCP_RX_BUFFER_MAX);
    if (!sock->rx_data) {
        plogk("inet: accept socket RX buffer alloc failed (%u bytes).\n", (unsigned)TCP_RX_BUFFER_MAX);
        tcp_close(endpoint);
        free(sock);
        return -ENOMEM;
    }
    tcp_set_event_callback(sock->endpoint.tcp, inet_tcp_event, sock);
    tcp_endpoint_info_t info;
    if (!tcp_get_info(endpoint, &info)) {
        sock->local_address   = info.local_address;
        sock->local_address6  = info.local_address6;
        sock->local_port      = info.local_port;
        sock->remote_address  = info.remote_address;
        sock->remote_address6 = info.remote_address6;
        sock->remote_port     = info.remote_port;
        if (addr && addrlen) {
            if (sock->family == AF_INET6 && !ipv6_address_is_unspecified(&info.remote_address6))
                inet6_make_native_address((sockaddr_in6_t *)addr, &info.remote_address6, info.remote_port, sock->remote_scope_id);
            else
                inet_make_socket_address(sock, addr, info.remote_address, info.remote_port, sock->remote_scope_id);
            *addrlen = inet_socket_address_size(sock);
        }
    }
    *accepted = sock;
    return EOK;
}

static int core_sendto(void *context, const void *buf, size_t len, int flags, const struct sockaddr *addr, uint32_t addrlen)
{
    inet_core_socket_t *sock = context;
    if (sock->type == SOCK_STREAM) {
        if (addr) return -EISCONN;
        size_t   sent     = 0;
        uint64_t deadline = sock->sndtimeo_ticks ? sched_ticks() + sock->sndtimeo_ticks : 0;
        while (sent < len) {
            uint64_t generation = inet_event_snapshot(sock);
            int      ret        = tcp_send(sock->endpoint.tcp, (const uint8_t *)buf + sent, len - sent);
            if (ret > 0) {
                sent += (size_t)ret;
                continue;
            }
            if (ret != -EAGAIN) return sent ? (int)sent : ret;
            if ((flags & MSG_DONTWAIT) || inet_timed_out(deadline)) return sent ? (int)sent : -EAGAIN;
            (void)inet_event_wait(sock, generation, deadline);
        }
        return (int)sent;
    }
    if (sock->type == SOCK_RAW) {
        uint32_t address = 0;
        uint16_t port    = 0;
        if (addr) {
            int ret = inet_address(sock, addr, addrlen, &address, &port, NULL, NULL, 0, NULL);
            if (ret) return ret;
        }
        return icmp_send(sock->endpoint.icmp, buf, len, address, (uint8_t)sock->ip_ttl);
    }
    uint32_t       address = 0;
    uint16_t       port    = 0;
    ipv6_address_t address6;
    memset(&address6, 0, sizeof(address6));
    int native6 = 0;
    if (addr) {
        int ret = inet_address(sock, addr, addrlen, &address, &port, &address6, NULL, 0, &native6);
        if (ret) return ret;
    } else if (sock->family == AF_INET6) {
        udp_endpoint_info_t info;
        if (!udp_get_info(sock->endpoint.udp, &info) && !ipv6_address_is_unspecified(&info.remote_address6)) {
            address6 = info.remote_address6;
            native6  = 1;
        }
    }
    int ret          = native6 ? udp_send6(sock->endpoint.udp, buf, len, &address6, port, (uint8_t)sock->ipv6_unicast_hops) :
                                 udp_send(sock->endpoint.udp, buf, len, address, port);
    sock->local_port = udp_local_port(sock->endpoint.udp);
    return ret;
}

static int core_recvfrom(void *context, void *buf, size_t len, int flags, struct sockaddr *addr, uint32_t *addrlen)
{
    inet_core_socket_t *sock = context;
    if (sock->type == SOCK_STREAM) {
        if (!len) return 0;
        size_t   copied   = 0;
        uint64_t deadline = sock->rcvtimeo_ticks ? sched_ticks() + sock->rcvtimeo_ticks : 0;
        for (;;) {
            uint64_t generation = inet_event_snapshot(sock);
            (void)inet_tcp_fill(sock);
            size_t available = sock->rx_length;
            if (available) {
                size_t take = available < len - copied ? available : len - copied;
                memcpy((uint8_t *)buf + copied, sock->rx_data, take);
                copied += take;
                if (!(flags & MSG_PEEK)) {
                    memmove(sock->rx_data, sock->rx_data + take, sock->rx_length - take);
                    sock->rx_length -= take;
                }
                if (!(flags & MSG_WAITALL) || copied == len || (flags & MSG_PEEK)) return (int)copied;
            }
            tcp_state_t state = tcp_get_state(sock->endpoint.tcp);
            if (state == TCP_CLOSE_WAIT || state == TCP_CLOSED || state == TCP_TIME_WAIT) return (int)copied;
            if ((flags & MSG_DONTWAIT) || inet_timed_out(deadline)) return copied ? (int)copied : -EAGAIN;
            (void)inet_event_wait(sock, generation, deadline);
        }
    }
    if (sock->type == SOCK_RAW) {
        uint32_t source;
        int      ret;
        uint64_t deadline = sock->rcvtimeo_ticks ? sched_ticks() + sock->rcvtimeo_ticks : 0;
        do {
            uint64_t generation = inet_event_snapshot(sock);
            ret                 = icmp_receive(sock->endpoint.icmp, buf, len, &source, (flags & MSG_PEEK) != 0);
            if (ret != -EAGAIN || (flags & MSG_DONTWAIT) || inet_timed_out(deadline)) break;
            (void)inet_event_wait(sock, generation, deadline);
        } while (1);
        if (ret >= 0 && addr && addrlen) {
            inet_make_address((sockaddr_in_t *)addr, source, 0);
            *addrlen = sizeof(sockaddr_in_t);
        }
        return ret;
    }
    udp_datagram_t info;
    int            ret;
    uint64_t       deadline = sock->rcvtimeo_ticks ? sched_ticks() + sock->rcvtimeo_ticks : 0;
    do {
        uint64_t generation = inet_event_snapshot(sock);
        ret                 = udp_receive(sock->endpoint.udp, buf, len, &info, (flags & MSG_PEEK) != 0);
        if (ret != -EAGAIN || (flags & MSG_DONTWAIT) || inet_timed_out(deadline)) break;
        (void)inet_event_wait(sock, generation, deadline);
    } while (1);
    if (ret >= 0 && addr && addrlen) {
        if (info.family == AF_INET6 && !ipv6_address_is_unspecified(&info.source_address6))
            inet6_make_native_address((sockaddr_in6_t *)addr, &info.source_address6, info.source_port, 0);
        else
            inet_make_socket_address(sock, addr, info.source_address, info.source_port, 0);
        *addrlen = inet_socket_address_size(sock);
    }
    return ret;
}

static int core_shutdown(void *context, int how)
{
    inet_core_socket_t *sock = context;
    if (sock->type != SOCK_STREAM) return -ENOTCONN;
    if (how == SHUT_RD) return -EOPNOTSUPP;
    return tcp_shutdown(sock->endpoint.tcp);
}

static int core_getsockname(void *context, struct sockaddr *addr, uint32_t *addrlen)
{
    inet_core_socket_t *sock     = context;
    uint32_t            required = inet_socket_address_size(sock);
    if (*addrlen < required) return -EINVAL;
    if (sock->type == SOCK_RAW) {
        /* Raw IPv4 socket addresses have no transport port. */
    } else if (sock->type == SOCK_DGRAM) {
        udp_endpoint_info_t info;
        sock->local_port = udp_local_port(sock->endpoint.udp);
        if (!udp_get_info(sock->endpoint.udp, &info)) sock->local_address6 = info.local_address6;
    } else {
        tcp_endpoint_info_t info;
        if (!tcp_get_info(sock->endpoint.tcp, &info)) {
            sock->local_address  = info.local_address;
            sock->local_address6 = info.local_address6;
            sock->local_port     = info.local_port;
        }
    }
    if (sock->family == AF_INET6 && !ipv6_address_is_unspecified(&sock->local_address6))
        inet6_make_native_address((sockaddr_in6_t *)addr, &sock->local_address6, sock->local_port, sock->local_scope_id);
    else
        inet_make_socket_address(sock, addr, sock->local_address, sock->local_port, sock->local_scope_id);
    *addrlen = required;
    return EOK;
}

static int core_getpeername(void *context, struct sockaddr *addr, uint32_t *addrlen)
{
    inet_core_socket_t *sock = context;
    if ((!sock->remote_address && ipv6_address_is_unspecified(&sock->remote_address6)) || (!sock->remote_port && sock->type != SOCK_RAW))
        return -ENOTCONN;
    uint32_t required = inet_socket_address_size(sock);
    if (*addrlen < required) return -EINVAL;
    if (sock->family == AF_INET6 && !ipv6_address_is_unspecified(&sock->remote_address6))
        inet6_make_native_address((sockaddr_in6_t *)addr, &sock->remote_address6, sock->remote_port, sock->remote_scope_id);
    else
        inet_make_socket_address(sock, addr, sock->remote_address, sock->remote_port, sock->remote_scope_id);
    *addrlen = required;
    return EOK;
}

static int core_setsockopt(void *context, int level, int option, const void *value, uint32_t length)
{
    inet_core_socket_t *sock = context;
    if (!value) return -EFAULT;
    if (level == SOL_IP) {
        if (sock->family != AF_INET || option != IP_TTL) return -ENOPROTOOPT;
        if (length < sizeof(int)) return -EINVAL;
        int val = *(const int *)value;
        if (val < 1 || val > 255) return -EINVAL;
        sock->ip_ttl = val;
        return EOK;
    }
    if (level == SOL_IPV6) {
        if (sock->family != AF_INET6) return -ENOPROTOOPT;
        if (option == IPV6_ADD_MEMBERSHIP || option == IPV6_DROP_MEMBERSHIP) return -EOPNOTSUPP;
        if (option == IPV6_ADDRFORM || option == IPV6_PKTINFO || option == IPV6_HOPLIMIT || option == IPV6_MTU) return -EOPNOTSUPP;
        if (length < sizeof(int)) return -EINVAL;
        int val = *(const int *)value;
        switch (option) {
            case IPV6_V6ONLY :
                if (sock->local_port || sock->remote_port) return -EINVAL;
                sock->v6only = val != 0;
                if (sock->type == SOCK_DGRAM)
                    udp_set_v6only(sock->endpoint.udp, sock->v6only);
                else
                    tcp_set_v6only(sock->endpoint.tcp, sock->v6only);
                return EOK;
            case IPV6_UNICAST_HOPS :
                if (val < -1 || val > 255) return -EINVAL;
                sock->ipv6_unicast_hops = val < 0 ? 64 : val;
                return EOK;
            case IPV6_MULTICAST_HOPS :
                if (val < -1 || val > 255) return -EINVAL;
                sock->ipv6_multicast_hops = val < 0 ? 1 : val;
                return EOK;
            case IPV6_MULTICAST_LOOP :
                sock->ipv6_multicast_loop = val != 0;
                return EOK;
            case IPV6_RECVPKTINFO :
                sock->ipv6_recvpktinfo = val != 0;
                return EOK;
            case IPV6_RECVHOPLIMIT :
                sock->ipv6_recvhoplimit = val != 0;
                return EOK;
            case IPV6_RECVTCLASS :
                sock->ipv6_recvtclass = val != 0;
                return EOK;
            case IPV6_TCLASS :
                if (val < -1 || val > 255) return -EINVAL;
                sock->ipv6_tclass = val < 0 ? 0 : val;
                return EOK;
            case IPV6_RECVERR :
                sock->ipv6_recverr = val != 0;
                return EOK;
            case IPV6_MTU_DISCOVER :
                sock->ipv6_mtu_discover = val;
                return EOK;
            case IPV6_FREEBIND :
                sock->ipv6_freebind = val != 0;
                return EOK;
            case IPV6_TRANSPARENT :
                sock->ipv6_transparent = val != 0;
                return EOK;
            case IPV6_MULTICAST_IF :
                return val ? -EOPNOTSUPP : EOK;
            default :
                return -ENOPROTOOPT;
        }
    }
    if (level == SOL_TCP) {
        if (option != TCP_NODELAY || sock->type != SOCK_STREAM) return -ENOPROTOOPT;
        if (length < sizeof(int)) return -EINVAL;
        sock->nodelay = *(const int *)value != 0;
        return EOK;
    }
    if (level != SOL_SOCKET) return -ENOPROTOOPT;
    if (option == SO_RCVTIMEO || option == SO_SNDTIMEO) {
        if (length < sizeof(socket_timeval_t)) return -EINVAL;
        uint64_t ticks = inet_timeval_ticks(value);
        if (ticks == UINT64_MAX) return -EDOM;
        if (option == SO_RCVTIMEO)
            sock->rcvtimeo_ticks = ticks;
        else
            sock->sndtimeo_ticks = ticks;
        return EOK;
    }
    if (length < sizeof(int)) return -EINVAL;
    int val = *(const int *)value;
    switch (option) {
        case SO_REUSEADDR :
            sock->reuseaddr = val != 0;
            return EOK;
        case SO_KEEPALIVE :
            sock->keepalive = val != 0;
            return EOK;
        case SO_BROADCAST :
            return EOK;
        case SO_SNDBUF :
            if (val <= 0) return -EINVAL;
            sock->sndbuf = (uint32_t)val > SOCK_BUF_MAX ? SOCK_BUF_MAX : (uint32_t)val;
            return EOK;
        case SO_RCVBUF :
            if (val <= 0) return -EINVAL;
            sock->rcvbuf = (uint32_t)val > SOCK_BUF_MAX ? SOCK_BUF_MAX : (uint32_t)val;
            return EOK;
        default :
            return -ENOPROTOOPT;
    }
}

static int core_getsockopt(void *context, int level, int option, void *value, uint32_t *length)
{
    inet_core_socket_t *sock = context;
    int                 val;
    if (level == SOL_IP) {
        if (sock->family != AF_INET || option != IP_TTL) return -ENOPROTOOPT;
        val = sock->ip_ttl;
        if (*length < sizeof(int)) return -EINVAL;
        memcpy(value, &val, sizeof(val));
        *length = sizeof(val);
        return EOK;
    }
    if (level == SOL_IPV6) {
        if (sock->family != AF_INET6) return -ENOPROTOOPT;
        switch (option) {
            case IPV6_V6ONLY :
                val = sock->v6only;
                break;
            case IPV6_UNICAST_HOPS :
                val = sock->ipv6_unicast_hops;
                break;
            case IPV6_MULTICAST_IF :
                val = 0;
                break;
            case IPV6_MULTICAST_HOPS :
                val = sock->ipv6_multicast_hops;
                break;
            case IPV6_MULTICAST_LOOP :
                val = sock->ipv6_multicast_loop;
                break;
            case IPV6_RECVPKTINFO :
                val = sock->ipv6_recvpktinfo;
                break;
            case IPV6_RECVHOPLIMIT :
                val = sock->ipv6_recvhoplimit;
                break;
            case IPV6_RECVTCLASS :
                val = sock->ipv6_recvtclass;
                break;
            case IPV6_TCLASS :
                val = sock->ipv6_tclass;
                break;
            case IPV6_RECVERR :
                val = sock->ipv6_recverr;
                break;
            case IPV6_MTU_DISCOVER :
                val = sock->ipv6_mtu_discover;
                break;
            case IPV6_MTU :
                val = 1500;
                break;
            case IPV6_FREEBIND :
                val = sock->ipv6_freebind;
                break;
            case IPV6_TRANSPARENT :
                val = sock->ipv6_transparent;
                break;
            default :
                return -ENOPROTOOPT;
        }
        if (*length < sizeof(int)) return -EINVAL;
        memcpy(value, &val, sizeof(val));
        *length = sizeof(val);
        return EOK;
    }
    if (level == SOL_SOCKET && (option == SO_RCVTIMEO || option == SO_SNDTIMEO)) {
        if (*length < sizeof(socket_timeval_t)) return -EINVAL;
        uint64_t         ticks = option == SO_RCVTIMEO ? sock->rcvtimeo_ticks : sock->sndtimeo_ticks;
        socket_timeval_t tv    = {
               .tv_sec  = (int64_t)(ticks / INET_TICKS_PER_SEC),
               .tv_usec = (int64_t)((ticks % INET_TICKS_PER_SEC) * (1000000U / INET_TICKS_PER_SEC)),
        };
        memcpy(value, &tv, sizeof(tv));
        *length = sizeof(tv);
        return EOK;
    }
    if (level == SOL_TCP) {
        if (option != TCP_NODELAY || sock->type != SOCK_STREAM) return -ENOPROTOOPT;
        val = sock->nodelay;
    } else if (level == SOL_SOCKET && option == SO_TYPE)
        val = sock->type;
    else if (level == SOL_SOCKET && option == SO_PROTOCOL)
        val = sock->protocol;
    else if (level == SOL_SOCKET && option == SO_DOMAIN)
        val = sock->family;
    else if (level == SOL_SOCKET && option == SO_ERROR)
        val = sock->type == SOCK_STREAM ? tcp_get_error(sock->endpoint.tcp) : 0;
    else if (level == SOL_SOCKET && option == SO_ACCEPTCONN)
        val = sock->listening;
    else if (level == SOL_SOCKET && option == SO_REUSEADDR)
        val = sock->reuseaddr;
    else if (level == SOL_SOCKET && option == SO_KEEPALIVE)
        val = sock->keepalive;
    else if (level == SOL_SOCKET && option == SO_SNDBUF)
        val = (int)sock->sndbuf;
    else if (level == SOL_SOCKET && option == SO_RCVBUF)
        val = (int)sock->rcvbuf;
    else
        return -ENOPROTOOPT;
    if (*length < sizeof(int)) return -EINVAL;
    memcpy(value, &val, sizeof(val));
    *length = sizeof(val);
    return EOK;
}

static int core_poll(void *context, size_t events)
{
    inet_core_socket_t *sock  = context;
    int                 ready = 0;
    if (sock->type == SOCK_RAW) {
        uint32_t state = icmp_readiness(sock->endpoint.icmp);
        if ((events & INET_POLLIN) && (state & ICMP_READY_READ)) ready |= INET_POLLIN;
        if ((events & INET_POLLOUT) && (state & ICMP_READY_WRITE)) ready |= INET_POLLOUT;
    } else if (sock->type == SOCK_DGRAM) {
        if (events & INET_POLLOUT) ready |= INET_POLLOUT;
        if (events & INET_POLLIN) {
            udp_datagram_t info;
            if (udp_receive(sock->endpoint.udp, NULL, 0, &info, 1) >= 0) ready |= INET_POLLIN;
        }
    } else if (sock->listening) {
        if (!sock->pending_accept) sock->pending_accept = tcp_accept(sock->endpoint.tcp);
        if (sock->pending_accept) ready |= INET_POLLIN;
    } else {
        tcp_state_t state = tcp_get_state(sock->endpoint.tcp);
        (void)inet_tcp_fill(sock);
        if (sock->rx_length || state == TCP_CLOSE_WAIT || state == TCP_CLOSED || state == TCP_TIME_WAIT) ready |= INET_POLLIN;
        if (state == TCP_ESTABLISHED || state == TCP_CLOSE_WAIT) ready |= INET_POLLOUT;
        if (sock->connecting && state != TCP_SYN_SENT) {
            ready |= INET_POLLOUT;
            if (state != TCP_ESTABLISHED) ready |= INET_POLLERR;
        }
        if (state == TCP_CLOSED) ready |= INET_POLLHUP;
    }
    return ready & ((int)events | INET_POLLERR | INET_POLLHUP);
}

static void core_set_event_callback(void *context, void (*callback)(void *argument, uint32_t events), void *argument)
{
    inet_core_socket_t *sock = context;
    spin_lock(&sock->event_lock);
    sock->event_callback = callback;
    sock->event_argument = argument;
    spin_unlock(&sock->event_lock);
}

static net_device_t *core_ifreq_device(ifreq_t *ifr)
{
    ifr->ifr_name[IFNAMSIZ - 1] = '\0';
    return ifr->ifr_name[0] ? netdev_get_by_name(ifr->ifr_name) : netdev_get_default();
}

static int core_ioctl(void *context, size_t request, struct ifreq *ifr)
{
    (void)context;
    net_device_t *dev = core_ifreq_device(ifr);
    if (!dev) return -ENODEV;
    int ret = EOK;
    switch (request) {
        case SIOCGIFFLAGS :
            ifr->ifr_flags = 0;
            if (dev->flags & NETDEV_F_UP) ifr->ifr_flags |= IFF_UP;
            if (dev->flags & NETDEV_F_RUNNING) ifr->ifr_flags |= IFF_RUNNING;
            if (dev->flags & NETDEV_F_BROADCAST) ifr->ifr_flags |= IFF_BROADCAST;
            break;
        case SIOCSIFFLAGS :
            ret = netdev_set_up(dev, (ifr->ifr_flags & IFF_UP) != 0);
            break;
        case SIOCGIFMTU :
            ifr->ifr_mtu = (int32_t)dev->mtu;
            break;
        case SIOCSIFMTU :
            ret = netdev_set_mtu(dev, (uint32_t)ifr->ifr_mtu);
            break;
        case SIOCGIFADDR :
            inet_make_address((sockaddr_in_t *)&ifr->ifr_addr, dev->ipv4_address, 0);
            break;
        case SIOCSIFADDR : {
            uint32_t           address;
            uint16_t           port;
            inet_core_socket_t ipv4 = {.family = AF_INET};
            ret = inet_address(&ipv4, (const struct sockaddr *)&ifr->ifr_addr, sizeof(ifr->ifr_addr), &address, &port, NULL, NULL, 1, NULL);
            if (!ret) ret = netdev_configure_ipv4(dev, address, dev->ipv4_netmask, dev->ipv4_gateway);
            break;
        }
        case SIOCGIFNETMASK :
            inet_make_address((sockaddr_in_t *)&ifr->ifr_netmask, dev->ipv4_netmask, 0);
            break;
        case SIOCSIFNETMASK : {
            uint32_t           netmask;
            uint16_t           port;
            inet_core_socket_t ipv4 = {.family = AF_INET};
            ret = inet_address(&ipv4, (const struct sockaddr *)&ifr->ifr_netmask, sizeof(ifr->ifr_netmask), &netmask, &port, NULL, NULL, 1,
                               NULL);
            if (!ret) ret = netdev_configure_ipv4(dev, dev->ipv4_address, netmask, dev->ipv4_gateway);
            break;
        }
        case SIOCGIFBRDADDR :
            inet_make_address((sockaddr_in_t *)&ifr->ifr_broadaddr, dev->ipv4_address | ~dev->ipv4_netmask, 0);
            break;
        case SIOCGIFHWADDR :
            memset(&ifr->ifr_hwaddr, 0, sizeof(ifr->ifr_hwaddr));
            ifr->ifr_hwaddr.sa_family = 1;
            memcpy(ifr->ifr_hwaddr.sa_data, dev->address, 6);
            break;
        default :
            ret = -EOPNOTSUPP;
            break;
    }
    netdev_put(dev);
    return ret;
}

static size_t core_proc_read(enum inet_proc_file file, char *buf, size_t capacity)
{
    net_device_t *dev = netdev_get_default();
    if (!dev) return 0;
    if (file == INET_PROC_ROUTE) {
        int n = snprintf(buf, capacity,
                         "Iface\tDestination Gateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
                         "%s\t00000000\t%08X\t0003\t0\t0\t0\t00000000\t0\t0\t0\n",
                         dev->name, abi_be32(dev->ipv4_gateway));
        netdev_put(dev);
        if (n < 0) return 0;
        return (size_t)n < capacity ? (size_t)n : capacity;
    }
    if (file != INET_PROC_DEV) {
        netdev_put(dev);
        return 0;
    }
    netdev_stats_t stats;
    netdev_get_stats(dev, &stats);
    int n
        = snprintf(buf, capacity,
                   "Inter-|   Receive                                                |  Transmit\n"
                   " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
                   "%6s: %llu %llu %llu %llu 0 0 0 0 %llu %llu %llu %llu 0 0 0 0\n",
                   dev->name, (unsigned long long)stats.rx_bytes, (unsigned long long)stats.rx_packets, (unsigned long long)stats.rx_errors,
                   (unsigned long long)stats.rx_dropped, (unsigned long long)stats.tx_bytes, (unsigned long long)stats.tx_packets,
                   (unsigned long long)stats.tx_errors, (unsigned long long)stats.tx_dropped);
    netdev_put(dev);
    if (n < 0) return 0;
    return (size_t)n < capacity ? (size_t)n : capacity;
}

static const struct inet_backend_ops core_ops = {
    .create             = core_create,
    .close              = core_close,
    .bind               = core_bind,
    .connect            = core_connect,
    .listen             = core_listen,
    .accept             = core_accept,
    .sendto             = core_sendto,
    .recvfrom           = core_recvfrom,
    .shutdown           = core_shutdown,
    .getsockname        = core_getsockname,
    .getpeername        = core_getpeername,
    .setsockopt         = core_setsockopt,
    .getsockopt         = core_getsockopt,
    .poll               = core_poll,
    .set_event_callback = core_set_event_callback,
    .ioctl              = core_ioctl,
    .proc_read          = core_proc_read,
};

int inet_backend_register(const struct inet_backend_ops *ops)
{
    if (!ops || !ops->create || !ops->close) return -EINVAL;
    if (inet_ops) return -EBUSY;
    inet_ops = ops;
    return EOK;
}

const struct inet_backend_ops *inet_backend_get(void)
{
    return inet_ops;
}

size_t inet_backend_proc_read(enum inet_proc_file file, char *buf, size_t capacity)
{
    if (!buf || !capacity || !inet_ops || !inet_ops->proc_read) return 0;
    return inet_ops->proc_read(file, buf, capacity);
}

int inet_builtin_backend_register(void)
{
    int status = inet_backend_register(&core_ops);
    if (status) return status;
    status = ipv6_set_transport_handler(IPV6_NEXT_TCP, tcp_input6);
    if (status && status != -EBUSY) return status;
    status = ipv6_set_transport_handler(IPV6_NEXT_UDP, udp_input6);
    return status == -EBUSY ? EOK : status;
}
