/*
 *
 *      ps2_keyboard.h
 *      PS/2 keyboard scan-set-1 decoder definitions
 *
 *      2026/7/26 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */
#ifndef INCLUDE_PS2_KEYBOARD_H_
#define INCLUDE_PS2_KEYBOARD_H_

#include <drivers/input/input_event.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>

typedef struct {
        uint8_t pause_index;
        bool    extended;
} ps2_keyboard_decoder_t;

typedef struct {
        uint16_t scan;
        uint16_t keycode;
        bool     pressed;
        bool     auto_release;
} ps2_key_event_t;

void     ps2_keyboard_decoder_init(ps2_keyboard_decoder_t *decoder);
int      ps2_keyboard_decode_byte(ps2_keyboard_decoder_t *decoder, uint8_t byte, ps2_key_event_t *event);
uint16_t ps2_keyboard_keycode_for_scancode(uint16_t scan);

#endif /* INCLUDE_PS2_KEYBOARD_H_ */
