/*
 *
 *      sysfs.c
 *      sysfs ?the filesystem for exporting kernel objects
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/kobject/kobject.h>
#include <libs/list/circular_list.h>
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

typedef enum sysfs_node_type {
    SYSFS_DIR,      // kobject directory
    SYSFS_ATTR,     // regular attribute file
    SYSFS_BIN_ATTR, // binary attribute file
    SYSFS_SYMLINK,  // symbolic link
} sysfs_node_type_t;

typedef struct sysfs_attr_entry {
        struct attribute *attr;
        vfs_node_t        vnode; // VFS node for this file
        struct kobject   *kobj;  // owning kobject
        uint16_t          mode;
} sysfs_attr_entry_t;

typedef struct sysfs_bin_attr_entry {
        struct bin_attribute *attr;
        vfs_node_t            vnode;
        struct kobject       *kobj;
        uint16_t              mode;
} sysfs_bin_attr_entry_t;

typedef struct sysfs_symlink_entry {
        const char     *name;
        struct kobject *target;
        vfs_node_t      vnode;
} sysfs_symlink_entry_t;

typedef struct sysfs_node {
        sysfs_node_type_t     type;
        struct kobject       *kobj;
        struct attribute     *attr;           // for SYSFS_ATTR
        struct bin_attribute *bin_attr;       // for SYSFS_BIN_ATTR
        struct kobject       *symlink_target; // for SYSFS_SYMLINK
        uint16_t              mode;
} sysfs_node_t;

typedef struct sysfs_open_file {
        sysfs_node_t   *node;
        struct kobject *kobj;
        char           *buffer;
        size_t          size;
        int             generated;
} sysfs_open_file_t;

/* ------------------------------------------------------------------ */
/*  Global state                                                       */
/* ------------------------------------------------------------------ */

static int        sysfs_id;         // VFS filesystem ID
struct kobject   *sysfs_root_kobj;  // /sys root kobject (global)
static vfs_node_t sysfs_root_vnode; // /sys mount point VFS node
struct kobject   *sysfs_dev_char_kobj;
struct kobject   *sysfs_dev_block_kobj;

/* Forward declarations */
static int  sysfs_stat(void *file, vfs_node_t node);
static void sysfs_populate_dir(struct kobject *kobj);
static void sysfs_unbind_dir(struct kobject *kobj);

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static sysfs_node_t *sysfs_node_alloc(sysfs_node_type_t type)
{
    sysfs_node_t *sn = calloc(1, sizeof(sysfs_node_t));
    if (!sn) return NULL;
    sn->type = type;
    return sn;
}

static void sysfs_node_free(sysfs_node_t *sn)
{
    if (!sn) return;
    kobject_put(sn->symlink_target);
    free(sn);
}

/* Look up a child kobject by name */
static struct kobject *sysfs_find_child_kobj(struct kobject *parent, const char *name)
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
static sysfs_attr_entry_t *sysfs_find_attr(struct kobject *kobj, const char *name)
{
    clist_t node;
    if (!kobj || !name) return NULL;

    for (node = kobj->attributes; node; node = node->next) {
        sysfs_attr_entry_t *entry = node->data;
        if (entry && entry->attr && entry->attr->name && streq(entry->attr->name, name)) return entry;
    }
    return NULL;
}

static sysfs_bin_attr_entry_t *sysfs_find_bin_attr(struct kobject *kobj, const char *name)
{
    if (!kobj || !name) return NULL;
    for (clist_t node = kobj->bin_attributes; node; node = node->next) {
        sysfs_bin_attr_entry_t *entry = node->data;
        if (entry && entry->attr && entry->attr->attr.name && streq(entry->attr->attr.name, name)) return entry;
    }
    return NULL;
}

/* Look up a symlink entry by name */
static sysfs_symlink_entry_t *sysfs_find_symlink(struct kobject *kobj, const char *name)
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
static int sysfs_name_valid(const char *name)
{
    return name && name[0] && !strchr(name, '/') && !streq(name, ".") && !streq(name, "..");
}

static int sysfs_name_exists(struct kobject *kobj, const char *name)
{
    return sysfs_find_child_kobj(kobj, name) || sysfs_find_attr(kobj, name) || sysfs_find_bin_attr(kobj, name) || sysfs_find_symlink(kobj, name);
}

static int sysfs_list_add(clist_t *list, void *data)
{
    clist_t node = clist_alloc(data);
    if (!node) return -ENOMEM;
    if (!*list) {
        *list = node;
    } else {
        clist_t tail = clist_tail(*list);
        tail->next   = node;
        node->prev   = tail;
    }
    return EOK;
}

static char *sysfs_relative_path(struct kobject *from, struct kobject *target)
{
    struct kobject *from_path[64];
    struct kobject *target_path[64];
    int             from_depth   = 0;
    int             target_depth = 0;

    for (struct kobject *node = from; node && from_depth < 64; node = node->parent) from_path[from_depth++] = node;
    for (struct kobject *node = target; node && target_depth < 64; node = node->parent) target_path[target_depth++] = node;
    if (!from_depth || !target_depth || from_path[from_depth - 1] != target_path[target_depth - 1]) return NULL;

    int from_index   = from_depth - 1;
    int target_index = target_depth - 1;
    while (from_index >= 0 && target_index >= 0 && from_path[from_index] == target_path[target_index]) {
        from_index--;
        target_index--;
    }

    size_t length = (size_t)(from_index + 1) * 3;
    for (int i = target_index; i >= 0; i--) length += strlen(target_path[i]->name) + (i ? 1 : 0);
    if (!length) length = 1;

    char *path = malloc(length + 1);
    if (!path) return NULL;
    path[0] = '\0';
    for (int i = 0; i <= from_index; i++) strcat(path, "../");
    for (int i = target_index; i >= 0; i--) {
        strcat(path, target_path[i]->name);
        if (i) strcat(path, "/");
    }
    if (!path[0]) strcpy(path, ".");
    return path;
}

/* ------------------------------------------------------------------ */
/*  Content generation (read path for attribute files)                 */
/* ------------------------------------------------------------------ */

