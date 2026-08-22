/*
 *
 *      extfs.c
 *      ext2/ext3/ext4 filesystem - VFS integration
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/block/ata/pata/ide.h>
#include <drivers/block/core/blockdev.h>
#include <fs/core/vfs.h>
#include <fs/devtmpfs/devtmpfs.h>
#include <fs/extfs/extfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>

static int extfs_id = 0;

/*
 * Overview
 * extfs.c is the VFS front-end for the ext2/3/4 inode engine in this
 * directory: it maps VFS nodes onto extfs handles, converts mode
 * bits, and implements read/write/lookup through the lower-level
 * inode/dir/extents/journal helpers.
 */

/* Map an ext2 mode type to the VFS node type. */
static uint16_t extfs_mode_to_vfs(uint16_t mode)
{
    switch (mode & 0xF000) {
        case EXT2_S_IFDIR :
            return file_dir;
        case EXT2_S_IFLNK :
            return file_symlink;
        case EXT2_S_IFBLK :
            return file_block;
        case EXT2_S_IFCHR :
            return file_stream;
        case EXT2_S_IFIFO :
            return file_pipe;
        case EXT2_S_IFSOCK :
            return file_socket;
        default :
            return file_none;
    }
}

/* Fill a VFS node from its on-disk inode. */
static void extfs_fill_node(vfs_node_t node, extfs_handle_t *h)
{
    extfs_sb_info_t *sb = h->sb;
    ext2_inode_t     raw;

    if (extfs_read_inode_raw(sb, h->inode_no, &raw) != EOK) return;

    node->inode       = h->inode_no;
    node->type        = extfs_mode_to_vfs(raw.i_mode);
    node->permissions = raw.i_mode & 0xFFF;
    node->owner       = raw.i_uid | (raw.l_i_uid_high << 16);
    node->group       = raw.i_gid | (raw.l_i_gid_high << 16);
    node->blksz       = sb->block_size;
    node->createtime  = raw.i_ctime;
    node->readtime    = raw.i_atime;
    node->writetime   = raw.i_mtime;
    node->size        = raw.i_size;
    node->realsize    = raw.i_size;
    if ((raw.i_mode & 0xF000) == EXT2_S_IFREG) node->size = node->realsize = ((uint64_t)raw.i_dir_acl << 32) | raw.i_size;
}

/* Return the extfs handle bound to a VFS node. */
static extfs_handle_t *extfs_get_handle(vfs_node_t node)
{
    if (!node || !node->handle) return 0;
    return (extfs_handle_t *)node->handle;
}

/* Initialize the timestamps and (for ext4) an empty extent root. */
static void extfs_init_new_inode(extfs_sb_info_t *sb, ext2_inode_t *raw)
{
    uint32_t now = timer_realtime_seconds32();
    raw->i_atime = now;
    raw->i_ctime = now;
    raw->i_mtime = now;
    if (!(sb->es->s_feature_incompat & EXT4_FEATURE_INCOMPAT_EXTENTS)) return;
    raw->i_flags |= EXT4_EXTENTS_FL;
    uint8_t header[12] = {
        (uint8_t)EXT4_EXT_MAGIC, (uint8_t)(EXT4_EXT_MAGIC >> 8), 0, 0, (uint8_t)((sizeof(raw->i_block) - 12) / 12), 0, 0, 0, 0, 0, 0, 0,
    };
    memcpy(raw->i_block, header, sizeof(header));
}

/* Refresh an inode's ctime (and optionally mtime) on disk. */
static int extfs_touch_inode(extfs_sb_info_t *sb, uint32_t ino, int modify)
{
    ext2_inode_t raw;
    int          status = extfs_read_inode_raw(sb, ino, &raw);
    if (status != EOK) return status;
    raw.i_ctime = timer_realtime_seconds32();
    if (modify) raw.i_mtime = raw.i_ctime;
    return extfs_write_inode_raw(sb, ino, &raw);
}

/* Validate a directory entry's length and inode reference. */
static int extfs_valid_dirent(extfs_sb_info_t *sb, ext2_dir_entry_t *de, uint32_t offset)
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

/* Materialize the children of a directory into the VFS node tree. */
static int extfs_load_directory(vfs_node_t node)
{
    extfs_handle_t  *h = extfs_get_handle(node);
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;
    uint8_t         *block;
    uint64_t         offset = 0;

    if (!h || !(node->type & file_dir)) return -ENOTDIR;
    if (node->visited) return EOK;
    sb = h->sb;
    if (extfs_read_inode_raw(sb, h->inode_no, &raw) != EOK) return -EIO;
    block = malloc(sb->block_size);
    if (!block) return -ENOMEM;

    while (offset < raw.i_size) {
        uint32_t logical = (uint32_t)(offset / sb->block_size);
        uint32_t boff    = (uint32_t)(offset % sb->block_size);
        uint32_t phys    = extfs_map_block(h, logical, 0);
        if (!phys || extfs_read_block(sb, phys, block) != EOK) {
            free(block);
            return -EIO;
        }
        if (!extfs_dir_block_verify(h, logical, block)) {
            free(block);
            return -EIO;
        }

        ext2_dir_entry_t *de = (ext2_dir_entry_t *)(block + boff);
        if (!extfs_valid_dirent(sb, de, boff)) {
            free(block);
            return -EIO;
        }
        if (de->inode && !(de->name_len == 1 && de->name[0] == '.') && !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')) {
            char name[EXT2_NAME_LEN + 1];
            memcpy(name, de->name, de->name_len);
            name[de->name_len] = '\0';
            if (!vfs_do_search(node, name)) {
                extfs_handle_t *child_h = extfs_alloc_handle(sb, de->inode);
                if (!child_h) {
                    free(block);
                    return -EIO;
                }
                vfs_node_t child = vfs_node_alloc(node, name);
                if (!child) {
                    free(child_h);
                    free(block);
                    return -ENOMEM;
                }
                child->handle = child_h;
                extfs_fill_node(child, child_h);
            }
        }
        offset += de->rec_len;
    }

    free(block);
    node->visited = 1;
    return EOK;
}

