/*
 * 
 *      vfs.c
 *      Virtual file system
 *
 *      2025/11/2 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/inotify.h>
#include <fs/pagecache.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <proc/process.h>
#include <sync/spin_lock.h>

#define VFS_ACCESS_R 4
#define VFS_ACCESS_W 2

#ifndef VFS_PATH_TEST_ONLY
vfs_node_t        rootdir = 0;
static spinlock_t vfs_namespace_lock;

/*
 * Check file access permissions against the current process.
 * Returns 0 if access is granted, -EACCES otherwise.
 * Kernel-internal calls (no process context) and root (uid==0) bypass checks.
 */
int vfs_access_check(vfs_node_t node, uint32_t access_mask)
{
    process_t *proc = process_current();
    if (!proc || proc->uid == 0) return 0;
    if (proc->uid == node->owner && (node->mode & (access_mask << 6)) == (access_mask << 6)) return 0;
    if (proc->gid == node->group && (node->mode & (access_mask << 3)) == (access_mask << 3)) return 0;
    if ((node->mode & access_mask) == access_mask) return 0;
    return -EACCES;
}

struct vfs_callback vfs_empty_callback;
vfs_callback_t      fs_callbacks[256] = {[0] = &vfs_empty_callback};
static const char  *fs_names[256];
static int          fs_nextid = 1;

/* Default callback function (does nothing) */
static int empty_func(void)
{
    return -ENOSYS;
}

/* Tokenize the path string, splitting it by '/' */
static char *pathtok(char **sp)
{
    char *e = *sp;
    while (*e == '/') e++;
    if (*e == '\0') {
        *sp = e;
        return 0;
    }

    char *s = e;
    while (*e != '\0' && *e != '/') e++;

    char *next = e;
    if (*e == '/') next++;
    if (*e != '\0') *e = '\0';

    *sp = next;
    return s;
}

static void *vfs_page_alloc(uint64_t *physical)
{
    size_t reserve = frame_allocator.origin_frames / 32;
    if (reserve < 256) reserve = 256;
    size_t available = __atomic_load_n(&frame_allocator.usable_frames, __ATOMIC_ACQUIRE);
    if (available <= reserve) {
        size_t target = reserve - available + 1;
        if (target > 64) target = 64;
        (void)pagecache_reclaim(target);
    }
    uint64_t frame = alloc_frames(1);
    if (!frame && pagecache_reclaim(64)) frame = alloc_frames(1);
    if (!frame) return NULL;
    *physical = frame;
    return phys_to_virt(frame);
}

static void vfs_page_free(void *page, uint64_t physical)
{
    (void)page;
    free_frames(physical, 1);
}

static int64_t vfs_page_read_backend(void *context, void *buffer, uint64_t offset, size_t size)
{
    vfs_node_t node = context;
    return (int64_t)callbackof(node, read)(node->handle, buffer, (size_t)offset, size);
}

static int64_t vfs_page_write_backend(void *context, const void *buffer, uint64_t offset, size_t size)
{
    vfs_node_t node = context;
    return (int64_t)callbackof(node, write)(node->handle, buffer, (size_t)offset, size);
}

static int vfs_page_resize_backend(void *context, uint64_t size)
{
    vfs_node_t node = context;
    if (callbackof(node, resize) == vfs_empty_callback.resize) return -EOPNOTSUPP;
    return callbackof(node, resize)(node->handle, size);
}

static int vfs_page_sync_backend(void *context)
{
    vfs_node_t node = context;
    if (callbackof(node, sync) == vfs_empty_callback.sync) return EOK;
    return callbackof(node, sync)(node->handle, 0);
}

static bool vfs_pagecache_eligible(vfs_node_t node)
{
    return node && (node->type & ~file_delete) == file_none && !(node->flags & VFS_NODE_NOCACHE) && node->handle
           && callbackof(node, read) != vfs_empty_callback.read;
}

