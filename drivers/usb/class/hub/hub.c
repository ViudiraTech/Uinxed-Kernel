/*
 *
 *      hub.c
 *      USB Hub Class driver — industrial-grade hub enumeration and port management
 *
 *      USB 2.0 §11, USB 3.x §10, xHCI §4.6 / §6.2.2
 *
 *      2026/8/27 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <drivers/base/device.h>
#include <drivers/usb/class/hub/usb_hub.h>
#include <drivers/usb/core/usb.h>
#include <drivers/usb/host/host.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>
#include <process/kthread.h>
#include <process/sched.h>
#include <process/task.h>
#include <sync/spin_lock.h>

#define USB_HUB_MAX_HUBS 16

typedef struct usb_hub {
        usb_interface_t *interface;
        usb_device_t    *device;
        usb_endpoint_t  *status_ep;
        uint8_t          port_count;
        uint16_t         characteristics;
        uint8_t          power_good_ms;
        uint8_t         *status_buffer;
        size_t           status_size;
        usb_device_t    *children[USB_HUB_MAX_PORTS + 1];
        volatile uint32_t pending_ports;
        wait_queue_t     wait;
        task_t          *worker;
        spinlock_t       lock;
        bool             running;
        bool             has_individual_power;
        bool             has_overcurrent;
} usb_hub_t;

static usb_hub_t *usb_hubs[USB_HUB_MAX_HUBS];
static spinlock_t usb_hub_global_lock;

/* Hub control helpers — class requests to the hub itself. */

static int hub_get_descriptor(usb_device_t *hub, void *buf, uint16_t len)
{
    // Try USB 2.0 hub descriptor (0x29) first, then SuperSpeed hub (0x2A) per §10.13.2.
    int ret = usb_control_msg(hub, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE, USB_HUB_REQ_GET_DESCRIPTOR, USB_DT_HUB << 8, 0, buf, len, USB_CTRL_TIMEOUT_MS);
    if (ret != EOK && hub->descriptor.usb_version >= 0x0300) {
        ret = usb_control_msg(hub, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE, USB_HUB_REQ_GET_DESCRIPTOR, USB_DT_HUB3 << 8, 0, buf, len, USB_CTRL_TIMEOUT_MS);
    }
    return ret;
}

static int hub_get_status(usb_device_t *hub, uint16_t *status, uint16_t *change)
{
    uint8_t buf[4];
    int ret = usb_control_msg(hub, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE, USB_REQ_GET_STATUS, 0, 0, buf, 4, USB_CTRL_TIMEOUT_MS);
    if (ret != EOK) return ret;
    if (status) *status = (uint16_t)buf[0] | (uint16_t)buf[1] << 8;
    if (change) *change = (uint16_t)buf[2] | (uint16_t)buf[3] << 8;
    return EOK;
}

static int hub_port_status(usb_device_t *hub, uint8_t port, uint16_t *status, uint16_t *change)
{
    uint8_t buf[4];
    int ret = usb_control_msg(hub, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQ_GET_STATUS, 0, port, buf, 4, USB_CTRL_TIMEOUT_MS);
    if (ret != EOK) return ret;
    if (status) *status = (uint16_t)buf[0] | (uint16_t)buf[1] << 8;
    if (change) *change = (uint16_t)buf[2] | (uint16_t)buf[3] << 8;
    return EOK;
}

static int hub_set_port_feature(usb_device_t *hub, uint8_t port, uint16_t feature)
{
    return usb_control_msg(hub, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQ_SET_FEATURE, feature, port, NULL, 0, USB_CTRL_TIMEOUT_MS);
}

static int hub_clear_port_feature(usb_device_t *hub, uint8_t port, uint16_t feature)
{
    return usb_control_msg(hub, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQ_CLEAR_FEATURE, feature, port, NULL, 0, USB_CTRL_TIMEOUT_MS);
}

/* Wait for a port status bit to reach the desired state. */

static int hub_wait_port(usb_device_t *hub, uint8_t port, uint16_t mask, uint16_t value, uint32_t timeout_ms)
{
    uint64_t deadline = nano_time() + (uint64_t)timeout_ms * 1000000ULL;
    while (1) {
        uint16_t status, change;
        int ret = hub_port_status(hub, port, &status, &change);
        if (ret != EOK) return ret;
        if ((status & mask) == value) return EOK;
        if (nano_time() >= deadline) return -ETIMEDOUT;
        msleep(10);
    }
}

