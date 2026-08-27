/*
 *
 *      hid_parser.c
 *      USB HID report descriptor and input report decoder
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/usb/class/hid/usb_hid.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>

#define HID_ITEM_TYPE_MAIN   0
#define HID_ITEM_TYPE_GLOBAL 1
#define HID_ITEM_TYPE_LOCAL  2

#define HID_MAIN_INPUT          8
#define HID_MAIN_OUTPUT         9
#define HID_MAIN_COLLECTION     10
#define HID_MAIN_FEATURE        11
#define HID_MAIN_END_COLLECTION 12

#define HID_GLOBAL_USAGE_PAGE   0
#define HID_GLOBAL_LOGICAL_MIN  1
#define HID_GLOBAL_LOGICAL_MAX  2
#define HID_GLOBAL_REPORT_SIZE  7
#define HID_GLOBAL_REPORT_ID    8
#define HID_GLOBAL_REPORT_COUNT 9
#define HID_GLOBAL_PUSH         10
#define HID_GLOBAL_POP          11

#define HID_LOCAL_USAGE     0
#define HID_LOCAL_USAGE_MIN 1
#define HID_LOCAL_USAGE_MAX 2

#define HID_COLLECTION_APPLICATION 1

#define HID_USAGE_PAGE_GENERIC_DESKTOP 0x01
#define HID_USAGE_PAGE_KEYBOARD        0x07
#define HID_USAGE_PAGE_BUTTON          0x09
#define HID_USAGE_PAGE_CONSUMER        0x0c

typedef struct {
        uint16_t usage_page;
        int32_t  logical_minimum;
        int32_t  logical_maximum;
        uint8_t  report_size;
        uint8_t  report_count;
        uint8_t  report_id;
} hid_global_state_t;

typedef struct {
        uint16_t usages[USB_HID_MAX_USAGES];
        uint16_t usage_pages[USB_HID_MAX_USAGES];
        uint8_t  usage_count;
        uint16_t usage_minimum;
        uint16_t usage_maximum;
        uint16_t usage_minimum_page;
        uint16_t usage_maximum_page;
        bool     has_usage_minimum;
        bool     has_usage_maximum;
} hid_local_state_t;

/* Assemble a little-endian unsigned item value from raw bytes. */
static uint32_t hid_unsigned_value(const uint8_t *data, size_t size)
{
    uint32_t value = 0;

    for (size_t i = 0; i < size; i++) value |= (uint32_t)data[i] << (i * 8);
    return value;
}

/* Sign-extend an item value according to its byte size. */
static int32_t hid_signed_value(const uint8_t *data, size_t size)
{
    uint32_t value = hid_unsigned_value(data, size);

    if (!size || size >= sizeof(value)) return (int32_t)value;
    uint32_t sign = 1U << (size * 8 - 1);
    if (value & sign) value |= UINT32_MAX << (size * 8);
    return (int32_t)value;
}

/* Usage-page of a local item: 32-bit values carry it explicitly. */
static uint16_t hid_local_usage_page(uint32_t value, size_t size, uint16_t global_page)
{
    return size == 4 ? (uint16_t)(value >> 16) : global_page;
}

/* Reset the local item state before each main item. */
static void hid_local_reset(hid_local_state_t *local)
{
    memset(local, 0, sizeof(*local));
}

/* Resolve a field's usage for an index, expanding usage ranges. */
static uint16_t hid_field_usage(const usb_hid_field_t *field, size_t index)
{
    if (index < field->usage_count) return field->usages[index];
    if (field->usage_maximum >= field->usage_minimum) {
        uint32_t usage = (uint32_t)field->usage_minimum + (uint32_t)index;
        if (usage <= field->usage_maximum) return (uint16_t)usage;
    }
    return field->usage_count ? field->usages[field->usage_count - 1] : 0;
}

