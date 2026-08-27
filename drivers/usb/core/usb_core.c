/*
 *
 *      usb_core.c
 *      USB host-side device and interface core
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/usb/core/usb.h>
#include <drivers/usb/host/host.h>
#include <fs/sysfs/usb_sysfs.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <kernel/timer/timer.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>

#define USB_MAX_STRING_DESC_SIZE        256
#define USB_MAX_ENDPOINTS_PER_INTERFACE 31

static bool usb_core_ready;

/* Read a little-endian 16-bit value from a USB descriptor. */
uint16_t usb_get_le16(const void *address)
{
    const uint8_t *bytes = address;
    return (uint16_t)bytes[0] | (uint16_t)bytes[1] << 8;
}

/* Mark the USB core ready for device registration (bus registered by usb_sysfs_init). */
int usb_core_init(void)
{
    if (usb_core_ready) return EOK;
    usb_core_ready = true;
    return EOK;
}

/* Issue one control transfer to the HCD. */
int usb_control_msg(usb_device_t *device, uint8_t request_type, uint8_t request, uint16_t value, uint16_t index, void *buffer, uint16_t length, uint32_t timeout_ms)
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

/* Perform one bulk transfer on an endpoint. */
int usb_bulk_msg(usb_endpoint_t *endpoint, void *buffer, size_t length, size_t *actual, uint32_t timeout_ms)
{
    if (actual) *actual = 0;
    if (!endpoint || !endpoint->interface || !endpoint->interface->device || (length && !buffer)) return -EINVAL;
    usb_device_t *device = endpoint->interface->device;
    if (!device->connected || !device->hcd_ops || !device->hcd_ops->transfer) return -ENODEV;
    return device->hcd_ops->transfer(endpoint, buffer, length, actual, timeout_ms);
}

/* Begin periodic interrupt-IN polling on an endpoint. */
int usb_interrupt_start(usb_endpoint_t *endpoint, size_t length, usb_interrupt_complete_t complete, void *context)
{
    if (!endpoint || !endpoint->interface || !endpoint->interface->device || !complete || !length) return -EINVAL;
    usb_device_t *device = endpoint->interface->device;
    if (!device->connected || !device->hcd_ops || !device->hcd_ops->interrupt_start) return -ENODEV;
    return device->hcd_ops->interrupt_start(endpoint, length, complete, context);
}

/* Stop periodic interrupt polling on an endpoint. */
void usb_interrupt_stop(usb_endpoint_t *endpoint)
{
    if (!endpoint || !endpoint->interface || !endpoint->interface->device) return;
    usb_device_t *device = endpoint->interface->device;
    if (device->hcd_ops && device->hcd_ops->interrupt_stop) device->hcd_ops->interrupt_stop(endpoint);
}

/* Clear an endpoint stall via CLEAR_FEATURE, then reset the HCD state. */
int usb_clear_halt(usb_endpoint_t *endpoint)
{
    if (!endpoint || !endpoint->interface || !endpoint->interface->device) return -EINVAL;
    usb_device_t *device = endpoint->interface->device;
    int           status = usb_control_msg(device, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_ENDPOINT, USB_REQ_CLEAR_FEATURE, 0, endpoint->descriptor.endpoint_address, NULL, 0, USB_CTRL_TIMEOUT_MS);
    if (status != EOK) return status;
    if (!device->hcd_ops || !device->hcd_ops->clear_halt) return EOK;
    return device->hcd_ops->clear_halt(endpoint);
}

/* Find an endpoint of the given transfer type and direction. */
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

/* Locate a descriptor of the given type in an interface's extra data. */
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

/* Parse a configuration descriptor into interfaces and endpoints. */
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

/* Register the device and its interfaces with the driver model. */
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
        result                     = kobject_set_name(&interface->dev.kobj, "%s:%u.%u", device->path, interface->descriptor.interface_number, interface->descriptor.alternate_setting);
        if (result != EOK || device_register(&interface->dev) != EOK) continue;
        interface->registered = true;
    }
    return EOK;
}

/* Disable the endpoints configured so far during a failed add. */
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

