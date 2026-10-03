/*
 *
 *      kernel_sysfs.c
 *      /sys/kernel/ attribute files (version, cmdline, hostname, etc.)
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/cmdline/cmdline.h>
#include <kernel/errno.h>
#include <kernel/timer/timer.h>
#include <kernel/uinxed.h>
#include <libs/kobject/kobject.h>
#include <libs/std/string.h>
#include <process/namespace.h>
#include <process/sched.h>

#if CONFIG_SYSFS

/* Attribute definitions */

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

/* Show the kernel name and version. */
static ssize_t version_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "%s %s\n", KERNEL_NAME, KERNEL_VERSION);
}

/* Show the boot command line. */
static ssize_t cmdline_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    const char *cmd = get_cmdline();
    return (ssize_t)sysfs_emit(buf, "%s\n", cmd ? cmd : "");
}

/* Show the current hostname. */
static ssize_t hostname_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;

    uts_namespace_t *uts = uts_namespace_current();
    char             name[65];
    spin_lock(&uts->ns.lock);
    memcpy(name, uts->nodename, sizeof(name));
    spin_unlock(&uts->ns.lock);
    return (ssize_t)sysfs_emit(buf, "%s\n", name);
}

/* Set the current hostname from a sysfs write. */
static ssize_t hostname_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)
{
    (void)kobj;
    (void)attr;

    size_t length = count;
    while (length && (buf[length - 1] == '\n' || buf[length - 1] == '\r')) length--;
    if (length > 64) return -EINVAL;

    uts_namespace_t *uts = uts_namespace_current();
    spin_lock(&uts->ns.lock);
    memcpy(uts->nodename, buf, length);
    uts->nodename[length] = '\0';
    spin_unlock(&uts->ns.lock);
    return (ssize_t)count;
}

/* Accept a write to an attribute that is not persisted. */
static ssize_t store_accepted(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)
{
    (void)kobj;
    (void)attr;
    (void)buf;
    return (ssize_t)count;
}

/* Show the operating system type. */
static ssize_t ostype_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, KERNEL_NAME "\n");
}

/* Show the kernel release version. */
static ssize_t osrelease_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "%s\n", KERNEL_VERSION);
}

/* Show the uevent sequence number. */
static ssize_t uevent_seqnum_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "%llu\n", kobject_uevent_seqnum());
}

/* Show the profiling mode. */
static ssize_t profiling_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "0\n");
}

/* Show system uptime in seconds. */
static ssize_t uptime_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    uint64_t ns  = timer_monotonic_ns();
    uint64_t sec = ns / TIMER_NSEC_PER_SEC;
    uint64_t cs  = (ns % TIMER_NSEC_PER_SEC) / 10000000ULL;
    return (ssize_t)sysfs_emit(buf, "%llu.%02llu\n", sec, cs);
}

/* Unified show/store that dispatches to the correct function based on the attribute pointer. */
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
    return -EINVAL;
}

/* Dispatch a store operation to the matching attribute handler. */
static ssize_t kernel_attr_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)
{
    if (attr == &hostname_attr) return hostname_store(kobj, attr, buf, count);
    if (attr == &profiling_attr) return store_accepted(kobj, attr, buf, count);
    return -EINVAL;
}

static const struct sysfs_ops kernel_sysfs_ops_dispatch = {
    .show  = kernel_attr_show,
    .store = kernel_attr_store,
};

static struct kobj_type kernel_ktype = {
    .release       = kobject_static_release,
    .sysfs_ops     = &kernel_sysfs_ops_dispatch,
    .default_attrs = kernel_attrs,
};

/* Attach the kernel attribute files to /sys/kernel/. */
void kernel_sysfs_init(void)
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
        plogk("kernel_sysfs: /sys/kernel/ kobject not found.\n");
        return;
    }

    /* Replace the kobj_type so the attrs and sysfs_ops take effect */
    kernel_kobj->ktype = &kernel_ktype;

    /* Create default attribute files */
    if (kernel_ktype.default_attrs) {
        struct attribute **attr;
        for (attr = kernel_ktype.default_attrs; *attr; attr++)
            if ((*attr)->name) sysfs_create_file(kernel_kobj, *attr);
    }

    plogk("kernel_sysfs: registered /sys/kernel/\n");
}

#endif
