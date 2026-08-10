/*
 *
 *      cgroupfs.c
 *      Control group virtual filesystem
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cgroup/cgroup.h>
#include <fs/core/vfs.h>
#include <fs/virtual/cgroupfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/heap.h>

#define CGROUPFS_BUFSIZE 4096

typedef enum cgroupfs_type {
    CGROUPFS_DIR,
    CGROUPFS_CONTROLLERS,
    CGROUPFS_SUBTREE_CONTROL,
    CGROUPFS_PROCS,
    CGROUPFS_EVENTS,
    CGROUPFS_TYPE,
    CGROUPFS_PIDS_CURRENT,
    CGROUPFS_PIDS_MAX,
    CGROUPFS_PIDS_EVENTS,
} cgroupfs_type_t;

typedef struct {
        cgroupfs_type_t type;
        cgroup_t       *cgroup;
} cgroupfs_node_t;

static int cgroupfs_id;

static cgroupfs_node_t *new_handle(cgroupfs_type_t type, cgroup_t *cgroup)
{
    cgroupfs_node_t *handle = calloc(1, sizeof(*handle));
    if (handle) {
        cgroup = cgroup_get(cgroup);
        if (!cgroup) {
            free(handle);
            return NULL;
        }
        handle->type   = type;
        handle->cgroup = cgroup;
    }
    return handle;
}

static int add_file(vfs_node_t parent, const char *name, cgroupfs_type_t type, uint16_t mode)
{
    vfs_node_t node = vfs_node_alloc(parent, name);
    if (!node) return -ENOMEM;
    node->handle = new_handle(type, ((cgroupfs_node_t *)parent->handle)->cgroup);
    if (!node->handle) {
        parent->child = clist_delete(parent->child, node);
        vfs_free(node);
        return -ENOMEM;
    }
    node->fsid  = cgroupfs_id;
    node->type  = file_stream;
    node->mode  = mode;
    node->flags = VFS_NODE_VIRTUAL;
    return EOK;
}

static int populate(vfs_node_t dir)
{
    static const struct {
            const char     *name;
            cgroupfs_type_t type;
            uint16_t        mode;
    } files[] = {
        {"cgroup.controllers",     CGROUPFS_CONTROLLERS,     0444},
        {"cgroup.subtree_control", CGROUPFS_SUBTREE_CONTROL, 0644},
        {"cgroup.procs",           CGROUPFS_PROCS,           0644},
        {"cgroup.events",          CGROUPFS_EVENTS,          0444},
        {"cgroup.type",            CGROUPFS_TYPE,            0644},
        {"pids.current",           CGROUPFS_PIDS_CURRENT,    0444},
        {"pids.max",               CGROUPFS_PIDS_MAX,        0644},
        {"pids.events",            CGROUPFS_PIDS_EVENTS,     0444},
    };

    cgroup_t *cgroup = ((cgroupfs_node_t *)dir->handle)->cgroup;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (cgroup_is_root(cgroup) && (files[i].type == CGROUPFS_EVENTS || files[i].type == CGROUPFS_TYPE)) continue;
        if (files[i].type >= CGROUPFS_PIDS_CURRENT && !cgroup_pids_available(cgroup)) continue;
        if (!vfs_do_search(dir, files[i].name)) {
            int status = add_file(dir, files[i].name, files[i].type, files[i].mode);
            if (status != EOK) return status;
        }
    }
    return EOK;
}

static int mount_cgroup2(const char *src, vfs_node_t node)
{
    /* cgroup2 is nodev; the source operand is informational. */
    (void)src;
    if (!node) return -EINVAL;
    if (!cgroup_root()) return -ENODEV;
    node->handle = new_handle(CGROUPFS_DIR, cgroup_root());
    if (!node->handle) return -ENOMEM;
    node->fsid = cgroupfs_id;
    node->type = file_dir;
    node->mode = 0755;
    node->flags |= VFS_NODE_DELETE_SYNC;
    return populate(node);
}

static int render(cgroupfs_node_t *node, char *buf, size_t size)
{
    if (!node->cgroup) return -ENOENT;
    switch (node->type) {
        case CGROUPFS_CONTROLLERS :
            return cgroup_show_controllers(node->cgroup, buf, size);
        case CGROUPFS_SUBTREE_CONTROL :
            return cgroup_show_subtree_control(node->cgroup, buf, size);
        case CGROUPFS_PROCS :
            return cgroup_show_procs(node->cgroup, buf, size);
        case CGROUPFS_EVENTS :
            return cgroup_show_events(node->cgroup, buf, size);
        case CGROUPFS_TYPE :
            return snprintf(buf, size, "domain\n");
        case CGROUPFS_PIDS_CURRENT :
            return cgroup_show_pids_current(node->cgroup, buf, size);
        case CGROUPFS_PIDS_MAX :
            return cgroup_show_pids_max(node->cgroup, buf, size);
        case CGROUPFS_PIDS_EVENTS :
            return cgroup_show_pids_events(node->cgroup, buf, size);
        default :
            return -EISDIR;
    }
}

