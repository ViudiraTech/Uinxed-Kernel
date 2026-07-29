/*
 *
 *      dir.c
 *      ext2/ext3/ext4 filesystem - directory operations
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

static uint32_t extfs_dir_rec_len(uint32_t name_len)
{
    return EXT2_DIR_REC_LEN(name_len);
}

int extfs_dir_lookup(extfs_handle_t *dir_h, const char *name, uint32_t *ino)
{
    extfs_sb_info_t *sb = dir_h->sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    uint32_t         offset = 0, block_num = 0, phys;
    uint32_t         name_len;
    int              found = 0;

    if (!dir_h || !name || !ino) return -EINVAL;

    if (extfs_read_inode_raw(sb, dir_h->inode_no, &raw) != EOK) return -EIO;

    name_len = strlen(name);

    block_buf = malloc(sb->block_size);
    if (!block_buf) return -ENOMEM;

    uint64_t dir_size     = raw.i_size;
    uint32_t block_offset = 0;

    while (block_offset < dir_size) {
        if (offset == 0 || offset >= sb->block_size) {
            if (offset >= sb->block_size) block_num++;
            offset = 0;

            if (block_num < EXT2_NDIR_BLOCKS) {
                phys = raw.i_block[block_num];
            } else {
                break;
            }

            if (phys == 0) {
                block_offset += sb->block_size;
                offset = sb->block_size;
                continue;
            }

            if (extfs_read_block(sb, phys, block_buf) != EOK) {
                free(block_buf);
                return -EIO;
            }
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + offset);
        if (de->rec_len == 0) {
            offset = sb->block_size;
            continue;
        }

        if (de->inode != 0 && de->name_len == name_len && memcmp(de->name, name, name_len) == 0) {
            *ino  = de->inode;
            found = 1;
            break;
        }

        offset += de->rec_len;
        block_offset += (offset >= sb->block_size) ? 0 : 0;

        if (offset >= sb->block_size) { block_offset = (block_num + 1) * sb->block_size; }
    }

    free(block_buf);
    return found ? EOK : -ENOENT;
}

int extfs_dir_add_entry(extfs_handle_t *dir_h, const char *name, uint32_t ino, uint8_t file_type)
{
    extfs_sb_info_t *sb = dir_h->sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    uint32_t         name_len, rec_len;
    uint32_t         block_num = 0;
    uint32_t         phys      = 0;
    int              status;

    if (!dir_h || !name || ino == 0) return -EINVAL;

    name_len = strlen(name);
    if (name_len > EXT2_NAME_LEN) return -ENAMETOOLONG;

    rec_len = extfs_dir_rec_len(name_len);

    status = extfs_read_inode_raw(sb, dir_h->inode_no, &raw);
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
            if (block_num < EXT2_NDIR_BLOCKS)
                phys = raw.i_block[block_num];
            else
                break;

            if (phys == 0) break;

            if (extfs_read_block(sb, phys, block_buf) != EOK) {
                free(block_buf);
                return -EIO;
            }
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + boff);
        if (de->rec_len == 0) {
            total_offset = ((total_offset / sb->block_size) + 1) * sb->block_size;
            continue;
        }

        if (de->inode == 0 && de->rec_len >= rec_len) {
            /* Use this free slot */
            de->inode     = ino;
            de->name_len  = name_len;
            de->file_type = file_type;
            de->rec_len   = de->rec_len;
            memcpy(de->name, name, name_len);

            if (extfs_write_block(sb, phys, block_buf) != EOK) {
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
            new_de->file_type        = file_type;
            new_de->rec_len          = old_rec - actual_len;
            memcpy(new_de->name, name, name_len);

            if (extfs_write_block(sb, phys, block_buf) != EOK) {
                free(block_buf);
                return -EIO;
            }
            found_space = 1;
            break;
        }

        total_offset += de->rec_len;
        if (total_offset % sb->block_size < de->rec_len % sb->block_size) { block_num++; }
    }

    if (!found_space) {
        /* Extend directory by one block */
        uint32_t new_block;
        status = extfs_alloc_block(sb, 0, &new_block);
        if (status != EOK) {
            free(block_buf);
            return status;
        }

        memset(block_buf, 0, sb->block_size);

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)block_buf;
        de->inode            = ino;
        de->name_len         = name_len;
        de->file_type        = file_type;
        de->rec_len          = sb->block_size;
        memcpy(de->name, name, name_len);

        if (extfs_write_block(sb, new_block, block_buf) != EOK) {
            extfs_free_block(sb, new_block);
            free(block_buf);
            return -EIO;
        }

        /* Add block pointer to inode */
        uint32_t last_block = (uint32_t)(dir_size / sb->block_size);
        if (last_block < EXT2_NDIR_BLOCKS) {
            raw.i_block[last_block] = new_block;
        } else {
            /* For simplicity, only support direct blocks in directories */
            extfs_free_block(sb, new_block);
            free(block_buf);
            return -ENOSPC;
        }

        raw.i_size   = dir_size + sb->block_size;
        raw.i_blocks = (last_block + 1) * (sb->block_size / 512);

        status = extfs_write_inode_raw(sb, dir_h->inode_no, &raw);
        if (status != EOK) {
            free(block_buf);
            return status;
        }
    }

    /* Update directory size in handle */
    if (extfs_read_inode_raw(sb, dir_h->inode_no, &raw) == EOK) { dir_h->ei.i_data[0] = raw.i_block[0]; }

    free(block_buf);
    return EOK;
}

