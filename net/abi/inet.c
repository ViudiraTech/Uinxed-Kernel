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
        union {
                udp_endpoint_t *udp;
                tcp_endpoint_t *tcp;
        } endpoint;
} inet_core_socket_t;

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
    if (type == SOCK_DGRAM) sock->endpoint.udp = udp_open();
    else if (type == SOCK_STREAM) sock->endpoint.tcp = tcp_open();
    if ((type == SOCK_DGRAM && !sock->endpoint.udp) || (type == SOCK_STREAM && !sock->endpoint.tcp)) {
        free(sock);
        return -ENOMEM;
    }
    *context = sock;
    return EOK;
}

static void core_close(void *context)
{
    inet_core_socket_t *sock = context;
    if (!sock) return;
    if (sock->type == SOCK_DGRAM) udp_close(sock->endpoint.udp);
    else tcp_close(sock->endpoint.tcp);
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

static int core_connect(void *context, const struct sockaddr *addr, uint32_t length)
{
    inet_core_socket_t *sock = context;
    uint32_t address;
    uint16_t port;
    int ret = inet_address(addr, length, &address, &port);
    if (ret) return ret;
    ret = sock->type == SOCK_DGRAM ? udp_connect(sock->endpoint.udp, address, port) : tcp_connect(sock->endpoint.tcp, address, port);
    if (!ret || ret == -EINPROGRESS) {
        sock->remote_address = address;
        sock->remote_port = port;
        if (sock->type == SOCK_DGRAM) sock->local_port = udp_local_port(sock->endpoint.udp);
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
    tcp_endpoint_t *endpoint = tcp_accept(listener->endpoint.tcp);
    if (!endpoint) return -EAGAIN;
    inet_core_socket_t *sock = calloc(1, sizeof(*sock));
    if (!sock) {
        tcp_close(endpoint);
        return -ENOMEM;
    }
    sock->type = SOCK_STREAM;
    sock->protocol = IPPROTO_TCP;
    sock->flags = flags;
    sock->local_address = listener->local_address;
    sock->local_port = listener->local_port;
    sock->endpoint.tcp = endpoint;
    *accepted = sock;
    if (addr && addrlen) *addrlen = 0;
    return EOK;
}

static int core_sendto(void *context, const void *buf, size_t len, int flags, const struct sockaddr *addr, uint32_t addrlen)
{
    inet_core_socket_t *sock = context;
    (void)flags;
    if (sock->type == SOCK_STREAM) {
        if (addr) return -EISCONN;
        return tcp_send(sock->endpoint.tcp, buf, len);
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
    if (sock->type == SOCK_STREAM) return tcp_receive(sock->endpoint.tcp, buf, len);
    udp_datagram_t info;
    int ret = udp_receive(sock->endpoint.udp, buf, len, &info, (flags & MSG_PEEK) != 0);
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
    (void)context;
    (void)level;
    (void)option;
    (void)value;
    (void)length;
    return -ENOPROTOOPT;
}

static int core_getsockopt(void *context, int level, int option, void *value, uint32_t *length)
{
    inet_core_socket_t *sock = context;
    int val;
    if (*length < sizeof(int)) return -EINVAL;
    if (level == SOL_SOCKET && option == SO_TYPE) val = sock->type;
    else if (level == SOL_SOCKET && option == SO_PROTOCOL) val = sock->protocol;
    else if (level == SOL_SOCKET && option == SO_DOMAIN) val = AF_INET;
    else if (level == SOL_SOCKET && option == SO_ERROR) val = sock->type == SOCK_STREAM ? tcp_get_error(sock->endpoint.tcp) : 0;
    else if (level == SOL_SOCKET && option == SO_ACCEPTCONN) val = sock->listening;
    else return -ENOPROTOOPT;
    memcpy(value, &val, sizeof(val));
    *length = sizeof(val);
    return EOK;
}

static int core_poll(void *context, size_t events)
{
    inet_core_socket_t *sock = context;
    int ready = 0;
    if (events & 0x004) ready |= 0x004;
    if (sock->type == SOCK_STREAM) {
        tcp_state_t state = tcp_get_state(sock->endpoint.tcp);
        if (state == TCP_CLOSE_WAIT || state == TCP_CLOSED) ready |= 0x001 | 0x010;
    }
    return ready & (int)events;
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
