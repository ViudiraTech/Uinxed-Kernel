/*
 *
 *      kobject.c
 *      Kernel object model implementation
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright Â© 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/sysfs/sysfs.h>
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
    free(kobj);
}

static struct kobj_type dynamic_kobj_ktype = {
    .release       = dynamic_kobj_release,
    .sysfs_ops     = NULL,
    .default_attrs = NULL,
};

static void dynamic_kset_release(struct kobject *kobj)
{
    struct kset *kset = (struct kset *)((char *)kobj - offsetof(struct kset, kobj));
    kset->list        = clist_free(kset->list);
    free(kset);
}

static void static_kset_release(struct kobject *kobj)
{
    (void)kobj;
}

static struct kobj_type dynamic_kset_ktype = {
    .release = dynamic_kset_release,
};

static struct kobj_type static_kset_ktype = {
    .release = static_kset_release,
};

static int kobject_name_valid(const char *name)
{
    return name && name[0] && !strchr(name, '/');
}

static int kobject_list_add(clist_t *list, void *data)
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

/* ------------------------------------------------------------------ */
/*  kobject_init                                                       */
/* ------------------------------------------------------------------ */

void kobject_init(struct kobject *kobj, struct kobj_type *ktype)
{
    if (!kobj) return;

    /* NOTE: does NOT zero the struct. The caller is responsible
     * for providing a pre-zeroed kobject (e.g. via calloc).
     * Fields that were set before init (like name) are preserved. */
    kobj->ktype          = ktype;
    kobj->kset           = NULL;
    kobj->parent         = NULL;
    kobj->sd             = NULL;
    kobj->children       = NULL;
    kobj->attributes     = NULL;
    kobj->bin_attributes = NULL;
    kobj->symlinks       = NULL;
    memset(&kobj->lock, 0, sizeof(kobj->lock));
    kref_init(&kobj->kref);
    kobj->state_initialized        = 1;
    kobj->state_in_sysfs           = 0;
    kobj->state_in_kset            = 0;
    kobj->state_add_uevent_sent    = 0;
    kobj->state_remove_uevent_sent = 0;
}

/* ------------------------------------------------------------------ */
/*  kobject_set_name                                                   */
/* ------------------------------------------------------------------ */

