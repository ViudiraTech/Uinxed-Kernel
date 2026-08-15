/*
 *
 *      ps2.h
 *      i8042 PS/2 controller and attached keyboard/mouse driver interface
 *
 *      2026/7/25 By MicroFish & JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PS2_H_
#define INCLUDE_PS2_H_

#include <drivers/input/evdev/evdev.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>

#define PS2_DATA_PORT   0x60
#define PS2_STATUS_PORT 0x64

#define PS2_CMD_DISABLE_PORT1 0xad
#define PS2_CMD_DISABLE_PORT2 0xa7
#define PS2_CMD_ENABLE_PORT1  0xae
#define PS2_CMD_ENABLE_PORT2  0xa8
#define PS2_CMD_READ_CONFIG   0x20
#define PS2_CMD_WRITE_CONFIG  0x60
#define PS2_CMD_SELF_TEST     0xaa
#define PS2_CMD_TEST_PORT1    0xab
#define PS2_CMD_TEST_PORT2    0xa9
#define PS2_CMD_WRITE_PORT2   0xd4

#define PS2_STATUS_OUTPUT_FULL 0x01
#define PS2_STATUS_INPUT_FULL  0x02
#define PS2_STATUS_AUX_DATA    0x20

#define PS2_CONFIG_PORT1_IRQ   0x01
#define PS2_CONFIG_PORT2_IRQ   0x02
#define PS2_CONFIG_PORT1_CLOCK 0x10
#define PS2_CONFIG_PORT2_CLOCK 0x20
#define PS2_CONFIG_TRANSLATION 0x40

#define PS2_RESPONSE_TEST     0x00
#define PS2_RESPONSE_OK       0xfa
#define PS2_RESPONSE_SELFTEST 0x55
#define PS2_RESPONSE_RESET_OK 0xaa

#define PS2_DEV_RESET           0xff
#define PS2_DEV_DISABLE_REPORT  0xf5
#define PS2_DEV_ENABLE_REPORT   0xf4
#define PS2_DEV_SET_SAMPLE_RATE 0xf3
#define PS2_DEV_GET_ID          0xf2
#define PS2_DEV_SET_LEDS        0xed

/* Poll the data port; returns the byte read or -1 on timeout. */
int wait_ps2_read(void);

/* Poll until the write buffer is free; returns -1 on timeout. */
int wait_ps2_write(void);

/* Read one byte from the PS/2 data port. */
uint8_t ps2_read_data(void);

/* Read the controller status register. */
uint8_t ps2_read_status(void);

/* Read the controller configuration byte. */
uint8_t ps2_read_config(void);

/* Write one byte to the PS/2 data port. */
void ps2_write_data(uint8_t data);

/* Issue a command byte to the i8042 controller. */
void ps2_write_cmd(uint8_t cmd);

/* Write the controller configuration byte. */
void ps2_write_config(uint8_t config);

/* Read data with a timeout; returns 0 on success or a negative errno. */
int ps2_read_data_timeout(uint8_t *data);

/* Send a device command through the selected port. */
int ps2_send_device_command(bool second_port, uint8_t command);

/* Send raw data to the selected port. */
int ps2_send_device_data(bool second_port, uint8_t data);

/* True if the given PS/2 port exists and is usable. */
bool ps2_port_available(bool second_port);

/* Probe and initialize the i8042 controller and attached devices. */
void init_ps2(void);

/* Initialize the PS/2 keyboard and register its evdev device. */
void ps2_keyboard_init(void);

/* Feed one scancode byte to the keyboard decoder. */
void ps2_keyboard_handle_byte(uint8_t scancode);

/* Block until keyboard events are ready (worker loop helper). */
int             ps2kbd_wait_events(void);
extern evdev_t *ps2_keyboard_evdev;

/* Initialize the PS/2 mouse and register its evdev device. */
void ps2_mouse_init(void);

/* Feed one PS/2 packet byte to the mouse decoder. */
void ps2_mouse_handle_byte(uint8_t byte);

/* True if a PS/2 mouse was detected on the aux port. */
bool            ps2_mouse_available(void);
extern evdev_t *ps2_mouse_evdev;

#endif // INCLUDE_PS2_H_