static pagecache_mapping_t *vfs_pagecache_mapping(vfs_node_t node, int create)
{
    pagecache_mapping_t *mapping = __atomic_load_n(&node->mapping, __ATOMIC_ACQUIRE);
    if (mapping || !create || !vfs_pagecache_eligible(node)) return mapping;
    pagecache_ops_t ops = {
        .read   = vfs_page_read_backend,
        .write  = callbackof(node, write) == vfs_empty_callback.write ? NULL : vfs_page_write_backend,
        .resize = callbackof(node, resize) == vfs_empty_callback.resize ? NULL : vfs_page_resize_backend,
        .sync   = vfs_page_sync_backend,
    };
    pagecache_mapping_t *new_mapping = pagecache_mapping_create(node, &ops, node->size, 0);
    if (!new_mapping) return NULL;
    mapping = NULL;
    if (!__atomic_compare_exchange_n(&node->mapping, &mapping, new_mapping, 0, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        pagecache_mapping_destroy(new_mapping);
        return mapping;
    }
    return new_mapping;
}

static void vfs_pagecache_destroy(vfs_node_t node)
{
    pagecache_mapping_t *mapping = __atomic_exchange_n(&node->mapping, NULL, __ATOMIC_ACQ_REL);
    if (mapping) pagecache_mapping_destroy(mapping);
}
#endif

int vfs_resolve_path(const char *base, const char *path, char *resolved, size_t size)
{
    size_t out = 1;

    if (!base || !path || !resolved || size < 2 || base[0] != '/') return -EINVAL;
    resolved[0] = '/';
    resolved[1] = '\0';

    const char *parts[2] = {path[0] == '/' ? "" : base, path};
    for (size_t part = 0; part < 2; part++) {
        const char *cur = parts[part];
        while (*cur) {
            while (*cur == '/') cur++;
            const char *component = cur;
            while (*cur && *cur != '/') cur++;
            size_t len = (size_t)(cur - component);

            if (!len || (len == 1 && component[0] == '.')) continue;
            if (len == 2 && component[0] == '.' && component[1] == '.') {
                if (out > 1) {
                    while (out > 1 && resolved[out - 1] != '/') out--;
                    if (out > 1) out--;
                    resolved[out] = '\0';
                }
                continue;
            }
            if (out + len + (out > 1) >= size) return -ENAMETOOLONG;
            if (out > 1) resolved[out++] = '/';
            memcpy(resolved + out, component, len);
            out += len;
            resolved[out] = '\0';
        }
    }
    return EOK;
}

int vfs_node_path(vfs_node_t node, char *path, size_t size)
{
    size_t     len = 0;
    vfs_node_t cur;

    if (!node || !path || size < 2) return -EINVAL;
    for (cur = node; cur && cur->parent; cur = cur->parent) len += strlen(cur->name) + 1;
    if (!len) len = 1;
    if (len + 1 > size) return -ENAMETOOLONG;

    path[len] = '\0';
    if (len == 1) {
        path[0] = '/';
        return EOK;
    }

    size_t pos = len;
    for (cur = node; cur && cur->parent; cur = cur->parent) {
        size_t name_len = strlen(cur->name);
        pos -= name_len;
        memcpy(path + pos, cur->name, name_len);
        path[--pos] = '/';
    }
    return EOK;
}

#ifndef VFS_PATH_TEST_ONLY

static char *vfs_node_absolute_path(vfs_node_t node)
{
    size_t     len = 0;
    vfs_node_t cur;

    if (!node) return 0;

    for (cur = node; cur && cur->parent; cur = cur->parent) len += strlen(cur->name) + 1;

    if (!len) len = 1;
    char *path = malloc(len + 1);
    if (!path) return 0;

    path[len] = '\0';
    if (len == 1) {
        path[0] = '/';
        path[1] = '\0';
        return path;
    }

    size_t pos = len;
    for (cur = node; cur && cur->parent; cur = cur->parent) {
        size_t name_len = strlen(cur->name);
        pos -= name_len;
        memcpy(path + pos, cur->name, name_len);
        path[--pos] = '/';
    }

    return path;
}

static char *vfs_resolve_link_path(vfs_node_t node)
{
    char *path;

    if (!node || !node->linkname) return 0;
    if (node->linkname[0] == '/') return normalize_path(node->linkname);

    char *base = vfs_node_absolute_path(node->parent ? node->parent : node);
    if (!base) return 0;

    size_t base_len = strlen(base);
    size_t link_len = strlen(node->linkname);
    if (base_len + link_len + 2 > VFS_PATH_MAX) {
        free(base);
        return 0;
    }
    path = malloc(base_len + link_len + 2);
    if (!path) {
        free(base);
        return 0;
    }

    memcpy(path, base, base_len);
    path[base_len] = '/';
    memcpy(path + base_len + 1, node->linkname, link_len + 1);
    free(base);

    char *normalized = normalize_path(path);
    free(path);
    return normalized;
}

static vfs_node_t vfs_open_internal(const char *str, int symlink_depth, bool follow_final);

/* Open a file or directory, invoking the appropriate callback */
static void do_open(vfs_node_t file)
{
    if (file->handle) {
        callbackof(file, stat)(file->handle, file);
    } else {
        callbackof(file, open)(file->parent->handle, file->name, file);
    }
}

/* Update a file or directory node, if necessary */
static void do_update(vfs_node_t file)
{
    if (file->type & file_none || !file->handle || file->type & file_dir || file->type & file_symlink || file->type & file_pipe) do_open(file);
    if (file->mapping) file->size = pagecache_size(file->mapping);
}

/* Add a child node to a parent directory */
static vfs_node_t vfs_child_append(vfs_node_t parent, const char *name, void *handle)
{
    vfs_node_t node = vfs_node_alloc(parent, name);
    if (!node) return 0;

    node->handle = handle;
    return node;
}

/* Find a child node by name within a parent directory */
static vfs_node_t vfs_child_find(vfs_node_t parent, const char *name)
{
    return clist_first(parent->child, data,
                       !(((vfs_node_t)data)->flags & (VFS_NODE_FINALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED))
                           && streq(name, ((vfs_node_t)data)->name));
}

/* Allocate a new vfs node with the given parent and name */
vfs_node_t vfs_node_alloc(vfs_node_t parent, const char *name)
{
    vfs_node_t node = (vfs_node_t)(malloc(sizeof(struct vfs_node)));
    if (!node) return 0;

    memset(node, 0, sizeof(struct vfs_node));
    node->parent   = parent;
    node->name     = name ? strdup(name) : 0;
    node->type     = file_none;
    node->fsid     = parent ? parent->fsid : 0;
    node->root     = parent ? parent->root : node;
    node->dev      = parent ? parent->dev : 0;
    node->refcount = 0;
    node->blksz    = PAGE_4K_SIZE;
    node->mode     = 0777;
    node->linkto   = 0;
    vfs_poll_source_init(&node->poll_source);

    if (parent) parent->child = clist_prepend(parent->child, node);
    return node;
}

/* Get the root directory node */
vfs_node_t get_rootdir(void)
{
    return rootdir;
}

/* Set the root directory node of the Virtual File System (VFS) */
void set_rootdir(vfs_node_t node)
{
    rootdir         = node;
    rootdir->parent = 0;
}

/* Search for a file or directory by name in the specified directory */
vfs_node_t vfs_do_search(vfs_node_t dir, const char *name)
{
    return clist_first(dir->child, data,
                       !(((vfs_node_t)data)->flags & (VFS_NODE_FINALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED))
                           && streq(name, ((vfs_node_t)data)->name));
}

/* Update a file or directory, ensuring it is open and ready */
void vfs_update(vfs_node_t node)
{
    do_update(node);
}

/* Open a file or directory by path */
static vfs_node_t vfs_open_internal(const char *str, int symlink_depth, bool follow_final)
{
    int  symlink_owned = 0;
    bool trailing_slash;

    if (!str || str[0] != '/') return 0;
    if (symlink_depth > 16) return 0;
    trailing_slash = str[1] != '\0' && str[strlen(str) - 1] == '/';
    if (str[1] == '\0') {
        rootdir->refcount++;
        return rootdir;
    }

    char *path = strdup(str + 1);
    if (!path) return 0;

    char      *save_ptr = path;
    vfs_node_t current  = rootdir;

    for (char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            if (current->parent) current = current->parent;
            continue;
        }

        current = vfs_child_find(current, buf);
        if (!current) goto err;

        do_update(current);
        if ((current->type & file_symlink) && (follow_final || trailing_slash || *save_ptr != '\0')) {
            char      *target_path = vfs_resolve_link_path(current);
            vfs_node_t target;

            if (!target_path) goto err;
            target = vfs_open_internal(target_path, symlink_depth + 1, true);
            free(target_path);
            if (!target) goto err;

            if (symlink_owned && current->refcount) current->refcount--;
            current       = target;
            symlink_owned = 1;
            continue;
        }
    }
    if (trailing_slash && !(current->type & file_dir)) goto err;
    if (!symlink_owned) current->refcount++;
    free(path);
    return current;
err:
    if (symlink_owned && current->refcount) current->refcount--;
    free(path);
    return 0;
}

