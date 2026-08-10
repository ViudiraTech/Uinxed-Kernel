/*
 *
 *      xhci.c
 *      PCI xHCI host-controller driver
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <drivers/bus/pci.h>
#include <drivers/firmware/apic.h>
#include <drivers/usb/core/usb.h>
#include <drivers/usb/host/host.h>
#include <drivers/usb/host/xhci/xhci.h>
#include <kernel/errno.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <process/sched.h>
#include <process/task.h>

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/*
 * Overview
 * xHCI is the USB 3.x/2.0/1.1 host controller. All transfers run as
 * TRBs on ring buffers (one command ring, one event ring, plus a
 * ring per endpoint); slots represent addressed devices. The driver
 * submits TRBs, rings the doorbell and processes transfer events.
 */

static void xhci_usb_device_release(struct device *dev);

#define XHCI_PCI_CLASS 0x0c0330

#define XHCI_CAP_HCSPARAMS1 0x04
#define XHCI_CAP_HCSPARAMS2 0x08
#define XHCI_CAP_HCCPARAMS1 0x10
#define XHCI_CAP_DBOFF      0x14
#define XHCI_CAP_RTSOFF     0x18

#define XHCI_OP_USBCMD   0x00
#define XHCI_OP_USBSTS   0x04
#define XHCI_OP_PAGESIZE 0x08
#define XHCI_OP_DNCTRL   0x14
#define XHCI_OP_CRCR     0x18
#define XHCI_OP_DCBAAP   0x30
#define XHCI_OP_CONFIG   0x38
#define XHCI_OP_PORTS    0x400
#define XHCI_PORT_STRIDE 0x10

#define XHCI_CMD_RUN    (1U << 0)
#define XHCI_CMD_RESET  (1U << 1)
#define XHCI_CMD_INTE   (1U << 2)
#define XHCI_CMD_HSEE   (1U << 3)
#define XHCI_STS_HALTED (1U << 0)
#define XHCI_STS_FATAL  (1U << 2)
#define XHCI_STS_EINT   (1U << 3)
#define XHCI_STS_PCD    (1U << 4)
#define XHCI_STS_CNR    (1U << 11)

#define XHCI_PORT_CCS         (1U << 0)
#define XHCI_PORT_PED         (1U << 1)
#define XHCI_PORT_PR          (1U << 4)
#define XHCI_PORT_PP          (1U << 9)
#define XHCI_PORT_SPEED_SHIFT 10
#define XHCI_PORT_SPEED_MASK  (0x0fU << XHCI_PORT_SPEED_SHIFT)
#define XHCI_PORT_CSC         (1U << 17)
#define XHCI_PORT_PEC         (1U << 18)
#define XHCI_PORT_WRC         (1U << 19)
#define XHCI_PORT_OCC         (1U << 20)
#define XHCI_PORT_PRC         (1U << 21)
#define XHCI_PORT_PLC         (1U << 22)
#define XHCI_PORT_CEC         (1U << 23)
#define XHCI_PORT_CHANGE_BITS (XHCI_PORT_CSC | XHCI_PORT_PEC | XHCI_PORT_WRC | XHCI_PORT_OCC | XHCI_PORT_PRC | XHCI_PORT_PLC | XHCI_PORT_CEC)

#define XHCI_RT_INTERRUPTER0 0x20
#define XHCI_IR_IMAN         0x00
#define XHCI_IR_IMOD         0x04
#define XHCI_IR_ERSTSZ       0x08
#define XHCI_IR_ERSTBA       0x10
#define XHCI_IR_ERDP         0x18
#define XHCI_IMAN_IP         (1U << 0)
#define XHCI_IMAN_IE         (1U << 1)
#define XHCI_ERDP_EHB        (1U << 3)

#define XHCI_CONTEXT_ENTRIES_SHIFT      27
#define XHCI_SLOT_SPEED_SHIFT           20
#define XHCI_SLOT_ROOT_PORT_SHIFT       16
#define XHCI_ENDPOINT_TYPE_SHIFT        3
#define XHCI_ENDPOINT_MAX_BURST_SHIFT   8
#define XHCI_ENDPOINT_MAX_PACKET_SHIFT  16
#define XHCI_ENDPOINT_INTERVAL_SHIFT    16
#define XHCI_ENDPOINT_ERROR_COUNT_SHIFT 1

#define XHCI_COMPLETION_SUCCESS      1
#define XHCI_COMPLETION_SHORT_PACKET 13
#define XHCI_COMPLETION_STOPPED      26

#define XHCI_RING_TRBS      (PAGE_4K_SIZE / sizeof(xhci_trb_t))
#define XHCI_EVENT_TRBS     XHCI_RING_TRBS
#define XHCI_MAX_ROOT_PORTS 64
#define XHCI_MAX_SLOTS      255
#define XHCI_MAX_ENDPOINTS  32

typedef struct __attribute__((packed, aligned(16))) {
        uint64_t address;
        uint32_t size;
        uint32_t reserved;
} xhci_erst_entry_t;

typedef struct xhci_controller xhci_controller_t;
typedef struct xhci_slot       xhci_slot_t;

typedef struct {
        xhci_ring_t           ring;
        uint64_t              ring_physical;
        struct xhci_transfer *periodic;
} xhci_endpoint_state_t;

typedef struct xhci_transfer {
        xhci_slot_t             *slot;
        usb_endpoint_t          *endpoint;
        xhci_endpoint_state_t   *endpoint_state;
        uint64_t                 dma_physical;
        void                    *dma_virtual;
        size_t                   dma_pages;
        size_t                   length;
        size_t                   actual;
        uint64_t                 trb_physical;
        usb_interrupt_complete_t complete;
        void                    *context;
        int                      status;
        volatile bool            completed;
        bool                     periodic;
        volatile bool            active;
} xhci_transfer_t;

typedef struct xhci_slot {
        xhci_controller_t    *controller;
        usb_device_t          usb;
        uint8_t               slot_id;
        uint8_t               port_id;
        uint8_t               context_entries;
        uint64_t              output_context_physical;
        uint64_t              input_context_physical;
        uint32_t             *output_context;
        uint32_t             *input_context;
        xhci_endpoint_state_t endpoints[XHCI_MAX_ENDPOINTS];
        xhci_transfer_t      *pending[XHCI_MAX_ENDPOINTS];
} xhci_slot_t;

typedef struct {
        uint64_t      trb_physical;
        volatile bool completed;
        uint8_t       completion_code;
        uint8_t       slot_id;
} xhci_command_wait_t;

typedef struct xhci_controller {
        pci_device_cache_t  *pci;
        volatile uint8_t    *capability;
        volatile uint8_t    *operational;
        volatile uint8_t    *runtime;
        volatile uint32_t   *doorbells;
        uint8_t              capability_length;
        uint8_t              max_slots;
        uint8_t              max_ports;
        uint8_t              context_size;
        uint8_t              bus_number;
        uint8_t              irq_slot;
        int                  vector;
        uint64_t             dcbaa_physical;
        uint64_t            *dcbaa;
        uint64_t             scratchpad_array_physical;
        uint64_t            *scratchpad_array;
        uint16_t             scratchpad_count;
        xhci_ring_t          command_ring;
        uint64_t             command_ring_physical;
        xhci_trb_t          *event_ring;
        uint64_t             event_ring_physical;
        uint16_t             event_dequeue;
        uint8_t              event_cycle;
        uint64_t             erst_physical;
        xhci_erst_entry_t   *erst;
        xhci_slot_t         *slots[XHCI_MAX_SLOTS + 1];
        xhci_command_wait_t *pending_command;
        uint64_t             pending_ports;
        wait_queue_t         worker_wait;
        task_t              *worker_task;
        spinlock_t           event_lock;
        spinlock_t           command_lock;
        spinlock_t           port_lock;
        bool                 running;
        bool                 interrupt_enabled;
        bool                 msix_enabled;
        bool                 worker_started;
        bool                 stopping;
} xhci_controller_t;

