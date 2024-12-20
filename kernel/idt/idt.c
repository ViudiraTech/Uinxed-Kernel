/*
 *
 *		idt.c
 *		设置中断描述符程序
 *
 *		2024/6/27 By Rainy101112
 *		基于 GPL-3.0 开源协议
 *		Copyright © 2020 ViudiraTech，保留最终解释权。
 *
 */

#include "common.h"
#include "string.h"
#include "debug.h"
#include "idt.h"
#include "printk.h"

/* 声明中断处理函数 0-19 属于 CPU 的异常中断 */
/* ISR:中断服务程序(interrupt service routine) */
void isr0(void); 		// 0 #DE 除 0 异常
void isr1(void); 		// 1 #DB 调试异常
void isr2(void); 		// 2 NMI
void isr3(void); 		// 3 BP 断点异常
void isr4(void); 		// 4 #OF 溢出
void isr5(void); 		// 5 #BR 对数组的引用超出边界
void isr6(void); 		// 6 #UD 无效或未定义的操作码
void isr7(void); 		// 7 #NM 设备不可用（无数学协处理器）
void isr8(void); 		// 8 #DF 双重故障（有错误代码）
void isr9(void); 		// 9 协处理器跨段操作
void isr10(void); 		// 10 #TS 无效TSS（有错误代码）
void isr11(void); 		// 11 #NP 段不存在（有错误代码）
void isr12(void); 		// 12 #SS 栈错误（有错误代码）
void isr13(void); 		// 13 #GP 常规保护（有错误代码）
void isr14(void); 		// 14 #PF 页故障（有错误代码）
void isr15(void); 		// 15 CPU 保留
void isr16(void); 		// 16 #MF 浮点处理单元错误
void isr17(void); 		// 17 #AC 对齐检查
void isr18(void); 		// 18 #MC 机器检查
void isr19(void); 		// 19 #XM SIMD（单指令多数据）浮点异常

/* 20-31 Intel 保留 */
void isr20(void);
void isr21(void);
void isr22(void);
void isr23(void);
void isr24(void);
void isr25(void);
void isr26(void);
void isr27(void);
void isr28(void);
void isr29(void);
void isr30(void);
void isr31(void);

/* 声明 IRQ 函数 */
/* IRQ:中断请求(Interrupt Request) */
void irq0(void);		// 电脑系统计时器
void irq1(void); 		// 键盘
void irq2(void); 		// 与 IRQ9 相接，MPU-401 MD 使用
void irq3(void); 		// 串口设备
void irq4(void); 		// 串口设备
void irq5(void); 		// 建议声卡使用
void irq6(void); 		// 软驱传输控制使用
void irq7(void); 		// 打印机传输控制使用
void irq8(void); 		// 即时时钟
void irq9(void); 		// 与 IRQ2 相接，可设定给其他硬件
void irq10(void); 		// 建议网卡使用
void irq11(void); 		// 建议 AGP 显卡使用
void irq12(void); 		// 接 PS/2 鼠标，也可设定给其他硬件
void irq13(void); 		// 协处理器使用
void irq14(void); 		// IDE0 传输控制使用
void irq15(void); 		// IDE1 传输控制使用

/* 32～255 用户自定义异常 */
void isr128(void);

/* 中断描述符表 */
idt_entry_t idt_entries[256];

/* IDTR */
idt_ptr_t idt_ptr;

/* 中断处理函数的指针数组 */
interrupt_handler_t interrupt_handlers[256];

