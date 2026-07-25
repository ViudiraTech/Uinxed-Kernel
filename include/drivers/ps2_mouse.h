/*
 * PS/2 mouse packet protocol helpers.
 */

#ifndef INCLUDE_DRIVERS_PS2_MOUSE_H_
#define INCLUDE_DRIVERS_PS2_MOUSE_H_

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

size_t ps2_mouse_packet_size(enum ps2_mouse_protocol protocol);
int    ps2_mouse_decode_packet(enum ps2_mouse_protocol protocol, const uint8_t *raw, struct ps2_mouse_packet *packet);

#endif /* INCLUDE_DRIVERS_PS2_MOUSE_H_ */
