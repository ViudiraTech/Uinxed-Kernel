/*
 *
 *      fatfs_vfs.c
 *      FatFs bridge for VFS
 *
 *      2026/5/18 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/block/ata/pata/ide.h>
#include <drivers/block/core/blockdev.h>
#include <fs/core/vfs.h>
#include <fs/fatfs/fatfs_disk.h>
#include <fs/fatfs/fatfs_vfs.h>
#include <fs/fatfs/ff.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>

static int fatfs_vfs_id = 0;

typedef struct fatfs_mount {
        FATFS fs;
        char  drive[4];
} fatfs_mount_t;

typedef struct fatfs_handle {
        fatfs_mount_t *mount;
        FIL            file;
        DIR            dir;
        FILINFO        info;
        char          *path;
        int            is_dir;
        int            opened;
        BYTE           open_mode;
        int            owns_mount;
} fatfs_handle_t;

/* Map a FatFs result code to a kernel errno. */
static int fatfs_result_to_errno(FRESULT res)
{
    switch (res) {
        case FR_OK :
            return EOK;
        case FR_NO_FILE :
        case FR_NO_PATH :
            return -ENOENT;
        case FR_INVALID_NAME :
        case FR_INVALID_PARAMETER :
            return -EINVAL;
        case FR_EXIST :
            return -EEXIST;
        case FR_DENIED :
            return -EACCES;
        case FR_NOT_READY :
            return -ENODEV;
        case FR_WRITE_PROTECTED :
            return -EROFS;
        case FR_LOCKED :
            return -EBUSY;
        case FR_TOO_MANY_OPEN_FILES :
            return -EMFILE;
        case FR_NOT_ENOUGH_CORE :
            return -ENOMEM;
        default :
            return -EIO;
    }
}

/* Close any open FatFs object and free the handle. */
static void fatfs_handle_destroy(fatfs_handle_t *handle)
{
    if (!handle) return;

    if (handle->opened) {
        if (handle->is_dir)
            f_closedir(&handle->dir);
        else
            f_close(&handle->file);
        handle->opened = 0;
    }

    free(handle->path);
    if (handle->owns_mount && handle->mount) {
        f_unmount(handle->mount->drive);
        free(handle->mount);
    }
    free(handle);
}

/* Join a parent path and a component into a FatFs path. */
static char *fatfs_join_path(const char *base, const char *name)
{
    size_t base_len;
    size_t name_len;
    char  *path;

    if (!base || !name) return 0;

    base_len = strlen(base);
    name_len = strlen(name);
    path     = malloc(base_len + name_len + 2);
    if (!path) return 0;

    memcpy(path, base, base_len);
    if (base_len > 0 && path[base_len - 1] == '/') base_len--;
    path[base_len++] = '/';

    memcpy(path + base_len, name, name_len);
    path[base_len + name_len] = '\0';
    return path;
}

/* Query FatFs stat data for a path into the handle. */
static int fatfs_fill_info(fatfs_handle_t *handle, const char *path)
{
    FRESULT res;

    memset(&handle->info, 0, sizeof(handle->info));
    res = f_stat(path, &handle->info);
    return fatfs_result_to_errno(res);
}

/* Copy the handle's stat data into a VFS node. */
static void fatfs_apply_info(vfs_node_t node, fatfs_handle_t *handle)
{
    node->size        = handle->is_dir ? 0 : handle->info.fsize;
    node->realsize    = node->size;
    node->type        = (handle->info.fattrib & AM_DIR) ? file_dir : file_none;
    node->permissions = (handle->info.fattrib & AM_RDO) ? 0444 : 0666;
}

/* Drop a directory's cached children so they are re-read from disk. */
static void fatfs_invalidate_directory(vfs_node_t node)
{
    if (!node) return;

    vfs_free_child(node);
    node->child   = 0;
    node->visited = 0;
}

