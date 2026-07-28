#include <kernel/errno.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <net/arp.h>
#include <net/dhcp.h>
#include <net/endian.h>
#include <net/ethernet.h>
#include <net/ipv4.h>
#include <net/netdev.h>
#include <net/packet.h>
#include <net/tcp.h>

static net_device_t       *devices[NETDEV_MAX];
static spinlock_t          devices_lock;
static uint32_t            next_ifindex = 1;
static netdev_lifecycle_fn lifecycle_notifier;
static void               *lifecycle_context;

uint32_t net_checksum_add(uint32_t sum, const void *data, size_t length)
{
    const uint8_t *bytes = data;
    while (length >= 2) {
        sum += ((uint16_t)bytes[0] << 8) | bytes[1];
        bytes += 2;
        length -= 2;
    }
    if (length) sum += (uint16_t)bytes[0] << 8;
    return sum;
}

uint16_t net_checksum_finish(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
    return (uint16_t)~sum;
}

uint16_t net_checksum(const void *data, size_t length)
{
    return net_checksum_finish(net_checksum_add(0, data, length));
}

uint16_t net_checksum_ipv4_pseudo(uint32_t source, uint32_t destination, uint8_t protocol, const void *data, size_t length)
{
    uint8_t pseudo[12];
    net_write_be32(pseudo, source);
    net_write_be32(pseudo + 4, destination);
    pseudo[8] = 0;
    pseudo[9] = protocol;
    net_write_be16(pseudo + 10, (uint16_t)length);
    return net_checksum_finish(net_checksum_add(net_checksum_add(0, pseudo, sizeof(pseudo)), data, length));
}

net_pbuf_t *net_pbuf_alloc(size_t payload_length, size_t headroom)
{
    if (payload_length > NET_PBUF_MAX_SIZE || headroom > NET_PBUF_MAX_SIZE || payload_length > NET_PBUF_MAX_SIZE - headroom) return NULL;
    net_pbuf_t *pbuf = calloc(1, sizeof(*pbuf));
    if (!pbuf) return NULL;
    size_t total  = payload_length + headroom;
    pbuf->storage = malloc(total ? total : 1);
    if (!pbuf->storage) {
        free(pbuf);
        return NULL;
    }
    pbuf->data     = pbuf->storage + headroom;
    pbuf->length   = payload_length;
    pbuf->capacity = total;
    pbuf->refs     = 1;
    return pbuf;
}

int net_packet_init_external(net_packet_t *packet, void *data, size_t length, net_packet_release_t release, void *context)
{
    if (!packet || (!data && length)) return -EINVAL;
    memset(packet, 0, sizeof(*packet));
    packet->storage         = data;
    packet->data            = data;
    packet->length          = length;
    packet->capacity        = length;
    packet->refs            = 1;
    packet->release         = release;
    packet->release_context = context;
    packet->external        = 1;
    return 0;
}

void net_packet_get(net_packet_t *packet)
{
    net_pbuf_ref(packet);
}
void net_packet_put(net_packet_t *packet)
{
    net_pbuf_free(packet);
}
void *net_packet_data(net_packet_t *packet)
{
    return packet ? packet->data : NULL;
}
size_t net_packet_length(const net_packet_t *packet)
{
    return packet ? packet->length : 0;
}

net_pbuf_t *net_pbuf_from(const void *data, size_t length, size_t headroom)
{
    if (!data && length) return NULL;
    net_pbuf_t *pbuf = net_pbuf_alloc(length, headroom);
    if (pbuf && length) memcpy(pbuf->data, data, length);
    return pbuf;
}

net_pbuf_t *net_pbuf_clone(const net_pbuf_t *pbuf, size_t headroom)
{
    return pbuf ? net_pbuf_from(pbuf->data, pbuf->length, headroom) : NULL;
}

void net_pbuf_ref(net_pbuf_t *pbuf)
{
    if (!pbuf) return;
    spin_lock(&pbuf->lock);
    pbuf->refs++;
    spin_unlock(&pbuf->lock);
}

void net_pbuf_free(net_pbuf_t *pbuf)
{
    if (!pbuf) return;
    spin_lock(&pbuf->lock);
    uint32_t refs = --pbuf->refs;
    spin_unlock(&pbuf->lock);
    if (!refs) {
        if (pbuf->release) pbuf->release(pbuf->release_context, pbuf->storage);
        if (!pbuf->external) {
            free(pbuf->storage);
            free(pbuf);
        }
    }
}

