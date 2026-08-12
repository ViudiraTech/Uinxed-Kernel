/*
 *
 *      ohci.c
 *      Open Host Controller Interface (OHCI) driver
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <drivers/bus/pci.h>
#include <drivers/firmware/apic.h>
#include <drivers/usb/host/host.h>
#include <drivers/usb/host/ohci/ohci.h>
#include <kernel/errno.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/stddef.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <process/sched.h>
#include <process/task.h>

/*
 * Overview
 * OHCI is a USB 1.1 host controller programmed through MMIO. Its
 * driver keeps static pools of Endpoint Descriptors (EDs) and
 * General Transfer Descriptors (gTDs); the HCD links EDs onto the
 * controller's control/bulk and periodic lists.
 */

#define OHCI_MAX_CONTROLLERS 8
#define OHCI_MAX_PORTS       15
#define OHCI_NUM_ED          32
#define OHCI_NUM_TD          128
#define OHCI_MAX_PERIODIC    32

typedef struct ohci_periodic_transfer {
        usb_endpoint_t          *endpoint;
        usb_interrupt_complete_t complete;
        void                    *context;
        void                    *buffer;
        size_t                   length;
        uint64_t                 next_poll;
        uint32_t                 interval_ms;
        volatile bool            active;
        volatile bool            in_callback;
} ohci_periodic_transfer_t;

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

        ohci_hcca_t  *hcca;
        uint64_t      hcca_physical;
        spinlock_t    lock;
        volatile bool io_busy;

        ohci_td_phys_t            tds[OHCI_NUM_TD];
        ohci_ed_phys_t            eds[OHCI_NUM_ED];
        usb_device_t             *devices[OHCI_MAX_PORTS];
        ohci_periodic_transfer_t *periodic[OHCI_MAX_PERIODIC];
        uint64_t                  pending_ports;
        task_t                   *worker_task;
        bool                      running;
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

/* Allocate zeroed DMA memory and return its physical address. */
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

