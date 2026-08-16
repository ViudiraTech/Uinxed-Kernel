/*
 *
 *      input_sysfs.c
 *      sysfs class support for evdev input devices
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <fs/sysfs/input_sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>

static bool input_class_ready;

/* Test one bit in an input capability bitmap. */
static bool input_test_bit(unsigned int bit, const uint32_t *bits)
{
    return bits && ((bits[bit / 32] >> (bit % 32)) & 1U);
}

/* Heuristically classify an input device as a keyboard. */
static bool input_is_keyboard(const input_dev_t *input)
{
    if (!input || !input_test_bit(EV_KEY, input->evbit)) return false;

    /*
     * Match the same kind of well-known alphanumeric keys that udev's
     * input_id builtin uses to distinguish a keyboard from a mouse whose
     * buttons are also reported through EV_KEY.
     */
    return input_test_bit(KEY_Q, input->keybit) && input_test_bit(KEY_A, input->keybit) && input_test_bit(KEY_Z, input->keybit) && input_test_bit(KEY_ENTER, input->keybit)
           && input_test_bit(KEY_SPACE, input->keybit);
}

/* Heuristically classify an input device as a mouse. */
static bool input_is_mouse(const input_dev_t *input)
{
    if (!input || !input_test_bit(EV_REL, input->evbit) || !input_test_bit(EV_KEY, input->evbit)) return false;
    return input_test_bit(REL_X, input->relbit) && input_test_bit(REL_Y, input->relbit) && input_test_bit(BTN_LEFT, input->keybit);
}

/* Return the evdev bound to a device-model device. */
static evdev_t *input_evdev(struct device *dev)
{
    return dev ? dev->driver_data : NULL;
}

/* Show the device name. */
static ssize_t name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    evdev_t *evdev = input_evdev(dev);
    return sysfs_emit(buf, "%s\n", evdev && evdev->input_dev ? evdev->input_dev->name : "");
}

/* Show the physical device path. */
static ssize_t phys_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    evdev_t *evdev = input_evdev(dev);
    return sysfs_emit(buf, "%s\n", evdev && evdev->input_dev ? evdev->input_dev->phys : "");
}

/* Show the unique device identifier. */
static ssize_t uniq_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    evdev_t *evdev = input_evdev(dev);
    return sysfs_emit(buf, "%s\n", evdev && evdev->input_dev ? evdev->input_dev->uniq : "");
}

/* Show the bus type. */
static ssize_t bustype_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    evdev_t *evdev = input_evdev(dev);
    return sysfs_emit(buf, "%04x\n", evdev && evdev->input_dev ? evdev->input_dev->id.bustype : 0);
}

/* Show the vendor id. */
static ssize_t vendor_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    evdev_t *evdev = input_evdev(dev);
    return sysfs_emit(buf, "%04x\n", evdev && evdev->input_dev ? evdev->input_dev->id.vendor : 0);
}

/* Show the product id. */
static ssize_t product_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    evdev_t *evdev = input_evdev(dev);
    return sysfs_emit(buf, "%04x\n", evdev && evdev->input_dev ? evdev->input_dev->id.product : 0);
}

/* Show the driver version. */
static ssize_t version_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    evdev_t *evdev = input_evdev(dev);
    return sysfs_emit(buf, "%04x\n", evdev && evdev->input_dev ? evdev->input_dev->id.version : 0);
}

/* Format a capability bitmap as space-separated hex words. */
static ssize_t bitmap_show(char *buf, const uint32_t *bits, size_t words)
{
    int at = 0;
    while (words > 1 && !bits[words - 1]) words--;
    for (size_t word = words; word > 0; word--) at += sysfs_emit_at(buf, at, word == 1 ? "%08x\n" : "%08x ", bits[word - 1]);
    return at;
}

/* Show the supported event types. */
static ssize_t ev_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return bitmap_show(buf, input_evdev(dev)->input_dev->evbit, (EV_CNT + 31) / 32);
}

/* Show the supported key codes. */
static ssize_t key_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return bitmap_show(buf, input_evdev(dev)->input_dev->keybit, (KEY_CNT + 31) / 32);
}

