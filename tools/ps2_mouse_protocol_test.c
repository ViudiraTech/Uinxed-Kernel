#include <drivers/ps2_mouse.h>
#include <stdio.h>

static int test_standard_packet(void)
{
    const uint8_t           raw[] = {0x29, 0x05, 0xfd};
    struct ps2_mouse_packet packet;

    if (ps2_mouse_decode_packet(PS2_MOUSE_STANDARD, raw, &packet) != 0) return 1;
    return packet.dx != 5 || packet.dy != -3 || !packet.left || packet.right || packet.middle;
}

static int test_wheel_packet(void)
{
    const uint8_t           raw[] = {0x08, 0x00, 0x00, 0x0f};
    struct ps2_mouse_packet packet;

    if (ps2_mouse_decode_packet(PS2_MOUSE_WHEEL, raw, &packet) != 0) return 1;
    return packet.wheel != -1;
}

static int test_explorer_packet(void)
{
    const uint8_t           raw[] = {0x08, 0x00, 0x00, 0x31};
    struct ps2_mouse_packet packet;

    if (ps2_mouse_decode_packet(PS2_MOUSE_EXPLORER, raw, &packet) != 0) return 1;
    return packet.wheel != 1 || !packet.side || !packet.extra;
}

static int test_rejects_malformed_packets(void)
{
    const uint8_t           missing_sync[] = {0x00, 0, 0};
    const uint8_t           overflow[]     = {0xc8, 0, 0};
    struct ps2_mouse_packet packet;

    return ps2_mouse_decode_packet(PS2_MOUSE_STANDARD, missing_sync, &packet) == 0
                   || ps2_mouse_decode_packet(PS2_MOUSE_STANDARD, overflow, &packet) == 0 ?
               1 :
               0;
}

static int test_stream_resynchronizes(void)
{
    const uint8_t           bytes[] = {0x00, 0x28, 0x01, 0xff};
    struct ps2_mouse_stream stream;
    struct ps2_mouse_packet packet;
    int                     result = 0;

    ps2_mouse_stream_init(&stream, PS2_MOUSE_STANDARD);
    for (size_t i = 0; i < sizeof(bytes); i++) result = ps2_mouse_stream_byte(&stream, bytes[i], &packet);
    return result != 1 || packet.dx != 1 || packet.dy != -1;
}

int main(void)
{
    int failed = test_standard_packet() || test_wheel_packet() || test_explorer_packet() || test_rejects_malformed_packets()
                 || test_stream_resynchronizes();

    if (failed) {
        fputs("ps2 mouse protocol tests failed\n", stderr);
        return 1;
    }

    puts("ps2 mouse protocol tests passed");
    return 0;
}
