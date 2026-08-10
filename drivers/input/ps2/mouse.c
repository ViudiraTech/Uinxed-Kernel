/*
 *
 *      mouse.c
 *      PS/2 mouse driver for standard, wheel, and Explorer protocols
 *
 *      2026/7/25 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/input/evdev/evdev.h>
#include <drivers/input/ps2/ps2.h>
#include <drivers/input/ps2/ps2_mouse.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>

static input_dev_t             ps2_mouse_dev;
static enum ps2_mouse_protocol ps2_mouse_protocol;
static struct ps2_mouse_stream ps2_mouse_stream;
static struct ps2_mouse_packet ps2_mouse_previous;
static bool                    ps2_mouse_ready;
evdev_t                       *ps2_mouse_evdev;

static void set_bit(unsigned int bit, uint32_t *bits)
{
    bits[bit / 32] |= 1U << (bit % 32);
}

static int ps2_mouse_read_id(uint8_t *id)
{
    int result = ps2_send_device_command(true, PS2_DEV_GET_ID);
    if (result != EOK) return result;
    return ps2_read_data_timeout(id);
}

static int ps2_mouse_set_rate(uint8_t rate)
{
    int result = ps2_send_device_command(true, PS2_DEV_SET_SAMPLE_RATE);
    if (result != EOK) return result;
    return ps2_send_device_data(true, rate);
}

static int ps2_mouse_negotiate(const uint8_t rates[3], uint8_t expected_id)
{
    uint8_t id;
    for (size_t i = 0; i < 3; i++) {
        int result = ps2_mouse_set_rate(rates[i]);
        if (result != EOK) return result;
    }
    if (ps2_mouse_read_id(&id) != EOK) return -EIO;
    return id == expected_id ? EOK : -ENODEV;
}

static void ps2_mouse_append_button(input_event_t *events, size_t *count, uint16_t code, bool value, bool *previous)
{
    if (*previous == value) return;
    *previous          = value;
    events[(*count)++] = (input_event_t) {.type = EV_KEY, .code = code, .value = value ? 1 : 0};
}

static void ps2_mouse_report(const struct ps2_mouse_packet *packet)
{
    input_event_t events[9];
    size_t        count = 0;

    ps2_mouse_append_button(events, &count, BTN_LEFT, packet->left, &ps2_mouse_previous.left);
    ps2_mouse_append_button(events, &count, BTN_RIGHT, packet->right, &ps2_mouse_previous.right);
    ps2_mouse_append_button(events, &count, BTN_MIDDLE, packet->middle, &ps2_mouse_previous.middle);
    if (ps2_mouse_protocol == PS2_MOUSE_EXPLORER) {
        ps2_mouse_append_button(events, &count, BTN_SIDE, packet->side, &ps2_mouse_previous.side);
        ps2_mouse_append_button(events, &count, BTN_EXTRA, packet->extra, &ps2_mouse_previous.extra);
    }
    if (packet->dx) events[count++] = (input_event_t) {.type = EV_REL, .code = REL_X, .value = packet->dx};
    if (packet->dy) events[count++] = (input_event_t) {.type = EV_REL, .code = REL_Y, .value = -packet->dy};
    if (packet->wheel) events[count++] = (input_event_t) {.type = EV_REL, .code = REL_WHEEL, .value = packet->wheel};
    events[count++] = (input_event_t) {.type = EV_SYN, .code = SYN_REPORT, .value = 0};
    evdev_inject_events(&ps2_mouse_dev, events, count);
}

void ps2_mouse_handle_byte(uint8_t byte)
{
    struct ps2_mouse_packet packet;
    int                     result;

    if (!ps2_mouse_ready) return;
    result = ps2_mouse_stream_byte(&ps2_mouse_stream, byte, &packet);
    if (result == 1)
        ps2_mouse_report(&packet);
    else if (result < 0)
        plogk("ps/2: Mouse packet decode error: %d\n", result);
}

bool ps2_mouse_available(void)
{
    return ps2_mouse_ready;
}

void ps2_mouse_init(void)
{
    static const uint8_t wheel_rates[]    = {200, 100, 80};
    static const uint8_t explorer_rates[] = {200, 200, 80};
    uint8_t              response;
    uint8_t              id;
    int                  result;

    ps2_mouse_ready = false;
    result          = ps2_send_device_command(true, PS2_DEV_RESET);
    if (result != EOK) {
        plogk("ps/2: Mouse reset command failed: %d\n", result);
        return;
    }
    result = ps2_read_data_timeout(&response);
    if (result != EOK || response != PS2_RESPONSE_RESET_OK) {
        plogk("ps/2: Mouse self-test response failed: status=%d response=0x%02x\n", result, response);
        return;
    }
    result = ps2_read_data_timeout(&id);
    if (result != EOK) {
        plogk("ps/2: Mouse device ID read failed: %d\n", result);
        return;
    }
    if (ps2_send_device_command(true, PS2_DEV_DISABLE_REPORT) != EOK) return;

    ps2_mouse_protocol = PS2_MOUSE_STANDARD;
    if (ps2_mouse_negotiate(wheel_rates, 0x03) == EOK) {
        ps2_mouse_protocol = PS2_MOUSE_WHEEL;
        id                 = 0x03;
    }
    if (ps2_mouse_protocol == PS2_MOUSE_WHEEL && ps2_mouse_negotiate(explorer_rates, 0x04) == EOK) {
        ps2_mouse_protocol = PS2_MOUSE_EXPLORER;
        id                 = 0x04;
    }
    if (ps2_send_device_command(true, PS2_DEV_ENABLE_REPORT) != EOK) return;
    ps2_mouse_stream_init(&ps2_mouse_stream, ps2_mouse_protocol);
    ps2_mouse_previous = (struct ps2_mouse_packet) {0};

    memset(&ps2_mouse_dev, 0, sizeof(ps2_mouse_dev));
    strncpy(ps2_mouse_dev.name, "PS/2 Generic Mouse", EVDEV_MAX_NAME_LEN - 1);
    strncpy(ps2_mouse_dev.phys, "isa0060/serio1/input0", EVDEV_MAX_NAME_LEN - 1);
    ps2_mouse_dev.id.bustype             = BUS_I8042;
    ps2_mouse_dev.id.vendor              = 1;
    ps2_mouse_dev.id.product             = id;
    ps2_mouse_dev.id.version             = 0x0100;
    ps2_mouse_dev.hint_events_per_packet = 9;
    ps2_mouse_dev.exist                  = true;
    set_bit(EV_KEY, ps2_mouse_dev.evbit);
    set_bit(EV_REL, ps2_mouse_dev.evbit);
    set_bit(EV_SYN, ps2_mouse_dev.evbit);
    set_bit(REL_X, ps2_mouse_dev.relbit);
    set_bit(REL_Y, ps2_mouse_dev.relbit);
    set_bit(BTN_LEFT, ps2_mouse_dev.keybit);
    set_bit(BTN_RIGHT, ps2_mouse_dev.keybit);
    set_bit(BTN_MIDDLE, ps2_mouse_dev.keybit);
    if (ps2_mouse_protocol != PS2_MOUSE_STANDARD) set_bit(REL_WHEEL, ps2_mouse_dev.relbit);
    if (ps2_mouse_protocol == PS2_MOUSE_EXPLORER) {
        set_bit(BTN_SIDE, ps2_mouse_dev.keybit);
        set_bit(BTN_EXTRA, ps2_mouse_dev.keybit);
    }
    ps2_mouse_evdev = evdev_create(&ps2_mouse_dev);
    if (!ps2_mouse_evdev) {
        (void)ps2_send_device_command(true, PS2_DEV_DISABLE_REPORT);
        plogk("evdev: Unable to allocate PS/2 mouse device.\n");
        return;
    }
    if (evdev_register(ps2_mouse_evdev) != EOK) {
        evdev_destroy(ps2_mouse_evdev);
        ps2_mouse_evdev = NULL;
        (void)ps2_send_device_command(true, PS2_DEV_DISABLE_REPORT);
        plogk("evdev: Unable to register PS/2 mouse device.\n");
        return;
    }
    ps2_mouse_ready = true;
    plogk("evdev: PS/2 mouse protocol %d registered as event%d\n", ps2_mouse_protocol, ps2_mouse_evdev->minor);
}