/* Re-query the node's stat data and refresh its VFS fields. */
static int fatfs_refresh_node(vfs_node_t node)
{
    fatfs_handle_t *handle = node ? node->handle : 0;

    if (!node || !handle || !handle->path) return -EINVAL;
    if (fatfs_fill_info(handle, handle->path) != EOK) return -EIO;
    handle->is_dir = (handle->info.fattrib & AM_DIR) != 0;
    fatfs_apply_info(node, handle);
    return EOK;
}

/* Open the FatFs file object with the given access mode. */
static int fatfs_prepare_file_handle(fatfs_handle_t *handle, BYTE mode)
{
    FRESULT res;

    if (!handle || handle->is_dir) return -EINVAL;
    if (handle->opened) {
        f_close(&handle->file);
        handle->opened = 0;
    }

    res = f_open(&handle->file, handle->path, mode);
    if (res != FR_OK) return fatfs_result_to_errno(res);

    handle->opened    = 1;
    handle->open_mode = mode;
    return EOK;
}

/* Attach a new FatFs handle to a VFS node using its parent's mount. */
static int fatfs_attach_new_node(vfs_node_t node)
{
    fatfs_handle_t *parent;
    fatfs_handle_t *handle;

    if (!node || !node->parent || node->handle) return -EINVAL;

    parent = node->parent->handle;
    if (!parent || !parent->mount) return -EINVAL;

    handle = calloc(1, sizeof(fatfs_handle_t));
    if (!handle) return -ENOMEM;

    handle->mount  = parent->mount;
    handle->path   = fatfs_join_path(parent->path, node->name);
    handle->opened = 0;
    if (!handle->path) {
        free(handle);
        return -ENOMEM;
    }

    node->handle = handle;
    return EOK;
}

/* Create a directory or file through FatFs and bind it to the node. */
static int fatfs_create_path(vfs_node_t node, BYTE mode, int is_dir)
{
    fatfs_handle_t *handle = node ? node->handle : 0;
    FRESULT         res;

    if (!node) return -EINVAL;
    if (!handle) {
        if (fatfs_attach_new_node(node) != EOK) return -EINVAL;
        handle = node->handle;
    }
    if (!handle->path) return -EINVAL;

    if (is_dir) {
        res = f_mkdir(handle->path);
        if (res != FR_OK) return fatfs_result_to_errno(res);
        res = f_opendir(&handle->dir, handle->path);
        if (res != FR_OK) return fatfs_result_to_errno(res);
        handle->is_dir    = 1;
        handle->opened    = 1;
        handle->open_mode = 0;
    } else {
        res = f_open(&handle->file, handle->path, mode);
        if (res != FR_OK) return fatfs_result_to_errno(res);
        handle->is_dir    = 0;
        handle->opened    = 1;
        handle->open_mode = mode;
    }

    if (fatfs_fill_info(handle, handle->path) != EOK) return -EIO;
    fatfs_apply_info(node, handle);
    if (node->parent) node->parent->visited = 1;
    if (node->type & file_dir) node->visited = 1;
    return EOK;
}

