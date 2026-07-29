/*
 *
 *      inode.c
 *      ext2/ext3/ext4 filesystem - inode operations and block mapping
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

static uint64_t extfs_inode_block_offset(extfs_sb_info_t *sb, uint32_t block)
{
    return (uint64_t)block * sb->block_size;
}

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
    extfs_sb_info_t *sb             = h->sb;
    uint32_t         ptrs_per_block = sb->block_size / sizeof(uint32_t);
    uint32_t         buf[128];
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
    extfs_sb_info_t *sb = h->sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    size_t           done = 0;
    int              status;

    if (!h || !buf || size == 0) return 0;

    status = extfs_read_inode_raw(sb, h->inode_no, &raw);
    if (status != EOK) return status;

    uint64_t file_size = raw.i_size;
    if (raw.i_dir_acl) file_size |= ((uint64_t)raw.i_dir_acl << 32);

    if (offset >= file_size) return 0;
    if (offset + size > file_size) size = (size_t)(file_size - offset);

    block_buf = malloc(sb->block_size);
    if (!block_buf) return -ENOMEM;

    while (done < size) {
        uint64_t abs_pos = offset + done;
        uint32_t logical = (uint32_t)(abs_pos / sb->block_size);
        uint32_t inblock = (uint32_t)(abs_pos % sb->block_size);
        uint32_t chunk   = (uint32_t)(size - done);
        uint32_t phys;

        if (chunk > sb->block_size - inblock) chunk = sb->block_size - inblock;

        /* Map logical to physical using inline i_block */
        if (logical < EXT2_NDIR_BLOCKS) {
            phys = raw.i_block[logical];
        } else {
            /* Need to rebuild in-memory handle temporarily */
            extfs_handle_t tmp_h = *h;
            memcpy(tmp_h.ei.i_data, raw.i_block, sizeof(raw.i_block));
            phys = extfs_map_block(&tmp_h, logical, 0);
        }

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
    extfs_sb_info_t *sb = h->sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    size_t           done = 0;
    int              status;

    if (!h || !buf || size == 0) return 0;

    status = extfs_read_inode_raw(sb, h->inode_no, &raw);
    if (status != EOK) return status;

    block_buf = malloc(sb->block_size);
    if (!block_buf) return -ENOMEM;

    while (done < size) {
        uint64_t abs_pos = offset + done;
        uint32_t logical = (uint32_t)(abs_pos / sb->block_size);
        uint32_t inblock = (uint32_t)(abs_pos % sb->block_size);
        uint32_t chunk   = (uint32_t)(size - done);
        uint32_t phys;

        if (chunk > sb->block_size - inblock) chunk = sb->block_size - inblock;

        phys = extfs_map_block(h, logical, 1);
        if (phys == 0) break;

        if (inblock > 0 || chunk < sb->block_size) {
            if (extfs_read_block(sb, phys, block_buf) != EOK) break;
        }

        memcpy(block_buf + inblock, (const uint8_t *)buf + done, chunk);

        if (extfs_write_block(sb, phys, block_buf) != EOK) break;

        done += chunk;
    }

    /* Update size on disk if needed */
    if (offset + done > (uint64_t)raw.i_size) {
        status = extfs_read_inode_raw(sb, h->inode_no, &raw);
        if (status == EOK) {
            uint64_t new_size = offset + done;
            raw.i_size        = (uint32_t)new_size;
            if (raw.i_dir_acl || new_size > 0xFFFFFFFF) raw.i_dir_acl = (uint32_t)(new_size >> 32);
            raw.i_blocks = 0;
            uint32_t i;
            for (i = 0; i < EXT2_N_BLOCKS; i++) {
                if (raw.i_block[i] != 0) raw.i_blocks++;
            }
            /* Count indirect blocks too - rough estimate */
            raw.i_blocks += (raw.i_blocks > EXT2_NDIR_BLOCKS) ? (raw.i_blocks - EXT2_NDIR_BLOCKS) : 0;
            raw.i_blocks *= (sb->block_size / 512);
            extfs_write_inode_raw(sb, h->inode_no, &raw);
        }
        h->ei.i_data[0] = h->ei.i_data[0]; /* keep in-memory in sync */
    }

    free(block_buf);
    return (int)done;
}

void extfs_free_inode_blocks(extfs_handle_t *h)
{
    extfs_sb_info_t *sb = h->sb;
    uint32_t         buf[128];
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
            for (j = 0; j < ptrs_per; j++) {
                if (buf[j]) extfs_free_block(sb, buf[j]);
            }
        }
        extfs_free_block(sb, phys);
        h->ei.i_data[EXT2_IND_BLOCK] = 0;
    }

    /* Free double indirect */
    phys = h->ei.i_data[EXT2_DIND_BLOCK];
    if (phys) {
        uint32_t dindir_buf[128];
        if (extfs_read_block(sb, phys, dindir_buf) == EOK) {
            for (i = 0; i < ptrs_per; i++) {
                if (dindir_buf[i] == 0) continue;
                if (extfs_read_block(sb, dindir_buf[i], buf) == EOK) {
                    for (j = 0; j < ptrs_per; j++) {
                        if (buf[j]) extfs_free_block(sb, buf[j]);
                    }
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
        uint32_t t1[128], t2[128];
        if (extfs_read_block(sb, phys, t1) == EOK) {
            for (i = 0; i < ptrs_per; i++) {
                if (t1[i] == 0) continue;
                if (extfs_read_block(sb, t1[i], t2) == EOK) {
                    for (j = 0; j < ptrs_per; j++) {
                        if (t2[j] == 0) continue;
                        uint32_t k;
                        uint32_t t3[128];
                        if (extfs_read_block(sb, t2[j], t3) == EOK) {
                            for (k = 0; k < ptrs_per; k++) {
                                if (t3[k]) extfs_free_block(sb, t3[k]);
                            }
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

int extfs_truncate(extfs_handle_t *h, uint64_t size)
{
    extfs_sb_info_t *sb = h->sb;
    ext2_inode_t     raw;
    uint32_t         old_end_block, new_end_block;
    uint32_t         i;
    int              status;

    if (!h) return -EINVAL;

    status = extfs_read_inode_raw(sb, h->inode_no, &raw);
    if (status != EOK) return status;

    uint64_t old_size = raw.i_size;
    if (raw.i_dir_acl) old_size |= ((uint64_t)raw.i_dir_acl << 32);

    old_end_block = (uint32_t)((old_size + sb->block_size - 1) / sb->block_size);
    new_end_block = (uint32_t)((size + sb->block_size - 1) / sb->block_size);

    if (size < old_size) {
        /* Free blocks beyond new size */
        for (i = new_end_block; i < old_end_block; i++) {
            uint32_t phys = extfs_map_block(h, i, 0);
            if (phys) extfs_free_block(sb, phys);
        }

        /* Update inode size */
        raw.i_size    = (uint32_t)size;
        raw.i_dir_acl = (uint32_t)(size >> 32);
        extfs_write_inode_raw(sb, h->inode_no, &raw);
    }

    return EOK;
}