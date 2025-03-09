/*
 *
 *		idt.c
 *		中断描述符
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，基于GPLv3协议。
 *
 */

#include "idt.h"
#include "printk.h"
#include "interrupt.h"

struct idt_register idt_pointer;
struct idt_entry idt_entries[256];

/* 中断服务程序
 * 0 #DE 除 0 异常					（已注册）
 * 1 #DB 调试异常					（已注册）
 * 2 NMI							（已注册）
 * 3 BP 断点异常						（已注册）
 * 4 #OF 溢出						（已注册）
 * 5 #BR 对数组的引用超出边界			（已注册）
 * 6 #UD 无效或未定义的操作码			（已注册）
 * 7 #NM 设备不可用（无数学协处理器）	（已注册）
 * 8 #DF 双重故障（有错误代码）		（已注册）
 * 9 协处理器跨段操作					（已注册）
 * 10 #TS 无效TSS（有错误代码）		（已注册）
 * 11 #NP 段不存在（有错误代码）		（已注册）
 * 12 #SS 栈错误（有错误代码）		（已注册）
 * 13 #GP 常规保护（有错误代码）		（已注册）
 * 14 #PF 页故障（有错误代码）		（已注册）
 * 15 CPU 保留						（已注册）
 * 16 #MF 浮点处理单元错误			（已注册）
 * 17 #AC 对齐检查					（已注册）
 * 18 #MC 机器检查					（已注册）
 * 19 #XM SIMD（单指令多数据）浮点异常（已注册）
 * 20-31 Intel 保留					（已保留）
 * 32-255 用户自定义异常
 */

/* 中断请求
 * 32 电脑系统计时器					（已注册）
 * 33 键盘							（未注册）
 * 34 与 IRQ9 相接，MPU-401 MD 使用	（未注册）
 * 35 串口设备						（未注册）
 * 36 串口设备						（未注册）
 * 37 建议声卡使用					（未注册）
 * 38 软驱传输控制使用				（未注册）
 * 39 打印机传输控制使用				（未注册）
 * 40 即时时钟						（未注册）
 * 41 与 IRQ2 相接，可设定给其他硬件	（未注册）
 * 42 建议网卡使用					（未注册）
 * 43 建议 AGP 显卡使用				（未注册）
 * 44 接 PS/2 鼠标，也可设定给其他硬件	（未注册）
 * 45 协处理器使用					（未注册）
 * 46 IDE0 传输控制使用				（未注册）
 * 47 IDE1 传输控制使用				（未注册）
 */

/* 初始化中断描述符表 */
void init_idt(void)
{
	idt_pointer.size = (uint16_t)sizeof(idt_entries) - 1;
	idt_pointer.ptr = &idt_entries;

	__asm__ __volatile__("lidt %0" :: "m"(idt_pointer) : "memory");
	plogk("IDT: IDT initialized at 0x%016x (limit: 0x%04x)\n", idt_entries, idt_pointer.size);
	plogk("IDT: Loaded IDTR with base = 0x%016x, limit = %d\n", idt_pointer.ptr, idt_pointer.size + 1);

	for (int i = 0; i < 256; i++) {
		register_interrupt_handler(i, empty_handle[i], 0, 0x8e);
	}
}

/* 注册一个中断处理函数 */
void register_interrupt_handler(uint16_t vector, void *handler, uint8_t ist, uint8_t flags)
{
	uint64_t addr = (uint64_t)handler;
	idt_entries[vector].offset_low = (uint16_t)addr;
	idt_entries[vector].ist = ist;
	idt_entries[vector].flags = flags;
	idt_entries[vector].selector = 0x08;
	idt_entries[vector].offset_mid = (uint16_t)(addr >> 16);
	idt_entries[vector].offset_hi = (uint32_t)(addr >> 32);
}