/* Materialize a directory's entries as VFS child nodes. */
static int fatfs_load_directory(vfs_node_t node)
{
    fatfs_handle_t *handle = node ? node->handle : 0;
    DIR             dir;
    FILINFO         info;
    FRESULT         res;

    if (!node || !handle || !handle->is_dir) return -EINVAL;
    if (node->visited) return EOK;

    res = f_opendir(&dir, handle->path);
    if (res != FR_OK) return fatfs_result_to_errno(res);

    while (1) {
        fatfs_handle_t *child_handle;
        vfs_node_t      child;

        memset(&info, 0, sizeof(info));
        res = f_readdir(&dir, &info);
        if (res != FR_OK) {
            f_closedir(&dir);
            return fatfs_result_to_errno(res);
        }
        if (!info.fname[0]) break;
        if (!strcmp(info.fname, ".") || !strcmp(info.fname, "..")) continue;

        child = vfs_do_search(node, info.fname);
        if (child) continue;

        child_handle = calloc(1, sizeof(fatfs_handle_t));
        if (!child_handle) {
            f_closedir(&dir);
            return -ENOMEM;
        }

        child_handle->mount  = handle->mount;
        child_handle->info   = info;
        child_handle->is_dir = (info.fattrib & AM_DIR) != 0;
        child_handle->opened = 0;

        child = vfs_node_alloc(node, info.fname);
        if (!child) {
            free(child_handle);
            f_closedir(&dir);
            return -ENOMEM;
        }

        child_handle->path = fatfs_join_path(handle->path, info.fname);
        if (!child_handle->path) {
            node->child = clist_delete(node->child, child);
            free(child_handle);
            free(child);
            f_closedir(&dir);
            return -ENOMEM;
        }

        child->handle = child_handle;
        fatfs_apply_info(child, child_handle);
    }

    f_closedir(&dir);
    node->visited = 1;
    return EOK;
}

/* Mount a FatFs volume, binding a block device or volume specifier. */
static int fatfs_vfs_mount(const char *src, vfs_node_t node)
{
    fatfs_mount_t  *mount;
    fatfs_handle_t *handle;
    FRESULT         res;

    if (!src || !node) return -EINVAL;

    char vol_str[4] = {0};

    if (strlen(src) == 2 && src[1] == ':') {
        memcpy(vol_str, src, 3);
    } else {
        blockdev_device_t bdev;
        if (blockdev_open_name(src, &bdev) != EOK) return -ENODEV;

        uint8_t pdrv;
        for (pdrv = 0; pdrv < FF_VOLUMES; pdrv++) {
            int exists = 0;
            for (int v = 0; v < FF_VOLUMES; v++) {
                if (VolToPart[v].pd == pdrv) {
                    exists = 1;
                    break;
                }
            }
            if (!exists) break;
        }
        if (pdrv >= FF_VOLUMES) return -ENOSPC;

        fatfs_bind_device(pdrv, &bdev);
        fatfs_assign_volume(pdrv, pdrv, 0);

        vol_str[0] = (char)('0' + pdrv);
        vol_str[1] = ':';
        vol_str[2] = '\0';
    }

    mount = calloc(1, sizeof(fatfs_mount_t));
    if (!mount) return -ENOMEM;

    memcpy(mount->drive, vol_str, 4);
    res = f_mount(&mount->fs, mount->drive, 1);
    if (res != FR_OK) {
        free(mount);
        return fatfs_result_to_errno(res);
    }

    handle = calloc(1, sizeof(fatfs_handle_t));
    if (!handle) {
        f_unmount(mount->drive);
        free(mount);
        return -ENOMEM;
    }

    handle->mount      = mount;
    handle->owns_mount = 1;
    handle->is_dir     = 1;
    handle->opened     = 1;
    handle->open_mode  = 0;
    handle->path       = strdup(mount->drive);
    if (!handle->path) {
        f_unmount(mount->drive);
        free(handle);
        free(mount);
        return -ENOMEM;
    }

    res = f_opendir(&handle->dir, mount->drive);
    if (res != FR_OK) {
        free(handle->path);
        f_unmount(mount->drive);
        free(handle);
        free(mount);
        return fatfs_result_to_errno(res);
    }

    node->handle  = handle;
    node->type    = file_dir;
    node->blksz   = handle->mount->fs.ssize;
    node->visited = 0;
    if (fatfs_load_directory(node) != EOK) {
        f_closedir(&handle->dir);
        free(handle->path);
        f_unmount(mount->drive);
        free(handle);
        free(mount);
        node->handle = 0;
        return -EIO;
    }
    plogk("fatfs: Mounted %s (source: %s)\n", mount->drive, src);
    return EOK;
}

/* Unmount and destroy the FatFs handle. */
static void fatfs_vfs_unmount(void *root)
{
    fatfs_handle_destroy(root);
}