vfs_node_t vfs_open(const char *str)
{
    spin_lock(&vfs_namespace_lock);
    vfs_node_t node = vfs_open_internal(str, 0, true);
    spin_unlock(&vfs_namespace_lock);
    return node;
}

vfs_node_t vfs_open_nofollow(const char *str)
{
    spin_lock(&vfs_namespace_lock);
    vfs_node_t node = vfs_open_internal(str, 0, false);
    spin_unlock(&vfs_namespace_lock);
    return node;
}

vfs_node_t vfs_node_retain(vfs_node_t node)
{
    if (!node) return NULL;
    spin_lock(&vfs_namespace_lock);
    if (node->flags & VFS_NODE_FINALIZING) node = NULL;
    if (node) node->refcount++;
    spin_unlock(&vfs_namespace_lock);
    return node;
}

/* Create a new directory at the specified path */
int vfs_mkdir(const char *name)
{
    if (name[0] != '/') return -EINVAL;

    char      *path     = strdup(name + 1);
    char      *save_ptr = path;
    vfs_node_t current  = rootdir;

    for (const char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        const vfs_node_t father = current;
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            if (current->parent && current->type & file_dir) {
                current = current->parent;
                goto upd;
            } else {
                goto err;
            }
        }
        current = vfs_child_find(current, buf);
upd:
        if (!current) {
            int status;
            current       = vfs_node_alloc(father, buf);
            current->type = file_dir;
            status        = callbackof(father, mkdir)(father->handle, buf, current);
            if (status != EOK) {
                father->child = clist_delete(father->child, current);
                vfs_free(current);
                free(path);
                return status;
            }
            do_update(current);
            inotify_notify_create(father, current);
        } else {
            do_update(current);
            if (!(current->type & file_dir)) goto err;
        }
    }
    free(path);
    return EOK;
err:
    free(path);
    return -ENOTDIR;
}

/* Create a new file at the specified path */
int vfs_mkfile(const char *name)
{
    if (name[0] != '/') return -EINVAL;

    char *fullpath  = strdup(name);
    char *filename  = fullpath;
    char *lastslash = strrchr(fullpath, '/');

    if (lastslash == fullpath) {
        filename   = fullpath + 1;
        *lastslash = '\0';
    } else if (lastslash) {
        *lastslash = '\0';
        filename   = lastslash + 1;
    }

    vfs_node_t parent;
    if (lastslash == fullpath) {
        parent = rootdir;
    } else {
        parent = vfs_open(fullpath);
    }
    if (!parent || !(parent->type & file_dir)) {
        if (parent && parent != rootdir) vfs_close(parent);
        free(fullpath);
        return -ENOENT;
    }

    vfs_node_t node = vfs_child_append(parent, filename, 0);
    node->type      = file_none;

    int status = callbackof(parent, mkfile)(parent->handle, filename, node);
    if (status != EOK) {
        parent->child = clist_delete(parent->child, node);
        vfs_free(node);
    } else
        inotify_notify_create(parent, node);
    if (parent != rootdir) vfs_close(parent);
    free(fullpath);
    return status;
}

/* Read a directory entry by index from the specified directory node */
int vfs_readdir(vfs_node_t dir, size_t index, vfs_dirent_t *entry)
{
    clist_t list;

    if (!dir || !entry) return -EINVAL;
    do_update(dir);
    if (!(dir->type & file_dir)) return -ENOTDIR;

    list = clist_nth(dir->child, index);
    if (!list || !list->data) return -ENOENT;

    vfs_node_t child = list->data;
    entry->name      = child->name;
    entry->type      = child->type;
    entry->size      = child->size;
    entry->inode     = child->inode;
    inotify_notify(dir, IN_ACCESS);
    return EOK;
}

/* Open a directory stream for sequential iteration */
vfs_dir_t vfs_opendir(const char *path)
{
    vfs_dir_t  dir;
    vfs_node_t node;

    if (!path) return 0;

    node = vfs_open(path);
    if (!node) return 0;
    if (!(node->type & file_dir)) {
        vfs_close(node);
        return 0;
    }

    dir = calloc(1, sizeof(*dir));
    if (!dir) {
        vfs_close(node);
        return 0;
    }

    dir->node  = node;
    dir->index = 0;
    return dir;
}