/* Mount an ext volume, opening the backing device and root inode. */
static int extfs_mount(const char *src, vfs_node_t node)
{
    extfs_sb_info_t  *sb;
    extfs_handle_t   *h;
    blockdev_device_t device;
    bool              device_retained = false;
    int               status;

    if (!src || !node) return -EINVAL;

    sb = calloc(1, sizeof(extfs_sb_info_t));
    if (!sb) return -ENOMEM;

    /*
     * A /dev name is only a namespace label.  In particular, removable USB
     * disks and AHCI disks can both use sdX names, so reparsing the string
     * cannot identify the correct backend.  Prefer the descriptor bound to
     * the device node and retain the legacy parser for kernel-only names.
     */
    status = devtmpfs_open_block_device(src, &device);
    if (status == EOK) {
        device_retained = true;
    } else if (status == -ENOENT) {
        status = blockdev_open_name(src, &device);
    }

    if (status == EOK) {
        status = extfs_read_super(sb, &device);
        if (device_retained) blockdev_release(&device);
    }
    if (status != EOK) {
        free(sb);
        return status;
    }

    h = extfs_alloc_handle(sb, EXT2_ROOT_INO);
    if (!h) {
        extfs_free_super(sb);
        free(sb);
        return -EIO;
    }

    h->owns_sb   = 1;
    node->handle = h;
    extfs_fill_node(node, h);
    if (!(node->type & file_dir) || !node->size) {
        extfs_free_super(sb);
        free(sb);
        free(h);
        node->handle = 0;
        return -EIO;
    }
    node->visited = 0;
    status        = extfs_load_directory(node);
    if (status != EOK) {
        extfs_free_super(sb);
        free(sb);
        free(h);
        node->handle = 0;
        return status;
    }

    if (!sb->read_only) {
        sb->es->s_state &= (uint16_t)~EXT2_VALID_FS;
        if (sb->journal) sb->es->s_feature_incompat |= EXT3_FEATURE_INCOMPAT_RECOVER;
        status = extfs_write_super(sb);
        if (status == EOK) status = blockdev_flush(&sb->device);
        if (status != EOK) {
            extfs_free_super(sb);
            free(sb);
            free(h);
            node->handle = 0;
            return status;
        }
    }

    plogk("extfs: Mounted ext%d volume '%.16s', block size %u, %u groups.\n", extfs_detect_version(sb->es), sb->es->s_volume_name, sb->block_size, sb->groups_count);

    return EOK;
}

/* Release the superblock and root handle on unmount. */
static void extfs_unmount(void *root)
{
    vfs_node_t      node = root;
    extfs_handle_t *h;

    if (!node) return;

    h = extfs_get_handle(node);
    if (!h) return;

    if (h->owns_sb && h->sb) {
        if (!h->sb->read_only && blockdev_flush(&h->sb->device) == EOK) {
            h->sb->es->s_state |= EXT2_VALID_FS;
            if (h->sb->journal) h->sb->es->s_feature_incompat &= ~EXT3_FEATURE_INCOMPAT_RECOVER;
            if (extfs_write_super(h->sb) == EOK) (void)blockdev_flush(&h->sb->device);
        }
        extfs_free_super(h->sb);
        free(h->sb);
    }
    free(h);
    node->handle = 0;
}

/* Bind a child VFS node to its on-disk inode. */
static void extfs_open(void *parent, const char *name, vfs_node_t node)
{
    extfs_handle_t *parent_h;
    uint32_t        child_ino;
    extfs_handle_t *child_h;

    if (!parent || !name || !node) return;

    parent_h = extfs_get_handle(node->parent);
    if (!parent_h) return;

    if (extfs_dir_lookup(parent_h, name, &child_ino) != EOK) return;

    child_h = extfs_alloc_handle(parent_h->sb, child_ino);
    if (!child_h) return;

    node->handle = child_h;
    extfs_fill_node(node, child_h);
    if (node->type & file_dir) (void)extfs_load_directory(node);
}

/* Release a file handle (no-op for extfs). */
static void extfs_close(void *current)
{
    (void)current;
}

/* Read file data through the inode block mapper. */
static size_t extfs_read_file(void *file, void *addr, size_t offset, size_t size)
{
    extfs_handle_t *h = file;
    if (!h) return 0;
    int r = extfs_read_data(h, addr, offset, size);
    return r > 0 ? (size_t)r : 0;
}

