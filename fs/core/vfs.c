/*
 *
 *      vfs.c
 *      Virtual file system
 *
 *      2025/11/2 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <fs/core/dcache.h>
#include <fs/core/icache.h>
#include <fs/core/inotify.h>
#include <fs/core/vfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/debug/debug.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/pagecache.h>
#include <process/process.h>
#include <process/namespace.h>
#include <process/uaccess.h>
#include <sync/mutex.h>

/* Largest slice of a userspace buffer moved in one VFS call. */
#define VFS_USER_IO_CHUNK PAGE_4K_SIZE

vfs_node_t               rootdir = 0;
static mutex_t           vfs_namespace_lock;
static mutex_t           vfs_rename_serial_lock;
static uint64_t          vfs_next_ino      = 1;
static uint64_t          vfs_next_mount_id = 1;
static uint64_t          vfs_next_peer_group = 1;
static uint64_t          mount_generation  = 1;
static vfs_poll_source_t mount_poll_source;

#define VFS_MOUNT_ATTRIBUTES (MOUNT_FLAG_RDONLY | MOUNT_FLAG_NOSUID | MOUNT_FLAG_NODEV | MOUNT_FLAG_NOEXEC | MOUNT_FLAG_ATIME)

/* Depth and breadth limit for walking a bound subtree.  A desktop session
 * carries enough submounts (systemd's per-unit credential mounts included)
 * that a small cap silently truncates the mirror set. */
#define VFS_MOUNT_MIRROR_MAX 64
/* One mount's own path; compose buffers are heap-allocated, not stacked. */
#define VFS_MOUNT_PATH_MAX 512

/*
 * A filesystem tree is shared by all namespaces referring to it.  Only the
 * attachment table and mount attributes are cloned.  Covered dentries stay
 * in their backing tree: mounting never substitutes their handle or children.
 */
typedef struct vfs_mount_object {
        vfs_node_t root;
        uint32_t references;
        bool filesystem;
        struct vfs_mount_object *retired_next;
} vfs_mount_object_t;

typedef struct vfs_mount_attachment {
        vfs_node_t covered;                 /* mountpoint dentry in the parent mount's tree */
        vfs_mount_object_t *object;
        struct vfs_mount_attachment *parent; /* the mount this one is attached into */
        uint64_t attributes;
        uint64_t locked_attributes;
        bool locked;
        size_t open_files;
        uint64_t id;
        uint32_t propagation;               /* VFS_MOUNT_* */
        uint32_t peer_group;                /* shared peer group id, 0 when not shared */
        uint32_t master_group;              /* peer group this mount is a slave of, 0 when none */
        struct vfs_mount_attachment *next;
} vfs_mount_attachment_t;

typedef struct vfs_mount_table {
        vfs_mount_attachment_t *entries;
        vfs_mount_attachment_t *root; /* namespace root mount; parent == NULL */
        uint64_t root_attributes;
        uint32_t root_propagation;
        uint32_t root_peer_group;
        uint32_t root_master_group;
        uint64_t generation;
        vfs_poll_source_t poll_source;
        mnt_namespace_t *namespace;
        struct vfs_mount_table *next_all;
} vfs_mount_table_t;

static vfs_mount_table_t *mount_tables;
static vfs_mount_attachment_t *retired_policies;
static vfs_mount_object_t *retired_mounts;
static bool mount_reaping;
static int vfs_propagate_attach_locked(vfs_mount_table_t *table, vfs_mount_attachment_t *root);
static void vfs_drop_tree_locked(vfs_mount_table_t *table, vfs_mount_attachment_t *root);

/* Build the namespace's root mount, the anchor of every mount tree in it. */
static void vfs_mount_root_init(vfs_mount_table_t *table)
{
    vfs_mount_object_t     *object = calloc(1, sizeof(*object));
    vfs_mount_attachment_t *root   = calloc(1, sizeof(*root));
    if (!object || !root) {
        free(object);
        free(root);
        return;
    }
    object->root       = rootdir;
    object->references = 1;
    object->filesystem = true;
    root->covered      = rootdir;
    root->object       = object;
    root->attributes   = table->root_attributes;
    /* The initial root mount is private; systemd shares it during boot. */
    root->propagation  = table->root_propagation;
    root->peer_group   = table->root_peer_group;
    root->master_group = table->root_master_group;
    root->id           = rootdir->mount_id ? rootdir->mount_id : __atomic_fetch_add(&vfs_next_mount_id, 1, __ATOMIC_RELAXED);
    table->root        = root;
}

/* Caller holds vfs_namespace_lock, except single-threaded early boot. */
static vfs_mount_table_t *vfs_mount_table(mnt_namespace_t *ns, bool create)
{
    if (!ns) ns = &init_mnt_ns;
    vfs_mount_table_t *table = ns->root_mount;
    if (!table && create) {
        table = calloc(1, sizeof(*table));
        if (table) {
            table->generation = 1;
            table->root_attributes = rootdir ? rootdir->flags & VFS_MOUNT_ATTRIBUTES : 0;
            table->root_propagation = VFS_MOUNT_PRIVATE;
            vfs_poll_source_init(&table->poll_source);
            table->namespace = ns;
            table->next_all = mount_tables;
            mount_tables = table;
            ns->root_mount = table;
        }
    }
    /* The root mount can only be built once the root dentry exists. */
    if (table && !table->root && rootdir) vfs_mount_root_init(table);
    return table;
}

static vfs_mount_attachment_t *vfs_mount_attachment(vfs_mount_table_t *table, vfs_node_t node)
{
    for (vfs_mount_attachment_t *entry = table ? table->entries : NULL; entry; entry = entry->next)
        if (entry->covered == node || entry->object->root == node) return entry;
    return NULL;
}

/*
 * The mount covering `dentry` when the walk arrives through `parent_mount`.
 * Keying on the pair is what keeps two views of one dentry apart, as __lookup_mnt
 * does in Linux.  Entries are pushed to the head, so the first match is the
 * topmost mount.
 */
static vfs_mount_attachment_t *vfs_mount_lookup(vfs_mount_table_t *table, vfs_mount_attachment_t *parent_mount, vfs_node_t dentry)
{
    for (vfs_mount_attachment_t *entry = table ? table->entries : NULL; entry; entry = entry->next)
        if (entry->parent == parent_mount && entry->covered == dentry) return entry;
    return NULL;
}

static vfs_mount_attachment_t *vfs_mount_find_id(vfs_mount_table_t *table, uint64_t id)
{
    if (!table || !id) return NULL;
    if (table->root && table->root->id == id) return table->root;
    for (vfs_mount_attachment_t *entry = table->entries; entry; entry = entry->next)
        if (entry->id == id) return entry;
    return NULL;
}

/*
 * A bind-mount root owns no subtree of its own: every name lookup and every
 * directory listing is answered by the subtree it aliases.  Following the
 * chain lets a bind of a bind resolve to the real directory in one step.
 */
static vfs_node_t vfs_alias_resolve(vfs_node_t node)
{
    for (unsigned depth = 0; node && node->alias && depth < 8; depth++) node = node->alias;
    return node;
}

/* Called only by a lookup that already owns the namespace lock. */
static vfs_node_t vfs_cross_mount(vfs_node_t node)
{
    vfs_mount_table_t *table = vfs_mount_table(mnt_namespace_current(), false);
    for (unsigned depth = 0; depth < 40; depth++) {
        vfs_mount_attachment_t *entry = vfs_mount_attachment(table, node);
        if (!entry || entry->covered != node || entry->object->root == node) break;
        node = entry->object->root;
    }
    return node;
}

static vfs_node_t vfs_path_parent_table(vfs_mount_table_t *table, vfs_node_t node)
{
    vfs_mount_attachment_t *entry = vfs_mount_attachment(table, node);
    return entry && entry->object->root == node ? entry->covered->parent : node->parent;
}

/*
 * The mount whose tree holds `dentry`: the innermost mount rooted at it, or the
 * innermost one attached at one of its strict ancestors.  A mount stacked at
 * `dentry` itself is deliberately not the answer, which is what lets a second
 * mount land beside the first instead of inside it.
 */
static vfs_mount_attachment_t *vfs_mount_containing(vfs_mount_table_t *table, vfs_node_t dentry)
{
    for (vfs_node_t node = dentry; node;) {
        for (vfs_mount_attachment_t *entry = table ? table->entries : NULL; entry; entry = entry->next)
            if (entry->object->root == node) return entry;
        vfs_node_t parent = vfs_path_parent_table(table, node);
        if (!parent || parent == node) break;
        if (parent != dentry)
            for (vfs_mount_attachment_t *entry = table ? table->entries : NULL; entry; entry = entry->next)
                if (entry->covered == parent) return entry;
        node = parent;
    }
    return table ? table->root : NULL;
}

static const char *vfs_path_name_table(vfs_mount_table_t *table, vfs_node_t node)
{
    vfs_mount_attachment_t *entry = vfs_mount_attachment(table, node);
    return entry && entry->object->root == node ? entry->covered->name : node->name;
}

/*
 * Is `mount` `ancestor` itself or a mount stacked below it?  The parent chain is
 * the only structural answer: covered dentries live in the parent's tree, so
 * comparing dentry paths cannot see across a mount boundary.
 */
static bool vfs_mount_below(vfs_mount_attachment_t *mount, vfs_mount_attachment_t *ancestor)
{
    if (!mount || !ancestor) return false;
    for (vfs_mount_attachment_t *cursor = mount; cursor; cursor = cursor->parent)
        if (cursor == ancestor) return true;
    return false;
}

static uint32_t vfs_new_peer_group(void)
{
    uint64_t id = __atomic_fetch_add(&vfs_next_peer_group, 1, __ATOMIC_RELAXED);
    return id ? (uint32_t)id : 1;
}

/* The three propagation fields a mount carries, wherever they are stored. */
typedef struct vfs_propagation {
        uint32_t type;
        uint32_t peer_group;
        uint32_t master_group;
} vfs_propagation_t;

/* The mount a new attachment lands in, or NULL for the namespace root, whose
 * state the table holds directly, as it does for its attributes. */
static vfs_mount_attachment_t *vfs_mount_target(vfs_mount_table_t *table, vfs_node_t node)
{
    return vfs_mount_attachment(table, node);
}

/* Does any mount but the one being tested carry `group`? */
static bool vfs_peer_group_alone(vfs_mount_table_t *table, vfs_mount_attachment_t *self, uint32_t group)
{
    if (!group) return true;
    if (!self) self = table->root;
    for (vfs_mount_table_t *other = mount_tables; other; other = other->next_all) {
        if (other->root && other->root != self && other->root->peer_group == group) return false;
        for (vfs_mount_attachment_t *entry = other->entries; entry; entry = entry->next)
            if (entry != self && entry->peer_group == group) return false;
    }
    return true;
}

static void vfs_peer_leave_locked(vfs_mount_attachment_t *self)
{
    if (!self->peer_group) return;
    for (vfs_mount_table_t *table = mount_tables; table; table = table->next_all)
        for (vfs_mount_attachment_t *entry = table->root; entry; entry = entry == table->root ? table->entries : entry->next)
            if (entry != self && entry->peer_group == self->peer_group) return;
    for (vfs_mount_table_t *table = mount_tables; table; table = table->next_all) {
        for (vfs_mount_attachment_t *entry = table->root; entry; entry = entry == table->root ? table->entries : entry->next) {
            if (entry->master_group != self->peer_group) continue;
            entry->master_group = self->master_group;
            if (!entry->peer_group && !entry->master_group) entry->propagation = VFS_MOUNT_PRIVATE;
        }
        if (table->root) {
            table->root_propagation = table->root->propagation;
            table->root_master_group = table->root->master_group;
        }
    }
}

static void vfs_propagation_read(vfs_mount_table_t *table, vfs_mount_attachment_t *entry, vfs_propagation_t *out)
{
    if (!entry && table) {
        out->type         = table->root_propagation;
        out->peer_group   = table->root_peer_group;
        out->master_group = table->root_master_group;
        return;
    }
    out->type         = entry ? entry->propagation : VFS_MOUNT_PRIVATE;
    out->peer_group   = entry ? entry->peer_group : 0;
    out->master_group = entry ? entry->master_group : 0;
}

static void vfs_propagation_write(vfs_mount_table_t *table, vfs_mount_attachment_t *entry, const vfs_propagation_t *value)
{
    if (!entry && table) {
        table->root_propagation  = value->type;
        table->root_peer_group   = value->peer_group;
        table->root_master_group = value->master_group;
        if (table->root) {
            table->root->propagation = value->type;
            table->root->peer_group = value->peer_group;
            table->root->master_group = value->master_group;
        }
        return;
    }
    if (!entry) return;
    entry->propagation  = value->type;
    entry->peer_group   = value->peer_group;
    entry->master_group = value->master_group;
}

/*
 * One make-xxxx step, as tabulated in Documentation/filesystems/sharedsubtree.rst.
 * Slaving a mount that is not shared is a no-op, and slaving a shared mount that
 * is alone in its peer group leaves it private, because there is no peer to
 * receive events from.
 */
static void vfs_propagation_change(vfs_mount_table_t *table, vfs_mount_attachment_t *entry, vfs_propagation_t *state, uint32_t type)
{
    switch (type) {
        case VFS_MOUNT_SHARED :
            /* Joining a peer group of one's own, keeping any existing master. */
            if (state->type != VFS_MOUNT_SHARED) state->peer_group = vfs_new_peer_group();
            state->type = VFS_MOUNT_SHARED;
            break;
        case VFS_MOUNT_SLAVE :
            if (state->type != VFS_MOUNT_SHARED) break;
            if (vfs_peer_group_alone(table, entry, state->peer_group)) {
                state->type         = state->master_group ? VFS_MOUNT_SLAVE : VFS_MOUNT_PRIVATE;
                state->peer_group   = 0;
            } else {
                state->master_group = state->peer_group;
                state->peer_group   = 0;
                state->type         = VFS_MOUNT_SLAVE;
            }
            break;
        case VFS_MOUNT_PRIVATE :
            state->type         = VFS_MOUNT_PRIVATE;
            state->peer_group   = 0;
            state->master_group = 0;
            break;
        default :
            state->type         = VFS_MOUNT_UNBINDABLE;
            state->peer_group   = 0;
            state->master_group = 0;
            break;
    }
}

/*
 * Propagation of a mount created over `dest`.  `source` is NULL for a fresh
 * filesystem, which the table treats as private; a NULL `dest` means the
 * namespace root, which is shared only after an explicit make-shared.
 */
static void vfs_propagation_bind(const vfs_propagation_t *source, const vfs_propagation_t *dest, vfs_propagation_t *result)
{
    uint32_t src          = source ? source->type : VFS_MOUNT_PRIVATE;
    bool     dest_shared  = dest && dest->type == VFS_MOUNT_SHARED;

    result->type         = VFS_MOUNT_PRIVATE;
    result->peer_group   = 0;
    result->master_group = source ? source->master_group : 0;

    if (src == VFS_MOUNT_SHARED) {
        /* The new mount joins the source's peer group. */
        result->type       = VFS_MOUNT_SHARED;
        result->peer_group = source->peer_group;
    } else if (src == VFS_MOUNT_SLAVE) {
        result->type = VFS_MOUNT_SLAVE;
        /* Slaving onto a shared destination keeps receiving and starts sharing. */
        if (dest_shared) result->peer_group = vfs_new_peer_group();
    } else if (dest_shared) {
        result->type       = VFS_MOUNT_SHARED;
        result->peer_group = vfs_new_peer_group();
    }
}

/*
 * Propagation of a moved mount: the move keeps its own type and only merges the
 * destination's peer group, so bind's "invalid on an unbindable source" does
 * not apply.  Returns -EINVAL when the source cannot land on a shared mount.
 */
static int vfs_propagation_move(const vfs_propagation_t *source, const vfs_propagation_t *dest, vfs_propagation_t *result)
{
    bool dest_shared = dest && dest->type == VFS_MOUNT_SHARED;

    *result = *source;
    if (source->type == VFS_MOUNT_UNBINDABLE) return dest_shared ? -EINVAL : EOK;
    if (dest_shared && !result->peer_group) {
        result->peer_group = vfs_new_peer_group();
        result->type = VFS_MOUNT_SHARED;
    }
    return EOK;
}

static vfs_node_t vfs_path_parent(vfs_node_t node)
{
    return vfs_path_parent_table(vfs_mount_table(mnt_namespace_current(), false), node);
}

/* Detached descriptors retain the final policy until their last close. */
static void vfs_retire_policy_locked(vfs_mount_attachment_t *entry)
{
    if (!entry->open_files) { free(entry); return; }
    entry->covered = NULL;
    entry->object = NULL;
    entry->parent = NULL;
    entry->next = retired_policies;
    retired_policies = entry;
}

