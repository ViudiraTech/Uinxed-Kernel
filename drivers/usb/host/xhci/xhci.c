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
#include <kernel/interrupt/interrupt.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <libs/util/byteorder.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <process/sched.h>

#if CONFIG_USB_XHCI && CONFIG_USB

#    define XHCI_PCI_CLASS 0x0c0330

#    define XHCI_CAP_HCSPARAMS1 0x04
#    define XHCI_CAP_HCSPARAMS2 0x08
#    define XHCI_CAP_HCCPARAMS1 0x10
#    define XHCI_CAP_DBOFF      0x14
#    define XHCI_CAP_RTSOFF     0x18

#    define XHCI_OP_USBCMD   0x00
#    define XHCI_OP_USBSTS   0x04
#    define XHCI_OP_PAGESIZE 0x08
#    define XHCI_OP_DNCTRL   0x14
#    define XHCI_OP_CRCR     0x18
#    define XHCI_OP_DCBAAP   0x30
#    define XHCI_OP_CONFIG   0x38
#    define XHCI_OP_PORTS    0x400
#    define XHCI_PORT_STRIDE 0x10

#    define XHCI_CMD_RUN    (1U << 0)
#    define XHCI_CMD_RESET  (1U << 1)
#    define XHCI_CMD_INTE   (1U << 2)
#    define XHCI_CMD_HSEE   (1U << 3)
#    define XHCI_STS_HALTED (1U << 0)
#    define XHCI_STS_FATAL  (1U << 2)
#    define XHCI_STS_EINT   (1U << 3)
#    define XHCI_STS_PCD    (1U << 4)
#    define XHCI_STS_CNR    (1U << 11)

#    define XHCI_PORT_CCS         (1U << 0)
#    define XHCI_PORT_PED         (1U << 1)
#    define XHCI_PORT_PR          (1U << 4)
#    define XHCI_PORT_PP          (1U << 9)
#    define XHCI_PORT_SPEED_SHIFT 10
#    define XHCI_PORT_SPEED_MASK  (0x0fU << XHCI_PORT_SPEED_SHIFT)
#    define XHCI_PORT_CSC         (1U << 17)
#    define XHCI_PORT_PEC         (1U << 18)
#    define XHCI_PORT_WRC         (1U << 19)
#    define XHCI_PORT_OCC         (1U << 20)
#    define XHCI_PORT_PRC         (1U << 21)
#    define XHCI_PORT_PLC         (1U << 22)
#    define XHCI_PORT_CEC         (1U << 23)
#    define XHCI_PORT_CHANGE_BITS (XHCI_PORT_CSC | XHCI_PORT_PEC | XHCI_PORT_WRC | XHCI_PORT_OCC | XHCI_PORT_PRC | XHCI_PORT_PLC | XHCI_PORT_CEC)
#    define XHCI_PORT_RWS_BITS    ((0x0fU << 5) | XHCI_PORT_PP | (3U << 14) | (7U << 25))

#    define XHCI_RT_INTERRUPTER0 0x20
#    define XHCI_IR_IMAN         0x00
#    define XHCI_IR_IMOD         0x04
#    define XHCI_IR_ERSTSZ       0x08
#    define XHCI_IR_ERSTBA       0x10
#    define XHCI_IR_ERDP         0x18
#    define XHCI_IMAN_IP         (1U << 0)
#    define XHCI_IMAN_IE         (1U << 1)
#    define XHCI_ERDP_EHB        (1U << 3)

#    define XHCI_CONTEXT_ENTRIES_SHIFT      27
#    define XHCI_SLOT_SPEED_SHIFT           20
#    define XHCI_SLOT_ROOT_PORT_SHIFT       16
#    define XHCI_ENDPOINT_TYPE_SHIFT        3
#    define XHCI_ENDPOINT_MAX_BURST_SHIFT   8
#    define XHCI_ENDPOINT_MAX_PACKET_SHIFT  16
#    define XHCI_ENDPOINT_MAX_PACKET_MASK   (0xffffU << XHCI_ENDPOINT_MAX_PACKET_SHIFT)
#    define XHCI_ENDPOINT_INTERVAL_SHIFT    16
#    define XHCI_ENDPOINT_ERROR_COUNT_SHIFT 1
#    define XHCI_ENDPOINT_STATE_MASK        0x7U

#    define XHCI_SLOT_HUB               (1U << 26)
#    define XHCI_SLOT_MTT               (1U << 25)
#    define XHCI_SLOT_ROUTE_STRING_MASK 0x000fffffU
#    define XHCI_SLOT_NUM_PORTS_SHIFT   24
#    define XHCI_TT_SLOT_SHIFT          0
#    define XHCI_TT_PORT_SHIFT          8
#    define XHCI_TT_THINK_SHIFT         16

#    define XHCI_COMPLETION_SUCCESS      1
#    define XHCI_COMPLETION_SHORT_PACKET 13
#    define XHCI_COMPLETION_STOPPED      26

#    define XHCI_RING_TRBS      (PAGE_4K_SIZE / sizeof(xhci_trb_t))
#    define XHCI_EVENT_TRBS     XHCI_RING_TRBS
#    define XHCI_MAX_ROOT_PORTS 64
#    define XHCI_MAX_SLOTS      255
#    define XHCI_MAX_ENDPOINTS  32

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
        uint64_t                 setup_trb_physical;
        uint64_t                 data_trb_physical;
        usb_interrupt_complete_t complete;
        void                    *context;
        int                      status;
        uint8_t                  completion_code;
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
        usb_host_t           host;
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
        uint64_t             failed_ports;
        wait_queue_t         worker_wait;
        task_t              *worker_task;
        spinlock_t           event_lock;
        spinlock_t           command_lock;
        spinlock_t           port_lock;
        bool                 running;
        bool                 interrupt_enabled;
        pci_irq_state_t      irq_state;
        bool                 worker_started;
        bool                 stopping;
} xhci_controller_t;

static xhci_controller_t *xhci_controllers[USB_MAX_CONTROLLERS];
static xhci_controller_t *xhci_irq_slots[USB_MAX_CONTROLLERS];
static size_t             xhci_controller_count;
static spinlock_t         xhci_irq_lock;

/* Xhci usb device release. */
static void xhci_usb_device_release(struct device *dev);

/*
 * MMIO and DMA helpers
 * The xHCI register space is memory-mapped; all access goes through
 * these helpers. DMA buffers are page-frame allocations translated
 * to virtual addresses for software use.
 */

/* Read a 32-bit MMIO register. */
static uint32_t xhci_read32(const volatile uint8_t *base, size_t offset)
{
    return mmio_read32(base + offset);
}

/* Read a 64-bit MMIO register as two 32-bit halves. */
static uint64_t xhci_read64(const volatile uint8_t *base, size_t offset)
{
    uint32_t low  = xhci_read32(base, offset);
    uint32_t high = xhci_read32(base, offset + 4);
    return (uint64_t)low | (uint64_t)high << 32;
}

/* Write a 32-bit MMIO register. */
static void xhci_write32(volatile uint8_t *base, size_t offset, uint32_t value)
{
    mmio_write32((base + offset), value);
}

/* Write a 64-bit MMIO register as two 32-bit halves. */
static void xhci_write64(volatile uint8_t *base, size_t offset, uint64_t value)
{
    xhci_write32(base, offset, (uint32_t)value);
    xhci_write32(base, offset + 4, (uint32_t)(value >> 32));
}

