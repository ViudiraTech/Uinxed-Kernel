/*
 *
 *      sysfs.h
 *      sysfs filesystem header file
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SYSFS_H_
#define INCLUDE_SYSFS_H_

#include <kernel/printk.h>
#include <libs/std/stdarg.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

struct kobject;
struct kset;
struct kobj_type;

/* ------------------------------------------------------------------ */
/*  Core attribute types                                               */
/* ------------------------------------------------------------------ */

struct attribute {
    const char *name;
    uint16_t    mode;
};

struct bin_attribute {
    struct attribute attr;
    size_t           size;
    ssize_t (*read)(struct kobject *kobj, struct bin_attribute *attr,
                    char *buffer, int64_t pos, size_t count);
    ssize_t (*write)(struct kobject *kobj, struct bin_attribute *attr,
                     char *buffer, int64_t pos, size_t count);
    int (*mmap)(struct kobject *kobj, struct bin_attribute *attr,
                void *vma);
};

struct attribute_group {
    const char  *name;
    int        (*is_visible)(struct kobject *kobj,
                             struct attribute *attr, int idx);
    struct attribute     **attrs;
    struct bin_attribute **bin_attrs;
};

/* ------------------------------------------------------------------ */
/*  Sysfs operations (per-ktype read/write callbacks)                  */
/* ------------------------------------------------------------------ */

struct sysfs_ops {
    ssize_t (*show)(struct kobject *kobj, struct attribute *attr,
                    char *buf);
    ssize_t (*store)(struct kobject *kobj, struct attribute *attr,
                     const char *buf, size_t count);
};

/* ------------------------------------------------------------------ */
/*  Convenience attribute macros                                       */
/* ------------------------------------------------------------------ */

#define __stringify_1(x) #x
#define __stringify(x)   __stringify_1(x)

#define __ATTR(_name, _mode) \
    { .name = __stringify(_name), .mode = _mode }

#define __ATTR_RO(_name) __ATTR(_name, 0444)
#define __ATTR_WO(_name) __ATTR(_name, 0200)
#define __ATTR_RW(_name) __ATTR(_name, 0644)

#define __ATTR_RO_MODE(_name, _mode) __ATTR(_name, _mode)

#define __ATTR_NULL \
    { .name = NULL }

/* ------------------------------------------------------------------ */
/*  sysfs buffer helpers (PAGE_SIZE semantics)                         */
/* ------------------------------------------------------------------ */

#define SYSFS_PAGE_SIZE 4096

/* Emit formatted output into a sysfs buffer at offset 0 */
static inline int sysfs_emit(char *buf, const char *fmt, ...)
{
    va_list args;
    int     n;

    if (!buf) return 0;

    va_start(args, fmt);
    n = vsnprintf(buf, SYSFS_PAGE_SIZE, fmt, args);
    va_end(args);

    return n;
}

/* Emit formatted output into a sysfs buffer at a given offset */
static inline int sysfs_emit_at(char *buf, int at, const char *fmt, ...)
{
    va_list args;
    int     n;

    if (!buf) return 0;
    if (at < 0 || at >= SYSFS_PAGE_SIZE) return 0;

    va_start(args, fmt);
    n = vsnprintf(buf + at, SYSFS_PAGE_SIZE - at, fmt, args);
    va_end(args);

    return n;
}

/* ------------------------------------------------------------------ */
/*  sysfs filesystem API                                               */
/* ------------------------------------------------------------------ */

/* Create / remove a single attribute file under a kobject */
int sysfs_create_file(struct kobject *kobj, const struct attribute *attr);
void sysfs_remove_file(struct kobject *kobj, const struct attribute *attr);

/* Create / remove a binary attribute file under a kobject */
int sysfs_create_bin_file(struct kobject *kobj,
                          const struct bin_attribute *attr);
void sysfs_remove_bin_file(struct kobject *kobj,
                           const struct bin_attribute *attr);

/* Create / remove a symbolic link under a kobject */
int sysfs_create_symlink(struct kobject *kobj, struct kobject *target,
                         const char *name);
void sysfs_remove_symlink(struct kobject *kobj, const char *name);

/* Create / remove a group of attribute files at once */
int sysfs_create_group(struct kobject *kobj,
                       const struct attribute_group *grp);
void sysfs_remove_group(struct kobject *kobj,
                        const struct attribute_group *grp);

