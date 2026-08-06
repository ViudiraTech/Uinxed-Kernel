/*
 *
 *      super.c
 *      ext2/ext3/ext4 filesystem - superblock handling
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/extfs/extfs.h>
#include <fs/extfs/jbd2.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/data/crc32c.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>

int extfs_detect_version(const ext2_super_block_t *es)
{
    const uint32_t ext4_compat   = EXT4_FEATURE_COMPAT_SPARSE_SUPER2;
    const uint32_t ext4_incompat = EXT4_FEATURE_INCOMPAT_EXTENTS | EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_FLEX_BG
                                   | EXT4_FEATURE_INCOMPAT_CSUM_SEED | EXT4_FEATURE_INCOMPAT_LARGEDIR;
    const uint32_t ext4_ro = EXT4_FEATURE_RO_COMPAT_HUGE_FILE | EXT4_FEATURE_RO_COMPAT_GDT_CSUM | EXT4_FEATURE_RO_COMPAT_DIR_NLINK
                             | EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE | EXT4_FEATURE_RO_COMPAT_METADATA_CSUM;

    if (!es) return 0;
    if ((es->s_feature_compat & ext4_compat) || (es->s_feature_incompat & ext4_incompat) || (es->s_feature_ro_compat & ext4_ro)) return 4;
    if ((es->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL)
        || (es->s_feature_incompat & (EXT3_FEATURE_INCOMPAT_RECOVER | EXT3_FEATURE_INCOMPAT_JOURNAL_DEV)))
        return 3;
    return 2;
}

static uint64_t extfs_block_offset(extfs_sb_info_t *sb, uint32_t block)
{
    return (uint64_t)block * sb->block_size;
}

static int extfs_disk_read(extfs_sb_info_t *sb, uint64_t offset, void *buf, size_t size)
{
    uint8_t *out = buf;
    if (sb->active_transaction && sb->block_size) {
        uint8_t *block = malloc(sb->block_size);
        if (!block) return -ENOMEM;
        while (size) {
            uint64_t logical = offset / sb->block_size;
            uint32_t within  = (uint32_t)(offset % sb->block_size);
            size_t   chunk   = size < sb->block_size - within ? size : sb->block_size - within;
            int      status  = fs_txn_read(sb->active_transaction, logical, block);
            if (status != EOK) {
                free(block);
                return status;
            }
            memcpy(out, block + within, chunk);
            out += chunk;
            offset += chunk;
            size -= chunk;
        }
        free(block);
        return EOK;
    }
    int status = blockdev_read_bytes(&sb->device, offset, buf, size);
    if (status != EOK)
        plogk("extfs: drive %u: block read failed at byte %llu (size %llu): %d\n", sb->device.drive, (unsigned long long)offset,
              (unsigned long long)size, status);
    return status;
}

static uint32_t extfs_super_u32(const ext2_super_block_t *super, size_t offset)
{
    uint32_t value;
    memcpy(&value, (const uint8_t *)super + offset, sizeof(value));
    return value;
}

static void extfs_super_put_u32(ext2_super_block_t *super, size_t offset, uint32_t value)
{
    memcpy((uint8_t *)super + offset, &value, sizeof(value));
}

static uint16_t extfs_crc16(uint16_t crc, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    while (size--) {
        crc ^= *bytes++;
        for (uint32_t bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ (uint16_t)(0xA001U & (uint16_t) - (int16_t)(crc & 1));
    }
    return crc;
}

static uint16_t extfs_group_desc_checksum(extfs_sb_info_t *sb, uint32_t group, const ext2_group_desc_t *desc)
{
    const uint8_t *bytes           = (const uint8_t *)desc;
    size_t         checksum_offset = offsetof(ext2_group_desc_t, bg_checksum);
    size_t         checksum_end    = checksum_offset + sizeof(desc->bg_checksum);
    uint16_t       result          = 0;

    if (sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        static const uint8_t zero[2]  = {0};
        uint32_t             checksum = crc32c_update(sb->checksum_seed, &group, sizeof(group));
        checksum                      = crc32c_update(checksum, bytes, checksum_offset);
        checksum                      = crc32c_update(checksum, &zero, sizeof(zero));
        if (checksum_end < sb->desc_size) checksum = crc32c_update(checksum, bytes + checksum_end, sb->desc_size - checksum_end);
        result = (uint16_t)checksum;
    } else if (sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_GDT_CSUM) {
        result = extfs_crc16((uint16_t)~0U, sb->es->s_uuid, sizeof(sb->es->s_uuid));
        result = extfs_crc16(result, &group, sizeof(group));
        result = extfs_crc16(result, bytes, checksum_offset);
        if (checksum_end < sb->desc_size) result = extfs_crc16(result, bytes + checksum_end, sb->desc_size - checksum_end);
    }
    return result;
}

static int extfs_inode_has_checksum_hi(extfs_sb_info_t *sb, const uint8_t *inode)
{
    uint16_t extra = 0;
    if (sb->inode_size <= 131) return 0;
    memcpy(&extra, inode + 128, sizeof(extra));
    return extra >= 4;
}

static uint32_t extfs_inode_checksum(extfs_sb_info_t *sb, uint32_t ino, uint8_t *inode)
{
    uint16_t checksum_lo, checksum_hi = 0;
    uint32_t generation;
    memcpy(&checksum_lo, inode + 124, sizeof(checksum_lo));
    memset(inode + 124, 0, sizeof(checksum_lo));
    int has_hi = extfs_inode_has_checksum_hi(sb, inode);
    if (has_hi) {
        memcpy(&checksum_hi, inode + 130, sizeof(checksum_hi));
        memset(inode + 130, 0, sizeof(checksum_hi));
    }
    memcpy(&generation, inode + 100, sizeof(generation));
    uint32_t checksum = crc32c_update(sb->checksum_seed, &ino, sizeof(ino));
    checksum          = crc32c_update(checksum, &generation, sizeof(generation));
    checksum          = crc32c_update(checksum, inode, sb->inode_size);
    memcpy(inode + 124, &checksum_lo, sizeof(checksum_lo));
    if (has_hi) memcpy(inode + 130, &checksum_hi, sizeof(checksum_hi));
    return checksum;
}

int extfs_update_bitmap_checksum(extfs_sb_info_t *sb, uint32_t group, int inode_bitmap, const void *bitmap)
{
    if (!sb || !bitmap || group >= sb->groups_count) return -EINVAL;
    if (!(sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) return EOK;
    ext2_group_desc_t *desc     = &sb->group_desc[group];
    size_t             bytes    = inode_bitmap ? (sb->inodes_per_group + 7) / 8 : (sb->blocks_per_group + 7) / 8;
    uint32_t           checksum = crc32c_update(sb->checksum_seed, bitmap, bytes);
    if (inode_bitmap) {
        desc->bg_inode_bitmap_csum_lo = (uint16_t)checksum;
        if (sb->desc_size >= 64) desc->bg_inode_bitmap_csum_hi = (uint16_t)(checksum >> 16);
    } else {
        desc->bg_block_bitmap_csum_lo = (uint16_t)checksum;
        if (sb->desc_size >= 64) desc->bg_block_bitmap_csum_hi = (uint16_t)(checksum >> 16);
    }
    return EOK;
}

static int extfs_disk_write(extfs_sb_info_t *sb, uint64_t offset, const void *buf, size_t size)
{
    const uint8_t *input = buf;
    if (sb->active_transaction && sb->block_size) {
        uint8_t *block = malloc(sb->block_size);
        if (!block) {
            plogk("extfs: drive %u: transaction write buffer allocation failed.\n", sb->device.drive);
            return -ENOMEM;
        }
        while (size) {
            uint64_t logical = offset / sb->block_size;
            uint32_t within  = (uint32_t)(offset % sb->block_size);
            size_t   chunk   = size < sb->block_size - within ? size : sb->block_size - within;
            int      status  = fs_txn_read(sb->active_transaction, logical, block);
            if (status == EOK) {
                memcpy(block + within, input, chunk);
                status = fs_txn_stage(sb->active_transaction, logical, block, FS_TXN_METADATA);
            }
            if (status != EOK) {
                plogk("extfs: drive %u: transaction write failed at block %llu (%d)\n", sb->device.drive, (unsigned long long)logical, status);
                sb->active_transaction->error = status;
                free(block);
                return status;
            }
            input += chunk;
            offset += chunk;
            size -= chunk;
        }
        free(block);
        return EOK;
    }
    int status = blockdev_write_bytes(&sb->device, offset, buf, size);
    if (status != EOK)
        plogk("extfs: drive %u: block write failed at byte %llu (size %llu): %d\n", sb->device.drive, (unsigned long long)offset,
              (unsigned long long)size, status);
    return status;
}

int extfs_read_block(extfs_sb_info_t *sb, uint32_t phys_block, void *buf)
{
    if (!sb || !sb->es || !buf || phys_block >= sb->blocks_count) {
        if (sb && sb->es)
            plogk("extfs: drive %u: read of block %u out of range (count %llu)\n", sb->device.drive, phys_block,
                  (unsigned long long)sb->blocks_count);
        return -EIO;
    }
    if (sb->active_transaction) return fs_txn_read(sb->active_transaction, phys_block, buf);
    return extfs_disk_read(sb, extfs_block_offset(sb, phys_block), buf, sb->block_size);
}

int extfs_write_block(extfs_sb_info_t *sb, uint32_t phys_block, const void *buf)
{
    if (!sb || !sb->es || !buf || sb->read_only) return sb && sb->read_only ? -EROFS : -EINVAL;
    if (phys_block >= sb->blocks_count) {
        plogk("extfs: drive %u: write to block %u out of range (count %llu)\n", sb->device.drive, phys_block,
              (unsigned long long)sb->blocks_count);
        return -EIO;
    }
    if (sb->active_transaction) {
        int status = fs_txn_stage(sb->active_transaction, phys_block, buf, FS_TXN_METADATA);
        if (status != EOK) {
            plogk("extfs: drive %u: metadata stage of block %u failed (%d)\n", sb->device.drive, phys_block, status);
            sb->active_transaction->error = status;
        }
        return status;
    }
    return extfs_disk_write(sb, extfs_block_offset(sb, phys_block), buf, sb->block_size);
}

int extfs_write_data_block(extfs_sb_info_t *sb, uint32_t phys_block, const void *buf)
{
    if (!sb || !sb->es || !buf || sb->read_only) return sb && sb->read_only ? -EROFS : -EINVAL;
    if (phys_block >= sb->blocks_count) {
        plogk("extfs: drive %u: data write to block %u out of range (count %llu)\n", sb->device.drive, phys_block,
              (unsigned long long)sb->blocks_count);
        return -EIO;
    }
    if (sb->active_transaction) {
        int status = fs_txn_stage(sb->active_transaction, phys_block, buf, FS_TXN_ORDERED_DATA);
        if (status != EOK) {
            plogk("extfs: drive %u: data stage of block %u failed (%d)\n", sb->device.drive, phys_block, status);
            sb->active_transaction->error = status;
        }
        return status;
    }
    return blockdev_write_bytes(&sb->device, extfs_block_offset(sb, phys_block), buf, sb->block_size);
}

int extfs_transaction_begin(extfs_sb_info_t *sb, fs_txn_t *transaction, uint32_t credits)
{
    int status;
    if (!sb || !transaction || sb->active_transaction || !sb->transaction_log_initialized) return -EINVAL;
    if (sb->read_only) return -EROFS;
    status = fs_txn_begin(&sb->transaction_log, credits, transaction);
    if (status == EOK) sb->active_transaction = transaction;
    return status;
}

int extfs_transaction_commit(extfs_sb_info_t *sb, fs_txn_t *transaction)
{
    if (!sb || !transaction || sb->active_transaction != transaction) return -EINVAL;
    sb->active_transaction = 0;
    int status             = fs_txn_commit(transaction);
    if (status != EOK) sb->read_only = 1;
    return status;
}

void extfs_transaction_abort(extfs_sb_info_t *sb, fs_txn_t *transaction, int error)
{
    if (!sb || !transaction || sb->active_transaction != transaction) return;
    sb->active_transaction = 0;
    fs_txn_abort(transaction, error);
    /* Discard allocator counter changes that only existed in the aborted transaction. */
    if (blockdev_read_bytes(&sb->device, 1024, sb->es, sizeof(*sb->es)) != EOK) {
        sb->read_only = 1;
        return;
    }
    for (uint32_t i = 0; i < sb->gdb_count; i++) {
        uint32_t count     = sb->desc_per_block;
        uint32_t remaining = sb->groups_count - i * sb->desc_per_block;
        if (remaining < count) count = remaining;
        uint64_t offset = extfs_block_offset(sb, sb->s_first_data_block + 1 + i);
        for (uint32_t j = 0; j < count; j++) {
            if (blockdev_read_bytes(&sb->device, offset + (uint64_t)j * sb->desc_size, &sb->group_desc[i * sb->desc_per_block + j],
                                    sb->desc_size)
                != EOK) {
                sb->read_only = 1;
                return;
            }
        }
    }
}

