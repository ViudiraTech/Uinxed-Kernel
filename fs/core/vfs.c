/*
 *
 *      vfs.c
 *      Virtual file system
 *
 *      2025/11/2 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/inotify.h>
#include <fs/core/vfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <mem/pagecache.h>
#include <process/process.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <sync/spin_lock.h>

#define VFS_ACCESS_R 4
#define VFS_ACCESS_W 2

#ifndef VFS_PATH_TEST_ONLY
vfs_node_t          rootdir = 0;
static spinlock_t   vfs_namespace_guard; // guards vfs_namespace_busy only
static bool         vfs_namespace_busy;
static wait_queue_t vfs_namespace_wait;
static spinlock_t   vfs_rename_serial_lock;
static wait_queue_t vfs_rename_wait;
static bool         vfs_rename_serial_busy;
static uint64_t     vfs_next_ino      = 1;
static uint64_t     vfs_next_mount_id = 1;

/*
 * Sleepable namespace lock.  Filesystem callbacks dispatched during path
 * lookup (stat/open) may block - a FatFS f_stat takes the per-volume rt_mutex
 * and sleeps, and every block-backed lookup does real disk I/O - so the lock
 * must not be a plain spinlock (which masks IRQs; sleeping under it stalls
 * the CPU and deadlocks the single-CPU target).  The busy flag is guarded by
 * a brief spinlock and the contention wait uses the two-phase wait queue,
 * the same pattern as vfs_rename_serial_acquire().  VFS is process-context
 * only, so sleeping here is always legal.
 */
static void vfs_ns_lock(void)
{
    for (;;) {
        spin_lock(&vfs_namespace_guard);
        if (!vfs_namespace_busy) {
            vfs_namespace_busy = true;
            spin_unlock(&vfs_namespace_guard);
            return;
        }
        wait_queue_prepare(&vfs_namespace_wait);
        spin_unlock(&vfs_namespace_guard);
        wait_queue_sleep();
    }
}

/* Release the namespace lock and wake every contender. */
static void vfs_ns_unlock(void)
{
    spin_lock(&vfs_namespace_guard);
    vfs_namespace_busy = false;
    spin_unlock(&vfs_namespace_guard);
    wait_queue_wake_all(&vfs_namespace_wait);
}

/* Filesystem callbacks may sleep, so serialize rename transactions with a wait queue. */
static void vfs_rename_serial_acquire(void)
{
    spin_lock(&vfs_rename_serial_lock);
    while (vfs_rename_serial_busy) {
        wait_queue_prepare(&vfs_rename_wait);
        spin_unlock(&vfs_rename_serial_lock);
        wait_queue_sleep();
        spin_lock(&vfs_rename_serial_lock);
    }
    vfs_rename_serial_busy = true;
    spin_unlock(&vfs_rename_serial_lock);
}

/* Release the rename serialization lock. */
static void vfs_rename_serial_release(void)
{
    spin_lock(&vfs_rename_serial_lock);
    vfs_rename_serial_busy = false;
    spin_unlock(&vfs_rename_serial_lock);
    wait_queue_wake_all(&vfs_rename_wait);
}

/* Return the current realtime clock in seconds. */
static int64_t vfs_now_seconds(void)
{
    int64_t nanoseconds = timer_realtime_ns();
    return nanoseconds / (int64_t)TIMER_NSEC_PER_SEC;
}

/* Update a node's atime. */
static void vfs_touch_access(vfs_node_t node)
{
    if (node) node->readtime = vfs_now_seconds();
}

/* Update a node's mtime and ctime. */
static void vfs_touch_modify(vfs_node_t node)
{
    if (!node) return;
    int64_t now      = vfs_now_seconds();
    node->writetime  = now;
    node->createtime = now;
}

/* Update a node's ctime. */
static void vfs_touch_change(vfs_node_t node)
{
    if (node) node->createtime = vfs_now_seconds();
}

/*
 * Check file access permissions against the current process.
 * Returns 0 if access is granted, -EACCES otherwise.
 * Kernel-internal calls (no process context) and root (uid==0) bypass checks.
 */
int vfs_access_check_process(vfs_node_t node, uint32_t access_mask, process_t *proc)
{
    if (!node) return -EACCES;
    if (!proc) return 0;
    if (proc->fsuid == 0) {
        if ((access_mask & VFS_ACCESS_X) && !(node->type & file_dir) && !(node->mode & 0111)) return -EACCES;
        return 0;
    }
    if (proc->fsuid == node->owner && (node->mode & (access_mask << 6)) == (access_mask << 6)) return 0;
    if (process_in_group(proc, node->group) && (node->mode & (access_mask << 3)) == (access_mask << 3)) return 0;
    if ((node->mode & access_mask) == access_mask) return 0;
    return -EACCES;
}

int vfs_access_check(vfs_node_t node, uint32_t access_mask)
{
    return vfs_access_check_process(node, access_mask, process_current());
}

/* Change a node's permission bits on behalf of a specific process. */
int vfs_chmod_process(vfs_node_t node, uint16_t mode, process_t *proc)
{
    if (!node || !proc) return -EINVAL;
    if (vfs_mount_is_readonly(node)) return -EROFS;
    if (proc->fsuid != 0 && proc->fsuid != node->owner) return -EPERM;
    mode &= 07777;
    if (proc->fsuid != 0 && !process_in_group(proc, node->group)) mode &= (uint16_t)~02000;
    if (callbackof(node, chmod) != vfs_empty_callback.chmod) {
        int result = callbackof(node, chmod)(node, mode);
        if (result != EOK) return result;
    }
    node->mode        = mode;
    node->permissions = mode;
    vfs_touch_change(node);
    inotify_notify(node, IN_ATTRIB);
    return EOK;
}

/* Change ownership, treating UINT32_MAX as "leave unchanged" value. */
int vfs_chown_process(vfs_node_t node, uint32_t owner, uint32_t group, process_t *proc)
{
    if (!node || !proc) return -EINVAL;
    if (vfs_mount_is_readonly(node)) return -EROFS;

    bool change_owner = owner != UINT32_MAX;
    bool change_group = group != UINT32_MAX;
    if (proc->fsuid != 0) {
        if (proc->fsuid != node->owner) return -EPERM;
        if (change_owner && owner != node->owner) return -EPERM;
        if (change_group && !process_in_group(proc, group)) return -EPERM;
    }

    if (change_owner) node->owner = owner;
    if (change_group) node->group = group;
    if ((change_owner || change_group) && !(node->type & file_dir)) {
        node->mode &= (uint16_t)~06000;
        node->permissions = node->mode;
    }
    vfs_touch_change(node);
    inotify_notify(node, IN_ATTRIB);
    return EOK;
}

/* Change file timestamps on behalf of a process. */
int vfs_set_times_process(vfs_node_t node, int64_t atime, int64_t mtime, uint32_t flags, process_t *proc)
{
    uint32_t which = flags & (VFS_SET_TIME_ATIME | VFS_SET_TIME_MTIME);
    if (!node || !proc) return -EINVAL;
    if (!which) return EOK;
    if (vfs_mount_is_readonly(node)) return -EROFS;

    if (proc->fsuid != 0 && proc->fsuid != node->owner) {
        if (flags & VFS_SET_TIME_EXPLICIT) return -EPERM;
        if (vfs_access_check_process(node, VFS_ACCESS_W, proc) != EOK) return -EACCES;
    }

    if (which & VFS_SET_TIME_ATIME) node->readtime = atime;
    if (which & VFS_SET_TIME_MTIME) node->writetime = mtime;
    vfs_touch_change(node);
    inotify_notify(node, IN_ATTRIB);
    return EOK;
}

struct vfs_callback vfs_empty_callback;
vfs_callback_t      fs_callbacks[256] = {[0] = &vfs_empty_callback};
static const char  *fs_names[256];
static uint32_t     fs_flags[256];
static int          fs_nextid = 1;

/* Default callback for filesystem slots with no registered operations */
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

/* Allocate a pagecache frame, reclaiming from the cache when the reserve dips. */
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

/* Release a pagecache frame. */
static void vfs_page_free(void *page, uint64_t physical)
{
    (void)page;
    free_frames(physical, 1);
}

/* Pagecache read callback forwarding to the filesystem. */
static int64_t vfs_page_read_backend(void *context, void *buffer, uint64_t offset, size_t size)
{
    vfs_node_t node = context;
    return (int64_t)callbackof(node, read)(node->handle, buffer, (size_t)offset, size);
}

/* Pagecache write callback forwarding to the filesystem. */
static int64_t vfs_page_write_backend(void *context, const void *buffer, uint64_t offset, size_t size)
{
    vfs_node_t node = context;
    return (int64_t)callbackof(node, write)(node->handle, buffer, (size_t)offset, size);
}

/* Pagecache resize callback forwarding to the filesystem. */
static int vfs_page_resize_backend(void *context, uint64_t size)
{
    vfs_node_t node = context;
    if (callbackof(node, resize) == vfs_empty_callback.resize) return -EOPNOTSUPP;
    return callbackof(node, resize)(node->handle, size);
}

/* Pagecache sync callback forwarding to the filesystem. */
static int vfs_page_sync_backend(void *context)
{
    vfs_node_t node = context;
    if (callbackof(node, sync) == vfs_empty_callback.sync) return EOK;
    return callbackof(node, sync)(node->handle, 0);
}

/* Whether the node can be served through the page cache. */
static bool vfs_pagecache_eligible(vfs_node_t node)
{
    return node && (node->type & ~file_delete) == file_none && !(node->flags & VFS_NODE_NOCACHE) && node->handle && callbackof(node, read) != vfs_empty_callback.read;
}

/* Look up a node's cache mapping, creating it on demand. */
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

/* Drop a node's cache mapping. */
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
    /*
     * Linux pathname-taking syscalls reject an empty pathname unless the
     * individual syscall explicitly implements AT_EMPTY_PATH.  Treating it as
     * the base directory made open("") and mkdir("") operate on cwd.
     */
    if (!path[0]) return -ENOENT;
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

/* Reconstruct the absolute path of a node into the caller's buffer. */
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

/* Build the absolute path of a node into a malloc'd buffer. */
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