/* Write file data inside a journal transaction. */
static size_t extfs_write_file(void *file, const void *addr, size_t offset, size_t size)
{
    extfs_handle_t *h = file;
    fs_txn_t        transaction;
    int             status;
    if (!h) return 0;
    status = extfs_transaction_begin(h->sb, &transaction, 8192);
    if (status != EOK) return 0;
    int written = extfs_write_data(h, addr, offset, size);
    if (written < 0 || (size_t)written != size) {
        extfs_transaction_abort(h->sb, &transaction, written < 0 ? written : -EIO);
        (void)extfs_load_inode(h);
        return 0;
    }
    status = extfs_transaction_commit(h->sb, &transaction);
    if (status != EOK) (void)extfs_load_inode(h);
    return status == EOK ? (size_t)written : 0;
}

/* Resize a file inside a journal transaction. */
static int extfs_resize(void *file, uint64_t size)
{
    extfs_handle_t *h = file;
    fs_txn_t        transaction;
    int             status;
    if (!h) return -EINVAL;
    status = extfs_transaction_begin(h->sb, &transaction, 8192);
    if (status != EOK) return status;
    status = extfs_truncate(h, size);
    if (status != EOK) {
        extfs_transaction_abort(h->sb, &transaction, status);
        (void)extfs_load_inode(h);
        return status;
    }
    status = extfs_transaction_commit(h->sb, &transaction);
    if (status != EOK) (void)extfs_load_inode(h);
    return status;
}

/* Flush the inode and device, downgrading the volume to read-only on failure. */
static int extfs_sync(void *file, int data_only)
{
    extfs_handle_t *h = file;
    (void)data_only;
    if (!h) return -EINVAL;
    int status = extfs_flush_inode(h);
    if (status == EOK) status = blockdev_flush(&h->sb->device);
    if (status != EOK) h->sb->read_only = 1;
    return status;
}

/* Read a symlink target, handling the fast and slow inode layouts. */
static size_t extfs_readlink_file(vfs_node_t node, void *addr, size_t offset, size_t size)
{
    extfs_handle_t  *h;
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;

    if (!node) return 0;
    h = extfs_get_handle(node);
    if (!h) return 0;
    sb = h->sb;

    if (extfs_read_inode_raw(sb, h->inode_no, &raw) != EOK) return 0;

    uint32_t symlink_len = raw.i_size;

    if (offset >= symlink_len) return 0;
    if (offset + size > symlink_len) size = symlink_len - offset;

    if (symlink_len <= sizeof(raw.i_block)) {
        memcpy(addr, (uint8_t *)raw.i_block + offset, size);
        return size;
    }
    int status = extfs_read_data(h, addr, offset, size);
    return status > 0 ? (size_t)status : 0;
}

/* Create a directory inode and link it into the parent. */
static int extfs_mkdir_impl(void *parent, const char *name, vfs_node_t node)
{
    extfs_handle_t  *dir_h;
    extfs_sb_info_t *sb;
    uint32_t         new_ino;
    int              status;
    extfs_handle_t  *new_h;
    ext2_inode_t     new_raw;

    if (!parent || !name || !node) return -EINVAL;

    dir_h = extfs_get_handle(node->parent);
    if (!dir_h) return -EINVAL;

    sb = dir_h->sb;

    uint32_t dummy;
    status = extfs_dir_lookup(dir_h, name, &dummy);
    if (status == EOK) return -EEXIST;
    if (status != -ENOENT) return status;

    ext2_inode_t parent_raw;
    status = extfs_read_inode_raw(sb, dir_h->inode_no, &parent_raw);
    if (status != EOK) return status;
    if (parent_raw.i_links_count == UINT16_MAX) {
        if (!(sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_DIR_NLINK)) return -EMLINK;
        parent_raw.i_links_count = 1;
    } else if (parent_raw.i_links_count != 1 || !(sb->es->s_feature_ro_compat & EXT4_FEATURE_RO_COMPAT_DIR_NLINK)) {
        parent_raw.i_links_count++;
    }

    status = extfs_alloc_inode(sb, &new_ino);
    if (status != EOK) return status;

    memset(&new_raw, 0, sizeof(new_raw));
    extfs_init_new_inode(sb, &new_raw);
    new_raw.i_mode        = EXT2_S_IFDIR | (node->mode & 07777);
    new_raw.i_uid         = (uint16_t)node->owner;
    new_raw.l_i_uid_high  = (uint16_t)(node->owner >> 16);
    new_raw.i_gid         = (uint16_t)node->group;
    new_raw.l_i_gid_high  = (uint16_t)(node->group >> 16);
    new_raw.i_links_count = 2;

    status = extfs_write_inode_raw(sb, new_ino, &new_raw);
    if (status != EOK) {
        extfs_free_inode(sb, new_ino);
        return status;
    }

    new_h = extfs_alloc_handle(sb, new_ino);
    if (!new_h) {
        extfs_free_inode(sb, new_ino);
        return -EIO;
    }

    status = extfs_make_empty_dir(new_h, new_ino, dir_h->inode_no);
    if (status != EOK) {
        free(new_h);
        extfs_free_inode(sb, new_ino);
        return status;
    }

    status = extfs_dir_add_entry(dir_h, name, new_ino, EXT2_FT_DIR);
    if (status != EOK) {
        extfs_free_inode_blocks(new_h);
        extfs_free_inode(sb, new_ino);
        free(new_h);
        return status;
    }

    status = extfs_adjust_used_dirs(sb, new_ino, 1);
    if (status != EOK) {
        extfs_free_inode_blocks(new_h);
        extfs_free_inode(sb, new_ino);
        free(new_h);
        return status;
    }

    parent_raw.i_ctime = timer_realtime_seconds32();
    parent_raw.i_mtime = parent_raw.i_ctime;
    status             = extfs_write_inode_raw(sb, dir_h->inode_no, &parent_raw);
    if (status != EOK) {
        free(new_h);
        return status;
    }

    node->handle = new_h;
    extfs_fill_node(node, new_h);

    return EOK;
}