size_t net_pbuf_headroom(const net_pbuf_t *pbuf)
{
    return pbuf ? (size_t)(pbuf->data - pbuf->storage) : 0;
}

void *net_pbuf_push(net_pbuf_t *pbuf, size_t length)
{
    if (!pbuf || length > net_pbuf_headroom(pbuf)) return NULL;
    pbuf->data -= length;
    pbuf->length += length;
    return pbuf->data;
}

void *net_pbuf_pull(net_pbuf_t *pbuf, size_t length)
{
    if (!pbuf || length > pbuf->length) return NULL;
    pbuf->data += length;
    pbuf->length -= length;
    return pbuf->data;
}

int net_pbuf_trim(net_pbuf_t *pbuf, size_t length)
{
    if (!pbuf || length > pbuf->length) return -EINVAL;
    pbuf->length = length;
    return 0;
}

void net_init(void)
{
}

void net_timer(uint64_t now_ticks)
{
    arp_timer(now_ticks);
    tcp_timer(now_ticks);
    dhcp_timer(now_ticks);
}

int netdev_register(net_device_t *device)
{
    if (!device || !device->ops || !device->ops->xmit || !device->name[0] || device->mtu < NETDEV_MTU_MIN || device->mtu > NETDEV_MTU_MAX)
        return -EINVAL;
    spin_lock(&devices_lock);
    int slot = -1;
    for (unsigned i = 0; i < NETDEV_MAX; i++) {
        if (devices[i] && !strncmp(devices[i]->name, device->name, NETDEV_NAME_MAX)) {
            spin_unlock(&devices_lock);
            return -EEXIST;
        }
        if (!devices[i] && slot < 0) slot = (int)i;
    }
    if (slot < 0) {
        spin_unlock(&devices_lock);
        return -ENOSPC;
    }
    device->refs    = 1;
    device->ifindex = next_ifindex++;
    if (!next_ifindex) next_ifindex = 1;
    device->registered           = 1;
    devices[slot]                = device;
    netdev_lifecycle_fn notifier = lifecycle_notifier;
    void               *context  = lifecycle_context;
    spin_unlock(&devices_lock);
    if (notifier) notifier(device, NETDEV_REGISTERED, context);
    return 0;
}

int netdev_init(netdev_t *device, const char *name, const netdev_ops_t *ops, void *private_data)
{
    if (!device || !name || !name[0] || !ops || !ops->xmit || strlen(name) >= NETDEV_NAME_MAX) return -EINVAL;
    memset(device, 0, sizeof(*device));
    strncpy(device->name, name, NETDEV_NAME_MAX - 1);
    device->ops         = ops;
    device->driver_data = private_data;
    device->mtu         = 1500;
    return 0;
}

netdev_t *netdev_find(const char *name)
{
    return netdev_get_by_name(name);
}
void netdev_get(netdev_t *device)
{
    if (!device) return;
    spin_lock(&device->lock);
    device->refs++;
    spin_unlock(&device->lock);
}
void *netdev_private(netdev_t *device)
{
    return device ? device->driver_data : NULL;
}

int netdev_unregister(net_device_t *device)
{
    if (!device) return -EINVAL;
    spin_lock(&devices_lock);
    int found = 0;
    for (unsigned i = 0; i < NETDEV_MAX; i++) {
        if (devices[i] == device) {
            devices[i] = NULL;
            found      = 1;
            break;
        }
    }
    if (!found) {
        spin_unlock(&devices_lock);
        return -ENOENT;
    }
    spin_lock(&device->lock);
    int active = !!(device->flags & NETDEV_F_UP);
    device->flags &= ~(NETDEV_F_UP | NETDEV_F_RUNNING);
    device->registered = 0;
    spin_unlock(&device->lock);
    spin_unlock(&devices_lock);
    if (lifecycle_notifier) lifecycle_notifier(device, NETDEV_UNREGISTERED, lifecycle_context);
    if (active && device->ops->stop) device->ops->stop(device);
    arp_device_removed(device);
    dhcp_device_removed(device);
    netdev_put(device);
    return 0;
}

static net_device_t *device_get_locked(net_device_t *device)
{
    if (!device || !device->registered) return NULL;
    spin_lock(&device->lock);
    device->refs++;
    spin_unlock(&device->lock);
    return device;
}

