/*
 *
 *      udp.c
 *      UDP protocol implementation
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <net/abi/inet.h>
#include <net/core/endian.h>
#include <net/ipv4/icmp.h>
#include <net/transport/udp.h>
#include <process/sched.h>

#define UDP_HEADER_LEN      8U
#define UDP_EPHEMERAL_FIRST 49152U

typedef struct udp_packet {
        struct udp_packet *next;
        uint16_t           family;
        uint32_t           source_address;
        ipv6_address_t     source_address6;
        uint16_t           source_port;
        size_t             length;
        uint8_t            data[];
} udp_packet_t;

typedef struct udp_endpoint {
        uint16_t             family;
        uint8_t              native6;
        uint8_t              v6only;
        uint32_t             local_address;
        uint32_t             remote_address;
        ipv6_address_t       local_address6;
        ipv6_address_t       remote_address6;
        uint16_t             local_port;
        uint16_t             remote_port;
        uint16_t             queue_length;
        uint32_t             queue_bytes;
        uint8_t              bound;
        udp_packet_t        *head;
        udp_packet_t        *tail;
        wait_queue_t         wait;
        spinlock_t           lock;
        udp_event_callback_t event_callback;
        void                *event_context;
} udp_endpoint_t;

/*
 * Connectionless UDP: each endpoint has a bound local port and a FIFO of
 * received datagrams. sendto/receive map directly onto the IP layer; there
 * is no retransmission or ordering.
 */

static udp_endpoint_t *udp_table[UDP_ENDPOINT_MAX];
static spinlock_t      udp_table_lock;
static uint16_t        udp_ephemeral = UDP_EPHEMERAL_FIRST;

static int udp_autobind(udp_endpoint_t *ep);

/* Wake waiters and fire the event callback for the endpoint. */
static void udp_notify(udp_endpoint_t *ep, uint32_t events)
{
    wait_queue_wake_all(&ep->wait);
    udp_event_callback_t callback = ep->event_callback;
    void                *context  = ep->event_context;
    if (callback) callback(ep, events, context);
}

/* Decode a UDP header and verify the IPv4 pseudo-header checksum */
int net_udp_parse(const void *data, size_t length, uint32_t source, uint32_t destination, net_udp_datagram_t *datagram)
{
    if (!data || !datagram || length < UDP_HEADER_LEN) return -EBADMSG;
    const uint8_t *bytes       = data;
    uint16_t       wire_length = net_read_be16(bytes + 4);
    uint16_t       checksum    = net_read_be16(bytes + 6);
    if (wire_length < UDP_HEADER_LEN || wire_length > length) return -EBADMSG;
    if (checksum && net_checksum_ipv4_pseudo(source, destination, IPV4_PROTO_UDP, bytes, wire_length) != 0) return -EBADMSG;
    datagram->source_port      = net_read_be16(bytes);
    datagram->destination_port = net_read_be16(bytes + 2);
    datagram->payload          = bytes + UDP_HEADER_LEN;
    datagram->payload_len      = wire_length - UDP_HEADER_LEN;
    datagram->checksum_present = checksum != 0;
    return 0;
}

int net_udp_parse6(const void *data, size_t length, const struct in6_addr *source, const struct in6_addr *destination, net_udp_datagram_t *datagram)
{
    if (!data || !source || !destination || !datagram || length < UDP_HEADER_LEN) return -EBADMSG;
    const uint8_t        *bytes       = data;
    uint16_t              wire_length = net_read_be16(bytes + 4);
    const ipv6_address_t *src         = (const ipv6_address_t *)source->s6_addr;
    const ipv6_address_t *dst         = (const ipv6_address_t *)destination->s6_addr;
    if (wire_length < UDP_HEADER_LEN || wire_length > length || !net_read_be16(bytes + 6) || net_checksum_ipv6_pseudo(src, dst, IPV6_NEXT_UDP, bytes, wire_length) != 0) return -EBADMSG;
    datagram->source_port      = net_read_be16(bytes);
    datagram->destination_port = net_read_be16(bytes + 2);
    datagram->payload          = bytes + UDP_HEADER_LEN;
    datagram->payload_len      = wire_length - UDP_HEADER_LEN;
    datagram->checksum_present = 1;
    return 0;
}