static ssize_t sysfs_gen_attr_content(sysfs_node_t *sn, char **content)
{
    struct kobject   *kobj = sn->kobj;
    struct attribute *attr = sn->attr;

    if (!content) return -EINVAL;
    *content = NULL;
    if (!kobj || !attr || !kobj->ktype || !kobj->ktype->sysfs_ops || !kobj->ktype->sysfs_ops->show) return -EIO;

    char *buf = malloc(SYSFS_PAGE_SIZE);
    if (!buf) {
        plogk("sysfs: Attribute content allocation failed.\n");
        return -ENOMEM;
    }

    ssize_t n = kobj->ktype->sysfs_ops->show(kobj, attr, buf);
    if (n < 0) {
        free(buf);
        return n;
    }
    if (n > SYSFS_PAGE_SIZE) {
        free(buf);
        return -EOVERFLOW;
    }

    *content = buf;
    return n;
}

/* ------------------------------------------------------------------ */
/*  VFS callbacks                                                      */
/* ------------------------------------------------------------------ */

static int sysfs_mount(const char *handle, vfs_node_t node)
{
    /* sysfs is nodev; tolerate the conventional "sysfs" source operand. */
    (void)handle;
    if (!node || !sysfs_root_kobj) return -EINVAL;
    if (sysfs_root_vnode) return -EBUSY;

    node->fsid = sysfs_id;
    node->flags |= VFS_NODE_NOCACHE;

    sysfs_node_t *root_sn = sysfs_node_alloc(SYSFS_DIR);
    if (!root_sn) return -ENOMEM;

    root_sn->kobj = sysfs_root_kobj;
    node->handle  = root_sn;
    node->type    = file_dir;

    /* Link back */
    sysfs_root_vnode = node;
    if (sysfs_root_kobj) {
        sysfs_root_kobj->sd             = node;
        sysfs_root_kobj->state_in_sysfs = 1;

        /*
         * Populate VFS nodes for kobjects that were created
         * before the filesystem was mounted
         */
        sysfs_populate_dir(sysfs_root_kobj);
    }

    return EOK;
}

static void sysfs_umount(void *root)
{
    sysfs_unbind_dir(sysfs_root_kobj);
    sysfs_root_vnode = NULL;
    sysfs_node_free(root);
}

static void sysfs_open(void *parent_handle, const char *name, vfs_node_t node)
{
    node->flags |= VFS_NODE_NOCACHE;
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
                sn->kobj          = attr_entry->kobj;
                sn->attr          = attr_entry->attr;
                sn->mode          = attr_entry->mode;
                node->handle      = sn;
                node->type        = file_none;
                attr_entry->vnode = node;
                return;
            }

            /* Check for binary attribute ?these are set up when created */
            /* Binary files are created proactively, skip here */

            sysfs_bin_attr_entry_t *bin_entry = sysfs_find_bin_attr(parent_kobj, name);
            if (bin_entry) {
                sysfs_node_t *sn = sysfs_node_alloc(SYSFS_BIN_ATTR);
                if (!sn) return;
                sn->kobj         = bin_entry->kobj;
                sn->bin_attr     = bin_entry->attr;
                sn->mode         = bin_entry->mode;
                node->handle     = sn;
                node->type       = file_none;
                node->size       = bin_entry->attr->size;
                bin_entry->vnode = node;
                return;
            }

            /* Check for symlink */
            sysfs_symlink_entry_t *sym_entry = sysfs_find_symlink(parent_kobj, name);
            if (sym_entry) {
                sysfs_node_t *sn = sysfs_node_alloc(SYSFS_SYMLINK);
                if (!sn) return;
                sn->kobj           = parent_kobj;
                sn->symlink_target = kobject_get(sym_entry->target);
                if (!sn->symlink_target) {
                    sysfs_node_free(sn);
                    return;
                }
                node->handle     = sn;
                node->type       = file_symlink;
                sym_entry->vnode = node;
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
    node->flags |= VFS_NODE_NOCACHE;
    sysfs_node_t *sn = file;
    if (!sn) return -ENOENT;

    switch (sn->type) {
        case SYSFS_DIR : {
            struct kobject *kobj = sn->kobj;
            if (!kobj) break;

            /*
             * sysfs_create_*()/sysfs_remove_*() maintain VFS children
             * eagerly after the filesystem is mounted.  The directory stat
             * callback therefore only needs to perform the initial lazy
             * population.  Rewalking and de-duplicating every kobject and
             * attribute on every pathname component made udev's parallel
             * attribute scan serialize on the global VFS lock.
             */
            if (node->visited) break;

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
                        child_vn->type = file_dir;
                        child_vn->fsid = sysfs_id;

                        sysfs_node_t *child_sn = sysfs_node_alloc(SYSFS_DIR);
                        if (!child_sn) {
                            node->child = clist_delete(node->child, child_vn);
                            vfs_free(child_vn);
                            continue;
                        }
                        child_sn->kobj             = child_kobj;
                        child_vn->handle           = child_sn;
                        child_kobj->sd             = child_vn;
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
                    if (entry->vnode) continue; // already has VFS node

                    vfs_node_t file_vn = vfs_node_alloc(node, entry->attr->name);
                    if (!file_vn) continue;

                    file_vn->type        = file_none;
                    file_vn->fsid        = sysfs_id;
                    file_vn->permissions = entry->mode;

                    sysfs_node_t *file_sn = sysfs_node_alloc(SYSFS_ATTR);
                    if (!file_sn) {
                        node->child = clist_delete(node->child, file_vn);
                        vfs_free(file_vn);
                        continue;
                    }
                    file_sn->kobj   = entry->kobj;
                    file_sn->attr   = entry->attr;
                    file_sn->mode   = entry->mode;
                    file_vn->handle = file_sn;
                    entry->vnode    = file_vn;
                }
            }

            /* Enumerate binary attribute files */
            for (clist_t bin_node = kobj->bin_attributes; bin_node; bin_node = bin_node->next) {
                sysfs_bin_attr_entry_t *entry = bin_node->data;
                if (!entry || !entry->attr || !entry->attr->attr.name || entry->vnode) continue;

                vfs_node_t file_vn = vfs_node_alloc(node, entry->attr->attr.name);
                if (!file_vn) continue;
                file_vn->type        = file_none;
                file_vn->fsid        = sysfs_id;
                file_vn->permissions = entry->mode;
                file_vn->size        = entry->attr->size;

                sysfs_node_t *file_sn = sysfs_node_alloc(SYSFS_BIN_ATTR);
                if (!file_sn) {
                    node->child = clist_delete(node->child, file_vn);
                    vfs_free(file_vn);
                    continue;
                }
                file_sn->kobj     = entry->kobj;
                file_sn->bin_attr = entry->attr;
                file_sn->mode     = entry->mode;
                file_vn->handle   = file_sn;
                entry->vnode      = file_vn;
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
                    if (!sym_sn) {
                        node->child = clist_delete(node->child, sym_vn);
                        vfs_free(sym_vn);
                        continue;
                    }
                    sym_sn->kobj           = kobj;
                    sym_sn->symlink_target = kobject_get(entry->target);
                    if (!sym_sn->symlink_target) {
                        sysfs_node_free(sym_sn);
                        node->child = clist_delete(node->child, sym_vn);
                        vfs_free(sym_vn);
                        continue;
                    }
                    sym_vn->handle = sym_sn;
                    entry->vnode   = sym_vn;
                }
            }
            node->visited = 1;
            break;
        }
        case SYSFS_ATTR : {
            node->type = file_none;
            node->size = 0;
            break;
        }
        case SYSFS_BIN_ATTR : {
            node->type = file_none;
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
            char   *content = NULL;
            ssize_t length  = sysfs_gen_attr_content(sn, &content);
            if (length < 0) {
                if (length != -ENODEV && length != -EIO) {
                    plogk("sysfs: Show() for %s failed (%d)\n", sn->attr && sn->attr->name ? sn->attr->name : "?", (int)length);
                }
                return 0;
            }
            if (!length || offset >= (size_t)length) {
                free(content);
                return 0;
            }
            size_t actual = size > (size_t)length - offset ? (size_t)length - offset : size;
            memcpy(addr, content + offset, actual);
            free(content);
            return actual;
        }
        case SYSFS_BIN_ATTR : {
            if (!sn->bin_attr || !sn->bin_attr->read) return 0;
            ssize_t ret = sn->bin_attr->read(sn->kobj, sn->bin_attr, addr, (int64_t)offset, size);
            return ret < 0 ? (size_t)-1 : (size_t)ret;
        }
        case SYSFS_DIR :
        case SYSFS_SYMLINK :
            return 0; // cannot read a directory
    }

    return 0;
}

