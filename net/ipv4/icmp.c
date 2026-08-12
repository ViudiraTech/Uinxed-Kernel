/*
 *
 *      icmp.c
 *      ICMP protocol implementation
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <net/core/endian.h>
#include <net/ipv4/icmp.h>

#define ICMP_HEADER_LEN   8U
#define ICMP_QUOTE_LEN    8U
#define ICMP_ENDPOINT_MAX 16U
#define ICMP_RX_QUEUE_MAX 64U
#define ICMP_RX_BYTES_MAX 131072U

typedef struct icmp_packet {
        struct icmp_packet *next;
        uint32_t            source;
        size_t              length;
        uint8_t             data[];
} icmp_packet_t;

typedef struct icmp_endpoint {
        uint32_t              local_address;
        uint32_t              remote_address;
        uint16_t              queue_length;
        uint32_t              queue_bytes;
        icmp_packet_t        *head;
        icmp_packet_t        *tail;
        spinlock_t            lock;
        icmp_event_callback_t event_callback;
        void                 *event_context;
} icmp_endpoint_t;

static icmp_endpoint_t *icmp_table[ICMP_ENDPOINT_MAX];
static spinlock_t       icmp_table_lock;

/* Allocate an ICMP endpoint and register it in the global table. */
icmp_endpoint_t *icmp_open(void)
{
    icmp_endpoint_t *endpoint = calloc(1, sizeof(*endpoint));
    if (!endpoint) return NULL;
    spin_lock(&icmp_table_lock);
    for (unsigned i = 0; i < ICMP_ENDPOINT_MAX; i++) {
        if (!icmp_table[i]) {
            icmp_table[i] = endpoint;
            spin_unlock(&icmp_table_lock);
            return endpoint;
        }
    }
    spin_unlock(&icmp_table_lock);
    free(endpoint);
    return NULL;
}

/* Unregister and free an endpoint, draining its queued packets. */
void icmp_close(icmp_endpoint_t *endpoint)
{
    if (!endpoint) return;
    spin_lock(&icmp_table_lock);
    for (unsigned i = 0; i < ICMP_ENDPOINT_MAX; i++)
        if (icmp_table[i] == endpoint) icmp_table[i] = NULL;
    spin_unlock(&icmp_table_lock);
    spin_lock(&endpoint->lock);
    icmp_packet_t *packet = endpoint->head;
    endpoint->head = endpoint->tail = NULL;
    spin_unlock(&endpoint->lock);
    while (packet) {
        icmp_packet_t *next = packet->next;
        free(packet);
        packet = next;
    }
    free(endpoint);
}

/* Bind the endpoint to a local address for RX filtering. */
int icmp_bind(icmp_endpoint_t *endpoint, uint32_t address)
{
    if (!endpoint) return -EINVAL;
    spin_lock(&endpoint->lock);
    endpoint->local_address = address;
    spin_unlock(&endpoint->lock);
    return 0;
}

/* Restrict the endpoint to a single remote address. */
int icmp_connect(icmp_endpoint_t *endpoint, uint32_t address)
{
    if (!endpoint || !address) return -EINVAL;
    spin_lock(&endpoint->lock);
    endpoint->remote_address = address;
    spin_unlock(&endpoint->lock);
    return 0;
}

/* Drop the remote-address restriction (revert to connected-to-any). */
int icmp_disconnect(icmp_endpoint_t *endpoint)
{
    if (!endpoint) return -EINVAL;
    spin_lock(&endpoint->lock);
    endpoint->remote_address = 0;
    spin_unlock(&endpoint->lock);
    return 0;
}

/* Send an ICMP payload to destination, routing through the IPv4 layer. */
int icmp_send(icmp_endpoint_t *endpoint, const void *data, size_t length, uint32_t destination, uint8_t ttl)
{
    if (!endpoint || (!data && length)) return -EINVAL;
    if (length > UINT16_MAX - IPV4_HEADER_MIN) return -EMSGSIZE;
    if (!destination) destination = endpoint->remote_address;
    if (!destination) return -EDESTADDRREQ;
    net_device_t *device;
    uint32_t      next_hop;
    int           status = ipv4_route(destination, &device, &next_hop);
    if (status) return status;
    net_pbuf_t *packet = net_pbuf_from(data, length, NET_PBUF_HEADROOM);
    if (!packet) {
        plogk("icmp: Send alloc failed (dest=%u.%u.%u.%u len=%lu)\n", (unsigned)(destination >> 24) & 0xff, (unsigned)(destination >> 16) & 0xff, (unsigned)(destination >> 8) & 0xff,
              (unsigned)destination & 0xff, (unsigned long)length);
        netdev_put(device);
        return -ENOMEM;
    }
    uint32_t source = endpoint->local_address ? endpoint->local_address : device->ipv4_address;
    status          = ipv4_output(device, source, destination, IPV4_PROTO_ICMP, ttl, packet);
    net_pbuf_free(packet);
    netdev_put(device);
    return status == -EINPROGRESS ? (int)length : (status ? status : (int)length);
}

