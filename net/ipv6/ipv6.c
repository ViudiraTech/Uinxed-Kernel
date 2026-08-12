/*
 *
 *      ipv6.c
 *      IPv6 protocol implementation
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <net/core/endian.h>
#include <net/core/ethernet.h>
#include <net/ipv6/icmpv6.h>
#include <net/ipv6/ipv6.h>
#include <net/ipv6/ndp.h>
#include <process/sched.h>

#define IPV6_MAX_PAYLOAD              65535U
#define IPV6_REASSEMBLY_BITMAP_SIZE   ((IPV6_MAX_PAYLOAD + 7U) / 8U)
#define IPV6_REASSEMBLY_TIMEOUT_TICKS ((uint64_t)60U * TIMER_HZ)
#define IPV6_MAX_EXTENSION_HEADERS    8U
#define IPV6_TRANSPORT_SLOTS          4U

typedef struct ipv6_transport_slot {
        uint8_t                protocol;
        ipv6_transport_input_t handler;
} ipv6_transport_slot_t;

typedef struct ipv6_reassembly {
        net_device_t  *device;
        ipv6_address_t source;
        ipv6_address_t destination;
        uint32_t       id;
        uint16_t       total_length;
        uint16_t       received;
        uint16_t       highest_end;
        uint8_t        protocol;
        uint8_t        have_first;
        uint8_t        have_last;
        uint8_t        hop_limit;
        uint8_t        first_quote[IPV6_HEADER_LEN + 16U];
        uint64_t       expires;
        uint8_t       *data;
        uint8_t       *bitmap;
} ipv6_reassembly_t;

static ipv6_transport_slot_t ipv6_transports[IPV6_TRANSPORT_SLOTS];
static ipv6_reassembly_t     ipv6_reassembly[IPV6_REASSEMBLY_SLOTS];
static ipv6_error_hook_t     ipv6_error_hook;
static spinlock_t            ipv6_lock;
static spinlock_t            ipv6_reassembly_lock;

/*
 * Address helpers
 * Small predicates and constructors for the common IPv6 address
 * classes. All byte-wise so they are independent of host endianness.
 */

/* Byte-wise equality of two IPv6 addresses. */
int ipv6_address_equal(const ipv6_address_t *left, const ipv6_address_t *right)
{
    return left && right && !memcmp(left->bytes, right->bytes, IPV6_ADDRESS_LEN);
}

/* True if the address is all zeros (::). */
int ipv6_address_is_unspecified(const ipv6_address_t *address)
{
    static const ipv6_address_t zero;
    return address && ipv6_address_equal(address, &zero);
}

/* True if the address is ::1. */
int ipv6_address_is_loopback(const ipv6_address_t *address)
{
    static const uint8_t loopback[IPV6_ADDRESS_LEN] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    return address && !memcmp(address->bytes, loopback, sizeof(loopback));
}

/* True if the first byte is 0xff. */
int ipv6_address_is_multicast(const ipv6_address_t *address)
{
    return address && address->bytes[0] == 0xff;
}

/* True if the address is in the fe80::/10 link-local range. */
int ipv6_address_is_link_local(const ipv6_address_t *address)
{
    return address && address->bytes[0] == 0xfe && (address->bytes[1] & 0xc0U) == 0x80U;
}

/* True if the address is neither unspecified nor multicast. */
int ipv6_address_is_unicast(const ipv6_address_t *address)
{
    return address && !ipv6_address_is_unspecified(address) && !ipv6_address_is_multicast(address);
}

/* Derive the fe80::/64 link-local address from a MAC (EUI-64, flipped U/L bit). */
void ipv6_link_local_from_mac(ipv6_address_t *address, const uint8_t mac[6])
{
    if (!address || !mac) return;
    memset(address, 0, sizeof(*address));
    address->bytes[0]  = 0xfe;
    address->bytes[1]  = 0x80;
    address->bytes[8]  = mac[0] ^ 0x02U;
    address->bytes[9]  = mac[1];
    address->bytes[10] = mac[2];
    address->bytes[11] = 0xff;
    address->bytes[12] = 0xfe;
    address->bytes[13] = mac[3];
    address->bytes[14] = mac[4];
    address->bytes[15] = mac[5];
}