/* Resolve a field's usage page for an index. */
static uint16_t hid_field_usage_page(const usb_hid_field_t *field, size_t index)
{
    if (index < field->usage_count) return field->usage_pages[index];
    if (field->usage_minimum_page == field->usage_maximum_page) return field->usage_minimum_page;
    return field->usage_page;
}

/* Record one input field from the current global and local state. */
static int hid_add_input_field(usb_hid_report_t *report, const hid_global_state_t *global, const hid_local_state_t *local, uint8_t application, uint8_t flags)
{
    uint32_t bits = (uint32_t)global->report_size * global->report_count;

    if (bits > UINT16_MAX || report->report_bits[global->report_id] > UINT16_MAX - bits) {
        plogk("usb-hid: add_input_field: report bits overflow (report_id=%u, bits=%u)\n", global->report_id, (unsigned)bits);
        return -EOVERFLOW;
    }
    if (!(flags & USB_HID_MAIN_CONSTANT)) {
        if (!global->report_size || !global->report_count || global->report_size > 32) {
            plogk("usb-hid: add_input_field: invalid report size/count (size=%u, count=%u)\n", global->report_size, global->report_count);
            return -EINVAL;
        }
        if (global->report_count > USB_HID_MAX_USAGES || report->field_count >= USB_HID_MAX_FIELDS) {
            plogk("usb-hid: add_input_field: too many usages/fields (report_count=%u, field_count=%u)\n", global->report_count, report->field_count);
            return -E2BIG;
        }

        usb_hid_field_t *field = &report->fields[report->field_count++];
        memset(field, 0, sizeof(*field));
        field->report_id          = global->report_id;
        field->application        = application;
        field->usage_page         = global->usage_page;
        field->usage_count        = local->usage_count;
        field->usage_minimum      = local->has_usage_minimum ? local->usage_minimum : 0;
        field->usage_maximum      = local->has_usage_maximum ? local->usage_maximum : 0;
        field->usage_minimum_page = local->has_usage_minimum ? local->usage_minimum_page : global->usage_page;
        field->usage_maximum_page = local->has_usage_maximum ? local->usage_maximum_page : global->usage_page;
        field->bit_offset         = report->report_bits[global->report_id];
        field->report_size        = global->report_size;
        field->report_count       = global->report_count;
        field->flags              = flags;
        field->logical_minimum    = global->logical_minimum;
        field->logical_maximum    = global->logical_maximum;
        memcpy(field->usages, local->usages, sizeof(uint16_t) * local->usage_count);
        memcpy(field->usage_pages, local->usage_pages, sizeof(uint16_t) * local->usage_count);
    }
    report->report_bits[global->report_id] += (uint16_t)bits;
    return EOK;
}

