/*
 * PS/2 mouse packet decoder shared by the mouse driver and host tests.
 */

#include <drivers/ps2_mouse.h>
#include <kernel/errno.h>
#include <libs/std/string.h>

#define PS2_MOUSE_PACKET_SYNC       0x08
#define PS2_MOUSE_PACKET_X_OVERFLOW 0x40
#define PS2_MOUSE_PACKET_Y_OVERFLOW 0x80

size_t ps2_mouse_packet_size(enum ps2_mouse_protocol protocol)
{
    switch (protocol) {
        case PS2_MOUSE_STANDARD : return 3;
        case PS2_MOUSE_WHEEL :
        case PS2_MOUSE_EXPLORER : return 4;
        default : return 0;
    }
}

int ps2_mouse_decode_packet(enum ps2_mouse_protocol protocol, const uint8_t *raw, struct ps2_mouse_packet *packet)
{
    uint8_t buttons;

    if (!raw || !packet || !ps2_mouse_packet_size(protocol)) return -EINVAL;
    if (!(raw[0] & PS2_MOUSE_PACKET_SYNC)) return -EINVAL;
    if (raw[0] & (PS2_MOUSE_PACKET_X_OVERFLOW | PS2_MOUSE_PACKET_Y_OVERFLOW)) return -EOVERFLOW;

    memset(packet, 0, sizeof(*packet));
    buttons       = raw[0];
    packet->dx    = (int8_t)raw[1];
    packet->dy    = (int8_t)raw[2];
    packet->left  = (buttons & 0x01) != 0;
    packet->right = (buttons & 0x02) != 0;
    packet->middle = (buttons & 0x04) != 0;

    if (protocol != PS2_MOUSE_STANDARD) {
        packet->wheel = (int8_t)(raw[3] & 0x0f);
        if (packet->wheel & 0x08) packet->wheel -= 0x10;
    }
    if (protocol == PS2_MOUSE_EXPLORER) {
        packet->side  = (raw[3] & 0x10) != 0;
        packet->extra = (raw[3] & 0x20) != 0;
    }

    return EOK;
}
