/*
 *
 *      8250.h
 *      PC 16550 UART hardware driver
 *
 *      2026/8/10 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_8250_H_
#define INCLUDE_8250_H_

#include <kernel/debug/ringlog.h>

#define UART8250_REG_DATA 0
#define UART8250_REG_IER  1
#define UART8250_REG_FCR  2
#define UART8250_REG_IIR  2
#define UART8250_REG_LCR  3
#define UART8250_REG_MCR  4
#define UART8250_REG_LSR  5
#define UART8250_REG_MSR  6

#define UART8250_BASE1 0x3f8
#define UART8250_BASE2 0x2f8
#define UART8250_BASE3 0x3e8
#define UART8250_BASE4 0x2e8

extern log_buffer_t serial_log;

#if CONFIG_SERIAL

/* Probe the legacy COM ports and register the uart driver. */
void init_serial(void);

/* Install IRQ handlers. Must run after init_idt(). */
void serial_irq_install(void);

#else
static inline void init_serial(void) {}
static inline void serial_irq_install(void) {}
#endif

/* Whether the serial port at the given index was detected. */
int serial_port_present(int index);

#endif // INCLUDE_8250_H_
