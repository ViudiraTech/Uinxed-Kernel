/*
 *
 *		parallel.c
 *		并行端口驱动程序
 *
 *		2024/9/8 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "timer.h"
#include "common.h"
#include "parallel.h"
#include "vdisk.h"
#include "printk.h"

/* 传递给vdisk的写接口 */
static void vdisk_lp0_write(int drive, uint8_t *buffer, uint32_t number, uint32_t lba)
{
	parallel_write_string((const char *)buffer);
}

/* 初始化并行端口设备 */
void init_parallel(void)
{
	print_busy("Initializing Parallel LPT device...\r"); // 提示用户正在初始化并口LPT设备，并回到行首等待覆盖

	/* 注册到vdisk */
	vdisk vd;
	vd.flag = 1;
	vd.Read = 0;
	vd.Write = vdisk_lp0_write;
	vd.sector_size = 1;
	vd.size = 1;
	sprintf(vd.DriveName,"lp0");
	register_vdisk(vd);

	print_succ("Parallel LPT device initialized successfully.\n");
}

/* 等待并行端口准备好 */
void wait_parallel_ready(void)
{
	while ((!inb(LPT1_PORT_STATUS)) & 0x80) {
		sleep(10);
	}
}

/* 写并行端口 */
void parallel_write(unsigned char c)
{
	unsigned char lControl;
	wait_parallel_ready();

	outb(LPT1_PORT_BASE, c);
	lControl = inb(LPT1_PORT_CONTROL);
	outb(LPT1_PORT_CONTROL, lControl | 1);
	sleep(10);
	outb(LPT1_PORT_CONTROL, lControl);

	wait_parallel_ready();
}

/* 写并行端口字符串 */
void parallel_write_string(const char *str)
{
	while (*str) {
		unsigned char lControl;
		wait_parallel_ready();

		outb(LPT1_PORT_BASE, *str);
		lControl = inb(LPT1_PORT_CONTROL);
		outb(LPT1_PORT_CONTROL, lControl | 1);
		sleep(10);
		outb(LPT1_PORT_CONTROL, lControl);

		wait_parallel_ready();
		str++;
	}
}
