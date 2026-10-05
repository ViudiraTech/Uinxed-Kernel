/*
 *
 *      netdev.c
 *      Network device management
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/printk.h>
#include <libs/std/stdbool.h>
#include <libs/std/string.h>
#include <net/core/endian.h>
#include <net/core/ethernet.h>
#include <net/core/netdev.h>
#include <net/ipv4/arp.h>
#include <net/ipv4/dhcp.h>
#include <net/ipv4/ipv4.h>
#include <net/ipv6/ndp.h>
#include <process/namespace.h>

#if CONFIG_NET

#define NETDEV_REGISTRY_MAX ((size_t)CONFIG_NETDEV_MAX * 16U)
static net_device_t       *devices[NETDEV_REGISTRY_MAX];
static spinlock_t          devices_lock;
static netdev_lifecycle_fn lifecycle_notifier;
static void               *lifecycle_context;

/* Register a device in the global table, assigning it a unique ifindex. */
int netdev_register(net_device_t *device)
{
    if (!device || !device->ops || !device->ops->xmit || !device->name[0] || device->mtu < NETDEV_MTU_MIN || device->mtu > CONFIG_NETDEV_MTU_MAX) return -EINVAL;
    if (!device->net_ns) device->net_ns = &init_net_ns;
    spin_lock(&devices_lock);
    uint32_t next_ifindex = 1;
    int slot = -1;
    for (unsigned i = 0; i < NETDEV_REGISTRY_MAX; i++) {
        if (devices[i] && devices[i]->net_ns == device->net_ns && !strncmp(devices[i]->name, device->name, CONFIG_NETDEV_NAME_MAX)) {
            spin_unlock(&devices_lock);
            plogk("net: Register %s failed: name already in use.\n", device->name);
            return -EEXIST;
        }
        if (devices[i] && devices[i]->net_ns == device->net_ns && devices[i]->ifindex >= next_ifindex) next_ifindex = devices[i]->ifindex + 1;
        if (!devices[i] && slot < 0) slot = (int)i;
    }
    if (slot < 0) {
        spin_unlock(&devices_lock);
        return -ENOSPC;
    }
    device->refs = 1;
    /* ifindex 0 is invalid; a full uint32_t space wraps to it, so skip it. */
    if (next_ifindex == 0) next_ifindex = 1;
    device->ifindex = next_ifindex;
    ++next_ifindex;
    device->registered           = 1;
    devices[slot]                = device;
    netdev_lifecycle_fn notifier = lifecycle_notifier;
    void               *context  = lifecycle_context;
    spin_unlock(&devices_lock);
    if (notifier) notifier(device, NETDEV_REGISTERED, context);
    return 0;
}

/* Initialize a driver-owned device structure with the given name and ops. */
int netdev_init(net_device_t *device, const char *name, const netdev_ops_t *ops, void *private_data)
{
    if (!device || !name || !name[0] || !ops || !ops->xmit || strlen(name) >= CONFIG_NETDEV_NAME_MAX) return -EINVAL;
    memset(device, 0, sizeof(*device));
    strncpy(device->name, name, CONFIG_NETDEV_NAME_MAX - 1);
    device->ops         = ops;
    device->driver_data = private_data;
    device->mtu         = 1500;
    return 0;
}

/* Look up a registered device by name. */
net_device_t *netdev_find(const char *name)
{
    return netdev_get_by_name(name);
}

/* Take a reference on a device. */
void netdev_get(net_device_t *device)
{
    if (!device) return;
    spin_lock(&device->lock);
    device->refs++;
    spin_unlock(&device->lock);
}

/* Driver-private pointer stored on the device. */
void *netdev_private(net_device_t *device)
{
    return device ? device->driver_data : NULL;
}

