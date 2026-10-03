/*
 *
 *      devtmpfs.h
 *      Device tmpfs population helpers
 *
 *      2026/5/20 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DEVTMPFS_H_
#define INCLUDE_DEVTMPFS_H_

#include <drivers/block/core/blockdev.h>
#include <drivers/block/core/partition.h>
#include <fs/tmpfs/tmpfs.h>
#include <kernel/kdev_t.h>

#define DEVTMPFS_MAX_BLOCK_NODES (CONFIG_PARTITION_MAX_COUNT + 1)

typedef struct devtmpfs_block_registration {
        char   paths[DEVTMPFS_MAX_BLOCK_NODES][96];
        size_t count;
} devtmpfs_block_registration_t;

/*
 * Initialize /dev on top of tmpfs and populate built-in device nodes.
 *
 * This currently includes block devices discovered from IDE, input devices,
 * framebuffer devices, and registered audio devices under /dev/snd.
 */
void devtmpfs_init(void);

/* Create a /dev device node for a device number.  Returns 0 on success, negative errno on failure. */
int devtmpfs_register_char_device(const char *path, dev_t dev, dev_t rdev, uint16_t node_type, const tmpfs_device_ops_t *ops);

/*
 * Unregister and remove a character device node previously registered via
 * devtmpfs_register_char_device().
 *
 * @path: absolute path under /dev
 *
 * Returns 0 on success, negative errno on failure.
 */
int devtmpfs_unregister_char_device(const char *path);

/*
 * Resolve a published /dev block node to the backend descriptor bound to it.
 * The returned descriptor holds a backend reference which the caller must
 * release with blockdev_release().
 */
int devtmpfs_open_block_device(const char *path, blockdev_device_t *device);

/* Publish a whole disk and, optionally, its MBR/GPT partition views. */
int devtmpfs_register_block_device(const char *path, const blockdev_device_t *device, dev_t dev, dev_t rdev, bool scan_partitions, bool use_p_separator, devtmpfs_block_registration_t **registration);

/* Remove all nodes in a dynamic block-device registration. */
void devtmpfs_unregister_block_device(devtmpfs_block_registration_t *registration);

/* Create a device node at an arbitrary path; an unregistered number opens with -ENXIO. */
int devtmpfs_mknod(const char *path, uint16_t mode, dev_t dev);

/* /proc/devices : one line per character/block device major. */
int devtmpfs_format_proc_devices(char *buf, size_t cap);

/* /proc/partitions : whole disks and their partition views. */
int devtmpfs_format_proc_partitions(char *buf, size_t cap);

/* /proc/diskstats : per-disk and per-partition I/O counters. */
int devtmpfs_format_proc_diskstats(char *buf, size_t cap);

#endif // INCLUDE_DEVTMPFS_H_