/* Parse a HID report descriptor into the fields and applications list. */
int usb_hid_parse_report_descriptor(const uint8_t *descriptor, size_t length, usb_hid_report_t *report)
{
    hid_global_state_t globals[5] = {0};
    hid_local_state_t  local;
    uint8_t            global_depth        = 0;
    uint8_t            applications[16]    = {0};
    uint8_t            collection_depth    = 0;
    uint8_t            current_application = 0;
    size_t             offset              = 0;

    if (!descriptor || !report) {
        plogk("usb-hid: parse_report: null descriptor or report.\n");
        return -EINVAL;
    }
    memset(report, 0, sizeof(*report));
    hid_local_reset(&local);

    while (offset < length) {
        uint8_t prefix = descriptor[offset++];
        if (prefix == 0xfe) {
            if (length - offset < 2) {
                plogk("usb-hid: parse_report: truncated long item header (offset=%lu)\n", (unsigned long)offset);
                return -EINVAL;
            }
            size_t item_length = descriptor[offset];
            offset += 2;
            if (item_length > length - offset) {
                plogk("usb-hid: parse_report: long item exceeds descriptor (item_length=%u, offset=%lu)\n", (unsigned)item_length, (unsigned long)offset);
                return -EINVAL;
            }
            offset += item_length;
            continue;
        }

        size_t size = prefix & 3;
        if (size == 3) size = 4;
        if (size > length - offset) {
            plogk("usb-hid: parse_report: item data exceeds descriptor (offset=%lu)\n", (unsigned long)offset);
            return -EINVAL;
        }
        uint8_t  type         = (prefix >> 2) & 3;
        uint8_t  tag          = prefix >> 4;
        uint32_t value        = hid_unsigned_value(descriptor + offset, size);
        int32_t  signed_value = hid_signed_value(descriptor + offset, size);
        offset += size;

        hid_global_state_t *global = &globals[global_depth];
        if (type == HID_ITEM_TYPE_GLOBAL) {
            switch (tag) {
                case HID_GLOBAL_USAGE_PAGE :
                    global->usage_page = (uint16_t)value;
                    break;
                case HID_GLOBAL_LOGICAL_MIN :
                    global->logical_minimum = signed_value;
                    break;
                case HID_GLOBAL_LOGICAL_MAX :
                    global->logical_maximum = global->logical_minimum < 0 ? signed_value : (int32_t)value;
                    break;
                case HID_GLOBAL_REPORT_SIZE :
                    if (value > 32) {
                        plogk("usb-hid: parse_report: report size too large (size=%u)\n", (unsigned)value);
                        return -EINVAL;
                    }
                    global->report_size = (uint8_t)value;
                    break;
                case HID_GLOBAL_REPORT_ID :
                    if (!value || value >= USB_HID_MAX_REPORT_IDS) {
                        plogk("usb-hid: parse_report: invalid report id (value=%u)\n", (unsigned)value);
                        return -EINVAL;
                    }
                    global->report_id        = (uint8_t)value;
                    report->numbered_reports = true;
                    break;
                case HID_GLOBAL_REPORT_COUNT :
                    if (value > USB_HID_MAX_USAGES) {
                        plogk("usb-hid: parse_report: report count too large (count=%u)\n", (unsigned)value);
                        return -E2BIG;
                    }
                    global->report_count = (uint8_t)value;
                    break;
                case HID_GLOBAL_PUSH :
                    if ((size_t)global_depth + 1 >= sizeof(globals) / sizeof(globals[0])) {
                        plogk("usb-hid: parse_report: global state push overflow (depth=%u)\n", (unsigned)global_depth);
                        return -E2BIG;
                    }
                    globals[global_depth + 1] = *global;
                    global_depth++;
                    break;
                case HID_GLOBAL_POP :
                    if (!global_depth) {
                        plogk("usb-hid: parse_report: global state pop underflow.\n");
                        return -EINVAL;
                    }
                    global_depth--;
                    break;
                default :
                    break;
            }
            continue;
        }

        if (type == HID_ITEM_TYPE_LOCAL) {
            uint16_t usage      = (uint16_t)value;
            uint16_t usage_page = hid_local_usage_page(value, size, global->usage_page);
            switch (tag) {
                case HID_LOCAL_USAGE :
                    if (local.usage_count >= USB_HID_MAX_USAGES) {
                        plogk("usb-hid: parse_report: too many local usages (count=%u)\n", (unsigned)local.usage_count);
                        return -E2BIG;
                    }
                    local.usages[local.usage_count]      = usage;
                    local.usage_pages[local.usage_count] = usage_page;
                    local.usage_count++;
                    break;
                case HID_LOCAL_USAGE_MIN :
                    local.usage_minimum      = usage;
                    local.usage_minimum_page = usage_page;
                    local.has_usage_minimum  = true;
                    break;
                case HID_LOCAL_USAGE_MAX :
                    local.usage_maximum      = usage;
                    local.usage_maximum_page = usage_page;
                    local.has_usage_maximum  = true;
                    break;
                default :
                    break;
            }
            continue;
        }

        if (type != HID_ITEM_TYPE_MAIN) continue;
        switch (tag) {
            case HID_MAIN_INPUT : {
                int result = hid_add_input_field(report, global, &local, current_application, (uint8_t)value);
                if (result != EOK) return result;
                break;
            }
            case HID_MAIN_OUTPUT :
                // HID Output report (e.g., keyboard LEDs). Record existence for SET_REPORT handling
                // per HID 1.11 §7.2; do not affect Input report_bits.
                report->has_output = true;
                break;
            case HID_MAIN_FEATURE :
                // Feature reports are ignored for Input parsing but reset local state.
                break;
            case HID_MAIN_COLLECTION :
                if (collection_depth >= sizeof(applications)) {
                    plogk("usb-hid: parse_report: collection nesting too deep (depth=%u)\n", (unsigned)collection_depth);
                    return -E2BIG;
                }
                applications[collection_depth++] = current_application;
                if ((uint8_t)value == HID_COLLECTION_APPLICATION) {
                    if (report->application_count >= USB_HID_MAX_APPLICATIONS) {
                        plogk("usb-hid: parse_report: too many applications (count=%u)\n", (unsigned)report->application_count);
                        return -E2BIG;
                    }
                    current_application                = report->application_count++;
                    usb_hid_application_t *application = &report->applications[current_application];
                    application->usage_page            = local.usage_count ? local.usage_pages[0] : (local.has_usage_minimum ? local.usage_minimum_page : global->usage_page);
                    application->usage                 = local.usage_count ? local.usages[0] : (local.has_usage_minimum ? local.usage_minimum : 0);
                }
                break;
            case HID_MAIN_END_COLLECTION :
                if (!collection_depth) {
                    plogk("usb-hid: parse_report: end collection without matching start.\n");
                    return -EINVAL;
                }
                current_application = applications[--collection_depth];
                break;
            default :
                break;
        }
        hid_local_reset(&local);
    }
    if (collection_depth || global_depth) {
        plogk("usb-hid: parse_report: unterminated collection or global state (collection=%u, global=%u)\n", (unsigned)collection_depth, (unsigned)global_depth);
        return -EINVAL;
    }
    if (!report->application_count) report->application_count = 1;
    if (!report->field_count) {
        plogk("usb-hid: parse_report: no input fields in descriptor.\n");
        return -EINVAL;
    }
    return EOK;
}