/* 初始化中断描述符表 */
void init_idt(void)
{
	print_busy("Initializing the interrupt descriptor table...\r"); // 提示用户正在初始化中断描述符表，并回到行首等待覆盖

	/* 重新映射 IRQ 表 */
	/* 两片级联的 Intel 8259A 芯片 */
	/* 主片端口 0x20 0x21 */
	/* 从片端口 0xA0 0xA1 */

	/* 初始化主片、从片 */
	/* 0001 0001 */
	outb(0x20, 0x11);
	outb(0xA0, 0x11);

	/* 设置主片 IRQ 从 0x20(32) 号中断开始 */
	outb(0x21, 0x20);

	/* 设置从片 IRQ 从 0x28(40) 号中断开始 */
	outb(0xA1, 0x28);

	/* 设置主片 IR2 引脚连接从片 */
	outb(0x21, 0x04);

	/* 告诉从片输出引脚和主片 IR2 号相连 */
	outb(0xA1, 0x02);

	/* 设置主片和从片按照 8086 的方式工作 */
	outb(0x21, 0x01);
	outb(0xA1, 0x01);

	/* 设置主从片允许中断 */
	outb(0x21, 0x0);
	outb(0xA1, 0x0);

	bzero((uint8_t *)&interrupt_handlers, sizeof(interrupt_handler_t) * 256);

	idt_ptr.limit	= sizeof(idt_entry_t) * 256 - 1;
	idt_ptr.base	= (uint32_t)&idt_entries;

	bzero((uint8_t *)&idt_entries, sizeof(idt_entry_t) * 256);

	/* 0-32:  用于 CPU 的中断处理 */
#define SET_ISR(N) idt_set_gate(N, (uint32_t)isr##N, 0x08, 0x8E)
	SET_ISR( 0);
	SET_ISR( 1);
	SET_ISR( 2);
	SET_ISR( 3);
	SET_ISR( 4);
	SET_ISR( 5);
	SET_ISR( 6);
	SET_ISR( 7);
	SET_ISR( 8);
	SET_ISR( 9);
	SET_ISR(10);
	SET_ISR(11);
	SET_ISR(12);
	SET_ISR(13);
	SET_ISR(14);
	SET_ISR(15);
	SET_ISR(16);
	SET_ISR(17);
	SET_ISR(18);
	SET_ISR(19);
	SET_ISR(20);
	SET_ISR(21);
	SET_ISR(22);
	SET_ISR(23);
	SET_ISR(24);
	SET_ISR(25);
	SET_ISR(26);
	SET_ISR(27);
	SET_ISR(28);
	SET_ISR(29);
	SET_ISR(30);
	SET_ISR(31);
#undef SET_ISR

#define SET_IRQ(N) idt_set_gate(32 + N, (uint32_t)irq##N, 0x08, 0x8E)
	SET_IRQ( 0);
	SET_IRQ( 1);
	SET_IRQ( 2);
	SET_IRQ( 3);
	SET_IRQ( 4);
	SET_IRQ( 5);
	SET_IRQ( 6);
	SET_IRQ( 7);
	SET_IRQ( 8);
	SET_IRQ( 9);
	SET_IRQ(10);
	SET_IRQ(11);
	SET_IRQ(12);
	SET_IRQ(13);
	SET_IRQ(14);
	SET_IRQ(15);
#undef SET_IRQ

	/* 系统调用 */
	idt_use_reg(128, (uint32_t)isr128);

	/* 更新设置中断描述符表 */
	idt_flush((uint32_t)&idt_ptr);

	print_succ("Interrupt Descriptor Table initialized successfully.\n"); // 提示用户idt初始化完成
}

/* 设置中断描述符 */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags)
{
	idt_entries[num].base_lo = base & 0xFFFF;
	idt_entries[num].base_hi = (base >> 16) & 0xFFFF;

	idt_entries[num].sel     = sel;
	idt_entries[num].always0 = 0;

	/* 先留下 0x60 这个魔数，以后实现用户态时候 */
	/* 这个与运算可以设置中断门的特权级别为 3 */
	// 你这不是或运算吗 <- 我勒个千年老评论啊(From MicroFish)
	idt_entries[num].flags = flags; // | 0x60
}

/* 设置用户中断描述符 */
void idt_use_reg(uint8_t num, uint32_t base)
{
	idt_entries[num].base_lo = base & 0xFFFF;
	idt_entries[num].base_hi = (base >> 16) & 0xFFFF;

	idt_entries[num].sel = 0x08;
	idt_entries[num].always0 = 0;
	idt_entries[num].flags = 0x8E | 0x60;
}

/* 调用中断处理函数 */
void isr_handler(pt_regs *regs)
{
	if (interrupt_handlers[regs->int_no]) {
		interrupt_handlers[regs->int_no](regs);
	} else {
		print_warn("Unhandled interrupt: ");
		printk("0x%02X\n", regs->int_no);
	}
}

/* 注册一个中断处理函数 */
void register_interrupt_handler(uint8_t n, interrupt_handler_t h)
{
	interrupt_handlers[n] = h;
}

/* IRQ 处理函数 */
void irq_handler(pt_regs *regs)
{
	/* 发送中断结束信号给 PICs */
	/* 按照我们的设置，从 32 号中断起为用户自定义中断 */
	/* 因为单片的 Intel 8259A 芯片只能处理 8 级中断 */
	/* 故大于等于 40 的中断号是由从片处理的 */
	if (regs->int_no >= 40) {
		/* 发送重设信号给从片 */
		outb(0xA0, 0x20);
	}
	/* 发送重设信号给主片 */
	outb(0x20, 0x20);
	
	if (interrupt_handlers[regs->int_no]) {
		interrupt_handlers[regs->int_no](regs);
	}
}
