/*
 * 
 *      tmpfs.c
 *      Temporary file system
 *
 *      2025/11/3 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/virtual/tmpfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <proc/uaccess.h>

int        tmpfs_id    = 0;
static int devtmpfs_id = 0;

/* Mount the tmpfs file system to a specified VFS node */
int tmpfs_mount(const char *handle, vfs_node_t node)
{
    /*
     * tmpfs has no block-device source.  mount(8) conventionally supplies
     * "tmpfs" as the source operand, which is intentionally ignored.
     */
    (void)handle;
    if (!node) return -EINVAL;

    tmpfs_file_t *tmpfs_root = calloc(1, sizeof(*tmpfs_root));
    if (!tmpfs_root) return -ENOMEM;
    tmpfs_root->type       = tp_file_dir;
    tmpfs_root->node_type  = file_dir;
    tmpfs_root->node       = node;
    tmpfs_root->root       = node;
    tmpfs_root->link_count = 1;

    strcpy(tmpfs_root->name, "tmp");
    node->handle = tmpfs_root;
    return EOK;
}

/* Unmount the tmpfs file system and free related resources */
void tmpfs_umount(void *root)
{
    tmpfs_file_t *tmpfs_root = root;

    if (!tmpfs_root) return;
    free(tmpfs_root);
}

/* Common function to create a tmpfs file or directory (internal use) */
int tmpfs_mk(void *parent, const char *name, vfs_node_t node, int is_dir)
{
    (void)parent;
    tmpfs_file_t *f = calloc(1, sizeof(tmpfs_file_t));
    if (!f) return -ENOMEM;

    strncpy(f->name, name, sizeof(f->name) - 1);
    f->name[sizeof(f->name) - 1] = '\0';

    f->type       = is_dir ? tp_file_dir : tp_file_file;
    f->node_type  = is_dir ? file_dir : file_none;
    f->link_count = 1;
    node->handle  = f;
    f->node       = node;

    return EOK;
}

/* Create a directory in tmpfs */
int tmpfs_mkdir(void *parent, const char *name, vfs_node_t node)
{
    return tmpfs_mk(parent, name, node, 1);
}

/* Create a regular file in tmpfs */
int tmpfs_mkfile(void *parent, const char *name, vfs_node_t node)
{
    return tmpfs_mk(parent, name, node, 0);
}

int tmpfs_link(void *parent, const char *target_name, vfs_node_t node)
{
    (void)parent;
    if (!target_name || !node) return -EINVAL;

    vfs_node_t target = vfs_open_nofollow(target_name);
    if (!target) return -ENOENT;
    if (target->fsid != tmpfs_id || !target->handle) {
        vfs_close(target);
        return -EXDEV;
    }
    if (target->type & file_dir) {
        vfs_close(target);
        return -EPERM;
    }

    tmpfs_file_t *inode = target->handle;
    spin_lock(&inode->link_lock);
    if (inode->link_count == UINT32_MAX) {
        spin_unlock(&inode->link_lock);
        vfs_close(target);
        return -EMLINK;
    }
    inode->link_count++;
    uint32_t links = inode->link_count;
    spin_unlock(&inode->link_lock);

    node->handle      = inode;
    node->type        = target->type & ~file_delete;
    node->size        = target->size;
    node->inode       = target->inode;
    node->nlink       = links;
    node->blksz       = target->blksz;
    node->owner       = target->owner;
    node->group       = target->group;
    node->mode        = target->mode;
    node->permissions = target->permissions;
    node->dev         = target->dev;
    node->rdev        = target->rdev;
    node->flags |= target->flags & (MOUNT_FLAG_NOSUID | MOUNT_FLAG_NODEV | MOUNT_FLAG_NOEXEC | MOUNT_FLAG_RDONLY);
    target->nlink = links;
    vfs_close(target);
    return EOK;
}

