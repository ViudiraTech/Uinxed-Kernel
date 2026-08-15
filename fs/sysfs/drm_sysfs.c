/*
 *
 *      drm_sysfs.c
 *      DRM class sysfs integration (/sys/class/drm/)
 *
 *      2026/8/15 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_mode.h>
#include <fs/sysfs/drm_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/kobject/kobject.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/heap.h>

/* DRM device class */

static int drm_device_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    (void)dev;
    /* DRM minor uevent contract. */
    return add_uevent_var(env, "DEVTYPE=drm_minor");
}

static struct class drm_class = {
    .name       = "drm",
    .dev_uevent = drm_device_uevent,
};

/* Map a connector type to its name. */
static const char *drm_connector_type_name(uint32_t type)
{
    switch (type) {
        case DRM_MODE_CONNECTOR_VGA :
            return "VGA";
        case DRM_MODE_CONNECTOR_DVII :
            return "DVI-I";
        case DRM_MODE_CONNECTOR_DVID :
            return "DVI-D";
        case DRM_MODE_CONNECTOR_DVIA :
            return "DVI-A";
        case DRM_MODE_CONNECTOR_Composite :
            return "Composite";
        case DRM_MODE_CONNECTOR_SVIDEO :
            return "SVIDEO";
        case DRM_MODE_CONNECTOR_LVDS :
            return "LVDS";
        case DRM_MODE_CONNECTOR_Component :
            return "Component";
        case DRM_MODE_CONNECTOR_9PinDIN :
            return "DIN";
        case DRM_MODE_CONNECTOR_DisplayPort :
            return "DP";
        case DRM_MODE_CONNECTOR_HDMIA :
            return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB :
            return "HDMI-B";
        case DRM_MODE_CONNECTOR_TV :
            return "TV";
        case DRM_MODE_CONNECTOR_eDP :
            return "eDP";
        case DRM_MODE_CONNECTOR_VIRTUAL :
            return "Virtual";
        case DRM_MODE_CONNECTOR_DSI :
            return "DSI";
        case DRM_MODE_CONNECTOR_DPI :
            return "DPI";
        case DRM_MODE_CONNECTOR_WRITEBACK :
            return "Writeback";
        case DRM_MODE_CONNECTOR_SPI :
            return "SPI";
        case DRM_MODE_CONNECTOR_USB :
            return "USB";
        default :
            return "Unknown";
    }
}

/* Connector attributes */

/* Read the connector's connection status. */
static ssize_t connector_status_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct drm_connector *connector = dev->driver_data;
    const char           *status    = "unknown";
    (void)attr;
    if (connector) {
        if (connector->status == connector_status_connected)
            status = "connected";
        else if (connector->status == connector_status_disconnected)
            status = "disconnected";
    }
    return sysfs_emit(buf, "%s\n", status);
}

/* List the connector's probed modes. */
static ssize_t connector_modes_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct drm_connector *connector = dev->driver_data;
    ilist_node_t         *node;
    int                   at = 0;
    (void)attr;
    if (!connector) return sysfs_emit(buf, "\n");
    for (node = connector->modes.next; node && node != &connector->modes; node = node->next) {
        struct drm_display_mode *mode = container_of(node, struct drm_display_mode, head);
        at += sysfs_emit_at(buf, at, "%s\n", mode->name);
    }
    return at;
}

/* Report whether the connector is enabled. */
static ssize_t connector_enabled_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct drm_connector *connector = dev->driver_data;
    const char           *enabled   = (connector && connector->state && connector->state->crtc) ? "enabled" : "disabled";
    (void)attr;
    return sysfs_emit(buf, "%s\n", enabled);
}

/* Read the connector's raw EDID. */
static ssize_t connector_edid_read(struct kobject *kobj, struct bin_attribute *attr, char *buf, int64_t pos, size_t count)
{
    struct device        *dev       = container_of(kobj, struct device, kobj);
    struct drm_connector *connector = dev->driver_data;
    size_t                length;
    (void)attr;
    if (!connector || !connector->edid_blob || !connector->edid_blob->data) return 0;
    length = connector->edid_blob->length;
    if (pos < 0 || (uint64_t)pos >= length) return 0;
    if ((uint64_t)pos + count > length) count = length - (uint64_t)pos;
    memcpy(buf, (const char *)connector->edid_blob->data + pos, count);
    return (ssize_t)count;
}

static DEVICE_ATTR(status, 0444, connector_status_show, NULL);
static DEVICE_ATTR(modes, 0444, connector_modes_show, NULL);
static DEVICE_ATTR(enabled, 0444, connector_enabled_show, NULL);

static struct bin_attribute connector_edid_attr = {
    .attr = __ATTR(edid, 0444),
    .read = connector_edid_read,
};

/* Initialization */

/* Register the DRM device class once at sysfs initialization. */
void drm_sysfs_init(void)
{
#if CONFIG_SYSFS
    int ret = class_register(&drm_class);
    if (ret != EOK) {
        plogk("drm_sysfs: Class_register(drm) failed: %d\n", ret);
        return;
    }
    plogk("drm_sysfs: /sys/class/drm registered.\n");
#endif
}

/* Publish one GPU under /sys/class/drm/. */
void drm_sysfs_register_device(struct drm_device *dev)
{
#if CONFIG_SYSFS
    if (!dev || !dev->primary) return;
    if (!device_create(&drm_class, NULL, MKDEV(226, dev->primary->index), dev, "card%d", dev->primary->index)) plogk("drm_sysfs: Failed to create /sys/class/drm/card%d\n", dev->primary->index);
#endif
}

/* Publish one connector under /sys/class/drm/ with status/modes/edid. */
void drm_sysfs_connector_add(struct drm_connector *connector)
{
#if CONFIG_SYSFS
    char name[48];
    if (!connector || !connector->dev || !connector->dev->primary) return;

    (void)snprintf(name, sizeof(name), "card%u-%s-%u", connector->dev->primary->index, drm_connector_type_name(connector->connector_type), connector->connector_type_id);
    struct device *cdev = device_create(&drm_class, NULL, 0, connector, "%s", name);
    if (!cdev) {
        plogk("drm_sysfs: Failed to create /sys/class/drm/%s\n", name);
        return;
    }

    (void)device_create_file(cdev, &dev_attr_status);
    (void)device_create_file(cdev, &dev_attr_enabled);
    (void)device_create_file(cdev, &dev_attr_modes);
    (void)sysfs_create_bin_file(&cdev->kobj, &connector_edid_attr);
#endif
}