static size_t sysfs_write(void *file, const void *addr, size_t offset, size_t size)
{
    sysfs_node_t *sn = file;
    if (!sn || offset || size >= SYSFS_PAGE_SIZE) return 0;

    switch (sn->type) {
        case SYSFS_ATTR : {
            if (!(sn->mode & 0200)) return 0;
            if (!sn->kobj || !sn->kobj->ktype || !sn->kobj->ktype->sysfs_ops || !sn->kobj->ktype->sysfs_ops->store) return 0;
            char *buffer = malloc(size + 1);
            if (!buffer) return 0;
            memcpy(buffer, addr, size);
            buffer[size] = '\0';
            ssize_t ret  = sn->kobj->ktype->sysfs_ops->store(sn->kobj, sn->attr, buffer, size);
            free(buffer);
            if (ret < 0) return 0;
            return (size_t)ret;
        }
        case SYSFS_BIN_ATTR : {
            if (!sn->bin_attr || !sn->bin_attr->write) return 0;
            ssize_t ret = sn->bin_attr->write(sn->kobj, sn->bin_attr, (char *)addr, (int64_t)offset, size);
            return ret < 0 ? (size_t)-1 : (size_t)ret;
        }
        default :
            return 0;
    }
}

static int sysfs_file_open(vfs_node_t vnode, uint64_t flags, void **private_data)
{
    sysfs_node_t      *sn;
    sysfs_open_file_t *open_file;

    (void)flags;
    if (!vnode || !private_data || !(sn = vnode->handle)) return -EINVAL; // NOLINT(bugprone-assignment-in-if-condition)
    /*
     * Directories are valid open-file descriptions.  readdir(2), fstat(2)
     * and *at(2) operations use the vnode itself and do not need a private
     * sysfs stream; rejecting the open here makes libudev unable to inspect
     * /sys/bus and /sys/class.
     */
    if (sn->type == SYSFS_DIR) {
        *private_data = NULL;
        return EOK;
    }
    if (sn->type == SYSFS_SYMLINK) return -EINVAL;

    open_file = calloc(1, sizeof(*open_file));
    if (!open_file) return -ENOMEM;
    open_file->kobj = kobject_get(sn->kobj);
    if (!open_file->kobj) {
        free(open_file);
        return -ENODEV;
    }
    open_file->node = sn;
    *private_data   = open_file;
    return EOK;
}

static void sysfs_file_release(vfs_node_t vnode, void *private_data)
{
    sysfs_open_file_t *open_file = private_data;
    (void)vnode;
    if (!open_file) return;
    free(open_file->buffer);
    kobject_put(open_file->kobj);
    free(open_file);
}

static int64_t sysfs_file_read(vfs_node_t vnode, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    sysfs_open_file_t *open_file = private_data;
    sysfs_node_t      *sn;

    (void)vnode;
    (void)flags;
    if (!open_file || !addr || !(sn = open_file->node)) return -EINVAL; // NOLINT(bugprone-assignment-in-if-condition)

    if (sn->type == SYSFS_BIN_ATTR) {
        if (!sn->bin_attr || !sn->bin_attr->read) return -EIO;
        return sn->bin_attr->read(open_file->kobj, sn->bin_attr, addr, (int64_t)offset, size);
    }
    if (sn->type != SYSFS_ATTR) return -EINVAL;

    if (!open_file->generated || offset == 0) {
        free(open_file->buffer);
        open_file->buffer    = NULL;
        open_file->size      = 0;
        open_file->generated = 1;
        ssize_t length       = sysfs_gen_attr_content(sn, &open_file->buffer);
        if (length < 0) return length;
        open_file->size = (size_t)length;
    }
    if (offset >= open_file->size) return 0;
    if (size > open_file->size - offset) size = open_file->size - offset;
    memcpy(addr, open_file->buffer + offset, size);
    return (int64_t)size;
}

