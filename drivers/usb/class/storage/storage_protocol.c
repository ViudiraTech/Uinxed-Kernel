/*
 *
 *      storage_protocol.c
 *      USB BOT wrappers and SCSI command encoding
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/usb/class/storage/usb_storage.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>

uint32_t usb_scsi_be32(const uint8_t *buffer)
{
    return (uint32_t)buffer[0] << 24 | (uint32_t)buffer[1] << 16 | (uint32_t)buffer[2] << 8 | buffer[3];
}

int usb_msc_build_cbw(usb_msc_cbw_t *cbw, uint32_t tag, uint8_t lun, const void *command, uint8_t command_length, uint32_t transfer_length,
                      bool input)
{
    if (!cbw || !command || !command_length || command_length > sizeof(cbw->command) || lun > 15) {
        plogk("usb_storage: build_cbw: invalid argument (command_length=%u, lun=%u)\n", (unsigned)command_length, (unsigned)lun);
        return -EINVAL;
    }
    memset(cbw, 0, sizeof(*cbw));
    cbw->signature       = USB_MSC_CBW_SIGNATURE;
    cbw->tag             = tag;
    cbw->transfer_length = transfer_length;
    cbw->flags           = input ? USB_MSC_CBW_FLAG_IN : 0;
    cbw->lun             = lun;
    cbw->command_length  = command_length;
    memcpy(cbw->command, command, command_length);
    return EOK;
}

void usb_scsi_build_rw10(uint8_t command[10], bool write, uint32_t lba, uint16_t blocks, bool fua)
{
    memset(command, 0, 10);
    command[0] = write ? 0x2a : 0x28;
    if (fua) command[1] |= 0x08;
    command[2] = (uint8_t)(lba >> 24);
    command[3] = (uint8_t)(lba >> 16);
    command[4] = (uint8_t)(lba >> 8);
    command[5] = (uint8_t)lba;
    command[7] = (uint8_t)(blocks >> 8);
    command[8] = (uint8_t)blocks;
}

int usb_scsi_parse_capacity10(const uint8_t response[8], uint64_t *sector_count, uint32_t *sector_size)
{
    if (!response || !sector_count || !sector_size) {
        plogk("usb_storage: parse_capacity10: invalid argument.\n");
        return -EINVAL;
    }
    uint32_t last_lba   = usb_scsi_be32(response);
    uint32_t block_size = usb_scsi_be32(response + 4);
    if (!block_size || (block_size & (block_size - 1)) || block_size < 512 || block_size > 65536) {
        plogk("usb_storage: parse_capacity10: invalid block size (block_size=%u, last_lba=%u)\n", (unsigned)block_size, (unsigned)last_lba);
        return -EINVAL;
    }
    *sector_count = (uint64_t)last_lba + 1;
    *sector_size  = block_size;
    return EOK;
}
