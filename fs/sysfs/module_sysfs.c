/*
 *
 *      module_sysfs.c
 *      Loadable module sysfs integration (/sys/module/<name>/)
 *
 *      2026/8/15 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <fs/sysfs/module_sysfs.h>
#include <fs/sysfs/sysfs.h>
#include <kernel/errno.h>
#include <kernel/module/module.h>
#include <libs/kobject/kobject.h>
#include <libs/list/circular_list.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/heap.h>

typedef struct module_group {
        struct kobject     kobj;
        struct module     *module;
        struct attribute **attrs;
        size_t             attr_count;
} module_group_t;

typedef struct module_sysfs {
        struct kobject  kobj;
        struct module  *module;
        module_group_t *sections;
        module_group_t *parameters;
        struct kobject *holders;
} module_sysfs_t;

static struct kobject *module_kobj;

/* Module attributes */

static struct attribute module_state_attr      = {.name = "state", .mode = 0444};
static struct attribute module_refcnt_attr     = {.name = "refcnt", .mode = 0444};
static struct attribute module_taint_attr      = {.name = "taint", .mode = 0444};
static struct attribute module_version_attr    = {.name = "version", .mode = 0444};
static struct attribute module_coresize_attr   = {.name = "coresize", .mode = 0444};
static struct attribute module_initsize_attr   = {.name = "initsize", .mode = 0444};
static struct attribute module_srcversion_attr = {.name = "srcversion", .mode = 0444};

static struct attribute *module_attrs[] = {
    &module_state_attr, &module_refcnt_attr, &module_taint_attr, &module_version_attr, &module_coresize_attr, &module_initsize_attr, &module_srcversion_attr, NULL,
};

/* Show one module attribute. */
static ssize_t module_attr_show(struct kobject *kobj, struct attribute *attribute, char *buffer)
{
    module_sysfs_t *entry  = (module_sysfs_t *)((char *)kobj - offsetof(module_sysfs_t, kobj));
    struct module  *module = __atomic_load_n(&entry->module, __ATOMIC_ACQUIRE);
    if (!module || !try_module_get(module)) return -ENODEV;
    ssize_t result;
    if (attribute == &module_state_attr) {
        result = (ssize_t)sysfs_emit(buffer, "%s\n", module_state_name(module->state));
    } else if (attribute == &module_refcnt_attr) {
        uint32_t refs = module_refcount(module);
        result        = (ssize_t)sysfs_emit(buffer, "%u\n", refs ? refs - 1 : 0);
    } else if (attribute == &module_taint_attr) {
        char   taint[8];
        size_t length = 0;
        if (module->taints & MODULE_TAINT_PROPRIETARY) taint[length++] = 'P';
        if (module->taints & MODULE_TAINT_FORCED) taint[length++] = 'F';
        if (module->taints & MODULE_TAINT_UNSIGNED) taint[length++] = 'E';
        if (module->taints & MODULE_TAINT_OUT_OF_TREE) taint[length++] = 'O';
        taint[length] = 0;
        result        = (ssize_t)sysfs_emit(buffer, "%s\n", taint);
    } else if (attribute == &module_version_attr) {
        result = (ssize_t)sysfs_emit(buffer, "%s\n", module_version(module));
    } else if (attribute == &module_coresize_attr) {
        result = (ssize_t)sysfs_emit(buffer, "%zu\n", module->core_size);
    } else if (attribute == &module_initsize_attr) {
        result = (ssize_t)sysfs_emit(buffer, "%zu\n", module->init_size);
    } else if (attribute == &module_srcversion_attr) {
        result = (ssize_t)sysfs_emit(buffer, "%s\n", module_srcversion(module));
    } else {
        result = -EIO;
    }
    module_put(module);
    return result;
}

static const struct sysfs_ops module_sysfs_ops = {
    .show  = module_attr_show,
    .store = NULL,
};

