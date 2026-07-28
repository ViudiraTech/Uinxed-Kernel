#include <kernel/errno.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <net/endian.h>
#include <net/icmp.h>
#include <net/udp.h>

#define UDP_HEADER_LEN 8U
#define UDP_EPHEMERAL_FIRST 49152U

typedef struct udp_packet {
        struct udp_packet *next;
        uint32_t           source_address;
        uint16_t           source_port;
        size_t             length;
        uint8_t            data[];
} udp_packet_t;

struct udp_endpoint {
        uint32_t      local_address;
        uint32_t      remote_address;
        uint16_t      local_port;
        uint16_t      remote_port;
        uint16_t      queue_length;
        uint32_t      queue_bytes;
        uint8_t       bound;
        udp_packet_t *head;
        udp_packet_t *tail;
        wait_queue_t  wait;
        spinlock_t    lock;
        udp_event_callback_t event_callback;
        void                 *event_context;
};

static udp_endpoint_t *udp_table[UDP_ENDPOINT_MAX];
static spinlock_t udp_table_lock;
static uint16_t udp_ephemeral = UDP_EPHEMERAL_FIRST;

static void udp_notify(udp_endpoint_t *ep, uint32_t events)
{
    wait_queue_wake_all(&ep->wait);
    udp_event_callback_t callback = ep->event_callback;
    void *context = ep->event_context;
    if (callback) callback(ep, events, context);
}

int net_udp_parse(const void *data, size_t length, uint32_t source, uint32_t destination, net_udp_datagram_t *datagram)
{
    if (!data || !datagram || length < UDP_HEADER_LEN) return -EBADMSG;
    const uint8_t *bytes = data;
    uint16_t wire_length = net_read_be16(bytes + 4);
    uint16_t checksum = net_read_be16(bytes + 6);
    if (wire_length < UDP_HEADER_LEN || wire_length > length) return -EBADMSG;
    if (checksum && net_checksum_ipv4_pseudo(source, destination, IPV4_PROTO_UDP, bytes, wire_length) != 0) return -EBADMSG;
    datagram->source_port = net_read_be16(bytes);
    datagram->destination_port = net_read_be16(bytes + 2);
    datagram->payload = bytes + UDP_HEADER_LEN;
    datagram->payload_len = wire_length - UDP_HEADER_LEN;
    datagram->checksum_present = checksum != 0;
    return 0;
}

static int udp_port_used_locked(uint32_t address, uint16_t port, const udp_endpoint_t *ignore)
{
    for (unsigned i = 0; i < UDP_ENDPOINT_MAX; i++) {
        udp_endpoint_t *ep = udp_table[i];
        if (ep && ep != ignore && ep->bound && ep->local_port == port && (!ep->local_address || !address || ep->local_address == address)) return 1;
    }
    return 0;
}

