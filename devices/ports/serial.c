/*
 *
 *		serial.c
 *		计算机串口驱动
 *
 *		2024/7/11 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "serial.h"
#include "common.h"
#include "printk.h"

/* 初始化串口 */
void init_serial(int baud_rate)
{
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
		plogk("Serial: Test failed.\n");
		return;
	}

	/* 如果串口没有故障 将其设置为正常运行模式 */
	/* （非环回，启用IRQ，启用OUT#1和OUT#2位） */
	outb(SERIAL_PORT + 4, 0x0F);
	plogk("Serial: Local port: COM1\n");
	plogk("Serial: Baud rate: %d\n", baud_rate);
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
void write_serial(char c)
{
	while (is_transmit_empty() == 0);
	outb(SERIAL_PORT, c);
}
