/*
 *
 *      usb_storage.h
 *      USB Mass Storage Bulk-Only Transport helpers
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_USB_STORAGE_H_
#define INCLUDE_USB_STORAGE_H_

#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define USB_MSC_CBW_SIGNATURE 0x43425355U
#define USB_MSC_CSW_SIGNATURE 0x53425355U
#define USB_MSC_CBW_FLAG_IN   0x80

typedef struct __attribute__((packed)) {
        uint32_t signature;
        uint32_t tag;
        uint32_t transfer_length;
        uint8_t  flags;
        uint8_t  lun;
        uint8_t  command_length;
        uint8_t  command[16];
} usb_msc_cbw_t;

typedef struct __attribute__((packed)) {
        uint32_t signature;
        uint32_t tag;
        uint32_t residue;
        uint8_t  status;
} usb_msc_csw_t;

int  usb_msc_build_cbw(usb_msc_cbw_t *cbw, uint32_t tag, uint8_t lun, const void *command, uint8_t command_length,
                       uint32_t transfer_length, bool input);
void usb_scsi_build_rw10(uint8_t command[10], bool write, uint32_t lba, uint16_t blocks, bool fua);
int  usb_scsi_parse_capacity10(const uint8_t response[8], uint64_t *sector_count, uint32_t *sector_size);

#endif /* INCLUDE_USB_STORAGE_H_ */
