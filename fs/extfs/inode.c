/*
 *
 *      inode.c
 *      ext2/ext3/ext4 filesystem - inode operations and block mapping
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/extfs/extfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <libs/data/crc32c.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>

static int extfs_update_i_blocks(extfs_handle_t *h, ext2_inode_t *raw);

extfs_handle_t *extfs_alloc_handle(extfs_sb_info_t *sb, uint32_t ino)
{
    extfs_handle_t *h;

    h = calloc(1, sizeof(extfs_handle_t));
    if (!h) return 0;

    h->sb       = sb;
    h->inode_no = ino;
    h->owns_sb  = 0;

    if (extfs_load_inode(h) != EOK) {
        free(h);
        return 0;
    }

    return h;
}

int extfs_load_inode(extfs_handle_t *h)
{
    ext2_inode_t raw;
    int          status;

    if (!h || !h->sb) return -EINVAL;

    status = extfs_read_inode_raw(h->sb, h->inode_no, &raw);
    if (status != EOK) return status;

    memcpy(h->ei.i_data, raw.i_block, sizeof(h->ei.i_data));
    h->ei.i_flags       = raw.i_flags;
    h->ei.i_file_acl    = raw.i_file_acl;
    h->ei.i_dir_acl     = raw.i_dir_acl;
    h->ei.i_dtime       = raw.i_dtime;
    h->ei.i_block_group = (h->inode_no - 1) / h->sb->inodes_per_group;

    return EOK;
}

int extfs_flush_inode(extfs_handle_t *h)
{
    ext2_inode_t raw;
    int          status;

    if (!h || !h->sb) return -EINVAL;

    status = extfs_read_inode_raw(h->sb, h->inode_no, &raw);
    if (status != EOK) return status;

    memcpy(raw.i_block, h->ei.i_data, sizeof(h->ei.i_data));
    raw.i_flags    = h->ei.i_flags;
    raw.i_file_acl = h->ei.i_file_acl;
    raw.i_dir_acl  = h->ei.i_dir_acl;
    raw.i_dtime    = h->ei.i_dtime;

    return extfs_write_inode_raw(h->sb, h->inode_no, &raw);
}

uint32_t extfs_map_block(extfs_handle_t *h, uint32_t logical, int create)
{
    if (!h || !h->sb) return 0;
    if (h->ei.i_flags & EXT4_EXTENTS_FL) return extfs_extent_map_block(h, logical, create);
    extfs_sb_info_t *sb             = h->sb;
    uint32_t         ptrs_per_block = sb->block_size / sizeof(uint32_t);
    uint32_t         buf[1024];
    uint32_t         phys, indir;
    int              status;

    if (logical < EXT2_NDIR_BLOCKS) {
        phys = h->ei.i_data[logical];
        if (phys == 0 && create) {
            status = extfs_alloc_block(sb, 0, &phys);
            if (status != EOK) return 0;
            h->ei.i_data[logical] = phys;
            memset(buf, 0, sb->block_size);
            extfs_write_block(sb, phys, buf);
        }
        return h->ei.i_data[logical];
    }

    logical -= EXT2_NDIR_BLOCKS;

    /* Single indirect */
    if (logical < ptrs_per_block) {
        if (h->ei.i_data[EXT2_IND_BLOCK] == 0 && create) {
            status = extfs_alloc_block(sb, 0, &indir);
            if (status != EOK) return 0;
            h->ei.i_data[EXT2_IND_BLOCK] = indir;
            memset(buf, 0, sb->block_size);
            extfs_write_block(sb, indir, buf);
        }
        indir = h->ei.i_data[EXT2_IND_BLOCK];
        if (indir == 0) return 0;

        if (extfs_read_block(sb, indir, buf) != EOK) return 0;
        phys = buf[logical];
        if (phys == 0 && create) {
            status = extfs_alloc_block(sb, 0, &phys);
            if (status != EOK) return 0;
            buf[logical] = phys;
            extfs_write_block(sb, indir, buf);
            memset(buf, 0, sb->block_size);
            extfs_write_block(sb, phys, buf);
        }
        return phys;
    }

    logical -= ptrs_per_block;

    /* Double indirect */
    if (logical < (uint32_t)ptrs_per_block * ptrs_per_block) {
        uint32_t idx1 = logical / ptrs_per_block;
        uint32_t idx2 = logical % ptrs_per_block;

        if (h->ei.i_data[EXT2_DIND_BLOCK] == 0 && create) {
            status = extfs_alloc_block(sb, 0, &indir);
            if (status != EOK) return 0;
            h->ei.i_data[EXT2_DIND_BLOCK] = indir;
            memset(buf, 0, sb->block_size);
            extfs_write_block(sb, indir, buf);
        }
        indir = h->ei.i_data[EXT2_DIND_BLOCK];
        if (indir == 0) return 0;

        if (extfs_read_block(sb, indir, buf) != EOK) return 0;
        uint32_t indir2 = buf[idx1];
        if (indir2 == 0 && create) {
            status = extfs_alloc_block(sb, 0, &indir2);
            if (status != EOK) return 0;
            buf[idx1] = indir2;
            extfs_write_block(sb, indir, buf);
            memset(buf, 0, sb->block_size);
            extfs_write_block(sb, indir2, buf);
        }
        if (indir2 == 0) return 0;

        if (extfs_read_block(sb, indir2, buf) != EOK) return 0;
        phys = buf[idx2];
        if (phys == 0 && create) {
            status = extfs_alloc_block(sb, 0, &phys);
            if (status != EOK) return 0;
            buf[idx2] = phys;
            extfs_write_block(sb, indir2, buf);
            memset(buf, 0, sb->block_size);
            extfs_write_block(sb, phys, buf);
        }
        return phys;
    }

    logical -= (uint32_t)ptrs_per_block * ptrs_per_block;

    /* Triple indirect */
    if (logical < (uint32_t)ptrs_per_block * ptrs_per_block * ptrs_per_block) {
        uint32_t idx1 = logical / (ptrs_per_block * ptrs_per_block);
        uint32_t idx2 = (logical / ptrs_per_block) % ptrs_per_block;
        uint32_t idx3 = logical % ptrs_per_block;

        if (h->ei.i_data[EXT2_TIND_BLOCK] == 0 && create) {
            status = extfs_alloc_block(sb, 0, &indir);
            if (status != EOK) return 0;
            h->ei.i_data[EXT2_TIND_BLOCK] = indir;
            memset(buf, 0, sb->block_size);
            extfs_write_block(sb, indir, buf);
        }
        indir = h->ei.i_data[EXT2_TIND_BLOCK];
        if (indir == 0) return 0;

        if (extfs_read_block(sb, indir, buf) != EOK) return 0;
        uint32_t indir2 = buf[idx1];
        if (indir2 == 0 && create) {
            status = extfs_alloc_block(sb, 0, &indir2);
            if (status != EOK) return 0;
            buf[idx1] = indir2;
            extfs_write_block(sb, indir, buf);
            memset(buf, 0, sb->block_size);
            extfs_write_block(sb, indir2, buf);
        }
        if (indir2 == 0) return 0;

        if (extfs_read_block(sb, indir2, buf) != EOK) return 0;
        uint32_t indir3 = buf[idx2];
        if (indir3 == 0 && create) {
            status = extfs_alloc_block(sb, 0, &indir3);
            if (status != EOK) return 0;
            buf[idx2] = indir3;
            extfs_write_block(sb, indir2, buf);
            memset(buf, 0, sb->block_size);
            extfs_write_block(sb, indir3, buf);
        }
        if (indir3 == 0) return 0;

        if (extfs_read_block(sb, indir3, buf) != EOK) return 0;
        phys = buf[idx3];
        if (phys == 0 && create) {
            status = extfs_alloc_block(sb, 0, &phys);
            if (status != EOK) return 0;
            buf[idx3] = phys;
            extfs_write_block(sb, indir3, buf);
            memset(buf, 0, sb->block_size);
            extfs_write_block(sb, phys, buf);
        }
        return phys;
    }

    return 0;
}

