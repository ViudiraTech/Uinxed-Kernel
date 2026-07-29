/*
 *
 *      ehci.c
 *      Enhanced Host Controller Interface (EHCI) driver
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <chipset/common.h>
#include <drivers/bus/pci.h>
#include <drivers/interrupt/apic.h>
#include <drivers/usb/host/ehci.h>
#include <drivers/usb/host/host.h>
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

#define EHCI_MAX_CONTROLLERS 8
#define EHCI_MAX_PORTS       15
#define EHCI_NUM_QH          32
#define EHCI_NUM_QTD         64

typedef struct ehci_qtd_phys {
        ehci_qtd_t *virtual;
        uint64_t physical;
        bool     used;
} ehci_qtd_phys_t;

typedef struct ehci_qh_phys {
        ehci_qh_t *virtual;
        uint64_t physical;
        bool     used;
} ehci_qh_phys_t;

typedef struct ehci_controller {
        usb_host_t          hcd;
        volatile uint8_t   *capability;
        volatile uint8_t   *operational;
        pci_device_cache_t *pci;
        int                 vector;
        uint8_t             bus_number;
        uint8_t             irq_slot;
        uint8_t             num_ports;
        uint8_t             caplength;

        uint32_t *periodic_list;
        uint64_t  periodic_list_physical;

        ehci_qh_phys_t  qhs[EHCI_NUM_QH];
        ehci_qtd_phys_t qtds[EHCI_NUM_QTD];
        uint64_t        pending_ports;
        spinlock_t      lock;
        task_t         *worker_task;
        bool            running;
} ehci_controller_t;

static ehci_controller_t *ehci_controllers[EHCI_MAX_CONTROLLERS];
static size_t             ehci_controller_count;

static inline uint32_t ehci_read32(const volatile uint8_t *base, size_t offset)
{
    return *(volatile const uint32_t *)(base + offset);
}

static inline void ehci_write32(volatile uint8_t *base, size_t offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static void *ehci_dma_alloc(size_t size, uint64_t *physical)
{
    size_t   count   = (size + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    uint64_t address = alloc_frames(count);
    if (!address) return NULL;
    void *memory = phys_to_virt(address);
    memset(memory, 0, count * PAGE_4K_SIZE);
    *physical = address;
    return memory;
}

static void ehci_dma_free(uint64_t physical, size_t size)
{
    size_t count = (size + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    if (physical && count) free_frames(physical, count);
}

static int ehci_wait_register(volatile uint8_t *base, size_t offset, uint32_t mask, uint32_t value, uint32_t timeout_ms)
{
    uint64_t deadline = nano_time() + (uint64_t)timeout_ms * 1000000ULL;
    while ((ehci_read32(base, offset) & mask) != value) {
        if (nano_time() >= deadline) return -ETIMEDOUT;
        __asm__ volatile("pause");
    }
    return EOK;
}

static int ehci_find_free_qh(ehci_controller_t *ctrl)
{
    for (int i = 0; i < EHCI_NUM_QH; i++) {
        if (!ctrl->qhs[i].used) {
            ctrl->qhs[i].used = true;
            return i;
        }
    }
    return -1;
}

static void ehci_free_qh(ehci_controller_t *ctrl, int index)
{
    if (index < 0 || index >= EHCI_NUM_QH) return;
    ctrl->qhs[index].used = false;
    memset(ctrl->qhs[index].virtual, 0, sizeof(ehci_qh_t));
}

static int ehci_find_free_qtd(ehci_controller_t *ctrl)
{
    for (int i = 0; i < EHCI_NUM_QTD; i++) {
        if (!ctrl->qtds[i].used) {
            ctrl->qtds[i].used = true;
            return i;
        }
    }
    return -1;
}

static void ehci_free_qtd(ehci_controller_t *ctrl, int index)
{
    if (index < 0 || index >= EHCI_NUM_QTD) return;
    ctrl->qtds[index].used = false;
    memset(ctrl->qtds[index].virtual, 0, sizeof(ehci_qtd_t));
}

static int ehci_control(usb_device_t *device, const usb_setup_packet_t *setup, void *buffer, size_t length, uint32_t timeout_ms)
{
    (void)device;
    (void)setup;
    (void)buffer;
    (void)length;
    (void)timeout_ms;
    return -ENOSYS;
}

static int ehci_transfer(usb_endpoint_t *endpoint, void *buffer, size_t length, size_t *actual, uint32_t timeout_ms)
{
    (void)endpoint;
    (void)buffer;
    (void)length;
    (void)actual;
    (void)timeout_ms;
    return -ENOSYS;
}

static int ehci_interrupt_start(usb_endpoint_t *endpoint, size_t length, usb_interrupt_complete_t complete, void *context)
{
    (void)endpoint;
    (void)length;
    (void)complete;
    (void)context;
    return -ENOSYS;
}

static void ehci_interrupt_stop(usb_endpoint_t *endpoint)
{
    (void)endpoint;
}

static int ehci_configure_endpoint(usb_endpoint_t *endpoint)
{
    (void)endpoint;
    return EOK;
}

static int ehci_clear_halt(usb_endpoint_t *endpoint)
{
    (void)endpoint;
    return EOK;
}

static void ehci_disable_device(usb_device_t *device)
{
    (void)device;
}

static const usb_hcd_ops_t ehci_hcd_ops = {
    .control            = ehci_control,
    .transfer           = ehci_transfer,
    .interrupt_start    = ehci_interrupt_start,
    .interrupt_stop     = ehci_interrupt_stop,
    .configure_endpoint = ehci_configure_endpoint,
    .clear_halt         = ehci_clear_halt,
    .disable_device     = ehci_disable_device,
};

static int ehci_port_reset(ehci_controller_t *ctrl, uint8_t port)
{
    if (port >= ctrl->num_ports) return -EINVAL;
    size_t   offset = EHCI_OP_PORTSC + port * EHCI_PORT_STRIDE;
    uint32_t portsc = ehci_read32(ctrl->operational, offset);
    if (!(portsc & EHCI_PORT_CCS)) return -ENODEV;
    uint32_t value = portsc | EHCI_PORT_PR;
    ehci_write32(ctrl->operational, offset, value);
    uint64_t deadline = nano_time() + 100 * 1000000ULL;
    while (ehci_read32(ctrl->operational, offset) & EHCI_PORT_PR) {
        if (nano_time() >= deadline) return -ETIMEDOUT;
        __asm__ volatile("pause");
    }
    portsc = ehci_read32(ctrl->operational, offset);
    ehci_write32(ctrl->operational, offset, portsc | EHCI_PORT_CHANGE_BITS);
    return (portsc & EHCI_PORT_CCS) ? EOK : -ENODEV;
}

static usb_speed_t ehci_port_speed_type(uint32_t portsc)
{
    uint8_t speed = (uint8_t)((portsc & EHCI_PORT_SPEED_MASK) >> EHCI_PORT_SPEED_SHIFT);
    switch (speed) {
        case 0 :
            return USB_SPEED_FULL;
        case 1 :
            return USB_SPEED_LOW;
        default :
            return USB_SPEED_HIGH;
    }
}

static void ehci_usb_device_release(struct device *dev)
{
    (void)dev;
}

static int ehci_get_string(usb_device_t *device, uint8_t index, uint16_t language, char *output, size_t capacity)
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

static int ehci_enumerate_port(ehci_controller_t *ctrl, uint8_t port)
{
    int result = ehci_port_reset(ctrl, port);
    if (result != EOK) return result;

    size_t      offset = EHCI_OP_PORTSC + port * EHCI_PORT_STRIDE;
    usb_speed_t speed  = ehci_port_speed_type(ehci_read32(ctrl->operational, offset));

    usb_device_t *device = calloc(1, sizeof(*device));
    if (!device) return -ENOMEM;
    device->connected   = true;
    device->speed       = speed;
    device->bus_number  = ctrl->bus_number;
    device->port_number = port;
    device->hcd_ops     = &ehci_hcd_ops;
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
    ehci_get_string(device, device->descriptor.manufacturer, language, device->manufacturer, sizeof(device->manufacturer));
    ehci_get_string(device, device->descriptor.product, language, device->product, sizeof(device->product));
    ehci_get_string(device, device->descriptor.serial_number, language, device->serial, sizeof(device->serial));

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
                             configuration, header.total_length, USB_CTRL_TIMEOUT_MS);
    device->dev.release = ehci_usb_device_release;
    if (result == EOK) result = usb_add_device(device, configuration, header.total_length);
    free(configuration);
    if (result == EOK) return EOK;

fail:
    usb_remove_device(device);
    free(device);
    return result;
}

static void ehci_worker(void *argument)
{
    ehci_controller_t *ctrl = argument;
    while (ctrl->running) {
        uint32_t sts = ehci_read32(ctrl->operational, EHCI_OP_USBSTS);
        if (!(sts & EHCI_STS_PCD)) {
            __asm__ volatile("pause");
            continue;
        }
        ehci_write32(ctrl->operational, EHCI_OP_USBSTS, EHCI_STS_PCD);
        for (uint8_t port = 0; port < ctrl->num_ports; port++) {
            size_t   offset = EHCI_OP_PORTSC + port * EHCI_PORT_STRIDE;
            uint32_t portsc = ehci_read32(ctrl->operational, offset);
            uint32_t change = portsc & EHCI_PORT_CHANGE_BITS;
            if (!change) continue;
            ehci_write32(ctrl->operational, offset, portsc);
            if ((portsc & EHCI_PORT_CCS) && (portsc & EHCI_PORT_CSC)) {
                msleep(100);
                int ret = ehci_enumerate_port(ctrl, port);
                if (ret != EOK) plogk("ehci: Port %u enumeration failed: %d\n", port, ret);
            }
        }
    }
}

static void ehci_interrupt_handler(void *frame)
{
    (void)frame;
    for (size_t i = 0; i < ehci_controller_count; i++) {
        ehci_controller_t *ctrl = ehci_controllers[i];
        if (!ctrl || !ctrl->running) continue;
        uint32_t sts = ehci_read32(ctrl->operational, EHCI_OP_USBSTS);
        if (!(sts & (EHCI_STS_INT | EHCI_STS_PCD | EHCI_STS_ERR | EHCI_STS_IAA))) continue;
        if (sts & EHCI_STS_PCD) {
            ehci_write32(ctrl->operational, EHCI_OP_USBSTS, EHCI_STS_PCD);
            ctrl->pending_ports++;
        }
        if (sts & EHCI_STS_INT) { ehci_write32(ctrl->operational, EHCI_OP_USBSTS, EHCI_STS_INT); }
        if (sts & EHCI_STS_ERR) { ehci_write32(ctrl->operational, EHCI_OP_USBSTS, EHCI_STS_ERR); }
        if (sts & EHCI_STS_IAA) { ehci_write32(ctrl->operational, EHCI_OP_USBSTS, EHCI_STS_IAA); }
    }
    send_eoi();
}

static int ehci_host_start(usb_host_t *host)
{
    (void)host;
    return EOK;
}

static void ehci_host_stop(usb_host_t *host)
{
    ehci_controller_t *ctrl = container_of(host, ehci_controller_t, hcd);
    ctrl->running           = false;
    ehci_write32(ctrl->operational, EHCI_OP_USBCMD, 0);
    ehci_write32(ctrl->operational, EHCI_OP_USBINTR, 0);
}

static int ehci_port_reset_hcd(usb_host_t *host, uint8_t port)
{
    ehci_controller_t *ctrl = container_of(host, ehci_controller_t, hcd);
    return ehci_port_reset(ctrl, port);
}

static int ehci_port_speed_hcd(usb_host_t *host, uint8_t port)
{
    ehci_controller_t *ctrl = container_of(host, ehci_controller_t, hcd);
    if (port >= ctrl->num_ports) return USB_SPEED_HIGH;
    size_t offset = EHCI_OP_PORTSC + port * EHCI_PORT_STRIDE;
    return ehci_port_speed_type(ehci_read32(ctrl->operational, offset));
}

static int ehci_port_connected_hcd(usb_host_t *host, uint8_t port)
{
    ehci_controller_t *ctrl = container_of(host, ehci_controller_t, hcd);
    if (port >= ctrl->num_ports) return 0;
    size_t offset = EHCI_OP_PORTSC + port * EHCI_PORT_STRIDE;
    return !!(ehci_read32(ctrl->operational, offset) & EHCI_PORT_CCS);
}

static usb_host_controller_ops_t ehci_controller_ops = {
    .host_start     = ehci_host_start,
    .host_stop      = ehci_host_stop,
    .port_reset     = ehci_port_reset_hcd,
    .port_speed     = ehci_port_speed_hcd,
    .port_connected = ehci_port_connected_hcd,
};

static int ehci_probe(pci_device_cache_t *pci, uint8_t bus_number)
{
    base_address_register_t bar = get_base_address_register(pci, 0);
    if (bar.type != mem_mapping || !bar.address) return -ENODEV;

    uint64_t bar_physical = (uint64_t)virt_to_phys((uint64_t)bar.address);
    uint64_t bar_size     = bar.size & ~BAR_64BIT_FLAG;
    if (!bar_size) bar_size = PAGE_4K_SIZE;
    uint64_t map_start  = ALIGN_DOWN(bar_physical, PAGE_4K_SIZE);
    uint64_t map_length = ALIGN_UP(bar_physical + bar_size, PAGE_4K_SIZE) - map_start;
    page_map_range_to(get_kernel_pagedir(), map_start, map_length, PTE_MMIO_FLAGS);

    ehci_controller_t *ctrl = calloc(1, sizeof(*ctrl));
    if (!ctrl) return -ENOMEM;

    ctrl->capability         = bar.address;
    ctrl->pci                = pci;
    ctrl->bus_number         = bus_number;
    ctrl->hcd.type           = USB_HOST_EHCI;
    ctrl->hcd.bus_number     = bus_number;
    ctrl->hcd.pci_dev        = pci;
    ctrl->hcd.hcd_ops        = &ehci_hcd_ops;
    ctrl->hcd.controller_ops = &ehci_controller_ops;
    ctrl->hcd.hc_private     = ctrl;
    snprintf(ctrl->hcd.name, sizeof(ctrl->hcd.name), "ehci-usb%u", bus_number);

    ctrl->caplength = *(volatile uint8_t *)ctrl->capability;
    if (ctrl->caplength < 0x20) ctrl->caplength = 0x20;
    ctrl->operational = ctrl->capability + ctrl->caplength;

    uint32_t hcsparams = ehci_read32(ctrl->capability, EHCI_CAP_HCSPARAMS);
    ctrl->num_ports    = hcsparams & EHCI_HCS_PORTS_MASK;
    if (ctrl->num_ports > EHCI_MAX_PORTS) ctrl->num_ports = EHCI_MAX_PORTS;
    ctrl->hcd.max_ports = ctrl->num_ports;

    uint32_t command = pci_read_command_status(pci) & 0xffff;
    pci_write_command_status(pci, command | 0x06);

    ehci_write32(ctrl->operational, EHCI_OP_USBCMD, EHCI_CMD_HCRESET);
    int ret = ehci_wait_register(ctrl->operational, EHCI_OP_USBCMD, EHCI_CMD_HCRESET, 0, 100);
    if (ret != EOK) {
        free(ctrl);
        return ret;
    }

    ctrl->periodic_list = ehci_dma_alloc(PAGE_4K_SIZE, &ctrl->periodic_list_physical);
    if (!ctrl->periodic_list) {
        free(ctrl);
        return -ENOMEM;
    }
    memset(ctrl->periodic_list, 0, PAGE_4K_SIZE);

    for (int i = 0; i < EHCI_NUM_QH; i++) {
        uint64_t phys;
        ctrl->qhs[i].virtual = ehci_dma_alloc(sizeof(ehci_qh_t), &phys);
        if (!ctrl->qhs[i].virtual) {
            for (int j = 0; j < i; j++) ehci_dma_free(ctrl->qhs[j].physical, sizeof(ehci_qh_t));
            ehci_dma_free(ctrl->periodic_list_physical, PAGE_4K_SIZE);
            free(ctrl);
            return -ENOMEM;
        }
        ctrl->qhs[i].physical                 = phys;
        ctrl->qhs[i].virtual->horizontal_link = EHCI_QHL_TERMINATE;
        ctrl->qhs[i].virtual->next_qtd        = EHCI_QTD_NEXT_TERMINATE;
        ctrl->qhs[i].virtual->alt_next_qtd    = EHCI_QTD_NEXT_TERMINATE;
    }
    for (int i = 0; i < EHCI_NUM_QTD; i++) {
        uint64_t phys;
        ctrl->qtds[i].virtual = ehci_dma_alloc(sizeof(ehci_qtd_t), &phys);
        if (!ctrl->qtds[i].virtual) {
            for (int j = 0; j < i; j++) ehci_dma_free(ctrl->qtds[j].physical, sizeof(ehci_qtd_t));
            for (int j = 0; j < EHCI_NUM_QH; j++) ehci_dma_free(ctrl->qhs[j].physical, sizeof(ehci_qh_t));
            ehci_dma_free(ctrl->periodic_list_physical, PAGE_4K_SIZE);
            free(ctrl);
            return -ENOMEM;
        }
        ctrl->qtds[i].physical = phys;
    }

    ehci_write32(ctrl->operational, EHCI_OP_PERIODICLISTBASE, (uint32_t)ctrl->periodic_list_physical);
    ehci_write32(ctrl->operational, EHCI_OP_ASYNCLISTADDR, 0);
    ehci_write32(ctrl->operational, EHCI_OP_USBCMD, EHCI_CMD_RUN | EHCI_CMD_ASENE | EHCI_CMD_PSEN);
    ret = ehci_wait_register(ctrl->operational, EHCI_OP_USBSTS, EHCI_STS_HCH, 0, 100);
    if (ret != EOK) {
        ehci_dma_free(ctrl->periodic_list_physical, PAGE_4K_SIZE);
        free(ctrl);
        return ret;
    }

    ehci_write32(ctrl->operational, EHCI_OP_USBINTR, EHCI_INTR_PCD | EHCI_INTR_TX | EHCI_INTR_ERR | EHCI_INTR_IAA);
    ehci_write32(ctrl->operational, EHCI_OP_CONFIGFLAG, 1);

    uint32_t irq = pci_get_irq(pci);
    ctrl->vector = (int)irq;
    pci_msi_init(pci);
    int msi_vector = pci_enable_msi(pci);
    if (msi_vector >= 0) ctrl->vector = msi_vector;
    if (ctrl->vector > 0) { register_interrupt_handler((uint16_t)ctrl->vector, ehci_interrupt_handler, 0, 0x8e); }

    ctrl->running     = true;
    ctrl->worker_task = kthread_create("ehci-hub", ehci_worker, ctrl);
    if (ctrl->worker_task) task_wakeup(ctrl->worker_task);
    ehci_controllers[ehci_controller_count++] = ctrl;
    usb_host_register(&ctrl->hcd);

    for (uint8_t port = 0; port < ctrl->num_ports; port++) {
        size_t   offset = EHCI_OP_PORTSC + port * EHCI_PORT_STRIDE;
        uint32_t portsc = ehci_read32(ctrl->operational, offset);
        ehci_write32(ctrl->operational, offset, portsc);
        if (portsc & EHCI_PORT_CCS) {
            ret = ehci_enumerate_port(ctrl, port);
            if (ret != EOK) plogk("ehci: Port %u enumeration failed: %d\n", port, ret);
        }
    }

    plogk("ehci: Controller at MMIO %p, bus usb%u, %u ports\n", (void *)bar.address, bus_number, ctrl->num_ports);
    return EOK;
}

void ehci_init(void)
{
#if !CONFIG_USB_EHCI
    return;
#endif
    if (usb_core_init() != EOK) return;
    pci_devices_cache_t *cache = pci_get_devices_cache();
    if (!cache) return;
    uint8_t bus_number = 1;
    for (pci_device_cache_t *pci = cache->head; pci && ehci_controller_count < EHCI_MAX_CONTROLLERS; pci = pci->next) {
        if (pci->class_code != EHCI_PCI_CLASS) continue;
        if (ehci_probe(pci, bus_number) == EOK) bus_number++;
    }
}

void ehci_start_workers(void)
{
#if !CONFIG_USB_EHCI
    return;
#endif
}
