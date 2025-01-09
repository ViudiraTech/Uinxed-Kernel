/*
 *
 *		serial.c
 *		计算机串口驱动
 *
 *		2024/7/11 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "serial.h"
#include "common.h"
#include "printk.h"
#include "vdisk.h"

/* 传递给vdisk的读接口 */
static void vdisk_ttyS0_read(int drive, uint8_t *buffer, uint32_t number, uint32_t lba)
{
	*buffer = read_serial();
}

/* 传递给vdisk的写接口 */
static void vdisk_ttyS0_write(int drive, uint8_t *buffer, uint32_t number, uint32_t lba)
{
	write_serial_string((const char *)buffer);
}

/* 初始化串口 */
void init_serial(int baud_rate)
{
	print_busy("Initializing RS-232 controller...\r"); // 提示用户正在初始化RS-232，并回到行首等待覆盖
	uint16_t divisor = 115200 / baud_rate;

	outb(SERIAL_PORT + 1, 0x00);					// 禁止COM的中断发生
	outb(SERIAL_PORT + 3, 0x80);					// 启用DLAB（设置波特率除数）。
	outb(SERIAL_PORT + 0, divisor & 0xFF);			// 设置低位波特
	outb(SERIAL_PORT + 1, (divisor >> 8) & 0xFF);	// 设置高位波特
	outb(SERIAL_PORT + 3, 0x03);					// 8位，无奇偶性，一个停止位
	outb(SERIAL_PORT + 2, 0xC7);					// 启用FIFO，有14字节的阈值
	outb(SERIAL_PORT + 4, 0x0B);					// 启用IRQ，设置RTS/DSR
	outb(SERIAL_PORT + 4, 0x1E);					// 设置为环回模式，测试串口
	outb(SERIAL_PORT + 0, 0xAE);					// 测试串口（发送字节0xAE并检查串口是否返回相同的字节）

	/* 检查串口是否有问题（即：与发送的字节不一样） */
	if (inb(SERIAL_PORT + 0) != 0xAE) {
		print_warn("RS-232 controller data is abnormal.\n");
		return;
	}

	/* 如果串口没有故障，将其设置为正常运行模式 */
	/* (非环回，启用IRQ，启用OUT#1和OUT#2位) */
	outb(SERIAL_PORT + 4, 0x0F);

	/* 注册到vdisk */
	vdisk vd;
	vd.flag = 1;
	vd.Read = vdisk_ttyS0_read;
	vd.Write = vdisk_ttyS0_write;
	vd.sector_size = 1;
	vd.size = 1;
	sprintf(vd.DriveName,"ttyS0");
	register_vdisk(vd);

	print_succ("The RS-232 controller initialized successfully | Port: COM1 |");
	printk(" Baud rate: %d\n", baud_rate);
}

/* 检测串口读是否就绪 */
int serial_received(void)
{
	return inb(SERIAL_PORT + 5) & 1;
}

/* 检测串口写是否空闲 */
int is_transmit_empty(void)
{
	return inb(SERIAL_PORT + 5) & 0x20;
}

/* 读串口 */
char read_serial(void)
{
	while (serial_received() == 0);
	return inb(SERIAL_PORT);
}

/* 写串口 */
void write_serial(char a)
{
	while (is_transmit_empty() == 0);
	outb(SERIAL_PORT, a);
}

/* 写串口字符串 */
void write_serial_string(const char *str)
{
	while (*str)
	{
		while (is_transmit_empty() == 0);
		outb(SERIAL_PORT, *str);
		str++;
	}
}