/* Resolve a symlink node to its absolute target path. */
static char *vfs_resolve_link_path(vfs_node_t node)
{
    char       *path;
    process_t  *proc;
    char        dynamic_target[VFS_PATH_MAX];
    const char *linkname;

    if (!node) return 0;
    linkname = node->linkname;
    if (!linkname && node->fsid && callbackof(node, readlink) != vfs_empty_callback.readlink) {
        size_t length = callbackof(node, readlink)(node, dynamic_target, 0, sizeof(dynamic_target) - 1);
        if (!length || length >= sizeof(dynamic_target)) return 0;
        dynamic_target[length] = '\0';
        linkname               = dynamic_target;
    }
    if (!linkname) return 0;
    proc = process_current();
    if (linkname[0] == '/') {
        char resolved[VFS_PATH_MAX];
        if (proc && proc->root[0]) {
            if (process_resolve_path_at(proc, PROCESS_AT_FDCWD, linkname, resolved, sizeof(resolved)) != EOK) return 0;
            return strdup(resolved);
        }
        return normalize_path(linkname);
    }

    char *base = vfs_node_absolute_path(node->parent ? node->parent : node);
    if (!base) return 0;

    size_t base_len = strlen(base);
    size_t link_len = strlen(linkname);
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
    memcpy(path + base_len + 1, linkname, link_len + 1);
    free(base);

    char *normalized = normalize_path(path);
    free(path);
    if (normalized && proc && proc->root[0]) {
        size_t root_len = strlen(proc->root);
        if (root_len != 1 && (strncmp(normalized, proc->root, root_len) != 0 || (normalized[root_len] && normalized[root_len] != '/'))) {
            free(normalized);
            return 0;
        }
    }
    return normalized;
}

static vfs_node_t vfs_open_internal(const char *str, int symlink_depth, bool follow_final, int *error);

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
                       !(((vfs_node_t)data)->flags & (VFS_NODE_FINALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_INITIALIZING)) && !(((vfs_node_t)data)->type & file_delete)
                           && streq(name, ((vfs_node_t)data)->name));
}

/*
 * Creation must also see entries which are not published yet.  Otherwise two
 * concurrent creators can both pass lookup and hand duplicate names to the
 * backing filesystem.  Deleted entries are deliberately ignored: POSIX
 * permits a name to be reused while an unlinked inode is still open.
 */
static vfs_node_t vfs_child_find_reserved(vfs_node_t parent, const char *name)
{
    return clist_first(parent->child, data, !(((vfs_node_t)data)->flags & VFS_NODE_UNLINKED) && !(((vfs_node_t)data)->type & file_delete) && streq(name, ((vfs_node_t)data)->name));
}

/* Check whether the directory still has children that can be seen. */
static bool vfs_directory_has_visible_children(vfs_node_t node)
{
    for (clist_t child = node ? node->child : NULL; child; child = child->next) {
        vfs_node_t vnode = child->data;
        if (!vnode || (vnode->flags & VFS_NODE_VIRTUAL)) continue;
        if (vnode->type & file_delete) continue;
        if (vnode->flags & (VFS_NODE_UNLINKED | VFS_NODE_FINALIZING)) continue;
        return true;
    }
    return false;
}

/* Allocate a new vfs node with the given parent and name */
vfs_node_t vfs_node_alloc(vfs_node_t parent, const char *name)
{
    vfs_node_t node = (vfs_node_t)(malloc(sizeof(struct vfs_node)));
    if (!node) return 0;

    memset(node, 0, sizeof(struct vfs_node));
    node->parent = parent;
    node->name   = name ? strdup(name) : 0;
    if (name && !node->name) {
        free(node);
        return 0;
    }
    node->type = file_none;
    node->fsid = parent ? parent->fsid : 0;
    node->root = parent ? parent->root : node;
    node->dev  = parent ? parent->dev : 0;
    /*
     * Virtual filesystems need real inode identity too.  In particular,
     * dynamic linkers use (st_dev, st_ino) to decide whether a shared object
     * is already loaded.  Zero for every tmpfs node aliases unrelated files.
     */
    node->inode = __atomic_fetch_add(&vfs_next_ino, 1, __ATOMIC_RELAXED);
    if (!node->inode) node->inode = __atomic_fetch_add(&vfs_next_ino, 1, __ATOMIC_RELAXED);
    node->nlink      = 1;
    node->refcount   = 0;
    node->blksz      = PAGE_4K_SIZE;
    node->mode       = 0777;
    node->linkto     = 0;
    node->createtime = node->readtime = node->writetime = vfs_now_seconds();
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
                       !(((vfs_node_t)data)->flags & (VFS_NODE_FINALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_INITIALIZING)) && !(((vfs_node_t)data)->type & file_delete)
                           && streq(name, ((vfs_node_t)data)->name));
}

/* Update a file or directory, ensuring it is open and ready */
void vfs_update(vfs_node_t node)
{
    if (!node) return;
    vfs_ns_lock();
    do_update(node);
    vfs_ns_unlock();
}

/* Open a file or directory by path */
static vfs_node_t vfs_open_internal(const char *str, int symlink_depth, bool follow_final, int *error)
{
    vfs_node_t owned_reference = NULL;
    bool       trailing_slash;

    if (error) *error = -ENOENT;
    if (!str || str[0] != '/') {
        if (error) *error = -EINVAL;
        return 0;
    }
    if (symlink_depth > 40) {
        plogk("vfs: Symlink depth exceeded while resolving %s\n", str);
        if (error) *error = -ELOOP;
        return 0;
    }
    trailing_slash = str[1] != '\0' && str[strlen(str) - 1] == '/';
    if (str[1] == '\0') {
        rootdir->refcount++;
        if (error) *error = EOK;
        return rootdir;
    }

    char *path = strdup(str + 1);
    if (!path) {
        plogk("vfs: Path allocation failed while resolving %s\n", str);
        if (error) *error = -ENOMEM;
        return 0;
    }

    char      *save_ptr = path;
    vfs_node_t current  = rootdir;

    for (char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (!(current->type & file_dir)) {
            if (error) *error = -ENOTDIR;
            goto err;
        }
        if (vfs_access_check(current, VFS_ACCESS_X) != EOK) {
            if (error) *error = -EACCES;
            goto err;
        }
        if (streq(buf, ".")) continue;
        if (streq(buf, "..")) {
            vfs_node_t next = current->parent ? current->parent : current;
            if (owned_reference && owned_reference != next && owned_reference->refcount) owned_reference->refcount--;
            owned_reference = owned_reference == next ? owned_reference : NULL;
            current         = next;
            continue;
        }

        vfs_node_t next = vfs_child_find(current, buf);
        if (!next) {
            if (error) *error = -ENOENT;
            goto err;
        }
        if (owned_reference && owned_reference->refcount) owned_reference->refcount--;
        owned_reference = NULL;
        current         = next;

        do_update(current);
        if ((current->type & file_symlink) && (follow_final || trailing_slash || *save_ptr != '\0')) {
            char      *target_path = vfs_resolve_link_path(current);
            vfs_node_t target;

            if (!target_path) goto err;
            target = vfs_open_internal(target_path, symlink_depth + 1, true, error);
            free(target_path);
            if (!target) goto err;

            current         = target;
            owned_reference = target;
            continue;
        }
    }
    if (trailing_slash && !(current->type & file_dir)) {
        if (error) *error = -ENOTDIR;
        goto err;
    }
    if (!owned_reference) current->refcount++;
    free(path);
    if (error) *error = EOK;
    return current;
err:
    if (owned_reference && owned_reference->refcount) owned_reference->refcount--;
    free(path);
    return 0;
}

/* Open a file or directory by path. */
vfs_node_t vfs_open(const char *str)
{
    vfs_ns_lock();
    vfs_node_t node = vfs_open_internal(str, 0, true, NULL);
    vfs_ns_unlock();
    return node;
}

/* Open a file or directory by path, reporting lookup errors. */
vfs_node_t vfs_open_checked(const char *str, int *error)
{
    vfs_ns_lock();
    vfs_node_t node = vfs_open_internal(str, 0, true, error);
    vfs_ns_unlock();
    return node;
}

/* Open a path without following the final symlink component. */
vfs_node_t vfs_open_nofollow(const char *str)
{
    vfs_ns_lock();
    vfs_node_t node = vfs_open_internal(str, 0, false, NULL);
    vfs_ns_unlock();
    return node;
}

/* Open a path without following the final symlink, reporting errors. */
vfs_node_t vfs_open_nofollow_checked(const char *str, int *error)
{
    vfs_ns_lock();
    vfs_node_t node = vfs_open_internal(str, 0, false, error);
    vfs_ns_unlock();
    return node;
}

/* Retain a node reference, refusing nodes already finalizing. */
vfs_node_t vfs_node_retain(vfs_node_t node)
{
    if (!node) return NULL;
    vfs_ns_lock();
    if (node->flags & VFS_NODE_FINALIZING) node = NULL;
    if (node) node->refcount++;
    vfs_ns_unlock();
    return node;
}

/*
 * Resolve the existing parent of a creation pathname.  Creation is deliberately
 * non-recursive: Linux mkdir/open/link/symlink never manufacture missing parent
 * directories as a side effect.
 */
static int vfs_prepare_create(const char *name, bool allow_trailing_slash, char **storage, char **leaf, vfs_node_t *parent)
{
    if (!name || !storage || !leaf || !parent) return -EINVAL;
    if (!name[0]) return -ENOENT;
    if (name[0] != '/') return -EINVAL;

    size_t length = strlen(name);
    if (length >= VFS_PATH_MAX) return -ENAMETOOLONG;
    if (!allow_trailing_slash && length > 1 && name[length - 1] == '/') return -ENOENT;

    char *path = strdup(name);
    if (!path) return -ENOMEM;
    if (allow_trailing_slash)
        while (length > 1 && path[length - 1] == '/') path[--length] = '\0';
    if (length == 1) {
        free(path);
        return -EEXIST;
    }

    char *last_slash = strrchr(path, '/');
    if (!last_slash || !last_slash[1]) {
        free(path);
        return -ENOENT;
    }
    *leaf = last_slash + 1;
    if (streq(*leaf, ".") || streq(*leaf, "..")) {
        free(path);
        return -EEXIST;
    }
    if (strlen(*leaf) > VFS_NAME_MAX) {
        free(path);
        return -ENAMETOOLONG;
    }

    const char *parent_path;
    if (last_slash == path) {
        parent_path = "/";
    } else {
        *last_slash = '\0';
        parent_path = path;
    }

    vfs_node_t dir = vfs_open(parent_path);
    if (!dir) {
        free(path);
        return -ENOENT;
    }
    if (!(dir->type & file_dir)) {
        vfs_close(dir);
        free(path);
        return -ENOTDIR;
    }
    if (vfs_access_check(dir, VFS_ACCESS_W | VFS_ACCESS_X) != EOK) {
        vfs_close(dir);
        free(path);
        return -EACCES;
    }

    *storage = path;
    *parent  = dir;
    return EOK;
}

