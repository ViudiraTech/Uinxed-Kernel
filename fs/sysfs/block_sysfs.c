/*
 *
 *      block_sysfs.c
 *      Block device sysfs integration (/sys/block/)
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/ata/pata/ide.h>
#include <drivers/ata/sata/ahci.h>
#include <drivers/block/blockdev.h>
#include <drivers/block/partition.h>
#include <drivers/nvme/nvme.h>
#include <fs/sysfs/block_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/kobject/kobject.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <proc/process.h>

/* ------------------------------------------------------------------ */
/*  Per-block-device wrapper                                           */
/* ------------------------------------------------------------------ */

typedef struct block_sysfs_dev {
        struct kobject    kobj;
        blockdev_device_t bdev;
        char              name[32];
        uint32_t          partition;
        uint64_t          start_lba;
        int               read_only;
        int               removable;
        int               valid;
} block_sysfs_dev_t;

static struct kobject *block_root_kobj;

/* ------------------------------------------------------------------ */
/*  Attribute show functions                                           */
/* ------------------------------------------------------------------ */

static block_sysfs_dev_t *to_bsd(struct kobject *kobj)
{
    return (block_sysfs_dev_t *)((char *)kobj - offsetof(block_sysfs_dev_t, kobj));
}

static void block_sysfs_dev_publish(block_sysfs_dev_t *bsd);
static void block_sysfs_dev_unpublish(block_sysfs_dev_t *bsd);

static ssize_t size_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    block_sysfs_dev_t *bsd = to_bsd(kobj);
    (void)attr;
    if (!bsd->valid) return -EIO;
    uint64_t sz = bsd->bdev.sector_count * (bsd->bdev.sector_size / 512);
    return (ssize_t)sysfs_emit(buf, "%llu\n", (unsigned long long)sz);
}

static ssize_t sector_size_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    block_sysfs_dev_t *bsd = to_bsd(kobj);
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
    block_sysfs_dev_t *bsd = to_bsd(kobj);
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "%llu\n", (unsigned long long)bsd->start_lba * (bsd->bdev.sector_size / 512));
}

static ssize_t removable_show(struct kobject *kobj, struct attribute *attr, char *buf)
{
    (void)attr;
    return (ssize_t)sysfs_emit(buf, "%d\n", to_bsd(kobj)->removable);
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
    if (streq(attr->name, "uevent")) {
        struct kobj_uevent_env env = {0};
        block_sysfs_dev_t     *bsd = to_bsd(kobj);
        int                    ret = add_uevent_var(&env, "DEVNAME=%s", bsd->name);
        if (!ret) ret = add_uevent_var(&env, "DEVTYPE=%s", bsd->partition ? "partition" : "disk");
        if (ret) return ret;
        int at = 0;
        for (int i = 0; i < env.envp_idx; i++) at += sysfs_emit_at(buf, at, "%s\n", env.envp[i]);
        return at;
    }
    return -EIO;
}

static ssize_t block_attr_store(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)
{
    if (!streq(attr->name, "uevent")) return -EIO;
    process_t *process = process_current();
    if (!process || process->uid != 0) return -EPERM;
    int ret = kobject_synth_uevent(kobj, buf, count);
    return ret ? ret : (ssize_t)count;
}

