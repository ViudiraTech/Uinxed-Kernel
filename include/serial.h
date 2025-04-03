/*
 *
 *		serial.h
 *		Serial Port Header File
 *
 *		2024/7/11 By MicroFish
 *		Based on GPL-3.0 open source agreement
 *		Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_SERIAL_H_
#define INCLUDE_SERIAL_H_

#define SERIAL_PORT 0x3f8					// By default, COM1 serial port is used.

void init_serial(int baud_rate);			// Initialize the serial port
int serial_received(void);					// Check whether the serial port is ready to read
int is_transmit_empty(void);				// Check whether the serial port is idle
char read_serial(void);						// Read serial port
void write_serial(char c);					// Write serial port

#endif // INCLUDE_SERIAL_H_