/* Each attachment keeps its covered dentry and its shared filesystem alive. */
static void vfs_mount_attachment_put_locked(vfs_mount_attachment_t *entry)
{
    vfs_peer_leave_locked(entry);
    if (entry->covered->mount_refs) entry->covered->mount_refs--;
    if (entry->covered->refcount) entry->covered->refcount--;
    vfs_mount_object_t *object = entry->object;
    if (!--object->references) {
        object->retired_next = retired_mounts;
        retired_mounts = object;
    }
    vfs_retire_policy_locked(entry);
}

int vfs_mntns_clone(mnt_namespace_t *source, mnt_namespace_t *target)
{
    if (!target) return -EINVAL;
    mutex_lock(&vfs_namespace_lock);
    vfs_mount_table_t *old = vfs_mount_table(source, true);
    vfs_mount_table_t *copy = calloc(1, sizeof(*copy));
    if (!old || !copy) { free(copy); mutex_unlock(&vfs_namespace_lock); return -ENOMEM; }
    copy->root_attributes = old->root_attributes;
    copy->generation = old->generation;
    copy->root_propagation  = old->root_propagation;
    copy->root_peer_group   = old->root_peer_group;
    copy->root_master_group = old->root_master_group;
    vfs_poll_source_init(&copy->poll_source);
    vfs_mount_attachment_t **tail = &copy->entries;
    for (vfs_mount_attachment_t *entry = old->entries; entry; entry = entry->next) {
        vfs_mount_attachment_t *new = malloc(sizeof(*new));
        if (!new) {
            while (copy->entries) {
                vfs_mount_attachment_t *next = copy->entries->next;
                vfs_mount_attachment_put_locked(copy->entries);
                copy->entries = next;
            }
            free(copy);
            mutex_unlock(&vfs_namespace_lock);
            return -ENOMEM;
        }
        *new = *entry;
        new->open_files = 0;
        new->next = NULL;
        new->id = __atomic_fetch_add(&vfs_next_mount_id, 1, __ATOMIC_RELAXED);
        new->covered->refcount++;
        new->covered->mount_refs++;
        new->object->references++;
        *tail = new;
        tail = &new->next;
    }
    /* Give the copy its own root mount, as the source namespace has one. */
    vfs_mount_root_init(copy);
    if (!copy->root) {
        while (copy->entries) {
            vfs_mount_attachment_t *next = copy->entries->next;
            vfs_mount_attachment_put_locked(copy->entries);
            copy->entries = next;
        }
        free(copy);
        mutex_unlock(&vfs_namespace_lock);
        return -ENOMEM;
    }
    copy->root->id = __atomic_fetch_add(&vfs_next_mount_id, 1, __ATOMIC_RELAXED);
    copy->root->attributes   = old->root->attributes;
    copy->root->propagation  = old->root->propagation;
    copy->root->peer_group   = old->root->peer_group;
    copy->root->master_group = old->root->master_group;
    copy->root->locked_attributes = old->root->locked_attributes;

    /*
     * A copied attachment must point into the copy: path resolution enters a
     * mount by matching (parent mount, mountpoint), so a link left aimed at the
     * source namespace's attachment would make the inherited mount invisible.
     */
    for (vfs_mount_attachment_t *entry = copy->entries; entry; entry = entry->next) {
        if (!entry->parent) continue;
        vfs_mount_attachment_t *want = entry->parent;
        if (want == old->root) {
            entry->parent = copy->root;
            continue;
        }
        for (vfs_mount_attachment_t *source_entry = old->entries, *copy_entry = copy->entries; source_entry && copy_entry;
             source_entry = source_entry->next, copy_entry = copy_entry->next)
            if (source_entry == want) {
                entry->parent = copy_entry;
                break;
            }
    }
    bool less_privileged = target->ns.owner != source->ns.owner;
    if (less_privileged) {
        for (vfs_mount_attachment_t *entry = copy->root; entry; entry = entry == copy->root ? copy->entries : entry->next) {
            entry->locked_attributes |= entry->attributes | MOUNT_FLAG_ATIME;
            entry->locked = entry != copy->root;
            if (entry->peer_group) {
                entry->master_group = entry->peer_group;
                entry->peer_group = 0;
                entry->propagation = VFS_MOUNT_SLAVE;
            }
        }
        copy->root_propagation = copy->root->propagation;
        copy->root_peer_group = copy->root->peer_group;
        copy->root_master_group = copy->root->master_group;
    }
    copy->namespace = target;
    copy->next_all = mount_tables;
    mount_tables = copy;
    target->root_mount = copy;
    mutex_unlock(&vfs_namespace_lock);
    return EOK;
}

/* Free a table's root mount; the child entries are released separately. */
static void vfs_mount_root_destroy(vfs_mount_table_t *table)
{
    vfs_mount_attachment_t *root = table ? table->root : NULL;
    if (!root) return;
    table->root = NULL;
    free(root->object);
    vfs_retire_policy_locked(root);
}

static void vfs_mount_reap(void);

void vfs_mntns_destroy(mnt_namespace_t *ns)
{
    if (!ns || ns == &init_mnt_ns) return;
    mutex_lock(&vfs_namespace_lock);
    vfs_mount_table_t *table = ns->root_mount;
    ns->root_mount = NULL;
    if (table) {
        vfs_mount_table_t **link = &mount_tables;
        while (*link && *link != table) link = &(*link)->next_all;
        if (*link) *link = table->next_all;
        while (table->entries) {
            vfs_mount_attachment_t *next = table->entries->next;
            vfs_mount_attachment_put_locked(table->entries);
            table->entries = next;
        }
        vfs_mount_root_destroy(table);
    }
    mutex_unlock(&vfs_namespace_lock);
    free(table);
    vfs_mount_reap();
}

uint64_t vfs_mount_generation_ns(mnt_namespace_t *ns)
{
    vfs_mount_table_t *table = vfs_mount_table(ns, false);
    return table ? __atomic_load_n(&table->generation, __ATOMIC_ACQUIRE) : mount_generation;
}

uint64_t vfs_mount_generation(void)
{
    return vfs_mount_generation_ns(mnt_namespace_current());
}

vfs_poll_source_t *vfs_mount_poll_source_ns(mnt_namespace_t *ns)
{
    vfs_mount_table_t *table = vfs_mount_table(ns, false);
    return table ? &table->poll_source : &mount_poll_source;
}

vfs_poll_source_t *vfs_mount_poll_source(void)
{
    return vfs_mount_poll_source_ns(mnt_namespace_current());
}

void vfs_mount_changed(void)
{
    vfs_mount_table_t *table = vfs_mount_table(mnt_namespace_current(), true);
    if (table) {
        __atomic_add_fetch(&table->generation, 1, __ATOMIC_RELEASE);
        vfs_poll_source_notify(&table->poll_source, 0x00a);
    } else {
        __atomic_add_fetch(&mount_generation, 1, __ATOMIC_RELEASE);
        vfs_poll_source_notify(&mount_poll_source, 0x00a);
    }
}

struct vfs_callback vfs_empty_callback;
vfs_callback_t      fs_callbacks[256] = {[0] = &vfs_empty_callback};
static const char  *fs_names[256];
static uint32_t     fs_flags[256];
static uint32_t     fs_magics[256];
static int          fs_nextid = 1;

/* Update a node's atime. */
static void vfs_touch_access(vfs_node_t node)
{
    if (!node) return;
    node->readtime = timer_realtime_seconds();
    vfs_icache_publish(node);
}

/* Update a node's mtime and ctime. */
static void vfs_touch_modify(vfs_node_t node)
{
    if (!node) return;
    int64_t now      = timer_realtime_seconds();
    node->writetime  = now;
    node->createtime = now;
    vfs_icache_publish(node);
}

/* Update a node's ctime. */
static void vfs_touch_change(vfs_node_t node)
{
    if (!node) return;
    node->createtime = timer_realtime_seconds();
    vfs_icache_publish(node);
}

/* A successful namespace removal changes the inode's link count immediately. */
static void vfs_inode_drop_link(vfs_node_t node)
{
    if (!node) return;
    if (node->nlink) node->nlink--;
    vfs_icache_publish(node);
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
    if (proc->fsuid == 0 && namespace_initial_root(proc)) {
        if ((access_mask & VFS_ACCESS_X) && !(node->type & file_dir) && !(node->mode & 0111)) return -EACCES;
        return 0;
    }
    if (proc->fsuid == node->owner && (node->mode & (access_mask << 6)) == (access_mask << 6)) return 0;
    if (process_in_group(proc, node->group) && (node->mode & (access_mask << 3)) == (access_mask << 3)) return 0;
    if ((node->mode & access_mask) == access_mask) return 0;
    return -EACCES;
}

/* VFS operation: access check. */
int vfs_access_check(vfs_node_t node, uint32_t access_mask)
{
    return vfs_access_check_process(node, access_mask, process_current());
}

