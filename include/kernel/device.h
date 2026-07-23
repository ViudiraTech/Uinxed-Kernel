/*
 *
 *      device.h
 *      Device model header file (bus, device, driver, class)
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DEVICE_H_
#define INCLUDE_DEVICE_H_

#include <fs/sysfs.h>
#include <kernel/kobject.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

struct device;
struct device_driver;
struct bus_type;
struct class;

/* ------------------------------------------------------------------ */
/*  Device type identifiers (major / minor encoding)                   */
/* ------------------------------------------------------------------ */

typedef uint64_t dev_t;

#define MAJOR(dev)  ((uint32_t)(((dev) >> 32) & 0xFFFFF))
#define MINOR(dev)  ((uint32_t)((dev) &0xFFFFF))
#define MKDEV(ma, mi) ((((uint64_t)(ma) & 0xFFFFF) << 32) | ((uint64_t)(mi) & 0xFFFFF))

/* ------------------------------------------------------------------ */
/*  Device attribute type (attribute + device-specific show/store)     */
/* ------------------------------------------------------------------ */

struct device_attribute {
    struct attribute attr;
    ssize_t (*show)(struct device *dev, struct device_attribute *attr,
                    char *buf);
    ssize_t (*store)(struct device *dev, struct device_attribute *attr,
                     const char *buf, size_t count);
};

/* ------------------------------------------------------------------ */
/*  Bus attribute type                                                 */
/* ------------------------------------------------------------------ */

struct bus_attribute {
    struct attribute attr;
    ssize_t (*show)(struct bus_type *bus, struct bus_attribute *attr,
                    char *buf);
    ssize_t (*store)(struct bus_type *bus, struct bus_attribute *attr,
                     const char *buf, size_t count);
};

/* ------------------------------------------------------------------ */
/*  Driver attribute type                                              */
/* ------------------------------------------------------------------ */

struct driver_attribute {
    struct attribute attr;
    ssize_t (*show)(struct device_driver *drv,
                    struct driver_attribute *attr, char *buf);
    ssize_t (*store)(struct device_driver *drv,
                     struct driver_attribute *attr, const char *buf,
                     size_t count);
};

/* ------------------------------------------------------------------ */
/*  Class attribute type                                               */
/* ------------------------------------------------------------------ */

struct class_attribute {
    struct attribute attr;
    ssize_t (*show)(struct class *cls, struct class_attribute *attr,
                    char *buf);
    ssize_t (*store)(struct class *cls, struct class_attribute *attr,
                     const char *buf, size_t count);
};

/* ------------------------------------------------------------------ */
/*  bus_type — a communication channel between CPUs and devices         */
/* ------------------------------------------------------------------ */

struct bus_type {
    const char             *name;
    const char             *dev_name;
    struct kset             subsys;          /* kset for this bus */
    struct kset            *devices_kset;    /* kset containing all devices */
    struct kset            *drivers_kset;    /* kset containing all drivers */

    int (*match)(struct device *dev, struct device_driver *drv);
    int (*uevent)(struct device *dev, char *envp[], int nenv);
    int (*probe)(struct device *dev);
    int (*remove)(struct device *dev);
    void (*shutdown)(struct device *dev);

    int (*suspend)(struct device *dev);
    int (*resume)(struct device *dev);

    const struct attribute_group **dev_groups;
    const struct attribute_group **drv_groups;
};

/* ------------------------------------------------------------------ */
/*  device_driver — binds to a bus and handles a class of devices       */
/* ------------------------------------------------------------------ */

struct device_driver {
    const char        *name;
    struct bus_type   *bus;
    struct kobject     kobj;             /* appears under /sys/bus/<bus>/drivers/ */

    int (*probe)(struct device *dev);
    int (*remove)(struct device *dev);
    void (*shutdown)(struct device *dev);
    int (*suspend)(struct device *dev);
    int (*resume)(struct device *dev);

    const struct attribute_group **groups;
};

/* ------------------------------------------------------------------ */
/*  device — represents a physical or virtual device in the system      */
/* ------------------------------------------------------------------ */

struct device {
    struct kobject         kobj;          /* appears under /sys/devices/ */
    struct device         *parent;        /* parent device (NULL = root) */
    struct bus_type       *bus;           /* bus the device sits on */
    struct device_driver  *driver;        /* driver bound to this device */
    struct class          *class;         /* optional class grouping */

    void                  *driver_data;   /* private driver data */
    dev_t                  devt;          /* major:minor device number */
    uint64_t               devid;         /* bus-specific device ID */

    void                 (*release)(struct device *dev);

    const struct attribute_group **groups;

    /* hotplug */
    int (*uevent)(struct device *dev, char *envp[], int nenv);
};

/* ------------------------------------------------------------------ */
/*  class — groups devices by functional type (e.g. "net", "tty")       */
/* ------------------------------------------------------------------ */

struct class {
    const char        *name;
    struct kset        subsys;           /* kset under /sys/class/ */
    struct kobject    *dev_kobj;         /* for /sys/class/<name>/devices/ */

    int (*dev_uevent)(struct device *dev, char *envp[], int nenv);
    void (*dev_release)(struct device *dev);

    const struct attribute_group **class_groups;
};

/* ------------------------------------------------------------------ */
/*  Device model registration API                                      */
/* ------------------------------------------------------------------ */

/* ---- bus ---- */
int bus_register(struct bus_type *bus);
void bus_unregister(struct bus_type *bus);

int bus_create_file(struct bus_type *bus, struct bus_attribute *attr);
void bus_remove_file(struct bus_type *bus, struct bus_attribute *attr);

/* ---- device ---- */
int device_register(struct device *dev);
void device_unregister(struct device *dev);

struct device *device_create(struct class *cls, struct device *parent,
                             dev_t devt, void *drvdata, const char *fmt, ...);
void device_destroy(struct class *cls, dev_t devt);

int device_create_file(struct device *dev,
                       const struct device_attribute *attr);
void device_remove_file(struct device *dev,
                        const struct device_attribute *attr);

int device_add_groups(struct device *dev,
                      const struct attribute_group **groups);
void device_remove_groups(struct device *dev,
                          const struct attribute_group **groups);

/* ---- driver ---- */
int driver_register(struct device_driver *drv);
void driver_unregister(struct device_driver *drv);

int driver_create_file(struct device_driver *drv,
                       const struct driver_attribute *attr);
void driver_remove_file(struct device_driver *drv,
                        const struct driver_attribute *attr);

/* ---- class ---- */
int class_register(struct class *cls);
void class_unregister(struct class *cls);

struct device *class_find_device(struct class *cls, struct device *start,
                                 const void *data,
                                 int (*match)(struct device *, const void *));

int class_create_file(struct class *cls,
                      const struct class_attribute *attr);
void class_remove_file(struct class *cls,
                       const struct class_attribute *attr);

/* ---- core init ---- */
int device_model_init(void);
void device_model_exit(void);

/* ------------------------------------------------------------------ */
/*  Helper: get /sys/devices/... path                                  */
/* ------------------------------------------------------------------ */

static inline const char *dev_name(const struct device *dev)
{
    if (!dev) return "(null)";
    return kobject_name(&dev->kobj);
}

/* Find a device driver by name on a bus */
struct device_driver *bus_find_driver_by_name(struct bus_type *bus,
                                              const char *name);

#endif // INCLUDE_DEVICE_H_
