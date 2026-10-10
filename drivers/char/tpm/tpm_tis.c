/*
 *
 *      tpm_tis.c
 *      TPM TIS (FIFO) interface implementation
 *
 *      2026/7/23 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/misc/common.h>
#include <drivers/char/tpm/tpm.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/util/byteorder.h>

#if CONFIG_TPM

typedef struct tis_stat_ctx {
        tpm_device_t *dev;
        uint8_t       mask;
} tis_stat_ctx_t;

/* Return the MMIO address of a TIS register. */
static void *tis_reg_addr(tpm_device_t *dev, uint32_t offset)
{
    return (void *)((uintptr_t)dev->mmio_base + offset);
}

/* Read a TIS byte register. */
static uint8_t tis_read8(tpm_device_t *dev, uint32_t offset)
{
    return mmio_read8(tis_reg_addr(dev, offset));
}

/* Write a TIS byte register. */
static void tis_write8(tpm_device_t *dev, uint32_t offset, uint8_t value)
{
    mmio_write8(tis_reg_addr(dev, offset), value);
}

/* Read a TIS 32-bit register. */
static uint32_t tis_read32(tpm_device_t *dev, uint32_t offset)
{
    return mmio_read32(tis_reg_addr(dev, offset));
}

/* Write a TIS 32-bit register. */
static void tis_write32(tpm_device_t *dev, uint32_t offset, uint32_t value)
{
    mmio_write32(tis_reg_addr(dev, offset), value);
}

/* Return the TIS status register, 0 if the locality is invalid. */
static uint8_t tpm_tis_status(tpm_device_t *dev)
{
    uint8_t sts = tis_read8(dev, TIS_REG_STS(dev->locality));

    /* Bits 0,1,5 must be zero on valid read; non-zero usually means locality was never properly acquired. */
    if (sts & TPM_STS_READ_ZERO) return 0;
    return sts;
}

/* Put the TIS interface into the command-ready state. */
static int tpm_tis_ready(tpm_device_t *dev)
{
    tis_write8(dev, TIS_REG_STS(dev->locality), TPM_STS_COMMAND_READY);
    return 0;
}

/* Poll context: returns 1 when the status mask matches. */
static int check_status(void *ctx)
{
    tis_stat_ctx_t *c = (tis_stat_ctx_t *)ctx;
    return ((c->dev->status(c->dev) & c->mask) == c->mask) ? 1 : 0;
}

/* Wait until the status register matches the given mask. */
static int wait_for_stat(tpm_device_t *dev, uint8_t mask, uint32_t timeout_ms)
{
    tis_stat_ctx_t ctx;
    ctx.dev  = dev;
    ctx.mask = mask;
    if (check_status(&ctx)) return 0;
    return tpm_poll_timeout(check_status, &ctx, timeout_ms) ? 0 : -ETIMEDOUT;
}

/* Return 1 if locality l is active and valid. */
static int check_locality(tpm_device_t *dev, int l)
{
    uint8_t access = tis_read8(dev, TIS_REG_ACCESS(l));
    if ((access & (TPM_ACCESS_ACTIVE_LOCALITY | TPM_ACCESS_VALID)) == (TPM_ACCESS_ACTIVE_LOCALITY | TPM_ACCESS_VALID)) {
        dev->locality = l;
        return 1;
    }
    return 0;
}

/* Return 1 if locality l is active. */
static int check_locality_active(tpm_device_t *dev, int l)
{
    uint8_t access = tis_read8(dev, TIS_REG_ACCESS(l));
    return (access & TPM_ACCESS_ACTIVE_LOCALITY) ? 1 : 0;
}

