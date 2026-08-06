/*
 *
 *      device.c
 *      Device model implementation (bus, device, driver, class)
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/core/device.h>
#include <fs/core/vfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/kobject.h>
#include <kernel/printk.h>
#include <libs/glist/circular_list.h>
#include <libs/std/stdarg.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <proc/process.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/*  Global sysfs root kobjects (initialised in sysfs_init)             */
/* ------------------------------------------------------------------ */

static struct kobject *devices_kobj; /* /sys/devices */
static struct kobject *bus_kobj;     /* /sys/bus */
static struct kobject *class_kobj;   /* /sys/class */

/* Forward declarations */
static struct kobject *sysfs_find_child_kobj(struct kobject *parent, const char *name);
static struct kobject *get_devices_kobj(void);
static struct kobject *get_bus_kobj(void);
static struct kobject *get_class_kobj(void);

/* ------------------------------------------------------------------ */
/*  Device attribute sysfs_ops                                         */
/* ------------------------------------------------------------------ */

static ssize_t dev_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    struct device           *dev   = (struct device *)((char *)kobj - offsetof(struct device, kobj));
    struct device_attribute *dattr = (struct device_attribute *)((char *)attr - offsetof(struct device_attribute, attr));

    if (!dattr->show) return -EIO;
    return dattr->show(dev, dattr, buf);
}

static ssize_t dev_attr_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)
{
    struct device           *dev   = (struct device *)((char *)kobj - offsetof(struct device, kobj));
    struct device_attribute *dattr = (struct device_attribute *)((char *)attr - offsetof(struct device_attribute, attr));

    if (!dattr->store) return -EIO;
    return dattr->store(dev, dattr, buf, count);
}

static const struct sysfs_ops dev_sysfs_ops = {
    .show  = dev_attr_show,
    .store = dev_attr_store,
};

/* ------------------------------------------------------------------ */
/*  Bus attribute sysfs_ops                                            */
/* ------------------------------------------------------------------ */

static ssize_t bus_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    struct bus_type      *bus   = (struct bus_type *)((char *)kobj - offsetof(struct bus_type, subsys.kobj));
    struct bus_attribute *battr = (struct bus_attribute *)((char *)attr - offsetof(struct bus_attribute, attr));

    if (!battr->show) return -EIO;
    return battr->show(bus, battr, buf);
}

static ssize_t bus_attr_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)
{
    struct bus_type      *bus   = (struct bus_type *)((char *)kobj - offsetof(struct bus_type, subsys.kobj));
    struct bus_attribute *battr = (struct bus_attribute *)((char *)attr - offsetof(struct bus_attribute, attr));

    if (!battr->store) return -EIO;
    return battr->store(bus, battr, buf, count);
}

static const struct sysfs_ops bus_sysfs_ops = {
    .show  = bus_attr_show,
    .store = bus_attr_store,
};

/* ------------------------------------------------------------------ */
/*  Driver attribute sysfs_ops                                         */
/* ------------------------------------------------------------------ */

static ssize_t drv_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    struct device_driver    *drv   = (struct device_driver *)((char *)kobj - offsetof(struct device_driver, kobj));
    struct driver_attribute *dattr = (struct driver_attribute *)((char *)attr - offsetof(struct driver_attribute, attr));

    if (!dattr->show) return -EIO;
    return dattr->show(drv, dattr, buf);
}

static ssize_t drv_attr_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)
{
    struct device_driver    *drv   = (struct device_driver *)((char *)kobj - offsetof(struct device_driver, kobj));
    struct driver_attribute *dattr = (struct driver_attribute *)((char *)attr - offsetof(struct driver_attribute, attr));

    if (!dattr->store) return -EIO;
    return dattr->store(drv, dattr, buf, count);
}

static const struct sysfs_ops drv_sysfs_ops = {
    .show  = drv_attr_show,
    .store = drv_attr_store,
};

/* ------------------------------------------------------------------ */
/*  Class attribute sysfs_ops                                          */
/* ------------------------------------------------------------------ */

static ssize_t class_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    struct class           *cls   = (struct class *)((char *)kobj - offsetof(struct class, subsys.kobj));
    struct class_attribute *cattr = (struct class_attribute *)((char *)attr - offsetof(struct class_attribute, attr));

    if (!cattr->show) return -EIO;
    return cattr->show(cls, cattr, buf);
}