/* Map a keyboard usage index to an evdev KEY_* code. */
uint16_t usb_hid_keyboard_keycode(uint16_t usage)
{
    static const uint16_t keycodes[256] = {
        [0x04] = KEY_A,          [0x05] = KEY_B,          [0x06] = KEY_C,          [0x07] = KEY_D,
        [0x08] = KEY_E,          [0x09] = KEY_F,          [0x0a] = KEY_G,          [0x0b] = KEY_H,
        [0x0c] = KEY_I,          [0x0d] = KEY_J,          [0x0e] = KEY_K,          [0x0f] = KEY_L,
        [0x10] = KEY_M,          [0x11] = KEY_N,          [0x12] = KEY_O,          [0x13] = KEY_P,
        [0x14] = KEY_Q,          [0x15] = KEY_R,          [0x16] = KEY_S,          [0x17] = KEY_T,
        [0x18] = KEY_U,          [0x19] = KEY_V,          [0x1a] = KEY_W,          [0x1b] = KEY_X,
        [0x1c] = KEY_Y,          [0x1d] = KEY_Z,          [0x1e] = KEY_1,          [0x1f] = KEY_2,
        [0x20] = KEY_3,          [0x21] = KEY_4,          [0x22] = KEY_5,          [0x23] = KEY_6,
        [0x24] = KEY_7,          [0x25] = KEY_8,          [0x26] = KEY_9,          [0x27] = KEY_0,
        [0x28] = KEY_ENTER,      [0x29] = KEY_ESC,        [0x2a] = KEY_BACKSPACE,  [0x2b] = KEY_TAB,
        [0x2c] = KEY_SPACE,      [0x2d] = KEY_MINUS,      [0x2e] = KEY_EQUAL,      [0x2f] = KEY_LEFTBRACE,
        [0x30] = KEY_RIGHTBRACE, [0x31] = KEY_BACKSLASH,  [0x32] = KEY_102ND,      [0x33] = KEY_SEMICOLON,
        [0x34] = KEY_APOSTROPHE, [0x35] = KEY_GRAVE,      [0x36] = KEY_COMMA,      [0x37] = KEY_DOT,
        [0x38] = KEY_SLASH,      [0x39] = KEY_CAPSLOCK,   [0x3a] = KEY_F1,         [0x3b] = KEY_F2,
        [0x3c] = KEY_F3,         [0x3d] = KEY_F4,         [0x3e] = KEY_F5,         [0x3f] = KEY_F6,
        [0x40] = KEY_F7,         [0x41] = KEY_F8,         [0x42] = KEY_F9,         [0x43] = KEY_F10,
        [0x44] = KEY_F11,        [0x45] = KEY_F12,        [0x46] = KEY_SYSRQ,      [0x47] = KEY_SCROLLLOCK,
        [0x48] = KEY_PAUSE,      [0x49] = KEY_INSERT,     [0x4a] = KEY_HOME,       [0x4b] = KEY_PAGEUP,
        [0x4c] = KEY_DELETE,     [0x4d] = KEY_END,        [0x4e] = KEY_PAGEDOWN,   [0x4f] = KEY_RIGHT,
        [0x50] = KEY_LEFT,       [0x51] = KEY_DOWN,       [0x52] = KEY_UP,         [0x53] = KEY_NUMLOCK,
        [0x54] = KEY_KPSLASH,    [0x55] = KEY_KPASTERISK, [0x56] = KEY_KPMINUS,    [0x57] = KEY_KPPLUS,
        [0x58] = KEY_KPENTER,    [0x59] = KEY_KP1,        [0x5a] = KEY_KP2,        [0x5b] = KEY_KP3,
        [0x5c] = KEY_KP4,        [0x5d] = KEY_KP5,        [0x5e] = KEY_KP6,        [0x5f] = KEY_KP7,
        [0x60] = KEY_KP8,        [0x61] = KEY_KP9,        [0x62] = KEY_KP0,        [0x63] = KEY_KPDOT,
        [0x64] = KEY_102ND,      [0x65] = KEY_COMPOSE,    [0x66] = KEY_POWER,      [0x67] = KEY_KPEQUAL,
        [0x68] = KEY_F13,        [0x69] = KEY_F14,        [0x6a] = KEY_F15,        [0x6b] = KEY_F16,
        [0x6c] = KEY_F17,        [0x6d] = KEY_F18,        [0x6e] = KEY_F19,        [0x6f] = KEY_F20,
        [0x70] = KEY_F21,        [0x71] = KEY_F22,        [0x72] = KEY_F23,        [0x73] = KEY_F24,
        [0x75] = KEY_HELP,       [0x76] = KEY_MENU,       [0x7a] = KEY_UNDO,       [0x7b] = KEY_CUT,
        [0x7c] = KEY_COPY,       [0x7d] = KEY_PASTE,      [0x7e] = KEY_FIND,       [0x7f] = KEY_MUTE,
        [0x80] = KEY_VOLUMEUP,   [0x81] = KEY_VOLUMEDOWN, [0x87] = KEY_RO,         [0x88] = KEY_KATAKANAHIRAGANA,
        [0x89] = KEY_YEN,        [0x8a] = KEY_HENKAN,     [0x8b] = KEY_MUHENKAN,   [0x90] = KEY_HANGEUL,
        [0x91] = KEY_HANJA,      [0xe0] = KEY_LEFTCTRL,   [0xe1] = KEY_LEFTSHIFT,  [0xe2] = KEY_LEFTALT,
        [0xe3] = KEY_LEFTMETA,   [0xe4] = KEY_RIGHTCTRL,  [0xe5] = KEY_RIGHTSHIFT, [0xe6] = KEY_RIGHTALT,
        [0xe7] = KEY_RIGHTMETA,
    };

    return usage < sizeof(keycodes) / sizeof(keycodes[0]) ? keycodes[usage] : 0;
}