/* Request TIS locality l and wait until it is active. */
static int tis_request_locality(tpm_device_t *dev, int l)
{
    uint32_t timeout_ms = dev->timeout_a;
    uint64_t deadline   = nano_time() + ((uint64_t)timeout_ms * 1000000ULL);

    if (check_locality(dev, l)) return l;

    /* If locality is active but not yet valid, wait for VALID */
    if (check_locality_active(dev, l)) {
        while (nano_time() < deadline) {
            if (check_locality(dev, l)) return l;
            tpm_udelay(200);
        }
        return -ETIMEDOUT;
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

    deadline = nano_time() + ((uint64_t)timeout_ms * 1000000ULL);
    while (nano_time() < deadline) {
        if (check_locality(dev, l)) return l;
        tpm_udelay(200);
    }
    return -ETIMEDOUT;
}

/* Release the currently held TIS locality. */
static void tis_relinquish_locality(tpm_device_t *dev, int l)
{
    (void)l;
    if (dev->locality < 0) return;
    tis_write8(dev, TIS_REG_ACCESS(dev->locality), TPM_ACCESS_ACTIVE_LOCALITY);
    dev->locality = -1;
}

/* Cancel a pending TIS command. */
static void tis_cancel(tpm_device_t *dev)
{
    if (dev->locality >= 0) tpm_tis_ready(dev);
}

/* Return the TIS FIFO burst count, or -ETIMEDOUT. */
static int get_burstcount(tpm_device_t *dev)
{
    uint32_t timeout_ms = (dev->flags & TPM_FLAG_TPM2) ? dev->timeout_a : dev->timeout_d;
    uint64_t deadline   = nano_time() + ((uint64_t)timeout_ms * 1000000ULL);
    uint32_t value;

    for (;;) {
        value        = tis_read32(dev, TIS_REG_STS(dev->locality));
        int burstcnt = (value >> 8) & 0xFFFF;
        if (burstcnt) return burstcnt;
        if (nano_time() >= deadline) return -ETIMEDOUT;
        tpm_udelay(100);
    }
}

/* Send a command through the TIS FIFO. */
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
        if (rc < 0) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("tpm_tis: COMMAND_READY wait failed.\n");
            return -ETIMEDOUT;
        }
    }

    while (count < len - 1) {
        burstcnt = get_burstcount(dev);
        if (burstcnt < 0) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("tpm_tis: Burst count timeout.\n");
            tpm_tis_ready(dev);
            return -ETIMEDOUT;
        }

        int chunk = burstcnt;
        if (chunk > (int)(len - count - 1)) chunk = (int)(len - count - 1);

        for (int i = 0; i < chunk; i++) tis_write8(dev, fifo_offset, buf[count + i]);
        count += chunk;

        rc = wait_for_stat(dev, TPM_STS_VALID, dev->timeout_c);
        if (rc < 0) {
            static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
            if (ratelimit_allow(&ratelimit)) plogk("tpm_tis: VALID wait failed.\n");
            tpm_tis_ready(dev);
            return -ETIMEDOUT;
        }

        sts = tpm_tis_status(dev);
        if (!(sts & TPM_STS_DATA_EXPECT)) {
            if (!itpm) {
                static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
                if (ratelimit_allow(&ratelimit)) plogk("tpm_tis: DATA_EXPECT missing (sts=0x%02x), retrying.\n", sts);
                tpm_tis_ready(dev);
                return -EIO; // Non-iTPM: treat as hard error, upper layer retries
            }
            /* iTPM: tolerate missing DATA_EXPECT */
        }
    }

    /* Write last byte */
    tis_write8(dev, fifo_offset, buf[count]);

    rc = wait_for_stat(dev, TPM_STS_VALID, dev->timeout_c);
    if (rc < 0) {
        tpm_tis_ready(dev);
        return -ETIMEDOUT;
    }

    sts = tpm_tis_status(dev);
    if (!itpm && (sts & TPM_STS_DATA_EXPECT)) {
        static DEFINE_RATELIMIT_STATE(ratelimit, PRINTK_RATELIMIT_TICKS, PRINTK_RATELIMIT_BURST);
        if (ratelimit_allow(&ratelimit)) plogk("tpm_tis: DATA_EXPECT stuck after last byte (sts=0x%02x)\n", sts);
        tpm_tis_ready(dev);
        return -EIO;
    }

    /* Issue GO command */
    tis_write8(dev, TIS_REG_STS(dev->locality), TPM_STS_GO);
    return 0;
}

/* Read count bytes from the TIS FIFO. */
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

        for (int i = 0; i < chunk; i++) buf[size + i] = tis_read8(dev, fifo_offset);
        size += chunk;
    }
    return (int)size;
}

/* Receive a TIS response. */
static int tis_recv(tpm_device_t *dev, uint8_t *buf, size_t maxlen)
{
    if (maxlen < TPM_HEADER_SIZE) return -EINVAL;

    int size = tis_recv_data(dev, buf, TPM_HEADER_SIZE);
    if (size < TPM_HEADER_SIZE) {
        tpm_tis_ready(dev);
        return -EIO;
    }

    int expected = load_be32(&buf[2]);
    if (expected > (int)maxlen || expected < TPM_HEADER_SIZE) {
        tpm_tis_ready(dev);
        return -EIO;
    }

    int rc = tis_recv_data(dev, &buf[TPM_HEADER_SIZE], expected - TPM_HEADER_SIZE);
    if (rc < 0) {
        tpm_tis_ready(dev);
        return rc;
    }
    size += rc;

    if (size < expected) {
        tpm_tis_ready(dev);
        return -EIO;
    }

    rc = wait_for_stat(dev, TPM_STS_VALID, dev->timeout_c);
    if (rc < 0) {
        tpm_tis_ready(dev);
        return -ETIMEDOUT;
    }

    uint8_t sts = tpm_tis_status(dev);
    if (sts & TPM_STS_DATA_AVAIL) {
        tpm_tis_ready(dev);
        return -EIO;
    }

    tpm_tis_ready(dev);
    return size;
}

/* Wait for the TIS access register to become valid. */
static int tis_wait_startup(tpm_device_t *dev)
{
    uint64_t deadline = nano_time() + ((uint64_t)dev->timeout_a * 1000000ULL);
    while (nano_time() < deadline) {
        uint8_t access = tis_read8(dev, TIS_REG_ACCESS(0));
        if (access & TPM_ACCESS_VALID) return 0;
        tpm_udelay(200);
    }
    return -ETIMEDOUT;
}

/* Initialize the TIS interface. */
int tpm_tis_init(tpm_device_t *dev)
{
    uint32_t did_vid;
    uint8_t  rid;

    did_vid = tis_read32(dev, TIS_REG_DID_VID(0));
    if (did_vid == 0 || did_vid == 0xFFFFFFFF) {
        plogk("tpm_tis: No TPM at MMIO base (DID/VID 0x%08x)\n", did_vid);
        return -ENODEV;
    }
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

    if (tis_wait_startup(dev) < 0) {
        plogk("tpm_tis: Timed out waiting for TPM startup.\n");
        return -ETIMEDOUT;
    }
    if (tis_request_locality(dev, 0) < 0) {
        plogk("tpm_tis: Failed to request locality 0\n");
        return -EIO;
    }

    uint32_t intfcaps = tis_read32(dev, TIS_REG_INTF_CAPS(0));
    plogk("tpm_tis: Interface capabilities: 0x%08x\n", intfcaps);

    uint32_t intmask = tis_read32(dev, TIS_REG_INT_ENABLE(0));
    intmask &= ~TPM_GLOBAL_INT_ENABLE;
    tis_write32(dev, TIS_REG_INT_ENABLE(0), intmask);
    tis_relinquish_locality(dev, 0);

    return 0;
}

#endif
