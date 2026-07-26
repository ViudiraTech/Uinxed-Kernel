#include <drivers/ps2_keyboard.h>
#include <stdio.h>

static int failures;

#define CHECK(condition, message)                                                                                                          \
    do {                                                                                                                                   \
        if (!(condition)) {                                                                                                                \
            printf("FAIL %s:%d: %s\n", __func__, __LINE__, message);                                                                     \
            failures++;                                                                                                                    \
            return;                                                                                                                        \
        }                                                                                                                                  \
    } while (0)

static int feed(ps2_keyboard_decoder_t *decoder, const uint8_t *bytes, size_t count, ps2_key_event_t *event)
{
    int result = 0;
    for (size_t i = 0; i < count; i++) {
        int current = ps2_keyboard_decode_byte(decoder, bytes[i], event);
        if (current) result = current;
    }
    return result;
}

static void test_base_make_and_break_use_linux_keycode(void)
{
    ps2_keyboard_decoder_t decoder;
    ps2_key_event_t key;

    ps2_keyboard_decoder_init(&decoder);
    CHECK(ps2_keyboard_decode_byte(&decoder, 0x1e, &key) == 1, "A make missing");
    CHECK(key.scan == 0x1e && key.keycode == KEY_A && key.pressed && !key.auto_release, "A make decoded incorrectly");
    CHECK(ps2_keyboard_decode_byte(&decoder, 0x9e, &key) == 1, "A break missing");
    CHECK(key.scan == 0x1e && key.keycode == KEY_A && !key.pressed, "A break decoded incorrectly");
    CHECK(ps2_keyboard_decode_byte(&decoder, 0x57, &key) == 1 && key.keycode == KEY_F11, "F11 make mapping");
    CHECK(ps2_keyboard_decode_byte(&decoder, 0xd7, &key) == 1 && key.keycode == KEY_F11 && !key.pressed, "F11 break mapping");
}

static void test_e0_extended_keys(void)
{
    static const uint8_t right_ctrl_make[]  = {0xe0, 0x1d};
    static const uint8_t right_ctrl_break[] = {0xe0, 0x9d};
    static const uint8_t up_make[]          = {0xe0, 0x48};
    static const uint8_t up_break[]         = {0xe0, 0xc8};
    ps2_keyboard_decoder_t decoder;
    ps2_key_event_t key;

    ps2_keyboard_decoder_init(&decoder);
    CHECK(feed(&decoder, right_ctrl_make, 2, &key) == 1, "right ctrl make missing");
    CHECK(key.scan == 0x9d && key.keycode == KEY_RIGHTCTRL && key.pressed, "right ctrl make mapping");
    CHECK(feed(&decoder, right_ctrl_break, 2, &key) == 1, "right ctrl break missing");
    CHECK(key.scan == 0x9d && key.keycode == KEY_RIGHTCTRL && !key.pressed, "right ctrl break mapping");
    CHECK(feed(&decoder, up_make, 2, &key) == 1 && key.keycode == KEY_UP && key.pressed, "up make mapping");
    CHECK(feed(&decoder, up_break, 2, &key) == 1 && key.keycode == KEY_UP && !key.pressed, "up break mapping");
}

static void test_print_screen_ignores_fake_shift(void)
{
    static const uint8_t make[]  = {0xe0, 0x2a, 0xe0, 0x37};
    static const uint8_t release[] = {0xe0, 0xb7, 0xe0, 0xaa};
    ps2_keyboard_decoder_t decoder;
    ps2_key_event_t key;
    int events = 0;

    ps2_keyboard_decoder_init(&decoder);
    for (size_t i = 0; i < sizeof(make); i++) {
        if (ps2_keyboard_decode_byte(&decoder, make[i], &key)) {
            events++;
            CHECK(key.keycode == KEY_SYSRQ && key.pressed, "print screen make mapping");
        }
    }
    for (size_t i = 0; i < sizeof(release); i++) {
        if (ps2_keyboard_decode_byte(&decoder, release[i], &key)) {
            events++;
            CHECK(key.keycode == KEY_SYSRQ && !key.pressed, "print screen break mapping");
        }
    }
    CHECK(events == 2, "fake shift bytes created key events");
}

static void test_pause_sequence_is_auto_released(void)
{
    static const uint8_t pause[] = {0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5};
    ps2_keyboard_decoder_t decoder;
    ps2_key_event_t key;

    ps2_keyboard_decoder_init(&decoder);
    CHECK(feed(&decoder, pause, sizeof(pause), &key) == 1, "Pause sequence missing");
    CHECK(key.scan == 0xc5 && key.keycode == KEY_PAUSE && key.pressed && key.auto_release, "Pause sequence mapping");
}

static void test_malformed_prefix_recovers(void)
{
    static const uint8_t malformed_then_a[] = {0xe1, 0x00, 0x1e};
    ps2_keyboard_decoder_t decoder;
    ps2_key_event_t key;

    ps2_keyboard_decoder_init(&decoder);
    CHECK(feed(&decoder, malformed_then_a, sizeof(malformed_then_a), &key) == 1, "decoder did not recover");
    CHECK(key.keycode == KEY_A && key.pressed, "recovered key mapping");
    CHECK(ps2_keyboard_keycode_for_scancode(0x80 | 0x2a) == 0, "fake extended shift advertised");
    CHECK(ps2_keyboard_keycode_for_scancode(0x80 | 0x48) == KEY_UP, "extended capability mapping");
}

int main(void)
{
    test_base_make_and_break_use_linux_keycode();
    test_e0_extended_keys();
    test_print_screen_ignores_fake_shift();
    test_pause_sequence_is_auto_released();
    test_malformed_prefix_recovers();

    if (failures) {
        printf("%d PS/2 keyboard test(s) failed\n", failures);
        return 1;
    }
    printf("PASS PS/2 translated scan-code decoding\n");
    return 0;
}
