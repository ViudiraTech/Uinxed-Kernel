/*
 *
 *      kobject.h
 *      Kernel object model header file
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_KOBJECT_H_
#define INCLUDE_KOBJECT_H_

#include <fs/sysfs.h>
#include <fs/vfs.h>
#include <libs/glist/circular_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/*  kref — reference-counting primitive                                */
/* ------------------------------------------------------------------ */

typedef struct kref {
    uint32_t refcount;
} kref_t;

/* Increment the reference count */
static inline void kref_init(kref_t *kref)
{
    kref->refcount = 1;
}

/* Take an additional reference */
static inline void kref_get(kref_t *kref)
{
    kref->refcount++;
}

/* Drop a reference; returns 1 if the count reached zero */
static inline int kref_put(kref_t *kref, void (*release)(kref_t *kref))
{
    if (--kref->refcount == 0) {
        if (release) release(kref);
        return 1;
    }
    return 0;
}

/* Return the current reference count */
static inline uint32_t kref_read(const kref_t *kref)
{
    return kref->refcount;
}

/* ------------------------------------------------------------------ */
/*  Forward declarations                                               */
/* ------------------------------------------------------------------ */

struct kobject;
struct kset;
struct kobj_type;

/* ------------------------------------------------------------------ */
/*  kobj_type — type descriptor for a kobject                          */
/* ------------------------------------------------------------------ */

struct kobj_type {
    void               (*release)(struct kobject *kobj);
    const struct sysfs_ops  *sysfs_ops;
    struct attribute       **default_attrs;
};

/* ------------------------------------------------------------------ */
/*  kset_uevent_ops — hotplug event callbacks for a kset               */
/* ------------------------------------------------------------------ */

enum kobject_action {
    KOBJ_ADD    = 1,
    KOBJ_REMOVE = 2,
    KOBJ_CHANGE = 3,
    KOBJ_MOVE   = 4,
    KOBJ_ONLINE = 5,
    KOBJ_OFFLINE = 6,
    KOBJ_BIND   = 7,
    KOBJ_UNBIND = 8,
};

struct kset_uevent_ops {
    int (*filter)(struct kset *kset, struct kobject *kobj);
    const char *(*name)(struct kset *kset, struct kobject *kobj);
    int (*uevent)(struct kset *kset, struct kobject *kobj,
                  char *envp[], int nenv);
};

/* ------------------------------------------------------------------ */
/*  kobject — the core object-model primitive                          */
/* ------------------------------------------------------------------ */

#define KOBJ_NAME_LEN 64

struct kobject {
    const char        *name;        /* name in sysfs */
    struct kobject    *parent;      /* parent kobject (NULL = sysfs root) */
    struct kset       *kset;        /* containing kset */
    struct kobj_type  *ktype;       /* type descriptor */
    kref_t             kref;        /* reference counter */

    /* internal */
    clist_t            children;    /* circular list of child kobjects */
    clist_t            attributes;  /* circular list of sysfs_attr_entry_t */
    clist_t            symlinks;    /* circular list of sysfs_symlink_entry_t */
    vfs_node_t         sd;          /* sysfs directory VFS node */
    spinlock_t         lock;        /* protects children/attributes/symlinks */

    unsigned int       state_initialized:1;
    unsigned int       state_in_sysfs:1;
};

/* ------------------------------------------------------------------ */
/*  kset — a collection of kobjects (appears as a sysfs subdirectory)  */
/* ------------------------------------------------------------------ */

struct kset {
    clist_t          list;          /* circular list of kobject entries */
    spinlock_t       list_lock;     /* protects list modifications */
    struct kobject   kobj;          /* embedded kobject (the default parent) */
    const struct kset_uevent_ops *uevent_ops;
};

/* ------------------------------------------------------------------ */
/*  kobject lifecycle API                                              */
/* ------------------------------------------------------------------ */

/* Initialise a kobject (must be called before kobject_add) */
void kobject_init(struct kobject *kobj, struct kobj_type *ktype);

/* Add a kobject to the hierarchy (creates sysfs directory) */
int kobject_add(struct kobject *kobj, struct kobject *parent,
                const char *fmt, ...);

/* Combined init + add with a va_list name format */
int kobject_init_and_add(struct kobject *kobj, struct kobj_type *ktype,
                         struct kobject *parent, const char *fmt, ...);

/* Allocate, init, and add a standalone kobject (creates a directory) */
struct kobject *kobject_create_and_add(const char *name,
                                       struct kobject *parent);

/* Take a reference on a kobject */
struct kobject *kobject_get(struct kobject *kobj);

/* Drop a reference on a kobject */
void kobject_put(struct kobject *kobj);

/* Remove a kobject from the hierarchy (removes sysfs directory) */
void kobject_del(struct kobject *kobj);

/* Rename a kobject */
int kobject_rename(struct kobject *kobj, const char *new_name);

/* Set the name of a kobject */
int kobject_set_name(struct kobject *kobj, const char *fmt, ...);

/* Move a kobject to a new parent */
int kobject_move(struct kobject *kobj, struct kobject *new_parent);

/* Return a pointer to the kobject's name */
const char *kobject_name(const struct kobject *kobj);

/* ------------------------------------------------------------------ */
/*  kset lifecycle API                                                 */
/* ------------------------------------------------------------------ */

/* Initialise a kset */
void kset_init(struct kset *kset);

/* Create a kset, add it, and return it */
struct kset *kset_create_and_add(const char *name,
                                 const struct kset_uevent_ops *uevent_ops,
                                 struct kobject *parent_kobj);

/* Unregister a kset (reverse of kset_create_and_add) */
void kset_unregister(struct kset *kset);

/* Get a reference to the kset */
static inline struct kset *kset_get(struct kset *kset)
{
    if (kset) kobject_get(&kset->kobj);
    return kset;
}

/* Drop a reference to the kset */
static inline void kset_put(struct kset *kset)
{
    if (kset) kobject_put(&kset->kobj);
}

/* ------------------------------------------------------------------ */
/*  uevent helpers                                                     */
/* ------------------------------------------------------------------ */

/* Send a KOBJ_ADD uevent for a kobject */
int kobject_uevent(struct kobject *kobj, enum kobject_action action);

/* Send a KOBJ_ADD event with environment variables */
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action,
                       char *envp[], int nenv);

/* Get the current uevent sequence number */
uint64_t kobject_uevent_seqnum(void);

#endif // INCLUDE_KOBJECT_H_
