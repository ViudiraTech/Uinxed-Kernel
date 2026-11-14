/*
 *
 *      ide.c
 *      Standard ATA/ATAPI device drivers
 *
 *      2024/7/11 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/misc/common.h>
#include <drivers/block/ata/pata/atapi.h>
#include <drivers/block/ata/pata/ide.h>
#include <drivers/bus/pci.h>
#include <drivers/firmware/apic.h>
#include <kernel/debug/printk.h>
#include <kernel/interrupt/interrupt.h>
#include <kernel/timer/timer.h>
#include <libs/std/errno.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/hhdm.h>

#if CONFIG_ATA

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
uint8_t ide_buf[2048] = {0};

/* Boot-time DMA/PIO comparison buffers, 8 sectors each */
static uint16_t ide_validate_pio[2048];
static uint16_t ide_validate_dma[2048];

static uint8_t ide_validate_ata(uint8_t drive);

/* IDE interrupt handling for the primary channel (IRQ14). */
INTERRUPT_BEGIN static void ide_irq_primary(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    (void)frame;
    disable_intr();
    channels[ATA_PRIMARY].irq_pending = 1;
    send_eoi();
    irq_leave_gs(frame);
}
INTERRUPT_END

/* IDE interrupt handling for the secondary channel (IRQ15). */
INTERRUPT_BEGIN static void ide_irq_secondary(interrupt_frame_t *frame)
{
    irq_enter_gs(frame);
    (void)frame;
    disable_intr();
    channels[ATA_SECONDARY].irq_pending = 1;
    send_eoi();
    irq_leave_gs(frame);
}
INTERRUPT_END

/* Wait for an IDE interrupt on a channel. No callers: ATA uses polling and ATAPI sets nIEN. */
int ide_wait_irq(uint8_t channel)
{
    int tout = IDE_IRQ_TIMEOUT;
    while (!channels[channel].irq_pending) {
        if (--tout <= 0) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ide: IRQ timeout on channel %u\n", channel);
            return -ETIMEDOUT;
        }
    }
    channels[channel].irq_pending = 0;
    return 0;
}

/* Clear the bus-master status bits */
void ide_bm_clear_status(uint8_t channel)
{
    outb(channels[channel].bmide + ATA_BM_STATUS, ATA_BM_STATUS_CLEAR);
}

/* One page per PRD keeps each region inside the bus-master 64 KiB boundary */
void ide_bm_set_prd(uint8_t channel, uint16_t bytes)
{
    uint64_t   phys = channels[channel].dma_buf_phys;
    uint32_t   left = bytes;
    ata_prd_t *prd  = channels[channel].prd;
    ata_prd_t *last = prd;

    /* The table is only safe to rewrite with the engine stopped */
    ide_bm_stop(channel);

    while (left != 0) {
        uint32_t chunk = left > 4096 ? 4096 : left;
        prd->base      = (uint32_t)phys;
        prd->count     = (uint16_t)chunk;
        prd->flags     = 0;
        last           = prd;
        phys += chunk;
        left -= chunk;
        prd++;
    }

    last->flags = ATA_PRD_EOT;
    dma_write_barrier();
    outl(channels[channel].bmide + ATA_BM_PRDT, (uint32_t)channels[channel].prd_phys);
}

/* Start the bus-master engine; write = host to device */
void ide_bm_start(uint8_t channel, uint8_t write)
{
    /* Deassert START first so every transfer sees a 0 -> 1 edge */
    outb(channels[channel].bmide + ATA_BM_CMD, 0);
    outb(channels[channel].bmide + ATA_BM_CMD, (uint8_t)((write ? 0 : ATA_BM_CMD_DIR) | ATA_BM_CMD_START));
}

/* Stop the bus-master engine */
void ide_bm_stop(uint8_t channel)
{
    outb(channels[channel].bmide + ATA_BM_CMD, 0);
}

/* Poll a bus-master transfer to completion */
uint8_t ide_bm_wait(uint8_t channel)
{
    int     timeout = IDE_POLL_RETRY;
    uint8_t status  = 0;

    for (;;) {
        status = inb(channels[channel].bmide + ATA_BM_STATUS);
        if (!(status & ATA_BM_STATUS_ACTIVE)) break;
        if ((status & ATA_BM_STATUS_ERR) || (ide_read(channel, ATA_REG_STATUS) & ATA_SR_ERR)) {
            ide_bm_stop(channel);
            ide_bm_clear_status(channel);
            return 2;
        }
        if (--timeout <= 0) {
            ide_bm_stop(channel);
            ide_bm_clear_status(channel);
            return 3;
        }
        nsleep(10);
    }
    if (status & ATA_BM_STATUS_ERR) {
        ide_bm_stop(channel);
        ide_bm_clear_status(channel);
        return 2;
    }

    ide_bm_stop(channel);
    ide_bm_clear_status(channel);
    if (ide_polling(channel, 0) != 0) return 3;

    status = ide_read(channel, ATA_REG_STATUS);
    if (status & ATA_SR_ERR) return 2;
    if (status & ATA_SR_DF) return 1;

    return 0;
}