/* Change a node's permission bits on behalf of a specific process. */
int vfs_chmod_process_at(vfs_node_t node, uint16_t mode, process_t *proc, uint64_t mount_id)
{
    if (!node || !proc) return -EINVAL;
    if (vfs_mount_is_readonly_at(node, mount_id)) return -EROFS;
    if (!namespace_initial_root(proc) && proc->fsuid != node->owner) return -EPERM;
    mode &= 07777;
    if (!namespace_initial_root(proc) && !process_in_group(proc, node->group)) mode &= (uint16_t)~02000;
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

int vfs_chmod_process(vfs_node_t node, uint16_t mode, process_t *proc)
{
    return vfs_chmod_process_at(node, mode, proc, 0);
}

int vfs_chown_process_at(vfs_node_t node, uint32_t owner, uint32_t group, process_t *proc, uint64_t mount_id)
{
    if (!node || !proc) return -EINVAL;
    if (vfs_mount_is_readonly_at(node, mount_id)) return -EROFS;

    bool change_owner = owner != UINT32_MAX;
    bool change_group = group != UINT32_MAX;
    if (!namespace_initial_root(proc)) {
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

int vfs_chown_process(vfs_node_t node, uint32_t owner, uint32_t group, process_t *proc)
{
    return vfs_chown_process_at(node, owner, group, proc, 0);
}

int vfs_set_times_process_at(vfs_node_t node, int64_t atime, int64_t mtime, uint32_t flags, process_t *proc, uint64_t mount_id)
{
    uint32_t which = flags & (VFS_SET_TIME_ATIME | VFS_SET_TIME_MTIME);
    if (!node || !proc) return -EINVAL;
    if (!which) return EOK;
    if (vfs_mount_is_readonly_at(node, mount_id)) return -EROFS;

    if (!namespace_initial_root(proc) && proc->fsuid != node->owner) {
        if (flags & VFS_SET_TIME_EXPLICIT) return -EPERM;
        if (vfs_access_check_process(node, VFS_ACCESS_W, proc) != EOK) return -EACCES;
    }

    if (which & VFS_SET_TIME_ATIME) node->readtime = atime;
    if (which & VFS_SET_TIME_MTIME) node->writetime = mtime;
    vfs_touch_change(node);
    inotify_notify(node, IN_ATTRIB);
    return EOK;
}

/* Default callback for filesystem slots with no registered operations */
int vfs_set_times_process(vfs_node_t node, int64_t atime, int64_t mtime, uint32_t flags, process_t *proc)
{
    return vfs_set_times_process_at(node, atime, mtime, flags, proc, 0);
}

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
    return node && (node->type & ~file_delete) == file_none && !(node->flags & (VFS_NODE_NOCACHE | VFS_NODE_VIRTUAL)) && node->handle && callbackof(node, read) != vfs_empty_callback.read;
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

/* Split an absolute path into a parent path copy and a leaf name pointer. */
int vfs_split_parent(const char *path, char *parent, size_t size, const char **leaf)
{
    const char *slash;
    size_t      length;

    if (!path || !parent || !size || path[0] != '/') return -EINVAL;
    slash = strrchr(path, '/');
    if (!slash || !slash[1]) return -ENOENT;
    length = slash == path ? 1 : (size_t)(slash - path);
    if (length + 1 > size) return -ENAMETOOLONG;

    memcpy(parent, path, length);
    parent[length] = '\0';
    if (leaf) *leaf = slash + 1;
    return EOK;
}

/* Open the parent directory of an absolute path, or NULL when there is none. */
vfs_node_t vfs_open_parent_of(const char *path)
{
    char parent[CONFIG_VFS_PATH_MAX];

    if (vfs_split_parent(path, parent, sizeof(parent), NULL) != EOK) return NULL;
    return vfs_open(parent);
}

/* Resolve a path against a base directory into an absolute path buffer. */
int vfs_resolve_path(const char *base, const char *path, char *resolved, size_t size)
{
    size_t out = 1;

    if (!base || !path || !resolved || size < 2 || base[0] != '/') return -EINVAL;

    /*
     * Pathname-taking syscalls reject an empty pathname unless the individual
     * syscall explicitly implements AT_EMPTY_PATH.  Treating it as the base
     * directory would make open("") and mkdir("") operate on cwd.
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
static int vfs_node_path_table(vfs_mount_table_t *table, vfs_node_t node, char *path, size_t size)
{
    size_t     len = 0;
    vfs_node_t cur;

    if (!node || !path || size < 2) return -EINVAL;
    for (cur = node; cur && vfs_path_parent_table(table, cur); cur = vfs_path_parent_table(table, cur)) len += strlen(vfs_path_name_table(table, cur)) + 1;
    if (!len) len = 1;
    if (len + 1 > size) return -ENAMETOOLONG;

    path[len] = '\0';
    if (len == 1) {
        path[0] = '/';
        return EOK;
    }

    size_t pos = len;
    for (cur = node; cur && vfs_path_parent_table(table, cur); cur = vfs_path_parent_table(table, cur)) {
        size_t name_len = strlen(vfs_path_name_table(table, cur));
        pos -= name_len;
        memcpy(path + pos, vfs_path_name_table(table, cur), name_len);
        path[--pos] = '/';
    }
    return EOK;
}

int vfs_node_path(vfs_node_t node, char *path, size_t size)
{
    return vfs_node_path_table(vfs_mount_table(mnt_namespace_current(), false), node, path, size);
}


/*
 * Join a base path with a relative one, tolerating a leading slash on either
 * side.  Returns how many bytes the result needs, like the other path helpers.
 */
static size_t vfs_path_join(char *out, size_t size, const char *base, const char *relative)
{
    size_t      base_len = strlen(base);
    bool        need_sep = base_len && base[base_len - 1] != '/';
    const char *rel      = relative;

    while (*rel == '/') rel++;
    size_t rel_len = strlen(rel);
    size_t used    = base_len + (need_sep && *rel ? 1 : 0) + rel_len;
    if (used + 1 > size) return used + 1;
    memcpy(out, base, base_len);
    size_t pos = base_len;
    if (need_sep && *rel) out[pos++] = '/';
    memcpy(out + pos, rel, rel_len);
    out[pos + rel_len] = '\0';
    return used + 1;
}

/*
 * The path `node` has inside one mount's own dentry tree, relative to `root`.
 * Unlike vfs_node_path_table() this never crosses a mount boundary, because the
 * two sides of a boundary name their files in different trees.
 */
static int vfs_dentry_path_within(vfs_node_t node, vfs_node_t root, char *out, size_t size)
{
    const char *names[VFS_MOUNT_MIRROR_MAX];
    unsigned    count  = 0;
    vfs_node_t  cursor = node;

    /* The root of this tree: nothing to walk.  Check before alias resolution,
     * which would otherwise walk up past a bind root and yield a bogus path. */
    if (node == root) {
        if (size < 2) return -ENAMETOOLONG;
        out[0] = '/';
        out[1] = '\0';
        return EOK;
    }

    /* A bind root is its own node that aliases the source subtree, and the
     * dentry parents lead to the source, not to the alias. */
    root = vfs_alias_resolve(root);

    while (cursor && cursor != root) {
        if (count == VFS_MOUNT_MIRROR_MAX || !cursor->name) return -EXDEV;
        names[count++] = cursor->name;
        cursor = cursor->parent;
    }
    if (cursor != root) return -EXDEV;

    size_t used = 0;
    if (size < 2) return -ENAMETOOLONG;
    out[used++] = '/';
    out[used]   = '\0';
    while (count) {
        const char *name = names[--count];
        size_t      len  = strlen(name);
        if (used + len + 1 >= size) return -ENAMETOOLONG;
        if (used > 1) out[used++] = '/';
        memcpy(out + used, name, len);
        used += len;
        out[used] = '\0';
    }
    return EOK;
}

/*
 * The namespace path of an attachment, composed mount by mount the way
 * __d_path()/prepend_path() do:
 *     path(m) = join(path(m->parent), relpath(m->parent->root, m->covered))
 * Computing it here instead of caching it at attach time keeps it correct when
 * a parent mount is later moved, and it never goes stale.
 */
static int vfs_mount_mountpoint_path(const vfs_mount_table_t *table, const vfs_mount_attachment_t *mount, char *out, size_t size)
{
    const vfs_mount_attachment_t *chain[VFS_MOUNT_MIRROR_MAX];
    unsigned                      depth = 0;

    if (size < 2) return -ENAMETOOLONG;
    out[0] = '/';
    out[1] = '\0';
    for (const vfs_mount_attachment_t *m = mount; m && m != table->root && m->parent && depth < VFS_MOUNT_MIRROR_MAX; m = m->parent) chain[depth++] = m;

    /* Compose from the outermost mount inwards, as __prepend_path() does. */
    while (depth--) {
        char relative[VFS_MOUNT_PATH_MAX];
        int  result = vfs_dentry_path_within(chain[depth]->covered, chain[depth]->parent->object->root, relative, sizeof(relative));
        if (result != EOK) return result;
        if (vfs_path_join(out, size, out, relative) > size) return -ENAMETOOLONG;
    }
    return EOK;
}

/*
 * The path a node has in this namespace.  A bind mount aliases the source's
 * dentries instead of copying the tree, so the dentry walk alone would name the
 * source's path; composing the enclosing mount's own mountpoint with the node's
 * position inside it recovers the namespace's view.
 */
static int vfs_namespace_path_table(vfs_mount_table_t *table, vfs_node_t node, char *out, size_t size)
{
    char relative[CONFIG_VFS_PATH_MAX];
    char base[CONFIG_VFS_PATH_MAX];
    int  r;

    if (!table || !table->root) return vfs_node_path_table(table, node, out, size);
    vfs_mount_attachment_t *mount = vfs_mount_containing(table, node);
    if (!mount || mount == table->root) return vfs_node_path_table(table, node, out, size);

    r = vfs_dentry_path_within(node, mount->object->root, relative, sizeof(relative));
    if (r != EOK) return vfs_node_path_table(table, node, out, size);
    r = vfs_mount_mountpoint_path(table, mount, base, sizeof(base));
    if (r != EOK) return vfs_node_path_table(table, node, out, size);
    if (vfs_path_join(out, size, base, relative) > size) return -ENAMETOOLONG;
    return EOK;
}

int vfs_node_path_at(vfs_node_t node, uint64_t mount_id, char *path, size_t size)
{
    mutex_lock(&vfs_namespace_lock);
    vfs_mount_table_t *table = vfs_mount_table(mnt_namespace_current(), true);
    vfs_mount_attachment_t *mount = table && table->root->id == mount_id ? table->root : NULL;
    for (vfs_mount_attachment_t *entry = table ? table->entries : NULL; !mount && entry; entry = entry->next)
        if (entry->id == mount_id) mount = entry;
    int result;
    if (mount) {
        char relative[VFS_MOUNT_PATH_MAX];
        result = vfs_dentry_path_within(node, mount->object->root, relative, sizeof(relative));
        if (result == EOK) result = vfs_mount_mountpoint_path(table, mount, path, size);
        if (result == EOK && vfs_path_join(path, size, path, relative) > size) result = -ENAMETOOLONG;
    } else {
        result = vfs_node_path_table(table, node, path, size);
    }
    mutex_unlock(&vfs_namespace_lock);
    return result;
}

/* The namespace path of a mountpoint dentry, for an attachment to record. */
static char *vfs_mountpoint_of(vfs_mount_table_t *table, vfs_node_t target)
{
    char path[CONFIG_VFS_PATH_MAX];
    if (vfs_namespace_path_table(table, target, path, sizeof(path)) != EOK) return NULL;
    return strdup(path);
}

/* The namespace path a mirrored submount takes under its new parent mount. */
static char *vfs_mountpoint_under(const char *base, const char *relative)
{
    size_t needed = vfs_path_join(NULL, 0, base, relative);
    char  *joined = malloc(needed);
    if (!joined) return NULL;
    if (vfs_path_join(joined, needed, base, relative) > needed) {
        free(joined);
        return NULL;
    }
    return joined;
}

/* Build the absolute path of a node into a malloc'd buffer. */
static char *vfs_node_absolute_path(vfs_node_t node)
{
    char *resolved = malloc(CONFIG_VFS_PATH_MAX);
    if (!resolved) return NULL;
    if (vfs_node_path(node, resolved, CONFIG_VFS_PATH_MAX) != EOK) { free(resolved); return NULL; }
    return resolved;
}

/* Resolve a symlink node to its absolute target path. */
static char *vfs_resolve_link_path(vfs_node_t node)
{
    char       *path;
    process_t  *proc;
    char        dynamic_target[CONFIG_VFS_PATH_MAX];
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
        char resolved[CONFIG_VFS_PATH_MAX];
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
    if (base_len + link_len + 2 > CONFIG_VFS_PATH_MAX) {
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

/* VFS operation: open internal. */
static vfs_node_t vfs_open_internal(const char *str, int symlink_depth, bool follow_final, int *error, vfs_mount_attachment_t **mount_out);

/* Open a file or directory, invoking the appropriate callback */
static void do_open(vfs_node_t file)
{
    bool authoritative = true;
    if (file->handle) {
        authoritative = callbackof(file, stat)(file->handle, file) == EOK;
    } else {
        callbackof(file, open)(file->parent->handle, file->name, file);
    }
    if (file->handle && file->inode) {
        if (authoritative) {
            (void)vfs_icache_refresh(file);
        } else {
            (void)vfs_icache_bind(file);
        }
    }
}

/* Update a file or directory node, if necessary */
static void do_update(vfs_node_t file)
{
    if (file->type & file_none || !file->handle || file->type & file_dir || file->type & file_symlink || file->type & file_pipe) do_open(file);
    if (file->mapping) file->size = pagecache_size(file->mapping);
    vfs_icache_publish(file);
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
    vfs_node_t             node   = NULL;
    parent                        = vfs_alias_resolve(parent);
    enum vfs_dcache_result cached = vfs_dcache_lookup(parent, name, &node);
    if (cached == VFS_DCACHE_POSITIVE) return net_sysfs_node_visible(node) ? node : NULL;
    if (cached == VFS_DCACHE_NEGATIVE) return NULL;

    node = clist_first(parent->child, data,
                       !(((vfs_node_t)data)->flags & (VFS_NODE_FINALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_INITIALIZING)) && !(((vfs_node_t)data)->type & file_delete)
                           && streq(name, ((vfs_node_t)data)->name));
    if (node) {
        vfs_dcache_add(node);
    } else {
        vfs_dcache_add_negative(parent, name);
    }
    return node && net_sysfs_node_visible(node) ? node : NULL;
}

/*
 * Creation must also see entries which are not published yet.  Otherwise two
 * concurrent creators can both pass lookup and hand duplicate names to the
 * backing filesystem.  Deleted entries are deliberately ignored: POSIX
 * permits a name to be reused while an unlinked inode is still open.
 */
static vfs_node_t vfs_child_find_reserved(vfs_node_t parent, const char *name)
{
    parent = vfs_alias_resolve(parent);
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

    /*
     * A bind root owns no subtree of its own, and every lookup aliases away from
     * it.  A child created through one has to land in the tree those lookups
     * reach, otherwise mkdir through a bind reports success and the entry is
     * invisible.
     */
    parent = vfs_alias_resolve(parent);

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
    node->nlink             = 1;
    node->refcount          = 0;
    node->dcache_generation = 1;
    node->blksz             = PAGE_4K_SIZE;
    node->mode              = 0777;
    node->linkto            = 0;
    node->createtime = node->readtime = node->writetime = timer_realtime_seconds();
    vfs_poll_source_init(&node->poll_source);

    if (parent) {
        parent->child = clist_prepend(parent->child, node);

        /* The node is not necessarily initialized yet; only kill a stale miss. */
        vfs_dcache_invalidate(parent, node->name);
    }
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
    vfs_node_t node = NULL;
    if (vfs_dcache_lookup(dir, name, &node) == VFS_DCACHE_POSITIVE) return net_sysfs_node_visible(node) ? node : NULL;
    node = clist_first(dir->child, data,
                       !(((vfs_node_t)data)->flags & (VFS_NODE_FINALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_INITIALIZING)) && !(((vfs_node_t)data)->type & file_delete)
                           && streq(name, ((vfs_node_t)data)->name));
    if (node) vfs_dcache_add(node);
    return node && net_sysfs_node_visible(node) ? node : NULL;
}

/* Update a file or directory, ensuring it is open and ready */
void vfs_update(vfs_node_t node)
{
    if (!node) return;
    mutex_lock(&vfs_namespace_lock);
    do_update(node);
    mutex_unlock(&vfs_namespace_lock);
}

/* Open a file or directory by path */
static vfs_node_t vfs_open_internal(const char *str, int symlink_depth, bool follow_final, int *error, vfs_mount_attachment_t **mount_out)
{
    if (mount_out) *mount_out = NULL;
    vfs_node_t owned_reference = NULL;
    bool       trailing_slash;

    /*
     * For PID1, /proc/1/root must be same as /.  The procfs symlink for pid 1's
     * root is correctly created as a symlink to "/", but the VFS lookup for the
     * intermediate "1" directory may be considered a different mount if the
     * dcache for "1" under /proc is stale. Handle the full path directly.
     */
    process_t *p = process_current();
    if (p && p->task && p->task->pid == 1) {
        if (str && (streq(str, "/proc/1/root") || streq(str, "/proc/self/root") || streq(str, "/proc/1/root/") || streq(str, "/proc/self/root/"))) str = "/";
    }

    if (error) *error = -ENOENT;
    if (!str || str[0] != '/') {
        if (error) *error = -EINVAL;
        return 0;
    }
    if (symlink_depth > 40) {
        if (error) *error = -ELOOP;
        return 0;
    }
    trailing_slash = str[1] != '\0' && str[strlen(str) - 1] == '/';
    vfs_mount_table_t *walk_table = vfs_mount_table(mnt_namespace_current(), true);
    vfs_mount_attachment_t *cur_mnt = walk_table ? walk_table->root : NULL;
    vfs_node_t current = cur_mnt ? cur_mnt->object->root : rootdir;
    for (unsigned depth = 0; cur_mnt && depth < 40; depth++) {
        vfs_mount_attachment_t *mounted = vfs_mount_lookup(walk_table, cur_mnt, current);
        if (!mounted) break;
        cur_mnt = mounted;
        current = mounted->object->root;
    }
    if (str[1] == '\0') {
        current->refcount++;
        if (error) *error = EOK;
        if (mount_out) *mount_out = cur_mnt;
        return current;
    }

    char *path = strdup(str + 1);
    if (!path) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("vfs: Path allocation failed while resolving %s\n", str);
        if (error) *error = -ENOMEM;
        return 0;
    }

    char                    *save_ptr = path;

    for (char *buf = pathtok(&save_ptr); buf; buf = pathtok(&save_ptr)) {
        if (!current) {
            if (error) *error = -ENOENT;
            goto err;
        }
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
            vfs_node_t next;
            if (cur_mnt && current == cur_mnt->object->root && cur_mnt->parent) {
                /* At a mount root: climb out above the mountpoint. */
                vfs_node_t mountpoint = cur_mnt->covered;
                cur_mnt               = cur_mnt->parent;
                next                  = vfs_path_parent_table(walk_table, mountpoint);
                if (!next) next = cur_mnt->object->root;
            } else {
                next = vfs_path_parent_table(walk_table, current);
                if (!next) next = current;
            }
            if (owned_reference && owned_reference != next && owned_reference->refcount) owned_reference->refcount--;
            owned_reference = owned_reference == next ? owned_reference : NULL;
            current         = next;
            continue;
        }

        vfs_node_t next = vfs_child_find(current, buf);
        /* Enter the topmost mount stacked at this name, if any. */
        if (next && cur_mnt) {
            for (unsigned depth = 0; depth < 40; depth++) {
                vfs_mount_attachment_t *mounted = vfs_mount_lookup(walk_table, cur_mnt, next);
                if (!mounted || mounted->object->root == next) break;
                cur_mnt = mounted;
                next    = mounted->object->root;
            }
        }
        if (!next) {
            if (error) *error = -ENOENT;
            goto err;
        }
        if (owned_reference && owned_reference->refcount) owned_reference->refcount--;
        owned_reference = NULL;
        current         = next;

        do_update(current);
        if ((current->type & file_symlink) && (follow_final || trailing_slash || *save_ptr != '\0')) {
            if (callbackof(current, follow_link) != vfs_empty_callback.follow_link) {
                /* A magic link may drop process references and enter VFS again. */
                current->refcount++;
                vfs_node_t link = current;
                mutex_unlock(&vfs_namespace_lock);
                vfs_node_t target = callbackof(link, follow_link)(link);
                char *fallback = target ? NULL : vfs_resolve_link_path(link);
                vfs_close(link);
                mutex_lock(&vfs_namespace_lock);
                if (!target && fallback) target = vfs_open_internal(fallback, symlink_depth + 1, true, error, &cur_mnt);
                else if (target) {
                    /* A magic link jumps into its target's mount. */
                    cur_mnt = vfs_mount_containing(walk_table, target);
                    for (unsigned depth = 0; cur_mnt && depth < 40; depth++) {
                        vfs_mount_attachment_t *mounted = vfs_mount_lookup(walk_table, cur_mnt, target);
                        if (!mounted || mounted->object->root == target) break;
                        if (target->refcount) target->refcount--;
                        cur_mnt = mounted;
                        target  = mounted->object->root;
                        target->refcount++;
                    }
                }
                free(fallback);
                if (!target) goto err;
                current = owned_reference = target;
                continue;
            }
            char      *target_path = vfs_resolve_link_path(current);
            vfs_node_t target;

            if (!target_path) goto err;
            target = vfs_open_internal(target_path, symlink_depth + 1, true, error, &cur_mnt);
            free(target_path);
            if (!target) goto err;

            current         = target;
            owned_reference = target;
            continue;
        }
    }
    if (!current) {
        if (error) *error = -ENOENT;
        goto err;
    }
    if (trailing_slash && !(current->type & file_dir)) {
        if (error) *error = -ENOTDIR;
        goto err;
    }
    if (!owned_reference) current->refcount++;
    free(path);
    if (error) *error = EOK;
    if (mount_out) *mount_out = cur_mnt;
    return current;
err:
    if (owned_reference && owned_reference->refcount) owned_reference->refcount--;
    free(path);
    return 0;
}

/* Resolve a path and report the mount the walk arrived through. */
vfs_node_t vfs_open_mount(const char *str, uint64_t *mount_out)
{
    return vfs_open_checked_at(str, false, NULL, mount_out);
}

void vfs_mount_identity(vfs_node_t node, uint64_t arrival_id, uint64_t *id, bool *is_root)
{
    mutex_lock(&vfs_namespace_lock);
    vfs_mount_table_t *table = vfs_mount_table(mnt_namespace_current(), true);
    vfs_mount_attachment_t *mount = table && table->root->id == arrival_id ? table->root : NULL;
    for (vfs_mount_attachment_t *entry = table ? table->entries : NULL; !mount && entry; entry = entry->next)
        if (entry->id == arrival_id) mount = entry;
    if (!mount && table) mount = vfs_mount_containing(table, node);
    *id = mount ? mount->id : 0;
    *is_root = mount && node == mount->object->root;
    mutex_unlock(&vfs_namespace_lock);
}

vfs_node_t vfs_open_checked_at(const char *path, bool nofollow, int *error, uint64_t *mount_id)
{
    mutex_lock(&vfs_namespace_lock);
    vfs_mount_attachment_t *mount = NULL;
    vfs_node_t node = vfs_open_internal(path, 0, !nofollow, error, &mount);
    if (mount_id) *mount_id = mount ? mount->id : 0;
    mutex_unlock(&vfs_namespace_lock);
    return node;
}

void vfs_mount_open_ref(uint64_t id, bool acquire)
{
    if (!id) return;
    mutex_lock(&vfs_namespace_lock);
    for (vfs_mount_table_t *table = mount_tables; table; table = table->next_all) {
        for (vfs_mount_attachment_t *entry = table->root; entry; entry = entry == table->root ? table->entries : entry->next) {
            if (entry->id != id) continue;
            if (acquire) entry->open_files++;
            else if (entry->open_files) entry->open_files--;
            mutex_unlock(&vfs_namespace_lock);
            return;
        }
    }
    vfs_mount_attachment_t **link = &retired_policies;
    while (*link) {
        vfs_mount_attachment_t *entry = *link;
        if (entry->id != id) { link = &entry->next; continue; }
        if (acquire) entry->open_files++;
        else if (entry->open_files) entry->open_files--;
        if (!entry->open_files) { *link = entry->next; free(entry); }
        break;
    }
    mutex_unlock(&vfs_namespace_lock);
}

uint64_t vfs_mount_flags_id(uint64_t id)
{
    uint64_t flags = 0;
    mutex_lock(&vfs_namespace_lock);
    for (vfs_mount_table_t *table = mount_tables; table; table = table->next_all)
        for (vfs_mount_attachment_t *entry = table->root; entry; entry = entry == table->root ? table->entries : entry->next)
            if (entry->id == id) flags = entry->attributes;
    for (vfs_mount_attachment_t *entry = retired_policies; entry; entry = entry->next)
        if (entry->id == id) flags = entry->attributes;
    mutex_unlock(&vfs_namespace_lock);
    return flags;
}

/* Open a file or directory by path. */
vfs_node_t vfs_open(const char *str)
{
    mutex_lock(&vfs_namespace_lock);
    vfs_node_t node = vfs_open_internal(str, 0, true, NULL, NULL);
    mutex_unlock(&vfs_namespace_lock);
    return node;
}

/* Open a file or directory by path, reporting lookup errors. */
vfs_node_t vfs_open_checked(const char *str, int *error)
{
    mutex_lock(&vfs_namespace_lock);
    vfs_node_t node = vfs_open_internal(str, 0, true, error, NULL);
    mutex_unlock(&vfs_namespace_lock);
    return node;
}

/* Open a path without following the final symlink component. */
vfs_node_t vfs_open_nofollow(const char *str)
{
    mutex_lock(&vfs_namespace_lock);
    vfs_node_t node = vfs_open_internal(str, 0, false, NULL, NULL);
    mutex_unlock(&vfs_namespace_lock);
    return node;
}

/* Open a path without following the final symlink, reporting errors. */
vfs_node_t vfs_open_nofollow_checked(const char *str, int *error)
{
    mutex_lock(&vfs_namespace_lock);
    vfs_node_t node = vfs_open_internal(str, 0, false, error, NULL);
    mutex_unlock(&vfs_namespace_lock);
    return node;
}

/* Retain a node reference, refusing nodes already finalizing. */
vfs_node_t vfs_node_retain(vfs_node_t node)
{
    if (!node) return NULL;
    mutex_lock(&vfs_namespace_lock);
    if (node->flags & VFS_NODE_FINALIZING) node = NULL;
    if (node) node->refcount++;
    mutex_unlock(&vfs_namespace_lock);
    return node;
}

/*
 * Resolve the existing parent of a creation pathname.  Creation is
 * non-recursive: mkdir/open/link/symlink never manufacture missing parent
 * directories as a side effect.
 */
static int vfs_prepare_create(const char *name, bool allow_trailing_slash, char **storage, char **leaf, vfs_node_t *parent)
{
    if (!name || !storage || !leaf || !parent) return -EINVAL;
    if (!name[0]) return -ENOENT;
    if (name[0] != '/') return -EINVAL;

    size_t length = strlen(name);
    if (length >= CONFIG_VFS_PATH_MAX) return -ENAMETOOLONG;
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

    int error = EOK;
    uint64_t mount_id = 0;
    vfs_node_t dir = vfs_open_checked_at(parent_path, false, &error, &mount_id);
    if (!dir) {
        free(path);
        return -ENOENT;
    }
    if (!(dir->type & file_dir)) {
        vfs_close(dir);
        free(path);
        return -ENOTDIR;
    }
    if (vfs_mount_flags_id(mount_id) & MOUNT_FLAG_RDONLY) {
        vfs_close(dir);
        free(path);
        return -EROFS;
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
    mutex_lock(&vfs_namespace_lock);
    vfs_dcache_remove(node);
    parent->child = clist_delete(parent->child, node);
    node->flags |= VFS_NODE_UNLINKED;
    vfs_dcache_add_negative(parent, node->name);
    mutex_unlock(&vfs_namespace_lock);
    vfs_free(node);
}

/* Reserve a child name to serialize concurrent creators. */
static vfs_node_t vfs_reserve_child(vfs_node_t parent, const char *name, int *status)
{
    if (status) *status = -ENOMEM;
    mutex_lock(&vfs_namespace_lock);
    if (parent->flags & VFS_NODE_RENAME_BUSY) {
        mutex_unlock(&vfs_namespace_lock);
        if (status) *status = -EBUSY;
        return NULL;
    }
    if (vfs_child_find_reserved(parent, name)) {
        mutex_unlock(&vfs_namespace_lock);
        if (status) *status = -EEXIST;
        return NULL;
    }
    vfs_node_t node = vfs_child_append(parent, name, NULL);
    if (node) node->flags |= VFS_NODE_INITIALIZING;
    mutex_unlock(&vfs_namespace_lock);
    if (node && status) *status = EOK;
    return node;
}

/* Make a reserved child visible to concurrent lookups. */
static void vfs_publish_child(vfs_node_t node)
{
    mutex_lock(&vfs_namespace_lock);
    node->flags &= ~VFS_NODE_INITIALIZING;
    vfs_dcache_invalidate(node->parent, node->name);
    vfs_dcache_add(node);
    mutex_unlock(&vfs_namespace_lock);
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

/* VFS operation: mkdir. */
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
        vfs_abort_created_node(parent, node);
    } else {
        (void)vfs_icache_refresh(node);
        vfs_touch_modify(parent);
        vfs_publish_child(node);
        inotify_notify_create(parent, node);
    }
out:
    vfs_close(parent);
    free(path);
    return status;
}

/* VFS operation: mkfile. */
int vfs_mkfile(const char *name)
{
    return vfs_mkfile_mode(name, 0666);
}

/* Read a directory entry by index from the specified directory node */
int vfs_readdir(vfs_node_t dir, size_t index, vfs_dirent_t *entry)
{
    if (!dir || !entry) return -EINVAL;
    dir = vfs_alias_resolve(dir);
    mutex_lock(&vfs_namespace_lock);

    /*
     * A pathname open already refreshes the directory.  Refresh once again
     * at the start of an enumeration to pick up dynamic procfs/sysfs entries,
     * but never once per returned entry: sysfs_stat() walks and de-duplicates
     * all children, so doing that from every getdents64 loop iteration turns
     * udev's parallel tree walk into an O(entries^2) global-spinlock storm.
     */
    if (index == 0) do_update(dir);
    if (!(dir->type & file_dir)) {
        mutex_unlock(&vfs_namespace_lock);
        return -ENOTDIR;
    }

    vfs_node_t child   = NULL;
    size_t     visible = 0;
    for (clist_t list = dir->child; list; list = list->next) {
        vfs_node_t candidate = list->data;
        if (!candidate || (candidate->flags & (VFS_NODE_FINALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_INITIALIZING)) || (candidate->type & file_delete) || !net_sysfs_node_visible(candidate)) continue;
        if (visible++ == index) {
            child = candidate;
            break;
        }
    }
    if (!child) {
        mutex_unlock(&vfs_namespace_lock);
        return -ENOENT;
    }

    size_t name_length = strlen(child->name);
    if (name_length > VFS_NAME_MAX) {
        mutex_unlock(&vfs_namespace_lock);
        return -ENAMETOOLONG;
    }
    memcpy(entry->name, child->name, name_length + 1);
    entry->type  = child->type;
    entry->size  = child->size;
    entry->inode = child->inode;
    mutex_unlock(&vfs_namespace_lock);
    if (index == 0) vfs_touch_access(dir);
    inotify_notify(dir, IN_ACCESS);
    return EOK;
}

/* Emit a directory range with one refresh, one lock acquisition and one walk. */
int vfs_readdir_batch(vfs_node_t dir, size_t start_index, vfs_readdir_emit_t emit, void *context, size_t *next_index)
{
    if (!dir || !emit || !next_index) return -EINVAL;

    dir         = vfs_alias_resolve(dir);
    *next_index = start_index;
    mutex_lock(&vfs_namespace_lock);
    if (start_index == 0) do_update(dir);
    if (!(dir->type & file_dir)) {
        mutex_unlock(&vfs_namespace_lock);
        return -ENOTDIR;
    }

    size_t visible = 0;
    size_t emitted = 0;
    int    status  = EOK;
    for (clist_t list = dir->child; list; list = list->next) {
        vfs_node_t child = list->data;
        if (!child || (child->flags & (VFS_NODE_FINALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_INITIALIZING)) || (child->type & file_delete) || !net_sysfs_node_visible(child)) continue;

        size_t current_index = visible++;
        if (current_index < start_index) continue;

        size_t name_length = strlen(child->name);
        if (name_length > VFS_NAME_MAX) {
            status = -ENAMETOOLONG;
            break;
        }

        vfs_dirent_t entry;
        memcpy(entry.name, child->name, name_length + 1);
        entry.type  = child->type;
        entry.size  = child->size;
        entry.inode = child->inode;
        if (!emit(&entry, current_index + 1, context)) break;

        *next_index = current_index + 1;
        emitted++;
    }
    mutex_unlock(&vfs_namespace_lock);

    if (emitted && start_index == 0) vfs_touch_access(dir);
    if (emitted) inotify_notify(dir, IN_ACCESS);
    return status;
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
    char        resolved_target[CONFIG_VFS_PATH_MAX];
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
        (void)vfs_icache_refresh(node);
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
        (void)vfs_icache_refresh(node);
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

/*
 * statfs f_type of each filesystem type, keyed by the name it registers under.
 * The aliases registered by tmpfs.c are listed explicitly so no filesystem can
 * be reported as some other type.
 */
static const struct {
        const char *name;
        uint32_t    magic;
} fs_magic_table[] = {
    {"tmpfs",       TMPFS_MAGIC          },
    {"devtmpfs",    TMPFS_MAGIC          },
    {"memfd",       TMPFS_MAGIC          },
    {"securityfs",  SECURITYFS_MAGIC     },
    {"selinuxfs",   SELINUX_MAGIC        },
    {"bpf",         BPF_FS_MAGIC         },
    {"bpffs",       BPF_FS_MAGIC         },
    {"debugfs",     DEBUGFS_MAGIC        },
    {"tracefs",     TRACEFS_MAGIC        },
    {"hugetlbfs",   HUGETLBFS_MAGIC      },
    {"mqueue",      MQUEUE_MAGIC         }, // not uapi: ipc/mqueue.c
    {"fusectl",     FUSE_CTL_SUPER_MAGIC }, // not uapi: fs/fuse/control.c
    {"configfs",    CONFIGFS_MAGIC       },
    {"binfmt_misc", BINFMTFS_MAGIC       },
    {"autofs",      AUTOFS_SUPER_MAGIC   },
    {"efivarfs",    EFIVARFS_MAGIC       },
    {"ramfs",       RAMFS_MAGIC          },
    {"devpts",      DEVPTS_SUPER_MAGIC   },
    {"pstore",      PSTOREFS_MAGIC       },
    {"cgroup",      CGROUP_SUPER_MAGIC   },
    {"nsfs",        NSFS_MAGIC           },
    {"overlay",     OVERLAYFS_SUPER_MAGIC},
    {"fuse",        FUSE_SUPER_MAGIC     },
    {"sysfs",       SYSFS_MAGIC          },
    {"proc",        PROC_SUPER_MAGIC     },
    {"cgroup2",     CGROUP2_SUPER_MAGIC  },
    {"isofs",       ISOFS_SUPER_MAGIC    },
    {"extfs",       EXT4_SUPER_MAGIC     },
    {"fatfs",       MSDOS_SUPER_MAGIC    },
    {"ntfs",        NTFS_SB_MAGIC        },
    {"sockfs",      SOCKFS_MAGIC         },
    {"pipefs",      PIPEFS_MAGIC         },
    {"pidfd",       PID_FS_MAGIC         },
    {"epoll",       ANON_INODE_FS_MAGIC  },
    {"inotify",     ANON_INODE_FS_MAGIC  },
    {"eventfd",     ANON_INODE_FS_MAGIC  },
    {"signalfd",    ANON_INODE_FS_MAGIC  },
    {"timerfd",     ANON_INODE_FS_MAGIC  },
    {"seccomp",     ANON_INODE_FS_MAGIC  },
    {"posix_mq",    MQUEUE_MAGIC         },
};

/* Return the statfs magic a filesystem type registers under, 0 when unknown. */
static uint32_t fs_magic_for_name(const char *name)
{
    if (!name) return 0;
    for (size_t i = 0; i < sizeof(fs_magic_table) / sizeof(fs_magic_table[0]); i++)
        if (streq(fs_magic_table[i].name, name)) return fs_magic_table[i].magic;
    return 0;
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
    fs_magics[id]    = fs_magic_for_name(name);
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

/* Return the statfs f_type of a registered filesystem, 0 when it has none. */
uint32_t vfs_filesystem_magic(uint16_t fsid)
{
    if (!fsid || fsid >= (uint16_t)fs_nextid) return 0;
    return fs_magics[fsid];
}

int vfs_set_filesystem_magic(uint16_t fsid, uint32_t magic)
{
    if (!fsid || fsid >= (uint16_t)fs_nextid) return -EINVAL;
    fs_magics[fsid] = magic;
    return EOK;
}

uint64_t vfs_mount_flags(vfs_node_t node)
{
    /*
     * Ask which mount owns the dentry.  Turning the node back into a path and
     * re-resolving it used to answer a different question -- the mount the
     * *path* names, not the one the caller reached the node through -- so a
     * node under a bind could report some other mount's read-only attribute.
     */
    uint64_t attributes = 0;
    mutex_lock(&vfs_namespace_lock);
    vfs_mount_table_t *table = vfs_mount_table(mnt_namespace_current(), false);
    vfs_mount_attachment_t *mount = table ? vfs_mount_containing(table, node) : NULL;
    if (mount) attributes = mount->attributes;
    mutex_unlock(&vfs_namespace_lock);
    return attributes;
}

bool vfs_is_mountpoint(vfs_node_t node)
{
    mutex_lock(&vfs_namespace_lock);
    bool mounted = node && (node == rootdir || vfs_mount_attachment(vfs_mount_table(mnt_namespace_current(), false), node));
    mutex_unlock(&vfs_namespace_lock);
    return mounted;
}

static uint64_t vfs_new_mount_id(void)
{
    uint64_t id = __atomic_fetch_add(&vfs_next_mount_id, 1, __ATOMIC_RELAXED);
    return id ? id : __atomic_fetch_add(&vfs_next_mount_id, 1, __ATOMIC_RELAXED);
}

/* Namespace handles may be bound to a regular file (for example /run/netns/x). */
int vfs_namespace_bind(vfs_node_t source, vfs_node_t target, uint64_t flags)
{
    if (!source || !target || source == target) return -EINVAL;
    if (flags & 16384ULL) return -EOPNOTSUPP; /* MS_REC needs a subtree clone. */
    if ((source->type & file_dir) || (target->type & file_dir)) return -EOPNOTSUPP;
    if (vfs_filesystem_magic(source->fsid) != 0x6e736673U) return -EOPNOTSUPP;
    vfs_mount_object_t *object = calloc(1, sizeof(*object));
    vfs_mount_attachment_t *entry = calloc(1, sizeof(*entry));
    if (!object || !entry) { free(object); free(entry); return -ENOMEM; }
    mutex_lock(&vfs_namespace_lock);
    vfs_mount_table_t *table = vfs_mount_table(mnt_namespace_current(), true);
    if (!table || vfs_mount_attachment(table, target) || (target->flags & (VFS_NODE_UNLINKED | VFS_NODE_FINALIZING))) {
        mutex_unlock(&vfs_namespace_lock);
        free(object); free(entry);
        return table ? -EBUSY : -ENOMEM;
    }
    source->refcount++;
    target->refcount++;
    target->mount_refs++;
    object->root = source;
    object->references = 1;
    entry->object = object;
    entry->covered = target;
    entry->attributes = MOUNT_FLAG_RDONLY | MOUNT_FLAG_NOSUID | MOUNT_FLAG_NODEV | MOUNT_FLAG_NOEXEC;
    entry->id = vfs_new_mount_id();
    entry->parent = vfs_mount_containing(table, target);
    {
        vfs_propagation_t dest;
        vfs_propagation_read(table, entry->parent, &dest);
        vfs_propagation_bind(NULL, &dest, &dest);
        vfs_propagation_write(table, entry, &dest);
    }
    entry->next = table->entries;
    table->entries = entry;
    int result = vfs_propagate_attach_locked(table, entry);
    if (result != EOK) vfs_drop_tree_locked(table, entry);
    mutex_unlock(&vfs_namespace_lock);
    vfs_mount_changed();
    return result;
}

static int vfs_mount_detached(const char *src, vfs_node_t covered, int fsid, uint64_t parent_id, uint64_t attributes)
{
    vfs_mount_object_t *object = calloc(1, sizeof(*object));
    vfs_mount_attachment_t *entry = calloc(1, sizeof(*entry));
    vfs_node_t root = vfs_node_alloc(NULL, covered->name);
    if (!object || !entry || !root) { free(object); free(entry); vfs_free(root); return -ENOMEM; }
    root->type = file_dir;
    root->mode = covered->mode;
    root->owner = covered->owner;
    root->group = covered->group;
    root->fsid = (uint16_t)fsid;
    root->parent = covered->parent;
    root->root = root;
    root->refcount = 1; /* filesystem object pin; ordinary opens add to it */
    const char *mount_source = src && src[0] ? src : fs_names[fsid];
    root->mount_source = strdup(mount_source ? mount_source : "none");
    if (!root->mount_source) { root->handle = NULL; vfs_free(root); free(object); free(entry); return -ENOMEM; }

    mutex_lock(&vfs_namespace_lock);
    vfs_mount_table_t *table = vfs_mount_table(mnt_namespace_current(), true);
    vfs_mount_attachment_t *parent_mount = vfs_mount_find_id(table, parent_id);
    if (parent_id && !parent_mount) { mutex_unlock(&vfs_namespace_lock); vfs_free(root); free(object); free(entry); return -ENOENT; }
    if (parent_mount && parent_mount->object->filesystem && parent_mount->object->root == covered && covered->fsid == (uint16_t)fsid && streq(fs_names[fsid], "sysfs")) {
        mutex_unlock(&vfs_namespace_lock); vfs_free(root); free(object); free(entry); return EOK;
    }
    vfs_mount_attachment_t *existing = parent_mount ? vfs_mount_lookup(table, parent_mount, covered) : vfs_mount_attachment(table, covered);
    if (!table || existing || (covered->flags & (VFS_NODE_INITIALIZING | VFS_NODE_UNLINKED | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY))) {
        int result = -ENOMEM;
        if (table) result = existing && existing->object->root && existing->object->root->fsid == (uint16_t)fsid ? EOK : -EBUSY;
        mutex_unlock(&vfs_namespace_lock);
        vfs_free(root); free(object); free(entry);
        return result;
    }
    covered->refcount++;
    covered->mount_refs++;
    mutex_unlock(&vfs_namespace_lock);
    int status = fs_callbacks[fsid]->mount(src, root);
    if (status != EOK) {
        mutex_lock(&vfs_namespace_lock);
        covered->refcount--; covered->mount_refs--;
        mutex_unlock(&vfs_namespace_lock);
        vfs_free(root); free(object); free(entry);
        return status;
    }
    root->is_mount = 1;
    root->mount_id = vfs_new_mount_id();
    root->root = root;
    (void)vfs_icache_refresh(root);
    object->root = root;
    object->references = 1;
    object->filesystem = true;
    entry->object = object;
    entry->covered = covered;
    entry->id = root->mount_id;
    entry->attributes = attributes;
    mutex_lock(&vfs_namespace_lock);
    parent_mount = vfs_mount_find_id(table, parent_id);
    if (parent_id && !parent_mount) {
        vfs_mount_attachment_put_locked(entry);
        mutex_unlock(&vfs_namespace_lock); vfs_mount_reap(); return -ENOENT;
    }
    existing = parent_mount ? vfs_mount_lookup(table, parent_mount, covered) : vfs_mount_attachment(table, covered);
    if (existing) {
        vfs_mount_attachment_put_locked(entry);
        mutex_unlock(&vfs_namespace_lock);
        vfs_mount_reap();
        return -EBUSY;
    }
    entry->parent = parent_mount ? parent_mount : vfs_mount_containing(table, covered);
    {
        /* A fresh filesystem inherits nothing: only a shared parent shares. */
        vfs_propagation_t dest;
        vfs_propagation_read(table, entry->parent, &dest);
        vfs_propagation_bind(NULL, &dest, &dest);
        vfs_propagation_write(table, entry, &dest);
    }
    entry->next = table->entries;
    table->entries = entry;
    status = vfs_propagate_attach_locked(table, entry);
    if (status != EOK) vfs_drop_tree_locked(table, entry);
    mutex_unlock(&vfs_namespace_lock);
    vfs_mount_changed();
    return status;
}

/* Mount one named filesystem type onto a directory node. */
static int vfs_mount_id(const char *src, vfs_node_t node, int fsid, uint64_t parent_id, uint64_t attributes)
{
    uint16_t old_fsid;
    int      status;

    if (!node || !(node->type & file_dir)) return -EINVAL;
    if (fsid <= 0 || fsid >= fs_nextid || !fs_callbacks[fsid]) return -ENOENT;
    if (node != rootdir || parent_id) return vfs_mount_detached(src, node, fsid, parent_id, attributes);

    mutex_lock(&vfs_namespace_lock);
    if (node->is_mount) {
        /*
         * OpenRC may discover and mount a nodev filesystem before localmount
         * processes the same fstab entry.  This VFS has no mount stacking;
         * treat an exact same-filesystem mount as an idempotent success.
         */
        bool same_filesystem = node->fsid == (uint16_t)fsid;
        mutex_unlock(&vfs_namespace_lock);
        return same_filesystem ? EOK : -EBUSY;
    }
    if (node->flags & (VFS_NODE_INITIALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) {
        mutex_unlock(&vfs_namespace_lock);
        return -EBUSY;
    }
    node->flags |= VFS_NODE_INITIALIZING;
    mutex_unlock(&vfs_namespace_lock);

    const char *display_source;
    if (src && src[0]) {
        display_source = src;
    } else if (fs_names[fsid]) {
        display_source = fs_names[fsid];
    } else {
        display_source = "none";
    }
    char *source_copy = strdup(display_source);
    if (!source_copy) {
        mutex_lock(&vfs_namespace_lock);
        node->flags &= ~VFS_NODE_INITIALIZING;
        mutex_unlock(&vfs_namespace_lock);
        return -ENOMEM;
    }

    old_fsid                = node->fsid;
    void      *old_handle   = node->handle;
    vfs_node_t old_root     = node->root;
    clist_t    old_children = node->child;
    node->child             = NULL;
    node->fsid              = fsid;

    status = fs_callbacks[fsid]->mount(src, node);
    if (status == EOK) {
        node->covered_handle   = old_handle;
        node->covered_root     = old_root;
        node->covered_children = old_children;
        node->covered_fsid     = old_fsid;
        node->covered_valid    = true;
        free(node->mount_source);
        node->mount_source = source_copy;
        node->mount_id     = __atomic_fetch_add(&vfs_next_mount_id, 1, __ATOMIC_RELAXED);
        if (!node->mount_id) node->mount_id = __atomic_fetch_add(&vfs_next_mount_id, 1, __ATOMIC_RELAXED);
        node->root     = node;
        node->is_mount = 1;
        vfs_icache_unbind(node);
        (void)vfs_icache_refresh(node);
        vfs_dcache_invalidate_parent(node);
        mutex_lock(&vfs_namespace_lock);
        node->flags &= ~VFS_NODE_INITIALIZING;
        mutex_unlock(&vfs_namespace_lock);
        vfs_mount_changed();
        return EOK;
    }

    free(source_copy);
    node->fsid   = old_fsid;
    node->handle = old_handle;
    node->root   = old_root;
    node->child  = old_children;
    mutex_lock(&vfs_namespace_lock);
    node->flags &= ~VFS_NODE_INITIALIZING;
    mutex_unlock(&vfs_namespace_lock);
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
        int status = vfs_mount_id(src, node, i, 0, 0);
        if (status == EOK) return EOK;
        if (status != -ENOENT) last_error = status;
    }
    return last_error;
}

/* Mount a named file system to a directory */
int vfs_mount_fs_at(const char *fstype, const char *src, vfs_node_t node, uint64_t parent_id, uint64_t attributes)
{
    if (!fstype || !fstype[0]) return -EINVAL;

    for (int i = 1; i < fs_nextid; i++) {
        if (!fs_names[i] || !streq(fs_names[i], fstype)) continue;
        return vfs_mount_id(src, node, i, parent_id, attributes);
    }

    return -ENOENT;
}

int vfs_mount_fs(const char *fstype, const char *src, vfs_node_t node)
{
    return vfs_mount_fs_at(fstype, src, node, 0, 0);
}

/* Unmount a file system from a directory: check whether the mount tree still holds references or nested mounts. */
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

/* Return whether a node belongs to the shared filesystem rooted at root. */
static bool vfs_node_below(vfs_node_t node, vfs_node_t root)
{
    for (vfs_node_t current = node; current; current = current->parent) {
        if (current == root) return true;
        if (current == current->parent) break;
    }
    return false;
}

/* Move only the attachment; the shared filesystem's dentries are unchanged. */
int vfs_move_mount_at(vfs_node_t source, vfs_node_t target, uint64_t source_id, uint64_t target_id)
{
    if (!source || !target || !(target->type & file_dir)) return -EINVAL;
    mutex_lock(&vfs_namespace_lock);
    vfs_mount_table_t *table = vfs_mount_table(mnt_namespace_current(), false);
    vfs_mount_attachment_t *source_mount = vfs_mount_find_id(table, source_id);
    vfs_mount_attachment_t *target_mount = vfs_mount_find_id(table, target_id);
    vfs_mount_attachment_t *entry = source_id ? source_mount : vfs_mount_attachment(table, source);
    int result = -EINVAL;
    if (!entry || source != entry->object->root || entry == table->root || (!target->parent && target != table->root->object->root)) goto out;
    if (entry->locked || (entry->parent && entry->parent->peer_group)) goto out;
    /* Moving onto a dentry that already carries a mount is legal: systemd lands
     * a freshly mounted /proc on the unit root's /proc that way. */
    if (vfs_mount_below(target_mount, entry) || vfs_node_below(target, entry->object->root)) { result = -EBUSY; goto out; }
    if (target->flags & (VFS_NODE_UNLINKED | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) { result = -EBUSY; goto out; }
    vfs_node_t old_covered = entry->covered;
    vfs_mount_attachment_t *old_parent = entry->parent;
    vfs_propagation_t old_state;
    vfs_propagation_read(table, entry, &old_state);
    {
        /* The move keeps its own type and only merges the destination's peers. */
        vfs_propagation_t source_state, dest_state, moved;
        vfs_propagation_read(table, entry, &source_state);
        vfs_propagation_read(table, target_mount ? target_mount : vfs_mount_containing(table, target), &dest_state);
        if (vfs_propagation_move(&source_state, &dest_state, &moved) != EOK) { result = -EINVAL; goto out; }
        vfs_propagation_write(table, entry, &moved);
    }
    target->refcount++;
    target->mount_refs++;
    entry->covered->refcount--;
    entry->covered->mount_refs--;
    entry->covered = target;
    entry->parent  = target_mount ? target_mount : vfs_mount_containing(table, target);
    result = vfs_propagate_attach_locked(table, entry);
    if (result != EOK) {
        target->refcount--; target->mount_refs--;
        old_covered->refcount++; old_covered->mount_refs++;
        entry->covered = old_covered;
        entry->parent = old_parent;
        vfs_propagation_write(table, entry, &old_state);
    }
out:
    mutex_unlock(&vfs_namespace_lock);
    if (!result) vfs_mount_changed();
    return result;
}

int vfs_move_mount(vfs_node_t source, vfs_node_t target)
{
    return vfs_move_mount_at(source, target, 0, 0);
}

/* True when `path` names something strictly below `base` in the namespace. */
static bool vfs_path_is_below(const char *path, const char *base)
{
    size_t len = strlen(base);
    while (len > 0 && base[len - 1] == '/') len--; /* "/" reduces to the empty prefix */
    if (len == 0) return path[0] == '/' && path[1] != '\0';
    if (strncmp(path, base, len) != 0) return false;
    return path[len] == '/' && path[len + 1] != '\0';
}

#define VFS_MOUNT_LIMIT 100000U

typedef struct vfs_mount_copy {
    vfs_mount_attachment_t *source;
    vfs_mount_attachment_t *copy;
} vfs_mount_copy_t;

static size_t vfs_mount_count_locked(const vfs_mount_table_t *table)
{
    size_t count = table->root ? 1 : 0;
    for (vfs_mount_attachment_t *entry = table->entries; entry; entry = entry->next) count++;
    return count;
}

/* Build the whole mirror set before publishing any child. */
static int vfs_bind_recursive(vfs_mount_table_t *table, vfs_node_t source,
                              vfs_mount_attachment_t *source_mount, vfs_mount_attachment_t *new_bind)
{
    size_t capacity = vfs_mount_count_locked(table);
    if (capacity >= VFS_MOUNT_LIMIT) return -ENOSPC;
    vfs_mount_copy_t *map = calloc(capacity, sizeof(*map));
    if (!map) return -ENOMEM;
    size_t count = 1;
    map[0].source = source_mount;
    map[0].copy = new_bind;
    int result = EOK;
    for (size_t cursor = 0; cursor < count && result == EOK; cursor++) {
        for (vfs_mount_attachment_t *entry = table->entries; entry; entry = entry->next) {
            if (entry == new_bind || entry->parent != map[cursor].source) continue;
            if (!cursor && !vfs_node_below(vfs_alias_resolve(entry->covered), vfs_alias_resolve(source))) continue;
            if (entry->propagation == VFS_MOUNT_UNBINDABLE) {
                if (entry->locked) { result = -EPERM; break; }
                continue;
            }
            if (capacity + count >= VFS_MOUNT_LIMIT) { result = -ENOSPC; break; }
            vfs_mount_attachment_t *copy = malloc(sizeof(*copy));
            if (!copy) { result = -ENOMEM; break; }
            *copy = *entry;
            copy->open_files = 0;
            copy->parent = map[cursor].copy;
            copy->id = vfs_new_mount_id();
            copy->next = NULL;
            vfs_propagation_t state, dest, bound;
            vfs_propagation_read(table, entry, &state);
            vfs_propagation_read(table, copy->parent, &dest);
            vfs_propagation_bind(&state, &dest, &bound);
            vfs_propagation_write(table, copy, &bound);
            map[count].source = entry;
            map[count++].copy = copy;
        }
    }
    for (size_t i = 1; i < count; i++) {
        vfs_mount_attachment_t *copy = map[i].copy;
        if (result != EOK) { free(copy); continue; }
        copy->object->references++;
        copy->covered->refcount++;
        copy->covered->mount_refs++;
        copy->next = table->entries;
        table->entries = copy;
    }
    free(map);
    return result;
}

typedef struct vfs_mount_receiver {
    vfs_mount_table_t *table;
    vfs_mount_attachment_t *mount;
    size_t master;
} vfs_mount_receiver_t;

static int vfs_receivers_locked(vfs_mount_table_t *table, vfs_mount_attachment_t *parent,
                                vfs_mount_attachment_t *exclude, vfs_mount_receiver_t **out, size_t *count)
{
    size_t capacity = 0;
    for (vfs_mount_table_t *other = mount_tables; other; other = other->next_all) {
        size_t mounts = vfs_mount_count_locked(other);
        if (mounts > (SIZE_MAX / sizeof(vfs_mount_receiver_t)) - capacity) return -ENOMEM;
        capacity += mounts;
    }
    vfs_mount_receiver_t *receivers = calloc(capacity + 1, sizeof(*receivers));
    if (!receivers) return -ENOMEM;
    size_t used = 1;
    receivers[0] = (vfs_mount_receiver_t){table, parent, 0};
    for (size_t cursor = 0; cursor < used; cursor++) {
        uint32_t group = receivers[cursor].mount->peer_group;
        if (!group) continue;
        for (vfs_mount_table_t *other = mount_tables; other; other = other->next_all) {
            for (vfs_mount_attachment_t *entry = other->root; entry; entry = entry == other->root ? other->entries : entry->next) {
                if (other == table && exclude && vfs_mount_below(entry, exclude)) continue;
                if (entry->peer_group != group && entry->master_group != group) continue;
                size_t i;
                for (i = 0; i < used && receivers[i].mount != entry; i++) {}
                if (i != used) continue;
                receivers[used++] = (vfs_mount_receiver_t){other, entry, cursor};
            }
        }
    }
    *out = receivers;
    *count = used;
    return EOK;
}

static void vfs_table_notify_locked(vfs_mount_table_t *table)
{
    __atomic_add_fetch(&table->generation, 1, __ATOMIC_RELEASE);
    vfs_poll_source_notify(&table->poll_source, 0x00a);
}

static void vfs_drop_tree_locked(vfs_mount_table_t *table, vfs_mount_attachment_t *root)
{
    vfs_mount_attachment_t *removed = NULL;
    vfs_mount_attachment_t **link = &table->entries;
    while (*link) {
        vfs_mount_attachment_t *entry = *link;
        if (!vfs_mount_below(entry, root)) { link = &entry->next; continue; }
        *link = entry->next;
        entry->next = removed;
        removed = entry;
    }
    while (removed) {
        vfs_mount_attachment_t *next = removed->next;
        vfs_mount_attachment_put_locked(removed);
        removed = next;
    }
}

/* Allocate every propagated tree before committing the event. */
static int vfs_propagate_attach_locked(vfs_mount_table_t *table, vfs_mount_attachment_t *root)
{
    if (!root->parent || !root->parent->peer_group) return EOK;
    vfs_mount_receiver_t *receivers = NULL;
    size_t receiver_count = 0;
    int result = vfs_receivers_locked(table, root->parent, root, &receivers, &receiver_count);
    if (result != EOK) return result;
    size_t capacity = vfs_mount_count_locked(table);
    vfs_mount_attachment_t **tree = calloc(capacity, sizeof(void *));
    if (!tree) { free(receivers); return -ENOMEM; }
    size_t tree_count = 1;
    tree[0] = root;
    /* Bounded: a parent-graph cycle would otherwise walk off the allocation. */
    for (size_t cursor = 0; cursor < tree_count && tree_count < capacity; cursor++)
        for (vfs_mount_attachment_t *entry = table->entries; entry && tree_count < capacity; entry = entry->next)
            if (entry->parent == tree[cursor]) tree[tree_count++] = entry;
    if (receiver_count > SIZE_MAX / tree_count / sizeof(void *)) {
        free(tree); free(receivers); return -ENOMEM;
    }
    vfs_mount_attachment_t **copies = calloc(receiver_count * tree_count, sizeof(*copies));
    if (!copies) { free(tree); free(receivers); return -ENOMEM; }
    for (size_t i = 0; i < tree_count; i++) copies[i] = tree[i];
    for (size_t r = 1; r < receiver_count && result == EOK; r++) {
        vfs_mount_attachment_t *parent = receivers[r].mount;
        vfs_node_t covered = root->covered;
        if (vfs_alias_resolve(covered) == vfs_alias_resolve(root->parent->object->root)) covered = parent->object->root;
        else if (!vfs_node_below(vfs_alias_resolve(covered), vfs_alias_resolve(parent->object->root))) continue;
        size_t pending = tree_count;
        for (size_t p = 1; p < r; p++)
            if (receivers[p].table == receivers[r].table && copies[p * tree_count]) pending += tree_count;
        if (pending > VFS_MOUNT_LIMIT || vfs_mount_count_locked(receivers[r].table) > VFS_MOUNT_LIMIT - pending) { result = -ENOSPC; break; }
        bool peers = parent->peer_group && parent->peer_group == root->parent->peer_group;
        size_t leader = r;
        if (!peers && parent->peer_group)
            for (size_t p = 1; p < r; p++)
                if (receivers[p].mount->peer_group == parent->peer_group && copies[p * tree_count]) { leader = p; break; }
        for (size_t i = 0; i < tree_count; i++) {
            vfs_mount_attachment_t *copy = malloc(sizeof(*copy));
            if (!copy) { result = -ENOMEM; break; }
            *copy = *tree[i];
            copy->open_files = 0;
            copies[(r * tree_count) + i] = copy;
            copy->id = vfs_new_mount_id();
            copy->next = NULL;
            if (!i) { copy->parent = parent; copy->covered = covered; }
            else {
                size_t p = 0;
                while (p < i && tree[p] != tree[i]->parent) p++;
                copy->parent = copies[(r * tree_count) + p];
            }
            if (!peers && tree[i]->peer_group) {
                size_t master = receivers[r].master;
                while (master && !copies[master * tree_count]) master = receivers[master].master;
                copy->master_group = copies[(master * tree_count) + i]->peer_group;
                if (!parent->peer_group) {
                    copy->peer_group = 0;
                } else {
                    copy->peer_group = leader == r ? vfs_new_peer_group() : copies[(leader * tree_count) + i]->peer_group;
                }
                copy->propagation = copy->peer_group ? VFS_MOUNT_SHARED : VFS_MOUNT_SLAVE;
                if (leader != r) copy->master_group = copies[(leader * tree_count) + i]->master_group;
            }
            if (receivers[r].table->namespace->ns.owner != table->namespace->ns.owner) {
                copy->locked = true;
                copy->locked_attributes |= copy->attributes | MOUNT_FLAG_ATIME;
            }
        }
    }
    for (size_t r = 1; r < receiver_count; r++) {
        for (size_t i = 0; i < tree_count; i++) {
            vfs_mount_attachment_t *copy = copies[(r * tree_count) + i];
            if (!copy) continue;
            if (result != EOK) { free(copy); continue; }
            copy->object->references++;
            copy->covered->refcount++;
            copy->covered->mount_refs++;
            copy->next = receivers[r].table->entries;
            receivers[r].table->entries = copy;
        }
        if (result == EOK && copies[r * tree_count]) vfs_table_notify_locked(receivers[r].table);
    }
    free(copies); free(tree); free(receivers);
    return result;
}

/*
 * Bind a subtree at a second place in the namespace.  A bind is a new mount
 * sharing the source's superblock and dentry tree: the new root borrows the
 * source's handle and identity and delegates lookups back to it, so only the
 * namespace attachment is genuinely new.
 */
int vfs_bind_mount(vfs_node_t source, vfs_node_t target, uint64_t flags, uint64_t parent_id, uint64_t arrival_id)
{
    if (!source || !target) return -EINVAL;
    /* Linux requires the source and the target to agree on directory-ness. */
    if (!!(source->type & file_dir) != !!(target->type & file_dir)) return -ENOTDIR;

    uint64_t attributes = vfs_mount_flags(source);
    vfs_node_t root     = vfs_node_alloc(NULL, target->name);
    vfs_mount_object_t      *object = calloc(1, sizeof(*object));
    vfs_mount_attachment_t  *entry  = calloc(1, sizeof(*entry));
    if (!root || !object || !entry) {
        if (root) vfs_free(root);
        free(object);
        free(entry);
        return -ENOMEM;
    }

    mutex_lock(&vfs_namespace_lock);
    /* Binding a mountpoint means binding the filesystem mounted there. */
    vfs_node_t         alias = vfs_alias_resolve(source);
    vfs_mount_table_t *table = vfs_mount_table(mnt_namespace_current(), true);
    /* A bind may stack on an existing mount, and may name its own target as
     * the source: systemd does both while building a unit root.  Linking at the
     * head makes the newest attachment the visible one. */
    if (!table) {
        mutex_unlock(&vfs_namespace_lock);
        vfs_free(root);
        free(object);
        free(entry);
        return -ENOMEM;
    }
    vfs_mount_attachment_t *parent_mount = vfs_mount_find_id(table, parent_id);
    vfs_mount_attachment_t *source_mount = arrival_id ? vfs_mount_find_id(table, arrival_id) : vfs_mount_containing(table, source);
    if ((parent_id && !parent_mount) || (arrival_id && !source_mount)) {
        mutex_unlock(&vfs_namespace_lock); vfs_free(root); free(object); free(entry); return -ENOENT;
    }
    /* Attach into the mount the path was resolved through, as Linux uses the
     * path's vfsmount: one dentry can be a mountpoint in several mounts. */
    vfs_mount_attachment_t *dest_mount   = parent_mount ? parent_mount : vfs_mount_containing(table, target);
    vfs_propagation_t       source_state, dest_state;
    vfs_propagation_read(table, source_mount, &source_state);
    vfs_propagation_read(table, dest_mount, &dest_state);
    /* An unbindable mount cannot be bind mounted at all. */
    if (source_state.type == VFS_MOUNT_UNBINDABLE) {
        mutex_unlock(&vfs_namespace_lock);
        vfs_free(root);
        free(object);
        free(entry);
        return -EINVAL;
    }
    if (!(flags & 16384ULL)) {
        for (vfs_mount_attachment_t *child = table->entries; child; child = child->next) {
            if (child == source_mount || !child->locked || !vfs_mount_below(child, source_mount)) continue;
            if (!vfs_node_below(vfs_alias_resolve(child->covered), alias)) continue;
            mutex_unlock(&vfs_namespace_lock);
            vfs_free(root); free(object); free(entry);
            return -EINVAL;
        }
    }
    target->refcount++;
    target->mount_refs++;
    alias->refcount++; /* the bind pins the subtree it shows */

    root->type        = alias->type;
    root->mode        = alias->mode;
    root->permissions = alias->permissions;
    root->owner       = alias->owner;
    root->group       = alias->group;
    root->fsid        = alias->fsid;
    root->handle      = alias->handle;
    root->inode       = alias->inode;
    root->size        = alias->size;
    root->blksz       = alias->blksz;
    root->dev         = alias->dev;
    root->rdev        = alias->rdev;
    root->root        = root;
    root->alias       = alias;
    /* As in vfs_mount_detached: lets /proc/<pid>/fd name a bind-mounted fd. */
    root->parent      = target->parent;
    root->refcount    = 1; /* mount object pin */
    root->flags |= VFS_NODE_BIND_ALIAS;
    root->mount_source = strdup(alias->mount_source ? alias->mount_source : "none");

    object->root       = root;
    object->references = 1;
    object->filesystem = false; /* shares the source's superblock, creates none */

    entry->object     = object;
    entry->covered    = target;
    entry->parent     = dest_mount;
    entry->attributes = attributes;
    entry->locked_attributes = source_mount ? source_mount->locked_attributes : 0;
    entry->id         = vfs_new_mount_id();
    /* The new mount takes its type from the bind table, then binds recurse. */
    {
        vfs_propagation_t bound;
        vfs_propagation_bind(&source_state, &dest_state, &bound);
        vfs_propagation_write(table, entry, &bound);
    }
    entry->next       = table->entries;
    table->entries    = entry;
    /* MS_REC: the bind carries the source's submounts with it. */
    int result = flags & 16384ULL ? vfs_bind_recursive(table, alias, source_mount, entry) : EOK;
    if (result == EOK) result = vfs_propagate_attach_locked(table, entry);
    if (result != EOK) vfs_drop_tree_locked(table, entry);
    mutex_unlock(&vfs_namespace_lock);
    vfs_mount_changed();
    return result;
}

/* Find the pathname's particular attachment, including multiple nsfs binds. */
static vfs_mount_attachment_t *vfs_mount_find_path(vfs_mount_table_t *table, const char *path)
{
    char resolved[CONFIG_VFS_PATH_MAX];
    for (vfs_mount_attachment_t *entry = table ? table->entries : NULL; entry; entry = entry->next)
        if (vfs_mount_mountpoint_path(table, entry, resolved, sizeof(resolved)) == EOK && streq(path, resolved)) return entry;
    return NULL;
}

static bool vfs_tree_has_object_locked(vfs_mount_table_t *table, vfs_mount_attachment_t *root, vfs_mount_object_t *object)
{
    if (root->object == object) return true;
    for (vfs_mount_attachment_t *entry = table->entries; entry; entry = entry->next)
        if (entry->object == object && vfs_mount_below(entry, root)) return true;
    return false;
}

static int vfs_propagate_remove_locked(vfs_mount_table_t *table, vfs_mount_attachment_t *root, bool detach)
{
    if (root->locked) return -EINVAL;
    if (!detach) {
        for (vfs_mount_attachment_t *entry = table->entries; entry; entry = entry->next)
            if (entry != root && vfs_mount_below(entry, root)) return -EBUSY;
        if (root->open_files) return -EBUSY;
    }
    vfs_mount_receiver_t *receivers = NULL;
    size_t count = 0;
    int result = vfs_receivers_locked(table, root->parent, root, &receivers, &count);
    if (result != EOK) return result;
    vfs_mount_attachment_t **roots = calloc(count, sizeof(void *));
    if (!roots) { free(receivers); return -ENOMEM; }
    roots[0] = root;
    for (size_t r = 1; r < count; r++) {
        vfs_node_t covered = root->covered;
        if (vfs_alias_resolve(covered) == vfs_alias_resolve(root->parent->object->root)) covered = receivers[r].mount->object->root;
        vfs_mount_attachment_t *candidate = vfs_mount_lookup(receivers[r].table, receivers[r].mount, covered);
        if (!candidate || candidate->object != root->object) continue;
        bool blocked = false;
        for (vfs_mount_attachment_t *entry = receivers[r].table->entries; entry; entry = entry->next) {
            if (entry == candidate || !vfs_mount_below(entry, candidate)) continue;
            if (vfs_tree_has_object_locked(table, root, entry->object)) continue;
            if (entry->covered != entry->parent->object->root) { blocked = true; break; }
        }
        if (blocked) continue;
        if (!detach && candidate->open_files) { result = -EBUSY; break; }
        roots[r] = candidate;
    }
    if (result == EOK) {
        /* Preserve peer-local overmounts while removing their underlying event. */
        for (size_t r = 1; r < count; r++) {
            if (!roots[r]) continue;
            vfs_mount_table_t *other = receivers[r].table;
            for (vfs_mount_attachment_t *entry = other->entries; entry; entry = entry->next) {
                if (!vfs_mount_below(entry, roots[r]) || vfs_tree_has_object_locked(table, root, entry->object)) continue;
                if (entry->parent->object == entry->object) continue;
                vfs_mount_attachment_t *parent = entry->parent;
                if (!vfs_tree_has_object_locked(table, root, parent->object)) continue;
                entry->covered->refcount--; entry->covered->mount_refs--;
                entry->covered = roots[r]->covered;
                entry->parent = roots[r]->parent;
                entry->covered->refcount++; entry->covered->mount_refs++;
                entry->locked |= roots[r]->locked;
            }
        }
        /* Receivers are snapshotted before any graph or attachment is removed. */
        for (size_t r = count; r-- > 1;) {
            if (!roots[r]) continue;
            vfs_drop_tree_locked(receivers[r].table, roots[r]);
            vfs_table_notify_locked(receivers[r].table);
        }
        vfs_drop_tree_locked(table, root);
    }
    free(roots); free(receivers);
    return result;
}

int vfs_umount_flags(const char *path, bool nofollow, bool detach)
{
    vfs_node_t node = nofollow ? vfs_open_nofollow(path) : vfs_open(path);
    if (!node) return -ENOENT;
    mutex_lock(&vfs_namespace_lock);
    vfs_mount_table_t *table = vfs_mount_table(mnt_namespace_current(), false);
    vfs_mount_attachment_t *entry = vfs_mount_find_path(table, path);
    int result = entry ? vfs_propagate_remove_locked(table, entry, detach) : -EINVAL;
    mutex_unlock(&vfs_namespace_lock);
    vfs_close(node);
    if (result == EOK) vfs_mount_changed();
    return result;
}

int vfs_umount(const char *path)
{
    return vfs_umount_flags(path, false, false);
}

/* Validate the whole operation before changing flags or propagation. */
int vfs_mount_update(vfs_node_t node, uint64_t arrival_id, uint64_t set_flags, uint64_t clr_flags, uint32_t type, bool recursive)
{
    if (!node || ((set_flags | clr_flags) & ~VFS_MOUNT_ATTRIBUTES) || (type != UINT32_MAX && type > VFS_MOUNT_UNBINDABLE)) return -EINVAL;
    mutex_lock(&vfs_namespace_lock);
    vfs_mount_table_t *table = vfs_mount_table(mnt_namespace_current(), true);
    if (!table) { mutex_unlock(&vfs_namespace_lock); return -ENOMEM; }
    vfs_mount_attachment_t *ancestor = arrival_id ? vfs_mount_find_id(table, arrival_id) : vfs_mount_target(table, node);
    if (!ancestor && !arrival_id && node == rootdir) ancestor = table->root;
    if (!ancestor) { mutex_unlock(&vfs_namespace_lock); return arrival_id ? -ENOENT : -EINVAL; }
    if (type == VFS_MOUNT_SHARED && vfs_next_peer_group > UINT32_MAX - vfs_mount_count_locked(table)) {
        mutex_unlock(&vfs_namespace_lock); return -ENOSPC;
    }
    for (vfs_mount_attachment_t *entry = table->root; entry; entry = entry == table->root ? table->entries : entry->next) {
        if (entry != ancestor && (!recursive || !vfs_mount_below(entry, ancestor))) continue;
        uint64_t next = (entry->attributes & ~clr_flags) | set_flags;
        if ((next ^ entry->attributes) & entry->locked_attributes) { mutex_unlock(&vfs_namespace_lock); return -EPERM; }
    }
    for (vfs_mount_attachment_t *entry = table->root; entry; entry = entry == table->root ? table->entries : entry->next) {
        if (entry != ancestor && (!recursive || !vfs_mount_below(entry, ancestor))) continue;
        entry->attributes = (entry->attributes & ~clr_flags) | set_flags;
        if (type != UINT32_MAX) {
            vfs_propagation_t state;
            vfs_propagation_read(table, entry, &state);
            vfs_propagation_change(table, entry, &state, type);
            if (entry->peer_group != state.peer_group) vfs_peer_leave_locked(entry);
            vfs_propagation_write(table, entry, &state);
        }
    }
    if (table->root) {
        table->root_attributes = table->root->attributes;
        table->root_propagation = table->root->propagation;
        table->root_peer_group = table->root->peer_group;
        table->root_master_group = table->root->master_group;
    }
    mutex_unlock(&vfs_namespace_lock);
    vfs_mount_changed();
    return EOK;
}

int vfs_mount_setattr(vfs_node_t node, uint64_t set_flags, uint64_t clr_flags, bool recursive)
{
    return vfs_mount_update(node, 0, set_flags, clr_flags, UINT32_MAX, recursive);
}

typedef struct vfs_mount_format_scratch {
        char path[CONFIG_VFS_PATH_MAX];
        char escaped_path[CONFIG_VFS_PATH_MAX * 4];
        char escaped_source[CONFIG_VFS_PATH_MAX * 4];
        char options[64];
        char propagation[64];
} vfs_mount_format_scratch_t;

int vfs_mount_setpropagation(vfs_node_t node, uint32_t type, bool recursive)
{
    return vfs_mount_update(node, 0, 0, 0, type, recursive);
}

/* VFS operation: mount escape. */
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
static size_t vfs_mount_options(char *output, size_t capacity, uint64_t flags)
{
    int n = snprintf(output, capacity, "%s%s%s%s%s%s%s", (flags & MOUNT_FLAG_RDONLY) ? "ro" : "rw", (flags & MOUNT_FLAG_NOSUID) ? ",nosuid" : "",
                     (flags & MOUNT_FLAG_NODEV) ? ",nodev" : "", (flags & MOUNT_FLAG_NOEXEC) ? ",noexec" : "",
                     (flags & MOUNT_FLAG_NOATIME) ? ",noatime" : "", (flags & MOUNT_FLAG_NODIRATIME) ? ",nodiratime" : "", (flags & MOUNT_FLAG_RELATIME) ? ",relatime" : "");
    return n < 0 ? 0 : (size_t)n;
}

static uint64_t vfs_parent_mount_id(vfs_mount_table_t *table, vfs_node_t covered)
{
    vfs_mount_attachment_t *containing = vfs_mount_containing(table, covered);
    if (containing) return containing->id;
    return rootdir ? rootdir->mount_id : 0;
}

/* The optional fields of a mountinfo line: shared:N, master:N, unbindable. */
static void vfs_format_propagation(char *output, size_t capacity, const vfs_mount_table_t *table, const vfs_mount_attachment_t *entry)
{
    if (!table) return;
    uint32_t type         = entry ? entry->propagation : table->root_propagation;
    uint32_t peer_group   = entry ? entry->peer_group : table->root_peer_group;
    uint32_t master_group = entry ? entry->master_group : table->root_master_group;
    size_t   used         = 0;

    output[0] = '\0';
    if (type == VFS_MOUNT_UNBINDABLE) {
        snprintf(output, capacity, " unbindable");
        return;
    }
    /* A slave that also shares a peer group reports both tags, as Linux does. */
    if (peer_group) used = (size_t)snprintf(output, capacity, " shared:%u", peer_group);
    if (used < capacity && master_group) snprintf(output + used, capacity - used, " master:%u", master_group);
}

static void vfs_format_mount_line(vfs_mount_table_t *table, vfs_node_t filesystem, vfs_node_t covered, uint64_t id, uint64_t parent_id, uint64_t flags,
                                  const vfs_mount_attachment_t *mount, char *buffer, size_t capacity, size_t *used, bool mountinfo, vfs_mount_format_scratch_t *scratch)
{
    if (!filesystem || !id) return;
    if (mount) {
        if (vfs_mount_mountpoint_path(table, mount, scratch->path, sizeof(scratch->path)) != EOK) return;
    } else if (vfs_node_path_table(table, covered, scratch->path, sizeof(scratch->path)) != EOK) {
        return;
    }
    vfs_mount_escape(scratch->escaped_path, sizeof(scratch->escaped_path), scratch->path);
    vfs_mount_escape(scratch->escaped_source, sizeof(scratch->escaped_source), filesystem->mount_source);
    vfs_mount_options(scratch->options, sizeof(scratch->options), flags);
    vfs_format_propagation(scratch->propagation, sizeof(scratch->propagation), table, mount);
    const char *type = vfs_filesystem_name(filesystem->fsid);
    if (!type) type = vfs_filesystem_magic(filesystem->fsid) == 0x6e736673U ? "nsfs" : "unknown";
    char *destination = *used < capacity ? buffer + *used : buffer + capacity - 1;
    size_t remaining = *used < capacity ? capacity - *used : 0;
    int length;
    if (mountinfo)
        length = snprintf(destination, remaining, "%llu %llu 0:%u / %s %s%s - %s %s %s\n", id, parent_id, filesystem->fsid, scratch->escaped_path,
                          scratch->options, scratch->propagation, type, scratch->escaped_source, (flags & MOUNT_FLAG_RDONLY) ? "ro" : "rw");
    else
        length = snprintf(destination, remaining, "%s %s %s %s 0 0\n", scratch->escaped_source, scratch->escaped_path, type, scratch->options);
    if (length > 0) *used += (size_t)length;
}

/* Format the full mount table for /proc/mounts or /proc/self/mountinfo. */
size_t vfs_format_mount_table_ns(mnt_namespace_t *ns, char *buffer, size_t capacity, bool mountinfo)
{
    if (!buffer || !capacity) return 0;
    vfs_mount_format_scratch_t *scratch = malloc(sizeof(*scratch));
    if (!scratch) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("vfs: mount table scratch alloc failed.\n");
        return 0;
    }

    size_t used = 0;
    buffer[0]   = '\0';
    mutex_lock(&vfs_namespace_lock);
    vfs_mount_table_t *table = vfs_mount_table(ns, false);
    vfs_format_mount_line(table, rootdir, rootdir, rootdir->mount_id, rootdir->mount_id, table ? table->root_attributes : rootdir->flags & VFS_MOUNT_ATTRIBUTES,
                          NULL, buffer, capacity, &used, mountinfo, scratch);
    for (vfs_mount_attachment_t *entry = table ? table->entries : NULL; entry; entry = entry->next)
        vfs_format_mount_line(table, entry->object->root, entry->covered, entry->id, vfs_parent_mount_id(table, entry->covered), entry->attributes,
                              entry, buffer, capacity, &used, mountinfo, scratch);
    mutex_unlock(&vfs_namespace_lock);
    free(scratch);

    if (used >= capacity) {
        buffer[capacity - 1] = '\0';
        return capacity - 1;
    }
    return used;
}

size_t vfs_format_mount_table(char *buffer, size_t capacity, bool mountinfo)
{
    return vfs_format_mount_table_ns(mnt_namespace_current(), buffer, capacity, mountinfo);
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
    if ((file->flags & VFS_NODE_SWAPFILE) || vfs_mount_is_readonly(file)) return (size_t)-1;
    if (vfs_access_check(file, VFS_ACCESS_W)) return (size_t)-1;
    do_update(file);

    if (file->type & file_dir) return (size_t)-1;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    int64_t              ret     = mapping ? pagecache_write(mapping, addr, offset, size) : (int64_t)callbackof(file, write)(file->handle, addr, offset, size);

    if (mapping) {
        file->size = pagecache_size(mapping);
    } else {
        do_update(file);
    }
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
    if (mapping) {
        result = pagecache_read(mapping, addr, offset, size);
    } else if (callbackof(file, file_read) != vfs_empty_callback.file_read) {
        result = callbackof(file, file_read)(file, private_data, flags, addr, offset, size);
    } else {
        size_t legacy_ret = callbackof(file, read)(file->handle, addr, offset, size);
        result            = legacy_ret == (size_t)-1 ? -EIO : (int64_t)legacy_ret;
    }

    /*
     * A filesystem callback may return a short read, but never more bytes
     * than the caller supplied.  Enforce the contract before the syscall
     * layer copies from its bounded bounce buffer.
     */
    if (result > 0 && (uint64_t)result > size) {
        plogk("vfs: Read overrun from %s callback: returned %lld for %zu requested.\n", file->name, result, size);
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
static int64_t vfs_file_write_process_impl(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, process_t *proc, bool check_access, uint64_t mount_id)
{
    int64_t ret;

    if (!file || !addr) return -EINVAL;
    if (vfs_mount_is_readonly_at(file, mount_id)) return -EROFS;
    if (file->flags & VFS_NODE_SWAPFILE) return -EBUSY;
    if (check_access && vfs_access_check_process(file, VFS_ACCESS_W, proc)) return -EACCES;
    do_update(file);
    if (file->type & file_dir) return -EISDIR;

    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    if (mapping) {
        ret        = pagecache_write(mapping, addr, offset, size);
        file->size = pagecache_size(mapping);
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
int64_t vfs_file_write_granted(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, process_t *proc, uint64_t mount_id)
{
    return vfs_file_write_process_impl(file, private_data, flags, addr, offset, size, proc, false, mount_id);
}

/* Write to a file node as a specific process, enforcing its permissions. */
int64_t vfs_file_write_process(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, process_t *proc)
{
    return vfs_file_write_process_impl(file, private_data, flags, addr, offset, size, proc, true, 0);
}

/* Write a file node as the current process. */
int64_t vfs_file_write(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    return vfs_file_write_process(file, private_data, flags, addr, offset, size, process_current());
}

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

/* Read through a process-authorized descriptor; see vfs_file_read_granted(). */
int64_t vfs_file_read_user_process(vfs_node_t file, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size, process_t *proc)
{
    return vfs_file_read_user_process_impl(file, private_data, flags, addr, offset, size, proc, true);
}

/* Write through a process-authorized descriptor; see vfs_file_write_granted(). */
static int64_t vfs_file_write_user_process_impl(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, process_t *proc, bool check_access, uint64_t mount_id)
{
    if (!file || (!addr && size)) return -EINVAL;
    if (!user_range_ok(addr, size)) return -EFAULT;
    if (!(file->type & (file_stream | file_pipe)) && vfs_mount_is_readonly_at(file, mount_id)) return -EROFS;
    vfs_file_write_user_cb_t write_user = callbackof(file, file_write_user);

    /*
     * See the matching read path above. A pipe write only moves bytes and
     * must not perform a per-call metadata refresh through do_update().
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
        return vfs_file_write_process_impl(file, private_data, flags, &empty, offset, 0, proc, check_access, mount_id);
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
        int64_t ret = vfs_file_write_process_impl(file, private_data, flags, tmp, offset + done, chunk, proc, check_access, mount_id);
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
int64_t vfs_file_write_user_granted(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, process_t *proc, uint64_t mount_id)
{
    return vfs_file_write_user_process_impl(file, private_data, flags, addr, offset, size, proc, false, mount_id);
}

/* Write through a process-authorized descriptor; see vfs_file_write_granted(). */
int64_t vfs_file_write_user_process(vfs_node_t file, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size, process_t *proc)
{
    return vfs_file_write_user_process_impl(file, private_data, flags, addr, offset, size, proc, true, 0);
}

/* Check whether the node's mount subtree is read-only. */
int vfs_mount_is_readonly(vfs_node_t node)
{
    return (vfs_mount_flags(node) & MOUNT_FLAG_RDONLY) != 0;
}

int vfs_mount_is_readonly_at(vfs_node_t node, uint64_t mount_id)
{
    return mount_id ? (vfs_mount_flags_id(mount_id) & MOUNT_FLAG_RDONLY) != 0 : vfs_mount_is_readonly(node);
}

/* Check whether the node's filesystem declares device-node support. */
bool vfs_node_supports_device_nodes(vfs_node_t node)
{
    return node && node->fsid && node->fsid < (uint16_t)fs_nextid && (fs_flags[node->fsid] & VFS_FS_DEVICE_NODES);
}

/* Flush all cached data for the file to stable storage, then report the errors this descriptor owes. */
int vfs_fsync(vfs_node_t file, uint32_t *wb_err, int data_only)
{
    if (!file) return -EINVAL;
    do_update(file);
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 0);
    if (mapping) {
        /* Take the mark past everything recorded up to now, including this flush's own failure. */
        int status   = pagecache_writeback(mapping, 0, UINT64_MAX, PAGECACHE_WB_SYNC);
        int recorded = pagecache_wb_err_check(mapping, wb_err);
        return status != EOK ? status : recorded;
    }
    if (callbackof(file, sync) != vfs_empty_callback.sync) return callbackof(file, sync)(file->handle, data_only);
    return EOK;
}

/* Writeback error mark a descriptor takes when it opens this node. */
uint32_t vfs_wb_err_sample(vfs_node_t node)
{
    return pagecache_wb_err_sample(vfs_pagecache_mapping(node, 0));
}

/* Report a writeback error no descriptor has taken yet, for a flush that has no descriptor behind it. */
int vfs_wb_err_claim(vfs_node_t node)
{
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(node, 0);
    if (!mapping) return 0;
    uint32_t sample = pagecache_wb_err_sample(mapping);
    return pagecache_wb_err_check(mapping, &sample);
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
int vfs_truncate_at(vfs_node_t file, uint64_t size, uint64_t mount_id)
{
    if (!file) return -EINVAL;
    if (file->flags & VFS_NODE_SWAPFILE) return -EBUSY;
    if (vfs_mount_is_readonly_at(file, mount_id)) return -EROFS;
    do_update(file);
    if ((file->type & ~file_delete) != file_none) return file->type & file_dir ? -EISDIR : -EINVAL;
    pagecache_mapping_t *mapping = vfs_pagecache_mapping(file, 1);
    int                  result;
    if (mapping) {
        result = pagecache_truncate(mapping, size);
    } else if (callbackof(file, resize) != vfs_empty_callback.resize) {
        result = callbackof(file, resize)(file->handle, size);
    } else {
        result = -EOPNOTSUPP;
    }
    if (!result) {
        file->size = size;
        vfs_touch_modify(file);
        inotify_notify(file, IN_MODIFY);
    }
    return result;
}

/* Drop cached pages in a byte range, optionally discarding dirty data. */
int vfs_truncate(vfs_node_t file, uint64_t size)
{
    return vfs_truncate_at(file, size, 0);
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
        cpu_relax();
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
     * concurrent unsubscribe (epoll_ctl(EPOLL_CTL_DEL), poll timeout) can no
     * longer mutate a detached subscription's ->next, so nothing can free or
     * truncate the snapshot.  Callbacks run after the lock is released - they may
     * unsubscribe other poll sources, remove epoll items, drop file references,
     * or close a source owning one of these subscriptions - because running them
     * under source->lock would self-deadlock on a cascading epoll close.
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
static int vfs_close_impl(vfs_node_t node)
{
    if (!node) return -EINVAL;

    mutex_lock(&vfs_namespace_lock);

    /*
     * Namespace nodes must be closed exactly once for every retained
     * reference.  Anonymous descriptor nodes deliberately start at zero and
     * use their first close as the final release.
     */
    if (!node->refcount && node->parent && !(node->type & file_delete)) {
        mutex_unlock(&vfs_namespace_lock);
        return -EINVAL;
    }
    if (node->refcount) node->refcount--;
    bool last_ref = (node->refcount == 0);

    if (node == rootdir || !node->handle || node->type & file_proxy || !last_ref || (node->flags & VFS_NODE_BIND_ALIAS)) {
        mutex_unlock(&vfs_namespace_lock);
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
        mutex_unlock(&vfs_namespace_lock);
        if (anonymous) vfs_poll_notify(node, UINT32_MAX);
        if (node->mapping) (void)pagecache_writeback(node->mapping, 0, UINT64_MAX, PAGECACHE_WB_SYNC);
        if (anonymous) vfs_pagecache_destroy(node);
        /* Named FIFOs remain reopenable until their inode is unlinked. */
        if (anonymous || !(node->type & file_pipe)) callbackof(node, close)(node->handle);
        if (anonymous) {
            callbackof(node, free)(node->handle);
            node->handle = 0;
            vfs_free(node);
        }
        return EOK;
    }

    node->flags |= VFS_NODE_FINALIZING;
    mutex_unlock(&vfs_namespace_lock);

    if (node->type & file_dir) {
        mutex_lock(&vfs_namespace_lock);
        bool not_empty = vfs_directory_has_visible_children(node);
        mutex_unlock(&vfs_namespace_lock);
        if (not_empty) {
            mutex_lock(&vfs_namespace_lock);
            node->flags &= ~VFS_NODE_FINALIZING;
            mutex_unlock(&vfs_namespace_lock);
            return -ENOTEMPTY;
        }
    }

    if (node->mapping && !(node->flags & VFS_NODE_DELETE_COMMITTED)) {
        int result = pagecache_writeback(node->mapping, 0, UINT64_MAX, PAGECACHE_WB_SYNC);
        if (result) {
            mutex_lock(&vfs_namespace_lock);
            node->flags &= ~VFS_NODE_FINALIZING;
            mutex_unlock(&vfs_namespace_lock);
            return result;
        }
    }

    if (!(node->flags & VFS_NODE_DELETE_COMMITTED)) {
        int res = node->parent ? callbackof(node, delete)(node->parent->handle, node) : EOK;
        if (res < 0) {
            mutex_lock(&vfs_namespace_lock);
            node->flags &= ~VFS_NODE_FINALIZING;
            mutex_unlock(&vfs_namespace_lock);
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
    mutex_lock(&vfs_namespace_lock);
    if (!(node->flags & VFS_NODE_UNLINKED) && node->parent) {
        vfs_node_t parent = node->parent;
        vfs_dcache_remove(node);
        node->parent->child = clist_delete(node->parent->child, node);
        node->flags |= VFS_NODE_UNLINKED;
        vfs_dcache_add_negative(parent, node->name);
    }
    if (node->flags & VFS_NODE_PARENT_RETAINED) {
        retained_parent = node->parent;
        node->flags &= ~VFS_NODE_PARENT_RETAINED;
    }
    node->parent = NULL;
    mutex_unlock(&vfs_namespace_lock);
    callbackof(node, free)(node->handle);
    node->handle = 0;
    vfs_free(node);
    if (retained_parent) vfs_close(retained_parent);
    return EOK;
}

/* Retired trees retain their object pin until inherited descriptors close. */
static void vfs_mount_reap(void)
{
    mutex_lock(&vfs_namespace_lock);
    if (mount_reaping) { mutex_unlock(&vfs_namespace_lock); return; }
    mount_reaping = true;
    for (;;) {
        vfs_mount_object_t **link = &retired_mounts;
        while (*link && ((*link)->filesystem ? vfs_mount_tree_busy_locked((*link)->root, (*link)->root) : (*link)->root->alias && (*link)->root->refcount > 1)) link = &(*link)->retired_next;
        vfs_mount_object_t *object = *link;
        if (!object) break;
        *link = object->retired_next;
        mutex_unlock(&vfs_namespace_lock);
        if (object->filesystem) {
            vfs_node_t root = object->root;
            vfs_icache_invalidate_mount(root);
            vfs_free_child(root);
            callbackof(root, unmount)(root->handle);
            root->handle = NULL;
            root->parent = NULL;
            vfs_free(root);
        } else if (object->root->alias) {
            /* A bind root renders the aliased subtree and owns none of it. */
            vfs_node_t root  = object->root;
            vfs_node_t alias = root->alias;
            root->alias      = NULL;
            root->handle     = NULL;
            root->child      = NULL;
            root->parent     = NULL;
            vfs_free(root);
            mutex_lock(&vfs_namespace_lock);
            if (alias->refcount) alias->refcount--;
            mutex_unlock(&vfs_namespace_lock);
        } else {
            vfs_close_impl(object->root);
        }
        free(object);
        mutex_lock(&vfs_namespace_lock);
    }
    mount_reaping = false;
    mutex_unlock(&vfs_namespace_lock);
}

int vfs_close(vfs_node_t node)
{
    int result = vfs_close_impl(node);
    vfs_mount_reap();
    return result;
}

/* Unlink a node from its parent, deferring the final free. */
int vfs_namespace_unlink(vfs_node_t node)
{
    if (!node || node == rootdir) return -EINVAL;
    if (node->is_mount || node->mount_refs) return -EBUSY;
    if ((node->flags & VFS_NODE_SWAPFILE) || node->mount_refs || node->is_mount) return -EBUSY;
    if (!node->parent) return -EINVAL;

    mutex_lock(&vfs_namespace_lock);
    if ((node->flags & (VFS_NODE_UNLINKED | VFS_NODE_UNLINKING | VFS_NODE_RENAME_BUSY)) || !node->parent || (node->parent->flags & VFS_NODE_RENAME_BUSY)) {
        mutex_unlock(&vfs_namespace_lock);
        return -ENOENT;
    }
    if ((node->type & file_dir) && vfs_directory_has_visible_children(node)) {
        mutex_unlock(&vfs_namespace_lock);
        return -ENOTEMPTY;
    }

    node->flags |= VFS_NODE_UNLINKING;
    mutex_unlock(&vfs_namespace_lock);

    int status = callbackof(node, delete)(node->parent->handle, node);
    if (status < 0) {
        mutex_lock(&vfs_namespace_lock);
        node->flags &= ~VFS_NODE_UNLINKING;
        mutex_unlock(&vfs_namespace_lock);
        return status;
    }

    if (!(node->flags & VFS_NODE_EVENT_DELETE)) {
        node->flags |= VFS_NODE_EVENT_DELETE;
        inotify_notify_delete(node);
    }
    vfs_inode_drop_link(node);

    mutex_lock(&vfs_namespace_lock);
    vfs_node_t parent = node->parent;
    vfs_dcache_remove(node);
    parent->child = clist_delete(parent->child, node);
    node->parent  = NULL;
    node->flags &= ~VFS_NODE_UNLINKING;
    node->flags |= VFS_NODE_DELETE_COMMITTED | VFS_NODE_UNLINKED;
    node->type |= file_delete;
    vfs_dcache_add_negative(parent, node->name);
    mutex_unlock(&vfs_namespace_lock);
    return EOK;
}

/* Detach a node and its children from the namespace for deferred free. */
void vfs_namespace_detach(vfs_node_t node)
{
    if (!node || node == rootdir) return;

    mutex_lock(&vfs_namespace_lock);
    if (node->flags & (VFS_NODE_UNLINKED | VFS_NODE_UNLINKING | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) {
        mutex_unlock(&vfs_namespace_lock);
        return;
    }
    node->flags |= VFS_NODE_UNLINKING;
    mutex_unlock(&vfs_namespace_lock);
    if (!(node->flags & VFS_NODE_EVENT_DELETE)) {
        node->flags |= VFS_NODE_EVENT_DELETE;
        inotify_notify_delete(node);
    }
    vfs_inode_drop_link(node);

    mutex_lock(&vfs_namespace_lock);
    vfs_node_t parent = node->parent;
    if (parent) {
        vfs_dcache_remove(node);
        parent->child = clist_delete(parent->child, node);
    }
    node->parent = NULL;
    node->flags |= VFS_NODE_UNLINKED | VFS_NODE_DELETE_COMMITTED | VFS_NODE_UNLINKING;
    node->type |= file_delete;
    if (parent) vfs_dcache_add_negative(parent, node->name);
    mutex_unlock(&vfs_namespace_lock);

    /*
     * Detach children through the same deferred-free path.  A temporary
     * reference keeps each selected child alive after dropping the namespace
     * lock; open descriptors retain their own references independently.
     */
    vfs_free_child(node);

    mutex_lock(&vfs_namespace_lock);
    node->flags &= ~VFS_NODE_UNLINKING;
    int release_now = node->refcount == 0;
    mutex_unlock(&vfs_namespace_lock);
    if (release_now) vfs_close(node);
}

/* Delete a VFS (Virtual File System) node and clean up associated resources */
int vfs_delete(vfs_node_t node)
{
    int status;

    if (!node || node == rootdir) return -EINVAL;
    if ((node->flags & VFS_NODE_SWAPFILE) || node->mount_refs || node->is_mount) return -EBUSY;

    do_update(node);
    mutex_lock(&vfs_namespace_lock);
    if ((node->flags & (VFS_NODE_INITIALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) || (node->parent && (node->parent->flags & VFS_NODE_RENAME_BUSY))
        || (node->type & file_delete)) {
        mutex_unlock(&vfs_namespace_lock);
        return -ENOENT;
    }
    if ((node->type & file_dir) && vfs_directory_has_visible_children(node)) {
        mutex_unlock(&vfs_namespace_lock);
        return -ENOTEMPTY;
    }
    node->flags |= VFS_NODE_UNLINKING;
    mutex_unlock(&vfs_namespace_lock);

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
    vfs_inode_drop_link(node);
    if (node->parent) vfs_touch_modify(node->parent);
    mutex_lock(&vfs_namespace_lock);
    node->type |= file_delete;
    if (node->parent) {
        vfs_dcache_remove(node);
        vfs_dcache_add_negative(node->parent, node->name);
    }
    node->flags &= ~VFS_NODE_UNLINKING;
    if (node->parent && !(node->flags & VFS_NODE_PARENT_RETAINED)) {
        node->parent->refcount++;
        node->flags |= VFS_NODE_PARENT_RETAINED;
    }
    bool release_now = node->refcount == 0;
    mutex_unlock(&vfs_namespace_lock);
    if (release_now) return vfs_close(node);
    return EOK;
delete_failed:
    mutex_lock(&vfs_namespace_lock);
    node->flags &= ~VFS_NODE_UNLINKING;
    mutex_unlock(&vfs_namespace_lock);
    return status;
}

/* Enforce the sticky bit on rename victims. */
static int vfs_rename_sticky_check(vfs_node_t parent, vfs_node_t victim)
{
    process_t *process = process_current();
    if (!process || namespace_initial_root(process) || !(parent->mode & 01000)) return EOK;
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
    if (node->is_mount || node->mount_refs || (node->flags & VFS_NODE_SWAPFILE)) return -EBUSY;
    if (vfs_mount_is_readonly(node) || vfs_mount_is_readonly(new_parent)) return -EROFS;
    if (callbackof(node, rename) == vfs_empty_callback.rename) return -EOPNOTSUPP;

    mutex_lock(&vfs_rename_serial_lock);
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

    mutex_lock(&vfs_namespace_lock);
    if (node->parent != old_parent || (node->flags & (VFS_NODE_INITIALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) || (node->type & file_delete)
        || (old_parent->flags & (VFS_NODE_INITIALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) || (old_parent->type & file_delete)
        || (new_parent->flags & (VFS_NODE_INITIALIZING | VFS_NODE_UNLINKING | VFS_NODE_UNLINKED | VFS_NODE_FINALIZING | VFS_NODE_RENAME_BUSY)) || (new_parent->type & file_delete)) {
        status = -EBUSY;
        goto unlock_error;
    }
    if (old_parent == new_parent && streq(node->name, new_name_arg)) {
        mutex_unlock(&vfs_namespace_lock);
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
        mutex_unlock(&vfs_namespace_lock);
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
            mutex_unlock(&vfs_namespace_lock);
            status = EOK;
            goto out;
        }
        bool source_is_dir = (node->type & file_dir) != 0;
        bool target_is_dir = (target->type & file_dir) != 0;
        if (source_is_dir && !target_is_dir) {
            status = -ENOTDIR;
        } else if (!source_is_dir && target_is_dir) {
            status = -EISDIR;
        } else if (target_is_dir && vfs_directory_has_visible_children(target)) {
            status = -ENOTEMPTY;
        } else if (target->is_mount || (target->flags & VFS_NODE_SWAPFILE)) {
            status = -EBUSY;
        } else {
            status = vfs_rename_sticky_check(new_parent, target);
        }
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
    mutex_unlock(&vfs_namespace_lock);

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
        mutex_lock(&vfs_namespace_lock);
        node->flags &= ~VFS_NODE_INITIALIZING;
        old_parent->flags &= ~VFS_NODE_RENAME_BUSY;
        new_parent->flags &= ~VFS_NODE_RENAME_BUSY;
        if (target) target->flags &= ~VFS_NODE_INITIALIZING;
        mutex_unlock(&vfs_namespace_lock);
        goto out;
    }

    if (target && !(target->flags & VFS_NODE_EVENT_DELETE)) {
        target->flags |= VFS_NODE_EVENT_DELETE;
        inotify_notify_delete(target);
    }
    if (target) vfs_inode_drop_link(target);
    mutex_lock(&vfs_namespace_lock);
    if (target) {
        vfs_dcache_remove(target);
        new_parent->child = clist_delete(new_parent->child, target);
        target->parent    = NULL;
        target->type |= file_delete;
        target->flags &= ~VFS_NODE_INITIALIZING;
        target->flags |= VFS_NODE_DELETE_COMMITTED | VFS_NODE_UNLINKED;
    }
    vfs_dcache_remove(node);
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
    vfs_dcache_add_negative(old_parent, old_name);
    vfs_dcache_invalidate(new_parent, node->name);
    vfs_dcache_add(node);
    node->flags &= ~VFS_NODE_INITIALIZING;
    old_parent->flags &= ~VFS_NODE_RENAME_BUSY;
    new_parent->flags &= ~VFS_NODE_RENAME_BUSY;
    old_parent->visited = 0;
    new_parent->visited = 0;
    vfs_touch_change(node);
    vfs_touch_modify(old_parent);
    if (new_parent != old_parent) vfs_touch_modify(new_parent);
    mutex_unlock(&vfs_namespace_lock);

    mutex_unlock(&vfs_rename_serial_lock);
    inotify_notify_move(node, old_parent, old_name, node->name);
    if (target) vfs_close(target);
    free(old_name);
    return EOK;
unlock_error:
    mutex_unlock(&vfs_namespace_lock);
out:
    mutex_unlock(&vfs_rename_serial_lock);
    if (target_retained && (target->flags & VFS_NODE_INITIALIZING)) {
        mutex_lock(&vfs_namespace_lock);
        target->flags &= ~VFS_NODE_INITIALIZING;
        mutex_unlock(&vfs_namespace_lock);
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
        mutex_lock(&vfs_namespace_lock);
        clist_t entry = vfs->child;
        if (!entry) {
            mutex_unlock(&vfs_namespace_lock);
            break;
        }
        vfs_node_t child = entry->data;
        /* Remove the list reference before a detach can release the inode. */
        vfs->child = clist_delete_node(vfs->child, entry);
        if (child) {
            vfs_dcache_remove(child);
            child->refcount++;
        }
        mutex_unlock(&vfs_namespace_lock);
        if (!child) continue;
        vfs_namespace_detach(child);
        vfs_close(child);
    }
}

/* Free the memory associated with a vfs node */
void vfs_free(vfs_node_t vfs)
{
    if (!vfs) return;

    vfs_dcache_invalidate_parent(vfs);
    vfs_dcache_remove(vfs);
    vfs_free_child(vfs);
    if (vfs->linkto) {
        vfs_close(vfs->linkto);
        vfs->linkto = 0;
    }
    if (vfs->handle && !(vfs->flags & VFS_NODE_BIND_ALIAS)) {
        vfs_pagecache_destroy(vfs);
        callbackof(vfs, close)(vfs->handle);
        callbackof(vfs, free)(vfs->handle);
        vfs->handle = 0;
    }
    free(vfs->linkname);
    free(vfs->mount_source);
    free(vfs->name);
    vfs_icache_unbind(vfs);
    free(vfs);
}

/* Initialize the virtual file system */
void init_vfs(void)
{
    for (size_t i = 0; i < sizeof(struct vfs_callback) / sizeof(void *); i++) ((void **)&vfs_empty_callback)[i] = empty_func;
    mutex_init(&vfs_namespace_lock);
    mutex_init(&vfs_rename_serial_lock);
    vfs_dcache_init();
    vfs_icache_init();
    pagecache_allocator_t allocator = {.alloc = vfs_page_alloc, .free = vfs_page_free};
    size_t                max_pages = frame_allocator.origin_frames / 2;
    if (max_pages < 256) max_pages = 256;
    (void)pagecache_init(&allocator, max_pages);
    rootdir = vfs_node_alloc(0, "/");
    if (!rootdir) panic("vfs: Cannot allocate the root directory node.");
    rootdir->type = file_dir;
    (void)vfs_mount_table(&init_mnt_ns, true);
    (void)vfs_icache_bind(rootdir);
    plogk("vfs: Initial root directory of the virtual file system: '/'\n");
}