static int64_t sysfs_file_write(vfs_node_t vnode, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    sysfs_open_file_t *open_file = private_data;
    sysfs_node_t      *sn;

    (void)vnode;
    (void)flags;
    if (!open_file || (!addr && size) || !(sn = open_file->node)) return -EINVAL; // NOLINT(bugprone-assignment-in-if-condition)

    if (sn->type == SYSFS_BIN_ATTR) {
        if (!sn->bin_attr || !sn->bin_attr->write) return -EIO;
        return sn->bin_attr->write(open_file->kobj, sn->bin_attr, (char *)addr, (int64_t)offset, size);
    }
    if (sn->type != SYSFS_ATTR) return -EINVAL;
    if (offset) return -ESPIPE;
    if (size >= SYSFS_PAGE_SIZE) return -EFBIG;
    if (!(sn->mode & 0200)) return -EACCES;
    if (!open_file->kobj->ktype || !open_file->kobj->ktype->sysfs_ops || !open_file->kobj->ktype->sysfs_ops->store) return -EIO;

    char *buffer = malloc(size + 1);
    if (!buffer) return -ENOMEM;
    memcpy(buffer, addr, size);
    buffer[size] = '\0';
    ssize_t ret  = open_file->kobj->ktype->sysfs_ops->store(open_file->kobj, sn->attr, buffer, size);
    free(buffer);
    if (ret >= 0) {
        free(open_file->buffer);
        open_file->buffer    = NULL;
        open_file->size      = 0;
        open_file->generated = 0;
    }
    return ret;
}

static size_t sysfs_readlink(vfs_node_t node, void *addr, size_t offset, size_t size)
{
    sysfs_node_t *sn = node->handle;
    if (!sn || sn->type != SYSFS_SYMLINK) return 0;
    if (!sn->kobj || !sn->symlink_target) return 0;

    char *path = sysfs_relative_path(sn->kobj, sn->symlink_target);
    if (!path) return 0;
    size_t len = strlen(path);
    if (offset >= len) {
        free(path);
        return 0;
    }

    size_t actual = (offset + size > len) ? (len - offset) : size;
    memcpy(addr, path + offset, actual);
    free(path);
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
    if (!node) return NULL;
    sysfs_node_t *old_sn = node->handle;
    sysfs_node_t *new_sn = NULL;
    vfs_node_t    copy   = vfs_node_alloc(node->parent, node->name);
    if (!copy) return NULL;

    copy->type        = node->type;
    copy->size        = node->size;
    copy->flags       = node->flags;
    copy->permissions = node->permissions;
    copy->owner       = node->owner;
    copy->fsid        = node->fsid;

    if (old_sn) {
        new_sn = sysfs_node_alloc(old_sn->type);
        if (!new_sn) goto err_copy;
        new_sn->kobj     = old_sn->kobj;
        new_sn->attr     = old_sn->attr;
        new_sn->bin_attr = old_sn->bin_attr;
        new_sn->mode     = old_sn->mode;
        if (old_sn->symlink_target) {
            new_sn->symlink_target = kobject_get(old_sn->symlink_target);
            if (!new_sn->symlink_target) goto err_new_sn;
        }
        copy->handle = new_sn;
    }

    return copy;

err_new_sn:
    sysfs_node_free(new_sn);

err_copy:
    if (copy->parent) copy->parent->child = clist_delete(copy->parent->child, copy);
    vfs_free(copy);
    return NULL;
}

static int sysfs_poll(void *file, size_t events)
{
    (void)file;
    int revents = 0;
    if (events & 0x0001) revents |= 0x0001; // POLLIN
    if (events & 0x0004) revents |= 0x0004; // POLLOUT
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
    .mount        = sysfs_mount,
    .unmount      = sysfs_umount,
    .open         = sysfs_open,
    .close        = sysfs_close,
    .read         = sysfs_read,
    .write        = sysfs_write,
    .readlink     = sysfs_readlink,
    .mkdir        = sysfs_mkdir,
    .mkfile       = sysfs_mkfile,
    .link         = sysfs_mkfile,
    .symlink      = sysfs_mkfile,
    .stat         = sysfs_stat,
    .ioctl        = sysfs_ioctl,
    .dup          = sysfs_dup,
    .poll         = sysfs_poll,
    .free         = sysfs_free,
    .delete       = sysfs_delete,
    .rename       = sysfs_rename_node,
    .file_open    = sysfs_file_open,
    .file_release = sysfs_file_release,
    .file_read    = sysfs_file_read,
    .file_write   = sysfs_file_write,
};

/* ------------------------------------------------------------------ */
/*  sysfs_create_dir / sysfs_remove_dir                                */
/* ------------------------------------------------------------------ */

int sysfs_create_dir(struct kobject *kobj)
{
    vfs_node_t parent_vnode;

    if (!kobj) return -EINVAL;
    if (kobj->state_in_sysfs) return -EEXIST;
    if (!kobj->name || !kobj->name[0]) return -EINVAL;
    if (kobj->parent) {
        struct kobject *collision = sysfs_find_child_kobj(kobj->parent, kobj->name);
        if ((collision && collision != kobj) || sysfs_find_attr(kobj->parent, kobj->name) || sysfs_find_bin_attr(kobj->parent, kobj->name)
            || sysfs_find_symlink(kobj->parent, kobj->name))
            return -EEXIST;
    }

    /*
     * Mark as in-sysfs even before VFS node creation.
     * If sysfs is not yet mounted, the VFS node is created
     * lazily by sysfs_populate_dir when the mount happens.
     */
    kobj->state_in_sysfs = 1;

    /* Determine the parent directory VFS node */
    if (kobj->parent)
        parent_vnode = kobj->parent->sd;
    else
        parent_vnode = sysfs_root_vnode;

    /* If sysfs is not mounted yet, defer VFS node creation */
    if (!parent_vnode) return EOK;

    vfs_node_t vnode = vfs_node_alloc(parent_vnode, kobj->name ? kobj->name : "unknown");
    if (!vnode) {
        kobj->state_in_sysfs = 0;
        return -ENOMEM;
    }

    vnode->type = file_dir;
    vnode->fsid = sysfs_id;

    sysfs_node_t *sn = sysfs_node_alloc(SYSFS_DIR);
    if (!sn) {
        /* Remove the VFS node we just created */
        parent_vnode->child = clist_delete(parent_vnode->child, vnode);
        vfs_free(vnode);
        kobj->state_in_sysfs = 0;
        return -ENOMEM;
    }

    sn->kobj      = kobj;
    vnode->handle = sn;
    kobj->sd      = vnode;

    return EOK;
}