/* Remove an unpublished node after a failed creation. */
static void vfs_abort_created_node(vfs_node_t parent, vfs_node_t node)
{
    if (!parent || !node) return;
    vfs_ns_lock();
    parent->child = clist_delete(parent->child, node);
    node->flags |= VFS_NODE_UNLINKED;
    vfs_ns_unlock();
    vfs_free(node);
}

/* Reserve a child name to serialize concurrent creators. */
static vfs_node_t vfs_reserve_child(vfs_node_t parent, const char *name, int *status)
{
    if (status) *status = -ENOMEM;
    vfs_ns_lock();
    if (parent->flags & VFS_NODE_RENAME_BUSY) {
        vfs_ns_unlock();
        if (status) *status = -EBUSY;
        return NULL;
    }
    if (vfs_child_find_reserved(parent, name)) {
        vfs_ns_unlock();
        if (status) *status = -EEXIST;
        return NULL;
    }
    vfs_node_t node = vfs_child_append(parent, name, NULL);
    if (node) node->flags |= VFS_NODE_INITIALIZING;
    vfs_ns_unlock();
    if (node && status) *status = EOK;
    return node;
}

/* Make a reserved child visible to concurrent lookups. */
static void vfs_publish_child(vfs_node_t node)
{
    vfs_ns_lock();
    node->flags &= ~VFS_NODE_INITIALIZING;
    vfs_ns_unlock();
}

/* Create exactly one new directory, matching mkdir(2) rather than mkdir -p. */
int vfs_mkdir_mode(const char *name, uint16_t mode)
{
    char      *path;
    char      *filename;
    vfs_node_t parent;
    int        status = vfs_prepare_create(name, true, &path, &filename, &parent);
    if (status != EOK) return status;

    vfs_node_t node = vfs_reserve_child(parent, filename, &status);
    if (!node) goto out;
    node->type        = file_dir;
    node->mode        = mode & 07777;
    node->permissions = node->mode;
    process_t *proc   = process_current();
    if (proc) {
        node->owner = proc->fsuid;
        node->group = proc->fsgid;
    }
    status = callbackof(parent, mkdir)(parent->handle, filename, node);
    if (status != EOK) {
        plogk("vfs: Mkdir %s failed (%d)\n", name, status);
        vfs_abort_created_node(parent, node);
    } else {
        do_update(node);
        vfs_touch_modify(parent);
        vfs_publish_child(node);
        inotify_notify_create(parent, node);
    }
out:
    vfs_close(parent);
    free(path);
    return status;
}

int vfs_mkdir(const char *name)
{
    return vfs_mkdir_mode(name, 0777);
}

/* Create a new regular file without replacing an existing namespace entry. */
int vfs_mkfile_mode(const char *name, uint16_t mode)
{
    char      *path;
    char      *filename;
    vfs_node_t parent;
    int        status = vfs_prepare_create(name, false, &path, &filename, &parent);
    if (status != EOK) return status;

    vfs_node_t node = vfs_reserve_child(parent, filename, &status);
    if (!node) goto out;
    node->type        = file_none;
    node->mode        = mode & 07777;
    node->permissions = node->mode;
    process_t *proc   = process_current();
    if (proc) {
        node->owner = proc->fsuid;
        node->group = proc->fsgid;
    }
    status = callbackof(parent, mkfile)(parent->handle, filename, node);
    if (status != EOK) {
        plogk("vfs: Mkfile %s failed (%d)\n", name, status);
        vfs_abort_created_node(parent, node);
    } else {
        vfs_touch_modify(parent);
        vfs_publish_child(node);
        inotify_notify_create(parent, node);
    }
out:
    vfs_close(parent);
    free(path);
    return status;
}

int vfs_mkfile(const char *name)
{
    return vfs_mkfile_mode(name, 0666);
}

