/*
 *
 *		gdt.h
 *		设置全局描述符程序头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#ifndef INCLUDE_GDT_H_
#define INCLUDE_GDT_H_

#include "types.h"

/* 全局描述符类型 */
typedef struct gdt_entry_t {
	uint16_t limit_low;     // 段界限			15 ～ 0
	uint16_t base_low;      // 段基地址			15 ～ 0
	uint8_t  base_middle;   // 段基地址			23 ～ 16
	uint8_t  access;        // 段存在位、描述符特权级、描述符类型、描述符子类别
	uint8_t  granularity;   // 其他标志、段界限		19 ～ 16
	uint8_t  base_high;     // 段基地址			31 ～ 24
} __attribute__((packed)) gdt_entry_t;

/* GDTR */
typedef struct gdt_ptr_t {
	uint16_t limit;			// 全局描述符表限长
	uint32_t base; 			// 全局描述符表 32 位基地址
} __attribute__((packed)) gdt_ptr_t;

typedef struct tss_table {
	uint32_t prev_tss;
	uint32_t esp0;
	uint32_t ss0;
	uint32_t esp1;
	uint32_t ss1;
	uint32_t esp2;
	uint32_t ss2;
	uint32_t cr3;
	uint32_t eip;
	uint32_t eflags;
	uint32_t eax;
	uint32_t ecx;
	uint32_t edx;
	uint32_t ebx;
	uint32_t esp;
	uint32_t ebp;
	uint32_t esi;
	uint32_t edi;
	uint32_t es;
	uint32_t cs;
	uint32_t ss;
	uint32_t ds;
	uint32_t fs;
	uint32_t gs;
	uint32_t ldt;
	uint16_t trap;
	uint16_t iomap_base;
} tss_entry;

typedef struct intr_frame_t {
	unsigned edi;
	unsigned esi;
	unsigned ebp;
	unsigned esp_dummy;

	unsigned ebx;
	unsigned edx;
	unsigned ecx;
	unsigned eax;

	unsigned gs;
	unsigned fs;
	unsigned es;
	unsigned ds;

	unsigned eip;
	unsigned cs;
	unsigned eflags;
	unsigned esp;
	unsigned ss;
} intr_frame_t;

/* 初始化全局描述符表 */
void init_gdt(void);

/* 设置任务状态段并将其写入全局描述符表 */
void write_tss(int32_t num, uint16_t ss0, uint32_t esp0);

/* 设置任务状态段 */
void set_kernel_stack(uintptr_t stack);

/* 设置任务状态段中的ss0字段 */
void set_tss_ss0(uintptr_t ss);

/* GDT 加载到 GDTR 的函数[汇编实现] */
extern void gdt_flush(uint32_t);

#endif // INCLUDE_GDT_H_
