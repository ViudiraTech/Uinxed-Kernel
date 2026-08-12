/*
 *
 *      fatfs_disk.h
 *      FatFs block device binding helpers
 *
 *      2026/5/20 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FATFS_DISK_H_
#define INCLUDE_FATFS_DISK_H_

#include <drivers/block/core/blockdev.h>
#include <fs/fatfs/ff.h>
#include <libs/std/stdint.h>

/* Bind a block device to a FatFs physical drive. */
int fatfs_bind_device(uint8_t drive, const blockdev_device_t *device);

/* Unbind a FatFs physical drive, invalidating its ready state. */
void fatfs_unbind_device(uint8_t drive);

/* Map a FatFs logical volume to a physical drive and partition. */
int fatfs_assign_volume(uint8_t volume, uint8_t drive, uint8_t partition);

/* Clear every volume mapping. */
void fatfs_reset_volumes(void);

#endif // INCLUDE_FATFS_DISK_H_