/* Read a directory entry by index from the specified directory node */
int vfs_readdir(vfs_node_t dir, size_t index, vfs_dirent_t *entry)
{
    if (!dir || !entry) return -EINVAL;
    vfs_ns_lock();

    /*
     * A pathname open already refreshes the directory.  Refresh once again
     * at the start of an enumeration to pick up dynamic procfs/sysfs entries,
     * but never once per returned entry: sysfs_stat() walks and de-duplicates
     * all children, so doing that from every getdents64 loop iteration turns
     * udev's parallel tree walk into an O(entries^2) global-spinlock storm.
     */
    if (index == 0) do_update(dir);
    if (!(dir->type & file_dir)) {
        vfs_ns_unlock();
        return -ENOTDIR;
    }

    vfs_node_t child   = NULL;
    size_t     visible = 0;
    for (clist_t list = dir->child; list; list = list->next) {
        vfs_node_t candidate = list->data;
        if (!candidate || (candidate->flags & (VFS_NODE_FINALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_INITIALIZING)) || (candidate->type & file_delete)) continue;
        if (visible++ == index) {
            child = candidate;
            break;
        }
    }
    if (!child) {
        vfs_ns_unlock();
        return -ENOENT;
    }

    size_t name_length = strlen(child->name);
    if (name_length > VFS_NAME_MAX) {
        vfs_ns_unlock();
        return -ENAMETOOLONG;
    }
    memcpy(entry->name, child->name, name_length + 1);
    entry->type  = child->type;
    entry->size  = child->size;
    entry->inode = child->inode;
    vfs_ns_unlock();
    if (index == 0) vfs_touch_access(dir);
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

/* Create a hard link, optionally following the final symlink component. */
static int vfs_link_internal(const char *name, const char *target_name, bool follow)
{
    if (!target_name || !target_name[0]) return -ENOENT;
    vfs_node_t target = follow ? vfs_open(target_name) : vfs_open_nofollow(target_name);
    if (!target) return -ENOENT;
    if (target->type & file_dir) {
        vfs_close(target);
        return -EPERM;
    }

    char      *path;
    char      *filename;
    vfs_node_t parent;
    int        status = vfs_prepare_create(name, false, &path, &filename, &parent);
    if (status != EOK) {
        vfs_close(target);
        return status;
    }
    if (parent->fsid != target->fsid) {
        status = -EXDEV;
        goto out_link;
    }
    vfs_node_t node = vfs_reserve_child(parent, filename, &status);
    if (!node) goto out_link;
    const char *callback_target = target_name;
    char        resolved_target[VFS_PATH_MAX];
    if (follow) {
        status = vfs_node_path(target, resolved_target, sizeof(resolved_target));
        if (status != EOK) {
            vfs_abort_created_node(parent, node);
            goto out_link;
        }
        callback_target = resolved_target;
    }
    node->type = file_none;
    status     = callbackof(parent, link)(parent->handle, callback_target, node);
    if (status != EOK) {
        vfs_abort_created_node(parent, node);
    } else {
        vfs_touch_change(target);
        vfs_touch_modify(parent);
        vfs_publish_child(node);
        inotify_notify_create(parent, node);
    }
out_link:
    vfs_close(parent);
    vfs_close(target);
    free(path);
    return status;
}

/* link(2) does not dereference the final component of the old path. */
int vfs_link(const char *name, const char *target_name)
{
    return vfs_link_internal(name, target_name, false);
}

/* linkat(2) with AT_SYMLINK_FOLLOW dereferences the old path. */
int vfs_link_follow(const char *name, const char *target_name)
{
    return vfs_link_internal(name, target_name, true);
}

/* Create a symlink at the specified path */
int vfs_symlink(const char *name, const char *target_name)
{
    if (!target_name || !target_name[0]) return -ENOENT;
    char      *path;
    char      *filename;
    vfs_node_t parent;
    int        status = vfs_prepare_create(name, false, &path, &filename, &parent);
    if (status != EOK) return status;
    vfs_node_t node = vfs_reserve_child(parent, filename, &status);
    if (!node) goto out;
    node->type        = file_symlink;
    node->mode        = 0777;
    node->permissions = 0777;
    process_t *proc   = process_current();
    if (proc) {
        node->owner = proc->fsuid;
        node->group = proc->fsgid;
    }
    node->linkname = strdup(target_name);
    if (!node->linkname) {
        vfs_abort_created_node(parent, node);
        status = -ENOMEM;
        goto out;
    }

    status = callbackof(parent, symlink)(parent->handle, target_name, node);
    if (status != EOK) {
        vfs_abort_created_node(parent, node);
    } else {
        vfs_touch_modify(parent);
        vfs_publish_child(node);
        inotify_notify_create(parent, node);
    }
out:
    vfs_close(parent);
    free(path);
    return status;
}

/* Register a vfs callback */
int vfs_regist(vfs_callback_t callback)
{
    return vfs_regist_fs(0, callback);
}

/* Register a vfs callback with a filesystem name */
int vfs_regist_fs(const char *name, vfs_callback_t callback)
{
    return vfs_regist_fs_flags(name, callback, 0);
}

/* Register a filesystem callback, filling NULL fields with empty_func. */
int vfs_regist_fs_flags(const char *name, vfs_callback_t callback, uint32_t flags)
{
    if (!callback) return -EINVAL;
    if (name) {
        for (int i = 1; i < fs_nextid; i++)
            if (fs_names[i] && streq(fs_names[i], name)) return -EEXIST;
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
    fs_flags[id]     = flags;
    return id;
}

/* Format the registered filesystem table for /proc/filesystems. */
size_t vfs_format_filesystems(char *buffer, size_t capacity)
{
    if (!buffer || !capacity) return 0;

    size_t used = 0;
    buffer[0]   = '\0';
    for (int i = 1; i < fs_nextid; i++) {
        if (!fs_names[i] || !fs_names[i][0] || fs_callbacks[i]->mount == vfs_empty_callback.mount) continue;
        const char *prefix = (fs_flags[i] & VFS_FS_NODEV) ? "nodev\t" : "\t";
        int         length = snprintf(used < capacity ? buffer + used : buffer + capacity - 1, used < capacity ? capacity - used : 0, "%s%s\n", prefix, fs_names[i]);
        if (length > 0) used += (size_t)length;
    }
    if (used >= capacity) {
        buffer[capacity - 1] = '\0';
        return capacity - 1;
    }
    return used;
}

/* Return the name registered for a filesystem id. */
const char *vfs_filesystem_name(uint16_t fsid)
{
    if (!fsid || fsid >= (uint16_t)fs_nextid) return NULL;
    return fs_names[fsid];
}

/* Mount one named filesystem type onto a directory node. */
static int vfs_mount_id(const char *src, vfs_node_t node, int fsid)
{
    uint16_t old_fsid;
    int      status;

    if (!node || !(node->type & file_dir)) return -EINVAL;
    if (fsid <= 0 || fsid >= fs_nextid || !fs_callbacks[fsid]) return -ENOENT;

    vfs_ns_lock();
    if (node->is_mount) {
        /*
         * OpenRC may discover and mount a nodev filesystem before localmount
         * processes the same fstab entry.  This VFS has no mount stacking;
         * treat an exact same-filesystem mount as an idempotent success.
         */
        bool same_filesystem = node->fsid == (uint16_t)fsid;
        vfs_ns_unlock();
        return same_filesystem ? EOK : -EBUSY;
    }
    if (node->flags & (VFS_NODE_INITIALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) {
        vfs_ns_unlock();
        return -EBUSY;
    }
    node->flags |= VFS_NODE_INITIALIZING;
    vfs_ns_unlock();

    const char *display_source = src && src[0] ? src : fs_names[fsid] ? fs_names[fsid] : "none";
    char       *source_copy    = strdup(display_source);
    if (!source_copy) {
        vfs_ns_lock();
        node->flags &= ~VFS_NODE_INITIALIZING;
        vfs_ns_unlock();
        return -ENOMEM;
    }

    old_fsid   = node->fsid;
    node->fsid = fsid;

    status = fs_callbacks[fsid]->mount(src, node);
    if (status == EOK) {
        free(node->mount_source);
        node->mount_source = source_copy;
        node->mount_id     = __atomic_fetch_add(&vfs_next_mount_id, 1, __ATOMIC_RELAXED);
        if (!node->mount_id) node->mount_id = __atomic_fetch_add(&vfs_next_mount_id, 1, __ATOMIC_RELAXED);
        node->root     = node;
        node->is_mount = 1;
        vfs_ns_lock();
        node->flags &= ~VFS_NODE_INITIALIZING;
        vfs_ns_unlock();
        return EOK;
    }

    free(source_copy);
    node->fsid = old_fsid;
    vfs_ns_lock();
    node->flags &= ~VFS_NODE_INITIALIZING;
    vfs_ns_unlock();
    return status;
}

/* Mount a file system to a directory */
int vfs_mount(const char *src, vfs_node_t node)
{
    int last_error = -ENOENT;

    if (!node || !(node->type & file_dir)) return -EINVAL;
    for (int i = 1; i < fs_nextid; i++) {
        /*
         * Anonymous VFS types are implementation details (pipe, socket,
         * eventfd, ...), not filesystem probes.  A missing mount callback is
         * likewise represented by empty_func and must not hide a disk
         * filesystem's diagnostic with -ENOSYS.
         */
        if (!fs_names[i] || fs_callbacks[i]->mount == vfs_empty_callback.mount) continue;
        int status = vfs_mount_id(src, node, i);
        if (status == EOK) return EOK;
        if (status != -ENOENT) last_error = status;
    }
    {
        char path[VFS_PATH_MAX];
        if (vfs_node_path(node, path, sizeof(path)) != EOK) strcpy(path, "?");
        plogk("vfs: Mount of %s on %s failed: %d\n", src ? src : "(null)", path, last_error);
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

    plogk("vfs: Unknown filesystem type '%s'\n", fstype);
    return -ENOENT;
}

/* Unmount a file system from a directory */
/* Check whether the mount tree still holds references or nested mounts. */
static bool vfs_mount_tree_busy_locked(vfs_node_t node, vfs_node_t mount_root)
{
    if (!node) return false;
    if (node != mount_root && node->is_mount) return true;
    uint32_t allowed = node == mount_root ? 1 : 0; // vfs_umount's lookup
    if (node->refcount > allowed) return true;
    for (clist_t link = node->child; link; link = link->next)
        if (link->data && vfs_mount_tree_busy_locked(link->data, mount_root)) return true;
    return false;
}

/* Unmount the filesystem mounted at path. */
int vfs_umount(const char *path)
{
    vfs_node_t node = vfs_open(path);

    if (!node) return -EINVAL;
    if (!node->fsid) {
        vfs_close(node);
        return -EINVAL;
    }
    if (!(node->type & file_dir)) {
        vfs_close(node);
        return -ENOTDIR;
    }
    if (!node->parent || node->root != node || !node->is_mount) {
        vfs_close(node);
        return -ENOENT;
    }

    vfs_ns_lock();
    if (vfs_mount_tree_busy_locked(node, node)) {
        vfs_ns_unlock();
        vfs_close(node);
        return -EBUSY;
    }
    node->flags |= VFS_NODE_INITIALIZING;
    vfs_ns_unlock();

    vfs_node_t parent = node->parent;
    inotify_notify_unmount(node);
    vfs_free_child(node);
    callbackof(node, unmount)(node->handle);
    free(node->mount_source);
    node->mount_source = NULL;
    node->mount_id     = 0;
    node->fsid         = parent->fsid;
    node->root         = parent->root;
    node->handle       = 0;
    node->child        = 0;
    node->is_mount     = 0;
    if (node->fsid) do_update(node);
    vfs_ns_lock();
    node->flags &= ~VFS_NODE_INITIALIZING;
    vfs_ns_unlock();
    vfs_close(node);
    return EOK;
}

typedef struct vfs_mount_format_scratch {
        char path[VFS_PATH_MAX];
        char escaped_path[VFS_PATH_MAX * 4];
        char escaped_source[VFS_PATH_MAX * 4];
        char options[64];
} vfs_mount_format_scratch_t;

static size_t vfs_mount_escape(char *output, size_t capacity, const char *input)
{
    size_t used = 0;
    if (!input) input = "none";
    for (size_t i = 0; input[i]; i++) {
        const char *escape = NULL;
        switch (input[i]) {
            case ' ' :
                escape = "\\040";
                break;
            case '\t' :
                escape = "\\011";
                break;
            case '\n' :
                escape = "\\012";
                break;
            case '\\' :
                escape = "\\134";
                break;
            default :
                break;
        }
        if (escape) {
            for (size_t j = 0; escape[j]; j++) {
                if (used + 1 < capacity) output[used] = escape[j];
                used++;
            }
        } else {
            if (used + 1 < capacity) output[used] = input[i];
            used++;
        }
    }
    if (capacity) output[used < capacity ? used : capacity - 1] = '\0';
    return used;
}

/* Format the mount flag options into the output buffer. */
static size_t vfs_mount_options(char *output, size_t capacity, const vfs_node_t node)
{
    size_t used = 0;
#    define APPEND_OPTION(_text)                                     \
        do {                                                         \
            const char *_option = (_text);                           \
            for (size_t _i = 0; _option[_i]; _i++) {                 \
                if (used + 1 < capacity) output[used] = _option[_i]; \
                used++;                                              \
            }                                                        \
        } while (0)
    APPEND_OPTION((node->flags & MOUNT_FLAG_RDONLY) ? "ro" : "rw");
    if (node->flags & MOUNT_FLAG_NOSUID) APPEND_OPTION(",nosuid");
    if (node->flags & MOUNT_FLAG_NODEV) APPEND_OPTION(",nodev");
    if (node->flags & MOUNT_FLAG_NOEXEC) APPEND_OPTION(",noexec");
#    undef APPEND_OPTION
    if (capacity) output[used < capacity ? used : capacity - 1] = '\0';
    return used;
}

/* Find the nearest enclosing mount id above the node. */
static uint64_t vfs_parent_mount_id(vfs_node_t node)
{
    for (vfs_node_t parent = node ? node->parent : NULL; parent; parent = parent->parent)
        if (parent->is_mount && parent->mount_id) return parent->mount_id;
    return node && node->mount_id ? node->mount_id : 0;
}

/* Emit one mount table line per mount in the subtree. */
static void vfs_format_mount_subtree(vfs_node_t node, char *buffer, size_t capacity, size_t *used, bool mountinfo, vfs_mount_format_scratch_t *scratch)
{
    if (!node) return;
    if (node->is_mount && node->mount_id && vfs_node_path(node, scratch->path, sizeof(scratch->path)) == EOK) {
        vfs_mount_escape(scratch->escaped_path, sizeof(scratch->escaped_path), scratch->path);
        vfs_mount_escape(scratch->escaped_source, sizeof(scratch->escaped_source), node->mount_source);
        vfs_mount_options(scratch->options, sizeof(scratch->options), node);
        const char *type        = node->fsid < (uint16_t)fs_nextid && fs_names[node->fsid] ? fs_names[node->fsid] : "unknown";
        char       *destination = *used < capacity ? buffer + *used : buffer + capacity - 1;
        size_t      remaining   = *used < capacity ? capacity - *used : 0;
        int         length;
        if (mountinfo) {
            length = snprintf(destination, remaining, "%llu %llu 0:%u / %s %s - %s %s %s\n", node->mount_id, vfs_parent_mount_id(node), (unsigned)node->fsid, scratch->escaped_path, scratch->options,
                              type, scratch->escaped_source, (node->flags & MOUNT_FLAG_RDONLY) ? "ro" : "rw");
        } else {
            length = snprintf(destination, remaining, "%s %s %s %s 0 0\n", scratch->escaped_source, scratch->escaped_path, type, scratch->options);
        }
        if (length > 0) *used += (size_t)length;
    }
    for (clist_t child = node->child; child; child = child->next) vfs_format_mount_subtree((vfs_node_t)child->data, buffer, capacity, used, mountinfo, scratch);
}

/* Format the full mount table for /proc/mounts or /proc/self/mountinfo. */
size_t vfs_format_mount_table(char *buffer, size_t capacity, bool mountinfo)
{
    if (!buffer || !capacity) return 0;
    vfs_mount_format_scratch_t *scratch = malloc(sizeof(*scratch));
    if (!scratch) {
        plogk("vfs: mount table scratch alloc failed.\n");
        return 0;
    }

    size_t used = 0;
    buffer[0]   = '\0';
    vfs_ns_lock();
    vfs_format_mount_subtree(rootdir, buffer, capacity, &used, mountinfo, scratch);
    vfs_ns_unlock();
    free(scratch);

    if (used >= capacity) {
        buffer[capacity - 1] = '\0';
        return capacity - 1;
    }
    return used;
}

/* Read data from a file node into the provided memory buffer */
size_t vfs_read(vfs_node_t file, void *addr, size_t offset, size_t size)
{
    if (!file || !addr) return (size_t)-1;
    if (vfs_access_check(file, VFS_ACCESS_R)) return (size_t)-1;
    do_update(file);

    if (file->type & file_dir) return (size_t)-1;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    int64_t              result  = mapping ? pagecache_read(mapping, addr, offset, size) : (int64_t)callbackof(file, read)(file->handle, addr, offset, size);
    if (mapping) file->size = pagecache_size(mapping);
    if (result > 0) {
        vfs_touch_access(file);
        inotify_notify(file, IN_ACCESS);
    }
    return (size_t)result;
}

/* Read data from a link file node into the provided memory buffer */
size_t vfs_readlink(vfs_node_t node, char *buf, size_t bufsize)
{
    size_t len;

    if (!node || !buf || !bufsize) return 0;
    if (node->linkname) {
        len = strlen(node->linkname);
        if (len > bufsize) len = bufsize;
        memcpy(buf, node->linkname, len);
        return len;
    }

    return callbackof(node, readlink)(node, buf, 0, bufsize);
}

/* Write data from the provided memory buffer to a file node */
size_t vfs_write(vfs_node_t file, const void *addr, size_t offset, size_t size)
{
    if (!file || !addr) return (size_t)-1;
    if (file->flags & VFS_NODE_SWAPFILE) return (size_t)-1;
    if (vfs_access_check(file, VFS_ACCESS_W)) return (size_t)-1;
    do_update(file);

    if (file->type & file_dir) return (size_t)-1;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    int64_t              ret     = mapping ? pagecache_write(mapping, addr, offset, size) : (int64_t)callbackof(file, write)(file->handle, addr, offset, size);

    if (mapping)
        file->size = pagecache_size(mapping);
    else
        do_update(file);
    if (ret > 0) {
        vfs_touch_modify(file);
        inotify_notify(file, IN_MODIFY);
    }
    return (size_t)ret;
}

/* Read from a file node as a specific process, optionally enforcing its permissions. */
static int64_t vfs_file_read_process_impl(vfs_node_t file, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, process_t *proc, bool check_access)
{
    if (!file || !addr) return -EINVAL;
    if (check_access && vfs_access_check_process(file, VFS_ACCESS_R, proc)) return -EACCES;
    do_update(file);
    if (file->type & file_dir) return -EISDIR;

    int64_t              result;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    if (mapping)
        result = pagecache_read(mapping, addr, offset, size);
    else if (callbackof(file, file_read) != vfs_empty_callback.file_read)
        result = callbackof(file, file_read)(file, private_data, flags, addr, offset, size);
    else {
        size_t legacy_ret = callbackof(file, read)(file->handle, addr, offset, size);
        result            = legacy_ret == (size_t)-1 ? -EIO : (int64_t)legacy_ret;
    }
    /*
     * A filesystem callback may return a short read, but never more bytes
     * than the caller supplied.  Enforce the contract before the syscall
     * layer copies from its bounded bounce buffer.
     */
    if (result > 0 && (uint64_t)result > size) {
        plogk("vfs: Read overrun from %s callback: returned %lld for %lu requested.\n", file->name, (long long)result, (unsigned long)size);
        return -EIO;
    }
    if (result > 0) {
        vfs_touch_access(file);
        inotify_notify(file, IN_ACCESS);
    }
    return result;
}

/*
 * Read through an already-authorized descriptor.  Access rights are decided
 * once at open time and travel with the open file description across setuid,
 * fork and exec; re-checking the current fsuid here would revoke access that
 * was legitimately granted (e.g. a shell inheriting its terminal from su).
 */
int64_t vfs_file_read_granted(vfs_node_t file, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, process_t *proc)
{
    return vfs_file_read_process_impl(file, private_data, flags, addr, offset, size, proc, false);
}

/* Read from a file node as a specific process, enforcing its permissions. */
int64_t vfs_file_read_process(vfs_node_t file, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, process_t *proc)
{
    return vfs_file_read_process_impl(file, private_data, flags, addr, offset, size, proc, true);
}

/* Read a file node as the current process. */
int64_t vfs_file_read(vfs_node_t file, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    return vfs_file_read_process(file, private_data, flags, addr, offset, size, process_current());
}

/* Write to a file node as a specific process, optionally enforcing its permissions. */
static int64_t vfs_file_write_process_impl(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, process_t *proc, bool check_access)
{
    int64_t ret;

    if (!file || !addr) return -EINVAL;
    if (file->flags & VFS_NODE_SWAPFILE) return -EBUSY;
    if (check_access && vfs_access_check_process(file, VFS_ACCESS_W, proc)) return -EACCES;
    do_update(file);
    if (file->type & file_dir) return -EISDIR;

    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    if (mapping) {
        ret        = pagecache_write(mapping, addr, offset, size);
        file->size = pagecache_size(mapping);
        if (ret >= 0 && (flags & 0x101000U)) {
            int sync_result = pagecache_writeback(mapping, offset, size ? offset + size - 1 : offset, PAGECACHE_WB_SYNC);
            if (sync_result) {
                plogk("vfs: Writeback of %s failed (%d)\n", file->name, sync_result);
                ret = sync_result;
            }
        }
    } else if (callbackof(file, file_write) != vfs_empty_callback.file_write) {
        ret = callbackof(file, file_write)(file, private_data, flags, addr, offset, size);
    } else {
        size_t legacy_ret = callbackof(file, write)(file->handle, addr, offset, size);
        ret               = legacy_ret == (size_t)-1 ? -EIO : (int64_t)legacy_ret;
    }
    if (ret > 0 && (uint64_t)ret > size) return -EIO;

    if (!mapping) do_update(file);
    if (ret > 0) {
        vfs_touch_modify(file);
        inotify_notify(file, IN_MODIFY);
    }
    return ret;
}

/* Write through an already-authorized descriptor; see vfs_file_read_granted(). */
int64_t vfs_file_write_granted(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, process_t *proc)
{
    return vfs_file_write_process_impl(file, private_data, flags, addr, offset, size, proc, false);
}

/* Write to a file node as a specific process, enforcing its permissions. */
int64_t vfs_file_write_process(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, process_t *proc)
{
    return vfs_file_write_process_impl(file, private_data, flags, addr, offset, size, proc, true);
}

/* Write a file node as the current process. */
int64_t vfs_file_write(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    return vfs_file_write_process(file, private_data, flags, addr, offset, size, process_current());
}

#    define VFS_USER_IO_CHUNK PAGE_4K_SIZE

/*
 * Userspace I/O is carried through VFS instead of being unconditionally
 * bounced by the syscall layer.  Filesystems that understand user buffers
 * (pipes and no-copy devices) can consume them directly; all existing
 * callbacks retain their old semantics through this bounded fallback.
 */
static int64_t vfs_file_read_user_process_impl(vfs_node_t file, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, process_t *proc, bool check_access)
{
    if (!file || (!addr && size)) return -EINVAL;
    if (!user_range_ok(addr, size)) return -EFAULT;
    vfs_file_read_user_cb_t read_user = callbackof(file, file_read_user);

    /*
     * Pipes are already opened and have no cache-backed metadata to refresh.
     * Keep their user-buffer callback completely outside the generic VFS
     * update path: do_update() calls pipe stat on every short read, which is
     * particularly expensive for small-block streaming workloads.
     */
    if ((file->type & (file_stream | file_pipe)) && read_user != vfs_empty_callback.file_read_user) {
        if (check_access && vfs_access_check_process(file, VFS_ACCESS_R, proc)) return -EACCES;

        int64_t ret = read_user(file, private_data, flags, addr, offset, size, proc);
        if (ret > 0 && (uint64_t)ret > size) return -EIO;
        if (ret > 0) inotify_notify(file, IN_ACCESS);
        return ret;
    }

    /*
     * Page-cached regular files must use the cache path for both kernel and
     * userspace buffers.  Calling a filesystem's direct-user callback after
     * O_TRUNC has created a mapping would update the backing inode while the
     * zero-length cache continued to shadow it (and vice versa).
     */
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    if (!mapping && read_user != vfs_empty_callback.file_read_user) {
        if (check_access && vfs_access_check_process(file, VFS_ACCESS_R, proc)) return -EACCES;
        do_update(file);
        if (file->type & file_dir) return -EISDIR;

        int64_t ret = read_user(file, private_data, flags, addr, offset, size, proc);
        if (ret > 0 && (uint64_t)ret > size) return -EIO;
        if (ret > 0) {
            vfs_touch_access(file);
            inotify_notify(file, IN_ACCESS);
        }
        return ret;
    }

    if (!size) return 0;

    size_t   capacity = size < VFS_USER_IO_CHUNK ? size : VFS_USER_IO_CHUNK;
    uint8_t *tmp      = malloc(capacity);
    if (!tmp) return -ENOMEM;

    size_t  done   = 0;
    int64_t result = 0;
    while (done < size) {
        size_t  chunk = size - done < capacity ? size - done : capacity;
        int64_t ret
            = check_access ? vfs_file_read_process(file, private_data, flags, tmp, offset + done, chunk, proc) : vfs_file_read_granted(file, private_data, flags, tmp, offset + done, chunk, proc);
        if (ret < 0) {
            result = done ? (int64_t)done : ret;
            goto out;
        }
        if (!ret) break;
        if ((uint64_t)ret > chunk) {
            result = done ? (int64_t)done : -EIO;
            goto out;
        }
        if (copy_to_user((uint8_t *)addr + done, tmp, (size_t)ret)) {
            result = done ? (int64_t)done : -EFAULT;
            goto out;
        }
        done += (size_t)ret;
        if ((size_t)ret < chunk) break;
    }
    result = (int64_t)done;
out:
    free(tmp);
    return result;
}

/* Read through an already-authorized descriptor; see vfs_file_read_granted(). */
int64_t vfs_file_read_user_granted(vfs_node_t file, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, process_t *proc)
{
    return vfs_file_read_user_process_impl(file, private_data, flags, addr, offset, size, proc, false);
}

int64_t vfs_file_read_user_process(vfs_node_t file, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, process_t *proc)
{
    return vfs_file_read_user_process_impl(file, private_data, flags, addr, offset, size, proc, true);
}

static int64_t vfs_file_write_user_process_impl(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, process_t *proc, bool check_access)
{
    if (!file || (!addr && size)) return -EINVAL;
    if (!user_range_ok(addr, size)) return -EFAULT;
    vfs_file_write_user_cb_t write_user = callbackof(file, file_write_user);

    /*
     * See the matching read path above. A pipe write only moves bytes and
     * must not perform a per-call metadata refresh through do_update().
     * /
     */
    if ((file->type & (file_stream | file_pipe)) && write_user != vfs_empty_callback.file_write_user) {
        if (file->flags & VFS_NODE_SWAPFILE) return -EBUSY;
        if (check_access && vfs_access_check_process(file, VFS_ACCESS_W, proc)) return -EACCES;

        int64_t ret = write_user(file, private_data, flags, addr, offset, size, proc);
        if (ret > 0 && (uint64_t)ret > size) return -EIO;
        if (ret > 0) inotify_notify(file, IN_MODIFY);
        return ret;
    }

    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    if (!mapping && write_user != vfs_empty_callback.file_write_user) {
        if (file->flags & VFS_NODE_SWAPFILE) return -EBUSY;
        if (check_access && vfs_access_check_process(file, VFS_ACCESS_W, proc)) return -EACCES;
        do_update(file);
        if (file->type & file_dir) return -EISDIR;

        int64_t ret = write_user(file, private_data, flags, addr, offset, size, proc);
        if (ret > 0 && (uint64_t)ret > size) return -EIO;
        if (ret >= 0) do_update(file);
        if (ret > 0) {
            vfs_touch_modify(file);
            inotify_notify(file, IN_MODIFY);
        }
        return ret;
    }

    if (!size) {
        uint8_t empty = 0;
        return check_access ? vfs_file_write_process(file, private_data, flags, &empty, offset, 0, proc) : vfs_file_write_granted(file, private_data, flags, &empty, offset, 0, proc);
    }

    size_t   capacity = size < VFS_USER_IO_CHUNK ? size : VFS_USER_IO_CHUNK;
    uint8_t *tmp      = malloc(capacity);
    if (!tmp) return -ENOMEM;

    size_t  done   = 0;
    int64_t result = 0;
    while (done < size) {
        size_t chunk = size - done < capacity ? size - done : capacity;
        if (copy_from_user(tmp, (const uint8_t *)addr + done, chunk)) {
            result = done ? (int64_t)done : -EFAULT;
            goto out;
        }
        int64_t ret
            = check_access ? vfs_file_write_process(file, private_data, flags, tmp, offset + done, chunk, proc) : vfs_file_write_granted(file, private_data, flags, tmp, offset + done, chunk, proc);
        if (ret < 0) {
            result = done ? (int64_t)done : ret;
            goto out;
        }
        if (!ret) break;
        if ((uint64_t)ret > chunk) {
            result = done ? (int64_t)done : -EIO;
            goto out;
        }
        done += (size_t)ret;
        if ((size_t)ret < chunk) break;
    }
    result = (int64_t)done;
out:
    free(tmp);
    return result;
}

/* Write through an already-authorized descriptor; see vfs_file_read_granted(). */
int64_t vfs_file_write_user_granted(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, process_t *proc)
{
    return vfs_file_write_user_process_impl(file, private_data, flags, addr, offset, size, proc, false);
}

int64_t vfs_file_write_user_process(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, process_t *proc)
{
    return vfs_file_write_user_process_impl(file, private_data, flags, addr, offset, size, proc, true);
}

/* Check whether the node's mount subtree is read-only. */
int vfs_mount_is_readonly(vfs_node_t node)
{
    for (vfs_node_t current = node; current; current = current->parent) {
        if (current->is_mount) return (current->flags & MOUNT_FLAG_RDONLY) != 0;
        if (current == current->parent) break;
    }
    return 0;
}

/* Flush all cached data for the file to stable storage. */
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

/* Write back a byte range of the file. */
int vfs_writeback_range(vfs_node_t file, uint64_t start, uint64_t end, int data_only)
{
    if (!file || end < start) return -EINVAL;
    do_update(file);
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 0);
    if (mapping) return pagecache_writeback(mapping, start, end, PAGECACHE_WB_SYNC);
    if (callbackof(file, sync) != vfs_empty_callback.sync) return callbackof(file, sync)(file->handle, data_only);
    return EOK;
}

/* Write back every dirty page in the system page cache. */
int vfs_sync_all(void)
{
    return pagecache_writeback_all(PAGECACHE_WB_SYNC);
}

/* Truncate or extend a regular file to the given size. */
int vfs_truncate(vfs_node_t file, uint64_t size)
{
    if (!file) return -EINVAL;
    if (file->flags & VFS_NODE_SWAPFILE) return -EBUSY;
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
        vfs_touch_modify(file);
        inotify_notify(file, IN_MODIFY);
    }
    return result;
}

/* Drop cached pages in a byte range, optionally discarding dirty data. */
int vfs_invalidate_pages(vfs_node_t file, uint64_t start, uint64_t end, int discard_dirty)
{
    if (!file) return -EINVAL;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 0);
    if (!mapping) return EOK;
    return pagecache_invalidate(mapping, start, end, discard_dirty ? PAGECACHE_INVALIDATE_DISCARD_DIRTY : 0);
}

/* Evict cached pages in a byte range, optionally writing them back first. */
int vfs_drop_pages(vfs_node_t file, uint64_t start, uint64_t end, int writeback)
{
    if (!file || end < start) return -EINVAL;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 0);
    if (!mapping) return EOK;
    return pagecache_evict(mapping, start, end, writeback ? PAGECACHE_EVICT_WRITEBACK : 0);
}