net_device_t *netdev_get_by_name(const char *name)
{
    if (!name) return NULL;
    spin_lock(&devices_lock);
    net_device_t *result = NULL;
    for (unsigned i = 0; i < NETDEV_MAX; i++) {
        if (devices[i] && !strncmp(devices[i]->name, name, NETDEV_NAME_MAX)) {
            result = device_get_locked(devices[i]);
            break;
        }
    }
    spin_unlock(&devices_lock);
    return result;
}

net_device_t *netdev_get_default(void)
{
    spin_lock(&devices_lock);
    net_device_t *result = NULL;
    for (unsigned i = 0; i < NETDEV_MAX; i++) {
        if (devices[i] && (devices[i]->flags & (NETDEV_F_UP | NETDEV_F_RUNNING)) == (NETDEV_F_UP | NETDEV_F_RUNNING)) {
            result = device_get_locked(devices[i]);
            break;
        }
    }
    spin_unlock(&devices_lock);
    return result;
}

void netdev_iterate(netdev_iter_fn callback, void *context)
{
    if (!callback) return;
    net_device_t *snapshot[NETDEV_MAX];
    size_t        count = 0;
    spin_lock(&devices_lock);
    for (size_t i = 0; i < NETDEV_MAX; i++) {
        if (devices[i]) snapshot[count++] = device_get_locked(devices[i]);
    }
    spin_unlock(&devices_lock);
    for (size_t i = 0; i < count; i++) {
        callback(snapshot[i], context);
        netdev_put(snapshot[i]);
    }
}

int netdev_set_lifecycle_notifier(netdev_lifecycle_fn callback, void *context)
{
    spin_lock(&devices_lock);
    if (lifecycle_notifier && callback && lifecycle_notifier != callback) {
        spin_unlock(&devices_lock);
        return -EBUSY;
    }
    lifecycle_notifier = callback;
    lifecycle_context  = callback ? context : NULL;
    spin_unlock(&devices_lock);
    return 0;
}

void netdev_put(net_device_t *device)
{
    if (!device) return;
    spin_lock(&device->lock);
    if (device->refs) device->refs--;
    spin_unlock(&device->lock);
}

int netdev_set_up(net_device_t *device, int up)
{
    if (!device || !device->registered) return -ENODEV;
    if (up) {
        int status = device->ops->open ? device->ops->open(device) : 0;
        if (status) return status;
        spin_lock(&device->lock);
        device->flags |= NETDEV_F_UP | NETDEV_F_RUNNING;
        spin_unlock(&device->lock);
    } else {
        spin_lock(&device->lock);
        int active = !!(device->flags & NETDEV_F_UP);
        device->flags &= ~(NETDEV_F_UP | NETDEV_F_RUNNING);
        spin_unlock(&device->lock);
        if (active && device->ops->stop) device->ops->stop(device);
    }
    return 0;
}

int netdev_set_mtu(net_device_t *device, uint32_t mtu)
{
    if (!device || mtu < NETDEV_MTU_MIN || mtu > NETDEV_MTU_MAX) return -EINVAL;
    int status = device->ops->set_mtu ? device->ops->set_mtu(device, mtu) : 0;
    if (status) return status;
    spin_lock(&device->lock);
    device->mtu = mtu;
    spin_unlock(&device->lock);
    return 0;
}

int netdev_configure_ipv4(net_device_t *device, uint32_t address, uint32_t netmask, uint32_t gateway)
{
    if (!device || !device->registered) return -ENODEV;
    if (netmask && (netmask | (netmask - 1U)) != UINT32_MAX) return -EINVAL;
    if (gateway && ((gateway & netmask) != (address & netmask))) return -EINVAL;
    spin_lock(&device->lock);
    device->ipv4_address = address;
    device->ipv4_netmask = netmask;
    device->ipv4_gateway = gateway;
    spin_unlock(&device->lock);
    return 0;
}

int netdev_configure_dns(net_device_t *device, const uint32_t *servers, size_t count)
{
    if (!device || !device->registered) return -ENODEV;
    if ((!servers && count) || count > NETDEV_DNS_MAX) return -EINVAL;
    spin_lock(&device->lock);
    memset(device->ipv4_dns, 0, sizeof(device->ipv4_dns));
    if (count) memcpy(device->ipv4_dns, servers, count * sizeof(*servers));
    spin_unlock(&device->lock);
    return 0;
}

