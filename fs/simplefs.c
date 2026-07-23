/*
 *
 *      simplefs.c
 *      Minimal IDE-backed filesystem using superblock metadata
 *
 *      2026/5/18 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/blockdev.h>
#include <drivers/ide.h>
#include <fs/simplefs.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>

static int simplefs_id = 0;

typedef struct simplefs_vnode {
        simplefs_handle_t    *fs;
        simplefs_inode_disk_t inode;
        uint32_t              inode_no;
        int                   owns_fs;
} simplefs_vnode_t;

static uint16_t simplefs_type_to_vfs(uint16_t type)
{
    switch (type) {
        case simplefs_inode_dir :
            return file_dir;
        case simplefs_inode_symlink :
            return file_symlink;
        case simplefs_inode_file :
        default :
            return file_none;
    }
}

static int simplefs_parse_drive(const char *src, uint8_t *drive)
{
    if (!src || !drive) return -EINVAL;

    return blockdev_parse_drive(src, drive);
}

static uint16_t simplefs_vfs_to_type(uint16_t type)
{
    if (type & file_dir) return simplefs_inode_dir;
    if (type & file_symlink) return simplefs_inode_symlink;
    return simplefs_inode_file;
}

static uint64_t simplefs_block_offset(const simplefs_handle_t *fs, uint32_t block)
{
    return (uint64_t)block * fs->disk.block_size;
}

static int simplefs_disk_read_bytes(const simplefs_handle_t *fs, uint64_t offset, void *buffer, size_t size)

{
    return blockdev_read_bytes(&fs->device, offset, buffer, size);
}

static int simplefs_disk_write_bytes(const simplefs_handle_t *fs, uint64_t offset, const void *buffer, size_t size)
{
    return blockdev_write_bytes(&fs->device, offset, buffer, size);
}

static int simplefs_read_inode(const simplefs_handle_t *fs, uint32_t inode_no, simplefs_inode_disk_t *inode)
{
    uint64_t offset;

    if (!fs || !inode) return -EINVAL;
    if (!inode_no || inode_no > fs->disk.inode_count) return -ENOENT;

    offset = simplefs_block_offset(fs, fs->disk.inode_table_start) + (uint64_t)(inode_no - 1) * fs->disk.inode_size;
    return simplefs_disk_read_bytes(fs, offset, inode, sizeof(*inode));
}

static int simplefs_write_inode(const simplefs_handle_t *fs, uint32_t inode_no, const simplefs_inode_disk_t *inode)
{
    uint64_t offset;

    if (!fs || !inode) return -EINVAL;
    if (!inode_no || inode_no > fs->disk.inode_count) return -ENOENT;

    offset = simplefs_block_offset(fs, fs->disk.inode_table_start) + (uint64_t)(inode_no - 1) * fs->disk.inode_size;
    return simplefs_disk_write_bytes(fs, offset, inode, sizeof(*inode));
}

static simplefs_vnode_t *simplefs_vnode_alloc(simplefs_handle_t *fs, uint32_t inode_no)
{
    simplefs_vnode_t *vnode;

    vnode = calloc(1, sizeof(simplefs_vnode_t));
    if (!vnode) return 0;

    vnode->fs       = fs;
    vnode->inode_no = inode_no;
    if (simplefs_read_inode(fs, inode_no, &vnode->inode) != EOK) {
        free(vnode);
        return 0;
    }

    return vnode;
}

static void simplefs_fill_node(vfs_node_t node, const simplefs_vnode_t *vnode)
{
    node->inode       = vnode->inode_no;
    node->type        = simplefs_type_to_vfs(vnode->inode.type);
    node->size        = vnode->inode.size;
    node->realsize    = vnode->inode.size;
    node->blksz       = vnode->fs->disk.block_size;
    node->permissions = vnode->inode.permissions;
    node->owner       = vnode->inode.owner;
    node->group       = vnode->inode.group;
    node->createtime  = vnode->inode.create_time;
    node->writetime   = vnode->inode.write_time;
    node->readtime    = vnode->inode.write_time;
}

static size_t simplefs_inode_read(simplefs_vnode_t *vnode, void *addr, size_t offset, size_t size)
{
    simplefs_handle_t *fs;
    uint8_t           *out  = addr;
    size_t             done = 0;
    uint8_t           *block_buf;

    if (!vnode || !addr || offset >= vnode->inode.size) return 0;

    fs        = vnode->fs;
    size      = MIN(size, (size_t)(vnode->inode.size - offset));
    block_buf = malloc(fs->disk.block_size);
    if (!block_buf) return 0;

    while (done < size) {
        size_t   absolute = offset + done;
        uint32_t block_id = (uint32_t)(absolute / fs->disk.block_size);
        size_t   inblock  = absolute % fs->disk.block_size;
        size_t   chunk    = MIN(size - done, fs->disk.block_size - inblock);
        uint32_t disk_block;

        if (block_id >= SIMPLEFS_INODE_DIRECT_COUNT) break;
        disk_block = vnode->inode.direct[block_id];
        if (!disk_block) break;

        if (simplefs_disk_read_bytes(fs, simplefs_block_offset(fs, disk_block), block_buf, fs->disk.block_size) != EOK) break;
        memcpy(out + done, block_buf + inblock, chunk);
        done += chunk;
    }

    free(block_buf);
    return done;
}

static int simplefs_sync_vnode(vfs_node_t node)
{
    simplefs_vnode_t *vnode = node ? node->handle : 0;

    if (!node || !vnode) return -EINVAL;

    vnode->inode.permissions = node->permissions;
    vnode->inode.owner       = node->owner;
    vnode->inode.group       = node->group;
    vnode->inode.size        = node->size;
    vnode->inode.type        = simplefs_vfs_to_type(node->type);
    return simplefs_write_inode(vnode->fs, vnode->inode_no, &vnode->inode);
}

static int simplefs_scan_next_inode(const simplefs_handle_t *fs, uint32_t *inode_no)
{
    simplefs_inode_disk_t inode;

    if (!fs || !inode_no) return -EINVAL;

    for (uint32_t i = 1; i <= fs->disk.inode_count; i++) {
        if (simplefs_read_inode(fs, i, &inode) != EOK) return -EIO;
        if (!inode.inode || inode.type == simplefs_inode_none) {
            *inode_no = i;
            return EOK;
        }
    }

    return -ENOSPC;
}

static int simplefs_scan_next_block(const simplefs_handle_t *fs, uint32_t *block_no)
{
    uint8_t *used;

    if (!fs || !block_no) return -EINVAL;
    if (fs->disk.block_count <= fs->disk.data_block_start) return -ENOSPC;

    used = calloc(fs->disk.block_count, 1);
    if (!used) return -ENOMEM;

    for (uint32_t i = 0; i < fs->disk.data_block_start; i++) used[i] = 1;

    for (uint32_t i = 1; i <= fs->disk.inode_count; i++) {
        simplefs_inode_disk_t inode;

        if (simplefs_read_inode(fs, i, &inode) != EOK) {
            free(used);
            return -EIO;
        }
        if (!inode.inode || inode.type == simplefs_inode_none) continue;

        for (size_t j = 0; j < SIMPLEFS_INODE_DIRECT_COUNT; j++) {
            if (inode.direct[j] && inode.direct[j] < fs->disk.block_count) used[inode.direct[j]] = 1;
        }
    }

    for (uint32_t i = fs->disk.data_block_start; i < fs->disk.block_count; i++) {
        if (!used[i]) {
            *block_no = i;
            free(used);
            return EOK;
        }
    }

    free(used);
    return -ENOSPC;
}

static int simplefs_ensure_block(simplefs_vnode_t *vnode, uint32_t block_id, uint32_t *disk_block)
{
    uint8_t *zero;
    uint32_t block_no;
    int      status;

    if (!vnode || !disk_block) return -EINVAL;
    if (block_id >= SIMPLEFS_INODE_DIRECT_COUNT) return -ENOSPC;
    if (vnode->inode.direct[block_id]) {
        *disk_block = vnode->inode.direct[block_id];
        return EOK;
    }

    status = simplefs_scan_next_block(vnode->fs, &block_no);
    if (status != EOK) return status;

    zero = calloc(1, vnode->fs->disk.block_size);
    if (!zero) return -ENOMEM;
    status = simplefs_disk_write_bytes(vnode->fs, simplefs_block_offset(vnode->fs, block_no), zero, vnode->fs->disk.block_size);
    free(zero);
    if (status != EOK) return status;

    vnode->inode.direct[block_id] = block_no;
    status                        = simplefs_write_inode(vnode->fs, vnode->inode_no, &vnode->inode);
    if (status != EOK) return status;

    *disk_block = block_no;
    return EOK;
}

static size_t simplefs_inode_write(simplefs_vnode_t *vnode, const void *addr, size_t offset, size_t size)
{
    const uint8_t *in = addr;
    uint8_t       *block_buf;
    size_t         done = 0;

    if (!vnode || !addr) return 0;

    block_buf = malloc(vnode->fs->disk.block_size);
    if (!block_buf) return 0;

    while (done < size) {
        size_t   absolute = offset + done;
        uint32_t block_id = (uint32_t)(absolute / vnode->fs->disk.block_size);
        size_t   inblock  = absolute % vnode->fs->disk.block_size;
        size_t   chunk    = MIN(size - done, vnode->fs->disk.block_size - inblock);
        uint32_t disk_block;

        if (simplefs_ensure_block(vnode, block_id, &disk_block) != EOK) break;
        if (simplefs_disk_read_bytes(vnode->fs, simplefs_block_offset(vnode->fs, disk_block), block_buf, vnode->fs->disk.block_size) != EOK)
            break;

        memcpy(block_buf + inblock, in + done, chunk);
        if (simplefs_disk_write_bytes(vnode->fs, simplefs_block_offset(vnode->fs, disk_block), block_buf, vnode->fs->disk.block_size) != EOK)
            break;
        done += chunk;
    }

    free(block_buf);
    return done;
}

static int simplefs_add_child(vfs_node_t parent, vfs_node_t child)
{
    simplefs_vnode_t      *dir = parent ? parent->handle : 0;
    simplefs_dirent_disk_t entry;
    size_t                 name_len;
    size_t                 written;

    if (!parent || !child || !dir) return -EINVAL;
    if (dir->inode.type != simplefs_inode_dir) return -ENOTDIR;

    memset(&entry, 0, sizeof(entry));
    name_len = strlen(child->name);
    if (!name_len || name_len > SIMPLEFS_DIRENT_NAME_LENGTH) return -ENAMETOOLONG;

    entry.inode       = child->inode;
    entry.type        = simplefs_vfs_to_type(child->type);
    entry.name_length = (uint16_t)name_len;
    memcpy(entry.name, child->name, name_len);

    written = simplefs_inode_write(dir, &entry, (size_t)dir->inode.size, sizeof(entry));
    if (written != sizeof(entry)) return -ENOSPC;

    dir->inode.size += sizeof(entry);
    parent->size     = dir->inode.size;
    parent->realsize = dir->inode.size;
    parent->visited  = 0;
    return simplefs_sync_vnode(parent);
}

static int simplefs_create_node(vfs_node_t parent, const char *name, vfs_node_t node, uint16_t type)
{
    simplefs_vnode_t     *handle;
    simplefs_inode_disk_t inode;
    uint32_t              inode_no;
    int                   status;

    if (!parent || !name || !node) return -EINVAL;

    handle = parent->handle;
    if (!handle || handle->inode.type != simplefs_inode_dir) return -ENOTDIR;

    status = simplefs_scan_next_inode(handle->fs, &inode_no);
    if (status != EOK) return status;

    memset(&inode, 0, sizeof(inode));
    inode.inode       = inode_no;
    inode.type        = type;
    inode.links       = 1;
    inode.permissions = type == simplefs_inode_dir ? 0755 : 0644;

    status = simplefs_write_inode(handle->fs, inode_no, &inode);
    if (status != EOK) return status;

    node->handle = simplefs_vnode_alloc(handle->fs, inode_no);
    if (!node->handle) return -EIO;

    node->inode       = inode_no;
    node->type        = simplefs_type_to_vfs(type);
    node->size        = 0;
    node->realsize    = 0;
    node->blksz       = handle->fs->disk.block_size;
    node->permissions = inode.permissions;
    node->owner       = 0;
    node->group       = 0;

    status = simplefs_add_child(parent, node);
    if (status != EOK) return status;

    simplefs_fill_node(node, node->handle);
    if (node->type & file_dir) node->visited = 1;
    return EOK;
}

static int simplefs_mkdir(void *parent, const char *name, vfs_node_t node)
{
    simplefs_vnode_t *dir = parent;

    if (!dir || !dir->fs || !node || !node->parent) return -EINVAL;
    return simplefs_create_node(node->parent, name, node, simplefs_inode_dir);
}

static int simplefs_mkfile(void *parent, const char *name, vfs_node_t node)
{
    simplefs_vnode_t *dir = parent;

    if (!dir || !dir->fs || !node || !node->parent) return -EINVAL;
    return simplefs_create_node(node->parent, name, node, simplefs_inode_file);
}

static int simplefs_no_link(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -ENOSYS;
}

static int simplefs_load_directory(vfs_node_t node)
{
    simplefs_vnode_t       *vnode = node ? node->handle : 0;
    simplefs_dirent_disk_t *entries;
    size_t                  entry_count;

    if (!node || !vnode) return -EINVAL;
    if (node->visited) return EOK;
    if (vnode->inode.type != simplefs_inode_dir) return EOK;
    if (!vnode->inode.size) {
        node->visited = 1;
        return EOK;
    }

    entries = malloc((size_t)vnode->inode.size);
    if (!entries) return -ENOMEM;

    if (simplefs_inode_read(vnode, entries, 0, (size_t)vnode->inode.size) != vnode->inode.size) {
        free(entries);
        return -EIO;
    }

    entry_count = (size_t)vnode->inode.size / sizeof(simplefs_dirent_disk_t);
    for (size_t i = 0; i < entry_count; i++) {
        simplefs_dirent_disk_t *entry = &entries[i];
        simplefs_vnode_t       *child_handle;
        vfs_node_t              child;
        char                    name[SIMPLEFS_DIRENT_NAME_LENGTH + 1];

        if (!entry->inode || !entry->name_length || entry->name_length > SIMPLEFS_DIRENT_NAME_LENGTH) continue;
        if (entry->name[0] == '.' && entry->name_length == 1) continue;
        if (entry->name[0] == '.' && entry->name[1] == '.' && entry->name_length == 2) continue;

        memcpy(name, entry->name, entry->name_length);
        name[entry->name_length] = '\0';
        child                    = vfs_do_search(node, name);
        if (child) continue;

        child_handle = simplefs_vnode_alloc(vnode->fs, entry->inode);
        if (!child_handle) continue;

        child         = vfs_node_alloc(node, name);
        child->handle = child_handle;
        simplefs_fill_node(child, child_handle);
    }

    node->visited = 1;
    free(entries);
    return EOK;
}

static int simplefs_lookup(vfs_node_t parent, const char *name, vfs_node_t node)
{
    simplefs_vnode_t       *dir = parent ? parent->handle : 0;
    simplefs_dirent_disk_t *entries;
    size_t                  entry_count;
    size_t                  name_len;

    if (!parent || !node || !name || !dir) return -EINVAL;
    if (simplefs_load_directory(parent) != EOK) return -EIO;

    node = vfs_do_search(parent, name);
    if (node && node->handle) return EOK;
    if (!dir->inode.size) return -ENOENT;

    entries = malloc((size_t)dir->inode.size);
    if (!entries) return -ENOMEM;

    if (simplefs_inode_read(dir, entries, 0, (size_t)dir->inode.size) != dir->inode.size) {
        free(entries);
        return -EIO;
    }

    name_len    = strlen(name);
    entry_count = (size_t)dir->inode.size / sizeof(simplefs_dirent_disk_t);
    for (size_t i = 0; i < entry_count; i++) {
        simplefs_dirent_disk_t *entry = &entries[i];

        if (!entry->inode || entry->name_length != name_len || entry->name_length > SIMPLEFS_DIRENT_NAME_LENGTH) continue;
        if (memcmp(entry->name, name, name_len) != 0) continue;

        node->handle = simplefs_vnode_alloc(dir->fs, entry->inode);
        if (!node->handle) {
            free(entries);
            return -ENOENT;
        }

        simplefs_fill_node(node, node->handle);
        free(entries);
        return EOK;
    }

    free(entries);
    return -ENOENT;
}

int simplefs_probe(uint8_t drive)
{
    return superblock_probe(drive);
}

static int simplefs_mount(const char *src, vfs_node_t node)
{
    uint8_t            drive;
    simplefs_vnode_t  *root;
    simplefs_handle_t *handle;
    int                status;

    if (simplefs_parse_drive(src, &drive) != EOK) return -EINVAL;

    handle = calloc(1, sizeof(simplefs_handle_t));
    if (!handle) return -ENOMEM;

    status = blockdev_open_drive(drive, &handle->device);
    if (status != EOK) {
        free(handle);
        return status;
    }

    if (superblock_read(drive, &handle->disk) != EOK) {
        free(handle);
        return -EINVAL;
    }

    root = simplefs_vnode_alloc(handle, handle->disk.root_inode);
    if (!root) {
        free(handle);
        return -EIO;
    }

    root->owns_fs = 1;
    node->handle  = root;
    simplefs_fill_node(node, root);
    if (simplefs_load_directory(node) != EOK) {
        free(root);
        free(handle);
        node->handle = 0;
        return -EIO;
    }

    plogk("simplefs: Mounted drive ide%u, volume '%s', block size %u\n", handle->device.drive, handle->disk.volume_name,
          handle->disk.block_size);
    return EOK;
}

static void simplefs_unmount(void *root)
{
    (void)root;
}

static void simplefs_open(void *parent, const char *name, vfs_node_t node)
{
    if (!parent || !node) return;

    if (simplefs_lookup(node->parent, name, node) != EOK) return;
    if (node->type & file_dir) simplefs_load_directory(node);
}

static void simplefs_close(void *current)
{
    (void)current;
}

static size_t simplefs_read_file(void *file, void *addr, size_t offset, size_t size)
{
    simplefs_vnode_t *vnode = file;

    if (!vnode || vnode->inode.type == simplefs_inode_dir) return 0;
    return simplefs_inode_read(vnode, addr, offset, size);
}

static size_t simplefs_write_file(void *file, const void *addr, size_t offset, size_t size)
{
    simplefs_vnode_t *vnode = file;
    size_t            written;

    if (!vnode || !addr || vnode->inode.type == simplefs_inode_dir) return 0;
    written = simplefs_inode_write(vnode, addr, offset, size);
    if (offset + written > vnode->inode.size) vnode->inode.size = offset + written;
    simplefs_write_inode(vnode->fs, vnode->inode_no, &vnode->inode);
    return written;
}

static size_t simplefs_readlink(vfs_node_t node, void *addr, size_t offset, size_t size)
{
    if (!node || !node->handle) return 0;
    return simplefs_inode_read(node->handle, addr, offset, size);
}

static int simplefs_stat(void *file, vfs_node_t node)
{
    simplefs_vnode_t *handle = file;

    if (!handle || !node) return -EINVAL;

    simplefs_fill_node(node, handle);
    if (node->type & file_dir) return simplefs_load_directory(node);
    return EOK;
}

static int simplefs_ioctl(void *file, size_t req, void *arg)
{
    simplefs_vnode_t *handle = file;

    if (!handle) return -EINVAL;
    if (req != 0 || !arg) return -ENOSYS;

    memcpy(arg, &handle->fs->disk, sizeof(handle->fs->disk));
    return EOK;
}

static vfs_node_t simplefs_dup(vfs_node_t node)
{
    vfs_node_t        copy;
    simplefs_vnode_t *copy_handle;
    simplefs_vnode_t *src_handle;

    if (!node) return 0;
    src_handle = node->handle;

    copy = vfs_node_alloc(node->parent, node->name);
    if (!copy) return 0;

    copy_handle = calloc(1, sizeof(simplefs_vnode_t));
    if (!copy_handle) {
        free(copy);
        return 0;
    }

    if (src_handle) {
        memcpy(copy_handle, src_handle, sizeof(*copy_handle));
        copy_handle->owns_fs = 0;
    }

    copy->handle      = copy_handle;
    copy->type        = node->type;
    copy->size        = node->size;
    copy->inode       = node->inode;
    copy->blksz       = node->blksz;
    copy->linkname    = node->linkname ? strdup(node->linkname) : 0;
    copy->flags       = node->flags;
    copy->permissions = node->permissions;
    copy->owner       = node->owner;
    copy->group       = node->group;
    copy->realsize    = node->realsize;
    copy->createtime  = node->createtime;
    copy->writetime   = node->writetime;
    copy->readtime    = node->readtime;
    return copy;
}

static int simplefs_poll(void *file, size_t events)
{
    (void)file;
    return (int)events;
}

static int simplefs_free(void *handle)
{
    simplefs_vnode_t *vnode = handle;

    if (!vnode) return EOK;
    if (vnode->owns_fs) free(vnode->fs);
    free(vnode);
    return EOK;
}

static int simplefs_delete(void *parent, vfs_node_t node)
{
    (void)parent;
    (void)node;
    return -EROFS;
}

static int simplefs_rename(void *current, const char *new_name)
{
    (void)current;
    (void)new_name;
    return -EROFS;
}

static struct vfs_callback simplefs_callbacks = {
    .mount    = simplefs_mount,
    .unmount  = simplefs_unmount,
    .open     = simplefs_open,
    .close    = simplefs_close,
    .read     = simplefs_read_file,
    .write    = simplefs_write_file,
    .readlink = simplefs_readlink,
    .mkdir    = simplefs_mkdir,
    .mkfile   = simplefs_mkfile,
    .link     = simplefs_no_link,
    .symlink  = simplefs_no_link,
    .stat     = simplefs_stat,
    .ioctl    = simplefs_ioctl,
    .dup      = simplefs_dup,
    .poll     = simplefs_poll,
    .free     = simplefs_free,
    .delete   = simplefs_delete,
    .rename   = simplefs_rename,
};

void simplefs_regist(void)
{
    simplefs_id = vfs_regist_fs("simplefs", &simplefs_callbacks);
    if (simplefs_id & ERRNO_MASK) {
        plogk("simplefs: Register error.\n");
        return;
    }

    for (uint8_t drive = 0; drive < 4; drive++) {
        if (!ide_devices[drive].reserved || ide_devices[drive].type != IDE_ATA) continue;
        if (simplefs_probe(drive) == EOK) plogk("simplefs: Detected valid superblock on ide%u\n", drive);
    }
}

void simplefs_mount_all(void)
{
    vfs_node_t root = get_rootdir();

    if (!root || !root->fsid) return;
    if (vfs_mkdir("/disk") != EOK) return;

    for (uint8_t drive = 0; drive < 4; drive++) {
        char       path[] = "/disk/ide0";
        const char src[]  = {'i', 'd', 'e', (char)('0' + drive), '\0'};
        vfs_node_t node;

        if (!ide_devices[drive].reserved || ide_devices[drive].type != IDE_ATA) continue;
        if (simplefs_probe(drive) != EOK) continue;

        path[9] = (char)('0' + drive);
        vfs_mkdir(path);
        node = vfs_open(path);
        if (!node) continue;
        if (node->is_mount) {
            vfs_close(node);
            continue;
        }

        if (vfs_mount(src, node) == EOK)
            plogk("simplefs: Auto-mounted ide%u at %s\n", drive, path);
        else
            plogk("simplefs: Failed to auto-mount ide%u at %s\n", drive, path);
        vfs_close(node);
    }
}
