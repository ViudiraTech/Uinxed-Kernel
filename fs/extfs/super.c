/*
 *
 *      super.c
 *      ext2/ext3/ext4 filesystem - superblock handling
 *
 *      Copyright (C) 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/extfs/extfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>

static uint64_t extfs_block_offset(extfs_sb_info_t *sb, uint32_t block)
{
    return (uint64_t)block * sb->block_size;
}

static int extfs_disk_read(extfs_sb_info_t *sb, uint64_t offset, void *buf, size_t size)
{
    return blockdev_read_bytes(&sb->device, offset, buf, size);
}

static int extfs_disk_write(extfs_sb_info_t *sb, uint64_t offset, const void *buf, size_t size)
{
    return blockdev_write_bytes(&sb->device, offset, buf, size);
}

int extfs_read_block(extfs_sb_info_t *sb, uint32_t phys_block, void *buf)
{
    return extfs_disk_read(sb, extfs_block_offset(sb, phys_block), buf, sb->block_size);
}

int extfs_write_block(extfs_sb_info_t *sb, uint32_t phys_block, const void *buf)
{
    return extfs_disk_write(sb, extfs_block_offset(sb, phys_block), buf, sb->block_size);
}

int extfs_read_inode_raw(extfs_sb_info_t *sb, uint32_t ino, ext2_inode_t *raw)
{
    uint32_t group, offset, block;
    uint64_t byte_offset;

    if (ino == 0) return -EINVAL;

    group       = (ino - 1) / sb->inodes_per_group;
    offset      = (ino - 1) % sb->inodes_per_group;
    block       = sb->group_desc[group].bg_inode_table + (offset * sb->inode_size) / sb->block_size;
    byte_offset = extfs_block_offset(sb, block) + (offset * sb->inode_size) % sb->block_size;

    return extfs_disk_read(sb, byte_offset, raw, sb->inode_size);
}

int extfs_write_inode_raw(extfs_sb_info_t *sb, uint32_t ino, const ext2_inode_t *raw)
{
    uint32_t group, offset, block;
    uint64_t byte_offset;

    if (ino == 0) return -EINVAL;

    group       = (ino - 1) / sb->inodes_per_group;
    offset      = (ino - 1) % sb->inodes_per_group;
    block       = sb->group_desc[group].bg_inode_table + (offset * sb->inode_size) / sb->block_size;
    byte_offset = extfs_block_offset(sb, block) + (offset * sb->inode_size) % sb->block_size;

    return extfs_disk_write(sb, byte_offset, raw, sb->inode_size);
}

int extfs_read_group_desc(extfs_sb_info_t *sb, uint32_t group, ext2_group_desc_t *desc)
{
    uint32_t desc_block, desc_offset;
    uint64_t byte_offset;

    desc_block  = sb->s_first_data_block + 1 + group / sb->desc_per_block;
    desc_offset = group % sb->desc_per_block;
    byte_offset = extfs_block_offset(sb, desc_block) + desc_offset * sizeof(ext2_group_desc_t);

    return extfs_disk_read(sb, byte_offset, desc, sizeof(ext2_group_desc_t));
}

int extfs_write_group_desc(extfs_sb_info_t *sb, uint32_t group, const ext2_group_desc_t *desc)
{
    uint32_t desc_block, desc_offset;
    uint64_t byte_offset;

    desc_block  = sb->s_first_data_block + 1 + group / sb->desc_per_block;
    desc_offset = group % sb->desc_per_block;
    byte_offset = extfs_block_offset(sb, desc_block) + desc_offset * sizeof(ext2_group_desc_t);

    return extfs_disk_write(sb, byte_offset, desc, sizeof(ext2_group_desc_t));
}

int extfs_read_super(extfs_sb_info_t *sb, uint8_t drive)
{
    int      status;
    uint32_t i;

    memset(sb, 0, sizeof(*sb));

    status = blockdev_open_drive(drive, &sb->device);
    if (status != EOK) return status;

    sb->es = calloc(1, sizeof(ext2_super_block_t));
    if (!sb->es) return -ENOMEM;

    status = extfs_disk_read(sb, 1024, sb->es, sizeof(ext2_super_block_t));
    if (status != EOK) {
        free(sb->es);
        return -EIO;
    }

    if (sb->es->s_magic != 0xEF53) {
        free(sb->es);
        return -EINVAL;
    }

    sb->log_block_size = sb->es->s_log_block_size;
    sb->block_size     = EXT2_MIN_BLOCK_SIZE << sb->log_block_size;

    sb->s_first_data_block = sb->es->s_first_data_block;
    sb->blocks_per_group   = sb->es->s_blocks_per_group;
    sb->inodes_per_group   = sb->es->s_inodes_per_group;
    sb->inode_size         = sb->es->s_inode_size;
    sb->s_first_ino        = sb->es->s_first_ino;

    if (sb->inode_size == 0) sb->inode_size = EXT2_GOOD_OLD_INODE_SIZE;

    if (sb->s_first_ino == 0) sb->s_first_ino = EXT2_GOOD_OLD_FIRST_INO;

    sb->groups_count = (sb->es->s_blocks_count - sb->s_first_data_block + sb->blocks_per_group - 1) / sb->blocks_per_group;

    sb->desc_per_block = sb->block_size / sizeof(ext2_group_desc_t);

    sb->gdb_count = (sb->groups_count + sb->desc_per_block - 1) / sb->desc_per_block;

    sb->group_desc = calloc(sb->gdb_count * sb->desc_per_block, sizeof(ext2_group_desc_t));
    if (!sb->group_desc) {
        free(sb->es);
        return -ENOMEM;
    }

    for (i = 0; i < sb->gdb_count; i++) {
        uint32_t blk       = sb->s_first_data_block + 1 + i;
        uint64_t off       = extfs_block_offset(sb, blk);
        uint32_t count     = sb->desc_per_block;
        uint32_t remaining = sb->groups_count - i * sb->desc_per_block;
        if (remaining < count) count = remaining;

        status = extfs_disk_read(sb, off, &sb->group_desc[i * sb->desc_per_block], count * sizeof(ext2_group_desc_t));
        if (status != EOK) {
            free(sb->group_desc);
            free(sb->es);
            return -EIO;
        }
    }

    return EOK;
}

int extfs_write_super(extfs_sb_info_t *sb)
{
    if (!sb || !sb->es) return -EINVAL;
    return extfs_disk_write(sb, 1024, sb->es, sizeof(ext2_super_block_t));
}

void extfs_free_super(extfs_sb_info_t *sb)
{
    if (!sb) return;
    if (sb->group_desc) free(sb->group_desc);
    if (sb->es) free(sb->es);
    memset(sb, 0, sizeof(*sb));
}
