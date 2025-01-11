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
#include "pci.h"
#include "printk.h"
#include "memory.h"
#include "idt.h"
#include "vdisk.h"
#include "sched.h"

static uint8_t atapi_packet[12] = {0xA8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static int package[2];
uint8_t ide_buf[2048] = {0};
int drive_mapping[26] = {0};

static struct ilist_node ide_proc_list;

volatile static uint8_t ide_irq_invoked = 0;
static void ide_initialize(uint32_t BAR0, uint32_t BAR1, uint32_t BAR2, uint32_t BAR3, uint32_t BAR4);

struct IDEChannelRegisters {
	uint16_t base;	// I/O Base.
	uint16_t ctrl;	// Control Base
	uint16_t bmide;	// Bus Master IDE
	uint8_t nIEN;	// nIEN (No Interrupt);
} channels[2];

struct ide_device {
	uint8_t Reserved;		// 0 (Empty) or 1 (This Drive really exists).
	uint8_t Channel;		// 0 (Primary Channel) or 1 (Secondary Channel).
	uint8_t Drive;			// 0 (Master Drive) or 1 (Slave Drive).
	uint16_t Type;			// 0: ATA | 1:ATAPI
	uint16_t Signature;		// Drive Signature
	uint16_t Capabilities;	// Features.
	uint32_t CommandSets;	// Command Sets Supported.
	uint32_t Size;			// Size in Sectors.
	uint8_t Model[41];		// Model in string.
} ide_devices[4];

/* 提供给vdisk的读接口 */
static void Read(int drive, uint8_t *buffer, uint32_t number, uint32_t lba)
{
	ide_read_sectors(drive_mapping[drive], number, lba, 1 * 8, (uint32_t)buffer);
}

/* 提供给vdisk的写接口 */
static void Write(int drive, uint8_t *buffer, uint32_t number, uint32_t lba)
{
	ide_write_sectors(drive_mapping[drive], number, lba, 1 * 8, (uint32_t)buffer);
}

/* 初始化IDE驱动 */
void init_ide(void)
{
	print_busy("Init IDE/ATAPI Driver...\r");
	ilist_init(&ide_proc_list);

	/* 检测计算机是否拥有IDE控制器 */
	if (!pci_find_name("IDE Controller")) {
		print_warn("The IDE controller could not be found!\n");
		return;
	}
	ide_initialize(0x1F0, 0x3F6, 0x170, 0x376, 0x000);
	if (ide_devices[0].Reserved == 0 && ide_devices[1].Reserved == 0) {
		if (ide_devices[2].Reserved == 0 && ide_devices[3].Reserved == 0) {
			print_warn("Main IDE Device Not Found!\n");
		}
	}
}

/* 等待IDE中断被触发 */
static void ide_wait_irq(void)
{
	while (!ide_irq_invoked) {
		ilist_insert_before(&ide_proc_list, &(current->wait_list));
		yield();
	}
	ide_irq_invoked = 0;
}

/* IDE中断处理函数 */
static void ide_irq(struct interrupt_frame *frame)
{
	ide_irq_invoked = 1;
	queue_task_list(&ide_proc_list);
	queue_task(current);
	yield();
}

/* 设置IDE */
static void ide_initialize(uint32_t BAR0, uint32_t BAR1, uint32_t BAR2, uint32_t BAR3, uint32_t BAR4)
{
	int j, k, count = 0, info = 0;
	uint8_t detected[4] = {0};  // 标记每个设备是否已经被检测过
	register_interrupt_handler(0x2f, &ide_irq);
	register_interrupt_handler(0x2e, &ide_irq);

	for (int i = 0; i < 4; i++) {
		ide_devices[i].Reserved = 0;
	}
	channels[ATA_PRIMARY].base = (BAR0 & 0xFFFFFFFC) + 0x1F0 * (!BAR0);
	channels[ATA_PRIMARY].ctrl = (BAR1 & 0xFFFFFFFC) + 0x3F6 * (!BAR1);
	channels[ATA_SECONDARY].base = (BAR2 & 0xFFFFFFFC) + 0x170 * (!BAR2);
	channels[ATA_SECONDARY].ctrl = (BAR3 & 0xFFFFFFFC) + 0x376 * (!BAR3);
	channels[ATA_PRIMARY].bmide = (BAR4 & 0xFFFFFFFC) + 0;
	channels[ATA_SECONDARY].bmide = (BAR4 & 0xFFFFFFFC) + 8;
	ide_write(ATA_PRIMARY, ATA_REG_CONTROL, 2);
	ide_write(ATA_SECONDARY, ATA_REG_CONTROL, 2);

	for (int i = 0; i < 2; i++) {
		for (j = 0; j < 2; j++) {
			uint8_t err = 0, type = IDE_ATA, status;
			int device_index = i * 2 + j;  // 计算设备索引
			ide_devices[device_index].Reserved = 0;
			ide_write(i, ATA_REG_HDDEVSEL, 0xA0 | (j << 4));
			ide_write(i, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

			if (ide_read(i, ATA_REG_STATUS) == 0) continue;
			while (1) {
				status = ide_read(i, ATA_REG_STATUS);
				if ((status & ATA_SR_ERR)) {
					err = 1;
					break;
				}
				if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) break;
			}
			if (err != 0) {
				uint8_t cl = ide_read(i, ATA_REG_LBA1);
				uint8_t ch = ide_read(i, ATA_REG_LBA2);
				if (cl == 0x14 && ch == 0xEB)
					type = IDE_ATAPI;
				else if (cl == 0x69 && ch == 0x96)
					type = IDE_ATAPI;
				else
					continue;
				ide_write(i, ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
			}
			ide_read_buffer(i, ATA_REG_DATA, (uint32_t)ide_buf, 128);
			ide_devices[device_index].Reserved = 1;
			ide_devices[device_index].Type = type;
			ide_devices[device_index].Channel = i;
			ide_devices[device_index].Drive = j;
			ide_devices[device_index].Signature = *((uint16_t *)(ide_buf + ATA_IDENT_DEVICETYPE));
			ide_devices[device_index].Capabilities = *((uint16_t *)(ide_buf + ATA_IDENT_CAPABILITIES));
			ide_devices[device_index].CommandSets = *((uint32_t *)(ide_buf + ATA_IDENT_COMMANDSETS));

			if (ide_devices[device_index].CommandSets & (1 << 26))
				ide_devices[device_index].Size = *((uint32_t *)(ide_buf + ATA_IDENT_MAX_LBA_EXT));
			else
				ide_devices[device_index].Size = *((uint32_t *)(ide_buf + ATA_IDENT_MAX_LBA));
			for (k = 0; k < 40; k += 2) {
				ide_devices[device_index].Model[k] = ide_buf[ATA_IDENT_MODEL + k + 1];
				ide_devices[device_index].Model[k + 1] = ide_buf[ATA_IDENT_MODEL + k];
			}
			ide_devices[device_index].Model[40] = 0;
			detected[device_index] = 1;  // 标记设备已检测
			count++;
		}
	}

	/* 注册到vdisk */
	vdisk vd;
	int hd_count = 0;
	int cdrom_count = 0;
	for (int i = 0; i < 4; i++) {
		if (ide_devices[i].Reserved == 1 && detected[i]) {
			if (!info) {
				print_succ("Found IDE Driver: ");
				if (!info) {
					printk("Type: %s | %d(MiB) - %s\n", ide_devices[i].Type ? "ATAPI" :
                                                        "ATA", ide_devices[i].Size / 1024 / 2, ide_devices[i].Model);
				}
				info = 1;
			} else {
				printk("                         Type: %s | %d(MiB) - %s\n", ide_devices[i].Type ? "ATAPI" :
                                                                             "ATA", ide_devices[i].Size / 1024 / 2, ide_devices[i].Model);
			}
			if (ide_devices[i].Type == IDE_ATA) {
				sprintf(vd.DriveName, "hd%c", 'a' + hd_count);
				vd.flag = 1;
				hd_count++;
			} else if (ide_devices[i].Type == IDE_ATAPI) {
				sprintf(vd.DriveName, "cdrom%d", cdrom_count);
				vd.flag = 2;
				cdrom_count++;
			}
			vd.Read = Read;
			vd.Write = Write;
			vd.size = ide_devices[i].Size;
			vd.sector_size = vd.flag == 2 ? 2048 : 512;
			int c = register_vdisk(vd);
			drive_mapping[c] = i;
		}
	}
}

/* 错误处理 */
static uint8_t ide_error(uint32_t drive, uint8_t err)
{
	if (err == 0) return err;
	if (err == 1) {
		err = 19;
	} else if (err == 2) {
		uint8_t st = ide_read(ide_devices[drive].Channel, ATA_REG_ERROR);
		if (st & ATA_ER_AMNF) {
			err = 7;
		}
		if (st & ATA_ER_TK0NF) {
			err = 3;
		}
		if (st & ATA_ER_ABRT) {
			err = 20;
			}
		if (st & ATA_ER_MCR) {
			err = 3;
		}
		if (st & ATA_ER_IDNF) {
			err = 21;
		}
		if (st & ATA_ER_MC) {
			err = 3;
		}
		if (st & ATA_ER_UNC) {
			err = 22;
		}
		if (st & ATA_ER_BBK) {
			err = 13;
		}
	} else if (err == 3) {
		err = 23;
	} else if (err == 4) {
		err = 8;
	}
	return err;
}

/* 从IDE设备的指定寄存器读取一个字节数据 */
uint8_t ide_read(uint8_t channel, uint8_t reg)
{
	uint8_t result = 0;
	if (reg > 0x07 && reg < 0x0C)
		ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN);
	if (reg < 0x08)
		result = inb(channels[channel].base + reg - 0x00);
	else if (reg < 0x0C)
		result = inb(channels[channel].base + reg - 0x06);
	else if (reg < 0x0E)
		result = inb(channels[channel].ctrl + reg - 0x0A);
	else if (reg < 0x16)
		result = inb(channels[channel].bmide + reg - 0x0E);
	if (reg > 0x07 && reg < 0x0C)
		ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN);
	return result;
}

