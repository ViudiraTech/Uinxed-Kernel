/*
 *
 *      extfs.c
 *      ext2/ext3/ext4 filesystem - VFS integration
 *
 *      Copyright (C) 2026 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/block/blockdev.h>
#include <drivers/block/ide.h>
#include <fs/core/vfs.h>
#include <fs/extfs/extfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>

static int extfs_id = 0;

static uint16_t extfs_mode_to_vfs(uint16_t mode)
{
    if (mode & EXT2_S_IFDIR) return file_dir;
    if (mode & EXT2_S_IFLNK) return file_symlink;
    if (mode & EXT2_S_IFBLK) return file_block;
    if (mode & EXT2_S_IFIFO) return file_pipe;
    return file_none;
}

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
    if (raw.i_dir_acl) node->size = node->realsize = ((uint64_t)raw.i_dir_acl << 32) | raw.i_size;
}

static extfs_handle_t *extfs_get_handle(vfs_node_t node)
{
    if (!node || !node->handle) return 0;
    return (extfs_handle_t *)node->handle;
}

static int extfs_mount(const char *src, vfs_node_t node)
{
    extfs_sb_info_t *sb;
    extfs_handle_t  *h;
    int              status;

    if (!src || !node) return -EINVAL;

    sb = calloc(1, sizeof(extfs_sb_info_t));
    if (!sb) return -ENOMEM;

    status = extfs_read_super(sb, 0);
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
    node->visited = 0;

    plogk("extfs: Mounted ext2 volume '%s', block size %u, %u groups\n", sb->es->s_volume_name, sb->block_size, sb->groups_count);

    return EOK;
}

static void extfs_unmount(void *root)
{
    vfs_node_t      node = root;
    extfs_handle_t *h;

    if (!node) return;

    h = extfs_get_handle(node);
    if (!h) return;

    if (h->owns_sb && h->sb) {
        extfs_write_super(h->sb);
        extfs_free_super(h->sb);
        free(h->sb);
    }
    free(h);
    node->handle = 0;
}

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
}

static void extfs_close(void *current)
{
    (void)current;
}

static size_t extfs_read_file(void *file, void *addr, size_t offset, size_t size)
{
    extfs_handle_t *h = file;
    if (!h) return 0;
    int r = extfs_read_data(h, addr, offset, size);
    return r > 0 ? (size_t)r : 0;
}

static size_t extfs_write_file(void *file, const void *addr, size_t offset, size_t size)
{
    extfs_handle_t *h = file;
    if (!h) return 0;
    return extfs_write_data(h, addr, offset, size);
}

static int extfs_resize(void *file, uint64_t size)
{
    extfs_handle_t *h = file;
    if (!h) return -EINVAL;
    return extfs_truncate(h, size);
}

static int extfs_sync(void *file, int data_only)
{
    extfs_handle_t *h = file;
    (void)data_only;
    if (!h) return -EINVAL;
    return extfs_flush_inode(h);
}

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
    if (symlink_len == 0) symlink_len = 60;
    if (symlink_len > 60) symlink_len = 60;

    if (offset >= symlink_len) return 0;
    if (offset + size > symlink_len) size = symlink_len - offset;

    memcpy(addr, (uint8_t *)raw.i_block + offset, size);
    return size;
}

static int extfs_mkdir_cb(void *parent, const char *name, vfs_node_t node)
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
    if (extfs_dir_lookup(dir_h, name, &dummy) == EOK) return -EEXIST;

    status = extfs_alloc_inode(sb, &new_ino);
    if (status != EOK) return status;

    memset(&new_raw, 0, sizeof(new_raw));
    new_raw.i_mode        = EXT2_S_IFDIR | 0755;
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

    ext2_inode_t parent_raw;
    if (extfs_read_inode_raw(sb, dir_h->inode_no, &parent_raw) == EOK) {
        parent_raw.i_links_count++;
        extfs_write_inode_raw(sb, dir_h->inode_no, &parent_raw);
    }

    node->handle = new_h;
    extfs_fill_node(node, new_h);

    return EOK;
}

static int extfs_mkfile_cb(void *parent, const char *name, vfs_node_t node)
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
    if (extfs_dir_lookup(dir_h, name, &dummy) == EOK) return -EEXIST;

    status = extfs_alloc_inode(sb, &new_ino);
    if (status != EOK) return status;

    memset(&new_raw, 0, sizeof(new_raw));
    new_raw.i_mode        = EXT2_S_IFREG | 0644;
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

    new_h = extfs_alloc_handle(sb, new_ino);
    if (!new_h) return -EIO;

    node->handle = new_h;
    extfs_fill_node(node, new_h);

    return EOK;
}

static int extfs_link_cb(void *parent, const char *name, vfs_node_t node)
{
    extfs_handle_t  *dir_h;
    extfs_handle_t  *target_h;
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;
    int              status;
    uint8_t          file_type;

    if (!parent || !name || !node) return -EINVAL;

    dir_h    = extfs_get_handle(node->parent);
    target_h = extfs_get_handle(node);
    if (!dir_h || !target_h) return -EINVAL;

    sb = dir_h->sb;

    uint32_t dummy;
    if (extfs_dir_lookup(dir_h, name, &dummy) == EOK) return -EEXIST;

    if (extfs_read_inode_raw(sb, target_h->inode_no, &raw) != EOK) return -EIO;

    if (raw.i_mode & EXT2_S_IFDIR)
        file_type = EXT2_FT_DIR;
    else if (raw.i_mode & EXT2_S_IFLNK)
        file_type = EXT2_FT_SYMLINK;
    else
        file_type = EXT2_FT_REG_FILE;

    status = extfs_dir_add_entry(dir_h, name, target_h->inode_no, file_type);
    if (status != EOK) return status;

    raw.i_links_count++;
    extfs_write_inode_raw(sb, target_h->inode_no, &raw);

    return EOK;
}

static int extfs_symlink_cb(void *parent, const char *name, vfs_node_t node)
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
    if (extfs_dir_lookup(dir_h, name, &dummy) == EOK) return -EEXIST;

    status = extfs_alloc_inode(sb, &new_ino);
    if (status != EOK) return status;

    memset(&new_raw, 0, sizeof(new_raw));
    new_raw.i_mode        = EXT2_S_IFLNK | 0777;
    new_raw.i_links_count = 1;

    uint32_t target_len = strlen(node->linkname ? node->linkname : "");
    if (target_len > 60) target_len = 60;
    new_raw.i_size = target_len;
    memcpy(new_raw.i_block, node->linkname ? node->linkname : "", target_len);

    status = extfs_write_inode_raw(sb, new_ino, &new_raw);
    if (status != EOK) {
        extfs_free_inode(sb, new_ino);
        return status;
    }

    status = extfs_dir_add_entry(dir_h, name, new_ino, EXT2_FT_SYMLINK);
    if (status != EOK) {
        extfs_free_inode(sb, new_ino);
        return status;
    }

    new_h = extfs_alloc_handle(sb, new_ino);
    if (!new_h) return -EIO;

    node->handle = new_h;
    node->type   = file_symlink;
    extfs_fill_node(node, new_h);

    return EOK;
}

static int extfs_delete(void *parent, vfs_node_t node)
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

    status = extfs_dir_remove_entry(dir_h, node->name);
    if (status != EOK) return status;

    raw.i_links_count--;
    if (raw.i_links_count == 0) {
        extfs_free_inode_blocks(file_h);
        extfs_write_inode_raw(sb, file_h->inode_no, &raw);
        extfs_free_inode(sb, file_h->inode_no);
    } else {
        extfs_write_inode_raw(sb, file_h->inode_no, &raw);
    }

    return EOK;
}

static int extfs_rmdir(void *parent, const char *name)
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

    if (!(raw.i_mode & EXT2_S_IFDIR)) return -ENOTDIR;

    child_h = extfs_alloc_handle(sb, child_ino);
    if (!child_h) return -EIO;

    if (!extfs_dir_empty(child_h)) {
        free(child_h);
        return -ENOTEMPTY;
    }

    status = extfs_dir_remove_entry(dir_h, name);
    if (status != EOK) {
        free(child_h);
        return status;
    }

    raw.i_links_count--;
    if (raw.i_links_count == 0) {
        extfs_write_inode_raw(sb, child_ino, &raw);
        extfs_free_inode(sb, child_ino);
    } else {
        extfs_write_inode_raw(sb, child_ino, &raw);
    }

    /* Update parent link count */
    ext2_inode_t parent_raw;
    if (extfs_read_inode_raw(sb, dir_h->inode_no, &parent_raw) == EOK) {
        if (parent_raw.i_links_count > 2) parent_raw.i_links_count--;
        extfs_write_inode_raw(sb, dir_h->inode_no, &parent_raw);
    }

    free(child_h);
    return EOK;
}
static int extfs_rename_cb(void *current, const char *new_name)
{
    vfs_node_t       node = current;
    extfs_handle_t  *old_h;
    extfs_handle_t  *parent_h;
    extfs_sb_info_t *sb;
    ext2_inode_t     raw;
    uint8_t          file_type;
    int              status;

    if (!node || !new_name) return -EINVAL;

    old_h = extfs_get_handle(node);
    if (!old_h) return -EINVAL;

    parent_h = extfs_get_handle(node->parent);
    if (!parent_h) return -EINVAL;

    sb = old_h->sb;

    /* Remove from old parent */
    status = extfs_dir_remove_entry(parent_h, node->name);
    if (status != EOK) return status;

    /* Determine file type */
    if (extfs_read_inode_raw(sb, old_h->inode_no, &raw) != EOK) return -EIO;

    if (raw.i_mode & EXT2_S_IFDIR)
        file_type = EXT2_FT_DIR;
    else if (raw.i_mode & EXT2_S_IFLNK)
        file_type = EXT2_FT_SYMLINK;
    else
        file_type = EXT2_FT_REG_FILE;

    /* Add to same parent with new name */
    status = extfs_dir_add_entry(parent_h, new_name, old_h->inode_no, file_type);
    if (status != EOK) return status;

    return EOK;
}