static ssize_t class_attr_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)
{
    struct class           *cls   = (struct class *)((char *)kobj - offsetof(struct class, subsys.kobj));
    struct class_attribute *cattr = (struct class_attribute *)((char *)attr - offsetof(struct class_attribute, attr));

    if (!cattr->store) return -EIO;
    return cattr->store(cls, cattr, buf, count);
}

static const struct sysfs_ops class_sysfs_ops = {
    .show  = class_attr_show,
    .store = class_attr_store,
};

/* ------------------------------------------------------------------ */
/*  Default kobj_type for device kobjects                              */
/* ------------------------------------------------------------------ */

static void device_release_internal(struct kobject *kobj)
{
    struct device *dev = (struct device *)((char *)kobj - offsetof(struct device, kobj));
    if (dev->release) dev->release(dev);
}

static void device_create_release(struct device *dev)
{
    free(dev);
}

static const char *device_uevent_name(struct kobject *kobj)
{
    struct device *dev = (struct device *)((char *)kobj - offsetof(struct device, kobj));
    if (dev->bus && dev->bus->name) return dev->bus->name;
    if (dev->class && dev->class->name) return dev->class->name;
    return "devices";
}

static const char *device_devnode(struct device *dev, char *buffer, size_t capacity)
{
    const char *name = dev_name(dev);
    if (dev->devnode && dev->devnode[0]) return dev->devnode;
    if (!dev->class || !dev->class->name) return name;

    if (streq(dev->class->name, "input")) {
        (void)snprintf(buffer, capacity, "input/%s", name);
        return buffer;
    }
    if (streq(dev->class->name, "drm")) {
        (void)snprintf(buffer, capacity, "dri/%s", name);
        return buffer;
    }
    return name;
}

static int device_build_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    char devnode_buffer[128];
    int  ret;

    if (dev->devt) {
        ret = add_uevent_var(env, "MAJOR=%u", MAJOR(dev->devt));
        if (ret) return ret;
        ret = add_uevent_var(env, "MINOR=%u", MINOR(dev->devt));
        if (ret) return ret;
        const char *devnode = device_devnode(dev, devnode_buffer, sizeof(devnode_buffer));
        if (!devnode || !devnode[0] || strlen(devnode) >= sizeof(devnode_buffer)) return -EINVAL;
        ret = add_uevent_var(env, "DEVNAME=%s", devnode);
        if (ret) return ret;
    }
    if (dev->driver && dev->driver->name) {
        ret = add_uevent_var(env, "DRIVER=%s", dev->driver->name);
        if (ret) return ret;
    }
    if (dev->bus && dev->bus->uevent) {
        ret = dev->bus->uevent(dev, env);
        if (ret) return ret;
    }
    if (dev->class && dev->class->dev_uevent) {
        ret = dev->class->dev_uevent(dev, env);
        if (ret) return ret;
    }
    if (dev->uevent) return dev->uevent(dev, env);
    return EOK;
}

static int device_kobj_uevent(struct kobject *kobj, struct kobj_uevent_env *env)
{
    struct device *dev = (struct device *)((char *)kobj - offsetof(struct device, kobj));
    return device_build_uevent(dev, env);
}

static ssize_t device_uevent_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    struct kobj_uevent_env env = {0};
    int                    at  = 0;
    (void)attr;

    int ret = device_build_uevent(dev, &env);
    if (ret) return ret;
    for (int i = 0; i < env.envp_idx; i++) {
        int written = sysfs_emit_at(buf, at, "%s\n", env.envp[i]);
        if (written <= 0 || at > SYSFS_PAGE_SIZE - written) return -EOVERFLOW;
        at += written;
    }
    return at;
}

static ssize_t device_uevent_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
    process_t *process = process_current();
    (void)attr;
    if (!process || process->uid != 0) return -EPERM;
    int ret = kobject_synth_uevent(&dev->kobj, buf, count);
    return ret ? ret : (ssize_t)count;
}

static DEVICE_ATTR(uevent, 0644, device_uevent_show, device_uevent_store);

/* libudev (and therefore Weston's DRM backend) discovers a class device's
 * character node from the standard sysfs `dev` attribute.  Without this,
 * `/sys/class/drm/card0` exists but udev cannot resolve it to /dev/dri/card0
 * and Weston reports "no drm device found". */
