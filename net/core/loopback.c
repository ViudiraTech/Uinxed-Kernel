/*
 * Loopback devices are owned by network namespaces. A shared bounded RX queue
 * delivers frames asynchronously: synchronous TCP SYN delivery while holding a
 * transport lock would recurse into that lock. Each queued frame pins both its
 * device and namespace until the RX worker has finished.
 */
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/heap.h>
#include <net/core/ethernet.h>
#include <net/core/loopback.h>
#include <process/kthread.h>
#include <process/namespace.h>
#include <process/sched.h>

#if CONFIG_INET && CONFIG_NET

typedef struct loopback_context {
        net_device_t device;
        uint32_t generation;
        bool enabled;
        spinlock_t lock;
} loopback_context_t;

typedef struct loopback_queue_entry {
        net_pbuf_t *packet;
        net_device_t *device;
        uint32_t generation;
} loopback_queue_entry_t;

static struct {
        loopback_queue_entry_t queue[CONFIG_LOOPBACK_QUEUE_MAX];
        uint16_t head, tail, count;
        size_t bytes;
        bool ready;
        spinlock_t lock;
        wait_queue_t wait;
        task_t *worker;
} loopback;

static int loopback_open(net_device_t *device)
{
    if (!device || !__atomic_load_n(&loopback.ready, __ATOMIC_ACQUIRE)) return -ENODEV;
    loopback_context_t *context = netdev_private(device);
    spin_lock(&context->lock);
    context->enabled = true;
    spin_unlock(&context->lock);
    return EOK;
}

static void loopback_stop(net_device_t *device)
{
    if (!device) return;
    loopback_context_t *context = netdev_private(device);
    spin_lock(&context->lock);
    context->enabled = false;
    context->generation++;
    spin_unlock(&context->lock);
}

static int loopback_xmit(net_device_t *device, net_pbuf_t *packet)
{
    if (!device || !packet || !packet->data) return -EINVAL;
    if (!__atomic_load_n(&loopback.ready, __ATOMIC_ACQUIRE)) return -ENETDOWN;
    net_pbuf_t *copy = net_pbuf_clone(packet, NET_PBUF_HEADROOM);
    if (!copy) return -ENOMEM;
    loopback_context_t *context = netdev_private(device);
    spin_lock(&context->lock);
    if (!context->enabled) {
        spin_unlock(&context->lock);
        net_pbuf_free(copy);
        return -ENETDOWN;
    }
    uint32_t generation = context->generation;
    spin_lock(&loopback.lock);
    if (loopback.count >= CONFIG_LOOPBACK_QUEUE_MAX || copy->length > CONFIG_LOOPBACK_BYTES_MAX - loopback.bytes) {
        spin_unlock(&loopback.lock);
        spin_unlock(&context->lock);
        net_pbuf_free(copy);
        return -ENOBUFS;
    }
    netdev_get(device);
    net_ns_get(device->net_ns);
    loopback.queue[loopback.tail] = (loopback_queue_entry_t) {copy, device, generation};
    loopback.tail = (uint16_t)((loopback.tail + 1U) % CONFIG_LOOPBACK_QUEUE_MAX);
    loopback.count++;
    loopback.bytes += copy->length;
    wait_queue_wake_one_sync(&loopback.wait);
    spin_unlock(&loopback.lock);
    spin_unlock(&context->lock);
    return EOK;
}

static int loopback_set_mtu(net_device_t *device, uint32_t mtu)
{
    return device && mtu >= NETDEV_MTU_MIN && mtu <= CONFIG_NETDEV_MTU_MAX ? EOK : -EINVAL;
}

static const netdev_ops_t loopback_ops = {
    .open = loopback_open, .stop = loopback_stop, .xmit = loopback_xmit, .set_mtu = loopback_set_mtu,
};

static void loopback_release(net_device_t *device)
{
    free(netdev_private(device));
}

static int loopback_worker(void *argument)
{
    (void)argument;
    while (!kthread_should_stop()) {
        unsigned processed = 0;
        while (processed++ < 64U) {
            spin_lock(&loopback.lock);
            if (!loopback.count) {
                wait_queue_prepare(&loopback.wait);
                spin_unlock(&loopback.lock);
                wait_queue_sleep();
                break;
            }
            loopback_queue_entry_t entry = loopback.queue[loopback.head];
            memset(&loopback.queue[loopback.head], 0, sizeof(entry));
            loopback.head = (uint16_t)((loopback.head + 1U) % CONFIG_LOOPBACK_QUEUE_MAX);
            loopback.count--;
            loopback.bytes -= entry.packet->length;
            spin_unlock(&loopback.lock);
            loopback_context_t *context = netdev_private(entry.device);
            spin_lock(&context->lock);
            bool valid = context->enabled && context->generation == entry.generation;
            spin_unlock(&context->lock);
            if (valid) (void)netdev_rx(entry.device, entry.packet);
            else net_pbuf_free(entry.packet);
            net_namespace_t *ns = entry.device->net_ns;
            netdev_put(entry.device);
            net_ns_put(ns);
        }
        sched_yield();
    }
    return EOK;
}

int loopback_namespace_init(net_namespace_t *ns)
{
    if (!ns) return -EINVAL;
    if (ns->loopback_dev) return EOK;
    loopback_context_t *context = calloc(1, sizeof(*context));
    if (!context) return -ENOMEM;
    net_device_t *device = &context->device;
    int status = netdev_init(device, "lo", &loopback_ops, context);
    if (status) { free(context); return status; }
    device->net_ns = ns;
    device->release = loopback_release;
    const uint8_t mac[ETH_ADDRESS_LEN] = {0x02, 0, 0, 0, 0, 1};
    memcpy(device->address, mac, ETH_ADDRESS_LEN);
    device->mtu = CONFIG_NETDEV_MTU_MAX;
    /* Linux creates isolated lo down; initial namespace preserves boot behavior. */
    bool initial = ns == &init_net_ns;
    device->flags = NETDEV_F_LOOPBACK | (initial ? NETDEV_F_UP | NETDEV_F_RUNNING : 0);
    device->ipv4_address = 0x7f000001U;
    device->ipv4_netmask = 0xff000000U;
    device->ipv6_address[15] = device->ipv6_link_local[15] = 1;
    device->ipv6_prefix_length = 128;
    device->ipv6_valid_until = UINT64_MAX;
    device->ipv6_preferred_until = UINT64_MAX;
    device->ipv6_mtu = device->mtu;
    context->enabled = initial;
    status = netdev_register(device);
    if (status) { free(context); return status; }
    ns->loopback_dev = device;
    return EOK;
}

void loopback_namespace_destroy(net_namespace_t *ns)
{
    if (!ns || !ns->loopback_dev) return;
    net_device_t *device = ns->loopback_dev;
    ns->loopback_dev = NULL;
    (void)netdev_unregister(device);
}

void loopback_init(void)
{
    if (loopback.ready) return;
    wait_queue_init(&loopback.wait);
    int status = kernel_worker_register("net-loopback", loopback_worker, NULL, &loopback.worker);
    if (status) { plogk("loopback: Worker registration failed (%d)\n", status); return; }
    __atomic_store_n(&loopback.ready, true, __ATOMIC_RELEASE);
    status = loopback_namespace_init(&init_net_ns);
    if (status) plogk("loopback: Initial device failed (%d)\n", status);
    else plogk("loopback: Interface 'lo' registered (127.0.0.1/8)\n");
}
#endif