/* Power on a single hub port and wait for power-good. */

static int hub_power_on_port(usb_hub_t *hub, uint8_t port)
{
    int ret = hub_set_port_feature(hub->device, port, USB_PORT_FEAT_POWER);
    if (ret != EOK) return ret;
    // Power-good is in 2ms units; add margin and TRSTRCY.
    msleep((uint32_t)hub->power_good_ms * 2 + USB_HUB_POWER_GOOD_MARGIN_MS);
    return hub_wait_port(hub->device, port, USB_PORT_STATUS_POWER, USB_PORT_STATUS_POWER, USB_HUB_POWER_TIMEOUT_MS);
}

/* Reset a downstream port and wait for enable. Returns EOK when the port is
 * enabled (device ready for address 0 SET_ADDRESS). Clears C_RESET / C_CONNECTION. */

static int hub_reset_port(usb_hub_t *hub, uint8_t port)
{
    int ret = hub_set_port_feature(hub->device, port, USB_PORT_FEAT_RESET);
    if (ret != EOK) return ret;
    // TRSTRCY 10ms + hub recovery. Poll until RESET clears and ENABLE asserts.
    msleep(USB_HUB_RESET_RECOVERY_MS);
    uint64_t deadline = nano_time() + (uint64_t)USB_HUB_RESET_TIMEOUT_MS * 1000000ULL;
    while (1) {
        uint16_t status, change;
        ret = hub_port_status(hub->device, port, &status, &change);
        if (ret != EOK) return ret;
        bool reset_done = !(status & USB_PORT_STATUS_RESET) && (change & USB_PORT_STATUS_C_RESET);
        if (reset_done) {
            hub_clear_port_feature(hub->device, port, USB_PORT_FEAT_C_RESET);
            // Device now enabled at address 0; wait for ENABLE.
            ret = hub_wait_port(hub->device, port, USB_PORT_STATUS_ENABLE, USB_PORT_STATUS_ENABLE, 100);
            if (ret == EOK) hub_clear_port_feature(hub->device, port, USB_PORT_FEAT_C_ENABLE);
            return ret;
        }
        if (nano_time() >= deadline) return -ETIMEDOUT;
        msleep(10);
    }
}

/* Downstream device enumeration — hub port has already been reset and is enabled
 * at address 0. Allocate a device, SET_ADDRESS at 0, then full descriptor fetch. */

static int hub_enumerate_device(usb_hub_t *hub, uint8_t port)
{
    if (!hub || !hub->device || !hub->device->connected) return -ENODEV;
    if (port == 0 || port > hub->port_count) return -EINVAL;
    if (hub->children[port]) return -EEXIST;

    // Determine downstream speed from port status.
    uint16_t status, change;
    int ret = hub_port_status(hub->device, port, &status, &change);
    if (ret != EOK) return ret;
    usb_speed_t speed;
    if (status & USB_PORT_STATUS_HIGH_SPEED) speed = USB_SPEED_HIGH;
    else if (status & USB_PORT_STATUS_LOW_SPEED) speed = USB_SPEED_LOW;
    else if (status & USB_PORT_STATUS_CONNECTION) speed = USB_SPEED_FULL;
    else return -ENODEV;

    // Clear C_CONNECTION now (we consumed the connect event).
    if (change & USB_PORT_STATUS_C_CONNECTION) hub_clear_port_feature(hub->device, port, USB_PORT_FEAT_C_CONNECTION);

    // Reset the port (already done in hub_handle_port, but ensure).
    ret = hub_reset_port(hub, port);
    if (ret != EOK) return ret;

    // Hand off to HCD for address+descriptor+config.
    usb_device_t *child = NULL;
    if (hub->device->hcd_ops && hub->device->hcd_ops->enumerate) {
        ret = hub->device->hcd_ops->enumerate(hub->device, port, &child);
    } else {
        // Legacy HCD (OHCI/UHCI/EHCI) — core helper does SET_ADDRESS at 0 + descriptors.
        ret = usb_enumerate_device(hub->device, port, speed, &child);
    }
    if (ret != EOK || !child) {
        plogk("usb-hub: %s port %u enumerate failed: %d\n", hub->device->path, port, ret);
        // De-power the port to avoid babble on failure.
        if (hub->has_individual_power) hub_clear_port_feature(hub->device, port, USB_PORT_FEAT_POWER);
        return ret ? ret : -EIO;
    }

    // Record child for disconnect tracking and path debugging.
    // child->depth/path/bus already set by HCD enumerate.
    hub->children[port] = child;
    plogk("usb-hub: %s port %u -> %s (%s %s) speed %u\n", hub->device->path, port, child->path, child->manufacturer, child->product, speed);
    return EOK;
}

