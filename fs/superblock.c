/*
 *
 *      superblock.c
 *      Superblock metadata access helpers
 *
 *      2026/5/18 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <errno.h>
#include <blockdev.h>
#include <heap.h>
#include <superblock.h>

int superblock_valid(const superblock_disk_t *sb)
{
    if (!sb) return -EINVAL;
    if (sb->magic != SUPERBLOCK_MAGIC) return -EINVAL;
    if (!sb->version) return -EINVAL;
    if (!sb->block_size) return -EINVAL;
    if (sb->block_size % SUPERBLOCK_BLOCK_SECTOR) return -EINVAL;
    if (!sb->inode_size) return -EINVAL;
    if (!sb->inode_count) return -EINVAL;
    if (!sb->block_count) return -EINVAL;
    if (!sb->root_inode) return -EINVAL;
    if (sb->root_inode > sb->inode_count) return -EINVAL;
    if (!sb->inode_table_start || !sb->data_block_start) return -EINVAL;
    if (sb->data_block_start < sb->inode_table_start) return -EINVAL;
    if (sb->data_block_start >= sb->block_count) return -EINVAL;
    return EOK;
}

int superblock_read(uint8_t drive, superblock_disk_t *sb)
{
    blockdev_device_t device;
    int status;

    if (!sb) return -EINVAL;
    status = blockdev_open_ide(drive, &device);
    if (status != EOK) return status;

    status = blockdev_read_bytes(&device, (uint64_t)SUPERBLOCK_SECTOR * SUPERBLOCK_BLOCK_SECTOR, sb, sizeof(*sb));
    if (status != EOK) return status;
    return superblock_valid(sb);
}

int superblock_write(uint8_t drive, const superblock_disk_t *sb)
{
    blockdev_device_t device;
    int               status;

    if (!sb) return -EINVAL;
    if (superblock_valid(sb) != EOK) return -EINVAL;
    status = blockdev_open_ide(drive, &device);
    if (status != EOK) return status;

    return blockdev_write_bytes(&device, (uint64_t)SUPERBLOCK_SECTOR * SUPERBLOCK_BLOCK_SECTOR, sb, sizeof(*sb));
}

int superblock_probe(uint8_t drive)
{
    superblock_disk_t sb;
    return superblock_read(drive, &sb);
}
