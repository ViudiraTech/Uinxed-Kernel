/*
 *
 *      input_sysfs.c
 *      sysfs class support for evdev input devices
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/input_sysfs.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/printk.h>

static struct class input_class = {.name = "input"};
static bool input_class_ready;

static evdev_t *input_evdev(struct device *dev)
{
    return dev ? dev->driver_data : NULL;
}
static ssize_t name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    evdev_t *evdev = input_evdev(dev);
    return sysfs_emit(buf, "%s\n", evdev && evdev->input_dev ? evdev->input_dev->name : "");
}
static ssize_t phys_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    evdev_t *evdev = input_evdev(dev);
    return sysfs_emit(buf, "%s\n", evdev && evdev->input_dev ? evdev->input_dev->phys : "");
}
static ssize_t dev_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return sysfs_emit(buf, "%u:%u\n", MAJOR(dev->devt), MINOR(dev->devt));
}
static ssize_t bustype_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    evdev_t *evdev = input_evdev(dev);
    return sysfs_emit(buf, "%04x\n", evdev && evdev->input_dev ? evdev->input_dev->id.bustype : 0);
}
static ssize_t vendor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    evdev_t *evdev = input_evdev(dev);
    return sysfs_emit(buf, "%04x\n", evdev && evdev->input_dev ? evdev->input_dev->id.vendor : 0);
}
static ssize_t product_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    evdev_t *evdev = input_evdev(dev);
    return sysfs_emit(buf, "%04x\n", evdev && evdev->input_dev ? evdev->input_dev->id.product : 0);
}
static ssize_t version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    evdev_t *evdev = input_evdev(dev);
    return sysfs_emit(buf, "%04x\n", evdev && evdev->input_dev ? evdev->input_dev->id.version : 0);
}
static ssize_t bitmap_show(char *buf, const uint32_t *bits, size_t words)
{
    int at = 0;
    while (words > 1 && !bits[words - 1]) words--;
    for (size_t word = words; word > 0; word--) at += sysfs_emit_at(buf, at, word == 1 ? "%08x\n" : "%08x ", bits[word - 1]);
    return at;
}
static ssize_t ev_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return bitmap_show(buf, input_evdev(dev)->input_dev->evbit, (EV_CNT + 31) / 32);
}
static ssize_t key_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return bitmap_show(buf, input_evdev(dev)->input_dev->keybit, (KEY_CNT + 31) / 32);
}
static ssize_t rel_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return bitmap_show(buf, input_evdev(dev)->input_dev->relbit, (REL_CNT + 31) / 32);
}

static DEVICE_ATTR(name, 0444, name_show, NULL);
static DEVICE_ATTR(phys, 0444, phys_show, NULL);
static DEVICE_ATTR(bustype, 0444, bustype_show, NULL);
static DEVICE_ATTR(vendor, 0444, vendor_show, NULL);
static DEVICE_ATTR(product, 0444, product_show, NULL);
static DEVICE_ATTR(version, 0444, version_show, NULL);
static DEVICE_ATTR(dev, 0444, dev_show, NULL);
static DEVICE_ATTR(ev, 0444, ev_show, NULL);
static DEVICE_ATTR(key, 0444, key_show, NULL);
static DEVICE_ATTR(rel, 0444, rel_show, NULL);

static const struct device_attribute *input_evdev_attributes[] = {
    &dev_attr_name,    &dev_attr_phys, &dev_attr_bustype, &dev_attr_vendor, &dev_attr_product,
    &dev_attr_version, &dev_attr_dev,  &dev_attr_ev,      &dev_attr_key,    &dev_attr_rel,
};

void input_sysfs_init(void)
{
    int result;

    if (input_class_ready) return;
    result = class_register(&input_class);
    if (result != EOK) {
        plogk("input_sysfs: input class registration failed: %d\n", result);
        return;
    }
    input_class_ready = true;
    for (int minor = 0; minor < EVDEV_MAX_DEVICES; minor++) input_sysfs_register_evdev(evdev_find_by_minor(minor));
}

int input_sysfs_register_evdev(evdev_t *evdev)
{
    char           name[24];
    struct device *device;

    if (!evdev) return -EINVAL;
    if (!input_class_ready || evdev->sysfs_device) return EOK;
    (void)snprintf(name, sizeof(name), "event%d", evdev->minor);
    device = device_create(&input_class, NULL, evdev_devt(evdev), evdev, "%s", name);
    if (!device) return -ENOMEM;
    evdev->sysfs_device = device;
    for (size_t i = 0; i < sizeof(input_evdev_attributes) / sizeof(input_evdev_attributes[0]); i++) {
        int result = device_create_file(device, input_evdev_attributes[i]);
        if (result == EOK) continue;
        while (i) device_remove_file(device, input_evdev_attributes[--i]);
        device_unregister(device);
        evdev->sysfs_device = NULL;
        return result;
    }
    plogk("input_sysfs: /sys/class/input/event%d registered (%u:%u)\n", evdev->minor, MAJOR(device->devt), MINOR(device->devt));
    return EOK;
}

void input_sysfs_unregister_evdev(evdev_t *evdev)
{
    struct device *device;

    if (!evdev || !evdev->sysfs_device) return;
    device = evdev->sysfs_device;
    for (size_t i = 0; i < sizeof(input_evdev_attributes) / sizeof(input_evdev_attributes[0]); i++)
        device_remove_file(device, input_evdev_attributes[i]);
    device_unregister(device);
    evdev->sysfs_device = NULL;
}