/* Open a child node's FatFs file or directory object. */
static int fatfs_open_child(vfs_node_t node)
{
    fatfs_handle_t *parent = node->parent ? node->parent->handle : 0;
    fatfs_handle_t *handle;
    char           *fullpath;
    FRESULT         res;
    int             status;

    if (!parent || !parent->mount) return -EINVAL;

    fullpath = fatfs_join_path(parent->path, node->name);
    if (!fullpath) return -ENOMEM;

    handle = calloc(1, sizeof(fatfs_handle_t));
    if (!handle) {
        free(fullpath);
        return -ENOMEM;
    }

    handle->mount = parent->mount;
    handle->path  = fullpath;
    status        = fatfs_fill_info(handle, fullpath);
    if (status != EOK) {
        free(handle->path);
        free(handle);
        return status;
    }

    handle->is_dir = (handle->info.fattrib & AM_DIR) != 0;
    if (handle->is_dir)
        res = f_opendir(&handle->dir, fullpath);
    else
        res = f_open(&handle->file, fullpath, FA_READ | FA_OPEN_EXISTING);

    if (res != FR_OK) {
        free(handle->path);
        free(handle);
        return fatfs_result_to_errno(res);
    }

    handle->opened    = 1;
    handle->open_mode = handle->is_dir ? 0 : (FA_READ | FA_OPEN_EXISTING);
    node->handle      = handle;
    fatfs_apply_info(node, handle);
    return EOK;
}

/* Open a child node's FatFs object. */
static void fatfs_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;

    if (!node || !node->parent) return;
    if (fatfs_open_child(node) != EOK) return;
    if (node->type & file_dir) fatfs_load_directory(node);
}

/* Close the open FatFs object. */
static void fatfs_vfs_close(void *current)
{
    fatfs_handle_t *handle = current;

    if (!handle) return;
    if (!handle->opened) return;

    if (handle->is_dir)
        f_closedir(&handle->dir);
    else
        f_close(&handle->file);
    handle->opened = 0;
}

/* Read from a FatFs file at the given offset. */
static size_t fatfs_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    fatfs_handle_t *handle     = file;
    UINT            read_count = 0;
    FRESULT         res;

    if (!handle || !addr || handle->is_dir) return 0;
    if (!handle->opened || handle->open_mode != (FA_READ | FA_OPEN_EXISTING)) {
        if (fatfs_prepare_file_handle(handle, FA_READ | FA_OPEN_EXISTING) != EOK) {
            plogk("fatfs: %s: open for read failed.\n", handle->path);
            return 0;
        }
    }

    res = f_lseek(&handle->file, offset);
    if (res != FR_OK) {
        plogk("fatfs: %s: seek failed: %d\n", handle->path, res);
        return 0;
    }

    res = f_read(&handle->file, addr, size, &read_count);
    if (res != FR_OK) {
        plogk("fatfs: %s: read failed: %d\n", handle->path, res);
        return 0;
    }
    return read_count;
}

/* Write to a FatFs file at the given offset, syncing afterwards. */
static size_t fatfs_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    fatfs_handle_t *handle  = file;
    UINT            written = 0;
    FRESULT         res;

    if (!handle || !addr || handle->is_dir) return 0;
    if (fatfs_prepare_file_handle(handle, FA_WRITE | FA_OPEN_EXISTING) != EOK) {
        plogk("fatfs: %s: prepare for write failed.\n", handle->path);
        return 0;
    }

    res = f_lseek(&handle->file, offset);
    if (res != FR_OK) {
        plogk("fatfs: %s: seek failed: %d\n", handle->path, res);
        return 0;
    }

    res = f_write(&handle->file, addr, size, &written);
    if (res != FR_OK) {
        if (res != FR_DENIED) plogk("fatfs: %s: write failed: %d\n", handle->path, res);
        return 0;
    }
    if (f_sync(&handle->file) != FR_OK) {
        plogk("fatfs: %s: sync failed.\n", handle->path);
        return 0;
    }

    if (handle->info.fsize < offset + written) handle->info.fsize = offset + written;
    handle->open_mode = FA_WRITE | FA_OPEN_EXISTING;

    return written;
}