int extfs_dir_remove_entry(extfs_handle_t *dir_h, const char *name)
{
    extfs_sb_info_t *sb = dir_h->sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    uint32_t         name_len;
    uint32_t         block_num      = 0;
    uint32_t         phys           = 0;
    uint32_t         prev_offset    = 0;
    uint32_t         prev_block_num = 0;
    uint8_t         *prev_block_buf = 0;
    int              found          = 0;

    if (!dir_h || !name) return -EINVAL;

    name_len = strlen(name);

    if (extfs_read_inode_raw(sb, dir_h->inode_no, &raw) != EOK) return -EIO;

    uint64_t dir_size = raw.i_size;
    block_buf         = malloc(sb->block_size);
    if (!block_buf) return -ENOMEM;

    uint32_t total_offset = 0;

    while (total_offset < dir_size) {
        uint32_t boff = total_offset % sb->block_size;

        if (boff == 0) {
            if (block_num < EXT2_NDIR_BLOCKS)
                phys = raw.i_block[block_num];
            else
                break;

            if (phys == 0) break;

            if (extfs_read_block(sb, phys, block_buf) != EOK) {
                free(block_buf);
                if (prev_block_buf) free(prev_block_buf);
                return -EIO;
            }
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + boff);
        if (de->rec_len == 0) {
            total_offset = ((total_offset / sb->block_size) + 1) * sb->block_size;
            block_num++;
            continue;
        }

        if (de->inode != 0 && de->name_len == name_len && memcmp(de->name, name, name_len) == 0) {
            /* Found: merge with previous entry or mark as deleted */
            if (prev_block_buf && prev_offset != total_offset) {
                ext2_dir_entry_t *prev_de = (ext2_dir_entry_t *)(prev_block_buf + (prev_offset % sb->block_size));
                prev_de->rec_len += de->rec_len;
                extfs_write_block(sb, raw.i_block[prev_block_num], prev_block_buf);
            } else {
                de->inode = 0;
                extfs_write_block(sb, phys, block_buf);
            }
            found = 1;
            break;
        }

        prev_offset    = total_offset;
        prev_block_num = block_num;
        if (!prev_block_buf) prev_block_buf = malloc(sb->block_size);
        if (prev_block_buf) memcpy(prev_block_buf, block_buf, sb->block_size);

        total_offset += de->rec_len;
        if (boff + de->rec_len >= sb->block_size) { block_num++; }
    }

    if (prev_block_buf) free(prev_block_buf);
    free(block_buf);
    return found ? EOK : -ENOENT;
}

