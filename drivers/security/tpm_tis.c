/*
 *
 *      tpm_tis.c
 *      TPM TIS (FIFO) interface implementation
 *
 *      2026/7/23 By MicroFish
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/tpm.h>
#include <kernel/printk.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>

/* ======================================================================
 *  TIS MMIO register access helpers
 *  Uses direct volatile access (matching project MMIO patterns)
 * ====================================================================== */

static inline void *tis_reg_addr(tpm_device_t *dev, uint32_t offset)
{
    return (void *)((uintptr_t)dev->mmio_base + offset);
}

static inline uint8_t tis_read8(tpm_device_t *dev, uint32_t offset)
{
    return *(volatile uint8_t *)tis_reg_addr(dev, offset);
}

static inline void tis_write8(tpm_device_t *dev, uint32_t offset, uint8_t value)
{
    *(volatile uint8_t *)tis_reg_addr(dev, offset) = value;
}

static inline uint32_t tis_read32(tpm_device_t *dev, uint32_t offset)
{
    return *(volatile uint32_t *)tis_reg_addr(dev, offset);
}

static inline void tis_write32(tpm_device_t *dev, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)tis_reg_addr(dev, offset) = value;
}

/* ======================================================================
 *  TIS status and helper functions
 * ====================================================================== */

static uint8_t tpm_tis_status(tpm_device_t *dev)
{
    uint8_t sts = tis_read8(dev, TIS_REG_STS(dev->locality));

    /* Bits 0,1,5 must be zero on valid read; non-zero usually
     * means locality was never properly acquired. */
    if (sts & TPM_STS_READ_ZERO) return 0;
    return sts;
}

static int tpm_tis_ready(tpm_device_t *dev)
{
    tis_write8(dev, TIS_REG_STS(dev->locality), TPM_STS_COMMAND_READY);
    return 0;
}

struct tis_stat_ctx {
        tpm_device_t *dev;
        uint8_t       mask;
};

static int check_status(void *ctx)
{
    struct tis_stat_ctx *c = (struct tis_stat_ctx *)ctx;
    return ((c->dev->status(c->dev) & c->mask) == c->mask) ? 1 : 0;
}

static int wait_for_stat(tpm_device_t *dev, uint8_t mask, uint32_t timeout_ms)
{
    struct tis_stat_ctx ctx;
    ctx.dev  = dev;
    ctx.mask = mask;
    if (check_status(&ctx)) return 0;
    return tpm_poll_timeout(check_status, &ctx, timeout_ms) ? 0 : -1;
}

/* ======================================================================
 *  TIS locality management
 * ====================================================================== */

static int check_locality(tpm_device_t *dev, int l)
{
    uint8_t access = tis_read8(dev, TIS_REG_ACCESS(l));
    if ((access & (TPM_ACCESS_ACTIVE_LOCALITY | TPM_ACCESS_VALID)) == (TPM_ACCESS_ACTIVE_LOCALITY | TPM_ACCESS_VALID)) {
        dev->locality = l;
        return 1;
    }
    return 0;
}

static int check_locality_active(tpm_device_t *dev, int l)
{
    uint8_t access = tis_read8(dev, TIS_REG_ACCESS(l));
    return (access & TPM_ACCESS_ACTIVE_LOCALITY) ? 1 : 0;
}

static int tis_request_locality(tpm_device_t *dev, int l)
{
    uint32_t timeout_ms = dev->timeout_a;
    uint64_t deadline   = nano_time() + (uint64_t)timeout_ms * 1000000ULL;

    if (check_locality(dev, l)) return l;

    /* If locality is active but not yet valid, wait for VALID */
    if (check_locality_active(dev, l)) {
        while (nano_time() < deadline) {
            if (check_locality(dev, l)) return l;
            tpm_udelay(200);
        }
        return -1;
    }

    /* Check for pending request from another locality; release if needed */
    uint8_t access = tis_read8(dev, TIS_REG_ACCESS(l));
    if (access & TPM_ACCESS_REQUEST_PENDING) {
        if (dev->locality >= 0) {
            tis_write8(dev, TIS_REG_ACCESS(dev->locality), TPM_ACCESS_ACTIVE_LOCALITY);
            dev->locality = -1;
        }
        tpm_udelay(2000);
    }

    /* Request the locality */
    tis_write8(dev, TIS_REG_ACCESS(l), TPM_ACCESS_REQUEST_USE);

    deadline = nano_time() + (uint64_t)timeout_ms * 1000000ULL;
    while (nano_time() < deadline) {
        if (check_locality(dev, l)) return l;
        tpm_udelay(200);
    }
    return -1;
}

