/*
 *
 *		vdisk.c
 *		虚拟磁盘/磁盘抽象层头文件
 *
 *		2024/11/24 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "vdisk.h"
#include "debug.h"
#include "printk.h"
#include "fifo.h"
#include "string.h"

vdisk vdisk_ctl[26];

void vdisk_init(void)
{
	for (int i = 0; i < 26; i++) {
		vdisk_ctl[i].flag = 0;		// 设置为未使用
	}
	print_succ("VDisk interface initialize.\n");
}

int register_vdisk(vdisk vd)
{
	for (int i = 0; i < 26; i++) {
		if (!vdisk_ctl[i].flag) {
			vdisk_ctl[i] = vd;		// 找到了！
			return i;				// 注册成功，返回drive
		}
	}
	return 0; // 注册失败
}

int logout_vdisk(int drive)
{
	int indx = drive;
	if (indx > 26) {
		return 0;					// 失败
	}
	if (vdisk_ctl[indx].flag) {
		vdisk_ctl[indx].flag = 0;	// 设置为没有
		return 1;					// 成功
	} else {
		return 0;					// 失败
	}
}

int rw_vdisk(int drive, uint32_t lba, uint8_t *buffer, uint32_t number, int read)
{
	int indx = drive;
	if (indx > 26) {
		return 0;					// 失败
	}
	if (vdisk_ctl[indx].flag) {
		if (read) {
			vdisk_ctl[indx].Read(drive, buffer, number, lba);
		} else {
			vdisk_ctl[indx].Write(drive, buffer, number, lba);
		}
		return 1;					// 成功
	} else {
		return 0;					// 失败
	}
}

int have_vdisk(int drive)
{
	int indx = drive;
	// printk("drive=%c\n",drive);
	if (indx > 26) {
		return 0;					// 失败
	}
	if (vdisk_ctl[indx].flag) {
		return 1;					// 成功
	} else {
		return 0;					// 失败
	}
}

/* 基于vdisk的通用读写 */
static uint8_t *drive_name[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                  0, 0, 0, 0, 0, 0, 0, 0};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"

static struct FIFO drive_fifo[16];
static uint8_t drive_buf[16][256];

#pragma GCC diagnostic pop

int set_drive(uint8_t *name)
{
	for (int i = 0; i != 16; i++) {
		if (drive_name[i] == 0) {
			drive_name[i] = name;
			//cir_queue8_init(&drive_fifo[i], 256, drive_buf[i]);
			return 1;
		}
	}
	return 0;
}

uint32_t get_drive_code(uint8_t *name)
{
	for (int i = 0; i != 16; i++) {
		if (strcmp((char *)drive_name[i], (char *)name) == 0) { return i; }
	}
	return 16;
}

int drive_semaphore_take(uint32_t drive_code)
{
	return 1;
	/*
	 * if (drive_code >= 16) { return 1; }
	 * cir_queue8_put(&drive_fifo[drive_code], get_tid(current_task()));
	 * // printk("FIFO: %d PUT: %d STATUS: %d\n", drive_code, Get_Tid(current_task()),
	 * //		fifo8_status(&drive_fifo[drive_code]));
	 * while (drive_buf[drive_code][drive_fifo[drive_code].head] != get_tid(current_task())) {
	 * 	// printk("Waiting....\n");
	 * }
	 * return 1;
	 */
}

void drive_semaphore_give(uint32_t drive_code)
{
	return;
	/*
	 * if (drive_code >= 16) { return; }
	 * if (drive_buf[drive_code][drive_fifo[drive_code].head] != get_tid(current_task())) {
	 * 	// 暂时先不做处理 一般不会出现这种情况
	 * 	return;
	 * }
	 * cir_queue8_get(&drive_fifo[drive_code]);
	 */
}

#define SECTORS_ONCE 8

void Disk_Read(uint32_t lba, uint32_t number, void *buffer, int drive)
{
	if (have_vdisk(drive)) {
		if (drive_semaphore_take(get_drive_code((uint8_t *)"DISK_DRIVE"))) {
			// printk("*buffer(%d %d) = %02x\n",lba,number,*(uint8_t *)buffer);
			for (int i = 0; i < number; i += SECTORS_ONCE) {
				int sectors = ((number - i) >= SECTORS_ONCE) ? SECTORS_ONCE : (number - i);
				rw_vdisk(drive, lba + i, (uint8_t *)((uint32_t)buffer + i * vdisk_ctl[drive].sector_size), sectors,
						1);
			}
			drive_semaphore_give(get_drive_code((uint8_t *)"DISK_DRIVE"));
		}
	}
}

uint32_t disk_Size(int drive)
{
	uint8_t drive1 = drive;
	if (have_vdisk(drive1)) {
		int indx = drive1;
		return vdisk_ctl[indx].size;
	} else {
		return 0;
	}
	
	return 0;
}

int DiskReady(int drive)
{
	return have_vdisk(drive);
}

int getReadyDisk(void)
{
	return 0;
}

void Disk_Write(uint32_t lba, uint32_t number, const void *buffer, int drive)
{
	//  printk("%d\n",lba);
	if (have_vdisk(drive)) {
		if (drive_semaphore_take(get_drive_code((uint8_t *)"DISK_DRIVE"))) {
			// printk("*buffer(%d %d) = %02x\n",lba,number,*(uint8_t *)buffer);
			for (int i = 0; i < number; i += SECTORS_ONCE) {
				int sectors = ((number - i) >= SECTORS_ONCE) ? SECTORS_ONCE : (number - i);
				rw_vdisk(drive, lba + i, (uint8_t *)((uint32_t)buffer + i * vdisk_ctl[drive].sector_size), sectors,
						0);
			}
			drive_semaphore_give(get_drive_code((uint8_t *)"DISK_DRIVE"));
		}
	}
}