/* Allocate zeroed DMA memory and return its physical address. */
static void *xhci_dma_alloc(size_t size, uint64_t *physical, size_t *pages)
{
    if (size > SIZE_MAX - PAGE_4K_SIZE + 1) return NULL;
    size_t count = (size + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    if (count == 0) return NULL;
    uint64_t address = alloc_frames(count);
    if (!address) return NULL;
    void *memory = phys_to_virt(address);
    memset(memory, 0, count * PAGE_4K_SIZE);
    *physical = address;
    if (pages) *pages = count;
    return memory;
}

/* Release DMA memory allocated by xhci_dma_alloc(). */
static void xhci_dma_free(uint64_t physical, size_t pages)
{
    if (physical && pages) free_frames(physical, pages);
}

/* Poll an MMIO register until a mask matches or the timeout elapses */
static int xhci_wait_register(volatile uint8_t *base, size_t offset, uint32_t mask, uint32_t value, uint32_t timeout_ms)
{
    uint64_t deadline = nano_time() + ((uint64_t)timeout_ms * 1000000ULL);
    while ((xhci_read32(base, offset) & mask) != value) {
        if (nano_time() >= deadline) return -ETIMEDOUT;
        cpu_relax();
    }
    return EOK;
}

/* Pointer to a dword-aligned output context entry. */
static uint32_t *xhci_output_context(xhci_slot_t *slot, uint8_t dci)
{
    return (uint32_t *)((uint8_t *)slot->output_context + ((size_t)dci * slot->controller->context_size));
}

/* Pointer to an input context entry (offset by the control context). */
static uint32_t *xhci_input_context(xhci_slot_t *slot, uint8_t dci)
{
    return (uint32_t *)((uint8_t *)slot->input_context + ((size_t)(dci + 1) * slot->controller->context_size));
}

/* Map an endpoint address to its xHCI device-context index. */
static uint8_t xhci_endpoint_dci(const usb_endpoint_t *endpoint)
{
    uint8_t number = endpoint->descriptor.endpoint_address & USB_ENDPOINT_NUMBER_MASK;
    return (uint8_t)((number * 2) + !!(endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK));
}

/* Ring the doorbell to notify the controller of new ring work. */
static void xhci_ring_doorbell(xhci_controller_t *controller, uint8_t slot_id, uint8_t endpoint_id)
{
    dma_write_barrier();
    controller->doorbells[slot_id] = endpoint_id;
}

/* Translate an xHCI completion code into an errno. */
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

/* Enqueue one periodic transfer TRB and ring the endpoint doorbell. */
static int xhci_submit_periodic(xhci_transfer_t *transfer)
{
    uint64_t physical;
    if (!transfer || !transfer->active || !transfer->endpoint_state) return -ENODEV;
    if (!xhci_ring_enqueue(&transfer->endpoint_state->ring, transfer->dma_physical, (uint32_t)transfer->length, XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC, &physical)) return -EIO;
    transfer->trb_physical = physical;
    transfer->completed    = false;
    uint8_t dci            = xhci_endpoint_dci(transfer->endpoint);
    __atomic_store_n(&transfer->slot->pending[dci], transfer, __ATOMIC_RELEASE);
    xhci_ring_doorbell(transfer->slot->controller, transfer->slot->slot_id, dci);
    return EOK;
}

/*
 * Resolve a transfer event to its pending transfer and report it.
 * Periodic completions are not run here: they are appended to @deferred so
 * xhci_process_events can invoke them after releasing the event lock.
 */
static void xhci_handle_transfer_event(xhci_controller_t *controller, const xhci_trb_t *event, xhci_transfer_t **deferred, int *deferred_count)
{
    uint8_t slot_id = event->control >> 24;
    uint8_t dci     = (event->control >> 16) & 0x1f;
    if (!slot_id || slot_id > controller->max_slots || dci >= XHCI_MAX_ENDPOINTS) return;
    xhci_slot_t *slot = controller->slots[slot_id];
    if (!slot) return;
    xhci_transfer_t *transfer = __atomic_load_n(&slot->pending[dci], __ATOMIC_ACQUIRE);
    if (!transfer) return;
    uint8_t completion = event->status >> 24;
    if (transfer->trb_physical != event->parameter) {
        /* Control errors can point to Setup/Data; EOK must match the Status TRB. */
        bool control_stage = !transfer->endpoint && event->parameter && (transfer->setup_trb_physical == event->parameter || transfer->data_trb_physical == event->parameter);
        if (!control_stage || xhci_completion_status(completion) == EOK) return;
    }

    uint32_t residual         = event->status & 0x00ffffff;
    transfer->completion_code = completion;
    transfer->status          = xhci_completion_status(completion);
    transfer->actual          = residual <= transfer->length ? transfer->length - residual : 0;
    __atomic_store_n(&slot->pending[dci], NULL, __ATOMIC_RELEASE);
    if (transfer->periodic) {
        /*
         * Defer the completion callback and the resubmission until the event lock is
         * dropped: the callback can re-enter the HCD (a HID lock-key press
         * synchronously issues a SET_REPORT control transfer for the LED), which would
         * re-lock event_lock on the same CPU and self-deadlock.  The array is sized
         * XHCI_EVENT_TRBS, the maximum number of events one drain can observe.
         * xhci_interrupt_stop clears transfer->active under event_lock before freeing,
         * so the deferred pass observes a consistent active flag.
         */
        if (*deferred_count >= 0 && (unsigned)*deferred_count < XHCI_EVENT_TRBS) deferred[(*deferred_count)++] = transfer;
    } else {
        __atomic_store_n(&transfer->completed, true, __ATOMIC_RELEASE);
    }
}

/* Dispatch one event-ring entry by its TRB type. */
static void xhci_handle_event(xhci_controller_t *controller, const xhci_trb_t *event, xhci_transfer_t **deferred, int *deferred_count)
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
        xhci_handle_transfer_event(controller, event, deferred, deferred_count);
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

/* Drain the event ring and re-arm the interrupter.  Periodic completion callbacks run after the lock is dropped so they may re-enter the HCD. */
static void xhci_process_events(xhci_controller_t *controller)
{
    xhci_transfer_t *deferred[XHCI_EVENT_TRBS];
    int              deferred_count = 0;

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
        xhci_handle_event(controller, &event, deferred, &deferred_count);
    }
    uint64_t dequeue = controller->event_ring_physical + ((uint64_t)controller->event_dequeue * sizeof(xhci_trb_t));
    xhci_write64(controller->runtime + XHCI_RT_INTERRUPTER0, XHCI_IR_ERDP, dequeue | XHCI_ERDP_EHB);
    uint32_t iman = xhci_read32(controller->runtime + XHCI_RT_INTERRUPTER0, XHCI_IR_IMAN);
    xhci_write32(controller->runtime + XHCI_RT_INTERRUPTER0, XHCI_IR_IMAN, iman | XHCI_IMAN_IP | XHCI_IMAN_IE);
    spin_unlock_irqrestore(&controller->event_lock, flags);

    for (int i = 0; i < deferred_count; i++) {
        xhci_transfer_t *transfer = deferred[i];

        if (!transfer->active) continue;
        if (transfer->complete) transfer->complete(transfer->endpoint, transfer->dma_virtual, transfer->actual, transfer->status, transfer->context);
        if (transfer->active && xhci_submit_periodic(transfer) != EOK) transfer->active = false;
    }
}

/* Poll the event ring until a completion flag is set or a timeout hits. */
static int xhci_wait_flag(xhci_controller_t *controller, volatile bool *completed, uint32_t timeout_ms)
{
    uint64_t deadline = nano_time() + ((uint64_t)timeout_ms * 1000000ULL);
    while (!__atomic_load_n(completed, __ATOMIC_ACQUIRE)) {
        xhci_process_events(controller);
        if (xhci_read32(controller->operational, XHCI_OP_USBSTS) & XHCI_STS_FATAL) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("usb-xhci: Host system error on bus %u\n", controller->bus_number);
            return -EIO;
        }
        if (nano_time() >= deadline) return -ETIMEDOUT;
        cpu_relax();
    }
    return EOK;
}

