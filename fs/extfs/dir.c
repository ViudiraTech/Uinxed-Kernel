/*
 *
 *      dir.c
 *      ext2/ext3/ext4 filesystem - directory operations
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/extfs/extfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <libs/util/crc32c.h>
#include <mem/alloc.h>
#include <mem/heap.h>

#define EXT4_FT_DIR_CSUM 0xDEU

/*
 * Overview
 * dir.c implements ext2/3/4 directory operations: reading directory
 * entries, looking up names, and adding/removing entries with the
 * linear- or htree-style record layout.
 */

typedef struct ext4_dir_entry_tail {
        uint32_t reserved_zero1;
        uint16_t rec_len;
        uint8_t  reserved_zero2;
        uint8_t  reserved_ft;
        uint32_t checksum;
} __attribute__((packed)) ext4_dir_entry_tail_t;

static int      extfs_dirent_valid(extfs_sb_info_t *sb, ext2_dir_entry_t *de, uint32_t offset);
static uint32_t extfs_dir_rec_len(uint32_t name_len);

/* Derive the per-directory checksum seed from its inode. */
static uint32_t extfs_dir_checksum_seed(extfs_handle_t *dir_h)
{
    ext2_inode_t raw;
    if (extfs_read_inode_raw(dir_h->sb, dir_h->inode_no, &raw) != EOK) return 0;
    uint32_t checksum = crc32c_update(dir_h->sb->checksum_seed, &dir_h->inode_no, sizeof(dir_h->inode_no));
    return crc32c_update(checksum, &raw.i_generation, sizeof(raw.i_generation));
}

