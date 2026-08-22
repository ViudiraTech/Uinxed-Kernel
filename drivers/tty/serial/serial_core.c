/*
 *
 *      serial_core.c
 *      UART serial core: tty integration for serial ports
 *
 *      2026/8/10 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/tty/serial/serial_core.h>
#include <drivers/tty/tty_core.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <syscall/fcntl.h>

static uart_driver_t *serial_uart_driver;
static bool           serial_cores_inited;
static spinlock_t     serial_core_lock;

static tty_file_endpoint_t  serial_endpoints[UART_MAX_PORTS];
static int                  serial_tty_emit(void *context, const uint8_t *data, size_t size, uint64_t flags);
static const tty_core_ops_t serial_operations = {.emit = serial_tty_emit, .event = NULL};

/* Initialize a port's locks and tty core; idempotent, call with the lock held. */
static void serial_core_init_port_locked(uart_port_t *port)
{
    if (!port) return;
    if (!port->tty) port->tty = &port->tty_core;
    if (port->tty_initialized) return;
    port->lock.lock      = 0;
    port->lock.rflags    = 0;
    port->rx_lock.lock   = 0;
    port->rx_lock.rflags = 0;
    tty_core_init(port->tty, &serial_operations, port);
    port->tty_initialized = true;
}

/* tty_core emit callback: push bytes out through the UART. */
static int serial_tty_emit(void *context, const uint8_t *data, size_t size, uint64_t flags)
{
    uart_port_t *port = context;
    (void)flags;
    if (!port || !port->ops || !port->ops->tx_write) return -EIO;
    return uart_write(port, data, size);
}

/* Initialize the tty cores of all present serial ports (once). */
static void serial_cores_init(void)
{
    spin_lock(&serial_core_lock);
    if (serial_cores_inited || !serial_uart_driver) {
        spin_unlock(&serial_core_lock);
        return;
    }
    for (int i = 0; i < serial_uart_driver->nr; i++) {
        uart_port_t *port = &serial_uart_driver->ports[i];
        if (!port->present) continue;
        serial_core_init_port_locked(port);
        serial_endpoints[i].core            = port->tty;
        serial_endpoints[i].virtual_console = false;
    }
    serial_cores_inited = true;
    spin_unlock(&serial_core_lock);
}

/* Open one file reference without shutting down a port used by another CPU. */
int uart_port_open(uart_port_t *port)
{
    int result = 0;

    if (!port) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&port->lock);
    if (!port->hw_started) {
        if (port->ops && port->ops->startup) result = port->ops->startup(port);
        if (!result) port->hw_started = true;
    }
    if (!result) port->open_count++;
    spin_unlock_irqrestore(&port->lock, flags);
    return result;
}

/* Release one file reference; the last close shuts down an unshared port. */
void uart_port_close(uart_port_t *port)
{
    if (!port) return;
    uint64_t flags = spin_lock_irqsave(&port->lock);
    if (port->open_count) port->open_count--;
    if (!port->open_count && port->hw_started && !port->console_started) {
        if (port->ops && port->ops->shutdown) port->ops->shutdown(port);
        port->hw_started = false;
    }
    spin_unlock_irqrestore(&port->lock, flags);
}

/* The console is a persistent user of the port and shares the same lock. */
int uart_port_console_startup(uart_port_t *port)
{
    int result = 0;

    if (!port) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&port->lock);
    if (!port->hw_started) {
        if (port->ops && port->ops->startup) result = port->ops->startup(port);
        if (!result) port->hw_started = true;
    }
    if (!result) port->console_started = true;
    spin_unlock_irqrestore(&port->lock, flags);
    return result;
}

/* tty open: start the port and acquire the tty core. */
static int serial_tty_open(tty_driver_t *drv, int index, uint64_t flags, void **private_data)
{
    uart_port_t *port;
    (void)drv;

    if (!serial_uart_driver || index < 0 || index >= serial_uart_driver->nr) return -ENXIO;
    port = &serial_uart_driver->ports[index];
    serial_cores_init();
    if (!port->present || !port->tty) return -ENXIO;
    int startup = uart_port_open(port);
    if (startup) return startup;
    tty_core_auto_acquire(port->tty, flags);
    *private_data = &serial_endpoints[index];
    return 0;
}

/* tty release: shut the port down. */
static int serial_tty_release(tty_driver_t *drv, int index, void *private_data)
{
    uart_port_t *port;
    (void)drv;
    (void)private_data;
    if (!serial_uart_driver || index < 0 || index >= serial_uart_driver->nr) return -EINVAL;
    port = &serial_uart_driver->ports[index];
    uart_port_close(port);
    return 0;
}

/* tty read: read from the serial tty core. */
static int64_t serial_tty_read(tty_driver_t *drv, int index, void *private_data, uint64_t flags, void *addr, size_t size)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    return ep && ep->core ? tty_core_read(ep->core, addr, size, flags) : -ENXIO;
}

