/*
 *
 *      net_sysfs.c
 *      Network device sysfs interface (/sys/class/net/)
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <fs/sysfs/net_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <net/core/netdev.h>

#define IFF_UP        0x0001U
#define IFF_BROADCAST 0x0002U
#define IFF_RUNNING   0x0040U
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
    if (!netdev) {
        plogk("net_sysfs: Address_show on %s without netdev.\n", device ? device->kobj.name : "?");
        return -ENODEV;
    }
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
    if (!netdev) {
        plogk("net_sysfs: Mtu_show on %s without netdev.\n", device ? device->kobj.name : "?");
        return -ENODEV;
    }
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
    if (!netdev) {
        plogk("net_sysfs: Operstate_show on %s without netdev.\n", device ? device->kobj.name : "?");
        return -ENODEV;
    }
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
    if (!netdev) {
        plogk("net_sysfs: Flags_show on %s without netdev.\n", device ? device->kobj.name : "?");
        return -ENODEV;
    }
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
    if (!netdev) {
        plogk("net_sysfs: Ifindex_show on %s without netdev.\n", device ? device->kobj.name : "?");
        return -ENODEV;
    }
    return sysfs_emit(buf, "%u\n", netdev->ifindex);
}

static ssize_t statistic_show(struct device *device, struct device_attribute *attr, char *buf)
{
    netdev_stats_t stats;
    net_device_t  *netdev = to_netdev(device);
    uint64_t       value;
    if (!netdev) {
        plogk("net_sysfs: Statistic_show on %s without netdev.\n", device ? device->kobj.name : "?");
        return -ENODEV;
    }
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

static struct attribute *net_attributes[] = {
    &dev_attr_address.attr, &dev_attr_mtu.attr, &dev_attr_operstate.attr, &dev_attr_flags.attr, &dev_attr_type.attr, &dev_attr_ifindex.attr, NULL,
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

static struct attribute_group net_group = {
    .attrs = net_attributes,
};

static int net_device_uevent(struct device *device, struct kobj_uevent_env *env)
{
    net_device_t *netdev = to_netdev(device);
    if (!netdev) {
        plogk("net_sysfs: Uevent on %s without netdev.\n", device ? device->kobj.name : "?");
        return -ENODEV;
    }
    int ret = add_uevent_var(env, "INTERFACE=%s", netdev->name);
    if (ret) return ret;
    return add_uevent_var(env, "IFINDEX=%u", netdev->ifindex);
}

static const struct attribute_group *net_dev_groups[] = {
    &net_group,
    &statistics_group,
    NULL,
};

static struct class net_class = {.name = "net", .dev_uevent = net_device_uevent, .dev_groups = net_dev_groups};

static void net_sysfs_publish(net_device_t *netdev, void *context)
{
    struct device *device;
    int            slot  = -1;
    int           *count = (int *)context;
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
        plogk("net_sysfs: Publish %s failed, slot table full (max %d)\n", netdev->name, NETDEV_MAX);
        spin_unlock(&net_devices_lock);
        return;
    }
    net_devices[slot].netdev = netdev;
    spin_unlock(&net_devices_lock);
    device = device_create(&net_class, NULL, 0, netdev, "%s", netdev->name);
    if (!device) {
        plogk("net_sysfs: Device_create failed for %s\n", netdev->name);
        goto clear_slot;
    }
    spin_lock(&net_devices_lock);
    if (net_devices[slot].netdev == netdev && netdev->registered) {
        net_devices[slot].device = device;
        spin_unlock(&net_devices_lock);
        if (count) (*count)++;
        return;
    }
    plogk("net_sysfs: Publish %s lost race, unregistering device.\n", netdev->name);
    spin_unlock(&net_devices_lock);
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

/* Export every registered network device to /sys/class/net/. */
void net_sysfs_init(void)
{
#if !CONFIG_NET || !CONFIG_SYSFS
    return;
#endif
    int devices = 0;
    if (net_class_ready) return;
    if (class_register(&net_class) != EOK) {
        plogk("net_sysfs: Class_register(net) failed.\n");
        return;
    }
    net_class_ready = 1;
    if (netdev_set_lifecycle_notifier(net_sysfs_lifecycle, NULL) != EOK) {
        plogk("net_sysfs: Lifecycle notifier registration failed.\n");
        return;
    }
    netdev_iterate(net_sysfs_publish, &devices);
    plogk("net_sysfs: %d network device(s) exported to /sys/class/net\n", devices);
}