/* Unregister a device, stopping it and notifying protocol layers. */
int netdev_unregister(net_device_t *device)
{
    if (!device) return -EINVAL;
    spin_lock(&devices_lock);
    int found = 0;
    for (unsigned i = 0; i < NETDEV_REGISTRY_MAX; i++) {
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
    ipv4_device_removed(device);
    arp_device_removed(device);
    dhcp_device_removed(device);
    ndp_device_removed(device);
    netdev_put(device);
    return 0;
}

/* Take a reference on a registered device while holding the table lock. */
static net_device_t *device_get_locked(net_device_t *device)
{
    if (!device || !device->registered) return NULL;
    spin_lock(&device->lock);
    device->refs++;
    spin_unlock(&device->lock);
    return device;
}

/* Look up a registered device by name and take a reference on it. */
net_device_t *netdev_get_by_name(const char *name)
{
    return netdev_get_by_name_ns(net_namespace_current(), name);
}

net_device_t *netdev_get_by_name_ns(net_namespace_t *ns, const char *name)
{
    if (!name) return NULL;
    spin_lock(&devices_lock);
    net_device_t *result = NULL;
    for (unsigned i = 0; i < NETDEV_REGISTRY_MAX; i++) {
        if (devices[i] && devices[i]->net_ns == ns && !strncmp(devices[i]->name, name, CONFIG_NETDEV_NAME_MAX)) {
            result = device_get_locked(devices[i]);
            break;
        }
    }
    spin_unlock(&devices_lock);
    return result;
}

/* Return the first non-loopback device that is up and running. */
net_device_t *netdev_get_default(void)
{
    return netdev_get_default_ns(net_namespace_current());
}

net_device_t *netdev_get_default_ns(net_namespace_t *ns)
{
    spin_lock(&devices_lock);
    net_device_t *result   = NULL;
    net_device_t *loopback = NULL;
    for (unsigned i = 0; i < NETDEV_REGISTRY_MAX; i++) {
        if (devices[i] && devices[i]->net_ns == ns && (devices[i]->flags & (NETDEV_F_UP | NETDEV_F_RUNNING)) == (NETDEV_F_UP | NETDEV_F_RUNNING)) {
            if (!(devices[i]->flags & NETDEV_F_LOOPBACK)) {
                result = device_get_locked(devices[i]);
                break;
            }
            if (!loopback) loopback = devices[i];
        }
    }
    if (!result && loopback) result = device_get_locked(loopback);
    spin_unlock(&devices_lock);
    return result;
}

/* Invoke callback for a snapshot of all registered devices, outside the table lock. */
void netdev_iterate(netdev_iter_fn callback, void *context)
{
    netdev_iterate_ns(net_namespace_current(), callback, context);
}

void netdev_iterate_ns(net_namespace_t *ns, netdev_iter_fn callback, void *context)
{
    if (!callback) return;
    net_device_t *snapshot[NETDEV_REGISTRY_MAX];
    size_t        count = 0;
    spin_lock(&devices_lock);
    for (size_t i = 0; i < NETDEV_REGISTRY_MAX; i++)
        if (devices[i] && devices[i]->net_ns == ns) snapshot[count++] = device_get_locked(devices[i]);
    spin_unlock(&devices_lock);
    for (size_t i = 0; i < count; i++) {
        callback(snapshot[i], context);
        netdev_put(snapshot[i]);
    }
}

/* Register a callback invoked on device registration/unregistration. */
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

/* Drop a device reference acquired by netdev_get_*. */
void netdev_put(net_device_t *device)
{
    if (!device) return;
    spin_lock(&device->lock);
    bool release = false;
    if (device->refs) {
        device->refs--;
        release = device->refs == 0;
    }
    spin_unlock(&device->lock);
    if (release && device->release) device->release(device);
}

/* Bring the device up or down, invoking the driver's open/stop hooks. */
int netdev_set_up(net_device_t *device, int up)
{
    if (!device || !device->registered) return -ENODEV;
    if (up) {
        int status = device->ops->open ? device->ops->open(device) : 0;
        if (status) return status;
        spin_lock(&device->lock);
        device->flags |= NETDEV_F_UP | NETDEV_F_RUNNING;
        spin_unlock(&device->lock);
        ndp_device_up(device);
    } else {
        spin_lock(&device->lock);
        int active = !!(device->flags & NETDEV_F_UP);
        device->flags &= ~(NETDEV_F_UP | NETDEV_F_RUNNING);
        spin_unlock(&device->lock);
        if (active && device->ops->stop) device->ops->stop(device);
    }
    return 0;
}

/* Change the device MTU through the driver, if it supports it. */
int netdev_set_mtu(net_device_t *device, uint32_t mtu)
{
    if (!device || mtu < NETDEV_MTU_MIN || mtu > CONFIG_NETDEV_MTU_MAX) return -EINVAL;
    int status = device->ops->set_mtu ? device->ops->set_mtu(device, mtu) : 0;
    if (status) return status;
    spin_lock(&device->lock);
    device->mtu = mtu;
    spin_unlock(&device->lock);
    return 0;
}

/* Assign the device an IPv4 address, netmask, and gateway. */
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

/* Set the device's DNS server list. */
int netdev_configure_dns(net_device_t *device, const uint32_t *servers, size_t count)
{
    if (!device || !device->registered) return -ENODEV;
    if ((!servers && count) || count > CONFIG_NETDEV_DNS_MAX) return -EINVAL;
    spin_lock(&device->lock);
    memset(device->ipv4_dns, 0, sizeof(device->ipv4_dns));
    if (count) memcpy(device->ipv4_dns, servers, count * sizeof(*servers));
    spin_unlock(&device->lock);
    return 0;
}

/* Copy the device's DNS server list, returning the number configured. */
size_t netdev_get_dns_servers(net_device_t *device, uint32_t *servers, size_t capacity)
{
    if (!device || (!servers && capacity)) return 0;
    size_t count  = 0;
    size_t copied = 0;
    spin_lock(&device->lock);
    for (size_t i = 0; i < CONFIG_NETDEV_DNS_MAX; i++) {
        if (device->ipv4_dns[i]) {
            if (copied < capacity) servers[copied++] = device->ipv4_dns[i];
            count++;
        }
    }
    spin_unlock(&device->lock);
    return count;
}

/* Send a UDP datagram to the IPv4 broadcast address on a device. */
int netdev_udp_broadcast(net_device_t *device, uint32_t source, uint16_t source_port, uint16_t destination_port, const void *data, size_t length)
{
    enum { UDP_HEADER_LENGTH = 8 };

    if (!device || !source_port || !destination_port || (!data && length)) return -EINVAL;
    if (length > UINT16_MAX - UDP_HEADER_LENGTH - IPV4_HEADER_MIN || length + UDP_HEADER_LENGTH + IPV4_HEADER_MIN > device->mtu) return -EMSGSIZE;
    net_pbuf_t *packet = net_pbuf_alloc(UDP_HEADER_LENGTH + length, NET_PBUF_HEADROOM);
    if (!packet) return -ENOMEM;
    store_be16(packet->data, source_port);
    store_be16(packet->data + 2, destination_port);
    store_be16(packet->data + 4, (uint16_t)packet->length);
    store_be16(packet->data + 6, 0);
    if (length) memcpy(packet->data + UDP_HEADER_LENGTH, data, length);
    uint16_t checksum = net_checksum_ipv4_pseudo(source, UINT32_MAX, IPV4_PROTO_UDP, packet->data, packet->length);
    store_be16(packet->data + 6, checksum ? checksum : UINT16_MAX);

    uint8_t *header = net_pbuf_push(packet, IPV4_HEADER_MIN);
    if (!header) {
        net_pbuf_free(packet);
        return -ENOBUFS;
    }
    memset(header, 0, IPV4_HEADER_MIN);
    header[0] = 0x45;
    store_be16(header + 2, (uint16_t)packet->length);
    store_be16(header + 6, 0x4000);
    header[8] = 64;
    header[9] = IPV4_PROTO_UDP;
    store_be32(header + 12, source);
    store_be32(header + 16, UINT32_MAX);
    store_be16(header + 10, net_checksum(header, IPV4_HEADER_MIN));
    int status = ethernet_output(device, packet, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", ETH_TYPE_IPV4);
    net_pbuf_free(packet);
    return status;
}

/* Snapshot the device's packet statistics. */
void netdev_get_stats(net_device_t *device, netdev_stats_t *stats)
{
    if (!device || !stats) return;
    spin_lock(&device->lock);
    *stats = device->stats;
    spin_unlock(&device->lock);
}

/* Deliver a received packet up the protocol stack, updating stats. */
int netdev_rx(net_device_t *device, net_pbuf_t *packet)
{
    if (!packet) return -EINVAL;
    if (!device) {
        net_pbuf_free(packet);
        return -ENETDOWN;
    }

    /*
     * Snapshot the liveness flags under the device lock: netdev_unregister()
     * clears registered/UP under the same lock, so an unlocked read here
     * could deliver a packet during teardown.
     */
    spin_lock(&device->lock);
    bool up = device->registered && (device->flags & NETDEV_F_UP);
    spin_unlock(&device->lock);
    if (!up) {
        spin_lock(&device->lock);
        device->stats.rx_dropped++;
        spin_unlock(&device->lock);
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

/* Hand a packet to the driver for transmission, updating stats. */
int netdev_tx(net_device_t *device, net_pbuf_t *packet)
{
    if (!device || !packet) return -EINVAL;

    /* Snapshot the liveness flags under the device lock (see netdev_rx). */
    spin_lock(&device->lock);
    bool up = device->registered && (device->flags & (NETDEV_F_UP | NETDEV_F_RUNNING)) == (NETDEV_F_UP | NETDEV_F_RUNNING);
    spin_unlock(&device->lock);

    if (!up) return -ENETDOWN;
    size_t length = packet->length;
    int    status = device->ops->xmit(device, packet);
    spin_lock(&device->lock);

    if (!status) {
        device->stats.tx_packets++;
        device->stats.tx_bytes += length;
    } else {
        device->stats.tx_errors++;
        device->stats.tx_dropped++;
        if (status != -EAGAIN && status != -ENETDOWN) plogk("net: %s: TX failed (%d)\n", device->name, status);
    }
    spin_unlock(&device->lock);
    return status;
}

#endif