size_t netdev_get_dns_servers(net_device_t *device, uint32_t *servers, size_t capacity)
{
    if (!device || (!servers && capacity)) return 0;
    size_t count  = 0;
    size_t copied = 0;
    spin_lock(&device->lock);
    for (size_t i = 0; i < NETDEV_DNS_MAX; i++) {
        if (device->ipv4_dns[i]) {
            if (copied < capacity) servers[copied++] = device->ipv4_dns[i];
            count++;
        }
    }
    spin_unlock(&device->lock);
    return count;
}

int netdev_udp_broadcast(net_device_t *device, uint32_t source, uint16_t source_port, uint16_t destination_port, const void *data,
                         size_t length)
{
    enum { UDP_HEADER_LENGTH = 8 };
    if (!device || !source_port || !destination_port || (!data && length)) return -EINVAL;
    if (length > UINT16_MAX - UDP_HEADER_LENGTH - IPV4_HEADER_MIN || length + UDP_HEADER_LENGTH + IPV4_HEADER_MIN > device->mtu)
        return -EMSGSIZE;
    net_pbuf_t *packet = net_pbuf_alloc(UDP_HEADER_LENGTH + length, NET_PBUF_HEADROOM);
    if (!packet) return -ENOMEM;
    net_write_be16(packet->data, source_port);
    net_write_be16(packet->data + 2, destination_port);
    net_write_be16(packet->data + 4, (uint16_t)packet->length);
    net_write_be16(packet->data + 6, 0);
    if (length) memcpy(packet->data + UDP_HEADER_LENGTH, data, length);
    uint16_t checksum = net_checksum_ipv4_pseudo(source, UINT32_MAX, IPV4_PROTO_UDP, packet->data, packet->length);
    net_write_be16(packet->data + 6, checksum ? checksum : UINT16_MAX);

    uint8_t *header = net_pbuf_push(packet, IPV4_HEADER_MIN);
    if (!header) {
        net_pbuf_free(packet);
        return -ENOBUFS;
    }
    memset(header, 0, IPV4_HEADER_MIN);
    header[0] = 0x45;
    net_write_be16(header + 2, (uint16_t)packet->length);
    net_write_be16(header + 6, 0x4000);
    header[8] = 64;
    header[9] = IPV4_PROTO_UDP;
    net_write_be32(header + 12, source);
    net_write_be32(header + 16, UINT32_MAX);
    net_write_be16(header + 10, net_checksum(header, IPV4_HEADER_MIN));
    int status = ethernet_output(device, packet, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", ETH_TYPE_IPV4);
    net_pbuf_free(packet);
    return status;
}

void netdev_get_stats(net_device_t *device, netdev_stats_t *stats)
{
    if (!device || !stats) return;
    spin_lock(&device->lock);
    *stats = device->stats;
    spin_unlock(&device->lock);
}

int netdev_rx(net_device_t *device, net_pbuf_t *packet)
{
    if (!packet) return -EINVAL;
    if (!device || !device->registered || !(device->flags & NETDEV_F_UP)) {
        if (device) device->stats.rx_dropped++;
        net_pbuf_free(packet);
        return -ENETDOWN;
    }
    size_t length = packet->length;
    int    status = ethernet_input(device, packet);
    spin_lock(&device->lock);
    if (!status) {
        device->stats.rx_packets++;
        device->stats.rx_bytes += length;
    } else
        device->stats.rx_dropped++;
    spin_unlock(&device->lock);
    return status;
}

int netdev_tx(net_device_t *device, net_pbuf_t *packet)
{
    if (!device || !packet) return -EINVAL;
    if (!device->registered || (device->flags & (NETDEV_F_UP | NETDEV_F_RUNNING)) != (NETDEV_F_UP | NETDEV_F_RUNNING)) return -ENETDOWN;
    int status = device->ops->xmit(device, packet);
    spin_lock(&device->lock);
    if (!status) {
        device->stats.tx_packets++;
        device->stats.tx_bytes += packet->length;
    } else {
        device->stats.tx_errors++;
        device->stats.tx_dropped++;
    }
    spin_unlock(&device->lock);
    return status;
}

int netdev_transmit(netdev_t *device, net_packet_t *packet)
{
    int status;
    if (!device || !packet)
        status = -EINVAL;
    else if (!device->registered)
        status = -ENODEV;
    else
        status = device->ops->xmit(device, packet);
    net_packet_put(packet);
    return status;
}
