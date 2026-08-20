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
#include <drivers/input/ps2/ps2_event_ring.h>
#include <kernel/errno.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <process/kthread.h>
#include <sync/spin_lock.h>

static bool ps2_port1_ok;
static bool ps2_port2_ok;

#define PS2_PORT_KEYBOARD    0U
#define PS2_PORT_MOUSE       1U
#define PS2_PORT_COUNT       2U
#define PS2_IRQ_DRAIN_BUDGET 64U
#define PS2_WAKE_KEYBOARD    (1U << PS2_PORT_KEYBOARD)
#define PS2_WAKE_MOUSE       (1U << PS2_PORT_MOUSE)

struct ps2_event_queue {
        struct ps2_event_ring ring;
        wait_queue_t          wait;
        spinlock_t            sleep_lock;
        volatile bool         sleeping;
        volatile uint64_t     dropped;
        bool                  resync_pending;
        bool                  second_port;
        bool                  worker_registered;
};

static struct ps2_event_queue ps2_event_queues[PS2_PORT_COUNT];

/* Serializes the shared status/data registers across IRQ1, IRQ12 and commands. */
static spinlock_t ps2_controller_lock;

/* Publish after the data-port read; the caller holds ps2_controller_lock. */
static unsigned int ps2_queue_input_locked(uint8_t status, uint8_t data)
{
    unsigned int            port  = (status & PS2_STATUS_AUX_DATA) ? PS2_PORT_MOUSE : PS2_PORT_KEYBOARD;
    struct ps2_event_queue *queue = &ps2_event_queues[port];

    /*
     * Mark the first byte after overflow as unusable so the decoder resets at
     * the exact stream gap instead of interpreting a stale prefix/packet.
     */
    if (queue->resync_pending) status |= PS2_STATUS_TIMEOUT;
    if (!ps2_event_ring_push(&queue->ring, (struct ps2_queued_byte) {.status = status, .data = data})) {
        __atomic_add_fetch(&queue->dropped, 1, __ATOMIC_RELAXED);
        queue->resync_pending = true;
        return 0;
    }
    queue->resync_pending = false;

    /* Exchange coalesces a burst into at most one scheduler wakeup. */
    if (!__atomic_exchange_n(&queue->sleeping, false, __ATOMIC_ACQ_REL)) return 0;
    return 1U << port;
}

/* Wake the port worker that is sleeping in the wait queue. */
static void ps2_wake_input_queue(struct ps2_event_queue *queue)
{
    /* Pairs with the worker's arm/recheck/prepare sequence. */
    spin_lock(&queue->sleep_lock);
    (void)wait_queue_wake_one_sync(&queue->wait);
    spin_unlock(&queue->sleep_lock);
}

/* Wake the workers of the ports flagged in wake_mask. */
static void ps2_wake_inputs(unsigned int wake_mask)
{
    if (wake_mask & PS2_WAKE_KEYBOARD) ps2_wake_input_queue(&ps2_event_queues[PS2_PORT_KEYBOARD]);
    if (wake_mask & PS2_WAKE_MOUSE) ps2_wake_input_queue(&ps2_event_queues[PS2_PORT_MOUSE]);
}

/* Drain this port's event ring and feed bytes to its decoder. */
static int ps2_input_worker(void *arg)
{
    struct ps2_event_queue *queue = arg;
    struct ps2_queued_byte  events[PS2_EVENT_BATCH_SIZE];

    while (!kthread_should_stop()) {
        size_t count = ps2_event_ring_pop_batch(&queue->ring, events, PS2_EVENT_BATCH_SIZE);

        if (!count) {
            /*
             * Arm first, then recheck under sleep_lock: a producer can neither
             * miss this sleeper nor wake it before prepare has published it.
             */
            spin_lock(&queue->sleep_lock);
            __atomic_store_n(&queue->sleeping, true, __ATOMIC_RELEASE);
            if (!ps2_event_ring_empty(&queue->ring) || kthread_should_stop()) {
                __atomic_store_n(&queue->sleeping, false, __ATOMIC_RELEASE);
                spin_unlock(&queue->sleep_lock);
                continue;
            }
            wait_queue_prepare(&queue->wait);
            spin_unlock(&queue->sleep_lock);
            wait_queue_sleep();
            __atomic_store_n(&queue->sleeping, false, __ATOMIC_RELEASE);
            continue;
        }

        for (size_t i = 0; i < count; i++) {
            if (events[i].status & (PS2_STATUS_TIMEOUT | PS2_STATUS_PARITY)) {
                if (queue->second_port)
                    ps2_mouse_reset_stream();
                else
                    ps2_keyboard_reset_stream();
                continue;
            }
            if (queue->second_port)
                ps2_mouse_handle_byte(events[i].data);
            else
                ps2_keyboard_handle_byte(events[i].data);
        }
    }
    return 0;
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
    uint8_t      response;
    unsigned int wake_mask = 0;
    int          result    = -EIO;

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
                wake_mask |= ps2_queue_input_locked(status, response);
                continue;
            }
            if (((status & PS2_STATUS_AUX_DATA) != 0) != second_port) {
                wake_mask |= ps2_queue_input_locked(status, response);
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
            wake_mask |= ps2_queue_input_locked(status, response);
        }
    }
out:
    spin_unlock(&ps2_controller_lock);
    ps2_wake_inputs(wake_mask);
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

/* IRQ handler: drain the shared output buffer and defer protocol work. */
INTERRUPT_BEGIN static void ps2_irq(interrupt_frame_t *frame)
{
    unsigned int wake_mask = 0;

    irq_enter_gs(frame);
    spin_lock(&ps2_controller_lock);
    for (size_t drained = 0; drained < PS2_IRQ_DRAIN_BUDGET; drained++) {
        uint8_t status = ps2_read_status();
        uint8_t data;

        if (!(status & PS2_STATUS_OUTPUT_FULL)) break;
        data = inb(PS2_DATA_PORT);
        wake_mask |= ps2_queue_input_locked(status, data);
    }
    spin_unlock(&ps2_controller_lock);
    send_eoi();
    ps2_wake_inputs(wake_mask);
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Register per-port workers now that kernel workers are available. */
void ps2_start_worker(void)
{
    static const char *names[PS2_PORT_COUNT]     = {"ps2-keyboard", "ps2-mouse"};
    bool               available[PS2_PORT_COUNT] = {ps2_port1_ok, ps2_port2_ok};

    for (size_t port = 0; port < PS2_PORT_COUNT; port++) {
        struct ps2_event_queue *queue = &ps2_event_queues[port];
        if (!available[port] || queue->worker_registered) continue;
        if (kernel_worker_register(names[port], ps2_input_worker, queue, NULL) != EOK) {
            plogk("ps2: Unable to register deferred %s worker.\n", port == PS2_PORT_MOUSE ? "mouse" : "keyboard");
            continue;
        }
        queue->worker_registered = true;
    }
}

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
    for (size_t port = 0; port < PS2_PORT_COUNT; port++) {
        struct ps2_event_queue *queue = &ps2_event_queues[port];

        ps2_event_ring_init(&queue->ring);
        wait_queue_init(&queue->wait);
        queue->sleep_lock        = (spinlock_t) {0};
        queue->sleeping          = false;
        queue->dropped           = 0;
        queue->resync_pending    = false;
        queue->second_port       = port == PS2_PORT_MOUSE;
        queue->worker_registered = false;
    }
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
