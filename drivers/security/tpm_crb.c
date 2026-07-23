/*
 *
 *      tpm_crb.c
 *      TPM CRB (Command Response Buffer) interface implementation
 *
 *      2026/7/23 By MicroFish
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/tpm.h>
#include <kernel/printk.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/hhdm.h>

/* ======================================================================
 *  CRB MMIO register access helpers
 * ====================================================================== */

static inline void *crb_reg_addr(tpm_device_t *dev, uint32_t offset)
{
    return (void *)((uintptr_t)dev->mmio_base + offset);
}

static inline uint32_t crb_read32(tpm_device_t *dev, uint32_t offset)
{
    return *(volatile uint32_t *)crb_reg_addr(dev, offset);
}

static inline void crb_write32(tpm_device_t *dev, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)crb_reg_addr(dev, offset) = value;
}

/* ======================================================================
 *  CRB register polling
 * ====================================================================== */

struct crb_reg32_ctx {
        tpm_device_t *dev;
        uint32_t      offset;
        uint32_t      mask;
        uint32_t      expected;
};

static int check_reg32(void *ctx)
{
    struct crb_reg32_ctx *c   = (struct crb_reg32_ctx *)ctx;
    uint32_t              val = crb_read32(c->dev, c->offset);
    return ((val & c->mask) == c->expected) ? 1 : 0;
}

static int crb_wait_reg32(tpm_device_t *dev, uint32_t offset, uint32_t mask, uint32_t expected, uint32_t timeout_ms)
{
    struct crb_reg32_ctx ctx;
    ctx.dev      = dev;
    ctx.offset   = offset;
    ctx.mask     = mask;
    ctx.expected = expected;

    if (check_reg32(&ctx)) return 0;
    return tpm_poll_timeout(check_reg32, &ctx, timeout_ms) ? 0 : -1;
}

/* ======================================================================
 *  CRB status
 * ====================================================================== */

static uint8_t crb_status(tpm_device_t *dev)
{
    uint32_t start = crb_read32(dev, CRB_CTRL_START_OFFSET);
    if (!(start & CRB_START_INVOKE)) return CRB_DRV_STS_COMPLETE;
    return 0;
}

/* ======================================================================
 *  CRB cmdReady / goIdle (standard CRB: methods 7, 8)
 * ====================================================================== */

static int crb_cmd_ready(tpm_device_t *dev)
{
    crb_write32(dev, CRB_CTRL_REQ_OFFSET, CRB_CTRL_REQ_CMD_READY);
    return crb_wait_reg32(dev, CRB_CTRL_REQ_OFFSET, CRB_CTRL_REQ_CMD_READY, 0, dev->timeout_c);
}

static int crb_go_idle(tpm_device_t *dev)
{
    crb_write32(dev, CRB_CTRL_REQ_OFFSET, CRB_CTRL_REQ_GO_IDLE);
    return crb_wait_reg32(dev, CRB_CTRL_REQ_OFFSET, CRB_CTRL_REQ_GO_IDLE, 0, dev->timeout_c);
}

/* ======================================================================
 *  CRB locality management (standard CRB: methods 7, 8)
 * ====================================================================== */

static int crb_request_locality(tpm_device_t *dev, int l)
{
    (void)l;
    uint32_t value = CRB_LOC_STATE_LOC_ASSIGNED | CRB_LOC_STATE_TPM_REG_VALID;

    crb_write32(dev, CRB_LOC_CTRL_OFFSET, CRB_LOC_CTRL_REQUEST_ACCESS);

    int rc = crb_wait_reg32(dev, CRB_LOC_STATE_OFFSET, value, value, dev->timeout_c);
    if (rc < 0) return -1;
    dev->locality = 0;
    return 0;
}

static void crb_relinquish_locality(tpm_device_t *dev, int l)
{
    (void)l;
    uint32_t mask  = CRB_LOC_STATE_LOC_ASSIGNED | CRB_LOC_STATE_TPM_REG_VALID;
    uint32_t value = CRB_LOC_STATE_TPM_REG_VALID;

    crb_write32(dev, CRB_LOC_CTRL_OFFSET, CRB_LOC_CTRL_RELINQUISH);
    crb_wait_reg32(dev, CRB_LOC_STATE_OFFSET, mask, value, dev->timeout_c);
    dev->locality = -1;
}

/* No-op versions for method 2 (locality managed by ACPI/firmware) */
static int crb_nop_req_locality(tpm_device_t *dev, int l)
{
    (void)dev;
    (void)l;
    dev->locality = 0;
    return 0;
}
static void crb_nop_rel_locality(tpm_device_t *dev, int l)
{
    (void)dev;
    (void)l;
    dev->locality = -1;
}

/* ======================================================================
 *  CRB cancel
 * ====================================================================== */

static void crb_cancel(tpm_device_t *dev)
{
    crb_write32(dev, CRB_CTRL_CANCEL_OFFSET, CRB_CANCEL_INVOKE);
}

/* ======================================================================
 *  CRB send
 * ====================================================================== */

