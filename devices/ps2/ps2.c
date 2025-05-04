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

#include "ps2.h"
#include "alloc.h"
#include "apic.h"
#include "common.h"
#include "idt.h"
#include "printk.h"
#include "stddef.h"
#include "stdint.h"
#include "string.h"

static char *holding;
int led_state = 0; // LED state

// Send command to PS/2 controller
static void ps2_send_cmd(uint8_t cmd)
{
    while ((inb(PS2_CMD_PORT) & 0x02) != 0); // Wait
    outb(PS2_CMD_PORT, cmd);
}

void update_led_state()
{
    while (inb(PS2_CMD_PORT) & 0x02);
    outb(PS2_DATA_PORT, 0xED);

    while ((inb(PS2_CMD_PORT) & 0x01) == 0);
    uint8_t ack = inb(PS2_DATA_PORT);
    if (ack != 0xFA) {
        plogk("LED no ack1: 0x%02X\n", ack);
        return;
    }

    while ((inb(PS2_CMD_PORT) & 0x02));
    outb(PS2_DATA_PORT,
         led_state & 0b111); // Strip off the high bits (if you want some other LEDs to be on, set them here)

    while ((inb(PS2_CMD_PORT) & 0x01) == 0);
    ack = inb(PS2_DATA_PORT);
    if (ack != 0xFA) { plogk("LED no ack2: 0x%02X\n", ack); }
}

