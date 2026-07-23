/*
 *
 *      sysfs.c
 *      sysfs — the filesystem for exporting kernel objects
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/sysfs.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <kernel/kobject.h>
#include <kernel/printk.h>
#include <libs/glist/circular_list.h>
#include <libs/std/stdarg.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/*  Internal types                                                     */
/* ------------------------------------------------------------------ */

enum sysfs_node_type {
    SYSFS_DIR,       /* kobject directory */
    SYSFS_ATTR,      /* regular attribute file */
    SYSFS_BIN_ATTR,  /* binary attribute file */
    SYSFS_SYMLINK,   /* symbolic link */
};

typedef struct sysfs_attr_entry {
    struct attribute *attr;
    vfs_node_t         vnode;    /* VFS node for this file */
    struct kobject    *kobj;     /* owning kobject */
} sysfs_attr_entry_t;

typedef struct sysfs_symlink_entry {
    const char     *name;
    struct kobject *target;
    vfs_node_t      vnode;
} sysfs_symlink_entry_t;

typedef struct sysfs_node {
    enum sysfs_node_type type;
    struct kobject       *kobj;
    struct attribute     *attr;         /* for SYSFS_ATTR */
    struct bin_attribute *bin_attr;     /* for SYSFS_BIN_ATTR */
    char                 *symlink_path; /* for SYSFS_SYMLINK */
    char                 *content;      /* cached content buffer */
    size_t                content_size;
} sysfs_node_t;

/* ------------------------------------------------------------------ */
/*  Global state                                                       */
/* ------------------------------------------------------------------ */

static int            sysfs_id;         /* VFS filesystem ID */
struct kobject        *sysfs_root_kobj; /* /sys root kobject (global) */
static vfs_node_t     sysfs_root_vnode;  /* /sys mount point VFS node */

/* Forward declarations */
static int sysfs_stat(void *file, vfs_node_t node);
static void sysfs_populate_dir(struct kobject *kobj);

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

/* Dummy shit */
static void sysfs_dummy(void)
{ // dont understand why this is needed but it is
}

static sysfs_node_t *sysfs_node_alloc(enum sysfs_node_type type)
{
    sysfs_node_t *sn = calloc(1, sizeof(sysfs_node_t));
    if (!sn) return NULL;
    sn->type = type;
    return sn;
}

static void sysfs_node_free(sysfs_node_t *sn)
{
    if (!sn) return;
    free(sn->content);
    free(sn->symlink_path);
    free(sn);
}

/* Look up a child kobject by name */
static struct kobject *sysfs_find_child_kobj(struct kobject *parent,
                                             const char *name)
{
    clist_t node;
    if (!parent || !name) return NULL;

    for (node = parent->children; node; node = node->next) {
        struct kobject *kobj = node->data;
        if (kobj && kobj->name && streq(kobj->name, name)) return kobj;
    }
    return NULL;
}

/* Look up an attribute entry by name */
static sysfs_attr_entry_t *sysfs_find_attr(struct kobject *kobj,
                                           const char *name)
{
    clist_t node;
    if (!kobj || !name) return NULL;

    for (node = kobj->attributes; node; node = node->next) {
        sysfs_attr_entry_t *entry = node->data;
        if (entry && entry->attr && entry->attr->name && streq(entry->attr->name, name)) return entry;
    }
    return NULL;
}

/* Look up a symlink entry by name */
static sysfs_symlink_entry_t *sysfs_find_symlink(struct kobject *kobj,
                                                  const char *name)
{
    clist_t node;
    if (!kobj || !name) return NULL;

    for (node = kobj->symlinks; node; node = node->next) {
        sysfs_symlink_entry_t *entry = node->data;
        if (entry && entry->name && streq(entry->name, name)) return entry;
    }
    return NULL;
}

/* Check if an attribute is writable */
static int sysfs_attr_is_writable(const struct attribute *attr)
{
    if (!attr) return 0;
    /* Owner write or group write bit set */
    return (attr->mode & 0200) != 0;
}

/* ------------------------------------------------------------------ */
/*  Content generation (read path for attribute files)                 */
/* ------------------------------------------------------------------ */