static int crb_send(tpm_device_t *dev, uint8_t *buf, size_t len)
{
    int rc;

    if (len > dev->crb_cmd_size) return -1;
    crb_write32(dev, CRB_CTRL_CANCEL_OFFSET, 0);

    /* Standard CRB (7,8): issue cmdReady */
    if (dev->crb_sm != ACPI_TPM2_START_METHOD) {
        rc = crb_cmd_ready(dev);
        if (rc < 0) return rc;
    }

    volatile uint8_t *cmd = (volatile uint8_t *)dev->crb_cmd_buf;
    for (size_t i = 0; i < len; i++) cmd[i] = buf[i];

    crb_write32(dev, CRB_CTRL_START_OFFSET, CRB_START_INVOKE);
    return 0;
}

/* ======================================================================
 *  CRB recv
 * ====================================================================== */

static int crb_recv(tpm_device_t *dev, uint8_t *buf, size_t maxlen)
{
    uint8_t  sts;
    int      expected;
    uint64_t deadline;
    int      has_idle = (dev->crb_sm != ACPI_TPM2_START_METHOD);

    if (maxlen < TPM_HEADER_SIZE) return -1;

    deadline = nano_time() + (uint64_t)dev->timeout_c * 1000000ULL;
    for (;;) {
        sts = crb_status(dev);
        if (sts & CRB_DRV_STS_COMPLETE) break;
        if (nano_time() >= deadline) {
            if (has_idle) crb_go_idle(dev);
            return -1;
        }
        tpm_udelay(200);
    }

    uint32_t ctrl_sts = crb_read32(dev, CRB_CTRL_STS_OFFSET);
    if (ctrl_sts & CRB_CTRL_STS_ERROR) {
        if (has_idle) crb_go_idle(dev);
        return -1;
    }

    volatile uint8_t *rsp = (volatile uint8_t *)dev->crb_rsp_buf;
    for (int i = 0; i < TPM_HEADER_SIZE; i++) buf[i] = rsp[i];

    expected = (buf[2] << 24) | (buf[3] << 16) | (buf[4] << 8) | buf[5];
    if (expected > (int)maxlen || expected < TPM_HEADER_SIZE) {
        if (has_idle) crb_go_idle(dev);
        return -1;
    }

    for (int i = TPM_HEADER_SIZE; i < expected; i++) buf[i] = rsp[i];
    if (has_idle) crb_go_idle(dev);
    return expected;
}

/* ======================================================================
 *  CRB initialization
 * ====================================================================== */

int tpm_crb_init(tpm_device_t *dev)
{
    uint32_t pa_low, pa_high, rsp_pa_low, rsp_pa_high;
    uint64_t cmd_pa, rsp_pa;
    uint32_t cmd_size, rsp_size;
    int      rc;
    int      is_acpi_start = (dev->crb_sm == ACPI_TPM2_START_METHOD);

    if (is_acpi_start) {
        /* Method 2: no head registers, no cmdReady/goIdle.
         * Locality is handled by ACPI/firmware. */
        dev->request_locality    = crb_nop_req_locality;
        dev->relinquish_locality = crb_nop_rel_locality;
        dev->ready               = NULL;
        plogk("tpm_crb: ACPI Start method, skipping head regs.\n");
    } else {
        /* Methods 7, 8: standard CRB with head registers.
         * Request locality to wake up the device before reading tail regs. */
        rc = crb_request_locality(dev, 0);
        if (rc < 0) return -1;

        rc = crb_cmd_ready(dev);
        if (rc < 0) {
            crb_relinquish_locality(dev, 0);
            return -1;
        }

        dev->request_locality    = crb_request_locality;
        dev->relinquish_locality = crb_relinquish_locality;
        dev->ready               = crb_go_idle;
    }

    /* Read command/response buffer addresses from tail registers */
    pa_low   = crb_read32(dev, CRB_CTRL_CMD_PA_L);
    pa_high  = crb_read32(dev, CRB_CTRL_CMD_PA_H);
    cmd_pa   = ((uint64_t)pa_high << 32) | pa_low;
    cmd_size = crb_read32(dev, CRB_CTRL_CMD_SIZE);

    rsp_pa_low  = crb_read32(dev, CRB_CTRL_RSP_PA_L);
    rsp_pa_high = crb_read32(dev, CRB_CTRL_RSP_PA_H);
    rsp_pa      = ((uint64_t)rsp_pa_high << 32) | rsp_pa_low;
    rsp_size    = crb_read32(dev, CRB_CTRL_RSP_SIZE);

    plogk("tpm_crb: CMD buf: 0x%llx (size %u), RSP buf: 0x%llx (size %u)\n", cmd_pa, cmd_size, rsp_pa, rsp_size);

    if (cmd_size == 0 || cmd_pa == 0) {
        if (!is_acpi_start) {
            crb_go_idle(dev);
            crb_relinquish_locality(dev, 0);
        }
        return -1;
    }

    dev->crb_cmd_buf  = phys_to_virt(cmd_pa);
    dev->crb_cmd_size = cmd_size;

    if (rsp_pa != cmd_pa && rsp_pa != 0)
        dev->crb_rsp_buf = phys_to_virt(rsp_pa);
    else
        dev->crb_rsp_buf = dev->crb_cmd_buf;

    dev->status = crb_status;
    dev->send   = crb_send;
    dev->recv   = crb_recv;
    dev->cancel = crb_cancel;

    if (!is_acpi_start) {
        crb_go_idle(dev);
        crb_relinquish_locality(dev, 0);
    }
    return 0;
}
