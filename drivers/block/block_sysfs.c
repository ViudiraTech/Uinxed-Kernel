/*
 *
 *      block_sysfs.c
 *      Block device sysfs integration (/sys/block/)
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/ahci.h>
#include <drivers/blockdev.h>
#include <drivers/ide.h>
#include <drivers/nvme.h>
#include <fs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/kobject.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>

/* ------------------------------------------------------------------ */
/*  Per-block-device wrapper                                           */
/* ------------------------------------------------------------------ */

struct block_sysfs_dev {
    struct kobject     kobj;
    blockdev_device_t  bdev;
    char               name[32];
    int                valid;
};

/* ------------------------------------------------------------------ */
/*  Attribute show functions                                           */
/* ------------------------------------------------------------------ */

static struct block_sysfs_dev *to_bsd(struct kobject *kobj)
{
    return (struct block_sysfs_dev *)((char *)kobj
           - offsetof(struct block_sysfs_dev, kobj));
}

static ssize_t size_show(struct kobject *kobj, struct attribute *attr,
                         char *buf)
{
    struct block_sysfs_dev *bsd = to_bsd(kobj);
    (void)attr;
    if (!bsd->valid) return -EIO;
    uint64_t sz = (uint64_t)bsd->bdev.sector_count * bsd->bdev.sector_size;
    return (ssize_t)sysfs_emit(buf, "%llu\n", (unsigned long long)sz);
}

static ssize_t sector_size_show(struct kobject *kobj, struct attribute *attr,
                                char *buf)
{
    struct block_sysfs_dev *bsd = to_bsd(kobj);
    (void)attr;
    if (!bsd->valid) return -EIO;
    return (ssize_t)sysfs_emit(buf, "%u\n", bsd->bdev.sector_size);
}

static ssize_t ro_show(struct kobject *kobj, struct attribute *attr,
                       char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "0\n");
}

static ssize_t removable_show(struct kobject *kobj, struct attribute *attr,
                              char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "0\n");
}

/* ------------------------------------------------------------------ */
/*  Sysfs ops                                                          */
/* ------------------------------------------------------------------ */

static ssize_t block_attr_show(struct kobject *kobj, struct attribute *attr,
                               char *buf)
{
    if (streq(attr->name, "size"))         return size_show(kobj, attr, buf);
    if (streq(attr->name, "sector_size"))  return sector_size_show(kobj, attr, buf);
    if (streq(attr->name, "ro"))           return ro_show(kobj, attr, buf);
    if (streq(attr->name, "removable"))    return removable_show(kobj, attr, buf);
    return -EIO;
}

static const struct sysfs_ops block_sysfs_ops = {
    .show  = block_attr_show,
    .store = NULL,
};

/* ------------------------------------------------------------------ */
/*  Attributes and kobj_type                                           */
/* ------------------------------------------------------------------ */

static struct attribute size_attr        = __ATTR_RO(size);
static struct attribute sector_size_attr = __ATTR_RO(sector_size);
static struct attribute ro_attr          = __ATTR_RO(ro);
static struct attribute removable_attr   = __ATTR_RO(removable);

static struct attribute *block_attrs[] = {
    &size_attr,
    &sector_size_attr,
    &ro_attr,
    &removable_attr,
    NULL,
};

static void block_kobj_release(struct kobject *kobj)
{
    struct block_sysfs_dev *bsd = to_bsd(kobj);
    free(bsd);
}

static struct kobj_type block_ktype = {
    .release       = block_kobj_release,
    .sysfs_ops     = &block_sysfs_ops,
    .default_attrs = block_attrs,
};

/* ------------------------------------------------------------------ */
/*  Helper: add a single block device                                  */
/* ------------------------------------------------------------------ */

static int block_add_one(struct kobject *parent, const char *name,
                          uint8_t drive, int type, void *ns_ptr)
{
    struct block_sysfs_dev *bsd;
    blockdev_device_t       bdev;
    int                     ret;

    memset(&bdev, 0, sizeof(bdev));

    switch (type) {
        case 0:
            ret = blockdev_open_ide(drive, &bdev);
            break;
        case 1:
            ret = blockdev_open_ahci(drive, &bdev);
            break;
        case 2:
            ret = blockdev_open_nvme(ns_ptr, &bdev);
            break;
        default:
            return -EINVAL;
    }

    if (ret != EOK) return ret;

    bsd = calloc(1, sizeof(*bsd));
    if (!bsd) return -ENOMEM;

    memcpy(&bsd->bdev, &bdev, sizeof(bdev));
    strncpy(bsd->name, name, sizeof(bsd->name) - 1);
    bsd->valid = 1;

    kobject_init(&bsd->kobj, &block_ktype);
    ret = kobject_add(&bsd->kobj, parent, "%s", name);
    if (ret != EOK) {
        free(bsd);
        return ret;
    }
    return EOK;
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                     */
/* ------------------------------------------------------------------ */

void block_sysfs_init(void)
{
    extern struct kobject *sysfs_root_kobj;
    struct kobject        *block_kobj = NULL;
    clist_t                node;
    int                    count = 0;

    if (!sysfs_root_kobj) return;

    for (node = sysfs_root_kobj->children; node; node = node->next) {
        struct kobject *child = node->data;
        if (child && child->name && streq(child->name, "block")) {
            block_kobj = child;
            break;
        }
    }

    if (!block_kobj) {
        plogk("block_sysfs: /sys/block/ kobject not found\n");
        return;
    }

    for (uint8_t d = 0; d < 4; d++) {
        if (!ide_devices[d].reserved || ide_devices[d].type != IDE_ATA) continue;
        char name[8];
        snprintf(name, sizeof(name), "hd%c", 'a' + d);
        block_add_one(block_kobj, name, d, 0, NULL);
        count++;
    }

    for (uint8_t d = 0; d < AHCI_MAX_DEVICES; d++) {
        if (!ahci_devices[d].reserved || ahci_devices[d].type != AHCI_DEV_SATA) continue;
        char name[8];
        snprintf(name, sizeof(name), "sd%c", 'a' + d);
        block_add_one(block_kobj, name, d, 1, NULL);
        count++;
    }

    for (int c = 0; c < nvme_controller_count(); c++) {
        nvme_controller_t *ctrl = nvme_get_controller(c);
        if (!ctrl || !ctrl->initialised) continue;
        for (uint32_t ns = 0; ns < ctrl->num_namespaces; ns++) {
            if (!ctrl->namespaces[ns].ready) continue;
            char name[24];
            snprintf(name, sizeof(name), "nvme%dn%u", ctrl->id, ctrl->namespaces[ns].nsid);
            block_add_one(block_kobj, name, (uint8_t)ctrl->id, 2, &ctrl->namespaces[ns]);
            count++;
        }
    }

    plogk("block_sysfs: %d block device entries created under /sys/block/\n", count);
}
