/*
 *
 *      cgroupfs.c
 *      Control group virtual filesystem
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <cgroup/cgroup.h>
#include <fs/cgroup/cgroupfs.h>
#include <fs/core/inotify.h>
#include <fs/core/vfs.h>
#include <fs/core/vfs_stub.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <sync/mutex.h>

#if CONFIG_CGROUP

#    define CGROUPFS_BUFSIZE 4096

typedef enum cgroupfs_type {
    CGROUPFS_DIR,
    CGROUPFS_CONTROLLERS,
    CGROUPFS_SUBTREE_CONTROL,
    CGROUPFS_PROCS,
    CGROUPFS_THREADS,
    CGROUPFS_EVENTS,
    CGROUPFS_TYPE,
    CGROUPFS_KILL,
    CGROUPFS_FREEZE,
    CGROUPFS_STAT,
    CGROUPFS_MAX_DESCENDANTS,
    CGROUPFS_MAX_DEPTH,

    /* pids */
    CGROUPFS_PIDS_CURRENT,
    CGROUPFS_PIDS_MAX,
    CGROUPFS_PIDS_EVENTS,

    /* memory */
    CGROUPFS_MEMORY_CURRENT,
    CGROUPFS_MEMORY_MAX,
    CGROUPFS_MEMORY_HIGH,
    CGROUPFS_MEMORY_LOW,
    CGROUPFS_MEMORY_STAT,
    CGROUPFS_MEMORY_EVENTS,
    CGROUPFS_MEMORY_SWAP_CURRENT,
    CGROUPFS_MEMORY_SWAP_MAX,

    /* cpu */
    CGROUPFS_CPU_MAX,
    CGROUPFS_CPU_WEIGHT,
    CGROUPFS_CPU_STAT,

    /* io */
    CGROUPFS_IO_MAX,
    CGROUPFS_IO_WEIGHT,
    CGROUPFS_IO_STAT,

    /* cpuset */
    CGROUPFS_CPUSET_CPUS,
    CGROUPFS_CPUSET_MEMS,
    CGROUPFS_CPUSET_CPUS_EFFECTIVE,
    CGROUPFS_CPUSET_MEMS_EFFECTIVE,
} cgroupfs_type_t;

typedef struct cgroup_event_path {
        char                      path[CONFIG_VFS_PATH_MAX];
        cgroup_t                 *cgroup;
        uint64_t                  observed;
        struct cgroup_event_path *next;
} cgroup_event_path_t;

static mutex_t              event_paths_lock;
static cgroup_event_path_t *event_paths;

typedef struct {
        cgroupfs_type_t      type;
        cgroup_t            *cgroup;
        cgroup_event_path_t *events;
} cgroupfs_node_t;

static int cgroupfs_id;

/* Allocate a cgroupfs handle, taking a reference on the cgroup. */
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

/* Add a virtual control file to the given cgroup directory. */
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
    node->fsid = cgroupfs_id;
    /* Control files have finite content; stream reads restart at offset zero. */
    node->type  = file_none;
    node->mode  = mode;
    node->flags = VFS_NODE_VIRTUAL;
    if (type == CGROUPFS_EVENTS) {
        cgroup_event_path_t *entry = calloc(1, sizeof(*entry));
        if (entry && vfs_node_path(node, entry->path, sizeof(entry->path)) == EOK) {
            cgroupfs_node_t *handle = node->handle;
            entry->cgroup           = handle->cgroup;
            entry->observed         = cgroup_event_sequence(handle->cgroup);
            handle->events          = entry;
            mutex_lock(&event_paths_lock);
            entry->next = event_paths;
            event_paths = entry;
            mutex_unlock(&event_paths_lock);
        } else
            free(entry);
    }
    return EOK;
}

