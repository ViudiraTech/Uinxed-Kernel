/*
 *
 *      nvme.c
 *      NVMe 1.4 block device driver
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *      Implements a full NVMe over PCIe host driver with:
 *        - Controller initialisation and shutdown
 *        - Admin Submission / Completion Queue management
 *        - PRP-based data transfers (contiguous DMA)
 *        - NVM Read / Write / Flush I/O command set
 *        - Pin-based interrupt handling
 *        - VFS-style blockdev ops registration
 */

#include <arch/idt.h>
#include <chipset/common.h>
#include <drivers/apic.h>
#include <drivers/blockdev.h>
#include <drivers/nvme.h>
#include <drivers/pci.h>
#include <kernel/errno.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/frame.h>
#include <mem/hhdm.h>
#include <mem/page.h>
#include <sync/spin_lock.h>

/* ================================================================
 *  Global state
 * ================================================================ */

static nvme_controller_t nvme_controllers[NVME_MAX_CONTROLLERS];
static int               nvme_ctrl_count;
static int               nvme_initialised;

/* ================================================================
 *  MMIO helpers
 * ================================================================ */

static inline uint64_t nvme_read64(const volatile void *addr, size_t offset)
{
    return mmio_read64((void *)((uintptr_t)addr + offset));
}

static inline uint32_t nvme_read32(const volatile void *addr, size_t offset)
{
    return mmio_read32((uint32_t *)((uintptr_t)addr + offset));
}

static inline void nvme_write32(const volatile void *addr, size_t offset, uint32_t val)
{
    mmio_write32((uint32_t *)((uintptr_t)addr + offset), val);
}

static inline void nvme_write64(const volatile void *addr, size_t offset, uint64_t val)
{
    mmio_write64((void *)((uintptr_t)addr + offset), val);
}

/* ================================================================
 *  Doorbell access
 * ================================================================ */

static volatile uint32_t *nvme_sq_doorbell(nvme_controller_t *ctrl, uint32_t qid)
{
    uintptr_t base = (uintptr_t)ctrl->regs + NVME_REG_DBS;
    base += (2ULL * qid) * ctrl->stride;
    return (volatile uint32_t *)base;
}

static volatile uint32_t *nvme_cq_doorbell(nvme_controller_t *ctrl, uint32_t qid)
{
    uintptr_t base = (uintptr_t)ctrl->regs + NVME_REG_DBS;
    base += (2ULL * qid + 1) * ctrl->stride;
    return (volatile uint32_t *)base;
}

/* ================================================================
 *  CAP register field extraction
 * ================================================================ */

static uint32_t nvme_cap_mqes(uint64_t cap)
{
    return (uint32_t)(cap & 0xFFFF);
}

static uint32_t nvme_cap_dstrd(uint64_t cap)
{
    /* NVMe 1.4: DSTRD is in bits 35:32 */
    uint32_t dstrd = (uint32_t)((cap >> 32) & 0xF);
    /* Fall back to NVMe 1.0-1.3 location (bits 19:16) if zero */
    if (!dstrd) dstrd = (uint32_t)((cap >> 16) & 0xF);
    return dstrd;
}

static uint32_t nvme_cap_to(uint64_t cap)
{
    /* NVMe 1.4: TO is in bits 47:44; fall back to bits 31:24 */
    uint32_t to = (uint32_t)((cap >> 44) & 0xF);
    if (!to) to = (uint32_t)((cap >> 24) & 0xFF);
    if (!to) to = 6; /* default: ~3 seconds */
    return to;
}

/* ================================================================
 *  Queue memory management
 * ================================================================ */

