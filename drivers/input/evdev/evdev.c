/*
 *
 *      evdev.c
 *      Linux-compatible evdev input event subsystem
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/base/device.h>
#include <drivers/firmware/acpi.h>
#include <drivers/input/evdev/evdev.h>
#include <drivers/input/input_event.h>
#include <fs/core/vfs.h>
#include <fs/devtmpfs/devtmpfs.h>
#include <fs/sysfs/input_sysfs.h>
#include <fs/tmpfs/tmpfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/list/intrusive_list.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <process/task.h>
#include <process/uaccess.h>
#include <sync/spin_lock.h>
#include <syscall/poll.h>
#include <syscall/syscall.h>

/* _IOC extraction macros */

#define _IOC_DIR(nr)  (((nr) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(nr) (((nr) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(nr)   (((nr) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(nr) (((nr) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

#define _IOC_DIRMASK  ((1U << _IOC_DIRBITS) - 1)
#define _IOC_TYPEMASK ((1U << _IOC_TYPEBITS) - 1)
#define _IOC_NRMASK   ((1U << _IOC_NRBITS) - 1)
#define _IOC_SIZEMASK ((1U << _IOC_SIZEBITS) - 1)

/* bit operations */

static inline void set_bit(unsigned int nr, uint32_t *addr)
{
    addr[nr / 32] |= (1U << (nr % 32));
}

static inline void clear_bit(unsigned int nr, uint32_t *addr)
{
    addr[nr / 32] &= ~(1U << (nr % 32));
}

static inline bool test_bit(unsigned int nr, const uint32_t *addr)
{
    return (addr[nr / 32] >> (nr % 32)) & 1U;
}

static inline unsigned int roundup_pow_of_two(unsigned int n)
{
    unsigned int r = 1;

    while (r < n) r <<= 1;
    return r;
}

/* global state */

static evdev_t   *evdev_table[EVDEV_MAX_DEVICES];
static spinlock_t evdev_table_lock  = {0};
static bool       evdev_initialized = false;
static bool       evdev_nodes_ready;

/* global LED state shared by every keyboard */

static uint32_t           evdev_led_state[(LED_CNT + 31) / 32];
static evdev_led_notify_t evdev_led_notifies[EVDEV_MAX_DEVICES];
static void              *evdev_led_notify_ctx[EVDEV_MAX_DEVICES];
static size_t             evdev_led_notify_count;

#define EVDEV_MAJOR 13

static int evdev_ungrab(evdev_t *evdev, evdev_client_t *client);

static uint64_t evdev_clock_ns(int clock_type)
{
    if (clock_type == CLOCK_REALTIME) {
        int64_t realtime = timer_realtime_ns();
        return realtime > 0 ? (uint64_t)realtime : 0;
    }

    /*
     * CLOCK_MONOTONIC and CLOCK_BOOTTIME currently share Uinxed's
     * monotonic timebase, matching clock_gettime() and timerfd.
     */
    return timer_monotonic_ns();
}

static int evdev_dev_open(vfs_node_t node, uint64_t flags, void **private_data)
{
    tmpfs_file_t   *file  = node ? node->handle : NULL;
    evdev_t        *evdev = file ? file->device.ctx : NULL;
    evdev_client_t *client;
    int             error;

    if (!private_data || !evdev) return -ENODEV;
    client = evdev_fop_open(evdev, &error);
    if (!client) return error;
    (void)flags;
    *private_data = client;
    return EOK;
}

static void evdev_dev_release(vfs_node_t node, void *private_data)
{
    (void)node;
    evdev_fop_release(private_data);
}

static void evdev_dev_descriptor_close(void *ctx, void *private_data)
{
    (void)ctx;
    evdev_client_t *client = private_data;
    if (!client) return;

    spin_lock(&client->buffer_lock);
    client->revoked = true;
    spin_unlock(&client->buffer_lock);
    (void)evdev_ungrab(client->evdev, client);
    wait_queue_wake_all(&client->wait);
    vfs_poll_source_notify(&client->poll_source, POLLHUP);
}

static int64_t evdev_dev_read(void *ctx, void *private_data, uint64_t flags, void *addr, size_t offset, size_t size)
{
    (void)ctx;
    if (offset) return -ESPIPE;
    return evdev_fop_read(private_data, addr, size, (flags & O_NONBLOCK) != 0);
}

static int64_t evdev_dev_write(void *ctx, void *private_data, uint64_t flags, const void *addr, size_t offset, size_t size)
{
    (void)ctx;
    (void)flags;
    if (offset) return -ESPIPE;
    return evdev_fop_write(private_data, addr, size);
}

static int evdev_dev_poll(void *ctx, void *private_data, uint64_t flags, size_t events)
{
    (void)ctx;
    (void)flags;
    return evdev_fop_poll(private_data, (int)events);
}

static vfs_poll_source_t *evdev_dev_poll_source(void *ctx, void *private_data)
{
    (void)ctx;
    evdev_client_t *client = private_data;
    return client ? &client->poll_source : NULL;
}

static int evdev_dev_ioctl(void *ctx, void *private_data, uint64_t flags, size_t request, void *arg)
{
    (void)ctx;
    (void)flags;
    return evdev_fop_ioctl(private_data, (uint32_t)request, arg);
}

/* evdev_get_mask_cnt */

static size_t evdev_get_mask_cnt(unsigned int type)
{
    static const size_t counts[EV_CNT] = {
        [EV_SYN] = EV_CNT, [EV_KEY] = KEY_CNT, [EV_REL] = REL_CNT, [EV_ABS] = ABS_CNT, [EV_MSC] = MSC_CNT, [EV_SW] = SW_CNT, [EV_LED] = LED_CNT, [EV_SND] = SND_CNT, [EV_FF] = FF_CNT,
    };

    return (type < EV_CNT) ? counts[type] : 0;
}

/* evdev_compute_buffer_size */

