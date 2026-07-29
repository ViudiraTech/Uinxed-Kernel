/*
 *
 *      ohci.c
 *      Open Host Controller Interface (OHCI) driver
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <chipset/common.h>
#include <drivers/bus/pci.h>
#include <drivers/interrupt/apic.h>
#include <drivers/usb/host/host.h>
#include <drivers/usb/host/ohci.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <kernel/timer.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <proc/sched.h>
#include <proc/task.h>

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

#define OHCI_MAX_CONTROLLERS 8
#define OHCI_MAX_PORTS       15
#define OHCI_NUM_ED          32
#define OHCI_NUM_TD          128

typedef struct ohci_td_phys {
        ohci_gtd_t *virtual;
        uint64_t physical;
        bool     used;
} ohci_td_phys_t;

typedef struct ohci_ed_phys {
        ohci_ed_t *virtual;
        uint64_t physical;
        bool     used;
} ohci_ed_phys_t;

typedef struct ohci_controller {
        usb_host_t          hcd;
        volatile uint8_t   *mmio_base;
        pci_device_cache_t *pci;
        int                 vector;
        uint8_t             bus_number;
        uint8_t             irq_slot;
        uint8_t             num_ports;

        ohci_hcca_t *hcca;
        uint64_t     hcca_physical;
        spinlock_t   lock;

        ohci_td_phys_t tds[OHCI_NUM_TD];
        ohci_ed_phys_t eds[OHCI_NUM_ED];
        uint64_t       pending_ports;
        task_t        *worker_task;
        bool           running;
} ohci_controller_t;

static ohci_controller_t *ohci_controllers[OHCI_MAX_CONTROLLERS];
static size_t             ohci_controller_count;

static inline uint32_t ohci_read32(ohci_controller_t *ctrl, uint8_t reg)
{
    return mmio_read32((void *)(ctrl->mmio_base + reg));
}

static inline void ohci_write32(ohci_controller_t *ctrl, uint8_t reg, uint32_t value)
{
    mmio_write32((uint32_t *)(ctrl->mmio_base + reg), value);
}

static void *ohci_dma_alloc(size_t size, uint64_t *physical)
{
    size_t   count   = (size + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    uint64_t address = alloc_frames(count);
    if (!address) return NULL;
    void *memory = phys_to_virt(address);
    memset(memory, 0, count * PAGE_4K_SIZE);
    *physical = address;
    return memory;
}

static void ohci_dma_free(uint64_t physical, size_t size)
{
    size_t count = (size + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    if (physical && count) free_frames(physical, count);
}

static int ohci_find_free_ed(ohci_controller_t *ctrl)
{
    for (int i = 0; i < OHCI_NUM_ED; i++) {
        if (!ctrl->eds[i].used) {
            ctrl->eds[i].used = true;
            return i;
        }
    }
    return -1;
}

static void ohci_free_ed(ohci_controller_t *ctrl, int index)
{
    if (index < 0 || index >= OHCI_NUM_ED) return;
    ctrl->eds[index].used = false;
    memset(ctrl->eds[index].virtual, 0, sizeof(ohci_ed_t));
}

static int ohci_find_free_td(ohci_controller_t *ctrl)
{
    for (int i = 0; i < OHCI_NUM_TD; i++) {
        if (!ctrl->tds[i].used) {
            ctrl->tds[i].used = true;
            return i;
        }
    }
    return -1;
}

static void ohci_free_td(ohci_controller_t *ctrl, int index)
{
    if (index < 0 || index >= OHCI_NUM_TD) return;
    ctrl->tds[index].used = false;
    memset(ctrl->tds[index].virtual, 0, sizeof(ohci_gtd_t));
}

static int ohci_control(usb_device_t *device, const usb_setup_packet_t *setup, void *buffer, size_t length, uint32_t timeout_ms)
{
    (void)device;
    (void)setup;
    (void)buffer;
    (void)length;
    (void)timeout_ms;
    return -ENOSYS;
}

static int ohci_transfer(usb_endpoint_t *endpoint, void *buffer, size_t length, size_t *actual, uint32_t timeout_ms)
{
    (void)endpoint;
    (void)buffer;
    (void)length;
    (void)actual;
    (void)timeout_ms;
    return -ENOSYS;
}

static int ohci_interrupt_start(usb_endpoint_t *endpoint, size_t length, usb_interrupt_complete_t complete, void *context)
{
    (void)endpoint;
    (void)length;
    (void)complete;
    (void)context;
    return -ENOSYS;
}

static void ohci_interrupt_stop(usb_endpoint_t *endpoint)
{
    (void)endpoint;
}

static int ohci_configure_endpoint(usb_endpoint_t *endpoint)
{
    (void)endpoint;
    return EOK;
}

static int ohci_clear_halt(usb_endpoint_t *endpoint)
{
    (void)endpoint;
    return EOK;
}

static void ohci_disable_device(usb_device_t *device)
{
    (void)device;
}

static const usb_hcd_ops_t ohci_hcd_ops = {
    .control            = ohci_control,
    .transfer           = ohci_transfer,
    .interrupt_start    = ohci_interrupt_start,
    .interrupt_stop     = ohci_interrupt_stop,
    .configure_endpoint = ohci_configure_endpoint,
    .clear_halt         = ohci_clear_halt,
    .disable_device     = ohci_disable_device,
};

static int ohci_port_reset(ohci_controller_t *ctrl, uint8_t port)
{
    if (!ctrl || port >= ctrl->num_ports) return -EINVAL;
    uint32_t portsc = ohci_read32(ctrl, OHCI_HcRhPortStatus + port * 4);
    if (!(portsc & OHCI_PORT_CCS)) return -ENODEV;
    ohci_write32(ctrl, OHCI_HcRhPortStatus + port * 4, OHCI_PORT_PRS);
    uint64_t deadline = nano_time() + 100 * 1000000ULL;
    while (ohci_read32(ctrl, OHCI_HcRhPortStatus + port * 4) & OHCI_PORT_PRS) {
        if (nano_time() >= deadline) {
            ohci_write32(ctrl, OHCI_HcRhPortStatus + port * 4, OHCI_PORT_CHANGE_BITS);
            return -ETIMEDOUT;
        }
        __asm__ volatile("pause");
    }
    portsc = ohci_read32(ctrl, OHCI_HcRhPortStatus + port * 4);
    ohci_write32(ctrl, OHCI_HcRhPortStatus + port * 4, portsc & OHCI_PORT_CHANGE_BITS);
    return (portsc & OHCI_PORT_CCS) ? EOK : -ENODEV;
}

static usb_speed_t ohci_port_speed(uint32_t portsc)
{
    return (portsc & OHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static void ohci_usb_device_release(struct device *dev)
{
    (void)dev;
}

static int ohci_get_string(usb_device_t *device, uint8_t index, uint16_t language, char *output, size_t capacity)
{
    uint8_t descriptor[128];
    if (!index || !output || capacity < 2) return -EINVAL;
    int result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, (USB_DT_STRING << 8) | index,
                                 language, descriptor, sizeof(descriptor), USB_CTRL_TIMEOUT_MS);
    if (result != EOK || descriptor[0] < 2 || descriptor[1] != USB_DT_STRING) return -EIO;
    size_t characters = (descriptor[0] - 2) / 2;
    if (characters >= capacity) characters = capacity - 1;
    for (size_t i = 0; i < characters; i++) {
        uint16_t character = descriptor[2 + i * 2] | (uint16_t)descriptor[3 + i * 2] << 8;
        output[i]          = character >= 0x20 && character < 0x7f ? (char)character : '?';
    }
    output[characters] = '\0';
    return EOK;
}

static int ohci_enumerate_port(ohci_controller_t *ctrl, uint8_t port)
{
    int result = ohci_port_reset(ctrl, port);
    if (result != EOK) return result;

    uint32_t    portsc = ohci_read32(ctrl, OHCI_HcRhPortStatus + port * 4);
    usb_speed_t speed  = ohci_port_speed(portsc);

    usb_device_t *device = calloc(1, sizeof(*device));
    if (!device) return -ENOMEM;
    device->connected   = true;
    device->speed       = speed;
    device->bus_number  = ctrl->bus_number;
    device->port_number = port;
    device->hcd_ops     = &ohci_hcd_ops;
    device->hc_private  = ctrl;
    device->address     = port + 1;
    snprintf(device->path, sizeof(device->path), "%u-%u", device->bus_number, port);

    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_ADDRESS, device->address, 0, NULL, 0,
                             USB_CTRL_TIMEOUT_MS);
    if (result != EOK) goto fail;

    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0,
                             &device->descriptor, sizeof(device->descriptor), USB_CTRL_TIMEOUT_MS);
    if (result != EOK || device->descriptor.length < sizeof(device->descriptor)) goto fail;

    uint16_t language = 0x0409;
    uint8_t  lang_desc[4];
    if (usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_STRING << 8, 0, lang_desc,
                        sizeof(lang_desc), USB_CTRL_TIMEOUT_MS)
            == EOK
        && lang_desc[0] >= 4)
        language = lang_desc[2] | (uint16_t)lang_desc[3] << 8;
    ohci_get_string(device, device->descriptor.manufacturer, language, device->manufacturer, sizeof(device->manufacturer));
    ohci_get_string(device, device->descriptor.product, language, device->product, sizeof(device->product));
    ohci_get_string(device, device->descriptor.serial_number, language, device->serial, sizeof(device->serial));

    usb_config_descriptor_t header;
    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, &header,
                             sizeof(header), USB_CTRL_TIMEOUT_MS);
    if (result != EOK) goto fail;
    uint16_t total_length = usb_get_le16(&header.total_length);
    if (total_length < sizeof(header) || total_length > PAGE_4K_SIZE) goto fail;

    uint8_t *configuration = malloc(total_length);
    if (!configuration) {
        result = -ENOMEM;
        goto fail;
    }
    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0,
                             configuration, total_length, USB_CTRL_TIMEOUT_MS);
    device->dev.release = ohci_usb_device_release;
    if (result == EOK) result = usb_add_device(device, configuration, total_length);
    free(configuration);
    if (result == EOK) return EOK;

fail:
    usb_remove_device(device);
    free(device);
    return result;
}

static void ohci_worker(void *argument)
{
    ohci_controller_t *ctrl = argument;
    while (ctrl->running) {
        uint32_t sts = ohci_read32(ctrl, OHCI_HcInterruptStatus);
        if (!(sts & OHCI_INTR_RHSC)) {
            __asm__ volatile("pause");
            continue;
        }
        ohci_write32(ctrl, OHCI_HcInterruptStatus, OHCI_INTR_RHSC);
        for (uint8_t port = 0; port < ctrl->num_ports; port++) {
            uint32_t portsc = ohci_read32(ctrl, OHCI_HcRhPortStatus + port * 4);
            uint32_t change = portsc & OHCI_PORT_CHANGE_BITS;
            if (!change) continue;
            ohci_write32(ctrl, OHCI_HcRhPortStatus + port * 4, change);
            if ((portsc & OHCI_PORT_CCS) && (portsc & OHCI_PORT_CSC)) {
                msleep(100);
                int ret = ohci_enumerate_port(ctrl, port);
                if (ret != EOK) plogk("ohci: Port %u enumeration failed: %d\n", port, ret);
            }
        }
    }
}

static void ohci_interrupt_handler(void *frame)
{
    (void)frame;
    for (size_t i = 0; i < ohci_controller_count; i++) {
        ohci_controller_t *ctrl = ohci_controllers[i];
        if (!ctrl || !ctrl->running) continue;
        uint32_t sts = ohci_read32(ctrl, OHCI_HcInterruptStatus);
        if (!sts) continue;
        if (sts & OHCI_INTR_RHSC) {
            ohci_write32(ctrl, OHCI_HcInterruptStatus, OHCI_INTR_RHSC);
            ctrl->pending_ports++;
        }
        if (sts & OHCI_INTR_WDH) { ohci_write32(ctrl, OHCI_HcInterruptStatus, OHCI_INTR_WDH); }
        if (sts & OHCI_INTR_UE) {
            ohci_write32(ctrl, OHCI_HcInterruptStatus, OHCI_INTR_UE);
            plogk("ohci: Controller error on bus %u\n", ctrl->bus_number);
        }
        if (sts & OHCI_INTR_SO) { ohci_write32(ctrl, OHCI_HcInterruptStatus, OHCI_INTR_SO); }
    }
    send_eoi();
}

static int ohci_host_start(usb_host_t *host)
{
    (void)host;
    return EOK;
}

static void ohci_host_stop(usb_host_t *host)
{
    ohci_controller_t *ctrl = container_of(host, ohci_controller_t, hcd);
    ctrl->running           = false;
    uint32_t control        = ohci_read32(ctrl, OHCI_HcControl);
    control &= ~OHCI_CTRL_HCFS_MASK;
    control |= OHCI_CTRL_HCFS_RESET;
    ohci_write32(ctrl, OHCI_HcControl, control);
    ohci_write32(ctrl, OHCI_HcInterruptDisable, OHCI_INTR_MIE);
}

static int ohci_port_reset_hcd(usb_host_t *host, uint8_t port)
{
    ohci_controller_t *ctrl = container_of(host, ohci_controller_t, hcd);
    return ohci_port_reset(ctrl, port);
}

static int ohci_port_speed_hcd(usb_host_t *host, uint8_t port)
{
    ohci_controller_t *ctrl = container_of(host, ohci_controller_t, hcd);
    if (port >= ctrl->num_ports) return USB_SPEED_FULL;
    return ohci_port_speed(ohci_read32(ctrl, OHCI_HcRhPortStatus + port * 4));
}

static int ohci_port_connected_hcd(usb_host_t *host, uint8_t port)
{
    ohci_controller_t *ctrl = container_of(host, ohci_controller_t, hcd);
    if (port >= ctrl->num_ports) return 0;
    return !!(ohci_read32(ctrl, OHCI_HcRhPortStatus + port * 4) & OHCI_PORT_CCS);
}

static usb_host_controller_ops_t ohci_controller_ops = {
    .host_start     = ohci_host_start,
    .host_stop      = ohci_host_stop,
    .port_reset     = ohci_port_reset_hcd,
    .port_speed     = ohci_port_speed_hcd,
    .port_connected = ohci_port_connected_hcd,
};

static int ohci_probe(pci_device_cache_t *pci, uint8_t bus_number)
{
    base_address_register_t bar = get_base_address_register(pci, 0);
    if (bar.type != mem_mapping || !bar.address) return -ENODEV;

    uint64_t bar_physical = (uint64_t)virt_to_phys((uint64_t)bar.address);
    uint64_t bar_size     = bar.size & ~BAR_64BIT_FLAG;
    if (!bar_size) bar_size = PAGE_4K_SIZE;
    uint64_t map_start  = ALIGN_DOWN(bar_physical, PAGE_4K_SIZE);
    uint64_t map_length = ALIGN_UP(bar_physical + bar_size, PAGE_4K_SIZE) - map_start;
    page_map_range_to(get_kernel_pagedir(), map_start, map_length, PTE_MMIO_FLAGS);

    ohci_controller_t *ctrl = calloc(1, sizeof(*ctrl));
    if (!ctrl) return -ENOMEM;

    ctrl->mmio_base          = bar.address;
    ctrl->pci                = pci;
    ctrl->bus_number         = bus_number;
    ctrl->hcd.type           = USB_HOST_OHCI;
    ctrl->hcd.bus_number     = bus_number;
    ctrl->hcd.pci_dev        = pci;
    ctrl->hcd.hcd_ops        = &ohci_hcd_ops;
    ctrl->hcd.controller_ops = &ohci_controller_ops;
    ctrl->hcd.hc_private     = ctrl;
    snprintf(ctrl->hcd.name, sizeof(ctrl->hcd.name), "ohci-usb%u", bus_number);

    uint32_t command = pci_read_command_status(pci) & 0xffff;
    pci_write_command_status(pci, command | 0x06);

    uint32_t control = ohci_read32(ctrl, OHCI_HcControl);
    control &= ~OHCI_CTRL_HCFS_MASK;
    ohci_write32(ctrl, OHCI_HcControl, control | OHCI_CTRL_HCFS_RESET);
    uint64_t deadline = nano_time() + 100 * 1000000ULL;
    while (ohci_read32(ctrl, OHCI_HcControl) & OHCI_CTRL_IR) {
        if (nano_time() >= deadline) {
            free(ctrl);
            return -ETIMEDOUT;
        }
        __asm__ volatile("pause");
    }

    uint32_t rh_descriptor_a = ohci_read32(ctrl, OHCI_HcRhDescriptorA);
    ctrl->num_ports          = rh_descriptor_a & 0x0f;
    if (ctrl->num_ports > OHCI_MAX_PORTS) ctrl->num_ports = OHCI_MAX_PORTS;
    ctrl->hcd.max_ports = ctrl->num_ports;

    ctrl->hcca = ohci_dma_alloc(256, &ctrl->hcca_physical);
    if (!ctrl->hcca) {
        free(ctrl);
        return -ENOMEM;
    }

    for (int i = 0; i < OHCI_NUM_TD; i++) {
        uint64_t phys;
        ctrl->tds[i].virtual = ohci_dma_alloc(sizeof(ohci_gtd_t), &phys);
        if (!ctrl->tds[i].virtual) {
            for (int j = 0; j < i; j++) ohci_dma_free(ctrl->tds[j].physical, sizeof(ohci_gtd_t));
            ohci_dma_free(ctrl->hcca_physical, 256);
            free(ctrl);
            return -ENOMEM;
        }
        ctrl->tds[i].physical = phys;
    }
    for (int i = 0; i < OHCI_NUM_ED; i++) {
        uint64_t phys;
        ctrl->eds[i].virtual = ohci_dma_alloc(sizeof(ohci_ed_t), &phys);
        if (!ctrl->eds[i].virtual) {
            for (int j = 0; j < i; j++) ohci_dma_free(ctrl->eds[j].physical, sizeof(ohci_ed_t));
            for (int j = 0; j < OHCI_NUM_TD; j++) ohci_dma_free(ctrl->tds[j].physical, sizeof(ohci_gtd_t));
            ohci_dma_free(ctrl->hcca_physical, 256);
            free(ctrl);
            return -ENOMEM;
        }
        ctrl->eds[i].physical = phys;
    }

    ohci_write32(ctrl, OHCI_HcHCCA, (uint32_t)ctrl->hcca_physical);
    ohci_write32(ctrl, OHCI_HcControlHeadED, 0);
    ohci_write32(ctrl, OHCI_HcBulkHeadED, 0);

    uint32_t fm_interval    = 0x2edf;
    uint32_t periodic_start = (fm_interval * 9) / 10;
    ohci_write32(ctrl, OHCI_HcFmInterval, fm_interval);
    ohci_write32(ctrl, OHCI_HcPeriodicStart, periodic_start);

    control = ohci_read32(ctrl, OHCI_HcControl);
    control &= ~OHCI_CTRL_HCFS_MASK;
    control |= OHCI_CTRL_HCFS_OPER | OHCI_CTRL_PLE | OHCI_CTRL_CLE | OHCI_CTRL_BLE | OHCI_CTRL_IE;
    ohci_write32(ctrl, OHCI_HcControl, control);

    ohci_write32(ctrl, OHCI_HcInterruptEnable, OHCI_INTR_MIE | OHCI_INTR_RHSC | OHCI_INTR_WDH | OHCI_INTR_SO | OHCI_INTR_UE);

    uint32_t irq = pci_get_irq(pci);
    ctrl->vector = (int)irq;
    pci_msi_init(pci);
    int msi_vector = pci_enable_msi(pci);
    if (msi_vector >= 0) ctrl->vector = msi_vector;
    if (ctrl->vector > 0) { register_interrupt_handler((uint16_t)ctrl->vector, ohci_interrupt_handler, 0, 0x8e); }

    ctrl->running     = true;
    ctrl->worker_task = kthread_create("ohci-hub", ohci_worker, ctrl);
    if (ctrl->worker_task) task_wakeup(ctrl->worker_task);
    ohci_controllers[ohci_controller_count++] = ctrl;
    usb_host_register(&ctrl->hcd);

    for (uint8_t port = 0; port < ctrl->num_ports; port++) {
        uint32_t portsc = ohci_read32(ctrl, OHCI_HcRhPortStatus + port * 4);
        ohci_write32(ctrl, OHCI_HcRhPortStatus + port * 4, portsc & OHCI_PORT_CHANGE_BITS);
        if (portsc & OHCI_PORT_CCS) {
            int ret = ohci_enumerate_port(ctrl, port);
            if (ret != EOK) plogk("ohci: Port %u enumeration failed: %d\n", port, ret);
        }
    }

    plogk("ohci: Controller at MMIO %p, bus usb%u, %u ports\n", (void *)bar.address, bus_number, ctrl->num_ports);
    return EOK;
}

void ohci_start_workers(void)
{
#if !CONFIG_USB_OHCI
    return;
#endif
}

void ohci_init(void)
{
#if !CONFIG_USB_OHCI
    return;
#endif
    if (usb_core_init() != EOK) return;
    pci_devices_cache_t *cache = pci_get_devices_cache();
    if (!cache) return;
    uint8_t bus_number = 1;
    for (pci_device_cache_t *pci = cache->head; pci && ohci_controller_count < OHCI_MAX_CONTROLLERS; pci = pci->next) {
        if (pci->class_code != OHCI_PCI_CLASS) continue;
        if (ohci_probe(pci, bus_number) == EOK) bus_number++;
    }
}