/* Create a regular file inode and link it into the parent. */
static int extfs_mkfile_impl(void *parent, const char *name, vfs_node_t node)
{
    extfs_handle_t  *dir_h;
    extfs_sb_info_t *sb;
    uint32_t         new_ino;
    int              status;
    extfs_handle_t  *new_h;
    ext2_inode_t     new_raw;

    if (!parent || !name || !node) return -EINVAL;

    dir_h = extfs_get_handle(node->parent);
    if (!dir_h) return -EINVAL;

    sb = dir_h->sb;

    uint32_t dummy;
    status = extfs_dir_lookup(dir_h, name, &dummy);
    if (status == EOK) return -EEXIST;
    if (status != -ENOENT) return status;

    status = extfs_alloc_inode(sb, &new_ino);
    if (status != EOK) return status;

    memset(&new_raw, 0, sizeof(new_raw));
    extfs_init_new_inode(sb, &new_raw);
    new_raw.i_mode        = EXT2_S_IFREG | (node->mode & 07777);
    new_raw.i_uid         = (uint16_t)node->owner;
    new_raw.l_i_uid_high  = (uint16_t)(node->owner >> 16);
    new_raw.i_gid         = (uint16_t)node->group;
    new_raw.l_i_gid_high  = (uint16_t)(node->group >> 16);
    new_raw.i_links_count = 1;

    status = extfs_write_inode_raw(sb, new_ino, &new_raw);
    if (status != EOK) {
        extfs_free_inode(sb, new_ino);
        return status;
    }

    status = extfs_dir_add_entry(dir_h, name, new_ino, EXT2_FT_REG_FILE);
    if (status != EOK) {
        extfs_free_inode(sb, new_ino);
        return status;
    }
    status = extfs_touch_inode(sb, dir_h->inode_no, 1);
    if (status != EOK) return status;

    new_h = extfs_alloc_handle(sb, new_ino);
    if (!new_h) return -EIO;

    node->handle = new_h;
    extfs_fill_node(node, new_h);

    return EOK;
}

/* Create a hard link to an existing inode within the same volume. */
static int extfs_link_impl(void *parent, const char *target_name, vfs_node_t node)
{
    extfs_handle_t  *dir_h;
    extfs_handle_t  *target_h;
    extfs_handle_t  *new_h = NULL;
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;
    vfs_node_t       target = NULL;
    int              status = -EINVAL;
    uint8_t          file_type;

    if (!parent || !target_name || !node || !node->name) return -EINVAL;

    dir_h = extfs_get_handle(node->parent);
    if (!dir_h) return -EINVAL;
    target = vfs_open_nofollow(target_name);
    if (!target) return -ENOENT;
    target_h = extfs_get_handle(target);
    if (!target_h) {
        status = -ENOENT;
        goto out;
    }

    sb = dir_h->sb;
    if (target_h->sb != sb) {
        status = -EXDEV;
        goto out;
    }

    uint32_t dummy;
    status = extfs_dir_lookup(dir_h, node->name, &dummy);
    if (status == EOK) {
        status = -EEXIST;
        goto out;
    }
    if (status != -ENOENT) goto out;

    if (extfs_read_inode_raw(sb, target_h->inode_no, &raw) != EOK) {
        status = -EIO;
        goto out;
    }

    if ((raw.i_mode & 0xF000) == EXT2_S_IFDIR) {
        status = -EPERM;
        goto out;
    }
    if (raw.i_links_count == UINT16_MAX) {
        status = -EMLINK;
        goto out;
    }
    if ((raw.i_mode & 0xF000) == EXT2_S_IFLNK)
        file_type = EXT2_FT_SYMLINK;
    else
        file_type = EXT2_FT_REG_FILE;

    status = extfs_dir_add_entry(dir_h, node->name, target_h->inode_no, file_type);
    if (status != EOK) goto out;

    raw.i_links_count++;
    raw.i_ctime = timer_realtime_seconds32();
    status      = extfs_write_inode_raw(sb, target_h->inode_no, &raw);
    if (status != EOK) goto out;
    status = extfs_touch_inode(sb, dir_h->inode_no, 1);
    if (status != EOK) goto out;

    new_h = extfs_alloc_handle(sb, target_h->inode_no);
    if (!new_h) {
        status = -ENOMEM;
        goto out;
    }
    if (extfs_load_inode(new_h) != EOK) {
        free(new_h);
        status = -EIO;
        goto out;
    }
    node->handle = new_h;
    extfs_fill_node(node, new_h);
    status = EOK;
out:
    vfs_close(target);
    return status;
}