/* Submit one command TRB and wait for its completion event. */
static int xhci_command(xhci_controller_t *controller, uint64_t parameter, uint32_t status, uint32_t control, uint8_t *slot_id)
{
    xhci_command_wait_t wait = {0};
    spin_lock(&controller->command_lock);
    if (!xhci_ring_enqueue(&controller->command_ring, parameter, status, control, &wait.trb_physical)) {
        plogk("usb-xhci: Command ring unusable on bus %u\n", controller->bus_number);
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
    if (result != EOK)
        plogk("usb-xhci: Command %u failed on bus %u slot %u (%d, completion code %u)\n", (control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT, controller->bus_number, wait.slot_id, result,
              wait.completion_code);
    return result;
}

/* Wait for a transfer to complete, stopping the endpoint on timeout. */
static int xhci_wait_transfer(xhci_transfer_t *transfer, uint32_t timeout_ms)
{
    int result = xhci_wait_flag(transfer->slot->controller, &transfer->completed, timeout_ms);
    if (result != EOK) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("usb-xhci: Transfer timed out on bus %u slot %u\n", transfer->slot->controller->bus_number, transfer->slot->slot_id);
        uint8_t dci = transfer->endpoint ? xhci_endpoint_dci(transfer->endpoint) : 1;
        if (__atomic_load_n(&transfer->slot->pending[dci], __ATOMIC_ACQUIRE) == transfer) __atomic_store_n(&transfer->slot->pending[dci], NULL, __ATOMIC_RELEASE);
        (void)xhci_command(transfer->slot->controller, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_STOP_ENDPOINT) | ((uint32_t)dci << 16) | ((uint32_t)transfer->slot->slot_id << 24), NULL);
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

/* Submit a control transfer as SETUP/DATA/STATUS TRBs. */
static int xhci_control(usb_device_t *device, const usb_setup_packet_t *setup, void *buffer, size_t length, uint32_t timeout_ms)
{
    xhci_slot_t *slot = device ? device->hc_private : NULL;
    if (!slot || !setup || length > PAGE_4K_SIZE) return -EINVAL;
    xhci_endpoint_state_t *endpoint = &slot->endpoints[1];
    xhci_transfer_t        transfer = {.slot = slot, .endpoint_state = endpoint, .length = length, .active = true};
    if (length) {
        transfer.dma_virtual = xhci_dma_alloc(length, &transfer.dma_physical, &transfer.dma_pages);
        if (!transfer.dma_virtual) {
            plogk("usb-xhci: Control transfer DMA allocation failed on bus %u (%zu bytes)\n", slot->controller->bus_number, length);
            return -ENOMEM;
        }
        if (!(setup->request_type & USB_DIR_IN)) memcpy(transfer.dma_virtual, buffer, length);
    }

    uint16_t saved_enqueue = endpoint->ring.enqueue;
    uint8_t  saved_cycle   = endpoint->ring.cycle;
    uint64_t setup_data    = 0;
    memcpy(&setup_data, setup, sizeof(*setup));
    uint32_t transfer_type = 0U;
    if (length) transfer_type = (setup->request_type & USB_DIR_IN) ? 3U : 2U;
    if (!xhci_ring_enqueue(&endpoint->ring, setup_data, 8, XHCI_TRB_TYPE(XHCI_TRB_SETUP_STAGE) | XHCI_TRB_IDT | (transfer_type << 16), &transfer.setup_trb_physical)) goto io_error;
    if (length
        && !xhci_ring_enqueue(&endpoint->ring, transfer.dma_physical, (uint32_t)length, XHCI_TRB_TYPE(XHCI_TRB_DATA_STAGE) | ((setup->request_type & USB_DIR_IN) ? XHCI_TRB_DIR_IN : 0),
                              &transfer.data_trb_physical))
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
        plogk("usb-xhci: Bulk transfer DMA allocation failed on bus %u (%zu bytes)\n", slot->controller->bus_number, length);
        return -ENOMEM;
    }
    bool input = (usb_endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK) != 0;
    if (!input) memcpy(transfer.dma_virtual, buffer, length);
    if (!xhci_ring_enqueue(&endpoint->ring, transfer.dma_physical, (uint32_t)length, XHCI_TRB_TYPE(XHCI_TRB_NORMAL) | XHCI_TRB_IOC, &transfer.trb_physical)) {
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

/* Register a periodic interrupt-IN transfer and submit its first TRB. */
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
        plogk("usb-xhci: Interrupt transfer DMA allocation failed on bus %u (%zu bytes)\n", transfer->slot->controller->bus_number, length);
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

/* Stop and free a periodic transfer. */
static void xhci_interrupt_stop(usb_endpoint_t *usb_endpoint)
{
    xhci_endpoint_state_t *endpoint = usb_endpoint ? usb_endpoint->hc_private : NULL;
    xhci_transfer_t       *transfer = endpoint ? endpoint->periodic : NULL;
    if (!transfer) return;
    uint8_t dci = xhci_endpoint_dci(usb_endpoint);

    /*
     * STOP_ENDPOINT drains the event ring, running any pending periodic callback
     * before xhci_command returns and therefore ahead of the free below.  Clear
     * transfer->active under event_lock so a concurrent drain on another CPU sees
     * the flag consistently with that free.
     */
    (void)xhci_command(transfer->slot->controller, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_STOP_ENDPOINT) | ((uint32_t)dci << 16) | ((uint32_t)transfer->slot->slot_id << 24), NULL);
    uint64_t flags   = spin_lock_irqsave(&transfer->slot->controller->event_lock);
    transfer->active = false;
    if (__atomic_load_n(&transfer->slot->pending[dci], __ATOMIC_ACQUIRE) == transfer) __atomic_store_n(&transfer->slot->pending[dci], NULL, __ATOMIC_RELEASE);
    endpoint->periodic = NULL;
    spin_unlock_irqrestore(&transfer->slot->controller->event_lock, flags);
    xhci_dma_free(transfer->dma_physical, transfer->dma_pages);
    free(transfer);
}

/* Map a USB transfer type to the xHCI endpoint-context type. */
static uint8_t xhci_endpoint_type(const usb_endpoint_t *endpoint)
{
    uint8_t transfer = endpoint->descriptor.attributes & USB_ENDPOINT_XFERTYPE_MASK;
    bool    input    = (endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK) != 0;
    if (transfer == USB_ENDPOINT_XFER_ISOC) return input ? 5 : 1;
    if (transfer == USB_ENDPOINT_XFER_BULK) return input ? 6 : 2;
    if (transfer == USB_ENDPOINT_XFER_INT) return input ? 7 : 3;
    return 4;
}

/* Convert bInterval to xHCI Interval per xHCI 1.2 §6.2.3.6 and USB 2.0 §9.6.6. */
static uint8_t xhci_endpoint_interval(const usb_endpoint_t *endpoint)
{
    uint8_t     bInterval = endpoint->descriptor.interval;
    usb_speed_t speed     = endpoint->interface->device->speed;
    uint8_t     type      = endpoint->descriptor.attributes & USB_ENDPOINT_XFERTYPE_MASK;
    if (!bInterval) return 0;
    if (speed >= USB_SPEED_HIGH) {
        if (type == USB_ENDPOINT_XFER_INT || type == USB_ENDPOINT_XFER_ISOC) {
            if (bInterval < 1) bInterval = 1;
            if (bInterval > 16) bInterval = 16;
            return bInterval - 1;
        }
        unsigned int v = bInterval, fls = 0;
        while (v) {
            v >>= 1;
            fls++;
        }
        unsigned int iv = fls ? fls - 1 : 0;
        return iv > 15 ? 15 : (uint8_t)iv;
    }
    if (speed == USB_SPEED_FULL && type == USB_ENDPOINT_XFER_ISOC) {
        if (bInterval < 1) bInterval = 1;
        if (bInterval > 16) bInterval = 16;
        unsigned int iv = (bInterval - 1) + 3;
        return iv > 15 ? 15 : (uint8_t)iv;
    }
    {
        unsigned int frames = (unsigned int)bInterval * 8;
        unsigned int v = frames, fls = 0;
        while (v) {
            v >>= 1;
            fls++;
        }
        unsigned int iv = fls ? fls - 1 : 0;
        if (iv < 3) iv = 3;
        if (iv > 10) iv = 10;
        return (uint8_t)iv;
    }
}

/* Allocate an endpoint ring and configure the endpoint context. */
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
    context[0]       = (uint32_t)xhci_endpoint_interval(usb_endpoint) << XHCI_ENDPOINT_INTERVAL_SHIFT;
    context[1]       = (3U << XHCI_ENDPOINT_ERROR_COUNT_SHIFT) | ((uint32_t)xhci_endpoint_type(usb_endpoint) << XHCI_ENDPOINT_TYPE_SHIFT) | ((uint32_t)max_packet << XHCI_ENDPOINT_MAX_PACKET_SHIFT);
    uint64_t dequeue = endpoint->ring_physical | 1U;
    context[2]       = (uint32_t)dequeue;
    context[3]       = (uint32_t)(dequeue >> 32);
    context[4]       = max_packet | ((uint32_t)max_packet << 16);
    dma_write_barrier();
    result = xhci_command(slot->controller, slot->input_context_physical, 0, XHCI_TRB_TYPE(XHCI_TRB_CONFIGURE_ENDPOINT) | ((uint32_t)slot->slot_id << 24), NULL);
    if (result != EOK) goto fail;
    usb_endpoint->hc_private = endpoint;
    return EOK;
fail:
    xhci_dma_free(endpoint->ring_physical, 1);
    memset(endpoint, 0, sizeof(*endpoint));
    return result;
}

/* Stop and free an endpoint's periodic transfer. */
static void xhci_disable_endpoint(usb_endpoint_t *usb_endpoint)
{
    xhci_endpoint_state_t *endpoint = usb_endpoint ? usb_endpoint->hc_private : NULL;
    if (!endpoint) return;
    xhci_interrupt_stop(usb_endpoint);
}

/* Reset an endpoint and set its dequeue pointer after a stall. */
static int xhci_clear_halt(usb_endpoint_t *usb_endpoint)
{
    if (!usb_endpoint || !usb_endpoint->hc_private) return -EINVAL;
    xhci_slot_t           *slot     = usb_endpoint->interface->device->hc_private;
    xhci_endpoint_state_t *endpoint = usb_endpoint->hc_private;
    uint8_t                dci      = xhci_endpoint_dci(usb_endpoint);
    int                    status   = xhci_command(slot->controller, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_RESET_ENDPOINT) | ((uint32_t)dci << 16) | ((uint32_t)slot->slot_id << 24), NULL);
    if (status != EOK) return status;
    uint64_t dequeue = endpoint->ring_physical + ((uint64_t)endpoint->ring.enqueue * sizeof(xhci_trb_t));
    dequeue |= endpoint->ring.cycle;
    return xhci_command(slot->controller, dequeue, 0, XHCI_TRB_TYPE(XHCI_TRB_SET_TR_DEQUEUE) | ((uint32_t)dci << 16) | ((uint32_t)slot->slot_id << 24), NULL);
}

