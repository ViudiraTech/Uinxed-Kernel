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
#include "gdt.h"
#include "printk.h"

#define printk_panic(str, ...) printk("\033[31m[KERISR]\033[0m"); printk(str, ##__VA_ARGS__);

static void exception_panic(const char *name, struct interrupt_frame *frame, uintptr_t error_code)
{
	printk_panic("Kernel exception %s: %08x\n", name, error_code);
	printk("EIP: %08x\nCS: %x\nEFLAGS: %08x\n", frame->ip, frame->cs, frame->flags);

	printk_panic("System halted\n");
	krn_halt();
}

#define ISR_NOERRCODE(n, name)                        \
__attribute__ ((interrupt))                           \
void ISR_##n##_handle(struct interrupt_frame *frame)  \
{                                                     \
	exception_panic(name, frame, 0);              \
}

#define ISR_ERRCODE(n, name)                          \
__attribute__ ((interrupt))                           \
void ISR_##n##_handle(struct interrupt_frame *frame,  \
                      uintptr_t error_code)           \
{                                                     \
	exception_panic(name, frame, error_code);     \
}

ISR_NOERRCODE(0, "#DE")  // 0 #DE 除 0 异常
ISR_NOERRCODE(1, "#DB")  // 1 #DB 调试异常
ISR_NOERRCODE(2, "NMI")  // 2 NMI
ISR_NOERRCODE(3, "#BP")  // 3 #BP 断点异常
ISR_NOERRCODE(4, "#OF")  // 4 #OF 溢出
ISR_NOERRCODE(5, "#BR")  // 5 #BR 对数组的引用超出边界
ISR_NOERRCODE(6, "#UD")  // 6 #UD 无效或未定义的操作码
ISR_NOERRCODE(7, "#NM")  // 7 #NM 设备不可用（无数学协处理器）
ISR_ERRCODE(8, "#DF")    // 8 #DF 双重故障（有错误代码）
ISR_NOERRCODE(9, "CSO")  // 9 协处理器跨段操作
ISR_ERRCODE(10, "#TS")   // 10 #TS 无效TSS（有错误代码）
ISR_ERRCODE(11, "#NP")   // 11 #NP 段不存在（有错误代码）
ISR_ERRCODE(12, "#SS")   // 12 #SS 栈错误（有错误代码）
ISR_ERRCODE(13, "#GP")   // 13 #GP 常规保护（有错误代码）
ISR_ERRCODE(14, "PF")    // 14 #PF 页故障（有错误代码）
ISR_NOERRCODE(16, "MF")  // 16 #MF 浮点处理单元错误
ISR_ERRCODE(17, "#AC")   // 17 #AC 对齐检查
ISR_NOERRCODE(18, "#MC") // 18 #MC 机器检查
ISR_NOERRCODE(19, "#XM") // 19 #XM SIMD（单指令多数据）浮点异常
ISR_NOERRCODE(20, "#VE") // 20 #VE
ISR_ERRCODE(21, "#CP")   // 21 #CP

static int irq_acknowledge(uint8_t irq)
{
	uint16_t port = (irq<8)?0x20:0xA0;
	/* 读ISR */
	outb(port, 0x0B);
	uint8_t isr = inb(port);
	uint8_t mask = (1 << (irq % 8))&0xFF;
	int is_servicing = (isr&mask)?1:0;

	if (is_servicing)
		outb(port, 0x60|(irq % 8)); // EOI
	if (port == 0xA0)
		outb(port, 0x60|2); // 从片默认接到IRQ2上
	return is_servicing;
}

static void irq_enable(uint8_t irq) {
	uint16_t port = (irq<8)?0x21:0xA1;
	uint8_t mask = inb(port);
	uint8_t clear = (1 << (irq % 8))&0xFF;
	mask ^= mask & clear;
	outb(port, mask);
}

/* 32～255 用户自定义异常 */

/* IRQ处理函数的指针数组 */
interrupt_handler_t irq_handlers[32] = {0};

/* 声明 IRQ 函数 */
#define IRQ(n)                                                 \
__attribute__ ((interrupt))                                    \
void IRQ_##n##_handle(struct interrupt_frame *frame)           \
{                                                              \
	if (irq_acknowledge(n))                                \
		irq_handlers[n](frame);                        \
}

/* IRQ:中断请求(Interrupt Request) */
IRQ(0);		// 电脑系统计时器
IRQ(1);		// 键盘
IRQ(2);		// 默认由从片占据，别用
IRQ(3);		// 串口设备
IRQ(4);		// 串口设备
IRQ(5);		// 建议声卡使用
IRQ(6);		// 软驱传输控制使用
IRQ(7);		// 打印机传输控制使用
IRQ(8);		// 即时时钟
IRQ(9);		// 与 IRQ2 相接，可设定给其他硬件
IRQ(10);		// 建议网卡使用
IRQ(11);		// 建议 AGP 显卡使用
IRQ(12);		// 接 PS/2 鼠标，也可设定给其他硬件
IRQ(13);		// 协处理器使用
IRQ(14);		// IDE0 传输控制使用
IRQ(15);		// IDE1 传输控制使用

/* 中断描述符表 */
idt_entry_t idt_entries[256] = {0};

/* IDTR */
idt_ptr_t idt_ptr = {0};

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

	/* 处理不了的就别接收了 */
	outb(0x21, 0xff);
	outb(0xA1, 0xff);

	/* 主片接收来自从片的中断 */
	irq_enable(0x02);

	idt_ptr.limit	= sizeof(idt_entry_t) * 256 - 1;
	idt_ptr.base	= (uint32_t)&idt_entries;

	/* 0-32:  用于 CPU 的中断处理 */
#define SET_ISR(N) idt_set_gate(N, (uint32_t)ISR_##N##_handle, 0x08, 0x8E)
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
	/* ISR 14 will be define by paging program */
	idt_set_gate(14, (uint32_t)page_fault, 0x08, 0x8E);
	SET_ISR(16);
	SET_ISR(17);
	SET_ISR(18);
	SET_ISR(19);
	SET_ISR(20);
	SET_ISR(21);
#undef SET_ISR

#define SET_IRQ(N) idt_set_gate(32 + N, (uint32_t)IRQ_##N##_handle, 0x08, 0x8E)
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

	/* 更新设置中断描述符表 */
	__asm__("lidt %0" :: "m"(idt_ptr));

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
void idt_use_reg(uint8_t num, interrupt_handler_t h)
{
	idt_set_gate(num, (uintptr_t)h, 0x08, 0x8E | 0x60);
}

/* 注册一个中断处理函数 */
void register_interrupt_handler(uint8_t n, interrupt_handler_t h)
{
	if ((n >= 0x20) && (n < 0x30))
	{
		uint8_t irq = n - 0x20;
		irq_handlers[irq] = h;
		irq_enable(irq);
	} else {
		idt_set_gate(n, (uintptr_t)h, 0x08, 0x8E);
	}
}