/* True if the IPv4 local address/port pair is already bound. */
static int udp_port_used_locked(uint32_t address, uint16_t port, const udp_endpoint_t *ignore)
{
    for (unsigned i = 0; i < UDP_ENDPOINT_MAX; i++) {
        udp_endpoint_t *ep = udp_table[i];
        if (ep && ep != ignore && ep->bound && ep->local_port == port && (!ep->local_address || !address || ep->local_address == address)) return 1;
    }
    return 0;
}

/* Allocate an endpoint and register it in the global table. */
udp_endpoint_t *udp_open_family(uint16_t family)
{
    udp_endpoint_t *ep = calloc(1, sizeof(*ep));
    if (!ep) return NULL;
    ep->family = family;
    wait_queue_init(&ep->wait);
    spin_lock(&udp_table_lock);
    for (unsigned i = 0; i < UDP_ENDPOINT_MAX; i++) {
        if (!udp_table[i]) {
            udp_table[i] = ep;
            spin_unlock(&udp_table_lock);
            return ep;
        }
    }
    spin_unlock(&udp_table_lock);
    free(ep);
    return NULL;
}

/* Open a new AF_INET UDP endpoint. */
udp_endpoint_t *udp_open(void)
{
    return udp_open_family(AF_INET);
}

/* Unregister and free an endpoint, draining its queued datagrams. */
void udp_close(udp_endpoint_t *ep)
{
    if (!ep) return;
    spin_lock(&udp_table_lock);
    for (unsigned i = 0; i < UDP_ENDPOINT_MAX; i++)
        if (udp_table[i] == ep) udp_table[i] = NULL;
    spin_unlock(&udp_table_lock);
    spin_lock(&ep->lock);
    udp_packet_t *packet = ep->head;
    ep->head = ep->tail = NULL;
    spin_unlock(&ep->lock);
    while (packet) {
        udp_packet_t *next = packet->next;
        free(packet);
        packet = next;
    }
    wait_queue_wake_all(&ep->wait);
    free(ep);
}

/* Bind the endpoint to a local address/port, or autobind if port is zero. */
int udp_bind(udp_endpoint_t *ep, uint32_t address, uint16_t port)
{
    if (!ep) return -EINVAL;
    if (!port) {
        int status = udp_autobind(ep);
        if (!status) ep->local_address = address;
        return status;
    }
    spin_lock(&udp_table_lock);
    if (ep->bound || udp_port_used_locked(address, port, ep)) {
        spin_unlock(&udp_table_lock);
        return ep->bound ? -EINVAL : -EADDRINUSE;
    }
    ep->local_address = address;
    ep->local_port    = port;
    ep->bound         = 1;
    spin_unlock(&udp_table_lock);
    return 0;
}

/* Bind an AF_INET6 endpoint to a local address/port. */
int udp_bind6(udp_endpoint_t *ep, const ipv6_address_t *address, uint16_t port)
{
    if (!ep || !address || ep->family != AF_INET6) return -EINVAL;
    ep->native6 = 1;
    int status  = udp_bind(ep, 0, port);
    if (!status) ep->local_address6 = *address;
    return status;
}

/* Choose a free ephemeral port in the 49152-65535 range. */
static int udp_autobind(udp_endpoint_t *ep)
{
    if (ep->bound) return 0;
    spin_lock(&udp_table_lock);
    for (unsigned n = 0; n <= UINT16_MAX - UDP_EPHEMERAL_FIRST; n++) {
        uint16_t port = udp_ephemeral++;
        if (udp_ephemeral < UDP_EPHEMERAL_FIRST) udp_ephemeral = UDP_EPHEMERAL_FIRST;
        if (!udp_port_used_locked(0, port, ep)) {
            ep->local_port = port;
            ep->bound      = 1;
            spin_unlock(&udp_table_lock);
            return 0;
        }
    }
    spin_unlock(&udp_table_lock);
    return -EADDRINUSE;
}

/* Set a fixed remote IPv4 address/port for the endpoint. */
int udp_connect(udp_endpoint_t *ep, uint32_t address, uint16_t port)
{
    if (!ep || !address || !port) return -EINVAL;
    int status = udp_autobind(ep);
    if (status) return status;
    spin_lock(&ep->lock);
    ep->native6        = 0;
    ep->remote_address = address;
    ep->remote_port    = port;
    spin_unlock(&ep->lock);
    return 0;
}

