/*
 *
 *		ide.c
 *		IDE设备驱动
 *
 *		2024/7/11 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "common.h"
#include "printk.h"
#include "debug.h"
#include "ide.h"
#include "pci.h"
#include "vdisk.h"

int no_ide_controller = 0;

/* 传递给vdisk的读接口 */
static void vdisk_ide_read(int drive, uint8_t *buffer, uint32_t number, uint32_t lba)
{
	ide_read_secs(lba, buffer, number);
}

/* 传递给vdisk的写接口 */
static void vdisk_ide_write(int drive, uint8_t *buffer, uint32_t number, uint32_t lba)
{
	ide_write_secs(lba, buffer, number);
}

/* 等待IDE设备可用 */
static int32_t ide_wait_ready(uint16_t iobase, int check_error)
{
	int r = 0;
	while ((r = inb(iobase + ISA_STATUS)) & IDE_BSY) {}
	if (check_error && (r & (IDE_DF | IDE_ERR)) != 0) {
		return -1;
	}
	return 0;
}

/* 初始化IDE设备 */
void init_ide(void)
{
	print_busy("Init IDE Driver ...\r");

	/* 检测计算机是否拥有IDE控制器 */
	if (!pci_find_name("IDE Controller")) {
		no_ide_controller = 1;
		print_warn("The IDE controller could not be found!\n");
		return;
	}
	ide_wait_ready(IOBASE, 0);

	/* 1: 选择要操作的设备
     * 0xE0 代表IDE Primary Master		(IDE 0.0)
     * 0xF0 代表IDE Primary Slave		(IDE 0.1)
     * 0xC0 代表IDE Secondary Master	(IDE 1.0)
     * 0xD0 代表IDE Secondary Slave		(IDE 1.1)
     */
	outb(IOBASE + ISA_SDH, 0xE0);
	ide_wait_ready(IOBASE, 0);

	/* 2: 发送IDE信息获取命令 */
	outb(IOBASE + ISA_COMMAND, IDE_CMD_IDENTIFY);
	ide_wait_ready(IOBASE, 0);

	/* 3: 检查设备是否存在 */
	if (inb(IOBASE + ISA_STATUS) == 0 || ide_wait_ready(IOBASE, 1) != 0) {
		print_warn("Main IDE Device Not Found!\n");
		return;
	}
	ide_device.valid = 1;

	/* 读取IDE设备信息 */
	uint32_t buffer[128];
	insl(IOBASE + ISA_DATA, buffer, sizeof(buffer) / sizeof(uint32_t));

	uint8_t *ident = (uint8_t *)buffer;
	uint32_t cmdsets = *(uint32_t *)(ident + IDE_IDENT_CMDSETS);
	uint32_t sectors;

	/* 检查设备使用48-bits还是28-bits地址 */
	if (cmdsets & (1 << 26)) {
		sectors = *(uint32_t *)(ident + IDE_IDENT_MAX_LBA_EXT);
	} else {
		sectors = *(uint32_t *)(ident + IDE_IDENT_MAX_LBA);
	}
	ide_device.sets = cmdsets;
	ide_device.size = sectors;

	char *desc = ide_device.desc;
	char *data = (char *)((uint32_t)ident + IDE_IDENT_MODEL);

	/* 复制设备描述信息 */
	int i, length = IDE_DESC_LEN;
	for (i = 0; i < length; i += 2) {
		desc[i] = data[i+1];
		desc[i+1] = data[i];
	} do {
		desc[i] = '\0';
	} while (i-- > 0 && desc[i] == ' ');

	/* 注册到vdisk */
	vdisk vd;
	vd.flag = 1;
	vd.Read = vdisk_ide_read;
	vd.Write = vdisk_ide_write;
	vd.sector_size = 0x200; // 512
	vd.size = ide_device.size * vd.sector_size;
	sprintf(vd.DriveName,"hda");
	register_vdisk(vd);

	print_succ("Found IDE Driver: ");
	printk("%s %d(MiB) %u(sectors)\n", ide_get_desc(), (ide_get_size() * 512 / (1024 * 1024)), ide_get_size());
}

