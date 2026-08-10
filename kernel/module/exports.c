/*
 *
 *      exports.c
 *      Stable symbols exported to loadable kernel modules
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#define UINXED_MODULE_CORE
#include <drivers/base/device.h>
#include <fs/core/vfs.h>
#include <kernel/module/module.h>
#include <kernel/printk.h>
#include <libs/kobject/kobject.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/page.h>
#include <sync/spin_lock.h>

EXPORT_SYMBOL(printk);
EXPORT_SYMBOL(snprintf);

EXPORT_SYMBOL(malloc);
EXPORT_SYMBOL(calloc);
EXPORT_SYMBOL(aligned_alloc);
EXPORT_SYMBOL(realloc);
EXPORT_SYMBOL(free);
EXPORT_SYMBOL(usable_size);

EXPORT_SYMBOL(memcpy);
EXPORT_SYMBOL(memmove);
EXPORT_SYMBOL(memset);
EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(strlen);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strncmp);
EXPORT_SYMBOL(strcpy);
EXPORT_SYMBOL(strncpy);
EXPORT_SYMBOL(strcat);
EXPORT_SYMBOL(strchr);
EXPORT_SYMBOL(strrchr);
EXPORT_SYMBOL(strstr);
EXPORT_SYMBOL(strdup);

EXPORT_SYMBOL(spin_lock_irqsave);
EXPORT_SYMBOL(spin_unlock_irqrestore);
EXPORT_SYMBOL(spin_lock);
EXPORT_SYMBOL(spin_unlock);

EXPORT_SYMBOL(try_module_get);
EXPORT_SYMBOL(__module_get);
EXPORT_SYMBOL(module_put);
EXPORT_SYMBOL(module_refcount);
EXPORT_SYMBOL(module_symbol_get);
EXPORT_SYMBOL(module_symbol_put);

EXPORT_SYMBOL(vfs_node_alloc);
EXPORT_SYMBOL(vfs_node_retain);
EXPORT_SYMBOL(vfs_open);
EXPORT_SYMBOL(vfs_close);
EXPORT_SYMBOL(vfs_read);
EXPORT_SYMBOL(vfs_write);
EXPORT_SYMBOL(vfs_file_read);
EXPORT_SYMBOL(vfs_file_write);
EXPORT_SYMBOL(vfs_ioctl);
EXPORT_SYMBOL(vfs_poll);
EXPORT_SYMBOL(vfs_regist);
EXPORT_SYMBOL(vfs_regist_fs);
EXPORT_SYMBOL(vfs_mount);
EXPORT_SYMBOL(vfs_mount_fs);
EXPORT_SYMBOL(vfs_umount);

EXPORT_SYMBOL(kobject_init);
EXPORT_SYMBOL(kobject_add);
EXPORT_SYMBOL(kobject_init_and_add);
EXPORT_SYMBOL(kobject_create_and_add);
EXPORT_SYMBOL(kobject_get);
EXPORT_SYMBOL(kobject_put);
EXPORT_SYMBOL(kobject_del);
EXPORT_SYMBOL(kobject_uevent);
EXPORT_SYMBOL(sysfs_create_file);
EXPORT_SYMBOL(sysfs_remove_file);
EXPORT_SYMBOL(sysfs_create_group);
EXPORT_SYMBOL(sysfs_remove_group);

EXPORT_SYMBOL(bus_register);
EXPORT_SYMBOL(bus_unregister);
EXPORT_SYMBOL(device_register);
EXPORT_SYMBOL(device_unregister);
EXPORT_SYMBOL(device_create);
EXPORT_SYMBOL(device_destroy);
EXPORT_SYMBOL(driver_register);
EXPORT_SYMBOL(driver_unregister);
EXPORT_SYMBOL(class_register);
EXPORT_SYMBOL(class_unregister);

EXPORT_SYMBOL_GPL(alloc_frames);
EXPORT_SYMBOL_GPL(frame_retain_range);
EXPORT_SYMBOL_GPL(frame_release_range);
EXPORT_SYMBOL_GPL(page_map_to);
EXPORT_SYMBOL_GPL(page_map_new_to);
EXPORT_SYMBOL_GPL(page_unmap);
EXPORT_SYMBOL_GPL(get_kernel_pagedir);