static ssize_t device_dev_show(struct device *dev, struct device_attribute *attr, char *buf)
{
    (void)attr;
    if (!dev || !dev->devt) return 0;
    return snprintf(buf, SYSFS_PAGE_SIZE, "%u:%u\n", MAJOR(dev->devt), MINOR(dev->devt));
}

static DEVICE_ATTR(dev, 0444, device_dev_show, NULL);

static struct attribute *device_default_attrs[] = {
    &dev_attr_uevent.attr,
    &dev_attr_dev.attr,
    NULL,
};

static struct kobj_type device_ktype = {
    .release       = device_release_internal,
    .sysfs_ops     = &dev_sysfs_ops,
    .default_attrs = device_default_attrs,
    .uevent_name   = device_uevent_name,
    .uevent        = device_kobj_uevent,
};

static void bus_release_internal(struct kobject *kobj)
{
    /* Bus types are typically static ?nothing to free */
    (void)kobj;
}

static const char *bus_uevent_name(struct kobject *kobj)
{
    struct bus_type *bus = (struct bus_type *)((char *)kobj - offsetof(struct bus_type, subsys.kobj));
    return bus->name ? bus->name : "bus";
}

static struct kobj_type bus_ktype = {
    .release       = bus_release_internal,
    .sysfs_ops     = &bus_sysfs_ops,
    .default_attrs = NULL,
    .uevent_name   = bus_uevent_name,
};

static void driver_release_internal(struct kobject *kobj)
{
    /* Drivers are typically static */
    (void)kobj;
}

static const char *driver_uevent_name(struct kobject *kobj)
{
    struct device_driver *driver = (struct device_driver *)((char *)kobj - offsetof(struct device_driver, kobj));
    return driver->bus && driver->bus->name ? driver->bus->name : "drivers";
}

static struct kobj_type driver_ktype = {
    .release       = driver_release_internal,
    .sysfs_ops     = &drv_sysfs_ops,
    .default_attrs = NULL,
    .uevent_name   = driver_uevent_name,
};

static void class_release_internal(struct kobject *kobj)
{
    (void)kobj;
}

static const char *class_uevent_name(struct kobject *kobj)
{
    struct class *cls = (struct class *)((char *)kobj - offsetof(struct class, subsys.kobj));
    return cls->name ? cls->name : "class";
}

static struct kobj_type class_ktype = {
    .release       = class_release_internal,
    .sysfs_ops     = &class_sysfs_ops,
    .default_attrs = NULL,
    .uevent_name   = class_uevent_name,
};

/* ------------------------------------------------------------------ */
/*  Lazy initialisation helpers                                        */
/* ------------------------------------------------------------------ */

static struct kobject *get_devices_kobj(void)
{
    if (!devices_kobj && sysfs_root_kobj) { devices_kobj = sysfs_find_child_kobj(sysfs_root_kobj, "devices"); }
    return devices_kobj;
}

static struct kobject *get_bus_kobj(void)
{
    if (!bus_kobj && sysfs_root_kobj) { bus_kobj = sysfs_find_child_kobj(sysfs_root_kobj, "bus"); }
    return bus_kobj;
}

static struct kobject *get_class_kobj(void)
{
    if (!class_kobj && sysfs_root_kobj) { class_kobj = sysfs_find_child_kobj(sysfs_root_kobj, "class"); }
    return class_kobj;
}

/* ------------------------------------------------------------------ */
/*  bus_register / bus_unregister                                      */
/* ------------------------------------------------------------------ */

int bus_register(struct bus_type *bus)
{
    int             ret;
    struct kobject *parent;

    if (!bus || !bus->name) return -EINVAL;

    parent = get_bus_kobj();
    if (!parent) return -ENOENT;

    kset_init(&bus->subsys);
    bus->subsys.kobj.ktype = &bus_ktype;

    ret = kobject_add(&bus->subsys.kobj, parent, "%s", bus->name);
    if (ret != EOK) return ret;

    /* Create devices kset */
    bus->devices_kset = kset_create_and_add("devices", NULL, &bus->subsys.kobj);
    if (!bus->devices_kset) {
        kobject_del(&bus->subsys.kobj);
        kobject_put(&bus->subsys.kobj);
        return -ENOMEM;
    }

    /* Create drivers kset */
    bus->drivers_kset = kset_create_and_add("drivers", NULL, &bus->subsys.kobj);
    if (!bus->drivers_kset) {
        kset_unregister(bus->devices_kset);
        bus->devices_kset = NULL;
        kobject_del(&bus->subsys.kobj);
        kobject_put(&bus->subsys.kobj);
        return -ENOMEM;
    }

    kobject_uevent(&bus->subsys.kobj, KOBJ_ADD);
    return EOK;
}