udp_endpoint_t *udp_open(void)
{
    udp_endpoint_t *ep = calloc(1, sizeof(*ep));
    if (!ep) return NULL;
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

void udp_close(udp_endpoint_t *ep)
{
    if (!ep) return;
    spin_lock(&udp_table_lock);
    for (unsigned i = 0; i < UDP_ENDPOINT_MAX; i++) if (udp_table[i] == ep) udp_table[i] = NULL;
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

int udp_bind(udp_endpoint_t *ep, uint32_t address, uint16_t port)
{
    if (!ep || !port) return -EINVAL;
    spin_lock(&udp_table_lock);
    if (ep->bound || udp_port_used_locked(address, port, ep)) {
        spin_unlock(&udp_table_lock);
        return ep->bound ? -EINVAL : -EADDRINUSE;
    }
    ep->local_address = address;
    ep->local_port = port;
    ep->bound = 1;
    spin_unlock(&udp_table_lock);
    return 0;
}

static int udp_autobind(udp_endpoint_t *ep)
{
    if (ep->bound) return 0;
    spin_lock(&udp_table_lock);
    for (unsigned n = 0; n <= UINT16_MAX - UDP_EPHEMERAL_FIRST; n++) {
        uint16_t port = udp_ephemeral++;
        if (udp_ephemeral < UDP_EPHEMERAL_FIRST) udp_ephemeral = UDP_EPHEMERAL_FIRST;
        if (!udp_port_used_locked(0, port, ep)) {
            ep->local_port = port;
            ep->bound = 1;
            spin_unlock(&udp_table_lock);
            return 0;
        }
    }
    spin_unlock(&udp_table_lock);
    return -EADDRINUSE;
}

int udp_connect(udp_endpoint_t *ep, uint32_t address, uint16_t port)
{
    if (!ep || !address || !port) return -EINVAL;
    int status = udp_autobind(ep);
    if (status) return status;
    spin_lock(&ep->lock);
    ep->remote_address = address;
    ep->remote_port = port;
    spin_unlock(&ep->lock);
    return 0;
}

int udp_disconnect(udp_endpoint_t *ep)
{
    if (!ep) return -EINVAL;
    spin_lock(&ep->lock);
    ep->remote_address = 0;
    ep->remote_port = 0;
    spin_unlock(&ep->lock);
    return 0;
}

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
    uint32_t next_hop;
    status = ipv4_route(destination, &device, &next_hop);
    if (status) return status;
    net_pbuf_t *packet = net_pbuf_alloc(UDP_HEADER_LEN + length, NET_PBUF_HEADROOM);
    if (!packet) {
        netdev_put(device);
        return -ENOMEM;
    }
    net_write_be16(packet->data, ep->local_port);
    net_write_be16(packet->data + 2, port);
    net_write_be16(packet->data + 4, (uint16_t)packet->length);
    net_write_be16(packet->data + 6, 0);
    if (length) memcpy(packet->data + UDP_HEADER_LEN, data, length);
    uint32_t source = ep->local_address ? ep->local_address : device->ipv4_address;
    uint16_t checksum = net_checksum_ipv4_pseudo(source, destination, IPV4_PROTO_UDP, packet->data, packet->length);
    net_write_be16(packet->data + 6, checksum ? checksum : UINT16_MAX);
    status = ipv4_output(device, source, destination, IPV4_PROTO_UDP, 64, packet);
    net_pbuf_free(packet);
    netdev_put(device);
    return status == -EINPROGRESS ? (int)length : (status ? status : (int)length);
}

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
        info->source_address = packet->source_address;
        info->source_port = packet->source_port;
        info->length = packet->length;
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

int udp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet)
{
    if (!device || !ip || !packet || packet->length < UDP_HEADER_LEN) goto bad;
    uint16_t source_port = net_read_be16(packet->data);
    uint16_t destination_port = net_read_be16(packet->data + 2);
    uint16_t length = net_read_be16(packet->data + 4);
    uint16_t checksum = net_read_be16(packet->data + 6);
    if (!destination_port || length < UDP_HEADER_LEN || length > packet->length) goto bad;
    if (checksum && net_checksum_ipv4_pseudo(ip->source, ip->destination, IPV4_PROTO_UDP, packet->data, length) != 0) goto bad;
    udp_endpoint_t *target = NULL;
    spin_lock(&udp_table_lock);
    for (unsigned i = 0; i < UDP_ENDPOINT_MAX; i++) {
        udp_endpoint_t *ep = udp_table[i];
        if (!ep || !ep->bound || ep->local_port != destination_port || (ep->local_address && ep->local_address != ip->destination)) continue;
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
        spin_unlock(&target->lock);
        spin_unlock(&udp_table_lock);
        net_pbuf_free(packet);
        return -ENOBUFS;
    }
    udp_packet_t *queued = malloc(sizeof(*queued) + payload_length);
    if (!queued) {
        spin_unlock(&target->lock);
        spin_unlock(&udp_table_lock);
        net_pbuf_free(packet);
        return -ENOMEM;
    }
    queued->next = NULL;
    queued->source_address = ip->source;
    queued->source_port = source_port;
    queued->length = payload_length;
    if (payload_length) memcpy(queued->data, packet->data + UDP_HEADER_LEN, payload_length);
    if (target->tail) target->tail->next = queued;
    else target->head = queued;
    target->tail = queued;
    target->queue_length++;
    target->queue_bytes += (uint32_t)payload_length;
    spin_unlock(&target->lock);
    spin_unlock(&udp_table_lock);
    udp_notify(target, UDP_READY_READ);
    net_pbuf_free(packet);
    return 0;
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}

uint16_t udp_local_port(const udp_endpoint_t *endpoint) { return endpoint ? endpoint->local_port : 0; }

uint32_t udp_readiness(udp_endpoint_t *endpoint)
{
    if (!endpoint) return UDP_READY_ERROR;
    spin_lock(&endpoint->lock);
    uint32_t events = UDP_READY_WRITE | (endpoint->head ? UDP_READY_READ : 0);
    spin_unlock(&endpoint->lock);
    return events;
}

int udp_get_info(udp_endpoint_t *endpoint, udp_endpoint_info_t *info)
{
    if (!endpoint || !info) return -EINVAL;
    spin_lock(&endpoint->lock);
    info->local_address = endpoint->local_address;
    info->remote_address = endpoint->remote_address;
    info->local_port = endpoint->local_port;
    info->remote_port = endpoint->remote_port;
    info->queued_datagrams = endpoint->queue_length;
    info->queued_bytes = endpoint->queue_bytes;
    info->connected = endpoint->remote_address && endpoint->remote_port;
    spin_unlock(&endpoint->lock);
    return 0;
}

void udp_set_event_callback(udp_endpoint_t *endpoint, udp_event_callback_t callback, void *context)
{
    if (!endpoint) return;
    spin_lock(&endpoint->lock);
    endpoint->event_callback = callback;
    endpoint->event_context = callback ? context : NULL;
    spin_unlock(&endpoint->lock);
    if (callback) callback(endpoint, udp_readiness(endpoint), context);
}

wait_queue_t *udp_wait_queue(udp_endpoint_t *endpoint) { return endpoint ? &endpoint->wait : NULL; }
