#include <kernel/errno.h>
#include <kernel/printk.h>
#include <ipc/socket.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <net/abi/inet.h>
#include <net/netdev.h>
#include <net/tcp.h>
#include <net/udp.h>
#include <proc/sched.h>

static const struct inet_backend_ops *inet_ops;

typedef struct inet_core_socket {
        int type;
        int protocol;
        uint32_t flags;
        uint32_t local_address;
        uint32_t remote_address;
        uint16_t local_port;
        uint16_t remote_port;
        int listening;
        int connecting;
        int reuseaddr;
        int keepalive;
        int nodelay;
        uint32_t sndbuf;
        uint32_t rcvbuf;
        uint64_t sndtimeo_ticks;
        uint64_t rcvtimeo_ticks;
        uint8_t *rx_data;
        size_t rx_length;
        tcp_endpoint_t *pending_accept;
        wait_queue_t wait;
        spinlock_t event_lock;
        uint64_t event_generation;
        union {
                udp_endpoint_t *udp;
                tcp_endpoint_t *tcp;
        } endpoint;
} inet_core_socket_t;

#define INET_POLLIN  0x001
#define INET_POLLOUT 0x004
#define INET_POLLERR 0x008
#define INET_POLLHUP 0x010
#define INET_TICKS_PER_SEC 100U

static uint64_t inet_timeval_ticks(const socket_timeval_t *tv)
{
    if (!tv || tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000) return UINT64_MAX;
    if (!tv->tv_sec && !tv->tv_usec) return 0;
    uint64_t ticks = (uint64_t)tv->tv_sec * INET_TICKS_PER_SEC;
    ticks += ((uint64_t)tv->tv_usec * INET_TICKS_PER_SEC + 999999U) / 1000000U;
    return ticks ? ticks : 1;
}

static int inet_timed_out(uint64_t deadline) { return deadline && sched_ticks() >= deadline; }

static void inet_tcp_event(tcp_endpoint_t *endpoint, uint32_t events, void *context)
{
    (void)endpoint;
    (void)events;
    inet_core_socket_t *sock = context;
    spin_lock(&sock->event_lock);
    sock->event_generation++;
    spin_unlock(&sock->event_lock);
    wait_queue_wake_all(&sock->wait);
}

static void inet_udp_event(udp_endpoint_t *endpoint, uint32_t events, void *context)
{
    (void)endpoint;
    (void)events;
    inet_core_socket_t *sock = context;
    spin_lock(&sock->event_lock);
    sock->event_generation++;
    spin_unlock(&sock->event_lock);
    wait_queue_wake_all(&sock->wait);
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

static int inet_address(const struct sockaddr *addr, uint32_t length, uint32_t *address, uint16_t *port)
{
    if (!addr || length < sizeof(sockaddr_in_t)) return -EINVAL;
    const sockaddr_in_t *in = (const sockaddr_in_t *)addr;
    if (in->sin_family != AF_INET) return -EAFNOSUPPORT;
    *address = abi_be32(in->sin_addr.s_addr);
    *port = abi_be16(in->sin_port);
    return EOK;
}

static void inet_make_address(sockaddr_in_t *addr, uint32_t address, uint16_t port)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = abi_be32(address);
    addr->sin_port = abi_be16(port);
}

static int core_create(int type, int protocol, uint32_t flags, void **context)
{
    inet_core_socket_t *sock = calloc(1, sizeof(*sock));
    if (!sock) return -ENOMEM;
    sock->type = type;
    sock->protocol = protocol;
    sock->flags = flags;
    sock->sndbuf = SOCK_BUF_SIZE;
    sock->rcvbuf = SOCK_BUF_SIZE;
    wait_queue_init(&sock->wait);
    if (type == SOCK_DGRAM) sock->endpoint.udp = udp_open();
    else if (type == SOCK_STREAM) sock->endpoint.tcp = tcp_open();
    if ((type == SOCK_DGRAM && !sock->endpoint.udp) || (type == SOCK_STREAM && !sock->endpoint.tcp)) {
        free(sock);
        return -ENOMEM;
    }
    if (type == SOCK_STREAM) {
        sock->rx_data = malloc(TCP_RX_BUFFER_MAX);
        if (!sock->rx_data) {
            tcp_close(sock->endpoint.tcp);
            free(sock);
            return -ENOMEM;
        }
        tcp_set_event_callback(sock->endpoint.tcp, inet_tcp_event, sock);
    } else {
        udp_set_event_callback(sock->endpoint.udp, inet_udp_event, sock);
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
    }
    else {
        tcp_set_event_callback(sock->endpoint.tcp, NULL, NULL);
        if (sock->pending_accept) tcp_close(sock->pending_accept);
        tcp_close(sock->endpoint.tcp);
        free(sock->rx_data);
    }
    free(sock);
}

