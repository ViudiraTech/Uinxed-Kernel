/*
 *
 *      devtmpfs.c
 *      Device tmpfs population helpers
 *
 *      2026/5/20 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <devtmpfs.h>

#include <blockdev.h>
#include <errno.h>
#include <ide.h>
#include <printk.h>
#include <tmpfs.h>
#include <vfs.h>

typedef struct mbr_partition_entry {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t first_lba;
    uint32_t sectors;
} __attribute__((packed)) mbr_partition_entry_t;

typedef struct mbr_sector {
    uint8_t               boot_code[446];
    mbr_partition_entry_t partitions[4];
    uint16_t              signature;
} __attribute__((packed)) mbr_sector_t;

static char devtmpfs_drive_letter(uint8_t drive)
{ return (char)('a' + drive); }

static int devtmpfs_scan_mbr(uint8_t drive, mbr_partition_entry_t parts[4])
{
    blockdev_device_t device;
    mbr_sector_t      mbr;
    int               status;

    status = blockdev_open_ide(drive, &device);
    if (status != EOK) return status;
    status = blockdev_read_bytes(&device, 0, &mbr, sizeof(mbr));
    if (status != EOK) return status;
    if (mbr.signature != 0xAA55) return -ENOENT;

    for (int i = 0; i < 4; i++) parts[i] = mbr.partitions[i];
    return EOK;
}

static void devtmpfs_create_block_node(uint8_t drive)
{
    char       dev_path[] = "/dev/hda";
    vfs_node_t node;
    int        status;

    dev_path[7] = devtmpfs_drive_letter(drive);
    status      = vfs_mkfile(dev_path);
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create %s: %d\n", dev_path, status);
        return;
    }

    node = vfs_open(dev_path);
    if (!node) {
        plogk("devtmpfs: Cannot open %s after creation.\n", dev_path);
        return;
    }

    node->type = file_block;
    node->blksz = 512;
    node->dev  = drive;
    node->rdev = drive;
    node->size = (uint64_t)ide_devices[drive].size * 512;
    plogk("devtmpfs: Registered %s as block device.\n", dev_path);
}

static void devtmpfs_create_partition_node(uint8_t drive, uint8_t part_index, const mbr_partition_entry_t *part)
{
    char       dev_path[] = "/dev/hda1";
    vfs_node_t node;
    int        status;

    dev_path[7] = devtmpfs_drive_letter(drive);
    dev_path[8] = (char)('1' + part_index);
    status      = vfs_mkfile(dev_path);
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create %s: %d\n", dev_path, status);
        return;
    }

    node = vfs_open(dev_path);
    if (!node) {
        plogk("devtmpfs: Cannot open %s after creation.\n", dev_path);
        return;
    }

    node->type = file_block;
    node->blksz = 512;
    node->dev  = drive;
    node->rdev = ((uint64_t)drive << 8) | (part_index + 1);
    node->size = (uint64_t)part->sectors * 512;
    plogk("devtmpfs: Registered %s for partition type 0x%02x, start %u, sectors %u.\n", dev_path, part->type, part->first_lba, part->sectors);
}

void devtmpfs_init(void)
{
    int status;
    vfs_node_t dev = 0;

    status = vfs_mkdir("/dev");
    if (status != EOK && status != -EEXIST) {
        plogk("devtmpfs: Cannot create /dev: %d\n", status);
        return;
    }

    dev = vfs_open("/dev");
    if (!dev) {
        plogk("devtmpfs: Cannot open /dev after creation.\n");
        return;
    }

    status = vfs_mount(0, dev);
    vfs_close(dev);
    if (status != EOK) {
        plogk("devtmpfs: Cannot mount tmpfs at /dev: %d\n", status);
        return;
    }

    for (uint8_t drive = 0; drive < 4; drive++) {
        mbr_partition_entry_t parts[4] = {0};

        if (!ide_devices[drive].reserved || ide_devices[drive].type != IDE_ATA) continue;

        devtmpfs_create_block_node(drive);

        if (devtmpfs_scan_mbr(drive, parts) == EOK) {
            for (uint8_t part = 0; part < 4; part++) {
                if (!parts[part].type || !parts[part].sectors) continue;
                devtmpfs_create_partition_node(drive, part, &parts[part]);
            }
        }
    }
}