/* Read data from a tmpfs regular file */
size_t tmpfs_read(void *file, void *addr, size_t offset, size_t size)
{
    tmpfs_file_t *f = (tmpfs_file_t *)file;
    if (!f || (!addr && size)) return 0;
    if (f->device.read) return f->device.read(f->device.ctx, addr, offset, size);

    spin_lock(&f->data_lock);
    if (offset >= f->size) {
        spin_unlock(&f->data_lock);
        return 0;
    }
    size_t actual = (offset + size > f->size) ? (f->size - offset) : size;
    memcpy(addr, f->data + offset, actual);
    spin_unlock(&f->data_lock);
    return actual;
}

static int tmpfs_make_private_locked(tmpfs_file_t *f, size_t minimum_capacity)
{
    size_t capacity = f->capacity;
    char  *data;

    if (minimum_capacity <= capacity && !f->data_external) return EOK;
    if (capacity < minimum_capacity) {
        capacity = minimum_capacity;
        if (capacity <= SIZE_MAX / 2) capacity *= 2;
    }
    if (!capacity) capacity = 1;

    if (!f->data_external) {
        data = realloc(f->data, capacity);
        if (!data) return -ENOMEM;
    } else {
        data = malloc(capacity);
        if (!data) return -ENOMEM;
        if (f->size) memcpy(data, f->data, f->size);
    }
    f->data          = data;
    f->capacity      = capacity;
    f->data_external = false;
    return EOK;
}

/* Write data to a tmpfs regular file */
static size_t tmpfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    tmpfs_file_t *f = (tmpfs_file_t *)file;
    size_t        old_size;

    if (!f || (!addr && size)) return 0;
    if (f->device.write) return f->device.write(f->device.ctx, addr, offset, size);
    if (!size) return 0;

    if (offset > SIZE_MAX - size) return 0;
    size_t end = offset + size;
    spin_lock(&f->data_lock);
    old_size = f->size;

    if (end > f->capacity || f->data_external) {
        if (tmpfs_make_private_locked(f, end) != EOK) {
            plogk("tmpfs: grow to %lu bytes failed (out of memory)\n", (unsigned long)end);
            spin_unlock(&f->data_lock);
            return 0;
        }
    }

    if (offset > old_size) memset(f->data + old_size, 0, offset - old_size);

    memcpy(f->data + offset, addr, size);
    if (end > f->size) f->size = end;

    spin_unlock(&f->data_lock);
    return size;
}

int tmpfs_resize(void *file, uint64_t size)
{
    tmpfs_file_t *f = file;
    if (!f || f->type != tp_file_file || f->device.read || f->device.write) return -EINVAL;
    if (size > SIZE_MAX) return -EFBIG;
    size_t requested = (size_t)size;
    spin_lock(&f->data_lock);
    if (requested > f->capacity || (f->data_external && requested != f->size)) {
        int status = tmpfs_make_private_locked(f, requested);
        if (status != EOK) {
            spin_unlock(&f->data_lock);
            return status;
        }
    }
    if (requested > f->size) memset(f->data + f->size, 0, requested - f->size);
    f->size = requested;
    spin_unlock(&f->data_lock);
    return EOK;
}

int tmpfs_adopt_file_data(vfs_node_t node, const void *data, size_t size)
{
    tmpfs_file_t *f;

    if (!node || node->fsid != tmpfs_id || (!data && size)) return -EINVAL;
    f = node->handle;
    if (!f || f->type != tp_file_file || f->device.read || f->device.write || f->device.file_read || f->device.file_write) return -EINVAL;

    spin_lock(&f->data_lock);
    if (f->data && !f->data_external) free(f->data);
    f->data          = (char *)data;
    f->size          = size;
    f->capacity      = size;
    f->data_external = true;
    node->size       = size;
    spin_unlock(&f->data_lock);
    return EOK;
}

/* Get file status (type, size) of a tmpfs file/directory */
int tmpfs_stat(void *file, vfs_node_t node)
{
    tmpfs_file_t *file0 = (tmpfs_file_t *)file;
    if (!file0) return -ENOENT;

    node->type = file0->node_type;
    node->size = file0->type == tp_file_dir ? 0 : file0->size;
    spin_lock(&file0->link_lock);
    node->nlink = file0->link_count;
    spin_unlock(&file0->link_lock);
    return EOK;
}

