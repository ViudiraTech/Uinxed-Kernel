/*
 *
 *      ehci.c
 *      Enhanced Host Controller Interface (EHCI) driver
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <drivers/bus/pci.h>
#include <drivers/firmware/apic.h>
#include <drivers/usb/host/ehci/ehci.h>
#include <drivers/usb/host/host.h>
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

#define EHCI_MAX_CONTROLLERS 8
#define EHCI_MAX_PORTS       15
#define EHCI_NUM_QH          32
#define EHCI_NUM_QTD         64
#define EHCI_MAX_PERIODIC    32

typedef struct ehci_periodic_transfer {
        usb_endpoint_t          *endpoint;
        usb_interrupt_complete_t complete;
        void                    *context;
        void                    *buffer;
        size_t                   length;
        uint64_t                 next_poll;
        uint32_t                 interval_ms;
        volatile bool            active;
        volatile bool            in_callback;
} ehci_periodic_transfer_t;

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

        ehci_qh_phys_t            qhs[EHCI_NUM_QH];
        ehci_qtd_phys_t           qtds[EHCI_NUM_QTD];
        usb_device_t             *devices[EHCI_MAX_PORTS];
        ehci_periodic_transfer_t *periodic[EHCI_MAX_PERIODIC];
        uint64_t                  pending_ports;
        spinlock_t                lock;
        volatile bool             io_busy;
        wait_queue_t              worker_wait;
        task_t                   *worker_task;
        bool                      worker_started;
        bool                      running;
} ehci_controller_t;

static ehci_controller_t *ehci_controllers[EHCI_MAX_CONTROLLERS];
static size_t             ehci_controller_count;

/* Read a 32-bit MMIO register. */
static inline uint32_t ehci_read32(const volatile uint8_t *base, size_t offset)
{
    return *(volatile const uint32_t *)(base + offset);
}

