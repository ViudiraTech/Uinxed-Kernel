/*
 *
 *		ide.c
 *		标准ATA/ATAPI设备驱动
 *
 *		2024/7/11 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "ide.h"
#include "common.h"
#include "idt.h"
#include "printk.h"
#include "pci.h"
#include "timer.h"
#include "string.h"

/* 结构体 */
struct IDE_channel_registers channels[2];
struct ide_device ide_devices[4];

/* 数据数组 */
uint8_t ide_buf[2048] = {0};
static uint8_t atapi_packet[12] = {0xa8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static int package[2];

/* 中断状态位 */
static volatile uint8_t ide_irq_invoked = 0;

/* IDE中断处理函数 */
__attribute__((interrupt)) static void ide_irq(interrupt_frame_t *frame)
{
	(void)frame;
	ide_irq_invoked = 1;
}

/* 等待IDE中断被触发 */
static void ide_wait_irq(void)
{
	while (!ide_irq_invoked);
	ide_irq_invoked = 0;
}

/* 设置IDE */
static void ide_initialize(uint32_t BAR0, uint32_t BAR1, uint32_t BAR2, uint32_t BAR3, uint32_t BAR4)
{
	int j, k, count = 0;
	for (int i = 0; i < 4; i++) {
		ide_devices[i].reserved = 0;
	}

	/* 检测IDE控制器的I/O端口 */
	channels[ATA_PRIMARY].base		= (BAR0 & 0xfffffffc) + 0x1f0 * (!BAR0);
	channels[ATA_PRIMARY].ctrl		= (BAR1 & 0xfffffffc) + 0x3f6 * (!BAR1);
	channels[ATA_SECONDARY].base	= (BAR2 & 0xfffffffc) + 0x170 * (!BAR2);
	channels[ATA_SECONDARY].ctrl	= (BAR3 & 0xfffffffc) + 0x376 * (!BAR3);
	channels[ATA_PRIMARY].bmide		= (BAR4 & 0xfffffffc) + 0;
	channels[ATA_SECONDARY].bmide	= (BAR4 & 0xfffffffc) + 8;

	/* 禁用IRQ */
	ide_write(ATA_PRIMARY, ATA_REG_CONTROL, 2);
	ide_write(ATA_SECONDARY, ATA_REG_CONTROL, 2);
	for (int i = 0; i < 2; i++)
		for (j = 0; j < 2; j++) {
			uint8_t err = 0, type = IDE_ATA, status;
			ide_devices[count].reserved = 0;

			/* 选择驱动器 */
			ide_write(i, ATA_REG_HDDEVSEL, 0xa0 | (j << 4));
			usleep(10);

			/* 发送ATA Identify命令 */
			ide_write(i, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
			usleep(10);

			if (ide_read(i, ATA_REG_STATUS) == 0) continue;
			while (1) {
				status = ide_read(i, ATA_REG_STATUS);
				if ((status & ATA_SR_ERR)) {
					err = 1;
					break;
				}
				if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) break;
			}

			/* ATAPI设备探测 */
			if (err != 0) {
				uint8_t cl = ide_read(i, ATA_REG_LBA1);
				uint8_t ch = ide_read(i, ATA_REG_LBA2);

				if (cl == 0x14 && ch == 0xeb)
					type = IDE_ATAPI;
				else if (cl == 0x69 && ch == 0x96)
					type = IDE_ATAPI;
				else
					continue;

				ide_write(i, ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
				usleep(10);
			}

			/* 读取设备的标识空间 */
			ide_read_buffer(i, ATA_REG_DATA, (uint64_t)ide_buf, 128);

			/* 读取设备参数 */
			ide_devices[count].reserved		= 1;
			ide_devices[count].type			= type;
			ide_devices[count].channel		= i;
			ide_devices[count].drive		= j;
			ide_devices[count].signature	= *((uint16_t *)(ide_buf + ATA_IDENT_DEVICETYPE));
			ide_devices[count].capabilities	= *((uint16_t *)(ide_buf + ATA_IDENT_CAPABILITIES));
			ide_devices[count].command_sets	= *((uint32_t *)(ide_buf + ATA_IDENT_COMMANDSETS));

			/* 获取大小 */
			if (ide_devices[count].command_sets & (1 << 26))
				/* 设备使用48位寻址 */
				ide_devices[count].size = *((uint32_t *)(ide_buf + ATA_IDENT_MAX_LBA_EXT));
			else
				/* 设备使用28位寻址 */
				ide_devices[count].size = *((uint32_t *)(ide_buf + ATA_IDENT_MAX_LBA));

			/* 获取设备型号 */
			for (k = 0; k < 40; k += 2) {
				ide_devices[count].model[k]		= ide_buf[ATA_IDENT_MODEL + k + 1];
				ide_devices[count].model[k + 1]	= ide_buf[ATA_IDENT_MODEL + k];
			}
			ide_devices[count].model[40] = 0;
			count++;
		}

	/* 打印设备信息 */
	for (int i = 0; i < 4; i++) {
		if (ide_devices[i].reserved == 1) {
			plogk("IDE: Found %s Drive %dMB - %s\n", ide_devices[i].type ? "ATAPI" : "ATA",
                  ide_devices[i].size / 1024 / 2, ide_devices[i].model);
		}
	}

}

/* 错误处理 */
static uint8_t ide_print_error(uint32_t drive, uint8_t err)
{
	if (err == 0) return err;
	if (err == 1) {
		plogk("IDE: Device fault.\n");
		err = 19;
	} else if (err == 2) {
		uint8_t st = ide_read(ide_devices[drive].channel, ATA_REG_ERROR);
		if (st & ATA_ER_AMNF) {
			plogk("IDE: No address mark found.\n");
			err = 7;
		}
		if (st & ATA_ER_TK0NF) {
			plogk("IDE: No media or media error.\n");
			err = 3;
		}
		if (st & ATA_ER_ABRT) {
			plogk("IDE: Command aborted.\n");
			err = 20;
		}
		if (st & ATA_ER_MCR) {
			plogk("IDE: No media or media error.\n");
			err = 3;
		}
		if (st & ATA_ER_IDNF) {
			plogk("IDE: ID mark not found.\n");
			err = 21;
		}
		if (st & ATA_ER_MC) {
			plogk("IDE: No media or media error.\n");
			err = 3;
		}
		if (st & ATA_ER_UNC) {
			plogk("IDE: Uncorrectable data error.\n");
			err = 22;
		}
		if (st & ATA_ER_BBK) {
			plogk("IDE: Bad sectors.\n");
			err = 13;
		}
	} else if (err == 3) {
		plogk("IDE: Reads nothing.\n");
		err = 23;
	} else if (err == 4) {
		plogk("IDE: Write protected.\n");
		err = 8;
	}
	return err;
}

/* 初始化IDE */
void init_ide(void)
{
	uint32_t bus, slot, func;

	/* 检测计算机是否拥有IDE控制器 */
	if (pci_found_class(0x010100, &bus, &slot, &func)) {
		register_interrupt_handler(IRQ_46, ide_irq, 0, 0x8e);
		register_interrupt_handler(IRQ_47, ide_irq, 0, 0x8e);
		ide_initialize(0x1f0, 0x3f6, 0x170, 0x376, 0x000);
		return;
	}
	plogk("IDE: Controller could not be found!\n");
}

/* 从IDE设备的指定寄存器读取一个字节数据 */
uint8_t ide_read(uint8_t channel, uint8_t reg)
{
	uint8_t result;
	if (reg > 0x07 && reg < 0x0c)
		ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN);
	if (reg < 0x08)
		result = inb(channels[channel].base + reg - 0x00);
	else if (reg < 0x0c)
		result = inb(channels[channel].base + reg - 0x06);
	else if (reg < 0x0e)
		result = inb(channels[channel].ctrl + reg - 0x0a);
	else if (reg < 0x16)
		result = inb(channels[channel].bmide + reg - 0x0E);
	if (reg > 0x07 && reg < 0x0c) ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN);
	return result;
}