/* Build the ff02::1:ffxx:xxxx solicited-node multicast for an address. */
void ipv6_solicited_node(const ipv6_address_t *address, ipv6_address_t *multicast)
{
    if (!address || !multicast) return;
    memset(multicast, 0, sizeof(*multicast));
    multicast->bytes[0]  = 0xff;
    multicast->bytes[1]  = 0x02;
    multicast->bytes[11] = 0x01;
    multicast->bytes[12] = 0xff;
    memcpy(multicast->bytes + 13, address->bytes + 13, 3);
}

/* Map an IPv6 multicast address to the 33:33:xx:xx:xx:xx Ethernet MAC. */
void ipv6_multicast_ethernet(const ipv6_address_t *address, uint8_t mac[6])
{
    if (!address || !mac) return;
    mac[0] = 0x33;
    mac[1] = 0x33;
    memcpy(mac + 2, address->bytes + 12, 4);
}

/* Compute the IPv6 pseudo-header checksum for a transport segment. */
uint16_t net_checksum_ipv6_pseudo(const ipv6_address_t *source, const ipv6_address_t *destination, uint8_t protocol, const void *data,
                                  size_t length)
{
    if (!source || !destination || (!data && length) || length > UINT32_MAX) return 0;
    uint8_t pseudo[40];
    memcpy(pseudo, source->bytes, IPV6_ADDRESS_LEN);
    memcpy(pseudo + 16, destination->bytes, IPV6_ADDRESS_LEN);
    net_write_be32(pseudo + 32, (uint32_t)length);
    pseudo[36] = pseudo[37] = pseudo[38] = 0;
    pseudo[39]                           = protocol;
    return net_checksum_finish(net_checksum_add(net_checksum_add(0, pseudo, sizeof(pseudo)), data, length));
}

/* Validate hop-by-hop options, rejecting unknown or malformed ones. */
static int ipv6_parse_hop_options(const uint8_t *bytes, size_t length)
{
    size_t offset = 2;
    while (offset < length) {
        uint8_t type = bytes[offset++];
        if (!type) continue;
        if (offset >= length) return -EBADMSG;
        size_t option_length = bytes[offset++];
        if (option_length > length - offset) return -EBADMSG;
        if (type == 1) {
            for (size_t i = 0; i < option_length; i++)
                if (bytes[offset + i]) return -EBADMSG;
        } else if (type == 5) {
            if (option_length != 2) return -EBADMSG;
        } else {
            return -EOPNOTSUPP;
        }
        offset += option_length;
    }
    return 0;
}

