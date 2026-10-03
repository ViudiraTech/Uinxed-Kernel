/*
 *
 *      gendisk.c
 *      Block device (gendisk) registry
 *
 *      2026/8/10 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/block/ata/pata/atapi.h>
#include <drivers/block/ata/pata/ide.h>
#include <drivers/block/ata/sata/ahci.h>
#include <drivers/block/core/gendisk.h>
#include <drivers/block/nvme/nvme.h>
#include <fs/devtmpfs/devtmpfs.h>
#include <kernel/errno.h>
#include <libs/std/string.h>
#include <mem/heap.h>

static gendisk_t *gendisk_list;
static spinlock_t gendisk_lock;

/* Register a disk under /dev/<name>, optionally scanning its partitions. */
int block_register_disk(const char *name, uint32_t major, uint32_t minor, const blockdev_device_t *device, bool scan_partitions, bool use_p_separator)
{
    gendisk_t *disk;

    if (!name || !device) return -EINVAL;
    disk = calloc(1, sizeof(*disk));

    if (!disk) return -ENOMEM;
    strncpy(disk->name, name, sizeof(disk->name) - 1);
    disk->major           = major;
    disk->minor_base      = minor;
    disk->device          = *device;
    disk->scan_partitions = scan_partitions;
    disk->use_p_separator = use_p_separator;
    blockdev_retain(device);

    /*
     * Publish the /dev node and its partitions before exposing the disk.
     * A failed publication must fail the whole registration.
     */
    char  dev_path[96];
    dev_t devt = MKDEV(major, minor);
    (void)snprintf(dev_path, sizeof(dev_path), "/dev/%s", name);

    int status = devtmpfs_register_block_device(dev_path, device, devt, devt, scan_partitions, use_p_separator, &disk->devtmpfs);
    if (status != EOK) {
        plogk("gendisk: Cannot publish /dev/%s: %d\n", name, status);
        blockdev_release(&disk->device);
        free(disk);
        return status;
    }

    spin_lock(&gendisk_lock);
    disk->next   = gendisk_list;
    gendisk_list = disk;
    spin_unlock(&gendisk_lock);

    return 0;
}

/* Unregister a disk previously added by block_register_disk(). */
int block_unregister_disk(const char *name)
{
    gendisk_t **link;

    if (!name) return -EINVAL;
    spin_lock(&gendisk_lock);

    link = &gendisk_list;
    while (*link) {
        if (streq((*link)->name, name)) {
            gendisk_t *victim = *link;
            *link             = victim->next;
            spin_unlock(&gendisk_lock);

            /*
             * Remove the /dev nodes and release the extra backend reference
             * devtmpfs holds before dropping the registry's own reference.
             */
            devtmpfs_unregister_block_device(victim->devtmpfs);
            blockdev_release(&victim->device);
            free(victim);
            return 0;
        }
        link = &(*link)->next;
    }
    spin_unlock(&gendisk_lock);
    return -ENOENT;
}

/* Return the number of registered disks. */
int block_disk_count(void)
{
    gendisk_t *disk;
    int        count = 0;

    spin_lock(&gendisk_lock);
    for (disk = gendisk_list; disk; disk = disk->next) count++;
    spin_unlock(&gendisk_lock);
    return count;
}

/* Copy the disk at an index, retaining its backend reference. */
bool block_disk_snapshot(int index, gendisk_t *out)
{
    bool found = false;

    if (!out || index < 0) return false;
    spin_lock(&gendisk_lock);
    gendisk_t *disk = gendisk_list;
    for (int i = 0; disk && i < index; i++) disk = disk->next;
    if (disk) {
        *out      = *disk;
        out->next = NULL;
        blockdev_retain(&out->device);
        found = true;
    }
    spin_unlock(&gendisk_lock);
    return found;
}

/* Invoke cb for each partition of a disk. */
static void gendisk_walk_partitions(const gendisk_t *disk, block_partition_cb_t cb, void *opaque)
{
    partition_table_t table;
    int               status;

    if (!disk->scan_partitions) return;
    status = partition_scan(&disk->device, &table);

    if (status == -ENOENT) return;
    if (status != EOK) {
        plogk("gendisk: Ignoring invalid partition table on %s: %d\n", disk->name, status);
        return;
    }

    bool separator = disk->use_p_separator;

    for (size_t i = 0; i < table.count; i++) {
        const partition_info_t *part = &table.partitions[i];
        blockdev_device_t       view;
        char                    part_name[96];

        if (blockdev_open_partition(&disk->device, part->start_lba, part->sector_count, &view) != EOK) continue;
        if (part->read_only) view.read_only = true;
        (void)snprintf(part_name, sizeof(part_name), "%s%s%u", disk->name, separator ? "p" : "", part->number);
        cb(disk, part_name, disk->major, disk->minor_base + part->number, view.sector_count * view.sector_size / 1024, opaque);
    }
    partition_table_destroy(&table);
}

