/*
 *
 *      8250.c
 *      PC 16550 UART hardware driver
 *
 *      2026/8/10 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <arch/idt.h>
#include <drivers/firmware/apic.h>
#include <drivers/tty/console.h>
#include <drivers/tty/serial/8250.h>
#include <drivers/tty/serial/serial_core.h>
#include <kernel/debug/ringlog.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <libs/std/string.h>

log_buffer_t serial_log;

static console_t serial_consoles[UART_MAX_PORTS];

#define UART8250_IER_RX       0x01
#define UART8250_IER_TX_EMPTY 0x02
#define UART8250_LSR_RX_READY 0x01
#define UART8250_LSR_TX_EMPTY 0x20
#define UART8250_FIFO_SIZE    16

static uart_driver_t uart_8250_driver = {
    .name        = "ttyS",
    .major       = 4,
    .minor_start = 64,
    .nr          = UART_MAX_PORTS,
};

/* Return the port's I/O base address. */
static uint16_t uart8250_base(const uart_port_t *port)
{
    return (uint16_t)(uintptr_t)port->private_data;
}

/* Enable RX interrupts (data available) on the port. */
static int uart8250_startup(uart_port_t *port)
{
    outb(uart8250_base(port) + UART8250_REG_IER, UART8250_IER_RX);
    return 0;
}

/* Disable all UART interrupts. */
static void uart8250_shutdown(uart_port_t *port)
{
    outb(uart8250_base(port) + UART8250_REG_IER, 0x00);
}

/* Program the baud-rate divisor and line control. */
static void uart8250_set_termios(uart_port_t *port)
{
    uint16_t base    = uart8250_base(port);
    uint16_t divisor = 115200 / SERIAL_BAUD_RATE;
    uint8_t  lcr     = 0x03; // 8 data bits, no parity, 1 stop bit

    (void)port;
    outb(base + UART8250_REG_LCR, 0x80);
    outb(base + UART8250_REG_DATA, divisor & 0xff);
    outb(base + UART8250_REG_IER, (divisor >> 8) & 0xff);
    outb(base + UART8250_REG_LCR, lcr);
}

/* Fill one empty 16550 FIFO from the software ring; port->lock is held. */
static void uart8250_tx_chars_locked(uart_port_t *port)
{
    uint16_t base = uart8250_base(port);

    if (!(inb(base + UART8250_REG_LSR) & UART8250_LSR_TX_EMPTY)) return;
    for (size_t sent = 0; sent < UART8250_FIFO_SIZE && port->tx_count; sent++) {
        outb(base + UART8250_REG_DATA, port->tx_buf[port->tx_tail]);
        port->tx_tail = (port->tx_tail + 1) % UART_TX_BUF_SIZE;
        port->tx_count--;
    }
}

/*
 * Queue tty output and let the TX-empty IRQ drain it.  tty_core serializes
 * writers and invokes this with port->lock held, so this path contains no
 * baud-rate-sized polling delay.
 */
static int uart8250_tx_write(uart_port_t *port, const uint8_t *data, size_t len)
{
    uint16_t base = uart8250_base(port);
    size_t   queued = 0;

    while (queued < len && port->tx_count < UART_TX_BUF_SIZE) {
        port->tx_buf[port->tx_head] = data[queued++];
        port->tx_head = (port->tx_head + 1) % UART_TX_BUF_SIZE;
    }
    uart8250_tx_chars_locked(port);

    uint8_t ier = inb(base + UART8250_REG_IER);
    if (port->tx_count)
        ier |= UART8250_IER_TX_EMPTY;
    else
        ier &= (uint8_t)~UART8250_IER_TX_EMPTY;
    outb(base + UART8250_REG_IER, ier);
    return (int)queued;
}

/* Console/panic output remains polling, but serial_core bounds it to a byte. */
static void uart8250_console_write(uart_port_t *port, const uint8_t *data, size_t len)
{
    uint16_t base = uart8250_base(port);

    for (size_t i = 0; i < len; i++) {
        uint32_t timeout = 100000;
        while (timeout-- && !(inb(base + UART8250_REG_LSR) & UART8250_LSR_TX_EMPTY)) {}
        if (!(inb(base + UART8250_REG_LSR) & UART8250_LSR_TX_EMPTY)) return;
        outb(base + UART8250_REG_DATA, data[i]);
    }
}