void bus_unregister(struct bus_type *bus)
{
    if (!bus) return;

    if (bus->subsys.kobj.state_add_uevent_sent && !bus->subsys.kobj.state_remove_uevent_sent)
        (void)kobject_uevent(&bus->subsys.kobj, KOBJ_REMOVE);

    if (bus->drivers_kset) kset_unregister(bus->drivers_kset);
    if (bus->devices_kset) kset_unregister(bus->devices_kset);
    bus->drivers_kset = NULL;
    bus->devices_kset = NULL;
    kobject_del(&bus->subsys.kobj);
    kobject_put(&bus->subsys.kobj);
}

int bus_create_file(struct bus_type *bus, struct bus_attribute *attr)
{
    if (!bus || !attr) return -EINVAL;
    return sysfs_create_file(&bus->subsys.kobj, &attr->attr);
}

void bus_remove_file(struct bus_type *bus, struct bus_attribute *attr)
{
    if (!bus || !attr) return;
    sysfs_remove_file(&bus->subsys.kobj, &attr->attr);
}

/* ------------------------------------------------------------------ */
/*  device_register / device_unregister                                */
/* ------------------------------------------------------------------ */

int device_register(struct device *dev)
{
    int ret;

    if (!dev) return -EINVAL;

    /* Initialise the embedded kobject */
    dev->kobj.ktype = &device_ktype;
    kobject_init(&dev->kobj, &device_ktype);

    /* Set the parent kobject */
    struct kobject *parent = NULL;
    if (dev->parent) {
        parent = &dev->parent->kobj;
    } else {
        parent = get_devices_kobj();
    }

    if (!parent) return -ENOENT;

    /* An explicit device name (for example a PCI BDF) is authoritative. */
    if (dev->kobj.name) {
        ret = kobject_add(&dev->kobj, parent, "%s", dev->kobj.name);
    } else if (dev->bus && dev->bus->dev_name) {
        ret = kobject_add(&dev->kobj, parent, "%s%llu", dev->bus->dev_name, dev->devid);
    } else {
        ret = kobject_add(&dev->kobj, parent, "device%llu", dev->devid);
    }

    if (ret != EOK) return ret;

    /* Publish every property group before the ADD event.  This ordering is
     * part of the userspace hotplug contract: udev may inspect attributes as
     * soon as it dequeues the event. */
    if (dev->bus && dev->bus->dev_groups) {
        ret = sysfs_create_groups(&dev->kobj, dev->bus->dev_groups);
        if (ret != EOK) {
            kobject_del(&dev->kobj);
            return ret;
        }
    }
    if (dev->class && dev->class->dev_groups) {
        ret = sysfs_create_groups(&dev->kobj, dev->class->dev_groups);
        if (ret != EOK) {
            plogk("device: class dev_groups failed for %s: %d\n", kobject_name(&dev->kobj), ret);
            goto rollback_bus_groups;
        }
    }
    if (dev->groups) {
        ret = sysfs_create_groups(&dev->kobj, dev->groups);
        if (ret != EOK) goto rollback_class_groups;
    }

    /* Linux-compatible topology links used by libudev and Xorg. */
    if (dev->bus) {
        ret = sysfs_create_symlink(&dev->kobj, &dev->bus->subsys.kobj, "subsystem");
        if (ret != EOK) goto rollback_groups;
        if (dev->bus->devices_kset) {
            ret = sysfs_create_symlink(&dev->bus->devices_kset->kobj, &dev->kobj, kobject_name(&dev->kobj));
            if (ret != EOK) goto rollback_subsystem;
        }
    } else if (dev->class) {
        ret = sysfs_create_symlink(&dev->kobj, &dev->class->subsys.kobj, "subsystem");
        if (ret != EOK) goto rollback_groups;
    }
    if (dev->class && dev->parent) {
        ret = sysfs_create_symlink(&dev->kobj, &dev->parent->kobj, "device");
        if (ret != EOK) goto rollback_bus_device;
    }

    /* Add to bus's device kset if the device has a bus */
    if (dev->bus && dev->bus->devices_kset) {
        spin_lock(&dev->bus->devices_kset->list_lock);
        dev->bus->devices_kset->list = clist_append(dev->bus->devices_kset->list, &dev->kobj);
        spin_unlock(&dev->bus->devices_kset->list_lock);

        /* Try to bind a driver */
        if (dev->bus->match) {
            /* Full driver matching requires iterating drivers_kset;
             * for now, probe using bus->probe if set */
            if (dev->bus->probe) { dev->bus->probe(dev); }
        }
    }

    /* Create symlinks if the device has a class */
    if (dev->class) {
        ret = sysfs_create_symlink(&dev->class->subsys.kobj, &dev->kobj, kobject_name(&dev->kobj));
        if (ret != EOK) { goto rollback_device_link; }
        /* /sys/class/<name>/<device>  ?/sys/devices/.../device */
        /* For now just add to the class kset */
    }

    /* /sys/dev/char/<major>:<minor> ?/sys/devices/...  (udev device-node map) */
    if (dev->devt && sysfs_dev_char_kobj) {
        char dev_link[24];
        (void)snprintf(dev_link, sizeof(dev_link), "%u:%u", MAJOR(dev->devt), MINOR(dev->devt));
        (void)sysfs_create_symlink(sysfs_dev_char_kobj, &dev->kobj, dev_link);
    }

    kobject_uevent(&dev->kobj, KOBJ_ADD);
    return EOK;

rollback_device_link:
    if (dev->class && dev->parent) sysfs_remove_symlink(&dev->kobj, "device");
rollback_bus_device:
    if (dev->bus && dev->bus->devices_kset) sysfs_remove_symlink(&dev->bus->devices_kset->kobj, kobject_name(&dev->kobj));
rollback_subsystem:
    if (dev->bus || dev->class) sysfs_remove_symlink(&dev->kobj, "subsystem");
rollback_groups:
    if (dev->groups) sysfs_remove_groups(&dev->kobj, dev->groups);
rollback_class_groups:
    if (dev->class && dev->class->dev_groups) sysfs_remove_groups(&dev->kobj, dev->class->dev_groups);
rollback_bus_groups:
    if (dev->bus && dev->bus->dev_groups) sysfs_remove_groups(&dev->kobj, dev->bus->dev_groups);
    kobject_del(&dev->kobj);
    return ret;
}