/* Stop every interrupt transfer of a device before it goes away. */
static void xhci_disable_device(usb_device_t *device)
{
    if (!device) return;
    for (size_t i = 0; i < device->interface_count; i++)
        for (size_t j = 0; j < device->interfaces[i].endpoint_count; j++) xhci_interrupt_stop(&device->interfaces[i].endpoints[j]);
}

/* Xhci enumerate device. */
static int xhci_enumerate_device(usb_device_t *hub, uint8_t port, usb_device_t **out);

static const usb_hcd_ops_t xhci_hcd_ops = {
    .control            = xhci_control,
    .transfer           = xhci_transfer,
    .interrupt_start    = xhci_interrupt_start,
    .interrupt_stop     = xhci_interrupt_stop,
    .configure_endpoint = xhci_configure_endpoint,
    .disable_endpoint   = xhci_disable_endpoint,
    .clear_halt         = xhci_clear_halt,
    .disable_device     = xhci_disable_device,
    .enumerate          = xhci_enumerate_device,
};

#    define XHCI_IRQ_WRAPPER(index)                                                  \
        INTERRUPT_BEGIN static void xhci_interrupt_##index(interrupt_frame_t *frame) \
        {                                                                            \
            irq_enter_gs(frame);                                                     \
            xhci_interrupt_slot(index, frame);                                       \
            irq_leave_gs(frame);                                                     \
        }                                                                            \
        INTERRUPT_END

/* Preserve ordinary RW fields; PED is RW1CS, and PR/LWS/WPR are write triggers. */
static uint32_t xhci_port_neutral(uint32_t status)
{
    return status & XHCI_PORT_RWS_BITS;
}

/* Perform the xHCI port reset sequence. */
static int xhci_port_reset(xhci_controller_t *controller, uint8_t port_id)
{
    size_t   offset = XHCI_OP_PORTS + ((size_t)(port_id - 1) * XHCI_PORT_STRIDE);
    uint32_t status = xhci_read32(controller->operational, offset);
    if (!(status & XHCI_PORT_CCS)) return -ENODEV;
    if (!(status & XHCI_PORT_PED)) {
        uint32_t value = xhci_port_neutral(status);
        xhci_write32(controller->operational, offset, value | XHCI_PORT_PP | XHCI_PORT_PR);
        int result = xhci_wait_register(controller->operational, offset, XHCI_PORT_PR, 0, 1000);
        if (result != EOK) return result;
        status = xhci_read32(controller->operational, offset);
    }
    xhci_write32(controller->operational, offset, xhci_port_neutral(status) | (status & XHCI_PORT_CHANGE_BITS));
    return (status & XHCI_PORT_CCS) && (status & XHCI_PORT_PED) ? EOK : -ENODEV;
}

/* Map the port speed code to a USB speed. */
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

/* USB core and xHCI use opposite Low/Full-speed encodings. */
static uint32_t xhci_slot_speed(usb_speed_t speed)
{
    switch (speed) {
        case USB_SPEED_FULL :
            return 1;
        case USB_SPEED_LOW :
            return 2;
        case USB_SPEED_HIGH :
            return 3;
        case USB_SPEED_SUPER :
            return 4;
        case USB_SPEED_SUPER_PLUS :
            return 5;
        default :
            return 0;
    }
}

/* Default endpoint-0 max packet size for a given speed. */
static uint16_t xhci_ep0_packet_size(usb_speed_t speed)
{
    if (speed == USB_SPEED_SUPER || speed == USB_SPEED_SUPER_PLUS) return 512;
    if (speed == USB_SPEED_HIGH) return 64;
    return 8;
}

/* Update a hub slot's Hub + Number of Ports after its hub descriptor is known. Called from the hub probe path after GET_DESCRIPTOR(HUB) succeeds. */
static int xhci_update_hub_slot(usb_device_t *hub, uint8_t port_count)
{
    xhci_slot_t *slot = hub ? hub->hc_private : NULL;
    if (!slot) return -EINVAL;
    memset(slot->input_context, 0, PAGE_4K_SIZE);
    uint32_t *ctrl = slot->input_context;
    ctrl[0]        = 0;
    ctrl[1]        = (1U << 0); // Slot context only
    memcpy(xhci_input_context(slot, 0), xhci_output_context(slot, 0), slot->controller->context_size);
    uint32_t *slot_ctx = xhci_input_context(slot, 0);
    slot_ctx[0] |= XHCI_SLOT_HUB;
    slot_ctx[1] &= ~(0xffU << XHCI_SLOT_NUM_PORTS_SHIFT);
    slot_ctx[1] |= (uint32_t)port_count << XHCI_SLOT_NUM_PORTS_SHIFT;

    /* Route string is already correct (0 for root-port hub); Hub bit distinguishes it. */
    dma_write_barrier();
    return xhci_command(slot->controller, slot->input_context_physical, 0, XHCI_TRB_TYPE(XHCI_TRB_EVALUATE_CONTEXT) | ((uint32_t)slot->slot_id << 24), NULL);
}

