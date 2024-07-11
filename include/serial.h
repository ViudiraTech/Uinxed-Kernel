/*
 *
 *		serial.h
 *		计算机串口驱动头文件
 *
 *		2024/7/11 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#ifndef INCLUDE_SERIAL_H_
#define INCLUDE_SERIAL_H_

#define SERIAL_PORT 0x3f8		// 默认使用COM1串口

int init_serial(void);			// 初始化串口
int serial_received(void);		// 检测串口读是否就绪
char read_serial(void);			// 读串口
int is_transmit_empty(void);	// 检测串口写是否空闲
void write_serial(char a);		// 写串口

#endif // INCLUDE_SERIAL_H_