void device_unregister(struct device *dev)
{
    if (!dev) return;

    if (dev->kobj.state_add_uevent_sent && !dev->kobj.state_remove_uevent_sent) (void)kobject_uevent(&dev->kobj, KOBJ_REMOVE);

    if (dev->devt && sysfs_dev_char_kobj) {
        char dev_link[24];
        (void)snprintf(dev_link, sizeof(dev_link), "%u:%u", MAJOR(dev->devt), MINOR(dev->devt));
        sysfs_remove_symlink(sysfs_dev_char_kobj, dev_link);
    }
    if (dev->class) sysfs_remove_symlink(&dev->class->subsys.kobj, kobject_name(&dev->kobj));
    if (dev->class && dev->parent) sysfs_remove_symlink(&dev->kobj, "device");
    if (dev->bus && dev->bus->devices_kset) sysfs_remove_symlink(&dev->bus->devices_kset->kobj, kobject_name(&dev->kobj));
    if (dev->bus || dev->class) sysfs_remove_symlink(&dev->kobj, "subsystem");

    /* Remove groups */
    if (dev->groups) sysfs_remove_groups(&dev->kobj, dev->groups);
    if (dev->class && dev->class->dev_groups) sysfs_remove_groups(&dev->kobj, dev->class->dev_groups);
    if (dev->bus && dev->bus->dev_groups) sysfs_remove_groups(&dev->kobj, dev->bus->dev_groups);

    /* Remove from bus device kset */
    if (dev->bus && dev->bus->devices_kset) {
        spin_lock(&dev->bus->devices_kset->list_lock);
        dev->bus->devices_kset->list = clist_delete(dev->bus->devices_kset->list, &dev->kobj);
        spin_unlock(&dev->bus->devices_kset->list_lock);

        /* Call bus remove if a driver is bound */
        if (dev->driver && dev->driver->remove) { dev->driver->remove(dev); }
        dev->driver = NULL;
    }

    kobject_del(&dev->kobj);
    kobject_put(&dev->kobj);
}

