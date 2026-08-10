/*
 *
 *      pci_sysfs.c
 *      PCI bus and device sysfs integration
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/core/device.h>
#include <drivers/pci/pci.h>
#include <fs/core/vfs.h>
#include <fs/sysfs/pci_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/kobject/kobject.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>

/* ------------------------------------------------------------------ */
/*  Per-device private data                                            */
/* ------------------------------------------------------------------ */

typedef struct pci_sysfs_dev {
        pci_device_cache_t *cache;
} pci_sysfs_dev_t;

static int pci_device_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    pci_sysfs_dev_t *psd = dev->driver_data;
    pci_device_reg_t reg;
    uint32_t         subsystem_vendor;
    uint32_t         subsystem_device;
    uint32_t         class_code;
    int              ret;

    if (!psd || !psd->cache) return -ENODEV;
    reg.parent       = psd->cache;
    reg.offset       = 0x2C;
    subsystem_vendor = read_pci(reg) & 0xFFFF;
    reg.offset       = 0x2E;
    subsystem_device = read_pci(reg) & 0xFFFF;
    class_code       = psd->cache->class_code;

    ret = add_uevent_var(env, "PCI_SLOT_NAME=%s", dev_name(dev));
    if (ret) return ret;
    return add_uevent_var(env, "MODALIAS=pci:v0000%04Xd0000%04Xsv0000%04Xsd0000%04Xbc%02Xsc%02Xi%02X", psd->cache->vendor_id,
                          psd->cache->device_id, subsystem_vendor, subsystem_device, (class_code >> 16) & 0xFF, (class_code >> 8) & 0xFF,
                          class_code & 0xFF);
}

/* ------------------------------------------------------------------ */
/*  PCI bus type                                                       */
/* ------------------------------------------------------------------ */

static struct bus_type pci_bus_type = {
    .name     = "pci",
    .dev_name = "0000",
    .uevent   = pci_device_uevent,
};

/* ------------------------------------------------------------------ */
/*  Device attribute show/store functions                              */
/* ------------------------------------------------------------------ */

static ssize_t pci_vendor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    pci_sysfs_dev_t *psd = dev->driver_data;
    (void)attr;
    if (!psd || !psd->cache) return -EIO;
    return (ssize_t)sysfs_emit(buf, "0x%04x\n", (uint32_t)psd->cache->vendor_id);
}

static ssize_t pci_device_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    pci_sysfs_dev_t *psd = dev->driver_data;
    (void)attr;
    if (!psd || !psd->cache) return -EIO;
    return (ssize_t)sysfs_emit(buf, "0x%04x\n", (uint32_t)psd->cache->device_id);
}

static ssize_t pci_class_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    pci_sysfs_dev_t *psd = dev->driver_data;
    (void)attr;
    if (!psd || !psd->cache) return -EIO;
    return (ssize_t)sysfs_emit(buf, "0x%06x\n", (uint32_t)psd->cache->class_code);
}

static ssize_t pci_revision_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    pci_sysfs_dev_t *psd = dev->driver_data;
    (void)attr;
    if (!psd || !psd->cache) return -EIO;
    pci_device_reg_t reg = {.parent = psd->cache, .offset = PCI_CONF_REVISION};
    uint32_t         rev = read_pci(reg) & 0xFF;
    return (ssize_t)sysfs_emit(buf, "0x%02x\n", rev);
}

static ssize_t pci_subsystem_vendor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    pci_sysfs_dev_t *psd = dev->driver_data;
    (void)attr;
    if (!psd || !psd->cache) return -EIO;
    pci_device_reg_t reg = {.parent = psd->cache, .offset = 0x2C};
    uint32_t         val = read_pci(reg) & 0xFFFF;
    return (ssize_t)sysfs_emit(buf, "0x%04x\n", val);
}

static ssize_t pci_subsystem_device_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    pci_sysfs_dev_t *psd = dev->driver_data;
    (void)attr;
    if (!psd || !psd->cache) return -EIO;
    pci_device_reg_t reg = {.parent = psd->cache, .offset = 0x2E};
    uint32_t         val = read_pci(reg) & 0xFFFF;
    return (ssize_t)sysfs_emit(buf, "0x%04x\n", val);
}

static ssize_t pci_header_type_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    pci_sysfs_dev_t *psd = dev->driver_data;
    (void)attr;
    if (!psd || !psd->cache) return -EIO;
    return (ssize_t)sysfs_emit(buf, "0x%02x\n", (uint32_t)psd->cache->header_type);
}