/* Map a consumer-control usage to an evdev KEY_* code. */
uint16_t hid_consumer_keycode(uint16_t usage)
{
    switch (usage) {
        case 0x030 :
            return KEY_POWER;
        case 0x0b0 :
            return KEY_PLAY;
        case 0x0b1 :
            return KEY_PAUSECD;
        case 0x0b5 :
            return KEY_NEXTSONG;
        case 0x0b6 :
            return KEY_PREVIOUSSONG;
        case 0x0b7 :
            return KEY_STOPCD;
        case 0x0cd :
            return KEY_PLAYPAUSE;
        case 0x0e2 :
            return KEY_MUTE;
        case 0x0e9 :
            return KEY_VOLUMEUP;
        case 0x0ea :
            return KEY_VOLUMEDOWN;
        case 0x183 :
            return KEY_CONFIG;
        case 0x18a :
            return KEY_MAIL;
        case 0x192 :
            return KEY_CALC;
        case 0x194 :
            return KEY_FILE;
        case 0x221 :
            return KEY_SEARCH;
        case 0x223 :
            return KEY_HOMEPAGE;
        case 0x224 :
            return KEY_BACK;
        case 0x225 :
            return KEY_FORWARD;
        case 0x227 :
            return KEY_REFRESH;
        case 0x22a :
            return KEY_BOOKMARKS;
        default :
            return 0;
    }
}