int extfs_read_data(extfs_handle_t *h, void *buf, uint64_t offset, size_t size)
{
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    size_t           done = 0;
    int              status;

    if (!h || !h->sb || !buf || size == 0) return 0;
    if (size > (UINT32_MAX >> 1)) return -EFBIG;
    sb = h->sb;

    status = extfs_read_inode_raw(sb, h->inode_no, &raw);
    if (status != EOK) return status;

    uint64_t file_size = raw.i_size;
    if ((raw.i_mode & 0xF000) == EXT2_S_IFREG) file_size |= ((uint64_t)raw.i_dir_acl << 32);

    uint64_t maximum_size = (uint64_t)UINT32_MAX * sb->block_size;
    if (offset >= file_size || offset >= maximum_size) return 0;
    if (size > file_size - offset) size = (size_t)(file_size - offset);
    if (size > maximum_size - offset) size = (size_t)(maximum_size - offset);

    block_buf = malloc(sb->block_size);
    if (!block_buf) return -ENOMEM;

    while (done < size) {
        uint64_t abs_pos   = offset + done;
        uint64_t logical64 = abs_pos / sb->block_size;
        if (logical64 > UINT32_MAX) break;
        uint32_t logical = (uint32_t)logical64;
        uint32_t inblock = (uint32_t)(abs_pos % sb->block_size);
        uint32_t chunk   = (uint32_t)(size - done);
        uint32_t phys;

        if (chunk > sb->block_size - inblock) chunk = sb->block_size - inblock;

        extfs_handle_t tmp_h = *h;
        memcpy(tmp_h.ei.i_data, raw.i_block, sizeof(raw.i_block));
        tmp_h.ei.i_flags = raw.i_flags;
        phys             = extfs_map_block(&tmp_h, logical, 0);

        if (phys == 0) {
            memset((uint8_t *)buf + done, 0, chunk);
            done += chunk;
            continue;
        }

        if (extfs_read_block(sb, phys, block_buf) != EOK) break;

        memcpy((uint8_t *)buf + done, block_buf + inblock, chunk);
        done += chunk;
    }

    free(block_buf);
    return (int)done;
}