static void sysfs_gen_attr_content(sysfs_node_t *sn)
{
    struct kobject   *kobj = sn->kobj;
    struct attribute *attr = sn->attr;

    if (!kobj || !attr) return;
    if (!kobj->ktype || !kobj->ktype->sysfs_ops) return;
    if (!kobj->ktype->sysfs_ops->show) return;

    char *buf = malloc(SYSFS_PAGE_SIZE);
    if (!buf) return;

    ssize_t n = kobj->ktype->sysfs_ops->show(kobj, attr, buf);
    if (n < 0) {
        free(buf);
        return;
    }

    sn->content      = buf;
    sn->content_size = (size_t)n;
}

/* ------------------------------------------------------------------ */
/*  VFS callbacks                                                      */
/* ------------------------------------------------------------------ */

static int sysfs_mount(const char *handle, vfs_node_t node)
{
    if (handle) return -EINVAL;

    node->fsid = sysfs_id;

    sysfs_node_t *root_sn = sysfs_node_alloc(SYSFS_DIR);
    if (!root_sn) return -ENOMEM;

    root_sn->kobj = sysfs_root_kobj;
    node->handle  = root_sn;
    node->type    = file_dir;

    /* Link back */
    sysfs_root_vnode = node;
    if (sysfs_root_kobj) {
        sysfs_root_kobj->sd = node;
        sysfs_root_kobj->state_in_sysfs = 1;

        /* Populate VFS nodes for kobjects that were created
         * before the filesystem was mounted */
        sysfs_populate_dir(sysfs_root_kobj);
    }

    return EOK;
}

static void sysfs_umount(void *root)
{
    sysfs_node_free(root);
}

static void sysfs_open(void *parent_handle, const char *name, vfs_node_t node)
{
    sysfs_node_t *psn = parent_handle;
    if (!psn || !name) return;

    /* If the node already has a handle (created proactively), keep it */
    if (node->handle) return;

    switch (psn->type) {
        case SYSFS_DIR : {
            struct kobject *parent_kobj = psn->kobj;
            if (!parent_kobj) return;

            /* Check for child kobject */
            struct kobject *child_kobj = sysfs_find_child_kobj(parent_kobj, name);
            if (child_kobj) {
                sysfs_node_t *sn = sysfs_node_alloc(SYSFS_DIR);
                if (!sn) return;
                sn->kobj       = child_kobj;
                node->handle   = sn;
                node->type     = file_dir;
                child_kobj->sd = node;
                return;
            }

            /* Check for attribute file */
            sysfs_attr_entry_t *attr_entry = sysfs_find_attr(parent_kobj, name);
            if (attr_entry) {
                sysfs_node_t *sn = sysfs_node_alloc(SYSFS_ATTR);
                if (!sn) return;
                sn->kobj       = parent_kobj;
                sn->attr       = attr_entry->attr;
                node->handle   = sn;
                node->type     = file_stream;
                return;
            }

            /* Check for binary attribute — these are set up when created */
            /* Binary files are created proactively, skip here */

            /* Check for symlink */
            sysfs_symlink_entry_t *sym_entry = sysfs_find_symlink(parent_kobj, name);
            if (sym_entry) {
                sysfs_node_t *sn = sysfs_node_alloc(SYSFS_SYMLINK);
                if (!sn) return;
                sn->kobj          = parent_kobj;
                sn->symlink_path  = kobject_get_path(sym_entry->target);
                node->handle      = sn;
                node->type        = file_symlink;
                return;
            }
            break;
        }
        default :
            break;
    }
}

static void sysfs_close(void *current)
{
    (void)current;
}