/* Show the supported relative axes. */
static ssize_t rel_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return bitmap_show(buf, input_evdev(dev)->input_dev->relbit, (REL_CNT + 31) / 32);
}

/* Show the supported absolute axes. */
static ssize_t abs_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return bitmap_show(buf, input_evdev(dev)->input_dev->absbit, (ABS_CNT + 31) / 32);
}

/* Show the supported miscellaneous events. */
static ssize_t msc_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return bitmap_show(buf, input_evdev(dev)->input_dev->mscbit, (MSC_CNT + 31) / 32);
}

/* Show the supported LEDs. */
static ssize_t led_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return bitmap_show(buf, input_evdev(dev)->input_dev->ledbit, (LED_CNT + 31) / 32);
}

/* Show the supported sound events. */
static ssize_t snd_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return bitmap_show(buf, input_evdev(dev)->input_dev->sndbit, (SND_CNT + 31) / 32);
}

/* Show the supported switch events. */
static ssize_t sw_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return bitmap_show(buf, input_evdev(dev)->input_dev->swbit, (SW_CNT + 31) / 32);
}

/* Show the supported force-feedback effects. */
static ssize_t ff_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return bitmap_show(buf, input_evdev(dev)->input_dev->ffbit, (FF_CNT + 31) / 32);
}

/* Show the input property bitmap. */
static ssize_t properties_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    return bitmap_show(buf, input_evdev(dev)->input_dev->propbit, (INPUT_PROP_CNT + 31) / 32);
}

static DEVICE_ATTR(name, 0444, name_show, NULL);
static DEVICE_ATTR(phys, 0444, phys_show, NULL);
static DEVICE_ATTR(uniq, 0444, uniq_show, NULL);
static DEVICE_ATTR(bustype, 0444, bustype_show, NULL);
static DEVICE_ATTR(vendor, 0444, vendor_show, NULL);
static DEVICE_ATTR(product, 0444, product_show, NULL);
static DEVICE_ATTR(version, 0444, version_show, NULL);
static DEVICE_ATTR(ev, 0444, ev_show, NULL);
static DEVICE_ATTR(key, 0444, key_show, NULL);
static DEVICE_ATTR(rel, 0444, rel_show, NULL);
static DEVICE_ATTR(abs, 0444, abs_show, NULL);
static DEVICE_ATTR(msc, 0444, msc_show, NULL);
static DEVICE_ATTR(led, 0444, led_show, NULL);
static DEVICE_ATTR(snd, 0444, snd_show, NULL);
static DEVICE_ATTR(sw, 0444, sw_show, NULL);
static DEVICE_ATTR(ff, 0444, ff_show, NULL);
static DEVICE_ATTR(properties, 0444, properties_show, NULL);

/* Keep the flat files for compatibility with early Uinxed user space. */
static struct attribute *input_compat_attributes[] = {
    &dev_attr_name.attr,    &dev_attr_phys.attr, &dev_attr_uniq.attr, &dev_attr_bustype.attr, &dev_attr_vendor.attr,     &dev_attr_product.attr,
    &dev_attr_version.attr, &dev_attr_ev.attr,   &dev_attr_key.attr,  &dev_attr_rel.attr,     &dev_attr_properties.attr, NULL,
};

static struct attribute_group input_compat_group = {
    .attrs = input_compat_attributes,
};

static struct attribute *input_id_attributes[] = {
    &dev_attr_bustype.attr, &dev_attr_vendor.attr, &dev_attr_product.attr, &dev_attr_version.attr, NULL,
};

static struct attribute_group input_id_group = {
    .name  = "id",
    .attrs = input_id_attributes,
};

static struct attribute *input_capability_attributes[] = {
    &dev_attr_ev.attr, &dev_attr_key.attr, &dev_attr_rel.attr, &dev_attr_abs.attr, &dev_attr_msc.attr, &dev_attr_led.attr, &dev_attr_snd.attr, &dev_attr_sw.attr, &dev_attr_ff.attr, NULL,
};