int extfs_dir_read_entries(extfs_handle_t *dir_h, void *buf, size_t bufsize, size_t *done)
{
    extfs_sb_info_t *sb = dir_h->sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    uint8_t         *out          = buf;
    size_t           written      = 0;
    uint32_t         total_offset = 0, block_num = 0, phys;

    *done = 0;

    if (!dir_h || !buf || bufsize == 0) return -EINVAL;

    if (extfs_read_inode_raw(sb, dir_h->inode_no, &raw) != EOK) return -EIO;

    uint64_t dir_size = raw.i_size;

    block_buf = malloc(sb->block_size);
    if (!block_buf) return -ENOMEM;

    while (total_offset < dir_size && written < bufsize) {
        uint32_t boff = total_offset % sb->block_size;

        if (boff == 0) {
            if (block_num < EXT2_NDIR_BLOCKS)
                phys = raw.i_block[block_num];
            else
                break;

            if (phys == 0) {
                total_offset += sb->block_size;
                block_num++;
                continue;
            }

            if (extfs_read_block(sb, phys, block_buf) != EOK) {
                free(block_buf);
                return -EIO;
            }
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + boff);
        if (de->rec_len == 0) {
            total_offset = ((total_offset / sb->block_size) + 1) * sb->block_size;
            block_num++;
            continue;
        }

        if (de->inode != 0) {
            /* Each entry: inode(4) + name_len(1) + file_type(1) + name */
            uint32_t entry_size = 8 + de->name_len;
            if (written + entry_size <= bufsize) {
                memcpy(out + written, &de->inode, 4);
                out[written + 4] = de->name_len;
                out[written + 5] = de->file_type;
                memcpy(out + written + 6, de->name, de->name_len);
                written += entry_size;
            }
        }

        total_offset += de->rec_len;
        if (boff + de->rec_len >= sb->block_size) { block_num++; }
    }

    free(block_buf);
    *done = written;
    return EOK;
}

int extfs_make_empty_dir(extfs_handle_t *dir_h, uint32_t self_ino, uint32_t parent_ino)
{
    extfs_sb_info_t *sb = dir_h->sb;
    uint8_t         *block_buf;
    int              status;
    uint32_t         new_block;

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
    de2->inode            = parent_ino;
    de2->rec_len          = sb->block_size - de->rec_len;
    de2->name_len         = 2;
    de2->file_type        = EXT2_FT_DIR;
    de2->name[0]          = '.';
    de2->name[1]          = '.';

    status = extfs_alloc_block(sb, 0, &new_block);
    if (status != EOK) {
        free(block_buf);
        return status;
    }

    status = extfs_write_block(sb, new_block, block_buf);
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

    raw.i_block[0]    = new_block;
    raw.i_size        = sb->block_size;
    raw.i_blocks      = sb->block_size / 512;
    raw.i_links_count = 2;

    status = extfs_write_inode_raw(sb, dir_h->inode_no, &raw);
    if (status != EOK) {
        extfs_free_block(sb, new_block);
        return status;
    }

    dir_h->ei.i_data[0] = new_block;

    return EOK;
}

int extfs_dir_empty(extfs_handle_t *dir_h)
{
    extfs_sb_info_t *sb = dir_h->sb;
    ext2_inode_t     raw;
    uint8_t         *block_buf;
    uint32_t         total_offset = 0, block_num = 0, phys;

    if (!dir_h) return -EINVAL;

    if (extfs_read_inode_raw(sb, dir_h->inode_no, &raw) != EOK) return -EIO;

    uint64_t dir_size = raw.i_size;

    block_buf = malloc(sb->block_size);
    if (!block_buf) return -ENOMEM;

    while (total_offset < dir_size) {
        uint32_t boff = total_offset % sb->block_size;

        if (boff == 0) {
            if (block_num < EXT2_NDIR_BLOCKS)
                phys = raw.i_block[block_num];
            else
                break;

            if (phys == 0) {
                total_offset += sb->block_size;
                block_num++;
                continue;
            }

            if (extfs_read_block(sb, phys, block_buf) != EOK) {
                free(block_buf);
                return -EIO;
            }
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block_buf + boff);
        if (de->rec_len == 0) {
            total_offset = ((total_offset / sb->block_size) + 1) * sb->block_size;
            block_num++;
            continue;
        }

        if (de->inode != 0) {
            if (!(de->name_len == 1 && de->name[0] == '.') && !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')) {
                free(block_buf);
                return 0;
            }
        }

        total_offset += de->rec_len;
        if (boff + de->rec_len >= sb->block_size) { block_num++; }
    }

    free(block_buf);
    return 1;
}
