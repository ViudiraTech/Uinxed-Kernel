/*
 *
 *		ide.h
 *		标准ATA/ATAPI设备驱动头文件
 *
 *		2024/7/11 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_IDE_H_
#define INCLUDE_IDE_H_

#include "stdint.h"

#define ATA_SR_BSY				0x80
#define ATA_SR_DRDY				0x40
#define ATA_SR_DF				0x20
#define ATA_SR_DSC				0x10
#define ATA_SR_DRQ				0x08
#define ATA_SR_CORR				0x04
#define ATA_SR_IDX				0x02
#define ATA_SR_ERR				0x01
#define ATA_ER_BBK				0x80
#define ATA_ER_UNC				0x40
#define ATA_ER_MC				0x20
#define ATA_ER_IDNF				0x10
#define ATA_ER_MCR				0x08
#define ATA_ER_ABRT				0x04
#define ATA_ER_TK0NF			0x02
#define ATA_ER_AMNF				0x01
#define ATA_CMD_READ_PIO		0x20
#define ATA_CMD_READ_PIO_EXT	0x24
#define ATA_CMD_READ_DMA		0xC8
#define ATA_CMD_READ_DMA_EXT	0x25
#define ATA_CMD_WRITE_PIO		0x30
#define ATA_CMD_WRITE_PIO_EXT	0x34
#define ATA_CMD_WRITE_DMA		0xCA
#define ATA_CMD_WRITE_DMA_EXT	0x35
#define ATA_CMD_CACHE_FLUSH		0xE7
#define ATA_CMD_CACHE_FLUSH_EXT	0xEA
#define ATA_CMD_PACKET			0xA0
#define ATA_CMD_IDENTIFY_PACKET	0xA1
#define ATA_CMD_IDENTIFY		0xEC
#define ATAPI_CMD_READ			0xA8
#define ATAPI_CMD_EJECT			0x1B
#define ATA_IDENT_DEVICETYPE	0
#define ATA_IDENT_CYLINDERS		2
#define ATA_IDENT_HEADS			6
#define ATA_IDENT_SECTORS		12
#define ATA_IDENT_SERIAL		20
#define ATA_IDENT_MODEL			54
#define ATA_IDENT_CAPABILITIES	98
#define ATA_IDENT_FIELDVALID	106
#define ATA_IDENT_MAX_LBA		120
#define ATA_IDENT_COMMANDSETS	164
#define ATA_IDENT_MAX_LBA_EXT	200
#define IDE_ATA					0x00
#define IDE_ATAPI				0x01

#define ATA_MASTER				0x00
#define ATA_SLAVE				0x01
#define ATA_REG_DATA			0x00
#define ATA_REG_ERROR			0x01
#define ATA_REG_FEATURES		0x01
#define ATA_REG_SECCOUNT0		0x02
#define ATA_REG_LBA0			0x03
#define ATA_REG_LBA1			0x04
#define ATA_REG_LBA2			0x05
#define ATA_REG_HDDEVSEL		0x06
#define ATA_REG_COMMAND			0x07
#define ATA_REG_STATUS			0x07
#define ATA_REG_SECCOUNT1		0x08
#define ATA_REG_LBA3			0x09
#define ATA_REG_LBA4			0x0A
#define ATA_REG_LBA5			0x0B
#define ATA_REG_CONTROL			0x0C
#define ATA_REG_ALTSTATUS		0x0C
#define ATA_REG_DEVADDRESS		0x0D

#define ATA_PRIMARY				0x00
#define ATA_SECONDARY			0x01

#define ATA_READ				0x00
#define ATA_WRITE				0x01

struct IDE_channel_registers {
	uint16_t base;	// I/O基地址
	uint16_t ctrl;	// 控制基地址
	uint16_t bmide;	// 总线主IDE
	uint8_t nIEN;	// nIEN（无中断）
};

struct ide_device {
	uint8_t reserved;		// 驱动器状态
	uint8_t channel;		// 主从通道
	uint8_t drive;			// 主从驱动器
	uint16_t type;			// 驱动器类型
	uint16_t signature;		// 驱动器签名
	uint16_t capabilities;	// 特征
	uint32_t command_sets;	// 支持的命令集
	uint32_t size;			// 以扇区为单位的大小
	uint8_t model[41];		// 驱动器名称
};

/* 初始化IDE */
void init_ide(void);

/* 从IDE设备的指定寄存器读取一个字节数据 */
uint8_t ide_read(uint8_t channel, uint8_t reg);

/* 向IDE设备的指定寄存器写入一个字节数据 */
void ide_write(uint8_t channel, uint8_t reg, uint8_t data);

/* 从IDE设备的指定寄存器读取多个字的数据到缓冲区 */
void ide_read_buffer(uint8_t channel, uint8_t reg, uint64_t buffer, uint32_t quads);

/* 轮询IDE设备的状态 */
uint8_t ide_polling(uint8_t channel, uint32_t advanced_check);

/* 对ATA设备进行读写操作 */
uint8_t ide_ata_access(uint8_t direction, uint8_t drive, uint32_t lba, uint8_t numsects, uint64_t edi);

/* 从ATAPI设备读取数据 */
uint8_t ide_atapi_read(uint8_t drive, uint32_t lba, uint8_t numsects, uint32_t edi);

/* 从IDE设备读取多个扇区数据 */
void ide_read_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, uint32_t edi);

/* 向IDE设备写入多个扇区数据 */
void ide_write_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, uint32_t edi);

#endif // INCLUDE_IDE_H_
