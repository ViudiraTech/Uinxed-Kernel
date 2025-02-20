/*
 *
 *		fdc.c
 *		软盘控制器
 *
 *		2025/1/9 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，开放所有权利。
 *
 */

#include "fdc.h"
#include "printk.h"
#include "idt.h"
#include "vdisk.h"
#include "string.h"
#include "common.h"
#include "dma.h"

volatile int floppy_int_count = 0;

static int dchange = 0;
static int motor = 0;
static int mtick = 0;
static byte status[7] = {0};
static byte statsz = 0;
static byte sr0 = 0;
static byte fdc_track = 0xff;
static DrvGeom geometry = {DG144_HEADS, DG144_TRACKS, DG144_SPT};
static unsigned int tbaddr = 0x80000L; // 位于1M以下的轨道缓冲器的物理地址

/* block转hts */
static void block2hts(int block, int *track, int *head, int *sector)
{
	*track = (block / 18) / 2;
	*head = (block / 18) % 2;
	*sector = block % 18 + 1;
}

/* 向软盘控制器发送一个字节 */
static void sendbyte(int byte)
{
	for (int tmo = 0; tmo < 128; tmo++) {
		int msr = inb(FDC_MSR);
		if ((msr & 0xc0) == 0x80) {
			outb(FDC_DATA, byte);
			return;
		}
		inb(0x80);
	}
}

/* 从软盘控制器读取一个字节 */
static int getbyte(void)
{
	for (int tmo = 0; tmo < 128; tmo++) {
		int msr = inb(FDC_MSR);
		if ((msr & 0xd0) == 0xd0) {
			return (byte)inb(FDC_DATA);
		}
		inb(0x80);
	}
	return -1;
}

/* 等待软盘控制器中断 */
static int wait_floppy_interrupt(void)
{
	int timeout = 128;

	enable_intr();
	while (!floppy_int_count && timeout--);

	if (timeout <= 0) return 1;

	statsz = 0; // 清空状态

	while ((statsz < 7) && (inb(FDC_MSR) & (1 << 4))) {
		status[statsz++] = getbyte(); // 获取数据
	}

	/* 获取中断状态 */
	sendbyte(CMD_SENSEI);
	sr0 = getbyte();
	fdc_track = getbyte();

	floppy_int_count = 0;
	return 0;
}

/* 软盘控制器SEEK */
static int fdc_seek(int track)
{
	if (fdc_track == track) {
		return 1;
	}

	/* 向软盘控制器发送SEEK命令 */
	sendbyte(CMD_SEEK);
	sendbyte(0);
	sendbyte(track);

	wait_floppy_interrupt(); // 等待软盘中断

	for (int i = 0; i < 500; i++);
	if ((sr0 != 0x20) || (fdc_track != track))
		return 0;
	else
		return 1;
}

/* 启动软驱电机 */
void motoron(void)
{
	if (!motor) {
		mtick = -1; /* 停止电机计时 */
		outb(FDC_DOR, 0x1c);
		for (int i = 0; i < 80000; i++);
		motor = 1; // 设置电机状态为true
	}
}

/* 停止软驱电机 */
void motoroff(void)
{
	if (motor) mtick = 13500;
}

/* 重新校准驱动器 */
void recalibrate(void)
{
	/* 先启用电机 */
	motoron();

	/* 然后重新校准电机 */
	sendbyte(CMD_RECAL);
	sendbyte(0);

	/* 等待软盘中断 */
	wait_floppy_interrupt();

	/* 关闭电机 */
	motoroff();
}

/* 复位软驱 */
int reset(void)
{
	/* 停止软盘电机并禁用IRQ和DMA传输 */
	outb(FDC_DOR, 0);

	/* 初始化电机计数器 */
	mtick = 0;
	motor = 0;

	/* 数据传输速度 (500K/s) */
	outb(FDC_DRS, 0);

	/* 重新启动软盘中断（让软盘发送iRQ6），这将会调用上面的flint函数 */
	outb(FDC_DOR, 0x0c);

	/* 重置软盘驱动器将会引发一个中断了，我们需要进行处理 */
	if (wait_floppy_interrupt()) return 1;

	/* 指定软盘驱动器定时（不使用在实模式时BIOS设定的操作） */
	sendbyte(CMD_SPECIFY);
	sendbyte(0xdf);
	sendbyte(0x02);

	/* 清除“磁盘更改”状态 */
	recalibrate();

	dchange = 0;
	return 0;
}