/* 向IDE设备的指定寄存器写入一个字节数据 */
void ide_write(uint8_t channel, uint8_t reg, uint8_t data)
{
	if (reg > 0x07 && reg < 0x0C)
		ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN);
	if (reg < 0x08)
		outb(channels[channel].base + reg - 0x00, data);
	else if (reg < 0x0C)
		outb(channels[channel].base + reg - 0x06, data);
	else if (reg < 0x0E)
		outb(channels[channel].ctrl + reg - 0x0A, data);
	else if (reg < 0x16)
		outb(channels[channel].bmide + reg - 0x0E, data);
	if (reg > 0x07 && reg < 0x0C)
		ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN);
}

/* 从IDE设备的指定寄存器读取多个字的数据到缓冲区 */
void ide_read_buffer(uint8_t channel, uint8_t reg, uint32_t buffer, uint32_t quads)
{
	if (reg > 0x07 && reg < 0x0C)
		ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN);
	if (reg < 0x08)
		insl(channels[channel].base + reg - 0x00, (uint32_t *)buffer, quads);
	else if (reg < 0x0C)
		insl(channels[channel].base + reg - 0x06, (uint32_t *)buffer, quads);
	else if (reg < 0x0E)
		insl(channels[channel].ctrl + reg - 0x0A, (uint32_t *)buffer, quads);
	else if (reg < 0x16)
		insl(channels[channel].bmide + reg - 0x0E, (uint32_t *)buffer, quads);
	if (reg > 0x07 && reg < 0x0C)
		ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN);
}

