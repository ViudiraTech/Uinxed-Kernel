/*
 *
 *      ps2_mouse.h
 *      PS/2 mouse packet protocol helpers
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_PS2_MOUSE_H_
#define INCLUDE_PS2_MOUSE_H_

#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

enum ps2_mouse_protocol {
    PS2_MOUSE_STANDARD,
    PS2_MOUSE_WHEEL,
    PS2_MOUSE_EXPLORER,
};

struct ps2_mouse_packet {
        int16_t dx;
        int16_t dy;
        int8_t  wheel;
        bool    left;
        bool    right;
        bool    middle;
        bool    side;
        bool    extra;
};

struct ps2_mouse_stream {
        enum ps2_mouse_protocol protocol;
        uint8_t                 bytes[4];
        size_t                  count;
};

size_t ps2_mouse_packet_size(enum ps2_mouse_protocol protocol);
int    ps2_mouse_decode_packet(enum ps2_mouse_protocol protocol, const uint8_t *raw, struct ps2_mouse_packet *packet);
void   ps2_mouse_stream_init(struct ps2_mouse_stream *stream, enum ps2_mouse_protocol protocol);
int    ps2_mouse_stream_byte(struct ps2_mouse_stream *stream, uint8_t byte, struct ps2_mouse_packet *packet);

#endif // INCLUDE_PS2_MOUSE_H_
