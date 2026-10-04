/* Host-side MMIO and USB mocks. The runner inserts the real driver helpers. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EOK                       0
#define PAGE_4K_SIZE              4096
#define USB_DIR_IN                0x80
#define USB_TYPE_STANDARD         0
#define USB_RECIP_DEVICE          0
#define USB_REQ_GET_DESCRIPTOR    6
#define USB_DT_DEVICE             1
#define USB_CTRL_TIMEOUT_MS       1000
#define XHCI_TRB_TYPE(v)          ((v) << 10)
#define XHCI_TRB_ADDRESS_DEVICE   11
#define XHCI_TRB_EVALUATE_CONTEXT 13
/* PRODUCTION TYPES */
typedef struct {
        uint64_t ring_physical;
} endpoint_t;
typedef struct xhci_transfer xhci_transfer_t;
typedef struct xhci_slot     xhci_slot_t;
typedef struct {
        uint64_t parameter;
        uint32_t status, control;
} xhci_trb_t;
typedef struct xhci_controller {
        volatile uint8_t *operational;
        uint8_t           context_size;
        uint64_t          failed_ports;
        uint8_t           max_slots;
        xhci_slot_t      *slots[256];
} xhci_controller_t;
typedef struct xhci_slot {
        xhci_controller_t *controller;
        uint8_t            port_id, slot_id;
        uint32_t          *input_context, *output_context;
        uint64_t           input_context_physical;
        endpoint_t         endpoints[32];
        xhci_transfer_t   *pending[32];
} xhci_slot_t;
struct xhci_transfer {
        xhci_slot_t *slot;
        void        *endpoint;
        uint64_t     trb_physical, setup_trb_physical, data_trb_physical;
        size_t       length, actual;
        int          status;
        uint8_t      completion_code;
        bool         periodic, completed;
};
typedef struct {
        void                   *hc_private;
        usb_speed_t             speed;
        usb_device_descriptor_t descriptor;
} usb_device_t;
/* PRODUCTION MACROS */
static uint32_t     portsc[64], writes[64];
static int          enumerate_calls, enumerate_result, disconnect_calls, command_calls, command_result, control_calls, control_result;
static int          wait_result;
static bool         descriptor_updated;
static uint16_t     descriptor_packet;
static uint8_t      descriptor_length = 18, descriptor_type = 1;
static xhci_slot_t *connected_slot;
static uint32_t     xhci_read32(const volatile uint8_t *base, size_t offset)
{
    (void)base;
    return portsc[(offset - XHCI_OP_PORTS) / XHCI_PORT_STRIDE];
}
static void xhci_write32(volatile uint8_t *base, size_t offset, uint32_t value)
{
    (void)base;
    size_t i  = (offset - XHCI_OP_PORTS) / XHCI_PORT_STRIDE;
    writes[i] = value;
    if (value & XHCI_PORT_PED) {
        portsc[i] &= ~XHCI_PORT_PED;
        portsc[i] |= XHCI_PORT_PEC;
    }
    portsc[i] &= ~(value & XHCI_PORT_CHANGE_BITS);
    if (value & XHCI_PORT_PR) portsc[i] |= XHCI_PORT_PED | XHCI_PORT_PRC;
}
static int xhci_wait_register(const volatile uint8_t *base, size_t offset, uint32_t mask, uint32_t value, uint32_t timeout)
{
    (void)base;
    (void)offset;
    (void)mask;
    (void)value;
    (void)timeout;
    return wait_result;
}
static xhci_slot_t *xhci_slot_on_port(xhci_controller_t *controller, uint8_t port)
{
    (void)controller;
    (void)port;
    return connected_slot;
}
static void xhci_disconnect_port(xhci_controller_t *controller, uint8_t port)
{
    (void)controller;
    (void)port;
    disconnect_calls++;
    connected_slot = NULL;
}
static int xhci_enumerate_port(xhci_controller_t *controller, uint8_t port)
{
    (void)controller;
    (void)port;
    enumerate_calls++;
    return enumerate_result;
}
static void msleep(int ms)
{
    (void)ms;
}
#define plogk(...)          ((void)0)
#define dma_write_barrier() ((void)0)
#define dma_read_barrier()  ((void)0)
static int xhci_command(xhci_controller_t *controller, uint64_t parameter, uint32_t status, uint32_t control, uint8_t *slot_id)
{
    (void)controller;
    (void)parameter;
    (void)status;
    (void)slot_id;
    command_calls++;
    if (((control >> 10) & 63) == XHCI_TRB_EVALUATE_CONTEXT && command_result == 0) descriptor_updated = true;
    return command_result;
}
static int usb_control_msg(usb_device_t *device, uint8_t type, uint8_t request, uint16_t value, uint16_t index, void *buffer, uint16_t length, uint32_t timeout)
{
    (void)type;
    (void)request;
    (void)value;
    (void)index;
    (void)timeout;
    control_calls++;
    if (control_result) return control_result;
    if (device->speed == USB_SPEED_FULL && descriptor_packet != 8 && length > 8) assert(descriptor_updated);
    assert(length == 8 || length == 18);
    usb_device_descriptor_t d = {.length = descriptor_length, .descriptor_type = descriptor_type, .max_packet_size0 = descriptor_packet};
    memcpy(buffer, &d, length);
    return 0;
}
/* PRODUCTION FUNCTIONS */
int main(int argc, char **argv)
{
    assert(argc == 3);
    xhci_controller_t c           = {.context_size = atoi(argv[2]), .max_slots = 255};
    unsigned          stride      = c.context_size / 4;
    uint32_t          input[1024] = {0}, output[1024] = {0};
    xhci_slot_t       s = {.controller = &c, .port_id = 1, .slot_id = 2, .input_context = input, .output_context = output};
    if (!strcmp(argv[1], "port")) {
        uint32_t power_wake = (1U << 9) | (3U << 14) | (7U << 25);
        portsc[0]           = XHCI_PORT_CCS | XHCI_PORT_PED | XHCI_PORT_CSC | power_wake;
        assert(xhci_port_reset(&c, 1) == EOK);
        assert(!(writes[0] & XHCI_PORT_PED));
        assert(portsc[0] & XHCI_PORT_PED);
        assert((writes[0] & power_wake) == power_wake);
        assert(!(portsc[0] & XHCI_PORT_CSC));
        portsc[0] = XHCI_PORT_CCS | power_wake;
        assert(xhci_port_reset(&c, 1) == EOK);
        assert(portsc[0] & XHCI_PORT_PED);
        portsc[0]      = XHCI_PORT_CCS | XHCI_PORT_PR | (1U << 16) | (1U << 31) | XHCI_PORT_PLC | power_wake;
        connected_slot = &s;
        xhci_service_port(&c, 1);
        assert(!(writes[0] & (XHCI_PORT_PED | XHCI_PORT_PR | (1U << 16) | (1U << 31))));
        wait_result = -ETIMEDOUT;
        portsc[0]   = XHCI_PORT_CCS;
        assert(xhci_port_reset(&c, 1) == -ETIMEDOUT);
        portsc[0] = 0;
        assert(xhci_port_reset(&c, 1) == -ENODEV);
    } else if (!strcmp(argv[1], "speed")) {
        const usb_speed_t speeds[] = {USB_SPEED_FULL, USB_SPEED_LOW, USB_SPEED_HIGH, USB_SPEED_SUPER, USB_SPEED_SUPER_PLUS};
        for (unsigned i = 0; i < 5; i++) {
            assert(xhci_usb_speed((i + 1) << 10) == speeds[i]);
            assert(xhci_address_slot(&s, speeds[i]) == EOK);
            assert(((input[stride] >> 20) & 15) == i + 1);
            assert(xhci_address_slot_tt(&s, speeds[i], 0x32, 7, false, 0, 8, 9) == EOK);
            assert(((input[stride] >> 20) & 15) == i + 1);
            assert((input[stride] & 0xfffff) == 0x32);
            assert((input[stride + 1] >> 16) == 7);
            assert(input[stride + 2] == 0x908);
        }
    } else if (!strcmp(argv[1], "retry")) {
        enumerate_result = -EIO;
        portsc[0]        = XHCI_PORT_CCS | XHCI_PORT_PED | XHCI_PORT_CSC;
        xhci_service_port(&c, 1);
        assert(enumerate_calls == 1);
        for (int i = 0; i < 16; i++) {
            portsc[0] |= XHCI_PORT_PEC | XHCI_PORT_PLC | XHCI_PORT_PRC;
            xhci_service_port(&c, 1);
        }
        assert(enumerate_calls == 1);
        portsc[0] |= XHCI_PORT_CSC;
        xhci_service_port(&c, 1);
        assert(enumerate_calls == 2);
        portsc[0] = XHCI_PORT_CSC;
        xhci_service_port(&c, 1);
        assert(!c.failed_ports);
        enumerate_result = 0;
        portsc[0]        = XHCI_PORT_CCS | XHCI_PORT_CSC;
        xhci_service_port(&c, 1);
        assert(enumerate_calls == 3);
        connected_slot = &s;
        portsc[0] |= XHCI_PORT_PEC;
        xhci_service_port(&c, 1);
        assert(enumerate_calls == 3);
        assert(disconnect_calls == 0);
        portsc[0] = XHCI_PORT_CSC;
        xhci_service_port(&c, 1);
        assert(disconnect_calls == 1);
    } else if (!strcmp(argv[1], "event")) {
        c.slots[2]         = &s;
        xhci_transfer_t  t = {.slot = &s, .length = 64, .setup_trb_physical = 0x1000, .data_trb_physical = 0x1010, .trb_physical = 0x1020};
        xhci_transfer_t *deferred[256];
        int              count      = 0;
        xhci_trb_t       e          = {.control = (2U << 24) | (1U << 16)};
        const uint64_t   pointers[] = {0x1000, 0x1010, 0x1020};
        for (unsigned i = 0; i < 3; i++) {
            s.pending[1] = &t;
            t.completed  = false;
            e.parameter  = pointers[i];
            e.status     = (i == 0 ? 6U : 4U) << 24;
            xhci_handle_transfer_event(&c, &e, deferred, &count);
            assert(t.completed);
            assert(t.status == -EIO);
            assert(!s.pending[1]);
            assert(t.completion_code == (i == 0 ? 6 : 4));
        }
        s.pending[1] = &t;
        t.completed  = false;
        e.parameter  = 0x2000;
        e.status     = 6U << 24;
        xhci_handle_transfer_event(&c, &e, deferred, &count);
        assert(!t.completed);
        e.parameter = 0x1010;
        e.status    = 13U << 24;
        xhci_handle_transfer_event(&c, &e, deferred, &count);
        assert(!t.completed);
        assert(s.pending[1] == &t);
        e.parameter = 0x1020;
        e.status    = 1U << 24;
        xhci_handle_transfer_event(&c, &e, deferred, &count);
        assert(t.completed);
        assert(t.status == EOK);
        assert(t.actual == 64);
        t.endpoint   = &c;
        t.completed  = false;
        s.pending[1] = &t;
        e.parameter  = 0x1000;
        e.status     = 6U << 24;
        xhci_handle_transfer_event(&c, &e, deferred, &count);
        assert(!t.completed);
    } else if (!strcmp(argv[1], "descriptor")) {
        usb_device_t d = {.hc_private = &s, .speed = USB_SPEED_FULL};
        for (int mps = 8; mps <= 64; mps *= 2) {
            descriptor_packet = mps;
            command_calls = control_calls = 0;
            descriptor_updated            = false;
            output[stride + 1]            = (8U << 16) | 0x26;
            output[stride + 2]            = 0x12345001;
            output[stride + 3]            = 2;
            assert(xhci_read_device_descriptor(&d) == EOK);
            assert(control_calls == 2);
            assert(command_calls == (mps != 8));
            if (mps != 8) {
                assert(input[0] == 0);
                assert(input[1] == 2);
                assert(input[2 * stride + 1] == ((uint32_t)mps << 16 | 0x26));
                assert(input[2 * stride + 2] == 0x12345001);
                assert(input[2 * stride + 3] == 2);
            }
        }
        descriptor_packet = 12;
        control_calls     = 0;
        assert(xhci_read_device_descriptor(&d) == -EINVAL);
        assert(control_calls == 1);
        descriptor_packet = 64;
        command_result    = -EIO;
        control_calls     = 0;
        assert(xhci_read_device_descriptor(&d) == -EIO);
        assert(control_calls == 1);
        command_result    = 0;
        descriptor_packet = 8;
        descriptor_type   = 2;
        assert(xhci_read_device_descriptor(&d) < 0);
        descriptor_type   = 1;
        descriptor_length = 7;
        assert(xhci_read_device_descriptor(&d) < 0);
        descriptor_length = 18;
        control_result    = -ETIMEDOUT;
        assert(xhci_read_device_descriptor(&d) == -ETIMEDOUT);
        control_result    = 0;
        d.speed           = USB_SPEED_LOW;
        descriptor_packet = 8;
        assert(xhci_read_device_descriptor(&d) == EOK);
        descriptor_packet = 16;
        assert(xhci_read_device_descriptor(&d) == -EINVAL);
        d.speed           = USB_SPEED_HIGH;
        descriptor_packet = 64;
        control_calls     = 0;
        assert(xhci_read_device_descriptor(&d) == EOK);
        assert(control_calls == 1);
        d.speed           = USB_SPEED_SUPER;
        descriptor_packet = 9;
        control_calls     = 0;
        assert(xhci_read_device_descriptor(&d) == EOK);
        assert(control_calls == 1);
    } else
        assert(!"unknown group");
    return 0;
}
