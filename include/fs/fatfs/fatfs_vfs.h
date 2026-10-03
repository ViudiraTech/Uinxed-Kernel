/*
 *
 *      fatfs_vfs.h
 *      FatFs bridge for VFS
 *
 *      2026/5/18 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FATFS_VFS_H_
#define INCLUDE_FATFS_VFS_H_

/* Mount a FatFs volume at a VFS path. */
int fatfs_vfs_mount_volume(const char *src, const char *path);

/* Register the fatfs filesystem with the VFS layer. */
#if CONFIG_FAT_FS
void fatfs_vfs_regist(void);
#else
static inline void fatfs_vfs_regist(void) {}
#endif

#endif // INCLUDE_FATFS_VFS_H_
