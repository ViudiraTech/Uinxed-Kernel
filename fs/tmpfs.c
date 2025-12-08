/*
 * 
 *      tmpfs.c
 *      Temporary file system
 *
 *      2025/11/3 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <errno.h>
#include <heap.h>
#include <printk.h>
#include <string.h>
#include <tmpfs.h>

int tmpfs_id = 0;

/* Mount the tmpfs file system to a specified VFS node */
int tmpfs_mount(const char *handle, vfs_node_t node)
{
    (void)handle;
    node->fsid = tmpfs_id;

    tmpfs_file_t *tmpfs_root = (tmpfs_file_t *)malloc(sizeof(tmpfs_file_t));
    tmpfs_root->type         = tp_file_dir;
    tmpfs_root->node         = node;
    tmpfs_root->root         = node;

    strcpy(tmpfs_root->name, "tmp");
    node->handle = tmpfs_root;
    return EOK;
}

/* Unmount the tmpfs file system and free related resources */
void tmpfs_umount(void *root)
{
    tmpfs_file_t *tmpfs_root = root;
    vfs_free(tmpfs_root->node);
}

/* Common function to create a tmpfs file or directory (internal use) */
int tmpfs_mk(void *parent, const char *name, vfs_node_t node, int is_dir)
{
    (void)parent;
    tmpfs_file_t *f = calloc(1, sizeof(tmpfs_file_t));
    strncpy(f->name, name, sizeof(f->name));

    f->type      = is_dir ? tp_file_dir : tp_file_file;
    node->handle = f;
    f->node      = node;

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

/* Read data from a tmpfs regular file */
size_t tmpfs_read(void *file, void *addr, size_t offset, size_t size)
{
    tmpfs_file_t *f = (tmpfs_file_t *)file;
    if (offset >= f->size) return 0;

    size_t actual = (offset + size > f->size) ? (f->size - offset) : size;
    memcpy(addr, f->data + offset, actual);
    return actual;
}

/* Write data to a tmpfs regular file */
size_t tmpfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    tmpfs_file_t *f   = (tmpfs_file_t *)file;
    size_t        end = offset + size;

    if (end > f->capacity) {
        size_t new_cap = end * 2;
        char  *new_buf = realloc(f->data, new_cap);

        if (!new_buf) return 0;

        f->data     = new_buf;
        f->capacity = new_cap;
    }

    memcpy(f->data + offset, addr, size);
    if (end > f->size) f->size = end;

    f->node->size = f->size;
    return size;
}

/* Get file status (type, size) of a tmpfs file/directory */
int tmpfs_stat(void *file, vfs_node_t node)
{
    tmpfs_file_t *file0 = (tmpfs_file_t *)file;
    if (!file0) return -ENOENT;

    node->type = file0->type == tp_file_symlink ? file_symlink : file0->type == tp_file_dir ? file_dir : file_none;
    node->size = file0->type == tp_file_dir ? 0 : file0->size;
    return EOK;
}

/* Delete a tmpfs file/directory and free its resources */
int tmpfs_delete(void *parent, vfs_node_t node)
{
    (void)parent;
    tmpfs_file_t *f = (tmpfs_file_t *)node->handle;
    free(f->data);
    free(f);
    return EOK;
}

/* Rename a tmpfs file/directory */
int tmpfs_rename(void *current, const char *new_name)
{
    tmpfs_file_t *f = (tmpfs_file_t *)current;
    strncpy(f->name, new_name, sizeof(f->name));
    return EOK;
}

/* Poll a tmpfs file for pending events (simplified implementation) */
int tmpfs_poll(void *file, size_t events)
{
    (void)file;
    int revents = 0;
    if (events & 0x0001) revents |= 0x0001;
    if (events & 0x0004) revents |= 0x0004;
    return revents;
}

/* Send control commands to a device or file */
int tmpfs_ioctl(void *file, size_t req, void *arg)
{
    (void)req;
    (void)arg;
    tmpfs_file_t *handle = file;
    if (handle->type == tp_file_char || handle->type == tp_file_blk) { return EOK; }
    return EOK;
}

/* Duplicate a VFS node bound to tmpfs */
vfs_node_t tmpfs_dup(vfs_node_t node)
{
    vfs_node_t copy   = vfs_node_alloc(node->parent, node->name);
    copy->handle      = node->handle;
    copy->type        = node->type;
    copy->size        = node->size;
    copy->linkname    = node->linkname == 0 ? 0 : strdup(node->linkname);
    copy->flags       = node->flags;
    copy->permissions = node->permissions;
    copy->owner       = node->owner;
    copy->child       = node->child;
    copy->realsize    = node->realsize;
    return copy;
}

/* Create a symbolic link in tmpfs */
int tmpfs_symlink(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    tmpfs_file_t *f = calloc(1, sizeof(tmpfs_file_t));

    strncpy(f->name, name, sizeof(f->name));
    f->type      = tp_file_symlink;
    node->handle = f;
    f->node      = node;

    return EOK;
}

/* Free resources of a tmpfs file/directory */
int tmpfs_free(void *handle)
{
    tmpfs_file_t *file = handle;

    if (file->type != tp_file_file) {
        free(file);
        return EOK;
    }
    if (file->data) free(file->data);

    free(file);
    return EOK;
}

/* Dummy function (placeholder for VFS callbacks not implemented) */
void tmpfs_dummy(void)
{
    /* Nothing */
}

static struct vfs_callback tmpfs_callbacks = {
    .mount    = tmpfs_mount,
    .unmount  = tmpfs_umount,
    .open     = (vfs_open_t)tmpfs_dummy,
    .close    = (vfs_close_t)tmpfs_dummy,
    .read     = tmpfs_read,
    .write    = tmpfs_write,
    .readlink = (vfs_readlink_t)tmpfs_dummy,
    .mkdir    = tmpfs_mkdir,
    .mkfile   = tmpfs_mkfile,
    .link     = (vfs_mk_t)tmpfs_dummy,
    .symlink  = tmpfs_symlink,
    .stat     = tmpfs_stat,
    .ioctl    = tmpfs_ioctl,
    .dup      = tmpfs_dup,
    .poll     = tmpfs_poll,
    .free     = tmpfs_free,
    .delete   = tmpfs_delete,
    .rename   = tmpfs_rename,
};

/* Register tmpfs with the VFS layer (initialize tmpfs) */
void tmpfs_regist(void)
{
    tmpfs_id = vfs_regist(&tmpfs_callbacks);
    if (tmpfs_id & ERRNO_MASK) plogk("tmpfs: Register error.\n");
}