/* Set a fixed remote IPv6 address/port for the endpoint. */
int udp_connect6(udp_endpoint_t *ep, const ipv6_address_t *address, uint16_t port)
{
    if (!ep || !address || ep->family != AF_INET6 || ipv6_address_is_unspecified(address) || !port) return -EINVAL;
    int status = udp_autobind(ep);
    if (status) return status;
    net_device_t  *device;
    ipv6_address_t source, next_hop;
    status = ipv6_route(address, &device, &source, &next_hop);
    if (status) return status;
    netdev_put(device);
    spin_lock(&ep->lock);
    ep->native6 = 1;
    if (ipv6_address_is_unspecified(&ep->local_address6)) ep->local_address6 = source;
    ep->remote_address6 = *address;
    ep->remote_port     = port;
    spin_unlock(&ep->lock);
    return 0;
}

/* Clear the fixed remote address (revert to unconnected). */
int udp_disconnect(udp_endpoint_t *ep)
{
    if (!ep) return -EINVAL;
    spin_lock(&ep->lock);
    ep->remote_address = 0;
    memset(&ep->remote_address6, 0, sizeof(ep->remote_address6));
    ep->remote_port = 0;
    spin_unlock(&ep->lock);
    return 0;
}

/* Send a UDP datagram to destination:port via the IPv4 layer. */
int udp_send(udp_endpoint_t *ep, const void *data, size_t length, uint32_t destination, uint16_t port)
{
    if (!ep || (!data && length)) return -EINVAL;
    if (length > UINT16_MAX - UDP_HEADER_LEN) return -EMSGSIZE;
    int status = udp_autobind(ep);
    if (status) return status;
    if (!destination) destination = ep->remote_address;
    if (!port) port = ep->remote_port;
    if (!destination || !port) return -EDESTADDRREQ;
    net_device_t *device;
    uint32_t      next_hop;
    status = ipv4_route(destination, &device, &next_hop);
    if (status) return status;
    net_pbuf_t *packet = net_pbuf_alloc(UDP_HEADER_LEN + length, NET_PBUF_HEADROOM);
    if (!packet) {
        plogk("udp: Send alloc failed (dest=%u.%u.%u.%u:%u len=%lu)\n", (unsigned)(destination >> 24) & 0xff, (unsigned)(destination >> 16) & 0xff, (unsigned)(destination >> 8) & 0xff,
              (unsigned)destination & 0xff, (unsigned)port, (unsigned long)length);
        netdev_put(device);
        return -ENOMEM;
    }
    net_write_be16(packet->data, ep->local_port);
    net_write_be16(packet->data + 2, port);
    net_write_be16(packet->data + 4, (uint16_t)packet->length);
    net_write_be16(packet->data + 6, 0);
    if (length) memcpy(packet->data + UDP_HEADER_LEN, data, length);
    uint32_t source   = ep->local_address ? ep->local_address : device->ipv4_address;
    uint16_t checksum = net_checksum_ipv4_pseudo(source, destination, IPV4_PROTO_UDP, packet->data, packet->length);
    net_write_be16(packet->data + 6, checksum ? checksum : UINT16_MAX);
    status = ipv4_output(device, source, destination, IPV4_PROTO_UDP, 64, packet);
    net_pbuf_free(packet);
    netdev_put(device);
    return status == -EINPROGRESS ? (int)length : (status ? status : (int)length);
}

/* Send a UDP datagram to an IPv6 destination via the IPv6 layer. */
int udp_send6(udp_endpoint_t *ep, const void *data, size_t length, const ipv6_address_t *destination, uint16_t port, uint8_t hop_limit)
{
    if (!ep || (!data && length) || ep->family != AF_INET6 || length > UINT16_MAX - UDP_HEADER_LEN) return -EINVAL;
    int status = udp_autobind(ep);
    if (status) return status;
    if (!destination || ipv6_address_is_unspecified(destination)) destination = &ep->remote_address6;
    if (!port) port = ep->remote_port;
    if (ipv6_address_is_unspecified(destination) || !port) return -EDESTADDRREQ;
    net_device_t  *device;
    ipv6_address_t source, next_hop;
    status = ipv6_route(destination, &device, &source, &next_hop);
    if (status) return status;
    if (!ipv6_address_is_unspecified(&ep->local_address6)) source = ep->local_address6;
    net_pbuf_t *packet = net_pbuf_alloc(UDP_HEADER_LEN + length, NET_PBUF_HEADROOM);
    if (!packet) {
        plogk("udp: Send6 alloc failed (dest=%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x:%u len=%lu)\n", (unsigned)net_read_be16(destination->bytes), (unsigned)net_read_be16(destination->bytes + 2),
              (unsigned)net_read_be16(destination->bytes + 4), (unsigned)net_read_be16(destination->bytes + 6), (unsigned)net_read_be16(destination->bytes + 8),
              (unsigned)net_read_be16(destination->bytes + 10), (unsigned)net_read_be16(destination->bytes + 12), (unsigned)net_read_be16(destination->bytes + 14), (unsigned)port,
              (unsigned long)length);
        netdev_put(device);
        return -ENOMEM;
    }
    net_write_be16(packet->data, ep->local_port);
    net_write_be16(packet->data + 2, port);
    net_write_be16(packet->data + 4, (uint16_t)packet->length);
    net_write_be16(packet->data + 6, 0);
    if (length) memcpy(packet->data + UDP_HEADER_LEN, data, length);
    uint16_t checksum = net_checksum_ipv6_pseudo(&source, destination, IPV6_NEXT_UDP, packet->data, packet->length);
    net_write_be16(packet->data + 6, checksum ? checksum : UINT16_MAX);
    status = ipv6_output(device, &source, destination, IPV6_NEXT_UDP, hop_limit, packet);
    net_pbuf_free(packet);
    netdev_put(device);
    return status == -EINPROGRESS ? (int)length : (status ? status : (int)length);
}

