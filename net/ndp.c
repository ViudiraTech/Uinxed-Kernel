/*
 *
 *      ndp.c
 *      NDP implementation
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <net/endian.h>
#include <net/ethernet.h>
#include <net/icmpv6.h>
#include <net/ndp.h>
#include <proc/sched.h>

#define NDP_TICKS_PER_SECOND 100U
#define NDP_REACHABLE_TICKS  3000U
#define NDP_RETRY_TICKS      100U
#define NDP_MAX_RETRIES      3U

#define NDP_OPT_SOURCE_LL 1U
#define NDP_OPT_TARGET_LL 2U
#define NDP_OPT_PREFIX    3U
#define NDP_OPT_MTU       5U

typedef enum ndp_state {
    NDP_EMPTY,
    NDP_INCOMPLETE,
    NDP_REACHABLE,
} ndp_state_t;

typedef struct ndp_pending {
        struct ndp_pending *next;
        net_pbuf_t         *packet;
} ndp_pending_t;

typedef struct ndp_entry {
        net_device_t  *device;
        ipv6_address_t address;
        uint8_t        mac[6];
        uint8_t        state;
        uint8_t        retries;
        uint8_t        pending_count;
        uint64_t       updated;
        uint64_t       retry_at;
        ndp_pending_t *head;
        ndp_pending_t *tail;
} ndp_entry_t;

static ndp_entry_t    ndp_cache[NDP_CACHE_CAPACITY];
static ndp_pending_t  ndp_pending_pool[NDP_PENDING_TOTAL];
static ndp_pending_t *ndp_pending_free;
static uint8_t        ndp_pool_initialized;
static spinlock_t     ndp_lock;

static int ndp_mac_unicast(const uint8_t mac[6])
{
    static const uint8_t zero[6];
    return mac && !(mac[0] & 1U) && memcmp(mac, zero, sizeof(zero)) != 0;
}

static uint64_t ndp_lifetime(uint64_t now, uint32_t seconds)
{
    if (seconds == UINT32_MAX) return UINT64_MAX;
    uint64_t ticks = (uint64_t)seconds * NDP_TICKS_PER_SECOND;
    return ticks > UINT64_MAX - now ? UINT64_MAX : now + ticks;
}

void ndp_init(void)
{
    for (unsigned i = 0; i < NDP_PENDING_TOTAL; i++) {
        ndp_pending_pool[i].next = ndp_pending_free;
        ndp_pending_free         = &ndp_pending_pool[i];
    }
    ndp_pool_initialized = 1;
}

static void ndp_pool_init_locked(void)
{
    if (!ndp_pool_initialized) {
        for (unsigned i = 0; i < NDP_PENDING_TOTAL; i++) {
            ndp_pending_pool[i].next = ndp_pending_free;
            ndp_pending_free         = &ndp_pending_pool[i];
        }
        ndp_pool_initialized = 1;
    }
}

static ndp_entry_t *ndp_find_locked(net_device_t *device, const ipv6_address_t *address)
{
    for (unsigned i = 0; i < NDP_CACHE_CAPACITY; i++)
        if (ndp_cache[i].state != NDP_EMPTY && ndp_cache[i].device == device && ipv6_address_equal(&ndp_cache[i].address, address))
            return &ndp_cache[i];
    return NULL;
}

static void ndp_drop_pending_locked(ndp_entry_t *entry)
{
    while (entry->head) {
        ndp_pending_t *pending = entry->head;
        entry->head            = pending->next;
        net_pbuf_free(pending->packet);
        pending->packet  = NULL;
        pending->next    = ndp_pending_free;
        ndp_pending_free = pending;
    }
    entry->tail          = NULL;
    entry->pending_count = 0;
}

static ndp_entry_t *ndp_alloc_locked(net_device_t *device, const ipv6_address_t *address, uint64_t now)
{
    ndp_entry_t *slot = NULL;
    for (unsigned i = 0; i < NDP_CACHE_CAPACITY; i++) {
        if (ndp_cache[i].state == NDP_EMPTY) {
            slot = &ndp_cache[i];
            break;
        }
        if (!slot || ndp_cache[i].updated < slot->updated) slot = &ndp_cache[i];
    }
    ndp_drop_pending_locked(slot);
    memset(slot, 0, sizeof(*slot));
    slot->device  = device;
    slot->address = *address;
    slot->state   = NDP_INCOMPLETE;
    slot->updated = now;
    return slot;
}

static int ndp_send(net_device_t *device, const ipv6_address_t *source, const ipv6_address_t *destination, uint8_t type, uint32_t flags,
                    const ipv6_address_t *target, uint8_t option_type)
{
    size_t      length = target ? 32U : 16U;
    net_pbuf_t *packet = net_pbuf_alloc(length, NET_PBUF_HEADROOM);
    if (!packet) {
        plogk("ndp: %s: message alloc failed (type=%u).\n", device->name, (unsigned)type);
        return -ENOMEM;
    }
    memset(packet->data, 0, length);
    packet->data[0] = type;
    if (target) {
        net_write_be32(packet->data + 4, flags);
        memcpy(packet->data + 8, target->bytes, IPV6_ADDRESS_LEN);
        packet->data[24] = option_type;
        packet->data[25] = 1;
        memcpy(packet->data + 26, device->address, 6);
    } else {
        packet->data[8] = option_type;
        packet->data[9] = 1;
        memcpy(packet->data + 10, device->address, 6);
    }
    uint16_t checksum = net_checksum_ipv6_pseudo(source, destination, IPV6_NEXT_ICMP, packet->data, packet->length);
    net_write_be16(packet->data + 2, checksum ? checksum : UINT16_MAX);
    int status = ipv6_output(device, source, destination, IPV6_NEXT_ICMP, 255, packet);
    net_pbuf_free(packet);
    return status;
}

static int ndp_neighbor_solicit(net_device_t *device, const ipv6_address_t *target)
{
    ipv6_address_t source, destination;
    memcpy(source.bytes, device->ipv6_link_local, 16);
    ipv6_solicited_node(target, &destination);
    return ndp_send(device, &source, &destination, ICMPV6_NEIGHBOR_SOLICIT, 0, target, NDP_OPT_SOURCE_LL);
}

void ndp_learn(net_device_t *device, const ipv6_address_t *address, const uint8_t mac[6], uint64_t now_ticks)
{
    if (!device || !ipv6_address_is_unicast(address) || ipv6_address_is_loopback(address) || !ndp_mac_unicast(mac)) return;
    spin_lock(&ndp_lock);
    ndp_pool_init_locked();
    ndp_entry_t *entry = ndp_find_locked(device, address);
    if (!entry) entry = ndp_alloc_locked(device, address, now_ticks);
    memcpy(entry->mac, mac, 6);
    entry->state           = NDP_REACHABLE;
    entry->updated         = now_ticks;
    entry->retries         = 0;
    ndp_pending_t *pending = entry->head;
    entry->head = entry->tail = NULL;
    entry->pending_count      = 0;
    spin_unlock(&ndp_lock);
    while (pending) {
        ndp_pending_t *next = pending->next;
        ethernet_output(device, pending->packet, mac, ETH_TYPE_IPV6);
        net_pbuf_free(pending->packet);
        spin_lock(&ndp_lock);
        pending->packet  = NULL;
        pending->next    = ndp_pending_free;
        ndp_pending_free = pending;
        spin_unlock(&ndp_lock);
        pending = next;
    }
}

int ndp_resolve(net_device_t *device, const ipv6_address_t *address, net_pbuf_t *packet)
{
    if (!device || !address || !packet || !ipv6_address_is_unicast(address) || ipv6_address_is_loopback(address)) return -EINVAL;
    uint64_t now = sched_ticks();
    uint8_t  mac[6];
    int      request = 0;
    spin_lock(&ndp_lock);
    ndp_pool_init_locked();
    ndp_entry_t *entry = ndp_find_locked(device, address);
    if (entry && entry->state == NDP_REACHABLE && now - entry->updated < NDP_REACHABLE_TICKS) {
        memcpy(mac, entry->mac, 6);
        spin_unlock(&ndp_lock);
        return ethernet_output(device, packet, mac, ETH_TYPE_IPV6);
    }
    if (!entry) entry = ndp_alloc_locked(device, address, now);
    if (entry->pending_count >= NDP_PENDING_PER_ENTRY || !ndp_pending_free) {
        spin_unlock(&ndp_lock);
        plogk("ndp: %s: pending pool exhausted for neighbor.\n", device->name);
        return -ENOBUFS;
    }
    ndp_pending_t *pending = ndp_pending_free;
    ndp_pending_free       = pending->next;
    pending->packet        = net_pbuf_clone(packet, NET_PBUF_HEADROOM);
    if (!pending->packet) {
        pending->next    = ndp_pending_free;
        ndp_pending_free = pending;
        spin_unlock(&ndp_lock);
        plogk("ndp: %s: pending packet clone failed.\n", device->name);
        return -ENOMEM;
    }
    pending->next = NULL;
    if (entry->tail)
        entry->tail->next = pending;
    else
        entry->head = pending;
    entry->tail = pending;
    entry->pending_count++;
    if (entry->state != NDP_INCOMPLETE || !entry->retries) {
        entry->state    = NDP_INCOMPLETE;
        entry->retries  = 1;
        entry->retry_at = now + NDP_RETRY_TICKS;
        entry->updated  = now;
        request         = 1;
    }
    spin_unlock(&ndp_lock);
    if (request) ndp_neighbor_solicit(device, address);
    return -EINPROGRESS;
}

int ndp_router_solicit(net_device_t *device)
{
    if (!device) return -EINVAL;
    static const ipv6_address_t all_routers = {
        .bytes = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2}
    };
    ipv6_address_t source;
    memcpy(source.bytes, device->ipv6_link_local, 16);
    if (!ipv6_address_is_link_local(&source)) return -EADDRNOTAVAIL;
    return ndp_send(device, &source, &all_routers, ICMPV6_ROUTER_SOLICIT, 0, NULL, NDP_OPT_SOURCE_LL);
}

void ndp_device_up(net_device_t *device)
{
    if (!device) return;
    ipv6_address_t link_local;
    ipv6_link_local_from_mac(&link_local, device->address);
    memcpy(device->ipv6_link_local, link_local.bytes, 16);
    device->ipv6_mtu = device->mtu >= IPV6_MIN_MTU ? device->mtu : 0;
    ndp_router_solicit(device);
}

static int ndp_parse_options(const uint8_t *options, size_t length, const uint8_t **source_ll, const uint8_t **target_ll)
{
    while (length) {
        if (length < 2 || !options[1]) return -EBADMSG;
        size_t option_length = (size_t)options[1] * 8U;
        if (option_length > length) return -EBADMSG;
        if (options[0] == NDP_OPT_SOURCE_LL) {
            if (option_length != 8 || *source_ll) return -EBADMSG;
            *source_ll = options + 2;
        } else if (options[0] == NDP_OPT_TARGET_LL) {
            if (option_length != 8 || *target_ll) return -EBADMSG;
            *target_ll = options + 2;
        }
        options += option_length;
        length -= option_length;
    }
    return 0;
}

static int ndp_target_is_local(net_device_t *device, const ipv6_address_t *target)
{
    return !memcmp(target->bytes, device->ipv6_link_local, 16) || !memcmp(target->bytes, device->ipv6_address, 16);
}

static int ndp_neighbor_input(net_device_t *device, const ipv6_info_t *ip, net_pbuf_t *packet)
{
    uint8_t type = packet->data[0];
    if (packet->length < 24 || packet->data[1]) return -EBADMSG;
    ipv6_address_t target;
    memcpy(target.bytes, packet->data + 8, 16);
    if (!ipv6_address_is_unicast(&target)) return -EBADMSG;
    const uint8_t *source_ll = NULL, *target_ll = NULL;
    if (ndp_parse_options(packet->data + 24, packet->length - 24, &source_ll, &target_ll)) return -EBADMSG;
    if (type == ICMPV6_NEIGHBOR_SOLICIT) {
        if (!ndp_target_is_local(device, &target)) return 0;
        ipv6_address_t solicited;
        ipv6_solicited_node(&target, &solicited);
        if (!ipv6_address_equal(&ip->destination, &target) && !ipv6_address_equal(&ip->destination, &solicited)) return -EBADMSG;
        if (ipv6_address_is_unspecified(&ip->source)) {
            if (source_ll || !ipv6_address_equal(&ip->destination, &solicited)) return -EBADMSG;
            static const ipv6_address_t all_nodes = {
                .bytes = {0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1}
            };
            return ndp_send(device, &target, &all_nodes, ICMPV6_NEIGHBOR_ADVERT, 0x20000000U, &target, NDP_OPT_TARGET_LL);
        }
        if (source_ll && !ndp_mac_unicast(source_ll)) return -EBADMSG;
        if (source_ll) ndp_learn(device, &ip->source, source_ll, sched_ticks());
        return ndp_send(device, &target, &ip->source, ICMPV6_NEIGHBOR_ADVERT, 0x60000000U, &target, NDP_OPT_TARGET_LL);
    }
    uint32_t flags = net_read_be32(packet->data + 4);
    if (ipv6_address_is_unspecified(&ip->source) || ((flags & 0x40000000U) && ipv6_address_is_multicast(&ip->destination)) || !target_ll
        || !ndp_mac_unicast(target_ll))
        return -EBADMSG;
    ndp_learn(device, &target, target_ll, sched_ticks());
    return 0;
}

static int ndp_router_advert(net_device_t *device, const ipv6_info_t *ip, net_pbuf_t *packet)
{
    if (packet->length < 16 || packet->data[1] || !ipv6_address_is_link_local(&ip->source)) return -EBADMSG;
    uint64_t       now             = sched_ticks();
    uint16_t       router_lifetime = net_read_be16(packet->data + 6);
    const uint8_t *options         = packet->data + 16;
    size_t         length          = packet->length - 16;
    const uint8_t *source_ll       = NULL;
    ipv6_address_t address;
    int            have_address = 0;
    uint64_t       valid_until = 0, preferred_until = 0;
    uint32_t       advertised_mtu = 0;
    while (length) {
        if (length < 2 || !options[1]) return -EBADMSG;
        size_t option_length = (size_t)options[1] * 8U;
        if (option_length > length) return -EBADMSG;
        if (options[0] == NDP_OPT_SOURCE_LL) {
            if (option_length != 8 || source_ll) return -EBADMSG;
            source_ll = options + 2;
        } else if (options[0] == NDP_OPT_PREFIX) {
            if (option_length != 32) return -EBADMSG;
            uint32_t       valid     = net_read_be32(options + 4);
            uint32_t       preferred = net_read_be32(options + 8);
            ipv6_address_t prefix;
            memcpy(prefix.bytes, options + 16, 16);
            if (preferred > valid) return -EBADMSG;
            if (options[2] == 64 && (options[3] & 0x40U) && valid && !ipv6_address_is_multicast(&prefix)
                && !ipv6_address_is_link_local(&prefix)) {
                memcpy(address.bytes, prefix.bytes, 8);
                memcpy(address.bytes + 8, device->ipv6_link_local + 8, 8);
                valid_until     = ndp_lifetime(now, valid);
                preferred_until = ndp_lifetime(now, preferred);
                have_address    = 1;
            }
        } else if (options[0] == NDP_OPT_MTU) {
            if (option_length != 8 || net_read_be16(options + 2)) return -EBADMSG;
            uint32_t mtu = net_read_be32(options + 4);
            if (mtu >= IPV6_MIN_MTU && mtu <= device->mtu) advertised_mtu = mtu;
        }
        options += option_length;
        length -= option_length;
    }
    if (source_ll) ndp_learn(device, &ip->source, source_ll, now);
    spin_lock(&device->lock);
    if (router_lifetime) {
        memcpy(device->ipv6_default_router, ip->source.bytes, 16);
        device->ipv6_router_until = ndp_lifetime(now, router_lifetime);
    } else if (!memcmp(device->ipv6_default_router, ip->source.bytes, 16)) {
        memset(device->ipv6_default_router, 0, 16);
        device->ipv6_router_until = 0;
    }
    if (have_address) {
        memcpy(device->ipv6_address, address.bytes, 16);
        device->ipv6_prefix_length   = 64;
        device->ipv6_valid_until     = valid_until;
        device->ipv6_preferred_until = preferred_until;
    }
    if (advertised_mtu) device->ipv6_mtu = advertised_mtu;
    spin_unlock(&device->lock);
    return 0;
}

int ndp_input(net_device_t *device, const ipv6_info_t *ip, net_pbuf_t *packet)
{
    if (!device || !ip || !packet || ip->hop_limit != 255 || packet->length < 8) goto bad;
    int status;
    if (packet->data[0] == ICMPV6_NEIGHBOR_SOLICIT || packet->data[0] == ICMPV6_NEIGHBOR_ADVERT)
        status = ndp_neighbor_input(device, ip, packet);
    else if (packet->data[0] == ICMPV6_ROUTER_ADVERT)
        status = ndp_router_advert(device, ip, packet);
    else if (packet->data[0] == ICMPV6_ROUTER_SOLICIT)
        status = packet->length >= 8 && !packet->data[1] ? 0 : -EBADMSG;
    else
        status = -EBADMSG;
    net_pbuf_free(packet);
    return status;
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}

void ndp_timer(uint64_t now_ticks)
{
    ipv6_timer(now_ticks);
    for (unsigned i = 0; i < NDP_CACHE_CAPACITY; i++) {
        net_device_t  *device = NULL;
        ipv6_address_t address;
        spin_lock(&ndp_lock);
        ndp_pool_init_locked();
        ndp_entry_t *entry = &ndp_cache[i];
        if (entry->state == NDP_REACHABLE && now_ticks - entry->updated >= NDP_REACHABLE_TICKS) {
            memset(entry, 0, sizeof(*entry));
        } else if (entry->state == NDP_INCOMPLETE && now_ticks >= entry->retry_at) {
            if (entry->retries >= NDP_MAX_RETRIES) {
                plogk("ndp: %s: neighbor resolution timed out, dropping pending packets.\n", entry->device ? entry->device->name : "?");
                ndp_drop_pending_locked(entry);
                memset(entry, 0, sizeof(*entry));
            } else {
                entry->retries++;
                entry->retry_at = now_ticks + NDP_RETRY_TICKS;
                device          = entry->device;
                address         = entry->address;
            }
        }
        spin_unlock(&ndp_lock);
        if (device) ndp_neighbor_solicit(device, &address);
    }
}

void ndp_device_removed(net_device_t *device)
{
    ipv6_device_removed(device);
    spin_lock(&ndp_lock);
    ndp_pool_init_locked();
    for (unsigned i = 0; i < NDP_CACHE_CAPACITY; i++) {
        if (ndp_cache[i].device != device) continue;
        ndp_drop_pending_locked(&ndp_cache[i]);
        memset(&ndp_cache[i], 0, sizeof(ndp_cache[i]));
    }
    spin_unlock(&ndp_lock);
}