static int scancode_to_ascii(int scancode, int is_numlock, int is_shift)
{
    static const int lowercase_ascii_map[128] = {
        [0x01] = '\e', [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4',  [0x06] = '5', [0x07] = '6',
        [0x08] = '7',  [0x09] = '8', [0x0A] = '9', [0x0B] = '0', [0x0C] = '-',  [0x0D] = '=', [0x0E] = '\b',
        [0x0F] = '\t', [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r',  [0x14] = 't', [0x15] = 'y',
        [0x16] = 'u',  [0x17] = 'i', [0x18] = 'o', [0x19] = 'p', [0x1A] = '[',  [0x1B] = ']', [0x1C] = '\n',
        [0x1D] = -1,   [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f',  [0x22] = 'g', [0x23] = 'h',
        [0x24] = 'j',  [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`', [0x2A] = -1,
        [0x2B] = '\\', [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v',  [0x30] = 'b', [0x31] = 'n',
        [0x32] = 'm',  [0x33] = ',', [0x34] = '.', [0x35] = '/', [0x36] = -1,   [0x37] = -1,  [0x38] = -1,
        [0x39] = ' ',
    };
    // When `shift` or `capslock`
    static const int uppercase_ascii_map[128] = {
        [0x01] = '\e', [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%', [0x07] = '^',
        [0x08] = '&',  [0x09] = '*', [0x0A] = '(', [0x0B] = ')', [0x0C] = '_', [0x0D] = '+', [0x0E] = '\b',
        [0x0F] = '\t', [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y',
        [0x16] = 'U',  [0x17] = 'I', [0x18] = 'O', [0x19] = 'P', [0x1A] = '{', [0x1B] = '}', [0x1C] = '\n',
        [0x1D] = -1,   [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F', [0x22] = 'G', [0x23] = 'H',
        [0x24] = 'J',  [0x25] = 'K', [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~', [0x2A] = -1,
        [0x2B] = '|',  [0x2C] = 'Z', [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B', [0x31] = 'N',
        [0x32] = 'M',  [0x33] = '<', [0x34] = '>', [0x35] = '?', [0x36] = -1,  [0x37] = -1,  [0x38] = -1,
        [0x39] = ' ',
    };

    const int *map;
    map = (is_shift) ? uppercase_ascii_map : lowercase_ascii_map;

    int tmp_code = 0;
    if (is_numlock) {
        switch (scancode) {
            case 0x47 :
                tmp_code = '7';
                break;
            case 0x48 :
                tmp_code = '8';
                break;
            case 0x49 :
                tmp_code = '9';
                break;
            case 0x4b :
                tmp_code = '4';
                break;
            case 0x4c :
                tmp_code = '5';
                break;
            case 0x4d :
                tmp_code = '6';
                break;
            case 0x4f :
                tmp_code = '1';
                break;
            case 0x50 :
                tmp_code = '2';
                break;
            case 0x51 :
                tmp_code = '3';
                break;
            case 0x52 :
                tmp_code = '0';
                break;
            case 0x53 :
                tmp_code = '.';
                break;
            default :
                goto numlock_syms;
        }
        return tmp_code;
    }
numlock_syms:
    switch (scancode) { // numlock symbols (Whenever numlock is off)
        case 0x135 :    // 0xe0 0x35 (encoded as 0x135)
            tmp_code = '/';
            break;
        case 0x11c :
            tmp_code = '\n';
            break;
        case 0x37 :
            tmp_code = '*';
            break;
        case 0x4a :
            tmp_code = '-';
            break;
        case 0x4e :
            tmp_code = '+';
            break;
        default :
            // Normal keys
            return (scancode <= 0x39) ? map[scancode] : -1;
            break;
    }
    return tmp_code;
}

void (*kbd_handler)(int scan_code, int ascii_code, int is_extended, int is_released) = NULL;

void register_kbd_handler(void (*handle)(int scan_code, int ascii_code, int is_extended, int is_released))
{
    kbd_handler = handle;
}

// For E1 1D 45 E1 9D C5 (pause pressed), only gods know why.
// (this is also a prefix of extended scan codes)
int extend_count = 0;
int byte_count   = 0; // byte to skip (extended scan code)
int is_extended  = 0;
int is_released  = 0;

void kbd_led_set(int led_id, int state)
{
    if (state) {
        led_state |= led_id;
    } else {
        led_state &= ~led_id;
    }
    update_led_state(); // Update led state of the keyboard
}

int kbd_led_get(int led_id)
{
    return led_state & led_id;
}

static void handle_scancode(uint32_t scancode)
{
    int ascii;
    switch (scancode) {
        case 0xE0 :
            // 1 byte extended scan code
            // So we will set the high bit to 1
            is_extended  = 1;
            extend_count = 1;
            byte_count   = 1;
            return;
        case 0xE1 :
            // 2 bytes extended scan code
            // So we will set the high bit to 2
            is_extended  = 1;
            extend_count = 2;
            byte_count   = 1;
            return;
    }
    if (is_extended && byte_count < extend_count) { // Skip extended scan code
        byte_count++;
        return;
    }
    if (scancode & 0x80) {
        is_released = 1;
        scancode ^= 0x80; // Strip off the release bit
    }
    if (is_extended) {
        scancode |= extend_count << 8; // Prefix for extended scan code
        extend_count = 0;              // Reset extended scan code count
        byte_count   = 0;              // Reset byte count
    }

    holding[scancode] = 0;
    if (!is_released) {
        switch (scancode) { // Num Lock, Scroll Lock, Caps Lock
            case 0x45 :
                kbd_led_set(LED_NUM_LOCK, !kbd_led_get(LED_NUM_LOCK));
                break;
            case 0x46 :
                kbd_led_set(LED_SCROLL_LOCK, !kbd_led_get(LED_SCROLL_LOCK));
                break;
            case 0x3A :
                kbd_led_set(LED_CAPS_LOCK, !kbd_led_get(LED_CAPS_LOCK));
                break;
            default :
                break;
        }
        holding[scancode] = 1; // 按键按下
    }

    // Check shift key state
    // 0x2a = left shift, 0x36 = right shift
    int shift_holding = is_key_holding(0x2a) || is_key_holding(0x36);

    // Get ASCII code
    ascii = scancode_to_ascii(scancode, kbd_led_get(LED_NUM_LOCK), shift_holding ^ (kbd_led_get(LED_CAPS_LOCK) != 0));
    // Reset extended state
    is_extended = 0;

    // Keyboard handler (will get release event)
    if (kbd_handler) { kbd_handler(scancode, ascii, is_extended, is_released); }
    if (is_released) { is_released = 0; }
}

int is_key_holding(int scancode)
{
    if (scancode < 0 || scancode >= 32768) {
        return 0; // Invalid scan code
    }
    return holding[scancode]; // Return 1 if key is holding
}

// Keyboard interrupt handler
__attribute__((interrupt)) static void keyboard_handler(interrupt_frame_t *stack)
{
    uint8_t scancode;
    scancode = inb(PS2_DATA_PORT);
    // 处理扫描码（例如转换为ASCII）
    handle_scancode(scancode);
    // 通知中断结束
    send_eoi();
    enable_intr();
}

// Register PS/2 interrupts
static void ps2_register_irq()
{
    register_interrupt_handler(1, keyboard_handler, 0, 0x8e);
    register_interrupt_handler(33, keyboard_handler, 0, 0x8e);
}

// Initialize PS/2 controller
void init_ps2()
{
    ps2_send_cmd(0xAD); // Disable keyboard

    while (inb(PS2_CMD_PORT) & 0x01) { inb(PS2_DATA_PORT); }

    // Read configuration byte
    ps2_send_cmd(0x20);
    uint8_t config = inb(PS2_DATA_PORT);
    config |= 0x01; // Enable keyboard interrupt

    ps2_send_cmd(0x60); // Write configuration byte
    outb(PS2_DATA_PORT, config);

    // Check configuration
    ps2_send_cmd(0x20);
    config = inb(PS2_DATA_PORT);
    if ((config & 0x03) != 0x03) { plogk("PS/2 Config error: 0x%02X\n", config); }

    // Allocate 32767 bytes for holding key states
    holding = malloc(32768 * sizeof(char));
    memset(holding, 0, 32768 * sizeof(char));

    // Re enable keyboard and mouse
    ps2_send_cmd(0xAE);

    // Set LED state
    kbd_led_set(LED_SCROLL_LOCK, 0);
    kbd_led_set(LED_CAPS_LOCK, 0);
    kbd_led_set(LED_NUM_LOCK, 1);

    // Register PS/2 interrupts
    ps2_register_irq();
}