/* Locate the directory checksum tail, validating its markers. */
static ext4_dir_entry_tail_t *extfs_dir_tail(extfs_handle_t *dir_h, const void *block)
{
    if (!(dir_h->sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) return 0;
    ext4_dir_entry_tail_t *tail = (ext4_dir_entry_tail_t *)((uint8_t *)block + dir_h->sb->block_size - sizeof(*tail));
    if (tail->reserved_zero1 || tail->rec_len != sizeof(*tail) || tail->reserved_zero2 || tail->reserved_ft != EXT4_FT_DIR_CSUM) return 0;
    return tail;
}

/* Verify the checksum of an HTree index block. */
static int extfs_dx_checksum_verify(extfs_handle_t *dir_h, uint32_t logical, const uint8_t *block)
{
    uint32_t count_offset;
    if (logical == 0) {
        ext2_dir_entry_t *dot    = (ext2_dir_entry_t *)block;
        ext2_dir_entry_t *dotdot = (ext2_dir_entry_t *)(block + dot->rec_len);
        if (dot->rec_len != 12 || dotdot->rec_len != dir_h->sb->block_size - 12) return 0;
        count_offset = 32;
    } else {
        ext2_dir_entry_t *fake = (ext2_dir_entry_t *)block;
        if (fake->inode || fake->rec_len != dir_h->sb->block_size) return 0;
        count_offset = 8;
    }
    uint16_t limit, count;
    memcpy(&limit, block + count_offset, 2);
    memcpy(&count, block + count_offset + 2, 2);
    if (!count || count > limit || count_offset + (uint32_t)limit * 8 + 8 > dir_h->sb->block_size) return 0;
    const uint8_t *tail = block + count_offset + (size_t)limit * 8;
    uint32_t       reserved, stored;
    memcpy(&reserved, tail, 4);
    memcpy(&stored, tail + 4, 4);
    if (reserved) return 0;
    uint32_t checksum = crc32c_update(extfs_dir_checksum_seed(dir_h), block, count_offset + (uint32_t)count * 8);
    checksum          = crc32c_update(checksum, tail, 4);
    uint32_t zero     = 0;
    checksum          = crc32c_update(checksum, &zero, 4);
    return checksum == stored;
}

/* Whether a block is a plausible HTree index node. */
static int extfs_dir_is_dx_node(extfs_handle_t *dir_h, const uint8_t *block)
{
    ext2_dir_entry_t *fake = (ext2_dir_entry_t *)block;
    uint16_t          limit, count;
    if (fake->inode || fake->rec_len != dir_h->sb->block_size) return 0;
    memcpy(&limit, block + 8, 2);
    memcpy(&count, block + 10, 2);
    return count && count <= limit && 8 + (uint32_t)limit * 8 <= dir_h->sb->block_size;
}

/* Verify a directory block, covering linear and HTree checksums. */
int extfs_dir_block_verify(extfs_handle_t *dir_h, uint32_t logical, const void *block)
{
    if (!(dir_h->sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) return 1;
    ext4_dir_entry_tail_t *tail = extfs_dir_tail(dir_h, block);
    if (tail) {
        uint32_t checksum = crc32c_update(extfs_dir_checksum_seed(dir_h), block, dir_h->sb->block_size - sizeof(*tail));
        if (checksum != tail->checksum) {
            plogk("extfs: Drive %u: dir %u block %u checksum mismatch.\n", dir_h->sb->device.drive, dir_h->inode_no, logical);
            return 0;
        }
        return 1;
    }
    int valid = (dir_h->ei.i_flags & EXT4_INDEX_FL) && extfs_dx_checksum_verify(dir_h, logical, block);
    if (!valid) plogk("extfs: Drive %u: dir %u block %u failed verification.\n", dir_h->sb->device.drive, dir_h->inode_no, logical);
    return valid;
}

static void extfs_dir_block_checksum_set(extfs_handle_t *dir_h, void *block)
{
    ext4_dir_entry_tail_t *tail = extfs_dir_tail(dir_h, block);
    if (tail) tail->checksum = crc32c_update(extfs_dir_checksum_seed(dir_h), block, dir_h->sb->block_size - sizeof(*tail));
}

static void extfs_dir_tail_init(extfs_handle_t *dir_h, void *block)
{
    if (!(dir_h->sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) return;
    ext4_dir_entry_tail_t *tail = (ext4_dir_entry_tail_t *)((uint8_t *)block + dir_h->sb->block_size - sizeof(*tail));
    memset(tail, 0, sizeof(*tail));
    tail->rec_len     = sizeof(*tail);
    tail->reserved_ft = EXT4_FT_DIR_CSUM;
}

static int extfs_dir_write_leaf(extfs_handle_t *dir_h, uint32_t physical, void *block)
{
    extfs_dir_block_checksum_set(dir_h, block);
    return extfs_write_block(dir_h->sb, physical, block);
}

typedef struct extfs_dir_item {
        uint32_t inode;
        uint8_t  name_len;
        uint8_t  file_type;
        char     name[EXT2_NAME_LEN];
} extfs_dir_item_t;

/* Append a directory entry to the item vector, growing it as needed. */
static int extfs_dir_item_push(extfs_dir_item_t **items, uint32_t *count, uint32_t *capacity, const ext2_dir_entry_t *entry)
{
    if (*count == *capacity) {
        uint32_t new_capacity = *capacity ? *capacity * 2 : 32;
        void    *resized      = realloc(*items, new_capacity * sizeof(**items));
        if (!resized) return -ENOMEM;
        *items    = resized;
        *capacity = new_capacity;
    }
    extfs_dir_item_t *item = &(*items)[(*count)++];
    item->inode            = entry->inode;
    item->name_len         = entry->name_len;
    item->file_type        = entry->file_type;
    memcpy(item->name, entry->name, entry->name_len);
    return EOK;
}

/* Convert an HTree directory to Linux-compatible linear blocks before mutation. */
static int extfs_dir_deindex(extfs_handle_t *dir_h, ext2_inode_t *raw)
{
    if (!(raw->i_flags & EXT4_INDEX_FL)) return EOK;
    extfs_sb_info_t  *sb     = dir_h->sb;
    uint32_t          blocks = (raw->i_size + sb->block_size - 1) / sb->block_size;
    uint8_t          *buffer = malloc(sb->block_size);
    extfs_dir_item_t *items  = 0;
    uint32_t          count = 0, capacity = 0;
    int               status = buffer ? EOK : -ENOMEM;

    for (uint32_t logical = 0; logical < blocks && status == EOK; logical++) {
        uint32_t physical = extfs_map_block(dir_h, logical, 0);
        if (!physical || (status = extfs_read_block(sb, physical, buffer)) != EOK // NOLINT(bugprone-assignment-in-if-condition)
            || !extfs_dir_block_verify(dir_h, logical, buffer)) {
            if (status == EOK) status = -EIO;
            break;
        }
        if (logical == 0) {
            ext2_dir_entry_t *dot    = (ext2_dir_entry_t *)buffer;
            ext2_dir_entry_t *dotdot = (ext2_dir_entry_t *)(buffer + dot->rec_len);
            if (!extfs_dirent_valid(sb, dot, 0) || !extfs_dirent_valid(sb, dotdot, dot->rec_len)
                || (status = extfs_dir_item_push(&items, &count, &capacity, dot)) != EOK     // NOLINT(bugprone-assignment-in-if-condition)
                || (status = extfs_dir_item_push(&items, &count, &capacity, dotdot)) != EOK) // NOLINT(bugprone-assignment-in-if-condition)
                break;
            continue;
        }
        ext4_dir_entry_tail_t *tail = extfs_dir_tail(dir_h, buffer);
        if (!tail && (sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) continue; // Checksummed internal HTree node.
        if (!tail && extfs_dir_is_dx_node(dir_h, buffer)) continue;
        uint32_t offset = 0, limit = sb->block_size - (tail ? sizeof(*tail) : 0);
        while (offset < limit) {
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(buffer + offset);
            if (!extfs_dirent_valid(sb, entry, offset) || entry->rec_len > limit - offset) {
                status = -EIO;
                break;
            }
            if (entry->inode) status = extfs_dir_item_push(&items, &count, &capacity, entry);
            if (status != EOK) break;
            offset += entry->rec_len;
        }
    }

    uint32_t item_index = 0;
    for (uint32_t logical = 0; logical < blocks && status == EOK; logical++) {
        uint32_t physical = extfs_map_block(dir_h, logical, 0);
        memset(buffer, 0, sb->block_size);
        extfs_dir_tail_init(dir_h, buffer);
        uint32_t limit  = sb->block_size - (extfs_dir_tail(dir_h, buffer) ? sizeof(ext4_dir_entry_tail_t) : 0);
        uint32_t offset = 0, previous = UINT32_MAX;
        while (item_index < count) {
            uint32_t required = extfs_dir_rec_len(items[item_index].name_len);
            if (required > limit - offset) break;
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(buffer + offset);
            entry->inode            = items[item_index].inode;
            entry->rec_len          = (uint16_t)required;
            entry->name_len         = items[item_index].name_len;
            entry->file_type        = items[item_index].file_type;
            memcpy(entry->name, items[item_index].name, entry->name_len);
            previous = offset;
            offset += required;
            item_index++;
        }
        if (previous != UINT32_MAX) {
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *)(buffer + previous);
            entry->rec_len += (uint16_t)(limit - offset);
        } else {
            ext2_dir_entry_t *entry = (ext2_dir_entry_t *)buffer;
            entry->rec_len          = (uint16_t)limit;
        }
        status = physical ? extfs_dir_write_leaf(dir_h, physical, buffer) : -EIO;
    }
    if (status == EOK && item_index != count) status = -ENOSPC;
    if (status == EOK) {
        raw->i_flags &= ~EXT4_INDEX_FL;
        dir_h->ei.i_flags &= ~EXT4_INDEX_FL;
        status = extfs_write_inode_raw(sb, dir_h->inode_no, raw);
    }
    free(items);
    free(buffer);
    return status;
}

static uint32_t extfs_dir_rec_len(uint32_t name_len)
{
    return EXT2_DIR_REC_LEN(name_len);
}

/* Validate a directory entry's length and inode reference. */
static int extfs_dirent_valid(extfs_sb_info_t *sb, ext2_dir_entry_t *de, uint32_t offset)
{
    uint32_t remaining = sb->block_size - offset;
    if (remaining < 8 || de->rec_len < 8 || (de->rec_len & EXT2_DIR_ROUND) || de->rec_len > remaining) {
        plogk("extfs: Drive %u: invalid directory entry at offset %u (inode %u, rec_len %u)\n", sb->device.drive, offset, de->inode, de->rec_len);
        return 0;
    }
    if (de->name_len > de->rec_len - 8) {
        plogk("extfs: Drive %u: invalid directory entry name at offset %u (inode %u, name_len %u)\n", sb->device.drive, offset, de->inode, de->name_len);
        return 0;
    }
    if (de->inode > sb->es->s_inodes_count) {
        plogk("extfs: Drive %u: directory entry %u references out-of-range inode %u\n", sb->device.drive, offset, de->inode);
        return 0;
    }
    return 1;
}

/* Look up a name in a directory, returning its inode number. */
int extfs_dir_lookup(extfs_handle_t *dir_h, const char *name, uint32_t *ino)
{
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    uint32_t         block_num, phys;
    uint32_t         name_len;
    int              found = 0;

    if (!dir_h || !dir_h->sb || !name || !ino) return -EINVAL;
    sb = dir_h->sb;

    if (extfs_read_inode_raw(sb, dir_h->inode_no, &raw) != EOK) return -EIO;
    if ((raw.i_mode & 0xF000) != EXT2_S_IFDIR) return -ENOTDIR;

    name_len = strlen(name);
    if (!name_len || name_len > EXT2_NAME_LEN) return -ENOENT;

    block_buf = malloc(sb->block_size);
    if (!block_buf) return -ENOMEM;

    uint64_t dir_size = raw.i_size;
    for (block_num = 0; (uint64_t)block_num * sb->block_size < dir_size; block_num++) {
        uint32_t offset = 0;
        uint32_t valid  = (uint32_t)((dir_size - (uint64_t)block_num * sb->block_size) > sb->block_size ? sb->block_size : dir_size - (uint64_t)block_num * sb->block_size);

        phys = extfs_map_block(dir_h, block_num, 0);
        if (!phys) continue;
        if (extfs_read_block(sb, phys, block_buf) != EOK) {
            free(block_buf);
            return -EIO;
        }
        if (!extfs_dir_block_verify(dir_h, block_num, block_buf)) {
            free(block_buf);
            return -EIO;
        }

        while (offset < valid) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + offset);
            if (!extfs_dirent_valid(sb, de, offset) || de->rec_len > valid - offset) {
                free(block_buf);
                return -EIO;
            }
            if (de->inode != 0 && de->name_len == name_len && memcmp(de->name, name, name_len) == 0) {
                *ino  = de->inode;
                found = 1;
                break;
            }
            offset += de->rec_len;
        }
        if (found) break;
    }

    free(block_buf);
    return found ? EOK : -ENOENT;
}

/* Add a name to a directory, reusing a free slot or extending the file. */
int extfs_dir_add_entry(extfs_handle_t *dir_h, const char *name, uint32_t ino, uint8_t file_type)
{
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    uint32_t         name_len, rec_len;
    uint32_t         block_num = 0;
    uint32_t         phys      = 0;
    int              status;

    if (!dir_h || !dir_h->sb || !name || ino == 0) return -EINVAL;
    sb = dir_h->sb;
    if (sb->block_size < EXT2_MIN_BLOCK_SIZE) return -EINVAL;
    if (ino > sb->es->s_inodes_count) return -EINVAL;

    name_len = strlen(name);
    if (!name_len) return -EINVAL;
    if (name_len > EXT2_NAME_LEN) return -ENAMETOOLONG;
    if ((name_len == 1 && name[0] == '.') || (name_len == 2 && name[0] == '.' && name[1] == '.')) return -EINVAL;

    rec_len = extfs_dir_rec_len(name_len);

    status = extfs_read_inode_raw(sb, dir_h->inode_no, &raw);
    if (status != EOK) return status;
    if ((raw.i_mode & 0xF000) != EXT2_S_IFDIR) return -ENOTDIR;
    status = extfs_dir_deindex(dir_h, &raw);
    if (status != EOK) return status;

    uint64_t dir_size = raw.i_size;

    block_buf = malloc(sb->block_size);
    if (!block_buf) return -ENOMEM;

    /* Scan for a free slot or extend the directory */
    uint32_t total_offset = 0;
    int      found_space  = 0;

    while (total_offset < dir_size) {
        uint32_t boff = total_offset % sb->block_size;

        if (boff == 0) {
            phys = extfs_map_block(dir_h, block_num, 0);

            if (phys == 0) break;

            if (extfs_read_block(sb, phys, block_buf) != EOK) {
                free(block_buf);
                return -EIO;
            }
            if (!extfs_dir_block_verify(dir_h, block_num, block_buf)) {
                free(block_buf);
                return -EIO;
            }
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + boff);
        if (!extfs_dirent_valid(sb, de, boff)) {
            free(block_buf);
            return -EIO;
        }

        int is_tail = extfs_dir_tail(dir_h, block_buf) && (uint8_t *)de == block_buf + sb->block_size - sizeof(ext4_dir_entry_tail_t);
        if (!is_tail && de->inode == 0 && de->rec_len >= rec_len) {
            /* Use this free slot */
            de->inode     = ino;
            de->name_len  = name_len;
            de->file_type = (sb->es->s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) ? file_type : EXT2_FT_UNKNOWN;
            memcpy(de->name, name, name_len);

            if (extfs_dir_write_leaf(dir_h, phys, block_buf) != EOK) {
                free(block_buf);
                return -EIO;
            }
            found_space = 1;
            break;
        }

        uint32_t actual_len = extfs_dir_rec_len(de->name_len);
        if (de->rec_len > actual_len && de->rec_len - actual_len >= rec_len) {
            /* Split: shrink existing entry and create new one */
            uint32_t old_rec = de->rec_len;
            de->rec_len      = actual_len;

            ext2_dir_entry_t *new_de = (ext2_dir_entry_t *)((uint8_t *)de + actual_len);
            new_de->inode            = ino;
            new_de->name_len         = name_len;
            new_de->file_type        = (sb->es->s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) ? file_type : EXT2_FT_UNKNOWN;
            new_de->rec_len          = old_rec - actual_len;
            memcpy(new_de->name, name, name_len);

            if (extfs_dir_write_leaf(dir_h, phys, block_buf) != EOK) {
                free(block_buf);
                return -EIO;
            }
            found_space = 1;
            break;
        }

        total_offset += de->rec_len;
        block_num = total_offset / sb->block_size;
    }

    if (!found_space) {
        /* Extend directory by one block through the normal inode mapper. */
        uint32_t last_block = (uint32_t)((dir_size + sb->block_size - 1) / sb->block_size);
        uint32_t new_block  = extfs_map_block(dir_h, last_block, 1);
        if (!new_block) {
            free(block_buf);
            return -ENOSPC;
        }

        memset(block_buf, 0, sb->block_size);
        extfs_dir_tail_init(dir_h, block_buf);

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)block_buf;
        de->inode            = ino;
        de->name_len         = name_len;
        de->file_type        = (sb->es->s_feature_incompat & EXT2_FEATURE_INCOMPAT_FILETYPE) ? file_type : EXT2_FT_UNKNOWN;
        de->rec_len          = sb->block_size - (extfs_dir_tail(dir_h, block_buf) ? sizeof(ext4_dir_entry_tail_t) : 0);
        memcpy(de->name, name, name_len);

        if (extfs_dir_write_leaf(dir_h, new_block, block_buf) != EOK) {
            free(block_buf);
            return -EIO;
        }

        memcpy(raw.i_block, dir_h->ei.i_data, sizeof(raw.i_block));
        raw.i_size = dir_size + sb->block_size;
        if (dir_h->ei.i_flags & EXT4_EXTENTS_FL) {
            uint64_t blocks;
            status = extfs_extent_count_blocks(dir_h, &blocks);
            if (status != EOK) {
                free(block_buf);
                return status;
            }
            uint32_t sectors_per_block = sb->block_size / 512;
            if (!sectors_per_block || blocks > UINT32_MAX / sectors_per_block) {
                free(block_buf);
                return -EOVERFLOW;
            }
            raw.i_blocks = (uint32_t)blocks * sectors_per_block;
        } else {
            raw.i_blocks += sb->block_size / 512;
        }

        status = extfs_write_inode_raw(sb, dir_h->inode_no, &raw);
        if (status != EOK) {
            free(block_buf);
            return status;
        }
    }

    free(block_buf);
    return EOK;
}