/* Dequeue (or peek) the next queued ICMP datagram, filling its source. */
int icmp_receive(icmp_endpoint_t *endpoint, void *data, size_t capacity, uint32_t *source, int peek)
{
    if (!endpoint || (!data && capacity)) return -EINVAL;
    spin_lock(&endpoint->lock);
    icmp_packet_t *packet = endpoint->head;
    if (!packet) {
        spin_unlock(&endpoint->lock);
        return -EAGAIN;
    }
    size_t copied = packet->length < capacity ? packet->length : capacity;
    if (copied) memcpy(data, packet->data, copied);
    if (source) *source = packet->source;
    if (!peek) {
        endpoint->head = packet->next;
        if (!endpoint->head) endpoint->tail = NULL;
        endpoint->queue_length--;
        endpoint->queue_bytes -= (uint32_t)packet->length;
    }
    spin_unlock(&endpoint->lock);
    if (!peek) free(packet);
    return (int)copied;
}

/* Report the endpoint's current read/write readiness mask. */
uint32_t icmp_readiness(icmp_endpoint_t *endpoint)
{
    if (!endpoint) return 0;
    spin_lock(&endpoint->lock);
    uint32_t events = ICMP_READY_WRITE | (endpoint->head ? ICMP_READY_READ : 0);
    spin_unlock(&endpoint->lock);
    return events;
}

/* Install the event callback and fire it once with current readiness. */
void icmp_set_event_callback(icmp_endpoint_t *endpoint, icmp_event_callback_t callback, void *context)
{
    if (!endpoint) return;
    spin_lock(&endpoint->lock);
    endpoint->event_callback = callback;
    endpoint->event_context  = callback ? context : NULL;
    spin_unlock(&endpoint->lock);
    if (callback) callback(endpoint, icmp_readiness(endpoint), context);
}

/* Queue a received ICMP message to every matching endpoint. */
static void icmp_deliver(const ipv4_info_t *ip, const net_pbuf_t *packet)
{
    size_t length = IPV4_HEADER_MIN + packet->length;
    spin_lock(&icmp_table_lock);
    for (unsigned i = 0; i < ICMP_ENDPOINT_MAX; i++) {
        icmp_endpoint_t *endpoint = icmp_table[i];
        if (!endpoint) continue;
        spin_lock(&endpoint->lock);
        if ((endpoint->local_address && endpoint->local_address != ip->destination) || (endpoint->remote_address && endpoint->remote_address != ip->source)
            || endpoint->queue_length >= ICMP_RX_QUEUE_MAX || length > ICMP_RX_BYTES_MAX - endpoint->queue_bytes) {
            spin_unlock(&endpoint->lock);
            continue;
        }
        icmp_packet_t *queued = malloc(sizeof(*queued) + length);
        if (!queued) {
            plogk("icmp: RX queue alloc failed (src=%u.%u.%u.%u len=%lu)\n", (unsigned)(ip->source >> 24) & 0xff, (unsigned)(ip->source >> 16) & 0xff, (unsigned)(ip->source >> 8) & 0xff,
                  (unsigned)ip->source & 0xff, (unsigned long)length);
            spin_unlock(&endpoint->lock);
            continue;
        }
        queued->next   = NULL;
        queued->source = ip->source;
        queued->length = length;
        memset(queued->data, 0, IPV4_HEADER_MIN);
        queued->data[0] = 0x45;
        net_write_be16(queued->data + 2, (uint16_t)length);
        net_write_be16(queued->data + 4, ip->identification);
        queued->data[8] = ip->ttl;
        queued->data[9] = IPV4_PROTO_ICMP;
        net_write_be32(queued->data + 12, ip->source);
        net_write_be32(queued->data + 16, ip->destination);
        net_write_be16(queued->data + 10, net_checksum(queued->data, IPV4_HEADER_MIN));
        memcpy(queued->data + IPV4_HEADER_MIN, packet->data, packet->length);
        if (endpoint->tail)
            endpoint->tail->next = queued;
        else
            endpoint->head = queued;
        endpoint->tail = queued;
        endpoint->queue_length++;
        endpoint->queue_bytes += (uint32_t)length;
        icmp_event_callback_t callback = endpoint->event_callback;
        void                 *context  = endpoint->event_context;
        spin_unlock(&endpoint->lock);
        if (callback) callback(endpoint, ICMP_READY_READ, context);
    }
    spin_unlock(&icmp_table_lock);
}

