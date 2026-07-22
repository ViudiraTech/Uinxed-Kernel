/*
 *
 *      fatfs_disk.h
 *      FatFs block device binding helpers
 *
 *      2026/5/20 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FATFS_DISK_H_
#define INCLUDE_FATFS_DISK_H_

#include <drivers/blockdev.h>
#include <fs/fatfs/ff.h>
#include <libs/std/stdint.h>

int  fatfs_bind_device(uint8_t drive, const blockdev_device_t *device);
void fatfs_unbind_device(uint8_t drive);
int  fatfs_assign_volume(uint8_t volume, uint8_t drive, uint8_t partition);
void fatfs_reset_volumes(void);

#endif // INCLUDE_FATFS_DISK_H_