int extfs_write_data(extfs_handle_t *h, const void *buf, uint64_t offset, size_t size)
{
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    size_t           done = 0;
    int              status;

    if (!h || !h->sb || !buf || size == 0) return 0;
    sb                    = h->sb;
    uint64_t maximum_size = (uint64_t)UINT32_MAX * sb->block_size;
    if (size > (UINT32_MAX >> 1) || offset >= maximum_size || size > maximum_size - offset) return -EFBIG;

    status = extfs_read_inode_raw(sb, h->inode_no, &raw);
    if (status != EOK) return status;

    block_buf = malloc(sb->block_size);
    if (!block_buf) return -ENOMEM;

    while (done < size) {
        uint64_t abs_pos   = offset + done;
        uint64_t logical64 = abs_pos / sb->block_size;
        if (logical64 > UINT32_MAX) break;
        uint32_t logical = (uint32_t)logical64;
        uint32_t inblock = (uint32_t)(abs_pos % sb->block_size);
        uint32_t chunk   = (uint32_t)(size - done);
        uint32_t phys;

        if (chunk > sb->block_size - inblock) chunk = sb->block_size - inblock;

        phys = extfs_map_block(h, logical, 1);
        if (phys == 0) break;

        if (inblock > 0 || chunk < sb->block_size)
            if (extfs_read_block(sb, phys, block_buf) != EOK) break;

        memcpy(block_buf + inblock, (const uint8_t *)buf + done, chunk);

        if (extfs_write_data_block(sb, phys, block_buf) != EOK) break;

        done += chunk;
    }

    if (done) {
        status = extfs_read_inode_raw(sb, h->inode_no, &raw);
        if (status != EOK) {
            free(block_buf);
            return status;
        }
        uint64_t old_size = raw.i_size;
        if ((raw.i_mode & 0xF000) == EXT2_S_IFREG) old_size |= (uint64_t)raw.i_dir_acl << 32;
        uint64_t new_size = offset + done;
        if (new_size > old_size) {
            raw.i_size = (uint32_t)new_size;
            if ((raw.i_mode & 0xF000) == EXT2_S_IFREG) raw.i_dir_acl = (uint32_t)(new_size >> 32);
        }
        memcpy(raw.i_block, h->ei.i_data, sizeof(raw.i_block));
        status = extfs_update_i_blocks(h, &raw);
        if (status != EOK) {
            free(block_buf);
            return status;
        }
        raw.i_mtime = timer_realtime_seconds32();
        raw.i_ctime = raw.i_mtime;
        status      = extfs_write_inode_raw(sb, h->inode_no, &raw);
        if (status != EOK) {
            free(block_buf);
            return status;
        }
    }

    free(block_buf);
    return (int)done;
}

