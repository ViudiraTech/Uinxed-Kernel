/*
 *
 *      uhci.c
 *      Universal Host Controller Interface (UHCI) driver
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <chipset/common.h>
#include <drivers/bus/pci.h>
#include <drivers/interrupt/apic.h>
#include <drivers/usb/host/host.h>
#include <drivers/usb/host/uhci.h>
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

#define UHCI_MAX_CONTROLLERS  8
#define UHCI_RESET_TIMEOUT_MS 100
#define UHCI_ENUM_TIMEOUT_MS  1000
#define UHCI_IO_CHUNK         PAGE_4K_SIZE

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

        uhci_td_phys_t tds[UHCI_NUM_TD];
        uhci_qh_phys_t qhs[UHCI_NUM_QH];
        uint8_t        port_state[UHCI_MAX_PORTS];
        uint64_t       pending_ports;

        spinlock_t lock;
        spinlock_t td_lock;
        task_t    *worker_task;
        bool       running;
        bool       worker_started;
} uhci_controller_t;

static uhci_controller_t *uhci_controllers[UHCI_MAX_CONTROLLERS];
static size_t             uhci_controller_count;

static inline uint16_t uhci_readw(uhci_controller_t *ctrl, uint8_t reg)
{
    return inw(ctrl->io_base + reg);
}

static inline void uhci_writew(uhci_controller_t *ctrl, uint8_t reg, uint16_t value)
{
    outw(ctrl->io_base + reg, value);
}

static inline uint32_t uhci_readl(uhci_controller_t *ctrl, uint8_t reg)
{
    return inl(ctrl->io_base + reg);
}

static inline void uhci_writel(uhci_controller_t *ctrl, uint8_t reg, uint32_t value)
{
    outl(ctrl->io_base + reg, value);
}

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

static void uhci_dma_free(uint64_t physical, size_t size)
{
    size_t count = (size + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    if (physical && count) free_frames(physical, count);
}

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

static void uhci_free_td(uhci_controller_t *ctrl, int index)
{
    if (index < 0 || index >= UHCI_NUM_TD) return;
    uint64_t flags        = spin_lock_irqsave(&ctrl->td_lock);
    ctrl->tds[index].used = false;
    memset(ctrl->tds[index].virtual, 0, sizeof(uhci_td_t));
    spin_unlock_irqrestore(&ctrl->td_lock, flags);
}

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

static void uhci_free_qh(uhci_controller_t *ctrl, int index)
{
    if (index < 0 || index >= UHCI_NUM_QH) return;
    uint64_t flags        = spin_lock_irqsave(&ctrl->lock);
    ctrl->qhs[index].used = false;
    memset(ctrl->qhs[index].virtual, 0, sizeof(uhci_qh_t));
    spin_unlock_irqrestore(&ctrl->lock, flags);
}

static void uhci_add_async_qh(uhci_controller_t *ctrl, uhci_qh_phys_t *qh)
{
    (void)ctrl;
    uint64_t qh_phys             = qh->physical;
    uint32_t link                = (uint32_t)qh_phys | UHCI_LINK_QH;
    qh->virtual->horizontal_link = link;
    qh->virtual->element_link    = UHCI_LINK_TERMINATE;
    dma_write_barrier();
}

static void uhci_wait_td(uhci_controller_t *ctrl, uhci_td_t *td, uint32_t timeout_ms)
{
    (void)ctrl;
    uint64_t deadline = nano_time() + (uint64_t)timeout_ms * 1000000ULL;
    while (td->control_status & UHCI_TD_ACTIVE) {
        if (nano_time() >= deadline) break;
        __asm__ volatile("pause");
    }
}

static int uhci_submit_control(usb_device_t *device, const usb_setup_packet_t *setup, void *buffer, size_t length, uint32_t timeout_ms)
{
    uhci_controller_t *ctrl = device ? device->hc_private : NULL;
    if (!ctrl || !setup) return -EINVAL;

    uint64_t setup_data = 0;
    memcpy(&setup_data, setup, sizeof(*setup));
    bool input = !!(setup->request_type & USB_DIR_IN);

    int td_setup_idx, td_data_idx = -1, td_status_idx;
    td_setup_idx = uhci_find_free_td(ctrl);
    if (td_setup_idx < 0) return -ENOMEM;
    td_status_idx = uhci_find_free_td(ctrl);
    if (td_status_idx < 0) {
        uhci_free_td(ctrl, td_setup_idx);
        return -ENOMEM;
    }

    void    *dma_buffer = NULL;
    uint64_t dma_phys   = 0;
    if (length) {
        td_data_idx = uhci_find_free_td(ctrl);
        if (td_data_idx < 0) {
            uhci_free_td(ctrl, td_setup_idx);
            uhci_free_td(ctrl, td_status_idx);
            return -ENOMEM;
        }
        dma_buffer = uhci_dma_alloc(length, &dma_phys);
        if (!dma_buffer) {
            uhci_free_td(ctrl, td_setup_idx);
            uhci_free_td(ctrl, td_status_idx);
            uhci_free_td(ctrl, td_data_idx);
            return -ENOMEM;
        }
        if (!input) memcpy(dma_buffer, buffer, length);
    }

    uint64_t setup_phys = 0;
    void    *setup_dma  = uhci_dma_alloc(sizeof(setup_data), &setup_phys);
    if (!setup_dma) {
        uhci_free_td(ctrl, td_setup_idx);
        uhci_free_td(ctrl, td_status_idx);
        if (td_data_idx >= 0) uhci_free_td(ctrl, td_data_idx);
        if (dma_buffer) uhci_dma_free(dma_phys, length);
        return -ENOMEM;
    }
    memcpy(setup_dma, &setup_data, sizeof(setup_data));

    int qh_idx = uhci_find_free_qh(ctrl);
    if (qh_idx < 0) {
        uhci_dma_free(setup_phys, sizeof(setup_data));
        uhci_free_td(ctrl, td_setup_idx);
        uhci_free_td(ctrl, td_status_idx);
        if (td_data_idx >= 0) uhci_free_td(ctrl, td_data_idx);
        if (dma_buffer) uhci_dma_free(dma_phys, length);
        return -ENOMEM;
    }

    uhci_td_t *td_setup_v = ctrl->tds[td_setup_idx].virtual;
    uint32_t   max_len    = 8;
    uint32_t   token      = (UHCI_PID_SETUP << UHCI_TOKEN_PID_SHIFT) | ((uint32_t)device->address << UHCI_TOKEN_DEVADDR_SHIFT)
                     | (0U << UHCI_TOKEN_ENDP_SHIFT) | (0U << UHCI_TOKEN_TOGGLE_SHIFT) | ((max_len & 0x7ff) << UHCI_TOKEN_MAXLEN_SHIFT);
    td_setup_v->link           = ctrl->tds[td_status_idx].physical | UHCI_LINK_QH;
    td_setup_v->control_status = UHCI_TD_ACTIVE;
    td_setup_v->token          = token;
    td_setup_v->buffer         = (uint32_t)setup_phys;
    dma_write_barrier();

    uhci_td_t *td_status_v  = ctrl->tds[td_status_idx].virtual;
    uint32_t   status_token = ((input ? UHCI_PID_IN : UHCI_PID_OUT) << UHCI_TOKEN_PID_SHIFT)
                            | ((uint32_t)device->address << UHCI_TOKEN_DEVADDR_SHIFT) | (0U << UHCI_TOKEN_ENDP_SHIFT)
                            | (1U << UHCI_TOKEN_TOGGLE_SHIFT) | (0x7ffU << UHCI_TOKEN_MAXLEN_SHIFT);
    td_status_v->link           = UHCI_LINK_TERMINATE | UHCI_LINK_QH;
    td_status_v->control_status = UHCI_TD_ACTIVE;
    td_status_v->token          = status_token;
    td_status_v->buffer         = 0;
    dma_write_barrier();

    if (length && td_data_idx >= 0) {
        uhci_td_t *td_data_v = ctrl->tds[td_data_idx].virtual;
        td_setup_v->link     = ctrl->tds[td_data_idx].physical | UHCI_LINK_QH;
        td_data_v->link      = ctrl->tds[td_status_idx].physical | UHCI_LINK_QH;
        uint32_t data_token  = ((input ? UHCI_PID_IN : UHCI_PID_OUT) << UHCI_TOKEN_PID_SHIFT)
                              | ((uint32_t)device->address << UHCI_TOKEN_DEVADDR_SHIFT) | (0U << UHCI_TOKEN_ENDP_SHIFT)
                              | (1U << UHCI_TOKEN_TOGGLE_SHIFT) | (((uint32_t)length & 0x7ff) << UHCI_TOKEN_MAXLEN_SHIFT);
        td_data_v->control_status = UHCI_TD_ACTIVE;
        td_data_v->token          = data_token;
        td_data_v->buffer         = (uint32_t)dma_phys;
        dma_write_barrier();
    }

    uhci_qh_t *qh       = ctrl->qhs[qh_idx].virtual;
    qh->horizontal_link = UHCI_LINK_TERMINATE;
    qh->element_link    = (uint32_t)ctrl->tds[td_setup_idx].physical;
    dma_write_barrier();

    spin_lock(&ctrl->lock);
    uint32_t *fl_base = ctrl->frame_list_virtual;
    for (int i = 0; i < UHCI_FRAME_LIST_SIZE; i++) { fl_base[i] = (uint32_t)ctrl->qhs[qh_idx].physical | UHCI_LINK_QH; }
    dma_write_barrier();
    spin_unlock(&ctrl->lock);

    uhci_wait_td(ctrl, td_setup_v, timeout_ms);
    if (td_data_idx >= 0) uhci_wait_td(ctrl, ctrl->tds[td_data_idx].virtual, timeout_ms);
    uhci_wait_td(ctrl, td_status_v, timeout_ms);

    int status = EOK;
    if (td_status_v->control_status & UHCI_TD_STALLED)
        status = -EPIPE;
    else if (td_status_v->control_status & UHCI_TD_ACTIVE)
        status = -ETIMEDOUT;
    else if (td_status_v->control_status & UHCI_TD_BABBLE)
        status = -EOVERFLOW;
    else if (td_status_v->control_status & UHCI_TD_NAK)
        status = -EAGAIN;

    if (status == EOK && length && input && dma_buffer) { memcpy(buffer, dma_buffer, length); }

    spin_lock(&ctrl->lock);
    for (int i = 0; i < UHCI_FRAME_LIST_SIZE; i++) { fl_base[i] = UHCI_LINK_TERMINATE; }
    dma_write_barrier();
    spin_unlock(&ctrl->lock);

    uhci_free_td(ctrl, td_setup_idx);
    uhci_free_td(ctrl, td_status_idx);
    if (td_data_idx >= 0) uhci_free_td(ctrl, td_data_idx);
    uhci_free_qh(ctrl, qh_idx);
    if (setup_phys) uhci_dma_free(setup_phys, sizeof(setup_data));
    if (dma_buffer) uhci_dma_free(dma_phys, length);
    return status;
}

static int uhci_submit_bulk(usb_endpoint_t *endpoint, void *buffer, size_t length, size_t *actual, uint32_t timeout_ms)
{
    if (!endpoint || !buffer || !length) return -EINVAL;
    uhci_controller_t *ctrl   = endpoint->interface->device->hc_private;
    bool               input  = !!(endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK);
    uint8_t            ep_num = endpoint->descriptor.endpoint_address & USB_ENDPOINT_NUMBER_MASK;

    uint64_t dma_phys   = 0;
    void    *dma_buffer = uhci_dma_alloc(length, &dma_phys);
    if (!dma_buffer) return -ENOMEM;
    if (!input) memcpy(dma_buffer, buffer, length);

    int td_idx = uhci_find_free_td(ctrl);
    if (td_idx < 0) {
        uhci_dma_free(dma_phys, length);
        return -ENOMEM;
    }

    int qh_idx = uhci_find_free_qh(ctrl);
    if (qh_idx < 0) {
        uhci_free_td(ctrl, td_idx);
        uhci_dma_free(dma_phys, length);
        return -ENOMEM;
    }

    uhci_td_t *td    = ctrl->tds[td_idx].virtual;
    uint32_t   token = ((input ? UHCI_PID_IN : UHCI_PID_OUT) << UHCI_TOKEN_PID_SHIFT)
                     | ((uint32_t)endpoint->interface->device->address << UHCI_TOKEN_DEVADDR_SHIFT) | ((uint32_t)ep_num << UHCI_TOKEN_ENDP_SHIFT)
                     | (1U << UHCI_TOKEN_TOGGLE_SHIFT) | (((uint32_t)length & 0x7ff) << UHCI_TOKEN_MAXLEN_SHIFT);
    td->link           = UHCI_LINK_TERMINATE | UHCI_LINK_QH;
    td->control_status = UHCI_TD_ACTIVE;
    td->token          = token;
    td->buffer         = (uint32_t)dma_phys;
    dma_write_barrier();

    uhci_qh_t *qh       = ctrl->qhs[qh_idx].virtual;
    qh->horizontal_link = UHCI_LINK_TERMINATE;
    qh->element_link    = (uint32_t)ctrl->tds[td_idx].physical;
    dma_write_barrier();

    spin_lock(&ctrl->lock);
    uint32_t *fl_base = ctrl->frame_list_virtual;
    for (int i = 0; i < UHCI_FRAME_LIST_SIZE; i++) { fl_base[i] = (uint32_t)ctrl->qhs[qh_idx].physical | UHCI_LINK_QH; }
    dma_write_barrier();
    spin_unlock(&ctrl->lock);

    uhci_wait_td(ctrl, td, timeout_ms);

    int status = EOK;
    if (td->control_status & UHCI_TD_STALLED)
        status = -EPIPE;
    else if (td->control_status & UHCI_TD_ACTIVE)
        status = -ETIMEDOUT;
    else if (td->control_status & UHCI_TD_BABBLE)
        status = -EOVERFLOW;

    size_t actlen = td->control_status & UHCI_TD_ACTLEN_MASK;
    if (status == EOK) {
        if (actlen > length) actlen = length;
        if (actual) *actual = actlen;
        if (input && actlen) memcpy(buffer, dma_buffer, actlen);
    }

    spin_lock(&ctrl->lock);
    for (int i = 0; i < UHCI_FRAME_LIST_SIZE; i++) { fl_base[i] = UHCI_LINK_TERMINATE; }
    dma_write_barrier();
    spin_unlock(&ctrl->lock);

    uhci_free_td(ctrl, td_idx);
    uhci_free_qh(ctrl, qh_idx);
    uhci_dma_free(dma_phys, length);
    return status;
}

static int uhci_submit_interrupt(usb_endpoint_t *endpoint, size_t length, usb_interrupt_complete_t complete, void *context)
{
    (void)endpoint;
    (void)length;
    (void)complete;
    (void)context;
    return -ENOSYS;
}

static void uhci_interrupt_stop(usb_endpoint_t *endpoint)
{
    (void)endpoint;
}

static int uhci_configure_endpoint(usb_endpoint_t *endpoint)
{
    (void)endpoint;
    return EOK;
}

static int uhci_clear_halt(usb_endpoint_t *endpoint)
{
    (void)endpoint;
    return EOK;
}

static void uhci_disable_device(usb_device_t *device)
{
    (void)device;
}

static const usb_hcd_ops_t uhci_hcd_ops = {
    .control            = uhci_submit_control,
    .transfer           = uhci_submit_bulk,
    .interrupt_start    = uhci_submit_interrupt,
    .interrupt_stop     = uhci_interrupt_stop,
    .configure_endpoint = uhci_configure_endpoint,
    .clear_halt         = uhci_clear_halt,
    .disable_device     = uhci_disable_device,
};

static int uhci_port_reset(uhci_controller_t *ctrl, uint8_t port)
{
    if (port >= UHCI_MAX_PORTS) return -EINVAL;
    uint16_t portsc = uhci_readw(ctrl, UHCI_PORTSC1 + port * 2);
    if (!(portsc & UHCI_PORTSC_CCS)) return -ENODEV;
    if (!(portsc & UHCI_PORTSC_PED)) {
        uhci_writew(ctrl, UHCI_PORTSC1 + port * 2, portsc | UHCI_PORTSC_PR);
        uint64_t deadline = nano_time() + 100 * 1000000ULL;
        while (uhci_readw(ctrl, UHCI_PORTSC1 + port * 2) & UHCI_PORTSC_PR) {
            if (nano_time() >= deadline) return -ETIMEDOUT;
            __asm__ volatile("pause");
        }
        portsc = uhci_readw(ctrl, UHCI_PORTSC1 + port * 2);
    }
    uhci_writew(ctrl, UHCI_PORTSC1 + port * 2, portsc & ~(UHCI_PORTSC_CSC | UHCI_PORTSC_PEC));
    return (portsc & UHCI_PORTSC_CCS) ? EOK : -ENODEV;
}

static usb_speed_t uhci_port_speed(uint16_t portsc)
{
    return (portsc & UHCI_PORTSC_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static void uhci_usb_device_release(struct device *dev)
{
    (void)dev;
}

static int uhci_enumerate_port(uhci_controller_t *ctrl, uint8_t port)
{
    int result = uhci_port_reset(ctrl, port);
    if (result != EOK) return result;

    uint16_t    portsc = uhci_readw(ctrl, UHCI_PORTSC1 + port * 2);
    usb_speed_t speed  = uhci_port_speed(portsc);

    usb_device_t *device = calloc(1, sizeof(*device));
    if (!device) return -ENOMEM;
    device->connected   = true;
    device->speed       = speed;
    device->bus_number  = ctrl->bus_number;
    device->port_number = port;
    device->hcd_ops     = &uhci_hcd_ops;
    device->hc_private  = ctrl;
    device->address     = port + 1;
    device->dev.release = uhci_usb_device_release;
    snprintf(device->path, sizeof(device->path), "%u-%u", device->bus_number, port);

    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_ADDRESS, device->address, 0, NULL, 0,
                             USB_CTRL_TIMEOUT_MS);
    if (result != EOK) goto fail;

    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0,
                             &device->descriptor, sizeof(device->descriptor), USB_CTRL_TIMEOUT_MS);
    if (result != EOK) goto fail;

    uint16_t language = 0x0409;
    uint8_t  lang_desc[4];
    if (usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_STRING << 8, 0, lang_desc,
                        sizeof(lang_desc), USB_CTRL_TIMEOUT_MS)
            == EOK
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
    if (result == EOK) return EOK;

fail:
    usb_remove_device(device);
    free(device);
    return result;
}

static void uhci_worker(void *argument)
{
    uhci_controller_t *ctrl = argument;
    while (ctrl->running) {
        uint64_t flags      = spin_lock_irqsave(&ctrl->lock);
        uint64_t ports      = ctrl->pending_ports;
        ctrl->pending_ports = 0;
        spin_unlock_irqrestore(&ctrl->lock, flags);
        for (uint8_t p = 0; ports && p < UHCI_MAX_PORTS; p++) {
            if (ports & (1ULL << p)) {
                uint16_t portsc = uhci_readw(ctrl, UHCI_PORTSC1 + p * 2);
                uint16_t clear  = portsc & (UHCI_PORTSC_CSC | UHCI_PORTSC_PEC);
                if (clear) uhci_writew(ctrl, UHCI_PORTSC1 + p * 2, portsc);
                if ((portsc & UHCI_PORTSC_CCS) && (portsc & UHCI_PORTSC_CSC)) {
                    msleep(100);
                    int ret = uhci_enumerate_port(ctrl, p);
                    if (ret != EOK) plogk("uhci: Port %u enumeration failed: %d\n", p, ret);
                }
            }
        }
        __asm__ volatile("pause");
    }
}

static void uhci_interrupt_handler(void *frame)
{
    (void)frame;
    for (size_t i = 0; i < uhci_controller_count; i++) {
        uhci_controller_t *ctrl = uhci_controllers[i];
        if (!ctrl || !ctrl->running) continue;
        uint16_t sts = uhci_readw(ctrl, UHCI_USBSTS);
        if (sts & UHCI_STS_USBINT) { uhci_writew(ctrl, UHCI_USBSTS, UHCI_STS_USBINT); }
        if (sts & UHCI_STS_ERROR) { uhci_writew(ctrl, UHCI_USBSTS, UHCI_STS_ERROR); }
        if (sts & UHCI_STS_RD) {
            uhci_writew(ctrl, UHCI_USBSTS, UHCI_STS_RD);
            uint64_t flags = spin_lock_irqsave(&ctrl->lock);
            for (uint8_t p = 0; p < UHCI_MAX_PORTS; p++) {
                uint16_t psc = uhci_readw(ctrl, UHCI_PORTSC1 + p * 2);
                if (psc & (UHCI_PORTSC_CSC | UHCI_PORTSC_PEC)) { ctrl->pending_ports |= 1ULL << p; }
            }
            spin_unlock_irqrestore(&ctrl->lock, flags);
        }
        if (sts & UHCI_STS_HSE) { uhci_writew(ctrl, UHCI_USBSTS, UHCI_STS_HSE); }
    }
    send_eoi();
}

static int uhci_host_start(usb_host_t *host)
{
    uhci_controller_t *ctrl = container_of(host, uhci_controller_t, hcd);
    (void)ctrl;
    return EOK;
}

static void uhci_host_stop(usb_host_t *host)
{
    uhci_controller_t *ctrl = container_of(host, uhci_controller_t, hcd);
    ctrl->running           = false;
    uhci_writew(ctrl, UHCI_USBCMD, 0);
    uhci_writew(ctrl, UHCI_USBINTR, 0);
}

static int uhci_port_reset_hcd(usb_host_t *host, uint8_t port)
{
    uhci_controller_t *ctrl = container_of(host, uhci_controller_t, hcd);
    return uhci_port_reset(ctrl, port);
}

static int uhci_port_speed_hcd(usb_host_t *host, uint8_t port)
{
    uhci_controller_t *ctrl = container_of(host, uhci_controller_t, hcd);
    if (port >= UHCI_MAX_PORTS) return USB_SPEED_FULL;
    uint16_t portsc = uhci_readw(ctrl, UHCI_PORTSC1 + port * 2);
    return (portsc & UHCI_PORTSC_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static int uhci_port_connected_hcd(usb_host_t *host, uint8_t port)
{
    uhci_controller_t *ctrl = container_of(host, uhci_controller_t, hcd);
    if (port >= UHCI_MAX_PORTS) return 0;
    return !!(uhci_readw(ctrl, UHCI_PORTSC1 + port * 2) & UHCI_PORTSC_CCS);
}

static usb_host_controller_ops_t uhci_controller_ops = {
    .host_start     = uhci_host_start,
    .host_stop      = uhci_host_stop,
    .port_reset     = uhci_port_reset_hcd,
    .port_speed     = uhci_port_speed_hcd,
    .port_connected = uhci_port_connected_hcd,
};

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
    snprintf(ctrl->hcd.name, sizeof(ctrl->hcd.name), "uhci-usb%u", bus_number);

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

    memset(ctrl->frame_list_virtual, 0, PAGE_4K_SIZE);
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
    if (ctrl->vector > 0) { register_interrupt_handler((uint16_t)ctrl->vector, uhci_interrupt_handler, 0, 0x8e); }

    ctrl->running     = true;
    ctrl->worker_task = kthread_create("uhci-hub", uhci_worker, ctrl);
    if (ctrl->worker_task) {
        ctrl->worker_started = true;
        task_wakeup(ctrl->worker_task);
    }
    uhci_controllers[uhci_controller_count++] = ctrl;
    usb_host_register(&ctrl->hcd);

    for (uint8_t port = 0; port < UHCI_MAX_PORTS; port++) {
        uint16_t portsc = uhci_readw(ctrl, UHCI_PORTSC1 + port * 2);
        uhci_writew(ctrl, UHCI_PORTSC1 + port * 2, portsc);
        if (portsc & UHCI_PORTSC_CCS) {
            int ret = uhci_enumerate_port(ctrl, port);
            if (ret != EOK) plogk("uhci: Port %u enumeration failed: %d\n", port, ret);
        }
    }

    plogk("uhci: Controller at I/O 0x%04x, bus usb%u\n", io_base, bus_number);
    return EOK;
}

void uhci_init(void)
{
#if !CONFIG_USB_UHCI
    return;
#endif
    if (usb_core_init() != EOK) return;
    pci_devices_cache_t *cache = pci_get_devices_cache();
    if (!cache) return;
    uint8_t bus_number = 1;
    for (pci_device_cache_t *pci = cache->head; pci && uhci_controller_count < UHCI_MAX_CONTROLLERS; pci = pci->next) {
        if (pci->class_code != UHCI_PCI_CLASS) continue;
        if (uhci_probe(pci, bus_number) == EOK) bus_number++;
    }
}

void uhci_start_workers(void)
{
#if !CONFIG_USB_UHCI
    return;
#endif
}