static xhci_controller_t *xhci_controllers[USB_MAX_CONTROLLERS];
static xhci_controller_t *xhci_irq_slots[USB_MAX_CONTROLLERS];
static size_t             xhci_controller_count;
static spinlock_t         xhci_irq_lock;

/*
 * MMIO and DMA helpers
 * The xHCI register space is memory-mapped; all access goes through
 * these helpers. DMA buffers are page-frame allocations translated
 * to virtual addresses for software use.
 */

static uint32_t xhci_read32(const volatile uint8_t *base, size_t offset)
{
    return *(volatile const uint32_t *)(base + offset);
}

static uint64_t xhci_read64(const volatile uint8_t *base, size_t offset)
{
    uint32_t low  = xhci_read32(base, offset);
    uint32_t high = xhci_read32(base, offset + 4);
    return (uint64_t)low | (uint64_t)high << 32;
}

static void xhci_write32(volatile uint8_t *base, size_t offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static void xhci_write64(volatile uint8_t *base, size_t offset, uint64_t value)
{
    xhci_write32(base, offset, (uint32_t)value);
    xhci_write32(base, offset + 4, (uint32_t)(value >> 32));
}

static void *xhci_dma_alloc(size_t size, uint64_t *physical, size_t *pages)
{
    size_t   count   = (size + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    uint64_t address = alloc_frames(count);
    if (!address) return NULL;
    void *memory = phys_to_virt(address);
    memset(memory, 0, count * PAGE_4K_SIZE);
    *physical = address;
    if (pages) *pages = count;
    return memory;
}

static void xhci_dma_free(uint64_t physical, size_t pages)
{
    if (physical && pages) free_frames(physical, pages);
}

/* Poll an MMIO register until a mask matches or the timeout elapses */
static int xhci_wait_register(volatile uint8_t *base, size_t offset, uint32_t mask, uint32_t value, uint32_t timeout_ms)
{
    uint64_t deadline = nano_time() + (uint64_t)timeout_ms * 1000000ULL;
    while ((xhci_read32(base, offset) & mask) != value) {
        if (nano_time() >= deadline) return -ETIMEDOUT;
        __asm__ volatile("pause");
    }
    return EOK;
}

static uint32_t *xhci_output_context(xhci_slot_t *slot, uint8_t dci)
{
    return (uint32_t *)((uint8_t *)slot->output_context + (size_t)dci * slot->controller->context_size);
}

static uint32_t *xhci_input_context(xhci_slot_t *slot, uint8_t dci)
{
    return (uint32_t *)((uint8_t *)slot->input_context + (size_t)(dci + 1) * slot->controller->context_size);
}

static uint8_t xhci_endpoint_dci(const usb_endpoint_t *endpoint)
{
    uint8_t number = endpoint->descriptor.endpoint_address & USB_ENDPOINT_NUMBER_MASK;
    return (uint8_t)(number * 2 + !!(endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK));
}

static void xhci_ring_doorbell(xhci_controller_t *controller, uint8_t slot_id, uint8_t endpoint_id)
{
    dma_write_barrier();
    controller->doorbells[slot_id] = endpoint_id;
}

static int xhci_completion_status(uint8_t completion_code)
{
    switch (completion_code) {
        case XHCI_COMPLETION_SUCCESS :
        case XHCI_COMPLETION_SHORT_PACKET :
            return EOK;
        case XHCI_COMPLETION_STOPPED :
            return -ECANCELED;
        default :
            return -EIO;
    }
}

static int xhci_submit_periodic(xhci_transfer_t *transfer)
{
    uint64_t physical;
    if (!transfer || !transfer->active || !transfer->endpoint_state) return -ENODEV;
    if (!xhci_ring_enqueue(&transfer->endpoint_state->ring, transfer->dma_physical, (uint32_t)transfer->length,
                           XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC, &physical))
        return -EIO;
    transfer->trb_physical = physical;
    transfer->completed    = false;
    uint8_t dci            = xhci_endpoint_dci(transfer->endpoint);
    __atomic_store_n(&transfer->slot->pending[dci], transfer, __ATOMIC_RELEASE);
    xhci_ring_doorbell(transfer->slot->controller, transfer->slot->slot_id, dci);
    return EOK;
}

static void xhci_handle_transfer_event(xhci_controller_t *controller, const xhci_trb_t *event)
{
    uint8_t slot_id = event->control >> 24;
    uint8_t dci     = (event->control >> 16) & 0x1f;
    if (!slot_id || slot_id > controller->max_slots || dci >= XHCI_MAX_ENDPOINTS) return;
    xhci_slot_t *slot = controller->slots[slot_id];
    if (!slot) return;
    xhci_transfer_t *transfer = __atomic_load_n(&slot->pending[dci], __ATOMIC_ACQUIRE);
    if (!transfer || transfer->trb_physical != event->parameter) return;

    uint8_t  completion = event->status >> 24;
    uint32_t residual   = event->status & 0x00ffffff;
    transfer->status    = xhci_completion_status(completion);
    transfer->actual    = residual <= transfer->length ? transfer->length - residual : 0;
    __atomic_store_n(&slot->pending[dci], NULL, __ATOMIC_RELEASE);
    if (transfer->periodic) {
        if (transfer->active && transfer->complete)
            transfer->complete(transfer->endpoint, transfer->dma_virtual, transfer->actual, transfer->status, transfer->context);
        if (transfer->active && xhci_submit_periodic(transfer) != EOK) transfer->active = false;
    } else {
        __atomic_store_n(&transfer->completed, true, __ATOMIC_RELEASE);
    }
}

static void xhci_handle_event(xhci_controller_t *controller, const xhci_trb_t *event)
{
    uint8_t type = (event->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
    if (type == XHCI_TRB_COMMAND_COMPLETION) {
        xhci_command_wait_t *wait = controller->pending_command;
        if (wait && wait->trb_physical == event->parameter) {
            wait->completion_code = event->status >> 24;
            wait->slot_id         = event->control >> 24;
            __atomic_store_n(&wait->completed, true, __ATOMIC_RELEASE);
        }
    } else if (type == XHCI_TRB_TRANSFER_EVENT) {
        xhci_handle_transfer_event(controller, event);
    } else if (type == XHCI_TRB_PORT_STATUS_CHANGE) {
        uint8_t port_id = (event->parameter >> 24) & 0xff;
        if (port_id && port_id <= controller->max_ports) {
            uint64_t flags = spin_lock_irqsave(&controller->port_lock);
            controller->pending_ports |= 1ULL << (port_id - 1);
            spin_unlock_irqrestore(&controller->port_lock, flags);
            if (controller->worker_started) wait_queue_wake_one(&controller->worker_wait);
        }
    }
}

static void xhci_process_events(xhci_controller_t *controller)
{
    uint64_t flags = spin_lock_irqsave(&controller->event_lock);
    while (1) {
        xhci_trb_t *source = &controller->event_ring[controller->event_dequeue];
        dma_read_barrier();
        if ((source->control & XHCI_TRB_CYCLE) != controller->event_cycle) break;
        xhci_trb_t event = *source;
        controller->event_dequeue++;
        if (controller->event_dequeue == XHCI_EVENT_TRBS) {
            controller->event_dequeue = 0;
            controller->event_cycle ^= 1;
        }
        xhci_handle_event(controller, &event);
    }
    uint64_t dequeue = controller->event_ring_physical + (uint64_t)controller->event_dequeue * sizeof(xhci_trb_t);
    xhci_write64(controller->runtime + XHCI_RT_INTERRUPTER0, XHCI_IR_ERDP, dequeue | XHCI_ERDP_EHB);
    uint32_t iman = xhci_read32(controller->runtime + XHCI_RT_INTERRUPTER0, XHCI_IR_IMAN);
    xhci_write32(controller->runtime + XHCI_RT_INTERRUPTER0, XHCI_IR_IMAN, iman | XHCI_IMAN_IP | XHCI_IMAN_IE);
    spin_unlock_irqrestore(&controller->event_lock, flags);
}

static int xhci_wait_flag(xhci_controller_t *controller, volatile bool *completed, uint32_t timeout_ms)
{
    uint64_t deadline = nano_time() + (uint64_t)timeout_ms * 1000000ULL;
    while (!__atomic_load_n(completed, __ATOMIC_ACQUIRE)) {
        xhci_process_events(controller);
        if (xhci_read32(controller->operational, XHCI_OP_USBSTS) & XHCI_STS_FATAL) {
            plogk("xhci: Host system error on bus %u\n", controller->bus_number);
            return -EIO;
        }
        if (nano_time() >= deadline) return -ETIMEDOUT;
        __asm__ volatile("pause");
    }
    return EOK;
}

static int xhci_command(xhci_controller_t *controller, uint64_t parameter, uint32_t status, uint32_t control, uint8_t *slot_id)
{
    xhci_command_wait_t wait = {0};
    spin_lock(&controller->command_lock);
    if (!xhci_ring_enqueue(&controller->command_ring, parameter, status, control, &wait.trb_physical)) {
        plogk("xhci: Command ring full on bus %u\n", controller->bus_number);
        spin_unlock(&controller->command_lock);
        return -EIO;
    }
    controller->pending_command = &wait;
    xhci_ring_doorbell(controller, 0, 0);
    int result                  = xhci_wait_flag(controller, &wait.completed, USB_CTRL_TIMEOUT_MS);
    controller->pending_command = NULL;
    if (result == EOK) result = xhci_completion_status(wait.completion_code);
    if (result == EOK && slot_id) *slot_id = wait.slot_id;
    spin_unlock(&controller->command_lock);
    if (result != EOK) plogk("xhci: Command failed on bus %u (%d)\n", controller->bus_number, result);
    return result;
}

static int xhci_wait_transfer(xhci_transfer_t *transfer, uint32_t timeout_ms)
{
    int result = xhci_wait_flag(transfer->slot->controller, &transfer->completed, timeout_ms);
    if (result != EOK) {
        plogk("xhci: Transfer timed out on bus %u slot %u\n", transfer->slot->controller->bus_number, transfer->slot->slot_id);
        uint8_t dci = transfer->endpoint ? xhci_endpoint_dci(transfer->endpoint) : 1;
        if (__atomic_load_n(&transfer->slot->pending[dci], __ATOMIC_ACQUIRE) == transfer)
            __atomic_store_n(&transfer->slot->pending[dci], NULL, __ATOMIC_RELEASE);
        (void)xhci_command(transfer->slot->controller, 0, 0,
                           XHCI_TRB_TYPE(XHCI_TRB_STOP_ENDPOINT) | ((uint32_t)dci << 16) | ((uint32_t)transfer->slot->slot_id << 24), NULL);
        return result;
    }
    return transfer->status;
}

/*
 * Transfer submission
 * Control transfers are encoded as SETUP / DATA / STATUS TRBs on
 * endpoint 1's ring; bulk and interrupt transfers use their own
 * endpoint rings. Completion is reported through a transfer event.
 */

static int xhci_control(usb_device_t *device, const usb_setup_packet_t *setup, void *buffer, size_t length, uint32_t timeout_ms)
{
    xhci_slot_t *slot = device ? device->hc_private : NULL;
    if (!slot || !setup || length > PAGE_4K_SIZE) return -EINVAL;
    xhci_endpoint_state_t *endpoint = &slot->endpoints[1];
    xhci_transfer_t        transfer = {.slot = slot, .endpoint_state = endpoint, .length = length, .active = true};
    if (length) {
        transfer.dma_virtual = xhci_dma_alloc(length, &transfer.dma_physical, &transfer.dma_pages);
        if (!transfer.dma_virtual) {
            plogk("xhci: Control transfer DMA allocation failed on bus %u (%zu bytes)\n", slot->controller->bus_number, length);
            return -ENOMEM;
        }
        if (!(setup->request_type & USB_DIR_IN)) memcpy(transfer.dma_virtual, buffer, length);
    }

    uint16_t saved_enqueue = endpoint->ring.enqueue;
    uint8_t  saved_cycle   = endpoint->ring.cycle;
    uint64_t setup_data    = 0;
    memcpy(&setup_data, setup, sizeof(*setup));
    uint32_t transfer_type = length ? ((setup->request_type & USB_DIR_IN) ? 3U : 2U) : 0U;
    if (!xhci_ring_enqueue(&endpoint->ring, setup_data, 8, XHCI_TRB_TYPE(XHCI_TRB_SETUP_STAGE) | XHCI_TRB_IDT | (transfer_type << 16), NULL))
        goto io_error;
    if (length
        && !xhci_ring_enqueue(&endpoint->ring, transfer.dma_physical, (uint32_t)length,
                              XHCI_TRB_TYPE(XHCI_TRB_DATA_STAGE) | ((setup->request_type & USB_DIR_IN) ? XHCI_TRB_DIR_IN : 0), NULL))
        goto io_error;
    uint32_t status_control = XHCI_TRB_TYPE(XHCI_TRB_STATUS_STAGE) | XHCI_TRB_IOC;
    if (!length || !(setup->request_type & USB_DIR_IN)) status_control |= XHCI_TRB_DIR_IN;
    if (!xhci_ring_enqueue(&endpoint->ring, 0, 0, status_control, &transfer.trb_physical)) goto io_error;
    __atomic_store_n(&slot->pending[1], &transfer, __ATOMIC_RELEASE);
    xhci_ring_doorbell(slot->controller, slot->slot_id, 1);
    int result = xhci_wait_transfer(&transfer, timeout_ms);
    if (result == EOK && length && (setup->request_type & USB_DIR_IN)) memcpy(buffer, transfer.dma_virtual, length);
    xhci_dma_free(transfer.dma_physical, transfer.dma_pages);
    return result;

io_error:
    endpoint->ring.enqueue = saved_enqueue;
    endpoint->ring.cycle   = saved_cycle;
    xhci_dma_free(transfer.dma_physical, transfer.dma_pages);
    return -EIO;
}

/* Submit a bulk/interrupt transfer on an endpoint ring */
static int xhci_transfer(usb_endpoint_t *usb_endpoint, void *buffer, size_t length, size_t *actual, uint32_t timeout_ms)
{
    if (!usb_endpoint || !buffer || !length || length > PAGE_4K_SIZE || !usb_endpoint->hc_private) return -EINVAL;
    xhci_slot_t           *slot     = usb_endpoint->interface->device->hc_private;
    xhci_endpoint_state_t *endpoint = usb_endpoint->hc_private;
    xhci_transfer_t        transfer = {.slot = slot, .endpoint = usb_endpoint, .endpoint_state = endpoint, .length = length, .active = true};
    transfer.dma_virtual            = xhci_dma_alloc(length, &transfer.dma_physical, &transfer.dma_pages);
    if (!transfer.dma_virtual) {
        plogk("xhci: Bulk transfer DMA allocation failed on bus %u (%zu bytes)\n", slot->controller->bus_number, length);
        return -ENOMEM;
    }
    bool input = (usb_endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK) != 0;
    if (!input) memcpy(transfer.dma_virtual, buffer, length);
    if (!xhci_ring_enqueue(&endpoint->ring, transfer.dma_physical, (uint32_t)length, XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC,
                           &transfer.trb_physical)) {
        xhci_dma_free(transfer.dma_physical, transfer.dma_pages);
        return -EIO;
    }
    uint8_t dci = xhci_endpoint_dci(usb_endpoint);
    __atomic_store_n(&slot->pending[dci], &transfer, __ATOMIC_RELEASE);
    xhci_ring_doorbell(slot->controller, slot->slot_id, dci);
    int result = xhci_wait_transfer(&transfer, timeout_ms);
    if (result == EOK && input) memcpy(buffer, transfer.dma_virtual, transfer.actual);
    if (actual) *actual = transfer.actual;
    xhci_dma_free(transfer.dma_physical, transfer.dma_pages);
    return result;
}

static int xhci_interrupt_start(usb_endpoint_t *usb_endpoint, size_t length, usb_interrupt_complete_t complete, void *context)
{
    if (!usb_endpoint || !usb_endpoint->hc_private || !length || length > PAGE_4K_SIZE || !complete) return -EINVAL;
    xhci_endpoint_state_t *endpoint = usb_endpoint->hc_private;
    if (endpoint->periodic) return -EBUSY;
    xhci_transfer_t *transfer = calloc(1, sizeof(*transfer));
    if (!transfer) return -ENOMEM;
    transfer->slot           = usb_endpoint->interface->device->hc_private;
    transfer->endpoint       = usb_endpoint;
    transfer->endpoint_state = endpoint;
    transfer->length         = length;
    transfer->complete       = complete;
    transfer->context        = context;
    transfer->periodic       = true;
    transfer->active         = true;
    transfer->dma_virtual    = xhci_dma_alloc(length, &transfer->dma_physical, &transfer->dma_pages);
    if (!transfer->dma_virtual) {
        plogk("xhci: Interrupt transfer DMA allocation failed on bus %u (%zu bytes)\n", transfer->slot->controller->bus_number, length);
        free(transfer);
        return -ENOMEM;
    }
    endpoint->periodic = transfer;
    int result         = xhci_submit_periodic(transfer);
    if (result != EOK) {
        endpoint->periodic = NULL;
        xhci_dma_free(transfer->dma_physical, transfer->dma_pages);
        free(transfer);
    }
    return result;
}

static void xhci_interrupt_stop(usb_endpoint_t *usb_endpoint)
{
    xhci_endpoint_state_t *endpoint = usb_endpoint ? usb_endpoint->hc_private : NULL;
    xhci_transfer_t       *transfer = endpoint ? endpoint->periodic : NULL;
    if (!transfer) return;
    transfer->active = false;
    uint8_t dci      = xhci_endpoint_dci(usb_endpoint);
    (void)xhci_command(transfer->slot->controller, 0, 0,
                       XHCI_TRB_TYPE(XHCI_TRB_STOP_ENDPOINT) | ((uint32_t)dci << 16) | ((uint32_t)transfer->slot->slot_id << 24), NULL);
    uint64_t flags = spin_lock_irqsave(&transfer->slot->controller->event_lock);
    if (__atomic_load_n(&transfer->slot->pending[dci], __ATOMIC_ACQUIRE) == transfer)
        __atomic_store_n(&transfer->slot->pending[dci], NULL, __ATOMIC_RELEASE);
    endpoint->periodic = NULL;
    spin_unlock_irqrestore(&transfer->slot->controller->event_lock, flags);
    xhci_dma_free(transfer->dma_physical, transfer->dma_pages);
    free(transfer);
}

static uint8_t xhci_endpoint_type(const usb_endpoint_t *endpoint)
{
    uint8_t transfer = endpoint->descriptor.attributes & USB_ENDPOINT_XFERTYPE_MASK;
    bool    input    = (endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK) != 0;
    if (transfer == USB_ENDPOINT_XFER_ISOC) return input ? 5 : 1;
    if (transfer == USB_ENDPOINT_XFER_BULK) return input ? 6 : 2;
    if (transfer == USB_ENDPOINT_XFER_INT) return input ? 7 : 3;
    return 4;
}

static uint8_t xhci_endpoint_interval(const usb_endpoint_t *endpoint)
{
    uint8_t     interval = endpoint->descriptor.interval;
    usb_speed_t speed    = endpoint->interface->device->speed;
    if (!interval) return 0;
    if (speed >= USB_SPEED_HIGH) return interval > 16 ? 15 : interval - 1;
    uint8_t exponent = 0;
    uint8_t value    = interval - 1;
    while (value) {
        value >>= 1;
        exponent++;
    }
    return exponent + 3;
}

static int xhci_configure_endpoint(usb_endpoint_t *usb_endpoint)
{
    if (!usb_endpoint || !usb_endpoint->interface || !usb_endpoint->interface->device) return -EINVAL;
    xhci_slot_t *slot = usb_endpoint->interface->device->hc_private;
    uint8_t      dci  = xhci_endpoint_dci(usb_endpoint);
    if (!slot || dci < 2 || dci >= XHCI_MAX_ENDPOINTS) return -EINVAL;
    xhci_endpoint_state_t *endpoint = &slot->endpoints[dci];
    if (endpoint->ring.trbs) {
        usb_endpoint->hc_private = endpoint;
        return EOK;
    }
    endpoint->ring.trbs = xhci_dma_alloc(PAGE_4K_SIZE, &endpoint->ring_physical, NULL);
    if (!endpoint->ring.trbs) return -ENOMEM;
    int result = xhci_ring_init(&endpoint->ring, endpoint->ring.trbs, endpoint->ring_physical, XHCI_RING_TRBS, true);
    if (result != EOK) goto fail;

    memset(slot->input_context, 0, PAGE_4K_SIZE);
    uint32_t *control = slot->input_context;
    control[1]        = 1U | (1U << dci);
    memcpy(xhci_input_context(slot, 0), xhci_output_context(slot, 0), slot->controller->context_size);
    if (dci > slot->context_entries) slot->context_entries = dci;
    uint32_t *slot_context = xhci_input_context(slot, 0);
    slot_context[0] &= ~(0x1fU << XHCI_CONTEXT_ENTRIES_SHIFT);
    slot_context[0] |= (uint32_t)slot->context_entries << XHCI_CONTEXT_ENTRIES_SHIFT;

    uint32_t *context    = xhci_input_context(slot, dci);
    uint16_t  max_packet = usb_endpoint->descriptor.max_packet_size & 0x07ff;
    if (max_packet < 1 || max_packet > 1024) max_packet = 512;
    context[0] = (uint32_t)xhci_endpoint_interval(usb_endpoint) << XHCI_ENDPOINT_INTERVAL_SHIFT;
    context[1] = (3U << XHCI_ENDPOINT_ERROR_COUNT_SHIFT) | ((uint32_t)xhci_endpoint_type(usb_endpoint) << XHCI_ENDPOINT_TYPE_SHIFT)
                 | ((uint32_t)max_packet << XHCI_ENDPOINT_MAX_PACKET_SHIFT);
    uint64_t dequeue = endpoint->ring_physical | 1U;
    context[2]       = (uint32_t)dequeue;
    context[3]       = (uint32_t)(dequeue >> 32);
    context[4]       = max_packet | ((uint32_t)max_packet << 16);
    dma_write_barrier();
    result = xhci_command(slot->controller, slot->input_context_physical, 0,
                          XHCI_TRB_TYPE(XHCI_TRB_CONFIGURE_ENDPOINT) | ((uint32_t)slot->slot_id << 24), NULL);
    if (result != EOK) goto fail;
    usb_endpoint->hc_private = endpoint;
    return EOK;

fail:
    xhci_dma_free(endpoint->ring_physical, 1);
    memset(endpoint, 0, sizeof(*endpoint));
    return result;
}

static void xhci_disable_endpoint(usb_endpoint_t *usb_endpoint)
{
    xhci_endpoint_state_t *endpoint = usb_endpoint ? usb_endpoint->hc_private : NULL;
    if (!endpoint) return;
    xhci_interrupt_stop(usb_endpoint);
}

static int xhci_clear_halt(usb_endpoint_t *usb_endpoint)
{
    if (!usb_endpoint || !usb_endpoint->hc_private) return -EINVAL;
    xhci_slot_t           *slot     = usb_endpoint->interface->device->hc_private;
    xhci_endpoint_state_t *endpoint = usb_endpoint->hc_private;
    uint8_t                dci      = xhci_endpoint_dci(usb_endpoint);
    int                    status   = xhci_command(slot->controller, 0, 0,
                                                   XHCI_TRB_TYPE(XHCI_TRB_RESET_ENDPOINT) | ((uint32_t)dci << 16) | ((uint32_t)slot->slot_id << 24), NULL);
    if (status != EOK) return status;
    uint64_t dequeue = endpoint->ring_physical + (uint64_t)endpoint->ring.enqueue * sizeof(xhci_trb_t);
    dequeue |= endpoint->ring.cycle;
    return xhci_command(slot->controller, dequeue, 0,
                        XHCI_TRB_TYPE(XHCI_TRB_SET_TR_DEQUEUE) | ((uint32_t)dci << 16) | ((uint32_t)slot->slot_id << 24), NULL);
}

static void xhci_disable_device(usb_device_t *device)
{
    (void)device;
}

static const usb_hcd_ops_t xhci_hcd_ops = {
    .control            = xhci_control,
    .transfer           = xhci_transfer,
    .interrupt_start    = xhci_interrupt_start,
    .interrupt_stop     = xhci_interrupt_stop,
    .configure_endpoint = xhci_configure_endpoint,
    .disable_endpoint   = xhci_disable_endpoint,
    .clear_halt         = xhci_clear_halt,
    .disable_device     = xhci_disable_device,
};

static int xhci_port_reset(xhci_controller_t *controller, uint8_t port_id)
{
    size_t   offset = XHCI_OP_PORTS + (size_t)(port_id - 1) * XHCI_PORT_STRIDE;
    uint32_t status = xhci_read32(controller->operational, offset);
    if (!(status & XHCI_PORT_CCS)) return -ENODEV;
    if (!(status & XHCI_PORT_PED)) {
        uint32_t value = status & ~XHCI_PORT_CHANGE_BITS;
        xhci_write32(controller->operational, offset, value | XHCI_PORT_PP | XHCI_PORT_PR);
        int result = xhci_wait_register(controller->operational, offset, XHCI_PORT_PR, 0, 1000);
        if (result != EOK) return result;
        status = xhci_read32(controller->operational, offset);
    }
    xhci_write32(controller->operational, offset, (status & ~XHCI_PORT_CHANGE_BITS) | (status & XHCI_PORT_CHANGE_BITS));
    return (status & XHCI_PORT_CCS) && (status & XHCI_PORT_PED) ? EOK : -ENODEV;
}

static usb_speed_t xhci_usb_speed(uint32_t port_status)
{
    switch ((port_status & XHCI_PORT_SPEED_MASK) >> XHCI_PORT_SPEED_SHIFT) {
        case 1 :
            return USB_SPEED_FULL;
        case 2 :
            return USB_SPEED_LOW;
        case 3 :
            return USB_SPEED_HIGH;
        case 4 :
            return USB_SPEED_SUPER;
        case 5 :
            return USB_SPEED_SUPER_PLUS;
        default :
            return USB_SPEED_FULL;
    }
}

static uint16_t xhci_ep0_packet_size(usb_speed_t speed)
{
    if (speed == USB_SPEED_SUPER || speed == USB_SPEED_SUPER_PLUS) return 512;
    if (speed == USB_SPEED_HIGH) return 64;
    return 8;
}

static int xhci_allocate_slot(xhci_controller_t *controller, uint8_t port_id, uint8_t slot_id, xhci_slot_t **result)
{
    xhci_slot_t *slot = calloc(1, sizeof(*slot));
    if (!slot) {
        plogk("xhci: Slot allocation failed on bus %u\n", controller->bus_number);
        return -ENOMEM;
    }
    slot->controller           = controller;
    slot->slot_id              = slot_id;
    slot->port_id              = port_id;
    slot->context_entries      = 1;
    slot->output_context       = xhci_dma_alloc(PAGE_4K_SIZE, &slot->output_context_physical, NULL);
    slot->input_context        = xhci_dma_alloc(PAGE_4K_SIZE, &slot->input_context_physical, NULL);
    xhci_endpoint_state_t *ep0 = &slot->endpoints[1];
    ep0->ring.trbs             = xhci_dma_alloc(PAGE_4K_SIZE, &ep0->ring_physical, NULL);
    if (!slot->output_context || !slot->input_context || !ep0->ring.trbs
        || xhci_ring_init(&ep0->ring, ep0->ring.trbs, ep0->ring_physical, XHCI_RING_TRBS, true) != EOK) {
        plogk("xhci: Slot %u context/ring allocation failed on bus %u\n", slot_id, controller->bus_number);
        xhci_dma_free(slot->output_context_physical, 1);
        xhci_dma_free(slot->input_context_physical, 1);
        xhci_dma_free(ep0->ring_physical, 1);
        free(slot);
        return -ENOMEM;
    }
    controller->dcbaa[slot_id] = slot->output_context_physical;
    controller->slots[slot_id] = slot;
    *result                    = slot;
    return EOK;
}

static int xhci_address_slot(xhci_slot_t *slot, usb_speed_t speed)
{
    memset(slot->input_context, 0, PAGE_4K_SIZE);
    uint32_t *control      = slot->input_context;
    control[1]             = 3;
    uint32_t *slot_context = xhci_input_context(slot, 0);
    slot_context[0]        = ((uint32_t)speed << XHCI_SLOT_SPEED_SHIFT) | (1U << XHCI_CONTEXT_ENTRIES_SHIFT);
    slot_context[1]        = (uint32_t)slot->port_id << XHCI_SLOT_ROOT_PORT_SHIFT;
    uint32_t *ep0_context  = xhci_input_context(slot, 1);
    uint16_t  max_packet   = xhci_ep0_packet_size(speed);
    ep0_context[1]
        = (3U << XHCI_ENDPOINT_ERROR_COUNT_SHIFT) | (4U << XHCI_ENDPOINT_TYPE_SHIFT) | ((uint32_t)max_packet << XHCI_ENDPOINT_MAX_PACKET_SHIFT);
    uint64_t dequeue = slot->endpoints[1].ring_physical | 1U;
    ep0_context[2]   = (uint32_t)dequeue;
    ep0_context[3]   = (uint32_t)(dequeue >> 32);
    ep0_context[4]   = 8;
    dma_write_barrier();
    return xhci_command(slot->controller, slot->input_context_physical, 0,
                        XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE) | ((uint32_t)slot->slot_id << 24), NULL);
}

static int xhci_get_string(usb_device_t *device, uint8_t index, uint16_t language, char *output, size_t capacity)
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

/*
 * Device enumeration
 * Reset the port, enable a slot, address the device, then read its
 * descriptors and register it with the USB core.
 */

static int xhci_enumerate_port(xhci_controller_t *controller, uint8_t port_id)
{
    int result = xhci_port_reset(controller, port_id);
    if (result != EOK) return result;
    uint8_t slot_id = 0;
    result          = xhci_command(controller, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT), &slot_id);
    if (result != EOK) return result;
    if (!slot_id || slot_id > controller->max_slots) {
        (void)xhci_command(controller, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_DISABLE_SLOT) | ((uint32_t)slot_id << 24), NULL);
        return -EIO;
    }
    xhci_slot_t *slot = NULL;
    result            = xhci_allocate_slot(controller, port_id, slot_id, &slot);
    if (result != EOK) goto free_slot;
    size_t      port_offset = XHCI_OP_PORTS + (size_t)(port_id - 1) * XHCI_PORT_STRIDE;
    usb_speed_t speed       = xhci_usb_speed(xhci_read32(controller->operational, port_offset));
    result                  = xhci_address_slot(slot, speed);
    if (result != EOK) goto free_slot;

    usb_device_t *device  = &slot->usb;
    device->connected     = true;
    device->speed         = speed;
    device->bus_number    = controller->bus_number;
    device->port_number   = port_id;
    device->hcd_ops       = &xhci_hcd_ops;
    device->hc_private    = slot;
    uint32_t *output_slot = xhci_output_context(slot, 0);
    device->address       = output_slot[3] & 0xff;
    if (!device->address) device->address = slot_id;
    (void)snprintf(device->path, sizeof(device->path), "%u-%u", device->bus_number, port_id);

    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0,
                             &device->descriptor, sizeof(device->descriptor), USB_CTRL_TIMEOUT_MS);
    if (result != EOK || device->descriptor.length < sizeof(device->descriptor)) goto remove_device;
    uint16_t language = 0x0409;
    uint8_t  language_descriptor[4];
    if (usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_STRING << 8, 0,
                        language_descriptor, sizeof(language_descriptor), USB_CTRL_TIMEOUT_MS)
            == EOK
        && language_descriptor[0] >= 4)
        language = language_descriptor[2] | (uint16_t)language_descriptor[3] << 8;
    (void)xhci_get_string(device, device->descriptor.manufacturer, language, device->manufacturer, sizeof(device->manufacturer));
    (void)xhci_get_string(device, device->descriptor.product, language, device->product, sizeof(device->product));
    (void)xhci_get_string(device, device->descriptor.serial_number, language, device->serial, sizeof(device->serial));

    usb_config_descriptor_t header;
    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, &header,
                             sizeof(header), USB_CTRL_TIMEOUT_MS);
    if (result != EOK || header.total_length < sizeof(header) || header.total_length > PAGE_4K_SIZE) goto remove_device;
    uint8_t *configuration = malloc(header.total_length);
    if (!configuration) {
        result = -ENOMEM;
        goto remove_device;
    }
    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0,
                             configuration, header.total_length, USB_CTRL_TIMEOUT_MS);
    device->dev.release = xhci_usb_device_release;
    if (result == EOK) result = usb_add_device(device, configuration, header.total_length);
    free(configuration);
    if (result == EOK) return EOK;