struct device *device_create(struct class *cls, struct device *parent, dev_t devt, void *drvdata, const char *fmt, ...)
{
    struct device *dev;
    va_list        args;
    char           namebuf[64];
    int            length;
    int            ret;

    if (!cls || !fmt) return NULL;

    va_start(args, fmt);
    length = vsnprintf(namebuf, sizeof(namebuf), fmt, args);
    va_end(args);
    if (length < 0 || length >= (int)sizeof(namebuf)) return NULL;

    dev = calloc(1, sizeof(struct device));
    if (!dev) return NULL;

    dev->class       = cls;
    dev->parent      = parent;
    dev->devt        = devt;
    dev->driver_data = drvdata;
    dev->release     = cls->dev_release ? cls->dev_release : device_create_release;

    ret = kobject_set_name(&dev->kobj, "%s", namebuf);
    if (ret != EOK) {
        free(dev);
        return NULL;
    }

    ret = device_register(dev);
    if (ret != EOK) {
        kobject_put(&dev->kobj);
        return NULL;
    }

    return dev;
}

void device_destroy(struct class *cls, dev_t devt)
{
    (void)cls;
    (void)devt;
    /* Find and unregister device by class + devt */
}

int device_create_file(struct device *dev, const struct device_attribute *attr)
{
    if (!dev || !attr) return -EINVAL;
    return sysfs_create_file(&dev->kobj, &attr->attr);
}

void device_remove_file(struct device *dev, const struct device_attribute *attr)
{
    if (!dev || !attr) return;
    sysfs_remove_file(&dev->kobj, &attr->attr);
}

int device_add_groups(struct device *dev, const struct attribute_group **groups)
{
    if (!dev) return -EINVAL;
    return sysfs_create_groups(&dev->kobj, groups);
}

void device_remove_groups(struct device *dev, const struct attribute_group **groups)
{
    if (!dev) return;
    sysfs_remove_groups(&dev->kobj, groups);
}

/* ------------------------------------------------------------------ */
/*  driver_register / driver_unregister                                */
/* ------------------------------------------------------------------ */

int driver_register(struct device_driver *drv)
{
    int ret;

    if (!drv || !drv->name || !drv->bus) return -EINVAL;

    kobject_init(&drv->kobj, &driver_ktype);

    ret = kobject_add(&drv->kobj, &drv->bus->drivers_kset->kobj, "%s", drv->name);
    if (ret != EOK) return ret;

    /* Add driver groups */
    if (drv->groups) {
        ret = sysfs_create_groups(&drv->kobj, drv->groups);
        if (ret != EOK) {
            kobject_del(&drv->kobj);
            kobject_put(&drv->kobj);
            return ret;
        }
    }

    /* Add driver to bus's driver kset */
    spin_lock(&drv->bus->drivers_kset->list_lock);
    drv->bus->drivers_kset->list = clist_append(drv->bus->drivers_kset->list, &drv->kobj);
    spin_unlock(&drv->bus->drivers_kset->list_lock);

    kobject_uevent(&drv->kobj, KOBJ_ADD);
    return EOK;
}

void driver_unregister(struct device_driver *drv)
{
    if (!drv) return;

    if (drv->kobj.state_add_uevent_sent && !drv->kobj.state_remove_uevent_sent) (void)kobject_uevent(&drv->kobj, KOBJ_REMOVE);

    /* Remove from bus */
    if (drv->bus && drv->bus->drivers_kset) {
        spin_lock(&drv->bus->drivers_kset->list_lock);
        drv->bus->drivers_kset->list = clist_delete(drv->bus->drivers_kset->list, &drv->kobj);
        spin_unlock(&drv->bus->drivers_kset->list_lock);
    }

    if (drv->groups) sysfs_remove_groups(&drv->kobj, drv->groups);
    kobject_del(&drv->kobj);
    kobject_put(&drv->kobj);
}

int driver_create_file(struct device_driver *drv, const struct driver_attribute *attr)
{
    if (!drv || !attr) return -EINVAL;
    return sysfs_create_file(&drv->kobj, &attr->attr);
}

void driver_remove_file(struct device_driver *drv, const struct driver_attribute *attr)
{
    if (!drv || !attr) return;
    sysfs_remove_file(&drv->kobj, &attr->attr);
}

/* ------------------------------------------------------------------ */
/*  class_register / class_unregister                                  */
/* ------------------------------------------------------------------ */