static int sysfs_stat(void *file, vfs_node_t node)
{
    sysfs_node_t *sn = file;
    if (!sn) return -ENOENT;

    switch (sn->type) {
        case SYSFS_DIR : {
            struct kobject *kobj = sn->kobj;
            if (!kobj) break;

            /* Enumerate child kobjects */
            {
                clist_t child;
                for (child = kobj->children; child; child = child->next) {
                    struct kobject *child_kobj = child->data;
                    if (!child_kobj || !child_kobj->name) continue;

                    /* Check if VFS node already exists */
                    vfs_node_t existing = NULL;
                    {
                        clist_t cn;
                        for (cn = node->child; cn; cn = cn->next) {
                            vfs_node_t cnv = cn->data;
                            if (cnv && cnv->name && streq(cnv->name, child_kobj->name)) {
                                existing = cnv;
                                break;
                            }
                        }
                    }

                    if (!existing) {
                        vfs_node_t child_vn = vfs_node_alloc(node, child_kobj->name);
                        if (!child_vn) continue;
                        child_vn->type   = file_dir;
                        child_vn->fsid   = sysfs_id;

                        sysfs_node_t *child_sn = sysfs_node_alloc(SYSFS_DIR);
                        if (child_sn) {
                            child_sn->kobj     = child_kobj;
                            child_vn->handle   = child_sn;
                        }
                        child_kobj->sd        = child_vn;
                        child_kobj->state_in_sysfs = 1;
                    }
                }
            }

            /* Enumerate attribute files */
            {
                clist_t attr_node;
                for (attr_node = kobj->attributes; attr_node; attr_node = attr_node->next) {
                    sysfs_attr_entry_t *entry = attr_node->data;
                    if (!entry || !entry->attr || !entry->attr->name) continue;
                    if (entry->vnode) continue; /* already has VFS node */

                    vfs_node_t file_vn = vfs_node_alloc(node, entry->attr->name);
                    if (!file_vn) continue;

                    file_vn->type        = file_stream;
                    file_vn->fsid        = sysfs_id;
                    file_vn->permissions = entry->attr->mode;

                    sysfs_node_t *file_sn = sysfs_node_alloc(SYSFS_ATTR);
                    if (file_sn) {
                        file_sn->kobj     = kobj;
                        file_sn->attr     = entry->attr;
                        file_vn->handle   = file_sn;
                    }
                    entry->vnode = file_vn;
                }
            }

            /* Enumerate symlinks */
            {
                clist_t sym_node;
                for (sym_node = kobj->symlinks; sym_node; sym_node = sym_node->next) {
                    sysfs_symlink_entry_t *entry = sym_node->data;
                    if (!entry || !entry->name) continue;
                    if (entry->vnode) continue;

                    vfs_node_t sym_vn = vfs_node_alloc(node, entry->name);
                    if (!sym_vn) continue;

                    sym_vn->type = file_symlink;
                    sym_vn->fsid = sysfs_id;

                    sysfs_node_t *sym_sn = sysfs_node_alloc(SYSFS_SYMLINK);
                    if (sym_sn) {
                        sym_sn->kobj         = kobj;
                        sym_sn->symlink_path = kobject_get_path(entry->target);
                        sym_vn->handle       = sym_sn;
                    }
                    entry->vnode = sym_vn;
                }
            }
            break;
        }
        case SYSFS_ATTR : {
            if (!sn->content) sysfs_gen_attr_content(sn);
            node->type = file_stream;
            if (sn->content) node->size = sn->content_size;
            break;
        }
        case SYSFS_BIN_ATTR : {
            node->type = file_stream;
            if (sn->bin_attr) node->size = sn->bin_attr->size;
            break;
        }
        case SYSFS_SYMLINK : {
            node->type = file_symlink;
            break;
        }
    }

    return EOK;
}

static size_t sysfs_read(void *file, void *addr, size_t offset, size_t size)
{
    sysfs_node_t *sn = file;
    if (!sn) return 0;

    switch (sn->type) {
        case SYSFS_ATTR : {
            if (!sn->content) sysfs_gen_attr_content(sn);
            if (!sn->content) return 0;
            if (offset >= sn->content_size) return 0;

            size_t actual = (offset + size > sn->content_size)
                                ? (sn->content_size - offset)
                                : size;
            memcpy(addr, sn->content + offset, actual);
            return actual;
        }
        case SYSFS_BIN_ATTR : {
            if (!sn->bin_attr || !sn->bin_attr->read) return 0;
            return sn->bin_attr->read(sn->kobj, sn->bin_attr, addr,
                                      (int64_t)offset, size);
        }
        case SYSFS_DIR :
            return 0; /* cannot read a directory */
        case SYSFS_SYMLINK :
            return 0; /* use readlink instead */
    }

    return 0;
}