/* Decode an IPv6 header, walking (and validating) extension headers */
int net_ipv6_parse(const void *data, size_t length, net_ipv6_packet_t *packet)
{
    if (!data || !packet || length < IPV6_HEADER_LEN) return -EBADMSG;
    const uint8_t *bytes = data;
    if ((bytes[0] >> 4) != 6) return -EBADMSG;
    uint16_t payload_length = net_read_be16(bytes + 4);
    if ((size_t)payload_length > length - IPV6_HEADER_LEN || (!payload_length && length != IPV6_HEADER_LEN)) return -EBADMSG;
    size_t ipv6_total = (size_t)IPV6_HEADER_LEN + payload_length;
    if (ipv6_total > UINT16_MAX) return -EBADMSG;

    memset(packet, 0, sizeof(*packet));
    packet->flow_label = ((uint32_t)(bytes[1] & 0x0fU) << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
    packet->total_len  = (uint16_t)ipv6_total;
    packet->hop_limit  = bytes[7];
    memcpy(packet->source.bytes, bytes + 8, IPV6_ADDRESS_LEN);
    memcpy(packet->destination.bytes, bytes + 24, IPV6_ADDRESS_LEN);

    uint8_t  next       = bytes[6];
    size_t   offset     = IPV6_HEADER_LEN;
    unsigned extensions = 0;
    while (next == IPV6_NEXT_HOP_BY_HOP || next == IPV6_NEXT_ROUTING || next == IPV6_NEXT_FRAGMENT || next == IPV6_NEXT_DEST_OPTS
           || next == IPV6_NEXT_AH || next == IPV6_NEXT_ESP) {
        if (++extensions > IPV6_MAX_EXTENSION_HEADERS) return -EBADMSG;
        if (next == IPV6_NEXT_ROUTING || next == IPV6_NEXT_DEST_OPTS || next == IPV6_NEXT_AH || next == IPV6_NEXT_ESP) return -EOPNOTSUPP;
        if (next == IPV6_NEXT_HOP_BY_HOP) {
            if (offset != IPV6_HEADER_LEN || payload_length < 8 || offset + 2 > packet->total_len) return -EBADMSG;
            size_t extension_length = ((size_t)bytes[offset + 1] + 1U) * 8U;
            if (extension_length > packet->total_len - offset) return -EBADMSG;
            int status = ipv6_parse_hop_options(bytes + offset, extension_length);
            if (status) return status;
            next = bytes[offset];
            offset += extension_length;
            continue;
        }
        if (packet->has_fragment || offset + 8U > packet->total_len) return -EBADMSG;
        uint16_t fragment = net_read_be16(bytes + offset + 2);
        if (bytes[offset + 1] || (fragment & 0x0006U)) return -EBADMSG;
        packet->has_fragment    = 1;
        packet->fragment_offset = fragment & 0xfff8U;
        packet->more_fragments  = fragment & 1U;
        packet->fragment_id     = net_read_be32(bytes + offset + 4);
        next                    = bytes[offset];
        offset += 8U;
        if (next == IPV6_NEXT_HOP_BY_HOP || next == IPV6_NEXT_ROUTING || next == IPV6_NEXT_FRAGMENT || next == IPV6_NEXT_DEST_OPTS
            || next == IPV6_NEXT_AH || next == IPV6_NEXT_ESP)
            return -EOPNOTSUPP;
    }
    if (next == IPV6_NEXT_NONE && offset != packet->total_len) return -EBADMSG;
    size_t transport_length = packet->total_len - offset;
    if (packet->more_fragments && (transport_length & 7U)) return -EBADMSG;
    if ((size_t)packet->fragment_offset + transport_length > IPV6_MAX_PAYLOAD) return -EBADMSG;
    packet->protocol         = next;
    packet->transport_offset = (uint16_t)offset;
    packet->payload_len      = (uint16_t)transport_length;
    packet->payload          = bytes + offset;
    return 0;
}

/* True if the two addresses share the first prefix bits. */
static int ipv6_prefix_matches(const uint8_t left[16], const uint8_t right[16], unsigned prefix)
{
    unsigned bytes = prefix / 8U;
    unsigned bits  = prefix & 7U;
    if (!left || !right) return 0;
    if (bytes && memcmp(left, right, bytes) != 0) return 0;
    return !bits || !((left[bytes] ^ right[bytes]) & (uint8_t)(0xffU << (8U - bits)));
}

/* True for destinations reachable on the local link (link-local or local multicast). */
static int ipv6_destination_link_scope(const ipv6_address_t *address)
{
    return ipv6_address_is_link_local(address) || (ipv6_address_is_multicast(address) && (address->bytes[1] & 0x0fU) <= 2U);
}

typedef struct ipv6_route_search {
        const ipv6_address_t *destination;
        net_device_t         *direct;
        net_device_t         *router;
} ipv6_route_search_t;

/* Candidate selection for IPv6 route lookup: direct interface or default router. */
static void ipv6_route_visit(net_device_t *device, void *context)
{
    ipv6_route_search_t *search = context;
    if ((device->flags & (NETDEV_F_UP | NETDEV_F_RUNNING)) != (NETDEV_F_UP | NETDEV_F_RUNNING)) return;
    ipv6_address_t link_local;
    memcpy(link_local.bytes, device->ipv6_link_local, 16);
    if (!ipv6_address_is_unicast(&link_local)) return;
    ipv6_address_t configured;
    memcpy(configured.bytes, device->ipv6_address, 16);
    if (!search->direct
        && (ipv6_address_is_multicast(search->destination) || ipv6_address_is_link_local(search->destination)
            || (device->ipv6_prefix_length && ipv6_address_is_unicast(&configured)
                && ipv6_prefix_matches(configured.bytes, search->destination->bytes, device->ipv6_prefix_length)))) {
        netdev_get(device);
        search->direct = device;
    }
    if (!search->router && device->ipv6_router_until > sched_ticks()) {
        netdev_get(device);
        search->router = device;
    }
}

/* Find an interface that can reach destination, choosing source and next hop */
/* Choose the device, source address, and next hop for a destination. */
int ipv6_route(const ipv6_address_t *destination, net_device_t **device, ipv6_address_t *source, ipv6_address_t *next_hop)
{
    if (!destination || !device || !source || !next_hop || ipv6_address_is_unspecified(destination) || ipv6_address_is_loopback(destination))
        return -EINVAL;
    ipv6_route_search_t search = {.destination = destination};
    netdev_iterate(ipv6_route_visit, &search);
    net_device_t *selected = search.direct ? search.direct : search.router;
    if (!selected) {
        static uint64_t last_log;
        if (sched_ticks() - last_log >= 1000) {
            plogk("ipv6: No route to %02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x\n", destination->bytes[0],
                  destination->bytes[1], destination->bytes[2], destination->bytes[3], destination->bytes[4], destination->bytes[5],
                  destination->bytes[6], destination->bytes[7], destination->bytes[8], destination->bytes[9], destination->bytes[10],
                  destination->bytes[11], destination->bytes[12], destination->bytes[13], destination->bytes[14], destination->bytes[15]);
            last_log = sched_ticks();
        }
        return -ENETUNREACH;
    }
    if (search.direct && search.router) netdev_put(search.router);
    memcpy(next_hop->bytes, search.direct || ipv6_address_is_multicast(destination) ? destination->bytes : selected->ipv6_default_router, 16);
    if (!ipv6_destination_link_scope(destination) && selected->ipv6_prefix_length && selected->ipv6_valid_until > sched_ticks())
        memcpy(source->bytes, selected->ipv6_address, 16);
    else
        memcpy(source->bytes, selected->ipv6_link_local, 16);
    *device = selected;
    return 0;
}

/* Transmit a packet, prepending the IPv6 header and resolving the next hop. */
int ipv6_output(net_device_t *device, const ipv6_address_t *source, const ipv6_address_t *destination, uint8_t protocol, uint8_t hop_limit,
                net_pbuf_t *packet)
{
    if (!destination || !packet || ipv6_address_is_unspecified(destination) || ipv6_address_is_loopback(destination)
        || packet->length > IPV6_MAX_PAYLOAD)
        return -EINVAL;
    ipv6_address_t selected_source, next_hop;
    int            release = 0;
    if (!device) {
        int status = ipv6_route(destination, &device, &selected_source, &next_hop);
        if (status) return status;
        release = 1;
    } else {
        memcpy(selected_source.bytes, device->ipv6_link_local, 16);
        if (!ipv6_destination_link_scope(destination) && device->ipv6_prefix_length && device->ipv6_valid_until > sched_ticks())
            memcpy(selected_source.bytes, device->ipv6_address, 16);
        if (ipv6_address_is_multicast(destination) || ipv6_address_is_link_local(destination)
            || (device->ipv6_prefix_length && ipv6_prefix_matches(device->ipv6_address, destination->bytes, device->ipv6_prefix_length)))
            next_hop = *destination;
        else
            memcpy(next_hop.bytes, device->ipv6_default_router, 16);
    }
    if (source && !ipv6_address_is_unspecified(source)) selected_source = *source;
    uint32_t mtu = device->ipv6_mtu ? device->ipv6_mtu : device->mtu;
    if (device->mtu < IPV6_MIN_MTU || ipv6_address_is_loopback(&selected_source)
        || (!ipv6_address_is_unicast(&selected_source) && !ipv6_address_is_unspecified(&selected_source))
        || packet->length + IPV6_HEADER_LEN > mtu || packet->length + IPV6_HEADER_LEN > UINT16_MAX) {
        if (release) netdev_put(device);
        return -EMSGSIZE;
    }
    uint8_t *header = net_pbuf_push(packet, IPV6_HEADER_LEN);
    if (!header) {
        plogk("ipv6: %s: Header push failed (dest=%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x len=%lu)\n",
              device->name, destination->bytes[0], destination->bytes[1], destination->bytes[2], destination->bytes[3], destination->bytes[4],
              destination->bytes[5], destination->bytes[6], destination->bytes[7], destination->bytes[8], destination->bytes[9],
              destination->bytes[10], destination->bytes[11], destination->bytes[12], destination->bytes[13], destination->bytes[14],
              destination->bytes[15], (unsigned long)packet->length);
        if (release) netdev_put(device);
        return -ENOBUFS;
    }
    memset(header, 0, IPV6_HEADER_LEN);
    header[0] = 0x60;
    net_write_be16(header + 4, (uint16_t)(packet->length - IPV6_HEADER_LEN));
    header[6] = protocol;
    header[7] = hop_limit ? hop_limit : 64;
    memcpy(header + 8, selected_source.bytes, 16);
    memcpy(header + 24, destination->bytes, 16);
    int status;
    if (ipv6_address_is_multicast(destination)) {
        uint8_t mac[6];
        ipv6_multicast_ethernet(destination, mac);
        status = ethernet_output(device, packet, mac, ETH_TYPE_IPV6);
    } else if (ipv6_address_is_unicast(&next_hop)) {
        status = ndp_resolve(device, &next_hop, packet);
    } else {
        static uint64_t last_log;
        if (sched_ticks() - last_log >= 1000) {
            plogk("ipv6: %s: Output dropped, no valid next hop for destination.\n", device->name);
            last_log = sched_ticks();
        }
        status = -ENETUNREACH;
    }
    net_pbuf_pull(packet, IPV6_HEADER_LEN);
    if (release) netdev_put(device);
    return status;
}

/* Register/unregister the input handler for a transport protocol. */
int ipv6_set_transport_handler(uint8_t protocol, ipv6_transport_input_t handler)
{
    if (!protocol || protocol == IPV6_NEXT_ICMP) return -EINVAL;
    spin_lock(&ipv6_lock);
    int free_slot = -1;
    for (unsigned i = 0; i < IPV6_TRANSPORT_SLOTS; i++) {
        if (ipv6_transports[i].handler && ipv6_transports[i].protocol == protocol) {
            if (handler && ipv6_transports[i].handler != handler) {
                spin_unlock(&ipv6_lock);
                return -EBUSY;
            }
            ipv6_transports[i].handler = handler;
            if (!handler) ipv6_transports[i].protocol = 0;
            spin_unlock(&ipv6_lock);
            return 0;
        }
        if (!ipv6_transports[i].handler && free_slot < 0) free_slot = (int)i;
    }
    if (!handler) {
        spin_unlock(&ipv6_lock);
        return -ENOENT;
    }
    if (free_slot < 0) {
        spin_unlock(&ipv6_lock);
        return -ENOSPC;
    }
    ipv6_transports[free_slot].protocol = protocol;
    ipv6_transports[free_slot].handler  = handler;
    spin_unlock(&ipv6_lock);
    return 0;
}

/* True if destination is one of this device's addresses or a local multicast. */
static int ipv6_is_local(const net_device_t *device, const ipv6_address_t *destination)
{
    if (!memcmp(destination->bytes, device->ipv6_link_local, 16) || !memcmp(destination->bytes, device->ipv6_address, 16)) return 1;
    static const uint8_t all_nodes[16] = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (!memcmp(destination->bytes, all_nodes, 16)) return 1;
    ipv6_address_t local, solicited;
    memcpy(local.bytes, device->ipv6_link_local, 16);
    ipv6_solicited_node(&local, &solicited);
    if (ipv6_address_equal(destination, &solicited)) return 1;
    memcpy(local.bytes, device->ipv6_address, 16);
    if (ipv6_address_is_unicast(&local)) {
        ipv6_solicited_node(&local, &solicited);
        if (ipv6_address_equal(destination, &solicited)) return 1;
    }
    return 0;
}

/* Release a reassembly entry's buffers and reset it. */
static void ipv6_reassembly_clear(ipv6_reassembly_t *entry)
{
    free(entry->data);
    free(entry->bitmap);
    memset(entry, 0, sizeof(*entry));
}

/* Find or allocate a reassembly slot for this fragment stream. */
static ipv6_reassembly_t *ipv6_reassembly_find(net_device_t *device, const net_ipv6_packet_t *ip, uint64_t now)
{
    ipv6_reassembly_t *slot = NULL;
    for (unsigned i = 0; i < IPV6_REASSEMBLY_SLOTS; i++) {
        ipv6_reassembly_t *entry = &ipv6_reassembly[i];
        if (entry->device == device && entry->id == ip->fragment_id && entry->protocol == ip->protocol
            && ipv6_address_equal(&entry->source, &ip->source) && ipv6_address_equal(&entry->destination, &ip->destination))
            return entry;
        if (!entry->device || !slot || entry->expires < slot->expires) slot = entry;
    }
    if (slot->device) ipv6_reassembly_clear(slot);
    slot->data   = malloc(IPV6_MAX_PAYLOAD);
    slot->bitmap = malloc(IPV6_REASSEMBLY_BITMAP_SIZE);
    if (!slot->data || !slot->bitmap) {
        plogk("ipv6: %s: Reassembly buffer alloc failed (src=%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x id=%u)\n",
              device->name, ip->source.bytes[0], ip->source.bytes[1], ip->source.bytes[2], ip->source.bytes[3], ip->source.bytes[4],
              ip->source.bytes[5], ip->source.bytes[6], ip->source.bytes[7], ip->source.bytes[8], ip->source.bytes[9], ip->source.bytes[10],
              ip->source.bytes[11], ip->source.bytes[12], ip->source.bytes[13], ip->source.bytes[14], ip->source.bytes[15],
              (unsigned)ip->fragment_id);
        ipv6_reassembly_clear(slot);
        return NULL;
    }
    memset(slot->bitmap, 0, IPV6_REASSEMBLY_BITMAP_SIZE);
    slot->device      = device;
    slot->source      = ip->source;
    slot->destination = ip->destination;
    slot->id          = ip->fragment_id;
    slot->protocol    = ip->protocol;
    slot->hop_limit   = ip->hop_limit;
    slot->expires     = now + IPV6_REASSEMBLY_TIMEOUT_TICKS;
    return slot;
}

/* Insert a fragment and return a reassembled packet once the stream is complete. */
static net_pbuf_t *ipv6_reassemble(net_device_t *device, const net_ipv6_packet_t *ip, uint64_t now, int *status)
{
    net_pbuf_t *complete = NULL;
    *status              = -EINPROGRESS;
    spin_lock(&ipv6_reassembly_lock);
    ipv6_reassembly_t *entry = ipv6_reassembly_find(device, ip, now);
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
    if (!ip->fragment_offset) {
        entry->have_first = 1;
        memset(entry->first_quote, 0, sizeof(entry->first_quote));
        entry->first_quote[0] = 0x60;
        size_t quoted_payload = ip->payload_len < 8U ? ip->payload_len : 8U;
        net_write_be16(entry->first_quote + 4, (uint16_t)(8U + quoted_payload));
        entry->first_quote[6] = IPV6_NEXT_FRAGMENT;
        entry->first_quote[7] = ip->hop_limit;
        memcpy(entry->first_quote + 8, ip->source.bytes, 16);
        memcpy(entry->first_quote + 24, ip->destination.bytes, 16);
        entry->first_quote[40] = ip->protocol;
        net_write_be32(entry->first_quote + 44, ip->fragment_id);
        memcpy(entry->first_quote + 48, ip->payload, quoted_payload);
    }
    if (!ip->more_fragments) {
        entry->have_last    = 1;
        entry->total_length = (uint16_t)end;
    }
    entry->expires = now + IPV6_REASSEMBLY_TIMEOUT_TICKS;
    if (entry->have_first && entry->have_last && entry->received == entry->total_length) {
        complete = net_pbuf_from(entry->data, entry->total_length, NET_PBUF_HEADROOM);
        ipv6_reassembly_clear(entry);
    }
    goto out;
overlap:
    *status = -EBADMSG;
    ipv6_reassembly_clear(entry);
out:
    spin_unlock(&ipv6_reassembly_lock);
    return complete;
}

/* Hand an IPv6 payload to its transport handler, generating ICMPv6 errors if needed. */
static int ipv6_dispatch(net_device_t *device, const ipv6_info_t *info, net_pbuf_t *packet, const void *quoted, size_t quote_length)
{
    if (info->protocol == IPV6_NEXT_ICMP) return icmpv6_input(device, info, packet);
    ipv6_transport_input_t handler = NULL;
    spin_lock(&ipv6_lock);
    for (unsigned i = 0; i < IPV6_TRANSPORT_SLOTS; i++)
        if (ipv6_transports[i].protocol == info->protocol) handler = ipv6_transports[i].handler;
    spin_unlock(&ipv6_lock);
    int status;
    if (handler)
        status = handler(device, info, packet);
    else {
        net_pbuf_free(packet);
        status = -EPROTONOSUPPORT;
    }
    if (status == -ECONNREFUSED)
        icmpv6_error(device, &info->source, ICMPV6_DEST_UNREACHABLE, ICMPV6_PORT_UNREACHABLE, 0, quoted, quote_length);
    else if (status == -EPROTONOSUPPORT)
        icmpv6_error(device, &info->source, ICMPV6_PARAMETER_PROBLEM, ICMPV6_BAD_NEXT_HEADER, 6, quoted, quote_length);
    return status;
}

/* Dispatch a decoded IPv6 packet: reassemble fragments if needed, then
 * hand the payload to the registered transport handler for its protocol */
int ipv6_input(net_device_t *device, net_pbuf_t *packet)
{
    if (!device || !packet) goto bad;
    net_ipv6_packet_t parsed;
    int               parse_status = net_ipv6_parse(packet->data, packet->length, &parsed);
    if (parse_status) goto bad_status;
    if (device->mtu < IPV6_MIN_MTU || ipv6_address_is_loopback(&parsed.source)
        || (!ipv6_address_is_unicast(&parsed.source) && !ipv6_address_is_unspecified(&parsed.source))
        || !ipv6_is_local(device, &parsed.destination)) {
        net_pbuf_free(packet);
        return -EHOSTUNREACH;
    }
    ipv6_info_t info         = {.source          = parsed.source,
                                .destination     = parsed.destination,
                                .flow_label      = parsed.flow_label,
                                .payload_length  = parsed.payload_len,
                                .protocol        = parsed.protocol,
                                .hop_limit       = parsed.hop_limit,
                                .fragment_id     = parsed.fragment_id,
                                .fragment_offset = parsed.fragment_offset,
                                .more_fragments  = parsed.more_fragments};
    size_t      quote_length = parsed.total_len < IPV6_MIN_MTU ? parsed.total_len : IPV6_MIN_MTU;
    uint8_t     quote[IPV6_MIN_MTU];
    memcpy(quote, packet->data, quote_length);
    if (parsed.has_fragment && (parsed.fragment_offset || parsed.more_fragments)) {
        int         reassembly_status;
        net_pbuf_t *complete = ipv6_reassemble(device, &parsed, sched_ticks(), &reassembly_status);
        net_pbuf_free(packet);
        if (!complete) return reassembly_status;
        info.payload_length  = (uint16_t)complete->length;
        info.fragment_offset = 0;
        info.more_fragments  = 0;
        return ipv6_dispatch(device, &info, complete, quote, quote_length);
    }
    net_pbuf_trim(packet, parsed.total_len);
    net_pbuf_pull(packet, parsed.transport_offset);
    return ipv6_dispatch(device, &info, packet, quote, quote_length);
bad_status:
    net_pbuf_free(packet);
    return parse_status;
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}

/* Register the callback invoked when an ICMPv6 error is reported upward. */
int ipv6_set_error_hook(ipv6_error_hook_t hook)
{
    spin_lock(&ipv6_lock);
    if (ipv6_error_hook && hook && ipv6_error_hook != hook) {
        spin_unlock(&ipv6_lock);
        return -EBUSY;
    }
    ipv6_error_hook = hook;
    spin_unlock(&ipv6_lock);
    return 0;
}

/* Translate an ICMPv6 error into the hook's errno and deliver it upward. */
void ipv6_control_error(uint8_t type, uint8_t code, uint32_t mtu, const void *quoted, size_t quoted_length)
{
    if (!quoted || quoted_length < IPV6_HEADER_LEN) return;
    const uint8_t *bytes = quoted;
    if ((bytes[0] >> 4) != 6) return;
    ipv6_address_t source, destination;
    memcpy(source.bytes, bytes + 8, 16);
    memcpy(destination.bytes, bytes + 24, 16);
    uint8_t  protocol   = bytes[6];
    size_t   offset     = IPV6_HEADER_LEN;
    unsigned extensions = 0;
    while (protocol == IPV6_NEXT_HOP_BY_HOP || protocol == IPV6_NEXT_FRAGMENT) {
        if (++extensions > IPV6_MAX_EXTENSION_HEADERS || offset + 8U > quoted_length) return;
        if (protocol == IPV6_NEXT_HOP_BY_HOP) {
            size_t extension_length = ((size_t)bytes[offset + 1] + 1U) * 8U;
            if (extension_length > quoted_length - offset) return;
            protocol = bytes[offset];
            offset += extension_length;
        } else {
            if (net_read_be16(bytes + offset + 2) & 0xfff8U) return;
            protocol = bytes[offset];
            offset += 8U;
        }
    }
    if (protocol == IPV6_NEXT_ROUTING || protocol == IPV6_NEXT_DEST_OPTS || protocol == IPV6_NEXT_AH || protocol == IPV6_NEXT_ESP
        || protocol == IPV6_NEXT_NONE)
        return;
    int error = 0;
    if (type == ICMPV6_PACKET_TOO_BIG)
        error = -EMSGSIZE;
    else if (type == ICMPV6_TIME_EXCEEDED)
        error = -ETIMEDOUT;
    else if (type == ICMPV6_DEST_UNREACHABLE)
        error = code == ICMPV6_PORT_UNREACHABLE ? -ECONNREFUSED : -EHOSTUNREACH;
    else if (type == ICMPV6_PARAMETER_PROBLEM)
        error = -EPROTO;
    if (!error) return;
    spin_lock(&ipv6_lock);
    ipv6_error_hook_t hook = ipv6_error_hook;
    spin_unlock(&ipv6_lock);
    if (hook) hook(protocol, &source, &destination, bytes + offset, quoted_length - offset, error, type == ICMPV6_PACKET_TOO_BIG ? mtu : 0);
}

/* Expire stale reassembly entries, reporting reassembly-timeout ICMPv6 errors. */
void ipv6_timer(uint64_t now_ticks)
{
    for (unsigned i = 0; i < IPV6_REASSEMBLY_SLOTS; i++) {
        net_device_t  *device = NULL;
        ipv6_address_t destination;
        uint8_t        quote[IPV6_HEADER_LEN + 16U];
        size_t         quote_length = 0;
        spin_lock(&ipv6_reassembly_lock);
        if (ipv6_reassembly[i].device && now_ticks >= ipv6_reassembly[i].expires) {
            if (ipv6_reassembly[i].have_first) {
                device       = ipv6_reassembly[i].device;
                destination  = ipv6_reassembly[i].source;
                quote_length = IPV6_HEADER_LEN + 8U + net_read_be16(ipv6_reassembly[i].first_quote + 4) - 8U;
                memcpy(quote, ipv6_reassembly[i].first_quote, quote_length);
            }
            ipv6_reassembly_clear(&ipv6_reassembly[i]);
        }
        spin_unlock(&ipv6_reassembly_lock);
        if (device) icmpv6_error(device, &destination, ICMPV6_TIME_EXCEEDED, ICMPV6_REASSEMBLY_TIMEOUT, 0, quote, quote_length);
    }
}

/* Drop all reassembly state for the removed device. */
void ipv6_device_removed(net_device_t *device)
{
    if (!device) return;
    spin_lock(&ipv6_reassembly_lock);
    for (unsigned i = 0; i < IPV6_REASSEMBLY_SLOTS; i++)
        if (ipv6_reassembly[i].device == device) ipv6_reassembly_clear(&ipv6_reassembly[i]);
    spin_unlock(&ipv6_reassembly_lock);
}
