/*
 *
 *      ipv4.c
 *      IPv4 protocol implementation
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <net/core/endian.h>
#include <net/ipv4/arp.h>
#include <net/ipv4/icmp.h>
#include <net/ipv4/ipv4.h>
#include <net/transport/tcp.h>
#include <net/transport/udp.h>
#include <process/sched.h>

#define IPV4_REASSEMBLY_TIMEOUT_TICKS ((uint64_t)30U * TIMER_HZ)
#define IPV4_MAX_HEADER               60U
#define IPV4_MAX_PAYLOAD              (UINT16_MAX - IPV4_HEADER_MIN)
#define IPV4_BITMAP_SIZE              ((IPV4_MAX_PAYLOAD + 7U) / 8U)

typedef struct ipv4_reassembly {
        net_device_t *device;
        uint32_t      source;
        uint32_t      destination;
        uint16_t      identification;
        uint16_t      total_length;
        uint16_t      received;
        uint16_t      highest_end;
        uint8_t       protocol;
        uint8_t       have_first;
        uint8_t       have_last;
        uint8_t       first_header[IPV4_MAX_HEADER];
        uint8_t       first_header_length;
        uint64_t      expires;
        uint8_t      *data;
        uint8_t      *bitmap;
} ipv4_reassembly_t;

static uint16_t          ipv4_id;
static spinlock_t        ipv4_id_lock;
static ipv4_reassembly_t ipv4_reassembly[IPV4_REASSEMBLY_SLOTS];
static spinlock_t        ipv4_reassembly_lock;
static ipv4_error_hook_t ipv4_error_hook;
static spinlock_t        ipv4_hook_lock;

static int ipv4_is_multicast(uint32_t address)
{
    return (address & 0xf0000000U) == 0xe0000000U;
}

static int ipv4_is_directed_broadcast(const net_device_t *device, uint32_t address)
{
    return device->ipv4_netmask && device->ipv4_netmask != UINT32_MAX && address == (device->ipv4_address | ~device->ipv4_netmask);
}

static int ipv4_source_valid(uint32_t address)
{
    return address && address != UINT32_MAX && !ipv4_is_multicast(address) && (address >> 24) != 127U;
}

int net_ipv4_parse(const void *data, size_t length, net_ipv4_packet_t *packet)
{
    if (!data || !packet || length < IPV4_HEADER_MIN) return -EBADMSG;
    const uint8_t *bytes         = data;
    size_t         header_length = (size_t)(bytes[0] & 0x0fU) * 4U;
    if ((bytes[0] >> 4) != 4 || header_length < IPV4_HEADER_MIN || header_length > IPV4_MAX_HEADER || header_length > length) return -EBADMSG;
    uint16_t total    = net_read_be16(bytes + 2);
    uint16_t fragment = net_read_be16(bytes + 6);
    if (total < header_length || total > length || (fragment & 0x8000U) || net_checksum(bytes, header_length) != 0) return -EBADMSG;
    packet->header_len      = (uint8_t)header_length;
    packet->total_len       = total;
    packet->protocol        = bytes[9];
    packet->source          = net_read_be32(bytes + 12);
    packet->destination     = net_read_be32(bytes + 16);
    packet->identification  = net_read_be16(bytes + 4);
    packet->fragment_offset = (uint16_t)((fragment & IPV4_FRAGMENT_MASK) * 8U);
    packet->more_fragments  = !!(fragment & IPV4_FLAG_MF);
    packet->payload         = bytes + header_length;
    packet->payload_len     = total - header_length;
    if ((packet->more_fragments && (packet->payload_len & 7U)) || packet->fragment_offset + packet->payload_len > IPV4_MAX_PAYLOAD)
        return -EBADMSG;
    return 0;
}

typedef struct ipv4_route_search {
        uint32_t      destination;
        net_device_t *direct;
        net_device_t *gateway;
        net_device_t *fallback;
        unsigned      prefix;
} ipv4_route_search_t;

static unsigned ipv4_prefix_length(uint32_t mask)
{
    unsigned length = 0;
    while (mask & 0x80000000U) {
        length++;
        mask <<= 1;
    }
    return length;
}

static void ipv4_route_visit(net_device_t *device, void *context)
{
    ipv4_route_search_t *search = context;
    if (!device->ipv4_address || (device->flags & (NETDEV_F_UP | NETDEV_F_RUNNING)) != (NETDEV_F_UP | NETDEV_F_RUNNING)) return;
    if (!search->fallback) {
        netdev_get(device);
        search->fallback = device;
    }
    if (device->ipv4_netmask && (search->destination & device->ipv4_netmask) == (device->ipv4_address & device->ipv4_netmask)) {
        unsigned prefix = ipv4_prefix_length(device->ipv4_netmask);
        if (!search->direct || prefix > search->prefix) {
            if (search->direct) netdev_put(search->direct);
            netdev_get(device);
            search->direct = device;
            search->prefix = prefix;
        }
    } else if (!search->gateway && device->ipv4_gateway) {
        netdev_get(device);
        search->gateway = device;
    }
}

int ipv4_route(uint32_t destination, net_device_t **device, uint32_t *next_hop)
{
    if (!device || !next_hop || !destination || ipv4_is_multicast(destination) || (destination >> 24) == 127U) return -EINVAL;
    ipv4_route_search_t search = {.destination = destination};
    netdev_iterate(ipv4_route_visit, &search);
    if (search.direct) {
        if (search.gateway) netdev_put(search.gateway);
        if (search.fallback) netdev_put(search.fallback);
        *device   = search.direct;
        *next_hop = destination;
        return 0;
    }
    if (search.gateway) {
        if (search.fallback) netdev_put(search.fallback);
        *device   = search.gateway;
        *next_hop = destination == UINT32_MAX ? destination : search.gateway->ipv4_gateway;
        return 0;
    }
    if (destination == UINT32_MAX && search.fallback) {
        *device   = search.fallback;
        *next_hop = destination;
        return 0;
    }
    if (search.fallback) netdev_put(search.fallback);
    static uint64_t last_log;
    if (sched_ticks() - last_log >= 1000) {
        plogk("ipv4: No route to %u.%u.%u.%u\n", (unsigned)(destination >> 24) & 0xff, (unsigned)(destination >> 16) & 0xff,
              (unsigned)(destination >> 8) & 0xff, (unsigned)destination & 0xff);
        last_log = sched_ticks();
    }
    return -ENETUNREACH;
}

static uint16_t ipv4_next_id(void)
{
    spin_lock(&ipv4_id_lock);
    uint16_t id = ipv4_id;
    ipv4_id     = (uint16_t)(ipv4_id + 1U + (uint16_t)((uint32_t)ipv4_id * 3U >> 8));
    spin_unlock(&ipv4_id_lock);
    return id;
}

static int ipv4_emit_fragment(net_device_t *device, uint32_t next_hop, uint32_t source, uint32_t destination, uint8_t protocol, uint8_t ttl,
                              uint16_t id, uint16_t flags_offset, const uint8_t *data, size_t length)
{
    net_pbuf_t *fragment = net_pbuf_alloc(IPV4_HEADER_MIN + length, NET_PBUF_HEADROOM);
    if (!fragment) {
        plogk("ipv4: %s: Fragment alloc failed (dest=%u.%u.%u.%u len=%lu)\n", device->name, (unsigned)(destination >> 24) & 0xff,
              (unsigned)(destination >> 16) & 0xff, (unsigned)(destination >> 8) & 0xff, (unsigned)destination & 0xff, (unsigned long)length);
        return -ENOMEM;
    }
    uint8_t *header = fragment->data;
    memset(header, 0, IPV4_HEADER_MIN);
    header[0] = 0x45;
    net_write_be16(header + 2, (uint16_t)fragment->length);
    net_write_be16(header + 4, id);
    net_write_be16(header + 6, flags_offset);
    header[8] = ttl ? ttl : 64;
    header[9] = protocol;
    net_write_be32(header + 12, source);
    net_write_be32(header + 16, destination);
    if (length) memcpy(header + IPV4_HEADER_MIN, data, length);
    net_write_be16(header + 10, net_checksum(header, IPV4_HEADER_MIN));
    int status = arp_resolve(device, next_hop, fragment);
    net_pbuf_free(fragment);
    return status;
}

int ipv4_output(net_device_t *device, uint32_t source, uint32_t destination, uint8_t protocol, uint8_t ttl, net_pbuf_t *packet)
{
    if (!packet || !destination || packet->length > IPV4_MAX_PAYLOAD || ipv4_is_multicast(destination) || (destination >> 24) == 127U)
        return -EMSGSIZE;
    uint32_t next_hop;
    int      release = 0;
    if (!device) {
        int status = ipv4_route(destination, &device, &next_hop);
        if (status) return status;
        release = 1;
    } else if (destination == UINT32_MAX || ipv4_is_directed_broadcast(device, destination)
               || (device->ipv4_netmask && (destination & device->ipv4_netmask) == (device->ipv4_address & device->ipv4_netmask))) {
        next_hop = destination;
    } else {
        next_hop = device->ipv4_gateway;
    }
    if (!source) source = device->ipv4_address;
    if (!ipv4_source_valid(source) || !next_hop || device->mtu <= IPV4_HEADER_MIN) {
        static uint64_t last_log;
        if (sched_ticks() - last_log >= 1000) {
            plogk("ipv4: %s: Output dropped (source %u.%u.%u.%u, next hop %u.%u.%u.%u)\n", device->name, (unsigned)(source >> 24) & 0xff,
                  (unsigned)(source >> 16) & 0xff, (unsigned)(source >> 8) & 0xff, (unsigned)source & 0xff, (unsigned)(next_hop >> 24) & 0xff,
                  (unsigned)(next_hop >> 16) & 0xff, (unsigned)(next_hop >> 8) & 0xff, (unsigned)next_hop & 0xff);
            last_log = sched_ticks();
        }
        if (release) netdev_put(device);
        return -ENETUNREACH;
    }
    size_t fragment_payload = (device->mtu - IPV4_HEADER_MIN) & ~7U;
    if (!fragment_payload) {
        if (release) netdev_put(device);
        return -EMSGSIZE;
    }
    uint16_t id     = ipv4_next_id();
    int      result = 0;
    for (size_t offset = 0; offset < packet->length || (!packet->length && !offset);) {
        size_t length = packet->length - offset;
        if (length > fragment_payload) length = fragment_payload;
        uint16_t fragment = (uint16_t)(offset / 8U);
        if (offset + length < packet->length) fragment |= IPV4_FLAG_MF;
        int status = ipv4_emit_fragment(device, next_hop, source, destination, protocol, ttl, id, fragment, packet->data + offset, length);
        if (status && status != -EINPROGRESS) {
            result = status;
            break;
        }
        if (status == -EINPROGRESS) result = -EINPROGRESS;
        if (!packet->length) break;
        offset += length;
    }
    if (release) netdev_put(device);
    return result;
}

static void ipv4_reassembly_clear(ipv4_reassembly_t *entry)
{
    free(entry->data);
    free(entry->bitmap);
    memset(entry, 0, sizeof(*entry));
}

static ipv4_reassembly_t *ipv4_reassembly_find(net_device_t *device, const net_ipv4_packet_t *ip, uint64_t now)
{
    ipv4_reassembly_t *free_entry = NULL;
    ipv4_reassembly_t *oldest     = NULL;
    for (unsigned i = 0; i < IPV4_REASSEMBLY_SLOTS; i++) {
        ipv4_reassembly_t *entry = &ipv4_reassembly[i];
        if (entry->device == device && entry->source == ip->source && entry->destination == ip->destination
            && entry->identification == ip->identification && entry->protocol == ip->protocol)
            return entry;
        if (!entry->device)
            free_entry = entry;
        else if (!oldest || entry->expires < oldest->expires)
            oldest = entry;
    }
    ipv4_reassembly_t *entry = free_entry ? free_entry : oldest;
    if (!entry) return NULL;
    if (entry->device) ipv4_reassembly_clear(entry);
    entry->data   = malloc(IPV4_MAX_PAYLOAD);
    entry->bitmap = malloc(IPV4_BITMAP_SIZE);
    if (!entry->data || !entry->bitmap) {
        plogk("ipv4: %s: Reassembly buffer alloc failed (src=%u.%u.%u.%u id=%u)\n", device->name, (unsigned)(ip->source >> 24) & 0xff,
              (unsigned)(ip->source >> 16) & 0xff, (unsigned)(ip->source >> 8) & 0xff, (unsigned)ip->source & 0xff,
              (unsigned)ip->identification);
        ipv4_reassembly_clear(entry);
        return NULL;
    }
    memset(entry->bitmap, 0, IPV4_BITMAP_SIZE);
    entry->device         = device;
    entry->source         = ip->source;
    entry->destination    = ip->destination;
    entry->identification = ip->identification;
    entry->protocol       = ip->protocol;
    entry->expires        = now + IPV4_REASSEMBLY_TIMEOUT_TICKS;
    return entry;
}

static net_pbuf_t *ipv4_reassemble(net_device_t *device, const net_ipv4_packet_t *ip, const uint8_t *header, uint64_t now, uint8_t *quote,
                                   size_t *quote_length)
{
    net_pbuf_t *complete = NULL;
    spin_lock(&ipv4_reassembly_lock);
    ipv4_reassembly_t *entry = ipv4_reassembly_find(device, ip, now);
    if (!entry) goto out;
    size_t end = ip->fragment_offset + ip->payload_len;
    if ((entry->have_last && end > entry->total_length)
        || (!ip->more_fragments && ((entry->have_last && end != entry->total_length) || entry->highest_end > end)))
        goto overlap;
    {
        uint8_t *bm      = entry->bitmap;
        size_t   off     = ip->fragment_offset;
        size_t   b_start = off >> 3;
        size_t   b_end   = (end + 7) >> 3;
        for (size_t j = b_start; j < b_end; j++) {
            uint8_t m = 0xff;
            if (j == b_start) m &= (uint8_t)(0xff << (off & 7));
            if (j == b_end - 1) {
                unsigned t = end & 7;
                if (t) m &= (uint8_t)(0xff >> (8 - t));
            }
            if (bm[j] & m) goto overlap;
        }
    }
    memcpy(entry->data + ip->fragment_offset, ip->payload, ip->payload_len);
    {
        uint8_t *bm      = entry->bitmap;
        size_t   off     = ip->fragment_offset;
        size_t   b_start = off >> 3;
        size_t   b_end   = (end + 7) >> 3;
        for (size_t j = b_start; j < b_end; j++) {
            uint8_t m = 0xff;
            if (j == b_start) m &= (uint8_t)(0xff << (off & 7));
            if (j == b_end - 1) {
                unsigned t = end & 7;
                if (t) m &= (uint8_t)(0xff >> (8 - t));
            }
            bm[j] |= m;
        }
    }
    entry->received = (uint16_t)(entry->received + ip->payload_len);
    if (end > entry->highest_end) entry->highest_end = (uint16_t)end;
    entry->expires = now + IPV4_REASSEMBLY_TIMEOUT_TICKS;
    if (!ip->fragment_offset) {
        entry->have_first          = 1;
        entry->first_header_length = ip->header_len;
        memcpy(entry->first_header, header, ip->header_len);
    }
    if (!ip->more_fragments) {
        entry->have_last    = 1;
        entry->total_length = (uint16_t)end;
    }
    if (entry->have_first && entry->have_last && entry->received == entry->total_length) {
        complete = net_pbuf_from(entry->data, entry->total_length, NET_PBUF_HEADROOM);
        if (complete && quote && quote_length) {
            size_t payload_quote = entry->total_length < 8U ? entry->total_length : 8U;
            *quote_length        = entry->first_header_length + payload_quote;
            memcpy(quote, entry->first_header, entry->first_header_length);
            memcpy(quote + entry->first_header_length, entry->data, payload_quote);
        }
        ipv4_reassembly_clear(entry);
    }
    goto out;
overlap:
    ipv4_reassembly_clear(entry);
out:
    spin_unlock(&ipv4_reassembly_lock);
    return complete;
}

static int ipv4_dispatch(net_device_t *device, const ipv4_info_t *info, net_pbuf_t *packet, const void *quoted, size_t quoted_length,
                         int may_error)
{
    int status;
    if (info->protocol == IPV4_PROTO_ICMP) return icmp_input(device, info, packet);
    if (info->protocol == IPV4_PROTO_UDP)
        status = udp_input(device, info, packet);
    else if (info->protocol == IPV4_PROTO_TCP)
        status = tcp_input(device, info, packet);
    else {
        net_pbuf_free(packet);
        status = -EPROTONOSUPPORT;
    }
    if (may_error && status == -ECONNREFUSED)
        icmp_error(device, info->source, ICMP_DEST_UNREACHABLE, ICMP_PORT_UNREACHABLE, quoted, quoted_length);
    else if (may_error && status == -EPROTONOSUPPORT)
        icmp_error(device, info->source, ICMP_DEST_UNREACHABLE, ICMP_PROTOCOL_UNREACHABLE, quoted, quoted_length);
    return status;
}

int ipv4_input(net_device_t *device, net_pbuf_t *packet)
{
    if (!device || !packet) goto bad;
    net_ipv4_packet_t parsed;
    if (net_ipv4_parse(packet->data, packet->length, &parsed)) goto bad;
    if (!ipv4_source_valid(parsed.source)) goto bad;
    uint32_t broadcast    = device->ipv4_netmask ? device->ipv4_address | ~device->ipv4_netmask : UINT32_MAX;
    int      is_broadcast = parsed.destination == UINT32_MAX || parsed.destination == broadcast;
    if (parsed.destination != device->ipv4_address && !is_broadcast) {
        net_pbuf_free(packet);
        return -EHOSTUNREACH;
    }
    if (parsed.destination == UINT32_MAX && (parsed.fragment_offset || parsed.more_fragments)) goto bad;
    ipv4_info_t info = {
        .source          = parsed.source,
        .destination     = parsed.destination,
        .payload_length  = (uint16_t)parsed.payload_len,
        .protocol        = parsed.protocol,
        .ttl             = packet->data[8],
        .identification  = parsed.identification,
        .fragment_offset = parsed.fragment_offset,
        .more_fragments  = parsed.more_fragments,
    };
    uint8_t quoted[IPV4_MAX_HEADER + 8U];
    size_t  quoted_length = parsed.total_len < sizeof(quoted) ? parsed.total_len : sizeof(quoted);
    memcpy(quoted, packet->data, quoted_length);
    if (parsed.fragment_offset || parsed.more_fragments) {
        net_pbuf_t *complete = ipv4_reassemble(device, &parsed, packet->data, sched_ticks(), quoted, &quoted_length);
        net_pbuf_free(packet);
        if (!complete) return -EINPROGRESS;
        info.payload_length  = (uint16_t)complete->length;
        info.fragment_offset = 0;
        info.more_fragments  = 0;
        return ipv4_dispatch(device, &info, complete, quoted, quoted_length, !is_broadcast);
    }
    net_pbuf_trim(packet, parsed.total_len);
    net_pbuf_pull(packet, parsed.header_len);
    return ipv4_dispatch(device, &info, packet, quoted, quoted_length, !is_broadcast);
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}

int ipv4_set_error_hook(ipv4_error_hook_t hook)
{
    spin_lock(&ipv4_hook_lock);
    if (ipv4_error_hook && hook && ipv4_error_hook != hook) {
        spin_unlock(&ipv4_hook_lock);
        return -EBUSY;
    }
    ipv4_error_hook = hook;
    spin_unlock(&ipv4_hook_lock);
    return 0;
}

void ipv4_control_error(uint8_t type, uint8_t code, uint32_t mtu, const void *quoted, size_t quoted_length)
{
    if (!quoted || quoted_length < IPV4_HEADER_MIN) return;
    const uint8_t *bytes         = quoted;
    size_t         header_length = (size_t)(bytes[0] & 0x0fU) * 4U;
    if ((bytes[0] >> 4) != 4 || header_length < IPV4_HEADER_MIN || header_length > quoted_length
        || (net_read_be16(bytes + 6) & IPV4_FRAGMENT_MASK))
        return;
    int error = 0;
    if (type == ICMP_DEST_UNREACHABLE) {
        if (code == ICMP_FRAGMENTATION_NEEDED)
            error = -EMSGSIZE;
        else if (code == ICMP_PORT_UNREACHABLE)
            error = -ECONNREFUSED;
        else if (code == ICMP_NET_UNREACHABLE || code == ICMP_HOST_UNREACHABLE)
            error = -EHOSTUNREACH;
    } else if (type == ICMP_TIME_EXCEEDED)
        error = -ETIMEDOUT;
    if (!error) return;
    spin_lock(&ipv4_hook_lock);
    ipv4_error_hook_t hook = ipv4_error_hook;
    spin_unlock(&ipv4_hook_lock);
    if (hook)
        hook(bytes[9], net_read_be32(bytes + 12), net_read_be32(bytes + 16), bytes + header_length, quoted_length - header_length, error, mtu);
}

void ipv4_timer(uint64_t now_ticks)
{
    for (unsigned i = 0; i < IPV4_REASSEMBLY_SLOTS; i++) {
        uint8_t       quote[IPV4_MAX_HEADER + 8U];
        size_t        quote_length = 0;
        net_device_t *device       = NULL;
        uint32_t      destination  = 0;
        spin_lock(&ipv4_reassembly_lock);
        ipv4_reassembly_t *entry = &ipv4_reassembly[i];
        if (entry->device && now_ticks >= entry->expires) {
            if (entry->have_first) {
                quote_length = entry->first_header_length + (entry->highest_end < 8U ? entry->highest_end : 8U);
                memcpy(quote, entry->first_header, entry->first_header_length);
                memcpy(quote + entry->first_header_length, entry->data, quote_length - entry->first_header_length);
                device      = entry->device;
                destination = entry->source;
            }
            ipv4_reassembly_clear(entry);
        }
        spin_unlock(&ipv4_reassembly_lock);
        if (device) icmp_error(device, destination, ICMP_TIME_EXCEEDED, ICMP_REASSEMBLY_TIMEOUT, quote, quote_length);
    }
}

void ipv4_device_removed(net_device_t *device)
{
    if (!device) return;
    spin_lock(&ipv4_reassembly_lock);
    for (unsigned i = 0; i < IPV4_REASSEMBLY_SLOTS; i++)
        if (ipv4_reassembly[i].device == device) ipv4_reassembly_clear(&ipv4_reassembly[i]);
    spin_unlock(&ipv4_reassembly_lock);
}