remove_device:
    usb_disconnect_device(device);
    for (size_t i = 0; i < device->interface_count; i++) {
        usb_interface_t *intf = &device->interfaces[i];
        if (intf->registered) {
            device_unregister(&intf->dev);
            intf->registered = false;
        }
    }
free_slot:
    controller->slots[slot_id] = NULL;
    controller->dcbaa[slot_id] = 0;
    if (slot) {
        for (size_t dci = 1; dci < XHCI_MAX_ENDPOINTS; dci++) {
            if (slot->endpoints[dci].ring_physical) xhci_dma_free(slot->endpoints[dci].ring_physical, 1);
        }
        if (slot->input_context_physical) xhci_dma_free(slot->input_context_physical, 1);
        if (slot->output_context_physical) xhci_dma_free(slot->output_context_physical, 1);
        free(slot);
    }
    (void)xhci_command(controller, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_DISABLE_SLOT) | ((uint32_t)slot_id << 24), NULL);
    return result;
}

static xhci_slot_t *xhci_slot_on_port(xhci_controller_t *controller, uint8_t port_id)
{
    for (uint16_t slot = 1; slot <= controller->max_slots; slot++)
        if (controller->slots[slot] && controller->slots[slot]->port_id == port_id) return controller->slots[slot];
    return NULL;
}

