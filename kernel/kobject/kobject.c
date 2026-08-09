/*
 *
 *      kobject.c
 *      Kernel object model implementation
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/core/vfs.h>
#include <fs/sysfs/sysfs.h>
#include <ipc/netlink.h>
#include <kernel/errno.h>
#include <kernel/kobject/kobject.h>
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

static const char *kset_uevent_name(struct kobject *kobj)
{
    return kobj && kobj->name ? kobj->name : "kset";
}

static struct kobj_type dynamic_kset_ktype = {
    .release     = dynamic_kset_release,
    .uevent_name = kset_uevent_name,
};

static struct kobj_type static_kset_ktype = {
    .release     = static_kset_release,
    .uevent_name = kset_uevent_name,
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

    /*
     * NOTE: does NOT zero the struct. The caller is responsible
     * for providing a pre-zeroed kobject (e.g. via calloc).
     * Fields that were set before init (like name) are preserved.
     */
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
    kobj->uevent_suppress          = 0;
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
    if (parent && (held_parent = kobject_get(parent)) == NULL) return -EINVAL; // NOLINT(bugprone-assignment-in-if-condition)
    if (kobj->kset && (held_kset = kset_get(kobj->kset)) == NULL) {            // NOLINT(bugprone-assignment-in-if-condition)
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
    plogk("kobject: add of \"%s\" (parent \"%s\") failed: %d\n", kobj->name ? kobj->name : "(unnamed)",
          parent && parent->name ? parent->name : "(none)", ret);
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
    char *old_path;
    int   ret;

    if (!kobj || !kobject_name_valid(new_name)) return -EINVAL;
    if (kobj->name && streq(kobj->name, new_name)) return EOK;

    char *replacement = strdup(new_name);
    if (!replacement) return -ENOMEM;

    old_path = kobject_get_path(kobj);
    if (!old_path) {
        free(replacement);
        return -ENOMEM;
    }

    if (kobj->state_in_sysfs) {
        ret = sysfs_rename_dir(kobj, new_name);
        if (ret != EOK) {
            free(old_path);
            free(replacement);
            return ret;
        }
    }

    free((void *)kobj->name);
    kobj->name                 = replacement;
    const char *event_old_path = old_path;
    if (strncmp(event_old_path, "/sys/", 5) == 0) event_old_path += 4;
    char old_path_env[UEVENT_BUFFER_SIZE];
    int  length = snprintf(old_path_env, sizeof(old_path_env), "DEVPATH_OLD=%s", event_old_path);
    if (length > 0 && length < (int)sizeof(old_path_env)) {
        char *envp[] = {old_path_env};
        (void)kobject_uevent_env(kobj, KOBJ_MOVE, envp, 1);
    }
    free(old_path);
    return EOK;
}

/* ------------------------------------------------------------------ */
/*  kobject_move                                                       */
/* ------------------------------------------------------------------ */

