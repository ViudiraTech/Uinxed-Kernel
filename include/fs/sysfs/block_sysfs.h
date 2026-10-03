/*
 *
 *      block_sysfs.h
 *      Dynamic block-device sysfs registration
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_BLOCK_SYSFS_H_
#define INCLUDE_BLOCK_SYSFS_H_

#include <drivers/block/core/blockdev.h>
#include <kernel/errno.h>
#include <libs/kobject/kobject.h>

/* Per-block-device wrapper */
typedef struct block_sysfs_dev {
        struct kobject    kobj;
        blockdev_device_t bdev;
        char              name[32];
        uint32_t          partition;
        uint64_t          start_lba;
        int               read_only;
        int               removable;
        int               valid;
} block_sysfs_dev_t;

#if CONFIG_SYSFS

/* Export every registered disk to /sys/block/. */
void block_sysfs_init(void);

/* Register a whole disk under /sys/block/ with its partitions. */
int block_sysfs_register_device(const char *name, const blockdev_device_t *device, bool removable, block_sysfs_dev_t **handle);

/* Remove a disk and its partitions from sysfs. */
void block_sysfs_unregister_device(block_sysfs_dev_t *handle);

#else
static inline void block_sysfs_init(void) {}
static inline int  block_sysfs_register_device(const char *, const blockdev_device_t *, bool, block_sysfs_dev_t **)
{
    return -EOPNOTSUPP;
}
static inline void block_sysfs_unregister_device(block_sysfs_dev_t *) {}
#endif

#endif // INCLUDE_BLOCK_SYSFS_H_