/* Dequeue the next datagram (or peek without consuming), filling sender info */
int udp_receive(udp_endpoint_t *ep, void *data, size_t capacity, udp_datagram_t *info, int peek)
{
    if (!ep || (!data && capacity)) return -EINVAL;
    spin_lock(&ep->lock);
    udp_packet_t *packet = ep->head;
    if (!packet) {
        spin_unlock(&ep->lock);
        return -EAGAIN;
    }
    size_t copied = packet->length < capacity ? packet->length : capacity;
    if (copied) memcpy(data, packet->data, copied);
    if (info) {
        info->source_address  = packet->source_address;
        info->source_address6 = packet->source_address6;
        info->family          = packet->family;
        info->source_port     = packet->source_port;
        info->length          = packet->length;
    }
    if (!peek) {
        ep->head = packet->next;
        if (!ep->head) ep->tail = NULL;
        ep->queue_length--;
        ep->queue_bytes -= (uint32_t)packet->length;
    }
    spin_unlock(&ep->lock);
    if (!peek) free(packet);
    return (int)copied;
}

/* UDP datagram input from the IPv4 layer: queue it on the matching endpoint */
int udp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet)
{
    if (!device || !ip || !packet || packet->length < UDP_HEADER_LEN) goto bad;
    uint16_t source_port      = net_read_be16(packet->data);
    uint16_t destination_port = net_read_be16(packet->data + 2);
    uint16_t length           = net_read_be16(packet->data + 4);
    uint16_t checksum         = net_read_be16(packet->data + 6);
    if (!destination_port || length < UDP_HEADER_LEN || length > packet->length) goto bad;
    if (checksum && net_checksum_ipv4_pseudo(ip->source, ip->destination, IPV4_PROTO_UDP, packet->data, length) != 0) goto bad;
    udp_endpoint_t *target = NULL;
    spin_lock(&udp_table_lock);
    for (unsigned i = 0; i < UDP_ENDPOINT_MAX; i++) {
        udp_endpoint_t *ep = udp_table[i];
        if (!ep || (ep->family != AF_INET && (ep->family != AF_INET6 || ep->v6only || !ipv6_address_is_unspecified(&ep->local_address6))) || !ep->bound || ep->local_port != destination_port
            || (ep->local_address && ep->local_address != ip->destination))
            continue;
        if (ep->remote_address && (ep->remote_address != ip->source || ep->remote_port != source_port)) continue;
        if (!target || ep->remote_address) target = ep;
    }
    if (!target) {
        spin_unlock(&udp_table_lock);
        net_pbuf_free(packet);
        return -ECONNREFUSED;
    }
    spin_lock(&target->lock);
    size_t payload_length = length - UDP_HEADER_LEN;
    if (target->queue_length >= UDP_RX_QUEUE_MAX || payload_length > UDP_RX_BYTES_MAX - target->queue_bytes) {
        static uint64_t last_log;
        if (sched_ticks() - last_log >= 1000) {
            plogk("udp: %s: RX queue overflow, dropping datagram from %u.%u.%u.%u:%u\n", device->name, (unsigned)(ip->source >> 24) & 0xff, (unsigned)(ip->source >> 16) & 0xff,
                  (unsigned)(ip->source >> 8) & 0xff, (unsigned)ip->source & 0xff, (unsigned)source_port);
            last_log = sched_ticks();
        }
        spin_unlock(&target->lock);
        spin_unlock(&udp_table_lock);
        net_pbuf_free(packet);
        return -ENOBUFS;
    }
    udp_packet_t *queued = malloc(sizeof(*queued) + payload_length);
    if (!queued) {
        plogk("udp: RX queue alloc failed (src=%u.%u.%u.%u:%u len=%lu)\n", (unsigned)(ip->source >> 24) & 0xff, (unsigned)(ip->source >> 16) & 0xff, (unsigned)(ip->source >> 8) & 0xff,
              (unsigned)ip->source & 0xff, (unsigned)source_port, (unsigned long)payload_length);
        spin_unlock(&target->lock);
        spin_unlock(&udp_table_lock);
        net_pbuf_free(packet);
        return -ENOMEM;
    }
    queued->next           = NULL;
    queued->family         = target->family;
    queued->source_address = ip->source;
    queued->source_port    = source_port;
    queued->length         = payload_length;
    if (payload_length) memcpy(queued->data, packet->data + UDP_HEADER_LEN, payload_length);
    if (target->tail)
        target->tail->next = queued;
    else
        target->head = queued;
    target->tail = queued;
    target->queue_length++;
    target->queue_bytes += (uint32_t)payload_length;
    udp_event_callback_t cb_udp  = target->event_callback;
    void                *ctx_udp = target->event_context;
    wait_queue_wake_all(&target->wait);

    /*
     * Pin the inet wrapper (event_context) before dropping the lock so a
     * concurrent core_close() cannot free it while the callback runs.
     */
    if (cb_udp) inet_sock_ref(ctx_udp);
    spin_unlock(&target->lock);
    spin_unlock(&udp_table_lock);
    if (cb_udp) {
        cb_udp(target, UDP_READY_READ, ctx_udp);
        inet_sock_unref(ctx_udp);
    }
    net_pbuf_free(packet);
    return 0;
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}

