/*
 *
 *      ps2.c
 *      i8042 PS/2 controller transport and IRQ dispatch
 *
 *      2026/7/25 By MicroFish & JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <drivers/firmware/apic.h>
#include <drivers/input/evdev/evdev.h>
#include <drivers/input/ps2/ps2.h>
#include <kernel/errno.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <sync/spin_lock.h>

static bool ps2_port1_ok;
static bool ps2_port2_ok;

#define PS2_PORT_KEYBOARD    0U
#define PS2_PORT_MOUSE       1U
#define PS2_PORT_COUNT       2U
#define PS2_IRQ_DRAIN_BUDGET 64U
#define PS2_INPUT_BATCH_SIZE PS2_IRQ_DRAIN_BUDGET

struct ps2_queued_byte {
        uint8_t status;
        uint8_t data;
};

struct ps2_input_batch {
        struct ps2_queued_byte events[PS2_INPUT_BATCH_SIZE];
        size_t                 count;
};

static bool ps2_resync_pending[PS2_PORT_COUNT];

/* Serializes the shared status/data registers across IRQ1, IRQ12 and commands. */
static spinlock_t ps2_controller_lock;

/* Save bytes that arrived while the shared controller registers were locked. */
static void ps2_batch_input_locked(struct ps2_input_batch *batch, uint8_t status, uint8_t data)
{
    unsigned int port = (status & PS2_STATUS_AUX_DATA) ? PS2_PORT_MOUSE : PS2_PORT_KEYBOARD;

    if (batch->count == PS2_INPUT_BATCH_SIZE) {
        ps2_resync_pending[port] = true;
        return;
    }
    batch->events[batch->count++] = (struct ps2_queued_byte) {.status = status, .data = data};
}

/* Decode and inject one byte synchronously, as Linux's serio interrupt path does. */
static void ps2_dispatch_input(uint8_t status, uint8_t data)
{
    unsigned int port = (status & PS2_STATUS_AUX_DATA) ? PS2_PORT_MOUSE : PS2_PORT_KEYBOARD;

    if (ps2_resync_pending[port] || (status & (PS2_STATUS_TIMEOUT | PS2_STATUS_PARITY))) {
        ps2_resync_pending[port] = false;
        if (port == PS2_PORT_MOUSE)
            ps2_mouse_reset_stream();
        else
            ps2_keyboard_reset_stream();
        if (status & (PS2_STATUS_TIMEOUT | PS2_STATUS_PARITY)) return;
    }

    if (port == PS2_PORT_MOUSE)
        ps2_mouse_handle_byte(data);
    else
        ps2_keyboard_handle_byte(data);
}

static void ps2_dispatch_batch(const struct ps2_input_batch *batch)
{
    for (size_t i = 0; i < batch->count; i++) ps2_dispatch_input(batch->events[i].status, batch->events[i].data);
}

/* Poll the status register until the controller has data for us. */
int wait_ps2_read(void)
{
    for (size_t i = 0; i < 10000; i++)
        if (ps2_read_status() & PS2_STATUS_OUTPUT_FULL) return EOK;
    return -ETIMEDOUT;
}

/* Poll the status register until the controller can accept a byte. */
int wait_ps2_write(void)
{
    for (size_t i = 0; i < 10000; i++)
        if (!(ps2_read_status() & PS2_STATUS_INPUT_FULL)) return EOK;
    return -ETIMEDOUT;
}

/* Read the controller status register. */
uint8_t ps2_read_status(void)
{
    return inb(PS2_STATUS_PORT);
}

/* Read one data byte, timing out if nothing arrives in time. */
int ps2_read_data_timeout(uint8_t *data)
{
    if (!data) return -EINVAL;
    if (wait_ps2_read() != EOK) return -ETIMEDOUT;
    *data = inb(PS2_DATA_PORT);
    return EOK;
}

/* Read one byte from the PS/2 data port. */
uint8_t ps2_read_data(void)
{
    uint8_t data = 0;
    (void)ps2_read_data_timeout(&data);
    return data;
}

/* Write one byte to the PS/2 data port. */
void ps2_write_data(uint8_t data)
{
    if (wait_ps2_write() == EOK) outb(PS2_DATA_PORT, data);
}

/* Issue a command byte to the i8042 controller. */
void ps2_write_cmd(uint8_t cmd)
{
    if (wait_ps2_write() == EOK) outb(PS2_STATUS_PORT, cmd);
}