static const uart_ops_t uart8250_ops = {
    .startup       = uart8250_startup,
    .shutdown      = uart8250_shutdown,
    .set_termios   = uart8250_set_termios,
    .tx_write      = uart8250_tx_write,
    .console_write = uart8250_console_write,
};

/* Console write callback: forward to the underlying UART port. */
static void serial_console_write(console_t *c, const uint8_t *buf, size_t len)
{
    uart_port_t *port = c->data;
    if (port) uart_console_write(port, buf, len);
}

/* Resolve the serial tty core for /dev/console without opening ttyS<N>. */
static tty_core_t *serial_console_get_tty(console_t *c)
{
    uart_port_t *port = c->data;
    tty_core_t  *tty  = NULL;

    /*
     * /dev/console reaches a serial console without opening /dev/ttyS<N>.
     * Resolve it through the serial core so its line discipline and wait
     * queues are initialized before init issues its first termios ioctl.
     */
    if (!port || serial_tty_core(port->number, &tty)) return NULL;

    /*
     * serial_tty_core() only initialises the line discipline; it does not
     * enable the port's RX interrupt.  serial_tty_open() performs that step
     * via startup(), but the /dev/console path bypasses serial_tty_open(), so
     * IER is left at 0 and the UART never raises the receive interrupt - the
     * console would print but never accept a byte typed on the serial line.
     * Enable RX here (startup() is idempotent) to match the ttyS<N> open path.
     */
    if (uart_port_console_startup(port)) return NULL;
    return tty;
}

/* Probe a UART base port with a loopback byte test. */
static int uart8250_detect(uint16_t base)
{
    uint16_t divisor = 115200 / SERIAL_BAUD_RATE;

    outb(base + UART8250_REG_IER, 0x00);
    outb(base + UART8250_REG_LCR, 0x80);
    outb(base + UART8250_REG_DATA, divisor & 0xff);
    outb(base + UART8250_REG_IER, (divisor >> 8) & 0xff);
    outb(base + UART8250_REG_LCR, 0x03);
    outb(base + UART8250_REG_FCR, 0xcf); // FIFO, 14-byte threshold
    outb(base + UART8250_REG_MCR, 0x0f);
    outb(base + UART8250_REG_MCR, 0x1e); // loopback
    outb(base + UART8250_REG_DATA, 0xae);

    if (inb(base + UART8250_REG_DATA) != 0xae) return 0;
    outb(base + UART8250_REG_MCR, 0x0f); // quit loopback
    return 1;
}

/* Map a port index to its shared legacy IRQ line. */
static int uart8250_irq_of(int number)
{
    /* COM1/COM3 share IRQ4, COM2/COM4 share IRQ3. */
    return (number == 0 || number == 2) ? 4 : 3;
}

/* Service RX and TX causes for every present port sharing this IRQ. */
static void uart8250_service(int line_irq)
{
    for (int i = 0; i < UART_MAX_PORTS; i++) {
        uart_port_t *port = &uart_8250_driver.ports[i];
        uint16_t     base;

        if (!port->present || uart8250_irq_of(i) != line_irq) continue;
        base = uart8250_base(port);
        /* Bound a broken/emulated UART so one shared interrupt cannot livelock. */
        for (size_t serviced = 0; serviced < 64; serviced++) {
            uint8_t  received[UART8250_FIFO_SIZE];
            size_t   received_count = 0;
            uint64_t flags          = spin_lock_irqsave(&port->lock);
            uint8_t  iir            = inb(base + UART8250_REG_IIR);
            uint8_t  reason         = iir & 0x0e;

            if (iir & 0x01) {
                spin_unlock_irqrestore(&port->lock, flags);
                break;
            }
            if (reason == 0x02) { /* transmitter holding register empty */
                uart8250_tx_chars_locked(port);
                if (!port->tx_count) {
                    uint8_t ier = inb(base + UART8250_REG_IER);
                    outb(base + UART8250_REG_IER, ier & (uint8_t)~UART8250_IER_TX_EMPTY);
                }
            } else if (reason == 0x04 || reason == 0x0c || reason == 0x06) {
                /* RX available/timeout/line status: empty the hardware FIFO. */
                while (received_count < UART8250_FIFO_SIZE &&
                       (inb(base + UART8250_REG_LSR) & UART8250_LSR_RX_READY))
                    received[received_count++] = (uint8_t)inb(base + UART8250_REG_DATA);
            } else {
                /* Reading MSR acknowledges the otherwise unhandled modem cause. */
                (void)inb(base + UART8250_REG_MSR);
            }
            spin_unlock_irqrestore(&port->lock, flags);

            /* The line discipline may echo or wake tasks; never call it locked. */
            for (size_t j = 0; j < received_count; j++) uart_insert_char(port, received[j]);
        }
    }
}

