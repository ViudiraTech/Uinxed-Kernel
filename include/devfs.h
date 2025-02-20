/*
 *
 *		devfs.h
 *		块设备文件系统头文件
 *
 *		2024/11/24 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
 *
 */

#ifndef INCLUDE_DEVFS_H_
#define INCLUDE_DEVFS_H_

#include "vfs.h"

int devfs_mount(const char* src, vfs_node_t node);
void devfs_regist(void);
int dev_get_sector_size(char *path);
int dev_get_size(char *path);
int dev_get_type(char *path); // 1:HDD 2:CDROM
void print_devfs(void);
void devfs_sysinfo_init(void);

#endif // INCLUDE_DEVFS_H_
