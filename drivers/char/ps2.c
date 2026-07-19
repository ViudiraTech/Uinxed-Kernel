/*
 *
 *      ps2.c
 *      PS/2 controller driver
 *
 *      2025/9/7 By MicroFish
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <common.h>
#include <apic.h>
#include <interrupt.h>
#include <printk.h>
#include <ps2.h>
#include <sched.h>
#include <spin_lock.h>

#define PS2_KBD_EVENT_QUEUE_SIZE 128

static input_event_t ps2kbd_events[PS2_KBD_EVENT_QUEUE_SIZE];
static size_t            ps2kbd_event_head = 0;
static size_t            ps2kbd_event_tail = 0;
static spinlock_t        ps2kbd_event_lock = {0};
static wait_queue_t      ps2kbd_event_wait;

static void ps2kbd_event_push(uint8_t scancode)
{
    size_t next;

    spin_lock(&ps2kbd_event_lock);
    next = (ps2kbd_event_head + 1) % PS2_KBD_EVENT_QUEUE_SIZE;
    if (next == ps2kbd_event_tail) ps2kbd_event_tail = (ps2kbd_event_tail + 1) % PS2_KBD_EVENT_QUEUE_SIZE;

    ps2kbd_events[ps2kbd_event_head] = (input_event_t) {
        .timestamp_ns = nano_time(),
        .type         = input_event_type_raw,
        .code         = scancode,
        .value        = (scancode & 0x80) ? 0 : 1,
        .raw          = scancode,
    };
    ps2kbd_event_head = next;
    spin_unlock(&ps2kbd_event_lock);
    wait_queue_wake_one(&ps2kbd_event_wait);
}

INTERRUPT_BEGIN static void ps2kbd_irq(interrupt_frame_t *frame)
{
    (void)frame;
    disable_intr();
    if (ps2_read_status() & PS2_STATUS_OUTPUT_FULL) ps2kbd_event_push(inb(PS2_DATA_PORT));
    send_eoi();
    enable_intr();
}
INTERRUPT_END

/* Waiting for PS/2 read ready */
int wait_ps2_read(void)
{
    for (size_t i = 0; i < 10000; ++i) {
        if (ps2_read_status() & PS2_STATUS_OUTPUT_FULL) return 0;
    }
    return 1;
}

/* Waiting for PS/2 write ready */
int wait_ps2_write(void)
{
    for (size_t i = 0; i < 10000; ++i) {
        if (!(ps2_read_status() & PS2_STATUS_INPUT_FULL)) return 0;
    }
    return 1;
}

/* Read PS/2 data */
uint8_t ps2_read_data(void)
{
    wait_ps2_read();
    return inb(PS2_DATA_PORT);
}

/* Read PS/2 status */
uint8_t ps2_read_status(void)
{ return inb(PS2_STATUS_PORT); }

/* Read PS/2 configuration */
uint8_t ps2_read_config(void)
{
    ps2_write_cmd(PS2_CMD_READ_CONFIG);
    return ps2_read_data();
}

/* Sending data to PS/2 */
void ps2_write_data(uint8_t data)
{
    wait_ps2_write();
    outb(PS2_DATA_PORT, data);
}

/* Sending commands to PS/2 */
void ps2_write_cmd(uint8_t cmd)
{
    wait_ps2_write();
    outb(PS2_STATUS_PORT, cmd);
}

/* Sending configuration to PS/2 */
void ps2_write_config(uint8_t config)
{
    ps2_write_cmd(PS2_CMD_WRITE_CONFIG);
    ps2_write_data(config);
}