/* Populate a directory with default cgroup2 control files. */
static int populate(vfs_node_t dir)
{
    static const struct {
            const char     *name;
            cgroupfs_type_t type;
            uint16_t        mode;
            uint64_t        ctrl_flag; // 0 = core control file
    } files[] = {
        {"cgroup.controllers",     CGROUPFS_CONTROLLERS,           0444, 0                       },
        {"cgroup.subtree_control", CGROUPFS_SUBTREE_CONTROL,       0644, 0                       },
        {"cgroup.procs",           CGROUPFS_PROCS,                 0644, 0                       },
        {"cgroup.threads",         CGROUPFS_THREADS,               0644, 0                       },
        {"cgroup.events",          CGROUPFS_EVENTS,                0444, 0                       },
        {"cgroup.type",            CGROUPFS_TYPE,                  0644, 0                       },
        {"cgroup.kill",            CGROUPFS_KILL,                  0200, 0                       },
        {"cgroup.freeze",          CGROUPFS_FREEZE,                0644, 0                       },
        {"cgroup.stat",            CGROUPFS_STAT,                  0444, 0                       },
        {"cgroup.max.descendants", CGROUPFS_MAX_DESCENDANTS,       0644, 0                       },
        {"cgroup.max.depth",       CGROUPFS_MAX_DEPTH,             0644, 0                       },

        {"pids.current",           CGROUPFS_PIDS_CURRENT,          0444, CGROUP_CONTROLLER_PIDS  },
        {"pids.max",               CGROUPFS_PIDS_MAX,              0644, CGROUP_CONTROLLER_PIDS  },
        {"pids.events",            CGROUPFS_PIDS_EVENTS,           0444, CGROUP_CONTROLLER_PIDS  },

        {"memory.current",         CGROUPFS_MEMORY_CURRENT,        0444, CGROUP_CONTROLLER_MEMORY},
        {"memory.max",             CGROUPFS_MEMORY_MAX,            0644, CGROUP_CONTROLLER_MEMORY},
        {"memory.high",            CGROUPFS_MEMORY_HIGH,           0644, CGROUP_CONTROLLER_MEMORY},
        {"memory.low",             CGROUPFS_MEMORY_LOW,            0644, CGROUP_CONTROLLER_MEMORY},
        {"memory.stat",            CGROUPFS_MEMORY_STAT,           0444, CGROUP_CONTROLLER_MEMORY},
        {"memory.events",          CGROUPFS_MEMORY_EVENTS,         0444, CGROUP_CONTROLLER_MEMORY},
        {"memory.swap.current",    CGROUPFS_MEMORY_SWAP_CURRENT,   0444, CGROUP_CONTROLLER_MEMORY},
        {"memory.swap.max",        CGROUPFS_MEMORY_SWAP_MAX,       0644, CGROUP_CONTROLLER_MEMORY},

        {"cpu.max",                CGROUPFS_CPU_MAX,               0644, CGROUP_CONTROLLER_CPU   },
        {"cpu.weight",             CGROUPFS_CPU_WEIGHT,            0644, CGROUP_CONTROLLER_CPU   },
        {"cpu.stat",               CGROUPFS_CPU_STAT,              0444, CGROUP_CONTROLLER_CPU   },

        {"io.max",                 CGROUPFS_IO_MAX,                0644, CGROUP_CONTROLLER_IO    },
        {"io.weight",              CGROUPFS_IO_WEIGHT,             0644, CGROUP_CONTROLLER_IO    },
        {"io.stat",                CGROUPFS_IO_STAT,               0444, CGROUP_CONTROLLER_IO    },

        {"cpuset.cpus.effective",  CGROUPFS_CPUSET_CPUS_EFFECTIVE, 0444, CGROUP_CONTROLLER_CPUSET},
        {"cpuset.mems.effective",  CGROUPFS_CPUSET_MEMS_EFFECTIVE, 0444, CGROUP_CONTROLLER_CPUSET},
        {"cpuset.cpus",            CGROUPFS_CPUSET_CPUS,           0644, CGROUP_CONTROLLER_CPUSET},
        {"cpuset.mems",            CGROUPFS_CPUSET_MEMS,           0644, CGROUP_CONTROLLER_CPUSET},
    };

    cgroup_t *cgroup = ((cgroupfs_node_t *)dir->handle)->cgroup;
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (cgroup_is_root(cgroup) && (files[i].type == CGROUPFS_EVENTS || files[i].type == CGROUPFS_TYPE || files[i].type == CGROUPFS_KILL)) continue;
        if (files[i].ctrl_flag && !cgroup_controller_available(cgroup, files[i].ctrl_flag)) continue;
        if (!vfs_do_search(dir, files[i].name)) {
            int status = add_file(dir, files[i].name, files[i].type, files[i].mode);
            if (status != EOK) return status;
        }
    }
    return EOK;
}