void sysfs_remove_dir(struct kobject *kobj)
{
    if (!kobj) return;
    if (!kobj->state_in_sysfs) return;

    if (!kobj->sd) {
        kobj->state_in_sysfs = 0;
        return;
    }

    vfs_node_t vnode = kobj->sd;

    sysfs_unbind_dir(kobj);
    kobj->state_in_sysfs = 0;
    vfs_namespace_detach(vnode);
}

/* ------------------------------------------------------------------ */
/*  sysfs_create_file / sysfs_remove_file                              */
/* ------------------------------------------------------------------ */

static int sysfs_create_file_mode(struct kobject *dir_kobj, struct kobject *owner, const struct attribute *attr, uint16_t mode)
{
    vfs_node_t dir_vnode;

    if (!dir_kobj || !owner || !attr || !sysfs_name_valid(attr->name)) return -EINVAL;

    /* Check for duplicates */
    if (sysfs_name_exists(dir_kobj, attr->name)) return -EEXIST;

    dir_vnode = dir_kobj->sd;
    if (!dir_vnode) {
        /* Kobject not yet in sysfs ?defer creation */
        /* Just track the attribute for later */
        ;
    }

    /* Create the tracking entry */
    sysfs_attr_entry_t *entry = calloc(1, sizeof(sysfs_attr_entry_t));
    if (!entry) return -ENOMEM;

    entry->attr  = (struct attribute *)attr; // const cast ?safe since attr is const in struct
    entry->kobj  = owner;
    entry->mode  = mode;
    entry->vnode = NULL;

    int ret = sysfs_list_add(&dir_kobj->attributes, entry);
    if (ret != EOK) {
        free(entry);
        return ret;
    }

    /* If kobject is already in sysfs, create the VFS node immediately */
    if (dir_vnode) {
        vfs_node_t file_vn = vfs_node_alloc(dir_vnode, attr->name);
        if (!file_vn) {
            dir_kobj->attributes = clist_delete(dir_kobj->attributes, entry);
            free(entry);
            return -ENOMEM;
        }
        file_vn->type        = file_none;
        file_vn->fsid        = sysfs_id;
        file_vn->permissions = mode;

        sysfs_node_t *sn = sysfs_node_alloc(SYSFS_ATTR);
        if (!sn) {
            dir_vnode->child = clist_delete(dir_vnode->child, file_vn);
            vfs_free(file_vn);
            dir_kobj->attributes = clist_delete(dir_kobj->attributes, entry);
            free(entry);
            return -ENOMEM;
        }
        sn->kobj        = owner;
        sn->attr        = entry->attr;
        sn->mode        = mode;
        file_vn->handle = sn;
        entry->vnode    = file_vn;
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
        vfs_namespace_detach(entry->vnode);
        entry->vnode = NULL;
    }

    /* Remove from tracking list */
    kobj->attributes = clist_delete(kobj->attributes, entry);
    free(entry);
}

/* ------------------------------------------------------------------ */
/*  sysfs_create_bin_file / sysfs_remove_bin_file                      */
/* ------------------------------------------------------------------ */

static int sysfs_create_bin_file_mode(struct kobject *dir_kobj, struct kobject *owner, const struct bin_attribute *attr, uint16_t mode)
{
    if (!dir_kobj || !owner || !attr || !sysfs_name_valid(attr->attr.name)) return -EINVAL;
    if (sysfs_name_exists(dir_kobj, attr->attr.name)) return -EEXIST;

    sysfs_bin_attr_entry_t *entry = calloc(1, sizeof(*entry));
    if (!entry) return -ENOMEM;
    entry->attr = (struct bin_attribute *)attr;
    entry->kobj = owner;
    entry->mode = mode;
    int ret     = sysfs_list_add(&dir_kobj->bin_attributes, entry);
    if (ret != EOK) {
        free(entry);
        return ret;
    }

    vfs_node_t dir_vnode = dir_kobj->sd;
    if (!dir_vnode) return EOK;

    vfs_node_t file_vn = vfs_node_alloc(dir_vnode, attr->attr.name);
    if (!file_vn) goto err_entry;

    file_vn->type        = file_none;
    file_vn->fsid        = sysfs_id;
    file_vn->permissions = mode;
    if (attr->size) file_vn->size = attr->size;

    sysfs_node_t *sn = sysfs_node_alloc(SYSFS_BIN_ATTR);
    if (!sn) {
        dir_vnode->child = clist_delete(dir_vnode->child, file_vn);
        vfs_free(file_vn);
        goto err_entry;
    }
    sn->kobj        = owner;
    sn->bin_attr    = (struct bin_attribute *)attr;
    sn->mode        = mode;
    file_vn->handle = sn;
    entry->vnode    = file_vn;

    return EOK;

err_entry:
    dir_kobj->bin_attributes = clist_delete(dir_kobj->bin_attributes, entry);
    free(entry);
    return -ENOMEM;
}

int sysfs_create_bin_file(struct kobject *kobj, const struct bin_attribute *attr)
{
    return sysfs_create_bin_file_mode(kobj, kobj, attr, attr ? attr->attr.mode : 0);
}

void sysfs_remove_bin_file(struct kobject *kobj, const struct bin_attribute *attr)
{
    if (!kobj || !attr || !attr->attr.name) return;

    sysfs_bin_attr_entry_t *entry = sysfs_find_bin_attr(kobj, attr->attr.name);
    if (!entry || entry->attr != attr) return;
    if (entry->vnode) vfs_namespace_detach(entry->vnode);
    kobj->bin_attributes = clist_delete(kobj->bin_attributes, entry);
    free(entry);
}