/* Truncate a FatFs file to the given size. */
static int fatfs_vfs_resize(void *file, uint64_t size)
{
    fatfs_handle_t *handle = file;
    if (!handle || handle->is_dir || size > (FSIZE_t)-1) return -EINVAL;
    int status = fatfs_prepare_file_handle(handle, FA_WRITE | FA_OPEN_EXISTING);
    if (status != EOK) return status;
    FRESULT result = f_lseek(&handle->file, (FSIZE_t)size);
    if (result == FR_OK) result = f_truncate(&handle->file);
    if (result == FR_OK) result = f_sync(&handle->file);
    if (result != FR_OK) return fatfs_result_to_errno(result);
    handle->info.fsize = (FSIZE_t)size;
    return EOK;
}

/* Flush the open FatFs file to disk. */
static int fatfs_vfs_sync(void *file, int data_only)
{
    (void)data_only;
    fatfs_handle_t *handle = file;
    if (!handle || handle->is_dir) return -EINVAL;
    if (!handle->opened) return EOK;
    return fatfs_result_to_errno(f_sync(&handle->file));
}

/* FatFs has no symlinks; return nothing. */
static size_t fatfs_vfs_readlink(vfs_node_t node, void *addr, size_t offset, size_t size)
{
    (void)node;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}

/* Create a directory through FatFs. */
static int fatfs_vfs_mkdir(void *parent, const char *name, vfs_node_t node)
{
    fatfs_handle_t *dir = parent;

    if (!dir || !node || !name) return -EINVAL;
    node->type = file_dir;
    return fatfs_create_path(node, 0, 1);
}

/* Create a regular file through FatFs. */
static int fatfs_vfs_mkfile(void *parent, const char *name, vfs_node_t node)
{
    fatfs_handle_t *dir = parent;

    if (!dir || !node || !name) return -EINVAL;
    node->type = file_none;
    return fatfs_create_path(node, FA_CREATE_NEW | FA_WRITE, 0);
}

/* Reject hard-link and symlink creation. */
static int fatfs_vfs_no_link(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -EPERM;
}

/* Refresh a node's stat data and load directories on demand. */
static int fatfs_vfs_stat(void *file, vfs_node_t node)
{
    fatfs_handle_t *handle = file;

    if (!handle || !node) return -EINVAL;
    if (fatfs_refresh_node(node) != EOK) return -EIO;
    if (node->type & file_dir) return fatfs_load_directory(node);
    return EOK;
}

/* Reject ioctl requests (fatfs supports none). */
static int fatfs_vfs_ioctl(void *file, size_t req, void *arg)
{
    (void)file;
    (void)req;
    (void)arg;
    return -ENOTTY;
}

/* Duplicate a VFS node sharing its parent's mount. */
static vfs_node_t fatfs_vfs_dup(vfs_node_t node)
{
    vfs_node_t copy;

    if (!node) return 0;
    copy = vfs_node_alloc(node->parent, node->name);
    if (!copy) return 0;

    copy->type     = node->type;
    copy->size     = node->size;
    copy->realsize = node->realsize;
    copy->blksz    = node->blksz;
    if (node->linkname) {
        copy->linkname = strdup(node->linkname);
        if (!copy->linkname) plogk("fatfs: linkname strdup failed.\n");
    } else
        copy->linkname = 0;
    copy->permissions = node->permissions;
    return copy;
}

/* Report the requested events as ready. */
static int fatfs_vfs_poll(void *file, size_t events)
{
    (void)file;
    return (int)events;
}

/* Close and free the FatFs handle. */
static int fatfs_vfs_free(void *handle)
{
    fatfs_handle_destroy(handle);
    return EOK;
}