/* Port status-change interrupt completion — decode bitmap and wake worker. */

static void hub_status_complete(usb_endpoint_t *ep, const void *data, size_t len, int status, void *ctx)
{
    (void)ep;
    usb_hub_t *hub = ctx;
    if (!hub || !hub->running || status != EOK || !data || !len) return;

    const uint8_t *bitmap = data;
    uint32_t pending = 0;
    // Byte 0 bit 0 is hub, ports start at bit 1. Each subsequent bit is a port.
    for (size_t byte = 0; byte < len && byte < hub->status_size; byte++) {
        uint8_t b = bitmap[byte];
        for (int bit = 0; bit < 8; bit++) {
            if (!(b & (1u << bit))) continue;
            int port;
            if (byte == 0 && bit == 0) {
                // Hub status changed — re-read hub status (over-current etc.)
                // For now just re-poll all ports.
                pending |= ((1u << hub->port_count) - 1) << 1;
                continue;
            }
            // Map bitmap position to port number (1-indexed).
            port = (int)(byte * 8 + bit); // bit 1 = port 1
            if (port >= 1 && port <= hub->port_count) pending |= 1u << port;
        }
    }
    if (!pending) return;
    uint64_t flags = spin_lock_irqsave(&hub->lock);
    hub->pending_ports |= pending;
    spin_unlock_irqrestore(&hub->lock, flags);
    wait_queue_wake_one(&hub->wait);
}

/* Handle a single port whose change bit was set. */

static void hub_handle_port(usb_hub_t *hub, uint8_t port)
{
    uint16_t status, change;
    int ret = hub_port_status(hub->device, port, &status, &change);
    if (ret != EOK) return;

    // Over-current — clear and power off.
    if (change & USB_PORT_STATUS_C_OVER_CURRENT) {
        hub_clear_port_feature(hub->device, port, USB_PORT_FEAT_C_OVER_CURRENT);
        plogk("usb-hub: %s port %u over-current\n", hub->device->path, port);
    }

    // Connection change — connect or disconnect.
    if (change & USB_PORT_STATUS_C_CONNECTION) {
        if (status & USB_PORT_STATUS_CONNECTION) {
            // Debounce per §9.2.6.2 / §11.24.2.2: 100ms stable.
            msleep(USB_HUB_DEBOUNCE_MS);
            hub_port_status(hub->device, port, &status, NULL);
            if (!(status & USB_PORT_STATUS_CONNECTION)) {
                hub_clear_port_feature(hub->device, port, USB_PORT_FEAT_C_CONNECTION);
                return;
            }
            // Device attached — enumerate.
            if (!hub->children[port]) hub_enumerate_device(hub, port);
            else hub_clear_port_feature(hub->device, port, USB_PORT_FEAT_C_CONNECTION);
        } else {
            // Device removed.
            hub_clear_port_feature(hub->device, port, USB_PORT_FEAT_C_CONNECTION);
            // Port disable is implicit on disconnect; clear C_ENABLE if set.
            if (change & USB_PORT_STATUS_C_ENABLE) hub_clear_port_feature(hub->device, port, USB_PORT_FEAT_C_ENABLE);
            usb_device_t *child = hub->children[port];
            if (child) {
                hub->children[port] = NULL;
                usb_remove_device(child);
                // The child's device struct was heap-allocated by the HCD enumerate.
                // usb_remove_device tears down sysfs/devtmpfs; if not registered, free it.
                if (!child->registered) free(child);
            }
            hub_clear_port_feature(hub->device, port, USB_PORT_FEAT_C_ENABLE);
        }
        return;
    }

    // Enable change without connection — e.g., port error.
    if (change & USB_PORT_STATUS_C_ENABLE) {
        hub_clear_port_feature(hub->device, port, USB_PORT_FEAT_C_ENABLE);
        if (!(status & USB_PORT_STATUS_ENABLE) && hub->children[port]) {
            // Port disabled while device was present — treat as disconnect.
            usb_device_t *child = hub->children[port];
            hub->children[port] = NULL;
            usb_remove_device(child);
            if (!child->registered) free(child);
        }
    }

    // Reset complete is handled inside hub_reset_port / hub_enumerate_device.
    if (change & USB_PORT_STATUS_C_RESET) hub_clear_port_feature(hub->device, port, USB_PORT_FEAT_C_RESET);
}