/* Setting up the IDE */
static void ide_initialize(uint32_t BAR0, uint32_t BAR1, uint32_t BAR2, uint32_t BAR3, uint32_t BAR4, uint8_t bmide_valid)
{
    int j, k, count = 0;

    for (int i = 0; i < 4; i++) ide_devices[i].reserved = 0;
    for (int i = 0; i < 2; i++) {
        channels[i].lock.lock    = 0;
        channels[i].lock.rflags  = 0;
        channels[i].irq_pending  = 0;
        channels[i].dma_ok       = 0;
        channels[i].dma_buf      = NULL;
        channels[i].dma_buf_phys = 0;
        channels[i].prd_phys     = 0;
        channels[i].prd          = NULL;
    }
    plogk("ide: BAR0 = 0x%03x, BAR1 = 0x%03x, BAR2 = 0x%03x, BAR3 = 0x%03x, BAR4 = 0x%03x\n", BAR0, BAR1, BAR2, BAR3, BAR4);

    /* Detect the I/O ports of the IDE controller */
    channels[ATA_PRIMARY].base    = (BAR0 & 0xfffffffc) + (0x1f0 * (!BAR0));
    channels[ATA_PRIMARY].ctrl    = (BAR1 & 0xfffffffc) + (0x3f4 * (!BAR1));
    channels[ATA_SECONDARY].base  = (BAR2 & 0xfffffffc) + (0x170 * (!BAR2));
    channels[ATA_SECONDARY].ctrl  = (BAR3 & 0xfffffffc) + (0x374 * (!BAR3));
    channels[ATA_PRIMARY].bmide   = bmide_valid ? (BAR4 & 0xfffffffc) + 0 : 0;
    channels[ATA_SECONDARY].bmide = bmide_valid ? (BAR4 & 0xfffffffc) + 8 : 0;

    /* Allocate a bounce buffer and PRD table per channel below 4 GiB */
    if (bmide_valid) {
        for (int i = 0; i < 2; i++) {
            uint64_t buf_phys = alloc_frames_below(ATA_DMA_BUF_PAGES, 0x100000000ULL);
            uint64_t prd_phys = alloc_frames_below(1, 0x100000000ULL);

            if (!buf_phys || !prd_phys) {
                if (buf_phys) free_frames(buf_phys, ATA_DMA_BUF_PAGES);
                if (prd_phys) free_frames(prd_phys, 1);
                plogk("ide: Channel %u DMA buffer allocation failed, using PIO.\n", i);
                continue;
            }
            channels[i].dma_buf      = phys_to_virt(buf_phys);
            channels[i].dma_buf_phys = buf_phys;
            channels[i].prd_phys     = prd_phys;
            channels[i].prd          = phys_to_virt(prd_phys);
            channels[i].dma_ok       = 1;
        }
    }

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

            /* Poll for command completion with timeout */
            if (!ide_read(i, ATA_REG_STATUS)) continue;
            {
                int tout = IDE_POLL_RETRY;
                while (tout--) {
                    status = ide_read(i, ATA_REG_STATUS);
                    if ((status & ATA_SR_ERR)) {
                        err = 1;
                        break;
                    }
                    if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRQ)) break;
                    nsleep(100);
                }
                if (tout <= 0) continue;
            }

            /* ATAPI device detection */
            if (err != 0) {
                uint8_t cl = ide_read(i, ATA_REG_LBA1);
                uint8_t ch = ide_read(i, ATA_REG_LBA2);

                if ((cl == 0x14 && ch == 0xeb) || (cl == 0x69 && ch == 0x96)) {
                    /* Delegate to ATAPI module for identification */
                    if (atapi_identify(i, j, count) != 0) continue;

                    /* Copy ATAPI device info to IDE device table */
                    ide_devices[count].reserved     = 1;
                    ide_devices[count].type         = IDE_ATAPI;
                    ide_devices[count].channel      = i;
                    ide_devices[count].drive        = j;
                    ide_devices[count].signature    = 0;
                    ide_devices[count].capabilities = 0;
                    ide_devices[count].command_sets = 0;
                    ide_devices[count].size         = atapi_devices[count].lba_size;
                    ide_devices[count].dma_enabled  = atapi_devices[count].dma_enabled;
                    for (k = 0; k < 40; k++) ide_devices[count].model[k] = atapi_devices[count].model[k];
                    ide_devices[count].model[40] = 0;
                    count++;
                    continue;
                }
                continue;
            }

            /* ATA device: read identification data */
            ide_read_buffer(i, ATA_REG_DATA, ide_buf, 128);

            /*
             * Validate: word 0 of identify data must be non-zero and non-0xFFFF.
             * 0x0000 or 0xFFFF indicates floating bus (no device).
             */
            {
                uint16_t sig = *((uint16_t *)ide_buf);
                if (sig == 0x0000 || sig == 0xffff) {
                    plogk("ide: Phantom device on ch%d drv%d (sig=0x%04x), skipping.\n", i, j, sig);
                    continue;
                }
            }

            /* Read device parameters (use memcpy to avoid strict-aliasing/alignment issues) */
            ide_devices[count].reserved = 1;
            ide_devices[count].type     = type;
            ide_devices[count].channel  = i;
            ide_devices[count].drive    = j;
            memcpy(&ide_devices[count].signature, ide_buf + ATA_IDENT_DEVICETYPE, 2);
            memcpy(&ide_devices[count].capabilities, ide_buf + ATA_IDENT_CAPABILITIES, 2);
            memcpy(&ide_devices[count].command_sets, ide_buf + ATA_IDENT_COMMANDSETS, 4);
            ide_devices[count].size = 0;

            /* Get Size */
            if (ide_devices[count].command_sets & (1 << 26)) {
                memcpy(&ide_devices[count].size, ide_buf + ATA_IDENT_MAX_LBA_EXT, 8);
            } else {
                memcpy(&ide_devices[count].size, ide_buf + ATA_IDENT_MAX_LBA, 4);
            }

            /* Get device model */
            for (k = 0; k < 40; k += 2) {
                ide_devices[count].model[k]     = ide_buf[ATA_IDENT_MODEL + k + 1];
                ide_devices[count].model[k + 1] = ide_buf[ATA_IDENT_MODEL + k];
            }

            ide_devices[count].model[40] = 0;

            for (k = 39; k > 0; k--) {
                if (ide_devices[count].model[k] == ' ') {
                    ide_devices[count].model[k] = '\0';
                } else {
                    break;
                }
            }

            ide_devices[count].dma_enabled = ide_validate_ata(count);
            count++;
        }
    }

    /* Release DMA resources for channels without drives */
    for (int i = 0; i < 2; i++) {
        int used = 0;

        for (int d = 0; d < 4; d++)
            if (ide_devices[d].reserved && ide_devices[d].channel == i) used = 1;
        if (!used && channels[i].dma_ok) {
            free_frames(channels[i].dma_buf_phys, ATA_DMA_BUF_PAGES);
            free_frames(channels[i].prd_phys, 1);
            channels[i].dma_ok       = 0;
            channels[i].dma_buf      = NULL;
            channels[i].dma_buf_phys = 0;
            channels[i].prd_phys     = 0;
            channels[i].prd          = NULL;
        }
    }

    {
        uint8_t total = 0, dma = 0;

        for (int i = 0; i < 4; i++) {
            /* Drives without media have no transfer mode to report */
            if (!ide_devices[i].reserved || ide_devices[i].size == 0) continue;
            total++;
            if (ide_devices[i].dma_enabled) dma++;
        }
        if (total != 0) {
            if (dma == 0) {
                plogk("ide: Transfer mode: PIO%s\n", bmide_valid ? "" : " (BAR4 unavailable)");
            } else if (dma == total) {
                plogk("ide: Transfer mode: DMA (BIOS-configured)\n");
            } else {
                plogk("ide: Transfer mode: DMA on %u/%u drives, PIO on the rest.\n", dma, total);
            }
        }
    }

    /* Print device information */
    for (int i = 0; i < 4; i++)
        if (ide_devices[i].reserved == 1) {
            if (ide_devices[i].type == IDE_ATAPI) {
                plogk("ide: Found ATAPI Drive %u blocks (%u bytes/block) - %s [%s]\n", atapi_devices[i].lba_size, atapi_devices[i].blk_size, ide_devices[i].model,
                      ide_devices[i].dma_enabled ? "DMA" : "PIO");
            } else {
                plogk("ide: Found ATA Drive %llu (KiB) - %s [%s]\n", (ide_devices[i].size / 2), ide_devices[i].model, ide_devices[i].dma_enabled ? "DMA" : "PIO");
            }
        }
}

