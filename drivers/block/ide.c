/*
 *
 *      ide.c
 *      Standard ATA/ATAPI device drivers
 *
 *      2024/7/11 By MicroFish
 *      Based on Apache 2.0 open source license.
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <apic.h>
#include <common.h>
#include <ide.h>
#include <interrupt.h>
#include <pci.h>
#include <printk.h>
#include <stddef.h>
#include <stdint.h>
#include <timer.h>

/* Request for operation IDE Controller */
pci_finding_request_t ide_pci_request = {
    .type = PCI_FOUND_CLASS,
    .req  = {
        .class_req = {
            .class_code = 0x010100,
        },
    },
};

/* Structure */
ide_channel_registers_t channels[2];
ide_device_t            ide_devices[4];

/* Data Array */
uint8_t        ide_buf[2048]    = {0};
static uint8_t atapi_packet[12] = {
    0xa8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};
static int package[2];

/* Interrupt status bit */
static volatile uint8_t ide_irq_invoked = 0;

/* IDE interrupt handling function */
INTERRUPT_BEGIN static void ide_irq(interrupt_frame_t *frame)
{
    (void)frame;
    disable_intr();
    ide_irq_invoked = 1;
    send_eoi();
    enable_intr();
}
INTERRUPT_END

/* Waiting for IDE interrupt to be triggered */
static void ide_wait_irq(void)
{
    while (!ide_irq_invoked);
    ide_irq_invoked = 0;
}

/* Setting up the IDE */
static void ide_initialize(uint32_t BAR0, uint32_t BAR1, uint32_t BAR2, uint32_t BAR3, uint32_t BAR4)
{
    int j, k, count = 0;
    for (int i = 0; i < 4; i++) ide_devices[i].reserved = 0;
    plogk("ide: BAR0 = 0x%03x, BAR1 = 0x%03x, BAR2 = 0x%03x, BAR3 = 0x%03x, BAR4 = 0x%03x\n", BAR0, BAR1, BAR2, BAR3, BAR4);

    /* Detect the I/O ports of the IDE controller */
    channels[ATA_PRIMARY].base    = (BAR0 & 0xfffffffc) + 0x1f0 * (!BAR0);
    channels[ATA_PRIMARY].ctrl    = (BAR1 & 0xfffffffc) + 0x3f6 * (!BAR1);
    channels[ATA_SECONDARY].base  = (BAR2 & 0xfffffffc) + 0x170 * (!BAR2);
    channels[ATA_SECONDARY].ctrl  = (BAR3 & 0xfffffffc) + 0x376 * (!BAR3);
    channels[ATA_PRIMARY].bmide   = (BAR4 & 0xfffffffc) + 0;
    channels[ATA_SECONDARY].bmide = (BAR4 & 0xfffffffc) + 8;

    /* Disable IRQ */
    ide_write(ATA_PRIMARY, ATA_REG_CONTROL, 2);
    ide_write(ATA_SECONDARY, ATA_REG_CONTROL, 2);
    for (int i = 0; i < 2; i++) {
        for (j = 0; j < 2; j++) {
            uint8_t err = 0, type = IDE_ATA, status;
            ide_devices[count].reserved = 0;

            /* Select Drive */
            ide_write(i, ATA_REG_HDDEVSEL, 0xa0 | (j << 4));
            nsleep(10);

            /* Send ATA Identify command */
            ide_write(i, ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
            nsleep(10);

            if (!ide_read(i, ATA_REG_STATUS)) continue;
            while (1) {
                status = ide_read(i, ATA_REG_STATUS);
                if ((status & ATA_SR_ERR)) {
                    err = 1;
                    break;
                }
                if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) break;
            }

            /* ATAPI device detection */
            if (err != 0) {
                uint8_t cl = ide_read(i, ATA_REG_LBA1);
                uint8_t ch = ide_read(i, ATA_REG_LBA2);

                if ((cl == 0x14 && ch == 0xeb) || (cl == 0x69 && ch == 0x96))
                    type = IDE_ATAPI;
                else
                    continue;

                ide_write(i, ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);
                nsleep(10);
            }

            /* Read the device's identification space */
            ide_read_buffer(i, ATA_REG_DATA, ide_buf, 128);

            /* Read device parameters */
            ide_devices[count].reserved     = 1;
            ide_devices[count].type         = type;
            ide_devices[count].channel      = i;
            ide_devices[count].drive        = j;
            ide_devices[count].signature    = *((uint16_t *)(ide_buf + ATA_IDENT_DEVICETYPE));
            ide_devices[count].capabilities = *((uint16_t *)(ide_buf + ATA_IDENT_CAPABILITIES));
            ide_devices[count].command_sets = *((uint32_t *)(ide_buf + ATA_IDENT_COMMANDSETS));

            /* Get Size */
            if (ide_devices[count].command_sets & (1 << 26)) // Devices use 48-bit addressing
                ide_devices[count].size = *((uint32_t *)(ide_buf + ATA_IDENT_MAX_LBA_EXT));
            else
                /* Devices use 28-bit addressing */
                ide_devices[count].size = *((uint32_t *)(ide_buf + ATA_IDENT_MAX_LBA));

            /* Get device model */
            for (k = 0; k < 40; k += 2) {
                ide_devices[count].model[k]     = ide_buf[ATA_IDENT_MODEL + k + 1];
                ide_devices[count].model[k + 1] = ide_buf[ATA_IDENT_MODEL + k];
            }
            ide_devices[count].model[40] = 0;
            count++;
        }
    }

    /* Print device information */
    for (int i = 0; i < 4; i++)
        if (ide_devices[i].reserved == 1)
            plogk("ide: Found %s Drive %u(KiB) - %s\n", ide_devices[i].type ? "ATAPI" : "ATA", (ide_devices[i].size * 512) / 1024,
                  ide_devices[i].model);
}