/* 轮询IDE设备的状态 */
uint8_t ide_polling(uint8_t channel, uint32_t advanced_check)
{
	for (int i = 0; i < 4; i++) ide_read(channel, ATA_REG_ALTSTATUS);
	int a = ide_read(channel, ATA_REG_STATUS);

	while (a & ATA_SR_BSY) {
		a = ide_read(channel, ATA_REG_STATUS);
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
uint8_t ide_ata_access(uint8_t direction, uint8_t drive, uint32_t lba, uint8_t numsects, uint16_t selector, uint32_t edi)
{
	uint8_t lba_mode, dma, cmd;
	uint8_t lba_io[6];
	uint32_t channel = ide_devices[drive].Channel;
	uint32_t slavebit = ide_devices[drive].Drive;
	uint32_t bus = channels[channel].base;
	uint32_t words = 256;
	uint16_t cyl, i;
	uint8_t head, sect, err;
	ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN = (ide_irq_invoked = 0x0) + 0x02);

	if (lba >= 0x10000000) {
		lba_mode	= 2;
		lba_io[0]	= (lba & 0x000000FF) >> 0;
		lba_io[1]	= (lba & 0x0000FF00) >> 8;
		lba_io[2]	= (lba & 0x00FF0000) >> 16;
		lba_io[3]	= (lba & 0xFF000000) >> 24;
		lba_io[4]	= 0;
		lba_io[5]	= 0;
		head		= 0;
	} else if (ide_devices[drive].Capabilities & 0x200) {
		lba_mode	= 1;
		lba_io[0]	= (lba & 0x00000FF) >> 0;
		lba_io[1]	= (lba & 0x000FF00) >> 8;
		lba_io[2]	= (lba & 0x0FF0000) >> 16;
		lba_io[3]	= 0;
		lba_io[4]	= 0;
		lba_io[5]	= 0;
		head		= (lba & 0xF000000) >> 24;
	} else {
		lba_mode	= 0;
		sect		= (lba % 63) + 1;
		cyl			= (lba + 1 - sect) / (16 * 63);
		lba_io[0]	= sect;
		lba_io[1]	= (cyl >> 0) & 0xFF;
		lba_io[2]	= (cyl >> 8) & 0xFF;
		lba_io[3]	= 0;
		lba_io[4]	= 0;
		lba_io[5]	= 0;
		head		= (lba + 1 - sect) % (16 * 63) / (63);
	}
	dma = 0;
	while (ide_read(channel, ATA_REG_STATUS) & ATA_SR_BSY) {}

	if (lba_mode == 0)
		ide_write(channel, ATA_REG_HDDEVSEL, 0xA0 | (slavebit << 4) | head);
	else
		ide_write(channel, ATA_REG_HDDEVSEL, 0xE0 | (slavebit << 4) | head);

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
		uint16_t *word_ = (uint16_t *)edi;
		for (i = 0; i < numsects; i++) {
			if ((err = ide_polling(channel, 1)) != 0)
				return err;
			insl(bus, (uint32_t *)(word_ + i * words), words / 2);
		}
	} else {
		uint16_t *word_ = (uint16_t *)edi;
		for (i = 0; i < numsects; i++) {
			ide_polling(channel, 0);
			for (int h = 0; h < words; h++) {
				outw(bus, word_[i * words + h]);
			}
		}
		ide_write(channel, ATA_REG_COMMAND, (char[]){ATA_CMD_CACHE_FLUSH, ATA_CMD_CACHE_FLUSH, ATA_CMD_CACHE_FLUSH_EXT}[lba_mode]);
		ide_polling(channel, 0);
	}
	return 0;
}