static size_t sysfs_write(void *file, const void *addr, size_t offset,
                          size_t size)
{
    sysfs_node_t *sn = file;
    if (!sn) return 0;

    switch (sn->type) {
        case SYSFS_ATTR : {
            if (!sysfs_attr_is_writable(sn->attr)) return 0;
            if (!sn->kobj || !sn->kobj->ktype) return 0;
            if (!sn->kobj->ktype->sysfs_ops) return 0;
            if (!sn->kobj->ktype->sysfs_ops->store) return 0;

            /* Invalidate cached content so next read regenerates */
            free(sn->content);
            sn->content      = NULL;
            sn->content_size = 0;

            ssize_t ret = sn->kobj->ktype->sysfs_ops->store(sn->kobj,
                                                             sn->attr,
                                                             addr, size);
            if (ret < 0) return 0;

            /* Send KOBJ_CHANGE uevent */
            kobject_uevent(sn->kobj, KOBJ_CHANGE);

            return (size_t)ret;
        }
        case SYSFS_BIN_ATTR : {
            if (!sn->bin_attr || !sn->bin_attr->write) return 0;
            return sn->bin_attr->write(sn->kobj, sn->bin_attr,
                                       (char *)addr, (int64_t)offset, size);
        }
        default :
            return 0;
    }
}

static size_t sysfs_readlink(vfs_node_t node, void *addr, size_t offset,
                             size_t size)
{
    sysfs_node_t *sn = node->handle;
    if (!sn || sn->type != SYSFS_SYMLINK) return 0;
    if (!sn->symlink_path) return 0;

    size_t len = strlen(sn->symlink_path);
    if (offset >= len) return 0;

    size_t actual = (offset + size > len) ? (len - offset) : size;
    memcpy(addr, sn->symlink_path + offset, actual);
    return actual;
}

static int sysfs_mkdir(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -EROFS;
}

static int sysfs_mkfile(void *parent, const char *name, vfs_node_t node)
{
    (void)parent;
    (void)name;
    (void)node;
    return -EROFS;
}

static int sysfs_delete(void *parent, vfs_node_t node)
{
    (void)parent;
    (void)node;
    return -EROFS;
}

static int sysfs_rename_node(void *current, const char *new_name)
{
    (void)current;
    (void)new_name;
    return -EROFS;
}

static int sysfs_free(void *handle)
{
    sysfs_node_t *sn = handle;
    sysfs_node_free(sn);
    return EOK;
}

static vfs_node_t sysfs_dup(vfs_node_t node)
{
    sysfs_node_t *old_sn = node->handle;
    vfs_node_t    copy   = vfs_node_alloc(node->parent, node->name);

    copy->type        = node->type;
    copy->size        = node->size;
    copy->flags       = node->flags;
    copy->permissions = node->permissions;
    copy->owner       = node->owner;
    copy->fsid        = node->fsid;

    if (old_sn) {
        /* Reference the same kobject/attribute — just share the pointer */
        sysfs_node_t *new_sn = sysfs_node_alloc(old_sn->type);
        if (new_sn) {
            new_sn->kobj          = old_sn->kobj;
            new_sn->attr          = old_sn->attr;
            new_sn->bin_attr      = old_sn->bin_attr;
            new_sn->symlink_path  = old_sn->symlink_path
                                        ? strdup(old_sn->symlink_path)
                                        : NULL;
            /* do not copy content — will be regenerated */
            copy->handle = new_sn;
        }
    }

    return copy;
}

static int sysfs_poll(void *file, size_t events)
{
    (void)file;
    int revents = 0;
    if (events & 0x0001) revents |= 0x0001; /* POLLIN */
    if (events & 0x0004) revents |= 0x0004; /* POLLOUT */
    return revents;
}

static int sysfs_ioctl(void *file, size_t req, void *arg)
{
    (void)file;
    (void)req;
    (void)arg;
    return -ENOTTY;
}

/* ------------------------------------------------------------------ */
/*  Callback table                                                     */
/* ------------------------------------------------------------------ */

static struct vfs_callback sysfs_callbacks = {
    .mount    = sysfs_mount,
    .unmount  = sysfs_umount,
    .open     = sysfs_open,
    .close    = sysfs_close,
    .read     = sysfs_read,
    .write    = sysfs_write,
    .readlink = sysfs_readlink,
    .mkdir    = sysfs_mkdir,
    .mkfile   = sysfs_mkfile,
    .link     = (vfs_mk_t)sysfs_dummy,
    .symlink  = (vfs_mk_t)sysfs_dummy,
    .stat     = sysfs_stat,
    .ioctl    = sysfs_ioctl,
    .dup      = sysfs_dup,
    .poll     = sysfs_poll,
    .free     = sysfs_free,
    .delete   = sysfs_delete,
    .rename   = sysfs_rename_node,
};