static unsigned int evdev_compute_buffer_size(input_dev_t *dev)
{
    unsigned int n_events = dev->hint_events_per_packet * EVDEV_BUF_PACKETS;

    if (n_events < EVDEV_MIN_BUFFER_SIZE) n_events = EVDEV_MIN_BUFFER_SIZE;
#ifdef INPUT_EVDEV_BUFSIZE
    if (n_events < INPUT_EVDEV_BUFSIZE) n_events = INPUT_EVDEV_BUFSIZE;
#endif
    return roundup_pow_of_two(n_events);
}

/* __evdev_is_filtered */

static bool __evdev_is_filtered(evdev_client_t *client, unsigned int type, unsigned int code)
{
    uint32_t *mask;
    size_t    cnt;

    if (type == EV_SYN || type >= EV_CNT) return false;

    mask = client->evmasks[0];
    if (mask && !test_bit(type, mask)) return true;

    cnt = evdev_get_mask_cnt(type);
    if (!cnt || code >= cnt) return false;

    mask = client->evmasks[type];
    return mask && !test_bit(code, mask);
}

/* __evdev_queue_syn_dropped */

static void __evdev_queue_syn_dropped(evdev_client_t *client)
{
    uint64_t      ns = evdev_clock_ns(client->clk_type);
    input_event_t ev;

    ev.sec   = ns / 1000000000ULL;
    ev.usec  = (ns / 1000ULL) % 1000000ULL;
    ev.type  = EV_SYN;
    ev.code  = SYN_DROPPED;
    ev.value = 0;

    (void)evdev_queue_push(&client->queue, &ev);
}

/*
 * Compact the ring buffer by removing all events that match the newly
 * installed filter mask.  Caller must hold client->buffer_lock.
 */
static void __evdev_flush_queue(evdev_client_t *client, unsigned int type)
{
    evdev_queue_flush_type(&client->queue, type);
}

/* __pass_event */

static bool __pass_event(evdev_client_t *client, const input_event_t *event)
{
    return evdev_queue_push(&client->queue, event);
}

/* evdev_pass_values */

static void evdev_pass_values(evdev_client_t *client, const input_event_t *values, unsigned int count)
{
    unsigned int  i;
    bool          wake = false;
    input_event_t event;
    uint64_t      time_ns;

    spin_lock(&client->buffer_lock);
    if (client->revoked) {
        spin_unlock(&client->buffer_lock);
        return;
    }
    time_ns = evdev_clock_ns(client->clk_type);
    for (i = 0; i < count; i++) {
        event = values[i];

        /* Timestamp the event */
        event.sec  = time_ns / 1000000000ULL;
        event.usec = (time_ns / 1000ULL) % 1000000ULL;

        /* Filter check */
        if (__evdev_is_filtered(client, event.type, event.code)) continue;

        /*
         * A client becomes readable only when a non-empty frame is committed.
         * This is the Linux evdev packet_head/SYN_REPORT contract.
         */
        if (event.type == EV_SYN && event.code == SYN_REPORT) {
            if (client->queue.packet_head == client->queue.head) continue;
            wake = true;
        }
        (void)__pass_event(client, &event);
    }
    spin_unlock(&client->buffer_lock);

    if (wake) {
        wait_queue_wake_all(&client->wait);
        vfs_poll_source_notify(&client->poll_source, POLLIN);
    }
}

/* evdev_events */

static void evdev_events(input_dev_t *dev, const input_event_t *values, unsigned int count)
{
    evdev_t        *evdev = dev->evdev;
    evdev_client_t *client;
    evdev_client_t *grab;
    ilist_node_t   *node;

    if (!evdev || !evdev->exist) return;

    spin_lock(&evdev->client_lock);

    grab = evdev->grab;
    if (grab) {
        /*
         * Exclusive grab: only the grab client gets events.
         * Hold client_lock across pass_values to prevent the grab
         * client from being freed (by evdev_fop_release) while
         * we are writing to it.
         */
        evdev_pass_values(grab, values, count);
        spin_unlock(&evdev->client_lock);
        return;
    }

    /* Distribute to all clients */
    for (node = evdev->client_list.next; node != &evdev->client_list; node = node->next) {
        client = (evdev_client_t *)((uintptr_t)node - offsetof(evdev_client_t, node));
        evdev_pass_values(client, values, count);
    }

    spin_unlock(&evdev->client_lock);
}

/* evdev_set_clk_type */

static int evdev_set_clk_type(evdev_client_t *client, int clk_type)
{
    input_event_t dropped;
    uint64_t      ns;

    if (clk_type != CLOCK_REALTIME && clk_type != CLOCK_MONOTONIC && clk_type != CLOCK_BOOTTIME) return -EINVAL;
    if (client->clk_type == clk_type) return EOK;

    spin_lock(&client->buffer_lock);
    client->clk_type = clk_type;
    ns               = evdev_clock_ns(client->clk_type);
    dropped          = (input_event_t) {
                 .sec   = ns / 1000000000ULL,
                 .usec  = (ns / 1000ULL) % 1000000ULL,
                 .type  = EV_SYN,
                 .code  = SYN_DROPPED,
                 .value = 0,
    };
    evdev_queue_discard_pending(&client->queue, &dropped);
    spin_unlock(&client->buffer_lock);
    return EOK;
}

/* evdev_grab / evdev_ungrab */

static int evdev_grab(evdev_t *evdev, evdev_client_t *client)
{
    spin_lock(&evdev->client_lock);
    if (evdev->grab) {
        spin_unlock(&evdev->client_lock);
        return -EBUSY;
    }
    evdev->grab = client;
    spin_unlock(&evdev->client_lock);
    return 0;
}

static int evdev_ungrab(evdev_t *evdev, evdev_client_t *client)
{
    spin_lock(&evdev->client_lock);
    if (evdev->grab != client) {
        spin_unlock(&evdev->client_lock);
        return -EINVAL;
    }
    evdev->grab = NULL;
    spin_unlock(&evdev->client_lock);
    return 0;
}

/* evdev_attach_client / evdev_detach_client */

static void evdev_attach_client(evdev_t *evdev, evdev_client_t *client)
{
    spin_lock(&evdev->client_lock);
    ilist_insert_after(&evdev->client_list, &client->node);
    spin_unlock(&evdev->client_lock);
}