void extfs_free_inode_blocks(extfs_handle_t *h)
{
    if (!h || !h->sb) return;
    if (h->ei.i_flags & EXT4_EXTENTS_FL) {
        (void)extfs_extent_free_all(h);
        return;
    }
    extfs_sb_info_t *sb = h->sb;
    uint32_t         buf[1024];
    uint32_t         ptrs_per = sb->block_size / sizeof(uint32_t);
    uint32_t         i, j, phys;

    /* Free direct blocks */
    for (i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        phys = h->ei.i_data[i];
        if (phys) {
            extfs_free_block(sb, phys);
            h->ei.i_data[i] = 0;
        }
    }

    /* Free single indirect */
    phys = h->ei.i_data[EXT2_IND_BLOCK];
    if (phys) {
        if (extfs_read_block(sb, phys, buf) == EOK) {
            for (j = 0; j < ptrs_per; j++)
                if (buf[j]) extfs_free_block(sb, buf[j]);
        }
        extfs_free_block(sb, phys);
        h->ei.i_data[EXT2_IND_BLOCK] = 0;
    }

    /* Free double indirect */
    phys = h->ei.i_data[EXT2_DIND_BLOCK];
    if (phys) {
        uint32_t dindir_buf[1024];
        if (extfs_read_block(sb, phys, dindir_buf) == EOK) {
            for (i = 0; i < ptrs_per; i++) {
                if (dindir_buf[i] == 0) continue;
                if (extfs_read_block(sb, dindir_buf[i], buf) == EOK) {
                    for (j = 0; j < ptrs_per; j++)
                        if (buf[j]) extfs_free_block(sb, buf[j]);
                }
                extfs_free_block(sb, dindir_buf[i]);
            }
        }
        extfs_free_block(sb, phys);
        h->ei.i_data[EXT2_DIND_BLOCK] = 0;
    }

    /* Free triple indirect */
    phys = h->ei.i_data[EXT2_TIND_BLOCK];
    if (phys) {
        uint32_t t1[1024], t2[1024];
        if (extfs_read_block(sb, phys, t1) == EOK) {
            for (i = 0; i < ptrs_per; i++) {
                if (t1[i] == 0) continue;
                if (extfs_read_block(sb, t1[i], t2) == EOK) {
                    for (j = 0; j < ptrs_per; j++) {
                        if (t2[j] == 0) continue;
                        uint32_t k;
                        uint32_t t3[1024];
                        if (extfs_read_block(sb, t2[j], t3) == EOK) {
                            for (k = 0; k < ptrs_per; k++)
                                if (t3[k]) extfs_free_block(sb, t3[k]);
                        }
                        extfs_free_block(sb, t2[j]);
                    }
                }
                extfs_free_block(sb, t1[i]);
            }
        }
        extfs_free_block(sb, phys);
        h->ei.i_data[EXT2_TIND_BLOCK] = 0;
    }
}