/* 向IDE设备的指定寄存器写入一个字节数据 */
void ide_write(uint8_t channel, uint8_t reg, uint8_t data)
{
	if (reg > 0x07 && reg < 0x0c)
		ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN);
	if (reg < 0x08)
		outb(channels[channel].base + reg - 0x00, data);
	else if (reg < 0x0c)
		outb(channels[channel].base + reg - 0x06, data);
	else if (reg < 0x0e)
		outb(channels[channel].ctrl + reg - 0x0a, data);
	else if (reg < 0x16)
		outb(channels[channel].bmide + reg - 0x0e, data);
	if (reg > 0x07 && reg < 0x0c) ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN);
}

/* 从IDE设备的指定寄存器读取多个字的数据到缓冲区 */
void ide_read_buffer(uint8_t channel, uint8_t reg, uint64_t buffer, uint32_t quads)
{
	if (reg > 0x07 && reg < 0x0c)
		ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN);
	if (reg < 0x08)
		insl(channels[channel].base + reg - 0x00, (uint32_t *)buffer, quads);
	else if (reg < 0x0c)
		insl(channels[channel].base + reg - 0x06, (uint32_t *)buffer, quads);
	else if (reg < 0x0e)
		insl(channels[channel].ctrl + reg - 0x0a, (uint32_t *)buffer, quads);
	else if (reg < 0x16)
		insl(channels[channel].bmide + reg - 0x0e, (uint32_t *)buffer, quads);
	if (reg > 0x07 && reg < 0x0c) ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN);
}