/* Write a 32-bit MMIO register. */
static inline void ehci_write32(volatile uint8_t *base, size_t offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

/* Allocate zeroed DMA memory and return its physical address. */
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

/* Release DMA memory allocated by ehci_dma_alloc(). */
static void ehci_dma_free(uint64_t physical, size_t size)
{
    size_t count = (size + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    if (physical && count) free_frames(physical, count);
}

/* Poll a register until the masked bits match value, with timeout. */
static int ehci_wait_register(volatile uint8_t *base, size_t offset, uint32_t mask, uint32_t value, uint32_t timeout_ms)
{
    uint64_t deadline = nano_time() + (uint64_t)timeout_ms * 1000000ULL;
    while ((ehci_read32(base, offset) & mask) != value) {
        if (nano_time() >= deadline) return -ETIMEDOUT;
        __asm__ volatile("pause");
    }
    return EOK;
}

/* Claim a free queue-head slot from the static pool. */
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

/* Return a queue-head slot to the pool. */
static void ehci_free_qh(ehci_controller_t *ctrl, int index)
{
    if (index < 0 || index >= EHCI_NUM_QH) return;
    ctrl->qhs[index].used = false;
    memset(ctrl->qhs[index].virtual, 0, sizeof(ehci_qh_t));
}

/* Claim a free qTD slot from the static pool. */
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

/* Return a qTD slot to the pool. */
static void ehci_free_qtd(ehci_controller_t *ctrl, int index)
{
    if (index < 0 || index >= EHCI_NUM_QTD) return;
    ctrl->qtds[index].used = false;
    memset(ctrl->qtds[index].virtual, 0, sizeof(ehci_qtd_t));
}

/* Spin until the controller's IO is exclusively owned by a transfer. */
static void ehci_io_lock(ehci_controller_t *ctrl)
{
    while (__atomic_test_and_set(&ctrl->io_busy, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
}

/* Release the exclusive IO lock. */
static void ehci_io_unlock(ehci_controller_t *ctrl)
{
    __atomic_clear(&ctrl->io_busy, __ATOMIC_RELEASE);
}

/* Translate a qTD token into a completion status. */
static int ehci_qtd_result(const ehci_qtd_t *qtd)
{
    uint32_t token = qtd->token;
    if (token & EHCI_QTD_ACTIVE) return -EINPROGRESS;
    if (token & EHCI_QTD_BABBLE) return -EOVERFLOW;
    if (token & EHCI_QTD_DBE) return -EPROTO;
    if (token & (EHCI_QTD_XACTERR | EHCI_QTD_MISSED)) return -EPROTO;
    if (token & EHCI_QTD_HALTED) return -EPIPE;
    return EOK;
}

/* Wait for every qTD in a chain to complete, in order. */
static int ehci_wait_chain(ehci_controller_t *ctrl, const int *qtd_indices, size_t count, uint32_t timeout_ms)
{
    uint64_t deadline = nano_time() + (uint64_t)timeout_ms * 1000000ULL;
    for (size_t position = 0; position < count; position++) {
        for (;;) {
            if (!ctrl->running) return -ESHUTDOWN;
            int status = ehci_qtd_result(ctrl->qtds[qtd_indices[position]].virtual);
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

/* Populate a qTD with the token, buffer, and next-link fields. */
static void ehci_fill_qtd(ehci_qtd_t *qtd, uint32_t pid, uint32_t toggle, uint32_t buffer, size_t length, uint32_t next, bool ioc)
{
    qtd->next_qtd     = next;
    qtd->alt_next_qtd = EHCI_QTD_NEXT_TERMINATE;
    qtd->token        = EHCI_QTD_ACTIVE | (3U << EHCI_QTD_CERR_SHIFT) | pid | toggle | ((uint32_t)length << EHCI_QTD_LENGTH_SHIFT) | (ioc ? EHCI_QTD_IOC : 0);
    qtd->buffer[0]    = length ? buffer : 0;
    for (size_t i = 1; i < 5; i++) qtd->buffer[i] = 0;
}

/* Build a QH endpoint-characteristics dword from the endpoint. */
static uint32_t ehci_qh_characteristics(const usb_device_t *device, uint8_t endpoint, uint16_t max_packet)
{
    return (uint32_t)device->address << EHCI_QH_FA_SHIFT | (uint32_t)endpoint << EHCI_QH_EN_SHIFT | EHCI_QH_EPS_HIGH | EHCI_QH_DTC | ((uint32_t)max_packet << EHCI_QH_MPL_SHIFT) | EHCI_QH_H;
}

/* Put a QH on the async list and wait for the controller to start. */
static int ehci_schedule_async(ehci_controller_t *ctrl, int qh_index)
{
    ehci_qh_t *qh       = ctrl->qhs[qh_index].virtual;
    qh->horizontal_link = (uint32_t)ctrl->qhs[qh_index].physical | EHCI_QHL_TYPE_QH;
    dma_write_barrier();
    ehci_write32(ctrl->operational, EHCI_OP_ASYNCLISTADDR, (uint32_t)ctrl->qhs[qh_index].physical);
    uint32_t command = ehci_read32(ctrl->operational, EHCI_OP_USBCMD) | EHCI_CMD_ASENE;
    ehci_write32(ctrl->operational, EHCI_OP_USBCMD, command);
    return ehci_wait_register(ctrl->operational, EHCI_OP_USBSTS, EHCI_STS_ASS, EHCI_STS_ASS, 100);
}

/* Remove a QH from the async list. */
static void ehci_unschedule_async(ehci_controller_t *ctrl, int qh_index)
{
    ctrl->qhs[qh_index].virtual->next_qtd = EHCI_QTD_NEXT_TERMINATE;
    dma_write_barrier();
    uint32_t command = ehci_read32(ctrl->operational, EHCI_OP_USBCMD) & ~EHCI_CMD_ASENE;
    ehci_write32(ctrl->operational, EHCI_OP_USBCMD, command);
    (void)ehci_wait_register(ctrl->operational, EHCI_OP_USBSTS, EHCI_STS_ASS, 0, 100);
    ehci_write32(ctrl->operational, EHCI_OP_ASYNCLISTADDR, 0);
}

/*
 * Transfer submission
 * Control transfers are built as a chain of qTDs under a QH on the
 * async list (SETUP / DATA / STATUS); bulk and interrupt transfers
 * use the same qTD mechanism on their own QHs.
 */

/* Submit a control transfer as a SETUP/DATA/STATUS qTD chain. */
static int ehci_control(usb_device_t *device, const usb_setup_packet_t *setup, void *buffer, size_t length, uint32_t timeout_ms)
{
    ehci_controller_t *ctrl = device ? device->hc_private : NULL;
    if (!ctrl || !setup || device->speed != USB_SPEED_HIGH || (length && !buffer) || length > PAGE_4K_SIZE || length != usb_get_le16(&setup->length)) return -EINVAL;
    uint16_t max_packet = device->descriptor.max_packet_size0 ? device->descriptor.max_packet_size0 : 64;
    if (max_packet != 64) return -EPROTO;
    bool input = (setup->request_type & USB_DIR_IN) != 0;

    ehci_io_lock(ctrl);
    int      status         = -ENOMEM;
    int      qh_index       = -1;
    int      qtd_indices[3] = {-1, -1, -1};
    size_t   qtd_count      = length ? 3 : 2;
    uint64_t setup_physical = 0, data_physical = 0;
    void    *setup_dma = ehci_dma_alloc(sizeof(*setup), &setup_physical);
    void    *data_dma  = NULL;
    if (!setup_dma || setup_physical > UINT32_MAX) {
        plogk("usb-ehci: Control DMA allocation failed on bus %u\n", ctrl->bus_number);
        goto control_cleanup;
    }
    memcpy(setup_dma, setup, sizeof(*setup));
    if (length) {
        data_dma = ehci_dma_alloc(length, &data_physical);
        if (!data_dma || data_physical > UINT32_MAX || data_physical + length - 1 > UINT32_MAX) {
            plogk("usb-ehci: Control data DMA allocation failed on bus %u (%zu bytes)\n", ctrl->bus_number, length);
            goto control_cleanup;
        }
        if (!input) memcpy(data_dma, buffer, length);
    }
    qh_index = ehci_find_free_qh(ctrl);
    if (qh_index < 0) {
        plogk("usb-ehci: QH pool exhausted on bus %u\n", ctrl->bus_number);
        goto control_cleanup;
    }
    for (size_t i = 0; i < qtd_count; i++) {
        qtd_indices[i] = ehci_find_free_qtd(ctrl);
        if (qtd_indices[i] < 0) {
            plogk("usb-ehci: QTD pool exhausted on bus %u\n", ctrl->bus_number);
            goto control_cleanup;
        }
    }
    size_t status_position = length ? 2 : 1;
    ehci_fill_qtd(ctrl->qtds[qtd_indices[0]].virtual, EHCI_QTD_PID_SETUP, 0, (uint32_t)setup_physical, sizeof(*setup), (uint32_t)ctrl->qtds[qtd_indices[1]].physical, false);
    if (length)
        ehci_fill_qtd(ctrl->qtds[qtd_indices[1]].virtual, input ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT, EHCI_QTD_TOGGLE, (uint32_t)data_physical, length, (uint32_t)ctrl->qtds[qtd_indices[2]].physical,
                      false);
    if (length) ctrl->qtds[qtd_indices[1]].virtual->alt_next_qtd = (uint32_t)ctrl->qtds[qtd_indices[2]].physical;
    ehci_fill_qtd(ctrl->qtds[qtd_indices[status_position]].virtual, input && length ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN, EHCI_QTD_TOGGLE, 0, 0, EHCI_QTD_NEXT_TERMINATE, true);

    ehci_qh_t *qh      = ctrl->qhs[qh_index].virtual;
    qh->endpoint_chars = ehci_qh_characteristics(device, 0, max_packet);
    qh->endpoint_caps  = 1U << EHCI_QH_MUL_SHIFT;
    qh->current_qtd    = 0;
    qh->next_qtd       = (uint32_t)ctrl->qtds[qtd_indices[0]].physical;
    qh->alt_next_qtd   = EHCI_QTD_NEXT_TERMINATE;
    qh->token          = 0;
    status             = ehci_schedule_async(ctrl, qh_index);
    if (status == EOK) status = ehci_wait_chain(ctrl, qtd_indices, qtd_count, timeout_ms);
    ehci_unschedule_async(ctrl, qh_index);
    if (status == EOK && input && length) {
        size_t remaining = (ctrl->qtds[qtd_indices[1]].virtual->token & EHCI_QTD_LENGTH_MASK) >> EHCI_QTD_LENGTH_SHIFT;
        if (remaining > length)
            status = -EPROTO;
        else
            memcpy(buffer, data_dma, length - remaining);
    }
control_cleanup:
    if (qh_index >= 0) ehci_free_qh(ctrl, qh_index);
    for (size_t i = 0; i < qtd_count; i++)
        if (qtd_indices[i] >= 0) ehci_free_qtd(ctrl, qtd_indices[i]);
    if (setup_dma) ehci_dma_free(setup_physical, sizeof(*setup));
    if (data_dma) ehci_dma_free(data_physical, length);
    ehci_io_unlock(ctrl);
    return status;
}

/* Bulk/interrupt transfer on an endpoint's QH */
static int ehci_transfer(usb_endpoint_t *endpoint, void *buffer, size_t length, size_t *actual, uint32_t timeout_ms)
{
    if (!endpoint || !endpoint->interface || !endpoint->interface->device || (length && !buffer) || length > PAGE_4K_SIZE) return -EINVAL;
    if (actual) *actual = 0;
    if (!length) return EOK;
    usb_device_t      *device     = endpoint->interface->device;
    ehci_controller_t *ctrl       = device->hc_private;
    uint16_t           max_packet = usb_get_le16(&endpoint->descriptor.max_packet_size) & 0x07ff;
    if (!ctrl || device->speed != USB_SPEED_HIGH || !max_packet || max_packet > 1024) return -EINVAL;
    bool    input           = (endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK) != 0;
    uint8_t endpoint_number = endpoint->descriptor.endpoint_address & USB_ENDPOINT_NUMBER_MASK;

    ehci_io_lock(ctrl);
    int      status   = -ENOMEM;
    int      qh_index = -1, qtd_index = -1;
    uint64_t data_physical = 0;
    void    *data_dma      = ehci_dma_alloc(length, &data_physical);
    if (!data_dma || data_physical > UINT32_MAX || data_physical + length - 1 > UINT32_MAX) {
        plogk("usb-ehci: Transfer DMA allocation failed on bus %u (%zu bytes)\n", ctrl->bus_number, length);
        goto transfer_cleanup;
    }
    if (!input) memcpy(data_dma, buffer, length);
    qh_index  = ehci_find_free_qh(ctrl);
    qtd_index = ehci_find_free_qtd(ctrl);
    if (qh_index < 0 || qtd_index < 0) {
        plogk("usb-ehci: QH/QTD pool exhausted on bus %u\n", ctrl->bus_number);
        goto transfer_cleanup;
    }
    ehci_fill_qtd(ctrl->qtds[qtd_index].virtual, input ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT, endpoint->data_toggle ? EHCI_QTD_TOGGLE : 0, (uint32_t)data_physical, length, EHCI_QTD_NEXT_TERMINATE,
                  true);
    ehci_qh_t *qh      = ctrl->qhs[qh_index].virtual;
    qh->endpoint_chars = ehci_qh_characteristics(device, endpoint_number, max_packet);
    qh->endpoint_caps  = 1U << EHCI_QH_MUL_SHIFT;
    qh->next_qtd       = (uint32_t)ctrl->qtds[qtd_index].physical;
    qh->alt_next_qtd   = EHCI_QTD_NEXT_TERMINATE;
    qh->token          = 0;
    status             = ehci_schedule_async(ctrl, qh_index);
    if (status == EOK) status = ehci_wait_chain(ctrl, &qtd_index, 1, timeout_ms);
    ehci_unschedule_async(ctrl, qh_index);
    size_t remaining = (ctrl->qtds[qtd_index].virtual->token & EHCI_QTD_LENGTH_MASK) >> EHCI_QTD_LENGTH_SHIFT;
    if (remaining > length) status = -EPROTO;
    if (status == EOK) {
        size_t transferred    = length - remaining;
        endpoint->data_toggle = (ctrl->qtds[qtd_index].virtual->token & EHCI_QTD_TOGGLE) != 0;
        if (actual) *actual = transferred;
        if (input && transferred) memcpy(buffer, data_dma, transferred);
    }
transfer_cleanup:
    if (qh_index >= 0) ehci_free_qh(ctrl, qh_index);
    if (qtd_index >= 0) ehci_free_qtd(ctrl, qtd_index);
    if (data_dma) ehci_dma_free(data_physical, length);
    ehci_io_unlock(ctrl);
    return status;
}

/* Register a periodic interrupt-IN transfer for the worker to service. */
static int ehci_interrupt_start(usb_endpoint_t *endpoint, size_t length, usb_interrupt_complete_t complete, void *context)
{
    if (!endpoint || !endpoint->interface || !endpoint->interface->device || !length || length > PAGE_4K_SIZE || !complete) return -EINVAL;
    if ((endpoint->descriptor.attributes & USB_ENDPOINT_XFERTYPE_MASK) != USB_ENDPOINT_XFER_INT || !(endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK)) return -EINVAL;
    if (endpoint->hc_private) return -EBUSY;
    ehci_controller_t *ctrl = endpoint->interface->device->hc_private;
    if (!ctrl) return -ENODEV;
    ehci_periodic_transfer_t *transfer = calloc(1, sizeof(*transfer));
    if (!transfer) return -ENOMEM;
    transfer->buffer = malloc(length);
    if (!transfer->buffer) {
        plogk("usb-ehci: Interrupt transfer buffer allocation failed on bus %u (%zu bytes)\n", ctrl->bus_number, length);
        free(transfer);
        return -ENOMEM;
    }
    uint8_t  interval     = endpoint->descriptor.interval;
    uint32_t microframes  = interval && interval <= 16 ? 1U << (interval - 1) : 1U;
    transfer->endpoint    = endpoint;
    transfer->complete    = complete;
    transfer->context     = context;
    transfer->length      = length;
    transfer->interval_ms = (microframes + 7) / 8;
    if (!transfer->interval_ms) transfer->interval_ms = 1;
    transfer->next_poll = nano_time();
    transfer->active    = true;
    uint64_t flags      = spin_lock_irqsave(&ctrl->lock);
    for (size_t i = 0; i < EHCI_MAX_PERIODIC; i++) {
        if (!ctrl->periodic[i]) {
            ctrl->periodic[i]    = transfer;
            endpoint->hc_private = transfer;
            spin_unlock_irqrestore(&ctrl->lock, flags);
            return EOK;
        }
    }
    spin_unlock_irqrestore(&ctrl->lock, flags);
    plogk("usb-ehci: Periodic transfer pool exhausted on bus %u\n", ctrl->bus_number);
    free(transfer->buffer);
    free(transfer);
    return -ENOSPC;
}

/* Unregister a periodic transfer and wait out any running callback. */
static void ehci_interrupt_stop(usb_endpoint_t *endpoint)
{
    ehci_periodic_transfer_t *transfer = endpoint ? endpoint->hc_private : NULL;
    if (!transfer || !endpoint->interface || !endpoint->interface->device) return;
    ehci_controller_t *ctrl  = endpoint->interface->device->hc_private;
    uint64_t           flags = spin_lock_irqsave(&ctrl->lock);
    transfer->active         = false;
    for (size_t i = 0; i < EHCI_MAX_PERIODIC; i++)
        if (ctrl->periodic[i] == transfer) ctrl->periodic[i] = NULL;
    endpoint->hc_private = NULL;
    spin_unlock_irqrestore(&ctrl->lock, flags);
    while (__atomic_load_n(&transfer->in_callback, __ATOMIC_ACQUIRE)) __asm__ volatile("pause");
    free(transfer->buffer);
    free(transfer);
}

/* Initialize per-endpoint state before it is used. */
static int ehci_configure_endpoint(usb_endpoint_t *endpoint)
{
    if (!endpoint) return -EINVAL;
    endpoint->data_toggle = 0;
    return EOK;
}

/* Reset the endpoint's data-toggle state after a stall. */
static int ehci_clear_halt(usb_endpoint_t *endpoint)
{
    if (!endpoint) return -EINVAL;
    endpoint->data_toggle = 0;
    return EOK;
}

/* Stop every interrupt transfer of a device before it goes away. */
static void ehci_disable_device(usb_device_t *device)
{
    if (!device) return;
    for (size_t i = 0; i < device->interface_count; i++)
        for (size_t j = 0; j < device->interfaces[i].endpoint_count; j++) ehci_interrupt_stop(&device->interfaces[i].endpoints[j]);
}

static const usb_hcd_ops_t ehci_hcd_ops = {
    .control            = ehci_control,
    .transfer           = ehci_transfer,
    .interrupt_start    = ehci_interrupt_start,
    .interrupt_stop     = ehci_interrupt_stop,
    .configure_endpoint = ehci_configure_endpoint,
    .disable_endpoint   = ehci_interrupt_stop,
    .clear_halt         = ehci_clear_halt,
    .disable_device     = ehci_disable_device,
};

/* Perform the EHCI port reset sequence and check for a high-speed device. */
static int ehci_port_reset(ehci_controller_t *ctrl, uint8_t port)
{
    if (port >= ctrl->num_ports) return -EINVAL;
    size_t   offset = EHCI_OP_PORTSC + port * EHCI_PORT_STRIDE;
    uint32_t portsc = ehci_read32(ctrl->operational, offset);
    if (!(portsc & EHCI_PORT_CCS)) return -ENODEV;
    if ((portsc & EHCI_PORT_LS_MASK) == EHCI_PORT_LS_KSTATE) {
        ehci_write32(ctrl->operational, offset, portsc | EHCI_PORT_PO);
        return -ENODEV;
    }
    ehci_write32(ctrl->operational, offset, portsc | EHCI_PORT_PR);
    msleep(50);
    portsc = ehci_read32(ctrl->operational, offset);
    ehci_write32(ctrl->operational, offset, portsc & ~EHCI_PORT_PR);
    int status = ehci_wait_register(ctrl->operational, offset, EHCI_PORT_PR, 0, 100);
    if (status != EOK) return status;
    msleep(10);
    portsc = ehci_read32(ctrl->operational, offset);
    ehci_write32(ctrl->operational, offset, portsc | EHCI_PORT_CHANGE_BITS);
    if (!(portsc & EHCI_PORT_CCS)) return -ENODEV;
    if (!(portsc & EHCI_PORT_PED)) {
        ehci_write32(ctrl->operational, offset, portsc | EHCI_PORT_PO);
        return -ENODEV;
    }
    return EOK;
}

/* EHCI ports only serve high-speed devices. */
static usb_speed_t ehci_port_speed_type(uint32_t portsc)
{
    (void)portsc;
    return USB_SPEED_HIGH;
}

/* Device-model release callback: free the usb_device. */
static void ehci_usb_device_release(struct device *dev)
{
    usb_device_t *device = container_of(dev, usb_device_t, dev);
    free(device);
}

/* Fetch and convert a device string descriptor to ASCII. */
static int ehci_get_string(usb_device_t *device, uint8_t index, uint16_t language, char *output, size_t capacity)
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

/* Reset a port, assign an address and register the device with the USB core */
static int ehci_enumerate_port(ehci_controller_t *ctrl, uint8_t port)
{
    if (!ctrl || port >= ctrl->num_ports) return -EINVAL;
    if (ctrl->devices[port]) return -EEXIST;
    int result = ehci_port_reset(ctrl, port);
    if (result != EOK) return result;

    size_t      offset = EHCI_OP_PORTSC + port * EHCI_PORT_STRIDE;
    usb_speed_t speed  = ehci_port_speed_type(ehci_read32(ctrl->operational, offset));

    usb_device_t *device = calloc(1, sizeof(*device));
    if (!device) return -ENOMEM;
    device->connected   = true;
    device->speed       = speed;
    device->bus_number  = ctrl->bus_number;
    device->port_number = port + 1;
    device->hcd_ops     = &ehci_hcd_ops;
    device->hc_private  = ctrl;
    device->dev.release = ehci_usb_device_release;
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
    ehci_get_string(device, device->descriptor.manufacturer, language, device->manufacturer, sizeof(device->manufacturer));
    ehci_get_string(device, device->descriptor.product, language, device->product, sizeof(device->product));
    ehci_get_string(device, device->descriptor.serial_number, language, device->serial, sizeof(device->serial));

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
fail:
    bool registered = device->registered;
    usb_remove_device(device);
    if (!registered) free(device);
    return result;
}

/* Tear down the device currently attached to a port. */
static void ehci_disconnect_port(ehci_controller_t *ctrl, uint8_t port)
{
    if (!ctrl || port >= ctrl->num_ports) return;
    usb_device_t *device = ctrl->devices[port];
    if (!device) return;
    ctrl->devices[port] = NULL;
    usb_remove_device(device);
}

/* Poll every due periodic transfer and deliver its results. */
static void ehci_service_periodic(ehci_controller_t *ctrl)
{
    uint64_t now = nano_time();
    for (size_t i = 0; i < EHCI_MAX_PERIODIC; i++) {
        uint64_t                  flags    = spin_lock_irqsave(&ctrl->lock);
        ehci_periodic_transfer_t *transfer = ctrl->periodic[i];
        if (!transfer || !transfer->active || transfer->in_callback || now < transfer->next_poll) {
            spin_unlock_irqrestore(&ctrl->lock, flags);
            continue;
        }
        transfer->in_callback = true;
        spin_unlock_irqrestore(&ctrl->lock, flags);
        size_t actual = 0;
        uint32_t timeout = transfer->interval_ms ? transfer->interval_ms : 10;
        if (timeout > 100) timeout = 100;
        int status = ehci_transfer(transfer->endpoint, transfer->buffer, transfer->length, &actual, timeout);
        if (status == -ETIMEDOUT) { status = EOK; actual = 0; }
        if (__atomic_load_n(&transfer->active, __ATOMIC_ACQUIRE) && (actual || status != EOK)) transfer->complete(transfer->endpoint, transfer->buffer, actual, status, transfer->context);
        transfer->next_poll = nano_time() + (uint64_t)transfer->interval_ms * 1000000ULL;
        __atomic_store_n(&transfer->in_callback, false, __ATOMIC_RELEASE);
    }
}

/* Hub worker: handle port changes, enumerate devices, poll periodic. */
static int ehci_worker(void *argument)
{
    ehci_controller_t *ctrl = argument;
    while (ctrl->running && !kthread_should_stop()) {
        uint64_t flags      = spin_lock_irqsave(&ctrl->lock);
        uint64_t ports      = ctrl->pending_ports;
        ctrl->pending_ports = 0;
        if (!ports && ctrl->running && !kthread_should_stop()) wait_queue_prepare(&ctrl->worker_wait);
        spin_unlock_irqrestore(&ctrl->lock, flags);
        for (uint8_t port = 0; port < ctrl->num_ports; port++) {
            if (!(ports & (1ULL << port))) continue;
            size_t   offset = EHCI_OP_PORTSC + port * EHCI_PORT_STRIDE;
            uint32_t portsc = ehci_read32(ctrl->operational, offset);
            uint32_t change = portsc & EHCI_PORT_CHANGE_BITS;
            if (change) ehci_write32(ctrl->operational, offset, portsc);
            if (!(portsc & EHCI_PORT_CCS)) {
                if (ctrl->devices[port]) ehci_disconnect_port(ctrl, port);
            } else if (!ctrl->devices[port] || (portsc & EHCI_PORT_CSC)) {
                if (ctrl->devices[port]) ehci_disconnect_port(ctrl, port);
                msleep(100);
                int ret = ehci_enumerate_port(ctrl, port);
                if (ret != EOK) plogk("usb-ehci: Port %u enumeration failed: %d\n", port, ret);
            }
        }
        ehci_service_periodic(ctrl);

        if (!ports && ctrl->running && !kthread_should_stop()) wait_queue_wait_timed(&ctrl->worker_wait, sched_ticks() + timer_ns_to_ticks_ceil(1000000ULL));
    }
    return 0;
}

/* ISR: acknowledge status bits and flag port-change work. */
INTERRUPT_BEGIN static void ehci_interrupt_handler(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    (void)frame;
    for (size_t i = 0; i < ehci_controller_count; i++) {
        ehci_controller_t *ctrl = ehci_controllers[i];
        if (!ctrl || !ctrl->running) continue;
        uint32_t sts = ehci_read32(ctrl->operational, EHCI_OP_USBSTS);
        if (!(sts & (EHCI_STS_INT | EHCI_STS_PCD | EHCI_STS_ERR | EHCI_STS_IAA | EHCI_STS_HSE))) continue;
        if (sts & EHCI_STS_PCD) {
            ehci_write32(ctrl->operational, EHCI_OP_USBSTS, EHCI_STS_PCD);
            uint64_t flags = spin_lock_irqsave(&ctrl->lock);
            ctrl->pending_ports |= (1ULL << ctrl->num_ports) - 1;
            spin_unlock_irqrestore(&ctrl->lock, flags);
            if (ctrl->worker_started) wait_queue_wake_one(&ctrl->worker_wait);
        }
        if (sts & EHCI_STS_INT) ehci_write32(ctrl->operational, EHCI_OP_USBSTS, EHCI_STS_INT);
        if (sts & EHCI_STS_ERR) {
            ehci_write32(ctrl->operational, EHCI_OP_USBSTS, EHCI_STS_ERR);
            plogk("usb-ehci: USB error interrupt on bus %u\n", ctrl->bus_number);
        }
        if (sts & EHCI_STS_HSE) {
            ehci_write32(ctrl->operational, EHCI_OP_USBSTS, EHCI_STS_HSE);
            plogk("usb-ehci: Host system error on bus %u\n", ctrl->bus_number);
        }
        if (sts & EHCI_STS_IAA) ehci_write32(ctrl->operational, EHCI_OP_USBSTS, EHCI_STS_IAA);
    }
    send_eoi();
    irq_leave_gs(frame);
}
INTERRUPT_END

/* host_ops: stop the controller and its interrupts. */
static void ehci_host_stop(usb_host_t *host)
{
    ehci_controller_t *ctrl = container_of(host, ehci_controller_t, hcd);
    ctrl->running           = false;
    wait_queue_wake_all(&ctrl->worker_wait);
    ehci_write32(ctrl->operational, EHCI_OP_USBCMD, 0);
    ehci_write32(ctrl->operational, EHCI_OP_USBINTR, 0);
}

static usb_host_controller_ops_t ehci_controller_ops = {
    .host_stop = ehci_host_stop,
};

/* Probe an EHCI PCI device: reset, allocate pools, and start. */
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
    (void)snprintf(ctrl->hcd.name, sizeof(ctrl->hcd.name), "ehci-usb%u", bus_number);

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
    for (size_t frame = 0; frame < EHCI_FRAME_LIST_SIZE; frame++) ctrl->periodic_list[frame] = EHCI_QHL_TERMINATE;

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
    ehci_write32(ctrl->operational, EHCI_OP_USBCMD, EHCI_CMD_RUN);
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
    if (ctrl->vector > 0) register_interrupt_handler((uint16_t)ctrl->vector, ehci_interrupt_handler, 0, 0x8e);

    wait_queue_init(&ctrl->worker_wait);
    ctrl->running                             = true;
    ehci_controllers[ehci_controller_count++] = ctrl;
    usb_host_register(&ctrl->hcd);
    ctrl->hcd.running = true;

    plogk("usb-ehci: Controller at MMIO %p, bus usb%u, %u ports.\n", (void *)bar.address, bus_number, ctrl->num_ports);
    return EOK;
}

/* Probe every EHCI controller in the PCI device cache. */
int ehci_init(void)
{
#if !CONFIG_USB_EHCI
    return 0;
#endif
    size_t               before = ehci_controller_count;
    pci_devices_cache_t *cache  = pci_get_devices_cache();
    if (!cache) return 0;
    for (pci_device_cache_t *pci = cache->head; pci && ehci_controller_count < EHCI_MAX_CONTROLLERS; pci = pci->next) {
        if (pci->class_code != EHCI_PCI_CLASS) continue;
        int bus_number = usb_host_allocate_bus_number();
        if (bus_number < 0) break;
        (void)ehci_probe(pci, (uint8_t)bus_number);
    }
    return (int)(ehci_controller_count - before);
}

void ehci_start_workers(void)
{
#if !CONFIG_USB_EHCI
    return;
#endif
    for (size_t i = 0; i < ehci_controller_count; i++) {
        ehci_controller_t *ctrl = ehci_controllers[i];
        if (!ctrl || ctrl->worker_started) continue;
        ctrl->worker_started = true;

        /* Enumerate every root port on the first worker pass. */
        ctrl->pending_ports |= (1ULL << ctrl->num_ports) - 1;
        kernel_worker_register("ehci-hub", ehci_worker, ctrl, &ctrl->worker_task);
    }
}