/* Read the next entry from an open directory stream */
vfs_dirent_t *vfs_readdir_next(vfs_dir_t dir)
{
    if (!dir || !dir->node) return 0;
    if (vfs_readdir(dir->node, dir->index, &dir->entry) != EOK) return 0;

    dir->index++;
    return &dir->entry;
}

/* Close an open directory stream */
int vfs_closedir(vfs_dir_t dir)
{
    int status;

    if (!dir) return -EINVAL;

    status = dir->node ? vfs_close(dir->node) : EOK;
    free(dir);
    return status;
}

/* Create a hard link at the specified path */
int vfs_link(const char *name, const char *target_name)
{
    vfs_node_t current  = rootdir;
    char      *path     = strdup(name + 1);
    char      *save_ptr = path;
    char      *filename = path + strlen(path);

    while (*--filename != '/' && filename != path);

    if (filename != path) {
        *filename++ = '\0';
    } else {
        goto create;
    }
    if (!strlen(path)) {
        free(path);
        return -EINVAL;
    }

    for (const char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            if (!current->parent || !(current->type & file_dir)) goto err;
            current = current->parent;
            continue;
        }

        vfs_node_t new_current = vfs_child_find(current, buf);
        if (!new_current) {
            new_current       = vfs_node_alloc(current, buf);
            new_current->type = file_dir;
            callbackof(current, mkdir)(current->handle, buf, new_current);
        }

        current = new_current;
        do_update(current);
        if (!(current->type & file_dir)) goto err;
    }
create:;
    vfs_node_t node = vfs_child_append(current, filename, 0);
    int        status;

    if (!node) goto err;
    node->type = file_none;
    status     = callbackof(current, link)(current->handle, target_name, node);
    if (status != EOK) {
        current->child = clist_delete(current->child, node);
        vfs_free(node);
        free(path);
        return status;
    }
    inotify_notify_create(current, node);
    free(path);
    return EOK;
err:
    free(path);
    return -EIO;
}

/* Create a symlink at the specified path */
int vfs_symlink(const char *name, const char *target_name)
{
    vfs_node_t current  = rootdir;
    char      *path     = strdup(name + 1);
    char      *save_ptr = path;
    char      *filename = path + strlen(path);

    while (*--filename != '/' && filename != path);

    if (filename != path) {
        *filename++ = '\0';
    } else {
        goto create;
    }
    if (!strlen(path)) {
        free(path);
        return -EINVAL;
    }

    for (const char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            if (!current->parent || !(current->type & file_dir)) goto err;
            current = current->parent;
            continue;
        }

        vfs_node_t new_current = vfs_child_find(current, buf);
        if (!new_current) {
            new_current       = vfs_node_alloc(current, buf);
            new_current->type = file_dir;
            callbackof(current, mkdir)(current->handle, buf, new_current);
        }

        current = new_current;
        do_update(current);
        if (!(current->type & file_dir)) goto err;
    }
create:;
    vfs_node_t node = vfs_child_append(current, filename, 0);
    int        status;

    if (!node) goto err;
    node->type     = file_symlink;
    node->linkname = strdup(target_name);
    if (!node->linkname) {
        current->child = clist_delete(current->child, node);
        vfs_free(node);
        goto err;
    }

    status = callbackof(current, symlink)(current->handle, target_name, node);
    if (status != EOK) {
        current->child = clist_delete(current->child, node);
        vfs_free(node);
        free(path);
        return status;
    }
    inotify_notify_create(current, node);
    free(path);
    return EOK;
err:
    free(path);
    return -EIO;
}

/* Register a vfs callback */
int vfs_regist(vfs_callback_t callback)
{
    return vfs_regist_fs(0, callback);
}

/* Register a vfs callback with a filesystem name */
int vfs_regist_fs(const char *name, vfs_callback_t callback)
{
    if (!callback) return -EINVAL;
    if (name) {
        for (int i = 1; i < fs_nextid; i++) {
            if (fs_names[i] && streq(fs_names[i], name)) return -EEXIST;
        }
    }

    int id = fs_nextid++;
    if (id >= 256) {
        fs_nextid--;
        return -ENOSPC;
    }

    /* Allocate and fill a copy of the callback, substituting empty_func for NULL fields */
    struct vfs_callback *cb_copy = malloc(sizeof(struct vfs_callback));
    if (!cb_copy) {
        fs_nextid--;
        return -ENOMEM;
    }

    size_t num_fields = sizeof(struct vfs_callback) / sizeof(void *);
    for (size_t i = 0; i < num_fields; i++) {
        void *func            = ((void **)callback)[i];
        ((void **)cb_copy)[i] = func ? func : ((void **)&vfs_empty_callback)[i];
    }

    fs_callbacks[id] = cb_copy;
    fs_names[id]     = name;
    return id;
}

static int vfs_mount_id(const char *src, vfs_node_t node, int fsid)
{
    uint16_t old_fsid;
    int      status;

    if (!node || !(node->type & file_dir)) return -EINVAL;
    if (fsid <= 0 || fsid >= fs_nextid || !fs_callbacks[fsid]) return -ENOENT;

    old_fsid   = node->fsid;
    node->fsid = fsid;

    status = fs_callbacks[fsid]->mount(src, node);
    if (status == EOK) {
        node->root     = node;
        node->is_mount = 1;
        return EOK;
    }

    node->fsid = old_fsid;
    return status;
}

