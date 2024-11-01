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

/* 初始化串口 */
int init_serial(void)
{
	outb(SERIAL_PORT + 1, 0x00); // 禁止COM的中断发生
	outb(SERIAL_PORT + 3, 0x80); // 启用DLAB（设置波特率除数）。
	outb(SERIAL_PORT + 0, 0x03); // 设置除数为3，(低位) 38400波特
	outb(SERIAL_PORT + 1, 0x00); //            (高位)
	outb(SERIAL_PORT + 3, 0x03); // 8位，无奇偶性，一个停止位
	outb(SERIAL_PORT + 2, 0xC7); // 启用FIFO，有14字节的阈值
	outb(SERIAL_PORT + 4, 0x0B); // 启用IRQ，设置RTS/DSR
	outb(SERIAL_PORT + 4, 0x1E); // 设置为环回模式，测试串口
	outb(SERIAL_PORT + 0, 0xAE); // 测试串口（发送字节0xAE并检查串口是否返回相同的字节）

	/* 检查串口是否有问题（即：与发送的字节不一样） */
	if (inb(SERIAL_PORT + 0) != 0xAE) {
		return 1;
	}

	/* 如果串口没有故障，将其设置为正常运行模式 */
	/* (非环回，启用IRQ，启用OUT#1和OUT#2位) */
	outb(SERIAL_PORT + 4, 0x0F);
	return 0;
}

/* 检测串口读是否就绪 */
int serial_received(void)
{
	return inb(SERIAL_PORT + 5) & 1;
}

/* 读串口 */
char read_serial(void)
{
	while (serial_received() == 0);
	return inb(SERIAL_PORT);
}

/* 检测串口写是否空闲 */
int is_transmit_empty(void)
{
	return inb(SERIAL_PORT + 5) & 0x20;
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