/* tty write: write to the serial tty core. */
static int64_t serial_tty_write(tty_driver_t *drv, int index, void *private_data, uint64_t flags, const void *addr, size_t size)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    return ep && ep->core ? tty_core_write(ep->core, addr, size, flags) : -ENXIO;
}

/* tty ioctl: forward to the terminal ioctl handler. */
static int serial_tty_ioctl(tty_driver_t *drv, int index, void *private_data, uint64_t flags, size_t req, void *arg)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    if (!ep || !ep->core) return -ENXIO;
    return tty_core_ioctl_terminal(ep->core, flags, req, arg, ep->virtual_console);
}

/* tty poll: report the serial tty core's readiness. */
static int serial_tty_poll(tty_driver_t *drv, int index, void *private_data, uint64_t flags, size_t events)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    (void)flags;
    return ep && ep->core ? tty_core_poll(ep->core, events) : 0;
}

/* Register a UART driver with the tty subsystem. */
void uart_register_driver(uart_driver_t *drv)
{
    if (!drv) return;
    drv->tty_drv.name        = drv->name;
    drv->tty_drv.major       = drv->major;
    drv->tty_drv.minor_start = drv->minor_start;
    drv->tty_drv.num         = (uint32_t)drv->nr;
    drv->tty_drv.node_type   = file_stream;
    drv->tty_drv.mode        = 0;
    drv->tty_drv.open        = serial_tty_open;
    drv->tty_drv.release     = serial_tty_release;
    drv->tty_drv.read        = serial_tty_read;
    drv->tty_drv.write       = serial_tty_write;
    drv->tty_drv.ioctl       = serial_tty_ioctl;
    drv->tty_drv.poll        = serial_tty_poll;

    spin_lock(&serial_core_lock);
    serial_uart_driver = drv;
    spin_unlock(&serial_core_lock);
    tty_register_driver(&drv->tty_drv);
}

/* Publish one serial port as a /dev/ttyS<N> device. */
int uart_add_port(uart_driver_t *drv, uart_port_t *port)
{
    char name[16];

    if (!drv || !port || port->number < 0 || port->number >= drv->nr || drv->nr > UART_MAX_PORTS) return -EINVAL;
    spin_lock(&serial_core_lock);
    serial_core_init_port_locked(port);
    serial_endpoints[port->number].core            = port->tty;
    serial_endpoints[port->number].virtual_console = false;
    spin_unlock(&serial_core_lock);
    if (!port->present) return 0;
    (void)snprintf(name, sizeof(name), "ttyS%d", port->number);
    return tty_register_device(&drv->tty_drv, port->number, name);
}

/* Queue one received byte and feed it to the tty core. */
void uart_insert_char(uart_port_t *port, uint8_t ch)
{
    if (!port) return;
    spin_lock(&port->rx_lock);
    if (port->rx_count < UART_RX_BUF_SIZE) {
        port->rx_buf[port->rx_head] = ch;
        port->rx_head               = (port->rx_head + 1) % UART_RX_BUF_SIZE;
        port->rx_count++;
    }
    spin_unlock(&port->rx_lock);
    tty_core_t *tty = port->tty;
    if (tty) tty_core_receive(tty, &ch, 1, O_NONBLOCK);
}

/* Transmit bytes through the port's hardware ops. */
int uart_write(uart_port_t *port, const uint8_t *data, size_t len)
{
    if (!port || !port->ops || !port->ops->tx_write) return -EIO;
    uint64_t flags  = spin_lock_irqsave(&port->lock);
    int      result = port->ops->tx_write(port, data, len);
    spin_unlock_irqrestore(&port->lock, flags);
    return result;
}

/*
 * Invoke the polling console callback in one-byte critical sections.  The
 * old implementation held IRQs disabled for the complete printk record; at
 * 9600 baud that is about 1 ms per byte and can postpone input/timer IRQs by
 * hundreds of milliseconds.  Byte-sized locking preserves register safety
 * while placing a hard bound on irq-off time.
 */
void uart_console_write(uart_port_t *port, const uint8_t *data, size_t len)
{
    if (!port || !port->ops || !port->ops->console_write) return;
    for (size_t i = 0; i < len; i++) {
        uint64_t flags = spin_lock_irqsave(&port->lock);
        port->ops->console_write(port, &data[i], 1);
        spin_unlock_irqrestore(&port->lock, flags);
    }
}

/* Look up the tty core for a serial port without opening it. */
int serial_tty_core(int index, tty_core_t **core)
{
    if (!core || !serial_uart_driver || index < 0 || index >= serial_uart_driver->nr) return -ENXIO;
    serial_cores_init();
    if (!serial_uart_driver->ports[index].present) return -ENXIO;
    *core = serial_uart_driver->ports[index].tty;
    return 0;
}