static void tis_relinquish_locality(tpm_device_t *dev, int l)
{
    (void)l;
    if (dev->locality < 0) return;
    tis_write8(dev, TIS_REG_ACCESS(dev->locality), TPM_ACCESS_ACTIVE_LOCALITY);
    dev->locality = -1;
}

static void tis_cancel(tpm_device_t *dev)
{
    if (dev->locality >= 0) tpm_tis_ready(dev);
}

/* ======================================================================
 *  TIS Burst Count
 * ====================================================================== */

static int get_burstcount(tpm_device_t *dev)
{
    uint32_t timeout_ms = (dev->flags & TPM_FLAG_TPM2) ? dev->timeout_a : dev->timeout_d;
    uint64_t deadline   = nano_time() + (uint64_t)timeout_ms * 1000000ULL;
    uint32_t value;

    for (;;) {
        value        = tis_read32(dev, TIS_REG_STS(dev->locality));
        int burstcnt = (value >> 8) & 0xFFFF;
        if (burstcnt) return burstcnt;
        if (nano_time() >= deadline) return -1;
        tpm_udelay(100);
    }
}

/* ======================================================================
 *  TIS FIFO send
 * ====================================================================== */

static int tis_send(tpm_device_t *dev, uint8_t *buf, size_t len)
{
    uint32_t fifo_offset = TIS_REG_DATA_FIFO(dev->locality);
    size_t   count       = 0;
    int      burstcnt;
    int      rc;
    int      itpm = 0;

    /* Detect iTPM (vendor 0x8086) which has DATA_EXPECT quirks */
    if ((dev->did_vid & 0xFFFF) == TPM_VID_INTEL) itpm = 1;

    uint8_t sts = tpm_tis_status(dev);
    if (!(sts & TPM_STS_COMMAND_READY)) {
        tpm_tis_ready(dev);
        rc = wait_for_stat(dev, TPM_STS_COMMAND_READY, dev->timeout_b);
        if (rc < 0) return -1;
    }

    while (count < len - 1) {
        burstcnt = get_burstcount(dev);
        if (burstcnt < 0) tpm_tis_ready(dev);
        return -1;

        int chunk = burstcnt;
        if (chunk > (int)(len - count - 1)) chunk = (int)(len - count - 1);

        for (int i = 0; i < chunk; i++) { tis_write8(dev, fifo_offset, buf[count + i]); }
        count += chunk;

        rc = wait_for_stat(dev, TPM_STS_VALID, dev->timeout_c);
        if (rc < 0) tpm_tis_ready(dev);
        return -1;

        sts = tpm_tis_status(dev);
        if (!(sts & TPM_STS_DATA_EXPECT)) {
            if (!itpm) {
                plogk("tpm_tis: DATA_EXPECT missing (sts=0x%02x), retrying.\n", sts);
                tpm_tis_ready(dev);
                return -1; /* Non-iTPM: treat as hard error, upper layer retries */
            }
            /* iTPM: tolerate missing DATA_EXPECT */
        }
    }

    /* Write last byte */
    tis_write8(dev, fifo_offset, buf[count]);

    rc = wait_for_stat(dev, TPM_STS_VALID, dev->timeout_c);
    if (rc < 0) tpm_tis_ready(dev);
    return -1;

    sts = tpm_tis_status(dev);
    if (!itpm && (sts & TPM_STS_DATA_EXPECT)) {
        plogk("tpm_tis: DATA_EXPECT stuck after last byte (sts=0x%02x)\n", sts);
        tpm_tis_ready(dev);
        return -1;
    }

    /* Issue GO command */
    tis_write8(dev, TIS_REG_STS(dev->locality), TPM_STS_GO);
    return 0;
}