/* Prefetch a byte range into the page cache. */
int vfs_readahead(vfs_node_t file, uint64_t offset, size_t size)
{
    if (!file) return -EINVAL;
    do_update(file);
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    return mapping ? pagecache_readahead(mapping, offset, size) : -EOPNOTSUPP;
}

/* Pin the file's cache mapping so it survives eviction. */
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

/* Release a pin on the file's cache mapping. */
void vfs_cache_mapping_unpin(vfs_node_t file)
{
    if (file && file->mapping) pagecache_mapping_unpin(file->mapping);
}

/* Map a cached page to a physical frame for userspace DMA. */
int vfs_cache_map_page(vfs_node_t file, uint64_t index, int dirty, uint64_t *physical)
{
    if (!file || !physical) return -EINVAL;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    if (!mapping) return -EOPNOTSUPP;

    /*
     * Reclaim publishes PC_PAGE_EVICTING before unlinking a page.  A fault
     * racing that short window can legitimately receive -ENOENT from
     * pagecache_lock_page(); treating it as a bad userspace address turns a
     * transient SMP cache race into SIGSEGV.  Drop the reference and retry so
     * the next lookup can install or find the replacement page.
     */
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        pagecache_page_t *page = pagecache_get_page(mapping, index, 1);
        if (!page) return -ENOMEM;
        int result = pagecache_lock_page(page, 1);
        if (!result) {
            if (dirty) pagecache_mark_dirty(page);
            *physical = pagecache_page_physical(page);
            if (frame_retain_range(*physical, 1)) result = -ENOMEM;
            pagecache_unlock_page(page);
            pagecache_put_page(page);
            if (!result) pagecache_mmap_readahead(mapping, index);
            return result;
        }
        pagecache_put_page(page);
        if (result != -ENOENT) return result;
        __asm__ volatile("pause");
    }
    return -EAGAIN;
}

