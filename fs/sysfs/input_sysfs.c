/*
 *
 *      input_sysfs.c
 *      sysfs class support for evdev input devices
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/core/device.h>
#include <fs/sysfs/input_sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>

static bool input_class_ready;

static bool input_test_bit(unsigned int bit, const uint32_t *bits)
{
    return bits && ((bits[bit / 32] >> (bit % 32)) & 1U);
}

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
static DEVICE_ATTR(ev, 0444, ev_show, NULL);
static DEVICE_ATTR(key, 0444, key_show, NULL);
static DEVICE_ATTR(rel, 0444, rel_show, NULL);

static struct attribute *input_evdev_attributes[] = {
    &dev_attr_name.attr,    &dev_attr_phys.attr, &dev_attr_bustype.attr, &dev_attr_vendor.attr, &dev_attr_product.attr,
    &dev_attr_version.attr, &dev_attr_ev.attr,   &dev_attr_key.attr,     &dev_attr_rel.attr,    NULL,
};

static struct attribute_group input_evdev_group = {
    .attrs = input_evdev_attributes,
};

static int input_device_uevent(struct device *device, struct kobj_uevent_env *env)
{
    evdev_t *evdev = input_evdev(device);
    if (!evdev || !evdev->input_dev) return -ENODEV;
    input_dev_t *input = evdev->input_dev;
    int          ret   = add_uevent_var(env, "PRODUCT=%x/%x/%x/%x", input->id.bustype, input->id.vendor, input->id.product, input->id.version);
    if (ret) return ret;
    /* libinput consumes these standard udev properties.  The generic
     * input_id builtin cannot infer them reliably from this kernel's compact
     * sysfs capability files, so publish the authoritative device classes at
     * the source uevent just like Linux input drivers do. */
    ret = add_uevent_var(env, "ID_INPUT=1");
    if (ret) return ret;
    if (input_test_bit(EV_KEY, input->evbit)) {
        ret = add_uevent_var(env, "ID_INPUT_KEYBOARD=1");
        if (ret) return ret;
    }
    if (input_test_bit(EV_REL, input->evbit)) {
        ret = add_uevent_var(env, "ID_INPUT_MOUSE=1");
        if (ret) return ret;
    }
    if (input->name[0]) {
        ret = add_uevent_var(env, "NAME=\"%s\"", input->name);
        if (ret) return ret;
    }
    if (input->phys[0]) return add_uevent_var(env, "PHYS=\"%s\"", input->phys);
    return EOK;
}

static const struct attribute_group *input_dev_groups[] = {
    &input_evdev_group,
    NULL,
};

static struct class input_class = {.name = "input", .dev_uevent = input_device_uevent, .dev_groups = input_dev_groups};

void input_sysfs_init(void)
{
#if CONFIG_INPUT_EVDEV
    int result;

    if (input_class_ready) return;
    result = class_register(&input_class);
    if (result != EOK) {
        plogk("input_sysfs: input class registration failed: %d\n", result);
        return;
    }
    input_class_ready = true;
    for (int minor = 0; minor < EVDEV_MAX_DEVICES; minor++) input_sysfs_register_evdev(evdev_find_by_minor(minor));
#endif
}

int input_sysfs_register_evdev(evdev_t *evdev)
{
    char           name[24];
    struct device *device;

    if (!evdev) return -EINVAL;
    if (evdev->sysfs_device) return EOK;
    if (!input_class_ready) { return EOK; }
    (void)snprintf(name, sizeof(name), "event%d", evdev->minor);
    device = device_create(&input_class, NULL, evdev_devt(evdev), evdev, "%s", name);
    if (!device) { return -ENOMEM; }
    evdev->sysfs_device = device;
    return EOK;
}

void input_sysfs_unregister_evdev(evdev_t *evdev)
{
    struct device *device;

    if (!evdev || !evdev->sysfs_device) return;
    device = evdev->sysfs_device;
    device_unregister(device);
    evdev->sysfs_device = NULL;
}