/* Remove a name from a directory, merging it into the previous entry. */
int extfs_dir_remove_entry(extfs_handle_t *dir_h, const char *name)
{
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    uint32_t         name_len;
    uint32_t         block_num = 0;
    uint32_t         phys      = 0;
    int              found     = 0;

    if (!dir_h || !dir_h->sb || !name) return -EINVAL;
    sb = dir_h->sb;
    if (sb->block_size < EXT2_MIN_BLOCK_SIZE) return -EINVAL;
    uint32_t block_size = sb->block_size;

    name_len = strlen(name);
    if (!name_len) return -EINVAL;
    if (name_len > EXT2_NAME_LEN) return -ENAMETOOLONG;

    if (extfs_read_inode_raw(sb, dir_h->inode_no, &raw) != EOK) return -EIO;
    if ((raw.i_mode & 0xF000) != EXT2_S_IFDIR) return -ENOTDIR;
    int deindex_status = extfs_dir_deindex(dir_h, &raw);
    if (deindex_status != EOK) return deindex_status;

    uint64_t dir_size = raw.i_size;
    block_buf         = malloc(block_size);
    if (!block_buf) return -ENOMEM;

    uint32_t total_offset = 0;

    while (total_offset < dir_size) {
        uint32_t boff      = total_offset % block_size;
        uint32_t prev_boff = UINT32_MAX;

        if (boff == 0) {
            phys = extfs_map_block(dir_h, block_num, 0);
            if (phys == 0) {
                total_offset += block_size;
                block_num++;
                continue;
            }

            if (extfs_read_block(sb, phys, block_buf) != EOK) {
                free(block_buf);
                return -EIO;
            }
            if (!extfs_dir_block_verify(dir_h, block_num, block_buf)) {
                free(block_buf);
                return -EIO;
            }
        }

        while (boff < block_size && total_offset < dir_size) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + boff);
            if (!extfs_dirent_valid(sb, de, boff)) {
                free(block_buf);
                return -EIO;
            }
            if (de->inode != 0 && de->name_len == name_len && memcmp(de->name, name, name_len) == 0) {
                if (prev_boff != UINT32_MAX) {
                    ext2_dir_entry_t *prev = (ext2_dir_entry_t *)(block_buf + prev_boff);
                    prev->rec_len += de->rec_len;
                } else {
                    de->inode = 0;
                }
                if (extfs_dir_write_leaf(dir_h, phys, block_buf) != EOK) {
                    free(block_buf);
                    return -EIO;
                }
                found = 1;
                break;
            }
            prev_boff = boff;
            boff += de->rec_len;
            total_offset += de->rec_len;
        }
        if (found) break;
        block_num++;
    }

    free(block_buf);
    return found ? EOK : -ENOENT;
}