/* Error handling */
static uint8_t ide_print_error(uint32_t drive, uint8_t err)
{
    if (err == 0) return err;
    if (err == 4) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("ide: Write protected.\n");
        return 8;
    }
    if (ide_devices[drive].type == IDE_ATAPI) {
        uint8_t key  = 0;
        uint8_t asc  = 0;
        uint8_t ascq = 0;
        uint8_t serr;
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);

        /* Sense decides the no-media mapping, so always consume it; only the logs are limited */
        serr = atapi_request_sense(drive, &key, &asc, &ascq);
        if (serr != 0) {
            if (ratelimit_allow(&ratelimit)) plogk("atapi: REQUEST SENSE failed (%u), original error %u on drive %u\n", serr, err, drive);
            return err;
        }
        if (ratelimit_allow(&ratelimit)) plogk("atapi: Sense key 0x%x, ASC 0x%02x, ASCQ 0x%02x on drive %u\n", key, asc, ascq, drive);
        if (key == 2 && asc == 0x3A) return IDE_ERR_NO_MEDIUM;
        return err;
    }
    if (err == 1) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("ide: Device fault.\n");
        err = 19;
    } else if (err == 2) {
        uint8_t st = ide_read(ide_devices[drive].channel, ATA_REG_ERROR);
        if (st & ATA_ER_AMNF) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ide: No address mark found.\n");
            err = 7;
        }
        if (st & ATA_ER_TK0NF) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ide: No media or media error.\n");
            err = 3;
        }
        if (st & ATA_ER_ABRT) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ide: Command aborted.\n");
            err = 20;
        }
        if (st & ATA_ER_MCR) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ide: No media or media error.\n");
            err = 3;
        }
        if (st & ATA_ER_IDNF) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ide: ID mark not found.\n");
            err = 21;
        }
        if (st & ATA_ER_MC) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ide: No media or media error.\n");
            err = 3;
        }
        if (st & ATA_ER_UNC) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ide: Uncorrectable data error.\n");
            err = 22;
        }
        if (st & ATA_ER_BBK) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ide: Bad sectors.\n");
            err = 13;
        }
    } else if (err == 3) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("ide: Reads nothing.\n");
        err = 23;
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
    if (ide_pci_request.response->error != PCI_FINDING_SUCCESS) return;
    bar_reg.parent = ide_pci_request.response->device;

    plogk("ide: Controller found at PCI %04x:%02x:%02x.%01x, vendor 0x%04x, device 0x%04x\n", bar_reg.parent->device->domain, bar_reg.parent->device->bus, bar_reg.parent->device->slot,
          bar_reg.parent->device->func, bar_reg.parent->vendor_id, bar_reg.parent->device_id);

    pci_enable_device(bar_reg.parent, PCI_CMD_IO | PCI_CMD_BUSMASTER);
    register_interrupt_handler(IRQ_14, (void *)ide_irq_primary, 0, 0x8e);
    register_interrupt_handler(IRQ_15, (void *)ide_irq_secondary, 0, 0x8e);

    for (uint32_t idx = 0; idx < 6; idx++) {
        bars[idx] = get_base_address_register(bar_reg.parent, idx);
        pointer_cast_t cast;
        cast.ptr       = bars[idx].address;
        bar_addrs[idx] = cast.val;
    }

    uint8_t bmide_valid = (bars[4].type == input_output) && bar_addrs[4] != 0;
    ide_initialize(bar_addrs[0], bar_addrs[1], bar_addrs[2], bar_addrs[3], bar_addrs[4], bmide_valid);
}