/* Delete a tmpfs file/directory and free its resources */
int tmpfs_delete(void *parent, vfs_node_t node)
{
    (void)parent;
    (void)node;
    return EOK;
}

/* Rename a tmpfs file/directory */
int tmpfs_rename(void *current, const char *new_name)
{
    tmpfs_file_t *f = (tmpfs_file_t *)current;
    strncpy(f->name, new_name, sizeof(f->name) - 1);
    f->name[sizeof(f->name) - 1] = '\0';
    return EOK;
}

/* Poll a tmpfs file for pending events (simplified implementation) */
int tmpfs_poll(void *file, size_t events)
{
    tmpfs_file_t *handle = file;

    if (handle->device.poll) return handle->device.poll(handle->device.ctx, events);
    int revents = 0;
    if (events & 0x0001) revents |= 0x0001;
    if (events & 0x0004) revents |= 0x0004;
    return revents;
}

/* Send control commands to a device or file */
int tmpfs_ioctl(void *file, size_t req, void *arg)
{
    tmpfs_file_t *handle = file;

    if (handle->device.ioctl) return handle->device.ioctl(handle->device.ctx, req, arg);
    return -ENOTTY;
}

/* Duplicate a VFS node bound to tmpfs */
vfs_node_t tmpfs_dup(vfs_node_t node)
{
    vfs_node_t copy     = vfs_node_alloc(node->parent, node->name);
    copy->handle        = node->handle;
    copy->type          = node->type;
    copy->size          = node->size;
    copy->inode         = node->inode;
    copy->nlink         = node->nlink;
    copy->dev           = node->dev;
    copy->rdev          = node->rdev;
    copy->fsid          = node->fsid;
    copy->linkname      = node->linkname == 0 ? 0 : strdup(node->linkname);
    copy->flags         = node->flags;
    copy->permissions   = node->permissions;
    copy->mode          = node->mode;
    copy->owner         = node->owner;
    copy->group         = node->group;
    copy->child         = NULL;
    copy->realsize      = node->realsize;
    tmpfs_file_t *inode = node->handle;
    if (inode) {
        spin_lock(&inode->link_lock);
        if (inode->link_count != UINT32_MAX) inode->link_count++;
        copy->nlink = inode->link_count;
        spin_unlock(&inode->link_lock);
    }
    return copy;
}

/* Create a symbolic link in tmpfs */
int tmpfs_symlink(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    tmpfs_file_t *f = calloc(1, sizeof(tmpfs_file_t));

    if (!f) return -ENOMEM;

    strncpy(f->name, node->name, sizeof(f->name) - 1);
    f->name[sizeof(f->name) - 1] = '\0';
    f->type                      = tp_file_symlink;
    f->node_type                 = file_symlink;
    f->link_count                = 1;
    node->handle                 = f;
    f->node                      = node;

    return EOK;
}

/* Free resources of a tmpfs file/directory */
int tmpfs_free(void *handle)
{
    tmpfs_file_t *file = handle;

    if (!file) return EOK;

    spin_lock(&file->link_lock);
    if (file->link_count > 1) {
        file->link_count--;
        spin_unlock(&file->link_lock);
        return EOK;
    }
    file->link_count = 0;
    spin_unlock(&file->link_lock);

    if (file->device.destroy) file->device.destroy(file->device.ctx);

    if (file->type != tp_file_file) {
        free(file);
        return EOK;
    }
    if (file->data && !file->data_external) free(file->data);

    free(file);
    return EOK;
}

/* Dummy function (placeholder for VFS callbacks not implemented) */
void tmpfs_dummy(void)
{ // Nothing
}

/* ------------------------------------------------------------------ */
/* Per-open-instance callbacks --delegate to device ops               */
/* ------------------------------------------------------------------ */

static int tmpfs_file_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    tmpfs_file_t *f = node->handle;

    if (!f) return -EINVAL;
    if (f->device.open) return f->device.open(node, flags, private_data);
    *private_data = NULL;
    return 0;
}