/* ------------------------------------------------------------------ */
/*  sysfs_create_dir / sysfs_remove_dir                                */
/* ------------------------------------------------------------------ */

int sysfs_create_dir(struct kobject *kobj)
{
    vfs_node_t parent_vnode;

    if (!kobj) return -EINVAL;
    if (kobj->state_in_sysfs && kobj->sd) return -EEXIST;

    /* Mark as in-sysfs even before VFS node creation.
     * If sysfs is not yet mounted, the VFS node is created
     * lazily by sysfs_populate_dir when the mount happens. */
    kobj->state_in_sysfs = 1;

    /* Determine the parent directory VFS node */
    if (kobj->parent && kobj->parent->sd) {
        parent_vnode = kobj->parent->sd;
    } else {
        parent_vnode = sysfs_root_vnode;
    }

    /* If sysfs is not mounted yet, defer VFS node creation */
    if (!parent_vnode) return EOK;

    vfs_node_t vnode = vfs_node_alloc(parent_vnode, kobj->name ? kobj->name : "unknown");
    if (!vnode) return -ENOMEM;

    vnode->type = file_dir;
    vnode->fsid = sysfs_id;

    sysfs_node_t *sn = sysfs_node_alloc(SYSFS_DIR);
    if (!sn) {
        /* Remove the VFS node we just created */
        parent_vnode->child = clist_delete(parent_vnode->child, vnode);
        vfs_free(vnode);
        return -ENOMEM;
    }

    sn->kobj     = kobj;
    vnode->handle = sn;
    kobj->sd      = vnode;

    return EOK;
}

void sysfs_remove_dir(struct kobject *kobj)
{
    if (!kobj) return;
    if (!kobj->state_in_sysfs || !kobj->sd) return;

    vfs_node_t vnode = kobj->sd;

    /* Remove from parent VFS children */
    if (vnode->parent) {
        vnode->parent->child = clist_delete(vnode->parent->child, vnode);
    }

    /* Free the VFS node and its handle */
    vfs_free(vnode);

    kobj->sd            = NULL;
    kobj->state_in_sysfs = 0;
}

/* ------------------------------------------------------------------ */
/*  sysfs_create_file / sysfs_remove_file                              */
/* ------------------------------------------------------------------ */

int sysfs_create_file(struct kobject *kobj, const struct attribute *attr)
{
    vfs_node_t dir_vnode;

    if (!kobj || !attr || !attr->name) return -EINVAL;

    /* Check for duplicates */
    if (sysfs_find_attr(kobj, attr->name)) return -EEXIST;

    dir_vnode = kobj->sd;
    if (!dir_vnode) {
        /* Kobject not yet in sysfs — defer creation */
        /* Just track the attribute for later */
        ;
    }

    /* Create the tracking entry */
    sysfs_attr_entry_t *entry = calloc(1, sizeof(sysfs_attr_entry_t));
    if (!entry) return -ENOMEM;

    entry->attr = (struct attribute *)attr; /* const cast — safe since attr is const in struct */
    entry->kobj = kobj;
    entry->vnode = NULL;

    kobj->attributes = clist_append(kobj->attributes, entry);

    /* If kobject is already in sysfs, create the VFS node immediately */
    if (dir_vnode) {
        vfs_node_t file_vn = vfs_node_alloc(dir_vnode, attr->name);
        if (file_vn) {
            file_vn->type        = file_stream;
            file_vn->fsid        = sysfs_id;
            file_vn->permissions = attr->mode;

            sysfs_node_t *sn = sysfs_node_alloc(SYSFS_ATTR);
            if (sn) {
                sn->kobj       = kobj;
                sn->attr       = entry->attr;
                file_vn->handle = sn;
            }
            entry->vnode = file_vn;
        }
    }

    return EOK;
}