/* Hub worker — enumerates initial ports, then sleeps on status-change interrupt. */

static int hub_worker(void *arg)
{
    usb_hub_t *hub = arg;
    // Initial enumeration: power was already applied in probe; poll all ports.
    for (uint8_t port = 1; port <= hub->port_count; port++) {
        uint16_t status;
        if (hub_port_status(hub->device, port, &status, NULL) != EOK) continue;
        if (status & USB_PORT_STATUS_CONNECTION) {
            msleep(USB_HUB_DEBOUNCE_MS);
            hub_handle_port(hub, port);
        }
    }

    while (hub->running && !kthread_should_stop()) {
        uint64_t flags = spin_lock_irqsave(&hub->lock);
        uint32_t pending = hub->pending_ports;
        hub->pending_ports = 0;
        if (!pending && hub->running && !kthread_should_stop()) wait_queue_prepare(&hub->wait);
        spin_unlock_irqrestore(&hub->lock, flags);

        if (!pending && hub->running && !kthread_should_stop()) {
            // Also periodic poll every 500ms for lost interrupts.
            wait_queue_wait_timed(&hub->wait, sched_ticks() + timer_ns_to_ticks_ceil(500 * 1000000ULL));
            continue;
        }

        for (uint8_t port = 1; port <= hub->port_count; port++) {
            if (pending & (1u << port)) hub_handle_port(hub, port);
        }
    }
    return 0;
}

/* Parse the hub descriptor to obtain port count and characteristics. */

static int hub_parse_descriptor(usb_hub_t *hub, const uint8_t *buf, size_t len)
{
    if (!buf || len < 7) return -EINVAL;
    // Hub descriptor may be USB 2.0 (0x29, 7+ bytes) or SuperSpeed (0x2A, 12+ bytes).
    uint8_t desc_type = buf[1];
    if (desc_type != USB_DT_HUB && desc_type != USB_DT_HUB3) return -EINVAL;
    usb_hub_descriptor_t *desc = (usb_hub_descriptor_t *)buf;
    if (desc->length < 7 || desc->length > len) return -EINVAL;
    hub->port_count = desc->port_count;
    if (hub->port_count == 0 || hub->port_count > USB_HUB_MAX_PORTS) return -EINVAL;
    hub->characteristics = desc->characteristics;
    hub->power_good_ms   = desc->power_good_time;
    hub->has_individual_power = (hub->characteristics & 0x0001) != 0;
    hub->has_overcurrent      = (hub->characteristics & 0x0008) != 0;
    return EOK;
}

/* Probe a hub interface — power ports, start status polling, spawn worker. */

