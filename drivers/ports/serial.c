/*
 *
 *      serial.c
 *      Serial port
 *
 *      2024/7/11 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <common.h>
#include <serial.h>
#include <stdint.h>

static int serial_initialized   = 0;
static int serial_port_ready[4] = {0};

static int serial_port_index(uint16_t port)
{
    switch (port) {
        case SERIAL_PORT_1 :
            return 0;
        case SERIAL_PORT_2 :
            return 1;
        case SERIAL_PORT_3 :
            return 2;
        case SERIAL_PORT_4 :
            return 3;
        default :
            return -1;
    }
}

/* Serial port LCR data configuration */
static uint8_t serial_calculate_lcr(void)
{
    uint8_t lcr = 0;

    switch (SERIAL_DATA_BITS) {
        case 5 :
            lcr |= 0x00;
            break;
        case 6 :
            lcr |= 0x01;
            break;
        case 7 :
            lcr |= 0x02;
            break;
        case 8 :
            lcr |= 0x03;
            break;
        default :
            lcr |= 0x03;
    }

    if (SERIAL_STOP_BITS == 2) lcr |= 0x04;

    switch (SERIAL_PARITY) {
        case 0 : // No parity
            lcr |= 0x00;
            break;
        case 1 : // Odd parity
            lcr |= 0x08;
            break;
        case 2 : // Even parity
            lcr |= 0x18;
            break;
        case 3 : // Mark parity
            lcr |= 0x28;
            break;
        case 4 : // Space parity
            lcr |= 0x38;
            break;
        default :
            lcr |= 0x00;
    }
    return lcr;
}

/* Initialize the specified serial port */
static int init_serial_port(uint16_t port)
{
    uint16_t divisor = 115200 / SERIAL_BAUD_RATE;
    int      index   = serial_port_index(port);

    if (index < 0) return 0;

    outb(port + SERIAL_REG_IER, 0x00);                   // Disable COM interrupts
    outb(port + SERIAL_REG_LCR, 0x80);                   // Enable DLAB (set baud rate divisor)
    outb(port + SERIAL_REG_DATA, divisor & 0xff);        // Set low baud rate
    outb(port + SERIAL_REG_IER, (divisor >> 8) & 0xff);  // Set high baud rate
    outb(port + SERIAL_REG_LCR, serial_calculate_lcr()); // Set LCR
    outb(port + SERIAL_REG_FCR, 0xcf);                   // Enable FIFO with 14-byte threshold
    outb(port + SERIAL_REG_MCR, 0x0f);                   // Enable IRQ, set RTS/DSR
    outb(port + SERIAL_REG_MCR, 0x1e);                   // Set to loopback mode and test the serial port
    outb(port + SERIAL_REG_DATA, 0xae);                  // Test serial port

    /* Check if there is a problem with the serial port */
    if (inb(port + SERIAL_REG_DATA) != 0xae) { return 0; }
    outb(port + SERIAL_REG_MCR, 0x0f); // Quit loopback mode
    serial_port_ready[index] = 1;
    return 1;
}

/* Initialize the serial port */
void init_serial(void)
{
    if (serial_initialized) return;

    uint16_t com_ports[4] = {SERIAL_PORT_1, SERIAL_PORT_2, SERIAL_PORT_3, SERIAL_PORT_4};
    int      valid_ports  = 0;

    for (int i = 0; i < 4; i++)
        if (init_serial_port(com_ports[i])) valid_ports++;
    (void)valid_ports;
    serial_initialized = 1;
}

/* Check whether the serial port is ready to read */
int serial_received(uint16_t port)
{
    return inb(port + SERIAL_REG_LSR) & 1;
}

/* Check whether the serial port is idle */
int is_transmit_empty(uint16_t port)
{
    return inb(port + SERIAL_REG_LSR) & 0x20;
}

/* Read serial port */
uint8_t read_serial(uint16_t port)
{
    while (!serial_received(port));
    return inb(port + SERIAL_REG_DATA);
}

/* Write serial port */
void write_serial(uint16_t port, uint8_t data)
{
    int index = serial_port_index(port);

    if (index < 0 || !serial_port_ready[index]) return;

    for (uint32_t timeout = 100000; timeout && !is_transmit_empty(port); --timeout);
    if (!is_transmit_empty(port)) return;
    outb(port + SERIAL_REG_DATA, data);
}

/* Get the status value of the specified serial port */
uint8_t get_serial_status(uint16_t port)
{
    return inb(port + SERIAL_REG_LSR);
}
