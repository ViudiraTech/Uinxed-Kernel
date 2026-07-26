/*
 * Decoder for i8042-translated keyboard scan-set-1 byte streams.
 */

#include <drivers/ps2_keyboard.h>

uint16_t ps2_keyboard_keycode_for_scancode(uint16_t scan)
{
    if (scan >= 0x01 && scan <= 0x53) return scan;
    if (scan >= 0x56 && scan <= 0x58) return scan;

    switch (scan) {
        case 0x80 | 0x1c :
            return KEY_KPENTER;
        case 0x80 | 0x1d :
            return KEY_RIGHTCTRL;
        case 0x80 | 0x35 :
            return KEY_KPSLASH;
        case 0x80 | 0x37 :
            return KEY_SYSRQ;
        case 0x80 | 0x38 :
            return KEY_RIGHTALT;
        case 0x80 | 0x47 :
            return KEY_HOME;
        case 0x80 | 0x48 :
            return KEY_UP;
        case 0x80 | 0x49 :
            return KEY_PAGEUP;
        case 0x80 | 0x4b :
            return KEY_LEFT;
        case 0x80 | 0x4d :
            return KEY_RIGHT;
        case 0x80 | 0x4f :
            return KEY_END;
        case 0x80 | 0x50 :
            return KEY_DOWN;
        case 0x80 | 0x51 :
            return KEY_PAGEDOWN;
        case 0x80 | 0x52 :
            return KEY_INSERT;
        case 0x80 | 0x53 :
            return KEY_DELETE;
        case 0x80 | 0x5b :
            return KEY_LEFTMETA;
        case 0x80 | 0x5c :
            return KEY_RIGHTMETA;
        case 0x80 | 0x5d :
            return KEY_COMPOSE;
        case 0xc5 :
            return KEY_PAUSE;
        default :
            return 0;
    }
}

void ps2_keyboard_decoder_init(ps2_keyboard_decoder_t *decoder)
{
    if (!decoder) return;
    decoder->pause_index = 0;
    decoder->extended    = false;
}

static int ps2_keyboard_decode_key(ps2_keyboard_decoder_t *decoder, uint8_t byte, ps2_key_event_t *event)
{
    uint16_t scan;
    uint16_t keycode;

    scan              = byte & 0x7f;
    event->pressed    = (byte & 0x80) == 0;
    event->auto_release = false;
    if (decoder->extended) scan |= 0x80;
    decoder->extended = false;

    keycode = ps2_keyboard_keycode_for_scancode(scan);
    if (!keycode) return 0;

    event->scan    = scan;
    event->keycode = keycode;
    return 1;
}

int ps2_keyboard_decode_byte(ps2_keyboard_decoder_t *decoder, uint8_t byte, ps2_key_event_t *event)
{
    static const uint8_t pause_sequence[] = {0xe1, 0x1d, 0x45, 0xe1, 0x9d, 0xc5};

    if (!decoder || !event) return 0;

    if (decoder->pause_index) {
        if (byte == pause_sequence[decoder->pause_index]) {
            decoder->pause_index++;
            if (decoder->pause_index != sizeof(pause_sequence)) return 0;

            decoder->pause_index = 0;
            event->scan          = 0xc5;
            event->keycode       = KEY_PAUSE;
            event->pressed       = true;
            event->auto_release  = true;
            return 1;
        }
        decoder->pause_index = 0;
    }

    if (byte == 0xe1) {
        decoder->extended    = false;
        decoder->pause_index = 1;
        return 0;
    }
    if (byte == 0xe0) {
        decoder->extended = true;
        return 0;
    }

    return ps2_keyboard_decode_key(decoder, byte, event);
}