/* 轮询IDE设备的状态 */
uint8_t ide_polling(uint8_t channel, uint32_t advanced_check)
{
	for (int i = 0; i < 4; i++)
		ide_read(channel, ATA_REG_ALTSTATUS);

	int a = ide_read(channel, ATA_REG_STATUS);
	while (a & ATA_SR_BSY) {
		a = ide_read(channel, ATA_REG_STATUS);
		usleep(10);
	}
	if (advanced_check) {
		uint8_t state = ide_read(channel, ATA_REG_STATUS);
		if (state & ATA_SR_ERR) return 2;
		if (state & ATA_SR_DF) return 1;
		if ((state & ATA_SR_DRQ) == 0) return 3;
	}
	return 0;
}

/* 对ATA设备进行读写操作 */
uint8_t ide_ata_access(uint8_t direction, uint8_t drive, uint32_t lba, uint8_t numsects, uint64_t edi)
{
	uint8_t lba_mode, dma, cmd;
	uint8_t lba_io[6];
	uint32_t channel = ide_devices[drive].channel;
	uint32_t slavebit = ide_devices[drive].drive;
	uint32_t bus = channels[channel].base;
	uint32_t words = 256;
	uint16_t cyl, i;
	uint8_t head, sect, err;

	ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN = (ide_irq_invoked = 0x0) + 0x02);
	if (lba >= 0x10000000) {
		lba_mode	= 2;
		lba_io[0]	= (lba & 0x000000ff) >> 0;
		lba_io[1]	= (lba & 0x0000ff00) >> 8;
		lba_io[2]	= (lba & 0x00ff0000) >> 16;
		lba_io[3]	= (lba & 0xff000000) >> 24;
		lba_io[4]	= 0;
		lba_io[5]	= 0;
		head		= 0;
	} else if (ide_devices[drive].capabilities & 0x200) {
		lba_mode	= 1;
		lba_io[0]	= (lba & 0x00000ff) >> 0;
		lba_io[1]	= (lba & 0x000ff00) >> 8;
		lba_io[2]	= (lba & 0x0ff0000) >> 16;
		lba_io[3]	= 0;
		lba_io[4]	= 0;
		lba_io[5]	= 0;
		head		= (lba & 0xf000000) >> 24;
	} else {
		lba_mode	= 0;
		sect		= (lba % 63) + 1;
		cyl			= (lba + 1 - sect) / (16 * 63);
		lba_io[0]	= sect;
		lba_io[1]	= (cyl >> 0) & 0xff;
		lba_io[2]	= (cyl >> 8) & 0xff;
		lba_io[3]	= 0;
		lba_io[4]	= 0;
		lba_io[5]	= 0;
		head		= (lba + 1 - sect) % (16 * 63) / (63);
	}
	dma = 0;

	while (ide_read(channel, ATA_REG_STATUS) & ATA_SR_BSY);
	if (lba_mode == 0)
		ide_write(channel, ATA_REG_HDDEVSEL, 0xa0 | (slavebit << 4) | head); // Drive & CHS.
	else
		ide_write(channel, ATA_REG_HDDEVSEL, 0xe0 | (slavebit << 4) | head); // Drive & LBA
	if (lba_mode == 2) {
		ide_write(channel, ATA_REG_SECCOUNT1, 0);
		ide_write(channel, ATA_REG_LBA3, lba_io[3]);
		ide_write(channel, ATA_REG_LBA4, lba_io[4]);
		ide_write(channel, ATA_REG_LBA5, lba_io[5]);
	}
	ide_write(channel, ATA_REG_SECCOUNT0, numsects);
	ide_write(channel, ATA_REG_LBA0, lba_io[0]);
	ide_write(channel, ATA_REG_LBA1, lba_io[1]);
	ide_write(channel, ATA_REG_LBA2, lba_io[2]);

	if (lba_mode == 0 && dma == 0 && direction == 0) cmd = ATA_CMD_READ_PIO;
	if (lba_mode == 1 && dma == 0 && direction == 0) cmd = ATA_CMD_READ_PIO;
	if (lba_mode == 2 && dma == 0 && direction == 0) cmd = ATA_CMD_READ_PIO_EXT;
	if (lba_mode == 0 && dma == 1 && direction == 0) cmd = ATA_CMD_READ_DMA;
	if (lba_mode == 1 && dma == 1 && direction == 0) cmd = ATA_CMD_READ_DMA;
	if (lba_mode == 2 && dma == 1 && direction == 0) cmd = ATA_CMD_READ_DMA_EXT;
	if (lba_mode == 0 && dma == 0 && direction == 1) cmd = ATA_CMD_WRITE_PIO;
	if (lba_mode == 1 && dma == 0 && direction == 1) cmd = ATA_CMD_WRITE_PIO;
	if (lba_mode == 2 && dma == 0 && direction == 1) cmd = ATA_CMD_WRITE_PIO_EXT;
	if (lba_mode == 0 && dma == 1 && direction == 1) cmd = ATA_CMD_WRITE_DMA;
	if (lba_mode == 1 && dma == 1 && direction == 1) cmd = ATA_CMD_WRITE_DMA;
	if (lba_mode == 2 && dma == 1 && direction == 1) cmd = ATA_CMD_WRITE_DMA_EXT;
	ide_write(channel, ATA_REG_COMMAND, cmd);

	if (direction == 0) {
		/* PIO读 */
		uint16_t *word_ = (uint16_t *)edi;
		for (i = 0; i < numsects; i++) {
			if ((err = ide_polling(channel, 1)) != 0)
				return err;
			insl(bus, (uint32_t *)(word_ + i * words), words / 2);
		}
	} else {
		/* PIO写 */
		uint16_t *word_ = (uint16_t *)edi;
		for (i = 0; i < numsects; i++) {
			ide_polling(channel, 0);
			for (int h = 0; h < (int)words; h++) {
				outw(bus, word_[i * words + h]);
			}
		}
		ide_write(channel, ATA_REG_COMMAND, (char[]){ATA_CMD_CACHE_FLUSH, ATA_CMD_CACHE_FLUSH,
                  ATA_CMD_CACHE_FLUSH_EXT}[lba_mode]);
		ide_polling(channel, 0);
	}
	return 0;
}