static void xhci_usb_device_release(struct device *dev)
{
    usb_device_t *usb  = container_of(dev, usb_device_t, dev);
    xhci_slot_t  *slot = container_of(usb, xhci_slot_t, usb);
    free(slot);
}

static void xhci_disconnect_port(xhci_controller_t *controller, uint8_t port_id)
{
    xhci_slot_t *slot = xhci_slot_on_port(controller, port_id);
    if (!slot) return;
    uint8_t       slot_id = slot->slot_id;
    usb_device_t *device  = &slot->usb;

    if (!device->connected) return;
    device->connected = false;

    for (size_t i = 0; i < device->interface_count; i++) {
        usb_interface_t *intf = &device->interfaces[i];
        if (intf->descriptor.interface_class == USB_CLASS_HID)
            usb_hid_disconnect(intf);
        else if (intf->descriptor.interface_class == USB_CLASS_MASS_STORAGE)
            usb_storage_disconnect(intf);
    }
    if (device->hcd_ops && device->hcd_ops->disable_device) device->hcd_ops->disable_device(device);

    for (size_t dci = 1; dci < XHCI_MAX_ENDPOINTS; dci++)
        xhci_dma_free(slot->endpoints[dci].ring_physical, slot->endpoints[dci].ring_physical ? 1 : 0);
    xhci_dma_free(slot->input_context_physical, 1);
    xhci_dma_free(slot->output_context_physical, 1);
    (void)xhci_command(controller, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_DISABLE_SLOT) | ((uint32_t)slot_id << 24), NULL);
    controller->slots[slot_id] = NULL;
    controller->dcbaa[slot_id] = 0;

    for (size_t i = 0; i < device->interface_count; i++) {
        usb_interface_t *intf = &device->interfaces[i];
        if (intf->registered) {
            device_unregister(&intf->dev);
            intf->registered = false;
        }
    }
    device->configured = false;
    device_unregister(&device->dev);
}