static void evdev_detach_client(evdev_t *evdev, evdev_client_t *client)
{
    spin_lock(&evdev->client_lock);
    if (evdev->grab == client) evdev->grab = NULL;
    ilist_remove(&client->node);
    spin_unlock(&evdev->client_lock);
}

/* evdev_open_device / evdev_close_device */

static int evdev_open_device(evdev_t *evdev)
{
    int ret = 0;

    spin_lock(&evdev->mutex);
    if (!evdev->exist) {
        ret = -ENODEV;
        goto out;
    }
    if (evdev->open_count == 0) {
        /*
         * input_open_device would go here.
         * For now, just count opens.
         */
    }
    evdev->open_count++;
out:
    spin_unlock(&evdev->mutex);
    return ret;
}

static bool evdev_close_device(evdev_t *evdev)
{
    bool destroy;

    spin_lock(&evdev->mutex);
    if (evdev->open_count > 0) evdev->open_count--;
    destroy = !evdev->exist && evdev->open_count == 0;
    spin_unlock(&evdev->mutex);
    return destroy;
}

/* evdev_hangup */

static void evdev_hangup(evdev_t *evdev)
{
    ilist_node_t   *node;
    evdev_client_t *client;

    spin_lock(&evdev->client_lock);
    evdev->exist = false;

    for (node = evdev->client_list.next; node != &evdev->client_list; node = node->next) {
        client = (evdev_client_t *)((uintptr_t)node - offsetof(evdev_client_t, node));
        spin_lock(&client->buffer_lock);
        spin_unlock(&client->buffer_lock);
        wait_queue_wake_all(&client->wait);
        vfs_poll_source_notify(&client->poll_source, POLLHUP);
    }
    spin_unlock(&evdev->client_lock);
}

/* evdev_create */

evdev_t *evdev_create(input_dev_t *dev)
{
    evdev_t *evdev;

    if (!dev) return NULL;

    evdev = malloc(sizeof(evdev_t));
    if (!evdev) return NULL;

    memset(evdev, 0, sizeof(evdev_t));
    evdev->input_dev      = dev;
    evdev->exist          = true;
    evdev->client_list    = (ilist_node_t) {&evdev->client_list, &evdev->client_list};
    evdev->client_lock    = (spinlock_t) {0};
    evdev->mutex          = (spinlock_t) {0};
    evdev->grab           = NULL;
    evdev->open_count     = 0;
    evdev->minor          = -1;
    evdev->node_published = false;
    evdev->node           = NULL;
    evdev->registered     = false;
    evdev->sysfs_device   = NULL;

    return evdev;
}

static void evdev_free(evdev_t *evdev)
{
    input_dev_t *input;

    if (!evdev) return;
    input = evdev->input_dev;
    if (evdev->node) {
        vfs_close(evdev->node);
        evdev->node = NULL;
    }
    free(evdev);
    if (input && input->release) input->release(input);
}

/* evdev_destroy */

void evdev_destroy(evdev_t *evdev)
{
    bool destroy;

    if (!evdev) return;
    if (evdev->registered) {
        evdev_unregister(evdev);
        return;
    }

    evdev_hangup(evdev);
    spin_lock(&evdev->mutex);
    destroy = evdev->open_count == 0;
    spin_unlock(&evdev->mutex);
    if (destroy) evdev_free(evdev);
}

/* evdev_register */

int evdev_register(evdev_t *evdev)
{
    int minor;
    int i;
    int result;

    if (!evdev || !evdev->input_dev || !evdev_initialized || evdev->registered) return -EINVAL;

    spin_lock(&evdev_table_lock);
    minor = -1;
    for (i = 0; i < EVDEV_MAX_DEVICES; i++) {
        if (!evdev_table[i]) {
            minor = i;
            break;
        }
    }
    if (minor < 0) {
        spin_unlock(&evdev_table_lock);
        return -ENFILE;
    }

    evdev->minor       = minor;
    evdev_table[minor] = evdev;
    spin_unlock(&evdev_table_lock);

    spin_lock(&evdev->input_dev->event_lock);
    if (evdev->input_dev->evdev) {
        spin_unlock(&evdev->input_dev->event_lock);
        result = -EBUSY;
        goto rollback_table;
    }
    evdev->input_dev->evdev = evdev;
    evdev->registered       = true;
    spin_unlock(&evdev->input_dev->event_lock);

    result = input_sysfs_register_evdev(evdev);
    if (result != EOK) goto rollback_binding;
    if (evdev_nodes_ready) {
        result = evdev_publish_node(evdev);
        if (result != EOK) goto rollback_sysfs;
    }

    return 0;
rollback_sysfs:
    input_sysfs_unregister_evdev(evdev);
rollback_binding:
    spin_lock(&evdev->input_dev->event_lock);
    if (evdev->input_dev->evdev == evdev) evdev->input_dev->evdev = NULL;
    evdev->registered = false;
    spin_unlock(&evdev->input_dev->event_lock);
rollback_table:
    spin_lock(&evdev_table_lock);
    if (evdev_table[minor] == evdev) evdev_table[minor] = NULL;
    spin_unlock(&evdev_table_lock);
    evdev->minor = -1;
    return result;
}

/* evdev_unregister */

void evdev_unregister(evdev_t *evdev)
{
    char path[32];
    bool destroy;

    if (!evdev) return;

    input_sysfs_unregister_evdev(evdev);
    if (evdev->node_published) {
        (void)snprintf(path, sizeof(path), "/dev/input/event%d", evdev->minor);
        devtmpfs_unregister_char_device(path);
        evdev->node_published = false;
        if (evdev->node) {
            vfs_close(evdev->node);
            evdev->node = NULL;
        }
    }

    spin_lock(&evdev->input_dev->event_lock);
    if (evdev->input_dev->evdev == evdev) evdev->input_dev->evdev = NULL;
    evdev->registered = false;
    spin_unlock(&evdev->input_dev->event_lock);

    spin_lock(&evdev_table_lock);
    if (evdev->minor >= 0 && evdev->minor < EVDEV_MAX_DEVICES && evdev_table[evdev->minor] == evdev) evdev_table[evdev->minor] = NULL;
    spin_unlock(&evdev_table_lock);

    evdev->minor = -1;
    evdev_hangup(evdev);
    spin_lock(&evdev->mutex);
    destroy = evdev->open_count == 0;
    spin_unlock(&evdev->mutex);
    if (destroy) evdev_free(evdev);
}