/* Read a byte of data from the specified register of the IDE device */
uint8_t ide_read(uint8_t channel, uint8_t reg)
{
    uint8_t result = 0;

    if (reg > 0x07 && reg < 0x0c) ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN);
    if (reg < 0x08) {
        result = inb(channels[channel].base + reg - 0x00);
    } else if (reg < 0x0c) {
        result = inb(channels[channel].base + reg - 0x06);
    } else if (reg < 0x0e) {
        result = inb(channels[channel].ctrl + reg - 0x0a);
    } else if (reg < 0x16) {
        result = inb(channels[channel].bmide + reg - 0x0e);
    }
    if (reg > 0x07 && reg < 0x0c) ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN);
    return result;
}

/* Write a byte of data to the specified register of the IDE device */
void ide_write(uint8_t channel, uint8_t reg, uint8_t data)
{
    if (reg > 0x07 && reg < 0x0c) {
        /* Control register writes also assert the nIEN interrupt-disable bit */
        outb(channels[channel].ctrl + ATA_REG_CONTROL - 0x0a, 0x80 | channels[channel].nIEN);
    }
    if (reg < 0x08) {
        outb(channels[channel].base + reg - 0x00, data);
    } else if (reg < 0x0c) {
        outb(channels[channel].base + reg - 0x06, data);
    } else if (reg < 0x0e) {
        outb(channels[channel].ctrl + reg - 0x0a, data);
    } else if (reg < 0x16) {
        outb(channels[channel].bmide + reg - 0x0e, data);
    }
    if (reg > 0x07 && reg < 0x0c) outb(channels[channel].ctrl + ATA_REG_CONTROL - 0x0a, channels[channel].nIEN);
}

