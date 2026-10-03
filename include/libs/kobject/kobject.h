/*
 *
 *      kobject.h
 *      Kernel object model header file
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_KOBJECT_H_
#define INCLUDE_KOBJECT_H_

#include <fs/core/vfs.h>
#include <fs/sysfs/sysfs.h>

#define KOBJ_NAME_LEN 64

/* kref - reference-counting primitive */

typedef struct kref {
        uint32_t refcount;
} kref_t;

/* Forward declarations */

struct kobject;
struct kset;
struct kobj_type;

struct kobj_uevent_env {
        char  envbuf[CONFIG_UEVENT_BUFFER_SIZE];
        char *envp[CONFIG_UEVENT_NUM_ENVP];
        int   envp_idx;
        int   buflen;
};

/* kobj_type - type descriptor for a kobject */

struct kobj_type {
        void (*release)(struct kobject *kobj);
        const struct sysfs_ops *sysfs_ops;
        struct attribute      **default_attrs;
        const char *(*uevent_name)(struct kobject *kobj);
        int (*uevent)(struct kobject *kobj, struct kobj_uevent_env *env);
};

/* kset_uevent_ops - hotplug event callbacks for a kset */

enum kobject_action {
    KOBJ_ADD     = 1,
    KOBJ_REMOVE  = 2,
    KOBJ_CHANGE  = 3,
    KOBJ_MOVE    = 4,
    KOBJ_ONLINE  = 5,
    KOBJ_OFFLINE = 6,
    KOBJ_BIND    = 7,
    KOBJ_UNBIND  = 8,
};

struct kset_uevent_ops {
        int (*filter)(struct kobject *kobj);
        const char *(*name)(struct kobject *kobj);
        int (*uevent)(struct kobject *kobj, struct kobj_uevent_env *env);
};

/* The core object-model primitive. */
struct kobject {
        const char       *name;   // name in sysfs
        struct kobject   *parent; // parent kobject (NULL = sysfs root)
        struct kset      *kset;   // containing kset
        struct kobj_type *ktype;  // type descriptor
        kref_t            kref;   // reference counter

        /* internal */
        clist_t    children;       // circular list of child kobjects
        clist_t    attributes;     // circular list of sysfs_attr_entry_t
        clist_t    bin_attributes; // list of sysfs_bin_attr_entry_t
        clist_t    symlinks;       // circular list of sysfs_symlink_entry_t
        vfs_node_t sd;             // sysfs directory VFS node
        spinlock_t lock;           // protects children/attributes/symlinks

        unsigned int state_initialized        : 1;
        unsigned int state_in_sysfs           : 1;
        unsigned int state_in_kset            : 1;
        unsigned int state_add_uevent_sent    : 1;
        unsigned int state_remove_uevent_sent : 1;
        unsigned int uevent_suppress          : 1;
};

/* kset - a collection of kobjects (appears as a sysfs subdirectory) */
struct kset {
        clist_t                       list;      // circular list of kobject entries
        spinlock_t                    list_lock; // protects list modifications
        struct kobject                kobj;      // embedded kobject (the default parent)
        const struct kset_uevent_ops *uevent_ops;
        unsigned int                  dynamic : 1;
};

/* Increment the reference count */
void kref_init(kref_t *kref);

/* Take an additional reference without resurrecting a released object. */
int kref_get_unless_zero(kref_t *kref);

/* Take an additional reference */
void kref_get(kref_t *kref);

/* Drop a reference; returns 1 if the count reached zero */
int kref_put(kref_t *kref, void (*release)(kref_t *kref));

/* Return the current reference count */
uint32_t kref_read(const kref_t *kref);

/* Initialise a kobject (must be called before kobject_add) */
void kobject_init(struct kobject *kobj, struct kobj_type *ktype);

/* Add a kobject to the hierarchy (creates sysfs directory) */
__attribute__((format(printf, 3, 4))) int kobject_add(struct kobject *kobj, struct kobject *parent, const char *fmt, ...);

/* Combined init + add with a va_list name format */
__attribute__((format(printf, 4, 5))) int kobject_init_and_add(struct kobject *kobj, struct kobj_type *ktype, struct kobject *parent, const char *fmt, ...);

/* Allocate, init, and add a standalone kobject (creates a directory) */
struct kobject *kobject_create_and_add(const char *name, struct kobject *parent);

/* Take a reference on a kobject */
struct kobject *kobject_get(struct kobject *kobj);

/* Drop a reference on a kobject */
void kobject_put(struct kobject *kobj);

/* Release callback for statically-allocated kobjects: nothing to free. */
void kobject_static_release(struct kobject *kobj);

/* Remove a kobject from the hierarchy (removes sysfs directory) */
void kobject_del(struct kobject *kobj);

/* Rename a kobject */
int kobject_rename(struct kobject *kobj, const char *new_name);

/* Set the name of a kobject */
__attribute__((format(printf, 2, 3))) int kobject_set_name(struct kobject *kobj, const char *fmt, ...);

/* Move a kobject to a new parent */
int kobject_move(struct kobject *kobj, struct kobject *new_parent);

/* Return a pointer to the kobject's name */
const char *kobject_name(const struct kobject *kobj);

/* Find a child kobject by name, or NULL */
struct kobject *kobject_find_child(struct kobject *parent, const char *name);

/* Append data to a circular linked list, creating the node as needed */
int kobject_list_add(clist_t *list, void *data);

/* Initialise a kset */
void kset_init(struct kset *kset);

/* Create a kset, add it, and return it */
struct kset *kset_create_and_add(const char *name, const struct kset_uevent_ops *uevent_ops, struct kobject *parent_kobj);

/* Unregister a kset (reverse of kset_create_and_add) */
void kset_unregister(struct kset *kset);

/* Get a reference to the kset */
struct kset *kset_get(struct kset *kset);

/* Drop a reference to the kset */
void kset_put(struct kset *kset);

/* Send a KOBJ_ADD uevent for a kobject */
int kobject_uevent(struct kobject *kobj, enum kobject_action action);

/* Send a KOBJ_ADD event with environment variables */
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action, char *envp[], int nenv);

/* Append one KEY=value entry to a type or subsystem uevent callback. */
__attribute__((format(printf, 2, 3))) int add_uevent_var(struct kobj_uevent_env *env, const char *fmt, ...);

/* Parse and emit a userspace-triggered action written to a sysfs uevent file. */
int kobject_synth_uevent(struct kobject *kobj, const char *buf, size_t count);

/* Translate between the stable textual ABI and internal action values. */
const char *kobject_action_name(enum kobject_action action);
int         kobject_action_type(const char *name, enum kobject_action *action);

/* Get the current uevent sequence number */
uint64_t kobject_uevent_seqnum(void);

#endif // INCLUDE_KOBJECT_H_