int udp_input6(net_device_t *device, const ipv6_info_t *ip, net_pbuf_t *packet)
{
    if (!device || !ip || !packet || packet->length < UDP_HEADER_LEN) goto bad;
    uint16_t source_port      = net_read_be16(packet->data);
    uint16_t destination_port = net_read_be16(packet->data + 2);
    uint16_t length           = net_read_be16(packet->data + 4);
    if (!destination_port || length < UDP_HEADER_LEN || length > packet->length || !net_read_be16(packet->data + 6)
        || net_checksum_ipv6_pseudo(&ip->source, &ip->destination, IPV6_NEXT_UDP, packet->data, length) != 0)
        goto bad;
    udp_endpoint_t *target = NULL;
    spin_lock(&udp_table_lock);
    for (unsigned i = 0; i < UDP_ENDPOINT_MAX; i++) {
        udp_endpoint_t *ep = udp_table[i];
        if (!ep || ep->family != AF_INET6 || !ep->bound || ep->local_port != destination_port
            || (!ipv6_address_is_unspecified(&ep->local_address6) && !ipv6_address_equal(&ep->local_address6, &ip->destination)))
            continue;
        if (!ipv6_address_is_unspecified(&ep->remote_address6) && (!ipv6_address_equal(&ep->remote_address6, &ip->source) || ep->remote_port != source_port)) continue;
        if (!target || !ipv6_address_is_unspecified(&ep->remote_address6)) target = ep;
    }
    if (!target) {
        spin_unlock(&udp_table_lock);
        net_pbuf_free(packet);
        return -ECONNREFUSED;
    }
    spin_lock(&target->lock);
    size_t payload_length = length - UDP_HEADER_LEN;
    if (target->queue_length >= UDP_RX_QUEUE_MAX || payload_length > UDP_RX_BYTES_MAX - target->queue_bytes) {
        static uint64_t last_log6;
        if (sched_ticks() - last_log6 >= 1000) {
            plogk("udp: %s: RX6 queue overflow, dropping datagram.\n", device->name);
            last_log6 = sched_ticks();
        }
        spin_unlock(&target->lock);
        spin_unlock(&udp_table_lock);
        net_pbuf_free(packet);
        return -ENOBUFS;
    }
    udp_packet_t *queued = malloc(sizeof(*queued) + payload_length);
    if (!queued) {
        plogk("udp: RX6 queue alloc failed (src=%04x:%04x:%04x:%04x:%04x:%04x:%04x:%04x:%u len=%lu)\n", (unsigned)net_read_be16(ip->source.bytes), (unsigned)net_read_be16(ip->source.bytes + 2),
              (unsigned)net_read_be16(ip->source.bytes + 4), (unsigned)net_read_be16(ip->source.bytes + 6), (unsigned)net_read_be16(ip->source.bytes + 8),
              (unsigned)net_read_be16(ip->source.bytes + 10), (unsigned)net_read_be16(ip->source.bytes + 12), (unsigned)net_read_be16(ip->source.bytes + 14), (unsigned)source_port,
              (unsigned long)payload_length);
        spin_unlock(&target->lock);
        spin_unlock(&udp_table_lock);
        net_pbuf_free(packet);
        return -ENOMEM;
    }
    queued->next            = NULL;
    queued->family          = AF_INET6;
    queued->source_address  = 0;
    queued->source_address6 = ip->source;
    queued->source_port     = source_port;
    queued->length          = payload_length;
    if (payload_length) memcpy(queued->data, packet->data + UDP_HEADER_LEN, payload_length);
    if (target->tail)
        target->tail->next = queued;
    else
        target->head = queued;
    target->tail = queued;
    target->queue_length++;
    target->queue_bytes += (uint32_t)payload_length;
    udp_event_callback_t cb_udp6  = target->event_callback;
    void                *ctx_udp6 = target->event_context;
    wait_queue_wake_all(&target->wait);

    /* Pin the inet wrapper so a concurrent core_close() cannot free it while the callback reads sock->event_* / sock->wait. */
    if (cb_udp6) inet_sock_ref(ctx_udp6);
    spin_unlock(&target->lock);
    spin_unlock(&udp_table_lock);
    if (cb_udp6) {
        cb_udp6(target, UDP_READY_READ, ctx_udp6);
        inet_sock_unref(ctx_udp6);
    }
    net_pbuf_free(packet);
    return 0;
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}