static struct attribute_group input_capability_group = {
    .name  = "capabilities",
    .attrs = input_capability_attributes,
};

/* Emit input device uevent environment variables. */
static int input_device_uevent(struct device *device, struct kobj_uevent_env *env)
{
    evdev_t *evdev = input_evdev(device);
    if (!evdev || !evdev->input_dev) return -ENODEV;
    input_dev_t *input = evdev->input_dev;
    int          ret   = add_uevent_var(env, "PRODUCT=%x/%x/%x/%x", input->id.bustype, input->id.vendor, input->id.product, input->id.version);
    if (ret) return ret;
    /*
     * libinput consumes these standard udev properties.  The generic
     * input_id builtin cannot infer them reliably from this kernel's compact
     * sysfs capability files, so publish the authoritative device classes at
     * the source uevent just like Linux input drivers do.
     */
    ret = add_uevent_var(env, "ID_INPUT=1");
    if (ret) return ret;
    bool keyboard = input_is_keyboard(input);
    bool mouse    = input_is_mouse(input);
    if (keyboard) {
        ret = add_uevent_var(env, "ID_INPUT_KEYBOARD=1");
        if (ret) return ret;
    } else if (input_test_bit(EV_KEY, input->evbit) && !mouse) {
        ret = add_uevent_var(env, "ID_INPUT_KEY=1");
        if (ret) return ret;
    }
    if (mouse) {
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
    &input_compat_group,
    &input_id_group,
    &input_capability_group,
    NULL,
};

static struct class input_class = {.name = "input", .dev_uevent = input_device_uevent, .dev_groups = input_dev_groups};

/* Register the input class and publish every evdev device. */
void input_sysfs_init(void)
{
#if CONFIG_INPUT_EVDEV
    int result;
    int devices = 0;

    if (input_class_ready) return;
    result = class_register(&input_class);
    if (result != EOK) {
        plogk("input_sysfs: Class_register(input) failed: %d\n", result);
        return;
    }
    input_class_ready = true;
    for (int minor = 0; minor < EVDEV_MAX_DEVICES; minor++) {
        evdev_t *evdev = evdev_find_by_minor(minor);
        if (!evdev) continue;
        if (input_sysfs_register_evdev(evdev) == EOK) devices++;
    }
    plogk("input_sysfs: exported %d input device(s) to /sys/class/input\n", devices);
#endif
}

/* Publish an evdev device as inputN with an eventN child. */
int input_sysfs_register_evdev(evdev_t *evdev)
{
    char           input_name[24];
    char           event_name[24];
    struct device *input_device;
    struct device *device;

    if (!evdev) return -EINVAL;
    if (evdev->sysfs_device) return EOK;
    if (!input_class_ready) return EOK;
    /*
     * Linux exposes an inputN device containing the identity/capability
     * files and an eventN child containing the character-device number.
     * libudev resolves /sys/dev/char/13:* back to eventN, while input_id
     * walks through eventN/device to inputN.  Reproducing that topology is
     * required by libinput's syspath safety check.
     */
    (void)snprintf(input_name, sizeof(input_name), "input%d", evdev->minor);
    input_device = device_create(&input_class, NULL, 0, evdev, "%s", input_name);
    if (!input_device) return -ENOMEM;

    (void)snprintf(event_name, sizeof(event_name), "event%d", evdev->minor);
    device = device_create(&input_class, input_device, evdev_devt(evdev), evdev, "%s", event_name);
    if (!device) {
        device_unregister(input_device);
        return -ENOMEM;
    }
    evdev->sysfs_input_device = input_device;
    evdev->sysfs_device       = device;
    return EOK;
}

/* Remove an evdev device's sysfs devices. */
void input_sysfs_unregister_evdev(evdev_t *evdev)
{
    struct device *device;

    if (!evdev) return;
    device = evdev->sysfs_device;
    if (device) device_unregister(device);
    evdev->sysfs_device = NULL;
    device              = evdev->sysfs_input_device;
    if (device) device_unregister(device);
    evdev->sysfs_input_device = NULL;
}
