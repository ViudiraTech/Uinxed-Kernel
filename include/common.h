/*
 *
 *		common.h
 *		通用设备IO驱动头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#ifndef INCLUDE_COMMON_H_
#define INCLUDE_COMMON_H_

#include "types.h"
#include "string.h"

void		outb(uint16_t port, uint8_t value);		// 端口写（8位）
void		outw(uint16_t port, uint16_t value);	// 端口写（16位）
void		outl(uint16_t port, uint32_t value);	// 端口写（32位）

uint8_t		inb(uint16_t port);						// 端口读（8位）
uint16_t	inw(uint16_t port);						// 端口读（16位）
uint32_t	inl(uint16_t port);						// 端口读（32位）

void enable_intr();									// 开启中断
void disable_intr();								// 关闭中断

#endif // INCLUDE_COMMON_H_
