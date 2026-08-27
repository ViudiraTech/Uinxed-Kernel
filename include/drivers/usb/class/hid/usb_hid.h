/*
 *
 *      usb_hid.h
 *      USB Human Interface Device report parser
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_USB_HID_H_
#define INCLUDE_USB_HID_H_

#include <drivers/input/input_event.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define USB_HID_MAX_FIELDS       64
#define USB_HID_MAX_USAGES       32
#define USB_HID_MAX_APPLICATIONS 8
#define USB_HID_MAX_REPORT_IDS   256

#define USB_HID_MAIN_CONSTANT 0x01
#define USB_HID_MAIN_VARIABLE 0x02
#define USB_HID_MAIN_RELATIVE 0x04

typedef struct {
        uint16_t usage_page;
        uint16_t usage;
} usb_hid_application_t;

typedef struct {
        uint8_t  report_id;
        uint8_t  application;
        uint16_t usage_page;
        uint16_t usages[USB_HID_MAX_USAGES];
        uint16_t usage_pages[USB_HID_MAX_USAGES];
        uint16_t usage_count;
        uint16_t usage_minimum;
        uint16_t usage_maximum;
        uint16_t usage_minimum_page;
        uint16_t usage_maximum_page;
        uint16_t bit_offset;
        uint8_t  report_size;
        uint8_t  report_count;
        uint8_t  flags;
        int32_t  logical_minimum;
        int32_t  logical_maximum;
        uint32_t previous[USB_HID_MAX_USAGES];
        uint8_t  previous_count;
} usb_hid_field_t;

typedef struct {
        usb_hid_field_t       fields[USB_HID_MAX_FIELDS];
        usb_hid_application_t applications[USB_HID_MAX_APPLICATIONS];
        uint16_t              report_bits[USB_HID_MAX_REPORT_IDS];
        uint8_t               field_count;
        uint8_t               application_count;
        bool                  numbered_reports;
        bool                  has_output;
} usb_hid_report_t;

typedef struct {
        uint8_t  application;
        uint16_t type;
        uint16_t code;
        int32_t  value;
} usb_hid_event_t;

/* Parse an HID report descriptor into field/application tables. */
int usb_hid_parse_report_descriptor(const uint8_t *descriptor, size_t length, usb_hid_report_t *report);

/* Decode one input report into a list of evdev events. */
int usb_hid_decode_report(usb_hid_report_t *report, const uint8_t *data, size_t length, usb_hid_event_t *events, size_t event_capacity);

/* Map a keyboard usage index to an evdev KEY_* code. */
uint16_t usb_hid_keyboard_keycode(uint16_t usage);

/* Map a consumer-control usage to an evdev KEY_* code. */
uint16_t hid_consumer_keycode(uint16_t usage);

#endif // INCLUDE_USB_HID_H_