/* Mount a file system to a directory */
int vfs_mount(const char *src, vfs_node_t node)
{
    int last_error = -ENOENT;

    if (!node || !(node->type & file_dir)) return -EINVAL;
    for (int i = 1; i < fs_nextid; i++) {
        int status = vfs_mount_id(src, node, i);
        if (status == EOK) return EOK;
        if (status != -ENOENT) last_error = status;
    }
    return last_error;
}

/* Mount a named file system to a directory */
int vfs_mount_fs(const char *fstype, const char *src, vfs_node_t node)
{
    if (!fstype || !fstype[0]) return -EINVAL;

    for (int i = 1; i < fs_nextid; i++) {
        if (!fs_names[i] || !streq(fs_names[i], fstype)) continue;
        return vfs_mount_id(src, node, i);
    }

    return -ENOENT;
}

/* Unmount a file system from a directory */
int vfs_umount(const char *path)
{
    vfs_node_t node = vfs_open(path);

    if (!node || !node->fsid) return -EINVAL;
    if (node->type != file_dir) return -ENOTDIR;
    if (node->parent) {
        vfs_node_t cur = node;
        node           = node->parent;
        if (cur->root == cur) {
            inotify_notify_unmount(cur);
            vfs_free_child(cur);
            callbackof(cur, unmount)(cur->handle);
            cur->fsid     = node->fsid;
            cur->root     = node->root;
            cur->handle   = 0;
            cur->child    = 0;
            cur->is_mount = 0;
            if (cur->fsid) do_update(cur);
            return EOK;
        }
    }
    return -ENOENT;
}

/* Read data from a file node into the provided memory buffer */
size_t vfs_read(vfs_node_t file, void *addr, size_t offset, size_t size)
{
    if (!file || !addr) return (size_t)-1;
    if (vfs_access_check(file, VFS_ACCESS_R)) return (size_t)-1;
    do_update(file);

    if (file->type & file_dir) return (size_t)-1;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    int64_t result = mapping ? pagecache_read(mapping, addr, offset, size) : (int64_t)callbackof(file, read)(file->handle, addr, offset, size);
    if (mapping) file->size = pagecache_size(mapping);
    if (result > 0) inotify_notify(file, IN_ACCESS);
    return (size_t)result;
}

/* Read data from a link file node into the provided memory buffer */
size_t vfs_readlink(vfs_node_t node, char *buf, size_t bufsize)
{
    size_t len;

    if (!node || !buf || !bufsize) return 0;
    if (node->linkname) {
        len = strlen(node->linkname);
        if (len >= bufsize) len = bufsize - 1;
        memcpy(buf, node->linkname, len);
        buf[len] = '\0';
        return len;
    }

    return callbackof(node, readlink)(node, buf, 0, bufsize);
}

/* Write data from the provided memory buffer to a file node */
size_t vfs_write(vfs_node_t file, void *addr, size_t offset, size_t size)
{
    if (!file || !addr) return (size_t)-1;
    if (vfs_access_check(file, VFS_ACCESS_W)) return (size_t)-1;
    do_update(file);

    if (file->type & file_dir) return (size_t)-1;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    int64_t ret = mapping ? pagecache_write(mapping, addr, offset, size) : (int64_t)callbackof(file, write)(file->handle, addr, offset, size);

    if (mapping)
        file->size = pagecache_size(mapping);
    else
        do_update(file);
    if (ret > 0) inotify_notify(file, IN_MODIFY);
    return (size_t)ret;
}

int64_t vfs_file_read(vfs_node_t file, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    if (!file || !addr) return -EINVAL;
    if (vfs_access_check(file, VFS_ACCESS_R)) return -EACCES;
    do_update(file);
    if (file->type & file_dir) return -EISDIR;

    int64_t result;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    if (mapping)
        result = pagecache_read(mapping, addr, offset, size);
    else if (callbackof(file, file_read) != vfs_empty_callback.file_read)
        result = callbackof(file, file_read)(file, private_data, flags, addr, offset, size);
    else {
        size_t legacy_ret = callbackof(file, read)(file->handle, addr, offset, size);
        result            = legacy_ret == (size_t)-1 ? -EIO : (int64_t)legacy_ret;
    }
    if (result > 0) inotify_notify(file, IN_ACCESS);
    return result;
}

int64_t vfs_file_write(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    int64_t ret;

    if (!file || !addr) return -EINVAL;
    if (vfs_access_check(file, VFS_ACCESS_W)) return -EACCES;
    do_update(file);
    if (file->type & file_dir) return -EISDIR;

    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    if (mapping) {
        ret        = pagecache_write(mapping, addr, offset, size);
        file->size = pagecache_size(mapping);
        if (ret >= 0 && (flags & 0x101000U)) {
            int sync_result = pagecache_writeback(mapping, offset, size ? offset + size - 1 : offset, PAGECACHE_WB_SYNC);
            if (sync_result) ret = sync_result;
        }
    } else if (callbackof(file, file_write) != vfs_empty_callback.file_write) {
        ret = callbackof(file, file_write)(file, private_data, flags, addr, offset, size);
    } else {
        size_t legacy_ret = callbackof(file, write)(file->handle, addr, offset, size);
        ret               = legacy_ret == (size_t)-1 ? -EIO : (int64_t)legacy_ret;
    }

    if (!mapping) do_update(file);
    if (ret > 0) inotify_notify(file, IN_MODIFY);
    return ret;
}

int vfs_fsync(vfs_node_t file, int data_only)
{
    if (!file) return -EINVAL;
    do_update(file);
    if (file->type & file_dir) return -EINVAL;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 0);
    if (mapping) return pagecache_writeback(mapping, 0, UINT64_MAX, PAGECACHE_WB_SYNC);
    if (callbackof(file, sync) != vfs_empty_callback.sync) return callbackof(file, sync)(file->handle, data_only);
    return EOK;
}