/* Error handling */
static uint8_t ide_print_error(uint32_t drive, uint8_t err)
{
    if (err == 0) return err;
    if (err == 1) {
        plogk("ide: Device fault.\n");
        err = 19;
    } else if (err == 2) {
        uint8_t st = ide_read(ide_devices[drive].channel, ATA_REG_ERROR);
        if (st & ATA_ER_AMNF) {
            plogk("ide: No address mark found.\n");
            err = 7;
        }
        if (st & ATA_ER_TK0NF) {
            plogk("ide: No media or media error.\n");
            err = 3;
        }
        if (st & ATA_ER_ABRT) {
            plogk("ide: Command aborted.\n");
            err = 20;
        }
        if (st & ATA_ER_MCR) {
            plogk("ide: No media or media error.\n");
            err = 3;
        }
        if (st & ATA_ER_IDNF) {
            plogk("ide: ID mark not found.\n");
            err = 21;
        }
        if (st & ATA_ER_MC) {
            plogk("ide: No media or media error.\n");
            err = 3;
        }
        if (st & ATA_ER_UNC) {
            plogk("ide: Uncorrectable data error.\n");
            err = 22;
        }
        if (st & ATA_ER_BBK) {
            plogk("ide: Bad sectors.\n");
            err = 13;
        }
    } else if (err == 3) {
        plogk("ide: Reads nothing.\n");
        err = 23;
    } else if (err == 4) {
        plogk("ide: Write protected.\n");
        err = 8;
    }
    return err;
}

/* Initialize IDE */
void init_ide(void)
{
    base_address_register_t bars[6];
    uint32_t                bar_addrs[6];

    pci_device_reg_t bar_reg = {
        .parent = 0,
        .offset = 0,
    };
    pci_device_find(&ide_pci_request);

    /* Detect if the computer has an IDE controller */
    if (ide_pci_request.response->error != PCI_FINDING_SUCCESS) {
        plogk("ide: No IDE controller found.\n");
        return;

        /* Re-try (but I think it is not necessary)
         * pci_flush_devices_cache();
         * pci_device_find(&ide_pci_request);
         * if (ide_pci_request.response->error != PCI_FINDING_SUCCESS) {
         *     plogk("ide: No IDE controller found.\n");
         *     return;
         * }
         */
    }
    bar_reg.parent = ide_pci_request.response->device;
    register_interrupt_handler(IRQ_14, (void *)ide_irq, 0, 0x8e);
    register_interrupt_handler(IRQ_15, (void *)ide_irq, 0, 0x8e);

    for (uint32_t idx = 0; idx < 6; idx++) {
        bars[idx] = get_base_address_register(bar_reg.parent, idx);
        pointer_cast_t cast;
        cast.ptr       = bars[idx].address;
        bar_addrs[idx] = cast.val;
    }
    ide_initialize(bar_addrs[0], bar_addrs[1], bar_addrs[2], bar_addrs[3], bar_addrs[4]);
}

