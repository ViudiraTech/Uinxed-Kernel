/*
 *
 *      alloc.c
 *      ext2/ext3/ext4 filesystem - block and inode allocation
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/extfs/extfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <process/sched.h>

/*
 * Block and inode allocation
 * Bitmap-based allocation for data blocks and inode numbers: search
 * the block/inode bitmaps for a free slot, mark it and update the
 * on-disk group descriptors.
 */

/* Test a bit in a bitmap. */
static int extfs_test_bit(const uint8_t *bitmap, uint32_t bit)
{
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

/* Set a bit in a bitmap. */
static void extfs_set_bit(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] |= (1 << (bit % 8));
}

/* Clear a bit in a bitmap. */
static void extfs_clear_bit(uint8_t *bitmap, uint32_t bit)
{
    bitmap[bit / 8] &= ~(1 << (bit % 8));
}

static int extfs_initialize_block_bitmap(extfs_sb_info_t *sb, uint32_t group, uint8_t *bitmap);
static int extfs_initialize_inode_bitmap(extfs_sb_info_t *sb, uint32_t group, uint8_t *bitmap);

/* Find a free bit in a block/inode bitmap, initializing it if needed. */
static int extfs_alloc_bit_from_bitmap(extfs_sb_info_t *sb, uint32_t group, int inode_bitmap, uint32_t bitmap_block, uint32_t total_bits, uint32_t start, uint32_t *out)
{
    uint8_t *buf;
    uint32_t i;

    buf = malloc(sb->block_size);
    if (!buf) return -ENOMEM;

    if (extfs_read_block(sb, bitmap_block, buf) != EOK) {
        free(buf);
        return -EIO;
    }

    uint16_t uninit = inode_bitmap ? EXT4_BG_INODE_UNINIT : EXT4_BG_BLOCK_UNINIT;
    if (sb->group_desc[group].bg_flags & uninit) {
        int status = inode_bitmap ? extfs_initialize_inode_bitmap(sb, group, buf) : extfs_initialize_block_bitmap(sb, group, buf);
        if (status != EOK) {
            free(buf);
            return status;
        }
    }

    for (i = start; i < total_bits; i++) {
        if (!extfs_test_bit(buf, i)) {
            extfs_set_bit(buf, i);
            extfs_update_bitmap_checksum(sb, group, inode_bitmap, buf);
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

/* Clear a bit in a block/inode bitmap, detecting double frees. */
static int extfs_free_bit_in_bitmap(extfs_sb_info_t *sb, uint32_t group, int inode_bitmap, uint32_t bitmap_block, uint32_t bit)
{
    uint8_t *buf;

    buf = malloc(sb->block_size);
    if (!buf) return -ENOMEM;

    if (extfs_read_block(sb, bitmap_block, buf) != EOK) {
        free(buf);
        return -EIO;
    }

    if (!extfs_test_bit(buf, bit)) {
        plogk("extfs: Drive %u: %s bitmap bit %u not allocated (double free)\n", sb->device.drive, inode_bitmap ? "inode" : "block", bit);
        free(buf);
        return -EINVAL;
    }
    extfs_clear_bit(buf, bit);
    extfs_update_bitmap_checksum(sb, group, inode_bitmap, buf);
    int status = extfs_write_block(sb, bitmap_block, buf);
    free(buf);
    return status;
}

/* Number of valid data blocks in a group (the last group may be short). */
static uint32_t extfs_blocks_in_group(extfs_sb_info_t *sb, uint32_t group)
{
    uint64_t first = (uint64_t)sb->s_first_data_block + (uint64_t)group * sb->blocks_per_group;
    uint64_t total = sb->blocks_count;
    if (first >= total) return 0;
    uint64_t remaining = total - first;
    return remaining < sb->blocks_per_group ? (uint32_t)remaining : sb->blocks_per_group;
}

/* Number of valid inodes in a group (the last group may be short). */
static uint32_t extfs_inodes_in_group(extfs_sb_info_t *sb, uint32_t group)
{
    uint64_t first = (uint64_t)group * sb->inodes_per_group;
    uint64_t total = sb->es->s_inodes_count;
    if (first >= total) return 0;
    uint64_t remaining = total - first;
    return remaining < sb->inodes_per_group ? (uint32_t)remaining : sb->inodes_per_group;
}

/* Whether value is a power of base. */
static int extfs_is_power(uint32_t value, uint32_t base)
{
    if (!value) return 0;
    while (value % base == 0) value /= base;
    return value == 1;
}

/* Whether a group carries a superblock backup for sparse-super layouts. */
static int extfs_group_has_super(extfs_sb_info_t *sb, uint32_t group)
{
    if (sb->es->s_feature_compat & EXT4_FEATURE_COMPAT_SPARSE_SUPER2) {
        uint32_t backup[2];
        memcpy(backup, (uint8_t *)sb->es + 588, sizeof(backup));
        return group == 0 || group == backup[0] || group == backup[1];
    }
    if (!(sb->es->s_feature_ro_compat & EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER)) return 1;
    return group <= 1 || extfs_is_power(group, 3) || extfs_is_power(group, 5) || extfs_is_power(group, 7);
}

/* Mark a physical block as allocated in a group bitmap. */
static void extfs_mark_group_block(extfs_sb_info_t *sb, uint32_t group, uint8_t *bitmap, uint64_t physical)
{
    uint64_t first = (uint64_t)sb->s_first_data_block + (uint64_t)group * sb->blocks_per_group;
    if (physical >= first && physical < first + sb->blocks_per_group) extfs_set_bit(bitmap, (uint32_t)(physical - first));
}

/* Rebuild a block bitmap, marking metadata blocks as allocated. */
static int extfs_initialize_block_bitmap(extfs_sb_info_t *sb, uint32_t group, uint8_t *bitmap)
{
    memset(bitmap, 0, sb->block_size);
    uint64_t first = (uint64_t)sb->s_first_data_block + (uint64_t)group * sb->blocks_per_group;
    if (extfs_group_has_super(sb, group)) {
        uint32_t overhead = 1 + sb->gdb_count + sb->es->s_reserved_gdt_blocks;
        for (uint32_t block = 0; block < overhead; block++) extfs_mark_group_block(sb, group, bitmap, first + block);
    }
    uint32_t inode_table_blocks = (sb->inodes_per_group * sb->inode_size + sb->block_size - 1) / sb->block_size;
    for (uint32_t other = 0; other < sb->groups_count; other++) {
        ext2_group_desc_t *desc = &sb->group_desc[other];
        extfs_mark_group_block(sb, group, bitmap, desc->bg_block_bitmap);
        extfs_mark_group_block(sb, group, bitmap, desc->bg_inode_bitmap);
        for (uint32_t block = 0; block < inode_table_blocks; block++) extfs_mark_group_block(sb, group, bitmap, (uint64_t)desc->bg_inode_table + block);
    }
    uint32_t valid = extfs_blocks_in_group(sb, group);
    for (uint32_t bit = valid; bit < sb->block_size * 8; bit++) extfs_set_bit(bitmap, bit);
    sb->group_desc[group].bg_flags &= (uint16_t)~EXT4_BG_BLOCK_UNINIT;
    return extfs_update_bitmap_checksum(sb, group, 0, bitmap);
}

/* Rebuild an inode bitmap, zeroing the inode table if not yet done. */
static int extfs_initialize_inode_bitmap(extfs_sb_info_t *sb, uint32_t group, uint8_t *bitmap)
{
    if (!(sb->group_desc[group].bg_flags & EXT4_BG_INODE_ZEROED)) {
        uint8_t *zero = calloc(1, sb->block_size);
        if (!zero) return -ENOMEM;
        uint32_t blocks = (extfs_inodes_in_group(sb, group) * sb->inode_size + sb->block_size - 1) / sb->block_size;
        for (uint32_t block = 0; block < blocks; block++) {
            int status = extfs_write_block(sb, sb->group_desc[group].bg_inode_table + block, zero);
            if (status != EOK) {
                free(zero);
                return status;
            }
        }
        free(zero);
        sb->group_desc[group].bg_flags |= EXT4_BG_INODE_ZEROED;
    }
    memset(bitmap, 0, sb->block_size);
    uint32_t valid = extfs_inodes_in_group(sb, group);
    for (uint32_t bit = valid; bit < sb->block_size * 8; bit++) extfs_set_bit(bitmap, bit);
    if (group == 0)
        for (uint32_t bit = 0; bit + 1 < sb->s_first_ino && bit < valid; bit++) extfs_set_bit(bitmap, bit);
    sb->group_desc[group].bg_flags &= (uint16_t)~EXT4_BG_INODE_UNINIT;
    return extfs_update_bitmap_checksum(sb, group, 1, bitmap);
}

/* Allocate a data block, preferring the group around the goal block. */
int extfs_alloc_block(extfs_sb_info_t *sb, uint32_t goal, uint32_t *out)
{
    uint32_t group, i, bit;
    int      status;

    if (!sb || !sb->es || !out) return -EINVAL;
    if (sb->read_only) return -EROFS;
    *out = 0;
    if (sb->es->s_free_blocks_count == 0) {
        static uint64_t last_log;
        if (sched_ticks() - last_log >= 1000) {
            plogk("extfs: Drive %u: filesystem full (no free blocks)\n", sb->device.drive);
            last_log = sched_ticks();
        }
        return -ENOSPC;
    }

    /* Try goal group first */
    if (goal > sb->s_first_data_block) {
        group = (goal - sb->s_first_data_block) / sb->blocks_per_group;
        if (group < sb->groups_count && sb->group_desc[group].bg_free_blocks_count > 0) {
            bit    = (goal - sb->s_first_data_block) % sb->blocks_per_group;
            status = extfs_alloc_bit_from_bitmap(sb, group, 0, sb->group_desc[group].bg_block_bitmap, extfs_blocks_in_group(sb, group), bit, out);
            if (status == EOK) {
                *out += group * sb->blocks_per_group + sb->s_first_data_block;
                sb->group_desc[group].bg_free_blocks_count--;
                sb->es->s_free_blocks_count--;
                status = extfs_write_group_desc(sb, group, &sb->group_desc[group]);
                if (status == EOK) status = extfs_write_super(sb);
                return status;
            }
        }
    }

    /* Scan all groups */
    for (i = 0; i < sb->groups_count; i++) {
        if (sb->group_desc[i].bg_free_blocks_count == 0) continue;
        status = extfs_alloc_bit_from_bitmap(sb, i, 0, sb->group_desc[i].bg_block_bitmap, extfs_blocks_in_group(sb, i), 0, out);
        if (status == EOK) {
            *out += i * sb->blocks_per_group + sb->s_first_data_block;
            sb->group_desc[i].bg_free_blocks_count--;
            sb->es->s_free_blocks_count--;
            status = extfs_write_group_desc(sb, i, &sb->group_desc[i]);
            if (status == EOK) status = extfs_write_super(sb);
            return status;
        }
    }

    static uint64_t last_log;
    if (sched_ticks() - last_log >= 1000) {
        plogk("extfs: Drive %u: block allocation failed, filesystem full.\n", sb->device.drive);
        last_log = sched_ticks();
    }
    return -ENOSPC;
}

/* Free a data block back to its group bitmap. */
void extfs_free_block(extfs_sb_info_t *sb, uint32_t block)
{
    uint32_t group, bit;

    if (!sb || !sb->es || sb->read_only || block < sb->s_first_data_block) return;

    group = (block - sb->s_first_data_block) / sb->blocks_per_group;
    bit   = (block - sb->s_first_data_block) % sb->blocks_per_group;

    if (group >= sb->groups_count) return;

    if (bit >= extfs_blocks_in_group(sb, group)) return;
    int status = extfs_free_bit_in_bitmap(sb, group, 0, sb->group_desc[group].bg_block_bitmap, bit);
    if (status != EOK) {
        /* active_transaction is valid for the whole commit: it is cleared only after fs_txn_commit() has freed every buffer, still under the lock. */
        fs_txn_log_lock(&sb->transaction_log);
        if (sb->active_transaction) sb->active_transaction->error = status;
        fs_txn_log_unlock(&sb->transaction_log);
        return;
    }
    sb->group_desc[group].bg_free_blocks_count++;
    sb->es->s_free_blocks_count++;
    status = extfs_write_group_desc(sb, group, &sb->group_desc[group]);
    if (status == EOK) status = extfs_write_super(sb);
    fs_txn_log_lock(&sb->transaction_log);
    if (status != EOK && sb->active_transaction) sb->active_transaction->error = status;
    fs_txn_log_unlock(&sb->transaction_log);
}

/* Allocate an inode number from the first group with a free slot. */
int extfs_alloc_inode(extfs_sb_info_t *sb, uint32_t *out)
{
    uint32_t i;
    int      status;

    if (!sb || !sb->es || !out) return -EINVAL;
    if (sb->read_only) return -EROFS;
    *out = 0;
    if (sb->es->s_free_inodes_count == 0) {
        static uint64_t last_log;
        if (sched_ticks() - last_log >= 1000) {
            plogk("extfs: Drive %u: inode table exhausted (no free inodes)\n", sb->device.drive);
            last_log = sched_ticks();
        }
        return -ENOSPC;
    }

    for (i = 0; i < sb->groups_count; i++) {
        if (sb->group_desc[i].bg_free_inodes_count == 0) continue;

        uint32_t start = i == 0 && sb->s_first_ino > 1 ? sb->s_first_ino - 1 : 0;
        status         = extfs_alloc_bit_from_bitmap(sb, i, 1, sb->group_desc[i].bg_inode_bitmap, extfs_inodes_in_group(sb, i), start, out);
        if (status == EOK) {
            *out += i * sb->inodes_per_group + 1;
            sb->group_desc[i].bg_free_inodes_count--;
            sb->group_desc[i].bg_flags &= (uint16_t)~EXT4_BG_INODE_UNINIT;
            uint32_t unused = sb->group_desc[i].bg_itable_unused_lo | (uint32_t)sb->group_desc[i].bg_itable_unused_hi << 16;
            uint32_t after  = extfs_inodes_in_group(sb, i) - ((*out - 1) % sb->inodes_per_group + 1);
            if (after < unused) {
                sb->group_desc[i].bg_itable_unused_lo = (uint16_t)after;
                sb->group_desc[i].bg_itable_unused_hi = (uint16_t)(after >> 16);
            }
            sb->es->s_free_inodes_count--;
            status = extfs_write_group_desc(sb, i, &sb->group_desc[i]);
            if (status == EOK) status = extfs_write_super(sb);
            return status;
        }
    }

    static uint64_t last_log;
    if (sched_ticks() - last_log >= 1000) {
        plogk("extfs: Drive %u: inode allocation failed, inode table full.\n", sb->device.drive);
        last_log = sched_ticks();
    }
    return -ENOSPC;
}

/* Free an inode number back to its group bitmap. */
void extfs_free_inode(extfs_sb_info_t *sb, uint32_t ino)
{
    uint32_t group, bit;

    if (!sb || !sb->es || sb->read_only || ino == 0) return;

    group = (ino - 1) / sb->inodes_per_group;
    bit   = (ino - 1) % sb->inodes_per_group;

    if (group >= sb->groups_count) return;

    if (ino < sb->s_first_ino || bit >= extfs_inodes_in_group(sb, group)) return;
    int status = extfs_free_bit_in_bitmap(sb, group, 1, sb->group_desc[group].bg_inode_bitmap, bit);
    if (status != EOK) {
        /* active_transaction is valid for the whole commit: it is cleared only after fs_txn_commit() has freed every buffer, still under the lock. */
        fs_txn_log_lock(&sb->transaction_log);
        if (sb->active_transaction) sb->active_transaction->error = status;
        fs_txn_log_unlock(&sb->transaction_log);
        return;
    }
    sb->group_desc[group].bg_free_inodes_count++;
    sb->es->s_free_inodes_count++;
    status = extfs_write_group_desc(sb, group, &sb->group_desc[group]);
    if (status == EOK) status = extfs_write_super(sb);
    fs_txn_log_lock(&sb->transaction_log);
    if (status != EOK && sb->active_transaction) sb->active_transaction->error = status;
    fs_txn_log_unlock(&sb->transaction_log);
}

/* Adjust the used-directory counter of an inode's group. */
int extfs_adjust_used_dirs(extfs_sb_info_t *sb, uint32_t ino, int delta)
{
    if (!sb || !sb->es || !ino) return -EINVAL;
    if (sb->read_only) return -EROFS;
    uint32_t group = (ino - 1) / sb->inodes_per_group;
    if (group >= sb->groups_count) return -EINVAL;
    uint32_t count = sb->group_desc[group].bg_used_dirs_count | (uint32_t)sb->group_desc[group].bg_used_dirs_count_hi << 16;
    if ((delta < 0 && count < (uint32_t)-delta) || (delta > 0 && count > UINT32_MAX - (uint32_t)delta)) return -EOVERFLOW;
    count                                       = (uint32_t)(count + delta);
    sb->group_desc[group].bg_used_dirs_count    = (uint16_t)count;
    sb->group_desc[group].bg_used_dirs_count_hi = (uint16_t)(count >> 16);
    return extfs_write_group_desc(sb, group, &sb->group_desc[group]);
}

/* Sum the free-block counters across all groups. */
uint32_t extfs_count_free_blocks(extfs_sb_info_t *sb)
{
    uint32_t i, count = 0;

    if (!sb) return 0;
    for (i = 0; i < sb->groups_count; i++) count += sb->group_desc[i].bg_free_blocks_count;

    return count;
}

/* Sum the free-inode counters across all groups. */
uint32_t extfs_count_free_inodes(extfs_sb_info_t *sb)
{
    uint32_t i, count = 0;

    if (!sb) return 0;
    for (i = 0; i < sb->groups_count; i++) count += sb->group_desc[i].bg_free_inodes_count;

    return count;
}