/* Invoke cb for every partition on all registered disks. */
void block_foreach_partition(block_partition_cb_t cb, void *opaque)
{
    if (!cb) return;
    for (int index = 0;; index++) {
        gendisk_t disk;
        if (!block_disk_snapshot(index, &disk)) break;
        gendisk_walk_partitions(&disk, cb, opaque);
        blockdev_release(&disk.device);
    }
}

/* Discover and register every disk currently exposed by the block backends. Called once after all storage drivers have completed their probe. */
void block_register_all_disks(void)
{
#if CONFIG_ATA
    uint8_t sr_idx = 0;

    /* IDE ATA -> /dev/hdX */
    for (uint8_t drive = 0; drive < 4; drive++) {
        if (!ide_devices[drive].reserved || ide_devices[drive].type != IDE_ATA) continue;
        blockdev_device_t device;
        char              name[8];

        if (blockdev_open_ide(drive, &device) != EOK) {
            plogk("gendisk: Cannot open IDE drive %u\n", drive);
            continue;
        }
        (void)snprintf(name, sizeof(name), "hd%c", 'a' + drive);
        (void)block_register_disk(name, 3, drive, &device, true, false);
    }

    /* IDE ATAPI -> /dev/srX */
    for (uint8_t drive = 0; drive < 4; drive++) {
        if (!atapi_devices[drive].reserved || atapi_devices[drive].type != IDE_ATAPI) continue;
        blockdev_device_t device;
        char              name[8];

        if (blockdev_open_atapi(drive, &device) != EOK) {
            plogk("gendisk: Cannot open IDE ATAPI drive %u\n", drive);
            continue;
        }
        (void)snprintf(name, sizeof(name), "sr%u", sr_idx);
        (void)block_register_disk(name, 11, sr_idx, &device, false, false);

        sr_idx++;
    }

    /* AHCI SATA -> /dev/sdX */
    for (uint8_t d = 0; d < (uint8_t)AHCI_MAX_DEVICES; d++) {
        if (!ahci_devices[d].reserved || ahci_devices[d].type != AHCI_DEV_SATA) continue;
        blockdev_device_t device;
        char              disk_name[16];

        if (blockdev_format_disk_name(disk_name, sizeof(disk_name), d) != EOK) {
            plogk("gendisk: Cannot name AHCI device %u\n", d);
            continue;
        }
        if (blockdev_open_ahci(d, &device) != EOK) {
            plogk("gendisk: Cannot open /dev/%s\n", disk_name);
            continue;
        }
        (void)block_register_disk(disk_name, 8, d, &device, true, false);
    }

    /* AHCI SATAPI -> /dev/srX (continuing IDE numbering) */
    for (uint8_t d = 0; d < (uint8_t)AHCI_MAX_DEVICES; d++) {
        if (!ahci_devices[d].reserved || ahci_devices[d].type != AHCI_DEV_SATAPI) continue;
        blockdev_device_t device;
        char              name[8];

        if (blockdev_open_ahci_atapi(d, &device) != EOK) {
            plogk("gendisk: Cannot open AHCI ATAPI device %u\n", d);
            continue;
        }
        (void)snprintf(name, sizeof(name), "sr%u", sr_idx);
        (void)block_register_disk(name, 11, sr_idx, &device, false, false);

        sr_idx++;
    }
#endif
#if CONFIG_NVME
    /* NVMe namespaces -> /dev/nvme0n1, /dev/nvme0n1p1, ... */
    for (int c = 0; c < nvme_controller_count(); c++) {
        nvme_controller_t *ctrl = nvme_get_controller(c);
        if (!ctrl || !ctrl->initialised) continue;
        for (uint32_t ns = 0; ns < ctrl->num_namespaces; ns++) {
            if (!ctrl->namespaces[ns].ready) continue;
            blockdev_device_t device;
            char              name[64];

            if (blockdev_open_nvme(&ctrl->namespaces[ns], &device) != EOK) {
                plogk("gendisk: Cannot open NVMe namespace %u on controller %d\n", ctrl->namespaces[ns].nsid, ctrl->id);
                continue;
            }
            (void)snprintf(name, sizeof(name), "nvme%dn%u", ctrl->id, ctrl->namespaces[ns].nsid);
            (void)block_register_disk(name, 259, ctrl->namespaces[ns].nsid, &device, true, true);
        }
    }
#endif
}
