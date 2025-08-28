/*
 *
 *      serial.h
 *      Serial Port Header File
 *
 *      2024/7/11 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_SERIAL_H_
#define INCLUDE_SERIAL_H_

#include "stdint.h"

/* Register offset */
#define SERIAL_REG_DATA 0 // Data Register
#define SERIAL_REG_IER 1  // Interrupt Enable Register
#define SERIAL_REG_FCR 2  // FIFO Control Register
#define SERIAL_REG_LCR 3  // Line Control Register
#define SERIAL_REG_MCR 4  // Modem Control Registers
#define SERIAL_REG_LSR 5  // Line Status Register

/* Serial port I/O */
#define SERIAL_PORT_1 0x3f8 // Serial port 1 number.
#define SERIAL_PORT_2 0x2f8 // Serial port 2 number.
#define SERIAL_PORT_3 0x3e8 // Serial port 3 number.
#define SERIAL_PORT_4 0x2e8 // Serial port 4 number.

#define PORT_TO_COM(port)                                                      \
  ((port) == 0x3f8   ? "COM1"                                                  \
   : (port) == 0x2f8 ? "COM2"                                                  \
   : (port) == 0x3e8 ? "COM3"                                                  \
   : (port) == 0x2e8 ? "COM4"                                                  \
                     : "Unknown")

#ifndef SERIAL_PARITY
#define SERIAL_PARITY 0
#endif

#ifndef SERIAL_BAUD_RATE
#define SERIAL_BAUD_RATE 9600
#endif

#ifndef SERIAL_DATA_BITS
#define SERIAL_DATA_BITS 8
#endif

#ifndef SERIAL_STOP_BITS
#define SERIAL_STOP_BITS 1
#endif

void init_serial(void); // Initialize the serial port
int serial_received(
    uint16_t port); // Check whether the serial port is ready to read
int is_transmit_empty(uint16_t port); // Check whether the serial port is idle
uint8_t read_serial(uint16_t port);   // Read serial port
void write_serial(uint16_t port, uint8_t data); // Write serial port
uint8_t get_serial_status(
    uint16_t port); // Get the status value of the specified serial port

#endif // INCLUDE_SERIAL_H_
