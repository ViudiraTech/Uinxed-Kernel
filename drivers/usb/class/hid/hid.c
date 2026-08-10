/*
 *
 *      hid.c
 *      USB HID transport and Linux evdev binding
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/input/evdev/evdev.h>
#include <drivers/usb/class/hid/usb_hid.h>
#include <drivers/usb/core/usb.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/heap.h>

#define USB_HID_REQ_SET_IDLE     0x0a
#define USB_HID_REQ_SET_PROTOCOL 0x0b
#define USB_HID_REPORT_PROTOCOL  1
#define USB_HID_MAX_REPORT_SIZE  4096
#define USB_HID_EVENT_CAPACITY   128

typedef struct {
        usb_interface_t *interface;
        usb_endpoint_t  *endpoint;
        usb_hid_report_t report;
        uint8_t         *report_descriptor;
        size_t           report_descriptor_length;
        input_dev_t     *input[USB_HID_MAX_APPLICATIONS];
        evdev_t         *evdev[USB_HID_MAX_APPLICATIONS];
        uint8_t          application_count;
        bool             running;
} usb_hid_device_t;

static void hid_input_release(input_dev_t *input)
{
    free(input);
}

static void hid_set_bit(unsigned int bit, uint32_t *bitmap)
{
    bitmap[bit / 32] |= 1U << (bit % 32);
}

static uint16_t hid_usage_at(const usb_hid_field_t *field, size_t index)
{
    if (index < field->usage_count) return field->usages[index];
    if (field->usage_maximum >= field->usage_minimum && field->usage_minimum + index <= field->usage_maximum)
        return field->usage_minimum + (uint16_t)index;
    return 0;
}

static uint16_t hid_usage_page_at(const usb_hid_field_t *field, size_t index)
{
    if (index < field->usage_count) return field->usage_pages[index];
    if (field->usage_minimum_page == field->usage_maximum_page) return field->usage_minimum_page;
    return field->usage_page;
}

static void hid_enable_key(input_dev_t *input, uint16_t keycode)
{
    if (!keycode || keycode >= KEY_CNT) return;
    hid_set_bit(EV_KEY, input->evbit);
    hid_set_bit(keycode, input->keybit);
}

static void hid_enable_axis(input_dev_t *input, const usb_hid_field_t *field, uint16_t usage)
{
    bool     relative = (field->flags & USB_HID_MAIN_RELATIVE) != 0;
    uint16_t code;

    switch (usage) {
        case 0x30 :
            code = relative ? REL_X : ABS_X; // NOLINT(bugprone-branch-clone)
            break;
        case 0x31 :
            code = relative ? REL_Y : ABS_Y; // NOLINT(bugprone-branch-clone)
            break;
        case 0x32 :
            code = relative ? REL_Z : ABS_Z; // NOLINT(bugprone-branch-clone)
            break;
        case 0x33 :
            code = relative ? REL_RX : ABS_RX; // NOLINT(bugprone-branch-clone)
            break;
        case 0x34 :
            code = relative ? REL_RY : ABS_RY; // NOLINT(bugprone-branch-clone)
            break;
        case 0x35 :
            code = relative ? REL_RZ : ABS_RZ; // NOLINT(bugprone-branch-clone)
            break;
        case 0x38 :
            code = relative ? REL_WHEEL : ABS_WHEEL; // NOLINT(bugprone-branch-clone)
            break;
        default :
            return;
    }
    if (relative) {
        if (code >= REL_CNT) return;
        hid_set_bit(EV_REL, input->evbit);
        hid_set_bit(code, input->relbit);
    } else {
        if (code >= ABS_CNT) return;
        hid_set_bit(EV_ABS, input->evbit);
        hid_set_bit(code, input->absbit);
        input->absinfo[code].minimum = field->logical_minimum;
        input->absinfo[code].maximum = field->logical_maximum;
    }
}

static void hid_build_capabilities(usb_hid_device_t *hid)
{
    for (size_t field_index = 0; field_index < hid->report.field_count; field_index++) {
        usb_hid_field_t *field = &hid->report.fields[field_index];
        if (field->application >= hid->application_count) continue;
        input_dev_t *input = hid->input[field->application];

        for (size_t usage_index = 0; usage_index < field->report_count; usage_index++) {
            uint16_t usage = hid_usage_at(field, usage_index);
            switch (hid_usage_page_at(field, usage_index)) {
                case 0x01 :
                    hid_enable_axis(input, field, usage);
                    break;
                case 0x07 :
                    if (field->flags & USB_HID_MAIN_VARIABLE) {
                        hid_enable_key(input, usb_hid_keyboard_keycode(usage));
                    } else {
                        uint16_t minimum = field->usage_minimum;
                        uint16_t maximum = field->usage_maximum;
                        if (maximum > 0xff) maximum = 0xff;
                        for (uint16_t key_usage = minimum; key_usage <= maximum; key_usage++)
                            hid_enable_key(input, usb_hid_keyboard_keycode(key_usage));
                    }
                    break;
                case 0x09 :
                    if (usage >= 1 && usage <= 8) hid_enable_key(input, BTN_LEFT + usage - 1);
                    break;
                case 0x0c :
                    hid_enable_key(input, hid_consumer_keycode(usage));
                    break;
                default :
                    break;
            }
        }
    }
}

static const char *hid_application_name(const usb_hid_application_t *application)
{
    if (application->usage_page == 0x01) {
        if (application->usage == 0x02) return "USB HID Mouse";
        if (application->usage == 0x06) return "USB HID Keyboard";
        if (application->usage == 0x07) return "USB HID Keypad";
        if (application->usage == 0x04 || application->usage == 0x05) return "USB HID Controller";
    }
    if (application->usage_page == 0x0c) return "USB HID Consumer Control";
    return "USB HID Device";
}

static int hid_register_inputs(usb_hid_device_t *hid)
{
    hid->application_count = hid->report.application_count;
    for (size_t i = 0; i < hid->application_count; i++) {
        input_dev_t *input = calloc(1, sizeof(*input));
        if (!input) return -ENOMEM;
        hid->input[i] = input;
        (void)snprintf(input->name, sizeof(input->name), "%s", hid_application_name(&hid->report.applications[i]));
        (void)snprintf(input->phys, sizeof(input->phys), "%s/input%u", hid->interface->device->path,
                       hid->interface->descriptor.interface_number);
        input->id.bustype             = BUS_USB;
        input->id.vendor              = hid->interface->device->descriptor.vendor_id;
        input->id.product             = hid->interface->device->descriptor.product_id;
        input->id.version             = hid->interface->device->descriptor.device_version;
        input->exist                  = true;
        input->release                = hid_input_release;
        input->hint_events_per_packet = 16;
        hid_set_bit(EV_SYN, input->evbit);
    }
    hid_build_capabilities(hid);
    for (size_t i = 0; i < hid->application_count; i++) {
        hid->evdev[i] = evdev_create(hid->input[i]);
        if (!hid->evdev[i]) return -ENOMEM;
        int result = evdev_register(hid->evdev[i]);
        if (result != EOK) return result;
    }
    return EOK;
}

static void hid_unregister_inputs(usb_hid_device_t *hid)
{
    for (size_t i = 0; i < hid->application_count; i++) {
        if (hid->input[i]) hid->input[i]->exist = false;
        if (hid->evdev[i]) {
            evdev_destroy(hid->evdev[i]);
            hid->evdev[i] = NULL;
            hid->input[i] = NULL;
        } else if (hid->input[i]) {
            free(hid->input[i]);
            hid->input[i] = NULL;
        }
    }
}

static void hid_interrupt_complete(usb_endpoint_t *endpoint, const void *data, size_t length, int status, void *context)
{
    (void)endpoint;
    usb_hid_device_t *hid = context;
    usb_hid_event_t   events[USB_HID_EVENT_CAPACITY];
    bool              touched[USB_HID_MAX_APPLICATIONS] = {false};

    if (!hid || !hid->running || status != EOK || !data || !length) return;
    int count = usb_hid_decode_report(&hid->report, data, length, events, USB_HID_EVENT_CAPACITY);
    if (count <= 0) return;
    for (int i = 0; i < count; i++) {
        usb_hid_event_t *event = &events[i];
        if (event->application >= hid->application_count || !hid->input[event->application]) continue;
        evdev_inject_event(hid->input[event->application], event->type, event->code, event->value);
        touched[event->application] = true;
    }
    for (size_t i = 0; i < hid->application_count; i++)
        if (touched[i]) evdev_inject_syn(hid->input[i]);
}

static int hid_report_descriptor_length(const usb_interface_t *interface, uint16_t *report_length)
{
    size_t         length;
    const uint8_t *descriptor = usb_find_extra_descriptor(interface, USB_DT_HID, &length);
    if (!descriptor || length < 9 || descriptor[5] == 0) return -EINVAL;
    size_t offset = 6;
    for (uint8_t index = 0; index < descriptor[5]; index++) {
        if (offset + 3 > length) return -EINVAL;
        if (descriptor[offset] == USB_DT_REPORT) {
            *report_length = (uint16_t)descriptor[offset + 1] | (uint16_t)descriptor[offset + 2] << 8;
            return *report_length && *report_length <= USB_HID_MAX_REPORT_SIZE ? EOK : -E2BIG;
        }
        offset += 3;
    }
    return -ENOENT;
}

int usb_hid_probe(usb_interface_t *interface)
{
#if CONFIG_USB_HID
    uint16_t report_length;
    int      result;

    if (!interface || interface->driver_data || interface->descriptor.interface_class != USB_CLASS_HID) return -EINVAL;
    usb_endpoint_t *endpoint = usb_find_endpoint(interface, USB_ENDPOINT_XFER_INT, true);
    if (!endpoint) return -ENODEV;
    result = hid_report_descriptor_length(interface, &report_length);
    if (result != EOK) return result;

    usb_hid_device_t *hid = calloc(1, sizeof(*hid));
    if (!hid) return -ENOMEM;
    hid->interface         = interface;
    hid->endpoint          = endpoint;
    hid->report_descriptor = malloc(report_length);
    if (!hid->report_descriptor) {
        free(hid);
        return -ENOMEM;
    }
    hid->report_descriptor_length = report_length;
    result = usb_control_msg(interface->device, USB_DIR_IN | USB_TYPE_STANDARD | USB_RECIP_INTERFACE, USB_REQ_GET_DESCRIPTOR, USB_DT_REPORT << 8,
                             interface->descriptor.interface_number, hid->report_descriptor, report_length, USB_CTRL_TIMEOUT_MS);
    if (result != EOK) goto fail;
    result = usb_hid_parse_report_descriptor(hid->report_descriptor, report_length, &hid->report);
    if (result != EOK) goto fail;

    (void)usb_control_msg(interface->device, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, USB_HID_REQ_SET_IDLE, 0,
                          interface->descriptor.interface_number, NULL, 0, USB_CTRL_TIMEOUT_MS);
    (void)usb_control_msg(interface->device, USB_DIR_OUT | USB_TYPE_CLASS | USB_RECIP_INTERFACE, USB_HID_REQ_SET_PROTOCOL,
                          USB_HID_REPORT_PROTOCOL, interface->descriptor.interface_number, NULL, 0, USB_CTRL_TIMEOUT_MS);
    result = hid_register_inputs(hid);
    if (result != EOK) goto fail_inputs;
    interface->driver_data = hid;
    hid->running           = true;
    size_t packet_size     = endpoint->descriptor.max_packet_size & 0x07ff;
    if (!packet_size) {
        result = -EINVAL;
        goto fail_registered;
    }
    result = usb_interrupt_start(endpoint, packet_size, hid_interrupt_complete, hid);
    if (result == EOK) return EOK;

fail_registered:
    hid->running           = false;
    interface->driver_data = NULL;
    hid_unregister_inputs(hid);
    goto fail;
fail_inputs:
    hid_unregister_inputs(hid);
fail:
    free(hid->report_descriptor);
    free(hid);
    return result;
#else
    (void)interface;
    return -ENOSYS;
#endif
}

void usb_hid_disconnect(usb_interface_t *interface)
{
#if CONFIG_USB_HID
    usb_hid_device_t *hid = interface ? interface->driver_data : NULL;
    if (!hid) return;
    hid->running = false;
    usb_interrupt_stop(hid->endpoint);
    hid_unregister_inputs(hid);
    interface->driver_data = NULL;
    free(hid->report_descriptor);
    free(hid);
#else
    (void)interface;
#endif
}