int extfs_release_xattr_block(extfs_handle_t *h)
{
    if (!h || !h->sb) return -EINVAL;
    extfs_sb_info_t *sb = h->sb;
    ext2_inode_t     raw;
    int              status = extfs_read_inode_raw(sb, h->inode_no, &raw);
    if (status != EOK) return status;
    uint64_t block = raw.i_file_acl | (uint64_t)raw.l_i_file_acl_high << 32;
    if (!block) return EOK;
    if (block >= sb->blocks_count || block > UINT32_MAX) return -EIO;

    uint8_t *buffer = malloc(sb->block_size);
    if (!buffer) return -ENOMEM;
    status = extfs_read_block(sb, (uint32_t)block, buffer);
    uint32_t magic, refcount, blocks;
    if (status == EOK) {
        memcpy(&magic, buffer, sizeof(magic));
        memcpy(&refcount, buffer + 4, sizeof(refcount));
        memcpy(&blocks, buffer + 8, sizeof(blocks));
        if (magic != 0xEA020000U || !refcount || blocks != 1) {
            plogk("extfs: drive %u: inode %u xattr block %llu has invalid header (magic 0x%x)\n", sb->device.drive, h->inode_no,
                  (unsigned long long)block, magic);
            status = -EIO;
        }
    }
    if (status == EOK && (sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)) {
        uint32_t stored;
        memcpy(&stored, buffer + 16, sizeof(stored));
        uint32_t checksum = crc32c_update(sb->checksum_seed, &block, sizeof(block));
        checksum          = crc32c_update(checksum, buffer, 16);
        uint32_t zero     = 0;
        checksum          = crc32c_update(checksum, &zero, sizeof(zero));
        checksum          = crc32c_update(checksum, buffer + 20, sb->block_size - 20);
        if (stored != checksum) {
            plogk("extfs: drive %u: inode %u xattr block %llu checksum mismatch\n", sb->device.drive, h->inode_no, (unsigned long long)block);
            status = -EIO;
        }
    }
    if (status == EOK && refcount > 1) {
        refcount--;
        memcpy(buffer + 4, &refcount, sizeof(refcount));
        if (sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM) {
            uint32_t zero = 0;
            memcpy(buffer + 16, &zero, sizeof(zero));
            uint32_t checksum = crc32c_update(sb->checksum_seed, &block, sizeof(block));
            checksum          = crc32c_update(checksum, buffer, 16);
            checksum          = crc32c_update(checksum, &zero, sizeof(zero));
            checksum          = crc32c_update(checksum, buffer + 20, sb->block_size - 20);
            memcpy(buffer + 16, &checksum, sizeof(checksum));
        }
        status = extfs_write_block(sb, (uint32_t)block, buffer);
    } else if (status == EOK) {
        extfs_free_block(sb, (uint32_t)block);
        if (sb->active_transaction && sb->active_transaction->error) status = sb->active_transaction->error;
    }
    free(buffer);
    if (status == EOK) h->ei.i_file_acl = 0;
    return status;
}

static int extfs_block_array_empty(const uint32_t *blocks, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
        if (blocks[i]) return 0;
    return 1;
}

