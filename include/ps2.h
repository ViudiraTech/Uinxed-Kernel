/*
 *
 *      ps2.h
 *      PS/2 controller driver header file
 *
 *      2025/9/7 By MicroFish
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef INCLUDE_PS2_H_
#define INCLUDE_PS2_H_

#include "stdint.h"

#define PS2_DATA_PORT 0x60
#define PS2_CMD_PORT  0x64

/* Waiting for PS/2 read idle */
void wait_ps2_read(void);

/* Waiting for PS/2 write idle */
void wait_ps2_write(void);

/* Read PS/2 data */
uint8_t ps2_read_data(void);

/* Read PS/2 command */
uint8_t ps2_read_cmd(void);

/* Sending data to PS/2 */
void ps2_write_data(uint8_t data);

/* Sending commands to PS/2 */
void ps2_write_cmd(uint8_t cmd);

#endif // INCLUDE_PS2_H_