/* True for ICMP error types that must not themselves trigger an error reply. */
static int icmp_is_error(uint8_t type)
{
    return type == ICMP_DEST_UNREACHABLE || type == 4U || type == 5U || type == ICMP_TIME_EXCEEDED || type == 12U;
}

/* Handle an inbound ICMP message: echo replies, delivery, and errors. */
int icmp_input(net_device_t *device, const ipv4_info_t *ip, net_pbuf_t *packet)
{
    if (!device || !ip || !packet || packet->length < ICMP_HEADER_LEN || net_checksum(packet->data, packet->length) != 0) goto bad;
    icmp_deliver(ip, packet);
    uint8_t type = packet->data[0];
    uint8_t code = packet->data[1];
    if (type == ICMP_ECHO_REQUEST) {
        if (code || ip->destination == UINT32_MAX || (device->ipv4_netmask && ip->destination == (device->ipv4_address | ~device->ipv4_netmask))) goto ignored;
        packet->data[0] = ICMP_ECHO_REPLY;
        packet->data[2] = packet->data[3] = 0;
        net_write_be16(packet->data + 2, net_checksum(packet->data, packet->length));
        int status = ipv4_output(device, device->ipv4_address, ip->source, IPV4_PROTO_ICMP, 64, packet);
        net_pbuf_free(packet);
        return status;
    }
    if ((type == ICMP_DEST_UNREACHABLE || type == ICMP_TIME_EXCEEDED) && packet->length >= ICMP_HEADER_LEN + IPV4_HEADER_MIN) {
        uint32_t mtu = type == ICMP_DEST_UNREACHABLE && code == ICMP_FRAGMENTATION_NEEDED ? net_read_be16(packet->data + 6) : 0;
        ipv4_control_error(type, code, mtu, packet->data + ICMP_HEADER_LEN, packet->length - ICMP_HEADER_LEN);
    }
ignored:
    net_pbuf_free(packet);
    return 0;
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}

/* Send an ICMP error message quoting the offending IP packet. */
int icmp_error_mtu(net_device_t *device, uint32_t destination, uint8_t type, uint8_t code, uint16_t mtu, const void *original, size_t original_length)
{
    if (!device || !destination || !original || original_length < IPV4_HEADER_MIN) return -EINVAL;
    const uint8_t *ip            = original;
    size_t         header_length = (size_t)(ip[0] & 0x0fU) * 4U;
    if ((ip[0] >> 4) != 4 || header_length < IPV4_HEADER_MIN || header_length > original_length || (net_read_be16(ip + 6) & IPV4_FRAGMENT_MASK) || destination == UINT32_MAX
        || (device->ipv4_netmask && destination == (device->ipv4_address | ~device->ipv4_netmask)))
        return -EINVAL;
    if (ip[9] == IPV4_PROTO_ICMP && original_length > header_length && icmp_is_error(ip[header_length])) return -EINVAL;
    size_t quote_length = header_length + ICMP_QUOTE_LEN;
    if (quote_length > original_length) quote_length = original_length;
    net_pbuf_t *packet = net_pbuf_alloc(ICMP_HEADER_LEN + quote_length, NET_PBUF_HEADROOM);
    if (!packet) {
        plogk("icmp: Error message alloc failed (type=%u code=%u dest=%u.%u.%u.%u)\n", (unsigned)type, (unsigned)code, (unsigned)(destination >> 24) & 0xff, (unsigned)(destination >> 16) & 0xff,
              (unsigned)(destination >> 8) & 0xff, (unsigned)destination & 0xff);
        return -ENOMEM;
    }
    memset(packet->data, 0, ICMP_HEADER_LEN);
    packet->data[0] = type;
    packet->data[1] = code;
    if (type == ICMP_DEST_UNREACHABLE && code == ICMP_FRAGMENTATION_NEEDED) net_write_be16(packet->data + 6, mtu);
    memcpy(packet->data + ICMP_HEADER_LEN, original, quote_length);
    net_write_be16(packet->data + 2, net_checksum(packet->data, packet->length));
    int status = ipv4_output(device, device->ipv4_address, destination, IPV4_PROTO_ICMP, 64, packet);
    net_pbuf_free(packet);
    return status;
}

/* Send an ICMP error without an MTU field. */
int icmp_error(net_device_t *device, uint32_t destination, uint8_t type, uint8_t code, const void *original, size_t original_length)
{
    return icmp_error_mtu(device, destination, type, code, 0, original, original_length);
}