/* Bring up a device: parse, configure endpoints, and probe interfaces. */
int usb_add_device(usb_device_t *device, const uint8_t *configuration, size_t length)
{
    if (!usb_core_ready || !device || !device->connected || !device->hcd_ops) return -EINVAL;
    if (!device->hcd_ops->configure_endpoint) return -ENOSYS;

    if (!configuration) return -EINVAL;
    if (length < sizeof(usb_config_descriptor_t)) return -EINVAL;

    const usb_config_descriptor_t *config_header = (const usb_config_descriptor_t *)configuration;
    if (config_header->length < sizeof(*config_header) || length < config_header->length) return -EINVAL;

    int result = usb_parse_configuration(device, configuration, length);
    if (result != EOK) {
        plogk("usb: %s: configuration descriptor parse failed: %d\n", device->path, result);
        return result;
    }

    for (size_t interface_index = 0; interface_index < device->interface_count; interface_index++) {
        usb_interface_t *interface = &device->interfaces[interface_index];
        for (size_t endpoint_index = 0; endpoint_index < interface->endpoint_count; endpoint_index++) {
            result = device->hcd_ops->configure_endpoint(&interface->endpoints[endpoint_index]);
            if (result != EOK) {
                plogk("usb: %s: endpoint 0x%02x configuration failed: %d\n", device->path, interface->endpoints[endpoint_index].descriptor.endpoint_address, result);
                usb_cleanup_endpoints(device, interface_index, endpoint_index);
                return result;
            }
        }
    }

    int temp_result
        = usb_control_msg(device, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION, device->configuration.configuration_value, 0, NULL, 0, USB_CTRL_TIMEOUT_MS);
    if (temp_result != EOK) {
        plogk("usb: %s: SET_CONFIGURATION failed: %d\n", device->path, temp_result);
        usb_cleanup_endpoints(device, device->interface_count, 0);
        return temp_result;
    }
    device->configured = true;
    result             = usb_register_device_model(device);
    if (result != EOK) {
        plogk("usb: %s: device model registration failed: %d\n", device->path, result);
        device->configured = false;
        for (size_t i = 0; i < device->interface_count; i++) {
            if (device->interfaces[i].registered) {
                device_unregister(&device->interfaces[i].dev);
                device->interfaces[i].registered = false;
            }
        }
        if (device->hcd_ops && device->hcd_ops->disable_device) device->hcd_ops->disable_device(device);
        usb_cleanup_endpoints(device, device->interface_count, 0);

        temp_result = usb_control_msg(device, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_CONFIGURATION, 0U, 0, NULL, 0, USB_CTRL_TIMEOUT_MS);
        if (temp_result != EOK) plogk("usb: Failed to reset device configuration: %d\n", temp_result);
        return result;
    }

    for (size_t i = 0; i < device->interface_count; i++) {
        usb_interface_t *interface = &device->interfaces[i];
        if (interface->descriptor.interface_class == USB_CLASS_HID)
            (void)usb_hid_probe(interface);
        else if (interface->descriptor.interface_class == USB_CLASS_MASS_STORAGE)
            (void)usb_storage_probe(interface);
        else if (interface->descriptor.interface_class == USB_CLASS_HUB)
            (void)usb_hub_probe(interface);
    }
    return EOK;
}

/* Mark a device offline and disconnect its class drivers. */
void usb_disconnect_device(usb_device_t *device)
{
    if (!device || !device->connected) return;
    plogk("usb: Device %s (addr %u) disconnected.\n", device->path, device->address);
    device->connected = false;
    for (size_t i = 0; i < device->interface_count; i++) {
        usb_interface_t *interface = &device->interfaces[i];
        if (interface->descriptor.interface_class == USB_CLASS_HID)
            usb_hid_disconnect(interface);
        else if (interface->descriptor.interface_class == USB_CLASS_MASS_STORAGE)
            usb_storage_disconnect(interface);
        else if (interface->descriptor.interface_class == USB_CLASS_HUB)
            usb_hub_disconnect(interface);
    }
    if (device->hcd_ops && device->hcd_ops->disable_device) device->hcd_ops->disable_device(device);
}