int vfs_writeback_range(vfs_node_t file, uint64_t start, uint64_t end, int data_only)
{
    if (!file || end < start) return -EINVAL;
    do_update(file);
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 0);
    if (mapping) return pagecache_writeback(mapping, start, end, PAGECACHE_WB_SYNC);
    if (callbackof(file, sync) != vfs_empty_callback.sync) return callbackof(file, sync)(file->handle, data_only);
    return EOK;
}

int vfs_sync_all(void)
{
    return pagecache_writeback_all(PAGECACHE_WB_SYNC);
}

int vfs_truncate(vfs_node_t file, uint64_t size)
{
    if (!file) return -EINVAL;
    do_update(file);
    if ((file->type & ~file_delete) != file_none) return file->type & file_dir ? -EISDIR : -EINVAL;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    int                  result;
    if (mapping)
        result = pagecache_truncate(mapping, size);
    else if (callbackof(file, resize) != vfs_empty_callback.resize)
        result = callbackof(file, resize)(file->handle, size);
    else
        result = -EOPNOTSUPP;
    if (!result) {
        file->size = size;
        inotify_notify(file, IN_MODIFY);
    }
    return result;
}

int vfs_invalidate_pages(vfs_node_t file, uint64_t start, uint64_t end, int discard_dirty)
{
    if (!file) return -EINVAL;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 0);
    if (!mapping) return EOK;
    return pagecache_invalidate(mapping, start, end, discard_dirty ? PAGECACHE_INVALIDATE_DISCARD_DIRTY : 0);
}

int vfs_drop_pages(vfs_node_t file, uint64_t start, uint64_t end, int writeback)
{
    if (!file || end < start) return -EINVAL;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 0);
    if (!mapping) return EOK;
    return pagecache_evict(mapping, start, end, writeback ? PAGECACHE_EVICT_WRITEBACK : 0);
}

int vfs_readahead(vfs_node_t file, uint64_t offset, size_t size)
{
    if (!file) return -EINVAL;
    do_update(file);
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    return mapping ? pagecache_readahead(mapping, offset, size) : -EOPNOTSUPP;
}

int vfs_cache_mapping_pin(vfs_node_t file)
{
    if (!file) return -EINVAL;
    pagecache_mapping_t *mapping = file->mapping;
    if (!mapping) {
        do_update(file);
        mapping = vfs_pagecache_mapping(file, 1);
    }
    if (!mapping) return -EOPNOTSUPP;
    pagecache_mapping_pin(mapping);
    return EOK;
}

void vfs_cache_mapping_unpin(vfs_node_t file)
{
    if (file && file->mapping) pagecache_mapping_unpin(file->mapping);
}

int vfs_cache_map_page(vfs_node_t file, uint64_t index, int dirty, uint64_t *physical)
{
    if (!file || !physical) return -EINVAL;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    if (!mapping) return -EOPNOTSUPP;
    pagecache_page_t *page = pagecache_get_page(mapping, index, 1);
    if (!page) return -ENOMEM;
    int result = pagecache_lock_page(page, 1);
    if (!result) {
        if (dirty) pagecache_mark_dirty(page);
        *physical = pagecache_page_physical(page);
        if (frame_retain_range(*physical, 1)) result = -ENOMEM;
        pagecache_unlock_page(page);
    }
    pagecache_put_page(page);
    return result;
}

int vfs_cache_mark_dirty_range(vfs_node_t file, uint64_t start, uint64_t end)
{
    if (!file || end < start || !file->mapping) return -EINVAL;
    uint64_t first = start / PAGECACHE_PAGE_SIZE;
    uint64_t last  = end / PAGECACHE_PAGE_SIZE;
    for (uint64_t index = first; index <= last; index++) {
        pagecache_page_t *page = pagecache_get_page(file->mapping, index, 0);
        if (page) {
            int result = pagecache_lock_page(page, 0);
            if (!result) {
                pagecache_mark_dirty(page);
                pagecache_unlock_page(page);
            }
            pagecache_put_page(page);
            if (result) return result;
        }
        if (index == UINT64_MAX) break;
    }
    inotify_notify(file, IN_MODIFY);
    return EOK;
}

int vfs_file_ioctl(vfs_node_t file, void *private_data, uint64_t flags, size_t req, void *arg)
{
    if (!file) return -EINVAL;
    do_update(file);
    if (file->type & file_dir) return -EISDIR;

    if (callbackof(file, file_ioctl) != vfs_empty_callback.file_ioctl) return callbackof(file, file_ioctl)(file, private_data, flags, req, arg);
    return callbackof(file, ioctl)(file->handle, req, arg);
}

int vfs_file_poll(vfs_node_t file, void *private_data, uint64_t flags, size_t events)
{
    if (!file) return -EINVAL;
    do_update(file);
    if (file->type & file_dir) return -EISDIR;

    if (callbackof(file, file_poll) != vfs_empty_callback.file_poll) return callbackof(file, file_poll)(file, private_data, flags, events);
    return callbackof(file, poll)(file->handle, events);
}

void vfs_poll_source_init(vfs_poll_source_t *source)
{
    if (!source) return;
    memset(source, 0, sizeof(*source));
}

void vfs_poll_source_subscribe(vfs_poll_source_t *source, vfs_poll_subscription_t *subscription, uint32_t events, vfs_poll_notify_t notify,
                               void *context)
{
    if (!source || !subscription || !notify) return;
    spin_lock(&source->lock);
    subscription->notify     = notify;
    subscription->context    = context;
    subscription->events     = events;
    subscription->next       = source->subscribers;
    subscription->subscribed = true;
    source->subscribers      = subscription;
    bool closed              = source->closed;
    spin_unlock(&source->lock);
    if (closed) notify(subscription, UINT32_MAX);
}

