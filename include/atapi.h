/*
 *
 *		atapi.h
 *		IDE光盘驱动器驱动头文件
 *
 *		2024/9/8 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#ifndef INCLUDE_ATAPI_H_
#define INCLUDE_ATAPI_H_

#include "types.h"
#include "common.h"

/* 便捷的寄存器编号的定义 */
#define DATA 0
#define ERROR_R 1
#define SECTOR_COUNT 2
#define LBA_LOW 3
#define LBA_MID 4
#define LBA_HIGH 5
#define DRIVE_SELECT 6
#define COMMAND_REGISTER 7

/* 控制寄存器的定义 */
#define CONTROL 0x206
#define ALTERNATE_STATUS 0

/* 从LBA到指针来读光盘扇区 */
int read_cdrom(uint16_t port, bool slave, uint32_t lba, uint32_t sectors, uint16_t *buffer);

#endif //INCLUDE_ATAPI_H_