/* Read a byte of data from the specified register of the IDE device */
uint8_t ide_read(uint8_t channel, uint8_t reg)
{
    uint8_t result = 0;
    if (reg > 0x07 && reg < 0x0c) ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN);
    if (reg < 0x08)
        result = inb(channels[channel].base + reg - 0x00);
    else if (reg < 0x0c)
        result = inb(channels[channel].base + reg - 0x06);
    else if (reg < 0x0e)
        result = inb(channels[channel].ctrl + reg - 0x0a);
    else if (reg < 0x16)
        result = inb(channels[channel].bmide + reg - 0x0e);
    if (reg > 0x07 && reg < 0x0c) ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN);
    return result;
}

/* Write a byte of data to the specified register of the IDE device */
void ide_write(uint8_t channel, uint8_t reg, uint8_t data)
{
    if (reg > 0x07 && reg < 0x0c) {
        /* Expanded by ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN); */
        outb(channels[channel].ctrl + ATA_REG_CONTROL - 0x0a, 0x80 | channels[channel].nIEN);
    }
    if (reg < 0x08)
        outb(channels[channel].base + reg - 0x00, data);
    else if (reg < 0x0c)
        outb(channels[channel].base + reg - 0x06, data);
    else if (reg < 0x0e)
        outb(channels[channel].ctrl + reg - 0x0a, data);
    else if (reg < 0x16)
        outb(channels[channel].bmide + reg - 0x0e, data);
    if (reg > 0x07 && reg < 0x0c) {
        /* Expanded by ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN); */
        outb(channels[channel].ctrl + ATA_REG_CONTROL - 0x0a, 0x80 | channels[channel].nIEN);
    }
}

/* Read multiple words of data from the specified register of the IDE device into the buffer */
void ide_read_buffer(uint8_t channel, uint8_t reg, uint8_t *buffer, uint32_t quads)
{
    if (reg > 0x07 && reg < 0x0c) ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN);
    if (reg < 0x08)
        insl(channels[channel].base + reg - 0x00, (uint32_t *)buffer, quads);
    else if (reg < 0x0c)
        insl(channels[channel].base + reg - 0x06, (uint32_t *)buffer, quads);
    else if (reg < 0x0e)
        insl(channels[channel].ctrl + reg - 0x0a, (uint32_t *)buffer, quads);
    else if (reg < 0x16)
        insl(channels[channel].bmide + reg - 0x0e, (uint32_t *)buffer, quads);
    if (reg > 0x07 && reg < 0x0c) ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN);
}

/* Polling the status of IDE devices */
uint8_t ide_polling(uint8_t channel, uint32_t advanced_check)
{
    for (int i = 0; i < 4; i++) ide_read(channel, ATA_REG_ALTSTATUS);

    int a = ide_read(channel, ATA_REG_STATUS);
    while (a & ATA_SR_BSY) {
        a = ide_read(channel, ATA_REG_STATUS);
        nsleep(10);
    }
    if (advanced_check) {
        uint8_t state = ide_read(channel, ATA_REG_STATUS);
        if (state & ATA_SR_ERR) return 2;
        if (state & ATA_SR_DF) return 1;
        if ((state & ATA_SR_DRQ) == 0) return 3;
    }
    return 0;
}

