/*
 *
 *		hhdm.h
 *		高半区内存映射头文件
 *
 *		2025/2/16 By XIAOYI12
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_HHDM_H_
#define INCLUDE_HHDM_H_

#include "stdint.h"

extern uint64_t physical_memory_offset;

/* 初始化高半区内存映射 */
void init_hhdm(void);

/* 获取物理内存偏移量 */
uint64_t get_physical_memory_offset(void);

/* 物理内存转虚拟内存 */
void *phys_to_virt(uint64_t phys_addr);

/* 虚拟内存转物理内存 */
void *virt_to_phys(uint64_t virt_addr);

#endif // INCLUDE_HHDM_H_