int usb_hub_probe(usb_interface_t *interface)
{
#if CONFIG_USB_HUB
    if (!interface || interface->driver_data || interface->descriptor.interface_class != USB_CLASS_HUB) return -EINVAL;
    usb_device_t *device = interface->device;
    if (!device) return -EINVAL;

    // Hubs have exactly one interrupt IN status endpoint.
    usb_endpoint_t *status_ep = usb_find_endpoint(interface, USB_ENDPOINT_XFER_INT, true);
    if (!status_ep) {
        plogk("usb-hub: %s: no status endpoint\n", device->path);
        return -ENODEV;
    }

    // Hubs are high/full speed per spec; SuperSpeed hubs appear as USB 3 hub (0x2A) but QEMU uses USB 2.
    uint8_t hub_buf[16];
    int ret = hub_get_descriptor(device, hub_buf, sizeof(hub_buf));
    if (ret != EOK) {
        plogk("usb-hub: %s: GET_DESCRIPTOR HUB failed: %d\n", device->path, ret);
        return ret;
    }

    usb_hub_t *hub = calloc(1, sizeof(*hub));
    if (!hub) return -ENOMEM;
    hub->interface = interface;
    hub->device    = device;
    hub->status_ep = status_ep;

    ret = hub_parse_descriptor(hub, hub_buf, sizeof(hub_buf));
    if (ret != EOK) {
        plogk("usb-hub: %s: hub descriptor parse failed: %d\n", device->path, ret);
        free(hub);
        return ret;
    }

    // Power on all ports (ganged vs individual per wHubCharacteristics).
    plogk("usb-hub: %s: %u ports, %s power, %s OC, power-good %ums\n", device->path, hub->port_count,
          hub->has_individual_power ? "individual" : "ganged", hub->has_overcurrent ? "global" : "none", (unsigned)hub->power_good_ms * 2);
    if (!hub->has_individual_power) {
        // Ganged: one PORT_POWER on port 1 powers all, but QEMU requires per-port.
        for (uint8_t port = 1; port <= hub->port_count; port++) hub_set_port_feature(device, port, USB_PORT_FEAT_POWER);
        msleep((uint32_t)hub->power_good_ms * 2 + USB_HUB_POWER_GOOD_MARGIN_MS);
    } else {
        for (uint8_t port = 1; port <= hub->port_count; port++) hub_power_on_port(hub, port);
    }

    // Status-change interrupt: 1 hub bit + port bits, rounded up.
    hub->status_size = (hub->port_count + 1 + 7) / 8;
    if (hub->status_size < 1) hub->status_size = 1;
    if (hub->status_size > USB_HUB_MAX_STATUS_BYTES) hub->status_size = USB_HUB_MAX_STATUS_BYTES;
    hub->status_buffer = malloc(hub->status_size);
    if (!hub->status_buffer) {
        free(hub);
        return -ENOMEM;
    }

    wait_queue_init(&hub->wait);
    hub->running = true;

    // Start status-change polling before worker, so initial changes are queued.
    ret = usb_interrupt_start(status_ep, hub->status_size, hub_status_complete, hub);
    if (ret != EOK) {
        plogk("usb-hub: %s: status interrupt start failed: %d\n", device->path, ret);
        free(hub->status_buffer);
        free(hub);
        return ret;
    }

    // Register worker — hub enumeration happens there (no blocking in probe).
    uint64_t flags = spin_lock_irqsave(&usb_hub_global_lock);
    size_t slot = USB_HUB_MAX_HUBS;
    for (size_t i = 0; i < USB_HUB_MAX_HUBS; i++) if (!usb_hubs[i]) { slot = i; break; }
    if (slot == USB_HUB_MAX_HUBS) {
        spin_unlock_irqrestore(&usb_hub_global_lock, flags);
        usb_interrupt_stop(status_ep);
        free(hub->status_buffer);
        free(hub);
        return -ENOSPC;
    }
    usb_hubs[slot] = hub;
    spin_unlock_irqrestore(&usb_hub_global_lock, flags);

    // Use a short name per hub for the worker.
    char worker_name[32];
    snprintf(worker_name, sizeof(worker_name), "usb-hub/%s", device->path);
    // kernel_worker_register copies the name; allocate a task slot in the hub.
    // Late probe (after kernel_workers_start) creates immediately.
    ret = kernel_worker_register(worker_name, hub_worker, hub, &hub->worker);
    if (ret != EOK) {
        plogk("usb-hub: %s: worker register failed: %d\n", device->path, ret);
        uint64_t f2 = spin_lock_irqsave(&usb_hub_global_lock);
        usb_hubs[slot] = NULL;
        spin_unlock_irqrestore(&usb_hub_global_lock, f2);
        usb_interrupt_stop(status_ep);
        hub->running = false;
        free(hub->status_buffer);
        free(hub);
        return ret;
    }

    interface->driver_data = hub;
    plogk("usb-hub: %s: hub ready (%u ports)\n", device->path, hub->port_count);
    return EOK;
#else
    (void)interface;
    return -ENOSYS;
#endif
}

void usb_hub_disconnect(usb_interface_t *interface)
{
#if CONFIG_USB_HUB
    usb_hub_t *hub = interface ? interface->driver_data : NULL;
    if (!hub) return;
    hub->running = false;
    wait_queue_wake_all(&hub->wait);
    if (hub->status_ep) usb_interrupt_stop(hub->status_ep);
    if (hub->worker) kthread_stop(hub->worker);
    // Disconnect all downstream devices.
    for (uint8_t port = 1; port <= hub->port_count; port++) {
        usb_device_t *child = hub->children[port];
        if (!child) continue;
        hub->children[port] = NULL;
        usb_remove_device(child);
        if (!child->registered) free(child);
    }
    uint64_t flags = spin_lock_irqsave(&usb_hub_global_lock);
    for (size_t i = 0; i < USB_HUB_MAX_HUBS; i++) if (usb_hubs[i] == hub) usb_hubs[i] = NULL;
    spin_unlock_irqrestore(&usb_hub_global_lock, flags);
    interface->driver_data = NULL;
    free(hub->status_buffer);
    free(hub);
#else
    (void)interface;
#endif
}