/* 从ATAPI设备读取数据 */
uint8_t ide_atapi_read(uint8_t drive, uint32_t lba, uint8_t numsects, uint32_t edi)
{
	uint32_t channel = ide_devices[drive].channel;
	uint32_t slavebit = ide_devices[drive].drive;
	uint32_t bus = channels[channel].base;
	uint32_t words = 1024;
	uint8_t err;
	int i;

	ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN = ide_irq_invoked = 0x0);

	/* 设置 SCSI 数据包 */
	atapi_packet[0]		= ATAPI_CMD_READ;
	atapi_packet[1]		= 0x0;
	atapi_packet[2]		= (lba >> 24) & 0xff;
	atapi_packet[3]		= (lba >> 16) & 0xff;
	atapi_packet[4]		= (lba >> 8) & 0xff;
	atapi_packet[5]		= (lba >> 0) & 0xff;
	atapi_packet[6]		= 0x0;
	atapi_packet[7]		= 0x0;
	atapi_packet[8]		= 0x0;
	atapi_packet[9]		= numsects;
	atapi_packet[10]	= 0x0;
	atapi_packet[11]	= 0x0;

	ide_write(channel, ATA_REG_HDDEVSEL, slavebit << 4);
	for (int i = 0; i < 4000; i++);

	for (int i = 0; i < 4; i++)
		ide_read(channel, ATA_REG_ALTSTATUS);

	ide_write(channel, ATA_REG_FEATURES, 0); // PIO模式
	ide_write(channel, ATA_REG_LBA1, (words * 2) & 0xff); // 扇区大小的字节较小
	ide_write(channel, ATA_REG_LBA2, (words * 2) >> 8); // 扇区大小的上限字节

	/* 发送packet命令 */
	ide_write(channel, ATA_REG_COMMAND, ATA_CMD_PACKET);
	if ((err = ide_polling(channel, 1)) != 0) return err;

	uint16_t *_atapi_packet = (uint16_t *)atapi_packet;
	for (int i = 0; i < 6; i++) {
		outw(bus, _atapi_packet[i]);
	}

	uint16_t *_word = (uint16_t *)(uintptr_t)edi;
	for (i = 0; i < numsects; i++) {
		ide_wait_irq();
		if ((err = ide_polling(channel, 1)) != 0) return err;
		for (int h = 0; h < (int)words; h++) {
			_word[i * words + h] = inw(bus);
		}
	}
	ide_wait_irq();

	while (ide_read(channel, ATA_REG_STATUS) & (ATA_SR_BSY | ATA_SR_DRQ));
	return 0;
}