/* Configure slot input context for a downstream device (route string, speed, TT info per xHCI §4.6). */
static int xhci_address_slot_tt(xhci_slot_t *slot, usb_speed_t speed, uint32_t route, uint8_t root_port, bool is_hub, uint8_t num_ports, uint8_t tt_slot, uint8_t tt_port)
{
    memset(slot->input_context, 0, PAGE_4K_SIZE);
    uint32_t *ctrl     = slot->input_context;
    ctrl[1]            = 3; // slot + ep0
    uint32_t *slot_ctx = xhci_input_context(slot, 0);
    slot_ctx[0]        = (route & XHCI_SLOT_ROUTE_STRING_MASK) | (xhci_slot_speed(speed) << XHCI_SLOT_SPEED_SHIFT) | (1U << XHCI_CONTEXT_ENTRIES_SHIFT);
    if (is_hub) slot_ctx[0] |= XHCI_SLOT_HUB;

    /* For downstream devices, MTT is 0 (single-TT). Could be 1 if hub is multi-TT. */
    slot_ctx[1] = (uint32_t)root_port << XHCI_SLOT_ROOT_PORT_SHIFT;
    if (is_hub) slot_ctx[1] |= (uint32_t)num_ports << XHCI_SLOT_NUM_PORTS_SHIFT;
    uint32_t *tt = xhci_input_context(slot, 0) + 2; // actually offset 0x08, but slot_context[2] is tt_info

    /* tt_info at slot_context[2] (third dword): TT Hub Slot ID [7:0], TT Port Number [15:8] */
    if (tt_slot || tt_port) *tt = (uint32_t)tt_slot << XHCI_TT_SLOT_SHIFT | (uint32_t)tt_port << XHCI_TT_PORT_SHIFT;
    uint32_t *ep0  = xhci_input_context(slot, 1);
    uint16_t  maxp = xhci_ep0_packet_size(speed);
    ep0[1]         = (3U << XHCI_ENDPOINT_ERROR_COUNT_SHIFT) | (4U << XHCI_ENDPOINT_TYPE_SHIFT) | ((uint32_t)maxp << XHCI_ENDPOINT_MAX_PACKET_SHIFT);
    uint64_t dq    = slot->endpoints[1].ring_physical | 1U;
    ep0[2]         = (uint32_t)dq;
    ep0[3]         = (uint32_t)(dq >> 32);
    ep0[4]         = 8;
    dma_write_barrier();
    return xhci_command(slot->controller, slot->input_context_physical, 0, XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE) | ((uint32_t)slot->slot_id << 24), NULL);
}

/* Allocate a slot's contexts and endpoint-0 ring. */
static int xhci_allocate_slot(xhci_controller_t *controller, uint8_t port_id, uint8_t slot_id, xhci_slot_t **result)
{
    xhci_slot_t *slot = calloc(1, sizeof(*slot));
    if (!slot) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("usb-xhci: Slot allocation failed on bus %u\n", controller->bus_number);
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
    if (!slot->output_context || !slot->input_context || !ep0->ring.trbs || xhci_ring_init(&ep0->ring, ep0->ring.trbs, ep0->ring_physical, XHCI_RING_TRBS, true) != EOK) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("usb-xhci: Slot %u context/ring allocation failed on bus %u\n", slot_id, controller->bus_number);
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

/* Program the input context and issue ADDRESS DEVICE. */
static int xhci_address_slot(xhci_slot_t *slot, usb_speed_t speed)
{
    memset(slot->input_context, 0, PAGE_4K_SIZE);
    uint32_t *control      = slot->input_context;
    control[1]             = 3;
    uint32_t *slot_context = xhci_input_context(slot, 0);
    slot_context[0]        = (xhci_slot_speed(speed) << XHCI_SLOT_SPEED_SHIFT) | (1U << XHCI_CONTEXT_ENTRIES_SHIFT);
    slot_context[1]        = (uint32_t)slot->port_id << XHCI_SLOT_ROOT_PORT_SHIFT;
    uint32_t *ep0_context  = xhci_input_context(slot, 1);
    uint16_t  max_packet   = xhci_ep0_packet_size(speed);
    ep0_context[1]         = (3U << XHCI_ENDPOINT_ERROR_COUNT_SHIFT) | (4U << XHCI_ENDPOINT_TYPE_SHIFT) | ((uint32_t)max_packet << XHCI_ENDPOINT_MAX_PACKET_SHIFT);
    uint64_t dequeue       = slot->endpoints[1].ring_physical | 1U;
    ep0_context[2]         = (uint32_t)dequeue;
    ep0_context[3]         = (uint32_t)(dequeue >> 32);
    ep0_context[4]         = 8;
    dma_write_barrier();
    return xhci_command(slot->controller, slot->input_context_physical, 0, XHCI_TRB_TYPE(XHCI_TRB_ADDRESS_DEVICE) | ((uint32_t)slot->slot_id << 24), NULL);
}

/* Learn Low/Full-speed EP0's packet size before requesting the full descriptor. */
static int xhci_read_device_descriptor(usb_device_t *device)
{
    xhci_slot_t *slot       = device->hc_private;
    uint16_t     max_packet = xhci_ep0_packet_size(device->speed);
    memset(&device->descriptor, 0, sizeof(device->descriptor));
    if (device->speed == USB_SPEED_LOW || device->speed == USB_SPEED_FULL) {
        int result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, &device->descriptor, 8, USB_CTRL_TIMEOUT_MS);
        if (result != EOK) return result;
        if (device->descriptor.length < sizeof(device->descriptor) || device->descriptor.descriptor_type != USB_DT_DEVICE) return -EIO;
        max_packet = device->descriptor.max_packet_size0;
        if ((device->speed == USB_SPEED_LOW && max_packet != 8) || (max_packet != 8 && max_packet != 16 && max_packet != 32 && max_packet != 64)) return -EINVAL;

        dma_read_barrier();
        uint32_t *output_ep0 = xhci_output_context(slot, 1);
        if ((output_ep0[1] >> XHCI_ENDPOINT_MAX_PACKET_SHIFT) != max_packet) {
            memset(slot->input_context, 0, PAGE_4K_SIZE);
            slot->input_context[1] = 1U << 1; /* Add EP0 only; no dropped contexts. */
            uint32_t *input_ep0    = xhci_input_context(slot, 1);
            memcpy(input_ep0, output_ep0, slot->controller->context_size);
            input_ep0[0] &= ~XHCI_ENDPOINT_STATE_MASK; // EP State must be 0 in an input context.
            input_ep0[1] = (input_ep0[1] & ~XHCI_ENDPOINT_MAX_PACKET_MASK) | ((uint32_t)max_packet << XHCI_ENDPOINT_MAX_PACKET_SHIFT);
            dma_write_barrier();
            result = xhci_command(slot->controller, slot->input_context_physical, 0, XHCI_TRB_TYPE(XHCI_TRB_EVALUATE_CONTEXT) | ((uint32_t)slot->slot_id << 24), NULL);
            if (result != EOK) return result;
        }
    }

    int result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, &device->descriptor, sizeof(device->descriptor),
                                 USB_CTRL_TIMEOUT_MS);
    if (result != EOK) return result;
    if (device->descriptor.length < sizeof(device->descriptor) || device->descriptor.descriptor_type != USB_DT_DEVICE) return -EIO;
    uint16_t expected = device->speed >= USB_SPEED_SUPER ? 9 : max_packet;
    return device->descriptor.max_packet_size0 == expected ? EOK : -EINVAL;
}