/* Mount the cgroup2 filesystem on a VFS node. */
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

/* Render the contents of a control file into the caller's buffer. */
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
        case CGROUPFS_THREADS :
            return cgroup_show_threads(node->cgroup, buf, size);
        case CGROUPFS_EVENTS :
            return cgroup_show_events(node->cgroup, buf, size);
        case CGROUPFS_TYPE :
            return snprintf(buf, size, "domain\n");
        case CGROUPFS_STAT :
            return cgroup_show_stat(node->cgroup, buf, size);
        case CGROUPFS_MAX_DESCENDANTS :
        case CGROUPFS_MAX_DEPTH :
            return cgroup_show_max(node->cgroup, buf, size);
        case CGROUPFS_PIDS_CURRENT :
            return cgroup_show_pids_current(node->cgroup, buf, size);
        case CGROUPFS_PIDS_MAX :
            return cgroup_show_pids_max(node->cgroup, buf, size);
        case CGROUPFS_PIDS_EVENTS :
            return cgroup_show_pids_events(node->cgroup, buf, size);
        case CGROUPFS_MEMORY_CURRENT :
            return cgroup_show_memory_zero(node->cgroup, buf, size);
        case CGROUPFS_MEMORY_MAX :
            return cgroup_show_memory_max(node->cgroup, buf, size);
        case CGROUPFS_MEMORY_HIGH :
            return cgroup_show_memory_high(node->cgroup, buf, size);
        case CGROUPFS_MEMORY_LOW :
            return cgroup_show_memory_low(node->cgroup, buf, size);
        case CGROUPFS_MEMORY_STAT :
            return cgroup_show_memory_stat(node->cgroup, buf, size);
        case CGROUPFS_MEMORY_EVENTS :
            return cgroup_show_memory_events(node->cgroup, buf, size);
        case CGROUPFS_MEMORY_SWAP_CURRENT :
            return cgroup_show_memory_zero(node->cgroup, buf, size);
        case CGROUPFS_MEMORY_SWAP_MAX :
            return cgroup_show_memory_swap_max(node->cgroup, buf, size);
        case CGROUPFS_CPU_MAX :
            return cgroup_show_cpu_max(node->cgroup, buf, size);
        case CGROUPFS_CPU_WEIGHT :
            return cgroup_show_cpu_weight(node->cgroup, buf, size);
        case CGROUPFS_CPU_STAT :
            return cgroup_show_cpu_stat(node->cgroup, buf, size);
        case CGROUPFS_IO_MAX :
            return cgroup_show_io_empty(node->cgroup, buf, size);
        case CGROUPFS_IO_WEIGHT :
            return cgroup_show_io_weight(node->cgroup, buf, size);
        case CGROUPFS_IO_STAT :
            return cgroup_show_io_empty(node->cgroup, buf, size);
        case CGROUPFS_CPUSET_CPUS_EFFECTIVE :
            return cgroup_show_cpuset(node->cgroup, buf, size, false, true);
        case CGROUPFS_CPUSET_MEMS_EFFECTIVE :
            return cgroup_show_cpuset(node->cgroup, buf, size, true, true);
        case CGROUPFS_CPUSET_CPUS :
            return cgroup_show_cpuset_cpus(node->cgroup, buf, size);
        case CGROUPFS_CPUSET_MEMS :
            return cgroup_show_cpuset_mems(node->cgroup, buf, size);
        default :
            return -EISDIR;
    }
}