static int core_bind(void *context, const struct sockaddr *addr, uint32_t length)
{
    inet_core_socket_t *sock = context;
    uint32_t address;
    uint16_t port;
    int ret = inet_address(addr, length, &address, &port);
    if (ret) return ret;
    ret = sock->type == SOCK_DGRAM ? udp_bind(sock->endpoint.udp, address, port) : tcp_bind(sock->endpoint.tcp, address, port);
    if (!ret) {
        sock->local_address = address;
        sock->local_port = sock->type == SOCK_DGRAM ? udp_local_port(sock->endpoint.udp) : port;
    }
    return ret;
}

static int core_connect(void *context, const struct sockaddr *addr, uint32_t length, uint32_t flags)
{
    inet_core_socket_t *sock = context;
    if (addr && length >= sizeof(sa_family_t) && addr->sa_family == AF_UNSPEC) {
        if (sock->type != SOCK_DGRAM) return -EAFNOSUPPORT;
        int ret = udp_disconnect(sock->endpoint.udp);
        if (!ret) {
            sock->remote_address = 0;
            sock->remote_port = 0;
        }
        return ret;
    }
    uint32_t address;
    uint16_t port;
    int ret = inet_address(addr, length, &address, &port);
    if (ret) return ret;
    if (sock->type == SOCK_STREAM && sock->connecting) {
        tcp_state_t state = tcp_get_state(sock->endpoint.tcp);
        if (state == TCP_ESTABLISHED) {
            sock->connecting = 0;
            return -EISCONN;
        }
        if (state == TCP_SYN_SENT) return -EALREADY;
        sock->connecting = 0;
        int error = tcp_get_error(sock->endpoint.tcp);
        return -(error ? error : ECONNREFUSED);
    }
    ret = sock->type == SOCK_DGRAM ? udp_connect(sock->endpoint.udp, address, port) : tcp_connect(sock->endpoint.tcp, address, port);
    if (!ret || ret == -EINPROGRESS) {
        sock->remote_address = address;
        sock->remote_port = port;
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
    uint64_t deadline = listener->rcvtimeo_ticks ? sched_ticks() + listener->rcvtimeo_ticks : 0;
    while (!endpoint) {
        uint64_t generation = inet_event_snapshot(listener);
        endpoint = tcp_accept(listener->endpoint.tcp);
        if (endpoint) break;
        if ((flags & SOCK_NONBLOCK) || inet_timed_out(deadline)) return -EAGAIN;
        (void)inet_event_wait(listener, generation, deadline);
    }
    inet_core_socket_t *sock = calloc(1, sizeof(*sock));
    if (!sock) {
        tcp_close(endpoint);
        return -ENOMEM;
    }
    sock->type = SOCK_STREAM;
    sock->protocol = IPPROTO_TCP;
    sock->flags = flags;
    sock->sndbuf = listener->sndbuf;
    sock->rcvbuf = listener->rcvbuf;
    sock->sndtimeo_ticks = listener->sndtimeo_ticks;
    sock->rcvtimeo_ticks = listener->rcvtimeo_ticks;
    wait_queue_init(&sock->wait);
    sock->local_address = listener->local_address;
    sock->local_port = listener->local_port;
    sock->endpoint.tcp = endpoint;
    sock->rx_data = malloc(TCP_RX_BUFFER_MAX);
    if (!sock->rx_data) {
        tcp_close(endpoint);
        free(sock);
        return -ENOMEM;
    }
    tcp_set_event_callback(sock->endpoint.tcp, inet_tcp_event, sock);
    tcp_endpoint_info_t info;
    if (!tcp_get_info(endpoint, &info)) {
        sock->local_address = info.local_address;
        sock->local_port = info.local_port;
        sock->remote_address = info.remote_address;
        sock->remote_port = info.remote_port;
        if (addr && addrlen) {
            inet_make_address((sockaddr_in_t *)addr, info.remote_address, info.remote_port);
            *addrlen = sizeof(sockaddr_in_t);
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
        size_t sent = 0;
        uint64_t deadline = sock->sndtimeo_ticks ? sched_ticks() + sock->sndtimeo_ticks : 0;
        while (sent < len) {
            uint64_t generation = inet_event_snapshot(sock);
            int ret = tcp_send(sock->endpoint.tcp, (const uint8_t *)buf + sent, len - sent);
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
    uint32_t address = 0;
    uint16_t port = 0;
    if (addr) {
        int ret = inet_address(addr, addrlen, &address, &port);
        if (ret) return ret;
    }
    int ret = udp_send(sock->endpoint.udp, buf, len, address, port);
    sock->local_port = udp_local_port(sock->endpoint.udp);
    return ret;
}

static int core_recvfrom(void *context, void *buf, size_t len, int flags, struct sockaddr *addr, uint32_t *addrlen)
{
    inet_core_socket_t *sock = context;
    if (sock->type == SOCK_STREAM) {
        if (!len) return 0;
        size_t copied = 0;
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
    udp_datagram_t info;
    int ret;
    uint64_t deadline = sock->rcvtimeo_ticks ? sched_ticks() + sock->rcvtimeo_ticks : 0;
    do {
        uint64_t generation = inet_event_snapshot(sock);
        ret = udp_receive(sock->endpoint.udp, buf, len, &info, (flags & MSG_PEEK) != 0);
        if (ret != -EAGAIN || (flags & MSG_DONTWAIT) || inet_timed_out(deadline)) break;
        (void)inet_event_wait(sock, generation, deadline);
    } while (1);
    if (ret >= 0 && addr && addrlen) {
        inet_make_address((sockaddr_in_t *)addr, info.source_address, info.source_port);
        *addrlen = sizeof(sockaddr_in_t);
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
    inet_core_socket_t *sock = context;
    if (*addrlen < sizeof(sockaddr_in_t)) return -EINVAL;
    if (sock->type == SOCK_DGRAM) sock->local_port = udp_local_port(sock->endpoint.udp);
    else {
        tcp_endpoint_info_t info;
        if (!tcp_get_info(sock->endpoint.tcp, &info)) {
            sock->local_address = info.local_address;
            sock->local_port = info.local_port;
        }
    }
    inet_make_address((sockaddr_in_t *)addr, sock->local_address, sock->local_port);
    *addrlen = sizeof(sockaddr_in_t);
    return EOK;
}

static int core_getpeername(void *context, struct sockaddr *addr, uint32_t *addrlen)
{
    inet_core_socket_t *sock = context;
    if (!sock->remote_address || !sock->remote_port) return -ENOTCONN;
    if (*addrlen < sizeof(sockaddr_in_t)) return -EINVAL;
    inet_make_address((sockaddr_in_t *)addr, sock->remote_address, sock->remote_port);
    *addrlen = sizeof(sockaddr_in_t);
    return EOK;
}

static int core_setsockopt(void *context, int level, int option, const void *value, uint32_t length)
{
    inet_core_socket_t *sock = context;
    if (!value) return -EFAULT;
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
        if (option == SO_RCVTIMEO) sock->rcvtimeo_ticks = ticks;
        else sock->sndtimeo_ticks = ticks;
        return EOK;
    }
    if (length < sizeof(int)) return -EINVAL;
    int val = *(const int *)value;
    switch (option) {
        case SO_REUSEADDR: sock->reuseaddr = val != 0; return EOK;
        case SO_KEEPALIVE: sock->keepalive = val != 0; return EOK;
        case SO_SNDBUF: if (val <= 0) return -EINVAL; sock->sndbuf = (uint32_t)val > SOCK_BUF_MAX ? SOCK_BUF_MAX : (uint32_t)val; return EOK;
        case SO_RCVBUF: if (val <= 0) return -EINVAL; sock->rcvbuf = (uint32_t)val > SOCK_BUF_MAX ? SOCK_BUF_MAX : (uint32_t)val; return EOK;
        default: return -ENOPROTOOPT;
    }
}

static int core_getsockopt(void *context, int level, int option, void *value, uint32_t *length)
{
    inet_core_socket_t *sock = context;
    int val;
    if (level == SOL_SOCKET && (option == SO_RCVTIMEO || option == SO_SNDTIMEO)) {
        if (*length < sizeof(socket_timeval_t)) return -EINVAL;
        uint64_t ticks = option == SO_RCVTIMEO ? sock->rcvtimeo_ticks : sock->sndtimeo_ticks;
        socket_timeval_t tv = {
            .tv_sec = (int64_t)(ticks / INET_TICKS_PER_SEC),
            .tv_usec = (int64_t)((ticks % INET_TICKS_PER_SEC) * (1000000U / INET_TICKS_PER_SEC)),
        };
        memcpy(value, &tv, sizeof(tv));
        *length = sizeof(tv);
        return EOK;
    }
    if (level == SOL_TCP) {
        if (option != TCP_NODELAY || sock->type != SOCK_STREAM) return -ENOPROTOOPT;
        val = sock->nodelay;
    } else if (level == SOL_SOCKET && option == SO_TYPE) val = sock->type;
    else if (level == SOL_SOCKET && option == SO_PROTOCOL) val = sock->protocol;
    else if (level == SOL_SOCKET && option == SO_DOMAIN) val = AF_INET;
    else if (level == SOL_SOCKET && option == SO_ERROR) val = sock->type == SOCK_STREAM ? tcp_get_error(sock->endpoint.tcp) : 0;
    else if (level == SOL_SOCKET && option == SO_ACCEPTCONN) val = sock->listening;
    else if (level == SOL_SOCKET && option == SO_REUSEADDR) val = sock->reuseaddr;
    else if (level == SOL_SOCKET && option == SO_KEEPALIVE) val = sock->keepalive;
    else if (level == SOL_SOCKET && option == SO_SNDBUF) val = (int)sock->sndbuf;
    else if (level == SOL_SOCKET && option == SO_RCVBUF) val = (int)sock->rcvbuf;
    else return -ENOPROTOOPT;
    if (*length < sizeof(int)) return -EINVAL;
    memcpy(value, &val, sizeof(val));
    *length = sizeof(val);
    return EOK;
}

static int core_poll(void *context, size_t events)
{
    inet_core_socket_t *sock = context;
    int ready = 0;
    if (sock->type == SOCK_DGRAM) {
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
        case SIOCGIFFLAGS:
            ifr->ifr_flags = 0;
            if (dev->flags & NETDEV_F_UP) ifr->ifr_flags |= IFF_UP;
            if (dev->flags & NETDEV_F_RUNNING) ifr->ifr_flags |= IFF_RUNNING;
            if (dev->flags & NETDEV_F_BROADCAST) ifr->ifr_flags |= IFF_BROADCAST;
            break;
        case SIOCSIFFLAGS: ret = netdev_set_up(dev, (ifr->ifr_flags & IFF_UP) != 0); break;
        case SIOCGIFMTU: ifr->ifr_mtu = (int32_t)dev->mtu; break;
        case SIOCSIFMTU: ret = netdev_set_mtu(dev, (uint32_t)ifr->ifr_mtu); break;
        case SIOCGIFADDR: inet_make_address((sockaddr_in_t *)&ifr->ifr_addr, dev->ipv4_address, 0); break;
        case SIOCSIFADDR: {
            uint32_t address;
            uint16_t port;
            ret = inet_address((const struct sockaddr *)&ifr->ifr_addr, sizeof(ifr->ifr_addr), &address, &port);
            if (!ret) ret = netdev_configure_ipv4(dev, address, dev->ipv4_netmask, dev->ipv4_gateway);
            break;
        }
        case SIOCGIFNETMASK: inet_make_address((sockaddr_in_t *)&ifr->ifr_netmask, dev->ipv4_netmask, 0); break;
        case SIOCSIFNETMASK: {
            uint32_t netmask;
            uint16_t port;
            ret = inet_address((const struct sockaddr *)&ifr->ifr_netmask, sizeof(ifr->ifr_netmask), &netmask, &port);
            if (!ret) ret = netdev_configure_ipv4(dev, dev->ipv4_address, netmask, dev->ipv4_gateway);
            break;
        }
        case SIOCGIFBRDADDR: inet_make_address((sockaddr_in_t *)&ifr->ifr_broadaddr, dev->ipv4_address | ~dev->ipv4_netmask, 0); break;
        case SIOCGIFHWADDR:
            memset(&ifr->ifr_hwaddr, 0, sizeof(ifr->ifr_hwaddr));
            ifr->ifr_hwaddr.sa_family = 1;
            memcpy(ifr->ifr_hwaddr.sa_data, dev->address, 6);
            break;
        default: ret = -EOPNOTSUPP; break;
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
    int n = snprintf(buf, capacity,
                     "Inter-|   Receive                                                |  Transmit\n"
                     " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
                     "%6s: %llu %llu %llu %llu 0 0 0 0 %llu %llu %llu %llu 0 0 0 0\n",
                     dev->name, (unsigned long long)stats.rx_bytes, (unsigned long long)stats.rx_packets,
                     (unsigned long long)stats.rx_errors, (unsigned long long)stats.rx_dropped,
                     (unsigned long long)stats.tx_bytes, (unsigned long long)stats.tx_packets,
                     (unsigned long long)stats.tx_errors, (unsigned long long)stats.tx_dropped);
    netdev_put(dev);
    if (n < 0) return 0;
    return (size_t)n < capacity ? (size_t)n : capacity;
}

static const struct inet_backend_ops core_ops = {
    .create = core_create, .close = core_close, .bind = core_bind, .connect = core_connect,
    .listen = core_listen, .accept = core_accept, .sendto = core_sendto, .recvfrom = core_recvfrom,
    .shutdown = core_shutdown, .getsockname = core_getsockname, .getpeername = core_getpeername,
    .setsockopt = core_setsockopt, .getsockopt = core_getsockopt, .poll = core_poll,
    .ioctl = core_ioctl, .proc_read = core_proc_read,
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
    return inet_backend_register(&core_ops);
}