/* Fetch and convert a device string descriptor to ASCII. */
static int xhci_get_string(usb_device_t *device, uint8_t index, uint16_t language, char *output, size_t capacity)
{
    uint8_t descriptor[128];
    if (!index || !output || capacity < 2) return -EINVAL;
    int result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, (USB_DT_STRING << 8) | index, language, descriptor, sizeof(descriptor),
                                 USB_CTRL_TIMEOUT_MS);
    if (result != EOK || descriptor[0] < 2 || descriptor[1] != USB_DT_STRING) return -EIO;
    size_t characters = (descriptor[0] - 2) / 2;
    if (characters >= capacity) characters = capacity - 1;
    for (size_t i = 0; i < characters; i++) {
        uint16_t character = descriptor[2 + (i * 2)] | (uint16_t)descriptor[3 + (i * 2)] << 8;
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

/* Enumerate a device attached to a root port. */
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
    size_t      port_offset = XHCI_OP_PORTS + ((size_t)(port_id - 1) * XHCI_PORT_STRIDE);
    usb_speed_t speed       = xhci_usb_speed(xhci_read32(controller->operational, port_offset));
    result                  = xhci_address_slot(slot, speed);
    if (result != EOK) goto free_slot;

    usb_device_t *device  = &slot->usb;
    device->connected     = true;
    device->speed         = speed;
    device->bus_number    = controller->bus_number;
    device->port_number   = port_id;
    device->depth         = 1; // Root-port device: depth 1 (root hub depth 0)
    device->hcd_ops       = &xhci_hcd_ops;
    device->hc_private    = slot;
    uint32_t *output_slot = xhci_output_context(slot, 0);
    device->address       = output_slot[3] & 0xff;
    if (!device->address) device->address = slot_id;
    (void)snprintf(device->path, sizeof(device->path), "%u-%u", device->bus_number, port_id);

    result = xhci_read_device_descriptor(device);
    if (result != EOK) goto remove_device;

    /* If the new device is a hub (device class 0x09), update its slot with Hub + NumPorts via EVALUATE_CONTEXT. */
    if (device->descriptor.device_class == USB_CLASS_HUB) {
        uint8_t hub_desc[16];
        if (usb_control_msg(device, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_HUB << 8, 0, hub_desc, 9, USB_CTRL_TIMEOUT_MS) == EOK) {
            uint8_t nports = hub_desc[2];
            if (nports && nports <= XHCI_MAX_ROOT_PORTS) {
                xhci_update_hub_slot(device, nports);
            } else if (nports == 0) {
                xhci_update_hub_slot(device, 4); // QEMU hub default
            }
        }
    }
    uint16_t language = 0x0409;
    uint8_t  language_descriptor[4];
    if (usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_STRING << 8, 0, language_descriptor, sizeof(language_descriptor), USB_CTRL_TIMEOUT_MS)
            == EOK
        && language_descriptor[0] >= 4) {
        language = load_le16(&language_descriptor[2]);
    }
    (void)xhci_get_string(device, device->descriptor.manufacturer, language, device->manufacturer, sizeof(device->manufacturer));
    (void)xhci_get_string(device, device->descriptor.product, language, device->product, sizeof(device->product));
    (void)xhci_get_string(device, device->descriptor.serial_number, language, device->serial, sizeof(device->serial));

    usb_config_descriptor_t header;
    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, &header, sizeof(header), USB_CTRL_TIMEOUT_MS);
    if (result != EOK || header.total_length < sizeof(header) || header.total_length > PAGE_4K_SIZE) goto remove_device;
    uint8_t *configuration = malloc(header.total_length);
    if (!configuration) {
        result = -ENOMEM;
        goto remove_device;
    }
    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, configuration, header.total_length, USB_CTRL_TIMEOUT_MS);
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
        for (size_t dci = 1; dci < XHCI_MAX_ENDPOINTS; dci++)
            if (slot->endpoints[dci].ring_physical) xhci_dma_free(slot->endpoints[dci].ring_physical, 1);
        if (slot->input_context_physical) xhci_dma_free(slot->input_context_physical, 1);
        if (slot->output_context_physical) xhci_dma_free(slot->output_context_physical, 1);
        free(slot);
    }
    (void)xhci_command(controller, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_DISABLE_SLOT) | ((uint32_t)slot_id << 24), NULL);
    return result;
}

/*
 * Enumerate a device on a hub downstream port (industrial-grade, USB 2.0/3.x).
 * The hub port has already been reset (SET_FEATURE PORT_RESET) by the hub
 * driver; this routine handles slot allocation with route-string routing per
 * xHCI §4.6 / §6.2.2, TT for low/full behind high-speed hub, and hub slot
 * update (Hub + Number of Ports) via EVALUATE_CONTEXT when the new device
 * is itself a hub.
 */