/* evdev_find_by_minor */

evdev_t *evdev_find_by_minor(int minor)
{
    evdev_t *evdev;

    if (minor < 0 || minor >= EVDEV_MAX_DEVICES) return NULL;

    spin_lock(&evdev_table_lock);
    evdev = evdev_table[minor];
    spin_unlock(&evdev_table_lock);

    return evdev;
}

/* evdev_init */

void evdev_init(void)
{
    int i;

    spin_lock(&evdev_table_lock);
    for (i = 0; i < EVDEV_MAX_DEVICES; i++) evdev_table[i] = NULL;
    evdev_initialized = true;
    spin_unlock(&evdev_table_lock);
}

/* event validation and injection */

static bool evdev_event_supported(const input_dev_t *dev, unsigned int type, unsigned int code)
{
    if (type >= EV_CNT || !test_bit(type, dev->evbit)) return false;

    switch (type) {
        case EV_SYN :
            return code < SYN_CNT;
        case EV_KEY :
            return code < KEY_CNT && test_bit(code, dev->keybit);
        case EV_REL :
            return code < REL_CNT && test_bit(code, dev->relbit);
        case EV_ABS :
            return code < ABS_CNT && test_bit(code, dev->absbit);
        case EV_MSC :
            return code < MSC_CNT && test_bit(code, dev->mscbit);
        case EV_SW :
            return code < SW_CNT && test_bit(code, dev->swbit);
        case EV_LED :
            return code < LED_CNT && test_bit(code, dev->ledbit);
        case EV_SND :
            return code < SND_CNT && test_bit(code, dev->sndbit);
        case EV_FF :
            return code < FF_CNT && test_bit(code, dev->ffbit);
        case EV_REP :
            return code < REP_CNT;
        default :
            return true;
    }
}

static bool evdev_prepare_event(input_dev_t *dev, input_event_t *event)
{
    bool active;

    if (!evdev_event_supported(dev, event->type, event->code)) return false;

    switch (event->type) {
        case EV_KEY :
            if (event->value < 0 || event->value > 2) return false;
            active = test_bit(event->code, dev->key_state);
            if (!event->value) {
                if (!active) return false;
                clear_bit(event->code, dev->key_state);
            } else {
                if (event->value == 2 && !active) return false;
                if (event->value == 1 && active) event->value = 2;
                set_bit(event->code, dev->key_state);
            }
            break;
        case EV_REL :
            if (!event->value) return false;
            break;
        case EV_ABS :
            if (dev->absinfo[event->code].value == event->value) return false;
            dev->absinfo[event->code].value = event->value;
            break;
        case EV_LED :
            active = test_bit(event->code, dev->led_state);
            if (!!event->value == active) return false;
            if (event->value)
                set_bit(event->code, dev->led_state);
            else
                clear_bit(event->code, dev->led_state);
            break;
        case EV_SND :
            active = test_bit(event->code, dev->snd_state);
            if (!!event->value == active) return false;
            if (event->value)
                set_bit(event->code, dev->snd_state);
            else
                clear_bit(event->code, dev->snd_state);
            break;
        case EV_SW :
            active = test_bit(event->code, dev->sw_state);
            if (!!event->value == active) return false;
            if (event->value)
                set_bit(event->code, dev->sw_state);
            else
                clear_bit(event->code, dev->sw_state);
            break;
        case EV_REP :
            dev->rep[event->code] = event->value;
            break;
        default :
            break;
    }
    return true;
}

/* Return the LED index for a lock key, or -1 when it is not a lock key. */
static int evdev_lock_key_led(uint16_t keycode)
{
    switch (keycode) {
        case KEY_NUMLOCK :
            return LED_NUML;
        case KEY_CAPSLOCK :
            return LED_CAPSL;
        case KEY_SCROLLLOCK :
            return LED_SCROLLL;
        default :
            return -1;
    }
}

/* Apply a global LED change: update state, broadcast EV_LED, and notify drivers. */
static void evdev_apply_led(int led, bool on)
{
    input_dev_t       *targets[EVDEV_MAX_DEVICES];
    evdev_led_notify_t notifies[EVDEV_MAX_DEVICES];
    void              *notify_ctx[EVDEV_MAX_DEVICES];
    size_t             target_count = 0;
    size_t             notify_count = 0;
    uint8_t            leds         = 0;

    if (on)
        set_bit(led, evdev_led_state);
    else
        clear_bit(led, evdev_led_state);

    if (test_bit(LED_NUML, evdev_led_state)) leds |= 1U << LED_NUML;
    if (test_bit(LED_CAPSL, evdev_led_state)) leds |= 1U << LED_CAPSL;
    if (test_bit(LED_SCROLLL, evdev_led_state)) leds |= 1U << LED_SCROLLL;

    /* Snapshot devices and notify callbacks, then act outside the table lock. */
    spin_lock(&evdev_table_lock);
    for (size_t i = 0; i < EVDEV_MAX_DEVICES; i++) {
        evdev_t *e = evdev_table[i];
        if (e && e->exist && e->input_dev && test_bit(EV_LED, e->input_dev->evbit)) targets[target_count++] = e->input_dev;
    }
    for (size_t i = 0; i < evdev_led_notify_count; i++) {
        notifies[notify_count]   = evdev_led_notifies[i];
        notify_ctx[notify_count] = evdev_led_notify_ctx[i];
        notify_count++;
    }
    spin_unlock(&evdev_table_lock);

    /* Broadcast the LED event to every keyboard device. */
    for (size_t i = 0; i < target_count; i++) evdev_inject_event(targets[i], EV_LED, (uint16_t)led, on ? 1 : 0);

    /* Notify keyboard drivers so they can push the LED to the hardware. */
    for (size_t i = 0; i < notify_count; i++) notifies[i](notify_ctx[i], leds);
}