/* Read multiple words of data from the specified register of the IDE device into the buffer */
void ide_read_buffer(uint8_t channel, uint8_t reg, uint8_t *buffer, uint32_t quads)
{
    if (reg > 0x07 && reg < 0x0c) ide_write(channel, ATA_REG_CONTROL, 0x80 | channels[channel].nIEN);
    if (reg < 0x08) {
        insl(channels[channel].base + reg - 0x00, (uint32_t *)buffer, quads);
    } else if (reg < 0x0c) {
        insl(channels[channel].base + reg - 0x06, (uint32_t *)buffer, quads);
    } else if (reg < 0x0e) {
        insl(channels[channel].ctrl + reg - 0x0a, (uint32_t *)buffer, quads);
    } else if (reg < 0x16) {
        insl(channels[channel].bmide + reg - 0x0e, (uint32_t *)buffer, quads);
    }
    if (reg > 0x07 && reg < 0x0c) ide_write(channel, ATA_REG_CONTROL, channels[channel].nIEN);
}

/* Polling the status of IDE devices */
uint8_t ide_polling(uint8_t channel, uint32_t advanced_check)
{
    int     timeout = IDE_POLL_RETRY;
    uint8_t a;

    for (int i = 0; i < 4; i++) ide_read(channel, ATA_REG_ALTSTATUS);

    a = ide_read(channel, ATA_REG_STATUS);
    while (a & ATA_SR_BSY) {
        a = ide_read(channel, ATA_REG_STATUS);
        if (--timeout <= 0) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ide: BSY timeout on channel %u\n", channel);
            return 3;
        }
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

/* Soft reset the ATA device */
void ide_soft_reset(uint8_t drive)
{
    uint8_t channel = ide_devices[drive].channel;

    channels[channel].irq_pending = 0;
    channels[channel].nIEN        = 0x04;

    ide_write(channel, ATA_REG_CONTROL, 0x04);
    nsleep(5000);

    channels[channel].irq_pending = 0;
    channels[channel].nIEN        = 0x00;

    ide_write(channel, ATA_REG_CONTROL, 0x00);
    nsleep(5000);
    ide_polling(channel, 0);
}

/* Flush write cache; the caller holds the channel lock */
static uint8_t ide_flush_cache_locked(uint8_t drive)
{
    uint8_t channel  = ide_devices[drive].channel;
    uint8_t slavebit = ide_devices[drive].drive;
    uint8_t cmd;

    int tout = IDE_POLL_RETRY;

    while (ide_read(channel, ATA_REG_STATUS) & ATA_SR_BSY) {
        if (--tout <= 0) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("ide: Cache flush BSY timeout on drive %u\n", drive);
            return 3;
        }
        nsleep(10);
    }
    ide_write(channel, ATA_REG_HDDEVSEL, 0xe0 | (slavebit << 4));

    if (ide_devices[drive].command_sets & (1 << 26)) {
        cmd = ATA_CMD_CACHE_FLUSH_EXT;
    } else {
        cmd = ATA_CMD_CACHE_FLUSH;
    }

    ide_write(channel, ATA_REG_COMMAND, cmd);
    if (ide_polling(channel, 0)) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("ide: Cache flush poll failed on drive %u\n", drive);
        return 3;
    }

    uint8_t status = ide_read(channel, ATA_REG_STATUS);

    if (status & ATA_SR_ERR) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("ide: Cache flush error on drive %u\n", drive);
        return 2;
    }
    if (status & ATA_SR_DF) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("ide: Cache flush device fault on drive %u\n", drive);
        return 1;
    }
    return 0;
}