void vfs_poll_source_unsubscribe(vfs_poll_source_t *source, vfs_poll_subscription_t *subscription)
{
    if (!source || !subscription) return;
    spin_lock(&source->lock);
    vfs_poll_subscription_t **link = &source->subscribers;
    while (*link && *link != subscription) link = &(*link)->next;
    if (*link) *link = subscription->next;
    subscription->next       = NULL;
    subscription->subscribed = false;
    spin_unlock(&source->lock);
}

void vfs_poll_source_notify(vfs_poll_source_t *source, uint32_t events)
{
    if (!source) return;
    spin_lock(&source->lock);
    for (vfs_poll_subscription_t *sub = source->subscribers; sub; sub = sub->next) {
        uint32_t matched = events & sub->events;
        if (matched) sub->notify(sub, matched);
    }
    spin_unlock(&source->lock);
}

void vfs_poll_subscribe(vfs_node_t file, vfs_poll_subscription_t *subscription, uint32_t events, vfs_poll_notify_t notify, void *context)
{
    if (file) vfs_poll_source_subscribe(&file->poll_source, subscription, events, notify, context);
}

void vfs_poll_unsubscribe(vfs_node_t file, vfs_poll_subscription_t *subscription)
{
    if (file) vfs_poll_source_unsubscribe(&file->poll_source, subscription);
}

void vfs_poll_notify(vfs_node_t file, uint32_t events)
{
    if (file) vfs_poll_source_notify(&file->poll_source, events);
}

/* Close the file or directory node */
int vfs_close(vfs_node_t node)
{
    if (!node) return -EINVAL;

    spin_lock(&vfs_namespace_lock);

    /* Prevent double-close: if the close callback has already been
     * invoked on the non-delete path, skip it on the delete path. */
    bool already_closed = (node->flags & VFS_NODE_CLOSED) != 0;

    if (node->refcount) node->refcount--;
    bool last_ref = (node->refcount == 0);

    if (node == rootdir || !node->handle || node->type & file_proxy || (!last_ref && !already_closed)) {
        spin_unlock(&vfs_namespace_lock);
        return EOK;
    }

    if (!(node->type & file_delete)) {
        /*
         * Non-delete close: invoke the close callback once.
         * For anonymous nodes (parent == NULL) that have no filesystem
         * entry, free the handle and node now instead of leaking them.
         */
        node->flags |= VFS_NODE_CLOSED;
        spin_lock(&node->poll_source.lock);
        node->poll_source.closed = true;
        spin_unlock(&node->poll_source.lock);
        spin_unlock(&vfs_namespace_lock);
        vfs_poll_notify(node, UINT32_MAX);
        if (node->mapping) (void)pagecache_writeback(node->mapping, 0, UINT64_MAX, PAGECACHE_WB_SYNC);
        if (!node->parent) vfs_pagecache_destroy(node);
        callbackof(node, close)(node->handle);
        if (!node->parent) {
            callbackof(node, free)(node->handle);
            node->handle = 0;
            vfs_free(node);
        }
        return EOK;
    }

    node->flags |= VFS_NODE_FINALIZING;
    spin_unlock(&vfs_namespace_lock);

    if (node->type & file_dir) {
        for (clist_t child = node->child; child; child = child->next) {
            vfs_node_t vnode = child->data;
            if (vnode && !(vnode->flags & VFS_NODE_VIRTUAL)) {
                spin_lock(&vfs_namespace_lock);
                node->flags &= ~VFS_NODE_FINALIZING;
                spin_unlock(&vfs_namespace_lock);
                return -ENOTEMPTY;
            }
        }
    }

    if (node->mapping) {
        int result = pagecache_writeback(node->mapping, 0, UINT64_MAX, PAGECACHE_WB_SYNC);
        if (result) {
            spin_lock(&vfs_namespace_lock);
            node->flags &= ~VFS_NODE_FINALIZING;
            spin_unlock(&vfs_namespace_lock);
            return result;
        }
    }

    if (!(node->flags & VFS_NODE_DELETE_COMMITTED)) {
        int res = node->parent ? callbackof(node, delete)(node->parent->handle, node) : EOK;
        if (res < 0) {
            spin_lock(&vfs_namespace_lock);
            node->flags &= ~VFS_NODE_FINALIZING;
            spin_unlock(&vfs_namespace_lock);
            return res;
        }
    }

    vfs_pagecache_destroy(node);
    if (!already_closed) { callbackof(node, close)(node->handle); }
    spin_lock(&vfs_namespace_lock);
    if (!(node->flags & VFS_NODE_UNLINKED) && node->parent) {
        node->parent->child = clist_delete(node->parent->child, node);
        node->flags |= VFS_NODE_UNLINKED;
    }
    spin_unlock(&vfs_namespace_lock);
    callbackof(node, free)(node->handle);
    node->handle = 0;
    vfs_free(node);
    return EOK;
}

