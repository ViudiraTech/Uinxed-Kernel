/*
 *
 *		idt.h
 *		设置中断描述符程序头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#ifndef INCLUDE_IDT_H_
#define INCLUDE_IDT_H_

#include "types.h"

/* 中断描述符 */
typedef
struct idt_entry_t {
	uint16_t base_lo;	// 中断处理函数地址 15～0 位
	uint16_t sel;		// 目标代码段描述符选择子
	uint8_t  always0;	// 置 0 段
	uint8_t  flags;		// 一些标志，文档有解释
	uint16_t base_hi;	// 中断处理函数地址 31～16 位
}__attribute__((packed)) idt_entry_t;

/* IDTR */
typedef
struct idt_ptr_t {
	uint16_t limit;		// 限长
	uint32_t base;		// 基址
} __attribute__((packed)) idt_ptr_t;

/* 寄存器类型 */
typedef
struct pt_regs_t {
	uint32_t ds;		// 用于保存用户的数据段描述符
	uint32_t edi;		// 从 edi 到 eax 由 pusha 指令压入
	uint32_t esi; 
	uint32_t ebp;
	uint32_t esp;
	uint32_t ebx;
	uint32_t edx;
	uint32_t ecx;
	uint32_t eax;
	uint32_t int_no;	// 中断号
	uint32_t err_code;	// 错误代码(有中断错误代码的中断会由CPU压入)
	uint32_t eip;		// 以下由处理器自动压入
	uint32_t cs; 		
	uint32_t eflags;
	uint32_t useresp;
	uint32_t ss;
} pt_regs;

/* 定义中断处理函数指针 */
typedef void (*interrupt_handler_t)(pt_regs *);

/* 定义IRQ */
#define	IRQ0	32		// 电脑系统计时器
#define	IRQ1	33		// 键盘
#define	IRQ2	34		// 与 IRQ9 相接，MPU-401 MD 使用
#define	IRQ3	35		// 串口设备
#define	IRQ4	36		// 串口设备
#define	IRQ5	37		// 建议声卡使用
#define	IRQ6	38		// 软驱传输控制使用
#define	IRQ7	39		// 打印机传输控制使用
#define	IRQ8	40		// 即时时钟
#define	IRQ9	41		// 与 IRQ2 相接，可设定给其他硬件
#define	IRQ10	42		// 建议网卡使用
#define	IRQ11	43		// 建议 AGP 显卡使用
#define	IRQ12	44		// 接 PS/2 鼠标，也可设定给其他硬件
#define	IRQ13	45		// 协处理器使用
#define	IRQ14	46		// IDE0 传输控制使用
#define	IRQ15	47		// IDE1 传输控制使用

/* 初始化中断描述符表 */
void init_idt(void);

/* 注册一个中断处理函数 */
void register_interrupt_handler(uint8_t n, interrupt_handler_t h);

/* ISR处理 */
void ISR_registe_Handle(void);

#endif // INCLUDE_IDT_H_