static void xhci_service_port(xhci_controller_t *controller, uint8_t port_id)
{
    size_t   offset = XHCI_OP_PORTS + (size_t)(port_id - 1) * XHCI_PORT_STRIDE;
    uint32_t status = xhci_read32(controller->operational, offset);
    xhci_write32(controller->operational, offset, (status & ~XHCI_PORT_CHANGE_BITS) | (status & XHCI_PORT_CHANGE_BITS));
    xhci_slot_t *slot = xhci_slot_on_port(controller, port_id);
    if (!(status & XHCI_PORT_CCS)) {
        if (slot) xhci_disconnect_port(controller, port_id);
    } else if ((status & XHCI_PORT_CSC) || !slot) {
        if (slot) xhci_disconnect_port(controller, port_id);
        msleep(100);
        (void)xhci_enumerate_port(controller, port_id);
    }
}

static void xhci_worker(void *argument)
{
    xhci_controller_t *controller = argument;
    while (!controller->stopping) {
        uint64_t flags            = spin_lock_irqsave(&controller->port_lock);
        uint64_t ports            = controller->pending_ports;
        controller->pending_ports = 0;
        if (!ports && !controller->stopping) wait_queue_prepare(&controller->worker_wait);
        spin_unlock_irqrestore(&controller->port_lock, flags);
        if (!ports && !controller->stopping) {
            wait_queue_sleep();
            continue;
        }
        for (uint8_t port = 1; port <= controller->max_ports; port++)
            if (ports & (1ULL << (port - 1))) xhci_service_port(controller, port);
    }
}