/* Serialize directory entries into the fixed VFS record layout. */
int extfs_dir_read_entries(extfs_handle_t *dir_h, void *buf, size_t bufsize, size_t *done)
{
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    uint8_t         *out          = buf;
    size_t           written      = 0;
    uint32_t         total_offset = 0, block_num = 0, phys;

    if (!dir_h || !dir_h->sb || !buf || !done || bufsize == 0) return -EINVAL;
    *done = 0;
    sb    = dir_h->sb;

    if (extfs_read_inode_raw(sb, dir_h->inode_no, &raw) != EOK) return -EIO;
    if ((raw.i_mode & 0xF000) != EXT2_S_IFDIR) return -ENOTDIR;

    uint64_t dir_size = raw.i_size;

    block_buf = malloc(sb->block_size);
    if (!block_buf) return -ENOMEM;

    while (total_offset < dir_size && written < bufsize) {
        uint32_t boff = total_offset % sb->block_size;

        if (boff == 0) {
            phys = extfs_map_block(dir_h, block_num, 0);

            if (phys == 0) {
                total_offset += sb->block_size;
                block_num++;
                continue;
            }

            if (extfs_read_block(sb, phys, block_buf) != EOK) {
                free(block_buf);
                return -EIO;
            }
            if (!extfs_dir_block_verify(dir_h, block_num, block_buf)) {
                free(block_buf);
                return -EIO;
            }
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + boff);
        if (!extfs_dirent_valid(sb, de, boff)) {
            free(block_buf);
            return -EIO;
        }

        if (de->inode != 0) {
            /* Each entry: inode(4) + name_len(1) + file_type(1) + name */
            uint32_t entry_size = 8 + de->name_len;
            if (written + entry_size <= bufsize) {
                memcpy(out + written, &de->inode, 4);
                out[written + 4]                = de->name_len;
                out[written + 5]                = de->file_type;
                out[written + 6 + de->name_len] = 0;
                out[written + 7 + de->name_len] = 0;
                memcpy(out + written + 6, de->name, de->name_len);
                written += entry_size;
            }
        }

        total_offset += de->rec_len;
        if (boff + de->rec_len >= sb->block_size) block_num++;
    }

    free(block_buf);
    *done = written;
    return EOK;
}