/* Free a module sysfs entry. */
static void module_kobject_release(struct kobject *kobj)
{
    module_sysfs_t *entry = (module_sysfs_t *)((char *)kobj - offsetof(module_sysfs_t, kobj));
    free(entry);
}

static struct kobj_type module_ktype = {
    .release       = module_kobject_release,
    .sysfs_ops     = &module_sysfs_ops,
    .default_attrs = module_attrs,
};

/* Section sub-directory (/sys/module/<name>/sections/) */

/* Show one section's load address. */
static ssize_t module_section_show(struct kobject *kobj, struct attribute *attribute, char *buffer)
{
    module_group_t *group  = (module_group_t *)((char *)kobj - offsetof(module_group_t, kobj));
    struct module  *module = __atomic_load_n(&group->module, __ATOMIC_ACQUIRE);
    if (!module || !try_module_get(module)) return -ENODEV;
    ssize_t result = -ENOENT;
    for (size_t i = 0; i < module_section_count(module); i++) {
        const char *name = module_section_name(module, i);
        if (name && streq(attribute->name, name)) {
            result = (ssize_t)sysfs_emit(buffer, "0x%lx\n", (unsigned long)module_section_address(module, i));
            break;
        }
    }
    module_put(module);
    return result;
}

static const struct sysfs_ops module_sections_ops = {
    .show  = module_section_show,
    .store = NULL,
};

/* Parameter sub-directory (/sys/module/<name>/parameters/) */

/* Show one parameter's current value. */
static ssize_t module_param_show(struct kobject *kobj, struct attribute *attribute, char *buffer)
{
    module_group_t *group  = (module_group_t *)((char *)kobj - offsetof(module_group_t, kobj));
    struct module  *module = __atomic_load_n(&group->module, __ATOMIC_ACQUIRE);
    if (!module || !try_module_get(module)) return -ENODEV;
    ssize_t result = -ENOENT;
    for (size_t i = 0; i < module_param_count(module); i++) {
        const char *name = module_param_name(module, i);
        if (name && streq(attribute->name, name)) {
            int n  = module_param_value(module, i, buffer, SYSFS_PAGE_SIZE);
            result = n >= 0 ? (ssize_t)n : -EIO;
            break;
        }
    }
    module_put(module);
    return result;
}

static const struct sysfs_ops module_params_ops = {
    .show  = module_param_show,
    .store = NULL,
};

/* Shared group (sections/parameters) lifecycle */

/* Free a group and its attributes. */
static void module_group_release(struct kobject *kobj)
{
    module_group_t *group = (module_group_t *)((char *)kobj - offsetof(module_group_t, kobj));
    for (size_t i = 0; i < group->attr_count; i++) {
        free((void *)group->attrs[i]->name);
        free(group->attrs[i]);
    }
    free(group->attrs);
    free(group);
}

static struct kobj_type module_sections_ktype = {
    .release   = module_group_release,
    .sysfs_ops = &module_sections_ops,
};

static struct kobj_type module_params_ktype = {
    .release   = module_group_release,
    .sysfs_ops = &module_params_ops,
};

/* Create a sub-directory of attribute files. */
static module_group_t *module_group_create(module_sysfs_t *entry, struct module *module, const char *subdir, struct kobj_type *ktype, size_t (*count_fn)(const struct module *),
                                           const char *(*name_fn)(const struct module *, size_t))
{
    size_t count = count_fn(module);
    if (!count) return NULL;

    module_group_t *group = calloc(1, sizeof(*group));
    if (!group) return NULL;
    group->module = module;

    kobject_init(&group->kobj, ktype);
    if (kobject_add(&group->kobj, &entry->kobj, "%s", subdir) != EOK) {
        kobject_put(&group->kobj);
        return NULL;
    }

    group->attrs = calloc(count, sizeof(struct attribute *));
    if (!group->attrs) {
        kobject_put(&group->kobj);
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        const char *name = name_fn(module, i);
        if (!name) continue;
        struct attribute *attr = calloc(1, sizeof(*attr));
        if (!attr) continue;
        attr->name = strdup(name);
        attr->mode = 0444;
        if (!attr->name || sysfs_create_file(&group->kobj, attr) != EOK) {
            free((void *)attr->name);
            free(attr);
            continue;
        }
        group->attrs[group->attr_count++] = attr;
    }

    return group;
}

