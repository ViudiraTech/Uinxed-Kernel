/*
 *
 *      uhci.c
 *      Universal Host Controller Interface (UHCI) driver
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <drivers/bus/pci.h>
#include <drivers/firmware/apic.h>
#include <drivers/usb/host/host.h>
#include <drivers/usb/host/uhci/uhci.h>
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

#define UHCI_MAX_CONTROLLERS  8
#define UHCI_RESET_TIMEOUT_MS 100
#define UHCI_ENUM_TIMEOUT_MS  1000
#define UHCI_IO_CHUNK         PAGE_4K_SIZE
#define UHCI_MAX_PERIODIC     32

typedef struct uhci_periodic_transfer {
        usb_endpoint_t          *endpoint;
        usb_interrupt_complete_t complete;
        void                    *context;
        void                    *buffer;
        size_t                   length;
        uint64_t                 next_poll;
        uint32_t                 interval_ms;
        volatile bool            active;
        volatile bool            in_callback;
} uhci_periodic_transfer_t;

typedef struct uhci_td_phys {
        uhci_td_t *virtual;
        uint64_t physical;
        bool     used;
} uhci_td_phys_t;

typedef struct uhci_qh_phys {
        uhci_qh_t *virtual;
        uint64_t physical;
        bool     used;
} uhci_qh_phys_t;

typedef struct uhci_async_transfer {
        struct uhci_controller *controller;
        usb_device_t           *device;
        usb_endpoint_t         *endpoint;
        uhci_td_phys_t         *tds;
        int                     td_count;
        uhci_qh_phys_t         *qh;
        void                   *buffer;
        size_t                  length;
        size_t                  actual;
        int                     status;
        volatile bool           completed;
        bool                    input;
        uint32_t                token;
} uhci_async_transfer_t;

typedef struct uhci_controller {
        usb_host_t          hcd;
        uint16_t            io_base;
        pci_device_cache_t *pci;
        uint8_t             irq;
        int                 vector;
        uint8_t             bus_number;
        uint8_t             irq_slot;

        uint32_t *frame_list_virtual;
        uint64_t  frame_list_physical;

        uhci_td_phys_t            tds[UHCI_NUM_TD];
        uhci_qh_phys_t            qhs[UHCI_NUM_QH];
        uint8_t                   port_state[UHCI_MAX_PORTS];
        usb_device_t             *devices[UHCI_MAX_PORTS];
        uhci_periodic_transfer_t *periodic[UHCI_MAX_PERIODIC];
        uint64_t                  pending_ports;

        spinlock_t    lock;
        spinlock_t    td_lock;
        volatile bool io_busy;
        wait_queue_t  worker_wait;
        task_t       *worker_task;
        bool          running;
        bool          worker_started;
} uhci_controller_t;

static uhci_controller_t *uhci_controllers[UHCI_MAX_CONTROLLERS];
static size_t             uhci_controller_count;

/* Read a 16-bit I/O register. */
static inline uint16_t uhci_readw(uhci_controller_t *ctrl, uint8_t reg)
{
    return inw(ctrl->io_base + reg);
}

/* Write a 16-bit I/O register. */
static inline void uhci_writew(uhci_controller_t *ctrl, uint8_t reg, uint16_t value)
{
    outw(ctrl->io_base + reg, value);
}

/* Read a 32-bit I/O register. */
static inline uint32_t uhci_readl(uhci_controller_t *ctrl, uint8_t reg)
{
    return inl(ctrl->io_base + reg);
}

/* Write a 32-bit I/O register. */
static inline void uhci_writel(uhci_controller_t *ctrl, uint8_t reg, uint32_t value)
{
    outl(ctrl->io_base + reg, value);
}