/* Flush write cache */
uint8_t ide_flush_cache(uint8_t drive)
{
    uint8_t channel = ide_devices[drive].channel;
    uint8_t err;

    spin_lock(&channels[channel].lock);
    channels[channel].irq_pending = 0;
    channels[channel].nIEN        = 0x02;
    ide_write(channel, ATA_REG_CONTROL, 0x02);

    err = ide_flush_cache_locked(drive);
    spin_unlock(&channels[channel].lock);

    return err;
}

/* Program the task file for one transfer; the caller holds the channel lock */
static uint8_t ide_ata_setup(uint8_t drive, uint64_t lba, uint8_t numsects, uint8_t *lba_mode)
{
    uint8_t  channel   = ide_devices[drive].channel;
    uint8_t  head      = 0;
    uint8_t  lba_io[6] = {0};
    uint64_t cyl, sect;

    if (ide_devices[drive].command_sets & (1 << 26)) {
        *lba_mode = 2;
        lba_io[0] = (lba >> 0) & 0xff;
        lba_io[1] = (lba >> 8) & 0xff;
        lba_io[2] = (lba >> 16) & 0xff;
        lba_io[3] = (lba >> 24) & 0xff;
        lba_io[4] = (lba >> 32) & 0xff;
        lba_io[5] = (lba >> 40) & 0xff;
    } else if (ide_devices[drive].capabilities & 0x200) {
        *lba_mode = 1;
        lba_io[0] = (lba >> 0) & 0xff;
        lba_io[1] = (lba >> 8) & 0xff;
        lba_io[2] = (lba >> 16) & 0xff;
        head      = (lba >> 24) & 0x0f;
    } else {
        *lba_mode = 0;
        sect      = (lba % 63) + 1;
        cyl       = (lba + 1 - sect) / ((uint64_t)16 * 63);
        lba_io[0] = sect;
        lba_io[1] = (cyl >> 0) & 0xff;
        lba_io[2] = (cyl >> 8) & 0xff;
        head      = (lba + 1 - sect) % ((uint64_t)16 * 63) / (63);
    }
    if (ide_polling(channel, 0) != 0) return 3;

    ide_write(channel, ATA_REG_HDDEVSEL, (*lba_mode == 0 ? 0xa0 : 0xe0) | (ide_devices[drive].drive << 4) | head);
    if (*lba_mode == 2) {
        ide_write(channel, ATA_REG_SECCOUNT1, 0);
        ide_write(channel, ATA_REG_LBA3, lba_io[3]);
        ide_write(channel, ATA_REG_LBA4, lba_io[4]);
        ide_write(channel, ATA_REG_LBA5, lba_io[5]);
    }
    ide_write(channel, ATA_REG_SECCOUNT0, numsects);
    ide_write(channel, ATA_REG_LBA0, lba_io[0]);
    ide_write(channel, ATA_REG_LBA1, lba_io[1]);
    ide_write(channel, ATA_REG_LBA2, lba_io[2]);

    return 0;
}

/* Perform a PIO data transfer; the caller holds the channel lock and programmed the task file */
static uint8_t ide_ata_pio_transfer(uint8_t direction, uint8_t drive, uint8_t lba_mode, uint8_t numsects, uint16_t *edi)
{
    uint8_t  channel = ide_devices[drive].channel;
    uint32_t bus     = channels[channel].base;
    uint16_t words   = 256;
    uint16_t i;
    uint8_t  cmd, err;

    if (direction == ATA_READ) {
        cmd = lba_mode == 2 ? ATA_CMD_READ_PIO_EXT : ATA_CMD_READ_PIO;
    } else {
        cmd = lba_mode == 2 ? ATA_CMD_WRITE_PIO_EXT : ATA_CMD_WRITE_PIO;
    }
    ide_write(channel, ATA_REG_COMMAND, cmd);

    if (direction == ATA_READ) {
        for (i = 0; i < numsects; i++) {
            err = ide_polling(channel, 1);
            if (err != 0) return err;
            insl(bus, (uint32_t *)(edi + ((size_t)i * words)), words / 2);
        }
    } else {
        for (i = 0; i < numsects; i++) {
            err = ide_polling(channel, 1);
            if (err != 0) return err;
            for (uint32_t h = 0; h < words; h++) outw(bus, edi[((size_t)i * words) + h]);
        }
        err = ide_flush_cache_locked(drive);
        if (err != 0) return err;
    }
    return 0;
}