/* Initialize a new directory inode with "." and ".." entries. */
int extfs_make_empty_dir(extfs_handle_t *dir_h, uint32_t self_ino, uint32_t parent_ino)
{
    extfs_sb_info_t *sb;
    uint8_t         *block_buf;
    int              status;
    uint32_t         new_block;

    if (!dir_h || !dir_h->sb || !self_ino || !parent_ino) return -EINVAL;
    sb = dir_h->sb;
    if (self_ino > sb->es->s_inodes_count || parent_ino > sb->es->s_inodes_count) return -EINVAL;
    block_buf = calloc(1, sb->block_size);
    if (!block_buf) return -ENOMEM;

    /* "." entry */
    ext2_dir_entry_t *de = (ext2_dir_entry_t *)block_buf;
    de->inode            = self_ino;
    de->rec_len          = extfs_dir_rec_len(1);
    de->name_len         = 1;
    de->file_type        = EXT2_FT_DIR;
    de->name[0]          = '.';

    /* ".." entry */
    ext2_dir_entry_t *de2 = (ext2_dir_entry_t *)(block_buf + de->rec_len);
    extfs_dir_tail_init(dir_h, block_buf);
    de2->inode     = parent_ino;
    de2->rec_len   = sb->block_size - de->rec_len - (extfs_dir_tail(dir_h, block_buf) ? sizeof(ext4_dir_entry_tail_t) : 0);
    de2->name_len  = 2;
    de2->file_type = EXT2_FT_DIR;
    de2->name[0]   = '.';
    de2->name[1]   = '.';

    new_block = extfs_map_block(dir_h, 0, 1);
    status    = new_block ? extfs_dir_write_leaf(dir_h, new_block, block_buf) : -ENOSPC;
    free(block_buf);
    if (status != EOK) {
        extfs_free_block(sb, new_block);
        return status;
    }

    /* Set the block in the directory inode */
    ext2_inode_t raw;
    status = extfs_read_inode_raw(sb, dir_h->inode_no, &raw);
    if (status != EOK) {
        extfs_free_block(sb, new_block);
        return status;
    }

    memcpy(raw.i_block, dir_h->ei.i_data, sizeof(raw.i_block));
    raw.i_size = sb->block_size;
    if (dir_h->ei.i_flags & EXT4_EXTENTS_FL) {
        uint64_t blocks;
        status = extfs_extent_count_blocks(dir_h, &blocks);
        if (status != EOK || blocks > UINT32_MAX / (sb->block_size / 512)) {
            extfs_free_block(sb, new_block);
            return status != EOK ? status : -EOVERFLOW;
        }
        raw.i_blocks = (uint32_t)blocks * (sb->block_size / 512);
    } else {
        raw.i_blocks = sb->block_size / 512;
    }
    raw.i_links_count = 2;

    status = extfs_write_inode_raw(sb, dir_h->inode_no, &raw);
    if (status != EOK) {
        extfs_free_block(sb, new_block);
        return status;
    }

    return EOK;
}