int kobject_set_name(struct kobject *kobj, const char *fmt, ...)
{
    char    buf[KOBJ_NAME_LEN];
    va_list args;
    int     n;

    if (!kobj || !fmt) return -EINVAL;

    va_start(args, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (n < 0 || n >= (int)sizeof(buf) || !kobject_name_valid(buf)) return n >= (int)sizeof(buf) ? -ENAMETOOLONG : -EINVAL;

    char *name = strdup(buf);
    if (!name) return -ENOMEM;

    free((void *)kobj->name);
    kobj->name = name;

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

    struct kobject *held_parent = NULL;
    struct kset    *held_kset   = NULL;

    if (!kobj || !kobj->state_initialized || kobj->state_in_sysfs) return -EINVAL;

    /* Set the name */
    if (fmt) {
        va_start(args, fmt);
        int n = vsnprintf(namebuf, sizeof(namebuf), fmt, args);
        va_end(args);

        if (n < 0 || n >= (int)sizeof(namebuf)) return n >= (int)sizeof(namebuf) ? -ENAMETOOLONG : -EINVAL;

        ret = kobject_set_name(kobj, "%s", namebuf);
        if (ret != EOK) return ret;
    }
    if (!kobject_name_valid(kobj->name)) return -EINVAL;

    /* Determine parent */
    if (!parent && kobj->kset) parent = &kobj->kset->kobj;
    if (parent && !(held_parent = kobject_get(parent))) return -EINVAL;
    if (kobj->kset && !(held_kset = kset_get(kobj->kset))) {
        kobject_put(held_parent);
        return -EINVAL;
    }
    kobj->parent = parent;

    /* Add to kset if one is set */
    if (kobj->kset) {
        spin_lock(&kobj->kset->list_lock);
        ret = kobject_list_add(&kobj->kset->list, kobj);
        spin_unlock(&kobj->kset->list_lock);

        if (ret != EOK) goto err_refs;
        kobj->state_in_kset = 1;
    }

    /* Add to parent's child list */
    if (kobj->parent) {
        spin_lock(&kobj->parent->lock);
        ret = kobject_list_add(&kobj->parent->children, kobj);
        spin_unlock(&kobj->parent->lock);
        if (ret != EOK) goto err_kset;
    }

    /* Create sysfs directory */
    ret = sysfs_create_dir(kobj);
    if (ret != EOK) { goto err_parent; }

    /* Create default attributes */
    if (kobj->ktype && kobj->ktype->default_attrs) {
        struct attribute **attr;
        for (attr = kobj->ktype->default_attrs; *attr; attr++) {
            if (!(*attr)->name) continue;
            ret = sysfs_create_file(kobj, *attr);
            if (ret != EOK) {
                for (struct attribute **created = kobj->ktype->default_attrs; created < attr; created++) sysfs_remove_file(kobj, *created);
                sysfs_remove_dir(kobj);
                goto err_parent;
            }
        }
    }

    return EOK;

err_parent:
    if (kobj->parent) {
        spin_lock(&kobj->parent->lock);
        kobj->parent->children = clist_delete(kobj->parent->children, kobj);
        spin_unlock(&kobj->parent->lock);
    }
err_kset:
    if (kobj->state_in_kset) {
        spin_lock(&kobj->kset->list_lock);
        kobj->kset->list = clist_delete(kobj->kset->list, kobj);
        spin_unlock(&kobj->kset->list_lock);
        kobj->state_in_kset = 0;
    }
err_refs:
    kobj->parent = NULL;
    kset_put(held_kset);
    kobject_put(held_parent);
    return ret;
}

/* ------------------------------------------------------------------ */
/*  kobject_init_and_add                                               */
/* ------------------------------------------------------------------ */

int kobject_init_and_add(struct kobject *kobj, struct kobj_type *ktype, struct kobject *parent, const char *fmt, ...)
{
    va_list args;
    char    namebuf[KOBJ_NAME_LEN];
    int     n;

    if (!kobj || !fmt) return -EINVAL;
    va_start(args, fmt);
    n = vsnprintf(namebuf, sizeof(namebuf), fmt, args);
    va_end(args);
    if (n < 0 || n >= (int)sizeof(namebuf)) return n >= (int)sizeof(namebuf) ? -ENAMETOOLONG : -EINVAL;

    kobject_init(kobj, ktype);
    return kobject_add(kobj, parent, "%s", namebuf);
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

    return kobj;
}

/* ------------------------------------------------------------------ */
/*  kobject_get / kobject_put                                          */
/* ------------------------------------------------------------------ */

struct kobject *kobject_get(struct kobject *kobj)
{
    if (!kobj || !kref_get_unless_zero(&kobj->kref)) return NULL;
    return kobj;
}

static void kobject_release_internal(kref_t *kref)
{
    struct kobject *kobj = (struct kobject *)((char *)kref - offsetof(struct kobject, kref));

    if (kobj->state_in_sysfs) kobject_del(kobj);
    free((void *)kobj->name);
    kobj->name              = NULL;
    kobj->state_initialized = 0;

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
    struct kobject *parent;
    struct kset    *kset;

    if (!kobj || !kobj->state_in_sysfs) return;

    if (kobj->state_add_uevent_sent && !kobj->state_remove_uevent_sent) kobject_uevent(kobj, KOBJ_REMOVE);

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
    parent = kobj->parent;
    kset   = kobj->kset;
    if (parent) {
        spin_lock(&parent->lock);
        parent->children = clist_delete(parent->children, kobj);
        spin_unlock(&parent->lock);
    }

    /* Remove from kset */
    if (kset && kobj->state_in_kset) {
        spin_lock(&kset->list_lock);
        kset->list = clist_delete(kset->list, kobj);
        spin_unlock(&kset->list_lock);
        kobj->state_in_kset = 0;
    }

    kobj->state_in_sysfs = 0;
    kobj->parent         = NULL;
    kobj->kset           = NULL;
    kobject_put(parent);
    kset_put(kset);
}

/* ------------------------------------------------------------------ */
/*  kobject_rename                                                     */
/* ------------------------------------------------------------------ */

int kobject_rename(struct kobject *kobj, const char *new_name)
{
    int ret;

    if (!kobj || !kobject_name_valid(new_name)) return -EINVAL;
    if (kobj->name && streq(kobj->name, new_name)) return EOK;

    char *replacement = strdup(new_name);
    if (!replacement) return -ENOMEM;

    if (kobj->state_in_sysfs) {
        ret = sysfs_rename_dir(kobj, new_name);
        if (ret != EOK) {
            free(replacement);
            return ret;
        }
    }

    free((void *)kobj->name);
    kobj->name = replacement;
    kobject_uevent(kobj, KOBJ_MOVE);
    return EOK;
}

/* ------------------------------------------------------------------ */
/*  kobject_move                                                       */
/* ------------------------------------------------------------------ */

int kobject_move(struct kobject *kobj, struct kobject *new_parent)
{
    struct kobject *old_parent;
    clist_t         new_link = NULL;
    int             ret;

    if (!kobj || !new_parent || !kobj->state_in_sysfs) return -EINVAL;
    if (kobj->parent == new_parent) return EOK;
    for (struct kobject *ancestor = new_parent; ancestor; ancestor = ancestor->parent)
        if (ancestor == kobj) return -EINVAL;

    if (!kobject_get(new_parent)) return -EINVAL;
    new_link = clist_alloc(kobj);
    if (!new_link) {
        kobject_put(new_parent);
        return -ENOMEM;
    }

    ret = sysfs_move_dir(kobj, new_parent);
    if (ret != EOK) {
        free(new_link);
        kobject_put(new_parent);
        return ret;
    }

    /* Remove from old parent */
    old_parent = kobj->parent;
    if (old_parent) {
        spin_lock(&old_parent->lock);
        old_parent->children = clist_delete(old_parent->children, kobj);
        spin_unlock(&old_parent->lock);
    }

    /* Add to new parent */
    kobj->parent = new_parent;
    spin_lock(&new_parent->lock);
    if (!new_parent->children) {
        new_parent->children = new_link;
    } else {
        clist_t tail   = clist_tail(new_parent->children);
        tail->next     = new_link;
        new_link->prev = tail;
    }
    spin_unlock(&new_parent->lock);

    kobject_put(old_parent);
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
    kobject_init(&kset->kobj, &static_kset_ktype);
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
    kset->dynamic    = 1;
    kset->kobj.ktype = &dynamic_kset_ktype;
    kset->uevent_ops = uevent_ops;

    ret = kobject_add(&kset->kobj, parent_kobj, "%s", name);
    if (ret != EOK) {
        kobject_put(&kset->kobj);
        return NULL;
    }

    kobject_uevent(&kset->kobj, KOBJ_ADD);

    return kset;
}

/* ------------------------------------------------------------------ */
/*  kset_unregister                                                    */
/* ------------------------------------------------------------------ */

void kset_unregister(struct kset *kset)
{
    if (!kset) return;
    kobject_del(&kset->kobj);
    kobject_put(&kset->kobj);
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
    int ret;

    if (!kobj) return -EINVAL;
    ret = kobject_uevent_env(kobj, action, NULL, 0);
    if (ret == EOK && action == KOBJ_ADD) kobj->state_add_uevent_sent = 1;
    if (ret == EOK && action == KOBJ_REMOVE) kobj->state_remove_uevent_sent = 1;
    return ret;
}

/* ------------------------------------------------------------------ */
/*  Global uevent sequence number                                      */
/* ------------------------------------------------------------------ */

static uint64_t uevent_seqnum;

uint64_t kobject_uevent_seqnum(void)
{
    return __atomic_load_n(&uevent_seqnum, __ATOMIC_RELAXED);
}

/* ------------------------------------------------------------------ */
/*  kobject_uevent_env â€?build and broadcast uevent                    */
/* ------------------------------------------------------------------ */

#define UEVENT_BUFFER_SIZE 2048
#define UEVENT_NUM_ENVP    32

#if CONFIG_UEVENT_HELPER
static int uevent_append(char *buffer, size_t capacity, size_t *position, char **entry, const char *fmt, ...)
{
    va_list args;

    if (!buffer || !position || *position >= capacity) return -ENOSPC;
    if (entry) *entry = buffer + *position;

    va_start(args, fmt);
    int length = vsnprintf(buffer + *position, capacity - *position, fmt, args);
    va_end(args);
    if (length < 0) return -EINVAL;
    if ((size_t)length >= capacity - *position) return -ENOSPC;

    *position += (size_t)length + 1;
    return EOK;
}
#endif

int kobject_uevent_env(struct kobject *kobj, enum kobject_action action, char *envp[], int nenv)
{
#if CONFIG_UEVENT_HELPER
    struct kset *kset;
    const char  *action_string = NULL;
    const char  *subsystem     = NULL;
    const char  *event_path;
    char        *event_envp[UEVENT_NUM_ENVP + 1];
    char        *devpath;
    char        *buffer;
    char        *nl_data;
    nlmsghdr_t  *nlh;
    size_t       buflen;
    size_t       pos;
    uint64_t     seq;
    int          event_nenv;
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
            return -EINVAL;
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

    if (kset && kset->uevent_ops && kset->uevent_ops->name) subsystem = kset->uevent_ops->name(kset, kobj);
    if (!subsystem && kset) subsystem = kobject_name(&kset->kobj);

    event_path = devpath;
    if (strncmp(event_path, "/sys/", 5) == 0) event_path += 4;
    if (streq(event_path, "/sys")) event_path = "/";

    /* Linux kobject uevents start with "action@devpath", followed by a
     * NUL-separated environment. */
    pos        = 0;
    event_nenv = 0;
    ret        = uevent_append(buffer, UEVENT_BUFFER_SIZE, &pos, NULL, "%s@%s", action_string, event_path);
    if (ret != EOK) goto err_buffer;

    ret = uevent_append(buffer, UEVENT_BUFFER_SIZE, &pos, &event_envp[event_nenv], "ACTION=%s", action_string);
    if (ret != EOK) goto err_buffer;
    event_nenv++;

    ret = uevent_append(buffer, UEVENT_BUFFER_SIZE, &pos, &event_envp[event_nenv], "DEVPATH=%s", event_path);
    if (ret != EOK) goto err_buffer;
    event_nenv++;

    if (subsystem) {
        ret = uevent_append(buffer, UEVENT_BUFFER_SIZE, &pos, &event_envp[event_nenv], "SUBSYSTEM=%s", subsystem);
        if (ret != EOK) goto err_buffer;
        event_nenv++;
    }

    seq = __atomic_add_fetch(&uevent_seqnum, 1, __ATOMIC_RELAXED);
    ret = uevent_append(buffer, UEVENT_BUFFER_SIZE, &pos, &event_envp[event_nenv], "SEQNUM=%llu", (unsigned long long)seq);
    if (ret != EOK) goto err_buffer;
    event_nenv++;

    if (envp && nenv > 0) {
        for (int i = 0; i < nenv; i++) {
            if (!envp[i]) continue;
            if (event_nenv >= UEVENT_NUM_ENVP) {
                ret = -E2BIG;
                goto err_buffer;
            }
            ret = uevent_append(buffer, UEVENT_BUFFER_SIZE, &pos, &event_envp[event_nenv], "%s", envp[i]);
            if (ret != EOK) goto err_buffer;
            event_nenv++;
        }
    }

    event_envp[event_nenv] = NULL;
    if (kset && kset->uevent_ops && kset->uevent_ops->uevent) {
        ret = kset->uevent_ops->uevent(kset, kobj, event_envp, event_nenv);
        if (ret != EOK) goto err_buffer;
    }

    if (pos >= UEVENT_BUFFER_SIZE) {
        ret = -ENOSPC;
        goto err_buffer;
    }
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

    if (ret == -ECONNREFUSED) ret = EOK;
    return ret;

err_buffer:
    free(devpath);
    free(buffer);
    return ret;
#else
    (void)kobj;
    (void)action;
    (void)envp;
    (void)nenv;
    return EOK;
#endif
}
