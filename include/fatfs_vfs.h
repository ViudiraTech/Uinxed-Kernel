/*
 *
 *      fatfs_vfs.h
 *      FatFs bridge for VFS
 *
 *      2026/5/18 By Rainy101112
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_FATFS_VFS_H_
#define INCLUDE_FATFS_VFS_H_

int  fatfs_vfs_mount_volume(const char *src, const char *path);
void fatfs_vfs_regist(void);

#endif // INCLUDE_FATFS_VFS_H_
