/*
 *
 *      8250.h
 *      PC 16550 UART hardware driver (Linux drivers/tty/serial/8250.c analog)
 *
 *      2026/8/10 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_8250_H_
#define INCLUDE_8250_H_

#include <kernel/debug/ringlog.h>
#include <libs/std/stdint.h>

extern log_buffer_t serial_log;

#define UART8250_REG_DATA 0
#define UART8250_REG_IER  1
#define UART8250_REG_FCR  2
#define UART8250_REG_IIR  2
#define UART8250_REG_LCR  3
#define UART8250_REG_MCR  4
#define UART8250_REG_LSR  5

#define UART8250_BASE1 0x3f8
#define UART8250_BASE2 0x2f8
#define UART8250_BASE3 0x3e8
#define UART8250_BASE4 0x2e8

#ifndef SERIAL_PARITY
#    define SERIAL_PARITY 0
#endif
#ifndef SERIAL_BAUD_RATE
#    define SERIAL_BAUD_RATE 9600
#endif
#ifndef SERIAL_DATA_BITS
#    define SERIAL_DATA_BITS 8
#endif
#ifndef SERIAL_STOP_BITS
#    define SERIAL_STOP_BITS 1
#endif

/* Probe the legacy COM ports and register the uart driver. */
void init_serial(void);

/* Install IRQ handlers. Must run after init_idt(). */
void serial_irq_install(void);

/* Whether the serial port at the given index was detected. */
int serial_port_present(int index);

#endif // INCLUDE_8250_H_