/* IRQ3 handler: service COM2/COM4. */
INTERRUPT_BEGIN static void uart8250_irq3_handler(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    (void)frame;
    uart8250_service(3);
    send_eoi();
    irq_leave_gs(frame);
}
INTERRUPT_END

/* IRQ4 handler: service COM1/COM3. */
INTERRUPT_BEGIN static void uart8250_irq4_handler(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    (void)frame;
    uart8250_service(4);
    send_eoi();
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Detect the legacy COM ports and register them with the serial core. */
void init_serial(void)
{
#if !CONFIG_SERIAL
    return;
#endif
    static const uint16_t legacy_bases[UART_MAX_PORTS] = {UART8250_BASE1, UART8250_BASE2, UART8250_BASE3, UART8250_BASE4};
    int                   detected                     = 0;

    for (int i = 0; i < UART_MAX_PORTS; i++) {
        uart_port_t *port  = &uart_8250_driver.ports[i];
        port->number       = i;
        port->ops          = &uart8250_ops;
        port->private_data = (void *)(uintptr_t)legacy_bases[i];
        uint64_t flags     = spin_lock_irqsave(&port->lock);
        int      present   = uart8250_detect(legacy_bases[i]);
        port->present      = present != 0;
        spin_unlock_irqrestore(&port->lock, flags);
        if (present) {
            detected++;
            log_buffer_write(&serial_log, "serial: Port COM%d detected.\n", i + 1);
        }
    }

    uart_register_driver(&uart_8250_driver);
    for (int i = 0; i < UART_MAX_PORTS; i++) {
        if (!uart_8250_driver.ports[i].present) continue;
        (void)uart_add_port(&uart_8250_driver, &uart_8250_driver.ports[i]);
        serial_consoles[i].name    = "ttyS";
        serial_consoles[i].index   = (uint32_t)i;
        serial_consoles[i].data    = &uart_8250_driver.ports[i];
        serial_consoles[i].write   = serial_console_write;
        serial_consoles[i].get_tty = serial_console_get_tty;
        (void)register_console(&serial_consoles[i]);
    }
    log_buffer_write(&serial_log, "serial: %d port(s) available.\n", detected);
}

/* Install IRQ handlers for the legacy COM lines in use. */
void serial_irq_install(void)
{
#if !CONFIG_SERIAL
    return;
#endif
    bool need_irq3 = false;
    bool need_irq4 = false;

    for (int i = 0; i < UART_MAX_PORTS; i++) {
        if (!uart_8250_driver.ports[i].present) continue;
        if (uart8250_irq_of(i) == 3)
            need_irq3 = true;
        else
            need_irq4 = true;
    }
    if (need_irq3) register_interrupt_handler(IRQ_3, (void *)uart8250_irq3_handler, 0, 0x8e);
    if (need_irq4) register_interrupt_handler(IRQ_4, (void *)uart8250_irq4_handler, 0, 0x8e);
}

/* True when the legacy serial port at the given index was detected. */
int serial_port_present(int index)
{
    if (index < 0 || index >= UART_MAX_PORTS) return 0;
    return uart_8250_driver.ports[index].present;
}