/* ======================================================================
 *  TIS FIFO recv
 * ====================================================================== */

static int tis_recv_data(tpm_device_t *dev, uint8_t *buf, size_t count)
{
    uint32_t fifo_offset = TIS_REG_DATA_FIFO(dev->locality);
    size_t   size        = 0;
    int      burstcnt;

    while (size < count) {
        int rc = wait_for_stat(dev, TPM_STS_DATA_AVAIL | TPM_STS_VALID, dev->timeout_c);
        if (rc < 0) return rc;

        burstcnt = get_burstcount(dev);
        if (burstcnt < 0) return burstcnt;

        int chunk = burstcnt;
        if (chunk > (int)(count - size)) chunk = (int)(count - size);

        for (int i = 0; i < chunk; i++) { buf[size + i] = tis_read8(dev, fifo_offset); }
        size += chunk;
    }
    return (int)size;
}

static int tis_recv(tpm_device_t *dev, uint8_t *buf, size_t maxlen)
{
    if (maxlen < TPM_HEADER_SIZE) return -1;

    int size = tis_recv_data(dev, buf, TPM_HEADER_SIZE);
    if (size < TPM_HEADER_SIZE) {
        tpm_tis_ready(dev);
        return -1;
    }

    int expected = (buf[2] << 24) | (buf[3] << 16) | (buf[4] << 8) | buf[5];
    if (expected > (int)maxlen || expected < TPM_HEADER_SIZE) {
        tpm_tis_ready(dev);
        return -1;
    }

    int rc = tis_recv_data(dev, &buf[TPM_HEADER_SIZE], expected - TPM_HEADER_SIZE);
    if (rc < 0) {
        tpm_tis_ready(dev);
        return rc;
    }
    size += rc;

    if (size < expected) tpm_tis_ready(dev);
    return -1;

    rc = wait_for_stat(dev, TPM_STS_VALID, dev->timeout_c);
    if (rc < 0) tpm_tis_ready(dev);
    return -1;

    uint8_t sts = tpm_tis_status(dev);
    if (sts & TPM_STS_DATA_AVAIL) tpm_tis_ready(dev);
    return -1;

    tpm_tis_ready(dev);
    return size;
}

/* ======================================================================
 *  TIS initialization
 * ====================================================================== */

static int tis_wait_startup(tpm_device_t *dev)
{
    uint64_t deadline = nano_time() + (uint64_t)dev->timeout_a * 1000000ULL;
    while (nano_time() < deadline) {
        uint8_t access = tis_read8(dev, TIS_REG_ACCESS(0));
        if (access & TPM_ACCESS_VALID) return 0;
        tpm_udelay(200);
    }
    return -1;
}

int tpm_tis_init(tpm_device_t *dev)
{
    uint32_t did_vid;
    uint8_t  rid;

    did_vid = tis_read32(dev, TIS_REG_DID_VID(0));
    if (did_vid == 0 || did_vid == 0xFFFFFFFF) return -1;
    dev->did_vid = did_vid;

    rid      = tis_read8(dev, TIS_REG_RID(0));
    dev->rid = rid;

    dev->status              = tpm_tis_status;
    dev->send                = tis_send;
    dev->recv                = tis_recv;
    dev->request_locality    = tis_request_locality;
    dev->relinquish_locality = tis_relinquish_locality;
    dev->cancel              = tis_cancel;
    dev->ready               = tpm_tis_ready;

    if (tis_wait_startup(dev) < 0) return -1;
    if (tis_request_locality(dev, 0) < 0) return -1;

    uint32_t intfcaps = tis_read32(dev, TIS_REG_INTF_CAPS(0));
    plogk("tpm_tis: Interface capabilities: 0x%08x\n", intfcaps);

    uint32_t intmask = tis_read32(dev, TIS_REG_INT_ENABLE(0));
    intmask &= ~TPM_GLOBAL_INT_ENABLE;
    tis_write32(dev, TIS_REG_INT_ENABLE(0), intmask);
    tis_relinquish_locality(dev, 0);

    return 0;
}