/* Read and write ATA devices */
uint8_t ide_ata_access(uint8_t direction, uint8_t drive, uint32_t lba, uint8_t numsects, uint16_t *edi)
{
    uint8_t  lba_mode, dma, cmd;
    uint8_t  lba_io[6];
    uint32_t channel  = ide_devices[drive].channel;
    uint32_t slavebit = ide_devices[drive].drive;
    uint32_t bus      = channels[channel].base;
    uint32_t words    = 256;
    uint16_t cyl, i;
    uint8_t  head, sect, err;

    ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN = (ide_irq_invoked = 0x0) + 0x02);
    if (lba >= 0x10000000) {
        lba_mode  = 2;
        lba_io[0] = (lba & 0x000000ff) >> 0;
        lba_io[1] = (lba & 0x0000ff00) >> 8;
        lba_io[2] = (lba & 0x00ff0000) >> 16;
        lba_io[3] = (lba & 0xff000000) >> 24;
        lba_io[4] = 0;
        lba_io[5] = 0;
        head      = 0;
    } else if (ide_devices[drive].capabilities & 0x200) {
        lba_mode  = 1;
        lba_io[0] = (lba & 0x00000ff) >> 0;
        lba_io[1] = (lba & 0x000ff00) >> 8;
        lba_io[2] = (lba & 0x0ff0000) >> 16;
        lba_io[3] = 0;
        lba_io[4] = 0;
        lba_io[5] = 0;
        head      = (lba & 0xf000000) >> 24;
    } else {
        lba_mode  = 0;
        sect      = (lba % 63) + 1;
        cyl       = (lba + 1 - sect) / (16 * 63);
        lba_io[0] = sect;
        lba_io[1] = (cyl >> 0) & 0xff;
        lba_io[2] = (cyl >> 8) & 0xff;
        lba_io[3] = 0;
        lba_io[4] = 0;
        lba_io[5] = 0;
        head      = (lba + 1 - sect) % (16 * 63) / (63);
    }
    dma = 0;

    while (ide_read(channel, ATA_REG_STATUS) & ATA_SR_BSY);
    if (lba_mode == 0)
        ide_write(channel, ATA_REG_HDDEVSEL, 0xa0 | (slavebit << 4) | head); // Drive & CHS.
    else
        ide_write(channel, ATA_REG_HDDEVSEL, 0xe0 | (slavebit << 4) | head); // Drive & LBA
    if (lba_mode == 2) {
        ide_write(channel, ATA_REG_SECCOUNT1, 0);
        ide_write(channel, ATA_REG_LBA3, lba_io[3]);
        ide_write(channel, ATA_REG_LBA4, lba_io[4]);
        ide_write(channel, ATA_REG_LBA5, lba_io[5]);
    }
    ide_write(channel, ATA_REG_SECCOUNT0, numsects);
    ide_write(channel, ATA_REG_LBA0, lba_io[0]);
    ide_write(channel, ATA_REG_LBA1, lba_io[1]);
    ide_write(channel, ATA_REG_LBA2, lba_io[2]);

    if (lba_mode == 0 && dma == 0 && direction == 0) cmd = ATA_CMD_READ_PIO;
    if (lba_mode == 1 && dma == 0 && direction == 0) cmd = ATA_CMD_READ_PIO;
    if (lba_mode == 2 && dma == 0 && direction == 0) cmd = ATA_CMD_READ_PIO_EXT;
    if (lba_mode == 0 && dma == 1 && direction == 0) cmd = ATA_CMD_READ_DMA;
    if (lba_mode == 1 && dma == 1 && direction == 0) cmd = ATA_CMD_READ_DMA;
    if (lba_mode == 2 && dma == 1 && direction == 0) cmd = ATA_CMD_READ_DMA_EXT;
    if (lba_mode == 0 && dma == 0 && direction == 1) cmd = ATA_CMD_WRITE_PIO;
    if (lba_mode == 1 && dma == 0 && direction == 1) cmd = ATA_CMD_WRITE_PIO;
    if (lba_mode == 2 && dma == 0 && direction == 1) cmd = ATA_CMD_WRITE_PIO_EXT;
    if (lba_mode == 0 && dma == 1 && direction == 1) cmd = ATA_CMD_WRITE_DMA;
    if (lba_mode == 1 && dma == 1 && direction == 1) cmd = ATA_CMD_WRITE_DMA;
    if (lba_mode == 2 && dma == 1 && direction == 1) cmd = ATA_CMD_WRITE_DMA_EXT;
    ide_write(channel, ATA_REG_COMMAND, cmd);

    if (direction == 0) {
        /* PIO Read */
        uint16_t *word_ = edi;
        for (i = 0; i < numsects; i++) {
            err = ide_polling(channel, 1);
            if (err != 0) return err;
            insl(bus, (uint32_t *)(word_ + (size_t)i * words), words / 2);
        }
    } else {
        /* PIO Write */
        uint16_t *word_ = edi;
        for (i = 0; i < numsects; i++) {
            ide_polling(channel, 0);
            for (uint32_t h = 0; h < words; h++) outw(bus, word_[i * words + h]);
        }
        ide_write(channel, ATA_REG_COMMAND, (char[]) {ATA_CMD_CACHE_FLUSH, ATA_CMD_CACHE_FLUSH, ATA_CMD_CACHE_FLUSH_EXT}[lba_mode]);
        ide_polling(channel, 0);
    }
    return 0;
}