/* Read the controller configuration byte. */
uint8_t ps2_read_config(void)
{
    ps2_write_cmd(PS2_CMD_READ_CONFIG);
    return ps2_read_data();
}

/* Write the controller configuration byte. */
void ps2_write_config(uint8_t config)
{
    ps2_write_cmd(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(config);
}

/* Forward one byte to a PS/2 device (through port 2 if requested). */
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

/* Send a command and wait for the device ACK, retrying on 0xfe. */
int ps2_send_device_command(bool second_port, uint8_t command)
{
    struct ps2_input_batch batch = {0};
    uint8_t                response;
    int                    result = -EIO;

    spin_lock(&ps2_controller_lock);
    for (int attempt = 0; attempt < 3; attempt++) {
        result = ps2_write_device_byte(second_port, command);
        if (result != EOK) break;
        result = -EIO;
        for (int byte = 0; byte < 16; byte++) {
            uint8_t status;

            if (wait_ps2_read() != EOK) {
                result = -ETIMEDOUT;
                goto out;
            }
            status   = ps2_read_status();
            response = inb(PS2_DATA_PORT);
            if (status & (PS2_STATUS_TIMEOUT | PS2_STATUS_PARITY)) {
                ps2_batch_input_locked(&batch, status, response);
                continue;
            }
            if (((status & PS2_STATUS_AUX_DATA) != 0) != second_port) {
                ps2_batch_input_locked(&batch, status, response);
                continue;
            }
            if (response == PS2_RESPONSE_OK) {
                result = EOK;
                goto out;
            }
            if (response == PS2_RESPONSE_RESEND) break;
            if (response == PS2_RESPONSE_ERROR1 || response == PS2_RESPONSE_ERROR2) {
                result = -EIO;
                goto out;
            }
            ps2_batch_input_locked(&batch, status, response);
        }
    }
out:
    spin_unlock(&ps2_controller_lock);
    ps2_dispatch_batch(&batch);
    return result;
}

/* Send raw data to the selected PS/2 port. */
int ps2_send_device_data(bool second_port, uint8_t data)
{
    return ps2_send_device_command(second_port, data);
}

/* Return whether the given PS/2 port exists and is usable. */
bool ps2_port_available(bool second_port)
{
    return second_port ? ps2_port2_ok : ps2_port1_ok;
}

/* IRQ handler: drain the controller, acknowledge it, then synchronously feed input core. */
INTERRUPT_BEGIN static void ps2_irq(interrupt_frame_t *frame)
{
    struct ps2_input_batch batch = {0};

    irq_enter_gs(frame);
    spin_lock(&ps2_controller_lock);
    for (size_t drained = 0; drained < PS2_IRQ_DRAIN_BUDGET; drained++) {
        uint8_t status = ps2_read_status();
        uint8_t data;

        if (!(status & PS2_STATUS_OUTPUT_FULL)) break;
        data = inb(PS2_DATA_PORT);
        ps2_batch_input_locked(&batch, status, data);
    }
    spin_unlock(&ps2_controller_lock);
    send_eoi();
    ps2_dispatch_batch(&batch);
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Probe and initialize the i8042 controller and its ports. */
void init_ps2(void)
{
#if !CONFIG_PS2_KEYBOARD_MOUSE
    return;
#endif
    uint8_t config;
    uint8_t result;
    bool    dual_channel;

    evdev_init();
    ps2_controller_lock = (spinlock_t) {0};
    for (size_t port = 0; port < PS2_PORT_COUNT; port++) ps2_resync_pending[port] = false;
    ps2_write_cmd(PS2_CMD_DISABLE_PORT1);
    ps2_write_cmd(PS2_CMD_DISABLE_PORT2);
    while (ps2_read_status() & PS2_STATUS_OUTPUT_FULL) (void)inb(PS2_DATA_PORT);

    config = ps2_read_config();
    config &= ~(PS2_CONFIG_PORT1_IRQ | PS2_CONFIG_PORT2_IRQ);
    config |= PS2_CONFIG_TRANSLATION;
    ps2_write_config(config);

    ps2_write_cmd(PS2_CMD_SELF_TEST);
    if (ps2_read_data_timeout(&result) != EOK || result != PS2_RESPONSE_SELFTEST) {
        plogk("ps2: Controller self-test failed.\n");
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
    plogk("ps2: Keyboard=%s mouse-port=%s\n", ps2_port1_ok ? "ready" : "unavailable", ps2_port2_ok ? "ready" : "unavailable");
}