/* Update the ".." entry while moving a directory between parents. */
int extfs_dir_set_parent(extfs_handle_t *dir_h, uint32_t parent_ino)
{
    if (!dir_h || !dir_h->sb || !parent_ino || parent_ino > dir_h->sb->es->s_inodes_count) return -EINVAL;

    ext2_inode_t raw;
    int          status = extfs_read_inode_raw(dir_h->sb, dir_h->inode_no, &raw);
    if (status != EOK) return status;
    if ((raw.i_mode & 0xF000) != EXT2_S_IFDIR) return -ENOTDIR;

    uint32_t physical = extfs_map_block(dir_h, 0, 0);
    if (!physical) return -EIO;
    uint8_t *block = malloc(dir_h->sb->block_size);
    if (!block) return -ENOMEM;
    status = extfs_read_block(dir_h->sb, physical, block);
    if (status != EOK || !extfs_dir_block_verify(dir_h, 0, block)) {
        free(block);
        return status != EOK ? status : -EIO;
    }

    ext2_dir_entry_t *dot    = (ext2_dir_entry_t *)block;
    ext2_dir_entry_t *dotdot = dot->rec_len <= dir_h->sb->block_size - 8 ? (ext2_dir_entry_t *)(block + dot->rec_len) : NULL;
    if (!dotdot || !extfs_dirent_valid(dir_h->sb, dot, 0) || !extfs_dirent_valid(dir_h->sb, dotdot, dot->rec_len) || dot->name_len != 1 || dot->name[0] != '.' || dotdot->name_len != 2
        || dotdot->name[0] != '.' || dotdot->name[1] != '.') {
        free(block);
        return -EIO;
    }
    dotdot->inode = parent_ino;
    status        = extfs_dir_write_leaf(dir_h, physical, block);
    free(block);
    return status;
}

