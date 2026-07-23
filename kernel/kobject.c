/*
 *
 *      kobject.c
 *      Kernel object model implementation
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/sysfs.h>
#include <fs/vfs.h>
#include <ipc/netlink.h>
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
/*  Static helpers                                                     */
/* ------------------------------------------------------------------ */

/* Default release function for dynamically-allocated kobjects */
static void dynamic_kobj_release(struct kobject *kobj)
{
    if (kobj->name) { free((void *)kobj->name); }
    free(kobj);
}

static struct kobj_type dynamic_kobj_ktype = {
    .release       = dynamic_kobj_release,
    .sysfs_ops     = NULL,
    .default_attrs = NULL,
};

/* ------------------------------------------------------------------ */
/*  kobject_init                                                       */
/* ------------------------------------------------------------------ */

void kobject_init(struct kobject *kobj, struct kobj_type *ktype)
{
    if (!kobj) return;

    /* NOTE: does NOT zero the struct. The caller is responsible
     * for providing a pre-zeroed kobject (e.g. via calloc).
     * Fields that were set before init (like name) are preserved. */
    kobj->ktype      = ktype;
    kobj->kset       = NULL;
    kobj->parent     = NULL;
    kobj->sd         = NULL;
    kobj->children   = NULL;
    kobj->attributes = NULL;
    kobj->symlinks   = NULL;
    memset(&kobj->lock, 0, sizeof(kobj->lock));
    kref_init(&kobj->kref);
    kobj->state_initialized = 1;
    kobj->state_in_sysfs    = 0;
}

/* ------------------------------------------------------------------ */
/*  kobject_set_name                                                   */
/* ------------------------------------------------------------------ */

