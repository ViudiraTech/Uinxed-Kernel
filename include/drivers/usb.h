/*
 *
 *      usb.h
 *      USB host core and host-controller interface
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_USB_H_
#define INCLUDE_USB_H_

#include <kernel/device.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define USB_MAX_INTERFACES 8
#define USB_MAX_ENDPOINTS  16
#define USB_MAX_CONTROLLERS 8
#define USB_MAX_DEVICES     64

#define USB_DIR_OUT 0x00
#define USB_DIR_IN  0x80

#define USB_TYPE_STANDARD (0x00 << 5)
#define USB_TYPE_CLASS    (0x01 << 5)
#define USB_RECIP_DEVICE    0x00
#define USB_RECIP_INTERFACE 0x01
#define USB_RECIP_ENDPOINT  0x02

#define USB_REQ_GET_STATUS        0x00
#define USB_REQ_CLEAR_FEATURE     0x01
#define USB_REQ_SET_FEATURE       0x03
#define USB_REQ_SET_ADDRESS       0x05
#define USB_REQ_GET_DESCRIPTOR    0x06
#define USB_REQ_SET_DESCRIPTOR    0x07
#define USB_REQ_GET_CONFIGURATION 0x08
#define USB_REQ_SET_CONFIGURATION 0x09
#define USB_REQ_GET_INTERFACE     0x0a
#define USB_REQ_SET_INTERFACE     0x0b

#define USB_DT_DEVICE    0x01
#define USB_DT_CONFIG    0x02
#define USB_DT_STRING    0x03
#define USB_DT_INTERFACE 0x04
#define USB_DT_ENDPOINT  0x05
#define USB_DT_HID       0x21
#define USB_DT_REPORT    0x22

#define USB_CLASS_HID          0x03
#define USB_CLASS_MASS_STORAGE 0x08

#define USB_ENDPOINT_NUMBER_MASK 0x0f
#define USB_ENDPOINT_DIR_MASK    0x80
#define USB_ENDPOINT_XFERTYPE_MASK 0x03
#define USB_ENDPOINT_XFER_CONTROL 0
#define USB_ENDPOINT_XFER_ISOC    1
#define USB_ENDPOINT_XFER_BULK    2
#define USB_ENDPOINT_XFER_INT     3

#define USB_CTRL_TIMEOUT_MS 1000
#define USB_IO_TIMEOUT_MS   5000

typedef enum {
    USB_SPEED_LOW = 1,
    USB_SPEED_FULL,
    USB_SPEED_HIGH,
    USB_SPEED_SUPER,
    USB_SPEED_SUPER_PLUS,
} usb_speed_t;

typedef struct __attribute__((packed)) {
        uint8_t  request_type;
        uint8_t  request;
        uint16_t value;
        uint16_t index;
        uint16_t length;
} usb_setup_packet_t;

typedef struct __attribute__((packed)) {
        uint8_t  length;
        uint8_t  descriptor_type;
        uint16_t usb_version;
        uint8_t  device_class;
        uint8_t  device_subclass;
        uint8_t  device_protocol;
        uint8_t  max_packet_size0;
        uint16_t vendor_id;
        uint16_t product_id;
        uint16_t device_version;
        uint8_t  manufacturer;
        uint8_t  product;
        uint8_t  serial_number;
        uint8_t  configuration_count;
} usb_device_descriptor_t;

typedef struct __attribute__((packed)) {
        uint8_t  length;
        uint8_t  descriptor_type;
        uint16_t total_length;
        uint8_t  interface_count;
        uint8_t  configuration_value;
        uint8_t  configuration;
        uint8_t  attributes;
        uint8_t  max_power;
} usb_config_descriptor_t;

typedef struct __attribute__((packed)) {
        uint8_t length;
        uint8_t descriptor_type;
        uint8_t interface_number;
        uint8_t alternate_setting;
        uint8_t endpoint_count;
        uint8_t interface_class;
        uint8_t interface_subclass;
        uint8_t interface_protocol;
        uint8_t interface;
} usb_interface_descriptor_t;

typedef struct __attribute__((packed)) {
        uint8_t  length;
        uint8_t  descriptor_type;
        uint8_t  endpoint_address;
        uint8_t  attributes;
        uint16_t max_packet_size;
        uint8_t  interval;
} usb_endpoint_descriptor_t;

struct usb_device;
struct usb_interface;
struct usb_endpoint;

typedef void (*usb_interrupt_complete_t)(struct usb_endpoint *endpoint, const void *data, size_t length, int status, void *context);

typedef struct usb_hcd_ops {
        int (*control)(struct usb_device *device, const usb_setup_packet_t *setup, void *buffer, size_t length, uint32_t timeout_ms);
        int (*transfer)(struct usb_endpoint *endpoint, void *buffer, size_t length, size_t *actual, uint32_t timeout_ms);
        int (*interrupt_start)(struct usb_endpoint *endpoint, size_t length, usb_interrupt_complete_t complete, void *context);
        void (*interrupt_stop)(struct usb_endpoint *endpoint);
        int (*configure_endpoint)(struct usb_endpoint *endpoint);
        int (*clear_halt)(struct usb_endpoint *endpoint);
        void (*disable_device)(struct usb_device *device);
} usb_hcd_ops_t;

typedef struct usb_endpoint {
        struct usb_interface      *interface;
        usb_endpoint_descriptor_t descriptor;
        void                     *hc_private;
} usb_endpoint_t;

typedef struct usb_interface {
        struct usb_device         *device;
        usb_interface_descriptor_t descriptor;
        usb_endpoint_t             endpoints[USB_MAX_ENDPOINTS];
        uint8_t                    endpoint_count;
        const uint8_t             *extra;
        size_t                     extra_length;
        void                      *driver_data;
        struct device              dev;
        bool                       registered;
} usb_interface_t;

typedef struct usb_device {
        usb_device_descriptor_t descriptor;
        usb_config_descriptor_t configuration;
        usb_interface_t         interfaces[USB_MAX_INTERFACES];
        uint8_t                 interface_count;
        uint8_t                 address;
        uint8_t                 bus_number;
        uint8_t                 port_number;
        uint8_t                 depth;
        usb_speed_t             speed;
        char                    manufacturer[64];
        char                    product[64];
        char                    serial[64];
        char                    path[64];
        const usb_hcd_ops_t    *hcd_ops;
        void                   *hc_private;
        struct device           dev;
        bool                    configured;
        bool                    connected;
} usb_device_t;

extern struct bus_type usb_bus_type;

int  usb_core_init(void);
int  usb_add_device(usb_device_t *device, const uint8_t *configuration, size_t length);
void usb_remove_device(usb_device_t *device);

int usb_control_msg(usb_device_t *device, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index,
                    void *buffer, uint16_t length, uint32_t timeout_ms);
int usb_bulk_msg(usb_endpoint_t *endpoint, void *buffer, size_t length, size_t *actual, uint32_t timeout_ms);
int usb_interrupt_start(usb_endpoint_t *endpoint, size_t length, usb_interrupt_complete_t complete, void *context);
void usb_interrupt_stop(usb_endpoint_t *endpoint);
int usb_clear_halt(usb_endpoint_t *endpoint);

usb_endpoint_t *usb_find_endpoint(usb_interface_t *interface, uint8_t transfer_type, bool input);
const uint8_t  *usb_find_extra_descriptor(const usb_interface_t *interface, uint8_t descriptor_type, size_t *length);

int  usb_hid_probe(usb_interface_t *interface);
void usb_hid_disconnect(usb_interface_t *interface);
int  usb_storage_probe(usb_interface_t *interface);
void usb_storage_disconnect(usb_interface_t *interface);

void xhci_init(void);
void xhci_start_workers(void);
void xhci_shutdown(void);

#endif /* INCLUDE_USB_H_ */
