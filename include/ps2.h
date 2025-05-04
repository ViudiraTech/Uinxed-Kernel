/*
 *
 *      ps2.h
 *      Driver of PS/2 devices
 *
 *      2025/5/4 By XSlime (W9pi3cZ1)
 *      Based on GPL-3.0 open source agreement
 *      Copyright © 2020 ViudiraTech, based on the GPLv3 agreement.
 *
 */

#ifndef PS2_H
#define PS2_H

/* LED Signal */
#define LED_SCROLL_LOCK 1 << 0
#define LED_NUM_LOCK    1 << 1
#define LED_CAPS_LOCK   1 << 2

/* PS/2 Ports */
#define PS2_CMD_PORT  0x64
#define PS2_DATA_PORT 0x60

/* Initializes the PS/2 controller */
void init_ps2();

/* Updates the LED state based on the current state of the keyboard */
void update_led_state();

/* Sets the state of a specific LED */
void kbd_led_set(int led_id, int state);

/* Gets the state of a specific LED */
int kbd_led_get(int led_id);

/* Registers a handler function for keyboard events */
void register_kbd_handler(void (*handle)(int scan_code, int ascii_code, int is_extended, int is_released));

/* Checks if a specific key is currently being held down */
int is_key_holding(int scancode);

#endif /* PS2_H */