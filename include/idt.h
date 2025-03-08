/*
 *
 *		idt.h
 *		中断描述符头文件
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#ifndef INCLUDE_IDT_H_
#define INCLUDE_IDT_H_

#include "stdint.h"

#define ISR_0 0		//#DE 除 0 异常
#define ISR_1 1		//#DB 调试异常
#define ISR_2 2		//NMI
#define ISR_3 3		//BP 断点异常
#define ISR_4 4		//#OF 溢出
#define ISR_5 5		//#BR 对数组的引用超出边界
#define ISR_6 6		//#UD 无效或未定义的操作码
#define ISR_7 7		//#NM 设备不可用（无数学协处理器）
#define ISR_8 8		//#DF 双重故障（有错误代码）
#define ISR_9 9		//协处理器跨段操作
#define ISR_10 10	//#TS 无效TSS（有错误代码）
#define ISR_11 11	//#NP 段不存在（有错误代码）
#define ISR_12 12	//#SS 栈错误（有错误代码）
#define ISR_13 13	//#GP 常规保护（有错误代码）
#define ISR_14 14	//#PF 页故障（有错误代码）
#define ISR_15 15	//CPU 保留
#define ISR_16 16	//#MF 浮点处理单元错误
#define ISR_17 17	//#AC 对齐检查
#define ISR_18 18	//#MC 机器检查
#define ISR_19 19	//#XM SIMD（单指令多数据）浮点异常

#define IRQ_32 32 // 电脑系统计时器
#define IRQ_33 33 // 键盘
#define IRQ_34 34 // 与 IRQ9 相接，MPU-401 MD 使用
#define IRQ_35 35 // 串口设备
#define IRQ_36 36 // 串口设备
#define IRQ_37 37 // 建议声卡使用
#define IRQ_38 38 // 软驱传输控制使用
#define IRQ_39 39 // 打印机传输控制使用
#define IRQ_40 40 // 即时时钟
#define IRQ_41 41 // 与 IRQ2 相接，可设定给其他硬件
#define IRQ_42 42 // 建议网卡使用
#define IRQ_43 43 // 建议 AGP 显卡使用
#define IRQ_44 44 // 接 PS/2 鼠标，也可设定给其他硬件
#define IRQ_45 45 // 协处理器使用
#define IRQ_46 46 // IDE0 传输控制使用
#define IRQ_47 47 // IDE1 传输控制使用

struct idt_register {
	uint16_t size;
	void *ptr;
} __attribute__((packed));

struct idt_entry {
	uint16_t offset_low;	// 处理函数指针低16位地址
	uint16_t selector;		// 段选择子
	uint8_t ist;
	uint8_t flags;			// 标志位
	uint16_t offset_mid;	// 处理函数指针中16位地址
	uint32_t offset_hi;		// 处理函数指针高32位地址
	uint32_t reserved;
} __attribute__((packed));

struct interrupt_frame {
	uint64_t rip;
	uint64_t cs;
	uint64_t rflags;
	uint64_t rsp;
	uint64_t ss;
};

typedef struct interrupt_frame interrupt_frame_t;

/* 初始化中断描述符表 */
void init_idt(void);

/* 注册一个中断处理函数 */
void register_interrupt_handler(uint16_t vector, void *handler, uint8_t ist, uint8_t flags);

#endif // INCLUDE_IDT_H_
