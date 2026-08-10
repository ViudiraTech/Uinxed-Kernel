/*
 *
 *      gendisk.c
 *      Block device (gendisk) registry
 *
 *      2026/8/10 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/block/ata/pata/atapi.h>
#include <drivers/block/ata/pata/ide.h>
#include <drivers/block/ata/sata/ahci.h>
#include <drivers/block/ata/sata/satapi.h>
#include <drivers/block/core/gendisk.h>
#include <drivers/block/core/partition.h>
#include <drivers/block/nvme/nvme.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

static gendisk_t *gendisk_list;
static spinlock_t gendisk_lock;

int block_register_disk(const char *name, uint32_t major, uint32_t minor, const blockdev_device_t *device, bool scan_partitions,
                        bool use_p_separator)
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

    spin_lock(&gendisk_lock);
    disk->next   = gendisk_list;
    gendisk_list = disk;
    spin_unlock(&gendisk_lock);
    return 0;
}

int block_unregister_disk(const char *name)
{
    gendisk_t **link;
    int         status = -ENOENT;

    if (!name) return -EINVAL;
    spin_lock(&gendisk_lock);
    link = &gendisk_list;
    while (*link) {
        if (streq((*link)->name, name)) {
            gendisk_t *victim = *link;
            *link             = victim->next;
            blockdev_release(&victim->device);
            free(victim);
            status = 0;
            break;
        }
        link = &(*link)->next;
    }
    spin_unlock(&gendisk_lock);
    return status;
}

int block_disk_count(void)
{
    gendisk_t *disk;
    int        count = 0;

    spin_lock(&gendisk_lock);
    for (disk = gendisk_list; disk; disk = disk->next) count++;
    spin_unlock(&gendisk_lock);
    return count;
}

gendisk_t *block_get_disk(int index)
{
    gendisk_t *disk;
    int        i = 0;

    spin_lock(&gendisk_lock);
    for (disk = gendisk_list; disk && i < index; disk = disk->next) i++;
    spin_unlock(&gendisk_lock);
    return disk;
}

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

void block_foreach_partition(block_partition_cb_t cb, void *opaque)
{
    gendisk_t *disk;

    if (!cb) return;
    spin_lock(&gendisk_lock);
    for (disk = gendisk_list; disk; disk = disk->next) gendisk_walk_partitions(disk, cb, opaque);
    spin_unlock(&gendisk_lock);
}

/*
 * Discover and register every disk currently exposed by the block backends.
 * Called once after all storage drivers have completed their probe.
 */
void block_register_all_disks(void)
{
    uint8_t sr_idx = 0;

#if CONFIG_ATA
    /* IDE ATA -> /dev/hdX */
    for (uint8_t drive = 0; drive < 4; drive++) {
        if (!ide_devices[drive].reserved || ide_devices[drive].type != IDE_ATA) continue;
        blockdev_device_t device;
        char              name[8];
        if (blockdev_open_ide(drive, &device) != EOK) continue;
        (void)snprintf(name, sizeof(name), "hd%c", 'a' + drive);
        (void)block_register_disk(name, 3, drive, &device, true, false);
    }

    /* IDE ATAPI -> /dev/srX */
    for (uint8_t drive = 0; drive < 4; drive++) {
        if (!atapi_devices[drive].reserved || atapi_devices[drive].type != IDE_ATAPI) continue;
        blockdev_device_t device;
        char              name[8];
        if (blockdev_open_atapi(drive, &device) != EOK) continue;
        (void)snprintf(name, sizeof(name), "sr%u", sr_idx);
        (void)block_register_disk(name, 11, sr_idx, &device, false, false);
        sr_idx++;
    }

    /* AHCI SATA -> /dev/sdX */
    for (uint8_t d = 0; d < (uint8_t)AHCI_MAX_DEVICES; d++) {
        if (!ahci_devices[d].reserved || ahci_devices[d].type != AHCI_DEV_SATA) continue;
        blockdev_device_t device;
        char              disk_name[16];
        if (blockdev_format_disk_name(disk_name, sizeof(disk_name), d) != EOK) continue;
        if (blockdev_open_ahci(d, &device) != EOK) continue;
        (void)block_register_disk(disk_name, 8, d, &device, true, false);
    }

    /* AHCI SATAPI -> /dev/srX (continuing IDE numbering) */
    for (uint8_t d = 0; d < (uint8_t)AHCI_MAX_DEVICES; d++) {
        if (!ahci_devices[d].reserved || ahci_devices[d].type != AHCI_DEV_SATAPI) continue;
        blockdev_device_t device;
        char              name[8];
        if (blockdev_open_ahci_atapi(d, &device) != EOK) continue;
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
            if (blockdev_open_nvme(&ctrl->namespaces[ns], &device) != EOK) continue;
            (void)snprintf(name, sizeof(name), "nvme%dn%u", ctrl->id, ctrl->namespaces[ns].nsid);
            (void)block_register_disk(name, 259, ctrl->namespaces[ns].nsid, &device, true, true);
        }
    }
#endif
}
