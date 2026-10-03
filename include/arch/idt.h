/*
 *
 *      idt.h
 *      Interrupt descriptor header file
 *
 *      2024/6/27 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_IDT_H_
#define INCLUDE_IDT_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* True when the trapped context ran in user mode.  Operates on any frame whose saved iret state begins with cs. */
#define user_mode(frame) (((frame)->cs & 3U) == 3U)

#define ISR_0  0  // #DE Division by 0 exception
#define ISR_1  1  // #DB Debugging exceptions
#define ISR_2  2  // NMI Non-maskable interrupt
#define ISR_3  3  // BP Breakpoint exception
#define ISR_4  4  // #OF overflow
#define ISR_5  5  // #BR Reference to array out of bounds
#define ISR_6  6  // #UD Invalid or undefined opcode
#define ISR_7  7  // #NM Device not available (no math coprocessor)
#define ISR_8  8  // #DF Double fault (with error code)
#define ISR_9  9  // Coprocessor cross-segment operation
#define ISR_10 10 // #TS Invalid TSS (with error code)
#define ISR_11 11 // #NP Segment does not exist (with error code)
#define ISR_12 12 // #SS Stack error (with error code)
#define ISR_13 13 // #GP General protection (with error code)
#define ISR_14 14 // #PF Page fault (with error code)
#define ISR_15 15 // CPU Reserved
#define ISR_16 16 // #MF Floating point processing unit error
#define ISR_17 17 // #AC Alignment Check
#define ISR_18 18 // #MC Machine inspection
#define ISR_19 19 // #XM SIMD (Single Instruction Multiple Data) floating point exceptions

#define IRQ_0  32 // Computer system timer
#define IRQ_1  33 // Keyboard
#define IRQ_2  34 // Connected to IRQ9, used by MPU-401 MD
#define IRQ_3  35 // Serial Devices
#define IRQ_4  36 // Serial Devices
#define IRQ_5  37 // Recommended sound card
#define IRQ_6  38 // Floppy drive transfer control usage
#define IRQ_7  39 // Printer transmission control use
#define IRQ_8  40 // Real-time clock
#define IRQ_9  41 // Connected to IRQ2, can be assigned to other hardware
#define IRQ_10 42 // Recommended network card
#define IRQ_11 43 // Recommended for AGP graphics cards
#define IRQ_12 44 // Connect to PS/2 mouse, can also be set to other hardware
#define IRQ_13 45 // Coprocessor usage
#define IRQ_14 46 // IDE0 transmission control usage
#define IRQ_15 47 // IDE1 transmission control usage

typedef struct {
        uint16_t size;
        void    *ptr;
} __attribute__((packed)) idt_register_t;

/* The exact 10-byte operand lidt expects: 2-byte limit followed by a 64-bit base. */
_Static_assert(sizeof(idt_register_t) == 10, "idt_register_t descriptor size");
_Static_assert(offsetof(idt_register_t, ptr) == 2, "idt_register_t base offset");

typedef struct {
        uint16_t offset_low; // Processing function pointer low 16-bit address
        uint16_t selector;   // Segment Selector
        uint8_t  ist;
        uint8_t  flags;      // Flags
        uint16_t offset_mid; // Handling 16-bit addresses in function pointers
        uint32_t offset_hi;  // Processing function pointer high 32-bit address
        uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

_Static_assert(sizeof(idt_entry_t) == 16, "x86-64 IDT gate descriptor size");

typedef struct {
        uint64_t rip;
        uint64_t cs;
        uint64_t rflags;
        uint64_t rsp;
        uint64_t ss;
} interrupt_frame_t;

/* The frame the CPU pushes for a vector that carries no error code. */
_Static_assert(sizeof(interrupt_frame_t) == 40, "x86-64 interrupt frame size");
_Static_assert(offsetof(interrupt_frame_t, rflags) == 16, "interrupt frame rflags offset");
_Static_assert(offsetof(interrupt_frame_t, rsp) == 24, "interrupt frame rsp offset");
_Static_assert(offsetof(interrupt_frame_t, ss) == 32, "interrupt frame ss offset");

extern idt_register_t idt_pointer;

/* Initialize the interrupt descriptor table */
void init_idt(void);

/* Register an interrupt handler */
void register_interrupt_handler(uint16_t vector, void *handler, uint8_t ist, uint8_t flags);

/* Restore an interrupt vector to its default empty handler */
void unregister_interrupt_handler(uint16_t vector);

#endif // INCLUDE_IDT_H_