/* Allocate zeroed DMA memory and return its physical address. */
static void *uhci_dma_alloc(size_t size, uint64_t *physical)
{
    size_t   count   = (size + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    uint64_t address = alloc_frames(count);
    if (!address) return NULL;
    void *memory = phys_to_virt(address);
    memset(memory, 0, count * PAGE_4K_SIZE);
    *physical = address;
    return memory;
}

/* Release DMA memory allocated by uhci_dma_alloc(). */
static void uhci_dma_free(uint64_t physical, size_t size)
{
    size_t count = (size + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    if (physical && count) free_frames(physical, count);
}

/* Claim a free transfer-descriptor slot from the static pool. */
static int uhci_find_free_td(uhci_controller_t *ctrl)
{
    uint64_t flags = spin_lock_irqsave(&ctrl->td_lock);
    for (int i = 0; i < UHCI_NUM_TD; i++) {
        if (!ctrl->tds[i].used) {
            ctrl->tds[i].used = true;
            spin_unlock_irqrestore(&ctrl->td_lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&ctrl->td_lock, flags);
    return -1;
}

/* Return a transfer-descriptor slot to the pool. */
static void uhci_free_td(uhci_controller_t *ctrl, int index)
{
    if (index < 0 || index >= UHCI_NUM_TD) return;
    uint64_t flags        = spin_lock_irqsave(&ctrl->td_lock);
    ctrl->tds[index].used = false;
    memset(ctrl->tds[index].virtual, 0, sizeof(uhci_td_t));
    spin_unlock_irqrestore(&ctrl->td_lock, flags);
}

/* Claim a free queue-head slot from the static pool. */
static int uhci_find_free_qh(uhci_controller_t *ctrl)
{
    uint64_t flags = spin_lock_irqsave(&ctrl->lock);
    for (int i = 0; i < UHCI_NUM_QH; i++) {
        if (!ctrl->qhs[i].used) {
            ctrl->qhs[i].used = true;
            spin_unlock_irqrestore(&ctrl->lock, flags);
            return i;
        }
    }
    spin_unlock_irqrestore(&ctrl->lock, flags);
    return -1;
}

/* Return a queue-head slot to the pool. */
static void uhci_free_qh(uhci_controller_t *ctrl, int index)
{
    if (index < 0 || index >= UHCI_NUM_QH) return;
    uint64_t flags        = spin_lock_irqsave(&ctrl->lock);
    ctrl->qhs[index].used = false;
    memset(ctrl->qhs[index].virtual, 0, sizeof(uhci_qh_t));
    spin_unlock_irqrestore(&ctrl->lock, flags);
}

/* Spin until the controller's IO is exclusively owned by a transfer. */
static void uhci_io_lock(uhci_controller_t *ctrl)
{
    while (__atomic_test_and_set(&ctrl->io_busy, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
}

/* Release the exclusive IO lock. */
static void uhci_io_unlock(uhci_controller_t *ctrl)
{
    __atomic_clear(&ctrl->io_busy, __ATOMIC_RELEASE);
}

/* Translate a TD control/status dword into a completion status. */
static int uhci_td_result(const uhci_td_t *td)
{
    uint32_t status = td->control_status;
    if (status & UHCI_TD_ACTIVE) return -EINPROGRESS;
    if (status & UHCI_TD_STALLED) return -EPIPE;
    if (status & UHCI_TD_BABBLE) return -EOVERFLOW;
    if (status & UHCI_TD_DBUFERR) return -EPROTO;
    if (status & (UHCI_TD_CRCTIMEO | UHCI_TD_BITSTUFF)) return -EILSEQ;
    return EOK;
}

/* Point every periodic frame at the given QH. */
static void uhci_schedule_qh(uhci_controller_t *ctrl, int qh_index)
{
    uint32_t link = (uint32_t)ctrl->qhs[qh_index].physical | UHCI_LINK_QH;
    for (size_t frame = 0; frame < UHCI_FRAME_LIST_SIZE; frame++) ctrl->frame_list_virtual[frame] = link;
    dma_write_barrier();
}

/* Detach a QH from the periodic frame list. */
static void uhci_unschedule_qh(uhci_controller_t *ctrl, int qh_index)
{
    ctrl->qhs[qh_index].virtual->element_link = UHCI_LINK_TERMINATE;
    dma_write_barrier();
    for (size_t frame = 0; frame < UHCI_FRAME_LIST_SIZE; frame++) ctrl->frame_list_virtual[frame] = UHCI_LINK_TERMINATE;
    dma_write_barrier();
    msleep(2);
}

/* Build a TD control/status dword from the device speed. */
static uint32_t uhci_td_flags(const usb_device_t *device, bool short_packet)
{
    uint32_t flags = UHCI_TD_ACTIVE | UHCI_TD_ERROR_COUNT;
    if (device->speed == USB_SPEED_LOW) flags |= UHCI_TD_LOW_SPEED;
    if (short_packet) flags |= UHCI_TD_SHORT_PACKET;
    return flags;
}

/* Encode the TD token word for a transfer phase. */
static uint32_t uhci_token(uint8_t pid, uint8_t address, uint8_t endpoint, uint8_t toggle, size_t length)
{
    return (uint32_t)pid | ((uint32_t)address << UHCI_TOKEN_DEVADDR_SHIFT) | ((uint32_t)endpoint << UHCI_TOKEN_ENDP_SHIFT) | ((uint32_t)(toggle & 1) << UHCI_TOKEN_TOGGLE_SHIFT)
           | (uhci_td_encode_length(length) << UHCI_TOKEN_MAXLEN_SHIFT);
}

/* Wait for a chain of TDs to complete, optionally stopping at a short packet. */
static int uhci_wait_chain(uhci_controller_t *ctrl, const int *td_indices, const size_t *packet_lengths, size_t count, uint32_t timeout_ms, bool stop_on_short, size_t *actual)
{
    uint64_t deadline    = nano_time() + (uint64_t)timeout_ms * 1000000ULL;
    size_t   transferred = 0;
    for (size_t i = 0; i < count;) {
        if (!ctrl->running) return -ESHUTDOWN;
        uhci_td_t *td     = ctrl->tds[td_indices[i]].virtual;
        int        status = uhci_td_result(td);
        if (status == -EINPROGRESS) {
            if (nano_time() >= deadline) return -ETIMEDOUT;
            __asm__ volatile("pause");
            continue;
        }
        if (status != EOK) return status;
        size_t packet_actual = uhci_td_decode_length(td->control_status);
        if (packet_actual > packet_lengths[i]) return -EPROTO;
        transferred += packet_actual;
        i++;
        if (stop_on_short && packet_actual != packet_lengths[i - 1]) break;
    }
    if (actual) *actual = transferred;
    return EOK;
}

/*
 * Transfer submission
 * Control transfers are built as a chain of TDs (SETUP, DATA,
 * STATUS) on a QH hung from the async list; bulk uses a plain
 * single-phase TD chain on a QH; interrupt uses the periodic list.
 */

/* Submit a control transfer as a SETUP/DATA/STATUS TD chain. */
static int uhci_submit_control(usb_device_t *device, const usb_setup_packet_t *setup, void *buffer, size_t length, uint32_t timeout_ms)
{
    uhci_controller_t *ctrl = device ? device->hc_private : NULL;
    if (!ctrl || !setup || (length && !buffer) || length != usb_get_le16(&setup->length)) return -EINVAL;
    uint16_t max_packet = device->descriptor.max_packet_size0 ? device->descriptor.max_packet_size0 : 8;
    if (max_packet != 8 && max_packet != 16 && max_packet != 32 && max_packet != 64) return -EPROTO;
    size_t data_count = length ? (length + max_packet - 1) / max_packet : 0;
    if (data_count + 2 > UHCI_NUM_TD) return -EMSGSIZE;

    uhci_io_lock(ctrl);
    int      status                      = -ENOMEM;
    int      qh_index                    = -1;
    int      td_indices[UHCI_NUM_TD]     = {0};
    size_t   packet_lengths[UHCI_NUM_TD] = {0};
    size_t   td_count                    = 0;
    void    *setup_dma                   = NULL;
    void    *data_dma                    = NULL;
    uint64_t setup_physical              = 0;
    uint64_t data_physical               = 0;
    bool     input                       = (setup->request_type & USB_DIR_IN) != 0;

    setup_dma = uhci_dma_alloc(sizeof(*setup), &setup_physical);
    if (!setup_dma || setup_physical > UINT32_MAX) {
        plogk("usb-uhci: %s: control setup DMA allocation failed.\n", device->path);
        goto cleanup;
    }
    memcpy(setup_dma, setup, sizeof(*setup));
    if (length) {
        data_dma = uhci_dma_alloc(length, &data_physical);
        if (!data_dma || data_physical > UINT32_MAX || data_physical + length - 1 > UINT32_MAX) {
            plogk("usb-uhci: %s: control data DMA allocation failed (%zu bytes)\n", device->path, length);
            goto cleanup;
        }
        if (!input) memcpy(data_dma, buffer, length);
    }
    qh_index = uhci_find_free_qh(ctrl);
    if (qh_index < 0) {
        plogk("usb-uhci: %s: no free queue head for control transfer.\n", device->path);
        goto cleanup;
    }
    for (size_t i = 0; i < data_count + 2; i++) {
        int index = uhci_find_free_td(ctrl);
        if (index < 0) {
            plogk("usb-uhci: %s: no free transfer descriptor for control transfer.\n", device->path);
            goto cleanup;
        }
        td_indices[td_count++] = index;
    }

    uhci_td_t *setup_td      = ctrl->tds[td_indices[0]].virtual;
    setup_td->control_status = uhci_td_flags(device, false);
    setup_td->token          = uhci_token(UHCI_PID_SETUP, device->address, 0, 0, sizeof(*setup));
    setup_td->buffer         = (uint32_t)setup_physical;
    packet_lengths[0]        = sizeof(*setup);

    size_t  offset = 0;
    uint8_t toggle = 1;
    for (size_t i = 0; i < data_count; i++) {
        size_t packet = length - offset;
        if (packet > max_packet) packet = max_packet;
        uhci_td_t *td         = ctrl->tds[td_indices[i + 1]].virtual;
        td->control_status    = uhci_td_flags(device, false);
        td->token             = uhci_token(input ? UHCI_PID_IN : UHCI_PID_OUT, device->address, 0, toggle, packet);
        td->buffer            = (uint32_t)(data_physical + offset);
        packet_lengths[i + 1] = packet;
        toggle ^= 1;
        offset += packet;
    }

    size_t     status_position      = data_count + 1;
    uhci_td_t *status_td            = ctrl->tds[td_indices[status_position]].virtual;
    uint8_t    status_pid           = length && input ? UHCI_PID_OUT : UHCI_PID_IN;
    status_td->control_status       = uhci_td_flags(device, false) | UHCI_TD_IOC;
    status_td->token                = uhci_token(status_pid, device->address, 0, 1, 0);
    status_td->buffer               = 0;
    packet_lengths[status_position] = 0;

    for (size_t i = 0; i < td_count; i++) {
        uhci_td_t *td = ctrl->tds[td_indices[i]].virtual;
        td->link      = i + 1 < td_count ? (uint32_t)ctrl->tds[td_indices[i + 1]].physical : UHCI_LINK_TERMINATE;
    }
    uhci_qh_t *qh       = ctrl->qhs[qh_index].virtual;
    qh->horizontal_link = UHCI_LINK_TERMINATE;
    qh->element_link    = (uint32_t)ctrl->tds[td_indices[0]].physical;
    dma_write_barrier();
    uhci_schedule_qh(ctrl, qh_index);

    status = uhci_wait_chain(ctrl, td_indices, packet_lengths, td_count, timeout_ms, false, NULL);
    uhci_unschedule_qh(ctrl, qh_index);
    if (status == EOK && input && length) {
        size_t actual = 0;
        for (size_t i = 0; i < data_count; i++) actual += uhci_td_decode_length(ctrl->tds[td_indices[i + 1]].virtual->control_status);
        if (actual > length)
            status = -EPROTO;
        else
            memcpy(buffer, data_dma, actual);
    }
cleanup:
    if (qh_index >= 0) uhci_free_qh(ctrl, qh_index);
    for (size_t i = 0; i < td_count; i++) uhci_free_td(ctrl, td_indices[i]);
    if (setup_dma) uhci_dma_free(setup_physical, sizeof(*setup));
    if (data_dma) uhci_dma_free(data_physical, length);
    uhci_io_unlock(ctrl);
    return status;
}

/* Bulk transfer: chain of data TDs, optionally stopping at a short packet */
static int uhci_submit_bulk(usb_endpoint_t *endpoint, void *buffer, size_t length, size_t *actual, uint32_t timeout_ms)
{
    if (!endpoint || !endpoint->interface || !endpoint->interface->device || (length && !buffer)) return -EINVAL;
    if (actual) *actual = 0;
    if (!length) return EOK;
    usb_device_t      *device     = endpoint->interface->device;
    uhci_controller_t *ctrl       = device->hc_private;
    uint16_t           max_packet = usb_get_le16(&endpoint->descriptor.max_packet_size) & 0x07ff;
    if (!ctrl || !max_packet || max_packet > 64) return -EINVAL;
    size_t td_count = (length + max_packet - 1) / max_packet;
    if (td_count > UHCI_NUM_TD) return -EMSGSIZE;

    uhci_io_lock(ctrl);
    int      status                      = -ENOMEM;
    int      qh_index                    = -1;
    int      td_indices[UHCI_NUM_TD]     = {0};
    size_t   packet_lengths[UHCI_NUM_TD] = {0};
    size_t   allocated                   = 0;
    uint64_t dma_physical                = 0;
    void    *dma_buffer                  = uhci_dma_alloc(length, &dma_physical);
    bool     input                       = (endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK) != 0;
    uint8_t  endpoint_number             = endpoint->descriptor.endpoint_address & USB_ENDPOINT_NUMBER_MASK;
    if (!dma_buffer || dma_physical > UINT32_MAX || dma_physical + length - 1 > UINT32_MAX) {
        plogk("usb-uhci: Transfer DMA allocation failed on bus %u (%zu bytes)\n", ctrl->bus_number, length);
        goto cleanup_bulk;
    }
    if (!input) memcpy(dma_buffer, buffer, length);
    qh_index = uhci_find_free_qh(ctrl);
    if (qh_index < 0) {
        plogk("usb-uhci: QH pool exhausted on bus %u\n", ctrl->bus_number);
        goto cleanup_bulk;
    }
    for (size_t i = 0; i < td_count; i++) {
        int index = uhci_find_free_td(ctrl);
        if (index < 0) {
            plogk("usb-uhci: TD pool exhausted on bus %u\n", ctrl->bus_number);
            goto cleanup_bulk;
        }
        td_indices[allocated++] = index;
    }

    size_t  offset = 0;
    uint8_t toggle = endpoint->data_toggle;
    for (size_t i = 0; i < td_count; i++) {
        size_t packet = length - offset;
        if (packet > max_packet) packet = max_packet;
        uhci_td_t *td      = ctrl->tds[td_indices[i]].virtual;
        td->control_status = uhci_td_flags(device, input) | (i + 1 == td_count ? UHCI_TD_IOC : 0);
        td->token          = uhci_token(input ? UHCI_PID_IN : UHCI_PID_OUT, device->address, endpoint_number, toggle, packet);
        td->buffer         = (uint32_t)(dma_physical + offset);
        td->link           = i + 1 < td_count ? (uint32_t)ctrl->tds[td_indices[i + 1]].physical : UHCI_LINK_TERMINATE;
        packet_lengths[i]  = packet;
        toggle ^= 1;
        offset += packet;
    }
    uhci_qh_t *qh       = ctrl->qhs[qh_index].virtual;
    qh->horizontal_link = UHCI_LINK_TERMINATE;
    qh->element_link    = (uint32_t)ctrl->tds[td_indices[0]].physical;
    dma_write_barrier();
    uhci_schedule_qh(ctrl, qh_index);

    size_t transferred = 0;
    status             = uhci_wait_chain(ctrl, td_indices, packet_lengths, td_count, timeout_ms, input, &transferred);
    uhci_unschedule_qh(ctrl, qh_index);
    if (status == EOK) {
        size_t completed = 0;
        while (completed < td_count && !(ctrl->tds[td_indices[completed]].virtual->control_status & UHCI_TD_ACTIVE)) completed++;
        endpoint->data_toggle ^= completed & 1;
        if (actual) *actual = transferred;
        if (input && transferred) memcpy(buffer, dma_buffer, transferred);
    }
cleanup_bulk:
    if (qh_index >= 0) uhci_free_qh(ctrl, qh_index);
    for (size_t i = 0; i < allocated; i++) uhci_free_td(ctrl, td_indices[i]);
    if (dma_buffer) uhci_dma_free(dma_physical, length);
    uhci_io_unlock(ctrl);
    return status;
}

/* Register a periodic interrupt-IN transfer for the worker to service. */
static int uhci_submit_interrupt(usb_endpoint_t *endpoint, size_t length, usb_interrupt_complete_t complete, void *context)
{
    if (!endpoint || !endpoint->interface || !endpoint->interface->device || !length || length > PAGE_4K_SIZE || !complete) return -EINVAL;
    if ((endpoint->descriptor.attributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_INT || !(endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK)) return -EINVAL;
    if (endpoint->hc_private) return -EBUSY;
    uhci_controller_t *ctrl = endpoint->interface->device->hc_private;
    if (!ctrl) return -ENODEV;
    uhci_periodic_transfer_t *transfer = calloc(1, sizeof(*transfer));
    if (!transfer) return -ENOMEM;
    transfer->buffer = malloc(length);
    if (!transfer->buffer) {
        plogk("usb-uhci: Interrupt transfer buffer allocation failed on bus %u (%zu bytes)\n", ctrl->bus_number, length);
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
    uint64_t flags        = spin_lock_irqsave(&ctrl->lock);
    for (size_t i = 0; i < UHCI_MAX_PERIODIC; i++) {
        if (!ctrl->periodic[i]) {
            ctrl->periodic[i]    = transfer;
            endpoint->hc_private = transfer;
            spin_unlock_irqrestore(&ctrl->lock, flags);
            return EOK;
        }
    }
    spin_unlock_irqrestore(&ctrl->lock, flags);
    plogk("usb-uhci: Periodic transfer pool exhausted on bus %u\n", ctrl->bus_number);
    free(transfer->buffer);
    free(transfer);
    return -ENOSPC;
}

/* Unregister a periodic transfer and wait out any running callback. */
static void uhci_interrupt_stop(usb_endpoint_t *endpoint)
{
    uhci_periodic_transfer_t *transfer = endpoint ? endpoint->hc_private : NULL;
    if (!transfer || !endpoint->interface || !endpoint->interface->device) return;
    uhci_controller_t *ctrl  = endpoint->interface->device->hc_private;
    uint64_t           flags = spin_lock_irqsave(&ctrl->lock);
    transfer->active         = false;
    for (size_t i = 0; i < UHCI_MAX_PERIODIC; i++)
        if (ctrl->periodic[i] == transfer) ctrl->periodic[i] = NULL;
    endpoint->hc_private = NULL;
    spin_unlock_irqrestore(&ctrl->lock, flags);
    while (__atomic_load_n(&transfer->in_callback, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    free(transfer->buffer);
    free(transfer);
}

/* Initialize per-endpoint state before it is used. */
static int uhci_configure_endpoint(usb_endpoint_t *endpoint)
{
    if (!endpoint) return -EINVAL;
    endpoint->data_toggle = 0;
    return EOK;
}

/* Reset the endpoint's data-toggle state after a stall. */
static int uhci_clear_halt(usb_endpoint_t *endpoint)
{
    if (!endpoint) return -EINVAL;
    endpoint->data_toggle = 0;
    return EOK;
}

/* Stop every interrupt transfer of a device before it goes away. */
static void uhci_disable_device(usb_device_t *device)
{
    if (!device) return;
    for (size_t i = 0; i < device->interface_count; i++)
        for (size_t j = 0; j < device->interfaces[i].endpoint_count; j++) uhci_interrupt_stop(&device->interfaces[i].endpoints[j]);
}

static const usb_hcd_ops_t uhci_hcd_ops = {
    .control            = uhci_submit_control,
    .transfer           = uhci_submit_bulk,
    .interrupt_start    = uhci_submit_interrupt,
    .interrupt_stop     = uhci_interrupt_stop,
    .configure_endpoint = uhci_configure_endpoint,
    .disable_endpoint   = uhci_interrupt_stop,
    .clear_halt         = uhci_clear_halt,
    .disable_device     = uhci_disable_device,
};

/* Perform the UHCI port reset sequence. */
static int uhci_port_reset(uhci_controller_t *ctrl, uint8_t port)
{
    if (port >= UHCI_MAX_PORTS) return -EINVAL;
    uint16_t portsc = uhci_readw(ctrl, UHCI_PORTSC1 + port * 2);
    if (!(portsc & UHCI_PORTSC_CCS)) return -ENODEV;
    uhci_writew(ctrl, UHCI_PORTSC1 + port * 2, portsc | UHCI_PORTSC_PR);
    msleep(50);
    portsc = uhci_readw(ctrl, UHCI_PORTSC1 + port * 2);
    uhci_writew(ctrl, UHCI_PORTSC1 + port * 2, portsc & ~UHCI_PORTSC_PR);
    msleep(10);
    portsc = uhci_readw(ctrl, UHCI_PORTSC1 + port * 2);
    if (!(portsc & UHCI_PORTSC_CCS)) return -ENODEV;
    uhci_writew(ctrl, UHCI_PORTSC1 + port * 2, portsc | UHCI_PORTSC_PED);
    uint64_t deadline = nano_time() + 100 * 1000000ULL;
    do {
        portsc = uhci_readw(ctrl, UHCI_PORTSC1 + port * 2);
        if (!(portsc & UHCI_PORTSC_CCS)) return -ENODEV;
        if (portsc & UHCI_PORTSC_PED) {
            uhci_writew(ctrl, UHCI_PORTSC1 + port * 2, portsc);
            return EOK;
        }
        __asm__ volatile("pause");
    } while (nano_time() < deadline);
    return -ETIMEDOUT;
}

/* Classify a port's device speed from the status word. */
static usb_speed_t uhci_port_speed(uint16_t portsc)
{
    return (portsc & UHCI_PORTSC_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

/* Device-model release callback: free the usb_device. */
static void uhci_usb_device_release(struct device *dev)
{
    usb_device_t *device = container_of(dev, usb_device_t, dev);
    free(device);
}

/* Reset a port, probe the attached device and register it with the USB core */
static int uhci_enumerate_port(uhci_controller_t *ctrl, uint8_t port)
{
    if (!ctrl || port >= UHCI_MAX_PORTS) return -EINVAL;
    if (ctrl->devices[port]) return -EEXIST;
    int result = uhci_port_reset(ctrl, port);
    if (result != EOK) return result;

    uint16_t    portsc = uhci_readw(ctrl, UHCI_PORTSC1 + port * 2);
    usb_speed_t speed  = uhci_port_speed(portsc);

    usb_device_t *device = calloc(1, sizeof(*device));
    if (!device) return -ENOMEM;
    device->connected   = true;
    device->speed       = speed;
    device->bus_number  = ctrl->bus_number;
    device->port_number = port + 1;
    device->hcd_ops     = &uhci_hcd_ops;
    device->hc_private  = ctrl;
    device->dev.release = uhci_usb_device_release;
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
    usb_get_string_descriptor(device, device->descriptor.manufacturer, language, device->manufacturer, sizeof(device->manufacturer));
    usb_get_string_descriptor(device, device->descriptor.product, language, device->product, sizeof(device->product));
    usb_get_string_descriptor(device, device->descriptor.serial_number, language, device->serial, sizeof(device->serial));

    uint8_t *config        = NULL;
    uint16_t config_length = 0;
    result                 = usb_read_config_descriptor(device, &config, &config_length);
    if (result == EOK) result = usb_add_device(device, config, config_length);
    free(config);
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
static void uhci_disconnect_port(uhci_controller_t *ctrl, uint8_t port)
{
    if (!ctrl || port >= UHCI_MAX_PORTS) return;
    usb_device_t *device = ctrl->devices[port];
    if (!device) return;
    ctrl->devices[port] = NULL;
    usb_remove_device(device);
}

/* Poll every due periodic transfer and deliver its results. */
static void uhci_service_periodic(uhci_controller_t *ctrl)
{
    uint64_t now = nano_time();
    for (size_t i = 0; i < UHCI_MAX_PERIODIC; i++) {
        uint64_t                  flags    = spin_lock_irqsave(&ctrl->lock);
        uhci_periodic_transfer_t *transfer = ctrl->periodic[i];
        if (!transfer || !transfer->active || transfer->in_callback || now < transfer->next_poll) {
            spin_unlock_irqrestore(&ctrl->lock, flags);
            continue;
        }
        transfer->in_callback = true;
        spin_unlock_irqrestore(&ctrl->lock, flags);
        size_t actual = 0;
        uint32_t timeout = transfer->interval_ms ? transfer->interval_ms : 10;
        if (timeout > 100) timeout = 100;
        int status = uhci_submit_bulk(transfer->endpoint, transfer->buffer, transfer->length, &actual, timeout);
        if (status == -ETIMEDOUT) { status = EOK; actual = 0; }
        if (__atomic_load_n(&transfer->active, __ATOMIC_ACQUIRE) && (actual || status != EOK)) transfer->complete(transfer->endpoint, transfer->buffer, actual, status, transfer->context);
        else if (__atomic_load_n(&transfer->active, __ATOMIC_ACQUIRE) && status == EOK && !actual) {
            // No data, still need to keep polling timely; no callback
        }
        transfer->next_poll = nano_time() + (uint64_t)transfer->interval_ms * 1000000ULL;
        __atomic_store_n(&transfer->in_callback, false, __ATOMIC_RELEASE);
    }
}

/* Hub worker: handle port changes, enumerate devices, poll periodic. */
static int uhci_worker(void *argument)
{
    uhci_controller_t *ctrl = argument;
    while (ctrl->running && !kthread_should_stop()) {
        uint64_t flags      = spin_lock_irqsave(&ctrl->lock);
        uint64_t ports      = ctrl->pending_ports;
        ctrl->pending_ports = 0;
        if (!ports && ctrl->running && !kthread_should_stop()) wait_queue_prepare(&ctrl->worker_wait);
        spin_unlock_irqrestore(&ctrl->lock, flags);
        for (uint8_t p = 0; ports && p < UHCI_MAX_PORTS; p++) {
            if (ports & (1ULL << p)) {
                uint16_t portsc = uhci_readw(ctrl, UHCI_PORTSC1 + p * 2);
                uint16_t clear  = portsc & (UHCI_PORTSC_CSC | UHCI_PORTSC_PEC);
                if (clear) uhci_writew(ctrl, UHCI_PORTSC1 + p * 2, portsc);
                if (!(portsc & UHCI_PORTSC_CCS)) {
                    if (ctrl->devices[p]) uhci_disconnect_port(ctrl, p);
                } else if (!ctrl->devices[p] || (portsc & UHCI_PORTSC_CSC)) {
                    if (ctrl->devices[p]) uhci_disconnect_port(ctrl, p);
                    msleep(100);
                    int ret = uhci_enumerate_port(ctrl, p);
                    if (ret != EOK) plogk("usb-uhci: Port %u enumeration failed: %d\n", p, ret);
                }
            }
        }
        uhci_service_periodic(ctrl);

        if (!ports && ctrl->running && !kthread_should_stop()) wait_queue_wait_timed(&ctrl->worker_wait, sched_ticks() + timer_ns_to_ticks_ceil(1000000ULL));
    }
    return 0;
}

/* ISR: acknowledge status bits and flag port-change work. */
INTERRUPT_BEGIN static void uhci_interrupt_handler(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    (void)frame;
    for (size_t i = 0; i < uhci_controller_count; i++) {
        uhci_controller_t *ctrl = uhci_controllers[i];
        if (!ctrl || !ctrl->running) continue;
        uint16_t sts = uhci_readw(ctrl, UHCI_USBSTS);
        if (sts & UHCI_STS_USBINT) uhci_writew(ctrl, UHCI_USBSTS, UHCI_STS_USBINT);
        if (sts & UHCI_STS_ERROR) {
            uhci_writew(ctrl, UHCI_USBSTS, UHCI_STS_ERROR);
            plogk("usb-uhci: USB error interrupt on bus %u\n", ctrl->bus_number);
        }
        if (sts & UHCI_STS_RD) {
            uhci_writew(ctrl, UHCI_USBSTS, UHCI_STS_RD);
            uint64_t flags = spin_lock_irqsave(&ctrl->lock);
            for (uint8_t p = 0; p < UHCI_MAX_PORTS; p++) {
                uint16_t psc = uhci_readw(ctrl, UHCI_PORTSC1 + p * 2);
                if (psc & (UHCI_PORTSC_CSC | UHCI_PORTSC_PEC)) ctrl->pending_ports |= 1ULL << p;
            }
            spin_unlock_irqrestore(&ctrl->lock, flags);
            if (ctrl->worker_started) wait_queue_wake_one(&ctrl->worker_wait);
        }
        if (sts & UHCI_STS_HSE) {
            uhci_writew(ctrl, UHCI_USBSTS, UHCI_STS_HSE);
            plogk("usb-uhci: Host system error on bus %u\n", ctrl->bus_number);
        }
    }
    send_eoi();
    irq_leave_gs(frame);
}
INTERRUPT_END

/* host_ops: stop the controller and its interrupts. */
static void uhci_host_stop(usb_host_t *host)
{
    uhci_controller_t *ctrl = container_of(host, uhci_controller_t, hcd);
    ctrl->running           = false;
    wait_queue_wake_all(&ctrl->worker_wait);
    uhci_writew(ctrl, UHCI_USBCMD, 0);
    uhci_writew(ctrl, UHCI_USBINTR, 0);
}

static usb_host_controller_ops_t uhci_controller_ops = {
    .host_stop = uhci_host_stop,
};

/* Probe a UHCI PCI device: reset, allocate pools, and start. */
static int uhci_probe(pci_device_cache_t *pci, uint8_t bus_number)
{
    uint16_t io_base = (uint16_t)pci_get_port_base(pci);
    if (!io_base) return -ENODEV;

    uhci_controller_t *ctrl = calloc(1, sizeof(*ctrl));
    if (!ctrl) return -ENOMEM;

    ctrl->io_base            = io_base;
    ctrl->pci                = pci;
    ctrl->bus_number         = bus_number;
    ctrl->hcd.type           = USB_HOST_UHCI;
    ctrl->hcd.bus_number     = bus_number;
    ctrl->hcd.pci_dev        = pci;
    ctrl->hcd.hcd_ops        = &uhci_hcd_ops;
    ctrl->hcd.controller_ops = &uhci_controller_ops;
    ctrl->hcd.hc_private     = ctrl;
    ctrl->hcd.max_ports      = UHCI_MAX_PORTS;
    (void)snprintf(ctrl->hcd.name, sizeof(ctrl->hcd.name), "uhci-usb%u", bus_number);

    uint32_t command = pci_read_command_status(pci) & 0xffff;
    pci_write_command_status(pci, command | 0x05);

    uhci_writew(ctrl, UHCI_USBCMD, UHCI_CMD_HCRESET);
    uint64_t deadline = nano_time() + UHCI_RESET_TIMEOUT_MS * 1000000ULL;
    while (uhci_readw(ctrl, UHCI_USBCMD) & UHCI_CMD_HCRESET) {
        if (nano_time() >= deadline) {
            free(ctrl);
            return -ETIMEDOUT;
        }
        __asm__ volatile("pause");
    }

    ctrl->frame_list_virtual = uhci_dma_alloc(PAGE_4K_SIZE, &ctrl->frame_list_physical);
    if (!ctrl->frame_list_virtual) {
        free(ctrl);
        return -ENOMEM;
    }

    for (int i = 0; i < UHCI_NUM_TD; i++) {
        uint64_t phys;
        ctrl->tds[i].virtual = uhci_dma_alloc(sizeof(uhci_td_t), &phys);
        if (!ctrl->tds[i].virtual) {
            for (int j = 0; j < i; j++) uhci_dma_free(ctrl->tds[j].physical, sizeof(uhci_td_t));
            uhci_dma_free(ctrl->frame_list_physical, PAGE_4K_SIZE);
            free(ctrl);
            return -ENOMEM;
        }
        ctrl->tds[i].physical = phys;
    }
    for (int i = 0; i < UHCI_NUM_QH; i++) {
        uint64_t phys;
        ctrl->qhs[i].virtual = uhci_dma_alloc(sizeof(uhci_qh_t), &phys);
        if (!ctrl->qhs[i].virtual) {
            for (int j = 0; j < i; j++) uhci_dma_free(ctrl->qhs[j].physical, sizeof(uhci_qh_t));
            for (int j = 0; j < UHCI_NUM_TD; j++) uhci_dma_free(ctrl->tds[j].physical, sizeof(uhci_td_t));
            uhci_dma_free(ctrl->frame_list_physical, PAGE_4K_SIZE);
            free(ctrl);
            return -ENOMEM;
        }
        ctrl->qhs[i].physical                 = phys;
        ctrl->qhs[i].virtual->horizontal_link = UHCI_LINK_TERMINATE;
        ctrl->qhs[i].virtual->element_link    = UHCI_LINK_TERMINATE;
    }

    for (size_t frame = 0; frame < UHCI_FRAME_LIST_SIZE; frame++) ctrl->frame_list_virtual[frame] = UHCI_LINK_TERMINATE;
    dma_write_barrier();
    uhci_writel(ctrl, UHCI_FLBASEADD, (uint32_t)ctrl->frame_list_physical);

    uhci_writew(ctrl, UHCI_SOFMOD, 64);
    uhci_writew(ctrl, UHCI_USBCMD, UHCI_CMD_RS | UHCI_CMD_MAXP | UHCI_CMD_CF);
    uhci_writew(ctrl, UHCI_USBINTR, UHCI_INTR_IOC | UHCI_INTR_RESUME | UHCI_INTR_TIMEOUT | UHCI_INTR_SP);

    uint32_t irq = pci_get_irq(pci);
    ctrl->vector = (int)irq;
    pci_msi_init(pci);
    int msi_vector = pci_enable_msi(pci);
    if (msi_vector >= 0) ctrl->vector = msi_vector;
    if (ctrl->vector > 0) register_interrupt_handler((uint16_t)ctrl->vector, uhci_interrupt_handler, 0, 0x8e);

    wait_queue_init(&ctrl->worker_wait);
    ctrl->running                             = true;
    uhci_controllers[uhci_controller_count++] = ctrl;
    usb_host_register(&ctrl->hcd);
    ctrl->hcd.running = true;

    plogk("usb-uhci: Controller at I/O 0x%04x, bus usb%u\n", io_base, bus_number);
    return EOK;
}

/* Probe every UHCI controller in the PCI device cache. */
int uhci_init(void)
{
#if !CONFIG_USB_UHCI
    return 0;
#endif
    size_t               before = uhci_controller_count;
    pci_devices_cache_t *cache  = pci_get_devices_cache();
    if (!cache) return 0;
    for (pci_device_cache_t *pci = cache->head; pci && uhci_controller_count < UHCI_MAX_CONTROLLERS; pci = pci->next) {
        if (pci->class_code != UHCI_PCI_CLASS) continue;
        int bus_number = usb_host_allocate_bus_number();
        if (bus_number < 0) break;
        (void)uhci_probe(pci, (uint8_t)bus_number);
    }
    return (int)(uhci_controller_count - before);
}

void uhci_start_workers(void)
{
#if !CONFIG_USB_UHCI
    return;
#endif
    for (size_t i = 0; i < uhci_controller_count; i++) {
        uhci_controller_t *ctrl = uhci_controllers[i];
        if (!ctrl || ctrl->worker_started) continue;
        ctrl->worker_started = true;

        /* Enumerate every root port on the first worker pass. */
        ctrl->pending_ports |= (1ULL << UHCI_MAX_PORTS) - 1;
        kernel_worker_register("uhci-hub", uhci_worker, ctrl, &ctrl->worker_task);
    }
}
