/*
 *
 *      keyboard.c
 *      PS/2 keyboard input driver
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */
#include <drivers/evdev.h>
#include <drivers/ps2.h>
#include <drivers/tty.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <proc/sched.h>
#include <sync/spin_lock.h>

static input_dev_t  ps2_keyboard_dev;
static wait_queue_t ps2kbd_event_wait;
static spinlock_t   ps2kbd_wait_lock;
static size_t       ps2kbd_pending_events;
evdev_t            *ps2_keyboard_evdev;

static void set_bit(unsigned int bit, uint32_t *bits)
{
    bits[bit / 32] |= 1U << (bit % 32);
}

void ps2_keyboard_init(void)
{
    memset(&ps2_keyboard_dev, 0, sizeof(ps2_keyboard_dev));
    strncpy(ps2_keyboard_dev.name, "AT Translated Set 2 keyboard", EVDEV_MAX_NAME_LEN - 1);
    strncpy(ps2_keyboard_dev.phys, "isa0060/serio0/input0", EVDEV_MAX_NAME_LEN - 1);
    ps2_keyboard_dev.id.bustype             = BUS_I8042;
    ps2_keyboard_dev.id.vendor              = 1;
    ps2_keyboard_dev.id.product             = 1;
    ps2_keyboard_dev.id.version             = 0xab41;
    ps2_keyboard_dev.hint_events_per_packet = 3;
    ps2_keyboard_dev.rep[0]                 = 250;
    ps2_keyboard_dev.rep[1]                 = 33;
    ps2_keyboard_dev.exist                  = true;
    set_bit(EV_KEY, ps2_keyboard_dev.evbit);
    set_bit(EV_MSC, ps2_keyboard_dev.evbit);
    set_bit(EV_SYN, ps2_keyboard_dev.evbit);
    set_bit(EV_REP, ps2_keyboard_dev.evbit);
    set_bit(MSC_SCAN, ps2_keyboard_dev.mscbit);
    for (unsigned int key = 0; key < KEY_CNT; key++) set_bit(key, ps2_keyboard_dev.keybit);
    wait_queue_init(&ps2kbd_event_wait);
    ps2kbd_wait_lock      = (spinlock_t) {0};
    ps2kbd_pending_events = 0;
    ps2_keyboard_evdev    = evdev_create(&ps2_keyboard_dev);
    if (ps2_keyboard_evdev && evdev_register(ps2_keyboard_evdev) == EOK)
        plogk("evdev: keyboard registered as event%d\n", ps2_keyboard_evdev->minor);
}

void ps2_keyboard_handle_byte(uint8_t scancode)
{
    bool pressed = !(scancode & 0x80);

    evdev_inject_event(&ps2_keyboard_dev, EV_MSC, MSC_SCAN, scancode);
    evdev_inject_event(&ps2_keyboard_dev, EV_KEY, scancode, pressed ? 1 : 0);
    evdev_inject_syn(&ps2_keyboard_dev);
    tty_handle_scancode(scancode, pressed);
    spin_lock(&ps2kbd_wait_lock);
    ps2kbd_pending_events++;
    spin_unlock(&ps2kbd_wait_lock);
    wait_queue_wake_one(&ps2kbd_event_wait);
}

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