static int xhci_enumerate_device(usb_device_t *hub, uint8_t port, usb_device_t **out)
{
    if (!hub || !port || !out) return -EINVAL;
    xhci_slot_t       *hub_slot = hub->hc_private;
    xhci_controller_t *ctrl     = hub_slot ? hub_slot->controller : NULL;
    if (!hub_slot || !ctrl) return -ENODEV;

    /* Query hub port status for speed (hub already did reset, but re-read): use hub class GET_STATUS on the hub. */
    uint8_t status_buf[4];
    int     ret = usb_control_msg(hub, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_OTHER, USB_REQ_GET_STATUS, 0, port, status_buf, 4, USB_CTRL_TIMEOUT_MS);
    if (ret != EOK) return ret;
    uint16_t    port_status = load_le16(status_buf);
    usb_speed_t speed;
    if (port_status & 0x0400) {
        speed = USB_SPEED_HIGH;
    } else if (port_status & 0x0200) {
        speed = USB_SPEED_LOW;
    } else if (port_status & 0x0001) {
        speed = USB_SPEED_FULL; // CONNECTION implies full if no high/low
    } else {
        return -ENODEV;
    }

    /*
     * SuperSpeed ports would have additional PORT_LINK_STATE etc., treat as Super.
     * SuperSpeed hub downstream uses the same logic; the hub descriptor tells the port count.
     */

    uint8_t slot_id = 0;
    ret             = xhci_command(ctrl, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_ENABLE_SLOT), &slot_id);
    if (ret != EOK) return ret;
    if (!slot_id || slot_id > ctrl->max_slots) {
        xhci_command(ctrl, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_DISABLE_SLOT) | ((uint32_t)slot_id << 24), NULL);
        return -EIO;
    }
    xhci_slot_t *slot = NULL;
    ret               = xhci_allocate_slot(ctrl, hub_slot->port_id, slot_id, &slot);
    if (ret != EOK) goto free_slot;

    /* Build route string: hub's route + downstream port in next nibble. Per xHCI §4.6, each 4-bit nibble is a port. */
    uint32_t *hub_out_slot = xhci_output_context(hub_slot, 0);
    uint32_t  hub_route    = hub_out_slot[0] & XHCI_SLOT_ROUTE_STRING_MASK;
    uint32_t  route        = hub_route | ((uint32_t)port << ((hub->depth > 0 ? hub->depth - 1 : 0) * 4));

    /* Root hub port is the hub's root port (where the hub chain attaches to the root). */
    uint32_t *hub_out_slot2 = xhci_output_context(hub_slot, 0);
    (void)hub_out_slot2;
    uint32_t root_port = hub_slot->port_id;

    /*
     * For the child, the root port is the same as the hub's root port.
     * TT handling: if child is low/full behind high-speed hub, set TT slot/port.
     */
    uint8_t tt_slot = 0, tt_port = 0;
    bool    is_low_full = (speed == USB_SPEED_LOW || speed == USB_SPEED_FULL);
    bool    hub_is_high = (hub->speed == USB_SPEED_HIGH);
    if (is_low_full && hub_is_high) {
        tt_slot = hub_slot->slot_id;
        tt_port = port;
    }

    /* Address the slot with route, speed, and TT if needed. Not yet Hub. */
    ret = xhci_address_slot_tt(slot, speed, route, root_port, false, 0, tt_slot, tt_port);
    if (ret != EOK) goto free_slot;

    usb_device_t *dev  = &slot->usb;
    dev->connected     = true;
    dev->speed         = speed;
    dev->bus_number    = ctrl->bus_number;
    dev->port_number   = port;
    dev->depth         = hub->depth + 1;
    dev->hcd_ops       = &xhci_hcd_ops;
    dev->hc_private    = slot;
    uint32_t *out_slot = xhci_output_context(slot, 0);
    dev->address       = out_slot[3] & 0xff;
    if (!dev->address) dev->address = slot_id;

    /* Path: hub path + "." + port, e.g., "1-3.2" */
    (void)snprintf(dev->path, sizeof(dev->path), "%s.%u", hub->path, port);
    dev->dev.release = xhci_usb_device_release;

    /* GET_DESCRIPTOR device */
    ret = xhci_read_device_descriptor(dev);
    if (ret != EOK) goto remove_device;

    /* If the new device is a hub, update its slot with Hub + NumPorts via EVALUATE_CONTEXT. */
    bool is_hub = (dev->descriptor.device_class == USB_CLASS_HUB) || (dev->descriptor.device_class == 0x00 && 0); // device class may be 0x09 or per-interface

    /* Peek before GET_DESCRIPTOR config: hubs have device class 0x09. */
    if (dev->descriptor.device_class == USB_CLASS_HUB) is_hub = true;

    if (is_hub) {
        /* Need the hub descriptor to know port count. Do a minimal GET_DESCRIPTOR HUB (9 bytes) first. */
        uint8_t hub_desc[16];
        int     r2 = usb_control_msg(dev, USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_HUB << 8, 0, hub_desc, 9, USB_CTRL_TIMEOUT_MS);
        if (r2 == EOK && hub_desc[0] >= 7) {
            uint8_t nports = hub_desc[2];
            xhci_update_hub_slot(dev, nports);
        }
    }

    /* Strings and config */
    uint16_t lang = 0x0409;
    uint8_t  lang_desc[4];
    if (usb_control_msg(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_STRING << 8, 0, lang_desc, sizeof(lang_desc), USB_CTRL_TIMEOUT_MS) == EOK
        && lang_desc[0] >= 4) {
        lang = load_le16(&lang_desc[2]);
    }

    /* Xhci get string. */
    xhci_get_string(dev, dev->descriptor.manufacturer, lang, dev->manufacturer, sizeof(dev->manufacturer));
    xhci_get_string(dev, dev->descriptor.product, lang, dev->product, sizeof(dev->product));
    xhci_get_string(dev, dev->descriptor.serial_number, lang, dev->serial, sizeof(dev->serial));

    usb_config_descriptor_t hdr;
    ret = usb_control_msg(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, &hdr, sizeof(hdr), USB_CTRL_TIMEOUT_MS);
    if (ret != EOK || hdr.total_length < sizeof(hdr) || hdr.total_length > PAGE_4K_SIZE) goto remove_device;
    uint8_t *cfg = malloc(hdr.total_length);
    if (!cfg) {
        ret = -ENOMEM;
        goto remove_device;
    }
    ret              = usb_control_msg(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, cfg, hdr.total_length, USB_CTRL_TIMEOUT_MS);
    dev->dev.release = xhci_usb_device_release;
    if (ret == EOK) ret = usb_add_device(dev, cfg, hdr.total_length);
    free(cfg);
    if (ret == EOK) {
        *out = dev;
        return EOK;
    }
remove_device:
    usb_disconnect_device(dev);
    for (size_t i = 0; i < dev->interface_count; i++)
        if (dev->interfaces[i].registered) {
            device_unregister(&dev->interfaces[i].dev);
            dev->interfaces[i].registered = false;
        }
free_slot:
    ctrl->slots[slot_id] = NULL;
    ctrl->dcbaa[slot_id] = 0;
    if (slot) {
        for (size_t dci = 1; dci < XHCI_MAX_ENDPOINTS; dci++)
            if (slot->endpoints[dci].ring_physical) xhci_dma_free(slot->endpoints[dci].ring_physical, 1);
        if (slot->input_context_physical) xhci_dma_free(slot->input_context_physical, 1);
        if (slot->output_context_physical) xhci_dma_free(slot->output_context_physical, 1);
        free(slot);
    }
    xhci_command(ctrl, 0, 0, XHCI_TRB_TYPE(XHCI_TRB_DISABLE_SLOT) | ((uint32_t)slot_id << 24), NULL);
    return ret;
}

/* Find the slot currently bound to a root port. */
static xhci_slot_t *xhci_slot_on_port(xhci_controller_t *controller, uint8_t port_id)
{
    for (uint16_t slot = 1; slot <= controller->max_slots; slot++)
        if (controller->slots[slot] && controller->slots[slot]->port_id == port_id) return controller->slots[slot];
    return NULL;
}

/* Device-model release callback: free the slot. */
static void xhci_usb_device_release(struct device *dev)
{
    usb_device_t *usb  = container_of(dev, usb_device_t, dev);
    xhci_slot_t  *slot = container_of(usb, xhci_slot_t, usb);
    free(slot);
}

/* Disconnect a port's device, disable its slot, and free it. */
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
        if (intf->descriptor.interface_class == USB_CLASS_HID) {
            usb_hid_disconnect(intf);
        } else if (intf->descriptor.interface_class == USB_CLASS_MASS_STORAGE) {
            usb_storage_disconnect(intf);
        }
    }
    if (device->hcd_ops && device->hcd_ops->disable_device) device->hcd_ops->disable_device(device);

    for (size_t dci = 1; dci < XHCI_MAX_ENDPOINTS; dci++) xhci_dma_free(slot->endpoints[dci].ring_physical, slot->endpoints[dci].ring_physical ? 1 : 0);
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

/* Handle a port's change bits: connect, disconnect, or reset. */
static void xhci_service_port(xhci_controller_t *controller, uint8_t port_id)
{
    size_t   offset = XHCI_OP_PORTS + ((size_t)(port_id - 1) * XHCI_PORT_STRIDE);
    uint32_t status = xhci_read32(controller->operational, offset);
    xhci_write32(controller->operational, offset, xhci_port_neutral(status) | (status & XHCI_PORT_CHANGE_BITS));
    uint64_t     port_mask = 1ULL << (port_id - 1);
    xhci_slot_t *slot      = xhci_slot_on_port(controller, port_id);
    if (!(status & XHCI_PORT_CCS)) {
        controller->failed_ports &= ~port_mask;
        if (slot) xhci_disconnect_port(controller, port_id);
    } else {
        if (status & XHCI_PORT_CSC) controller->failed_ports &= ~port_mask;
        if (controller->failed_ports & port_mask) return;
        if (!(status & XHCI_PORT_CSC) && slot) return;
        if (slot) xhci_disconnect_port(controller, port_id);
        msleep(100);
        int result = xhci_enumerate_port(controller, port_id);
        if (result != EOK) {
            /* Reset/enable/link changes from this attempt must not retry it. */
            controller->failed_ports |= port_mask;
            plogk("usb-xhci: Port %u enumeration failed: %d; waiting for reconnect\n", port_id, result);
        }
    }
}

/* Hub worker: service pending port changes. */
static int xhci_worker(void *argument)
{
    xhci_controller_t *controller = argument;
    while (!controller->stopping && !kthread_should_stop()) {
        uint64_t flags            = spin_lock_irqsave(&controller->port_lock);
        uint64_t ports            = controller->pending_ports;
        controller->pending_ports = 0;
        if (!ports && !controller->stopping && !kthread_should_stop()) wait_queue_prepare(&controller->worker_wait);
        spin_unlock_irqrestore(&controller->port_lock, flags);
        if (!ports && !controller->stopping && !kthread_should_stop()) {
            wait_queue_sleep();
            continue;
        }
        for (uint8_t port = 1; port <= controller->max_ports; port++)
            if (ports & (1ULL << (port - 1))) xhci_service_port(controller, port);
    }
    return 0;
}

