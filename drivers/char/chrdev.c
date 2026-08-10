/*
 *
 *      chrdev.c
 *      Character device registry (Linux fs/char_dev.c analog)
 *
 *      2026/8/10 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/char/chrdev.h>
#include <fs/devtmpfs/devtmpfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

static cdev_t    *chrdev_list;
static spinlock_t chrdev_lock;

int cdev_add(const char *dir, const char *name, uint32_t major, uint32_t minor, uint32_t count, uint16_t node_type, uint16_t mode,
             const tmpfs_device_ops_t *ops)
{
    cdev_t *cdev;

    if (!name || !ops) return -EINVAL;

    cdev = calloc(1, sizeof(*cdev));
    if (!cdev) return -ENOMEM;
    if (dir) strncpy(cdev->dir, dir, sizeof(cdev->dir) - 1);
    strncpy(cdev->name, name, sizeof(cdev->name) - 1);
    cdev->major      = major;
    cdev->minor_base = minor;
    cdev->count      = count ? count : 1;
    cdev->node_type  = node_type;
    cdev->mode       = mode;
    cdev->ops        = ops;

    spin_lock(&chrdev_lock);
    cdev->next  = chrdev_list;
    chrdev_list = cdev;
    spin_unlock(&chrdev_lock);
    return 0;
}

int cdev_del(const char *path)
{
    char     full[96];
    cdev_t **link;
    int      status = -ENOENT;

    if (!path) return -EINVAL;
    (void)snprintf(full, sizeof(full), "/dev/%s", path);

    spin_lock(&chrdev_lock);
    link = &chrdev_list;
    while (*link) {
        char entry[96];
        (void)snprintf(entry, sizeof(entry), "/dev/%s%s%s", (*link)->dir, (*link)->dir[0] ? "/" : "", (*link)->name);
        if (streq(entry, full)) {
            cdev_t *victim = *link;
            *link          = victim->next;
            free(victim);
            status = 0;
            break;
        }
        link = &(*link)->next;
    }
    spin_unlock(&chrdev_lock);
    return status;
}

cdev_t *chrdev_lookup(uint32_t major, uint32_t minor)
{
    cdev_t *cdev;

    spin_lock(&chrdev_lock);
    for (cdev = chrdev_list; cdev; cdev = cdev->next) {
        if (cdev->major != major) continue;
        if (minor >= cdev->minor_base && minor < cdev->minor_base + cdev->count) break;
    }
    spin_unlock(&chrdev_lock);
    return cdev;
}

int chrdev_populate(void)
{
    int     count = 0;
    cdev_t *cdev;

    spin_lock(&chrdev_lock);
    for (cdev = chrdev_list; cdev; cdev = cdev->next) {
        char     path[96];
        uint32_t major     = cdev->major;
        uint32_t first     = cdev->minor_base;
        uint32_t n         = cdev->count;
        bool     has_dir   = cdev->dir[0] != '\0';
        uint16_t mode      = cdev->mode;
        uint16_t node_type = cdev->node_type;

        for (uint32_t i = 0; i < n; i++) {
            char numbered[32];

            if (n > 1)
                (void)snprintf(numbered, sizeof(numbered), "%s%u", cdev->name, first + i);
            else
                numbered[0] = '\0';
            const char *leaf = n > 1 ? numbered : cdev->name;
            if (has_dir)
                (void)snprintf(path, sizeof(path), "/dev/%s/%s", cdev->dir, leaf);
            else
                (void)snprintf(path, sizeof(path), "/dev/%s", leaf);

            if (devtmpfs_register_char_device(path, MKDEV(major, first + i), MKDEV(major, first + i), node_type, cdev->ops) != 0) continue;
            if (mode) {
                vfs_node_t node = vfs_open(path);
                if (node) {
                    node->mode = mode;
                    vfs_close(node);
                }
            }
            count++;
        }
    }
    spin_unlock(&chrdev_lock);
    return count;
}

void chrdev_init(void)
{
    memdev_init();
    kmsgdev_init();
}