/* Mark every cached page in a byte range as dirty. */
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

/* Forward an ioctl to the file's callback. */
int vfs_file_ioctl(vfs_node_t file, void *private_data, uint64_t flags, size_t req, void *arg)
{
    if (!file) return -EINVAL;
    do_update(file);
    if (file->type & file_dir) return -EISDIR;

    if (callbackof(file, file_ioctl) != vfs_empty_callback.file_ioctl) return callbackof(file, file_ioctl)(file, private_data, flags, req, arg);
    return callbackof(file, ioctl)(file->handle, req, arg);
}

/* Poll a file for readiness of the given events. */
int vfs_file_poll(vfs_node_t file, void *private_data, uint64_t flags, size_t events)
{
    if (!file) return -EINVAL;
    do_update(file);
    if (file->type & file_dir) return -EISDIR;

    if (callbackof(file, file_poll) != vfs_empty_callback.file_poll) return callbackof(file, file_poll)(file, private_data, flags, events);
    return callbackof(file, poll)(file->handle, events);
}

/* Notify the filesystem that the last descriptor closed. */
void vfs_file_descriptor_close(vfs_node_t file, void *private_data)
{
    if (!file) return;
    if (callbackof(file, file_descriptor_close) != vfs_empty_callback.file_descriptor_close) callbackof(file, file_descriptor_close)(file, private_data);
}

/* Return the readiness-notification source of a file. */
vfs_poll_source_t *vfs_file_poll_source(vfs_node_t file, void *private_data)
{
    if (!file) return NULL;
    if (callbackof(file, file_poll_source) != vfs_empty_callback.file_poll_source) {
        vfs_poll_source_t *source = callbackof(file, file_poll_source)(file, private_data);
        if (source) return source;
    }
    return &file->poll_source;
}

/* Initialize a poll source. */
void vfs_poll_source_init(vfs_poll_source_t *source)
{
    if (!source) return;
    memset(source, 0, sizeof(*source));
}

/* Register a subscription, notifying immediately if the source is closed. */
void vfs_poll_source_subscribe(vfs_poll_source_t *source, vfs_poll_subscription_t *subscription, uint32_t events, vfs_poll_notify_t notify, void *context)
{
    if (!source || !subscription || !notify) return;
    spin_lock(&source->lock);
    subscription->notify  = notify;
    subscription->context = context;
    subscription->events  = events;
    bool closed           = source->closed;
    if (!closed) {
        subscription->next       = source->subscribers;
        subscription->subscribed = true;
        source->subscribers      = subscription;
    } else {
        subscription->next       = NULL;
        subscription->subscribed = false;
    }
    spin_unlock(&source->lock);
    if (closed) notify(subscription, UINT32_MAX);
}

/* Remove a subscription from the source. */
void vfs_poll_source_unsubscribe(vfs_poll_source_t *source, vfs_poll_subscription_t *subscription)
{
    if (!source || !subscription) return;
    spin_lock(&source->lock);
    vfs_poll_subscription_t **link = &source->subscribers;
    while (*link && *link != subscription) link = &(*link)->next;
    if (*link) {
        *link = subscription->next;

        /*
         * Only touch ->next while the subscription is still linked.  A
         * subscription detached by vfs_poll_source_close() is iterated
         * outside the lock; clearing its ->next here would truncate that
         * iteration.
         */
        subscription->next = NULL;
    }
    subscription->subscribed = false;
    spin_unlock(&source->lock);
}