void sysfs_remove_file(struct kobject *kobj, const struct attribute *attr)
{
    if (!kobj || !attr || !attr->name) return;

    sysfs_attr_entry_t *entry = sysfs_find_attr(kobj, attr->name);
    if (!entry) return;

    /* Remove VFS node from parent */
    if (entry->vnode) {
        vfs_node_t dir = entry->vnode->parent;
        if (dir) {
            dir->child = clist_delete(dir->child, entry->vnode);
        }
        vfs_free(entry->vnode);
        entry->vnode = NULL;
    }

    /* Remove from tracking list */
    kobj->attributes = clist_delete(kobj->attributes, entry);
    free(entry);
}

/* ------------------------------------------------------------------ */
/*  sysfs_create_bin_file / sysfs_remove_bin_file                      */
/* ------------------------------------------------------------------ */

int sysfs_create_bin_file(struct kobject *kobj,
                          const struct bin_attribute *attr)
{
    vfs_node_t dir_vnode;

    if (!kobj || !attr || !attr->attr.name) return -EINVAL;

    dir_vnode = kobj->sd;
    if (!dir_vnode) return -ENOENT;

    vfs_node_t file_vn = vfs_node_alloc(dir_vnode, attr->attr.name);
    if (!file_vn) return -ENOMEM;

    file_vn->type        = file_stream;
    file_vn->fsid        = sysfs_id;
    file_vn->permissions = attr->attr.mode;
    if (attr->size) file_vn->size = attr->size;

    sysfs_node_t *sn = sysfs_node_alloc(SYSFS_BIN_ATTR);
    if (sn) {
        sn->kobj      = kobj;
        sn->bin_attr  = (struct bin_attribute *)attr;
        file_vn->handle = sn;
    }

    return EOK;
}

