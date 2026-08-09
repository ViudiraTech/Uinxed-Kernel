/*
 *
 *      block_sysfs.h
 *      Dynamic block-device sysfs registration
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_BLOCK_SYSFS_H_
#define INCLUDE_BLOCK_SYSFS_H_

#include <drivers/block/blockdev.h>
#include <libs/std/stdbool.h>

typedef struct block_sysfs_dev block_sysfs_dev_t;

void block_sysfs_init(void);
int  block_sysfs_register_device(const char *name, const blockdev_device_t *device, bool removable, block_sysfs_dev_t **handle);
void block_sysfs_unregister_device(block_sysfs_dev_t *handle);

#endif // INCLUDE_BLOCK_SYSFS_H_