/* Read from a control file into the given buffer. */
static int64_t file_read(vfs_node_t vnode, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    char buf[CGROUPFS_BUFSIZE];
    int  length;
    (void)flags;
    if (!vnode || !vnode->handle) return -ENOENT;
    if (!offset && private_data && ((cgroupfs_node_t *)vnode->handle)->type == CGROUPFS_EVENTS) *(uint64_t *)private_data = cgroup_event_sequence(((cgroupfs_node_t *)vnode->handle)->cgroup);
    if (!vnode->handle) return -ENOENT;
    length = render(vnode->handle, buf, sizeof(buf));
    if (length < 0) return length;
    if (offset >= (size_t)length) return 0;
    if (size > (size_t)length - offset) size = (size_t)length - offset;
    memcpy(addr, buf + offset, size);
    return (int64_t)size;
}

/* Apply a write to a control file. */
static int64_t file_write(vfs_node_t vnode, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    if (!vnode) return -ENOENT;
    cgroupfs_node_t *node = vnode->handle;
    int              status;
    (void)private_data;
    (void)flags;
    if (!node) return -ENOENT;
    if (offset) return -EINVAL;

    switch (node->type) {
        case CGROUPFS_SUBTREE_CONTROL :
            status = cgroup_set_subtree_control(node->cgroup, addr, size);
            break;
        case CGROUPFS_PROCS :
        case CGROUPFS_THREADS :
            status = cgroup_move_pid(node->cgroup, addr, size);
            break;
        case CGROUPFS_KILL :
            status = cgroup_kill(node->cgroup, addr, size);
            break;
        case CGROUPFS_FREEZE :
            status = cgroup_set_freeze(node->cgroup, addr, size);
            break;
        case CGROUPFS_PIDS_MAX :
            status = cgroup_set_pids_max(node->cgroup, addr, size);
            break;
        case CGROUPFS_MEMORY_MAX :
            status = cgroup_set_memory_max(node->cgroup, addr, size);
            break;
        case CGROUPFS_MEMORY_HIGH :
            status = cgroup_set_memory_high(node->cgroup, addr, size);
            break;
        case CGROUPFS_MEMORY_LOW :
            status = cgroup_set_memory_low(node->cgroup, addr, size);
            break;
        case CGROUPFS_CPU_MAX :
            status = cgroup_set_cpu_max(node->cgroup, addr, size);
            break;
        case CGROUPFS_CPU_WEIGHT :
            status = cgroup_set_cpu_weight(node->cgroup, addr, size);
            break;
        case CGROUPFS_IO_MAX :
            status = cgroup_set_io_max(node->cgroup, addr, size);
            break;
        case CGROUPFS_IO_WEIGHT :
            status = cgroup_set_io_weight(node->cgroup, addr, size);
            break;
        case CGROUPFS_CPUSET_CPUS :
            status = cgroup_set_cpuset_cpus(node->cgroup, addr, size);
            break;
        case CGROUPFS_CPUSET_MEMS :
            status = cgroup_set_cpuset_mems(node->cgroup, addr, size);
            break;
        case CGROUPFS_TYPE :
            status = -EOPNOTSUPP;
            break;
        default :
            status = -EROFS;
            break;
    }

    return status == EOK ? (int64_t)size : status;
}

/* Legacy read callback for the VFS adapter. */
static size_t legacy_read(void *handle, void *addr, size_t offset, size_t size)
{
    char buf[CGROUPFS_BUFSIZE];
    int  length = handle ? render(handle, buf, sizeof(buf)) : -ENOENT;
    if (length < 0 || offset >= (size_t)length) return 0;
    if (size > (size_t)length - offset) size = (size_t)length - offset;
    memcpy(addr, buf + offset, size);
    return size;
}

/* Legacy write callback for the VFS adapter. */
static size_t legacy_write(void *handle, const void *addr, size_t offset, size_t size)
{
    cgroupfs_node_t *node = handle;
    if (!node || offset) return (size_t)-1;
    int64_t ret = file_write(NULL, NULL, 0, addr, 0, size);
    return ret >= 0 ? (size_t)ret : (size_t)-1;
}