/* Remove data blocks in [first, last) from an indirect subtree. */
static int extfs_free_branch_range(extfs_sb_info_t *sb, uint32_t block, uint32_t depth, uint64_t first, uint64_t last, int *empty)
{
    uint32_t  ptrs = sb->block_size / sizeof(uint32_t);
    uint32_t *entries;
    uint64_t  span    = 1;
    int       changed = 0;
    int       status  = EOK;

    if (!block || !depth || first >= last) {
        *empty = !block;
        return EOK;
    }
    for (uint32_t level = 1; level < depth; level++) span *= ptrs;
    entries = malloc(sb->block_size);
    if (!entries) return -ENOMEM;
    status = extfs_read_block(sb, block, entries);
    if (status != EOK) {
        free(entries);
        return status;
    }

    uint32_t begin = (uint32_t)(first / span);
    uint32_t end   = (uint32_t)((last + span - 1) / span);
    if (end > ptrs) end = ptrs;
    for (uint32_t i = begin; i < end; i++) {
        uint32_t child = entries[i];
        if (!child) continue;
        uint64_t child_first = first > (uint64_t)i * span ? first - (uint64_t)i * span : 0;
        uint64_t child_last  = last - (uint64_t)i * span;
        if (child_last > span) child_last = span;
        if (depth == 1) {
            entries[i] = 0;
            extfs_free_block(sb, child);
            changed = 1;
        } else {
            int child_empty = 0;
            status          = extfs_free_branch_range(sb, child, depth - 1, child_first, child_last, &child_empty);
            if (status != EOK) break;
            if (child_empty) {
                entries[i] = 0;
                extfs_free_block(sb, child);
                changed = 1;
            }
        }
    }
    *empty = extfs_block_array_empty(entries, ptrs);
    if (status == EOK && changed && !*empty) status = extfs_write_block(sb, block, entries);
    free(entries);
    return status;
}

static int extfs_truncate_block_tree(extfs_handle_t *h, uint64_t first, uint64_t last)
{
    extfs_sb_info_t *sb   = h->sb;
    uint64_t         ptrs = sb->block_size / sizeof(uint32_t);
    uint64_t         base = 0;
    int              status;

    for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        if (i < first || i >= last || !h->ei.i_data[i]) continue;
        uint32_t block  = h->ei.i_data[i];
        h->ei.i_data[i] = 0;
        extfs_free_block(sb, block);
    }

    first = first > EXT2_NDIR_BLOCKS ? first - EXT2_NDIR_BLOCKS : 0;
    last  = last > EXT2_NDIR_BLOCKS ? last - EXT2_NDIR_BLOCKS : 0;
    for (uint32_t depth = 1; depth <= 3 && first < last; depth++) {
        uint64_t span = ptrs;
        for (uint32_t level = 1; level < depth; level++) span *= ptrs;
        uint64_t range_first = first > base ? first - base : 0;
        uint64_t range_last  = last > base ? last - base : 0;
        if (range_last > span) range_last = span;
        if (range_first < range_last) {
            uint32_t root_index = EXT2_IND_BLOCK + depth - 1;
            uint32_t root       = h->ei.i_data[root_index];
            if (root) {
                int empty = 0;
                status    = extfs_free_branch_range(sb, root, depth, range_first, range_last, &empty);
                if (status != EOK) return status;
                if (empty) {
                    h->ei.i_data[root_index] = 0;
                    extfs_free_block(sb, root);
                }
            }
        }
        base += span;
    }
    return EOK;
}

static int extfs_count_branch(extfs_sb_info_t *sb, uint32_t block, uint32_t depth, uint64_t *blocks)
{
    uint32_t *entries;
    uint32_t  ptrs = sb->block_size / sizeof(uint32_t);
    int       status;
    if (!block) return EOK;
    (*blocks)++;
    entries = malloc(sb->block_size);
    if (!entries) return -ENOMEM;
    status = extfs_read_block(sb, block, entries);
    if (status == EOK) {
        for (uint32_t i = 0; i < ptrs; i++) {
            if (!entries[i]) continue;
            if (depth == 1)
                (*blocks)++;
            else if ((status = extfs_count_branch(sb, entries[i], depth - 1, blocks)) != EOK)
                break;
        }
    }
    free(entries);
    return status;
}

