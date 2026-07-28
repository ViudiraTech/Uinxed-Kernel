#include <kernel/errno.h>
#include <libs/std/string.h>
#include <net/arp.h>
#include <net/endian.h>
#include <net/ethernet.h>
#include <proc/sched.h>

#define ARP_PACKET_LEN    28U
#define ARP_TTL_TICKS     6000U
#define ARP_RETRY_TICKS   100U
#define ARP_MAX_RETRIES   3U

typedef struct arp_entry {
        net_device_t *device;
        uint32_t      ipv4;
        uint8_t       address[6];
        uint8_t       valid;
        uint8_t       retries;
        uint64_t      updated;
        uint64_t      retry_at;
        net_pbuf_t   *pending;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_CAPACITY];
static spinlock_t arp_lock;

int net_arp_parse(const void *data, size_t length, net_arp_packet_t *arp)
{
    if (!data || !arp || length < 8) return -EBADMSG;
    const uint8_t *bytes = data;
    size_t address_length = (size_t)bytes[4] * 2U + (size_t)bytes[5] * 2U;
    if (!bytes[4] || !bytes[5] || address_length > length - 8) return -EBADMSG;
    arp->hardware_type = net_read_be16(bytes);
    arp->protocol_type = net_read_be16(bytes + 2);
    arp->hardware_len = bytes[4];
    arp->protocol_len = bytes[5];
    arp->operation = net_read_be16(bytes + 6);
    return 0;
}

static int arp_send(net_device_t *device, uint16_t operation, const uint8_t target_address[6], uint32_t target_ipv4)
{
    net_pbuf_t *packet = net_pbuf_alloc(ARP_PACKET_LEN, NET_PBUF_HEADROOM);
    if (!packet) return -ENOMEM;
    uint8_t *arp = packet->data;
    net_write_be16(arp, 1);
    net_write_be16(arp + 2, ETH_TYPE_IPV4);
    arp[4] = 6;
    arp[5] = 4;
    net_write_be16(arp + 6, operation);
    memcpy(arp + 8, device->address, 6);
    net_write_be32(arp + 14, device->ipv4_address);
    memcpy(arp + 18, target_address, 6);
    net_write_be32(arp + 24, target_ipv4);
    const uint8_t *ether_target = operation == 1 ? (const uint8_t *)"\xff\xff\xff\xff\xff\xff" : target_address;
    int status = ethernet_output(device, packet, ether_target, ETH_TYPE_ARP);
    net_pbuf_free(packet);
    return status;
}

static arp_entry_t *arp_find_locked(net_device_t *device, uint32_t ipv4)
{
    for (unsigned i = 0; i < ARP_CACHE_CAPACITY; i++)
        if (arp_cache[i].device == device && arp_cache[i].ipv4 == ipv4) return &arp_cache[i];
    return NULL;
}

static arp_entry_t *arp_alloc_locked(net_device_t *device, uint32_t ipv4, uint64_t now)
{
    arp_entry_t *oldest = &arp_cache[0];
    for (unsigned i = 0; i < ARP_CACHE_CAPACITY; i++) {
        if (!arp_cache[i].device) return &arp_cache[i];
        if (arp_cache[i].updated < oldest->updated) oldest = &arp_cache[i];
    }
    if (oldest->pending) net_pbuf_free(oldest->pending);
    memset(oldest, 0, sizeof(*oldest));
    oldest->device = device;
    oldest->ipv4 = ipv4;
    oldest->updated = now;
    return oldest;
}

void arp_learn(net_device_t *device, uint32_t ipv4, const uint8_t address[6], uint64_t now_ticks)
{
    if (!device || !ipv4 || !address) return;
    net_pbuf_t *pending = NULL;
    spin_lock(&arp_lock);
    arp_entry_t *entry = arp_find_locked(device, ipv4);
    if (!entry) entry = arp_alloc_locked(device, ipv4, now_ticks);
    entry->device = device;
    entry->ipv4 = ipv4;
    memcpy(entry->address, address, 6);
    entry->valid = 1;
    entry->updated = now_ticks;
    pending = entry->pending;
    entry->pending = NULL;
    spin_unlock(&arp_lock);
    if (pending) {
        ethernet_output(device, pending, address, ETH_TYPE_IPV4);
        net_pbuf_free(pending);
    }
}

