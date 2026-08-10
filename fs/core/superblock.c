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
        plogk("superblock: Valid with NULL superblock.\n");
        return -EINVAL;
    }
    if (sb->magic != SUPERBLOCK_MAGIC) {
        plogk("superblock: Invalid magic %#x (expected %#x)\n", sb->magic, SUPERBLOCK_MAGIC);
        return -EINVAL;
    }
    if (!sb->version) {
        plogk("superblock: Zero version.\n");
        return -EINVAL;
    }
    if (!sb->block_size) {
        plogk("superblock: Zero block_size.\n");
        return -EINVAL;
    }
    if (sb->block_size % SUPERBLOCK_BLOCK_SECTOR) {
        plogk("superblock: Block_size %u not sector-aligned.\n", sb->block_size);
        return -EINVAL;
    }
    if (!sb->inode_size) {
        plogk("superblock: Zero inode_size.\n");
        return -EINVAL;
    }
    if (!sb->inode_count) {
        plogk("superblock: Zero inode_count.\n");
        return -EINVAL;
    }
    if (!sb->block_count) {
        plogk("superblock: Zero block_count.\n");
        return -EINVAL;
    }
    if (!sb->root_inode) {
        plogk("superblock: Zero root_inode.\n");
        return -EINVAL;
    }
    if (sb->root_inode > sb->inode_count) {
        plogk("superblock: Root_inode %u exceeds inode_count %u\n", sb->root_inode, sb->inode_count);
        return -EINVAL;
    }
    if (!sb->inode_table_start || !sb->data_block_start) {
        plogk("superblock: Zero inode_table_start/data_block_start (%u/%u)\n", sb->inode_table_start, sb->data_block_start);
        return -EINVAL;
    }
    if (sb->data_block_start < sb->inode_table_start) {
        plogk("superblock: Data_block_start %u below inode_table_start %u\n", sb->data_block_start, sb->inode_table_start);
        return -EINVAL;
    }
    if (sb->data_block_start >= sb->block_count) {
        plogk("superblock: Data_block_start %u beyond block_count %u\n", sb->data_block_start, sb->block_count);
        return -EINVAL;
    }
    return EOK;
}

int superblock_read(uint8_t drive, superblock_disk_t *sb)
{
    blockdev_device_t device;
    int               status;

    if (!sb) {
        plogk("superblock: Read with NULL output buffer.\n");
        return -EINVAL;
    }
    status = blockdev_open_drive(drive, &device);
    if (status != EOK) {
        plogk("superblock: Open drive %u failed (status %d)\n", drive, status);
        return status;
    }

    status = blockdev_read_bytes(&device, (uint64_t)SUPERBLOCK_SECTOR * SUPERBLOCK_BLOCK_SECTOR, sb, sizeof(*sb));
    if (status != EOK) {
        plogk("superblock: Read drive %u failed (status %d)\n", drive, status);
        return status;
    }
    return superblock_valid(sb);
}

int superblock_write(uint8_t drive, const superblock_disk_t *sb)
{
    blockdev_device_t device;
    int               status;

    if (!sb) {
        plogk("superblock: Write with NULL superblock.\n");
        return -EINVAL;
    }
    if (superblock_valid(sb) != EOK) return -EINVAL;
    status = blockdev_open_drive(drive, &device);
    if (status != EOK) {
        plogk("superblock: Open drive %u failed (status %d)\n", drive, status);
        return status;
    }

    status = blockdev_write_bytes(&device, (uint64_t)SUPERBLOCK_SECTOR * SUPERBLOCK_BLOCK_SECTOR, sb, sizeof(*sb));
    if (status != EOK) plogk("superblock: Write drive %u failed (status %d)\n", drive, status);
    return status;
}

int superblock_probe(uint8_t drive)
{
    superblock_disk_t sb;
    return superblock_read(drive, &sb);
}