/* Deliver matching events to every subscribed poll waiter. */
void vfs_poll_source_notify(vfs_poll_source_t *source, uint32_t events)
{
    if (!source) return;
    if (!__atomic_load_n(&source->subscribers, __ATOMIC_ACQUIRE)) return;
    spin_lock(&source->lock);
    for (vfs_poll_subscription_t *sub = source->subscribers; sub; sub = sub->next) {
        uint32_t matched = events & sub->events;
        if (matched) sub->notify(sub, matched);
    }
    spin_unlock(&source->lock);
}

/* Close the source, waking all subscribers exactly once. */
void vfs_poll_source_close(vfs_poll_source_t *source, uint32_t events)
{
    if (!source) return;

    spin_lock(&source->lock);
    source->closed                        = true;
    vfs_poll_subscription_t *subscription = source->subscribers;
    source->subscribers                   = NULL;
    for (vfs_poll_subscription_t *sub = subscription; sub; sub = sub->next) sub->subscribed = false;
    spin_unlock(&source->lock);

    /*
     * Close is one-shot: the subscriber list is detached under the lock and a
     * concurrent unsubscribe (epoll_ctl(EPOLL_CTL_DEL), poll timeout) no
     * longer mutates a detached subscription's ->next, so nothing can free or
     * truncate the snapshot we are about to walk.  Callbacks run AFTER the
     * lock is released - they may unsubscribe other poll sources, remove epoll
     * items, drop file references, or close a source that owns one of these
     * subscriptions.  Running them under source->lock would self-deadlock if a
     * callback re-entered this source (cascading epoll close), which is why
     * the lock must not be held across invocation.
     */
    while (subscription) {
        vfs_poll_subscription_t *next    = subscription->next;
        uint32_t                 matched = events & subscription->events;
        subscription->next               = NULL;
        if (matched) subscription->notify(subscription, matched);
        subscription = next;
    }
}

/* Subscribe to readiness notifications on a node's poll source. */
void vfs_poll_subscribe(vfs_node_t file, vfs_poll_subscription_t *subscription, uint32_t events, vfs_poll_notify_t notify, void *context)
{
    if (file) vfs_poll_source_subscribe(&file->poll_source, subscription, events, notify, context);
}

/* Remove a readiness-notification subscription. */
void vfs_poll_unsubscribe(vfs_node_t file, vfs_poll_subscription_t *subscription)
{
    if (file) vfs_poll_source_unsubscribe(&file->poll_source, subscription);
}

/* Notify subscribers of readiness events on a node. */
void vfs_poll_notify(vfs_node_t file, uint32_t events)
{
    if (file) vfs_poll_source_notify(&file->poll_source, events);
}

/* Close the file or directory node */
int vfs_close(vfs_node_t node)
{
    if (!node) return -EINVAL;

    vfs_ns_lock();

    /*
     * Namespace nodes must be closed exactly once for every retained
     * reference.  Anonymous descriptor nodes deliberately start at zero and
     * use their first close as the final release.
     */
    if (!node->refcount && node->parent && !(node->type & file_delete)) {
        vfs_ns_unlock();
        return -EINVAL;
    }
    if (node->refcount) node->refcount--;
    bool last_ref = (node->refcount == 0);

    if (node == rootdir || !node->handle || node->type & file_proxy || !last_ref) {
        vfs_ns_unlock();
        return EOK;
    }

    if (!(node->type & file_delete)) {
        /*
         * Non-delete close: invoke the close callback once.
         * For anonymous nodes (parent == NULL) that have no filesystem
         * entry, free the handle and node now instead of leaking them.
         */
        bool anonymous = node->parent == NULL;
        if (anonymous) {
            node->flags |= VFS_NODE_FINALIZING;
            spin_lock(&node->poll_source.lock);
            node->poll_source.closed = true;
            spin_unlock(&node->poll_source.lock);
        }
        vfs_ns_unlock();
        if (anonymous) vfs_poll_notify(node, UINT32_MAX);
        if (node->mapping) (void)pagecache_writeback(node->mapping, 0, UINT64_MAX, PAGECACHE_WB_SYNC);
        if (anonymous) vfs_pagecache_destroy(node);
        callbackof(node, close)(node->handle);
        if (anonymous) {
            callbackof(node, free)(node->handle);
            node->handle = 0;
            vfs_free(node);
        }
        return EOK;
    }

    node->flags |= VFS_NODE_FINALIZING;
    vfs_ns_unlock();

    if (node->type & file_dir) {
        vfs_ns_lock();
        bool not_empty = vfs_directory_has_visible_children(node);
        vfs_ns_unlock();
        if (not_empty) {
            vfs_ns_lock();
            node->flags &= ~VFS_NODE_FINALIZING;
            vfs_ns_unlock();
            return -ENOTEMPTY;
        }
    }

    if (node->mapping && !(node->flags & VFS_NODE_DELETE_COMMITTED)) {
        int result = pagecache_writeback(node->mapping, 0, UINT64_MAX, PAGECACHE_WB_SYNC);
        if (result) {
            vfs_ns_lock();
            node->flags &= ~VFS_NODE_FINALIZING;
            vfs_ns_unlock();
            return result;
        }
    }

    if (!(node->flags & VFS_NODE_DELETE_COMMITTED)) {
        int res = node->parent ? callbackof(node, delete)(node->parent->handle, node) : EOK;
        if (res < 0) {
            vfs_ns_lock();
            node->flags &= ~VFS_NODE_FINALIZING;
            vfs_ns_unlock();
            return res;
        }
    }

    /*
     * A synchronous unlink has already released the backing inode/blocks.
     * Never let mapping destruction write stale pages back to that storage.
     */
    if (node->mapping && (node->flags & VFS_NODE_DELETE_COMMITTED)) (void)pagecache_invalidate(node->mapping, 0, UINT64_MAX, PAGECACHE_INVALIDATE_DISCARD_DIRTY);
    vfs_pagecache_destroy(node);
    callbackof(node, close)(node->handle);
    vfs_node_t retained_parent = NULL;
    vfs_ns_lock();
    if (!(node->flags & VFS_NODE_UNLINKED) && node->parent) {
        node->parent->child = clist_delete(node->parent->child, node);
        node->flags |= VFS_NODE_UNLINKED;
    }
    if (node->flags & VFS_NODE_PARENT_RETAINED) {
        retained_parent = node->parent;
        node->flags &= ~VFS_NODE_PARENT_RETAINED;
    }
    node->parent = NULL;
    vfs_ns_unlock();
    callbackof(node, free)(node->handle);
    node->handle = 0;
    vfs_free(node);
    if (retained_parent) vfs_close(retained_parent);
    return EOK;
}

/* Unlink a node from its parent, deferring the final free. */
int vfs_namespace_unlink(vfs_node_t node)
{
    if (!node || node == rootdir) return -EINVAL;
    if (node->flags & VFS_NODE_SWAPFILE) return -EBUSY;
    if (!node->parent) return -EINVAL;

    vfs_ns_lock();
    if ((node->flags & (VFS_NODE_UNLINKED | VFS_NODE_UNLINKING | VFS_NODE_RENAME_BUSY)) || !node->parent || (node->parent->flags & VFS_NODE_RENAME_BUSY)) {
        vfs_ns_unlock();
        return -ENOENT;
    }
    if ((node->type & file_dir) && vfs_directory_has_visible_children(node)) {
        vfs_ns_unlock();
        return -ENOTEMPTY;
    }

    node->flags |= VFS_NODE_UNLINKING;
    vfs_ns_unlock();

    int status = callbackof(node, delete)(node->parent->handle, node);
    if (status < 0) {
        vfs_ns_lock();
        node->flags &= ~VFS_NODE_UNLINKING;
        vfs_ns_unlock();
        return status;
    }

    if (!(node->flags & VFS_NODE_EVENT_DELETE)) {
        node->flags |= VFS_NODE_EVENT_DELETE;
        inotify_notify_delete(node);
    }

    vfs_ns_lock();
    vfs_node_t parent = node->parent;
    parent->child     = clist_delete(parent->child, node);
    node->parent      = NULL;
    node->flags &= ~VFS_NODE_UNLINKING;
    node->flags |= VFS_NODE_DELETE_COMMITTED | VFS_NODE_UNLINKED;
    node->type |= file_delete;
    vfs_ns_unlock();
    return EOK;
}

