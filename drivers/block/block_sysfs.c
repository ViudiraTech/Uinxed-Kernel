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
#include <drivers/partition.h>
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
        struct kobject    kobj;
        blockdev_device_t bdev;
        char              name[32];
        uint32_t          partition;
        uint64_t          start_lba;
        int               read_only;
        int               valid;
};

/* ------------------------------------------------------------------ */
/*  Attribute show functions                                           */
/* ------------------------------------------------------------------ */

static struct block_sysfs_dev *to_bsd(struct kobject *kobj)
{
    return (struct block_sysfs_dev *)((char *)kobj - offsetof(struct block_sysfs_dev, kobj));
}

static ssize_t size_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    struct block_sysfs_dev *bsd = to_bsd(kobj);
    (void)attr;
    if (!bsd->valid) return -EIO;
    uint64_t sz = bsd->bdev.sector_count * (bsd->bdev.sector_size / 512);
    return (ssize_t)sysfs_emit(buf, "%llu\n", (unsigned long long)sz);
}

static ssize_t sector_size_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    struct block_sysfs_dev *bsd = to_bsd(kobj);
    (void)attr;
    if (!bsd->valid) return -EIO;
    return (ssize_t)sysfs_emit(buf, "%u\n", bsd->bdev.sector_size);
}

static ssize_t ro_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "%d\n", to_bsd(kobj)->read_only);
}

static ssize_t partition_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "%u\n", to_bsd(kobj)->partition);
}

static ssize_t start_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    struct block_sysfs_dev *bsd = to_bsd(kobj);
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "%llu\n", (unsigned long long)(bsd->start_lba * (bsd->bdev.sector_size / 512)));
}

static ssize_t removable_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)kobj;
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "0\n");
}

/* ------------------------------------------------------------------ */
/*  Sysfs ops                                                          */
/* ------------------------------------------------------------------ */

static ssize_t block_attr_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    if (streq(attr->name, "size")) return size_show(kobj, attr, buf);
    if (streq(attr->name, "sector_size")) return sector_size_show(kobj, attr, buf);
    if (streq(attr->name, "ro")) return ro_show(kobj, attr, buf);
    if (streq(attr->name, "removable")) return removable_show(kobj, attr, buf);
    if (streq(attr->name, "partition")) return partition_show(kobj, attr, buf);
    if (streq(attr->name, "start")) return start_show(kobj, attr, buf);
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
static struct attribute partition_attr   = __ATTR_RO(partition);
static struct attribute start_attr       = __ATTR_RO(start);

static struct attribute *block_attrs[] = {
    &size_attr, &sector_size_attr, &ro_attr, &removable_attr, NULL,
};

static struct attribute *partition_attrs[] = {
    &size_attr, &partition_attr, &start_attr, &ro_attr, NULL,
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

static struct kobj_type partition_ktype = {
    .release       = block_kobj_release,
    .sysfs_ops     = &block_sysfs_ops,
    .default_attrs = partition_attrs,
};

static int block_add_partitions(struct block_sysfs_dev *disk)
{
    partition_table_t table;
    int               status;

    status = partition_scan(&disk->bdev, &table);
    if (status == -ENOENT) return EOK;
    if (status != EOK) return status;

    for (size_t i = 0; i < table.count; i++) {
        const partition_info_t *info = &table.partitions[i];
        struct block_sysfs_dev *part = calloc(1, sizeof(*part));
        if (!part) {
            status = -ENOMEM;
            break;
        }
        status = blockdev_open_partition(&disk->bdev, info->start_lba, info->sector_count, &part->bdev);
        if (status != EOK) {
            free(part);
            break;
        }
        part->partition = info->number;
        part->start_lba = info->start_lba;
        part->read_only = info->read_only;
        part->valid     = 1;
        if (disk->name[strlen(disk->name) - 1] >= '0' && disk->name[strlen(disk->name) - 1] <= '9')
            snprintf(part->name, sizeof(part->name), "%sp%u", disk->name, info->number);
        else
            snprintf(part->name, sizeof(part->name), "%s%u", disk->name, info->number);

        kobject_init(&part->kobj, &partition_ktype);
        status = kobject_add(&part->kobj, &disk->kobj, "%s", part->name);
        if (status != EOK) {
            kobject_put(&part->kobj);
            break;
        }
        kobject_uevent(&part->kobj, KOBJ_ADD);
    }
    partition_table_destroy(&table);
    return status;
}

/* ------------------------------------------------------------------ */
/*  Helper: add a single block device                                  */
/* ------------------------------------------------------------------ */

static int block_add_one(struct kobject *parent, const char *name, uint8_t drive, int type, void *ns_ptr)
{
    struct block_sysfs_dev *bsd;
    blockdev_device_t       bdev;
    int                     ret;

    memset(&bdev, 0, sizeof(bdev));

    switch (type) {
        case 0 :
            ret = blockdev_open_ide(drive, &bdev);
            break;
        case 1 :
            ret = blockdev_open_ahci(drive, &bdev);
            break;
        case 2 :
            ret = blockdev_open_nvme(ns_ptr, &bdev);
            break;
        default :
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
        kobject_put(&bsd->kobj);
        return ret;
    }
    kobject_uevent(&bsd->kobj, KOBJ_ADD);
    ret = block_add_partitions(bsd);
    if (ret != EOK && ret != -ENOENT) plogk("block_sysfs: Cannot scan partitions on %s: %d\n", name, ret);
    return EOK;
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                     */
/* ------------------------------------------------------------------ */

void block_sysfs_init(void)
{
#if CONFIG_SYSFS
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
        plogk("block_sysfs: /sys/block/ kobject not found.\n");
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
        char name[16];
        if (blockdev_format_disk_name(name, sizeof(name), d) != EOK) continue;
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

    plogk("block_sysfs: %d block devices exported to /sys/block/\n", count);
#endif
}