/* Create a symlink inode, using the fast layout for short targets. */
static int extfs_symlink_impl(void *parent, const char *name, vfs_node_t node)
{
    extfs_handle_t  *dir_h;
    extfs_sb_info_t *sb;
    uint32_t         new_ino;
    int              status;
    ext2_inode_t     new_raw;
    extfs_handle_t  *new_h;

    if (!parent || !name || !node) return -EINVAL;

    dir_h = extfs_get_handle(node->parent);
    if (!dir_h) return -EINVAL;

    sb = dir_h->sb;

    uint32_t dummy;
    status = extfs_dir_lookup(dir_h, name, &dummy);
    if (status == EOK) return -EEXIST;
    if (status != -ENOENT) return status;

    status = extfs_alloc_inode(sb, &new_ino);
    if (status != EOK) return status;

    memset(&new_raw, 0, sizeof(new_raw));
    extfs_init_new_inode(sb, &new_raw);
    new_raw.i_mode        = EXT2_S_IFLNK | 0777;
    new_raw.i_uid         = (uint16_t)node->owner;
    new_raw.l_i_uid_high  = (uint16_t)(node->owner >> 16);
    new_raw.i_gid         = (uint16_t)node->group;
    new_raw.l_i_gid_high  = (uint16_t)(node->group >> 16);
    new_raw.i_links_count = 1;

    uint32_t target_len = strlen(node->linkname ? node->linkname : "");
    if (target_len > EXT2_NAME_LEN * 16) {
        extfs_free_inode(sb, new_ino);
        return -ENAMETOOLONG;
    }

    if (target_len <= sizeof(new_raw.i_block)) {
        new_raw.i_flags &= ~EXT4_EXTENTS_FL;
        memset(new_raw.i_block, 0, sizeof(new_raw.i_block));
        new_raw.i_size = target_len;
        memcpy(new_raw.i_block, node->linkname ? node->linkname : "", target_len);
    }

    status = extfs_write_inode_raw(sb, new_ino, &new_raw);
    if (status != EOK) {
        extfs_free_inode(sb, new_ino);
        return status;
    }

    new_h = extfs_alloc_handle(sb, new_ino);
    if (!new_h) {
        extfs_free_inode(sb, new_ino);
        return -EIO;
    }
    if (target_len > sizeof(new_raw.i_block)) {
        status = extfs_write_data(new_h, node->linkname, 0, target_len);
        if (status != (int)target_len) {
            extfs_free_inode_blocks(new_h);
            extfs_free_inode(sb, new_ino);
            free(new_h);
            return status < 0 ? status : -EIO;
        }
    }

    status = extfs_dir_add_entry(dir_h, name, new_ino, EXT2_FT_SYMLINK);
    if (status != EOK) {
        extfs_free_inode_blocks(new_h);
        extfs_free_inode(sb, new_ino);
        free(new_h);
        return status;
    }
    status = extfs_touch_inode(sb, dir_h->inode_no, 1);
    if (status != EOK) {
        free(new_h);
        return status;
    }

    node->handle = new_h;
    node->type   = file_symlink;
    extfs_fill_node(node, new_h);

    return EOK;
}

/* Unlink a file, freeing its inode when the link count reaches zero. */
static int extfs_delete_impl(void *parent, vfs_node_t node)
{
    extfs_handle_t  *dir_h;
    extfs_handle_t  *file_h;
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;
    int              status;

    if (!parent || !node) return -EINVAL;

    dir_h  = extfs_get_handle(parent);
    file_h = extfs_get_handle(node);
    if (!dir_h || !file_h) return -EINVAL;

    sb = dir_h->sb;

    status = extfs_read_inode_raw(sb, file_h->inode_no, &raw);
    if (status != EOK) return status;
    if ((raw.i_mode & 0xF000) == EXT2_S_IFDIR) return -EISDIR;

    status = extfs_dir_remove_entry(dir_h, node->name);
    if (status != EOK) return status;

    if (!raw.i_links_count) {
        plogk("extfs: Inode %llu has zero link count during unlink (corrupt on-disk state)\n", (unsigned long long)file_h->inode_no);
        return -EIO;
    }
    raw.i_links_count--;
    if (raw.i_links_count == 0) {
        extfs_free_inode_blocks(file_h);
        status = extfs_release_xattr_block(file_h);
        if (status != EOK) return status;
        memset(raw.i_block, 0, sizeof(raw.i_block));
        raw.i_size            = 0;
        raw.i_blocks          = 0;
        raw.i_file_acl        = 0;
        raw.l_i_file_acl_high = 0;
        raw.i_dtime           = timer_realtime_seconds32();
        raw.i_ctime           = raw.i_dtime;
        status                = extfs_write_inode_raw(sb, file_h->inode_no, &raw);
        if (status != EOK) return status;
        extfs_free_inode(sb, file_h->inode_no);
    } else {
        raw.i_ctime = timer_realtime_seconds32();
        status      = extfs_write_inode_raw(sb, file_h->inode_no, &raw);
        if (status != EOK) return status;
    }

    return extfs_touch_inode(sb, dir_h->inode_no, 1);
}

