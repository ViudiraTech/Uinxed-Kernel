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
#define UART_TX_BUF_SIZE 8192

typedef struct uart_port uart_port_t;

typedef struct uart_ops {
        /* All callbacks are invoked with uart_port.lock held. */
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
        /*
         * Normal tty writes are queued here and drained by the UART TX-empty
         * interrupt.  Keeping this in the port makes serial output bounded:
         * a userspace logger must never busy-wait for every character while
         * holding interrupts off on the only vCPU.
         */
        uint8_t     tx_buf[UART_TX_BUF_SIZE];
        size_t      tx_head;
        size_t      tx_tail;
        size_t      tx_count;

        /* Serializes UART registers, driver callbacks, and open/close state. */
        spinlock_t   lock;
        spinlock_t   rx_lock;
        unsigned int open_count;
        bool         hw_started;
        bool         console_started;
        bool         tty_initialized;
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

/* Start/stop a port with open-count and per-port locking. */
int  uart_port_open(uart_port_t *port);
void uart_port_close(uart_port_t *port);
int  uart_port_console_startup(uart_port_t *port);

/* Output path: emit data through the port's uart_ops. */
int uart_write(uart_port_t *port, const uint8_t *data, size_t len);

/* Console output path: invoke the driver console callback under the port lock. */
void uart_console_write(uart_port_t *port, const uint8_t *data, size_t len);

/* Resolve the tty core backing a serial port (for /dev/console). */
int serial_tty_core(int index, tty_core_t **core);

#endif // INCLUDE_SERIAL_CORE_H_