int vfs_namespace_unlink(vfs_node_t node)
{
    if (!node || node == rootdir) return -EINVAL;
    if (!node->parent) return -EINVAL;

    spin_lock(&vfs_namespace_lock);
    if (node->flags & (VFS_NODE_UNLINKED | VFS_NODE_UNLINKING)) {
        spin_unlock(&vfs_namespace_lock);
        return -ENOENT;
    }
    if (node->type & file_dir) {
        for (clist_t child = node->child; child; child = child->next) {
            vfs_node_t vnode = child->data;
            if (vnode && !(vnode->flags & VFS_NODE_VIRTUAL)) {
                spin_unlock(&vfs_namespace_lock);
                return -ENOTEMPTY;
            }
        }
    }

    node->flags |= VFS_NODE_UNLINKING;
    spin_unlock(&vfs_namespace_lock);

    int status = callbackof(node, delete)(node->parent->handle, node);
    if (status < 0) {
        spin_lock(&vfs_namespace_lock);
        node->flags &= ~VFS_NODE_UNLINKING;
        spin_unlock(&vfs_namespace_lock);
        return status;
    }

    if (!(node->flags & VFS_NODE_EVENT_DELETE)) {
        node->flags |= VFS_NODE_EVENT_DELETE;
        inotify_notify_delete(node);
    }

    spin_lock(&vfs_namespace_lock);
    node->parent->child = clist_delete(node->parent->child, node);
    node->flags &= ~VFS_NODE_UNLINKING;
    node->flags |= VFS_NODE_DELETE_COMMITTED | VFS_NODE_UNLINKED;
    node->type |= file_delete;
    spin_unlock(&vfs_namespace_lock);
    return EOK;
}

void vfs_namespace_detach(vfs_node_t node)
{
    if (!node || node == rootdir) return;

    while (node->child) {
        vfs_node_t child = node->child->data;
        if (!child) {
            node->child = clist_delete_node(node->child, node->child);
            continue;
        }
        vfs_namespace_detach(child);
    }

    if (!(node->flags & VFS_NODE_EVENT_DELETE)) {
        node->flags |= VFS_NODE_EVENT_DELETE;
        inotify_notify_delete(node);
    }

    spin_lock(&vfs_namespace_lock);
    if (node->flags & VFS_NODE_UNLINKED) {
        spin_unlock(&vfs_namespace_lock);
        return;
    }
    if (node->parent) node->parent->child = clist_delete(node->parent->child, node);
    node->flags |= VFS_NODE_UNLINKED | VFS_NODE_DELETE_COMMITTED;
    node->type |= file_delete;
    int release_now = node->refcount == 0;
    spin_unlock(&vfs_namespace_lock);

    if (release_now) vfs_close(node);
}

/* Delete a VFS (Virtual File System) node and clean up associated resources */
int vfs_delete(vfs_node_t node)
{
    int status;

    if (!node || node == rootdir) return -EINVAL;

    do_update(node);
    if (node->type & file_dir) {
        for (clist_t child = node->child; child; child = child->next) {
            vfs_node_t vnode = child->data;
            if (vnode && !(vnode->flags & VFS_NODE_VIRTUAL)) return -ENOTEMPTY;
        }
    }
    if ((node->flags & VFS_NODE_DELETE_SYNC) && node->parent) {
        status = callbackof(node, delete)(node->parent->handle, node);
        if (status < 0) return status;
        node->flags |= VFS_NODE_DELETE_COMMITTED;
    }
    if (!(node->flags & VFS_NODE_EVENT_DELETE)) {
        node->flags |= VFS_NODE_EVENT_DELETE;
        inotify_notify_delete(node);
    }
    node->type |= file_delete;
    if (!node->refcount) return vfs_close(node);
    return EOK;
}

/* Rename a VFS (Virtual File System) node to a new name */
int vfs_rename(vfs_node_t node, const char *new)
{
    int res;

    if (!node || !new) return -EINVAL;
    char *old      = strdup(node->name);
    char *new_name = strdup(new);
    if (!old || !new_name) {
        free(old);
        free(new_name);
        return -ENOMEM;
    }
    res = callbackof(node, rename)(node->handle, new);
    if (res != EOK) {
        free(old);
        free(new_name);
        return res;
    }

    free(node->name);
    node->name = new_name;
    if (node->parent) node->parent->visited = 0;
    inotify_notify_move(node, old, new);
    free(old);
    return EOK;
}

/* Send control commands to a device or file */
int vfs_ioctl(vfs_node_t device, size_t options, void *arg)
{
    if (!device) return -EINVAL;
    do_update(device);

    if (device->type & file_dir) return -EISDIR;
    return callbackof(device, ioctl)(device->handle, options, arg);
}

/* Listen for actionable events on one or more file descriptors */
int vfs_poll(vfs_node_t node, size_t event)
{
    do_update(node);
    if (node->type & file_dir) return -EISDIR;
    return callbackof(node, poll)(node->handle, event);
}

/* Free all child nodes of a VFS node */
void vfs_free_child(vfs_node_t vfs)
{
    if (!vfs) return;
    vfs->child = clist_free_with(vfs->child, (void (*)(void *))vfs_free);
}

/* Free the memory associated with a vfs node */
void vfs_free(vfs_node_t vfs)
{
    if (!vfs) return;

    vfs->child = clist_free_with(vfs->child, (void (*)(void *))vfs_free);
    if (vfs->linkto) {
        vfs_close(vfs->linkto);
        vfs->linkto = 0;
    }
    if (vfs->handle) {
        vfs_pagecache_destroy(vfs);
        callbackof(vfs, close)(vfs->handle);
        callbackof(vfs, free)(vfs->handle);
        vfs->handle = 0;
    }
    free(vfs->linkname);
    free(vfs->name);
    free(vfs);
}

/* Initialize the virtual file system */
void init_vfs(void)
{
    for (size_t i = 0; i < sizeof(struct vfs_callback) / sizeof(void *); i++) ((void **)&vfs_empty_callback)[i] = empty_func;
    pagecache_allocator_t allocator = {.alloc = vfs_page_alloc, .free = vfs_page_free};
    size_t                max_pages = frame_allocator.origin_frames / 2;
    if (max_pages < 256) max_pages = 256;
    (void)pagecache_init(&allocator, max_pages);
    rootdir       = vfs_node_alloc(0, "/");
    rootdir->type = file_dir;
    plogk("vfs: Initial root directory of the virtual file system: '/'\n");
}

#endif