static int extfs_stat(void *file, vfs_node_t node)
{
    extfs_handle_t *h = file;
    if (!h || !node) return -EINVAL;
    extfs_fill_node(node, h);
    return EOK;
}

static int extfs_ioctl_cb(void *file, size_t req, void *arg)
{
    (void)file;
    (void)arg;
    if (req == 0) return -ENOSYS;
    return -ENOSYS;
}

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
    copy->linkname    = node->linkname ? strdup(node->linkname) : 0;
    return copy;
}

static int extfs_poll(void *file, size_t events)
{
    (void)file;
    return (int)events;
}

static int extfs_free(void *handle)
{
    extfs_handle_t *h = handle;
    if (!h) return EOK;
    free(h);
    return EOK;
}

static int extfs_no_link(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -ENOSYS;
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

void extfs_regist(void)
{
    extfs_id = vfs_regist_fs("extfs", &extfs_callbacks);
    if (extfs_id & ERRNO_MASK) {
        plogk("extfs: Register error.\n");
        return;
    }
    plogk("extfs: Filesystem registered (fsid=%d)\n", extfs_id);

    for (uint8_t drive = 0; drive < 4; drive++) {
        extfs_sb_info_t sb;
        if (!ide_devices[drive].reserved || ide_devices[drive].type != IDE_ATA) continue;
        if (extfs_read_super(&sb, drive) == EOK) {
            plogk("extfs: Detected ext2 on ide%u, volume '%s'\n", drive, sb.es->s_volume_name);
            extfs_free_super(&sb);
        }
    }
}