/* Remove a sub-directory. */
static void module_group_destroy(module_group_t *group)
{
    if (!group) return;
    kobject_del(&group->kobj);
    kobject_put(&group->kobj);
}

/* Return the kobject of a module's sysfs entry (or NULL). */
struct kobject *module_sysfs_kobj(module_sysfs_t *entry)
{
    return entry ? &entry->kobj : NULL;
}

/* Holder sub-directory (/sys/module/<name>/holders/) of symlinks. */
static void module_holders_create(module_sysfs_t *entry, struct module *module)
{
    size_t count = module_holder_count(module);
    if (!count) return;

    struct kobject *holders = kobject_create_and_add("holders", &entry->kobj);
    if (!holders) return;

    for (size_t i = 0; i < count; i++) {
        struct module  *holder      = module_holder(module, i);
        struct kobject *holder_kobj = holder ? module_sysfs_object(holder) : NULL;
        if (!holder_kobj || !holder->name[0]) continue;
        (void)sysfs_create_symlink(holders, holder_kobj, holder->name);
    }

    entry->holders = holders;
}

/* Remove the holders sub-directory. */
static void module_holders_destroy(module_sysfs_t *entry)
{
    if (!entry->holders) return;
    kobject_del(entry->holders);
    kobject_put(entry->holders);
    entry->holders = NULL;
}

/* Locate the /sys/module/ kobject created by sysfs_kobject_init(). */
void module_sysfs_init(void)
{
#if CONFIG_SYSFS
    if (!sysfs_root_kobj) return;
    for (clist_t node = sysfs_root_kobj->children; node; node = node->next) {
        struct kobject *child = node->data;
        if (child && child->name && streq(child->name, "module")) {
            module_kobj = child;
            break;
        }
    }
    if (!module_kobj)
        plogk("module_sysfs: /sys/module/ not found.\n");
    else
        plogk("module_sysfs: registered /sys/module/\n");
#endif
}

/* Publish a module under /sys/module/<name>/. */
int module_sysfs_create(struct module *module, module_sysfs_t **handle)
{
#if CONFIG_SYSFS
    if (!module || !handle || !module_kobj) return -ENODEV;
    *handle               = NULL;
    module_sysfs_t *entry = calloc(1, sizeof(*entry));
    if (!entry) return -ENOMEM;
    entry->module = module;
    int ret       = kobject_init_and_add(&entry->kobj, &module_ktype, module_kobj, "%s", module->name);
    if (ret != EOK) {
        plogk("module_sysfs: Failed to create /sys/module/%s: %d\n", module->name, ret);
        kobject_put(&entry->kobj);
        return ret;
    }
    *handle           = entry;
    entry->sections   = module_group_create(entry, module, "sections", &module_sections_ktype, module_section_count, module_section_name);
    entry->parameters = module_group_create(entry, module, "parameters", &module_params_ktype, module_param_count, module_param_name);
    module_holders_create(entry, module);
    (void)kobject_uevent(&entry->kobj, KOBJ_ADD);
#else
    (void)module;
    (void)handle;
#endif
    return EOK;
}

/* Remove a module's /sys/module entry. */
void module_sysfs_destroy(module_sysfs_t *entry)
{
    if (!entry) return;
    __atomic_store_n(&entry->module, NULL, __ATOMIC_RELEASE);
    module_group_destroy(entry->sections);
    module_group_destroy(entry->parameters);
    module_holders_destroy(entry);
    kobject_del(&entry->kobj);
    kobject_put(&entry->kobj);
}