/* Fetch a string descriptor and convert it to ASCII. */
int usb_get_string_descriptor(usb_device_t *device, uint8_t index, uint16_t language, char *output, size_t capacity)
{
    uint8_t descriptor[USB_MAX_STRING_DESC_SIZE];
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

/* Fetch the full configuration descriptor into a malloc'd buffer. */
int usb_read_config_descriptor(usb_device_t *device, uint8_t **config_out, uint16_t *length_out)
{
    if (!device || !config_out || !length_out) return -EINVAL;
    if (!device->connected || !device->hcd_ops) return -ENODEV;
    usb_config_descriptor_t header;
    int result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, &header, sizeof(header), USB_CTRL_TIMEOUT_MS);
    if (result != EOK) return result;
    uint16_t total_length = usb_get_le16(&header.total_length);
    if (header.descriptor_type != USB_DT_CONFIG || header.length < sizeof(header) || total_length < sizeof(header)) return -EINVAL;
    uint8_t *config = malloc(total_length);
    if (!config) return -ENOMEM;
    result = usb_control_msg(device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_CONFIG << 8, 0, config, total_length, USB_CTRL_TIMEOUT_MS);
    if (result != EOK) {
        free(config);
        return result;
    }
    *config_out = config;
    *length_out = total_length;
    return EOK;
}

/* Device-address helpers — per-bus 1..127 allocator in host.c. */

/* Enumerate a device on a hub downstream port using the hub's HCD.
 * The hub port has already been reset and is enabled at address 0.
 * This helper handles SET_ADDRESS (address 0 → new) + GET_DESCRIPTOR + config.
 * For xHCI the HCD's enumerate hook is used instead (slot-based). */
int usb_enumerate_device(usb_device_t *hub, uint8_t port, usb_speed_t speed, usb_device_t **out)
{
    if (!hub || !hub->connected || !hub->hcd_ops || !out) return -EINVAL;
    if (hub->hcd_ops->enumerate) return hub->hcd_ops->enumerate(hub, port, out);

    uint8_t addr;
    int ret = usb_host_allocate_address(hub->bus_number, &addr);
    if (ret != EOK) return ret;

    usb_device_t *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        usb_host_release_address(hub->bus_number, addr);
        return -ENOMEM;
    }
    dev->connected  = true;
    dev->speed      = speed;
    dev->bus_number = hub->bus_number;
    dev->port_number = port;
    dev->depth      = hub->depth + 1;
    dev->hcd_ops    = hub->hcd_ops;
    dev->hc_private = hub->hc_private;
    // Path: parent path + "." + port, or "bus-port" if parent is root hub (depth 0).
    if (hub->depth == 0) snprintf(dev->path, sizeof(dev->path), "%u-%u", hub->bus_number, port);
    else snprintf(dev->path, sizeof(dev->path), "%s.%u", hub->path, port);

    // SET_ADDRESS at address 0 → new addr. Hub routes address-0 to the reset port.
    ret = usb_control_msg(dev, USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_SET_ADDRESS, addr, 0, NULL, 0, USB_CTRL_TIMEOUT_MS);
    if (ret != EOK) goto fail;
    dev->address = addr;
    msleep(2); // TRSTRCY + recovery per USB 2.0 §9.2.6.3

    // GET_DESCRIPTOR device (18) at new address.
    ret = usb_control_msg(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_DEVICE << 8, 0, &dev->descriptor, sizeof(dev->descriptor), USB_CTRL_TIMEOUT_MS);
    if (ret != EOK || dev->descriptor.length < sizeof(dev->descriptor)) {
        if (ret == EOK) ret = -EPROTO;
        goto fail;
    }
    uint16_t lang = 0x0409;
    uint8_t lang_desc[4];
    if (usb_control_msg(dev, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_DEVICE, USB_REQ_GET_DESCRIPTOR, USB_DT_STRING << 8, 0, lang_desc, sizeof(lang_desc), USB_CTRL_TIMEOUT_MS)==EOK && lang_desc[0]>=4)
        lang = lang_desc[2] | (uint16_t)lang_desc[3]<<8;
    usb_get_string_descriptor(dev, dev->descriptor.manufacturer, lang, dev->manufacturer, sizeof(dev->manufacturer));
    usb_get_string_descriptor(dev, dev->descriptor.product, lang, dev->product, sizeof(dev->product));
    usb_get_string_descriptor(dev, dev->descriptor.serial_number, lang, dev->serial, sizeof(dev->serial));

    uint8_t *config = NULL;
    uint16_t config_len = 0;
    ret = usb_read_config_descriptor(dev, &config, &config_len);
    if (ret != EOK) goto fail;
    ret = usb_add_device(dev, config, config_len);
    free(config);
    if (ret != EOK) goto fail;

    *out = dev;
    return EOK;

fail:
    usb_host_release_address(hub->bus_number, addr);
    free(dev);
    return ret;
}

/* Fully tear down a device and unregister its model entries. */
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
    if (device->address) {
        usb_host_release_address(device->bus_number, device->address);
        device->address = 0;
    }
}
