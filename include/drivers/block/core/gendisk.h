/*
 *
 *      gendisk.h
 *      Block device (gendisk) registry
 *
 *      2026/8/10 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_GENDISK_H_
#define INCLUDE_GENDISK_H_

#include <drivers/block/core/blockdev.h>

typedef struct devtmpfs_block_registration devtmpfs_block_registration_t;

typedef struct gendisk {
        char                           name[32]; // "sda", "nvme0n1", "sr0", ...
        uint32_t                       major;
        uint32_t                       minor_base; // whole-disk minor
        blockdev_device_t              device;     // whole-disk descriptor (retained)
        bool                           scan_partitions;
        bool                           use_p_separator; // partition names use a "p" separator
        devtmpfs_block_registration_t *devtmpfs;
        struct gendisk                *next;
} gendisk_t;

/* Partition iteration shared by devtmpfs, sysfs and procfs. */
typedef void (*block_partition_cb_t)(const gendisk_t *disk, const char *part_name, uint32_t major, uint32_t minor, uint64_t blocks, void *opaque);

/* Register a whole disk. The device descriptor is copied and retained; the caller may reuse or drop its own copy afterwards. */
int block_register_disk(const char *name, uint32_t major, uint32_t minor, const blockdev_device_t *device, bool scan_partitions, bool use_p_separator);

/* Remove a previously registered disk and release its backend reference. */
int block_unregister_disk(const char *name);

/* Discover and register every disk exposed by the storage backends. */
void block_register_all_disks(void);

/* Return the number of registered disks */
int block_disk_count(void);

/* Copy the disk at an index; the caller releases the backend with blockdev_release(). */
bool block_disk_snapshot(int index, gendisk_t *out);

/* Invoke cb for each partition of every registered disk. */
void block_foreach_partition(block_partition_cb_t cb, void *opaque);

#endif // INCLUDE_GENDISK_H_
