/*
 * Native USB protocol regression tests.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <drivers/usb/class/usb_hid.h>
#include <drivers/usb/class/usb_storage.h>
#include <drivers/usb/host/uhci.h>
#include <drivers/usb/host/ohci.h>
#include <drivers/usb/host/ehci.h>
#include <kernel/errno.h>

static void test_uhci_lengths(void)
{
    assert(uhci_td_encode_length(0) == 0x7ffU);
    assert(uhci_td_encode_length(1) == 0U);
    assert(uhci_td_encode_length(8) == 7U);
    assert(uhci_td_encode_length(64) == 63U);
    assert(uhci_td_decode_length(0x7ffU) == 0);
    assert(uhci_td_decode_length(0U) == 1);
    assert(uhci_td_decode_length(63U) == 64);
}

static void test_legacy_descriptor_layouts(void)
{
    _Static_assert(sizeof(ohci_ed_t) == 16, "OHCI ED must be 16 bytes");
    _Static_assert(sizeof(ohci_gtd_t) == 16, "OHCI general TD must be 16 bytes");
    _Static_assert(OHCI_TD_DP_OUT == (1U << 19), "OHCI OUT PID encoding");
    _Static_assert(OHCI_TD_DP_IN == (2U << 19), "OHCI IN PID encoding");
    _Static_assert(OHCI_TD_EC_SHIFT == 26, "OHCI ErrorCount field offset");

    _Static_assert(offsetof(ehci_qh_t, token) == 24, "EHCI QH overlay token offset");
    _Static_assert(offsetof(ehci_qh_t, buffer) == 28, "EHCI QH buffer offset");
    _Static_assert(offsetof(ehci_qh_t, extended) == 48, "EHCI QH high buffer offset");
    _Static_assert(offsetof(ehci_qtd_t, token) == 8, "EHCI qTD token offset");
    _Static_assert(EHCI_QTD_TOGGLE == (1U << 31), "EHCI qTD toggle field offset");
}

static void test_mass_storage_wrappers(void)
{
    _Static_assert(sizeof(usb_msc_cbw_t) == 31, "BOT CBW must be 31 bytes");
    _Static_assert(sizeof(usb_msc_csw_t) == 13, "BOT CSW must be 13 bytes");
    const uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    usb_msc_cbw_t cbw;
    assert(usb_msc_build_cbw(&cbw, 0x12345678U, 3, inquiry, sizeof(inquiry), 36, true) == EOK);
    assert(cbw.signature == USB_MSC_CBW_SIGNATURE);
    assert(cbw.tag == 0x12345678U && cbw.lun == 3);
    assert(cbw.flags == USB_MSC_CBW_FLAG_IN && cbw.command_length == sizeof(inquiry));
    assert(memcmp(cbw.command, inquiry, sizeof(inquiry)) == 0);

    uint8_t capacity[8] = {0, 0, 0, 15, 0, 0, 2, 0};
    uint64_t sectors = 0;
    uint32_t sector_size = 0;
    assert(usb_scsi_parse_capacity10(capacity, &sectors, &sector_size) == EOK);
    assert(sectors == 16 && sector_size == 512);
}

static void test_hid_mouse_report(void)
{
    static const uint8_t descriptor[] = {
        0x05, 0x01, 0x09, 0x02, 0xa1, 0x01, 0x09, 0x01, 0xa1, 0x00,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00, 0x25, 0x01,
        0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05,
        0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81,
        0x25, 0x7f, 0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xc0, 0xc0,
    };
    usb_hid_report_t report;
    assert(usb_hid_parse_report_descriptor(descriptor, sizeof(descriptor), &report) == EOK);
    assert(report.field_count == 2 && report.report_bits[0] == 24);

    const uint8_t packet[] = {1, 5, 0xfb};
    usb_hid_event_t events[8];
    int count = usb_hid_decode_report(&report, packet, sizeof(packet), events, 8);
    assert(count == 3);
    assert(events[0].type == EV_KEY && events[0].code == BTN_LEFT && events[0].value == 1);
    assert(events[1].type == EV_REL && events[1].code == REL_X && events[1].value == 5);
    assert(events[2].type == EV_REL && events[2].code == REL_Y && events[2].value == -5);
}

static void test_hid_extended_usage_page(void)
{
    /* A 32-bit Local Usage carries its own Usage Page in the high word. */
    static const uint8_t descriptor[] = {
        0x05, 0x01,             /* Usage Page (Generic Desktop) */
        0x09, 0x02,             /* Usage (Mouse) */
        0xa1, 0x01,             /* Collection (Application) */
        0x15, 0x00, 0x25, 0x01, /* Logical Min/Max */
        0x75, 0x01, 0x95, 0x01, /* Report Size/Count */
        0x0b, 0xe9, 0x00, 0x0c, 0x00, /* Usage (Consumer: Volume Increment) */
        0x81, 0x02, 0xc0,
    };
    usb_hid_report_t report;
    assert(usb_hid_parse_report_descriptor(descriptor, sizeof(descriptor), &report) == EOK);
    assert(report.fields[0].usage_pages[0] == 0x0c);
    assert(report.fields[0].usages[0] == 0x00e9);

    const uint8_t packet[] = {1};
    usb_hid_event_t event;
    assert(usb_hid_decode_report(&report, packet, sizeof(packet), &event, 1) == 1);
    assert(event.type == EV_KEY && event.code == KEY_VOLUMEUP && event.value == 1);
}

int main(void)
{
    test_uhci_lengths();
    test_legacy_descriptor_layouts();
    test_mass_storage_wrappers();
    test_hid_mouse_report();
    test_hid_extended_usage_page();
    puts("PASS USB UHCI, BOT/SCSI and HID protocol regressions");
    return 0;
}
