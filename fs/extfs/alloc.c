/*
 *
 *      alloc.c
 *      ext2/ext3/ext4 filesystem - block and inode allocation
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

static uint32_t extfs_count_zeros_in_bitmap(extfs_sb_info_t *sb, uint32_t bitmap_block, uint32_t total_bits)
{
    uint8_t *buf;
    uint32_t i, count = 0;
    uint32_t bytes = (total_bits + 7) / 8;

    buf = malloc(sb->block_size);
    if (!buf) return 0;

    if (extfs_read_block(sb, bitmap_block, buf) != EOK) {
        free(buf);
        return 0;
    }

    for (i = 0; i < total_bits && i / 8 < bytes; i++) {
        if (!(buf[i / 8] & (1 << (i % 8)))) count++;
    }

    free(buf);
    return count;
}

static int extfs_test_bit(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

static void extfs_set_bit(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] |= (1 << (bit % 8));
}

static void extfs_clear_bit(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static int extfs_alloc_bit_from_bitmap(extfs_sb_info_t *sb, uint32_t bitmap_block, uint32_t total_bits, uint32_t start, uint32_t *out)
{
    uint8_t *buf;
    uint32_t i;

    buf = malloc(sb->block_size);
    if (!buf) return -ENOMEM;

    if (extfs_read_block(sb, bitmap_block, buf) != EOK) {
        free(buf);
        return -EIO;
    }

    for (i = start; i < total_bits; i++) {
        if (!extfs_test_bit(buf, i)) {
            extfs_set_bit(buf, i);
            if (extfs_write_block(sb, bitmap_block, buf) != EOK) {
                free(buf);
                return -EIO;
            }
            *out = i;
            free(buf);
            return EOK;
        }
    }

    free(buf);
    return -ENOSPC;
}

static void extfs_free_bit_in_bitmap(extfs_sb_info_t *sb, uint32_t bitmap_block, uint32_t bit)
{
    uint8_t *buf;

    buf = malloc(sb->block_size);
    if (!buf) return;

    if (extfs_read_block(sb, bitmap_block, buf) != EOK) {
        free(buf);
        return;
    }

    extfs_clear_bit(buf, bit);
    extfs_write_block(sb, bitmap_block, buf);
    free(buf);
}

int extfs_alloc_block(extfs_sb_info_t *sb, uint32_t goal, uint32_t *out)
{
    uint32_t group, i, bit;
    int      status;

    if (sb->es->s_free_blocks_count == 0) return -ENOSPC;

    /* Try goal group first */
    if (goal > sb->s_first_data_block) {
        group = (goal - sb->s_first_data_block) / sb->blocks_per_group;
        if (group < sb->groups_count && sb->group_desc[group].bg_free_blocks_count > 0) {
            bit    = (goal - sb->s_first_data_block) % sb->blocks_per_group;
            status = extfs_alloc_bit_from_bitmap(sb, sb->group_desc[group].bg_block_bitmap, sb->blocks_per_group, bit, out);
            if (status == EOK) {
                *out += group * sb->blocks_per_group + sb->s_first_data_block;
                sb->group_desc[group].bg_free_blocks_count--;
                sb->es->s_free_blocks_count--;
                extfs_write_group_desc(sb, group, &sb->group_desc[group]);
                return EOK;
            }
        }
    }

    /* Scan all groups */
    for (i = 0; i < sb->groups_count; i++) {
        if (sb->group_desc[i].bg_free_blocks_count == 0) continue;

        status = extfs_alloc_bit_from_bitmap(sb, sb->group_desc[i].bg_block_bitmap, sb->blocks_per_group, 0, out);
        if (status == EOK) {
            *out += i * sb->blocks_per_group + sb->s_first_data_block;
            sb->group_desc[i].bg_free_blocks_count--;
            sb->es->s_free_blocks_count--;
            extfs_write_group_desc(sb, i, &sb->group_desc[i]);
            return EOK;
        }
    }

    return -ENOSPC;
}

void extfs_free_block(extfs_sb_info_t *sb, uint32_t block)
{
    uint32_t group, bit;

    if (block < sb->s_first_data_block) return;

    group = (block - sb->s_first_data_block) / sb->blocks_per_group;
    bit   = (block - sb->s_first_data_block) % sb->blocks_per_group;

    if (group >= sb->groups_count) return;

    extfs_free_bit_in_bitmap(sb, sb->group_desc[group].bg_block_bitmap, bit);
    sb->group_desc[group].bg_free_blocks_count++;
    sb->es->s_free_blocks_count++;
    extfs_write_group_desc(sb, group, &sb->group_desc[group]);
}

int extfs_alloc_inode(extfs_sb_info_t *sb, uint32_t *out)
{
    uint32_t i;
    int      status;

    if (sb->es->s_free_inodes_count == 0) return -ENOSPC;

    for (i = 0; i < sb->groups_count; i++) {
        if (sb->group_desc[i].bg_free_inodes_count == 0) continue;

        status = extfs_alloc_bit_from_bitmap(sb, sb->group_desc[i].bg_inode_bitmap, sb->inodes_per_group, 0, out);
        if (status == EOK) {
            *out += i * sb->inodes_per_group + 1;
            sb->group_desc[i].bg_free_inodes_count--;
            sb->es->s_free_inodes_count--;
            extfs_write_group_desc(sb, i, &sb->group_desc[i]);
            return EOK;
        }
    }

    return -ENOSPC;
}

void extfs_free_inode(extfs_sb_info_t *sb, uint32_t ino)
{
    uint32_t group, bit;

    if (ino == 0) return;

    group = (ino - 1) / sb->inodes_per_group;
    bit   = (ino - 1) % sb->inodes_per_group;

    if (group >= sb->groups_count) return;

    extfs_free_bit_in_bitmap(sb, sb->group_desc[group].bg_inode_bitmap, bit);
    sb->group_desc[group].bg_free_inodes_count++;
    sb->es->s_free_inodes_count++;
    extfs_write_group_desc(sb, group, &sb->group_desc[group]);
}

uint32_t extfs_count_free_blocks(extfs_sb_info_t *sb)
{
    uint32_t i, count = 0;

    for (i = 0; i < sb->groups_count; i++) count += sb->group_desc[i].bg_free_blocks_count;

    return count;
}

uint32_t extfs_count_free_inodes(extfs_sb_info_t *sb)
{
    uint32_t i, count = 0;

    for (i = 0; i < sb->groups_count; i++) count += sb->group_desc[i].bg_free_inodes_count;

    return count;
}
