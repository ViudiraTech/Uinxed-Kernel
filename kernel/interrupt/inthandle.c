/*
 *
 *      inthandle.c
 *      Interrupt handler
 *
 *      2024/8/1 By Rainy101112
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <debug.h>
#include <interrupt.h>
#include <printk.h>
#include <stdint.h>

void page_fault_handle(interrupt_frame_t *frame, uint64_t error_code);

INTERRUPT_BEGIN static void ISR_0_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: #DE");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_1_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: #DB");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_2_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel fatal error: NMI");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_3_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel breakpoint exception: BP");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_4_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: #OF");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_5_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: #BR");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_6_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: #UD");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_7_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: #NM");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_8_handle(interrupt_frame_t *frame)
{
    (void)frame;
    carry_error_code = 1; // carry error code
    panic("Kernel exception: #DF");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_9_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: Coprocessor Segment Overrun");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_10_handle(interrupt_frame_t *frame)
{
    (void)frame;
    carry_error_code = 1; // carry error code
    panic("Kernel exception: #TS");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_11_handle(interrupt_frame_t *frame)
{
    (void)frame;
    carry_error_code = 1; // carry error code
    panic("Kernel exception: #NP");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_12_handle(interrupt_frame_t *frame)
{
    (void)frame;
    carry_error_code = 1; // carry error code
    panic("Kernel exception: #SS");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_13_handle(interrupt_frame_t *frame)
{
    (void)frame;
    carry_error_code = 1; // carry error code
    panic("Kernel exception: #GP");
}
INTERRUPT_END

/* ISR 14 will be define by pagine program */

/* ISR 15 CPU reserved */

INTERRUPT_BEGIN static void ISR_16_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: #MF");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_17_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: #AC");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_18_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: #MC");
}
INTERRUPT_END

INTERRUPT_BEGIN static void ISR_19_handle(interrupt_frame_t *frame)
{
    (void)frame;
    panic("Kernel exception: #XM");
}
INTERRUPT_END

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
    register_interrupt_handler(ISR_14, (void *)page_fault_handle, 0, 0x8e);

    /* ISR 15 CPU reserved */

    register_interrupt_handler(ISR_16, (void *)ISR_16_handle, 0, 0x8e);
    register_interrupt_handler(ISR_17, (void *)ISR_17_handle, 0, 0x8e);
    register_interrupt_handler(ISR_18, (void *)ISR_18_handle, 0, 0x8e);
    register_interrupt_handler(ISR_19, (void *)ISR_19_handle, 0, 0x8e);

    plogk("isr: All ISR handlers are registered.\n");
}