/* Report the node type; populate directories lazily on stat. */
static int stat_node(void *handle, vfs_node_t node)
{
    cgroupfs_node_t *cn = handle;
    if (!cn) return -ENOENT;
    if (cn->type == CGROUPFS_DIR) {
        node->type = file_dir;
        return populate(node);
    }
    node->type = file_none;
    return EOK;
}

/* Create a child cgroup directory. */
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
    if (status != EOK) cgroup_destroy(cgroup);
    return status;
}

/* Destroy a cgroup directory. */
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

/* Release the cgroup reference and free the handle. */
static int free_handle(void *handle)
{
    cgroupfs_node_t *node = handle;
    if (node && node->events) {
        mutex_lock(&event_paths_lock);
        cgroup_event_path_t **link = &event_paths;
        while (*link && *link != node->events) link = &(*link)->next;
        if (*link) *link = node->events->next;
        mutex_unlock(&event_paths_lock);
        free(node->events);
    }
    if (node) cgroup_put(node->cgroup);
    free(node);
    return EOK;
}

/* Deliver outside cgroup and filesystem registry locks, including oneshot watches. */
void cgroupfs_notify_events(void)
{
    for (unsigned budget = 0; budget < 32; budget++) {
        char path[CONFIG_VFS_PATH_MAX];
        bool changed = false;
        mutex_lock(&event_paths_lock);
        for (cgroup_event_path_t *entry = event_paths; entry; entry = entry->next) {
            uint64_t sequence = cgroup_event_sequence(entry->cgroup);
            if (sequence == entry->observed) continue;
            entry->observed = sequence;
            memcpy(path, entry->path, sizeof(path));
            changed = true;
            break;
        }
        mutex_unlock(&event_paths_lock);
        if (!changed) break;
        vfs_node_t node = vfs_open(path);
        if (!node) continue;
        inotify_notify(node, IN_MODIFY);
        vfs_poll_notify(node, POLLPRI | POLLERR);
        vfs_close(node);
    }
}

static int control_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    (void)flags;
    cgroupfs_node_t *handle = node->handle;
    *private_data           = NULL;
    if (handle->type != CGROUPFS_EVENTS) return EOK;
    uint64_t *sequence = malloc(sizeof(*sequence));
    if (!sequence) return -ENOMEM;
    *sequence     = cgroup_event_sequence(handle->cgroup);
    *private_data = sequence;
    return EOK;
}

static void control_release(vfs_node_t node, void *private_data)
{
    (void)node;
    free(private_data);
}

static int control_poll(vfs_node_t node, void *private_data, uint64_t flags, size_t events)
{
    (void)flags;
    int              ready  = (int)(events & (POLLIN | POLLOUT));
    cgroupfs_node_t *handle = node->handle;
    if (handle->type == CGROUPFS_EVENTS && private_data && *(uint64_t *)private_data != cgroup_event_sequence(handle->cgroup)) ready |= POLLPRI | POLLERR;
    return ready;
}

static struct vfs_callback callbacks = {
    .mount        = mount_cgroup2,
    .read         = legacy_read,
    .write        = legacy_write,
    .mkdir        = mkdir_node,
    .mkfile       = vfs_stub_mk_readonly,
    .stat         = stat_node,
    .free         = free_handle,
    .delete       = delete_node,
    .file_open    = control_open,
    .file_release = control_release,
    .file_poll    = control_poll,
    .file_read    = file_read,
    .file_write   = file_write,
};

/* Register the cgroup2 filesystem with the VFS layer. */
void cgroupfs_regist(void)
{
    mutex_init(&event_paths_lock);
    cgroupfs_id = vfs_regist_fs_flags("cgroup2", &callbacks, VFS_FS_NODEV);
    if (cgroupfs_id & ERRNO_MASK) {
        plogk("cgroup2: Registration failed (%d)\n", cgroupfs_id);
    } else {
        plogk("cgroup2: Filesystem registered (fsid=%d)\n", cgroupfs_id);
    }
}

#endif