/* 从ATAPI设备读取数据 */
uint8_t ide_atapi_read(uint8_t drive, uint32_t lba, uint8_t numsects, uint16_t selector, uint32_t edi)
{
	uint32_t channel = ide_devices[drive].Channel;
	uint32_t slavebit = ide_devices[drive].Drive;
	uint32_t bus = channels[channel].base;
	uint32_t words = 1024;
	uint8_t err;
	int i;

	ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN = ide_irq_invoked = 0x0);
	atapi_packet[0]		= ATAPI_CMD_READ;
	atapi_packet[1]		= 0x0;
	atapi_packet[2]		= (lba >> 24) & 0xFF;
	atapi_packet[3]		= (lba >> 16) & 0xFF;
	atapi_packet[4]		= (lba >> 8) & 0xFF;
	atapi_packet[5]		= (lba >> 0) & 0xFF;
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

	ide_write(channel, ATA_REG_FEATURES, 0);
	ide_write(channel, ATA_REG_LBA1, (words * 2) & 0xFF);
	ide_write(channel, ATA_REG_LBA2, (words * 2) >> 8);
	ide_write(channel, ATA_REG_COMMAND, ATA_CMD_PACKET);

	if ((err = ide_polling(channel, 1)) != 0) return err;

	uint16_t *_atapi_packet = (uint16_t *)atapi_packet;
	for (int i = 0; i < 6; i++) {
		outw(bus, _atapi_packet[i]);
	}

	uint16_t *_word = (uint16_t *)edi;
	for (i = 0; i < numsects; i++) {
		ide_wait_irq();
		if ((err = ide_polling(channel, 1)) != 0) return err;
		for (int h = 0; h < words; h++) {
			uint16_t a = inw(bus);
			_word[i * words + h] = a;
		}
	}
	ide_wait_irq();
	while(ide_read(channel, ATA_REG_STATUS) & (ATA_SR_BSY | ATA_SR_DRQ));
	return 0;
}

