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

struct module;
struct kobject;

typedef struct module_sysfs module_sysfs_t;

/* Locate the /sys/module/ kobject. */
void module_sysfs_init(void);

/* Publish a module under /sys/module/<name>/. */
int module_sysfs_create(struct module *module, module_sysfs_t **handle);

/* Remove a module's /sys/module entry. */
void module_sysfs_destroy(module_sysfs_t *entry);

/* Return the kobject of a module's sysfs entry (or NULL). */
struct kobject *module_sysfs_kobj(module_sysfs_t *entry);

#endif // INCLUDE_MODULE_SYSFS_H_
