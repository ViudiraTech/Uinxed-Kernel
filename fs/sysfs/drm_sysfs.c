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

/* Emit the DRM uevent environment variables. */
static int drm_device_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    /*
     * Linux drm_sysfs.c: minor class devices emit "DEVTYPE=drm_minor",
     * connector class devices (devt == 0) emit "DEVTYPE=drm_connector".
     */
    return add_uevent_var(env, dev && dev->devt ? "DEVTYPE=drm_minor" : "DEVTYPE=drm_connector");
}

static struct class drm_class = {
    .name       = "drm",
    .dev_uevent = drm_device_uevent,
};

/* Class attribute: /sys/class/drm/version */

/* : CLASS_ATTR_STRING(version, 0444, "drm 1.1.0 20060810"). */
static ssize_t drm_version_show(struct class *cls, struct class_attribute *attr, char *buf)
{
    (void)cls;
    (void)attr;
    return sysfs_emit(buf, "drm 1.1.0 20060810\n");
}

static CLASS_ATTR(version, 0444, drm_version_show, NULL);

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

/* Match a write buffer against an expected token (newline / NUL terminated). */
static bool drm_sysfs_match(const char *buf, size_t count, const char *token)
{
    size_t token_len;

    if (!buf || !token) return false;

    /* Strip a trailing newline (optionally preceded by CR) and stray whitespace. */
    while (count && (buf[count - 1] == '\n' || buf[count - 1] == '\r' || buf[count - 1] == ' ' || buf[count - 1] == '\t')) count--;

    token_len = strlen(token);
    return count == token_len && memcmp(buf, token, token_len) == 0;
}

/* Force the connector state from sysfs: "detect" clears the override, "on"/"on-digital"/"off" force the status, else -EINVAL. */
static ssize_t connector_status_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    struct drm_connector    *connector = dev->driver_data;
    enum drm_connector_force new_force;
    (void)attr;

    if (!connector) {
        plogk("drm_sysfs: status_store with NULL connector.\n");
        return -EINVAL;
    }

    if (drm_sysfs_match(buf, count, "detect"))
        new_force = DRM_FORCE_UNSPECIFIED;
    else if (drm_sysfs_match(buf, count, "on"))
        new_force = DRM_FORCE_ON;
    else if (drm_sysfs_match(buf, count, "on-digital"))
        new_force = DRM_FORCE_ON_DIGITAL;
    else if (drm_sysfs_match(buf, count, "off"))
        new_force = DRM_FORCE_OFF;
    else {
        plogk("drm_sysfs: invalid status value written.\n");
        return -EINVAL;
    }

    connector->force = new_force;

    /*
     * Apply the force contract immediately so a subsequent read of "status"
     * reflects the request: forced on/off pins the reported status, while
     * "detect" re-probes through the connector helper when one exists
     * (mirrors Linux drm_sysfs.c status_store).
     */
    if (new_force == DRM_FORCE_ON || new_force == DRM_FORCE_ON_DIGITAL) {
        connector->status = connector_status_connected;
    } else if (new_force == DRM_FORCE_OFF) {
        connector->status = connector_status_disconnected;
    } else {
        struct drm_connector_helper_funcs *funcs = (struct drm_connector_helper_funcs *)connector->helper_private;
        if (funcs && funcs->detect) connector->status = funcs->detect(connector, true);
    }

    plogk("drm_sysfs: [CONNECTOR:%d:%s] force=%d status=%d\n", connector->base.id, connector->name, connector->force, connector->status);

    return (ssize_t)count;
}

/* Read the connector's current DPMS level ("On"/"Standby"/"Suspend"/"Off"). */
static ssize_t connector_dpms_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct drm_connector *connector = dev->driver_data;
    (void)attr;
    if (!connector) return sysfs_emit(buf, "Unknown\n");
    return sysfs_emit(buf, "%s\n", drm_get_dpms_name(drm_connector_dpms_get(connector)));
}

/* Read the connector's stable mode-object ID. */
static ssize_t connector_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct drm_connector *connector = dev->driver_data;
    (void)attr;
    if (!connector) return sysfs_emit(buf, "0\n");
    return sysfs_emit(buf, "%d\n", connector->base.id);
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

static DEVICE_ATTR(status, 0644, connector_status_show, connector_status_store);
static DEVICE_ATTR(modes, 0444, connector_modes_show, NULL);
static DEVICE_ATTR(enabled, 0444, connector_enabled_show, NULL);
static DEVICE_ATTR(dpms, 0444, connector_dpms_show, NULL);
static DEVICE_ATTR(connector_id, 0444, connector_id_show, NULL);

static struct bin_attribute connector_edid_attr = {
    .attr = __ATTR(edid, 0444),
    .read = connector_edid_read,
};

/* Register the DRM device class once at sysfs initialization. */
void drm_sysfs_init(void)
{
#if CONFIG_SYSFS
    int ret = class_register(&drm_class);
    if (ret != EOK) {
        plogk("drm_sysfs: Class_register(drm) failed: %d\n", ret);
        return;
    }
    ret = class_create_file(&drm_class, &class_attr_version);
    if (ret != EOK) plogk("drm_sysfs: class_create_file(version) failed: %d\n", ret);
    plogk("drm_sysfs: registered /sys/class/drm\n");
#endif
}

/* Publish one GPU under /sys/class/drm/. */
void drm_sysfs_register_device(struct drm_device *dev)
{
#if CONFIG_SYSFS
    if (!dev || !dev->primary) return;
    if (!device_create(&drm_class, NULL, MKDEV(DRM_MAJOR, dev->primary->index), dev, "card%d", dev->primary->index)) plogk("drm_sysfs: Failed to create /sys/class/drm/card%d\n", dev->primary->index);
#endif
}

/* Publish the render node under /sys/class/drm/ (renderD128+N). */
void drm_sysfs_register_render_device(struct drm_device *dev)
{
#if CONFIG_SYSFS
    if (!dev || !dev->render || !dev->driver || !(dev->driver->driver_features & DRIVER_RENDER)) return;
    if (!device_create(&drm_class, NULL, MKDEV(DRM_MAJOR, 128 + dev->render->index), dev, "renderD%d", 128 + dev->render->index)) {
        plogk("drm_sysfs: Failed to create /sys/class/drm/renderD%d\n", 128 + dev->render->index);
    }
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
    connector->kdev = cdev;

    (void)device_create_file(cdev, &dev_attr_status);
    (void)device_create_file(cdev, &dev_attr_enabled);
    (void)device_create_file(cdev, &dev_attr_dpms);
    (void)device_create_file(cdev, &dev_attr_modes);
    (void)device_create_file(cdev, &dev_attr_connector_id);
    (void)sysfs_create_bin_file(&cdev->kobj, &connector_edid_attr);
#endif
}

/* Remove a connector's /sys/class/drm/ device (during connector cleanup). */
void drm_sysfs_connector_remove(struct drm_connector *connector)
{
#if CONFIG_SYSFS
    if (!connector || !connector->kdev) return;
    device_unregister(connector->kdev);
    connector->kdev = NULL;
#endif
}
