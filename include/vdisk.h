/*
 *
 *		vdisk.h
 *		虚拟磁盘/磁盘抽象层头文件
 *
 *		2024/11/24 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_VDISK_H_
#define INCLUDE_VDISK_H_

#include "types.h"
#include "ctypes.h"

typedef struct {
	void (*Read) (int drive, uint8_t *buffer, uint32_t number, uint32_t lba);
	void (*Write)(int drive, uint8_t *buffer, uint32_t number, uint32_t lba);
	int flag;
	uint32_t size;			// 大小
	uint32_t sector_size;	// 扇区大小
	char DriveName[50];
} vdisk;

/* 初始化虚拟磁盘 */
void vdisk_init(void);

/* 注册一个新的虚拟磁盘 */
int register_vdisk(vdisk vd);

/* 注销虚拟磁盘 */
int logout_vdisk(int drive);

/* 对虚拟磁盘读写 */
int rw_vdisk(int drive, uint32_t lba, uint8_t *buffer, uint32_t number, int read);

/* 检查指定驱动器是否已被注册 */
int have_vdisk(int drive);

/* 为虚拟磁盘设置一个名称 */
int set_drive(uint8_t *name);

/* 根据磁盘名返回磁盘号码 */
uint32_t get_drive_code(uint8_t *name);

/* 获取驱动器的信号量 */
int drive_semaphore_take(uint32_t drive_code);

/* 释放驱动器的信号量 */
void drive_semaphore_give(uint32_t drive_code);

/* 从虚拟磁盘读取数据 */
void Disk_Read(uint32_t lba, uint32_t number, void *buffer, int drive);

/* 获取指定虚拟磁盘的总大小 */
uint32_t disk_size(int drive);

/* 检查虚拟磁盘是否就绪 */
int DiskReady(int drive);

/* 获取一个就绪的虚拟磁盘 */
int getReadyDisk(void);

/* 向虚拟磁盘写入数据 */
void Disk_Write(uint32_t lba, uint32_t number, const void *buffer, int drive);

#endif // INCLUDE_VDISK_H_
