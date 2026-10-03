/*
 *
 *      module_sysfs.h
 *      Loadable module sysfs integration header (/sys/module/)
 *
 *      2026/8/15 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_MODULE_SYSFS_H_
#define INCLUDE_MODULE_SYSFS_H_

#include <libs/kobject/kobject.h>

struct module;
struct module_group;

typedef struct module_group module_group_t;

typedef struct module_sysfs {
        struct kobject  kobj;
        struct module  *module;
        module_group_t *sections;
        module_group_t *parameters;
        struct kobject *holders;
} module_sysfs_t;

/* Locate the /sys/module/ kobject. */
#if CONFIG_MODULES
void module_sysfs_init(void);
#else
static inline void module_sysfs_init(void) {}
#endif

/* Publish a module under /sys/module/<name>/. */
int module_sysfs_create(struct module *module, module_sysfs_t **handle);

/* Remove a module's /sys/module entry. */
void module_sysfs_destroy(module_sysfs_t *entry);

/* Return the kobject of a module's sysfs entry (or NULL). */
struct kobject *module_sysfs_kobj(module_sysfs_t *entry);

#endif // INCLUDE_MODULE_SYSFS_H_