static void tmpfs_file_release(vfs_node_t node, void *private_data)
{
    tmpfs_file_t *f = node->handle;

    if (!f) return;
    if (f->device.release) f->device.release(node, private_data);
}

static void tmpfs_file_descriptor_close(vfs_node_t node, void *private_data)
{
    tmpfs_file_t *f = node->handle;

    if (f && f->device.descriptor_close) f->device.descriptor_close(f->device.ctx, private_data);
}

static void *tmpfs_file_mmap(vfs_node_t node, void *private_data, size_t offset, size_t size, int flags, struct vm_area *vma)
{
    tmpfs_file_t *f = node->handle;

    (void)private_data;
    (void)vma;
    if (!f) return NULL;
    if (f->device.mmap) return f->device.mmap(f->device.ctx, private_data, offset, size, flags, vma);

    /* Regular tmpfs file: return the data buffer directly. */
    if (f->data && offset < f->size) return f->data + offset;
    return NULL;
}

static int64_t tmpfs_file_read(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    tmpfs_file_t *f = node->handle;

    if (!f) return -EINVAL;
    if (f->device.file_read) return f->device.file_read(f->device.ctx, private_data, flags, addr, offset, size);
    return (int64_t)tmpfs_read(f, addr, offset, size);
}

static int64_t tmpfs_file_write(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    tmpfs_file_t *f = node->handle;

    if (!f) return -EINVAL;
    if (f->device.file_write) return f->device.file_write(f->device.ctx, private_data, flags, addr, offset, size);
    return (int64_t)tmpfs_write(f, addr, offset, size);
}

#define TMPFS_USER_IO_CHUNK 16384

static int64_t tmpfs_file_read_user(vfs_node_t node, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size,
                                    struct process *proc)
{
    tmpfs_file_t *f = node->handle;
    if (!f) return -EINVAL;
    if (f->device.file_read_user) return f->device.file_read_user(f->device.ctx, private_data, flags, addr, offset, size, proc);

    uint8_t tmp[TMPFS_USER_IO_CHUNK];
    size_t  done = 0;
    while (done < size) {
        size_t  chunk = size - done < sizeof(tmp) ? size - done : sizeof(tmp);
        int64_t ret   = tmpfs_file_read(node, private_data, flags, tmp, offset + done, chunk);
        if (ret < 0) return done ? (int64_t)done : ret;
        if (!ret) break;
        if (copy_to_user((uint8_t *)addr + done, tmp, (size_t)ret)) return done ? (int64_t)done : -EFAULT;
        done += (size_t)ret;
        if ((size_t)ret < chunk) break;
    }
    return (int64_t)done;
}

static int64_t tmpfs_file_write_user(vfs_node_t node, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size,
                                     struct process *proc)
{
    tmpfs_file_t *f = node->handle;
    if (!f) return -EINVAL;
    if (f->device.file_write_user) return f->device.file_write_user(f->device.ctx, private_data, flags, addr, offset, size, proc);

    if (!size) {
        uint8_t empty = 0;
        return tmpfs_file_write(node, private_data, flags, &empty, offset, 0);
    }

    uint8_t tmp[TMPFS_USER_IO_CHUNK];
    size_t  done = 0;
    while (done < size) {
        size_t chunk = size - done < sizeof(tmp) ? size - done : sizeof(tmp);
        if (copy_from_user(tmp, (const uint8_t *)addr + done, chunk)) return done ? (int64_t)done : -EFAULT;
        int64_t ret = tmpfs_file_write(node, private_data, flags, tmp, offset + done, chunk);
        if (ret < 0) return done ? (int64_t)done : ret;
        if (!ret) break;
        done += (size_t)ret;
        if ((size_t)ret < chunk) break;
    }
    return (int64_t)done;
}

static int tmpfs_file_poll(vfs_node_t node, void *private_data, uint64_t flags, size_t events)
{
    tmpfs_file_t *f = node->handle;

    if (!f) return -EINVAL;
    if (f->device.file_poll) return f->device.file_poll(f->device.ctx, private_data, flags, events);
    return tmpfs_poll(f, events);
}