/* Remove an empty directory, freeing its inode and blocks. */
static int extfs_rmdir_impl(void *parent, const char *name)
{
    extfs_handle_t  *dir_h;
    extfs_sb_info_t *sb;
    uint32_t         child_ino;
    ext2_inode_t     raw;
    extfs_handle_t  *child_h;
    int              status;

    if (!parent || !name) return -EINVAL;

    dir_h = extfs_get_handle(parent);
    if (!dir_h) return -EINVAL;

    sb = dir_h->sb;

    status = extfs_dir_lookup(dir_h, name, &child_ino);
    if (status != EOK) return status;

    status = extfs_read_inode_raw(sb, child_ino, &raw);
    if (status != EOK) return status;

    if ((raw.i_mode & 0xF000) != EXT2_S_IFDIR) return -ENOTDIR;

    child_h = extfs_alloc_handle(sb, child_ino);
    if (!child_h) return -EIO;

    status = extfs_dir_empty(child_h);
    if (status <= 0) {
        free(child_h);
        return status < 0 ? status : -ENOTEMPTY;
    }

    status = extfs_dir_remove_entry(dir_h, name);
    if (status != EOK) {
        free(child_h);
        return status;
    }

    extfs_free_inode_blocks(child_h);
    status = extfs_release_xattr_block(child_h);
    if (status != EOK) {
        free(child_h);
        return status;
    }
    memset(raw.i_block, 0, sizeof(raw.i_block));
    raw.i_links_count     = 0;
    raw.i_size            = 0;
    raw.i_blocks          = 0;
    raw.i_file_acl        = 0;
    raw.l_i_file_acl_high = 0;
    raw.i_dtime           = timer_realtime_seconds32();
    raw.i_ctime           = raw.i_dtime;
    status                = extfs_write_inode_raw(sb, child_ino, &raw);
    if (status != EOK) {
        free(child_h);
        return status;
    }
    extfs_free_inode(sb, child_ino);
    status = extfs_adjust_used_dirs(sb, child_ino, -1);
    if (status != EOK) {
        free(child_h);
        return status;
    }

    /* Update parent link count */
    ext2_inode_t parent_raw;
    status = extfs_read_inode_raw(sb, dir_h->inode_no, &parent_raw);
    if (status != EOK) {
        free(child_h);
        return status;
    }
    if (parent_raw.i_links_count > 2) parent_raw.i_links_count--;
    parent_raw.i_ctime = timer_realtime_seconds32();
    parent_raw.i_mtime = parent_raw.i_ctime;
    status             = extfs_write_inode_raw(sb, dir_h->inode_no, &parent_raw);
    if (status != EOK) {
        free(child_h);
        return status;
    }

    free(child_h);
    return EOK;
}

/* Adjust a directory's link count by delta. */
static int extfs_adjust_directory_links(extfs_handle_t *directory, int delta)
{
    if (!directory || (delta != -1 && delta != 1)) return -EINVAL;
    ext2_inode_t raw;
    int          status = extfs_read_inode_raw(directory->sb, directory->inode_no, &raw);
    if (status != EOK) return status;
    if ((raw.i_mode & 0xF000) != EXT2_S_IFDIR) return -ENOTDIR;
    if ((delta < 0 && raw.i_links_count <= 2) || (delta > 0 && raw.i_links_count == UINT16_MAX)) return -EMLINK;
    raw.i_links_count = (uint16_t)(raw.i_links_count + delta);
    raw.i_ctime = raw.i_mtime = timer_realtime_seconds32();
    return extfs_write_inode_raw(directory->sb, directory->inode_no, &raw);
}