void sysfs_remove_bin_file(struct kobject *kobj,
                           const struct bin_attribute *attr)
{
    if (!kobj || !attr || !attr->attr.name) return;

    /* Find and remove the VFS node */
    if (kobj->sd && kobj->sd->child) {
        vfs_node_t vnode = NULL;
        {
            clist_t cn;
            for (cn = kobj->sd->child; cn; cn = cn->next) {
                vfs_node_t cv = cn->data;
                if (cv && cv->name && streq(cv->name, attr->attr.name)) {
                    sysfs_node_t *sn = cv->handle;
                    if (sn && sn->type == SYSFS_BIN_ATTR && sn->bin_attr == attr) {
                        vnode = cv;
                        break;
                    }
                }
            }
        }
        if (vnode) {
            kobj->sd->child = clist_delete(kobj->sd->child, vnode);
            vfs_free(vnode);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  sysfs_create_symlink / sysfs_remove_symlink                        */
/* ------------------------------------------------------------------ */

int sysfs_create_symlink(struct kobject *kobj, struct kobject *target,
                         const char *name)
{
    vfs_node_t dir_vnode;

    if (!kobj || !target || !name) return -EINVAL;

    /* Check for duplicates */
    if (sysfs_find_symlink(kobj, name)) return -EEXIST;

    /* Create tracking entry */
    sysfs_symlink_entry_t *entry = calloc(1, sizeof(sysfs_symlink_entry_t));
    if (!entry) return -ENOMEM;

    entry->name   = strdup(name);
    entry->target = target;
    entry->vnode  = NULL;

    kobj->symlinks = clist_append(kobj->symlinks, entry);

    /* If kobject is already in sysfs, create VFS node immediately */
    dir_vnode = kobj->sd;
    if (dir_vnode) {
        vfs_node_t sym_vn = vfs_node_alloc(dir_vnode, name);
        if (sym_vn) {
            sym_vn->type = file_symlink;
            sym_vn->fsid = sysfs_id;

            sysfs_node_t *sn = sysfs_node_alloc(SYSFS_SYMLINK);
            if (sn) {
                sn->kobj         = kobj;
                sn->symlink_path = kobject_get_path(target);
                sym_vn->handle   = sn;
            }
            entry->vnode = sym_vn;
        }
    }

    return EOK;
}

void sysfs_remove_symlink(struct kobject *kobj, const char *name)
{
    if (!kobj || !name) return;

    sysfs_symlink_entry_t *entry = sysfs_find_symlink(kobj, name);
    if (!entry) return;

    /* Remove VFS node */
    if (entry->vnode) {
        vfs_node_t dir = entry->vnode->parent;
        if (dir) {
            dir->child = clist_delete(dir->child, entry->vnode);
        }
        vfs_free(entry->vnode);
        entry->vnode = NULL;
    }

    /* Remove from tracking list */
    kobj->symlinks = clist_delete(kobj->symlinks, entry);
    free((void *)entry->name);
    free(entry);
}

/* ------------------------------------------------------------------ */
/*  sysfs_create_group / sysfs_remove_group                            */
/* ------------------------------------------------------------------ */

int sysfs_create_group(struct kobject *kobj,
                       const struct attribute_group *grp)
{
    struct kobject   *target_kobj = kobj;
    int               ret;

    if (!kobj || !grp) return -EINVAL;

    /* If the group has a name, create a subdirectory */
    if (grp->name) {
        target_kobj = kobject_create_and_add(grp->name, kobj);
        if (!target_kobj) return -ENOMEM;
    }

    /* Create regular attribute files */
    if (grp->attrs) {
        struct attribute **attr;
        for (attr = grp->attrs; *attr; attr++) {
            if (!(*attr)->name) continue;

            /* Check visibility */
            if (grp->is_visible && !grp->is_visible(kobj, *attr, (int)(attr - grp->attrs))) {
                continue;
            }

            ret = sysfs_create_file(target_kobj, *attr);
            if (ret != EOK && ret != -EEXIST) {
                /* Clean up on failure */
                if (grp->name) kobject_del(target_kobj);
                return ret;
            }
        }
    }

    /* Create binary attribute files */
    if (grp->bin_attrs) {
        struct bin_attribute **bin;
        for (bin = grp->bin_attrs; *bin; bin++) {
            if (!(*bin)->attr.name) continue;

            if (grp->is_visible && !grp->is_visible(kobj, &(*bin)->attr, -1)) {
                continue;
            }

            ret = sysfs_create_bin_file(target_kobj, *bin);
            if (ret != EOK && ret != -EEXIST) {
                if (grp->name) kobject_del(target_kobj);
                return ret;
            }
        }
    }

    return EOK;
}

void sysfs_remove_group(struct kobject *kobj,
                        const struct attribute_group *grp)
{
    struct kobject *target_kobj;

    if (!kobj || !grp) return;

    /* If the group has a name, find the subdirectory kobject */
    if (grp->name) {
        target_kobj = sysfs_find_child_kobj(kobj, grp->name);
        if (!target_kobj) return;
    } else {
        target_kobj = kobj;
    }

    /* Remove attribute files */
    if (grp->attrs) {
        struct attribute **attr;
        for (attr = grp->attrs; *attr; attr++) {
            if (!(*attr)->name) continue;
            sysfs_remove_file(target_kobj, *attr);
        }
    }

    /* Remove binary attribute files */
    if (grp->bin_attrs) {
        struct bin_attribute **bin;
        for (bin = grp->bin_attrs; *bin; bin++) {
            if (!(*bin)->attr.name) continue;
            sysfs_remove_bin_file(target_kobj, *bin);
        }
    }

    /* Remove the subdirectory kobject if we created one */
    if (grp->name) {
        kobject_del(target_kobj);
    }
}

/* ------------------------------------------------------------------ */
/*  sysfs_create_groups / sysfs_remove_groups                          */
/* ------------------------------------------------------------------ */

int sysfs_create_groups(struct kobject *kobj,
                        const struct attribute_group **groups)
{
    int ret;

    if (!groups) return EOK;

    for (int i = 0; groups[i]; i++) {
        ret = sysfs_create_group(kobj, groups[i]);
        if (ret != EOK) {
            /* Rollback previously created groups */
            for (int j = i - 1; j >= 0; j--) {
                sysfs_remove_group(kobj, groups[j]);
            }
            return ret;
        }
    }

    return EOK;
}

void sysfs_remove_groups(struct kobject *kobj,
                         const struct attribute_group **groups)
{
    if (!groups) return;

    for (int i = 0; groups[i]; i++) {
        sysfs_remove_group(kobj, groups[i]);
    }
}

/* ------------------------------------------------------------------ */
/*  sysfs_cleanup_kobject_files                                        */
/* ------------------------------------------------------------------ */

void sysfs_cleanup_kobject_files(struct kobject *kobj)
{
    if (!kobj) return;

    /* Remove all attribute files */
    {
        clist_t node;
        clist_t next;
        for (node = kobj->attributes; node; node = next) {
            next = node->next;
            sysfs_attr_entry_t *entry = node->data;
            if (!entry) continue;

            /* Remove VFS node */
            if (entry->vnode) {
                vfs_node_t dir = entry->vnode->parent;
                if (dir) {
                    dir->child = clist_delete(dir->child, entry->vnode);
                }
                vfs_free(entry->vnode);
                entry->vnode = NULL;
            }

            /* Remove from tracking list */
            kobj->attributes = clist_delete(kobj->attributes, entry);
            free(entry);
        }
    }

    /* Remove all symlinks */
    {
        clist_t node;
        clist_t next;
        for (node = kobj->symlinks; node; node = next) {
            next = node->next;
            sysfs_symlink_entry_t *entry = node->data;
            if (!entry) continue;

            /* Remove VFS node */
            if (entry->vnode) {
                vfs_node_t dir = entry->vnode->parent;
                if (dir) {
                    dir->child = clist_delete(dir->child, entry->vnode);
                }
                vfs_free(entry->vnode);
                entry->vnode = NULL;
            }

            /* Remove from tracking list */
            kobj->symlinks = clist_delete(kobj->symlinks, entry);
            free((void *)entry->name);
            free(entry);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  sysfs_rename_dir                                                   */
/* ------------------------------------------------------------------ */

void sysfs_rename_dir(struct kobject *kobj, const char *new_name)
{
    if (!kobj || !new_name) return;
    /* The actual rename is handled by kobject_rename which
     * updates the VFS node name directly */
    (void)new_name;
}

/* ------------------------------------------------------------------ */
/*  sysfs_init / sysfs_regist                                          */
/* ------------------------------------------------------------------ */

void sysfs_regist(void)
{
    sysfs_id = vfs_regist_fs("sysfs", &sysfs_callbacks);
    if (sysfs_id & ERRNO_MASK) {
        plogk("sysfs: Register error.\n");
    }
}

/* Recursively create VFS nodes for pending kobjects after mount */
static void sysfs_populate_dir(struct kobject *kobj)
{
    clist_t node;
    if (!kobj || !kobj->sd) return;

    for (node = kobj->children; node; node = node->next) {
        struct kobject *child = node->data;
        if (!child || child->sd) continue;

        vfs_node_t vnode = vfs_node_alloc(kobj->sd, child->name ? child->name : "unknown");
        if (!vnode) continue;

        vnode->type = file_dir;
        vnode->fsid = sysfs_id;

        sysfs_node_t *sn = sysfs_node_alloc(SYSFS_DIR);
        if (!sn) continue;
        sn->kobj       = child;
        vnode->handle   = sn;
        child->sd       = vnode;
        child->state_in_sysfs = 1;

        /* Recurse into grandchildren */
        sysfs_populate_dir(child);
    }
}

int sysfs_init(void)
{
    /* Create the root kobject (only — mount creates the VFS nodes) */
    sysfs_root_kobj = calloc(1, sizeof(struct kobject));
    if (!sysfs_root_kobj) return -ENOMEM;

    kobject_init(sysfs_root_kobj, NULL);
    sysfs_root_kobj->name = strdup("sys");
    if (!sysfs_root_kobj->name) {
        free(sysfs_root_kobj);
        sysfs_root_kobj = NULL;
        return -ENOMEM;
    }

    sysfs_root_kobj->state_initialized = 1;
    sysfs_root_kobj->state_in_sysfs    = 1;

    /* Create top-level directory kobjects as children of the root.
     * Since sysfs isn't mounted yet, sysfs_create_dir will defer
     * VFS node creation until the mount callback populates them. */
    kobject_create_and_add("block", sysfs_root_kobj);
    kobject_create_and_add("bus", sysfs_root_kobj);
    kobject_create_and_add("class", sysfs_root_kobj);
    kobject_create_and_add("dev", sysfs_root_kobj);
    kobject_create_and_add("devices", sysfs_root_kobj);
    kobject_create_and_add("firmware", sysfs_root_kobj);
    kobject_create_and_add("fs", sysfs_root_kobj);
    kobject_create_and_add("kernel", sysfs_root_kobj);
    kobject_create_and_add("module", sysfs_root_kobj);
    kobject_create_and_add("power", sysfs_root_kobj);

    plogk("sysfs: Initialised (userspace will mount at /sys)\n");
    return EOK;
}
