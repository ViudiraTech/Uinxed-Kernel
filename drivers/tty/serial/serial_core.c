/*
 *
 *      serial_core.c
 *      UART serial core: tty integration for serial ports
 *
 *      2026/8/10 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/tty/serial/serial_core.h>
#include <drivers/tty/tty_core.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <syscall/fcntl.h>

static uart_driver_t *serial_uart_driver;
static bool           serial_cores_inited;

static tty_file_endpoint_t serial_endpoints[UART_MAX_PORTS];

static int serial_tty_emit(void *context, const uint8_t *data, size_t size, uint64_t flags)
{
    uart_port_t *port = context;
    (void)flags;
    if (!port || !port->ops || !port->ops->tx_write) return -EIO;
    return port->ops->tx_write(port, data, size);
}

static void serial_cores_init(void)
{
    if (serial_cores_inited || !serial_uart_driver) return;
    static const tty_core_ops_t serial_operations = {.emit = serial_tty_emit, .event = NULL};
    for (int i = 0; i < serial_uart_driver->nr; i++) {
        uart_port_t *port = &serial_uart_driver->ports[i];
        if (!port->present) continue;
        tty_core_init(port->tty, &serial_operations, port);
        serial_endpoints[i].core            = port->tty;
        serial_endpoints[i].virtual_console = false;
    }
    serial_cores_inited = true;
}

static int serial_tty_open(tty_driver_t *drv, int index, uint64_t flags, void **private_data)
{
    uart_port_t *port;
    (void)drv;

    if (!serial_uart_driver || index < 0 || index >= serial_uart_driver->nr) return -ENXIO;
    port = &serial_uart_driver->ports[index];
    serial_cores_init();
    if (!port->present || !port->tty) return -ENXIO;
    if (port->ops && port->ops->startup) port->ops->startup(port);
    tty_core_auto_acquire(port->tty, flags);
    *private_data = &serial_endpoints[index];
    return 0;
}

static int serial_tty_release(tty_driver_t *drv, int index, void *private_data)
{
    uart_port_t *port;
    (void)drv;
    (void)private_data;
    if (!serial_uart_driver || index < 0 || index >= serial_uart_driver->nr) return -EINVAL;
    port = &serial_uart_driver->ports[index];
    if (port->ops && port->ops->shutdown) port->ops->shutdown(port);
    return 0;
}

static int64_t serial_tty_read(tty_driver_t *drv, int index, void *private_data, uint64_t flags, void *addr, size_t size)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    return ep && ep->core ? tty_core_read(ep->core, addr, size, flags) : -ENXIO;
}

static int64_t serial_tty_write(tty_driver_t *drv, int index, void *private_data, uint64_t flags, const void *addr, size_t size)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    return ep && ep->core ? tty_core_write(ep->core, addr, size, flags) : -ENXIO;
}

static int serial_tty_ioctl(tty_driver_t *drv, int index, void *private_data, uint64_t flags, size_t req, void *arg)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    if (!ep || !ep->core) return -ENXIO;
    return tty_core_ioctl_terminal(ep->core, flags, req, arg, ep->virtual_console);
}

static int serial_tty_poll(tty_driver_t *drv, int index, void *private_data, uint64_t flags, size_t events)
{
    tty_file_endpoint_t *ep = private_data;
    (void)drv;
    (void)index;
    (void)flags;
    return ep && ep->core ? tty_core_poll(ep->core, events) : 0;
}

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

    serial_uart_driver = drv;
    tty_register_driver(&drv->tty_drv);
}

int uart_add_port(uart_driver_t *drv, uart_port_t *port)
{
    char name[16];

    if (!drv || !port || port->number < 0 || port->number >= drv->nr) return -EINVAL;
    port->tty = &port->tty_core;
    if (!port->present) return 0;
    (void)snprintf(name, sizeof(name), "ttyS%d", port->number);
    return tty_register_device(&drv->tty_drv, port->number, name);
}

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
    if (port->tty) tty_core_receive(port->tty, &ch, 1, O_NONBLOCK);
}

int uart_write(uart_port_t *port, const uint8_t *data, size_t len)
{
    if (!port || !port->ops || !port->ops->tx_write) return -EIO;
    return port->ops->tx_write(port, data, len);
}

int serial_tty_core(int index, tty_core_t **core)
{
    if (!core || !serial_uart_driver || index < 0 || index >= serial_uart_driver->nr) return -ENXIO;
    serial_cores_init();
    if (!serial_uart_driver->ports[index].present) return -ENXIO;
    *core = serial_uart_driver->ports[index].tty;
    return 0;
}