/* Run one bus-master chunk; the caller holds the channel lock and programmed the task file */
static uint8_t ide_ata_dma_chunk(uint8_t direction, uint8_t drive, uint8_t lba_mode, uint8_t numsects, uint16_t *edi)
{
    uint8_t  channel = ide_devices[drive].channel;
    uint16_t bytes   = (uint16_t)(numsects * 512);
    uint8_t  cmd, err;

    if (direction == ATA_WRITE) {
        memcpy(channels[channel].dma_buf, edi, bytes);
        dma_write_barrier();
    }

    ide_bm_clear_status(channel);
    ide_bm_set_prd(channel, bytes);

    if (direction == ATA_READ) {
        cmd = lba_mode == 2 ? ATA_CMD_READ_DMA_EXT : ATA_CMD_READ_DMA;
    } else {
        cmd = lba_mode == 2 ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_WRITE_DMA;
    }
    ide_write(channel, ATA_REG_COMMAND, cmd);
    ide_bm_start(channel, direction == ATA_WRITE);

    err = ide_bm_wait(channel);
    if (err != 0) {
        ide_bm_stop(channel);
        return err;
    }
    if (direction == ATA_READ) {
        dma_read_barrier();
        memcpy(edi, channels[channel].dma_buf, bytes);
    }

    return 0;
}

/* Perform a bus-master DMA transfer in bounce-buffer sized chunks */
static uint8_t ide_ata_dma_transfer(uint8_t direction, uint8_t drive, uint64_t lba, uint8_t numsects, uint16_t *edi)
{
    uint8_t done = 0;
    uint8_t err;

    while (done < numsects) {
        uint8_t chunk    = numsects - done;
        uint8_t lba_mode = 0;

        if (chunk > ATA_DMA_MAX_SECTORS) chunk = ATA_DMA_MAX_SECTORS;
        err = ide_ata_setup(drive, lba + done, chunk, &lba_mode);
        if (err != 0) return err;

        if (lba_mode == 0) return 3;
        err = ide_ata_dma_chunk(direction, drive, lba_mode, chunk, edi + ((size_t)done * 256));
        if (err != 0) return err;

        done += chunk;
    }
    if (direction == ATA_WRITE) return ide_flush_cache_locked(drive);

    return 0;
}

/* Drop a drive to PIO after a bus-master error; a stuck BSY resets the channel */
void ide_dma_fallback(uint8_t drive)
{
    uint8_t is_atapi = drive < 4 && atapi_devices[drive].reserved;
    uint8_t channel  = is_atapi ? atapi_devices[drive].channel : ide_devices[drive].channel;

    if (!channels[channel].bmide) return;
    ide_bm_stop(channel);
    ide_bm_clear_status(channel);
    ide_devices[drive].dma_enabled = 0;

    if (is_atapi) atapi_devices[drive].dma_enabled = 0;
    if (!(ide_read(channel, ATA_REG_STATUS) & ATA_SR_BSY)) return;

    /* A channel reset also clears the firmware's DMA mode */
    static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
    if (ratelimit_allow(&ratelimit)) plogk("ide: Channel %u DMA timeout, resetting to PIO.\n", channel);

    if (is_atapi) {
        atapi_soft_reset(drive);
    } else {
        ide_soft_reset(drive);
    }

    /* SRST resets both drives on the channel, so every ATAPI drive needs its unit attention cleared */
    for (int d = 0; d < 4; d++) {
        if (ide_devices[d].reserved && ide_devices[d].channel == channel) ide_devices[d].dma_enabled = 0;
        if (atapi_devices[d].reserved && atapi_devices[d].channel == channel) {
            atapi_devices[d].dma_enabled = 0;
            atapi_reset_recover(d);
        }
    }
    if (channels[channel].dma_ok) {
        free_frames(channels[channel].dma_buf_phys, ATA_DMA_BUF_PAGES);
        free_frames(channels[channel].prd_phys, 1);
        channels[channel].dma_ok       = 0;
        channels[channel].dma_buf      = NULL;
        channels[channel].dma_buf_phys = 0;
        channels[channel].prd_phys     = 0;
        channels[channel].prd          = NULL;
    }
}

/*
 * The firmware paired host timing with the drive's DMA mode; reuse it instead of
 * per-vendor chipset programming (Intel/VIA/SiS/AMD/NVIDIA/ATI/...).  A boot-time
 * DMA/PIO comparison validates it; full speed still needs those chipset drivers.
 */