int class_register(struct class *cls)
{
    int             ret;
    struct kobject *parent;

    if (!cls || !cls->name) return -EINVAL;

    parent = get_class_kobj();
    if (!parent) return -ENOENT;

    kset_init(&cls->subsys);
    cls->subsys.kobj.ktype = &class_ktype;

    ret = kobject_add(&cls->subsys.kobj, parent, "%s", cls->name);
    if (ret != EOK) return ret;

    /* Create class attribute groups */
    if (cls->class_groups) {
        ret = sysfs_create_groups(&cls->subsys.kobj, cls->class_groups);
        if (ret != EOK) {
            kobject_del(&cls->subsys.kobj);
            kobject_put(&cls->subsys.kobj);
            return ret;
        }
    }

    kobject_uevent(&cls->subsys.kobj, KOBJ_ADD);
    return EOK;
}

void class_unregister(struct class *cls)
{
    if (!cls) return;
    if (cls->subsys.kobj.state_add_uevent_sent && !cls->subsys.kobj.state_remove_uevent_sent)
        (void)kobject_uevent(&cls->subsys.kobj, KOBJ_REMOVE);
    if (cls->class_groups) sysfs_remove_groups(&cls->subsys.kobj, cls->class_groups);
    kobject_del(&cls->subsys.kobj);
    kobject_put(&cls->subsys.kobj);
}

int class_create_file(struct class *cls, const struct class_attribute *attr)
{
    if (!cls || !attr) return -EINVAL;
    return sysfs_create_file(&cls->subsys.kobj, &attr->attr);
}

void class_remove_file(struct class *cls, const struct class_attribute *attr)
{
    if (!cls || !attr) return;
    sysfs_remove_file(&cls->subsys.kobj, &attr->attr);
}

struct device *class_find_device(struct class *cls, struct device *start, const void *data, int (*match)(struct device *, const void *))
{
    (void)cls;
    (void)start;
    (void)data;
    (void)match;
    /* For now, return NULL ?full implementation would
     * iterate over the class's device list */
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  bus_find_driver_by_name                                            */
/* ------------------------------------------------------------------ */

struct device_driver *bus_find_driver_by_name(struct bus_type *bus, const char *name)
{
    clist_t node;

    if (!bus || !bus->drivers_kset) return NULL;

    spin_lock(&bus->drivers_kset->list_lock);
    for (node = bus->drivers_kset->list; node; node = node->next) {
        struct kobject *kobj = node->data;
        if (!kobj) continue;

        struct device_driver *drv = (struct device_driver *)((char *)kobj - offsetof(struct device_driver, kobj));

        if (!name || (drv->name && streq(drv->name, name))) {
            spin_unlock(&bus->drivers_kset->list_lock);
            return drv;
        }
    }
    spin_unlock(&bus->drivers_kset->list_lock);

    return NULL;
}

/* ------------------------------------------------------------------ */
/*  device_model_init                                                  */
/* ------------------------------------------------------------------ */

int device_model_init(void)
{
#if CONFIG_SYSFS
    /* Locate the top-level sysfs kobjects created by sysfs_init */
    /* They are children of sysfs_root_kobj */

    /* Wait ?we need sysfs_root_kobj to find these.
     * The kobject_create_and_add calls in sysfs_init already
     * create them under sysfs_root_kobj. We just need to find them. */

    /* For now, we'll find them lazily. The registration functions
     * for bus/class will use the extern pointers declared at the
     * top of this file. They need to be initialised. */

    if (sysfs_root_kobj) {
        devices_kobj = sysfs_find_child_kobj(sysfs_root_kobj, "devices");
        bus_kobj     = sysfs_find_child_kobj(sysfs_root_kobj, "bus");
        class_kobj   = sysfs_find_child_kobj(sysfs_root_kobj, "class");
    }

    return EOK;
#endif
}

/* Internal helper: find child kobject by name */
static struct kobject *sysfs_find_child_kobj(struct kobject *parent, const char *name)
{
    clist_t node;
    if (!parent || !name) return NULL;

    for (node = parent->children; node; node = node->next) {
        struct kobject *kobj = node->data;
        if (kobj && kobj->name && streq(kobj->name, name)) return kobj;
    }
    return NULL;
}

void device_model_exit(void)
{
    devices_kobj = NULL;
    bus_kobj     = NULL;
    class_kobj   = NULL;
}
