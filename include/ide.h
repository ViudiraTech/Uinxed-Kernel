/*
 *
 *		ide.h
 *		IDE设备驱动头文件
 *
 *		2024/7/11 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_IDE_H_
#define INCLUDE_IDE_H_

#include "types.h"

#define SECTSIZE 512 // 默认扇区大小

/*
 * bit 7 = 1	控制器忙		* bit 6 = 1		驱动器就绪
 * bit 5 = 1	设备错误		* bit 4			N/A
 * bit 3 = 1	扇区缓冲区错误	* bit 2 = 1		磁盘已被读校验
 * bit 1		N/A			* bit 0 = 1		上一次命令执行失败
 */
#define IDE_BSY					0x80		// IDE驱动器忙 
#define IDE_DRDY				0x40		// IDE驱动器就绪
#define IDE_DF					0x20		// IDE驱动器错误
#define IDE_ERR					0x01		// 上一次命令失败

#define IDE_CMD_READ			0x20		// IDE读扇区命令
#define IDE_CMD_WRITE			0x30		// IDE写扇区命令
#define IDE_CMD_IDENTIFY		0xEC		// IDE识别命令

/* IDE设备端口起始端口定义 */
#define IOBASE					0x1F0		// 主IDE设备起始操作端口
#define IOCTRL					0x3F4		// 主IDE控制起始控制端口

/* IDE设备控制端口偏移量 */
#define ISA_DATA				0x00		// IDE数据端口偏移
#define ISA_ERROR				0x01		// IDE错误端口偏移
#define ISA_PRECOMP				0x01
#define ISA_CTRL				0x02		// IDE控制端口偏移
#define ISA_SECCNT				0x02
#define ISA_SECTOR				0x03
#define ISA_CYL_LO				0x04
#define ISA_CYL_HI				0x05
#define ISA_SDH					0x06		// IDE选择端口偏移
#define ISA_COMMAND				0x07		// IDE命令端口偏移
#define ISA_STATUS				0x07		// IDE状态端口偏移

/* IDE设备限制值 */
#define MAX_NSECS				128			// IDE设备最大操作扇区数
#define MAX_DISK_NSECS			0x10000000	// IDE设备最大扇区号

/* IDE设备身份信息在读取的信息块中的偏移 */
#define IDE_IDENT_SECTORS		20
#define IDE_IDENT_MODEL			54
#define IDE_IDENT_CAPABILITIES	98
#define IDE_IDENT_CMDSETS		164
#define IDE_IDENT_MAX_LBA		120
#define IDE_IDENT_MAX_LBA_EXT	200

#define IDE_DESC_LEN			40			// IDE设备描述信息尺寸

/* 初始化IDE设备 */
void init_ide(void);

/* 获取IDE设备描述 */
char *ide_get_desc(void);

/* 获取IDE设备扇区大小 */
int ide_get_size(void);

/* 检测是否存在IDE控制器 */
int check_ide_controller(void);

/* 检测IDE设备是否可用 */
int ide_device_valid(void);

/* 读取IDE设备若干扇区 */
int ide_read_secs(uint32_t secno, void *dst, uint32_t nsecs);

/* 写入IDE设备若干扇区 */
int ide_write_secs(uint32_t secno, const void *src, uint32_t nsecs);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"

/* IDE设备信息 */
static struct ide_device {
	uint8_t valid;							// 是否可用
	uint32_t sets;							// 命令支持
	uint32_t size;							// 扇区数量
	char desc[IDE_DESC_LEN+1];				// IDE设备描述
} ide_device;

#pragma GCC diagnostic pop

#endif // INCLUDE_IDE_H_
