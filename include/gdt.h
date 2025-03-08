/*
 *
 *		gdt.h
 *		全局描述符头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_GDT_H_
#define INCLUDE_GDT_H_

#include "stdint.h"

struct gdt_register {
	uint16_t size;
	void *ptr;
} __attribute__((packed));

typedef struct tss {
	uint32_t unused0;
	uint64_t rsp[3];
	uint64_t unused1;
	uint64_t ist[7];
	uint64_t unused2;
	uint16_t unused3;
	uint16_t iopb;
} __attribute__((packed)) tss_t;

typedef uint8_t tss_stack_t[1024];
typedef uint64_t gdt_entries_t[7];

/* 初始化全局描述符表 */
void init_gdt(void);

/* 初始化TSS */
void tss_init(void);

/* 设置内核栈 */
void set_kernel_stack(uint64_t rsp);

#endif // INCLUDE_GDT_H_