/* Initialize the PS/2 controller */
void init_ps2(void)
{
    /* Disable all ports */
    ps2_write_cmd(PS2_CMD_DISABLE_PORT1);
    ps2_write_cmd(PS2_CMD_DISABLE_PORT2);

    /* Flush the output buffer */
    for (size_t i = 0; i < 10000; ++i) {
        if (ps2_read_status() & PS2_STATUS_OUTPUT_FULL) ps2_read_data();
    }

    /* Disable all IRQs and clocks */
    uint8_t config = ps2_read_config();
    config &= ~PS2_CONFIG_PORT1_IRQ;
    config &= ~PS2_CONFIG_PORT2_IRQ;
    config &= ~PS2_CONFIG_PORT1_CLOCK;
    config &= ~PS2_CONFIG_PORT2_CLOCK;
    ps2_write_config(config);

    /* Controller self-test */
    ps2_write_cmd(PS2_CMD_SELF_TEST);
    if (ps2_read_data() != PS2_RESPONSE_SELFTEST) {
        plogk("ps/2: Controller self-test failed.\n");
    } else {
        plogk("ps/2: Controller self-test successful.\n");
    }

    /* Detect whether it is a dual-channel controller */
    ps2_write_cmd(PS2_CMD_ENABLE_PORT2);
    int is_dual_channel = (ps2_read_config() & PS2_CONFIG_PORT2_CLOCK) == 0;

    if (!is_dual_channel) {
        ps2_write_cmd(PS2_CMD_DISABLE_PORT2);
        plogk("ps/2: Single channel controller detected.\n");
    } else {
        plogk("ps/2: Dual channel controller detected.\n");
    }

    /* Test port */
    ps2_write_cmd(PS2_CMD_TEST_PORT1);
    if (ps2_read_data() != PS2_RESPONSE_TEST) plogk("ps/2: Port 1 test failed.\n");
    if (is_dual_channel) {
        ps2_write_cmd(PS2_CMD_TEST_PORT2);
        if (ps2_read_data() != PS2_RESPONSE_TEST) plogk("ps/2: Port 2 test failed.\n");
    }

    /* Enable all ports, IRQs, and clocks */
    ps2_write_cmd(PS2_CMD_ENABLE_PORT1);
    if (is_dual_channel) ps2_write_cmd(PS2_CMD_ENABLE_PORT2);

    uint8_t final_config = ps2_read_config();
    final_config |= PS2_CONFIG_PORT1_IRQ;
    final_config |= PS2_CONFIG_PORT2_IRQ;
    final_config |= PS2_CONFIG_PORT1_CLOCK;
    final_config |= PS2_CONFIG_PORT2_CLOCK;
    ps2_write_config(final_config);

    register_interrupt_handler(IRQ_1, (void *)ps2kbd_irq, 0, 0x8e);
    wait_queue_init(&ps2kbd_event_wait);
}

size_t ps2kbd_read_events(void *ctx, void *addr, size_t offset, size_t size)
{
    size_t            count;
    input_event_t *out = addr;

    (void)ctx;
    if (!addr) return 0;
    if (offset != 0 || size < sizeof(input_event_t)) return 0;

    spin_lock(&ps2kbd_event_lock);
    count = size / sizeof(input_event_t);
    for (size_t i = 0; i < count; i++) {
        if (ps2kbd_event_tail == ps2kbd_event_head) {
            spin_unlock(&ps2kbd_event_lock);
            return i * sizeof(input_event_t);
        }

        out[i]          = ps2kbd_events[ps2kbd_event_tail];
        ps2kbd_event_tail = (ps2kbd_event_tail + 1) % PS2_KBD_EVENT_QUEUE_SIZE;
    }
    spin_unlock(&ps2kbd_event_lock);
    return count * sizeof(input_event_t);
}

int ps2kbd_poll_events(void *ctx, size_t events)
{
    int ready = 0;

    (void)ctx;
    spin_lock(&ps2kbd_event_lock);
    if ((events & 0x0001) && ps2kbd_event_tail != ps2kbd_event_head) ready |= 0x0001;
    if (events & 0x0004) ready |= 0x0004;
    spin_unlock(&ps2kbd_event_lock);
    return ready;
}

int ps2kbd_wait_events(void)
{
    int available;

    disable_intr();
    spin_lock(&ps2kbd_event_lock);
    available = ps2kbd_event_tail != ps2kbd_event_head;
    spin_unlock(&ps2kbd_event_lock);

    if (!available) wait_queue_wait(&ps2kbd_event_wait);
    enable_intr();
    return 0;
}