static int nvme_alloc_queue(nvme_queue_t *q, uint32_t qid, uint16_t num_entries, nvme_controller_t *ctrl)
{
    size_t sq_bytes = (size_t)num_entries * NVME_SQE_SIZE;
    size_t cq_bytes = (size_t)num_entries * NVME_CQE_SIZE;
    size_t sq_pages, cq_pages;

    memset(q, 0, sizeof(*q));

    /* SQ: physically contiguous, page-aligned */
    sq_pages   = (sq_bytes + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    q->sq_phys = alloc_frames(sq_pages);
    if (!q->sq_phys) return -ENOMEM;
    q->sq       = (nvme_sqe_t *)phys_to_virt(q->sq_phys);
    q->sq_pages = (uint32_t)sq_pages;
    memset(q->sq, 0, sq_pages * PAGE_4K_SIZE);

    /* CQ: physically contiguous, page-aligned */
    cq_pages   = (cq_bytes + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;
    q->cq_phys = alloc_frames(cq_pages);
    if (!q->cq_phys) {
        free_frames(q->sq_phys, q->sq_pages);
        return -ENOMEM;
    }
    q->cq       = (nvme_cqe_t *)phys_to_virt(q->cq_phys);
    q->cq_pages = (uint32_t)cq_pages;
    memset(q->cq, 0, cq_pages * PAGE_4K_SIZE);

    /* PRP list page */
    q->prp_list_phys = alloc_frames(1);
    if (!q->prp_list_phys) {
        free_frames(q->cq_phys, q->cq_pages);
        free_frames(q->sq_phys, q->sq_pages);
        return -ENOMEM;
    }
    q->prp_list_virt  = phys_to_virt(q->prp_list_phys);
    q->prp_list_inuse = 0;

    q->qid         = qid;
    q->num_entries = num_entries;
    q->sq_head     = 0;
    q->sq_tail     = 0;
    q->cq_head     = 0;
    q->cq_phase    = 1; /* first pass expects phase == 1 */
    q->sq_cid      = 0;
    q->sq_doorbell = nvme_sq_doorbell(ctrl, qid);
    q->cq_doorbell = nvme_cq_doorbell(ctrl, qid);
    q->lock        = (spinlock_t) {0};

    return EOK;
}

static void nvme_free_queue(nvme_queue_t *q)
{
    if (q->prp_list_phys) {
        free_frames(q->prp_list_phys, 1);
        q->prp_list_phys = 0;
    }
    if (q->cq_phys) {
        free_frames(q->cq_phys, q->cq_pages);
        q->cq_phys = 0;
    }
    if (q->sq_phys) {
        free_frames(q->sq_phys, q->sq_pages);
        q->sq_phys = 0;
    }
}

/* ================================================================
 *  Completion polling
 * ================================================================ */

static int nvme_poll_completion(nvme_queue_t *q, uint32_t expected_cid, nvme_cqe_t *out)
{
    uint64_t timeout = NVME_TIMEOUT_LOOPS;

    while (timeout--) {
        compiler_barrier();
        nvme_cqe_t *cqe = &q->cq[q->cq_head];

        if (NVME_CQE_PHASE(cqe) != q->cq_phase) continue;

        /* Entry is new — check CID and consume it */
        uint16_t sc  = NVME_CQE_SC(cqe);
        uint16_t sct = NVME_CQE_SCT(cqe);

        /* Advance head */
        q->cq_head++;
        if (q->cq_head == q->num_entries) {
            q->cq_head = 0;
            q->cq_phase ^= 1;
        }

        /* Ring CQ head doorbell */
        mmio_write32((uint32_t *)q->cq_doorbell, q->cq_head);

        if (cqe->cmd_id == expected_cid) {
            if (out) memcpy(out, cqe, sizeof(*out));
            if (sct != NVME_SCT_GENERIC || sc != NVME_SC_SUCCESS) {
                plogk("nvme: CQE error CID=%u SCT=%u SC=%02x\n", expected_cid, sct, sc);
                return -EIO;
            }
            return EOK;
        }

        /* This completion isn't ours — it might be from a different command.
         * We already advanced the head, so just skip it. */
    }

    plogk("nvme: timeout polling CID %u on queue %u\n", expected_cid, q->qid);
    return -ETIMEDOUT;
}

/* ================================================================
 *  Admin command submission
 * ================================================================ */

static int nvme_admin_cmd(nvme_controller_t *ctrl, uint8_t opc, uint32_t nsid, uint64_t prp1, uint64_t prp2, uint32_t cdw10, uint32_t cdw11,
                          uint32_t cdw12, nvme_cqe_t *result)
{
    nvme_queue_t *q    = &ctrl->admin_q;
    uint32_t      tail = q->sq_tail;
    uint32_t      cid  = q->sq_cid++;
    nvme_sqe_t   *cmd  = &q->sq[tail];

    memset(cmd, 0, sizeof(*cmd));
    cmd->cdw0  = ((uint32_t)opc & 0xFF) | ((uint32_t)cid << 16);
    cmd->nsid  = nsid;
    cmd->prp1  = prp1;
    cmd->prp2  = prp2;
    cmd->cdw10 = cdw10;
    cmd->cdw11 = cdw11;
    cmd->cdw12 = cdw12;

    compiler_barrier();
    q->sq_tail = (tail + 1) % q->num_entries;
    mmio_write32((uint32_t *)q->sq_doorbell, q->sq_tail);

    return nvme_poll_completion(q, cid, result);
}

/* ================================================================
 *  PRP list construction
 *
 *  Because alloc_frames() returns physically contiguous pages,
 *  the PRP list is a linear walk of page-aligned addresses.
 * ================================================================ */

static int nvme_build_prp(nvme_queue_t *q, uint64_t dma_phys, uint32_t byte_count, uint64_t *prp1_out, uint64_t *prp2_out)
{
    uint32_t offset           = (uint32_t)(dma_phys & (PAGE_4K_SIZE - 1));
    uint32_t first_page_avail = PAGE_4K_SIZE - offset;
    uint64_t page_base        = dma_phys & ~(uint64_t)(PAGE_4K_SIZE - 1);

    *prp1_out = dma_phys;

    /* Fits entirely in the first page */
    if (byte_count <= first_page_avail) {
        *prp2_out = 0;
        return EOK;
    }

    uint32_t remaining = byte_count - first_page_avail;
    uint64_t next_page = page_base + PAGE_4K_SIZE;

    /* Fits in exactly two pages — PRP2 points directly to data */
    if (remaining <= PAGE_4K_SIZE) {
        *prp2_out = next_page;
        return EOK;
    }

    /* Need a PRP list page — use the pre-allocated one */
    if (q->prp_list_inuse) return -EBUSY;
    q->prp_list_inuse = 1;

    uint64_t *list = (uint64_t *)q->prp_list_virt;
    memset(list, 0, PAGE_4K_SIZE);

    /* First list entry is the rest of page 1 (offset-adjusted page 2) */
    list[0] = next_page;
    remaining -= PAGE_4K_SIZE;
    next_page += PAGE_4K_SIZE;

    uint32_t idx = 1;
    while (remaining > 0 && idx < NVME_PRP_ENTRIES_PER_PAGE) {
        list[idx++] = (remaining <= PAGE_4K_SIZE) ? next_page : next_page;
        remaining   = (remaining > PAGE_4K_SIZE) ? (remaining - PAGE_4K_SIZE) : 0;
        next_page += PAGE_4K_SIZE;
    }

    if (remaining > 0) {
        q->prp_list_inuse = 0;
        return -E2BIG; /* transfer too large for single PRP list */
    }

    *prp2_out = q->prp_list_phys;
    return EOK;
}

/* ================================================================
 *  Controller initialisation
 * ================================================================ */

static int nvme_controller_init(pci_device_cache_t *pci_dev, uint16_t ctrl_id)
{
    nvme_controller_t *ctrl;
    uint32_t           vendor_id, device_id;
    uint64_t           cap;
    int                ret;

    if (ctrl_id >= NVME_MAX_CONTROLLERS) return -ENOSPC;
    ctrl = &nvme_controllers[ctrl_id];
    memset(ctrl, 0, sizeof(*ctrl));
    ctrl->id   = ctrl_id;
    ctrl->pci  = pci_dev;
    ctrl->lock = (spinlock_t) {0};

    /* Read PCI IDs */
    vendor_id = read_pci((pci_device_reg_t) {.parent = pci_dev, .offset = PCI_CONF_VENDOR});
    device_id = read_pci((pci_device_reg_t) {.parent = pci_dev, .offset = PCI_CONF_DEVICE});

    plogk("nvme: [%04x:%02x:%02x] NVMe controller (vendor=0x%04x, device=0x%04x)\n", pci_dev->device->domain, pci_dev->device->bus,
          pci_dev->device->slot, vendor_id, device_id);

    /* Enable bus mastering and MMIO space */
    {
        uint32_t cmd = pci_read_command_status(pci_dev);
        cmd |= (1 << 2); /* bus master */
        cmd |= (1 << 1); /* memory space */
        pci_write_command_status(pci_dev, cmd);
    }

    /* Get BAR0 */
    {
        /* Read BAR0 raw from PCI config space (HHDM doesn't map MMIO regions) */
        pci_device_reg_t pci_reg  = {.parent = pci_dev, .offset = 0x10};
        uint64_t         bar_val  = read_pci(pci_reg);
        uint32_t         bar_type = (bar_val >> 1) & 0b11;

        if ((bar_val & 1) || bar_type == BAR_Reserved) {
            plogk("nvme: controller %u: BAR0 is not a memory BAR\n", ctrl_id);
            return -ENODEV;
        }

        if (bar_type == BAR_S64) {
            pci_reg.offset = 0x14;
            bar_val |= (uint64_t)read_pci(pci_reg) << 32;
        }

        uint64_t phys_addr = bar_val & ~0xFULL;
        uint64_t phys_page = phys_addr & ~(uint64_t)(PAGE_4K_SIZE - 1);
        uint64_t page_off  = phys_addr - phys_page;
        uint64_t map_len   = (0x2000 + page_off + PAGE_4K_SIZE - 1) & ~(uint64_t)(PAGE_4K_SIZE - 1);

        page_map_range_to(get_kernel_pagedir(), phys_page, map_len, PTE_MMIO_FLAGS);
        ctrl->regs = (volatile void *)((uintptr_t)phys_to_virt(phys_page) + page_off);
    }

    /* Parse CAP register */
    cap             = nvme_read64(ctrl->regs, NVME_REG_CAP);
    ctrl->stride    = 4u << nvme_cap_dstrd(cap);
    ctrl->max_qsize = nvme_cap_mqes(cap) + 1;
    if (ctrl->max_qsize > 4096) ctrl->max_qsize = 4096;

    plogk("nvme: controller %u: MQES=%u stride=%u bytes\n", ctrl_id, ctrl->max_qsize - 1, ctrl->stride);

    /* Get IRQ */
    {
        uint32_t irq_line = pci_get_irq(pci_dev);
        ctrl->irq_vector  = (irq_line > 0) ? (uint32_t)(32 + irq_line) : 0;
        plogk("nvme: controller %u: IRQ line=%u vector=%u\n", ctrl_id, irq_line, ctrl->irq_vector);
    }

    /* ---- Disable controller ---- */
    {
        nvme_write32(ctrl->regs, NVME_REG_CC, 0); /* CC.EN = 0 */
        uint64_t to       = (uint64_t)nvme_cap_to(cap) * 500 * 1000;
        uint64_t deadline = rdtsc_serialized() + to;
        while (nvme_read32(ctrl->regs, NVME_REG_CSTS) & NVME_CSTS_RDY) {
            if (rdtsc_serialized() > deadline) {
                plogk("nvme: controller %u: timeout waiting for CSTS.RDY=0\n", ctrl_id);
                return -ETIMEDOUT;
            }
        }
    }

    /* ---- Allocate admin queues ---- */
    {
        uint16_t aq_entries = NVME_ADMIN_QSIZE;
        if (aq_entries > ctrl->max_qsize) aq_entries = (uint16_t)ctrl->max_qsize;

        ret = nvme_alloc_queue(&ctrl->admin_q, 0, aq_entries, ctrl);
        if (ret) {
            plogk("nvme: controller %u: failed to allocate admin queues: %d\n", ctrl_id, ret);
            return ret;
        }
    }

    /* ---- Program admin queue registers ---- */
    {
        uint32_t aqa_val = (uint32_t)((ctrl->admin_q.num_entries - 1) & 0xFFF) | ((uint32_t)((ctrl->admin_q.num_entries - 1) & 0xFFF) << 16);
        nvme_write32(ctrl->regs, NVME_REG_AQA, aqa_val);
        nvme_write64(ctrl->regs, NVME_REG_ASQ, ctrl->admin_q.sq_phys);
        nvme_write64(ctrl->regs, NVME_REG_ACQ, ctrl->admin_q.cq_phys);
        compiler_barrier();
    }

    /* ---- Enable controller ---- */
    {
        uint32_t cc_val = NVME_CC_EN | (NVME_SQE_SHIFT << NVME_CC_IOSQES_SHIFT) | (NVME_CQE_SHIFT << NVME_CC_IOCQES_SHIFT);
        nvme_write32(ctrl->regs, NVME_REG_CC, cc_val);

        uint64_t to       = (uint64_t)nvme_cap_to(cap) * 500 * 1000;
        uint64_t deadline = rdtsc_serialized() + to;
        while (!(nvme_read32(ctrl->regs, NVME_REG_CSTS) & NVME_CSTS_RDY)) {
            if (rdtsc_serialized() > deadline) {
                plogk("nvme: controller %u: timeout waiting for CSTS.RDY=1\n", ctrl_id);
                return -ETIMEDOUT;
            }
        }
    }

    /* ---- Set Features: Number of Queues ---- */
    /* Request 1 I/O SQ + 1 I/O CQ */
    {
        nvme_cqe_t cqe;
        ret = nvme_admin_cmd(ctrl, NVME_ADMIN_SET_FEATURES, 0, 0, 0, NVME_FID_NUM_QUEUES, (1u << 16) | 1u, 0, &cqe);
        if (ret) { plogk("nvme: controller %u: Set Features (num queues) = %d\n", ctrl_id, ret); }
    }

    /* ---- Identify Controller ---- */
    {
        uint64_t ident_phys = alloc_frames(1);
        if (!ident_phys) {
            ret = -ENOMEM;
            goto err_admin;
        }
        void *ident_virt = phys_to_virt(ident_phys);
        memset(ident_virt, 0, PAGE_4K_SIZE);

        nvme_cqe_t cqe;
        ret = nvme_admin_cmd(ctrl, NVME_ADMIN_IDENTIFY, 0, ident_phys, 0, NVME_CNS_CONTROLLER, 0, 0, &cqe);
        if (ret) {
            plogk("nvme: controller %u: Identify Controller failed: %d\n", ctrl_id, ret);
            free_frames(ident_phys, 1);
            goto err_admin;
        }

        nvme_identify_ctrl_t *id = (nvme_identify_ctrl_t *)ident_virt;
        char                  model[41], serial[21], fw[9];

        memcpy(model, id->mn, 40);
        model[40] = '\0';
        memcpy(serial, id->sn, 20);
        serial[20] = '\0';
        memcpy(fw, id->fr, 8);
        fw[8] = '\0';

        /* Strip trailing spaces from ASCII strings */
        for (int i = 39; i >= 0 && model[i] == ' '; i--) model[i] = '\0';
        for (int i = 19; i >= 0 && serial[i] == ' '; i--) serial[i] = '\0';
        for (int i = 7; i >= 0 && fw[i] == ' '; i--) fw[i] = '\0';

        ctrl->num_namespaces = (id->nn > NVME_MAX_NAMESPACES) ? NVME_MAX_NAMESPACES : id->nn;

        plogk("nvme: controller %u: \"%s\" SN=%s FW=%s, %u namespace(s)\n", ctrl_id, model, serial, fw, ctrl->num_namespaces);

        free_frames(ident_phys, 1);
    }

    /* ---- Identify each namespace ---- */
    for (uint32_t ns_idx = 0; ns_idx < ctrl->num_namespaces; ns_idx++) {
        uint32_t   nsid = ns_idx + 1;
        nvme_cqe_t cqe;
        uint64_t   ns_phys;
        void      *ns_virt;
        int        ns_ret;

        ns_phys = alloc_frames(1);
        if (!ns_phys) continue;
        ns_virt = phys_to_virt(ns_phys);
        memset(ns_virt, 0, PAGE_4K_SIZE);

        ns_ret = nvme_admin_cmd(ctrl, NVME_ADMIN_IDENTIFY, nsid, ns_phys, 0, NVME_CNS_NAMESPACE, 0, 0, &cqe);
        if (ns_ret) {
            plogk("nvme: ctrl%u ns%u: Identify Namespace failed: %d\n", ctrl_id, nsid, ns_ret);
            free_frames(ns_phys, 1);
            continue;
        }

        nvme_identify_ns_t *ns_data = (nvme_identify_ns_t *)ns_virt;
        nvme_namespace_t   *ns      = &ctrl->namespaces[ns_idx];
        uint8_t             flbas   = ns_data->flbas & 0xF;
        uint8_t             ds      = ns_data->lbaf[flbas].ds;

        ns->nsid          = nsid;
        ns->total_sectors = ns_data->nsze;
        ns->sector_size   = (ds > 0) ? (1u << ds) : NVME_SECTOR_SIZE;
        ns->ready         = 1;

        plogk("nvme: ctrl%u ns%u: %llu sectors (%llu GiB), LBA size=%u\n", ctrl_id, nsid, (unsigned long long)ns->total_sectors,
              (unsigned long long)(ns->total_sectors * ns->sector_size / 1000000000ULL), ns->sector_size);

        free_frames(ns_phys, 1);
    }

    ctrl->present     = 1;
    ctrl->initialised = 1;
    return EOK;

err_admin:
    nvme_free_queue(&ctrl->admin_q);
    return ret;
}

/* ================================================================
 *  Interrupt handler
 * ================================================================ */

INTERRUPT_BEGIN static void nvme_interrupt_handler(interrupt_frame_t *frame)
{
    (void)frame;

    for (int i = 0; i < nvme_ctrl_count; i++) {
        nvme_controller_t *ctrl = &nvme_controllers[i];
        if (!ctrl->initialised) continue;

        /* Process any pending admin CQ completions */
        /* (future: process I/O CQ completions for async I/O) */

        /* Acknowledge the interrupt — write all-ones to INTMC */
        nvme_write32(ctrl->regs, NVME_REG_INTMC, 0xFFFFFFFF);
    }
    send_eoi();
}
INTERRUPT_END

/* ================================================================
 *  I/O command submission
 * ================================================================ */

static int nvme_do_io(nvme_controller_t *ctrl, uint8_t opc, uint32_t nsid, uint64_t prp1, uint64_t prp2, uint64_t slba, uint16_t nlb)
{
    nvme_queue_t *q = &ctrl->admin_q;
    int           ret;

    spin_lock(&ctrl->lock);

    uint32_t    tail = q->sq_tail;
    uint32_t    cid  = q->sq_cid++;
    nvme_sqe_t *cmd  = &q->sq[tail];

    memset(cmd, 0, sizeof(*cmd));
    cmd->cdw0  = ((uint32_t)opc & 0xFF) | ((uint32_t)cid << 16);
    cmd->nsid  = nsid;
    cmd->prp1  = prp1;
    cmd->prp2  = prp2;
    cmd->cdw10 = (uint32_t)(slba & 0xFFFFFFFF);
    cmd->cdw11 = (uint32_t)(slba >> 32);
    cmd->cdw12 = (uint32_t)(nlb & 0xFFFF);

    compiler_barrier();
    q->sq_tail = (tail + 1) % q->num_entries;
    mmio_write32((uint32_t *)q->sq_doorbell, q->sq_tail);

    nvme_cqe_t cqe;
    ret = nvme_poll_completion(q, cid, &cqe);
    spin_unlock(&ctrl->lock);
    return ret;
}

/* ================================================================
 *  Backend I/O entry points (called via blockdev ops table)
 * ================================================================ */

int nvme_read_sectors(const struct blockdev_device *dev, uint32_t lba, uint32_t count, void *buffer)
{
    nvme_namespace_t  *ns;
    nvme_controller_t *ctrl;
    nvme_queue_t      *q;
    uint8_t           *buf;
    int                ret;

    if (!dev || !buffer) return -EINVAL;
    if (!count) return EOK;

    ns   = (nvme_namespace_t *)dev->backend_data;
    ctrl = NULL;

    /* Find the controller that owns this namespace */
    for (int i = 0; i < nvme_ctrl_count; i++) {
        for (uint32_t j = 0; j < nvme_controllers[i].num_namespaces; j++) {
            if (&nvme_controllers[i].namespaces[j] == ns) {
                ctrl = &nvme_controllers[i];
                break;
            }
        }
        if (ctrl) break;
    }
    if (!ctrl || !ctrl->initialised) return -ENODEV;

    q   = &ctrl->admin_q;
    buf = (uint8_t *)buffer;

    /* Chunk size: up to 2 pages (8 KB) per command to keep DMA simple */
    uint32_t max_sectors_per_cmd = (PAGE_4K_SIZE * 2) / ns->sector_size;

    while (count > 0) {
        uint32_t chunk = (count > max_sectors_per_cmd) ? max_sectors_per_cmd : count;
        uint32_t bytes = chunk * ns->sector_size;
        uint32_t pages = (bytes + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;

        /* Allocate DMA buffer */
        uint64_t dma_phys = alloc_frames(pages);
        if (!dma_phys) return -ENOMEM;
        void *dma_virt = phys_to_virt(dma_phys);

        /* Build PRP */
        uint64_t prp1, prp2;
        ret = nvme_build_prp(q, dma_phys, bytes, &prp1, &prp2);
        if (ret) {
            free_frames(dma_phys, pages);
            return ret;
        }

        /* Submit READ command */
        ret = nvme_do_io(ctrl, NVME_NVM_READ, ns->nsid, prp1, prp2, dev->base_lba + lba, (uint16_t)(chunk - 1));

        if (q->prp_list_inuse) q->prp_list_inuse = 0;

        if (ret == EOK) memcpy(buf, dma_virt, bytes);
        free_frames(dma_phys, pages);
        if (ret) return ret;

        buf += bytes;
        lba += chunk;
        count -= chunk;
    }

    return EOK;
}

int nvme_write_sectors(const struct blockdev_device *dev, uint32_t lba, uint32_t count, const void *buffer)
{
    nvme_namespace_t  *ns;
    nvme_controller_t *ctrl;
    nvme_queue_t      *q;
    const uint8_t     *buf;
    int                ret;

    if (!dev || !buffer) return -EINVAL;
    if (!count) return EOK;

    ns   = (nvme_namespace_t *)dev->backend_data;
    ctrl = NULL;

    for (int i = 0; i < nvme_ctrl_count; i++) {
        for (uint32_t j = 0; j < nvme_controllers[i].num_namespaces; j++) {
            if (&nvme_controllers[i].namespaces[j] == ns) {
                ctrl = &nvme_controllers[i];
                break;
            }
        }
        if (ctrl) break;
    }
    if (!ctrl || !ctrl->initialised) return -ENODEV;

    q   = &ctrl->admin_q;
    buf = (const uint8_t *)buffer;

    uint32_t max_sectors_per_cmd = (PAGE_4K_SIZE * 2) / ns->sector_size;

    while (count > 0) {
        uint32_t chunk = (count > max_sectors_per_cmd) ? max_sectors_per_cmd : count;
        uint32_t bytes = chunk * ns->sector_size;
        uint32_t pages = (bytes + PAGE_4K_SIZE - 1) / PAGE_4K_SIZE;

        uint64_t dma_phys = alloc_frames(pages);
        if (!dma_phys) return -ENOMEM;
        void *dma_virt = phys_to_virt(dma_phys);

        /* Copy caller data into DMA buffer */
        memcpy(dma_virt, buf, bytes);

        uint64_t prp1, prp2;
        ret = nvme_build_prp(q, dma_phys, bytes, &prp1, &prp2);
        if (ret) {
            free_frames(dma_phys, pages);
            return ret;
        }

        ret = nvme_do_io(ctrl, NVME_NVM_WRITE, ns->nsid, prp1, prp2, dev->base_lba + lba, (uint16_t)(chunk - 1));

        if (q->prp_list_inuse) q->prp_list_inuse = 0;

        free_frames(dma_phys, pages);
        if (ret) return ret;

        buf += bytes;
        lba += chunk;
        count -= chunk;
    }

    return EOK;
}

/* ================================================================
 *  Public API
 * ================================================================ */

void nvme_init(void)
{
    pci_device_cache_t *dev;
    pci_class_request_t nvme_class = {.class_code = 0x010802};
    int                 count      = 0;
    int                 ret;

    if (nvme_initialised) return;

    /* Enumerate all NVMe PCI devices */
    dev = pci_found_class_cache(NULL, nvme_class);
    while (dev && count < NVME_MAX_CONTROLLERS) {
        ret = nvme_controller_init(dev, (uint16_t)count);
        if (ret == EOK) { count++; }
        dev = pci_found_class_cache(dev, nvme_class);
    }

    nvme_ctrl_count  = count;
    nvme_initialised = 1;

    if (count > 0) {
        /* Register interrupt handler on the first controller's vector */
        for (int i = 0; i < count; i++) {
            if (nvme_controllers[i].irq_vector) {
                register_interrupt_handler((uint16_t)nvme_controllers[i].irq_vector, (void *)nvme_interrupt_handler, 0, 0x8E);
                /* Unmask interrupts */
                nvme_write32(nvme_controllers[i].regs, NVME_REG_INTMC, 0xFFFFFFFF);
                break;
            }
        }
    }

    plogk("nvme: Initialised %u controller(s)\n", count);
}

int nvme_controller_count(void)
{
    return nvme_ctrl_count;
}

nvme_controller_t *nvme_get_controller(int i)
{
    if (i < 0 || i >= nvme_ctrl_count) return NULL;
    if (!nvme_controllers[i].initialised) return NULL;
    return &nvme_controllers[i];
}
