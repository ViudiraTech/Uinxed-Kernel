/*
 *
 *      parallel.h
 *      Parallel port header file
 *
 *      2024/9/8 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PARALLEL_H_
#define INCLUDE_PARALLEL_H_

#include <stdint.h>

/* Register offset */
#define DATA_REG    0 // Data register
#define STATUS_REG  1 // Status register
#define CONTROL_REG 2 // Control register

/* Parallel port I/O */
#define PARALLEL_PORT_1 0x378 // Serial port 1 number.
#define PARALLEL_PORT_2 0x278 // Serial port 2 number.
#define PARALLEL_PORT_3 0x3bc // Serial port 3 number.

#define PORT_TO_LPT(port) ((port) == 0x378 ? "LPT1" : (port) == 0x278 ? "LPT2" : (port) == 0x3bc ? "LPT3" : "Unknown")

/* Initialize parallel port */
void init_parallel(void);

/* Check if the specified parallel port is busy */
int parallel_port_busy(uint16_t port);

/* Write parallel port */
void write_parallel(uint16_t port, uint8_t data);

/* Get the status value of the specified parallel port */
uint8_t get_parallel_status(uint16_t port);

#endif // INCLUDE_PARALLEL_H_