/* 软盘控制器读写 */
static int fdc_rw(int block, byte *blockbuff, int read, uint64_t nosectors)
{
	int head, track, sector, tries, copycount = 0;
	byte *p_tbaddr = (byte *)0x80000; // 512byte
	byte *p_blockbuff = blockbuff; // r/w的数据缓冲区

	/* 获取block对应的ths */
	block2hts(block, &track, &head, &sector);

	motoron();

	if (!read && blockbuff) {
		/* 从数据缓冲区复制数据到轨道缓冲区 */
		for (copycount = 0; copycount < (nosectors * 512); copycount++) {
			*p_tbaddr = *p_blockbuff;
			p_blockbuff++;
			p_tbaddr++;
		}
	}

	for (tries = 0; tries < 3; tries++) {
		/* 检查 */
		if (inb(FDC_DIR) & 0x80) {
			dchange = 1;
			fdc_seek(1);
			recalibrate();
			motoroff();
			return fdc_rw(block, blockbuff, read, nosectors);
		}

		/* seek到track的位置*/
		if (!fdc_seek(track)) {
			motoroff();
			return 0;
		}

		/* 传输速度（500K/s） */
		outb(FDC_CCR, 0);

		/* 发送命令 */
		if (read) {
			dma_recv(2, (void *)tbaddr, nosectors * 512);
			sendbyte(CMD_READ);
		} else {
			dma_send(2, (void *)tbaddr, nosectors * 512);
			sendbyte(CMD_WRITE);
		}

		sendbyte(head << 2);
		sendbyte(track);
		sendbyte(head);
		sendbyte(sector);
		sendbyte(2);
		sendbyte(geometry.spt);

		if (geometry.spt == DG144_SPT)
			sendbyte(DG144_GAP3RW);
		else
			sendbyte(DG168_GAP3RW);
		sendbyte(0xff);

		/* 等待中断...... */
		/* 读写数据不需要中断状态 */
		wait_floppy_interrupt();

		if ((status[0] & 0xc0) == 0) break;
		recalibrate();
	}

	/* 关闭电动机 */
	motoroff();

	if (read && blockbuff) {
		/* 复制数据 */
		p_blockbuff = blockbuff;
		p_tbaddr = (byte *)0x80000;

		for (copycount = 0; copycount < (nosectors * 512); copycount++) {
			*p_blockbuff = *p_tbaddr;
			p_blockbuff++;
			p_tbaddr++;
		}
	}
	return (tries != 3);
}

/* 读一个扇区 */
int read_block(int block, byte *blockbuff, uint64_t nosectors)
{
	int track = 0, sector = 0, head = 0, track2 = 0, result = 0, loop = 0;
	block2hts(block, &track, &head, &sector);
	block2hts(block + nosectors, &track2, &head, &sector);

	if (track != track2) {
		for (loop = 0; loop < nosectors; loop++)
			result = fdc_rw(block + loop, blockbuff + (loop * 512), 1, 1);
		return result;
	}
	return fdc_rw(block, blockbuff, 1, nosectors);
}

/* 写一个扇区 */
int write_block(int block, byte *blockbuff, uint64_t nosectors)
{
	return fdc_rw(block, blockbuff, 0, nosectors);
}

/* 软驱中断处理 */
static void flint(pt_regs *regs)
{
	floppy_int_count = 1; // 设置中断计数器为1，代表中断已经发生（或者是系统已经收到了中断）
}

/* 传递给vdisk的读接口 */
static void Read(int drive, byte *buffer, uint number, uint lba)
{
	for (int i = 0; i < number; i += SECTORS_ONCE) {
		int sectors = ((number - i) >= SECTORS_ONCE) ? SECTORS_ONCE : (number - i);
		fdc_rw(lba + i, buffer + i * 512, 1, sectors);
	}
}

/* 传递给vdisk的写接口 */
static void Write(int drive, byte *buffer, uint number, uint lba)
{
	for (int i = 0; i < number; i += SECTORS_ONCE) {
		int sectors = ((number - i) >= SECTORS_ONCE) ? SECTORS_ONCE : (number - i);
		fdc_rw(lba + i, buffer + i * 512, 0, sectors);
	}
}

/* 初始化软盘控制器 */
void floppy_init(void)
{
	print_busy("Initializing floppy disk controller...\r"); // 提示用户正在初始化软盘控制器，并回到行首等待覆盖

	sendbyte(CMD_VERSION); // 发送命令（获取软盘版本），如果收到回应，说明软盘正在工作
	if (getbyte() == -1) {
		print_warn("The floppy disk controller could not be found!\n");
		return;
	}

	register_interrupt_handler(0x26, &flint);

	if (reset()) {
		print_warn("The floppy disk controller interrupt timed out.\n");
		return;
	}

	sendbyte(CMD_VERSION); // 获取软盘版本

	vdisk vd;
	vd.flag = 1;
	vd.Read = Read;
	vd.Write = Write;
	vd.sector_size = 512;
	vd.size = 1474560;
	sprintf(vd.DriveName,"fd0");
	register_vdisk(vd);

	print_succ("Floppy disk controller initialized successfully | ");
	printk("FDC_VER:0x%x\n", getbyte());
}