/* Check whether a directory contains only "." and "..". */
int extfs_dir_empty(extfs_handle_t *dir_h)
{
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    uint32_t         total_offset = 0, block_num = 0, phys;

    if (!dir_h || !dir_h->sb) return -EINVAL;
    sb = dir_h->sb;

    if (extfs_read_inode_raw(sb, dir_h->inode_no, &raw) != EOK) return -EIO;
    if ((raw.i_mode & 0xF000) != EXT2_S_IFDIR) return -ENOTDIR;

    uint64_t dir_size = raw.i_size;

    block_buf = malloc(sb->block_size);
    if (!block_buf) return -ENOMEM;

    while (total_offset < dir_size) {
        uint32_t boff = total_offset % sb->block_size;

        if (boff == 0) {
            phys = extfs_map_block(dir_h, block_num, 0);

            if (phys == 0) {
                total_offset += sb->block_size;
                block_num++;
                continue;
            }

            if (extfs_read_block(sb, phys, block_buf) != EOK) {
                free(block_buf);
                return -EIO;
            }
            if (!extfs_dir_block_verify(dir_h, block_num, block_buf)) {
                free(block_buf);
                return -EIO;
            }
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + boff);
        if (!extfs_dirent_valid(sb, de, boff)) {
            free(block_buf);
            return -EIO;
        }

        if (de->inode != 0) {
            if (!(de->name_len == 1 && de->name[0] == '.') && !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')) {
                free(block_buf);
                return 0;
            }
        }

        total_offset += de->rec_len;
        if (boff + de->rec_len >= sb->block_size) block_num++;
    }

    free(block_buf);
    return 1;
}