/* Extract a bit field from the report data at the given bit offset. */
static uint32_t hid_extract_bits(const uint8_t *data, size_t length, uint16_t bit_offset, uint8_t bit_size)
{
    uint32_t value = 0;

    for (uint8_t bit = 0; bit < bit_size; bit++) {
        size_t source = bit_offset + bit;
        if (source / 8 >= length) break;
        if (data[source / 8] & (1U << (source & 7))) value |= 1U << bit;
    }
    return value;
}

/* Convert a raw field value to signed when the logical range is signed. */
static int32_t hid_field_value(const usb_hid_field_t *field, uint32_t value)
{
    if (field->logical_minimum >= 0 || field->report_size == 32 || field->report_size == 0) return (int32_t)value;
    uint32_t sign = 1U << (field->report_size - 1);
    if (value & sign) value |= UINT32_MAX << field->report_size;
    return (int32_t)value;
}

/* True when value appears in the array. */
static bool hid_array_contains(const uint32_t *values, size_t count, uint32_t value)
{
    for (size_t i = 0; i < count; i++)
        if (values[i] == value) return true;
    return false;
}

/* Append one decoded event, skipping invalid codes and full buffers. */
static void hid_emit(usb_hid_event_t *events, size_t capacity, size_t *count, uint8_t application, uint16_t type, uint16_t code, int32_t value)
{
    if (!type || (type == EV_KEY && !code) || *count >= capacity) return;
    events[(*count)++] = (usb_hid_event_t) {.application = application, .type = type, .code = code, .value = value};
}

/* Decode an array field: emit key changes against the previous state. */
static void hid_decode_array(usb_hid_field_t *field, const uint32_t *values, size_t value_count, usb_hid_event_t *events, size_t capacity, size_t *count)
{
    if (field->usage_page != HID_USAGE_PAGE_KEYBOARD) return;
    for (size_t i = 0; i < field->previous_count; i++) {
        uint32_t usage = field->previous[i];
        if (usage > 3 && !hid_array_contains(values, value_count, usage)) hid_emit(events, capacity, count, field->application, EV_KEY, usb_hid_keyboard_keycode((uint16_t)usage), 0);
    }
    for (size_t i = 0; i < value_count; i++) {
        uint32_t usage = values[i];
        if (usage > 3 && !hid_array_contains(field->previous, field->previous_count, usage))
            hid_emit(events, capacity, count, field->application, EV_KEY, usb_hid_keyboard_keycode((uint16_t)usage), 1);
    }
    field->previous_count = (uint8_t)value_count;
    memcpy(field->previous, values, value_count * sizeof(values[0]));
}

