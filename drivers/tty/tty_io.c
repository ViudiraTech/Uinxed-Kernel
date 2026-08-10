/*
 *
 *      tty_io.c
 *      TTY driver registry and per-node dispatch
 *
 *      2026/8/10 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/char/chrdev.h>
#include <drivers/tty/tty_driver.h>
#include <fs/devtmpfs/devtmpfs.h>
#include <fs/tmpfs/tmpfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

#define TTY_MAJOR             4
#define TTY_AUX_MAJOR         5
#define TTY_SERIAL_MINOR_BASE 64

static tty_driver_t *tty_driver_list;
static spinlock_t    tty_driver_lock;

typedef struct tty_registered_device {
        tty_driver_t                 *drv;
        int                           index;
        char                          name[32];
        struct tty_registered_device *next;
} tty_registered_device_t;

static tty_registered_device_t *tty_registered_devices;

typedef struct tty_dispatch {
        tty_driver_t *drv;
        int           index;
        void         *drv_data;
} tty_dispatch_t;

void tty_for_each_registered(int (*cb)(tty_driver_t *drv, int index, const char *name, void *opaque), void *opaque)
{
    tty_registered_device_t *dev;

    spin_lock(&tty_driver_lock);
    for (dev = tty_registered_devices; dev; dev = dev->next) (void)cb(dev->drv, dev->index, dev->name, opaque);
    spin_unlock(&tty_driver_lock);
}

tty_driver_t *tty_driver_for_dev(uint32_t major, uint32_t minor)
{
    tty_driver_t *drv;

    spin_lock(&tty_driver_lock);
    for (drv = tty_driver_list; drv; drv = drv->next) {
        if (drv->major != major) continue;
        if (minor >= drv->minor_start && minor < drv->minor_start + drv->num) break;
    }
    spin_unlock(&tty_driver_lock);
    return drv;
}

int tty_register_device(tty_driver_t *drv, int index, const char *node_name)
{
    tty_registered_device_t *dev;

    if (!drv || !node_name || index < 0 || (uint32_t)index >= drv->num) return -EINVAL;

    dev = calloc(1, sizeof(*dev));
    if (!dev) return -ENOMEM;
    strncpy(dev->name, node_name, sizeof(dev->name) - 1);
    dev->drv   = drv;
    dev->index = index;
    spin_lock(&tty_driver_lock);
    dev->next              = tty_registered_devices;
    tty_registered_devices = dev;
    spin_unlock(&tty_driver_lock);
    return 0;
}

int tty_register_driver(tty_driver_t *drv)
{
    if (!drv || !drv->name) return -EINVAL;

    spin_lock(&tty_driver_lock);
    drv->next       = tty_driver_list;
    tty_driver_list = drv;
    spin_unlock(&tty_driver_lock);

    plogk("tty_io: Registered tty driver \"%s\" (major %u, %u minors).\n", drv->name, drv->major, drv->num);
    return 0;
}

/* Create the /dev nodes for every registered tty device. */
int tty_devices_populate(void)
{
    static const tmpfs_device_ops_t tty_node_ops = {
        .open       = tty_dispatch_open,
        .release    = tty_dispatch_release,
        .file_read  = tty_dispatch_read,
        .file_write = tty_dispatch_write,
        .file_poll  = tty_dispatch_poll,
        .file_ioctl = tty_dispatch_ioctl,
    };
    tty_registered_device_t *dev;
    int                      count = 0;

    spin_lock(&tty_driver_lock);
    for (dev = tty_registered_devices; dev; dev = dev->next) {
        char     path[64];
        uint32_t major = dev->drv->major;
        uint32_t minor = dev->drv->minor_start + (uint32_t)dev->index;

        (void)snprintf(path, sizeof(path), "/dev/%s", dev->name);
        if (devtmpfs_register_char_device(path, MKDEV(major, minor), MKDEV(major, minor), dev->drv->node_type, &tty_node_ops) != 0) continue;
        if (dev->drv->mode) {
            vfs_node_t node = vfs_open(path);
            if (node) {
                node->mode = dev->drv->mode;
                vfs_close(node);
            }
        }
        count++;
    }
    spin_unlock(&tty_driver_lock);
    return count;
}

void tty_unregister_driver(tty_driver_t *drv)
{
    tty_driver_t            **link;
    tty_registered_device_t **dev;

    if (!drv) return;
    spin_lock(&tty_driver_lock);
    link = &tty_driver_list;
    while (*link && *link != drv) link = &(*link)->next;
    if (*link) *link = drv->next;
    dev = &tty_registered_devices;
    while (*dev) {
        if ((*dev)->drv == drv) {
            tty_registered_device_t *victim = *dev;
            *dev                            = victim->next;
            free(victim);
        } else {
            dev = &(*dev)->next;
        }
    }
    spin_unlock(&tty_driver_lock);
}

/* Per-node dispatch */

int tty_dispatch_open(struct vfs_node *node, uint64_t flags, void **private_data)
{
    tty_driver_t   *drv;
    tty_dispatch_t *d;
    int             index;

    if (!node) return -ENXIO;
    drv = tty_driver_for_dev(MAJOR(node->rdev), MINOR(node->rdev));
    if (!drv) return -ENXIO;
    index = (int)(MINOR(node->rdev) - drv->minor_start);
    if (index < 0 || (uint32_t)index >= drv->num) return -ENXIO;

    d = calloc(1, sizeof(*d));
    if (!d) return -ENOMEM;
    d->drv   = drv;
    d->index = index;

    if (drv->open) {
        int status = drv->open(drv, index, flags, &d->drv_data);
        if (status) {
            free(d);
            return status;
        }
    }
    *private_data = d;
    return 0;
}

void tty_dispatch_release(struct vfs_node *node, void *private_data)
{
    tty_dispatch_t *d = private_data;
    (void)node;
    if (!d) return;
    if (d->drv->release) d->drv->release(d->drv, d->index, d->drv_data);
    free(d);
}

int64_t tty_dispatch_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    tty_dispatch_t *d = private_data;
    (void)ctx;
    (void)offset;
    if (!d || !d->drv->read) return -ENXIO;
    return d->drv->read(d->drv, d->index, d->drv_data, flags, addr, size);
}

int64_t tty_dispatch_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    tty_dispatch_t *d = private_data;
    (void)ctx;
    (void)offset;
    if (!d || !d->drv->write) return -ENXIO;
    return d->drv->write(d->drv, d->index, d->drv_data, flags, addr, size);
}

int tty_dispatch_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    tty_dispatch_t *d = private_data;
    (void)ctx;
    (void)flags;
    if (!d || !d->drv->poll) return 0;
    return d->drv->poll(d->drv, d->index, d->drv_data, flags, events);
}

int tty_dispatch_ioctl(void *ctx, void *private_data, uint64_t flags, size_t req, void *arg)
{
    tty_dispatch_t *d = private_data;
    (void)ctx;
    if (!d || !d->drv->ioctl) return -ENOTTY;
    return d->drv->ioctl(d->drv, d->index, d->drv_data, flags, req, arg);
}