int kobject_move(struct kobject *kobj, struct kobject *new_parent)
{
    struct kobject *old_parent;
    char           *old_path;
    clist_t         new_link = NULL;
    int             ret;

    if (!kobj || !new_parent || !kobj->state_in_sysfs) return -EINVAL;
    if (kobj->parent == new_parent) return EOK;
    for (struct kobject *ancestor = new_parent; ancestor; ancestor = ancestor->parent)
        if (ancestor == kobj) return -EINVAL;

    old_path = kobject_get_path(kobj);
    if (!old_path) return -ENOMEM;
    if (!kobject_get(new_parent)) {
        free(old_path);
        return -EINVAL;
    }
    new_link = clist_alloc(kobj);
    if (!new_link) {
        free(old_path);
        kobject_put(new_parent);
        return -ENOMEM;
    }

    ret = sysfs_move_dir(kobj, new_parent);
    if (ret != EOK) {
        free(old_path);
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
    const char *event_old_path = old_path;
    if (strncmp(event_old_path, "/sys/", 5) == 0) event_old_path += 4;
    char old_path_env[UEVENT_BUFFER_SIZE];
    int  length = snprintf(old_path_env, sizeof(old_path_env), "DEVPATH_OLD=%s", event_old_path);
    if (length > 0 && length < (int)sizeof(old_path_env)) {
        char *envp[] = {old_path_env};
        (void)kobject_uevent_env(kobj, KOBJ_MOVE, envp, 1);
    }
    free(old_path);
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
    struct kobject *components[32]; // max path depth

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
    if (len == 0) len = 1; // just '/'

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
    if (!kobj) return -EINVAL;
    return kobject_uevent_env(kobj, action, NULL, 0);
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
/*  kobject_uevent_env - build and broadcast the Linux uevent ABI      */
/* ------------------------------------------------------------------ */

static const char *const kobject_actions[] = {
    [KOBJ_ADD] = "add",       [KOBJ_REMOVE] = "remove",   [KOBJ_CHANGE] = "change", [KOBJ_MOVE] = "move",
    [KOBJ_ONLINE] = "online", [KOBJ_OFFLINE] = "offline", [KOBJ_BIND] = "bind",     [KOBJ_UNBIND] = "unbind",
};

const char *kobject_action_name(enum kobject_action action)
{
    if ((unsigned int)action >= sizeof(kobject_actions) / sizeof(kobject_actions[0])) return NULL;
    return kobject_actions[action];
}

int kobject_action_type(const char *name, enum kobject_action *action)
{
    if (!name || !action) return -EINVAL;
    for (unsigned int i = 0; i < sizeof(kobject_actions) / sizeof(kobject_actions[0]); i++) {
        if (kobject_actions[i] && streq(name, kobject_actions[i])) {
            *action = (enum kobject_action)i;
            return EOK;
        }
    }
    return -EINVAL;
}

int add_uevent_var(struct kobj_uevent_env *env, const char *fmt, ...)
{
    va_list args;
    int     length;

    if (!env || !fmt || env->envp_idx < 0 || env->buflen < 0) return -EINVAL;
    if (env->envp_idx >= UEVENT_NUM_ENVP - 1) return -ENOMEM;
    if (env->buflen >= UEVENT_BUFFER_SIZE) return -ENOMEM;

    env->envp[env->envp_idx] = env->envbuf + env->buflen;
    va_start(args, fmt);
    length = vsnprintf(env->envbuf + env->buflen, UEVENT_BUFFER_SIZE - (size_t)env->buflen, fmt, args);
    va_end(args);
    if (length < 0) return -EINVAL;
    if (length >= UEVENT_BUFFER_SIZE - env->buflen) return -ENOMEM;

    env->buflen += length + 1;
    env->envp_idx++;
    env->envp[env->envp_idx] = NULL;
    return EOK;
}

static struct kset *kobject_uevent_kset(struct kobject *kobj)
{
    for (struct kobject *cursor = kobj; cursor; cursor = cursor->parent)
        if (cursor->kset) return cursor->kset;
    return NULL;
}

static void zap_modalias_env(struct kobj_uevent_env *env)
{
    for (int i = 0; i < env->envp_idx; i++) {
        if (strncmp(env->envp[i], "MODALIAS=", 9) != 0) continue;
        for (int j = i; j + 1 < env->envp_idx; j++) env->envp[j] = env->envp[j + 1];
        env->envp_idx--;
        env->envp[env->envp_idx] = NULL;
        return;
    }
}

static int kobject_uevent_message(struct kobj_uevent_env *env, const char *action, const char *devpath, uint8_t **message, uint32_t *message_len)
{
    size_t header_len;
    size_t total;
    size_t offset;

    header_len = strlen(action) + 1 + strlen(devpath) + 1;
    total      = header_len + (size_t)env->buflen;
    if (total > UINT32_MAX) return -EOVERFLOW;

    *message = malloc(total);
    if (!*message) return -ENOMEM;

    int written = snprintf((char *)*message, header_len, "%s@%s", action, devpath);
    if (written < 0 || (size_t)written + 1 != header_len) {
        free(*message);
        *message = NULL;
        return -EINVAL;
    }

    offset = header_len;
    for (int i = 0; i < env->envp_idx; i++) {
        size_t length = strlen(env->envp[i]) + 1;
        memcpy(*message + offset, env->envp[i], length);
        offset += length;
    }
    *message_len = (uint32_t)offset;
    return EOK;
}

int kobject_uevent_env(struct kobject *kobj, enum kobject_action action, char *envp[], int nenv)
{
    struct kobj_uevent_env *env;
    struct kset            *kset;
    const char             *action_string;
    const char             *subsystem = NULL;
    const char             *event_path;
    char                   *devpath;
    uint8_t                *message = NULL;
    uint32_t                message_len;
    uint64_t                seq;
    int                     ret;

    if (!kobj) return -EINVAL;
    if (nenv < 0 || nenv >= UEVENT_NUM_ENVP) return -EINVAL;
    action_string = kobject_action_name(action);
    if (!action_string) return -EINVAL;
    if (action == KOBJ_REMOVE) kobj->state_remove_uevent_sent = 1;
    if (kobj->uevent_suppress) return EOK;

    kset = kobject_uevent_kset(kobj);

    /* Apply event filter */
    if (kset && kset->uevent_ops && kset->uevent_ops->filter)
        if (!kset->uevent_ops->filter(kobj)) return EOK;

    env = calloc(1, sizeof(*env));
    if (!env) return -ENOMEM;

    devpath = kobject_get_path(kobj);
    if (!devpath) {
        free(env);
        return -ENOMEM;
    }

    if (kset && kset->uevent_ops && kset->uevent_ops->name) subsystem = kset->uevent_ops->name(kobj);
    if (!subsystem && kset) subsystem = kobject_name(&kset->kobj);
    if (!subsystem && kobj->ktype && kobj->ktype->uevent_name) subsystem = kobj->ktype->uevent_name(kobj);
    if (!subsystem || !subsystem[0]) {
        ret = -EINVAL;
        goto out;
    }

    event_path = devpath;
    if (strncmp(event_path, "/sys/", 5) == 0) event_path += 4;
    if (streq(event_path, "/sys")) event_path = "/";

    ret = add_uevent_var(env, "ACTION=%s", action_string);
    if (ret) goto out;
    ret = add_uevent_var(env, "DEVPATH=%s", event_path);
    if (ret) goto out;
    ret = add_uevent_var(env, "SUBSYSTEM=%s", subsystem);
    if (ret) goto out;

    if (envp && nenv > 0) {
        for (int i = 0; i < nenv; i++) {
            if (!envp[i]) continue;
            if (!strchr(envp[i], '=') || envp[i][0] == '=') {
                ret = -EINVAL;
                goto out;
            }
            ret = add_uevent_var(env, "%s", envp[i]);
            if (ret) goto out;
        }
    }

    if (kset && kset->uevent_ops && kset->uevent_ops->uevent) {
        ret = kset->uevent_ops->uevent(kobj, env);
        if (ret) goto out;
    }
    if (kobj->ktype && kobj->ktype->uevent) {
        ret = kobj->ktype->uevent(kobj, env);
        if (ret) goto out;
    }

    if (action == KOBJ_UNBIND) zap_modalias_env(env);

    if (action == KOBJ_ADD) kobj->state_add_uevent_sent = 1;

    seq = __atomic_add_fetch(&uevent_seqnum, 1, __ATOMIC_RELAXED);
    ret = add_uevent_var(env, "SEQNUM=%llu", (unsigned long long)seq);
    if (ret) goto out;

    ret = kobject_uevent_message(env, action_string, event_path, &message, &message_len);
    if (ret) goto out;
    ret = netlink_broadcast(NETLINK_KOBJECT_UEVENT, 1U, message, message_len, 0);
    if (ret >= 0 || ret == -ESRCH || ret == -ECONNREFUSED || ret == -ENOBUFS) ret = EOK;

out:
    free(message);
    free(devpath);
    free(env);
    return ret;
}

static int synth_uuid_valid(const char *uuid)
{
    if (!uuid || strlen(uuid) != 36) return 0;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (uuid[i] != '-') return 0;
        } else if (!((uuid[i] >= '0' && uuid[i] <= '9') || (uuid[i] >= 'a' && uuid[i] <= 'f') || (uuid[i] >= 'A' && uuid[i] <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

int kobject_synth_uevent(struct kobject *kobj, const char *buf, size_t count)
{
    enum kobject_action action;
    char                command[128];
    char               *argument = NULL;
    size_t              length;

    if (!kobj || !buf || count == 0 || count >= sizeof(command)) return -EINVAL;
    memcpy(command, buf, count);
    command[count] = '\0';

    length = count;
    while (length && (command[length - 1] == '\n' || command[length - 1] == '\r' || command[length - 1] == ' ' || command[length - 1] == '\t'))
        command[--length] = '\0';
    if (!length) return -EINVAL;

    for (size_t i = 0; i < length; i++) {
        if (command[i] != ' ' && command[i] != '\t') continue;
        command[i] = '\0';
        argument   = command + i + 1;
        while (*argument == ' ' || *argument == '\t') argument++;
        if (!*argument || strchr(argument, ' ') || strchr(argument, '\t')) return -EINVAL;
        break;
    }

    int ret = kobject_action_type(command, &action);
    if (ret) return ret;
    if (!argument) return kobject_uevent(kobj, action);
    if (!synth_uuid_valid(argument)) return -EINVAL;

    char synth_uuid[48];
    int  written = snprintf(synth_uuid, sizeof(synth_uuid), "SYNTH_UUID=%s", argument);
    if (written < 0 || written >= (int)sizeof(synth_uuid)) return -EINVAL;
    char *envp[] = {synth_uuid};
    return kobject_uevent_env(kobj, action, envp, 1);
}
