/*
 *
 *      keyboard.c
 *      PS/2 keyboard input driver
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/input/evdev/evdev.h>
#include <drivers/input/ps2/ps2.h>
#include <drivers/input/ps2/ps2_keyboard.h>
#include <drivers/tty/tty.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <sync/spin_lock.h>

static input_dev_t            ps2_keyboard_dev;
static wait_queue_t           ps2kbd_event_wait;
static spinlock_t             ps2kbd_wait_lock;
static size_t                 ps2kbd_pending_events;
static ps2_keyboard_decoder_t ps2kbd_decoder;
evdev_t                      *ps2_keyboard_evdev;

/* Set one bit in a bitmap word array. */
static void set_bit(unsigned int bit, uint32_t *bits)
{
    bits[bit / 32] |= 1U << (bit % 32);
}

/* Convert evdev LED bits to the PS/2 Set-LEDs bit order and push them. */
static void ps2kbd_set_leds(uint8_t leds)
{
    uint8_t ps2 = 0;
    if (leds & (1U << LED_NUML)) ps2 |= 0x02;
    if (leds & (1U << LED_CAPSL)) ps2 |= 0x04;
    if (leds & (1U << LED_SCROLLL)) ps2 |= 0x01;

    ps2_send_device_command(false, PS2_DEV_SET_LEDS);
    ps2_send_device_data(false, ps2);
}

/* LED notify callback: push the global LED state to the hardware. */
static void ps2kbd_led_notify(void *ctx, uint8_t leds)
{
    (void)ctx;
    ps2kbd_set_leds(leds);
}

/* Initialize and register the PS/2 keyboard input device. */
void ps2_keyboard_init(void)
{
    memset(&ps2_keyboard_dev, 0, sizeof(ps2_keyboard_dev));
    strncpy(ps2_keyboard_dev.name, "AT Translated Set 2 keyboard", EVDEV_MAX_NAME_LEN - 1);
    strncpy(ps2_keyboard_dev.phys, "isa0060/serio0/input0", EVDEV_MAX_NAME_LEN - 1);
    ps2_keyboard_dev.id.bustype             = BUS_I8042;
    ps2_keyboard_dev.id.vendor              = 1;
    ps2_keyboard_dev.id.product             = 1;
    ps2_keyboard_dev.id.version             = 0xab41;
    ps2_keyboard_dev.hint_events_per_packet = 6;
    ps2_keyboard_dev.rep[0]                 = 250;
    ps2_keyboard_dev.rep[1]                 = 33;
    ps2_keyboard_dev.exist                  = true;
    set_bit(EV_KEY, ps2_keyboard_dev.evbit);
    set_bit(EV_MSC, ps2_keyboard_dev.evbit);
    set_bit(EV_SYN, ps2_keyboard_dev.evbit);
    set_bit(EV_REP, ps2_keyboard_dev.evbit);
    set_bit(EV_LED, ps2_keyboard_dev.evbit);
    set_bit(MSC_SCAN, ps2_keyboard_dev.mscbit);
    set_bit(LED_NUML, ps2_keyboard_dev.ledbit);
    set_bit(LED_CAPSL, ps2_keyboard_dev.ledbit);
    set_bit(LED_SCROLLL, ps2_keyboard_dev.ledbit);
    for (unsigned int scan = 0; scan <= 0xff; scan++) {
        uint16_t keycode = ps2_keyboard_keycode_for_scancode((uint16_t)scan);

        if (keycode) set_bit(keycode, ps2_keyboard_dev.keybit);
    }
    wait_queue_init(&ps2kbd_event_wait);
    ps2kbd_wait_lock      = (spinlock_t) {0};
    ps2kbd_pending_events = 0;
    ps2_keyboard_decoder_init(&ps2kbd_decoder);
    ps2_keyboard_evdev = evdev_create(&ps2_keyboard_dev);
    if (!ps2_keyboard_evdev) {
        plogk("evdev: Unable to allocate keyboard device.\n");
        return;
    }
    if (evdev_register(ps2_keyboard_evdev) != EOK) {
        evdev_destroy(ps2_keyboard_evdev);
        ps2_keyboard_evdev = NULL;
        plogk("evdev: Unable to register keyboard device.\n");
        return;
    }
    plogk("evdev: Keyboard registered as event%d\n", ps2_keyboard_evdev->minor);
    evdev_register_led_notify(ps2kbd_led_notify, NULL);
    ps2kbd_set_leds(0);
}

/* Decode one scancode byte and inject the resulting evdev/tty events. */
void ps2_keyboard_handle_byte(uint8_t byte)
{
    input_event_t   events[6];
    ps2_key_event_t key;
    size_t          count = 0;

    if (!ps2_keyboard_decode_byte(&ps2kbd_decoder, byte, &key)) return;

    events[count++] = (input_event_t) {.type = EV_MSC, .code = MSC_SCAN, .value = key.scan};
    events[count++] = (input_event_t) {.type = EV_KEY, .code = key.keycode, .value = key.pressed ? 1 : 0};
    events[count++] = (input_event_t) {.type = EV_SYN, .code = SYN_REPORT, .value = 0};
    if (key.auto_release) {
        events[count++] = (input_event_t) {.type = EV_MSC, .code = MSC_SCAN, .value = key.scan};
        events[count++] = (input_event_t) {.type = EV_KEY, .code = key.keycode, .value = 0};
        events[count++] = (input_event_t) {.type = EV_SYN, .code = SYN_REPORT, .value = 0};
    }
    evdev_inject_events(&ps2_keyboard_dev, events, count);

    tty_handle_scancode((uint8_t)key.keycode, key.pressed);
    if (key.auto_release) tty_handle_scancode((uint8_t)key.keycode, false);
    spin_lock(&ps2kbd_wait_lock);
    ps2kbd_pending_events++;
    spin_unlock(&ps2kbd_wait_lock);
    wait_queue_wake_one(&ps2kbd_event_wait);
}

/* Block until at least one keyboard event has been reported. */
int ps2kbd_wait_events(void)
{
    for (;;) {
        spin_lock(&ps2kbd_wait_lock);
        if (ps2kbd_pending_events) {
            ps2kbd_pending_events--;
            spin_unlock(&ps2kbd_wait_lock);
            return EOK;
        }
        wait_queue_prepare(&ps2kbd_event_wait);
        spin_unlock(&ps2kbd_wait_lock);
        wait_queue_sleep();
    }
}
