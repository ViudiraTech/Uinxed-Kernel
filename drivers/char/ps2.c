/*
 *
 *      ps2.c
 *      i8042 PS/2 controller transport and IRQ dispatch
 *
 *      2025/9/7 By MicroFish
 *      2026/7/25 Split keyboard/mouse devices by JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */
#include <chipset/common.h>
#include <drivers/char/ps2.h>
#include <drivers/input/evdev.h>
#include <drivers/interrupt/apic.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>

static bool ps2_port1_ok;
static bool ps2_port2_ok;

int wait_ps2_read(void)
{
    for (size_t i = 0; i < 10000; i++) {
        if (ps2_read_status() & PS2_STATUS_OUTPUT_FULL) return EOK;
    }
    return -ETIMEDOUT;
}

int wait_ps2_write(void)
{
    for (size_t i = 0; i < 10000; i++) {
        if (!(ps2_read_status() & PS2_STATUS_INPUT_FULL)) return EOK;
    }
    return -ETIMEDOUT;
}

uint8_t ps2_read_status(void)
{
    return inb(PS2_STATUS_PORT);
}

int ps2_read_data_timeout(uint8_t *data)
{
    if (!data) return -EINVAL;
    if (wait_ps2_read() != EOK) return -ETIMEDOUT;
    *data = inb(PS2_DATA_PORT);
    return EOK;
}

uint8_t ps2_read_data(void)
{
    uint8_t data = 0;
    (void)ps2_read_data_timeout(&data);
    return data;
}

void ps2_write_data(uint8_t data)
{
    if (wait_ps2_write() == EOK) outb(PS2_DATA_PORT, data);
}

void ps2_write_cmd(uint8_t cmd)
{
    if (wait_ps2_write() == EOK) outb(PS2_STATUS_PORT, cmd);
}

uint8_t ps2_read_config(void)
{
    ps2_write_cmd(PS2_CMD_READ_CONFIG);
    return ps2_read_data();
}

void ps2_write_config(uint8_t config)
{
    ps2_write_cmd(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(config);
}

static int ps2_write_device_byte(bool second_port, uint8_t byte)
{
    if (second_port) {
        if (wait_ps2_write() != EOK) return -ETIMEDOUT;
        outb(PS2_STATUS_PORT, PS2_CMD_WRITE_PORT2);
    }
    if (wait_ps2_write() != EOK) return -ETIMEDOUT;
    outb(PS2_DATA_PORT, byte);
    return EOK;
}

int ps2_send_device_command(bool second_port, uint8_t command)
{
    uint8_t response;

    for (int attempt = 0; attempt < 3; attempt++) {
        int result = ps2_write_device_byte(second_port, command);
        if (result != EOK) return result;
        for (int byte = 0; byte < 16; byte++) {
            result = ps2_read_data_timeout(&response);
            if (result != EOK) return result;
            if (response == PS2_RESPONSE_OK) return EOK;
            if (response == 0xfe) break;
        }
    }
    return -EIO;
}

int ps2_send_device_data(bool second_port, uint8_t data)
{
    return ps2_send_device_command(second_port, data);
}
bool ps2_port_available(bool second_port)
{
    return second_port ? ps2_port2_ok : ps2_port1_ok;
}

INTERRUPT_BEGIN static void ps2_irq(interrupt_frame_t *frame)
{
    uint8_t status;
    uint8_t data;

    (void)frame;
    status = ps2_read_status();
    if (status & (0x40 | 0x80)) {
        plogk("ps/2: %s receive error (status=0x%02x)\n", (status & PS2_STATUS_AUX_DATA) ? "mouse" : "keyboard", status);
    }
    if (status & PS2_STATUS_OUTPUT_FULL) {
        data = inb(PS2_DATA_PORT);
        if (status & PS2_STATUS_AUX_DATA)
            ps2_mouse_handle_byte(data);
        else
            ps2_keyboard_handle_byte(data);
    }
    send_eoi();
}
INTERRUPT_END

void init_ps2(void)
{
#if !CONFIG_PS2_KEYBOARD_MOUSE
    return;
#endif
    uint8_t config;
    uint8_t result;
    bool    dual_channel;

    evdev_init();
    ps2_write_cmd(PS2_CMD_DISABLE_PORT1);
    ps2_write_cmd(PS2_CMD_DISABLE_PORT2);
    while (ps2_read_status() & PS2_STATUS_OUTPUT_FULL) (void)inb(PS2_DATA_PORT);

    config = ps2_read_config();
    config &= ~(PS2_CONFIG_PORT1_IRQ | PS2_CONFIG_PORT2_IRQ);
    config |= PS2_CONFIG_TRANSLATION;
    ps2_write_config(config);

    ps2_write_cmd(PS2_CMD_SELF_TEST);
    if (ps2_read_data_timeout(&result) != EOK || result != PS2_RESPONSE_SELFTEST) {
        plogk("ps/2: controller self-test failed.\n");
        return;
    }

    ps2_write_cmd(PS2_CMD_ENABLE_PORT2);
    dual_channel = !(ps2_read_config() & PS2_CONFIG_PORT2_CLOCK);
    if (!dual_channel) ps2_write_cmd(PS2_CMD_DISABLE_PORT2);

    ps2_write_cmd(PS2_CMD_TEST_PORT1);
    ps2_port1_ok = ps2_read_data_timeout(&result) == EOK && result == PS2_RESPONSE_TEST;
    ps2_port2_ok = false;
    if (dual_channel) {
        ps2_write_cmd(PS2_CMD_TEST_PORT2);
        ps2_port2_ok = ps2_read_data_timeout(&result) == EOK && result == PS2_RESPONSE_TEST;
    }

    if (ps2_port1_ok) ps2_write_cmd(PS2_CMD_ENABLE_PORT1);
    if (ps2_port2_ok) ps2_write_cmd(PS2_CMD_ENABLE_PORT2);
    config = ps2_read_config();
    config &= ~(PS2_CONFIG_PORT1_IRQ | PS2_CONFIG_PORT2_IRQ);
    config |= PS2_CONFIG_TRANSLATION;
    if (ps2_port1_ok) {
        config &= ~PS2_CONFIG_PORT1_CLOCK;
        config |= PS2_CONFIG_PORT1_IRQ;
    } else {
        config |= PS2_CONFIG_PORT1_CLOCK;
    }
    if (ps2_port2_ok) {
        config &= ~PS2_CONFIG_PORT2_CLOCK;
        config |= PS2_CONFIG_PORT2_IRQ;
    } else {
        config |= PS2_CONFIG_PORT2_CLOCK;
    }
    ps2_write_config(config);

    if (ps2_port1_ok) {
        ps2_keyboard_init();
        register_interrupt_handler(IRQ_1, (void *)ps2_irq, 0, 0x8e);
    }
    if (ps2_port2_ok) {
        ps2_mouse_init();
        register_interrupt_handler(IRQ_12, (void *)ps2_irq, 0, 0x8e);
    }
    plogk("ps/2: keyboard=%s mouse-port=%s\n", ps2_port1_ok ? "ready" : "unavailable", ps2_port2_ok ? "ready" : "unavailable");
}