static ssize_t pci_modalias_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    pci_sysfs_dev_t *psd = dev->driver_data;
    pci_device_reg_t reg;
    uint32_t         subsystem_vendor;
    uint32_t         subsystem_device;
    uint32_t         class_code;

    (void)attr;
    if (!psd || !psd->cache) return -EIO;

    reg.parent       = psd->cache;
    reg.offset       = 0x2C;
    subsystem_vendor = read_pci(reg) & 0xFFFF;
    reg.offset       = 0x2E;
    subsystem_device = read_pci(reg) & 0xFFFF;
    class_code       = psd->cache->class_code;

    return (ssize_t)sysfs_emit(buf, "pci:v0000%04Xd0000%04Xsv0000%04Xsd0000%04Xbc%02Xsc%02Xi%02X\n", psd->cache->vendor_id,
                               psd->cache->device_id, subsystem_vendor, subsystem_device, (class_code >> 16) & 0xFF, (class_code >> 8) & 0xFF,
                               class_code & 0xFF);
}

/* ------------------------------------------------------------------ */
/*  Device attributes                                                  */
/* ------------------------------------------------------------------ */

static DEVICE_ATTR(vendor, 0444, pci_vendor_show, NULL);
static DEVICE_ATTR(device, 0444, pci_device_show, NULL);
static DEVICE_ATTR(class, 0444, pci_class_show, NULL);
static DEVICE_ATTR(revision, 0444, pci_revision_show, NULL);
static DEVICE_ATTR(subsystem_vendor, 0444, pci_subsystem_vendor_show, NULL);
static DEVICE_ATTR(subsystem_device, 0444, pci_subsystem_device_show, NULL);
static DEVICE_ATTR(header_type, 0444, pci_header_type_show, NULL);
static DEVICE_ATTR(modalias, 0444, pci_modalias_show, NULL);

static struct attribute *pci_dev_attrs[] = {
    &dev_attr_vendor.attr,      &dev_attr_device.attr,           &dev_attr_class.attr,
    &dev_attr_revision.attr,    &dev_attr_subsystem_vendor.attr, &dev_attr_subsystem_device.attr,
    &dev_attr_header_type.attr, &dev_attr_modalias.attr,         NULL,
};

static struct attribute_group pci_dev_attr_group = {
    .attrs = pci_dev_attrs,
};

static const struct attribute_group *pci_dev_groups[] = {
    &pci_dev_attr_group,
    NULL,
};

/* ------------------------------------------------------------------ */
/*  Device release                                                     */
/* ------------------------------------------------------------------ */

static void pci_dev_release(struct device *dev)
{
    pci_sysfs_dev_t *psd = dev->driver_data;
    if (psd) free(psd);
    free(dev);
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                     */
/* ------------------------------------------------------------------ */

void pci_sysfs_init(void)
{
#if CONFIG_SYSFS
    pci_devices_cache_t *cache;
    pci_device_cache_t  *item;
    int                  ret;
    int                  dev_count = 0;

    /* Register the PCI bus type */
    ret = bus_register(&pci_bus_type);
    if (ret != EOK) {
        plogk("pci_sysfs: Bus_register(pci) failed: %d\n", ret);
        return;
    }

    /* Get the cached PCI devices */
    cache = pci_get_devices_cache();
    if (!cache || !cache->head) return;

    /* Iterate and register each device */
    for (item = cache->head; item; item = item->next) {
        pci_sysfs_dev_t *psd;
        struct device   *dev;
        char             name[32];

        if (!item->device) continue;

        psd = calloc(1, sizeof(*psd));
        if (!psd) continue;

        psd->cache = item;

        /* Format: 0000:bb:dd.f (domain:bus:slot.func) */
        (void)snprintf(name, sizeof(name), "%04x:%02x:%02x.%01x", item->device->domain, item->device->bus, item->device->slot,
                       item->device->func);

        dev = calloc(1, sizeof(struct device));
        if (!dev) {
            free(psd);
            continue;
        }

        dev->bus         = &pci_bus_type;
        dev->parent      = NULL;
        dev->driver_data = psd;
        dev->release     = pci_dev_release;
        dev->groups      = pci_dev_groups;
        /* devid encodes the BDF for identification */
        dev->devid = ((uint64_t)item->device->bus << 8) | ((uint64_t)item->device->slot << 3) | ((uint64_t)item->device->func);

        ret = kobject_set_name(&dev->kobj, "%s", name);
        if (ret != EOK) {
            free(dev);
            free(psd);
            continue;
        }

        ret = device_register(dev);
        if (ret != EOK) {
            plogk("pci_sysfs: Device_register(%s) failed: %d\n", name, ret);
            kobject_put(&dev->kobj);
            continue;
        }

        dev_count++;
    }

    plogk("pci_sysfs: Registered %d PCI devices on bus 'pci'\n", dev_count);
#endif
}
