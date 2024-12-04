/*
 *
 *		atapi.c
 *		IDE光盘驱动器驱动
 *
 *		2024/9/8 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "atapi.h"
#include "ide.h"

/* 这段代码会延迟400纳秒 */
static void ata_io_wait(const uint8_t p)
{
	inb(p + CONTROL + ALTERNATE_STATUS);
	inb(p + CONTROL + ALTERNATE_STATUS);
	inb(p + CONTROL + ALTERNATE_STATUS);
	inb(p + CONTROL + ALTERNATE_STATUS);
}

/* 从LBA到指针来读光盘扇区 */
int read_cdrom(uint16_t port, int slave, uint32_t lba, uint32_t sectors, uint16_t *buffer)
{
	if (!check_ide_controller()) {
		printk("The IDE optical drive could not be found.\n");
		return 1;
	}
	volatile uint8_t read_cmd[12] = {0xA8, 0,
                                    (lba >> 0x18) & 0xFF, (lba >> 0x10) & 0xFF, (lba >> 0x08) & 0xFF,
                                    (lba >> 0x00) & 0xFF,
                                    (sectors >> 0x18) & 0xFF, (sectors >> 0x10) & 0xFF, (sectors >> 0x08) & 0xFF,
                                    (sectors >> 0x00) & 0xFF,
                                    0, 0};

	outb(port + DRIVE_SELECT, 0xA0 & (slave << 4)); // 选择驱动器
	ata_io_wait(port);
	outb(port + ERROR_R, 0x00); 
	outb(port + LBA_MID, 2048 & 0xFF);
	outb(port + LBA_HIGH, 2048 >> 8);
	outb(port + COMMAND_REGISTER, 0xA0); // 报文指令
	ata_io_wait(port); // 我认为我们需要延迟一下，当然这也不咋确定，还是写一下比较好

	/* 等待状态 */
	while (1) {
		uint8_t status = inb(port + COMMAND_REGISTER);
		if ((status & 0x01) == 1)
			return 1;
		if (!(status & 0x80) && (status & 0x08))
			break;
		ata_io_wait(port);
	}

	/* 发送指令 */
	outsw(port + DATA, (uint16_t *) read_cmd, 6);

	/* 读文本 */
	for (uint32_t i = 0; i < sectors; i++) {
		/* 一直等到它准备好为止 */
		while (1) {
			uint8_t status = inb(port + COMMAND_REGISTER);
			if (status & 0x01)
				return 1;
			if (!(status & 0x80) && (status & 0x08))
				break;
		}
		int size = inb(port + LBA_HIGH) << 8 | inb(port + LBA_MID);
		insw(port + DATA, (uint16_t *) ((uint8_t *) buffer + i * 0x800), size / 2);
	}
	return 0;
}