int arp_request(net_device_t *device, uint32_t ipv4)
{
    static const uint8_t empty[6];
    if (!device || !ipv4 || !device->ipv4_address) return -EINVAL;
    return arp_send(device, 1, empty, ipv4);
}

int arp_resolve(net_device_t *device, uint32_t ipv4, net_pbuf_t *packet)
{
    if (!device || !packet || !ipv4) return -EINVAL;
    if (ipv4 == UINT32_MAX || (device->ipv4_netmask && ipv4 == (device->ipv4_address | ~device->ipv4_netmask)))
        return ethernet_output(device, packet, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", ETH_TYPE_IPV4);
    uint8_t address[6];
    int found = 0;
    uint64_t now = sched_ticks();
    spin_lock(&arp_lock);
    arp_entry_t *entry = arp_find_locked(device, ipv4);
    if (entry && entry->valid && now - entry->updated < ARP_TTL_TICKS) {
        memcpy(address, entry->address, 6);
        found = 1;
    } else {
        if (!entry) entry = arp_alloc_locked(device, ipv4, now);
        if (entry->pending) {
            spin_unlock(&arp_lock);
            return -ENOBUFS;
        }
        entry->device = device;
        entry->ipv4 = ipv4;
        entry->pending = net_pbuf_clone(packet, NET_PBUF_HEADROOM);
        entry->valid = 0;
        entry->retries = 1;
        entry->retry_at = now + ARP_RETRY_TICKS;
        entry->updated = now;
    }
    spin_unlock(&arp_lock);
    if (found) return ethernet_output(device, packet, address, ETH_TYPE_IPV4);
    int status = arp_request(device, ipv4);
    return status ? status : -EINPROGRESS;
}

int arp_input(net_device_t *device, net_pbuf_t *packet)
{
    if (!device || !packet || packet->length < ARP_PACKET_LEN) {
        net_pbuf_free(packet);
        return -EBADMSG;
    }
    const uint8_t *arp = packet->data;
    if (net_read_be16(arp) != 1 || net_read_be16(arp + 2) != ETH_TYPE_IPV4 || arp[4] != 6 || arp[5] != 4) {
        net_pbuf_free(packet);
        return -EBADMSG;
    }
    uint16_t operation = net_read_be16(arp + 6);
    uint32_t sender = net_read_be32(arp + 14);
    uint32_t target = net_read_be32(arp + 24);
    uint8_t sender_address[6];
    memcpy(sender_address, arp + 8, 6);
    if (sender) arp_learn(device, sender, sender_address, sched_ticks());
    int status = 0;
    if (operation == 1 && target == device->ipv4_address) status = arp_send(device, 2, sender_address, sender);
    else if (operation != 1 && operation != 2) status = -EBADMSG;
    net_pbuf_free(packet);
    return status;
}

void arp_timer(uint64_t now_ticks)
{
    for (unsigned i = 0; i < ARP_CACHE_CAPACITY; i++) {
        net_device_t *device = NULL;
        uint32_t ipv4 = 0;
        spin_lock(&arp_lock);
        arp_entry_t *entry = &arp_cache[i];
        if (entry->device && entry->valid && now_ticks - entry->updated >= ARP_TTL_TICKS) {
            if (entry->pending) net_pbuf_free(entry->pending);
            memset(entry, 0, sizeof(*entry));
        } else if (entry->device && !entry->valid && entry->pending && now_ticks >= entry->retry_at) {
            if (entry->retries >= ARP_MAX_RETRIES) {
                net_pbuf_free(entry->pending);
                memset(entry, 0, sizeof(*entry));
            } else {
                entry->retries++;
                entry->retry_at = now_ticks + ARP_RETRY_TICKS;
                device = entry->device;
                ipv4 = entry->ipv4;
            }
        }
        spin_unlock(&arp_lock);
        if (device) arp_request(device, ipv4);
    }
}

void arp_device_removed(net_device_t *device)
{
    spin_lock(&arp_lock);
    for (unsigned i = 0; i < ARP_CACHE_CAPACITY; i++) {
        if (arp_cache[i].device == device) {
            if (arp_cache[i].pending) net_pbuf_free(arp_cache[i].pending);
            memset(&arp_cache[i], 0, sizeof(arp_cache[i]));
        }
    }
    spin_unlock(&arp_lock);
}
