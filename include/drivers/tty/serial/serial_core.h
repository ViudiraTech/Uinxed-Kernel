/*
 *
 *      serial_core.h
 *      UART serial core (Linux drivers/tty/serial/serial_core.c analog)
 *
 *      2026/8/10 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SERIAL_CORE_H_
#define INCLUDE_SERIAL_CORE_H_

#include <drivers/tty/tty_core.h>
#include <drivers/tty/tty_driver.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

#define UART_MAX_PORTS   4
#define UART_RX_BUF_SIZE 256

typedef struct uart_port uart_port_t;

typedef struct uart_ops {
        int (*startup)(uart_port_t *port);
        void (*shutdown)(uart_port_t *port);
        void (*set_termios)(uart_port_t *port);
        int (*tx_write)(uart_port_t *port, const uint8_t *data, size_t len);
        void (*console_write)(uart_port_t *port, const uint8_t *data, size_t len);
} uart_ops_t;

struct uart_port {
        int               number;  // 0-based hardware index
        bool              present; // hardware detected at probe
        const uart_ops_t *ops;
        void             *private_data; // bus driver data (I/O base, etc.)

        tty_core_t  tty_core; // line discipline for /dev/ttyS<number>
        tty_core_t *tty;      // == &tty_core
        uint8_t     rx_buf[UART_RX_BUF_SIZE];
        size_t      rx_head;
        size_t      rx_tail;
        size_t      rx_count;
        spinlock_t  rx_lock;
};

typedef struct uart_driver {
        const char  *name; // "ttyS"
        uint32_t     major;
        uint32_t     minor_start;
        tty_driver_t tty_drv; // embedded tty driver
        uart_port_t  ports[UART_MAX_PORTS];
        int          nr;
} uart_driver_t;

/* Register the uart driver and wire its embedded tty driver. */
void uart_register_driver(uart_driver_t *drv);

/* Publish /dev/ttyS<port.number> for a probed port. */
int uart_add_port(uart_driver_t *drv, uart_port_t *port);

/* RX path: called from the hardware IRQ handler for every received byte. */
void uart_insert_char(uart_port_t *port, uint8_t ch);

/* Output path: emit data through the port's uart_ops. */
int uart_write(uart_port_t *port, const uint8_t *data, size_t len);

/* Resolve the tty core backing a serial port (for /dev/console). */
int serial_tty_core(int index, tty_core_t **core);

#endif // INCLUDE_SERIAL_CORE_H_