/* ------------------------------------------------------------------ */
/*  sysfs_create_symlink / sysfs_remove_symlink                        */
/* ------------------------------------------------------------------ */

int sysfs_create_symlink(struct kobject *kobj, struct kobject *target, const char *name)
{
    vfs_node_t dir_vnode;

    if (!kobj || !target || !sysfs_name_valid(name)) return -EINVAL;

    /* Check for duplicates */
    if (sysfs_name_exists(kobj, name)) return -EEXIST;

    /* Create tracking entry */
    sysfs_symlink_entry_t *entry = calloc(1, sizeof(sysfs_symlink_entry_t));
    if (!entry) return -ENOMEM;

    entry->name = strdup(name);
    if (!entry->name) {
        free(entry);
        return -ENOMEM;
    }
    entry->target = kobject_get(target);
    if (!entry->target) {
        free((void *)entry->name);
        free(entry);
        return -ENODEV;
    }
    entry->vnode = NULL;

    int ret = sysfs_list_add(&kobj->symlinks, entry);
    if (ret != EOK) {
        kobject_put(entry->target);
        free((void *)entry->name);
        free(entry);
        return ret;
    }

    /* If kobject is already in sysfs, create VFS node immediately */
    dir_vnode = kobj->sd;
    if (dir_vnode) {
        vfs_node_t sym_vn = vfs_node_alloc(dir_vnode, name);
        if (!sym_vn) goto err_entry;
        sym_vn->type = file_symlink;
        sym_vn->fsid = sysfs_id;

        sysfs_node_t *sn = sysfs_node_alloc(SYSFS_SYMLINK);
        if (!sn) {
            dir_vnode->child = clist_delete(dir_vnode->child, sym_vn);
            vfs_free(sym_vn);
            goto err_entry;
        }
        sn->kobj           = kobj;
        sn->symlink_target = kobject_get(target);
        if (!sn->symlink_target) {
            sysfs_node_free(sn);
            dir_vnode->child = clist_delete(dir_vnode->child, sym_vn);
            vfs_free(sym_vn);
            goto err_entry;
        }
        sym_vn->handle = sn;
        entry->vnode   = sym_vn;
    }

    return EOK;

err_entry:
    kobj->symlinks = clist_delete(kobj->symlinks, entry);
    kobject_put(entry->target);
    free((void *)entry->name);
    free(entry);
    return -ENOMEM;
}

void sysfs_remove_symlink(struct kobject *kobj, const char *name)
{
    if (!kobj || !name) return;

    sysfs_symlink_entry_t *entry = sysfs_find_symlink(kobj, name);
    if (!entry) return;

    /* Remove VFS node */
    if (entry->vnode) {
        vfs_namespace_detach(entry->vnode);
        entry->vnode = NULL;
    }

    /* Remove from tracking list */
    kobj->symlinks = clist_delete(kobj->symlinks, entry);
    kobject_put(entry->target);
    free((void *)entry->name);
    free(entry);
}

/* ------------------------------------------------------------------ */
/*  sysfs_create_group / sysfs_remove_group                            */
/* ------------------------------------------------------------------ */

int sysfs_create_group(struct kobject *kobj, const struct attribute_group *grp)
{
    struct kobject        *target_kobj       = kobj;
    struct attribute     **created_attrs     = NULL;
    struct bin_attribute **created_bin_attrs = NULL;
    size_t                 attr_capacity     = 0;
    size_t                 bin_capacity      = 0;
    size_t                 attr_count        = 0;
    size_t                 bin_count         = 0;
    int                    ret;

    if (!kobj || !grp) return -EINVAL;

    if (grp->attrs)
        while (grp->attrs[attr_capacity]) attr_capacity++;
    if (grp->bin_attrs)
        while (grp->bin_attrs[bin_capacity]) bin_capacity++;

    if (attr_capacity) {
        created_attrs = calloc(attr_capacity, sizeof(*created_attrs)); // NOLINT(bugprone-sizeof-expression)
        if (!created_attrs) return -ENOMEM;
    }
    if (bin_capacity) {
        created_bin_attrs = calloc(bin_capacity, sizeof(*created_bin_attrs)); // NOLINT(bugprone-sizeof-expression)
        if (!created_bin_attrs) {
            free(created_attrs);
            return -ENOMEM;
        }
    }

    /* If the group has a name, create a subdirectory */
    if (grp->name) {
        if (!sysfs_name_valid(grp->name)) {
            ret = -EINVAL;
            goto err_group;
        }
        if (sysfs_name_exists(kobj, grp->name)) {
            ret = -EEXIST;
            goto err_group;
        }
        target_kobj = kobject_create_and_add(grp->name, kobj);
        if (!target_kobj) {
            ret = -ENOMEM;
            goto err_group;
        }
    }

    /* Create regular attribute files */
    if (grp->attrs) {
        struct attribute **attr;
        for (attr = grp->attrs; *attr; attr++) {
            if (!(*attr)->name) continue;

            uint16_t mode = (*attr)->mode;
            if (grp->is_visible) {
                int visible = grp->is_visible(kobj, *attr, (int)(attr - grp->attrs));
                if (!visible) continue;
                mode = (uint16_t)visible;
            }

            ret = sysfs_create_file_mode(target_kobj, kobj, *attr, mode);
            if (ret != EOK) goto err_group;
            created_attrs[attr_count++] = *attr;
        }
    }

    /* Create binary attribute files */
    if (grp->bin_attrs) {
        struct bin_attribute **bin;
        for (bin = grp->bin_attrs; *bin; bin++) {
            if (!(*bin)->attr.name) continue;

            uint16_t mode = (*bin)->attr.mode;
            if (grp->is_visible) {
                int visible = grp->is_visible(kobj, &(*bin)->attr, (int)(bin - grp->bin_attrs));
                if (!visible) continue;
                mode = (uint16_t)visible;
            }

            ret = sysfs_create_bin_file_mode(target_kobj, kobj, *bin, mode);
            if (ret != EOK) goto err_group;
            created_bin_attrs[bin_count++] = *bin;
        }
    }

    free(created_attrs);
    free(created_bin_attrs);
    return EOK;

err_group:
    while (attr_count) sysfs_remove_file(target_kobj, created_attrs[--attr_count]);
    while (bin_count) sysfs_remove_bin_file(target_kobj, created_bin_attrs[--bin_count]);
    if (grp->name && target_kobj != kobj) {
        kobject_del(target_kobj);
        kobject_put(target_kobj);
    }
    free(created_attrs);
    free(created_bin_attrs);
    return ret;
}