static void xhci_interrupt_slot(size_t index, void *frame)
{
    (void)frame;
    uint64_t           flags      = spin_lock_irqsave(&xhci_irq_lock);
    xhci_controller_t *controller = xhci_irq_slots[index];
    spin_unlock_irqrestore(&xhci_irq_lock, flags);
    if (controller && controller->running) {
        uint32_t status = xhci_read32(controller->operational, XHCI_OP_USBSTS);
        if (status & (XHCI_STS_EINT | XHCI_STS_PCD)) {
            xhci_write32(controller->operational, XHCI_OP_USBSTS, status & (XHCI_STS_EINT | XHCI_STS_PCD));
            xhci_process_events(controller);
        }
    }
    send_eoi();
}

#define XHCI_IRQ_WRAPPER(index)                     \
    static void xhci_interrupt_##index(void *frame) \
    {                                               \
        xhci_interrupt_slot(index, frame);          \
    }

XHCI_IRQ_WRAPPER(0)
XHCI_IRQ_WRAPPER(1)
XHCI_IRQ_WRAPPER(2)
XHCI_IRQ_WRAPPER(3)
XHCI_IRQ_WRAPPER(4)
XHCI_IRQ_WRAPPER(5)
XHCI_IRQ_WRAPPER(6)
XHCI_IRQ_WRAPPER(7)