/* Register a callback to receive global LED state changes. */
void evdev_register_led_notify(evdev_led_notify_t notify, void *ctx)
{
    if (!notify || evdev_led_notify_count >= EVDEV_MAX_DEVICES) return;
    spin_lock(&evdev_table_lock);
    evdev_led_notifies[evdev_led_notify_count]   = notify;
    evdev_led_notify_ctx[evdev_led_notify_count] = ctx;
    evdev_led_notify_count++;
    spin_unlock(&evdev_table_lock);
}

/* Unregister a previously registered LED notify callback. */
void evdev_unregister_led_notify(evdev_led_notify_t notify, void *ctx)
{
    spin_lock(&evdev_table_lock);
    for (size_t i = 0; i < evdev_led_notify_count; i++) {
        if (evdev_led_notifies[i] == notify && evdev_led_notify_ctx[i] == ctx) {
            evdev_led_notify_count--;
            evdev_led_notifies[i]   = evdev_led_notifies[evdev_led_notify_count];
            evdev_led_notify_ctx[i] = evdev_led_notify_ctx[evdev_led_notify_count];
            break;
        }
    }
    spin_unlock(&evdev_table_lock);
}

void evdev_inject_events(input_dev_t *dev, const input_event_t *events, size_t count)
{
    input_event_t prepared[64];
    size_t        offset = 0;

    if (!dev || !events || !count) return;

    /* Detect lock-key presses and apply the global LED change. */
    for (size_t i = 0; i < count; i++) {
        if (events[i].type == EV_KEY && events[i].value == 1) {
            int led = evdev_lock_key_led(events[i].code);
            if (led >= 0) evdev_apply_led(led, !test_bit(led, evdev_led_state));
        }
    }

    spin_lock(&dev->event_lock);
    while (offset < count) {
        size_t prepared_count = 0;

        while (offset < count && prepared_count < sizeof(prepared) / sizeof(prepared[0])) {
            input_event_t event = events[offset++];

            event.sec  = 0;
            event.usec = 0;
            if (evdev_prepare_event(dev, &event)) prepared[prepared_count++] = event;
        }
        if (prepared_count) evdev_events(dev, prepared, (unsigned int)prepared_count);
    }
    spin_unlock(&dev->event_lock);
}

void evdev_inject_event(input_dev_t *dev, uint16_t type, uint16_t code, int32_t value)
{
    input_event_t event = {.type = type, .code = code, .value = value};

    evdev_inject_events(dev, &event, 1);
}

/* evdev_inject_syn */

void evdev_inject_syn(input_dev_t *dev)
{
    evdev_inject_event(dev, EV_SYN, SYN_REPORT, 0);
}

/* evdev_fop_open */

evdev_client_t *evdev_fop_open(evdev_t *evdev, int *error)
{
    evdev_client_t *client;
    unsigned int    bufsize;
    size_t          client_size;
    int             ret;

    if (error) *error = -ENODEV;
    if (!evdev || !evdev->exist) return NULL;

    ret = evdev_open_device(evdev);
    if (ret < 0) {
        plogk("evdev: Open of \"%s\" failed: %d\n", evdev->input_dev->name, ret);
        if (error) *error = ret;
        return NULL;
    }

    bufsize     = evdev_compute_buffer_size(evdev->input_dev);
    client_size = sizeof(evdev_client_t) + bufsize * sizeof(input_event_t);

    client = malloc(client_size);
    if (!client) {
        plogk("evdev: Failed to allocate client for \"%s\" (%u bytes)\n", evdev->input_dev->name, client_size);
        if (error) *error = -ENOMEM;
        if (evdev_close_device(evdev)) evdev_free(evdev);
        return NULL;
    }

    memset(client, 0, client_size);
    client->evdev       = evdev;
    client->buffer_lock = (spinlock_t) {0};
    client->clk_type    = CLOCK_REALTIME;
    client->revoked     = false;
    if (!evdev_queue_init(&client->queue, client->buffer, bufsize)) {
        plogk("evdev: Queue init failed for \"%s\" (size %u)\n", evdev->input_dev->name, bufsize);
        free(client);
        if (error) *error = -EINVAL;
        if (evdev_close_device(evdev)) evdev_free(evdev);
        return NULL;
    }
    wait_queue_init(&client->wait);
    vfs_poll_source_init(&client->poll_source);

    evdev_attach_client(evdev, client);

    if (error) *error = EOK;
    return client;
}

/* evdev_fop_release */

void evdev_fop_release(evdev_client_t *client)
{
    evdev_t *evdev;
    bool     destroy;
    int      i;

    if (!client) return;

    evdev = client->evdev;

    vfs_poll_source_close(&client->poll_source, POLLHUP);
    evdev_detach_client(evdev, client);

    /* Free event filter masks */
    for (i = 0; i < EV_CNT; i++) {
        if (client->evmasks[i]) {
            free(client->evmasks[i]);
            client->evmasks[i] = NULL;
        }
    }

    destroy = evdev_close_device(evdev);
    free(client);
    if (destroy) evdev_free(evdev);
}

/* evdev_fop_read */

ssize_t evdev_fop_read(evdev_client_t *client, void *buf, size_t count, bool nonblock)
{
    evdev_t       *evdev;
    input_event_t *dst;
    size_t         max_events;
    size_t         read_count;

    if (!client) return -EINVAL;
    evdev      = client->evdev;
    dst        = (input_event_t *)buf;
    max_events = count / sizeof(input_event_t);

    if (!evdev->exist) return -ENODEV;

    if (client->revoked) return -ENODEV;
    if (count == 0) return 0;
    if (max_events == 0 || !dst) return -EINVAL;

    spin_lock(&client->buffer_lock);

    while (!evdev_queue_has_packet(&client->queue)) {
        if (nonblock) {
            spin_unlock(&client->buffer_lock);
            return -EAGAIN;
        }

        if (!evdev->exist) {
            spin_unlock(&client->buffer_lock);
            return -ENODEV;
        }

        if (client->revoked) {
            spin_unlock(&client->buffer_lock);
            return -ENODEV;
        }

        wait_queue_prepare(&client->wait);
        spin_unlock(&client->buffer_lock);
        wait_queue_sleep();

        if (!evdev->exist) return -ENODEV;

        if (client->revoked) return -ENODEV;

        spin_lock(&client->buffer_lock);
    }

    read_count = evdev_queue_read(&client->queue, dst, max_events);
    spin_unlock(&client->buffer_lock);

    return (ssize_t)(read_count * sizeof(input_event_t));
}