/* 获取IDE设备描述 */
char *ide_get_desc(void)
{
	if (ide_device_valid()) {
		return ide_device.desc;
	}
	return 0;
}

/* 获取IDE设备扇区大小 */
int ide_get_size(void)
{
	if (ide_device_valid()) {
		return ide_device.size;
	}
	return 0;
}

/* 检测是否存在IDE控制器 */
int check_ide_controller(void)
{
	if (no_ide_controller) {
		return 0;
	} else {
		return 1;
	}
}

/* 检测IDE设备是否可用 */
int ide_device_valid(void)
{
	return ide_device.valid == 1;
}

/* 读取指定IDE设备若干扇区 */
int ide_read_secs(uint32_t secno, void *dst, uint32_t nsecs)
{
	if (no_ide_controller) {
		printk("The IDE optical drive could not be found.\n");
		return 0;
	} else if (!ide_device.valid) {
		printk("Main IDE Device Not Found!\n");
		return 0;
	} else if (nsecs >= MAX_NSECS) {
		printk("The maximum number of operating sectors of the IDE is exceeded.\n");
		return 0;
	} else if (secno + nsecs >= MAX_DISK_NSECS) {
		printk("The maximum sector number of the IDE is exceeded.\n");
		return 0;
	}
	ide_wait_ready(IOBASE, 0);

	outb(IOCTRL + ISA_CTRL, 0);
	outb(IOBASE + ISA_SECCNT, nsecs);
	outb(IOBASE + ISA_SECTOR, secno & 0xFF);
	outb(IOBASE + ISA_CYL_LO, (secno >> 8) & 0xFF);
	outb(IOBASE + ISA_CYL_HI, (secno >> 16) & 0xFF);
	outb(IOBASE + ISA_SDH, 0xE0 | ((secno >> 24) & 0xF));
	outb(IOBASE + ISA_COMMAND, IDE_CMD_READ);

	int ret = 0;
	for ( ; nsecs > 0; nsecs --, dst += SECTSIZE) {
		if ((ret = ide_wait_ready(IOBASE, 1)) != 0) {
			return ret;
		}
		insl(IOBASE, dst, SECTSIZE / sizeof(uint32_t));
	}
	return ret;
}

/* 写入指定IDE设备若干扇区 */
int ide_write_secs(uint32_t secno, const void *src, uint32_t nsecs)
{
	if (no_ide_controller) {
		printk("The IDE optical drive could not be found.\n");
		return 0;
	} else if (!ide_device.valid) {
		printk("Main IDE Device Not Found!\n");
		return 0;
	} else if (nsecs >= MAX_NSECS) {
		printk("The maximum number of operating sectors of the IDE is exceeded.\n");
		return 0;
	} else if (secno + nsecs >= MAX_DISK_NSECS) {
		printk("The maximum sector number of the IDE is exceeded.\n");
		return 0;
	}
	ide_wait_ready(IOBASE, 0);

	outb(IOCTRL + ISA_CTRL, 0);
	outb(IOBASE + ISA_SECCNT, nsecs);
	outb(IOBASE + ISA_SECTOR, secno & 0xFF);
	outb(IOBASE + ISA_CYL_LO, (secno >> 8) & 0xFF);
	outb(IOBASE + ISA_CYL_HI, (secno >> 16) & 0xFF);
	outb(IOBASE + ISA_SDH, 0xE0 | ((secno >> 24) & 0xF));
	outb(IOBASE + ISA_COMMAND, IDE_CMD_WRITE);

	int ret = 0;
	for ( ; nsecs > 0; nsecs --, src += SECTSIZE) {
		if ((ret = ide_wait_ready(IOBASE, 1)) != 0) {
			return ret;
		}
		outsl(IOBASE, src, SECTSIZE / sizeof(uint32_t));
	}
	return ret;
}
