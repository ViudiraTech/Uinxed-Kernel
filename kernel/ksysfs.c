/*
 *
 *      ksysfs.c
 *      /sys/kernel/ attribute files (version, cmdline, hostname, etc.)
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/sysfs.h>
#include <fs/vfs.h>
#include <kernel/cmdline.h>
#include <kernel/errno.h>
#include <kernel/kobject.h>
#include <kernel/printk.h>
#include <kernel/uinxed.h>
#include <libs/std/stdarg.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <proc/sched.h>

/* ------------------------------------------------------------------ */
/*  Forward reference                                                  */
/* ------------------------------------------------------------------ */

extern struct kobject *sysfs_root_kobj;

/* ------------------------------------------------------------------ */
/*  Attribute show functions                                           */
/* ------------------------------------------------------------------ */

static ssize_t version_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "%s %s\n", KERNEL_NAME, KERNEL_VERSION);
}

static ssize_t cmdline_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    const char *cmd = get_cmdline();
    return (ssize_t)sysfs_emit(buf, "%s\n", cmd ? cmd : "");
}

static ssize_t hostname_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "localhost\n");
}

static ssize_t hostname_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)
{
    (void)kobj;
    (void)attr;
    (void)buf;
    /* For now, hostname changes are accepted but not stored persistently */
    return (ssize_t)count;
}

static ssize_t ostype_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "Uinxed\n");
}

static ssize_t osrelease_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "%s\n", KERNEL_VERSION);
}

static ssize_t uevent_seqnum_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "%llu\n", (unsigned long long)kobject_uevent_seqnum());
}

static ssize_t profiling_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "0\n");
}

static ssize_t profiling_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)
{
    (void)kobj;
    (void)attr;
    (void)buf;
    return (ssize_t)count;
}

static ssize_t uptime_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    uint64_t ticks = sched_ticks();
    uint64_t sec   = ticks / 100;
    uint64_t cs    = ticks % 100;
    return (ssize_t)sysfs_emit(buf, "%llu.%02llu\n", (unsigned long long)sec, (unsigned long long)cs);
}

/* ------------------------------------------------------------------ */
/*  Attribute definitions                                              */
/* ------------------------------------------------------------------ */

static struct attribute version_attr       = __ATTR_RO(version);
static struct attribute cmdline_attr       = __ATTR_RO(cmdline);
static struct attribute hostname_attr      = __ATTR_RW(hostname);
static struct attribute ostype_attr        = __ATTR_RO(ostype);
static struct attribute osrelease_attr     = __ATTR_RO(osrelease);
static struct attribute uevent_seqnum_attr = __ATTR_RO(uevent_seqnum);
static struct attribute profiling_attr     = __ATTR_RW(profiling);
static struct attribute uptime_attr        = __ATTR_RO(uptime);

static struct attribute *kernel_attrs[] = {
    &version_attr, &cmdline_attr, &hostname_attr, &ostype_attr, &osrelease_attr, &uevent_seqnum_attr, &profiling_attr, &uptime_attr, NULL,
};

/* ------------------------------------------------------------------ */
/*  Sysfs ops                                                          */
/* ------------------------------------------------------------------ */

/*
 * Unified show/store that dispatches to the correct function based
 * on the attribute pointer.
 */
static ssize_t kernel_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    if (attr == &version_attr) return version_show(kobj, attr, buf);
    if (attr == &cmdline_attr) return cmdline_show(kobj, attr, buf);
    if (attr == &hostname_attr) return hostname_show(kobj, attr, buf);
    if (attr == &ostype_attr) return ostype_show(kobj, attr, buf);
    if (attr == &osrelease_attr) return osrelease_show(kobj, attr, buf);
    if (attr == &uevent_seqnum_attr) return uevent_seqnum_show(kobj, attr, buf);
    if (attr == &profiling_attr) return profiling_show(kobj, attr, buf);
    if (attr == &uptime_attr) return uptime_show(kobj, attr, buf);
    return -EIO;
}

static ssize_t kernel_attr_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)
{
    if (attr == &hostname_attr) return hostname_store(kobj, attr, buf, count);
    if (attr == &profiling_attr) return profiling_store(kobj, attr, buf, count);
    return -EIO;
}

static const struct sysfs_ops kernel_sysfs_ops_dispatch = {
    .show  = kernel_attr_show,
    .store = kernel_attr_store,
};

/* ------------------------------------------------------------------ */
/*  Kobj type                                                          */
/* ------------------------------------------------------------------ */

static void kernel_kobj_release(struct kobject *kobj)
{
    (void)kobj;
    /* Static, nothing to free */
}

static struct kobj_type kernel_ktype = {
    .release       = kernel_kobj_release,
    .sysfs_ops     = &kernel_sysfs_ops_dispatch,
    .default_attrs = kernel_attrs,
};

/* ------------------------------------------------------------------ */
/*  Initialization                                                     */
/* ------------------------------------------------------------------ */

void ksysfs_init(void)
{
    struct kobject *kernel_kobj = NULL;
    clist_t         node;

    if (!sysfs_root_kobj) return;

    /* Find /sys/kernel/ kobject */
    for (node = sysfs_root_kobj->children; node; node = node->next) {
        struct kobject *child = node->data;
        if (child && child->name && streq(child->name, "kernel")) {
            kernel_kobj = child;
            break;
        }
    }

    if (!kernel_kobj) {
        plogk("ksysfs: /sys/kernel/ kobject not found\n");
        return;
    }

    /* Replace the kobj_type so our attrs and sysfs_ops take effect */
    kernel_kobj->ktype = &kernel_ktype;

    /* Create default attribute files */
    if (kernel_ktype.default_attrs) {
        struct attribute **attr;
        for (attr = kernel_ktype.default_attrs; *attr; attr++) {
            if ((*attr)->name) { sysfs_create_file(kernel_kobj, *attr); }
        }
    }

    plogk("ksysfs: /sys/kernel/ attributes registered\n");
}