/* Commit the complete old-parent/source/new-parent/target rename transaction. */
static int extfs_rename_impl(const vfs_rename_context_t *context)
{
    if (!context || !context->source || !context->old_parent || !context->new_parent || !context->new_name) return -EINVAL;
    extfs_handle_t *source_h     = extfs_get_handle(context->source);
    extfs_handle_t *old_parent_h = extfs_get_handle(context->old_parent);
    extfs_handle_t *new_parent_h = extfs_get_handle(context->new_parent);
    extfs_handle_t *target_h     = extfs_get_handle(context->target);
    if (!source_h || !old_parent_h || !new_parent_h || (context->target && !target_h)) return -EINVAL;
    if (source_h->sb != old_parent_h->sb || source_h->sb != new_parent_h->sb || (target_h && source_h->sb != target_h->sb)) return -EXDEV;

    extfs_sb_info_t *sb = source_h->sb;
    ext2_inode_t     raw;
    uint8_t          file_type;
    uint32_t         existing;
    int              status;

    /* Determine file type */
    status = extfs_read_inode_raw(sb, source_h->inode_no, &raw);
    if (status != EOK) return status;

    if ((raw.i_mode & 0xF000) == EXT2_S_IFDIR)
        file_type = EXT2_FT_DIR;
    else if ((raw.i_mode & 0xF000) == EXT2_S_IFLNK)
        file_type = EXT2_FT_SYMLINK;
    else
        file_type = EXT2_FT_REG_FILE;

    status = extfs_dir_lookup(new_parent_h, context->new_name, &existing);
    if (context->target) {
        if (status != EOK) return status;
        if (existing != target_h->inode_no) return -EIO;
        status = (context->target->type & file_dir) ? extfs_rmdir_impl(context->new_parent, context->new_name) : extfs_delete_impl(context->new_parent, context->target);
        if (status != EOK) return status;
    } else if (status != -ENOENT)
        return status == EOK ? -EEXIST : status;

    status = extfs_dir_add_entry(new_parent_h, context->new_name, source_h->inode_no, file_type);
    if (status != EOK) return status;
    status = extfs_dir_remove_entry(old_parent_h, context->source->name);
    if (status != EOK) return status;

    if ((context->source->type & file_dir) && context->old_parent != context->new_parent) {
        status = extfs_dir_set_parent(source_h, new_parent_h->inode_no);
        if (status != EOK) return status;
        status = extfs_adjust_directory_links(old_parent_h, -1);
        if (status != EOK) return status;
        status = extfs_adjust_directory_links(new_parent_h, 1);
        if (status != EOK) return status;
    }
    status = extfs_touch_inode(sb, source_h->inode_no, 0);
    if (status != EOK) return status;
    status = extfs_touch_inode(sb, old_parent_h->inode_no, 1);
    if (status != EOK) return status;
    return context->new_parent == context->old_parent ? EOK : extfs_touch_inode(sb, new_parent_h->inode_no, 1);
}

/* Commit a mutation transaction, aborting it on failure. */
static int extfs_finish_mutation(extfs_sb_info_t *sb, fs_txn_t *transaction, int status)
{
    if (status != EOK) {
        extfs_transaction_abort(sb, transaction, status);
        return status;
    }
    return extfs_transaction_commit(sb, transaction);
}

/* VFS callback: create a directory inside a transaction. */
static int extfs_mkdir_cb(void *parent, const char *name, vfs_node_t node)
{
    extfs_handle_t *h = node ? extfs_get_handle(node->parent) : 0;
    fs_txn_t        transaction;
    int             status;
    if (!h) return -EINVAL;
    status = extfs_transaction_begin(h->sb, &transaction, 8192);
    if (status != EOK) return status;
    status = extfs_mkdir_impl(parent, name, node);
    status = extfs_finish_mutation(h->sb, &transaction, status);
    if (status != EOK) (void)extfs_load_inode(h);
    return status;
}

/* VFS callback: create a regular file inside a transaction. */
static int extfs_mkfile_cb(void *parent, const char *name, vfs_node_t node)
{
    extfs_handle_t *h = node ? extfs_get_handle(node->parent) : 0;
    fs_txn_t        transaction;
    int             status;
    if (!h) return -EINVAL;
    status = extfs_transaction_begin(h->sb, &transaction, 8192);
    if (status != EOK) return status;
    status = extfs_mkfile_impl(parent, name, node);
    status = extfs_finish_mutation(h->sb, &transaction, status);
    if (status != EOK) (void)extfs_load_inode(h);
    return status;
}

/* VFS callback: create a hard link inside a transaction. */
static int extfs_link_cb(void *parent, const char *name, vfs_node_t node)
{
    extfs_handle_t *h = node ? extfs_get_handle(node->parent) : 0;
    fs_txn_t        transaction;
    int             status;
    if (!h) return -EINVAL;
    status = extfs_transaction_begin(h->sb, &transaction, 8192);
    if (status != EOK) return status;
    status = extfs_link_impl(parent, name, node);
    status = extfs_finish_mutation(h->sb, &transaction, status);
    if (status != EOK) (void)extfs_load_inode(h);
    return status;
}

/* VFS callback: create a symlink inside a transaction. */
static int extfs_symlink_cb(void *parent, const char *name, vfs_node_t node)
{
    extfs_handle_t *h = node ? extfs_get_handle(node->parent) : 0;
    fs_txn_t        transaction;
    int             status;
    if (!h) return -EINVAL;
    status = extfs_transaction_begin(h->sb, &transaction, 8192);
    if (status != EOK) return status;
    status = extfs_symlink_impl(parent, name, node);
    status = extfs_finish_mutation(h->sb, &transaction, status);
    if (status != EOK) (void)extfs_load_inode(h);
    return status;
}

/* VFS callback: unlink a file or directory inside a transaction. */
static int extfs_delete(void *parent, vfs_node_t node)
{
    extfs_handle_t *h = extfs_get_handle(parent);
    fs_txn_t        transaction;
    int             status;
    if (!h) return -EINVAL;
    status = extfs_transaction_begin(h->sb, &transaction, 8192);
    if (status != EOK) return status;
    status = node && (node->type & file_dir) ? extfs_rmdir_impl(parent, node->name) : extfs_delete_impl(parent, node);
    status = extfs_finish_mutation(h->sb, &transaction, status);
    if (status != EOK) {
        (void)extfs_load_inode(h);
        if (node && extfs_get_handle(node)) (void)extfs_load_inode(extfs_get_handle(node));
    }
    return status;
}