/* Unlink a FatFs file. */
static int fatfs_vfs_delete(void *parent, vfs_node_t node)
{
    fatfs_handle_t *handle = node ? node->handle : 0;
    FRESULT         res;

    if (!parent || !node || !handle || !handle->path) return -EINVAL;
    res = f_unlink(handle->path);
    if (res != FR_OK) return fatfs_result_to_errno(res);

    fatfs_invalidate_directory(node->parent);
    return EOK;
}

/* FatFs can move within a volume, but cannot atomically replace a victim. */
static int fatfs_vfs_rename(const vfs_rename_context_t *context)
{
    fatfs_handle_t *handle;
    fatfs_handle_t *new_parent;
    char           *new_path;
    FRESULT         res;

    if (!context || !context->source || !context->new_parent || !context->new_name) return -EINVAL;
    if (context->target) return -EOPNOTSUPP;
    if (context->source->type & file_dir) return -EOPNOTSUPP;
    handle     = context->source->handle;
    new_parent = context->new_parent->handle;
    if (!handle || !handle->path || !new_parent || !new_parent->path || handle->mount != new_parent->mount) return -EXDEV;

    new_path = fatfs_join_path(new_parent->path, context->new_name);
    if (!new_path) return -ENOMEM;

    /* FatFs keeps an open FIL locked; close it before changing its path. */
    if (handle->opened) {
        res = f_close(&handle->file);
        if (res != FR_OK) {
            free(new_path);
            return fatfs_result_to_errno(res);
        }
        handle->opened    = 0;
        handle->open_mode = 0;
    }
    res = f_rename(handle->path, new_path);
    if (res != FR_OK) {
        free(new_path);
        return fatfs_result_to_errno(res);
    }

    free(handle->path);
    handle->path = new_path;
    return EOK;
}

static struct vfs_callback fatfs_vfs_callbacks = {
    .mount    = fatfs_vfs_mount,
    .unmount  = fatfs_vfs_unmount,
    .open     = fatfs_vfs_open,
    .close    = fatfs_vfs_close,
    .read     = fatfs_vfs_read,
    .write    = fatfs_vfs_write,
    .readlink = fatfs_vfs_readlink,
    .mkdir    = fatfs_vfs_mkdir,
    .mkfile   = fatfs_vfs_mkfile,
    .link     = fatfs_vfs_no_link,
    .symlink  = fatfs_vfs_no_link,
    .stat     = fatfs_vfs_stat,
    .ioctl    = fatfs_vfs_ioctl,
    .dup      = fatfs_vfs_dup,
    .poll     = fatfs_vfs_poll,
    .free     = fatfs_vfs_free,
    .delete   = fatfs_vfs_delete,
    .rename   = fatfs_vfs_rename,
    .resize   = fatfs_vfs_resize,
    .sync     = fatfs_vfs_sync,
};

/* Register the fatfs filesystem with the VFS layer. */
void fatfs_vfs_regist(void)
{
#if CONFIG_FAT_FS
    f_setcp(936); // Default active code page (GBK) for SFN
    fatfs_vfs_id = vfs_regist_fs("fatfs", &fatfs_vfs_callbacks);
    if (fatfs_vfs_id & ERRNO_MASK) plogk("fatfs: Register error.\n");
    if (!(fatfs_vfs_id & ERRNO_MASK)) plogk("fatfs: Filesystem registered (fsid=%d)\n", fatfs_vfs_id);
#endif
}

/* Mount a FatFs volume at a VFS path. */
int fatfs_vfs_mount_volume(const char *src, const char *path)
{
    vfs_node_t node;
    int        status;

    if (!src || !path) return -EINVAL;

    status = vfs_mkdir(path);
    if (status != EOK) return status;

    node = vfs_open(path);
    if (!node) return -ENOENT;
    if (node->is_mount) {
        vfs_close(node);
        return EOK;
    }

    status = vfs_mount_fs("fatfs", src, node);
    vfs_close(node);
    return status;
}