/* 从IDE设备读取多个扇区数据 */
void ide_read_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, uint16_t es, uint32_t edi)
{
	if (drive > 3 || ide_devices[drive].Reserved == 0)
		package[0] = 0x1;
	else if (((lba + numsects) > ide_devices[drive].Size) && (ide_devices[drive].Type == IDE_ATA))
		package[0] = 0x2;
    else {
		static uint8_t err;
		if (ide_devices[drive].Type == IDE_ATA) {
			err = ide_ata_access(ATA_READ, drive, lba, numsects, es, edi);
		} else if (ide_devices[drive].Type == IDE_ATAPI)
			for (int i = 0; i < numsects; i++)
				err = ide_atapi_read(drive, lba + i, 1, es, edi + (i * 2048));
		package[0] = ide_error(drive, err);
    }
}

/* 向IDE设备写入多个扇区数据 */
void ide_write_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, uint16_t es, uint32_t edi)
{
	if (drive > 3 || ide_devices[drive].Reserved == 0)
		package[0] = 0x1;
	else if (((lba + numsects) > ide_devices[drive].Size) && (ide_devices[drive].Type == IDE_ATA))
		package[0] = 0x2;
    else {
		static uint8_t err;
		if (ide_devices[drive].Type == IDE_ATA)
			err = ide_ata_access(ATA_WRITE, drive, lba, numsects, es, edi);
		else if (ide_devices[drive].Type == IDE_ATAPI)
			err = 4;
		package[0] = ide_error(drive, err);
	}
}

/* 弹出ATAPI设备的托盘 */
void ide_atapi_eject(uint8_t drive)
{
	uint32_t channel = ide_devices[drive].Channel;
	uint32_t slavebit = ide_devices[drive].Drive;
	uint32_t bus = channels[channel].base;
	static uint8_t err = 0;
	ide_irq_invoked = 0;

	if (drive > 3 || ide_devices[drive].Reserved == 0)
		package[0] = 0x1;
	else if (ide_devices[drive].Type == IDE_ATA)
		package[0] = 20;
	else {
		ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN = ide_irq_invoked = 0x0);
		atapi_packet[0]		= ATAPI_CMD_EJECT;
		atapi_packet[1]		= 0x00;
		atapi_packet[2]		= 0x00;
		atapi_packet[3]		= 0x00;
		atapi_packet[4]		= 0x02;
		atapi_packet[5]		= 0x00;
		atapi_packet[6]		= 0x00;
		atapi_packet[7]		= 0x00;
		atapi_packet[8]		= 0x00;
		atapi_packet[9]		= 0x00;
		atapi_packet[10]	= 0x00;
		atapi_packet[11]	= 0x00;

		ide_write(channel, ATA_REG_HDDEVSEL, slavebit << 4);
		for (int i = 0; i < 4; i++) ide_read(channel, ATA_REG_ALTSTATUS);
		ide_write(channel, ATA_REG_COMMAND, ATA_CMD_PACKET);

		__asm__("rep   outsw" ::"c"(6), "d"(bus), "S"(atapi_packet));
		ide_wait_irq();
		err = ide_polling(channel, 1);
		if (err == 3) err = 0;

		package[0] = ide_error(drive, err);
	}
}