int extfs_read_inode_raw(extfs_sb_info_t *sb, uint32_t ino, ext2_inode_t *raw)
{
    uint32_t group, offset, block;
    uint64_t byte_offset;

    if (!sb || !raw || ino == 0 || ino > sb->es->s_inodes_count) return -EINVAL;

    group       = (ino - 1) / sb->inodes_per_group;
    offset      = (ino - 1) % sb->inodes_per_group;
    block       = sb->group_desc[group].bg_inode_table + (offset * sb->inode_size) / sb->block_size;
    byte_offset = extfs_block_offset(sb, block) + (offset * sb->inode_size) % sb->block_size;

    uint8_t *inode = malloc(sb->inode_size);
    if (!inode) return -ENOMEM;
    int status = extfs_disk_read(sb, byte_offset, inode, sb->inode_size);
    if (status == EOK && (sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) {
        uint16_t stored_lo, stored_hi = 0;
        memcpy(&stored_lo, inode + 124, sizeof(stored_lo));
        if (extfs_inode_has_checksum_hi(sb, inode)) memcpy(&stored_hi, inode + 130, sizeof(stored_hi));
        uint32_t calculated = extfs_inode_checksum(sb, ino, inode);
        uint32_t stored     = stored_lo | (uint32_t)stored_hi << 16;
        uint32_t mask       = extfs_inode_has_checksum_hi(sb, inode) ? UINT32_MAX : UINT16_MAX;
        if ((stored & mask) != (calculated & mask)) {
            plogk("extfs: drive %u: inode %u checksum mismatch\n", sb->device.drive, ino);
            status = -EIO;
        }
    }
    memset(raw, 0, sizeof(*raw));
    if (status == EOK) memcpy(raw, inode, sizeof(*raw));
    free(inode);
    return status;
}

int extfs_write_inode_raw(extfs_sb_info_t *sb, uint32_t ino, const ext2_inode_t *raw)
{
    uint32_t group, offset, block;
    uint64_t byte_offset;

    if (!sb || !raw || sb->read_only) return sb && sb->read_only ? -EROFS : -EINVAL;
    if (ino == 0 || ino > sb->es->s_inodes_count) return -EINVAL;

    group       = (ino - 1) / sb->inodes_per_group;
    offset      = (ino - 1) % sb->inodes_per_group;
    block       = sb->group_desc[group].bg_inode_table + (offset * sb->inode_size) / sb->block_size;
    byte_offset = extfs_block_offset(sb, block) + (offset * sb->inode_size) % sb->block_size;

    /* Preserve ext4 extra inode fields and update the checksum over the full inode. */
    uint8_t *inode = malloc(sb->inode_size);
    if (!inode) return -ENOMEM;
    int status = extfs_disk_read(sb, byte_offset, inode, sb->inode_size);
    if (status == EOK) {
        memcpy(inode, raw, sizeof(*raw));
        if (sb->inode_size > 128 && raw->i_mode) {
            uint16_t extra = 0, desired = 0;
            memcpy(&extra, inode + 128, 2);
            memcpy(&desired, (uint8_t *)sb->es + 350, 2);
            if (!desired) memcpy(&desired, (uint8_t *)sb->es + 348, 2);
            if (desired > sb->inode_size - 128) desired = (uint16_t)(sb->inode_size - 128);
            if (!extra && desired) memcpy(inode + 128, &desired, 2);
        }
        if (sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
            memset(inode + 124, 0, 2);
            if (extfs_inode_has_checksum_hi(sb, inode)) memset(inode + 130, 0, 2);
            uint32_t checksum = extfs_inode_checksum(sb, ino, inode);
            memcpy(inode + 124, &checksum, 2);
            if (extfs_inode_has_checksum_hi(sb, inode)) memcpy(inode + 130, (uint8_t *)&checksum + 2, 2);
        }
        status = extfs_disk_write(sb, byte_offset, inode, sb->inode_size);
    }
    free(inode);
    return status;
}

int extfs_read_group_desc(extfs_sb_info_t *sb, uint32_t group, ext2_group_desc_t *desc)
{
    uint32_t desc_block, desc_offset;
    uint64_t byte_offset;

    if (!sb || !desc || group >= sb->groups_count) return -EINVAL;
    desc_block  = sb->s_first_data_block + 1 + group / sb->desc_per_block;
    desc_offset = group % sb->desc_per_block;
    byte_offset = extfs_block_offset(sb, desc_block) + (uint64_t)desc_offset * sb->desc_size;

    memset(desc, 0, sizeof(*desc));
    return extfs_disk_read(sb, byte_offset, desc, sb->desc_size);
}

int extfs_write_group_desc(extfs_sb_info_t *sb, uint32_t group, const ext2_group_desc_t *desc)
{
    uint32_t desc_block, desc_offset;
    uint64_t byte_offset;

    if (!sb || !desc || sb->read_only) return sb && sb->read_only ? -EROFS : -EINVAL;
    if (group >= sb->groups_count) return -EINVAL;
    desc_block  = sb->s_first_data_block + 1 + group / sb->desc_per_block;
    desc_offset = group % sb->desc_per_block;
    byte_offset = extfs_block_offset(sb, desc_block) + (uint64_t)desc_offset * sb->desc_size;

    ext2_group_desc_t copy            = *desc;
    copy.bg_checksum                  = extfs_group_desc_checksum(sb, group, &copy);
    sb->group_desc[group].bg_checksum = copy.bg_checksum;
    return extfs_disk_write(sb, byte_offset, &copy, sb->desc_size);
}

int extfs_read_super(extfs_sb_info_t *sb, const blockdev_device_t *device)
{
    int      status;
    uint32_t i;

    if (!sb || !device) return -EINVAL;
    memset(sb, 0, sizeof(*sb));
    memcpy(&sb->device, device, sizeof(sb->device));
    blockdev_retain(&sb->device);

    sb->es = calloc(1, sizeof(ext2_super_block_t));
    if (!sb->es) {
        blockdev_release(&sb->device);
        return -ENOMEM;
    }

    status = extfs_disk_read(sb, 1024, sb->es, sizeof(ext2_super_block_t));
    if (status != EOK) {
        free(sb->es);
        blockdev_release(&sb->device);
        return -EIO;
    }

    if (sb->es->s_magic != 0xEF53) {
        free(sb->es);
        blockdev_release(&sb->device);
        return -EINVAL;
    }

    if (sb->es->s_rev_level > EXT2_MAX_SUPP_REV || sb->es->s_log_block_size > 2 || !sb->es->s_blocks_per_group || !sb->es->s_inodes_per_group
        || sb->es->s_blocks_count <= sb->es->s_first_data_block) {
        free(sb->es);
        blockdev_release(&sb->device);
        return -EINVAL;
    }

    uint32_t supported_incompat = EXT2_FEATURE_INCOMPAT_FILETYPE | EXT3_FEATURE_INCOMPAT_RECOVER | EXT4_FEATURE_INCOMPAT_EXTENTS
                                  | EXT4_FEATURE_INCOMPAT_64BIT | EXT4_FEATURE_INCOMPAT_FLEX_BG | EXT4_FEATURE_INCOMPAT_CSUM_SEED
                                  | EXT4_FEATURE_INCOMPAT_LARGEDIR;
    uint32_t unsupported_incompat = sb->es->s_feature_incompat & ~supported_incompat;
    if (unsupported_incompat || (sb->es->s_feature_incompat & EXT3_FEATURE_INCOMPAT_JOURNAL_DEV)) {
        free(sb->es);
        blockdev_release(&sb->device);
        return -EOPNOTSUPP;
    }

    sb->log_block_size = sb->es->s_log_block_size;
    sb->block_size     = EXT2_MIN_BLOCK_SIZE << sb->log_block_size;
    sb->read_only      = device->read_only;

    sb->blocks_count = sb->es->s_blocks_count;
    if (sb->es->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) sb->blocks_count |= (uint64_t)extfs_super_u32(sb->es, 336) << 32;
    if (sb->blocks_count > UINT32_MAX) {
        free(sb->es);
        blockdev_release(&sb->device);
        return -EFBIG;
    }

    if (sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_BIGALLOC) {
        free(sb->es);
        blockdev_release(&sb->device);
        return -EOPNOTSUPP;
    }

    sb->checksum_seed = (sb->es->s_feature_incompat & EXT4_FEATURE_INCOMPAT_CSUM_SEED) ?
                            extfs_super_u32(sb->es, 624) :
                            crc32c_update(~0U, sb->es->s_uuid, sizeof(sb->es->s_uuid));
    if (sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
        if (*((uint8_t *)sb->es + 373) != 1 || extfs_super_u32(sb->es, 1020) != crc32c_update(~0U, sb->es, 1020)) {
            free(sb->es);
            blockdev_release(&sb->device);
            return -EIO;
        }
    }

    uint32_t supported_ro = EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT2_FEATURE_RO_COMPAT_LARGE_FILE | EXT2_FEATURE_RO_COMPAT_BTREE_DIR
                            | EXT4_FEATURE_RO_COMPAT_HUGE_FILE | EXT4_FEATURE_RO_COMPAT_GDT_CSUM | EXT4_FEATURE_RO_COMPAT_DIR_NLINK
                            | EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE | EXT4_FEATURE_RO_COMPAT_METADATA_CSUM;
    if (sb->es->s_feature_ro_compat & ~supported_ro) sb->read_only = 1;
    if (!(sb->es->s_state & EXT2_VALID_FS) && !(sb->es->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL)) sb->read_only = 1;

    if (sb->es->s_blocks_per_group > sb->block_size * 8 || sb->es->s_inodes_per_group > sb->block_size * 8) {
        free(sb->es);
        blockdev_release(&sb->device);
        return -EINVAL;
    }

    sb->s_first_data_block = sb->es->s_first_data_block;
    sb->blocks_per_group   = sb->es->s_blocks_per_group;
    sb->inodes_per_group   = sb->es->s_inodes_per_group;
    sb->inode_size         = sb->es->s_inode_size;
    sb->s_first_ino        = sb->es->s_first_ino;

    if (sb->inode_size == 0) sb->inode_size = EXT2_GOOD_OLD_INODE_SIZE;

    if (sb->inode_size < sizeof(ext2_inode_t) || sb->inode_size > sb->block_size || (sb->inode_size & (sb->inode_size - 1))) {
        free(sb->es);
        blockdev_release(&sb->device);
        return -EINVAL;
    }

    if (sb->s_first_ino == 0) sb->s_first_ino = EXT2_GOOD_OLD_FIRST_INO;

    sb->groups_count = (uint32_t)((sb->blocks_count - sb->s_first_data_block + sb->blocks_per_group - 1) / sb->blocks_per_group);

    sb->desc_size = (sb->es->s_feature_incompat & EXT4_FEATURE_INCOMPAT_64BIT) ? sb->es->s_desc_size : 32;
    if (sb->desc_size < 32 || sb->desc_size > sizeof(ext2_group_desc_t) || (sb->desc_size & 7)) {
        extfs_free_super(sb);
        return -EINVAL;
    }
    sb->desc_per_block = sb->block_size / sb->desc_size;

    sb->gdb_count = (sb->groups_count + sb->desc_per_block - 1) / sb->desc_per_block;

    sb->group_desc = calloc(sb->groups_count, sizeof(ext2_group_desc_t));
    if (!sb->group_desc) {
        free(sb->es);
        blockdev_release(&sb->device);
        return -ENOMEM;
    }

    for (i = 0; i < sb->gdb_count; i++) {
        uint32_t blk       = sb->s_first_data_block + 1 + i;
        uint64_t off       = extfs_block_offset(sb, blk);
        uint32_t count     = sb->desc_per_block;
        uint32_t remaining = sb->groups_count - i * sb->desc_per_block;
        if (remaining < count) count = remaining;

        for (uint32_t j = 0; j < count; j++) {
            status = extfs_disk_read(sb, off + (uint64_t)j * sb->desc_size, &sb->group_desc[i * sb->desc_per_block + j], sb->desc_size);
            if (status != EOK) {
                extfs_free_super(sb);
                return -EIO;
            }
        }
    }

    for (i = 0; i < sb->groups_count; i++) {
        ext2_group_desc_t *desc = &sb->group_desc[i];
        if (desc->bg_block_bitmap_hi || desc->bg_inode_bitmap_hi || desc->bg_inode_table_hi || desc->bg_block_bitmap >= sb->blocks_count
            || desc->bg_inode_bitmap >= sb->blocks_count || desc->bg_inode_table >= sb->blocks_count
            || desc->bg_checksum != extfs_group_desc_checksum(sb, i, desc)) {
            extfs_free_super(sb);
            return -EINVAL;
        }
    }

    const fs_txn_backend_ops_t *journal_ops = 0;
    if (sb->es->s_feature_compat & EXT3_FEATURE_COMPAT_HAS_JOURNAL) {
        status = extfs_jbd2_open(sb, &sb->journal);
        if (status != EOK) {
            extfs_free_super(sb);
            return status;
        }
        journal_ops = extfs_jbd2_backend_ops();
    } else if (sb->es->s_feature_incompat & EXT3_FEATURE_INCOMPAT_RECOVER) {
        extfs_free_super(sb);
        return -EINVAL;
    }

    status = fs_txn_log_init(&sb->transaction_log, &sb->device, sb->block_size, journal_ops, sb->journal);
    if (status != EOK) {
        extfs_free_super(sb);
        return status;
    }
    sb->transaction_log_initialized = 1;

    /* Linux loads and replays JBD2 before exposing the root inode. */
    status = fs_txn_recover(&sb->transaction_log);
    if (status != EOK) {
        extfs_free_super(sb);
        return status;
    }

    return EOK;
}

int extfs_write_super(extfs_sb_info_t *sb)
{
    if (!sb || !sb->es) return -EINVAL;
    if (sb->read_only) return -EROFS;
    if (sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) extfs_super_put_u32(sb->es, 1020, crc32c_update(~0U, sb->es, 1020));
    return extfs_disk_write(sb, 1024, sb->es, sizeof(ext2_super_block_t));
}

void extfs_free_super(extfs_sb_info_t *sb)
{
    if (!sb) return;
    if (sb->transaction_log_initialized) fs_txn_log_destroy(&sb->transaction_log);
    if (sb->journal) extfs_jbd2_close(sb->journal);
    if (sb->group_desc) free(sb->group_desc);
    if (sb->es) free(sb->es);
    blockdev_release(&sb->device);
    memset(sb, 0, sizeof(*sb));
}