typedef void (*xhci_irq_handler_t)(void *frame);
static const xhci_irq_handler_t xhci_irq_handlers[USB_MAX_CONTROLLERS] = {
    xhci_interrupt_0, xhci_interrupt_1, xhci_interrupt_2, xhci_interrupt_3,
    xhci_interrupt_4, xhci_interrupt_5, xhci_interrupt_6, xhci_interrupt_7,
};

static int xhci_take_ownership(xhci_controller_t *controller)
{
    uint32_t hccparams = xhci_read32(controller->capability, XHCI_CAP_HCCPARAMS1);
    size_t   offset    = (size_t)(hccparams >> 16) * 4U;
    for (unsigned int count = 0; offset && count < 64; count++) {
        uint32_t capability = xhci_read32(controller->capability, offset);
        uint8_t  id         = capability & 0xff;
        size_t   next       = (size_t)((capability >> 8) & 0xff) * 4U;
        if (id == 1) {
            xhci_write32((volatile uint8_t *)controller->capability, offset, capability | (1U << 24));
            int result = xhci_wait_register((volatile uint8_t *)controller->capability, offset, 1U << 16, 0, 1000);
            if (result != EOK) return result;
            uint32_t control = xhci_read32(controller->capability, offset + 4);
            xhci_write32((volatile uint8_t *)controller->capability, offset + 4, control & 0xffff0000U);
            return EOK;
        }
        offset = next ? offset + next : 0;
    }
    return EOK;
}

