/*
 *
 *      net_sysfs.c
 *      Network device sysfs interface (/sys/class/net/)
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <fs/sysfs/net_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <net/netdev.h>

#define IFF_UP        0x0001U
#define IFF_BROADCAST 0x0002U
#define IFF_RUNNING   0x0040U
#define ARPHRD_ETHER  1U

static struct class net_class = {.name = "net"};
static int net_class_ready;
static struct {
        net_device_t  *netdev;
        struct device *device;
} net_devices[NETDEV_MAX];
static spinlock_t net_devices_lock;

static net_device_t *to_netdev(struct device *device)
{
    return device ? device->driver_data : NULL;
}

static ssize_t address_show(struct device *device, struct device_attribute *attr, char *buf)
{
    uint8_t       address[6];
    net_device_t *netdev = to_netdev(device);
    (void)attr;
    if (!netdev) return -ENODEV;
    spin_lock(&netdev->lock);
    for (size_t i = 0; i < sizeof(address); i++) address[i] = netdev->address[i];
    spin_unlock(&netdev->lock);
    return sysfs_emit(buf, "%02x:%02x:%02x:%02x:%02x:%02x\n", address[0], address[1], address[2], address[3], address[4], address[5]);
}

static ssize_t mtu_show(struct device *device, struct device_attribute *attr, char *buf)
{
    net_device_t *netdev = to_netdev(device);
    uint32_t      mtu;
    (void)attr;
    if (!netdev) return -ENODEV;
    spin_lock(&netdev->lock);
    mtu = netdev->mtu;
    spin_unlock(&netdev->lock);
    return sysfs_emit(buf, "%u\n", mtu);
}

static ssize_t operstate_show(struct device *device, struct device_attribute *attr, char *buf)
{
    net_device_t *netdev = to_netdev(device);
    uint32_t      flags;
    (void)attr;
    if (!netdev) return -ENODEV;
    spin_lock(&netdev->lock);
    flags = netdev->flags;
    spin_unlock(&netdev->lock);
    return sysfs_emit(buf, "%s\n", (flags & NETDEV_F_RUNNING) ? "up" : "down");
}

static ssize_t flags_show(struct device *device, struct device_attribute *attr, char *buf)
{
    net_device_t *netdev = to_netdev(device);
    uint32_t      netdev_flags;
    uint32_t      flags = 0;
    (void)attr;
    if (!netdev) return -ENODEV;
    spin_lock(&netdev->lock);
    netdev_flags = netdev->flags;
    spin_unlock(&netdev->lock);
    if (netdev_flags & NETDEV_F_UP) flags |= IFF_UP;
    if (netdev_flags & NETDEV_F_BROADCAST) flags |= IFF_BROADCAST;
    if (netdev_flags & NETDEV_F_RUNNING) flags |= IFF_RUNNING;
    return sysfs_emit(buf, "0x%x\n", flags);
}

static ssize_t type_show(struct device *device, struct device_attribute *attr, char *buf)
{
    (void)device;
    (void)attr;
    return sysfs_emit(buf, "%u\n", ARPHRD_ETHER);
}

static ssize_t ifindex_show(struct device *device, struct device_attribute *attr, char *buf)
{
    net_device_t *netdev = to_netdev(device);
    (void)attr;
    return netdev ? sysfs_emit(buf, "%u\n", netdev->ifindex) : -ENODEV;
}

static ssize_t statistic_show(struct device *device, struct device_attribute *attr, char *buf)
{
    netdev_stats_t stats;
    net_device_t  *netdev = to_netdev(device);
    uint64_t       value;
    if (!netdev) return -ENODEV;
    netdev_get_stats(netdev, &stats);
    if (streq(attr->attr.name, "rx_bytes"))
        value = stats.rx_bytes;
    else if (streq(attr->attr.name, "rx_packets"))
        value = stats.rx_packets;
    else if (streq(attr->attr.name, "rx_errors"))
        value = stats.rx_errors;
    else if (streq(attr->attr.name, "rx_dropped"))
        value = stats.rx_dropped;
    else if (streq(attr->attr.name, "tx_bytes"))
        value = stats.tx_bytes;
    else if (streq(attr->attr.name, "tx_packets"))
        value = stats.tx_packets;
    else if (streq(attr->attr.name, "tx_errors"))
        value = stats.tx_errors;
    else
        value = stats.tx_dropped;
    return sysfs_emit(buf, "%llu\n", (unsigned long long)value);
}

static DEVICE_ATTR(address, 0444, address_show, NULL);
static DEVICE_ATTR(mtu, 0444, mtu_show, NULL);
static DEVICE_ATTR(operstate, 0444, operstate_show, NULL);
static DEVICE_ATTR(flags, 0444, flags_show, NULL);
static DEVICE_ATTR(type, 0444, type_show, NULL);
static DEVICE_ATTR(ifindex, 0444, ifindex_show, NULL);
static DEVICE_ATTR(rx_bytes, 0444, statistic_show, NULL);
static DEVICE_ATTR(rx_packets, 0444, statistic_show, NULL);
static DEVICE_ATTR(rx_errors, 0444, statistic_show, NULL);
static DEVICE_ATTR(rx_dropped, 0444, statistic_show, NULL);
static DEVICE_ATTR(tx_bytes, 0444, statistic_show, NULL);
static DEVICE_ATTR(tx_packets, 0444, statistic_show, NULL);
static DEVICE_ATTR(tx_errors, 0444, statistic_show, NULL);
static DEVICE_ATTR(tx_dropped, 0444, statistic_show, NULL);

static const struct device_attribute *net_attributes[] = {
    &dev_attr_address, &dev_attr_mtu, &dev_attr_operstate, &dev_attr_flags, &dev_attr_type, &dev_attr_ifindex,
};

static struct attribute *statistics_attributes[] = {
    &dev_attr_rx_bytes.attr,   &dev_attr_rx_packets.attr, &dev_attr_rx_errors.attr,
    &dev_attr_rx_dropped.attr, &dev_attr_tx_bytes.attr,   &dev_attr_tx_packets.attr,
    &dev_attr_tx_errors.attr,  &dev_attr_tx_dropped.attr, NULL,
};

static const struct attribute_group statistics_group = {
    .name  = "statistics",
    .attrs = statistics_attributes,
};

static void net_sysfs_publish(net_device_t *netdev, void *context)
{
    struct device *device;
    int            slot    = -1;
    size_t         created = 0;
    (void)context;
    if (!net_class_ready || !netdev || !netdev->registered) return;
    spin_lock(&net_devices_lock);
    for (size_t i = 0; i < NETDEV_MAX; i++) {
        if (net_devices[i].netdev == netdev) {
            spin_unlock(&net_devices_lock);
            return;
        }
        if (!net_devices[i].netdev && slot < 0) slot = (int)i;
    }
    if (slot < 0) {
        spin_unlock(&net_devices_lock);
        return;
    }
    net_devices[slot].netdev = netdev;
    spin_unlock(&net_devices_lock);
    device = device_create(&net_class, NULL, 0, netdev, "%s", netdev->name);
    if (!device) goto clear_slot;
    for (; created < sizeof(net_attributes) / sizeof(net_attributes[0]); created++) {
        if (device_create_file(device, net_attributes[created]) == EOK) continue;
        while (created) device_remove_file(device, net_attributes[--created]);
        device_unregister(device);
        goto clear_slot;
    }
    if (sysfs_create_group(&device->kobj, &statistics_group) == EOK) {
        spin_lock(&net_devices_lock);
        if (net_devices[slot].netdev == netdev && netdev->registered) {
            net_devices[slot].device = device;
            spin_unlock(&net_devices_lock);
            return;
        }
        spin_unlock(&net_devices_lock);
        sysfs_remove_group(&device->kobj, &statistics_group);
        while (created) device_remove_file(device, net_attributes[--created]);
        device_unregister(device);
        return;
    }
    while (created) device_remove_file(device, net_attributes[--created]);
    device_unregister(device);

clear_slot:
    spin_lock(&net_devices_lock);
    if (net_devices[slot].netdev == netdev) {
        net_devices[slot].netdev = NULL;
        net_devices[slot].device = NULL;
    }
    spin_unlock(&net_devices_lock);
}

static void net_sysfs_unpublish(net_device_t *netdev)
{
    struct device *device = NULL;
    spin_lock(&net_devices_lock);
    for (size_t i = 0; i < NETDEV_MAX; i++) {
        if (net_devices[i].netdev == netdev) {
            device                = net_devices[i].device;
            net_devices[i].netdev = NULL;
            net_devices[i].device = NULL;
            break;
        }
    }
    spin_unlock(&net_devices_lock);
    if (!device) return;
    device->driver_data = NULL;
    sysfs_remove_group(&device->kobj, &statistics_group);
    for (size_t i = 0; i < sizeof(net_attributes) / sizeof(net_attributes[0]); i++) device_remove_file(device, net_attributes[i]);
    device_unregister(device);
}

static void net_sysfs_lifecycle(net_device_t *netdev, netdev_lifecycle_event_t event, void *context)
{
    (void)context;
    if (event == NETDEV_REGISTERED)
        net_sysfs_publish(netdev, NULL);
    else
        net_sysfs_unpublish(netdev);
}

void net_sysfs_init(void)
{
#if !CONFIG_NET || !CONFIG_SYSFS
    return;
#endif
    if (net_class_ready) return;
    if (class_register(&net_class) != EOK) return;
    net_class_ready = 1;
    if (netdev_set_lifecycle_notifier(net_sysfs_lifecycle, NULL) != EOK) return;
    netdev_iterate(net_sysfs_publish, NULL);
}
