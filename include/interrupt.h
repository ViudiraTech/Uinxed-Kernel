/*
 *
 *		interrupt.h
 *		中断相关头文件
 *
 *		2024/8/1 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_INTERRUPT_H_
#define INCLUDE_INTERRUPT_H_

/* 寄存器 */
typedef struct registers {
	uint64_t ds;
	uint64_t es;
	uint64_t fs;
	uint64_t gs;
	uint64_t rax;
	uint64_t rbx;
	uint64_t rcx;
	uint64_t rdx;
	uint64_t rbp;
	uint64_t rsi;
	uint64_t rdi;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;
	uint64_t vector;
	uint64_t err_code;
	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
	uint64_t rsp;
	uint64_t ss;
} registers_t;

/* 空函数处理 */
extern void (*empty_handle[256])(interrupt_frame_t *frame);

/* 注册ISR中断处理 */
void isr_registe_handle(void);

#endif // INCLUDE_INTERRUPT_H_