static int64_t file_read(vfs_node_t vnode, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    char buf[CGROUPFS_BUFSIZE];
    int  length;
    (void)private_data;
    (void)flags;
    if (!vnode->handle) return -ENOENT;
    length = render(vnode->handle, buf, sizeof(buf));
    if (length < 0) return length;
    if (offset >= (size_t)length) return 0;
    if (size > (size_t)length - offset) size = (size_t)length - offset;
    memcpy(addr, buf + offset, size);
    return (int64_t)size;
}

static int64_t file_write(vfs_node_t vnode, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    cgroupfs_node_t *node = vnode->handle;
    int              status;
    (void)private_data;
    (void)flags;
    if (!node) return -ENOENT;
    if (offset) return -EINVAL;
    if (node->type == CGROUPFS_SUBTREE_CONTROL)
        status = cgroup_set_subtree_control(node->cgroup, addr, size);
    else if (node->type == CGROUPFS_PROCS)
        status = cgroup_move_pid(node->cgroup, addr, size);
    else if (node->type == CGROUPFS_PIDS_MAX)
        status = cgroup_set_pids_max(node->cgroup, addr, size);
    else if (node->type == CGROUPFS_TYPE)
        status = -EOPNOTSUPP;
    else
        status = -EROFS;
    return status == EOK ? (int64_t)size : status;
}

static size_t legacy_read(void *handle, void *addr, size_t offset, size_t size)
{
    char buf[CGROUPFS_BUFSIZE];
    int  length = handle ? render(handle, buf, sizeof(buf)) : -ENOENT;
    if (length < 0 || offset >= (size_t)length) return 0;
    if (size > (size_t)length - offset) size = (size_t)length - offset;
    memcpy(addr, buf + offset, size);
    return size;
}

static size_t legacy_write(void *handle, const void *addr, size_t offset, size_t size)
{
    cgroupfs_node_t *node = handle;
    int              status;
    if (!node || offset) return (size_t)-1;
    if (node->type == CGROUPFS_SUBTREE_CONTROL)
        status = cgroup_set_subtree_control(node->cgroup, addr, size);
    else if (node->type == CGROUPFS_PROCS)
        status = cgroup_move_pid(node->cgroup, addr, size);
    else if (node->type == CGROUPFS_PIDS_MAX)
        status = cgroup_set_pids_max(node->cgroup, addr, size);
    else
        status = -EROFS;
    return status == EOK ? size : (size_t)-1;
}

static int stat_node(void *handle, vfs_node_t node)
{
    cgroupfs_node_t *cn = handle;
    if (!cn) return -ENOENT;
    if (cn->type == CGROUPFS_DIR) {
        node->type = file_dir;
        return populate(node);
    }
    /*
     * cgroup control files have regular-file offset semantics even though
     * their contents are generated dynamically.
     */
    node->type = file_none;
    return EOK;
}

static int mkdir_node(void *parent, const char *name, vfs_node_t node)
{
    cgroupfs_node_t *pn = parent;
    cgroup_t        *cgroup;
    int              status;
    if (!pn || pn->type != CGROUPFS_DIR) return -ENOTDIR;
    status = cgroup_create(pn->cgroup, name, &cgroup);
    if (status != EOK) return status;
    node->handle = new_handle(CGROUPFS_DIR, cgroup);
    if (!node->handle) {
        cgroup_destroy(cgroup);
        return -ENOMEM;
    }
    node->fsid = cgroupfs_id;
    node->type = file_dir;
    node->mode = 0755;
    node->flags |= VFS_NODE_DELETE_SYNC;
    status = populate(node);
    if (status != EOK) { cgroup_destroy(cgroup); }
    return status;
}

static int readonly_create(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -EROFS;
}

static int delete_node(void *parent, vfs_node_t node)
{
    cgroupfs_node_t *cn = node->handle;
    int              status;
    (void)parent;
    if (!cn) return -ENOENT;
    if (cn->type != CGROUPFS_DIR) return -EROFS;
    status = cgroup_destroy(cn->cgroup);
    return status;
}

static int free_handle(void *handle)
{
    cgroupfs_node_t *node = handle;
    if (node) cgroup_put(node->cgroup);
    free(node);
    return EOK;
}
static int no_rename(void *handle, const char *name)
{
    (void)handle;
    (void)name;
    return -EOPNOTSUPP;
}

static struct vfs_callback callbacks = {
    .mount      = mount_cgroup2,
    .read       = legacy_read,
    .write      = legacy_write,
    .mkdir      = mkdir_node,
    .mkfile     = readonly_create,
    .stat       = stat_node,
    .free       = free_handle,
    .delete     = delete_node,
    .rename     = no_rename,
    .file_read  = file_read,
    .file_write = file_write,
};

void cgroupfs_regist(void)
{
#if CONFIG_CGROUP
    cgroupfs_id = vfs_regist_fs_flags("cgroup2", &callbacks, VFS_FS_NODEV);
    if (cgroupfs_id & ERRNO_MASK) plogk("cgroup2: Registration failed (%d)\n", cgroupfs_id);
#endif
}
