/*
 *
 *		inthandle.c
 *		中断处理程序
 *
 *		2024/8/1 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留所有权利。
 *
 */

#include "idt.h"
#include "debug.h"
#include "printk.h"
#include "console.h"
#include "common.h"

#define printk_panic(str) printk_color(rc_black, rc_red, "[KERISR]"); printk(str);

/*
 *
 *	ISR_NOERRCODE  0 ; 0 #DE 除 0 异常
 *	ISR_NOERRCODE  1 ; 1 #DB 调试异常
 *	ISR_NOERRCODE  2 ; 2 NMI
 *	ISR_NOERRCODE  3 ; 3 BP 断点异常
 *	ISR_NOERRCODE  4 ; 4 #OF 溢出
 *	ISR_NOERRCODE  5 ; 5 #BR 对数组的引用超出边界
 *	ISR_NOERRCODE  6 ; 6 #UD 无效或未定义的操作码
 *	ISR_NOERRCODE  7 ; 7 #NM 设备不可用（无数学协处理器）
 *	ISR_ERRCODE    8 ; 8 #DF 双重故障（有错误代码）
 *	ISR_NOERRCODE  9 ; 9 协处理器跨段操作
 *	ISR_ERRCODE   10 ; 10 #TS 无效TSS（有错误代码）
 *	ISR_ERRCODE   11 ; 11 #NP 段不存在（有错误代码）
 *	ISR_ERRCODE   12 ; 12 #SS 栈错误（有错误代码）
 *	ISR_ERRCODE   13 ; 13 #GP 常规保护（有错误代码）
 *	ISR_ERRCODE   14 ; 14 #PF 页故障（有错误代码）
 *	ISR_NOERRCODE 15 ; 15 CPU 保留
 *	ISR_NOERRCODE 16 ; 16 #MF 浮点处理单元错误
 *	ISR_ERRCODE   17 ; 17 #AC 对齐检查
 *	ISR_NOERRCODE 18 ; 18 #MC 机器检查
 *	ISR_NOERRCODE 19 ; 19 #XM SIMD（单指令多数据）浮点异常
 *
 */

void ISR_0_handle(void)
{
	printk_panic("Kernel exception: #DE\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_1_handle(void)
{
	printk_panic("Kernel exception: #DB\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_2_handle(void)
{
	printk_panic("Kernel fatal error: NMI\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_3_handle(void)
{
	printk_panic("Kernel breakpoint exception: BP\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_4_handle(void)
{
	printk_panic("Kernel exception: #OF OverFlow\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_5_handle(void)
{
	printk_panic("Kernel exception: #BR\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_6_handle(void)
{
	printk_panic("Kernel exception: #UD\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_7_handle(void)
{
	printk_panic("Kernel exception: #NM\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_8_handle(void)
{
	printk_panic("Kernel exception: #DF\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_9_handle(void)
{
	printk_panic("Kernel exception: Coprocessor Segment Overrun\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_10_handle(void)
{
	printk_panic("Kernel exception: #TS\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_11_handle(void)
{
	printk_panic("Kernel exception: #NP\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_12_handle(void)
{
	printk_panic("Kernel exception: #SS\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_13_handle(void)
{
	printk_panic("Kernel exception: #GP\n");
	printk_panic("System halted\n");
	krn_halt();
}

/* ISR 14 will be define by pagine program */

/* ISR 15 CPU reserved */

void ISR_16_handle(void)
{
	printk_panic("Kernel exception: #MF\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_17_handle(void)
{
	printk_panic("Kernel exception: #AC\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_18_handle(void)
{
	printk_panic("Kernel exception: #MC\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_19_handle(void)
{
	printk_panic("Kernel exception: #XM\n");
	printk_panic("System halted\n");
	krn_halt();
}

void ISR_registe_Handle(void)
{
	print_busy("Registering ISR handles...\r");
	register_interrupt_handler(0, ISR_0_handle);
	register_interrupt_handler(1, ISR_1_handle);
	register_interrupt_handler(2, ISR_2_handle);
	register_interrupt_handler(3, ISR_3_handle);
	register_interrupt_handler(4, ISR_4_handle);
	register_interrupt_handler(5, ISR_5_handle);
	register_interrupt_handler(6, ISR_6_handle);
	register_interrupt_handler(7, ISR_7_handle);
	register_interrupt_handler(8, ISR_8_handle);
	register_interrupt_handler(9, ISR_9_handle);
	register_interrupt_handler(10, ISR_10_handle);
	register_interrupt_handler(11, ISR_11_handle);
	register_interrupt_handler(12, ISR_12_handle);
	register_interrupt_handler(13, ISR_13_handle);
	
	/* ISR 14 will be define by pagine program */
	/* ISR 15 CPU reserved */
	
	register_interrupt_handler(16, ISR_16_handle);
	register_interrupt_handler(17, ISR_17_handle);
	register_interrupt_handler(18, ISR_18_handle);
	register_interrupt_handler(19, ISR_19_handle);

	print_succ("The ISR handle was registered successfully.\n");
}