/* Release DMA memory allocated by ohci_dma_alloc(). */
static void ohci_dma_free(uint64_t physical, size_t size)
{
    size_t count = (size + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    if (physical && count) free_frames(physical, count);
}

/* Claim a free endpoint-descriptor slot from the static pool. */
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

/* Return an endpoint-descriptor slot to the pool. */
static void ohci_free_ed(ohci_controller_t *ctrl, int index)
{
    if (index < 0 || index >= OHCI_NUM_ED) return;
    ctrl->eds[index].used = false;
    memset(ctrl->eds[index].virtual, 0, sizeof(ohci_ed_t));
}

/* Claim a free transfer-descriptor slot from the static pool. */
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

/* Return a transfer-descriptor slot to the pool. */
static void ohci_free_td(ohci_controller_t *ctrl, int index)
{
    if (index < 0 || index >= OHCI_NUM_TD) return;
    ctrl->tds[index].used = false;
    memset(ctrl->tds[index].virtual, 0, sizeof(ohci_gtd_t));
}

/* Spin until the controller's IO is exclusively owned by a transfer. */
static void ohci_io_lock(ohci_controller_t *ctrl)
{
    while (__atomic_test_and_set(&ctrl->io_busy, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
}

/* Release the exclusive IO lock. */
static void ohci_io_unlock(ohci_controller_t *ctrl)
{
    __atomic_clear(&ctrl->io_busy, __ATOMIC_RELEASE);
}

/* Translate a gTD condition code into a completion status. */
static int ohci_td_result(const ohci_gtd_t *td, bool allow_short)
{
    uint32_t condition = (td->control & OHCI_TD_CC_MASK) >> OHCI_TD_CC_SHIFT;
    switch (condition) {
        case OHCI_TD_CC_NOERROR :
            return EOK;
        case OHCI_TD_CC_NOT_ACCESSED :
            return -EINPROGRESS;
        case OHCI_TD_CC_CRC :
        case OHCI_TD_CC_TOGGLE :
            return -EILSEQ;
        case OHCI_TD_CC_BITSTUFF :
        case OHCI_TD_CC_PID :
        case OHCI_TD_CC_UNEXPECTED_PID :
            return -EPROTO;
        case OHCI_TD_CC_STALL :
            return -EPIPE;
        case OHCI_TD_CC_NORESPONSE :
            return -ETIMEDOUT;
        case OHCI_TD_CC_DATA_OVERRUN :
            return -EOVERFLOW;
        case OHCI_TD_CC_DATA_UNDERRUN :
            return allow_short ? EOK : -EREMOTEIO;
        case OHCI_TD_CC_BUFFER_OVERRUN :
        case OHCI_TD_CC_BUFFER_UNDERRUN :
            return -ECOMM;
        default :
            return -EIO;
    }
}

/* Wait for a single gTD to finish, with timeout. */
static int ohci_wait_td(ohci_controller_t *ctrl, ohci_gtd_t *td, bool allow_short, uint32_t timeout_ms)
{
    uint64_t deadline = nano_time() + (uint64_t)timeout_ms * 1000000ULL;
    for (;;) {
        if (!ctrl->running) return -ESHUTDOWN;
        int status = ohci_td_result(td, allow_short);
        if (status != -EINPROGRESS) return status;
        if (nano_time() >= deadline) return -ETIMEDOUT;
        __asm__ volatile("pause");
    }
}

/* Wait for every control-transfer gTD to complete, in order. */
static int ohci_wait_control(ohci_controller_t *ctrl, const int *td_indices, size_t count, size_t data_position, bool input, uint32_t timeout_ms)
{
    uint64_t deadline = nano_time() + (uint64_t)timeout_ms * 1000000ULL;
    for (size_t position = 0; position < count; position++) {
        for (;;) {
            if (!ctrl->running) return -ESHUTDOWN;
            bool allow_short = input && position == data_position;
            int  status      = ohci_td_result(ctrl->tds[td_indices[position]].virtual, allow_short);
            if (status != -EINPROGRESS) {
                if (status != EOK) return status;
                break;
            }
            if (nano_time() >= deadline) return -ETIMEDOUT;
            __asm__ volatile("pause");
        }
    }
    return EOK;
}

/* Compute the bytes actually transferred from the gTD's current pointer. */
static size_t ohci_td_actual(const ohci_gtd_t *td, uint32_t buffer_start, size_t requested)
{
    if (!requested || !td->current_buffer_pointer) return requested;
    uint32_t current = td->current_buffer_pointer;
    if (current < buffer_start || (uint64_t)current > (uint64_t)buffer_start + requested) return 0;
    return current - buffer_start;
}

/* Build an ED control dword from the endpoint's address and speed. */
static uint32_t ohci_ed_control(const usb_device_t *device, uint8_t endpoint, uint16_t max_packet)
{
    uint32_t control = (uint32_t)device->address << OHCI_ED_FA_SHIFT | (uint32_t)endpoint << OHCI_ED_EN_SHIFT | (uint32_t)max_packet << OHCI_ED_MPS_SHIFT;
    if (device->speed == USB_SPEED_LOW) control |= OHCI_ED_S;
    return control;
}

/* Populate a gTD with its flags, buffer range, and next link. */
static void ohci_fill_td(ohci_gtd_t *td, uint32_t flags, uint32_t buffer, size_t length, uint32_t next)
{
    td->control                = flags | ((uint32_t)OHCI_TD_CC_NOT_ACCESSED << OHCI_TD_CC_SHIFT);
    td->current_buffer_pointer = length ? buffer : 0;
    td->buffer_end             = length ? buffer + (uint32_t)length - 1 : 0;
    td->next_td                = next;
}

/*
 * Transfer submission
 * Control transfers are built as an ED whose TD chain performs
 * SETUP / DATA / STATUS; bulk and interrupt use gTDs under their
 * own EDs. Completion is polled from the gTD's done status.
 */

static int ohci_control(usb_device_t *device, const usb_setup_packet_t *setup, void *buffer, size_t length, uint32_t timeout_ms)
{
    ohci_controller_t *ctrl = device ? device->hc_private : NULL;
    if (!ctrl || !setup || (length && !buffer) || length > PAGE_4K_SIZE || length != usb_get_le16(&setup->length)) return -EINVAL;
    uint16_t max_packet = device->descriptor.max_packet_size0 ? device->descriptor.max_packet_size0 : 8;
    if (max_packet != 8 && max_packet != 16 && max_packet != 32 && max_packet != 64) return -EPROTO;
    bool input = (setup->request_type & USB_DIR_IN) != 0;

    ohci_io_lock(ctrl);
    int      status         = -ENOMEM;
    int      ed_index       = -1;
    int      td_indices[4]  = {-1, -1, -1, -1};
    size_t   td_count       = length ? 4 : 3;
    uint64_t setup_physical = 0, data_physical = 0;
    void    *setup_dma = ohci_dma_alloc(sizeof(*setup), &setup_physical);
    void    *data_dma  = NULL;
    if (!setup_dma || setup_physical > UINT32_MAX) {
        plogk("ohci: Control DMA allocation failed on bus %u\n", ctrl->bus_number);
        goto control_cleanup;
    }
    memcpy(setup_dma, setup, sizeof(*setup));
    if (length) {
        data_dma = ohci_dma_alloc(length, &data_physical);
        if (!data_dma || data_physical > UINT32_MAX || data_physical + length - 1 > UINT32_MAX) {
            plogk("ohci: Control data DMA allocation failed on bus %u (%zu bytes)\n", ctrl->bus_number, length);
            goto control_cleanup;
        }
        if (!input) memcpy(data_dma, buffer, length);
    }
    ed_index = ohci_find_free_ed(ctrl);
    if (ed_index < 0) {
        plogk("ohci: ED pool exhausted on bus %u\n", ctrl->bus_number);
        goto control_cleanup;
    }
    for (size_t i = 0; i < td_count; i++) {
        td_indices[i] = ohci_find_free_td(ctrl);
        if (td_indices[i] < 0) {
            plogk("ohci: TD pool exhausted on bus %u\n", ctrl->bus_number);
            goto control_cleanup;
        }
    }

    size_t data_position   = 1;
    size_t status_position = length ? 2 : 1;
    size_t dummy_position  = status_position + 1;
    ohci_fill_td(ctrl->tds[td_indices[0]].virtual, OHCI_TD_DP_SETUP | OHCI_TD_TOGGLE_DATA0 | OHCI_TD_DI_NONE, (uint32_t)setup_physical, sizeof(*setup),
                 (uint32_t)ctrl->tds[td_indices[data_position]].physical);
    if (length) {
        ohci_fill_td(ctrl->tds[td_indices[data_position]].virtual, (input ? OHCI_TD_DP_IN | OHCI_TD_R : OHCI_TD_DP_OUT) | OHCI_TD_TOGGLE_DATA1 | OHCI_TD_DI_NONE, (uint32_t)data_physical, length,
                     (uint32_t)ctrl->tds[td_indices[status_position]].physical);
    }
    ohci_fill_td(ctrl->tds[td_indices[status_position]].virtual, (input && length ? OHCI_TD_DP_OUT : OHCI_TD_DP_IN) | OHCI_TD_TOGGLE_DATA1, 0, 0,
                 (uint32_t)ctrl->tds[td_indices[dummy_position]].physical);

    ohci_ed_t *ed    = ctrl->eds[ed_index].virtual;
    ed->control      = ohci_ed_control(device, 0, max_packet);
    ed->tail_pointer = (uint32_t)ctrl->tds[td_indices[dummy_position]].physical;
    ed->head_pointer = (uint32_t)ctrl->tds[td_indices[0]].physical;
    ed->next_ed      = 0;
    dma_write_barrier();
    ohci_write32(ctrl, OHCI_HcControlHeadED, (uint32_t)ctrl->eds[ed_index].physical);
    ohci_write32(ctrl, OHCI_HcCommandStatus, OHCI_CMD_CLF);
    status = ohci_wait_control(ctrl, td_indices, status_position + 1, data_position, input && length, timeout_ms);

    ed->control |= OHCI_ED_K;
    ohci_write32(ctrl, OHCI_HcControlHeadED, 0);
    dma_write_barrier();
    msleep(2);
    if (status == EOK && input && length) {
        size_t actual = ohci_td_actual(ctrl->tds[td_indices[data_position]].virtual, (uint32_t)data_physical, length);
        memcpy(buffer, data_dma, actual);
    }
control_cleanup:
    if (ed_index >= 0) ohci_free_ed(ctrl, ed_index);
    for (size_t i = 0; i < td_count; i++)
        if (td_indices[i] >= 0) ohci_free_td(ctrl, td_indices[i]);
    if (setup_dma) ohci_dma_free(setup_physical, sizeof(*setup));
    if (data_dma) ohci_dma_free(data_physical, length);
    ohci_io_unlock(ctrl);
    return status;
}

/* Bulk/interrupt transfer under an endpoint's ED */
static int ohci_transfer(usb_endpoint_t *endpoint, void *buffer, size_t length, size_t *actual, uint32_t timeout_ms)
{
    if (!endpoint || !endpoint->interface || !endpoint->interface->device || (length && !buffer) || length > PAGE_4K_SIZE) return -EINVAL;
    if (actual) *actual = 0;
    if (!length) return EOK;
    usb_device_t      *device     = endpoint->interface->device;
    ohci_controller_t *ctrl       = device->hc_private;
    uint16_t           max_packet = usb_get_le16(&endpoint->descriptor.max_packet_size) & 0x07ff;
    if (!ctrl || !max_packet || max_packet > 1023) return -EINVAL;
    bool    input           = (endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK) != 0;
    uint8_t endpoint_number = endpoint->descriptor.endpoint_address & USB_ENDPOINT_NUMBER_MASK;

    ohci_io_lock(ctrl);
    int      status   = -ENOMEM;
    int      ed_index = -1, data_index = -1, dummy_index = -1;
    uint64_t data_physical = 0;
    void    *data_dma      = ohci_dma_alloc(length, &data_physical);
    if (!data_dma || data_physical > UINT32_MAX || data_physical + length - 1 > UINT32_MAX) {
        plogk("ohci: Transfer DMA allocation failed on bus %u (%zu bytes)\n", ctrl->bus_number, length);
        goto transfer_cleanup;
    }
    if (!input) memcpy(data_dma, buffer, length);
    ed_index    = ohci_find_free_ed(ctrl);
    data_index  = ohci_find_free_td(ctrl);
    dummy_index = ohci_find_free_td(ctrl);
    if (ed_index < 0 || data_index < 0 || dummy_index < 0) {
        plogk("ohci: ED/TD pool exhausted on bus %u\n", ctrl->bus_number);
        goto transfer_cleanup;
    }

    ohci_fill_td(ctrl->tds[data_index].virtual, (input ? OHCI_TD_DP_IN | OHCI_TD_R : OHCI_TD_DP_OUT) | OHCI_TD_TOGGLE_CARRY, (uint32_t)data_physical, length,
                 (uint32_t)ctrl->tds[dummy_index].physical);
    ohci_ed_t *ed    = ctrl->eds[ed_index].virtual;
    ed->control      = ohci_ed_control(device, endpoint_number, max_packet);
    ed->tail_pointer = (uint32_t)ctrl->tds[dummy_index].physical;
    ed->head_pointer = (uint32_t)ctrl->tds[data_index].physical | (endpoint->data_toggle ? 2U : 0U);
    ed->next_ed      = 0;
    dma_write_barrier();
    ohci_write32(ctrl, OHCI_HcBulkHeadED, (uint32_t)ctrl->eds[ed_index].physical);
    ohci_write32(ctrl, OHCI_HcCommandStatus, OHCI_CMD_BLF);
    status = ohci_wait_td(ctrl, ctrl->tds[data_index].virtual, input, timeout_ms);

    ed->control |= OHCI_ED_K;
    ohci_write32(ctrl, OHCI_HcBulkHeadED, 0);
    dma_write_barrier();
    msleep(2);
    size_t transferred = ohci_td_actual(ctrl->tds[data_index].virtual, (uint32_t)data_physical, length);
    if (status == EOK) {
        endpoint->data_toggle = (ed->head_pointer & 2U) != 0;
        if (actual) *actual = transferred;
        if (input && transferred) memcpy(buffer, data_dma, transferred);
    }
transfer_cleanup:
    if (ed_index >= 0) ohci_free_ed(ctrl, ed_index);
    if (data_index >= 0) ohci_free_td(ctrl, data_index);
    if (dummy_index >= 0) ohci_free_td(ctrl, dummy_index);
    if (data_dma) ohci_dma_free(data_physical, length);
    ohci_io_unlock(ctrl);
    return status;
}

/* Register a periodic interrupt-IN transfer for the worker to service. */
static int ohci_interrupt_start(usb_endpoint_t *endpoint, size_t length, usb_interrupt_complete_t complete, void *context)
{
    if (!endpoint || !endpoint->interface || !endpoint->interface->device || !length || length > PAGE_4K_SIZE || !complete) return -EINVAL;
    if ((endpoint->descriptor.attributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_INT || !(endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK)) return -EINVAL;
    if (endpoint->hc_private) return -EBUSY;
    ohci_controller_t *ctrl = endpoint->interface->device->hc_private;
    if (!ctrl) return -ENODEV;
    ohci_periodic_transfer_t *transfer = calloc(1, sizeof(*transfer));
    if (!transfer) return -ENOMEM;
    transfer->buffer = malloc(length);
    if (!transfer->buffer) {
        free(transfer);
        return -ENOMEM;
    }
    transfer->endpoint    = endpoint;
    transfer->complete    = complete;
    transfer->context     = context;
    transfer->length      = length;
    transfer->interval_ms = endpoint->descriptor.interval ? endpoint->descriptor.interval : 1;
    transfer->next_poll   = nano_time();
    transfer->active      = true;

    uint64_t flags = spin_lock_irqsave(&ctrl->lock);
    for (size_t i = 0; i < OHCI_MAX_PERIODIC; i++) {
        if (!ctrl->periodic[i]) {
            ctrl->periodic[i]    = transfer;
            endpoint->hc_private = transfer;
            spin_unlock_irqrestore(&ctrl->lock, flags);
            return EOK;
        }
    }
    spin_unlock_irqrestore(&ctrl->lock, flags);
    free(transfer->buffer);
    free(transfer);
    return -ENOSPC;
}

/* Unregister a periodic transfer and wait out any running callback. */
static void ohci_interrupt_stop(usb_endpoint_t *endpoint)
{
    ohci_periodic_transfer_t *transfer = endpoint ? endpoint->hc_private : NULL;
    if (!transfer || !endpoint->interface || !endpoint->interface->device) return;
    ohci_controller_t *ctrl  = endpoint->interface->device->hc_private;
    uint64_t           flags = spin_lock_irqsave(&ctrl->lock);
    transfer->active         = false;
    for (size_t i = 0; i < OHCI_MAX_PERIODIC; i++)
        if (ctrl->periodic[i] == transfer) ctrl->periodic[i] = NULL;
    endpoint->hc_private = NULL;
    spin_unlock_irqrestore(&ctrl->lock, flags);
    while (__atomic_load_n(&transfer->in_callback, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    free(transfer->buffer);
    free(transfer);
}

/* Initialize per-endpoint state before it is used. */
static int ohci_configure_endpoint(usb_endpoint_t *endpoint)
{
    if (!endpoint) return -EINVAL;
    endpoint->data_toggle = 0;
    return EOK;
}

/* Reset the endpoint's data-toggle state after a stall. */
static int ohci_clear_halt(usb_endpoint_t *endpoint)
{
    if (!endpoint) return -EINVAL;
    endpoint->data_toggle = 0;
    return EOK;
}

/* Stop every interrupt transfer of a device before it goes away. */
static void ohci_disable_device(usb_device_t *device)
{
    if (!device) return;
    for (size_t i = 0; i < device->interface_count; i++)
        for (size_t j = 0; j < device->interfaces[i].endpoint_count; j++) ohci_interrupt_stop(&device->interfaces[i].endpoints[j]);
}

static const usb_hcd_ops_t ohci_hcd_ops = {
    .control            = ohci_control,
    .transfer           = ohci_transfer,
    .interrupt_start    = ohci_interrupt_start,
    .interrupt_stop     = ohci_interrupt_stop,
    .configure_endpoint = ohci_configure_endpoint,
    .disable_endpoint   = ohci_interrupt_stop,
    .clear_halt         = ohci_clear_halt,
    .disable_device     = ohci_disable_device,
};

/* Perform the OHCI port reset sequence. */
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

/* Classify a port's device speed from the status dword. */
static usb_speed_t ohci_port_speed(uint32_t portsc)
{
    return (portsc & OHCI_PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

/* Device-model release callback: free the usb_device. */
static void ohci_usb_device_release(struct device *dev)
{
    usb_device_t *device = container_of(dev, usb_device_t, dev);
    free(device);
}

/* Fetch and convert a device string descriptor to ASCII. */
static int ohci_get_string(usb_device_t *device, uint8_t index, uint16_t language, char *output, size_t capacity)
{
    uint8_t descriptor[128];
    if (!index || !output || capacity < 2) return -EINVAL;
    int result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, (USB_DT_STRING << 8) | index, language, descriptor, sizeof(descriptor),
                                 USB_CTRL_TIMEOUT_MS);
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

/* Reset a port, address the device and register it with the USB core */
static int ohci_enumerate_port(ohci_controller_t *ctrl, uint8_t port)
{
    if (!ctrl || port >= ctrl->num_ports) return -EINVAL;
    if (ctrl->devices[port]) return -EEXIST;
    int result = ohci_port_reset(ctrl, port);
    if (result != EOK) return result;

    uint32_t    portsc = ohci_read32(ctrl, OHCI_HcRhPortStatus + port * 4);
    usb_speed_t speed  = ohci_port_speed(portsc);

    usb_device_t *device = calloc(1, sizeof(*device));
    if (!device) return -ENOMEM;
    device->connected   = true;
    device->speed       = speed;
    device->bus_number  = ctrl->bus_number;
    device->port_number = port + 1;
    device->hcd_ops     = &ohci_hcd_ops;
    device->hc_private  = ctrl;
    device->dev.release = ohci_usb_device_release;
    (void)snprintf(device->path, sizeof(device->path), "%u-%u", device->bus_number, port + 1);

    uint8_t address = port + 1;
    result          = usb_control_msg(device, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_ADDRESS, address, 0, NULL, 0, USB_CTRL_TIMEOUT_MS);
    if (result != EOK) goto fail;
    msleep(10);
    device->address = address;

    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, &device->descriptor, sizeof(device->descriptor),
                             USB_CTRL_TIMEOUT_MS);
    if (result != EOK || device->descriptor.length < sizeof(device->descriptor) || device->descriptor.descriptor_type != USB_DT_DEVICE) {
        if (result == EOK) result = -EPROTO;
        goto fail;
    }

    uint16_t language = 0x0409;
    uint8_t  lang_desc[4];
    if (usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_STRING << 8, 0, lang_desc, sizeof(lang_desc), USB_CTRL_TIMEOUT_MS) == EOK
        && lang_desc[0] >= 4)
        language = lang_desc[2] | (uint16_t)lang_desc[3] << 8;
    ohci_get_string(device, device->descriptor.manufacturer, language, device->manufacturer, sizeof(device->manufacturer));
    ohci_get_string(device, device->descriptor.product, language, device->product, sizeof(device->product));
    ohci_get_string(device, device->descriptor.serial_number, language, device->serial, sizeof(device->serial));

    usb_config_descriptor_t header;
    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, &header, sizeof(header), USB_CTRL_TIMEOUT_MS);
    if (result != EOK) goto fail;
    uint16_t total_length = usb_get_le16(&header.total_length);
    if (header.descriptor_type != USB_DT_CONFIG || header.length < sizeof(header) || total_length < sizeof(header) || total_length > PAGE_4K_SIZE) {
        result = -EPROTO;
        goto fail;
    }

    uint8_t *configuration = malloc(total_length);
    if (!configuration) {
        result = -ENOMEM;
        goto fail;
    }
    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, configuration, total_length, USB_CTRL_TIMEOUT_MS);
    if (result == EOK) result = usb_add_device(device, configuration, total_length);
    free(configuration);
    if (result == EOK) {
        ctrl->devices[port] = device;
        return EOK;
    }

fail: {
    bool registered = device->registered;
    usb_remove_device(device);
    if (!registered) free(device);
    return result;
}
}

/* Tear down the device currently attached to a port. */
static void ohci_disconnect_port(ohci_controller_t *ctrl, uint8_t port)
{
    if (!ctrl || port >= ctrl->num_ports) return;
    usb_device_t *device = ctrl->devices[port];
    if (!device) return;
    ctrl->devices[port] = NULL;
    usb_remove_device(device);
}

/* Poll every due periodic transfer and deliver its results. */
static void ohci_service_periodic(ohci_controller_t *ctrl)
{
    uint64_t now = nano_time();
    for (size_t i = 0; i < OHCI_MAX_PERIODIC; i++) {
        uint64_t                  flags    = spin_lock_irqsave(&ctrl->lock);
        ohci_periodic_transfer_t *transfer = ctrl->periodic[i];
        if (!transfer || !transfer->active || transfer->in_callback || now < transfer->next_poll) {
            spin_unlock_irqrestore(&ctrl->lock, flags);
            continue;
        }
        transfer->in_callback = true;
        spin_unlock_irqrestore(&ctrl->lock, flags);

        size_t actual = 0;
        int    status = ohci_transfer(transfer->endpoint, transfer->buffer, transfer->length, &actual, USB_IO_TIMEOUT_MS);
        if (__atomic_load_n(&transfer->active, __ATOMIC_ACQUIRE)) transfer->complete(transfer->endpoint, transfer->buffer, actual, status, transfer->context);
        transfer->next_poll = nano_time() + (uint64_t)transfer->interval_ms * 1000000ULL;
        __atomic_store_n(&transfer->in_callback, false, __ATOMIC_RELEASE);
    }
}

/* Hub worker: handle port changes, enumerate devices, poll periodic. */
static void ohci_worker(void *argument)
{
    ohci_controller_t *ctrl = argument;
    while (ctrl->running) {
        uint64_t flags      = spin_lock_irqsave(&ctrl->lock);
        uint64_t ports      = ctrl->pending_ports;
        ctrl->pending_ports = 0;
        spin_unlock_irqrestore(&ctrl->lock, flags);
        for (uint8_t port = 0; port < ctrl->num_ports; port++) {
            if (!(ports & (1ULL << port))) continue;
            uint32_t portsc = ohci_read32(ctrl, OHCI_HcRhPortStatus + port * 4);
            uint32_t change = portsc & OHCI_PORT_CHANGE_BITS;
            if (!change) continue;
            ohci_write32(ctrl, OHCI_HcRhPortStatus + port * 4, change);
            if (!(portsc & OHCI_PORT_CCS)) {
                ohci_disconnect_port(ctrl, port);
            } else if (portsc & OHCI_PORT_CSC) {
                ohci_disconnect_port(ctrl, port);
                msleep(100);
                int ret = ohci_enumerate_port(ctrl, port);
                if (ret != EOK) plogk("ohci: Port %u enumeration failed: %d\n", port, ret);
            }
        }
        ohci_service_periodic(ctrl);
        msleep(1);
    }
}

/* ISR: acknowledge status bits and flag port-change work. */
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
            uint64_t flags = spin_lock_irqsave(&ctrl->lock);
            ctrl->pending_ports |= (1ULL << ctrl->num_ports) - 1;
            spin_unlock_irqrestore(&ctrl->lock, flags);
        }
        if (sts & OHCI_INTR_WDH) ohci_write32(ctrl, OHCI_HcInterruptStatus, OHCI_INTR_WDH);
        if (sts & OHCI_INTR_UE) {
            ohci_write32(ctrl, OHCI_HcInterruptStatus, OHCI_INTR_UE);
            plogk("ohci: Controller error on bus %u\n", ctrl->bus_number);
        }
        if (sts & OHCI_INTR_SO) {
            ohci_write32(ctrl, OHCI_HcInterruptStatus, OHCI_INTR_SO);
            plogk("ohci: Scheduling overrun on bus %u\n", ctrl->bus_number);
        }
    }
    send_eoi();
}

/* host_ops: controller is already running after probe. */
static int ohci_host_start(usb_host_t *host)
{
    (void)host;
    return EOK;
}

/* host_ops: reset the controller and disable its interrupts. */
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

/* host_ops: reset a port. */
static int ohci_port_reset_hcd(usb_host_t *host, uint8_t port)
{
    ohci_controller_t *ctrl = container_of(host, ohci_controller_t, hcd);
    return ohci_port_reset(ctrl, port);
}

/* host_ops: report the speed of a port. */
static int ohci_port_speed_hcd(usb_host_t *host, uint8_t port)
{
    ohci_controller_t *ctrl = container_of(host, ohci_controller_t, hcd);
    if (port >= ctrl->num_ports) return USB_SPEED_FULL;
    return ohci_port_speed(ohci_read32(ctrl, OHCI_HcRhPortStatus + port * 4));
}

/* host_ops: report whether a port has a device connected. */
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

/* Probe an OHCI PCI device: reset, allocate pools, and start. */
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
    (void)snprintf(ctrl->hcd.name, sizeof(ctrl->hcd.name), "ohci-usb%u", bus_number);

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
    if (ctrl->vector > 0) register_interrupt_handler((uint16_t)ctrl->vector, ohci_interrupt_handler, 0, 0x8e);

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

    plogk("ohci: Controller at MMIO %p, bus usb%u, %u ports.\n", (void *)bar.address, bus_number, ctrl->num_ports);
    return EOK;
}

void ohci_start_workers(void)
{
#if !CONFIG_USB_OHCI
    return;
#endif
}

/* Probe every OHCI controller in the PCI device cache. */
void ohci_init(void)
{
#if !CONFIG_USB_OHCI
    return;
#endif
    if (usb_core_init() != EOK) return;
    pci_devices_cache_t *cache = pci_get_devices_cache();
    if (!cache) return;
    for (pci_device_cache_t *pci = cache->head; pci && ohci_controller_count < OHCI_MAX_CONTROLLERS; pci = pci->next) {
        if (pci->class_code != OHCI_PCI_CLASS) continue;
        int bus_number = usb_host_allocate_bus_number();
        if (bus_number < 0) break;
        (void)ohci_probe(pci, (uint8_t)bus_number);
    }
}