static int extfs_update_i_blocks(extfs_handle_t *h, ext2_inode_t *raw)
{
    uint64_t blocks = 0;
    int      status;
    if (h->ei.i_flags & EXT4_EXTENTS_FL) {
        status = extfs_extent_count_blocks(h, &blocks);
        if (status != EOK) return status;
        blocks *= h->sb->block_size / 512;
        if (blocks > UINT32_MAX) return -EOVERFLOW;
        raw->i_blocks = (uint32_t)blocks;
        return EOK;
    }
    for (uint32_t i = 0; i < EXT2_NDIR_BLOCKS; i++)
        if (h->ei.i_data[i]) blocks++;
    for (uint32_t depth = 1; depth <= 3; depth++) {
        status = extfs_count_branch(h->sb, h->ei.i_data[EXT2_IND_BLOCK + depth - 1], depth, &blocks);
        if (status != EOK) return status;
    }
    blocks *= h->sb->block_size / 512;
    if (blocks > UINT32_MAX) return -EOVERFLOW;
    raw->i_blocks = (uint32_t)blocks;
    return EOK;
}

int extfs_truncate(extfs_handle_t *h, uint64_t size)
{
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;
    uint32_t         old_end_block, new_end_block;
    int              status;

    if (!h || !h->sb) return -EINVAL;
    sb = h->sb;
    if (size > (uint64_t)UINT32_MAX * sb->block_size) return -EFBIG;

    status = extfs_read_inode_raw(sb, h->inode_no, &raw);
    if (status != EOK) return status;

    uint64_t old_size = raw.i_size;
    if ((raw.i_mode & 0xF000) == EXT2_S_IFREG) old_size |= ((uint64_t)raw.i_dir_acl << 32);
    if (old_size > (uint64_t)UINT32_MAX * sb->block_size) return -EFBIG;

    old_end_block = (uint32_t)((old_size + sb->block_size - 1) / sb->block_size);
    new_end_block = (uint32_t)((size + sb->block_size - 1) / sb->block_size);

    if (size < old_size) {
        if (h->ei.i_flags & EXT4_EXTENTS_FL) {
            status = extfs_extent_remove_space(h, new_end_block, old_end_block);
            if (status != EOK) return status;
        } else if (new_end_block == 0) {
            extfs_free_inode_blocks(h);
        } else {
            status = extfs_truncate_block_tree(h, new_end_block, old_end_block);
            if (status != EOK) return status;
        }

        if (size && (size % sb->block_size)) {
            uint32_t phys = extfs_map_block(h, (uint32_t)(size / sb->block_size), 0);
            if (phys) {
                uint8_t *tail = malloc(sb->block_size);
                if (!tail) return -ENOMEM;
                status = extfs_read_block(sb, phys, tail);
                if (status == EOK) {
                    memset(tail + size % sb->block_size, 0, sb->block_size - size % sb->block_size);
                    status = extfs_write_data_block(sb, phys, tail);
                }
                free(tail);
                if (status != EOK) return status;
            }
        }

        /* Update inode size */
        raw.i_size = (uint32_t)size;
        if ((raw.i_mode & 0xF000) == EXT2_S_IFREG) raw.i_dir_acl = (uint32_t)(size >> 32);
        memcpy(raw.i_block, h->ei.i_data, sizeof(raw.i_block));
        status = extfs_update_i_blocks(h, &raw);
        if (status != EOK) return status;
        raw.i_mtime = timer_realtime_seconds32();
        raw.i_ctime = raw.i_mtime;
        status      = extfs_write_inode_raw(sb, h->inode_no, &raw);
        if (status != EOK) return status;
    } else if (size > old_size) {
        raw.i_size = (uint32_t)size;
        if ((raw.i_mode & 0xF000) == EXT2_S_IFREG) raw.i_dir_acl = (uint32_t)(size >> 32);
        raw.i_mtime = timer_realtime_seconds32();
        raw.i_ctime = raw.i_mtime;
        status      = extfs_write_inode_raw(sb, h->inode_no, &raw);
        if (status != EOK) return status;
    }

    return EOK;
}