/* Create / remove a group of attribute files (with merge semantics) */
int sysfs_create_groups(struct kobject *kobj,
                        const struct attribute_group **groups);
void sysfs_remove_groups(struct kobject *kobj,
                         const struct attribute_group **groups);

/* Create / remove the sysfs directory for a kobject (called internally) */
int sysfs_create_dir(struct kobject *kobj);
void sysfs_remove_dir(struct kobject *kobj);

/* Register sysfs with the VFS layer and mount at /sys */
void sysfs_regist(void);
int sysfs_init(void);

/* Internal: remove all attribute files and symlinks under a kobject */
void sysfs_cleanup_kobject_files(struct kobject *kobj);

/* Internal: notify sysfs that a kobject's name has changed */
void sysfs_rename_dir(struct kobject *kobj, const char *new_name);

/* Internal: get the absolute sysfs path of a kobject */
char *kobject_get_path(struct kobject *kobj);

/* ------------------------------------------------------------------ */
/*  Device-model convenience macros                                    */
/* ------------------------------------------------------------------ */

#define DEVICE_ATTR(_name, _mode, _show, _store)          \
    struct device_attribute dev_attr_##_name = {           \
        .attr = __ATTR(_name, _mode),                     \
        .show = _show,                                    \
        .store = _store,                                  \
    }

#define DEVICE_ATTR_RO(_name)                             \
    struct device_attribute dev_attr_##_name = {           \
        .attr = __ATTR_RO(_name),                         \
        .show = _name##_show,                             \
        .store = NULL,                                    \
    }

#define DEVICE_ATTR_WO(_name)                             \
    struct device_attribute dev_attr_##_name = {           \
        .attr = __ATTR_WO(_name),                         \
        .show = NULL,                                     \
        .store = _name##_store,                           \
    }

#define DEVICE_ATTR_RW(_name)                             \
    struct device_attribute dev_attr_##_name = {           \
        .attr = __ATTR_RW(_name),                         \
        .show = _name##_show,                             \
        .store = _name##_store,                           \
    }

#define BUS_ATTR(_name, _mode, _show, _store)             \
    struct bus_attribute bus_attr_##_name = {              \
        .attr = __ATTR(_name, _mode),                     \
        .show = _show,                                    \
        .store = _store,                                  \
    }

#define BUS_ATTR_RO(_name)                                \
    struct bus_attribute bus_attr_##_name = {              \
        .attr = __ATTR_RO(_name),                         \
        .show = _name##_show,                             \
        .store = NULL,                                    \
    }

#define BUS_ATTR_WO(_name)                                \
    struct bus_attribute bus_attr_##_name = {              \
        .attr = __ATTR_WO(_name),                         \
        .show = NULL,                                     \
        .store = _name##_store,                           \
    }

#define BUS_ATTR_RW(_name)                                \
    struct bus_attribute bus_attr_##_name = {              \
        .attr = __ATTR_RW(_name),                         \
        .show = _name##_show,                             \
        .store = _name##_store,                           \
    }

#define DRIVER_ATTR(_name, _mode, _show, _store)          \
    struct driver_attribute driver_attr_##_name = {        \
        .attr = __ATTR(_name, _mode),                     \
        .show = _show,                                    \
        .store = _store,                                  \
    }

#define DRIVER_ATTR_RO(_name)                             \
    struct driver_attribute driver_attr_##_name = {        \
        .attr = __ATTR_RO(_name),                         \
        .show = _name##_show,                             \
        .store = NULL,                                    \
    }

#define DRIVER_ATTR_RW(_name)                             \
    struct driver_attribute driver_attr_##_name = {        \
        .attr = __ATTR_RW(_name),                         \
        .show = _name##_show,                             \
        .store = _name##_store,                           \
    }

#define CLASS_ATTR(_name, _mode, _show, _store)           \
    struct class_attribute class_attr_##_name = {          \
        .attr = __ATTR(_name, _mode),                     \
        .show = _show,                                    \
        .store = _store,                                  \
    }

#define CLASS_ATTR_RO(_name)                              \
    struct class_attribute class_attr_##_name = {          \
        .attr = __ATTR_RO(_name),                         \
        .show = _name##_show,                             \
        .store = NULL,                                    \
    }

#endif // INCLUDE_SYSFS_H_