/* 从IDE设备读取多个扇区数据 */
void ide_read_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, uint32_t edi)
{
	/* 检查驱动器是否存在 */
	if (drive > 3 || ide_devices[drive].reserved == 0) package[0] = 0x1;

	/* 检查输入是否有效 */
	else if (((lba + numsects) > ide_devices[drive].size) && (ide_devices[drive].type == IDE_ATA))
		package[0] = 0x2;

	/* 通过轮询和IRQ以PIO模式读取 */
	else {
		uint8_t err;
		if (ide_devices[drive].type == IDE_ATA)
			err = ide_ata_access(ATA_READ, drive, lba, numsects, edi);
		else if (ide_devices[drive].type == IDE_ATAPI)
			for (int i = 0; i < numsects; i++)
				err = ide_atapi_read(drive, lba + i, 1, edi + (i * 2048));
		package[0] = ide_print_error(drive, err);
	}
}

/* 向IDE设备写入多个扇区数据 */
void ide_write_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, uint32_t edi)
{
	/* 检查驱动器是否存在 */
	if (drive > 3 || ide_devices[drive].reserved == 0) package[0] = 0x1;

	/* 检查输入是否有效 */
	else if (((lba + numsects) > ide_devices[drive].size) && (ide_devices[drive].type == IDE_ATA))
		package[0] = 0x2;

	/* 通过轮询和IRQ以PIO模式写入 */
	else {
		uint8_t err;
		if (ide_devices[drive].type == IDE_ATA)
			err = ide_ata_access(ATA_WRITE, drive, lba, numsects, edi);
		else if (ide_devices[drive].type == IDE_ATAPI)
			err = 4;
		package[0] = ide_print_error(drive, err);
	}
}
