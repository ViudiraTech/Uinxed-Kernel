/*
 *
 *      idt.h
 *      Interrupt descriptor header file
 *
 *      2024/6/27 By Rainy101112
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_IDT_H_
#define INCLUDE_IDT_H_

#include <stdint.h>

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

typedef struct {
        uint16_t offset_low; // Processing function pointer low 16-bit address
        uint16_t selector;   // Segment Selector
        uint8_t  ist;
        uint8_t  flags;      // Flags
        uint16_t offset_mid; // Handling 16-bit addresses in function pointers
        uint32_t offset_hi;  // Processing function pointer high 32-bit address
        uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct {
        uint64_t rip;
        uint64_t cs;
        uint64_t rflags;
        uint64_t rsp;
        uint64_t ss;
} __attribute__((packed)) interrupt_frame_t;

extern idt_register_t idt_pointer;

/* Initialize the interrupt descriptor table */
void init_idt(void);

/* Register an interrupt handler */
void register_interrupt_handler(uint16_t vector, void *handler, uint8_t ist, uint8_t flags);

#endif // INCLUDE_IDT_H_
