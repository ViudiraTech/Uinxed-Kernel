/*
 *
 *		parallel.h
 *		并行端口驱动程序头文件
 *
 *		2024/9/8 By MicroFish
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#ifndef INCLUDE_PARALLEL_H_
#define INCLUDE_PARALLEL_H_

#define LPT1_PORT_BASE		0x378				// 并行接口基地址
#define LPT1_PORT_DATA		LPT1_PORT_BASE		// 并行数据接口
#define LPT1_PORT_STATUS	LPT1_PORT_BASE + 1	// 并行状态接口
#define LPT1_PORT_CONTROL	LPT1_PORT_BASE + 2	// 并行控制接口

void parallel_write(unsigned char c);

#endif // INCLUDE_PARALLEL_H_
