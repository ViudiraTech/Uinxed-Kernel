/*
 *
 *      serial.c
 *      Serial Port
 *
 *      2024/7/11 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#include "serial.h"
#include "common.h"
#include "printk.h"
#include "stdint.h"

/* Initialize the serial port */
void init_serial(void)
{
    uint16_t divisor = 115200 / 9600;

    outb(SERIAL_PORT + 1, 0x00);                  // Disable COM interrupts
    outb(SERIAL_PORT + 3, 0x80);                  // Enable DLAB (set baud rate divisor)
    outb(SERIAL_PORT + 0, divisor & 0xFF);        // Set low baud rate
    outb(SERIAL_PORT + 1, (divisor >> 8) & 0xFF); // Set high baud rate
    outb(SERIAL_PORT + 3, 0x03);                  // 8 bits, no parity, one stop bit
    outb(SERIAL_PORT + 2, 0xC7);                  // Enable FIFO with 14-byte threshold
    outb(SERIAL_PORT + 4, 0x0B);                  // Enable IRQ, set RTS/DSR
    outb(SERIAL_PORT + 4, 0x1E);                  // Set to loopback mode and test the serial port
    outb(SERIAL_PORT + 0, 0xAE);                  // Test serial port

    /* Check if there is a problem with the serial port (ie: the bytes sent are different) */
    if (inb(SERIAL_PORT + 0) != 0xAE) {
        plogk("Serial: Serial port test failed.\n");
        return;
    }

    /* If the serial port is not faulty, set it to normal operation mode */
    /* (non-loopback, IRQ enabled, OUT#1 and OUT#2 bits enabled) */
    outb(SERIAL_PORT + 4, 0x0F);
    plogk("Serial: Local port = COM1\n");
    plogk("Serial: Baud rate = %d\n", 9600);
}

/* Check whether the serial port is ready to read */
int serial_received(void)
{
    return inb(SERIAL_PORT + 5) & 1;
}

/* Check whether the serial port is idle */
int is_transmit_empty(void)
{
    return inb(SERIAL_PORT + 5) & 0x20;
}

/* Read serial port */
char read_serial(void)
{
    while (serial_received() == 0);
    return (char)inb(SERIAL_PORT);
}

/* Write serial port */
void write_serial(const char c)
{
    while (is_transmit_empty() == 0);
    outb(SERIAL_PORT, c);
}
