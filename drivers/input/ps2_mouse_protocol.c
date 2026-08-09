/*
 *
 *      ps2_mouse_protocol.c
 *      PS/2 mouse packet decoder shared by the driver and host tests
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/input/ps2/ps2_mouse.h>
#include <kernel/errno.h>

#define PS2_MOUSE_PACKET_SYNC       0x08
#define PS2_MOUSE_PACKET_X_OVERFLOW 0x40
#define PS2_MOUSE_PACKET_Y_OVERFLOW 0x80

size_t ps2_mouse_packet_size(enum ps2_mouse_protocol protocol)
{
    switch (protocol) {
        case PS2_MOUSE_STANDARD :
            return 3;
        case PS2_MOUSE_WHEEL :
        case PS2_MOUSE_EXPLORER :
            return 4;
        default :
            return 0;
    }
}

int ps2_mouse_decode_packet(enum ps2_mouse_protocol protocol, const uint8_t *raw, struct ps2_mouse_packet *packet)
{
    uint8_t buttons;

    if (!raw || !packet || !ps2_mouse_packet_size(protocol)) return -EINVAL;
    if (!(raw[0] & PS2_MOUSE_PACKET_SYNC)) return -EINVAL;
    if (raw[0] & (PS2_MOUSE_PACKET_X_OVERFLOW | PS2_MOUSE_PACKET_Y_OVERFLOW)) return -EOVERFLOW;

    *packet        = (struct ps2_mouse_packet) {0};
    buttons        = raw[0];
    packet->dx     = (int16_t)((int)raw[1] - ((raw[0] & 0x10) ? 256 : 0));
    packet->dy     = (int16_t)((int)raw[2] - ((raw[0] & 0x20) ? 256 : 0));
    packet->left   = (buttons & 0x01) != 0;
    packet->right  = (buttons & 0x02) != 0;
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

void ps2_mouse_stream_init(struct ps2_mouse_stream *stream, enum ps2_mouse_protocol protocol)
{
    if (!stream) return;
    *stream = (struct ps2_mouse_stream) {.protocol = protocol};
}

int ps2_mouse_stream_byte(struct ps2_mouse_stream *stream, uint8_t byte, struct ps2_mouse_packet *packet)
{
    size_t packet_size;
    int    result;

    if (!stream || !packet) return -EINVAL;
    packet_size = ps2_mouse_packet_size(stream->protocol);
    if (!packet_size) return -EINVAL;
    if (!stream->count && !(byte & PS2_MOUSE_PACKET_SYNC)) return 0;
    stream->bytes[stream->count++] = byte;
    if (stream->count < packet_size) return 0;
    stream->count = 0;
    result        = ps2_mouse_decode_packet(stream->protocol, stream->bytes, packet);
    return result == EOK ? 1 : result;
}