void sysfs_remove_group(struct kobject *kobj, const struct attribute_group *grp)
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
        kobject_put(target_kobj);
    }
}

/* ------------------------------------------------------------------ */
/*  sysfs_create_groups / sysfs_remove_groups                          */
/* ------------------------------------------------------------------ */

int sysfs_create_groups(struct kobject *kobj, const struct attribute_group **groups)
{
    int ret;

    if (!groups) return EOK;

    for (int i = 0; groups[i]; i++) {
        ret = sysfs_create_group(kobj, groups[i]);
        if (ret != EOK) {
            /* Rollback previously created groups */
            for (int j = i - 1; j >= 0; j--) sysfs_remove_group(kobj, groups[j]);
            return ret;
        }
    }

    return EOK;
}

void sysfs_remove_groups(struct kobject *kobj, const struct attribute_group **groups)
{
    if (!groups) return;

    for (int i = 0; groups[i]; i++) sysfs_remove_group(kobj, groups[i]);
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
            next                      = node->next;
            sysfs_attr_entry_t *entry = node->data;
            if (!entry) continue;

            /* Remove VFS node */
            if (entry->vnode) {
                vfs_namespace_detach(entry->vnode);
                entry->vnode = NULL;
            }

            /* Remove from tracking list */
            kobj->attributes = clist_delete(kobj->attributes, entry);
            free(entry);
        }
    }

    /* Remove all binary attribute files */
    {
        clist_t node;
        clist_t next;
        for (node = kobj->bin_attributes; node; node = next) {
            next                          = node->next;
            sysfs_bin_attr_entry_t *entry = node->data;
            if (!entry) continue;
            if (entry->vnode) vfs_namespace_detach(entry->vnode);
            kobj->bin_attributes = clist_delete(kobj->bin_attributes, entry);
            free(entry);
        }
    }

    /* Remove all symlinks */
    {
        clist_t node;
        clist_t next;
        for (node = kobj->symlinks; node; node = next) {
            next                         = node->next;
            sysfs_symlink_entry_t *entry = node->data;
            if (!entry) continue;

            /* Remove VFS node */
            if (entry->vnode) {
                vfs_namespace_detach(entry->vnode);
                entry->vnode = NULL;
            }

            /* Remove from tracking list */
            kobj->symlinks = clist_delete(kobj->symlinks, entry);
            kobject_put(entry->target);
            free((void *)entry->name);
            free(entry);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  sysfs_rename_dir                                                   */
/* ------------------------------------------------------------------ */

int sysfs_rename_dir(struct kobject *kobj, const char *new_name)
{
    if (!kobj || !new_name || !new_name[0]) return -EINVAL;
    if (kobj->parent) {
        struct kobject *collision = sysfs_find_child_kobj(kobj->parent, new_name);
        if ((collision && collision != kobj) || sysfs_find_attr(kobj->parent, new_name) || sysfs_find_bin_attr(kobj->parent, new_name)
            || sysfs_find_symlink(kobj->parent, new_name))
            return -EEXIST;
    }
    if (!kobj->sd) return EOK;

    char *replacement = strdup(new_name);
    if (!replacement) return -ENOMEM;
    free(kobj->sd->name);
    kobj->sd->name = replacement;
    if (kobj->sd->parent) kobj->sd->parent->visited = 0;
    return EOK;
}

int sysfs_create_file(struct kobject *kobj, const struct attribute *attr)
{
    return sysfs_create_file_mode(kobj, kobj, attr, attr ? attr->mode : 0);
}

int sysfs_move_dir(struct kobject *kobj, struct kobject *new_parent)
{
    if (!kobj || !new_parent) return -EINVAL;
    struct kobject *collision = sysfs_find_child_kobj(new_parent, kobj->name);
    if ((collision && collision != kobj) || sysfs_find_attr(new_parent, kobj->name) || sysfs_find_bin_attr(new_parent, kobj->name)
        || sysfs_find_symlink(new_parent, kobj->name))
        return -EEXIST;
    if (!kobj->sd && !new_parent->sd) return EOK;
    if (!kobj->sd || !new_parent->sd) return -ENOENT;
    for (clist_t node = new_parent->sd->child; node; node = node->next) {
        vfs_node_t child = node->data;
        if (child != kobj->sd && child && child->name && streq(child->name, kobj->name)) return -EEXIST;
    }

    clist_t new_link = clist_alloc(kobj->sd);
    if (!new_link) return -ENOMEM;

    vfs_node_t old_parent = kobj->sd->parent;
    if (old_parent) old_parent->child = clist_delete(old_parent->child, kobj->sd);
    kobj->sd->parent = new_parent->sd;
    new_link->next   = new_parent->sd->child;
    if (new_link->next) new_link->next->prev = new_link;
    new_parent->sd->child = new_link;
    if (old_parent) old_parent->visited = 0;
    new_parent->sd->visited = 0;
    return EOK;
}

/* ------------------------------------------------------------------ */
/*  sysfs_init / sysfs_regist                                          */
/* ------------------------------------------------------------------ */

void sysfs_regist(void)
{
#if CONFIG_SYSFS
    sysfs_id = vfs_regist_fs_flags("sysfs", &sysfs_callbacks, VFS_FS_NODEV);
    if (!(sysfs_id & ERRNO_MASK)) plogk("sysfs: Filesystem registered (fsid=%d)\n", sysfs_id);
    if (sysfs_id & ERRNO_MASK) plogk("sysfs: Register error.\n");
#endif
}

/* Recursively create VFS nodes for pending kobjects after mount */
static void sysfs_populate_symlinks(struct kobject *kobj)
{
    if (!kobj || !kobj->sd) return;

    /*
     * Symlinks created before sysfs was mounted are tracked in the
     * kobject, but have no VFS node yet.  They must be materialized during
     * the mount walk: vfs_readdir() enumerates the VFS child list directly
     * and does not call sysfs_stat() first.  Without this step, class links
     * such as /sys/class/drm/card0 are invisible to libudev, even though
     * the corresponding DRM device is registered and /dev/dri exists.
     */
    for (clist_t node = kobj->symlinks; node; node = node->next) {
        sysfs_symlink_entry_t *entry = node->data;
        if (!entry || !entry->name || entry->vnode || !entry->target) continue;

        vfs_node_t sym_vn = vfs_node_alloc(kobj->sd, entry->name);
        if (!sym_vn) continue;
        sym_vn->type = file_symlink;
        sym_vn->fsid = sysfs_id;

        sysfs_node_t *sn = sysfs_node_alloc(SYSFS_SYMLINK);
        if (!sn) {
            kobj->sd->child = clist_delete(kobj->sd->child, sym_vn);
            vfs_free(sym_vn);
            continue;
        }

        sn->kobj           = kobj;
        sn->symlink_target = kobject_get(entry->target);
        if (!sn->symlink_target) {
            sysfs_node_free(sn);
            kobj->sd->child = clist_delete(kobj->sd->child, sym_vn);
            vfs_free(sym_vn);
            continue;
        }

        sym_vn->handle = sn;
        entry->vnode   = sym_vn;
    }
}

static void sysfs_populate_dir(struct kobject *kobj)
{
    clist_t node;
    if (!kobj || !kobj->sd) return;

    /* Materialize class/device links before walking children. */
    sysfs_populate_symlinks(kobj);

    for (node = kobj->children; node; node = node->next) {
        struct kobject *child = node->data;
        if (!child || child->sd) continue;

        vfs_node_t vnode = vfs_node_alloc(kobj->sd, child->name ? child->name : "unknown");
        if (!vnode) continue;

        vnode->type = file_dir;
        vnode->fsid = sysfs_id;

        sysfs_node_t *sn = sysfs_node_alloc(SYSFS_DIR);
        if (!sn) {
            kobj->sd->child = clist_delete(kobj->sd->child, vnode);
            vfs_free(vnode);
            continue;
        }
        sn->kobj              = child;
        vnode->handle         = sn;
        child->sd             = vnode;
        child->state_in_sysfs = 1;

        /* Recurse into grandchildren */
        sysfs_populate_dir(child);
    }
}

/*
 * VFS tears down its nodes before invoking ->unmount().  Clear every
 * metadata back-pointer without dereferencing the already-freed vnodes so
 * a later userspace mount can materialize a fresh namespace.
 */
static void sysfs_unbind_dir(struct kobject *kobj)
{
    if (!kobj) return;

    for (clist_t node = kobj->attributes; node; node = node->next) {
        sysfs_attr_entry_t *entry = node->data;
        if (entry) entry->vnode = NULL;
    }
    for (clist_t node = kobj->bin_attributes; node; node = node->next) {
        sysfs_bin_attr_entry_t *entry = node->data;
        if (entry) entry->vnode = NULL;
    }
    for (clist_t node = kobj->symlinks; node; node = node->next) {
        sysfs_symlink_entry_t *entry = node->data;
        if (entry) entry->vnode = NULL;
    }
    for (clist_t node = kobj->children; node; node = node->next) sysfs_unbind_dir(node->data);
    kobj->sd = NULL;
}

static void sysfs_root_release(struct kobject *kobj)
{
    free(kobj);
}

static struct kobj_type sysfs_root_ktype = {
    .release = sysfs_root_release,
};

int sysfs_init(void)
{
#if CONFIG_SYSFS
    static const char *const top_level_names[] = {
        "block", "bus", "class", "dev", "devices", "firmware", "fs", "kernel", "module", "power",
    };

    if (sysfs_root_kobj) return -EEXIST;

    /* Create the root kobject (only ?mount creates the VFS nodes) */
    sysfs_root_kobj = calloc(1, sizeof(struct kobject));
    if (!sysfs_root_kobj) return -ENOMEM;

    kobject_init(sysfs_root_kobj, &sysfs_root_ktype);
    int ret = kobject_set_name(sysfs_root_kobj, "%s", "sys");
    if (ret != EOK) goto err_root;

    sysfs_root_kobj->state_in_sysfs = 1;

    /*
     * Create top-level directory kobjects as children of the root.
     * Since sysfs isn't mounted yet, sysfs_create_dir will defer
     * VFS node creation until the mount callback populates them.
     */
    for (size_t i = 0; i < sizeof(top_level_names) / sizeof(top_level_names[0]); i++) {
        if (!kobject_create_and_add(top_level_names[i], sysfs_root_kobj)) {
            ret = -ENOMEM;
            goto err_children;
        }
    }

    /*
     * cgroup2 is mounted on this kernel-owned sysfs mountpoint.  sysfs is
     * intentionally read-only to userspace, so the directory must exist in
     * the kernel object tree before OpenRC/elogind attempt mount(2), matching
     * Linux's /sys/fs/cgroup ABI.
     */
    struct kobject *fs_kobj = sysfs_find_child_kobj(sysfs_root_kobj, "fs");
    if (!fs_kobj || !kobject_create_and_add("cgroup", fs_kobj)) {
        ret = -ENOMEM;
        goto err_children;
    }

    /*
     * /sys/dev/char and /sys/dev/block hold the major:minor -> device
     * symlinks that udev uses to resolve a device number to a node.
     */
    struct kobject *dev_kobj = sysfs_find_child_kobj(sysfs_root_kobj, "dev");
    if (dev_kobj) {
        sysfs_dev_char_kobj  = kobject_create_and_add("char", dev_kobj);
        sysfs_dev_block_kobj = kobject_create_and_add("block", dev_kobj);
        if (!sysfs_dev_char_kobj || !sysfs_dev_block_kobj) {
            ret = -ENOMEM;
            goto err_children;
        }
    }

    return EOK;

err_children:
    while (sysfs_root_kobj->children) {
        struct kobject *child = sysfs_root_kobj->children->data;
        kobject_put(child);
    }
err_root:
    sysfs_root_kobj->state_in_sysfs = 0;
    kobject_put(sysfs_root_kobj);
    sysfs_root_kobj = NULL;
    return ret;
#else
    return EOK;
#endif
}