/* evdev_fop_write */

ssize_t evdev_fop_write(evdev_client_t *client, const void *buf, size_t count)
{
    evdev_t             *evdev;
    const input_event_t *src;
    size_t               n_events;

    if (!client || (!buf && count)) return -EINVAL;
    evdev    = client->evdev;
    src      = (const input_event_t *)buf;
    n_events = count / sizeof(input_event_t);
    if (count == 0) return 0;
    if (n_events == 0) return -EINVAL;

    if (!evdev->exist) return -ENODEV;

    if (client->revoked) return -ENODEV;

    evdev_inject_events(evdev->input_dev, src, n_events);

    return (ssize_t)(n_events * sizeof(input_event_t));
}

/* evdev_fop_poll */

int evdev_fop_poll(evdev_client_t *client, int events)
{
    evdev_t *evdev;
    int      revents = 0;

    (void)events;
    if (!client) return POLLHUP;
    evdev = client->evdev;

    if (client->revoked || !evdev->exist)
        revents |= POLLHUP;
    else
        revents |= POLLOUT;

    spin_lock(&client->buffer_lock);
    if (evdev_queue_has_packet(&client->queue)) revents |= POLLIN;
    spin_unlock(&client->buffer_lock);

    return revents;
}

/* evdev_fop_ioctl */

static int evdev_copy_string_to_user(void *arg, const char *string, size_t maxlen)
{
    size_t length = strlen(string) + 1;

    if (length > maxlen) length = maxlen;
    if (!length) return 0;
    return copy_to_user(arg, string, length) ? -EFAULT : (int)length;
}

static int evdev_fill_user(void *arg, uint8_t value, size_t size);

static int evdev_copy_bits_to_user(void *arg, const uint32_t *bits, size_t bit_count, size_t maxlen)
{
    size_t data_length = (bit_count + 7) / 8;
    size_t word_bits   = sizeof(unsigned long) * 8;
    size_t length      = ((bit_count + word_bits - 1) / word_bits) * sizeof(unsigned long);
    size_t copy_length;

    if (length > maxlen) length = maxlen;
    if (!length) return 0;
    copy_length = data_length < length ? data_length : length;
    if (copy_length && copy_to_user(arg, bits, copy_length)) return -EFAULT;
    if (copy_length < length && evdev_fill_user((uint8_t *)arg + copy_length, 0, length - copy_length) != EOK) return -EFAULT;
    return (int)length;
}

static int evdev_get_state(evdev_client_t *client, input_dev_t *dev, unsigned int type, const uint32_t *state, size_t bit_count, void *arg, size_t maxlen)
{
    size_t    bytes = (bit_count + 7) / 8;
    uint32_t *snapshot;
    int       result;

    snapshot = malloc(bytes);
    if (!snapshot) return -ENOMEM;

    spin_lock(&dev->event_lock);
    spin_lock(&client->buffer_lock);
    memcpy(snapshot, state, bytes);
    spin_unlock(&dev->event_lock);
    __evdev_flush_queue(client, type);
    spin_unlock(&client->buffer_lock);

    result = evdev_copy_bits_to_user(arg, snapshot, bit_count, maxlen);
    if (result < 0) {
        spin_lock(&client->buffer_lock);
        __evdev_queue_syn_dropped(client);
        spin_unlock(&client->buffer_lock);
    }
    free(snapshot);
    return result;
}

static int evdev_set_mask(evdev_client_t *client, const input_mask_t *descriptor)
{
    size_t    bit_count = evdev_get_mask_cnt(descriptor->type);
    size_t    bytes;
    size_t    copy_bytes;
    uint32_t *replacement;
    uint32_t *old;

    if (!bit_count) return EOK;
    if (descriptor->codes_size % sizeof(unsigned long)) return -EINVAL;
    bytes       = (bit_count + 7) / 8;
    copy_bytes  = descriptor->codes_size < bytes ? descriptor->codes_size : bytes;
    replacement = malloc(bytes);
    if (!replacement) return -ENOMEM;
    memset(replacement, 0, bytes);
    if (copy_bytes && copy_from_user(replacement, (const void *)(uintptr_t)descriptor->codes_ptr, copy_bytes)) {
        free(replacement);
        return -EFAULT;
    }

    spin_lock(&client->buffer_lock);
    old                               = client->evmasks[descriptor->type];
    client->evmasks[descriptor->type] = replacement;
    spin_unlock(&client->buffer_lock);
    free(old);
    return EOK;
}

static int evdev_fill_user(void *arg, uint8_t value, size_t size)
{
    uint8_t bytes[32];

    memset(bytes, value, sizeof(bytes));
    for (size_t offset = 0; offset < size; offset += sizeof(bytes)) {
        size_t count = size - offset;

        if (count > sizeof(bytes)) count = sizeof(bytes);
        if (copy_to_user((uint8_t *)arg + offset, bytes, count)) return -EFAULT;
    }
    return EOK;
}

