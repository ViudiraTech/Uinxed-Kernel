/*
 *
 *      fatfs_vfs.c
 *      FatFs bridge for VFS
 *
 *      2026/5/18 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <errno.h>
#include <ff.h>
#include <fatfs_vfs.h>
#include <heap.h>
#include <ide.h>
#include <printk.h>
#include <string.h>
#include <vfs.h>

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
    int            owns_mount;
} fatfs_handle_t;

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
        default :
            return -EIO;
    }
}

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
    if (base_len > 1 && path[base_len - 1] == '/') base_len--;
    if (base_len == 1 && path[0] == '/')
        (void)0;
    else
        path[base_len++] = '/';

    memcpy(path + base_len, name, name_len);
    path[base_len + name_len] = '\0';
    return path;
}

static int fatfs_fill_info(fatfs_handle_t *handle, const char *path)
{
    FRESULT res;

    memset(&handle->info, 0, sizeof(handle->info));
    res = f_stat(path, &handle->info);
    return fatfs_result_to_errno(res);
}

static void fatfs_apply_info(vfs_node_t node, fatfs_handle_t *handle)
{
    node->size        = handle->is_dir ? 0 : handle->info.fsize;
    node->realsize    = node->size;
    node->type        = (handle->info.fattrib & AM_DIR) ? file_dir : file_none;
    node->permissions = (handle->info.fattrib & AM_RDO) ? 0444 : 0666;
}

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

        child_handle->mount = handle->mount;
        child_handle->info  = info;
        child_handle->is_dir = (info.fattrib & AM_DIR) != 0;

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

static int fatfs_vfs_mount(const char *src, vfs_node_t node)
{
    fatfs_mount_t  *mount;
    fatfs_handle_t *handle;
    FRESULT         res;

    if (!src || !node) return -EINVAL;
    if (strlen(src) != 2 || src[1] != ':') return -EINVAL;

    mount = calloc(1, sizeof(fatfs_mount_t));
    if (!mount) return -ENOMEM;

    memcpy(mount->drive, src, 3);
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

    handle->mount     = mount;
    handle->owns_mount = 1;
    handle->is_dir    = 1;
    handle->path      = strdup("/");
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

    node->handle = handle;
    node->type   = file_dir;
    node->blksz  = 512;
    node->visited = 0;
    plogk("fatfs: Mounted %s\n", mount->drive);
    return EOK;
}

static void fatfs_vfs_unmount(void *root)
{
    fatfs_handle_t *handle = root;

    if (!handle) return;
    if (handle->is_dir) f_closedir(&handle->dir);
    if (handle->owns_mount && handle->mount) f_unmount(handle->mount->drive);
    free(handle->path);
    if (handle->owns_mount) free(handle->mount);
    free(handle);
}

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

    node->handle = handle;
    fatfs_apply_info(node, handle);
    return EOK;
}

static void fatfs_vfs_open(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;

    if (!node || !node->parent) return;
    if (fatfs_open_child(node) != EOK) return;
    if (node->type & file_dir) fatfs_load_directory(node);
}

static void fatfs_vfs_close(void *current)
{
    fatfs_handle_t *handle = current;

    if (!handle) return;
    if (handle->is_dir)
        f_closedir(&handle->dir);
    else
        f_close(&handle->file);
}

static size_t fatfs_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    fatfs_handle_t *handle = file;
    UINT            read_count = 0;

    if (!handle || !addr || handle->is_dir) return 0;
    if (f_lseek(&handle->file, offset) != FR_OK) return 0;
    if (f_read(&handle->file, addr, size, &read_count) != FR_OK) return 0;
    return read_count;
}

static size_t fatfs_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    (void)file;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}

static size_t fatfs_vfs_readlink(vfs_node_t node, void *addr, size_t offset, size_t size)
{
    (void)node;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}

static int fatfs_vfs_no_mk(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -EROFS;
}

static int fatfs_vfs_stat(void *file, vfs_node_t node)
{
    fatfs_handle_t *handle = file;

    if (!handle || !node) return -EINVAL;
    fatfs_apply_info(node, handle);
    if (node->type & file_dir) return fatfs_load_directory(node);
    return EOK;
}

static int fatfs_vfs_ioctl(void *file, size_t req, void *arg)
{
    (void)file;
    (void)req;
    (void)arg;
    return -ENOSYS;
}

static vfs_node_t fatfs_vfs_dup(vfs_node_t node)
{
    vfs_node_t copy;

    if (!node) return 0;
    copy = vfs_node_alloc(node->parent, node->name);
    if (!copy) return 0;

    copy->type        = node->type;
    copy->size        = node->size;
    copy->realsize    = node->realsize;
    copy->blksz       = node->blksz;
    copy->permissions = node->permissions;
    return copy;
}

static int fatfs_vfs_poll(void *file, size_t events)
{
    (void)file;
    return (int)events;
}

static int fatfs_vfs_free(void *handle)
{
    (void)handle;
    return EOK;
}

static int fatfs_vfs_delete(void *parent, vfs_node_t node)
{
    (void)parent;
    (void)node;
    return -EROFS;
}

static int fatfs_vfs_rename(void *current, const char *new_name)
{
    (void)current;
    (void)new_name;
    return -EROFS;
}

static struct vfs_callback fatfs_vfs_callbacks = {
    .mount    = fatfs_vfs_mount,
    .unmount  = fatfs_vfs_unmount,
    .open     = fatfs_vfs_open,
    .close    = fatfs_vfs_close,
    .read     = fatfs_vfs_read,
    .write    = fatfs_vfs_write,
    .readlink = fatfs_vfs_readlink,
    .mkdir    = fatfs_vfs_no_mk,
    .mkfile   = fatfs_vfs_no_mk,
    .link     = fatfs_vfs_no_mk,
    .symlink  = fatfs_vfs_no_mk,
    .stat     = fatfs_vfs_stat,
    .ioctl    = fatfs_vfs_ioctl,
    .dup      = fatfs_vfs_dup,
    .poll     = fatfs_vfs_poll,
    .free     = fatfs_vfs_free,
    .delete   = fatfs_vfs_delete,
    .rename   = fatfs_vfs_rename,
};

void fatfs_vfs_regist(void)
{
    fatfs_vfs_id = vfs_regist(&fatfs_vfs_callbacks);
    if (fatfs_vfs_id & ERRNO_MASK) plogk("fatfs: Register error.\n");
}

void fatfs_vfs_mount_all(void)
{
    vfs_node_t root = get_rootdir();

    if (!root || !root->fsid) return;
    if (vfs_mkdir("/fat") != EOK) return;

    for (uint8_t drive = 0; drive < 4; drive++) {
        char       path[] = "/fat/ide0";
        char       src[]  = {'0' + drive, ':', '\0', '\0'};
        vfs_node_t node;

        if (!ide_devices[drive].reserved || ide_devices[drive].type != IDE_ATA) continue;

        path[8] = (char)('0' + drive);
        vfs_mkdir(path);
        node = vfs_open(path);
        if (!node || node->is_mount) continue;

        if (vfs_mount(src, node) == EOK)
            plogk("fatfs: Auto-mounted %s at %s\n", src, path);
        else {
            FATFS fs;
            FRESULT res = f_mount(&fs, src, 1);

            plogk("fatfs: Failed to mount %s at %s, f_mount=%d\n", src, path, res);
            if (res == FR_OK) f_unmount(src);
        }
    }
}