static void xhci_free_scratchpads(xhci_controller_t *controller)
{
    if (!controller) return;
    if (!controller->scratchpad_array) return;
    for (uint16_t i = 0; i < controller->scratchpad_count; i++) {
        if (controller->scratchpad_array[i]) xhci_dma_free(controller->scratchpad_array[i], 1);
    }
    size_t pages = ((size_t)controller->scratchpad_count * sizeof(uint64_t) + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    if (controller->scratchpad_array_physical) xhci_dma_free(controller->scratchpad_array_physical, pages);
    controller->scratchpad_array          = NULL;
    controller->scratchpad_array_physical = 0;
    controller->scratchpad_count          = 0;
}

static int xhci_allocate_scratchpads(xhci_controller_t *controller, uint32_t hcsparams2)
{
    controller->scratchpad_count = (uint16_t)(((hcsparams2 >> 27) & 0x1f) << 5) | ((hcsparams2 >> 21) & 0x1f);
    if (!controller->scratchpad_count) return EOK;
    size_t pointer_bytes = (size_t)controller->scratchpad_count * sizeof(uint64_t);
    size_t pointer_pages;
    controller->scratchpad_array = xhci_dma_alloc(pointer_bytes, &controller->scratchpad_array_physical, &pointer_pages);
    if (!controller->scratchpad_array) return -ENOMEM;
    for (uint16_t i = 0; i < controller->scratchpad_count; i++) {
        uint64_t physical;
        if (!xhci_dma_alloc(PAGE_4K_SIZE, &physical, NULL)) {
            xhci_free_scratchpads(controller);
            return -ENOMEM;
        }
        controller->scratchpad_array[i] = physical;
    }
    controller->dcbaa[0] = controller->scratchpad_array_physical;
    return EOK;
}

static int xhci_setup_interrupt(xhci_controller_t *controller)
{
    uint64_t flags = spin_lock_irqsave(&xhci_irq_lock);
    size_t   slot;
    for (slot = 0; slot < USB_MAX_CONTROLLERS; slot++)
        if (!xhci_irq_slots[slot]) break;
    if (slot == USB_MAX_CONTROLLERS) {
        spin_unlock_irqrestore(&xhci_irq_lock, flags);
        return -ENOSPC;
    }
    xhci_irq_slots[slot] = controller;
    controller->irq_slot = slot;
    spin_unlock_irqrestore(&xhci_irq_lock, flags);
    pci_msi_init(controller->pci);
    controller->vector = pci_enable_msi(controller->pci);
    if (controller->vector < 0) {
        if (pci_enable_msix(controller->pci, 1) == 1) {
            controller->vector       = pci_irq_vector(controller->pci, 0);
            controller->msix_enabled = true;
        }
    }
    if (controller->vector < 0) {
        flags                = spin_lock_irqsave(&xhci_irq_lock);
        xhci_irq_slots[slot] = NULL;
        spin_unlock_irqrestore(&xhci_irq_lock, flags);
        return -ENODEV;
    }
    register_interrupt_handler((uint16_t)controller->vector, (void *)xhci_irq_handlers[slot], 0, 0x8e);
    controller->interrupt_enabled = true;
    return EOK;
}

static void xhci_release_controller(xhci_controller_t *controller)
{
    if (!controller) return;
    if (controller->running) {
        controller->running = false;
        xhci_write32(controller->operational, XHCI_OP_USBCMD,
                     xhci_read32(controller->operational, XHCI_OP_USBCMD) & ~(XHCI_CMD_RUN | XHCI_CMD_INTE));
    }
    if (controller->interrupt_enabled) {
        uint64_t flags = spin_lock_irqsave(&xhci_irq_lock);
        if (controller->irq_slot < USB_MAX_CONTROLLERS && xhci_irq_slots[controller->irq_slot] == controller)
            xhci_irq_slots[controller->irq_slot] = NULL;
        spin_unlock_irqrestore(&xhci_irq_lock, flags);
        if (controller->msix_enabled)
            pci_disable_msix(controller->pci);
        else
            pci_disable_msi(controller->pci);
    }
    xhci_free_scratchpads(controller);
    if (controller->erst_physical) xhci_dma_free(controller->erst_physical, controller->erst ? 1 : 0);
    if (controller->event_ring_physical) xhci_dma_free(controller->event_ring_physical, controller->event_ring ? 1 : 0);
    if (controller->command_ring_physical) xhci_dma_free(controller->command_ring_physical, controller->command_ring.trbs ? 1 : 0);
    if (controller->dcbaa_physical) xhci_dma_free(controller->dcbaa_physical, controller->dcbaa ? 1 : 0);
    free(controller);
}

/* Probe a PCI xHCI controller: map BAR0, take ownership, init the rings */
static int xhci_probe(pci_device_cache_t *pci, uint8_t bus_number)
{
    int                     result = -EINVAL;
    base_address_register_t bar    = get_base_address_register(pci, 0);
    if (bar.type != mem_mapping || !bar.address) return -ENODEV;
    uint64_t bar_physical = (uint64_t)virt_to_phys((uint64_t)bar.address);
    uint64_t bar_size     = bar.size & ~BAR_64BIT_FLAG;
    if (!bar_size) bar_size = PAGE_4K_SIZE;
    uint64_t map_start  = ALIGN_DOWN(bar_physical, PAGE_4K_SIZE);
    uint64_t map_length = ALIGN_UP(bar_physical + bar_size, PAGE_4K_SIZE) - map_start;
    page_map_range_to(get_kernel_pagedir(), map_start, map_length, PTE_MMIO_FLAGS);
    xhci_controller_t *controller = calloc(1, sizeof(*controller));
    if (!controller) return -ENOMEM;
    controller->pci               = pci;
    controller->capability        = bar.address;
    controller->bus_number        = bus_number;
    controller->capability_length = *(volatile uint8_t *)controller->capability;
    controller->operational       = controller->capability + controller->capability_length;
    controller->runtime           = controller->capability + (xhci_read32(controller->capability, XHCI_CAP_RTSOFF) & ~0x1fU);
    controller->doorbells         = (volatile uint32_t *)(controller->capability + (xhci_read32(controller->capability, XHCI_CAP_DBOFF) & ~3U));
    uint32_t hcsparams1           = xhci_read32(controller->capability, XHCI_CAP_HCSPARAMS1);
    uint32_t hcsparams2           = xhci_read32(controller->capability, XHCI_CAP_HCSPARAMS2);
    uint32_t hccparams1           = xhci_read32(controller->capability, XHCI_CAP_HCCPARAMS1);
    controller->max_slots         = hcsparams1 & 0xff;
    controller->max_ports         = hcsparams1 >> 24;
    if (!controller->max_slots || !controller->max_ports || controller->max_ports > XHCI_MAX_ROOT_PORTS) goto invalid;
    controller->context_size = (hccparams1 & (1U << 2)) ? 64 : 32;
    wait_queue_init(&controller->worker_wait);

    uint32_t command = pci_read_command_status(pci) & 0xffff;
    pci_write_command_status(pci, command | 0x06);
    result = xhci_take_ownership(controller);
    if (result != EOK) goto invalid;
    xhci_write32(controller->operational, XHCI_OP_USBCMD, xhci_read32(controller->operational, XHCI_OP_USBCMD) & ~XHCI_CMD_RUN);
    result = xhci_wait_register(controller->operational, XHCI_OP_USBSTS, XHCI_STS_HALTED, XHCI_STS_HALTED, 1000);
    if (result != EOK) goto invalid;
    xhci_write32(controller->operational, XHCI_OP_USBCMD, XHCI_CMD_RESET);
    result = xhci_wait_register(controller->operational, XHCI_OP_USBCMD, XHCI_CMD_RESET, 0, 1000);
    if (result == EOK) result = xhci_wait_register(controller->operational, XHCI_OP_USBSTS, XHCI_STS_CNR, 0, 1000);
    if (result != EOK || !(xhci_read32(controller->operational, XHCI_OP_PAGESIZE) & 1U)) goto invalid;

    controller->dcbaa             = xhci_dma_alloc(PAGE_4K_SIZE, &controller->dcbaa_physical, NULL);
    controller->command_ring.trbs = xhci_dma_alloc(PAGE_4K_SIZE, &controller->command_ring_physical, NULL);
    controller->event_ring        = xhci_dma_alloc(PAGE_4K_SIZE, &controller->event_ring_physical, NULL);
    controller->erst              = xhci_dma_alloc(sizeof(*controller->erst), &controller->erst_physical, NULL);
    if (!controller->dcbaa || !controller->command_ring.trbs || !controller->event_ring || !controller->erst) {
        result = -ENOMEM;
        goto fail;
    }
    result = xhci_ring_init(&controller->command_ring, controller->command_ring.trbs, controller->command_ring_physical, XHCI_RING_TRBS, true);
    if (result != EOK) goto fail;
    controller->event_cycle   = 1;
    controller->erst->address = controller->event_ring_physical;
    controller->erst->size    = XHCI_EVENT_TRBS;
    result                    = xhci_allocate_scratchpads(controller, hcsparams2);
    if (result != EOK) goto fail;

    xhci_write64(controller->operational, XHCI_OP_DCBAAP, controller->dcbaa_physical);
    xhci_write64(controller->operational, XHCI_OP_CRCR, controller->command_ring_physical | 1U);
    xhci_write32(controller->operational, XHCI_OP_CONFIG, controller->max_slots);
    volatile uint8_t *interrupter = controller->runtime + XHCI_RT_INTERRUPTER0;
    xhci_write32(interrupter, XHCI_IR_ERSTSZ, 1);
    xhci_write64(interrupter, XHCI_IR_ERSTBA, controller->erst_physical);
    xhci_write64(interrupter, XHCI_IR_ERDP, controller->event_ring_physical);
    xhci_write32(interrupter, XHCI_IR_IMOD, 4000);
    xhci_write32(interrupter, XHCI_IR_IMAN, XHCI_IMAN_IE | XHCI_IMAN_IP);
    result = xhci_setup_interrupt(controller);
    if (result != EOK) goto fail;
    controller->running = true;
    xhci_write32(controller->operational, XHCI_OP_USBCMD, XHCI_CMD_RUN | XHCI_CMD_INTE | XHCI_CMD_HSEE);
    result = xhci_wait_register(controller->operational, XHCI_OP_USBSTS, XHCI_STS_HALTED, 0, 1000);
    if (result != EOK) goto fail;
    xhci_controllers[xhci_controller_count++] = controller;

    for (uint8_t port = 1; port <= controller->max_ports; port++) {
        size_t   offset = XHCI_OP_PORTS + (size_t)(port - 1) * XHCI_PORT_STRIDE;
        uint32_t status = xhci_read32(controller->operational, offset);
        xhci_write32(controller->operational, offset, status);
        if (status & XHCI_PORT_CCS) {
            int port_status = xhci_enumerate_port(controller, port);
            if (port_status != EOK) plogk("xhci: Port %u enumeration failed: %d\n", port, port_status);
        }
    }
    return EOK;

fail:
    xhci_release_controller(controller);
    return result;
invalid:
    free(controller);
    return result;
}

void xhci_init(void)
{
#if !CONFIG_USB_XHCI
    return;
#endif
    if (usb_core_init() != EOK) return;
    pci_devices_cache_t *cache = pci_get_devices_cache();
    if (!cache) return;
    for (pci_device_cache_t *pci = cache->head; pci && xhci_controller_count < USB_MAX_CONTROLLERS; pci = pci->next) {
        if (pci->class_code != XHCI_PCI_CLASS) continue;
        int bus_number = usb_host_allocate_bus_number();
        if (bus_number < 0) break;
        int status = xhci_probe(pci, (uint8_t)bus_number);
        if (status == EOK) {
            plogk("xhci: Controller %04x:%02x:%02x.%u registered as usb%u\n", pci->device->domain, pci->device->bus, pci->device->slot,
                  pci->device->func, bus_number);
        } else {
            plogk("xhci: Controller %04x:%02x:%02x.%u initialization failed: %d\n", pci->device->domain, pci->device->bus, pci->device->slot,
                  pci->device->func, status);
        }
    }
}

void xhci_start_workers(void)
{
#if !CONFIG_USB_XHCI
    return;
#endif
    for (size_t i = 0; i < xhci_controller_count; i++) {
        xhci_controller_t *controller = xhci_controllers[i];
        if (!controller || controller->worker_started) continue;
        controller->worker_task = kthread_create("xhci-hub", xhci_worker, controller);
        if (!controller->worker_task) continue;
        controller->worker_started = true;
        task_wakeup(controller->worker_task);
        if (controller->pending_ports) wait_queue_wake_one(&controller->worker_wait);
    }
}

void xhci_shutdown(void)
{
#if CONFIG_USB_XHCI
    for (size_t i = 0; i < xhci_controller_count; i++) {
        xhci_controller_t *controller = xhci_controllers[i];
        if (!controller) continue;
        controller->stopping = true;
        controller->running  = false;
        wait_queue_wake_all(&controller->worker_wait);
        xhci_write32(controller->operational, XHCI_OP_USBCMD,
                     xhci_read32(controller->operational, XHCI_OP_USBCMD) & ~(XHCI_CMD_RUN | XHCI_CMD_INTE));
        if (controller->msix_enabled)
            pci_disable_msix(controller->pci);
        else
            pci_disable_msi(controller->pci);
    }
#endif
}
