/*
 *
 *		inthandle.c
 *		Interrupt HandlerInterrupt Handler
 *
 *		2024/8/1 By Rainy101112
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "common.h"
#include "debug.h"
#include "idt.h"
#include "printk.h"

__attribute__((interrupt)) static void ISR_0_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #DE\n");
}

__attribute__((interrupt)) static void ISR_1_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #DB\n");
}

__attribute__((interrupt)) static void ISR_2_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel fatal error: NMI\n");
}

__attribute__((interrupt)) static void ISR_3_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel breakpoint exception: BP\n");
}

__attribute__((interrupt)) static void ISR_4_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #OF\n");
}

__attribute__((interrupt)) static void ISR_5_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #BR\n");
}

__attribute__((interrupt)) static void ISR_6_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #UD\n");
}

__attribute__((interrupt)) static void ISR_7_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #NM\n");
}

__attribute__((interrupt)) static void ISR_8_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #DF\n");
}

__attribute__((interrupt)) static void ISR_9_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: Coprocessor Segment Overrun\n");
}

__attribute__((interrupt)) static void ISR_10_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #TS\n");
}

__attribute__((interrupt)) static void ISR_11_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #NP\n");
}

__attribute__((interrupt)) static void ISR_12_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #SS\n");
}

__attribute__((interrupt)) static void ISR_13_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #GP\n");
}

/* ISR 14 will be define by pagine program */

/* ISR 15 CPU reserved */

__attribute__((interrupt)) static void ISR_16_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #MF\n");
}

__attribute__((interrupt)) static void ISR_17_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #AC\n");
}

__attribute__((interrupt)) static void ISR_18_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #MC\n");
}

__attribute__((interrupt)) static void ISR_19_handle(interrupt_frame_t *frame)
{
	(void)frame;
	panic("Kernel exception: #XM\n");
}

/* Register ISR interrupt processing */
void isr_registe_handle(void)
{
	register_interrupt_handler(ISR_0, (void *)ISR_0_handle, 0, 0x8e);
	register_interrupt_handler(ISR_1, (void *)ISR_1_handle, 0, 0x8e);
	register_interrupt_handler(ISR_2, (void *)ISR_2_handle, 0, 0x8e);
	register_interrupt_handler(ISR_3, (void *)ISR_3_handle, 0, 0x8e);
	register_interrupt_handler(ISR_4, (void *)ISR_4_handle, 0, 0x8e);
	register_interrupt_handler(ISR_5, (void *)ISR_5_handle, 0, 0x8e);
	register_interrupt_handler(ISR_6, (void *)ISR_6_handle, 0, 0x8e);
	register_interrupt_handler(ISR_7, (void *)ISR_7_handle, 0, 0x8e);
	register_interrupt_handler(ISR_8, (void *)ISR_8_handle, 0, 0x8e);
	register_interrupt_handler(ISR_9, (void *)ISR_9_handle, 0, 0x8e);
	register_interrupt_handler(ISR_10, (void *)ISR_10_handle, 0, 0x8e);
	register_interrupt_handler(ISR_11, (void *)ISR_11_handle, 0, 0x8e);
	register_interrupt_handler(ISR_12, (void *)ISR_12_handle, 0, 0x8e);
	register_interrupt_handler(ISR_13, (void *)ISR_13_handle, 0, 0x8e);

	/* ISR 14 will be define by pagine program */
	/* ISR 15 CPU reserved */

	register_interrupt_handler(ISR_16, (void *)ISR_16_handle, 0, 0x8e);
	register_interrupt_handler(ISR_17, (void *)ISR_17_handle, 0, 0x8e);
	register_interrupt_handler(ISR_18, (void *)ISR_18_handle, 0, 0x8e);
	register_interrupt_handler(ISR_19, (void *)ISR_19_handle, 0, 0x8e);
}
