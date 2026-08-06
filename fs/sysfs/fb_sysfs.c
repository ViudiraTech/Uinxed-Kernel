/*
 *
 *      fb_sysfs.c
 *      Framebuffer sysfs integration.
 *
 *      2026/8/2 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/core/device.h>
#include <drivers/gpu/video.h>
#include <fs/sysfs/fb_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/string.h>

static struct bus_type framebuffer_platform_bus = {.name = "platform"};
static struct device   framebuffer_platform_device;
static struct device  *framebuffer_class_device;
static bool            framebuffer_sysfs_ready;

static ssize_t fb_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    return sysfs_emit(buf, "uinxed-virtio-gpu\n");
}

static ssize_t fb_stride_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    video_info_t info = video_get_info();
    return sysfs_emit(buf, "%llu\n", (unsigned long long)info.stride * (info.bpp / 8));
}

static ssize_t fb_bpp_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    return sysfs_emit(buf, "%u\n", video_get_info().bpp);
}

static ssize_t fb_virtual_size_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    video_info_t info = video_get_info();
    return sysfs_emit(buf, "%llu,%llu\n", (unsigned long long)info.width, (unsigned long long)info.height);
}

static ssize_t fb_modes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)dev;
    (void)attr;
    video_info_t info = video_get_info();
    return sysfs_emit(buf, "U:%llux%llu-0\n", (unsigned long long)info.width, (unsigned long long)info.height);
}

static DEVICE_ATTR(name, 0444, fb_name_show, NULL);
static DEVICE_ATTR(stride, 0444, fb_stride_show, NULL);
static DEVICE_ATTR(bits_per_pixel, 0444, fb_bpp_show, NULL);
static DEVICE_ATTR(virtual_size, 0444, fb_virtual_size_show, NULL);
static DEVICE_ATTR(modes, 0444, fb_modes_show, NULL);

static struct attribute *framebuffer_attributes[] = {
    &dev_attr_name.attr, &dev_attr_stride.attr, &dev_attr_bits_per_pixel.attr, &dev_attr_virtual_size.attr, &dev_attr_modes.attr, NULL,
};

static struct attribute_group framebuffer_group = {
    .attrs = framebuffer_attributes,
};

static const struct attribute_group *framebuffer_groups[] = {
    &framebuffer_group,
    NULL,
};

static struct class graphics_class = {.name = "graphics", .dev_groups = framebuffer_groups};

void fb_sysfs_init(void)
{
#if CONFIG_SYSFS
    int status;

    if (framebuffer_sysfs_ready) return;
    status = bus_register(&framebuffer_platform_bus);
    if (status != EOK) {
        plogk("fb_sysfs: platform bus registration failed: %d\n", status);
        return;
    }
    status = class_register(&graphics_class);
    if (status != EOK) {
        plogk("fb_sysfs: graphics class registration failed: %d\n", status);
        bus_unregister(&framebuffer_platform_bus);
        return;
    }

    memset(&framebuffer_platform_device, 0, sizeof(framebuffer_platform_device));
    framebuffer_platform_device.bus   = &framebuffer_platform_bus;
    framebuffer_platform_device.devid = 0;
    if (kobject_set_name(&framebuffer_platform_device.kobj, "virtio-framebuffer") != EOK
        || device_register(&framebuffer_platform_device) != EOK) {
        plogk("fb_sysfs: physical framebuffer registration failed.\n");
        class_unregister(&graphics_class);
        bus_unregister(&framebuffer_platform_bus);
        return;
    }

    framebuffer_class_device = device_create(&graphics_class, &framebuffer_platform_device, MKDEV(29, 0), NULL, "fb0");
    if (!framebuffer_class_device) {
        plogk("fb_sysfs: fb0 class device registration failed.\n");
        device_unregister(&framebuffer_platform_device);
        class_unregister(&graphics_class);
        bus_unregister(&framebuffer_platform_bus);
        return;
    }
    framebuffer_sysfs_ready = true;
    plogk("fb_sysfs: /sys/class/graphics/fb0 registered on the platform bus.\n");
#endif
}