/* Decode one variable field element into an evdev event. */
static void hid_decode_variable(usb_hid_field_t *field, size_t index, int32_t value, usb_hid_event_t *events, size_t capacity, size_t *count)
{
    uint16_t usage      = hid_field_usage(field, index);
    uint16_t usage_page = hid_field_usage_page(field, index);
    bool     relative   = (field->flags & USB_HID_MAIN_RELATIVE) != 0;
    uint16_t type       = 0;
    uint16_t code       = 0;

    switch (usage_page) {
        case HID_USAGE_PAGE_KEYBOARD :
            type  = EV_KEY;
            code  = usb_hid_keyboard_keycode(usage);
            value = !!value;
            break;
        case HID_USAGE_PAGE_BUTTON :
            type  = EV_KEY;
            code  = usage && usage <= 8 ? BTN_LEFT + usage - 1 : 0;
            value = !!value;
            break;
        case HID_USAGE_PAGE_CONSUMER :
            type  = EV_KEY;
            code  = hid_consumer_keycode(usage);
            value = !!value;
            break;
        case HID_USAGE_PAGE_GENERIC_DESKTOP :
            type = relative ? EV_REL : EV_ABS;
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
                    break;
            }
            break;
        default :
            break;
    }

    if (type == EV_KEY) {
        if (field->previous[index] == (uint32_t)value) return;
        field->previous[index] = (uint32_t)value;
    } else if (relative && !value) {
        return;
    }
    hid_emit(events, capacity, count, field->application, type, code, value);
}

/* Decode one input report into a list of evdev events. */
int usb_hid_decode_report(usb_hid_report_t *report, const uint8_t *data, size_t length, usb_hid_event_t *events, size_t event_capacity)
{
    uint8_t report_id   = 0;
    size_t  event_count = 0;

    if (!report || !data || (!events && event_capacity)) {
        plogk("usb-hid: decode_report: invalid argument.\n");
        return -EINVAL;
    }
    if (report->numbered_reports) {
        if (!length) {
            plogk("usb-hid: decode_report: missing report id byte.\n");
            return -EINVAL;
        }
        report_id = *data++;
        length--;
        if (!report_id) {
            plogk("usb-hid: decode_report: report id zero is invalid.\n");
            return -EINVAL;
        }
    }
    if ((size_t)report->report_bits[report_id] > length * 8) {
        plogk("usb-hid: decode_report: report too short for fields (report_id=%u, bits=%u, data_bits=%lu)\n", report_id, report->report_bits[report_id], (unsigned long)length * 8);
        return -EMSGSIZE;
    }

    for (size_t field_index = 0; field_index < report->field_count; field_index++) {
        usb_hid_field_t *field = &report->fields[field_index];
        uint32_t         values[USB_HID_MAX_USAGES];

        if (field->report_id != report_id) continue;
        for (size_t i = 0; i < field->report_count; i++) {
            uint32_t raw = hid_extract_bits(data, length, field->bit_offset + i * field->report_size, field->report_size);
            values[i]    = raw;
            if (field->flags & USB_HID_MAIN_VARIABLE) hid_decode_variable(field, i, hid_field_value(field, raw), events, event_capacity, &event_count);
        }
        if (!(field->flags & USB_HID_MAIN_VARIABLE)) hid_decode_array(field, values, field->report_count, events, event_capacity, &event_count);
    }
    return (int)event_count;
}