int kobject_set_name(struct kobject *kobj, const char *fmt, ...)
{
    char    buf[KOBJ_NAME_LEN];
    va_list args;
    int     n;

    if (!kobj) return -EINVAL;

    va_start(args, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (n < 0) return -EINVAL;

    /* Free old name if it was dynamically allocated */
    if (kobj->name) {
        /* Only free if the kobject owns it (set via kobject_set_name) */
        free((void *)kobj->name);
    }

    kobj->name = strdup(buf);
    if (!kobj->name) return -ENOMEM;

    return EOK;
}

/* ------------------------------------------------------------------ */
/*  kobject_add                                                        */
/* ------------------------------------------------------------------ */

int kobject_add(struct kobject *kobj, struct kobject *parent, const char *fmt, ...)
{
    va_list args;
    char    namebuf[KOBJ_NAME_LEN];
    int     ret;

    if (!kobj) return -EINVAL;
    if (!kobj->state_initialized) return -EINVAL;

    /* Set the name */
    if (fmt) {
        va_start(args, fmt);
        vsnprintf(namebuf, sizeof(namebuf), fmt, args);
        va_end(args);

        ret = kobject_set_name(kobj, "%s", namebuf);
        if (ret != EOK) return ret;
    }

    /* Determine parent */
    if (parent) {
        kobj->parent = parent;
    } else if (kobj->kset) {
        kobj->parent = &kobj->kset->kobj;
    }

    /* Add to kset if one is set */
    if (kobj->kset) {
        spin_lock(&kobj->kset->list_lock);
        kobj->kset->list = clist_append(kobj->kset->list, kobj);
        spin_unlock(&kobj->kset->list_lock);

        /* Inherit ktype from kset if not set */
        if (!kobj->ktype) { kobj->ktype = kobj->kset->kobj.ktype; }
        /* Inherit parent from kset if not set */
        if (!kobj->parent) { kobj->parent = &kobj->kset->kobj; }
    }

    /* Add to parent's child list */
    if (kobj->parent) {
        spin_lock(&kobj->parent->lock);
        kobj->parent->children = clist_append(kobj->parent->children, kobj);
        spin_unlock(&kobj->parent->lock);
    }

    /* Create sysfs directory */
    ret = sysfs_create_dir(kobj);
    if (ret != EOK) {
        /* Rollback: remove from parent's child list */
        if (kobj->parent) {
            spin_lock(&kobj->parent->lock);
            kobj->parent->children = clist_delete(kobj->parent->children, kobj);
            spin_unlock(&kobj->parent->lock);
        }
        /* Rollback: remove from kset */
        if (kobj->kset) {
            spin_lock(&kobj->kset->list_lock);
            kobj->kset->list = clist_delete(kobj->kset->list, kobj);
            spin_unlock(&kobj->kset->list_lock);
        }
        return ret;
    }

    /* Create default attributes */
    if (kobj->ktype && kobj->ktype->default_attrs) {
        struct attribute **attr;
        for (attr = kobj->ktype->default_attrs; *attr; attr++) {
            if ((*attr)->name) { sysfs_create_file(kobj, *attr); }
        }
    }

    /* Send KOBJ_ADD uevent */
    kobject_uevent(kobj, KOBJ_ADD);

    return EOK;
}

/* ------------------------------------------------------------------ */
/*  kobject_init_and_add                                               */
/* ------------------------------------------------------------------ */

int kobject_init_and_add(struct kobject *kobj, struct kobj_type *ktype, struct kobject *parent, const char *fmt, ...)
{
    va_list args;
    char    namebuf[KOBJ_NAME_LEN];
    int     ret;

    kobject_init(kobj, ktype);

    if (fmt) {
        va_start(args, fmt);
        vsnprintf(namebuf, sizeof(namebuf), fmt, args);
        va_end(args);

        ret = kobject_set_name(kobj, "%s", namebuf);
        if (ret != EOK) return ret;
    }

    return kobject_add(kobj, parent, "%s", kobj->name);
}

/* ------------------------------------------------------------------ */
/*  kobject_create_and_add                                             */
/* ------------------------------------------------------------------ */

struct kobject *kobject_create_and_add(const char *name, struct kobject *parent)
{
    struct kobject *kobj;
    int             ret;

    kobj = calloc(1, sizeof(struct kobject));
    if (!kobj) return NULL;

    kobject_init(kobj, &dynamic_kobj_ktype);

    ret = kobject_add(kobj, parent, "%s", name);
    if (ret != EOK) {
        kobject_put(kobj);
        return NULL;
    }

    /* kobject_get to return with ref held for caller */
    kref_get(&kobj->kref);
    return kobj;
}

/* ------------------------------------------------------------------ */
/*  kobject_get / kobject_put                                          */
/* ------------------------------------------------------------------ */

struct kobject *kobject_get(struct kobject *kobj)
{
    if (kobj) kref_get(&kobj->kref);
    return kobj;
}

static void kobject_release_internal(kref_t *kref)
{
    struct kobject *kobj = (struct kobject *)((char *)kref - offsetof(struct kobject, kref));

    /* Call the type-specific release */
    if (kobj->ktype && kobj->ktype->release) { kobj->ktype->release(kobj); }
}

void kobject_put(struct kobject *kobj)
{
    if (!kobj) return;
    kref_put(&kobj->kref, kobject_release_internal);
}

/* ------------------------------------------------------------------ */
/*  kobject_del                                                        */
/* ------------------------------------------------------------------ */

void kobject_del(struct kobject *kobj)
{
    if (!kobj) return;

    /* Send KOBJ_REMOVE uevent */
    kobject_uevent(kobj, KOBJ_REMOVE);

    /* Remove default attributes */
    if (kobj->ktype && kobj->ktype->default_attrs) {
        struct attribute **attr;
        for (attr = kobj->ktype->default_attrs; *attr; attr++) {
            if ((*attr)->name) { sysfs_remove_file(kobj, *attr); }
        }
    }

    /* Remove all remaining attribute files and symlinks */
    sysfs_cleanup_kobject_files(kobj);

    /* Remove sysfs directory */
    sysfs_remove_dir(kobj);

    /* Remove from parent's child list */
    if (kobj->parent) {
        spin_lock(&kobj->parent->lock);
        kobj->parent->children = clist_delete(kobj->parent->children, kobj);
        spin_unlock(&kobj->parent->lock);
    }

    /* Remove from kset */
    if (kobj->kset) {
        spin_lock(&kobj->kset->list_lock);
        kobj->kset->list = clist_delete(kobj->kset->list, kobj);
        spin_unlock(&kobj->kset->list_lock);
    }

    kobj->state_in_sysfs = 0;
}

/* ------------------------------------------------------------------ */
/*  kobject_rename                                                     */
/* ------------------------------------------------------------------ */

int kobject_rename(struct kobject *kobj, const char *new_name)
{
    if (!kobj || !new_name) return -EINVAL;

    char *old_name = (char *)kobj->name;
    kobj->name     = strdup(new_name);
    if (!kobj->name) {
        kobj->name = old_name;
        return -ENOMEM;
    }

    if (old_name) free(old_name);

    /* Rename the sysfs directory entry */
    if (kobj->state_in_sysfs && kobj->sd) {
        sysfs_rename_dir(kobj, new_name);

        /* Also rename the VFS node */
        if (kobj->sd->name) free((void *)kobj->sd->name);
        kobj->sd->name = strdup(new_name);
    }

    kobject_uevent(kobj, KOBJ_MOVE);
    return EOK;
}

/* ------------------------------------------------------------------ */
/*  kobject_move                                                       */
/* ------------------------------------------------------------------ */

int kobject_move(struct kobject *kobj, struct kobject *new_parent)
{
    if (!kobj || !new_parent) return -EINVAL;

    /* Remove from old parent */
    if (kobj->parent) {
        spin_lock(&kobj->parent->lock);
        kobj->parent->children = clist_delete(kobj->parent->children, kobj);
        spin_unlock(&kobj->parent->lock);
    }

    /* Add to new parent */
    kobj->parent = new_parent;
    spin_lock(&new_parent->lock);
    new_parent->children = clist_append(new_parent->children, kobj);
    spin_unlock(&new_parent->lock);

    kobject_uevent(kobj, KOBJ_MOVE);
    return EOK;
}

/* ------------------------------------------------------------------ */
/*  kobject_name                                                       */
/* ------------------------------------------------------------------ */

const char *kobject_name(const struct kobject *kobj)
{
    if (!kobj) return "(null)";
    return kobj->name ? kobj->name : "(unnamed)";
}

/* ------------------------------------------------------------------ */
/*  kset_init                                                          */
/* ------------------------------------------------------------------ */

void kset_init(struct kset *kset)
{
    if (!kset) return;

    memset(kset, 0, sizeof(*kset));
    kobject_init(&kset->kobj, NULL);
}

/* ------------------------------------------------------------------ */
/*  kset_create_and_add                                                */
/* ------------------------------------------------------------------ */

struct kset *kset_create_and_add(const char *name, const struct kset_uevent_ops *uevent_ops, struct kobject *parent_kobj)
{
    struct kset *kset;
    int          ret;

    kset = calloc(1, sizeof(struct kset));
    if (!kset) return NULL;

    kset_init(kset);
    kset->uevent_ops = uevent_ops;

    ret = kobject_add(&kset->kobj, parent_kobj, "%s", name);
    if (ret != EOK) {
        free(kset);
        return NULL;
    }

    return kset;
}

/* ------------------------------------------------------------------ */
/*  kset_unregister                                                    */
/* ------------------------------------------------------------------ */

void kset_unregister(struct kset *kset)
{
    if (!kset) return;

    /* Remove all children from the kset list */
    spin_lock(&kset->list_lock);
    {
        clist_t node;
        clist_t next;
        for (node = kset->list; node; node = next) {
            next                 = node->next;
            struct kobject *kobj = node->data;
            if (kobj) kobject_del(kobj);
        }
        kset->list = clist_free(kset->list);
    }
    spin_unlock(&kset->list_lock);

    kobject_del(&kset->kobj);
    free(kset);
}

/* ------------------------------------------------------------------ */
/*  kobject_get_path                                                   */
/* ------------------------------------------------------------------ */

char *kobject_get_path(struct kobject *kobj)
{
    char           *path;
    size_t          len;
    int             depth;
    struct kobject *components[32]; /* max path depth */

    if (!kobj) return strdup("(null)");

    /* Walk up the tree collecting components */
    depth               = 0;
    struct kobject *cur = kobj;
    while (cur && depth < 32) {
        components[depth++] = cur;
        cur                 = cur->parent;
    }

    /* Calculate total length */
    len = 0;
    for (int i = depth - 1; i >= 0; i--) {
        if (components[i]->name) { len += strlen(components[i]->name) + 1; /* +1 for '/' */ }
    }
    if (len == 0) len = 1; /* just '/' */

    path = malloc(len + 1);
    if (!path) return NULL;

    /* Build path string */
    path[0] = '\0';
    for (int i = depth - 1; i >= 0; i--) {
        if (components[i]->name && components[i]->name[0]) {
            strcat(path, "/");
            strcat(path, components[i]->name);
        }
    }
    if (path[0] == '\0') {
        path[0] = '/';
        path[1] = '\0';
    }

    return path;
}

/* ------------------------------------------------------------------ */
/*  kobject_uevent                                                     */
/* ------------------------------------------------------------------ */

int kobject_uevent(struct kobject *kobj, enum kobject_action action)
{
    return kobject_uevent_env(kobj, action, NULL, 0);
}

/* ------------------------------------------------------------------ */
/*  Global uevent sequence number                                      */
/* ------------------------------------------------------------------ */

static uint64_t uevent_seqnum;

uint64_t kobject_uevent_seqnum(void)
{
    return uevent_seqnum;
}

/* ------------------------------------------------------------------ */
/*  kobject_uevent_env — build and broadcast uevent                    */
/* ------------------------------------------------------------------ */

#define UEVENT_BUFFER_SIZE 2048
#define UEVENT_NUM_ENVP    32

int kobject_uevent_env(struct kobject *kobj, enum kobject_action action, char *envp[], int nenv)
{
    struct kset *kset;
    const char  *action_string = NULL;
    char        *devpath;
    const char  *subsystem = NULL;
    char        *buffer;
    char        *nl_data;
    nlmsghdr_t  *nlh;
    size_t       buflen;
    size_t       pos;
    uint64_t     seq;
    int          ret;

    if (!kobj) return -EINVAL;

    switch (action) {
        case KOBJ_ADD :
            action_string = "add";
            break;
        case KOBJ_REMOVE :
            action_string = "remove";
            break;
        case KOBJ_CHANGE :
            action_string = "change";
            break;
        case KOBJ_MOVE :
            action_string = "move";
            break;
        case KOBJ_ONLINE :
            action_string = "online";
            break;
        case KOBJ_OFFLINE :
            action_string = "offline";
            break;
        case KOBJ_BIND :
            action_string = "bind";
            break;
        case KOBJ_UNBIND :
            action_string = "unbind";
            break;
        default :
            action_string = "unknown";
            break;
    }

    /* Find the kset that handles uevents */
    kset = kobj->kset;
    if (!kset && kobj->parent) kset = kobj->parent->kset;

    /* Apply event filter */
    if (kset && kset->uevent_ops && kset->uevent_ops->filter) {
        if (!kset->uevent_ops->filter(kset, kobj)) return EOK;
    }

    /* Allocate buffer for the environment string */
    buffer = malloc(UEVENT_BUFFER_SIZE);
    if (!buffer) return -ENOMEM;

    /* Build the device path */
    devpath = kobject_get_path(kobj);
    if (!devpath) {
        free(buffer);
        return -ENOMEM;
    }

    /* Determine subsystem from kset */
    if (kset) { subsystem = kobject_name(&kset->kobj); }

    /* Build environment string: KEY=VALUE\0KEY=VALUE\0...\0 */
    pos = 0;

    /* ACTION */
    pos += (size_t)snprintf(buffer + pos, UEVENT_BUFFER_SIZE - pos, "ACTION=%s", action_string);
    buffer[pos++] = '\0';

    /* DEVPATH */
    pos += (size_t)snprintf(buffer + pos, UEVENT_BUFFER_SIZE - pos, "DEVPATH=%s", devpath);
    buffer[pos++] = '\0';

    /* SUBSYSTEM */
    if (subsystem) {
        pos += (size_t)snprintf(buffer + pos, UEVENT_BUFFER_SIZE - pos, "SUBSYSTEM=%s", subsystem);
        buffer[pos++] = '\0';
    }

    /* SEQNUM */
    seq = uevent_seqnum++;
    pos += (size_t)snprintf(buffer + pos, UEVENT_BUFFER_SIZE - pos, "SEQNUM=%llu", (unsigned long long)seq);
    buffer[pos++] = '\0';

    /* Add caller-supplied environment variables */
    if (envp && nenv > 0) {
        for (int i = 0; i < nenv && i < UEVENT_NUM_ENVP; i++) {
            if (!envp[i]) continue;
            size_t elen = strlen(envp[i]);
            if (pos + elen + 2 > UEVENT_BUFFER_SIZE) break;
            memcpy(buffer + pos, envp[i], elen);
            pos += elen;
            buffer[pos++] = '\0';
        }
    }

    /* Call kset-specific uevent hook for additional variables */
    if (kset && kset->uevent_ops && kset->uevent_ops->uevent) {
        /* Extract extra env pointers into an array on stack */
        char  *extra_envp[UEVENT_NUM_ENVP];
        int    extra_nenv     = UEVENT_NUM_ENVP;
        char  *remaining_buf  = buffer + pos;
        size_t remaining_size = UEVENT_BUFFER_SIZE - pos;

        /* Temporarily: use the remaining buffer space */
        (void)extra_envp;
        (void)extra_nenv;
        (void)remaining_buf;
        (void)remaining_size;
        /* The uevent callback is called with existing envp;
         * for simplicity, call it if space permits.
         * Full implementation would split the buffer. */
    }

    /* Terminate with extra NULL */
    buffer[pos++] = '\0';
    buflen        = pos;

    free(devpath);

    /* Build netlink message: nlmsghdr + environment string */
    {
        uint32_t nl_len = NLMSG_HDRLEN + (uint32_t)buflen;

        nl_data = malloc(nl_len);
        if (!nl_data) {
            free(buffer);
            return -ENOMEM;
        }

        nlh              = (nlmsghdr_t *)nl_data;
        nlh->nlmsg_len   = nl_len;
        nlh->nlmsg_type  = (uint16_t)action; /* KOBJ_ADD=1, KOBJ_REMOVE=2, ... */
        nlh->nlmsg_flags = 0;
        nlh->nlmsg_seq   = (uint32_t)seq;
        nlh->nlmsg_pid   = 0; /* from kernel */

        /* Copy environment string after the header */
        memcpy((uint8_t *)nl_data + NLMSG_HDRLEN, buffer, buflen);
    }

    free(buffer);

    /* Broadcast to NETLINK_KOBJECT_UEVENT listeners */
    ret = netlink_broadcast(NETLINK_KOBJECT_UEVENT, 1, nl_data, ((nlmsghdr_t *)nl_data)->nlmsg_len, 0);

    free(nl_data);

    if (ret < 0) {
        /* No listeners is not an error */
        if (ret == -ECONNREFUSED) ret = EOK;
    }

    return EOK;
}