static vfs_poll_source_t *tmpfs_file_poll_source(vfs_node_t node, void *private_data)
{
    tmpfs_file_t *f = node->handle;

    if (!f || !f->device.file_poll_source) return NULL;
    return f->device.file_poll_source(f->device.ctx, private_data);
}

static int tmpfs_file_ioctl(vfs_node_t node, void *private_data, uint64_t flags, size_t req, void *arg)
{
    tmpfs_file_t *f = node->handle;

    if (!f) return -EINVAL;
    if (f->device.file_ioctl) return f->device.file_ioctl(f->device.ctx, private_data, flags, req, arg);
    return tmpfs_ioctl(f, req, arg);
}

static struct vfs_callback tmpfs_callbacks = {
    .mount                 = tmpfs_mount,
    .unmount               = tmpfs_umount,
    .open                  = (vfs_open_t)tmpfs_dummy,
    .close                 = (vfs_close_t)tmpfs_dummy,
    .read                  = tmpfs_read,
    .write                 = tmpfs_write,
    .readlink              = (vfs_readlink_t)tmpfs_dummy,
    .mkdir                 = tmpfs_mkdir,
    .mkfile                = tmpfs_mkfile,
    .link                  = tmpfs_link,
    .symlink               = tmpfs_symlink,
    .stat                  = tmpfs_stat,
    .ioctl                 = tmpfs_ioctl,
    .dup                   = tmpfs_dup,
    .poll                  = tmpfs_poll,
    .free                  = tmpfs_free,
    .delete                = tmpfs_delete,
    .rename                = tmpfs_rename,
    .file_open             = tmpfs_file_open,
    .file_release          = tmpfs_file_release,
    .file_descriptor_close = tmpfs_file_descriptor_close,
    .file_mmap             = tmpfs_file_mmap,
    .file_read             = tmpfs_file_read,
    .file_write            = tmpfs_file_write,
    .file_read_user        = tmpfs_file_read_user,
    .file_write_user       = tmpfs_file_write_user,
    .file_ioctl            = tmpfs_file_ioctl,
    .file_poll             = tmpfs_file_poll,
    .file_poll_source      = tmpfs_file_poll_source,
    .resize                = tmpfs_resize,
};

/* Register tmpfs with the VFS layer (initialize tmpfs) */
void tmpfs_regist(void)
{
    tmpfs_id = vfs_regist_fs_flags("tmpfs", &tmpfs_callbacks, VFS_FS_NODEV);
    if (!(tmpfs_id & ERRNO_MASK)) plogk("tmpfs: Filesystem registered (fsid=%d)\n", tmpfs_id);
    if (tmpfs_id & ERRNO_MASK) plogk("tmpfs: Register error.\n");

    /*
     * devtmpfs uses tmpfs storage and device callbacks, but is a distinct
     * mount type in the userspace ABI (/proc/filesystems and mountinfo).
     */
    devtmpfs_id = vfs_regist_fs_flags("devtmpfs", &tmpfs_callbacks, VFS_FS_NODEV);
    if (!(devtmpfs_id & ERRNO_MASK)) plogk("devtmpfs: Filesystem registered (fsid=%d)\n", devtmpfs_id);
    if (devtmpfs_id & ERRNO_MASK) plogk("devtmpfs: Register error.\n");
}

int tmpfs_bind_device(vfs_node_t node, uint16_t node_type, const tmpfs_device_ops_t *device)
{
    tmpfs_file_t *handle;

    if (!node || !node->handle || !device) return -EINVAL;
    handle = node->handle;

    handle->device    = *device;
    handle->node_type = node_type;
    node->type        = node_type;
    node->flags |= VFS_NODE_NOCACHE;
    return EOK;
}

int tmpfs_set_node_type(vfs_node_t node, uint16_t node_type)
{
    if (!node || !node->handle) return -EINVAL;
    tmpfs_file_t *handle = node->handle;
    if (handle->type != tp_file_file) return -EINVAL;
    handle->node_type = node_type;
    node->type        = node_type;
    return EOK;
}