/* Reading data from ATAPI devices */
uint8_t ide_atapi_read(uint8_t drive, uint32_t lba, uint8_t numsects, uint16_t *edi)
{
    uint32_t channel  = ide_devices[drive].channel;
    uint32_t slavebit = ide_devices[drive].drive;
    uint32_t bus      = channels[channel].base;
    uint32_t words    = 1024;
    uint8_t  err;
    int      i;

    ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN = ide_irq_invoked = 0x0);

    /* Setting up SCSI packets */
    atapi_packet[0]  = ATAPI_CMD_READ;
    atapi_packet[1]  = 0x0;
    atapi_packet[2]  = (lba >> 24) & 0xff;
    atapi_packet[3]  = (lba >> 16) & 0xff;
    atapi_packet[4]  = (lba >> 8) & 0xff;
    atapi_packet[5]  = (lba >> 0) & 0xff;
    atapi_packet[6]  = 0x0;
    atapi_packet[7]  = 0x0;
    atapi_packet[8]  = 0x0;
    atapi_packet[9]  = numsects;
    atapi_packet[10] = 0x0;
    atapi_packet[11] = 0x0;

    ide_write(channel, ATA_REG_HDDEVSEL, slavebit << 4);
    for (int i = 0; i < 4000; i++);
    for (int i = 0; i < 4; i++) ide_read(channel, ATA_REG_ALTSTATUS);

    ide_write(channel, ATA_REG_FEATURES, 0);              // PIO Mode
    ide_write(channel, ATA_REG_LBA1, (words * 2) & 0xff); // Sector size is smaller in bytes
    ide_write(channel, ATA_REG_LBA2, (words * 2) >> 8);   // The upper limit of the sector size in bytes

    /* Send packet command */
    ide_write(channel, ATA_REG_COMMAND, ATA_CMD_PACKET);
    err = ide_polling(channel, 1);
    if (err != 0) return err;

    uint16_t *_atapi_packet = (uint16_t *)atapi_packet;
    for (int i = 0; i < 6; i++) outw(bus, _atapi_packet[i]);

    uint16_t *_word = edi;
    for (i = 0; i < numsects; i++) {
        ide_wait_irq();
        err = ide_polling(channel, 1);
        if (err != 0) return err;
        for (uint32_t h = 0; h < words; h++) _word[i * words + h] = inw(bus);
    }
    while (ide_read(channel, ATA_REG_STATUS) & (ATA_SR_BSY | ATA_SR_DRQ));
    return 0;
}

/* Read multiple sectors from an IDE device */
void ide_read_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, uint16_t *edi)
{
    /* read multiple sector sf Roman IDE device */
    if (drive > 3 || !ide_devices[drive].reserved) package[0] = 0x1;

    /* Check if the input is valid */
    else if (((lba + numsects) > ide_devices[drive].size) && (ide_devices[drive].type == IDE_ATA))
        package[0] = 0x2;

    /* Reading in PIO mode via polling and IRQ */
    else {
        uint8_t err = 0;
        if (ide_devices[drive].type == IDE_ATA)
            err = ide_ata_access(ATA_READ, drive, lba, numsects, edi);
        else if (ide_devices[drive].type == IDE_ATAPI)
            for (int i = 0; i < numsects; i++) err = ide_atapi_read(drive, lba + i, 1, edi + (size_t)(i * 2048));
        package[0] = ide_print_error(drive, err);
    }
}

/* Write multiple sectors to an IDE device */
void ide_write_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, uint16_t *edi)
{
    /* Check if the drive exists */
    if (drive > 3 || !ide_devices[drive].reserved) package[0] = 0x1;

    /* Check if the input is valid */
    else if (((lba + numsects) > ide_devices[drive].size) && (ide_devices[drive].type == IDE_ATA))
        package[0] = 0x2;

    /* Writing in PIO mode via polling and IRQ */
    else {
        uint8_t err = 0;
        if (ide_devices[drive].type == IDE_ATA)
            err = ide_ata_access(ATA_WRITE, drive, lba, numsects, edi);
        else if (ide_devices[drive].type == IDE_ATAPI)
            err = 4;
        package[0] = ide_print_error(drive, err);
    }
}