static int evdev_get_mask(evdev_client_t *client, const input_mask_t *descriptor)
{
    size_t    bit_count  = evdev_get_mask_cnt(descriptor->type);
    size_t    bytes      = (bit_count + 7) / 8;
    size_t    word_bits  = sizeof(unsigned long) * 8;
    size_t    user_bytes = ((bit_count + word_bits - 1) / word_bits) * sizeof(unsigned long);
    size_t    copy_bytes;
    uint32_t *snapshot = NULL;
    void     *user     = (void *)(uintptr_t)descriptor->codes_ptr;
    bool      allow_all;
    int       result;

    if (!descriptor->codes_size) return EOK;
    copy_bytes = descriptor->codes_size < user_bytes ? descriptor->codes_size : user_bytes;
    if (copy_bytes) {
        snapshot = malloc(bytes);
        if (!snapshot) return -ENOMEM;
        spin_lock(&client->buffer_lock);
        allow_all = client->evmasks[descriptor->type] == NULL;
        if (!allow_all)
            memcpy(snapshot, client->evmasks[descriptor->type], bytes);
        else
            memset(snapshot, 0xff, bytes);
        spin_unlock(&client->buffer_lock);
        if (allow_all)
            result = evdev_fill_user(user, 0xff, copy_bytes);
        else
            result = evdev_copy_bits_to_user(user, snapshot, bit_count, copy_bytes) < 0 ? -EFAULT : EOK;
        free(snapshot);
        if (result != EOK) return result;
    }
    if (copy_bytes < descriptor->codes_size) return evdev_fill_user((uint8_t *)user + copy_bytes, 0, descriptor->codes_size - copy_bytes);
    return EOK;
}

int evdev_fop_ioctl(evdev_client_t *client, uint32_t request, void *arg)
{
    evdev_t     *evdev;
    input_dev_t *dev;
    size_t       len;
    unsigned int ev_type;
    int          ival;

    if (!client) return -EINVAL;
    evdev = client->evdev;
    dev   = evdev->input_dev;
    if (!evdev->exist) return -ENODEV;

    if (client->revoked) return -ENODEV;

    switch (request) {
        case EVIOCGVERSION : {
            int32_t version = EV_VERSION;

            return copy_to_user(arg, &version, sizeof(version)) ? -EFAULT : EOK;
        }
        case EVIOCGID :
            return copy_to_user(arg, &dev->id, sizeof(dev->id)) ? -EFAULT : EOK;

        case EVIOCGREP : {
            int repeat[2];

            if (!test_bit(EV_REP, dev->evbit)) return -ENOSYS;
            spin_lock(&dev->event_lock);
            memcpy(repeat, dev->rep, sizeof(repeat));
            spin_unlock(&dev->event_lock);
            return copy_to_user(arg, repeat, sizeof(repeat)) ? -EFAULT : EOK;
        }

        case EVIOCSREP : {
            int           repeat[2];
            input_event_t events[2];

            if (!test_bit(EV_REP, dev->evbit)) return -ENOSYS;
            if (copy_from_user(repeat, arg, sizeof(repeat))) return -EFAULT;
            events[0] = (input_event_t) {.type = EV_REP, .code = REP_DELAY, .value = repeat[0]};
            events[1] = (input_event_t) {.type = EV_REP, .code = REP_PERIOD, .value = repeat[1]};
            evdev_inject_events(dev, events, 2);
            return EOK;
        }
        case EVIOCGKEYCODE :
        case EVIOCGKEYCODE_V2 :
        case EVIOCSKEYCODE :
        case EVIOCSKEYCODE_V2 :
        case EVIOCSFF :
        case EVIOCRMFF :
        case EVIOCGEFFECTS :
            return -EINVAL;
        default :
            break;
    }

    /* Handle variable-length ioctls by extracting the _IOC_NR */

    if (_IOC_TYPE(request) != 'E') return -EINVAL;

    switch (_IOC_NR(request)) {
        case 0x06 : // EVIOCGNAME(len)
            if (!(_IOC_DIR(request) & _IOC_READ)) return -EINVAL;
            len = _IOC_SIZE(request);
            return evdev_copy_string_to_user(arg, dev->name, len);
        case 0x07 : // EVIOCGPHYS(len)
            if (!(_IOC_DIR(request) & _IOC_READ)) return -EINVAL;
            len = _IOC_SIZE(request);
            return evdev_copy_string_to_user(arg, dev->phys, len);
        case 0x08 : // EVIOCGUNIQ(len)
            if (!(_IOC_DIR(request) & _IOC_READ)) return -EINVAL;
            len = _IOC_SIZE(request);
            return evdev_copy_string_to_user(arg, dev->uniq, len);
        case 0x09 : // EVIOCGPROP(len)
            if (!(_IOC_DIR(request) & _IOC_READ)) return -EINVAL;
            len = _IOC_SIZE(request);
            return evdev_copy_bits_to_user(arg, dev->propbit, INPUT_PROP_CNT, len);
        case 0x18 : // EVIOCGKEY(len)
            if (!(_IOC_DIR(request) & _IOC_READ)) return -EINVAL;
            len = _IOC_SIZE(request);
            return evdev_get_state(client, dev, EV_KEY, dev->key_state, KEY_CNT, arg, len);
        case 0x19 : // EVIOCGLED(len) / EVIOCSLED(len)
            len = _IOC_SIZE(request);
            if (_IOC_DIR(request) & _IOC_READ) return evdev_get_state(client, dev, EV_LED, dev->led_state, LED_CNT, arg, len);
            if (_IOC_DIR(request) & _IOC_WRITE) {
                uint32_t leds[(LED_CNT + 31) / 32] = {0};
                if (len > sizeof(leds)) len = sizeof(leds);
                if (copy_from_user(leds, arg, len)) return -EFAULT;
                for (int led = 0; led < LED_CNT; led++) {
                    bool on = test_bit(led, leds);
                    if (on != test_bit(led, evdev_led_state)) evdev_apply_led(led, on);
                }
                return EOK;
            }
            return -EINVAL;
        case 0x1a : // EVIOCGSND(len)
            if (!(_IOC_DIR(request) & _IOC_READ)) return -EINVAL;
            len = _IOC_SIZE(request);
            return evdev_get_state(client, dev, EV_SND, dev->snd_state, SND_CNT, arg, len);
        case 0x1b : // EVIOCGSW(len)
            if (!(_IOC_DIR(request) & _IOC_READ)) return -EINVAL;
            len = _IOC_SIZE(request);
            return evdev_get_state(client, dev, EV_SW, dev->sw_state, SW_CNT, arg, len);
        default :
            break;
    }

    /* EVIOCGBIT(ev, len) and EVIOCGABS/EVIOCSABS */
    ev_type = _IOC_NR(request);
    if (ev_type >= 0x20 && ev_type <= 0x3f) {
        /* EVIOCGBIT(ev, len) */
        unsigned int    ev  = ev_type - 0x20;
        size_t          cnt = evdev_get_mask_cnt(ev);
        const uint32_t *src = NULL;

        if (!(_IOC_DIR(request) & _IOC_READ)) return -EINVAL;
        len = _IOC_SIZE(request);

        switch (ev) {
            case 0 :
                src = dev->evbit;
                cnt = EV_CNT;
                break;
            case EV_KEY :
                src = dev->keybit;
                break;
            case EV_REL :
                src = dev->relbit;
                break;
            case EV_ABS :
                src = dev->absbit;
                break;
            case EV_MSC :
                src = dev->mscbit;
                break;
            case EV_LED :
                src = dev->ledbit;
                break;
            case EV_SND :
                src = dev->sndbit;
                break;
            case EV_SW :
                src = dev->swbit;
                break;
            case EV_FF :
                src = dev->ffbit;
                break;
            default :
                return -EINVAL;
        }

        if (!src || cnt == 0) return evdev_fill_user(arg, 0, len) == EOK ? 0 : -EFAULT;
        return evdev_copy_bits_to_user(arg, src, cnt, len);
    }

    if (ev_type >= 0x40 && ev_type <= 0x5f) {
        /* EVIOCGABS(abs) */
        unsigned int    abs = ev_type - 0x40;
        input_absinfo_t info;

        if (!(_IOC_DIR(request) & _IOC_READ) || abs >= ABS_CNT || !test_bit(abs, dev->absbit)) return -EINVAL;
        len = _IOC_SIZE(request);
        if (len > sizeof(info)) len = sizeof(info);
        spin_lock(&dev->event_lock);
        info = dev->absinfo[abs];
        spin_unlock(&dev->event_lock);
        return len && copy_to_user(arg, &info, len) ? -EFAULT : EOK;
    }

    if (ev_type >= 0xc0 && ev_type <= 0xdf) {
        /* EVIOCSABS(abs) */
        unsigned int abs = ev_type - 0xc0;

        input_absinfo_t info = {0};

        if (!(_IOC_DIR(request) & _IOC_WRITE) || abs >= ABS_CNT || !test_bit(abs, dev->absbit)) return -EINVAL;
        len = _IOC_SIZE(request);
        if (len > sizeof(info)) len = sizeof(info);
        if (len && copy_from_user(&info, arg, len)) return -EFAULT;
        spin_lock(&dev->event_lock);
        dev->absinfo[abs] = info;
        spin_unlock(&dev->event_lock);
        return EOK;
    }

    /* EVIOCGRAB, EVIOCREVOKE, EVIOCGMASK, EVIOCSMASK, EVIOCSCLOCKID */
    switch (request) {
        case EVIOCGRAB :
            if ((uintptr_t)arg)
                return evdev_grab(evdev, client);
            else
                return evdev_ungrab(evdev, client);
        case EVIOCREVOKE :
            if ((uintptr_t)arg) return -EINVAL;
            spin_lock(&client->buffer_lock);
            client->revoked = true;
            spin_unlock(&client->buffer_lock);
            (void)evdev_ungrab(evdev, client);
            wait_queue_wake_all(&client->wait);
            vfs_poll_source_notify(&client->poll_source, POLLHUP);
            return EOK;
        case EVIOCSCLOCKID :
            if (copy_from_user(&ival, arg, sizeof(ival))) return -EFAULT;
            return evdev_set_clk_type(client, ival);

        case EVIOCGMASK : {
            input_mask_t mask;

            if (copy_from_user(&mask, arg, sizeof(mask))) return -EFAULT;
            return evdev_get_mask(client, &mask);
        }

        case EVIOCSMASK : {
            input_mask_t mask;

            if (copy_from_user(&mask, arg, sizeof(mask))) return -EFAULT;
            return evdev_set_mask(client, &mask);
        }
        default :
            return -EINVAL;
    }
}

