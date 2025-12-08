/*
 *
 *      ps2.h
 *      PS/2 controller driver header file
 *
 *      2025/9/7 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PS2_H_
#define INCLUDE_PS2_H_

#include <stdint.h>

/* PS/2 port */
#define PS2_DATA_PORT   0x60 // Data port
#define PS2_STATUS_PORT 0x64 // Status/command port

/* PS/2 command */
#define PS2_CMD_DISABLE_PORT1 0xad // Disable port 1
#define PS2_CMD_DISABLE_PORT2 0xa7 // Disable port 2
#define PS2_CMD_ENABLE_PORT1  0xae // Enable port 1
#define PS2_CMD_ENABLE_PORT2  0xa8 // Enable port 2
#define PS2_CMD_READ_CONFIG   0x20 // Read configuration byte
#define PS2_CMD_WRITE_CONFIG  0x60 // Write configuration byte
#define PS2_CMD_SELF_TEST     0xaa // Controller self-test
#define PS2_CMD_TEST_PORT1    0xab // Test port 1
#define PS2_CMD_TEST_PORT2    0xa9 // Test port 2

/* Device command */
#define PS2_DEV_RESET 0xff // Device reset command
#define PS2_DEVICE_2  0xd4 // Select device 2

/* Status register bit */
#define PS2_STATUS_OUTPUT_FULL 0x01 // Output buffer full
#define PS2_STATUS_INPUT_FULL  0x02 // Input buffer full

/* Configuration byte bits */
#define PS2_CONFIG_PORT1_IRQ   0x01 // Port 1 interrupt enable
#define PS2_CONFIG_PORT2_IRQ   0x02 // Port 2 interrupt enable
#define PS2_CONFIG_POST_PASSED 0x20 // POST completion flag
#define PS2_CONFIG_PORT2_CLOCK 0x40 // Port 2 clock enable
#define PS2_CONFIG_PORT1_CLOCK 0x80 // Port 1 clock enable

/* Response code */
#define PS2_RESPONSE_TEST     0x00 // Port self-test successful
#define PS2_RESPONSE_OK       0xfa // Command confirmation
#define PS2_RESPONSE_SELFTEST 0x55 // Self-test successful
#define PS2_RESPONSE_RESET_OK 0xaa // Device reset successful

/* Waiting for PS/2 read ready */
int wait_ps2_read(void);

/* Waiting for PS/2 write ready */
int wait_ps2_write(void);

/* Read PS/2 data */
uint8_t ps2_read_data(void);

/* Read PS/2 status */
uint8_t ps2_read_status(void);

/* Read PS/2 configuration */
uint8_t ps2_read_config(void);

/* Sending data to PS/2 */
void ps2_write_data(uint8_t data);

/* Sending commands to PS/2 */
void ps2_write_cmd(uint8_t cmd);

/* Sending configuration to PS/2 */
void ps2_write_config(uint8_t config);

/* Initialize the PS/2 controller */
void init_ps2(void);

#endif // INCLUDE_PS2_H_