/* Return the endpoint's bound local port. */
uint16_t udp_local_port(const udp_endpoint_t *endpoint)
{
    return endpoint ? endpoint->local_port : 0;
}

/* Report the endpoint's current read/write readiness mask. */
uint32_t udp_readiness(udp_endpoint_t *endpoint)
{
    if (!endpoint) return UDP_READY_ERROR;
    spin_lock(&endpoint->lock);
    uint32_t events = UDP_READY_WRITE | (endpoint->head ? UDP_READY_READ : 0);
    spin_unlock(&endpoint->lock);
    return events;
}

/* Snapshot endpoint state for getsockname/getpeername-style queries. */
int udp_get_info(udp_endpoint_t *endpoint, udp_endpoint_info_t *info)
{
    if (!endpoint || !info) return -EINVAL;
    spin_lock(&endpoint->lock);
    info->family           = endpoint->family;
    info->local_address    = endpoint->local_address;
    info->remote_address   = endpoint->remote_address;
    info->local_address6   = endpoint->local_address6;
    info->remote_address6  = endpoint->remote_address6;
    info->local_port       = endpoint->local_port;
    info->remote_port      = endpoint->remote_port;
    info->queued_datagrams = endpoint->queue_length;
    info->queued_bytes     = endpoint->queue_bytes;
    info->connected        = endpoint->remote_port && (endpoint->remote_address || !ipv6_address_is_unspecified(&endpoint->remote_address6));
    spin_unlock(&endpoint->lock);
    return 0;
}

/* Install the event callback and fire it once with current readiness. */
void udp_set_event_callback(udp_endpoint_t *endpoint, udp_event_callback_t callback, void *context)
{
    if (!endpoint) return;
    spin_lock(&endpoint->lock);
    endpoint->event_callback = callback;
    endpoint->event_context  = callback ? context : NULL;
    spin_unlock(&endpoint->lock);
    if (callback) callback(endpoint, udp_readiness(endpoint), context);
}

/* Toggle IPv6-only mode for a dual-stack endpoint. */
void udp_set_v6only(udp_endpoint_t *endpoint, int enabled)
{
    if (!endpoint) return;
    spin_lock(&endpoint->lock);
    endpoint->v6only = enabled != 0;
    spin_unlock(&endpoint->lock);
}

/* Return the endpoint's wait queue for blocking on readiness. */
wait_queue_t *udp_wait_queue(udp_endpoint_t *endpoint)
{
    return endpoint ? &endpoint->wait : NULL;
}
