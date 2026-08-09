/*
 *
 *      usb_core.c
 *      USB host-side device and interface core
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/usb/core/usb.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/alloc.h>

#define min(a, b)                       ((a) < (b) ? (a) : (b))
#define USB_MAX_STRING_DESC_SIZE        256
#define USB_MAX_ENDPOINTS_PER_INTERFACE 31

struct bus_type usb_bus_type = {.name = "usb", .dev_name = NULL};
static bool     usb_core_ready;

uint16_t usb_get_le16(const void *address)
{
    const uint8_t *bytes = address;
    return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

static ssize_t id_vendor_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%04x\n", device ? device->descriptor.vendor_id : 0);
}

static ssize_t id_product_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%04x\n", device ? device->descriptor.product_id : 0);
}

static ssize_t product_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%s\n", device ? device->product : "");
}

static ssize_t manufacturer_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%s\n", device ? device->manufacturer : "");
}

static ssize_t serial_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device = dev ? dev->driver_data : NULL;
    return sysfs_emit(buffer, "%s\n", device ? device->serial : "");
}

static ssize_t speed_show(struct device *dev, struct device_attribute *attribute, char *buffer)
{
    (void)attribute;
    usb_device_t *device        = dev ? dev->driver_data : NULL;
    const char   *speed_table[] = {"0", "1.5", "12", "480", "5000", "10000"};
    const char   *speed         = "0";
    if (device && device->speed > 0 && device->speed <= USB_SPEED_SUPER_PLUS) { speed = speed_table[device->speed]; }
    return sysfs_emit(buffer, "%s\n", speed);
}

static DEVICE_ATTR(idVendor, 0444, id_vendor_show, NULL);
static DEVICE_ATTR(idProduct, 0444, id_product_show, NULL);
static DEVICE_ATTR(product, 0444, product_show, NULL);
static DEVICE_ATTR(manufacturer, 0444, manufacturer_show, NULL);
static DEVICE_ATTR(serial, 0444, serial_show, NULL);
static DEVICE_ATTR(speed, 0444, speed_show, NULL);

static struct attribute *usb_device_attributes[] = {
    &dev_attr_idVendor.attr,
    &dev_attr_idProduct.attr,
    &dev_attr_product.attr,
    &dev_attr_manufacturer.attr,
    &dev_attr_serial.attr,
    &dev_attr_speed.attr,
    NULL,
};
static const struct attribute_group  usb_device_group    = {.attrs = usb_device_attributes};
static const struct attribute_group *usb_device_groups[] = {&usb_device_group, NULL};

int usb_core_init(void)
{
    if (usb_core_ready) return EOK;
    int result = bus_register(&usb_bus_type);
    if (result == EOK) usb_core_ready = true;
    return result;
}

int usb_control_msg(usb_device_t *device, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, void *buffer, uint16_t length,
                    uint32_t timeout_ms)
{
    usb_setup_packet_t setup = {
        .request_type = request_type,
        .request      = request,
        .value        = value,
        .index        = index,
        .length       = length,
    };
    if (!device || !device->connected || !device->hcd_ops || !device->hcd_ops->control) return -ENODEV;
    if (length && !buffer) return -EINVAL;
    return device->hcd_ops->control(device, &setup, buffer, length, timeout_ms);
}

int usb_bulk_msg(usb_endpoint_t *endpoint, void *buffer, size_t length, size_t *actual, uint32_t timeout_ms)
{
    if (actual) *actual = 0;
    if (!endpoint || !endpoint->interface || !endpoint->interface->device || (length && !buffer)) return -EINVAL;
    usb_device_t *device = endpoint->interface->device;
    if (!device->connected || !device->hcd_ops || !device->hcd_ops->transfer) return -ENODEV;
    return device->hcd_ops->transfer(endpoint, buffer, length, actual, timeout_ms);
}

int usb_interrupt_start(usb_endpoint_t *endpoint, size_t length, usb_interrupt_complete_t complete, void *context)
{
    if (!endpoint || !endpoint->interface || !endpoint->interface->device || !complete || !length) return -EINVAL;
    usb_device_t *device = endpoint->interface->device;
    if (!device->connected || !device->hcd_ops || !device->hcd_ops->interrupt_start) return -ENODEV;
    return device->hcd_ops->interrupt_start(endpoint, length, complete, context);
}

void usb_interrupt_stop(usb_endpoint_t *endpoint)
{
    if (!endpoint || !endpoint->interface || !endpoint->interface->device) return;
    usb_device_t *device = endpoint->interface->device;
    if (device->hcd_ops && device->hcd_ops->interrupt_stop) device->hcd_ops->interrupt_stop(endpoint);
}

int usb_clear_halt(usb_endpoint_t *endpoint)
{
    if (!endpoint || !endpoint->interface || !endpoint->interface->device) return -EINVAL;
    usb_device_t *device = endpoint->interface->device;
    int           status = usb_control_msg(device, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE, 0,
                                           endpoint->descriptor.endpoint_address, NULL, 0, USB_CTRL_TIMEOUT_MS);
    if (status != EOK) return status;
    if (!device->hcd_ops || !device->hcd_ops->clear_halt) return EOK;
    return device->hcd_ops->clear_halt(endpoint);
}

usb_endpoint_t *usb_find_endpoint(usb_interface_t *interface, uint8_t transfer_type, bool input)
{
    if (!interface) return NULL;
    for (size_t i = 0; i < interface->endpoint_count; i++) {
        usb_endpoint_t *endpoint = &interface->endpoints[i];
        if ((endpoint->descriptor.attributes & USB_ENDPOINT_XFERTYPE_MASK) != transfer_type) continue;
        if (!!(endpoint->descriptor.endpoint_address & USB_ENDPOINT_DIR_MASK) != input) continue;
        return endpoint;
    }
    return NULL;
}

const uint8_t *usb_find_extra_descriptor(const usb_interface_t *interface, uint8_t descriptor_type, size_t *length)
{
    if (length) *length = 0;
    if (!interface || !interface->extra) return NULL;
    size_t offset = 0;
    while (offset + 2 <= interface->extra_length) {
        const uint8_t *descriptor = interface->extra + offset;
        uint8_t        desc_len   = descriptor[0];
        if (desc_len < 2 || desc_len > interface->extra_length - offset) return NULL;
        if (descriptor[1] == descriptor_type) {
            if (length) *length = desc_len;
            return descriptor;
        }
        if (!desc_len) break;
        offset += desc_len;
    }
    return NULL;
}

static int usb_parse_configuration(usb_device_t *device, const uint8_t *buffer, size_t length)
{
    usb_interface_t *interface = NULL;
    size_t           offset    = 0;

    if (!device || !buffer || length < sizeof(usb_config_descriptor_t)) return -EINVAL;
    const usb_config_descriptor_t *configuration = (const usb_config_descriptor_t *)buffer;
    if (configuration->descriptor_type != USB_DT_CONFIG || configuration->length < sizeof(*configuration)) return -EINVAL;
    uint16_t total_length = usb_get_le16(&configuration->total_length);
    if (total_length > length || total_length < configuration->length) return -EINVAL;
    device->configuration = *configuration;

    while (offset + 2 <= total_length) {
        const uint8_t *descriptor        = buffer + offset;
        size_t         descriptor_length = descriptor[0];
        if (descriptor_length < 2 || descriptor_length > total_length - offset) return -EINVAL;

        if (descriptor[1] == USB_DT_INTERFACE && descriptor_length >= sizeof(usb_interface_descriptor_t)) {
            const usb_interface_descriptor_t *source = (const usb_interface_descriptor_t *)descriptor;
            if (source->alternate_setting != 0) {
                interface = NULL;
            } else {
                if (device->interface_count >= USB_MAX_INTERFACES) return -E2BIG;
                interface = &device->interfaces[device->interface_count++];
                memset(interface, 0, sizeof(*interface));
                interface->device     = device;
                interface->descriptor = *source;
                interface->extra      = descriptor + descriptor_length;
            }
        } else if (descriptor[1] == USB_DT_ENDPOINT && descriptor_length >= sizeof(usb_endpoint_descriptor_t) && interface) {
            if (interface->endpoint_count >= USB_MAX_ENDPOINTS_PER_INTERFACE) return -E2BIG;
            usb_endpoint_t *endpoint = &interface->endpoints[interface->endpoint_count++];
            memset(endpoint, 0, sizeof(*endpoint));
            endpoint->interface  = interface;
            endpoint->descriptor = *(const usb_endpoint_descriptor_t *)descriptor;
            if (interface->extra && !interface->extra_length) interface->extra_length = (size_t)(descriptor - interface->extra);
        }
        offset += descriptor_length;
    }
    if (interface && interface->extra && !interface->extra_length) interface->extra_length = (size_t)(buffer + total_length - interface->extra);
    return device->interface_count ? EOK : -EINVAL;
}

static int usb_register_device_model(usb_device_t *device)
{
    int result;

    device->dev.bus         = &usb_bus_type;
    device->dev.driver_data = device;
    device->dev.devid       = ((uint64_t)device->bus_number << 8) | device->address;
    device->dev.groups      = usb_device_groups;
    result                  = kobject_set_name(&device->dev.kobj, "%s", device->path);
    if (result != EOK) return result;
    result = device_register(&device->dev);
    if (result != EOK) return result;
    device->registered = true;

    for (size_t i = 0; i < device->interface_count; i++) {
        usb_interface_t *interface = &device->interfaces[i];
        interface->dev.parent      = &device->dev;
        interface->dev.bus         = &usb_bus_type;
        interface->dev.driver_data = interface;
        interface->dev.devid       = ((uint64_t)device->address << 8) | interface->descriptor.interface_number;
        result                     = kobject_set_name(&interface->dev.kobj, "%s:%u.%u", device->path, interface->descriptor.interface_number,
                                                      interface->descriptor.alternate_setting);
        if (result != EOK || device_register(&interface->dev) != EOK) continue;
        interface->registered = true;
    }
    return EOK;
}

static void usb_cleanup_endpoints(usb_device_t *device, size_t up_to_interface, size_t up_to_endpoint)
{
    for (size_t i = 0; i <= up_to_interface && i < device->interface_count; i++) {
        usb_interface_t *intf  = &device->interfaces[i];
        size_t           limit = i == up_to_interface ? up_to_endpoint : intf->endpoint_count;
        for (size_t j = 0; j < limit; j++) {
            usb_endpoint_t *ep = &intf->endpoints[j];
            if (device->hcd_ops && device->hcd_ops->disable_endpoint) device->hcd_ops->disable_endpoint(ep);
            ep->hc_private = NULL;
        }
    }
}

int usb_add_device(usb_device_t *device, const uint8_t *configuration, size_t length)
{
    if (!usb_core_ready || !device || !device->connected || !device->hcd_ops) return -EINVAL;
    if (!device->hcd_ops->configure_endpoint) return -ENOSYS;

    if (!configuration) return -EINVAL;
    if (length < sizeof(usb_config_descriptor_t)) return -EINVAL;

    const usb_config_descriptor_t *config_header = (const usb_config_descriptor_t *)configuration;
    if (config_header->length < sizeof(*config_header) || length < config_header->length) return -EINVAL;

    int result = usb_parse_configuration(device, configuration, length);
    if (result != EOK) return result;

    for (size_t interface_index = 0; interface_index < device->interface_count; interface_index++) {
        usb_interface_t *interface = &device->interfaces[interface_index];
        for (size_t endpoint_index = 0; endpoint_index < interface->endpoint_count; endpoint_index++) {
            result = device->hcd_ops->configure_endpoint(&interface->endpoints[endpoint_index]);
            if (result != EOK) {
                usb_cleanup_endpoints(device, interface_index, endpoint_index);
                return result;
            }
        }
    }

    int temp_result = usb_control_msg(device, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION,
                                      device->configuration.configuration_value, 0, NULL, 0, USB_CTRL_TIMEOUT_MS);
    if (temp_result != EOK) {
        usb_cleanup_endpoints(device, device->interface_count, 0);
        return temp_result;
    }
    device->configured = true;
    result             = usb_register_device_model(device);
    if (result != EOK) {
        device->configured = false;
        for (size_t i = 0; i < device->interface_count; i++) {
            if (device->interfaces[i].registered) {
                device_unregister(&device->interfaces[i].dev);
                device->interfaces[i].registered = false;
            }
        }
        if (device->hcd_ops && device->hcd_ops->disable_device) device->hcd_ops->disable_device(device);
        usb_cleanup_endpoints(device, device->interface_count, 0);

        temp_result = usb_control_msg(device, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION, 0U, 0, NULL, 0,
                                      USB_CTRL_TIMEOUT_MS);
        if (temp_result != EOK) { plogk("usb-core: Failed to reset device configuration: %d\n", temp_result); }
        return result;
    }

    for (size_t i = 0; i < device->interface_count; i++) {
        usb_interface_t *interface = &device->interfaces[i];
        if (interface->descriptor.interface_class == USB_CLASS_HID)
            (void)usb_hid_probe(interface);
        else if (interface->descriptor.interface_class == USB_CLASS_MASS_STORAGE)
            (void)usb_storage_probe(interface);
    }
    return EOK;
}

void usb_disconnect_device(usb_device_t *device)
{
    if (!device || !device->connected) return;
    plogk("usb-core: device %s (addr %u) disconnected\n", device->path, device->address);
    device->connected = false;
    for (size_t i = 0; i < device->interface_count; i++) {
        usb_interface_t *interface = &device->interfaces[i];
        if (interface->descriptor.interface_class == USB_CLASS_HID)
            usb_hid_disconnect(interface);
        else if (interface->descriptor.interface_class == USB_CLASS_MASS_STORAGE)
            usb_storage_disconnect(interface);
    }
    if (device->hcd_ops && device->hcd_ops->disable_device) device->hcd_ops->disable_device(device);
}

int usb_get_string_descriptor(usb_device_t *device, uint8_t index, uint16_t language, char *output, size_t capacity)
{
    uint8_t descriptor[USB_MAX_STRING_DESC_SIZE];
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

int usb_read_config_descriptor(usb_device_t *device, uint8_t **config_out, uint16_t *length_out)
{
    if (!device || !config_out || !length_out) return -EINVAL;
    if (!device->connected || !device->hcd_ops) return -ENODEV;
    usb_config_descriptor_t header;
    int result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0,
                                 &header, sizeof(header), USB_CTRL_TIMEOUT_MS);
    if (result != EOK) return result;
    uint16_t total_length = usb_get_le16(&header.total_length);
    if (header.descriptor_type != USB_DT_CONFIG || header.length < sizeof(header) || total_length < sizeof(header)) return -EINVAL;
    uint8_t *config = malloc(total_length);
    if (!config) return -ENOMEM;
    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, config,
                             total_length, USB_CTRL_TIMEOUT_MS);
    if (result != EOK) {
        free(config);
        return result;
    }
    *config_out = config;
    *length_out = total_length;
    return EOK;
}

void usb_remove_device(usb_device_t *device)
{
    if (!device) return;
    if (device->connected) usb_disconnect_device(device);
    for (size_t i = 0; i < device->interface_count; i++) {
        usb_interface_t *intf = &device->interfaces[i];
        if (intf->registered) {
            device_unregister(&intf->dev);
            intf->registered = false;
        }
    }
    device->configured = false;
    if (device->registered) {
        device->registered = false;
        device_unregister(&device->dev);
    }
}
