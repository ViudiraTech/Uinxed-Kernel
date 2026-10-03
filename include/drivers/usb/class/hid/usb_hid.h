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

#define USB_HID_MAX_FIELDS       64
#define USB_HID_MAX_USAGES       32
#define USB_HID_MAX_APPLICATIONS 8
#define USB_HID_MAX_REPORT_IDS   256

#define HID_ITEM_TYPE_MAIN             0
#define HID_ITEM_TYPE_GLOBAL           1
#define HID_ITEM_TYPE_LOCAL            2
#define HID_MAIN_INPUT                 8
#define HID_MAIN_OUTPUT                9
#define HID_MAIN_COLLECTION            10
#define HID_MAIN_FEATURE               11
#define HID_MAIN_END_COLLECTION        12
#define HID_GLOBAL_USAGE_PAGE          0
#define HID_GLOBAL_LOGICAL_MIN         1
#define HID_GLOBAL_LOGICAL_MAX         2
#define HID_GLOBAL_REPORT_SIZE         7
#define HID_GLOBAL_REPORT_ID           8
#define HID_GLOBAL_REPORT_COUNT        9
#define HID_GLOBAL_PUSH                10
#define HID_GLOBAL_POP                 11
#define HID_LOCAL_USAGE                0
#define HID_LOCAL_USAGE_MIN            1
#define HID_LOCAL_USAGE_MAX            2
#define HID_COLLECTION_APPLICATION     1
#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_PAGE_KEYBOARD        0x07
#define HID_USAGE_PAGE_BUTTON          0x09
#define HID_USAGE_PAGE_CONSUMER        0x0c

#define USB_HID_MAIN_CONSTANT 0x01
#define USB_HID_MAIN_VARIABLE 0x02
#define USB_HID_MAIN_RELATIVE 0x04

/* Class requests and report/protocol selectors (USB HID 1.11 section 7.2) */
#define USB_HID_REQ_SET_REPORT     0x09
#define USB_HID_REQ_SET_IDLE       0x0a
#define USB_HID_REQ_SET_PROTOCOL   0x0b
#define USB_HID_REPORT_TYPE_OUTPUT 0x02
#define USB_HID_REPORT_PROTOCOL    1

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

/* Return the usage page of a field's usage at `index`. */
uint16_t hid_field_usage_page(const usb_hid_field_t *field, size_t index);

#endif // INCLUDE_USB_HID_H_