uint64_t evdev_devt(const evdev_t *evdev)
{
    if (!evdev || evdev->minor < 0) return 0;
    return MKDEV(EVDEV_MAJOR, EVDEV_MINOR_BASE + evdev->minor);
}

int evdev_publish_node(evdev_t *evdev)
{
    char     path[32];
    int      result;
    uint16_t node_type = file_stream;

    /*
     * Let poll/epoll users identify input fds without relying on the
     * pathname.  Relative-axis devices are mice/pointers; key-only devices
     * are keyboards.
     */
    if (test_bit(EV_REL, evdev->input_dev->evbit))
        node_type |= file_mouse;
    else if (test_bit(EV_KEY, evdev->input_dev->evbit))
        node_type |= file_keyboard;

    const tmpfs_device_ops_t ops = {
        .open             = evdev_dev_open,
        .release          = evdev_dev_release,
        .descriptor_close = evdev_dev_descriptor_close,
        .file_read        = evdev_dev_read,
        .file_write       = evdev_dev_write,
        .file_poll        = evdev_dev_poll,
        .file_poll_source = evdev_dev_poll_source,
        .file_ioctl       = evdev_dev_ioctl,
        .ctx              = evdev,
    };

    if (!evdev || !evdev->exist) return -ENODEV;
    if (evdev->node_published) return EOK;
    (void)snprintf(path, sizeof(path), "/dev/input/event%d", evdev->minor);
    result = devtmpfs_register_char_device(path, evdev_devt(evdev), evdev_devt(evdev), node_type, &ops);
    if (result == EOK) {
        evdev->node = vfs_open_nofollow(path);
        if (!evdev->node) {
            (void)devtmpfs_unregister_char_device(path);
            return -ENOENT;
        }
        evdev->node_published = true;
    }
    return result;
}

int evdev_publish_nodes(void)
{
    int count = 0;

    evdev_nodes_ready = true;
    for (int minor = 0; minor < EVDEV_MAX_DEVICES; minor++) {
        evdev_t *evdev = evdev_find_by_minor(minor);
        if (evdev && evdev_publish_node(evdev) == EOK) count++;
    }
    return count;
}