static uint8_t ide_validate_ata(uint8_t drive)
{
    uint8_t channel = ide_devices[drive].channel;
    uint8_t lba_mode;
    uint8_t dma_tried = 0;
    uint8_t err;

    if (!channels[channel].dma_ok) return 0;
    if (!(ide_devices[drive].capabilities & 0x100)) return 0;
    if (!(ide_devices[drive].command_sets & (1 << 26)) && !(ide_devices[drive].capabilities & 0x200)) return 0;
    if (ide_devices[drive].size < 8) return 0;

    spin_lock(&channels[channel].lock);
    channels[channel].irq_pending = 0;
    channels[channel].nIEN        = 0x02;
    ide_write(channel, ATA_REG_CONTROL, 0x02);

    err = ide_ata_setup(drive, 0, 8, &lba_mode);
    if (err == 0) err = ide_ata_pio_transfer(ATA_READ, drive, lba_mode, 8, ide_validate_pio);
    if (err == 0) {
        dma_tried = 1;
        err       = ide_ata_dma_transfer(ATA_READ, drive, 0, 8, ide_validate_dma);
        if (err == 0 && memcmp(ide_validate_pio, ide_validate_dma, 4096) != 0) err = 4;
    }

    if (err != 0) {
        if (!dma_tried) {
            plogk("ide: DMA validation skipped on drive %u (PIO read error %u), using PIO.\n", drive, err);
            spin_unlock(&channels[channel].lock);
            return 0;
        }
        ide_dma_fallback(drive);
        plogk("ide: DMA validation failed on drive %u, using PIO.\n", drive);
        spin_unlock(&channels[channel].lock);
        return 0;
    }

    spin_unlock(&channels[channel].lock);
    return 1;
}

/* Read and write ATA devices */
uint8_t ide_ata_access(uint8_t direction, uint8_t drive, uint64_t lba, uint8_t numsects, uint16_t *edi)
{
    uint8_t channel  = ide_devices[drive].channel;
    uint8_t lba_mode = 0;
    uint8_t err;

    /* A zero sector count means 256 sectors in the task file */
    if (numsects == 0) return 0;

    /* Serialise the channel's shared registers across concurrent I/O. Both paths are polled, so no IRQ is needed while the lock is held. */
    spin_lock(&channels[channel].lock);
    channels[channel].irq_pending = 0;
    channels[channel].nIEN        = 0x02;
    ide_write(channel, ATA_REG_CONTROL, 0x02);

    if (ide_devices[drive].dma_enabled && channels[channel].dma_ok) {
        err = ide_ata_dma_transfer(direction, drive, lba, numsects, edi);
        if (err == 0) {
            spin_unlock(&channels[channel].lock);
            return 0;
        }

        ide_dma_fallback(drive);
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("ide: DMA error %u on drive %u, falling back to PIO.\n", err, drive);
    }

    err = ide_ata_setup(drive, lba, numsects, &lba_mode);
    if (err == 0) err = ide_ata_pio_transfer(direction, drive, lba_mode, numsects, edi);

    spin_unlock(&channels[channel].lock);
    return err;
}

/* Read multiple sectors from an IDE device */
int ide_read_sectors(uint8_t drive, uint8_t numsects, uint64_t lba, uint16_t *edi)
{
    if (drive > 3 || !ide_devices[drive].reserved) return -ENODEV;
    if (ide_devices[drive].type == IDE_ATAPI && ide_devices[drive].size == 0) return -ENOMEDIUM;
    if (lba >= ide_devices[drive].size || numsects > ide_devices[drive].size - lba) return -EINVAL;

    /* Reading in PIO mode via polling and IRQ */
    uint8_t err = 0;
    if (ide_devices[drive].type == IDE_ATA) {
        err = ide_ata_access(ATA_READ, drive, lba, numsects, edi);
    } else if (ide_devices[drive].type == IDE_ATAPI) {
        err = atapi_read(drive, lba, numsects, edi);
    }

    err = ide_print_error(drive, err);
    if (err == IDE_ERR_NO_MEDIUM) return -ENOMEDIUM;
    return err ? -EIO : EOK;
}

/* Write multiple sectors to an IDE device */
int ide_write_sectors(uint8_t drive, uint8_t numsects, uint64_t lba, uint16_t *edi)
{
    if (drive > 3 || !ide_devices[drive].reserved) return -ENODEV;
    if ((lba >= ide_devices[drive].size || numsects > ide_devices[drive].size - lba) && (ide_devices[drive].type == IDE_ATA)) return -EINVAL;

    /* Writing in PIO mode via polling and IRQ */
    uint8_t err = 0;
    if (ide_devices[drive].type == IDE_ATA) {
        err = ide_ata_access(ATA_WRITE, drive, lba, numsects, edi);
    } else if (ide_devices[drive].type == IDE_ATAPI) {
        err = 4;
    }
    (void)ide_print_error(drive, err);
    return err ? -EIO : EOK;
}

#endif
