/*
 *
 *      arp.c
 *      ARP protocol implementation
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <net/ipv4/arp.h>
#include <net/core/endian.h>
#include <net/core/ethernet.h>
#include <net/ipv4/ipv4.h>
#include <proc/sched.h>

#define ARP_PACKET_LEN  28U
#define ARP_TTL_TICKS   (60U * TIMER_HZ)
#define ARP_RETRY_TICKS TIMER_HZ
#define ARP_MAX_RETRIES 3U

typedef enum arp_state {
    ARP_EMPTY,
    ARP_INCOMPLETE,
    ARP_REACHABLE,
} arp_state_t;

typedef struct arp_pending {
        struct arp_pending *next;
        net_pbuf_t         *packet;
} arp_pending_t;

typedef struct arp_entry {
        net_device_t  *device;
        uint32_t       ipv4;
        uint8_t        address[ETH_ADDRESS_LEN];
        uint8_t        state;
        uint8_t        retries;
        uint8_t        pending_count;
        uint64_t       updated;
        uint64_t       retry_at;
        arp_pending_t *head;
        arp_pending_t *tail;
} arp_entry_t;

static arp_entry_t    arp_cache[ARP_CACHE_CAPACITY];
static arp_pending_t  arp_pending_pool[ARP_PENDING_TOTAL];
static arp_pending_t *arp_pending_free;
static uint8_t        arp_pending_initialized;
static spinlock_t     arp_lock;

int net_arp_parse(const void *data, size_t length, net_arp_packet_t *arp)
{
    if (!data || !arp || length < 8) return -EBADMSG;
    const uint8_t *bytes          = data;
    size_t         address_length = ((size_t)bytes[4] + bytes[5]) * 2U;
    if (!bytes[4] || !bytes[5] || address_length > length - 8) return -EBADMSG;
    arp->hardware_type = net_read_be16(bytes);
    arp->protocol_type = net_read_be16(bytes + 2);
    arp->hardware_len  = bytes[4];
    arp->protocol_len  = bytes[5];
    arp->operation     = net_read_be16(bytes + 6);
    return 0;
}

static int arp_ipv4_unicast(uint32_t address)
{
    return address && address != UINT32_MAX && (address >> 24) != 127U && (address >> 28) != 14U;
}

static int arp_mac_unicast(const uint8_t address[ETH_ADDRESS_LEN])
{
    static const uint8_t zero[ETH_ADDRESS_LEN];
    return !(address[0] & 1U) && memcmp(address, zero, sizeof(zero)) != 0;
}

static int arp_send(net_device_t *device, uint16_t operation, const uint8_t target_address[ETH_ADDRESS_LEN], uint32_t target_ipv4)
{
    net_pbuf_t *packet = net_pbuf_alloc(ARP_PACKET_LEN, NET_PBUF_HEADROOM);
    if (!packet) {
        plogk("arp: %s: request alloc failed (target=%u.%u.%u.%u).\n", device->name, (unsigned)(target_ipv4 >> 24) & 0xff,
              (unsigned)(target_ipv4 >> 16) & 0xff, (unsigned)(target_ipv4 >> 8) & 0xff, (unsigned)target_ipv4 & 0xff);
        return -ENOMEM;
    }
    uint8_t *arp = packet->data;
    net_write_be16(arp, 1);
    net_write_be16(arp + 2, ETH_TYPE_IPV4);
    arp[4] = ETH_ADDRESS_LEN;
    arp[5] = 4;
    net_write_be16(arp + 6, operation);
    memcpy(arp + 8, device->address, ETH_ADDRESS_LEN);
    net_write_be32(arp + 14, device->ipv4_address);
    memcpy(arp + 18, target_address, ETH_ADDRESS_LEN);
    net_write_be32(arp + 24, target_ipv4);
    int status = ethernet_output(device, packet, operation == 1 ? ethernet_broadcast_address : target_address, ETH_TYPE_ARP);
    net_pbuf_free(packet);
    return status;
}

void arp_init(void)
{
    for (unsigned i = 0; i < ARP_PENDING_TOTAL; i++) {
        arp_pending_pool[i].next = arp_pending_free;
        arp_pending_free         = &arp_pending_pool[i];
    }
    arp_pending_initialized = 1;
}

static void arp_pool_init_locked(void)
{
    if (!arp_pending_initialized) {
        for (unsigned i = 0; i < ARP_PENDING_TOTAL; i++) {
            arp_pending_pool[i].next = arp_pending_free;
            arp_pending_free         = &arp_pending_pool[i];
        }
        arp_pending_initialized = 1;
    }
}

static arp_entry_t *arp_find_locked(net_device_t *device, uint32_t ipv4)
{
    for (unsigned i = 0; i < ARP_CACHE_CAPACITY; i++)
        if (arp_cache[i].state != ARP_EMPTY && arp_cache[i].device == device && arp_cache[i].ipv4 == ipv4) return &arp_cache[i];
    return NULL;
}

static void arp_drop_pending_locked(arp_entry_t *entry)
{
    while (entry->head) {
        arp_pending_t *pending = entry->head;
        entry->head            = pending->next;
        net_pbuf_free(pending->packet);
        pending->packet  = NULL;
        pending->next    = arp_pending_free;
        arp_pending_free = pending;
    }
    entry->tail          = NULL;
    entry->pending_count = 0;
}

static arp_entry_t *arp_alloc_locked(net_device_t *device, uint32_t ipv4, uint64_t now)
{
    arp_entry_t *oldest = NULL;
    for (unsigned i = 0; i < ARP_CACHE_CAPACITY; i++) {
        if (arp_cache[i].state == ARP_EMPTY) {
            oldest = &arp_cache[i];
            break;
        }
        if (!oldest || arp_cache[i].updated < oldest->updated) oldest = &arp_cache[i];
    }
    arp_drop_pending_locked(oldest);
    memset(oldest, 0, sizeof(*oldest));
    oldest->device  = device;
    oldest->ipv4    = ipv4;
    oldest->updated = now;
    oldest->state   = ARP_INCOMPLETE;
    return oldest;
}

void arp_learn(net_device_t *device, uint32_t ipv4, const uint8_t address[ETH_ADDRESS_LEN], uint64_t now_ticks)
{
    if (!device || !arp_ipv4_unicast(ipv4) || !address || !arp_mac_unicast(address)) return;
    arp_pending_t *pending;
    spin_lock(&arp_lock);
    arp_pool_init_locked();
    arp_entry_t *entry = arp_find_locked(device, ipv4);
    if (!entry) entry = arp_alloc_locked(device, ipv4, now_ticks);
    memcpy(entry->address, address, ETH_ADDRESS_LEN);
    entry->state   = ARP_REACHABLE;
    entry->updated = now_ticks;
    entry->retries = 0;
    pending        = entry->head;
    entry->head = entry->tail = NULL;
    entry->pending_count      = 0;
    spin_unlock(&arp_lock);
    while (pending) {
        arp_pending_t *next = pending->next;
        ethernet_output(device, pending->packet, address, ETH_TYPE_IPV4);
        net_pbuf_free(pending->packet);
        spin_lock(&arp_lock);
        pending->packet  = NULL;
        pending->next    = arp_pending_free;
        arp_pending_free = pending;
        spin_unlock(&arp_lock);
        pending = next;
    }
}

int arp_request(net_device_t *device, uint32_t ipv4)
{
    static const uint8_t empty[ETH_ADDRESS_LEN];
    if (!device || !arp_ipv4_unicast(ipv4) || !device->ipv4_address) return -EINVAL;
    return arp_send(device, 1, empty, ipv4);
}

int arp_resolve(net_device_t *device, uint32_t ipv4, net_pbuf_t *packet)
{
    if (!device || !packet || !ipv4) return -EINVAL;
    uint32_t broadcast = device->ipv4_netmask ? device->ipv4_address | ~device->ipv4_netmask : UINT32_MAX;
    if (ipv4 == UINT32_MAX || ipv4 == broadcast) return ethernet_output(device, packet, ethernet_broadcast_address, ETH_TYPE_IPV4);
    if (!arp_ipv4_unicast(ipv4)) return -EHOSTUNREACH;

    uint8_t  address[ETH_ADDRESS_LEN];
    uint64_t now     = sched_ticks();
    int      request = 0;
    spin_lock(&arp_lock);
    arp_pool_init_locked();
    arp_entry_t *entry = arp_find_locked(device, ipv4);
    if (entry && entry->state == ARP_REACHABLE && now - entry->updated < ARP_TTL_TICKS) {
        memcpy(address, entry->address, sizeof(address));
        spin_unlock(&arp_lock);
        return ethernet_output(device, packet, address, ETH_TYPE_IPV4);
    }
    if (!entry) entry = arp_alloc_locked(device, ipv4, now);
    if (entry->pending_count >= ARP_PENDING_PER_ENTRY || !arp_pending_free) {
        spin_unlock(&arp_lock);
        plogk("arp: %s: pending pool exhausted for %u.%u.%u.%u.\n", device->name, (unsigned)(ipv4 >> 24) & 0xff, (unsigned)(ipv4 >> 16) & 0xff,
              (unsigned)(ipv4 >> 8) & 0xff, (unsigned)ipv4 & 0xff);
        return -ENOBUFS;
    }
    arp_pending_t *pending = arp_pending_free;
    arp_pending_free       = pending->next;
    pending->packet        = net_pbuf_clone(packet, NET_PBUF_HEADROOM);
    if (!pending->packet) {
        pending->next    = arp_pending_free;
        arp_pending_free = pending;
        spin_unlock(&arp_lock);
        plogk("arp: %s: pending packet clone failed (%u.%u.%u.%u).\n", device->name, (unsigned)(ipv4 >> 24) & 0xff,
              (unsigned)(ipv4 >> 16) & 0xff, (unsigned)(ipv4 >> 8) & 0xff, (unsigned)ipv4 & 0xff);
        return -ENOMEM;
    }
    pending->next = NULL;
    if (entry->tail)
        entry->tail->next = pending;
    else
        entry->head = pending;
    entry->tail = pending;
    entry->pending_count++;
    if (entry->state != ARP_INCOMPLETE || !entry->retries) {
        entry->state    = ARP_INCOMPLETE;
        entry->retries  = 1;
        entry->retry_at = now + ARP_RETRY_TICKS;
        entry->updated  = now;
        request         = 1;
    }
    spin_unlock(&arp_lock);
    if (request) arp_request(device, ipv4);
    return -EINPROGRESS;
}

int arp_input(net_device_t *device, net_pbuf_t *packet)
{
    if (!device || !packet || packet->length < ARP_PACKET_LEN) goto bad;
    const uint8_t *arp = packet->data;
    if (net_read_be16(arp) != 1 || net_read_be16(arp + 2) != ETH_TYPE_IPV4 || arp[4] != ETH_ADDRESS_LEN || arp[5] != 4) goto bad;
    uint16_t operation = net_read_be16(arp + 6);
    uint32_t sender    = net_read_be32(arp + 14);
    uint32_t target    = net_read_be32(arp + 24);
    if ((operation != 1 && operation != 2) || !arp_mac_unicast(arp + 8) || !arp_ipv4_unicast(sender)) goto bad;
    if (target == device->ipv4_address || target == sender) arp_learn(device, sender, arp + 8, sched_ticks());
    int status = operation == 1 && target == device->ipv4_address ? arp_send(device, 2, arp + 8, sender) : 0;
    net_pbuf_free(packet);
    return status;
bad:
    if (packet) net_pbuf_free(packet);
    return -EBADMSG;
}

void arp_timer(uint64_t now_ticks)
{
    ipv4_timer(now_ticks);
    for (unsigned i = 0; i < ARP_CACHE_CAPACITY; i++) {
        net_device_t *device = NULL;
        uint32_t      ipv4   = 0;
        spin_lock(&arp_lock);
        arp_pool_init_locked();
        arp_entry_t *entry = &arp_cache[i];
        if (entry->state == ARP_REACHABLE && now_ticks - entry->updated >= ARP_TTL_TICKS) {
            memset(entry, 0, sizeof(*entry));
        } else if (entry->state == ARP_INCOMPLETE && now_ticks >= entry->retry_at) {
            if (entry->retries >= ARP_MAX_RETRIES) {
                plogk("arp: %s: neighbor resolution timed out for %u.%u.%u.%u, dropping pending packets.\n",
                      entry->device ? entry->device->name : "?", (unsigned)(entry->ipv4 >> 24) & 0xff, (unsigned)(entry->ipv4 >> 16) & 0xff,
                      (unsigned)(entry->ipv4 >> 8) & 0xff, (unsigned)entry->ipv4 & 0xff);
                arp_drop_pending_locked(entry);
                memset(entry, 0, sizeof(*entry));
            } else {
                entry->retries++;
                entry->retry_at = now_ticks + ARP_RETRY_TICKS;
                device          = entry->device;
                ipv4            = entry->ipv4;
            }
        }
        spin_unlock(&arp_lock);
        if (device) arp_request(device, ipv4);
    }
}

void arp_device_removed(net_device_t *device)
{
    ipv4_device_removed(device);
    spin_lock(&arp_lock);
    arp_pool_init_locked();
    for (unsigned i = 0; i < ARP_CACHE_CAPACITY; i++) {
        if (arp_cache[i].device != device) continue;
        arp_drop_pending_locked(&arp_cache[i]);
        memset(&arp_cache[i], 0, sizeof(arp_cache[i]));
    }
    spin_unlock(&arp_lock);
}