/* Detach a node and its children from the namespace for deferred free. */
void vfs_namespace_detach(vfs_node_t node)
{
    if (!node || node == rootdir) return;

    vfs_ns_lock();
    if (node->flags & (VFS_NODE_UNLINKED | VFS_NODE_UNLINKING | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) {
        vfs_ns_unlock();
        return;
    }
    node->flags |= VFS_NODE_UNLINKING;
    vfs_ns_unlock();
    if (!(node->flags & VFS_NODE_EVENT_DELETE)) {
        node->flags |= VFS_NODE_EVENT_DELETE;
        inotify_notify_delete(node);
    }

    vfs_ns_lock();
    if (node->parent) node->parent->child = clist_delete(node->parent->child, node);
    node->parent = NULL;
    node->flags |= VFS_NODE_UNLINKED | VFS_NODE_DELETE_COMMITTED | VFS_NODE_UNLINKING;
    node->type |= file_delete;
    vfs_ns_unlock();

    /*
     * Detach children through the same deferred-free path.  A temporary
     * reference keeps each selected child alive after dropping the namespace
     * lock; open descriptors retain their own references independently.
     */
    vfs_free_child(node);

    vfs_ns_lock();
    node->flags &= ~VFS_NODE_UNLINKING;
    int release_now = node->refcount == 0;
    vfs_ns_unlock();
    if (release_now) vfs_close(node);
}

/* Delete a VFS (Virtual File System) node and clean up associated resources */
int vfs_delete(vfs_node_t node)
{
    int status;

    if (!node || node == rootdir) return -EINVAL;
    if (node->flags & VFS_NODE_SWAPFILE) return -EBUSY;

    do_update(node);
    vfs_ns_lock();
    if ((node->flags & (VFS_NODE_INITIALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) || (node->parent && (node->parent->flags & VFS_NODE_RENAME_BUSY))
        || (node->type & file_delete)) {
        vfs_ns_unlock();
        return -ENOENT;
    }
    if ((node->type & file_dir) && vfs_directory_has_visible_children(node)) {
        vfs_ns_unlock();
        return -ENOTEMPTY;
    }
    node->flags |= VFS_NODE_UNLINKING;
    vfs_ns_unlock();

    if ((node->flags & VFS_NODE_DELETE_SYNC) && node->parent) {
        /*
         * Flush while the filesystem object still exists.  Once delete()
         * succeeds, the callback may have freed its inode and data blocks.
         */
        if (node->mapping) {
            status = pagecache_writeback(node->mapping, 0, UINT64_MAX, PAGECACHE_WB_SYNC);
            if (status < 0) goto delete_failed;
        }
        status = callbackof(node, delete)(node->parent->handle, node);
        if (status < 0) goto delete_failed;
        node->flags |= VFS_NODE_DELETE_COMMITTED;
    }
    if (!(node->flags & VFS_NODE_EVENT_DELETE)) {
        node->flags |= VFS_NODE_EVENT_DELETE;
        inotify_notify_delete(node);
    }
    if (node->parent) vfs_touch_modify(node->parent);
    vfs_ns_lock();
    node->type |= file_delete;
    node->flags &= ~VFS_NODE_UNLINKING;
    if (node->parent && !(node->flags & VFS_NODE_PARENT_RETAINED)) {
        node->parent->refcount++;
        node->flags |= VFS_NODE_PARENT_RETAINED;
    }
    bool release_now = node->refcount == 0;
    vfs_ns_unlock();
    if (release_now) return vfs_close(node);
    return EOK;
delete_failed:
    vfs_ns_lock();
    node->flags &= ~VFS_NODE_UNLINKING;
    vfs_ns_unlock();
    return status;
}

/* Enforce the sticky bit on rename victims. */
static int vfs_rename_sticky_check(vfs_node_t parent, vfs_node_t victim)
{
    process_t *process = process_current();
    if (!process || process->fsuid == 0 || !(parent->mode & 01000)) return EOK;
    return process->fsuid == parent->owner || process->fsuid == victim->owner ? EOK : -EPERM;
}

/* Rename is a single backend transaction followed by one VFS namespace commit. */
int vfs_rename(vfs_node_t node, vfs_node_t new_parent, const char *new_name_arg, uint32_t flags)
{
    int        status   = EOK;
    char      *old_name = NULL, *new_name = NULL;
    clist_t    new_link   = NULL;
    vfs_node_t old_parent = NULL, target = NULL;
    bool       target_retained = false;

    if (!node || !new_parent || !new_name_arg || !new_name_arg[0] || strchr(new_name_arg, '/') || streq(new_name_arg, ".") || streq(new_name_arg, "..")) return -EINVAL;
    if (flags & ~VFS_RENAME_NOREPLACE) return -EINVAL;
    if (strlen(new_name_arg) > VFS_NAME_MAX) return -ENAMETOOLONG;
    if (!(new_parent->type & file_dir)) return -ENOTDIR;
    if (!node->parent) return -EINVAL;
    if (node->fsid != new_parent->fsid || node->root != new_parent->root) return -EXDEV;
    if (node->is_mount || (node->flags & VFS_NODE_SWAPFILE)) return -EBUSY;
    if (vfs_mount_is_readonly(node) || vfs_mount_is_readonly(new_parent)) return -EROFS;
    if (callbackof(node, rename) == vfs_empty_callback.rename) return -EOPNOTSUPP;

    vfs_rename_serial_acquire();
    old_parent = node->parent;
    if (!old_parent) {
        status = -ENOENT;
        goto out;
    }
    if (vfs_access_check(old_parent, VFS_ACCESS_W | VFS_ACCESS_X) != EOK || vfs_access_check(new_parent, VFS_ACCESS_W | VFS_ACCESS_X) != EOK) {
        status = -EACCES;
        goto out;
    }
    status = vfs_rename_sticky_check(old_parent, node);
    if (status != EOK) goto out;

    old_name = strdup(node->name);
    new_name = strdup(new_name_arg);
    if (!old_name || !new_name) {
        status = -ENOMEM;
        goto out;
    }
    if (old_parent != new_parent) {
        new_link = clist_alloc(node);
        if (!new_link) {
            status = -ENOMEM;
            goto out;
        }
    }

    vfs_ns_lock();
    if (node->parent != old_parent || (node->flags & (VFS_NODE_INITIALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) || (node->type & file_delete)
        || (old_parent->flags & (VFS_NODE_INITIALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) || (old_parent->type & file_delete)
        || (new_parent->flags & (VFS_NODE_INITIALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) || (new_parent->type & file_delete)) {
        status = -EBUSY;
        goto unlock_error;
    }
    if (old_parent == new_parent && streq(node->name, new_name_arg)) {
        vfs_ns_unlock();
        status = EOK;
        goto out;
    }
    for (vfs_node_t ancestor = new_parent; ancestor; ancestor = ancestor->parent) {
        if (ancestor == node) {
            status = -EINVAL;
            goto unlock_error;
        }
        if (ancestor == ancestor->parent) break;
    }

    target = vfs_child_find_reserved(new_parent, new_name_arg);
    if (target == node) {
        vfs_ns_unlock();
        status = EOK;
        goto out;
    }
    if (target) {
        if (flags & VFS_RENAME_NOREPLACE) {
            status = -EEXIST;
            goto unlock_error;
        }
        if (target->flags & (VFS_NODE_INITIALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) {
            status = -EBUSY;
            goto unlock_error;
        }
        bool same_inode = target->handle == node->handle || (target->fsid == node->fsid && target->inode && target->inode == node->inode);
        if (same_inode) {
            vfs_ns_unlock();
            status = EOK;
            goto out;
        }
        bool source_is_dir = (node->type & file_dir) != 0;
        bool target_is_dir = (target->type & file_dir) != 0;
        if (source_is_dir && !target_is_dir)
            status = -ENOTDIR;
        else if (!source_is_dir && target_is_dir)
            status = -EISDIR;
        else if (target_is_dir && vfs_directory_has_visible_children(target))
            status = -ENOTEMPTY;
        else if (target->is_mount || (target->flags & VFS_NODE_SWAPFILE))
            status = -EBUSY;
        else
            status = vfs_rename_sticky_check(new_parent, target);
        if (status != EOK) goto unlock_error;
        if (target->refcount == UINT32_MAX) {
            status = -EOVERFLOW;
            goto unlock_error;
        }
        target->refcount++;
        target_retained = true;
        target->flags |= VFS_NODE_INITIALIZING;
    }
    node->flags |= VFS_NODE_INITIALIZING;
    old_parent->flags |= VFS_NODE_RENAME_BUSY;
    new_parent->flags |= VFS_NODE_RENAME_BUSY;
    vfs_ns_unlock();

    vfs_rename_context_t context = {
        .old_parent = old_parent,
        .source     = node,
        .new_parent = new_parent,
        .target     = target,
        .new_name   = new_name,
        .flags      = flags,
    };
    status = callbackof(node, rename)(&context);
    if (status != EOK) {
        vfs_ns_lock();
        node->flags &= ~VFS_NODE_INITIALIZING;
        old_parent->flags &= ~VFS_NODE_RENAME_BUSY;
        new_parent->flags &= ~VFS_NODE_RENAME_BUSY;
        if (target) target->flags &= ~VFS_NODE_INITIALIZING;
        vfs_ns_unlock();
        goto out;
    }

    if (target && !(target->flags & VFS_NODE_EVENT_DELETE)) {
        target->flags |= VFS_NODE_EVENT_DELETE;
        inotify_notify_delete(target);
    }
    vfs_ns_lock();
    if (target) {
        new_parent->child = clist_delete(new_parent->child, target);
        target->parent    = NULL;
        target->type |= file_delete;
        target->flags &= ~VFS_NODE_INITIALIZING;
        target->flags |= VFS_NODE_DELETE_COMMITTED | VFS_NODE_UNLINKED;
    }
    if (old_parent != new_parent) {
        old_parent->child = clist_delete(old_parent->child, node);
        new_link->next    = new_parent->child;
        if (new_parent->child) new_parent->child->prev = new_link;
        new_parent->child = new_link;
        new_link          = NULL;
        node->parent      = new_parent;
    }
    free(node->name);
    node->name = new_name;
    new_name   = NULL;
    node->flags &= ~VFS_NODE_INITIALIZING;
    old_parent->flags &= ~VFS_NODE_RENAME_BUSY;
    new_parent->flags &= ~VFS_NODE_RENAME_BUSY;
    old_parent->visited = 0;
    new_parent->visited = 0;
    vfs_touch_change(node);
    vfs_touch_modify(old_parent);
    if (new_parent != old_parent) vfs_touch_modify(new_parent);
    vfs_ns_unlock();

    vfs_rename_serial_release();
    inotify_notify_move(node, old_parent, old_name, node->name);
    if (target) vfs_close(target);
    free(old_name);
    return EOK;
unlock_error:
    vfs_ns_unlock();
out:
    vfs_rename_serial_release();
    if (target_retained && (target->flags & VFS_NODE_INITIALIZING)) {
        vfs_ns_lock();
        target->flags &= ~VFS_NODE_INITIALIZING;
        vfs_ns_unlock();
    }
    if (target_retained) vfs_close(target);
    free(new_link);
    free(new_name);
    free(old_name);
    return status;
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
    for (;;) {
        vfs_ns_lock();
        while (vfs->child && !vfs->child->data) vfs->child = clist_delete_node(vfs->child, vfs->child);
        vfs_node_t child = vfs->child ? vfs->child->data : NULL;
        if (child) child->refcount++;
        vfs_ns_unlock();
        if (!child) break;
        vfs_namespace_detach(child);
        vfs_close(child);
    }
}

/* Free the memory associated with a vfs node */
void vfs_free(vfs_node_t vfs)
{
    if (!vfs) return;

    vfs_free_child(vfs);
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
    free(vfs->mount_source);
    free(vfs->name);
    free(vfs);
}

/* Initialize the virtual file system */
void init_vfs(void)
{
    for (size_t i = 0; i < sizeof(struct vfs_callback) / sizeof(void *); i++) ((void **)&vfs_empty_callback)[i] = empty_func;
    wait_queue_init(&vfs_namespace_wait);
    vfs_namespace_busy = false;
    wait_queue_init(&vfs_rename_wait);
    vfs_rename_serial_busy          = false;
    pagecache_allocator_t allocator = {.alloc = vfs_page_alloc, .free = vfs_page_free};
    size_t                max_pages = frame_allocator.origin_frames / 2;
    if (max_pages < 256) max_pages = 256;
    (void)pagecache_init(&allocator, max_pages);
    rootdir       = vfs_node_alloc(0, "/");
    rootdir->type = file_dir;
    plogk("vfs: Initial root directory of the virtual file system: '/'\n");
}

#endif