/* ISR: acknowledge event/port interrupts and drain the event ring. */
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

XHCI_IRQ_WRAPPER(0)
XHCI_IRQ_WRAPPER(1)
XHCI_IRQ_WRAPPER(2)
XHCI_IRQ_WRAPPER(3)
XHCI_IRQ_WRAPPER(4)
XHCI_IRQ_WRAPPER(5)
XHCI_IRQ_WRAPPER(6)
XHCI_IRQ_WRAPPER(7)

typedef void (*xhci_irq_handler_t)(interrupt_frame_t *frame);
static const xhci_irq_handler_t xhci_irq_handlers[USB_MAX_CONTROLLERS] = {
    xhci_interrupt_0, xhci_interrupt_1, xhci_interrupt_2, xhci_interrupt_3, xhci_interrupt_4, xhci_interrupt_5, xhci_interrupt_6, xhci_interrupt_7,
};

/* Claim ownership from the BIOS via the xHCI extended-capability handoff. */
static int xhci_take_ownership(xhci_controller_t *controller)
{
    uint32_t hccparams = xhci_read32(controller->capability, XHCI_CAP_HCCPARAMS1);
    size_t   offset    = (size_t)(hccparams >> 16) * 4U;
    for (unsigned int count = 0; offset && count < 64; count++) {
        uint32_t capability = xhci_read32(controller->capability, offset);
        uint8_t  id         = capability & 0xff;
        size_t   next       = (size_t)((capability >> 8) & 0xff) * 4U;
        if (id == 1) {
            xhci_write32(controller->capability, offset, capability | (1U << 24));
            int result = xhci_wait_register(controller->capability, offset, 1U << 16, 0, 1000);
            if (result != EOK) return result;
            uint32_t control = xhci_read32(controller->capability, offset + 4);
            xhci_write32(controller->capability, offset + 4, control & 0xffff0000U);
            return EOK;
        }
        offset = next ? offset + next : 0;
    }
    return EOK;
}

/* Release the scratchpad buffers and their pointer array. */
static void xhci_free_scratchpads(xhci_controller_t *controller)
{
    if (!controller) return;
    if (!controller->scratchpad_array) return;
    for (uint16_t i = 0; i < controller->scratchpad_count; i++)
        if (controller->scratchpad_array[i]) xhci_dma_free(controller->scratchpad_array[i], 1);
    size_t pages = ((size_t)controller->scratchpad_count * sizeof(uint64_t) + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    if (controller->scratchpad_array_physical) xhci_dma_free(controller->scratchpad_array_physical, pages);
    controller->scratchpad_array          = NULL;
    controller->scratchpad_array_physical = 0;
    controller->scratchpad_count          = 0;
}

/* Allocate the controller's scratchpad buffers. */
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

/* Claim an IRQ slot and enable MSI (or MSI-X) delivery. */
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
    pci_irq_request_t request = {
        .modes       = PCI_IRQ_MSI | PCI_IRQ_MSIX,
        .idt_handler = (void *)xhci_irq_handlers[slot],
    };
    if (pci_request_irq(controller->pci, &request, &controller->irq_state) < 0) {
        flags                = spin_lock_irqsave(&xhci_irq_lock);
        xhci_irq_slots[slot] = NULL;
        spin_unlock_irqrestore(&xhci_irq_lock, flags);
        return -ENODEV;
    }
    controller->interrupt_enabled = true;
    return EOK;
}

/* Stop the controller and free all of its resources. */
static void xhci_release_controller(xhci_controller_t *controller)
{
    if (!controller) return;
    if (controller->running) {
        controller->running = false;
        xhci_write32(controller->operational, XHCI_OP_USBCMD, xhci_read32(controller->operational, XHCI_OP_USBCMD) & ~(XHCI_CMD_RUN | XHCI_CMD_INTE));
    }
    if (controller->interrupt_enabled) {
        uint64_t flags = spin_lock_irqsave(&xhci_irq_lock);
        if (controller->irq_slot < USB_MAX_CONTROLLERS && xhci_irq_slots[controller->irq_slot] == controller) xhci_irq_slots[controller->irq_slot] = NULL;
        spin_unlock_irqrestore(&xhci_irq_lock, flags);
        pci_free_irq(controller->pci, &controller->irq_state);
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
    int       result = -EINVAL;
    pci_bar_t bar;
    if (pci_map_bar(pci, 0, &bar) < 0) return -ENODEV;
    xhci_controller_t *controller = calloc(1, sizeof(*controller));
    if (!controller) return -ENOMEM;
    controller->pci               = pci;
    controller->capability        = bar.virt;
    controller->bus_number        = bus_number;
    controller->capability_length = *controller->capability;
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

    pci_enable_device(pci, PCI_CMD_MEM | PCI_CMD_BUSMASTER);
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

    controller->host.type       = USB_HOST_XHCI;
    controller->host.bus_number = bus_number;
    controller->host.max_ports  = controller->max_ports;
    controller->host.hcd_ops    = &xhci_hcd_ops;
    controller->host.hc_private = controller;
    (void)snprintf(controller->host.name, sizeof(controller->host.name), "xhci-usb%u", bus_number);
    usb_host_register(&controller->host);

    plogk("usb-xhci: Controller at MMIO %p, bus usb%u, %u ports.\n", bar.virt, bus_number, controller->max_ports);
    return EOK;
fail:
    xhci_release_controller(controller);
    return result;
invalid:
    free(controller);
    return result;
}

/* Probe every xHCI controller in the PCI device cache. */
int xhci_init(void)
{
    size_t               before = xhci_controller_count;
    pci_devices_cache_t *cache  = pci_get_devices_cache();
    if (!cache) return 0;
    for (pci_device_cache_t *pci = cache->head; pci && xhci_controller_count < USB_MAX_CONTROLLERS; pci = pci->next) {
        if (pci->class_code != XHCI_PCI_CLASS) continue;
        int bus_number = usb_host_allocate_bus_number();
        if (bus_number < 0) break;
        int status = xhci_probe(pci, (uint8_t)bus_number);
        if (status != EOK) plogk("usb-xhci: Controller %04x:%02x:%02x.%u initialization failed: %d\n", pci->device->domain, pci->device->bus, pci->device->slot, pci->device->func, status);
    }
    return (int)(xhci_controller_count - before);
}

/* Register the hub worker task of every controller for unified creation. */
void xhci_start_workers(void)
{
    for (size_t i = 0; i < xhci_controller_count; i++) {
        xhci_controller_t *controller = xhci_controllers[i];
        if (!controller || controller->worker_started) continue;
        controller->worker_started = true;

        /* Enumerate already-connected root ports on the first worker pass. */
        for (uint8_t port = 1; port <= controller->max_ports; port++) {
            size_t   offset = XHCI_OP_PORTS + ((size_t)(port - 1) * XHCI_PORT_STRIDE);
            uint32_t status = xhci_read32(controller->operational, offset);
            if (status & XHCI_PORT_CCS) controller->pending_ports |= 1ULL << (port - 1);
        }
        kernel_worker_register("xhci-hub", xhci_worker, controller, &controller->worker_task);
    }
}

/* Stop every xHCI controller. */
void xhci_shutdown(void)
{
    for (size_t i = 0; i < xhci_controller_count; i++) {
        xhci_controller_t *controller = xhci_controllers[i];
        if (!controller) continue;
        controller->stopping = true;
        controller->running  = false;
        wait_queue_wake_all(&controller->worker_wait);
        xhci_write32(controller->operational, XHCI_OP_USBCMD, xhci_read32(controller->operational, XHCI_OP_USBCMD) & ~(XHCI_CMD_RUN | XHCI_CMD_INTE));
        pci_free_irq(controller->pci, &controller->irq_state);
    }
}

#endif