/* VFS callback: rename inside a transaction. */
static int extfs_rename_cb(const vfs_rename_context_t *context)
{
    vfs_node_t      node = context ? context->source : NULL;
    extfs_handle_t *h    = extfs_get_handle(node);
    fs_txn_t        transaction;
    int             status;
    if (!h) return -EINVAL;
    status = extfs_transaction_begin(h->sb, &transaction, 65536);
    if (status != EOK) return status;
    status = extfs_rename_impl(context);
    status = extfs_finish_mutation(h->sb, &transaction, status);
    if (status != EOK) {
        (void)extfs_load_inode(h);
        extfs_handle_t *old_parent_h = context ? extfs_get_handle(context->old_parent) : 0;
        extfs_handle_t *new_parent_h = context ? extfs_get_handle(context->new_parent) : 0;
        extfs_handle_t *target_h     = context ? extfs_get_handle(context->target) : 0;
        if (old_parent_h) (void)extfs_load_inode(old_parent_h);
        if (new_parent_h && new_parent_h != old_parent_h) (void)extfs_load_inode(new_parent_h);
        if (target_h) (void)extfs_load_inode(target_h);
    }
    return status;
}

/* Refresh a VFS node from its on-disk inode. */
static int extfs_stat(void *file, vfs_node_t node)
{
    extfs_handle_t *h = file;
    if (!h || !node) return -EINVAL;
    extfs_fill_node(node, h);
    return EOK;
}

/* Reject ioctl requests (extfs supports none). */
static int extfs_ioctl_cb(void *file, size_t req, void *arg)
{
    (void)file;
    (void)req;
    (void)arg;
    return -ENOTTY;
}

/* Duplicate a VFS node with a fresh handle to the same inode. */
static vfs_node_t extfs_dup(vfs_node_t node)
{
    vfs_node_t      copy;
    extfs_handle_t *src_h;
    extfs_handle_t *copy_h;

    if (!node) return 0;

    src_h = extfs_get_handle(node);

    copy = vfs_node_alloc(node->parent, node->name);
    if (!copy) return 0;

    if (src_h) {
        copy_h = extfs_alloc_handle(src_h->sb, src_h->inode_no);
        if (!copy_h) {
            free(copy);
            return 0;
        }
        copy->handle = copy_h;
    }

    copy->type        = node->type;
    copy->size        = node->size;
    copy->realsize    = node->realsize;
    copy->blksz       = node->blksz;
    copy->inode       = node->inode;
    copy->permissions = node->permissions;
    copy->owner       = node->owner;
    copy->group       = node->group;
    copy->createtime  = node->createtime;
    copy->readtime    = node->readtime;
    copy->writetime   = node->writetime;
    if (node->linkname) {
        copy->linkname = strdup(node->linkname);
        if (!copy->linkname) { plogk("extfs: linkname strdup failed for inode %llu\n", (unsigned long long)node->inode); }
    } else
        copy->linkname = 0;
    return copy;
}

/* Report the requested events as ready. */
static int extfs_poll(void *file, size_t events)
{
    (void)file;
    return (int)events;
}

/* Free a handle allocated by extfs_alloc_handle. */
static int extfs_free(void *handle)
{
    extfs_handle_t *h = handle;
    if (!h) return EOK;
    free(h);
    return EOK;
}

static struct vfs_callback extfs_callbacks = {
    .mount    = extfs_mount,
    .unmount  = extfs_unmount,
    .open     = extfs_open,
    .close    = extfs_close,
    .read     = extfs_read_file,
    .write    = extfs_write_file,
    .readlink = extfs_readlink_file,
    .mkdir    = extfs_mkdir_cb,
    .mkfile   = extfs_mkfile_cb,
    .link     = extfs_link_cb,
    .symlink  = extfs_symlink_cb,
    .stat     = extfs_stat,
    .ioctl    = extfs_ioctl_cb,
    .dup      = extfs_dup,
    .poll     = extfs_poll,
    .free     = extfs_free,
    .delete   = extfs_delete,
    .rename   = extfs_rename_cb,
    .resize   = extfs_resize,
    .sync     = extfs_sync,
};

/* Register the extfs filesystem and probe IDE disks for volumes. */
void extfs_regist(void)
{
#if CONFIG_EXTFS
    extfs_id = vfs_regist_fs("extfs", &extfs_callbacks);
    if (extfs_id & ERRNO_MASK) {
        plogk("extfs: Register error.\n");
        return;
    }
    plogk("extfs: Filesystem registered (fsid=%d)\n", extfs_id);

#    if CONFIG_ATA
    for (uint8_t drive = 0; drive < 4; drive++) {
        extfs_sb_info_t   sb;
        blockdev_device_t device;
        if (!ide_devices[drive].reserved || ide_devices[drive].type != IDE_ATA) continue;
        if (blockdev_open_drive(drive, &device) == EOK && extfs_read_super(&sb, &device) == EOK) {
            plogk("extfs: Detected ext2 on ide%u, volume '%.16s'\n", drive, sb.es->s_volume_name);
            extfs_free_super(&sb);
        }
    }
#    endif
#endif
}