static const struct sysfs_ops block_sysfs_ops = {
    .show  = block_attr_show,
    .store = block_attr_store,
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
static struct attribute uevent_attr      = __ATTR(uevent, 0644);

static struct attribute *block_attrs[] = {
    &size_attr, &sector_size_attr, &ro_attr, &removable_attr, &uevent_attr, NULL,
};

static struct attribute *partition_attrs[] = {
    &size_attr, &partition_attr, &start_attr, &ro_attr, &uevent_attr, NULL,
};

static void block_kobj_release(struct kobject *kobj)
{
    block_sysfs_dev_t *bsd = to_bsd(kobj);
    free(bsd);
}

static const char *block_uevent_name(struct kobject *kobj)
{
    (void)kobj;
    return "block";
}

static int block_kobj_uevent(struct kobject *kobj, struct kobj_uevent_env *env)
{
    block_sysfs_dev_t *bsd = to_bsd(kobj);
    int                ret = add_uevent_var(env, "DEVNAME=%s", bsd->name);
    if (ret) return ret;
    return add_uevent_var(env, "DEVTYPE=%s", bsd->partition ? "partition" : "disk");
}

static struct kobj_type block_ktype = {
    .release       = block_kobj_release,
    .sysfs_ops     = &block_sysfs_ops,
    .default_attrs = block_attrs,
    .uevent_name   = block_uevent_name,
    .uevent        = block_kobj_uevent,
};

static struct kobj_type partition_ktype = {
    .release       = block_kobj_release,
    .sysfs_ops     = &block_sysfs_ops,
    .default_attrs = partition_attrs,
    .uevent_name   = block_uevent_name,
    .uevent        = block_kobj_uevent,
};

static int block_add_partitions(block_sysfs_dev_t *disk)
{
    partition_table_t table;
    int               status;

    status = partition_scan(&disk->bdev, &table);
    if (status == -ENOENT) return EOK;
    if (status != EOK) return status;

    for (size_t i = 0; i < table.count; i++) {
        const partition_info_t *info = &table.partitions[i];
        block_sysfs_dev_t      *part = calloc(1, sizeof(*part));
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
            (void)snprintf(part->name, sizeof(part->name), "%sp%u", disk->name, info->number);
        else
            (void)snprintf(part->name, sizeof(part->name), "%s%u", disk->name, info->number);

        kobject_init(&part->kobj, &partition_ktype);
        status = kobject_add(&part->kobj, &disk->kobj, "%s", part->name);
        if (status != EOK) {
            kobject_put(&part->kobj);
            break;
        }
        kobject_uevent(&part->kobj, KOBJ_ADD);
        block_sysfs_dev_publish(part);
    }
    partition_table_destroy(&table);
    return status;
}

/* ------------------------------------------------------------------ */
/*  Helper: add a single block device                                  */
/* ------------------------------------------------------------------ */

/*
 * Map a Linux disk/partition name ("sda", "nvme0n1p2", "hdb", "sr1", ...)
 * to the conventional (major, minor) pair used by /sys/dev/block.
 */
static void block_sysfs_devt(const char *name, uint32_t *major, uint32_t *minor)
{
    const char *n  = name;
    uint32_t    ma = 8, mi = 0;

    if (!strncmp(n, "/dev/", 5)) n += 5;

    if (!strncmp(n, "hd", 2)) {
        ma = 3;
        mi = n[2] >= 'a' ? (uint32_t)(n[2] - 'a') : 0;
        mi += (uint32_t)strtol(n + 3, NULL, 10);
    } else if (!strncmp(n, "sr", 2)) {
        ma = 11;
        mi = (uint32_t)strtol(n + 2, NULL, 10);
    } else if (!strncmp(n, "nvme", 4)) {
        const char *cursor = n + 4;
        ma                 = 259;
        while (*cursor >= '0' && *cursor <= '9') cursor++; // controller
        if (*cursor == 'n') cursor++;
        uint32_t    nsid = (uint32_t)strtol(cursor, NULL, 10);
        const char *part = strchr(cursor, 'p');
        mi               = part ? (nsid << 4) | (uint32_t)strtol(part + 1, NULL, 10) : nsid;
    } else { // sdX, sdaa, ...
        const char *cursor = n + 2;
        uint32_t    index  = 0;
        ma                 = 8;
        while (*cursor >= 'a' && *cursor <= 'z') {
            index = index * 26 + (uint32_t)(*cursor - 'a') + 1;
            cursor++;
        }
        mi = index ? index - 1 : 0;
        mi += (uint32_t)strtol(cursor, NULL, 10);
    }

    *major = ma;
    *minor = mi;
}

static void block_sysfs_dev_publish(block_sysfs_dev_t *bsd)
{
    uint32_t               major, minor;
    char                   link[24];
    extern struct kobject *sysfs_dev_block_kobj;
    if (!bsd || !bsd->name[0]) return;
    block_sysfs_devt(bsd->name, &major, &minor);
    (void)snprintf(link, sizeof(link), "%u:%u", major, minor);
    if (sysfs_dev_block_kobj) (void)sysfs_create_symlink(sysfs_dev_block_kobj, &bsd->kobj, link);
}

static void block_sysfs_dev_unpublish(block_sysfs_dev_t *bsd)
{
    uint32_t               major, minor;
    char                   link[24];
    extern struct kobject *sysfs_dev_block_kobj;
    if (!bsd || !bsd->name[0]) return;
    block_sysfs_devt(bsd->name, &major, &minor);
    (void)snprintf(link, sizeof(link), "%u:%u", major, minor);
    if (sysfs_dev_block_kobj) sysfs_remove_symlink(sysfs_dev_block_kobj, link);
}

static int block_add_one(struct kobject *parent, const char *name, uint8_t drive, int type, void *ns_ptr)
{
    block_sysfs_dev_t *bsd;
    blockdev_device_t  bdev;
    int                ret;

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
    block_sysfs_dev_publish(bsd);
    ret = block_add_partitions(bsd);
    if (ret != EOK && ret != -ENOENT) plogk("block_sysfs: Cannot scan partitions on %s: %d\n", name, ret);
    return EOK;
}

int block_sysfs_register_device(const char *name, const blockdev_device_t *device, bool removable, block_sysfs_dev_t **handle)
{
#if CONFIG_SYSFS
    block_sysfs_dev_t *bsd;
    int                status;

    if (!name || !device || !handle || !block_root_kobj) return -EINVAL;
    *handle = NULL;
    bsd     = calloc(1, sizeof(*bsd));
    if (!bsd) return -ENOMEM;
    bsd->bdev      = *device;
    bsd->read_only = device->read_only;
    bsd->removable = removable;
    bsd->valid     = 1;
    strncpy(bsd->name, name, sizeof(bsd->name) - 1);
    kobject_init(&bsd->kobj, &block_ktype);
    status = kobject_add(&bsd->kobj, block_root_kobj, "%s", name);
    if (status != EOK) {
        kobject_put(&bsd->kobj);
        return status;
    }
    kobject_uevent(&bsd->kobj, KOBJ_ADD);
    block_sysfs_dev_publish(bsd);
    status = block_add_partitions(bsd);
    if (status != EOK && status != -ENOENT) plogk("block_sysfs: Cannot scan partitions on %s: %d\n", name, status);
    *handle = bsd;
    return EOK;
#else
    (void)name;
    (void)device;
    (void)removable;
    (void)handle;
    return -ENOSYS;
#endif
}

void block_sysfs_unregister_device(block_sysfs_dev_t *handle)
{
#if CONFIG_SYSFS
    if (!handle) return;
    handle->valid = 0;
    while (handle->kobj.children) {
        block_sysfs_dev_t *part = to_bsd(handle->kobj.children->data);
        part->valid             = 0;
        block_sysfs_dev_unpublish(part);
        kobject_del(&part->kobj);
        kobject_put(&part->kobj);
    }
    block_sysfs_dev_unpublish(handle);
    kobject_del(&handle->kobj);
    kobject_put(&handle->kobj);
#else
    (void)handle;
#endif
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                     */
/* ------------------------------------------------------------------ */

void block_sysfs_init(void)
{
#if CONFIG_SYSFS
    extern struct kobject *sysfs_root_kobj;
    clist_t                node;
    int                    count = 0;

    if (!sysfs_root_kobj) return;

    for (node = sysfs_root_kobj->children; node; node = node->next) {
        struct kobject *child = node->data;
        if (child && child->name && streq(child->name, "block")) {
            block_root_kobj = child;
            break;
        }
    }

    if (!block_root_kobj) {
        plogk("block_sysfs: /sys/block/ kobject not found.\n");
        return;
    }
    for (uint8_t d = 0; d < 4; d++) {
        if (!ide_devices[d].reserved || ide_devices[d].type != IDE_ATA) continue;
        char name[8];
        (void)snprintf(name, sizeof(name), "hd%c", 'a' + d);
        block_add_one(block_root_kobj, name, d, 0, NULL);
        count++;
    }

    for (uint8_t d = 0; d < AHCI_MAX_DEVICES; d++) {
        if (!ahci_devices[d].reserved || ahci_devices[d].type != AHCI_DEV_SATA) continue;
        char name[16];
        if (blockdev_format_disk_name(name, sizeof(name), d) != EOK) continue;
        block_add_one(block_root_kobj, name, d, 1, NULL);
        count++;
    }

    for (int c = 0; c < nvme_controller_count(); c++) {
        nvme_controller_t *ctrl = nvme_get_controller(c);
        if (!ctrl || !ctrl->initialised) continue;
        for (uint32_t ns = 0; ns < ctrl->num_namespaces; ns++) {
            if (!ctrl->namespaces[ns].ready) continue;
            char name[24];
            (void)snprintf(name, sizeof(name), "nvme%dn%u", ctrl->id, ctrl->namespaces[ns].nsid);
            block_add_one(block_root_kobj, name, (uint8_t)ctrl->id, 2, &ctrl->namespaces[ns]);
            count++;
        }
    }

    plogk("block_sysfs: %d block devices exported to /sys/block/\n", count);
#endif
}
