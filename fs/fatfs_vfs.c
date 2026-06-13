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
    int            opened;
    BYTE           open_mode;
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

static void fatfs_invalidate_directory(vfs_node_t node)
{
    if (!node) return;

    vfs_free_child(node);
    node->child   = 0;
    node->visited = 0;
}

static char *fatfs_rename_target(const char *path, const char *new_name)
{
    char   *target;
    char   *slash;
    size_t  prefix_len;
    size_t  name_len;

    if (!path || !new_name || !new_name[0]) return 0;

    slash      = strrchr(path, '/');
    prefix_len = slash ? (size_t)(slash - path + 1) : 0;
    name_len   = strlen(new_name);
    target     = malloc(prefix_len + name_len + 1);
    if (!target) return 0;

    if (prefix_len) memcpy(target, path, prefix_len);
    memcpy(target + prefix_len, new_name, name_len);
    target[prefix_len + name_len] = '\0';
    return target;
}

static int fatfs_refresh_node(vfs_node_t node)
{
    fatfs_handle_t *handle = node ? node->handle : 0;

    if (!node || !handle || !handle->path) return -EINVAL;
    if (fatfs_fill_info(handle, handle->path) != EOK) return -EIO;
    handle->is_dir = (handle->info.fattrib & AM_DIR) != 0;
    fatfs_apply_info(node, handle);
    return EOK;
}

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

    handle->opened = 1;
    handle->open_mode = mode;
    return EOK;
}

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
        handle->is_dir = 1;
        handle->opened = 1;
        handle->open_mode = 0;
    } else {
        res = f_open(&handle->file, handle->path, mode);
        if (res != FR_OK) return fatfs_result_to_errno(res);
        handle->is_dir = 0;
        handle->opened = 1;
        handle->open_mode = mode;
    }

    if (fatfs_fill_info(handle, handle->path) != EOK) return -EIO;
    fatfs_apply_info(node, handle);
    if (node->parent) node->parent->visited = 1;
    if (node->type & file_dir) node->visited = 1;
    return EOK;
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
    handle->opened    = 1;
    handle->open_mode = 0;
    handle->path      = strdup(mount->drive);
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
    if (fatfs_load_directory(node) != EOK) {
        f_closedir(&handle->dir);
        free(handle->path);
        f_unmount(mount->drive);
        free(handle);
        free(mount);
        node->handle = 0;
        return -EIO;
    }
    plogk("fatfs: Mounted %s\n", mount->drive);
    return EOK;
}

static void fatfs_vfs_unmount(void *root)
{
    fatfs_handle_destroy(root);
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

    handle->opened = 1;
    handle->open_mode = handle->is_dir ? 0 : (FA_READ | FA_OPEN_EXISTING);
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
    if (!handle->opened) return;

    if (handle->is_dir)
        f_closedir(&handle->dir);
    else
        f_close(&handle->file);
    handle->opened = 0;
}

static size_t fatfs_vfs_read(void *file, void *addr, size_t offset, size_t size)
{
    fatfs_handle_t *handle = file;
    UINT            read_count = 0;
    FRESULT         res;

    if (!handle || !addr || handle->is_dir) return 0;
    if (!handle->opened || handle->open_mode != (FA_READ | FA_OPEN_EXISTING)) {
        res = f_open(&handle->file, handle->path, FA_READ | FA_OPEN_EXISTING);
        if (res != FR_OK) return 0;
        handle->opened = 1;
        handle->open_mode = FA_READ | FA_OPEN_EXISTING;
    }

    res = f_lseek(&handle->file, offset);
    if (res != FR_OK) return 0;

    res = f_read(&handle->file, addr, size, &read_count);
    if (res != FR_OK) return 0;
    return read_count;
}

static size_t fatfs_vfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    fatfs_handle_t *handle = file;
    UINT            written = 0;
    FRESULT         res;

    if (!handle || !addr || handle->is_dir) return 0;
    if (fatfs_prepare_file_handle(handle, FA_WRITE | FA_OPEN_EXISTING) != EOK) return 0;

    res = f_lseek(&handle->file, offset);
    if (res != FR_OK) return 0;

    res = f_write(&handle->file, addr, size, &written);
    if (res != FR_OK) return 0;
    if (f_sync(&handle->file) != FR_OK) return 0;

    if (handle->info.fsize < offset + written) handle->info.fsize = offset + written;
    handle->open_mode = FA_WRITE | FA_OPEN_EXISTING;

    return written;
}

static size_t fatfs_vfs_readlink(vfs_node_t node, void *addr, size_t offset, size_t size)
{
    (void)node;
    (void)addr;
    (void)offset;
    (void)size;
    return 0;
}

static int fatfs_vfs_mkdir(void *parent, const char *name, vfs_node_t node)
{
    fatfs_handle_t *dir = parent;

    if (!dir || !node || !name) return -EINVAL;
    node->type = file_dir;
    return fatfs_create_path(node, 0, 1);
}

static int fatfs_vfs_mkfile(void *parent, const char *name, vfs_node_t node)
{
    fatfs_handle_t *dir = parent;

    if (!dir || !node || !name) return -EINVAL;
    node->type = file_none;
    return fatfs_create_path(node, FA_CREATE_NEW | FA_WRITE, 0);
}

static int fatfs_vfs_no_link(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -ENOSYS;
}

static int fatfs_vfs_stat(void *file, vfs_node_t node)
{
    fatfs_handle_t *handle = file;

    if (!handle || !node) return -EINVAL;
    if (fatfs_refresh_node(node) != EOK) return -EIO;
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
    copy->linkname    = node->linkname ? strdup(node->linkname) : 0;
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
    fatfs_handle_destroy(handle);
    return EOK;
}

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

static int fatfs_vfs_rename(void *current, const char *new_name)
{
    fatfs_handle_t *handle = current;
    char           *new_path;
    FRESULT         res;

    if (!handle || !handle->path || !new_name) return -EINVAL;
    if (!new_name[0]) return -EINVAL;

    new_path = fatfs_rename_target(handle->path, new_name);
    if (!new_path) return -ENOMEM;

    res = f_rename(handle->path, new_path);
    if (res != FR_OK) {
        free(new_path);
        return fatfs_result_to_errno(res);
    }

    free(handle->path);
    handle->path = new_path;
    handle->opened = 0;
    handle->open_mode = 0;
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
};

void fatfs_vfs_regist(void)
{
    fatfs_vfs_id = vfs_regist_fs("fatfs", &fatfs_vfs_callbacks);
    if (fatfs_vfs_id & ERRNO_MASK) plogk("fatfs: Register error.\n");
}

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
