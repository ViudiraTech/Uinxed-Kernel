/*
 *
 *      superblock.c
 *      Superblock metadata access helpers
 *
 *      2026/5/18 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/block/blockdev.h>
#include <fs/core/superblock.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <mem/heap.h>

int superblock_valid(const superblock_disk_t *sb)
{
    if (!sb) {
        plogk("superblock: valid with NULL superblock.\n");
        return -EINVAL;
    }
    if (sb->magic != SUPERBLOCK_MAGIC) {
        plogk("superblock: invalid magic %#x (expected %#x)\n", sb->magic, SUPERBLOCK_MAGIC);
        return -EINVAL;
    }
    if (!sb->version) {
        plogk("superblock: zero version.\n");
        return -EINVAL;
    }
    if (!sb->block_size) {
        plogk("superblock: zero block_size.\n");
        return -EINVAL;
    }
    if (sb->block_size % SUPERBLOCK_BLOCK_SECTOR) {
        plogk("superblock: block_size %u not sector-aligned.\n", sb->block_size);
        return -EINVAL;
    }
    if (!sb->inode_size) {
        plogk("superblock: zero inode_size.\n");
        return -EINVAL;
    }
    if (!sb->inode_count) {
        plogk("superblock: zero inode_count.\n");
        return -EINVAL;
    }
    if (!sb->block_count) {
        plogk("superblock: zero block_count.\n");
        return -EINVAL;
    }
    if (!sb->root_inode) {
        plogk("superblock: zero root_inode.\n");
        return -EINVAL;
    }
    if (sb->root_inode > sb->inode_count) {
        plogk("superblock: root_inode %u exceeds inode_count %u\n", sb->root_inode, sb->inode_count);
        return -EINVAL;
    }
    if (!sb->inode_table_start || !sb->data_block_start) {
        plogk("superblock: zero inode_table_start/data_block_start (%u/%u)\n", sb->inode_table_start, sb->data_block_start);
        return -EINVAL;
    }
    if (sb->data_block_start < sb->inode_table_start) {
        plogk("superblock: data_block_start %u below inode_table_start %u\n", sb->data_block_start, sb->inode_table_start);
        return -EINVAL;
    }
    if (sb->data_block_start >= sb->block_count) {
        plogk("superblock: data_block_start %u beyond block_count %u\n", sb->data_block_start, sb->block_count);
        return -EINVAL;
    }
    return EOK;
}

int superblock_read(uint8_t drive, superblock_disk_t *sb)
{
    blockdev_device_t device;
    int               status;

    if (!sb) {
        plogk("superblock: read with NULL output buffer.\n");
        return -EINVAL;
    }
    status = blockdev_open_drive(drive, &device);
    if (status != EOK) {
        plogk("superblock: open drive %u failed (status %d)\n", drive, status);
        return status;
    }

    status = blockdev_read_bytes(&device, (uint64_t)SUPERBLOCK_SECTOR * SUPERBLOCK_BLOCK_SECTOR, sb, sizeof(*sb));
    if (status != EOK) {
        plogk("superblock: read drive %u failed (status %d)\n", drive, status);
        return status;
    }
    return superblock_valid(sb);
}

int superblock_write(uint8_t drive, const superblock_disk_t *sb)
{
    blockdev_device_t device;
    int               status;

    if (!sb) {
        plogk("superblock: write with NULL superblock.\n");
        return -EINVAL;
    }
    if (superblock_valid(sb) != EOK) return -EINVAL;
    status = blockdev_open_drive(drive, &device);
    if (status != EOK) {
        plogk("superblock: open drive %u failed (status %d)\n", drive, status);
        return status;
    }

    status = blockdev_write_bytes(&device, (uint64_t)SUPERBLOCK_SECTOR * SUPERBLOCK_BLOCK_SECTOR, sb, sizeof(*sb));
    if (status != EOK) plogk("superblock: write drive %u failed (status %d)\n", drive, status);
    return status;
}

int superblock_probe(uint8_t drive)
{
    superblock_disk_t sb;
    return superblock_read(drive, &sb);
}
