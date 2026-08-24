/*
 *
 *      loopback.c
 *      Loopback network device
 *
 *      2026/8/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdbool.h>
#include <libs/std/string.h>
#include <net/core/ethernet.h>
#include <net/core/netdev.h>
#include <process/kthread.h>
#include <process/sched.h>

#define LOOPBACK_ADDRESS   0x7f000001u /* 127.0.0.1 */
#define LOOPBACK_NETMASK   0xff000000u /* 255.0.0.0 */
#define LOOPBACK_QUEUE_MAX 256U
#define LOOPBACK_BYTES_MAX (2U * 1024U * 1024U)
#define LOOPBACK_RX_BUDGET 64U

/* Locally administered unicast address for the internal Ethernet shim. */
static const uint8_t loopback_mac[ETH_ADDRESS_LEN] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

static netdev_t loopback_device;

typedef struct loopback_queue_entry {
        net_pbuf_t *packet;
        uint32_t    generation;
} loopback_queue_entry_t;

typedef struct loopback_context {
        loopback_queue_entry_t queue[LOOPBACK_QUEUE_MAX];
        uint16_t               head;
        uint16_t               tail;
        uint16_t               count;
        size_t                 bytes;
        uint32_t               generation;
        bool                   ready;
        bool                   enabled;
        spinlock_t             lock;
        wait_queue_t           wait;
        task_t                *worker;
} loopback_context_t;

static loopback_context_t loopback;

/* Bring the device up. */
static int loopback_open(net_device_t *device)
{
    if (!device || !__atomic_load_n(&loopback.ready, __ATOMIC_ACQUIRE)) return -ENODEV;
    spin_lock(&loopback.lock);
    loopback.enabled = true;
    spin_unlock(&loopback.lock);
    return EOK;
}

/* Take the device down and discard frames queued before the transition. */
static void loopback_stop(net_device_t *device)
{
    if (!device) return;

    net_pbuf_t *discard[LOOPBACK_QUEUE_MAX];
    size_t      count = 0;
    spin_lock(&loopback.lock);
    loopback.enabled = false;
    loopback.generation++;
    while (loopback.count) {
        loopback_queue_entry_t *entry = &loopback.queue[loopback.head];
        discard[count++]              = entry->packet;
        entry->packet                 = NULL;
        loopback.head                 = (uint16_t)((loopback.head + 1U) % LOOPBACK_QUEUE_MAX);
        loopback.count--;
    }
    loopback.bytes = 0;
    spin_unlock(&loopback.lock);
    for (size_t i = 0; i < count; i++) net_pbuf_free(discard[i]);
}

/*
 * Transmit by queueing a frame for the loopback RX worker.
 *
 * The transport layer commonly calls ipv4_output() while holding its endpoint
 * lock.  Synchronous RX would recurse into TCP/UDP and deadlock on that same
 * lock (a local TCP SYN/SYN-ACK is the simplest reproducer), so loopback uses
 * the same process-context delivery model as physical NIC drivers.
 */
static int loopback_xmit(net_device_t *device, net_pbuf_t *packet)
{
    if (!device || !packet || !packet->data) return -EINVAL;
    if (!__atomic_load_n(&loopback.ready, __ATOMIC_ACQUIRE)) return -ENETDOWN;
    net_pbuf_t *copy = net_pbuf_clone(packet, NET_PBUF_HEADROOM);
    if (!copy) return -ENOMEM;

    spin_lock(&loopback.lock);
    if (!loopback.enabled) {
        spin_unlock(&loopback.lock);
        net_pbuf_free(copy);
        return -ENETDOWN;
    }
    if (loopback.count >= LOOPBACK_QUEUE_MAX || copy->length > LOOPBACK_BYTES_MAX - loopback.bytes) {
        spin_unlock(&loopback.lock);
        net_pbuf_free(copy);
        return -ENOBUFS;
    }
    loopback_queue_entry_t *entry = &loopback.queue[loopback.tail];
    entry->packet                 = copy;
    entry->generation             = loopback.generation;
    loopback.tail                 = (uint16_t)((loopback.tail + 1U) % LOOPBACK_QUEUE_MAX);
    loopback.count++;
    loopback.bytes += copy->length;
    wait_queue_wake_one_sync(&loopback.wait);
    spin_unlock(&loopback.lock);
    return EOK;
}

/* Validate and store a new MTU (loopback accepts anything the core allows). */
static int loopback_set_mtu(net_device_t *device, uint32_t mtu)
{
    if (!device || mtu < NETDEV_MTU_MIN || mtu > NETDEV_MTU_MAX) return -EINVAL;
    return EOK;
}

static const netdev_ops_t loopback_ops = {
    .open    = loopback_open,
    .stop    = loopback_stop,
    .xmit    = loopback_xmit,
    .set_mtu = loopback_set_mtu,
};

/* Drain queued frames outside every transport caller's lock context. */
static int loopback_worker(void *argument)
{
    (void)argument;

    while (!kthread_should_stop()) {
        unsigned processed = 0;
        while (processed < LOOPBACK_RX_BUDGET) {
            spin_lock(&loopback.lock);
            if (!loopback.count) {
                wait_queue_prepare(&loopback.wait);
                spin_unlock(&loopback.lock);
                wait_queue_sleep();
                break;
            }
            loopback_queue_entry_t entry         = loopback.queue[loopback.head];
            loopback.queue[loopback.head].packet = NULL;
            loopback.head                        = (uint16_t)((loopback.head + 1U) % LOOPBACK_QUEUE_MAX);
            loopback.count--;
            loopback.bytes -= entry.packet->length;
            uint32_t generation = loopback.generation;
            spin_unlock(&loopback.lock);

            if (entry.generation == generation)
                (void)netdev_rx(&loopback_device, entry.packet);
            else
                net_pbuf_free(entry.packet);
            processed++;
        }

        spin_lock(&loopback.lock);
        bool more = loopback.count != 0;
        spin_unlock(&loopback.lock);
        if (more) sched_yield();
    }
    return EOK;
}

/* Create and register the loopback interface with its 127.0.0.1 address. */
void loopback_init(void)
{
#if CONFIG_NET
    static bool initialized;
    if (initialized) return;

    memset(&loopback, 0, sizeof(loopback));
    wait_queue_init(&loopback.wait);
    if (netdev_init(&loopback_device, "lo", &loopback_ops, NULL)) {
        plogk("loopback: Device init failed.\n");
        return;
    }
    memcpy(loopback_device.address, loopback_mac, ETH_ADDRESS_LEN);
    loopback_device.mtu          = NETDEV_MTU_MAX;
    loopback_device.flags        = NETDEV_F_UP | NETDEV_F_RUNNING | NETDEV_F_LOOPBACK;
    loopback_device.ipv4_address = LOOPBACK_ADDRESS;
    loopback_device.ipv4_netmask = LOOPBACK_NETMASK;
    int status                   = netdev_register(&loopback_device);
    if (status) {
        plogk("loopback: Device register failed (%d).\n", status);
        return;
    }

    status = kernel_worker_register("net-loopback", loopback_worker, NULL, &loopback.worker);
    if (status) {
        (void)netdev_unregister(&loopback_device);
        plogk("loopback: Worker registration failed (%d).\n", status);
        return;
    }
    spin_lock(&loopback.lock);
    loopback.enabled = true;
    spin_unlock(&loopback.lock);
    __atomic_store_n(&loopback.ready, true, __ATOMIC_RELEASE);
    initialized = true;
    plogk("loopback: Interface 'lo' registered (127.0.0.1/8).\n");
#endif
}
